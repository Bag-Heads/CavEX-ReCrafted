/*
	GOLDEN (next-gaps-0924/G5, Astra route c): LOSSLESS redstone propagation inside the originating
	tick. Beta 1.7.3 BlockRedstoneWire.updateAndPropagateCurrentStrength (BlockRedstoneWire.java:45-55)
	runs the calculateCurrentChanges recursion (:57-171), collecting qualifying cells into the
	UNCAPPED HashSet blocksNeedingUpdate (:10, adds :160-168), then drains every position into
	World.notifyBlocksOfNeighborChange -- whose component handlers may set blocks that re-enter a
	NESTED updateAndPropagateCurrentStrength, synchronously, to any depth. Nothing is ever capped,
	dropped or deferred to a later tick.

	Pre-G5 (51f6beaf) three loss points existed; each scenario here is RED there:
	  A. marks sweep: the deferred-notify record array held 256 entries -- the 257th+ changed wire
	     cell never notified its components (and REPEATED marks of one cell consumed slots, so a
	     falling edge with <=256 distinct cells could also lose its tail).
	  B. deep support-removal cascade: the nesting guard SKIPPED every wire flood past depth 12.
	  C. worklist saturation (pure logic): the 4096-entry ring dropped re-queued links when full.
	Plus D (new API only): allocation failure suspends with all work retained, then resumes.
	E (new API only) pins the port's documented FIRST-INSERTION drain order of Beta's seven-position
	set (Java HashSet iteration order is not emulated -- docs/deviations.md). F: a suspended torch
	clock resumes on the next server tick. G: request-ring overflow + torch/repeater-tick resume.
	H: world destroy + create discards retained cascade work (dimension travel recreates the world).

	What is compared (never final state only): EVERY wire cell's metadata against the Beta fixpoint
	(strength = 15 - path distance from the powered cell, the decompile's per-cell rule), and the
	component timeline tick by tick -- a torch probe (BlockRedstoneTorch tickRate 2), a repeater
	probe (delay index 1 = 4 game ticks, field_22023_b {1,2,3,4}*2), and a piston probe (Beta
	BlockPistonBase.onNeighborBlockChange -> updatePistonState is synchronous: the extended bit
	flips in the SAME tick). The probes sit exactly two hops from their tail wire and >= 3 from
	every other wire, so ONLY the tail's deferred notify can reach them.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#include "block/redstone_logic.h"
#include "block/redstone_piston.h"
#include "block/redstone_piston_server.h"
#include "block/redstone_server.h"
#include "entity/entity.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100
#define Y 101
#define REP_META (2 | (1 << 2)) // facing 2 = input from -Z, output +Z; delay index 1 = 4 ticks
#define REP_DELAY 4
#define PISTON_FACE_NZ 2

static struct server_local s;
static int g_failures = 0;
#define CHECK(cond, ...)                                                                           \
	do {                                                                                           \
		if(!(cond)) {                                                                              \
			g_failures++;                                                                          \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
			fprintf(stderr, __VA_ARGS__);                                                          \
			fprintf(stderr, "\n");                                                                 \
		}                                                                                          \
	} while(0)

static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		// Spin (no 1 ms sleep): the CRPC inbox holds 16 tokens, so a sleeping consumer throttles
		// every wire write to the OS timer and the edge latency would measure the harness, not
		// the redstone cascade.
		clin_drain_discard();
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type, int meta) {
	server_world_set_block(&s.worlds[0], x, y, z, (struct block_data) {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)});
}
static void set_quiet(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block_silent(&s.worlds[0], x, y, z, (struct block_data) {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)}));
}
static int get(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.type : -1;
}
static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? (b.metadata & 0x0F) : -1;
}
static double g_worst_tick_ms = 0.0;
static void tick(void) {
	clock_t t0 = clock();
	server_world_tick(&s.world, &s);
	piston_moving_te_tick(&s);
	double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
	if(ms > g_worst_tick_ms)
		g_worst_tick_ms = ms;
}
// Time a synchronous edge (the originating tick's work happens inside set()).
static double g_worst_edge_ms = 0.0, g_last_edge_ms = 0.0;
static void timed_set(int x, int y, int z, int type, int meta) {
	clock_t t0 = clock();
	set(x, y, z, type, meta);
	double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
	g_last_edge_ms = ms;
	if(ms > g_worst_edge_ms)
		g_worst_edge_ms = ms;
}

// ================================ A. marks sweep (field fixture) =================================
// Plane cells (dx,dz) relative to the origin wire O=(ox,Y,oz). Source torch at (-1,0) powers only O.
// Three spokes (+x, +z, -z) of length 13 end in three tails at path distance 14 (strength 1); the
// filler is a BFS prefix (path distance <= 13) so the tails are the LAST three changed cells.
#define R 20
#define GW (2 * R + 1)
static int8_t g_in[GW][GW];	  // 1 = wire in the fixture
static int16_t g_dist[GW][GW]; // BFS path distance from O within the fixture (-1 unreachable)
#define IDX(d) ((d) + R)

static bool forbidden(int dx, int dz) {
	// the source torch cell and its other three neighbours (they would also be powered)
	return (dx == -1 && dz == 0) || (dx == -2 && dz == 0) || (dx == -1 && (dz == 1 || dz == -1));
}
static bool is_spoke(int dx, int dz) {
	return (dz == 0 && dx >= 0 && dx <= 13) || (dx == 0 && dz >= -13 && dz <= 13);
}
static void bfs(int8_t in[GW][GW], int16_t dist[GW][GW]) {
	static int qx[GW * GW], qz[GW * GW];
	for(int i = 0; i < GW; i++)
		for(int j = 0; j < GW; j++)
			dist[i][j] = -1;
	int h = 0, t = 0;
	dist[IDX(0)][IDX(0)] = 0;
	qx[t] = 0, qz[t] = 0, t++;
	while(h < t) {
		int x = qx[h], z = qz[h];
		h++;
		static const int ddx[4] = {-1, 1, 0, 0}, ddz[4] = {0, 0, -1, 1};
		for(int k = 0; k < 4; k++) {
			int nx = x + ddx[k], nz = z + ddz[k];
			if(nx < -R || nx > R || nz < -R || nz > R || !in[IDX(nx)][IDX(nz)])
				continue;
			if(dist[IDX(nx)][IDX(nz)] >= 0)
				continue;
			dist[IDX(nx)][IDX(nz)] = (int16_t)(dist[IDX(x)][IDX(z)] + 1);
			qx[t] = nx, qz[t] = nz, t++;
		}
	}
}
// Build the fixture cell set with exactly n wires (incl. 3 tails). Returns false if impossible.
static bool build_field(int n) {
	static int8_t uni[GW][GW];
	static int16_t ud[GW][GW];
	memset(uni, 0, sizeof uni);
	for(int dx = -R; dx <= R; dx++)
		for(int dz = -R; dz <= R; dz++)
			uni[IDX(dx)][IDX(dz)] = !forbidden(dx, dz) && abs(dx) + abs(dz) <= 13;
	bfs(uni, ud);
	memset(g_in, 0, sizeof g_in);
	int count = 0;
	for(int dx = -R; dx <= R; dx++)
		for(int dz = -R; dz <= R; dz++)
			if(is_spoke(dx, dz)) {
				g_in[IDX(dx)][IDX(dz)] = 1;
				count++;
			}
	g_in[IDX(14)][IDX(0)] = g_in[IDX(0)][IDX(14)] = g_in[IDX(0)][IDX(-14)] = 1;
	count += 3;
	// filler: BFS prefix by (distance, dx, dz), distance <= 13
	for(int d = 1; d <= 13 && count < n; d++)
		for(int dx = -R; dx <= R && count < n; dx++)
			for(int dz = -R; dz <= R && count < n; dz++)
				if(ud[IDX(dx)][IDX(dz)] == d && !g_in[IDX(dx)][IDX(dz)]) {
					g_in[IDX(dx)][IDX(dz)] = 1;
					count++;
				}
	if(count != n)
		return false;
	bfs(g_in, g_dist);
	for(int dx = -R; dx <= R; dx++)
		for(int dz = -R; dz <= R; dz++)
			if(g_in[IDX(dx)][IDX(dz)]) {
				int d = g_dist[IDX(dx)][IDX(dz)];
				bool tail = (dx == 14 && dz == 0) || (dx == 0 && (dz == 14 || dz == -14));
				assert(d >= 0 && (tail ? d == 14 : d <= 13)); // tails uniquely last
			}
	return true;
}

struct probes {
	int ox, oz;
};
static int torch_probe(const struct probes* p) { return get(p->ox + 16, Y, p->oz); }
static int rep_probe(const struct probes* p) { return get(p->ox, Y, p->oz + 16); }
static bool piston_extended(const struct probes* p) {
	return (getmeta(p->ox, Y, p->oz - 16) & 8) != 0 && get(p->ox, Y, p->oz - 16) == BLOCK_PISTON_BASE;
}

static int check_field_state(const struct probes* p, bool powered, const char* tag) {
	int bad = 0;
	for(int dx = -R; dx <= R; dx++)
		for(int dz = -R; dz <= R; dz++)
			if(g_in[IDX(dx)][IDX(dz)]) {
				int want = powered ? 15 - g_dist[IDX(dx)][IDX(dz)] : 0;
				int got = getmeta(p->ox + dx, Y, p->oz + dz);
				if(got != want && bad++ < 3)
					fprintf(stderr, "  [%s] wire (%d,%d) strength %d, Beta %d\n", tag, dx, dz, got, want);
			}
	CHECK(bad == 0, "[%s] %d wire cells differ from the Beta fixpoint", tag, bad);
	return bad;
}

static void run_field(int n, int slot) {
	char tag[64];
	struct probes p = {.ox = 0, .oz = slot * 48};
	if(!build_field(n)) {
		CHECK(false, "field n=%d cannot be built", n);
		return;
	}
	// floor + probes (quiet construction: the fixture starts settled and unpowered)
	for(int dx = -R; dx <= R; dx++)
		for(int dz = -R; dz <= R; dz++)
			set_quiet(p.ox + dx, YF, p.oz + dz, BLOCK_STONE, 0);
	for(int dx = -R; dx <= R; dx++)
		for(int dz = -R; dz <= R; dz++)
			if(g_in[IDX(dx)][IDX(dz)])
				set_quiet(p.ox + dx, Y, p.oz + dz, BLOCK_REDSTONE_WIRE, 0);
	set_quiet(p.ox + 15, Y, p.oz, BLOCK_STONE, 0);
	set_quiet(p.ox + 16, Y, p.oz, BLOCK_REDSTONE_TORCH_LIT, 1); // on the east face of (15,0)
	set_quiet(p.ox, Y, p.oz + 15, BLOCK_STONE, 0);
	set_quiet(p.ox, Y, p.oz + 16, BLOCK_REDSTONE_REPEATER, REP_META);
	set_quiet(p.ox, Y, p.oz - 15, BLOCK_STONE, 0);
	set_quiet(p.ox, Y, p.oz - 16, BLOCK_PISTON_BASE, PISTON_FACE_NZ);
	for(int k = 0; k < 3; k++)
		tick();
	CHECK(torch_probe(&p) == BLOCK_REDSTONE_TORCH_LIT && rep_probe(&p) == BLOCK_REDSTONE_REPEATER
			  && !piston_extended(&p),
		  "n=%d fixture not idle", n);

	// ---- RISING edge: the source torch lights the origin wire; every cell rises from 0 ----
	redstone_server_wire_flood_diag_reset();
	timed_set(p.ox - 1, Y, p.oz, BLOCK_REDSTONE_TORCH_LIT, 5);
	snprintf(tag, sizeof tag, "n=%d rise +0", n);
	check_field_state(&p, true, tag);
	// tick-by-tick component timeline (Beta: torch +2, repeater +4, piston same tick)
	const int torch_rise[7] = {BLOCK_REDSTONE_TORCH_LIT, BLOCK_REDSTONE_TORCH_LIT,
							   BLOCK_REDSTONE_TORCH, BLOCK_REDSTONE_TORCH, BLOCK_REDSTONE_TORCH,
							   BLOCK_REDSTONE_TORCH, BLOCK_REDSTONE_TORCH};
	for(int k = 0; k <= 6; k++) {
		if(k > 0)
			tick();
		int want_rep = k >= REP_DELAY ? BLOCK_REDSTONE_REPEATER_LIT : BLOCK_REDSTONE_REPEATER;
		CHECK(torch_probe(&p) == torch_rise[k], "n=%d rise +%d: torch probe %d, Beta %d", n, k,
			  torch_probe(&p), torch_rise[k]);
		CHECK(rep_probe(&p) == want_rep, "n=%d rise +%d: repeater probe %d, Beta %d", n, k,
			  rep_probe(&p), want_rep);
		CHECK(piston_extended(&p), "n=%d rise +%d: piston probe not extended (Beta: same tick)", n, k);
	}
#ifdef REDSTONE_WIRE_UPDATE_LOSSLESS
	printf("  field n=%d rise: drain visits=%u\n", n, (unsigned)redstone_server_drain_visits());
	printf("  field n=%d rise: edge %.1f ms marks_peak=%zu dedup=%u store_heap=%zu B worklist_peak=%zu\n", n,
		   g_last_edge_ms, redstone_server_marks_peak(), redstone_server_marks_dedup_hits(),
		   redstone_server_store_heap_peak_bytes(), redstone_wire_update_peak_entries());
#endif

	// ---- FALLING edge: remove the source; the whole field counts down to 0 in this tick ----
	redstone_server_wire_flood_diag_reset();
	timed_set(p.ox - 1, Y, p.oz, BLOCK_AIR, 0);
	snprintf(tag, sizeof tag, "n=%d fall +0", n);
	check_field_state(&p, false, tag);
	for(int k = 0; k <= 6; k++) {
		if(k > 0)
			tick();
		int want_torch = k >= 2 ? BLOCK_REDSTONE_TORCH_LIT : BLOCK_REDSTONE_TORCH;
		int want_rep = k >= REP_DELAY ? BLOCK_REDSTONE_REPEATER : BLOCK_REDSTONE_REPEATER_LIT;
		CHECK(torch_probe(&p) == want_torch, "n=%d fall +%d: torch probe %d, Beta %d", n, k,
			  torch_probe(&p), want_torch);
		CHECK(rep_probe(&p) == want_rep, "n=%d fall +%d: repeater probe %d, Beta %d", n, k,
			  rep_probe(&p), want_rep);
		CHECK(!piston_extended(&p), "n=%d fall +%d: piston probe still extended (Beta: same tick)", n,
			  k);
	}
#ifdef REDSTONE_WIRE_UPDATE_LOSSLESS
	printf("  field n=%d fall: edge %.1f ms marks_peak=%zu dedup=%u store_heap=%zu B worklist_peak=%zu\n", n,
		   g_last_edge_ms, redstone_server_marks_peak(), redstone_server_marks_dedup_hits(),
		   redstone_server_store_heap_peak_bytes(), redstone_wire_update_peak_entries());
	printf("  field n=%d fall: drain visits=%u\n", n, (unsigned)redstone_server_drain_visits());
	CHECK(redstone_server_store_heap_bytes() == 0, "n=%d: heap stores not released when idle", n);
#endif
}

// ============================ B. deep support-removal cascade ====================================
// A zig-zag chain: T0 (supported, the trigger) powers W1; floating torch T_i (mount = air) sits two
// hops from W_i and powers W_{i+1}. Removing T0 depowers W1 -> its drain drops T1 (Beta
// BlockTorch.onNeighborBlockChange: dropBlockAsItem + setBlockWithNotify(0)) -> W2 depowers in a
// NESTED flood -> T2 drops -> ... all synchronously inside the originating tick.
// ============================ chain drop ORDER (base == new) =====================================
// Beta's recursion drops T1, T2, ... strictly in chain order (each drop happens inside the previous
// stage's drain). dropBlockAsItem spawns the item BEFORE setBlockWithNotify, so the item-entity
// spawn order IS the drop order. Up to 12 stages the port recurses exactly like Beta (on 51f6beaf
// too); past 12 the continuation frames must keep the same order.
static uint32_t max_entity_id(void) {
	uint32_t m = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it); dict_entity_next(it))
		if(dict_entity_ref(it)->key > m)
			m = dict_entity_ref(it)->key;
	return m;
}
static void check_drop_order(int stages, int bx, int bz, uint32_t id0) {
	enum { MAXN = 128 };
	uint32_t ids[MAXN];
	int xs[MAXN], n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(dict_entity_ref(it)->key <= id0 || !e || n >= MAXN)
			continue;
		float ez = e->pos[2];
		if(ez < (float)(bz - 1) || ez > (float)(bz + 3))
			continue;
		ids[n] = dict_entity_ref(it)->key;
		xs[n] = (int)floorf(e->pos[0]) - bx;
		n++;
	}
	for(int i = 1; i < n; i++) // insertion sort by id
		for(int j = i; j > 0 && ids[j - 1] > ids[j]; j--) {
			uint32_t ti = ids[j];
			ids[j] = ids[j - 1];
			ids[j - 1] = ti;
			int tx = xs[j];
			xs[j] = xs[j - 1];
			xs[j - 1] = tx;
		}
	int bad = (n != stages);
	for(int i = 0; i < n && i < stages; i++)
		if(xs[i] != 2 * (i + 1))
			bad++;
	CHECK(bad == 0, "chain %d: drop order differs from Beta's T1..T%d sequence (%d drops seen)",
		  stages, stages, n);
}

static void chain_cell(int i, bool wire, int* x, int* z) {
	// W_i at x = 2i-1, z = (i odd ? 0 : 1); T_i at x = 2i, z = (i odd ? 1 : 0); T0 at (0,0)
	if(wire) {
		*x = 2 * i - 1;
		*z = (i & 1) ? 0 : 1;
	} else {
		*x = 2 * i;
		*z = i == 0 ? 0 : ((i & 1) ? 1 : 0);
	}
}
static void run_chain(int stages, int bx, int bz) {
	// floor under wires + T0 only; the chain torches float
	for(int i = 1; i <= stages; i++) {
		int x, z;
		chain_cell(i, true, &x, &z);
		set_quiet(bx + x, YF, bz + z, BLOCK_STONE, 0);
		set_quiet(bx + x, Y, bz + z, BLOCK_REDSTONE_WIRE, 15); // settled: powered by T_{i-1}
		chain_cell(i, false, &x, &z);
		set_quiet(bx + x, Y, bz + z, BLOCK_REDSTONE_TORCH_LIT, 5);
	}
	set_quiet(bx, YF, bz, BLOCK_STONE, 0);
	set_quiet(bx, Y, bz, BLOCK_REDSTONE_TORCH_LIT, 5);

	redstone_server_wire_flood_diag_reset();
	const uint32_t id0 = max_entity_id();
	timed_set(bx, Y, bz, BLOCK_AIR, 0); // the trigger
	check_drop_order(stages, bx, bz, id0);
	int wires_left = 0, torches_left = 0, first_bad = -1;
	for(int i = 1; i <= stages; i++) {
		int x, z;
		chain_cell(i, true, &x, &z);
		if(getmeta(bx + x, Y, bz + z) != 0) {
			wires_left++;
			if(first_bad < 0)
				first_bad = i;
		}
		chain_cell(i, false, &x, &z);
		if(get(bx + x, Y, bz + z) != BLOCK_AIR)
			torches_left++;
	}
	CHECK(wires_left == 0 && torches_left == 0,
		  "chain %d: Beta settles the whole cascade in the trigger tick; %d wires still powered, %d "
		  "torches still standing (first stranded stage %d)",
		  stages, wires_left, torches_left, first_bad);
	printf("  chain %d: edge %.1f ms depth_max=%d continuations=%d\n", stages, g_last_edge_ms,
		   redstone_server_wire_flood_depth_max(), redstone_server_wire_flood_depth_cap_hits());
	// no delayed work may still be pending: the state stays put for the following ticks
	for(int k = 1; k <= 3; k++) {
		tick();
		int x, z;
		chain_cell(stages, true, &x, &z);
		CHECK(getmeta(bx + x, Y, bz + z) == 0, "chain %d +%d: last wire re-powered", stages, k);
	}
#ifdef REDSTONE_WIRE_UPDATE_LOSSLESS
	printf("  chain %d: frames_peak=%zu store_heap=%zu B drain visits=%u\n", stages,
		   redstone_server_frames_peak(), redstone_server_store_heap_peak_bytes(),
		   (unsigned)redstone_server_drain_visits());
	CHECK(redstone_server_store_heap_bytes() == 0, "chain %d: heap stores not released", stages);
#endif
}

// ============================ C. worklist saturation (pure logic) ================================
// A 900x900 stub field where every cell is a source (flat 15) plus a two-wide gap holding
// single-parent "whisker" wires. The FIFO holds > 4096 pending entries mid-flood; each whisker is
// reachable only through its +X link from one parent, so a dropped re-queue strands it at 0.
// Beta's rule: whisker = 15 - 1 = 14, and it is a 0-crossing, so it must be marked for notify.
#define SW 900
#define SWY 64
static uint8_t* g_st;	 // 0 none, 1 source wire, 2 whisker wire
static uint8_t* g_sm;	 // metadata
static uint8_t* g_smark; // marked
#define SIDX(x, z) ((size_t)(x) * SW + (size_t)(z))
static bool s_in(int x, int y, int z) { return y == SWY && x >= 0 && x < SW && z >= 0 && z < SW; }
static int s_gt(void* c, int x, int y, int z) {
	(void)c;
	return s_in(x, y, z) && g_st[SIDX(x, z)] ? REDSTONE_WIRE_ID : 0;
}
static int s_gm(void* c, int x, int y, int z) {
	(void)c;
	return s_in(x, y, z) ? g_sm[SIDX(x, z)] : 0;
}
static void s_sm(void* c, int x, int y, int z, int m) {
	(void)c;
	if(s_in(x, y, z))
		g_sm[SIDX(x, z)] = (uint8_t)m;
}
static bool s_cube(void* c, int x, int y, int z) {
	(void)c, (void)x, (void)y, (void)z;
	return false;
}
static bool s_pow(void* c, int x, int y, int z) {
	(void)c;
	return s_in(x, y, z) && g_st[SIDX(x, z)] == 1;
}
static void s_mark(void* c, int x, int y, int z) {
	(void)c;
	if(s_in(x, y, z))
		g_smark[SIDX(x, z)] = 1;
}
static void run_saturation(void) {
	g_st = malloc((size_t)SW * SW);
	g_sm = calloc((size_t)SW * SW, 1);
	g_smark = calloc((size_t)SW * SW, 1);
	assert(g_st && g_sm && g_smark);
	memset(g_st, 1, (size_t)SW * SW);
	const int X0 = 300, Z1 = SW / 2 - 150, Z2 = SW / 2 + 150;
	for(int z = Z1; z < Z2; z++)
		g_st[SIDX(X0, z)] = g_st[SIDX(X0 + 1, z)] = 0;
	int whiskers = 0;
	for(int z = Z1 + 1; z < Z2 - 1; z += 2) {
		g_st[SIDX(X0, z)] = 2;
		whiskers++;
	}
	struct redstone_world w = {
		.get_type = s_gt,
		.get_meta = s_gm,
		.set_meta = s_sm,
		.is_normal_cube = s_cube,
		.is_indirectly_powered = s_pow,
		.mark_needs_update = s_mark,
	};
	redstone_wire_update_diag_reset();
	clock_t t0 = clock();
	redstone_wire_update(&w, SW / 2, SWY, SW / 2);
	double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
	int stranded = 0, unmarked = 0, sources_bad = 0;
	for(int z = Z1 + 1; z < Z2 - 1; z += 2) {
		if(g_sm[SIDX(X0, z)] != 14)
			stranded++;
		if(!g_smark[SIDX(X0, z)])
			unmarked++;
	}
	for(size_t i = 0; i < (size_t)SW * SW; i++)
		if(g_st[i] == 1 && (g_sm[i] != 15 || !g_smark[i]))
			sources_bad++;
	CHECK(redstone_wire_update_full_hits() == 0,
		  "saturation: %d re-queued links DROPPED by a full worklist", redstone_wire_update_full_hits());
	CHECK(stranded == 0 && unmarked == 0 && sources_bad == 0,
		  "saturation: %d/%d whiskers stranded (Beta 14), %d unmarked, %d source cells wrong", stranded,
		  whiskers, unmarked, sources_bad);
	printf("  saturation: %d whiskers, %.1f ms", whiskers, ms);
#ifdef REDSTONE_WIRE_UPDATE_LOSSLESS
	printf(", worklist peak %zu entries, peak heap %zu B, grows %u", redstone_wire_update_peak_entries(),
		   redstone_wire_update_peak_heap_bytes(), redstone_wire_update_grows());
#endif
	printf("\n");
	free(g_st);
	free(g_sm);
	free(g_smark);
#ifdef REDSTONE_WIRE_UPDATE_LOSSLESS
	CHECK(redstone_wire_update_heap_bytes() == 0, "saturation: grown worklist not released");
#endif
}

// ============================ D. allocation failure: suspend + resume ============================
#ifdef REDSTONE_WIRE_UPDATE_LOSSLESS
static bool g_fail_alloc = false;
static void* fail_alloc(size_t n) { return g_fail_alloc ? NULL : malloc(n); }
static void run_oom(void) {
	// A 100-stage chain needs more continuation frames than any earlier scenario grew the store to;
	// with every allocation failing the cascade must SUSPEND (not drop), then finish on service
	// once memory is back.
	const int stages = 100, bx = -10, bz = 470;
	for(int i = 1; i <= stages; i++) {
		int x, z;
		chain_cell(i, true, &x, &z);
		set_quiet(bx + x, YF, bz + z, BLOCK_STONE, 0);
		set_quiet(bx + x, Y, bz + z, BLOCK_REDSTONE_WIRE, 15);
		chain_cell(i, false, &x, &z);
		set_quiet(bx + x, Y, bz + z, BLOCK_REDSTONE_TORCH_LIT, 5);
	}
	set_quiet(bx, YF, bz, BLOCK_STONE, 0);
	set_quiet(bx, Y, bz, BLOCK_REDSTONE_TORCH_LIT, 5);
	unsigned susp0 = redstone_server_suspends();
	unsigned fails0 = redstone_alloc_failures();
	redstone_logic_set_alloc_hooks(fail_alloc, free);
	g_fail_alloc = true;
	set(bx, Y, bz, BLOCK_AIR, 0);
	bool pending = redstone_server_pending();
	unsigned susp = redstone_server_suspends() - susp0;
	unsigned fails = redstone_alloc_failures() - fails0;
	int x, z;
	chain_cell(stages, true, &x, &z);
	int last_before = getmeta(bx + x, Y, bz + z);
	printf("  oom: pending=%d suspends=%u alloc_failures=%u last_wire_before_service=%d\n",
		   (int)pending, susp, fails, last_before);
	CHECK(pending && susp >= 1 && fails >= 1 && last_before == 15,
		  "oom: a frame-store allocation failure must SUSPEND with the tail of the chain retained");
	g_fail_alloc = false;
	redstone_logic_set_alloc_hooks(NULL, NULL);
	redstone_server_service_pending(&s);
	CHECK(!redstone_server_pending(), "oom: work still pending after service");
	CHECK(redstone_server_requests_lost() == 0, "oom: %u requests lost", redstone_server_requests_lost());
	int left = 0;
	for(int i = 1; i <= stages; i++) {
		chain_cell(i, true, &x, &z);
		left += getmeta(bx + x, Y, bz + z) != 0;
		chain_cell(i, false, &x, &z);
		left += get(bx + x, Y, bz + z) != BLOCK_AIR;
	}
	CHECK(left == 0, "oom: %d chain cells not settled after resume", left);
	CHECK(redstone_server_store_heap_bytes() == 0, "oom: heap stores not released");
}

// Fresh-process variant of D run FIRST (before any store has grown): a field whose marks exceed
// the 256 static records, with every allocation failing -> suspension; then resume completes it.
static void run_oom_marks(int slot, int n) {
	struct probes p = {.ox = 0, .oz = slot * 48};
	assert(build_field(n));
	for(int dx = -R; dx <= R; dx++)
		for(int dz = -R; dz <= R; dz++)
			set_quiet(p.ox + dx, YF, p.oz + dz, BLOCK_STONE, 0);
	for(int dx = -R; dx <= R; dx++)
		for(int dz = -R; dz <= R; dz++)
			if(g_in[IDX(dx)][IDX(dz)])
				set_quiet(p.ox + dx, Y, p.oz + dz, BLOCK_REDSTONE_WIRE, 0);
	set_quiet(p.ox + 15, Y, p.oz, BLOCK_STONE, 0);
	set_quiet(p.ox + 16, Y, p.oz, BLOCK_REDSTONE_TORCH_LIT, 1);
	for(int k = 0; k < 3; k++)
		tick();
	redstone_logic_set_alloc_hooks(fail_alloc, free);
	g_fail_alloc = true;
	set(p.ox - 1, Y, p.oz, BLOCK_REDSTONE_TORCH_LIT, 5);
	CHECK(redstone_server_pending(), "oom-marks: n=%d with failing allocator must SUSPEND", n);
	CHECK(redstone_server_suspends() >= 1, "oom-marks: suspension not reported");
	g_fail_alloc = false;
	redstone_logic_set_alloc_hooks(NULL, NULL);
	// the next redstone entry (here: the explicit service hook) finishes the retained work
	redstone_server_service_pending(&s);
	CHECK(!redstone_server_pending(), "oom-marks: still pending after service");
	check_field_state(&p, true, "oom-marks resumed");
	// the tail's deferred notify survived the suspension: probe goes dark 2 ticks later
	tick();
	CHECK(torch_probe(&p) == BLOCK_REDSTONE_TORCH_LIT, "oom-marks +1: probe early");
	tick();
	CHECK(torch_probe(&p) == BLOCK_REDSTONE_TORCH, "oom-marks +2: probe never notified (lost)");
	set(p.ox - 1, Y, p.oz, BLOCK_AIR, 0);
	check_field_state(&p, false, "oom-marks fall");
	for(int k = 0; k < 3; k++)
		tick();
	CHECK(redstone_server_store_heap_bytes() == 0, "oom-marks: heap stores not released");
}
#endif


#ifdef REDSTONE_WIRE_UPDATE_LOSSLESS
// ============================ F4: Beta seven-position notify sequence ============================
// Two wires W1=(0,0), W2=(1,0) rise from a torch at (-1,0): the flood marks W1 then W2. Beta adds
// W1's seven positions (cell, -x, +x, -y, +y, -z, +z; BlockRedstoneWire.java:161-167), then W2's
// (its cell and -x are already present -- HashSet), and drains each DISTINCT position once through
// notifyBlocksOfNeighborChange -X,+X,-Y,+Y,-Z,+Z (World.java:498-505): 12 positions, 72 calls.
static int g_vis[256][3];
static int g_vis_n = 0;
static void visit_rec(int x, int y, int z) {
	if(g_vis_n < 256) {
		g_vis[g_vis_n][0] = x;
		g_vis[g_vis_n][1] = y;
		g_vis[g_vis_n][2] = z;
	}
	g_vis_n++;
}
static void run_visit_sequence(int bx, int bz) {
	set_quiet(bx - 1, YF, bz, BLOCK_STONE, 0);
	set_quiet(bx, YF, bz, BLOCK_STONE, 0);
	set_quiet(bx + 1, YF, bz, BLOCK_STONE, 0);
	set_quiet(bx, Y, bz, BLOCK_REDSTONE_WIRE, 0);
	set_quiet(bx + 1, Y, bz, BLOCK_REDSTONE_WIRE, 0);
	int exp[128][3], pos[16][3], np = 0, ne = 0;
	const int cells[2][3] = {{bx, Y, bz}, {bx + 1, Y, bz}};
	static const int ax[7] = {0, -1, 1, 0, 0, 0, 0}, ay[7] = {0, 0, 0, -1, 1, 0, 0},
					 az[7] = {0, 0, 0, 0, 0, -1, 1};
	for(int c = 0; c < 2; c++)
		for(int k = 0; k < 7; k++) {
			int px = cells[c][0] + ax[k], py = cells[c][1] + ay[k], pz = cells[c][2] + az[k];
			bool dup = false;
			for(int i = 0; i < np; i++)
				dup |= pos[i][0] == px && pos[i][1] == py && pos[i][2] == pz;
			if(!dup) {
				pos[np][0] = px, pos[np][1] = py, pos[np][2] = pz;
				np++;
			}
		}
	static const int nx[6] = {-1, 1, 0, 0, 0, 0}, ny[6] = {0, 0, -1, 1, 0, 0}, nz[6] = {0, 0, 0, 0, -1, 1};
	for(int i = 0; i < np; i++)
		for(int d = 0; d < 6; d++) {
			exp[ne][0] = pos[i][0] + nx[d], exp[ne][1] = pos[i][1] + ny[d], exp[ne][2] = pos[i][2] + nz[d];
			ne++;
		}
	g_vis_n = 0;
	redstone_server_set_visit_hook(visit_rec);
	set(bx - 1, Y, bz, BLOCK_REDSTONE_TORCH_LIT, 5);
	redstone_server_set_visit_hook(NULL);
	CHECK(getmeta(bx, Y, bz) == 15 && getmeta(bx + 1, Y, bz) == 14, "visit: wires not settled");
	int first_bad = -1;
	for(int i = 0; i < ne && i < g_vis_n; i++)
		if(first_bad < 0
		   && (g_vis[i][0] != exp[i][0] || g_vis[i][1] != exp[i][1] || g_vis[i][2] != exp[i][2]))
			first_bad = i;
	CHECK(np == 12 && ne == 72 && g_vis_n == ne && first_bad < 0,
		  "visit sequence: %d notifications (Beta %d over %d positions), first mismatch at %d", g_vis_n,
		  ne, np, first_bad);
	printf("  visit sequence: %d notifications over %d distinct positions (Beta order)\n", g_vis_n, np);
	CHECK(redstone_server_store_heap_bytes() == 0, "visit: heap store not released");
}

// ================================ F1: suspended torch CLOCK resumes ================================
// A free-running torch clock whose network is big enough that every toggle's cascade must grow the
// position store. An allocation failure suspends the cascade that holds the notify re-arming the
// torch; with no external event the next server tick (server_world_tick -> service) resumes it.
#define CLK_BX (-20)
#define CLK_BZ 530
static bool clk_is_wire(int dx, int dz) {
	static const int path[10][2] = {{0, 1}, {0, 2}, {-1, 2}, {-2, 2}, {-3, 2},
									{-4, 2}, {-4, 1}, {-4, 0}, {-3, 0}, {-2, 0}};
	for(int i = 0; i < 10; i++)
		if(path[i][0] == dx && path[i][1] == dz)
			return true;
	return dz >= 2 && abs(dx) + (dz - 1) <= 14; // the big side field (196 cells)
}
static int clk_torch(void) { return get(CLK_BX, Y, CLK_BZ); }
static bool clk_consistent(void) {
	// origin wire (0,1) powered <=> torch lit (the cascade is synchronous inside the tick)
	int m = getmeta(CLK_BX, Y, CLK_BZ + 1);
	return (clk_torch() == BLOCK_REDSTONE_TORCH_LIT) == (m > 0);
}
static void run_clock_resume(void) {
	for(int dx = -16; dx <= 16; dx++)
		for(int dz = -2; dz <= 16; dz++) {
			set_quiet(CLK_BX + dx, YF, CLK_BZ + dz, BLOCK_STONE, 0);
			if(clk_is_wire(dx, dz))
				set_quiet(CLK_BX + dx, Y, CLK_BZ + dz, BLOCK_REDSTONE_WIRE, 0);
		}
	set_quiet(CLK_BX - 1, Y, CLK_BZ, BLOCK_STONE, 0); // C, the torch mount
	set(CLK_BX, Y, CLK_BZ, BLOCK_REDSTONE_TORCH_LIT, 1); // kick: attached to C (-X)
	int toggles = 0, prev = clk_torch();
	for(int k = 0; k < 6; k++) {
		tick();
		CHECK(clk_consistent(), "clock +%d: wires disagree with the torch", k);
		toggles += clk_torch() != prev;
		prev = clk_torch();
	}
	CHECK(toggles >= 2, "clock: control run did not oscillate (%d toggles)", toggles);
	CHECK(redstone_server_store_heap_bytes() == 0, "clock: heap not released between cascades");
	redstone_logic_set_alloc_hooks(fail_alloc, free);
	g_fail_alloc = true;
	int k = 0;
	while(!redstone_server_pending() && k++ < 8)
		tick();
	CHECK(redstone_server_pending(), "clock: a toggle cascade with a failing allocator must suspend");
	g_fail_alloc = false;
	redstone_logic_set_alloc_hooks(NULL, NULL);
	// NO external event: only server ticks from here on
	toggles = 0;
	prev = clk_torch();
	for(int t = 0; t < 10; t++) {
		tick();
		CHECK(!redstone_server_pending(), "clock +%d after suspend: still pending", t);
		CHECK(clk_consistent(), "clock +%d after suspend: wires disagree with the torch", t);
		toggles += clk_torch() != prev;
		prev = clk_torch();
	}
	CHECK(toggles >= 3, "clock stalled after the suspension (%d toggles in 10 ticks)", toggles);
	CHECK(redstone_server_store_heap_bytes() == 0, "clock: heap not released after resume");
	printf("  clock: suspended mid-toggle, resumed by the next server tick, %d toggles after\n",
		   toggles);
}

// ============================ F5: request ring + resume via torch / repeater tick ==================
static void build_chain_quiet(int stages, int bx, int bz) {
	for(int i = 1; i <= stages; i++) {
		int x, z;
		chain_cell(i, true, &x, &z);
		set_quiet(bx + x, YF, bz + z, BLOCK_STONE, 0);
		set_quiet(bx + x, Y, bz + z, BLOCK_REDSTONE_WIRE, 15);
		chain_cell(i, false, &x, &z);
		set_quiet(bx + x, Y, bz + z, BLOCK_REDSTONE_TORCH_LIT, 5);
	}
	set_quiet(bx, YF, bz, BLOCK_STONE, 0);
	set_quiet(bx, Y, bz, BLOCK_REDSTONE_TORCH_LIT, 5);
}
static bool chain_settled(int stages, int bx, int bz) {
	for(int i = 1; i <= stages; i++) {
		int x, z;
		chain_cell(i, true, &x, &z);
		if(getmeta(bx + x, Y, bz + z) != 0)
			return false;
		chain_cell(i, false, &x, &z);
		if(get(bx + x, Y, bz + z) != BLOCK_AIR)
			return false;
	}
	return true;
}
static void suspend_chain(int stages, int bx, int bz) {
	build_chain_quiet(stages, bx, bz);
	redstone_logic_set_alloc_hooks(fail_alloc, free);
	g_fail_alloc = true;
	set(bx, Y, bz, BLOCK_AIR, 0);
	g_fail_alloc = false;
	redstone_logic_set_alloc_hooks(NULL, NULL);
	CHECK(redstone_server_pending(), "suspend_chain %d: expected a suspension", stages);
}
// A 29x9 wire strip lit from the middle of one long side: ~200 powered cells = more positions than
// the static store holds, so with a failing allocator the FLOOD itself suspends (flood_pending) and
// every retry fails again -- no progress is possible until memory returns.
#define STRIP_BX 20
#define STRIP_BZ 514
static void suspend_strip(void) {
	for(int dx = -14; dx <= 14; dx++)
		for(int dz = -1; dz <= 8; dz++) {
			set_quiet(STRIP_BX + dx, YF, STRIP_BZ + dz, BLOCK_STONE, 0);
			if(dz >= 0)
				set_quiet(STRIP_BX + dx, Y, STRIP_BZ + dz, BLOCK_REDSTONE_WIRE, 0);
		}
	redstone_logic_set_alloc_hooks(fail_alloc, free);
	g_fail_alloc = true;
	set(STRIP_BX, Y, STRIP_BZ - 1, BLOCK_REDSTONE_TORCH_LIT, 5);
	g_fail_alloc = false;
	redstone_logic_set_alloc_hooks(NULL, NULL);
	CHECK(redstone_server_pending() && redstone_wire_update_pending(),
		  "strip: expected the flood itself to suspend");
}
static bool strip_settled(void) {
	for(int dx = -14; dx <= 14; dx++)
		for(int dz = 0; dz <= 8; dz++) {
			int want = 15 - abs(dx) - dz;
			if(getmeta(STRIP_BX + dx, Y, STRIP_BZ + dz) != (want > 0 ? want : 0))
				return false;
		}
	return true;
}
static void run_ring_and_tick_resume(void) {
	const int stages = 30;
	// (1) ring: while suspended, wire changes queue; once 256 are waiting the next one is LOST
	suspend_strip();
	unsigned lost0 = redstone_server_requests_lost();
	unsigned queued0 = redstone_server_requests_queued();
	size_t free_slots = 256 - redstone_server_request_ring_count();
	struct block_data wb = {.type = BLOCK_REDSTONE_WIRE};
	struct block_info wi = {.block = &wb, .x = STRIP_BX + 3, .y = Y, .z = STRIP_BZ + 3};
	redstone_logic_set_alloc_hooks(fail_alloc, free);
	g_fail_alloc = true; // memory stays exhausted: every service attempt fails again
	for(size_t i = 0; i < free_slots + 1; i++)
		redstone_wire_neighbour_change(&s, &wi);
	g_fail_alloc = false;
	redstone_logic_set_alloc_hooks(NULL, NULL);
	CHECK(redstone_server_requests_lost() - lost0 == 1
			  && redstone_server_requests_queued() - queued0 == free_slots,
		  "ring: %zu free slots + 1 request must queue %zu and lose exactly one (queued %u lost %u)",
		  free_slots, free_slots, redstone_server_requests_queued() - queued0,
		  redstone_server_requests_lost() - lost0);
	// (2) resume from a TORCH tick entry (a standalone supported torch; no server tick)
	set_quiet(-14, YF, 520, BLOCK_STONE, 0);
	set_quiet(-14, Y, 520, BLOCK_REDSTONE_TORCH_LIT, 5);
	struct block_data tb = {.type = BLOCK_REDSTONE_TORCH_LIT, .metadata = 5};
	struct block_info ti = {.block = &tb, .x = -14, .y = Y, .z = 520};
	redstone_torch_tick(&s, &ti);
	CHECK(!redstone_server_pending() && strip_settled(),
		  "torch-tick entry did not resume the suspended cascade");
	CHECK(redstone_server_store_heap_bytes() == 0, "ring: heap not released");
	// (3) resume from a REPEATER tick entry
	suspend_chain(stages, -10, 510);
	set_quiet(-14, YF, 524, BLOCK_STONE, 0);
	set_quiet(-14, Y, 524, BLOCK_REDSTONE_REPEATER_LIT, REP_META);
	struct block_data rb = {.type = BLOCK_REDSTONE_REPEATER_LIT, .metadata = REP_META};
	struct block_info ri = {.block = &rb, .x = -14, .y = Y, .z = 524};
	redstone_repeater_server_tick(&s, &ri);
	CHECK(!redstone_server_pending() && chain_settled(stages, -10, 510),
		  "repeater-tick entry did not resume the suspended cascade");
	CHECK(redstone_server_store_heap_bytes() == 0, "repeater resume: heap not released");
	printf("  ring: %zu free slots + 1 -> exactly 1 lost; torch-tick and repeater-tick entries resume\n",
		   free_slots);
}
#endif

int main(void) {
	test_fixture_dir("test_redstone_lossless_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	piston_moving_store_init(&s.piston_moving);
	dict_entity_init(s.entities);
	rand_gen_seed(&s.rand_src);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 0; // unlimited: scheduled ticks fire exactly when due
	// fields: x -20..20, z -20..(7*48+20); chains + the OOM chain: x 0..81, z 420..478
	for(int cz = -2; cz <= 23; cz++)
		for(int cx = -2; cx <= 1; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}
	for(int cz = 25; cz <= 31; cz++)
		for(int cx = -1; cx <= 12; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}
	for(int cz = 32; cz <= 35; cz++) // ring/tick-resume chains, the clock, the visit fixture
		for(int cx = -3; cx <= 3; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	int slot = 0;
#ifdef REDSTONE_WIRE_UPDATE_LOSSLESS
	printf("D0. allocation failure with the static stores (first, before anything grew)\n");
	run_oom_marks(slot++, 300);
#endif
	printf("A. marks sweep (deferred-notify set)\n");
	const int sizes[] = {200, 255, 256, 257, 259, 300, 340};
	for(size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
		run_field(sizes[i], slot++);
	printf("B. deep support-removal cascade\n");
	const int chains[] = {11, 12, 13, 40};
	for(size_t i = 0; i < sizeof chains / sizeof chains[0]; i++)
		run_chain(chains[i], 0, 420 + 8 * (int)i);
	printf("C. worklist saturation\n");
	run_saturation();
#ifdef REDSTONE_WIRE_UPDATE_LOSSLESS
	printf("D. allocation failure inside a deep cascade\n");
	run_oom();
	printf("E. Beta seven-position notify sequence\n");
	run_visit_sequence(-30, 560);
	printf("F. suspended torch clock resumes on the next server tick\n");
	run_clock_resume();
	printf("G. request ring overflow + resume via torch/repeater tick\n");
	run_ring_and_tick_resume();
	printf("H. world destroy + create discards retained cascade work\n");
	suspend_chain(30, -10, 500);
	server_world_destroy(&s.world); // dimension travel does exactly this, then creates the new world
	CHECK(!redstone_server_pending() && redstone_server_store_heap_bytes() == 0,
		  "reset: retained cascade work survived server_world_destroy");
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	redstone_server_service_pending(&s); // nothing may replay into the new world
	CHECK(!redstone_server_pending() && redstone_server_store_heap_bytes() == 0,
		  "reset: pending work after world recreate");
	printf("  reset: suspended 30-stage cascade discarded at destroy; new world starts clean\n");
#endif
	printf("worst edge (originating-tick) latency %.2f ms, worst following tick %.2f ms (PC)\n",
		   g_worst_edge_ms, g_worst_tick_ms);

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	test_fixture_dir_cleanup(TMP_WORLD);
	if(g_failures) {
		printf("test_redstone_lossless_golden: FAILED (%d checks)\n", g_failures);
		return 1;
	}
	printf("test_redstone_lossless_golden: OK\n");
	return 0;
}
