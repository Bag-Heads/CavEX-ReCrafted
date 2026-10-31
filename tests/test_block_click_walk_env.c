/*
	R1 goldens (docs/intent/r1-block-click-walk-callbacks.md): Beta 1.7.3's
	Block.onBlockClicked (LEFT-click / dig-START) and Block.onEntityWalking
	(footstep) reactions, over a live superflat server_local, driven through the
	REAL production entry points: SRPC_BLOCK_DIG (finished = false is this port's
	Packet14 status 0) and SRPC_PLAYER_POS (the player footstep odometer), plus
	the shared mob-footstep dispatcher server_local_entity_walked_on_block.

	Every expected value is read off the decompile, not snapshotted:

	  Dig start [740c583 server ItemInWorldManager.java:41-47, func_324_a]:
	    func_28096_a (fire punch-out); then, for a non-air block id,
	    that block's onBlockClicked(world, x, y, z, player)
	  Footstep [740c583 Entity.java:437-458]:
	    gate canTriggerWalking && !(onGround && isSneaking) && ridingEntity == null;
	    id = block at (x, floor(posY - 0.2 - yOffset), z); fence BELOW substitutes it;
	    on distanceWalkedModified > nextStepDistance && id > 0:
	      ++nextStepDistance; step sound; that block's onEntityWalking(x, y, z)

	  Overrides (every 1.7.3 class that has one):
	    BlockRedstoneOre.onBlockClicked/onEntityWalking:21-29 -> func_320_h: 73 -> 74
	    BlockNote.onBlockClicked:38-43 -> triggerNote, NO changePitch
	    BlockLever/BlockButton/BlockDoor/BlockTrapDoor.onBlockClicked -> blockActivated
	      (lever :129, button :120, door :85 [iron: returns at once], trapdoor :70)
	    BlockCake.onBlockClicked:62 -> eatCakeSlice (health < 20 -> heal 3, bite)
	    BlockTNT.onBlockClicked:54-60 -> flint & steel held: setBlockMetadata(1);
	      onBlockDestroyedByPlayer:44-53 -> (meta & 1) primes + "random.fuse", else
	      drops one TNT (quantityDropped 0 -> harvest adds nothing)
	    BlockStairs.onBlockClicked/onEntityWalking:73,150 -> modelBlock (planks /
	      cobblestone: base no-op)
	    BlockFarmland.onEntityWalking:46 -> nextInt(4) == 0 -> dirt, for ANY walker
	      (W2-R1c closed the old player-only deviation).
*/

#include <assert.h>
#include <math.h>
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
#include "block/noteblock_logic.h"
#include "block/piston_moving.h"
#include "entity/combat.h" // PLAYER_EYE_HEIGHT
#include "entity/entity.h"
#include "entity/entity_fish.h"
#include "entity/monsters_object.h"
#include "game/dig_start.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h" // inventory_logic_player
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_noteblock.h"
#include "network/server_world.h"
#include "platform/audio.h"
#include "platform/thread.h"

#define ORE_UNLIT 73 // Block.oreRedstone
#define ORE_LIT 74	 // Block.oreRedstoneGlowing

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

static struct server_local s;
static int gy;		// superflat surface y (the foot cell of a standing player)
static double eye;	// player eye y standing on gy

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

static bool feq(float a, float b) { return fabsf(a - b) < 1.0e-5f; }

// --- sound capture ----------------------------------------------------------
static int g_snd_count;
static int g_snd_by_id[AUDIO_SOUND_COUNT];
static float g_snd_pitch;
static void snd_reset(void) {
	g_snd_count = 0;
	memset(g_snd_by_id, 0, sizeof(g_snd_by_id));
	g_snd_pitch = -1.0F;
}
static void capture(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type != CRPC_SOUND)
		return;
	g_snd_count++;
	if(call->payload.sound.sound_id < AUDIO_SOUND_COUNT)
		g_snd_by_id[call->payload.sound.sound_id]++;
	g_snd_pitch = call->payload.sound.pitch;
}

// --- world helpers ----------------------------------------------------------
static void set(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)}));
}
static int type_at(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? b.type : -1;
}
static int meta_at(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? (b.metadata & 0x0F) : -1;
}

