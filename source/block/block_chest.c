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

#include <math.h>

#include "../cglm/cglm.h" // glm_rad (onItemPlace facing needs degrees->radians); cglm.h is the block stratum's frozen cglm edge (check_header_hygiene.py)
#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/client_interface.h"
#include "../network/inventory_logic.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "blocks.h"
#include "chest_link.h"

// Is the block at (x,y,z) in world `w` a chest? False if the chunk isn't
// loaded. C2 tiles (Fable F-A1): pairing reads the ACTOR's world, never slot 0
// -- a Nether actor's chest pairs by Nether neighbours.
static bool cell_is_chest(struct server_world* w, w_coord_t x, w_coord_t y,
						  w_coord_t z) {
	struct block_data b;
	return w && server_world_get_block(w, x, y, z, &b)
		&& b.type == BLOCK_CHEST;
}

// Does the chest at (x,y,z) already have a horizontally-adjacent chest, i.e.
// is it already half of a double chest?
static bool chest_already_paired(struct server_world* w, w_coord_t x,
								 w_coord_t y, w_coord_t z) {
	for(int i = 0; i < 4; i++)
		if(cell_is_chest(w, x + CHEST_LINK_DX[i], y, z + CHEST_LINK_DZ[i]))
			return true;
	return false;
}

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_WOOD;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x)
		aabb_setsize(x, 1.0F, 1.0F, 1.0F);
	return 1;
}

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
	return face_occlusion_full();
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	uint8_t tex[SIDE_MAX] = {
		[SIDE_TOP] = tex_atlas_lookup(TEXAT_CHEST_TOP),
		[SIDE_BOTTOM] = tex_atlas_lookup(TEXAT_CHEST_TOP),
		[SIDE_BACK] = tex_atlas_lookup(TEXAT_CHEST_SIDE),
		[SIDE_FRONT] = tex_atlas_lookup(TEXAT_CHEST_SIDE),
		[SIDE_LEFT] = tex_atlas_lookup(TEXAT_CHEST_SIDE),
		[SIDE_RIGHT] = tex_atlas_lookup(TEXAT_CHEST_SIDE),
	};

	// Facing (latch) side from metadata, plus the back side and the two
	// horizontal sides perpendicular to the facing axis. A Beta double chest's
	// two halves face the same way and sit side by side along that perpendicular
	// axis, so a chest neighbour on a perpendicular side means we are a double.
	enum side front, back, perp_neg, perp_pos;
	switch(this->block->metadata) {
		default:
		case 0: front = SIDE_FRONT; back = SIDE_BACK;
				perp_neg = SIDE_LEFT; perp_pos = SIDE_RIGHT; break; // faces -Z
		case 2: front = SIDE_BACK; back = SIDE_FRONT;
				perp_neg = SIDE_LEFT; perp_pos = SIDE_RIGHT; break; // faces +Z
		case 1: front = SIDE_RIGHT; back = SIDE_LEFT;
				perp_neg = SIDE_FRONT; perp_pos = SIDE_BACK; break; // faces +X
		case 3: front = SIDE_LEFT; back = SIDE_RIGHT;
				perp_neg = SIDE_FRONT; perp_pos = SIDE_BACK; break; // faces -X
	}

	// neighbours is the real 6-neighbour grid during world meshing; for item
	// rendering it is 6x AIR (or NULL via render_item_flat) -> renders single.
	bool dbl_pos
		= this->neighbours && this->neighbours[perp_pos].type == BLOCK_CHEST;
	bool dbl_neg
		= this->neighbours && this->neighbours[perp_neg].type == BLOCK_CHEST;

	// The double-chest front graphic is split across the two blocks with the
	// latch at the shared seam (FRONT_1 carries the latch on its texture-right
	// edge, FRONT_2 on its texture-left). Whether a given half lands the latch
	// on the seam depends on the front face's texture-U direction, which is
	// mirrored between facings: on the -Z (SIDE_FRONT) and +X (SIDE_RIGHT)
	// fronts the world +axis maps to texture-left, while on the +Z (SIDE_BACK)
	// and -X (SIDE_LEFT) fronts it maps to texture-right. So metadata 0 and 1
	// need the halves assigned opposite to metadata 2 and 3.
	bool flip_halves
		= (this->block->metadata == 0 || this->block->metadata == 1);

	if(dbl_pos) {
		// partner toward the +axis
		tex[front] = tex_atlas_lookup(flip_halves ? TEXAT_CHEST_FRONT_2
												  : TEXAT_CHEST_FRONT_1);
		tex[back] = tex_atlas_lookup(flip_halves ? TEXAT_CHEST_BACK_1
												 : TEXAT_CHEST_BACK_2);
	} else if(dbl_neg) {
		// partner toward the -axis
		tex[front] = tex_atlas_lookup(flip_halves ? TEXAT_CHEST_FRONT_1
												  : TEXAT_CHEST_FRONT_2);
		tex[back] = tex_atlas_lookup(flip_halves ? TEXAT_CHEST_BACK_2
												 : TEXAT_CHEST_BACK_1);
	} else {
		// single chest
		tex[front] = tex_atlas_lookup(TEXAT_CHEST_FRONT_SINGLE);
	}

 	return tex[side];
}

