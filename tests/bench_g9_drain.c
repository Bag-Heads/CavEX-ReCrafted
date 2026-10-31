/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * bench_g9_drain / test_g9_drain_equiv -- the I07 G9 light-drain worst case
 * (KANBAN lane 3, "BUG (I07): G9 light drain 0.2-1.1 s ticks on Wii").
 *
 * ONE harness, two targets (the bench_chunk_gather pattern):
 *   bench_g9_drain      -- production engine pools, NDEBUG: single-threaded
 *                          cost of the real server_world_light_drain on the
 *                          perf_stress_5m light_queue_stress workload
 *                          (FAILS-DIAG F1: i14 rt=31494/wt=529777, i14b
 *                          wt=1085289 us on the Wii; 16-torch grid toggled
 *                          every 20 ticks, 8x8 roof every 40).
 *   test_g9_drain_equiv -- test engine pools, asserts on: the byte-exact
 *                          GOLDEN of that workload. Every light write of
 *                          every drain is digested IN ORDER (x, y, z, value)
 *                          together with the pop/push/write counts and the
 *                          settled field of all 49 chunks, and compared with
 *                          the digest frozen from the pre-optimisation build
 *                          (a8383ad28 + the drain telemetry). A T1 change to
 *                          the drain's hot path must leave every digest, every
 *                          count and the settled field identical: same cells
 *                          popped in the same order, same values written in
 *                          the same order, same fixed point. Order is
 *                          observable here (the write trace), so it is pinned
 *                          too, as the lane brief requires.
 *
 * The workload reproduces light_queue_stress_drive (server_local.c) for the
 * two producers FAILS-DIAG attributes the spikes to, at the same offsets
 * from spawn (base = (px - 24, py + 12, pz + 8)): a 16x16 stone floor at
 * y - 1, the 4x4 torch grid at y (odd/even halves toggled every 20 ticks),
 * the 8x8 stone roof at y + 4 placed/removed every 40 ticks. The furnace,
 * piston, blast and checkpoint producers are left out on purpose: they are
 * the 200 / 40 / 1200 / 600-tick producers and FAILS-DIAG's spike cadence
 * (st 241, 261, 300, 341, ...) is the 20/40-tick one; torch toggles alone
 * cost ~380 ms on the Wii. Each tick's edits are drained by ONE prescribed
 * drain (MinecraftServer.java:346, drain to empty), exactly as production.
 */

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
#include "lighting.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/g9_drain_workload.h"
#include "platform/g9_probe.h"
#include "platform/g9_capture.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "sha256.h" /* tools/wii_stage_server/source */
#include "tps_observer.h"
#include "util.h"

#define RADIUS 3	 /* 7x7 resident chunks, as test_light_queue_equiv hw_workload */
#define TICKS 1200 /* one light_queue_stress period (blast cadence) */
#define LQ_STRESS_ROOF 40
#define LQ_STRESS_TORCH 20

static struct server_local s;
#ifdef CAVEWII_SERVER_LOCAL_TEST
#include "support/g9_frozen_flood.h"
#include "column_view.h"
static struct server_local frozen;
static char frozen_dir[220];
static unsigned s_trace_phase;
struct g9_bytes { uint32_t* data; size_t n, capacity; };
static struct g9_bytes s_write_bytes[2], s_pub_bytes[2];
static bool s_pair_armed;
static struct column_view* s_views[2];
static struct g9_bytes s_yield_bytes[2];
static unsigned s_probe_index;
static uint64_t s_mutations[2][5];
static uint64_t s_plane_cases[4];
static uint8_t s_view_bytes[2][COLUMN_VIEW_PRIVATE_BYTES];
static void export_view(struct column_view* view, uint8_t* bytes) {
	(void)column_view_copy(view, bytes, bytes + 32768, bytes + 49152, bytes + 65536);
}
static void trace_mutation(unsigned kind) {
	assert(kind < 5);
	s_mutations[s_trace_phase][kind]++;
}
static void append_bytes(struct g9_bytes*, const uint32_t*, size_t);
static void trace_yield_reads(void) {
	for(unsigned i = 0; i < 2; i++) {
		struct block_data b = column_view_get(s_views[s_trace_phase], s_probe_index + i * 4);
		uint32_t v[4] = {b.type, b.metadata, b.sky_light, b.torch_light};
		append_bytes(&s_yield_bytes[s_trace_phase], v, 4);
	}
}
static void append_bytes(struct g9_bytes* b, const uint32_t* v, size_t n) {
	if(b->n + n > b->capacity) {
		size_t cap = (b->n + n) * 2;
		void* p = realloc(b->data, cap * sizeof *b->data);
		assert(p); b->data = p; b->capacity = cap;
	}
	memcpy(b->data + b->n, v, n * sizeof *v); b->n += n;
}
static void trace_publication(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(!s_pair_armed) return;
	assert(rpc->type == CRPC_SET_BLOCK);
	const uint32_t v[8] = {rpc->type, rpc->payload.set_block.x,
		rpc->payload.set_block.y, rpc->payload.set_block.z,
		rpc->payload.set_block.block.type, rpc->payload.set_block.block.metadata,
		rpc->payload.set_block.block.sky_light, rpc->payload.set_block.block.torch_light};
	append_bytes(&s_pub_bytes[s_trace_phase], v, 8);
}
#endif
static char s_dir[220];
static int s_fail;