static void hold(uint16_t id) {
	inventory_set_hotbar(&s.player.inventory, 0);
	if(id == 0)
		inventory_clear_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR);
	else
		inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
						   (struct item_data) {.id = id, .count = 1, .durability = 0});
}

static void dig(int x, int y, int z, bool finished) {
	struct server_rpc r = {
		.type = SRPC_BLOCK_DIG,
		.payload.block_dig
		= {.x = x, .y = y, .z = z, .side = SIDE_TOP, .finished = finished}};
	server_local_test_process_rpc(&s, &r);
}
static void click(int x, int y, int z) { dig(x, y, z, false); }

static void send_pos(double x, double z) {
	struct server_rpc pos = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = x,
							   .y = eye,
							   .z = z,
							   .on_ground = true,
							   .sneaking = false,
							   .correction_seq = s.player.correction_seq},
	};
	server_local_test_process_rpc(&s, &pos);
}

static size_t count_entities(enum entity_type type) {
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == type)
			n++;
	}
	return n;
}

// ============================================================================
// 1. Redstone ore: LEFT-click glows it (dig-start); a lit ore stays lit; the
//    click runs for any held item. [BlockRedstoneOre.java:21-24, func_320_h]
// ============================================================================
static void check_redstone_ore_click(void) {
	int x = 2, y = gy + 3, z = 2;
	set(x, y, z, ORE_UNLIT, 0);
	hold(0);
	click(x, y, z);
	assert(type_at(x, y, z) == ORE_LIT && "punching redstone ore must light it");
	click(x, y, z);
	assert(type_at(x, y, z) == ORE_LIT && "punching a lit ore is func_320_h's no-op");
	set(x + 1, y, z, ORE_UNLIT, 0);
	hold(ITEM_FLINT_STEEL);
	click(x + 1, y, z);
	assert(type_at(x + 1, y, z) == ORE_LIT && "the held item is irrelevant");
	// A dig-start on AIR is Beta's var5 > 0 gate: nothing happens (no crash).
	click(x, y + 4, z);
	assert(type_at(x, y + 4, z) == BLOCK_AIR);
	printf("  [1] redstone ore: left-click 73 -> 74, lit stays lit, any held item; "
		   "air click inert\n");
}

// ============================================================================
// 2. Redstone ore: a PLAYER footstep through the real SRPC_PLAYER_POS path
//    lights exactly the step cell (0.5-block samples add 0.3 each; the 4th
//    crosses nextStepDistance 1). [Entity.java:437-458; BlockRedstoneOre:26-29]
// ============================================================================
static void check_redstone_ore_player_walk(void) {
	int z = 6;
	for(int x = 1; x <= 4; x++)
		set(x, gy, z, ORE_UNLIT, 0);
	s.player.x = 1.5;
	s.player.y = eye;
	s.player.z = z + 0.5;
	s.player.on_ground = true;
	s.player.sneaking = false;
	s.player.distance_walked = 0.0F;
	s.player.next_step_distance = 1;
	send_pos(2.0, z + 0.5); // 0.3
	send_pos(2.5, z + 0.5); // 0.6
	send_pos(3.0, z + 0.5); // 0.9
	assert(type_at(1, gy, z) == ORE_UNLIT && type_at(2, gy, z) == ORE_UNLIT
		   && type_at(3, gy, z) == ORE_UNLIT && "no footstep yet: nothing lit");
	send_pos(3.5, z + 0.5); // 1.2 > 1 -> the footstep, foot cell (3, gy, z)
	assert(s.player.next_step_distance == 2);
	assert(type_at(3, gy, z) == ORE_LIT && "the footstep cell must light");
	assert(type_at(2, gy, z) == ORE_UNLIT && type_at(4, gy, z) == ORE_UNLIT
		   && "only the footstep cell lights");
	printf("  [2] redstone ore: player footstep (SRPC_PLAYER_POS) lights exactly "
		   "the step cell\n");
}

