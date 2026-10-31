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

#ifndef BLOCKS_H
#define BLOCKS_H

#include <stdbool.h>
#include <stdint.h>

#include "../graphics/texture_atlas.h"
// Step 7 split: struct block embeds struct item BY VALUE, so the item value
// types are a genuine dependency -- but only the types. The item API +
// registry (items.h) stays out: item API edits no longer rebuild every
// blocks.h consumer (~660 objects measured 2026-08-27).
#include "../item/item_types.h"
#include "../platform/displaylist.h"
#include "../util.h"
#include "../worldgen/java_random.h" // F4-RNG: getDroppedItem draws World.rand (java.util.Random)
#include "aabb.h"
#include "blocks_data.h"
#include "face_occlusion.h"

/* S1-a1b: the acting actor for the player-driven hooks below (place /
 * activate / item use). Declared in server_local_state.h; blocks.h stays
 * opaque over it, exactly as it already is over struct server_local. */
struct server_player;

struct block {
	char name[32];
	enum block_material (*getMaterial)(struct block_info*);
	uint8_t (*getTextureIndex)(struct block_info*, enum side);
	struct face_occlusion* (*getSideMask)(struct block_info*, enum side,
										  struct block_info*);
	size_t (*getBoundingBox)(struct block_info*, bool, struct AABB*);
	size_t (*renderBlock)(struct displaylist*, struct block_info*, enum side,
						  struct block_info*, uint8_t*, bool);
	size_t (*renderBlockAlways)(struct displaylist*, struct block_info*,
								enum side, struct block_info*, uint8_t*, bool);
	size_t (*getDroppedItem)(struct block_info*, struct item_data*,
							 struct java_random*, struct server_local*);
	// Ownership teardown independent of whether a block item drops. Container
	// tile entities must be released on every removal (TNT's drop roll is only
	// 33%); implementations are idempotent because some legacy drop paths call
	// this defensively more than once.
	void (*onBreak)(struct server_local*, struct block_info*);
	// Beta Chunk.setBlockID[WithMetadata] invokes the OLD block's removal
	// callback after publishing the replacement id but before writing its new
	// metadata. This is lifecycle/power teardown, distinct from player/explosion
	// drop ownership in onBreak.
	void (*onBlockRemoval)(struct server_local*, struct block_info*);
	// Beta Chunk.setBlockID[WithMetadata] invokes onBlockAdded after the id /
	// metadata write and lighting update, before World.notifyBlockChange tells
	// the six neighbours. This is distinct from an item placement callback:
	// worldgen, fluid spread, and any other setBlock path all pass through it.
	void (*onBlockAdded)(struct server_local*, struct block_info*);
	void (*onRandomTick)(struct server_local*, struct block_info*);
	// S1-a1b: `actor` is the player whose click this is -- the ONLY body,
	// inventory, facing and window this hook may read or write. Never
	// s->player: on a split-screen server that is the HOST, so a guest click
	// would seize P1's screen / pose / health.
	void (*onRightClick)(struct server_local*, struct server_player* actor,
						 struct item_data*,
						 struct block_info*, struct block_info*, enum side);
	// Activation callbacks historically returned void and therefore always consumed
	// the click. Blocks whose Beta blockActivated can return false use this scoped
	// form so item use/placement can fall through on an unhandled activation.
	bool (*onRightClickHandled)(struct server_local*, struct server_player* actor,
							  struct item_data*,
							  struct block_info*, struct block_info*, enum side);
	// R1 (docs/intent/r1-block-click-walk-callbacks.md): Beta Block.onBlockClicked, the
	// LEFT-click / dig-START reaction. Run by the server's dig-start path exactly like
	// ItemInWorldManager.func_324_a:44-47 (onBlockHit first, then the block at the cell,
	// id > 0). `held` is the actor's hotbar item, NULL for an empty hand (Beta reads it
	// off the EntityPlayer; passed in so block files need no inventory include). NULL
	// hook = Beta's base no-op. [740c583: Block.java:486]
	void (*onBlockClicked)(struct server_local* s, struct server_player* actor,
						   const struct item_data* held, struct block_info* this);
	// R1: Beta Block.onEntityWalking, fired by Entity.moveEntity:437-458 on each footstep
	// (canTriggerWalking && !(onGround && sneaking) && ridingEntity == null, the
	// distanceWalkedModified > nextStepDistance crossing with a non-air foot cell). `walker`
	// says whether a player or a mob stepped; like Beta's Entity argument, no 1.7.3 override
	// reads it. NULL hook = Beta's base no-op. [740c583: Block.java:480]
	void (*onEntityWalking)(struct server_local* s, struct block_info* this,
							enum block_walker walker);
	void (*onWorldTick)(struct server_local* s, struct block_info* this);
	void (*onNeighbourBlockChange)(struct server_local* s, struct block_info* info);
	void (*onDay)(struct server_local* s, struct block_info* info);
	void (*onNight)(struct server_local* s, struct block_info* info);
	uint8_t tick_delay;
	bool transparent;
	uint8_t luminance : 4;
	uint8_t opacity : 4;
	bool double_sided;
	bool can_see_through;
	bool ignore_lighting;
	bool flammable;
	bool place_ignore;
	enum biome_tint biome_tint; // Beta grass/foliage colour tint (default none)
	struct block_dig_data {
		int hardness;
		enum tool_type tool;
		enum tool_tier min;
		enum tool_tier best;
	} digging;
	union block_render_data {
		bool cross_random_displacement;
		bool rail_curved_possible;
	} render_block_data;
	struct item block_item;
};

