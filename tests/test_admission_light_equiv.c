/* T1 predicate reuse: compare every seed and resident light byte, not hashes.
 * Beta reference: 740c583 Chunk.generateSkylightMap + occlusion propagation.
 * Uses the split-residency fixture and fixed admission clock for 600 updates. */
#include "support/split_residency_fixture.h"
#include "lighting.h"
#include "worldgen/worldgen.h"
#include "platform/audio.h"
#include "tps_observer.h"
#include <inttypes.h>
#include <zlib.h>

static gzFile trace;
static bool capture;
static uint64_t compared, relights, snapshots, far_snapshots;
static uint64_t reference_cells[2], reference_sources[2], emitted_sources[2];
static struct tps_census totals;
static unsigned summary_records, seals;

static void bytes(const void* data, size_t length) {
	const uint8_t* p = data;
	uint8_t expected[1024];
	while(length) {
		unsigned n = length < sizeof(expected) ? length : sizeof(expected);
		if(capture) assert(gzwrite(trace, p, n) == (int)n);
		else {
			assert(gzread(trace, expected, n) == (int)n);
			assert(!memcmp(p, expected, n));
		}
		compared += n;
		p += n;
		length -= n;
	}
}
static void lights(struct server_world* w) {
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, w->chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it)) {
		const struct dict_server_chunks_pair_s* pair = dict_server_chunks_ref(it);
		bytes(&pair->key, sizeof(pair->key));
		bytes(pair->value.lighting_sky, 16384);
		bytes(pair->value.lighting_torch, 16384);
		bytes(pair->value.heightmap, 256);
	}
}
static void relight(struct server_world* w,
	const struct world_modification_entry* seeds, size_t count, unsigned kind) {
	bytes(&kind, sizeof(kind));
	bytes(&count, sizeof(count));
	for(size_t i = 0; i < count; i++) {
		bytes(&seeds[i].x, sizeof(seeds[i].x));
		bytes(&seeds[i].y, sizeof(seeds[i].y));
		bytes(&seeds[i].z, sizeof(seeds[i].z));
		if(!kind) bytes(&seeds[i].blk.type, sizeof(seeds[i].blk.type));
	}
	lights(w);
	relights++;
	emitted_sources[kind] += count;
}
static void rpc(const struct client_rpc* r, void* unused) {
	(void)unused;
	clin_drain_discard();
	bytes(&r->type, sizeof(r->type));
	if(r->type == CRPC_FAR_CHUNK) {
		const struct far_rpc_slot* f = r->payload.far_chunk.slot;
		bytes(&r->payload.far_chunk.x, sizeof(r->payload.far_chunk.x));
		bytes(&r->payload.far_chunk.z, sizeof(r->payload.far_chunk.z));
		bytes(f->skin_y, sizeof(f->skin_y));
		bytes(f->top_id, sizeof(f->top_id));
		bytes(f->grass_i, sizeof(f->grass_i));
		bytes(f->grass_j, sizeof(f->grass_j));
		far_snapshots++;
		return;
	}
	if(r->type != CRPC_CHUNK) return;
	const struct chunk_rpc_slot* c = r->payload.chunk.slot;
	bytes(&r->payload.chunk.x, sizeof(r->payload.chunk.x));
	bytes(&r->payload.chunk.z, sizeof(r->payload.chunk.z));
	bytes(c->ids, 32768);
	bytes(c->metadata, 16384);
	bytes(c->lighting_sky, 16384);
	bytes(c->lighting_torch, 16384);
	bytes(c->heightmap, 256);
	snapshots++;
}
/* Loading deliberately suspends automatic far sends. Schedule the faithful
 * production far admission/extract/save/send path explicitly for each anchor;
 * a fixed cadence avoids FAR_GEN_BUDGET_MS changing replay work across runs. */
static void far_step(struct server_local* s, unsigned tick) {
	unsigned anchor = (tick / 100) % 2;
	w_coord_t x = WCOORD_CHUNK_OFFSET((int)s->player.x)
		+ anchor * (s->anchor_sep / CHUNK_SIZE) + MAX_VIEW_DISTANCE + 1;
	w_coord_t z = -1;
	struct far_rpc_slot* slot = clin_far_slot_try_acquire();
	assert(slot);
	struct server_chunk* chunk = NULL;
	bool ready = false;
	server_tps_boundary(s, true);
	for(unsigned attempt = 0; attempt < 1000 && !ready; attempt++) {
		server_world_begin_update(&s->world);
		ready = server_world_ensure_populated_far(&s->world, s, x, z, &chunk);
	}
	assert(ready && chunk);
	assert(server_local_test_finish_far_column(s, x, z, chunk, slot));
	assert(clin_try_rpc_send(&(struct client_rpc) {
		.type = CRPC_FAR_CHUNK,
		.payload.far_chunk = {.x = x * CHUNK_SIZE, .z = z * CHUNK_SIZE, .slot = slot}
	}));
	server_world_mark_far_sent(&s->world, x, z);
	clin_drain_discard();
	server_tps_boundary(s, false);
}
static bool census(const char* line) {
	if(strstr(line, "\"event\":\"tps_end\"")) {
		const char* records = strstr(line, "\"records\":");
		assert(records && strstr(line, "\"stream_valid\":true"));
		assert(strtoul(records + strlen("\"records\":"), NULL, 10) == summary_records);
		seals++;
		return true;
	}
	const char* count = strstr(line, "\"count\":");
	assert(count && strstr(line, "\"census_version\":3"));
	summary_records += (unsigned)strtoul(count + strlen("\"count\":"), NULL, 10);
	const char* p = strstr(line, "\"census\":[");
	if(!p) return true;
	p += strlen("\"census\":[");
	for(unsigned i = 0; i < TPS_C_COUNT; i++) {
		char* end;
		totals.value[i] += strtoull(p, &end, 10);
		assert(end != p);
		p = end + 1;
	}
	return true;
}

