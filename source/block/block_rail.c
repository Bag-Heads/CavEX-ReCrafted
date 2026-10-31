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

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "block_rail.h" // block_detector_rail_on_minecart_tick (CR-05 public entry point)
#include "blocks.h"
#include "redstone_server.h" // redstone_server_indirectly_getting_powered (the power query)

static void powered_rail_refresh(struct server_local* s, w_coord_t x, w_coord_t y,
                                 w_coord_t z);

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	// Beta BlockRail.setBlockBoundsBasedOnState (java:40-45) uses RAW metadata,
	// so a powered slope (10..13) has the flat 2/16 selection height.
	if(x)
		aabb_setsize(x, 1.0F,
					 (this->block->metadata > 1
					  && this->block->metadata < 6) ?
						 0.625F :
						 0.125F,
					 1.0F);
	return entity ? 0 : 1;
}

static inline bool isRail(uint8_t type) {
    return type == BLOCK_RAIL
        || type == BLOCK_POWERED_RAIL
        || type == BLOCK_DETECTOR_RAIL;
}

/* POWERED-RAIL BIT 8 (2026-07-30, handoff P1(a)). Powered and detector rails keep their
 * redstone state in metadata bit 0x8 and their SHAPE in the low 3 bits (0-5 only -- Beta
 * RailLogic assigns curve metas 6-9 exclusively to rails without the powered-rail flag;
 * isPoweredRail is the ctor flag, TRUE for the detector too). A plain rail has no bit 8:
 * its whole 4-bit metadata is shape 0-9.
 *
 * Until this fix NOTHING in the engine ever set bit 8, and two paths actively destroyed
 * it: metadata is a 4-BIT bitfield (blocks_data.h), so clearing all four bits before
 * combining the new shape lost every old bit -- including the powered flag
 * on every shape rewrite. Beta retains bit 8 and combines it with the new shape
 * (RailLogic.java:217-218, :338-339). We preserve it ONLY for the two powerable ids:
 * for a plain rail bit 8 of a curve meta (8 = NW, 9 = NE) is SHAPE, not state, and
 * OR-ing it back would corrupt the curve. */
static inline bool rail_is_powerable(uint8_t type) {
    return type == BLOCK_POWERED_RAIL || type == BLOCK_DETECTOR_RAIL;
}

static inline uint8_t rail_shape(struct block_data bd) {
    return rail_is_powerable(bd.type) ? (bd.metadata & 0x7) : (bd.metadata & 0xF);
}

static inline uint8_t rail_power_bit(struct block_data bd) {
    return rail_is_powerable(bd.type) ? (bd.metadata & 0x8) : 0;
}

static void updateRailToSlope(struct server_local* s, w_coord_t x, w_coord_t y, w_coord_t z, uint8_t newMeta) {
    struct block_data bd;
    if (!server_world_get_block(block_world(s), x, y, z, &bd) || !isRail(bd.type)) {
        return;
    }
    uint8_t oldShape = rail_shape(bd);
    if (oldShape >= 6 && oldShape <= 9) {
        return;
    }
    // Callers pass slope shapes 2-5. Preserve the powered flag: this used to be
    // clearing all four old metadata bits before combining the shape, which wiped bit 8.
    bd.metadata = rail_power_bit(bd) | (newMeta & 0x7);
    server_world_set_block(block_world(s), x, y, z, bd);
}



