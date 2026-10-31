/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

// todo: Powered rails with redstone logic -- DONE (2026-07-30, handoff P1(a): metadata
//       bit 8 + the powered-rail chain walk in block_rail.c).
// todo: Detector rails -- occupancy + redstone output DONE (CR-05: block_rail.c's
//       detector_rail_update_state / block_detector_rail_on_minecart_tick below, driven
//       from this file's minecart_server_tick). Cart TRAVERSAL already treated the
//       detector like a plain rail (no propulsion/brake to defeat -- see the RIDER
//       THROTTLE comment below); this line only ever tracked the redstone half.
// todo: Polish the movement
// todo: Bounding box behavior (not clipping through blocks and such

#include "../block/block_rail.h" // block_detector_rail_on_minecart_tick (CR-05)
#include "../block/blocks_data.h"
#include "../game/game_state.h"
#include "../network/client_interface.h"
#include "../network/server_interface.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "combat.h" /* PLAYER_EYE_HEIGHT for the player-to-cart nudge */
#include "entity.h"
#include "../platform/audio.h"
#include "../graphics/gfx_settings.h"
#include "entity_minecart.h"
#include "mob_motion.h" /* O06: Entity.moveEntity box arithmetic */
/* O06: the mob collision env (block + moving-piston boxes), declared in
 * entity_monster.h -- not included here: that header also declares a
 * non-static onLeftClick that would clash with this file's static one. */
struct mob_collide_env entity_monster_collide_env(struct server_local* s);
#include "../platform/gfx.h"
#include "../platform/texture.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../platform/displaylist.h"
#include "../graphics/render_block.h"
#include "../graphics/render_entity.h"
#include "../network/client_interface.h"
#include "../network/server_local.h"
#include "../platform/gfx.h"
#include "../item/items.h"


#define MC_EPS 0.02f
/* Beta never hard-stops ordinary rail coasting. CaveWii snaps only this
 * sub-threshold tail so parked carts can leave the Wii's active simulation /
 * correction rotation. At 1e-5 this is 1000x below Beta's smallest live rail
 * decision threshold (0.01); see docs/deviations.md. */
#define MINECART_TERMINAL_SPEED 1e-5f

static inline float tile_center(float v) {
    // Center of current tile
    return floorf(v) + 0.5f;
}

static inline int is_near_center_axis(float pos, int axis_is_x) {
    float c = axis_is_x ? tile_center(pos) : tile_center(pos);
    (void)c; // silence -O2 warning
    return fabsf((axis_is_x ? pos : pos) - (axis_is_x ? tile_center(pos) : tile_center(pos))) < MC_EPS;
}

static inline int is_near_center2(float x, float z) {
    return fabsf(x - tile_center(x)) < MC_EPS && fabsf(z - tile_center(z)) < MC_EPS;
}

// Turn table for curved rails (6..9) based on *effective* incoming dir (dx,dz).
// Meta meaning (MC Beta 1.7.3):
// 6: SE (connects South(+z) & East(+x))
// 7: SW (connects South(+z) & West(-x))
// 8: NW (connects North(-z) & West(-x))
// 9: NE (connects North(-z) & East(+x))
static inline void curve_turn(uint8_t meta, int in_dx, int in_dz, int *out_dx, int *out_dz) {
    *out_dx = in_dx; *out_dz = in_dz; // default: no change (safety)
    switch (meta) {
        case 6: // SE
            if (in_dz == -1) { *out_dx = +1; *out_dz = 0; }   // N -> E
            else if (in_dx == -1) { *out_dx = 0; *out_dz = +1; } // W -> S
            break;
        case 7: // SW
            if (in_dz == -1) { *out_dx = -1; *out_dz = 0; }   // N -> W
            else if (in_dx == +1) { *out_dx = 0; *out_dz = +1; } // E -> S
            break;
        case 8: // NW
            if (in_dz == +1) { *out_dx = -1; *out_dz = 0; }   // S -> W
            else if (in_dx == +1) { *out_dx = 0; *out_dz = -1; } // E -> N
            break;
        case 9: // NE
            if (in_dz == +1) { *out_dx = +1; *out_dz = 0; }   // S -> E
            else if (in_dx == -1) { *out_dx = 0; *out_dz = -1; } // W -> N
            break;
        default: break;
    }
}

// Seed initial heading (hx,hz) on a tile given metadata and current speed sign.
// Returns (hx,hz) with |hx|+|hz|==1. Straight/slopes pick axis; curves pick axis
// towards the tile center to avoid ambiguity when spawning on a curve.
/* The heading is the rail's AXIS as a unit vector -- it must NOT carry the sign
 * of the speed. `speed` is signed and the caller already applies sgn(speed) to
 * this heading every tick, so baking sgn in here applied it TWICE: sgn*sgn is
 * always +1, and a freshly-seeded cart therefore always departed +Z (straight
 * NS) or +X (straight EW) no matter which way the player pushed it. That is the
 * "minecarts only go in one direction" report of 2026-07-31; `spd` is now unused
 * for straight track and only picks the curve's entry side. */
static inline void seed_heading(uint8_t meta, float px, float pz, float spd, int8_t *hx, int8_t *hz) {
    (void)spd;
    switch (meta) {
        case 0: case 4: case 5:  *hx = 0;     *hz = +1;  break; // NS (lock X)
        case 1: case 2: case 3:  *hx = +1;    *hz = 0;   break; // EW (lock Z)
        case 6: case 7: case 8: case 9: {
            /* Curve: head toward the tile centre on the dominant axis. This is a
             * GEOMETRIC direction, so it keeps its own sign and still must not be
             * multiplied by sgn(speed). */
            float cx = tile_center(px), cz = tile_center(pz);
            float dx = cx - px, dz = cz - pz;
            if (fabsf(dx) > fabsf(dz)) { *hx = (dx > 0 ? +1 : -1); *hz = 0; }
            else { *hx = 0; *hz = (dz > 0 ? +1 : -1); }
            break;
        }
        default: *hx = 0; *hz = +1; break;
    }
}

// Try to find a rail at (bx, by, bz); if not found, also try by+1 and by-1.
// Returns 1 and fills *out/*out_by when a rail is found, else 0.
//
// BOUNDARY FIX (M1c): reads go through entity_get_block -- the dispatcher
// that routes a SERVER entity to server_world_get_block(e->world == &s->world)
// and a client entity to world_get_block. The old code read gstate.world (the
// CLIENT world) directly from the SERVER tick thread while clin_update mutates
// those sections every frame: a cross-thread torn read / use-after-free. Same
// pattern as every entity_monster.c server tick. An unloaded server chunk now
// just returns no-rail (cart stops) instead of reading foreign memory.
static inline int get_rail_with_y_fallback(struct entity* e, int bx, int by,
                                           int bz, struct block_data* out,
                                           int* out_by) {
    static const int dy[3] = {0, 1, -1};
    for(int k = 0; k < 3; k++) {
        struct block_data b;
        if(entity_get_block(e, bx, by + dy[k], bz, &b)
           && (b.type == BLOCK_RAIL || b.type == BLOCK_POWERED_RAIL
               || b.type == BLOCK_DETECTOR_RAIL)) {
            *out = b;
            *out_by = by + dy[k];
            return 1;
        }
    }
    return 0;
}

static inline bool minecart_is_rail_type(uint8_t type) {
    return type == BLOCK_RAIL || type == BLOCK_POWERED_RAIL
        || type == BLOCK_DETECTOR_RAIL;
}

/* G1: Beta's own rail probe [EntityMinecart.java:212-219]:
 *     by = floor(posY);
 *     a rail at (x, by - 1, z) lowers by by one;
 *     then a rail block id at (x, by, z) means on rail.
 * ONE BELOW first, then the feet cell, never above. by is floor(posY). */
static inline bool get_rail_beta_probe(struct entity* e, int bx, int by,
                                       int bz, struct block_data* out,
                                       int* out_by) {
    struct block_data b;
    if (entity_get_block(e, bx, by - 1, bz, &b) && minecart_is_rail_type(b.type)) {
        *out = b;
        *out_by = by - 1;
        return true;
    }
    if (entity_get_block(e, bx, by, bz, &b) && minecart_is_rail_type(b.type)) {
        *out = b;
        *out_by = by;
        return true;
    }
    return false;
}

// Absolute height on a slope tile (linear, center-based).
// by is the chosen rail-block Y (after fallback), cx/cz are centers of the current tile.
// current_y is used for non-slope metas.
//
// WHICH SIDE IS HIGH. Cases 2 and 3 were INVERTED: a cart on a real
// ascending-east ramp descended as it drove east, sinking through the track it
// was supposed to be climbing. Four independent sources fix the convention, and
// all four agree that shape 2's high side is +X:
//
//   1. Beta assigns the shape by probing for a rail one block UP in a
//      direction -- if it finds one, the track ascends THAT way
//      [740c583 RailLogic.refreshTrackShape:312-330]:
//          rail at (x, y+1, z-1) -> shape 4;           // ascends -Z
//          rail at (x, y+1, z+1) -> shape 5;           // ascends +Z
//          rail at (x+1, y+1, z) -> shape 2;           // ascends +X
//          rail at (x-1, y+1, z) -> shape 3;           // ascends -X
//   2. Beta's gravity pushes DOWNHILL, i.e. away from those high sides
//      [740c583 EntityMinecart.onUpdate:232-246]: shape 2 -> motionX -= 1/128.
//   3. Beta's MATRIX rows mark the low end with dy -1: shape 2 is
//      {{-1,-1,0},{1,0,0}} -- the -X end is the low one.
//   4. THIS engine's own placement path already agreed with Beta and only the
//      consumer disagreed: block_rail.c:129-132 returns 2 for a higher rail to
//      the east, and block_rail.c:216-227 promotes a lower rail to the WEST of
//      a new rail into shape 2 (`if (lower_w) updateRailToSlope(..., 2)`).
//
// Shapes 4 and 5 were already correct and are unchanged. Guarded end to end by
// tests/test_minecart_slope_beta.c, which builds each ramp through the real
// placement path rather than hand-writing the metadata -- so the fixture cannot
// silently agree with a wrong consumer.
static inline float slope_height(uint8_t meta, int by,
                                 float x, float z, float cx, float cz,
                                 float current_y)
{
    switch (meta) {
        case 2: /* HIGH side EAST  (+X) */ return (float)by + 0.5f + (x - cx);
        case 3: /* HIGH side WEST  (-X) */ return (float)by + 0.5f - (x - cx);
        case 4: /* HIGH side NORTH (-Z) */ return (float)by + 0.5f - (z - cz);
        case 5: /* HIGH side SOUTH (+Z) */ return (float)by + 0.5f + (z - cz);
        default: return current_y; // non-slope
    }
}
// Keep comments in English per project style.
// Keep comments in English per project style.
/* Beta's per-entity `rand` (Entity.java:50 `protected Random rand = new
 * Random();`), never World.rand: the furnace-cart fuel roll
 * [EntityMinecart.java:485 `this.rand.nextInt(4)`] draws this private server
 * stream so s->rand_src stays aligned to Beta's world-stream draw count
 * (CODESTYLE 2.3; next-gaps-0924 W3-s2). This is still the SHARED server
 * s->entity_rng (also used by arrow unstick rolls), not a per-cart Java
 * Random; dropped items moved to their own gameplay_rng (item-rng-1003).
 * Seeded in server_local_create; the lazy guard only keeps hand-built
 * fixtures out of the absorbing zero state without borrowing a draw. */
#define MINECART_ENTITY_RNG_FALLBACK_SEED 0x6C8E9CF5u
static struct random_gen* minecart_entity_rng(struct server_local* s) {
	if(s->entity_rng.seed == 0)
		s->entity_rng.seed = MINECART_ENTITY_RNG_FALLBACK_SEED;
	return &s->entity_rng;
}

