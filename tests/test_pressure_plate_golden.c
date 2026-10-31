/*
	Golden test for audit #24 (ratified): source/block/block_pressure_plate.c
	(a) sensed ONLY the local player via an eye-height heuristic -- mobs,
	dropped items and arrows never pressed a plate; (b) neither plate struct
	set .tick_delay, so the engine re-armed the poll every tick instead of
	Beta's tickRate() == 20 while-pressed recheck; (c) the re-arm was
	unconditional, not gated on the pressed state. Authored per the
	beta-tick-golden skill's discipline: expected values below are derived BY
	HAND from the decompile, not from running this engine's own (pre-fix)
	code.

	GROUND TRUTH -- decompile 740c583, BlockPressurePlate.java (verified
	verbatim this session, quoted in full for the record):

	    constructor (id, texture, mob type, material):
	        base block init with id, texture and material;
	        triggerMobType = the given mob type;
	        tick on load set;
	        ... setBlockBounds(0.0625, 0, 0.0625, 0.9375, 0.03125, 0.9375);
	    }
	    tickRate: 20.
	    canPlaceBlockAt: a normal cube directly below (x,y-1,z).
	    onBlockAdded: does nothing.                                // NO-OP
	    onNeighborBlockChange:
	        if (!w.isBlockNormalCube(x,y-1,z)) { dropBlockAsItem(...); setBlockWithNotify(0); }
	    }                                                            // NO scheduling, either branch
	    updateTick:
	        if (!w.multiplayerWorld && w.getBlockMetadata(x,y,z) != 0)
	            setStateIfMobInteractsWithPlate(w,x,y,z);
	    }
	    onEntityCollidedWithBlock:
	        if (!w.multiplayerWorld && w.getBlockMetadata(x,y,z) != 1)
	            setStateIfMobInteractsWithPlate(w,x,y,z);
	    }
	    setStateIfMobInteractsWithPlate:
	        was_pressed = (metadata == 1);
	        float m = 0.125F;
	        AxisAlignedBB box = AABB(x+m, y, z+m, x+1-m, y+0.25, z+1-m);
	        List hits = (triggerMobType == EnumMobType.everything)
	            ? w.getEntitiesWithinAABBExcludingEntity(null, box)      // ANY entity, no filter
	            : w.getEntitiesWithinAABB(EntityLiving.class, box);      // triggerMobType.mobs
	        present = (any hits);
	        if (present && !was_pressed) {
	            w.setBlockMetadataWithNotify(x,y,z,1);
	            w.notifyBlocksOfNeighborChange(x,y,z,blockID);
	            w.notifyBlocksOfNeighborChange(x,y-1,z,blockID);
	            w.playSoundEffect(x+0.5,y+0.1,z+0.5,"random.click",0.3F,0.6F);
	        }
	        if (!present && was_pressed) {
	            w.setBlockMetadataWithNotify(x,y,z,0);
	            w.notifyBlocksOfNeighborChange(x,y,z,blockID);
	            w.notifyBlocksOfNeighborChange(x,y-1,z,blockID);
	            w.playSoundEffect(x+0.5,y+0.1,z+0.5,"random.click",0.3F,0.5F);
	        }
	        if (present) w.scheduleBlockUpdate(x,y,z,blockID,tickRate());   // (c): ONLY if present
	    }
	    isPoweringTo: metadata > 0.                           // full power, any side
	    isIndirectlyPoweringTo(side):
	        if (metadata == 0) return false; else return side == 1;      // "the block below" only
	    }

	Registration (Block.java:662/664): pressurePlateStone = EnumMobType.mobs
	(EntityLiving.class ONLY: players + every mob -- NOT items/arrows/carts);
	pressurePlatePlanks = EnumMobType.everything (getEntitiesWithinAABBExcludingEntity
	with a null exclusion -- literally every entity type, no filter at all).

	EntityArrow.onUpdate (740c583, verified this session) NEVER calls
	Entity.moveEntity -- it is a bespoke ray-trace-driven position update, both
	in-flight (rayTraceBlocks_do_do + manual posX/Y/Z += motionX/Y/Z) and once
	stuck (inGround early-returns before any movement call). Real Beta 1.7.3
	arrows therefore NEVER trigger onEntityCollidedWithBlock for ANY block --
	pressure plates included, wood ones too, despite wood's EnumMobType.everything
	having no type filter. This is a DELIBERATE correction of this golden's
	original brief (which assumed an arrow presses a wood plate, matching
	modern-Minecraft intuition, not Beta 1.7.3) -- see SECTION D below.

	COVERAGE:
	  A. Player press/release timing on a STONE plate through the REAL
	     production path (server_world_tick's player feet-cell probe ->
	     block_pressure_plate_on_entity_tick -> plate_update_state), through
	     the REAL scheduled-tick queue: tick-1 press (immediate arrival, no
	     scheduling delay), the tickRate()==20 while-pressed recheck cadence,
	     release ONLY on the first recheck after the player leaves (up to a
	     19-tick phantom-power window, pinning Beta's exact latency -- not
	     instant), no dangling reschedule afterward, STRONG power into the
	     support cube below (the "below" notify) reaching a wire two cells
	     down, and the sound-pair state machine's notify fan-out.
	  B. A mob (EntityLiving) presses a STONE plate -- audit #24 (a)'s core
	     regression: the old code could NEVER see this.
	  C. A dropped item presses a WOOD plate but does NOT press a STONE one --
	     EnumMobType.everything vs EnumMobType.mobs, driven through the REAL
	     production entity tick (server_local_test_tick_entities ->
	     entity_item.c's post-move scan -> block_pressure_plate_on_entity_tick).
	  C2. Field regression B (2026-09-03, hardware report): the SAME item
	     press, but through REAL gravity settling (spawned above the plate,
	     falls under entity_item.c's own physics across ~12 ticks) instead
	     of C's exact teleport -- closes the gap between "the sensing math
	     is right" and "an item actually falls into the sensed box". Passes
	     on PC; no code defect reproduces here (see docs/deviations.md /
	     the field-regressions-0903 lane commit message for the hardware-
	     unconfirmed status and next-step hypotheses).
	  D. An arrow resting in a WOOD plate's box does NOT press it, even under
	     several REAL production entity ticks -- the corrected case (see the
	     EntityArrow citation above). Contrasted directly against C's item at
	     the identical geometry class, proving the exclusion is real wiring
	     (arrows are never called into), not a geometry accident.
	  E. Support-drop: mining the block under a PRESSED plate pops it (no
	     float) and clears the phantom power it was driving -- audit #24's
	     preserved existing coverage, revalidated against the rewritten
	     onNeighbourBlockChange (which no longer re-arms anything when still
	     supported).
*/

