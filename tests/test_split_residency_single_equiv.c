#include "settle_observer.h"
/* Copyright (c) 2026 CaveWii_v2 contributors
 * Frozen against the complete 2cfa185 engine. Actual update -> RPC pool -> full
 * column snapshots, including a deliberately exhausted pool. No model of the
 * selector or the serializer participates in this trace. */
#include "platform/watchdog.h"
#include "platform/audio.h"
#include "support/split_residency_fixture.h"
#include "worldgen/worldgen.h"
#include <inttypes.h>
#include <stdatomic.h>
#ifndef CW_CAPTURE_PINNED_BASELINE
#include "tps_observer.h"
#include "carve_observer.h"
static uint64_t observer_clock;
static uint64_t observer_cpu(void) { return observer_clock += 1000; }
#endif
static uint64_t trace;
static unsigned chunks, calls, unloads;
/* Observer-only accounting. The real producer owns selection and serialization;
 * the tick-12 slot hook owns exactly the near-admission/refusal boundary. */
static struct server_local* observed_server;
static unsigned observed_tick, near_admitted, refreshed, retry_admitted;
static unsigned busy_before;
static bool refused;
static int64_t selected_key, refused_key;
static int64_t refused_debt[512];
static unsigned refused_debt_count;
struct accepted_snapshot {
	int64_t key;
	struct accepted_snapshot* next;
	uint8_t planes[82176];
};
static struct accepted_snapshot* accepted;

static void snapshot_planes(uint8_t* out, const struct chunk_rpc_slot* slot) {
	memcpy(out, slot->ids, 32768);
	memcpy(out + 32768, slot->metadata, 16384);
	memcpy(out + 49152, slot->lighting_sky, 16384);
	memcpy(out + 65536, slot->lighting_torch, 16384);
	memcpy(out + 81920, slot->heightmap, 256);
}

static bool matches_live(const uint8_t* p, const struct server_chunk* sc) {
	return !memcmp(p, sc->ids, 32768)
		&& !memcmp(p + 32768, sc->metadata, 16384)
		&& !memcmp(p + 49152, sc->lighting_sky, 16384)
		&& !memcmp(p + 65536, sc->lighting_torch, 16384)
		&& !memcmp(p + 81920, sc->heightmap, 256);
}

static struct accepted_snapshot* last_snapshot(int64_t key) {
	for(struct accepted_snapshot* p = accepted; p; p = p->next)
		if(p->key == key) return p;
	return NULL;
}

static bool observe_selection(const struct server_world_history_event* e) {
	if(e->world == &observed_server->worlds[0] && observed_tick == 12
	   && !strcmp(e->operation, "provide") && !strcmp(e->edge, "end")
	   && !strcmp(e->outcome, "ok"))
		selected_key = S_CHUNK_ID(e->x, e->z);
	return true;
}

static void account_snapshot(const struct client_rpc* rpc) {
	const int64_t key = S_CHUNK_ID(rpc->payload.chunk.x / CHUNK_SIZE,
		rpc->payload.chunk.z / CHUNK_SIZE);
	struct server_chunk* sc = dict_server_chunks_get(observed_server->worlds[0].chunks, key);
	assert(sc);
	static uint8_t now[82176];
	snapshot_planes(now, rpc->payload.chunk.slot);
	assert(matches_live(now, sc)); /* all five accepted planes, not a digest */
	struct accepted_snapshot* previous = last_snapshot(key);
	if(observed_tick == 12) {
		if(!refused) {
			assert(!sc->sent && ++near_admitted == 1);
			assert(key == S_CHUNK_ID(3, 3));
		} else {
			/* Changed stored light must publish after near sends (RetroMCP
			 * World:491-500, MinecraftServer:328-338). No aggregate RPC pin. */
			assert(sc->sent && sc->pub_debt && previous);
			assert(memcmp(previous->planes, now, sizeof now) != 0);
			refreshed++;
		}
	}
	if(refused && observed_tick > 12 && key == refused_key && !sc->sent)
		retry_admitted++;
	if(!previous) {
		previous = malloc(sizeof(*previous));
		assert(previous);
		previous->key = key;
		previous->next = accepted;
		accepted = previous;
	}
	memcpy(previous->planes, now, sizeof now);
}

