/* Card m1 (docs/intent/m1-mp-arm-swing.md): the local player's arm swing on
 * the multiplayer wire -- Packet18Animation(self, 1), sent exactly where
 * Beta 1.7.3's EntityClientPlayerMP.swingItem is called
 * [EntityClientPlayerMP.java:118-121].
 *
 * Every expectation below is derived from the decompile (740c583
 * minecraft/src_original), never recorded from CaveWii output:
 *   - Minecraft.clickMouse(0) swings FIRST, then attackEntity's Packet7 or
 *     clickBlock's Packet14 status 0 [Minecraft.java:789-791,801,813;
 *     PlayerControllerMP.java:41-43,159-163]; held, it repeats every 5 ticks
 *     [Minecraft.java:1000-1003]; into air it arms leftClickCounter = 10
 *     [Minecraft.java:795-797,991-993].
 *   - Minecraft.clickMouse(1) swings AFTER sendPlaceBlock's Packet15, iff it
 *     returned true [Minecraft.java:817-820; PlayerControllerMP.java:141-146;
 *     PlayerController.java:75-78].
 *   - ItemFishingRod.onItemRightClick swings after sendUseItem's Packet15(-1)
 *     [ItemFishingRod.java:22,29; PlayerControllerMP.java:148-153]; bow/egg/
 *     snowball/boat do not.
 *   - EntityPlayerSP (single player) sends nothing.
 *
 * Part 1 pins the pure cadence + place predicate (game/mp_arm_swing.h).
 * Part 2 drives the REAL in-game action path (screen_ingame.c) through the
 * REAL svin mailbox into the REAL remote provider (remote_process_srpc) and
 * reads the bytes it queued for the socket, in order.
 *
 * RED build: -DMP_ARM_SWING_BASE_RED drops Part 1 (it names symbols the fix
 * adds) so Part 2 compiles against the base and shows the missing Packet18.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "entity/entity.h"
#include "entity/entity_monster.h" /* the shared left-click attack handler */
#include "game/agent_input.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "item/inventory.h"
#include "item/item_types.h"
#include "item/items.h"
#include "item/window_container.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_remote.h"
#include "network/server_remote_internal.h"
#include "particle.h"
#include "platform/input.h"
#include "platform/thread.h"
#include "world.h"
#ifndef MP_ARM_SWING_BASE_RED
#include "game/mp_arm_swing.h"
#endif

#define LOCAL_EID 1234
#define TARGET_EID 777
#define HIT_X 4
#define HIT_Y 100
#define HIT_Z 4
#define CHEST_X 8

#ifndef MP_ARM_SWING_BASE_RED
/* ---- Part 1: pure goldens ------------------------------------------------ */

static void test_left_click_cadence(void) {
	/* Block/entity target: press swings, the held button every 5 ticks. */
	struct mp_swing_clock c = {0};
	assert(mp_swing_left_click(&c, 1000, true, true, false));
	assert(!mp_swing_left_click(&c, 1100, false, true, false));
	assert(!mp_swing_left_click(&c, 1249, false, true, false));
	assert(mp_swing_left_click(&c, 1250, false, true, false));
	assert(!mp_swing_left_click(&c, 1499, false, true, false));
	assert(mp_swing_left_click(&c, 1500, false, true, false));
	assert(!mp_swing_left_click(&c, 1600, false, false, false)); /* released */
	/* A fresh press is the Mouse event path: no 5-tick wait. */
	assert(mp_swing_left_click(&c, 1610, true, true, false));

	/* Air: leftClickCounter = 10 blocks clickMouse(0) (mouseTicksRan is still
	 * re-stamped at tick 5), so held air swings land on ticks 0, 10, 20. */
	struct mp_swing_clock a = {0};
	assert(mp_swing_left_click(&a, 0, true, true, true));
	assert(!mp_swing_left_click(&a, 250, false, true, true));
	assert(a.last_click_ms == 250);
	assert(!mp_swing_left_click(&a, 499, false, true, true));
	assert(mp_swing_left_click(&a, 500, false, true, true));
	assert(!mp_swing_left_click(&a, 750, false, true, true));
	assert(mp_swing_left_click(&a, 1000, false, true, true));
	/* A press inside the air lockout is swallowed like Beta's gated call. */
	struct mp_swing_clock l = {0};
	assert(mp_swing_left_click(&l, 0, true, true, true));
	assert(!mp_swing_left_click(&l, 100, true, true, false));
	assert(mp_swing_left_click(&l, 500, true, true, false));
	/* A button already down with no click on record: runTick's held check
	 * compares against mouseTicksRan and fires at once. */
	struct mp_swing_clock h = {0};
	assert(mp_swing_left_click(&h, 42, false, true, false));
	/* The forced click (dig start / attack on a non-cadence frame) restarts
	 * the 5-tick clock like a real clickMouse(0). */
	struct mp_swing_clock f = {0};
	assert(mp_swing_left_click(&f, 0, true, true, false));
	mp_swing_left_click_forced(&f, 100);
	assert(!mp_swing_left_click(&f, 250, false, true, false));
	assert(mp_swing_left_click(&f, 350, false, true, false));
	assert(MP_SWING_REPEAT_MS == 5 * 50 && MP_SWING_AIR_LOCKOUT_MS == 10 * 50);
	puts("PASS left-click cadence: press swings, held every 5 ticks, air every "
		 "10 (leftClickCounter), forced click restarts the clock");
}

