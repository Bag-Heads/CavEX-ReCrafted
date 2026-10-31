/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

#include "../../block/blocks_data.h"
#include "../../entity/entity.h"
#include "../../entity/entity_painting.h"
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)

// [740c583: ItemPainting.onItemUse] the CLICKED block (`on`) becomes the
// wall tile; `side` selects both the direction AND whether placement is even
// attempted (top/bottom faces refuse). Beta's own onItemUse acts on the
// clicked block directly (there is no separate "adjacent air" position for a
// painting), matching this engine's onUseOnBlock contract exactly -- see the
// items.h onUseOnBlock doc comment.
static bool onUseOnBlock(struct server_local* s, struct server_player* actor, struct item_data* it,
						 struct block_info* on, enum side side) {
	assert(s && it && on);

	int direction;
	if(!painting_direction_from_side(side, &direction))
		return false;
	struct server_world* pw = server_local_world_of(s, actor ? actor : &s->player);
	if(!pw)
		return false;

	// Scratch candidate entity, world-attached but never inserted into
	// s->entities -- exactly Beta's `this` during EntityPainting's
	// constructor selection loop (before `entityJoinedWorld`). Zero-inited
	// defensively: entity_default_init does not touch every field (health,
	// drop_item, ...), and this local must never carry stack garbage into
	// anything that later reads them. art_index=0 is a placeholder;
	// entity_painting_select_art tests every index itself.
	struct entity scratch = {0};
	entity_painting(0, &scratch, true, pw, /*art_index=*/0, direction,
					on->x, on->y, on->z);

	/* I02 (board #8): Beta's Entity constructor takes the id
	 * (nextEntityID++) BEFORE EntityPainting's selection loop, and the pick
	 * is that new painting's own `this.rand.nextInt(n)` [CFR
	 * EntityPainting.java:61]. So the id is reserved here -- consumed even
	 * when nothing fits, as Beta's is -- and the pick draws from the
	 * lifetime stream that id seeds (entity_lifetime_rng_seed). The world
	 * stream is not touched. */
	const uint32_t id = server_local_gen_id(s);
	/* Beta ItemPainting.onItemUse constructs and joins in its World argument.
	 * Selection and the joined entity must seed from that SAME world. */
	struct java_random own;
	entity_lifetime_rng_seed(&own, pw->seed, pw->dimension, id);

	int art_index;
	if(!entity_painting_select_art(&scratch, s, direction, on->x, on->y,
								   on->z, &own, &art_index))
		// [740c583] var6 empty (nothing fits): the constructed EntityPainting
		// never reaches entityJoinedWorld, so nothing joins the world and
		// the stack is untouched -- but the click IS still consumed (Beta
		// returns true here too, just like every other non-top/bottom path).
		return true;

	if(!server_local_spawn_painting_id_w(s, pw, id, art_index, direction, on->x,
									   on->y, on->z))
		return true; // allocation/join failure: click consumed, nothing
					 // spawns, stack untouched (droppable per CODESTYLE 1.7)

	it->count--; // [740c583] `--var1.stackSize;`
	return true;
}

struct item item_painting = {
	.name = "Painting",
	.has_damage = false,
	// Beta Item default maxStackSize=64; the painting.setIconCoord(...)
	// static-init chain [740c583: Item.java] never calls setMaxStackSize.
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.onUseOnBlock = onUseOnBlock,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			// [740c583: Item.java static initializer] `painting = (new
			// ItemPainting(65)).setIconCoord(10, 1)...` -- NOT ItemPainting's
			// own constructor (which sets nothing); the icon is assigned at
			// the Item.painting singleton call site.
			.texture_x = 10,
			.texture_y = 1,
		},
	},
};
