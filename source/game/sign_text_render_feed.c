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

/*
	Wave-3 integration glue (2026-08-20): joins the two halves of in-world
	sign text, which were deliberately authored against a fixed contract and
	nothing else --

	  sign_text_store.c      OWNS the four text lines per sign position
	                         (fed by chunk load / edit echo / break / the MP
	                         UpdateSign packet), but knows nothing about
	                         rendering.
	  render_sign_text.c     OWNS Beta's TileEntitySignRenderer geometry as a
	                         PURE function (golden-tested with no GL/GX), and
	                         render_sign_text_draw.c wraps it in the platform
	                         draw pass -- but takes its signs from a caller-
	                         supplied iterator, so it knows nothing about the
	                         store.

	This file is the ONLY place the two meet. It also supplies the third
	ingredient neither half has: the sign's BLOCK METADATA (its rotation),
	which lives in the client world, not in the text store.

	Culling policy lives here on purpose (the draw pass documents that the
	caller owns it): a bounded, nearest-wins collect inside
	SIGN_TEXT_RENDER_DISTANCE blocks. Text at Beta's 1/60-ish scale is
	unreadable well before that, so the bound costs nothing visible while
	keeping the per-frame cost flat regardless of how many signs a world
	holds.
*/

#include "sign_text_render_feed.h"

#include <stdio.h>

#include "../graphics/render_sign_text_draw.h"
#include "../world.h"
#include "game_state.h"
#include "sign_text_store.h"

// Blocks. Beyond this the glyphs are sub-pixel; Beta relies on ordinary
// distance/fog culling for the same effect.
#define SIGN_TEXT_RENDER_DISTANCE 24.0f
// Per-frame ceiling (also the collector's clamp). 16 signs in one view is
// already a dense sign wall; past that the nearest ones win.
#define SIGN_TEXT_RENDER_MAX 16

struct sign_feed_cursor {
	struct sign_text_record records[SIGN_TEXT_RENDER_MAX];
	size_t count;
	size_t next;
	struct world* world;
};

// sign_text_iter_fn: yields each collected sign plus its live block id and
// metadata for the draw pass's variant/rotation branch. A stored sign whose
// block is no longer either BlockSign id (broken this frame, or unloaded
// before the store caught up) is SKIPPED rather than drawn against stale
// geometry -- the store's own removal feeds are authoritative but not
// instantaneous.
static bool sign_feed_next(void* ctx, int32_t* x, int32_t* y, int32_t* z,
						   uint8_t* block_type, uint8_t* meta,
						   char* out_grid) {
	struct sign_feed_cursor* cur = ctx;
	while(cur->next < cur->count) {
		const struct sign_text_record* rec = &cur->records[cur->next++];
		struct block_data blk
			= world_get_block(cur->world, rec->x, rec->y, rec->z);
		if(!block_is_any_sign(blk.type))
			continue;
		*x = rec->x;
		*y = rec->y;
		*z = rec->z;
		*block_type = blk.type;
		*meta = blk.metadata;
		memcpy(out_grid, rec->grid, sizeof(rec->grid));
		return true;
	}
	return false;
}

void sign_text_render_feed(mat4 view) {
	sign_text_render_feed_owned(view, &gstate.players[0].camera);
}

void sign_text_render_feed_owned(mat4 view, const struct camera* camera) {
	struct sign_feed_cursor cur;
	cur.world = world_view_current();
	cur.next = 0;
	cur.count = sign_text_store_collect_near(
		camera->x, camera->y, camera->z,
		SIGN_TEXT_RENDER_DISTANCE, cur.records, SIGN_TEXT_RENDER_MAX);
	if(cur.count == 0)
		return; // the draw pass defers ALL state changes until the first
				// sign, so an empty world costs exactly this early-out
	render_sign_text_pass(view, sign_feed_next, &cur);
}