// ============================================================================
// 3. The shared footstep dispatcher: a MOB lights redstone ore; a fence BELOW
//    the foot cell substitutes var28 (fence has no override -> nothing lights);
//    a mob does NOT trample farmland (recorded player-only deviation) while a
//    player's footsteps do (1/4 per step). [Entity.java:441-457;
//    BlockFarmland.java:46-51]
// ============================================================================
static void check_walk_dispatch(void) {
	int x = 8, y = gy + 3, z = 2;
	set(x, y, z, ORE_UNLIT, 0);
	server_local_entity_walked_on_block(&s, x, y, z, BLOCK_WALKER_MOB);
	assert(type_at(x, y, z) == ORE_LIT && "a mob footstep must light redstone ore");

	set(x, y + 3, z + 2, BLOCK_FENCE, 0);
	set(x, y + 4, z + 2, ORE_UNLIT, 0);
	server_local_entity_walked_on_block(&s, x, y + 4, z + 2, BLOCK_WALKER_PLAYER);
	assert(type_at(x, y + 4, z + 2) == ORE_UNLIT
		   && "fence below substitutes var28: fence.onEntityWalking is the no-op");

	// BlockFarmland.onEntityWalking [BlockFarmland.java:46-51]:
	//   if(world.rand.nextInt(4) == 0) world.setBlockWithNotify(x, y, z, dirt);
	// NO walker condition: every canTriggerWalking walker (mob or player)
	// tramples; sneaking is filtered earlier, in Entity.moveEntity's gate.
	// Exactly ONE world-RNG draw per footstep: predict each call's roll on a
	// copy of s.rand_src, then require the same outcome AND the same stream.
	int fx = 10, fy = gy + 3, fz = 8;
	enum block_walker walkers[2] = {BLOCK_WALKER_MOB, BLOCK_WALKER_PLAYER};
	int steps_by[2] = {0, 0};
	for(int w = 0; w < 2; w++) {
		set(fx + w, fy, fz, BLOCK_FARMLAND, 7);
		while(type_at(fx + w, fy, fz) == BLOCK_FARMLAND && steps_by[w] < 256) {
			struct random_gen predict = s.rand_src;
			bool tramples = rand_gen_int(&predict, 4) == 0;
			server_local_entity_walked_on_block(&s, fx + w, fy, fz, walkers[w]);
			steps_by[w]++;
			assert(type_at(fx + w, fy, fz) == (tramples ? BLOCK_DIRT : BLOCK_FARMLAND)
				   && "nextInt(4) == 0 is the whole trample condition, any walker");
			assert(memcmp(&predict, &s.rand_src, sizeof predict) == 0
				   && "exactly one world-RNG draw per farmland footstep");
		}
		assert(type_at(fx + w, fy, fz) == BLOCK_DIRT);
	}
	int steps = steps_by[1];

	// BlockStairs forwards onEntityWalking to its model block (planks /
	// cobblestone): the base no-op -- the stairs and meta are untouched.
	set(x + 3, y, z, BLOCK_WOODEN_STAIRS, 2);
	server_local_entity_walked_on_block(&s, x + 3, y, z, BLOCK_WALKER_PLAYER);
	assert(type_at(x + 3, y, z) == BLOCK_WOODEN_STAIRS && meta_at(x + 3, y, z) == 2);
	printf("  [3] walk dispatch: mob lights ore; fence-below no-op; farmland "
		   "trampled by a mob after %d and a player after %d steps (1 draw "
		   "each); stairs inert\n",
		   steps_by[0], steps);
}

// ============================================================================
// 4. Note block: LEFT-click plays the CURRENT pitch once, never changePitch.
//    [BlockNote.java:38-43]
// ============================================================================
static void check_noteblock_click(void) {
	int x = 2, y = gy + 3, z = 10;
	set(x, y - 1, z, BLOCK_STONE, 0); // rock below -> "bd"
	set(x, y, z, BLOCK_NOTEBLOCK, 0);
	assert(noteblock_te_register(&s, x, y, z) >= 0);
	int8_t* note = noteblock_te_lookup(&s, x, y, z);
	assert(note);
	*note = 7;
	snd_reset();
	click(x, y, z);
	assert(*note == 7 && "left-click must NOT change the pitch");
	assert(g_snd_count == 1 && g_snd_by_id[SOUND_NOTE_BD] == 1);
	assert(feq(g_snd_pitch, noteblock_pitch(7)));
	click(x, y, z);
	assert(*note == 7 && g_snd_count == 2 && "each punch plays once more");
	printf("  [4] note block: left-click plays note 7 (bd) twice, pitch unchanged\n");
}