/* O06 (KANBAN Beta-parity card "minecart rail travel collision").
 * Beta does NOT add the rail displacement to the position: after projecting
 * the cart onto the rail line it calls
 *     setPosition(posX, posY + yOffset, posZ);            EntityMinecart:297
 *     ... dx/dz = motion (*0.75 ridden), clamped +-0.4      :298-315
 *     moveEntity(dx, 0.0D, dz);                            EntityMinecart:316
 * and Entity.moveEntity (Entity.java:257-) clips that against
 * World.getCollidingBoundingBoxes(boundingBox.addCoord(dx, dy, dz)):
 * Y first, then X (calculateXOffset), then Z (calculateZOffset); with
 * field_9077_F = true (Entity.java:80) a clipped axis leaves the others
 * alone; stepHeight is 0 (Entity.java:93), so a cart never steps; the new
 * position is the box centre, and motionX / motionZ are zeroed on a clipped
 * axis. The move box is the one setPosition built at :297 (Entity.java:154-
 * 161): half-width (double)(0.98F / 2.0F) around the rail-snapped X/Z, minY =
 * the rail block (+1 on slopes 2-5, :233-235 posY = rail y + 1), height
 * (double)0.7F. Without this the engine ran a cart straight into a solid end
 * stop, off its rail, and stuck it there forever (PC vehicle smoke
 * vs_pc_cart_20261004_091553: cart at z = 3.079 inside the az+3 stop).
 *
 * BLOCK half only (plus moving-piston boxes, as for mobs). The ENTITY half
 * (EntityMinecart.func_89_d :77-79 returns every other entity's box) and the
 * cart-cart applyEntityCollision (:441-447) do not exist in this engine and
 * are a separate card; the client-prediction re-entry (s == NULL) clips
 * against its own world's blocks only.
 *
 * Returns MINECART_RAIL_FREE when nothing clips (the caller keeps its exact
 * pre-fix float add, byte-identical), MINECART_RAIL_CLIPPED with *new_axis_pos
 * = Beta's box-centre position, or MINECART_RAIL_INCOMPLETE when the box query
 * ran out of memory (the move is skipped, never made wrong -- mob precedent). */
enum minecart_rail_move {
	MINECART_RAIL_FREE,
	MINECART_RAIL_CLIPPED,
	MINECART_RAIL_INCOMPLETE,
};
static enum minecart_rail_move minecart_rail_clip(struct entity* e,
		struct server_local* s, int rail_y, bool slope, int axis,
		float amount, float* new_axis_pos) {
	if (amount == 0.0f)
		return MINECART_RAIL_FREE;
	const double half = (double)(0.98F / 2.0F);
	const double min_y = (double)rail_y + (slope ? 1.0 : 0.0);
	struct mob_aabb_d bb = {
		{(double)e->pos[0] - half, min_y, (double)e->pos[2] - half},
		{(double)e->pos[0] + half, min_y + (double)0.7F,
		 (double)e->pos[2] + half}};
	const double d0 = (double)amount;
	struct mob_aabb_d q = mob_aabb_add_coord(&bb, axis == 0 ? d0 : 0.0, 0.0,
											 axis == 2 ? d0 : 0.0);
	struct mob_collide_env env = {0};
	if (s) {
		env = entity_monster_collide_env(s);
		env.entity_boxes = NULL; /* block half only: see above */
		env.entity_ctx = NULL;
	}
	struct mob_box_list list = {0};
	mob_colliding_boxes(e, &q, s ? &env : NULL, &list);
	if (list.oom) {
		mob_box_list_free(&list);
		return MINECART_RAIL_INCOMPLETE;
	}
	/* dy = 0: every calculateYOffset returns 0, and the other horizontal axis
	 * moves 0 -- only this axis can clip. */
	double d = d0;
	for (size_t i = 0; i < list.count; i++)
		d = axis == 0 ? mob_aabb_calc_x_offset(&list.boxes[i], &bb, d)
					  : mob_aabb_calc_z_offset(&list.boxes[i], &bb, d);
	mob_box_list_free(&list);
	if (d == d0)
		return MINECART_RAIL_FREE;
	bb.min[axis] += d;
	bb.max[axis] += d;
	/* O08 / G4: Beta keeps the clipped box itself (double), so a cart at
	 * rest against the face still clips the next push into it (calcZOffset
	 * needs maxZ <= the wall's minZ). This engine stores the centre as a
	 * float and re-derives the box every tick; plain rounding can land a
	 * hair PAST the face (4.51 -> 4.51000023, box max 5.0000002 > 5), the
	 * next push then skips the clip and tunnels the cart into the stop
	 * (a dismounted rider walking off a cart parked at the stop shoved it
	 * 0.71 into the stone, off its rail). Take the float nearest Beta's
	 * centre whose re-derived box stays on Beta's side of the clipped
	 * face -- at most one ulp from the rounded value. */
	float np = (float)((bb.min[axis] + bb.max[axis]) / 2.0);
	if (d0 > 0.0) {
		while ((double)np + half > bb.max[axis])
			np = nextafterf(np, -INFINITY);
	} else {
		while ((double)np - half < bb.min[axis])
			np = nextafterf(np, INFINITY);
	}
	*new_axis_pos = np;
	return MINECART_RAIL_CLIPPED;
}

/* PLAYER-TO-CART NUDGE, factored out of minecart_server_tick by G1 so the
 * off-rail branch runs the same sweep (Beta's applyEntityCollision does
 * not care whether the cart is on a rail). The on-rail body is moved
 * verbatim; see the comment at its call site for the Beta citation. */
static void minecart_player_nudge(struct entity* e, struct server_local* s,
                                  uint8_t meta, bool on_rail) {
    struct server_world* cart_world = s ? server_local_entity_world(s, e) : NULL;
    for (unsigned body_i = 0; s && cart_world && body_i < 2; body_i++) {
        struct server_player* body = server_local_damage_candidate(s, body_i);
        if (!body || !body->has_pos || !body->finished_loading
			|| server_transfer_actor_inert(s, body))
            continue;
        /* A2-E (M6): the cart's world is its resolved (slot, generation)
         * stamp, not the raw pointer (slot storage is reused by a successor
         * world); resolved once per tick, above the body loop. */
        struct server_world* body_world = server_local_world_of(s, body);
        if (!body_world || body_world != cart_world)
            continue;
        /* var1 == riddenByEntity, by rider identity: the occupant id names
         * the body's own entity id (host SERVER_LOCAL_PLAYER_ENTITY_ID, guest
         * s->guest_entity_id). */
        uint32_t body_id = body == &s->player ? SERVER_LOCAL_PLAYER_ENTITY_ID
                                              : s->guest_entity_id;
        if (e->data.minecart.occupied
            && e->data.minecart.occupant_id == body_id)
            continue;
        /* The sweep box, as Beta builds it (Entity.setPosition:126-133,
         * strict AxisAlignedBB.intersectsWith): the player box half-width
         * (double)0.3F, minY = posY - (double)1.62F, height (double)1.8F,
         * expanded (double)0.2F on X/Z (EntityLiving.java:643); the cart box
         * half-width (double)0.49F (setSize 0.98F) and centred vertically
         * on posY: yOffset = height / 2 = 0.35F, so [posY - 0.35F,
         * posY - 0.35F + 0.7F]. */
        double pfx = body->x, pfz = body->z;
        double p_min_y = body->y - (double)PLAYER_EYE_HEIGHT; /* feet */
        double p_max_y = p_min_y + (double)1.8F;
        double c_min_y = (double)e->pos[1] - (double)0.35F;
        double c_max_y = c_min_y + (double)0.7F;
        double reach = (double)0.3F + (double)0.2F;
        bool xz_overlap = pfx + reach > (double)e->pos[0] - (double)0.49F
            && pfx - reach < (double)e->pos[0] + (double)0.49F
            && pfz + reach > (double)e->pos[2] - (double)0.49F
            && pfz - reach < (double)e->pos[2] + (double)0.49F;
        bool y_overlap = p_max_y > c_min_y && p_min_y < c_max_y;
        if (!(xz_overlap && y_overlap))
            continue;
        double dx = pfx - e->pos[0];
        double dz = pfz - e->pos[2];
        double d2 = dx * dx + dz * dz;
        if (d2 < 9.999999747378752e-5) /* (double)1.0E-4F */
            continue;
        double d = (double)(float)sqrt(d2); /* MathHelper.sqrt_double */
        dx /= d;
        dz /= d;
        double f = 1.0 / d;
        if (f > 1.0)
            f = 1.0;
        dx *= f;
        dz *= f;
        dx *= (double)0.1F;
        dz *= (double)0.1F;
        dx *= 0.5;
        dz *= 0.5;
        /* Cart: -d, projected on the heading (seed one if parked -- the
         * slope-kick precedent; pick the axis from the rail shape, sign from
         * the push itself). */
        /* O21 (lead ruling, Beta-first): Beta's cart.addVelocity(-d)
         * [EntityMinecart.java:612] lands on the motion VECTOR, on or off the
         * rail. On a rail the caller runs this BEFORE the fold point, so
         * Beta's :255-264 projection keeps the full magnitude |d| (signed by
         * the dot with the rail axis) the same tick -- a perpendicular shove
         * starts a parked cart, as in Beta. Before O21 the on-rail shove
         * kept only d.h (the heading component). */
        (void)on_rail;
        (void)meta;
        if (dx != 0.0 || dz != 0.0) {
            e->vel[0] += (float)(-dx);
            e->vel[2] += (float)(-dz);
            entity_minecart_mark_snapshot_dirty(e, false);
        }
        /* Body: +d/4 through the server knockback velocity, unless seated. */
        if (!body->riding_minecart && !body->riding_boat) {
            /* G8: delivered to the body's client as an additive velocity. */
            server_local_push_actor_velocity(s, body, (float)(dx / 4.0),
                                             (float)(dz / 4.0));
        }
    }

}

/* G1 (vehicle-smoke-1004; ASTRA-CART-RULING item 5): Beta 1.7.3's OFF-RAIL
 * branch. Before G1 a cart with no rail under it simply stopped (speed = 0,
 * return) -- frozen in place, even in mid-air, after rolling off an open rail
 * end. Beta instead runs, every tick [CFR server decompile
 * net/minecraft/src/EntityMinecart.java]:
 *     motionY -= (double)0.04f;                          :211 (EVERY tick)
 *     clamp motionX, motionZ to [-0.4, 0.4]             :398-409 (not Y)
 *     if (onGround) motion *= 0.5;                       :410-414 (PREVIOUS
 *                                                          move's onGround)
 *     moveEntity(motionX, motionY, motionZ);             :415
 *     if (!onGround) motion *= (double)0.95f;            :416-420 (THIS move's)
 * Entity.moveEntity clips Y, then X, then Z; a clipped axis zeroes its motion
 * and onGround = (Y clipped && dy < 0); stepHeight is 0. entity_try_move
 * (entity.c) is exactly that per axis, so it is reused in Beta's Y, X, Z order
 * (the entity_item precedent).
 *
 * ENGINE MAPPING. On a rail the cart's motion is the signed scalar `speed`
 * along the heading (hx, hz); off the rail Beta's motion vector lives in
 * e->vel (blocks/tick) and Beta's onGround in e->on_ground -- both already
 * networked in the cart snapshot and saved as Motion/OnGround. On the tick a
 * cart leaves the rail its motion is speed * h with motionY 0 (Beta's on-rail
 * tail zeroes it); landing back on a rail folds e->vel into `speed` through
 * Beta's axis projection (the fold_motion block in minecart_server_tick).
 * Positions are floats, Beta's doubles: tests/test_minecart_offrail_golden.c
 * compares against Beta's double recurrence within 1e-4.
 *
 * Shared by the server tick and the client-prediction re-entry (s == NULL),
 * which runs the same physics on its own world, exactly like the rail path;
 * the server snapshot stays authoritative. */