static void onRightClick(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* where, struct block_info* on,
						 enum side on_side) {
	if(actor->active_inventory != &actor->inventory)
		return;
	/* C2 tiles: BlockChest.blockActivated (BlockChest.java:199-211) opens the
	 * chest of the world the actor clicked in. */
	struct server_world* w = server_local_world_of(s, actor);
	if(!w)
		return;

	// The chest being opened is the clicked block (on), not the adjacent cell.
	w_coord_t cx = on->x, cy = on->y, cz = on->z;

	bool nb_chest[4];
	for(int i = 0; i < 4; i++)
		nb_chest[i]
			= cell_is_chest(w, cx + CHEST_LINK_DX[i], cy, cz + CHEST_LINK_DZ[i]);

	int pidx = chest_partner_index(nb_chest);

	if(pidx < 0) {
		server_local_open_inventory_in(s, w, actor, &inventory_logic_chest,
										CHEST_SIZE, cx, cy, cz,
										WINDOWC_CHEST, WINDOW_TYPE_CHEST);
		return;
	}

	// double chest: 54-slot window spanning both halves.
	//
	// BETA USES ONE GUI FOR BOTH CHEST SIZES (2026-07-29, handoff P2(f)).
	// BlockChest.blockActivated:199-211 wraps the pair in an InventoryLargeChest and hands
	// it to the SAME displayGUIChest; GuiChest.java:17 then derives
	// `inventoryRows = lowerChestInventory.getSizeInventory() / 9`, so 54 storage slots
	// simply draw six rows of the same container.png. This used to send
	// WINDOW_TYPE_IRON_CHEST, i.e. the FORK's iron-chest screen, which carries its own
	// "HACK: hardcoded slot counts to awkwardly fit a double chest in 320x200"
	// (screen_iron_chest.c) and whose gui/iron_chest.png is fork-only -- so a player using
	// a real Beta pack silently got fork art for every double chest.
	//
	// ⚠ ONLY THE WIRE TYPE CHANGES. The routing INDEX stays WINDOWC_IRON_CHEST on purpose:
	// it is written into persisted death_drop_provenance records
	// (inventory_double_chest.c, death_drop_journal.h `window` byte), so renumbering it
	// would silently invalidate every existing death-drop journal. The index only selects
	// a gstate.players[0].windows[] slot; the TYPE is what picks the client screen
	// (client_interface.c CRPC_OPEN_WINDOW), and screen_chest takes its container id from
	// screen_chest_set_windowc, so it works from slot 4 exactly as from slot 3.
	//
	// Assign primary/secondary canonically so both halves agree on slot order.
	w_coord_t px = cx + CHEST_LINK_DX[pidx];
	w_coord_t py = cy;
	w_coord_t pz = cz + CHEST_LINK_DZ[pidx];

	w_coord_t ax, ay, az, bx, by, bz;
	if(chest_is_primary(cx, cz, px, pz)) {
		ax = cx, ay = cy, az = cz;
		bx = px, by = py, bz = pz;
	} else {
		ax = px, ay = py, az = pz;
		bx = cx, by = cy, bz = cz;
	}

	server_local_open_linked_inventory_in(
		s, w, actor, &inventory_logic_double_chest, IRON_CHEST_SIZE, ax, ay, az,
		bx, by, bz, WINDOWC_IRON_CHEST, WINDOW_TYPE_CHEST);
}

/* Does a live window own this world's chest half at (x,y,z)? Shared chest
 * views (either actor) answer first; otherwise any present body's window
 * bound to the removal world (C2 tiles, row #12). Removals run in world
 * slot 0 (block callbacks carry no world). */