/* Beta Block.quantityDropped(Random), including every vanilla override.
 * Item identity/count is separate: quantity runs ONCE before chance/id. */
int block_quantity_dropped(uint8_t type, struct java_random* rng);

extern struct block block_bedrock;
extern struct block block_slab;
extern struct block block_dirt;
extern struct block block_log;
extern struct block block_stone;
extern struct block block_leaves;
extern struct block block_grass;
extern struct block block_water_still;
extern struct block block_water_flowing;
extern struct block block_lava_flowing;
extern struct block block_lava_still;
extern struct block block_sand;
extern struct block block_sandstone;
extern struct block block_gravel;
extern struct block block_ice;
extern struct block block_snow;
extern struct block block_snow_block;
extern struct block block_tallgrass;
extern struct block block_deadbush;
extern struct block block_flower;
extern struct block block_rose;
extern struct block block_furnaceoff;
extern struct block block_furnaceon;
extern struct block block_workbench;
extern struct block block_glass;
extern struct block block_clay;
extern struct block block_coalore;
extern struct block block_ironore;
extern struct block block_goldore;
extern struct block block_diamondore;
extern struct block block_redstoneore;
extern struct block block_redstoneore_lit;
extern struct block block_lapisore;
extern struct block block_wooden_stairs;
extern struct block block_stone_stairs;
extern struct block block_obsidian;
extern struct block block_spawner;
extern struct block block_cobblestone;
extern struct block block_mossstone;
extern struct block block_chest;
extern struct block block_iron_chest;
extern struct block block_locked_chest;
extern struct block block_redstone_wire;
extern struct block block_cactus;
extern struct block block_pumpkin;
extern struct block block_pumpkin_lit;
extern struct block block_brown_mushroom;
extern struct block block_red_mushroom;
extern struct block block_reed;
extern struct block block_glowstone;
extern struct block block_torch;
extern struct block block_lever; // id 69 (Beta BlockLever)
extern struct block block_rail;
extern struct block block_powered_rail;
extern struct block block_detector_rail;
extern struct block block_redstone_torch;
extern struct block block_redstone_torch_lit;
extern struct block block_stone_button; // id 77 (Beta BlockButton)
extern struct block block_redstone_repeater;
extern struct block block_redstone_repeater_lit;
extern struct block block_ladder;
extern struct block block_farmland;
extern struct block block_crops;
extern struct block block_planks;
extern struct block block_portal;
extern struct block block_iron;
extern struct block block_gold;
extern struct block block_diamond;
extern struct block block_lapis;
extern struct block block_cake;
extern struct block block_fire;
extern struct block block_double_slab;
extern struct block block_bed;
extern struct block block_sapling;
extern struct block block_bricks;
extern struct block block_wool;
extern struct block block_netherrack;
extern struct block block_soulsand;
extern struct block block_bookshelf;
extern struct block block_stone_pressure_plate;
extern struct block block_wooden_pressure_plate;
extern struct block block_jukebox;
extern struct block block_noteblock;
extern struct block block_sponge;
extern struct block block_dispenser;
extern struct block block_tnt;
extern struct block block_cobweb;
extern struct block block_piston;		 // id 33 (Beta pistonBase)
extern struct block block_piston_sticky; // id 29 (Beta pistonStickyBase)
extern struct block block_piston_head;	 // id 34 (Beta pistonExtension)
extern struct block block_piston_moving; // id 36 (Beta pistonMoving)
extern struct block block_fence;
extern struct block block_trapdoor;
extern struct block block_wooden_door;
extern struct block block_iron_door;
extern struct block block_tree2d;
extern struct block block_sign;
//extern struct block block_minecart;

