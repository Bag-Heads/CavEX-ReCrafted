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

#include "../network/server_local.h"
#include "../network/server_world.h"
#include "blocks.h"
#include "redstone_server.h"
#include "../particle.h"
#include "../game/game_state.h"
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)


static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_STONE;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
                             struct AABB* x) {
    if (x) {
        // Beta [740c583 BlockRedstoneWire.java:14]:
        //     this.setBlockBounds(0.0F, 0.0F, 0.0F, 1.0F, 0.0625F, 1.0F);
        // (was the fork-inherited 0.125F -- FH-15: box twice Beta's height)
        aabb_setsize(x,
                     1.0F,
                     0.0625F,
                     1.0F);
    }
    return entity ? 0 : 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_empty();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	uint8_t lvl = this->block->metadata & 0x0F;
	if (lvl == 0) {
		return tex_atlas_lookup(TEXAT_REDSTONE_WIRE_OFF);
	}
	return tex_atlas_lookup(TEXAT_REDSTONE_WIRE_L1 + (lvl - 1));
}

// Beta wire is SYNCHRONOUS: it recomputes on a neighbour change (not a scheduled crawl). The
// faithful flood lives in redstone_logic.c (redstone_wire_update) and is wired to the live
// world by redstone_server.c. The old 1-cell/tick onWorldTick crawl is gone -- it advanced one
// hop per game-tick (~15 ticks to settle a 15-long run) and was not Beta-faithful.

static size_t getDroppedItem(struct block_info* this,
                             struct item_data* it,
                             struct java_random* g,
                             struct server_local* s)
{
    if (it) {
        it->id = ITEM_REDSTONE;
        it->durability = 0;
        it->count = 1;
    }
    return 1;
}

// Beta BlockRedstoneWire.onNeighborBlockChange (src_original BlockRedstoneWire.java:268-279):
// FIRST canPlaceBlockAt (:41-43, isBlockNormalCube below); if it fails, dropBlockAsItem(meta) +
// setBlockWithNotify(0) -- the wire's onBlockRemoved (redstone_wire_removal) then re-floods the
// cut line -- and NO self-flood. Otherwise updateAndPropagateCurrentStrength, i.e. the unchanged
// flood entry. onBlockAdded (:185-188) has no support check, so it keeps the plain entry.
static void onNeighbourBlockChange(struct server_local* s,
								   struct block_info* info) {
	if(server_world_simulation_stopped(block_world(s)))
		return;
	struct block_data below;
	if(!(server_world_get_block(block_world(s), info->x, info->y - 1, info->z,
								&below)
		 && block_is_normal_cube(below.type))) {
		server_local_spawn_block_drops(s, info); // dropBlockAsItem(meta)
		server_world_set_block_plain(block_world(s), info->x, info->y, info->z,
									 BLOCK_AIR); // setBlockWithNotify(x,y,z,0)
		return;
	}
	redstone_wire_neighbour_change(s, info);
}

struct block block_redstone_wire = {
	.name = "Redstone wire",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onBlockRemoval = redstone_wire_removal,
	.onRandomTick = NULL,
	.onRightClick = NULL,
	.onWorldTick = NULL, // wire is synchronous (onNeighbourBlockChange), not a scheduled tick
	/* Beta puts the placed wire's SELF-flood in onBlockAdded
	 * (updateAndPropagateCurrentStrength at the placed cell [740c583
	 * BlockRedstoneWire.onBlockAdded:185-188]) -- so it fires for ANY
	 * setBlock creating wire, not just the player item path. This port
	 * carried the flood only in the item hooks (redstone_wire_place /
	 * item_redstone), which left a wire installed by a raw
	 * server_world_set_block DARK next to a powered neighbour whose own
	 * value did not change (the rs2 oracle scenario's x=10 cell -- found
	 * 2026-08-26 by the torch-inverter capture; the neighbour-notify flood
	 * only propagates on CHANGE, so an unchanged donor never seeds the new
	 * cell). Registering the same neighbour_change entry here is exactly
	 * Beta's structure; the item hooks' explicit floods become idempotent
	 * doubles (same fixed point) kept for their settled-notify ordering. */
	.onBlockAdded = redstone_wire_neighbour_change,
	.onNeighbourBlockChange = onNeighbourBlockChange, // support-drop, then the flood
	.transparent = false,
	.renderBlock = render_block_redstone_wire,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = true,
	.can_see_through = true,
	.opacity = 0,
	// Wire is NOT a rail: render_block_redstone_wire picks cross/line from the wire's
	// connections, never the rail curve-rotation. (The old renderer abused this flag and
	// rotated the dust at power levels 6/7/8 -- metadata is the POWER level, not a rail shape.)
	.render_block_data.rail_curved_possible = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	// FH-14 (docs/fork_hack_audit_2026-08-21.md:166) -- fork-inherited 1050 (a
	// real multi-second dig time) replaced with Beta's instant-break value.
	// [740c583 Block.java:647]:
	//   redstoneWire = (new BlockRedstoneWire(55, 164)).setHardness(0.0F)...
	// [740c583 Block.java:327-332 blockStrength]:
	//   blockHardness < 0 -> 0.0F (unbreakable); else
	//   !canHarvestBlock ? 1.0F/hardness/100.0F : strVsBlock/hardness/30.0F.
	// hardness==0.0F divides by zero (Java float -> +Infinity, no exception),
	// so accumulated damage always clears 1.0F on the very first tick of
	// digging -- Beta's "instant break" is really "breaks after exactly one
	// game tick", not a literal zero-time removal.
	//
	// This port's OWN encoding of a Beta hardness-0.0F block is inconsistent
	// (see block_locked_chest.c's identical note): block_repeater.c stores
	// the LITERAL 0, while the majority -- torch/tallgrass/sapling/flower/
	// mushroom/tnt/fire/crops/reed/rose/locked_chest -- store a nominal 50.
	// Checked source/item/tool.c before picking one: tool_dig_delay_ms()
	// collapses ANY hardness<=0 (the literal-0 encoding included) to a
	// single "-1" sentinel -- the SAME sentinel block_bedrock.c/
	// block_portal.c/block_piston.c use for genuinely unbreakable
	// (hardness==-1). Both consumers of that return value gate on it being
	// positive: screen_ingame.c's dig-completion check is
	// `delay > 0 && elapsed >= delay` and render_block.c's crack-overlay is
	// `b->digging.hardness > 0` -- neither ever fires for -1. A literal 0
	// therefore does NOT complete a hold-to-dig in this engine at all; it is
	// indistinguishable from truly unbreakable. (block_repeater.c's "instant
	// break" comment on its literal 0 is consequently misleading under the
	// port's own tool.c -- a pre-existing gap, out of FH-14's scope, flagged
	// separately rather than silently touched here.)
	// The nominal-50 encoding is the one that actually reproduces Beta's
	// "clears on the first tick" behaviour through this engine's real
	// mechanism: hardness=50 -> tool_dig_delay_ms returns exactly 50ms (one
	// 20-TPS tick, `(50+49)/50*50`) for ANY held item, so the dig-completion
	// gate fires almost immediately and the crack overlay is skipped for all
	// but a single frame or two -- i.e. perceptibly instant, exactly like
	// every other ported Beta hardness-0.0F block. Following that (majority,
	// actually-functional) precedent here, not repeater's outlier literal 0.
	.digging.hardness = 50,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_flat,
		.onItemPlace = redstone_wire_place, // place + self-flood (Beta onBlockAdded)
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
