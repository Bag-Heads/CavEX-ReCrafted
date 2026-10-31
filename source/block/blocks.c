/*
	Copyright (c) 2022 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <stdlib.h>

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../platform/thread.h" // thread_yield: pace this notify cascade on the single core
#include "blocks.h"

struct block* blocks[256];
static bool blocks_day_night_callbacks[256];

/* Owned RetroMCP server Block*.java quantityDropped overrides; full id/class
 * audit and JVM vectors: lead-1010/f4rng-r3/beta-block-audit.json.
 * Inherited overrides returning one (Crops, Log, Ladder, Rail, Obsidian)
 * and Stairs' planks/cobblestone delegation equal Block.java:282's default.
 * PistonMoving delegates the stored block's COMPLETE drop operation instead.
 * Bed's half gate and harvest overrides are outside quantityDropped. */
int block_quantity_dropped(uint8_t type, struct java_random* rng) {
	switch(type) {
		case BLOCK_AIR:
		case BLOCK_WATER_FLOW:
		case BLOCK_WATER_STILL:
		case BLOCK_LAVA_FLOW:
		case BLOCK_LAVA_STILL: // BlockFluid.java:69
		case BLOCK_GLASS: // BlockGlass.java:10
		case BLOCK_PISTON_HEAD: // BlockPistonExtension.java:54
		case BLOCK_TNT: // BlockTNT.java:31
		case BLOCK_BOOKSHELF: // BlockBookshelf.java:14
		case BLOCK_FIRE: // BlockFire.java:43
		case BLOCK_MOB_SPAWNER: // BlockMobSpawner.java:18
		case BLOCK_SNOW: // BlockSnow.java:67 (harvestBlock bypasses this)
		case BLOCK_ICE: // BlockIce.java:21
		case BLOCK_PORTAL: // BlockPortal.java:120
		case BLOCK_CAKE: // BlockCake.java:82
			return 0;
		case BLOCK_LEAVES: // BlockLeaves.java:133
			return jrand_next_int_bound(rng, 20) == 0 ? 1 : 0;
		case BLOCK_LAPIS_ORE: // BlockOre.java:14
			return 4 + jrand_next_int_bound(rng, 5);
		case 73:
		case 74: // BlockRedstoneOre.java:55
			return 4 + jrand_next_int_bound(rng, 2);
		case BLOCK_GLOWSTONE: // BlockGlowStone.java:10
			return 2 + jrand_next_int_bound(rng, 3);
		case BLOCK_DOUBLE_SLAB: // BlockStep.java:52
			return 2;
		case BLOCK_SNOW_BLOCK: // BlockSnowBlock.java:15
		case BLOCK_CLAY: // BlockClay.java:14
			return 4;
		default:
			return 1; // Block.java:282-284
	}
}

static void blocks_build_callback_tables(void) {
	for(int k = 0; k < 256; k++)
		blocks_day_night_callbacks[k]
			= blocks[k] && (blocks[k]->onDay || blocks[k]->onNight);
}

bool block_has_day_night_callback(uint8_t type) {
	return blocks_day_night_callbacks[type];
}

// Beta World.isBlockNormalCube(id): a FULL OPAQUE SOLID cube -- redstone power
// re-emits through these and wire stair-steps over them. Beta's test is
// material.getIsTranslucent() (here: blocks flagged setIsTranslucent() are the
// can_see_through ones -- leaves/glass/ice/snow) ANDed with renderAsNormalBlock();
// the lone opaque non-cube is TNT (Material.tnt.setIsTranslucent). Single source
// of truth shared by the redstone power model (rs_is_normal_cube) and the
// redstone-wire renderer's connection probe so the two can never drift.
//
// next-gaps-0924/W1A-b audit of ids 1-96 against World.isBlockNormalCube:
// 1636-1639 (material.getIsTranslucent() -- Material.java:87-89, i.e. not
// setIsTranslucent and solid -- && renderAsNormalBlock()): can_see_through is
// a RENDER flag (isOpaqueCube), and exactly one Beta block differs from it the
// other way: the MOB SPAWNER (BlockMobSpawner, Material.rock, isOpaqueCube
// false but renderAsNormalBlock inherited true, Block.java:75 +
// BlockMobSpawner.java:7,22) is a normal cube -- it suffocates, conducts
// redstone, mounts torches/levers/buttons. Glowstone is Material.rock in
// b1.7.3 (Block.java:112), so it already reads as a normal cube, correctly.
bool block_is_normal_cube(uint8_t type) {
	const struct block* b = blocks[type];
	if(!b)
		return false;
	if(type == BLOCK_MOB_SPAWNER)
		return true;
	return !b->can_see_through && type != BLOCK_TNT;
}