/* A 16x128x16 fake World for the place predicate. */
static uint8_t fw_id[16][128][16], fw_meta[16][128][16];
static uint8_t fw_block(void* ctx, int x, int y, int z) {
	(void)ctx;
	if(y < 0 || y > 127 || x < 0 || x > 15 || z < 0 || z > 15)
		return 0;
	return fw_id[x][y][z];
}
static uint8_t fw_meta_at(void* ctx, int x, int y, int z) {
	(void)ctx;
	if(y < 0 || y > 127 || x < 0 || x > 15 || z < 0 || z > 15)
		return 0;
	return fw_meta[x][y][z];
}
static void fw_clear(void) {
	memset(fw_id, 0, sizeof fw_id);
	memset(fw_meta, 0, sizeof fw_meta);
}
static void fw_set(int x, int y, int z, uint8_t id, uint8_t meta) {
	fw_id[x][y][z] = id;
	fw_meta[x][y][z] = meta;
}

static bool place(int x, int y, int z, int face, uint16_t item, uint16_t dmg,
				  uint8_t count, float yaw) {
	struct mp_swing_world w = {.ctx = NULL, .block = fw_block,
		.meta = fw_meta_at};
	return mp_swing_after_place(&w, x, y, z, face, item != 0, item, dmg, count,
								yaw);
}

