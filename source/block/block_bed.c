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

#include "../daytime.h"
#include "../entity/combat.h"
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/client_interface.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_local_guest.h" // A1d: per-actor sleep/HUD (lane C)
#include "bed_meta.h" // PAR-BED-META: Beta 1.7.3 bed metadata layout
#include "blocks.h"

#include <math.h>

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_WOOD;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x)
		aabb_setsize(x, 1.0F, 0.5625F, 1.0F);
	return 1;
}

// TODO: add missing bed legs to mask? (difficult, many states)
static struct face_occlusion side_mask = {
	.mask = {0x00000000, 0x00000000, 0x00000000, 0x0000FFFF, 0xFFFFFFFF,
			 0xFFFFFFFF, 0xFFFF0000, 0x00000000},
};

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	switch(side) {
		case SIDE_TOP:
		case SIDE_BOTTOM: return face_occlusion_empty();
		default: return &side_mask;
	}
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
    /* Beta layout (bed_meta.h): 0x8 = pillow half; the end tiles sit on the
     * faces across the bed's axis (X faces for directions 1/3). */
    const bool ends_x = bed_meta_ends_on_x_axis(this->block->metadata);
    if (bed_meta_is_head(this->block->metadata)) {
		switch(side) {
			case SIDE_TOP: return tex_atlas_lookup(TEXAT_BED_TOP_2);
			case SIDE_LEFT:
			case SIDE_RIGHT:
				return ends_x ?
					tex_atlas_lookup(TEXAT_BED_FRONT) :
					tex_atlas_lookup(TEXAT_BED_SIDE_2);
			case SIDE_FRONT:
			case SIDE_BACK:
				return ends_x ?
					tex_atlas_lookup(TEXAT_BED_SIDE_2) :
					tex_atlas_lookup(TEXAT_BED_FRONT);
			default: return tex_atlas_lookup(TEXAT_PLANKS);
		}
	} else {
		switch(side) {
			case SIDE_TOP: return tex_atlas_lookup(TEXAT_BED_TOP_1);
			case SIDE_LEFT:
			case SIDE_RIGHT:
				return ends_x ?
					tex_atlas_lookup(TEXAT_BED_BACK) :
					tex_atlas_lookup(TEXAT_BED_SIDE_1);
			case SIDE_FRONT:
			case SIDE_BACK:
				return ends_x ?
					tex_atlas_lookup(TEXAT_BED_SIDE_1) :
					tex_atlas_lookup(TEXAT_BED_BACK);
			default: return tex_atlas_lookup(TEXAT_PLANKS);
		}
	}
}

// Beta BlockBed.idDropped:127-129 `isBlockFootOfBed(meta) ? 0 : Item.bed`
// with isBlockFootOfBed = (meta & 8) != 0. PAR-BED-META (2026-10-21): this
// project now stores Beta's own layout (bed_meta.h), so the literal applies --
// the 0x8 half (the pillow half ItemBed places at foot + dir, the one
// blockActivated sleeps in) never drops; the other half drops the bed. The
// old code inverted this on the belief that the port had mirrored the bit;
// it never had (ItemBed and this port both put 0x8 on the far half).
static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
    if (it) {
        it->id = ITEM_BED;
        it->durability = 0;
        it->count = 1;
    }
    if (bed_meta_is_head(this->block->metadata)) return 0;
	return 1;
}

