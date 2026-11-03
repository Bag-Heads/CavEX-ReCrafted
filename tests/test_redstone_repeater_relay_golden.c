/*
	Golden (HUNT-14 item 1, Bucket A): a repeater's id swap must notify the NEIGHBOURS OF ITS
	NEIGHBOURS, so a repeater -> solid block -> wire/torch relay updates.

	Beta BlockRedstoneRepeater.onBlockAdded [740c583 BlockRedstoneRepeater.java:127-134] calls
	World.notifyBlocksOfNeighborChange(x+-1 / y+-1 / z+-1, this.blockID) for all SIX neighbours:
	each neighbour's own six neighbours get onNeighborBlockChange with var5 = the repeater id.
	Chunk.setBlockIDWithMetadata [Chunk.java:236-267] runs onBlockAdded on EVERY id swap, so both
	idle->active (updateTick -> setBlockAndMetadataWithNotify(redstoneRepeaterActive)) and
	active->idle fire it. Without it a solid block in front of the repeater is told about the
	change (6-neighbour notifyBlockChange) but the wire / torch on the far side of that block is
	not, and it keeps its stale power until something else touches it.

	Circuit (plane Y, row Z, stone floor at YF): torch SX=2 -> wire x=3..5 -> repeater RX=6
	(facing 1: input -X, output +X; delay index 0 = field_22023_b[0]*2 = 2 game-ticks
	[BlockRedstoneRepeater.java:14,81-83]) -> stone cube at 7 -> wire at 8 (behind the cube),
	plus a lit redstone torch standing on the cube at (7, Y+1).

	Expected (hand-derived from the decompile, NOT from this engine):
	  - rising edge at T: wire 3..5 powered at once; repeater lit after tick T+2
	    (scheduleBlockUpdate delay 2; World.tick increments worldTime before TickUpdates);
	    in that SAME tick onBlockAdded reaches wire 8 -> BlockRedstoneWire.onNeighborBlockChange
	    -> updateAndPropagateCurrentStrength -> isBlockIndirectlyGettingPowered(8) via the cube
	    (isBlockGettingPowered(cube) -> repeater.isPoweringTo(cube, side 4) with facing 1) = 15;
	    the torch on the cube gets onNeighborBlockChange -> scheduleBlockUpdate(tickRate 2)
	    [BlockRedstoneTorch.java] -> OFF after tick T+4 (lit after T+3).
	  - falling edge at T': repeater idle after T'+2, wire 8 -> 0 in that same tick, torch
	    re-lit after T'+4.
	The cube itself never changes, so only the 2-hop notify can carry the edge.

	HEADLESS HARNESS: same as test_redstone_repeater_circuit_env (RPC drain thread).
*/

#include <assert.h>
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
#include "block/redstone_logic.h" // REDSTONE_TORCH_TICK_RATE
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100
#define Y 101
#define Z 8
#define SX 2
#define RX 6
#define CX 7 // the relay cube
#define BX 8 // wire behind the cube
#define REP_META (1 | (0 << 2)) // facing 1 (input -X, output +X), delay idx 0 -> 2 ticks
#define REP_DELAY 2
#define TORCH_DELAY REDSTONE_TORCH_TICK_RATE

// Overworld-only fixture: every world access names &s.worlds[0] (the Overworld slot) explicitly.
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
	assert(server_world_set_block(&s.worlds[0], x, y, z,
								  (struct block_data) {.type = (uint8_t)type,
													   .metadata = (uint8_t)(meta & 0x0F),
													   .sky_light = 0,
													   .torch_light = 0}));
}
static int get(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? b.type : -1;
}
static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? (b.metadata & 0x0F) : -1;
}
static void tick(void) {
	server_world_tick(&s.worlds[0], &s);
}

