/*
	BUG-HUNT env test (2026-06-27): cover the bounded-work side of the real-Wii freeze trigger where
	a player COMPLETES a redstone feedback loop with a few dropped redstone-torch ITEM entities present.

	Distinct from test_redstone_torch_clock_storm_env.c, which drives a FORCED clock (manually
	break/place the source torch each cycle) and DISCARDS client RPCs, and from
	test_redstone_loop_close_drops_env.c, which models the live item-pile loop-completion trigger more
	fully. The user's exact repro is:
	  - build a clock (torch + repeaters + dust),
	  - place the MISSING dust to CLOSE the loop -> CRASH.

	This test exercises what the forced test never did:
	  (1) a closed odd-inverter dust ring: place the final dust, then tick long enough to prove
	      bounded notify/wire work after loop completion.
	  (2) the peak SYNCHRONOUS nesting depth and dispatch/pops count of the wire flood + notify
	      cascade, to test the stack-overflow / unbounded-recursion angle.
	  (3) a handful of dropped redstone-torch item entities living in the area while the loop runs.

	Diagnostic, not a faithfulness golden. WATCHDOG: each server_world_tick wall-timed; >2s aborts.
*/

#include <assert.h>
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
#include "block/redstone_server.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "profile.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_rs_closed_loop_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 /* stone floor */
#define Y  101 /* circuit plane */
// Diagnostic ceiling for ONE top-level notify cascade in this repro. This is a
// bounded-work assertion, not a PC wall-clock proof.
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

static const char* g_phase = "init";
static void tick_wd(void) {
	clock_t t0 = clock();
	server_world_tick(&s.world, &s);
	double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
	if(ms > 2000.0) {
		fprintf(stderr,
				"WATCHDOG: single server_world_tick took %.0f ms in phase [%s] -- FREEZE. "
				"sched=%zu drops=%u wire_depth_max=%d ncas_us=%u\n",
				ms, g_phase, s.world.scheduled_tick_count,
				(unsigned)s.world.scheduled_tick_drops,
				redstone_server_wire_flood_depth_max(),
				(unsigned)server_world_notify_cascade_max_us());
		abort();
	}
}

/* Place redstone dust via the REAL item path (item_redstone -> place + self-flood). */
static bool place_redstone_dust(int x, int y, int z) {
	struct item_data it = {.id = ITEM_REDSTONE, .count = 1};
	struct block_data on_blk;
	if(!server_world_get_block(&s.world, x, y - 1, z, &on_blk))
		return false;
	struct block_info where = {.x = x, .y = y, .z = z};
	struct block_info on = {.x = x, .y = y - 1, .z = z, .block = &on_blk};
	struct item* ir = item_get(&it);
	assert(ir && ir->onItemPlace);
	return ir->onItemPlace(&s, &s.player, &it, &where, &on, SIDE_TOP);
}

