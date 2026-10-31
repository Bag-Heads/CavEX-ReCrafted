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
#include <math.h>
#include <string.h>

#include "../block/aabb.h"
#include "../block/blocks.h"
#include "../item/items.h"
#include "../network/server_interface.h" // svin_rpc_send (onLeftClick)
#include "../network/server_local.h"
#include "../worldgen/java_random.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../platform/gfx.h" // gfx_bind_texture/gfx_draw_quads_flt/gfx_lookup_light (render)
#include "../platform/texture.h" // texture_painting (render)
#include "../util.h"
#include "entity.h"
#include "entity_painting.h"

/* Beta EnumArt [740c583], verbatim: title, sizeX, sizeY, offsetX, offsetY
 * (all pixels). Declaration order IS EnumArt.values() order -- pinned by
 * test_painting_env's exhaustive per-entry check. */
const struct painting_art_def g_painting_art[PAINTING_ART_COUNT] = {
	{"Kebab", 16, 16, 0, 0},
	{"Aztec", 16, 16, 16, 0},
	{"Alban", 16, 16, 32, 0},
	{"Aztec2", 16, 16, 48, 0},
	{"Bomb", 16, 16, 64, 0},
	{"Plant", 16, 16, 80, 0},
	{"Wasteland", 16, 16, 96, 0},
	{"Pool", 32, 16, 0, 32},
	{"Courbet", 32, 16, 32, 32},
	{"Sea", 32, 16, 64, 32},
	{"Sunset", 32, 16, 96, 32},
	{"Creebet", 32, 16, 128, 32},
	{"Wanderer", 16, 32, 0, 64},
	{"Graham", 16, 32, 16, 64},
	{"Match", 32, 32, 0, 128},
	{"Bust", 32, 32, 32, 128},
	{"Stage", 32, 32, 64, 128},
	{"Void", 32, 32, 96, 128},
	{"SkullAndRoses", 32, 32, 128, 128},
	{"Fighters", 64, 32, 0, 96},
	{"Pointer", 64, 64, 0, 192},
	{"Pigscene", 64, 64, 64, 192},
	{"BurningSkull", 64, 64, 128, 192},
	{"Skeleton", 64, 48, 192, 64},
	{"DonkeyKong", 64, 48, 192, 112},
};

#define PAINTING_VALIDITY_CHECK_INTERVAL 100

bool painting_direction_from_side(enum side s, int* out_direction) {
	assert(out_direction);
	switch(s) {
	case SIDE_FRONT: *out_direction = PAINTING_DIR_NORTH; return true; // -Z
	case SIDE_LEFT: *out_direction = PAINTING_DIR_WEST; return true;   // -X
	case SIDE_BACK: *out_direction = PAINTING_DIR_SOUTH; return true;  // +Z
	case SIDE_RIGHT: *out_direction = PAINTING_DIR_EAST; return true;  // +X
	case SIDE_TOP:
	case SIDE_BOTTOM:
	default:
		// [740c583: ItemPainting.onItemUse] Reject both bottom and top faces;
		// paintings can be placed only on horizontal sides.
		return false;
	}
}

// Beta EntityPainting.getArtSize [740c583], verbatim: even-block-count
// footprints (32px=2 blocks, 64px=4 blocks) need a +0.5 re-center shift;
// odd counts (16px=1, 48px=3) need none. Always called with art.sizeX for
// the wall-plane shift and separately with art.sizeY for the vertical one.
static float painting_art_center_shift(int size_px) {
	if(size_px == 32 || size_px == 64)
		return 0.5f;
	return 0.0f;
}

void painting_compute_geometry(int art_index, int direction, int tile_x,
							   int tile_y, int tile_z, vec3 out_pos,
							   struct AABB* out_bbox) {
	assert(art_index >= 0 && art_index < PAINTING_ART_COUNT);
	assert(direction >= PAINTING_DIR_NORTH && direction <= PAINTING_DIR_EAST);
	const struct painting_art_def* art = &g_painting_art[art_index];

	/* [740c583: EntityPainting.setDirection] var2/var3/var4 = box half-size
	 * (pre /32-scale); var4 defaults to sizeX, NOT a separate depth field --
	 * Beta reuses sizeX as the "along-wall" footprint width regardless of
	 * whether that wall runs along the world X or Z axis. */
	float half_x = (float)art->size_x_px;
	float half_y = (float)art->size_y_px;
	float half_z = (float)art->size_x_px;
	if(direction != PAINTING_DIR_NORTH && direction != PAINTING_DIR_SOUTH)
		half_x = 0.5f; // E/W wall: X is the thin (perpendicular) axis
	else
		half_z = 0.5f; // N/S wall: Z is the thin (perpendicular) axis
	half_x /= 32.0f;
	half_y /= 32.0f;
	half_z /= 32.0f;

	float px = (float)tile_x + 0.5f;
	float py = (float)tile_y + 0.5f;
	float pz = (float)tile_z + 0.5f;
	const float wall_offset = 0.5625f;
	if(direction == PAINTING_DIR_NORTH)
		pz -= wall_offset;
	if(direction == PAINTING_DIR_WEST)
		px -= wall_offset;
	if(direction == PAINTING_DIR_SOUTH)
		pz += wall_offset;
	if(direction == PAINTING_DIR_EAST)
		px += wall_offset;

	float center_shift = painting_art_center_shift(art->size_x_px);
	if(direction == PAINTING_DIR_NORTH)
		px -= center_shift;
	if(direction == PAINTING_DIR_WEST)
		pz += center_shift;
	if(direction == PAINTING_DIR_SOUTH)
		px += center_shift;
	if(direction == PAINTING_DIR_EAST)
		pz -= center_shift;

	py += painting_art_center_shift(art->size_y_px);

	if(out_pos) {
		out_pos[0] = px;
		out_pos[1] = py;
		out_pos[2] = pz;
	}

	if(out_bbox) {
		// [740c583] The box adjustment is -0.00625F on every axis:
		// subtract it from each minimum and add it to each maximum.
		// Its negative sign raises the minimum by 0.00625 and lowers the
		// maximum by the same amount, producing an inset rather than expansion.
		const float inset = -0.00625f;
		out_bbox->x1 = px - half_x - inset;
		out_bbox->y1 = py - half_y - inset;
		out_bbox->z1 = pz - half_z - inset;
		out_bbox->x2 = px + half_x + inset;
		out_bbox->y2 = py + half_y + inset;
		out_bbox->z2 = pz + half_z + inset;
	}
}