int main(void) {
	test_fixture_dir("test_redstone_repeater_relay_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);

	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	for(int cz = -1; cz <= 1; cz++)
		for(int cx = -1; cx <= 1; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}

	for(int x = SX; x <= BX + 1; x++)
		set(x, YF, Z, BLOCK_STONE, 0);
	set(CX, Y, Z, BLOCK_STONE, 0);						  // the relay cube
	set(RX, Y, Z, BLOCK_REDSTONE_REPEATER, REP_META);	  // idle repeater aimed at the cube
	for(int x = 3; x <= 5; x++)
		set(x, Y, Z, BLOCK_REDSTONE_WIRE, 0);			  // input run
	set(BX, Y, Z, BLOCK_REDSTONE_WIRE, 0);				  // wire BEHIND the cube
	set(CX, Y + 1, Z, BLOCK_REDSTONE_TORCH_LIT, 5);		  // torch standing ON the cube

	for(int k = 0; k < 6; k++)
		tick();
	assert(get(RX, Y, Z) == BLOCK_REDSTONE_REPEATER);
	assert(getmeta(BX, Y, Z) == 0);
	assert(get(CX, Y + 1, Z) == BLOCK_REDSTONE_TORCH_LIT); // unpowered cube: torch stays lit

	// ---- RISING edge --------------------------------------------------------------------
	set(SX, Y, Z, BLOCK_REDSTONE_TORCH_LIT, 5);
	assert(getmeta(3, Y, Z) == 15); // the input run floods synchronously
	for(int k = 0; k < REP_DELAY - 1; k++) {
		tick();
		assert(get(RX, Y, Z) == BLOCK_REDSTONE_REPEATER);
		assert(getmeta(BX, Y, Z) == 0);
	}
	tick(); // T+2: repeater lights ...
	assert(get(RX, Y, Z) == BLOCK_REDSTONE_REPEATER_LIT);
	// ... and its onBlockAdded 2-hop notify reaches the wire behind the cube IN THIS TICK.
	assert(getmeta(BX, Y, Z) == 15
		   && "repeater->cube->wire: wire behind the cube must be powered when the repeater lights "
			  "(BlockRedstoneRepeater.onBlockAdded notifies the cube's neighbours)");
	// The torch on the cube is scheduled (tickRate 2) by that same notify: lit until T+4.
	for(int k = 0; k < TORCH_DELAY - 1; k++) {
		tick();
		assert(get(CX, Y + 1, Z) == BLOCK_REDSTONE_TORCH_LIT);
	}
	tick(); // T+4
	assert(get(CX, Y + 1, Z) == BLOCK_REDSTONE_TORCH
		   && "repeater->cube->torch: the torch on the powered cube must go out");
	for(int k = 0; k < 6; k++) {
		tick();
		assert(get(RX, Y, Z) == BLOCK_REDSTONE_REPEATER_LIT);
		assert(getmeta(BX, Y, Z) == 15);
		assert(get(CX, Y + 1, Z) == BLOCK_REDSTONE_TORCH);
	}

	// ---- FALLING edge -------------------------------------------------------------------
	set(SX, Y, Z, BLOCK_AIR, 0);
	assert(getmeta(3, Y, Z) == 0);
	for(int k = 0; k < REP_DELAY - 1; k++) {
		tick();
		assert(get(RX, Y, Z) == BLOCK_REDSTONE_REPEATER_LIT);
		assert(getmeta(BX, Y, Z) == 15);
	}
	tick(); // T'+2: repeater goes idle; the idle id's onBlockAdded carries the drop.
	assert(get(RX, Y, Z) == BLOCK_REDSTONE_REPEATER);
	assert(getmeta(BX, Y, Z) == 0
		   && "repeater->cube->wire: wire behind the cube must drop when the repeater goes idle");
	for(int k = 0; k < TORCH_DELAY - 1; k++) {
		tick();
		assert(get(CX, Y + 1, Z) == BLOCK_REDSTONE_TORCH);
	}
	tick(); // T'+4
	assert(get(CX, Y + 1, Z) == BLOCK_REDSTONE_TORCH_LIT
		   && "repeater->cube->torch: the torch must re-light when the cube loses power");
	for(int k = 0; k < 6; k++) {
		tick();
		assert(get(RX, Y, Z) == BLOCK_REDSTONE_REPEATER);
		assert(getmeta(BX, Y, Z) == 0);
		assert(get(CX, Y + 1, Z) == BLOCK_REDSTONE_TORCH_LIT);
	}

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	printf("test_redstone_repeater_relay_golden: OK (repeater id swap notifies 2 hops: cube-relayed "
		   "wire 0->15->0 in the swap tick, torch on the cube off/on at +%d)\n", TORCH_DELAY);
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