#include <assert.h>
#include <math.h>
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
#include "block/block_pressure_plate.h"
#include "block/redstone_server.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_pressure_plate_tmp' */
#define TMP_WORLD s_fixture_TMP_WORLD

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
		(struct block_data) {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)}));
}

static uint8_t meta_at(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	return b.metadata;
}

static uint8_t type_at(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	return b.type;
}

static void world_tick(void) { server_world_tick(&s.world, &s); }

// Parked far from every plate cell so the player feet-probe (server_world.c)
// never accidentally contributes presence in a scene it isn't part of.
static void park_player(void) {
	s.player.x = -5000.0;
	s.player.y = 64.0;
	s.player.z = -5000.0;
}

static uint32_t g_next_id = 9000;
static struct entity* spawn_entity_slot(void) {
	uint32_t id = g_next_id++;
	struct entity** e_ptr = dict_entity_safe_get(s.entities, id);
	*e_ptr = calloc(1, sizeof(struct entity));
	assert(*e_ptr);
	(*e_ptr)->id = id;
	return *e_ptr;
}

/* =========================================================================
   SECTION A -- player press/release timing + strong power + notify fan-out,
   through the REAL scheduled-tick queue.
   ========================================================================= */

#define A_SUPPORT2_Y 88 /* wire's own support               */
#define A_WIRE_Y     89 /* wire, powered THROUGH the plate's support below */
#define A_SUPPORT_Y  90 /* the plate's support / "below" notify target      */
#define A_PLATE_Y    91
#define A_BX 10
#define A_BZ 10