static void break_block(int x, int y, int z) {
	server_world_set_block(&s.worlds[0], x, y, z, (struct block_data) {.type = BLOCK_AIR, .metadata = 0});
	notifyNeighbours(&s, x, y, z);
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

static void assert_redstone_work_ran(const char* phase) {
	int wire_pops = redstone_wire_update_max_pops();
	if(wire_pops <= 0)
		fprintf(stderr, "REDSTONE NO-WORK FAIL [%s]: wire_pops=%d\n",
				phase, wire_pops);
	assert(wire_pops > 0);
}

int main(void) {
	test_fixture_dir("test_rs_closed_loop_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	setvbuf(stdout, NULL, _IONBF, 0);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	redstone_torch_reset_burnout();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = FCAVEX_SCHEDULED_TICK_BUDGET;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	for(int x = 0; x <= 15; x++)
		for(int z = 0; z <= 15; z++)
			set(x, YF, z, BLOCK_STONE, 0);

	/* ----------------------------------------------------------------------
	   Drop ~10 redstone-torch item entities into the area (the user had a few
	   to 10 on the ground at crash time).
	   ---------------------------------------------------------------------- */
	g_phase = "drop-items";
	for(int i = 0; i < 10; i++) {
		struct item_data it = {.id = BLOCK_REDSTONE_TORCH_LIT, .count = 1};
		server_local_spawn_item((vec3) {2.0f + i * 0.3f, Y + 0.5f, 2.0f},
								&it, false, &s);
	}
	for(int k = 0; k < 5; k++)
		tick_wd();
	printf("[items] spawned 10 torch items; entities=%zu\n",
		   (size_t)dict_entity_size(s.entities));

	/* ----------------------------------------------------------------------
	   PHASE A: 3-stage TORCH NOT-gate RING, CLOSED.
	   Each stage: wire -> stone post B -> torch on top of B (powered post = torch OFF).
	   Stage i's torch output wire feeds stage (i+1)'s post. The LAST connecting
	   wire is the "missing piece" the player places to close the loop.
	   ---------------------------------------------------------------------- */
	g_phase = "phaseA-build-open";
	/* posts */
	set(4, Y, 4, BLOCK_STONE, 0);
	set(8, Y, 4, BLOCK_STONE, 0);
	set(8, Y, 8, BLOCK_STONE, 0);
	/* torches on top of each post */
	set(4, Y + 1, 4, BLOCK_REDSTONE_TORCH_LIT, 5);
	set(8, Y + 1, 4, BLOCK_REDSTONE_TORCH_LIT, 5);
	set(8, Y + 1, 8, BLOCK_REDSTONE_TORCH_LIT, 5);
	/* wire runs connecting torch output to next post (leave ONE gap to close) */
	set(5, Y, 4, BLOCK_REDSTONE_WIRE, 0); /* torchA out toward postB */
	set(6, Y, 4, BLOCK_REDSTONE_WIRE, 0);
	set(7, Y, 4, BLOCK_REDSTONE_WIRE, 0); /* into postB */
	set(8, Y, 5, BLOCK_REDSTONE_WIRE, 0); /* torchB out toward postC */
	set(8, Y, 6, BLOCK_REDSTONE_WIRE, 0);
	set(8, Y, 7, BLOCK_REDSTONE_WIRE, 0); /* into postC */
	set(7, Y, 8, BLOCK_REDSTONE_WIRE, 0); /* torchC out back toward postA */
	set(6, Y, 8, BLOCK_REDSTONE_WIRE, 0);
	set(5, Y, 8, BLOCK_REDSTONE_WIRE, 0);
	set(4, Y, 8, BLOCK_REDSTONE_WIRE, 0);
	set(4, Y, 7, BLOCK_REDSTONE_WIRE, 0);
	set(4, Y, 6, BLOCK_REDSTONE_WIRE, 0);
	/* GAP at (4,Y,5): postA is at (4,Y,4); placing wire at (4,Y,5) closes the ring */
	for(int k = 0; k < 30; k++)
		tick_wd();
	printf("[A] open ring settled: torchA=%d torchB=%d torchC=%d wire_depth_max=%d\n",
		   get(4, Y + 1, 4), get(8, Y + 1, 4), get(8, Y + 1, 8),
		   redstone_server_wire_flood_depth_max());

	g_phase = "phaseA-CLOSE-LOOP";
	printf("[A] CLOSING THE LOOP: placing the missing dust at (4,%d,5)...\n", Y);
	reset_redstone_work_bounds();
	bool placed_a = place_redstone_dust(4, Y, 5); /* the missing piece -> ring closes */
	assert(placed_a);
	int placed_a_type = get(4, Y, 5);
	if(placed_a_type != BLOCK_REDSTONE_WIRE)
		fprintf(stderr, "phaseA place_wire returned true but target type=%d\n",
				placed_a_type);
	assert(placed_a_type == BLOCK_REDSTONE_WIRE);

	g_phase = "phaseA-oscillate";
	int toggles = 0, prevA = get(4, Y + 1, 4);
	for(int t = 0; t < 4000; t++) {
		tick_wd();
		int a = get(4, Y + 1, 4);
		if(a != prevA) { toggles++; prevA = a; }
		if(t < 8 || (t % 1000) == 0)
			printf("  [A osc t=%d] torchA=%d toggles=%d sched=%zu drops=%u depth=%d ncas=%uus\n",
				   t, a, toggles, s.world.scheduled_tick_count,
				   (unsigned)s.world.scheduled_tick_drops,
				   redstone_server_wire_flood_depth_max(),
				   (unsigned)server_world_notify_cascade_max_us());
	}
	printf("phaseA OK: closed torch ring ran 4000 ticks, observed toggles=%d, peak wire_depth=%d, ncas_max=%uus, drops=%u\n",
		   toggles, redstone_server_wire_flood_depth_max(),
		   (unsigned)server_world_notify_cascade_max_us(),
		   (unsigned)s.world.scheduled_tick_drops);
	assert_redstone_work_ran("phaseA");
	assert_redstone_work_bounds("phaseA");

	/* ----------------------------------------------------------------------
	   PHASE B: REPEATER feedback-loop placement smoke. A repeater whose output
	   feeds (through wire) back toward its input via an inverting torch. Close it
	   by placing the final dust and assert bounded notify/wire work.
	   ---------------------------------------------------------------------- */
	g_phase = "phaseB-build";
	/* clear phase A */
	for(int x = 3; x <= 9; x++)
		for(int z = 3; z <= 9; z++) {
			set(x, Y, z, BLOCK_AIR, 0);
			set(x, Y + 1, z, BLOCK_AIR, 0);
		}
	for(int k = 0; k < 10; k++)
		tick_wd();

	/* repeater clock: torch source -> wire -> repeater -> wire -> stone post -> torch
	   on top -> back to source post. */
	set(2, Y, 12, BLOCK_STONE, 0);                 /* source post */
	set(2, Y + 1, 12, BLOCK_REDSTONE_TORCH_LIT, 5);/* source torch */
	set(3, Y, 12, BLOCK_REDSTONE_WIRE, 0);
	/* repeater facing so input is the -X side (reads from source wire) */
	assert(redstone_repeater_place(&s, &s.player, &(struct block_info){.x=4,.y=Y,.z=12}, 1));
	set(5, Y, 12, BLOCK_REDSTONE_WIRE, 0);
	set(6, Y, 12, BLOCK_STONE, 0);                 /* post B */
	set(6, Y + 1, 12, BLOCK_REDSTONE_TORCH_LIT, 5);/* inverter torch on B */
	/* feed B's torch output back toward the source post to make a ring */
	set(6, Y, 11, BLOCK_REDSTONE_WIRE, 0);
	set(5, Y, 11, BLOCK_REDSTONE_WIRE, 0);
	set(4, Y, 11, BLOCK_REDSTONE_WIRE, 0);
	set(3, Y, 11, BLOCK_REDSTONE_WIRE, 0);
	set(2, Y, 11, BLOCK_REDSTONE_WIRE, 0); /* adjacent to source post -> powers it */
	for(int k = 0; k < 40; k++)
		tick_wd();
	printf("[B] repeater ring settled: srcTorch=%d invTorch=%d depth=%d sched=%zu\n",
		   get(2, Y + 1, 12), get(6, Y + 1, 12),
		   redstone_server_wire_flood_depth_max(),
		   s.world.scheduled_tick_count);

	g_phase = "phaseB-CLOSE";
	/* break + replace a dust in the loop to force the "complete the missing piece" event */
	break_block(4, Y, 11);
	for(int k = 0; k < 6; k++) tick_wd();
	printf("[B] placing the missing dust at (4,%d,11)...\n", Y);
	reset_redstone_work_bounds();
	bool placed_b = place_redstone_dust(4, Y, 11);
	assert(placed_b);
	assert(get(4, Y, 11) == BLOCK_REDSTONE_WIRE);
	for(int t = 0; t < 4000; t++) {
		tick_wd();
		if(t < 8 || (t % 1000) == 0)
			printf("  [B post-close t=%d] invTorch=%d sched=%zu drops=%u depth=%d ncas=%uus\n",
				   t, get(6, Y + 1, 12), s.world.scheduled_tick_count,
				   (unsigned)s.world.scheduled_tick_drops,
				   redstone_server_wire_flood_depth_max(),
				   (unsigned)server_world_notify_cascade_max_us());
	}
	printf("phaseB OK: repeater loop placement smoke, peak wire_depth=%d, ncas_max=%uus, drops=%u, entities=%zu\n",
		   redstone_server_wire_flood_depth_max(),
		   (unsigned)server_world_notify_cascade_max_us(),
		   (unsigned)s.world.scheduled_tick_drops, (size_t)dict_entity_size(s.entities));
	assert_redstone_work_ran("phaseB");
	assert_redstone_work_bounds("phaseB");

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_redstone_closed_loop_freeze_env: OK (no freeze; peak wire_depth=%d ncas_max=%uus)\n",
		   redstone_server_wire_flood_depth_max(),
		   (unsigned)server_world_notify_cascade_max_us());
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
