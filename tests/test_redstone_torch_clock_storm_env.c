/*
	BUG-HUNT env test (2026-06-26): reproduce the real-Wii FREEZE that hits when a player builds a
	redstone CLOCK and rapidly PLACES + BREAKS redstone torches.

	On real Wii the game watchdog-froze (heartbeat stalled >3s) with NO corruption counter set
	(lfc/cnry/soom/dlbad all 0) -- a clean main-thread stall, NOT a GX/canary/stack/lighting-ceiling
	hit. Prime suspect: the redstone torch was just given an onNeighbourBlockChange handler
	(torch_neighbour_change, block_torch.c) that re-arms the inverter via
	server_world_schedule_block_tick on EVERY neighbour change -- a path the single-drop env test
	never exercised inside a live oscillating circuit / rapid place+break.

	Drives the REAL production paths (the torch's onItemPlace, server_world_set_block -> notify
	cascade, redstone_torch_tick inverter, the wire flood, scheduled-tick drain) over a live
	superflat server_world. The oscillator is a 3-stage NOT-gate RING (odd inverter ring = the
	classic Beta torch clock; never settles). The NOT-gate primitive is the verified one from
	test_redstone_torch_inverter_topologies.c: wire -> stone block B -> torch on top of B => B powered
	turns the torch OFF. Wiring an odd ring of these makes a free-running clock.

	Escalation:
	  1) place/break a redstone torch beside powered wire ~2000x.
	  2) run the oscillator thousands of ticks; ASSERT it actually toggles (no false-negative).
	  3) place+break torches WHILE the clock oscillates.

	WATCHDOG: every tick is wall-timed; >2s aborts with the phase label so a hang reports WHERE.
	Diagnostic repro, not a faithfulness golden.
*/

#include <assert.h>
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
#include "block/redstone_server.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "profile.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_rs_torch_clock_storm_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y  101 // circuit plane
// Diagnostic ceiling for ONE top-level notify cascade in this repro. This is not a timing
// proof; it catches unbounded synchronous handler fan-out on PC before hardware time is spent.
#define REDSTONE_REPRO_NOTIFY_DISPATCH_CAP 4096
#define REDSTONE_REPRO_WIRE_POP_CAP REDSTONE_WIRE_WORK_MAX

static struct server_local s;

static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type, int meta) {
	server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)});
}
static int get(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.type : -1;
}
static bool place_redstone_torch(int x, int y, int z);
static void break_block(int x, int y, int z);

static const char* g_phase = "init";
static void tick_wd(void) {
	clock_t t0 = clock();
	server_world_tick(&s.world, &s);
	double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
	if(ms > 2000.0) {
		fprintf(stderr,
				"WATCHDOG: single server_world_tick took %.0f ms in phase [%s] -- THIS IS THE "
				"FREEZE. scheduled_tick_count=%zu drops=%u\n",
				ms, g_phase, s.world.scheduled_tick_count,
				(unsigned)s.world.scheduled_tick_drops);
		abort();
	}
}

static void reset_redstone_work_bounds(void) {
	server_world_notify_diag_reset();
	blocks_notify_neighbours_diag_reset();
	redstone_server_wire_flood_diag_reset();
}