void blocks_init() {
	for(int k = 0; k < 256; k++)
		blocks[k] = NULL;

	render_block_init();

	blocks[1] = &block_stone;
	blocks[2] = &block_grass;
	blocks[3] = &block_dirt;
	blocks[4] = &block_cobblestone;
	blocks[5] = &block_planks;
	blocks[6] = &block_sapling;
	blocks[7] = &block_bedrock;
	blocks[8] = &block_water_flowing;
	blocks[9] = &block_water_still;
	blocks[10] = &block_lava_flowing;
	blocks[11] = &block_lava_still;
	blocks[12] = &block_sand;
	blocks[13] = &block_gravel;
	blocks[14] = &block_goldore;
	blocks[15] = &block_ironore;
	blocks[16] = &block_coalore;
	blocks[17] = &block_log;
	blocks[18] = &block_leaves;
	blocks[19] = &block_sponge;
	blocks[20] = &block_glass;
	blocks[21] = &block_lapisore;
	blocks[22] = &block_lapis;
	blocks[23] = &block_dispenser;
	blocks[24] = &block_sandstone;
	blocks[25] = &block_noteblock;
	blocks[26] = &block_bed;
	blocks[27] = &block_powered_rail;
	blocks[28] = &block_detector_rail;
	blocks[29] = &block_piston_sticky;
	blocks[30] = &block_cobweb;
	blocks[31] = &block_tallgrass;
	blocks[32] = &block_deadbush;
	blocks[33] = &block_piston;
	blocks[34] = &block_piston_head;
	blocks[35] = &block_wool;
	blocks[36] = &block_piston_moving;
	blocks[37] = &block_flower;
	blocks[38] = &block_rose;
	blocks[39] = &block_brown_mushroom;
	blocks[40] = &block_red_mushroom;
	blocks[41] = &block_gold;
	blocks[42] = &block_iron;
	blocks[43] = &block_double_slab;
	blocks[44] = &block_slab;
	blocks[45] = &block_bricks;
	blocks[46] = &block_tnt;
	blocks[47] = &block_bookshelf;
	blocks[48] = &block_mossstone;
	blocks[49] = &block_obsidian;
	blocks[50] = &block_torch;
	blocks[51] = &block_fire;
	blocks[52] = &block_spawner;
	blocks[53] = &block_wooden_stairs;
	blocks[54] = &block_chest;
	blocks[55] = &block_redstone_wire;
	blocks[56] = &block_diamondore;
	blocks[57] = &block_diamond;
	blocks[58] = &block_workbench;
	blocks[59] = &block_crops;
	blocks[60] = &block_farmland;
	blocks[61] = &block_furnaceoff;
	blocks[62] = &block_furnaceon;
	blocks[63] = &block_sign;
	blocks[64] = &block_wooden_door;
	blocks[65] = &block_ladder;
	blocks[66] = &block_rail;
	blocks[67] = &block_stone_stairs;
	blocks[68] = &block_sign;
	blocks[69] = &block_lever;
	blocks[70] = &block_stone_pressure_plate;
	blocks[71] = &block_iron_door;
	blocks[72] = &block_wooden_pressure_plate;
	blocks[73] = &block_redstoneore;
	blocks[74] = &block_redstoneore_lit;
	blocks[75] = &block_redstone_torch;
	blocks[76] = &block_redstone_torch_lit;
	blocks[77] = &block_stone_button;
	blocks[78] = &block_snow;
	blocks[79] = &block_ice;
	blocks[80] = &block_snow_block;
	blocks[81] = &block_cactus;
	blocks[82] = &block_clay;
	blocks[83] = &block_reed;
	blocks[84] = &block_jukebox;
	blocks[85] = &block_fence;
	blocks[86] = &block_pumpkin;
	blocks[87] = &block_netherrack;
	blocks[88] = &block_soulsand;
	blocks[89] = &block_glowstone;
	blocks[90] = &block_portal;
	blocks[91] = &block_pumpkin_lit;
	blocks[92] = &block_cake;
	blocks[93] = &block_redstone_repeater;
	blocks[94] = &block_redstone_repeater_lit;
	blocks[95] = &block_locked_chest; // Beta Block.lockedChest -- FH-07 (was iron chest)
	blocks[96] = &block_trapdoor;
	blocks[97] = &block_tree2d;
	//blocks[98] = &block_minecart;

	// NET-NEW (non-Beta, FH-07 relocation): iron chest, relocated off Beta's own
	// id 95. See BLOCK_IRON_CHEST's comment in blocks_data.h.
	blocks[200] = &block_iron_chest;

	for(int k = 0; k < 256; k++) {
		if(blocks[k]) {
			assert(blocks[k]->getMaterial);
			assert(blocks[k]->getTextureIndex);
			assert(blocks[k]->getSideMask);
			assert(blocks[k]->getBoundingBox);
			assert(blocks[k]->renderBlock);
			assert(blocks[k]->getDroppedItem);
			assert(blocks[k]->block_item.renderItem);
			assert(blocks[k]->block_item.onItemPlace);
		}
	}

	blocks_build_callback_tables();
}