static void section_a(void) {
	park_player();

	set(A_BX, A_SUPPORT2_Y, A_BZ, BLOCK_STONE, 0);
	set(A_BX, A_WIRE_Y, A_BZ, BLOCK_REDSTONE_WIRE, 0);
	set(A_BX, A_SUPPORT_Y, A_BZ, BLOCK_STONE, 0);
	set(A_BX, A_PLATE_Y, A_BZ, BLOCK_STONE_PRESSURE_PLATE, 0);

	assert(meta_at(A_BX, A_PLATE_Y, A_BZ) == 0 && "fixture must start unpressed");
	assert(meta_at(A_BX, A_WIRE_Y, A_BZ) == 0 && "fixture wire must start unpowered");

	// Stand on the plate (footY = player.y - PLAYER_EYE_HEIGHT must floor to
	// A_PLATE_Y -- the SAME player.y convention as the pre-fix regression
	// guard this replaces, test_block_stay_growth_env.c's "Y + 1.67").
	s.player.x = A_BX + 0.5;
	s.player.z = A_BZ + 0.5;
	s.player.y = A_PLATE_Y + 1.67;

	// --- (1) ARRIVAL: press on the VERY FIRST world tick -- no scheduling
	// delay for arrival, exactly like Beta's onEntityCollidedWithBlock (fired
	// every tick regardless of any schedule) ------------------------------
	assert(s.world.current_tick == 0);
	world_tick();
	assert(s.world.current_tick == 1);
	assert(meta_at(A_BX, A_PLATE_Y, A_BZ) == 1
		   && "the plate must press on tick 1 -- immediate arrival detection, "
			  "not a poll delay");
	assert(redstone_server_indirect_power(&s, A_BX, A_SUPPORT_Y, A_BZ, 4, true)
		   && "the plate's support cube must be strongly powered (the 'below' notify)");
	assert(meta_at(A_BX, A_WIRE_Y, A_BZ) == 15
		   && "the 'below' notify must reach the wire through the support cube "
			  "(BlockPressurePlate.notifyBlocksOfNeighborChange(x,y-1,z,...))");

	// --- (2) the tickRate()==20 recheck cadence WHILE the player stays put:
	// audit #24 fix (b) -- must NOT re-arm every tick anymore ---------------
	for(int t = 2; t <= 20; t++) {
		world_tick();
		assert(meta_at(A_BX, A_PLATE_Y, A_BZ) == 1
			   && "bit must not clear before the 20-tick scheduled recheck");
		assert(meta_at(A_BX, A_WIRE_Y, A_BZ) == 15);
	}
	world_tick(); /* current_tick == 21: the scheduled recheck (armed at tick 1 + 20) */
	assert(s.world.current_tick == 21);
	assert(meta_at(A_BX, A_PLATE_Y, A_BZ) == 1
		   && "the recheck must leave the plate pressed while the player is still on it");
	assert(meta_at(A_BX, A_WIRE_Y, A_BZ) == 15);

	// --- (3) player leaves: audit #24 fix (c) -- the plate must stay pressed
	// until the NEXT scheduled recheck (tick 41), not clear the instant the
	// player steps off (Beta has no departure hook, only the poll) ---------
	park_player();
	for(int t = 22; t <= 40; t++) {
		world_tick();
		assert(meta_at(A_BX, A_PLATE_Y, A_BZ) == 1
			   && "phantom power must persist until the scheduled recheck notices "
				  "the player is gone");
		assert(meta_at(A_BX, A_WIRE_Y, A_BZ) == 15);
	}

	// --- (4) the tick-41 recheck (player absent) releases + de-powers, and
	// does NOT reschedule again (Beta's `if (present) scheduleBlockUpdate`) -
	world_tick();
	assert(s.world.current_tick == 41);
	assert(meta_at(A_BX, A_PLATE_Y, A_BZ) == 0
		   && "the tick-41 recheck must release the plate once nobody remains");
	assert(!redstone_server_indirect_power(&s, A_BX, A_SUPPORT_Y, A_BZ, 4, true));
	assert(meta_at(A_BX, A_WIRE_Y, A_BZ) == 0
		   && "the clear-path 'below' notify must de-power the wire back to 0");

	for(int t = 42; t <= 65; t++) {
		world_tick();
		assert(meta_at(A_BX, A_PLATE_Y, A_BZ) == 0
			   && "no dangling reschedule -- Beta reschedules ONLY while present");
		assert(meta_at(A_BX, A_WIRE_Y, A_BZ) == 0);
	}

	printf("  [A] player press-on-tick-1 + tickRate()==20 recheck cadence + "
		   "phantom-power release + strong power/notify fan-out through the "
		   "real scheduled-tick queue: OK\n");
}

