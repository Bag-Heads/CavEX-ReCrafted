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

#ifndef ENTITY_PAINTING_H
#define ENTITY_PAINTING_H

/* Post-review finding #7 (adversarial review, 2026-08-13), maintainer call
 * recorded: this module deliberately exports TWO prefixes, not the single
 * `entity_painting_*` CODESTYLE.md S1.3 otherwise mandates. `painting_*`
 * (bare) names pure geometry/data helpers with no entity-lifecycle
 * dependency -- painting_compute_geometry, painting_direction_from_side,
 * painting_art_by_title -- callable with nothing but plain ints/an art
 * index, no `struct entity` in sight. `entity_painting_*` names the
 * lifecycle half -- entity_painting_on_valid_surface, entity_painting_
 * select_art, entity_painting_drop_self, entity_painting() itself -- every
 * one of which constructs, validates, or mutates a live `struct entity`.
 * Kept as two prefixes on purpose: a maintainer who only needs the pure
 * geometry (e.g. a future renderer computing a frame box to draw, with no
 * entity yet) greps `painting_` and gets exactly that half, not the whole
 * module. The cost is that a blind `grep entity_painting_` alone misses the
 * three bare-prefixed data functions -- searching this file's declarations
 * (or `painting_.*\|entity_painting_`) finds the complete set either way. */

#include <stdbool.h>
#include <stdint.h>

#include "../block/blocks_data.h"
#include "../cglm/cglm.h"

struct entity;
struct server_local;
struct random_gen;
struct java_random;
struct AABB;

#define PAINTING_ART_COUNT 25
// Longest EnumArt title is "SkullAndRoses" (13 chars); +1 NUL fits in 16 with
// headroom, mirroring the entity.h wolf_owner[32] "fixed buffer, sized off the
// cited max" convention.
#define PAINTING_ART_TITLE_MAX 16

/* Beta EnumArt (740c583): title + PIXEL dimensions + PIXEL atlas offset.
 * blocks = pixels/16 (PAINTING_DISPATCH.md). Array index IS EnumArt's
 * declaration/values() order -- the placement selection loop below depends
 * on that order being exact. */
struct painting_art_def {
	const char name[PAINTING_ART_TITLE_MAX];
	uint8_t size_x_px;
	uint8_t size_y_px;
	uint16_t atlas_u_px;
	uint16_t atlas_v_px;
};

extern const struct painting_art_def g_painting_art[PAINTING_ART_COUNT];

/* Beta EntityPainting.direction [740c583: setDirection]: 0 faces -Z (north),
 * 1 faces -X (west), 2 faces +Z (south), 3 faces +X (east). */
enum painting_direction {
	PAINTING_DIR_NORTH = 0,
	PAINTING_DIR_WEST = 1,
	PAINTING_DIR_SOUTH = 2,
	PAINTING_DIR_EAST = 3,
};

/* engine `enum side` (blocks_data.h; FRONT=-Z, BACK=+Z, LEFT=-X, RIGHT=+X --
 * see piston_texture.c's canonical comment) -> Beta painting direction, per
 * the clicked-face switch in ItemPainting.onItemUse [740c583]. false for
 * SIDE_TOP/SIDE_BOTTOM (Beta: `if(var7==0) return false; else if(var7==1)
 * return false;` -- a painting cannot hang on a floor/ceiling face). */
bool painting_direction_from_side(enum side s, int* out_direction);

/* setDirection's pure geometry [740c583: EntityPainting.setDirection]: the
 * logical center position AND the thin world-space frame AABB for
 * `art_index` hung at `direction` on the solid block (tile_x,tile_y,tile_z).
 * Either output pointer may be NULL if the caller only needs the other. */
void painting_compute_geometry(int art_index, int direction, int tile_x,
							   int tile_y, int tile_z, vec3 out_pos,
							   struct AABB* out_bbox);

