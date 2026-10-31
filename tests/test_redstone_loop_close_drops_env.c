/*
	BUG-HUNT env test (2026-06-27): reproduce the real-Wii FREEZE (build md5 991d2306, which
	ALREADY contains the notify-cascade thread_yield + NOTIFY_MAX_DEPTH=16 fix) that hits with
	the user's EXACT, refined trigger:

	  1) Build a redstone CLOCK (torch + repeaters + dust).
	  2) Spamming place/break of the clock's torch did NOT crash (so it is NOT the notify-
	     cascade timing path the prior fix addressed).
	  3) INTERRUPT the loop by breaking a redstone DUST, then PLACE that dust BACK to COMPLETE
	     the loop -- via the REAL dust item onItemPlace path (item_redstone.c) -- CRASHED.
	  4) ~a few-to-10 dropped redstone-TORCH ITEM entities were on the ground at the time
	     (every torch break support-drops an item; rapid spam piles them up).

	This drives the parts test_redstone_torch_clock_storm_env never did:
	  * the REAL dust item placer (item_redstone.onItemPlace -> server_world_set_block ->
	    redstone_wire_neighbour_change self-flood) to CLOSE an OPEN feedback loop,
	  * live dropped redstone-torch ITEM entities (server_local_spawn_item / entity_item),
	  * the ENTITY tick loop (entity_server_tick: physics + pickup, which READS the world
	    blocks the items rest on -- the redstone field), interleaved with the redstone settle.

	Three scenarios, each asserting the circuit actually DOES work (no false-negative):
	  A) POWERED DUST CYCLE: a lever -> a ring of dust with a one-cell GAP. Closing the gap with
	     the real dust placer forms a genuine graph CYCLE the wire flood worklist must re-traverse.
	     Asserts the ring powers up to >0 strength on close (the flood ran) -- with ~10 dropped
	     torch items present and entity ticks interleaved.
	  B) FORCED TORCH CLOCK (the proven inverter from the storm test) running while we break +
	     REPLACE a dust feeding it -- the literal "break a dust, place it back" on a live clock,
	     with the item pile present. Asserts the inverter toggles.
	  C) Stress: rapid close/reopen of the cycle gap while re-piling torch items + a transient
	     torch, entity ticks every step.

	WATCHDOG: every world tick is wall-timed; >2s aborts with the phase label. Built with
	-DSANITIZE=ON (ASan+UBSan) the heap/OOB/UB report is the real proof.
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
#include "entity/entity.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "profile.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_rs_loop_close_drops_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y  101 // circuit plane
// Diagnostic ceiling for ONE top-level notify cascade in this repro. This is intentionally a
// bounded-work assertion, not a PC wall-clock assertion.
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
static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? (b.metadata & 0x0F) : -1;
}

static const char* g_phase = "init";
static void server_tick_only(void) {
	clock_t t0 = clock();
	server_world_tick(&s.world, &s);
	double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
	if(ms > 2000.0) {
		fprintf(stderr,
				"WATCHDOG: single server_world_tick took %.0f ms in phase [%s] -- FREEZE. "
				"sched=%zu drops=%u\n",
				ms, g_phase, s.world.scheduled_tick_count,
				(unsigned)s.world.scheduled_tick_drops);
		abort();
	}
}

// Tick the LIVE ENTITIES exactly like server_local's static tick loop (snapshot keys, get()
// per key, tick_server, reap on remove). This is the path that runs entity_item's server tick
// -- physics + pickup reading the redstone field -- which the world-only tick never exercises.
static void entity_tick_loop(void) {
	enum { SNAP_MAX = 1024 };
	uint32_t keys[SNAP_MAX];
	size_t n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it) && n < SNAP_MAX) {
		keys[n++] = dict_entity_ref(it)->key;
		dict_entity_next(it);
	}
	for(size_t i = 0; i < n; i++) {
		struct entity** pp = dict_entity_get(s.entities, keys[i]);
		if(!pp || !*pp)
			continue;
		struct entity* e = *pp;
		if(!e->tick_server)
			continue;
		bool remove = (e->delay_destroy == 0) || e->tick_server(e, &s);
		if(remove) {
			free(e);
			dict_entity_erase(s.entities, keys[i]);
		}
	}
}

static void tick_wd(void) {
	server_tick_only();
	entity_tick_loop();
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

static size_t entity_count(void) { return dict_entity_size(s.entities); }

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

// Place redstone DUST via the REAL item path (item_redstone.onItemPlace): places on TOP of the
// block below, then runs redstone_wire_neighbour_change (the Beta onBlockAdded self-flood) -- the
// EXACT path the user's "place the missing piece of redstone" hits.
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

// Drop a redstone-torch item entity -- mirrors the support-drop entity spawn that piles up when
// torches are rapidly broken (server_local_spawn_block_drops -> server_local_spawn_item).
static void drop_torch_item(int x, int y, int z) {
	struct item_data it = {.id = BLOCK_REDSTONE_TORCH_LIT, .durability = 0, .count = 1};
	server_local_spawn_item((vec3) {x + 0.5F, y + 0.5F, z + 0.5F}, &it, false, &s);
}

int main(void) {
	test_fixture_dir("test_rs_loop_close_drops_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	setvbuf(stdout, NULL, _IONBF, 0);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init(); // the dust placer goes through the ITEM registry (item_redstone)
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
	s.scheduled_tick_budget = FCAVEX_SCHEDULED_TICK_BUDGET;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);

	// A real player so the entity pickup path (on_collect) is live, exactly like the game.
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE,
							0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 7.0f;
	s.player.y = (float)Y + 1.0f;
	s.player.z = 7.0f;

	for(int cz = -1; cz <= 1; cz++)
		for(int cx = -1; cx <= 1; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	for(int x = 0; x <= 15; x++)
		for(int z = 0; z <= 15; z++)
			set(x, YF, z, BLOCK_STONE, 0);

	// ========================================================================================
	// SCENARIO A: POWERED DUST CYCLE closed by the real placer, with dropped torch items.
	//
	// A lever at (1,Y,1) (floor mount, meta 5|8) powers a wire ring. Lay a rectangular ring of
	// dust around z=1..3, x=1..6 with a ONE-CELL GAP at (4,Y,1). With the gap open the ring is a
	// powered open path; CLOSING the gap forms a true graph CYCLE -- the wire flood worklist
	// re-traverses the loop (the case the RS_WORK_MAX ring + full-check guards). We assert the
	// gap cell powers up (>0) on close, proving the flood actually ran over the cycle.
	// ========================================================================================
	g_phase = "A-build-powered-ring";
	set(1, Y, 1, BLOCK_LEVER, 5 | 8); // ON floor lever, source
	// ring perimeter (dust), leaving (4,Y,1) as the GAP:
	set(2, Y, 1, BLOCK_REDSTONE_WIRE, 0);
	set(3, Y, 1, BLOCK_REDSTONE_WIRE, 0);
	// GAP at (4,Y,1)
	set(5, Y, 1, BLOCK_REDSTONE_WIRE, 0);
	set(6, Y, 1, BLOCK_REDSTONE_WIRE, 0);
	set(6, Y, 2, BLOCK_REDSTONE_WIRE, 0);
	set(6, Y, 3, BLOCK_REDSTONE_WIRE, 0);
	for(int x = 6; x >= 1; x--)
		set(x, Y, 3, BLOCK_REDSTONE_WIRE, 0);
	set(1, Y, 2, BLOCK_REDSTONE_WIRE, 0); // back up to the lever side
	for(int k = 0; k < 20; k++)
		tick_wd();
	// The lever-fed left arm is powered; the right arm (past the gap) is fed only through the
	// long way around (x=6..1 on z=3 then up). Verify the near side is live (flood ran at all).
	assert(getmeta(2, Y, 1) > 0);
	printf("A: powered ring built (gap @ (4,Y,1)); near wire meta=%d far wire(5)=%d\n",
		   getmeta(2, Y, 1), getmeta(5, Y, 1));

	g_phase = "A-drop-torch-items";
	for(int i = 0; i < 10; i++)
		drop_torch_item(3 + (i % 3), Y, 1 + (i % 3));
	printf("A: spawned 10 dropped torch items; entity_count=%zu\n", entity_count());
	for(int k = 0; k < 10; k++)
		tick_wd();

	// CLOSE THE LOOP via the real placer -> forms the cycle; assert the gap cell powers up.
	g_phase = "A-close-cycle";
	reset_redstone_work_bounds();
	bool placed = place_redstone_dust(4, Y, 1);
	assert(placed);
	for(int k = 0; k < 6; k++)
		tick_wd();
	int gap_meta = getmeta(4, Y, 1);
	printf("A: cycle CLOSED, gap cell (4,Y,1) meta=%d (>0 => flood traversed the loop)\n",
		   gap_meta);
	assert(gap_meta > 0); // the closed cycle is powered -- the flood did real work

	printf("A OK: powered dust cycle closed with item pile + entity ticks; entity_count=%zu\n",
		   entity_count());
	assert_redstone_work_bounds("A");

	// ========================================================================================
	// SCENARIO B: the PROVEN inverter (continuous wire -> stone B -> torch on B = OFF), driven
	// as a clock by breaking + REPLACING the dust that feeds it -- the literal user action
	// "break a redstone dust, then place it back to complete the loop", on a LIVE inverter, with
	// the item pile present and entity ticks every step. Asserts the inverter actually toggles.
	// ========================================================================================
	g_phase = "B-build-inverter";
	const int ZB = 8;
	// EXACT proven topology-A geometry + build ORDER (test_redstone_torch_inverter_topologies row
	// A): wire -> stone B -> torch on TOP of B, and the SOURCE floor torch placed LAST so its
	// onBlockAdded flood lights the whole run in one shot => torch-on-B turns OFF (inverts).
	set(3, Y, ZB, BLOCK_REDSTONE_WIRE, 0);
	set(4, Y, ZB, BLOCK_REDSTONE_WIRE, 0); // <-- the dust we break+replace
	set(5, Y, ZB, BLOCK_REDSTONE_WIRE, 0);
	set(6, Y, ZB, BLOCK_STONE, 0);				   // B
	set(6, Y + 1, ZB, BLOCK_REDSTONE_TORCH_LIT, 5); // torch on B (inverter output)
	set(2, Y, ZB, BLOCK_REDSTONE_TORCH_LIT, 5);	   // SOURCE floor torch (placed last)
	for(int k = 0; k < 40; k++)
		tick_wd();
	// with the wire feeding B, B is powered -> the on-top torch is OFF (idle id 75).
	printf("B: inverter built, feed-wire(4)meta=%d B-powered?, torch-on-B=%d (75=OFF active)\n",
		   getmeta(4, Y, ZB), get(6, Y + 1, ZB));
	assert(get(6, Y + 1, ZB) == BLOCK_REDSTONE_TORCH); // inverter active (OFF)

	g_phase = "B-break-replace-feed-dust";
	reset_redstone_work_bounds();
	int toggles = 0;
	int prev = get(6, Y + 1, ZB);
	for(int rep = 0; rep < 80; rep++) {
		// BREAK the feed dust -> B loses power -> on-top torch RELIGHTS (after its scheduled tick)
		break_block(4, Y, ZB);
		for(int k = 0; k < 3; k++) {
			tick_wd();
			int cur = get(6, Y + 1, ZB);
			if(cur != prev) { toggles++; prev = cur; }
		}
		if((rep % 4) == 0) {
			drop_torch_item(4, Y, ZB);
			drop_torch_item(4, Y, ZB + 1);
		}
		// REPLACE the feed dust via the real placer -> B re-powered -> on-top torch OFF
		bool p = place_redstone_dust(4, Y, ZB);
		(void)p;
		for(int k = 0; k < 3; k++) {
			tick_wd();
			int cur = get(6, Y + 1, ZB);
			if(cur != prev) { toggles++; prev = cur; }
		}
		if(rep < 5 || (rep % 20) == 0)
			printf("  [B rep %d] torch-on-B=%d toggles=%d entities=%zu sched=%zu drops=%u\n",
				   rep, get(6, Y + 1, ZB), toggles, entity_count(),
				   s.world.scheduled_tick_count, (unsigned)s.world.scheduled_tick_drops);
	}
	printf("B OK: 80 break+replace feed-dust cycles, inverter toggles=%d (proves it ran)\n",
		   toggles);
	assert(toggles > 0); // no false-negative: the inverter actually toggled
	assert_redstone_work_bounds("B");

	// ========================================================================================
	// SCENARIO C: rapid close/reopen of the cycle gap (scenario A) while re-piling torch items
	// and a transient torch, entity ticks every step -- the sustained "interrupt+complete" spam.
	// ========================================================================================
	g_phase = "C-rapid-cycle-spam";
	reset_redstone_work_bounds();
	for(int rep = 0; rep < 120; rep++) {
		if((rep % 3) == 0) {
			drop_torch_item(4, Y, 1);
			drop_torch_item(3, Y, 2);
		}
		// transient torch on a stone post beside the ring, then break it (support-drop pile)
		set(8, Y, 4, BLOCK_STONE, 0);
		place_redstone_torch(8, Y + 1, 4);
		tick_wd();
		break_block(8, Y + 1, 4);
		tick_wd();
		// break + replace the gap cell (interrupt + complete the loop)
		break_block(4, Y, 1);
		tick_wd();
		place_redstone_dust(4, Y, 1);
		tick_wd();
		if(rep < 5 || (rep % 30) == 0)
			printf("  [C rep %d] gap meta=%d entities=%zu sched=%zu drops=%u\n", rep,
				   getmeta(4, Y, 1), entity_count(), s.world.scheduled_tick_count,
				   (unsigned)s.world.scheduled_tick_drops);
	}
	printf("C OK: 120 rapid close/reopen cycles with item pile + entity ticks\n");
	assert_redstone_work_bounds("C");

	// ========================================================================================
	// SCENARIO D: the FULLY faithful step-2 -> step-3 user sequence end to end:
	//   (i)   rapid place+break a real redstone TORCH on a post (each break support-drops a real
	//         item via torch_neighbour_change -> server_local_spawn_block_drops -> entity_item),
	//   (ii)  walk the PLAYER onto the dropped items so the entity pickup path (on_collect for the
	//         redstone-torch item) actually runs during the ticks,
	//   (iii) break the gap dust then place it back to COMPLETE the loop, items in flight.
	// This is the only path that spawns items through the REAL block-drop code and runs pickup.
	// ========================================================================================
	g_phase = "D-real-support-drop-pile";
	reset_redstone_work_bounds();
	set(12, Y, 12, BLOCK_STONE, 0); // a post for the torch to mount on
	for(int i = 0; i < 12; i++) {
		// place a real lit redstone torch on the post's +X side, then break the POST so the
		// torch's onNeighbourBlockChange support-drops the real item.
		struct item_data tit = {.id = BLOCK_REDSTONE_TORCH_LIT, .count = 1};
		struct block_data postb;
		server_world_get_block(&s.world, 12, Y, 12, &postb);
		struct block_info tw = {.x = 13, .y = Y, .z = 12};
		struct block_info ton = {.x = 12, .y = Y, .z = 12, .block = &postb};
		blocks[BLOCK_REDSTONE_TORCH_LIT]->block_item.onItemPlace(&s, &s.player, &tit, &tw, &ton, SIDE_RIGHT);
		tick_wd();
		break_block(12, Y, 12);	  // remove the post -> torch support-drops a REAL item entity
		tick_wd();
		set(12, Y, 12, BLOCK_STONE, 0); // rebuild the post for the next iteration
		tick_wd();
	}
	printf("D: real support-drop pile built via torch place/break; entities=%zu\n",
		   entity_count());

	// Walk the player onto the drop pile so pickup (on_collect for the torch item) runs.
	s.player.x = 12.5f;
	s.player.y = (float)Y;
	s.player.z = 12.5f;
	for(int k = 0; k < 30; k++)
		tick_wd();
	printf("D: after player-pickup pass, entities=%zu (some torch items collected)\n",
		   entity_count());

	// Now the user's exact step 3 on the live cycle from scenario A, items in flight.
	g_phase = "D-complete-loop-with-pickup";
	for(int rep = 0; rep < 60; rep++) {
		break_block(4, Y, 1); // interrupt
		tick_wd();
		tick_wd();
		place_redstone_dust(4, Y, 1); // complete the loop (real placer self-flood)
		tick_wd();
		tick_wd();
		if(rep < 5 || (rep % 20) == 0)
			printf("  [D rep %d] gap meta=%d entities=%zu sched=%zu drops=%u\n", rep,
				   getmeta(4, Y, 1), entity_count(), s.world.scheduled_tick_count,
				   (unsigned)s.world.scheduled_tick_drops);
	}
	printf("D OK: faithful support-drop + pickup + loop-complete sequence; entities=%zu\n",
		   entity_count());
	assert_redstone_work_bounds("D");

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_redstone_loop_close_drops_env: OK (no freeze / no crash)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