// Beta onValidSurface's backing-wall loop [740c583], verbatim shape: the axis
// running ALONG the wall gets floor(pos - sizeX/32) as its origin; the axis
// PERPENDICULAR to the wall stays pinned at the clicked tile's own
// coordinate. `pos` is the setDirection-computed logical center (posX/Y/Z).
//
// Post-review finding #4 (PAINTING_SLICE_NOTES.md): a cell whose CHUNK is not
// currently RESIDENT server-side is not the same fact as Beta's "block id 0
// outside the loaded world". Beta's ChunkProviderServer resolves every block
// query synchronously -- there is no persistent "this entity's chunk is
// resident but its dependency's neighbour chunk was evicted" state to be
// unfaithful to -- while this engine's near/far streaming can hold exactly
// that state for many ticks, with no atomicity between neighbouring chunks
// (server_local.c's eviction loop). A painting's OWN chunk membership (keyed
// by e->pos, the wall-offset geometry centre) can differ from its wall's own
// tile_x/tile_z by one chunk across a seam (painting_compute_geometry's
// wall_offset+center_shift), so "my own chunk stayed resident" does not
// imply "my wall's chunk did too". A tri-state result lets the caller
// decide: placement-time (the wall is always resident then -- the player is
// standing next to it) still rejects on ANY unresolved cell, byte-identical
// to the old bool contract (see entity_painting_on_valid_surface below); the
// periodic revalidation (entity_painting_server_tick) treats INDETERMINATE
// as "skip this cycle, recheck later", never as "invalid, drop it". A
// DEFINITIVE gap found anywhere in the footprint still wins over an
// indeterminate cell found elsewhere in the same scan -- a real hole is a
// real hole regardless of what else couldn't be resolved.
enum painting_wall_probe {
	PAINTING_WALL_SOLID,
	PAINTING_WALL_GAP,
	PAINTING_WALL_INDETERMINATE,
};

static enum painting_wall_probe painting_backing_wall_probe(
	struct entity* e, int art_index, int direction, int tile_x, int tile_y,
	int tile_z, const vec3 pos) {
	const struct painting_art_def* art = &g_painting_art[art_index];
	int blocks_w = art->size_x_px / 16;
	int blocks_h = art->size_y_px / 16;

	int wall_x = tile_x;
	int wall_z = tile_z;
	if(direction == PAINTING_DIR_NORTH || direction == PAINTING_DIR_SOUTH)
		wall_x = (int)floorf(pos[0] - (float)art->size_x_px / 32.0f);
	if(direction == PAINTING_DIR_WEST || direction == PAINTING_DIR_EAST)
		wall_z = (int)floorf(pos[2] - (float)art->size_x_px / 32.0f);
	int wall_y = (int)floorf(pos[1] - (float)art->size_y_px / 32.0f);

	bool any_unresolved = false;
	for(int along = 0; along < blocks_w; along++) {
		for(int up = 0; up < blocks_h; up++) {
			struct block_data blk;
			bool ok;
			if(direction == PAINTING_DIR_WEST || direction == PAINTING_DIR_EAST)
				ok = entity_get_block(e, tile_x, wall_y + up, wall_z + along,
									  &blk);
			else
				ok = entity_get_block(e, wall_x + along, wall_y + up, tile_z,
									  &blk);
			if(!ok) {
				// Beta World.getBlockMaterial defaults to Material.air (not
				// solid) outside a loaded chunk -- an unloaded wall fails
				// the same as an air one for the FINAL bool-collapsed
				// verdict, but which verdict this maps to is the caller's
				// call (see doc comment above). Keep scanning: a definitive
				// gap elsewhere in the footprint must still win.
				any_unresolved = true;
				continue;
			}
			if(!block_beta_material_is_solid(blk.type))
				return PAINTING_WALL_GAP;
		}
	}
	return any_unresolved ? PAINTING_WALL_INDETERMINATE : PAINTING_WALL_SOLID;
}

