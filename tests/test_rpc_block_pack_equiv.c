/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * test_rpc_block_pack_equiv -- G9 R21 (ASTRA-G9-R21-RING alternative):
 * LOCAL-ONLY ordered packing of consecutive CRPC_SET_BLOCK records in the
 * server pending-output ring is a pure transport change. Proven against
 * the REAL consumer (clin_update_drain -> route -> clin_process_queued ->
 * clin_process -> the client world), not a test pop:
 *
 *   arm U  packing OFF (the accepted 128 ring as before)
 *   arm U2 packing OFF again (determinism control: the fixture must be
 *          bit-reproducible, or the comparison below would prove nothing)
 *   arm B  the R21 SET_BLOCK-only pack (the k_pack200 build)
 *   arm P  packing ON with CRPC_SPAWN_ITEM records too (shipping)
 *
 * Each arm runs in its own process (argv: arm U|P <out>) on the same
 * fixture: a real server_local streaming a disk island to the real client
 * world, then 160 production ticks (server_local_test_update_once: random
 * ticks, fluids, scheduled ticks, tick-start seal) with the light_queue
 * stress shapes injected through the real server_world_set_block publish
 * path -- the 8x8 roof toggled every 4th tick (the h_ring1 B class), a
 * 4x4 torch grid, water sources (fluid cascades), and an intervening
 * CRPC_TIME_SET in the middle of every roof (a pack barrier), and every
 * 40th tick the perf_stress burst: three power-4 blasts on a radius-28 ring
 * (the real-Wii k_pack200 start-up class), whose pass 3 interleaves
 * CRPC_SPAWN_ITEM drops between its CRPC_SET_BLOCKs. After every
 * tick the consumer drains to empty (the same schedule in every arm, so
 * chunk-slot / token feedback is identical by construction).
 *
 * Compared byte-for-byte between arms (parent process):
 *   - the CONSUMER-EFFECT TRACE: every envelope the real consumer
 *     processed, in order (type, world stamp, guest tag; every
 *     CRPC_SET_BLOCK field incl. local_revision / previous content);
 *   - the per-tick client world digest of the edit region after each
 *     drain, and the final client world bytes (id/meta/sky/torch) of the
 *     whole streamed island;
 *   - the server simulation: every chunk plane, the RNGs and the
 *     scheduled-tick queue.
 * Also: arm P must really have packed (packs > 0) and arm U must not; the
 * consumer cost per block update is measured in both arms (host). */

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/split_residency_fixture.h"

#include "entity/entity.h"
#include "game/game_state.h"
#include "item/items.h"
#include "item/recipe.h"
#include "world.h"

#define ISLAND 3 /* chunks -ISLAND..ISLAND on both axes */
#define TICKS 160
#define TAIL_N 7

static FILE* s_out;
/* the blast-burst ticks (consumer-side histogram + ring entries after the
 * tick, before the drain) */
static unsigned s_hist[256];
static bool s_hist_on;
static uint32_t s_blast_ticks, s_blast_entries_max, s_blast_setblocks,
	s_blast_spawns, s_blast_envelopes;
static uint64_t s_trace_n, s_setblock_n, s_trace_hash = 1469598103934665603ull;
static void out_bytes(const void* p, size_t n) {
	const uint8_t* b = p;
	for(size_t i = 0; i < n; i++)
		s_trace_hash = (s_trace_hash ^ b[i]) * 1099511628211ull;
	assert(fwrite(p, 1, n, s_out) == n);
}

/* The real consumer calls this once per processed envelope (packed records
 * included, one call each, after clin_process_queued). */