static void test_place_predicate(void) {
	fw_clear();
	/* blockActivated on a multiplayer world: activatable targets swing even
	 * with an empty hand [PlayerController.java:76]. */
	static const uint8_t activated[] = {BLOCK_DISPENSER, BLOCK_NOTEBLOCK,
		BLOCK_BED, BLOCK_CHEST, BLOCK_WORKBENCH, BLOCK_FURNACE,
		BLOCK_FURNACE_LIT, BLOCK_DOOR_WOOD, BLOCK_DOOR_IRON, BLOCK_LEVER,
		BLOCK_STONE_BUTTON, BLOCK_CAKE, BLOCK_REDSTONE_REPEATER,
		BLOCK_REDSTONE_REPEATER_LIT, BLOCK_TRAP_DOOR};
	for(size_t i = 0; i < sizeof activated; i++) {
		fw_set(5, 60, 5, activated[i], 0);
		assert(place(5, 60, 5, 1, 0, 0, 0, 0.0F));
	}
	/* BlockJukeBox: empty (meta 0) -> false; with a record -> true. */
	fw_set(5, 60, 5, BLOCK_JUKEBOX, 0);
	assert(!place(5, 60, 5, 1, 0, 0, 0, 0.0F));
	fw_set(5, 60, 5, BLOCK_JUKEBOX, 1);
	assert(place(5, 60, 5, 1, 0, 0, 0, 0.0F));
	/* ItemRecord into the empty jukebox: true; into a full one the jukebox
	 * itself already answered true. */
	fw_set(5, 60, 5, BLOCK_JUKEBOX, 0);
	assert(place(5, 60, 5, 1, ITEM_RECORD_13, 0, 1, 0.0F));
	/* Non-activated blocks (stairs defer to planks/cobble; piston, TNT,
	 * redstone ore are false) with an empty hand: no swing. */
	static const uint8_t inert[] = {BLOCK_STONE, 53, 67, 33, 29, 46, 73, 74};
	for(size_t i = 0; i < sizeof inert; i++) {
		fw_set(5, 60, 5, inert[i], 0);
		assert(!place(5, 60, 5, 1, 0, 0, 0, 0.0F));
	}

	/* ItemBlock: stepped cell must hold air/water/lava/fire/snow layer. */
	fw_clear();
	fw_set(5, 60, 5, BLOCK_STONE, 0);
	assert(place(5, 60, 5, 1, BLOCK_COBBLESTONE, 0, 64, 0.0F));
	assert(!place(5, 60, 5, 1, BLOCK_COBBLESTONE, 0, 0, 0.0F)); /* stack 0 */
	fw_set(5, 61, 5, BLOCK_DIRT, 0);
	assert(!place(5, 60, 5, 1, BLOCK_COBBLESTONE, 0, 64, 0.0F));
	fw_set(5, 61, 5, BLOCK_WATER_STILL, 0);
	assert(place(5, 60, 5, 1, BLOCK_COBBLESTONE, 0, 64, 0.0F));
	fw_set(5, 61, 5, BLOCK_FIRE, 0);
	assert(place(5, 60, 5, 1, BLOCK_COBBLESTONE, 0, 64, 0.0F));
	/* Face steps: -X face of (5,60,5) is (4,60,5). */
	fw_set(4, 60, 5, BLOCK_DIRT, 0);
	assert(!place(5, 60, 5, 4, BLOCK_COBBLESTONE, 0, 64, 0.0F));
	assert(place(5, 60, 5, 5, BLOCK_COBBLESTONE, 0, 64, 0.0F));
	/* Clicking a snow layer places INTO it (no face step). */
	fw_set(6, 70, 6, BLOCK_SNOW, 0);
	fw_set(6, 71, 6, BLOCK_STONE, 0);
	assert(place(6, 70, 6, 1, BLOCK_COBBLESTONE, 0, 64, 0.0F));
	/* ItemBlock's top-of-world refusal for solid blocks at y = 127 only. */
	fw_set(7, 126, 7, BLOCK_STONE, 0);
	assert(!place(7, 126, 7, 1, BLOCK_COBBLESTONE, 0, 64, 0.0F));
	assert(place(7, 126, 7, 1, 50 /* torch: circuits, not solid */, 0, 64,
				 0.0F));
	/* ItemReed (sugar cane) has no y = 127 refusal. */
	assert(place(7, 126, 7, 1, ITEM_REED, 0, 64, 0.0F));
	assert(place(7, 126, 7, 1, ITEM_CAKE, 0, 1, 0.0F));
	/* ItemReed.onItemUse returns true for a nonempty stack even when the
	 * stepped destination cannot accept a block [ItemReed.java:40-53]. */
	fw_set(7, 127, 7, BLOCK_DIRT, 0);
	static const uint16_t reed_items[] = {
		ITEM_REED, ITEM_CAKE, ITEM_REDSTONE_REPEATER};
	for(size_t i = 0; i < sizeof reed_items / sizeof reed_items[0]; i++) {
		assert(place(7, 126, 7, 1, reed_items[i], 0, 1, 0.0F));
		assert(!place(7, 126, 7, 1, reed_items[i], 0, 0, 0.0F));
	}
	/* The same blocked destination still refuses an ordinary ItemBlock. */
	assert(!place(7, 126, 7, 1, BLOCK_COBBLESTONE, 0, 1, 0.0F));

	/* Items without onItemUse (a stick, a sword, a bucket, the rod). */
	fw_clear();
	fw_set(5, 60, 5, BLOCK_STONE, 0);
	assert(!place(5, 60, 5, 1, ITEM_STICK, 0, 1, 0.0F));
	assert(!place(5, 60, 5, 1, ITEM_FISHING_ROD, 0, 1, 0.0F));
	assert(!place(5, 60, 5, 1, ITEM_BUCKET, 0, 1, 0.0F));
	/* ItemFlintAndSteel: always true. */
	assert(place(5, 60, 5, 0, ITEM_FLINT_STEEL, 0, 1, 0.0F));
	/* ItemHoe: grass with air above on a non-bottom face; dirt always. */
	fw_set(5, 60, 5, BLOCK_GRASS, 0);
	assert(place(5, 60, 5, 1, ITEM_IRON_HOE, 0, 1, 0.0F));
	assert(!place(5, 60, 5, 0, ITEM_IRON_HOE, 0, 1, 0.0F));
	fw_set(5, 61, 5, BLOCK_STONE, 0);
	assert(!place(5, 60, 5, 1, ITEM_IRON_HOE, 0, 1, 0.0F));
	fw_set(5, 60, 5, BLOCK_DIRT, 0);
	assert(place(5, 60, 5, 0, ITEM_WOOD_HOE, 0, 1, 0.0F));
	/* ItemSeeds: farmland top face, air above. */
	fw_clear();
	fw_set(5, 60, 5, BLOCK_FARMLAND, 0);
	assert(place(5, 60, 5, 1, ITEM_SEED, 0, 1, 0.0F));
	assert(!place(5, 60, 5, 2, ITEM_SEED, 0, 1, 0.0F));
	fw_set(5, 60, 5, BLOCK_DIRT, 0);
	assert(!place(5, 60, 5, 1, ITEM_SEED, 0, 1, 0.0F));
	/* ItemDye: only bone meal (15) on sapling / crops / grass. */
	fw_set(5, 60, 5, BLOCK_SAPLING, 0);
	assert(place(5, 60, 5, 1, ITEM_DYE, 15, 1, 0.0F));
	assert(!place(5, 60, 5, 1, ITEM_DYE, 4, 1, 0.0F));
	fw_set(5, 60, 5, BLOCK_STONE, 0);
	assert(!place(5, 60, 5, 1, ITEM_DYE, 15, 1, 0.0F));
	/* ItemMinecart on a rail of any kind. */
	fw_set(5, 60, 5, BLOCK_POWERED_RAIL, 0);
	assert(place(5, 60, 5, 1, ITEM_MINECART_CHEST, 0, 1, 0.0F));
	fw_set(5, 60, 5, BLOCK_STONE, 0);
	assert(!place(5, 60, 5, 1, ITEM_MINECART, 0, 1, 0.0F));
	/* ItemPainting: side faces only. */
	assert(place(5, 60, 5, 2, ITEM_PAINTING, 0, 1, 0.0F));
	assert(!place(5, 60, 5, 1, ITEM_PAINTING, 0, 1, 0.0F));
	/* ItemRedstone: stepped cell must be air; then true even if unsupported. */
	assert(place(5, 60, 5, 1, ITEM_REDSTONE, 0, 1, 0.0F));
	fw_set(5, 61, 5, BLOCK_STONE, 0);
	assert(!place(5, 60, 5, 1, ITEM_REDSTONE, 0, 1, 0.0F));
	/* ItemSign: not the bottom face, a solid clicked block, a free cell. */
	fw_clear();
	fw_set(5, 60, 5, BLOCK_STONE, 0);
	assert(place(5, 60, 5, 1, ITEM_SIGN, 0, 1, 0.0F));
	assert(place(5, 60, 5, 2, ITEM_SIGN, 0, 1, 0.0F));
	assert(!place(5, 60, 5, 0, ITEM_SIGN, 0, 1, 0.0F));
	fw_set(5, 60, 4, BLOCK_STONE, 0);
	assert(!place(5, 60, 5, 2, ITEM_SIGN, 0, 1, 0.0F));
	fw_set(5, 60, 5, 50 /* torch: not solid */, 0);
	assert(!place(5, 60, 5, 1, ITEM_SIGN, 0, 1, 0.0F));
	/* ItemDoor: top face; y+1 over a normal cube, two ground-cover cells. */
	fw_clear();
	fw_set(5, 60, 5, BLOCK_STONE, 0);
	assert(place(5, 60, 5, 1, ITEM_DOOR_WOOD, 0, 1, 0.0F));
	assert(!place(5, 60, 5, 2, ITEM_DOOR_IRON, 0, 1, 0.0F));
	fw_set(5, 62, 5, BLOCK_STONE, 0);
	assert(!place(5, 60, 5, 1, ITEM_DOOR_WOOD, 0, 1, 0.0F));
	fw_set(5, 125, 5, BLOCK_STONE, 0); /* door bottom would be y 126 < 127 */
	assert(place(5, 125, 5, 1, ITEM_DOOR_WOOD, 0, 1, 0.0F));
	fw_set(5, 126, 5, BLOCK_STONE, 0); /* door bottom y 127: refused */
	assert(!place(5, 126, 5, 1, ITEM_DOOR_WOOD, 0, 1, 0.0F));
	/* ItemBed: top face; foot and yaw-derived head both air over cubes.
	 * yaw 0 -> dir 0 -> head at +z; yaw 90 -> dir 1 -> head at -x. */
	fw_clear();
	fw_set(5, 60, 5, BLOCK_STONE, 0);
	assert(!place(5, 60, 5, 1, ITEM_BED, 0, 1, 0.0F)); /* no floor at z 6 */
	fw_set(5, 60, 6, BLOCK_STONE, 0);
	assert(place(5, 60, 5, 1, ITEM_BED, 0, 1, 0.0F));
	assert(!place(5, 60, 5, 1, ITEM_BED, 0, 1, 90.0F)); /* needs (4,60,5) */
	fw_set(4, 60, 5, BLOCK_STONE, 0);
	assert(place(5, 60, 5, 1, ITEM_BED, 0, 1, 90.0F));
	/* Negative yaw floors toward -inf: -100 -> floor(-0.611) = -1 -> dir 3
	 * (head at +x), needing (6,60,5). */
	assert(!place(5, 60, 5, 1, ITEM_BED, 0, 1, -100.0F));
	fw_set(6, 60, 5, BLOCK_STONE, 0);
	assert(place(5, 60, 5, 1, ITEM_BED, 0, 1, -100.0F));
	assert(!place(5, 60, 5, 3, ITEM_BED, 0, 1, 0.0F));

	/* The use-item swing belongs to the rod alone. */
	assert(mp_swing_after_use_item(true, ITEM_FISHING_ROD));
	assert(!mp_swing_after_use_item(true, ITEM_BOW));
	assert(!mp_swing_after_use_item(true, ITEM_EGG));
	assert(!mp_swing_after_use_item(true, ITEM_SNOW_BALL));
	assert(!mp_swing_after_use_item(true, ITEM_BOAT));
	assert(!mp_swing_after_use_item(false, ITEM_FISHING_ROD));
	puts("PASS place predicate: blockActivated set + every Item.onItemUse "
		 "override, rod-only use swing");
}
#endif