// Beta World.getCollidingBoundingBoxes filters candidates with
// AxisAlignedBB.intersectsWith -- STRICT, a shared face is NOT an
// intersection [740c583]. entity_block_aabb_test (entity.c) intentionally
// uses the INCLUSIVE aabb_intersection for this engine's general-purpose
// collision callers (aabb.c's own comment: "existing collision callers
// depend on" the inclusive test) -- reusing it for painting's TOP block
// check was an internal inconsistency with THIS SAME onValidSurface's
// entity-overlap loop below, which already correctly uses
// aabb_intersection_strict for the analogous Beta check. Post-review finding
// #5: mirror entity_block_aabb_test exactly, swapping only the leaf
// comparison, so the cell-walk geometry (the fence extra Y-row, etc.) stays
// byte-identical to every other entity_intersection caller.
static bool painting_block_aabb_test_strict(struct AABB* entity,
											struct block_info* blk_info) {
	assert(entity && blk_info);
	struct block* b = blocks[blk_info->block->type];
	size_t count = b->getBoundingBox(blk_info, true, NULL);
	if(count > 0) {
		struct AABB bbox[count];
		b->getBoundingBox(blk_info, true, bbox);
		for(size_t k = 0; k < count; k++) {
			aabb_translate(bbox + k, blk_info->x, blk_info->y, blk_info->z);
			if(aabb_intersection_strict(entity, bbox + k))
				return true;
		}
	}
	return false;
}

enum painting_surface_check {
	PAINTING_SURFACE_VALID,
	PAINTING_SURFACE_BLOCKED,
	PAINTING_SURFACE_INDETERMINATE,
};

// Post-review finding #2 (adversarial review, 2026-08-13): the TOP check's
// block-half used to hand `frame` straight to the GENERIC entity_intersection
// (entity.c) + painting_block_aabb_test_strict. That generic walk's own
// residency guard (`if(entity_get_block(...) && ... && test(...)) return
// true;`) silently folds "chunk not resident" and "cell resolved, genuinely
// clear" into the same "keep scanning, nothing found yet" outcome --
// entity_intersection is the engine's shared collision core (mob movement,
// LOS, arrow/egg/fireball hits, TNT, the piston sweep), so growing ITS
// contract a tri-state return is a change far outside painting's blast
// radius and not attempted here.
//
// Instead: a FIRST pass over entity_intersection's own exact cell-bounds
// formula (fence -1 Y row and the WORLD_HEIGHT clamp included, copied
// verbatim) that ONLY asks "did every cell resolve" -- entity_get_block's
// bool result, nothing else -- followed by the ORIGINAL, byte-for-byte
// UNCHANGED entity_intersection(e, a, painting_block_aabb_test_strict) call
// for the real BLOCKED/clear verdict (get_piston_moving plumbing and all,
// since that second pass is literally the old code, untouched). Whenever
// every cell in the footprint resolves -- the ONLY case the pre-fix code
// needed to handle correctly -- `any_unresolved` stays false and the result
// reduces to exactly `entity_intersection(...) ? BLOCKED : VALID`: provably
// byte-identical to the old single-call behaviour. The only new fact is
// INDETERMINATE, mirroring painting_backing_wall_probe's own tri-state for
// the MIDDLE check (finding #4 of the first review round).
static enum painting_surface_check painting_top_block_probe(struct entity* e,
															 struct AABB* a) {
	w_coord_t min_x = (w_coord_t)floorf(a->x1);
	w_coord_t min_y = (w_coord_t)floorf(a->y1) - 1; // fence extra Y-row, see
													 // entity_intersection
	if(min_y < 0)
		min_y = 0;
	w_coord_t min_z = (w_coord_t)floorf(a->z1);

	w_coord_t max_x = (w_coord_t)ceilf(a->x2) + 1;
	w_coord_t max_y = (w_coord_t)ceilf(a->y2) + 1;
	if(max_y > WORLD_HEIGHT - 1)
		max_y = WORLD_HEIGHT - 1;
	w_coord_t max_z = (w_coord_t)ceilf(a->z2) + 1;

	bool any_unresolved = false;
	for(w_coord_t x = min_x; x < max_x && !any_unresolved; x++)
		for(w_coord_t z = min_z; z < max_z && !any_unresolved; z++)
			for(w_coord_t y = min_y; y < max_y; y++) {
				struct block_data blk;
				if(!entity_get_block(e, x, y, z, &blk)) {
					any_unresolved = true;
					break;
				}
			}

	if(entity_intersection(e, a, painting_block_aabb_test_strict))
		// Definitive: a real solid intersection wins even over an unresolved
		// cell found elsewhere in the footprint (finding #4's priority rule,
		// mirrored here).
		return PAINTING_SURFACE_BLOCKED;
	return any_unresolved ? PAINTING_SURFACE_INDETERMINATE
						  : PAINTING_SURFACE_VALID;
}