/* onValidSurface [740c583: EntityPainting.onValidSurface]: true iff
 * `art_index` fits at (direction, tile_x/y/z) -- the backing wall is solid
 * across the art's full block footprint, the frame's own thin box is clear
 * of solid blocks/an existing minecart or boat (batch-f landing note: the
 * boat-slice landed on tip after this module's own 2026-08-13 base, which
 * only had minecart to reason about -- the entity-occupancy scan was ported
 * boat-aware, matching Beta's real {EntityBoat, EntityMinecart} getBoundingBox()
 * override set, see entity_painting.c's validate_surface comment), and no
 * other painting already occupies the same footprint. `self_id` excludes one
 * live entity from the overlap sweep: UINT32_MAX for a not-yet-placed
 * candidate (never in the dict, so exclusion is moot either way, matching
 * Beta calling this before `entityJoinedWorld`), or the checked entity's OWN
 * id when re-validating an already-live painting (matching Beta's `this`
 * being excluded from its own getCollidingBoundingBoxes/
 * getEntitiesWithinAABBExcludingEntity scans). `e` needs only
 * `.world`/`.on_server` populated (entity_default_init suffices). */
bool entity_painting_on_valid_surface(struct entity* e, struct server_local* s,
									  int art_index, int direction, int tile_x,
									  int tile_y, int tile_z, uint32_t self_id);

/* The placement-time art SELECTION loop [740c583: EntityPainting(World,x,y,z,
 * dir)]: tries every EnumArt entry in enum order, collects the ones that fit,
 * then draws ONE nextInt(count) pick among them from `rng` -- I02: the NEW
 * painting's own lifetime stream (CFR EntityPainting.java:61 `this.rand`). Returns false
 * (*out_art_index untouched) when nothing fits, mirroring var6.isEmpty();
 * the caller must not spawn an entity in that case. `scratch` is a
 * caller-owned `struct entity` populated via entity_painting() (or at least
 * entity_default_init) but never inserted into `s->entities` -- exactly
 * Beta's `this` mid-construction. */
bool entity_painting_select_art(struct entity* scratch, struct server_local* s,
								int direction, int tile_x, int tile_y,
								int tile_z, struct java_random* rng,
								int* out_art_index);

/* Exact-title match with the Kebab(0) fallback [740c583:
 * EntityPainting.readEntityFromNBT: `if (this.art == null) this.art =
 * EnumArt.Kebab;`]. */
int painting_art_by_title(const char* title);

/* Beta EntityPainting(World) + setDirection, collapsed into one constructor:
 * the caller supplies the ALREADY-DECIDED art_index (from
 * entity_painting_select_art for a fresh placement, or painting_art_by_title
 * for an NBT/name-driven restore) -- this function only performs the
 * deterministic geometry + entity bookkeeping, never RNG. */
void entity_painting(uint32_t id, struct entity* e, bool server, void* world,
					 int art_index, int direction, int tile_x, int tile_y,
					 int tile_z);

/* Shared drop [740c583: onUpdate's invalid-surface branch AND
 * attackEntityFrom -- BOTH spawn exactly one `new EntityItem(worldObj, posX,
 * posY, posZ, new ItemStack(Item.painting))`; extracted per CODESTYLE S3.2
 * (second transcription)]. Spawns the item only -- does NOT remove `e` from
 * the world; the two call sites own that themselves (tick_server's
 * return-true contract vs. the attack dispatch's inline free/erase). */
void entity_painting_drop_self(struct entity* e, struct server_local* s);

/* Beta Entity.boundingBox of a painting (EntityPainting.setDirection :123,
 * the inset frame box) -- the box World.getEntitiesWithinAABBExcludingEntity
 * selects by, so explosions and lightning bolts reach a painting through
 * it. Distinct from getBoundingBox(), which Beta leaves null for paintings
 * (the collision accessor; this engine keeps e->getBoundingBox NULL for the
 * same reason, see entity_painting()). False for a non-painting. */
bool entity_painting_hit_box(const struct entity* e, struct AABB* out);

/* EntityPainting.attackEntityFrom :200-208 from a non-player source
 * (explosion, lightning): `if(!isDead) { setEntityDead(); spawn EntityItem(
 * painting) }` -- any damage breaks it. The item cannot be spawned (nor the
 * entity freed) inside the caller's entity-dict walk, so the break is
 * latched (delay_destroy = 1) and consumed at the top of the painting's own
 * next tick (drop + remove), exactly like the vehicles' blast_break_pending.
 * Idempotent (Beta's !isDead guard). */
void entity_painting_mark_broken(struct entity* e);

#endif