enum side blocks_side_opposite(enum side s) {
	switch(s) {
		default:
		case SIDE_TOP: return SIDE_BOTTOM;
		case SIDE_BOTTOM: return SIDE_TOP;
		case SIDE_LEFT: return SIDE_RIGHT;
		case SIDE_RIGHT: return SIDE_LEFT;
		case SIDE_FRONT: return SIDE_BACK;
		case SIDE_BACK: return SIDE_FRONT;
	}
}

const char* block_side_name(enum side s) {
	switch(s) {
		case SIDE_TOP: return "top";
		case SIDE_BOTTOM: return "bottom";
		case SIDE_LEFT: return "left";
		case SIDE_RIGHT: return "right";
		case SIDE_FRONT: return "front";
		case SIDE_BACK: return "back";
		default: return "invalid";
	}
}

void blocks_side_offset(enum side s, int* x, int* y, int* z) {
	assert(x && y && z);

	switch(s) {
		default:
		case SIDE_TOP:
			*x = 0;
			*y = 1;
			*z = 0;
			break;
		case SIDE_BOTTOM:
			*x = 0;
			*y = -1;
			*z = 0;
			break;
		case SIDE_LEFT:
			*x = -1;
			*y = 0;
			*z = 0;
			break;
		case SIDE_RIGHT:
			*x = 1;
			*y = 0;
			*z = 0;
			break;
		case SIDE_BACK:
			*x = 0;
			*y = 0;
			*z = 1;
			break;
		case SIDE_FRONT:
			*x = 0;
			*y = 0;
			*z = -1;
			break;
	}
}