static void* drain_entry(void* arg) {
	(void)arg;
	while(1) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}

static void check(bool ok, const char* what) {
	printf("  %s: %s\n", ok ? "PASS" : "FAIL", what);
	if(!ok)
		s_fail++;
}

/* --- the write trace digest (test build only; the bench has no hook) --- */
static cw_sha256_ctx s_trace_sha;
static uint64_t s_trace_writes;
#ifdef CAVEWII_SERVER_LOCAL_TEST
static void trace_write(w_coord_t x, w_coord_t y, w_coord_t z, uint8_t light) {
	const int32_t v[4] = {(int32_t)x, (int32_t)y, (int32_t)z, (int32_t)light};
	if(s_trace_phase == 0) {
		cw_sha256_update(&s_trace_sha, v, sizeof v);
		s_trace_writes++;
	}
	if(s_pair_armed) append_bytes(&s_write_bytes[s_trace_phase], (const uint32_t*)v, 4);
}
/* Freeze the pre-R12 flood writer independently of server_chunk_store.
 * Both planes still call the original COW point writer, even if identical. */
static void frozen_write(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
	uint8_t light) {
	struct server_world* w = user;
	struct server_chunk* sc = dict_server_chunks_get(w->chunks,
		S_CHUNK_ID(WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z)));
	assert(sc);
	unsigned index = (W2C_COORD(x) * CHUNK_SIZE + W2C_COORD(z)) * WORLD_HEIGHT + y;
	bool sky = nibble_read(sc->lighting_sky, index) != (light & 15);
	bool torch = nibble_read(sc->lighting_torch, index) != (light >> 4);
	s_plane_cases[(sky ? 1 : 0) | (torch ? 2 : 0)]++;
	for(unsigned plane = COLUMN_SKY; plane <= COLUMN_TORCH; plane++) {
		uint8_t* data = plane == COLUMN_SKY ? sc->lighting_sky : sc->lighting_torch;
		uint8_t value = plane == COLUMN_SKY ? light & 15 : light >> 4;
		tps_mutation_callback census = __atomic_load_n(&world_tps_mutation, __ATOMIC_ACQUIRE);
		if(census && nibble_read(data, index) != value) census(2);
		if(sc->backing) column_backing_set_cell(sc->backing, plane, index, value);
		else nibble_write(data, index, value);
	}
	server_world_chunk_touch(sc);
	if(server_world_test_light_write_hook)
		server_world_test_light_write_hook(x, y, z, light);
}
static enum lighting_drain_result writer_frozen_kernel(
	struct stack* q, bool ignore_sky_light, size_t ceiling, size_t* pops,
	struct lighting_drain_stats* st,
	bool (*get_block)(void*, w_coord_t, w_coord_t, w_coord_t, struct block_data*, uint8_t*),
	bool (*get_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t*, uint8_t*),
	void (*set_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t), void* user) {
	(void)set_light;
	return g9_frozen_kernel(q, ignore_sky_light, ceiling, pops, st,
		get_block, get_light, frozen_write, user);
}
#endif

