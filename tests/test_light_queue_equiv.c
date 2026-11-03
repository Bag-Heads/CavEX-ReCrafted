/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * test_light_queue_equiv -- G9 B4/B5 proofs for the server light queue
 * (ASTRA-1005B B4-B5, ASTRA-G9-RULING 5-9).
 *
 *   equiv     : a long seeded random edit run (stone/glass/torch/glowstone/
 *               leaves/ice/water/air, across the x/z = 0 and 16 chunk seams)
 *               on three identical sessions -- the old synchronous flood
 *               (test baseline), the queue drained by ONE batched flood, and
 *               the queue drained by single-seed floods in Beta's LIFO pop
 *               order. After every World.tick + drain the full id, metadata,
 *               Sky and Block arrays of all nine chunks match the exact
 *               contract below (including four JAR-proven ordered-water
 *               differences from sync), as does the session RNG.
 *   explosion : a TNT-scale blast (power 4) in dirt under open sky: the
 *               pre-drain column is func_339_g's, the settled arrays equal the
 *               synchronous blast byte-for-byte, and the drain time is
 *               measured against the old synchronous flood.
 *   fault     : an injected reservation failure refuses the write before any
 *               mutation (block unchanged, nothing queued), stops the world
 *               (light_fault) and a later blast is refused whole.
 *   lifecycle : pending light pins its chunk and the 8 neighbours; an
 *               unload of a pinned chunk defers (retry next tick, resident,
 *               dirty), a checkpoint save writes but stays dirty; after the
 *               drain both succeed and the chunk is clean. A second world pins nothing in the first.
 *   provision : generation never runs over pending gameplay light: a
 *               populate whose chunks are pinned defers (like a budget
 *               deferral), then runs after the drain, and the final world
 *               equals the synchronous session byte-for-byte.
 *   shutdown  : a persisting teardown drains before saving -- the reloaded
 *               column carries the settled torch light (no lost work).
 *   phases    : the production update (server_local_test_update_once):
 *               a packet-phase torch is still pending in the world prelude
 *               (spawning) and settled by the time the entity pass and tile
 *               entities run (MinecraftServer.doTick:346-348); a torch
 *               written during the entity pass stays pending through the
 *               tile entities and the end of the update, is pending again in
 *               the next prelude, and settles at the next drain.
 *   drain_loss: (Required 2) injected frontier OOM at seeding / mid-flood and
 *               the pop ceiling each stop the drain with all work retained;
 *               a resumed drain settles byte-identical to the sync path.
 *   drain_stop_update: a drain stop in the production update skips the
 *               entity and tile-entity passes and ends the session (terminal).
 *   contain   : (Required 3) slot 1 prelude fault, a nested failure after
 *               partial progress in slot 0's entity pass, and a same-tick
 *               failure after a successful write: each stops the session
 *               (terminal), runs no later consumer and saves nothing.
 *   checkpoint: (Required 1) a live checkpoint writes a settled trial image
 *               and rolls back: the live world stays pending; crash/reload
 *               and failed-checkpoint retry reload the settled image. */

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/region_archive.h"
#include "lighting.h"
#include "worldgen/worldgen.h"
#include "stack.h"
#include "item/inventory.h"
#include "entity/combat.h"
#include "network/inventory_logic.h"
#include "platform/thread.h"
#include "platform/mem2.h" /* G9X-5b: mem2_lwp_allocatable_from_block */
#include "platform/time.h"
#include "util.h"
#include "network/server_furnace.h"
#include "network/server_spawner.h"
#include "network/spawner.h"
#include "item/window_container.h"
#include "block/piston_moving.h"
#include "sha256.h" /* tools/wii_stage_server/source (debt golden) */
#include "block/furnace_logic.h"
#include "block/fluid_block.h"
#include "block/redstone_server.h"
#include "network/server_transfer.h"

#define NS 3
static struct server_local S[NS];
static char s_dir[NS][220];
static int s_fail;

/* clin_drain_discard keeps unsynchronised consumer state (the held call,
 * the SET_BLOCK_PACK replay); the background consumer and a case's
 * synchronous drain (g2_run) take turns through this lock. */
static int s_drain_busy;
static void drain_discard_locked(void) {
	while(__atomic_exchange_n(&s_drain_busy, 1, __ATOMIC_ACQUIRE))
		thread_msleep(1);
	clin_drain_discard();
	__atomic_store_n(&s_drain_busy, 0, __ATOMIC_RELEASE);
}

static void* drain_entry(void* arg) {
	(void)arg;
	while(1) {
		drain_discard_locked();
		thread_msleep(1);
	}
	return NULL;
}

static void check(bool ok, const char* what) {
	printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);
	if(!ok)
		s_fail++;
}

static void open_world(struct server_local* s, char* dir, size_t dir_n,
					   const char* id, bool superflat, int radius) {
	test_fixture_dir(id, dir, dir_n);
	MKDIR(dir);
	char region[260];
	snprintf(region, sizeof region, "%s/region", dir);
	MKDIR(region);
	memset(s, 0, sizeof *s);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(s))
		abort();
	dict_entity_init(s->entities);
	clin_server_publication_stop(false); /* a fresh session */
	string_t level;
	string_init_set_str(level, dir);
	server_world_create(&s->worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						superflat);
	string_clear(level);
	s->worlds[0].session = s;
	s->worlds[0].local = s;
	/* Original-JAR fixture: World.rand is distinct from the provider and
	 * cosmetic owners (World.scheduleBlockUpdate immediate branch). */
	jrand_set_seed(&s->worlds[0].rand, 4242);
	s->scheduled_tick_budget = 128;
	s->rand_src.seed = 0x9E11u;
	s->world_time = 6000;
	server_local_skylight_recompute(s);
	for(int cx = -radius; cx <= radius; cx++)
		for(int cz = -radius; cz <= radius; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s->worlds[0], cx, cz, &sc) && sc);
		}
	/* Fixed-input gameplay twins begin after the initial generation drain:
	 * CFR MinecraftServer.java:346-348. The actual-history gate covers the
	 * separate un-drained generation phase. */
	assert(server_world_light_drain(&s->worlds[0]));
}

static void close_world(struct server_local* s, const char* dir, bool persist) {
	dict_entity_clear(s->entities);
	if(persist)
		server_world_destroy(&s->worlds[0]);
	else
		server_world_discard(&s->worlds[0]);
	test_fixture_dir_cleanup(dir);
}

/* Session-mode switch: the sync baseline is a process-wide test toggle, so
 * every operation on session i runs under its own mode. */
static const bool MODE_SYNC[NS] = {true, false, false};
static const bool MODE_LIFO[NS] = {false, false, true};
static void mode(int i) {
	server_world_test_set_light_sync(MODE_SYNC[i]);
	server_world_test_set_light_lifo(MODE_LIFO[i]);
}

static void tick_drain(int i) {
	mode(i);
	(void)server_local_world_block_ticks_w(&S[i], &S[i].worlds[0], false, 0, 0);
	server_world_light_drain(&S[i].worlds[0]);
}

/* First differing cell between sessions a and b over the 3x3 chunks. */
static long diff_cells_contract(int a, int b, int lo_y, int hi_y,
	int batch, unsigned* water_observations) {
	long n = 0;
	for(int x = -16; x < 32; x++)
		for(int z = -16; z < 32; z++)
			for(int y = lo_y; y < hi_y; y++) {
				struct block_data p = {0}, q = {0};
				bool pa = server_world_get_block(&S[a].worlds[0], x, y, z, &p);
				bool qa = server_world_get_block(&S[b].worlds[0], x, y, z, &q);
				/* Original JAR batch54/55: lower water retains 2 after the
				 * merged region visits y3 before y4. RetroMCP server
				 * MetadataChunkBlock:35-36,65,125-144; World:446-464.
				 * Assert even if equal: missing/moved differences must fail.
				 * Evidence: tmp/jobs/loop/IN-005/amend-jar-batch/result.json
				 * and amend-jar-b55/result.json. No other field is exempt. */
				if(water_observations && a == 0 && (b == 1 || b == 2)
				   && (batch == 54 || batch == 55)
				   && x == -4 && y == 3 && z == 8) {
					assert(pa && qa && p.type == 9 && q.type == 9
						   && p.metadata == 0 && q.metadata == 0
						   && p.sky_light == 9 && q.sky_light == 9
						   && p.torch_light == 0 && q.torch_light == 2);
					(*water_observations)++;
					continue;
				}
				if(pa != qa || p.type != q.type || p.metadata != q.metadata
				   || p.sky_light != q.sky_light
				   || p.torch_light != q.torch_light) {
					if(n == 0)
						printf("    first diff s%d/s%d at (%d,%d,%d): id %d/%d "
							   "meta %d/%d sky %d/%d blk %d/%d\n", a, b, x, y,
							   z, p.type, q.type, p.metadata, q.metadata,
							   p.sky_light, q.sky_light, p.torch_light,
							   q.torch_light);
					n++;
				}
			}
	return n;
}

static long diff_cells(int a, int b, int lo_y, int hi_y) {
	return diff_cells_contract(a, b, lo_y, hi_y, -1, NULL);
}

static uint32_t lcg_state;
static uint32_t lcg(void) {
	lcg_state = lcg_state * 1664525u + 1013904223u;
	return lcg_state >> 8;
}

/* Identical edits for the full three-arm comparison and the focused replay. */
static void equiv_edits(int arm, int batch, int h0) {
	static const uint8_t palette[] = {
		BLOCK_AIR, BLOCK_AIR, BLOCK_STONE, BLOCK_STONE, BLOCK_GLASS,
		BLOCK_TORCH, BLOCK_GLOWSTONE, BLOCK_LEAVES, BLOCK_ICE, BLOCK_WATER_STILL,
	};
	mode(arm);
	lcg_state = 0xC0FFEEu + (uint32_t)batch * 7919u;
	const int k = 1 + (int)(lcg_state % 23u);
	for(int e = 0; e < k; e++) {
		int x = -12 + (int)(lcg() % 40u);
		int z = -12 + (int)(lcg() % 40u);
		int y = h0 - 3 + (int)(lcg() % 18u);
		uint8_t t = palette[lcg() % sizeof palette];
		(void)server_world_set_block(&S[arm].worlds[0], x, y, z,
			(struct block_data) {.type = t});
	}
}

static unsigned water_requests, water_merges, water_visits, water_upper_stores;
static unsigned water_request_mask;
static uint64_t water_quiet_request;
static bool water_in_visit;

static bool water_api(const struct server_world_history_event* e) {
	if(e->world != &S[1].worlds[0]) return true;
	if(!strcmp(e->operation, "setter-id-meta") && !strcmp(e->edge, "begin"))
		water_quiet_request = e->request;
	if(!strcmp(e->operation, "setter-id-meta") && !strcmp(e->edge, "end"))
		water_quiet_request = 0;
	if(!strcmp(e->operation, "store") && e->chunk
	   == dict_server_chunks_get(S[1].worlds[0].chunks, S_CHUNK_ID(-1, 0))
	   && e->y == COLUMN_TORCH && e->x == ((12 << 11) | (8 << 7) | 4)) {
		assert(water_in_visit && e->before_id == 5 && e->after_id == 3);
		water_upper_stores++;
	}
	return true;
}

static bool water_regions(const struct lighting_history_event* e) {
	if(e->object != &S[1].worlds[0] || e->channel != COLUMN_TORCH)
		return true;
	if(!strcmp(e->operation, "region-request") && e->x == -4
	   && e->z == 8 && (e->y == 3 || e->y == 4)) {
		assert(water_quiet_request && e->x2 == -4 && e->y2 == e->y
			   && e->z2 == 8);
		water_requests++;
		water_request_mask |= 1u << (e->y - 3);
	}
	if(!strcmp(e->operation, "region-merge") && e->x == -4
	   && e->y == 3 && e->z == 8 && e->x2 == -3
	   && e->y2 == 4 && e->z2 == 8)
		water_merges++;
	if(!strcmp(e->operation, "region-pop")) {
		water_in_visit = e->x == -4 && e->y == 3 && e->z == 8
			&& e->x2 == -3 && e->y2 == 4 && e->z2 == 8;
		if(water_in_visit) {
			/* History hooks read borrowed storage directly; no engine API
			 * re-entry while a region pop is being observed. */
			const struct server_chunk* sc = dict_server_chunks_get(
				S[1].worlds[0].chunks, S_CHUNK_ID(-1, 0));
			assert(sc);
			assert(nibble_read(sc->lighting_torch, (12 << 11) | (8 << 7) | 3) == 2
				   && nibble_read(sc->lighting_torch, (12 << 11) | (8 << 7) | 4) == 5);
			water_visits++;
		}
	}
	return true;
}

static void case_ordered_water(void) {
	/* Replay actual production input/order, not a miniature light solver.
	 * JAR stepped pop15 visits [lower,upper] [2,5] -> [2,3]. Quiet
	 * Chunk:215-250 requests are essential even for equal-opacity swaps. */
	open_world(&S[1], s_dir[1], sizeof s_dir[1],
		"test_light_queue_ordered_water", true, 1);
	uint8_t h0;
	assert(server_world_get_height(&S[1].worlds[0], 8, 8, &h0));
	water_requests = water_merges = water_visits = water_upper_stores = 0;
	water_request_mask = 0;
	water_quiet_request = 0;
	water_in_visit = false;
	for(int b = 0; b <= 54; b++) {
		equiv_edits(1, b, h0);
		if(b == 54) {
			server_world_test_history_hook = water_api;
			lighting_test_history_hook = water_regions;
		}
		tick_drain(1);
	}
	server_world_test_history_hook = NULL;
	lighting_test_history_hook = NULL;
	assert(!server_world_test_history_failed() && !lighting_test_history_failed());
	struct server_world* w = &S[1].worlds[0];
	struct block_data lower, upper;
	assert(server_world_get_block(w, -4, 3, 8, &lower));
	assert(server_world_get_block(w, -4, 4, 8, &upper));
	printf("  ordered water: requests=%u merges=%u visits=%u upper stores=%u\n",
		water_requests, water_merges, water_visits, water_upper_stores);
	assert(water_request_mask == 3 && water_merges > 0 && water_visits == 1
		   && water_upper_stores == 1);
	assert(lower.type == 9 && lower.metadata == 0 && lower.sky_light == 9
		   && lower.torch_light == 2 && upper.torch_light == 3);
	assert(w->generation_light_n == 0 && w->light_seed_n == 0 && !w->light_fault);
	/* Separate extra-work control, never inserted into the 60-batch run.
	 * Original-JAR amend-jar-water-v2/result.json independently gives 0. */
	assert(server_world_test_generation_light_schedule(w, COLUMN_TORCH,
		-4, 3, 8, -4, 3, 8));
	assert(server_world_light_drain(w));
	assert(server_world_get_block(w, -4, 3, 8, &lower));
	assert(lower.torch_light == 0 && w->generation_light_n == 0
		   && w->light_seed_n == 0 && !w->light_fault);
	puts("  PASS: Beta ordered-region quiet requests/merged visit retain lower2; "
		 "explicit later recomputation gives0");
	close_world(&S[1], s_dir[1], false);
}

static void hs_run(bool healthy);

static void case_equiv(void) {
	hs_run(true);
	if(getenv("CW_HALT_ONLY")) return;
	const char* ids[NS] = {"test_light_queue_equiv_sync",
						   "test_light_queue_equiv_batch",
						   "test_light_queue_equiv_lifo"};
	for(int i = 0; i < NS; i++)
		open_world(&S[i], s_dir[i], sizeof s_dir[i], ids[i], true, 1);
	uint8_t hh = 0;
	assert(server_world_get_height(&S[0].worlds[0], 8, 8, &hh));
	const int h0 = hh;
	const int batches = 60;
	long total_diff = 0;
	unsigned water_observations = 0;
	server_world_test_light_stats_reset();
	for(int b = 0; b < batches; b++) {
		for(int i = 0; i < NS; i++) equiv_edits(i, b, h0);
		/* Before the drain the queued sessions show Beta pending values;
		 * they must agree with each other (same immediate column writes). */
		total_diff += diff_cells(1, 2, 0, 128);
		for(int i = 0; i < NS; i++)
			tick_drain(i);
		long d01 = diff_cells_contract(0, 1, 0, 128, b, &water_observations);
		long d02 = diff_cells_contract(0, 2, 0, 128, b, &water_observations);
		total_diff += d01 + d02;
		if(d01 || d02)
			printf("  batch %d: %ld / %ld unexpected differing cells\n", b, d01,
				   d02);
		total_diff += S[0].rand_src.seed != S[1].rand_src.seed
			|| S[0].rand_src.seed != S[2].rand_src.seed;
	}
	struct server_world_light_stats st;
	server_world_light_stats(&st);
	printf("  queue: %u drains, %llu seeds, peak %u seeds / %u undo columns "
		   "(%u B), drain max %u us\n", st.drains,
		   (unsigned long long)st.seeds_total, st.seeds_peak, st.undo_peak,
		   (unsigned)(st.seeds_peak * sizeof(struct world_modification_entry)
					  + st.undo_peak * sizeof(struct server_light_undo)),
		   st.drain_us_max);
	assert(water_observations == 4);
	printf("  Original-JAR-backed water observations=%u (required 4)\n",
		water_observations);
	check(total_diff == 0,
		  "60 batches: queued arms equal; sync differs only at the four literal "
		  "Beta water observations; all other ID/meta/Sky/Block cells + RNG equal");
	for(int i = 0; i < NS; i++)
		close_world(&S[i], s_dir[i], false);
	case_ordered_water();
}

static void case_explosion(void) {
	open_world(&S[0], s_dir[0], sizeof s_dir[0], "test_light_queue_expl_sync",
			   true, 1);
	open_world(&S[1], s_dir[1], sizeof s_dir[1], "test_light_queue_expl_queue",
			   true, 1);
	/* F4-RNG (f049fa3d8): rays and drops use runtime-seeded World.rand.
	 * Supply the same blast input to the synchronous and queued arms. */
	for(int i = 0; i < 2; i++)
		jrand_set_seed(&S[i].worlds[0].rand, 4242);
	uint8_t hh = 0;
	assert(server_world_get_height(&S[0].worlds[0], 8, 8, &hh));
	const int h0 = hh;
	for(int i = 0; i < 2; i++) {
		mode(i);
		for(int x = 0; x < 16; x++)
			for(int z = 0; z < 16; z++)
				for(int y = h0; y < h0 + 6; y++)
					(void)server_world_set_block(&S[i].worlds[0], x, y, z,
						(struct block_data) {.type = BLOCK_DIRT});
		tick_drain(i);
	}
	assert(diff_cells(0, 1, 0, 128) == 0);
	vec3 c = {8.5f, (float)h0 + 5.5f, 8.5f};
	mode(0);
	ptime_t t0 = time_get();
	server_world_explode(&S[0].worlds[0], c, 4.0f);
	float sync_us = time_diff_s(t0, time_get()) * 1.0e6f;
	mode(1);
	server_world_test_light_stats_reset();
	t0 = time_get();
	server_world_explode(&S[1].worlds[0], c, 4.0f);
	float queue_us = time_diff_s(t0, time_get()) * 1.0e6f;
	struct server_world_light_stats st;
	server_world_light_stats(&st);
	int air = 0;
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++)
			for(int y = h0; y < h0 + 6; y++) {
				struct block_data q = {0};
				air += server_world_get_block(&S[1].worlds[0], x, y, z, &q)
					&& q.type == BLOCK_AIR;
			}
	printf("  blast removed %d dirt cells\n", air);
	check(air > 20, "the blast removed a crater");
	/* the blast centre column: open sky now reaches the crater floor at once
	 * (func_339_g lowering = 15), the crater rim light waits for the drain */
	uint8_t nh = 0;
	assert(server_world_get_height(&S[1].worlds[0], 8, 8, &nh));
	struct block_data at = {0};
	assert(server_world_get_block(&S[1].worlds[0], 8, nh, 8, &at));
	printf("  blast: %u seeds queued, centre column height %d sky %d; sync "
		   "blast %.0f us vs queued blast %.0f us\n", st.seeds_peak, nh,
		   at.sky_light, sync_us, queue_us);
	check(server_world_light_pending(&S[1].worlds[0]) && at.sky_light == 15,
		  "pre-drain: the crater column is func_339_g's 15, propagation queued");
	mode(1);
	t0 = time_get();
	server_world_light_drain(&S[1].worlds[0]);
	float drain_us = time_diff_s(t0, time_get()) * 1.0e6f;
	printf("  drain of the blast: %.0f us\n", drain_us);
	long d = diff_cells(0, 1, 0, 128);
	check(S[0].worlds[0].rand.seed == S[1].worlds[0].rand.seed,
		  "both blasts consumed the same World.rand draws");
	check(d == 0, "post-drain: the blast settles byte-identical to the "
				  "synchronous blast (ids, metadata, Sky, Block)");
	close_world(&S[0], s_dir[0], false);
	close_world(&S[1], s_dir[1], false);
}

static void case_fault(void) {
	open_world(&S[1], s_dir[1], sizeof s_dir[1], "test_light_queue_fault", true,
			   1);
	mode(1);
	struct server_world* w = &S[1].worlds[0];
	uint8_t hh = 0;
	assert(server_world_get_height(w, 8, 8, &hh));
	const int h0 = hh;
	server_world_test_light_fail_after(0);
	bool wrote = server_world_set_block(w, 4, h0, 4,
										(struct block_data) {.type = BLOCK_TORCH});
	struct block_data b = {0};
	assert(server_world_get_block(w, 4, h0, 4, &b));
	check(!wrote && b.type == BLOCK_AIR && !server_world_light_pending(w)
			  && w->light_fault,
		  "reservation failure: write refused before any mutation, nothing "
		  "queued, world stopped");
	server_world_test_light_fail_after(-1);
	wrote = server_world_set_block(w, 4, h0, 4,
								   (struct block_data) {.type = BLOCK_TORCH});
	vec3 c = {8.5f, (float)h0 - 1.5f, 8.5f};
	server_world_explode(w, c, 4.0f);
	assert(server_world_get_block(w, 8, h0 - 2, 8, &b));
	check(!wrote && b.type != BLOCK_AIR && !server_world_light_pending(w),
		  "a stopped world refuses later writes and whole blasts (no partial "
		  "state)");
	close_world(&S[1], s_dir[1], false);
}

static void case_lifecycle(void) {
	open_world(&S[1], s_dir[1], sizeof s_dir[1], "test_light_queue_life", true,
			   2);
	mode(1);
	struct server_world* w = &S[1].worlds[0];
	uint8_t hh = 0;
	assert(server_world_get_height(w, 8, 8, &hh));
	assert(server_world_set_block(w, 4, hh, 4,
								  (struct block_data) {.type = BLOCK_TORCH}));
	check(server_world_light_pinned(w, 0, 0) && server_world_light_pinned(w, 1, 1)
			  && server_world_light_pinned(w, -1, 0)
			  && !server_world_light_pinned(w, 2, 0)
			  && !server_world_light_pinned(w, 0, -2),
		  "pending light pins its chunk and the 8 neighbours only");
	bool saved = server_world_save_chunk(w, true, 1, 0);
	struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(1, 0));
	check(!saved && w->evict_retire_deferred && sc && sc->modified,
		  "unload of a pinned neighbour defers (retry next tick), stays "
		  "resident and dirty");
	sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
	saved = server_world_save_chunk(w, false, 0, 0);
	check(!saved && sc && sc->modified,
		  "a non-evicting save of the pinned chunk writes nothing (no "
		  "incoherent image on disk) and stays dirty");
	struct server_world_light_stats st;
	server_world_light_stats(&st);
	server_world_light_drain(w);
	saved = server_world_save_chunk(w, false, 0, 0);
	check(saved && !server_world_light_pinned(w, 0, 0) && !sc->modified
			  && st.save_deferred >= 2,
		  "after the drain the save writes the settled column");
	saved = server_world_save_chunk(w, true, 1, 0);
	check(saved && !dict_server_chunks_get(w->chunks, S_CHUNK_ID(1, 0)),
		  "after the drain the unload proceeds");
	close_world(&S[1], s_dir[1], false);
}

static void case_provision(void) {
	/* Generated (non-superflat) worlds: chunk (2,0) is populated by the
	 * quadrants (1..2, -1..0), whose chunks a seed in chunk (0,0) pins. */
	const char* ids[2] = {"test_light_queue_prov_sync",
						  "test_light_queue_prov_queue"};
	for(int i = 0; i < 2; i++)
		open_world(&S[i], s_dir[i], sizeof s_dir[i], ids[i], false, 1);
	server_world_test_set_gen_elapsed_mode(
		SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_chunk* sc = NULL;
	for(int i = 0; i < 2; i++) {
		mode(i);
		uint8_t hh = 0;
		assert(server_world_get_height(&S[i].worlds[0], 12, 4, &hh));
		assert(server_world_set_block(&S[i].worlds[0], 12, hh, 4,
			(struct block_data) {.type = BLOCK_GLOWSTONE}));
		assert(server_world_set_block(&S[i].worlds[0], 13, hh + 3, 4,
			(struct block_data) {.type = BLOCK_STONE}));
	}
	server_world_test_light_stats_reset();
	mode(1);
	bool early = server_world_ensure_populated(&S[1].worlds[0], &S[1], 2, 0, &sc);
	struct server_world_light_stats st;
	server_world_light_stats(&st);
	check(!early && st.provision_deferred == 1,
		  "provision over pinned chunks defers instead of reading pending "
		  "light (no early drain)");
	server_world_light_drain(&S[1].worlds[0]);
	bool done[2] = {false, false};
	for(int i = 0; i < 2; i++) {
		mode(i);
		for(int n = 0; n < 64 && !done[i]; n++)
			done[i] = server_world_ensure_populated(&S[i].worlds[0], &S[i], 2,
													0, &sc);
	}
	long d = 0;
	for(int x = -16; x < 48; x++)
		for(int z = -16; z < 32; z++)
			for(int y = 0; y < 128; y++) {
				struct block_data p = {0}, q = {0};
				bool pa = server_world_get_block(&S[0].worlds[0], x, y, z, &p);
				bool qa = server_world_get_block(&S[1].worlds[0], x, y, z, &q);
				d += pa != qa || p.type != q.type || p.metadata != q.metadata
					|| p.sky_light != q.sky_light
					|| p.torch_light != q.torch_light;
			}
	printf("  populated sync %d queue %d; %ld differing cells\n", done[0],
		   done[1], d);
	check(done[0] && done[1] && d == 0,
		  "after the drain the deferred populate yields the synchronous world "
		  "byte-for-byte (generation output unchanged)");
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_NORMAL);
	for(int i = 0; i < 2; i++)
		close_world(&S[i], s_dir[i], false);
}

static void case_shutdown(void) {
	open_world(&S[1], s_dir[1], sizeof s_dir[1], "test_light_queue_shutdown",
			   true, 1);
	mode(1);
	struct server_world* w = &S[1].worlds[0];
	uint8_t hh = 0;
	assert(server_world_get_height(w, 8, 8, &hh));
	const int h0 = hh;
	assert(server_world_set_block(w, 4, h0, 4,
								  (struct block_data) {.type = BLOCK_TORCH}));
	struct block_data b = {0};
	assert(server_world_get_block(w, 4, h0, 4, &b) && b.torch_light == 0);
	server_world_destroy(w); /* persisting teardown: drain, then save */
	string_t level;
	string_init_set_str(level, s_dir[1]);
	server_world_create(w, level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	w->session = &S[1];
	w->local = &S[1];
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(w, 0, 0, &sc) && sc);
	assert(server_world_get_block(w, 4, h0, 4, &b));
	printf("  reloaded torch cell: id %d block light %d (settled 14)\n", b.type,
		   b.torch_light);
	check(b.type == BLOCK_TORCH && b.torch_light == 14,
		  "shutdown drains before saving: no pending light is lost");
	close_world(&S[1], s_dir[1], false);
}

/* phase probe state */
static int ph_seen[3];
static int ph_light[3][2];
static int ph_h0;
static int ph_update;
static void phase_probe(struct server_local* s, struct server_world* w,
						int phase) {
	if(w != &s->worlds[0] || phase < 0 || phase > 2 || ph_update > 1)
		return;
	struct block_data a = {0};
	(void)server_world_get_block(w, 4, ph_h0, 4, &a);
	ph_light[phase][ph_update] = a.torch_light;
	ph_seen[phase]++;
	if(phase == 1 && ph_update == 0)
		/* an entity-phase write (a falling block landing, a lightning fire) */
		(void)server_world_set_block(w, 12, ph_h0, 12,
									 (struct block_data) {.type = BLOCK_TORCH});
}

static void case_phases(void) {
	struct server_local* s = &S[1];
	open_world(s, s_dir[1], sizeof s_dir[1], "test_light_queue_phases", true, 1);
	mode(1);
	assert(inventory_create(&s->player.inventory, &inventory_logic_player, s,
							&s->player, INVENTORY_SIZE, 0, 0, 0));
	s->player.active_inventory = &s->player.inventory;
	uint8_t hh = 0;
	assert(server_world_get_height(&s->worlds[0], 8, 8, &hh));
	ph_h0 = hh;
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.x = 8.5;
	s->player.y = ph_h0 + (double)PLAYER_EYE_HEIGHT;
	s->player.z = 8.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->render_demo_mob = -1;
	server_local_test_update_once(s); /* settle the fixture */
	server_world_light_drain(&s->worlds[0]);
	/* the packet-phase write (Beta: handleNetworkListenThread of the
	 * previous doTick) */
	assert(server_world_set_block(&s->worlds[0], 4, ph_h0, 4,
								  (struct block_data) {.type = BLOCK_TORCH}));
	memset(ph_seen, 0, sizeof ph_seen);
	server_local_test_phase_hook = phase_probe;
	ph_update = 0;
	server_local_test_update_once(s);
	struct block_data e = {0};
	assert(server_world_get_block(&s->worlds[0], 12, ph_h0, 12, &e));
	const int entity_write_after = e.torch_light;
	const bool entity_pending = server_world_light_pinned(&s->worlds[0], 0, 0);
	ph_update = 1;
	/* the entity-phase torch must be pending in the next prelude */
	int next_prelude = -1;
	{
		struct block_data q = {0};
		(void)server_world_get_block(&s->worlds[0], 12, ph_h0, 12, &q);
		next_prelude = q.torch_light;
	}
	server_local_test_update_once(s);
	assert(server_world_get_block(&s->worlds[0], 12, ph_h0, 12, &e));
	server_local_test_phase_hook = NULL;
	printf("  phases seen %d/%d/%d; packet torch: prelude %d, entities %d, "
		   "tile entities %d (Beta 0, 14, 14)\n", ph_seen[0], ph_seen[1],
		   ph_seen[2], ph_light[0][0], ph_light[1][0], ph_light[2][0]);
	printf("  entity-phase torch: end of update %d (pending %d), next "
		   "prelude %d, after the next drain %d (Beta 0, 1, 0, 14)\n",
		   entity_write_after, entity_pending, next_prelude, e.torch_light);
	check(ph_seen[0] >= 1 && ph_seen[1] >= 1 && ph_seen[2] >= 1
			  && ph_light[0][0] == 0 && ph_light[1][0] == 14
			  && ph_light[2][0] == 14,
		  "a packet write is pending through spawning and settled before the "
		  "entity and tile-entity passes");
	check(entity_write_after == 0 && entity_pending && next_prelude == 0
			  && e.torch_light == 14,
		  "an entity-phase write waits for the next prescribed drain (no "
		  "extra early drain)");
	inventory_destroy(&s->player.inventory, &s->player);
	close_world(s, s_dir[1], false);
}


/* ---- G9 R6 v2 (ASTRA-G9-REVIEW-2 item 5): independent allocation samples.
 * The heartbeat accounting is checked against the capacities read straight
 * off the world (seed/undo/pin arrays) and the frontier read by the drain
 * hook after every flood, completed or stopped. */
static size_t sm_seed, sm_undo, sm_pin, sm_front;
static void sm_reset(void) {
	sm_seed = sm_undo = sm_pin = sm_front = 0;
}
static void sm_sample(const struct server_world* w) {
	const size_t a = w->light_seed_cap * sizeof *w->light_seeds;
	const size_t b = w->light_undo_cap * sizeof *w->light_undo;
	const size_t c = w->light_pin_cap * sizeof *w->light_pins;
	if(a > sm_seed)
		sm_seed = a;
	if(b > sm_undo)
		sm_undo = b;
	if(c > sm_pin)
		sm_pin = c;
}
static void sm_drain_hook(struct server_world* w) {
	const size_t f = lighting_drain_queue_bytes(&w->light_frontier);
	if(f > sm_front)
		sm_front = f;
	sm_sample(w);
}

/* ---- drain_loss (ASTRA-G9-REVIEW Required 2) ----
 * The drain is lossless: an injected frontier-allocation failure (at seeding
 * and mid-flood) and the pop ceiling each STOP the drain with every piece of
 * work retained (seeds, pins, undo, frontier) and the world stopped; nothing
 * pinned can be saved or unloaded. Lifting the stop and resuming the retained
 * frontier settles byte-identical to the synchronous baseline, which proves
 * no cell was dropped. */
static void loss_edits(int i, int h0) {
	mode(i);
	struct server_world* w = &S[i].worlds[0];
	for(int x = 2; x < 14; x += 3)
		(void)server_world_set_block(w, x, h0, 5,
									 (struct block_data) {.type = BLOCK_TORCH});
	for(int x = -4; x < 20; x++)
		(void)server_world_set_block(w, x, h0 + 4, 9,
									 (struct block_data) {.type = BLOCK_STONE});
	(void)server_world_set_block(w, 15, h0 + 1, 15,
								 (struct block_data) {.type = BLOCK_GLOWSTONE});
}

static long snapshot_diff(int i, const uint8_t* snap, bool take) {
	long n = 0;
	size_t k = 0;
	for(int x = -16; x < 32; x++)
		for(int z = -16; z < 32; z++)
			for(int y = 0; y < 128; y++, k++) {
				struct block_data b = {0};
				(void)server_world_get_block(&S[i].worlds[0], x, y, z, &b);
				uint8_t v = (uint8_t)((b.torch_light << 4) | b.sky_light);
				if(take)
					((uint8_t*)snap)[k] = v;
				else
					n += snap[k] != v;
			}
	return n;
}

static void loss_run(const char* label, int kind) {
	/* kind 0: seeding alloc fails; 1: mid-flood alloc fails; 2: ceiling */
	char id0[96], id1[96];
	snprintf(id0, sizeof id0, "test_light_queue_loss_sync_%d", kind);
	snprintf(id1, sizeof id1, "test_light_queue_loss_queue_%d", kind);
	open_world(&S[0], s_dir[0], sizeof s_dir[0], id0, true, 1);
	open_world(&S[1], s_dir[1], sizeof s_dir[1], id1, true, 1);
	uint8_t hh = 0;
	assert(server_world_get_height(&S[0].worlds[0], 8, 8, &hh));
	const int h0 = hh;
	loss_edits(0, h0);
	loss_edits(1, h0);
	mode(1);
	struct server_world* w = &S[1].worlds[0];
	const size_t seeds = w->light_seed_n;
	static uint8_t snap[48 * 48 * 128];
	(void)snapshot_diff(1, snap, true);
	server_world_test_light_stats_reset();
	sm_reset();
	server_world_test_light_drain_hook = sm_drain_hook;
	if(kind == 0)
		lighting_test_drain_fail_reserve_after(0);
	else if(kind == 1)
		lighting_test_drain_fail_reserve_after(2);
	else
		server_world_test_light_drain_ceiling(40);
	bool done = server_world_light_drain(w);
	lighting_test_drain_fail_reserve_after(-1);
	server_world_test_light_drain_ceiling(0);
	server_world_test_light_drain_hook = NULL;
	struct server_world_light_stats st;
	server_world_light_stats(&st);
	/* G9 R6 v2: the STOPPED flood is measured -- its scratch is in the
	 * frontier peak (exactly the hook's own sample), its cells/us in the
	 * drain maxima, its cause in the counters */
	{
		struct server_world_light_hb hb;
		server_world_light_hb(&hb);
		const size_t at_stop = lighting_drain_queue_bytes(&w->light_frontier);
		printf("  %s: heartbeat front %u B (sampled %zu, at the stop %zu), "
			   "drain cells %u, faults %u oom %u ceil %u\n", label,
			   hb.front_b[0], sm_front, at_stop, hb.drain_pops[0], hb.faults,
			   hb.oom, hb.ceil);
		check(hb.front_b[0] == sm_front && hb.front_b[0] >= at_stop
				  && (kind == 0 || (at_stop > 0 && hb.drain_pops[0] > 0))
				  && hb.faults == 1
				  && (kind == 2 ? hb.ceil == 1 && hb.oom == 0 :
								  hb.oom == 1 && hb.ceil == 0),
			  "a stopped drain is measured: frontier peak, cells and the "
			  "cause counter");
	}
	const uint8_t want = kind == 2 ? SERVER_LIGHT_FAULT_DRAIN_CEILING :
									 SERVER_LIGHT_FAULT_DRAIN_OOM;
	const size_t frontier = stack_size(&w->light_frontier);
	printf("  %s: drain=%d reason=%u seeds %zu/%zu frontier %zu oom %u "
		   "ceiling %u\n", label, done, w->light_fault_reason,
		   w->light_seed_n, seeds, frontier, st.drain_oom, st.drain_ceiling);
	bool kept = !done && w->light_fault && w->light_fault_reason == want
		&& w->light_seed_n == seeds && server_world_light_pinned(w, 0, 0)
		&& (kind == 0 || frontier > 0)
		&& (kind == 2 ? st.drain_ceiling == 1 : st.drain_oom == 1);
	check(kept, "stop: completion reported false, world stopped with the "
				"cause, seeds/pins/frontier all retained");
	if(kind == 0)
		check(snapshot_diff(1, snap, false) == 0,
			  "a seeding failure changed nothing (field still pre-drain)");
	/* retained work blocks every save and unload of the pinned columns */
	struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
	const bool modified_before = sc && sc->modified;
	bool saved = server_world_save_chunk(w, false, 0, 0);
	bool unloaded = server_world_save_chunk(w, true, 1, 0);
	check(!unloaded && dict_server_chunks_get(w->chunks, S_CHUNK_ID(1, 0))
			  && (!saved || !modified_before) && sc && sc->modified == modified_before,
		  "stopped world: pinned chunks are neither written nor unloaded");
	check(!server_world_light_drain(w) && w->light_seed_n == seeds,
		  "a later drain of the stopped world does not half-apply the work");
	/* resume the retained frontier: settles exactly like the sync baseline */
	server_world_test_light_clear_fault(w);
	done = server_world_light_drain(w);
	tick_drain(0);
	long d = diff_cells(0, 1, 0, 128);
	check(done && !server_world_light_pending(w) && d == 0,
		  "resuming the retained work settles byte-identical to the "
		  "synchronous path (no cell lost)");
	close_world(&S[0], s_dir[0], false);
	close_world(&S[1], s_dir[1], false);
}

static void case_drain_loss(void) {
	loss_run("seed alloc", 0);
	loss_run("mid-flood alloc", 1);
	loss_run("pop ceiling", 2);
}

/* drain_stop_update: a drain stop inside the production update ends the
 * session before any entity / tile entity pass of the tick (visible
 * CRPC_LOAD_FAILED terminal) and nothing of the world is published after. */
static int stop_phase_seen[3];
static void stop_probe(struct server_local* s, struct server_world* w,
					   int phase) {
	(void)s;
	(void)w;
	if(phase >= 0 && phase <= 2)
		stop_phase_seen[phase]++;
}

static void case_drain_stop_update(void) {
	struct server_local* s = &S[1];
	open_world(s, s_dir[1], sizeof s_dir[1], "test_light_queue_stopupd", true,
			   1);
	mode(1);
	assert(inventory_create(&s->player.inventory, &inventory_logic_player, s,
							&s->player, INVENTORY_SIZE, 0, 0, 0));
	s->player.active_inventory = &s->player.inventory;
	uint8_t hh = 0;
	assert(server_world_get_height(&s->worlds[0], 8, 8, &hh));
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.x = 8.5;
	s->player.y = hh + (double)PLAYER_EYE_HEIGHT;
	s->player.z = 8.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->render_demo_mob = -1;
	server_local_test_update_once(s);
	assert(server_world_light_drain(&s->worlds[0]));
	assert(server_world_set_block(&s->worlds[0], 4, hh, 4,
								  (struct block_data) {.type = BLOCK_TORCH}));
	memset(stop_phase_seen, 0, sizeof stop_phase_seen);
	server_local_test_phase_hook = stop_probe;
	lighting_test_drain_fail_reserve_after(0);
	server_local_test_update_once(s);
	lighting_test_drain_fail_reserve_after(-1);
	const int after_first = stop_phase_seen[1] + stop_phase_seen[2];
	server_local_test_update_once(s); /* a later update stays stopped */
	server_local_test_phase_hook = NULL;
	printf("  phases prelude %d entities %d tile %d; terminal %d\n",
		   stop_phase_seen[0], stop_phase_seen[1], stop_phase_seen[2],
		   s->load_failure_terminal);
	check(stop_phase_seen[0] == 1 && after_first == 0
			  && stop_phase_seen[1] + stop_phase_seen[2] == 0
			  && s->load_failure_terminal && s->worlds[0].light_fault
			  && server_world_light_pending(&s->worlds[0]),
		  "drain stop: no entity/tile-entity pass, terminal outcome, work "
		  "retained; later updates stay stopped");
	inventory_destroy(&s->player.inventory, &s->player);
	close_world(s, s_dir[1], false);
}


/* ---- contain (ASTRA-G9-REVIEW Required 3) ----
 * Fault containment in the production update, for every world:
 *   slot1   : a reservation failure in the Nether slot 1 prelude stops the
 *             session; slot 1 runs no entity / tile entity pass, the next
 *             update runs no phase of any world, nothing is saved.
 *   nested  : in slot 0's entity pass an outer write succeeds and its
 *             nested write (the torch dropping off the removed block)
 *             fails: partial progress is kept pending, the entity pass and
 *             tile entities stop, the terminal is published (slot 0).
 *   sametick: in slot 0's prelude one write succeeds and the next fails:
 *             no drain half-applies, no entity / tile entity pass. */
static int ct_seen[2][3];
static int ct_mode;
static int ct_h0;
static bool ct_done;
static bool ct_wrote[2];
static void ct_probe(struct server_local* s, struct server_world* w,
					 int phase) {
	const int wi = w == &s->worlds[0] ? 0 : 1;
	if(phase < 0 || phase > 2)
		return;
	ct_seen[wi][phase]++;
	if(ct_done)
		return;
	if(ct_mode == 0 && wi == 1 && phase == 0) {
		ct_done = true;
		server_world_test_light_fail_after(0);
		ct_wrote[0] = server_world_set_block(
			w, 4, 40, 4, (struct block_data) {.type = BLOCK_GLOWSTONE});
		server_world_test_light_fail_after(-1);
	} else if(ct_mode == 1 && wi == 0 && phase == 1) {
		ct_done = true;
		server_world_test_light_fail_after(1); /* outer ok, nested fails */
		ct_wrote[0] = server_world_set_block(
			w, 6, ct_h0, 6, (struct block_data) {.type = BLOCK_AIR});
		server_world_test_light_fail_after(-1);
	} else if(ct_mode == 2 && wi == 0 && phase == 0) {
		ct_done = true;
		ct_wrote[0] = server_world_set_block(
			w, 3, ct_h0, 3, (struct block_data) {.type = BLOCK_TORCH});
		server_world_test_light_fail_after(0);
		ct_wrote[1] = server_world_set_block(
			w, 11, ct_h0, 11, (struct block_data) {.type = BLOCK_TORCH});
		server_world_test_light_fail_after(-1);
	}
}

static void ct_run(int mode_id) {
	struct server_local* s = &S[1];
	char id[64];
	snprintf(id, sizeof id, "test_light_queue_contain_%d", mode_id);
	open_world(s, s_dir[1], sizeof s_dir[1], id, true, 1);
	mode(1);
	/* slot 1: a live Nether world of the same session */
	char ndir[260];
	snprintf(ndir, sizeof ndir, "%s_nether", s_dir[1]);
	MKDIR(ndir);
	char nreg[300];
	snprintf(nreg, sizeof nreg, "%s/region", ndir);
	MKDIR(nreg);
	string_t level;
	string_init_set_str(level, ndir);
	server_world_create(&s->worlds[1], level, WORLD_DIM_NETHER, 4242, true);
	string_clear(level);
	s->worlds[1].session = s;
	s->worlds[1].local = s;
	s->worlds[1].world_index = 1;
	for(int cx = -1; cx <= 1; cx++)
		for(int cz = -1; cz <= 1; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s->worlds[1], cx, cz, &sc) && sc);
		}
	s->world_slot_state[1] = SERVER_WORLD_SLOT_LIVE;
	assert(inventory_create(&s->player.inventory, &inventory_logic_player, s,
							&s->player, INVENTORY_SIZE, 0, 0, 0));
	s->player.active_inventory = &s->player.inventory;
	uint8_t hh = 0;
	assert(server_world_get_height(&s->worlds[0], 8, 8, &hh));
	ct_h0 = hh;
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.x = 8.5;
	s->player.y = ct_h0 + (double)PLAYER_EYE_HEIGHT;
	s->player.z = 8.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->render_demo_mob = -1;
	if(mode_id == 1) {
		assert(server_world_set_block(&s->worlds[0], 6, ct_h0, 6,
			(struct block_data) {.type = BLOCK_STONE}));
		assert(server_world_set_block(&s->worlds[0], 6, ct_h0 + 1, 6,
			(struct block_data) {.type = BLOCK_TORCH}));
	}
	server_local_test_update_once(s);
	assert(server_world_light_drain(&s->worlds[0]));
	assert(server_world_light_drain(&s->worlds[1]));
	memset(ct_seen, 0, sizeof ct_seen);
	ct_mode = mode_id;
	ct_done = false;
	ct_wrote[0] = ct_wrote[1] = false;
	server_local_test_phase_hook = ct_probe;
	server_local_test_update_once(s);
	int first[2][3];
	memcpy(first, ct_seen, sizeof first);
	memset(ct_seen, 0, sizeof ct_seen);
	server_local_test_update_once(s);
	server_local_test_phase_hook = NULL;
	int later = 0;
	for(int a = 0; a < 2; a++)
		for(int b = 0; b < 3; b++)
			later += ct_seen[a][b];
	struct server_world* fw = &s->worlds[mode_id == 0 ? 1 : 0];
	/* every save of the stopped session is refused, dirty kept */
	struct server_chunk* c0 = dict_server_chunks_get(s->worlds[0].chunks,
													 S_CHUNK_ID(-1, -1));
	assert(c0);
	c0->modified = true;
	bool saved0 = server_world_save_chunk(&s->worlds[0], false, -1, -1);
	printf("  mode %d: w0 phases %d/%d/%d, w1 %d/%d/%d; later %d; terminal "
		   "%d slot %u; wrote %d/%d; pending %zu; save %d\n", mode_id,
		   first[0][0], first[0][1], first[0][2], first[1][0], first[1][1],
		   first[1][2], later, s->load_failure_terminal,
		   (unsigned)s->light_stop_slot, ct_wrote[0], ct_wrote[1],
		   fw->light_seed_n, saved0);
	bool common = ct_done && s->load_failure_terminal && s->light_stop_published
		&& fw->light_fault && later == 0 && !saved0 && c0->modified
		&& server_local_light_stopped(s);
	if(mode_id == 0) {
		check(common && s->light_stop_slot == 1 && !ct_wrote[0]
				  && first[0][1] == 1 && first[0][2] == 1 && first[1][0] == 1
				  && first[1][1] == 0 && first[1][2] == 0,
			  "slot 1 prelude fault: slot 1 runs no entity/tile pass, the "
			  "session stops (terminal), later updates run nothing, no save");
	} else if(mode_id == 1) {
		struct block_data b = {0}, t = {0};
		assert(server_world_get_block(&s->worlds[0], 6, ct_h0, 6, &b));
		assert(server_world_get_block(&s->worlds[0], 6, ct_h0 + 1, 6, &t));
		printf("  nested: removed %d (id %d), torch above id %d\n", ct_wrote[0],
			   b.type, t.type);
		check(common && s->light_stop_slot == 0 && b.type == BLOCK_AIR
				  && fw->light_seed_n > 0 && first[0][1] == 1
				  && first[0][2] == 0 && first[1][0] == 0,
			  "nested failure in the entity pass: partial progress kept "
			  "pending, tile entities and slot 1 never run, terminal");
	} else {
		struct block_data a = {0}, b = {0};
		assert(server_world_get_block(&s->worlds[0], 3, ct_h0, 3, &a));
		assert(server_world_get_block(&s->worlds[0], 11, ct_h0, 11, &b));
		check(common && s->light_stop_slot == 0 && ct_wrote[0] && !ct_wrote[1]
				  && a.type == BLOCK_TORCH && a.torch_light == 0
				  && b.type == BLOCK_AIR && fw->light_seed_n == 1
				  && first[0][0] == 1 && first[0][1] == 0 && first[0][2] == 0
				  && first[1][0] == 0,
			  "same-tick failure after progress: the first write stays "
			  "pending (no half drain), no entity/tile pass, terminal");
	}
	inventory_destroy(&s->player.inventory, &s->player);
	s->world_slot_state[1] = SERVER_WORLD_SLOT_EMPTY;
	server_world_discard(&s->worlds[1]);
	test_fixture_dir_cleanup(ndir);
	close_world(s, s_dir[1], false);
}


/* ---- contain, ASTRA-G9-REVIEW-2 item 1: stop INSIDE the loops ----
 * The stop contract holds inside every loop, not only between passes:
 *   furnace0 / furnace1 : a real furnace ignition (furnace_tick toggles it
 *            lit; Beta TileEntityFurnace.updateEntity -> BlockFurnace.
 *            updateFurnaceBlockState -> setBlockWithNotify, CFR
 *            TileEntityFurnace.java / BlockFurnace.java) in world slot 0 or
 *            1, its window open, fails its light reservation. A second
 *            active furnace of the same world (a later row), the other
 *            world (a furnace, a spawner, a mob), an in-flight piston of
 *            slot 0 and the RNG streams must not advance from the fault on,
 *            in this update or the next; no window/gameplay RPC follows the
 *            fault; exactly one lighting terminal; nothing is saved.
 *   rpc    : two queued player BLOCK_PLACE requests; the first fails its
 *            reservation; the second is never processed.
 *   sched  : two falling-sand scheduled ticks are due; the first write
 *            fails; the second tick is not popped, no random tick draws.
 * The fault hook snapshots the whole observable state at the exact moment
 * the light stops; everything after must equal it. */
static struct cx_snap {
	uint64_t world_time;
	uint32_t tick[2];
	uint32_t rand_seed, entity_seed, drop_seed;
	struct furnace_te furnace[4];
	struct active_spawner spawners[MAX_ACTIVE_SPAWNERS];
	uint64_t piston_hash;
	uint64_t entity_hash;
	size_t sched[2];
	bool taken;
} cx_at_fault;
static int cx_mode;
static int cx_slot;
static int cx_h0;
static bool cx_armed;
static int cx_fx[4][4]; /* furnace world, x, y, z */
static unsigned cx_rpc_after, cx_terminals, cx_rpc_total;
static int cx_bad_rpc_type = -1;

static uint64_t cx_mix(uint64_t h, uint64_t v) {
	h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
	return h;
}
static uint64_t cx_entity_hash(struct server_local* s) {
	uint64_t h = 1;
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		const struct entity* e = dict_entity_ref(it)->value;
		if(e) {
			uint32_t p[6];
			memcpy(p, e->pos, sizeof(float) * 3);
			memcpy(p + 3, e->vel, sizeof(float) * 3);
			h = cx_mix(h, e->id);
			for(int k = 0; k < 6; k++)
				h = cx_mix(h, p[k]);
		}
		dict_entity_next(it);
	}
	return h;
}
static uint64_t cx_piston_hash(struct server_local* s) {
	uint64_t h = 7;
	for(size_t i = 0; i < s->piston_moving.capacity; i++) {
		const struct piston_moving_store_entry* e = &s->piston_moving.entries[i];
		if(!e->used)
			continue;
		const uint8_t* b = (const uint8_t*)&e->state;
		for(size_t k = 0; k < sizeof e->state; k++)
			h = cx_mix(h, b[k]);
		h = cx_mix(h, (uint64_t)e->x * 31 + (uint64_t)e->z);
	}
	return h;
}
static void cx_take(struct server_local* s, struct cx_snap* o) {
	memset(o, 0, sizeof *o);
	o->world_time = s->world_time;
	for(int i = 0; i < 2; i++) {
		o->tick[i] = (uint32_t)s->worlds[i].current_tick;
		o->sched[i] = s->worlds[i].scheduled_tick_count;
	}
	o->rand_seed = s->rand_src.seed;
	o->entity_seed = s->entity_rng.seed;
	o->drop_seed = s->container_drop_rng.seed;
	for(int f = 0; f < 4; f++) {
		struct furnace_te* t = furnace_te_lookup_w(&s->worlds[cx_fx[f][0]],
			cx_fx[f][1], cx_fx[f][2], cx_fx[f][3]);
		if(t)
			o->furnace[f] = *t;
	}
	memcpy(o->spawners, s->active_spawners, sizeof o->spawners);
	o->piston_hash = cx_piston_hash(s);
	o->entity_hash = cx_entity_hash(s);
	o->taken = true;
}
static bool cx_same(const struct cx_snap* a, const struct cx_snap* b,
					const char** what) {
	*what = "world time/ticks";
	if(a->world_time != b->world_time || a->tick[0] != b->tick[0]
	   || a->tick[1] != b->tick[1])
		return false;
	*what = "RNG streams";
	if(a->rand_seed != b->rand_seed || a->entity_seed != b->entity_seed
	   || a->drop_seed != b->drop_seed)
		return false;
	*what = "furnaces";
	if(memcmp(a->furnace, b->furnace, sizeof a->furnace))
		return false;
	*what = "spawners";
	if(memcmp(a->spawners, b->spawners, sizeof a->spawners))
		return false;
	*what = "piston";
	if(a->piston_hash != b->piston_hash)
		return false;
	*what = "entities";
	if(a->entity_hash != b->entity_hash)
		return false;
	*what = "scheduled queue";
	if(a->sched[0] != b->sched[0] || a->sched[1] != b->sched[1])
		return false;
	return true;
}

static struct server_local* cx_s;
static void cx_fault_hook(struct server_world* w) {
	(void)w;
	if(!cx_at_fault.taken)
		cx_take(cx_s, &cx_at_fault);
}
static void cx_rpc_observer(const struct client_rpc* call, void* user) {
	(void)user;
	cx_rpc_total++;
	if(call->type == CRPC_LOAD_FAILED) {
		cx_terminals++;
		return;
	}
	if(cx_at_fault.taken) {
		cx_rpc_after++;
		cx_bad_rpc_type = call->type;
	}
}
static void cx_probe(struct server_local* s, struct server_world* w,
					 int phase) {
	const int wi = w == &s->worlds[0] ? 0 : 1;
	if(cx_armed && (cx_mode == 0 || cx_mode == 1) && phase == 2
	   && wi == cx_slot) {
		cx_armed = false;
		server_world_test_light_fail_after(0); /* the ignition write */
	}
	if(cx_mode <= 1 && wi == 0 && phase == 0 && cx_armed) {
		/* an in-flight piston of slot 0 (Beta BlockPistonBase extends on
		 * the torch's neighbour notify; the moving TEs settle at T+2) */
		(void)server_world_set_block(w, -6, cx_h0, -7,
			(struct block_data) {.type = BLOCK_REDSTONE_TORCH_LIT, .metadata = 5});
	}
	if(cx_mode == 3 && wi == 0 && phase == 0 && cx_armed)
		server_world_test_light_fail_after(0);
	if(cx_mode == 3 && wi == 0 && phase == 1)
		server_world_test_light_fail_after(-1); /* not consumed: re-arm */
}

static void cx_furnace(struct server_world* w, int f, int x, int y, int z,
					   bool lit, bool fueled) {
	cx_fx[f][0] = w->world_index;
	cx_fx[f][1] = x;
	cx_fx[f][2] = y;
	cx_fx[f][3] = z;
	assert(server_world_set_block(w, x, y, z,
		(struct block_data) {.type = lit ? BLOCK_FURNACE_LIT : BLOCK_FURNACE,
							 .metadata = 2}));
	assert(furnace_te_register_w(w, x, y, z) >= 0);
	struct furnace_te* t = furnace_te_lookup_w(w, x, y, z);
	assert(t);
	*t = (struct furnace_te) {0};
	t->items[FURNACE_SLOT_INPUT_TE] = (struct item_data) {.id = BLOCK_IRON_ORE, .count = 8};
	if(fueled)
		t->items[FURNACE_SLOT_FUEL_TE] = (struct item_data) {.id = ITEM_COAL, .count = 4};
	if(lit) {
		t->burn_time = 1000;
		t->current_item_burn_time = 1600;
		t->cook_time = 40;
	}
}

static void cx_run(int mode_id, int slot) {
	struct server_local* s = &S[1];
	cx_s = s;
	char id[64];
	snprintf(id, sizeof id, "test_light_queue_cx_%d_%d", mode_id, slot);
	open_world(s, s_dir[1], sizeof s_dir[1], id, true, 2);
	/* the production fresh-session TE tables (all .y < 0 == free) */
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(s))
		abort();
	mode(1);
	char ndir[260];
	snprintf(ndir, sizeof ndir, "%s_nether", s_dir[1]);
	MKDIR(ndir);
	char nreg[300];
	snprintf(nreg, sizeof nreg, "%s/region", ndir);
	MKDIR(nreg);
	string_t level;
	string_init_set_str(level, ndir);
	server_world_create(&s->worlds[1], level, WORLD_DIM_NETHER, 4242, true);
	string_clear(level);
	s->worlds[1].session = s;
	s->worlds[1].local = s;
	s->worlds[1].world_index = 1;
	for(int cx = -1; cx <= 1; cx++)
		for(int cz = -1; cz <= 1; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s->worlds[1], cx, cz, &sc) && sc);
		}
	s->world_slot_state[1] = SERVER_WORLD_SLOT_LIVE;
	assert(inventory_create(&s->player.inventory, &inventory_logic_player, s,
							&s->player, INVENTORY_SIZE, 0, 0, 0));
	s->player.active_inventory = &s->player.inventory;
	uint8_t hh = 0;
	assert(server_world_get_height(&s->worlds[0], 8, 8, &hh));
	cx_h0 = hh;
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.x = 8.5;
	s->player.y = cx_h0 + (double)PLAYER_EYE_HEIGHT;
	s->player.z = 8.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->render_demo_mob = -1;
	struct server_world* w0 = &s->worlds[0];
	struct server_world* w1 = &s->worlds[1];
	/* the furnaces: [0] the igniting one, [1] a later active furnace of the
	 * same world, [2]/[3] active furnaces of the other world */
	int ny = 1; /* the Nether floor top at (8,8) */
	for(; ny < 120; ny++) {
		struct block_data g = {0};
		assert(server_world_get_block(w1, 8, ny, 8, &g));
		if(g.type == BLOCK_AIR)
			break;
	}
	struct server_world* fw = slot == 0 ? w0 : w1;
	struct server_world* ow = slot == 0 ? w1 : w0;
	const int fy = slot == 0 ? cx_h0 : ny;
	const int oy = slot == 0 ? ny : cx_h0;
	if(slot == 1) {
		/* the host committed to the Nether (A2-W): slot 1 is a lived-in
		 * world (no vacant-world drain), slot 0 a live world without a
		 * player of its own */
		s->player.world_slot = 1;
		s->player.y = ny + (double)PLAYER_EYE_HEIGHT;
	}
	memset(cx_fx, 0, sizeof cx_fx);
	if(mode_id <= 1) {
		/* row order: [0] first (unfuelled through the settle update), then
		 * the later active furnace [1] of the same world */
		cx_furnace(fw, 0, 3, fy, 3, false, false);
		cx_furnace(fw, 1, 5, fy, 3, true, true);
		cx_furnace(ow, 2, 3, oy, 5, true, true);
		cx_furnace(ow, 3, 5, oy, 5, true, true);
		/* slot 0 spawner beside the player; a slot 1 mob */
		assert(server_world_set_block(w0, 10, cx_h0, 12,
			(struct block_data) {.type = BLOCK_MOB_SPAWNER}));
		assert(spawner_te_upsert_w(w0, 10, cx_h0, 12, "Zombie"));
		vec3 mp = {2.5F, (float)ny, 2.5F};
		assert(server_local_spawn_monster_at_w(w1, mp, MONSTER_ZOMBIE));
		/* a piston for the in-flight move (slot 0) */
		assert(server_world_set_block(w0, -6, cx_h0, -6,
			(struct block_data) {.type = BLOCK_PISTON_BASE, .metadata = 5}));
		assert(server_world_set_block(w0, -5, cx_h0, -6,
			(struct block_data) {.type = BLOCK_STONE}));
	} else if(mode_id == 2) {
		inventory_set_hotbar(&s->player.inventory, 0);
		inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR,
			(struct item_data) {.id = BLOCK_STONE, .count = 8});
	}
	server_local_test_update_once(s);
	assert(server_world_light_drain(w0));
	assert(server_world_light_drain(w1));
	if(mode_id <= 1) {
		/* fuel the first furnace now: it ignites on its next tick (Beta
		 * TileEntityFurnace.updateEntity: burnTime 0 + canSmelt -> burn the
		 * coal -> BlockFurnace.updateFurnaceBlockState), window open */
		furnace_te_lookup_w(fw, 3, fy, 3)->items[FURNACE_SLOT_FUEL_TE]
			= (struct item_data) {.id = ITEM_COAL, .count = 4};
		assert(server_local_open_inventory_in(s, fw, &s->player,
			&inventory_logic_furnace, FURNACE_SIZE, 3, fy, 3, WINDOWC_FURNACE,
			WINDOW_TYPE_FURNACE));
		assert(server_local_window_bound_to(s->player.active_inventory, fw));
	}
	if(mode_id == 3) {
		/* sand over air near the loaded edge (BlockSand.tryToFall settles
		 * directly when the 32-block area is not loaded: its writes run
		 * inside the scheduled-tick loop) */
		for(int k = 0; k < 2; k++)
			assert(server_world_set_block(w0, 26 + 4 * k, cx_h0 + 3, 4,
				(struct block_data) {.type = BLOCK_SAND}));
		assert(server_world_light_drain(w0));
		assert(w0->scheduled_tick_count >= 2);
	}
	memset(&cx_at_fault, 0, sizeof cx_at_fault);
	cx_mode = mode_id;
	cx_slot = slot;
	cx_armed = true;
	cx_rpc_after = cx_terminals = cx_rpc_total = 0;
	cx_bad_rpc_type = -1;
	const uint32_t dropped0 = s->light_rpc_dropped;
	server_world_test_light_fault_hook = cx_fault_hook;
	clin_test_set_rpc_observer(cx_rpc_observer, NULL);
	server_local_test_phase_hook = cx_probe;
	if(mode_id == 2) {
		for(int k = 0; k < 2; k++) {
			struct server_rpc place = {.type = SRPC_BLOCK_PLACE,
				.payload.block_place = {.x = 6 + 4 * k, .y = cx_h0 - 1,
										.z = 6, .side = SIDE_TOP}};
			assert(svin_rpc_send(&place));
		}
		server_world_test_light_fail_after(0);
	}
	struct block_data before2 = {0};
	(void)server_world_get_block(w0, 10, cx_h0, 6, &before2);
	/* the faulting update (sand: until its ticks are due) */
	for(int n = 0; n < 8 && !cx_at_fault.taken; n++)
		server_local_test_update_once(s);
	server_world_test_light_fail_after(-1);
	struct cx_snap end1, end2;
	cx_take(s, &end1);
	server_local_test_update_once(s); /* the next update */
	server_local_test_update_once(s);
	cx_take(s, &end2);
	server_local_test_phase_hook = NULL;
	clin_test_set_rpc_observer(NULL, NULL);
	server_world_test_light_fault_hook = NULL;
	const char* what1 = "";
	const char* what2 = "";
	const bool same1 = cx_at_fault.taken && cx_same(&cx_at_fault, &end1, &what1);
	const bool same2 = cx_at_fault.taken && cx_same(&cx_at_fault, &end2, &what2);
	/* no save of the stopped session */
	struct server_chunk* c0 = dict_server_chunks_get(w0->chunks, S_CHUNK_ID(-1, -1));
	assert(c0);
	c0->modified = true;
	const bool saved0 = server_world_save_chunk(w0, false, -1, -1);
	printf("  %s slot %d: fault %d; after fault: same in update %d (%s), next "
		   "updates %d (%s); RPCs after %u (last type %d), terminals %u; save "
		   "%d; requests dropped %u\n",
		   mode_id <= 1 ? "furnace" : mode_id == 2 ? "rpc" : "sched", slot,
		   cx_at_fault.taken, same1, same1 ? "-" : what1, same2,
		   same2 ? "-" : what2, cx_rpc_after, cx_bad_rpc_type, cx_terminals,
		   saved0, s->light_rpc_dropped - dropped0);
	bool extra = true;
	if(mode_id == 2) {
		struct block_data a = {0}, b = {0};
		(void)server_world_get_block(w0, 6, cx_h0, 6, &a);
		(void)server_world_get_block(w0, 10, cx_h0, 6, &b);
		extra = a.type == BLOCK_AIR && b.type == before2.type
			&& s->light_rpc_dropped - dropped0 >= 1;
		printf("    first place cell id %d, second place cell id %d\n", a.type,
			   b.type);
	}
	char what[220];
	snprintf(what, sizeof what,
			 "%s fault (slot %d): nothing advances from the fault on (furnaces, "
			 "spawner, piston, entities, RNG, ticks), no RPC but one terminal, "
			 "no save",
			 mode_id <= 1 ? "furnace ignition" : mode_id == 2 ?
				 "queued player RPC" : "scheduled tick", slot);
	check(cx_at_fault.taken && same1 && same2 && cx_rpc_after == 0
			  && cx_terminals == 1 && !saved0 && s->load_failure_terminal
			  && s->light_stop_slot == (uint8_t)slot && extra, what);
	clin_server_publication_stop(false);
	if(s->player.active_inventory != &s->player.inventory) {
		s->player.active_inventory = &s->player.inventory;
	}
	inventory_destroy(&s->player.inventory, &s->player);
	s->world_slot_state[1] = SERVER_WORLD_SLOT_EMPTY;
	server_world_discard(&s->worlds[1]);
	test_fixture_dir_cleanup(ndir);
	close_world(s, s_dir[1], false);
}

static void case_contain(void) {
	hs_run(false);
	if(getenv("CW_HALT_ONLY")) return;
	ct_run(0);
	ct_run(1);
	ct_run(2);
	cx_run(0, 0); /* furnace ignition fault, slot 0 */
	cx_run(1, 1); /* furnace ignition fault, slot 1 */
	cx_run(2, 0); /* queued player RPCs */
	cx_run(3, 0); /* scheduled-tick loop */
}


/* ---- checkpoint (ASTRA-G9-REVIEW Required 1) ----
 * A live checkpoint never settles the live world early: it writes a
 * settled trial image and rolls the live field back. Proved by
 *   crash   : edits pending, checkpoint, then the live world still shows the
 *             pending Beta values (torch 0) with its seeds; a crash (discard,
 *             no persist) and reload yields the settled image, byte-equal to
 *             the synchronous session over all 9 chunks.
 *   resume  : after a checkpoint the live world drains at its prescribed
 *             boundary to the synchronous result (the trial changed nothing).
 *   retry   : an injected region write failure fails the checkpoint with
 *             the queue still pending and the chunks dirty; a retry
 *             succeeds and the reload is settled. */
static void ckpt_reopen(int i) {
	struct server_world* w = &S[i].worlds[0];
	string_t level;
	string_init_set_str(level, s_dir[i]);
	server_world_create(w, level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	w->session = &S[i];
	w->local = &S[i];
	for(int cx = -1; cx <= 1; cx++)
		for(int cz = -1; cz <= 1; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(w, cx, cz, &sc) && sc);
		}
}

static void ckpt_run(int kind) {
	char id0[64], id1[64];
	snprintf(id0, sizeof id0, "test_light_queue_ckpt_sync_%d", kind);
	snprintf(id1, sizeof id1, "test_light_queue_ckpt_queue_%d", kind);
	open_world(&S[0], s_dir[0], sizeof s_dir[0], id0, true, 1);
	open_world(&S[1], s_dir[1], sizeof s_dir[1], id1, true, 1);
	uint8_t hh = 0;
	assert(server_world_get_height(&S[0].worlds[0], 8, 8, &hh));
	const int h0 = hh;
	for(int i = 0; i < 2; i++) {
		mode(i);
		struct server_world* w = &S[i].worlds[0];
		assert(server_world_set_block(w, 4, h0, 4,
									  (struct block_data) {.type = BLOCK_TORCH}));
		for(int x = 8; x < 20; x++)
			for(int z = 8; z < 12; z++)
				assert(server_world_set_block(w, x, h0 + 3, z,
					(struct block_data) {.type = BLOCK_STONE}));
	}
	tick_drain(0); /* the sync baseline settles */
	mode(1);
	struct server_world* w = &S[1].worlds[0];
	const size_t seeds = w->light_seed_n;
	server_world_test_light_stats_reset();
	bool ok = true;
	if(kind == 2) {
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
		bool first = server_world_checkpoint_chunks(w);
		struct block_data t = {0};
		assert(server_world_get_block(w, 4, h0, 4, &t));
		struct server_chunk* sc = dict_server_chunks_get(w->chunks,
														 S_CHUNK_ID(0, 0));
		check(!first && t.torch_light == 0 && w->light_seed_n == seeds
				  && server_world_light_pinned(w, 0, 0) && sc && sc->modified,
			  "failed checkpoint: queue still pending (no early drain), "
			  "chunks dirty, transaction retryable");
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_NONE);
	}
	ok = server_world_checkpoint_chunks(w) && server_world_sync_regions(w);
	struct block_data t = {0};
	assert(server_world_get_block(w, 4, h0, 4, &t));
	struct server_world_light_stats st;
	server_world_light_stats(&st);
	printf("  kind %d: checkpoint %d; live torch %d seeds %zu/%zu; trials %u\n",
		   kind, ok, t.torch_light, w->light_seed_n, seeds,
		   st.checkpoint_trials);
	check(ok && t.torch_light == 0 && w->light_seed_n == seeds
			  && server_world_light_pending(w) && st.checkpoint_trials >= 1,
		  "checkpoint written; the live world still holds the pending Beta "
		  "values and its queue (no early drain)");
	if(kind == 1) {
		tick_drain(1);
		long d = diff_cells(0, 1, 0, 128);
		check(d == 0 && !server_world_light_pending(w),
			  "after the checkpoint the prescribed drain settles exactly as "
			  "the synchronous session (the trial left no trace)");
		close_world(&S[1], s_dir[1], false);
	} else {
		/* crash: no persist, no drain; reload what the checkpoint wrote */
		server_world_discard(w);
		ckpt_reopen(1);
		long d = diff_cells(0, 1, 0, 128);
		struct block_data r = {0};
		assert(server_world_get_block(&S[1].worlds[0], 4, h0, 4, &r));
		printf("  reloaded torch %d; %ld cells differ from sync\n",
			   r.torch_light, d);
		check(d == 0 && r.torch_light == 14,
			  "crash after the checkpoint reloads the settled image of the "
			  "same edits, byte-equal to the synchronous session");
		close_world(&S[1], s_dir[1], false);
	}
	close_world(&S[0], s_dir[0], false);
}

/* kind 3 (ASTRA-G9-REVIEW-2 item 5): the trial snapshot allocation fails.
 * The checkpoint fails retryably with the live world untouched (pending
 * values, seeds, pins, dirty chunks, no write), the heartbeat counts it;
 * the retry writes the settled image, measured: the snapshot is exactly
 * the 9 resident chunks around the two pins x 32 KiB + 18 chunk ids. */
static void ckpt_oom_run(void) {
	open_world(&S[0], s_dir[0], sizeof s_dir[0], "test_light_queue_ckpt_sync_3",
			   true, 1);
	open_world(&S[1], s_dir[1], sizeof s_dir[1],
			   "test_light_queue_ckpt_queue_3", true, 1);
	uint8_t hh = 0;
	assert(server_world_get_height(&S[0].worlds[0], 8, 8, &hh));
	const int h0 = hh;
	for(int i = 0; i < 2; i++) {
		mode(i);
		struct server_world* w = &S[i].worlds[0];
		assert(server_world_set_block(w, 4, h0, 4,
									  (struct block_data) {.type = BLOCK_TORCH}));
		for(int x = 8; x < 20; x++)
			for(int z = 8; z < 12; z++)
				assert(server_world_set_block(w, x, h0 + 3, z,
					(struct block_data) {.type = BLOCK_STONE}));
	}
	tick_drain(0);
	mode(1);
	struct server_world* w = &S[1].worlds[0];
	const size_t seeds = w->light_seed_n, pins = w->light_pin_n;
	server_world_test_light_stats_reset();
	static uint8_t snap[48 * 48 * 128];
	(void)snapshot_diff(1, snap, true);
	server_world_test_light_trial_fail(1);
	const bool first = server_world_checkpoint_chunks(w);
	struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
	struct server_world_light_hb hb;
	server_world_light_hb(&hb);
	const long moved = snapshot_diff(1, snap, false);
	printf("  snapshot OOM: checkpoint %d, seeds %zu/%zu, field cells changed "
		   "%ld, ckpt_fail %u, trials %u\n", first, w->light_seed_n, seeds,
		   moved, hb.ckpt_fail, hb.trials);
	check(!first && w->light_seed_n == seeds && w->light_pin_n == pins
			  && moved == 0 && sc && sc->modified && hb.ckpt_fail == 1
			  && hb.trials == 0 && !w->light_fault,
		  "snapshot OOM: checkpoint fails retryably, live world untouched "
		  "(pending, pinned, dirty), counted");
	const bool retry = server_world_checkpoint_chunks(w)
		&& server_world_sync_regions(w);
	server_world_light_hb(&hb);
	/* Nine plane pairs plus the id reservation and one lazy-page map per
	 * chunk (a plane pointer and 512 capture bits). Account for metadata too. */
	const uint32_t want = (uint32_t)(9 * 16 * 16 * 128 + pins * 9 * sizeof(int64_t)
		+ 9 * (sizeof(uint8_t*) + 16 * sizeof(uint32_t)));
	printf("  retry %d: snapshot peak %u B (expected %u), ckpt_us %u, "
		   "tick_us %u, trials %u\n", retry, hb.ckpt_b[0], want, hb.ckpt_us,
		   hb.tick_us, hb.trials);
	check(retry && hb.trials == 1 && hb.ckpt_b[0] == want && hb.ckpt_us > 0
			  && snapshot_diff(1, snap, false) == 0
			  && w->light_seed_n == seeds,
		  "the retry writes the settled trial and rolls back; the snapshot "
		  "allocation is measured exactly");
	server_world_discard(w);
	ckpt_reopen(1);
	long d = diff_cells(0, 1, 0, 128);
	check(d == 0, "crash after the retried checkpoint reloads the settled "
				  "image byte-equal to the synchronous session");
	close_world(&S[1], s_dir[1], false);
	close_world(&S[0], s_dir[0], false);
}

static void case_checkpoint(void) {
	ckpt_run(0); /* crash/reload */
	ckpt_run(1); /* resume at the prescribed drain */
	ckpt_run(2); /* failed checkpoint, retry, crash/reload */
	ckpt_oom_run(); /* snapshot OOM, rollback, retry */
}


/* ---- beta_oracle (ASTRA-G9-REVIEW Required 4) ----
 * An independent Beta 1.7.3 lighting reference, transcribed from the CFR
 * server decompile (/c/tmp/mcb173-server-decompile/out/net/minecraft/src/):
 *   Chunk.setBlockIDWithMetadata:208-239 (immediate func_339_g, then the
 *     Sky + Block single-cell schedules and func_333_c),
 *   Chunk.func_339_g:141-200, func_333_c:121-129, func_355_f:131-139,
 *   World.func_484_a:1300-1330 (blockExists gate, merge into the last 5
 *     entries via MetadataChunkBlock.func_692_a, 1,000,000 abort),
 *   World.func_6156_d:1269-1291 (LIFO pop; MinecraftServer:347 loops it
 *     until empty), MetadataChunkBlock.func_4107_a (box relaxation,
 *     32768-cell skip, doChunksNearChunkExist gate, neighbour schedules),
 *   World.neighborLightPropagationChanged:477-491, getSavedLightValue:
 *     493-510 (y clamp, missing chunk = 0), setLightValue, blockExists,
 *     canExistingBlockSeeTheSky:449-463, getHeightValue.
 * Block.lightOpacity / lightValue for the palette are Beta's (Block.java:
 * stone/glowstone opaque 255, glass/torch 0, leaves 1, ice/water 3; torch
 * (int)(15*0.9375)=14, glowstone 15), NOT CaveWii's tables.
 * The model holds a 5x5-chunk region; edits stay >= 16 blocks from its rim
 * so Beta's chunk-existence gates never trip. It never calls CaveWii's
 * flood: its queue, merge and relaxation are Beta's own. */
/* The model region: 5x5 chunks from (-2,-2) by default; the residency-
 * stable generation-overlap fixture (ASTRA-G9-RULING-2 C) widens it to its
 * whole resident domain (bo_region). Storage is sized for the widest. */
#define BO_NCMAX 23
#define BO_NMAX (BO_NCMAX * 16)
static int bo_minc = -2, bo_nc = 5;
#define BO_MINC bo_minc
#define BO_NC bo_nc
#define BO_MIN (BO_MINC * 16)
#define BO_N (BO_NC * 16)
static uint8_t (*bo_id)[BO_NMAX][128];
static uint8_t (*bo_sky)[BO_NMAX][128];
static uint8_t (*bo_blk)[BO_NMAX][128];
static uint8_t (*bo_h)[BO_NMAX];
static void bo_alloc(void) {
	if(bo_id)
		return;
	bo_id = calloc(BO_NMAX, sizeof *bo_id);
	bo_sky = calloc(BO_NMAX, sizeof *bo_sky);
	bo_blk = calloc(BO_NMAX, sizeof *bo_blk);
	bo_h = calloc(BO_NMAX, sizeof *bo_h);
	assert(bo_id && bo_sky && bo_blk && bo_h);
}
/* the region the model holds: chunks c0..c0+n-1 on both axes */
static void bo_region(int c0, int n) {
	assert(n <= BO_NCMAX);
	bo_alloc();
	bo_minc = c0;
	bo_nc = n;
}
static bool bo_nether;
enum { BO_SKY = 0, BO_BLOCK = 1 };
struct bo_mcb {
	int type, x1, y1, z1, x2, y2, z2;
};
static struct bo_mcb* bo_q;
static size_t bo_qn, bo_qcap;
static unsigned long bo_relaxed, bo_merged;

/* Mutation switch for the kill check (G9_BO_MUTATE): 1 = the oracle never
 * drains, 2 = leaves get opacity 0 in the oracle, 3 = no merge. */
static int bo_mutate;
static int bo_opacity(uint8_t id) {
	if(bo_mutate == 2 && id == BLOCK_LEAVES)
		return 0;
	/* Beta Block.lightOpacity: isOpaqueCube() ? 255 : 0 (Block.java:341),
	 * then the explicit setLightOpacity calls (Block.java:225-228 water 3 /
	 * lava 255, 235 leaves 1, 247 web 1, 296 ice 3; BlockFarmland:28,
	 * BlockStairs:40, BlockStep:28 255). Non-opaque classes (isOpaqueCube
	 * false): Bed, Breakable (glass, ice), Button, Cactus, Cake, Door,
	 * Farmland, Fence, Fire, Flower (sapling, tall grass, dead bush, flowers,
	 * mushrooms, crops), Fluid, Ladder, LeavesBase, Lever, MobSpawner,
	 * PistonBase/Extension/Moving, Portal, PressurePlate, Rail, Repeater,
	 * RedstoneWire, Reed, Sign, Snow, Stairs, Torch, TrapDoor, Web. */
	switch(id) {
		case 8: case 9: return 3;      /* water */
		case 10: case 11: return 255;  /* lava */
		case 18: return 1;             /* leaves */
		case 30: return 1;             /* web */
		case 79: return 3;             /* ice */
		case 60: return 255;           /* farmland */
		case 53: case 67: return 255;  /* stairs */
		case 44: return 255;           /* slab */
		case 0:                        /* air */
		case 6: case 26: case 20: case 77: case 81: case 92: case 64:
		case 71: case 85: case 51: case 31: case 32: case 37: case 38:
		case 39: case 40: case 59: case 65: case 69: case 52: case 29:
		case 33: case 34: case 36: case 90: case 70: case 72: case 27:
		case 28: case 66: case 93: case 94: case 55: case 83: case 63:
		case 68: case 78: case 50: case 75: case 76: case 96:
			return 0;
		default: return 255; /* opaque cubes */
	}
}
static int bo_lightvalue(uint8_t id) {
	/* Beta Block.lightValue = (int)(15 * setLightValue(f)), Block.java */
	switch(id) {
		case BLOCK_TORCH: return 14;         /* 0.9375 */
		case BLOCK_GLOWSTONE: return 15;
		case BLOCK_FURNACE_LIT: return 13;   /* 0.875 */
		case BLOCK_REDSTONE_TORCH_LIT: return 7; /* 0.5 */
		case 10: case 11: case 51: return 15; /* lava, fire 1.0 */
		case 39: return 1;                   /* brown mushroom 0.125 */
		case 90: return 11;                  /* portal 0.75 */
		case 74: return 9;                   /* glowing redstone ore 0.625 */
		case 91: return 15;                  /* jack-o-lantern 1.0 */
		case 94: return 9;                   /* powered repeater 0.625 */
		default: return 0;
	}
}
static bool bo_chunk_exists(int cx, int cz) {
	return cx >= BO_MINC && cx < BO_MINC + BO_NC && cz >= BO_MINC
		&& cz < BO_MINC + BO_NC;
}
static int bo_fdiv16(int v) {
	return v >> 4; /* Java >> on int == floor */
}
static bool bo_block_exists(int x, int y, int z) {
	return y >= 0 && y < 128 && bo_chunk_exists(bo_fdiv16(x), bo_fdiv16(z));
}
static int bo_get_saved(int type, int x, int y, int z) {
	if(y < 0)
		y = 0;
	if(y >= 128)
		y = 127;
	if(!bo_chunk_exists(bo_fdiv16(x), bo_fdiv16(z)))
		return 0;
	return type == BO_SKY ? bo_sky[x - BO_MIN][z - BO_MIN][y] :
							bo_blk[x - BO_MIN][z - BO_MIN][y];
}
static void bo_set_light(int type, int x, int y, int z, int v) {
	if(y < 0 || y >= 128 || !bo_chunk_exists(bo_fdiv16(x), bo_fdiv16(z)))
		return;
	if(type == BO_SKY)
		bo_sky[x - BO_MIN][z - BO_MIN][y] = (uint8_t)v;
	else
		bo_blk[x - BO_MIN][z - BO_MIN][y] = (uint8_t)v;
}
static int bo_height(int x, int z) {
	if(!bo_chunk_exists(bo_fdiv16(x), bo_fdiv16(z)))
		return 0;
	return bo_h[x - BO_MIN][z - BO_MIN];
}
static bool bo_can_see_sky(int x, int y, int z) {
	if(y < 0)
		return false;
	if(y >= 128)
		return true;
	if(!bo_chunk_exists(bo_fdiv16(x), bo_fdiv16(z)))
		return false;
	return y >= bo_h[x - BO_MIN][z - BO_MIN];
}
static uint8_t bo_get_id(int x, int y, int z) {
	if(y < 0 || y >= 128 || !bo_chunk_exists(bo_fdiv16(x), bo_fdiv16(z)))
		return 0;
	return bo_id[x - BO_MIN][z - BO_MIN][y];
}

/* MetadataChunkBlock.func_692_a */
static bool bo_merge(struct bo_mcb* m, int a1, int a2, int a3, int a4, int a5,
					 int a6) {
	if(a1 >= m->x1 && a2 >= m->y1 && a3 >= m->z1 && a4 <= m->x2
	   && a5 <= m->y2 && a6 <= m->z2)
		return true;
	int k = 1;
	if(a1 >= m->x1 - k && a2 >= m->y1 - k && a3 >= m->z1 - k
	   && a4 <= m->x2 + k && a5 <= m->y2 + k && a6 <= m->z2 + k) {
		int v8 = m->x2 - m->x1, v9 = m->y2 - m->y1, v10 = m->z2 - m->z1;
		if(a1 > m->x1)
			a1 = m->x1;
		if(a2 > m->y1)
			a2 = m->y1;
		if(a3 > m->z1)
			a3 = m->z1;
		if(a4 < m->x2)
			a4 = m->x2;
		if(a5 < m->y2)
			a5 = m->y2;
		if(a6 < m->z2)
			a6 = m->z2;
		int v15 = (a4 - a1) * (a5 - a2) * (a6 - a3);
		int v14 = v8 * v9 * v10;
		if(v15 - v14 <= 2) {
			m->x1 = a1;
			m->y1 = a2;
			m->z1 = a3;
			m->x2 = a4;
			m->y2 = a5;
			m->z2 = a6;
			return true;
		}
	}
	return false;
}

/* World.scheduleLightingUpdate -> func_484_a(..., true) */
static void bo_schedule(int type, int x1, int y1, int z1, int x2, int y2,
						int z2) {
	if(bo_nether && type == BO_SKY)
		return;
	if(!bo_block_exists((x2 + x1) / 2, 64, (z2 + z1) / 2))
		return;
	int n = (int)bo_qn, lim = bo_mutate == 3 ? 0 : 5;
	if(lim > n)
		lim = n;
	for(int i = 0; i < lim; i++) {
		struct bo_mcb* m = &bo_q[bo_qn - (size_t)i - 1];
		if(m->type == type && bo_merge(m, x1, y1, z1, x2, y2, z2)) {
			bo_merged++;
			return;
		}
	}
	if(bo_qn == bo_qcap) {
		bo_qcap = bo_qcap ? bo_qcap * 2 : 256;
		bo_q = realloc(bo_q, bo_qcap * sizeof *bo_q);
		assert(bo_q);
	}
	bo_q[bo_qn++] = (struct bo_mcb) {type, x1, y1, z1, x2, y2, z2};
	assert(bo_qn <= 1000000 && "Beta aborts the queue here");
}

/* World.neighborLightPropagationChanged */
static void bo_neighbor_changed(int type, int x, int y, int z, int v) {
	if(bo_nether && type == BO_SKY)
		return;
	if(!bo_block_exists(x, y, z))
		return;
	if(type == BO_SKY) {
		if(bo_can_see_sky(x, y, z))
			v = 15;
	} else if(bo_lightvalue(bo_get_id(x, y, z)) > v)
		v = bo_lightvalue(bo_get_id(x, y, z));
	if(bo_get_saved(type, x, y, z) != v)
		bo_schedule(type, x, y, z, x, y, z);
}

/* World.checkChunksExist via doChunksNearChunkExist(x, 0, z, 1) */
static bool bo_near_exist(int x, int z) {
	for(int cx = bo_fdiv16(x - 1); cx <= bo_fdiv16(x + 1); cx++)
		for(int cz = bo_fdiv16(z - 1); cz <= bo_fdiv16(z + 1); cz++)
			if(!bo_chunk_exists(cx, cz))
				return false;
	return true;
}

/* MetadataChunkBlock.func_4107_a */
static void bo_relax(struct bo_mcb m) {
	int v2 = m.x2 - m.x1 + 1, v3 = m.y2 - m.y1 + 1, v4 = m.z2 - m.z1 + 1;
	if(v2 * v3 * v4 > 32768)
		return; /* "Light too large, skipping!" */
	for(int x = m.x1; x <= m.x2; x++)
		for(int z = m.z1; z <= m.z2; z++) {
			if(!bo_near_exist(x, z))
				continue;
			if(m.y1 < 0)
				m.y1 = 0;
			if(m.y2 >= 128)
				m.y2 = 127;
			for(int y = m.y1; y <= m.y2; y++) {
				bo_relaxed++;
				int v16 = bo_get_saved(m.type, x, y, z);
				uint8_t id = bo_get_id(x, y, z);
				int v19 = bo_opacity(id);
				if(v19 == 0)
					v19 = 1;
				int v20 = 0;
				if(m.type == BO_SKY) {
					if(bo_can_see_sky(x, y, z))
						v20 = 15;
				} else
					v20 = bo_lightvalue(id);
				int v28;
				if(v19 >= 15 && v20 == 0)
					v28 = 0;
				else {
					int a = bo_get_saved(m.type, x - 1, y, z);
					int b = bo_get_saved(m.type, x + 1, y, z);
					int c = bo_get_saved(m.type, x, y - 1, z);
					int d = bo_get_saved(m.type, x, y + 1, z);
					int e = bo_get_saved(m.type, x, y, z - 1);
					int f = bo_get_saved(m.type, x, y, z + 1);
					v28 = a;
					if(b > v28)
						v28 = b;
					if(c > v28)
						v28 = c;
					if(d > v28)
						v28 = d;
					if(e > v28)
						v28 = e;
					if(f > v28)
						v28 = f;
					if((v28 -= v19) < 0)
						v28 = 0;
					if(v20 > v28)
						v28 = v20;
				}
				if(v16 == v28)
					continue;
				bo_set_light(m.type, x, y, z, v28);
				int v21 = v28 - 1;
				if(v21 < 0)
					v21 = 0;
				bo_neighbor_changed(m.type, x - 1, y, z, v21);
				bo_neighbor_changed(m.type, x, y - 1, z, v21);
				bo_neighbor_changed(m.type, x, y, z - 1, v21);
				if(x + 1 >= m.x2)
					bo_neighbor_changed(m.type, x + 1, y, z, v21);
				if(y + 1 >= m.y2)
					bo_neighbor_changed(m.type, x, y + 1, z, v21);
				if(z + 1 >= m.z2)
					bo_neighbor_changed(m.type, x, y, z + 1, v21);
			}
		}
}

/* MinecraftServer.doTick:347 drains lighting in LIFO order until empty. */
static void bo_drain(void) {
	if(bo_mutate == 1) {
		bo_qn = 0;
		return;
	}
	while(bo_qn > 0) {
		struct bo_mcb m = bo_q[--bo_qn];
		bo_relax(m);
	}
}

/* Chunk.func_355_f / func_333_c */
static void bo_355f(int x, int z, int h) {
	int v4 = bo_height(x, z);
	if(v4 > h)
		bo_schedule(BO_SKY, x, h, z, x, v4, z);
	else if(v4 < h)
		bo_schedule(BO_SKY, x, v4, z, x, h, z);
}
static void bo_333c(int x, int z) {
	int h = bo_h[x - BO_MIN][z - BO_MIN];
	bo_355f(x - 1, z, h);
	bo_355f(x + 1, z, h);
	bo_355f(x, z - 1, h);
	bo_355f(x, z + 1, h);
}

/* Chunk.func_339_g (world coords; the chunk is the one holding x,z) */
static void bo_339g(int x, int y2, int z) {
	uint8_t* col = bo_id[x - BO_MIN][z - BO_MIN];
	uint8_t* sky = bo_sky[x - BO_MIN][z - BO_MIN];
	int v4 = bo_h[x - BO_MIN][z - BO_MIN];
	int v5 = v4;
	if(y2 > v4)
		v5 = y2;
	while(v5 > 0 && bo_opacity(col[v5 - 1]) == 0)
		--v5;
	if(v5 == v4)
		return;
	bo_h[x - BO_MIN][z - BO_MIN] = (uint8_t)v5;
	if(v5 < v4) {
		for(int y = v5; y < v4; y++)
			sky[y] = 15;
	} else {
		bo_schedule(BO_SKY, x, v4, z, x, v5, z);
		for(int y = v4; y < v5; y++)
			sky[y] = 0;
	}
	int v9 = 15, v10 = v5;
	while(v5 > 0 && v9 > 0) {
		int op = bo_opacity(col[--v5]);
		if(op == 0)
			op = 1;
		if((v9 -= op) < 0)
			v9 = 0;
		sky[v5] = (uint8_t)v9;
	}
	while(v5 > 0 && bo_opacity(col[v5 - 1]) == 0)
		--v5;
	if(v5 != v10)
		bo_schedule(BO_SKY, x - 1, v5, z - 1, x + 1, v10, z + 1);
}

/* Chunk.setBlockIDWithMetadata, light part (no block callbacks: the
 * palette's onBlockAdded/onBlockRemoval write nothing) */
static void bo_set_block(int x, int y, int z, uint8_t id) {
	uint8_t* cell = &bo_id[x - BO_MIN][z - BO_MIN][y];
	if(*cell == id)
		return;
	int v7 = bo_h[x - BO_MIN][z - BO_MIN];
	*cell = id;
	if(!bo_nether) {
		if(bo_opacity(id) != 0) {
			if(y >= v7)
				bo_339g(x, y + 1, z);
		} else if(y == v7 - 1)
			bo_339g(x, y, z);
		bo_schedule(BO_SKY, x, y, z, x, y, z);
	}
	bo_schedule(BO_BLOCK, x, y, z, x, y, z);
	bo_333c(x, z);
}

/* load the oracle from a CaveWii world (ids, Sky, Block, heightmap) */
static void bo_load(struct server_world* w) {
	bo_alloc();
	bo_nether = w->dimension == WORLD_DIM_NETHER;
	bo_qn = 0;
	for(int x = 0; x < BO_N; x++)
		for(int z = 0; z < BO_N; z++) {
			uint8_t hh = 0;
			assert(server_world_get_height(w, x + BO_MIN, z + BO_MIN, &hh));
			bo_h[x][z] = hh;
			for(int y = 0; y < 128; y++) {
				struct block_data b = {0};
				assert(server_world_get_block(w, x + BO_MIN, y, z + BO_MIN, &b));
				bo_id[x][z][y] = b.type;
				bo_sky[x][z][y] = b.sky_light;
				bo_blk[x][z][y] = b.torch_light;
			}
		}
}

/* first difference oracle vs CaveWii over the whole region */
static long bo_diff(struct server_world* w, const char* tag) {
	long n = 0;
	for(int x = 0; x < BO_N; x++)
		for(int z = 0; z < BO_N; z++) {
			uint8_t hh = 0;
			assert(server_world_get_height(w, x + BO_MIN, z + BO_MIN, &hh));
			if(hh != bo_h[x][z]) {
				if(n == 0)
					printf("    %s first diff: height (%d,%d) cavewii %d "
						   "beta %d\n", tag, x + BO_MIN, z + BO_MIN, hh,
						   bo_h[x][z]);
				n++;
			}
			for(int y = 0; y < 128; y++) {
				struct block_data b = {0};
				assert(server_world_get_block(w, x + BO_MIN, y, z + BO_MIN, &b));
				if(b.type != bo_id[x][z][y] || b.sky_light != bo_sky[x][z][y]
				   || b.torch_light != bo_blk[x][z][y]) {
					if(n == 0)
						printf("    %s first diff at (%d,%d,%d): id %d/%d "
							   "sky %d/%d blk %d/%d (cavewii/beta)\n", tag,
							   x + BO_MIN, y, z + BO_MIN, b.type,
							   bo_id[x][z][y], b.sky_light, bo_sky[x][z][y],
							   b.torch_light, bo_blk[x][z][y]);
					n++;
				}
			}
		}
	return n;
}

struct bo_edit {
	int x, y, z;
	uint8_t id;
};

/* one batch on both: CaveWii set_block (queue) and the oracle; compare the
 * pre-drain fields (func_339_g immediate writes), drain both, compare the
 * settled fields. */
static void bo_batch(struct server_world* w, const struct bo_edit* e, int n,
					 long* pre, long* post, const char* tag) {
	for(int i = 0; i < n; i++) {
		(void)server_world_set_block(w, e[i].x, e[i].y, e[i].z,
									 (struct block_data) {.type = e[i].id});
		bo_set_block(e[i].x, e[i].y, e[i].z, e[i].id);
	}
	char t[96];
	snprintf(t, sizeof t, "%s pre", tag);
	*pre += bo_diff(w, t);
	assert(server_world_light_drain(w));
	bo_drain();
	snprintf(t, sizeof t, "%s post", tag);
	*post += bo_diff(w, t);
}

static void bo_world(struct server_local* s, char* dir, size_t n,
					 const char* id, enum world_dim dim) {
	if(dim == WORLD_DIM_OVERWORLD) {
		open_world(s, dir, n, id, true, 2);
		return;
	}
	test_fixture_dir(id, dir, n);
	MKDIR(dir);
	char region[260];
	snprintf(region, sizeof region, "%s/region", dir);
	MKDIR(region);
	memset(s, 0, sizeof *s);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(s))
		abort();
	dict_entity_init(s->entities);
	clin_server_publication_stop(false);
	string_t level;
	string_init_set_str(level, dir);
	server_world_create(&s->worlds[0], level, dim, 4242, true);
	string_clear(level);
	s->worlds[0].session = s;
	s->worlds[0].local = s;
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s->worlds[0], cx, cz, &sc) && sc);
		}
}

static void bo_scenarios(enum world_dim dim, const char* name) {
	struct server_local* s = &S[1];
	char id[64];
	snprintf(id, sizeof id, "test_light_queue_beta_%s", name);
	bo_world(s, s_dir[1], sizeof s_dir[1], id, dim);
	mode(1);
	struct server_world* w = &s->worlds[0];
	/* the first air cell above the flat ground at (8,8) */
	int h0 = 1;
	for(; h0 < 120; h0++) {
		struct block_data g = {0};
		assert(server_world_get_block(w, 8, h0, 8, &g));
		if(g.type == BLOCK_AIR)
			break;
	}
	printf("  %s: ground top at y=%d\n", name, h0);
	assert(h0 > 1 && h0 < 100);
	bo_load(w);
	long pre = 0, post = 0;
	struct bo_edit e[512];
	int n;
	/* roof across the x = 15|16 seam, then its removal */
	n = 0;
	for(int x = 10; x < 22; x++)
		for(int z = 4; z < 9; z++)
			e[n++] = (struct bo_edit) {x, h0 + 3, z, BLOCK_STONE};
	bo_batch(w, e, n, &pre, &post, "roof_place");
	for(int i = 0; i < n; i++)
		e[i].id = BLOCK_AIR;
	bo_batch(w, e, n, &pre, &post, "roof_remove");
	/* torch place, then removal, at the seam */
	e[0] = (struct bo_edit) {15, h0, 12, BLOCK_TORCH};
	e[1] = (struct bo_edit) {16, h0, 12, BLOCK_GLOWSTONE};
	bo_batch(w, e, 2, &pre, &post, "torch_place");
	e[0].id = BLOCK_AIR;
	e[1].id = BLOCK_AIR;
	bo_batch(w, e, 2, &pre, &post, "torch_remove");
	/* repeated changes of one column in one batch */
	n = 0;
	e[n++] = (struct bo_edit) {5, h0 + 6, 5, BLOCK_STONE};
	e[n++] = (struct bo_edit) {5, h0 + 2, 5, BLOCK_LEAVES};
	e[n++] = (struct bo_edit) {5, h0 + 6, 5, BLOCK_AIR};
	e[n++] = (struct bo_edit) {5, h0 + 9, 5, BLOCK_ICE};
	e[n++] = (struct bo_edit) {5, h0 + 2, 5, BLOCK_GLOWSTONE};
	e[n++] = (struct bo_edit) {5, h0 + 9, 5, BLOCK_STONE};
	e[n++] = (struct bo_edit) {5, h0 + 9, 5, BLOCK_AIR};
	bo_batch(w, e, n, &pre, &post, "same_column");
	/* random batches across both seams (x,z = 0 and 16) */
	static const uint8_t pal[] = {BLOCK_AIR, BLOCK_AIR, BLOCK_STONE,
								  BLOCK_STONE, BLOCK_GLASS, BLOCK_GLOWSTONE,
								  BLOCK_LEAVES, BLOCK_ICE};
	lcg_state = 0xB37A1u + (uint32_t)dim;
	for(int b = 0; b < 40; b++) {
		n = 1 + (int)(lcg() % 24u);
		for(int i = 0; i < n; i++) {
			e[i].x = -10 + (int)(lcg() % 36u);
			e[i].z = -10 + (int)(lcg() % 36u);
			e[i].y = h0 + (int)(lcg() % 16u);
			e[i].id = pal[lcg() % (sizeof pal)];
		}
		/* a torch on undisturbed ground (its support is never edited) */
		if(b % 3 == 0) {
			e[n].x = -10 + (int)(lcg() % 36u);
			e[n].z = -10 + (int)(lcg() % 36u);
			e[n].y = h0;
			e[n].id = BLOCK_TORCH;
			n++;
		}
		char t[32];
		snprintf(t, sizeof t, "random %d", b);
		bo_batch(w, e, n, &pre, &post, t);
	}
	printf("  %s: oracle relaxed %lu cells, merged %lu schedules; %ld "
		   "pre-drain / %ld settled cells differ (cavewii vs Beta)\n", name,
		   bo_relaxed, bo_merged, pre, post);
	char what[200];
	snprintf(what, sizeof what,
			 "%s: pre-drain AND settled Sky/Block/height of 25 chunks equal "
			 "the Beta 1.7.3 queue model (CFR), incl. seams, removals, "
			 "same-column rewrites, 40 random batches", name);
	check(pre == 0 && post == 0, what);
	close_world(s, s_dir[1], false);
}


/* ---- beta_oracle, generation overlap (ASTRA-G9-REVIEW-2 item 4) ----
 * Generated (non-superflat) Overworld and Nether, seed 4242, chunks -2..2
 * resident. A pending seam roof + torch/glowstone edit across the chunk 1|2
 * seam overlaps the generation of chunk (2,0) (its populate quadrants and
 * the synchronous populate relight / skylight bleed touch chunks 1..3,
 * ASTRA-G9-RULING 1-4: generation stays synchronous, never over pending
 * gameplay light). Required and checked:
 *   deferral: the provision request made while the edit is pending defers
 *     (no early drain: the seeds are still pending, no chunk changed);
 *   pre-drain: every Sky/Block/height cell of the 25 chunks equals the
 *     independent Beta model (immediate func_339_g writes only);
 *   boundary: in the PRODUCTION update the drain runs after World.tick;
 *     at its entity-phase start the settled field equals the Beta model
 *     drained at that boundary (25 chunks), the queue is empty;
 *   order: in the Nether a second (far-stream) populate requested while
 *     the first is light-deferred is held behind it (the order guard), so
 *     the shared population RNG (the Nether's hell_populate_rng: Beta
 *     ChunkProviderHell.populate never reseeds hellRNG) draws in request
 *     order;
 *   progress: the next provision requests after that boundary populate,
 *     the held column first;
 *   generation unchanged: every id / metadata / Sky / Block cell of chunks
 *     -2..3 and the population RNG equal a twin session that ran the same
 *     edits synchronously, the same update and the same populates in the
 *     same order (generation byte-exact; without the order guard the far
 *     populate would run first and the Nether's shared population RNG
 *     would diverge). */
static int go_phase_seen;
static long go_settled_diff = -1;
static size_t go_seeds_at_boundary;
static struct server_world* go_w;
static void go_probe(struct server_local* s, struct server_world* w,
					 int phase) {
	(void)s;
	if(w != go_w || phase != 1 || go_phase_seen)
		return;
	go_phase_seen = 1;
	go_seeds_at_boundary = w->light_seed_n;
	bo_drain();
	go_settled_diff = bo_diff(w, "gen-overlap settled");
}

static void go_session(int i, const char* id, enum world_dim dim) {
	struct server_local* s = &S[i];
	test_fixture_dir(id, s_dir[i], sizeof s_dir[i]);
	MKDIR(s_dir[i]);
	memset(s, 0, sizeof *s);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(s))
		abort();
	dict_entity_init(s->entities);
	clin_server_publication_stop(false);
	char region[260];
	snprintf(region, sizeof region, "%s/region", s_dir[i]);
	MKDIR(region);
	string_t level;
	string_init_set_str(level, s_dir[i]);
	server_world_create(&s->worlds[0], level, dim, 4242, false);
	string_clear(level);
	s->worlds[0].session = s;
	s->worlds[0].local = s;
	jrand_set_seed(&s->worlds[0].rand, 4242);
	s->scheduled_tick_budget = 128;
	s->rand_src.seed = 0x9E11u;
	s->world_time = 6000;
	server_local_skylight_recompute(s);
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s->worlds[0], cx, cz, &sc) && sc);
		}
	assert(server_world_light_drain(&s->worlds[0]));
	assert(inventory_create(&s->player.inventory, &inventory_logic_player, s,
							&s->player, INVENTORY_SIZE, 0, 0, 0));
	s->player.active_inventory = &s->player.inventory;
	/* the player far away: its own ring (random ticks, spawning) never
	 * reaches the compared chunks, so the twins can only differ by light
	 * timing or generation */
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.x = 4000.5;
	s->player.y = 120.0;
	s->player.z = 4000.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->render_demo_mob = -1;
}

static void go_edits(struct server_world* w, int top, bool model) {
	/* a roof across the x = 31|32 seam and a torch + glowstone at it */
	for(int x = 27; x < 37; x++)
		for(int z = 5; z < 10; z++) {
			(void)server_world_set_block(w, x, top + 4, z,
				(struct block_data) {.type = BLOCK_STONE});
			if(model)
				bo_set_block(x, top + 4, z, BLOCK_STONE);
		}
	(void)server_world_set_block(w, 31, top + 1, 13,
		(struct block_data) {.type = BLOCK_GLOWSTONE});
	(void)server_world_set_block(w, 32, top + 2, 12,
		(struct block_data) {.type = BLOCK_GLOWSTONE});
	if(model) {
		bo_set_block(31, top + 1, 13, BLOCK_GLOWSTONE);
		bo_set_block(32, top + 2, 12, BLOCK_GLOWSTONE);
	}
}

static void go_run(enum world_dim dim, const char* name) {
	char id0[64], id1[64];
	snprintf(id0, sizeof id0, "test_light_queue_go_sync_%s", name);
	snprintf(id1, sizeof id1, "test_light_queue_go_queue_%s", name);
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	go_session(0, id0, dim);
	go_session(1, id1, dim);
	struct server_world* w0 = &S[0].worlds[0];
	struct server_world* w1 = &S[1].worlds[0];
	/* a high point of the seam columns: the roof clears the terrain */
	int top = 1;
	for(int x = 27; x < 37; x++)
		for(int z = 5; z < 14; z++) {
			uint8_t hh = 0;
			assert(server_world_get_height(w1, x, z, &hh));
			if(hh > top)
				top = hh;
		}
	if(dim == WORLD_DIM_NETHER)
		top = 64; /* inside the Nether's open middle */
	bo_load(w1);
	bo_qn = 0;
	mode(0);
	go_edits(w0, top, false);
	mode(1);
	go_edits(w1, top, true);
	const size_t seeds = w1->light_seed_n;
	const long pre = bo_diff(w1, "gen-overlap pre");
	/* the provision request while the edit is pending */
	server_world_test_light_stats_reset();
	static uint8_t snap[48 * 48 * 128];
	(void)snapshot_diff(1, snap, true);
	struct server_chunk* sc = NULL;
	mode(1);
	const bool early = server_world_ensure_populated(w1, &S[1], 2, 0, &sc);
	struct server_world_light_stats st;
	server_world_light_stats(&st);
	/* a far-stream populate requested meanwhile, away from the pins */
	struct server_chunk* fsc = NULL;
	const bool far_early = server_world_ensure_populated_far(w1, &S[1], -5, -5,
															 &fsc);
	server_world_light_stats(&st);
	const bool deferred = !early && st.provision_deferred >= 1
		&& w1->light_seed_n == seeds && snapshot_diff(1, snap, false) == 0;
	/* the Nether holds it (shared population RNG); the Overworld reseeds
	 * per chunk, so its far populate proceeds at once */
	const bool held = dim == WORLD_DIM_NETHER ?
		(!far_early && st.gen_order_held >= 1) :
		(st.gen_order_held == 0);
	/* the no-deferral baseline twin: the same populates at request time, in
	 * request order (synchronous light, nothing pinned) */
	mode(0);
	bool twin_done = false, twin_far = false;
	for(int n = 0; n < 64 && !twin_done; n++)
		twin_done = server_world_ensure_populated(w0, &S[0], 2, 0, &sc);
	for(int n = 0; n < 64 && !twin_far; n++)
		twin_far = server_world_ensure_populated_far(w0, &S[0], -5, -5, &fsc);
	const uint64_t rng0 = w0->hell_populate_rng.seed;

	/* the production update: World.tick, the drain, then entities */
	go_w = w1;
	go_phase_seen = 0;
	go_settled_diff = -1;
	server_local_test_phase_hook = go_probe;
	mode(1);
	server_local_test_update_once(&S[1]);
	server_local_test_phase_hook = NULL;
	mode(0);
	server_local_test_update_once(&S[0]);
	/* the next provision request after the boundary progresses */
	mode(1);
	bool done = false, far_done = false;
	for(int n = 0; n < 64 && !done; n++)
		done = server_world_ensure_populated(w1, &S[1], 2, 0, &sc);
	for(int n = 0; n < 64 && !far_done; n++)
		far_done = server_world_ensure_populated_far(w1, &S[1], -5, -5, &fsc);
	const uint64_t rng1 = w1->hell_populate_rng.seed;
	struct server_chunk* p2 = dict_server_chunks_get(w1->chunks, S_CHUNK_ID(2, 0));
	const bool p2_populated = p2 && p2->populated;
	/* one more production update each: the population's own gameplay
	 * writes (Nether lava/fire scheduled ticks) settle at the queue twin's
	 * prescribed drain, exactly as the sync twin lit them at once */
	for(int i = 0; i < 2; i++) {
		mode(i);
		server_local_test_update_once(&S[i]);
	}
	/* EVICTION DIAGNOSTIC (evidence only, ASTRA-G9-RULING-2 item 18): the
	 * far player's streaming evicts (saves) compared columns at different
	 * points of the two histories; reload them to measure what that did.
	 * This residency-UNSTABLE run certifies no light value -- g2_run (the
	 * residency-stable fixture) asserts full light equality. */
	long evicted[2] = {0, 0};
	for(int i = 0; i < 2; i++) {
		mode(i);
		for(int cx = -2; cx <= 3; cx++)
			for(int cz = -2; cz <= 2; cz++) {
				struct server_chunk* lc = NULL;
				if(!server_world_is_chunk_loaded(&S[i].worlds[0], cx, cz)) {
					evicted[i]++;
					assert(server_world_load_chunk(&S[i].worlds[0], cx, cz,
												   &lc) && lc);
				}
			}
		/* a reloaded column's load-time gameplay writes (the fluid tick
		 * re-arm, the furnace fixup) light at the queue twin's next
		 * prescribed drain; compare at that settled boundary */
		assert(server_world_light_drain(&S[i].worlds[0]));
	}
	mode(1);
	/* generation unchanged: the twins, chunks -2..3 x -2..2 */
	long gen_diff = 0, light_info = 0;
	for(int x = -32; x < 64; x++)
		for(int z = -32; z < 48; z++)
			for(int y = 0; y < 128; y++) {
				struct block_data p = {0}, q = {0};
				bool pa = server_world_get_block(w0, x, y, z, &p);
				bool qa = server_world_get_block(w1, x, y, z, &q);
				light_info += p.sky_light != q.sky_light
					|| p.torch_light != q.torch_light;
				if(pa != qa || p.type != q.type || p.metadata != q.metadata) {
					if(gen_diff == 0)
						printf("    first twin diff (%d,%d,%d): id %d/%d meta "
							   "%d/%d sky %d/%d blk %d/%d (sync/queue)\n", x, y,
							   z, p.type, q.type, p.metadata, q.metadata,
							   p.sky_light, q.sky_light, p.torch_light,
							   q.torch_light);
					gen_diff++;
				}
			}
	const bool rng_same = rng0 == rng1;
	printf("  %s gen overlap: %zu seeds pending; provision deferred %d "
		   "(deferrals %u); pre-drain %ld cells differ from Beta; boundary seen "
		   "%d, seeds there %zu, settled %ld cells differ; populated after the "
		   "boundary %d (chunk flag %d), far held %d then %d, twin %d/%d; "
		   "twin ids/metadata differ in %ld cells (light %ld), population RNG "
		   "same %d\n", name, seeds,
		   deferred, st.provision_deferred, pre, go_phase_seen,
		   go_seeds_at_boundary, go_settled_diff, done,
		   (int)p2_populated, held, far_done, twin_done, twin_far,
		   gen_diff, light_info, rng_same);
	char what[240];
	snprintf(what, sizeof what,
			 "%s (eviction diagnostic): a pending seam edit defers the "
			 "overlapping generation (no early drain), pre-drain and boundary-"
			 "settled Sky/Block/height equal Beta, ids/metadata/RNG byte-exact "
			 "(light after eviction: evidence only, see g2)", name);
	printf("  %s EVICTION DIAGNOSTIC: compared columns evicted and reloaded "
		   "sync %ld / queue %ld; twin light cells differing after it %ld "
		   "(the Round 4 Nether evidence was 48634; NOT an allowed-error "
		   "threshold)\n", name, evicted[0], evicted[1], light_info);
	check(deferred && held && pre == 0 && go_phase_seen
			  && go_seeds_at_boundary == 0 && go_settled_diff == 0 && done
			  && far_done && twin_done && twin_far && gen_diff == 0 && rng_same,
		  what);
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_NORMAL);
	for(int i = 0; i < 2; i++) {
		inventory_destroy(&S[i].player.inventory, &S[i].player);
		close_world(&S[i], s_dir[i], false);
	}
}


/* ---- beta_oracle, residency-stable generation overlap (ASTRA-G9-RULING-2
 * items 14-18, option C) ----
 * Seed 4242, both dimensions. Explicit spawn: the player (and the world
 * spawn fields) at block (40, 70|h+2, 40), chunk (2,2), in the production
 * world-entry LOADING state (MinecraftServer:200-203 spawn preparation:
 * no World.tick, so no eviction, clock, random tick, spawn or far send;
 * every update drains, then the near-send streams). Unrelated activity is
 * thereby controlled; no production retention is added.
 * Domain: the whole near ring (radius MAX_VIEW_DISTANCE = 10 around chunk
 * (2,2): chunks -8..12) plus the populate neighbours it needs (-9..13) is
 * generated up front in a fixed request order in BOTH twins, so the
 * resident set is the same 23x23 chunks before, at and after both settle
 * points; the boundary ring (-10 / 14) is never resident. Both are
 * ASSERTED at every point.
 * Conflict: a pending seam roof across x = 31|32 plus glowstone in chunk
 * row z = 4 pins chunks 0..3 x 3..5, so the provision request for (2,2)
 * (the near-send's first pick) is light-deferred; the competing Nether
 * populate (2,1) (the near-send's second pick, its 3x3 clear of the pins)
 * is held by the order guard. The PRODUCTION update then drains (the
 * loading boundary) and the near-send retries (2,2), then (2,1), then the
 * ring nearest-first -- the same populate order as the synchronous twin,
 * which ran both requests at request time.
 * Model: the independent CFR model (bo_*) holds the 23x23 region. Its
 * initial state is the declared STRUCTURAL-PASS INPUT -- CaveWii's retained
 * generation output (RULING 1(a)), captured before any edit, NOT a Beta-
 * generated state (Beta's ChunkProviderHell builds unlit chunks). It takes
 * the edits, is compared pre-drain and at the first boundary, then takes
 * the population's block writes (ids from the synchronous twin, chunk by
 * chunk in populate order, drained per chunk) and is compared after the
 * final production drain. */
#define G2_C0 (-9)
#define G2_NC 23
static int g2_boundary_seen;
static long g2_boundary_diff = -1;
static size_t g2_boundary_seeds;
static struct server_world* g2_w;
static void g2_drain_probe(struct server_world* w) {
	if(w != g2_w || g2_boundary_seen)
		return;
	g2_boundary_seen = 1;
	bo_drain();
	g2_boundary_diff = bo_diff(w, "g2 boundary");
}

/* resident chunk identities: the 23x23 domain resident, its boundary ring
 * not; returns the number of violations */
static long g2_residency(int i) {
	long bad = 0;
	struct server_world* w = &S[i].worlds[0];
	for(int cx = G2_C0 - 1; cx <= G2_C0 + G2_NC; cx++)
		for(int cz = G2_C0 - 1; cz <= G2_C0 + G2_NC; cz++) {
			const bool inside = cx >= G2_C0 && cx < G2_C0 + G2_NC
				&& cz >= G2_C0 && cz < G2_C0 + G2_NC;
			bad += server_world_is_chunk_loaded(w, cx, cz) != inside;
		}
	return bad;
}

static void g2_session(int i, const char* id, enum world_dim dim) {
	struct server_local* s = &S[i];
	test_fixture_dir(id, s_dir[i], sizeof s_dir[i]);
	MKDIR(s_dir[i]);
	memset(s, 0, sizeof *s);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(s))
		abort();
	dict_entity_init(s->entities);
	clin_server_publication_stop(false);
	char region[260];
	snprintf(region, sizeof region, "%s/region", s_dir[i]);
	MKDIR(region);
	string_t level;
	string_init_set_str(level, s_dir[i]);
	server_world_create(&s->worlds[0], level, dim, 4242, false);
	string_clear(level);
	s->worlds[0].session = s;
	s->worlds[0].local = s;
	jrand_set_seed(&s->worlds[0].rand, 4242);
	s->scheduled_tick_budget = 128;
	s->rand_src.seed = 0x9E11u;
	s->world_time = 6000;
	server_local_skylight_recompute(s);
	/* R5 retained reconstruction/debt diagnostic: declared structural
	 * fixture, not normal Beta load semantics. Fixed x-major/z-minor order. */
	for(int cx = G2_C0; cx < G2_C0 + G2_NC; cx++)
		for(int cz = G2_C0; cz < G2_C0 + G2_NC; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_test_load_structural_chunk(&s->worlds[0], cx, cz, &sc) && sc);
			/* R5 fixture equivalence evidence, never a Beta expectation. */
			const char* prefix = getenv("CW_G9B_STRUCTURAL_DUMP");
			if(prefix && i == 0) {
				char path[512];
				snprintf(path, sizeof path, "%s-%d.bin", prefix, (int)dim);
				FILE* f = fopen(path, cx == G2_C0 && cz == G2_C0 ? "wb" : "ab");
				assert(f);
				assert(fwrite(sc->ids, 1, 32768, f) == 32768);
				assert(fwrite(sc->metadata, 1, 16384, f) == 16384);
				assert(fwrite(sc->lighting_sky, 1, 16384, f) == 16384);
				assert(fwrite(sc->lighting_torch, 1, 16384, f) == 16384);
				assert(fwrite(sc->heightmap, 1, 256, f) == 256);
				const uint8_t state[] = {sc->populated, sc->sent,
					server_world_is_chunk_loaded(&s->worlds[0], cx, cz)};
				assert(fwrite(state, 1, sizeof state, f) == sizeof state);
				assert(fwrite(&s->worlds[0].hell_populate_rng.seed, 8, 1, f) == 1);
				assert(fwrite(&s->rand_src.seed, 4, 1, f) == 1);
				assert(fclose(f) == 0);
			}
		}
	/* Settled structural input for this retained diagnostic (R5/R7), never
	 * an original-JAR generation expectation. */
	assert(server_world_light_drain(&s->worlds[0]));
	assert(inventory_create(&s->player.inventory, &inventory_logic_player, s,
							&s->player, INVENTORY_SIZE, 0, 0, 0));
	s->player.active_inventory = &s->player.inventory;
	int py = 70;
	if(dim != WORLD_DIM_NETHER) {
		uint8_t hh = 0;
		assert(server_world_get_height(&s->worlds[0], 40, 40, &hh));
		py = hh + 2;
	}
	s->player.spawn_x = 40;
	s->player.spawn_y = py;
	s->player.spawn_z = 40;
	s->player.has_pos = true;
	s->player.finished_loading = false; /* world entry: loading */
	s->player.x = 40.5;
	s->player.y = py;
	s->player.z = 40.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->render_demo_mob = -1;
}

static void g2_edits(struct server_world* w, int top, bool model) {
	/* a roof across the x = 31|32 seam and glowstone, chunk row z = 4 */
	for(int x = 27; x < 37; x++)
		for(int z = 69; z < 74; z++) {
			(void)server_world_set_block(w, x, top + 4, z,
				(struct block_data) {.type = BLOCK_STONE});
			if(model)
				bo_set_block(x, top + 4, z, BLOCK_STONE);
		}
	(void)server_world_set_block(w, 31, top + 1, 77,
		(struct block_data) {.type = BLOCK_GLOWSTONE});
	(void)server_world_set_block(w, 32, top + 2, 76,
		(struct block_data) {.type = BLOCK_GLOWSTONE});
	if(model) {
		bo_set_block(31, top + 1, 77, BLOCK_GLOWSTONE);
		bo_set_block(32, top + 2, 76, BLOCK_GLOWSTONE);
	}
}

static uint64_t g2_hash(struct server_world* w) {
	uint64_t h = 1469598103934665603ULL;
	for(int x = 0; x < 64; x++)
		for(int z = 16; z < 96; z++)
			for(int y = 0; y < 128; y++) {
				struct block_data b = {0};
				(void)server_world_get_block(w, x, y, z, &b);
				h = (h ^ b.type) * 1099511628211ULL;
				h = (h ^ b.metadata) * 1099511628211ULL;
				h = (h ^ (uint8_t)(b.sky_light | b.torch_light << 4))
					* 1099511628211ULL;
			}
	return h;
}

static bool g2_all_sent(struct server_world* w) {
	for(int cx = 2 - 10; cx <= 2 + 10; cx++)
		for(int cz = 2 - 10; cz <= 2 + 10; cz++)
			if(!server_world_is_chunk_sent(w, cx, cz))
				return false;
	return true;
}

/* Which claim a g2_run certifies (ASTRA-G9-RULING-3 items 9-11). The
 * fixture is the same; each claim is its own registered test:
 *   G2_TWINS  -- beta_oracle (gated): residency, deferral/hold, production
 *                progress, pre-drain and first-boundary CFR equality, zero
 *                twin differences and equal population RNG;
 *   G2_PARITY -- legacy claim name (card G9-OPTION-B): post-population
 *                equality to the CFR model (the registered
 *                generation_beta_parity now runs the original-JAR history);
 *   G2_DEBT   -- current reconstruction diagnostic (R7): the recorded
 *                mismatch record of that comparison. */
enum g2_claim { G2_TWINS, G2_PARITY, G2_DEBT };
static void g2_debt_record(enum world_dim dim, struct server_world* w);

static void g2_run(enum world_dim dim, const char* name, enum g2_claim claim) {
	char id0[64], id1[64];
	snprintf(id0, sizeof id0, "test_light_queue_g2_sync_%s", name);
	snprintf(id1, sizeof id1, "test_light_queue_g2_queue_%s", name);
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	mode(0);
	g2_session(0, id0, dim);
	mode(1);
	g2_session(1, id1, dim);
	struct server_world* w0 = &S[0].worlds[0];
	struct server_world* w1 = &S[1].worlds[0];
	long res_bad = g2_residency(0) + g2_residency(1);
	int top = 1;
	for(int x = 27; x < 37; x++)
		for(int z = 69; z < 78; z++) {
			uint8_t hh = 0;
			assert(server_world_get_height(w1, x, z, &hh));
			if(hh > top)
				top = hh;
		}
	if(dim == WORLD_DIM_NETHER)
		top = 64; /* inside the Nether's open middle */
	/* the model: the declared structural-pass input, before any edit */
	bo_region(G2_C0, G2_NC);
	bo_load(w1);
	bo_qn = 0;
	const uint64_t rng_gen0 = w0->hell_populate_rng.seed;
	const uint64_t rng_gen1 = w1->hell_populate_rng.seed;
	mode(0);
	g2_edits(w0, top, false);
	mode(1);
	g2_edits(w1, top, true);
	const size_t seeds = w1->light_seed_n;
	const long pre = bo_diff(w1, "g2 pre");
	/* the conflicting seam request and the competing populate, while the
	 * edit is pending */
	server_world_test_light_stats_reset();
	const uint64_t h_before = g2_hash(w1);
	struct server_chunk* sc = NULL;
	mode(1);
	const bool early = server_world_ensure_populated(w1, &S[1], 2, 2, &sc);
	struct server_world_light_stats st;
	server_world_light_stats(&st);
	bool deferred = !early && st.provision_deferred >= 1
		&& w1->light_seed_n == seeds && g2_hash(w1) == h_before
		&& w1->hell_populate_rng.seed == rng_gen1;
	/* the competing Nether populate (the shared hellRNG makes its order
	 * observable; the Overworld reseeds per chunk and has none) */
	const bool nether = dim == WORLD_DIM_NETHER;
	struct server_chunk* fsc = NULL;
	bool held = true;
	if(nether) {
		const bool far_early
			= server_world_ensure_populated_far(w1, &S[1], 2, 1, &fsc);
		server_world_light_stats(&st);
		held = !far_early && st.gen_order_held >= 1;
		deferred = deferred && w1->light_seed_n == seeds
			&& g2_hash(w1) == h_before
			&& w1->hell_populate_rng.seed == rng_gen1;
	}
	/* the unchanged synchronous twin: both requests at request time */
	mode(0);
	const bool twin_done = server_world_ensure_populated(w0, &S[0], 2, 2, &sc);
	const bool twin_far = !nether
		|| server_world_ensure_populated_far(w0, &S[0], 2, 1, &fsc);
	res_bad += g2_residency(0) + g2_residency(1);
	/* PRODUCTION: update 1 drains at the loading boundary (the probe
	 * compares the model there), then the near-send retries (2,2) and
	 * (2,1) and streams the ring; both twins run until every ring chunk is
	 * sent */
	g2_w = w1;
	g2_boundary_seen = 0;
	g2_boundary_diff = -1;
	server_world_test_light_drain_hook = g2_drain_probe;
	int updates[2] = {0, 0};
	bool p22_after_1 = false, p21_after_1 = false;
	/* REGRESS-1015: a fixed near-send cadence (four columns per update, as
	 * test_tnt_primed_env / g9x_publication pin it) instead of the 12 ms
	 * wall-clock batch budget, so how the twins' populations group into
	 * updates is not a host-timing input either (measured: with only the
	 * synchronous drain below, the Overworld twins ran 114..121 updates and
	 * one tall-grass cell at (83,80,-3) differed in some runs, with the
	 * G9X-6 refresh disabled too). */
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	for(int i = 1; i >= 0; i--) {
		mode(i);
		while(updates[i] < 2000 && !g2_all_sent(&S[i].worlds[0])) {
			server_local_test_update_once(&S[i]);
			/* REGRESS-1015: the client consumes this update's output
			 * before the next update, for both twins. The background
			 * discard thread alone (1 ms sleeps, ~15.6 ms on Windows)
			 * made the update COUNT a function of host scheduling: since
			 * G9X-6 an update also republishes changed sent columns on
			 * the shared 16-slot chunk pool, so with the consumer behind,
			 * the following updates found the pool empty and spun
			 * (pool-busy, nothing sent) until both twins hit the 2000
			 * cap with unsent ring columns. Same uncongested-transport
			 * precondition as test_tnt_primed_env; no assertion
			 * changed. */
			drain_discard_locked();
			updates[i]++;
			if(i == 1 && updates[i] == 1) {
				server_world_test_light_drain_hook = NULL;
				res_bad += g2_residency(1);
				g2_boundary_seeds = w1->light_seed_n;
				struct server_chunk* a
					= dict_server_chunks_get(w1->chunks, S_CHUNK_ID(2, 2));
				struct server_chunk* b
					= dict_server_chunks_get(w1->chunks, S_CHUNK_ID(2, 1));
				p22_after_1 = a && a->populated;
				p21_after_1 = b && b->populated;
			}
		}
		/* one more update: the population's prescribed drain */
		server_local_test_update_once(&S[i]);
		res_bad += g2_residency(i);
	}
	server_local_test_set_near_send_minimum(0);
	server_local_test_expire_near_budget(false);
	server_world_test_light_drain_hook = NULL;
	const uint64_t rng0 = w0->hell_populate_rng.seed;
	const uint64_t rng1 = w1->hell_populate_rng.seed;
	/* evidence only: end-state identity of each twin (REGRESS-1015 compares
	 * it with and without the G9X-6 refresh) */
	printf("    g2 end state: hellRNG %016" PRIx64 "/%016" PRIx64
		   " sample hash %016" PRIx64 "/%016" PRIx64 " (sync/queue)\n",
		   rng0, rng1, g2_hash(w0), g2_hash(w1));
	/* the twins: every id / metadata / Sky / Block / height of the domain */
	long twin_diff = 0;
	for(int x = G2_C0 * 16; x < (G2_C0 + G2_NC) * 16; x++)
		for(int z = G2_C0 * 16; z < (G2_C0 + G2_NC) * 16; z++) {
			uint8_t h0 = 0, h1 = 0;
			bool ha = server_world_get_height(w0, x, z, &h0);
			bool hb = server_world_get_height(w1, x, z, &h1);
			if(ha != hb || h0 != h1) {
				if(twin_diff == 0)
					printf("    g2 first twin diff: height (%d,%d) %d/%d\n", x,
						   z, h0, h1);
				twin_diff++;
			}
			for(int y = 0; y < 128; y++) {
				struct block_data p = {0}, q = {0};
				bool pa = server_world_get_block(w0, x, y, z, &p);
				bool qa = server_world_get_block(w1, x, y, z, &q);
				if(pa != qa || p.type != q.type || p.metadata != q.metadata
				   || p.sky_light != q.sky_light
				   || p.torch_light != q.torch_light) {
					if(twin_diff == 0)
						printf("    g2 first twin diff (%d,%d,%d): id %d/%d "
							   "meta %d/%d sky %d/%d blk %d/%d (sync/queue)\n",
							   x, y, z, p.type, q.type, p.metadata, q.metadata,
							   p.sky_light, q.sky_light, p.torch_light,
							   q.torch_light);
					twin_diff++;
				}
			}
		}
	/* the model through the population: the synchronous twin's block writes,
	 * chunk by chunk in the near-send's order (nearest to chunk (2,2)
	 * first), each chunk's writes drained like Beta's next boundary */
	long writes = 0;
	for(int d = 0; d <= 2 * G2_NC; d++)
		for(int cz = G2_C0; cz < G2_C0 + G2_NC; cz++)
			for(int cx = G2_C0; cx < G2_C0 + G2_NC; cx++) {
				if((cx - 2) * (cx - 2) + (cz - 2) * (cz - 2) < d * d
				   || (cx - 2) * (cx - 2) + (cz - 2) * (cz - 2)
					   >= (d + 1) * (d + 1))
					continue;
				for(int x = cx * 16; x < cx * 16 + 16; x++)
					for(int z = cz * 16; z < cz * 16 + 16; z++)
						for(int y = 0; y < 128; y++) {
							struct block_data p = {0};
							(void)server_world_get_block(w0, x, y, z, &p);
							if(p.type != bo_get_id(x, y, z)) {
								bo_set_block(x, y, z, p.type);
								writes++;
							}
						}
				bo_drain();
			}
	const long post = bo_diff(w1, "g2 post-population");
	if(getenv("G9_G2_DIAG")) {
		long by_rim[12] = {0}, up = 0, down = 0, sky_d = 0;
		const int lo = G2_C0 * 16, hi = (G2_C0 + G2_NC) * 16 - 1;
		for(int x = lo; x <= hi; x++)
			for(int z = lo; z <= hi; z++)
				for(int y = 0; y < 128; y++) {
					struct block_data b = {0};
					(void)server_world_get_block(w1, x, y, z, &b);
					const int ms = bo_get_saved(BO_SKY, x, y, z);
					const int mb = bo_get_saved(BO_BLOCK, x, y, z);
					if(b.sky_light == ms && b.torch_light == mb)
						continue;
					int r = x - lo;
					if(hi - x < r) r = hi - x;
					if(z - lo < r) r = z - lo;
					if(hi - z < r) r = hi - z;
					by_rim[r > 11 ? 11 : r]++;
					static int shown_in;
					if(r >= 11 && shown_in++ < 8)
						printf("    G2DIAG interior (%d,%d,%d) id %d sky %d/%d blk "
							   "%d/%d (cavewii/model)\n", x, y, z, b.type,
							   b.sky_light, ms, b.torch_light, mb);
					if(b.sky_light != ms)
						sky_d++;
					else if(b.torch_light > mb)
						up++;
					else
						down++;
				}
		for(int r = 0; r < 12; r++)
			printf("    G2DIAG rim distance %d%s: %ld\n", r, r == 11 ? "+" : "",
				   by_rim[r]);
		printf("    G2DIAG sky %ld, block higher in CaveWii %ld, lower %ld\n",
			   sky_d, up, down);
	}
	const bool rng_same = rng0 == rng1 && rng0 != rng_gen0;
	printf("  %s g2: spawn (40,%d,40) chunk (2,2), domain chunks %d..%d "
		   "resident in both, ring %d/%d not (violations %ld); %zu seeds "
		   "pending; deferred %d (deferrals %u), competing held %d "
		   "(held %u); pre-drain %ld cells differ from the model; boundary "
		   "seen %d, seeds there %zu, settled %ld differ; after update 1 "
		   "(2,2) populated %d, (2,1) %d; updates %d/%d; twin id/meta/sky/"
		   "block/height diffs %ld; population RNG same %d; model took %ld "
		   "population writes, post-population %ld cells differ\n",
		   name, S[1].player.spawn_y, G2_C0, G2_C0 + G2_NC - 1, G2_C0 - 1,
		   G2_C0 + G2_NC, res_bad, seeds, deferred, st.provision_deferred,
		   held, st.gen_order_held, pre, g2_boundary_seen, g2_boundary_seeds,
		   g2_boundary_diff, p22_after_1, p21_after_1, updates[0], updates[1],
		   twin_diff, rng_same, writes, post);
	char what[300];
	if(claim == G2_DEBT)
		g2_debt_record(dim, w1);
	if(claim == G2_TWINS) {
	snprintf(what, sizeof what,
			 "%s: residency-stable overlap -- identical resident chunks at "
			 "every point; deferral (no early drain) and the competing "
			 "populate held; the PRODUCTION update settles at the boundary "
			 "and retries (2,2) then (2,1)", name);
	check(res_bad == 0 && seeds > 0 && deferred && held && twin_done
			  && twin_far && pre == 0 && g2_boundary_seen
			  && g2_boundary_seeds == 0 && g2_boundary_diff == 0
			  && p22_after_1 && p21_after_1 && updates[1] < 2000,
		  what);
	snprintf(what, sizeof what,
			 "%s: after population and its production drain, ZERO "
			 "id/meta/Sky/Block/height differences against the synchronous "
			 "twin over all 23x23 chunks, population RNG equal", name);
	check(twin_diff == 0 && rng_same == (dim == WORLD_DIM_NETHER)
			  && rng0 == rng1,
		  what);
	}
	if(claim == G2_PARITY) {
		snprintf(what, sizeof what,
				 "%s: post-population Sky/Block/height equal the independent "
				 "CFR model (structural-pass input declared, never re-"
				 "initialised from the candidate) [card G9-OPTION-B]", name);
		check(post == 0, what);
	}
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_NORMAL);
	bo_region(-2, 5);
	for(int i = 0; i < 2; i++) {
		inventory_destroy(&S[i].player.inventory, &S[i].player);
		close_world(&S[i], s_dir[i], false);
	}
}

static void case_beta_oracle(void) {
	const char* mu = getenv("G9_BO_MUTATE");
	bo_mutate = mu ? atoi(mu) : 0;
	bo_scenarios(WORLD_DIM_OVERWORLD, "overworld");
	bo_scenarios(WORLD_DIM_NETHER, "nether");
	go_run(WORLD_DIM_OVERWORLD, "overworld");
	go_run(WORLD_DIM_NETHER, "nether");
	g2_run(WORLD_DIM_OVERWORLD, "overworld", G2_TWINS);
	g2_run(WORLD_DIM_NETHER, "nether", G2_TWINS);
	free(bo_q);
	bo_q = NULL;
	bo_qcap = 0;
}

/* Card G9-OPTION-B parity gate. Until G9X-7 (2026-10-15) it carried the
 * known_fail label (ASTRA-G9-RULING-3 items 9-10); it now passes the full
 * original-JAR history exactly (history_case.h) and is an ordinary blocking
 * test. The equality assertions and their failing exit are unchanged. */
static void case_generation_beta_parity_history(void);
static void case_generation_beta_parity(void) {
	/* R7: original-JAR API-kind-preserving ordered history. The old final-ID
	 * spatial reconstruction remains a separate historical diagnostic. */
	case_generation_beta_parity_history();
}

/* ---- current reconstruction diagnostic (ASTRA-G9B R7) ----
 * Final-ID reconstruction remains diagnostic evidence, never Beta history.
 * G9_DEBT_DUMP=<file> writes the CURRENT comparison for inspection. This no
 * longer freezes changing production. The mandatory historical integrity /
 * reproduction gate runs the pinned pre-G9B-3 producer in isolation instead.
 * Record grammar (one line each, '\n'-terminated, ASCII):
 *   header lines (seed, domain, residency, ordering assumptions, below)
 *   "D <dim>" per dimension (0 Overworld, then -1 Nether)
 *   per column x ascending, then z ascending (whole 23x23-chunk domain):
 *     "H x z cand model"          the heightmap value differs
 *     then y = 0..127: "I x y z cand model" (block id), "S ..." (Sky),
 *     "B ..." (Block light), each only where it differs.
 * Metadata is not modelled by bo_* (light does not depend on it); the
 * zero-difference twin claim (beta_oracle) covers metadata. */
static const char* const G2_DEBT_HEADER =
	"G9 generation parity debt record v1\n"
	"seed 4242; dims Overworld then Nether\n"
	"domain chunks -9..13 x -9..13 (23x23), ring -10/14 absent; resident set "
	"asserted identical in both twins before/at/after both settle points\n"
	"spawn (40,y,40) chunk (2,2), production world-entry loading state, "
	"near radius MAX_VIEW_DISTANCE=10\n"
	"edits: stone roof x 27..36 z 69..73 y top+4, glowstone (31,top+1,77) "
	"(32,top+2,76); top 64 Nether, max seam height Overworld\n"
	"requests: ensure_populated(2,2) then (Nether) ensure_populated_far(2,1); "
	"then production updates until the radius-10 ring is sent, +1 update\n"
	"model: CFR bo_* with Beta Block.lightOpacity/lightValue; initial state = "
	"CaveWii generation output before the edits (declared structural input, "
	"NOT Beta generation); edits; first boundary drain; then population ids "
	"from the SYNC twin applied per chunk in distance-ring order from chunk "
	"(2,2), z then x within a ring, y ascending, drained after each chunk "
	"(a reconstruction, not Beta's execution history: RULING-3 item 7)\n";
static cw_sha256_ctx g2_debt_sha;
static FILE* g2_debt_dump;
static long g2_debt_n[2][4]; /* [dim][H, I, S, B] */
static void g2_debt_line(const char* line) {
	cw_sha256_update(&g2_debt_sha, line, strlen(line));
	if(g2_debt_dump)
		fputs(line, g2_debt_dump);
}
static void g2_debt_record(enum world_dim dim, struct server_world* w) {
	const int d = dim == WORLD_DIM_NETHER ? 1 : 0;
	char line[96];
	snprintf(line, sizeof line, "D %d\n", (int)dim);
	g2_debt_line(line);
	for(int x = G2_C0 * 16; x < (G2_C0 + G2_NC) * 16; x++)
		for(int z = G2_C0 * 16; z < (G2_C0 + G2_NC) * 16; z++) {
			uint8_t hh = 0;
			assert(server_world_get_height(w, x, z, &hh));
			if(hh != bo_height(x, z)) {
				snprintf(line, sizeof line, "H %d %d %d %d\n", x, z, hh,
						 bo_height(x, z));
				g2_debt_line(line);
				g2_debt_n[d][0]++;
			}
			for(int y = 0; y < 128; y++) {
				struct block_data b = {0};
				assert(server_world_get_block(w, x, y, z, &b));
				const int mi = bo_get_id(x, y, z);
				const int ms = bo_get_saved(BO_SKY, x, y, z);
				const int mb = bo_get_saved(BO_BLOCK, x, y, z);
				if(b.type != mi) {
					snprintf(line, sizeof line, "I %d %d %d %d %d\n", x, y,
							 z, b.type, mi);
					g2_debt_line(line);
					g2_debt_n[d][1]++;
				}
				if(b.sky_light != ms) {
					snprintf(line, sizeof line, "S %d %d %d %d %d\n", x, y,
							 z, b.sky_light, ms);
					g2_debt_line(line);
					g2_debt_n[d][2]++;
				}
				if(b.torch_light != mb) {
					snprintf(line, sizeof line, "B %d %d %d %d %d\n", x, y,
							 z, b.torch_light, mb);
					g2_debt_line(line);
					g2_debt_n[d][3]++;
				}
			}
		}
}

static void case_generation_reconstruction_diagnostic(void) {
	const char* mu = getenv("G9_BO_MUTATE"); /* sensitivity check only */
	bo_mutate = mu ? atoi(mu) : 0;
	cw_sha256_init(&g2_debt_sha);
	memset(g2_debt_n, 0, sizeof g2_debt_n);
	const char* dump = getenv("G9_DEBT_DUMP");
	g2_debt_dump = dump ? fopen(dump, "wb") : NULL;
	g2_debt_line(G2_DEBT_HEADER);
	g2_run(WORLD_DIM_OVERWORLD, "overworld", G2_DEBT);
	g2_run(WORLD_DIM_NETHER, "nether", G2_DEBT);
	if(g2_debt_dump)
		fclose(g2_debt_dump);
	g2_debt_dump = NULL;
	char hex[CW_SHA256_HEX_LENGTH + 1];
	cw_sha256_final_hex(&g2_debt_sha, hex);
	printf("  debt record sha256 %s\n", hex);
	for(int d = 0; d < 2; d++) {
		printf("  debt %s: height %ld, id %ld, sky %ld, block %ld (total "
			   "cells with a light diff are reported by generation_beta_"
			   "parity)\n", d ? "nether" : "overworld", g2_debt_n[d][0],
			   g2_debt_n[d][1], g2_debt_n[d][2], g2_debt_n[d][3]);
	}
	printf("  DIAGNOSTIC ONLY: final-ID reconstruction, not Beta execution "
		   "history or the historical debt integrity/reproduction gate\n");
	free(bo_q);
	bo_q = NULL;
	bo_qcap = 0;
}


/* ---- consumers (ASTRA-G9-REVIEW Required 4: a real entity consumer) ----
 * EntityMob.onLivingUpdate reads getEntityBrightness and ages a bright mob
 * by 2 extra (CFR EntityMob.java:35-41 onLivingUpdate: brightness above 0.5
 * adds two to age; CaveWii entity_monster.c bright bonus). At midnight a
 * creeper's torso cell is dark; a glowstone beside it lights it to 14.
  * (creeper at (12.5, h0, 12.5), glowstone at (13, h0+1, 12))
 *   packet : glowstone written before the update (Beta: the previous
 *            doTick's packet phase) -> settled by this update's drain ->
 *            the creeper's own entity tick sees 14: age +3.
 *   entity : glowstone written at the start of the entity pass (an earlier
 *            entity's write) -> Beta keeps it pending through updateEntities
 *            -> the creeper sees the dark cell: age +1; the next update's
 *            drain lights it: age +3.
 * The synchronous pre-G9 path gives +3 in the entity case (it lit the cell
 * at the write): the check proves the production entity consumer observes
 * Beta's phase, not just that a hook reads the field. */
static int cs_mode; /* 0 none, 1 write in the entity phase */
static int cs_h0;
static void cs_probe(struct server_local* s, struct server_world* w,
					 int phase) {
	if(cs_mode == 1 && w == &s->worlds[0] && phase == 1) {
		cs_mode = 0;
		(void)server_world_set_block(w, 13, cs_h0 + 1, 12,
			(struct block_data) {.type = BLOCK_GLOWSTONE});
	}
}

static int cs_run(bool entity_phase, bool sync, int* second) {
	struct server_local* s = &S[1];
	open_world(s, s_dir[1], sizeof s_dir[1], "test_light_queue_consumers",
			   true, 2); /* updateEntities needs +-32 blocks loaded */
	server_world_test_set_light_sync(sync);
	server_world_test_set_light_lifo(false);
	assert(inventory_create(&s->player.inventory, &inventory_logic_player, s,
							&s->player, INVENTORY_SIZE, 0, 0, 0));
	s->player.active_inventory = &s->player.inventory;
	uint8_t hh = 0;
	assert(server_world_get_height(&s->worlds[0], 8, 8, &hh));
	cs_h0 = hh;
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.x = 1.5;
	s->player.y = cs_h0 + (double)PLAYER_EYE_HEIGHT;
	s->player.z = 1.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->render_demo_mob = -1;
	s->world_time = 18000; /* midnight: skylightSubtracted 11 */
	server_local_skylight_recompute(s);
	vec3 cpos = {12.5F, (float)cs_h0, 12.5F};
	struct entity* c = server_local_spawn_monster_at(cpos, MONSTER_CREEPER, s);
	assert(c);
	const uint32_t cid = c->id;
	server_local_test_update_once(s);
	(void)server_world_light_drain(&s->worlds[0]);
	struct entity** pp = dict_entity_get(s->entities, cid);
	assert(pp && *pp);
	/* pin the creeper so its sample cell stays (20, h0+1, 20) */
	(*pp)->pos[0] = 12.5F;
	(*pp)->pos[2] = 12.5F;
	int a0 = (*pp)->data.monster.entity_age;
	if(!entity_phase)
		assert(server_world_set_block(&s->worlds[0], 13, cs_h0 + 1, 12,
			(struct block_data) {.type = BLOCK_GLOWSTONE}));
	cs_mode = entity_phase ? 1 : 0;
	server_local_test_phase_hook = cs_probe;
	server_local_test_update_once(s);
	pp = dict_entity_get(s->entities, cid);
	assert(pp && *pp);
	int a1 = (*pp)->data.monster.entity_age;
	(*pp)->pos[0] = 12.5F;
	(*pp)->pos[2] = 12.5F;
	server_local_test_update_once(s);
	server_local_test_phase_hook = NULL;
	pp = dict_entity_get(s->entities, cid);
	assert(pp && *pp);
	*second = (*pp)->data.monster.entity_age - a1;
	inventory_destroy(&s->player.inventory, &s->player);
	close_world(s, s_dir[1], false);
	server_world_test_set_light_sync(false);
	return a1 - a0;
}


/* ---- consumers, ASTRA-G9-REVIEW-2 item 3: production consumer traces ----
 * Beta per world [CFR MinecraftServer.java:346-349; World.tick:1346-1375;
 * World.updateEntities]: tick() = SpawnerAnimals.performSpawning (natural
 * spawning: EntityMob.getCanSpawnHere reads getSavedLightValue(Sky) and
 * getBlockLightValue, EntityMob.java:91-102), TickUpdates (scheduled ticks:
 * here a redstone torch turning off, BlockRedstoneTorch.updateTick), the
 * random ticks (BlockGrass.updateTick reads getBlockLightValue(x,y+1,z));
 * then func_6156_d drains the light queue until empty; then updateEntities:
 * entities (EntityMob.onLivingUpdate getEntityBrightness; the falling sand's
 * first tick setBlockWithNotify(0), EntityFallingSand.onUpdate, and its
 * landing), then tile entities (TileEntityMobSpawner.updateEntity ->
 * getCanSpawnHere; TileEntityFurnace ignition/extinction ->
 * BlockFurnace.updateFurnaceBlockState; TileEntityPiston completion ->
 * setBlockWithNotify of the moved glowstone).
 * The test drives PRODUCTION updates with real producers in every phase:
 *   packet : SRPC_BLOCK_PLACE torches (the previous doTick's network phase)
 *   block  : a lit redstone torch turned off by a lever (packet) at its
 *            scheduled tick (TickUpdates)
 *   entity : falling sand leaving its cell and landing beside a torch
 *   tile   : a furnace igniting, another going out, a piston completing a
 *            glowstone push
 * and traces EVERY consumer light read the production code makes (natural
 * spawn candidates incl. their two Java-Random rolls, grass random ticks,
 * mob brightness, dungeon spawner members). The expected trace is an
 * independent Beta model (bo_*, CFR Chunk/World/MetadataChunkBlock) fed the
 * same writes in the same order and drained ONLY at Beta's boundary
 * (after World.tick, before updateEntities). Every traced read must equal
 * the model at that instant; every spawn candidate's decision and Random
 * state must equal Beta's formula replayed on an independent
 * java.util.Random; and each consumer class must read cells whose light was
 * still pending (a different drain point would change the value read). */
static struct server_world* c3_w;
static int c3_slot;
static int c3_window; /* 0 World.tick (pre-drain), 1 entities, 2 tiles */
static long c3_reads[3], c3_bad[3], c3_pending_hits[3];
static long c3_spawn_rolls, c3_spawn_bad, c3_spawn_pending;
static long c3_spawner_reads, c3_spawner_bad, c3_spawner_pending;
static long c3_out_region;
static int c3_mutate; /* G9_CS_MUTATE=1: the model drains BEFORE World.tick */
/* cells whose value differs between the model now and the model drained */
static uint8_t c3_pend[BO_NMAX][BO_NMAX][128];
static int c3_last_light = -1;
static bool c3_dirty; /* a write after the window's pending set was taken */
/* the piston completion trace (ASTRA-G9-FINAL item 1): the tick whose tile
 * phase wrote the settled glowstone, its light pending after that update,
 * still pending at the next World.tick, settled by the next drain */
static int c3_t = -1, c3_glow_tick = -1, c3_glow_y;
static int c3_glow_pre_blk = -1, c3_glow_pre_nb = -1;
static bool c3_glow_pending_after, c3_glow_pending_tick, c3_glow_drained;

static bool c3_in_region(int x, int z) {
	return x >= BO_MIN + 16 && x < BO_MIN + BO_N - 16 && z >= BO_MIN + 16
		&& z < BO_MIN + BO_N - 16;
}
static bool c3_in_model(int x, int z) {
	return x >= BO_MIN && x < BO_MIN + BO_N && z >= BO_MIN && z < BO_MIN + BO_N;
}

/* the settled view of the current model, as the cells it would change */
static void c3_compute_pending(void) {
	static uint8_t sky[BO_NMAX][BO_NMAX][128], blk[BO_NMAX][BO_NMAX][128];
	static uint8_t h[BO_NMAX][BO_NMAX];
	for(int x = 0; x < BO_N; x++) {
		memcpy(sky[x], bo_sky[x], (size_t)BO_N * 128);
		memcpy(blk[x], bo_blk[x], (size_t)BO_N * 128);
		memcpy(h[x], bo_h[x], (size_t)BO_N);
	}
	struct bo_mcb* q = NULL;
	const size_t qn = bo_qn;
	if(qn) {
		q = malloc(qn * sizeof *q);
		assert(q);
		memcpy(q, bo_q, qn * sizeof *q);
	}
	const unsigned long relaxed = bo_relaxed, merged = bo_merged;
	const int mut = bo_mutate;
	bo_mutate = 0;
	bo_drain();
	bo_mutate = mut;
	for(int x = 0; x < BO_N; x++)
		for(int z = 0; z < BO_N; z++)
			for(int y = 0; y < 128; y++)
				c3_pend[x][z][y] = bo_sky[x][z][y] != sky[x][z][y]
					|| bo_blk[x][z][y] != blk[x][z][y];
	for(int x = 0; x < BO_N; x++) {
		memcpy(bo_sky[x], sky[x], (size_t)BO_N * 128);
		memcpy(bo_blk[x], blk[x], (size_t)BO_N * 128);
		memcpy(bo_h[x], h[x], (size_t)BO_N);
	}
	if(qn) {
		if(bo_qcap < qn) {
			bo_q = realloc(bo_q, qn * sizeof *bo_q);
			bo_qcap = qn;
		}
		memcpy(bo_q, q, qn * sizeof *q);
	}
	bo_qn = qn;
	bo_relaxed = relaxed;
	bo_merged = merged;
	free(q);
}

static int c3_cell(int x, int y, int z, int sub) {
	if(y < 0)
		return 0;
	if(y >= 128)
		y = 127;
	if(!bo_chunk_exists(bo_fdiv16(x), bo_fdiv16(z)))
		return 0;
	int v = bo_get_saved(BO_SKY, x, y, z) - sub;
	int b = bo_get_saved(BO_BLOCK, x, y, z);
	return b > v ? b : v;
}
/* World.getBlockLightValue_do [World.java:537-583] over the model */
static int c3_expected(int x, int y, int z, int sub) {
	uint8_t id = (y >= 0 && y < 128) ? bo_get_id(x, y, z) : 0;
	if(id == BLOCK_SLAB || id == BLOCK_FARMLAND || id == BLOCK_STONE_STAIRS
	   || id == BLOCK_WOODEN_STAIRS) {
		int v = c3_cell(x, y + 1, z, sub), t;
		if((t = c3_cell(x + 1, y, z, sub)) > v)
			v = t;
		if((t = c3_cell(x - 1, y, z, sub)) > v)
			v = t;
		if((t = c3_cell(x, y, z + 1, sub)) > v)
			v = t;
		if((t = c3_cell(x, y, z - 1, sub)) > v)
			v = t;
		return v;
	}
	return c3_cell(x, y, z, sub);
}

static void c3_read_hook(struct server_local* s, struct server_world* w,
						 w_coord_t x, w_coord_t y, w_coord_t z, int sub,
						 int value) {
	(void)s;
	c3_last_light = value;
	if(w != c3_w)
		return;
	if(!c3_in_region(x, z)) {
		c3_out_region++;
		return;
	}
	if(c3_dirty) {
		c3_dirty = false;
		c3_compute_pending();
	}
	const int k = c3_window;
	c3_reads[k]++;
	const int want = c3_expected(x, y, z, sub);
	if(want != value) {
		if(c3_bad[k] == 0)
			printf("    window %d read (%d,%d,%d) sub %d: %d, Beta %d\n", k, x,
				   y, z, sub, value, want);
		c3_bad[k]++;
	}
	if(y >= 0 && y < 128 && c3_pend[x - BO_MIN][z - BO_MIN][y])
		c3_pending_hits[k]++;
}

/* an independent java.util.Random (CFR-free: the JDK 1.x algorithm) */
static int32_t c3_jnext(uint64_t* seed, int bits) {
	*seed = (*seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
	return (int32_t)(*seed >> (48 - bits));
}
static int32_t c3_jnext_int(uint64_t* seed, int32_t n) {
	if((n & -n) == n)
		return (int32_t)(((int64_t)n * (int64_t)c3_jnext(seed, 31)) >> 31);
	int32_t bits, val;
	do {
		bits = c3_jnext(seed, 31);
		val = bits % n;
	} while(bits - val + (n - 1) < 0);
	return val;
}

static void c3_roll_hook(struct server_world* w, w_coord_t x, w_coord_t y,
						 w_coord_t z, int raw_sky, uint64_t before,
						 uint64_t after, bool decision) {
	if(w != c3_w || !c3_in_region(x, z))
		return;
	c3_spawn_rolls++;
	/* EntityMob.getCanSpawnHere: sky > nextInt(32) refuses, then
	 * getBlockLightValue <= nextInt(8) */
	uint64_t seed = before;
	bool want = false;
	const int r32 = c3_jnext_int(&seed, 32);
	if(!(raw_sky > r32)) {
		const int r8 = c3_jnext_int(&seed, 8);
		want = c3_last_light <= r8;
	}
	const bool sky_ok = raw_sky == bo_get_saved(BO_SKY, x, y, z);
	if(want != decision || seed != after || !sky_ok) {
		if(c3_spawn_bad == 0)
			printf("    spawn roll (%d,%d,%d): sky %d (Beta %d) light %d -> %d "
				   "(Beta %d), state %s\n", x, y, z, raw_sky,
				   bo_get_saved(BO_SKY, x, y, z), c3_last_light, decision, want,
				   seed == after ? "same" : "DIFFERS");
		c3_spawn_bad++;
	}
	if(y >= 0 && y < 128 && c3_pend[x - BO_MIN][z - BO_MIN][y])
		c3_spawn_pending++;
}

static void c3_spawner_hook(w_coord_t x, w_coord_t y, w_coord_t z, int raw_sky,
							int torch, int sub) {
	(void)sub;
	if(!c3_in_region(x, z) || y < 0 || y >= 128)
		return;
	c3_spawner_reads++;
	if(raw_sky != bo_get_saved(BO_SKY, x, y, z)
	   || torch != bo_get_saved(BO_BLOCK, x, y, z)) {
		if(c3_spawner_bad == 0)
			printf("    spawner member (%d,%d,%d): sky %d/%d blk %d/%d "
				   "(cavewii/Beta)\n", x, y, z, raw_sky,
				   bo_get_saved(BO_SKY, x, y, z), torch,
				   bo_get_saved(BO_BLOCK, x, y, z));
		c3_spawner_bad++;
	}
	if(c3_pend[x - BO_MIN][z - BO_MIN][y])
		c3_spawner_pending++;
}

static void c3_write_hook(struct server_world* w, w_coord_t x, w_coord_t y,
						  w_coord_t z, uint8_t type, bool light) {
	(void)light;
	if(w != c3_w || !c3_in_model(x, z) || y < 0 || y >= 128)
		return;
	if(type == BLOCK_GLOWSTONE && x == -6 && z == 2 && c3_window == 2
	   && c3_glow_tick < 0) {
		c3_glow_tick = c3_t; /* TileEntityPiston.updateEntity settles */
		c3_glow_y = y;
	}
	bo_set_block(x, y, z, type);
	c3_dirty = true;
}

/* production block light at the settled glowstone and its +x neighbour */
static void c3_glow_light(int* glow, int* nb) {
	struct block_data b = {0}, n = {0};
	(void)server_world_get_block(c3_w, -6, c3_glow_y, 2, &b);
	(void)server_world_get_block(c3_w, -5, c3_glow_y, 2, &n);
	*glow = b.torch_light;
	*nb = n.torch_light;
}

static int c3_spawner_force;
/* discrimination evidence summed over both worlds (mob AI and so the
 * reads' positions vary run to run; the equality checks are per world) */
static long c3_tot_tick_pending, c3_tot_entity_pending, c3_tot_rolls;
static long c3_ow_rolls; /* the Overworld run's hostile light decisions */
static void c3_phase(struct server_local* s, struct server_world* w,
					 int phase) {
	if(w != c3_w)
		return;
	if(phase == 0) {
		if(c3_mutate == 1)
			bo_drain(); /* the wrong boundary: before World.tick */
		c3_window = 0;
		c3_compute_pending();
		if(c3_glow_tick >= 0 && c3_t == c3_glow_tick + 1) {
			/* the next update's World.tick: still the pre-settle light */
			int gl, nb;
			c3_glow_light(&gl, &nb);
			c3_glow_pending_tick = gl == c3_glow_pre_blk
				&& nb == c3_glow_pre_nb
				&& c3_pend[-6 - BO_MIN][2 - BO_MIN][c3_glow_y];
		}
	} else if(phase == 1) {
		bo_drain(); /* Beta: while(func_6156_d()) after World.tick */
		c3_window = 1;
		c3_compute_pending();
		if(c3_glow_tick >= 0 && c3_t == c3_glow_tick + 1) {
			/* the next drain settles it: glowstone 15, its neighbour 14 */
			int gl, nb;
			c3_glow_light(&gl, &nb);
			c3_glow_drained = gl == 15 && nb == 14
				&& gl == bo_get_saved(BO_BLOCK, -6, c3_glow_y, 2)
				&& nb == bo_get_saved(BO_BLOCK, -5, c3_glow_y, 2);
			printf("  slot %d: piston settle tick %d: block light glowstone "
				   "%d / +x %d pending after it, %d / %d after the next "
				   "drain (Beta %d / %d)\n", c3_slot, c3_glow_tick,
				   c3_glow_pre_blk, c3_glow_pre_nb, gl, nb,
				   bo_get_saved(BO_BLOCK, -6, c3_glow_y, 2),
				   bo_get_saved(BO_BLOCK, -5, c3_glow_y, 2));
		}
	} else if(phase == 2) {
		c3_window = 2;
		c3_compute_pending();
		if(c3_spawner_force)
			for(int i = 0; i < MAX_ACTIVE_SPAWNERS; i++)
				if(s->active_spawners[i].used)
					s->active_spawners[i].delay = 1;
	}
}

static void c3_place(int x, int y, int z, size_t slot, enum side side) {
	struct server_rpc hb = {.type = SRPC_HOTBAR_SLOT,
		.payload.hotbar_slot.slot = slot};
	assert(svin_rpc_send(&hb));
	struct server_rpc call = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = side}};
	assert(svin_rpc_send(&call));
}

static void c3_run(int slot) {
	struct server_local* s = &S[1];
	char id[64];
	snprintf(id, sizeof id, "test_light_queue_c3_%d", slot);
	/* ASTRA-G9-RULING-2 item 20: deterministic spawn inputs. The whole
	 * near ring (radius MAX_VIEW_DISTANCE) is resident before the run and
	 * generation ignores wall-clock budgets, so the near-send never changes
	 * the loaded set mid-run (it decided which chunks animals/hostiles could
	 * draw candidates from, so the shared rand_src stream -- and with it the
	 * hostile candidate cells -- varied run to run). */
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	open_world(s, s_dir[1], sizeof s_dir[1], id, true, MAX_VIEW_DISTANCE);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(s))
		abort();
	mode(1);
	char ndir[260];
	snprintf(ndir, sizeof ndir, "%s_nether", s_dir[1]);
	MKDIR(ndir);
	char nreg[300];
	snprintf(nreg, sizeof nreg, "%s/region", ndir);
	MKDIR(nreg);
	string_t level;
	string_init_set_str(level, ndir);
	server_world_create(&s->worlds[1], level, WORLD_DIM_NETHER, 4242, true);
	string_clear(level);
	s->worlds[1].session = s;
	s->worlds[1].local = s;
	s->worlds[1].world_index = 1;
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s->worlds[1], cx, cz, &sc) && sc);
		}
	s->world_slot_state[1] = SERVER_WORLD_SLOT_LIVE;
	struct server_world* w = &s->worlds[slot];
	c3_w = w;
	c3_slot = slot;
	assert(inventory_create(&s->player.inventory, &inventory_logic_player, s,
							&s->player, INVENTORY_SIZE, 0, 0, 0));
	s->player.active_inventory = &s->player.inventory;
	int g = 1;
	for(; g < 120; g++) {
		struct block_data b = {0};
		assert(server_world_get_block(w, 8, g, 8, &b));
		if(b.type == BLOCK_AIR)
			break;
	}
	s->player.world_slot = (uint8_t)slot;
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.x = 8.5;
	s->player.y = g + (double)PLAYER_EYE_HEIGHT;
	s->player.z = 8.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->render_demo_mob = -1;
	s->world_time = 18000; /* midnight */
	server_local_skylight_recompute(s);
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR + 0,
		(struct item_data) {.id = BLOCK_TORCH, .count = 64});
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR + 1,
		(struct item_data) {.id = BLOCK_LEVER, .count = 4});
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR + 2,
		(struct item_data) {.id = BLOCK_REDSTONE_TORCH_LIT, .count = 4});
	/* producers (setup, settled before the trace) */
	/* block phase: a lit redstone torch on a stone post, a lever beside */
	assert(server_world_set_block(w, 4, g, 12, (struct block_data) {.type = BLOCK_STONE}));
	assert(server_world_set_block(w, 4, g + 1, 12,
		(struct block_data) {.type = BLOCK_REDSTONE_TORCH_LIT, .metadata = 5}));
	/* entity phase: sand over air at (12, g+3, 4) next to a torch at g+1.. */
	assert(server_world_set_block(w, 13, g, 4, (struct block_data) {.type = BLOCK_STONE}));
	assert(server_world_set_block(w, 13, g + 1, 4,
		(struct block_data) {.type = BLOCK_TORCH, .metadata = 5}));
	/* tile phase: a furnace (unfuelled for now), a burning furnace about to
	 * go out, a piston with glowstone in front */
	cx_fx[0][0] = w->world_index;
	cx_furnace(w, 0, -4, g, 6, false, false);
	cx_furnace(w, 1, -4, g, 10, true, false);
	assert(server_world_set_block(w, -8, g, 2,
		(struct block_data) {.type = BLOCK_PISTON_BASE, .metadata = 5}));
	assert(server_world_set_block(w, -7, g, 2,
		(struct block_data) {.type = BLOCK_GLOWSTONE}));
	/* consumers: a dungeon spawner, a creeper */
	assert(server_world_set_block(w, 10, g, 14,
		(struct block_data) {.type = BLOCK_MOB_SPAWNER}));
	assert(spawner_te_upsert_w(w, 10, g, 14, "Zombie"));
	vec3 cpos = {6.5F, (float)g, 4.5F};
	assert(server_local_spawn_monster_at_w(w, cpos, MONSTER_CREEPER));
	server_local_test_update_once(s);
	assert(server_world_light_drain(&s->worlds[0]));
	assert(server_world_light_drain(&s->worlds[1]));
	/* the model starts from the settled field */
	bo_load(w);
	bo_qn = 0;
	memset(c3_pend, 0, sizeof c3_pend);
	memset(c3_reads, 0, sizeof c3_reads);
	memset(c3_bad, 0, sizeof c3_bad);
	memset(c3_pending_hits, 0, sizeof c3_pending_hits);
	c3_spawn_rolls = c3_spawn_bad = c3_spawn_pending = 0;
	c3_spawner_reads = c3_spawner_bad = c3_spawner_pending = 0;
	c3_out_region = 0;
	c3_t = c3_glow_tick = c3_glow_pre_blk = c3_glow_pre_nb = -1;
	c3_glow_pending_after = c3_glow_pending_tick = c3_glow_drained = false;
	{
		const char* mu = getenv("G9_CS_MUTATE");
		c3_mutate = mu ? atoi(mu) : 0;
	}
	server_world_test_block_write_hook = c3_write_hook;
	server_local_test_light_read_hook = c3_read_hook;
	server_local_test_spawn_roll_hook = c3_roll_hook;
	spawner_test_cell_hook = c3_spawner_hook;
	server_local_test_phase_hook = c3_phase;
	lcg_state = 0xC3C3u + (uint32_t)slot;
	for(int t = 0; t < 800; t++) {
		inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR + 0,
			(struct item_data) {.id = BLOCK_TORCH, .count = 64});
		for(int k = 0; k < 3 && t >= 6; k++)
			c3_place(-14 + (int)(lcg() % 44u), g - 1,
					 -14 + (int)(lcg() % 44u), 0, SIDE_TOP);
		/* a 20-tick spawner window every 200 ticks: its bursts are traced,
		 * and the hostile cap stays open for natural spawning */
		c3_spawner_force = t % 200 < 20;
		if(t == 0) {
			/* packet: torches all over the region (the network phase) */
			for(int k = 0; k < 16; k++)
				c3_place(-14 + 12 * (k % 4) + (k / 4), g - 1,
						 -14 + 12 * (k / 4) + (k % 3), 0, SIDE_TOP);
			/* packet: a lever on the redstone torch post's west face */
			c3_place(4, g, 12, 1, SIDE_LEFT);
			/* tile: the cold furnace gets fuel; the piston gets power */
			furnace_te_lookup_w(w, -4, g, 6)->items[FURNACE_SLOT_FUEL_TE]
				= (struct item_data) {.id = ITEM_COAL, .count = 2};
			furnace_te_lookup_w(w, -4, g, 10)->burn_time = 2;
		}
		if(t == 1) /* packet: pull the lever on (BlockLever.blockActivated);
					* the post is powered, the torch on it turns off at its
					* scheduled tick (BlockRedstoneTorch.updateTick) */
			c3_place(3, g, 12, 1, SIDE_TOP);
		if(t == 2) /* packet: a redstone torch beside the piston: it extends
					* at once, its moving glowstone settles at T+2 in the tile
					* phase (TileEntityPiston) */
			c3_place(-8, g - 1, 1, 2, SIDE_TOP);
		if(t == 4 || t == 24 || t == 44) {
			/* entity: sand above air beside the torch and over the spawner
			 * room (its scheduled tick spawns the falling entity; the
			 * entity's first tick clears the cell, its landing writes) */
			(void)server_world_set_block(w, 12, g + 4, 4,
				(struct block_data) {.type = BLOCK_SAND});
			(void)server_world_set_block(w, 10, g + 5, 12 + t / 20,
				(struct block_data) {.type = BLOCK_SAND});
		}
		if(t >= 6 && t <= 9) {
			/* later entities (ids above the falling sand's) on the cell the
			 * landing sand darkens: (11,g,4) loses the torch path through
			 * (12,g,4) -- their brightness read in the same updateEntities
			 * is Beta's pending value */
			vec3 zp = {11.5F, (float)g, 4.5F};
			(void)server_local_spawn_monster_at_w(w, zp, MONSTER_ZOMBIE);
		}
		if(t == 5 || t == 25) {
			/* a later entity (higher id: it ticks after the sand) beside
			 * the sand's column */
			vec3 zp = {11.5F, (float)g, 12.5F + (float)(t / 20)};
			(void)server_local_spawn_monster_at_w(w, zp, MONSTER_ZOMBIE);
		}
		if(t % 8 == 5)
			for(int k = 0; k < 8; k++)
				c3_place(-15 + 6 * k, g - 1, 26 - (t % 24), 0, SIDE_TOP);
		s->player.health = MAX_PLAYER_HEALTH;
		c3_t = t;
		server_local_test_update_once(s);
		if(c3_glow_tick == t) {
			/* after the settle update: the glowstone light is pending
			 * (the undrained model agrees; the drained model differs) */
			c3_glow_light(&c3_glow_pre_blk, &c3_glow_pre_nb);
			c3_compute_pending();
			c3_glow_pending_after
				= c3_glow_pre_blk == bo_get_saved(BO_BLOCK, -6, c3_glow_y, 2)
				&& c3_glow_pre_nb == bo_get_saved(BO_BLOCK, -5, c3_glow_y, 2)
				&& c3_glow_pre_blk != 15
				&& c3_pend[-6 - BO_MIN][2 - BO_MIN][c3_glow_y];
		}
	}
	c3_t = -1;
	server_local_test_phase_hook = NULL;
	/* the settled fields, interior: the production drain (next update) and
	 * the model's */
	server_local_test_update_once(s);
	bo_drain();
	long field_bad = 0;
	for(int x = BO_MIN + 16; x < BO_MIN + BO_N - 16; x++)
		for(int z = BO_MIN + 16; z < BO_MIN + BO_N - 16; z++)
			for(int y = 0; y < 128; y++) {
				struct block_data b = {0};
				(void)server_world_get_block(w, x, y, z, &b);
				if(b.type != bo_get_id(x, y, z)
				   || b.sky_light != bo_get_saved(BO_SKY, x, y, z)
				   || b.torch_light != bo_get_saved(BO_BLOCK, x, y, z)) {
					if(field_bad < 3)
						printf("    settled field (%d,%d,%d): id %d/%d sky %d/%d "
							   "blk %d/%d (cavewii/Beta)\n", x, y, z, b.type,
							   bo_get_id(x, y, z), b.sky_light,
							   bo_get_saved(BO_SKY, x, y, z), b.torch_light,
							   bo_get_saved(BO_BLOCK, x, y, z));
					field_bad++;
				}
			}
	printf("  slot %d: settled interior field: %ld cells differ from the Beta "
		   "model\n", slot, field_bad);
	server_world_test_block_write_hook = NULL;
	server_local_test_light_read_hook = NULL;
	server_local_test_spawn_roll_hook = NULL;
	spawner_test_cell_hook = NULL;
	struct block_data fur = {0}, ext = {0}, mv = {0}, rt = {0};
	(void)server_world_get_block(w, -4, g, 6, &fur);
	(void)server_world_get_block(w, -4, g, 10, &ext);
	(void)server_world_get_block(w, -6, g, 2, &mv);
	(void)server_world_get_block(w, 4, g + 1, 12, &rt);
	printf("  slot %d: producers: furnace %d (lit 62), out %d (61), pushed %d "
		   "(glowstone 89), redstone torch %d (75 off)\n", slot, fur.type,
		   ext.type, mv.type, rt.type);
	printf("  slot %d: reads tick %ld / entities %ld / tiles %ld, mismatches "
		   "%ld/%ld/%ld, on pending cells %ld/%ld/%ld; spawn rolls %ld (bad "
		   "%ld, pending %ld); spawner members %ld (bad %ld, pending %ld); "
		   "outside the model %ld\n", slot, c3_reads[0], c3_reads[1],
		   c3_reads[2], c3_bad[0], c3_bad[1], c3_bad[2], c3_pending_hits[0],
		   c3_pending_hits[1], c3_pending_hits[2], c3_spawn_rolls,
		   c3_spawn_bad, c3_spawn_pending, c3_spawner_reads, c3_spawner_bad,
		   c3_spawner_pending, c3_out_region);
	char what[240];
	snprintf(what, sizeof what,
			 "slot %d: real packet/block/entity/tile producers fired (furnace "
			 "lit and out, piston completed, torch off)", slot);
	check(fur.type == BLOCK_FURNACE_LIT && ext.type == BLOCK_FURNACE
			  && mv.type == BLOCK_GLOWSTONE && rt.type == BLOCK_REDSTONE_TORCH,
		  what);
	snprintf(what, sizeof what,
			 "slot %d: the piston glowstone settled in this world's tile "
			 "phase; its light was pending after that update and at the next "
			 "World.tick, and the next drain settled it to Beta's 15/14", slot);
	check(c3_glow_tick >= 0 && c3_glow_pending_after && c3_glow_pending_tick
			  && c3_glow_drained,
		  what);
	snprintf(what, sizeof what,
			 "slot %d: every production light read (World.tick, entities, "
			 "tiles), every spawn roll and Random state and every spawner "
			 "member equals the Beta model drained at Beta's boundary", slot);
	check(c3_bad[0] + c3_bad[1] + c3_bad[2] == 0 && c3_spawn_bad == 0
			  && c3_spawner_bad == 0 && c3_reads[0] > 0 && c3_reads[1] > 0
			  && c3_spawner_reads > 0,
		  what);
	snprintf(what, sizeof what,
			 "slot %d: World.tick reads hit cells whose light was still "
			 "pending (a different drain point would change them)", slot);
	check(c3_pending_hits[0] > 0, what);
	c3_tot_tick_pending += c3_pending_hits[0];
	c3_tot_entity_pending += c3_pending_hits[1];
	c3_tot_rolls += c3_spawn_rolls;
	if(slot == 0)
		c3_ow_rolls = c3_spawn_rolls;
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_NORMAL);
	inventory_destroy(&s->player.inventory, &s->player);
	s->player.world_slot = 0;
	s->world_slot_state[1] = SERVER_WORLD_SLOT_EMPTY;
	server_world_discard(&s->worlds[1]);
	test_fixture_dir_cleanup(ndir);
	close_world(s, s_dir[1], false);
}

static void case_consumers(void) {
	c3_tot_tick_pending = c3_tot_entity_pending = c3_tot_rolls = 0;
	c3_run(0); /* the Overworld (slot 0) */
	c3_run(1); /* the Nether (slot 1, the host committed to it) */
	printf("  both worlds: pending-cell reads World.tick %ld, entities %ld; "
		   "natural spawn rolls %ld\n", c3_tot_tick_pending,
		   c3_tot_entity_pending, c3_tot_rolls);
	check(c3_tot_entity_pending > 0 && c3_ow_rolls > 0,
		  "entity reads hit still-pending cells and real Overworld hostile "
		  "spawn light decisions were traced, each replayed exactly (decision, "
		  "draws, post-RNG) -- deterministic inputs, no retry (Beta Nether "
		  "pigmen roll no light)");
	int s2 = 0;
	const int packet = cs_run(false, false, &s2);
	const int entity = cs_run(true, false, &s2);
	const int entity_next = s2;
	int s3 = 0;
	const int entity_sync = cs_run(true, true, &s3);
	printf("  creeper age delta: packet-phase light %d (Beta 3); entity-phase "
		   "light %d then %d (Beta 1, 3); old synchronous path %d\n", packet,
		   entity, entity_next, entity_sync);
	check(packet == 3, "a packet-phase light is settled before the creeper's "
					   "own entity tick (bright bonus)");
	check(entity == 1 && entity_next == 3,
		  "an entity-phase light stays pending for later entities of the "
		  "same updateEntities and is seen after the next drain");
	check(entity_sync == 3,
		  "the old synchronous path differs (+3): the check is sensitive");
}


/* ---- workloads (ASTRA-G9-REVIEW Required 6: measured on PC) ----
 * 240 production updates of a 5x5-chunk session with engaged light work in
 * every phase: packet-phase roof (8x8 at y+4) and torch-grid place/remove,
 * tile-entity-phase furnace lit toggles and piston-like block moves, and an
 * entity-phase TNT-scale blast every 20 ticks. Measured: the heartbeat
 * light-queue maxima (allocated bytes, seeds, drain us / cells), and per tick
 * the pending debt at the prescribed boundary. Required: zero pending seeds
 * at every drain boundary (no persistent debt), zero light stops / drain
 * ceilings / OOM / flood ceilings / stack drops, and a final field that is a
 * fixed point of Beta's relaxation (MetadataChunkBlock.func_4107_a's cell
 * rule) in every interior cell of all 25 chunks, Sky and Block -- a lost
 * update would leave a cell the rule changes. */
static int wl_h0;
static long wl_tick;
static size_t wl_debt_max;
static long wl_boundaries;
static void wl_probe(struct server_local* s, struct server_world* w,
					 int phase) {
	if(w != &s->worlds[0])
		return;
	sm_sample(w);
	if(phase == 1) {
		/* the prescribed boundary: right after this world's drain */
		if(w->light_seed_n > wl_debt_max)
			wl_debt_max = w->light_seed_n;
		wl_boundaries++;
		if(wl_tick % 20 == 10) {
			vec3 c = {(float)(14 + (wl_tick / 20) % 3 * 6) + 0.5f,
					  (float)wl_h0 - 1.5f,
					  (float)(14 + (wl_tick / 60) % 3 * 6) + 0.5f};
			server_world_explode(w, c, 4.0f); /* an entity-phase blast */
			sm_sample(w);
		}
	} else if(phase == 2) {
		/* tile-entity phase: a furnace lit toggle and a piston-like move */
		(void)server_world_set_block(w, -6, wl_h0, -6,
			(struct block_data) {.type = (wl_tick & 1) ? BLOCK_FURNACE_LIT :
														 BLOCK_FURNACE});
		const int px = -8 + (int)(wl_tick % 6);
		(void)server_world_set_block(w, px, wl_h0 + 1, 6,
			(struct block_data) {.type = BLOCK_AIR});
		(void)server_world_set_block(w, px + 1, wl_h0 + 1, 6,
			(struct block_data) {.type = BLOCK_STONE});
		sm_sample(w);
	}
}

/* Beta's relaxation cell rule over the oracle copy: the value
 * MetadataChunkBlock.func_4107_a would compute for (x,y,z). */
static int wl_rule(int type, int x, int y, int z) {
	uint8_t id = bo_get_id(x, y, z);
	int op = bo_opacity(id);
	if(op == 0)
		op = 1;
	int src = type == BO_SKY ? (bo_can_see_sky(x, y, z) ? 15 : 0) :
							   bo_lightvalue(id);
	if(op >= 15 && src == 0)
		return 0;
	int m = bo_get_saved(type, x - 1, y, z);
	int v;
	if((v = bo_get_saved(type, x + 1, y, z)) > m)
		m = v;
	if((v = bo_get_saved(type, x, y - 1, z)) > m)
		m = v;
	if((v = bo_get_saved(type, x, y + 1, z)) > m)
		m = v;
	if((v = bo_get_saved(type, x, y, z - 1)) > m)
		m = v;
	if((v = bo_get_saved(type, x, y, z + 1)) > m)
		m = v;
	if((m -= op) < 0)
		m = 0;
	return src > m ? src : m;
}

static void case_workloads(void) {
	struct server_local* s = &S[1];
	open_world(s, s_dir[1], sizeof s_dir[1], "test_light_queue_workloads",
			   true, 2);
	mode(1);
	assert(inventory_create(&s->player.inventory, &inventory_logic_player, s,
							&s->player, INVENTORY_SIZE, 0, 0, 0));
	s->player.active_inventory = &s->player.inventory;
	uint8_t hh = 0;
	assert(server_world_get_height(&s->worlds[0], 1, 1, &hh));
	wl_h0 = hh;
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.x = 1.5;
	s->player.y = wl_h0 + (double)PLAYER_EYE_HEIGHT;
	s->player.z = 1.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->render_demo_mob = -1;
	server_local_test_update_once(s);
	assert(server_world_light_drain(&s->worlds[0]));
	server_world_test_light_stats_reset();
	const uint32_t fceil0 = lighting_flood_ceiling_hits();
	const uint32_t drop0 = stack_oom_drops();
	wl_debt_max = 0;
	wl_boundaries = 0;
	sm_reset();
	server_world_test_light_drain_hook = sm_drain_hook;
	server_local_test_phase_hook = wl_probe;
	for(wl_tick = 0; wl_tick < 240; wl_tick++) {
		struct server_world* w = &s->worlds[0];
		/* packet phase: a roof over (4..11, 4..11) and a torch grid */
		const bool on = (wl_tick / 4) % 2 == 0;
		for(int x = 4; x < 12; x++)
			for(int z = 4; z < 12; z++)
				(void)server_world_set_block(w, x, wl_h0 + 4, z,
					(struct block_data) {.type = on ? BLOCK_STONE : BLOCK_AIR});
		for(int k = 0; k < 4; k++)
			(void)server_world_set_block(w, -12 + 3 * k, wl_h0, -2 + k,
				(struct block_data) {.type = (wl_tick + k) % 3 ?
													  BLOCK_TORCH : BLOCK_AIR});
		s->player.health = MAX_PLAYER_HEALTH;
		sm_sample(w);
		server_local_test_update_once(s);
	}
	server_local_test_phase_hook = NULL;
	server_local_test_update_once(s); /* the last writes' prescribed drain */
	const bool clear = server_world_light_drain(&s->worlds[0])
		&& !server_world_light_pending(&s->worlds[0]);
	struct server_world_light_hb hb;
	server_world_light_hb(&hb);
	struct server_world_light_stats st;
	server_world_light_stats(&st);
	const uint32_t fceil = lighting_flood_ceiling_hits() - fceil0;
	const uint32_t drops = stack_oom_drops() - drop0;
	/* fixed point of Beta's cell rule over the interior (rim chunk skipped:
	 * Beta's own relaxation skips cells whose neighbour chunks are absent) */
	bo_load(&s->worlds[0]);
	long bad = 0;
	for(int x = BO_MIN + 16; x < BO_MIN + BO_N - 16; x++)
		for(int z = BO_MIN + 16; z < BO_MIN + BO_N - 16; z++)
			for(int y = 0; y < 127; y++)
				for(int t = 0; t < 2; t++) {
					int want = wl_rule(t, x, y, z);
					int have = bo_get_saved(t, x, y, z);
					if(want != have) {
						if(bad == 0)
							printf("    first inconsistent cell (%d,%d,%d) "
								   "%s %d, rule %d\n", x, y, z,
								   t == BO_SKY ? "sky" : "block", have, want);
						bad++;
					}
				}
	printf("  workloads: %ld boundaries, pending at a boundary max %zu; "
		   "drains %u, seeds total %llu\n", wl_boundaries, wl_debt_max,
		   st.drains, (unsigned long long)st.seeds_total);
	printf("  queue storage held after the run: %u B (blast-sized scratch "
		   "returned after each drain)\n", hb.bytes[0]);
	printf("  heartbeat lq (slot 0): peak allocated %u B, seeds peak %u, drain "
		   "max %u us / %u cells; faults %u ceil %u oom %u trials %u; "
		   "flood ceilings %u; stack drops %u\n", hb.peak[0], hb.seeds_pk[0],
		   hb.drain_us[0], hb.drain_pops[0], hb.faults, hb.ceil, hb.oom,
		   hb.trials, fceil, drops);
	printf("  final field: %ld cells inconsistent with Beta's cell rule\n",
		   bad);
	check(wl_boundaries >= 240 && wl_debt_max == 0 && clear,
		  "no persistent pending debt: zero seeds at every prescribed "
		  "boundary, nothing pending after the last drain");
	check(hb.bytes[0] <= 2 * 16384,
		  "the blast-sized drain scratch is not held across ticks");
	check(hb.faults == 0 && hb.ceil == 0 && hb.oom == 0 && fceil == 0
			  && drops == 0,
		  "zero light stops, drain ceilings, OOM, flood ceilings, stack drops");
	check(bad == 0 && hb.seeds_pk[0] > 64 && hb.drain_pops[0] > 1000,
		  "engaged workloads settle to Beta's fixed point in every interior "
		  "cell (no lost update)");
	server_world_test_light_drain_hook = NULL;
	/* G9 R6 v2: the heartbeat's allocation peaks equal the capacities and
	 * frontier sampled independently; the prescribed boundaries carried
	 * no debt; the per-update and drain maxima are populated */
	printf("  heartbeat v2 (slot 0): seed %u undo %u pin %u front %u B "
		   "(sampled %zu/%zu/%zu/%zu); debt %u; tick_us %u drain_us %u; "
		   "ckpt_fail %u\n", hb.seed_b[0], hb.undo_b[0], hb.pin_b[0],
		   hb.front_b[0], sm_seed, sm_undo, sm_pin, sm_front, hb.debt[0],
		   hb.tick_us, hb.drain_us[0], hb.ckpt_fail);
	check(hb.seed_b[0] == sm_seed && hb.undo_b[0] == sm_undo
			  && hb.pin_b[0] == sm_pin && hb.front_b[0] == sm_front
			  && sm_front > 0 && sm_seed > 0,
		  "measured allocation peaks: seed/undo/pin capacity and frontier "
		  "peaks equal the independent samples");
	check(hb.debt[0] == 0 && hb.debt[1] == 0 && hb.ckpt_fail == 0
			  && hb.tick_us >= hb.drain_us[0] && hb.drain_us[0] > 0,
		  "zero post-drain debt, zero fault counters, per-update light time "
		  ">= the slowest drain");
	inventory_destroy(&s->player.inventory, &s->player);
	close_world(s, s_dir[1], false);
}


/* ---- packet_delivery (ASTRA-G9-REVIEW-2 item 2) ----
 * Beta packet cycle [CFR MinecraftServer.java:340-353, PlayerInstance.java,
 * World.java:512-519 setLightValue -> markBlockNeedsUpdate]: doTick runs,
 * per world, tick() -> while(func_6156_d()) -> updateEntities(), THEN
 * handleNetworkListenThread (player place/dig packets: setBlockWithNotify,
 * the func_339_g column write at once, propagation queued), THEN
 * configManager.onTick -> PlayerInstance.onUpdate flushes every marked cell
 * (block AND light changes): 1 cell -> Packet53 (id/meta), 2..9 ->
 * Packet52 (id/meta), 10 -> Packet51 cuboid WITH the server light arrays.
 * A packet-phase edit is therefore published (id/meta) before the next
 * drain; the client relights it itself (its own World light queue); the
 * settled server light reaches it only through a Packet51 cuboid.
 * CaveWii: RPCs run at the top of an update (the previous doTick's network
 * phase), CRPC_SET_BLOCK is published at the write (before the drain, as
 * Packet53/52), the client relights from it (world_set_block -> the
 * per-frame world_update_lighting driver); a column with pending light is
 * never snapshotted (CRPC_CHUNK carries only settled light, like Packet51).
 * This case PROVES the self-relighting delivery is equivalent: real
 * SRPC_BLOCK_PLACE / SRPC_BLOCK_DIG requests at the x = -1|0 chunk seam,
 * real client FIFO consumption (clin_update_drain) and client lighting,
 * and a seam chunk whose CHUNK publication is held back (its client column
 * unloaded, every chunk RPC slot held: backpressure) while the edits land.
 *   pre/post: the server field right after the requests (pre-boundary) and
 *     after the production drain (post-boundary) equals the independent
 *     Beta model (bo_*), Sky/Block/height;
 *   order: the client consumed one CRPC_SET_BLOCK per edit, in request
 *     order, before the drain;
 *   delivery: once the held chunk is published, every light cell of the
 *     client's 4x3 chunks around the seam equals the server, with no
 *     manual drain anywhere (server: production updates; client: its
 *     per-frame lighting driver until idle). */
#define check owner_swap_check
#include "support/owner_swap_harness.h"
#undef check
#include "game/gui/screen.h"

static struct server_local* pd_fixture(enum world_dim dim) {
	test_fixture_dir("test_light_queue_pd", split_path, sizeof(split_path));
	MKDIR(split_path);
	struct server_local* s = calloc(1, sizeof(*s));
	assert(s);
	server_local_test_create_without_thread(s, false, false, false, false,
											false, true, false);
	string_t path;
	string_init_set_str(path, split_path);
	assert(level_archive_create_new(&s->level, path, 12345, (vec3) {0, 4, 0}));
	assert(server_world_create_with_populate_scratch(
		&s->worlds[0], path, dim, 12345, true, s->world_populate_scratch));
	string_clear(path);
	s->worlds[0].local = s;
	s->worlds[0].session = s;
	s->player.has_pos = true;
	s->player.finished_loading = false;
	s->player.x = s->player.z = 0.5;
	s->player.y = 4.0 + PLAYER_EYE_HEIGHT;
	s->player.health = MAX_PLAYER_HEALTH;
	s->player.oxygen = MAX_OXYGEN;
	s->autosave_ticks = 0;
	s->far_view_distance = MAX_VIEW_DISTANCE + 1;
	b1_bind_client_world(&s->worlds[0]);
	return s;
}

static unsigned pd_sets;
static int pd_set_xyz[64][3];
static bool pd_watch;
static void pd_observe(const struct client_rpc* call, void* user) {
	consumed(call, user);
	if(pd_watch && call->type == CRPC_SET_BLOCK && pd_sets < 64) {
		pd_set_xyz[pd_sets][0] = call->payload.set_block.x;
		pd_set_xyz[pd_sets][1] = call->payload.set_block.y;
		pd_set_xyz[pd_sets][2] = call->payload.set_block.z;
		pd_sets++;
	}
}

/* Mutation switch for the sensitivity check (G9_PD_MUTATE): 1 = the client
 * lighting driver never runs after the edits (no self-relight). */
static int pd_mutate;
static void pd_client_light_idle(void) {
	for(int n = 0; n < 100000; n++) {
		if(stack_empty(&gstate.world.lighting_updates)
		   && lighting_flood_queue_empty(&gstate.world.lighting_flood_queue))
			return;
		world_update_lighting(&gstate.world);
	}
	assert(!"client lighting never went idle");
}

/* every light/id cell of the client vs the server over chunks
 * x -2..1, z -1..1 (the seam pair and their neighbours) */
static long pd_sky_diffs, pd_blk_diffs;
static long pd_client_diff(struct server_local* s, const char* tag) {
	long n = 0;
	pd_sky_diffs = pd_blk_diffs = 0;
	for(int x = -32; x < 32; x++)
		for(int z = -16; z < 32; z++)
			for(int y = 0; y < 128; y++) {
				struct block_data sv = {0};
				if(!server_world_get_block(&s->worlds[0], x, y, z, &sv))
					continue;
				struct block_data cl = world_get_block(&gstate.world, x, y, z);
				pd_sky_diffs += cl.sky_light != sv.sky_light;
				pd_blk_diffs += cl.torch_light != sv.torch_light;
				if(cl.type != sv.type || cl.sky_light != sv.sky_light
				   || cl.torch_light != sv.torch_light) {
					if(n == 0)
						printf("    %s first client diff at (%d,%d,%d): id %d/%d "
							   "sky %d/%d blk %d/%d (client/server)\n", tag, x,
							   y, z, cl.type, sv.type, cl.sky_light, sv.sky_light,
							   cl.torch_light, sv.torch_light);
					n++;
				}
			}
	return n;
}

static void pd_hotbar(struct server_local* s, size_t slot) {
	struct server_rpc call = {.type = SRPC_HOTBAR_SLOT,
		.payload.hotbar_slot.slot = slot};
	host_send(s, &call);
}
static void pd_place(struct server_local* s, int x, int y, int z,
					 enum side side) {
	struct server_rpc call = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = side}};
	host_send(s, &call);
}
static void pd_dig(struct server_local* s, int x, int y, int z) {
	struct server_rpc call = {.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.finished = true, .x = x, .y = y, .z = z,
							  .side = SIDE_TOP}};
	host_send(s, &call);
}

static struct chunk_rpc_slot* pd_held[256];
static int pd_nheld;
static void pd_release_slots(void) {
	for(int k = 0; k < pd_nheld; k++)
		clin_chunk_slot_release(pd_held[k]);
	pd_nheld = 0;
}

struct pd_edit {
	int x, y, z; /* the cell written */
	uint8_t id;	 /* its id afterwards */
};

static void case_packet_delivery_dim(enum world_dim dim) {
	recipe_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset.dimension = dim});
	clin_test_set_consume_observer(pd_observe, NULL);
	struct server_local* s = pd_fixture(dim);
	s->paused = true;
	stream_to_marker(s);
	acknowledge(s);
	s->paused = false;
	s->thread_started = true;
	struct server_world* w = &s->worlds[0];
	/* ground top at the player's column */
	int g = 1;
	for(; g < 120; g++) {
		struct block_data b = {0};
		assert(server_world_get_block(w, 0, g, 0, &b));
		if(b.type == BLOCK_AIR)
			break;
	}
	s->player.y = g + (double)PLAYER_EYE_HEIGHT;
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR + 0,
		(struct item_data) {.id = BLOCK_TORCH, .count = 16});
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR + 1,
		(struct item_data) {.id = BLOCK_STONE, .count = 32});
	for(int k = 0; k < 3; k++)
		tick(s);
	pd_client_light_idle();
	const long settled0 = pd_client_diff(s, "before");
	printf("  %s: ground top y=%d, client==server before the edits: %ld cells "
		   "differ\n", dim == WORLD_DIM_NETHER ? "nether" : "overworld", g,
		   settled0);
	/* the seam chunk (-1,0) leaves the client (an unpublished column: the
	 * production near-send owes it again) */
	clin_process(&(struct client_rpc) {.type = CRPC_UNLOAD_CHUNK,
		.payload.unload_chunk = {.x = -1, .z = 0}});
	struct server_chunk* sb = dict_server_chunks_get(w->chunks, S_CHUNK_ID(-1, 0));
	assert(sb);
	sb->sent = false;
	bo_load(w);
	bo_qn = 0;
	/* batch 1: real place / dig requests at the seam (packet phase) */
	const struct pd_edit e1[] = {
		{0, g, 2, BLOCK_TORCH},	   /* torch on the A side of the seam */
		{-1, g, 5, BLOCK_TORCH},   /* torch in the held chunk */
		{0, g, 9, BLOCK_STONE},	   /* a pillar ... */
		{0, g + 1, 9, BLOCK_STONE},
		{-1, g + 1, 9, BLOCK_STONE}, /* ... with an overhang into B */
		{1, g + 1, 9, BLOCK_STONE},	 /* ... and into A */
		{0, g - 1, 12, BLOCK_AIR},	 /* dig the ground: a sky hole in A */
		{-1, g - 1, 12, BLOCK_AIR},	 /* and in B */
		/* the overhang grows into a roof strip across the seam */
		{-2, g + 1, 9, BLOCK_STONE},
		{-3, g + 1, 9, BLOCK_STONE},
		{-4, g + 1, 9, BLOCK_STONE},
		{2, g + 1, 9, BLOCK_STONE},
		{3, g + 1, 9, BLOCK_STONE},
		{4, g + 1, 9, BLOCK_STONE},
	};
	pd_sets = 0;
	pd_watch = true;
	pd_hotbar(s, 0);
	pd_place(s, 0, g - 1, 2, SIDE_TOP);
	pd_place(s, -1, g - 1, 5, SIDE_TOP);
	pd_hotbar(s, 1);
	pd_place(s, 0, g - 1, 9, SIDE_TOP);
	pd_place(s, 0, g, 9, SIDE_TOP);
	pd_place(s, 0, g + 1, 9, SIDE_LEFT);
	pd_place(s, 0, g + 1, 9, SIDE_RIGHT);
	pd_dig(s, 0, g - 1, 12);
	pd_dig(s, -1, g - 1, 12);
	for(int k = 1; k <= 3; k++)
		pd_place(s, -k, g + 1, 9, SIDE_LEFT);
	for(int k = 1; k <= 3; k++)
		pd_place(s, k, g + 1, 9, SIDE_RIGHT);
	pd_watch = false;
	long pre = 0, post = 0;
	int bad_order = 0;
	for(size_t i = 0; i < sizeof e1 / sizeof e1[0]; i++) {
		struct block_data b = {0};
		assert(server_world_get_block(w, e1[i].x, e1[i].y, e1[i].z, &b));
		if(b.type != e1[i].id)
			printf("    edit %zu at (%d,%d,%d): id %d, wanted %d\n", i, e1[i].x,
				   e1[i].y, e1[i].z, b.type, e1[i].id);
		bad_order += b.type != e1[i].id;
		bo_set_block(e1[i].x, e1[i].y, e1[i].z, e1[i].id);
		bad_order += i >= pd_sets || pd_set_xyz[i][0] != e1[i].x
			|| pd_set_xyz[i][1] != e1[i].y || pd_set_xyz[i][2] != e1[i].z;
	}
	const bool pending = server_world_light_pending(w)
		&& server_world_light_pinned(w, -1, 0) && !sb->sent;
	pre += bo_diff(w, "pd pre");
	/* Hold every snapshot slot for the WHOLE update: streaming returns busy,
	 * the prescribed drain still runs, and the seam remains unsent. Releasing
	 * slots then retries publication before the second packet phase; the
	 * snapshot remains unconsumed so corrections must follow it in FIFO. */
	pd_nheld = 0;
	while(pd_nheld < 256 && (pd_held[pd_nheld] = clin_chunk_slot_try_acquire()))
		pd_nheld++;
	assert(pd_nheld > 0 && pd_nheld < 256);
	const bool unsent_before = !sb->sent;
	server_local_test_update_once(s);
	const bool busy_unsent = !sb->sent;
	pd_release_slots();
	for(int retry = 0; retry < 8 && !sb->sent; retry++) {
		/* Older deferred output and an eviction UNLOAD must be consumed
		 * before their replacement snapshots can be admitted. Drain BEFORE
		 * this update, leaving its new CHUNK unconsumed for batch 2. */
		clin_update_drain();
		while(!clin_rpc_flush_pending()) clin_update_drain();
		clin_update_drain();
		server_local_test_update_once(s);
	}
	const bool sent_in_flight = sb->sent;
	bo_drain();
	post += bo_diff(w, "pd post");
	/* batch 2 (the next packet phase, the CHUNK still unconsumed): remove
	 * the A torch, remove the torch IN the in-flight chunk, a torch further
	 * in A -- their CRPC_SET_BLOCKs queue behind the CHUNK */
	pd_watch = true;
	const unsigned sets1 = pd_sets;
	pd_hotbar(s, 0);
	pd_dig(s, 0, g, 2);
	bo_set_block(0, g, 2, BLOCK_AIR);
	pd_dig(s, -1, g, 5);
	bo_set_block(-1, g, 5, BLOCK_AIR);
	pd_place(s, 1, g - 1, 14, SIDE_TOP);
	bo_set_block(1, g, 14, BLOCK_TORCH);
	pd_watch = false;
	const bool order2 = pd_sets == sets1 + 3;
	pre += bo_diff(w, "pd pre2");
	tick(s);
	bo_drain();
	post += bo_diff(w, "pd post2");
	for(int k = 0; k < 3; k++)
		tick(s);
	{
		const char* mu = getenv("G9_PD_MUTATE");
		pd_mutate = mu ? atoi(mu) : 0;
	}
	if(pd_mutate != 1)
		pd_client_light_idle();
	const long cdiff = pd_client_diff(s, "delivered");
	printf("  set_block RPCs consumed in order: %u (bad %d, batch 2 %d); "
		   "pending+pinned at the boundary %d, unsent before the drain %d, "
		   "published after slot retry %d\n", pd_sets, bad_order, order2,
		   pending, unsent_before, sent_in_flight);
	printf("  server vs Beta model: %ld pre-drain / %ld settled cells differ; "
		   "client vs server after delivery: %ld cells differ (sky %ld, "
		   "block %ld)\n", pre, post, cdiff, pd_sky_diffs, pd_blk_diffs);
	check(settled0 == 0 && w->dimension == dim
			  && gstate.world.dimension == dim,
		  "the client field equals the server before the edits (both in the "
		  "same dimension)");
	check(bad_order == 0 && order2 && pending,
		  "real place/dig requests: each edit published once, in order, before "
		  "the drain; the held seam chunk pinned");
	check(pre == 0 && post == 0,
		  "pre-boundary and settled Sky/Block/height equal the Beta 1.7.3 "
		  "model");
	check(unsent_before && busy_unsent && sent_in_flight && cdiff == 0,
		  "after the held chunk is published every client light cell of the "
		  "seam neighbourhood equals the settled server field (no manual "
		  "drain, no stale overwrite, no lost correction)");
	clin_test_set_consume_observer(NULL, NULL);
	teardown(s);
}

static void case_client_fluid_noop(void);
static void case_packet_delivery(void) {
	case_client_fluid_noop();
	case_packet_delivery_dim(WORLD_DIM_OVERWORLD);
}
static void case_packet_delivery_nether(void) {
	case_packet_delivery_dim(WORLD_DIM_NETHER);
}

/* ---- g9x_publication (G9X-6 RED, review item 11) ----
 * Beta [CFR World.java:512-517 setLightValue -> markBlockNeedsUpdate,
 * Chunk.java:474-486 getChunkData copies the STORED planes]: every stored
 * light change in a column a player watches is published (Packet51/52/53),
 * so the client copy of an already-sent column always converges to the
 * server's stored Sky/Block. CaveWii: a column is snapshotted once
 * (CRPC_CHUNK) and marked sent; generation of a NEIGHBOUR column later
 * lights across the seam into it. This case drives the production startup
 * stream with two fixed anchors (dial 4, 9 columns apart), fixed RNG and
 * fixed admission inputs, a REAL client (production FIFO consumer +
 * the client's own lighting driver), then startup retirement and
 * re-admission by moving, and compares every published client column to
 * the server's stored planes at each boundary. Printed as 'G9X-6 RED:'. */
#include "platform/audio.h"
unsigned server_local_test_publish_debt_w(struct server_local* s,
										  struct server_world* w);
#define GP_IDX(lx, y, lz) ((size_t)(y) + ((size_t)(lz) + (size_t)(lx) * CHUNK_SIZE) * WORLD_HEIGHT)
#define GP_X0 (-16)
#define GP_W 48
#define GP_Z0 (-16)
#define GP_H 33
struct gp_delivery {
	bool have_chunk;
	unsigned chunks, sets, unloads, chunk_update;
	uint8_t last_type;
	unsigned last_update;
	struct column_revision rev;
	uint64_t sky, blk;
};
static struct gp_delivery gp_del[GP_W][GP_H];
static unsigned gp_update_n;
static struct gp_delivery* gp_at(int cx, int cz) {
	int i = cx - GP_X0, j = cz - GP_Z0;
	return i < 0 || i >= GP_W || j < 0 || j >= GP_H ? NULL : &gp_del[i][j];
}
static uint64_t gp_digest_nibbles(const uint8_t* bytes) {
	uint64_t h = UINT64_C(14695981039346656037);
	for(size_t i = 0; i < 32768; i++) {
		h ^= (uint8_t)((bytes[i / 2] >> ((i & 1) ? 4 : 0)) & 15);
		h *= UINT64_C(1099511628211);
	}
	return h;
}
static uint64_t gp_digest_client(int cx, int cz, bool sky) {
	uint64_t h = UINT64_C(14695981039346656037);
	for(int lx = 0; lx < 16; lx++) for(int lz = 0; lz < 16; lz++)
		for(int y = 0; y < WORLD_HEIGHT; y++) {
			struct block_data b = world_get_block(&gstate.world, cx * 16 + lx, y, cz * 16 + lz);
			h ^= sky ? b.sky_light : b.torch_light;
			h *= UINT64_C(1099511628211);
		}
	return h;
}
static void gp_note(int cx, int cz, uint8_t type) {
	struct gp_delivery* d = gp_at(cx, cz);
	if(!d) return;
	d->last_type = type; d->last_update = gp_update_n;
	if(type == CRPC_UNLOAD_CHUNK) { d->unloads++; d->have_chunk = false; }
	if(type == CRPC_SET_BLOCK) d->sets++;
}
static void gp_send(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_CHUNK) {
		int cx = WCOORD_CHUNK_OFFSET(rpc->payload.chunk.x);
		int cz = WCOORD_CHUNK_OFFSET(rpc->payload.chunk.z);
		gp_note(cx, cz, CRPC_CHUNK);
		struct gp_delivery* d = gp_at(cx, cz);
		if(d) {
			d->chunks++; d->have_chunk = true; d->chunk_update = gp_update_n;
			d->rev = rpc->payload.chunk.slot->local_revision;
			d->sky = gp_digest_nibbles(rpc->payload.chunk.slot->lighting_sky);
			d->blk = gp_digest_nibbles(rpc->payload.chunk.slot->lighting_torch);
		}
	} else if(rpc->type == CRPC_UNLOAD_CHUNK) {
		gp_note(rpc->payload.unload_chunk.x, rpc->payload.unload_chunk.z, CRPC_UNLOAD_CHUNK);
	} else if(rpc->type == CRPC_SET_BLOCK) {
		gp_note(WCOORD_CHUNK_OFFSET(rpc->payload.set_block.x),
			WCOORD_CHUNK_OFFSET(rpc->payload.set_block.z), CRPC_SET_BLOCK);
	} else if(rpc->type == CRPC_SET_BLOCK_PACK) {
		for(uint32_t i = 0; i < rpc->payload.set_block_pack.n; i++)
			if(!((rpc->payload.set_block_pack.spawn_mask | rpc->payload.set_block_pack.other_mask) & (1u << i)))
				gp_note(WCOORD_CHUNK_OFFSET(rpc->payload.set_block_pack.rec[i].set_block.x),
					WCOORD_CHUNK_OFFSET(rpc->payload.set_block_pack.rec[i].set_block.z),
					CRPC_SET_BLOCK);
	}
	/* the production client consumes everything queued before this send,
	 * synchronously: identical delivery timing on every run */
	clin_update_drain();
}
/* consumption order (FIFO) for the ordering check */
static unsigned gp_seq, gp_watch_chunk_seq, gp_watch_set_seq;
static int gp_watch_cx = INT32_MIN, gp_watch_cz, gp_watch_x, gp_watch_y, gp_watch_z;
static void gp_consume(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	gp_seq++;
	if(rpc->type == CRPC_CHUNK
	   && WCOORD_CHUNK_OFFSET(rpc->payload.chunk.x) == gp_watch_cx
	   && WCOORD_CHUNK_OFFSET(rpc->payload.chunk.z) == gp_watch_cz)
		gp_watch_chunk_seq = gp_seq;
	if(rpc->type == CRPC_SET_BLOCK && rpc->payload.set_block.x == gp_watch_x
	   && rpc->payload.set_block.y == gp_watch_y
	   && rpc->payload.set_block.z == gp_watch_z)
		gp_watch_set_seq = gp_seq;
}
static void gp_pump(void) {
	clin_update_drain();
	for(unsigned n = 0; !clin_rpc_flush_pending(); n++) {
		assert(n < 10000);
		clin_update_drain();
	}
	clin_update_drain();
	pd_client_light_idle();
}
static void gp_update(struct server_local* s) {
	gp_update_n++;
	server_local_test_update_once(s);
	gp_pump();
}
/* every published client column vs the server's stored planes */
static unsigned long gp_compare(struct server_local* s, const char* phase, bool quiet) {
	unsigned long cells[5] = {0}; /* id meta Sky Block height */
	unsigned columns = 0, stale = 0, loss = 0, copy = 0, other = 0;
	bool first_plane[4] = {!quiet, !quiet, !quiet, !quiet};
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, s->worlds[0].chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it)) {
		struct server_chunk* c = &dict_server_chunks_ref(it)->value;
		int64_t id = dict_server_chunks_ref(it)->key;
		int cx = S_CHUNK_X(id), cz = S_CHUNK_Z(id);
		if(!c->sent || !world_find_chunk(&gstate.world, cx * 16, 0, cz * 16)) continue;
		columns++;
		bool col_stale = false, sky_stale = false;
		for(int lx = 0; lx < 16; lx++) for(int lz = 0; lz < 16; lz++) {
			int x = cx * 16 + lx, z = cz * 16 + lz;
			if((uint8_t)world_get_height(&gstate.world, x, z) != c->heightmap[lx + lz * CHUNK_SIZE]) {
				cells[4]++; col_stale = true;
			}
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				size_t i = GP_IDX(lx, y, lz);
				struct block_data b = world_get_block(&gstate.world, x, y, z);
				const uint8_t sv[4] = {c->ids[i], nibble_read(c->metadata, i),
					nibble_read(c->lighting_sky, i), nibble_read(c->lighting_torch, i)};
				const uint8_t cl[4] = {b.type, b.metadata, b.sky_light, b.torch_light};
				for(int p = 0; p < 4; p++) {
					if(sv[p] == cl[p]) continue;
					cells[p]++; col_stale = true; sky_stale |= p == 2;
					if(first_plane[p]) {
						static const char* const names[4] = {"id", "meta", "Sky", "Block"};
						struct gp_delivery* d = gp_at(cx, cz);
						const struct column_revision* r = c->backing ? &c->backing->revision : NULL;
						printf("G9X-6 RED: %s first divergence (%d,%d,%d) plane=%s expected(server)=%u "
							   "actual(client)=%u; update %u; world generation %u; server column (%d,%d) "
							   "rev content/light/events=%u/%u/%u settled_light_rev=%u populated=%d; "
							   "last CHUNK snapshot at update %u rev content/light/events=%u/%u/%u; "
							   "last delivery event %s at update %u (chunks %u sets %u unloads %u)\n",
							phase, x, y, z, names[p], sv[p], cl[p], gp_update_n, s->worlds[0].generation,
							cx, cz, r ? r->content : 0, r ? r->light : 0, r ? r->events : 0,
							c->backing ? c->backing->settled_light_revision : 0, c->populated,
							d ? d->chunk_update : 0, d ? d->rev.content : 0, d ? d->rev.light : 0,
							d ? d->rev.events : 0, !d ? "none" : d->last_type == CRPC_CHUNK ? "CHUNK" : d->last_type == CRPC_UNLOAD_CHUNK ? "UNLOAD_CHUNK" : "SET_BLOCK", d ? d->last_update : 0,
							d ? d->chunks : 0, d ? d->sets : 0, d ? d->unloads : 0);
						first_plane[p] = false;
					}
				}
			}
		}
		if(!col_stale) continue;
		stale++;
		if(quiet) continue;
		struct gp_delivery* d = gp_at(cx, cz);
		if(!d || !d->have_chunk) { other++; continue; }
		uint64_t client = gp_digest_client(cx, cz, sky_stale);
		uint64_t sent = sky_stale ? d->sky : d->blk;
		uint64_t server = gp_digest_nibbles(sky_stale ? c->lighting_sky : c->lighting_torch);
		if(client == sent && server != sent) loss++;
		else if(client != sent) copy++;
		else other++;
	}
	unsigned long total = cells[0] + cells[1] + cells[2] + cells[3] + cells[4];
	if(quiet) return total;
	printf("%s %s: %u published columns, %u stale; stale cells id %lu meta %lu Sky %lu Block %lu "
		   "height %lu; publication loss (client == delivered snapshot, server newer) %u columns, "
		   "client differs from its delivered snapshot %u, other %u\n",
		total ? "G9X-6 RED:" : "  g9x_publication", phase, columns, stale, cells[0], cells[1],
		cells[2], cells[3], cells[4], loss, copy, other);
	return total;
}
static void case_g9x_publication(void) {
	recipe_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	memset(gp_del, 0, sizeof gp_del);
	gp_update_n = 0;
	struct server_local* s = pd_fixture(WORLD_DIM_OVERWORLD);
	/* real terrain (seed 42) with fixed World.rand, as test_startup_retirement */
	server_world_discard(&s->worlds[0]);
	server_world_test_seed_runtime_jrand(&s->worlds[0].rand, INT64_C(0x5EED2E71));
	string_t path;
	string_init_set_str(path, split_path);
	assert(server_world_create_with_populate_scratch(&s->worlds[0], path,
		WORLD_DIM_OVERWORLD, 42, false, s->world_populate_scratch));
	string_clear(path);
	s->worlds[0].local = s;
	s->worlds[0].session = s;
	/* the client's session WORLD_RESET, stamped like the server's own
	 * (server_local.c session start) */
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset.dimension = WORLD_DIM_OVERWORLD,
		.payload.world_reset.world_index = s->worlds[0].world_index,
		.payload.world_reset.world_generation = s->worlds[0].generation});
	s->render_demo_mob = -1;
	s->input_ack_pending = 1;
	s->player.finished_loading = true;
	s->rand_src = (struct random_gen) {.seed = 12345};
	*server_local_sound_rng() = (struct random_gen) {.seed = 6789};
	*audio_cosmetic_rng() = (struct random_gen) {.seed = 9876};
	server_local_test_seed_math_random(7654);
	/* fixed admission inputs */
	s->split_view_radius = 4;
	s->far_view_distance = 5;
	s->far_lod_terrain_only = false;
	assert(server_local_set_anchor_sep(s, 144));
	assert(server_local_view_radius(s) == 4);
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	server_local_test_far_gen_one_per_update(true);
	server_local_test_startup_retirement(true);
	server_local_test_startup_retirement_limit(8);
	clin_test_set_rpc_observer(gp_send, NULL);
	clin_test_set_consume_observer(gp_consume, NULL);
	unsigned load = 0;
	bool seen_stale = false;
	do {
		gp_update(s);
		/* chronological first divergence: the first update after which a
		 * published client column no longer equals the server's planes */
		if(!seen_stale && gp_compare(s, NULL, true)) {
			seen_stale = true;
			char phase[96];
			snprintf(phase, sizeof phase, "CHRONOLOGICAL FIRST stale update %u (loading)", gp_update_n);
			(void)gp_compare(s, phase, false);
		}
		assert(++load < 1000);
	} while(s->anchor_near_owed[0] || s->anchor_near_owed[1]);
	gp_update(s);
	printf("  g9x_publication: loaded in %u updates\n", load + 1);
	unsigned long stale = gp_compare(s, "after load (already-sent columns, neighbour generation)", false);
	for(unsigned pass = 0; pass < 1000; pass++) {
		uint32_t before = s->startup_retired;
		server_local_test_retire_startup_support(s);
		if(s->startup_retired == before) break;
		assert(pass + 1 < 1000);
	}
	const uint32_t retired = s->startup_retired;
	printf("  g9x_publication: startup retirement retired %u support columns\n", retired);
	stale += gp_compare(s, "after retirement", false);
	/* re-admission: both anchors one column west (the west support ring is
	 * re-admitted from disk), then back */
	const double xs[2] = {-16 + 0.5, 0.5};
	const char* const phases[2] = {"after moving west (re-admission)", "after moving back"};
	for(unsigned k = 0; k < 2; k++) {
		s->player.x = xs[k];
		unsigned updates = 0;
		do {
			gp_update(s);
			assert(++updates < 1000);
		} while(s->anchor_near_owed[0] || s->anchor_near_owed[1]);
		for(int t = 0; t < 4; t++)
			gp_update(s);
		stale += gp_compare(s, phases[k], false);
	}
	/* FIFO pressure: a light change in a sent column while every chunk slot
	 * is held. The republish must be refused without blocking, keep the
	 * debt, and go out once the client frees a slot. */
	struct server_world* w = &s->worlds[0];
	struct server_chunk* cc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
	assert(cc && cc->sent);
	int g = WORLD_HEIGHT - 1;
	for(; g > 1; g--) {
		struct block_data b = {0};
		assert(server_world_get_block(w, 8, g - 1, 8, &b));
		if(b.type != BLOCK_AIR) break;
	}
	struct gp_delivery* cd = gp_at(0, 0);
	const unsigned chunks0 = cd->chunks;
	pd_nheld = 0;
	while(pd_nheld < 256 && (pd_held[pd_nheld] = clin_chunk_slot_try_acquire()))
		pd_nheld++;
	assert(pd_nheld > 0 && pd_nheld < 256);
	assert(server_world_set_block(w, 8, g, 8, (struct block_data) {.type = BLOCK_GLOWSTONE}));
	gp_update(s);
	cc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
	const bool debt_held = cc && cc->pub_debt && cd->chunks == chunks0;
	pd_release_slots();
	gp_update(s);
	cc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
	const bool republished = cc && !cc->pub_debt && cd->chunks == chunks0 + 1;
	printf("  g9x_publication: slots held -> debt kept, no CHUNK %d; released -> one "
		   "republish, debt cleared %d (column (0,0) chunks %u -> %u)\n",
		   debt_held, republished, chunks0, cd->chunks);
	stale += gp_compare(s, "after FIFO pressure released", false);
	/* Ordering: remove the glowstone (light change -> debt), let ONE update
	 * republish the column, and write a NEWER block before the client has
	 * consumed that CHUNK. The newer SET_BLOCK must follow the snapshot in
	 * the FIFO and survive it. */
	assert(server_world_set_block(w, 8, g, 8, (struct block_data) {.type = BLOCK_AIR}));
	gp_pump();
	const unsigned chunks1 = cd->chunks;
	gp_watch_cx = 0; gp_watch_cz = 0;
	gp_watch_x = 8; gp_watch_y = g; gp_watch_z = 8;
	gp_watch_chunk_seq = gp_watch_set_seq = 0;
	gp_update_n++;
	server_local_test_update_once(s); /* no pump: the CHUNK may still be queued */
	const bool snap_sent = cd->chunks == chunks1 + 1;
	assert(server_world_set_block(w, 8, g, 8, (struct block_data) {.type = BLOCK_STONE}));
	gp_pump();
	struct block_data cl = world_get_block(&gstate.world, 8, g, 8);
	const bool ordered = snap_sent && gp_watch_chunk_seq && gp_watch_set_seq
		&& gp_watch_chunk_seq < gp_watch_set_seq && cl.type == BLOCK_STONE;
	printf("  g9x_publication: republished snapshot sent %d, consumed at #%u; newer "
		   "SET_BLOCK consumed at #%u; client id at (8,%d,8) = %u (server %u)\n",
		   snap_sent, gp_watch_chunk_seq, gp_watch_set_seq, g, cl.type, BLOCK_STONE);
	gp_watch_cx = INT32_MIN;
	for(int k = 0; k < 4; k++)
		gp_update(s);
	stale += gp_compare(s, "after the newer write", false);
	check(debt_held && republished, "a full slot pool refuses the republish without "
									"blocking and keeps the debt; one republish follows");
	check(ordered, "a SET_BLOCK written after the republished snapshot is consumed after "
				   "it and is not overwritten by the older snapshot");
	/* [P1] the snapshot baseline must not outlive a later block publication.
	 * The accepted CHUNK holds metadata 0 at a ground cell; a published
	 * SET_BLOCK gives the client metadata 5; an unpublished store returns the
	 * server to 0 (Beta's quiet setBlockMetadata); then debt is raised while
	 * the column's bytes equal the accepted snapshot exactly (the reported
	 * sequence's population notify write restoring A; set directly here so
	 * the bytes are provably equal). The refresh must still send: the client
	 * holds 5. */
	{
		for(int k = 0; k < 4; k++)
			gp_update(s);
		int gm = g - 1;
		struct block_data ground = {0};
		assert(server_world_get_block(w, 4, gm, 4, &ground));
		pd_nheld = 0;
		while(pd_nheld < 256 && (pd_held[pd_nheld] = clin_chunk_slot_try_acquire()))
			pd_nheld++;
		assert(server_world_set_block(w, 4, gm, 4,
			(struct block_data) {.type = ground.type, .metadata = 5}));
		assert(server_world_set_block_metadata_quiet(w, 4, gm, 4, ground.metadata));
		gp_update(s);
		cc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
		assert(cc);
		/* RED on the pre-fix code: the digest was still the accepted
		 * snapshot's here, so the refresh below was skipped. */
		printf("  g9x_publication [P1]: snapshot baseline after the published SET_BLOCK: %s\n",
			   cc->pub_digest_valid ? "still valid" : "invalidated");
		cc->pub_debt = true;
		pd_release_slots();
		for(int k = 0; k < 4; k++)
			gp_update(s);
		struct block_data cl = world_get_block(&gstate.world, 4, gm, 4);
		struct block_data sv = {0};
		assert(server_world_get_block(w, 4, gm, 4, &sv));
		printf("  g9x_publication [P1]: client metadata at (4,%d,4) = %u, server %u\n",
			   gm, cl.metadata, sv.metadata);
		const unsigned long p1 = gp_compare(s, "after a published SET_BLOCK then an unpublished revert", false);
		stale += p1;
		check(p1 == 0 && cl.metadata == sv.metadata,
			  "[P1] a later SET_BLOCK invalidates the snapshot baseline: equal bytes never skip a needed refresh");
	}
	/* [P2] sustained pins must not starve an eligible column: eight pinned
	 * debt columns sort before an unpinned ninth. */
	{
		dict_server_chunks_it_t it;
		for(dict_server_chunks_it(it, w->chunks); !dict_server_chunks_end_p(it);
			dict_server_chunks_next(it))
			dict_server_chunks_ref(it)->value.pub_debt = false;
		for(int x = 0; x <= 7; x++) {
			struct server_chunk* pc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, 0));
			assert(pc && pc->sent);
			pc->pub_debt = true;
		}
		struct server_chunk* ninth = dict_server_chunks_get(w->chunks, S_CHUNK_ID(9, 0));
		assert(ninth && ninth->sent);
		ninth->pub_debt = true;
		ninth->pub_digest_valid = false;
		w->pub_cursor_valid = false;
		int64_t pins[3] = {S_CHUNK_ID(1, 0), S_CHUNK_ID(4, 0), S_CHUNK_ID(6, 0)};
		int64_t* saved_pins = w->light_pins;
		const size_t saved_n = w->light_pin_n, saved_cap = w->light_pin_cap;
		w->light_pins = pins; w->light_pin_n = 3; w->light_pin_cap = 3;
		for(int x = 0; x <= 7; x++)
			assert(server_world_light_pinned(w, x, 0));
		assert(!server_world_light_pinned(w, 9, 0));
		const unsigned before9 = gp_at(9, 0)->chunks;
		for(int k = 0; k < 3; k++) {
			(void)server_local_test_publish_debt_w(s, w);
			gp_pump();
		}
		w->light_pins = saved_pins; w->light_pin_n = saved_n; w->light_pin_cap = saved_cap;
		ninth = dict_server_chunks_get(w->chunks, S_CHUNK_ID(9, 0));
		const bool reached = gp_at(9, 0)->chunks == before9 + 1 && !ninth->pub_debt;
		printf("  g9x_publication [P2]: 8 pinned debt columns, unpinned (9,0) republished %u time(s), debt %d\n",
			   gp_at(9, 0)->chunks - before9, ninth->pub_debt);
		for(int x = 0; x <= 7; x++)
			dict_server_chunks_get(w->chunks, S_CHUNK_ID(x, 0))->pub_debt = false;
		check(reached, "[P2] pinned candidates never starve an eligible column");
	}
	printf("  g9x_publication: total stale client cells %lu\n", stale);
	check(retired > 0, "startup retirement really retired support columns (re-admission exercised)");
	check(stale == 0, "every published client column equals the server's stored planes after "
					  "neighbour generation, retirement and re-admission (no publication loss)");
	clin_test_set_rpc_observer(NULL, NULL);
	clin_test_set_consume_observer(NULL, NULL);
	server_local_test_startup_retirement(false);
	server_local_test_startup_retirement_limit(0);
	server_local_test_far_gen_one_per_update(false);
	server_local_test_expire_near_budget(false);
	teardown(s);
	clin_update_drain();
}

/* ---- hw_workload (ASTRA-G9-REVIEW-2 item 6): the hardware workload ----
 * The production debug driver the Wii measurements arm
 * ("debug.light_queue_stress" / --cw-set debug.light_queue_stress_int=1 on
 * perf_stress_5m, collector --require-g9-evidence), run here through 1,300
 * production updates: every producer engages (roof, torches, a real
 * furnace igniting and going out, a sticky piston pushing glowstone, a
 * blast, a coherent save of a pinned column), the heartbeat v2 counters
 * measure it, and the run stays clean -- zero light stops, ceilings, OOM,
 * checkpoint OOM, flood ceilings, stack drops and post-drain debt (the last
 * update's tile-phase writes legitimately wait for the next drain). */
static int hw_furnace_lit, hw_furnace_out, hw_piston_moving;
static uint8_t hw_furnace_prev;
static void hw_probe(struct server_local* s, struct server_world* w,
					 int phase) {
	if(w != &s->worlds[0] || phase != 1 || s->light_queue_tick <= 0)
		return;
	struct block_data f = {0}, p = {0};
	(void)server_world_get_block(w, s->light_queue_bx + 13, s->light_queue_by,
								 s->light_queue_bz + 2, &f);
	if(f.type == BLOCK_FURNACE_LIT && hw_furnace_prev == BLOCK_FURNACE)
		hw_furnace_lit++;
	if(f.type == BLOCK_FURNACE && hw_furnace_prev == BLOCK_FURNACE_LIT)
		hw_furnace_out++;
	hw_furnace_prev = f.type;
	(void)server_world_get_block(w, s->light_queue_bx + 13, s->light_queue_by,
								 s->light_queue_bz + 8, &p);
	hw_piston_moving += p.type == BLOCK_PISTON_MOVING;
}

static void case_hw_workload(void) {
	struct server_local* s = &S[1];
	open_world(s, s_dir[1], sizeof s_dir[1], "test_light_queue_hw", true, 3);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(s))
		abort();
	mode(1);
	assert(inventory_create(&s->player.inventory, &inventory_logic_player, s,
							&s->player, INVENTORY_SIZE, 0, 0, 0));
	s->player.active_inventory = &s->player.inventory;
	uint8_t hh = 0;
	assert(server_world_get_height(&s->worlds[0], 8, 8, &hh));
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.x = 8.5;
	s->player.y = hh + (double)PLAYER_EYE_HEIGHT;
	s->player.z = 8.5;
	s->player.health = MAX_PLAYER_HEALTH;
	s->render_demo_mob = -1;
	s->light_queue_stress = true;
	s->light_queue_defer = 1; /* the production arming (load + 100) */
	server_world_test_light_stats_reset();
	const uint32_t fceil0 = lighting_flood_ceiling_hits();
	const uint32_t drop0 = stack_oom_drops();
	hw_furnace_lit = hw_furnace_out = hw_piston_moving = 0;
	hw_furnace_prev = 0;
	server_local_test_phase_hook = hw_probe;
	for(int t = 0; t < 1300; t++) {
		s->player.health = MAX_PLAYER_HEALTH;
		server_local_test_update_once(s);
	}
	server_local_test_phase_hook = NULL;
	server_local_test_update_once(s);
	struct server_world_light_hb hb;
	server_world_light_hb(&hb);
	const uint32_t fceil = lighting_flood_ceiling_hits() - fceil0;
	const uint32_t drops = stack_oom_drops() - drop0;
	struct server_world_light_stats st;
	server_world_light_stats(&st);
	printf("  hw workload: driver tick %d; furnace lit %d / out %d; piston "
		   "moving seen %d; drains %u, seeds total %llu\n", s->light_queue_tick,
		   hw_furnace_lit, hw_furnace_out, hw_piston_moving, st.drains,
		   (unsigned long long)st.seeds_total);
	printf("  heartbeat v2 slot 0: seed %u undo %u pin %u front %u ckpt %u B; "
		   "seeds %u; drain %u us / %u cells; tick %u us; ckpt %u us; debt %u; "
		   "faults %u ceil %u oom %u ckpt_fail %u trials %u; flood ceilings "
		   "%u; stack drops %u\n", hb.seed_b[0], hb.undo_b[0], hb.pin_b[0],
		   hb.front_b[0], hb.ckpt_b[0], hb.seeds_pk[0], hb.drain_us[0],
		   hb.drain_pops[0], hb.tick_us, hb.ckpt_us, hb.debt[0], hb.faults,
		   hb.ceil, hb.oom, hb.ckpt_fail, hb.trials, fceil, drops);
	check(s->light_queue_tick > 1200 && hw_furnace_lit >= 2
			  && hw_furnace_out >= 2 && hw_piston_moving > 0 && hb.trials >= 2
			  && hb.ckpt_b[0] > 0 && hb.front_b[0] > 0
			  && hb.drain_pops[0] > 1000,
		  "the hardware workload engages every producer (roof, torches, "
		  "furnace lit/out, piston, blast, coherent pinned save)");
	check(hb.faults == 0 && hb.ceil == 0 && hb.oom == 0 && hb.ckpt_fail == 0
			  && hb.debt[0] == 0 && fceil == 0 && drops == 0
			  && !s->load_failure_terminal,
		  "and stays clean: no stop, ceiling, OOM, checkpoint OOM, flood "
		  "ceiling, stack drop or post-drain debt");
	inventory_destroy(&s->player.inventory, &s->player);
	close_world(s, s_dir[1], false);
}

#include "oracle/lighting/g9_option_b/capture/history_case.h"

/* ---- G9X-3: metadata/ID writes beyond the population quadrant persist ----
 * (Astra review item 8). Beta marks EVERY chunk a setter actually mutates:
 * CFR Chunk.java:238 (setBlockIDWithMetadata) and :269 (setBlockID) set
 * isModified after a non-no-op write, Chunk.java:277-279 (setBlockMetadata)
 * sets it unconditionally; World.getChunkFromChunkCoords reaches ANY
 * resident chunk (ChunkProviderServer.java:98-104) and never invents one
 * (EmptyChunk.java:70-71 swallows a metadata write to an absent chunk). The
 * actual removal callback BlockLeaves.java:40-50 flags leaves (|8) in a
 * clean resident chunk west of the 2x2; that chunk is then drained, evicted
 * and reloaded, in the Overworld AND the Nether, with each dimension in
 * BOTH world slots. Every ID and metadata store the scripts make is checked
 * after reload (the plain setter's metadata reset, Chunk.java:255, too). A
 * no-op setter leaves flags and bytes untouched. A transaction failed
 * mid-script is observed BEFORE its rollback (failed writes store nothing)
 * and after it (bytes, modified, save_pending AND the live save job of
 * pre-existing dirty and pending-save chunks inside and outside the 2x2 are
 * restored; the pending job still settles durable); nothing new becomes
 * resident; the opposite world is untouched. A mismatch prints
 * "G9X-3 RED:". P1-P3 also run under the ordered event recorder below; the
 * original-JAR comparison of those streams is
 * tmp/jobs/loop/G9X-3/jar_dirty_witness.py. */
static void dr_check(bool ok, const char* world, const char* what) {
	if(ok) printf("  PASS: %s: %s\n", world, what);
	else { printf("  G9X-3 RED: %s: %s\n", world, what); s_fail++; }
}

static uint64_t dr_fnv(uint64_t h, const void* p, size_t n) {
	const uint8_t* b = p;
	for(size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
	return h;
}

static struct server_chunk* dr_chunk(struct server_world* w, int cx, int cz) {
	return dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
}

/* Bytes + dirty/save flags of one resident chunk (0 when absent). */
static uint64_t dr_chunk_digest(struct server_world* w, int cx, int cz) {
	struct server_chunk* sc = dr_chunk(w, cx, cz);
	if(!sc) return 0;
	uint64_t h = 0xCBF29CE484222325ull;
	h = dr_fnv(h, sc->ids, 32768);
	h = dr_fnv(h, sc->metadata, 16384);
	h = dr_fnv(h, sc->lighting_sky, 16384);
	h = dr_fnv(h, sc->lighting_torch, 16384);
	h = dr_fnv(h, sc->heightmap, 256);
	h = dr_fnv(h, &sc->modified, sizeof sc->modified);
	h = dr_fnv(h, &sc->save_pending, sizeof sc->save_pending);
	return h;
}

static uint64_t dr_residency(struct server_world* w) {
	uint64_t h = 0xCBF29CE484222325ull;
	for(int cx = -8; cx <= 8; cx++) for(int cz = -8; cz <= 8; cz++) {
		const bool r = server_world_is_chunk_loaded(w, cx, cz);
		h = dr_fnv(h, &r, sizeof r);
	}
	return h;
}

/* Every resident chunk of w: residency, bytes and flags, plus its queue. */
static uint64_t dr_world_digest(struct server_world* w) {
	uint64_t h = dr_residency(w);
	for(int cx = -8; cx <= 8; cx++) for(int cz = -8; cz <= 8; cz++) {
		uint64_t d = dr_chunk_digest(w, cx, cz);
		h = dr_fnv(h, &d, sizeof d);
	}
	h = dr_fnv(h, &w->generation_light_n, sizeof w->generation_light_n);
	return h;
}

static struct block_data dr_block(struct server_world* w, int x, int y, int z) {
	struct block_data b = {0};
	assert(server_world_get_block(w, x, y, z, &b));
	return b;
}

/* Persist every resident chunk (non-evicting) and settle its durability, so
 * each one starts CLEAN (modified false, nothing pending). */
static void dr_clean(struct server_world* w) {
	assert(server_world_light_drain(w) && !server_world_light_pending(w));
	server_world_save_batch_begin(w);
	for(int cx = -8; cx <= 8; cx++) for(int cz = -8; cz <= 8; cz++)
		if(server_world_is_chunk_loaded(w, cx, cz))
			assert(server_world_save_chunk(w, false, cx, cz));
	assert(server_world_save_batch_end(w));
	assert(server_world_sync_regions(w)); /* durability barrier */
	server_world_reconcile_pending_saves(w);
	for(int cx = -8; cx <= 8; cx++) for(int cz = -8; cz <= 8; cz++) {
		struct server_chunk* sc = dr_chunk(w, cx, cz);
		if(sc && (sc->modified || sc->save_pending))
			printf("  dr_clean: chunk (%d,%d) modified %d save_pending %d pinned %d\n",
				cx, cz, sc->modified, sc->save_pending,
				server_world_light_pinned(w, cx, cz));
		assert(!sc || (!sc->modified && !sc->save_pending));
	}
}

/* Drain, evict chunk (cx,cz) through the normal unload save, reload it. */
static void dr_evict_reload(struct server_world* w, int cx, int cz,
	const char* label) {
	assert(server_world_light_drain(w) && !server_world_light_pending(w));
	server_world_save_batch_begin(w);
	assert(server_world_save_chunk(w, true, cx, cz));
	assert(server_world_save_batch_end(w));
	server_world_reconcile_pending_saves(w);
	dr_check(!server_world_is_chunk_loaded(w, cx, cz), label,
		"the outside chunk was actually evicted before the reload");
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(w, cx, cz, &sc) && sc);
	assert(server_world_light_drain(w));
}

/* Box of stone x[-3,2] y[18,22] z[6,10] (no light can move inside it) with
 * five species-tagged leaves in chunk -1 around the quadrant-edge leaf at
 * (0,20,8) (itself species 1, so the plain setter's metadata reset is
 * visible), a wool sentinel (meta 5) for the plain setter to replace, and
 * single stone markers in chunks 1 and 2. Gameplay setters: these writes
 * are setup, not under test. */
#define DR_Y 20
#define DR_Z 8
#define DR_CENTER_META 1
#define DR_WOOL_META 5
static const int dr_leaf[5][4] = { /* x, y, z, species metadata */
	{-1, DR_Y, DR_Z, 0}, {-1, DR_Y - 1, DR_Z, 1}, {-1, DR_Y + 1, DR_Z, 2},
	{-1, DR_Y, DR_Z - 1, 1}, {-1, DR_Y, DR_Z + 1, 2}};
static const int dr_marker_x[2] = {20, 36}; /* chunk 1 (quadrant), chunk 2 (outside) */

static void dr_set(struct server_world* w, int x, int y, int z, uint8_t type,
	uint8_t meta) {
	assert(server_world_set_block(w, x, y, z,
		(struct block_data){.type = type, .metadata = meta}));
	struct block_data b = dr_block(w, x, y, z);
	assert(b.type == type && b.metadata == meta);
}

static void dr_setup(struct server_world* w) {
	for(int x = -3; x <= 2; x++) for(int y = DR_Y - 2; y <= DR_Y + 2; y++)
		for(int z = DR_Z - 2; z <= DR_Z + 2; z++) {
			/* false only for an unchanged block (already stone) */
			(void)server_world_set_block(w, x, y, z,
				(struct block_data){.type = BLOCK_STONE});
			assert(dr_block(w, x, y, z).type == BLOCK_STONE);
		}
	for(int k = 0; k < 2; k++) {
		(void)server_world_set_block(w, dr_marker_x[k], DR_Y, DR_Z,
			(struct block_data){.type = BLOCK_STONE});
		assert(dr_block(w, dr_marker_x[k], DR_Y, DR_Z).type == BLOCK_STONE);
	}
	dr_set(w, 0, DR_Y, DR_Z, BLOCK_LEAVES, DR_CENTER_META);
	for(int i = 0; i < 5; i++)
		dr_set(w, dr_leaf[i][0], dr_leaf[i][1], dr_leaf[i][2], BLOCK_LEAVES,
			(uint8_t)dr_leaf[i][3]);
	dr_set(w, -3, DR_Y - 1, DR_Z, BLOCK_WOOL, DR_WOOL_META);
	assert(server_world_light_drain(w));
	for(int i = 0; i < 5; i++)
		assert(dr_block(w, dr_leaf[i][0], dr_leaf[i][1], dr_leaf[i][2]).metadata
			== dr_leaf[i][3]);
}

/* One block's ID AND metadata against the expected pair. */
static void dr_check_block(struct server_world* w, const char* label,
	const char* phase, int x, int y, int z, int id, int meta) {
	char what[320];
	struct block_data b = dr_block(w, x, y, z);
	snprintf(what, sizeof what, "%s (%d,%d,%d) is id %d meta %d (got id %d meta %d)",
		phase, x, y, z, id, meta, b.type, b.metadata);
	dr_check(b.type == id && b.metadata == meta, label, what);
}

static void dr_check_leaves(struct server_world* w, const char* label,
	const char* phase) {
	for(int i = 0; i < 5; i++)
		dr_check_block(w, label, phase, dr_leaf[i][0], dr_leaf[i][1],
			dr_leaf[i][2], BLOCK_LEAVES, dr_leaf[i][3] | 8);
}

/* ---- G9X-3 ordered event recorder (plan items 3-6) ----
 * P1-P3 run through the G9X-2 recorder of history_case.h (gx_* : ordered
 * api / setter-end / store / light-request rows with raw sequence and
 * __FILE__/__LINE__ sites, read-only hooks) PLUS one row kind of its own:
 * ["modified", cx, cz, value], a transition of a watched chunk's dirty flag.
 * The flag is a plain field write with no hook, so a transition is recorded
 * at the FIRST instrumented event after it (its site is that event's
 * file:line); the original-JAR side records Chunk.isModified at the same
 * granularity. With CW_G9X3_ORDERED_OUT set, each step is written as one
 * JSON line for tmp/jobs/loop/G9X-3/jar_dirty_witness.py, which compares the
 * streams event by event against the instrumented original JAR. Recording
 * never changes what the case asserts. */
#define DR_REC_N 8 /* 8 chunks x 5 planes = the 40 gx_planes slots */
static const int dr_rec_chunk[DR_REC_N][2] = {
	{-2, 0}, {-1, 0}, {0, 0}, {1, 0}, {-2, 1}, {-1, 1}, {0, 1}, {1, 1}};
static struct server_world* dr_rec_w;
static bool dr_rec_flag[DR_REC_N];
static FILE* dr_rec_out;

static void dr_rec_scan(const char* file, int line, const void* caller) {
	if(!gx_record || !dr_rec_w) return;
	for(int k = 0; k < DR_REC_N; k++) {
		const struct server_chunk* sc = dr_chunk(dr_rec_w, dr_rec_chunk[k][0], dr_rec_chunk[k][1]);
		const bool v = sc && sc->modified;
		if(v == dr_rec_flag[k]) continue;
		dr_rec_flag[k] = v;
		gx_push((struct gx_event){.kind = 'm', .cx = dr_rec_chunk[k][0],
			.cz = dr_rec_chunk[k][1], .after = v, .object = dr_rec_w,
			.file = file, .line = line, .caller = caller});
	}
}
static bool dr_rec_light_hook(const struct lighting_history_event* e) {
	dr_rec_scan(e->file, e->line, e->caller);
	return gx_light_observe(e);
}
static bool dr_rec_world_hook(const struct server_world_history_event* e) {
	dr_rec_scan(e->file, e->line, NULL);
	return gx_world_observe(e);
}

static void dr_rec_emit(struct server_world* w, const char* step) {
	FILE* out = dr_rec_out;
	if(!out) return;
	fprintf(out, "{\"dimension\":%d,\"slot\":%u,\"phase\":\"test\",\"step\":\"%s\",\"events\":[",
		(int)w->dimension, (unsigned)w->world_index, step);
	bool sep = false;
	for(size_t k = 0; k < gx_n; k++) {
		const struct gx_event* e = &gx_ev[k];
		if(e->kind == 'r') continue; /* no JVM row; same filter as gx_emit */
		fputs(sep ? "," : "", out);
		sep = true;
		if(e->kind == 'a') {
			fprintf(out, "[\"api\",\"%s\",%u,%d,%d,%d,", gx_api_name(e->api), e->depth, e->x, e->y, e->z);
			if(e->api == 't') fprintf(out, "null,%u]", e->meta);
			else if(e->api == 'p') fprintf(out, "%u,null]", e->id);
			else fprintf(out, "%u,%u]", e->id, e->meta);
		} else if(e->kind == 'e') {
			fprintf(out, "[\"end\",\"%s\",%u,", gx_api_name(e->api), e->depth);
			if(e->api == 't') fprintf(out, "null]");
			else fprintf(out, "%s]", e->result ? "true" : "false");
		} else if(e->kind == 's') {
			fprintf(out, "[\"store\",%u,%d,%d,%u,%u,%u]", e->plane, e->cx, e->cz, e->index, e->before, e->after);
		} else if(e->kind == 'm') {
			fprintf(out, "[\"modified\",%d,%d,%u]", e->cx, e->cz, e->after);
		} else {
			assert(e->kind == 'q');
			fprintf(out, "[\"light\",%d,%d,%d,%d,%d,%d,%d]", e->channel, e->x, e->y, e->z, e->x2, e->y2, e->z2);
		}
	}
	fprintf(out, "],\"anchor\":\"0x%llx\",\"sites\":[",
		(unsigned long long)(uintptr_t)&jrand_set_seed);
	sep = false;
	for(size_t k = 0; k < gx_n; k++) {
		const struct gx_event* e = &gx_ev[k];
		if(e->kind == 'r') continue;
		fprintf(out, "%s[%u,\"%s\",\"0x%llx\"]", sep ? "," : "", e->seq,
			e->file ? gh_site(e->file, e->line) : "?",
			(unsigned long long)(uintptr_t)e->caller);
		sep = true;
	}
	fprintf(out, "],\"flags\":[");
	for(int k = 0; k < DR_REC_N; k++) {
		const struct server_chunk* sc = dr_chunk(w, dr_rec_chunk[k][0], dr_rec_chunk[k][1]);
		fprintf(out, "%s[%d,%d,%d]", k ? "," : "", dr_rec_chunk[k][0], dr_rec_chunk[k][1],
			sc ? (int)sc->modified : -1);
	}
	static const int cells[8][3] = {
		{0, DR_Y, DR_Z}, {-1, DR_Y, DR_Z}, {-1, DR_Y - 1, DR_Z}, {-1, DR_Y + 1, DR_Z},
		{-1, DR_Y, DR_Z - 1}, {-1, DR_Y, DR_Z + 1}, {-2, DR_Y, DR_Z}, {-3, DR_Y - 1, DR_Z}};
	fprintf(out, "],\"cells\":[");
	for(int i = 0; i < 8; i++) {
		const struct block_data b = dr_block(w, cells[i][0], cells[i][1], cells[i][2]);
		fprintf(out, "%s[%d,%d,%d,%u,%u]", i ? "," : "", cells[i][0], cells[i][1],
			cells[i][2], (unsigned)b.type, (unsigned)b.metadata);
	}
	fprintf(out, "],\"resident\":%d}\n", (int)server_world_is_chunk_loaded(w, -3, 0));
}

/* server_world_test_populate_setter_script under the ordered recorder. */
static bool dr_rec_run(struct server_world* w,
	const struct server_world_test_setter_op* ops, size_t n, const char* step) {
	gx_plane_n = 0;
	for(int k = 0; k < DR_REC_N; k++) {
		struct server_chunk* sc = dr_chunk(w, dr_rec_chunk[k][0], dr_rec_chunk[k][1]);
		assert(sc);
		const uint8_t* p[5] = {sc->ids, sc->metadata, sc->lighting_sky,
			sc->lighting_torch, sc->heightmap};
		for(unsigned j = 0; j < 5; j++)
			gx_planes[gx_plane_n++] = (struct gx_plane){p[j], w,
				dr_rec_chunk[k][0], dr_rec_chunk[k][1], j};
		dr_rec_flag[k] = sc->modified;
	}
	dr_rec_w = w;
	gx_n = 0; gx_overflow = false; gx_record = true;
	memset(gx_open, '?', sizeof gx_open);
	lighting_test_history_reset(); server_world_test_history_reset();
	lighting_test_history_hook = dr_rec_light_hook;
	server_world_test_history_hook = dr_rec_world_hook;
	const bool ok = server_world_test_populate_setter_script(w, 0, 0, ops, n);
	/* A transition after the last instrumented event (e.g. at commit). */
	dr_rec_scan(__FILE__, __LINE__, NULL);
	lighting_test_history_hook = NULL; server_world_test_history_hook = NULL;
	gx_record = false;
	assert(!gx_overflow && !lighting_test_history_failed()
		&& !server_world_test_history_failed());
	dr_rec_emit(w, step);
	dr_rec_w = NULL;
	return ok;
}

static struct region_archive* dr_region(struct server_world* w, int cx, int cz) {
	for(size_t k = 0; k < server_world_region_count(w); k++) {
		struct region_archive* ra = server_world_region_at(w, k);
		if(ra->x == CHUNK_REGION_COORD(cx) && ra->z == CHUNK_REGION_COORD(cz))
			return ra;
	}
	return NULL;
}

/* P4 watches chunks x -2..2 of row z 0 (the quadrant's (0,0),(1,0) plus
 * outside -2,-1,2) and the quadrant's (0,1),(1,1). */
#define DR_P4_N 7
static const int dr_p4_chunk[DR_P4_N][2] = {
	{-2, 0}, {-1, 0}, {0, 0}, {1, 0}, {2, 0}, {0, 1}, {1, 1}};
#define DR_P4_OPS 7
#define DR_P4_FAIL 4

struct dr_state {
	uint64_t chunk[DR_P4_N];
	bool modified[DR_P4_N], save_pending[DR_P4_N];
	uint64_t save_job[DR_P4_N];
	size_t gen_n;
	uint64_t residency;
	struct block_data cell[DR_P4_OPS]; /* each P4 op's target cell */
	size_t writes;
};

static int dr_p4_top; /* the height-edge Y of column (8,8) */
static void dr_p4_cell(int i, int out[3]) {
	static const int c[DR_P4_OPS][3] = {
		{-1, DR_Y, DR_Z}, {8, -1, 8}, {36, DR_Y, DR_Z}, {20, DR_Y, DR_Z},
		{-1, DR_Y + 1, DR_Z}, {-2, DR_Y, DR_Z}, {-20, DR_Y, DR_Z}};
	out[0] = c[i][0]; out[1] = c[i][1] < 0 ? dr_p4_top : c[i][1]; out[2] = c[i][2];
}

static void dr_capture(struct server_world* w, struct dr_state* s) {
	memset(s, 0, sizeof *s);
	for(int k = 0; k < DR_P4_N; k++) {
		struct server_chunk* sc = dr_chunk(w, dr_p4_chunk[k][0], dr_p4_chunk[k][1]);
		assert(sc);
		s->chunk[k] = dr_chunk_digest(w, dr_p4_chunk[k][0], dr_p4_chunk[k][1]);
		s->modified[k] = sc->modified;
		s->save_pending[k] = sc->save_pending;
		s->save_job[k] = sc->save_job;
	}
	s->gen_n = w->generation_light_n;
	s->residency = dr_residency(w);
	for(int i = 0; i < DR_P4_OPS; i++) {
		int p[3]; dr_p4_cell(i, p);
		s->cell[i] = dr_block(w, p[0], p[1], p[2]);
	}
}

static bool dr_state_eq(const struct dr_state* a, const struct dr_state* b) {
	bool eq = a->gen_n == b->gen_n && a->residency == b->residency;
	for(int k = 0; k < DR_P4_N; k++)
		eq &= a->chunk[k] == b->chunk[k] && a->modified[k] == b->modified[k]
			&& a->save_pending[k] == b->save_pending[k]
			&& a->save_job[k] == b->save_job[k];
	for(int i = 0; i < DR_P4_OPS; i++)
		eq &= a->cell[i].type == b->cell[i].type
			&& a->cell[i].metadata == b->cell[i].metadata;
	return eq;
}

static struct dr_state dr_p4_seen[DR_P4_OPS];
static void dr_p4_observe(struct server_world* w, size_t i, size_t writes,
	void* ctx) {
	(void)ctx;
	assert(i < DR_P4_OPS);
	dr_capture(w, &dr_p4_seen[i]);
	dr_p4_seen[i].writes = writes;
}

static void dr_test_world(struct server_world* w, struct server_world* other,
	const char* label) {
	char what[320];
	const uint64_t other_before = dr_world_digest(other);
	/* ---- P1: the actual leaf removal callback writes metadata only ---- */
	struct server_chunk* west = dr_chunk(w, -1, 0);
	assert(west && !west->modified && !west->save_pending);
	assert(!dr_chunk(w, 0, 0)->modified);
	assert(!server_world_is_chunk_loaded(w, -3, 0));
	uint64_t res = dr_residency(w);
	const struct server_world_test_setter_op p1[] = {
		{SERVER_WORLD_TEST_SET_BLOCK, 0, DR_Y, DR_Z, BLOCK_STONE, 0},
		/* absent chunk -3: swallowed (EmptyChunk), never loaded */
		{SERVER_WORLD_TEST_SET_BLOCK_METADATA, -40, DR_Y, DR_Z, 0, 7},
	};
	assert(dr_rec_run(w, p1, 2, "p1-leaf-callback"));
	dr_check_leaves(w, label,
		"P1 in memory, leaf callback flagged (BlockLeaves.java:40-50):");
	dr_check_block(w, label, "P1 in memory, quadrant plain-setter replacement "
		"(metadata reset, Chunk.java:255):", 0, DR_Y, DR_Z, BLOCK_STONE, 0);
	dr_check(dr_chunk(w, -1, 0)->modified, label,
		"P1 chunk -1, mutated only by callback metadata stores, is marked modified (Chunk.java:277-279)");
	dr_check(dr_chunk(w, 0, 0)->modified, label,
		"P1 quadrant chunk (0,0), mutated by setBlock, is marked modified (Chunk.java:269)");
	dr_check(dr_residency(w) == res && !server_world_is_chunk_loaded(w, -3, 0), label,
		"P1 no chunk became resident (absent-chunk write swallowed, EmptyChunk.java:70-71)");
	dr_evict_reload(w, -1, 0, label);
	dr_evict_reload(w, 0, 0, label);
	dr_check_leaves(w, label, "P1 after save/evict/reload of chunk -1:");
	dr_check_block(w, label, "P1 after save/evict/reload of chunk (0,0), the "
		"quadrant replacement store:", 0, DR_Y, DR_Z, BLOCK_STONE, 0);
	dr_check(dr_residency(w) == res, label, "P1 residency after reload equals residency before");

	/* ---- P2: ID writes (both ID APIs) into the reloaded chunk, cleaned
	 * first (a reload from a still-retiring record is legitimately dirty) ---- */
	dr_clean(w);
	west = dr_chunk(w, -1, 0);
	assert(west && !west->modified && !west->save_pending);
	dr_check_block(w, label, "P2 setup, the plain setter's target:", -3,
		DR_Y - 1, DR_Z, BLOCK_WOOL, DR_WOOL_META);
	const struct server_world_test_setter_op p2[] = {
		{SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA, -2, DR_Y, DR_Z, BLOCK_WOOL, 14},
		{SERVER_WORLD_TEST_SET_BLOCK, -3, DR_Y - 1, DR_Z, BLOCK_COBBLESTONE, 0},
	};
	assert(dr_rec_run(w, p2, 2, "p2-id-writes"));
	dr_check(dr_chunk(w, -1, 0)->modified, label,
		"P2 chunk -1 is marked modified by setBlockAndMetadata/setBlock (Chunk.java:238/269)");
	dr_evict_reload(w, -1, 0, label);
	dr_check_block(w, label, "P2 after save/evict/reload, setBlockAndMetadata:",
		-2, DR_Y, DR_Z, BLOCK_WOOL, 14);
	dr_check_block(w, label, "P2 after save/evict/reload, setBlock resets the "
		"wool metadata (Chunk.java:255):", -3, DR_Y - 1, DR_Z, BLOCK_COBBLESTONE, 0);
	dr_check_leaves(w, label, "P2 reload still holds the P1 leaf flag:");

	/* ---- P3: setter no-ops change nothing; same-value setBlockMetadata
	 * still marks modified (Chunk.java:277-279 is unconditional) ---- */
	dr_clean(w); /* P1/P2 legitimately dirtied quadrant chunks */
	west = dr_chunk(w, -1, 0);
	assert(west && !west->modified && !west->save_pending);
	uint64_t before = dr_chunk_digest(w, -1, 0);
	uint64_t quad_before[2][2];
	for(int dx = 0; dx < 2; dx++) for(int dz = 0; dz < 2; dz++)
		quad_before[dx][dz] = dr_chunk_digest(w, dx, dz);
	size_t gen_before = w->generation_light_n;
	assert(dr_rec_run(w, p2, 2, "p3-noop")); /* same again */
	bool quad_ok = true;
	for(int dx = 0; dx < 2; dx++) for(int dz = 0; dz < 2; dz++)
		quad_ok &= dr_chunk_digest(w, dx, dz) == quad_before[dx][dz];
	dr_check(dr_chunk_digest(w, -1, 0) == before && quad_ok
		&& w->generation_light_n == gen_before, label,
		"P3 unchanged ID(+metadata) setters leave bytes, modified, save_pending and the light queue untouched");
	const struct server_world_test_setter_op p3b[] = {
		{SERVER_WORLD_TEST_SET_BLOCK_METADATA, -1, DR_Y, DR_Z - 1, 0,
			(uint8_t)(dr_leaf[3][3] | 8)},
	};
	assert(dr_rec_run(w, p3b, 1, "p3-same-meta"));
	dr_check(dr_chunk(w, -1, 0)->modified, label,
		"P3 same-value setBlockMetadata marks chunk -1 modified (Chunk.java:277-279)");
	dr_evict_reload(w, -1, 0, label);
	dr_check_leaves(w, label, "P3 reload still holds the leaf flag:");

	/* ---- P4: a failed transaction over PRE-EXISTING dirty and pending-save
	 * chunks, observed before AND after its rollback ---- */
	dr_clean(w);
	/* An opaque block at or above the stored height raises it
	 * (Chunk.java:256-258 -> func_339_g), a real height/Sky edge. */
	dr_p4_top = -1;
	for(int y = dr_chunk(w, 0, 0)->heightmap[8 + 8 * CHUNK_SIZE];
		y < WORLD_HEIGHT && dr_p4_top < 0; y++)
		if(dr_block(w, 8, y, 8).type != BLOCK_STONE) dr_p4_top = y;
	assert(dr_p4_top >= 0);
	/* Real gameplay edits (opaque -> opaque) dirty all four target chunks. */
	dr_set(w, -3, DR_Y - 2, DR_Z, BLOCK_COBBLESTONE, 0);  /* chunk -1 */
	dr_set(w, 2, DR_Y - 2, DR_Z, BLOCK_COBBLESTONE, 0);   /* quadrant (0,0) */
	dr_set(w, dr_marker_x[0], DR_Y, DR_Z, BLOCK_COBBLESTONE, 0); /* quadrant (1,0) */
	dr_set(w, dr_marker_x[1], DR_Y, DR_Z, BLOCK_COBBLESTONE, 0); /* outside (2,0) */
	assert(server_world_light_drain(w));
	/* A non-evicting save of chunk -1 (outside) and (0,0) (quadrant) INSIDE
	 * an open batch: their records join a publication job that is genuinely
	 * still PENDING, so modified is false and save_pending true. (1,0) and
	 * (2,0) stay dirty; (0,1), (1,1) and -2 stay clean. */
	server_world_save_batch_begin(w);
	assert(server_world_save_chunk(w, false, -1, 0));
	assert(server_world_save_chunk(w, false, 0, 0));
	struct region_archive* ra_w = dr_region(w, -1, 0);
	struct region_archive* ra_q = dr_region(w, 0, 0);
	assert(ra_w && ra_q);
	struct server_chunk* pw = dr_chunk(w, -1, 0);
	struct server_chunk* pq = dr_chunk(w, 0, 0);
	snprintf(what, sizeof what, "P4 setup: chunk -1 (modified %d pending %d "
		"state %d) and (0,0) (modified %d pending %d state %d) hold a PENDING "
		"save job; (1,0) and (2,0) are dirty", pw->modified, pw->save_pending,
		(int)region_archive_save_state(ra_w, pw->save_job), pq->modified,
		pq->save_pending, (int)region_archive_save_state(ra_q, pq->save_job));
	const bool setup_ok = !pw->modified && pw->save_pending && pw->save_job
		&& region_archive_save_state(ra_w, pw->save_job) == REGION_SAVE_PENDING
		&& !pq->modified && pq->save_pending && pq->save_job
		&& region_archive_save_state(ra_q, pq->save_job) == REGION_SAVE_PENDING
		&& dr_chunk(w, 1, 0)->modified && !dr_chunk(w, 1, 0)->save_pending
		&& dr_chunk(w, 2, 0)->modified && !dr_chunk(w, 2, 0)->save_pending
		&& !dr_chunk(w, 0, 1)->modified && !dr_chunk(w, 1, 1)->modified
		&& !dr_chunk(w, -2, 0)->modified;
	dr_check(setup_ok, label, what);
	assert(setup_ok);
	struct dr_state s_before;
	dr_capture(w, &s_before);
	const uint64_t job_w = pw->save_job, job_q = pq->save_job;
	const struct server_world_test_setter_op p4[DR_P4_OPS] = {
		/* recorded (before the injected failure) */
		{SERVER_WORLD_TEST_SET_BLOCK_METADATA, -1, DR_Y, DR_Z, 0, 3},     /* pending, outside */
		{SERVER_WORLD_TEST_SET_BLOCK, 8, dr_p4_top, 8, BLOCK_STONE, 0},  /* pending, quadrant, height edge */
		{SERVER_WORLD_TEST_SET_BLOCK_METADATA, 36, DR_Y, DR_Z, 0, 6},     /* dirty, outside */
		{SERVER_WORLD_TEST_SET_BLOCK_METADATA, 20, DR_Y, DR_Z, 0, 9},     /* dirty, quadrant */
		/* failed (after the injected failure) */
		{SERVER_WORLD_TEST_SET_BLOCK_METADATA, -1, DR_Y + 1, DR_Z, 0, 5},
		{SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA, -2, DR_Y, DR_Z, BLOCK_STONE, 0},
		{SERVER_WORLD_TEST_SET_BLOCK_METADATA, -20, DR_Y, DR_Z, 0, 4},    /* clean chunk -2, never recorded */
	};
	size_t at_fail = 0, after = 0;
	memset(dr_p4_seen, 0, sizeof dr_p4_seen);
	assert(server_world_test_populate_setter_script_fail(w, 0, 0, p4, DR_P4_OPS,
		DR_P4_FAIL, &at_fail, &after, dr_p4_observe, NULL));
	/* Before the rollback: the recorded stores are live, every chunk they
	 * mutated is marked modified with its pending record superseded
	 * (Chunk.java:238/269/277-279 + server_world_chunk_touch), and NOTHING
	 * after the injected failure stored a byte, flag, job or queue entry. */
	const struct dr_state* live = &dr_p4_seen[DR_P4_FAIL - 1];
	{
		static const int want[4][2] = {
			{BLOCK_LEAVES, 3}, {BLOCK_STONE, 0}, {BLOCK_COBBLESTONE, 6},
			{BLOCK_COBBLESTONE, 9}};
		for(int i = 0; i < 4; i++) {
			int p[3]; dr_p4_cell(i, p);
			snprintf(what, sizeof what, "P4 before rollback, recorded store %d "
				"(%d,%d,%d) is id %d meta %d (got id %d meta %d)", i, p[0], p[1],
				p[2], want[i][0], want[i][1], live->cell[i].type,
				live->cell[i].metadata);
			dr_check(live->cell[i].type == want[i][0]
				&& live->cell[i].metadata == want[i][1], label, what);
		}
		/* dr_p4_chunk index of the chunk each recorded store mutated:
		 * -1 -> 1, (0,0) -> 2, (2,0) -> 4, (1,0) -> 3 */
		static const int touched[4] = {1, 2, 4, 3};
		for(int i = 0; i < 4; i++) {
			int k = touched[i];
			snprintf(what, sizeof what, "P4 before rollback, chunk (%d,%d) mutated "
				"by recorded store %d is modified (now %d) and its pending record "
				"superseded (save_pending now %d)", dr_p4_chunk[k][0],
				dr_p4_chunk[k][1], i, live->modified[k], live->save_pending[k]);
			dr_check(live->modified[k] && !live->save_pending[k], label, what);
		}
	}
	snprintf(what, sizeof what, "P4 the four recorded writes were counted (%u at "
		"the failure, %u after the failed ops, %u observed)", (unsigned)at_fail,
		(unsigned)after, (unsigned)live->writes);
	dr_check(at_fail == 4 && after == at_fail && live->writes == 4, label, what);
	for(int i = DR_P4_FAIL; i < DR_P4_OPS; i++) {
		int p[3]; dr_p4_cell(i, p);
		snprintf(what, sizeof what, "P4 before rollback, failed op %d at (%d,%d,%d) "
			"stored no byte, flag, job or queue entry (cell id %d meta %d, was id "
			"%d meta %d, writes %u)", i, p[0], p[1], p[2],
			dr_p4_seen[i].cell[i].type, dr_p4_seen[i].cell[i].metadata,
			s_before.cell[i].type, s_before.cell[i].metadata,
			(unsigned)dr_p4_seen[i].writes);
		dr_check(dr_state_eq(&dr_p4_seen[i], live)
			&& dr_p4_seen[i].writes == live->writes
			&& dr_p4_seen[i].cell[i].type == s_before.cell[i].type
			&& dr_p4_seen[i].cell[i].metadata == s_before.cell[i].metadata,
			label, what);
	}
	/* After the rollback: every captured flag is RESTORED (not cleared), the
	 * save jobs are kept and still pending, bytes/queue/residency are back. */
	struct dr_state s_after;
	dr_capture(w, &s_after);
	for(int k = 0; k < DR_P4_N; k++) {
		snprintf(what, sizeof what, "P4 rollback restores chunk (%d,%d): bytes %s, "
			"modified %d (was %d), save_pending %d (was %d), save job %s",
			dr_p4_chunk[k][0], dr_p4_chunk[k][1],
			s_after.chunk[k] == s_before.chunk[k] ? "equal" : "DIFFER",
			s_after.modified[k], s_before.modified[k], s_after.save_pending[k],
			s_before.save_pending[k],
			s_after.save_job[k] == s_before.save_job[k] ? "kept" : "CHANGED");
		dr_check(s_after.chunk[k] == s_before.chunk[k]
			&& s_after.modified[k] == s_before.modified[k]
			&& s_after.save_pending[k] == s_before.save_pending[k]
			&& s_after.save_job[k] == s_before.save_job[k], label, what);
	}
	dr_check(dr_state_eq(&s_after, &s_before), label,
		"P4 rollback restores every target cell, the light queue and residency");
	dr_check(region_archive_save_state(ra_w, job_w) == REGION_SAVE_PENDING
		&& region_archive_save_state(ra_q, job_q) == REGION_SAVE_PENDING, label,
		"P4 the restored save jobs of chunk -1 and (0,0) are still PENDING after the rollback");
	/* The kept jobs then settle: close the batch, durability barrier. */
	assert(server_world_save_batch_end(w));
	assert(server_world_sync_regions(w));
	server_world_reconcile_pending_saves(w);
	snprintf(what, sizeof what, "P4 the kept jobs settle DURABLE (states %d/%d) "
		"and clear save_pending without re-dirtying (chunk -1 modified %d pending %d, "
		"(0,0) modified %d pending %d)",
		(int)region_archive_save_state(ra_w, job_w),
		(int)region_archive_save_state(ra_q, job_q), dr_chunk(w, -1, 0)->modified,
		dr_chunk(w, -1, 0)->save_pending, dr_chunk(w, 0, 0)->modified,
		dr_chunk(w, 0, 0)->save_pending);
	dr_check(region_archive_save_state(ra_w, job_w) == REGION_SAVE_DURABLE
		&& region_archive_save_state(ra_q, job_q) == REGION_SAVE_DURABLE
		&& !dr_chunk(w, -1, 0)->modified && !dr_chunk(w, -1, 0)->save_pending
		&& !dr_chunk(w, 0, 0)->modified && !dr_chunk(w, 0, 0)->save_pending,
		label, what);
	/* What reached disk for chunk -1 is the pre-transaction record. */
	dr_evict_reload(w, -1, 0, label);
	dr_check_leaves(w, label, "P4 after rollback + save/evict/reload, the "
		"rolled-back leaf metadata stayed out of the save:");
	dr_check_block(w, label, "P4 after rollback + save/evict/reload, the "
		"failed setBlockAndMetadata stayed out:", -2, DR_Y, DR_Z, BLOCK_WOOL, 14);
	dr_check_block(w, label, "P4 after rollback + save/evict/reload, the "
		"pre-existing gameplay edit is kept:", -3, DR_Y - 2, DR_Z,
		BLOCK_COBBLESTONE, 0);
	dr_check(dr_world_digest(other) == other_before, label,
		"the opposite world's chunks, flags, residency and light queue are untouched");
}

static void dr_scenario(unsigned ow_slot, const char* out_dir) {
	if(out_dir) {
		char path[512];
		snprintf(path, sizeof path, "%s/cave-overworld-slot%u.jsonl", out_dir, ow_slot);
		dr_rec_out = fopen(path, "wb"); assert(dr_rec_out);
	}
	char name[64]; snprintf(name, sizeof name, "g9x_dirty_reload_ow%u", ow_slot);
	test_fixture_dir(name, s_dir[1], sizeof s_dir[1]); MKDIR(s_dir[1]);
	char rdir[300]; snprintf(rdir, sizeof rdir, "%s/region", s_dir[1]); MKDIR(rdir);
	char ndir[260]; snprintf(ndir, sizeof ndir, "%s_slot1", s_dir[1]); MKDIR(ndir);
	snprintf(rdir, sizeof rdir, "%s/region", ndir); MKDIR(rdir);
	memset(&S[1], 0, sizeof S[1]); dict_entity_init(S[1].entities);
	if(!server_local_te_tables_init(&S[1])) abort(); /* IMP-TE-CAPS */
	string_t level; string_init_set_str(level, s_dir[1]);
	server_world_create(&S[1].worlds[0], level,
		ow_slot == 0 ? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER, 4242, false);
	string_clear(level); string_init_set_str(level, ndir);
	server_world_create(&S[1].worlds[1], level,
		ow_slot == 0 ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD, 4242, false);
	string_clear(level);
	struct server_world* s0 = &S[1].worlds[0];
	struct server_world* s1 = &S[1].worlds[1];
	s0->session = s0->local = s1->session = s1->local = &S[1];
	s0->world_index = 0; s1->world_index = 1;
	jrand_set_seed(&s0->rand, 4242); jrand_set_seed(&s1->rand, 4242); mode(1);
	for(int x = -2; x <= 2; x++) for(int z = -1; z <= 2; z++) {
		struct server_chunk* sc;
		assert(server_world_load_chunk(s0, x, z, &sc));
		assert(server_world_load_chunk(s1, x, z, &sc));
	}
	assert(server_world_light_drain(s0) && server_world_light_drain(s1));
	dr_setup(s0); dr_setup(s1);
	dr_clean(s0); dr_clean(s1);
	char label0[48], label1[48];
	snprintf(label0, sizeof label0, "%s(slot0)",
		s0->dimension == WORLD_DIM_NETHER ? "nether" : "overworld");
	snprintf(label1, sizeof label1, "%s(slot1)",
		s1->dimension == WORLD_DIM_NETHER ? "nether" : "overworld");
	dr_test_world(s0, s1, label0);
	dr_clean(s0);
	dr_test_world(s1, s0, label1);
	if(dr_rec_out) { assert(fclose(dr_rec_out) == 0); dr_rec_out = NULL; }
	server_world_discard(s1);
	test_fixture_dir_cleanup(ndir);
	close_world(&S[1], s_dir[1], false);
}

static void case_g9x_dirty_reload(void) {
	const char* out_dir = getenv("CW_G9X3_ORDERED_OUT");
	dr_scenario(0, out_dir); /* Overworld slot 0, Nether slot 1 */
	dr_scenario(1, out_dir); /* Nether slot 0, Overworld slot 1 */
}

/* ---- G9X-4: population owns its snow pass and every notification ----
 * (Astra review item 12). The snow loop is the end of
 * ChunkProviderGenerate.populate (CFR ChunkProviderGenerate.java:553-564):
 * each World.setBlockWithNotify (World.java:332-337) and its recursive
 * neighbour notifications -- BlockStationary wake-ups / BlockFlowing
 * (BlockFlowing.java:157-187), lava hardening, BlockSand rescheduling
 * (BlockSand.java:44-57) through World.scheduleUpdateTick (World.java:827-843)
 * -- are part of population. CaveWii's rollback is its own infrastructure;
 * this case requires that EVERY region / notification-driver / staged-drop /
 * outside-chunk record allocation attempt during that work, injected one at
 * a time, fails the population transaction (never committed, never
 * populated), observed live BEFORE the rollback, and that the rollback
 * restores IDs, metadata, height, Sky, Block light, modified/save_pending of
 * pre-existing dirty and pending-save chunks inside AND outside the 2x2,
 * populated flags, residency, the named RNG states (World.rand, hellRNG,
 * session rand, Math.random), the scheduled ticks, the whole mergeable
 * region queue, entities, the lava-mix effect count and the light-fault
 * latch exactly. The retry must equal the uninterrupted run's final state
 * AND its ordered setter/notification/store/region history (so no
 * notification is duplicated).
 *  A: a scripted scene through the production snow-and-commit tail (seam):
 *     Overworld snow on 5 cold columns wakes still water, reschedules sand,
 *     hardens lava and starts a 10-cell hardening chain into chunk 2, OUTSIDE
 *     the quadrant; the Nether (no snow pass, as in production) runs the same
 *     cascade from decorate-stage netherrack writes; the Overworld also makes
 *     one decorate-stage height-raising write (func_339_g's Sky request,
 *     whose failure latches light_fault). A region left PENDING at the
 *     transaction's start is merged into by the first snow cell. Both
 *     dimensions, each in both world slots, the opposite world untouched.
 *  B: the production populate of the coldest real seed-4242 quadrant near
 *     the origin, triggered by its real 4th chunk load, failing at every
 *     snow-phase point in turn, each retried through the production entry.
 * A mismatch prints "G9X-4 RED:". */
#define SO_Y 100
static long so_pass, so_red;
/* Process-global inputs every fixture starts from: the Math.random stream
 * and the lava-mix effect count (counted from the fixture's start). */
static struct server_local_math_random_state so_math0;
static uint32_t so_lava_base;
static void so_check(bool ok, const char* label, const char* what) {
	if(ok) { so_pass++; return; }
	so_red++; s_fail++;
	printf("  G9X-4 RED: %s: %s\n", label, what);
}

struct so_state {
	uint64_t chunks, regions, world_rng, session_rng, ticks;
	size_t region_n, tick_n, entities;
	bool light_fault, scratch;
	uint8_t reason;
	uint32_t lava_total;
};

static uint64_t so_u64(uint64_t h, uint64_t v) { return dr_fnv(h, &v, sizeof v); }
/* The captured chunk window: chunks so_cx0-3..so_cx0+4 / so_cz0-2..so_cz0+3
 * (the scene's quadrant is (0,0); B moves it onto the real cold quadrant). */
static int so_cx0, so_cz0;

static void so_capture(struct server_world* w, struct so_state* o) {
	memset(o, 0, sizeof *o);
	uint64_t h = 0xCBF29CE484222325ull;
	for(int cx = so_cx0 - 3; cx <= so_cx0 + 4; cx++)
		for(int cz = so_cz0 - 2; cz <= so_cz0 + 3; cz++) {
		struct server_chunk* sc = dr_chunk(w, cx, cz);
		h = so_u64(h, dr_chunk_digest(w, cx, cz));
		h = so_u64(h, sc ? 2u + sc->populated : 0u);
	}
	o->chunks = h;
	h = 0xCBF29CE484222325ull;
	int r[7];
	for(size_t i = 0; server_world_test_generation_light_region(w, i, r); i++) {
		h = dr_fnv(h, r, sizeof r);
		o->region_n = i + 1;
	}
	o->regions = h;
	struct server_local* s = w->session;
	h = so_u64(0xCBF29CE484222325ull, w->rand.seed);
	o->world_rng = so_u64(h, w->hell_populate_rng.seed);
	h = so_u64(0xCBF29CE484222325ull, s->rand_src.seed);
	struct server_local_math_random_state m;
	server_local_math_random_capture(&m);
	h = so_u64(h, m.rng.seed);
	o->session_rng = so_u64(h, m.initialized);
	h = so_u64(0xCBF29CE484222325ull, w->scheduled_tick_head);
	h = so_u64(h, w->scheduled_tick_drops);
	for(size_t i = 0; i < w->scheduled_tick_count; i++) {
		const struct scheduled_tick* t = &w->scheduled_ticks[
			(w->scheduled_tick_head + i) % w->scheduled_tick_capacity];
		h = so_u64(h, (uint64_t)(uint32_t)t->x << 32 | (uint32_t)t->z);
		h = so_u64(h, (uint64_t)(uint32_t)t->y << 40 | (uint64_t)t->fire_at_tick << 8
			| t->block_type);
	}
	o->ticks = h;
	o->tick_n = w->scheduled_tick_count;
	o->entities = dict_entity_size(s->entities);
	o->light_fault = w->light_fault;
	o->reason = w->light_fault_reason;
	o->lava_total = fluid_block_lava_mix_effect_total() - so_lava_base;
	o->scratch = server_world_test_populate_scratch_in_use(w);
}

/* The world's own components (the opposite world shares the session's
 * RNGs, entities and the global effect count with the world under test). */
static const char* so_diff_world(const struct so_state* a, const struct so_state* b) {
	if(a->chunks != b->chunks) return "chunk bytes/flags/populated/residency";
	if(a->region_n != b->region_n || a->regions != b->regions) return "region queue";
	if(a->world_rng != b->world_rng) return "World.rand / hellRNG";
	if(a->tick_n != b->tick_n || a->ticks != b->ticks) return "scheduled ticks";
	if(a->light_fault != b->light_fault || a->reason != b->reason) return "light-fault latch";
	if(a->scratch != b->scratch) return "population scratch ownership";
	return NULL;
}

/* Every component; names the first one that differs (NULL = equal). */
static const char* so_diff(const struct so_state* a, const struct so_state* b) {
	const char* d = so_diff_world(a, b);
	if(d) return d;
	if(a->session_rng != b->session_rng) return "session rand / Math.random";
	if(a->entities != b->entities) return "entities";
	if(a->lava_total != b->lava_total) return "lava-mix effect total";
	if(a->scratch != b->scratch) return "population scratch ownership";
	return NULL;
}

/* Ordered setter / notification / store / region / request history. With
 * so_span set, only events inside the "populate" request are hashed (the
 * real load wraps the populate; the retry calls it directly). */
/* Ordered raw tape for tmp/jobs/loop/G9X-4/jar_snow_witness.py (only with
 * CW_G9X4_ORDERED_OUT; recording never changes what the case asserts). The
 * world under test of the BASE fixture writes the G9X-1 raw-tape format of
 * history_case.h (gh_raw_observe / gh_observe: allocate, store, api-*,
 * setter-end, region-request/reject/merge(+target)/admit/pop, markers) to
 * <dir>/cave-dim<d>-slot<s>/raw.jsonl.gz, read unchanged by
 * tests/oracle/lighting/g9_option_b/compare/first_divergence.py cave_rows(),
 * PLUS, in the same raw sequence:
 *   "rng"      every java.util.Random call (jrand_test_hook): named owner
 *              world.rand / hell.rng / math.random (the session
 *              java.lang.Math stream, verified against its captured state)
 *              or local, API, argument, 48-bit pre/post, result, file:line;
 *   "modified" a transition of a resident chunk's dirty flag;
 *   "tick"     a scheduled-tick entry appended to the world's queue
 *              (x, y, z, block, delay = fire_at_tick - current_tick).
 * The plain-field transitions (flags, tick ring) are revealed at the FIRST
 * instrumented event after them, as the JAR side reveals isModified and
 * World.scheduledTickSet. External-call boundaries ("pre", after every
 * population World.setBlockWithNotify returned, "end") write the 24 resident
 * chunks' IDs/meta/Sky/Block/height and the RNG / region-tail / tick-queue /
 * flag state to boundaries.jsonl for the independent array guard. */
#define SO_RX0 (-2)
#define SO_RX1 3
#define SO_RZ0 (-1)
#define SO_RZ1 2
#define SO_MATH_SEED INT64_C(0x5EED6A9D1234)
static struct server_world* so_tape_w;
static bool so_tape_failed;
static bool so_tape_flag[SO_RX1 - SO_RX0 + 1][SO_RZ1 - SO_RZ0 + 1];
static size_t so_tape_tick_seen, so_tape_tick_head;
static unsigned so_tape_boundaries;
static FILE* so_tape_bfile;
static char so_tape_dir[600];
static void (*so_tape_prev_jrand)(const struct jrand_test_event*);

static void so_tape_scan(void) {
	struct server_world* w = so_tape_w;
	if(!w || !gh_raw) return;
	for(int cx = SO_RX0; cx <= SO_RX1; cx++) for(int cz = SO_RZ0; cz <= SO_RZ1; cz++) {
		const struct server_chunk* sc = dr_chunk(w, cx, cz);
		const bool v = sc && sc->modified;
		bool* f = &so_tape_flag[cx - SO_RX0][cz - SO_RZ0];
		if(v == *f) continue;
		*f = v;
		assert(gzprintf(gh_raw, "{\"seq\":%u,\"op\":\"modified\",\"dimension\":%d,"
			"\"cx\":%d,\"cz\":%d,\"value\":%d}\n", ++gh_raw_seq, (int)w->dimension,
			cx, cz, (int)v) > 0);
	}
	if(w->scheduled_tick_head != so_tape_tick_head || w->scheduled_tick_count < so_tape_tick_seen) {
		/* never expected in the step: any removal/reorder is a row of its own */
		assert(gzprintf(gh_raw, "{\"seq\":%u,\"op\":\"tick-reset\",\"dimension\":%d,"
			"\"head\":%zu,\"count\":%zu}\n", ++gh_raw_seq, (int)w->dimension,
			w->scheduled_tick_head, w->scheduled_tick_count) > 0);
		so_tape_tick_head = w->scheduled_tick_head;
		so_tape_tick_seen = w->scheduled_tick_count;
	}
	for(; so_tape_tick_seen < w->scheduled_tick_count; so_tape_tick_seen++) {
		const struct scheduled_tick* t = &w->scheduled_ticks[
			(w->scheduled_tick_head + so_tape_tick_seen) % w->scheduled_tick_capacity];
		assert(gzprintf(gh_raw, "{\"seq\":%u,\"op\":\"tick\",\"dimension\":%d,"
			"\"xyz\":[%d,%d,%d],\"block\":%u,\"delay\":%lld}\n", ++gh_raw_seq,
			(int)w->dimension, t->x, t->y, t->z, (unsigned)t->block_type,
			(long long)t->fire_at_tick - (long long)w->current_tick) > 0);
	}
}

static void so_tape_jrand(const struct jrand_test_event* e) {
	if(!so_tape_w || !gh_raw) return;
	so_tape_scan();
	const struct server_world* w = so_tape_w;
	const char* owner = e->rng == &w->rand ? "world.rand"
		: e->rng == &w->hell_populate_rng ? "hell.rng" : "local";
	if(e->file && strstr(e->file, "server_local.c")) {
		struct server_local_math_random_state m;
		server_local_math_random_capture(&m);
		/* the java.lang.Math stream: its live state IS this receiver's post */
		if(!(m.initialized && (uint64_t)m.rng.seed == e->post)) so_tape_failed = true;
		else owner = "math.random";
	}
	assert(gzprintf(gh_raw, "{\"seq\":%u,\"dimension\":%d,\"op\":\"rng\",\"owner\":\"%s\","
		"\"rng\":\"0x%llx\",\"api\":%d,\"arg\":%lld,\"has_pre\":%d,\"pre\":%llu,"
		"\"post\":%llu,\"result\":%llu,\"site\":\"%s\",\"caller\":\"0x%llx\"}\n",
		++gh_raw_seq, (int)w->dimension, owner, (unsigned long long)(uintptr_t)e->rng,
		(int)e->api, (long long)e->arg, (int)e->has_pre, (unsigned long long)e->pre,
		(unsigned long long)e->post, (unsigned long long)e->result,
		e->file ? gh_site(e->file, e->line) : "?",
		(unsigned long long)(uintptr_t)e->caller) > 0);
}

static const struct scheduled_tick* so_tick_at(const struct server_world* w, size_t i) {
	return &w->scheduled_ticks[(w->scheduled_tick_head + i) % w->scheduled_tick_capacity];
}

static void so_tape_boundary(const char* name) {
	struct server_world* w = so_tape_w;
	if(!w) return;
	char path[700];
	snprintf(path, sizeof path, "%s/boundary-%s.bin.gz", so_tape_dir, name);
	gzFile g = gzopen(path, "wb1");
	assert(g);
	for(int cx = SO_RX0; cx <= SO_RX1; cx++) for(int cz = SO_RZ0; cz <= SO_RZ1; cz++) {
		const struct server_chunk* sc = dr_chunk(w, cx, cz);
		assert(sc);
		assert(gzwrite(g, sc->ids, 32768) == 32768);
		assert(gzwrite(g, sc->metadata, 16384) == 16384);
		assert(gzwrite(g, sc->lighting_sky, 16384) == 16384);
		assert(gzwrite(g, sc->lighting_torch, 16384) == 16384);
		assert(gzwrite(g, sc->heightmap, 256) == 256);
	}
	assert(gzclose(g) == Z_OK);
	struct server_local_math_random_state m;
	server_local_math_random_capture(&m);
	FILE* f = so_tape_bfile;
	fprintf(f, "{\"name\":\"%s\",\"file\":\"boundary-%s.bin.gz\",\"seq\":%u,"
		"\"rand\":%llu,\"hell\":", name, name, gh_raw_seq,
		(unsigned long long)w->rand.seed);
	if(w->dimension == WORLD_DIM_NETHER)
		fprintf(f, "%llu", (unsigned long long)w->hell_populate_rng.seed);
	else fputs("null", f);
	fprintf(f, ",\"math\":%llu,\"regions\":%zu,\"tail\":[",
		(unsigned long long)m.rng.seed, w->generation_light_n);
	const size_t n = w->generation_light_n, first = n > 5 ? n - 5 : 0;
	for(size_t i = first; i < n; i++) {
		int r[7];
		assert(server_world_test_generation_light_region(w, i, r));
		fprintf(f, "%s[%d,%d,%d,%d,%d,%d,%d]", i > first ? "," : "", r[0] - 2,
			r[1], r[2], r[3], r[4], r[5], r[6]);
	}
	/* Beta's TreeSet order: scheduled time, then insertion (a stable sort) */
	fputs("],\"ticks\":[", f);
	static size_t order[4096];
	const size_t c = w->scheduled_tick_count;
	assert(c <= sizeof order / sizeof order[0]);
	for(size_t i = 0; i < c; i++) {
		size_t j = i;
		while(j > 0 && so_tick_at(w, order[j - 1])->fire_at_tick > so_tick_at(w, i)->fire_at_tick) {
			order[j] = order[j - 1];
			j--;
		}
		order[j] = i;
	}
	for(size_t i = 0; i < c; i++) {
		const struct scheduled_tick* t = so_tick_at(w, order[i]);
		fprintf(f, "%s[%d,%d,%d,%u,%lld]", i ? "," : "", t->x, t->y, t->z,
			(unsigned)t->block_type, (long long)t->fire_at_tick - (long long)w->current_tick);
	}
	fputs("],\"flags\":[", f);
	bool sep = false;
	for(int cx = SO_RX0; cx <= SO_RX1; cx++) for(int cz = SO_RZ0; cz <= SO_RZ1; cz++) {
		fprintf(f, "%s[%d,%d,%d]", sep ? "," : "", cx, cz, (int)dr_chunk(w, cx, cz)->modified);
		sep = true;
	}
	fputs("]}\n", f);
	assert(gh_raw && gzprintf(gh_raw, "{\"seq\":%u,\"op\":\"boundary\",\"dimension\":%d,"
		"\"name\":\"%s\"}\n", ++gh_raw_seq, (int)w->dimension, name) > 0);
}

static void so_tape_notify_boundary(struct server_world* w, int x, int y, int z) {
	(void)x; (void)y; (void)z;
	if(w != so_tape_w) return;
	so_tape_scan();
	char name[32];
	snprintf(name, sizeof name, "op%u", ++so_tape_boundaries);
	so_tape_boundary(name);
}

static uint64_t so_hist;
static size_t so_hist_n, so_hist_api;
static bool so_span, so_in_span;
static struct server_world* so_pre_w;
static struct so_state so_pre_state;
static bool so_pre_armed, so_pre_seen;
static bool so_light_hook(const struct lighting_history_event* e) {
	if(so_tape_w) {
		so_tape_scan();
		if(!gh_raw_observe(e)) so_tape_failed = true;
	}
	if(so_span && !so_in_span) return true;
	uint64_t h = so_hist;
	for(const char* c = e->operation; c && *c; c++) h = so_u64(h, (uint8_t)*c);
	const int64_t v[] = {e->x, e->y, e->z, e->x2, e->y2, e->z2, e->channel,
		e->before, e->after, e->depth, e->result, (int64_t)e->index,
		(int64_t)e->before_depth, (int64_t)e->after_depth};
	so_hist = dr_fnv(h, v, sizeof v);
	so_hist_n++;
	if(e->operation && strncmp(e->operation, "api-", 4) == 0) so_hist_api++;
	return true;
}
static bool so_world_hook(const struct server_world_history_event* e) {
	if(so_tape_w) {
		so_tape_scan();
		if(!gh_observe(e)) so_tape_failed = true;
	}
	const bool populate = strcmp(e->operation, "populate") == 0;
	if(populate && strcmp(e->edge, "begin") == 0) {
		if(so_pre_armed && e->world == so_pre_w && !so_pre_seen) {
			so_capture(so_pre_w, &so_pre_state); /* the real pre-populate state */
			so_pre_seen = true;
		}
		so_in_span = true;
	}
	if(!so_span || so_in_span) {
		uint64_t h = so_hist;
		for(const char* c = e->operation; c && *c; c++) h = so_u64(h, (uint8_t)*c);
		for(const char* c = e->edge; c && *c; c++) h = so_u64(h, (uint8_t)*c);
		const int64_t v[] = {e->x, e->y, e->z, e->before_id, e->before_meta,
			e->after_id, e->after_meta};
		so_hist = dr_fnv(h, v, sizeof v);
		so_hist_n++;
	}
	if(populate && strcmp(e->edge, "end") == 0) so_in_span = false;
	return true;
}
static void so_hist_begin(bool span) {
	so_hist = 0xCBF29CE484222325ull; so_hist_n = so_hist_api = 0;
	so_span = span; so_in_span = false;
	lighting_test_history_reset(); server_world_test_history_reset();
	lighting_test_history_hook = so_light_hook;
	server_world_test_history_hook = so_world_hook;
}
static void so_hist_end(void) {
	lighting_test_history_hook = NULL; server_world_test_history_hook = NULL;
	assert(!lighting_test_history_failed() && !server_world_test_history_failed());
}

/* The live failed state, captured at the start of the rollback. */
static struct server_world* so_rb_w;
static unsigned so_rb_calls;
static struct so_state so_rb_state;
static void so_rb_hook(struct server_world* w) {
	if(w != so_rb_w) return;
	so_rb_calls++;
	so_capture(w, &so_rb_state);
}

/* Scene (world coords, quadrant (0,0), stone platform at y=SO_Y). Cold
 * columns in worldgen_snow's order (x outer, z inner). */
#define SO_SNOW_N 5
static const int so_snow[SO_SNOW_N][2] = {{9, 9}, {10, 14}, {12, 10}, {15, 12}, {23, 12}};
#define SO_CHAIN_X0 24
#define SO_CHAIN_X1 33 /* x 32..33 are chunk 2: outside the 2x2 */
#define SO_CHAIN_Z 12
#define SO_SCENE_MAX (30 * 13 * 11 + 40)

static size_t so_scene_ops(struct server_world_test_setter_op* ops) {
	size_t n = 0;
#define SO_OP(X, Y, Z, ID) ops[n++] = (struct server_world_test_setter_op){ \
	SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA, (X), (Y), (Z), (ID), 0}
	for(int x = 6; x <= 35; x++) for(int z = 6; z <= 18; z++)
		for(int y = SO_Y - 4; y <= SO_Y + 6; y++)
			SO_OP(x, y, z, y == SO_Y ? BLOCK_STONE : BLOCK_AIR);
	/* lava first, then the water above it (CFR BlockFluid.onBlockAdded
	 * hardens only lava, so the JAR's setup leaves both in place) */
	for(int x = SO_CHAIN_X0; x <= SO_CHAIN_X1; x++) {
		SO_OP(x, SO_Y + 1, SO_CHAIN_Z, BLOCK_LAVA_STILL);
		SO_OP(x, SO_Y + 2, SO_CHAIN_Z, BLOCK_WATER_STILL);
	}
	SO_OP(12, SO_Y + 1, 9, BLOCK_LAVA_STILL);   /* beside snow (12,10) */
	SO_OP(12, SO_Y + 2, 9, BLOCK_WATER_STILL);
	SO_OP(10, SO_Y + 1, 15, BLOCK_WATER_STILL); /* beside snow (10,14) */
	SO_OP(16, SO_Y + 1, 12, BLOCK_SAND);        /* beside snow (15,12) */
#undef SO_OP
	return n;
}

static void so_temp(double temp[256]) {
	for(int i = 0; i < 256; i++) temp[i] = 2.0;
	for(int i = 0; i < SO_SNOW_N; i++)
		temp[(so_snow[i][0] - 8) * 16 + (so_snow[i][1] - 8)] = 0.0;
}

/* Starts the raw tape on w (see so_tape_scan); `dir` is CW_G9X4_ORDERED_OUT. */
static const char* so_tape_request;
static void so_tape_start(struct server_world* w, const char* dir) {
	snprintf(so_tape_dir, sizeof so_tape_dir, "%s/cave-dim%d-slot%u", dir,
		(int)w->dimension, (unsigned)w->world_index);
	MKDIR(so_tape_dir);
	gh_dir = so_tape_dir;
	gh_raw_seq = 0;
	gh_raw_open(); /* anchor line; installs gh_raw_observe, replaced below */
	lighting_test_history_reset(); server_world_test_history_reset();
	lighting_test_history_hook = so_light_hook;
	server_world_test_history_hook = so_world_hook;
	for(int cx = SO_RX0; cx <= SO_RX1; cx++) for(int cz = SO_RZ0; cz <= SO_RZ1; cz++) {
		const struct server_chunk* sc = dr_chunk(w, cx, cz);
		assert(sc);
		gh_register(sc, cx, cz, (int)w->dimension);
		so_tape_flag[cx - SO_RX0][cz - SO_RZ0] = sc->modified;
	}
	gh_rng_dimension = (int)w->dimension;
	so_tape_tick_head = w->scheduled_tick_head;
	so_tape_tick_seen = w->scheduled_tick_count;
	so_tape_boundaries = 0;
	so_tape_failed = false;
	char path[700];
	snprintf(path, sizeof path, "%s/boundaries.jsonl", so_tape_dir);
	so_tape_bfile = fopen(path, "wb");
	assert(so_tape_bfile);
	so_tape_w = w;
	so_tape_prev_jrand = jrand_test_hook;
	jrand_test_hook = so_tape_jrand;
	server_world_test_notify_boundary_hook = so_tape_notify_boundary;
}

static void so_tape_finish(size_t ops) {
	so_tape_scan(); /* a transition after the last instrumented event */
	gh_marker("end");
	so_tape_boundary("end");
	jrand_test_hook = so_tape_prev_jrand;
	server_world_test_notify_boundary_hook = NULL;
	assert(fclose(so_tape_bfile) == 0);
	so_tape_bfile = NULL;
	so_tape_w = NULL;
	gh_raw_close();
	gh_dir = NULL;
	assert(!so_tape_failed && so_tape_boundaries == ops);
}

/* Two worlds (slot 0/1) of seed 4242, chunks x -2..3 / z -1..2, the scene
 * in both, all clean; then, in the world under test, pre-existing state:
 * (1,0) and (3,0) dirty, (0,0) and (2,0) PENDING saves in an open batch. */
static struct server_world* so_fixture(unsigned ow_slot, unsigned test_slot,
	const char* tag, char ndir[260]) {
	char name[64]; snprintf(name, sizeof name, "g9x_snow_oom_%s", tag);
	test_fixture_dir(name, s_dir[1], sizeof s_dir[1]); MKDIR(s_dir[1]);
	char rdir[300]; snprintf(rdir, sizeof rdir, "%s/region", s_dir[1]); MKDIR(rdir);
	snprintf(ndir, 260, "%s_slot1", s_dir[1]); MKDIR(ndir);
	snprintf(rdir, sizeof rdir, "%s/region", ndir); MKDIR(rdir);
	memset(&S[1], 0, sizeof S[1]); dict_entity_init(S[1].entities);
	if(!server_local_te_tables_init(&S[1])) abort(); /* IMP-TE-CAPS */
	S[1].rand_src.seed = 0x9E11u;
	string_t level; string_init_set_str(level, s_dir[1]);
	server_world_create(&S[1].worlds[0], level,
		ow_slot == 0 ? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER, 4242, false);
	string_clear(level); string_init_set_str(level, ndir);
	server_world_create(&S[1].worlds[1], level,
		ow_slot == 0 ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD, 4242, false);
	string_clear(level);
	struct server_world* s0 = &S[1].worlds[0];
	struct server_world* s1 = &S[1].worlds[1];
	s0->session = s0->local = s1->session = s1->local = &S[1];
	s0->world_index = 0; s1->world_index = 1;
	jrand_set_seed(&s0->rand, 4242); jrand_set_seed(&s1->rand, 4242); mode(1);
	for(int x = -2; x <= 3; x++) for(int z = -1; z <= 2; z++) {
		struct server_chunk* sc;
		assert(server_world_load_chunk(s0, x, z, &sc));
		assert(server_world_load_chunk(s1, x, z, &sc));
	}
	assert(server_world_light_drain(s0) && server_world_light_drain(s1));
	static struct server_world_test_setter_op ops[SO_SCENE_MAX];
	const size_t n = so_scene_ops(ops);
	assert(n <= SO_SCENE_MAX);
	for(unsigned k = 0; k < 2; k++) {
		struct server_world* w = &S[1].worlds[k];
		assert(server_world_test_populate_setter_script(w, 0, 0, ops, n));
		assert(server_world_light_drain(w));
		dr_clean(w);
	}
	struct server_world* w = &S[1].worlds[test_slot];
	const struct server_world_test_setter_op dirty[4] = {
		{SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA, 2, 5, 2, BLOCK_GLASS, 0},
		{SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA, 20, 5, 2, BLOCK_GLASS, 0},
		{SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA, 36, 5, 2, BLOCK_GLASS, 0},
		{SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA, 52, 5, 2, BLOCK_GLASS, 0}};
	assert(server_world_test_populate_setter_script(w, 0, 0, dirty, 4));
	assert(server_world_light_drain(w));
	server_world_save_batch_begin(w);
	assert(server_world_save_chunk(w, false, 0, 0));
	assert(server_world_save_chunk(w, false, 2, 0));
	assert(!dr_chunk(w, 0, 0)->modified && dr_chunk(w, 0, 0)->save_pending);
	assert(!dr_chunk(w, 2, 0)->modified && dr_chunk(w, 2, 0)->save_pending);
	/* A PENDING region at the transaction's start, left undrained: the
	 * first snow cell's requests merge into it (CFR World.java:1293-1331,
	 * the last-five merge), so a rollback must restore the merged tail.
	 * It re-dirties (0,0), superseding its pending record (G9X-3); (2,0)
	 * stays a pending, unmodified save. */
	/* The step starts with an EMPTY scheduled-tick queue (the scene's own
	 * onBlockAdded ticks, e.g. the sand's, are dropped; the JAR witness clears
	 * World.scheduledTickSet/TreeSet the same way), so every tick the step
	 * schedules -- the sand's notification included -- is a new entry. The
	 * java.lang.Math stream starts from a fixed seed on both sides. */
	w->scheduled_tick_count = 0;
	w->scheduled_tick_head = 0;
	server_world_scheduled_ticks_resync(w); /* the containment set forgets them too */
	server_local_test_seed_math_random(SO_MATH_SEED);
	if(so_tape_request) so_tape_start(w, so_tape_request);
	const struct server_world_test_setter_op pend = {
		SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA, 9, SO_Y, 10, BLOCK_STONE, 1};
	const size_t regions_before = w->generation_light_n;
	assert(server_world_test_populate_setter_script(w, 0, 0, &pend, 1));
	assert(w->generation_light_n > regions_before);
	assert(dr_chunk(w, 0, 0)->modified);
	assert(!dr_chunk(w, 2, 0)->modified && dr_chunk(w, 2, 0)->save_pending);
	assert(dr_chunk(w, 1, 0)->modified && dr_chunk(w, 3, 0)->modified);
	assert(!dr_chunk(w, 0, 1)->modified && !dr_chunk(w, 1, 1)->modified);
	/* the Overworld snow scan starts at y=127: nothing above the scene */
	if(w->dimension == WORLD_DIM_OVERWORLD)
		for(int i = 0; i < SO_SNOW_N; i++) for(int y = SO_Y + 1; y < WORLD_HEIGHT; y++)
			assert(dr_block(w, so_snow[i][0], y, so_snow[i][1]).type == BLOCK_AIR);
	so_lava_base = fluid_block_lava_mix_effect_total();
	return w;
}

static void so_fixture_close(struct server_world* w, char ndir[260]) {
	assert(server_world_save_batch_end(w));
	server_world_discard(&S[1].worlds[1]);
	test_fixture_dir_cleanup(ndir);
	close_world(&S[1], s_dir[1], false);
}

/* Overworld decorate stage: one notifying netherrack write on the platform
 * that RAISES its column height, so func_339_g's own Sky request (and its
 * light-fault latch, server_world_light_column_write_impl) is a fault point. */
#define SO_RAISE_X 20
#define SO_RAISE_Z 16
static void so_run(struct server_world* w, unsigned mask, long fail_at,
	bool* committed) {
	struct server_world_test_notify_op ops[SO_SNOW_N];
	for(int i = 0; i < SO_SNOW_N; i++)
		ops[i] = (struct server_world_test_notify_op){so_snow[i][0], SO_Y + 1,
			so_snow[i][1], BLOCK_NETHERRACK};
	const struct server_world_test_notify_op raise = {SO_RAISE_X, SO_Y + 1,
		SO_RAISE_Z, BLOCK_NETHERRACK};
	double temp[256];
	so_temp(temp);
	const bool nether = w->dimension == WORLD_DIM_NETHER;
	server_world_test_populate_fault(mask, fail_at);
	assert(server_world_test_populate_snow_script(w, 0, 0,
		nether ? ops : &raise, nether ? SO_SNOW_N : 1, temp, committed));
	server_world_test_populate_fault(0, -1);
}

/* Pending scheduled-tick entries equal (CFR NextTickListEntry.equals) to the
 * sand at (16,SO_Y+1,12); *delay = the last one's fire_at_tick - now. */
static size_t so_sand_ticks(const struct server_world* w, size_t* delay) {
	size_t n = 0;
	for(size_t i = 0; i < w->scheduled_tick_count; i++) {
		const struct scheduled_tick* t = so_tick_at(w, i);
		if(t->x == 16 && t->y == SO_Y + 1 && t->z == 12 && t->block_type == BLOCK_SAND) {
			n++;
			*delay = (size_t)(t->fire_at_tick - w->current_tick);
		}
	}
	return n;
}

static void so_scenario(unsigned ow_slot, unsigned test_slot) {
	char ndir[260], label[64], what[512];
	so_tape_request = getenv("CW_G9X4_ORDERED_OUT");
	struct server_world* w = so_fixture(ow_slot, test_slot, "base", ndir);
	so_tape_request = NULL;
	const bool nether = w->dimension == WORLD_DIM_NETHER;
	const unsigned mask = SERVER_WORLD_TEST_POP_PHASE_DECORATE
		| SERVER_WORLD_TEST_POP_PHASE_SNOW;
	snprintf(label, sizeof label, "%s(slot%u)", nether ? "nether" : "overworld", test_slot);
	struct server_world* other = &S[1].worlds[test_slot ^ 1];
	struct so_state pre, post, other_pre, now;
	so_capture(w, &pre);
	so_capture(other, &other_pre);
	/* ---- the uninterrupted run: its point count, history and final state ---- */
	bool committed = false;
	const bool taped = so_tape_w == w;
	size_t sand_pre_delay = 0;
	const size_t sand_pre = so_sand_ticks(w, &sand_pre_delay);
	so_hist_begin(false);
	if(taped) {
		char begin[32];
		snprintf(begin, sizeof begin, "begin slot %u", (unsigned)w->world_index);
		so_tape_scan();
		gh_marker(begin);
		so_tape_boundary("pre");
	}
	so_run(w, mask, -1, &committed);
	if(taped) so_tape_finish(nether ? SO_SNOW_N : SO_SNOW_N + 1);
	so_hist_end();
	const size_t points = server_world_test_populate_fault_points();
	const uint64_t hist = so_hist;
	const size_t hist_n = so_hist_n, hist_api = so_hist_api;
	so_capture(w, &post);
	so_check(committed, label, "the uninterrupted run commits");
	so_check(points > 0, label, "the uninterrupted run has allocation fault points");
	for(int i = 0; i < SO_SNOW_N; i++) {
		const struct block_data b = dr_block(w, so_snow[i][0], SO_Y + 1, so_snow[i][1]);
		snprintf(what, sizeof what, "the %s write landed at (%d,%d,%d) (id %u)",
			nether ? "netherrack" : "snow", so_snow[i][0], SO_Y + 1, so_snow[i][1], b.type);
		so_check(b.type == (nether ? BLOCK_NETHERRACK : BLOCK_SNOW), label, what);
	}
	for(int x = SO_CHAIN_X0; x <= SO_CHAIN_X1; x++) {
		snprintf(what, sizeof what, "the notification chain hardened (%d,%d,%d) to obsidian",
			x, SO_Y + 1, SO_CHAIN_Z);
		so_check(dr_block(w, x, SO_Y + 1, SO_CHAIN_Z).type == BLOCK_OBSIDIAN, label, what);
	}
	so_check(dr_block(w, 12, SO_Y + 1, 9).type == BLOCK_OBSIDIAN, label,
		"the lava beside a cold column hardened");
	so_check(dr_block(w, 10, SO_Y + 1, 15).type == BLOCK_WATER_FLOW, label,
		"the still water beside a cold column woke (BlockStationary)");
	/* CFR BlockSand.java:36-37 onNeighborBlockChange -> World.java:827-843
	 * scheduleUpdateTick(x, y, z, 12, tickRate() = 3 (BlockSand.java:62-63)):
	 * exactly ONE pending (16,101,12) sand entry, 3 ticks out, none before. */
	size_t sand_delay = 0;
	const size_t sand_n = so_sand_ticks(w, &sand_delay);
	snprintf(what, sizeof what, "the sand beside a cold column was scheduled by its "
		"notification: (16,%d,12) block %d entries before %zu, after %zu (want 0 -> 1), "
		"delay %zu (want 3)", SO_Y + 1, BLOCK_SAND, sand_pre, sand_n, sand_delay);
	(void)sand_pre_delay;
	so_check(dr_block(w, 16, SO_Y + 1, 12).type == BLOCK_SAND && sand_pre == 0
		&& sand_n == 1 && sand_delay == 3, label, what);
	so_check(dr_chunk(w, 2, 0)->modified, label,
		"the chain's outside-quadrant chunk 2 was written");
	if(!nether)
		so_check(dr_block(w, SO_RAISE_X, SO_Y + 1, SO_RAISE_Z).type == BLOCK_NETHERRACK,
			label, "the decorate-stage height-raising write landed");
	snprintf(what, sizeof what, "every hardening's lava-mix effect committed once (%u -> %u)",
		pre.lava_total, post.lava_total);
	so_check(post.lava_total == pre.lava_total + SO_CHAIN_X1 - SO_CHAIN_X0 + 2, label, what);
	so_check(post.world_rng != pre.world_rng && post.region_n > pre.region_n, label,
		"the run drew World.rand and queued regions");
	so_capture(other, &now);
	so_check(!so_diff_world(&now, &other_pre), label, "the opposite world is untouched by the run");
	printf("  %s: uninterrupted run: %zu fault points, %zu history events (%zu setter calls), "
		"%zu regions, %zu ticks\n", label, points, hist_n, hist_api, post.region_n, post.tick_n);
	so_fixture_close(w, ndir);

	/* ---- every point, one at a time, on one world (each rollback must
	 * return it to `pre` exactly, so the next point starts from it) ---- */
	w = so_fixture(ow_slot, test_slot, "fault", ndir);
	other = &S[1].worlds[test_slot ^ 1];
	so_capture(w, &now);
	so_check(!so_diff(&now, &pre), label, "the fault fixture starts from the same state");
	size_t live_changed = 0, bad = 0;
	for(size_t k = 0; k < points; k++) {
		so_rb_w = w; so_rb_calls = 0;
		server_world_test_populate_rollback_hook = so_rb_hook;
		committed = true;
		so_run(w, mask, (long)k, &committed);
		server_world_test_populate_rollback_hook = NULL;
		so_capture(w, &now);
		const char* d = so_diff(&now, &pre);
		/* populated/residency are inside the digest: the seam never marks
		 * population, so a restored digest is also "not marked populated".
		 * At the rollback the transaction must still own its scratch. */
		const bool ok = !committed && so_rb_calls == 1 && !d && so_rb_state.scratch;
		if(so_rb_calls == 1) {
			struct so_state live = so_rb_state;
			live.scratch = pre.scratch;
			if(so_diff(&live, &pre)) live_changed++;
		}
		if(!ok) {
			bad++;
			snprintf(what, sizeof what, "fault point %zu/%zu: committed %d, rollbacks %u, "
				"state after: %s", k, points, committed, so_rb_calls, d ? d : "restored");
			so_check(false, label, what);
		} else so_pass++;
	}
	snprintf(what, sizeof what, "all %zu fault points fail the transaction, roll back once "
		"and restore every store and flag (%zu bad)", points, bad);
	so_check(bad == 0, label, what);
	printf("  %s: %s\n", label, what);
	snprintf(what, sizeof what, "the live state before the rollback differed from the "
		"restored state (bytes/flags/queue/RNG/ticks/latch) at %zu/%zu points",
		live_changed, points);
	so_check(live_changed > 0, label, what);
	printf("  %s: %s\n", label, what);
	so_capture(other, &now);
	so_check(!so_diff_world(&now, &other_pre), label, "the opposite world is untouched by the faults");
	/* ---- retry: identical to the uninterrupted run ---- */
	so_hist_begin(false);
	so_run(w, mask, -1, &committed);
	so_hist_end();
	so_capture(w, &now);
	so_check(committed, label, "the retry commits");
	snprintf(what, sizeof what, "the retry's ordered history equals the uninterrupted run, "
		"no duplicate notification (%zu/%zu events, %zu/%zu setter calls)",
		so_hist_n, hist_n, so_hist_api, hist_api);
	so_check(so_hist == hist && so_hist_n == hist_n && so_hist_api == hist_api, label, what);
	const char* d = so_diff(&now, &post);
	snprintf(what, sizeof what, "the retry's final state equals the uninterrupted run (%s)",
		d ? d : "equal");
	so_check(!d, label, what);
	so_fixture_close(w, ndir);
}

/* B: the production populate of a real cold quadrant. */
static void so_real_open(int px, int pz) {
	test_fixture_dir("g9x_snow_oom_real", s_dir[1], sizeof s_dir[1]); MKDIR(s_dir[1]);
	char rdir[300]; snprintf(rdir, sizeof rdir, "%s/region", s_dir[1]); MKDIR(rdir);
	memset(&S[1], 0, sizeof S[1]); dict_entity_init(S[1].entities);
	if(!server_local_te_tables_init(&S[1])) abort(); /* IMP-TE-CAPS */
	S[1].rand_src.seed = 0x9E11u;
	string_t level; string_init_set_str(level, s_dir[1]);
	server_world_create(&S[1].worlds[0], level, WORLD_DIM_OVERWORLD, 4242, false);
	string_clear(level);
	struct server_world* w = &S[1].worlds[0];
	w->session = w->local = &S[1]; w->world_index = 0;
	jrand_set_seed(&w->rand, 4242); mode(1);
	struct server_chunk* sc;
	assert(server_world_load_chunk(w, px, pz, &sc));
	assert(server_world_load_chunk(w, px + 1, pz, &sc));
	assert(server_world_load_chunk(w, px, pz + 1, &sc));
	server_local_math_random_restore(&so_math0);
	so_lava_base = fluid_block_lava_mix_effect_total();
}

static void so_real(void) {
	const char* label = "overworld(real populate)";
	char what[512];
	/* the coldest quadrant near the origin (most columns below 0.4; every
	 * second quadrant of x/z -96..96, the search itself is not under test) */
	so_real_open(0, 0);
	int px = 0, pz = 0, best = -1;
	for(int qx = -96; qx <= 96; qx += 2) for(int qz = -96; qz <= 96; qz += 2) {
		double t[256]; int cold = 0;
		worldgen_temperature_grid(S[1].worlds[0].generator, qx * 16 + 8, qz * 16 + 8, t);
		for(int i = 0; i < 256; i++) cold += t[i] < 0.4;
		if(cold > best) { best = cold; px = qx; pz = qz; }
	}
	close_world(&S[1], s_dir[1], false);
	so_cx0 = px; so_cz0 = pz;
	printf("  %s: quadrant (%d,%d), %d of 256 columns below 0.4\n", label, px, pz, best);
	/* uninterrupted: the 4th load populates (px,pz) */
	so_real_open(px, pz);
	struct server_world* w = &S[1].worlds[0];
	struct server_chunk* sc;
	so_pre_w = w; so_pre_armed = true; so_pre_seen = false;
	server_world_test_populate_fault(SERVER_WORLD_TEST_POP_PHASE_SNOW, -1);
	so_hist_begin(true);
	assert(server_world_load_chunk(w, px + 1, pz + 1, &sc));
	so_hist_end();
	server_world_test_populate_fault(0, -1);
	so_pre_armed = false;
	const size_t points = server_world_test_populate_fault_points();
	const uint64_t hist = so_hist;
	const size_t hist_n = so_hist_n;
	const struct so_state pre = so_pre_state;
	struct so_state post;
	so_capture(w, &post);
	int snow = 0;
	for(int x = px * 16 + 8; x < px * 16 + 24; x++) for(int z = pz * 16 + 8; z < pz * 16 + 24; z++)
		for(int y = 0; y < WORLD_HEIGHT; y++) snow += dr_block(w, x, y, z).type == BLOCK_SNOW;
	snprintf(what, sizeof what, "the real load populated the quadrant with %d snow layers, "
		"%zu snow-phase fault points, %zu populate history events", snow, points, hist_n);
	so_check(so_pre_seen && dr_chunk(w, px, pz)->populated && snow > 0 && points > 0, label, what);
	printf("  %s: %s\n", label, what);
	close_world(&S[1], s_dir[1], false);
	size_t bad = 0;
	for(size_t k = 0; k < points; k++) {
		so_real_open(px, pz);
		w = &S[1].worlds[0];
		so_pre_w = w; so_pre_armed = true; so_pre_seen = false;
		so_rb_w = w; so_rb_calls = 0;
		server_world_test_populate_rollback_hook = so_rb_hook;
		server_world_test_populate_fault(SERVER_WORLD_TEST_POP_PHASE_SNOW, (long)k);
		so_hist_begin(true);
		assert(server_world_load_chunk(w, px + 1, pz + 1, &sc));
		so_hist_end();
		server_world_test_populate_fault(0, -1);
		server_world_test_populate_rollback_hook = NULL;
		so_pre_armed = false;
		struct so_state now;
		so_capture(w, &now);
		const char* d = so_diff(&now, &so_pre_state);
		struct so_state live = so_rb_state;
		live.scratch = pre.scratch;
		const bool failed_ok = so_pre_seen && so_rb_calls == 1 && !d
			&& so_rb_state.scratch && !dr_chunk(w, px, pz)->populated
			&& !so_diff(&so_pre_state, &pre) && so_diff(&live, &pre);
		/* the retry, through the production populate entry point */
		so_hist_begin(true);
		const bool ran = server_world_test_populate_quadrant(w, px, pz);
		so_hist_end();
		so_capture(w, &now);
		const char* r = so_diff(&now, &post);
		const bool retry_ok = ran && !r && dr_chunk(w, px, pz)->populated
			&& so_hist == hist && so_hist_n == hist_n;
		if(!failed_ok || !retry_ok) {
			bad++;
			snprintf(what, sizeof what, "real fault point %zu/%zu: rollbacks %u, after "
				"rollback: %s; retry ran %d, state %s, history %s (%zu/%zu events)",
				k, points, so_rb_calls, d ? d : "restored", ran, r ? r : "equal",
				so_hist == hist ? "equal" : "DIFFERENT", so_hist_n, hist_n);
			so_check(false, label, what);
		} else so_pass++;
		close_world(&S[1], s_dir[1], false);
	}
	snprintf(what, sizeof what, "all %zu real snow-phase fault points defer the population, "
		"restore the pre-populate state, and the retry equals the uninterrupted state and "
		"populate history (%zu bad)", points, bad);
	so_check(bad == 0, label, what);
	printf("  %s: %s\n", label, what);
	so_cx0 = so_cz0 = 0;
}

static void case_g9x_snow_oom(void) {
	so_pass = so_red = 0;
	so_cx0 = so_cz0 = 0;
	server_local_math_random_capture(&so_math0);
	so_scenario(0, 0); /* Overworld under test in slot 0 */
	so_scenario(1, 1); /* Overworld under test in slot 1 */
	so_scenario(1, 0); /* Nether under test in slot 0 */
	so_scenario(0, 1); /* Nether under test in slot 1 */
	so_real();
	printf("  g9x_snow_oom: %ld PASS, %ld G9X-4 RED\n", so_pass, so_red);
}

/* ---- G9X-5: g9x_region_budget ----------------------------------------------
 * The generation light-region list (CFR World.java:1293-1331; pop order
 * World.java:1268-1289; MetadataChunkBlock.java:58-139) is bounded to 16,384
 * resident entries (seven relax reserve slots included, 448 KiB) per world,
 * older 128-entry pages spilled to an owned temporary backing file and read
 * back only when the unchanged LIFO reaches them. The SAME deterministic
 * script runs twice in fresh, identical fixtures: first BOUNDED (the
 * production 128 slots, with real storage/allocation faults injected), then
 * as the UNCAPPED reference (8,192 slots = the whole million-entry list
 * resident, i.e. HEAD's unbounded list) where each injected storage fault is
 * replaced by a logical reservation failure at the SAME reserve call. At
 * every step the complete ordered history (every lighting event: request,
 * reject, merge+target, admit, pop, yield, array store, with normalized
 * objects) and the final state (IDs/meta/Sky/Block/height/flags of all 24
 * chunks of both worlds, World.rand, hellRNG, the fault latch and the WHOLE
 * logical queue in order) must be byte-identical. Bounded-only bounds are
 * asserted on top (peak bytes, spills, loads, stops, file removal). Both
 * slot orders (Overworld in slot 0 / Nether in slot 0) are run. */
#define RB_CAP 16384u
#define RB_PAGE 128u
#define RB_PAGE_BYTES 3584u
#define RB_WORLD_BYTES 458752u
#define RB_CEILING (1472u * 1024u)
#define RB_MAX_STEPS 512
_Static_assert(RB_CAP * 28u == RB_WORLD_BYTES, "448 KiB per world");
static long rb_pass, rb_red;
static bool rb_bounded;
static struct server_world* rb_w[2];
static uint64_t rb_hist;
static size_t rb_hist_n, rb_unknown;
static bool rb_skip_yield;
struct rb_step {
	char label[64];
	uint64_t hist, state;
	size_t hist_n, region_n;
	long fault_index; /* the reserve call a bounded storage fault hit */
};
static struct rb_step rb_steps[RB_MAX_STEPS];
static size_t rb_step_n, rb_step_i;
static struct { const void* p; uint32_t id; } rb_cache[1024];

static void rb_check(bool ok, const char* what) {
	if(ok) { rb_pass++; return; }
	rb_red++;
	printf("  G9X-5 RED: %s\n", what);
}

static uint32_t rb_obj(const void* o) {
	if(!o) return 0;
	for(unsigned k = 0; k < 2; k++) if(o == rb_w[k]) return 1 + k;
	const size_t slot = ((uintptr_t)o >> 4) % 1024;
	if(rb_cache[slot].p == o) return rb_cache[slot].id;
	for(unsigned k = 0; k < 2; k++)
		for(int cx = -2; cx <= 3; cx++) for(int cz = -1; cz <= 2; cz++) {
			struct server_chunk* sc = dr_chunk(rb_w[k], cx, cz);
			if(!sc) continue;
			const void* planes[5] = {sc->ids, sc->metadata, sc->lighting_sky,
				sc->lighting_torch, sc->heightmap};
			for(unsigned p = 0; p < 5; p++) if(planes[p] == o) {
				const uint32_t id = 0x100u + k * 0x1000u
					+ (uint32_t)(((cx + 2) * 4 + (cz + 1)) * 8 + p);
				rb_cache[slot].p = o; rb_cache[slot].id = id;
				return id;
			}
		}
	rb_unknown++;
	return 0xFFFFFFFFu;
}

/* the lowest logical depth a drain reached (region-pop's index is the depth
 * before the pop): entries below it, minus the merge window, never changed */
static size_t rb_min_depth = SIZE_MAX;
static void rb_note_pop(const struct lighting_history_event* e) {
	if(e->operation && !strcmp(e->operation, "region-pop")
	   && e->after_depth < rb_min_depth)
		rb_min_depth = e->after_depth;
}
static bool rb_hook(const struct lighting_history_event* e) {
	rb_note_pop(e);
	if(rb_skip_yield && e->operation && !strcmp(e->operation, "region-yield"))
		return true;
	uint64_t h = rb_hist;
	for(const char* c = e->operation; c && *c; c++) h = so_u64(h, (uint8_t)*c);
	for(const char* c = e->file; c && *c; c++) h = so_u64(h, (uint8_t)*c);
	const int64_t v[] = {rb_obj(e->object), e->x, e->y, e->z, e->x2, e->y2, e->z2,
		e->channel, e->before, e->after, e->packed, e->forced, e->depth, e->result,
		e->line, (int64_t)e->index, (int64_t)e->before_depth, (int64_t)e->after_depth};
	rb_hist = dr_fnv(h, v, sizeof v);
	rb_hist_n++;
	return true;
}

static uint64_t rb_queue_hash(struct server_world* w, size_t* n) {
	uint64_t h = 0xCBF29CE484222325ull;
	int r[7];
	size_t i = 0;
	for(; server_world_test_generation_light_region(w, i, r); i++) h = dr_fnv(h, r, sizeof r);
	*n = i;
	return h;
}

static uint64_t rb_state(bool flags) {
	uint64_t h = 0xCBF29CE484222325ull;
	for(unsigned k = 0; k < 2; k++) {
		struct server_world* w = rb_w[k];
		for(int cx = -2; cx <= 3; cx++) for(int cz = -1; cz <= 2; cz++) {
			struct server_chunk* sc = dr_chunk(w, cx, cz);
			if(!sc) { h = so_u64(h, 0); continue; }
			h = dr_fnv(h, sc->ids, 32768);
			h = dr_fnv(h, sc->metadata, 16384);
			h = dr_fnv(h, sc->lighting_sky, 16384);
			h = dr_fnv(h, sc->lighting_torch, 16384);
			h = dr_fnv(h, sc->heightmap, 256);
			if(flags) h = so_u64(h, 4u * sc->modified + 2u * sc->save_pending + sc->populated);
		}
		h = so_u64(h, w->rand.seed);
		h = so_u64(h, w->hell_populate_rng.seed);
		h = so_u64(h, w->light_fault);
		h = so_u64(h, w->light_fault_reason);
		h = so_u64(h, w->light_seed_n);
		size_t n;
		h = so_u64(h, rb_queue_hash(w, &n));
		h = so_u64(h, n);
		assert(n == w->generation_light_n);
	}
	return h;
}

/* One comparison point: bounded records, the reference must match. */
static void rb_step(const char* label, bool flags, long fault_index) {
	struct rb_step s = {0};
	snprintf(s.label, sizeof s.label, "%s", label);
	s.hist = rb_hist; s.hist_n = rb_hist_n;
	s.state = rb_state(flags);
	s.region_n = rb_w[0]->generation_light_n + rb_w[1]->generation_light_n;
	s.fault_index = fault_index;
	memset(rb_cache, 0, sizeof rb_cache);
	if(rb_bounded) {
		assert(rb_step_n < RB_MAX_STEPS);
		rb_steps[rb_step_n++] = s;
		return;
	}
	assert(rb_step_i < rb_step_n);
	const struct rb_step* b = &rb_steps[rb_step_i++];
	char what[200];
	snprintf(what, sizeof what, "step %s: bounded history %zu events %016llx / state %016llx"
		" == uncapped reference %zu events %016llx / state %016llx", label,
		b->hist_n, (unsigned long long)b->hist, (unsigned long long)b->state,
		s.hist_n, (unsigned long long)s.hist, (unsigned long long)s.state);
	rb_check(!strcmp(b->label, label) && b->hist == s.hist && b->hist_n == s.hist_n
		&& b->state == s.state && b->region_n == s.region_n, what);
}

static long rb_recorded_fault(void) {
	return rb_bounded ? -1 : rb_steps[rb_step_i].fault_index;
}

/* count synthetic single-cell Block-light requests that never merge (each
 * differs by 2 in x from the newest five), x/z in chunks (0..1, 0..1). */
static size_t rb_fill_from(struct server_world* w, size_t start, size_t count, unsigned salt) {
	size_t ok = 0;
	for(size_t i = start; i < count; i++) {
		const size_t j = i + salt * 7u;
		const int x = (int)(j % 16) * 2, z = (int)((j / 16) % 16) * 2;
		const int y = 1 + (int)((j / 256) % 126);
		if(!server_world_test_generation_light_schedule(w, COLUMN_TORCH, x, y, z, x, y, z)) break;
		ok++;
	}
	return ok;
}
static size_t rb_fill(struct server_world* w, size_t count, unsigned salt) {
	return rb_fill_from(w, 0, count, salt);
}

static void rb_stats(struct server_world* w, struct server_world_light_queue_stats* st) {
	server_world_test_light_queue_stats(w, st);
}

/* A deterministic air cell with air on all six sides (a real flood source). */
static void rb_air_cell(struct server_world* w, int* ox, int* oy, int* oz) {
	for(int y = 120; y > 4; y--) for(int x = 4; x < 28; x++) for(int z = 4; z < 28; z++) {
		bool all = true;
		const int d[7][3] = {{0,0,0},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
		for(unsigned k = 0; k < 7 && all; k++)
			all = dr_block(w, x + d[k][0], y + d[k][1], z + d[k][2]).type == BLOCK_AIR;
		if(all) { *ox = x; *oy = y; *oz = z; return; }
	}
	assert(!"no air cell");
}

static bool rb_set(struct server_world* w, int x, int y, int z, uint8_t id) {
	const struct server_world_test_setter_op op = {
		SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA, x, y, z, id, 0};
	return server_world_test_populate_setter_script(w, 0, 0, &op, 1);
}

/* Drain; with a bounded storage fault armed the drain must STOP retaining
 * every region, then an explicit retry (clear the latch) completes it. The
 * reference gets a logical reserve failure at the same reserve call. */
/* One logical queue entry as server_world_test_generation_queue_copy writes
 * it: seven 32-bit fields (channel, x1, y1, z1, x2, y2, z2), 28 bytes. */
struct rb_region { int32_t v[7]; };
_Static_assert(sizeof(struct rb_region) == 28, "the queue's 28-byte entry");
/* The whole logical list of w (malloc'd, *n entries; free()). */
static struct rb_region* rb_queue_copy(struct server_world* w, size_t* n) {
	const size_t bytes = server_world_test_generation_queue_copy(w, NULL, 0);
	struct rb_region* q = malloc(bytes + 1);
	assert(q);
	assert(server_world_test_generation_queue_copy(w, q, bytes) == bytes);
	*n = bytes / sizeof *q;
	return q;
}

static long rb_drain_fault(struct server_world* w, enum server_world_test_lq_fault kind,
	long at, const char* label) {
	const long ref_at = rb_recorded_fault();
	server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, rb_bounded ? -1 : ref_at);
	size_t qn_before = 0;
	struct rb_region* before = rb_bounded ? rb_queue_copy(w, &qn_before) : NULL;
	if(rb_bounded) server_world_test_light_queue_fault(kind, at);
	const bool expect_stop = rb_bounded ? true : ref_at >= 0;
	rb_min_depth = SIZE_MAX;
	const bool done = server_world_light_drain(w);
	const size_t reserves = server_world_test_light_queue_fault_count(SERVER_WORLD_TEST_LQ_RESERVE);
	long idx = -1;
	if(rb_bounded) {
		const bool hit = server_world_test_light_queue_fault_count(kind) > (size_t)at;
		server_world_test_light_queue_fault(kind, -1);
		struct server_world_light_queue_stats st;
		rb_stats(w, &st);
		char what[220];
		snprintf(what, sizeof what, "%s: a bounded storage fault stops the drain with the "
			"work retained (stop=%d, reason=%u, n %zu, site %u)", label, !done,
			w->light_fault_reason, w->generation_light_n, st.last_fail_site);
		rb_check(hit && !done && w->light_fault
			&& w->light_fault_reason == SERVER_LIGHT_FAULT_DRAIN_OOM
			&& w->generation_light_n > 0, what);
		if(hit && st.last_fail_site == SERVER_WORLD_TEST_LQ_SITE_LOAD) {
			/* a load fault stops BEFORE the pop that needed the page: every
			 * spilled entry [0, base*128) is the pre-drain entry, byte for
			 * byte, and only the <= 5 newest entries are resident */
			size_t qn;
			struct rb_region* now = rb_queue_copy(w, &qn);
			const size_t keep = st.spilled_pages * RB_PAGE;
			const size_t low = rb_min_depth < 5 ? 0 : rb_min_depth - 5;
			snprintf(what, sizeof what, "%s: the stopped list keeps its %zu spilled entries (%zu resident "
				"above them); the %zu entries below the drain's lowest depth %zu (minus the merge "
				"window) are the pre-drain entries exactly", label, keep, qn - keep, low, rb_min_depth);
			rb_check(qn == w->generation_light_n && keep > 0 && keep <= qn && qn - keep <= 6
				&& low <= keep && low <= qn_before && !memcmp(now, before, low * sizeof *now), what);
			free(now);
		} else if(hit) idx = (long)reserves - 1; /* spill side: that reserve call failed */
	} else {
		rb_check(expect_stop == !done, "reference: the logical failure stops at the same call");
	}
	free(before);
	server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, -1);
	server_world_test_light_clear_fault(w);
	const bool retried = server_world_light_drain(w);
	rb_check(retried && w->generation_light_n == 0, "explicit retry settles the retained work");
	return idx;
}

/* ONE population whose own requests exceed the resident cap: cobblestone on
 * a 2-spaced grid over the whole 2x2 quadrant (x fastest, so the newest five
 * never merge), Overworld Sky+Block (+height edges), Nether Block only. */
#define RB_POP_OPS (126u * 256u)
static struct server_world_test_setter_op rb_pop_ops[RB_POP_OPS];
static size_t rb_pop_script(void) {
	size_t n = 0;
	for(int y = 1; y <= 126; y++) for(int z = 0; z < 32; z += 2) for(int x = 0; x < 32; x += 2)
		rb_pop_ops[n++] = (struct server_world_test_setter_op){
			SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA, x, y, z, BLOCK_COBBLESTONE, 0};
	return n;
}

/* The queue just before a population: P synthetic regions, the newest
 * straddling a page edge, the newest one adjacent to the population's first
 * write so that write's Block request MERGES into (and widens) it. */
static void rb_prefill(struct server_world* w, unsigned salt) {
	const size_t p = 125u * RB_PAGE + 1u;
	assert(rb_fill(w, p, salt) == p);
	assert(server_world_test_generation_light_schedule(w, COLUMN_TORCH, 1, 1, 0, 1, 1, 0));
	assert(w->generation_light_n == p + 1);
}

struct rb_obs { size_t calls, n_max, spilled_max, resident_max; };
static void rb_observe(struct server_world* w, size_t op, size_t writes, void* ctx) {
	(void)op; (void)writes;
	struct rb_obs* o = ctx;
	struct server_world_light_queue_stats st;
	rb_stats(w, &st);
	o->calls++;
	if(w->generation_light_n > o->n_max) o->n_max = w->generation_light_n;
	if(st.spilled_pages > o->spilled_max) o->spilled_max = st.spilled_pages;
	if(st.resident_bytes > o->resident_max) o->resident_max = st.resident_bytes;
}

/* G9X-5a copy-on-write witness state (rb_world_script section 6). */
#define CW_PAGES 8
static struct {
	struct server_world* w;
	bool armed, abandon, respilled, fault_armed, fault_hit, old_intact, new_differs;
	size_t base0, page, fault_page;
	int loc0[CW_PAGES], loc_new;
	uint8_t old_bytes[CW_PAGES][RB_PAGE_BYTES];
} cw;
static bool cw_hook(const struct lighting_history_event* e) {
	rb_hook(e);
	if(!rb_bounded || !cw.armed || cw.respilled) return true;
	struct server_world* w = cw.w;
	int loc = -1;
	bool flipped = false, trial = false;
	if(!server_world_test_light_queue_backing_page(w, 0, 0, NULL, &loc, &flipped, &trial) || !trial)
		return true;
	if(cw.abandon) {
		struct server_world_light_queue_stats st;
		rb_stats(w, &st);
		if(!cw.fault_armed && st.spilled_pages < cw.base0) {
			/* a pre-trial page is loaded again: every later spill is a
			 * copy-on-write re-spill of a page <= this base -- fail its write */
			cw.fault_armed = true;
			cw.fault_page = st.spilled_pages;
			server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_WRITE, 0);
		}
		return true;
	}
	for(size_t p = 0; p < cw.base0 && p < CW_PAGES; p++) {
		if(!server_world_test_light_queue_backing_page(w, p, 0, NULL, &loc, &flipped, NULL) || !flipped)
			continue;
		/* page p was re-spilled during this trial: the new version went to
		 * the OTHER physical slot and the original one is untouched */
		uint8_t buf[RB_PAGE_BYTES];
		cw.respilled = true;
		cw.page = p;
		cw.loc_new = loc;
		cw.old_intact = server_world_test_light_queue_backing_page(w, p, (unsigned)cw.loc0[p], buf,
			NULL, NULL, NULL) && !memcmp(buf, cw.old_bytes[p], RB_PAGE_BYTES);
		cw.new_differs = server_world_test_light_queue_backing_page(w, p, (unsigned)loc, buf,
			NULL, NULL, NULL) && memcmp(buf, cw.old_bytes[p], RB_PAGE_BYTES) != 0;
		break;
	}
	return true;
}

static void rb_world_script(struct server_world* w, unsigned k) {
	char label[64];
	const char* dim = w->dimension == WORLD_DIM_NETHER ? "nether" : "overworld";
	struct server_world_light_queue_stats st0, st;
	/* fresh, unallocated resident page slots (the fixture's generation
	 * allocated some): bounded = the production 128, reference = 8192 */
	assert(rb_bounded || w->generation_light_slots == 8192u);
	assert(server_world_test_light_queue_slots(w, w->generation_light_slots));
	/* 1. cap-1 / cap / cap+1 */
	const size_t edges[3] = {RB_CAP - 1, RB_CAP, RB_CAP + 1};
	for(unsigned e = 0; e < 3; e++) {
		rb_stats(w, &st0);
		if(e == 2) /* drains above may already have spilled (relax reserve(7)):
		            * start e 2 from a fresh ring and no backing file, both runs */
			assert(server_world_test_light_queue_slots(w, w->generation_light_slots));
		if(e != 1) {
			/* e 0: a page-slot ALLOC fault; e 2: the backing-file OPEN fault
			 * at the world's first spill (bounded) / a reserve failure at the
			 * same call (reference) refuses one admission; nothing is
			 * queued, the fill resumes */
			const enum server_world_test_lq_fault kind = e == 0 ? SERVER_WORLD_TEST_LQ_ALLOC
				: SERVER_WORLD_TEST_LQ_OPEN;
			const long ref_at = rb_recorded_fault();
			server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, rb_bounded ? -1 : ref_at);
			if(rb_bounded) server_world_test_light_queue_fault(kind, e == 0 ? 2 : 0);
			const size_t got = rb_fill(w, edges[e], e);
			const size_t reserves = server_world_test_light_queue_fault_count(SERVER_WORLD_TEST_LQ_RESERVE);
			const size_t opened = server_world_test_light_queue_fault_count(SERVER_WORLD_TEST_LQ_OPEN);
			server_world_test_light_queue_fault(kind, -1);
			server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, -1);
			char what[140];
			snprintf(what, sizeof what, "%s: page %s fault refuses admission %zu, list intact (n %zu)",
				dim, e == 0 ? "alloc" : "backing open", got, w->generation_light_n);
			rb_check(got < edges[e] && w->generation_light_n == got
				&& (e == 0 || !rb_bounded || (opened >= 1 && got == RB_CAP)), what);
			snprintf(label, sizeof label, "%s-%s-fault", dim, e == 0 ? "alloc" : "open");
			rb_step(label, true, rb_bounded && got < edges[e] ? (long)reserves - 1 : -1);
			assert(rb_fill_from(w, got, edges[e], e) == edges[e] - got);
		} else assert(rb_fill(w, edges[e], e) == edges[e]);
		rb_stats(w, &st);
		if(rb_bounded) {
			char what[160];
			snprintf(what, sizeof what, "%s cap edge %zu: %zu spill(s) (want %u), resident %zu B <= %u",
				dim, edges[e], st.spills - st0.spills, edges[e] > RB_CAP ? 1u : 0u,
				st.resident_bytes, RB_WORLD_BYTES);
			rb_check(st.spills - st0.spills == (edges[e] > RB_CAP ? 1u : 0u)
				&& st.resident_bytes <= RB_WORLD_BYTES, what);
		}
		snprintf(label, sizeof label, "%s-edge-%zu-fill", dim, edges[e]); rb_step(label, true, -1);
		assert(server_world_light_drain(w));
		rb_stats(w, &st);
		if(rb_bounded && edges[e] > RB_CAP)
			rb_check(st.loads - st0.loads == 1, "the spilled page is loaded back exactly once");
		snprintf(label, sizeof label, "%s-edge-%zu-drain", dim, edges[e]); rb_step(label, true, -1);
	}
	/* 2. the seven-slot relax reserve at every alignment of the window edge */
	int ax, ay, az;
	rb_air_cell(w, &ax, &ay, &az);
	size_t drain_spills = 0;
	for(unsigned f = 0; f <= 16; f++) {
		assert(rb_fill(w, RB_CAP - 16 + f, 100 + f) == RB_CAP - 16 + f);
		assert(rb_set(w, ax, ay, az, (f & 1) ? BLOCK_AIR : BLOCK_GLOWSTONE));
		rb_stats(w, &st0);
		assert(server_world_light_drain(w));
		rb_stats(w, &st);
		drain_spills += st.spills - st0.spills;
		if(rb_bounded && st.resident_bytes > RB_WORLD_BYTES)
			rb_check(false, "reserve sweep: resident bytes above 448 KiB");
		snprintf(label, sizeof label, "%s-reserve7-%u", dim, f); rb_step(label, true, -1);
	}
	if(rb_bounded) {
		char what[120];
		snprintf(what, sizeof what, "%s: reserve(7) spilled inside the drain %zu time(s)", dim, drain_spills);
		rb_check(drain_spills > 0, what);
	}
	/* 3. spill-side storage faults inside a drain (reserve(7) at the window
	 *    edge): failed write, real short write, failed seek, failed flush */
	static const struct { enum server_world_test_lq_fault kind; const char* name; } spill_faults[] = {
		{SERVER_WORLD_TEST_LQ_WRITE, "write"}, {SERVER_WORLD_TEST_LQ_SHORT_WRITE, "short-write"},
		{SERVER_WORLD_TEST_LQ_SEEK, "seek"}, {SERVER_WORLD_TEST_LQ_FLUSH, "flush"}};
	long idx = -1;
	for(unsigned f = 0; f < 4; f++) {
		const bool lit = dr_block(w, ax, ay, az).type == BLOCK_GLOWSTONE;
		if(lit) assert(rb_set(w, ax, ay, az, BLOCK_AIR));
		assert(server_world_light_drain(w)); /* the sweep ended on glowstone */
		assert(rb_fill(w, RB_CAP - 3, 200 + f) == RB_CAP - 3);
		assert(rb_set(w, ax, ay, az, BLOCK_GLOWSTONE));
		rb_skip_yield = false;
		char what[80];
		snprintf(what, sizeof what, "drain spill %s fault", spill_faults[f].name);
		idx = rb_drain_fault(w, spill_faults[f].kind, 0, what);
		snprintf(label, sizeof label, "%s-drain-%s-fault", dim, spill_faults[f].name);
		rb_step(label, true, idx);
		snprintf(label, sizeof label, "%s-reset-%u", dim, f); rb_step(label, true, -1);
	}

	/* 4. ONE population exceeding the RAM cap, committed; checkpoint
	 *    cancel/retry over the spilled list; READ-fault drain + retry. */
	const size_t ops = rb_pop_script();
	rb_prefill(w, 300);
	rb_stats(w, &st0);
	assert(server_world_test_populate_setter_script(w, 0, 0, rb_pop_ops, ops));
	rb_stats(w, &st);
	if(rb_bounded) {
		char what[200];
		snprintf(what, sizeof what, "%s: one population of %zu writes queued %zu regions (> %u), "
			"spilled %zu page(s), resident %zu B <= %u", dim, ops, w->generation_light_n,
			RB_CAP, st.spills - st0.spills, st.resident_bytes, RB_WORLD_BYTES);
		rb_check(w->generation_light_n > 2 * RB_CAP && st.spills > st0.spills
			&& st.resident_bytes <= RB_WORLD_BYTES, what);
	}
	snprintf(label, sizeof label, "%s-pop-commit", dim); rb_step(label, true, -1);
	{
		size_t qn0; const uint64_t q0 = rb_queue_hash(w, &qn0);
		const uint64_t s0 = rb_state(true);
		server_world_test_light_trial_fail(1);
		const bool c1 = server_world_save_chunk_coherent(w, 0, 0);
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
		const bool c2 = server_world_save_chunk_coherent(w, 0, 0);
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_NONE);
		const uint64_t s1 = rb_state(true);
		rb_stats(w, &st0);
		const bool c3 = server_world_save_chunk_coherent(w, 0, 0);
		rb_stats(w, &st);
		size_t qn1; const uint64_t q1 = rb_queue_hash(w, &qn1);
		rb_check(!c1 && !c2 && s1 == s0, "checkpoint cancel (trial alloc / payload write) leaves "
			"the world and the whole spilled list unchanged");
		rb_check(c3 && q1 == q0 && qn1 == qn0 && qn0 == w->generation_light_n,
			"checkpoint retry succeeds and restores every region in exact order");
		if(rb_bounded) {
			char what[220];
			snprintf(what, sizeof what, "%s checkpoint: trial spilled %zu / loaded %zu pages "
				"copy-on-write, copy %zu B <= %u + bitmap; subsystem peak %zu B <= %u", dim,
				st.spills - st0.spills, st.loads - st0.loads, st.ckpt_peak,
				RB_WORLD_BYTES, st.subsystem_peak, RB_CEILING);
			rb_check(st.loads > st0.loads && st.ckpt_peak <= RB_WORLD_BYTES + 1024
				&& st.subsystem_peak <= RB_CEILING, what);
		}
	}
	snprintf(label, sizeof label, "%s-checkpoint", dim); rb_step(label, true, -1);
	rb_skip_yield = true; /* a stop + retry restarts the CaveWii yield counter */
	(void)rb_drain_fault(w, SERVER_WORLD_TEST_LQ_READ, 3, "drain page read fault");
	rb_skip_yield = false;
	snprintf(label, sizeof label, "%s-pop-drain", dim); rb_step(label, true, -1);
	for(size_t i = 0; i < ops; i++) rb_pop_ops[i].id = BLOCK_AIR;
	assert(server_world_test_populate_setter_script(w, 0, 0, rb_pop_ops, ops));
	assert(server_world_light_drain(w));
	rb_pop_script();
	snprintf(label, sizeof label, "%s-pop-undo", dim); rb_step(label, true, -1);

	/* 5. failed populations over a spilled tail: rollback, then retry. */
	for(unsigned v = 0; v < 2; v++) {
		rb_prefill(w, 400 + v);
		const uint64_t pre = rb_state(true);
		size_t qn_pre; const uint64_t qpre = rb_queue_hash(w, &qn_pre);
		struct rb_obs o = {0};
		size_t wa = 0, wb = 0;
		const size_t fail_before = v == 0 ? ops - 1 : ops * 3 / 4;
		assert(server_world_test_populate_setter_script_fail(w, 0, 0, rb_pop_ops, ops,
			fail_before, &wa, &wb, rb_observe, &o));
		const uint64_t post = rb_state(true);
		size_t qn_post; const uint64_t qpost = rb_queue_hash(w, &qn_post);
		char what[220];
		snprintf(what, sizeof what, "%s rollback %u: observed live n %zu over %zu ops BEFORE the "
			"rollback; after it the list (%zu) and world equal the pre-state", dim, v, o.n_max,
			o.calls, qn_post);
		rb_check(o.calls == ops && o.n_max > qn_pre && pre == post && qpre == qpost
			&& qn_pre == qn_post, what);
		if(rb_bounded) {
			snprintf(what, sizeof what, "%s rollback %u: the tail page was spilled inside the "
				"transaction (%zu pages), resident max %zu <= %u", dim, v, o.spilled_max,
				o.resident_max, RB_WORLD_BYTES);
			rb_check(o.spilled_max * RB_PAGE > qn_pre - 5 && o.resident_max <= RB_WORLD_BYTES, what);
		}
		snprintf(label, sizeof label, "%s-rollback-%u", dim, v); rb_step(label, true, -1);
		/* a storage WRITE fault (bounded) / reserve failure (reference)
		 * through the TEST setter seam: the transaction's rollback must be
		 * exact. (The production population entry turns such a failure into
		 * the sticky POPULATION_STORAGE_FAILED stop: rb_population_stop.) */
		const long ref_at = rb_recorded_fault();
		server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, rb_bounded ? -1 : ref_at);
		if(rb_bounded) server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_WRITE, 5 + v);
		const bool committed = server_world_test_populate_setter_script(w, 0, 0, rb_pop_ops, ops);
		const size_t reserves = server_world_test_light_queue_fault_count(SERVER_WORLD_TEST_LQ_RESERVE);
		server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_WRITE, -1);
		server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, -1);
		size_t qn2; const uint64_t q2 = rb_queue_hash(w, &qn2);
		snprintf(what, sizeof what, "%s storage fault %u: the setter-seam transaction fails and is "
			"rolled back exactly (never a partial success)", dim, v);
		rb_check(!committed && rb_state(true) == pre && q2 == qpre && qn2 == qn_pre, what);
		snprintf(label, sizeof label, "%s-storage-fault-%u", dim, v);
		rb_step(label, true, rb_bounded ? (long)reserves - 1 : -1);
		assert(server_world_test_populate_setter_script(w, 0, 0, rb_pop_ops, ops));
		snprintf(label, sizeof label, "%s-retry-%u", dim, v); rb_step(label, true, -1);
		assert(server_world_light_drain(w));
		snprintf(label, sizeof label, "%s-retry-drain-%u", dim, v); rb_step(label, true, -1);
		/* undo the cobblestone with air so the next variant writes again */
		for(size_t i = 0; i < ops; i++) rb_pop_ops[i].id = BLOCK_AIR;
		assert(server_world_test_populate_setter_script(w, 0, 0, rb_pop_ops, ops));
		assert(server_world_light_drain(w));
		rb_pop_script();
		snprintf(label, sizeof label, "%s-undo-%u", dim, v); rb_step(label, true, -1);
	}
	/* 6. COPY-ON-WRITE witness (SPLIT-PLAN item 19). The bounded run uses a
	 *    2-page (256-entry) window here (the same production code, a smaller
	 *    test-only slot count; the reference stays uncapped). Three fresh
	 *    variants per world: (a) a successful checkpoint whose trial drain
	 *    loads a page P that was spilled BEFORE the trial (sentinel regions
	 *    outside the cursor's resident window), changes P's entries by its
	 *    production pops/merges/appends and RE-SPILLS the same logical page
	 *    while P's original backing version is still the one the restore
	 *    needs; (b) the same with a backing WRITE fault at the trial's first
	 *    re-spill (the trial is abandoned, the world keeps its live list);
	 *    (c) a trial whose spill opens the backing file for the first time. */
	for(unsigned variant = 0; variant < 3; variant++) {
		/* every variant starts dark (air, settled), so each trial runs the
		 * same kind of flood: glowstone placed in air */
		for(int dx = -4; dx <= 4; dx += 4) for(int dz = -4; dz <= 4; dz += 4)
			if(dr_block(w, ax + dx, ay, az + dz).type == BLOCK_GLOWSTONE)
				assert(rb_set(w, ax + dx, ay, az + dz, BLOCK_AIR));
		if(w->generation_light_n) assert(server_world_light_drain(w));
		if(rb_bounded) assert(server_world_test_light_queue_slots(w, 2));
		/* the flood sources at the BOTTOM of the list */
		for(int dx = -4; dx <= 4; dx += 4) for(int dz = -4; dz <= 4; dz += 4)
			assert(rb_set(w, ax + dx, ay, az + dz, BLOCK_GLOWSTONE));
		/* sentinel pages above them: (a)/(b) 5 pages + 50 (pages 0..2 spill
		 * before the trial), (c) below one window (no backing before it) */
		const size_t target = variant < 2 ? 5 * RB_PAGE + 50 : 2 * RB_PAGE - 40;
		const size_t have = w->generation_light_n;
		assert(have < target);
		assert(rb_fill(w, target - have, 600 + 10 * variant) == target - have);
		struct server_world_light_queue_stats st0, st;
		rb_stats(w, &st0);
		size_t qn0;
		struct rb_region* list0 = rb_queue_copy(w, &qn0);
		const uint64_t s0 = rb_state(false);
		memset(&cw, 0, sizeof cw);
		cw.w = w;
		char what[400];
		if(rb_bounded) {
			char path[600];
			snprintf(path, sizeof path, "%s", server_world_test_light_queue_backing_path(w));
			cw.base0 = st0.spilled_pages;
			if(variant < 2) {
				snprintf(what, sizeof what, "%s cow %u fixture: %zu pages spilled before the trial "
					"(window %zu..%zu), backing open %d", dim, variant, cw.base0, cw.base0 * RB_PAGE, qn0,
					path[0] != 0);
				rb_check(cw.base0 >= 2 && cw.base0 <= CW_PAGES && path[0] != 0, what);
				/* every pre-trial page's current version IS its sentinel entries */
				for(size_t p = 0; p < cw.base0 && p < CW_PAGES; p++) {
					int loc = -1;
					const bool read = server_world_test_light_queue_backing_page(w, p, 0, NULL, &loc, NULL, NULL)
						&& server_world_test_light_queue_backing_page(w, p, (unsigned)loc, cw.old_bytes[p],
							NULL, NULL, NULL);
					cw.loc0[p] = loc;
					snprintf(what, sizeof what, "%s cow %u: spilled page %zu (slot %d) holds exactly logical "
						"entries %zu..%zu", dim, variant, p, loc, p * RB_PAGE, p * RB_PAGE + RB_PAGE - 1);
					rb_check(read && !memcmp(cw.old_bytes[p], list0 + p * RB_PAGE, RB_PAGE_BYTES), what);
				}
			} else {
				snprintf(what, sizeof what, "%s cow %u fixture: nothing spilled and no backing file before "
					"the trial (n %zu)", dim, variant, qn0);
				rb_check(cw.base0 == 0 && path[0] == 0, what);
			}
			cw.armed = variant < 2;
			cw.abandon = variant == 1;
		}
		/* the reference: a logical reserve failure at the call the bounded
		 * fault hit (variant b) */
		const long ref_at = variant == 1 ? rb_recorded_fault() : -1;
		server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, rb_bounded ? -1 : ref_at);
		lighting_test_history_hook = cw_hook;
		const bool ok = server_world_save_chunk_coherent(w, 0, 0);
		lighting_test_history_hook = rb_hook;
		const size_t reserves = server_world_test_light_queue_fault_count(SERVER_WORLD_TEST_LQ_RESERVE);
		server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, -1);
		cw.fault_hit = cw.fault_armed
			&& server_world_test_light_queue_fault_count(SERVER_WORLD_TEST_LQ_WRITE) > 0;
		if(cw.fault_armed) server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_WRITE, -1);
		cw.armed = false;
		rb_stats(w, &st);
		long fault_idx = -1;
		if(rb_bounded && variant < 2) {
			/* the copy-on-write itself, observed DURING the trial */
			snprintf(what, sizeof what, "%s cow %u: during the trial page %zu (pre-trial spilled, outside "
				"the cursor window from page %zu) was loaded, changed and %s (slot %d -> %d; original "
				"version intact %d; new bytes differ %d)", dim, variant, cw.page, cw.base0,
				variant == 0 ? "RE-SPILLED" : "re-spilled (fault)", cw.page < CW_PAGES ? cw.loc0[cw.page] : -1,
				cw.loc_new, cw.old_intact, cw.new_differs);
			if(variant == 0) {
				printf("  %s\n", what);
				rb_check(cw.respilled && cw.page < cw.base0 && cw.loc_new == 1 - cw.loc0[cw.page]
					&& cw.old_intact && cw.new_differs, what);
			} else {
				const bool hit = cw.fault_hit;
				snprintf(what, sizeof what, "%s cow %u: a backing WRITE fault at the trial's copy-on-write "
					"re-spill (page %zu, fault armed %d) abandons the checkpoint (ok %d, stop reason %u, "
					"n %zu retained)", dim, variant, cw.fault_page, cw.fault_armed, ok,
					w->light_fault_reason, w->generation_light_n);
				printf("  %s\n", what);
				rb_check(hit && !ok && cw.fault_page < cw.base0 && w->light_fault
					&& w->light_fault_reason == SERVER_LIGHT_FAULT_DRAIN_OOM && w->generation_light_n > 0, what);
				if(hit) fault_idx = (long)reserves - 1;
			}
		}
		if(variant != 1) {
			/* restored: the whole logical list entry for entry, the arrays,
			 * the RNGs; P's slot bit and original bytes are back */
			size_t qn1;
			struct rb_region* list1 = rb_queue_copy(w, &qn1);
			size_t first = 0;
			while(first < qn0 && first < qn1 && !memcmp(&list0[first], &list1[first], sizeof *list0)) first++;
			snprintf(what, sizeof what, "%s cow %u: the checkpoint succeeded and restored all %zu regions "
				"(first difference at %zu of %zu) and the arrays/RNGs (%d)", dim, variant, qn0, first, qn1,
				rb_state(false) == s0);
			rb_check(ok && qn1 == qn0 && first == qn0 && rb_state(false) == s0, what);
			if(first < qn0 && first < qn1)
				printf("    raw first difference at %zu: pre-trial {%d %d %d %d %d %d %d} restored {%d %d %d %d %d "
					"%d %d}\n", first, list0[first].v[0], list0[first].v[1], list0[first].v[2],
					list0[first].v[3], list0[first].v[4], list0[first].v[5], list0[first].v[6],
					list1[first].v[0], list1[first].v[1], list1[first].v[2], list1[first].v[3],
					list1[first].v[4], list1[first].v[5], list1[first].v[6]);
			free(list1);
			if(rb_bounded && variant == 0) {
				int loc = -1; bool flipped = true, trial = true;
				uint8_t now[RB_PAGE_BYTES];
				const bool read = server_world_test_light_queue_backing_page(w, cw.page, 0, NULL, &loc,
					&flipped, &trial) && server_world_test_light_queue_backing_page(w, cw.page,
					(unsigned)loc, now, NULL, NULL, NULL);
				snprintf(what, sizeof what, "%s cow 0: after the restore page %zu reads its ORIGINAL version "
					"(slot %d == %d, trial %d, bytes equal %d) and lies outside the restored resident window "
					"(%zu < %zu)", dim, cw.page, loc, cw.loc0[cw.page], trial,
					read && !memcmp(now, cw.old_bytes[cw.page], RB_PAGE_BYTES), cw.page, st.spilled_pages);
				printf("  %s\n", what);
				rb_check(read && loc == cw.loc0[cw.page] && !trial
					&& !memcmp(now, cw.old_bytes[cw.page], RB_PAGE_BYTES) && cw.page < st.spilled_pages, what);
			}
			if(rb_bounded && variant == 2) {
				snprintf(what, sizeof what, "%s cow 2: the trial spilled %zu page(s), opening the backing "
					"file for the first time (%s)", dim, st.spills - st0.spills,
					server_world_test_light_queue_backing_path(w));
				printf("  %s\n", what);
				rb_check(st.spills > st0.spills && server_world_test_light_queue_backing_path(w)[0] != 0, what);
			}
		} else if(!rb_bounded)
			rb_check(ok == (ref_at < 0) && (ref_at < 0 || (w->light_fault
				&& w->light_fault_reason == SERVER_LIGHT_FAULT_DRAIN_OOM)),
				"reference: the trial stops at the same reserve call");
		free(list0);
		snprintf(label, sizeof label, "%s-cow-%u-checkpoint", dim, variant);
		rb_step(label, false, fault_idx);
		if(variant == 1) server_world_test_light_clear_fault(w);
		/* the subsequent drain reloads every page (P included) through the
		 * production LIFO; its order is compared with the reference */
		assert(server_world_light_drain(w));
		snprintf(label, sizeof label, "%s-cow-%u-drain", dim, variant);
		rb_step(label, false, -1);
	}
	if(rb_bounded) assert(server_world_test_light_queue_slots(w, 128));
	(void)k;
}

static void rb_open(unsigned ow_slot, const char* tag, char dirs[2][260]) {
	struct server_local* s = &S[1];
	for(unsigned k = 0; k < 2; k++) {
		char name[64], rdir[300];
		snprintf(name, sizeof name, "g9x_region_budget_%s_%u", tag, k);
		test_fixture_dir(name, dirs[k], 260); MKDIR(dirs[k]);
		snprintf(rdir, sizeof rdir, "%s/region", dirs[k]); MKDIR(rdir);
		snprintf(rdir, sizeof rdir, "%s/DIM-1", dirs[k]); MKDIR(rdir);
		snprintf(rdir, sizeof rdir, "%s/DIM-1/region", dirs[k]); MKDIR(rdir);
	}
	memset(s, 0, sizeof *s); dict_entity_init(s->entities);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(s))
		abort();
	s->rand_src.seed = 0x9E11u;
	for(unsigned k = 0; k < 2; k++) {
		string_t level; string_init_set_str(level, dirs[k]);
		server_world_create(&s->worlds[k], level,
			(k == 0) == (ow_slot == 0) ? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER, 4242, false);
		string_clear(level);
		s->worlds[k].session = s->worlds[k].local = s;
		s->worlds[k].world_index = (uint8_t)k;
		jrand_set_seed(&s->worlds[k].rand, 4242);
		rb_w[k] = &s->worlds[k];
	}
	mode(1);
	for(int x = -2; x <= 3; x++) for(int z = -1; z <= 2; z++)
		for(unsigned k = 0; k < 2; k++) {
			struct server_chunk* sc;
			assert(server_world_load_chunk(rb_w[k], x, z, &sc));
		}
	for(unsigned k = 0; k < 2; k++) assert(server_world_light_drain(rb_w[k]));
	for(unsigned k = 0; k < 2; k++) {
		struct server_world_light_queue_stats st;
		rb_stats(rb_w[k], &st);
		/* the production default is the bounded ring; the reference widens it */
		if(rb_bounded) rb_check(rb_w[k]->generation_light_slots == 128,
			"the production queue ring is 128 resident pages");
		else assert(server_world_test_light_queue_slots(rb_w[k], 8192));
	}
}

static void rb_close(char dirs[2][260]) {
	struct server_local* s = &S[1];
	dict_entity_clear(s->entities);
	for(unsigned k = 0; k < 2; k++) {
		server_world_discard(&s->worlds[k]);
		test_fixture_dir_cleanup(dirs[k]);
	}
}

static void rb_run(unsigned ow_slot, bool bounded) {
	char dirs[2][260], label[64];
	rb_bounded = bounded;
	rb_hist = 0xCBF29CE484222325ull; rb_hist_n = 0; rb_unknown = 0;
	memset(rb_cache, 0, sizeof rb_cache);
	server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, -1);
	struct server_world_light_queue_stats st;
	rb_stats(NULL, &st);
	const size_t base_bytes = st.subsystem_bytes, base_wii = st.wii_bytes;
	server_world_test_light_queue_stats_reset(); /* no live queue world here */
	rb_open(ow_slot, bounded ? "b" : "r", dirs);
	lighting_test_history_reset();
	lighting_test_history_hook = rb_hook;
	rb_step("fixture", true, -1);
	for(unsigned k = 0; k < 2; k++) rb_world_script(rb_w[k], k);
	/* two worlds holding spilled lists at once, then chunk unload/reload */
	for(unsigned k = 0; k < 2; k++) assert(rb_fill(rb_w[k], 3 * RB_CAP, 500 + k) == 3 * RB_CAP);
	rb_stats(rb_w[0], &st);
	if(bounded) {
		char what[200];
		struct server_world_light_queue_stats s1;
		rb_stats(rb_w[1], &s1);
		snprintf(what, sizeof what, "two worlds: %zu + %zu spilled pages, subsystem peak %zu B <= %u "
			"(per-world resident peak %zu, checkpoint copy peak %zu)", st.spilled_pages,
			s1.spilled_pages, st.subsystem_peak, RB_CEILING, st.world_resident_peak, st.ckpt_peak);
		rb_check(st.spilled_pages > 0 && s1.spilled_pages > 0 && st.subsystem_peak <= RB_CEILING
			&& st.world_resident_peak <= RB_WORLD_BYTES, what);
		printf("  G9X-5 peak bytes: subsystem %zu (ceiling %u), world resident %zu, checkpoint "
			"copy %zu, meta %zu+%zu\n", st.subsystem_peak, RB_CEILING, st.world_resident_peak,
			st.ckpt_peak, st.meta_bytes, s1.meta_bytes);
		/* item 17: the PPC allocator ledger of the same run (MEM2_GUARD +
		 * lwp block overhead per page, malloc headers, FILE state, PPC
		 * pointer/struct sizes, the window's extra byte) */
		snprintf(what, sizeof what, "PPC (Wii) allocator ledger peak %zu B <= %u (payload peak %zu B)",
			st.wii_peak, RB_CEILING, st.subsystem_peak);
		rb_check(st.wii_peak <= RB_CEILING && st.wii_peak > st.subsystem_peak, what);
		printf("  G9X-5 Wii-ledger peak: %zu B (ceiling %u; payload peak %zu B; +%zu B allocator/"
			"FILE overhead)\n", st.wii_peak, RB_CEILING, st.subsystem_peak, st.wii_peak - st.subsystem_peak);
	}
	rb_step("two-worlds-filled", true, -1);
	for(unsigned k = 0; k < 2; k++) {
		const bool unloaded = server_world_save_chunk(rb_w[k], true, 1, 1);
		struct server_chunk* sc;
		if(unloaded) assert(server_world_load_chunk(rb_w[k], 1, 1, &sc));
		snprintf(label, sizeof label, "unload-%u-%d", k, unloaded); rb_step(label, false, -1);
	}
	for(unsigned k = 0; k < 2; k++) assert(server_world_light_drain(rb_w[k]));
	rb_step("two-worlds-drained", false, -1);
	/* the million-region resource stop stays an explicit failure witness */
	{
		struct server_world* w = rb_w[0];
		const size_t got = rb_fill(w, 1000001, 900);
		char what[160];
		snprintf(what, sizeof what, "the 1,000,001st admission is refused (queued %zu, n %zu): "
			"an explicit resource failure, nothing cleared", got, w->generation_light_n);
		rb_check(got == 1000000 && w->generation_light_n == 1000000, what);
		rb_stats(w, &st);
		if(bounded) {
			snprintf(what, sizeof what, "million: resident %zu B <= %u with %zu pages spilled",
				st.resident_bytes, RB_WORLD_BYTES, st.spilled_pages);
			rb_check(st.resident_bytes <= RB_WORLD_BYTES && st.spilled_pages >= 7000, what);
		}
		rb_step("million-filled", false, -1);
		assert(server_world_light_drain(w));
		rb_step("million-drained", false, -1);
	}
	/* unload with a spilled list pending: the owned backing file goes away */
	assert(rb_fill(rb_w[1], 2 * RB_CAP, 950) == 2 * RB_CAP);
	char path[600];
	snprintf(path, sizeof path, "%s", server_world_test_light_queue_backing_path(rb_w[1]));
	if(bounded) rb_check(path[0] != 0, "a spilled list owns a backing file");
	lighting_test_history_hook = NULL;
	printf("  %s run (overworld slot %u): %zu steps, %zu events, %zu unmapped objects\n",
		bounded ? "bounded" : "reference", ow_slot, bounded ? rb_step_n : rb_step_i,
		rb_hist_n, rb_unknown);
	rb_close(dirs);
	if(path[0]) {
		FILE* f = fopen(path, "rb");
		rb_check(!f, "world teardown removes its backing file");
		if(f) fclose(f);
	}
	rb_stats(NULL, &st);
	rb_check(st.subsystem_bytes == base_bytes && st.wii_bytes == base_wii,
		"teardown releases every queue byte (payload and Wii ledger)");
}

/* ORDERED original-JAR witness input (tmp/jobs/loop/G9X-5/jar_budget_witness.py;
 * only with CW_G9X5_ORDERED_OUT, recording never changes an assertion): the
 * G9X-4 base fixture (so_fixture, raw-tape format of history_case.h + rng /
 * modified / tick rows), then ONE population whose World.setBlockAndMetadata
 * writes queue more than the resident cap (Overworld y 1..40, Nether y
 * 40..109, cobblestone on the 2-spaced x-fastest grid of quadrant (0,0)) =
 * boundary op1, then the complete drain (World.func_6156_d loop) = op2. The
 * bounded list MUST spill and load back during it. */
static size_t rb_tape_ops(bool nether) {
	size_t n = 0;
	const int y0 = nether ? 40 : 1, y1 = nether ? 109 : 40;
	for(int y = y0; y <= y1; y++) for(int z = 0; z < 32; z += 2) for(int x = 0; x < 32; x += 2)
		rb_pop_ops[n++] = (struct server_world_test_setter_op){
			SERVER_WORLD_TEST_SET_BLOCK_AND_METADATA, x, y, z, BLOCK_COBBLESTONE, 0};
	return n;
}

static void rb_ordered_tape(const char* dir) {
	for(unsigned ow_slot = 0; ow_slot < 2; ow_slot++)
		for(unsigned test_slot = 0; test_slot < 2; test_slot++) {
			char ndir[260];
			so_tape_request = dir;
			struct server_world* w = so_fixture(ow_slot, test_slot, "g9x5", ndir);
			so_tape_request = NULL;
			assert(so_tape_w == w);
			const bool nether = w->dimension == WORLD_DIM_NETHER;
			char begin[32];
			snprintf(begin, sizeof begin, "begin slot %u", (unsigned)w->world_index);
			so_tape_scan();
			gh_marker(begin);
			so_tape_boundary("pre");
			struct server_world_light_queue_stats st0, st1, st2;
			server_world_test_light_queue_stats(w, &st0);
			const size_t ops = rb_tape_ops(nether);
			assert(server_world_test_populate_setter_script(w, 0, 0, rb_pop_ops, ops));
			const size_t queued = w->generation_light_n;
			server_world_test_light_queue_stats(w, &st1);
			so_tape_scan();
			so_tape_boundary("op1");
			assert(server_world_light_drain(w));
			server_world_test_light_queue_stats(w, &st2);
			so_tape_scan();
			so_tape_boundary("op2");
			so_tape_finish(0);
			char what[200];
			snprintf(what, sizeof what, "ordered tape %s slot %u: %zu writes queued %zu regions, "
				"%zu spills / %zu loads, resident %zu B <= %u", nether ? "nether" : "overworld",
				test_slot, ops, queued, st2.spills - st0.spills, st2.loads - st0.loads,
				st1.resident_bytes, RB_WORLD_BYTES);
			printf("  %s\n", what);
			rb_check(queued > RB_CAP && st1.spills > st0.spills && st2.loads > st1.loads
				&& st1.resident_bytes <= RB_WORLD_BYTES, what);
			so_fixture_close(w, ndir);
		}
	rb_pop_script();
}

/* ---- G9X-5a PRODUCTION STOP: POPULATION_STORAGE_FAILED through
 * server_world_ensure_populated (SPLIT-PLAN items 6, 7, 18). Fresh,
 * identical fixtures provision the same unpopulated column (PS_X, PS_Z) of
 * both worlds through the production entry point:
 *   U  uninterrupted: the reference order of the quadrants that actually
 *      populated (hellRNG after each, nested or not, admitting or not) and
 *      the state at each quadrant's transaction start;
 *   spill faults (OPEN, SEEK, WRITE, SHORT_WRITE, FLUSH): armed at the first
 *      lighting event of an observed ADMITTING quadrant (the next reserve is
 *      forced to spill one page, so the fault lands inside that quadrant's
 *      transaction), once at a top-level quadrant and once (WRITE) at a
 *      quadrant populated NESTED inside a neighbour load;
 *   tail-reload faults (READ, SHORT_READ, SEEK): a rolled-back population
 *      (test setter seam) leaves the mergeable tail spilled; the provision's
 *      first page reload fails before any transaction begins;
 *   non-storage: a logical reserve failure (no backing I/O) at the same
 *      admitting quadrant must NOT stop the world (no all-cause hold);
 *   terrain (P1, attempt 2): a WRITE spill fault at the terrain height-edge
 *      reserve of each of a quadrant's 4 neighbour loads (load index 0..3,
 *      each the first quadrant in U whose load at that index generates):
 *      the generated chunk is discarded and the stop is classified as
 *      storage (SITE_TERRAIN at that chunk), never as an ordinary reserve
 *      fault, before any further load, pointer check or quadrant call.
 * Every retained RNG / work owner (World.rand, hellRNG, the session RNG,
 * Math.random, scheduled ticks, planned entities, light seeds, the logical
 * queue) is compared field by field, and direct counters armed BEFORE the
 * failing call count every population start, terrain generation, load,
 * transaction start, owner move and lighting event after the latch.
 * A backing failure must leave the exact pre-quadrant state (arrays, flags,
 * World.rand, hellRNG, the whole logical queue), a distinct first-failure
 * record (cause, site, quadrant), no later quadrant start, RNG draw or
 * lighting event, and no progress across repeated ticks, moved requests,
 * fault removal and an attempted generic latch clear. */
#define PS_X 8
#define PS_Z 8
#define PS_MAX 64
#define PS_QMAX 128
struct ps_rec { int x, z; uint64_t rng; bool nested; };
/* every retained RNG / work owner of world k and its session */
struct ps_owners {
	uint64_t world_rand, hell_rng, math_seed, queue_hash;
	uint32_t session_seed, session_has_gauss, math_init, sched_drops;
	float session_next_gauss;
	size_t sched_head, sched_count, entities, light_seeds, queue_n;
};
static struct ps_rec ps_ok[2][PS_MAX];
static size_t ps_ok_n[2];
/* the state at each quadrant's transaction start (the last one before its
 * first setter: after its loads, before its RNG draws) */
static struct { unsigned k; int x, z; uint64_t pre; } ps_pre[PS_QMAX];
static size_t ps_pre_n;
static struct { unsigned k; const char* op; int x, z; bool before, setter, nested; uint64_t pre;
	int loads, load_idx; bool generated; } ps_ops[128];
static unsigned ps_ops_n;
static struct { unsigned k; int x, z; } ps_admit[PS_MAX];
static size_t ps_admit_n;
/* the run's injection */
static int ps_kind = -1;          /* enum server_world_test_lq_fault, -1 = U */
static bool ps_force;             /* spill-side: force the next reserve to spill */
static struct { int x, z; bool set; } ps_target[2];
static bool ps_armed[2], ps_hit[2];
static size_t ps_after_events[2]; /* lighting events after the stop */
static bool ps_stopped[2];
/* direct post-stop counters, armed BEFORE the failing call: every event of
 * world k observed while its sticky stop is latched */
static struct { size_t pop_starts, gen_starts, load_starts, tx_starts, light_events, owner_moves;
	bool seen; struct ps_owners at_stop; } ps_post[2];
static bool ps_count_on;
/* terrain mode: the neighbour-load index under test and, per world, the U
 * quadrant / load chosen for it (U records the first per index) */
static int ps_term_idx = -1;
struct ps_term { int qx, qz, lx, lz; bool set; };
static struct ps_term ps_term_cur[2][4], ps_u_term[2][2][4], ps_term_tgt[2];
static uint64_t ps_term_pre[2];
static struct ps_owners ps_term_owners[2];

static uint64_t ps_state(unsigned k) {
	struct server_world* w = rb_w[k];
	uint64_t h = 0xCBF29CE484222325ull;
	for(int cx = PS_X - 3; cx <= PS_X + 3; cx++) for(int cz = PS_Z - 3; cz <= PS_Z + 3; cz++) {
		struct server_chunk* sc = dr_chunk(w, cx, cz);
		if(!sc) { h = so_u64(h, 0); continue; }
		h = dr_fnv(h, sc->ids, 32768);
		h = dr_fnv(h, sc->metadata, 16384);
		h = dr_fnv(h, sc->lighting_sky, 16384);
		h = dr_fnv(h, sc->lighting_torch, 16384);
		h = dr_fnv(h, sc->heightmap, 256);
		h = so_u64(h, 4u * sc->modified + 2u * sc->save_pending + sc->populated);
	}
	h = so_u64(h, w->rand.seed);
	h = so_u64(h, w->hell_populate_rng.seed);
	h = so_u64(h, w->light_seed_n);
	size_t n;
	h = so_u64(h, rb_queue_hash(w, &n));
	h = so_u64(h, n);
	/* G9X-5a P2: the session RNG, Math.random, scheduled work, entities */
	h = so_u64(h, S[1].rand_src.seed);
	h = so_u64(h, S[1].rand_src.has_next_gaussian);
	uint32_t g; memcpy(&g, &S[1].rand_src.next_gaussian, sizeof g);
	h = so_u64(h, g);
	struct server_local_math_random_state m;
	server_local_math_random_capture(&m);
	h = so_u64(h, m.rng.seed);
	h = so_u64(h, m.initialized);
	h = so_u64(h, w->scheduled_tick_head);
	h = so_u64(h, w->scheduled_tick_count);
	h = so_u64(h, w->scheduled_tick_drops);
	for(size_t i = 0; i < w->scheduled_tick_count; i++) {
		const size_t at = (w->scheduled_tick_head + i) % w->scheduled_tick_capacity;
		const struct scheduled_tick* t = &w->scheduled_ticks[at]; /* fields: no padding bytes */
		h = so_u64(h, (uint64_t)(uint32_t)t->x << 32 | (uint32_t)t->y);
		h = so_u64(h, (uint64_t)(uint32_t)t->z << 32 | t->fire_at_tick);
		h = so_u64(h, (uint64_t)t->block_type << 8 | t->queue_flags);
	}
	h = so_u64(h, dict_entity_size(S[1].entities));
	return h;
}

static void ps_owners_get_ex(unsigned k, struct ps_owners* o, bool with_queue) {
	struct server_world* w = rb_w[k];
	struct server_local_math_random_state m;
	server_local_math_random_capture(&m);
	memset(o, 0, sizeof *o);
	o->world_rand = w->rand.seed;
	o->hell_rng = w->hell_populate_rng.seed;
	o->session_seed = S[1].rand_src.seed;
	o->session_has_gauss = S[1].rand_src.has_next_gaussian;
	o->session_next_gauss = S[1].rand_src.next_gaussian;
	o->math_seed = m.rng.seed;
	o->math_init = m.initialized;
	o->sched_head = w->scheduled_tick_head;
	o->sched_count = w->scheduled_tick_count;
	o->sched_drops = w->scheduled_tick_drops;
	o->entities = dict_entity_size(S[1].entities);
	o->light_seeds = w->light_seed_n;
	if(with_queue) o->queue_hash = rb_queue_hash(w, &o->queue_n);
}
static void ps_owners_get(unsigned k, struct ps_owners* o) { ps_owners_get_ex(k, o, true); }
/* the number of owner fields that differ (0 = nothing drew or moved) */
static unsigned ps_owners_moved(const struct ps_owners* a, const struct ps_owners* b) {
	return (unsigned)(a->world_rand != b->world_rand) + (a->hell_rng != b->hell_rng)
		+ (a->session_seed != b->session_seed) + (a->session_has_gauss != b->session_has_gauss)
		+ (memcmp(&a->session_next_gauss, &b->session_next_gauss, sizeof(float)) != 0)
		+ (a->math_seed != b->math_seed) + (a->math_init != b->math_init)
		+ (a->sched_head != b->sched_head) + (a->sched_count != b->sched_count)
		+ (a->sched_drops != b->sched_drops) + (a->entities != b->entities)
		+ (a->light_seeds != b->light_seeds) + (a->queue_hash != b->queue_hash)
		+ (a->queue_n != b->queue_n);
}
static bool ps_post_zero(unsigned k) {
	return !ps_post[k].pop_starts && !ps_post[k].gen_starts && !ps_post[k].load_starts
		&& !ps_post[k].tx_starts && !ps_post[k].light_events && !ps_post[k].owner_moves;
}
static int ps_post_str(char* out, size_t cap, unsigned k) {
	return snprintf(out, cap, "after the latch: %zu population starts, %zu terrain generations, %zu loads, "
		"%zu transaction starts, %zu owner moves, %zu lighting events", ps_post[k].pop_starts,
		ps_post[k].gen_starts, ps_post[k].load_starts, ps_post[k].tx_starts, ps_post[k].owner_moves,
		ps_post[k].light_events);
}

static bool ps_fail_noted[2];
static void ps_pre_note(unsigned k, int x, int z, uint64_t pre) {
	size_t i = 0;
	while(i < ps_pre_n && !(ps_pre[i].k == k && ps_pre[i].x == x && ps_pre[i].z == z)) i++;
	if(i == ps_pre_n) { assert(ps_pre_n < PS_QMAX); ps_pre_n++; }
	ps_pre[i].k = k; ps_pre[i].x = x; ps_pre[i].z = z;
	ps_pre[i].pre = pre;
}

static unsigned ps_world(const void* w) {
	return w == rb_w[0] ? 0u : w == rb_w[1] ? 1u : 2u;
}

static void ps_disarm(unsigned k) {
	if(!ps_armed[k]) return;
	const enum server_world_test_lq_fault kind = (enum server_world_test_lq_fault)ps_kind;
	if(server_world_test_light_queue_fault_count(kind) > 0) ps_hit[k] = true;
	server_world_test_light_queue_fault(kind, -1);
	server_world_test_light_queue_force_spill(false);
	ps_armed[k] = false;
}

static bool ps_hook(const struct server_world_history_event* e) {
	if(!e->operation || !e->edge) return true;
	const unsigned k = ps_world(e->world);
	const bool pop = !strcmp(e->operation, "populate");
	const bool setter = !strncmp(e->operation, "setter", 6) || !strcmp(e->operation, "raw-id-meta");
	/* direct post-stop counters (armed before the failing call) */
	const bool post = k < 2 && ps_count_on && rb_w[k]->gen_storage_failed;
	if(post) {
		/* per event: every owner but the logical queue (its full hash per
		 * event is O(queue)); the queue is compared at the call's end */
		struct ps_owners o;
		ps_owners_get_ex(k, &o, !ps_post[k].seen);
		if(!ps_post[k].seen) { ps_post[k].seen = true; ps_post[k].at_stop = o; }
		else {
			o.queue_hash = ps_post[k].at_stop.queue_hash;
			o.queue_n = ps_post[k].at_stop.queue_n;
			ps_post[k].owner_moves += ps_owners_moved(&ps_post[k].at_stop, &o);
		}
		if(!strcmp(e->edge, "begin")) {
			if(pop) ps_post[k].pop_starts++;
			if(!strcmp(e->operation, "generate")) ps_post[k].gen_starts++;
			if(!strcmp(e->operation, "load")) ps_post[k].load_starts++;
		}
	}
	if(!strcmp(e->edge, "attempt")) {
		/* a setter attempt / array store directly under a populate: its
		 * first write (the decorate writes through the population view) */
		if(ps_ops_n && !strcmp(ps_ops[ps_ops_n - 1].op, "populate") && !ps_ops[ps_ops_n - 1].setter
		   && ps_ops[ps_ops_n - 1].k < 2) {
			if(ps_count_on && rb_w[ps_ops[ps_ops_n - 1].k]->gen_storage_failed)
				ps_post[ps_ops[ps_ops_n - 1].k].tx_starts++;
			ps_ops[ps_ops_n - 1].setter = true;
			ps_pre_note(ps_ops[ps_ops_n - 1].k, ps_ops[ps_ops_n - 1].x, ps_ops[ps_ops_n - 1].z,
				ps_ops[ps_ops_n - 1].pre);
		}
		return true;
	}
	if(!strcmp(e->edge, "begin")) {
		assert(ps_ops_n < 128);
		if(setter && ps_ops_n && !strcmp(ps_ops[ps_ops_n - 1].op, "populate")
		   && !ps_ops[ps_ops_n - 1].setter) {
			/* the quadrant's first write: its transaction began after the
			 * frozen state (its loads done, before its RNG draws) */
			if(ps_count_on && ps_ops[ps_ops_n - 1].k < 2 && rb_w[ps_ops[ps_ops_n - 1].k]->gen_storage_failed)
				ps_post[ps_ops[ps_ops_n - 1].k].tx_starts++;
			ps_ops[ps_ops_n - 1].setter = true;
			ps_pre_note(ps_ops[ps_ops_n - 1].k, ps_ops[ps_ops_n - 1].x, ps_ops[ps_ops_n - 1].z,
				ps_ops[ps_ops_n - 1].pre);
		}
		/* a load directly under a populate: its neighbour-load index */
		int load_idx = -1;
		if(!strcmp(e->operation, "load") && ps_ops_n && !strcmp(ps_ops[ps_ops_n - 1].op, "populate")
		   && ps_ops[ps_ops_n - 1].k == k)
			load_idx = ps_ops[ps_ops_n - 1].loads++;
		if(!strcmp(e->operation, "generate") && ps_ops_n && !strcmp(ps_ops[ps_ops_n - 1].op, "load"))
			ps_ops[ps_ops_n - 1].generated = true;
		/* terrain mode: arm the spill fault at the chosen load's start; its
		 * terrain height-edge reserve is the first reserve after it */
		if(ps_term_idx >= 0 && k < 2 && load_idx == ps_term_idx && ps_term_tgt[k].set
		   && !ps_armed[k] && !ps_hit[k] && ps_ops[ps_ops_n - 1].x == ps_term_tgt[k].qx
		   && ps_ops[ps_ops_n - 1].z == ps_term_tgt[k].qz && (int)e->x == ps_term_tgt[k].lx
		   && (int)e->z == ps_term_tgt[k].lz) {
			ps_term_pre[k] = ps_state(k);
			ps_owners_get(k, &ps_term_owners[k]);
			ps_armed[k] = true;
			server_world_test_light_queue_force_spill(true);
			server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_WRITE, 0);
		}
		bool nested = false;
		for(unsigned i = 0; i < ps_ops_n && !nested; i++)
			nested = ps_ops[i].k == k && !strcmp(ps_ops[i].op, "load");
		struct server_chunk* c = k < 2 && pop ? dr_chunk(rb_w[k], (int)e->x, (int)e->z) : NULL;
		ps_ops[ps_ops_n].k = k; ps_ops[ps_ops_n].op = e->operation;
		ps_ops[ps_ops_n].x = (int)e->x; ps_ops[ps_ops_n].z = (int)e->z;
		ps_ops[ps_ops_n].setter = false;
		ps_ops[ps_ops_n].nested = nested;
		ps_ops[ps_ops_n].loads = 0;
		ps_ops[ps_ops_n].load_idx = load_idx;
		ps_ops[ps_ops_n].generated = false;
		ps_ops[ps_ops_n].pre = k < 2 && pop ? ps_state(k) : 0;
		ps_ops[ps_ops_n++].before = c && c->populated;
		return true;
	}
	if(strcmp(e->edge, "end")) return true;
	assert(ps_ops_n > 0);
	ps_ops_n--;
	/* U: the first quadrant whose neighbour load at each index generated */
	if(ps_kind < 0 && k < 2 && !strcmp(e->operation, "load") && ps_ops[ps_ops_n].load_idx >= 0
	   && ps_ops[ps_ops_n].load_idx < 4 && ps_ops[ps_ops_n].generated && ps_ops_n > 0
	   && !ps_term_cur[k][ps_ops[ps_ops_n].load_idx].set)
		ps_term_cur[k][ps_ops[ps_ops_n].load_idx] = (struct ps_term){ps_ops[ps_ops_n - 1].x,
			ps_ops[ps_ops_n - 1].z, (int)e->x, (int)e->z, true};
	/* a neighbour load of a populate that has not reached its first setter:
	 * the populate's transaction-start state is after it */
	if(ps_ops_n && !strcmp(ps_ops[ps_ops_n - 1].op, "populate") && !ps_ops[ps_ops_n - 1].setter
	   && ps_ops[ps_ops_n - 1].k < 2 && !strcmp(e->operation, "load"))
		ps_ops[ps_ops_n - 1].pre = ps_state(ps_ops[ps_ops_n - 1].k);
	if(k > 1 || !pop) return true;
	if(ps_target[k].set && ps_target[k].x == (int)e->x && ps_target[k].z == (int)e->z)
		ps_disarm(k);
	if(!ps_ops[ps_ops_n].setter && !ps_ops[ps_ops_n].before && !ps_fail_noted[k]) {
		/* the attempt that met the stop before any write (tail reload or
		 * its loads): its start state is the frozen one */
		struct server_world_test_gen_storage g;
		server_world_test_gen_storage(rb_w[k], &g);
		if(g.failed && g.x == (int)e->x && g.z == (int)e->z) {
			ps_fail_noted[k] = true;
			ps_pre_note(k, (int)e->x, (int)e->z, ps_ops[ps_ops_n].pre);
		}
	}
	struct server_chunk* c = dr_chunk(rb_w[k], (int)e->x, (int)e->z);
	bool same = false; /* a load can re-enter populate for the SAME quadrant */
	for(unsigned i = 0; i < ps_ops_n && !same; i++)
		same = ps_ops[i].k == k && !strcmp(ps_ops[i].op, "populate")
			&& ps_ops[i].x == (int)e->x && ps_ops[i].z == (int)e->z;
	if(!same && !ps_ops[ps_ops_n].before && c && c->populated) {
		assert(ps_ok_n[k] < PS_MAX);
		ps_ok[k][ps_ok_n[k]++] = (struct ps_rec){(int)e->x, (int)e->z, e->hell_rng,
			ps_ops[ps_ops_n].nested};
	}
	return true;
}

static bool ps_light_hook(const struct lighting_history_event* e) {
	if(!e->operation) return true;
	for(unsigned k = 0; k < 2; k++) {
		if(ps_stopped[k] && e->object == rb_w[k]) ps_after_events[k]++;
		if(ps_count_on && e->object == rb_w[k] && rb_w[k]->gen_storage_failed) ps_post[k].light_events++;
	}
	if(!ps_ops_n) return true;
	/* the innermost populate, with only its own setter writes above it */
	unsigned p = ps_ops_n;
	while(p > 0 && strcmp(ps_ops[p - 1].op, "populate")) {
		if(strncmp(ps_ops[p - 1].op, "setter", 6) && strcmp(ps_ops[p - 1].op, "raw-id-meta"))
			return true;
		p--;
	}
	if(!p) return true;
	p--;
	const unsigned k = ps_ops[p].k;
	if(k > 1) return true;
	const int x = ps_ops[p].x, z = ps_ops[p].z;
	if(ps_kind < 0) { /* U: which quadrants' own populations admit regions */
		if(strcmp(e->operation, "region-admit")) return true;
		for(size_t i = 0; i < ps_admit_n; i++)
			if(ps_admit[i].k == k && ps_admit[i].x == x && ps_admit[i].z == z) return true;
		assert(ps_admit_n < PS_MAX);
		ps_admit[ps_admit_n].k = k; ps_admit[ps_admit_n].x = x; ps_admit[ps_admit_n++].z = z;
		return true;
	}
	if(ps_term_idx >= 0 || !ps_target[k].set || ps_armed[k] || ps_hit[k] || x != ps_target[k].x
	   || z != ps_target[k].z)
		return true;
	ps_armed[k] = true;
	if(ps_force) server_world_test_light_queue_force_spill(true);
	server_world_test_light_queue_fault((enum server_world_test_lq_fault)ps_kind, 0);
	return true;
}

static const struct ps_rec* ps_find(unsigned k, int x, int z, const struct ps_rec* list, size_t n) {
	for(size_t i = 0; i < n; i++) if(list[i].x == x && list[i].z == z) return &list[i];
	(void)k;
	return NULL;
}
static uint64_t ps_pre_of(unsigned k, int x, int z, bool* found) {
	for(size_t i = 0; i < ps_pre_n; i++)
		if(ps_pre[i].k == k && ps_pre[i].x == x && ps_pre[i].z == z) { *found = true; return ps_pre[i].pre; }
	*found = false;
	return 0;
}

/* the U reference */
static struct ps_rec ps_u_ok[2][2][PS_MAX];
static size_t ps_u_n[2][2];
static struct ps_rec ps_u_top[2][2], ps_u_nested[2][2];
static uint64_t ps_u_pre_top[2][2], ps_u_pre_nested[2][2];
static uint64_t ps_u_final[2][2];

enum ps_mode { PS_U, PS_SPILL_TOP, PS_SPILL_NESTED, PS_TAIL, PS_NONSTORAGE, PS_TERRAIN };

static const char* ps_kind_name(int kind) {
	static const char* names[] = {"OPEN", "WRITE", "READ", "ALLOC", "RESERVE", "SHORT_WRITE",
		"SHORT_READ", "SEEK", "FLUSH"};
	return kind >= 0 && kind < SERVER_WORLD_TEST_LQ_FAULTS ? names[kind] : "none";
}
static unsigned ps_cause_of(int kind) {
	switch(kind) {
	case SERVER_WORLD_TEST_LQ_OPEN: return SERVER_POPULATION_STORAGE_OPEN;
	case SERVER_WORLD_TEST_LQ_WRITE: return SERVER_POPULATION_STORAGE_WRITE;
	case SERVER_WORLD_TEST_LQ_READ: return SERVER_POPULATION_STORAGE_READ;
	case SERVER_WORLD_TEST_LQ_SHORT_WRITE: return SERVER_POPULATION_STORAGE_SHORT_WRITE;
	case SERVER_WORLD_TEST_LQ_SHORT_READ: return SERVER_POPULATION_STORAGE_SHORT_READ;
	case SERVER_WORLD_TEST_LQ_SEEK: return SERVER_POPULATION_STORAGE_SEEK;
	case SERVER_WORLD_TEST_LQ_FLUSH: return SERVER_POPULATION_STORAGE_FLUSH;
	default: return SERVER_POPULATION_STORAGE_NONE;
	}
}

static bool ps_ensure(unsigned k, int x, int z, bool far) {
	struct server_chunk* sc = NULL;
	return far ? server_world_ensure_populated_far(rb_w[k], &S[1], x, z, &sc)
		: server_world_ensure_populated(rb_w[k], &S[1], x, z, &sc);
}

/* After the stop: nothing moves, whatever is tried. */
static void ps_after_stop(unsigned k, const char* what_run) {
	struct server_world* w = rb_w[k];
	struct server_world_test_gen_storage g0, g1;
	server_world_test_gen_storage(w, &g0);
	const uint64_t s0 = ps_state(k);
	struct ps_owners o0, o1;
	ps_owners_get(k, &o0);
	const size_t ok0 = ps_ok_n[k];
	const char* path0 = server_world_test_light_queue_backing_path(w);
	char path[600];
	snprintf(path, sizeof path, "%s", path0);
	ps_after_events[k] = 0;
	ps_stopped[k] = true;
	bool any_ok = false;
	for(unsigned r = 0; r < 3; r++) { /* repeated ticks: the boundary drain + the same request */
		any_ok |= server_world_light_drain(w);
		any_ok |= ps_ensure(k, PS_X, PS_Z, false);
	}
	/* moved requests (near and far streams) */
	any_ok |= ps_ensure(k, PS_X + 5, PS_Z - 3, false);
	any_ok |= ps_ensure(k, PS_X - 4, PS_Z + 6, true);
	/* fault removal (every kind disarmed) and an attempted generic latch clear */
	for(int f = 0; f < SERVER_WORLD_TEST_LQ_FAULTS; f++)
		server_world_test_light_queue_fault((enum server_world_test_lq_fault)f, -1);
	server_world_test_light_queue_force_spill(false);
	server_world_test_light_clear_fault(w);
	const bool halted_after_clear = server_world_light_halted(w) && w->light_fault;
	any_ok |= server_world_light_drain(w);
	any_ok |= ps_ensure(k, PS_X, PS_Z, false);
	any_ok |= ps_ensure(k, PS_X + 2, PS_Z + 2, false);
	ps_stopped[k] = false;
	server_world_test_gen_storage(w, &g1);
	ps_owners_get(k, &o1);
	char what[600], post[300];
	ps_post_str(post, sizeof post, k);
	snprintf(what, sizeof what, "%s: every retained RNG / work owner unchanged across the after-stop "
		"attempts (%u fields moved); over the failing call and after it, %s", what_run,
		ps_owners_moved(&o0, &o1), post);
	rb_check(ps_owners_moved(&o0, &o1) == 0 && ps_post_zero(k), what);
	snprintf(what, sizeof what, "%s: after the stop, 3 ticks + 2 moved requests + fault removal + a generic "
		"latch clear change nothing (state equal %d, populated +%zu, lighting events %zu, any ok %d, "
		"halted after clear %d, record kept %d, reason %u)", what_run, ps_state(k) == s0,
		ps_ok_n[k] - ok0, ps_after_events[k], any_ok, halted_after_clear,
		g1.failed && g1.cause == g0.cause && g1.site == g0.site && g1.x == g0.x && g1.z == g0.z,
		w->light_fault_reason);
	rb_check(!any_ok && ps_state(k) == s0 && ps_ok_n[k] == ok0 && ps_after_events[k] == 0
		&& halted_after_clear && w->light_fault_reason == SERVER_LIGHT_FAULT_POPULATION_STORAGE
		&& g1.failed && g1.cause == g0.cause && g1.site == g0.site && g1.x == g0.x && g1.z == g0.z
		&& g1.io_fails == g0.io_fails, what);
	if(path[0]) {
		FILE* f = fopen(path, "rb");
		snprintf(what, sizeof what, "%s: the stopped world still owns its backing file (pending work)", what_run);
		rb_check(f != NULL, what);
		if(f) fclose(f);
	}
}

static void ps_run(unsigned ow_slot, enum ps_mode mode, int kind) {
	char dirs[2][260], tag[16];
	static unsigned run_id;
	snprintf(tag, sizeof tag, "p%u", run_id++);
	rb_bounded = true;
	ps_kind = mode == PS_U ? -1 : kind;
	ps_force = mode == PS_SPILL_TOP || mode == PS_SPILL_NESTED;
	assert((mode == PS_TERRAIN) == (ps_term_idx >= 0));
	rb_open(ow_slot, tag, dirs);
	/* Math.random is process-global: every run starts from the same state,
	 * so U's and this run's states (which include it) are comparable */
	static struct server_local_math_random_state math0; /* also re-pinned per world below */
	static bool math0_set;
	if(!math0_set) { server_local_math_random_capture(&math0); math0_set = true; }
	server_local_math_random_restore(&math0);
	uint64_t pre_ensure[2], q_pre_ensure[2];
	size_t qn_pre_ensure[2];
	for(unsigned k = 0; k < 2; k++) {
		struct server_world* w = rb_w[k];
		const bool nether = w->dimension == WORLD_DIM_NETHER;
		/* production ring, no backing file yet (the fixture drained) */
		assert(server_world_test_light_queue_slots(w, mode == PS_TAIL ? (nether ? 2u : 16u) : 128u));
		/* two pages pending, so a page is always spillable */
		assert(rb_fill(w, 2 * RB_PAGE, 800 + k) == 2 * RB_PAGE);
		ps_ok_n[k] = 0; ps_armed[k] = ps_hit[k] = ps_stopped[k] = ps_fail_noted[k] = false;
		ps_target[k].set = false;
		memset(&ps_post[k], 0, sizeof ps_post[k]);
		memset(ps_term_cur[k], 0, sizeof ps_term_cur[k]);
		ps_term_tgt[k].set = false;
		if(mode == PS_TERRAIN) {
			ps_term_tgt[k] = ps_u_term[ow_slot][k][ps_term_idx];
			ps_target[k].x = ps_term_tgt[k].qx; ps_target[k].z = ps_term_tgt[k].qz;
			ps_target[k].set = ps_term_tgt[k].set;
		} else if(mode == PS_SPILL_TOP || mode == PS_NONSTORAGE) {
			ps_target[k].x = ps_u_top[ow_slot][k].x; ps_target[k].z = ps_u_top[ow_slot][k].z;
			ps_target[k].set = true;
		} else if(mode == PS_SPILL_NESTED) {
			ps_target[k].x = ps_u_nested[ow_slot][k].x; ps_target[k].z = ps_u_nested[ow_slot][k].z;
			ps_target[k].set = ps_u_nested[ow_slot][k].x != INT_MIN;
		} else if(mode == PS_TAIL) {
			/* a rolled-back population (no backing failure) leaves the
			 * mergeable tail spilled: the next reload is the first I/O */
			const size_t ops = rb_pop_script();
			size_t wa = 0, wb = 0;
			assert(server_world_test_populate_setter_script_fail(w, 0, 0, rb_pop_ops, ops, ops - 1,
				&wa, &wb, NULL, NULL));
			struct server_world_light_queue_stats st;
			rb_stats(w, &st);
			char what[200];
			snprintf(what, sizeof what, "%s tail-reload fixture: after the rolled-back population the "
				"tail is not resident (n %zu, %zu pages spilled)", nether ? "nether" : "overworld",
				w->generation_light_n, st.spilled_pages);
			rb_check(st.spilled_pages * RB_PAGE + 6 > w->generation_light_n && !w->light_fault, what);
		}
		pre_ensure[k] = ps_state(k);
		q_pre_ensure[k] = rb_queue_hash(w, &qn_pre_ensure[k]);
	}
	ps_ops_n = 0; ps_pre_n = 0;
	if(mode == PS_U) ps_admit_n = 0;
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_world_test_history_reset();
	server_world_test_history_hook = ps_hook;
	lighting_test_history_hook = ps_light_hook;
	ps_count_on = true; /* the post-stop counters cover the failing call */
	for(unsigned k = 0; k < 2; k++) {
		struct server_world* w = rb_w[k];
		const char* dim = w->dimension == WORLD_DIM_NETHER ? "nether" : "overworld";
		char run[96], what[720];
		snprintf(run, sizeof run, "slot-order %u %s %s %s", ow_slot, dim,
			mode == PS_U ? "uninterrupted" : mode == PS_SPILL_TOP ? "spill fault (top-level quadrant)"
			: mode == PS_SPILL_NESTED ? "spill fault (nested quadrant)" : mode == PS_TAIL
			? "tail-reload fault" : mode == PS_TERRAIN ? "terrain spill fault (neighbour load)"
			: "NON-storage reserve failure", ps_kind_name(ps_kind));
		if(mode == PS_TERRAIN) {
			char idx[16];
			snprintf(idx, sizeof idx, " #%d", ps_term_idx);
			strncat(run, idx, sizeof run - strlen(run) - 1);
			/* CFR ChunkProviderGenerate.java:234 / Chunk.java:90-116 queue the
			 * height edges of a new OVERWORLD chunk; ChunkProviderHell's
			 * generation queues none, so a Nether load has no terrain reserve */
			if(w->dimension == WORLD_DIM_NETHER) {
				printf("  %s: n/a (Nether terrain generation queues no light requests)\n", run);
				continue;
			}
			if(!ps_term_tgt[k].set) {
				snprintf(what, sizeof what, "%s: no quadrant whose neighbour load #%d generates was "
					"observed in U", run, ps_term_idx);
				rb_check(false, what);
				continue;
			}
		}
		if(mode == PS_SPILL_NESTED && !ps_target[k].set) {
			snprintf(what, sizeof what, "%s: no nested admitting quadrant was observed in U", run);
			rb_check(false, what);
			continue;
		}
		/* each world's segment starts from the same process-global
		 * Math.random state, whatever the other world drew before it */
		server_local_math_random_restore(&math0);
		struct server_world_light_queue_stats st0, st;
		rb_stats(w, &st0);
		const size_t io0 = w->generation_light_io_fails;
		if(mode == PS_TAIL) {
			ps_armed[k] = true;
			server_world_test_light_queue_fault((enum server_world_test_lq_fault)kind, 0);
		}
		bool done = false;
		size_t calls = 0;
		struct server_world_test_gen_storage g = {0};
		/* two columns: (PS_X, PS_Z), then PS_X + 2 -- whose loads complete
		 * the 2x2 between them, so Beta's neighbour populate runs NESTED
		 * inside a load (ChunkProviderServer.java:67-95) */
		static const int cols[2][2] = {{PS_X, PS_Z}, {PS_X + 2, PS_Z}};
		for(unsigned c = 0; c < 2 && !g.failed; c++) {
			done = false;
			for(size_t n = 0; n < 32 && !done; n++, calls++) {
				done = ps_ensure(k, cols[c][0], cols[c][1], false);
				server_world_test_gen_storage(w, &g);
				if(g.failed) break;
			}
		}
		if(mode == PS_TAIL || mode == PS_TERRAIN) ps_disarm(k);
		server_world_test_gen_storage(w, &g);
		rb_stats(w, &st);
		if(mode == PS_U || mode == PS_NONSTORAGE) {
			snprintf(what, sizeof what, "%s: provisioned in %zu call(s); no population storage stop "
				"(failed %d, io failures +%zu, light fault %d)%s", run, calls, g.failed,
				w->generation_light_io_fails - io0, w->light_fault,
				mode == PS_NONSTORAGE ? " -- an allocation-class rollback is never the terminal stop" : "");
			rb_check(done && !g.failed && w->generation_light_io_fails == io0 && !w->light_fault
				&& (mode == PS_U || ps_hit[k]), what);
			if(mode == PS_U) {
				memcpy(ps_u_term[ow_slot][k], ps_term_cur[k], sizeof ps_term_cur[k]);
				memcpy(ps_u_ok[ow_slot][k], ps_ok[k], sizeof ps_ok[k]);
				ps_u_n[ow_slot][k] = ps_ok_n[k];
				/* targets: the first admitting quadrant after the first, top-level
				 * and nested, each followed by another populated quadrant */
				ps_u_top[ow_slot][k].x = ps_u_nested[ow_slot][k].x = INT_MIN;
				for(size_t i = 1; i + 1 < ps_ok_n[k]; i++) {
					bool admits = false;
					for(size_t a = 0; a < ps_admit_n && !admits; a++)
						admits = ps_admit[a].k == k && ps_admit[a].x == ps_ok[k][i].x
							&& ps_admit[a].z == ps_ok[k][i].z;
					if(!admits) continue;
					struct ps_rec* slot = ps_ok[k][i].nested ? &ps_u_nested[ow_slot][k] : &ps_u_top[ow_slot][k];
					uint64_t* pre = ps_ok[k][i].nested ? &ps_u_pre_nested[ow_slot][k] : &ps_u_pre_top[ow_slot][k];
					if(slot->x != INT_MIN) continue;
					*slot = ps_ok[k][i];
					bool found;
					*pre = ps_pre_of(k, ps_ok[k][i].x, ps_ok[k][i].z, &found);
					assert(found);
				}
				snprintf(what, sizeof what, "%s: an admitting top-level quadrant followed by another is "
					"observed (target (%d,%d) of %zu populated; nested target %s)", run,
					ps_u_top[ow_slot][k].x, ps_u_top[ow_slot][k].z, ps_ok_n[k],
					ps_u_nested[ow_slot][k].x != INT_MIN ? "found" : "none");
				rb_check(ps_u_top[ow_slot][k].x != INT_MIN, what);
				ps_u_final[ow_slot][k] = ps_state(k);
			}
			printf("  %s: %zu call(s), %zu quadrants populated\n", run, calls, ps_ok_n[k]);
			continue;
		}
		/* a backing failure: the sticky terminal stop */
		const unsigned want_cause = ps_cause_of(kind);
		const bool pre_tx_site = g.site == SERVER_POPULATION_SITE_PROVISION
			|| g.site == SERVER_POPULATION_SITE_LOAD || g.site == SERVER_POPULATION_SITE_TX_BEGIN
			|| g.site == SERVER_POPULATION_SITE_TERRAIN;
		/* the provisioned column's own terrain = the provision's start */
		const bool c_site = g.site == SERVER_POPULATION_SITE_PROVISION
			|| (g.site == SERVER_POPULATION_SITE_TERRAIN && g.x == PS_X && g.z == PS_Z);
		snprintf(what, sizeof what, "%s: the first backing failure is the sticky POPULATION_STORAGE_FAILED "
			"stop in the same call (failed %d after %zu call(s), fault hit %d, cause %u want %u, site %u, "
			"quadrant (%d,%d), reason %u)", run, g.failed, calls, ps_hit[k], g.cause, want_cause, g.site,
			g.x, g.z, w->light_fault_reason);
		rb_check(g.failed && !done && ps_hit[k] && g.cause == want_cause && w->light_fault
			&& w->light_fault_reason == SERVER_LIGHT_FAULT_POPULATION_STORAGE
			&& server_world_light_halted(w)
			&& (mode == PS_TAIL ? pre_tx_site
				: mode == PS_TERRAIN ? g.site == SERVER_POPULATION_SITE_TERRAIN
					&& g.x == ps_term_tgt[k].lx && g.z == ps_term_tgt[k].lz
				: g.site == SERVER_POPULATION_SITE_TX && g.x == ps_target[k].x && g.z == ps_target[k].z),
			what);
		{ /* P2: direct counters armed before this call; owners at the end of
		   * the call == at the latch, field by field */
			struct ps_owners o_end;
			ps_owners_get(k, &o_end);
			const unsigned moved = ps_post[k].seen ? ps_owners_moved(&ps_post[k].at_stop, &o_end) : 99u;
			char post[300];
			ps_post_str(post, sizeof post, k);
			snprintf(what, sizeof what, "%s: within the failing call, %s; owners at its end vs the latch: "
				"%u fields moved (latch observed %d)", run, post, moved, ps_post[k].seen);
			rb_check(ps_post_zero(k) && ps_post[k].seen && moved == 0, what);
		}
		printf("  %s: stop cause %u site %u at (%d,%d) after %zu populated quadrant(s)\n", run, g.cause,
			g.site, g.x, g.z, ps_ok_n[k]);
		if(mode == PS_TAIL) {
			/* nothing began: the logical queue (its rolled-back tail patches
			 * included) is the pre-provision list exactly; the stopping
			 * quadrant (if a population) is at its transaction-start state */
			size_t qn; const uint64_t q = rb_queue_hash(w, &qn);
			bool found = false;
			const uint64_t pre = c_site ? pre_ensure[k] : ps_pre_of(k, g.x, g.z, &found);
			if(c_site) found = true;
			snprintf(what, sizeof what, "%s: no transaction began -- the queue is the pre-provision list "
				"(%zu == %zu entries, equal %d) and the state equals the stopping point's start (found %d, "
				"equal %d)", run, qn, qn_pre_ensure[k], q == q_pre_ensure[k], found, found && ps_state(k) == pre);
			rb_check(q == q_pre_ensure[k] && qn == qn_pre_ensure[k] && found && ps_state(k) == pre, what);
		} else if(mode == PS_TERRAIN) {
			/* the generated chunk was discarded (never installed); the state
			 * and every owner equal that load's start; the populated
			 * quadrants are U's, in order, and the quadrant never populated */
			struct ps_owners o_end;
			ps_owners_get(k, &o_end);
			const bool absent = dr_chunk(w, ps_term_tgt[k].lx, ps_term_tgt[k].lz) == NULL;
			const bool prefix = ps_ok_n[k] <= ps_u_n[ow_slot][k]
				&& !memcmp(ps_ok[k], ps_u_ok[ow_slot][k], ps_ok_n[k] * sizeof ps_ok[k][0]);
			const bool quad_never = !ps_find(k, ps_term_tgt[k].qx, ps_term_tgt[k].qz, ps_ok[k], ps_ok_n[k]);
			snprintf(what, sizeof what, "%s: quadrant (%d,%d) load (%d,%d): chunk discarded %d, state == the "
				"load's start %d, owners moved %u, populated = U's first %zu in order %d, quadrant never "
				"populated %d, reason %u (not RESERVE)", run, ps_term_tgt[k].qx, ps_term_tgt[k].qz,
				ps_term_tgt[k].lx, ps_term_tgt[k].lz, absent, ps_state(k) == ps_term_pre[k],
				ps_owners_moved(&ps_term_owners[k], &o_end), ps_ok_n[k], prefix, quad_never,
				w->light_fault_reason);
			rb_check(absent && ps_state(k) == ps_term_pre[k] && ps_owners_moved(&ps_term_owners[k], &o_end) == 0
				&& prefix && quad_never && w->light_fault_reason != SERVER_LIGHT_FAULT_RESERVE, what);
		} else {
			/* exact pre-quadrant rollback: equal to this run's AND to U's
			 * transaction-start state of the target; the quadrants that
			 * populated are U's, in order, up to (not incl.) the target */
			bool found;
			const uint64_t pre = ps_pre_of(k, ps_target[k].x, ps_target[k].z, &found);
			const uint64_t u_pre = mode == PS_SPILL_TOP ? ps_u_pre_top[ow_slot][k] : ps_u_pre_nested[ow_slot][k];
			size_t ti = 0;
			while(ti < ps_u_n[ow_slot][k] && !(ps_u_ok[ow_slot][k][ti].x == ps_target[k].x
				&& ps_u_ok[ow_slot][k][ti].z == ps_target[k].z)) ti++;
			const bool prefix = ps_ok_n[k] == ti && !memcmp(ps_ok[k], ps_u_ok[ow_slot][k], ti * sizeof ps_ok[k][0]);
			snprintf(what, sizeof what, "%s: the target (%d,%d) rolled back to its exact transaction start "
				"(this run %d, U %d) and the populated quadrants are U's first %zu in order (%zu, equal %d); "
				"no later quadrant started", run, ps_target[k].x, ps_target[k].z,
				found && ps_state(k) == pre, ps_state(k) == u_pre, ti, ps_ok_n[k], prefix);
			rb_check(found && ps_state(k) == pre && ps_state(k) == u_pre && prefix
				&& !ps_find(k, ps_target[k].x, ps_target[k].z, ps_ok[k], ps_ok_n[k]), what);
			if(!prefix)
				for(size_t i = 0; i < PS_MAX && (i < ps_u_n[ow_slot][k] || i < ps_ok_n[k]); i++)
					printf("    order %zu: U (%d,%d) %016llx | this run (%d,%d) %016llx\n", i,
						i < ps_u_n[ow_slot][k] ? ps_u_ok[ow_slot][k][i].x : 0,
						i < ps_u_n[ow_slot][k] ? ps_u_ok[ow_slot][k][i].z : 0,
						(unsigned long long)(i < ps_u_n[ow_slot][k] ? ps_u_ok[ow_slot][k][i].rng : 0),
						i < ps_ok_n[k] ? ps_ok[k][i].x : 0, i < ps_ok_n[k] ? ps_ok[k][i].z : 0,
						(unsigned long long)(i < ps_ok_n[k] ? ps_ok[k][i].rng : 0));
		}
		ps_after_stop(k, run);
		(void)st0;
	}
	ps_count_on = false;
	server_world_test_history_hook = NULL;
	lighting_test_history_hook = NULL;
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_NORMAL);
	for(int f = 0; f < SERVER_WORLD_TEST_LQ_FAULTS; f++)
		server_world_test_light_queue_fault((enum server_world_test_lq_fault)f, -1);
	server_world_test_light_queue_force_spill(false);
	ps_kind = -1;
	rb_pop_script();
	rb_close(dirs);
}

static void rb_population_stop(void) {
	static const int spill_kinds[] = {SERVER_WORLD_TEST_LQ_OPEN, SERVER_WORLD_TEST_LQ_SEEK,
		SERVER_WORLD_TEST_LQ_WRITE, SERVER_WORLD_TEST_LQ_SHORT_WRITE, SERVER_WORLD_TEST_LQ_FLUSH};
	static const int load_kinds[] = {SERVER_WORLD_TEST_LQ_READ, SERVER_WORLD_TEST_LQ_SHORT_READ,
		SERVER_WORLD_TEST_LQ_SEEK};
	for(unsigned ow_slot = 0; ow_slot < 2; ow_slot++) {
		ps_run(ow_slot, PS_U, -1);
		for(unsigned i = 0; i < sizeof spill_kinds / sizeof spill_kinds[0]; i++)
			ps_run(ow_slot, PS_SPILL_TOP, spill_kinds[i]);
		ps_run(ow_slot, PS_SPILL_NESTED, SERVER_WORLD_TEST_LQ_WRITE);
		for(unsigned i = 0; i < sizeof load_kinds / sizeof load_kinds[0]; i++)
			ps_run(ow_slot, PS_TAIL, load_kinds[i]);
		ps_run(ow_slot, PS_NONSTORAGE, SERVER_WORLD_TEST_LQ_RESERVE);
		/* P1: a spill failure at the terrain reserve of each of the 4
		 * neighbour loads */
		for(int i = 0; i < 4; i++) {
			ps_term_idx = i;
			ps_run(ow_slot, PS_TERRAIN, SERVER_WORLD_TEST_LQ_WRITE);
			ps_term_idx = -1;
		}
	}
}

/* ---- G9X-5 storage-failure matrix (problem P4), the production code with a
 * 2-page window, OUTSIDE population. Every refused admission must leave the
 * whole logical list byte-identical, be classified (backing I/O cause vs
 * allocation) and be admitted on a retry; every load failure must stop the
 * drain before the pop that needed the page. (The memory preflight is
 * G9X-5b's, not tested here.) */
static uint64_t mx_hash(struct server_world* w, size_t* n) { return rb_queue_hash(w, n); }

static void mx_reset(struct server_world* w) {
	if(w->generation_light_n) assert(server_world_light_drain(w));
	assert(server_world_test_light_queue_slots(w, 2));
}

static void mx_admit_fault(struct server_world* w, const char* dim, enum server_world_test_lq_fault kind,
	const char* name, size_t fill, unsigned want_site, unsigned want_cause) {
	mx_reset(w);
	assert(rb_fill(w, fill, 700) == fill);
	struct server_world_light_queue_stats st0, st;
	rb_stats(w, &st0);
	const size_t io0 = w->generation_light_io_fails;
	size_t n0; const uint64_t h0 = mx_hash(w, &n0);
	server_world_test_light_queue_fault(kind, 0);
	const int x = 31, y = 127, z = 31; /* never merges with the 2-spaced fill */
	const bool refused = !server_world_test_generation_light_schedule(w, COLUMN_TORCH, x, y, z, x, y, z);
	const bool hit = server_world_test_light_queue_fault_count(kind) > 0;
	server_world_test_light_queue_fault(kind, -1);
	rb_stats(w, &st);
	size_t n1; const uint64_t h1 = mx_hash(w, &n1);
	const bool alloc = kind == SERVER_WORLD_TEST_LQ_ALLOC;
	char what[300];
	snprintf(what, sizeof what, "%s %s: the admission is refused before any change (list %zu == %zu, "
		"hash equal %d, spilled %zu == %zu, site %u want %u, cause %u want %u, I/O failures +%zu, "
		"allocation failures +%zu, world I/O record +%zu)", dim, name, n1, n0, h1 == h0, st.spilled_pages,
		st0.spilled_pages, st.last_fail_site, want_site, st.last_fail_cause, want_cause,
		st.storage_failures - st0.storage_failures, st.alloc_failures - st0.alloc_failures,
		w->generation_light_io_fails - io0);
	rb_check(refused && hit && n1 == n0 && h1 == h0 && st.spilled_pages == st0.spilled_pages
		&& st.last_fail_site == want_site && st.last_fail_cause == want_cause
		&& st.storage_failures - st0.storage_failures == (alloc ? 0u : 1u)
		&& st.alloc_failures - st0.alloc_failures == (alloc ? 1u : 0u)
		&& w->generation_light_io_fails - io0 == (alloc ? 0u : 1u)
		&& (alloc || w->generation_light_io_cause == want_cause)
		&& !w->light_fault, what);
	/* retry: admitted after the unchanged prefix */
	const bool ok = server_world_test_generation_light_schedule(w, COLUMN_TORCH, x, y, z, x, y, z);
	const size_t n2 = w->generation_light_n;
	int r[7];
	const bool last = server_world_test_generation_light_region(w, n2 - 1, r);
	uint64_t hp = 0xCBF29CE484222325ull;
	for(size_t i = 0; i < n0; i++) {
		int e[7]; assert(server_world_test_generation_light_region(w, i, e)); hp = dr_fnv(hp, e, sizeof e);
	}
	snprintf(what, sizeof what, "%s %s: the retry admits it after the unchanged %zu entries", dim, name, n0);
	rb_check(ok && n2 == n0 + 1 && hp == h0 && last && r[0] == COLUMN_TORCH && r[1] == x && r[2] == y
		&& r[3] == z && r[4] == x && r[5] == y && r[6] == z, what);
	assert(server_world_light_drain(w));
}

static bool mx_pop_hook(const struct lighting_history_event* e) { rb_note_pop(e); return true; }

static void mx_load_fault(struct server_world* w, const char* dim, enum server_world_test_lq_fault kind,
	const char* name) {
	mx_reset(w);
	/* 668 entries: base 4, and slack in the window so the drain's relax
	 * reserve(7) never spills -- the first storage access is the load */
	assert(rb_fill(w, 6 * RB_PAGE - 100, 710) == 6 * RB_PAGE - 100);
	size_t n0;
	struct rb_region* before = rb_queue_copy(w, &n0);
	server_world_test_light_queue_fault(kind, 0);
	rb_min_depth = SIZE_MAX;
	lighting_test_history_hook = mx_pop_hook;
	const bool done = server_world_light_drain(w);
	lighting_test_history_hook = NULL;
	const bool hit = server_world_test_light_queue_fault_count(kind) > 0;
	server_world_test_light_queue_fault(kind, -1);
	struct server_world_light_queue_stats st;
	rb_stats(w, &st);
	size_t n1;
	struct rb_region* now = rb_queue_copy(w, &n1);
	const size_t keep = st.spilled_pages * RB_PAGE;
	const size_t low = rb_min_depth < 5 ? 0 : rb_min_depth - 5;
	char what[300];
	snprintf(what, sizeof what, "%s %s: the drain stops (reason %u, site %u) before the pop that needed "
		"the page; %zu spilled + %zu resident retained; the %zu entries below its lowest depth %zu "
		"(minus the merge window) are the pre-drain entries exactly", dim, name, w->light_fault_reason,
		st.last_fail_site, keep, n1 - keep, low, rb_min_depth);
	rb_check(hit && !done && w->light_fault && w->light_fault_reason == SERVER_LIGHT_FAULT_DRAIN_OOM
		&& st.last_fail_site == SERVER_WORLD_TEST_LQ_SITE_LOAD && keep > 0 && keep <= n1 && n1 - keep <= 6
		&& low > 0 && low <= keep && low <= n0 && !memcmp(now, before, low * sizeof *now), what);
	free(now); free(before);
	server_world_test_light_clear_fault(w);
	rb_check(server_world_light_drain(w) && w->generation_light_n == 0, "the explicit retry settles it");
}

/* a population rolled back over a spilled tail, then a load fault at the
 * very first page the next drain needs: the stopped list IS the pre-drain
 * list, entry for entry (the rollback patches included) */
static void mx_rollback_load_fault(struct server_world* w, const char* dim) {
	mx_reset(w);
	const size_t ops = rb_pop_script();
	assert(rb_fill(w, 300, 720) == 300);
	size_t n0;
	struct rb_region* before = rb_queue_copy(w, &n0);
	size_t wa = 0, wb = 0;
	assert(server_world_test_populate_setter_script_fail(w, 0, 0, rb_pop_ops, ops, ops - 1, &wa, &wb,
		NULL, NULL));
	server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_READ, 0);
	const bool done = server_world_light_drain(w);
	const bool hit = server_world_test_light_queue_fault_count(SERVER_WORLD_TEST_LQ_READ) > 0;
	server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_READ, -1);
	size_t n1;
	struct rb_region* now = rb_queue_copy(w, &n1);
	char what[200];
	snprintf(what, sizeof what, "%s rollback + load fault: the stopped list is the pre-drain list exactly "
		"(%zu == %zu entries)", dim, n1, n0);
	rb_check(hit && !done && n1 == n0 && !memcmp(now, before, n0 * sizeof *now), what);
	free(now); free(before);
	server_world_test_light_clear_fault(w);
	rb_check(server_world_light_drain(w) && w->generation_light_n == 0,
		"the explicit retry settles the rolled-back list");
}

static void rb_fault_matrix(void) {
	char dirs[2][260];
	rb_bounded = true;
	rb_open(0, "m", dirs);
	for(unsigned k = 0; k < 2; k++) {
		struct server_world* w = rb_w[k];
		const char* dim = w->dimension == WORLD_DIM_NETHER ? "nether" : "overworld";
		const size_t spill = 3 * RB_PAGE; /* window full, backing open: the next admission spills */
		mx_admit_fault(w, dim, SERVER_WORLD_TEST_LQ_OPEN, "backing OPEN fault", 2 * RB_PAGE,
			SERVER_WORLD_TEST_LQ_SITE_OPEN, SERVER_POPULATION_STORAGE_OPEN);
		mx_admit_fault(w, dim, SERVER_WORLD_TEST_LQ_WRITE, "spill WRITE fault", spill,
			SERVER_WORLD_TEST_LQ_SITE_SPILL, SERVER_POPULATION_STORAGE_WRITE);
		mx_admit_fault(w, dim, SERVER_WORLD_TEST_LQ_SHORT_WRITE, "real SHORT write", spill,
			SERVER_WORLD_TEST_LQ_SITE_SPILL, SERVER_POPULATION_STORAGE_SHORT_WRITE);
		mx_admit_fault(w, dim, SERVER_WORLD_TEST_LQ_SEEK, "spill SEEK fault", spill,
			SERVER_WORLD_TEST_LQ_SITE_SPILL, SERVER_POPULATION_STORAGE_SEEK);
		mx_admit_fault(w, dim, SERVER_WORLD_TEST_LQ_FLUSH, "spill FLUSH fault", spill,
			SERVER_WORLD_TEST_LQ_SITE_SPILL, SERVER_POPULATION_STORAGE_FLUSH);
		mx_admit_fault(w, dim, SERVER_WORLD_TEST_LQ_ALLOC, "page-slot ALLOC fault (allocation, not I/O)", 0,
			SERVER_WORLD_TEST_LQ_SITE_ALLOC, SERVER_POPULATION_STORAGE_NONE);
		mx_load_fault(w, dim, SERVER_WORLD_TEST_LQ_READ, "page READ fault");
		mx_load_fault(w, dim, SERVER_WORLD_TEST_LQ_SHORT_READ, "real SHORT read");
		mx_load_fault(w, dim, SERVER_WORLD_TEST_LQ_SEEK, "load SEEK fault");
		mx_rollback_load_fault(w, dim);
		assert(server_world_test_light_queue_slots(w, 128));
	}
	rb_close(dirs);
}

static void case_g9x_region_budget(void) {
	const char* ordered = getenv("CW_G9X5_ORDERED_OUT");
	if(ordered) rb_ordered_tape(ordered);
	rb_population_stop();
	rb_fault_matrix();
	for(unsigned ow_slot = 0; ow_slot < 2; ow_slot++) {
		rb_step_n = rb_step_i = 0;
		rb_run(ow_slot, true);
		rb_run(ow_slot, false);
		rb_check(rb_step_i == rb_step_n, "the reference reached every bounded step");
	}
	printf("  g9x_region_budget: %ld PASS, %ld G9X-5 RED\n", rb_pass, rb_red);
	if(rb_red) s_fail++;
}

/* ---- G9X-5b: g9x_memory_progress -------------------------------------------
 * Part 1: mp_increment_arena. The checkpoint trial's allocations must be
 * admitted per allocator INCREMENT, in production order, never as one
 * aggregate: newlib grows its heap by each request's own page-rounded
 * increment, and libogc _sbrk_r puts an increment WHOLE in MEM1 while it
 * fits there (logc256kr/libogc/sbrk.c:16-68), so the small IDs / cursor
 * window / slot bitmap land in MEM1 BEFORE the 8 MiB planes switch the heap
 * to Arena2. With MEM1 = floor + 64 KiB a full 256-column, full-cursor trial
 * would push MEM1 below the 4,096 KiB floor, although the aggregate
 * ("8.8 MB does not fit MEM1, so Arena2") model admits it. The production
 * trial runs through server_world_checkpoint_chunks with a sampled
 * allocator state (test seam); growth is computed independently here
 * (mp_growth) and compared with the planner's trace, never equated with the
 * requested bytes. The planner's guard units, carve floor, evolving largest
 * hole, arena exact fit and sticky switch are checked directly.
 * Parts 2 (mp_fragmented_checkpoint256, journal) and 3 (mp_ordered_retry)
 * are not in this commit. */
static long mp_pass, mp_red;
static void mp_check(bool ok, const char* what) {
	if(ok) { mp_pass++; return; }
	mp_red++;
	printf("  G9X-5 RED: %s\n", what);
}
/* newlib malloc_extend_top: sbrk(max(16, round8(b + 4)) + 16), page-rounded */
static size_t mp_growth(size_t bytes) {
	size_t nb = (bytes + 4u + 7u) & ~(size_t)7u;
	if(nb < 16u) nb = 16u;
	return (nb + 16u + 4095u) / 4096u * 4096u;
}
#define MP_FLOOR ((size_t)SERVER_WORLD_MEM1_FLOOR)
/* mem2_alloc carve debit: round32(p + 256 guard) + 40 + an unsplit tail <= 40 */
#define MP_MEM2_COST(sz) ((((size_t)(sz) + 256u + 31u) / 32u) * 32u + 80u)
#define MP_CORR ((size_t)4095u) /* _malloc_r correction sbrk, masked to the page */
/* _malloc_r request padding: max(16, (b + 11) & ~7) */
static size_t mp_pad(size_t bytes) {
	size_t nb = (bytes + 4u + 7u) & ~(size_t)7u;
	return nb < 16u ? 16u : nb;
}
static struct server_world_mem_state mp_base(void) {
	return (struct server_world_mem_state) {
		.mem1_free = (size_t)64u << 20, .arena2_free = (size_t)64u << 20,
		.bins_free = 0, .top_free = 0, .arena = SERVER_WORLD_MEM_UNSWITCHED,
		.carve_valid = true, .carve_free = (size_t)32u << 20,
		.carve_largest = (size_t)16u << 20, .carve_reserve = 0};
}
static uint64_t mp_hash(struct server_world* w) {
	size_t qn = 0;
	uint64_t h = rb_queue_hash(w, &qn);
	h = dr_fnv(h, &qn, sizeof qn);
	const size_t counts[3] = {w->light_seed_n, w->light_undo_n, w->light_pin_n};
	h = dr_fnv(h, counts, sizeof counts);
	for(int cx = -8; cx < 8; cx++) for(int cz = -8; cz < 8; cz++) {
		struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
		if(!sc) continue;
		h = dr_fnv(h, sc->lighting_sky, 16 * 16 * 128 / 2);
		h = dr_fnv(h, sc->lighting_torch, 16 * 16 * 128 / 2);
	}
	return h;
}

static bool mp_plan(struct server_world_mem_state st, const struct server_world_mem_event* ev,
	size_t n, struct server_world_mem_plan* p) {
	return server_world_mem_plan_run(&st, ev, n, p);
}

/* the planner's own boundaries (guard units, carve debit/floor, largest
 * hole after each carve, arena exact fit, sticky switch, overflow) */
static void mp_planner(void) {
	struct server_world_mem_plan p;
	const struct server_world_mem_event page = {SERVER_WORLD_MEM_EV_MEM2_STRICT, 3584, 1};
	struct server_world_mem_state st = mp_base();
	st.carve_largest = 3840;
	mp_check(mp_plan(st, &page, 1, &p) && p.step[0].carve_debit == 3920
		&& p.step[0].largest_after == 0 && p.step[0].carve_after == ((size_t)32u << 20) - 3920,
		"planner: a 3,584 B page takes a hole whose largest request is exactly payload + 256 "
		"guard (3,840), debiting 3,880 + the 40 B unsplit tail");
	st.carve_largest = 3839;
	mp_check(!mp_plan(st, &page, 1, &p) && p.reason == SERVER_WORLD_MEM_CARVE_LARGEST,
		"planner: largest 3,839 < payload + guard refuses a strict page (guard counted once)");
	st = mp_base();
	st.carve_reserve = 1000;
	st.carve_free = SERVER_WORLD_CARVE_FLOOR + 1000 + 3920;
	mp_check(mp_plan(st, &page, 1, &p) && p.carve_min == SERVER_WORLD_CARVE_FLOOR + 1000,
		"planner: carve exactly floor + reserve after the page is admitted");
	st.carve_free--;
	mp_check(!mp_plan(st, &page, 1, &p) && p.reason == SERVER_WORLD_MEM_CARVE_FLOOR,
		"planner: one byte less breaches carve floor + admission reserve");
	st = mp_base();
	const struct server_world_mem_event pages2 = {SERVER_WORLD_MEM_EV_MEM2_STRICT, 3584, 2};
	const struct server_world_mem_event pages3 = {SERVER_WORLD_MEM_EV_MEM2_STRICT, 3584, 3};
	st.carve_largest = 7744;
	mp_check(mp_plan(st, &pages2, 1, &p) && p.step[0].largest_after == 3840
		&& !mp_plan(st, &pages3, 1, &p) && p.reason == SERVER_WORLD_MEM_CARVE_LARGEST
		&& p.steps == 2,
		"planner: the largest hole is lowered after EACH carve (two pages fit 7,744, a third does not)");
	st.carve_largest = 7743;
	mp_check(!mp_plan(st, &pages2, 1, &p) && p.reason == SERVER_WORLD_MEM_CARVE_LARGEST
		&& p.steps == 1, "planner: 7,743 admits one page, not two");
	st = mp_base();
	st.carve_largest = 0;
	const struct server_world_mem_event soft = {SERVER_WORLD_MEM_EV_MEM2, 3584, 1};
	mp_check(mp_plan(st, &soft, 1, &p) && p.step[0].where == SERVER_WORLD_MEM_AT_EITHER
		&& p.step[0].growth == mp_growth(mp_pad(3584 + 256) + 32 + 16)
		&& p.step[0].carve_debit == 3920,
		"planner: a soft page above the largest-hole ESTIMATE is charged both ways: the carve "
		"debit (another hole may take it) and memalign(32, 3,840) = malloc(3,848 + 48) growth");
	/* Sol P1-2: two 3,880 B holes (largest request 3,840 each), carve free =
	 * floor + 3,920, two soft pages: the heap carves both (floor - 3,920). */
	st = mp_base();
	st.carve_largest = mem2_lwp_allocatable_from_block(3880, 32, 8);
	st.carve_free = SERVER_WORLD_CARVE_FLOOR + 3920;
	const struct server_world_mem_event soft2 = {SERVER_WORLD_MEM_EV_MEM2, 3584, 2};
	mp_check(st.carve_largest == 3840 && !mp_plan(st, &soft2, 1, &p)
		&& p.reason == SERVER_WORLD_MEM_CARVE_FLOOR && p.steps == 1,
		"planner: a lowered largest-hole estimate does not prove the second page misses the "
		"carve: two equal holes, the second carve would breach the carve floor -> refused");
	st.carve_valid = false;
	mp_check(!mp_plan(st, &soft, 1, &p) && p.reason == SERVER_WORLD_MEM_CARVE_UNKNOWN,
		"planner: unknown carve topology refuses, never assumes contiguity");
	const struct server_world_mem_event big = {SERVER_WORLD_MEM_EV_MALLOC, 100000, 1};
	st = mp_base();
	st.mem1_free = MP_FLOOR + mp_growth(100000);
	mp_check(mp_plan(st, &big, 1, &p) && p.mem1_min == MP_FLOOR
		&& p.step[0].growth == mp_growth(100000) && p.step[0].requested == 100000,
		"planner: MEM1 exact fit leaves exactly the floor (growth 102,400 != requested 100,000)");
	st.mem1_free--;
	mp_check(!mp_plan(st, &big, 1, &p) && p.reason == SERVER_WORLD_MEM_MEM1_FLOOR,
		"planner: one byte less, the increment still fits MEM1 and breaches the floor");
	st = mp_base();
	st.mem1_free = 1000;
	st.arena2_free = mp_growth(100000) + MP_CORR;
	mp_check(mp_plan(st, &big, 1, &p) && p.step[0].where == SERVER_WORLD_MEM_AT_ARENA2
		&& p.step[0].growth == mp_growth(100000) + MP_CORR,
		"planner: an increment above MEM1 goes WHOLE to Arena2 (+ correction), exact fit");
	st.arena2_free--;
	mp_check(!mp_plan(st, &big, 1, &p) && p.reason == SERVER_WORLD_MEM_ARENA2_FULL,
		"planner: Arena2 one byte short refuses");
	const struct server_world_mem_event seq[2] = {
		{SERVER_WORLD_MEM_EV_MALLOC, (size_t)8u << 20, 1}, {SERVER_WORLD_MEM_EV_MALLOC, 1000, 1}};
	st = mp_base();
	st.mem1_free = MP_FLOOR + 4095;
	mp_check(mp_plan(st, seq, 2, &p) && p.step[1].where == SERVER_WORLD_MEM_AT_ARENA2
		&& p.mem1_min == MP_FLOOR + 4095,
		"planner: after a certain switch, a later small increment stays in Arena2 (sticky)");
	st.bins_free = (size_t)9u << 20; /* a free chunk >= the 8 MiB request */
	mp_check(!mp_plan(st, seq, 2, &p) && p.reason == SERVER_WORLD_MEM_MEM1_FLOOR && p.fail_event == 1,
		"planner: when a free chunk could have served the big request (no switch), the "
		"later small increment is charged to MEM1 and refused");
	st = mp_base();
	st.arena = SERVER_WORLD_MEM_SWITCHED;
	st.mem1_free = MP_FLOOR;
	st.bins_free = (size_t)8u << 20; /* reusable MEM1 chunks after the switch */
	mp_check(mp_plan(st, seq, 2, &p) && p.mem1_min == MP_FLOOR
		&& p.step[0].where == SERVER_WORLD_MEM_AT_ARENA2 && p.step[1].where == SERVER_WORLD_MEM_AT_ARENA2,
		"planner: already switched: nothing grows MEM1 (reuse lowers no arena)");
	/* Sol P1-1: unswitched at floor + 4,096; 1,000 B then 4,194,304 B. If the
	 * first reuses a free chunk, the second fits MEM1 exactly, leaving 0. */
	const struct server_world_mem_event reuse_seq[2] = {
		{SERVER_WORLD_MEM_EV_MALLOC, 1000, 1}, {SERVER_WORLD_MEM_EV_MALLOC, 4194304, 1}};
	st = mp_base();
	st.mem1_free = MP_FLOOR + 4096;
	st.bins_free = 2048; /* a chunk for the 1,000 B request, none for the big one */
	mp_check(mp_growth(4194304) == MP_FLOOR + 4096 && !mp_plan(st, reuse_seq, 2, &p)
		&& p.reason == SERVER_WORLD_MEM_MEM1_FLOOR && p.fail_event == 1,
		"planner: a possible reuse keeps the higher MEM1 balance credible: the 4 MiB increment "
		"may land in MEM1 exactly (0 left) -> refused, not sent to Arena2");
	st.bins_free = 0;
	mp_check(mp_plan(st, reuse_seq, 2, &p) && p.step[1].where == SERVER_WORLD_MEM_AT_ARENA2
		&& p.mem1_min == MP_FLOOR,
		"planner: without a reusable chunk the 1,000 B request certainly grows, so the 4 MiB "
		"increment certainly switches to Arena2");
	const struct server_world_mem_event huge = {SERVER_WORLD_MEM_EV_MALLOC, SIZE_MAX - 8, 1};
	mp_check(!mp_plan(mp_base(), &huge, 1, &p) && p.reason == SERVER_WORLD_MEM_OVERFLOW,
		"planner: size overflow refuses");
}

/* A refused checkpoint whose HEAD allocations really do breach the floor:
 * sample 0 (plan) = pre, sample 1 (right after HEAD's allocations) = MEM1
 * one byte under min(pre, floor), then pre again (after freeing them; the
 * journal reservation's own samples). */
static void mp_model_refused(const struct server_world_mem_state* pre) {
	struct server_world_mem_state m[4] = {*pre, *pre, *pre, *pre};
	const size_t low = pre->mem1_free < MP_FLOOR ? pre->mem1_free : MP_FLOOR;
	m[1].mem1_free = low ? low - 1 : 0;
	server_world_test_mem_model(m, 4);
}

static void mp_increment_arena(void) {
	open_world(&S[0], s_dir[0], sizeof s_dir[0], "test_light_queue_mp_arena", true, 0);
	mode(1);
	struct server_world* w = &S[0].worlds[0];
	for(int cx = -8; cx < 8; cx++) for(int cz = -8; cz < 8; cz++) {
		struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
		if(!sc && (!server_world_load_chunk(w, cx, cz, &sc) || !sc)) {
			printf("  load %d,%d failed at %zu resident\n", cx, cz, dict_server_chunks_size(w->chunks));
			assert(0);
		}
	}
	assert(server_world_light_drain(w));
	const size_t want = 17000; /* > 16,384: a full resident window + backing */
	assert(rb_fill(w, want, 0) == want);
	const size_t cols = dict_server_chunks_size(w->chunks);
	const size_t count = w->generation_light_n - w->generation_light_base * 128u;
	const bool backing = server_world_test_light_queue_backing_path(w)[0] != 0;
	printf("  mp_increment_arena: %zu resident columns, queue %zu (resident window %zu, backing %d)\n",
		cols, w->generation_light_n, count, backing);
	assert(cols == 256 && backing && count >= 16384u - 128u);
	const size_t req[5] = {cols * 8, count * 28 + 1, 977, cols * 32768, cols * 68};
	size_t total = 0;
	for(unsigned k = 0; k < 5; k++) total += req[k];
	const size_t x = mp_growth(req[0]) + mp_growth(req[1]) + mp_growth(req[2]);
	printf("  requested IDs %zu window %zu bitmap %zu planes %zu captures %zu (total %zu); "
		"MEM1 growth before the planes %zu\n", req[0], req[1], req[2], req[3], req[4], total, x);
	/* the rejected aggregate model: one increment of `total` > MEM1 headroom
	 * goes whole to Arena2, so it admits */
	mp_check(total > (size_t)65536 && x > (size_t)65536,
		"the aggregate model would admit (8.8 MB > 64 KiB headroom -> Arena2) while the "
		"small increments alone exceed the headroom");
	struct server_world_light_stats ls;
	struct server_world_test_mem_report rep;
	struct server_world_light_queue_stats qs;
	const uint64_t h0 = mp_hash(w);

	/* part 1 lands the preflight OFF: the production trial is unchanged */
	struct server_world_mem_state st = mp_base();
	st.mem1_free = MP_FLOOR + 65536;
	server_world_test_mem_preflight(false);
	server_world_test_mem_model(&st, 1);
	server_world_test_light_stats_reset();
	bool ok = server_world_checkpoint_chunks(w);
	server_world_test_mem_last(&rep);
	server_world_light_stats(&ls);
	server_world_test_light_queue_stats(w, &qs);
	mp_check(ok && ls.checkpoint_trials == 1 && ls.trial_alloc_fail == 0 && rep.refusals == 0
		&& rep.plan.steps == 0 && rep.captures_skipped == 0 && qs.ckpt_bytes == 0
		&& mp_hash(w) == h0,
		"mp_increment_arena: preflight switch OFF (test seam; production is ON): the same "
		"pressured state runs the unchanged trial, no sample, no plan, no refusal");
	/* the journal cannot be reserved: never worse than HEAD -- the refused
	 * trial runs HEAD's RAM path, ungated, exactly as the switch-off build */
	server_world_test_mem_preflight(true);
	mp_model_refused(&st);
	server_world_test_journal_fault(SERVER_WORLD_TEST_JOURNAL_RESERVE, 0);
	server_world_test_light_stats_reset();
	ok = server_world_checkpoint_chunks(w);
	server_world_test_journal_fault(SERVER_WORLD_TEST_JOURNAL_RESERVE, -1);
	server_world_test_mem_last(&rep);
	server_world_light_stats(&ls);
	for(size_t k = 0; k < rep.plan.steps && k < SERVER_WORLD_MEM_TRACE_MAX; k++)
		printf("    step %zu: event %u requested %zu growth %zu at %u -> MEM1 %zu\n", k,
			rep.plan.step[k].event, rep.plan.step[k].requested, rep.plan.step[k].growth,
			rep.plan.step[k].where, rep.plan.step[k].mem1_after);
	printf("    floor+64KiB unswitched, no journal: checkpoint %d mode %d, trials %u, refusals %zu "
		"reason %d at event %zu, head fallbacks %zu\n", ok, rep.mode, ls.checkpoint_trials,
		rep.refusals, rep.plan.reason, rep.plan.fail_event, rep.head_fallbacks);
	mp_check(rep.refusals == 1 && rep.journal_reserve_fail == 1
		&& rep.plan.reason == SERVER_WORLD_MEM_MEM1_FLOOR
		&& rep.plan.fail_event == 1 && rep.plan.steps == 1
		&& rep.plan.step[0].requested == req[0] && rep.plan.step[0].growth == mp_growth(req[0])
		&& rep.plan.step[0].where == SERVER_WORLD_MEM_AT_MEM1,
		"mp_increment_arena: floor + 64 KiB, unswitched: the IDs increment lands in MEM1 and the "
		"cursor-window increment would breach the floor -> the full copy is refused before it");
	mp_check(ok && rep.head_fallbacks == 1 && rep.mode == SERVER_WORLD_CKPT_FULL
		&& ls.checkpoint_trials == 1 && mp_hash(w) == h0,
		"mp_increment_arena: journal unavailable -> HEAD's RAM path runs exactly as the switch-off "
		"build (checkpoint succeeds, restored), never a new refusal");

	/* Sol P1 (extra round): the planner refuses (its model cannot see
	 * reusable newlib chunks), but HEAD's real allocations succeed and a
	 * fresh sample right after them holds the floors: the RAM path is kept,
	 * no journal, the same result as the switch-off build */
	server_world_test_mem_model(&st, 1);
	server_world_test_light_stats_reset();
	ok = server_world_checkpoint_chunks(w);
	server_world_test_mem_last(&rep);
	server_world_light_stats(&ls);
	mp_check(ok && rep.refusals == 1 && rep.ram_after_refusal == 1 && rep.ram_floor_breach == 0
		&& rep.mode == SERVER_WORLD_CKPT_FULL && rep.journal_trials == 0 && rep.journal_written == 0
		&& server_world_test_journal_path(w)[0] == 0 && ls.checkpoint_trials == 1 && mp_hash(w) == h0,
		"mp_increment_arena: planner refusal is not proof HEAD fails: HEAD's own allocations run "
		"first, succeed within the floors (fresh sample) -> RAM path kept, no journal, no SD I/O, "
		"result identical to the switch-off build");

	/* the same state with the journal: the planner's refusal becomes progress */
	mp_model_refused(&st);
	server_world_test_light_stats_reset();
	ok = server_world_checkpoint_chunks(w);
	server_world_test_mem_last(&rep);
	server_world_light_stats(&ls);
	server_world_test_light_queue_stats(w, &qs);
	printf("    floor+64KiB unswitched, journal: checkpoint %d mode %d columns %zu written %llu read %llu "
		"samples %zu (%zu us)\n", ok, rep.mode, rep.journal_columns,
		(unsigned long long)rep.journal_written, (unsigned long long)rep.journal_read,
		rep.samples, rep.sample_us);
	mp_check(ok && rep.refusals == 1 && rep.plan.reason == SERVER_WORLD_MEM_MEM1_FLOOR
		&& rep.plan.fail_event == 1 && rep.mode == SERVER_WORLD_CKPT_JOURNAL
		&& rep.journal_columns == cols && ls.checkpoint_trials == 1 && qs.ckpt_bytes == 0
		&& rep.journal_written >= (uint64_t)cols * 32800u + count * 28u
		&& rep.journal_read >= 2u * rep.journal_written,
		"mp_increment_arena: the refused full copy takes the journal: all 256 columns and the "
		"resident cursor window go to disk, are read back and verified BEFORE the drain, no "
		"cursor/plane malloc, checkpoint succeeds");
	mp_check(mp_hash(w) == h0 && rep.samples == 4,
		"mp_increment_arena: the journal trial restored live light/queue exactly; four samples "
		"(plan, after HEAD's allocations, after freeing them, after reserving the journal: once "
		"per world)");
	mp_check(server_world_test_journal_path(w)[0] != 0 && rep.journal_reserve_fail == 0
		&& rep.journal_reserve_refused == 0,
		"mp_increment_arena: the journal is reserved through the planner (block + FILE) and kept");

	st.mem1_free = MP_FLOOR + x - 1;
	mp_model_refused(&st);
	ok = server_world_checkpoint_chunks(w);
	server_world_test_mem_last(&rep);
	mp_check(ok && rep.plan.reason == SERVER_WORLD_MEM_MEM1_FLOOR && rep.plan.fail_event == 2
		&& rep.mode == SERVER_WORLD_CKPT_JOURNAL && rep.samples == 3 && mp_hash(w) == h0,
		"mp_increment_arena: floor + growth(IDs, window, bitmap) - 1: the bitmap increment "
		"breaches; HEAD's real allocations then breach the floor -> freed -> journal (three "
		"samples), exact restore");
	st.arena = SERVER_WORLD_MEM_UNKNOWN;
	mp_model_refused(&st);
	ok = server_world_checkpoint_chunks(w);
	server_world_test_mem_last(&rep);
	mp_check(ok && rep.plan.reason == SERVER_WORLD_MEM_MEM1_FLOOR && rep.plan.fail_event == 2
		&& rep.mode == SERVER_WORLD_CKPT_JOURNAL,
		"mp_increment_arena: unknown switch state is refused like unswitched -> journal");

	/* a stale sample at the pre-drain revalidation */
	struct server_world_mem_state two[2] = {mp_base(), mp_base()};
	two[0].mem1_free = MP_FLOOR + x;
	two[1].mem1_free = MP_FLOOR - 1;
	server_world_test_mem_model(two, 2);
	server_world_test_light_stats_reset();
	ok = server_world_checkpoint_chunks(w);
	server_world_test_mem_last(&rep);
	server_world_light_stats(&ls);
	server_world_test_light_queue_stats(w, &qs);
	mp_check(ok && rep.revalidation_refusals == 1 && rep.plan.reason == SERVER_WORLD_MEM_STALE
		&& rep.mode == SERVER_WORLD_CKPT_JOURNAL && ls.checkpoint_trials == 1
		&& qs.ckpt_bytes == 0 && mp_hash(w) == h0 && rep.samples == 2,
		"mp_increment_arena: a stale plan (MEM1 below floor at revalidation) unwinds every "
		"full-copy allocation before the drain, then the journal completes the checkpoint");

	st = mp_base();
	st.mem1_free = MP_FLOOR + x;
	server_world_test_mem_model(&st, 1);
	server_world_test_light_stats_reset();
	ok = server_world_checkpoint_chunks(w);
	server_world_test_mem_last(&rep);
	server_world_light_stats(&ls);
	const struct server_world_mem_step* s3 = &rep.plan.step[3];
	const struct server_world_mem_step* s4 = &rep.plan.step[4];
	printf("    floor+%zu unswitched: checkpoint %d trials %u, MEM1 min %zu, planes growth %zu at %u, "
		"captures at %u\n", x, ok, ls.checkpoint_trials, rep.plan.mem1_min, s3->growth, s3->where, s4->where);
	mp_check(ok && ls.checkpoint_trials == 1 && rep.refusals == 0 && rep.plan.ok
		&& rep.mode == SERVER_WORLD_CKPT_FULL && rep.samples == 2
		&& rep.plan.mem1_min == MP_FLOOR
		&& s3->requested == req[3] && s3->growth == mp_growth(req[3]) + MP_CORR
		&& s3->where == SERVER_WORLD_MEM_AT_ARENA2
		&& s4->requested == req[4] && s4->where == SERVER_WORLD_MEM_AT_ARENA2,
		"mp_increment_arena: exact fit admits the full copy; the planes switch WHOLE to Arena2 and "
		"the later captures increment stays there (sticky), MEM1 never below the floor");
	mp_check(mp_hash(w) == h0, "mp_increment_arena: the admitted trial rolled back exactly");

	st = mp_base();
	st.arena = SERVER_WORLD_MEM_SWITCHED;
	st.mem1_free = MP_FLOOR;
	st.bins_free = (size_t)8u << 20;
	server_world_test_mem_model(&st, 1);
	server_world_test_light_stats_reset();
	ok = server_world_checkpoint_chunks(w);
	server_world_test_mem_last(&rep);
	server_world_light_stats(&ls);
	bool all2 = rep.plan.steps >= 5;
	for(size_t k = 0; k < rep.plan.steps && k < SERVER_WORLD_MEM_TRACE_MAX; k++)
		all2 = all2 && rep.plan.step[k].where == SERVER_WORLD_MEM_AT_ARENA2;
	mp_check(ok && ls.checkpoint_trials == 1 && all2 && rep.plan.mem1_min == MP_FLOOR
		&& rep.mode == SERVER_WORLD_CKPT_FULL && mp_hash(w) == h0,
		"mp_increment_arena: already switched (reusable MEM1 chunks): every increment in Arena2, admitted");
	/* pending gameplay light: planned with its frontier allowance; a full
	 * copy that is admitted stays HEAD's RAM path (no forced journal) */
	uint8_t hh = 0;
	assert(server_world_get_height(w, 4, 4, &hh));
	assert(server_world_set_block(w, 4, hh, 4, (struct block_data) {.type = BLOCK_TORCH}));
	const size_t seeds = w->light_seed_n;
	const uint64_t h1 = mp_hash(w);
	st = mp_base();
	server_world_test_mem_model(&st, 1);
	server_world_test_light_stats_reset();
	ok = server_world_checkpoint_chunks(w);
	server_world_test_mem_last(&rep);
	server_world_light_stats(&ls);
	const size_t el = w->light_frontier.element_size;
	size_t flen = w->light_frontier.length && w->light_frontier.data ? w->light_frontier.length : 16;
	while(flen < seeds + 5) flen *= 2;
	bool allowance = false;
	for(size_t k = 0; k < rep.plan.steps && k < SERVER_WORLD_MEM_TRACE_MAX; k++)
		allowance = allowance || rep.plan.step[k].requested == 2 * flen * el;
	mp_check(seeds > 0 && ok && rep.refusals == 0 && rep.mode == SERVER_WORLD_CKPT_FULL
		&& allowance && ls.checkpoint_trials == 1 && w->light_seed_n == seeds && mp_hash(w) == h1,
		"mp_increment_arena: pending gameplay light with an admitted full copy (frontier allowance "
		"= one doubling past the seeds, planned) runs HEAD's RAM path, restored exactly");
	/* Sol P1 (point 1, revalidation branch): the plan admits; at the
	 * pre-drain revalidation the fresh sample's floors are healthy (MEM1
	 * floor + 100 B) but the forecast of what the drain may still allocate
	 * (the frontier allowance) would not fit -- a forecast, not proof HEAD
	 * fails: the RAM snapshot is kept, no journal, same as switch-off */
	{
		struct server_world_mem_state fs[2] = {mp_base(), mp_base()};
		fs[1].mem1_free = MP_FLOOR + 100;
		struct server_world_mem_plan fp;
		const struct server_world_mem_event grow = {SERVER_WORLD_MEM_EV_MALLOC, 2 * flen * el, 1};
		const bool forecast_refuses = !server_world_mem_plan_run(&fs[1], &grow, 1, &fp);
		server_world_test_mem_model(fs, 2);
		server_world_test_light_stats_reset();
		ok = server_world_checkpoint_chunks(w);
		server_world_test_mem_last(&rep);
		server_world_light_stats(&ls);
		mp_check(forecast_refuses && ok && rep.revalidation_forecast_kept == 1
			&& rep.revalidation_refusals == 0 && rep.mode == SERVER_WORLD_CKPT_FULL
			&& rep.journal_trials == 0 && rep.journal_written == 0 && ls.checkpoint_trials == 1
			&& w->light_seed_n == seeds && mp_hash(w) == h1,
			"mp_increment_arena: revalidation forecast refuses while the sampled floors are healthy "
			"and every RAM allocation succeeded -> RAM snapshot kept, no journal, same as switch-off");
	}
	st.mem1_free = MP_FLOOR + 65536;
	mp_model_refused(&st);
	ok = server_world_checkpoint_chunks(w);
	server_world_test_mem_last(&rep);
	mp_check(ok && rep.refusals == 1 && rep.mode == SERVER_WORLD_CKPT_JOURNAL
		&& w->light_seed_n == seeds && mp_hash(w) == h1,
		"mp_increment_arena: pending gameplay light whose full copy is refused takes the journal");
	server_world_test_mem_preflight(true);
	server_world_test_mem_model(NULL, 0);
	close_world(&S[0], s_dir[0], false);
}

/* ---- part 2: mp_fragmented_checkpoint256 ----------------------------------
 * Two identical 256-column worlds, each with a full resident queue window +
 * backing (both worlds' queue footprints live at once). The journal of
 * world 0 is reserved before fragmentation (a refused checkpoint in an
 * unfragmented state). Then a torch flood is pending in both; session 1
 * checkpoints with the unrestricted full copy (the reference), session 0
 * under a fragmented carve (total free plentiful, page-sized holes only),
 * transfer reservations the column pool does not cover, MEM1 floor + 64 KiB
 * and the recorded Arena2 1,371 KiB: the monolithic copy cannot be admitted,
 * the pre-reserved journal carries all 256 columns. Both live worlds must be
 * restored exactly and their saved settled images reload byte-identically.
 * A fresh world in the same fragmented state cannot reserve a journal (its
 * strict block does not fit a page-sized hole) and runs HEAD's path. */
static void mp_torch(struct server_world* w) {
	uint8_t hh = 0;
	assert(server_world_get_height(w, 20, 20, &hh));
	assert(server_world_set_block(w, 20, hh, 20, (struct block_data) {.type = BLOCK_TORCH}));
	assert(w->light_seed_n > 0);
}
static void mp_world256(int i, const char* id, bool torch) {
	open_world(&S[i], s_dir[i], sizeof s_dir[i], id, true, 0);
	struct server_world* w = &S[i].worlds[0];
	for(int cx = -8; cx < 8; cx++) for(int cz = -8; cz < 8; cz++) {
		struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
		if(!sc) assert(server_world_load_chunk(w, cx, cz, &sc) && sc);
	}
	assert(server_world_light_drain(w));
	assert(rb_fill(w, 17000, 0) == 17000);
	if(torch) mp_torch(w);
	assert(dict_server_chunks_size(w->chunks) == 256);
}
/* reload every column from disk into a fresh world on the same level */
static uint64_t mp_reload_hash(int i, int tx, int ty, int tz, int* torch_light) {
	struct server_world* w = &S[i].worlds[0];
	server_world_discard(w);
	string_t level;
	string_init_set_str(level, s_dir[i]);
	server_world_create(w, level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	w->session = &S[i];
	w->local = &S[i];
	uint64_t h = 1469598103934665603ull;
	for(int cx = -8; cx < 8; cx++) for(int cz = -8; cz < 8; cz++) {
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(w, cx, cz, &sc) && sc);
		h = dr_fnv(h, sc->ids, 16 * 16 * 128);
		h = dr_fnv(h, sc->metadata, 16 * 16 * 128 / 2);
		h = dr_fnv(h, sc->lighting_sky, 16 * 16 * 128 / 2);
		h = dr_fnv(h, sc->lighting_torch, 16 * 16 * 128 / 2);
	}
	struct block_data b = {0};
	*torch_light = server_world_get_block(w, tx, ty, tz, &b) ? b.torch_light : -1;
	return h;
}
static struct server_world_mem_state mp_fragmented_state(void) {
	struct server_world_mem_state st = mp_base();
	st.mem1_free = MP_FLOOR + 65536;
	st.arena2_free = (size_t)1371u * 1024u; /* recorded Arena2 */
	st.carve_free = (size_t)24u << 20;
	st.carve_largest = mem2_lwp_allocatable_from_block(3880, 32, 8); /* page-sized holes only */
	st.carve_reserve = server_world_mem_column_reserve(9, 4, 0, 0);
	return st;
}
static void mp_fragmented_checkpoint256(void) {
	mode(1);
	mp_world256(0, "test_light_queue_mp_frag_j", false);
	mp_world256(1, "test_light_queue_mp_frag_r", false);
	struct server_world* wj = &S[0].worlds[0];
	struct server_world* wr = &S[1].worlds[0];
	struct server_world_test_mem_report rep;
	struct server_world_light_stats ls;
	const struct server_world_mem_state st = mp_fragmented_state();
	mp_check(st.carve_largest >= 3584u + 256u && st.carve_largest < 65804u + 256u
		&& st.carve_largest < 4848u + 256u
		&& st.carve_reserve == 5u * (size_t)MP_MEM2_COST(MEM2_POOL_COLUMN_OBJECT),
		"mp_fragmented_checkpoint256: holes take a guarded page but not one population snapshot "
		"nor the journal block; 5 of 9 transfer reservations need an unpooled column object each");
	/* generation work only, unfragmented carve: the planner refuses the
	 * 8 MiB copy, the journal is reserved (before fragmentation) and used */
	struct server_world_mem_state pre = st;
	pre.carve_largest = (size_t)16u << 20;
	server_world_test_mem_preflight(true);
	mp_model_refused(&pre);
	const uint64_t hq = mp_hash(wj);
	const bool ok_q = server_world_checkpoint_chunks(wj);
	server_world_test_mem_last(&rep);
	printf("  mp_fragmented_checkpoint256: queue-only checkpoint ok %d mode %d reason %d at event %zu\n",
		ok_q, rep.mode, rep.plan.reason, rep.plan.fail_event);
	mp_check(ok_q && rep.mode == SERVER_WORLD_CKPT_JOURNAL && rep.journal_columns == 256
		&& (rep.plan.reason == SERVER_WORLD_MEM_MEM1_FLOOR || rep.plan.reason == SERVER_WORLD_MEM_ARENA2_FULL)
		&& mp_hash(wj) == hq,
		"mp_fragmented_checkpoint256: generation-only trial: the planner refuses the 8 MiB copy "
		"(MEM1 floor / Arena2 1,371 KiB), the journal carries all 256 columns, exact restore");
	/* now pending gameplay light too: the settled image differs from live */
	mp_torch(wj);
	mp_torch(wr);
	uint8_t hh = 0;
	assert(server_world_get_height(wj, 20, 20, &hh));
	struct block_data b0 = {0};
	assert(server_world_get_block(wj, 21, hh, 20, &b0));
	const int live_torch = b0.torch_light;
	const uint64_t hj0 = mp_hash(wj), hr0 = mp_hash(wr);
	mp_check(hj0 == hr0, "mp_fragmented_checkpoint256: the two fixtures are identical before the checkpoint");
	/* reference: the unrestricted full copy */
	server_world_test_mem_preflight(false);
	server_world_test_mem_model(NULL, 0);
	const bool ok_r = server_world_checkpoint_chunks(wr) && server_world_sync_regions(wr);
	mp_check(ok_r && mp_hash(wr) == hr0, "mp_fragmented_checkpoint256: reference full copy ok, restored");
	server_world_test_mem_preflight(true);
	mp_model_refused(&st);
	server_world_test_light_stats_reset();
	const bool ok_j = server_world_checkpoint_chunks(wj) && server_world_sync_regions(wj);
	server_world_test_mem_last(&rep);
	server_world_light_stats(&ls);
	printf("  mp_fragmented_checkpoint256: journal ok %d mode %d columns %zu refusal reason %d "
		"written %llu read %llu samples %zu (%zu us)\n", ok_j, rep.mode, rep.journal_columns,
		rep.plan.reason, (unsigned long long)rep.journal_written,
		(unsigned long long)rep.journal_read, rep.samples, rep.sample_us);
	mp_check(ok_j && rep.mode == SERVER_WORLD_CKPT_JOURNAL && rep.journal_columns == 256
		&& rep.refusals == 1 && ls.checkpoint_trials == 1 && rep.samples == 3,
		"mp_fragmented_checkpoint256: under fragmentation the monolithic copy is refused and the "
		"pre-reserved journal carries all 256 columns to a successful checkpoint (three samples)");
	mp_check(mp_hash(wj) == hj0 && !server_world_light_halted(wj),
		"mp_fragmented_checkpoint256: live light, counts and queue restored exactly from the journal");
	/* a repeated checkpoint takes a new journal version */
	const uint32_t seq1 = rep.journal_sequence;
	mp_model_refused(&st);
	const bool ok_j2 = server_world_checkpoint_chunks(wj);
	server_world_test_mem_last(&rep);
	mp_check(ok_j2 && rep.journal_sequence == seq1 + 1 && mp_hash(wj) == hj0,
		"mp_fragmented_checkpoint256: a repeated checkpoint uses a new journal version, exact again");
	mp_check(wj->generation_light_n == wr->generation_light_n
		&& wj->light_seed_n == wr->light_seed_n,
		"mp_fragmented_checkpoint256: both worlds keep identical pending work");
	server_world_test_mem_model(NULL, 0);
	/* the settled images on disk: reload both, compare byte for byte */
	int tj = -1, tr = -1;
	char jpath[300];
	snprintf(jpath, sizeof jpath, "%s", server_world_test_journal_path(wj));
	const uint64_t dj = mp_reload_hash(0, 21, hh, 20, &tj);
	FILE* gone = fopen(jpath, "rb");
	const uint64_t dr = mp_reload_hash(1, 21, hh, 20, &tr);
	printf("  mp_fragmented_checkpoint256: reload journal %016llx reference %016llx, torch-side light "
		"live %d saved %d/%d\n", (unsigned long long)dj, (unsigned long long)dr, live_torch, tj, tr);
	mp_check(dj == dr && tj == tr && tj > live_torch,
		"mp_fragmented_checkpoint256: the journal checkpoint saved the SETTLED image (torch flood "
		"included), byte-identical to the unrestricted reference after reload");
	mp_check(jpath[0] && !gone, "mp_fragmented_checkpoint256: teardown removed the owned journal file");
	if(gone) fclose(gone);
	close_world(&S[0], s_dir[0], false);
	close_world(&S[1], s_dir[1], false);
	/* a fresh world, already fragmented: no journal block fits -> HEAD */
	mp_world256(0, "test_light_queue_mp_frag_fresh", true);
	struct server_world* wf = &S[0].worlds[0];
	const uint64_t hf = mp_hash(wf);
	mp_model_refused(&st);
	const bool ok_f = server_world_checkpoint_chunks(wf);
	server_world_test_mem_last(&rep);
	mp_check(ok_f && rep.journal_reserve_refused == 1 && rep.head_fallbacks == 1
		&& rep.mode == SERVER_WORLD_CKPT_FULL && mp_hash(wf) == hf
		&& server_world_test_journal_path(wf)[0] == 0,
		"mp_fragmented_checkpoint256: journal admission refuses a strict block no hole fits; the "
		"checkpoint runs HEAD's RAM path and succeeds (never worse than HEAD)");
	server_world_test_mem_model(NULL, 0);
	server_world_test_mem_preflight(true);
	close_world(&S[0], s_dir[0], false);
}

/* ---- part 2: recorded real-Wii memory states ------------------------------
 * Every checkpoint consumer reaches the trial through
 * server_world_checkpoint_chunks (portal destination server_transfer.c:2216,
 * world retirement server_transfer.c:379), server_world_save_chunk_coherent
 * (death-drop journaling server_local.c:12766) or
 * server_world_checkpoint_submit (region checkpoint tokens). The switch-off
 * build (HEAD) succeeds for all of them on the PC; with the preflight ON each
 * must still succeed in every recorded state -- single-player MEM1 7.4 /
 * 9.2 MiB with Arena2 1,371 KiB, split MEM1 3,312 KiB with carve 66 /
 * 1,411 KiB, key-OFF MEM1 4 KiB / carve 342 KiB (docs/p2_storage_ruling_
 * fable_2026-09-09.md:254-262) -- and under every journal-setup failure
 * (reservation, open, write, flush, read-back verify). Live state exact. */
static void mp_model_refused(const struct server_world_mem_state* pre);
static bool mp_consumers(struct server_world* w, const struct server_world_mem_state* st,
	uint64_t h0, int* mode_out, bool refused) {
	struct server_world_test_mem_report rep;
	if(refused) mp_model_refused(st); else server_world_test_mem_model(st, 1);
	const bool a = server_world_checkpoint_chunks(w) && server_world_sync_regions(w);
	server_world_test_mem_last(&rep);
	*mode_out = rep.mode;
	const bool ha = mp_hash(w) == h0;
	if(refused) mp_model_refused(st); else server_world_test_mem_model(st, 1);
	const bool b = server_world_save_chunk_coherent(w, 1, 1);
	const bool hb = mp_hash(w) == h0;
	if(refused) mp_model_refused(st); else server_world_test_mem_model(st, 1);
	struct server_checkpoint_token* tok = NULL;
	const enum region_checkpoint_state cs = server_world_checkpoint_submit(w, 2, 2, 1, &tok);
	bool c = cs != REGION_CHECKPOINT_REJECTED;
	if(!c) {
		/* the caller retries a rejected capture; the retry runs
		 * unrestricted so the token completes before teardown */
		server_world_test_mem_preflight(false);
		for(unsigned r = 0; r < 4 && server_world_checkpoint_submit(w, 2, 2, 1, &tok)
			== REGION_CHECKPOINT_REJECTED; r++) {}
		server_world_test_mem_preflight(true);
	}
	c = server_world_checkpoint_drain(w) && c;
	const bool hc = mp_hash(w) == h0;
	server_world_test_mem_model(NULL, 0);
	printf("    chunks %d (mode %d) coherent %d submit %d (state %d)\n", a, *mode_out, b, c, (int)cs);
	return a && b && c && ha && hb && hc && !server_world_light_halted(w);
}
static void mp_recorded_states(void) {
	struct server_world_mem_state rs[5];
	const size_t k = 1024u;
	const size_t m1[5] = {(size_t)(7.4 * 1048576.0), (size_t)(9.2 * 1048576.0), 3312 * k, 3312 * k, 4 * k};
	const size_t a2[5] = {1371 * k, 1371 * k, 3931 * k, 3931 * k, 1579 * k};
	const size_t cf[5] = {2246 * k + 512 * k, 2246 * k + 512 * k, 66 * k, 1411 * k, 342 * k};
	for(unsigned i = 0; i < 5; i++) {
		rs[i] = mp_base();
		rs[i].mem1_free = m1[i];
		rs[i].arena2_free = a2[i];
		rs[i].bins_free = rs[i].top_free = SIZE_MAX; /* the Wii sampler: unknown */
		rs[i].arena = SERVER_WORLD_MEM_UNKNOWN;
		rs[i].carve_free = cf[i];
		rs[i].carve_largest = cf[i] / 4;
		rs[i].carve_reserve = server_world_mem_column_reserve(9, 0, 0, 0);
	}
	mode(1);
	server_world_test_mem_preflight(true);
	for(unsigned i = 0; i < 5; i++) {
		/* generation-scale: 256 columns, full queue, pending torch */
		mp_world256(0, "test_light_queue_mp_recorded", true);
		struct server_world* w = &S[0].worlds[0];
		int md = 0;
		printf("  recorded state %u (MEM1 %zu KiB, Arena2 %zu KiB, carve %zu KiB):\n", i,
			m1[i] / k, a2[i] / k, cf[i] / k);
		const bool ok = mp_consumers(w, &rs[i], mp_hash(w), &md, false);
		char what[200];
		snprintf(what, sizeof what, "recorded state %u: portal/retirement (checkpoint_chunks), "
			"death-drop (save_chunk_coherent) and token (checkpoint_submit) all succeed with the "
			"preflight ON, live state exact", i);
		mp_check(ok, what);
		close_world(&S[0], s_dir[0], false);
	}
	/* journal-setup failures at the single-player state (journal admitted) */
	const struct { enum server_world_test_journal_fault k; long at; const char* name; } jf[] = {
		{SERVER_WORLD_TEST_JOURNAL_RESERVE, 0, "reserve"},
		{SERVER_WORLD_TEST_JOURNAL_OPEN, 0, "open"},
		{SERVER_WORLD_TEST_JOURNAL_WRITE, 1, "write (SD full)"},
		{SERVER_WORLD_TEST_JOURNAL_FLUSH, 0, "flush"},
		{SERVER_WORLD_TEST_JOURNAL_VERIFY, 3, "read-back verify"},
	};
	for(unsigned f = 0; f < sizeof jf / sizeof jf[0]; f++) {
		mp_world256(0, "test_light_queue_mp_recorded_f", true);
		struct server_world* w = &S[0].worlds[0];
		int md = 0;
		printf("  journal-setup fault %s at the 7.4 MiB state:\n", jf[f].name);
		server_world_test_journal_fault(jf[f].k, jf[f].at);
		const bool ok = mp_consumers(w, &rs[0], mp_hash(w), &md, true);
		server_world_test_journal_fault(jf[f].k, -1);
		char what[220];
		snprintf(what, sizeof what, "journal-setup fault (%s): every consumer still succeeds "
			"(HEAD's RAM path), live state exact", jf[f].name);
		mp_check(ok && md == SERVER_WORLD_CKPT_FULL, what);
		close_world(&S[0], s_dir[0], false);
	}
}

/* ---- part 2: journal faults, drain stop, tampering ------------------------
 * A small world (9 columns, a short generation queue, a pending torch) in a
 * state where the full copy is refused (Arena2 0) but the journal is
 * admitted. */
static const struct server_world_mem_state* mp_small_state(void) {
	static struct server_world_mem_state st;
	st = mp_base();
	st.mem1_free = MP_FLOOR + 65536;
	st.arena2_free = 0;
	return &st;
}
static uint64_t mp_light_hash(struct server_world* w) {
	uint64_t h = 1469598103934665603ull;
	for(int cx = -8; cx < 8; cx++) for(int cz = -8; cz < 8; cz++) {
		struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
		if(!sc) continue;
		h = dr_fnv(h, sc->lighting_sky, 16 * 16 * 128 / 2);
		h = dr_fnv(h, sc->lighting_torch, 16 * 16 * 128 / 2);
	}
	return h;
}
static void mp_small_world(int i, const char* id, uint64_t* h) {
	open_world(&S[i], s_dir[i], sizeof s_dir[i], id, true, 1);
	struct server_world* w = &S[i].worlds[0];
	assert(rb_fill(w, 300, 0) == 300);
	uint8_t hh = 0;
	assert(server_world_get_height(w, 8, 8, &hh));
	assert(server_world_set_block(w, 8, hh, 8, (struct block_data) {.type = BLOCK_TORCH}));
	assert(w->light_seed_n > 0 && w->generation_light_n > 0);
	*h = mp_hash(w);
}
static void mp_journal_faults(void) {
	mode(1);
	const struct server_world_mem_state* st = mp_small_state();
	struct server_world_test_mem_report rep;
	server_world_test_mem_preflight(true);
	/* prepare / verify failures: HEAD's path, then the journal next time */
	const struct { enum server_world_test_journal_fault k; long at; const char* name; } prep[] = {
		{SERVER_WORLD_TEST_JOURNAL_OPEN, 0, "open"},
		{SERVER_WORLD_TEST_JOURNAL_SEEK, 0, "seek"},
		{SERVER_WORLD_TEST_JOURNAL_WRITE, 1, "write"},
		{SERVER_WORLD_TEST_JOURNAL_SHORT_WRITE, 2, "short write"},
		{SERVER_WORLD_TEST_JOURNAL_FLUSH, 0, "flush"},
		{SERVER_WORLD_TEST_JOURNAL_VERIFY, 0, "verify header read"},
		{SERVER_WORLD_TEST_JOURNAL_VERIFY, 4, "verify plane read"},
	};
	for(unsigned f = 0; f < sizeof prep / sizeof prep[0]; f++) {
		uint64_t h0;
		mp_small_world(0, "test_light_queue_mp_jfault", &h0);
		struct server_world* w = &S[0].worlds[0];
		struct server_world_light_queue_stats q0, q1;
		server_world_test_light_queue_stats(w, &q0);
		mp_model_refused(st);
		server_world_test_journal_fault(prep[f].k, prep[f].at);
		const bool first = server_world_checkpoint_chunks(w);
		server_world_test_journal_fault(prep[f].k, -1);
		server_world_test_mem_last(&rep);
		server_world_test_light_queue_stats(w, &q1);
		const bool head = rep.mode == SERVER_WORLD_CKPT_FULL && rep.head_fallbacks == 1;
		/* Sol P1 (extra round): the journal's block + FILE were given back
		 * BEFORE HEAD's RAM attempt (ledger back to the pre-journal value) */
		char rel[220];
		snprintf(rel, sizeof rel, "journal setup fault (%s): the journal block and FILE are released "
			"before the RAM fallback (ledger %zu -> %zu B, no journal held)", prep[f].name,
			q0.wii_bytes, q1.wii_bytes);
		mp_check(server_world_test_journal_path(w)[0] == 0 && q1.wii_bytes == q0.wii_bytes
			&& q1.subsystem_bytes == q0.subsystem_bytes, rel);
		const bool exact = mp_hash(w) == h0 && !server_world_light_halted(w);
		mp_model_refused(st);
		const bool good = server_world_checkpoint_chunks(w);
		server_world_test_mem_last(&rep);
		char what[200];
		snprintf(what, sizeof what, "journal setup fault (%s) before any mutation: the checkpoint "
			"succeeds through HEAD's RAM path, exact; the next one uses the journal", prep[f].name);
		mp_check(first && head && exact && good && rep.mode == SERVER_WORLD_CKPT_JOURNAL
			&& mp_hash(w) == h0, what);
		server_world_test_mem_model(NULL, 0);
		close_world(&S[0], s_dir[0], false);
	}
	/* restore faults and tampering after the verified read-back: nothing of
	 * a bad record is applied; the world takes the sticky journal stop */
	const struct { enum server_world_test_journal_fault k; long at; int tamper; bool untouched;
		const char* name; } rest[] = {
		{SERVER_WORLD_TEST_JOURNAL_READ, 0, 0, true, "read header"},
		{SERVER_WORLD_TEST_JOURNAL_READ, 9, 0, false, "read mid record"},
		{SERVER_WORLD_TEST_JOURNAL_SHORT_READ, 3, 0, true, "short read"},
		{SERVER_WORLD_TEST_JOURNAL_CORRUPT, 4, 0, false, "plane checksum"},
		{SERVER_WORLD_TEST_JOURNAL_FAULTS, -1, 1, true, "corrupted column id"},
		{SERVER_WORLD_TEST_JOURNAL_FAULTS, -1, 2, true, "corrupted queue record"},
		{SERVER_WORLD_TEST_JOURNAL_FAULTS, -1, 3, true, "missing column"},
		{SERVER_WORLD_TEST_JOURNAL_FAULTS, -1, 4, false, "queue bytes changed after verify"},
	};
	for(unsigned f = 0; f < sizeof rest / sizeof rest[0]; f++) {
		uint64_t h0, h1;
		mp_small_world(0, "test_light_queue_mp_jfault_r", &h0);
		mp_small_world(1, "test_light_queue_mp_jfault_s", &h1);
		struct server_world* w = &S[0].worlds[0];
		/* the settled twin: what live light is after the trial's drain */
		server_world_test_mem_preflight(false);
		assert(server_world_light_drain(&S[1].worlds[0]));
		const uint64_t settled = mp_light_hash(&S[1].worlds[0]);
		server_world_test_mem_preflight(true);
		mp_model_refused(st);
		if(rest[f].tamper) server_world_test_journal_tamper(rest[f].tamper);
		else server_world_test_journal_fault(rest[f].k, rest[f].at);
		const bool bad = server_world_checkpoint_chunks(w);
		if(!rest[f].tamper) server_world_test_journal_fault(rest[f].k, -1);
		server_world_test_journal_tamper(0);
		server_world_test_mem_last(&rep);
		const bool no_apply = !rest[f].untouched || mp_light_hash(w) == settled;
		const bool stopped = server_world_light_halted(w)
			&& w->light_fault_reason == SERVER_LIGHT_FAULT_CHECKPOINT_JOURNAL;
		server_world_test_light_clear_fault(w);
		const bool sticky = server_world_light_halted(w);
		char jpath[300];
		snprintf(jpath, sizeof jpath, "%s", server_world_test_journal_path(w));
		FILE* kept = fopen(jpath, "rb");
		const bool owned = kept != NULL;
		if(kept) fclose(kept);
		mp_model_refused(st);
		const bool after = server_world_checkpoint_chunks(w);
		char what[240];
		snprintf(what, sizeof what, "journal restore %s: detected (checkpoint fails), %s, sticky "
			"journal stop survives the generic reset, journal retained, no later checkpoint",
			rest[f].name, rest[f].untouched ? "no record applied (live == settled, no wrong column)"
			: "stopped mid-restore");
		mp_check(!bad && rep.mode == SERVER_WORLD_CKPT_JOURNAL && no_apply && stopped && sticky
			&& owned && !after && rep.journal_restore_fail == 1, what);
		server_world_test_mem_model(NULL, 0);
		close_world(&S[0], s_dir[0], false);
		close_world(&S[1], s_dir[1], false);
		FILE* gone = fopen(jpath, "rb");
		mp_check(!gone, "journal restore failure: teardown releases the retained journal");
		if(gone) fclose(gone);
	}
	/* a journal trial whose drain stops (ceiling): no crash, the checkpoint
	 * fails, the world stops with its work retained (HEAD's abandon) */
	{
		uint64_t h0;
		mp_small_world(0, "test_light_queue_mp_jceiling", &h0);
		struct server_world* w = &S[0].worlds[0];
		const size_t seeds = w->light_seed_n;
		mp_model_refused(st);
		server_world_test_light_drain_ceiling(1);
		const bool r = server_world_checkpoint_chunks(w);
		server_world_test_light_drain_ceiling(0);
		server_world_test_mem_last(&rep);
		mp_check(!r && rep.mode == SERVER_WORLD_CKPT_JOURNAL && server_world_light_halted(w)
			&& w->light_fault_reason == SERVER_LIGHT_FAULT_DRAIN_CEILING
			&& w->light_seed_n == seeds,
			"journal trial + drain ceiling: the abandoned trial reads no ids (no NULL deref), the "
			"checkpoint fails, the world stops with its seeds retained");
		server_world_test_mem_model(NULL, 0);
		close_world(&S[0], s_dir[0], false);
	}
	/* an orphan journal of this slot/dimension is removed at level open */
	{
		uint64_t h0;
		mp_small_world(0, "test_light_queue_mp_orphan", &h0);
		struct server_world* w = &S[0].worlds[0];
		server_world_discard(w); /* the level is closed */
		char orphan[300];
		snprintf(orphan, sizeof orphan, "%s/cw_light_journal_0_%d.tmp", s_dir[0], (int)WORLD_DIM_OVERWORLD);
		FILE* o = fopen(orphan, "wb");
		assert(o);
		fputs("orphan", o);
		fclose(o);
		string_t level;
		string_init_set_str(level, s_dir[0]);
		server_world_create(w, level, WORLD_DIM_OVERWORLD, 4242, true); /* reopened */
		string_clear(level);
		w->session = &S[0];
		w->local = &S[0];
		FILE* g = fopen(orphan, "rb");
		mp_check(!g, "an orphan cw_light_journal_<slot>_<dim>.tmp is removed when the level opens");
		if(g) fclose(g);
		close_world(&S[0], s_dir[0], false);
	}
	server_world_test_mem_preflight(true);
}

/* the column-admission reserve: free slots, unpooled fallback, slab growth */
static void mp_column_reserve(void) {
	const size_t obj = MP_MEM2_COST(MEM2_POOL_COLUMN_OBJECT);
	const size_t slab = (size_t)MEM2_POOL_SLAB_SLOTS * ((MEM2_POOL_COLUMN_OBJECT + 31u) / 32u * 32u) + 32u;
	mp_check(server_world_mem_column_reserve(0, 0, 1, slab) == 0
		&& server_world_mem_column_reserve(3, 5, 1, slab) == 0
		&& server_world_mem_column_reserve(6, 5, 0, 0) == obj
		&& server_world_mem_column_reserve(9, 0, 0, 0) == 9 * obj,
		"column reserve: free pool slots cover reservations; with no slab room each uncovered one "
		"costs one unpooled column object (guard + lwp overhead)");
	mp_check(server_world_mem_column_reserve(1, 0, 1, slab) == MP_MEM2_COST(slab)
		&& server_world_mem_column_reserve(40, 0, 1, slab)
			== (MP_MEM2_COST(slab) + 8 * obj > 40 * obj ? MP_MEM2_COST(slab) + 8 * obj : 40 * obj)
		&& server_world_mem_column_reserve(1, 0, 1, slab) > (size_t)2500000u,
		"column reserve: below the pool cap one uncovered reservation charges the WHOLE slab "
		"mem2_pool_grow carves (~2.6 MB), the rest beyond its 32 slots unpooled; the larger of "
		"the slab path and the all-unpooled fallback");
	/* Sol: a contiguous 3 MiB carve satisfies the slab; the planner must
	 * not admit a carve that the slab then pushes below the floor */
	struct server_world_mem_state st = mp_base();
	/* carve free = floor + 1 MiB, one contiguous 3 MiB hole: the slab fits */
	st.carve_free = SERVER_WORLD_CARVE_FLOOR + ((size_t)1u << 20);
	st.carve_largest = (size_t)3u << 20;
	const struct server_world_mem_event page = {SERVER_WORLD_MEM_EV_MEM2, 3584, 1};
	struct server_world_mem_plan p;
	st.carve_reserve = server_world_mem_column_reserve(1, 0, 0, 0); /* the old unpooled-only model */
	const bool old_admits = server_world_mem_plan_run(&st, &page, 1, &p);
	st.carve_reserve = server_world_mem_column_reserve(1, 0, 1, slab);
	const bool slab_refuses = !server_world_mem_plan_run(&st, &page, 1, &p)
		&& p.reason == SERVER_WORLD_MEM_CARVE_FLOOR;
	mp_check(old_admits && slab_refuses,
		"column reserve: carve floor + 1 MiB with a 3 MiB hole: the unpooled-only reserve would "
		"admit a page and let the slab breach the floor; the slab-aware reserve refuses");
}

static void case_g9x_memory_progress(void) {
	mp_planner();
	mp_column_reserve();
	mp_increment_arena();
	mp_fragmented_checkpoint256();
	mp_recorded_states();
	mp_journal_faults();
	printf("  g9x_memory_progress: %ld PASS, %ld G9X-5 RED\n", mp_pass, mp_red);
	if(mp_red) s_fail++;
}

/* ---- G9X-7: g9x_drain_t1_equiv (G9-EXACT-PLAN item 20) ----
 * The generation light queue (CFR World.java:1269-1331 updatingLighting,
 * MetadataChunkBlock.java:37-139 func_4107_a, drained to EMPTY at the
 * MinecraftServer.java:346-349 boundary) runs twice on the same input: once
 * with the frozen pre-T1 relax (server_world_test_beta_light_reference) and
 * once with the shipping one. Both dimensions; each run generates a 9x9
 * x-major domain, takes a coherent checkpoint (trial drain + exact rollback)
 * of a chunk with pending work, drains to empty, saves, reloads the domain
 * from disk (imported chunks) and generates its 11x11 rim against them,
 * drains again. Every ordered queue event (request/reject/merge/admit/pop/
 * yield), every store attempt (plane, index, before/after, chunk ordinal,
 * emitting file), every java.util.Random call (owner, API, pre/post state)
 * and every final ID/meta/Sky/Block/height byte, queue image, RNG state and
 * saved-region byte must be identical. Source LINE numbers are excluded:
 * the two bodies are different lines of the same file by construction.
 * The work census is the T1 claim: the same cells relaxed with fewer chunk
 * lookups. Timing is printed (paired, hooks off), never asserted. */
#define T1_N 9
#define T1_C0 (-4)
static cw_sha256_ctx t1_ev;
static uint64_t t1_ev_n;
static const struct server_world* t1_world;
static const struct server_local* t1_session;
static uint64_t t1_gen0, t1_kind[3];
/* A store's chunk identity is its coordinates in the resident dict. Dict
 * values move when the dict grows, so the struct address is NOT an identity
 * (two runs differ); the `ids` slot does not move while the chunk lives. A
 * cache entry is re-verified against the dict on every use. A chunk not (yet)
 * resident is tagged as such; its bytes reach the final-state hash. */
static const uint8_t* t1_cache_ids[64];
static int64_t t1_cache_key[64];
static void t1_ord_reset(void) {
	memset(t1_cache_ids, 0, sizeof t1_cache_ids);
}
static void t1_chunk(const struct server_chunk* sc, uint64_t out[2]) {
	out[0] = 0;
	out[1] = 0;
	if(!sc) return;
	struct server_world* w = (struct server_world*)t1_world;
	const size_t slot = (size_t)(((uintptr_t)sc->ids >> 12) * 2654435761u) & 63u;
	if(t1_cache_ids[slot] == sc->ids) {
		const struct server_chunk* r = dict_server_chunks_get(w->chunks, t1_cache_key[slot]);
		if(r && r->ids == sc->ids) {
			out[0] = 1;
			out[1] = (uint64_t)t1_cache_key[slot];
			return;
		}
	}
	out[0] = 2; /* not resident */
	dict_server_chunks_it_t it;
	for(dict_server_chunks_it(it, w->chunks); !dict_server_chunks_end_p(it);
		dict_server_chunks_next(it))
		if(dict_server_chunks_ref(it)->value.ids == sc->ids) {
			t1_cache_ids[slot] = sc->ids;
			t1_cache_key[slot] = dict_server_chunks_ref(it)->key;
			out[0] = 1;
			out[1] = (uint64_t)t1_cache_key[slot];
			return;
		}
}
static void t1_upd(const void* p, size_t n) {
	cw_sha256_update(&t1_ev, p, n);
}
static void t1_str(const char* s) {
	if(!s) s = "";
	const char* b = strrchr(s, '/');
	const char* c = strrchr(s, '\\');
	if(c > b) b = c;
	if(b) s = b + 1;
	t1_upd(s, strlen(s) + 1);
}
static void t1_u64(uint64_t v) { t1_upd(&v, sizeof v); }
static bool t1_light_hook(const struct lighting_history_event* e) {
	t1_ev_n++;
	t1_kind[0]++;
	t1_u64('L');
	t1_str(e->operation);
	t1_str(e->file);
	t1_u64(e->object == t1_world ? 1 : 2);
	const int64_t v[] = {(int64_t)e->index, (int64_t)e->before_depth,
		(int64_t)e->after_depth, e->x, e->y, e->z, e->x2, e->y2, e->z2,
		e->channel, e->before, e->after, e->packed, e->forced, e->depth,
		e->result};
	t1_upd(v, sizeof v);
	return true;
}
static bool t1_world_hook(const struct server_world_history_event* e) {
	t1_ev_n++;
	t1_kind[1]++;
	t1_u64('W');
	t1_str(e->operation); t1_str(e->edge); t1_str(e->outcome); t1_str(e->file);
	t1_u64(e->world == t1_world ? 1 : e->world ? 2 : 0);
	t1_u64(e->session == t1_session ? 1 : e->session ? 2 : 0);
	uint64_t chunk[2];
	t1_chunk(e->chunk, chunk);
	t1_upd(chunk, sizeof chunk);
	const int64_t v[] = {(int64_t)e->request, (int64_t)e->parent,
		(int64_t)e->update, e->generation ? (int64_t)(e->generation - t1_gen0) : -1, (int64_t)e->hell_rng,
		(int64_t)e->session_rng, e->x, e->y, e->z, e->before_id,
		e->before_meta, e->after_id, e->after_meta, e->slot, e->dimension};
	t1_upd(v, sizeof v);
	return true;
}
static void t1_jrand_hook(const struct jrand_test_event* e) {
	t1_ev_n++;
	t1_kind[2]++;
	t1_u64('R');
	t1_str(e->file);
	uint64_t owner = 0;
	if(t1_world && e->rng == &t1_world->rand) owner = 1;
	else if(t1_world && e->rng == &t1_world->hell_populate_rng) owner = 2;
	else if(t1_session && e->rng == &t1_session->rand_src) owner = 3;
	t1_u64(owner);
	const uint64_t v[] = {(uint64_t)e->api, (uint64_t)e->arg, e->has_pre,
		e->pre, e->post, e->result};
	t1_upd(v, sizeof v);
}

struct t1_result {
	char events[65], state[4][65];
	uint64_t events_n, kinds[3], work[4];
	uint64_t drain_us[2], total_us;
	bool ok;
};

static void t1_chunk_bytes(cw_sha256_ctx* h, const struct server_chunk* sc) {
	cw_sha256_update(h, sc->ids, 32768);
	cw_sha256_update(h, sc->metadata, 16384);
	cw_sha256_update(h, sc->lighting_sky, 16384);
	cw_sha256_update(h, sc->lighting_torch, 16384);
	cw_sha256_update(h, sc->heightmap, 256);
	const uint8_t f[] = {sc->populated, sc->modified};
	cw_sha256_update(h, f, sizeof f);
}
static void t1_state(struct server_world* w, int c0, int n, char out[65]) {
	cw_sha256_ctx h;
	cw_sha256_init(&h);
	for(int cx = c0 - 1; cx <= c0 + n; cx++)
		for(int cz = c0 - 1; cz <= c0 + n; cz++) {
			const struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
			const uint8_t present = sc != NULL;
			cw_sha256_update(&h, &present, 1);
			if(sc) t1_chunk_bytes(&h, sc);
		}
	const uint64_t v[] = {w->rand.seed, w->hell_populate_rng.seed,
		w->session ? w->session->rand_src.seed : 0, w->generation_light_n,
		w->generation_light_stat[0], w->generation_light_stat[1],
		w->generation_light_stat[2], (uint64_t)w->light_fault,
		(uint64_t)w->light_fault_reason};
	cw_sha256_update(&h, v, sizeof v);
	cw_sha256_final_hex(&h, out);
}

static void t1_load(struct server_world* w, int c0, int n) {
	for(int cx = c0; cx < c0 + n; cx++)
		for(int cz = c0; cz < c0 + n; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(w, cx, cz, &sc) && sc);
		}
}

static void t1_open(struct server_world* w, const char* dir, enum world_dim dim, bool fresh) {
	if(fresh) {
		memset(&S[1], 0, sizeof S[1]);
		if(!server_local_te_tables_init(&S[1])) abort(); /* IMP-TE-CAPS */
		S[1].rand_src.seed = 0x9E11u;
	}
	dict_entity_init(S[1].entities);
	clin_server_publication_stop(false);
	string_t level;
	string_init_set_str(level, dir);
	/* Beta's world RNG is a fresh unseeded Random: pinned, as is Math.random, so the two runs
	 * see identical streams (the clock seeds differ run to run by design) */
	server_world_test_seed_runtime_jrand(&w->rand, 4242);
	server_local_test_seed_math_random(0x51A7);
	server_world_create(w, level, dim, 4242, false);
	string_clear(level);
	w->session = w->local = &S[1];
	jrand_set_seed(&w->rand, 4242);
	if(fresh) t1_gen0 = w->generation; /* process-wide epoch counter */
	t1_world = w;
	t1_session = &S[1];
	t1_ord_reset();
}

/* One complete run. hooked = record the ordered event stream (equivalence
 * run); otherwise only time it. */
static struct t1_result t1_run(enum world_dim dim, bool reference, bool hooked) {
	struct t1_result r;
	memset(&r, 0, sizeof r);
	r.ok = true;
	server_world_test_beta_light_reference(reference);
	server_world_test_beta_light_work(NULL, true);
	char id[96];
	snprintf(id, sizeof id, "g9x_t1_%s_%s", dim == WORLD_DIM_NETHER ? "nether" : "overworld",
		reference ? "ref" : "ship");
	test_fixture_dir(id, s_dir[1], sizeof s_dir[1]);
	MKDIR(s_dir[1]);
	char region[260];
	snprintf(region, sizeof region, "%s/region", s_dir[1]);
	MKDIR(region);
	cw_sha256_init(&t1_ev);
	t1_ev_n = 0;
	memset(t1_kind, 0, sizeof t1_kind);
	void (*prev_jrand)(const struct jrand_test_event*) = jrand_test_hook;
	if(hooked) {
		lighting_test_history_reset();
		server_world_test_history_reset();
		lighting_test_history_hook = t1_light_hook;
		server_world_test_history_hook = t1_world_hook;
		jrand_test_hook = t1_jrand_hook;
	}
	const ptime_t t0 = time_get();
	struct server_world* w = &S[1].worlds[0];
	t1_open(w, s_dir[1], dim, true);
	/* 1. generation: population requests queue, nothing drains yet */
	t1_load(w, T1_C0, T1_N);
	r.ok &= w->generation_light_n > 0;
	/* 2. coherent checkpoint of a chunk with pending work: a trial drain,
	 * then the exact restore of every array and queue byte */
	const size_t qb = server_world_test_generation_queue_copy(w, NULL, 0);
	uint8_t* q0 = malloc(qb);
	uint8_t* q1 = malloc(qb);
	assert(q0 && q1);
	assert(server_world_test_generation_queue_copy(w, q0, qb) == qb);
	r.ok &= server_world_save_chunk_coherent(w, 0, 0);
	r.ok &= server_world_test_generation_queue_copy(w, q1, qb) == qb && !memcmp(q0, q1, qb);
	free(q0);
	free(q1);
	assert(server_world_sync_regions(w));
	{
		struct server_chunk saved;
		server_chunk_init(&saved);
		r.ok &= region_archive_get_chunk(server_world_chunk_region(w, 0, 0), 0, 0, &saved, NULL, NULL);
		cw_sha256_ctx h;
		cw_sha256_init(&h);
		t1_chunk_bytes(&h, &saved);
		cw_sha256_final_hex(&h, r.state[0]);
		server_world_chunk_destroy(&saved);
	}
	/* 3. the prescribed boundary: drain to EMPTY */
	ptime_t d0 = time_get();
	r.ok &= server_world_light_drain(w);
	r.drain_us[0] = time_diff_us(d0, time_get());
	r.ok &= w->generation_light_n == 0;
	t1_state(w, T1_C0, T1_N, r.state[1]);
	/* 4. save, reload from disk, generate the rim against imported chunks */
	server_world_destroy(w); /* persists every chunk (needs the entity dict) */
	dict_entity_clear(S[1].entities);
	t1_open(w, s_dir[1], dim, false);
	t1_load(w, T1_C0 - 1, T1_N + 2);
	t1_state(w, T1_C0 - 1, T1_N + 2, r.state[2]);
	d0 = time_get();
	r.ok &= server_world_light_drain(w);
	r.drain_us[1] = time_diff_us(d0, time_get());
	r.ok &= w->generation_light_n == 0;
	t1_state(w, T1_C0 - 1, T1_N + 2, r.state[3]);
	r.total_us = time_diff_us(t0, time_get());
	if(hooked) {
		r.ok &= !lighting_test_history_failed() && !server_world_test_history_failed();
		lighting_test_history_hook = NULL;
		server_world_test_history_hook = NULL;
		jrand_test_hook = prev_jrand;
	}
	cw_sha256_ctx h = t1_ev;
	cw_sha256_final_hex(&h, r.events);
	r.events_n = t1_ev_n;
	memcpy(r.kinds, t1_kind, sizeof r.kinds);
	server_world_test_beta_light_work(r.work, true);
	server_world_test_beta_light_reference(false);
	close_world(&S[1], s_dir[1], false);
	t1_world = NULL;
	t1_session = NULL;
	return r;
}

static void case_g9x_drain_t1_equiv(void) {
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	mode(1);
	long red = 0;
	for(int d = 0; d < 2; d++) {
		const enum world_dim dim = d ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
		const char* dn = d ? "nether" : "overworld";
		const struct t1_result a = t1_run(dim, true, true);
		const struct t1_result b = t1_run(dim, false, true);
		for(int i = 0; i < 2; i++) {
			const struct t1_result* t = i ? &b : &a;
			printf("  %s %s: %llu events (light %llu, world %llu, rng %llu) %s\n", dn,
				i ? "shipping " : "reference", (unsigned long long)t->events_n,
				(unsigned long long)t->kinds[0], (unsigned long long)t->kinds[1],
				(unsigned long long)t->kinds[2], t->events);
		}
		for(int k = 0; k < 4; k++)
			printf("  %s state[%d] %s / %s\n", dn, k, a.state[k], b.state[k]);
		for(int i = 0; i < 2; i++) {
			const struct t1_result* t = i ? &b : &a;
			printf("  %s work %s: columns %llu cells %llu dict %llu resolves %llu\n", dn,
				i ? "shipping " : "reference", (unsigned long long)t->work[0],
				(unsigned long long)t->work[1], (unsigned long long)t->work[2],
				(unsigned long long)t->work[3]);
		}
		char what[220];
		snprintf(what, sizeof what, "%s: both runs complete (checkpoint rollback exact, queues drained empty, no stop)", dn);
		check(a.ok && b.ok, what);
		snprintf(what, sizeof what, "%s: identical ordered queue/store/setter/RNG event stream (%llu events)",
			dn, (unsigned long long)a.events_n);
		check(a.events_n == b.events_n && a.events_n > 0 && !strcmp(a.events, b.events), what);
		bool same = true;
		for(int k = 0; k < 4; k++) same &= !strcmp(a.state[k], b.state[k]);
		snprintf(what, sizeof what, "%s: identical checkpoint disk image, settled domain, imported reload and rim-settled bytes + RNG/queue state", dn);
		check(same, what);
		snprintf(what, sizeof what, "%s: the same Beta work (columns entered, cells relaxed)", dn);
		check(a.work[0] == b.work[0] && a.work[1] == b.work[1] && a.work[1] > 0, what);
		/* T1 claim: chunk lookups per relaxed cell */
		const uint64_t ra = a.work[2] + a.work[3], rb = b.work[2] + b.work[3];
		printf("  %s chunk lookups per relaxed cell: reference %.3f shipping %.3f\n", dn,
			(double)ra / (double)(a.work[1] ? a.work[1] : 1),
			(double)rb / (double)(b.work[1] ? b.work[1] : 1));
		if(!(rb * 2 <= ra)) {
			printf("G9X-7 RED: %s generation drain makes %llu chunk lookups (%llu residency dict "
				"probes + %llu resolves) for %llu relaxed cells; the frozen reference makes %llu "
				"-- not at most half\n", dn, (unsigned long long)rb,
				(unsigned long long)b.work[2], (unsigned long long)b.work[3],
				(unsigned long long)b.work[1], (unsigned long long)ra);
			red++;
		}
		/* paired timing, hooks off (reported, not asserted) */
		uint64_t best[2][3];
		for(int i = 0; i < 2; i++) for(int k = 0; k < 3; k++) best[i][k] = UINT64_MAX;
		const char* reps_env = getenv("CW_G9X7_REPS");
		const int reps = reps_env ? atoi(reps_env) : 1;
		for(int rep = 0; rep < reps; rep++)
			for(int impl = 0; impl < 2; impl++) {
				const struct t1_result t = t1_run(dim, impl == 0, false);
				const uint64_t v[3] = {t.drain_us[0], t.drain_us[1], t.total_us};
				for(int k = 0; k < 3; k++) if(v[k] < best[impl][k]) best[impl][k] = v[k];
			}
		if(reps > 0)
			printf("  %s timing (best of %d, us, reference -> shipping): drain %llu -> %llu, "
				"rim drain %llu -> %llu, whole run %llu -> %llu\n", dn, reps,
				(unsigned long long)best[0][0], (unsigned long long)best[1][0],
				(unsigned long long)best[0][1], (unsigned long long)best[1][1],
				(unsigned long long)best[0][2], (unsigned long long)best[1][2]);
	}
	check(red == 0, "T1: the shipping generation relax makes at most half the reference's chunk lookups");
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_NORMAL);
}


/* HALT-STOP P1. Canonical tapes contain explicit LE integers and byte planes,
 * never pointers, padding, timing, allocation capacity or fault diagnostics.
 * Fault snapshots start INSIDE the existing fault observer: valid prefix work
 * is retained. Reservation/depth/binding unwind is checked separately.
 * Beta CFR World:380-392; BlockFlowing:69-83; BlockFluid:205-223;
 * BlockRedstoneWire:63-78; World.tickUpdates:1526-1569 (1000 cap).
 * This suite tests dispatcher cancellation, not arbitrary leaf cancellation. */
struct hs_bytes { uint8_t* p; size_t n, cap; };
static struct hs_bytes hs_events, hs_at_fault;
static struct server_world* hs_world;
static struct server_world* hs_fault_world;
static struct block* hs_original[256];
static unsigned hs_callbacks, hs_fault_callbacks, hs_fault_visit;
static bool hs_inject, hs_hit, hs_wire;
static unsigned hs_fail_child;
static bool hs_peer_fault;
static bool hs_nested_rs, hs_rs_active;
extern void redstone_server_test_queue_request(struct server_world* w,int x,int y,int z);
static FILE* hs_tape;
extern size_t redstone_server_test_snapshot(uint64_t* out, size_t capacity);
/* F_SCOPE_BEGIN */
extern int blocks_test_notify_depth(void);
extern int server_world_test_notify_scope_depth(void);
extern int redstone_server_test_drain_depth(void);
/* F_SCOPE_END */
static void hs_tail(struct server_world* w, bool reactivate);
static unsigned hs_tail_after_stop;

static void hs_put(struct hs_bytes* b, const void* p, size_t n) {
	if(b->n + n > b->cap) {
		b->cap = (b->n + n) * 2 + 1024;
		uint8_t* grown = realloc(b->p, b->cap);
		assert(grown); b->p = grown;
	}
	memcpy(b->p + b->n, p, n); b->n += n;
}
static void hs_u64(struct hs_bytes* b, uint64_t v) {
	uint8_t p[8];
	for(int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8*i));
	hs_put(b, p, sizeof p);
}
static void hs_float(struct hs_bytes* b, float value) {
	uint32_t bits; memcpy(&bits,&value,sizeof bits); hs_u64(b,bits);
}
static void hs_jrand(struct hs_bytes* b, const struct java_random* rng) {
	hs_u64(b,rng->seed); hs_u64(b,rng->have_next_gaussian);
	uint64_t cached=0;
	if(rng->have_next_gaussian) memcpy(&cached,&rng->next_gaussian_cached,sizeof cached);
	hs_u64(b,cached);
	struct java_random copy=*rng;
	for(int i=0;i<8;i++) { double d=jrand_next_double(&copy); uint64_t bits; memcpy(&bits,&d,sizeof bits); hs_u64(b,bits); }
}
static void hs_rpc(const struct client_rpc* rpc, void* user) {
	(void)user;
	hs_u64(&hs_events,UINT64_C(0x525043)); hs_u64(&hs_events,rpc->type);
	hs_u64(&hs_events,rpc->world_index); hs_u64(&hs_events,rpc->world_generation);
	if(rpc->type==CRPC_SET_BLOCK) {
		hs_u64(&hs_events,rpc->payload.set_block.x);
		hs_u64(&hs_events,rpc->payload.set_block.y);
		hs_u64(&hs_events,rpc->payload.set_block.z);
		struct block_data b=rpc->payload.set_block.block;
		hs_u64(&hs_events,b.type); hs_u64(&hs_events,b.metadata);
		hs_u64(&hs_events,b.sky_light); hs_u64(&hs_events,b.torch_light);
		const struct column_revision* r=&rpc->payload.set_block.local_revision;
		hs_u64(&hs_events,r->owner); hs_u64(&hs_events,r->incarnation); hs_u64(&hs_events,r->content);
		hs_u64(&hs_events,r->light); hs_u64(&hs_events,r->events);
		hs_u64(&hs_events,rpc->payload.set_block.local_previous_content);
	} else if(rpc->type==CRPC_SOUND) {
		hs_u64(&hs_events,rpc->payload.sound.sound_id);
		for(int i=0;i<3;i++) hs_float(&hs_events,rpc->payload.sound.pos[i]);
		hs_float(&hs_events,rpc->payload.sound.vol); hs_float(&hs_events,rpc->payload.sound.pitch);
		hs_u64(&hs_events,rpc->payload.sound.ui);
	} else if(rpc->type==CRPC_SMOKE_PUFF) {
		for(int i=0;i<3;i++) hs_float(&hs_events,rpc->payload.smoke_puff.pos[i]);
		hs_float(&hs_events,rpc->payload.smoke_puff.intensity);
	} else if(rpc->type==CRPC_SPAWN_ITEM) {
		const struct crpc_spawn_item* p=&rpc->payload.spawn_item;
		hs_u64(&hs_events,p->entity_id); hs_u64(&hs_events,p->item.id);
		hs_u64(&hs_events,p->item.count); hs_u64(&hs_events,p->item.durability);
		hs_u64(&hs_events,p->item.nbt_record);
		for(int i=0;i<3;i++) { hs_float(&hs_events,p->pos[i]); hs_float(&hs_events,p->vel[i]); }
	} else if(rpc->type==CRPC_WORLD_RELEASE) {
		hs_u64(&hs_events,rpc->payload.world_release.retiring);
	} else if(rpc->type!=CRPC_LOAD_FAILED) {
		printf("unexpected HALT witness RPC type %d\n",rpc->type);
		check(false,"canonical tape must serialize every witness RPC payload");
	}
}
static void hs_capture(struct hs_bytes* b) {
	b->n = 0;
	struct server_local* s = hs_world->session ? hs_world->session : hs_world->local;
	hs_u64(b, hs_callbacks);
	hs_u64(b, redstone_server_drain_visits());
	hs_u64(b, redstone_server_request_ring_count());
	hs_u64(b, redstone_server_pending());
	size_t rs_n=redstone_server_test_snapshot(NULL,0);
	uint64_t* rs=malloc(rs_n*sizeof *rs); assert(rs);
	assert(redstone_server_test_snapshot(rs,rs_n)==rs_n);
	hs_u64(b,rs_n); for(size_t i=0;i<rs_n;i++) hs_u64(b,rs[i]); free(rs);
	hs_u64(b, fluid_block_lava_mix_effect_total());
	hs_u64(b, s->rand_src.seed);
	hs_u64(b,server_local_sound_rng()->seed);
	hs_u64(b, dict_entity_size(s->entities));
	hs_u64(b,s->entity_rng.seed); hs_u64(b,s->container_drop_rng.seed); hs_u64(b,s->dispenser_break_rng.seed);
	dict_entity_it_t entities;
	for(dict_entity_it(entities,s->entities); !dict_entity_end_p(entities); dict_entity_next(entities)) {
		const struct entity* e=dict_entity_ref(entities)->value;
		hs_u64(b,e->id); hs_u64(b,e->type); hs_u64(b,e->world_slot);
		hs_u64(b,e->delay_destroy); hs_u64(b,e->health); hs_u64(b,e->on_server);
		for(int i=0;i<3;i++) { hs_float(b,e->pos[i]); hs_float(b,e->pos_old[i]); hs_float(b,e->vel[i]); }
		hs_float(b,e->orient[0]); hs_float(b,e->orient[1]);
		assert(e->type==ENTITY_ITEM);
		hs_u64(b,e->data.item.item.id); hs_u64(b,e->data.item.item.count);
		hs_u64(b,e->data.item.item.durability); hs_u64(b,e->data.item.item.nbt_record);
		hs_u64(b,e->data.item.age); hs_u64(b,e->data.item.persisted_fire); hs_u64(b,e->data.item.persisted_air);
		hs_u64(b,e->data.item.pickup_delay); hs_u64(b,e->data.item.was_in_water);
	}
	struct server_local_math_random_state math;
	server_local_math_random_capture(&math);
	hs_u64(b, math.initialized); hs_jrand(b,&math.rng);
	for(int slot = 0; slot < SERVER_LOCAL_WORLD_SLOTS; slot++) {
		if(!hs_world->session && slot) break;
		struct server_world* w = hs_world->session ? &s->worlds[slot] : hs_world;
		hs_u64(b, server_world_chunks_ready(w));
		if(!server_world_chunks_ready(w)) continue;
		hs_jrand(b,&w->rand); hs_u64(b, w->current_tick);
		hs_u64(b, w->light_seed_n); hs_u64(b, w->generation_light_n);
		for(size_t i=0;i<w->generation_light_n;i++) {
			int r[7]; assert(server_world_test_generation_light_region(w,i,r));
			for(int j=0;j<7;j++) hs_u64(b,r[j]);
		}
		for(size_t i=0;i<w->light_seed_n;i++) {
			const struct world_modification_entry* p=&w->light_seeds[i];
			hs_u64(b,p->x); hs_u64(b,p->y); hs_u64(b,p->z);
			hs_u64(b,p->blk.type); hs_u64(b,p->blk.metadata);
			hs_u64(b,p->blk.sky_light); hs_u64(b,p->blk.torch_light);
		}
		hs_u64(b,w->light_undo_n);
		for(size_t i=0;i<w->light_undo_n;i++) {
			hs_u64(b,w->light_undo[i].x); hs_u64(b,w->light_undo[i].z);
			hs_put(b,w->light_undo[i].sky,sizeof w->light_undo[i].sky);
		}
		hs_u64(b,w->light_pin_n);
		for(size_t i=0;i<w->light_pin_n;i++) hs_u64(b,w->light_pins[i]);
		/* Fixed spatial order, all five planes, including untouched cells. */
		size_t resident_seen=0;
		for(int cx = -1; cx <= 1; cx++) for(int cz = -1; cz <= 1; cz++) {
			struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx,cz));
			hs_u64(b, sc != NULL);
			if(!sc) continue;
			resident_seen++;
			hs_put(b, sc->ids, SERVER_CHUNK_IDS_BYTES);
			hs_put(b, sc->metadata, SERVER_CHUNK_NIBBLE_BYTES);
			hs_put(b, sc->lighting_sky, SERVER_CHUNK_NIBBLE_BYTES);
			hs_put(b, sc->lighting_torch, SERVER_CHUNK_NIBBLE_BYTES);
			hs_put(b, sc->heightmap, SERVER_CHUNK_HEIGHTMAP_BYTES);
			hs_u64(b, sc->load_tick_rearm_cursor); hs_u64(b, sc->load_ticks_rearmed);
		}
		assert(resident_seen==dict_server_chunks_size(w->chunks)); /* no sampled columns */
		hs_u64(b, w->scheduled_tick_count);
		for(size_t i=0; i<w->scheduled_tick_count; i++) {
			const struct scheduled_tick* t = &w->scheduled_ticks[(w->scheduled_tick_head+i)%w->scheduled_tick_capacity];
			hs_u64(b,t->x); hs_u64(b,t->y); hs_u64(b,t->z);
			hs_u64(b,t->block_type); hs_u64(b,t->fire_at_tick); hs_u64(b,t->queue_flags);
		}
	}
}
static void hs_fault(struct server_world* w) {
	if(hs_hit) return;
	hs_hit = true; hs_fault_world = w;
	hs_fault_callbacks = hs_callbacks;
	hs_fault_visit = redstone_server_drain_visits();
	hs_capture(&hs_at_fault);
}
static void hs_raw(struct server_world* w, int x, int y, int z, int id, int meta) {
	(void)server_world_set_block_silent(w,x,y,z,(struct block_data){.type=id,.metadata=meta});
	struct block_data b; assert(server_world_get_block(w,x,y,z,&b) && b.type==id && b.metadata==meta);
}
static void hs_nested_write(void) {
	server_world_test_light_fail_after(0);
	struct server_world* target=hs_peer_fault ? &hs_world->session->worlds[1-hs_world->world_index] : hs_world;
	assert(!server_world_set_block_plain(target, 12,100,12,BLOCK_OBSIDIAN));
	server_world_test_light_fail_after(-1);
}
static void hs_neighbour(struct server_local* s, struct block_info* info) {
	hs_callbacks++;
	hs_u64(&hs_events, info->x); hs_u64(&hs_events, info->y); hs_u64(&hs_events, info->z);
	hs_u64(&hs_events, info->block->type); hs_u64(&hs_events, info->block->metadata);
	hs_u64(&hs_events, server_world_notify_origin());
	if(hs_rs_active && hs_nested_rs && hs_callbacks==1) {
		/* Retained request seam is the existing tagged production ring. The
		 * nested flood/drain itself is real and owns a second live frame. */
		redstone_server_test_queue_request(hs_world,8,100,8);
		struct block_data b; assert(server_world_get_block(hs_world,12,100,8,&b));
		struct block_info child={.block=&b,.x=12,.y=100,.z=8};
		redstone_wire_neighbour_change(s,&child);
		return;
	}
	if(hs_inject && hs_callbacks == hs_fail_child) {
		if(hs_wire) hs_nested_write();
		else {
			/* Fail the real child reservation, not the outer valid write. */
			server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, 0);
			if(info->block->type == BLOCK_LAVA_STILL && info->block->metadata == 0)
				server_world_test_light_fail_after(0);
		}
	}
	struct block* original = hs_original[info->block->type];
	if(original && original->onNeighbourBlockChange)
		original->onNeighbourBlockChange(s, info);
}
static void hs_scheduled(struct server_local* s, struct block_info* info) {
	(void)s; (void)info;
	hs_callbacks++;
	if(hs_inject && hs_callbacks==hs_fail_child) hs_nested_write();
}
static void hs_write_tape(const char* name, const struct hs_bytes* state) {
	if(!hs_tape) return;
	struct hs_bytes record={0};
	hs_u64(&record,strlen(name)); hs_put(&record,name,strlen(name));
	hs_u64(&record,hs_events.n); hs_put(&record,hs_events.p,hs_events.n);
	hs_u64(&record,state->n); hs_put(&record,state->p,state->n);
	assert(fwrite(record.p,1,record.n,hs_tape)==record.n); free(record.p);
}
static void hs_finish(const char* name, bool healthy, struct server_world* previous) {
	server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, -1);
	server_world_test_light_fail_after(-1);
	struct hs_bytes after = {0};
	hs_capture(&after);
	if(healthy) {
		check(!hs_hit && !server_world_light_halted(hs_world), name);
		hs_write_tape(name,&after);
	} else {
		bool same = hs_hit && after.n == hs_at_fault.n && !memcmp(after.p,hs_at_fault.p,after.n);
		printf("HALT %s: hit=%d callbacks=%u fault_callbacks=%u visits=%u fault_visits=%u bytes=%zu unchanged=%d\n",
			name,hs_hit,hs_callbacks,hs_fault_callbacks,redstone_server_drain_visits(),hs_fault_visit,after.n,same);
		check(same, name);
		check(hs_hit && hs_fault_world->light_fault, "real nested reservation failure observed");
	}
	check((hs_world->session ? hs_world->session : hs_world->local)->block_world == previous && !hs_world->editing_blocks
		&& !hs_world->light_resv_cols && !hs_world->light_resv_seeds, "scope/binding/reservation cleanup");
	/* F_SCOPE_BEGIN */
	check(!blocks_test_notify_depth() && !server_world_test_notify_scope_depth()
		&& !redstone_server_test_drain_depth(),"all dispatcher recursion depths unwind");
	/* F_SCOPE_END */
	free(after.p);
	server_world_test_light_fault_hook = NULL;
}
static void hs_fluid(const char* name, bool healthy, int kind, int slot) {
	struct server_local* s = &S[2];
	open_world(s,s_dir[2],sizeof s_dir[2],"test_halt_fluid",true,1);
	struct server_world* w = &s->worlds[slot];
	if(slot) {
		string_t level; string_init_set_str(level,s_dir[2]);
		server_world_create(w,level,WORLD_DIM_NETHER,4242,true); string_clear(level);
		w->session=s; w->local=s; w->world_index=1;
		for(int cx=-1;cx<=1;cx++) for(int cz=-1;cz<=1;cz++) {
			struct server_chunk* c=NULL; assert(server_world_load_chunk(w,cx,cz,&c));
		}
		assert(server_world_light_drain(w));
	}
	hs_world=w; hs_wire=false; hs_inject=!healthy; hs_hit=false;
	hs_callbacks=0; hs_fault_callbacks=0; hs_events.n=0; hs_fail_child=1;
	const int x=8,y=100,z=8;
	for(int dx=-2;dx<=2;dx++) for(int dz=-1;dz<=1;dz++) for(int dy=-2;dy<=2;dy++)
		hs_raw(w,x+dx,y+dy,z+dz,BLOCK_STONE,0);
	int dest_y = kind ? y-1 : y;
	hs_raw(w,x-1,dest_y,z,kind==2?BLOCK_LAVA_STILL:BLOCK_WATER_STILL,kind==2?0:2);
	hs_raw(w,x+1,dest_y,z,BLOCK_LAVA_STILL,5);
	if(kind) hs_raw(w,x,dest_y,z,BLOCK_AIR,0);
	hs_raw(w,x,y,z,BLOCK_WATER_FLOW,kind?0:4);
	jrand_set_seed(&w->rand,4242); server_local_test_seed_math_random(424242);
	/* Remove load-rearm ticks; this fixture invokes the REGISTERED tick. */
	w->scheduled_tick_count=w->scheduled_tick_head=0;
	server_world_scheduled_ticks_resync(w); /* the containment set forgets them too */
	struct block copies[5];
	const int ids[]={BLOCK_STONE,BLOCK_WATER_FLOW,BLOCK_WATER_STILL,BLOCK_LAVA_FLOW,BLOCK_LAVA_STILL};
	for(int i=0;i<5;i++) {
		hs_original[ids[i]]=blocks[ids[i]]; copies[i]=*blocks[ids[i]];
		copies[i].onNeighbourBlockChange=hs_neighbour; blocks[ids[i]]=&copies[i];
	}
	struct server_world* previous=s->block_world;
	struct server_world* binding=block_world_enter(s,w);
	server_world_test_light_fault_hook=hs_fault;
	hs_tail_after_stop=0; server_world_test_fluid_tail_hook=hs_tail;
	struct block_data b; assert(server_world_get_block(w,x,y,z,&b));
	struct block_info info={.block=&b,.x=x,.y=y,.z=z};
	blocks[BLOCK_WATER_FLOW]->onWorldTick(s,&info);
	block_world_leave(s,binding);
	if(!healthy) {
		unsigned before=hs_callbacks;
		notifyNeighbours_from_w(&S[0],w,x,y,z,BLOCK_WATER_FLOW);
		notifyNeighbours_from_w(s,w,x,y,z,BLOCK_WATER_FLOW);
		check(hs_callbacks==before,"repeated stopped explicit-world dispatch, foreign callback host");
	}
	hs_finish(name,healthy,previous);
	check(!hs_tail_after_stop,"no fluid reactivation/rearm tail entry after stop");
	server_world_test_fluid_tail_hook=NULL;
	for(int i=0;i<5;i++) blocks[ids[i]]=hs_original[ids[i]];
	if(slot) server_world_discard(w);
	close_world(s,s_dir[2],false); clin_server_publication_stop(false);
}
static void hs_dispatch(const char* name, bool healthy, bool wire, int slot, bool peer_fault, bool nested) {
	struct server_local* s=&S[2];
	open_world(s,s_dir[2],sizeof s_dir[2],"test_halt_dispatch",true,1);
	hs_world=&s->worlds[slot]; hs_wire=wire; hs_inject=!healthy;
	if(slot || peer_fault) {
		struct server_world* peer=&s->worlds[1];
		string_t level; string_init_set_str(level,s_dir[2]);
		server_world_create(peer,level,WORLD_DIM_NETHER,4242,true); string_clear(level);
		peer->session=s; peer->local=s; peer->world_index=1;
		for(int cx=-1;cx<=1;cx++) for(int cz=-1;cz<=1;cz++) {
			struct server_chunk* c=NULL; assert(server_world_load_chunk(peer,cx,cz,&c));
		}
		assert(server_world_light_drain(peer)); jrand_set_seed(&peer->rand,4242);
	}
	hs_hit=false; hs_callbacks=0; hs_events.n=0; hs_fail_child=nested?2:1; hs_peer_fault=peer_fault; hs_rs_active=wire; hs_nested_rs=nested;
	server_local_test_seed_math_random(424242);
	struct server_world* w=hs_world;
	struct server_world* previous=s->block_world;
	struct block copy=*blocks[BLOCK_STONE]; hs_original[BLOCK_STONE]=blocks[BLOCK_STONE];
	copy.onNeighbourBlockChange=hs_neighbour;
	struct server_world* binding=block_world_enter(s,w);
	if(wire) {
		hs_raw(w,8,99,8,BLOCK_STONE,0); hs_raw(w,9,100,8,BLOCK_REDSTONE_TORCH_LIT,5);
		hs_raw(w,8,100,8,BLOCK_REDSTONE_WIRE,0);
		hs_raw(w,7,100,8,BLOCK_STONE,0); hs_raw(w,8,100,7,BLOCK_STONE,0);
		if(nested) {
			hs_raw(w,12,99,8,BLOCK_STONE,0); hs_raw(w,12,100,8,BLOCK_REDSTONE_WIRE,0);
			hs_raw(w,13,100,8,BLOCK_REDSTONE_TORCH_LIT,5); hs_raw(w,11,100,8,BLOCK_STONE,0);
		}
		blocks[BLOCK_STONE]=&copy;
		server_world_test_light_fault_hook=hs_fault;
		struct block_data b; assert(server_world_get_block(w,8,100,8,&b));
		struct block_info info={.block=&b,.x=8,.y=100,.z=8};
		redstone_server_wire_flood_diag_reset();
		redstone_wire_neighbour_change(s,&info);
		if(healthy) redstone_server_service_pending(s);
		if(!healthy) {
			unsigned visits=redstone_server_drain_visits();
			redstone_server_service_pending(s);
			check(redstone_server_drain_visits()==visits,"stopped retained redstone does not resume");
		}
	} else {
		/* Real full ring; a due callback frees exactly one slot. A pending
		 * button load scan must not take it following the nested failure. */
		struct block tick=*blocks[BLOCK_STONE_BUTTON];
		struct block* old=blocks[BLOCK_STONE_BUTTON];
		tick.onWorldTick=hs_scheduled; blocks[BLOCK_STONE_BUTTON]=&tick;
		hs_raw(w,8,100,8,BLOCK_STONE_BUTTON,9);
		if(nested) hs_raw(w,9,100,8,BLOCK_STONE_BUTTON,9);
		server_world_scheduled_ticks_destroy(w); server_world_scheduled_ticks_create(w,4);
		for(int i=0;i<4;i++) assert(server_world_schedule_tick_raw(w,8+i,100,8,BLOCK_STONE_BUTTON,i && !(nested && i==1)?1000:0));
		struct server_chunk* c=dict_server_chunks_get(w->chunks,S_CHUNK_ID(0,0)); assert(c);
		c->load_ticks_rearmed=false; c->load_tick_rearm_cursor=8*16*128+8*128+100;
		server_world_test_light_fault_hook=hs_fault;
		server_world_tick(w,s);
		blocks[BLOCK_STONE_BUTTON]=old;
	}
	block_world_leave(s,binding);
	hs_finish(name,healthy,previous);
	hs_peer_fault=hs_rs_active=hs_nested_rs=false;
	blocks[BLOCK_STONE]=hs_original[BLOCK_STONE];
	redstone_server_reset();
	if(slot || peer_fault) server_world_discard(&s->worlds[1]);
	close_world(s,s_dir[2],false); clin_server_publication_stop(false);
}
/* Healthy contact family and editing/no-op/origin witnesses share exactly the
 * same code for Q and F. No test expectations are derived from F. */
static void hs_healthy_contacts(void) {
	struct server_local* s=&S[2];
	open_world(s,s_dir[2],sizeof s_dir[2],"test_halt_contacts",true,1);
	hs_world=&s->worlds[0]; hs_callbacks=0; hs_events.n=0;
	server_local_test_seed_math_random(424242);
	for(int meta=0;meta<16;meta++) {
		struct server_world* w=hs_world;
		for(int dx=-1;dx<=1;dx++) for(int dz=-1;dz<=1;dz++) for(int dy=-1;dy<=1;dy++)
			hs_raw(w,8+dx,100+dy,8+dz,BLOCK_STONE,0);
		hs_raw(w,8,100,7,BLOCK_WATER_STILL,0);
		hs_raw(w,8,100,8,BLOCK_LAVA_STILL,meta);
		struct block_data b; assert(server_world_get_block(w,8,100,8,&b));
		struct block_info info={.block=&b,.x=8,.y=100,.z=8};
		uint32_t effects=fluid_block_lava_mix_effect_total();
		blocks[BLOCK_LAVA_STILL]->onNeighbourBlockChange(s,&info);
		assert(server_world_get_block(w,8,100,8,&b));
		check(b.type==(meta==0?BLOCK_OBSIDIAN:meta<=4?BLOCK_COBBLESTONE:BLOCK_LAVA_FLOW)
			&& fluid_block_lava_mix_effect_total()>effects,"healthy lava contact metadata 0/1..4/5..15");
		struct hs_bytes state={0}; hs_capture(&state);
		hs_write_tape("CONTACT",&state);
		free(state.p);
		assert(server_world_light_drain(w));
	}
	close_world(s,s_dir[2],false);
}
static void hs_healthy_admit(void) {
	struct server_local* s=&S[2];
	open_world(s,s_dir[2],sizeof s_dir[2],"test_halt_admit_tape",true,1);
	hs_world=&s->worlds[0]; hs_callbacks=0; hs_events.n=0;
	server_local_test_seed_math_random(424242);
	string_init_set_str(s->level_name,s_dir[2]); s->world_slot_state[0]=SERVER_WORLD_SLOT_LIVE;
	for(int i=0;i<2;i++) {
		struct server_world* w=server_local_world_admit(s,WORLD_DIM_NETHER); assert(w);
		jrand_set_seed(&w->rand,4242);
		check(!server_world_light_halted(w),"healthy admit/recreation remains live");
		struct hs_bytes state={0}; hs_capture(&state);
		hs_write_tape("SLOT-RECREATE",&state);
		free(state.p);
		/* Give best-effort release identical transport headroom in both runs.
		 * Do not let consumer scheduling decide whether its RPC is observed. */
		drain_discard_locked();
		while(!clin_rpc_flush_pending()) drain_discard_locked();
		drain_discard_locked();
		assert(clin_rpc_headroom(1));
		size_t before_release=hs_events.n;
		check(server_local_world_retire(s,w),"healthy real secondary retire");
		check(hs_events.n==before_release+40,"healthy retire observes stamped release RPC");
		state=(struct hs_bytes){0}; hs_capture(&state);
		hs_write_tape("SLOT-RETIRE",&state); free(state.p);
	}
	string_clear(s->level_name); close_world(s,s_dir[2],false);
}

/* Normal-lit-setter witnesses use only H-era APIs. A distinct callback host
 * catches guards consulting the implicit routed world instead of the owner. */
static void hs_boundary(const char* name, bool healthy, bool explicit_notify,
	int slot, unsigned child, bool standalone) {
	struct server_local* s=&S[2];
	open_world(s,s_dir[2],sizeof s_dir[2],"test_halt_boundary",true,1);
	struct server_world* w=&s->worlds[slot];
	struct server_world* independent=NULL;
	if(slot || standalone) {
		if(standalone) { independent=malloc(sizeof *independent); assert(independent); w=independent; }
		string_t level; string_init_set_str(level,s_dir[2]);
		server_world_create(w,level,WORLD_DIM_NETHER,4242,true); string_clear(level);
		w->local=s;
		if(!standalone) { w->session=s; w->world_index=slot; }
		for(int cx=-1;cx<=1;cx++) for(int cz=-1;cz<=1;cz++) {
			struct server_chunk* c=NULL; assert(server_world_load_chunk(w,cx,cz,&c));
		}
		assert(server_world_light_drain(w)); jrand_set_seed(&w->rand,4242);
	}
	hs_world=w; hs_wire=true; hs_inject=!healthy; hs_hit=false;
	hs_callbacks=0; hs_events.n=0; hs_fail_child=child;
	server_local_test_seed_math_random(424242);
	for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++) for(int dz=-1;dz<=1;dz++)
		hs_raw(w,8+dx,100+dy,8+dz,BLOCK_STONE,0);
	if(explicit_notify) hs_raw(w,8,100,8,BLOCK_COBBLESTONE,0);
	struct block copy=*blocks[BLOCK_STONE]; hs_original[BLOCK_STONE]=blocks[BLOCK_STONE];
	copy.onNeighbourBlockChange=hs_neighbour; blocks[BLOCK_STONE]=&copy;
	struct server_world* previous=s->block_world;
	server_world_test_light_fault_hook=hs_fault;
	if(explicit_notify) notifyNeighbours_from_w(s,w,8,100,8,BLOCK_WATER_FLOW);
	else assert(server_world_set_block_plain(w,8,100,8,BLOCK_COBBLESTONE));
	if(!healthy) {
		unsigned before=hs_callbacks;
		notifyNeighbours_from_w(&S[0],w,8,100,8,BLOCK_WATER_FLOW);
		(void)server_world_set_block_plain(w,8,100,8,BLOCK_STONE);
		check(before==hs_callbacks,"stopped boundary re-entry is inert");
	} else {
		unsigned before=hs_callbacks;
		/* World.editingBlocks suppresses D2 neighbour dispatch; explicit D1's
		 * existing policy stays unchanged and is recorded separately. */
		w->editing_blocks=true;
		assert(server_world_set_block_plain(w,8,100,8,BLOCK_STONE));
		check(before==hs_callbacks,"healthy editing suppresses D2 neighbours");
		w->editing_blocks=false;
		check(!server_world_set_block_plain(w,8,100,8,BLOCK_STONE),"healthy no-op setter");
	}
	hs_finish(name,healthy,previous); blocks[BLOCK_STONE]=hs_original[BLOCK_STONE];
	if(slot || standalone) server_world_discard(w);
	free(independent); close_world(s,s_dir[2],false); clin_server_publication_stop(false);
}

static void* hs_oom_alloc(size_t n) { (void)n; return NULL; }
static void hs_visit(int x,int y,int z) {
	hs_u64(&hs_events,UINT64_C(0x56495349));
	hs_u64(&hs_events,x); hs_u64(&hs_events,y); hs_u64(&hs_events,z);
}
static void hs_healthy_oom(void) {
	struct server_local* s=&S[2];
	open_world(s,s_dir[2],sizeof s_dir[2],"test_halt_wire_oom",true,1);
	hs_world=&s->worlds[0]; hs_callbacks=0; hs_events.n=0;
	server_local_test_seed_math_random(424242);
	for(int x=0;x<15;x++) for(int z=0;z<15;z++) {
		hs_raw(hs_world,x,99,z,BLOCK_STONE,0);
		hs_raw(hs_world,x,100,z,BLOCK_REDSTONE_WIRE,15);
	}
	hs_events.n=0; redstone_server_wire_flood_diag_reset();
	redstone_server_set_visit_hook(hs_visit);
	struct block_data b; assert(server_world_get_block(hs_world,7,100,7,&b));
	struct block_info info={.block=&b,.x=7,.y=100,.z=7};
	redstone_logic_set_alloc_hooks(hs_oom_alloc,free);
	redstone_wire_neighbour_change(s,&info);
	check(redstone_server_pending() && redstone_server_suspends(),"real wire allocation failure suspends healthy cascade");
	/* A second real entry first retries the old suspended flood, then queues
	 * its own request because allocation is still unavailable. */
	redstone_wire_neighbour_change(s,&info);
	check(redstone_server_request_ring_count()>0,"real OOM retry retains ordered request");
	struct hs_bytes state={0}; hs_capture(&state); hs_write_tape("WIRE-OOM-SUSPENDED",&state);
	redstone_logic_set_alloc_hooks(NULL,NULL);
	redstone_server_service_pending(s);
	check(!redstone_server_pending() && !server_world_light_halted(hs_world),"ordinary allocation suspension resumes without terminal stop");
	bool settled=true;
	for(int x=0;x<15;x++) for(int z=0;z<15;z++) {
		assert(server_world_get_block(hs_world,x,100,z,&b)); settled &= b.metadata==0;
	}
	check(settled,"healthy resumed wire fixpoint matches Beta unpowered zero");
	hs_capture(&state); hs_write_tape("WIRE-OOM-RESUMED",&state); free(state.p);
	redstone_server_set_visit_hook(NULL); redstone_server_reset(); close_world(s,s_dir[2],false);
}
static void hs_healthy_mixed_queue(void) {
	struct server_local* s=&S[2];
	open_world(s,s_dir[2],sizeof s_dir[2],"test_halt_mixed",true,1);
	hs_world=&s->worlds[0]; hs_callbacks=0; hs_events.n=0; hs_inject=false;
	server_local_test_seed_math_random(424242);
	for(int x=8;x<10;x++) {
		for(int dx=-1;dx<=1;dx++) for(int dz=-1;dz<=1;dz++)
			hs_raw(hs_world,x+dx,99,8+dz,BLOCK_STONE,0);
		hs_raw(hs_world,x,100,8,BLOCK_WATER_FLOW,0);
	}
	struct block copy=*blocks[BLOCK_STONE_BUTTON]; struct block* old=blocks[BLOCK_STONE_BUTTON];
	copy.onWorldTick=hs_scheduled; blocks[BLOCK_STONE_BUTTON]=&copy;
	hs_raw(hs_world,10,100,8,BLOCK_STONE_BUTTON,9);
	server_world_scheduled_ticks_destroy(hs_world); server_world_scheduled_ticks_create(hs_world,4);
	s->scheduled_tick_budget=1;
	assert(server_world_schedule_tick_raw(hs_world,8,100,8,BLOCK_WATER_FLOW,0));
	assert(server_world_schedule_tick_raw(hs_world,9,100,8,BLOCK_WATER_FLOW,0));
	assert(server_world_schedule_tick_raw(hs_world,10,100,8,BLOCK_STONE_BUTTON,0));
	assert(server_world_schedule_tick_raw(hs_world,11,100,8,BLOCK_STONE_BUTTON,1000));
	server_world_tick(hs_world,s);
	check(hs_callbacks==1 && !server_world_light_halted(hs_world),"full mixed queue fires due nonfluid behind capped fluid");
	struct hs_bytes state={0}; hs_capture(&state); hs_write_tape("MIXED-DEFERRED",&state);
	server_world_tick(hs_world,s);
	hs_capture(&state); hs_write_tape("MIXED-RESUMED",&state); free(state.p);
	blocks[BLOCK_STONE_BUTTON]=old; close_world(s,s_dir[2],false);
}

static unsigned hs_observers, hs_constructors;
static bool hs_expect_constructor_stop;
static struct server_local* hs_expected_owner;
static void hs_tail(struct server_world* w, bool reactivate) {
	(void)reactivate;
	if(server_world_simulation_stopped(w)) hs_tail_after_stop++;
}
static void hs_ordered_fault(struct server_world* w) {
	hs_observers++;
	check(server_world_simulation_stopped(w), "origin stopped before observer");
	if(w->session) {
		for(int i=0;i<SERVER_LOCAL_WORLD_SLOTS;i++)
			check(server_world_simulation_stopped(&w->session->worlds[i]), "peer/inactive slot stopped before observer");
		check(clin_server_publication_stopped(), "publication barrier before observer");
		/* Reentrant dispatch must observe the peer byte, even with no peer fault. */
		unsigned before=hs_callbacks;
		notifyNeighbours_from_w(w->session,&w->session->worlds[1-w->world_index],8,100,8,BLOCK_STONE);
		check(before==hs_callbacks,"reentrant peer dispatch is inert");
	}
}
static void hs_constructor(struct server_world* w) {
	hs_constructors++;
	check(w->session==hs_expected_owner && w->world_index==(hs_expected_owner?1:0), "constructor owner before exposure");
	check(server_world_simulation_stopped(w)==hs_expect_constructor_stop, "constructor inherits stop before exposure");
}
static void hs_authority(bool healthy, bool unused) {
	struct server_local* s=&S[2];
	open_world(s,s_dir[2],sizeof s_dir[2],"test_halt_authority",true,1);
	hs_world=&s->worlds[0]; hs_callbacks=0; hs_events.n=0;
	check(sizeof hs_world->simulation_stopped == 1,"PC simulation stop is one byte");
	printf("HALT PC stop field: size=%zu offset=%zu world_size=%zu\n", sizeof hs_world->simulation_stopped, offsetof(struct server_world,simulation_stopped),sizeof *hs_world);
	string_init_set_str(s->level_name,s_dir[2]);
	s->world_slot_state[0]=SERVER_WORLD_SLOT_LIVE;
	hs_expected_owner=s; hs_expect_constructor_stop=false; hs_constructors=0;
	server_world_test_create_hook=hs_constructor;
	struct server_world* peer=&s->worlds[1];
	if(!unused) {
		peer=server_local_world_admit(s,WORLD_DIM_NETHER);
		assert(peer && peer->session==s && peer->world_index==1);
		check(!server_world_simulation_stopped(peer),"healthy secondary admission is live");
		check(server_local_world_retire(s,peer),"healthy secondary retires through real owner");
	}
	if(!healthy) {
		hs_observers=0; server_world_test_light_fault_hook=hs_ordered_fault;
		hs_nested_write();
		check(hs_observers==1 && hs_world->light_fault && !peer->light_fault,
			"one origin, inactive slot is stopped without false fault attribution");
		server_world_stop_simulation(hs_world,SERVER_LIGHT_FAULT_PUBLICATION);
		check(hs_observers==1 && hs_world->light_fault_reason==SERVER_LIGHT_FAULT_RESERVE,
			"first cause and accounting are idempotent");
		hs_expect_constructor_stop=true;
	}
	peer=server_local_world_admit(s,WORLD_DIM_NETHER);
	assert(peer);
	check(server_world_simulation_stopped(peer)==!healthy,"recreated secondary preserves session stop after binding");
	if(!healthy) {
		check(!peer->light_fault && hs_world->light_fault_reason==SERVER_LIGHT_FAULT_RESERVE,
			"recreation preserves first origin");
		server_world_test_light_fault_hook=NULL;
		/* Production teardown and reconstruction of the ORIGIN must preserve
		 * terminal cause too; only full session initialization may clear it. */
		server_world_discard(hs_world);
		string_t level; string_init_set_str(level,s_dir[2]);
		server_world_test_create_hook=NULL;
		server_world_create_owned(hs_world,level,WORLD_DIM_OVERWORLD,4242,true,s,0);
		string_clear(level);
		check(server_world_simulation_stopped(hs_world) && hs_world->light_fault
			&& hs_world->light_fault_reason==SERVER_LIGHT_FAULT_RESERVE,"origin cause survives free/recreation");
	}
	server_world_test_create_hook=NULL;
	server_world_discard(peer); s->world_slot_state[1]=SERVER_WORLD_SLOT_EMPTY;
	/* Untouched inactive slot and arbitrary fresh standalone storage. */
	hs_expected_owner=NULL; hs_expect_constructor_stop=false;
	server_world_test_create_hook=hs_constructor;
	struct server_world* standalone=malloc(sizeof *standalone); assert(standalone);
	memset(standalone,0xa5,sizeof *standalone);
	string_t level; string_init_set_str(level,s_dir[2]);
	server_world_create(standalone,level,WORLD_DIM_OVERWORLD,4242,true);
	string_clear(level);
	check(!server_world_simulation_stopped(standalone) && !standalone->session,
		"fresh standalone ignores prior storage and stopped unrelated session");
	server_world_discard(standalone); free(standalone); server_world_test_create_hook=NULL;
	check(hs_constructors>=(unused?2u:3u),"constructor observers actually executed");
	string_clear(s->level_name);
	close_world(s,s_dir[2],false); clin_server_publication_stop(false);
	if(!healthy) {
		/* An existing terminal legacy latch must import at the real publisher,
		 * with its cause intact, before the notifying dispatcher can continue. */
		open_world(s,s_dir[2],sizeof s_dir[2],"test_halt_import",true,1);
		hs_world=&s->worlds[0];
		s->worlds[1].light_fault=true;
		s->worlds[1].light_fault_reason=SERVER_LIGHT_FAULT_DRAIN_OOM;
		hs_observers=0; server_world_test_light_fault_hook=hs_ordered_fault;
		/* Give the inactive legacy origin its correct initialized identity. */
		s->worlds[1].session=s; s->worlds[1].world_index=1;
		(void)server_world_set_block(hs_world,8,100,8,(struct block_data){.type=BLOCK_STONE});
		/* The reserving setter's slow containment path must import too. */
		server_local_test_update_once(s);
		check(hs_observers==1 && server_world_simulation_stopped(hs_world)
			&& s->worlds[1].light_fault_reason==SERVER_LIGHT_FAULT_DRAIN_OOM,
			"legacy terminal import fans out and preserves originating cause");
		server_world_test_light_fault_hook=NULL;
		close_world(s,s_dir[2],false); clin_server_publication_stop(false);
	}
}

/* Real output deadline, followed by owner construction before the existing
 * publication fault has acquired an origin. An inherited byte alone must not
 * suppress that first authority import or leave the inactive slot live. */
static void hs_publication_constructor(struct server_world* w) {
	bool all=true;
	for(int i=0;i<SERVER_LOCAL_WORLD_SLOTS;i++) all &= server_world_simulation_stopped(&w->session->worlds[i]);
	check(all && w->session->worlds[0].light_fault
		&& w->session->worlds[0].light_fault_reason==SERVER_LIGHT_FAULT_PUBLICATION,
		"publication authority owns every byte before constructor exposure");
}
static void hs_publication_inherit(void) {
	struct server_local* s=&S[2];
	open_world(s,s_dir[2],sizeof s_dir[2],"test_halt_publication",true,1);
	hs_world=&s->worlds[0]; hs_callbacks=0;
	server_world_discard(hs_world);
	clin_test_set_rpc_observer(NULL,NULL);
	drain_discard_locked();
	while(__atomic_exchange_n(&s_drain_busy,1,__ATOMIC_ACQUIRE)) thread_msleep(1);
	clin_test_fail_pending_alloc(true);
	struct client_rpc rpc={.type=CRPC_HUD_MESSAGE};
	unsigned admitted=0;
	while(clin_try_rpc_send(&rpc)) admitted++;
	assert(admitted);
	clin_test_set_rpc_exhaustion_timeout_ms(1000); /* shipping: 120 s (ci-1101/61) */
	clin_rpc_send(&rpc); /* actual retained overflow + one-second deadline */
	clin_test_set_rpc_exhaustion_timeout_ms(0);
	clin_test_fail_pending_alloc(false);
	check(clin_rpc_publication_faulted(),"real publication deadline reached before constructor");
	hs_observers=0; server_world_test_light_fault_hook=hs_ordered_fault;
	server_world_test_create_hook=hs_publication_constructor;
	string_t level; string_init_set_str(level,s_dir[2]);
	server_world_create_owned(hs_world,level,WORLD_DIM_OVERWORLD,4242,true,s,0);
	string_clear(level); hs_world->local=s;
	server_world_test_create_hook=NULL;
	check(server_world_simulation_stopped(hs_world) && hs_world->light_fault,
		"constructor inherits an imported publication authority");
	server_local_test_update_once(s);
	check(hs_observers==1 && hs_world->light_fault
		&& hs_world->light_fault_reason==SERVER_LIGHT_FAULT_PUBLICATION
		&& server_world_simulation_stopped(&s->worlds[1]),
		"inherited publication byte still imports first cause and peer stop");
	server_world_test_light_fault_hook=NULL;
	clin_rpc_pending_discard();
	__atomic_store_n(&s_drain_busy,0,__ATOMIC_RELEASE);
	close_world(s,s_dir[2],false); clin_server_publication_stop(false);
	clin_test_set_rpc_observer(hs_rpc,NULL);
}

/* A fan-out byte is not an own committed cause. Real generation regression
 * ps_run injects storage failures in both slots sequentially; this small API
 * witness also pins first-session attribution when slot 1 precedes slot 0. */
static void hs_peer_own_cause(void) {
	struct server_local* s=&S[2];
	open_world(s,s_dir[2],sizeof s_dir[2],"test_halt_peer_cause",true,1);
	struct server_world* peer=&s->worlds[1];
	string_t level; string_init_set_str(level,s_dir[2]);
	server_world_create(peer,level,WORLD_DIM_NETHER,4242,true); string_clear(level);
	peer->session=peer->local=s; peer->world_index=1;
	for(int cx=-1;cx<=1;cx++) for(int cz=-1;cz<=1;cz++) {
		struct server_chunk* c=NULL; assert(server_world_load_chunk(peer,cx,cz,&c));
	}
	assert(server_world_light_drain(peer));
	hs_world=peer; hs_peer_fault=false; hs_callbacks=0; hs_observers=0;
	server_world_test_light_fault_hook=hs_ordered_fault;
	hs_nested_write(); /* genuine first terminal reservation, slot 1 */
	check(s->worlds[0].simulation_stopped && !s->worlds[0].light_fault,
		"peer stop alone does not invent an originating fault");
	/* Exercise the authority API as a second committed producer. The large
	 * unchanged region-budget regression provides the real storage producer. */
	server_world_stop_simulation(&s->worlds[0],SERVER_LIGHT_FAULT_POPULATION_STORAGE);
	check(s->worlds[0].light_fault
		&& s->worlds[0].light_fault_reason==SERVER_LIGHT_FAULT_POPULATION_STORAGE
		&& peer->light_fault_reason==SERVER_LIGHT_FAULT_RESERVE && hs_observers==2,
		"stopped peer records its first genuine own cause once");
	server_world_stop_simulation(&s->worlds[0],SERVER_LIGHT_FAULT_PUBLICATION);
	check(hs_observers==2 && s->worlds[0].light_fault_reason==SERVER_LIGHT_FAULT_POPULATION_STORAGE,
		"second origin is idempotent without overwriting either first cause");
	server_local_test_update_once(s);
	check(s->light_stop_slot==1 && s->light_stop_published && s->load_failure_terminal,
		"first session origin survives later lower-index genuine fault");
	server_world_test_light_fault_hook=NULL;
	server_world_discard(peer); close_world(s,s_dir[2],false); clin_server_publication_stop(false);
}

static void hs_run(bool healthy) {
	server_local_sound_rng()->seed=0x12345;
	clin_test_set_rpc_observer(hs_rpc,NULL);
	const char* selected=getenv("CW_HALT_ONLY");
	const char* path=getenv("CW_HALT_TAPE");
	hs_tape=path && healthy ? fopen(path,"wb") : NULL;
	if(path && healthy) assert(hs_tape);
	const char* names[]={"SOL-R3","SOL-R4-DOWN","SOL-R4-HARDEN","RS-NESTED","SCHED-REARM"};
	for(int i=0;i<5;i++) {
		if(selected && strcmp(selected,"all") && strcmp(selected,names[i])) continue;
		if(i<3) for(int slot=0;slot<2;slot++) hs_fluid(names[i],healthy,i,slot);
		else for(int slot=0;slot<2;slot++) for(int peer=0;peer<2;peer++)
			for(int nested=0;nested<2;nested++)
				hs_dispatch(names[i],healthy,i==3,slot,peer,nested);
	}
	if(!selected || !strcmp(selected,"all") || !strcmp(selected,"LIT-BOUNDARY"))
		for(int d=0;d<2;d++) for(int slot=0;slot<2;slot++) for(int later=0;later<2;later++)
			hs_boundary(d?"D1-LIT":"D2-LIT",healthy,d,slot,later?4:1,false);
	if(!selected || !strcmp(selected,"all") || !strcmp(selected,"STANDALONE"))
		hs_boundary("D1-STANDALONE",healthy,true,0,1,true);
	if(!selected || !strcmp(selected,"all") || !strcmp(selected,"SLOT-RECREATE")) {
		if(healthy) hs_healthy_admit();
		else { hs_authority(false,false); hs_authority(false,true); hs_publication_inherit(); hs_peer_own_cause(); }
	}
	if(healthy && (!selected || !strcmp(selected,"all") || !strcmp(selected,"CONTACT"))) hs_healthy_contacts();
	if(healthy && (!selected || !strcmp(selected,"all") || !strcmp(selected,"WIRE-OOM"))) hs_healthy_oom();
	if(healthy && (!selected || !strcmp(selected,"all") || !strcmp(selected,"MIXED-QUEUE"))) hs_healthy_mixed_queue();
	if(hs_tape) { assert(!fclose(hs_tape)); hs_tape=NULL; }
	clin_test_set_rpc_observer(NULL,NULL);
	free(hs_events.p); memset(&hs_events,0,sizeof hs_events);
	free(hs_at_fault.p); memset(&hs_at_fault,0,sizeof hs_at_fault);
}

/* Beta MetadataChunkBlock:123: equal source light does not visit neighbours;
 * BlockFlowing:29-33 still publishes the phase swap. Actual quiet producer,
 * FIFO/local and direct-remote consumers; the unrelated unlit lava is stored
 * state, not a request to recompute the whole connected light field. */
static struct client_rpc cf_call;
static unsigned cf_published;
static void cf_observe(const struct client_rpc* call, void* unused) {
	(void)unused;
	if(call->type == CRPC_SET_BLOCK) { cf_call = *call; cf_published++; }
}
static void cf_cell(struct server_world* w, int x, int y, int z,
	struct block_data b) {
	struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0,0));
	assert(sc && sc->backing);
	unsigned i = (unsigned)y + ((unsigned)z + (unsigned)x * 16) * 128;
	column_backing_set_cell(sc->backing,COLUMN_IDS,i,b.type);
	column_backing_set_cell(sc->backing,COLUMN_META,i,b.metadata);
	column_backing_set_cell(sc->backing,COLUMN_SKY,i,b.sky_light);
	column_backing_set_cell(sc->backing,COLUMN_TORCH,i,b.torch_light);
	sc->heightmap[x + z*16] = 83;
	world_set_block(&gstate.world,x,y,z,b,false);
}
static void cf_capture(FILE* f) {
	if(!f) return;
	for(int x=0;x<16;x++) for(int z=0;z<16;z++) {
		uint8_t h=(uint8_t)world_get_height(&gstate.world,x,z); assert(fwrite(&h,1,1,f)==1);
		for(int y=0;y<128;y++) {
			struct block_data a=world_get_block(&gstate.world,x,y,z);
			uint8_t bytes[4]={a.type,a.metadata,a.sky_light,a.torch_light};
			assert(fwrite(bytes,1,4,f)==4);
		}
	}
}
static void case_client_fluid_noop(void) {
	const bool controls_only=getenv("CW_CLIENT_FLUID_CONTROLS_ONLY")!=NULL;
	const bool queue_only=getenv("CW_CLIENT_FLUID_QUEUE_ONLY")!=NULL;
	const char* path=getenv("CW_CLIENT_FLUID_TAPE");
	FILE* tape=path?fopen(path,"wb"):NULL; if(path) assert(tape);
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	for(int dim=0;dim<2;dim++) for(int remote=0;remote<2;remote++)
	for(int family=0;family<2;family++) for(int reverse=0;reverse<2;reverse++)
	for(int kind=controls_only?1:0;kind<14;kind++) {
		if(queue_only && (kind!=5 || family)) continue;
		if(controls_only && (kind==11 || kind==5)) continue;
		const int source_y=kind==12?95:80;
		printf("client fluid dim=%d remote=%d lava=%d reverse=%d kind=%d\n",dim,remote,family,reverse,kind);
		world_unload_all(&gstate.world);
		clin_process(&(struct client_rpc){.type=CRPC_WORLD_RESET,
			.payload.world_reset.dimension=dim?WORLD_DIM_NETHER:WORLD_DIM_OVERWORLD});
		open_world(&S[0],s_dir[0],sizeof s_dir[0],"test_client_fluid_noop",true,1);
		struct server_world* w=&S[0].worlds[0];
		w->dimension=gstate.world.dimension=dim?WORLD_DIM_NETHER:WORLD_DIM_OVERWORLD;
		b1_bind_client_world(w);
		for(int x=6;x<=10;x++) for(int z=6;z<=10;z++) for(int y=source_y-2;y<=source_y+(kind==12?0:2);y++)
			cf_cell(w,x,y,z,(struct block_data){.type=BLOCK_STONE});
		int base=family?BLOCK_LAVA_FLOW:BLOCK_WATER_FLOW;
		int before=base+reverse, after=base+1-reverse;
		struct block_data old={.type=before,.torch_light=family?15:0};
		if(kind==2) { old.torch_light=0; if(!family) old.sky_light=15; } /* emission/source not at fixed point */
		cf_cell(w,8,source_y,8,old);
		cf_cell(w,9,source_y,8,(struct block_data){.type=BLOCK_LAVA_FLOW});
		/* Incoming stored-light change, independently of optical identity. */
		if(kind==1) {
			struct server_chunk* sc=dict_server_chunks_get(w->chunks,S_CHUNK_ID(0,0));
			column_backing_set_cell(sc->backing,COLUMN_TORCH,80 + (8 + 8 * 16) * 128,family?0:1);
		}
		struct block* original=blocks[after]; struct block modified=*original;
		if(kind==3) { modified.opacity=family?14:4; blocks[after]=&modified; }
		if(kind==4) { modified.luminance=family?14:1; blocks[after]=&modified; }
		/* Existing genuine work remains in its original queue slot. */
		if(kind==5 || kind==6) world_set_block(&gstate.world,kind==6?8:7,source_y,8,
			(struct block_data){.type=BLOCK_TORCH},true);
		if(kind==11) lighting_flood_seed(&gstate.world.lighting_flood_queue,7,source_y,8,true);
		uint8_t frontier[64]={0};
		size_t frontier_n=stack_size(&gstate.world.lighting_flood_queue);
		assert(gstate.world.lighting_flood_queue.element_size <= sizeof frontier);
		if(frontier_n) memcpy(frontier,gstate.world.lighting_flood_queue.data,
			gstate.world.lighting_flood_queue.element_size);
		struct world_section* section=dict_wsection_get(gstate.world.sections,SECTION_TO_ID(0,0));
		if(kind==8) section->is_far_lod=true;
		if(kind==9) world_unload_all(&gstate.world);
		if(kind==13) section->heightmap[8+8*16]=80;
		size_t pending=stack_size(&gstate.world.lighting_updates);
		struct world_modification_entry saved={0};
		if(pending) stack_at(&gstate.world.lighting_updates,&saved,pending-1);
		cf_published=0; clin_test_set_rpc_observer(cf_observe,NULL);
		assert(server_world_set_block_quiet(w,8,source_y,8,
			(struct block_data){.type=after,.metadata=kind==7?1:0}));
		clin_test_set_rpc_observer(NULL,NULL);
		check(cf_published==1,"fluid phase swap retains actual SET_BLOCK publication");
		gstate.remote_session=remote!=0;
		if(remote) { clin_drain_discard(); clin_process_direct_remote(&cf_call); }
		else clin_update_drain();
		struct block_data installed=world_get_block(&gstate.world,8,source_y,8);
		check(installed.type==after && installed.metadata==(kind==7?1:0),
			"real consumer installs phase ID and metadata");
		const bool noop=kind==0 || kind==11 || kind==5;
		if(noop) check(stack_size(&gstate.world.lighting_updates)==pending,
			"Beta optical/source fixed-point phase swap adds no forced propagation");
		if(!noop && kind!=10) check(stack_size(&gstate.world.lighting_updates)==pending+1,
			"genuine change or unproven resident input retains forced seed");
		if(kind==8) section->is_far_lod=false;
		check(stack_size(&gstate.world.lighting_flood_queue)==frontier_n
			&& (!frontier_n || !memcmp(frontier,gstate.world.lighting_flood_queue.data,
			gstate.world.lighting_flood_queue.element_size)),"in-flight frontier is untouched");
		if(kind==10) {
			size_t queued=stack_size(&gstate.world.lighting_updates);
			struct client_rpc later=cf_call;
			later.payload.set_block.block=(struct block_data){.type=BLOCK_STONE};
			if(remote) clin_process_direct_remote(&later); else clin_process(&later);
			check(stack_size(&gstate.world.lighting_updates)==queued+1,
				"later genuine same-cell overwrite retains its forced seed");
		}
		if(pending) {
			struct world_modification_entry retained;
			stack_at(&gstate.world.lighting_updates,&retained,pending-1);
			check(!memcmp(&saved,&retained,sizeof saved),"pre-existing queued edit is retained in order");
		}
		pd_client_light_idle();
		if(noop && !(kind==5 && !family)) check(world_get_block(&gstate.world,9,source_y,8).torch_light==0,
			"phase no-op does not recompute unrelated stored unlit lava");
		if(kind==5) {
			check(world_get_block(&gstate.world,7,source_y,8).torch_light==14,
				"earlier queued torch still performs its deferred emissive change" );
			if(!family) check(world_get_block(&gstate.world,9,source_y,8).torch_light==15,
				"genuine queued torch propagates through water after the phase no-op" );
		}
		if(!noop || (kind==5 && !family)) cf_capture(tape);
		blocks[after]=original;
		gstate.remote_session=false;
		close_world(&S[0],s_dir[0],false);
	}
	world_destroy(&gstate.world);
	dict_entity_clear(gstate.entities);
	if(tape) assert(!fclose(tape));
}

struct test_case {
	const char* name;
	void (*run)(void);
	bool own_client; /* drives a real client: argv only, no discard thread */
};
static const struct test_case cases[] = {
	{"client_fluid_noop", case_client_fluid_noop, true},
	{"ordered_water", case_ordered_water},
	{"history", case_history, true}, {"history_capture", case_history_capture, false},
	{"history_off", case_history_off, true},
	{"generated_nether_beta", case_generated_nether_beta, false},
	{"nether_population_height_beta", case_nether_population_height_beta, false},
	{"automatic_population_beta", case_automatic_population_beta, false},
	{"nether_provider_rng_beta", case_nether_provider_rng_beta, false},
	{"generation_height_order_beta", case_generation_height_order_beta, false},
	{"automatic_population_hold", case_automatic_population_hold, false},
	{"history_capture_off", case_history_capture_off, false},
	{"nether_setter_capture", case_nether_setter_capture, false},
	{"world_rng_load32_capture", case_world_rng_load32_capture, false},
	{"nether_plain_rollback", case_nether_plain_rollback, false},
	{"nether_lava_runtime_capture", case_nether_lava_runtime_capture, false},
	{"generation_checkpoint_beta", case_generation_checkpoint_beta, false},
	{"g9x_callbacks", case_g9x_callbacks, false},
	{"g9x_dirty_reload", case_g9x_dirty_reload, false},
	{"g9x_snow_oom", case_g9x_snow_oom, false},
	{"g9x_observer_reuse", case_g9x_observer_reuse, false},
	{"g9x_region_budget", case_g9x_region_budget, false},
	{"g9x_memory_progress", case_g9x_memory_progress, false},
	{"g9x_drain_t1_equiv", case_g9x_drain_t1_equiv, false},
	{"equiv", case_equiv},		   {"explosion", case_explosion},
	{"fault", case_fault},		   {"lifecycle", case_lifecycle},
	{"provision", case_provision}, {"shutdown", case_shutdown},
	{"phases", case_phases},
	{"drain_loss", case_drain_loss}, {"drain_stop_update", case_drain_stop_update},
	{"contain", case_contain}, {"checkpoint", case_checkpoint},
	{"beta_oracle", case_beta_oracle}, {"consumers", case_consumers},
	{"generation_beta_parity", case_generation_beta_parity},
	{"generation_reconstruction_diagnostic", case_generation_reconstruction_diagnostic},
	{"workloads", case_workloads},
	{"hw_workload", case_hw_workload},
	{"packet_delivery", case_packet_delivery, true},
	{"packet_delivery_nether", case_packet_delivery_nether, true},
	{"g9x_publication", case_g9x_publication, true},
};

#include "support/g9_frozen_flood.h"
#include "platform/g9_probe.h"
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	/* R7: opt-in observer equivalence runs; every expected byte stays frozen. */
	const char* probe = getenv("CW_G9_PROBE");
	if(probe) g9_probe_configure(true, strcmp(probe, "on") == 0);
	const char* main_trace = getenv("CW_G9_MAIN_TRACE");
	if(main_trace && strcmp(main_trace, "1") == 0) {
		g9_probe_configure(true, false);
		g9_probe_main_configure(true);
	}
	if(argc > 2 && strcmp(argv[2], "frozen") == 0)
		server_world_test_light_flood_override = g9_frozen_kernel;
	assert(clin_init());
	assert(svin_init());
	bool own_client = false;
	for(size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
		own_client |= argc > 1 && cases[i].own_client
			&& strcmp(argv[1], cases[i].name) == 0;
	/* a discard consumer, unless the case drives the real client itself */
	struct thread drain;
	if(!own_client)
		assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	int ran = 0;
	for(size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		if(argc > 1 && strcmp(argv[1], cases[i].name) != 0)
			continue;
		if(argc <= 1 && cases[i].own_client)
			continue; /* argv-only (ctest runs it by name) */
		printf("case %s\n", cases[i].name);
		cases[i].run();
		ran++;
	}
	assert(ran > 0 && "unknown case");
	server_world_test_set_light_sync(false);
	server_world_test_set_light_lifo(false);
	if(s_fail || failures) {
		printf("test_light_queue_equiv: %d FAIL\n", s_fail + (int)failures);
		return 1;
	}
	printf("test_light_queue_equiv: PASS\n");
	return 0;
}