static enum painting_surface_check entity_painting_validate_surface(
	struct entity* e, struct server_local* s, int art_index, int direction,
	int tile_x, int tile_y, int tile_z, uint32_t self_id) {
	assert(e && s && art_index >= 0 && art_index < PAINTING_ART_COUNT);

	vec3 pos;
	struct AABB frame;
	painting_compute_geometry(art_index, direction, tile_x, tile_y, tile_z,
							  pos, &frame);

	// TOP check, block half [740c583: World.getCollidingBoundingBoxes]: solid
	// block geometry inside the thin frame box (e.g. a block filling the gap
	// right in front of the wall) blocks placement. STRICT (finding #5).
	// Tri-state (post-review finding #2): a BLOCKED verdict here is
	// definitive and returned immediately; an INDETERMINATE one is deferred
	// to the bottom of this function so a later DEFINITIVE result (entity
	// overlap below, or a wall GAP) still wins over it -- the same priority
	// rule the MIDDLE check's own tri-state already follows.
	struct AABB block_probe = frame;
	enum painting_surface_check block_chk
		= painting_top_block_probe(e, &block_probe);
	if(block_chk == PAINTING_SURFACE_BLOCKED)
		return PAINTING_SURFACE_BLOCKED;

	// TOP check, entity half [740c583: World.getCollidingBoundingBoxes ->
	// Entity.getBoundingBox()]: only entities that OVERRIDE the nullable
	// getBoundingBox() accessor can occupy this half of the check -- grep-
	// verified against the decompile pack, only EntityBoat and EntityMinecart
	// do. Batch-f landing note (2026-08-25): this module's original
	// 2026-08-13 base predates CaveWii's own boat entity, so its scan
	// hardcoded ENTITY_MINECART only; landing onto tip (which now HAS boats)
	// required reconciling this against Beta rather than blindly generalizing
	// to "any entity with a non-null e->getBoundingBox". That generalization
	// would have been WRONG here: THIS engine's e->getBoundingBox field is
	// overloaded to ALSO serve raycast/click-targeting for entity kinds Beta's
	// own getBoundingBox() never makes solid (arrow, fireball, egg, item,
	// primed_tnt, fish, local/remote player, monster all wire a non-null
	// e->getBoundingBox purely for that unrelated purpose -- see round-2
	// finding #1's own "two Beta concepts collapsed into one field" analysis
	// above raycast_entity in entity.c). Reusing that generic non-null check
	// here would have made painting placement wrongly refuse next to a live
	// mob, a dropped item, or an in-flight arrow, none of which Beta's real
	// onValidSurface would ever treat as occupying this half. The correct,
	// Beta-faithful set is the explicit {ENTITY_MINECART, ENTITY_BOAT} below
	// -- both of which genuinely mirror Beta's own getBoundingBox() override
	// set in this engine's architecture (block_place_obstructed and the
	// piston push sweep both treat them as solid/pushable, matching Beta).
	//
	// BOTTOM check [740c583: onValidSurface's trailing `instanceof
	// EntityPainting` scan]: an existing painting occupying the same
	// footprint blocks a second one.
	//
	// Post-review findings #1/#2: a live painting's OWN e->getBoundingBox
	// must stay NULL -- Beta has no EntityPainting override of that accessor,
	// so a non-null one wrongly exposes a live painting to every OTHER
	// generic getBoundingBox consumer (block_place_obstructed, the piston
	// push sweep) as if it were a solid, pushable boat/minecart-like object,
	// which Beta never does (see entity_painting()'s assignment below). The
	// PAINTING branch here therefore computes the OTHER painting's frame box
	// directly from its own tile/art/direction fields -- never through
	// other->getBoundingBox -- so nulling that accessor cannot break this
	// self-overlap check; only the MINECART/BOAT branch still legitimately
	// reads the vtable (entity_minecart.c/entity_boat.c both wire a real
	// accessor).
	//
	// Both checks scan the SAME un-expanded frame box Beta does (its 0.25
	// pre-expand on the top check is an entity-FETCH broad-phase only -- the
	// accept/reject boundary is the strict intersect against the
	// un-expanded box either way) -- one dict pass covers both, order
	// relative to the wall-solidity check below is unobservable (this
	// predicate has no side effects and draws no RNG).
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* other = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(!other || other->id == self_id
		   || server_local_entity_world(s, other) != server_local_entity_world(s, e))
			continue;
		struct AABB obox;
		if(other->type == ENTITY_MINECART || other->type == ENTITY_BOAT) {
			if(!other->getBoundingBox
			   || other->getBoundingBox(other, &obox) == 0)
				continue;
		} else if(other->type == ENTITY_PAINTING) {
			painting_compute_geometry(other->data.painting.art_index,
				other->data.painting.direction, other->data.painting.tile_x,
				other->data.painting.tile_y, other->data.painting.tile_z,
				NULL, &obox);
		} else {
			continue;
		}
		struct AABB tbox = frame;
		if(aabb_intersection_strict(&tbox, &obox))
			return PAINTING_SURFACE_BLOCKED;
	}

	// MIDDLE check: the backing wall is solid across the art's full block
	// footprint. [740c583: onValidSurface's getBlockMaterial/.isSolid loop]
	enum painting_wall_probe wall_chk = painting_backing_wall_probe(
		e, art_index, direction, tile_x, tile_y, tile_z, pos);
	if(wall_chk == PAINTING_WALL_GAP)
		return PAINTING_SURFACE_BLOCKED;

	// Neither check found a DEFINITIVE obstruction: report INDETERMINATE
	// (post-review finding #2) if EITHER the TOP block-half or the MIDDLE
	// wall check couldn't fully resolve its own footprint, else VALID --
	// same deferral semantics finding #4 already established for the wall
	// alone, now applied consistently across both chunk-dependent checks.
	if(block_chk == PAINTING_SURFACE_INDETERMINATE
	   || wall_chk == PAINTING_WALL_INDETERMINATE)
		return PAINTING_SURFACE_INDETERMINATE;
	return PAINTING_SURFACE_VALID;
}