static void assert_no_lost_debt(void) {
	for(unsigned i = 0; i < refused_debt_count; i++) {
		struct accepted_snapshot* p = last_snapshot(refused_debt[i]);
		struct server_chunk* sc = dict_server_chunks_get(observed_server->worlds[0].chunks, refused_debt[i]);
		assert(p && sc && sc->sent);
		if(!matches_live(p->planes, sc))
			assert(sc->pub_debt);
	}
}
/* Write-only attribution records; assertions and folded bytes are unchanged.
 * CW_ATTR_DUMP=<prefix> writes the complete stream and a field/offset map. */
static FILE* attr_bytes;
static FILE* attr_map;
static uint64_t attr_offset;
static void attr_record(const void* data, size_t n, const char* field,
						const char* function) {
	const char* prefix = getenv("CW_ATTR_DUMP");
	if(!prefix || !prefix[0]) return;
	if(!attr_bytes) {
		char path[1024];
		int written = snprintf(path, sizeof(path), "%s.bin", prefix);
		assert(written > 0 && (size_t)written < sizeof(path));
		attr_bytes = fopen(path, "wb"); assert(attr_bytes);
		written = snprintf(path, sizeof(path), "%s.map", prefix);
		assert(written > 0 && (size_t)written < sizeof(path));
		attr_map = fopen(path, "w"); assert(attr_map);
	}
	assert(fprintf(attr_map, "%" PRIu64 " %zu %s %s\n",
				   attr_offset, n, function, field) > 0);
	assert(fwrite(data, 1, n, attr_bytes) == n);
	attr_offset += n;
}

static void bytes_at(const void* ptr, size_t n, const char* field,
                     const char* function) {
	attr_record(ptr, n, field, function);
	const unsigned char* p = ptr;
	for(size_t i = 0; i < n; i++) {
		trace ^= p[i];
		trace *= UINT64_C(1099511628211);
	}
}
#define bytes(ptr, n) bytes_at((ptr), (n), #ptr, __func__)