// Beta BlockBed.onNeighborBlockChange [RetroMCP BlockBed.java:111-125]:
//
//   int dir = getDirectionFromMetadata(meta);           // meta & 3
//   if (isBlockFootOfBed(meta)) {                        // (meta & 8) != 0
//       if (getBlockId(x - headBlockToFootBlockMap[dir][0], y,
//                       z - headBlockToFootBlockMap[dir][1]) != bed.blockID)
//           setBlockWithNotify(x, y, z, 0);               // 0x8 half: SILENT
//   } else if (getBlockId(x + headBlockToFootBlockMap[dir][0], y,
//                          z + headBlockToFootBlockMap[dir][1]) != bed.blockID) {
//       setBlockWithNotify(x, y, z, 0);
//       if (!multiplayerWorld) dropBlockAsItem(x, y, z, meta); // drops
//   }
//
// Ported literally on Beta's own metadata (PAR-BED-META): the 0x8 half finds
// its partner at (x,z) - dir and pops silently; the other half finds it at
// (x,z) + dir and pops AND drops. The occupied bit 0x4 plays no part.
//
// Drop-then-clear order (matching every other onNeighbourBlockChange in this
// codebase -- block_sapling.c/block_flower.c/block_brown_mushroom.c) rather
// than Beta's literal clear-then-drop: dropping first reads the pre-mutation
// metadata/type off `info->block` unambiguously, with no dependency on whether
// that pointer is a live chunk cell or a dispatcher snapshot. No observable
// behavioural difference -- both happen synchronously within the same
// notification, before anything else can observe the intermediate state.
static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	uint8_t meta = info->block->metadata;
	const int dx = bed_meta_dx(meta), dz = bed_meta_dz(meta);

	if(bed_meta_is_head(meta)) {
		// 0x8 half: its partner sits at (x,z) - dir; pops silently.
		struct block_data foot;
		if(!server_world_get_block(block_world(s), info->x - dx, info->y,
								   info->z - dz, &foot)
		   || foot.type != BLOCK_BED) {
			server_world_set_block(block_world(s), info->x, info->y, info->z,
								   (struct block_data) {.type = BLOCK_AIR,
														.metadata = 0});
		}
	} else {
		// Half without 0x8: its partner sits at (x,z) + dir; pops AND drops.
		struct block_data head;
		if(!server_world_get_block(block_world(s), info->x + dx, info->y,
								   info->z + dz, &head)
		   || head.type != BLOCK_BED) {
			server_local_spawn_block_drops(s, info); // drops the bed item
			server_world_set_block(block_world(s), info->x, info->y, info->z,
								   (struct block_data) {.type = BLOCK_AIR,
														.metadata = 0});
		}
	}
}

/* [RetroMCP EntityPlayer.sleepInBedAt:557-578]: the lying body sits in the
 * HEAD cell at 0.5 +/- 0.4 toward the head direction (Beta's per-direction
 * 0.1/0.9 switch on meta&3 collapses to 0.5 + 0.4*dir on each axis).
 * Exported for the server's sleep body-snap (batch 2a); dx/dz expose the raw
 * direction for the wake empty-cell scan. */
void bed_sleep_body_offset(uint8_t head_meta, float* ox, float* oz, int* dx,
						   int* dz) {
	const int ddx = bed_meta_dx(head_meta), ddz = bed_meta_dz(head_meta);
	if(ox)
		*ox = 0.5f + 0.4f * (float)ddx;
	if(oz)
		*oz = 0.5f + 0.4f * (float)ddz;
	if(dx)
		*dx = ddx;
	if(dz)
		*dz = ddz;
}

/* BlockBed.blockActivated:18-28 normalizes every interaction to the 0x8
 * half before sleeping/exploding: a half without 0x8 steps +dir and must
 * find a bed there (its metadata is then used). */
static bool bed_normalize_head(struct server_world* w,
							   const struct block_info* clicked,
							   int* x, int* y, int* z, uint8_t* meta) {
	if(!w || !clicked || !clicked->block || !x || !y || !z || !meta)
		return false;
	*x = clicked->x;
	*y = clicked->y;
	*z = clicked->z;
	*meta = clicked->block->metadata;
	if(bed_meta_is_head(*meta))
		return true;
	*x += bed_meta_dx(*meta);
	*z += bed_meta_dz(*meta);
	struct block_data head;
	if(!server_world_get_block(w, *x, *y, *z, &head)
	   || head.type != BLOCK_BED)
		return false;
	*meta = head.metadata;
	return true;
}