/* ---- Part 2: the wire, in order ------------------------------------------ */

static struct server_remote remote;
static uint8_t wire[64][32];
static size_t wire_len[64];
static size_t wire_count;
static unsigned arm_swing_rpcs;
static unsigned use_item_rpcs;

static void to_remote(struct server_rpc* call, void* user) {
	(void)user;
#ifndef MP_ARM_SWING_BASE_RED
	if(call->type == SRPC_ARM_SWING)
		arm_swing_rpcs++;
#endif
	if(call->type == SRPC_USE_ITEM)
		use_item_rpcs++;
	if(gstate.remote_session)
		remote_process_srpc(call, &remote);
	struct server_remote_impl* impl = remote.impl;
	while(impl->frame_count) {
		struct mp_frame* f = &impl->frames[impl->frame_head].frame;
		assert(wire_count < 64 && f->length <= sizeof wire[0]);
		memcpy(wire[wire_count], f->data, f->length);
		wire_len[wire_count++] = f->length;
		impl->frame_head = (impl->frame_head + 1) % REMOTE_QUEUE_CAPACITY;
		impl->frame_count--;
	}
}

static void pump(void) {
	svin_rpc_flush_pending();
	svin_process_messages(to_remote, NULL, false);
}

static void wire_clear(void) {
	pump();
	wire_count = 0;
	arm_swing_rpcs = 0;
}