static void assert_redstone_work_bounds(const char* phase) {
	int sw_depth = server_world_notify_depth_max();
	int sw_hits = server_world_notify_depth_cap_hits();
	int sw_dispatch = server_world_notify_cascade_max_dispatches();
	int nn_depth = blocks_notify_neighbours_depth_max();
	int nn_hits = blocks_notify_neighbours_depth_cap_hits();
	int nn_dispatch = blocks_notify_neighbours_cascade_max_dispatches();
	int wire_depth = redstone_server_wire_flood_depth_max();
	int wire_hits = redstone_server_wire_flood_depth_cap_hits();
	int wire_pops = redstone_wire_update_max_pops();
	int wire_full = redstone_wire_update_full_hits();
	if(sw_depth > SERVER_WORLD_NOTIFY_MAX_DEPTH || sw_hits != 0
	   || sw_dispatch > REDSTONE_REPRO_NOTIFY_DISPATCH_CAP
	   || nn_depth > BLOCKS_NOTIFY_NEIGHBOURS_MAX_DEPTH || nn_hits != 0
	   || nn_dispatch > REDSTONE_REPRO_NOTIFY_DISPATCH_CAP
	   || wire_depth > RS_WIRE_FLOOD_MAX_DEPTH || wire_hits != 0
	   || wire_pops > REDSTONE_REPRO_WIRE_POP_CAP
	   || wire_full != 0
	   || s.world.scheduled_tick_drops != 0) {
		fprintf(stderr,
				"REDSTONE BOUND FAIL [%s]: sw_depth=%d/%d sw_hits=%d sw_dispatch=%d "
				"nn_depth=%d/%d nn_hits=%d nn_dispatch=%d wire_depth=%d/%d "
				"wire_hits=%d wire_pops=%d/%d wire_full=%d scheduled_drops=%u\n",
				phase, sw_depth, SERVER_WORLD_NOTIFY_MAX_DEPTH, sw_hits, sw_dispatch,
				nn_depth, BLOCKS_NOTIFY_NEIGHBOURS_MAX_DEPTH, nn_hits, nn_dispatch,
				wire_depth, RS_WIRE_FLOOD_MAX_DEPTH, wire_hits,
				wire_pops, REDSTONE_REPRO_WIRE_POP_CAP, wire_full,
				(unsigned)s.world.scheduled_tick_drops);
	}
	assert(sw_depth <= SERVER_WORLD_NOTIFY_MAX_DEPTH);
	assert(sw_hits == 0);
	assert(sw_dispatch <= REDSTONE_REPRO_NOTIFY_DISPATCH_CAP);
	assert(nn_depth <= BLOCKS_NOTIFY_NEIGHBOURS_MAX_DEPTH);
	assert(nn_hits == 0);
	assert(nn_dispatch <= REDSTONE_REPRO_NOTIFY_DISPATCH_CAP);
	assert(wire_depth <= RS_WIRE_FLOOD_MAX_DEPTH);
	assert(wire_hits == 0);
	assert(wire_pops <= REDSTONE_REPRO_WIRE_POP_CAP);
	assert(wire_full == 0);
	assert(s.world.scheduled_tick_drops == 0);
}

struct redstone_work_sample {
	int sw_dispatch;
	int nn_dispatch;
	int wire_pops;
	int wire_depth;
	int wire_full;
};

static struct redstone_work_sample sample_redstone_work(void) {
	return (struct redstone_work_sample) {
		.sw_dispatch = server_world_notify_cascade_max_dispatches(),
		.nn_dispatch = blocks_notify_neighbours_cascade_max_dispatches(),
		.wire_pops = redstone_wire_update_max_pops(),
		.wire_depth = redstone_server_wire_flood_depth_max(),
		.wire_full = redstone_wire_update_full_hits(),
	};
}

static void clear_circuit_area(void) {
	for(int x = 0; x <= 14; x++)
		for(int z = 0; z <= 14; z++) {
			set(x, Y, z, BLOCK_AIR, 0);
			set(x, Y + 1, z, BLOCK_AIR, 0);
		}
	for(int k = 0; k < 20; k++)
		tick_wd();
}