static void open_world_at(struct server_local* host, const char* id, char* dir) {
	test_fixture_dir(id, dir, 220);
	MKDIR(dir);
	char region[260];
	snprintf(region, sizeof region, "%s/region", dir);
	MKDIR(region);
	memset(host, 0, sizeof *host);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(host))
		abort();
	dict_entity_init(host->entities);
	string_t level;
	string_init_set_str(level, dir);
	server_world_create(&host->worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat */);
	string_clear(level);
	host->worlds[0].session = host;
	host->worlds[0].local = host;
	host->worlds[0].world_index = 0;
	host->scheduled_tick_budget = 128;
	host->rand_src.seed = 0x9E11u;
	host->world_time = 6000; /* noon: skylightSubtracted 0 */
	server_local_skylight_recompute(host);
	for(int cx = -RADIUS; cx <= RADIUS; cx++)
		for(int cz = -RADIUS; cz <= RADIUS; cz++) {
			struct server_chunk* sc = NULL;
			bool ok = server_world_load_chunk(&host->worlds[0], cx, cz, &sc);
			assert(ok && sc);
			(void)ok;
		}
}

static void open_world(const char* id) { open_world_at(&s, id, s_dir); }

static void close_world(void) {
	dict_entity_clear(s.entities);
	server_world_discard(&s.worlds[0]);
	test_fixture_dir_cleanup(s_dir);
}

/* The settled field of every resident chunk, digested: ids, meta, sky, block
 * light and the heightmap, column-major in chunk order. */