static bool minecart_offrail_tick(struct entity* e, struct server_local* s) {
    if (e->data.minecart.speed != 0.0f) {
        e->vel[0] += e->data.minecart.speed * (float)e->data.minecart.hx;
        e->vel[2] += e->data.minecart.speed * (float)e->data.minecart.hz;
        e->vel[1] = 0.0f;
        e->data.minecart.speed = 0.0f;
        if (s)
            entity_minecart_mark_snapshot_dirty(e, false);
    }
    /* Rider throttle is the on-rail carve-out only: Beta couples no rider
     * input into a cart, and off the rail neither does this engine. */
    e->data.minecart.pending_throttle = 0;
    /* No rail under the cart: the next probe uses Beta's basis (see the probe
     * comment in minecart_server_tick). The heading is kept for the renderer
     * and is re-derived from the motion on landing. */
    e->data.minecart.rail_meta = 0xFF;
    e->data.minecart.last_meta = 0xFF;

    minecart_player_nudge(e, s, 0xFF, false);

    const bool was_on_ground = e->on_ground;
    e->vel[1] -= 0.04f;                                     /* :211 */
    for (int k = 0; k < 3; k += 2) {                         /* :398-409 */
        if (e->vel[k] < -0.4f) e->vel[k] = -0.4f;
        if (e->vel[k] > 0.4f) e->vel[k] = 0.4f;
    }
    if (was_on_ground) {                                     /* :410-414 */
        e->vel[0] *= 0.5f;
        e->vel[1] *= 0.5f;
        e->vel[2] *= 0.5f;
    }
    struct AABB bbox;                                        /* :415 */
    entity_make_local_bbox(e, &bbox);
    bool collision_xz = false;
    static const size_t order[3] = {1, 0, 2};
    for (int k = 0; k < 3; k++)
        entity_try_move(e, e->pos, e->vel, &bbox, order[k], &collision_xz,
                        &e->on_ground);
    e->collided_horizontally = collision_xz;
    if (!e->on_ground) {                                     /* :416-420 */
        e->vel[0] *= 0.95f;
        e->vel[1] *= 0.95f;
        e->vel[2] *= 0.95f;
    }
    /* Keep the background correction rotation running while the cart is
     * actually moving or its ground state flipped; a cart at rest off the
     * rail leaves it, like a parked rail cart. */
    if (s && (e->pos[0] != e->pos_old[0] || e->pos[1] != e->pos_old[1]
              || e->pos[2] != e->pos_old[2]
              || e->on_ground != was_on_ground))
        entity_minecart_mark_snapshot_dirty(e, false);
    return false;
}

/* ---- G2: Beta minecart collision (vehicle-smoke-1004) --------------------
 * A cart's Beta motion vector is speed * heading (on a rail) plus e->vel (off
 * the rail, or an impulse not yet folded -- G1's fold_motion projects it onto
 * the rail axis at the start of the next rail tick, exactly where Beta's
 * :255-264 projection would see the added motion). So:
 *   motionX/Z                  -> speed*h + vel
 *   motion *= k                -> speed *= k, vel *= k
 *   addVelocity(dx, 0, dz)     -> vel += d                                  */
static void cart_motion(const struct entity* e, double* mx, double* mz) {
	*mx = (double)(e->data.minecart.speed * (float)e->data.minecart.hx)
		+ (double)e->vel[0];
	*mz = (double)(e->data.minecart.speed * (float)e->data.minecart.hz)
		+ (double)e->vel[2];
}
static void cart_scale_motion(struct entity* e, double k) {
	e->data.minecart.speed = (float)((double)e->data.minecart.speed * k);
	e->vel[0] = (float)((double)e->vel[0] * k);
	e->vel[2] = (float)((double)e->vel[2] * k);
}
static void cart_add_velocity(struct entity* e, double dx, double dz) {
	e->vel[0] = (float)((double)e->vel[0] + dx);
	e->vel[2] = (float)((double)e->vel[2] + dz);
}

/* The shared head of EntityMinecart.applyEntityCollision [:556-581]: the
 * push d from THIS cart (rx, rz) toward var1 (mx, mz). False below
 * (double)1.0E-4F squared distance. */
static bool cart_collision_push(double rx, double rz, double mx, double mz,
								double* d2, double* d4) {
	double v2 = mx - rx, v4 = mz - rz;
	double v6 = v2 * v2 + v4 * v4;
	if(!(v6 >= (double)1.0E-4F))
		return false;
	v6 = (double)(float)sqrt(v6); /* MathHelper.sqrt_double -> float */
	v2 /= v6;
	v4 /= v6;
	double v8 = 1.0 / v6;
	if(v8 > 1.0)
		v8 = 1.0;
	v2 *= v8;
	v4 *= v8;
	v2 *= (double)0.1F;
	v4 *= (double)0.1F;
	v2 *= (double)(1.0F - 0.0F); /* entityCollisionReduction is 0 */
	v4 *= (double)(1.0F - 0.0F);
	*d2 = v2 * 0.5;
	*d4 = v4 * 0.5;
	return true;
}

/* recv.applyEntityCollision(mover), both carts [:582-611]. */
static void cart_receive_cart(struct entity* recv, struct entity* mover) {
	double v2, v4;
	if(!cart_collision_push(recv->pos[0], recv->pos[2], mover->pos[0],
							mover->pos[2], &v2, &v4))
		return;
	double mmx, mmz, rmx, rmz;
	cart_motion(mover, &mmx, &mmz);
	cart_motion(recv, &rmx, &rmz);
	/* Beta's own quirk, verbatim: var1.prevPosX where motionX would be
	 * expected. prevPosX is the mover's position at its tick start, which
	 * this engine keeps in pos_old (set at the top of the cart tick). */
	double v10 = (double)mover->pos[0] - (double)recv->pos[0];
	double v12 = (double)mover->pos[2] - (double)recv->pos[2];
	double v14 = v10 * mmz + v12 * (double)mover->pos_old[0];
	if(v14 * v14 > 5.0)
		return;
	double v16 = mmx + rmx, v18 = mmz + rmz;
	const bool mover_furnace = mover->data.minecart.minecart_type == 2;
	const bool recv_furnace = recv->data.minecart.minecart_type == 2;
	if(mover_furnace && !recv_furnace) {
		cart_scale_motion(recv, (double)0.2F);
		cart_add_velocity(recv, mmx - v2, mmz - v4);
		cart_scale_motion(mover, (double)0.7F);
	} else if(!mover_furnace && recv_furnace) {
		cart_scale_motion(mover, (double)0.2F);
		cart_add_velocity(mover, rmx + v2, rmz + v4);
		cart_scale_motion(recv, (double)0.7F);
	} else {
		cart_scale_motion(recv, (double)0.2F);
		cart_add_velocity(recv, v16 / 2.0 - v2, v18 / 2.0 - v4);
		cart_scale_motion(mover, (double)0.2F);
		cart_add_velocity(mover, v16 / 2.0 + v2, v18 / 2.0 + v4);
	}
	entity_minecart_mark_snapshot_dirty(recv, false);
	entity_minecart_mark_snapshot_dirty(mover, false);
}

static bool g2_boxes_intersect(const double a[6], const double b[6]) {
	/* AxisAlignedBB.intersectsWith: strict on every axis */
	return a[3] > b[0] && a[0] < b[3] && a[4] > b[1] && a[1] < b[4]
		&& a[5] > b[2] && a[2] < b[5];
}
static void g2_entity_box(const struct entity* e, double x, double y,
						  double z, double out[6]) {
	const double half = (double)(e->width / 2.0F);
	out[0] = x - half;
	out[3] = x + half;
	out[2] = z - half;
	out[5] = z + half;
	out[1] = y - (double)e->y_offset + (double)e->y_size;
	out[4] = out[1] + (double)e->height;
}

/* G2: Beta's two cart collision sources, run from the server cart tick after
 * the move (EntityMinecart.onUpdate :441-447 is the last thing it does to
 * motion):
 *  1. the cart's own sweep: every OTHER minecart in boundingBox.expand(0.2F,
 *     0, 0.2F) gets other.applyEntityCollision(this) (cart <-> cart);
 *  2. the living half: Beta runs it from each mob's EntityLiving.
 *     onLivingUpdate sweep [:641-651], calling cart.applyEntityCollision(mob)
 *     -> cart.addVelocity(-d), mob.addVelocity(d/4) [:612-613]. This engine
 *     has no per-mob hook in lane V's files (entity.c belongs to another
 *     lane), so the cart runs the same pairing over the mobs whose expanded
 *     box meets it -- the same placement precedent as the player nudge
 *     (minecart_player_nudge), one tick-order shift recorded in deviations.md.
 * NOT modelled (recorded): Beta's applyEntityCollision also MOUNTS a
 * non-player living entity onto an empty, moving (motion^2 > 0.01) type-0
 * cart [:556-558]; this engine has no mob-rides-cart state. */
void entity_minecart_collide_neighbours(struct server_local* s,
										struct entity* cart) {
	if(!s || !cart || cart->type != ENTITY_MINECART || cart->delay_destroy == 0)
		return;
	struct server_world* world = server_local_entity_world(s, cart);
	if(!world)
		return;
	const double ex = (double)0.2F;
	double cart_box[6];
	g2_entity_box(cart, cart->pos[0], cart->pos[1], cart->pos[2], cart_box);
	double sweep[6] = {cart_box[0] - ex, cart_box[1], cart_box[2] - ex,
					   cart_box[3] + ex, cart_box[4], cart_box[5] + ex};

	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* o = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(!o || o == cart || o->delay_destroy == 0
		   || server_local_entity_world(s, o) != world)
			continue;
		if(o->type == ENTITY_MINECART) {
			double ob[6];
			g2_entity_box(o, o->pos[0], o->pos[1], o->pos[2], ob);
			if(g2_boxes_intersect(sweep, ob))
				cart_receive_cart(o, cart);
		} else if(o->type == ENTITY_MONSTER) {
			struct mob_motion* md = &o->data.monster.motion_d;
			double mx = md->valid ? md->pos[0] : (double)o->pos[0];
			double my = md->valid ? md->pos[1] : (double)o->pos[1];
			double mz = md->valid ? md->pos[2] : (double)o->pos[2];
			double mb[6];
			g2_entity_box(o, mx, my, mz, mb);
			double msweep[6] = {mb[0] - ex, mb[1], mb[2] - ex,
								mb[3] + ex, mb[4], mb[5] + ex};
			if(!g2_boxes_intersect(msweep, cart_box))
				continue;
			/* `var1 != this.riddenByEntity`: only players ride carts here */
			double v2, v4;
			if(!cart_collision_push(cart->pos[0], cart->pos[2], mx, mz, &v2,
									&v4))
				continue;
			cart_add_velocity(cart, -v2, -v4);
			mob_motion_add_velocity(o, md, v2 / 4.0, 0.0, v4 / 4.0);
			entity_minecart_mark_snapshot_dirty(cart, false);
		}
	}
}

