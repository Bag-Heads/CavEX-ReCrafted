/*
	Golden (HUNT-14 item 3, Bucket A): a repeater is NOT a canProvidePower block, so the
	neighbour-id-gated consumers (door, trapdoor, TNT, dispenser, note block) ignore a notify whose
	ORIGIN is a repeater -- a repeater aimed straight at a door does not toggle it.

	Beta BlockRedstoneRepeater.canProvidePower [740c583 BlockRedstoneRepeater.java:113-115]
	returns false (Block.canProvidePower default; only wire/torch/lever/button/plates/detector
	rail override it true). The consumers gate their power re-read on
	the condition that the notifying neighbour id is positive and names a block whose
	canProvidePower is true:
	  BlockDoor.onNeighborBlockChange:141,165  BlockTrapDoor.onNeighborBlockChange
	  BlockTNT.onNeighborBlockChange:44        BlockDispenser.onNeighborBlockChange
	  BlockNote.onNeighborBlockChange
	When the repeater swaps id, every notify it emits (World.notifyBlockChange around itself,
	BlockRedstoneRepeater.onBlockAdded around its neighbours) carries the repeater id as the notifying neighbour id,
	so the door never re-reads its power even though isBlockIndirectlyGettingPowered(door)
	is TRUE (BlockRedstoneRepeater.isPoweringTo:63-70). That is the Beta quirk pinned here.

	Part A (pure): redstone_id_can_provide_power == Block.canProvidePower over the ids.
	Part B (env): torch -> wire -> repeater (facing +X, 2 ticks) -> wooden door directly in
	front stays CLOSED while powered; the same for a trapdoor. Positive control: a lever's
	notify DOES open the door (BlockLever.canProvidePower true), so the gate is the only
	thing keeping it shut.
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
#include "block/redstone_power.h"
#include "block/redstone_server.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100
#define Y 101
#define SX 2
#define RX 6
#define TX 7 // the target cell directly in front of the repeater
#define REP_META (1 | (0 << 2)) // facing 1 (input -X, output +X), delay idx 0 -> 2 ticks
#define REP_DELAY 2

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
static struct block_info info_at(int x, int y, int z) {
	static struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	return (struct block_info) {.block = &b, .neighbours = NULL, .x = x, .y = y, .z = z};
}
static void tick(void) {
	server_world_tick(&s.worlds[0], &s);
}

// One torch->wire->repeater driver per row z; the target sits at TX.
static void build_driver(int z) {
	for(int x = SX; x <= TX + 1; x++)
		set(x, YF, z, BLOCK_STONE, 0);
	set(RX, Y, z, BLOCK_REDSTONE_REPEATER, REP_META);
	for(int x = 3; x <= 5; x++)
		set(x, Y, z, BLOCK_REDSTONE_WIRE, 0);
}
static void drive_on(int z) {
	set(SX, Y, z, BLOCK_REDSTONE_TORCH_LIT, 5);
	for(int k = 0; k < REP_DELAY; k++)
		tick();
	assert(get(RX, Y, z) == BLOCK_REDSTONE_REPEATER_LIT);
}
static void drive_off(int z) {
	set(SX, Y, z, BLOCK_AIR, 0);
	for(int k = 0; k < REP_DELAY; k++)
		tick();
	assert(get(RX, Y, z) == BLOCK_REDSTONE_REPEATER);
}

int main(void) {
	// ---- Part A: Block.canProvidePower over the ids -------------------------------------
	assert(!redstone_id_can_provide_power(RSP_REPEATER_IDLE_ID)
		   && "BlockRedstoneRepeater.canProvidePower() is false (idle 93)");
	assert(!redstone_id_can_provide_power(RSP_REPEATER_ACTIVE_ID)
		   && "BlockRedstoneRepeater.canProvidePower() is false (active 94)");
	assert(redstone_id_can_provide_power(RSP_WIRE_ID));		 // BlockRedstoneWire
	assert(redstone_id_can_provide_power(RSP_TORCH_IDLE_ID)); // BlockRedstoneTorch (both ids)
	assert(redstone_id_can_provide_power(RSP_TORCH_LIT_ID));
	assert(redstone_id_can_provide_power(RSP_LEVER_ID));	 // BlockLever
	assert(redstone_id_can_provide_power(RSP_BUTTON_ID));	 // BlockButton
	assert(redstone_id_can_provide_power(RSP_STONE_PLATE_ID)); // BlockPressurePlate
	assert(redstone_id_can_provide_power(RSP_WOOD_PLATE_ID));
	assert(redstone_id_can_provide_power(RSP_DETECTOR_RAIL_ID)); // BlockDetectorRail
	assert(!redstone_id_can_provide_power(BLOCK_STONE));
	assert(!redstone_id_can_provide_power(0));

	// ---- Part B: live world -------------------------------------------------------------
	test_fixture_dir("test_redstone_repeater_direct_target_tmp", s_fixture_TMP_WORLD,
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

	// B1. DOOR directly in front of the repeater (bottom at TX,Y; top at TX,Y+1).
	const int ZD = 4;
	build_driver(ZD);
	set(TX, Y, ZD, BLOCK_DOOR_WOOD, 0);
	set(TX, Y + 1, ZD, BLOCK_DOOR_WOOD, 8);
	for(int k = 0; k < 4; k++)
		tick();
	assert((getmeta(TX, Y, ZD) & 4) == 0);

	drive_on(ZD);
	// The door IS powered (isBlockIndirectlyGettingPowered via isPoweringTo) ...
	assert(redstone_server_indirectly_getting_powered(&s, TX, Y, ZD));
	// ... yet nothing with canProvidePower()==true notified it, so it never re-read its power.
	assert((getmeta(TX, Y, ZD) & 4) == 0
		   && "repeater aimed at a door must NOT open it (origin id 94: canProvidePower false)");
	assert(getmeta(TX, Y + 1, ZD) == 8);
	for(int k = 0; k < 6; k++) {
		tick();
		assert((getmeta(TX, Y, ZD) & 4) == 0);
	}
	// Positive control: merely PLACING an OFF lever beside the door opens it. The placement is
	// World.setBlockWithNotify -> notifyBlockChange(var5 = 69) [World.java:444-447,498-505];
	// BlockLever.canProvidePower() is true [BlockLever.java:195], so BlockDoor re-reads
	// isBlockIndirectlyGettingPowered -- TRUE via the repeater (BlockRedstoneRepeater.isPoweringTo
	// :63-70) -- and opens [BlockDoor.java:163-165]. The lever itself supplies no power: the gate,
	// not a dead or unpowered door, kept it shut above.
	set(TX + 1, Y, ZD, BLOCK_LEVER, 5); // floor lever, OFF, on the stone at (TX+1, YF)
	assert((getmeta(TX + 1, Y, ZD) & 8) == 0);
	assert((getmeta(TX, Y, ZD) & 4) != 0
		   && "control: a canProvidePower origin (lever placement) makes the repeater-powered "
			  "door re-read its power and open");
	{
		struct block_info on = info_at(TX + 1, Y, ZD);
		block_lever.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_TOP); // lever ON
	}
	assert((getmeta(TX + 1, Y, ZD) & 8) != 0);
	assert((getmeta(TX, Y, ZD) & 4) != 0);
	{
		struct block_info on = info_at(TX + 1, Y, ZD);
		block_lever.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_TOP); // lever OFF
	}
	// Lever off, repeater still powering the door: BlockDoor.onNeighborBlockChange re-reads
	// isBlockIndirectlyGettingPowered (TRUE via the repeater) on the LEVER's notify -> stays open.
	assert((getmeta(TX, Y, ZD) & 4) != 0);
	set(TX + 1, Y, ZD, BLOCK_AIR, 0);
	drive_off(ZD);
	// Torch off: the wire at x=5 drops to 0 at once and notifies around (6,Y) with origin 55
	// [BlockRedstoneWire.java:45-53,160-167] while the repeater is STILL lit, so the door re-reads
	// TRUE and stays open; 2 ticks later the repeater goes idle with only repeater-origin
	// notifies -> the door stays as it was (open).
	assert((getmeta(TX, Y, ZD) & 4) != 0
		   && "repeater going idle must NOT close the door either (same gate)");

	// B2. TRAPDOOR directly in front (hangs from the stone at TX+1: facing 2 -> support x+1,
	//     block_trapdoor.c offsets / BlockTrapDoor.onNeighborBlockChange support side).
	const int ZT = 12;
	build_driver(ZT);
	set(TX + 1, Y, ZT, BLOCK_STONE, 0);
	set(TX, Y, ZT, BLOCK_TRAP_DOOR, 2);
	for(int k = 0; k < 4; k++)
		tick();
	assert(get(TX, Y, ZT) == BLOCK_TRAP_DOOR && (getmeta(TX, Y, ZT) & 4) == 0);
	drive_on(ZT);
	assert(redstone_server_indirectly_getting_powered(&s, TX, Y, ZT));
	assert((getmeta(TX, Y, ZT) & 4) == 0
		   && "repeater aimed at a trapdoor must NOT open it (origin canProvidePower false)");
	for(int k = 0; k < 6; k++) {
		tick();
		assert((getmeta(TX, Y, ZT) & 4) == 0);
	}
	// Torch off: the wire at x=5 falls to 0 synchronously and notifies the neighbours of (6,Y)
	// with origin 55 (BlockRedstoneWire.updateAndPropagateCurrentStrength:45-53 over the set
	// filled at :160-167, var9 == 0). BlockRedstoneWire.canProvidePower is true, so the trapdoor
	// re-reads isBlockIndirectlyGettingPowered [BlockTrapDoor.java:120-122] -- still TRUE, the
	// repeater has 2 ticks left -- and OPENS. The repeater then goes idle with repeater-origin
	// notifies only, which the trapdoor ignores: it is left open with no power at all.
	drive_off(ZT);
	assert(!redstone_server_indirectly_getting_powered(&s, TX, Y, ZT));
	assert((getmeta(TX, Y, ZT) & 4) != 0
		   && "wire-off notify (origin 55) opens the still-repeater-powered trapdoor; the "
			  "repeater going idle (origin 93) does not close it");

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	printf("test_redstone_repeater_direct_target_golden: OK (repeater ids 93/94 are not "
		   "canProvidePower origins: a repeater aimed at a door/trapdoor powers but never toggles "
		   "it; lever/wire-origin notifies make it re-read)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