// compute a single metadata value 0–9:
//   2–5 = ascending slopes,
//   0–1 = straight NS/EW,
//   6–9 = curves,
//   2–5 also reused for descending slopes (mirrored)
// allow_curves: FALSE for powered/detector rails. Beta's RailLogic assigns every curve
// meta (6-9) only without the powered-rail flag (RailLogic.java:174/:248/:275, ctor flag
// true for the detector too), so a powerable rail can only ever hold shapes 0-5. This
// matters doubly here because 8 and 9 CARRY BIT 0x8: a curve meta handed to a powered
// rail renders as POWERED_ON with nothing powering it -- a false positive that would
// survive the bit-8 fix and mislead the next hardware session. Falling through to the
// straight tests is the faithful result.
static uint8_t calcRailMeta(struct server_local* s,
                            w_coord_t x, w_coord_t y, w_coord_t z,
                            bool allow_curves) {

	 struct block_data bd;

	// curve when this rail sits atop a diagonal slope
	bool lower_e = server_world_get_block(block_world(s), x+1, y-1, z, &bd) && isRail(bd.type);
	bool lower_w = server_world_get_block(block_world(s), x-1, y-1, z, &bd) && isRail(bd.type);
	bool lower_n = server_world_get_block(block_world(s), x,   y-1, z-1, &bd) && isRail(bd.type);
	bool lower_s = server_world_get_block(block_world(s), x,   y-1, z+1, &bd) && isRail(bd.type);

	// same‐level horizontal neighbours
	bool n = server_world_get_block(block_world(s), x,   y, z-1, &bd) && isRail(bd.type);
	bool s_ =server_world_get_block(block_world(s), x,   y, z+1, &bd) && isRail(bd.type);
	bool w = server_world_get_block(block_world(s), x-1, y, z,   &bd) && isRail(bd.type);
	bool e = server_world_get_block(block_world(s), x+1, y, z,   &bd) && isRail(bd.type);

	// if exactly one diagonal‐below slope + exactly one H‐neighbour on the perpendicular axis => curve
	if (allow_curves) {
		if (lower_e && (n ^ s_) && !w && !e) return n ? 9 : 6;  // NE or SE
		if (lower_w && (n ^ s_) && !w && !e) return n ? 8 : 7;  // NW or SW
		if (lower_n && (w ^ e) && !n && !s_) return e ? 9 : 8;  // NE or NW
		if (lower_s && (w ^ e) && !n && !s_) return e ? 6 : 7;  // SE or SW
	}

    // 1) slopes
    if (server_world_get_block(block_world(s), x+1,y+1,z, &bd) && isRail(bd.type)) return 2;
    if (server_world_get_block(block_world(s), x-1,y+1,z, &bd) && isRail(bd.type)) return 3;
    if (server_world_get_block(block_world(s), x,y+1,z-1, &bd) && isRail(bd.type)) return 4;
    if (server_world_get_block(block_world(s), x,y+1,z+1, &bd) && isRail(bd.type)) return 5;


    // 2) straight & curves
    n  = server_world_get_block(block_world(s), x,   y, z-1, &bd) && isRail(bd.type);
    s_ = server_world_get_block(block_world(s), x,   y, z+1, &bd) && isRail(bd.type);
    w  = server_world_get_block(block_world(s), x-1, y, z,   &bd) && isRail(bd.type);
    e  = server_world_get_block(block_world(s), x+1, y, z,   &bd) && isRail(bd.type);

    if ((n||s_) && !(w||e))      return 0; // NS
    if ((w||e) && !(n||s_))      return 1; // EW
    if (allow_curves) {
        if (s_ && e)             return 6; // SE
        if (s_ && w)             return 7; // SW
        if (n   && w)            return 8; // NW
        if (n   && e)            return 9; // NE
    }

    return 0;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex1(struct block_info* this, enum side side) {
	return (this->block->metadata < 6) ? tex_atlas_lookup(TEXAT_RAIL) :
										 tex_atlas_lookup(TEXAT_RAIL_CURVED);
}

static uint8_t getTextureIndex2(struct block_info* this, enum side side) {
	return (this->block->metadata & 0x8) ?
		tex_atlas_lookup(TEXAT_RAIL_POWERED_ON) :
		tex_atlas_lookup(TEXAT_RAIL_POWERED_OFF);
}

// The detector rail's SINGLE texture is CORRECT -- do not "symmetrise" it with the powered
// rail. Beta's -16 OFF swap requires the powered constructor flag AND
// the powered-rail block id (BlockRail.java:51-52), and
// Block.java:620 registers the detector with ONE sprite (195): 1.7.3 ships no second
// detector-rail texture. Worded precisely: the detector has NO SECOND TEXTURE -- it DOES
// have a powered state (bit 8, set while a cart sits on it; occupancy + power now land
// via detector_rail_update_state / block_detector_rail_on_minecart_tick below, CR-05) --
// that state changes redstone output, never this texture lookup.
static uint8_t getTextureIndex3(struct block_info* this, enum side side) {
	return tex_atlas_lookup(TEXAT_RAIL_DETECTOR);
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
                        struct block_info* where, struct block_info* on,
                        enum side on_side) {
    struct block_data below_blk;
    if (!server_world_get_block(block_world(s), where->x, where->y - 1, where->z, &below_blk)) {
        return false;
    }
    if (!blocks[below_blk.type] || blocks[below_blk.type]->can_see_through) {
        return false;
    }

    uint8_t meta = calcRailMeta(s, where->x, where->y, where->z,
                                !rail_is_powerable(it->id));
    bool lower_e = false, lower_w = false, lower_n = false, lower_s = false;
    struct block_data neighbor;
    if (server_world_get_block(block_world(s), where->x + 1, where->y - 1, where->z, &neighbor) && isRail(neighbor.type)) {
        lower_e = true;
    }
    if (server_world_get_block(block_world(s), where->x - 1, where->y - 1, where->z, &neighbor) && isRail(neighbor.type)) {
        lower_w = true;
    }
    if (server_world_get_block(block_world(s), where->x, where->y - 1, where->z + 1, &neighbor) && isRail(neighbor.type)) {
        lower_s = true;
    }
    if (server_world_get_block(block_world(s), where->x, where->y - 1, where->z - 1, &neighbor) && isRail(neighbor.type)) {
        lower_n = true;
    }

    if (meta < 2) {
        if ((lower_e || lower_w) && !(lower_n || lower_s)) {
            meta = 1;
        } else if ((lower_n || lower_s) && !(lower_e || lower_w)) {
            meta = 0;
        }
    }

    if (lower_e) {
        updateRailToSlope(s, where->x + 1, where->y - 1, where->z, 3);
    }
    if (lower_w) {
        updateRailToSlope(s, where->x - 1, where->y - 1, where->z, 2);
    }
    if (lower_s) {
        updateRailToSlope(s, where->x, where->y - 1, where->z + 1, 4);
    }
    if (lower_n) {
        updateRailToSlope(s, where->x, where->y - 1, where->z - 1, 5);
    }

    struct block_data new_blk = {
        .type = it->id,
        .metadata = meta,
        .sky_light = 0,
        .torch_light = 0
    };
    server_world_set_block(block_world(s), where->x, where->y, where->z, new_blk);
    // Beta refreshes power from onBlockAdded, which this engine's set_block does not
    // model (the item_redstone precedent: run the self-update explicitly after the set).
    // Without this, a powered rail placed beside an ALREADY-lit torch stays dark until
    // some other neighbour change happens by.
    if (it->id == BLOCK_POWERED_RAIL)
        powered_rail_refresh(s, where->x, where->y, where->z);
    return true;
}

/* THE POWERED-RAIL CHAIN WALK -- a line-for-line port of Beta's two mutually recursive
   privates, verified verbatim at 740c583 this session:
     BlockRail.isNeighborRailPowered (BlockRail.java:148-219)  -> rail_neighbour_powered
     BlockRail.isRailPassingPower    (BlockRail.java:221-244)  -> rail_passing_power
   The walk advances ALONG the track (direction `dir`) using each rail's shape, so it never
   ping-pongs; depth caps at 8 (:149-151), which with the directly-powered rail itself gives
   Beta's classic 9-rail powered run per direction. A neighbour passes power only when it is
   a powered rail whose SHAPE CONNECTS toward us, whose bit 8 is ALREADY set, and which is
   either directly powered or (recursively) fed by one that is. */
static bool rail_passing_power(struct server_local* s, w_coord_t x, w_coord_t y,
                               w_coord_t z, bool dir, int depth,
                               uint8_t target_shape);

static bool rail_neighbour_powered(struct server_local* s, w_coord_t x, w_coord_t y,
                                   w_coord_t z, uint8_t meta, bool dir, int depth) {
    if (depth >= 8)
        return false;
    uint8_t shape = meta & 0x7;
    bool flat = true; // var9: may we also probe one cell DOWN (descending-slope seam)?
    switch (shape) {
    case 0: if (dir) z++; else z--; break;
    case 1: if (dir) x--; else x++; break;
    case 2: if (dir) { x--; } else { x++; y++; flat = false; } shape = 1; break;
    case 3: if (dir) { x--; y++; flat = false; } else { x++; } shape = 1; break;
    case 4: if (dir) { z++; } else { z--; y++; flat = false; } shape = 0; break;
    case 5: if (dir) { z++; y++; flat = false; } else { z--; } shape = 0; break;
    default:
        // Shapes 6-9 are unreachable on a powered rail (calcRailMeta clamps curves away,
        // exactly as Beta's RailLogic never assigns them). Beta's switch would fall
        // through and probe SELF; refusing is the safe answer to an impossible input.
        return false;
    }
    if (rail_passing_power(s, x, y, z, dir, depth, shape))
        return true;
    return flat && rail_passing_power(s, x, y - 1, z, dir, depth, shape);
}

static bool rail_passing_power(struct server_local* s, w_coord_t x, w_coord_t y,
                               w_coord_t z, bool dir, int depth,
                               uint8_t target_shape) {
    struct block_data bd;
    if (!server_world_get_block(block_world(s), x, y, z, &bd)
        || bd.type != BLOCK_POWERED_RAIL)
        return false;
    uint8_t s2 = bd.metadata & 0x7;
    // Orientation gate (BlockRail.java:225-232): an EW probe cannot take power from a
    // NS-ish rail and vice versa.
    if (target_shape == 1 && (s2 == 0 || s2 == 4 || s2 == 5))
        return false;
    if (target_shape == 0 && (s2 == 1 || s2 == 2 || s2 == 3))
        return false;
    if (!(bd.metadata & 0x8))
        return false;
    if (redstone_server_indirectly_getting_powered(s, x, y, z)
        || redstone_server_indirectly_getting_powered(s, x, y + 1, z))
        return true;
    return rail_neighbour_powered(s, x, y, z, bd.metadata, dir, depth + 1);
}

/* Beta BlockRail.onNeighborBlockChange's railPowered branch (BlockRail.java:117-134),
   server-side (the local-authority gate selects this authoritative handler). Power =
   directly powered at the cell or the cell above, OR fed by the chain walk in either
   direction. Writes happen ONLY on a state change (var10), which is what terminates the
   propagation cascade.

   NOTIFY SEMANTICS, verified against World.java:422-432: Beta's setBlockMetadataWithNotify
   fans all six neighbours for rails (all three set the misleadingly-named
   disableNeighborNotifyOnMetadataChange flag, which ENABLES it) -- our
   server_world_set_block does the same. Beta then ADDITIONALLY fans the BELOW cell's
   neighbours, and the ABOVE cell's for slope shapes 2-5 (:129-134). Those extra fans are
   NOT optional: a sloped rail's track continuation sits DIAGONALLY (x±1, y±1), which is
   not one of the rail's own six neighbours -- without them a powered chain stops dead at
   every slope. notifyNeighbours is the paced, depth-capped dispatcher from the 2026-06-27
   redstone-freeze fix, so this rides the same cascade budget as every other notify. */
static void powered_rail_refresh(struct server_local* s, w_coord_t x, w_coord_t y,
                                 w_coord_t z) {
    struct block_data cur;
    if (!server_world_get_block(block_world(s), x, y, z, &cur)
        || cur.type != BLOCK_POWERED_RAIL)
        return;
    uint8_t meta = cur.metadata;
    uint8_t shape = meta & 0x7;
    bool powered = redstone_server_indirectly_getting_powered(s, x, y, z)
        || redstone_server_indirectly_getting_powered(s, x, y + 1, z)
        || rail_neighbour_powered(s, x, y, z, meta, true, 0)
        || rail_neighbour_powered(s, x, y, z, meta, false, 0);
    bool changed = false;
    if (powered && !(meta & 0x8)) {
        cur.metadata = shape | 0x8;
        changed = true;
    } else if (!powered && (meta & 0x8)) {
        cur.metadata = shape; // Beta extracts the low three shape bits (BlockRail.java:90, written at :125).
        changed = true;
    }
    if (changed) {
        server_world_set_block(block_world(s), x, y, z, cur);
        notifyNeighbours(s, x, y - 1, z);
        if (shape >= 2 && shape <= 5)
            notifyNeighbours(s, x, y + 1, z);
    }
}

// Beta BlockRail.onNeighborBlockChange support-drop head (BlockRail.java
// ~86-104), ported faithfully BEFORE the existing shape/power refresh:
// Start from the metadata shape, masking to three bits for powerable rails.
// rail_shape already implements that mask.
// Lack of a normal supporting cube below marks the rail for removal.
// An ascending east slope also requires a normal cube to the east;
// an ascending west slope requires one to the west;
// an ascending north slope requires one to the north;
// an ascending south slope requires one to the south.
// On missing support, drop the item and replace the rail with air and notify.
// Only a supported rail proceeds to the existing shape/power refresh.
// Beta's if/else-if means a drop here SKIPS the shape/power refresh entirely
// for this call -- ported as an early return, matching block_ladder.c's own
// support-drop shape (spawn drops, then set air).
static bool rail_unsupported(struct server_local* s, w_coord_t x, w_coord_t y,
                             w_coord_t z, uint8_t shape) {
    struct block_data nb;
    if (!server_world_get_block(block_world(s), x, y - 1, z, &nb)
        || !block_is_normal_cube(nb.type))
        return true;

    w_coord_t sx = x, sz = z;
    switch (shape) {
        case 2: sx += 1; break; // ascending +X
        case 3: sx -= 1; break; // ascending -X
        case 4: sz -= 1; break; // ascending -Z
        case 5: sz += 1; break; // ascending +Z
        default: return false;  // not an ascending slope: below-check only
    }
    return !server_world_get_block(block_world(s), sx, y, sz, &nb)
        || !block_is_normal_cube(nb.type);
}

static void onNeighbourBlockChange(struct server_local* s,
                                   struct block_info* info) {
    struct block_data cur;
    if (!server_world_get_block(block_world(s), info->x, info->y, info->z, &cur)
        || !isRail(cur.type))
        return;

    if (rail_unsupported(s, info->x, info->y, info->z, rail_shape(cur))) {
        struct block_info drop_info = *info;
        drop_info.block = &cur;
        server_local_spawn_block_drops(s, &drop_info);
        server_world_set_block(block_world(s), info->x, info->y, info->z,
                               (struct block_data) {.type = BLOCK_AIR,
                                                     .metadata = 0});
        return;
    }

    bool powerable = rail_is_powerable(cur.type);
    // Compare SHAPES, not raw metadata: with bit 8 set, four-bit metadata (8..13) never equals
    // a 0-5 shape, so the old comparison always "changed" -- and the old write wiped the
    // bit (clearing all four bits of the field). Both halves of the reported defect.
    uint8_t oldShape = rail_shape(cur);
    uint8_t newShape = calcRailMeta(s, info->x, info->y, info->z,
                                    !powerable) & 0xF;
    if (newShape != oldShape) {
        cur.metadata = rail_power_bit(cur) | newShape;
        server_world_set_block(block_world(s), info->x, info->y, info->z, cur);
    }

    // CR-05 CLOSED: the DETECTOR rail as a power SOURCE (its bit 8 set while a minecart
    // sits on it) is now implemented -- detector_rail_update_state/
    // block_detector_rail_on_minecart_tick/detector_rail_world_tick below, driven from
    // entity_minecart.c's minecart_server_tick and this block's onWorldTick. A detector
    // -> powered-rail circuit therefore works end to end: this powered-rail refresh below
    // already reads the detector's bit through redstone_power.c's isPoweringTo/isIndirectly-
    // PoweringTo dispatch (CR-05), so nothing further is needed here.
    if (cur.type == BLOCK_POWERED_RAIL)
        powered_rail_refresh(s, info->x, info->y, info->z);
}


struct block block_rail = {
	.name = "Rail",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex1,
	.getDroppedItem = block_drop_default,
	.onNeighbourBlockChange = onNeighbourBlockChange,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_rail,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = true,
	.can_see_through = true,
	.opacity = 0,
	.render_block_data.rail_curved_possible = true,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 1050,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_flat,
		.onItemPlace = onItemPlace,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

struct block block_powered_rail = {
	.name = "Powered rail",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex2,
	.getDroppedItem = block_drop_default,
	.onNeighbourBlockChange = onNeighbourBlockChange,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_rail,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = true,
	.can_see_through = true,
	.opacity = 0,
	.render_block_data.rail_curved_possible = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 1050,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_flat,
		.onItemPlace = onItemPlace,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

/* CR-05 -- BlockDetectorRail (740c583) occupancy + redstone state machine.
 *
 * Beta's detection box (setStateIfMinecartInteractsWithRail):
 * The box begins 0.125 inside each horizontal face of the cell, at cell-floor height,
 * and ends 0.25 above the floor.
 * It is 0.75 wide/deep (0.125 margin each side of X/Z),
 * 0.25 tall starting AT the cell floor; the existing AABB helper makes these dimensions
 * and translates them to the cell origin. Any minecart in this box counts as present.
 *
 * Entity query semantics (World.getEntitiesWithinAABB -> Chunk.getEntitiesWithinAABB-
 * ForEntity): test overlap with each entity bounding box. AxisAlignedBB.intersectsWith
 * requires strict overlap (a shared face/edge is NOT an intersection) --
 * aabb_intersection_strict already implements that method
 * (aabb.c), and entity_make_world_bbox already reproduces Entity.setPosition
 * boundingBox maintenance, so both halves of the query are existing, proven
 * primitives; nothing new is invented here. */
static bool detector_rail_cart_present(struct server_local* s, w_coord_t x, w_coord_t y,
                                       w_coord_t z) {
    struct AABB box;
    aabb_setsize(&box, 0.75F, 0.25F, 0.75F);
    aabb_translate(&box, (float)x, (float)y, (float)z);

    dict_entity_it_t it;
    dict_entity_it(it, s->entities);
    while(!dict_entity_end_p(it)) {
        struct entity* e = dict_entity_ref(it)->value;
        dict_entity_next(it);
        // Same live-entity filter as every other entities-in-AABB scan in this engine
        // (mob_spawn_entity_clear, the piston pusher): a zero delay_destroy means already
        // marked dead, not a real Beta occupant.
        if(!e || e->delay_destroy == 0 || e->type != ENTITY_MINECART)
            continue;
        struct AABB eb;
        entity_make_world_bbox(e, &eb);
        if(aabb_intersection_strict(&box, &eb))
            return true;
    }
    return false;
}

/* Beta BlockDetectorRail.setStateIfMinecartInteractsWithRail, the ONE private both
 * public hooks (onEntityCollidedWithBlock, updateTick) call:
 * Read the previous powered state from metadata bit 8.
 * Query whether a cart is currently in the detection box.
 * An arriving cart sets bit 8, then notifies this cell and the cell below.
 * A departing cart clears bit 8, then notifies this cell and the cell below.
 * Schedule another check in 20 ticks only while a cart is present.
 * Both notify calls pass an EXPLICIT source id (this.blockID), unlike the shared
 * 6-neighbour fan server_world_set_block already performs implicitly (Beta
 * setBlockMetadataWithNotify also fires the implicit fan first -- the explicit
 * notification of this cell is a faithfully-preserved REDUNDANT second call, not a
 * bug: preserved rather than cleaned up against the decompile). */
static void detector_rail_update_state(struct server_local* s, w_coord_t x, w_coord_t y,
                                       w_coord_t z) {
    struct block_data cur;
    if (!server_world_get_block(block_world(s), x, y, z, &cur)
        || cur.type != BLOCK_DETECTOR_RAIL)
        return;

    bool was_powered = (cur.metadata & 0x8) != 0;
    bool present = detector_rail_cart_present(s, x, y, z);

    if ((present && !was_powered) || (!present && was_powered)) {
        cur.metadata = present ? (cur.metadata | 0x8) : (cur.metadata & 0x7);
        server_world_set_block(block_world(s), x, y, z, cur);
        notifyNeighbours_from(s, x, y, z, BLOCK_DETECTOR_RAIL);
        notifyNeighbours_from(s, x, y - 1, z, BLOCK_DETECTOR_RAIL);
    }

    // Beta only reschedules while a cart remains, so the recheck chain
    // self-terminates the instant the cell empties -- the NEXT occupancy is picked up
    // by onEntityCollidedWithBlock (block_detector_rail_on_minecart_tick), not by a
    // dangling scheduled tick.
    if (present)
        server_world_schedule_block_tick(block_world(s), x, y, z,
                                         BLOCK_DETECTOR_RAIL);
}

// Public entry point (block_rail.h): Beta's onEntityCollidedWithBlock outer gate.
void block_detector_rail_on_minecart_tick(struct server_local* s, w_coord_t x,
                                          w_coord_t y, w_coord_t z) {
    struct block_data cur;
    if (!server_world_get_block(block_world(s), x, y, z, &cur)
        || cur.type != BLOCK_DETECTOR_RAIL || (cur.metadata & 0x8))
        return; // Beta calls the state update on collision only when bit 8 is clear.
    detector_rail_update_state(s, x, y, z);
}

// The scheduled 20-tick recheck (Beta updateTick). Guarded by bit 8 for exact fidelity
// even though, by construction here, onWorldTick is only ever (re)scheduled while the
// bit is already set. info->neighbours is populated (BLOCK_DETECTOR_RAIL is in the
// server_world_block_needs_neighbours allowlist) but unused -- this handler only needs
// the world accessor and the entity dict, like powered_rail_refresh above it.
static void detector_rail_world_tick(struct server_local* s, struct block_info* info) {
    if (!(info->block->metadata & 0x8))
        return; // Beta calls the scheduled state update only when bit 8 is set.
    detector_rail_update_state(s, info->x, info->y, info->z);
}

struct block block_detector_rail = {
	.name = "Detector rail",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex3,
	.getDroppedItem = block_drop_default,
	.onNeighbourBlockChange = onNeighbourBlockChange,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_rail,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = true,
	.can_see_through = true,
	.opacity = 0,
	.render_block_data.rail_curved_possible = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.onWorldTick = detector_rail_world_tick,
	.tick_delay = 20, // BlockDetectorRail.tickRate returns 20.
	.digging.hardness = 1050,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_flat,
		.onItemPlace = onItemPlace,
		.fuel = 0,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