static void frame_run(const char* script) {
	if(script)
		agent_input_feed_line(script);
	agent_input_poll();
	screen_ingame.update(&screen_ingame, 0.05F);
	pump();
}

static const uint8_t swing_bytes[] = {0x12, 0x00, 0x00, 0x04, 0xD2, 0x01};

static bool is_swing(size_t i) {
	return i < wire_count && wire_len[i] == sizeof swing_bytes
		&& memcmp(wire[i], swing_bytes, sizeof swing_bytes) == 0;
}

/* Packet15(-1,-1,-1,255): x int -1, y byte 0xFF, z int -1, face 0xFF. */
static bool is_use_sentinel(size_t i) {
	static const uint8_t head[] = {0x0F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF};
	return i < wire_count && wire_len[i] >= sizeof head
		&& memcmp(wire[i], head, sizeof head) == 0;
}
/* Packet15 at a real block (not the sentinel). */
static bool is_block_place(size_t i) {
	return i < wire_count && wire[i][0] == 0x0F && !is_use_sentinel(i);
}

static void dump_wire(const char* what) {
	printf("  %s: %zu frame(s):", what, wire_count);
	for(size_t i = 0; i < wire_count; i++)
		printf(" %02X(%zu)", wire[i][0], wire_len[i]);
	printf("\n");
}

static struct client_session* host(void) {
	return &gstate.players[0];
}

static void aim_block(int x, int y, int z, enum side side) {
	host()->camera_hit.hit = true;
	host()->camera_hit.entity_hit = false;
	host()->camera_hit.x = x;
	host()->camera_hit.y = y;
	host()->camera_hit.z = z;
	host()->camera_hit.side = side;
}

static void aim_air(void) {
	host()->camera_hit.hit = false;
	host()->camera_hit.entity_hit = false;
}

static void aim_entity(void) {
	host()->camera_hit.hit = false;
	host()->camera_hit.entity_hit = true;
	host()->camera_hit.entity_id = TARGET_EID;
}

static void hold_item(uint16_t id, uint8_t count) {
	struct inventory* inv = windowc_get_latest(host()->windows[WINDOWC_INVENTORY]);
	inventory_set_slot(inv, inventory_get_hotbar(inv) + INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = id, .count = count});
}

/* Let the dig cooldown and the swing clock settle between scenarios. */
static void settle(void) {
	frame_run("MINE=0");
	frame_run("USE=0");
	thread_msleep(600);
	wire_clear();
}

static void test_dig_wire_order(void) {
	settle();
	aim_block(HIT_X, HIT_Y, HIT_Z, SIDE_TOP);
	hold_item(0, 0);
	frame_run("MINE=1");
	dump_wire("dig press");
	/* clickMouse(0): swing, THEN clickBlock's Packet14 status 0. */
	assert(wire_count == 2);
	assert(is_swing(0));
	assert(wire[1][0] == 0x0E && wire[1][1] == 0x00);
	/* Held on the same block: 5 ticks later one more swing, no second
	 * Packet14 (clickBlock is a no-op while already hitting it). */
	wire_clear();
	frame_run(NULL);
	assert(wire_count == 0);
	thread_msleep(260);
	frame_run(NULL);
	dump_wire("dig held 260 ms");
	assert(wire_count == 1 && is_swing(0));
	frame_run("MINE=0");
	puts("PASS dig: Packet18 precedes Packet14(0); held re-swings every 5 "
		 "ticks without a second dig start");
}

static void test_attack_wire_order(void) {
	settle();
	aim_entity();
	frame_run("MINE=1");
	dump_wire("attack press");
	/* clickMouse(0): swing, THEN attackEntity's Packet7 (left = 1). */
	assert(wire_count == 2);
	assert(is_swing(0));
	assert(wire[1][0] == 0x07 && wire_len[1] == 10 && wire[1][9] == 1);
	frame_run("MINE=0");
	puts("PASS attack: Packet18 precedes Packet7");
}

static void test_air_click_wire(void) {
	settle();
	aim_air();
	frame_run("MINE=1");
	dump_wire("air press");
	assert(wire_count == 1 && is_swing(0));
	wire_clear();
	thread_msleep(260);
	frame_run(NULL); /* tick 5: gated by leftClickCounter */
	assert(wire_count == 0);
	thread_msleep(260);
	frame_run(NULL); /* tick 10: swings again */
	dump_wire("air held 520 ms");
	assert(wire_count == 1 && is_swing(0));
	frame_run("MINE=0");
	puts("PASS air click: a lone Packet18, repeated every 10 ticks held");
}