static void run(const char* path, unsigned mode, unsigned ticks) {
	capture = mode == 1;
	trace = gzopen(path, capture ? "wb1" : "rb");
	assert(trace);
	compared = relights = snapshots = far_snapshots = 0;
	memset(emitted_sources, 0, sizeof(emitted_sources));
	server_world_test_light_memo_mode(mode);
	struct server_local* s = split_fixture();
	s->rand_src = (struct random_gen) {.seed = 12345};
	*server_local_sound_rng() = (struct random_gen) {.seed = 6789};
	*audio_cosmetic_rng() = (struct random_gen) {.seed = 9876};
	server_world_discard(&s->world);
	string_t world_path;
	string_init_set_str(world_path, split_path);
	assert(server_world_create_with_populate_scratch(&s->world, world_path,
		WORLD_DIM_OVERWORLD, 42, false, s->world_populate_scratch));
	/* F4-RNG: population/drop callbacks share this world's runtime stream.
	 * Pin it after the replacement World is constructed, in every arm. */
	jrand_set_seed(&s->worlds[0].rand, 4242);
	string_clear(world_path);
	s->world.local = s;
	s->worlds[0].session = s; /* A1a (D1): owning session for services/callbacks */
	assert(server_local_set_anchor_sep(s, 256));
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	char prefix[256];
	snprintf(prefix, sizeof(prefix), "%s/census_", split_path);
	assert(tps_observer_start(prefix));
	tps_observer_output(census);
	memset(&totals, 0, sizeof(totals));
	summary_records = seals = 0;
	server_world_test_light_memo_output = relight;
	clin_test_set_rpc_observer(rpc, NULL);
	for(unsigned tick = 0; tick < ticks; tick++) {
		/* Outward and return legs exercise overlap, eviction and disk reload.
		 * Keep player loading: deterministic admission without time-seeded mobs. */
		s->player.x = 0.5 + (tick < 300 ? tick : 600 - tick) * 0.25;
		s->player.z = -0.5;
		tps_observer_frame(true);
		server_local_test_update_once(s);
		clin_drain_discard();
		if(tick % 100 == 0) far_step(s, tick);
		/* Drain without sealing: the integrated observer has one terminal
		 * measurement window for the entire replay. */
		tps_observer_frame(true);
		lights(&s->world);
		if(tick % 100 == 0) {
			printf("mode=%u tick=%u relights=%" PRIu64 "\n", mode, tick, relights);
			fflush(stdout);
		}
		bytes(&s->rand_src.seed, sizeof(s->rand_src.seed));
		bytes(&s->world.current_tick, sizeof(s->world.current_tick));
		bytes(&s->rand_src.has_next_gaussian, sizeof(s->rand_src.has_next_gaussian));
		bytes(&s->rand_src.next_gaussian, sizeof(s->rand_src.next_gaussian));
		bytes(&s->world.hell_populate_rng.seed, sizeof(s->world.hell_populate_rng.seed));
		bytes(&s->world.scheduled_tick_count, sizeof(s->world.scheduled_tick_count));
		for(size_t i = 0; i < s->world.scheduled_tick_count; i++) {
			const struct scheduled_tick* q = &s->world.scheduled_ticks[
				(s->world.scheduled_tick_head + i) % s->world.scheduled_tick_capacity];
			bytes(&q->x, sizeof(q->x));
			bytes(&q->y, sizeof(q->y));
			bytes(&q->z, sizeof(q->z));
			bytes(&q->fire_at_tick, sizeof(q->fire_at_tick));
			bytes(&q->block_type, sizeof(q->block_type));
			bytes(&q->queue_flags, sizeof(q->queue_flags));
		}
	}
	tps_observer_frame(false);
	server_world_test_light_memo_output = NULL;
	clin_test_set_rpc_observer(NULL, NULL);
	assert(tps_observer_stop());
	assert(seals == 1 && summary_records == ticks + (ticks + 99) / 100);
	/* Original-JAR full-domain history, ChunkProviderServer:157-160 and
	 * MinecraftServer:346-348: no whole-quadrant emitter census. Capacity
	 * helpers are covered separately; actual admission/tick/RPC bytes compare. */
	assert(relights == 0 && snapshots);
	if(ticks == 600) assert(far_snapshots == 6);
	if(!capture) assert(gzgetc(trace) == -1 && gzeof(trace));
	assert(gzclose(trace) == Z_OK);
	printf("mode=%u ticks=%u bytes=%" PRIu64 " relights=%" PRIu64
		" snapshots=%" PRIu64 " far=%" PRIu64 "\n",
		mode, ticks, compared, relights, snapshots, far_snapshots);
	for(unsigned kind = 0; kind < 2; kind++) {
		unsigned b = kind ? TPS_C_SKY_COUNT_NS : TPS_C_BLOCK_COUNT_NS;
		printf("%s count/fill ms/update=%.6f/%.6f cells/update=%.3f/%.3f "
			"pairs=%" PRIu64 " duplicates=%" PRIu64 " fallback=%" PRIu64 "\n",
			kind ? "sky" : "block", totals.value[b] / 1e6 / ticks,
			totals.value[b + 1] / 1e6 / ticks,
			(double)totals.value[b + 4] / ticks,
			(double)totals.value[b + 5] / ticks, totals.value[b + 2],
			totals.value[b + 3], totals.value[TPS_C_BLOCK_MEMO_FALLBACKS + kind]);
		unsigned consumed = TPS_C_BLOCK_CONSUMED_CELLS + 2 * kind;
		if(capture) {
			reference_cells[kind] = totals.value[b + 5];
			reference_sources[kind] = totals.value[consumed + 1];
		}
		assert(totals.value[consumed] == reference_cells[kind]);
		assert(totals.value[consumed + 1] == reference_sources[kind]);
		assert(reference_sources[kind] == 0);
		assert(totals.value[consumed + 1] == emitted_sources[kind]);
		printf("consumed cells/sources=%" PRIu64 "/%" PRIu64 "\n",
			totals.value[consumed], totals.value[consumed + 1]);
		assert(totals.value[b + 2] == 0);
		assert(totals.value[b + 3] == (mode ? totals.value[b + 2] : 0));
		if(!mode) assert(totals.value[b + 5] == 0);
		if(mode >= 2) assert(totals.value[TPS_C_BLOCK_MEMO_FALLBACKS + kind]
			>= totals.value[b + 2]);
	}
	fflush(stdout);
	split_destroy(s);
}
static void capacity_boundaries(void) {
	const unsigned cells = CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	uint8_t* ids = calloc(5 * cells, 1);
	uint8_t* expected = calloc(5 * cells, 1);
	uint8_t* actual = malloc(5 * cells);
	assert(ids && expected && actual);
	for(unsigned column = 0; column < 5; column++) {
		/* Distinct column patterns cross byte edges; each final bit is set
		 * and its predecessor clear, detecting stride/last-index errors. */
		for(unsigned i = 0; i < cells; i++) {
			unsigned offset = column * cells + i;
			expected[offset] = (i < cells - 2 && i % (9 + column) == 0)
				|| i == cells - 1;
			ids[offset] = expected[offset] ? BLOCK_GLOWSTONE : BLOCK_STONE;
		}
	}
	bool fallback;
	server_world_test_light_memo_mode(1);
	assert(!server_world_test_light_memo_cells(ids, 5, actual, &fallback));
	assert(!fallback && !memcmp(actual, expected, 5 * cells));
	server_world_test_light_memo_mode(0);
	for(unsigned columns = 4; columns <= 5; columns++) {
		bool fallback;
		memset(actual, 0xa5, 5 * cells);
		bool used = server_world_test_light_memo_cells(ids, columns, actual, &fallback);
		assert(used == (columns == 4));
		assert(fallback == (columns == 5));
		assert(!memcmp(actual, expected, columns * cells));
		for(unsigned i = columns * cells; i < 5 * cells; i++) assert(actual[i] == 0xa5);
		for(unsigned column = 0; column < columns; column++)
			assert(actual[(column + 1) * cells - 1] == 1);
	}
	free(actual);
	free(expected);
	free(ids);
	puts("PASS real memo capacity: four full columns, fifth recomputes; last bits/byte edges");
}
int main(void) {
	assert(clin_init());
	blocks_init();
	capacity_boundaries();
	char dir[256], path[300];
	test_fixture_dir("test_admission_light_equiv", dir, sizeof(dir));
#ifdef _WIN32
	assert(_mkdir(dir) == 0);
#else
	assert(mkdir(dir, 0777) == 0);
#endif
	snprintf(path, sizeof(path), "%s/trace.gz", dir);
	run(path, 1, 600);
	run(path, 0, 600);
	/* A short identical prefix exercises allocation fallbacks. */
	run(path, 1, 24);
	run(path, 2, 24);
	run(path, 4, 24);
	server_world_test_light_memo_mode(0);
	test_fixture_dir_cleanup(dir);
	puts("PASS byte-exact ordered seeds, every relight and tick, RPC planes; OOM/cap fallback");
	return 0;
}