// M2 + review fix 2026-06-11: Beta's World.canBlockBePlacedAt entity gate,
// SHARED by every placer. Beta refuses placement when the would-be block's
// collision box overlaps ANY colliding entity (getCollidingBoundingBoxes) --
// the player included -- and runs that gate for EVERY ItemBlock, custom
// placers included. The first cut of this sweep lived only inside
// block_place_default, so chests/dispensers/doors/slabs/stairs/TNT/pumpkins
// (which replicate only the old player-only check) still placed straight
// inside mobs, embedding them in solid geometry. ITEM drops and arrows are
// exempt (Beta places over them; they pop out via their own physics);
// dead/flopping mobs don't block (their box is a corpse visual). The test
// uses the block's OWN bounding box (entity_block_aabb_test reads blk_info),
// so no-collision blocks (signs, open trapdoors) never refuse -- exactly
// Beta's per-block box semantics. Server thread owns s->entities, so the
// sweep is race-free. Design adapted from the lberwa fork analysis, rebuilt
// on our dict + entity_block_aabb_test.
bool block_place_obstructed_for(struct server_local* s,
								struct server_player* actor,
								struct block_info* blk_info) {
	/* S1 fix round 2 (Astra call #8): sweep EVERY player body present in the
	 * world, not just the placer's.
	 *
	 * Beta, exactly: ItemBlock.onItemUse [740c583 ItemBlock.java:45] gates on
	 * World.canBlockBePlacedAt [World.java:2096], which takes the placed
	 * block's OWN getCollisionBoundingBoxFromPool and passes it to
	 * World.checkIfAABBIsClear [World.java:1357]; that calls
	 * World.getEntitiesWithinAABBExcludingEntity((Entity)null, box)
	 * [World.java:1358] and refuses on any returned entity with
	 * `!isDead && preventEntitySpawning`. The EXCLUDED entity is null, so
	 * nobody is exempt -- not even the placer -- and EntityPlayer inherits
	 * preventEntitySpawning = true from EntityLiving [EntityLiving.java], so
	 * on a two-player world BOTH bodies refuse the placement. (S1-a1b's
	 * acting-actor-only sweep was a parity GAP, not a sanctioned deviation:
	 * a guest could place a block inside the host and vice versa.)
	 *
	 * ORDER: Beta's own iteration is the chunk scan inside
	 * getEntitiesWithinAABBExcludingEntity, which mixes players and mobs in
	 * one list. This gate is a pure OR with no side effects and no output but
	 * the bool, so WHICH overlapping body is found first is not observable;
	 * the bodies go first because they are not in s->entities and must be
	 * enumerated separately, then the entity sweep runs exactly as before.
	 *
	 * HOST-ONLY IS BYTE-IDENTICAL: with no guest bound the presence list is
	 * {&s->player} and this is the same single unconditional body test the
	 * pre-split sweep did (proved by test_split_off_equiv and
	 * test_block_hooks_actor_golden, whose STATE/RPC-ORDER hashes are
	 * unchanged by this commit). The host leg has always been unconditional
	 * -- no health/isDead gate -- and the guest leg follows the same rule, so
	 * the two bodies obey one rule rather than two. */
	struct server_player* bodies[SERVER_LOCAL_MAX_PRESENT_ACTORS];
	size_t body_count = server_local_present_actors(
		s, bodies, SERVER_LOCAL_MAX_PRESENT_ACTORS);
	bool actor_present = (actor == NULL);
	for(size_t i = 0; i < body_count; i++) {
		if(bodies[i] == actor)
			actor_present = true;
		if(entity_local_player_block_collide(
			   (vec3) {bodies[i]->x, bodies[i]->y, bodies[i]->z}, blk_info))
			return true;
	}
	/* The placer is ALWAYS tested. A harness actor that was never bound to
	 * this server (a stack body in a unit test) is not in the presence list,
	 * so sweep it here rather than silently skipping it -- production callers
	 * always pass the host or the bound guest, both of which are above. */
	if(!actor_present
	   && entity_local_player_block_collide(
		   (vec3) {actor->x, actor->y, actor->z}, blk_info))
		return true;

	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(!e || !e->getBoundingBox)
			continue;
		if(e->type == ENTITY_ITEM || e->type == ENTITY_ARROW)
			continue; // exempt per Beta
		if(e->type == ENTITY_MONSTER
		   && (e->health <= 0 || e->data.monster.death_time > 0))
			continue; // corpses don't block
		struct AABB ebox;
		if(!e->getBoundingBox(e, &ebox))
			continue;
		if(entity_block_aabb_test(&ebox, blk_info))
			return true;
	}
	return false;
}