static void test_place_wire_order(void) {
	/* Stone block with air above, cobblestone in hand: Packet15 then 18. */
	settle();
	aim_block(HIT_X, HIT_Y, HIT_Z, SIDE_TOP);
	hold_item(BLOCK_COBBLESTONE, 64);
	frame_run("PLACE=1");
	dump_wire("place cobble on stone");
	assert(wire_count == 2);
	assert(wire[0][0] == 0x0F);
	assert(is_swing(1));
	/* Empty hand at stone: sendPlaceBlock false -> Packet15 alone. */
	settle();
	hold_item(0, 0);
	frame_run("PLACE=1");
	dump_wire("empty hand on stone");
	assert(wire_count == 1 && wire[0][0] == 0x0F);
	/* Empty hand at a chest: blockActivated true -> Packet15 then 18. */
	settle();
	aim_block(CHEST_X, HIT_Y, HIT_Z, SIDE_TOP);
	frame_run("PLACE=1");
	dump_wire("empty hand on chest");
	assert(wire_count == 2 && wire[0][0] == 0x0F && is_swing(1));
	/* A stick at stone: Item.onItemUse false -> no swing. */
	settle();
	aim_block(HIT_X, HIT_Y, HIT_Z, SIDE_TOP);
	hold_item(ITEM_STICK, 1);
	frame_run("PLACE=1");
	/* ...but the trailing sendUseItem still queues Packet15(-1) for it. */
	assert(wire_count == 2 && wire[0][0] == 0x0F && is_use_sentinel(1));
	/* A blocked destination does not prevent ItemReed.onItemUse from
	 * returning true: Beta sends Packet15 then Packet18 for all three
	 * ItemReed instances [Item.java:90,106,108; ItemReed.java:40-53]. */
	world_set_block(&gstate.world, HIT_X, HIT_Y + 1, HIT_Z,
		(struct block_data) {.type = BLOCK_DIRT}, false);
	static const uint16_t reed_items[] = {
		ITEM_REED, ITEM_CAKE, ITEM_REDSTONE_REPEATER};
	for(size_t i = 0; i < sizeof reed_items / sizeof reed_items[0]; i++) {
		settle();
		aim_block(HIT_X, HIT_Y, HIT_Z, SIDE_TOP);
		hold_item(reed_items[i], 1);
		frame_run("PLACE=1");
		assert(wire_count == 2 && wire[0][0] == 0x0F && is_swing(1));
	}
	settle();
	aim_block(HIT_X, HIT_Y, HIT_Z, SIDE_TOP);
	hold_item(ITEM_REED, 0);
	frame_run("PLACE=1");
	assert(wire_count == 1 && wire[0][0] == 0x0F);
	world_set_block(&gstate.world, HIT_X, HIT_Y + 1, HIT_Z,
		(struct block_data) {.type = BLOCK_AIR}, false);
	frame_run("PLACE=0");
	puts("PASS place: Packet15 then Packet18 iff sendPlaceBlock is true");
}

static void test_use_item_wire_order(void) {
	settle();
	aim_air();
	hold_item(ITEM_FISHING_ROD, 1);
	frame_run("USE=1");
	dump_wire("rod into air");
	/* sendUseItem's Packet15(-1,-1,-1,255) then the rod's swing. */
	assert(wire_count == 2);
	assert(wire[0][0] == 0x0F && wire[0][5] == 0xFF && wire[0][10] == 0xFF);
	assert(is_swing(1));
	settle();
	hold_item(ITEM_BOW, 1);
	frame_run("USE=1");
	dump_wire("bow into air");
	assert(wire_count == 1 && wire[0][0] == 0x0F);
	frame_run("USE=0");
	puts("PASS use item: the rod swings after Packet15(-1), the bow does not");
}

/* Minecraft.clickMouse(1) at a block [Minecraft.java:817-838]: Packet15 at
 * the block; if sendPlaceBlock was false, PlayerControllerMP.sendUseItem
 * then queues Packet15(-1,-1,-1,255) [PlayerControllerMP.java:148-153] --
 * the only packet from which a vanilla server eats / fires / fills. */
static void test_use_at_block_follow_up(void) {
	struct {
		const char* what;
		uint16_t item;
		uint8_t count;
		int x;
		bool follow_up, swing;
	} cases[] = {
		{"bread at stone", ITEM_BREAD, 1, HIT_X, true, false},
		{"bow at stone", ITEM_BOW, 1, HIT_X, true, false},
		{"bucket at stone", ITEM_BUCKET, 1, HIT_X, true, false},
		{"snowball at stone", ITEM_SNOW_BALL, 1, HIT_X, true, false},
		{"egg at stone", ITEM_EGG, 1, HIT_X, true, false},
		{"rod at stone", ITEM_FISHING_ROD, 1, HIT_X, true, true},
		{"cobblestone placed on stone", BLOCK_COBBLESTONE, 64, HIT_X, false,
			true},
		{"bread at a chest (activated)", ITEM_BREAD, 1, CHEST_X, false, true},
	};
	for(size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		settle();
		aim_block(cases[i].x, HIT_Y, HIT_Z, SIDE_TOP);
		hold_item(cases[i].item, cases[i].count);
		frame_run("PLACE=1");
		dump_wire(cases[i].what);
		size_t want = 1 + (cases[i].follow_up ? 1 : 0)
			+ (cases[i].swing ? 1 : 0);
		assert(wire_count == want);
		assert(is_block_place(0));
		size_t n = 1;
		if(cases[i].follow_up)
			assert(is_use_sentinel(n++));
		if(cases[i].swing)
			assert(is_swing(n++));
		frame_run("PLACE=0");
	}
	/* At an ENTITY var2 stays true too [Minecraft.java:799-806,834-838]:
	 * Packet7 use (flag 0), then Packet15(-1), then the rod's Packet18. */
	struct {
		const char* what;
		uint16_t item;
		bool follow_up, swing;
	} ent[] = {
		{"rod at entity", ITEM_FISHING_ROD, true, true},
		{"bread at entity", ITEM_BREAD, true, false},
		{"saddle at entity", ITEM_SADDLE, true, false},
		{"empty bucket at entity (carve-out)", ITEM_BUCKET, false, false},
	};
	for(size_t i = 0; i < sizeof ent / sizeof ent[0]; i++) {
		settle();
		aim_entity();
		hold_item(ent[i].item, 1);
		frame_run("PLACE=1");
		dump_wire(ent[i].what);
		size_t want = 1 + (ent[i].follow_up ? 1 : 0) + (ent[i].swing ? 1 : 0);
		assert(wire_count == want);
		assert(wire[0][0] == 0x07 && wire_len[0] == 10 && wire[0][9] == 0);
		size_t n = 1;
		if(ent[i].follow_up)
			assert(is_use_sentinel(n++));
		if(ent[i].swing)
			assert(is_swing(n++));
		frame_run("PLACE=0");
	}
	/* Local play: server_local's SRPC_BLOCK_PLACE already falls through to
	 * the held item's use, so no second SRPC_USE_ITEM may be queued. */
	gstate.remote_session = false;
	settle();
	unsigned use_rpcs_before = use_item_rpcs;
	aim_block(HIT_X, HIT_Y, HIT_Z, SIDE_TOP);
	hold_item(ITEM_BREAD, 1);
	frame_run("PLACE=1");
	frame_run("PLACE=0");
	assert(use_item_rpcs == use_rpcs_before);
	gstate.remote_session = true;
	puts("PASS use at a block: Packet15(block) then Packet15(-1) iff "
		 "sendPlaceBlock false (online only), before the rod's Packet18");
}