static struct redstone_work_sample run_scaled_inverter_bank(int copies,
		const struct redstone_work_sample* baseline) {
	assert(copies >= 1 && copies <= 4);
	clear_circuit_area();
	const int src_x = 1;
	const int wire_a = 2;
	const int wire_b = 3;
	const int post_x = 4;
	int prev[4] = {0};
	int toggles[4] = {0};

	for(int i = 0; i < copies; i++) {
		int z = 1 + i * 3;
		set(src_x, Y, z, BLOCK_REDSTONE_TORCH_LIT, 5);
		set(wire_a, Y, z, BLOCK_REDSTONE_WIRE, 0);
		set(wire_b, Y, z, BLOCK_REDSTONE_WIRE, 0);
		set(post_x, Y, z, BLOCK_STONE, 0);
		set(post_x, Y + 1, z, BLOCK_REDSTONE_TORCH_LIT, 5);
	}
	for(int k = 0; k < 30; k++)
		tick_wd();
	for(int i = 0; i < copies; i++)
		prev[i] = get(post_x, Y + 1, 1 + i * 3);

	reset_redstone_work_bounds();
	for(int cyc = 0; cyc < 60; cyc++) {
		for(int i = 0; i < copies; i++) {
			int z = 1 + i * 3;
			break_block(src_x, Y, z);
			for(int k = 0; k < 4; k++) {
				tick_wd();
				int cur = get(post_x, Y + 1, z);
				if(cur != prev[i]) {
					toggles[i]++;
					prev[i] = cur;
				}
			}
			place_redstone_torch(src_x, Y, z);
			for(int k = 0; k < 4; k++) {
				tick_wd();
				int cur = get(post_x, Y + 1, z);
				if(cur != prev[i]) {
					toggles[i]++;
					prev[i] = cur;
				}
			}
		}
	}
	char phase[64];
	snprintf(phase, sizeof(phase), "scale-bank-%d", copies);
	for(int i = 0; i < copies; i++)
		assert(toggles[i] > 0);
	assert_redstone_work_bounds(phase);

	struct redstone_work_sample now = sample_redstone_work();
	if(baseline) {
		if(now.sw_dispatch > baseline->sw_dispatch
		   || now.nn_dispatch > baseline->nn_dispatch
		   || now.wire_depth > baseline->wire_depth
		   || now.wire_pops > baseline->wire_pops
		   || now.wire_full > baseline->wire_full) {
			fprintf(stderr,
					"REDSTONE SCALE FAIL [%d copies]: sw_dispatch=%d>%d "
					"nn_dispatch=%d>%d wire_depth=%d>%d wire_pops=%d>%d "
					"wire_full=%d>%d\n",
					copies, now.sw_dispatch, baseline->sw_dispatch,
					now.nn_dispatch, baseline->nn_dispatch,
					now.wire_depth, baseline->wire_depth,
					now.wire_pops, baseline->wire_pops,
					now.wire_full, baseline->wire_full);
		}
		assert(now.sw_dispatch <= baseline->sw_dispatch);
		assert(now.nn_dispatch <= baseline->nn_dispatch);
		assert(now.wire_depth <= baseline->wire_depth);
		assert(now.wire_pops <= baseline->wire_pops);
		assert(now.wire_full <= baseline->wire_full);
	}
	printf("scale-bank %d OK: sw_dispatch=%d nn_dispatch=%d wire_depth=%d "
		   "wire_pops=%d wire_full=%d\n",
		   copies, now.sw_dispatch, now.nn_dispatch, now.wire_depth,
		   now.wire_pops, now.wire_full);
	return now;
}