bool block_place_default(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	struct block_data blk = (struct block_data) {
		.type = it->id,
		.metadata = it->durability,
		.sky_light = 0,
		.torch_light = 0,
	};

	struct block_info blk_info = *where;
	blk_info.block = &blk;

	if(block_place_obstructed_for(s, actor, &blk_info))
		return false;

	server_world_set_block(block_world(s), where->x, where->y, where->z, blk);
	return true;
}

size_t block_drop_default(struct block_info* this, struct item_data* it,
						  struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = this->block->type;
		it->durability = 0;
		it->count = 1;
	}

	return 1;
}

/* Beta World.notifyBlocksOfNeighborChange exact call order. This is observable
 * when one callback mutates what a later callback reads (especially redstone). */
static const int dx[6] = {-1, 1, 0, 0, 0, 0};
static const int dy[6] = {0, 0, -1, 1, 0, 0};
static const int dz[6] = {0, 0, 0, 0, -1, 1};

// WII single-core PACING + recursion bound for THIS notify cascade -- the twin of
// server_world_notify_block (server_world.c). A handler here may server_world_set_block ->
// notify again (the torch support-drop / lever / plate / trapdoor route through here), so an
// unguarded run could starve the main render heartbeat on the 729 MHz Broadway core. The periodic
// thread_yield hands the core back to render mid-cascade (no-op on PC -> byte-exact, proven by the
// redstone golden tests); BLOCKS_NOTIFY_NEIGHBOURS_MAX_DEPTH mirrors the server_world notify cap
// (real circuits nest 1-2, so 16 is never hit -> byte-exact; it only caps a pathological chain
// below the 64 KB stack).
static int s_nn_depth = 0;
static unsigned s_nn_yield_ctr = 0;
static int s_nn_depth_max = 0;
static int s_nn_depth_cap_hits = 0;
static int s_nn_cascade_dispatches = 0;
static int s_nn_cascade_max_dispatches = 0;
static uint32_t s_nn_dispatch_total = 0; // monotonic; survives diag_reset

uint32_t blocks_notify_neighbours_dispatch_total(void) { return s_nn_dispatch_total; }
int blocks_notify_neighbours_depth_max(void) { return s_nn_depth_max; }
int blocks_notify_neighbours_depth_cap_hits(void) { return s_nn_depth_cap_hits; }
int blocks_notify_neighbours_cascade_max_dispatches(void) { return s_nn_cascade_max_dispatches; }
void blocks_notify_neighbours_diag_reset(void) {
    s_nn_depth_max = 0;
    s_nn_depth_cap_hits = 0;
    s_nn_cascade_dispatches = 0;
    s_nn_cascade_max_dispatches = 0;
}

// Beta notifyBlocksOfNeighborChange(x, y, z, SOURCE id): notify the 6 neighbours of (x,y,z)
// with an EXPLICIT var5. Needed by the "notify my MOUNT cell's neighbours with source=ME"
// pattern (BlockButton/BlockLever blockActivated/updateTick/onBlockRemoval,
// BlockPressurePlate's below-cell notify) -- there the origin is the COMPONENT, not the block
// at the notify center.
void notifyNeighbours_from(struct server_local* s,
                           w_coord_t x, w_coord_t y, w_coord_t z, uint8_t origin)
{
    notifyNeighbours_from_w(s, block_world(s), x, y, z, origin);
}