extern struct block* blocks[256];

// (step 7 split: the render_block.h / render_item.h convenience re-exports
// that used to sit here are gone -- render-header edits no longer rebuild
// every simulation TU. Renderer implementations include them directly.)

void blocks_init(void);
bool block_has_day_night_callback(uint8_t type);
// Beta World.isBlockNormalCube: a full opaque solid cube (shared by the redstone
// power model and the redstone-wire renderer; see blocks.c).
bool block_is_normal_cube(uint8_t type);
enum side blocks_side_opposite(enum side s);
void blocks_side_offset(enum side s, int* x, int* y, int* z);
const char* block_side_name(enum side s);

// Beta World.canBlockBePlacedAt entity gate: true if the would-be block at
// blk_info (which must carry the placed block's type/metadata so its OWN
// collision box is used) overlaps the player or any live entity. EVERY placer
// -- block_place_default AND the custom onItemPlace ones -- must gate its
// server_world_set_block on this (review fix 2026-06-11: the sweep used to
// live only in the default path, so custom placers embedded mobs in blocks).
// S1 fix round 2: EVERY present player body is swept -- the host always, the
// split-screen guest whenever one is bound (server_local_present_actors) --
// alongside every live entity, because Beta's checkIfAABBIsClear excludes
// NOBODY (getEntitiesWithinAABBExcludingEntity(null, box)) and EntityPlayer
// carries preventEntitySpawning. So a guest cannot place a block inside the
// host, nor the host inside the guest. With no guest bound the list is just
// the host and the result is byte-identical to the pre-split sweep.
// `actor` is the PLACING player: it no longer selects which bodies are swept
// (S1-a1b's acting-actor-only sweep was the parity gap this closes), but the
// placer is still always tested, so an unbound harness body passed here is
// swept too. There is no actor-free spelling: the 2-argument host wrapper
// this used to carry was deleted (R-S1a1b F4) once S1-a1b converted its last
// callers -- the piston push sweep, the repeater place and the wire settle
// all pass their own actor now. Pass the acting actor; NULL is legal and
// means "no extra placer body", not "skip the present bodies".
bool block_place_obstructed_for(struct server_local* s,
								struct server_player* actor,
								struct block_info* blk_info);

// Beta BlockReed.canBlockStay/canPlaceBlockAt: a reed may exist at (x,y,z) iff the block below is reed,
// OR it is dirt/grass with water adjacent to that block on any of the 4 horizontal sides. Shared by
// block_reed.c (stay/grow + neighbour-change cleanup) and item_sugarcane.c (placement gate).
bool reed_can_block_stay(struct server_local* s, int x, int y, int z);

// Batch 2a bed presentation: the Beta sleeping-body offset within the bed's
// HEAD cell [740c583 EntityPlayer.sleepInBedAt switch]: 0.5 + 0.4*dir per
// axis. Decodes the head block's metadata with block_bed.c's shifted
// direction convention. dx/dz also expose the raw head direction for the
// wake-side scan.
void bed_sleep_body_offset(uint8_t head_meta, float* ox, float* oz, int* dx,
						   int* dz);

// Beta BlockFarmland.onEntityWalking: a 1-in-4 roll reverts the farmland at (x,y,z) to dirt
// (the trample mechanic). Called by the player footstep detector in server_local.c (the split
// of Beta's Entity.moveEntity step-event vs BlockFarmland.onEntityWalking).
void block_farmland_on_entity_walking(struct server_local* s, int x, int y, int z);