/* =========================================================================
   SECTION B -- a mob (EntityLiving) presses a STONE plate.
   ========================================================================= */

#define B_YF 90
#define B_PLATE_Y 91
#define B_BX 20
#define B_BZ 20

static void section_b(void) {
	park_player();
	set(B_BX, B_YF, B_BZ, BLOCK_STONE, 0);
	set(B_BX, B_PLATE_Y, B_BZ, BLOCK_STONE_PRESSURE_PLATE, 0);
	assert(meta_at(B_BX, B_PLATE_Y, B_BZ) == 0);

	struct entity* mob = spawn_entity_slot();
	entity_monster(mob->id, mob, true, &s.world, MONSTER_PIG);
	// Cell-centre spawn snap, y_size 0 -- the SAME convention
	// server_local.c's mob_spawn_box_at uses, giving a world box Y in
	// [B_PLATE_Y, B_PLATE_Y + box_h] (entity_set_size's y_offset=0.0F puts
	// local Y in [0, height]) that overlaps the plate's [y, y+0.25]
	// sensitivity box at the very bottom.
	mob->teleport(mob, (vec3) {(float)B_BX + 0.5F, (float)B_PLATE_Y, (float)B_BZ + 0.5F});

	// Drive the REAL, exported per-tick post-move collision scan directly
	// (entity_monster.c's entity_monster_post_move_cactus_collisions -- the
	// exact site audit #24 wires the plate hook into) rather than a full AI
	// tick_server pass: pig AI can choose to wander/idle non-deterministically,
	// which would make a byte-exact single-tick pin flaky for reasons having
	// nothing to do with this fix. This is still the genuine production
	// dispatch (redstone_piston_server.c already calls it externally too),
	// just without the surrounding AI noise.
	entity_monster_post_move_cactus_collisions(mob, &s);

	assert(meta_at(B_BX, B_PLATE_Y, B_BZ) == 1
		   && "a mob (EntityLiving) must press a STONE plate -- audit #24 (a): "
			  "the pre-fix player-only heuristic could never see this");

	printf("  [B] a mob presses a STONE plate via the real post-move collision "
		   "scan: OK\n");
}

/* =========================================================================
   SECTION C -- a dropped item presses a WOOD plate but NOT a STONE one,
   through the REAL production entity tick.
   ========================================================================= */

#define C_YF 90
#define C_PLATE_Y 91
#define C_STONE_BX 30
#define C_STONE_BZ 30
#define C_WOOD_BX  31
#define C_WOOD_BZ  30