static void consumed(const struct client_rpc* c, void* unused) {
	(void)unused;
	assert(c->type != CRPC_SET_BLOCK_PACK && "a pack reached a consumer");
	uint32_t rec[16];
	memset(rec, 0, sizeof rec);
	rec[0] = (uint32_t)c->type;
	rec[1] = c->world_index;
	rec[2] = c->world_generation;
	if(c->type == CRPC_SET_BLOCK) {
		const struct crpc_set_block* b = &c->payload.set_block;
		rec[3] = (uint32_t)b->x;
		rec[4] = (uint32_t)b->y;
		rec[5] = (uint32_t)b->z;
		rec[6] = b->block.type | (uint32_t)b->block.metadata << 8
			| (uint32_t)b->block.sky_light << 12
			| (uint32_t)b->block.torch_light << 16;
		rec[7] = b->local_revision.owner;
		rec[8] = b->local_revision.incarnation;
		rec[9] = b->local_revision.content;
		rec[10] = b->local_revision.light;
		rec[11] = b->local_revision.events;
		rec[12] = b->local_previous_content;
		s_setblock_n++;
	}
	out_bytes(rec, sizeof rec);
	s_trace_n++;
	if(s_hist_on && c->type < 256) s_hist[c->type]++;
}

/* The consumer is serialised by this lock: the test thread drains after
 * every tick; the RESCUE thread drains only while the server (this same
 * test thread) is parked in the R5 exhaustion wait -- exactly what the
 * client does on hardware. Without it the single-threaded unpacked arm
 * would hit the 1-s exhaustion deadline (the B class itself). The consumer
 * trace is FIFO whichever thread consumes; every arm runs the same code. */
static struct thread_mutex s_consumer;
static double s_drain_us;
static unsigned s_rescues, s_roof_checks;
static volatile int s_rescue_stop;
/* The exhaustion-count baseline is sampled by the CREATING thread before
 * thread_create, never inside the rescue thread: a rescue thread that is
 * scheduled late (a busy host) would otherwise sample a count that already
 * includes the first exhaustion wait, never see it change, and leave the
 * server parked until the 1-s deadline faults publication (IN-010: every
 * loaded failure was "exhausted 1, faulted 1" at the first blast tick). */
static uint32_t s_rescue_seen0;
static void* rescue_entry(void* unused) {
	(void)unused;
	uint32_t seen = s_rescue_seen0;
	while(!s_rescue_stop) {
		uint32_t ex = 0;
		clin_rpc_pending_stats(NULL, NULL, NULL, &ex);
		if(ex != seen) { /* the server is waiting for ONE token */
			seen = ex;
			thread_mutex_lock(&s_consumer);
			const ptime_t t0 = time_get();
			clin_update_drain();
			s_drain_us += time_diff_s(t0, time_get()) * 1.0e6;
			s_rescues++;
			thread_mutex_unlock(&s_consumer);
		}
		thread_msleep(1);
	}
	return NULL;
}
static void drain_to_empty(void) {
	thread_mutex_lock(&s_consumer);
	const ptime_t t0 = time_get();
	for(;;) {
		clin_update_drain();
		const bool flushed = clin_rpc_flush_pending();
		uint32_t pending = 0;
		clin_rpc_pending_stats(&pending, NULL, NULL, NULL);
		if(flushed && pending == 0) {
			clin_update_drain();
			break;
		}
	}
	s_drain_us += time_diff_s(t0, time_get()) * 1.0e6;
	assert(!clin_world_message_held());
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	thread_mutex_unlock(&s_consumer);
}

static uint64_t client_region_digest(int x0, int x1, int y0, int y1, int z0, int z1) {
	uint64_t h = 1469598103934665603ull;
	for(int x = x0; x < x1; x++)
		for(int z = z0; z < z1; z++)
			for(int y = y0; y < y1; y++) {
				struct block_data b = world_get_block(&gstate.world, x, y, z);
				const uint8_t v[4] = {b.type, b.metadata, b.sky_light, b.torch_light};
				for(int k = 0; k < 4; k++) h = (h ^ v[k]) * 1099511628211ull;
			}
	return h;
}

static const char* arm_name(char mode) {
	return mode == 'P' ? "P" : mode == 'B' ? "B" : "U";
}