// ============================================================================
// 5. Lever / button / trapdoor / doors: onBlockClicked -> blockActivated.
// ============================================================================
static void check_activation_forwarders(void) {
	int y = gy + 1;
	// Lever (floor, meta 5 on the grass surface): toggle + click pitch 0.6 on /
	// 0.5 off. [BlockLever.java:129-131 -> blockActivated]
	set(14, y, 2, BLOCK_LEVER, 5);
	snd_reset();
	click(14, y, 2);
	assert(meta_at(14, y, 2) == 13 && g_snd_by_id[SOUND_RANDOM_CLICK] == 1
		   && feq(g_snd_pitch, 0.6F));
	click(14, y, 2);
	assert(meta_at(14, y, 2) == 5 && feq(g_snd_pitch, 0.5F));

	// Stone button on the +X face of a stone mount (meta 1 -> mount at x-1):
	// press sets bit 8; a second punch while pressed is the no-op.
	// [BlockButton.java:120-122 -> blockActivated]
	set(15, y, 5, BLOCK_STONE, 0);
	set(16, y, 5, BLOCK_STONE_BUTTON, 1);
	snd_reset();
	click(16, y, 5);
	assert(meta_at(16, y, 5) == 9 && g_snd_count == 1);
	click(16, y, 5);
	assert(meta_at(16, y, 5) == 9 && g_snd_count == 1 && "pressed button: no-op");

	// Trapdoor (meta 0, supporting wall at +Z): meta ^ 4 per punch.
	// [BlockTrapDoor.java:70-72 -> blockActivated]
	set(14, y, 9, BLOCK_STONE, 0);
	set(14, y, 8, BLOCK_TRAP_DOOR, 0);
	click(14, y, 8);
	assert(meta_at(14, y, 8) == 4);
	click(14, y, 8);
	assert(meta_at(14, y, 8) == 0);

	// Wooden door: punching the bottom opens both halves; punching the TOP
	// forwards to the bottom and closes both. [BlockDoor.java:85-107]
	set(20, y, 2, BLOCK_DOOR_WOOD, 0);
	set(20, y + 1, 2, BLOCK_DOOR_WOOD, 8);
	click(20, y, 2);
	assert(meta_at(20, y, 2) == 4 && meta_at(20, y + 1, 2) == 12);
	click(20, y + 1, 2);
	assert(meta_at(20, y, 2) == 0 && meta_at(20, y + 1, 2) == 8);

	// Iron door: blockActivated returns at once for Material.iron -> no change.
	set(22, y, 2, BLOCK_DOOR_IRON, 0);
	set(22, y + 1, 2, BLOCK_DOOR_IRON, 8);
	click(22, y, 2);
	assert(meta_at(22, y, 2) == 0 && meta_at(22, y + 1, 2) == 8);

	// Stone stairs: BlockStairs.onBlockClicked -> cobblestone (base no-op).
	set(24, y, 2, BLOCK_STONE_STAIRS, 1);
	click(24, y, 2);
	assert(type_at(24, y, 2) == BLOCK_STONE_STAIRS && meta_at(24, y, 2) == 1);
	printf("  [5] lever/button/trapdoor/wooden door toggle on left-click; iron "
		   "door and stairs inert\n");
}

// ============================================================================
// 6. Cake: LEFT-click eats a slice exactly like right-click; not at full health.
//    [BlockCake.java:62-77; CaveWii health = 8 x Beta]
// ============================================================================
static void check_cake_click(void) {
	int x = 18, y = gy + 1, z = 12;
	set(x, y, z, BLOCK_CAKE, 0);
	s.player.health = MAX_PLAYER_HEALTH;
	click(x, y, z);
	assert(meta_at(x, y, z) == 0 && s.player.health == MAX_PLAYER_HEALTH
		   && "full health: Beta's health < 20 gate refuses");
	s.player.health = MAX_PLAYER_HEALTH - 40;
	click(x, y, z);
	assert(meta_at(x, y, z) == 1 && s.player.health == MAX_PLAYER_HEALTH - 16
		   && "one slice: heal(3) == 24 at the 8x scale, bite 0 -> 1");
	s.player.health = MAX_PLAYER_HEALTH;
	printf("  [6] cake: left-click eats a slice (+24, bite 1); refused at full health\n");
}