static void digest_field(char hex[65]) {
	cw_sha256_ctx c;
	cw_sha256_init(&c);
	for(int cx = -RADIUS; cx <= RADIUS; cx++)
		for(int cz = -RADIUS; cz <= RADIUS; cz++)
			for(int lx = 0; lx < 16; lx++)
				for(int lz = 0; lz < 16; lz++) {
					uint8_t h = 0;
					bool ok = server_world_get_height(&s.worlds[0], cx * 16 + lx,
													 cz * 16 + lz, &h);
					assert(ok);
					(void)ok;
					cw_sha256_update(&c, &h, 1);
					for(int y = 0; y < WORLD_HEIGHT; y++) {
						struct block_data b = {0};
						ok = server_world_get_block(&s.worlds[0], cx * 16 + lx, y,
													cz * 16 + lz, &b);
						assert(ok);
						const uint8_t v[4] = {b.type, b.metadata, b.sky_light,
											  b.torch_light};
						cw_sha256_update(&c, v, sizeof v);
					}
				}
	cw_sha256_final_hex(&c, hex);
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
static void compare_bytes(struct g9_bytes* a, struct g9_bytes* b) {
	assert(a->n == b->n);
	if(a->n) assert(!memcmp(a->data, b->data, a->n * sizeof *a->data));
	a->n = b->n = 0;
}
static void compare_worlds(void) {
	assert(!memcmp(s_mutations[0], s_mutations[1], sizeof s_mutations[0]));
	memset(s_mutations, 0, sizeof s_mutations);
	for(int cx = -RADIUS; cx <= RADIUS; cx++)
		for(int cz = -RADIUS; cz <= RADIUS; cz++) {
			struct server_chunk* ac = dict_server_chunks_get(s.worlds[0].chunks, S_CHUNK_ID(cx, cz));
			struct server_chunk* bc = dict_server_chunks_get(frozen.worlds[0].chunks, S_CHUNK_ID(cx, cz));
			assert(ac && bc);
			assert(!memcmp(ac->ids, bc->ids, 32768));
			assert(!memcmp(ac->metadata, bc->metadata, 16384));
			assert(!memcmp(ac->lighting_sky, bc->lighting_sky, 16384));
			assert(!memcmp(ac->lighting_torch, bc->lighting_torch, 16384));
			assert(!memcmp(ac->heightmap, bc->heightmap, 256));
			assert(ac->modified == bc->modified && ac->save_pending == bc->save_pending);
			assert(ac->persisted_tile_entities_complete == bc->persisted_tile_entities_complete);
			assert(ac->persisted_entities_nonempty == bc->persisted_entities_nonempty);
			assert(ac->a2w_pub_stage == bc->a2w_pub_stage && ac->a2w_sign_cursor == bc->a2w_sign_cursor);
		}
	assert(s.rand_src.seed == frozen.rand_src.seed);
	struct server_world* a = &s.worlds[0];
	struct server_world* b = &frozen.worlds[0];
	assert(a->light_frontier_live == b->light_frontier_live);
	assert(a->light_fault == b->light_fault && a->light_fault_reason == b->light_fault_reason);
	assert(a->light_seed_n == b->light_seed_n && a->light_undo_n == b->light_undo_n
		&& a->light_pin_n == b->light_pin_n);
	for(size_t i = 0; i < a->light_seed_n; i++) {
		assert(a->light_seeds[i].x == b->light_seeds[i].x
			&& a->light_seeds[i].y == b->light_seeds[i].y
			&& a->light_seeds[i].z == b->light_seeds[i].z);
	}
	for(size_t i = 0; i < a->light_undo_n; i++) {
		assert(a->light_undo[i].x == b->light_undo[i].x && a->light_undo[i].z == b->light_undo[i].z);
		assert(!memcmp(a->light_undo[i].sky, b->light_undo[i].sky, WORLD_HEIGHT / 2));
	}
	if(a->light_pin_n) assert(!memcmp(a->light_pins, b->light_pins, a->light_pin_n * sizeof *a->light_pins));
	assert(a->light_frontier.index == b->light_frontier.index);
	assert(a->light_frontier.length == b->light_frontier.length);
	for(size_t i = 0; i < a->light_frontier.index; i++) {
		struct g9_frozen_entry ae, be;
		stack_at(&a->light_frontier, &ae, i); stack_at(&b->light_frontier, &be, i);
		assert(ae.x == be.x && ae.y == be.y && ae.z == be.z && ae.forced == be.forced);
	}
}
static void mirror_drain(int tick, int y, const struct server_world_light_drain_rec* a) {
	s_trace_phase = 1;
	if(tick) g9_drain_edits(&frozen.worlds[0], y, tick);
	else g9_drain_floor(&frozen.worlds[0], y);
	server_world_test_light_flood_override = writer_frozen_kernel;
	assert(server_world_light_drain(&frozen.worlds[0]));
	server_world_test_light_flood_override = NULL;
	if(tick == 0 || tick % 20 == 0) {
		struct server_world_light_drain_rec b;
		server_world_light_drain_last(&b);
		assert(a->pops == b.pops && a->pushes == b.pushes && a->writes == b.writes);
		assert(a->seeds == b.seeds && a->depth_peak == b.depth_peak);
		if(tick && tick <= 80) printf("paired t=%d resolves %u/%u misses %u/%u (specialised/frozen)\n",
			tick, a->resolves, b.resolves, a->resolve_misses, b.resolve_misses);
	}
	compare_bytes(&s_write_bytes[0], &s_write_bytes[1]);
	compare_bytes(&s_pub_bytes[0], &s_pub_bytes[1]);
	compare_bytes(&s_yield_bytes[0], &s_yield_bytes[1]);
	for(unsigned k = 0; k < 2; k++) export_view(s_views[k], s_view_bytes[k]);
	assert(!memcmp(s_view_bytes[0], s_view_bytes[1], sizeof s_view_bytes[0]));
	compare_worlds();
	s_trace_phase = 0;
}
static void pair_edit(int type) {
	for(unsigned k = 0; k < 2; k++) {
		struct server_world* w = k ? &frozen.worlds[0] : &s.worlds[0];
		assert(server_world_set_block(w, 20, 80, 20,
			(struct block_data){.type = type, .metadata = type == BLOCK_TORCH ? 5 : 0}));
	}
}
static void pair_drain(bool want, bool oom, size_t ceiling) {
	struct server_world_light_drain_rec d[2];
	server_world_test_light_drain_ceiling(ceiling);
	for(unsigned k = 0; k < 2; k++) {
		server_world_test_light_flood_override = k ? writer_frozen_kernel : NULL;
		s_trace_phase = k;
		lighting_test_drain_fail_reserve_after(oom ? 1 : -1);
		assert(server_world_light_drain(k ? &frozen.worlds[0] : &s.worlds[0]) == want);
		server_world_light_drain_last(&d[k]);
	}
	lighting_test_drain_fail_reserve_after(-1);
	server_world_test_light_drain_ceiling(0);
	server_world_test_light_flood_override = NULL;
	s_trace_phase = 0;
	assert(d[0].pops == d[1].pops && d[0].pushes == d[1].pushes
		&& d[0].writes == d[1].writes && d[0].depth_peak == d[1].depth_peak);
	compare_worlds();
}
static void test_pair_lifecycle(void) {
	server_world_test_light_write_hook = NULL;
	pair_edit(BLOCK_STONE);
	/* Inject page-pool exhaustion into the REAL changing-light writer. The
	 * unchanged plane must remain harmless; snapshots survive private COW,
	 * rollback, stopped-state/retry and column retirement. */
	struct column_view_stats before, after;
	column_view_get_stats(&before);
	struct column_view* views[2];
	struct block_data old[2];
	unsigned index = (4 * CHUNK_SIZE + 4) * WORLD_HEIGHT + 80;
	for(unsigned k = 0; k < 2; k++) {
		struct server_world* w = k ? &frozen.worlds[0] : &s.worlds[0];
		struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(1, 1));
		views[k] = column_view_create(sc->backing, sc->ids, sc->metadata,
			sc->lighting_sky, sc->lighting_torch);
		assert(views[k]);
		old[k] = column_view_get(views[k], index);
	}
	column_view_test_page_limit(0);
	export_view(views[0], s_view_bytes[0]);
	for(unsigned k = 0; k < 2; k++) {
		server_world_test_light_flood_override = k ? writer_frozen_kernel : NULL;
		assert(server_world_save_chunk_coherent(k ? &frozen.worlds[0] : &s.worlds[0], 1, 1));
	}
	server_world_test_light_flood_override = NULL;
	compare_worlds(); /* checkpoint rollback leaves the original pending field */
	pair_drain(true, false, 0);
	for(unsigned k = 0; k < 2; k++) {
		struct server_world* w = k ? &frozen.worlds[0] : &s.worlds[0];
		assert(server_world_save_chunk(w, true, 1, 1));
		assert(!server_world_is_chunk_loaded(w, 1, 1));
		struct server_chunk* sc;
		assert(server_world_load_chunk(w, 1, 1, &sc));
	}
	compare_worlds();
	pair_edit(BLOCK_TORCH);
	pair_drain(false, false, 4); /* exact retained frontier at ceiling */
	server_world_test_light_clear_fault(&s.worlds[0]);
	server_world_test_light_clear_fault(&frozen.worlds[0]);
	pair_drain(true, false, 0);
	pair_edit(BLOCK_STONE);
	pair_drain(false, true, 0); /* failure after seed reserve, before lost pop */
	server_world_test_light_clear_fault(&s.worlds[0]);
	server_world_test_light_clear_fault(&frozen.worlds[0]);
	pair_drain(true, false, 0);
	column_view_get_stats(&after);
	assert(after.page_failures >= before.page_failures + 2);
	assert(after.fallback_total == before.fallback_total + 2);
	for(unsigned k = 0; k < 2; k++) {
		struct block_data b = column_view_get(views[k], index);
		assert(b.type == old[k].type && b.metadata == old[k].metadata
			&& b.sky_light == old[k].sky_light && b.torch_light == old[k].torch_light);
		export_view(views[k], s_view_bytes[1]);
		assert(!memcmp(s_view_bytes[0], s_view_bytes[1], sizeof s_view_bytes[0]));
		column_view_unref(views[k]);
	}
	column_view_test_page_limit(COLUMN_VIEW_POOL_PAGES);
	puts("PASS frozen paired rollback/save, unload/reload, ceiling/OOM stopped state and retry");
	server_world_test_light_write_hook = trace_write;
}

