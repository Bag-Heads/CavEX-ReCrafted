/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * test_g9_trial_kernel_equiv -- g9-trial-1009 (T1) byte-for-byte proof of
 * the server light kernel (server_light_kernel.inc) on the cases the
 * roof/torch golden (test_g9_drain_equiv) does not reach: y = 0 / 127 edge
 * cells, chunk seams next to NON-resident columns, opaque luminous blocks,
 * forced/removed sources, the Nether (sky ignored), the probe-census and
 * priority-lease kernel modes, and the coherent checkpoint TRIAL with its
 * rollback. Two identical worlds take the same seeded random edits; one
 * drains through the production kernel, the other through the frozen
 * bf92f3e33 flood (tests/support/g9_frozen_flood.h, the independent G9 R6
 * oracle) behind the same drain. After every drain and every checkpoint the
 * ordered (x, y, z, value) write traces, the pop/push/write/depth counts,
 * the retained frontier and every resident column's arrays must be equal.
 */
#include <assert.h>
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
#include "support/g9_frozen_flood.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "item/items.h"
#include "lighting.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/g9_probe.h"
#include "platform/thread.h"
#include "tps_observer.h"
#include "util.h"

#define R 2 /* columns -2..2, minus the holes below */
static const int HOLES[][2] = {{1, 0}, {-1, -1}, {2, 2}, {-2, 1}};
#define STEPS 220

static struct server_local hosts[2];
static char dirs[2][220];
static unsigned s_side; /* which world's writes are being traced */
struct trace { int32_t* v; size_t n, cap; };
static struct trace traces[2];
static uint32_t s_rng;
static unsigned s_checked_drains, s_checked_trials, s_edge_writes, s_seam_writes;

static uint32_t rnd(uint32_t n) {
	s_rng = s_rng * 1103515245u + 12345u;
	return (s_rng >> 8) % n;
}
static bool hole(int cx, int cz) {
	for(size_t k = 0; k < sizeof HOLES / sizeof HOLES[0]; k++)
		if(HOLES[k][0] == cx && HOLES[k][1] == cz) return true;
	return false;
}
static void trace_write(w_coord_t x, w_coord_t y, w_coord_t z, uint8_t light) {
	struct trace* t = &traces[s_side];
	if(t->n + 4 > t->cap) {
		t->cap = (t->n + 4) * 2;
		t->v = realloc(t->v, t->cap * sizeof *t->v);
		assert(t->v);
	}
	t->v[t->n++] = x; t->v[t->n++] = y; t->v[t->n++] = z; t->v[t->n++] = light;
	if(s_side == 0) {
		if(y == 0 || y == WORLD_HEIGHT - 1) s_edge_writes++;
		if(!W2C_COORD(x) || W2C_COORD(x) == 15 || !W2C_COORD(z) || W2C_COORD(z) == 15)
			s_seam_writes++;
	}
}
/* The frozen writer: both planes through the original point writer. */
static void frozen_write(void* user, w_coord_t x, w_coord_t y, w_coord_t z,
	uint8_t light) {
	struct server_world* w = user;
	struct server_chunk* sc = dict_server_chunks_get(w->chunks,
		S_CHUNK_ID(WCOORD_CHUNK_OFFSET(x), WCOORD_CHUNK_OFFSET(z)));
	assert(sc);
	unsigned index = (W2C_COORD(x) * CHUNK_SIZE + W2C_COORD(z)) * WORLD_HEIGHT + y;
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
static enum lighting_drain_result frozen_kernel(
	struct stack* q, bool ignore_sky_light, size_t ceiling, size_t* pops,
	struct lighting_drain_stats* st,
	bool (*get_block)(void*, w_coord_t, w_coord_t, w_coord_t, struct block_data*, uint8_t*),
	bool (*get_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t*, uint8_t*),
	void (*set_light)(void*, w_coord_t, w_coord_t, w_coord_t, uint8_t), void* user) {
	(void)set_light;
	return g9_frozen_kernel(q, ignore_sky_light, ceiling, pops, st,
		get_block, get_light, frozen_write, user);
}

static void open_host(unsigned k, enum world_dim dim, const char* id) {
	struct server_local* h = &hosts[k];
	test_fixture_dir(id, dirs[k], sizeof dirs[k]);
	MKDIR(dirs[k]);
	char region[260];
	snprintf(region, sizeof region, "%s/region", dirs[k]);
	MKDIR(region);
	memset(h, 0, sizeof *h);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(h))
		abort();
	dict_entity_init(h->entities);
	string_t level;
	string_init_set_str(level, dirs[k]);
	server_world_create(&h->worlds[0], level, dim, 4242, true);
	string_clear(level);
	h->worlds[0].session = h;
	h->worlds[0].local = h;
	h->worlds[0].world_index = 0;
	h->scheduled_tick_budget = 128;
	h->rand_src.seed = 0x9E11u;
	h->world_time = 6000;
	server_local_skylight_recompute(h);
	for(int cx = -R; cx <= R; cx++)
		for(int cz = -R; cz <= R; cz++) {
			if(hole(cx, cz)) continue;
			struct server_chunk* sc = NULL;
			bool ok = server_world_load_chunk(&h->worlds[0], cx, cz, &sc);
			assert(ok && sc);
			(void)ok;
		}
}
static void close_host(unsigned k) {
	dict_entity_clear(hosts[k].entities);
	server_world_discard(&hosts[k].worlds[0]);
	test_fixture_dir_cleanup(dirs[k]);
}