static bool minecart_server_tick_body(struct entity* e, struct server_local* s) {
    // Boundary invariant (M1c): a SERVER-ticked cart must carry the server
    // world; trips the asserts-on suite/PC build immediately if a cart is
    // ever constructed against the client world again. (s == NULL is the
    // client_tick re-entry, which runs with the CLIENT cart/world -- legal.)
    // A2-E (M6): "the server world" is now the cart's RESOLVED world -- a live
    // world of this session its (slot, generation) stamp names, bound as
    // e->world -- rather than the deprecated slot-0 alias; identical to the
    // old `e->world == &s->world` while only slot 0 is live.
    assert(!s || (e->on_server && server_local_entity_world(s, e) != NULL
                  && (const void*)server_local_entity_world(s, e) == e->world));

    /* Explosion entity pass latch (server_world.c): the blast pushed
     * damage_taken past 40 while the entity dict was being iterated. Beta's
     * attackEntityFrom ran dismount + drops + setEntityDead right there; the
     * removal is honoured by the world's list sweep either way, so the same
     * break runs at the top of the cart's own next tick and this tick asks
     * the loop to remove it. [740c583 EntityMinecart.attackEntityFrom:73-116] */
    if(s && (e->data.minecart.blast_break_pending
             || e->data.minecart.damage_taken > 40)) {
        server_local_minecart_break_drops(s, e);
        return true;
    }

    /* A moving/controlled server cart remains in the background correction
     * rotation until its stored speed is exact zero. This also covers the
     * final tick when the generic snap below or an earlier Beta brake turns a
     * sub-threshold residual into the terminal state. */
    if(s && (e->data.minecart.pending_throttle != 0
             || e->data.minecart.speed != 0.0f))
        entity_minecart_mark_snapshot_dirty(e, false);

    // Provide a stable previous for interpolation in this sim step.
    glm_vec3_copy(e->pos, e->pos_old);

    // Beta onUpdate:155-161: hit shake and accumulated damage both bleed off
    // 1/tick, so spaced punches never break a cart but ~5 quick ones do.
    if(e->data.minecart.shake_ticks > 0)
        e->data.minecart.shake_ticks--;
    if(e->data.minecart.damage_taken > 0)
        e->data.minecart.damage_taken--;

    // Find base block coords under cart
    int bx = (int)floorf(e->pos[0]);
    int bz = (int)floorf(e->pos[2]);

    /* G1 (vehicle-smoke-1004): the rail probe. Beta probes the cell ONE BELOW
     * floor(posY) first, then floor(posY) itself, and never looks up
     * [EntityMinecart.java:212-215: `if (isRail(x, (var2 = floor(posY)) - 1,
     * z)) --var2;`]. A cart on a flat rail, a curve, or off the rail keeps
     * Beta's posY basis here (flat track centre = railY + 0.5), so it gets
     * exactly that probe. A cart that was on a SLOPE last tick carries this
     * engine's slope_height basis, which sits 0.5 BELOW Beta's (posY =
     * railY + 1 on shapes 2-5, :233-235); for that basis only, the older
     * floor(y - 0.1) probe with its +1 fallback stands in for Beta's higher
     * posY at slope seams (climbing onto the upper flat, low half of a ramp)
     * and stays byte-identical. Before G1 the +1 fallback ran for EVERY cart,
     * so a cart under a rail one cell above its feet read as on-rail. */
    const uint8_t prev_shape = e->data.minecart.last_meta;
    const bool slope_basis = prev_shape >= 2 && prev_shape <= 5;
    struct block_data below;
    int by_guess, rail_y;
    bool on_rail;
    if (slope_basis) {
        by_guess = (int)floorf(e->pos[1] - 0.1f);
        rail_y = by_guess;
        on_rail = get_rail_with_y_fallback(e, bx, by_guess, bz, &below,
                                           &rail_y) != 0;
    } else {
        by_guess = (int)floorf(e->pos[1]);
        rail_y = by_guess;
        on_rail = get_rail_beta_probe(e, bx, by_guess, bz, &below, &rail_y);
    }
    if (!on_rail)
        return minecart_offrail_tick(e, s);

    /* G1: Beta's on-rail tail zeroes motionY every tick (:322-326 ridden,
     * :345-347 unridden), and moveEntity(dx, 0, dz) on the rail leaves
     * onGround false (Entity.java: `onGround = var13 != dy && var13 < 0`
     * with dy = 0). An off-rail cart that just landed on a rail, or a rail
     * cart an explosion pushed (server_world.c adds the blast into e->vel),
     * still carries world motion in e->vel: it is folded into the scalar
     * rail model below, right where Beta projects its motion vector onto
     * the rail axis (:255-264). */
    /* O21: the player-to-cart nudge (Beta: the PLAYER's applyEntityCollision
     * sweep adds -d to this cart's motion) lands in e->vel here, so the
     * projection below folds it with the full magnitude this tick. */
    minecart_player_nudge(e, s, 0xFF, true);
    const bool fold_motion = e->vel[0] != 0.0f || e->vel[2] != 0.0f;
    e->vel[1] = 0.0f;
    e->on_ground = false;

    // Stable centers for THIS tile (prevents micro drift across frames)
    float cx = (float)bx + 0.5f;
    float cz = (float)bz + 0.5f;

    uint8_t meta = below.metadata & 0xF;

    // Powered AND detector rails keep their redstone state in bit 8; Beta
    // strips it before ANY shape logic ("if getIsPowered() var11 &= 7" --
    // getIsPowered is true for BOTH ids), so a powered straight (raw meta 8/9)
    // must not be mistaken for a curve tile below.
    // [740c583: EntityMinecart.onUpdate; BlockRail.getIsPowered]
    if (below.type == BLOCK_POWERED_RAIL || below.type == BLOCK_DETECTOR_RAIL)
        meta &= 7;

    // Beta's rail branch pins posY EVERY tick, even at zero speed: onUpdate
    // sets posY = railY up front, and its final setPosition uses getPos()'s
    // track height, which on every flat-profile tile (straights 0/1 and
    // curves 6-9 -- both MATRIX rows carry dy 0) is railY + 0.5. The TRUE
    // Beta resting posY on a flat rail at cell y = railY is therefore
    // railY + 0.5 (bbox bottom railY + 0.15), reached on the FIRST tick after
    // ItemMinecart's railY + 0.85 spawn (railY + .5 + yOffset .35) -- the
    // cart settles instead of hovering at spawn height forever. Slopes settle
    // onto this engine's slope_height track (center = railY + .5; the
    // renderer's +.5 slope lift restores Beta's railY + 1 center), clamped
    // exactly like the moving slope branch below.
    // [740c583: EntityMinecart.onUpdate sets posY to the rail y as double, then
    //  setPosition at the re-sampled rail Y;
    //  EntityMinecart.getPos y = rail y + 0.5 on dy-0 MATRIX rows]
    // Guard (adversarial-review finding): Beta probes only the feet cell and
    // ONE BELOW (never above), so the settle must not act on the engine's +1
    // fallback hit -- otherwise a stopped cart under an elevated crossing line
    // would silently warp UP onto it. The +1 fallback stays available for the
    // moving/slope-seam math below, which is what it was added for.
    if (rail_y <= by_guess) {
        float track_y = slope_height(meta, rail_y, e->pos[0], e->pos[2],
                                     cx, cz, (float)rail_y + 0.5f);
        if (track_y < (float)rail_y) track_y = (float)rail_y;
        if (track_y > (float)rail_y + 1.0f) track_y = (float)rail_y + 1.0f;
        e->pos[1] = track_y;
    }

    // CR-05: Beta calls BlockDetectorRail.onEntityCollidedWithBlock every tick a
    // minecart's bbox overlaps the rail's detection box -- Entity's generic per-tick
    // "which blocks does my box overlap" scan, not a solid-collision hook (rails have no
    // getCollisionBoundingBoxFromPool box). This engine has no such generic dispatch
    // table (see block_rail.h), so drive it from the ONE place a cart already resolves
    // "which rail cell am I on" every tick, server-side only (Beta: `if
    // (!world.multiplayerWorld)`; the client-prediction re-entry passes s=NULL and must
    // not touch world state, same gate as the furnace-cart fuel burn below).
    // AFTER the posY settle-pin above, NOT before it (integration fix): Beta's
    // World.moveEntity block scan runs on the post-move position, and on the very
    // settle tick the pre-pin bbox still floats at ItemMinecart's spawn height --
    // above the 0.25-tall detection box, so a pre-pin check misses the arrival
    // tick. Still ahead of every early return below, so a fully parked cart
    // re-triggers the check every tick, matching Beta -- the callee itself is
    // idempotent (no-ops once bit 8 is already set).
    // [740c583: BlockDetectorRail.onEntityCollidedWithBlock]
    if (s && below.type == BLOCK_DETECTOR_RAIL)
        block_detector_rail_on_minecart_tick(s, bx, rail_y, bz);

    // RIDER THROTTLE -- the ONE deterministic apply point (milestone item 6).
    // Beta couples no rider motion into minecarts at all [740c583
    // EntityMinecart.onUpdate]. CaveWii keeps the documented controller
    // carve-out on ordinary and detector rails: detector rail has no propulsion
    // or brake to defeat, while powered rails remain autonomous. Server and
    // client still share this path, and every accepted cumulative step is
    // consumed exactly once even when the current rail type makes it a no-op.
    if((below.type == BLOCK_RAIL || below.type == BLOCK_DETECTOR_RAIL)
       && e->data.minecart.occupied
       && e->data.minecart.pending_throttle != 0) {
        const float cap = 0.3f;
        float before = e->data.minecart.speed;
        // pending_throttle is a signed STEP COUNT. The server derives it from
        // cumulative counter differences, so coalescing can recover several
        // authored ticks at once; it is not a bare -1/0/+1 held-state flag.
        float after = before + 0.01f * (float)e->data.minecart.pending_throttle;
        // Clamp to the THROTTLE's cap, or to the speed rail physics already
        // reached -- whichever is larger. The carve-out may not erase momentum
        // legitimately earned from a preceding slope or powered-rail tile.
        float limit = fabsf(before) > cap ? fabsf(before) : cap;
        if(after >  limit) after =  limit;
        if(after < -limit) after = -limit;
        e->data.minecart.speed = after;
    }
    e->data.minecart.pending_throttle = 0;

    // SLOPE GRAVITY -- Beta accelerates a cart downhill on every sloped shape.
    // [740c583 EntityMinecart.onUpdate:209-246]
    //     slope impulse = 0.0078125D (1/128, per tick):
    //     shape 2: X motion minus the impulse     (high side +X -> push -X)
    //     shape 3: X motion plus the impulse      (high side -X -> push +X)
    //     shape 4: Z motion plus the impulse      (high side -Z -> push +Z)
    //     shape 5: Z motion minus the impulse     (high side +Z -> push -Z)
    // In every case the impulse points AWAY from the high side, i.e. downhill.
    //
    // This engine had NO slope impulse at all: the only writes to
    // data.minecart.speed were the no-rail stop, the powered-rail boost/brake
    // and friction. So a cart parked on a ramp sat there forever and a cart
    // rolling downhill never gained speed -- carts simply could not roll.
    //
    // ENGINE MAPPING. Beta carries a motion VECTOR; this port carries a signed
    // scalar `speed` along the unit heading (hx,hz), where world motion
    // m = speed * h. Since h is +/-1, a world-axis impulse dm maps to
    // d(speed) = dm * h (and speed * h round-trips m for either sign of h).
    // Beta re-projects its motion onto the rail axis every tick with the
    // magnitude preserved (:258-260), which is exactly what makes the scalar
    // model equivalent here.
    //
    // ORDER. This sits ahead of the powered-rail branch and the friction step
    // because Beta's on-rail sequence is: slope impulse (:232-246) -> axis
    // projection (:258-260) -> brake (:262-273) -> ... -> boost (:396-415).
    // The distinction is REACHABLE, not theoretical: calcRailMeta's slope tests
    // (block_rail.c:129-132) run before the allow_curves gate, so a POWERED
    // rail can legally hold shapes 2-5, and on such a tile the brake/boost and
    // this impulse compose in an order that changes the result.
    // Applying it before the axis magnitude is taken is also what lets a cart
    // crawling uphill SLOWER than the impulse reverse within the SAME tick.
    {
        float slope_kick = 0.0f;
        int8_t axis_h = 0;
        switch (meta) {
            case 2: slope_kick = -0.0078125f; axis_h = e->data.minecart.hx; break;
            case 3: slope_kick = +0.0078125f; axis_h = e->data.minecart.hx; break;
            case 4: slope_kick = +0.0078125f; axis_h = e->data.minecart.hz; break;
            case 5: slope_kick = -0.0078125f; axis_h = e->data.minecart.hz; break;
            default: break;
        }
        if (slope_kick != 0.0f && fold_motion) {
            /* G1: with world motion to fold, Beta's kick lands on the
             * motion VECTOR before the projection below (:232-246 precede
             * :255-264), so apply it on its world axis here. */
            e->vel[(meta == 2 || meta == 3) ? 0 : 2] += slope_kick;
        } else if (slope_kick != 0.0f) {
            // A cart that has never moved has no heading yet (hx == hz == 0);
            // seed one on the slope's own axis so gravity has a direction to
            // act along. seed_heading picks the axis from the shape and does
            // NOT carry the speed's sign, which is what this needs.
            if (axis_h == 0) {
                seed_heading(meta, e->pos[0], e->pos[2], 0.0f,
                             &e->data.minecart.hx, &e->data.minecart.hz);
                axis_h = (meta == 2 || meta == 3) ? e->data.minecart.hx
                                                  : e->data.minecart.hz;
            }
            if (axis_h != 0)
                e->data.minecart.speed += slope_kick * (float)axis_h;
        }
    }

    /* G1: Beta's axis projection of a world motion vector [EntityMinecart.
     * java:255-264]: keep the horizontal MAGNITUDE sqrt(mx^2 + mz^2), point
     * it along the shape's MATRIX axis (glowing[meta][1] - [0]: +Z for 0/4/5,
     * +X for 1/2/3 -- seed_heading's axes), flipped when the motion's dot
     * product with that axis is negative. On the signed-scalar model that is
     * speed = +/-magnitude along h. Only reached when e->vel carries motion
     * (a landing or an explosion push); otherwise the rail model is
     * untouched. Curves: Beta's axis is the diagonal between the two exits,
     * this engine's is the segment seed_heading picks -- the existing
     * two-segment curve model difference (O06), recorded in deviations.md. */
    if (fold_motion) {
        int8_t ax = 0, az = 0;
        seed_heading(meta, e->pos[0], e->pos[2], 0.0f, &ax, &az);
        const float mx = e->data.minecart.speed * (float)e->data.minecart.hx
            + e->vel[0];
        const float mz = e->data.minecart.speed * (float)e->data.minecart.hz
            + e->vel[2];
        const float mag = (float)sqrt((double)mx * mx + (double)mz * mz);
        e->data.minecart.speed = (mx * (float)ax + mz * (float)az < 0.0f)
            ? -mag : mag;
        e->data.minecart.hx = ax;
        e->data.minecart.hz = az;
        e->vel[0] = 0.0f;
        e->vel[2] = 0.0f;
        if (s)
            entity_minecart_mark_snapshot_dirty(e, false);
    }

    // PLAYER-TO-CART NUDGE (batch 2b; both bodies: lane C #18) -- Beta
    // EntityMinecart.applyEntityCollision [740c583 EntityMinecart.java:646-714,
    // non-cart else-branch]:
    //     if (var1 == riddenByEntity) return;   // the rider never shoves its cart
    //     d = (dx,dz) / sqrt_double(d2)          // MathHelper.sqrt_double -> FLOAT
    //         * min(1/dist, 1) * (double)0.1F * (1 - entityCollisionReduction=0) * 0.5
    //     cart.addVelocity(-d); entity.addVelocity(+d/4)
    // triggered from EACH living entity's own EntityLiving.onLivingUpdate sweep
    // [EntityLiving.java:643-651: boundingBox.expand(0.2F, 0, 0.2F) over its own
    // world, canBePushed carts]. Every player runs it -- riding or not
    // (Entity.updateRidden still calls onUpdate) -- so each present body of THIS
    // cart's world is swept here, host first then the guest
    // (server_local_damage_candidate order), and a body in another world context
    // never touches it. An OCCUPIED cart is still shoved by any non-rider body;
    // only the rider itself is skipped. A seated body takes no counter-shove:
    // Entity.updateRidden zeroes the rider's motion every tick and re-seats it on
    // the mount, so its +d/4 never moves it. (Only the host can ride a minecart --
    // SRPC_MINECART_RIDE_INTENT is host-only -- so the occupied cart's rider is
    // the host.) This engine's players have no server locomotion tick, so the
    // sweep runs here in the cart tick (the same relative geometry each tick).
    // The world-axis impulse maps onto the scalar rail model exactly like the
    // slope kick above: d(speed) = dm . h, seeding a heading for a parked cart
    // so a push can start it. The counter-shove rides the server knockback
    // velocity (decayed into position by server_local_tick_actor_knockback for
    // either body). The overlap gate is Beta's own box test (below).
    /* (O21: the player-to-cart nudge now runs before the fold point,
     * above.) */

    // Beta splits the powered-rail state before stripping bit 8. An UNPOWERED
    // powered rail brakes here, before movement; the powered boost is deferred
    // until after movement, friction, and slope-energy exchange below, matching
    // EntityMinecart.onUpdate's exact order. [740c583 :219-222, :262-273,
    // :331-370, :396-415]
    const bool is_powered_rail = below.type == BLOCK_POWERED_RAIL;
    const bool rail_has_power = is_powered_rail && (below.metadata & 0x8);
    if (is_powered_rail && !rail_has_power) {
        if (fabsf(e->data.minecart.speed) < 0.03f)
            e->data.minecart.speed = 0.0f;
        else
            e->data.minecart.speed *= 0.5f;
    }

    float spd = e->data.minecart.speed;

    // A parked cart still needs a rail-axis heading: Beta's powered rest launch
    // writes world-axis motion after the movement/friction phase below, while
    // this scalar model maps that signed kick through (hx,hz).
    if (e->data.minecart.hx == 0 && e->data.minecart.hz == 0) {
        seed_heading(meta, e->pos[0], e->pos[2], spd, &e->data.minecart.hx, &e->data.minecart.hz);
    }

    // Preserve the cheap idle fast path for every rail that cannot assign a
    // post-move launch. A powered flat rail is the sole zero-speed exception:
    // Beta checks its adjacent walls after movement/friction and may set 0.02.
    // Sloped powered rails already have nonzero `spd` from gravity above.
    //
    // FURNACE-CART SECOND EXCEPTION. A type-2 cart with an active push vector
    // can ALSO launch from rest -- Beta's push-physics branch below computes
    // `motion = motion*0.8 + push_normalised*0.04`, which is nonzero even when
    // motion starts at exactly 0. Without this exclusion a freshly-fuelled,
    // freshly-pointed (interact() sets pushX/pushZ), still-at-rest furnace cart
    // would return here EVERY tick and never reach the push-physics block that
    // is the only thing that could ever move it. Mirrors Beta's own gate
    // exactly: [740c583 EntityMinecart.onUpdate:337-338] `var39 > 0.01D`.
    bool furnace_push_active = e->data.minecart.minecart_type == 2
        && !e->data.minecart.occupied
        && sqrtf(e->data.minecart.push_x * e->data.minecart.push_x
                 + e->data.minecart.push_z * e->data.minecart.push_z) > 0.01f;
    if (spd == 0.0f && !rail_has_power && !furnace_push_active) {
        e->data.minecart.rail_meta = meta;
        e->data.minecart.last_meta = meta;
        return false;
    }

    // Beta clamps the DISPLACEMENT COPY, not the stored momentum: the dx/dz copy
    // start as motionX/motionZ, are scaled *0.75 when ridden, clamped to
    // +-0.4D (the max speed), and only that copy reaches moveEntity -- motionX/motionZ
    // themselves stay unclamped for the next tick's physics. [740c583
    // EntityMinecart.onUpdate:209 (max speed), :303-306 (ridden *0.75), :308-317
    // (clamp)]. Without this the slope energy exchange converges to ~1.9
    // blocks/tick, and past 1.0 the per-tile movement switch below applies ONE
    // tile's meta to a multi-tile displacement -- carts skip curves and
    // hard-stop at lookup misses. The adversarial review caught it; the clamp
    // is plain Beta parity, not a deviation.
    float move_spd = spd;
    if (e->data.minecart.occupied)
        move_spd *= 0.75f;
    if (move_spd >  0.4f) move_spd =  0.4f;
    if (move_spd < -0.4f) move_spd = -0.4f;

    int sgn    = (spd >= 0.0f) ? +1 : -1;
    int cur_dx = e->data.minecart.hx * sgn;
    int cur_dz = e->data.minecart.hz * sgn;

    // Cheap helpers
    #define MOVE_ALONG_X(amount) do { e->pos[0] += (amount) * (float)cur_dx; } while (0)
    #define MOVE_ALONG_Z(amount) do { e->pos[2] += (amount) * (float)cur_dz; } while (0)

    // Track height BEFORE this tick's movement -- Beta's `var10`, the rail
    // surface sampled before moveEntity. Paired with the post-move sample by
    // the energy exchange after the switch. [740c583 EntityMinecart.onUpdate:214]
    const float y_before_move = e->pos[1];

    /* O06: set when Beta's moveEntity clipped the rail displacement. */
    bool rail_blocked = false;

    switch (meta) {
        // --- Straight NS ---
        case 0: {
            e->pos[0] = cx;                // lock X
            float np;
            enum minecart_rail_move m = minecart_rail_clip(e, s, rail_y, false,
                2, fabsf(move_spd) * (float)cur_dz, &np);
            if (m == MINECART_RAIL_FREE) MOVE_ALONG_Z(fabsf(move_spd));
            else if (m == MINECART_RAIL_CLIPPED) { e->pos[2] = np; rail_blocked = true; }
            break;
        }

        // --- Straight EW ---
        case 1: {
            e->pos[2] = cz;                // lock Z
            float np;
            enum minecart_rail_move m = minecart_rail_clip(e, s, rail_y, false,
                0, fabsf(move_spd) * (float)cur_dx, &np);
            if (m == MINECART_RAIL_FREE) MOVE_ALONG_X(fabsf(move_spd));
            else if (m == MINECART_RAIL_CLIPPED) { e->pos[0] = np; rail_blocked = true; }
            break;
        }

        // --- Slopes EW (2: up to W, 3: up to E) ---
        case 2: // ascend West (-X uphill)
        case 3: { // ascend East (+X uphill)
            e->pos[2] = cz;                // lock Z (unchanged)
            {
                float np;
                enum minecart_rail_move m = minecart_rail_clip(e, s, rail_y,
                    true, 0, fabsf(move_spd) * (float)cur_dx, &np);
                if (m == MINECART_RAIL_FREE) MOVE_ALONG_X(fabsf(move_spd));
                else if (m == MINECART_RAIL_CLIPPED) { e->pos[0] = np; rail_blocked = true; }
            }

            // Deterministic absolute Y from tile center (no accumulation drift)
            float y = slope_height(meta, rail_y, e->pos[0], e->pos[2], cx, cz, e->pos[1]);
            // Clamp to [rail_y, rail_y+1] to avoid tiny float overshoot
            if (y < (float)rail_y) y = (float)rail_y;
            if (y > (float)rail_y + 1.0f) y = (float)rail_y + 1.0f;
            e->pos[1] = y;
            break;
        }

        // --- Slopes NS (4: up to N, 5: up to S) ---
        case 4: // ascend North (-Z uphill)
        case 5: { // ascend South (+Z uphill)
            e->pos[0] = cx;                // lock X (unchanged)
            {
                float np;
                enum minecart_rail_move m = minecart_rail_clip(e, s, rail_y,
                    true, 2, fabsf(move_spd) * (float)cur_dz, &np);
                if (m == MINECART_RAIL_FREE) MOVE_ALONG_Z(fabsf(move_spd));
                else if (m == MINECART_RAIL_CLIPPED) { e->pos[2] = np; rail_blocked = true; }
            }

            float y = slope_height(meta, rail_y, e->pos[0], e->pos[2], cx, cz, e->pos[1]);
            if (y < (float)rail_y) y = (float)rail_y;
            if (y > (float)rail_y + 1.0f) y = (float)rail_y + 1.0f;
            e->pos[1] = y;
            break;
        }

        // --- Curves (center -> turn -> out) ---
        case 6: case 7: case 8: case 9: {
            if (cur_dx != 0) e->pos[2] = cz; else e->pos[0] = cx;

            float dist_to_center = (cur_dx != 0)
                ? (cx - e->pos[0]) * (float)cur_dx
                : (cz - e->pos[2]) * (float)cur_dz;

            float step = fabsf(move_spd);

            if (dist_to_center > MC_EPS) {
                float move = (step <= dist_to_center) ? step : dist_to_center;
                /* O06: this engine's curve is two axis segments (Beta moves a
                 * curve diagonally -- a pre-existing model difference); each
                 * segment is still clipped against blocks like moveEntity. */
                float np;
                enum minecart_rail_move m = minecart_rail_clip(e, s, rail_y,
                    false, cur_dx != 0 ? 0 : 2,
                    move * (float)(cur_dx != 0 ? cur_dx : cur_dz), &np);
                if (m != MINECART_RAIL_FREE) {
                    if (m == MINECART_RAIL_CLIPPED) {
                        e->pos[cur_dx != 0 ? 0 : 2] = np;
                        rail_blocked = true;
                    }
                    break;
                }
                if (cur_dx != 0) MOVE_ALONG_X(move); else MOVE_ALONG_Z(move);
                if (step <= dist_to_center + 1e-6f) break;
                step -= dist_to_center;
            }

            int out_dx = cur_dx, out_dz = cur_dz;
            curve_turn(meta, cur_dx, cur_dz, &out_dx, &out_dz);

            if (out_dx != 0) e->pos[2] = cz; else e->pos[0] = cx;
            {
                float np;
                enum minecart_rail_move m = minecart_rail_clip(e, s, rail_y,
                    false, out_dx != 0 ? 0 : 2,
                    step * (float)(out_dx != 0 ? out_dx : out_dz), &np);
                if (m == MINECART_RAIL_FREE) {
                    if (out_dx != 0) e->pos[0] += step * (float)out_dx;
                    else             e->pos[2] += step * (float)out_dz;
                } else if (m == MINECART_RAIL_CLIPPED) {
                    e->pos[out_dx != 0 ? 0 : 2] = np;
                    rail_blocked = true;
                }
            }

            e->data.minecart.hx = (int8_t)(out_dx * sgn);
            e->data.minecart.hz = (int8_t)(out_dz * sgn);
            break;
        }

        default:
            // Unknown meta: clamp to center to avoid drifting off-rail
            e->pos[0] = cx;
            e->pos[2] = cz;
            break;
    }

    /* O06: Entity.moveEntity zeroed the clipped motion component; the rail
     * motion is single-axis, so the scalar speed is exactly that component.
     * Everything after (furnace push, friction, slope energy, powered rest
     * launch) then runs on the stopped cart exactly as Beta's does. */
    if (rail_blocked)
        e->data.minecart.speed = 0.0f;

    // FURNACE-CART PUSH PHYSICS -- type 2 only, unridden only (Beta nests this
    // INSIDE the `riddenByEntity == null` else-branch, ahead of the shared 0.96
    // friction below, which still applies on top afterward -- both multipliers
    // compose on a pushing furnace cart). mag>0.01: normalise the push vector
    // (write-back, matching Beta's in-place `pushX /= var39`) and blend it into
    // speed at 0.04 after decaying the existing speed by 0.8. Otherwise: decay
    // by 0.9 alone, no direction invented. furnace_pushing mirrors Beta's local
    // `var6`, consumed below by the fuel-decrement gate (also 1-in-4, ALSO
    // server-only: fuel/pushX/pushZ are simulation state, never networked --
    // see MINECART_SLICE_NOTES.md). [740c583 EntityMinecart.onUpdate:336-353]
    bool furnace_pushing = false;
    if (e->data.minecart.minecart_type == 2 && !e->data.minecart.occupied) {
        float px = e->data.minecart.push_x;
        float pz = e->data.minecart.push_z;
        float push_mag = sqrtf(px * px + pz * pz);
        if (push_mag > 0.01f) {
            furnace_pushing = true;
            px /= push_mag;
            pz /= push_mag;
            e->data.minecart.push_x = px;
            e->data.minecart.push_z = pz;
            float dot_heading = px * (float)e->data.minecart.hx
                + pz * (float)e->data.minecart.hz;
            e->data.minecart.speed = e->data.minecart.speed * 0.800000011920929f
                + dot_heading * 0.04f;
        } else {
            e->data.minecart.speed *= 0.8999999761581421f;
        }
    }

    // Beta applies rail friction AFTER moveEntity and chooses the multiplier
    // from rider occupancy: 0.996999979019165 when ridden, otherwise
    // 0.9599999785423279 for an ordinary empty cart. The signed-scalar mapping
    // is exact because the projected motion lies on one unit rail axis.
    // [740c583 EntityMinecart.onUpdate:331-358]
    e->data.minecart.speed *= e->data.minecart.occupied
        ? 0.996999979019165f
        : 0.9599999785423279f;

    // RAIL-HEIGHT ENERGY EXCHANGE -- Beta converts the height the cart just
    // gave up into speed (and charges it for height gained).
    // [740c583 EntityMinecart.onUpdate:360-370]
    //     after = getPos at the cart's position (post-move);
    //     when both after and before (sampled pre-move) exist:
    //         drop = (before.y - after.y) * 0.05D;
    //         speed = sqrt(motionX^2 + motionZ^2);
    //         when speed > 0.0D, both horizontal motions
    //             are divided by speed and multiplied by
    //             (speed + drop), X and Z alike;
    //
    //         then the cart is set to the after.y height.
    //
    // before is the rail surface sampled BEFORE moveEntity, after AFTER, so
    // drop is positive when the cart DESCENDED. The motion is rescaled to
    // magnitude (speed + drop) with its direction preserved -- which is why a
    // long descent keeps accelerating instead of settling at the 1/128 impulse
    // alone. This is the SECOND half of Beta's slope behaviour; the impulse
    // above is the first, and both are needed.
    //
    // ORDER: Beta applies this AFTER friction (:331-358) and after the move,
    // unlike the impulse which precedes both. Reversing the two changes the
    // fixed-point speed on a sustained slope.
    //
    // ENGINE MAPPING: on the signed-scalar model the direction lives in the
    // sign, so rescaling to (|speed| + drop) is exactly Beta's operation --
    // INCLUDING the reversal when |speed| + drop goes negative on a steep
    // climb, which Beta gets from the (speed + drop) factor flipping sign.
    //
    // A flat tile (straights, curves) leaves the height unchanged, so drop is
    // 0 and this is a no-op there -- flat-track behaviour is untouched.
    {
        const float dropped = y_before_move - e->pos[1]; // >0 when descending
        /* Beta rescales only `if (var23 > 0.0D)` (:351-352): a cart that is
         * not moving (e.g. just stopped by moveEntity) gains nothing. */
        if (dropped != 0.0f && e->data.minecart.speed != 0.0f) {
            const float sp_now = e->data.minecart.speed;
            const float mag = fabsf(sp_now) + dropped * 0.05f;
            e->data.minecart.speed = (sp_now < 0.0f) ? -mag : mag;
        }
    }

    // FURNACE-CART PUSH-VECTOR UPDATE -- the push direction tracks the cart's
    // OWN resulting motion once it is genuinely moving, so a stalled/blocked
    // cart naturally stops re-arming itself. Sits OUTSIDE the riddenByEntity
    // branch in Beta (no occupancy gate) -- academic in practice since nothing
    // in this engine ever mounts a type-2 cart, but kept unguarded to match the
    // decompile's literal structure. Double-gated on BOTH the push magnitude
    // (>0.01, re-normalised -- a write-back like section A above) AND the
    // cart's own speed (motion^2 > 0.001, i.e. |speed| > ~0.0316): below that
    // this is a no-op and push_x/push_z carry over unchanged. When gated in:
    // the dot of the (re-normalised) push with the current motion decides
    // whether the cart is being pushed BACKWARDS (dot<0 -> zero the push, this
    // is the "drops its push direction when shoved backwards" the dispatch
    // cites) or forwards/neutral (push := current motion vector, UNnormalised
    // -- Beta's own `pushX = motionX`, not `pushX/mag = ...`). motionX/Z map to
    // speed*hx/hz on the signed-scalar model, exactly as section A's dot used.
    // [740c583 EntityMinecart.onUpdate:380-394]
    if (e->data.minecart.minecart_type == 2) {
        float px = e->data.minecart.push_x;
        float pz = e->data.minecart.push_z;
        float push_mag = sqrtf(px * px + pz * pz);
        float spd_now = e->data.minecart.speed;
        if (push_mag > 0.01f && spd_now * spd_now > 0.001f) {
            px /= push_mag;
            pz /= push_mag;
            float motion_x = spd_now * (float)e->data.minecart.hx;
            float motion_z = spd_now * (float)e->data.minecart.hz;
            float dot = px * motion_x + pz * motion_z;
            if (dot < 0.0f) {
                e->data.minecart.push_x = 0.0f;
                e->data.minecart.push_z = 0.0f;
            } else {
                e->data.minecart.push_x = motion_x;
                e->data.minecart.push_z = motion_z;
            }
        }
    }

    // POWERED BOOST -- exact Beta constant, gate, and order. This runs after
    // movement, occupancy-specific friction, and slope-energy exchange.
    // Moving carts receive +0.06 along their existing motion with NO stored-
    // momentum cap; Beta clamps only the displacement copy to 0.4 above.
    // At magnitude <= 0.01, only a wall beside a FLAT straight assigns the
    // signed 0.02 launch. Open track, slopes, and curves invent no direction.
    // [740c583 EntityMinecart.onUpdate:396-415]
    if (rail_has_power) {
        float boost_speed = e->data.minecart.speed;
        if (fabsf(boost_speed) > 0.01f) {
            e->data.minecart.speed
                = boost_speed + (boost_speed < 0.0f ? -0.06f : +0.06f);
        } else {
            int8_t hx = e->data.minecart.hx;
            int8_t hz = e->data.minecart.hz;
            struct block_data wneg, wpos;
            float kick = 0.0f;
            int8_t axis_h = 0;

            if (meta == 1 && hx != 0) { // flat EW -> X axis
                bool solid_neg = entity_get_block(e, bx - 1, rail_y, bz, &wneg)
                                 && block_is_normal_cube(wneg.type);
                bool solid_pos = entity_get_block(e, bx + 1, rail_y, bz, &wpos)
                                 && block_is_normal_cube(wpos.type);
                if (solid_neg)      kick = +0.02f;
                else if (solid_pos) kick = -0.02f;
                axis_h = hx;
            } else if (meta == 0 && hz != 0) { // flat NS -> Z axis
                bool solid_neg = entity_get_block(e, bx, rail_y, bz - 1, &wneg)
                                 && block_is_normal_cube(wneg.type);
                bool solid_pos = entity_get_block(e, bx, rail_y, bz + 1, &wpos)
                                 && block_is_normal_cube(wpos.type);
                if (solid_neg)      kick = +0.02f;
                else if (solid_pos) kick = -0.02f;
                axis_h = hz;
            }

            if (kick != 0.0f)
                e->data.minecart.speed = kick * (float)axis_h;
        }
    }

    // FURNACE-CART FUEL BURN -- SERVER-ONLY. This "if(s)" gate is why: the
    // client-prediction re-entry (minecart_client_tick) passes s=NULL and must
    // not roll RNG or mutate fuel, which is simulation-only state never
    // networked (see MINECART_SLICE_NOTES.md). Beta's own guard is a
    // short-circuited `&&`, so the 1-in-4 roll itself is CONDITIONAL on
    // furnace_pushing (Beta's local push flag) -- draw it only when Beta would, not
    // unconditionally: CODESTYLE.md S2.3's "draw unconditionally" rule governs
    // draws Beta ITSELF always makes every tick; this is one Beta itself SKIPS
    // whenever the cart is not actively pushing. fuel<0 clears the push
    // vector, which stops next tick's push-physics branch (mag(push)==0) and
    // therefore stops further decrements too -- fuel settles at exactly -1 and
    // never drifts further negative. The "largesmoke" particle spawn
    // [740c583 :491] is a documented, deferred cosmetic (MINECART_SLICE_NOTES.md)
    // with no simulation effect. [740c583 EntityMinecart.onUpdate:485-492]
    // The roll uses its own rand -- the cart's OWN Random, not World.rand
    // (W3-s2): see minecart_entity_rng.
    /* I03 (board #8): now the cart's OWN lifetime stream, not the shared
     * s->entity_rng [CFR EntityMinecart.java:452, own rand nextInt(4)]. */
    if (s && furnace_pushing
        && jrand_next_int_bound(&e->gameplay_rng, 4) == 0) {
        e->data.minecart.fuel--;
        if (e->data.minecart.fuel < 0) {
            e->data.minecart.push_x = 0.0f;
            e->data.minecart.push_z = 0.0f;
        }
    }

    /* PERFORMANCE DEVIATION: Beta has no generic rail cutoff. This must remain
     * the final speed write in the tick: physics, slope energy, and a powered
     * wall launch all get their full chance first. Retire only a residual tail
     * below 1e-5, then mark the transition at its source so physics, the next
     * tick's exact-zero idle gate, and the terminal snapshot share one stopped
     * state. Client prediction executes the same snap. */
    if(e->data.minecart.speed != 0.0f
       && fabsf(e->data.minecart.speed) < MINECART_TERMINAL_SPEED) {
        e->data.minecart.speed = 0.0f;
        if(s)
            entity_minecart_mark_snapshot_dirty(e, false);
    }

    // Save meta for debugging/consistency
    e->data.minecart.rail_meta = meta;
    e->data.minecart.last_meta = meta;

    // IMPORTANT: do NOT touch rider here. Local player handles camera attach.

    return false;
}