bool entity_painting_on_valid_surface(struct entity* e, struct server_local* s,
									  int art_index, int direction, int tile_x,
									  int tile_y, int tile_z,
									  uint32_t self_id) {
	assert(e && s && art_index >= 0 && art_index < PAINTING_ART_COUNT);
	// Byte-identical to the pre-refactor bool contract: BLOCKED and
	// INDETERMINATE both collapse to false here (any unresolved cell still
	// rejects placement) -- only entity_painting_server_tick below observes
	// the tri-state directly. See entity_painting_validate_surface and
	// painting_backing_wall_probe's doc comments (finding #4).
	return entity_painting_validate_surface(e, s, art_index, direction,
		tile_x, tile_y, tile_z, self_id) == PAINTING_SURFACE_VALID;
}

bool entity_painting_select_art(struct entity* scratch, struct server_local* s,
								int direction, int tile_x, int tile_y,
								int tile_z, struct java_random* rng,
								int* out_art_index) {
	assert(scratch && s && rng && out_art_index);
	int fitting[PAINTING_ART_COUNT];
	int fitting_count = 0;
	// [740c583: EntityPainting(World,x,y,z,dir)] iterate EnumArt.values() in
	// order, testing each candidate's OWN geometry (Beta mutates this.art +
	// re-runs setDirection/onValidSurface per iteration; collecting fitting
	// indices into a local array instead is a provably-equivalent
	// restructure -- same 25 candidates tested in the same order, same
	// fitting SET, zero RNG draws inside the loop either way).
	for(int i = 0; i < PAINTING_ART_COUNT; i++)
		if(entity_painting_on_valid_surface(scratch, s, i, direction, tile_x,
											tile_y, tile_z, UINT32_MAX))
			fitting[fitting_count++] = i;
	if(fitting_count == 0)
		// [740c583] var6.isEmpty(): every candidate, including the
		// last-tried one, failed onValidSurface -- the caller must not spawn
		// anything (see entity_painting.h and PAINTING_SLICE_NOTES.md for why
		// Beta's own redundant final re-check is safely omitted here).
		return false;
	// [740c583] Choose one fitting art with a single bounded random draw.
	// -- ONE draw, in a named local per S2.3's sibling-argument rule. I02:
	// `this.rand` is the painting's own Random (CFR EntityPainting.java:61,
	// Entity.java:96), so `rng` is the new painting's lifetime stream.
	int pick = jrand_next_int_bound(rng, fitting_count);
	*out_art_index = fitting[pick];
	return true;
}

int painting_art_by_title(const char* title) {
	if(title)
		for(int i = 0; i < PAINTING_ART_COUNT; i++)
			if(strcmp(g_painting_art[i].name, title) == 0)
				return i;
	// [740c583: EntityPainting.readEntityFromNBT] An unknown art name falls
	// back to Kebab, which is index 0.
	return 0;
}

void entity_painting_drop_self(struct entity* e, struct server_local* s) {
	assert(e && s);
	// [740c583: onUpdate's invalid-surface branch + attackEntityFrom] BOTH
	// spawn a painting item at the entity position, using an EntityItem
	// with a painting ItemStack -- the standard small-spread drop physics
	// (server_local_spawn_item's throw=false path), never a player-throw
	// toss.
	struct item_data drop = {.id = ITEM_PAINTING, .durability = 0, .count = 1};
	vec3 pos = {e->pos[0], e->pos[1], e->pos[2]};
	struct server_world* w = server_local_entity_world(s, e);
	if(w == &s->worlds[0])
		server_local_spawn_item(pos, &drop, false, s); // legacy SP bytes/RNG
	else if(w) {
		struct entity* item = server_local_spawn_item_w(w, pos, &drop, false);
		if(item)
			(void)server_local_try_publish_entity_w(s, w, item);
	}
}

bool entity_painting_hit_box(const struct entity* e, struct AABB* out) {
	if(!e || !out || e->type != ENTITY_PAINTING)
		return false;
	const struct entity_painting* p = &e->data.painting;
	if(p->art_index < 0 || p->art_index >= PAINTING_ART_COUNT
	   || p->direction < PAINTING_DIR_NORTH || p->direction > PAINTING_DIR_EAST)
		return false;
	painting_compute_geometry(p->art_index, p->direction, p->tile_x,
							  p->tile_y, p->tile_z, NULL, out);
	return true;
}

void entity_painting_mark_broken(struct entity* e) {
	if(e && e->type == ENTITY_PAINTING && e->delay_destroy < 0)
		e->delay_destroy = 1;
}