static void compare(void) {
	assert(traces[0].n == traces[1].n);
	assert(!traces[0].n || !memcmp(traces[0].v, traces[1].v, traces[0].n * sizeof(int32_t)));
	traces[0].n = traces[1].n = 0;
	struct server_world* a = &hosts[0].worlds[0];
	struct server_world* b = &hosts[1].worlds[0];
	for(int cx = -R; cx <= R; cx++)
		for(int cz = -R; cz <= R; cz++) {
			struct server_chunk* ac = dict_server_chunks_get(a->chunks, S_CHUNK_ID(cx, cz));
			struct server_chunk* bc = dict_server_chunks_get(b->chunks, S_CHUNK_ID(cx, cz));
			assert(!ac == !bc);
			assert(!ac == hole(cx, cz));
			if(!ac) continue;
			assert(!memcmp(ac->ids, bc->ids, 32768));
			assert(!memcmp(ac->metadata, bc->metadata, 16384));
			assert(!memcmp(ac->lighting_sky, bc->lighting_sky, 16384));
			assert(!memcmp(ac->lighting_torch, bc->lighting_torch, 16384));
			assert(!memcmp(ac->heightmap, bc->heightmap, 256));
			assert(ac->modified == bc->modified && ac->save_pending == bc->save_pending);
		}
	assert(a->light_frontier_live == b->light_frontier_live);
	assert(a->light_fault == b->light_fault);
	assert(a->light_seed_n == b->light_seed_n && a->light_undo_n == b->light_undo_n
		&& a->light_pin_n == b->light_pin_n);
	assert(a->light_frontier.index == b->light_frontier.index);
	for(size_t i = 0; i < a->light_frontier.index; i++) {
		struct g9_frozen_entry ae, be;
		stack_at(&a->light_frontier, &ae, i);
		stack_at(&b->light_frontier, &be, i);
		assert(ae.x == be.x && ae.y == be.y && ae.z == be.z && ae.forced == be.forced);
	}
}

static void drain_both(void) {
	struct server_world_light_drain_rec d[2];
	for(unsigned k = 0; k < 2; k++) {
		server_world_test_light_flood_override = k ? frozen_kernel : NULL;
		s_side = k;
		bool ok = server_world_light_drain(&hosts[k].worlds[0]);
		assert(ok);
		(void)ok;
		server_world_light_drain_last(&d[k]);
	}
	server_world_test_light_flood_override = NULL;
	s_side = 0;
	assert(d[0].seeds == d[1].seeds && d[0].pops == d[1].pops
		&& d[0].pushes == d[1].pushes && d[0].writes == d[1].writes
		&& d[0].depth_peak == d[1].depth_peak);
	compare();
	s_checked_drains++;
}
/* The coherent checkpoint trial (settle, save, roll back) on both worlds.
 * The production side runs the real trial (lazy page capture in the store
 * path); the frozen writer bypasses that store, so its world takes the
 * trial's eager whole-plane snapshot instead. Both must roll back to the
 * same pending field, and the trial's ordered writes must be equal. */
static void trial_both(int cx, int cz) {
	bool r[2];
	for(unsigned k = 0; k < 2; k++) {
		server_world_test_light_flood_override = k ? frozen_kernel : NULL;
		server_world_test_light_trial_eager(k == 1);
		s_side = k;
		r[k] = server_world_save_chunk_coherent(&hosts[k].worlds[0], cx, cz);
	}
	server_world_test_light_trial_eager(false);
	server_world_test_light_flood_override = NULL;
	s_side = 0;
	assert(r[0] == r[1]);
	compare();
	s_checked_trials++;
}