/* Order-independent digest of the client entity table (id, type, position,
 * item stack): the consumer installed the same entities in every arm. */
static uint64_t client_entities_digest(unsigned* count) {
	uint64_t sum = 0;
	unsigned n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, gstate.entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		const struct entity* e = dict_entity_cref(it)->value;
		uint64_t h = 1469598103934665603ull;
		uint32_t f[8] = {e->id, (uint32_t)e->type};
		memcpy(&f[2], e->pos, sizeof(float) * 3);
		if(e->type == ENTITY_ITEM) {
			f[5] = e->data.item.item.id;
			f[6] = e->data.item.item.durability;
			f[7] = e->data.item.item.count;
		}
		const uint8_t* b = (const uint8_t*)f;
		for(size_t k = 0; k < sizeof f; k++) h = (h ^ b[k]) * 1099511628211ull;
		sum += h;
		n++;
	}
	*count = n;
	return sum;
}

static int arm(char mode, const char* path) {
	const bool pack = mode != 'U';
	s_out = fopen(path, "wb");
	assert(s_out);
	assert(thread_mutex_init(&s_consumer));
	assert(clin_init() && svin_init());
	blocks_init();
	items_init();
	recipe_init();
	clin_test_set_block_pack(pack);
	clin_test_set_pack_spawn_items(mode == 'P');
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc) {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD, .local_entity = 0},
	});
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_seed_math_random(0x73C0DE);
	s->rand_src.seed = 0x9E11u;
	for(int z = -ISLAND; z <= ISLAND; z++)
		for(int x = -ISLAND; x <= ISLAND; x++)
			split_populate(s, x, z);
	while(dict_server_chunks_size(s->worlds[0].chunks)) {
		dict_server_chunks_it_t it;
		dict_server_chunks_it(it, s->worlds[0].chunks);
		int64_t id = dict_server_chunks_ref(it)->key;
		assert(server_world_save_chunk(&s->worlds[0], true, S_CHUNK_X(id), S_CHUNK_Z(id)));
	}
	s->worlds[0].generator = NULL; /* disk-only streaming: deterministic */
	s->player.x = 8.5;
	s->player.z = 8.5;
	s->player.y = 4.0 + PLAYER_EYE_HEIGHT;
	s->input_ack_pending = 1;
	s->input_epoch_counter = 1;
	s->player.finished_loading = true;
	for(unsigned n = 0; n < 120; n++) {
		server_local_test_update_once(s);
		drain_to_empty();
	}
	s->input_ack_pending = 0;
	for(unsigned n = 0; n < 20; n++) {
		server_local_test_update_once(s);
		drain_to_empty();
	}
	/* the island is resident on the client */
	unsigned resident = 0;
	for(int z = -1; z <= 1; z++)
		for(int x = -1; x <= 1; x++)
			resident += world_find_chunk(&gstate.world, x * 16, 4, z * 16) != NULL;
	printf("  arm %s: %u/9 centre chunks resident on the client\n", arm_name(mode),
		   resident);
	assert(resident == 9 && "fixture: the near ring streamed to the real client");

	/* the edit phase (traced) */
	struct thread rescue;
	s_rescue_stop = 0;
	clin_rpc_pending_stats(NULL, NULL, NULL, &s_rescue_seen0);
	assert(thread_create(&rescue, rescue_entry, NULL, 64));
	clin_test_set_consume_observer(consumed, NULL);
	s_drain_us = 0;
	s_setblock_n = 0;
	uint8_t hh = 0;
	(void)server_world_get_height(&s->worlds[0], 8, 8, &hh);
	const int y = hh + 6;
	/* the stone floor the torches stand on, one 16-block row per drain (the
	 * single-threaded unpacked arm must stay below 16 + 128 outstanding) */
	for(int dx = -8; dx < 8; dx++) {
		for(int dz = -8; dz < 8; dz++)
			(void)server_world_set_block(&s->worlds[0], dx, y - 1, dz,
				(struct block_data) {.type = BLOCK_STONE});
		drain_to_empty();
	}
	server_local_test_update_once(s);
	drain_to_empty();
	assert(!clin_rpc_publication_faulted() && !server_local_light_stopped(s));
	uint32_t packs0, rec0;
	clin_rpc_pack_stats(&packs0, &rec0);
	uint64_t region[TICKS];
	for(int t = 0; t < TICKS; t++) {
		struct server_world* w = &s->worlds[0];
		if(t % 4 == 0) { /* the 8x8 roof, a TIME_SET barrier in the middle */
			const bool on = ((t / 4) & 1) == 0;
			for(int k = 0; k < 64; k++) {
				if(k == 32) {
					struct client_rpc ts = {.type = CRPC_TIME_SET,
						.payload.time_set = s->world_time};
					clin_rpc_send(&ts);
				}
				(void)server_world_set_block(w, -4 + k % 8, y + 4, -4 + k / 8,
					(struct block_data) {.type = on ? BLOCK_STONE : BLOCK_AIR});
			}
		}
		if(t % 4 == 2) /* the 4x4 torch grid on a stone floor */
			for(int k = 0; k < 16; k++) {
				const bool on = ((k + t / 4) & 1) != 0;
				(void)server_world_set_block(w, -6 + 2 * (k % 4), y, -6 + 2 * (k / 4),
					(struct block_data) {.type = on ? BLOCK_TORCH : BLOCK_AIR,
										 .metadata = on ? 5 : 0});
			}
		if(t % 20 == 10) /* a water source: fluid cascades publish too */
			(void)server_world_set_block(w, 6 + (t / 20) % 4, y + 1, 6,
				(struct block_data) {.type = BLOCK_WATER_STILL});
		/* G9 startup class (real-Wii k_pack200 seq 20-40): the perf_stress
		 * burst -- THREE power-4 blasts in one tick on a ring of radius 28
		 * around the player, bearing rotating 47 degrees per burst
		 * (server_local_perf_stress_tick's shape). Explosion pass 3 sends
		 * drop roll -> SPAWN_ITEM, then SET_BLOCK, per broken cell. */
		const bool blast = t % 40 == 20;
		uint64_t blast_before = 0;
		if(blast) {
			/* count from the first blast: an exhaustion rescue may consume
			 * part of the burst while it is still being produced */
			thread_mutex_lock(&s_consumer);
			memset(s_hist, 0, sizeof s_hist);
			s_hist_on = true;
			blast_before = s_trace_n;
			thread_mutex_unlock(&s_consumer);
			const float base = (float)((t / 40) * 47 % 360) * 0.01745329f;
			for(int i = 0; i < 3; i++) {
				const float a = base + (float)(i * 120) * 0.01745329f;
				const int bx = (int)floorf(8.5f + 28.0f * cosf(a));
				const int bz = (int)floorf(8.5f + 28.0f * sinf(a));
				uint8_t bh = 0;
				(void)server_world_get_height(w, bx, bz, &bh);
				vec3 c = {(float)bx + 0.5f, (float)bh - 0.5f - (float)(i % 2),
						  (float)bz + 0.5f};
				server_world_explode(w, c, 4.0f);
			}
		}
		const uint64_t before = s_trace_n;
		uint32_t pend_edit = 0;
		clin_rpc_pending_stats(&pend_edit, NULL, NULL, NULL);
		server_local_test_update_once(s);
		uint32_t pend_tick = 0;
		clin_rpc_pending_stats(&pend_tick, NULL, NULL, NULL);
		if(getenv("PACK_EQUIV_DEBUG")) {
			uint32_t pend = 0, ex = 0;
			clin_rpc_pending_stats(&pend, NULL, NULL, &ex);
			printf("  tick %d: pending after edits %u, after tick %u, exhausted %u, "
				   "faulted %d\n", t, pend_edit, pend, ex,
				   clin_rpc_publication_faulted());
		}
		drain_to_empty();
		if(getenv("PACK_EQUIV_DEBUG"))
			printf("  tick %d: %" PRIu64 " consumed\n", t, s_trace_n - before);
		if(blast) {
			s_hist_on = false;
			s_blast_ticks++;
			const uint32_t entries = pend_edit > pend_tick ? pend_edit : pend_tick;
			if(entries > s_blast_entries_max) s_blast_entries_max = entries;
			s_blast_setblocks += s_hist[CRPC_SET_BLOCK];
			s_blast_spawns += s_hist[CRPC_SPAWN_ITEM];
			s_blast_envelopes += (uint32_t)(s_trace_n - blast_before);
		}
		region[t] = client_region_digest(-16, 16, y - 4, y + 8, -16, 16);
		if(t % 4 == 0) { /* the consumer really applied every roof cell */
			assert(!clin_rpc_publication_faulted()
				   && "the exhaustion rescue kept publication alive");
			const uint8_t want = ((t / 4) & 1) == 0 ? BLOCK_STONE : BLOCK_AIR;
			for(int k = 0; k < 64; k++) {
				struct block_data sb = {0};
				assert(server_world_get_block(&s->worlds[0], -4 + k % 8, y + 4,
											  -4 + k / 8, &sb) && sb.type == want);
				const struct block_data cb = world_get_block(&gstate.world,
					-4 + k % 8, y + 4, -4 + k / 8);
				assert(cb.type == want && "client world holds the server roof");
			}
			s_roof_checks++;
		}
	}
	uint32_t packs1, rec1;
	clin_rpc_pack_stats(&packs1, &rec1);
	s_rescue_stop = 1;
	thread_join(&rescue);
	uint32_t exhausted = 0;
	clin_rpc_pending_stats(NULL, NULL, NULL, &exhausted);
	printf("  arm %s: %u roof states verified on the client; exhaustion waits %u "
		   "(consumer rescues %u), publication %s\n",
		   arm_name(mode), s_roof_checks, exhausted, s_rescues,
		   clin_rpc_publication_faulted() ? "FAULTED" : "ok");
	assert(!clin_rpc_publication_faulted() && !server_local_light_stopped(s)
		   && "fixture validity: the session never stopped publishing");
	clin_test_set_consume_observer(NULL, NULL);
	const uint64_t consumed_n = s_trace_n, trace_hash = s_trace_hash;
	printf("  arm %s: %" PRIu64 " consumed envelopes (%" PRIu64 " block updates), "
		   "trace %016" PRIx64 ", packs %u carrying %u appended records\n",
		   arm_name(mode), consumed_n, s_setblock_n, trace_hash, packs1 - packs0,
		   rec1 - rec0);
	printf("  arm %s: consumer drain %.0f us total = %.2f us per block update\n",
		   arm_name(mode), s_drain_us,
		   s_setblock_n ? s_drain_us / (double)s_setblock_n : 0.0);
	/* section markers keep the comparison self-describing */
	const uint32_t marker = 0xC0FFEE01u;
	out_bytes(&marker, sizeof marker);
	out_bytes(region, sizeof region);
	/* final client bytes of the streamed island */
	for(int x = -ISLAND * 16; x < (ISLAND + 1) * 16; x++)
		for(int z = -ISLAND * 16; z < (ISLAND + 1) * 16; z++)
			for(int yy = 0; yy < WORLD_HEIGHT; yy++) {
				struct block_data b = world_get_block(&gstate.world, x, yy, z);
				const uint8_t v[4] = {b.type, b.metadata, b.sky_light, b.torch_light};
				out_bytes(v, sizeof v);
			}
	const uint32_t marker2 = 0xC0FFEE02u;
	out_bytes(&marker2, sizeof marker2);
	unsigned client_entities = 0;
	const uint64_t edigest = client_entities_digest(&client_entities);
	out_bytes(&client_entities, sizeof client_entities);
	out_bytes(&edigest, sizeof edigest);
	/* the server simulation */
	for(int cx = -ISLAND; cx <= ISLAND; cx++)
		for(int cz = -ISLAND; cz <= ISLAND; cz++) {
			struct server_chunk* sc
				= dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(cx, cz));
			const uint8_t present = sc != NULL;
			out_bytes(&present, 1);
			if(!sc) continue;
			const size_t cells = CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
			out_bytes(sc->ids, cells);
			out_bytes(sc->metadata, cells / 2);
			out_bytes(sc->lighting_sky, cells / 2);
			out_bytes(sc->lighting_torch, cells / 2);
			out_bytes(sc->heightmap, CHUNK_SIZE * CHUNK_SIZE);
		}
	struct server_local_math_random_state math;
	server_local_math_random_capture(&math);
	const uint64_t rngs[] = {(uint64_t)s->rand_src.seed, (uint64_t)s->container_drop_rng.seed,
		(uint64_t)s->dispenser_break_rng.seed, (uint64_t)s->entity_rng.seed,
		math.rng.seed, s->world_time};
	out_bytes(rngs, sizeof rngs);
	const struct server_world* w0 = &s->worlds[0];
	const uint32_t q[] = {w0->current_tick, (uint32_t)w0->scheduled_tick_count};
	out_bytes(q, sizeof q);
	for(size_t i = 0; i < w0->scheduled_tick_count; i++) {
		const struct scheduled_tick* tick = &w0->scheduled_ticks[
			(w0->scheduled_tick_head + i) % w0->scheduled_tick_capacity];
		const uint32_t f[] = {(uint32_t)tick->x, (uint32_t)tick->y, (uint32_t)tick->z,
			tick->fire_at_tick, tick->block_type, tick->queue_flags};
		out_bytes(f, sizeof f);
	}
	printf("  arm %s: %u blast bursts (3 power-4 blasts each): %u SET_BLOCK + %u "
		   "SPAWN_ITEM in %u consumed envelopes; ring entries after a burst max %u; "
		   "%u client entities\n",
		   arm_name(mode), s_blast_ticks, s_blast_setblocks, s_blast_spawns,
		   s_blast_envelopes, s_blast_entries_max, client_entities);
	/* summary line the parent reads back */
	const uint32_t tail[TAIL_N] = {packs1 - packs0, rec1 - rec0, (uint32_t)consumed_n,
		(uint32_t)s_setblock_n, exhausted, s_blast_entries_max, s_blast_spawns};
	assert(fwrite(tail, 1, sizeof tail, s_out) == sizeof tail);
	fclose(s_out);
	clin_update_drain();
	world_pre_render_clear(&gstate.world);
	world_render_completed(&gstate.world, false);
	world_destroy(&gstate.world);
	dict_entity_clear(gstate.entities);
	split_destroy(s);
	return 0;
}