// Place a redstone torch via the REAL item onItemPlace path (SIDE_TOP -> floor mount, meta 5).
static bool place_redstone_torch(int x, int y, int z) {
	struct item_data it = {.id = BLOCK_REDSTONE_TORCH_LIT, .count = 1};
	struct block_data on_blk;
	if(!server_world_get_block(&s.world, x, y - 1, z, &on_blk))
		return false;
	struct block_info where = {.x = x, .y = y, .z = z};
	struct block_info on = {.x = x, .y = y - 1, .z = z, .block = &on_blk};
	const struct block* tb = blocks[BLOCK_REDSTONE_TORCH_LIT];
	return tb->block_item.onItemPlace(&s, &s.player, &it, &where, &on, SIDE_TOP);
}
static void break_block(int x, int y, int z) {
	server_world_set_block(&s.worlds[0], x, y, z, (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
	notifyNeighbours(&s, x, y, z);
}

// One NOT-gate stage: a stone post at (px,Y,pz) with a torch on top (px,Y+1,pz); the torch's
// output is read off an adjacent wire. The post is powered (turning the torch off) by wire run
// INTO the post. We chain stage i's torch-output wire to feed stage i+1's post.
//
// Geometry (all on the Y plane, posts raised; torches at Y+1):
//   stage 0 post @ (2,Y,2)   torch @ (2,Y+1,2)
//   stage 1 post @ (2,Y,6)   torch @ (2,Y+1,6)
//   stage 2 post @ (2,Y,10)  torch @ (2,Y+1,10)
// A torch on top of a post emits power DOWN into the post and to wire beside the post at Y+1 is
// awkward; instead use the inverter-topology primitive directly: torch on top of B powers wire
// that is adjacent to B at the SAME Y as the wire. The torch (meta 5, floor mount) provides power
// to the cell it sits on AND acts as a source for wire at its own level around the post top.
//
// Simpler faithful clock that the engine demonstrably runs: chain three floor torches where each
// torch's mount post is powered by the previous torch's output wire (NOT-gate ring). Lay wire from
// each torch to the next post.

int main(void) {
	test_fixture_dir("test_rs_torch_clock_storm_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	setvbuf(stdout, NULL, _IONBF, 0); // unbuffered: see exactly which phase hangs
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	redstone_torch_reset_burnout();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = FCAVEX_SCHEDULED_TICK_BUDGET; // the REAL game budget (Wii path)
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	for(int cz = -1; cz <= 1; cz++)
		for(int cx = -1; cx <= 1; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	for(int x = 0; x <= 14; x++)
		for(int z = 0; z <= 14; z++)
			set(x, YF, z, BLOCK_STONE, 0);

	// ============================================================================
	// PHASE 1: rapid place+break of a redstone torch beside powered wire (~2000x).
	// ============================================================================
	g_phase = "phase1-setup";
	set(4, Y, 4, BLOCK_REDSTONE_TORCH_LIT, 5); // source torch (floor mount)
	set(5, Y, 4, BLOCK_REDSTONE_WIRE, 0);
	set(6, Y, 4, BLOCK_REDSTONE_WIRE, 0);
	for(int k = 0; k < 10; k++)
		tick_wd();

	g_phase = "phase1-place-break-loop";
	reset_redstone_work_bounds();
	for(int i = 0; i < 400; i++) {
		place_redstone_torch(7, Y, 4);
		tick_wd();
		tick_wd();
		break_block(7, Y, 4);
		tick_wd();
		tick_wd();
	}
	printf("phase1 OK: 400 torch place/break cycles beside powered wire; sched=%zu drops=%u\n",
		   s.world.scheduled_tick_count, (unsigned)s.world.scheduled_tick_drops);
	assert_redstone_work_bounds("phase1");

	for(int x = 4; x <= 7; x++)
		set(x, Y, 4, BLOCK_AIR, 0);
	for(int k = 0; k < 10; k++)
		tick_wd();

	// ============================================================================
	// PHASE 2: 3-stage NOT-gate RING oscillator (the classic Beta torch clock).
	//
	// Stage layout (NOT-gate primitive from test_redstone_torch_inverter_topologies: a CONTINUOUS
	// wire run into a stone block B with a torch ON TOP turns the torch OFF). We chain three:
	//   stageX_torch (on post Bx) -> wire -> ... -> post B(x+1) -> torch on B(x+1)
	// Three inverters in a ring never settle -> free-running clock.
	//
	// Rows (z), each on the Y plane; posts BLOCK_STONE at Y, torches BLOCK_REDSTONE_TORCH_LIT at Y+1.
	//   B0 @ (2,Y,2)  torch @ (2,Y+1,2);  out wire (3,Y,2),(4,Y,2) -> into B1
	//   B1 @ (5,Y,2)  torch @ (5,Y+1,2);  out wire (3,Y,4),(4,Y,4) ... route to B2
	// To keep the wiring simple and robust, place each torch-on-post and a 2-wire run that
	// terminates against the NEXT post. Use distinct z-rows so runs don't cross.
	// ============================================================================
	// ============================================================================
	// PHASE 2: the PROVEN inverter primitive (test_redstone_torch_inverter_topologies row A),
	// driven as a forced clock: a SOURCE floor torch -> continuous wire -> stone B -> torch on TOP
	// of B. We then FORCE the inverter to fire repeatedly by breaking + replacing the SOURCE torch
	// every few ticks (each toggle flips the wire, then the torch-on-B, exercising redstone_torch_
	// tick + the NEW torch_neighbour_change re-arm storm). Thousands of forced cycles.
	// ============================================================================
	g_phase = "phase2-setup";
	set(2, Y, 12, BLOCK_REDSTONE_TORCH_LIT, 5); // SOURCE floor torch (mount = YF floor)
	set(3, Y, 12, BLOCK_REDSTONE_WIRE, 0);
	set(4, Y, 12, BLOCK_REDSTONE_WIRE, 0);
	set(5, Y, 12, BLOCK_STONE, 0);					// B
	set(5, Y + 1, 12, BLOCK_REDSTONE_TORCH_LIT, 5); // torch on TOP of B (the inverter output)
	for(int k = 0; k < 20; k++)
		tick_wd();
	int diag_torch_on_B = get(5, Y + 1, 12);
	printf("[diag] inverter settled: torch-on-B=%d (75=OFF means inverter active)\n",
		   diag_torch_on_B);

	g_phase = "phase2-forced-clock";
	reset_redstone_work_bounds();
	int toggles = 0;
	int peak_sched = 0;
	int prevB = get(5, Y + 1, 12);
	clock_t p2_t0 = clock();
	for(int cyc = 0; cyc < 600; cyc++) {
		// break the SOURCE torch -> wire de-energises -> B unpowered -> torch-on-B relights
		break_block(2, Y, 12);
		for(int k = 0; k < 4; k++) {
			tick_wd();
			if((int)s.world.scheduled_tick_count > peak_sched)
				peak_sched = (int)s.world.scheduled_tick_count;
			int cB = get(5, Y + 1, 12);
			if(cB != prevB) {
				toggles++;
				prevB = cB;
			}
		}
		// replace the SOURCE torch -> wire re-energises -> B powered -> torch-on-B turns off
		place_redstone_torch(2, Y, 12);
		for(int k = 0; k < 4; k++) {
			tick_wd();
			if((int)s.world.scheduled_tick_count > peak_sched)
				peak_sched = (int)s.world.scheduled_tick_count;
			int cB = get(5, Y + 1, 12);
			if(cB != prevB) {
				toggles++;
				prevB = cB;
			}
		}
		if(cyc < 5 || (cyc % 200) == 0) {
			double el = 1000.0 * (double)(clock() - p2_t0) / CLOCKS_PER_SEC;
			printf("  [p2 cyc %d] elapsed=%.0fms toggles=%d peak_sched=%d drops=%u\n", cyc, el,
				   toggles, peak_sched, (unsigned)s.world.scheduled_tick_drops);
		}
	}
	printf("phase2 OK: 600 forced-clock cycles, inverter toggles=%d, peak_sched=%d, drops=%u\n",
		   toggles, peak_sched, (unsigned)s.world.scheduled_tick_drops);
	assert(toggles > 0); // no false-negative: the inverter actually toggled
	assert_redstone_work_bounds("phase2");

	// ============================================================================
	// PHASE 3: place+break EXTRA torches WHILE the forced clock runs (user sequence): two adjacent
	// torches on the same mount neighbourhood + a transient beside the live wire, every cycle.
	// ============================================================================
	g_phase = "phase3-place-break-during-clock";
	reset_redstone_work_bounds();
	for(int i = 0; i < 400; i++) {
		// keep toggling the source clock
		if(i & 1)
			break_block(2, Y, 12);
		else
			place_redstone_torch(2, Y, 12);
		tick_wd();
		// transient torch beside the live feed wire
		place_redstone_torch(3, Y, 11);
		tick_wd();
		break_block(3, Y, 11);
		tick_wd();
		// a torch mounted ON the same post B as the inverter output (side mount), then break it --
		// stresses two torches sharing a mount + the neighbour-change re-arm
		place_redstone_torch(6, Y, 12);
		tick_wd();
		break_block(6, Y, 12);
		tick_wd();
	}
	printf("phase3 OK: 400 place/break cycles during the forced clock; sched=%zu drops=%u\n",
		   s.world.scheduled_tick_count, (unsigned)s.world.scheduled_tick_drops);
	assert_redstone_work_bounds("phase3");

	// ============================================================================
	// PHASE 4: DENSE torch field driven by one shared wire, toggled fast. Many torches re-arm at
	// once via torch_neighbour_change -> does the scheduled queue grow unbounded / a tick blow up?
	// A long wire row at z=6 (x=1..13) with a torch on a post every other cell, all powered/de-
	// powered together. Watch peak_sched + drops across a fast toggle.
	// ============================================================================
	g_phase = "phase4-dense-field-setup";
	printf("phase4 setup begin\n");
	for(int x = 1; x <= 13; x++)
		set(x, Y, 6, BLOCK_REDSTONE_WIRE, 0);
	// a torch on a side post beside each wire cell (mount on the post at Y, torch at Y+1 won't be
	// adjacent to the wire; instead mount torches on the floor at z=7 beside the wire so the wire
	// powers their mount -> they invert together)
	for(int x = 1; x <= 13; x++) {
		set(x, Y, 7, BLOCK_STONE, 0);				   // post the torch mounts on
		set(x, Y + 1, 7, BLOCK_REDSTONE_TORCH_LIT, 5); // torch on the post
		set(x, Y, 8, BLOCK_REDSTONE_WIRE, 0);		   // wire beside the post (reads it)
	}
	// a source we toggle: floor torch at (0,Y,6) feeding the z=6 wire row
	set(0, Y, 6, BLOCK_REDSTONE_TORCH_LIT, 5);
	printf("phase4 setup done, settling...\n");
	for(int k = 0; k < 20; k++)
		tick_wd();
	printf("phase4 settled\n");
	int p4_peak = (int)s.world.scheduled_tick_count;
	int p4_drops0 = (int)s.world.scheduled_tick_drops;

	g_phase = "phase4-dense-fast-toggle";
	reset_redstone_work_bounds();
	double worst_op_ms = 0;
	int worst_op_cyc = -1;
	for(int cyc = 0; cyc < 300; cyc++) {
		clock_t t0 = clock();
		if(cyc & 1)
			break_block(0, Y, 6); // de-power the whole row (synchronous wire flood + notifies)
		else
			place_redstone_torch(0, Y, 6); // re-power the whole row
		double op_ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
		if(op_ms > worst_op_ms) {
			worst_op_ms = op_ms;
			worst_op_cyc = cyc;
		}
		tick_wd();
		if((int)s.world.scheduled_tick_count > p4_peak)
			p4_peak = (int)s.world.scheduled_tick_count;
		if(cyc < 12 || (cyc % 50) == 0)
			printf("  [p4 cyc %d] place/break op=%.1f ms, sched=%zu drops=%u\n", cyc, op_ms,
				   s.world.scheduled_tick_count, (unsigned)s.world.scheduled_tick_drops);
	}
	printf("phase4 OK: 300 dense-field fast toggles; peak_sched=%d drops_delta=%u worst_op=%.1fms@cyc%d\n",
		   p4_peak, (unsigned)s.world.scheduled_tick_drops - (unsigned)p4_drops0, worst_op_ms,
		   worst_op_cyc);
	assert_redstone_work_bounds("phase4");

	g_phase = "phase5-scale-bank";
	struct redstone_work_sample base = run_scaled_inverter_bank(1, NULL);
	run_scaled_inverter_bank(2, &base);
	run_scaled_inverter_bank(4, &base);

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_redstone_torch_clock_storm_env: OK (no freeze / no crash across all 3 phases)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