static void section_c(void) {
	park_player();
	set(C_STONE_BX, C_YF, C_STONE_BZ, BLOCK_STONE, 0);
	set(C_STONE_BX, C_PLATE_Y, C_STONE_BZ, BLOCK_STONE_PRESSURE_PLATE, 0);
	set(C_WOOD_BX, C_YF, C_WOOD_BZ, BLOCK_STONE, 0);
	set(C_WOOD_BX, C_PLATE_Y, C_WOOD_BZ, BLOCK_WOOD_PRESSURE_PLATE, 0);

	struct entity* it = spawn_entity_slot();
	entity_item(it->id, it, true, &s.world,
		(struct item_data) {.id = BLOCK_DIRT, .durability = 0, .count = 1});
	it->data.item.pickup_delay = 100; // never let the (far-away) player collect it mid-test
	glm_vec3_zero(it->vel);

	// entity_set_size(0.25,0.25, y_offset=0.125) -> local Y centered at
	// pos_y with height 0.25, i.e. world Y = [pos_y-0.125, pos_y+0.125].
	// pos_y = plate_y + 0.125 aligns that EXACTLY onto the plate's
	// [plate_y, plate_y+0.25] sensitivity box -- deliberately placed, not
	// settled by gravity, so ONE tick with vel=0 leaves the position (and
	// therefore the box the post-move scan sees) exactly where set.
	it->teleport(it, (vec3) {(float)C_STONE_BX + 0.5F, (float)C_PLATE_Y + 0.125F,
							 (float)C_STONE_BZ + 0.5F});
	assert(server_local_test_tick_entities(&s));
	assert(meta_at(C_STONE_BX, C_PLATE_Y, C_STONE_BZ) == 0
		   && "a dropped item must NOT press a STONE plate -- EnumMobType.mobs "
			  "== EntityLiving.class only");

	it->teleport(it, (vec3) {(float)C_WOOD_BX + 0.5F, (float)C_PLATE_Y + 0.125F,
							 (float)C_WOOD_BZ + 0.5F});
	glm_vec3_zero(it->vel);
	assert(server_local_test_tick_entities(&s));
	assert(meta_at(C_WOOD_BX, C_PLATE_Y, C_WOOD_BZ) == 1
		   && "a dropped item MUST press a WOOD plate -- EnumMobType.everything "
			  "has no entity-type filter at all");

	printf("  [C] a dropped item presses a WOOD plate but not a STONE one, via "
		   "the real production entity tick: OK\n");
}

// SECTION C2 (field regression B, 2026-09-03): does a NATURALLY
// gravity-settled item -- spawned above the plate with zero velocity, left
// to fall across many real ticks through the real physics/collision path,
// exactly like an actual block-break or player Q-drop -- still press a
// wood plate? Section C above proves the SENSING math is correct once an
// item's box is exactly aligned with the plate's sensitivity box; this
// section additionally proves the real gravity SETTLING path (entity_try_move
// + the 0.04F/tick gravity accel in entity_item.c's entity_server_tick)
// actually lands the item inside that box, closing the gap the maintainer's
// hardware field report raised (items reportedly not triggering plates on a
// real Wii). Both pass on PC -- no code defect reproduces here; the item
// settles at pos.y == plate_y + 0.1558 (support top 91.0 + item half-height
// 0.125 + a small penetration/step remainder), inside the plate's sensitivity
// column [plate_y, plate_y+0.25], and the plate presses on tick 12.
#define C2_WOOD_BX 33
#define C2_WOOD_BZ 30
static void section_c2(void) {
	park_player();
	set(C2_WOOD_BX, C_YF, C2_WOOD_BZ, BLOCK_STONE, 0);
	set(C2_WOOD_BX, C_PLATE_Y, C2_WOOD_BZ, BLOCK_WOOD_PRESSURE_PLATE, 0);

	struct entity* it = spawn_entity_slot();
	entity_item(it->id, it, true, &s.world,
		(struct item_data) {.id = BLOCK_DIRT, .durability = 0, .count = 1});
	it->data.item.pickup_delay = 1000; // never let the (far-away) player collect it mid-test
	glm_vec3_zero(it->vel);
	it->teleport(it, (vec3) {(float)C2_WOOD_BX + 0.5F, (float)C_PLATE_Y + 3.0F,
							 (float)C2_WOOD_BZ + 0.5F});

	bool pressed = false;
	for(int i = 0; i < 200 && !pressed; i++) {
		assert(server_local_test_tick_entities(&s));
		pressed = meta_at(C2_WOOD_BX, C_PLATE_Y, C2_WOOD_BZ) == 1;
	}
	assert(pressed
		   && "a naturally gravity-settled item must press a WOOD plate "
			  "(field regression B: PC repro attempt -- passes here)");

	printf("  [C2] a naturally gravity-dropped item (real fall physics, not "
		   "teleported into place) presses a wood plate: OK\n");
}