/* G2: Beta runs the cart's collision sweep at the end of every server
 * onUpdate (on or off the rail, moving or parked); the client prediction
 * re-entry (s == NULL) never does. */
static bool minecart_server_tick(struct entity* e, struct server_local* s) {
    bool remove = minecart_server_tick_body(e, s);
    if(s && !remove)
        entity_minecart_collide_neighbours(s, e);
    return remove;
}

/* Rolling sound (DELIBERATE NON-BETA ADD-ON -- Beta carts are silent; see
 * audio_sound_table.h and docs/deviations.md).
 *
 * 2026-09-03: re-scoped from a synthesized filler to a gated import of the
 * REAL modern Mojang sample. The engine's per-key rule already does the
 * gating for free -- a key with no bundled default plays nothing until a
 * file actually lands under its scan path (audio_registry.c) -- so this key
 * ships NO bundled default any more (assets/sound/minecart/ is empty by
 * construction: base1..3.wav retired 2026-09-03). A stock pack is therefore
 * genuinely SILENT, matching Beta; running
 * tools/import_owned_beta_assets.py --modern-minecraft <owned .minecraft>
 * drops the user's own minecraft/sounds/minecart/base.ogg into their pack,
 * and THIS same code plays it once it is there. Nothing below this comment
 * changed to make that true -- only the shipped asset did.
 *
 * CLIENT-LOCAL on purpose. Routing it through the server would put a ~20Hz
 * message on the client->server queue, which is exactly the pressure that
 * starves dig/place; presentation belongs on the client anyway (Beta plays its
 * own mining-hit ticks client-side for the same reason).
 *
 * RETRIGGERED, not looped: the mixer is 16 one-shot voices with no loop
 * support, so the cart carries a distance odometer and fires one short
 * rail-joint sample every ROLL_STEP blocks. Cadence therefore follows SPEED for
 * free -- faster cart, closer joints -- with pitch/volume scaled on top. */