static bool entity_painting_server_tick(struct entity* e,
										struct server_local* s) {
	assert(e && s && e->type == ENTITY_PAINTING);
	struct entity_painting* p = &e->data.painting;
	/* A latched break (entity_painting_mark_broken: an explosion or a
	 * lightning bolt hit it) -- EntityPainting.attackEntityFrom :200-208:
	 * drop the painting item and die. */
	if(e->delay_destroy == 1) {
		entity_painting_drop_self(e, s);
		return true;
	}
	// [740c583: EntityPainting.onUpdate] On the authoritative world, compare
	// the old counter to 100 before incrementing. This check fires on
	// the tick tickCounter1 WAS 100 (the 101st call), then resets to 0. The
	// `!multiplayerWorld` guard is satisfied by this being the SERVER
	// (authoritative) tick only -- there is no client-side painting tick.
	if(p->tick_counter++ == PAINTING_VALIDITY_CHECK_INTERVAL) {
		p->tick_counter = 0;
		enum painting_surface_check chk = entity_painting_validate_surface(
			e, s, p->art_index, p->direction, p->tile_x, p->tile_y,
			p->tile_z, e->id);
		if(chk == PAINTING_SURFACE_BLOCKED) {
			entity_painting_drop_self(e, s);
			return true; // caller (server entity-tick loop) frees, erases
						 // from the dict, and sends CRPC_ENTITY_DESTROY
		}
		// PAINTING_SURFACE_VALID: confirmed intact.
		// PAINTING_SURFACE_INDETERMINATE (post-review finding #4): the
		// wall's own chunk is not resident right now (a streaming artifact,
		// not a real gap) -- do not destroy on unverifiable data. The
		// counter reset above already restarts the cycle, so this gets a
		// real re-check in another 100 ticks, exactly like a
		// confirmed-valid wall does.
	}
	return false;
}