#endif

struct run_result {
	uint64_t pops, pushes, writes, drains, us_total;
	uint64_t kernel_hits, kernel_misses;
	uint32_t pops_max, us_max, us_max_pops, depth_max;
	uint32_t torch_us_max, roof_us_max; /* per producer tick (attribution) */
	uint32_t torch_pops_max, roof_pops_max;
	char trace_hex[65], field_hex[65];
};

static void run_workload(struct run_result* r, bool verbose) {
	memset(r, 0, sizeof *r);
	cw_sha256_init(&s_trace_sha);
	s_trace_writes = 0;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	s_trace_phase = 0;
	s_pair_armed = true;
	memset(s_mutations, 0, sizeof s_mutations);
	memset(s_plane_cases, 0, sizeof s_plane_cases);
	__atomic_store_n(&world_tps_mutation, trace_mutation, __ATOMIC_RELEASE);
	clin_test_set_rpc_observer(trace_publication, NULL);
	lighting_test_drain_yield_hook = trace_yield_reads;
	for(unsigned k = 0; k < 2; k++) {
		struct server_world* w = k ? &frozen.worlds[0] : &s.worlds[0];
		struct server_chunk* sc = dict_server_chunks_get(w->chunks, S_CHUNK_ID(-1, 1));
		s_views[k] = column_view_create(sc->backing, sc->ids, sc->metadata,
			sc->lighting_sky, sc->lighting_torch);
		assert(s_views[k]);
	}
#endif
	uint8_t hh = 0;
	bool ok = server_world_get_height(&s.worlds[0], 8, 8, &hh);
	assert(ok);
	(void)ok;
	const int y = hh + 12;
#ifdef CAVEWII_SERVER_LOCAL_TEST
	s_probe_index = (1 * 16 + 1) * 128 + y;
#endif
	g9_drain_floor(&s.worlds[0], y);
	server_world_light_drain(&s.worlds[0]);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	struct server_world_light_drain_rec floor;
	server_world_light_drain_last(&floor);
	mirror_drain(0, y, &floor);
#endif
	for(int t = 1; t <= TICKS; t++) {
		const bool roof = t % LQ_STRESS_ROOF == 0;
		const bool torch = t % LQ_STRESS_TORCH == 0;
		g9_drain_edits(&s.worlds[0], y, t);
		/* the prescribed boundary: one drain to empty per tick */
		server_world_perf_tick_reset(); /* the resolve-cache counters */
		const bool drained = server_world_light_drain(&s.worlds[0]);
		assert(drained);
		(void)drained;
		struct server_world_light_drain_rec d;
		server_world_light_drain_last(&d);
#ifdef CAVEWII_SERVER_LOCAL_TEST
		if(g9_probe_enabled() && (roof || torch)) {
			assert(d.kernel_resolve_hits + d.kernel_resolve_misses >= d.pops);
			assert(d.kernel_resolve_hits + d.kernel_resolve_misses
				<= 3 * (uint64_t)d.pops);
		} else if(!g9_probe_enabled()) {
			assert(d.kernel_resolve_hits == 0 && d.kernel_resolve_misses == 0);
		}
		mirror_drain(t, y, &d);
#endif
		server_world_perf_tick_reset();
		const int hit_pct = server_world_chunkres_hit_pct();
		if(!(roof || torch))
			continue;
#ifdef CAVEWII_SERVER_LOCAL_TEST
		assert(d.slices == d.pops / 256);
		assert((uint64_t)d.yield_us + d.lock_us + d.recovery_us <= d.us);
#endif
		r->drains++;
		r->pops += d.pops;
		r->pushes += d.pushes;
		r->writes += d.writes;
		r->us_total += d.us;
		r->kernel_hits += d.kernel_resolve_hits;
		r->kernel_misses += d.kernel_resolve_misses;
		if(d.pops > r->pops_max)
			r->pops_max = d.pops;
		if(d.us > r->us_max) { r->us_max = d.us; r->us_max_pops = d.pops; }
		if(d.depth_peak > r->depth_max)
			r->depth_max = d.depth_peak;
		if(roof) {
			if(d.us > r->roof_us_max) {
				r->roof_us_max = d.us; r->roof_pops_max = d.pops;
			}
		} else {
			if(d.us > r->torch_us_max) {
				r->torch_us_max = d.us; r->torch_pops_max = d.pops;
			}
		}
		if(verbose && t <= 80)
			printf("  t=%4d %-10s seeds %5u pops %7u pushes %7u writes %7u "
				   "depth %6u  %7u us  (%.2f us/pop)  chunk-resolve hit %d%%\n",
				   t, roof ? "roof+torch" : "torch", d.seeds, d.pops, d.pushes,
				   d.writes, d.depth_peak, d.us,
				   d.pops ? (double)d.us / (double)d.pops : 0.0, hit_pct);
	}
#ifdef CAVEWII_SERVER_LOCAL_TEST
	s_pair_armed = false;
	for(unsigned i = 0; i < 4; i++) assert(s_plane_cases[i] > 0);
	puts("PASS frozen writer SKY-only/TORCH-only/both/unchanged, mutation counts and full COW snapshots");
	__atomic_store_n(&world_tps_mutation, NULL, __ATOMIC_RELEASE);
	lighting_test_drain_yield_hook = NULL;
	column_view_unref(s_views[0]); column_view_unref(s_views[1]);
	clin_test_set_rpc_observer(NULL, NULL);
#endif
	cw_sha256_final_hex(&s_trace_sha, r->trace_hex);
	digest_field(r->field_hex);
}