/* =========================================================================
   SECTION D -- an arrow resting in a WOOD plate's box does NOT press it,
   even under real production entity ticks (EntityArrow never calls
   Entity.moveEntity in Beta 1.7.3 -- see the file header citation).
   ========================================================================= */

#define D_YF 90
#define D_PLATE_Y 91
#define D_BX 40
#define D_BZ 40

static void section_d(void) {
	park_player();
	set(D_BX, D_YF, D_BZ, BLOCK_STONE, 0);
	set(D_BX, D_PLATE_Y, D_BZ, BLOCK_WOOD_PRESSURE_PLATE, 0);
	assert(meta_at(D_BX, D_PLATE_Y, D_BZ) == 0);

	// Defensive parity with tests/test_arrow_env.c's own setup -- an arrow's
	// tick_server probes s.player for interception/homing; a far-parked,
	// non-"has_pos" player keeps that path inert and irrelevant to this test.
	s.player.has_pos = false;
	s.player.finished_loading = false;

	struct entity* arrow = spawn_entity_slot();
	entity_arrow(arrow->id, arrow, true, &s.world, 0, ITEM_ARROW, ARROW_DAMAGE, false);
	// ARROW_BBOX_SIZE (entity_arrow.c) is 0.5F, y_offset 0.0F -> world Y =
	// [pos_y, pos_y+0.5], the SAME "sits at the plate's own Y" placement as
	// section_b's mob -- geometric overlap with the sensitivity box is just
	// as real as the item's in section C. Zero velocity so the arrow doesn't
	// need to already be "stuck" for this: no wiring calls into it either way.
	arrow->teleport(arrow, (vec3) {(float)D_BX + 0.5F, (float)D_PLATE_Y, (float)D_BZ + 0.5F});
	glm_vec3_zero(arrow->vel);

	for(int t = 0; t < 5; t++)
		assert(server_local_test_tick_entities(&s));

	assert(meta_at(D_BX, D_PLATE_Y, D_BZ) == 0
		   && "an arrow must NOT press a pressure plate -- EntityArrow.onUpdate "
			  "never calls Entity.moveEntity in Beta 1.7.3, so "
			  "onEntityCollidedWithBlock is never reached, wood plate or not "
			  "(contrast with section C's item at the identical geometry class)");

	s.player.has_pos = true;
	s.player.finished_loading = true;

	printf("  [D] an arrow resting in a WOOD plate's box does NOT press it "
		   "under real production ticks (contrast with C): OK\n");
}

/* =========================================================================
   SECTION E -- support-drop (preserved existing coverage, revalidated
   against the rewritten onNeighbourBlockChange).
   ========================================================================= */

#define E_YF 90
#define E_PLATE_Y 91
#define E_BX 5
#define E_BZ 5

static void section_e(void) {
	park_player();
	set(E_BX, E_YF, E_BZ, BLOCK_STONE, 0);
	set(E_BX + 1, E_YF, E_BZ, BLOCK_STONE, 0);
	set(E_BX + 1, E_PLATE_Y, E_BZ, BLOCK_REDSTONE_WIRE, 0);
	set(E_BX, E_PLATE_Y, E_BZ, BLOCK_STONE_PRESSURE_PLATE, 1); // pre-pressed

	assert(meta_at(E_BX + 1, E_PLATE_Y, E_BZ) == 15
		   && "isPoweringTo: metadata>0 powers every adjacent cell, side-independent");

	set(E_BX, E_YF, E_BZ, BLOCK_AIR, 0); // mine the support
	assert(type_at(E_BX, E_PLATE_Y, E_BZ) == BLOCK_AIR
		   && "the plate must pop (no float) once its support is gone");
	assert(meta_at(E_BX + 1, E_PLATE_Y, E_BZ) == 0
		   && "no phantom power once the plate itself is gone");

	printf("  [E] support-drop pops a pressed plate and clears its phantom "
		   "power: OK\n");
}

