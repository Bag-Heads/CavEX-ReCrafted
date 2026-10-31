/*
	FAITHFULNESS env test (Beta 1.7.3, decompile 740c583): the stone BUTTON (BlockButton, id 77)
	and the door's redstone rewiring (BlockDoor.onNeighborBlockChange / onPoweredBlockChange),
	driven over a live superflat server_world (the test_redstone_wire_place_flood_env harness).

	BUTTON (new component): wall-mount placement (meta 1..4 = mount at x-1/x+1/z-1/z+1); press =
	meta|8 + notify self + mount-cell neighbours + a SELF-scheduled release tickRate()==20
	game-ticks out (GAP #1 suppresses any auto-schedule -- the block defines BOTH onWorldTick and
	onNeighbourBlockChange); isPoweringTo = pressed on every side (a wire beside it latches 15);
	support-drop when the mount wall is removed.

	DOOR (rewired): Beta has ONE open bit (0x04); redstone drives it ONLY when the notify origin
	canProvidePower (the var5 gate, via server_world_notify_origin) with
	powered = isBlockIndirectlyGettingPowered(bottom) || (top). Guards:
	  * a lever beside the TOP half opens the door (the old code early-returned on the top half,
	    so top-side power never worked);
	  * a NON-power neighbour change must NOT touch a hand-opened door (the var5 gate);
	  * cutting power CLOSES a door that was opened BY HAND (the Beta quirk: onPoweredBlockChange
	    flips whenever open != powered);
	  * the iron door has NO hand toggle (Beta blockActivated returns for Material.iron).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "network/client_interface.h"
#include "item/items.h"
#include "network/inventory_logic.h" /* E2E RPC placement section */
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_rs_button_door_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y 101  // circuit plane

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
	assert(server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)}));
}
static int get(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.type : -1;
}
static int getmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? (b.metadata & 0x0F) :
															 -1;
}
static void tick(void) { server_world_tick(&s.world, &s); }

static struct block_data g_bd;
static struct block_info info_at(int x, int y, int z) {
	assert(server_world_get_block(&s.world, x, y, z, &g_bd));
	return (struct block_info) {
		.block = &g_bd, .neighbours = NULL, .x = x, .y = y, .z = z};
}