/* FROZEN from the pre-optimisation build (lane codex/g9drain-1007, commit 1:
 * a8383ad28 + the per-drain telemetry, which adds counters only). Any
 * change here -- including an "improvement" -- is a semantic change to the
 * drain and needs its own review; a T1 optimisation leaves every line as
 * is. Seed 4242 superflat, 7x7 chunks, base (-16, h0+12, 16). */
#define GOLDEN_POPS 2209840ull
#define GOLDEN_PUSHES 2206968ull
#define GOLDEN_WRITES 367828ull
#define GOLDEN_TRACE_HEX "de383c29edbddacc9afd81b4636d94daa24f03b55483e1e73cb3afa0117467c2"
#define GOLDEN_FIELD_HEX "b3184402621da991082721a939dde0713b1b37ce0c7e3a2bf1825506cf1ac552"

#ifdef CAVEWII_SERVER_LOCAL_TEST
static struct thread_mutex s_wait_test_mutex;
static uint32_t s_wait_test_ready;
static void* wait_test_holder(void* unused) {
	(void)unused;
	thread_mutex_lock(&s_wait_test_mutex);
	__atomic_store_n(&s_wait_test_ready, 1, __ATOMIC_RELEASE);
	thread_msleep(20);
	/* A non-owner's recovery is not added to the drain owner's totals. */
	thread_drain_recovery_sleep();
	thread_mutex_unlock(&s_wait_test_mutex);
	return NULL;
}
static void test_wait_attribution(void) {
	assert(thread_mutex_init(&s_wait_test_mutex));
	struct thread holder;
	assert(thread_create(&holder, wait_test_holder, NULL, 64));
	while(!__atomic_load_n(&s_wait_test_ready, __ATOMIC_ACQUIRE)) thread_msleep(1);
	ptime_t start = time_get();
	thread_drain_wait_begin();
	thread_mutex_lock(&s_wait_test_mutex);
	thread_mutex_unlock(&s_wait_test_mutex);
	thread_drain_recovery_sleep();
	struct thread_drain_waits waits;
	thread_drain_wait_end(&waits);
	assert(waits.lock_us >= 1000 && waits.recovery_us >= 500);
	assert(waits.lock_us + waits.recovery_us <= time_diff_us(start, time_get()));
	assert(!thread_drain_wait_active());
	thread_join(&holder);
	thread_mutex_destroy(&s_wait_test_mutex);
}
#endif