void notifyNeighbours_from_w(struct server_local* s, struct server_world* w,
                             w_coord_t x, w_coord_t y, w_coord_t z, uint8_t origin)
{
    if (server_world_simulation_stopped(w))
        return;
    const bool top = (s_nn_depth == 0);
    if(top)
        s_nn_cascade_dispatches = 0;
    if (s_nn_depth >= BLOCKS_NOTIFY_NEIGHBOURS_MAX_DEPTH) {
        s_nn_depth_cap_hits++;
        return; // pathological deep cascade: settle on the next neighbour change
    }
    s_nn_depth++;
    if(s_nn_depth > s_nn_depth_max)
        s_nn_depth_max = s_nn_depth;
    for (int i = 0; i < 6; i++) {
        // A nested callback can halt lighting. Stop before the next neighbour's
        // scheduling/callback effects, including on entry to a halted world.
        // Keep the normal Beta order and unwind the cascade bookkeeping below.
        if (server_world_simulation_stopped(w))
            break;
        w_coord_t nx = x + dx[i];
        w_coord_t ny = y + dy[i];
        w_coord_t nz = z + dz[i];

        struct block_data nb;
        if (!server_world_get_block(w, nx, ny, nz, &nb))
            continue;

        const struct block* b = blocks[nb.type];
        // GAP #1 (this dispatcher's half, matching server_world_notify_block): a block that
        // defines onNeighbourBlockChange self-schedules its own ticks -- an auto-schedule
        // here would plant a constant-delay entry that the keep-first (pos,type) dedup makes
        // AUTHORITATIVE over the component's own arm (a delay-1 phantom toggle for a stable
        // repeater; a press-shortening stale entry for the button).
        if (b && b->onWorldTick && !b->onNeighbourBlockChange)
            server_world_schedule_block_tick(w, nx, ny, nz, nb.type);

        if (b && b->onNeighbourBlockChange) {
            // Pace: every 32nd handler dispatch yield the single Broadway core to render so the
            // main heartbeat advances mid-cascade. No-op on PC -> byte-exact.
            if ((++s_nn_yield_ctr & 31u) == 0)
                thread_yield();
            s_nn_cascade_dispatches++;
            s_nn_dispatch_total++;
            struct block_info info = {
                .block      = &nb,
                .neighbours = NULL,
                .x          = nx,
                .y          = ny,
                .z          = nz
            };
            server_world_notify_origin_set(origin); // Beta var5 (the caller's source id)
            struct server_world* bw_prev = block_world_enter(s, w);
            b->onNeighbourBlockChange(s, &info);
            block_world_leave(s, bw_prev);
        }
    }
    s_nn_depth--;
    if(top && s_nn_cascade_dispatches > s_nn_cascade_max_dispatches)
        s_nn_cascade_max_dispatches = s_nn_cascade_dispatches;
}

struct server_world* block_world(struct server_local* s) {
	assert(s);
	return s->block_world ? s->block_world : &s->worlds[0];
}

struct server_world* block_world_enter(struct server_local* s,
									   struct server_world* w) {
	if(!s || !w)
		return s ? s->block_world : NULL;
	struct server_world* prev = s->block_world;
	s->block_world = w == &s->worlds[0] ? NULL : w;
	return prev;
}

void block_world_leave(struct server_local* s, struct server_world* prev) {
	if(s)
		s->block_world = prev;
}

void notifyNeighbours(struct server_local* s,
                      w_coord_t x, w_coord_t y, w_coord_t z)
{
    notifyNeighbours_w(s, block_world(s), x, y, z);
}

void notifyNeighbours_w(struct server_local* s, struct server_world* w,
                        w_coord_t x, w_coord_t y, w_coord_t z)
{
    // Beta var5: callers invoke notifyNeighbours(x,y,z) right after changing (x,y,z), so the
    // notify origin is the CURRENT block there.
    struct block_data origin_blk;
    uint8_t origin = server_world_get_block(w, x, y, z, &origin_blk)
        ? origin_blk.type
        : 0;
    notifyNeighbours_from_w(s, w, x, y, z, origin);
}


#ifdef CAVEWII_SERVER_LOCAL_TEST
int blocks_test_notify_depth(void) { return s_nn_depth; }
#endif