static bool live_window_owns_chest(struct server_local* s, w_coord_t x,
								   w_coord_t y, w_coord_t z) {
	if(server_local_has_chest_view(s, x, y, z)) return true;
	for(unsigned i = 0; i < 2; i++) {
		struct server_player* body = server_local_damage_candidate(s, i);
		struct inventory* inv = body ? body->active_inventory : NULL;
		if(!inv || inv == &body->inventory)
			continue;
		/* A1b: a window bound to another world views a different chest. */
		if(!server_local_window_bound_to(inv, block_world(s)))
			continue;
		if(inv->logic == &inventory_logic_chest
		   && !(inv->orphaned_chest_halves & 1)
		   && inv->x == x && inv->y == y && inv->z == z)
			return true;
		if(inv->logic == &inventory_logic_double_chest
		   && ((!(inv->orphaned_chest_halves & 1) && inv->x == x && inv->y == y
				&& inv->z == z)
			   || (!(inv->orphaned_chest_halves & 2) && inv->x2 == x
				   && inv->y2 == y && inv->z2 == z)))
			return true;
	}
	return false;
}

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	// player.rx is yaw in DEGREES (Beta rotationYaw convention); sinf/cosf need
	// RADIANS. Convert with the SAME sign convention as every other look-vector
	// derivation in this codebase (server_local.c:2199, 8768, 8848, 12805:
	// crx = glm_rad(-actor->rx)) so lx/lz land on the correct cardinal
	// quadrant. Cross-checked against Beta's real yaw-quadrant classifier --
	// MathHelper.floor_double(rotationYaw*4/360+0.5)&3, e.g.
	// BlockFurnace.onBlockPlacedBy (furnace_facing_meta) and
	// BlockPistonBase.determineOrientation (piston_determine_orientation) --
	// at all 4 cardinal yaws (0/90/180/270): with this sign convention the
	// lx/lz branches below select the identical facing the integer formula
	// gives. (BlockChest itself has NO onBlockPlacedBy override in the 1.7.3
	// decompile -- Block's default is a no-op, so Beta's chest "facing" is
	// actually a render-time neighbour-opacity auto-orientation, not a stored
	// yaw-derived metadata. CaveWii's chest already stores an explicit facing
	// meta -- reused for double-chest pairing consistency at line ~282-293 --
	// so this fix corrects the existing yaw-based design to match Beta's
	// quadrant math rather than reintroducing Beta's auto-orient scheme,
	// which is out of scope for this bug fix.)
	float ang = glm_rad(-actor->rx);
	float lx = sinf(ang), lz = cosf(ang);
	/* C2 tiles (F-A1): the placing actor's world for the neighbour scan, the
	 * row claim, the block write and the partner-facing fix. */
	struct server_world* w = server_local_world_of(s, actor);
	if(!w)
		return false;

	// Neighbour scan (CHEST_LINK idx 0=-X, 1=+X, 2=-Z, 3=+Z).
	bool nb_chest[4], nb_paired[4];
	int partner = -1, n_chest = 0;
	for(int i = 0; i < 4; i++) {
		w_coord_t nx = where->x + CHEST_LINK_DX[i];
		w_coord_t nz = where->z + CHEST_LINK_DZ[i];
		nb_chest[i] = cell_is_chest(w, nx, where->y, nz);
		nb_paired[i] = nb_chest[i] && chest_already_paired(w, nx, where->y, nz);
		if(nb_chest[i]) {
			n_chest++;
			partner = i;
		}
	}

	// Beta no-triple rule (BlockChest.canPlaceBlockAt): a chest belongs to at
	// most one pair, so reject a placement that would cluster.
	if(!chest_can_place(nb_chest, nb_paired))
		return false;

	// Facing (getTextureIndex front metadata: 0=-Z, 1=+X, 2=+Z, 3=-X). A single
	// chest faces the placer. A double's two halves BOTH face perpendicular to
	// the axis joining them, toward the placer — matching Beta, and required so
	// getTextureIndex detects the pair (it looks for a chest on the axis
	// perpendicular to the facing). Without this, two adjacent chests placed at
	// different yaws got mismatched facings and rendered as a broken double.
	uint8_t facing;
	if(n_chest == 1) {
		if(partner <= 1) // neighbour on the X axis -> face along Z
			facing = (lz > 0.0F) ? 0 : 2;
		else // neighbour on the Z axis -> face along X
			facing = (lx > 0.0F) ? 3 : 1;
	} else if(fabsf(lx) > fabsf(lz)) {
		facing = (lx > 0.0F) ? 3 : 1;
	} else {
		facing = (lz > 0.0F) ? 0 : 2;
	}

	struct block_data blk = (struct block_data) {
		.type = it->id,
		.metadata = facing,
		.sky_light = 0,
		.torch_light = 0,
	};

	struct block_info blk_info = *where;
	blk_info.block = &blk;

	if(block_place_obstructed_for(s, actor, &blk_info)) // player + all entities (Beta gate)
		return false;

	// A removed chest whose content spill hit OOM keeps its backing row until
	// the retry pass can materialize every EntityItem. Do not place a new owner
	// at that coordinate and accidentally alias the retained contents.
	/* A1b: rows are looked up and claimed in the placing world only;
	 * another world's same-coordinate chest is neither an obstruction nor
	 * reusable. */
	struct te_store st = server_world_tile_store(w);
	if(te_store_find(&st, TE_KIND_CHEST, where->x, where->y, where->z) >= 0)
		return false;

	{
		int i = te_store_claim(&st, TE_KIND_CHEST, where->x, where->y,
							   where->z, NULL);
		if(i >= 0) {
			// Clear the WHOLE backing row, not just the first 27. chest_items[]
			// is a shared MAX_CHEST_SLOTS(54)-wide pool drawn by both wooden (27)
			// and iron (54) chests; a freed iron-chest row keeps its items in
			// slots 27..53, so clearing only 0..26 here left those stale -- the
			// wooden break loop then dropped all 54 = item dupe. (matches the
			// iron-chest place clear + the dungeon/server_world full-row clears.)
			memset(&s->chest_items[i], 0, sizeof(s->chest_items[i]));
			server_world_set_block(w, where->x, where->y, where->z, blk);

			// Force the existing partner to the same facing so the double
			// renders consistently (Beta sets both halves to one facing).
			if(n_chest == 1) {
				w_coord_t px = where->x + CHEST_LINK_DX[partner];
				w_coord_t pz = where->z + CHEST_LINK_DZ[partner];
				struct block_data pb;
				if(server_world_get_block(w, px, where->y, pz, &pb)
				   && pb.type == BLOCK_CHEST && pb.metadata != facing) {
					pb.metadata = facing;
					server_world_set_block(w, px, where->y, pz, pb);
				}
			}
			return true;
		}
	}

	puts("Too many chests");
	return false;
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	if(it) {
		it->id = this->block->type;
		it->durability = this->block->metadata;
		it->count = 1;
	}

	return 1;
}