static void edit_both(int x, int y, int z, int type, int meta) {
	bool r[2];
	for(unsigned k = 0; k < 2; k++)
		r[k] = server_world_set_block(&hosts[k].worlds[0], x, y, z,
			(struct block_data){.type = type, .metadata = meta});
	assert(r[0] == r[1]);
}

static void run(enum world_dim dim, const char* name, uint32_t seed) {
	open_host(0, dim, "test_g9_trial_kernel_equiv_a");
	open_host(1, dim, "test_g9_trial_kernel_equiv_b");
	s_rng = seed;
	compare();
	static const int types[] = {BLOCK_TORCH, BLOCK_GLOWSTONE, BLOCK_STONE,
		BLOCK_AIR, BLOCK_GLASS, BLOCK_LEAVES, BLOCK_PUMPKIN_LIT,
		BLOCK_AIR, BLOCK_TORCH, BLOCK_GLOWSTONE};
	const unsigned drains0 = s_checked_drains, trials0 = s_checked_trials;
	for(unsigned step = 0; step < STEPS; step++) {
		const unsigned edits = 1 + rnd(6);
		int last_x = 0, last_z = 0;
		for(unsigned e = 0; e < edits; e++) {
			/* Bias towards the cases under test: the column seams (local 0,
			 * 1, 14, 15, next to holes too) and the y = 0 / 127 edge layers. */
			const int cx = (int)rnd(2 * R + 1) - R, cz = (int)rnd(2 * R + 1) - R;
			static const int locals[] = {0, 1, 7, 14, 15};
			const int lx = rnd(3) ? locals[rnd(5)] : (int)rnd(16);
			const int lz = rnd(3) ? locals[rnd(5)] : (int)rnd(16);
			static const int heights[] = {0, 1, 2, 3, 4, 63, 125, 126, 127};
			const int y = rnd(2) ? heights[rnd(9)] : (int)rnd(WORLD_HEIGHT);
			const int type = types[rnd(sizeof types / sizeof types[0])];
			last_x = cx * 16 + lx;
			last_z = cz * 16 + lz;
			edit_both(last_x, y, last_z, type, type == BLOCK_TORCH ? 5 : 0);
		}
		if(step % 9 == 4)
			trial_both(WCOORD_CHUNK_OFFSET(last_x), WCOORD_CHUNK_OFFSET(last_z));
		drain_both();
	}
	printf("  %s: %u paired drains, %u paired checkpoint trials\n", name,
		s_checked_drains - drains0, s_checked_trials - trials0);
	close_host(0);
	close_host(1);
}

static void* drain_entry(void* arg) {
	(void)arg;
	while(1) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	/* the arithmetic-shift chunk coordinate the kernel uses is
	 * WCOORD_CHUNK_OFFSET for every int32 (spot-check the edges) */
	static const int32_t xs[] = {0, 1, 15, 16, 17, -1, -15, -16, -17, -32,
		-33, 2147483647, -2147483647 - 1, 1000000007, -1000000007};
	for(size_t i = 0; i < sizeof xs / sizeof xs[0]; i++)
		assert((xs[i] >> 4) == WCOORD_CHUNK_OFFSET(xs[i]));
	for(int32_t x = -70000; x <= 70000; x++)
		assert((x >> 4) == WCOORD_CHUNK_OFFSET(x));
	bool ok = clin_init() && svin_init();
	assert(ok);
	struct thread drain;
	ok = thread_create(&drain, drain_entry, NULL, 64);
	assert(ok);
	(void)ok;
	blocks_init();
	items_init();
	server_world_test_light_write_hook = trace_write;

	run(WORLD_DIM_OVERWORLD, "overworld", 0x6a09e667u);
	run(WORLD_DIM_NETHER, "nether (sky ignored)", 0xbb67ae85u);
	g9_probe_configure(true, false); /* census kernel: per-pop resolve */
	assert(g9_probe_enabled());
	run(WORLD_DIM_OVERWORLD, "overworld, probe census", 0x3c6ef372u);
	g9_probe_configure(false, false);
	thread_g9_lease_configure(true, 8000, 500); /* per-write stripe locking */
	assert(thread_g9_lease_enabled());
	run(WORLD_DIM_OVERWORLD, "overworld, priority lease", 0xa54ff53au);
	thread_g9_lease_configure(false, 8000, 500);

	assert(s_edge_writes > 0 && s_seam_writes > 0);
	printf("  covered: %u y-edge writes, %u seam writes\n", s_edge_writes,
		s_seam_writes);
	printf("test_g9_trial_kernel_equiv: PASS (production kernel == frozen "
		   "flood: ordered writes, counts, frontier, all arrays)\n");
	return 0;
}