static void observe(const struct client_rpc* rpc, void* user) {
	(void)user;
	clin_drain_discard();
	calls++;
	if(rpc->type == CRPC_UNLOAD_CHUNK) unloads++;
	uint32_t type = rpc->type;
	bytes(&type, sizeof(type));
	if(rpc->type != CRPC_CHUNK)
		return;
	account_snapshot(rpc);
	chunks++;
	bytes(&rpc->payload.chunk.x, sizeof(rpc->payload.chunk.x));
	bytes(&rpc->payload.chunk.z, sizeof(rpc->payload.chunk.z));
	struct chunk_rpc_slot* slot = rpc->payload.chunk.slot;
	size_t n = CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	bytes(slot->ids, n);
	bytes(slot->metadata, n / 2);
	bytes(slot->lighting_sky, n / 2);
	bytes(slot->lighting_torch, n / 2);
	bytes(slot->heightmap, CHUNK_SIZE * CHUNK_SIZE);
}
static atomic_bool drain_paused = ATOMIC_VAR_INIT(false);
static atomic_bool drain_ack = ATOMIC_VAR_INIT(false);
static atomic_bool drain_run = ATOMIC_VAR_INIT(false);
static atomic_bool release_requested = ATOMIC_VAR_INIT(false);
static atomic_bool release_completed = ATOMIC_VAR_INIT(false);
static struct chunk_rpc_slot* held[64];
static unsigned held_count;
static bool saw_backpressure;
static void* drain(void* unused) {
	(void)unused;
	while(atomic_load(&drain_run)) {
		if(atomic_exchange(&release_requested, false)) {
			/* The producer entered acquire with an empty pool. It must wait
			 * for a consumer, then publish the same frozen payload/order. */
			thread_msleep(20);
			while(held_count)
				clin_chunk_slot_release(held[--held_count]);
			saw_backpressure = true;
			atomic_store(&release_completed, true);
		}
		if(atomic_load(&drain_paused)) {
			atomic_store(&drain_ack, true);
			thread_msleep(1);
			continue;
		}
		atomic_store(&drain_ack, false);
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}
static void release_at_wait(enum wd_phase_srv phase, void* unused) {
	(void)unused;
	if(phase == WDS_SLOT_WAIT) {
		watchdog_test_set_srv_phase_observer(NULL, NULL);
		/* bf92f3e33 makes acquisition nonblocking: prove the empty pool,
		 * then await the fixture consumer here instead of assuming acquire
		 * parks the producer. Keep the frozen per-update payload/order. */
		assert(clin_chunk_slot_try_acquire() == NULL);
		atomic_store(&release_requested, true);
		ptime_t start = time_get();
		while(!atomic_load(&release_completed)) {
			assert(time_diff_s(start, time_get()) < 20);
			thread_msleep(1);
		}
	}
}
static unsigned slot_waits;
static bool failed_send;
static void fail_second_send(enum wd_phase_srv phase, void* unused) {
	(void)unused;
	if(atomic_load(&drain_paused) && phase != WDS_SLOT_WAIT) {
		/* This exact stamp occurs only after the real failed try_acquire.
		 * Check while the pool is still exhausted, before releasing it. */
		assert(phase == WDS_NEAR_SCAN && slot_waits == 2 && near_admitted == 1);
		assert(clin_chunk_slot_try_acquire() == NULL);
		struct server_chunk* sc = dict_server_chunks_get(observed_server->worlds[0].chunks, refused_key);
		assert(sc && !sc->sent && sc->populated);
		dict_server_chunks_it_t it;
		for(dict_server_chunks_it(it, observed_server->worlds[0].chunks);
			!dict_server_chunks_end_p(it); dict_server_chunks_next(it)) {
			const struct server_chunk* c = &dict_server_chunks_cref(it)->value;
			if(c->sent && c->pub_debt) {
				assert(refused_debt_count < 512);
				refused_debt[refused_debt_count++] = dict_server_chunks_cref(it)->key;
			}
		}
		refused = true;
		while(held_count) clin_chunk_slot_release(held[--held_count]);
		atomic_store(&drain_paused, false);
		while(atomic_load(&drain_ack)) thread_msleep(1);
		failed_send = true;
		watchdog_test_set_srv_phase_observer(NULL, NULL);
	} else if(phase == WDS_SLOT_WAIT && ++slot_waits == 2) {
		assert(near_admitted == 1);
		refused_key = selected_key;
		struct server_chunk* sc = dict_server_chunks_get(observed_server->worlds[0].chunks, refused_key);
		assert(sc && !sc->sent && sc->populated);
		atomic_store(&drain_paused, true);
		while(!atomic_load(&drain_ack)) thread_msleep(1);
		struct chunk_rpc_slot* slot;
		while((slot = clin_chunk_slot_try_acquire()) != NULL) {
			assert(held_count < 64);
			held[held_count++] = slot;
		}
	}
}
static uint64_t run(bool pressure) {
	struct server_local* s = split_fixture();
	observed_server = s;
	observed_tick = 0;
	near_admitted = refreshed = retry_admitted = 0;
	refused_debt_count = 0;
	refused = false;
	assert(!accepted);
	s->rand_src = (struct random_gen) {.seed = 12345};
	/* F4-RNG: both backpressure arms must populate/drop on the same stream. */
	jrand_set_seed(&s->worlds[0].rand, 4242);
	server_local_test_seed_body_rand(s, &s->player, 6789);
	/* Active mob sound scheduling has its own lazily time-seeded RNG. Freeze
	 * cosmetic streams too: an extra idle-sound RPC otherwise flakes the trace. */
	*server_local_sound_rng() = (struct random_gen) {.seed = 6789};
	*audio_cosmetic_rng() = (struct random_gen) {.seed = 9876};
	s->world.superflat = false;
	worldgen_set_superflat(s->world.generator, false);
#ifndef CW_CAPTURE_PINNED_BASELINE
	/* These fields did not exist in 2cfa185. The scenario below is shared. */
	/* Allocating the scheduler alone must never opt a one-anchor run in. */
	s->residency = mem2_alloc(sizeof(*s->residency));
	assert(s->residency);
	memset(s->residency, 0xa5, sizeof(*s->residency));
	s->anchor_sep = 0;
	s->residency_anchors[0].x = 123456;
	s->anchor_near_owed[0] = 9876;
	s->anchor_oldest[0] = 5432;
#endif
	server_world_test_set_gen_elapsed_mode(
		SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	for(int z = -2; z <= 2; z++)
		for(int x = -2; x <= 2; x++)
			split_populate(s, x, z);
	server_local_test_set_near_send_minimum(4);
	struct server_chunk* evicted = split_populate(s, 80, 80);
	evicted->sent = true;
	trace = UINT64_C(14695981039346656037);
	chunks = calls = unloads = slot_waits = 0;
	failed_send = false;
	clin_test_set_rpc_observer(observe, NULL);
	server_world_test_history_hook = observe_selection;
	struct thread consumer;
	atomic_store(&drain_run, true);
	atomic_store(&release_completed, false);
	assert(thread_create(&consumer, drain, NULL, 64));
	for(unsigned tick = 0; tick < 40; tick++) {
		observed_tick = tick;
		if(pressure && tick == 0) {
			struct chunk_rpc_slot* slot;
			while((slot = clin_chunk_slot_try_acquire()) != NULL) {
				assert(held_count < 64);
				held[held_count++] = slot;
			}
			assert(held_count);
			watchdog_test_set_srv_phase_observer(release_at_wait, NULL);
		}
		if(tick == 4) {
			s->player.finished_loading = true;
			s->render_demo_mob = 0;
		}
		if(tick == 8) {
			s->player.x = 32.5;
			server_world_test_fail_gen_attempts(1);
		}
		if(tick == 24)
			s->player.x = -32.5;
		if(tick == 12) {
			server_local_test_near_exit_counters(NULL, NULL, &busy_before, NULL, NULL);
			watchdog_test_set_srv_phase_observer(fail_second_send, NULL);
		}
		unsigned before_chunks = chunks;
		server_local_test_update_once(s);
		if(tick == 12) {
			unsigned busy_after;
			server_local_test_near_exit_counters(NULL, NULL, &busy_after, NULL, NULL);
			assert(near_admitted == 1 && failed_send && refused && slot_waits == 2);
			assert(busy_after == busy_before + 1);
			struct server_chunk* sc = dict_server_chunks_get(s->worlds[0].chunks, refused_key);
			assert(sc && !sc->sent && sc->populated && retry_admitted == 0);
			assert(chunks == before_chunks + near_admitted + refreshed);
			assert_no_lost_debt();
			printf("tick12 near=%u refused=1 pool_empty=1 refresh=%u retry=(%d,%d)\n",
				near_admitted, refreshed, S_CHUNK_X(refused_key), S_CHUNK_Z(refused_key));
		}
		watchdog_test_set_srv_phase_observer(NULL, NULL);
	}
	atomic_store(&drain_run, false);
	thread_join(&consumer);
	clin_test_set_rpc_observer(NULL, NULL);
	server_world_test_history_hook = NULL;
	assert(!server_world_test_history_failed() && retry_admitted == 1);
	while(accepted) {
		struct accepted_snapshot* next = accepted->next;
		free(accepted);
		accepted = next;
	}
	bytes(&s->rand_src.seed, sizeof(s->rand_src.seed));
	bytes(&s->rand_src.has_next_gaussian,
		  sizeof(s->rand_src.has_next_gaussian));
	bytes(&s->rand_src.next_gaussian, sizeof(s->rand_src.next_gaussian));
	bytes(&s->world.hell_populate_rng.seed,
		  sizeof(s->world.hell_populate_rng.seed));
	bytes(&s->world.current_tick, sizeof(s->world.current_tick));
	printf("single trace=%016" PRIx64 " chunks=%u RPCs=%u pressure=%d\n", trace,
		   chunks, calls, pressure);
	fflush(stdout);
#ifndef CW_CAPTURE_PINNED_BASELINE
	assert(s->residency_anchors[0].x == 123456 && s->anchor_near_owed[0] == 9876
		   && s->anchor_oldest[0] == 5432);
#endif
	assert(chunks > 16 && unloads > 0 && s->world.current_tick == 36);
	assert(!server_world_is_chunk_loaded(&s->world, 80, 80));
	assert(server_world_test_gen_failures_remaining() == 0);
	uint64_t result = trace;
	split_destroy(s);
	if(attr_bytes) { assert(fflush(attr_bytes) == 0); assert(fflush(attr_map) == 0); }
	return result;
}

unsigned server_local_test_publish_debt_w(struct server_local*, struct server_world*);
static unsigned control_refreshes;
static void observe_control(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type != CRPC_CHUNK) return;
	struct server_chunk* sc = dict_server_chunks_get(observed_server->worlds[0].chunks,
		S_CHUNK_ID(rpc->payload.chunk.x / 16, rpc->payload.chunk.z / 16));
	assert(sc && sc->sent && sc->pub_debt);
	static uint8_t now[82176];
	snapshot_planes(now, rpc->payload.chunk.slot);
	assert(matches_live(now, sc));
	control_refreshes++;
}

static void publication_controls(void) {
	/* Isolated actual publisher: unchanged debt is suppressed, exhausted
	 * slots retain changed debt, then retry accepts the current live image.
	 * This fixture contributes no bytes to the frozen 40-tick trace. */
	struct server_local* s = split_fixture();
	observed_server = s;
	struct server_chunk* sc = split_populate(s, 0, 0);
	assert(server_world_light_drain(&s->worlds[0]));
	sc->sent = sc->pub_debt = true;
	sc->pub_digest_valid = false;
	control_refreshes = 0;
	clin_test_set_rpc_observer(observe_control, NULL);
	atomic_store(&drain_run, true);
	struct thread consumer;
	assert(thread_create(&consumer, drain, NULL, 64));
	assert(server_local_test_publish_debt_w(s, &s->worlds[0]) == 1);
	assert(control_refreshes == 1 && !sc->pub_debt);
	sc->pub_debt = true; /* redundant dirty mark, identical accepted planes */
	assert(server_local_test_publish_debt_w(s, &s->worlds[0]) == 0);
	assert(control_refreshes == 1 && !sc->pub_debt);
	assert(server_world_set_block_quiet(&s->worlds[0], 8, 8, 8,
		(struct block_data) {.type = BLOCK_LAVA_STILL}));
	assert(server_world_light_drain(&s->worlds[0]) && sc->pub_debt);
	atomic_store(&drain_paused, true);
	while(!atomic_load(&drain_ack)) thread_msleep(1);
	clin_drain_discard();
	struct chunk_rpc_slot* slot;
	while((slot = clin_chunk_slot_try_acquire()) != NULL) {
		assert(held_count < 64);
		held[held_count++] = slot;
	}
	assert(held_count && clin_chunk_slot_try_acquire() == NULL);
	assert(server_local_test_publish_debt_w(s, &s->worlds[0]) == 0);
	assert(sc->pub_debt && control_refreshes == 1);
	while(held_count) clin_chunk_slot_release(held[--held_count]);
	atomic_store(&drain_paused, false);
	while(atomic_load(&drain_ack)) thread_msleep(1);
	assert(server_local_test_publish_debt_w(s, &s->worlds[0]) == 1);
	assert(!sc->pub_debt && control_refreshes == 2);
	assert(server_local_test_publish_debt_w(s, &s->worlds[0]) == 0);
	assert(control_refreshes == 2);
	atomic_store(&drain_run, false);
	thread_join(&consumer);
	clin_test_set_rpc_observer(NULL, NULL);
	split_destroy(s);
	puts("PASS unchanged refresh suppression and lossless pool refusal/retry");
}

int main(void) {
	assert(clin_init());
	blocks_init();
	uint64_t a = run(false), b = run(true);
	assert(a == b && saw_backpressure);
	publication_controls();
	/* Captured by tools/capture_split_single_golden.py from the full 2cfa185
	 * tree, with no current engine objects or dependency stubs. */
	/* Re-baselined 2026-09-24 (next-gaps-0924 M2b): bisected across the M2 first-parent merges (tmp/jobs/next-gaps-0924/m2b-bisect-*.log): W1C item physics (deb39952, EntityItem.java:45-69 per-entity Random/pushOut/bounce), W1A player physics + suffocation (d4cf78ee, EntityLiving.moveEntityWithHeading:455-497, Entity.isEntityInsideOpaqueBlock:851-863) and W1B mob movement (7c7a8ff2, EntityLiving.moveEntityWithHeading:479-497) moved the observed stream; every value is the one printed by this file at 0b33e352 and again after the F1b/W1E-e merges. */
	/* Re-baselined 2026-09-27 (lane C mob motion units S2): the S1 tree 325a10e9 prints
	 * exactly the old value (single trace=f8b4de82bca0e1e0, chunks=157, RPCs=1176); S2
	 * prints 20a93d2c01c2a354 with the SAME 157 chunks and one more RPC (1177) -- the
	 * trace folds the mobs' server state and their move sends, which S2's Beta mob
	 * motion (move-then-gravity, jump(), knockBack at the hit) changes; residency and
	 * backpressure are unchanged (both runs still agree). Previous: 0xf8b4de82bca0e1e0. */
	/* pre-V6 pin (P=625ac64d4): 0x20a93d2c01c2a354.
	 * FAST re-pin (maintainer 2026-10-11): first divergence field180/offset1643840 SET_BLOCK12->LOCAL_PLAYER_VELOCITY66 in source/network/server_local.c:29010 (d86e8d673/90cf80703). */
	/* R5 FAST re-pin: P=fe2b74e46 reproduces 0x647dda7c086f5093.
	 * First changed byte 11352419 is an emitted chunk ID (sandstone -> air).
	 * Skipping only R5 automatic population in HEAD with the debugger restores
	 * the complete P stream, both pressure arms and all observer checks.
	 * Normal HEAD repeats and evidence: tmp/jobs/integ-1011/sol1j/. */
	/* integ R5 pin was 0x5511ba3765caebba. */
	/* L1 FAST: b11193e03 prints prior647dda7c086f5093; HEAD8/8 full
	 * streams identical across normal/pressure/observer arms. First changed
	 * byte570074 is Block light in CRPC_CHUNK(16,-16), local(11,12,2), inside
	 * generation setter/region repair (CFR MetadataChunkBlock:35-171).
	 * Old pin0x647dda7c086f5093 retained; full evidence l1-single/ on
	 * D:/cavewii-evidence/g9b3-evidence. This is not a Beta expectation. */
	/* Existing capture mode skips only the frozen expectation; all repeated
	 * normal/pressure/observer equivalence assertions still run. */
	/* G9X-6 pin 0x20ae0cf605437c8d (maintainer ruling 3, 2026-10-14);
	 * previous 0x4690c53d53bccebf. All 1178 old RPC records were identical
	 * and in order, tick/RNG tail equal; 43 added CRPC_CHUNK republishes
	 * of Beta-published light, first (3,0) Block 0->15 at (63,7,12).
	 * tmp/jobs/loop/G9X-6/RERECORD.md.
	 * ci-1101/12 FAST re-pin: P=354809385 reproduces 20ae0cf605437c8d
	 * in all five normal/pressure/observer arms. Git bisect identifies the
	 * accepted IN-005 quiet-but-lit setter aa1ec22fb (Chunk.java:215-250:
	 * quiet ID/meta writes still schedule light and run block callbacks).
	 * First changed byte 2794189 is CHUNK(48,16) Block light at world
	 * (63,18,29), low nibble 1->0 (packed 17->16). aa1ec22fb and d2bb7a89c
	 * both reproduce the new pin in all five arms, 207 chunks/1228 RPCs.
	 * This is a CaveWii regression trace, not a Beta oracle expectation.
	 * Evidence: C:/tmp/cavex-jobs/ci-1101/12/{bisect-*,first-single-cell.json,
	 * parent-single-attr.log,moving-single-attr.log,head-single-attr.log}.
	 * Native GDB: the old baseline assertion called UCRT abort (0xc0000409).
	 */
	const uint64_t baseline = getenv("CW_CAPTURE_SINGLE_GOLDEN") ? a
		: UINT64_C(0x30a9fe4e7e8885a3);
	if(!getenv("CW_CAPTURE_SINGLE_GOLDEN"))
		assert(a == baseline);
#ifndef CW_CAPTURE_PINNED_BASELINE
	/* The same frozen real-engine stream with every CPU callback armed.
	 * The artificial clock tests observer neutrality, not CPU performance. */
	char observer_dir[128], observer_prefix[160];
	test_fixture_dir("test_tps_golden", observer_dir, sizeof(observer_dir));
#ifdef _WIN32
	assert(_mkdir(observer_dir) == 0);
#else
	assert(mkdir(observer_dir, 0777) == 0);
#endif
	snprintf(observer_prefix, sizeof(observer_prefix), "%s/", observer_dir);
	assert(tps_observer_test_clock(observer_cpu));
	assert(tps_observer_start(observer_prefix));
	assert(run(false) == a);
	assert(tps_observer_stop());
	assert(tps_observer_test_clock(NULL));
	test_fixture_dir_cleanup(observer_dir);
	assert(carve_observer_start());
	assert(run(false) == baseline);
	assert(carve_observer_stop());
	assert(settle_observer_start());
	assert(run(false) == baseline);
	struct settle_counts settle;
	settle_observer_counts(&settle);
	assert(settle.lost == 0);
	assert(settle.enqueue == settle.dequeue + settle.rpc_outstanding);
	assert(settle.submitted
		   == settle.completed + settle.cancelled + settle.outstanding);
	printf("settle concurrent RPC reconciliation enqueue=%" PRIu64
		   " dequeue=%" PRIu64 " outstanding=%" PRIu64 " lost=%" PRIu64
		   " overflow=%" PRIu64 "\n",
		   settle.enqueue, settle.dequeue, settle.rpc_outstanding, settle.lost,
		   settle.overflow);
	settle_observer_stop(NULL); /* no telemetry in this byte-identity fixture */
#endif
	puts("PASS single-anchor selection/RPC/chunk/light/RNG trace and pool "
		 "backpressure");
	return 0;
}