#define MINECART_ROLL_STEP 1.15f /* blocks between joint clacks */
#define MINECART_ROLL_MIN_SPEED 0.02f

/* Speed -> (volume, pitch) for the rolling sound. Pure so it can be unit-
 * tested directly (same "extract the decision, test it without a fixture"
 * shape as entity_minecart_resolve_throttle above). 0.3 is the throttle cap
 * [MINECART_ROLL_STEP's sibling constant, entity_minecart_resolve_throttle's
 * caller], so speed is normalised against it and a floor keeps a slow coast
 * audible rather than fading to nothing. */
void entity_minecart_roll_volume(float speed, float* vol_out,
								  float* pitch_out) {
	assert(vol_out && pitch_out);
	float t = fabsf(speed) / 0.3f;
	if(t > 1.0f)
		t = 1.0f;
	*vol_out = 0.25f + 0.45f * t;
	*pitch_out = 0.85f + 0.35f * t;
}

static void minecart_roll_sound(struct entity* e) {
    if(!gstate.config_minecart_sound)
        return; /* debug.minecart_sound "off" -> Beta silence */

    float spd = fabsf(e->data.minecart.speed);
    if(spd < MINECART_ROLL_MIN_SPEED) {
        /* Parked: bleed the odometer so a nudge does not instantly clack. */
        e->data.minecart.roll_dist = 0.0f;
        return;
    }

    float dx = e->pos[0] - e->pos_old[0];
    float dz = e->pos[2] - e->pos_old[2];
    float moved = sqrtf(dx * dx + dz * dz);
    /* Ignore teleport-scale jumps (chunk load, restore) -- same guard, and the
     * same reason, as the player footstep odometer in server_local.c. */
    /* Fail CLOSED: `!(moved <= 2.0f)` also catches NaN, which `> 2.0f` lets
     * straight through into the odometer -- and a NaN odometer clacks at 20Hz
     * forever (review finding 6). */
    if(!(moved <= 2.0f))
        return;
    e->data.minecart.roll_dist += moved;
    if(e->data.minecart.roll_dist < MINECART_ROLL_STEP)
        return;
    e->data.minecart.roll_dist -= MINECART_ROLL_STEP;

    float vol, pitch;
    entity_minecart_roll_volume(spd, &vol, &pitch);
    audio_play3d(SOUND_MINECART_BASE,
                 (vec3) {e->pos[0], e->pos[1], e->pos[2]}, vol, pitch);
}