static void test_single_player_sends_nothing(void) {
	gstate.remote_session = false;
	settle();
	aim_block(HIT_X, HIT_Y, HIT_Z, SIDE_TOP);
	hold_item(BLOCK_COBBLESTONE, 64);
	frame_run("MINE=1");
	thread_msleep(260);
	frame_run(NULL);
	frame_run("MINE=0");
	frame_run("PLACE=1");
	frame_run("PLACE=0");
	aim_entity();
	frame_run("MINE=1");
	frame_run("MINE=0");
	assert(arm_swing_rpcs == 0);
	gstate.remote_session = true;
	puts("PASS single player: EntityPlayerSP emits no swing RPC at all");
}

#ifndef MP_ARM_SWING_BASE_RED
/* ---- Part 3: the LOCAL arm swing per right-click action ------------------
 * Beta's Minecraft.clickMouse(1) calls thePlayer.swingItem() -- the same
 * EntityPlayer.swingItem the left click uses [EntityPlayer.java:483-486] --
 * in single player AND online, iff sendPlaceBlock returned true
 * [Minecraft.java:817-820]; the ENTITY branch never swings
 * [Minecraft.java:804-806]; of the trailing sendUseItem's onItemRightClick
 * only the fishing rod swings [ItemFishingRod.java:22,29]. A true sendUseItem
 * re-equips without a swing [Minecraft.java:836-838]. The swing is the
 * session's held_item_animation, which both the first-person hand and the
 * split peer's view of this body (entity_remote_player.c) read through
 * held_anim_swing_lerp. */
#define LEVER_X 12

static bool local_swung(void) {
	const struct held_anim* a = &host()->held_item_animation;
	return !a->punch.equip_only
		&& time_diff_ms(a->punch.start, time_get()) < 300;
}
static bool local_dipped(void) {
	const struct held_anim* a = &host()->held_item_animation;
	return a->punch.place && time_diff_ms(a->punch.start, time_get()) < 300;
}

struct rclick_case {
	const char* what;
	int target; /* 0 air, 1 entity, 2 stone, 3 chest, 4 lever */
	uint16_t item;
	uint8_t count;
	bool swing; /* Beta swingItem */
	bool dip;	/* Beta equippedProgress = 0 */
};

static void aim_case(int target) {
	if(target == 0)
		aim_air();
	else if(target == 1)
		aim_entity();
	else
		aim_block(target == 2 ? HIT_X : target == 3 ? CHEST_X : LEVER_X,
				  HIT_Y, HIT_Z, SIDE_TOP);
}