static unsigned char* slurp(const char* path, size_t* n) {
	FILE* f = fopen(path, "rb");
	if(!f) return NULL;
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char* b = malloc((size_t)len + 1);
	assert(b);
	*n = fread(b, 1, (size_t)len, f);
	fclose(f);
	return b;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if(argc == 4 && strcmp(argv[1], "arm") == 0)
		return arm(argv[2][0], argv[3]);
	char dir[220], outs[4][260], cmd[1200];
	test_fixture_dir("test_rpc_block_pack_equiv", dir, sizeof dir);
#ifdef _WIN32
	_mkdir(dir);
#else
	mkdir(dir, 0777);
#endif
	/* U unpacked, U2 unpacked again (control), B the R21 SET_BLOCK-only
	 * pack, P the shipping pack (SET_BLOCK + SPAWN_ITEM records) */
	const char* arms[4] = {"U", "U", "B", "P"};
	for(int i = 0; i < 4; i++) {
		snprintf(outs[i], sizeof outs[i], "%s/%s%s.bin", dir, arms[i], i == 1 ? "2" : "");
		snprintf(cmd, sizeof cmd, "\"\"%s\" arm %s \"%s\"\"", argv[0], arms[i], outs[i]);
#ifndef _WIN32
		snprintf(cmd, sizeof cmd, "\"%s\" arm %s \"%s\"", argv[0], arms[i], outs[i]);
#endif
		printf("arm %s%s\n", arms[i], i == 1 ? " (control)" : "");
		const int rc = system(cmd);
		if(rc != 0) {
			printf("FAIL: arm %s exited %d\n", arms[i], rc);
			return 1;
		}
	}
	size_t n[4];
	unsigned char* b[4];
	uint32_t tl[4][TAIL_N];
	enum { TAIL = TAIL_N * sizeof(uint32_t) };
	for(int i = 0; i < 4; i++) {
		b[i] = slurp(outs[i], &n[i]);
		assert(b[i] && n[i] >= TAIL);
		memcpy(tl[i], b[i] + n[i] - TAIL, TAIL);
	}
	int fail = 0;
	const bool control = n[0] == n[1] && memcmp(b[0], b[1], n[0] - TAIL) == 0;
	printf("  %s: determinism control (U == U2, %zu bytes)\n", control ? "PASS" : "FAIL", n[0]);
	fail += !control;
	const bool really = tl[0][0] == 0 && tl[2][0] > 0 && tl[3][0] > 0
		&& tl[3][1] > tl[2][1] && tl[0][6] > 0;
	printf("  %s: arm U created no pack; arm B %u packs / %u appended records; arm P "
		   "%u packs / %u appended records (more: SPAWN_ITEM joins); %u SPAWN_ITEM "
		   "in the bursts\n", really ? "PASS" : "FAIL", tl[2][0], tl[2][1], tl[3][0],
		   tl[3][1], tl[0][6]);
	fail += !really;
	printf("  info: exhaustion waits U %u / B %u / P %u; ring entries after a "
		   "3-blast burst U %u / B %u / P %u (same workload, same 128 ring)\n",
		   tl[0][4], tl[2][4], tl[3][4], tl[0][5], tl[2][5], tl[3][5]);
	const bool absorbed = tl[3][4] <= tl[2][4] && tl[2][4] <= tl[0][4]
		&& tl[3][5] < tl[2][5];
	printf("  %s: packing never adds an exhaustion wait, and SPAWN_ITEM packing "
		   "lowers the burst's ring entries\n", absorbed ? "PASS" : "FAIL");
	fail += !absorbed;
	/* everything except the pack/exhaustion-count tail must be identical */
	for(int i = 2; i < 4; i++) {
		const bool same = n[0] == n[i] && memcmp(b[0], b[i], n[0] - TAIL) == 0
			&& tl[0][2] == tl[i][2] && tl[0][3] == tl[i][3] && tl[0][6] == tl[i][6];
		size_t first = 0;
		if(!same)
			while(first < n[0] && first < n[i] && b[0][first] == b[i][first]) first++;
		printf("  %s: arm %s == unpacked: consumer-effect trace (%u envelopes, %u "
			   "block updates), per-tick client digests, final client bytes, client "
			   "entities and server simulation bytes (%zu bytes)%s\n",
			   same ? "PASS" : "FAIL", arms[i], tl[0][2], tl[0][3], n[0] - TAIL,
			   same ? "" : " -- first difference below");
		if(!same)
			printf("  first differing byte offset %zu (sizes %zu / %zu)\n", first, n[0], n[i]);
		fail += !same;
	}
	for(int i = 0; i < 4; i++) free(b[i]);
	test_fixture_dir_cleanup(dir);
	printf("%s test_rpc_block_pack_equiv\n", fail ? "FAIL" : "PASS");
	return fail ? 1 : 0;
}