static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it,
                         struct block_info* where, struct block_info* on,
                         enum side on_side) {
    // Beta BlockBed.onBlockActivated: where you CANNOT respawn
    // (WorldProviderHell.canRespawnHere() == false, i.e. the nether), using a bed
    // does not sleep and shows no message -- it removes the bed and detonates a
    // power-5.0 FLAMING explosion. Beta's exact walk on Beta's own metadata
    // (bed_meta.h: direction meta&3, 0x8 = head): normalize the clicked half
    // to the HEAD, remove it, step
    // once MORE in the same direction (in Beta this lands one cell PAST the head
    // -- the second removal usually no-ops and the explosion centers there; the
    // FOOT half is left for the power-5 blast to destroy, and the averaged
    // center Beta computes in var16/var17/var11 is dead code). Faithful jank,
    // kept on purpose. [decompile 740c583 BlockBed.onBlockActivated:30-45 +
    // WorldProviderHell.canRespawnHere:45]
	/* A1d (split-dimensions-plan-0924 row #3; A1-CONTRACT "A1d/C implements
	 * actor sleep/wake/spawn state and Overworld-only quorum"): the S1-a1b
	 * fail-closed guard is GONE. BlockBed.blockActivated runs for the
	 * CLICKING player (BlockBed.java:14-85, sleepInBedAt for var5 at :70):
	 * every step below takes `actor`, reads the actor's OWN world (D8,
	 * server_local_world_of) -- so the Nether blast is decided by the world
	 * the actor stands in, not a session-wide flag -- and sleeps, messages and
	 * spawns on that actor's own state and route. For the host every value is
	 * the one it was. */
	struct server_world* w = server_local_world_of(s, actor);
	if(!w)
		return;

	int head_x, head_y, head_z;
	uint8_t head_meta;
	if(!bed_normalize_head(
		   w, on, &head_x, &head_y, &head_z, &head_meta))
		return;
	/* EntityPlayer.sleepInBedAt returns OTHER_PROBLEM immediately for a player
	 * already sleeping. In particular, a repeated click must not reset the
	 * 0..100 sleep timer and postpone dawn indefinitely. */
	if(actor->sleeping)
		return;
	/* EntityPlayer.sleepInBedAt enforces these exact bounds in the Overworld;
	 * the vanilla network layer also prevents a client from activating a remote
	 * block in either dimension. Cave's block RPC has no separate reach guard,
	 * so enforce the shared authoritative boundary here before a Nether blast can
	 * mutate terrain. Cave stores eye Y while Beta Entity.posY is the feet. */
	double feet_y = actor->y - PLAYER_EYE_HEIGHT;
	if(!actor->has_pos || actor->health <= 0
	   || fabs(actor->x - (double)head_x) > 3.0
	   || fabs(feet_y - (double)head_y) > 2.0
	   || fabs(actor->z - (double)head_z) > 3.0)
		return;

    if (w->dimension == WORLD_DIM_NETHER) {
		int x = head_x, y = head_y, z = head_z;
        server_world_set_block(w, x, y, z,
                               (struct block_data) {.type = BLOCK_AIR});
        x += bed_meta_dx(head_meta); // Beta's second += map step (past the head)
        z += bed_meta_dz(head_meta);
        struct block_data past;
        if (server_world_get_block(w, x, y, z, &past)
            && past.type == BLOCK_BED)
            server_world_set_block(w, x, y, z,
                                   (struct block_data) {.type = BLOCK_AIR});
        /* RetroMCP BlockBed.java:45 adds in float before widening to double. */
        server_world_explode_flaming_precise(
            w, (double[3]) {(double)((float)x + 0.5F),
                           (double)((float)y + 0.5F),
                           (double)((float)z + 0.5F)},
            5.0F);
        return;
    }

	/* BlockBed.blockActivated:48-64 ("tile.bed.occupied"): with two players
	 * this is reachable -- the OTHER body already sleeps in this bed. Beta
	 * tests it after the Nether branch and before sleepInBedAt, sends the
	 * message to the clicking player and returns. */
	/* PAR-BED-META: Beta gates the scan on the head's occupied bit (0x4) and,
	 * when no sleeper is found there, clears the stale bit with
	 * setBlockMetadataWithNotify before trying to sleep (:48-68). */
	if(bed_meta_is_occupied(head_meta)) {
		if(server_local_bed_occupied_by_other(s, actor, head_x, head_y,
											  head_z)) {
			server_local_actor_hud_message(s, actor, "This bed is occupied");
			return;
		}
		head_meta = (uint8_t)(head_meta & ~BED_META_OCCUPIED);
		server_world_set_block_metadata_with_notify(w, head_x, head_y, head_z,
													head_meta);
	} else if(server_local_bed_occupied_by_other(s, actor, head_x, head_y,
												 head_z)) {
		/* Unreachable when every sleep entry sets the bit (below); kept as a
		 * fail-safe so two bodies can never share one bed. */
		server_local_actor_hud_message(s, actor, "This bed is occupied");
		return;
	}

    // Beta EntityPlayer.sleepInBedAt: you may only sleep at NIGHT. If it is daytime
    // (World.isDaytime() == skylightSubtracted < 4) the bed refuses (NOT_POSSIBLE_NOW)
    // and the world time is left unchanged. The old code had NO night gate (it let you
    // "sleep" by day) and skipped to 13000 = dusk -- both non-faithful.
    // [decompile 740c583 EntityPlayer.sleepInBedAt + World.isDaytime]
    // Weather counts: Beta's isDaytime() reads the same weather-darkened
    // skylightSubtracted, so a full daytime THUNDERSTORM (subtract 5 >= 4) reads as
    // night and lets you sleep, while ordinary rain (subtract 3) does not. That is
    // Beta's real rule, not an addition -- see daytime.h. The Nether never reaches
    // here (the explode branch above returns first), so the shared-struct frozen-
    // strength hazard does not apply.
    if (server_local_is_daytime(s)) { /* w2: World.isDaytime reads the cached value */
        // tile.bed.noSleep (BlockBed.java:75-77, NOT_POSSIBLE_NOW). Emitted only for
        // IN-REACH daytime clicks: the reach guard above returns first for far clicks.
        // Beta checks daytime before distance, but a client cannot send an out-of-reach
        // activate, so the ordering difference is unobservable (handoff P4(a) caveat).
        server_local_actor_hud_message(s, actor, "You can only sleep at night");
        return;
    }

    // P4(b): ENTER the sleeping state -- the night skip happens ~100 server ticks later
    // in server_local_sleep_tick, never here. Beta's sleepInBedAt only records state
    // (EntityPlayer.java:537-596); the old instant skip WAS the reported "clicking a
    // bed instantly turns it to day". The bed spawn is set on WAKE, not here (Beta sets
    // it in wakeUpPlayer(setSpawn=true), so a bed destroyed mid-sleep leaves the
    // previous spawn untouched). "This bed is occupied" (tile.bed.occupied) has no
    // reachable site in single player; with a split-screen guest it is the check above.
	server_local_begin_sleep_for(s, actor, head_x, head_y, head_z);
	/* BlockBed.blockActivated:70-72: sleepInBedAt == OK ->
	 * setBedOccupied(head, true). begin_sleep_for refuses silently for an
	 * absent guest body, so test the outcome rather than assume it. The
	 * wake (server_local_wake_up / its guest twin) clears it again. */
	if(actor->sleeping) {
		struct block_data live;
		if(server_world_get_block(w, head_x, head_y, head_z, &live)
		   && live.type == BLOCK_BED)
			server_world_set_block_metadata_with_notify(
				w, head_x, head_y, head_z,
				(uint8_t)(live.metadata | BED_META_OCCUPIED));
	}
}


struct block block_bed = {
	.name = "Bed",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onRandomTick = NULL,
	.onNeighbourBlockChange = onNeighbourBlockChange,
	.onRightClick = onRightClick,
	.transparent = false,
	.renderBlock = render_block_bed,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 1,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 300,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = block_place_default,
		.fuel = 0,
		.render_data.block.has_default = false,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