static void test_local_right_click_swing(bool online) {
	static const struct rclick_case cases[] = {
		{"empty hand flips a lever", 4, 0, 0, true, false},
		{"empty hand opens a chest", 3, 0, 0, true, false},
		{"held block on a lever: activation, no place", 4, BLOCK_COBBLESTONE,
			64, true, false},
		{"cobblestone placed on stone", 2, BLOCK_COBBLESTONE, 64, true, true},
		{"flint and steel on stone", 2, ITEM_FLINT_STEEL, 1, true, false},
		{"empty hand at stone", 2, 0, 0, false, false},
		{"stick at stone", 2, ITEM_STICK, 1, false, false},
		{"bread at stone (trailing sendUseItem eats)", 2, ITEM_BREAD, 1, false,
			true},
		{"fishing rod at stone (trailing use swings)", 2, ITEM_FISHING_ROD, 1,
			true, false},
		{"saddle on an entity", 1, ITEM_SADDLE, 1, false, false},
		{"shears on an entity", 1, ITEM_SHEARS, 1, false, false},
		{"empty bucket on an entity (carve-out: no trailing use)", 1,
			ITEM_BUCKET, 1, false, false},
		{"fishing rod at an entity (trailing use casts)", 1, ITEM_FISHING_ROD,
			1, true, false},
		{"bread at an entity (trailing use eats)", 1, ITEM_BREAD, 1, false,
			true},
		{"bread into air: eats, re-equip only", 0, ITEM_BREAD, 1, false, true},
		{"bow into air", 0, ITEM_BOW, 1, false, false},
		{"empty bucket into air", 0, ITEM_BUCKET, 1, false, false},
		{"fishing rod into air", 0, ITEM_FISHING_ROD, 1, true, false},
	};
	gstate.remote_session = online;
	for(size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		const struct rclick_case* c = &cases[i];
		settle();
		aim_case(c->target);
		hold_item(c->item, c->count);
		frame_run("PLACE=1");
		bool sw = local_swung(), dp = local_dipped();
		unsigned rpcs = arm_swing_rpcs;
		bool wire_swing = false;
		for(size_t f = 0; f < wire_count; f++)
			wire_swing |= is_swing(f);
		printf("  %s %-46s swing=%d dip=%d packet18=%d\n",
			   online ? "online" : "local ", c->what, sw, dp, wire_swing);
		assert(sw == c->swing);
		assert(dp == c->dip);
		if(online) {
			/* Packet18 exactly when Beta's EntityClientPlayerMP.swingItem
			 * runs, and always AFTER the click's Packet15/Packet7. */
			assert(rpcs == (c->swing ? 1u : 0u));
			assert(wire_swing == c->swing);
			if(c->swing)
				assert(is_swing(wire_count - 1) && wire_count >= 2);
		} else {
			assert(rpcs == 0); /* EntityPlayerSP.swingItem sends nothing */
		}
		/* The split peer reads the same timer: a swing is visible on this
		 * body, an equip-only dip is not. */
		const struct held_anim* ha = &host()->held_item_animation;
		if(c->swing) /* mid-swing, 100 ms after this click's start */
			assert(held_anim_swing_lerp(ha, time_add_ms(ha->punch.start, 100))
				   > 0.0F);
		else /* settle() slept 600 ms > the 400 ms swing: nothing on show */
			assert(held_anim_swing_lerp(ha, time_get()) == 0.0F);
		frame_run("PLACE=0");
	}
	gstate.remote_session = true;
	printf("PASS local right-click swing (%s): swing/dip per action matches "
		   "Beta clickMouse(1)%s\n",
		   online ? "online" : "single player",
		   online ? ", Packet18 iff swingItem" : ", no Packet18");
}

static void test_left_attack_swings_empty_hand(void) {
	settle();
	aim_entity();
	hold_item(0, 0);
	frame_run("MINE=1");
	assert(local_swung());
	frame_run("MINE=0");
	puts("PASS left attack with an empty hand swings (clickMouse(0) "
		 "swings first)");
}
#endif

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
#ifndef MP_ARM_SWING_BASE_RED
	test_left_click_cadence();
	test_place_predicate();
#endif
	static char env[] = "CAVEWII_AGENT=1";
	putenv(env);
	assert(agent_input_armed());
	assert(clin_init());
	assert(svin_init());
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	struct client_rpc reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = LOCAL_EID},
	};
	clin_process(&reset);
	assert(host()->local_player && host()->windows[WINDOWC_INVENTORY]);
	host()->local_player->health = MAX_PLAYER_HEALTH;
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	host()->current_screen = &screen_ingame;
	world_set_block(&gstate.world, HIT_X, HIT_Y, HIT_Z,
		(struct block_data) {.type = BLOCK_STONE}, false);
	world_set_block(&gstate.world, CHEST_X, HIT_Y, HIT_Z,
		(struct block_data) {.type = BLOCK_CHEST}, false);
#ifndef MP_ARM_SWING_BASE_RED
	world_set_block(&gstate.world, LEVER_X, HIT_Y, HIT_Z,
		(struct block_data) {.type = BLOCK_LEVER, .metadata = 5}, false);
#endif
	assert(world_get_block(&gstate.world, HIT_X, HIT_Y + 1, HIT_Z).type == 0);

	static struct entity target;
	memset(&target, 0, sizeof target);
	target.id = TARGET_EID;
	target.onLeftClick = onLeftClick;
	struct entity* tp = &target;
	dict_entity_set_at(gstate.entities, TARGET_EID, tp);

	assert(server_remote_init(&remote));
	struct server_remote_impl* impl = remote.impl;
	impl->state = REMOTE_PLAY;
	impl->entity_id = LOCAL_EID;
	gstate.remote_session = true;

	test_dig_wire_order();
	test_attack_wire_order();
	test_air_click_wire();
	test_place_wire_order();
	test_use_item_wire_order();
	test_use_at_block_follow_up();
	test_single_player_sends_nothing();
#ifndef MP_ARM_SWING_BASE_RED
	test_local_right_click_swing(true);
	test_local_right_click_swing(false);
	test_left_attack_swings_empty_hand();
#endif
	puts("PASS test_mp_arm_swing");
	return 0;
}