// Beta BlockFire.onBlockAdded head: if the block DIRECTLY BELOW the just-placed fire is
// obsidian, attempt portal creation. Dispatched from server_world_set_block for every
// BLOCK_FIRE write (the Chunk.setBlockID -> onBlockAdded chokepoint), which covers all of
// Beta's ignition paths -- flint&steel, fire spread, still-lava ignition, the flaming
// explosion fire pass. Fire AGE bumps deliberately do NOT re-attempt: Beta's age write is
// setBlockMetadata (metadata-only, no onBlockAdded) and ours is the matching _silent
// path, so completing a frame around already-burning fire does not ignite -- authentic.
// The survival-check + first-tick-schedule tail of Beta's onBlockAdded is already
// modeled by the engine's tick machinery. [decompile 740c583 BlockFire.onBlockAdded:192,
// BlockFire.updateTick:66]
void block_fire_on_added(struct server_local* s, w_coord_t x, w_coord_t y, w_coord_t z);

// Beta BlockPortal.tryToCreatePortal(world, x, y, z): validate a corners-exempt 4-wide x
// 5-tall obsidian frame around (x,y,z) (the ignition/fire cell) and, on success, fill the
// 2x3 interior with portal blocks under world.editingBlocks (no neighbour dispatch).
// Returns true iff the portal was created. [decompile 740c583 BlockPortal.tryToCreatePortal]
bool block_portal_try_create(struct server_local* s, w_coord_t x, w_coord_t y, w_coord_t z);

// Beta Material.getIsSolid() by block id (the per-id material transcription lives in
// block_lava.c). Non-solid = air/liquids/fire/plants/circuits/snow-layer/portal;
// LEAVES ARE SOLID. Used by the Teleporter's createPortal floor check.
bool block_beta_material_is_solid(int id);

bool block_place_default(struct server_local* s, struct server_player* actor,
						 struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side);
size_t block_drop_default(struct block_info* this, struct item_data* it,
						  struct java_random* g, struct server_local* s);

// Shared sign placement primitives (block_sign.c). BLOCK_SIGN_POST/63 takes
// yaw metadata 0..15; BLOCK_SIGN/68 takes Beta's raw wall metadata 2..5.
uint8_t block_sign_wall_metadata_for_side(enum side on_side);
bool block_sign_place(struct server_local* s, struct server_player* actor,
					  struct block_info* where,
					  uint8_t type, uint8_t metadata);


struct server_world;
/* A2-T D4: the world a block callback acts on. Block callbacks take `s`
 * only; every block file reaches its world through block_world(s), which is
 * the world of the dispatch in progress -- the actor's committed world for a
 * click/dig/place/use, the ticking world for scheduled/random/day ticks, the
 * notified world for neighbour changes and set-block lifecycle hooks -- and
 * worlds[0] outside any dispatch (so single-player is unchanged).
 * block_world_enter returns the previous value; block_world_leave restores
 * it (nesting-safe: a callback that writes into another world re-enters).
 * A NULL `s` or `w` is a no-op. */
struct server_world* block_world(struct server_local* s);
struct server_world* block_world_enter(struct server_local* s,
									   struct server_world* w);
void block_world_leave(struct server_local* s, struct server_world* prev);

void notifyNeighbours(struct server_local* s, w_coord_t x, w_coord_t y, w_coord_t z);
/* A2-T (D4 seam): the same dispatch against an EXPLICIT world -- the portal
 * teleporter builds frames in the destination world, which is not the routed
 * slot 0 while two worlds are live. Its callbacks run with block_world = w. */
void notifyNeighbours_w(struct server_local* s, struct server_world* w, w_coord_t x,
						w_coord_t y, w_coord_t z);
void notifyNeighbours_from_w(struct server_local* s, struct server_world* w,
							 w_coord_t x, w_coord_t y, w_coord_t z, uint8_t origin);
// Beta notifyBlocksOfNeighborChange with an EXPLICIT source id (var5) -- for the
// "notify my mount/support cell's neighbours with source=ME" component pattern.
void notifyNeighbours_from(struct server_local* s, w_coord_t x, w_coord_t y,
                           w_coord_t z, uint8_t origin);
#define BLOCKS_NOTIFY_NEIGHBOURS_MAX_DEPTH 16
int blocks_notify_neighbours_depth_max(void);
int blocks_notify_neighbours_depth_cap_hits(void);
int blocks_notify_neighbours_cascade_max_dispatches(void);
// Monotonic handler-dispatch total since boot (NOT reset by diag_reset); the
// second half of the hw-run heartbeat "ndisp" scenario signal (twin of
// server_world_notify_dispatch_total).
uint32_t blocks_notify_neighbours_dispatch_total(void);
void blocks_notify_neighbours_diag_reset(void);

#endif