/* Companion "you are riding" seat loop -- same add-on family, same 2026-09-03
 * re-scope (see minecart_roll_sound above). SOUND_MINECART_INSIDE ships no
 * bundled default either; it stays silent until the user imports the real
 * minecraft/sounds/minecart/inside.ogg.
 *
 * KEY SELECTION is deliberately narrower than the roll sound: "inside" is
 * what the RIDER hears from their own seat, not ambience anyone nearby
 * hears, so it must fire for the LOCAL PLAYER's own occupied cart only --
 * never a remote rider's, never an empty one. Pure predicate so the
 * selection logic is unit-testable without a world/entity fixture. */
bool entity_minecart_wants_inside_sound(bool occupied, uint32_t occupant_id,
										 uint32_t local_player_id) {
	return occupied && occupant_id == local_player_id;
}

/* Measured duration of the real modern inside.ogg: 5.770181s, ffprobe
 * against the user's OWN owned copy (hash f11f499...; never committed --
 * see audio_sound_table.h). Retriggered rather than looped for the same
 * one-shot-mixer reason as the roll sound; rounded UP to the next tick so
 * two copies can never audibly overlap (a ~30ms silent gap is the accepted
 * trade). */
#define MINECART_INSIDE_LOOP_TICKS 116 /* ceil(5.770181s * 20 ticks/s) */

static void minecart_inside_sound(struct entity* e) {
	if(!gstate.config_minecart_sound)
		return; /* debug.minecart_sound "off" -> Beta silence */

	struct entity* player = gstate.players[0].local_player;
	uint32_t local_id = player ? player->id : (uint32_t)-1;
	if(!entity_minecart_wants_inside_sound(e->data.minecart.occupied,
											e->data.minecart.occupant_id,
											local_id)) {
		/* Not (or no longer) our own seat: reset so the NEXT boarding fires
		 * immediately rather than waiting out a stale countdown. */
		e->data.minecart.inside_timer = 0.0f;
		return;
	}

	if(e->data.minecart.inside_timer > 0.0f) {
		e->data.minecart.inside_timer -= 1.0f;
		return;
	}
	e->data.minecart.inside_timer = (float)MINECART_INSIDE_LOOP_TICKS;
	audio_play3d(SOUND_MINECART_INSIDE,
				 (vec3) {e->pos[0], e->pos[1], e->pos[2]}, 1.0f, 1.0f);
}

static bool minecart_client_tick(struct entity* e) {
    /* CONTRACT (audit F4): the tick's return value feeds entities_client_tick's
     * `bool remove = ... || e->tick_client(e)` -- true means REMOVE THIS
     * ENTITY, not "alive". The first version of this gate read it as "alive"
     * and played the roll sound only on the removal frame, i.e. NEVER: every
     * normal minecart tick returns false. Play on the frames the cart LIVES. */
    int32_t current_input = e->data.minecart.pending_throttle;
    if(e->data.minecart.replay_throttle != 0) {
        /* Corrections arrive in clin_update before the entity pass. Consume the
         * historical suffix now, but preserve input authored by a local-player
         * tick that happened to precede this cart in dictionary order. In the
         * opposite order that input is authored after this function; either way
         * it belongs to the next cart tick, so hash order cannot change physics. */
        e->data.minecart.pending_throttle
            = e->data.minecart.replay_throttle;
        e->data.minecart.replay_throttle = 0;
    } else {
        current_input = 0;
    }

    bool remove = minecart_server_tick_body(e, NULL);
    if(!remove && current_input != 0)
        e->data.minecart.pending_throttle = current_input;
    if(!remove) {
        minecart_roll_sound(e);
        minecart_inside_sound(e);
    }
    return remove;
}

void entity_minecart_stage_replay_control(struct entity* e, int32_t steps) {
    assert(e && e->type == ENTITY_MINECART && !e->on_server);
    if(steps == 0)
        return;
    assert(steps >= -(int32_t)MINECART_CONTROL_MAX_GAP
           && steps <= (int32_t)MINECART_CONTROL_MAX_GAP);
    /* clin_update precedes the ordinary 20 Hz client entity pass. Leave the
     * suffix at that pass's one consume-and-clear point; re-entering the full
     * physics routine here would move the cart and decay its hit state twice in
     * a frame that also executes minecart_client_tick. */
    e->data.minecart.replay_throttle = steps;
}

void entity_minecart_mark_snapshot_dirty(struct entity* e, bool urgent) {
    assert(e && e->type == ENTITY_MINECART);
    e->data.minecart.snapshot_dirty = true;
    e->data.minecart.snapshot_urgent
        = e->data.minecart.snapshot_urgent || urgent;
}

/* Map RIDER intent (+1 forward / -1 back) onto the cart's signed heading axis.
 * Pure so it can be tested directly; see tests/test_minecart_throttle_sign.c.
 *   MOVING -> "forward" means FASTER along the current direction of travel.
 *     Deliberately not facing-based: a facing test flaps when the rider looks
 *     near-perpendicular to the rail, inverting the throttle between ticks.
 *   AT REST -> no travel direction exists, so use where the rider is looking
 *     along the rail axis. Player forward in world XZ is (sin yaw, cos yaw),
 *     the same basis entity_local_player uses for walking.
 * A zero/absent heading resolves to +1 rather than flapping; the first tick
 * seeds one. */
int8_t entity_minecart_resolve_throttle(int8_t intent, float speed, int8_t hx,
										int8_t hz, float yaw) {
	if(intent == 0)
		return 0;
	int8_t axis_dir;
	if(fabsf(speed) > 0.001f) {
		axis_dir = (speed > 0.0f) ? 1 : -1;
	} else {
		float d = sinf(yaw) * (float)hx + cosf(yaw) * (float)hz;
		/* Flip only on a MEANINGFULLY negative projection. A bare `d < 0`
		 * decided the perpendicular case on floating-point noise -- looking
		 * exactly across an NS rail gives d = cosf(M_PI/2) = -4.4e-8, i.e. a
		 * "backwards" answer produced entirely by rounding, and a rider
		 * hovering near 90 degrees could flip direction on sub-degree stick
		 * wobble. The deadzone makes across-the-rail resolve to +1 the same
		 * way every time. It costs nothing for real intent: at 1e-4 the
		 * threshold is ~0.006 degrees off perpendicular. */
		axis_dir = (d < -1e-4f) ? -1 : 1;
	}
	return (int8_t)((intent > 0 ? 1 : -1) * axis_dir);
}

void entity_minecart_rail_pose(uint8_t block_type, uint8_t metadata,
                               struct minecart_rail_pose* pose) {
	assert(pose);
    pose->yaw_deg = 0.0f;
    pose->pitch_deg = 0.0f;
    pose->y_offset = 0.0f;

    if (block_type != BLOCK_RAIL
        && block_type != BLOCK_POWERED_RAIL
        && block_type != BLOCK_DETECTOR_RAIL)
        return;

    uint8_t meta = metadata & 0xF;
    // BlockRail.getIsPowered() is true for powered AND detector rails, so
    // EntityMinecart.onUpdate strips their redstone-state bit before using
    // metadata as the track shape. Plain rail curve shapes 8/9 stay intact.
    // [740c583: BlockRail.<init>; EntityMinecart.onUpdate "var11 &= 7"]
    if (block_type == BLOCK_POWERED_RAIL
        || block_type == BLOCK_DETECTOR_RAIL)
        meta &= 7;

    switch (meta) {
        case 0: pose->yaw_deg = 0.0f; break; // NS
        case 1: pose->yaw_deg = 90.0f; break; // EW
        case 2:
            pose->yaw_deg = 90.0f;
            pose->pitch_deg = 45.0f;
            pose->y_offset = 0.5f;
            break; // slope W
        case 3:
            pose->yaw_deg = 90.0f;
            pose->pitch_deg = -45.0f;
            pose->y_offset = 0.5f;
            break; // slope E
        case 4:
            pose->pitch_deg = 45.0f;
            pose->y_offset = 0.5f;
            break; // slope S
        case 5:
            pose->pitch_deg = -45.0f;
            pose->y_offset = 0.5f;
            break; // slope N
        case 9: pose->yaw_deg = 135.0f; break; // SE curve
        case 8: pose->yaw_deg = -135.0f; break; // SW curve
        case 7: pose->yaw_deg = -45.0f; break; // NW curve
        case 6: pose->yaw_deg = 45.0f; break; // NE curve
        default: break;
    }
}