static void onBreak(struct server_local* s, struct block_info* this) {
	bool window_owns_contents
		= live_window_owns_chest(s, this->x, this->y, this->z);
	struct te_store st = server_world_tile_store(block_world(s)); /* A1b */
	for(int i = te_store_find(&st, TE_KIND_CHEST, this->x, this->y, this->z);
		i >= 0; i = -1) {
		if(window_owns_contents) {
			// The window is authoritative. Spill only this half now (Beta
			// onBlockRemoval), leaving the player-owned cursor alone. On OOM the
			// helper leaves every slot intact and close retries the orphan batch.
			(void)server_local_spill_active_container(
				s, this->x, this->y, this->z);
			te_store_free_row(&st, TE_KIND_CHEST, i);
			memset(s->chest_items[i], 0, sizeof(s->chest_items[i]));
		} else {
			if(!server_local_spawn_destroyed_container_items(
				   s, this->x, this->y, this->z, s->chest_items[i],
				   CHEST_SIZE_STORAGE))
				return; // retain the row for the server retry pass
			te_store_free_row(&st, TE_KIND_CHEST, i);
			memset(s->chest_items[i], 0, sizeof(s->chest_items[i]));
		}
		break;
	}
}

struct block block_chest = {
	.name = "Chest",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onBreak = onBreak,
	.onRandomTick = NULL,
	.onRightClick = onRightClick,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.luminance = 0,
	.double_sided = false,
	.can_see_through = false,
	.ignore_lighting = false,
	.flammable = false,
	.place_ignore = false,
	.digging.hardness = 3750,
	.digging.tool = TOOL_TYPE_AXE,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_MAX,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = onItemPlace,
		.fuel = 2,
		.render_data.block.has_default = true,
		.render_data.block.default_metadata = 0,
		.render_data.block.default_rotation = 2,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};