int main(void) {
	test_fixture_dir("test_rs_button_door_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init(); /* the E2E RPC place path resolves items[] for block items */
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0; // unlimited: the button's release tick must fire
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	for(int cz = -1; cz <= 1; cz++)
		for(int cx = -1; cx <= 1; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}
	// Keep the player far from the circuits (the door's nudge carve-out reads s.player).
	s.player.x = 1000.0;
	s.player.y = 1000.0;
	s.player.z = 1000.0;

	// stone floor under everything
	for(int x = 1; x <= 12; x++)
		for(int z = 3; z <= 9; z++)
			set(x, YF, z, BLOCK_STONE, 0);

	// ================== BUTTON ==================
	const int ZB = 4;
	// Mount cube at (2,Y,ZB); button placed on its +X face -> button at (3,Y,ZB), meta 1.
	set(2, Y, ZB, BLOCK_STONE, 0);
	{
		struct block_info on = info_at(2, Y, ZB);
		struct block_info where = {
			.block = NULL, .neighbours = NULL, .x = 3, .y = Y, .z = ZB};
		struct item_data it = {
			.id = BLOCK_STONE_BUTTON, .durability = 0, .count = 1};
		assert(block_stone_button.block_item.onItemPlace(&s, &s.player, &it, &where, &on,
														 SIDE_RIGHT));
	}
	assert(get(3, Y, ZB) == BLOCK_STONE_BUTTON);
	assert(getmeta(3, Y, ZB) == 1); // mount at x-1

	// A wire dot east of the button: unpressed button provides nothing.
	set(4, Y, ZB, BLOCK_REDSTONE_WIRE, 0);
	assert(getmeta(4, Y, ZB) == 0);

	// Floor/ceiling placement is rejected (Beta canPlaceBlockOnSide: wall sides only).
	{
		struct block_info on = info_at(2, Y, ZB);
		struct block_info where = {
			.block = NULL, .neighbours = NULL, .x = 2, .y = Y + 1, .z = ZB};
		struct item_data it = {
			.id = BLOCK_STONE_BUTTON, .durability = 0, .count = 1};
		assert(!block_stone_button.block_item.onItemPlace(&s, &s.player, &it, &where, &on,
														  SIDE_TOP));
	}

	// PRESS: bit 8 sets, the adjacent wire latches 15 (BlockButton.isPoweringTo, all sides).
	{
		struct block_info on = info_at(3, Y, ZB);
		block_stone_button.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_RIGHT);
	}
	assert((getmeta(3, Y, ZB) & 8) != 0);
	assert(getmeta(4, Y, ZB) == 15);

	// EXACT Beta tickRate()==20: still held through the 19th tick, released on the 20th.
	for(int k = 0; k < 19; k++)
		tick();
	assert((getmeta(3, Y, ZB) & 8) != 0);
	assert(getmeta(4, Y, ZB) == 15);
	tick();
	assert((getmeta(3, Y, ZB) & 8) == 0);
	assert(getmeta(3, Y, ZB) == 1); // orientation preserved
	assert(getmeta(4, Y, ZB) == 0);

	// RE-PRESS after a release must hold the FULL 20 again -- guards the stale-queue-entry
	// class (an ungated neighbour auto-schedule + keep-first dedup shortened the press).
	for(int k = 0; k < 5; k++)
		tick();
	{
		struct block_info on = info_at(3, Y, ZB);
		block_stone_button.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_RIGHT);
	}
	for(int k = 0; k < 19; k++)
		tick();
	assert((getmeta(3, Y, ZB) & 8) != 0);
	tick();
	assert((getmeta(3, Y, ZB) & 8) == 0);

	// SUPPORT-DROP: press again, then mine the mount -- the button pops and the wire unpowers.
	{
		struct block_info on = info_at(3, Y, ZB);
		block_stone_button.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_RIGHT);
	}
	assert(getmeta(4, Y, ZB) == 15);
	set(2, Y, ZB, BLOCK_AIR, 0);
	assert(get(3, Y, ZB) == BLOCK_AIR);
	assert(getmeta(4, Y, ZB) == 0);

	// THROUGH-CUBE WEAK POWER (BlockButton.isIndirectlyPoweringTo, the mount face): a wire on
	// the FAR side of the mount cube reads the strong power the cube re-emits. This is the
	// path a transposed orientation->side map would break while every direct-adjacency test
	// stayed green.
	const int ZC = 5;
	set(9, Y, ZC, BLOCK_STONE, 0); // the mount cube
	{
		struct block_info on = info_at(9, Y, ZC);
		struct block_info where = {
			.block = NULL, .neighbours = NULL, .x = 10, .y = Y, .z = ZC};
		struct item_data it = {
			.id = BLOCK_STONE_BUTTON, .durability = 0, .count = 1};
		assert(block_stone_button.block_item.onItemPlace(&s, &s.player, &it, &where, &on,
														 SIDE_RIGHT));
	}
	set(8, Y, ZC, BLOCK_REDSTONE_WIRE, 0); // far side of the cube
	assert(getmeta(8, Y, ZC) == 0);
	{
		struct block_info on = info_at(10, Y, ZC);
		block_stone_button.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_RIGHT);
	}
	assert(getmeta(8, Y, ZC) == 15); // strong power THROUGH the mount cube
	for(int k = 0; k < 20; k++)
		tick();
	assert(getmeta(8, Y, ZC) == 0); // release un-powers through the cube too

	// ================== DOOR ==================
	const int ZD = 7;
	// Wooden door: bottom (facing 0, closed) + top (bit 8).
	set(5, Y, ZD, BLOCK_DOOR_WOOD, 0);
	set(5, Y + 1, ZD, BLOCK_DOOR_WOOD, 8);
	assert(get(5, Y, ZD) == BLOCK_DOOR_WOOD);

	// Floor lever east of the BOTTOM half.
	set(6, Y, ZD, BLOCK_LEVER, 5); // floor lever, OFF, mount = the stone below
	assert((getmeta(5, Y, ZD) & 4) == 0);

	// Lever ON -> origin is a power provider -> door opens; the top half mirrors (open|8).
	{
		struct block_info on = info_at(6, Y, ZD);
		block_lever.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_TOP);
	}
	assert((getmeta(6, Y, ZD) & 8) != 0); // lever really ON
	assert((getmeta(5, Y, ZD) & 4) != 0); // door open
	assert(getmeta(5, Y + 1, ZD) == (getmeta(5, Y, ZD) | 8));

	// Lever OFF -> closes.
	{
		struct block_info on = info_at(6, Y, ZD);
		block_lever.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_TOP);
	}
	assert((getmeta(5, Y, ZD) & 4) == 0);

	// HAND-OPEN, then a NON-power neighbour change: the var5 gate must leave it open.
	{
		struct block_info on = info_at(5, Y, ZD);
		block_wooden_door.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_FRONT);
	}
	assert((getmeta(5, Y, ZD) & 4) != 0);
	set(4, Y, ZD, BLOCK_STONE, 0); // origin = stone: canProvidePower false
	assert((getmeta(5, Y, ZD) & 4) != 0); // still open
	set(4, Y, ZD, BLOCK_AIR, 0);
	assert((getmeta(5, Y, ZD) & 4) != 0);

	// THE BETA QUIRK: with the door hand-opened and NO power present, a power-capable origin
	// (the lever toggling ON then OFF) closes it -- onPoweredBlockChange flips on open!=powered.
	{
		struct block_info on = info_at(6, Y, ZD);
		block_lever.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_TOP); // ON: open stays
	}
	assert((getmeta(5, Y, ZD) & 4) != 0);
	{
		struct block_info on = info_at(6, Y, ZD);
		block_lever.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_TOP); // OFF: closes it
	}
	assert((getmeta(5, Y, ZD) & 4) == 0);

	// TOP-HALF POWER (the old code's early-return bug): a wall lever adjacent ONLY to the top
	// half must open the door. Pillar at (7,Y..Y+1,ZD); lever at (6,Y+1,ZD) meta 2 (mount x+1).
	set(7, Y, ZD, BLOCK_STONE, 0);
	set(7, Y + 1, ZD, BLOCK_STONE, 0);
	set(6, Y + 1, ZD, BLOCK_LEVER, 2);
	{
		struct block_info on = info_at(6, Y + 1, ZD);
		block_lever.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_LEFT);
	}
	assert((getmeta(6, Y + 1, ZD) & 8) != 0);
	assert((getmeta(5, Y, ZD) & 4) != 0); // opened THROUGH the top half
	assert(getmeta(5, Y + 1, ZD) == (getmeta(5, Y, ZD) | 8));

	// SUPPORT-DROP: mine the block under the bottom half -> BOTH halves clear.
	set(5, YF, ZD, BLOCK_AIR, 0);
	assert(get(5, Y, ZD) == BLOCK_AIR);
	assert(get(5, Y + 1, ZD) == BLOCK_AIR);

	// ================== IRON DOOR (behavioral) ==================
	// The canonical Beta circuit: button on a frame cube, iron door beside the cube. The
	// press must open it THROUGH the cube (this needs the mount-cell notify to carry
	// origin=buttonID -- the var5 the door's gate demands), the 20-tick release must close
	// it, and the HAND must do nothing (Beta blockActivated returns true for Material.iron).
	const int ZI = 9;
	set(9, Y, ZI, BLOCK_STONE, 0); // frame cube
	{
		struct block_info on = info_at(9, Y, ZI);
		struct block_info where = {
			.block = NULL, .neighbours = NULL, .x = 10, .y = Y, .z = ZI};
		struct item_data it = {
			.id = BLOCK_STONE_BUTTON, .durability = 0, .count = 1};
		assert(block_stone_button.block_item.onItemPlace(&s, &s.player, &it, &where, &on,
														 SIDE_RIGHT));
	}
	set(8, Y, ZI, BLOCK_DOOR_IRON, 0);
	set(8, Y + 1, ZI, BLOCK_DOOR_IRON, 8);
	// Hand: consumed, no toggle.
	assert(blocks[BLOCK_DOOR_IRON]->onRightClick != NULL);
	{
		struct block_info on = info_at(8, Y, ZI);
		blocks[BLOCK_DOOR_IRON]->onRightClick(&s, &s.player, NULL, &on, &on, SIDE_FRONT);
	}
	assert((getmeta(8, Y, ZI) & 4) == 0); // still closed
	// Button press opens it through the frame cube...
	{
		struct block_info on = info_at(10, Y, ZI);
		block_stone_button.onRightClick(&s, &s.player, NULL, &on, &on, SIDE_RIGHT);
	}
	assert((getmeta(8, Y, ZI) & 4) != 0);
	assert(getmeta(8, Y + 1, ZI) == (getmeta(8, Y, ZI) | 8));
	// ...and the 20-tick release closes it again.
	for(int k = 0; k < 20; k++)
		tick();
	assert((getmeta(8, Y, ZI) & 4) == 0);

	for(int k = 0; k < 30; k++)
		tick();

	/* ============== E2E RPC placement (2026-08-07 caveat batch 1c) ==========
	   The hardware video aimed a button at the TOP of a block and read the
	   no-op as a bug; Beta buttons are wall-only. Drive the REAL
	   SRPC_BLOCK_PLACE dispatch (client raycast -> RPC -> onItemPlace) for
	   all six faces of an isolated stone pillar + a non-solid mount:
	     - four wall faces place at the face-adjacent cell with the exact
	       Beta side->orientation meta (RIGHT->1 LEFT->2 BACK->3 FRONT->4,
	       [740c583 BlockButton.onBlockPlaced]) and consume the item;
	     - TOP and BOTTOM place NOTHING and consume NOTHING
	       [740c583 BlockButton.canPlaceBlockOnSide: sides 2..5 only];
	     - a GLASS mount (not a normal cube) rejects every face. */
	{
		const int px = 4, pz = 24; /* isolated pillar on the chunk the player
									  area already resident */
		set(px, Y, pz, BLOCK_STONE, 0);
		assert(inventory_create(&s.player.inventory, &inventory_logic_player,
								&s, &s.player, INVENTORY_SIZE, 0, 0, 0));
		s.player.has_pos = true;
		s.player.finished_loading = true;
		/* Deviation-#18: zero-init health reads as DEAD; be explicitly alive. */
		s.player.health = MAX_PLAYER_HEALTH;
		s.player.active_inventory = &s.player.inventory;
		s.player.x = px + 0.5;
		s.player.y = Y + 1.62;
		s.player.z = pz + 3.5;
		inventory_set_hotbar(&s.player.inventory, 0);

		static const struct {
			enum side side;
			int want_meta;
		} walls[4] = {
			{SIDE_RIGHT, 1}, /* clicked +X face -> mount at button's x-1 */
			{SIDE_LEFT, 2},
			{SIDE_BACK, 3},
			{SIDE_FRONT, 4},
		};
		for(int w = 0; w < 4; w++) {
			int ox = 0, oy = 0, oz = 0;
			blocks_side_offset(walls[w].side, &ox, &oy, &oz);
			int bx = px + ox, by = Y + oy, bz = pz + oz;
			assert(get(bx, by, bz) == BLOCK_AIR);
			inventory_set_slot(
				&s.player.inventory, INVENTORY_SLOT_HOTBAR,
				(struct item_data) {.id = BLOCK_STONE_BUTTON, .count = 1});
			struct server_rpc place = {
				.type = SRPC_BLOCK_PLACE,
				.payload.block_place
				= {.x = px, .y = Y, .z = pz, .side = walls[w].side},
			};
			server_local_test_process_rpc(&s, &place);
			assert(get(bx, by, bz) == BLOCK_STONE_BUTTON);
			assert(getmeta(bx, by, bz) == walls[w].want_meta);
			struct item_data held;
			bool have = inventory_get_slot(&s.player.inventory,
										   INVENTORY_SLOT_HOTBAR, &held);
			assert(!have || held.id == 0
				   || held.count == 0); /* the one button was consumed */
			set(bx, by, bz, BLOCK_AIR, 0); /* clear for the next face */
		}

		/* TOP + BOTTOM: refused, nothing placed, nothing consumed. */
		static const enum side flats[2] = {SIDE_TOP, SIDE_BOTTOM};
		for(int f = 0; f < 2; f++) {
			int ox = 0, oy = 0, oz = 0;
			blocks_side_offset(flats[f], &ox, &oy, &oz);
			int bx = px + ox, by = Y + oy, bz = pz + oz;
			int before = get(bx, by, bz);
			inventory_set_slot(
				&s.player.inventory, INVENTORY_SLOT_HOTBAR,
				(struct item_data) {.id = BLOCK_STONE_BUTTON, .count = 1});
			struct server_rpc place = {
				.type = SRPC_BLOCK_PLACE,
				.payload.block_place
				= {.x = px, .y = Y, .z = pz, .side = flats[f]},
			};
			server_local_test_process_rpc(&s, &place);
			assert(get(bx, by, bz) == before
				   && "top/bottom button placement must be a no-op");
			struct item_data held;
			assert(inventory_get_slot(&s.player.inventory,
									  INVENTORY_SLOT_HOTBAR, &held)
				   && held.id == BLOCK_STONE_BUTTON && held.count == 1);
		}

		/* Non-solid mount: a glass pillar refuses wall placement too. */
		const int gx = 8, gz = 24;
		set(gx, Y, gz, BLOCK_GLASS, 0);
		{
			int ox = 0, oy = 0, oz = 0;
			blocks_side_offset(SIDE_RIGHT, &ox, &oy, &oz);
			int bx = gx + ox, by = Y + oy, bz = gz + oz;
			inventory_set_slot(
				&s.player.inventory, INVENTORY_SLOT_HOTBAR,
				(struct item_data) {.id = BLOCK_STONE_BUTTON, .count = 1});
			struct server_rpc place = {
				.type = SRPC_BLOCK_PLACE,
				.payload.block_place
				= {.x = gx, .y = Y, .z = gz, .side = SIDE_RIGHT},
			};
			server_local_test_process_rpc(&s, &place);
			assert(get(bx, by, bz) == BLOCK_AIR
				   && "a non-solid mount must refuse the button");
			struct item_data held;
			assert(inventory_get_slot(&s.player.inventory,
									  INVENTORY_SLOT_HOTBAR, &held)
				   && held.id == BLOCK_STONE_BUTTON && held.count == 1);
		}
		puts("  E2E RPC: 4 wall faces place metas 1/2/3/4 + consume; "
			 "top/bottom + glass mount refuse + keep the item");
	}

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_redstone_button_door_env: OK (button exact-20 press/re-press, through-cube "
		   "weak power, support-drop; door var5-gated redstone incl. top-half power, hand-open "
		   "persistence, the power-cut close quirk; iron door hand-proof + button-through-cube "
		   "open/close)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