static bool diag_print(const char* row) { printf("{%s\n", row); return true; }
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	test_wait_attribution();
#endif
	const bool capture = argc > 1 && strcmp(argv[1], "capture") == 0;
	bool capture_ok = g9_capture_configure(capture);
	assert(capture_ok);
	if(!capture_ok) return 1;
	if(capture) g9_capture_arm();
	const bool record = argc > 1 && strcmp(argv[1], "record") == 0;
	g9_probe_configure(argc > 1 && (strcmp(argv[1], "census") == 0
		|| strcmp(argv[1], "trace") == 0), false);
	g9_probe_main_configure(argc > 1 && strcmp(argv[1], "trace") == 0);
	const bool paced2 = argc > 1 && strcmp(argv[1], "paced2") == 0;
	thread_main_pacing_configure(paced2 || (argc > 1 && strcmp(argv[1], "paced") == 0),
		paced2 ? 500 : 2000, 1, paced2 ? 16 : 8);
	const int reps = argc > 1 && !record ? atoi(argv[1]) : 1;
	bool ok = clin_init();
	assert(ok);
	ok = svin_init();
	assert(ok);
	(void)ok;
	if(argc > 1 && strcmp(argv[1], "diagnostic") == 0) {
		blocks_init(); items_init();
		return server_world_g9_diagnostic(diag_print) ? 0 : 1;
	}
	struct thread drain;
	ok = thread_create(&drain, drain_entry, NULL, 64);
	assert(ok);
	blocks_init();
	items_init();
	/* lighting.c's LIGHT_SIDE_OFF table (T1) is designated by enum value
	 * with these six offsets; pin blocks_side_offset to the same answers so
	 * the two cannot drift apart silently. */
	{
		static const int want[SIDE_MAX][3] = {
			[SIDE_TOP] = {0, 1, 0},	   [SIDE_BOTTOM] = {0, -1, 0},
			[SIDE_LEFT] = {-1, 0, 0},  [SIDE_RIGHT] = {1, 0, 0},
			[SIDE_FRONT] = {0, 0, -1}, [SIDE_BACK] = {0, 0, 1},
		};
		for(enum side sd = 0; sd < SIDE_MAX; sd++) {
			int x, y, z;
			blocks_side_offset(sd, &x, &y, &z);
			if(x != want[sd][0] || y != want[sd][1] || z != want[sd][2]) {
				printf("FAIL: blocks_side_offset(%d) = (%d,%d,%d), table "
					   "(%d,%d,%d)\n", (int)sd, x, y, z, want[sd][0],
					   want[sd][1], want[sd][2]);
				return 1;
			}
		}
	}