// ============================================================================
// 7. TNT: bare-hand punch leaves metadata 0 and the break drops one TNT; a
//    punch holding flint & steel stamps metadata 1 and the break primes it
//    (random.fuse, no item, flint & steel NOT damaged).
//    [BlockTNT.java:44-60; ItemInWorldManager.removeBlock -> func_325_c]
// ============================================================================
static void check_tnt_click(void) {
	int y = gy + 1;
	size_t items0 = count_entities(ENTITY_ITEM);
	size_t primed0 = count_entities(ENTITY_PRIMED_TNT);

	set(26, y, 12, BLOCK_TNT, 0);
	hold(0);
	click(26, y, 12);
	assert(type_at(26, y, 12) == BLOCK_TNT && meta_at(26, y, 12) == 0);
	dig(26, y, 12, true);
	assert(type_at(26, y, 12) == BLOCK_AIR);
	assert(count_entities(ENTITY_ITEM) == items0 + 1
		   && count_entities(ENTITY_PRIMED_TNT) == primed0
		   && "unstamped TNT: one TNT item, no primed entity");

	set(28, y, 12, BLOCK_TNT, 0);
	hold(ITEM_FLINT_STEEL);
	click(28, y, 12);
	assert(type_at(28, y, 12) == BLOCK_TNT && meta_at(28, y, 12) == 1
		   && "flint & steel punch stamps metadata 1 and does NOT prime yet");
	assert(count_entities(ENTITY_PRIMED_TNT) == primed0);
	snd_reset();
	dig(28, y, 12, true);
	assert(type_at(28, y, 12) == BLOCK_AIR);
	assert(count_entities(ENTITY_PRIMED_TNT) == primed0 + 1
		   && count_entities(ENTITY_ITEM) == items0 + 1
		   && "stamped TNT primes on the break and drops nothing");
	assert(g_snd_by_id[SOUND_RANDOM_FUSE] == 1);
	struct item_data held;
	assert(inventory_get_hotbar_item(&s.player.inventory, &held)
		   && held.id == ITEM_FLINT_STEEL && held.durability == 0
		   && "the punch prime is durability-free");
	hold(0);
	printf("  [7] TNT: hand punch -> meta 0 + one TNT drop; flint punch -> meta 1, "
		   "break primes (fuse sound), no drop, no wear\n");
}

// ============================================================================
// 8. R1d (Fable review): one logical dig-start == one click.
//    Beta PlayerControllerMP.clickBlock:41-60 sends Packet14 status 0 only on
//    `!isHittingBlock || cell differs`; resetBlockRemoving (crosshair off every
//    block) sends nothing. The server drops a repeated start_seq; a NEW press
//    at the same cell (new seq) clicks again, as Beta's re-press does.
// ============================================================================
static void seq_click(int x, int y, int z, uint32_t seq) {
	struct server_rpc r = {
		.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.x = x, .y = y, .z = z, .side = SIDE_TOP,
							  .finished = false, .start_seq = seq}};
	server_local_test_process_rpc(&s, &r);
}