static void entity_minecart_render(struct entity* e, mat4 view, float tick_delta) {
    vec3 pos_lerp;
    glm_vec3_lerp(e->pos_old, e->pos, tick_delta, pos_lerp);

    /* A render can briefly outlive its chunk during an unload. Both lookups
     * are fallible, so use neutral defaults instead of reading uninitialized
     * light or rail metadata when the world no longer owns the cell. */
    struct block_data in_block = {0};
    (void)entity_get_block(e,
        floorf(pos_lerp[0]),
        floorf(pos_lerp[1]),
        floorf(pos_lerp[2]),
        &in_block
    );

    render_entity_update_light(
        (in_block.torch_light << 4) | (in_block.sky_light)
    );

    struct block_data rail_block = {.type = BLOCK_AIR, .metadata = 0};
    int bx = (int)floorf(pos_lerp[0]);
    int by = (int)floorf(pos_lerp[1] - 0.1f);
    int bz = (int)floorf(pos_lerp[2]);
    (void)entity_get_block(e, bx, by, bz, &rail_block);

    struct minecart_rail_pose rail_pose;
    entity_minecart_rail_pose(rail_block.type, rail_block.metadata, &rail_pose);

    // Beta draws the cart model CENTERED on the render origin: under
    // RenderMinecart's glScalef(-1,-1,1) y-down flip, ModelMinecart's floor
    // slab (rotationPointY 4, +/-1 px thick after its X-rotation) spans
    // 3..5 px BELOW the origin and the walls reach 5 px above it. This
    // engine's cart mesh starts at model y 0 (its bottom), so shift the
    // translate down 5/16 to land the visual bottom at Beta's posY - 0.3125:
    // a settled flat-rail cart (posY = railY + .5) skirts the 2 px-high rail
    // at railY + .1875 instead of floating half a block above it.
    // [740c583: RenderMinecart.func_152_a; ModelMinecart.<init>]
    const float model_center_down = 0.3125f;

    mat4 model, mv;
    glm_translate_make(model, (vec3){
        pos_lerp[0],
        pos_lerp[1] + rail_pose.y_offset - model_center_down,
        pos_lerp[2]
    });

    glm_rotate_y(model, glm_rad(-rail_pose.yaw_deg), model);
    glm_rotate_x(model, glm_rad(rail_pose.pitch_deg), model);

    // Hit shake [740c583 RenderMinecart.func_152_a]: while shake ticks remain,
    // rock about the cart's local X by sin(t)*t*(damage)/10 degrees, direction
    // alternating per hit. Both inputs decay 1/tick, so the rock rings down.
    {
        float shake_f = (float)e->data.minecart.shake_ticks - tick_delta;
        float dmg_f = (float)e->data.minecart.damage_taken - tick_delta;
        if(dmg_f < 0.0f)
            dmg_f = 0.0f;
        if(shake_f > 0.0f)
            glm_rotate_x(model,
                         glm_rad(sinf(shake_f) * shake_f * dmg_f / 10.0f
                                 * (float)e->data.minecart.shake_dir),
                         model);
    }

    glm_mat4_mul(view, model, mv);

    render_entity_minecart(mv);

    struct AABB bbox;
    aabb_setsize_centered(&bbox, 0.25F, 0.25F, 0.25F);
    aabb_translate(&bbox,
        pos_lerp[0],
        pos_lerp[1] - 0.04F,
        pos_lerp[2]
    );
    entity_shadow(e, &bbox, view);
}

static size_t getBoundingBox(const struct entity *e, struct AABB *out) {
    assert(e && out);
	entity_make_world_bbox(e, out);
    return 1;
}

// entity_minecart.c (of waar je onRightClick staat)

static bool onRightClick(struct entity* e) {
	assert(e);

	/* Type 1 (chest) and type 2 (furnace) carts do not mount -- Beta's
	 * interact() branches entirely differently for them (displayGUIChest /
	 * coal-fuel). This client-local prediction hook is MOUNT-ONLY (the ride
	 * intent RPC + optimistic occupied=true below); for the other two types
	 * every bit of the client-visible effect is server-authoritative via
	 * SRPC_ENTITY_USE (screen_ingame.c's send-gate excludes only THIS type-0
	 * path from that RPC, so the two are mutually exclusive, never both-fired).
	 * Early-return BEFORE any of the mount logic so a fresh (default type 0)
	 * cart's behaviour is provably unchanged -- this is the ONLY edit inside
	 * onRightClick for the whole slice. [740c583: EntityMinecart.interact] */
	if(e->data.minecart.minecart_type != 0)
		return false;

	struct entity* player = gstate.players[0].local_player;
	if(!player) return false;

	/* Preserve the existing ownership rule: an occupied cart belonging to a
	 * different entity is not ours to dismount or predict empty. */
	if(e->data.minecart.occupied
	   && e->data.minecart.occupant_id != player->id)
		return false;

	/* F3: author a ride-intent RPC instead of toggling local occupancy.
	 * The desired state is the inverse of the current local prediction:
	 * if not riding, request mount; if riding, request dismount. The
	 * server's correlated CRPC_MINECART_OCCUPANCY reply is the authority
	 * that corrects local prediction. */
	bool desired = !e->data.minecart.occupied;

	/* Set up the centralized ride state. input_epoch is filled by the
	 * send logic in clin_update which has access to s_last_input_epoch. */
	gstate.players[0].ride.target_id = e->id;
	gstate.players[0].ride.next_request_seq++;
	if(gstate.players[0].ride.next_request_seq == 0)
		gstate.players[0].ride.next_request_seq = 1;
	gstate.players[0].ride.pending_request_seq = gstate.players[0].ride.next_request_seq;
	gstate.players[0].ride.desired_occupied = desired;
	gstate.players[0].ride.send_dirty = true;
	gstate.players[0].ride.pending = true;

	/* Optimistic local prediction for immediate feel on a Wii. The
	 * authoritative reply will correct this if the server disagrees. */
	if(desired) {
		e->data.minecart.occupied = true;
		e->data.minecart.occupant_id = player->id;
		e->data.minecart.pending_throttle = 0;
		e->data.minecart.replay_throttle = 0;
		e->data.minecart.control_ready = false;
		e->data.minecart.control_dirty = false;
		e->rightClickText = "Dismount";
	} else {
		e->data.minecart.occupied = false;
		e->data.minecart.occupant_id = 0;
		e->data.minecart.pending_throttle = 0;
		e->data.minecart.replay_throttle = 0;
		/* Clearance lift: both interpolation samples to at least
		 * cart_y + 1.0 + 1.2, recording that absolute floor. */
		float floor_y = e->pos[1] + 1.0f + 1.2f;
		if(player->pos[1] < floor_y)
			player->pos[1] = floor_y;
		if(player->pos_old[1] < floor_y)
			player->pos_old[1] = floor_y;
		gstate.players[0].ride.exit_floor_y = floor_y;
		gstate.players[0].ride.exit_lift_pending = true;
		/* Lifecycle criterion 12: explicitly set
		 * exit_repin_eligible=false on dismount authoring. It may be set
		 * true only by a future reconciliation path that actually moves the
		 * player body back onto the cart during the pending leave.
		 * With the follow/snapshot guards that should normally never
		 * happen in production -- the positive handler is exercised by
		 * a fixture that drives the explicit latch/move. */
		gstate.players[0].ride.exit_repin_eligible = false;
		e->rightClickText = "Ride";
	}

	return true;
}

static bool onLeftClick(struct entity *e) {
    assert(e);
    /* CLIENT PREDICTION: rock the cart the instant the punch lands so the hit
     * reads immediately (Beta's client gets this via handleHealthUpdate). The
     * AUTHORITATIVE damage/break lives in the server's SRPC_ENTITY_ATTACK
     * minecart branch, so send that RPC here -- screen_ingame does NOT send it
     * for us (it calls e->onLeftClick(e) and returns; the only other send site
     * is the mob's own handler, entity_monster.c onLeftClick, whose pattern
     * this follows). Without this send the server branch is unreachable
     * in-game and a punched cart can never break or drop its item. */
    e->data.minecart.shake_dir = (int8_t)-e->data.minecart.shake_dir;
    e->data.minecart.shake_ticks = 10;
    e->data.minecart.damage_taken += 10;

    svin_rpc_send(&(struct server_rpc){
        .type = SRPC_ENTITY_ATTACK,
        .payload.entity_attack.entity_id = e->id,
    });

    return true;
}

void entity_minecart(uint32_t id, struct entity* e, bool server, void* world) {
    e->name 	   = "Minecart";
    e->id          = id;
    e->tick_server = minecart_server_tick;
    e->tick_client = minecart_client_tick;
    e->render      = entity_minecart_render;
    e->teleport    = entity_default_teleport;
    e->type        = ENTITY_MINECART;
    e->getBoundingBox = getBoundingBox;
    e->leftClickText = NULL;
    e->onLeftClick   = onLeftClick;
    e->data.minecart.occupied = false;
    e->data.minecart.occupant_id = 0;
    /* G8-VEHICLE-SEAMS-01 (S1): no rider lifetime until a mount writes one. */
    e->data.minecart.rider_actor = ACTOR_IDENT_HOST;
	e->data.minecart.ride_epoch = 0;
	e->data.minecart.control_seq = 0;
	e->data.minecart.control_positive_steps = 0;
	e->data.minecart.control_negative_steps = 0;
	e->data.minecart.control_acked_seq = 0;
	e->data.minecart.control_acked_positive_steps = 0;
	e->data.minecart.control_acked_negative_steps = 0;
	e->data.minecart.control_ready = false;
	e->data.minecart.control_dirty = false;
    /* MUST be cleared here. Client carts are streamed in through
     * CRPC_SPAWN_MINECART -> s_clin_alloc (plain malloc), so this byte is
     * whatever the recycled heap block held; the tick applies it as rider input
     * on the very first frame after a mount, which reads as a cart that lurches
     * off on its own. The two flags above were already initialised -- this
     * field was added later and missed the initialiser. */
    e->data.minecart.pending_throttle = 0;
    e->data.minecart.replay_throttle = 0;
    /* Same malloc trap as pending_throttle above (audit F5): the rolling-sound
     * odometer must start at zero, not at recycled heap bytes -- a garbage
     * float here (or NaN) makes the first moving tick misfire or go silent. */
    e->data.minecart.roll_dist = 0.0f;
    /* Same malloc trap again: break-state must start clean or a fresh cart can
     * spawn pre-shaken or pre-damaged from recycled heap. This block belongs
     * in the constructor -- it previously sat in minecart_roll_sound's parked
     * branch, so moving carts kept recycled-heap state (root cause of the
     * flaky test_minecart_authority poison-construction failure) and parked
     * carts had their punch feedback erased every tick. */
    e->data.minecart.damage_taken = 0;
    e->data.minecart.blast_break_pending = false;
    e->data.minecart.shake_ticks = 0;
    e->data.minecart.shake_dir = 1;
	e->data.minecart.snapshot_last_tick = 0;
	e->data.minecart.snapshot_dirty = false;
	e->data.minecart.snapshot_urgent = false;
    /* Malloc-trap precedent (audit F5) applies to every field added since: a
     * client cart is streamed through s_clin_alloc's plain malloc, so an
     * uninitialised byte here is recycled heap, not zero. minecart_type
     * defaults to 0 (rideable); the real value is installed right after this
     * constructor returns by whichever caller knows it (server spawn, client
     * CRPC_SPAWN_MINECART apply, or the archive restore path) -- all three
     * also re-derive rightClickText below once the type is known. */
    e->data.minecart.minecart_type = 0;
    e->data.minecart.push_x = 0.0f;
    e->data.minecart.push_z = 0.0f;
    e->data.minecart.fuel = 0;
    memset(e->data.minecart.inventory, 0, sizeof(e->data.minecart.inventory));
    e->rightClickText = e->data.minecart.occupied ? "Dismount" : "Ride";
    e->onRightClick   = onRightClick;

    // zero vectors
    glm_vec3_zero(e->pos);
    glm_vec3_zero(e->pos_old);
    glm_vec3_zero(e->vel);
    glm_vec2_zero(e->orient);
    glm_vec2_zero(e->orient_old);

    e->data.minecart.hx = 0;
    e->data.minecart.hz = 0;
	e->data.minecart.rail_meta = 0xFF;
    e->data.minecart.last_meta = 0xFF;

    e->data.minecart.item.id         = ITEM_MINECART;
    e->data.minecart.item.durability = 0;
    e->data.minecart.item.count      = 1;
    e->data.minecart.speed = 0.0f;

    entity_default_init(e, server, world);
    /* I03 (board #8): the cart's own Random (Entity.java:96), the lifetime
     * stream every server entity gets (world seed, dimension, id). */
    if (server && world) {
        const struct server_world* w = world;
        entity_lifetime_rng_seed(&e->gameplay_rng, w->seed, w->dimension, id);
    }
	/* EntityMinecart: setSize(.98,.7), yOffset=height/2. */
	entity_set_size(e, 0.98F, 0.7F, 0.35F);
}