/* =========================================================================
   SECTION F -- a bound guest is a second EntityPlayer in both pressure-plate
   queries. Beta BlockPressurePlate.setStateIfMobInteractsWithPlate queries
   World's entity lists (EntityLiving for stone, every Entity for wood), so a
   second player must press, sustain the tickRate()==20 poll, then disappear
   from the next poll after its lifetime is released. [b1.7.3 server decompile:
   BlockPressurePlate.java:69-110; World.java:1751-1784]
   ========================================================================= */

#define F_YF 90
#define F_PLATE_Y 91
#define F_BX 50
#define F_BZ 50

static void section_f_guest(void) {
	park_player();
	set(F_BX, F_YF, F_BZ, BLOCK_STONE, 0);
	set(F_BX, F_PLATE_Y, F_BZ, BLOCK_STONE_PRESSURE_PLATE, 0);
	static struct server_player guest;
	memset(&guest, 0, sizeof guest);
	guest.has_pos = true;
	guest.health = 0; /* a bound body still occupies space before respawn */
	guest.x = F_BX + 0.5;
	guest.y = F_PLATE_Y + PLAYER_EYE_HEIGHT;
	guest.z = F_BZ + 0.5;
	s.guest_actor = &guest;
	s.guest_generation = 77;
	s.guest_gameplay = false; /* offered/window-only: presence is geometric */

	/* This is the real onEntityCollidedWithBlock bridge. It calls the same
	 * sensing routine as every scheduled recheck; no private test predicate. */
	block_pressure_plate_on_entity_tick(&s, F_BX, F_PLATE_Y, F_BZ);
	assert(meta_at(F_BX, F_PLATE_Y, F_BZ) == 1
		   && "a bound guest alone must press a stone plate");
	uint64_t pressed_at = s.world.current_tick;
	for(int t = 1; t < 20; t++) {
		world_tick();
		assert(meta_at(F_BX, F_PLATE_Y, F_BZ) == 1);
	}
	world_tick();
	assert(s.world.current_tick == pressed_at + 20);
	assert(meta_at(F_BX, F_PLATE_Y, F_BZ) == 1
		   && "guest presence must sustain the scheduled tick-20 recheck");

	/* Release owns the lifetime boundary. Stale coordinates must not keep
	 * power alive after guest_actor/generation are cleared. */
	s.guest_actor = NULL;
	s.guest_generation = 0;
	for(int t = 1; t < 20; t++) {
		world_tick();
		assert(meta_at(F_BX, F_PLATE_Y, F_BZ) == 1);
	}
	world_tick();
	assert(meta_at(F_BX, F_PLATE_Y, F_BZ) == 0
		   && "the first scheduled recheck after guest release must clear");
	printf("  [F] guest-only press/hold/release through real plate scheduling: OK\n");
}

int main(void) {
	test_fixture_dir("test_pressure_plate_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	printf("test_pressure_plate_golden: audit #24 BlockPressurePlate (Beta "
		   "1.7.3, 740c583)\n");

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 0; /* unlimited: this golden pins exact tick counts */
	park_player();

	// Every scene's coordinates, including Beta's position +/-32 entity-update
	// gate and the scheduled-tick/notify footprints, fit inside this grid:
	// x/z in [-32, 95].
	struct server_chunk* sc = NULL;
	for(int lcx = -2; lcx <= 5; lcx++)
		for(int lcz = -2; lcz <= 5; lcz++) {
			sc = NULL;
			assert(server_world_load_chunk(&s.world, lcx, lcz, &sc) && sc);
		}

	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	section_a();
	section_b();
	section_c();
	section_c2();
	section_d();
	section_e();
	section_f_guest();

	g_drain_run = 0;
	printf("test_pressure_plate_golden: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