static void check_dig_start_dedup(void) {
	// Client decision: off every block -> reset without a packet.
	assert(dig_start_decide(false, true) == DIG_START_RESET);
	assert(dig_start_decide(false, false) == DIG_START_RESET);
	assert(dig_start_decide(true, true) == DIG_START_KEEP);
	assert(dig_start_decide(true, false) == DIG_START_SEND);
	uint32_t ctr = UINT32_MAX;
	assert(dig_start_next_seq(&ctr) == 1 && "seq skips the unsequenced 0");

	int y = gy + 1;
	set(14, y, 14, BLOCK_LEVER, 5);
	for(int k = 0; k < 4; k++) // even count: a per-copy toggle would end OFF
		seq_click(14, y, 14, 700);
	assert(meta_at(14, y, 14) == 13 && "four copies of ONE dig-start toggle once");
	seq_click(14, y, 14, 701);
	assert(meta_at(14, y, 14) == 5 && "a fresh press at the same cell clicks again");

	// Flint-stamped TNT is inert to a NON-power neighbour update (Beta
	// BlockTNT.onNeighborBlockChange primes only on canProvidePower +
	// indirect power); it still primes on the player break.
	size_t primed0 = count_entities(ENTITY_PRIMED_TNT);
	set(16, y, 14, BLOCK_TNT, 0);
	hold(ITEM_FLINT_STEEL);
	click(16, y, 14);
	assert(meta_at(16, y, 14) == 1);
	set(17, y, 14, BLOCK_STONE, 0);  // neighbour change, not power
	set(16, y + 1, 14, BLOCK_SAND, 0);
	set(16, y + 1, 14, BLOCK_AIR, 0);
	assert(type_at(16, y, 14) == BLOCK_TNT && meta_at(16, y, 14) == 1
		   && count_entities(ENTITY_PRIMED_TNT) == primed0
		   && "a non-power neighbour update must not prime stamped TNT");
	hold(0);
	dig(16, y, 14, true);
	assert(type_at(16, y, 14) == BLOCK_AIR
		   && count_entities(ENTITY_PRIMED_TNT) == primed0 + 1
		   && "the bare-hand break then primes the stamped TNT");
	// Riding a pig: Beta's footstep gate `ridingEntity == null` fails, so a
	// walk over redstone ore while mounted lights nothing. [Entity.java:437]
	{
		int z = 20;
		for(int x = 1; x <= 4; x++)
			set(x, gy, z, ORE_UNLIT, 0);
		struct entity* pig = server_local_spawn_monster(
			(vec3) {30.5F, (float)(gy + 1), 30.5F}, MONSTER_PIG, &s);
		assert(pig);
		pig->data.monster.pig_ridden = true;
		pig->data.monster.pig_rider_id = SERVER_LOCAL_PLAYER_ENTITY_ID;
		pig->data.monster.pig_rider_actor = server_local_actor_ident(&s, &s.player);
		s.player.x = 1.5;
		s.player.y = eye;
		s.player.z = z + 0.5;
		s.player.distance_walked = 0.0F;
		s.player.next_step_distance = 1;
		for(int k = 1; k <= 6; k++)
			send_pos(1.5 + 0.5 * k, z + 0.5);
		for(int x = 1; x <= 4; x++)
			assert(type_at(x, gy, z) == ORE_UNLIT && "a pig rider takes no footsteps");
		assert(s.player.next_step_distance == 1);
		pig->data.monster.pig_ridden = false;
	}
	printf("  [8] dig-start dedup: 4 copies of one start = 1 click, new press "
		   "clicks again; stamped TNT inert to non-power neighbours, primes on "
		   "break; pig rider takes no footsteps\n");
}

// ============================================================================
// 9. B2: farmland reverts to dirt when the block ABOVE has a SOLID Material.
//    BlockFarmland.onNeighborBlockChange:81-88 -- `getBlockMaterial(x,y+1,z)
//    .isSolid()` (Material.isSolid true except MaterialLogic plants/circuits/
//    snow, MaterialTransparent air/fire, MaterialLiquid, MaterialPortal) --
//    NOT a normal-cube test: glass, leaves, TNT, slab, chest, fence, cake and
//    cobweb are all solid materials; torch, crops, snow layer, sapling are not.
//    BlockFarmland has no canBlockStay, and updateTick's only above-block read
//    is isCropsNearby (id == crops), already exact.
// ============================================================================
static void check_farmland_solid_above(void) {
	int y = gy + 3;
	static const int solid[] = {BLOCK_GLASS, BLOCK_LEAVES, BLOCK_TNT, BLOCK_SLAB,
								BLOCK_CHEST, BLOCK_FENCE, BLOCK_CAKE, BLOCK_COBWEB,
								BLOCK_STONE};
	static const int not_solid[] = {BLOCK_TORCH, BLOCK_CROPS, BLOCK_SNOW,
									BLOCK_SAPLING, BLOCK_AIR};
	int x = 2;
	for(size_t i = 0; i < sizeof solid / sizeof *solid; i++, x += 2) {
		set(x, y, 24, BLOCK_FARMLAND, 7);
		set(x, y + 1, 24, solid[i], 0);
		if(type_at(x, y, 24) != BLOCK_DIRT)
			fprintf(stderr, "solid-material id %d above farmland did not revert it\n",
					solid[i]);
		assert(type_at(x, y, 24) == BLOCK_DIRT
			   && "Material.isSolid() above reverts farmland to dirt");
	}
	x = 2;
	for(size_t i = 0; i < sizeof not_solid / sizeof *not_solid; i++, x += 2) {
		set(x, y, 26, BLOCK_FARMLAND, 7);
		if(not_solid[i] != BLOCK_AIR) // air: the farmland placement itself notified nothing solid
			set(x, y + 1, 26, not_solid[i], 0);
		else
			set(x + 1, y, 26, BLOCK_STONE, 0); // a side neighbour update with air above
		assert(type_at(x, y, 26) == BLOCK_FARMLAND
			   && "a non-solid material above leaves farmland alone");
	}
	printf("  [9] farmland: solid-material above (glass/leaves/TNT/slab/chest/"
		   "fence/cake/web/stone) -> dirt; torch/crops/snow/sapling/air -> kept\n");
}