// [740c583: RenderPainting.func_158_a / func_159_a] Beta draws the frame as
// a grid of 16x16-px THIN BOXES (6 faces each -- a real front-face motive
// crop plus a fixed generic border strip reused on the other 5 faces) tiling
// the art's full sizeX x sizeY footprint. This port draws the front face
// only, as ONE quad spanning the whole footprint with one UV rect
// (atlas_u_px/atlas_v_px .. +size_x_px/+size_y_px) instead of N separate
// 16x16 sub-quads -- mathematically identical final pixels (linear texture
// sampling makes "one quad, one rect" and "N tiled sub-quads, N rects"
// indistinguishable in the rendered image; only the vertex count differs),
// so the per-motive UV data itself stays byte-exact to the cited table with
// zero art-layout approximation. What IS a documented rendering-completeness
// gap, not a data/placement deviation: the back/top/bottom/left/right frame
// faces (Beta's generic border-strip patch, kz.png pixels ~[192,0]-[208,16])
// are not drawn -- those faces sit on a box only 1px (0.0625 world units)
// thick, so they are near edge-on and barely visible in Beta itself; a
// future pass can add them (the bundled assets/art/kz.png now DOES paint a
// generic border-strip patch at that exact region -- tools/gen_kz_
// placeholder.py's draw_back_strip -- so the pixels to sample already exist;
// only the extra 5-face geometry is the remaining gap). A real Beta kz.png
// dropping into a user's own pack (pack-first lookup, texture.c) still
// takes over automatically once that pass lands.
static void entity_painting_render(struct entity* e, mat4 view,
								   float tick_delta) {
	assert(e && e->type == ENTITY_PAINTING);
	// PROJECT_MEMORY 2026-09-01 deferred item #1, now landed: the bundled
	// (CC0) pack ships an ORIGINAL assets/art/kz.png (tools/gen_kz_
	// placeholder.py -- zero Mojang pixels, see assets/README.md), tiled at
	// every EnumArt offset/size from the SAME g_painting_art table this file
	// defines above. texture.c's TEX_LOAD_FILE/tex_read_impl pack-first
	// lookup still wins if the user's OWN texture pack ships a real Beta
	// kz.png (see texture.c's art/kz.png load site) -- this guard only
	// covers the genuine "neither pack has ANY kz.png" case (a corrupt/
	// missing bundled file), matching every other optional-asset consumer
	// in this engine (e.g. texture_zombie's fallback note).
	if(!texture_painting.data)
		return; // Optional asset missing from BOTH packs: draw nothing.

	assert(e->data.painting.art_index >= 0
		  && e->data.painting.art_index < PAINTING_ART_COUNT);
	const struct painting_art_def* art
		= &g_painting_art[e->data.painting.art_index];

	vec3 pos_lerp;
	glm_vec3_lerp(e->pos_old, e->pos, tick_delta, pos_lerp);

	struct block_data in_block = {0};
	(void)entity_get_block(e, (w_coord_t)floorf(pos_lerp[0]),
						   (w_coord_t)floorf(pos_lerp[1]),
						   (w_coord_t)floorf(pos_lerp[2]), &in_block);
	float brightness
		= gfx_lookup_light((in_block.torch_light << 4) | in_block.sky_light);
	uint8_t light_u8 = (uint8_t)lroundf(
		glm_clamp(brightness, 0.0f, 1.0f) * 255.0f);
	uint8_t colors[4 * 4];
	for(int v = 0; v < 4; v++) {
		colors[v * 4 + 0] = light_u8;
		colors[v * 4 + 1] = light_u8;
		colors[v * 4 + 2] = light_u8;
		colors[v * 4 + 3] = 255;
	}

	// Per-direction screen-right world axis, derived so the motif reads
	// correctly to a viewer standing on the frame's visible (outward-normal)
	// side looking back toward the wall: gaze = -outward_normal, world +Y is
	// always screen-up, and right = cross(gaze, up) in this engine's
	// right-handed Y-up convention (the same convention every other mat4/
	// cglm call in this codebase assumes). Outward normals per
	// entity_painting.h's painting_direction doc comment: NORTH=(0,0,-1),
	// WEST=(-1,0,0), SOUTH=(0,0,1), EAST=(1,0,0).
	//   NORTH: gaze=(0,0,1)  -> right=cross(gaze,up)=(-1,0,0)
	//   WEST:  gaze=(1,0,0)  -> right=cross(gaze,up)=(0,0,1)
	//   SOUTH: gaze=(0,0,-1) -> right=cross(gaze,up)=(1,0,0)
	//   EAST:  gaze=(-1,0,0) -> right=cross(gaze,up)=(0,0,-1)
	// UNVERIFIED against a REAL Beta pack: the bundled assets/art/kz.png is
	// an original CC0 placeholder (tools/gen_kz_placeholder.py), not Beta's
	// own asset (never committed here -- Mojang IP), so this derivation can
	// now be visually SANITY-checked (a painting renders, per-direction
	// motifs differ, nothing appears mirrored/upside-down) but not confirmed
	// pixel-correct against the genuine art -- flagged as owed visual proof
	// against a real pack, see this commit's report/deviations note.
	vec3 right_axis;
	switch(e->data.painting.direction) {
	case PAINTING_DIR_NORTH: glm_vec3_copy((vec3){-1.0f, 0.0f, 0.0f}, right_axis); break;
	case PAINTING_DIR_WEST:  glm_vec3_copy((vec3){0.0f, 0.0f, 1.0f}, right_axis); break;
	case PAINTING_DIR_SOUTH: glm_vec3_copy((vec3){1.0f, 0.0f, 0.0f}, right_axis); break;
	case PAINTING_DIR_EAST:  glm_vec3_copy((vec3){0.0f, 0.0f, -1.0f}, right_axis); break;
	default: glm_vec3_copy((vec3){1.0f, 0.0f, 0.0f}, right_axis); break;
	}

	// painting_compute_geometry already centers pos_lerp on the frame's own
	// logical center (wall_offset + the even-size center_shift both baked
	// in) -- no further placement math needed here.
	float half_w = (float)art->size_x_px / 32.0f;
	float half_h = (float)art->size_y_px / 32.0f;
	vec3 right_half;
	glm_vec3_scale(right_axis, half_w, right_half);

	vec3 top_left, top_right, bottom_right, bottom_left;
	glm_vec3_copy(pos_lerp, top_left);
	glm_vec3_sub(top_left, right_half, top_left);
	top_left[1] += half_h;
	glm_vec3_copy(pos_lerp, top_right);
	glm_vec3_add(top_right, right_half, top_right);
	top_right[1] += half_h;
	glm_vec3_copy(pos_lerp, bottom_right);
	glm_vec3_add(bottom_right, right_half, bottom_right);
	bottom_right[1] -= half_h;
	glm_vec3_copy(pos_lerp, bottom_left);
	glm_vec3_sub(bottom_left, right_half, bottom_left);
	bottom_left[1] -= half_h;

	// UV rect normalized against the ACTUAL bound texture's dimensions
	// (matching every other atlas consumer in this engine, e.g. particle.c's
	// TEXTURE_X/Y macros) rather than a hardcoded 256 -- Beta's real kz.png
	// is 256x256 (RenderPainting's own "/256.0F" normalization constant),
	// so this is identical for the faithful asset and only differs if a
	// custom pack ever ships a differently-sized one.
	float u0 = (float)art->atlas_u_px / (float)texture_painting.width;
	float v0 = (float)art->atlas_v_px / (float)texture_painting.height;
	float u1 = (float)(art->atlas_u_px + art->size_x_px)
		/ (float)texture_painting.width;
	float v1 = (float)(art->atlas_v_px + art->size_y_px)
		/ (float)texture_painting.height;

	float verts[4 * 3] = {
		top_left[0], top_left[1], top_left[2],
		top_right[0], top_right[1], top_right[2],
		bottom_right[0], bottom_right[1], bottom_right[2],
		bottom_left[0], bottom_left[1], bottom_left[2],
	};
	float texcoords[4 * 2] = {
		u0, v0,
		u1, v0,
		u1, v1,
		u0, v1,
	};

	gfx_matrix_modelview(view);
	gfx_bind_texture(&texture_painting);
	// Winding-agnostic on purpose: the exact left/right mirroring of the
	// derivation above has no way to be visually confirmed in this repo yet
	// (no kz.png asset -- see the UNVERIFIED note above), so disable
	// backface culling for this one quad the same way this engine already
	// does for other double-sided world content (chunk.c cross-quads,
	// render_block.c leaves, render_weather.c rain/snow all bracket
	// MODE_NONE/MODE_BACK identically) -- guarantees the motif is visible
	// from the room regardless of which winding the derivation turns out to
	// produce; a future pass can tighten this to MODE_BACK once verified.
	gfx_cull_func(MODE_NONE);
	gfx_lighting(false); /* gfx_draw_quads_flt uses direct GX CLR0. */
	gfx_draw_quads_flt(4, verts, colors, texcoords);
	gfx_lighting(true); /* Restore the entity pass's indexed CLR0 descriptor. */
	gfx_cull_func(MODE_BACK);
}