#ifdef CAVEWII_SERVER_LOCAL_TEST
	server_world_test_light_write_hook = trace_write;
	const bool traced = true;
#else
	const bool traced = false;
#endif
	struct run_result best = {0}, r;
	for(int rep = 0; rep < (reps > 0 ? reps : 1); rep++) {
		open_world("bench_g9_drain");
#ifdef CAVEWII_SERVER_LOCAL_TEST
		open_world_at(&frozen, "bench_g9_drain_frozen", frozen_dir);
#endif
#ifdef CAVEWII_SERVER_LOCAL_TEST
		server_world_test_light_stats_reset();
#endif
		if(capture) g9_capture_tick_begin();
		run_workload(&r, rep == 0);
#ifdef CAVEWII_SERVER_LOCAL_TEST
		test_pair_lifecycle();
#endif
		if(capture) g9_capture_tick_end();
		close_world();
#ifdef CAVEWII_SERVER_LOCAL_TEST
		dict_entity_clear(frozen.entities);
		server_world_discard(&frozen.worlds[0]);
		test_fixture_dir_cleanup(frozen_dir);
#endif
		if(rep == 0 || r.us_total < best.us_total)
			best = r;
	}
	printf("g9 drain workload: %" PRIu64 " drains, %" PRIu64 " pops, %" PRIu64
		   " pushes, %" PRIu64 " writes (%s); frontier peak %u entries\n",
		   best.drains, best.pops, best.pushes, best.writes,
		   traced ? "traced" : "untraced", best.depth_max);
	printf("  time: total %" PRIu64 " us over the %d-tick period, max drain "
		   "%u us / %u pops; mean %.3f us/pop\n",
		   best.us_total, TICKS, best.us_max, best.us_max_pops,
		   best.pops ? (double)best.us_total / (double)best.pops : 0.0);
	printf("  attribution: torch-only tick max %u us / %u pops; roof+torch "
		   "tick max %u us / %u pops\n",
		   best.torch_us_max, best.torch_pops_max, best.roof_us_max,
		   best.roof_pops_max);
	printf("  kernel resolver census: hits %" PRIu64 " misses %" PRIu64 " (%s)\n",
		best.kernel_hits, best.kernel_misses, g9_probe_enabled() ? "armed" : "dormant");
	printf("  trace sha256 %s (%" PRIu64 " writes)\n  field sha256 %s\n",
		   best.trace_hex, s_trace_writes, best.field_hex);
	if(record) {
		printf("GOLDEN: pops %" PRIu64 " pushes %" PRIu64 " writes %" PRIu64
			   "\n  trace %s\n  field %s\n",
			   best.pops, best.pushes, best.writes, best.trace_hex,
			   best.field_hex);
		return 0;
	}
	if(!traced) {
		printf("bench_g9_drain: done (production engine, no trace)\n");
		return 0;
	}
	check(best.pops == GOLDEN_POPS && best.pushes == GOLDEN_PUSHES
			  && best.writes == GOLDEN_WRITES,
		  "pop / push / write counts equal the frozen golden");
	check(strcmp(best.trace_hex, GOLDEN_TRACE_HEX) == 0,
		  "the ordered (x, y, z, value) write trace digest equals the golden");
	check(strcmp(best.field_hex, GOLDEN_FIELD_HEX) == 0,
		  "the settled field digest (ids/meta/sky/block/height) equals the "
		  "golden");
	if(s_fail) {
		printf("test_g9_drain_equiv: %d FAIL\n", s_fail);
		return 1;
	}
	if(capture) {
		capture_ok = g9_capture_configure(false); /* test has no telemetry consumer */
		assert(capture_ok);
		puts("PASS bounded capture ON: frozen ordered writes/full fields/publication/dirty/RNG/COW/yield reads unchanged");
	}
	printf("test_g9_drain_equiv: PASS (full fields/write/publication/dirty/RNG/yield reads after every drain)\n");
	return 0;
}