// ============================================================================
// 10. B1: the fishing bobber takes footsteps. EntityFish extends Entity and
//     does NOT override canTriggerWalking (only boat/FX/falling sand/item/
//     minecart/spider/primed TNT/wolf do), its yOffset is 0 (:68), isSneaking
//     is the never-set entity flag 1, ridingEntity is null, and onUpdate calls
//     moveEntity(motion) whenever !inGround (:233-234) -> Entity.moveEntity's
//     odometer, step and onEntityWalking at floor(posY - 0.2) all apply.
// ============================================================================
static void check_fish_bobber_walks(void) {
	int z = 28, y = gy; // ore replaces the surface cell under the glide
	for(int x = 3; x <= 6; x++)
		set(x, y, z, ORE_UNLIT, 0);
	hold(ITEM_FISHING_ROD);
	s.player.x = 4.5;
	s.player.y = eye;
	s.player.z = z - 2.5;
	struct entity* e = calloc(1, sizeof *e);
	assert(e);
	entity_fish(0xF15Fu, e, true, &s.worlds[0]);
	e->data.fish.angler_actor = server_local_actor_ident(&s, &s.player);
	s.player.fish_entity_id = e->id;
	// Glide 0.1 above the surface: no block contact, foot cell = the ore row.
	e->pos[0] = 3.5F;
	e->pos[1] = (float)(y + 1) + 0.1F;
	e->pos[2] = (float)z + 0.5F;
	e->vel[0] = 0.8F;
	e->vel[1] = 0.0F;
	e->vel[2] = 0.0F;
	e->data.fish.walk_dist = 0.9F; // 0.9 + 0.8*0.6 = 1.38 > nextStepDistance 1
	e->data.fish.walk_next_step = 1;
	bool removed = e->tick_server(e, &s);
	assert(!removed && !e->data.fish.in_ground && "the glide touches nothing");
	assert(e->data.fish.walk_next_step == 2 && "one bobber footstep");
	assert(type_at(4, y, z) == ORE_LIT && "the bobber's step cell (x = 4) lights");
	assert(type_at(3, y, z) == ORE_UNLIT && type_at(5, y, z) == ORE_UNLIT);
	free(e);
	s.player.fish_entity_id = 0;
	hold(0);
	printf("  [10] fishing bobber: canTriggerWalking true -> one footstep lights "
		   "the ore under its glide\n");
}

int main(void) {
	test_fixture_dir("test_block_click_walk_env_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	printf("test_block_click_walk_env: R1 onBlockClicked / onEntityWalking "
		   "goldens (740c583)\n");
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = UINT32_C(0xA1B2C3D4);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s, &s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;

	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	gy = -1;
	for(int y = 127; y >= 0; y--) {
		struct block_data probe;
		if(server_world_get_block(&s.worlds[0], 8, y, 8, &probe) && probe.type != BLOCK_AIR) {
			gy = y;
			break;
		}
	}
	assert(gy > 0);
	eye = (double)(gy + 1) + PLAYER_EYE_HEIGHT;
	s.player.x = 30.5; // parked away from every rig (the door nudge reads it)
	s.player.y = eye;
	s.player.z = 30.5;

	clin_test_set_rpc_observer(capture, NULL);

	check_redstone_ore_click();
	check_redstone_ore_player_walk();
	check_walk_dispatch();
	check_noteblock_click();
	check_activation_forwarders();
	check_cake_click();
	check_tnt_click();
	check_dig_start_dedup();
	check_farmland_solid_above();
	check_fish_bobber_walks();

	g_drain_run = 0;
	thread_join(&drain);
	printf("test_block_click_walk_env: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