// Post-review findings #3/#8: screen_ingame.c's click-interaction path only
// sends SRPC_ENTITY_ATTACK when e->onLeftClick is non-NULL (it calls
// e->onLeftClick(e) and returns -- see game/gui/screen_ingame.c's left-click
// block). Leaving this NULL made the already-cited, already-tested server
// SRPC_ENTITY_ATTACK dispatch (server_local.c: ENTITY_PAINTING branch)
// unreachable from real input, exactly the bug class entity_minecart.c's own
// onLeftClick comment documents having been fixed once already. Unlike the
// minecart, a painting has no shake/health state to predict client-side --
// Beta's attackEntityFrom is an unconditional, no-accumulation instant kill
// -- so this is just the RPC send, matching entity_monster.c's onLeftClick
// pattern exactly (no local prediction).
static bool onLeftClick(struct entity* e) {
	assert(e);
	svin_rpc_send(&(struct server_rpc){
		.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = e->id,
	});
	return true;
}

void entity_painting(uint32_t id, struct entity* e, bool server, void* world,
					 int art_index, int direction, int tile_x, int tile_y,
					 int tile_z) {
	assert(e && world && art_index >= 0 && art_index < PAINTING_ART_COUNT
		  && direction >= PAINTING_DIR_NORTH && direction <= PAINTING_DIR_EAST);
	entity_default_init(e, server, world);
	// [740c583: EntityPainting(World)] Start with zero yOffset and a
	// 0.5F-wide, 0.5F-high entity -- the generic width/height/y_offset are
	// NEVER actually read for collision/hit-testing (getBoundingBox below
	// always recomputes the real art-sized frame box via
	// painting_compute_geometry); set them anyway to mirror the Beta
	// constructor and keep entity_make_local_bbox sane for anything generic
	// that might still read them.
	entity_set_size(e, 0.5f, 0.5f, 0.0f);
	e->id = id;
	e->type = ENTITY_PAINTING;
	e->name = "Painting";
	// e->health is a short on the shared struct, never touched by
	// entity_default_init, and server_entity_malloc is a raw non-zeroing
	// malloc -- left uninitialized this was a read of garbage memory
	// (post-review finding #9) every time build_entity's common NBT-write
	// prefix unconditionally serializes 'Health'. Functionally inert
	// (parse_entity never reads it back for any entity kind, and Beta's
	// EntityPainting has no health concept at all -- attackEntityFrom is an
	// unconditional instant kill) but real undefined behavior; 0 is an
	// honest, harmless placeholder.
	e->health = 0;
	e->tick_server = entity_painting_server_tick;
	// [740c583: EntityPainting.onUpdate] the whole tickCounter1 body is
	// skipped on multiplayer clients -- a real MP CLIENT's
	// onUpdate does nothing at all (position/art never change post-spawn,
	// so there is genuinely nothing for a client tick to recompute). Client
	// spawn now exists (CRPC_SPAWN_PAINTING, client_interface.c) but this
	// stays NULL on purpose, matching Beta's own no-op, not a leftover gap.
	e->tick_client = NULL;
	e->render = entity_painting_render;
	e->teleport = entity_default_teleport;
	// Post-review findings #1/#2: Beta's Entity.getBoundingBox() is null for
	// EntityPainting -- only EntityBoat/EntityMinecart override it
	// (grep-verified against the decompile pack). A non-null accessor here
	// leaked a live painting into every OTHER generic getBoundingBox
	// consumer as if it were a solid, pushable object Beta never treats it
	// as: block_place_obstructed (block/blocks.c) let it block ordinary
	// block placement through its own frame, and the piston push sweep
	// (pmove_push_entity_candidate, block/redstone_piston_server.c -- this
	// engine's real generic entity-pushing path; see the corrected
	// entity.h ENTITY_PAINTING comment) silently overwrote e->pos via
	// bottom-anchored math that is wrong for painting's center-anchored
	// frame geometry. Both consumers already gate on `!e->getBoundingBox`
	// with no per-type exception, so leaving this NULL is the complete fix
	// for both -- see entity_painting_validate_surface's own entity-overlap
	// scan for how the "another painting/minecart/boat occupies this
	// footprint" checks keep working without going through this accessor.
	e->getBoundingBox = NULL;
	e->onLeftClick = onLeftClick;
	e->leftClickText = NULL;
	e->onRightClick = NULL;
	e->rightClickText = NULL;

	e->data.painting.art_index = art_index;
	e->data.painting.direction = direction;
	e->data.painting.tile_x = tile_x;
	e->data.painting.tile_y = tile_y;
	e->data.painting.tile_z = tile_z;
	e->data.painting.tick_counter = 0;

	vec3 pos;
	painting_compute_geometry(art_index, direction, tile_x, tile_y, tile_z,
							  pos, NULL);
	e->teleport(e, pos);
	// [740c583: setDirection] Both previous and current yaw start at
	// the direction number multiplied by 90 degrees, converted to float.
	e->orient[0] = (float)(direction * 90);
	e->orient[1] = 0.0f;
	e->orient_old[0] = e->orient[0];
	e->orient_old[1] = e->orient[1];
	glm_vec3_copy(e->pos, e->network_pos);
}
