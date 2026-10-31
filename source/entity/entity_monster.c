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
    along with CavEX. If not, see <http://www.gnu.org/licenses/>.
*/

#include "../block/blocks.h" /* block_world: the dispatch (entity tick) world */
#include <assert.h>
#include <math.h>
#include "entity.h"
#include "entity_monster.h"
#include "combat.h"
#include "los.h"
#include "pathfinding.h" // FH-13: Beta 1.7.3 A* (source/entity/pathfinding.h)
#include "../light_brightness.h" // light_lookup_overworld: Beta-faithful floored table
#include "../daytime.h" // daytime_is_daytime_ticks: byte-pinned isDaytime() port (undead sun-burn outer gate)
#include "../block/block_pressure_plate.h" // audit #24: block_pressure_plate_on_entity_tick
#include "../block/blocks_data.h"
#include "../network/server_local.h"
#include "../network/server_local_guest.h" // G8-VEHICLE-SEAMS-01: guest seat correction
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_weather.h" // Entity.isWet rain exposure
#include "../network/client_interface.h"
#include "../network/server_interface.h"
#include "../world.h"
#include "../graphics/render_entity.h"
#include "../graphics/render_entity_anchor.h" // render_entity_death_tilt (G8)
#include "../platform/audio.h" // S0: SOUND_* ids + audio_mob_sounds
#include "../platform/gfx.h"
#include "../util.h"
#include "../particle.h"
#include "../game/game_state.h"
#include "../network/server_world.h"
#include "../item/items.h"
#include "entity_wolf.h"
#include "mob_motion.h" // mob motion units S1
#include "../block/redstone_piston_server.h" // piston_moving_state_at (S2 collide env)
#include <string.h>

static void monster_retire_dead_windup_cue(struct entity* e,
                                          struct server_local* s);

/* The client-visible burning flag. Fire-immune mobs publish the value Beta's
 * Entity.java:226 computed mid-tick (stored in e->on_fire by the fire branch,
 * mob_immune_fire_flag); every other mob keeps the end-of-tick fire_time > 0. */
bool monster_published_on_fire(const struct entity* e) {
    return monster_def_get(e->data.monster.id)->fire_immune
        ? e->on_fire : e->data.monster.fire_time > 0;
}

enum monster_damage_result entity_monster_apply_damage(
    struct entity* e, int incoming, bool from_skeleton_arrow) {
    /* incoming >= 0: a ZERO-damage hit is Beta-real (the thrown egg's
     * zero-damage attackEntityFrom from the thrower -- hurt + knockback, no health change).
     * See the M4 S5 note in mob_iframe_damage. */
    assert(e && e->type == ENTITY_MONSTER && incoming >= 0);

    /* EntityLiving.attackEntityFrom resets entityAge before either the dead-body
     * guard or the resistance arbitration. [740c583: EntityLiving] */
    e->data.monster.entity_age = 0;
    if (e->health <= 0)
        return MONSTER_DAMAGE_ABSORBED;

    bool fresh;
    int applied = mob_iframe_damage(incoming,
        &e->data.monster.hurt_resist_time,
        &e->data.monster.last_damage, &fresh);
    /* A fresh 0-damage hit is NOT absorbed: Beta returns true, arming hurtTime
     * and knockback with a zero damageEntity. Only a non-fresh zero (in-window or
     * negative) absorbs. */
    if (applied == 0 && !fresh)
        return MONSTER_DAMAGE_ABSORBED;
    if (fresh)
        e->data.monster.hurt_time = MOB_HURT_ANIMATION_TICKS;

    e->health -= (short)applied;
    if(e->health <= 0)
        e->data.monster.windup_cue.phase = 0;
    if(e->health <= 0 && e->on_server && e->world
       && e->data.monster.last_sent_move_valid
       && e->data.monster.last_sent_move.windup_cue.phase != 0)
        monster_retire_dead_windup_cue(e, server_local_of_entity(e));
    e->data.monster.hit_by_skeleton_arrow = from_skeleton_arrow;

    /* S0: Beta plays the hurt/death sound ONLY on the fresh branch (the fresh flag
     * false for an excess-only in-window hit -- even a lethal one): death
     * sound when the hit left health <= 0, else hurt. Volume is
     * getSoundVolume() (1.0; cow overrides to 0.4), pitch 1.0 plus 0.2 x a roll difference,
     * at the mob (mob yOffset is 0 -> feet). A fresh ZERO-damage hit (the
     * thrown egg) plays the hurt sound too, exactly like Beta. The pitch
     * roll draws two floats from the mob's OWN Random (its rand field) -- before
     * onDeath's dropFewItems rolls on that same stream (LIVING-SOUND-RNG).
     * [server EntityLiving.java:340 death, :345 hurt] */
    if (fresh) {
        uint16_t hurt_sid, death_sid;
        float mob_vol;
        audio_mob_sounds(e->data.monster.id, NULL, &hurt_sid, &death_sid,
                         &mob_vol);
        server_local_emit_sound(
            e->health <= 0 ? death_sid : hurt_sid,
            (vec3) {e->pos[0], e->pos[1], e->pos[2]}, mob_vol,
            mob_living_sound_pitch(e));
    }
    return fresh ? MONSTER_DAMAGE_FRESH : MONSTER_DAMAGE_EXCESS;
}

// Null/environmental and ordinary mob damage is halved for wolves before the
// shared EntityLiving i-frame gate. [740c583: EntityWolf.attackEntityFrom]
// Exported (weather slice): lightning strike damage on a mob is a null-
// attacker environmental hit exactly like lava/fire (Entity.dealFireDamage ->
// attackEntityFrom with a null source), so it reuses this SAME wolf-aware path rather
// than duplicating the halving check.
enum monster_damage_result monster_apply_environment_damage(
	struct entity* e, struct server_local* s, int incoming) {
	if(entity_is_wolf(e))
		return entity_wolf_attack_from(e, s, incoming, UINT32_MAX,
			false, false, false, ACTOR_IDENT_HOST);
	return entity_monster_apply_damage(e, incoming, false);
}

// ---- next-gaps-0924 W1B: Beta 1.7.3 mob movement parity (ice / ladder /
// suffocation). The pure arithmetic lives in entity_monster.h so the golden
// (tests/test_mob_move_parity_golden.c) pins it; these three are the world
// reads that feed it.

// The block under the feet box: getBlockId at the floored X, the floored box
// minimum Y minus 1, and the floored Z [EntityLiving.moveEntityWithHeading:482,493]. The mob
// yOffset is 0, and this engine's physics box sits at pos + y_size
// (entity_try_move_axis / entity.c:939), so bb.minY == pos[1] + y_size. Unloaded
// / out-of-range reads as 0 (Beta World.getBlockId returns 0 there too).
/* MathHelper.floor_double. */
static int32_t mob_floor_d(double v) {
	int32_t i = (int32_t)v;
	return v < (double)i ? i - 1 : i;
}

/* Mob motion units S2: the probes read the authoritative doubles --
 * floor(posX), floor(boundingBox.minY), floor(posZ) exactly as Beta, not the
 * float mirror (whose rounding can cross an integer). */
static int mob_block_under_id(struct server_local* s, const struct entity* e) {
	struct block_data under;
	const struct mob_motion* md = &e->data.monster.motion_d;
	w_coord_t bx = mob_floor_d(md->pos[0]);
	w_coord_t by = mob_floor_d(md->bb.min[1]) - 1;
	w_coord_t bz = mob_floor_d(md->pos[2]);
	return server_world_get_block(block_world(s), bx, by, bz, &under) ? under.type
																  : 0;
}

// EntityLiving.isOnLadder():550-555 -- the SAME cell test the player uses:
// ladder (id 65) at (floor x, floor(bb.minY), floor z).
static bool mob_on_ladder(struct server_local* s, const struct entity* e) {
	struct block_data cell;
	const struct mob_motion* md = &e->data.monster.motion_d;
	w_coord_t bx = mob_floor_d(md->pos[0]);
	w_coord_t by = mob_floor_d(md->bb.min[1]);
	w_coord_t bz = mob_floor_d(md->pos[2]);
	return server_world_get_block(block_world(s), bx, by, bz, &cell)
		&& cell.type == BLOCK_LADDER;
}

// Entity.isEntityInsideOpaqueBlock (Entity.java:851-863), behaviour:
//     visits the eight corners of a small box centred on the eye, ordered by
//     the three low bits of the corner index (bit 0 picks the X side, bit 1
//     the Y side, bit 2 the Z side). Each side sits half the extent from the
//     centre: the X and Z extent is 0.9 of the entity width, the Y extent is
//     0.1. Each corner offset is computed in float, added to the position in
//     double (eye height included on Y) and floored to a block coordinate.
//     The first corner whose cell is a normal cube makes the answer true;
//     if none is, the answer is false.
//
//
// Eight corner samples in a (0.9*width) x 0.1 x (0.9*width) box around the eye
// (mob getEyeHeight = height*0.85 -> monster_def.eye_height, the same table the
// LOS and drown probes read; width = monster_def.box_w, Beta's setSize width).
// World.isBlockNormalCube (World.java:1636-1639) is true when the material is opaque
// and the block renders as a normal cube -- block_is_normal_cube (blocks.c) is this port's
// existing transcription of exactly that predicate (glass / ice / leaves /
// snow layer / cactus are translucent materials, slabs / stairs / fences /
// ladders are not normal-rendering cubes -- none of them suffocate).
// Beta short-circuits on the first opaque corner, so a free head costs up to 8
// block reads per mob per tick. T1 (bit-exact): the sample box is only
// 0.9*width x 0.1 x 0.9*width (0.54 x 0.1 x 0.54 for a biped), so the eight
// corners almost always share ONE cell -- a corner whose cell was already read
// this call is skipped. isBlockNormalCube is a pure function of the cell, every
// skipped cell was already found non-opaque, and the first-true return order is
// unchanged, so the verdict is byte-identical to the 8-read loop; the world
// read count drops to 1 in the common case (2-4 on a cell boundary).
bool entity_monster_inside_opaque_block_at(struct server_local* s, float x,
										   float y, float z, float width,
										   float eye) {
	w_coord_t seen[8][3];
	int seen_n = 0;
	for(int corner = 0; corner < 8; ++corner) {
		float off_x = ((float)((corner >> 0) % 2) - 0.5F) * width * 0.9F;
		float off_y = ((float)((corner >> 1) % 2) - 0.5F) * 0.1F;
		float off_z = ((float)((corner >> 2) % 2) - 0.5F) * width * 0.9F;
		w_coord_t cx = (w_coord_t)floor((double)x + (double)off_x);
		w_coord_t cy = (w_coord_t)floor((double)y + (double)eye + (double)off_y);
		w_coord_t cz = (w_coord_t)floor((double)z + (double)off_z);
		bool dup = false;
		for(int k = 0; k < seen_n; k++)
			if(seen[k][0] == cx && seen[k][1] == cy && seen[k][2] == cz) {
				dup = true;
				break;
			}
		if(dup)
			continue;
		seen[seen_n][0] = cx;
		seen[seen_n][1] = cy;
		seen[seen_n][2] = cz;
		seen_n++;
		struct block_data cell;
		if(server_world_get_block(block_world(s), cx, cy, cz, &cell)
		   && block_is_normal_cube(cell.type))
			return true;
	}
	return false;
}

// The tick's call: geometry from the mob def (width = setSize width, eye =
// getEyeHeight = height*0.85, the same table the LOS/drown probes read).
static bool mob_inside_opaque_block(struct server_local* s,
									const struct entity* e) {
	const struct monster_def* def = monster_def_get(e->data.monster.id);
	return entity_monster_inside_opaque_block_at(s, e->pos[0], e->pos[1],
												 e->pos[2], def->box_w,
												 def->eye_height);
}

// Entity.handleLavaMovement: World.isMaterialInBB tests the box shrunk by 0.1 / 0.4 / 0.1
// (x/y/z, float values widened) for lava -- the same contracted-body scan the lava
// hazard block runs; factored so the ladder gate can ask it for a fire-immune
// mob (whose hazard block skips the scan, Beta's damage being fully gated on
// !isImmuneToFire there) without a second copy of the bounds.
//
// MOBLAVA-0925: the scanned box is Beta's boundingBox, whose minY is
// posY - yOffset + ySize [Entity.setPosition, Entity.java:132], NOT posY. This
// engine keeps pos primary and the collision box at pos + y_size (entity.c:102,
// the water scan at the hazard block reads the same minY), so a mob carrying a
// banked step/hop pop (y_size > 0) scans from its BOX, one release-tail above
// its observable pos -- the same minY the water scan and the burning-box scan
// use. Pinned by tests/test_mob_lava_aabb_golden.c (cases Y1/Y2).
static bool mob_body_in_lava(struct server_local* s, const struct entity* e) {
	const struct monster_def* def = monster_def_get(e->data.monster.id);
	const float half_width = def->box_w / 2.0F;
	const double inset_xz = (double)0.1F;
	const double inset_y = (double)0.4F;
	const struct mob_motion* md = &e->data.monster.motion_d;
	const double box_min_y = md->bb.min[1];
	return server_world_aabb_has_lava(block_world(s),
		md->pos[0] - (double)half_width + inset_xz,
		box_min_y + inset_y,
		md->pos[2] - (double)half_width + inset_xz,
		md->pos[0] + (double)half_width - inset_xz,
		box_min_y + (double)def->box_h - inset_y,
		md->pos[2] + (double)half_width - inset_xz);
}

/* Mob motion units S2: the attackEntity leap on the doubles -- the Beta
 * arithmetic is combat.h's mob_leap_impulse_beta (shared with the tests). */
static void mob_leap_impulse_d(struct mob_motion* md, double dx, double dz) {
	mob_leap_impulse_beta(dx, dz, md->motion);
}

/* World.getCollidingBoundingBoxes' entity half for a mob mover: every other
 * entity's getBoundingBox() inside the query box grown by 0.25 that intersects the
 * query. Only EntityBoat and EntityMinecart override getBoundingBox() in
 * Beta 1.7.3, and only the BOAT's returns a box [CFR EntityBoat.java:59-61];
 * EntityMinecart.getBoundingBox() returns null [CFR EntityMinecart.java:81-83],
 * so a cart is never solid to a mob's move (it pushes through its own
 * applyEntityCollision instead -- G2). This engine also exposes monster and
 * cart boxes through the same accessor (placement solidity, rider/hit
 * tests), which is why the admit is by type: boats only (G3). The mover's
 * getCollisionBox is null for every EntityLiving, so that half adds nothing. */
struct mob_env_ctx {
	struct server_local* s;
};
static void mob_env_entity_boxes(void* ctx, const struct entity* self,
								 const struct mob_aabb_d* q,
								 struct mob_box_list* out) {
	struct server_local* s = ((struct mob_env_ctx*)ctx)->s;
	struct mob_aabb_d wide = *q;
	for (int i = 0; i < 3; i++) {
		wide.min[i] -= 0.25;
		wide.max[i] += 0.25;
	}
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	uint32_t visited = 0; /* Fable S2 F2: the walk cost, counted */
	while (!dict_entity_end_p(it)) {
		struct entity* o = dict_entity_ref(it)->value;
		dict_entity_next(it);
		visited++;
		if (!o || o == self || o->delay_destroy == 0 || !o->getBoundingBox
			|| o->type != ENTITY_BOAT
			|| server_local_entity_world(s, o)
				!= server_local_entity_world(s, (struct entity*)self))
			continue;
		struct AABB b;
		if (o->getBoundingBox(o, &b) == 0)
			continue;
		struct mob_aabb_d bd = {{b.x1, b.y1, b.z1}, {b.x2, b.y2, b.z2}};
		if (mob_aabb_intersects(&wide, &bd) && mob_aabb_intersects(q, &bd))
			mob_box_list_push(out, &bd);
	}
	mob_motion_cost_note_env(visited);
}
static bool mob_env_piston(void* ctx, int32_t x, int32_t y, int32_t z,
						   struct piston_moving_state* out) {
	return piston_moving_state_at(((struct mob_env_ctx*)ctx)->s, x, y, z, out);
}
static struct mob_env_ctx s_mob_env_ctx;
static struct mob_collide_env mob_collide_env_for(struct server_local* s) {
	s_mob_env_ctx.s = s;
	return (struct mob_collide_env) {
		.entity_boxes = mob_env_entity_boxes,
		.entity_ctx = &s_mob_env_ctx,
		.piston = mob_env_piston,
		.piston_ctx = &s_mob_env_ctx,
	};
}
struct mob_collide_env entity_monster_collide_env(struct server_local* s) {
	return mob_collide_env_for(s);
}

/* Entity.moveEntity dispatches block-collision callbacks only when every
 * horizontal chunk touched by the final AABB is resident. The scan uses the
 * post-move box and Beta's exact +/-0.001 inclusive bounds/order. Cactus is the
 * damaging callback relevant to EntityLiving here; keep scanning after a
 * lethal callback, as Beta does. [740c583: Entity.moveEntity, BlockCactus]
 *
 * Audit #24: this SAME per-cell scan is also Beta's trigger for
 * BlockPressurePlate.onEntityCollidedWithBlock -- every mob whose post-move
 * box overlaps a plate's cell dispatches into it every tick, exactly like the
 * cactus damage callback above. Name/doc kept cactus-scoped (a full rename
 * touching every external caller is its own, separately-provable refactor
 * commit per CODESTYLE 3.2/3.3, deferred rather than bundled with this fix);
 * the dispatch itself is the real, complete Beta wiring, not a stub. */
void entity_monster_post_move_cactus_collisions(
    struct entity* e, struct server_local* s) {
    assert(e && s);

    // Mob motion units S2: the floored box minimum plus 0.001 up to
    // the floored maximum minus 0.001, on the authoritative box [Entity.java:
    // 461-466].
    const struct mob_aabb_d* bb = &e->data.monster.motion_d.bb;
    w_coord_t x0 = mob_floor_d(bb->min[0] + 0.001);
    w_coord_t y0 = mob_floor_d(bb->min[1] + 0.001);
    w_coord_t z0 = mob_floor_d(bb->min[2] + 0.001);
    w_coord_t x1 = mob_floor_d(bb->max[0] - 0.001);
    w_coord_t y1 = mob_floor_d(bb->max[1] - 0.001);
    w_coord_t z1 = mob_floor_d(bb->max[2] - 0.001);

    if (y1 < 0 || y0 >= WORLD_HEIGHT)
        return;

    w_coord_t cx0 = WCOORD_CHUNK_OFFSET(x0);
    w_coord_t cz0 = WCOORD_CHUNK_OFFSET(z0);
    w_coord_t cx1 = WCOORD_CHUNK_OFFSET(x1);
    w_coord_t cz1 = WCOORD_CHUNK_OFFSET(z1);
    /* Entity.moveEntity: residency and collision callbacks query worldObj. */
    struct server_world* w = server_local_entity_world(s, e);
    if (!w)
        return;
    for (w_coord_t cx = cx0; cx <= cx1; cx++)
        for (w_coord_t cz = cz0; cz <= cz1; cz++)
            if (!dict_server_chunks_get(
                    w->chunks, S_CHUNK_ID(cx, cz)))
                return;

    for (w_coord_t x = x0; x <= x1; x++)
        for (w_coord_t y = y0; y <= y1; y++)
            for (w_coord_t z = z0; z <= z1; z++) {
                struct block_data blk;
                if (!server_world_get_block(block_world(s), x, y, z, &blk))
                    continue;
                if (blk.type == BLOCK_CACTUS)
                    (void)monster_apply_environment_damage(
                        e, s, MOB_CACTUS_DAMAGE);
                // next-gaps-0924/W2 P12: [BlockWeb.java:10-12] isInWeb = true
                if (blk.type == BLOCK_COBWEB)
                    e->in_web = true;
                // Audit #24: [740c583: BlockPressurePlate.onEntityCollidedWithBlock]
                if (blk.type == BLOCK_STONE_PRESSURE_PLATE
                    || blk.type == BLOCK_WOOD_PRESSURE_PLATE)
                    block_pressure_plate_on_entity_tick(s, x, y, z);
            }
}




// Creeper movement constants
#define CREEPER_ACCEL            0.25f    // acceleration per tick
#define CREEPER_MAX_SPEED        1.25f    // max horizontal speed per tick
#define CREEPER_WANDER_INTERVAL  20      // ticks between direction changes
#define YAW_SMOOTH_FACTOR        0.2f    // smoothing factor [0..1]

// Local bounding box for collision (centered, half-height offset)
void make_creeper_bbox(struct AABB* out) {
    const float sx = 0.6f, sy = 1.8f, sz = 0.6f;
    aabb_setsize_centered_offset(out, sx, sy, sz, 0.0f, sy * 0.5f, 0.0f);
}

// Mob collision box (mob motion units S2): the authoritative box is Beta's
// boundingBox on the doubles (mob_motion.h, Entity.setPosition from
// width/height/yOffset/ySize); the per-id float box thunks of the retired
// float mover are gone. getBoundingBox_creeper below is the float raycast /
// placement box, built from the same width/height (entity_make_world_bbox).

// True iff the world cell (x,y,z) holds a solid (collidable) block -- the same
// "has a bounding box" test entity_block_aabb_test / the collision sweep use, so the
// skeleton back-off probe agrees with the physics about what counts as ground. An
// out-of-world / unloaded / air / non-collidable (plant, fluid) cell is NOT solid.
static bool mob_cell_is_solid(struct server_world* w, w_coord_t x, w_coord_t y,
                              w_coord_t z) {
    struct block_data blk;
    if(!server_world_get_block(w, x, y, z, &blk))
        return false; // out of world / unloaded
    struct block* b = blocks[blk.type];
    if(!b)
        return false; // air / unregistered
    struct block_info bi = {.block = &blk, .neighbours = NULL,
                            .x = x, .y = y, .z = z};
    return b->getBoundingBox(&bi, true, NULL) > 0;
}

// True iff the world cell (x,y,z) is lava (flow or still) -- the contact-damage
// hazard the back-off must not retreat into.
static bool mob_cell_is_lava(struct server_world* w, w_coord_t x, w_coord_t y,
                             w_coord_t z) {
    struct block_data blk;
    if(!server_world_get_block(w, x, y, z, &blk))
        return false;
    return blk.type == BLOCK_LAVA_FLOW || blk.type == BLOCK_LAVA_STILL;
}

// Skeleton back-off hazard gate (audit #17e). The keep-distance steer is the only mob
// steer that drives a mob AWAY from the player (backward), and this engine has NO
// edge/cliff avoidance and treats lava as contact-damage only -- so an unguarded
// back-off could walk a cornered skeleton off a ledge or into lava. Probe the ONE cell
// the mob would back into: away_x/away_z is the unit AWAY-from-player heading (-radial).
// Return false (suppress the back-off; the steer orbits in place instead) when that
// destination cell -- or the cell it would fall to -- is LAVA, or when stepping there is
// a DROP (the would-stand cell is non-solid AND the cell below it is also non-solid =
// air over a fall, i.e. no ground to land on). A 1-block step-down onto solid ground is
// allowed (that is not a hazard). This is a cheap 2-3 cell probe, not pathfinding.
static bool skeleton_backoff_safe(struct server_world* w, float fx, float fy,
                                  float fz, float away_x, float away_z) {
    // The cell the mob's feet would occupy one block back along the away heading.
    w_coord_t bx = (w_coord_t)floorf(fx + away_x);
    w_coord_t by = (w_coord_t)floorf(fy);
    w_coord_t bz = (w_coord_t)floorf(fz + away_z);
    // Lava in the step cell or the cell just below it -> never back off there.
    if(mob_cell_is_lava(w, bx, by, bz) || mob_cell_is_lava(w, bx, by - 1, bz))
        return false;
    // If the step cell itself is solid, the mob can't enter it anyway (it would just
    // slide / auto-jump) -- not a fall hazard, so the back-off is safe to attempt.
    if(mob_cell_is_solid(w, bx, by, bz))
        return true;
    // Step cell is passable: it is a DROP only if there is no ground directly under it.
    // Solid one block down = a safe step-down/level walk; non-solid = air over a fall.
    return mob_cell_is_solid(w, bx, by - 1, bz);
}

// Client-side tick: interpolate and smooth orientation. Generic across all
// hostile mobs (the creeper explosion-smoke tail only fires when a fuse is
// running, which currently only creepers ever have).
// Defined below with the SP idle-voice scheduler; the MP client-side idle
// block in client_tick_monster shares it.
static int mob_talk_interval(int monster_id);

// Beta EntityLiving.updatePlayerActionState's idle-look FALLBACK (the branch
// EntityCreature reaches by deferring to the EntityLiving update whenever it
// has no active path this tick -- entity_monster.c's mob_path_follow_tail is
// that shared no-locomotion tail on the server side; this is its CLIENT-side
// render analogue). A 2% per-tick roll acquires the local viewer as a look
// target while within 8 blocks (Beta's getClosestPlayerToEntity at 8 blocks,
// reduced to "the one CaveWii player" per aggro_time's own single-player-
// target convention -- entity.h) and smoothly turns to face them (yaw rate
// 10 deg/tick via mob_turn_toward); absent a target, a 5% roll re-picks a
// random yaw drift (+/-10 deg) added every tick until re-rolled.
// [740c583: EntityLiving.updatePlayerActionState] Purely cosmetic (moveForward/
// moveStrafing/direction are untouched -- no attack or acquisition decision
// reads this), so it draws from the CLIENT's own non-deterministic cosmetic
// stream (gstate.rand_src, the same one particle.c's scatter uses) rather
// than the mob's server-side gameplay_rng: this fix draws NO additional RNG from
// the simulation stream. Feet-to-feet distance basis, matching this file's
// own established Y-basis convention (see server_tick_monster's chase_dy
// comment) for consistency with the rest of the mob-AI distance gates.
static void mob_idle_look_tick(struct entity* e) {
    struct entity_monster* m = &e->data.monster;
    float dx = gstate.players[0].camera.x - e->pos[0];
    float dy = gstate.players[0].camera.y - PLAYER_EYE_HEIGHT - e->pos[1];
    float dz = gstate.players[0].camera.z - e->pos[2];
    float dist_sq = dx * dx + dy * dy + dz * dz;
    struct random_gen* rng = &gstate.rand_src;

    if(rand_gen_flt(rng) < MOB_IDLE_ACQUIRE_CHANCE) {
        if(dist_sq < MOB_IDLE_LOOK_RANGE_SQ) {
            m->idle_has_target = true;
            m->idle_target_ticks = 10 + rand_gen_int(rng, 20);
        } else {
            m->idle_random_yaw_vel
                = (rand_gen_flt(rng) - 0.5f) * MOB_IDLE_RANDOM_YAW_RANGE;
        }
    }

    if(m->idle_has_target) {
        float target_yaw = atan2f(dx, dz); // this file's atan2f(dx,dz) head_yaw convention
        m->head_yaw = mob_turn_toward(m->head_yaw, target_yaw,
                                      MOB_IDLE_YAW_TURN_RATE);
        if(--m->idle_target_ticks <= 0 || dist_sq >= MOB_IDLE_LOOK_RANGE_SQ)
            m->idle_has_target = false;
    } else {
        if(rand_gen_flt(rng) < MOB_IDLE_YAW_REROLL_CHANCE)
            m->idle_random_yaw_vel
                = (rand_gen_flt(rng) - 0.5f) * MOB_IDLE_RANDOM_YAW_RANGE;
        m->head_yaw += m->idle_random_yaw_vel;
    }
}

static bool client_tick_monster(struct entity* e) {
    assert(e);

    glm_vec3_copy(e->pos, e->pos_old);
    /* Beta increments approach (remote: lands exactly in <=3 ticks) or the
     * local same-tick snap -- replaces a 0.3f exponential decay that never
     * landed and made teleports glide. Position only: mob head yaw is
     * driven below by network_head_yaw/network_head_increments (a SEPARATE
     * dedicated pair, not network_orient/network_increments -- see entity.h's
     * network_head_yaw comment) when a real wire look is armed, else
     * movement-derived. [740c583 EntityLiving.onLivingUpdate] */
    entity_network_approach_tick(e, false);

    vec3 delta;
    entity_get_delta(e, delta);
    float speed = sqrtf(delta[0]*delta[0] + delta[2]*delta[2]);
    // HARDEN AUDIT #9 (cosmetic): a mob in its audit-#18 death-roll (death_time > 0)
    // must NOT gait its legs under the flop -- Beta freezes the walk cycle the instant a
    // mob dies and tips it over. The walk-anim (limb_swing + frame advance) keys off
    // `speed`, so force speed to 0 while dying: limb_swing then lerps toward 0 (legs
    // settle to rest) and entity_tick_animation holds the frame. The position lerp above
    // is untouched, so the corpse still slides/settles to its networked pose.
    bool dying = e->type == ENTITY_MONSTER && e->data.monster.death_time > 0;
    if (dying)
        speed = 0.0f;

    // MP client-side idle voices (remote_session only -- in SP the SAME
    // scheduler runs server-side below in mob_tick and this gate keeps the
    // two exclusive). Beta rolls this in EntityLiving.onEntityUpdate with NO
    // multiplayer gate (the multiplayer-world fire-zeroing sits two
    // lines below it, so the gating there is deliberate): every client ticks
    // the ramp on ITS OWN unsynced Random for every remote living entity --
    // MP ambient grunts are client-rolled, never wire events. Same linear
    // ramp (fires when the 0-999 roll is under the post-incremented counter), reset to -talk interval,
    // volume getSoundVolume() (cow 0.4, ghast 10), pitch 1.0 plus 0.2 x a roll difference
    // [playLivingSound], at the mob's feet. The dying gate mirrors the SP
    // tick's own death early-out. Rolls draw from the client cosmetic stream
    // (audio_cosmetic_rng) -- never a sim RNG.
    // [740c583: EntityLiving.onEntityUpdate:105-111 / .playLivingSound]
    if (gstate.remote_session && !dying
        && rand_gen_int(audio_cosmetic_rng(), 1000)
            < e->data.monster.living_sound_time++) {
        e->data.monster.living_sound_time
            = -mob_talk_interval(e->data.monster.id);
        uint16_t idle_sid;
        float mob_vol;
        audio_mob_sounds(e->data.monster.id, &idle_sid, NULL, NULL, &mob_vol);
        if (entity_is_wolf(e)) {
            // EntityWolf.getLivingSound's own nextInt(3), fired only after
            // the scheduler; wolf_flags/wolf_watched_health are wire-fed.
            // [740c583: EntityWolf.getLivingSound]
            switch (wolf_living_sound_pick(
                e->data.monster.wolf_flags,
                e->data.monster.wolf_watched_health,
                rand_gen_int(audio_cosmetic_rng(), 3))) {
            case WOLF_LIVING_GROWL: idle_sid = SOUND_MOB_WOLF_GROWL; break;
            case WOLF_LIVING_WHINE: idle_sid = SOUND_MOB_WOLF_WHINE; break;
            case WOLF_LIVING_PANTING: idle_sid = SOUND_MOB_WOLF_PANTING; break;
            case WOLF_LIVING_BARK:
            default: idle_sid = SOUND_MOB_WOLF_BARK; break;
            }
        }
        float wobble = (rand_gen_flt(audio_cosmetic_rng())
                        - rand_gen_flt(audio_cosmetic_rng())) * 0.2f + 1.0f;
        audio_play3d(idle_sid, (vec3) {e->pos[0], e->pos[1], e->pos[2]},
                     mob_vol, wobble); // no-ops on AUDIO_SOUND_NONE ids
    }
    if (e->data.monster.network_head_increments > 0) {
        /* MP residual item 2 (2026-08-26): a real Packet32/33/34 look arrived
         * (client_interface.c's CRPC_ENTITY_LOOK armed this pair) -- approach
         * it via Beta's own increments mechanism, shortest-way-around, scoped
         * entirely to this dedicated pair so it never touches
         * network_orient/network_increments (position-only for a mob) or the
         * player-frame e->orient convention. [740c583 NetClientHandler.
         * handleEntity rotating flag] */
        float dyaw = e->data.monster.network_head_yaw - e->data.monster.head_yaw;
        while(dyaw < -(float)M_PI)
            dyaw += 2.0f * (float)M_PI;
        while(dyaw >= (float)M_PI)
            dyaw -= 2.0f * (float)M_PI;
        e->data.monster.head_yaw
            += dyaw / (float)e->data.monster.network_head_increments;
        e->data.monster.network_head_increments--;
    } else if (monster_is_slime(e->data.monster.id) && gstate.players[0].local_player
              && slime_face_player_yaw(e->pos[0], e->pos[1], e->pos[2],
                                       gstate.players[0].local_player->pos[0],
                                       gstate.players[0].local_player->pos[1],
                                       gstate.players[0].local_player->pos[2],
                                       &e->data.monster.head_yaw)) {
        /* FIX (mob-AI wave, slime): Beta EntitySlime.updatePlayerActionState
         * calls faceEntity on the player (yaw step 10, pitch 20) UNCONDITIONALLY every tick a
         * player is within 16 blocks -- before/independent of the hop-delay
         * gate (server_tick_monster's SLIME hop block, this file), so a
         * grounded slime between hops keeps turning to track the player.
         * CaveWii's mob head yaw is deliberately movement-derived client-side
         * (entity.h's network_head_yaw comment) rather than server-networked,
         * so this slots in at the SAME layer as the generic fallback below,
         * ahead of it: ModelSlime has no head joint (render_entity_slime:
         * headYawDeg is unused, body yaw only), so setting head_yaw here is
         * enough -- entity_blend_body_to_head (below) already eases body_yaw
         * toward it every tick, standing in for Beta's own +-10deg/tick
         * updateRotation clamp. A slime beyond 16 blocks (or before the local
         * player exists) falls through to the ordinary movement-derived
         * heading, same as before this fix. [740c583:
         * EntitySlime.updatePlayerActionState] */
    } else if (speed > 0.001f) {
        /* Beta's own "non-rotating" fallback: a plain move with no
         * accompanying look re-arms the increments toward the entity's OWN
         * current rotation (a no-op target) rather than a wire value -- this
         * movement-derived approximation stands in for that same case, and
         * for every non-networked (SP) mob, where network_head_increments is
         * always 0. [740c583 NetClientHandler.handleEntity] */
        e->data.monster.head_yaw = atan2f(delta[0], delta[2]);
    } else if (!dying && !monster_is_slime(e->data.monster.id)
               && e->data.monster.id != MONSTER_SQUID
               && e->data.monster.id != MONSTER_GHAST) {
        // Genuinely idle (not moving, no networked look armed, not flopping
        // dead -- Beta's isMovementBlocked() skips updatePlayerActionState
        // entirely once health<=0): head-turn-only locomotion instead of a
        // frozen stare. See mob_idle_look_tick above.
        //
        // EXCLUDED: EntitySlime, EntitySquid, EntityGhast each fully
        // override updatePlayerActionState with their own mob-specific AI
        // and never call super (verified via decompile) -- Beta's generic
        // idle-look fallback never reaches them. Slime already faces the
        // nearest player unconditionally within 16 blocks as part of its own
        // hop logic [740c583: EntitySlime.updatePlayerActionState] (a
        // separate, pre-existing mechanic, untouched here); squid has no
        // look-at-player behaviour at all [740c583:
        // EntitySquid.updatePlayerActionState]; ghast runs its own flight/
        // attack state machine. EntityWolf DOES call
        // the inherited action-state update first [740c583:
        // EntityWolf.updatePlayerActionState], so wolves correctly still
        // reach this branch.
        mob_idle_look_tick(e);
    }
    // Walk-anim amplitude (Beta limbSwingAmount): lerp toward 1 while moving, 0
    // when stopped, so the legs settle to rest instead of freezing mid-stride.
    float swing_target = (speed > 0.015f) ? 1.0f : 0.0f;
    e->data.monster.limb_swing
        += (swing_target - e->data.monster.limb_swing) * 0.2f;
    // HURT FLAIL. Beta slams the walk amplitude to 1.5F on damage -- DELIBERATELY
    // ABOVE the 1.0F ceiling the lerp above can ever reach -- and feeds it
    // straight into the limb swing, so a hit mob visibly scrambles its legs for
    // a few ticks while its AI does nothing at all. Without this a hit animal
    // just keeps its normal stride and reads as oddly inert; it is the single
    // most visible part of "something happened" on a passive mob, which has no
    // flee AI in Beta (verified: EntityAnimal has none, and Beta has no flee
    // primitive at all -- a hit cow keeps wandering, and that IS faithful).
    //
    // The trigger is the RISING EDGE of the networked hurt flag, because Beta's
    // client sets this from a one-shot packet (handleHealthUpdate case 2 ->
    // legYaw set to 1.5), not continuously. A second hit inside the 10-tick window
    // does not re-slam -- which also matches Beta, where an in-window hit that
    // is not strictly stronger is absorbed and never reaches the setter.
    // Applied AFTER the lerp so this tick shows 1.5 and the following ticks
    // decay it back down, exactly like Beta's onLivingUpdate.
    // [740c583: EntityLiving.attackEntityFrom / .handleHealthUpdate / :546]
    if(e->hurt && !e->data.monster.hurt_prev)
        e->data.monster.limb_swing = 1.5f;
    e->data.monster.hurt_prev = e->hurt;
    entity_blend_body_to_head(&e->data.monster.body_yaw,
                              e->data.monster.head_yaw,
                              YAW_SMOOTH_FACTOR);

    e->orient[0] = 0.0f;
    e->orient[1] = e->data.monster.body_yaw + e->data.monster.head_yaw;

    entity_tick_animation(e, speed, 60);
    // (A creeper's detonation puff is emitted via CRPC_ENTITY_EXPLODE on this same
    // render thread -- see client_interface.c. The old health<=0 && fuse>0 block here
    // was dead: the client never receives health or fuse changes that way.)

    // Creeper swell warning: puff explosion-smoke while the fuse runs, so the
    // player gets a visible ~1.5s cue to back out of the blast before detonation.
	if (e->swelling) {
        particle_generate_explosion_smoke(
            (vec3) {e->pos[0], e->pos[1] + 0.8f, e->pos[2]}, 0.8f);
    }

    return false;
}

// AUDIT #18 ITEM 8: server_explode() was DELETED here. It was orphaned dead scaffold
// (zero callers anywhere in source/, no header declaration -- it just cleared a sphere of
// blocks and passed NULL to server_world_get_block) left over from early explosion WIP
// (it even carried non-English placeholder comments). The LIVE, Beta-faithful explosion is
// server_world_explode() (source/network/server_world.c) -- it does the proper
// explosion_half_hearts entity damage and is what the creeper detonation, the queued-
// explosion tick, and TNT all call. Removing the duplicate kills a misleading second
// "explosion" entry point.

// LOS opacity predicate for los_segment_blocked: a voxel BLOCKS the ray iff it holds
// a real, solid LOS-occluding block. Context is the server world. The Beta-faithful
// occlusion RULE (incl. why slab/stair/farmland block but glass/leaves/fluids don't)
// lives in los.h::los_block_occludes, which this forwards the three relevant registry
// flags to -- one rule, unit-tested directly, no copy here to drift. A cell outside
// the world / in an unloaded chunk returns false from server_world_get_block ->
// treated as air (fail-open) so a mob near an unloaded frontier isn't permanently
// blinded.
// NOT static (entity_monster.h): despite the name, this predicate is just "world LOS
// opacity, wired to server_world_get_block" -- nothing mob-specific about it. Extracted
// to a shared symbol (CODESTYLE.md §3.2, second-transcription rule) when server_local.c
// needed the identical Beta canEntityBeSeen predicate for the boat SRPC_ENTITY_USE/
// SRPC_ENTITY_ATTACK reach gates, rather than re-deriving a second copy that could drift.
bool mob_los_cell_opaque(int x, int y, int z, void* ctx) {
	struct server_world* w = (struct server_world*)ctx;
	struct block_data blk;
	if(!server_world_get_block(w, (w_coord_t)x, (w_coord_t)y, (w_coord_t)z, &blk))
		return false; // out of world / unloaded -> passable (air), fail-open
	struct block* b = blocks[blk.type];
	if(!b)
		return false; // air / unregistered id -> passable
	return los_block_occludes(b->can_see_through, b->opacity, b->place_ignore);
}

// Beta 1.7.3 EntityLiving.canEntityBeSeen: true iff the eye-to-eye segment from the
// mob's eye to the player's eye is not interrupted by an opaque block. The mob eye
// is feet (e->pos[1]) + the per-type eye height (biped 1.53 / spider 0.765); the
// player network pos IS the eye, so it is used raw. This is the FAITHFULNESS gate a
// through-wall mob is missing -- a mob that acquires / shoots / primes / hits the
// player through a solid wall is a BUG, not a Beta behavior. Callers must distance-
// gate first (LOS is only meaningful inside the 16-block follow range); this walks a
// voxel DDA per call so it should not run every tick for out-of-range mobs.
/* S1-a2: the player is now a PARAMETER, not s->player. Beta canEntityBeSeen
 * takes the entity it is asked about [740c583 EntityLiving.canEntityBeSeen];
 * with a local split guest "the player" is whichever actor this mob targets,
 * so the caller passes the resolved target -- or, for the wolf OWNER-follow
 * teleport probe, the owner, which is the host by definition because tame
 * ownership stays the persistent owner-name field. */
static bool mob_can_see_player(struct server_local* s, struct entity* e,
							   const struct server_player* p) {
	// TODO(perf): LOS DDA laziness/caching (skip/cache the per-cell opacity walk between
	// ticks for a stationary mob+player). Deferred -- premature for a 0.3ms tick.
	float eye = mob_eye_height(e->data.monster.id);
	return !los_segment_blocked(
		e->pos[0], e->pos[1] + eye, e->pos[2],
		(float)p->x, (float)p->y, (float)p->z,
		mob_los_cell_opaque, block_world(s));
}

// Same eye-to-eye LOS primitive as mob_can_see_player, generalized to an arbitrary
// target point instead of the player: Beta's canEntityBeSeen is defined on Entity/
// EntityLiving generically (decompile-verified, 740c583:EntityLiving.java:73-75:
// a block ray trace from its eye to the other entity's eye), not player-specific --
// EntityCreature.updatePlayerActionState dispatches attackEntity() under this same
// gate no matter what playerToAttack actually is. Used for the wolf's non-player
// bite target (wolf_target_id can name another entity -- a CaveWii extension beyond
// Beta's player-only EntityWolf.findPlayerToAttack), so that path gets the identical
// wall-blocking discipline mob_can_see_player already gives the player-target case.
static bool mob_can_see_point(struct server_local* s, struct entity* e, float tx,
							  float ty, float tz) {
	float eye = mob_eye_height(e->data.monster.id);
	return !los_segment_blocked(e->pos[0], e->pos[1] + eye, e->pos[2], tx, ty, tz,
								mob_los_cell_opaque, block_world(s));
}

/* Beta b1.7.3 EntityLiving.onDeath/dropFewItems (RetroMCP client :407-433)
 * calls Entity.entityDropItem (:840-844), which constructs AND joins in
 * the dropping entity's own world. Every species and chicken egg uses its own resolved world.
 * Preserve the slot-0/SP entry point, including legacy fixture bindings. */
static struct entity* mob_spawn_item(struct entity* e, struct server_local* s,
    vec3 pos, struct item_data* item) {
    struct server_world* w = server_local_entity_world(s, e);
    return w == &s->worlds[0]
        ? server_local_spawn_item(pos, item, true, s)
        : server_local_spawn_item_w(w, pos, item, true);
}

// AUDIT #18 ITEMS 3+4: EntityLiving.onDeath/dropFewItems is distinct from the
// earlier onEntityUpdate deathTime increment. A fall/cactus kill in moveEntity
// drops immediately but remains at deathTime 0 until the next tick. Keep a
// separate persisted guard so that exact edge cannot duplicate loot on tick or
// reload. drop_pos is the entity's final death-site position.
void entity_monster_begin_death(
    struct entity* e, struct server_local* s, vec3 drop_pos) {
    assert(e && s && e->type == ENTITY_MONSTER);
    if (!e->data.monster.death_loot_dropped) {
        e->data.monster.death_loot_dropped = true;
        // Death edge: Beta onDeath -> dropFewItems drops nextInt(3) of the item HERE,
        // BEFORE the 20-tick removal countdown. Roll the count with the seeded stream.
        // EntitySheep.dropFewItems:23-28 overrides this with no count roll.
        // EntityLiving.dropFewItems:424-433 draws only for a positive item id.
        // Squid/skeleton overrides have positive C item ids and retain their rolls.
        int count_roll = (e->data.monster.id == MONSTER_SHEEP || e->drop_item.id == 0)
            ? 0 : jrand_next_int_bound(&e->gameplay_rng, MOB_DROP_COUNT_DEN);
        int count = mob_drop_count(count_roll);
        // Beta EntitySquid.dropFewItems overrides the base 0-2 with
        // a 0-2 roll plus one -> 1-3 ink sacs (the only +1 drop roll in the
        // mob set; decompile-verified). Same single roll, shifted.
        if (e->data.monster.id == MONSTER_SQUID)
            count = count_roll + 1;
        if (e->data.monster.id == MONSTER_SHEEP) {
            // Beta 1.7.3 EntitySheep.dropFewItems: drop EXACTLY 1 wool block (NOT nextInt(3)) of
            // the fleece colour, ONLY if !getSheared() -- a sheared sheep's death drops NOTHING
            // Its override draws no RNG. The colour is the same persisted
            // per-entity colour the render tint uses, so the dropped wool matches
            // the rendered fleece. item_data has no `metadata` field -- the wool
            // colour rides `durability` (block_wool getDroppedItem copies the metadata into durability).
            // [740c583: EntitySheep.dropFewItems drops one wool of the fleece colour only when unsheared]
            if (!e->data.monster.sheared) {
                struct item_data drop = e->drop_item; // {id=BLOCK_WOOL, ...}
                drop.id = BLOCK_WOOL;
                drop.durability = e->data.monster.fleece_color;
                drop.count = 1;
                // FH-12: EntitySheep.dropFewItems spawns the wool via entityDropItem,
                // which sets a 10-tick pickup delay. [740c583: Entity.entityDropItem]
                struct entity* wool_death_drop
                    = mob_spawn_item(e, s, drop_pos, &drop);
                if (wool_death_drop)
                    wool_death_drop->data.item.pickup_delay = 10;
            }
        } else if (count > 0 && e->drop_item.id != 0) {
            // No-item species skip both the count draw above and item emission.
            struct item_data drop = e->drop_item; // copy: keep the def count untouched
            drop.count = (uint8_t)count;
            // Beta 1.7.3 pig getDropItemId: a pig that died BURNING drops COOKED porkchop
            // instead of raw -- the only on-fire drop swap in 1.7.3 (cow leather + chicken
            // feather have no cooked form). fire_time>0 = the mob is on fire (lava/fire;
            // animals never sun-burn).
            if (e->data.monster.id == MONSTER_PIG && e->data.monster.fire_time > 0)
                drop.id = ITEM_PORKCHOP_COOKED;
            // FH-12: EntityLiving.dropFewItems drops each item via dropItem,
            // dropItemWithOffset and entityDropItem, which sets a pickup delay of
            // 10 ticks. [740c583: EntityLiving.java, Entity.entityDropItem]
            struct entity* mob_death_drop
                = mob_spawn_item(e, s, drop_pos, &drop);
            if (mob_death_drop)
                mob_death_drop->data.item.pickup_delay = 10;
        }
        // count == 0 (non-sheep) -> Beta drops nothing (the loop body never runs); no empty stack.

        // PARITY FIX (2026-09-01): Beta 1.7.3 EntitySkeleton.dropFewItems is a COMPLETE
        // OVERRIDE of EntityMob.dropFewItems (it never calls super) and draws TWO
        // INDEPENDENT nextInt(3) rolls: the first is the arrows roll already reproduced
        // above via the generic count>0/drop_item path (skeleton's .drop=ITEM_ARROW, so
        // that branch already IS Beta's arrow half, byte-for-byte -- same nextInt(3),
        // same drop loop); the second, separate nextInt(3) rolls 0-2 BONES and is drawn
        // UNCONDITIONALLY, regardless of how many arrows the first roll produced. The
        // old monsters_object.c comment claiming "bones were added in Beta 1.8" is
        // factually wrong -- the 1.7.3-LTS decompile (740c583) shows bones dropping
        // in-tree. Without this, ITEM_BONE was unobtainable in survival (no worldgen/
        // dungeon loot table drops it either), permanently blocking wolf taming
        // (entity_wolf.c requires a held bone) and the bone->dye recipe.
        // STREAM NOTE: this is an ADDITIONAL draw on the entity rand_src stream beyond
        // every other mob's single roll -- the entity stream is not golden-pinned like
        // worldgen's (no equivalence/replay test depends on its exact length), so this
        // is a documented stream-length change, not a forbidden one.
        // [740c583: EntitySkeleton.java dropFewItems -- rolls a count in 0..2 on
        // the mob's own Random and drops that many single arrows, then rolls a
        // second, independent count in 0..2 and drops that many single bones,
        // each drop going through the ordinary one-item drop path.]
        if (e->data.monster.id == MONSTER_SKELETON) {
            int bone_roll = jrand_next_int_bound(&e->gameplay_rng, MOB_DROP_COUNT_DEN);
            int bone_count = mob_drop_count(bone_roll);
            if (bone_count > 0) {
                struct item_data bones = {
                    .id = ITEM_BONE,
                    .count = (uint8_t)bone_count,
                    .durability = 0,
                };
                // FH-12: EntityLiving.dropFewItems -> dropItem -> dropItemWithOffset ->
                // entityDropItem, which sets a 10-tick pickup delay.
                struct entity* bone_drop
                    = mob_spawn_item(e, s, drop_pos, &bones);
                if (bone_drop)
                    bone_drop->data.item.pickup_delay = 10;
            }
        }

        // Beta EntityCreeper.onDeath: a creeper whose KILLING BLOW was a
        // skeleton-fired arrow ADDITIONALLY drops one music record --
        // record_13 or cat, a fresh 50/50 roll (Beta's bound-2 integer draw). The
        // attribution flag is set by the arrow's mob-hit and cleared by every
        // other damage source, so on the death edge it reflects exactly the
        // killing blow. Gunpowder still drops above; this is IN ADDITION.
        if (e->data.monster.id == MONSTER_CREEPER
            && e->data.monster.hit_by_skeleton_arrow) {
            struct item_data record = {
                .id = (uint16_t)(ITEM_RECORD_13
                                 + jrand_next_int_bound(&e->gameplay_rng, 2)),
                .count = 1,
                .durability = 0,
            };
            // FH-12: EntityCreeper.dropFewItems drops the record via dropItem
            // and entityDropItem, which sets a 10-tick pickup delay.
            // [740c583: EntityCreeper.java]
            struct entity* record_drop
                = mob_spawn_item(e, s, drop_pos, &record);
            if (record_drop)
                record_drop->data.item.pickup_delay = 10;
        }
    }
}

static bool mob_die_tick(struct entity* e, struct server_local* s, vec3 drop_pos) {
    entity_monster_begin_death(e, s, drop_pos);
    e->data.monster.death_time++;          // Beta ++deathTime each tick health<=0
    if (mob_death_finished(e->data.monster.death_time)) {
        // Beta EntitySlime.setEntityDead SPLIT (decompile-verified,
        // 740c583:EntitySlime.java):
        //     on a non-multiplayer world, a slime larger than size 1 whose
        //     health is exactly zero splits into four smaller slimes.
        //
        // The gate is size above 1 AND health equal to 0 -- STRICT equality, not
        // <=0. deathTime reaching the flop threshold only proves health <= 0
        // at SOME earlier tick, not health == 0 at THIS one: overkill damage
        // (e.g. a big weapon hit that drives health well negative in one
        // shot, per EntityLiving.damageEntity's unclamped health
        // subtraction) reaches this same reap edge with health < 0, and Beta does NOT
        // split that slime. The prior version of this code split on
        // `child >= 0` alone (size>1) with no health check, so ANY death --
        // including overkill -- always split; a big enough hit denied the
        // player the intended "chip damage doesn't skip splits" cost. Gate
        // on `e->health == 0` (the CaveWii mirror of Beta's health field,
        // also unclamped -- see entity_monster_apply_damage's plain
        // `e->health -= applied`) to match exactly. size==1 (child < 0) is
        // covered separately as before. `!multiplayerWorld` needs no mirror:
        // mob_die_tick only ever runs on the authoritative server_local tick.
        // The children are QUEUED, not spawned inline (the documented
        // deferred pattern beside pending_arrows -- the snapshot tick loop
        // tolerates mid-tick inserts, but the queue keeps the CRPC order
        // parent-DESTROY-then-child-SPAWNs deterministic and the children
        // unticked this tick). Beta also rolls a random yaw per child; v2
        // monsters have no spawn yaw (the wander re-roll picks a heading on
        // tick one), so no draw is consumed.
        {
            int child = slime_child_id(e->data.monster.id);
            if (slime_should_split(child, e->health)) {
                int sz = slime_beta_size(e->data.monster.id); // parent size 2|4
                for (int i = 0; i < 4; i++) {
                    float dx, dz;
                    slime_split_offset(i, sz, &dx, &dz);
                    server_local_queue_monster_w(
                        s, server_local_entity_world(s, e),
                        (vec3) {e->pos[0] + dx, e->pos[1] + 0.5f,
                                e->pos[2] + dz},
                        child);
                }
            }
        }
        e->delay_destroy = 0;              // deathTime > 20 -> setEntityDead
        return true;                       // reap THIS tick (no extra 21st flop frame)
    }
    return false;                          // still flopping -- keep the corpse
}

// S2: Beta EntityLiving.getTalkInterval -- 120 for EntityAnimal (pig, sheep,
// cow, chicken) AND EntityWaterMob (squid), 80 for everything else (the four
// EntityMob hostiles and the slimes, which extend EntityLiving directly).
// [740c583: EntityLiving.getTalkInterval; EntityAnimal; EntityWaterMob]
static int mob_talk_interval(int monster_id) {
    return (monster_is_passive(monster_id)
            || monster_is_water_creature(monster_id))
        ? 120
        : 80;
}

// LIVING-SOUND-RNG: one EntityLiving.onEntityUpdate living-sound step,
// factored so the tick calls it at Beta's exact point (after
// super.onEntityUpdate's fire/lava, before suffocation/drowning).
// --- S2 IDLE-VOICE scheduler (Beta EntityLiving.onEntityUpdate) --------
// each tick, a 0-999 roll below the post-incremented sound counter resets the counter
// to minus the talk interval and plays the living sound -- a LINEAR probability ramp
// from the last utterance, NOT a flat interval (P = n/1000 on the n-th
// tick past the interval; expected gap ~ interval + sqrt(2000) ticks).
// Runs for EVERY living mob exactly like Beta (the ramp ticks even for
// voiceless ids -- creeper/squid/slime idle are AUDIO_SOUND_NONE and the
// emit no-ops). LIVING-SOUND-RNG: the nextInt(1000) roll, the wolf's
// getLivingSound nextInt(3) and the playLivingSound pitch floats are ALL
// the mob's rand field -- its own stream, drawn on the logical (server)
// world every tick -- so they come from e->gameplay_rng in Beta order
// (roll; [wolf pick]; pitch a; pitch b). Volume getSoundVolume()
// (cow 0.4F), at the mob's feet.
// [server EntityLiving.java:104-106 onEntityUpdate, :93-96 playLivingSound;
//  EntityWolf.getLivingSound]
void entity_monster_living_sound_tick(struct entity* e) {
    if (jrand_next_int_bound(&e->gameplay_rng, 1000)
        < e->data.monster.living_sound_time++) {
        e->data.monster.living_sound_time
            = -mob_talk_interval(e->data.monster.id);
        uint16_t idle_sid;
        float mob_vol;
        audio_mob_sounds(e->data.monster.id, &idle_sid, NULL, NULL, &mob_vol);
		if(entity_is_wolf(e)) {
			// EntityWolf.getLivingSound owns a second nextInt(3) only after the
			// living-sound scheduler fires, on the wolf's own Random, and it
			// is evaluated BEFORE the pitch floats (getLivingSound runs first
			// in playLivingSound). [EntityWolf.getLivingSound]
			switch(wolf_living_sound_pick(
				e->data.monster.wolf_flags,
				e->data.monster.wolf_watched_health,
				jrand_next_int_bound(&e->gameplay_rng, 3))) {
			case WOLF_LIVING_GROWL: idle_sid = SOUND_MOB_WOLF_GROWL; break;
			case WOLF_LIVING_WHINE: idle_sid = SOUND_MOB_WOLF_WHINE; break;
			case WOLF_LIVING_PANTING: idle_sid = SOUND_MOB_WOLF_PANTING; break;
			case WOLF_LIVING_BARK:
			default: idle_sid = SOUND_MOB_WOLF_BARK; break;
			}
		}
        // Beta playLivingSound draws the pitch only when getLivingSound()
        // is non-null: voiceless mobs (creeper/squid/slime idle) skip both
        // floats. [server EntityLiving.java:93-97]
        if (idle_sid != AUDIO_SOUND_NONE)
            server_local_emit_sound(idle_sid,
                                    (vec3) {e->pos[0], e->pos[1], e->pos[2]},
                                    mob_vol, mob_living_sound_pitch(e));
    }
}

// ================================================================================
// FH-13: Beta 1.7.3 A* pathfinding integration [740c583: EntityCreature,
// Pathfinder, World.getPathToEntity/getEntityPathToXYZ]. The search core itself
// (source/entity/pathfinding.{c,h}) is pure and world-free; everything here is the
// wiring layer: the world-query predicate, the getBlockPathWeight port (the
// wander-target scorer), and the waypoint-follow steer that replaces the old
// mob_chase_dir beeline / random-heading wander in server_tick_monster below.
// ================================================================================

// One shared search scratch instance, reused across every mob's search this tick
// (CODESTYLE.md 1.7: no allocation in the per-tick hot path). pathfinder_reset() is
// called at the top of every pathfinder_create_path_to_* call, so reuse is
// behaviourally identical to Beta building a fresh Pathfinder on every call (pathfinding.h
// header note 4).
static struct pathfinder s_pathfinder_scratch;

// Pathfinder.getVerticalOffset's per-cell classification [740c583], wired to the
// live server world: air/open-door -> PASSABLE, closed door -> SOLID (Beta's door
// special-case), water/lava materials -> WATER/LAVA, everything else falls back to
// this engine's own "does this block occupy space" test -- getBoundingBox(entity=
// true) returning any box, the SAME collision predicate entity.c's physics/shadow
// code already uses (there is no separate Beta-style Material.getIsSolid table in
// this engine). An unloaded/out-of-world cell fails CLOSED (SOLID): unlike LOS
// (mob_los_cell_opaque, which fails open so a mob near an unloaded frontier isn't
// blinded), a pathfinding search failing open into unresolved terrain could route a
// mob through ground that turns out solid once the chunk loads -- failing closed
// just means the search stops early (documented overflow-adjacent policy,
// pathfinding.h note 1: "mob stands", never a bad route). get_piston_moving stays
// NULL, mirroring server_local.c's own sanctioned "orphan id36 -> no box" pattern.
static enum pathfinder_cell_kind mob_path_cell_query(int32_t x, int32_t y,
													 int32_t z, void* ctx) {
	struct server_world* w = (struct server_world*)ctx;
	struct block_data blk;
	if(!server_world_get_block(w, (w_coord_t)x, (w_coord_t)y, (w_coord_t)z, &blk))
		return PATH_CELL_SOLID;
	if(blk.type == 0)
		return PATH_CELL_PASSABLE;
	if(blk.type == BLOCK_DOOR_WOOD || blk.type == BLOCK_DOOR_IRON) {
		// [740c583: Pathfinder.getVerticalOffset door branch + BlockDoor.isOpen]
		return (blk.metadata & 0x04) ? PATH_CELL_PASSABLE : PATH_CELL_SOLID;
	}
	if(blk.type == BLOCK_WATER_FLOW || blk.type == BLOCK_WATER_STILL)
		return PATH_CELL_WATER;
	if(blk.type == BLOCK_LAVA_FLOW || blk.type == BLOCK_LAVA_STILL)
		return PATH_CELL_LAVA;
	struct block* b = blocks[blk.type];
	if(!b)
		return PATH_CELL_PASSABLE; // unregistered id: treat as air, defensive only
	struct block_info info = (struct block_info) {
		.block = &blk,
		.x = (w_coord_t)x,
		.y = (w_coord_t)y,
		.z = (w_coord_t)z,
	};
	return b->getBoundingBox(&info, true, NULL) > 0 ? PATH_CELL_SOLID
													 : PATH_CELL_PASSABLE;
}

// World.getLightBrightness for a block position [740c583: the world provider's brightness table indexed by
// the block light value at x,y,z] -- the SAME max(torch, sky-subtract)-clamped-then-
// table-lookup mechanism entity_monster.c already wires three times above (undead
// sunburn, spider daytime passivity x2), extracted here as a fourth, standalone use
// for the wander-target scorer below (CODESTYLE.md 3.2's second-transcription rule
// -- past due, but out of scope to retrofit the other three call sites in this
// lane). Unlike those, this samples the CANDIDATE cell directly (no per-mob body-
// height offset -- Beta's getBlockPathWeight passes the raw sampled coordinate).
static float mob_path_weight_light(struct server_local* s, w_coord_t x, w_coord_t y,
								   w_coord_t z) {
	// next-gaps-0924/F1: World.getLightBrightness indexes getBlockLightValue (World.java:688-690),
	// the shared helper -- neighbour max + weather darkening; a non-resident cell reads 0.
	int eff = server_local_block_light_value(s, x, y, z);
	int idx = eff < 0 ? 0 : (eff > 15 ? 15 : eff);
	return light_lookup_overworld[idx];
}

// EntityMob.getBlockPathWeight [740c583]: 0.5 minus the cell's light brightness --
// hostiles prefer DARKER wander targets (higher weight = more attractive). Every
// EntityMob hostile (zombie/skeleton/creeper/spider/pig zombie) inherits this
// unmodified.
static float mob_path_weight_hostile(struct server_local* s, w_coord_t x,
									 w_coord_t y, w_coord_t z) {
	return 0.5f - mob_path_weight_light(s, x, y, z);
}

// EntityAnimal.getBlockPathWeight [740c583]: grass directly underfoot (the cell at
// y-1) is a flat 10 (irresistibly attractive); otherwise the light brightness of
// the cell minus 0.5 -- animals prefer LIGHT, the opposite sign from EntityMob's
// hostiles. Every passive animal (pig/sheep/cow/chicken) AND the wolf (EntityWolf
// extends EntityAnimal, no override) inherit this unmodified.
static float mob_path_weight_animal(struct server_local* s, w_coord_t x, w_coord_t y,
									w_coord_t z) {
	struct block_data under;
	if(server_world_get_block(block_world(s), x, y - 1, z, &under)
	   && under.type == BLOCK_GRASS)
		return 10.0f;
	return mob_path_weight_light(s, x, y, z) - 0.5f;
}

// EntityCreature.updateWanderPath [740c583]: sample 10 candidate cells (posX +
// nextInt(13)-6, posY + nextInt(7)-3, posZ + nextInt(13)-6, in that draw order,
// EVERY iteration unconditionally -- 30 draws total, matching Beta's discarded-draw
// discipline) and keep the single best-weighted one (STRICT `>`, so an EARLIER tie
// wins -- do not "fix" to `>=`). On a hit, replaces the mob's path with a fresh
// search toward that point at Beta's own MOB_WANDER_SEARCH_RANGE (combat.h).
// `is_animal_lineage` selects EntityAnimal's grass/light scorer over EntityMob's
// darkness scorer (see the two weight functions above).
static void mob_acquire_wander_path(struct server_local* s, struct entity* e,
									bool is_animal_lineage) {
	int32_t best_x = 0, best_y = 0, best_z = 0;
	float best_w = -99999.0f;
	bool found = false;
	for(int i = 0; i < 10; i++) {
		int32_t cx = (int32_t)floorf(
			e->pos[0] + (float)jrand_next_int_bound(&e->gameplay_rng, 13) - 6.0f);
		int32_t cy = (int32_t)floorf(
			e->pos[1] + (float)jrand_next_int_bound(&e->gameplay_rng, 7) - 3.0f);
		int32_t cz = (int32_t)floorf(
			e->pos[2] + (float)jrand_next_int_bound(&e->gameplay_rng, 13) - 6.0f);
		float w = is_animal_lineage
			? mob_path_weight_animal(s, cx, cy, cz)
			: mob_path_weight_hostile(s, cx, cy, cz);
		if(w > best_w) {
			best_w = w;
			best_x = cx;
			best_y = cy;
			best_z = cz;
			found = true;
		}
	}
	if(found)
		pathfinder_create_path_to_xyz(&s_pathfinder_scratch, mob_path_cell_query,
									  block_world(s), e->pos[0], e->pos[1], e->pos[2],
									  e->width, e->height, best_x, best_y, best_z,
									  MOB_WANDER_SEARCH_RANGE,
									  &e->data.monster.path, NULL);
}

// World.getPathToEntity toward the target, 16-block search [740c583], EntityCreature's chase
// acquisition/re-acquisition call. target_x/y/z is the ABSOLUTE world position of
// the chase target (player or, for a wolf, its attack target) -- callers already
// compute this as `e->pos + chase_d{x,y,z}`.
static void mob_acquire_chase_path(struct server_local* s, struct entity* e,
								   float target_x, float target_y, float target_z) {
	pathfinder_create_path_to_entity(&s_pathfinder_scratch, mob_path_cell_query,
									 block_world(s), e->pos[0], e->pos[1], e->pos[2],
									 e->width, e->height, target_x, target_y,
									 target_z, MOB_FOLLOW_RANGE,
									 &e->data.monster.path, NULL);
}

// next-gaps-0924/P7 (WORLD lane; separate function, MOVE lane's file): SpawnerAnimals.
// performSleepSpawning's fresh-pathfinder path search from the mob to the player, range 32
// [740c583 SpawnerAnimals.java:171,225] for a mob that is not an entity yet -- the same
// scratch pathfinder and world-query predicate as mob AI (no second 60 KB scratch in
// .bss). mob_x/z are the mob's centre, mob_y its feet; target is the sleeper's
// posX / boundingBox.minY / posZ.
bool monster_path_to_entity(struct server_local* s, float mob_x, float mob_y,
							float mob_z, float mob_width, float mob_height,
							float target_x, float target_y, float target_z,
							float search_range, struct path_entity* out_path) {
	return pathfinder_create_path_to_entity(&s_pathfinder_scratch, mob_path_cell_query,
											block_world(s), mob_x, mob_y, mob_z, mob_width,
											mob_height, target_x, target_y, target_z,
											search_range, out_path, NULL);
}

// PathEntity waypoint-consumption + steer [740c583: EntityCreature.
// updatePlayerActionState, the branch taken while a path is held]. Advances `path`'s
// cursor past every waypoint already within (width*2)^2 of (pos_x,pos_z) (Beta's
// arrival-advance loop), then writes a UNIT XZ heading toward the next remaining
// waypoint into out_x/out_z and returns true; on false (path empty/just-exhausted),
// out_x/out_z are left untouched (0.0f from path_entity_clear having already run,
// or simply unset on the length==0 precondition) -- the caller (mob_path_follow_tail)
// decides how to react: a same-tick exhaustion decays the mob's EXISTING heading
// (matching Beta's persisted, still-decaying moveForward -- see the FIX comment at
// that call site), never the flat zero mob_chase_dir's contract used to guarantee.
// Clears `path` (path length set to 0, Beta's path cleared) when the last waypoint is
// consumed. SCOPE NOTE: Beta also turns the heading through a +-30 deg/tick yaw-
// rate clamp and derives moveForward/moveStrafing from that yaw; this engine's
// chase steering has never modelled the yaw smoothing (mob_chase_dir snapped
// straight to the beeline every tick too), so it is not added here. Beta's
// isJumping (the next waypoint above floor(boundingBox.minY + 0.5)) IS modelled
// since mob motion units S2 -- in server_tick_monster's pre-move block, which
// reads the waypoint this function leaves current. FH-13 replaces the ROUTE (beeline/random-heading ->
// waypoint-by-waypoint A*), not those pre-existing simplifications, which are
// unchanged by this port.
static bool path_follow_steer(struct path_entity* path, float width, float pos_x,
							  float pos_z, float* out_x, float* out_z) {
	if(path->length == 0 || path_entity_is_finished(path)) {
		path_entity_clear(path);
		*out_x = 0.0f;
		*out_z = 0.0f;
		return false;
	}
	float wx, wy, wz;
	path_entity_get_position(path, width, &wx, &wy, &wz);
	(void)wy; // vertical waypoint-arrival handling is out of scope (see above)
	float arrive_sq = (width * 2.0f) * (width * 2.0f);
	for(;;) {
		float ddx = wx - pos_x;
		float ddz = wz - pos_z;
		if(ddx * ddx + ddz * ddz >= arrive_sq)
			break;
		path_entity_increment(path);
		if(path_entity_is_finished(path)) {
			path_entity_clear(path);
			*out_x = 0.0f;
			*out_z = 0.0f;
			return false;
		}
		path_entity_get_position(path, width, &wx, &wy, &wz);
	}
	float ddx = wx - pos_x;
	float ddz = wz - pos_z;
	float len2 = ddx * ddx + ddz * ddz;
	if(len2 < 1.0e-4f) {
		*out_x = 1.0f; // degenerate same-column case: finite, no NaN (mob_chase_dir
		*out_z = 0.0f; // precedent)
		return true;
	}
	float inv = 1.0f / sqrtf(len2);
	*out_x = ddx * inv;
	*out_z = ddz * inv;
	return true;
}

// FH-13: shared tail for BOTH the chase and wander branches in server_tick_monster
// below (NOT the skeleton standoff branch above them, which sets direction[]
// directly via skeleton_steer and never touches `path` -- audit #17e, out of scope
// for this port; see combat.h) [740c583: updatePlayerActionState's
// gate: a path must exist AND a 1-in-100 skip roll must miss]. `is_chase` selects
// whether the pre-existing arrival-deceleration (a v2 addition layered on the old
// mob_chase_dir call site, not a Beta mechanic) still applies; `chase_dist_sq`
// (mob-to-target, computed once per tick regardless of routing) is only read when
// is_chase is true, against `in_reach_dist_sq` -- the CALLER'S own melee-reach-
// squared (MOB_ATTACK_REACH_SQ generically, WOLF_MELEE_REACH_SQ for a wolf; see
// the FIX comment below for why this must NOT be one shared constant).
static bool mob_path_follow_tail(struct server_local* s, struct entity* e,
								 bool is_chase, float chase_dist_sq,
								 float in_reach_dist_sq) {
	bool has_path = e->data.monster.path.length > 0
		&& !path_entity_is_finished(&e->data.monster.path);
	// Short-circuit like Beta: the path check runs first, so the 1-in-100 skip
	// roll draws NOTHING when there is no path.
	bool follow_roll = has_path && jrand_next_int_bound(&e->gameplay_rng, 100) != 0;
	if(follow_roll) {
		float sx = 0.0f, sz = 0.0f;
		if(path_follow_steer(&e->data.monster.path, e->width, e->pos[0], e->pos[2],
							 &sx, &sz)) {
			bool in_reach = is_chase && chase_dist_sq < in_reach_dist_sq;
			e->data.monster.direction[0] = in_reach ? 0.0f : sx;
			e->data.monster.direction[1] = in_reach ? 0.0f : sz;
			return true;
		}
		// FIX (mob-AI wave, pathfinding dead-zone -- filed task_958b34f1, first
		// observed/documented in test_wolf_follow_sit.c's LOS-gate comment):
		// path_follow_steer's own arrival loop just consumed the LAST waypoint
		// THIS tick -- Beta's next-waypoint vector became empty INSIDE the
		// has-a-path branch [740c583: EntityCreature.
		// updatePlayerActionState]. That is a DIFFERENT, non-exclusive moment
		// from the true no-path arm (which defers to the parent AI tick)
		// below: Beta never reaches the moveForward/moveStrafing zeroing reset
		// on THIS tick -- it leaves them untouched and lets the
		// UNCONDITIONAL per-tick decay of forward and strafe input by a
		// factor of 0.98 (EntityLiving.onLivingUpdate, run every tick regardless of
		// which updatePlayerActionState branch fired) decay them toward zero
		// instead. Hard-zeroing direction here (as this code used to, falling
		// into the exact same reset as the true no-path case below) was the
		// ROOT CAUSE of a real, permanent pathfinding freeze: EntityCreature's
		// recalc-gate re-acquires pathToEntity the SAME tick it goes null
		// (the keep-path test, path present AND a 1-in-20 roll missing, is
		// false, taking the unconditional re-acquire branch, whenever the mob
		// still has a live target) -- so for any mob whose width*2 arrival
		// radius (path_follow_steer) exceeds its OWN melee reach
		// (in_reach_dist_sq), the freshly re-acquired path is ALREADY inside
		// its own arrival radius and collapses again, same tick, every tick:
		// has_path never goes true again, so the old hard zero froze the mob
		// there FOREVER, just outside its own attack reach and unable to ever
		// land a hit. Hit by the wolf (0.8 wide -> 1.6 arrival vs its own
		// tighter WOLF_MELEE_REACH_SQ, 1.5F -- this is WHY in_reach_dist_sq is
		// a parameter and not one shared MOB_ATTACK_REACH_SQ: a wolf chasing
		// with the generic 2.0F reach stopped its APPROACH before it ever hit
		// this exhaustion branch, parking at a distance inside 2.0F but
		// outside its real 1.5F bite range -- an equally permanent freeze via
		// the OTHER branch above, only fixable by passing the right reach in)
		// and the spider (1.4 wide -> 2.8 arrival vs the shared 2.0F) chief
		// among the roster; a narrow biped (zombie/skeleton/creeper, 0.6 wide
		// -> 1.2 arrival < 2.0F reach) always reaches `in_reach` in the branch
		// above FIRST and never gets here. Decaying the existing heading
		// instead of zeroing it lets exactly the same residual coast Beta
		// relies on close the (small: a few tenths of a block) residual gap
		// over the next several ticks, same as Beta's own geometric
		// moveForward decay would. `in_reach` mobs are zeroed here too
		// (matching the steer branch's own in-reach stop just above) rather
		// than left to coast into the target's own hitbox -- that only
		// matters for a mob that enters reach and exhausts its path on the
		// exact same tick, and mirrors the existing v2 in_reach addition, not
		// a new mechanic.
		bool in_reach = is_chase && chase_dist_sq < in_reach_dist_sq;
		if(in_reach) {
			e->data.monster.direction[0] = 0.0f;
			e->data.monster.direction[1] = 0.0f;
		} else {
			e->data.monster.direction[0] *= 0.98f;
			e->data.monster.direction[1] *= 0.98f;
		}
		return true;
	}
	// No path this tick at all / the 1-in-100 skip roll hit: Beta's
	// fallback arm, which runs the parent AI tick and then drops the path
	// [740c583: EntityCreature.updatePlayerActionState] -- THIS is the branch
	// EntityLiving.updatePlayerActionState's hard moveStrafing/moveForward zeroing
	// reset actually belongs to (see the FIX comment above for why the
	// same-tick arrival-exhaustion case must NOT reuse this reset). It is
	// NOT a full freeze, though: EntityLiving's base idle behaviour is
	// head-turn only (a 2% roll to face a nearby player, else a 5% roll to
	// pick a random yaw drift), which client_tick_monster's mob_idle_look_tick
	// implements on the render side (this function only owns LOCOMOTION;
	// head/body facing is a client-derived cosmetic, like head_yaw's own
	// movement fallback). path_entity_clear is a no-op if already empty.
	path_entity_clear(&e->data.monster.path);
	e->data.monster.direction[0] = 0.0f;
	e->data.monster.direction[1] = 0.0f;
	return false;
}

// Server-side hostile-mob logic. Generic wander + die-drops-its-item for every
// monster type. The fuse/explosion block only runs for a creeper that has a
// fuse counting down (fuse stays -1 for all mobs until proximity detonation
// lands in the living-world milestone), so this safely drives zombie, skeleton
// and spider too — each just wanders and drops its own item on death.
/* death-motion-0927: a mob in its deathTime flop keeps moving, as in Beta.
 * EntityLiving.onUpdate:203-206 calls onLivingUpdate unconditionally;
 * isMovementBlocked (:655-657, health <= 0) only zeroes moveStrafing /
 * moveForward / isJumping (:618-622) and skips updatePlayerActionState, and
 * moveEntityWithHeading with zero input (:642) still runs -- so the killing hit's
 * knockBack carries the corpse and it falls, slides, sinks and floats under
 * the SAME branch arithmetic the live S2 tick uses (only the input-driven
 * parts are absent: no moveFlying push, no jump, no AI). Beta's order within
 * the tick: Entity.onEntityUpdate first (handleWaterMovement: the flow push,
 * the splash edge, fallDistance/fire reset in water; deathTime advancing and the
 * >20 reap -- mob_die_tick, run by the caller BEFORE this), then
 * onLivingUpdate (this function), ending with the entity push (:641-651).
 * The landing thud of EntityLiving.fall (:440-452) plays whenever
 * ceil(fallDistance - 3) > 0, even though the attackEntityFrom it follows is
 * refused for a dead body. Named residual: the squid keeps the frozen body
 * (EntitySquid's own onLivingUpdate would keep driving it with its last swim
 * vector and phase re-rolls). */
static void mob_corpse_move(struct entity* e, struct server_local* s,
                            const bool* water_known) {
    const int monster_id = e->data.monster.id;
    if (monster_id == MONSTER_SQUID)
        return;
    struct mob_motion* md = &e->data.monster.motion_d;
    const bool is_ghast = monster_id == MONSTER_GHAST;
    // Entity.onEntityUpdate:168 handleWaterMovement: the flow push and the
    // inWater field; the splash edge; water zeroes fallDistance and fire.
    // water_known: a mob killed by a hazard mid-tick already ran this tick's
    // handleWaterMovement (flow push, splash edge) -- never twice.
    const bool body_in_water = water_known ? *water_known
        : entity_handle_water_movement(
              e, md->pos[0], md->bb.min[1], md->pos[2], e->width, e->height);
    if (!water_known)
        server_local_splash_edge(s, e, body_in_water,
                                 &e->data.monster.was_in_water);
    if (body_in_water) {
        e->data.monster.fall_distance = 0.0f;
        e->data.monster.fire_time = 0;
    }
    const bool lava_movement = mob_body_in_lava(s, e);
    const bool liquid_branch = body_in_water || lava_movement;
    const bool beta_on_ground = e->on_ground;
    const int block_under
        = (!liquid_branch && beta_on_ground) ? mob_block_under_id(s, e) : 0;
    const float ground_friction = mob_beta_ground_friction(beta_on_ground, block_under);

    // moveEntityWithHeading with zero input: moveFlying with zero input adds nothing;
    // the ladder PRE-move clamp (dry branch only, never the ghast).
    const bool ladder_branch = !liquid_branch && !is_ghast;
    const bool ladder_rules = ladder_branch && monster_id != MONSTER_SPIDER;
    if (ladder_branch
        && (monster_id == MONSTER_SPIDER ? e->data.monster.prev_collided_xz
                                         : mob_on_ladder(s, e))) {
        const double ladder_cap = (double)0.15F;
        for (int ax = 0; ax < 3; ax += 2) {
            if (md->motion[ax] < -ladder_cap)
                md->motion[ax] = -ladder_cap;
            if (md->motion[ax] > ladder_cap)
                md->motion[ax] = ladder_cap;
        }
        e->data.monster.fall_distance = 0.0F;
        if (md->motion[1] < -0.15)
            md->motion[1] = -0.15;
    }

    const double pre_move_y = md->pos[1];
    struct mob_collide_env env = mob_collide_env_for(s);
    struct mob_move_result mv = mob_move_entity_d(
        e, md, md->motion[0], md->motion[1], md->motion[2], &env);
    const bool collision_xz = e->collided_horizontally;
    e->data.monster.prev_on_ground = e->on_ground;
    e->data.monster.prev_collided_xz = collision_xz;

    // Entity.updateFallState: fallDistance is reduced by dy (a downward dy adds) while airborne; on landing
    // EntityLiving.fall (the damage is refused -- the body is dead -- but the
    // landing thud still plays) and the reset.
    bool landed_in_water = body_in_water;
    if (!landed_in_water) {
        struct block_data pfoot;
        if (server_world_get_block(block_world(s), mob_floor_d(md->pos[0]),
                                   mob_floor_d(md->pos[1]),
                                   mob_floor_d(md->pos[2]), &pfoot)
            && (pfoot.type == BLOCK_WATER_FLOW || pfoot.type == BLOCK_WATER_STILL))
            landed_in_water = true;
    }
    if (landed_in_water) {
        e->data.monster.fall_distance = 0.0f;
    } else if (e->on_ground) {
        if (!monster_def_get(monster_id)->fall_immune
            && mob_fall_damage(e->data.monster.fall_distance) > 0) {
            struct block_data under;
            if (server_world_get_block(block_world(s), mob_floor_d(md->pos[0]),
                                       mob_floor_d(md->pos[1] - 0.2),
                                       mob_floor_d(md->pos[2]), &under)
                && under.type != BLOCK_AIR)
                server_local_emit_sound(
                    audio_block_step_sound_id(under.type),
                    (vec3) {(float)md->pos[0], (float)md->pos[1], (float)md->pos[2]},
                    audio_block_sound_volume(under.type) * 0.5f,
                    audio_block_sound_pitch(under.type) * 0.75f);
        }
        e->data.monster.fall_distance = 0.0f;
    } else if (!mv.incomplete && mv.dy < 0.0) {
        e->data.monster.fall_distance
            = (float)((double)e->data.monster.fall_distance - mv.dy);
    }

    // moveEntityWithHeading TAIL, per branch -- the live S2 arithmetic.
    if (is_ghast) {
        double gdrag = body_in_water ? (double)0.8F
            : (lava_movement ? 0.5 : (double)ground_friction);
        md->motion[0] *= gdrag;
        md->motion[1] *= gdrag;
        md->motion[2] *= gdrag;
    } else if (liquid_branch) {
        double k = body_in_water ? (double)0.8F : 0.5;
        md->motion[0] *= k;
        md->motion[1] *= k;
        md->motion[2] *= k;
        md->motion[1] -= 0.02;
        if (collision_xz
            && mob_offset_in_liquid(
                e, md, md->motion[0],
                md->motion[1] + (double)0.6F - md->pos[1] + pre_move_y,
                md->motion[2], &env))
            md->motion[1] = (double)MOB_WATER_CLIMB_OUT_VEL; /* 0.3 float, widened */
    } else {
        if (collision_xz
            && (monster_id == MONSTER_SPIDER
                || (ladder_rules && mob_on_ladder(s, e))))
            md->motion[1] = 0.2;
        md->motion[1] -= 0.08;
        md->motion[1] *= (double)0.98F;
        md->motion[0] *= (double)ground_friction;
        md->motion[2] *= (double)ground_friction;
    }
    // EntityChicken.onLivingUpdate after super (a dead chicken too).
    if (monster_id == MONSTER_CHICKEN && !e->on_ground && md->motion[1] < 0.0)
        md->motion[1] *= 0.6;
    mob_motion_mirror(e, md);
    // onLivingUpdate:641-651, the last statement: the corpse still pushes.
    entity_living_collide_with_nearby(e, s);
}

/* Presentation-only MOVE: never changes position/lerp, hurt, death or other
 * existing render inputs. A cue edge must not advance the ordinary MOVE gate. */
static void monster_publish_windup_cue(struct entity* e, struct server_local* s) {
    server_local_send_entity(s, e, (struct client_rpc){
        .type = CRPC_ENTITY_MOVE,
        .payload.entity_move.entity_id = e->id,
        .payload.entity_move.windup_cue = e->data.monster.windup_cue,
        .payload.entity_move.windup_cue_only = true,
    });
    e->data.monster.last_sent_move.windup_cue = e->data.monster.windup_cue;
}

/* A damage event can kill a mob after its own tick has already published.
 * Retire that advertised pose at the event, without waiting another tick. */
static void monster_retire_dead_windup_cue(struct entity* e,
                                          struct server_local* s) {
    if(s && e->health <= 0 && e->data.monster.last_sent_move_valid
       && e->data.monster.last_sent_move.windup_cue.phase != 0) {
        e->data.monster.windup_cue.phase = 0;
        monster_publish_windup_cue(e, s);
    }
}

static bool server_tick_monster(struct entity* e, struct server_local* s) {
    assert(e && s);
    bool windup_was_active = e->data.monster.windup_cue.phase != 0;
    e->data.monster.windup_cue.phase = 0;

	/* A cap-rejected terminal fuse is already a pending explosion, not a living
	 * AI decision. Retry before sunlight/drowning/death/despawn/motion so the
	 * artificial Wii pacing window cannot change its owner or blast center. The
	 * server update also runs this seam before input RPCs; this early gate protects
	 * later pending creepers when more than the per-update cap are waiting. */
	if(entity_monster_has_pending_explosion(e))
		return entity_monster_retry_pending_explosion(e, s);

    // Entity.onEntityUpdate: prevPosX/Y/Z = posX/Y/Z on the authoritative
    // doubles (mob motion units S2). The ySize ease is Beta's own now
    // (moveEntity scales ySize by 0.4 and puts the feet at box bottom plus yOffset less ySize, inside
    // mob_move_entity_d); the engine's release/bank pair is retired.
    {
        struct mob_motion* mdp = &e->data.monster.motion_d;
        for (int i = 0; i < 3; i++)
            mdp->prev_pos[i] = mdp->pos[i];
        mob_motion_mirror(e, mdp);
    }
    glm_vec3_copy(e->pos,    e->pos_old);
    glm_vec2_copy(e->orient, e->orient_old);

    // PIG ZOMBIE random anger bark [740c583: EntityPigZombie.onUpdate:
    // while the random sound delay is positive it counts down, and on reaching zero plays
    // "mob.zombiepig.zpigangry" at twice the sound volume, pitch
    // (difference of two floats * 0.2 + 1) * 1.8]. Both floats on the pig zombie's own Random,
    // BEFORE super.onUpdate -- so ahead of Entity.onEntityUpdate's fire/lava
    // hurt pitches and the idle-voice roll.
    // [server EntityPigZombie.java:18-24] includes every death-flop tick.
    if (e->data.monster.id == MONSTER_PIG_ZOMBIE
        && e->data.monster.anger_sound_delay > 0
        && --e->data.monster.anger_sound_delay == 0) {
        server_local_emit_sound(
            SOUND_MOB_ZOMBIEPIGANGRY,
            (vec3) {e->pos[0], e->pos[1], e->pos[2]}, 2.0f,
            mob_living_sound_pitch(e) * 1.8f);
    }

    // AUDIT #18 ITEM 3: a mob that is ALREADY in its death animation (health<=0 was
    // reached on a prior tick) skips ALL AI -- it just advances the deathTime flop and is
    // reaped at deathTime > 20. Loot already dropped on the edge tick, so mob_die_tick
    // won't re-drop (death_time != 0). Placed FIRST so a dying mob doesn't path, swing,
    // or take fresh hazard damage during its ~1s flop. death-motion-0927: the body is
    // NOT frozen any more -- after the reap check it runs Beta's zero-input
    // moveEntityWithHeading (mob_corpse_move), so it falls, slides and floats.
    if (e->data.monster.death_time > 0) {
        // EntityLiving.java:104 unconditionally rolls nextInt(1000) (and
        // any scheduled voice pitch) BEFORE :151-153 advances deathTime.
        // A pig-zombie corpse can be re-angered, observing this same Random.
        entity_monster_living_sound_tick(e);
        vec3 here = { e->pos[0], e->pos[1], e->pos[2] };
        // Forward mob_die_tick's reap decision as this tick's return so the main entity
        // loop removes the corpse ON the reap tick (deathTime just passed 20), not one tick
        // later -- exactly 20 visible flop ticks, matching Beta's deathTime > 20 removal.
        bool reap = mob_die_tick(e, s, here);
        if (reap) {
            // Saddle-slice review fix (finding 1): auto-dismount on mount death
            // [740c583: Entity.onEntityUpdate / Entity.updateRidden --
            // a rider whose mount is dead drops the mount reference].
            // Beta's isDead becomes true only at the deathTime>20 REMOVAL edge
            // (EntityLiving.onUpdate increments deathTime and marks the entity
            // dead once it passes 20), i.e. exactly this reap tick -- the rider
            // stays glued to the (moving) corpse through the whole ~1s flop and is
            // released only now, matching Beta's timing precisely. The entity is
            // about to be freed by the caller (CRPC_ENTITY_DESTROY below), so
            // this mainly documents intent/keeps the struct consistent for any
            // caller that inspects it before the free -- nothing else in this
            // engine tracks "what am I riding" on the player side to clean up.
            if (e->data.monster.id == MONSTER_PIG && e->data.monster.pig_ridden) {
                server_local_pig_clear_rider(e);
            }
            // Spider jockey: the SAME auto-dismount-on-death rule, applied to a
            // real entity-to-entity mount (unlike the pig/player case, BOTH
            // halves here are live struct entity records that must be
            // un-linked or the survivor keeps chasing a freed id).
            // [740c583: Entity.onEntityUpdate's drop of a dead mount
            // reference, mirrored for riddenByEntity by the
            // same reasoning -- Beta's own onEntityUpdate walks both
            // directions each tick].
            // A2-E: a jockey reference resolves only inside this mob's own live
            // world (server_local_entity_ref); a partner in another world or a
            // retired generation is released here, never written through.
            if (e->data.monster.jockey_rider_id != UINT32_MAX) {
                // This mob (about to be reaped) was a MOUNT: release its rider.
                struct entity* rider = server_local_entity_ref(
                    s, e, e->data.monster.jockey_rider_id);
                if (rider && rider->data.monster.jockey_mount_id == e->id)
                    rider->data.monster.jockey_mount_id = UINT32_MAX;
                e->data.monster.jockey_rider_id = UINT32_MAX;
            }
            if (e->data.monster.jockey_mount_id != UINT32_MAX) {
                // This mob (about to be reaped) was a RIDER: release its mount's
                // rider slot so the mount doesn't keep following a freed id.
                struct entity* mount = server_local_entity_ref(
                    s, e, e->data.monster.jockey_mount_id);
                if (mount && mount->data.monster.jockey_rider_id == e->id)
                    mount->data.monster.jockey_rider_id = UINT32_MAX;
                e->data.monster.jockey_mount_id = UINT32_MAX;
            }
            return true; // reaped this tick: CRPC_ENTITY_DESTROY fires, no move RPC needed
        }
        mob_corpse_move(e, s, NULL); // death-motion-0927: Beta keeps moving the corpse
        // Still flopping: network the dying pose (death_time drives the client flop). Keep
        // on_fire so a mob that died burning still shows flames mid-flop; no swell on a
        // dying creeper.
        server_local_send_entity(s, e, (struct client_rpc){
            .type = CRPC_ENTITY_MOVE,
            .payload.entity_move.entity_id = e->id,
            .payload.entity_move.windup_cue = e->data.monster.windup_cue,
            .payload.entity_move.pos = {e->pos[0], e->pos[1], e->pos[2]},
            .payload.entity_move.on_fire = monster_published_on_fire(e),
            .payload.entity_move.swelling = false,
            .payload.entity_move.hurt = false,
            .payload.entity_move.fuse = -1,
            .payload.entity_move.sheared = e->data.monster.sheared,
            .payload.entity_move.saddled = e->data.monster.saddled,
        .payload.entity_move.ghast_attacking = e->data.monster.ghast_attacking,
            .payload.entity_move.fleece_color = e->data.monster.fleece_color,
			.payload.entity_move.wolf_flags = e->data.monster.wolf_flags,
			.payload.entity_move.wolf_health
				= e->data.monster.wolf_watched_health,
			.payload.entity_move.wolf_shake_armed
				= e->data.monster.wolf_shake_armed,
			.payload.entity_move.wolf_shake_active
				= e->data.monster.wolf_shake_active,
			.payload.entity_move.wolf_shake_time
				= e->data.monster.wolf_shake_time,
			.payload.entity_move.wolf_prev_shake_time
				= e->data.monster.wolf_prev_shake_time,
			.payload.entity_move.wolf_head_rotation
				= e->data.monster.wolf_head_rotation,
			.payload.entity_move.wolf_head_rotation_old
				= e->data.monster.wolf_head_rotation_old,
            .payload.entity_move.death_time = (int8_t)e->data.monster.death_time,
            .payload.entity_move.charged = e->data.monster.charged,
            .payload.entity_move.pig_ridden = e->data.monster.pig_ridden,
            .payload.entity_move.pig_rider_id = e->data.monster.pig_rider_id,
        });
        return false;
    }

    // next-gaps-0924 W1B-b: the fork's entity_damp_velocity(e, 0.005f) used to
    // zero |vel X/Z| < 0.005 here. Beta 1.7.3 has NO such motion kill (the only
    // 0.005 in the decompile is the lava-texture RNG; the |motion|<0.005 zeroing
    // arrived after Beta), so a coasting mob decays geometrically under ground friction
    // like Beta. tests/test_mob_move_parity_golden.c pins the slide past 0.005
    // and measures the CRPC_ENTITY_MOVE rate of a resting mob without it.

    vec3 center = { e->pos[0], e->pos[1], e->pos[2] };

    // Beta 1.7.3: a creeper killed by the player simply dies and drops its
    // gunpowder — it does NOT explode. (Proximity-triggered detonation is the
    // living-world milestone's job; see docs/beta_parity_roadmap.md #2.) The
    // fuse mechanism below is left in place, dormant, for that work to drive
    // on player proximity rather than on death.
    // AUDIT #18 ITEM 3: instead of an instant reap, ENTER the deathTime flop -- drop loot
    // once (rolled count) and start the 20-tick removal countdown (handled at the top of
    // the NEXT tick by the death-animation block). Returning false leaves the mob alive
    // this tick; it skips the rest of the AI below.
    if (e->health <= 0) {
        // The first corpse tick also reaches EntityLiving.java:104 before
        // :151-153 starts deathTime; later flops take the branch above.
        entity_monster_living_sound_tick(e);
        mob_die_tick(e, s, center);
        // next-gaps-0924/W2 P4b: EntityLiving.onUpdate:203-206 calls
        // onLivingUpdate unconditionally -- isMovementBlocked (:618-622) only
        // zeroes the input -- so a mob in its deathTime flop still runs the
        // push loop (:641-651) every tick until setEntityDead (:155-159).
        // delay_destroy 0 == isDead: the reaper takes it, no push that tick.
        // death-motion-0927: ...and moveEntityWithHeading with zero input before that push
        // (mob_corpse_move ends with it).
        if (e->delay_destroy != 0)
            mob_corpse_move(e, s, NULL);
        return false;
    }

    // --- Beta 1.7.3 ENVIRONMENTAL HAZARDS (audit #15): drown + lava/fire + the
    // fall-distance water reset, for ALL 4 hostiles ---------------------------------
    // Mirrors EntityLiving.onEntityUpdate order: body-in-water resets fall/fire, the
    // (existing) fire DoT burns above, then lava contact + drowning. All damage is on
    // the MOB 20-pt scale, applied DIRECTLY to e->health (NOT *8). A hazard hit can
    // drive health <= 0 MID-tick; the explicit reap immediately after this block
    // (NOT next tick) stops a hazard-killed creeper from still detonating / a zombie
    // from still swinging on the tick it dies. `body_in_water` is also consumed by
    // the fall logic at the Y-axis move below (Beta handleWaterMovement zeroes
    // fallDistance every wet tick).
    //
    // The foot cell (fx/fy/fz + foot/got_foot) is fetched ONCE here and reused by the
    // undead sun-burn RE-IGNITION block further down -- both need the identical cell, so
    // a single server_world_get_block + one floorf triple serves both (no refetch).
    w_coord_t fx = (w_coord_t)floorf(e->pos[0]);
    w_coord_t fy = (w_coord_t)floorf(e->pos[1]);
    w_coord_t fz = (w_coord_t)floorf(e->pos[2]);
    struct block_data foot;
    bool got_foot = server_world_get_block(block_world(s), fx, fy, fz, &foot);
    bool body_in_water = false;
    // SQUID PRE-MOVE GATE SAMPLE (2026-06-12 audit; combat.h SQUID_GATE_PROBE_RAISE):
    // Beta's swim-vector repick gate reads the inherited inWater FIELD -- cached
    // PRE-move from the -0.4-shrunk bbox test -- so it is sampled here, before the
    // axis moves, at the LOOSE edge of the 0.4..0.6 hysteresis band. The squid
    // MOTION branch instead probes LIVE post-move at the TIGHT edge (below, in the
    // is_squid block). The bare foot-cell body_in_water (wet to foot+1.0) stays for
    // the generic fire/fall/paddle logic but no longer drives squid behavior.
    bool squid_gate_wet = false;
    // next-gaps-0924 W1B-b: Beta's handleLavaMovement() verdict for THIS tick,
    // hoisted for the ladder gate (moveEntityWithHeading's lava branch has no
    // ladder code). The hazard block scans it for non-immune mobs; an immune mob
    // (Beta gates the damage on !isImmuneToFire, so the scan is skipped there)
    // is scanned lazily at the ladder gate only when it actually stands in a
    // ladder cell -- no new per-tick cost for the common case.
    bool lava_movement = false;
    bool lava_scanned = false;
    {
        // next-gaps-0924/W2 P3: Beta Entity.onEntityUpdate:168 ->
        // handleWaterMovement [Entity.java:543-545] -- World.handleMaterial-
        // Acceleration over the Entity.setPosition box (minY = posY - yOffset +
        // ySize) shrunk 0.4F in Y and 0.001 on every axis. This is the inherited
        // inWater FIELD every consumer below reads (fire quench, fall reset, the
        // water movement branch, isWet, the swim gate) AND the 0.014D flow
        // impulse on e->vel, applied here BEFORE the move, as Beta's onUpdate
        // orders it. Replaces the foot-cell probe (a second inWater definition,
        // docs/intent/p3-water-flow-push.md). The server tick is the single
        // authority for a mob's push (clients only receive moves).
        body_in_water = entity_handle_water_movement(
            e, e->data.monster.motion_d.pos[0],
            e->data.monster.motion_d.bb.min[1],
            e->data.monster.motion_d.pos[2], e->width, e->height);
        // SPLASH on the dry->wet edge, at Beta's own point in the tick
        // [Entity.java:168-198: on water contact while not yet in water and not
        // on the first update; the latch starts TRUE at spawn]. Volume terms are
        // motionX/Y/Z. Moved here from the post-move site with this card so
        // the latch shares the ONE inWater definition (server_local_splash_edge).
        server_local_splash_edge(s, e, body_in_water,
                                 &e->data.monster.was_in_water);

        if (e->data.monster.id == MONSTER_SQUID) {
            struct block_data gb;
            if (server_world_get_block(
                    block_world(s), fx, (w_coord_t)squid_gate_probe_cell_y(e->pos[1]),
                    fz, &gb)
                && (gb.type == BLOCK_WATER_FLOW || gb.type == BLOCK_WATER_STILL))
                squid_gate_wet = true;
        }

        // Beta Entity.onEntityUpdate order, faithfully reproduced:
        // (1a) handleWaterMovement: body in water -> fire cleared to 0 (+ fall reset, below).
        // (1b) FIRE DoT: while fire > 0, 1 dmg per MOB_FIRE_DAMAGE_INTERVAL ticks on
        //      the CURRENT fire value, then decrement (a 600 ignition fires at
        //      600,580,...,20). Runs for ALL 4 hostiles -- Beta runs it for every
        //      EntityLiving, not just the undead. (It previously lived in the undead-
        //      only block, so a LAVA-ignited creeper/spider set fire but never burned
        //      and on_fire latched forever; moving it here fixes that.)
        // (1c) LAVA: this tick's ignition happens AFTER the DoT, so the DoT consumes
        //      the OLD fire value first -- exactly Beta's ordering.
        if (body_in_water) {
            // S2 extinguish hiss: Beta plays "random.fizz" at volume 0.7, pitch
            // 1.6 plus 0.4 x a roll difference, once when a still-burning entity gets wet
            // (Entity.moveEntity: wet while the fire counter is positive), then clears the burn.
            // [740c583: Entity.moveEntity:534-537]
            if (e->data.monster.fire_time > 0) {
                struct random_gen* sg = server_local_sound_rng();
                server_local_emit_sound(
                    SOUND_RANDOM_FIZZ,
                    (vec3) {e->pos[0], e->pos[1], e->pos[2]}, 0.7f,
                    1.6f + (rand_gen_flt(sg) - rand_gen_flt(sg)) * 0.4f);
            }
            e->data.monster.fire_time = 0; // handleWaterMovement: water extinguishes
        }
        // G6c: Entity.java:226 publishes the immune mob's flag here, on the
        // -4, before EntityLiving.onEntityUpdate:117 zeroes fire.
        if (monster_def_get(e->data.monster.id)->fire_immune)
            e->on_fire = mob_immune_fire_flag(e->data.monster.fire_time);
        if (e->data.monster.fire_time > 0) {
            // Beta Entity.onEntityUpdate fire branch (decompile-verified,
            // 740c583:Entity.java:240-251):
            //     while the fire counter is positive, a fire-immune entity
            //     loses 4 per tick, clamped at zero; any other entity takes
            //     1 damage from no attacker whenever the counter is a
            //     multiple of 20 (checked before the decrement) and then
            //     loses 1 per tick.
            //
            //
            //
            //
            //
            // An immune mob (pig zombie, ghast) NEVER takes the periodic
            // damage, and EntityLiving.onEntityUpdate:117 zeroes its fire in
            // the same tick as the -4 -- so a sun-burned pig zombie shows
            // fire for ONE tick per daylight roll, not ~75 (G6c).
            // mob_fire_tick_decay (combat.h) is the shared,
            // directly-tested production decision for both branches.
            bool apply_damage = false;
            e->data.monster.fire_time = mob_fire_tick_decay(
                e->data.monster.fire_time,
                monster_def_get(e->data.monster.id)->fire_immune,
                &apply_damage);
            if (apply_damage) {
                (void)monster_apply_environment_damage(
                    e, s, MOB_FIRE_DAMAGE);
            }
        }

        // LAVA + FIRE: whenever the lava scan hits, Beta first deals a 4-point hit and
        // then sets the burn timer to 600 (Entity.java:239-240). handleLavaMovement scans
        // the box shrunk by 0.1 / 0.4 / 0.1 (x/y/z, float values widened) through
        // World.isMaterialInBB. Keep the separately fetched foot cell above for
        // the sun-burn block; lava must use the full contracted body.
        // The per-tick 4 uses the mob's shared resistance arbitration: Beta calls it
        // every tick but the absorb gate collapses it to ~4 every
        // 10 ticks. fire_time is (re)set EVERY contact tick (Beta sets the fire counter to 600
        // outside the attack gate), reusing the fire DoT above -- NOT a parallel burn.
        // setOnFireFromLava is FULLY gated on !isImmuneToFire in Beta (neither
        // the damage nor the ignition happens for pig zombie / ghast).
        const struct monster_def* lava_def
            = monster_def_get(e->data.monster.id);
        // The scan is side-effect free, so testing immunity first skips it for
        // ghasts / pig zombies with output identical to Beta's unconditional call.
        // MOBLAVA-0925: ONE scanner (mob_body_in_lava, the ladder gate's) --
        // the box minY is pos - y_offset + y_size, Beta's boundingBox.minY, not
        // pos: a mob with a banked step pop was scanned a release-tail too low.
        const bool body_in_lava = !lava_def->fire_immune
            && mob_body_in_lava(s, e);
        lava_scanned = !lava_def->fire_immune;
        lava_movement = body_in_lava;
        if(body_in_lava) {
            (void)monster_apply_environment_damage(e, s, MOB_LAVA_DAMAGE);
            e->data.monster.fire_time = MOB_LAVA_FIRE_TICKS; // setOnFireFromLava: 600
        }

        // LIVING-SOUND-RNG: EntityLiving.onEntityUpdate rolls the living
        // sound right after super.onEntityUpdate (the water/fire/lava block
        // above) and BEFORE suffocation/drowning -- both of which can draw
        // the hurt pitch on this same Random. The roll is unconditional,
        // including a mob just killed by fire/lava; subsequent corpse hits
        // can observe its advanced stream. [server EntityLiving.java:101-111]
        entity_monster_living_sound_tick(e);

        // SUFFOCATION (next-gaps-0924 W1B, P1): EntityLiving.onEntityUpdate:113-115
        //     a living entity whose head is inside an opaque block takes 1
        //     damage from no attacker,
        // runs right after super.onEntityUpdate (the fire / lava block above) and
        // BEFORE the drowning air drain below -- same order here. It goes through
        // the ordinary null-attacker path, so the hurt-resistance window
        // (EntityLiving.attackEntityFrom:317) collapses the per-tick call to 1
        // point every MOB_HURT_RESIST_TICKS. isEntityAlive() is simply not isDead: a mob
        // whose health just went <= 0 (lava this tick) is still "alive" in that
        // sense and Beta still calls attackEntityFrom (entityAge reset, then the
        // health<=0 early-out) -- entity_monster_apply_damage does the same, so
        // no extra health gate here. Every EntityLiving suffocates (squid, ghast
        // and wolf included). The wolf takes the FULL point: EntityWolf.
        // attackEntityFrom:246-248 halves only a non-null attacker that is
        // neither a player nor an arrow, and this is the null-attacker call --
        // entity_wolf_attack_from (via monster_apply_environment_damage) applies
        // no halving for attacker_id == UINT32_MAX, only the sit reset.
        if(mob_inside_opaque_block(s, e))
            (void)monster_apply_environment_damage(e, s, MOB_SUFFOCATION_DAMAGE);

        // DROWNING: Beta isInsideOfMaterial for water at the EYE cell (head), per-type eye
        // height (biped 1.53 / spider 0.765). air drains while submerged; the first hit
        // lands when air reaches MOB_DROWN_AIR_THRESHOLD (-20) -- 320 ticks / 16s -- then
        // 2 every MOB_DROWN_INTERVAL ticks. Out of water, air refills to MOB_AIR_MAX
        // instantly. None of the 4 hostiles breathe underwater. The eye cell is distinct
        // from the foot cell above: a mob standing in 1-deep water has a wet body (fall
        // reset / fire out) but a dry head (no drowning).
        // Beta routes the drown 2 through attackEntityFrom, so it is ALSO subject to
        // heartsLife arbitration -- use the same helper as lava/melee/fall.
        // (In the common pure-drown case the 20-tick interval exceeds the 10-tick
        // i-frame so the gate never fires; gating it matters only on a contrived
        // simultaneous lava/melee + drown frame, where Beta's single absorb counter
        // would likewise drop one of the two hits. This is byte-exact parity.)
        float eye = mob_eye_height(e->data.monster.id);
        w_coord_t ey = (w_coord_t)floorf(e->pos[1] + eye);
        struct block_data head;
        bool head_in_water
            = server_world_get_block(block_world(s), fx, ey, fz, &head)
              && (head.type == BLOCK_WATER_FLOW || head.type == BLOCK_WATER_STILL);
        if (head_in_water) {
            e->data.monster.fire_time = 0; // submerged head also extinguishes (Beta)
            if (e->data.monster.id == MONSTER_SQUID) {
                // Beta EntityWaterMob.canBreatheUnderwater(): the squid NEVER
                // drowns -- skip the air drain entirely (fire-extinguish above
                // still applies; Beta puts that on every entity).
            } else if (--e->data.monster.air == MOB_DROWN_AIR_THRESHOLD) {
                e->data.monster.air = 0;
                (void)monster_apply_environment_damage(
                    e, s, MOB_DROWN_DAMAGE);
            }
        } else {
            e->data.monster.air = MOB_AIR_MAX; // instant refill out of water (Beta)
        }
    }

    // REAP a hazard-killed mob THIS tick (Beta sets isDead in attackEntityFrom and
    // short-circuits the rest of onUpdate). Without this, a creeper killed by lava/
    // drown/fire above would still fall through to its detonation branch, and a zombie/
    // spider would still swing at the player, on the tick they die. Mirrors the
    // top-of-tick death branch (audit #18 item 3: enter the deathTime flop + drop rolled
    // loot, NOT an instant destroy). Fall damage lands later (after the Y-move) and is
    // reaped the same way at its own site.
    if (e->health <= 0) {
        mob_die_tick(e, s, center);
        // P4b: the dying tick's onLivingUpdate still runs (see the top-of-tick
        // death branch): death-motion-0927 moves the corpse (this tick's
        // handleWaterMovement already ran above) and then pushes.
        if (e->delay_destroy != 0)
            mob_corpse_move(e, s, &body_in_water);
        return false;
    }

    /* EntityLiving.onEntityUpdate applies fire/lava/drown in Entity.onUpdate
     * before decrementing hurtTime and heartsLife. Fall occurs later, after
     * this decrement. Preserve that observable ordering. [740c583: EntityLiving] */
    if (e->data.monster.hurt_time > 0)
        e->data.monster.hurt_time--;
    if (e->data.monster.hurt_resist_time > 0)
        e->data.monster.hurt_resist_time--;

    // Whether the mob is mid-knockback this tick (steer + speed cap are
    // suppressed below so the player's shove actually carries it).
    bool knocked = e->data.monster.knockback_time > 0;

    // --- Beta 1.7.3 natural DESPAWN (audit #16) ------------------------------------
    // EntityLiving.updatePlayerActionState increments the entity age, then calls
    // despawnEntity() EVERY tick for an active mob. We mirror that here: bump the age,
    // then run the byte-exact decision (mob_despawn_action in combat.h). Placed AFTER
    // the death/hazard reaps but BEFORE the sun-burn re-ignition and all the chase/
    // move/attack work, so a despawning mob neither burns nor acts on its final tick.
    //
    // Distance is the 3D feet-to-feet squared distance to the single server player.
    // s->player.{x,y,z} is the player's EYE. NOTE (corrected 2026-06-19): Beta's
    // getDistanceSqToEntity uses each entity's posY RAW, and the player's posY IS the eye
    // (EntityPlayer.yOffset 1.62) -- so a byte-faithful gate would be EYE-to-feet, not the
    // feet-to-feet used here (we subtract PLAYER_EYE_HEIGHT to drop to the player's feet;
    // e->pos[1] is the mob's feet). For despawn the ~1.62 offset is negligible against the
    // 32/128 edges, and this matches the shared feet-to-feet basis the chase/acquire/melee
    // gates use -- see the canonical Y-BASIS DEVIATION note at chase_dy.
    //
    // DESPAWN IS SILENT: setEntityDead() only sets isDead -- no loot, no XP. So this
    // path sets delay_destroy=0 (the reaper sends CRPC_ENTITY_DESTROY + frees the mob,
    // identical to death) WITHOUT server_local_spawn_item, and returns false so the mob
    // runs no AI this tick. Beta has NO 0-players force-despawn (the closest-player
    // lookup with -1.0 returns any player and the single server player is always present).
    // AGING (corrected 2026-06-19): EntityLiving.updatePlayerActionState bumps entityAge
    // +1/tick; EntityMob.onLivingUpdate adds +2 MORE on a tick where getEntityBrightness >
    // 0.5 -- so a bright EntityMob hostile ages +3/tick and despawns FASTER. (The old
    // comment wrongly called the +2 an mc173 embellishment; it is REAL Beta 1.7.3.) The +2
    // is applied just below for the four EntityMob hostiles; the +1 is the unconditional
    // bump after it. Slimes/animals extend EntityLiving/EntityAnimal (not EntityMob) and
    // never get the +2.
    {
        int amid = e->data.monster.id;
        if (amid == MONSTER_ZOMBIE || amid == MONSTER_SKELETON
            || amid == MONSTER_CREEPER || amid == MONSTER_SPIDER
            || amid == MONSTER_PIG_ZOMBIE /* EntityMob-derived (batch 1b);
                                             the ghast is EntityFlying, no +2 */
            || amid == MONSTER_GIANT /* EntityGiantZombie extends EntityMob
                                        directly [740c583] -- gets the bright
                                        +2, but NOT the zombie/skeleton
                                        sun-burn below (that is EntityZombie/
                                        EntitySkeleton's own override) */) {
            // getEntityBrightness > 0.5 == the floored-table light >= 12 (UNDEAD_SUNBURN_
            // SKY_MIN, the same "bright enough" gate the sun-burn uses), sampled at the
            // Beta torso point (feet + 0.66*body) with the block/torch-light max. The
            // per-tick read is bounded by the spawn cap; deduping it with the later
            // sun-burn / spider-passivity light read is a deferred micro-opt.
            w_coord_t aby = (w_coord_t)floorf(mob_brightness_sample_y(e->pos[1], amid));
            struct block_data abc;
            if (server_world_get_block(block_world(s), fx, aby, fz, &abc)) {
                // F1: getEntityBrightness -> getLightBrightness -> getBlockLightValue.
                int ael = server_local_block_light_value(s, fx, aby, fz);
                if (ael >= UNDEAD_SUNBURN_SKY_MIN)
                    e->data.monster.entity_age += 2; // EntityMob.onLivingUpdate bright bonus
            }
        }
    }
    e->data.monster.entity_age++;
    // EntityWolf.canDespawn returns !tamed. A tamed wolf still ages, but the
	// despawnEntity decision is bypassed. [740c583: EntityWolf.canDespawn]
    /* lane C #19: Beta despawns against the CLOSEST player of the mob's own
     * world -- EntityLiving.func_27021_X (despawnEntity) :667-685 does
     * an unlimited-range closest-player lookup and only proceeds when the
     * mob may despawn and a player was found, and World.getClosestPlayer
     * :2166-2180 scans the world's playerEntities keeping the first strict
     * minimum (a strict less-than, so the earlier player wins a tie). Every
     * eligible actor committed to this world is a candidate (host first,
     * then the guest: server_local_eligible_actors_w, A1c); an actor in
     * another world never governs it, and with NO eligible actor Beta skips
     * the whole block -- no despawn and no nextInt(800) draw. The distance
     * basis (feet-to-feet) and the roll gate are the existing ones below. */
    struct server_player* dd_near = NULL;
    float dd_sq = 0.0f;
    {
        struct server_world* dd_w = server_local_entity_world(s, e);
        struct server_player* dd_cand[SERVER_LOCAL_MAX_PRESENT_ACTORS];
        size_t dd_n = server_local_eligible_actors_w(
            s, dd_w, dd_cand, SERVER_LOCAL_MAX_PRESENT_ACTORS);
        for (size_t i = 0; i < dd_n; i++) {
            /* A body without a position is not in the world's player list
             * (the #20 wolf scan's rule; Fable G6 LOW): it never governs. */
            if (!dd_cand[i]->has_pos)
                continue;
            float dx = (float)(dd_cand[i]->x - e->pos[0]);
            float dy = (float)(dd_cand[i]->y - PLAYER_EYE_HEIGHT - e->pos[1]);
            float dz = (float)(dd_cand[i]->z - e->pos[2]);
            float d2 = dx * dx + dy * dy + dz * dz;
            if (!dd_near || d2 < dd_sq) {
                dd_near = dd_cand[i];
                dd_sq = d2;
            }
        }
    }
    if(dd_near && (!entity_is_wolf(e)
	   || wolf_can_despawn(e->data.monster.wolf_flags))) {
        // DETERMINISM: Beta despawnEntity gates the give-up roll on an age above 600 AND a
        // 1-in-800 roll, and the AND SHORT-CIRCUITS on the AGE test -- so the roll is
        // drawn ONLY on ticks where entityAge already exceeds 600. This is the
        // mob's own stream; young ticks do not consume this despawn draw.
        // EntityLiving.java:674-678 uses sequential far-removal and aged-roll
        // ifs with no return between them: even an aged mob beyond 128 draws
        // before the far-removal outcome below returns without AI or loot.
        // Use the SEEDED source, never libc rand(); jrand_next_int_bound returns 0..den-1, so ==0
        // is exactly nextInt(800)==0.
        bool roll_hit = e->data.monster.entity_age > MOB_DESPAWN_MIN_AGE
            && jrand_next_int_bound(&e->gameplay_rng, MOB_DESPAWN_ROLL_DEN) == 0;
        switch (mob_despawn_action(e->data.monster.entity_age, dd_sq, roll_hit)) {
        case MOB_DESPAWN_DESPAWN:
            // Silent removal: mark for the reaper (CRPC_ENTITY_DESTROY), NO loot drop
            // (the sole behavioral difference from the death branch above), short-circuit.
            e->delay_destroy = 0;
            return false;
        case MOB_DESPAWN_RESET_AGE:
            // close (<32) + aged + roll: Beta zeroes entityAge, the mob persists.
            e->data.monster.entity_age = 0;
            break;
        case MOB_DESPAWN_KEEP:
        default:
            break;
        }
    }

    // Beta 1.7.3 daylight sun-burn RE-IGNITION for the UNDEAD (zombie + skeleton).
    // EntityZombie/EntitySkeleton.onLivingUpdate runs this AFTER super.onEntityUpdate
    // (the fire DoT + water-extinguish above), so it just (re)sets the fire field to
    // MOB_FIRE_TICKS (300, NOT the lava 600) when the mob is sky-exposed, bright enough,
    // and passes the ~4%/tick random roll; the DoT consumes it from NEXT tick. Water
    // (body) prevents it; shade (foot can't see the sky) prevents it; no helmet mechanic
    // exists in b1.7.3. (The DoT itself now lives in the shared hazard block above so it
    // also burns lava-ignited creepers/spiders -- only the daytime IGNITION is undead.)
    //
    // AUDIT #18 ITEM 7: the light sample is taken at Beta getEntityBrightness's TORSO
    // point -- floor(feet + 0.66*bodyHeight) -- NOT the foot cell. v2 applies the Beta
    // 0.66 fraction to its inherited 1.8 biped collision box (mob_body_height), so the
    // sample is feet + 0.66*1.8 = feet + 1.188 -- the block ~1 above the feet. It differs
    // from the foot cell only when the mob straddles a light gradient (half-lit, under a 1-deep
    // overhang), but it is what Beta actually reads. We re-floor the column at this Y and
    // read THAT cell's sky_light + heightmap, instead of the foot cell's. The 0.85
    // mob_eye_height stays for canEntityBeSeen LOS only (a different Beta method).
    {
        int mid = e->data.monster.id;
        // Parity fix 2026-06-19 (audit B): the OUTER gate no longer carries
        // `!body_in_water`, so the ignition roll is DRAWN even for a wet exposed undead --
        // exactly as Beta does. Beta's sunburn `&&` chain (EntityZombie/Skeleton.on-
        // LivingUpdate) has NO inWater term; the water-extinguish lives in
        // Entity.onEntityUpdate (the earlier hazard block above). Drawing the roll
        // regardless keeps the mob-owned gameplay_rng stream aligned with Beta's per-tick draw
        // count; body_in_water now suppresses only the fire ASSIGNMENT, not the draw.
        //
        // Parity fix 2026-09-01: Beta's outer world-is-daytime test (World.
        // isDaytime(): sky light subtraction below 4) wraps this ENTIRE mechanism and is
        // evaluated BEFORE getEntityBrightness is ever sampled. The prior port never
        // computed it at all, relying on eff_light>=12 alone -- which stopped being an
        // isDaytime() proxy the moment block/torch light was folded into eff (a torch
        // under open sky can push eff>=12 at night). Compute it here, first, via the
        // byte-pinned daytime_is_daytime_ticks (source/daytime.h:106; same wiring as
        // block_bed.c's sleep gate) and thread it into undead_sunburn_exposed as its
        // own required outer term. [740c583: EntityZombie/EntitySkeleton.onLivingUpdate]
        //
        // Parity fix 2026-09-02: EntityPigZombie extends EntityZombie and its
        // onLivingUpdate override only delegates to the superclass
        // (decompile-verified, 740c583:EntityPigZombie.java) -- it inherits
        // the daylight ignition wholesale, so a sky-exposed pig zombie DOES
        // draw the sun-burn roll and gets its burn timer set to 300 exactly like a
        // zombie. What makes it look different is EntityPigZombie's ctor
        // marking itself fire-immune: the fire-immune branch above
        // (Entity.java:240-251) decays that fire 4/tick with no damage ever
        // landing, so the ignition is real but purely cosmetic -- a visible
        // flicker for ~75 ticks, never a hit. The prior gate omitted
        // MONSTER_PIG_ZOMBIE entirely, so a sky-exposed pig zombie never
        // ignited at all (no flicker, and RNG draws Beta makes here were
        // silently skipped, desyncing the mob-owned gameplay_rng consumption count
        // for that mob relative to Beta). Adding it here does not touch the
        // immunity/damage gate above -- that is unconditional on
        // monster_def_get(...)->fire_immune, which the pig zombie already
        // carries (monsters_object.c:266).
        bool undead_is_daytime = server_local_is_daytime(s); // w2: World.isDaytime (cached)
        if (mob_is_undead_sunburn_candidate(mid) && got_foot) {
            w_coord_t by = (w_coord_t)floorf(
                mob_brightness_sample_y(e->pos[1], mid)); // feet + 0.66*body
            struct block_data bcell;
            bool got_bcell
                = server_world_get_block(block_world(s), fx, by, fz, &bcell);
            uint8_t height = 0;
            bool can_see_sky = got_bcell
                && server_world_get_height(block_world(s), fx, fz, &height)
                && by >= (w_coord_t)height;
            // Beta getEntityBrightness -> Chunk.getBlockLightValue takes the larger of the
            // dimmed sky light and the block light: the block/torch light at the SAME sample cell
            // participates (audit fix: the old code fed eff_sky only, so a torch could
            // not push a dawn/dusk-shaded undead over the burn threshold).
            // F1: the shared getBlockLightValue (neighbour max + weather darkening).
            int eff = server_local_block_light_value(s, fx, by, fz);
            if (undead_sunburn_exposed(undead_is_daytime, eff, can_see_sky)) {
                // Beta WorldProvider.generateLightBrightnessTable -> getEntityBrightness,
                // then the ignition roll (30 x a uniform float below twice the excess of b over 0.4). Use the SHARED
                // floored Overworld table (light_lookup_overworld, pinned byte-faithful by
                // test_daylight_curve.c): WorldProviderSurface is empty, so the Overworld
                // INHERITS the 0.05 ambient floor (scaled by 0.95, then plus 0.05). The old open-coded
                // floorless divide understated b at every partial-light level (e.g. light
                // 12: 0.500 vs the faithful 0.525), which both slowed ignition and made
                // light 12 never burn. jrand_next_float is drawn UNCONDITIONALLY here (left of
                // the `<`); body_in_water only blocks the fire set, preserving the draw.
                int idx = eff < 0 ? 0 : (eff > 15 ? 15 : eff);
                float bright = light_lookup_overworld[idx];
                if (jrand_next_float(&e->gameplay_rng) * 30.0f < (bright - 0.4f) * 2.0f
                    && !body_in_water)
                    e->data.monster.fire_time = MOB_FIRE_TICKS;
            }
        }
    }

    // --- M4 S3: CHICKEN EGG LAYING (Beta EntityChicken.onLivingUpdate) ---------
    // Beta: on the authoritative world, the egg timer counts down and at zero or below -> play
    // "mob.chickenplop", then drop one egg, then re-roll the timer to 6000 plus 0-5999.
    // The authoritative-side gate IS this server tick. The constructor's initial
    // 6000 plus 0-5999 roll is drawn HERE on the first tick instead (a
    // retained, documented deferral -- the slime jump_delay precedent; egg_timer==0
    // is the unrolled sentinel, one tick late, no lay).
    // [740c583: EntityChicken ctor seeds the egg timer with 6000 plus a 0-5999 roll;
    //  EntityChicken.onLivingUpdate egg-lay branch]
    if (e->data.monster.id == MONSTER_CHICKEN) {
        if (e->data.monster.egg_timer <= 0) {
            e->data.monster.egg_timer = 6000 + jrand_next_int_bound(&e->gameplay_rng, 6000);
        } else if (--e->data.monster.egg_timer <= 0) {
            struct item_data egg = {
                .id = ITEM_EGG, .durability = 0, .count = 1};
            // S0: Beta's exact order -- the "mob.chickenplop" sound (volume 1.0, pitch
            // 1.0 plus 0.2 x a roll difference) THEN the egg drop THEN the timer re-roll, all
            // on the chicken's own Random (LIVING-SOUND-RNG).
            // [EntityChicken.onLivingUpdate:44-46]
            server_local_emit_sound(SOUND_MOB_CHICKENPLOP,
                                    (vec3) {e->pos[0], e->pos[1], e->pos[2]},
                                    1.0F, mob_living_sound_pitch(e));
            // Beta dropItem -> entityDropItem with a zero y-offset: at the chicken's
            // own position (no y-offset), with the standard drop throw jitter.
            // FH-12: entityDropItem sets a 10-tick pickup delay.
            struct entity* egg_drop = mob_spawn_item(e, s,
                (vec3) {e->pos[0], e->pos[1], e->pos[2]}, &egg);
            if (egg_drop)
                egg_drop->data.item.pickup_delay = 10;
            e->data.monster.egg_timer = 6000 + jrand_next_int_bound(&e->gameplay_rng, 6000);
        }
    }

	/* ===== S1-a2: RESOLVE THE RETAINED TARGET (Astra gate-1 item 5) ========
	 * aggro_time says HOW LONG this mob keeps its target; target_actor says
	 * WHICH actor that is. Beta stores a live EntityCreature.entityToAttack /
	 * EntityMob.playerToAttack reference [740c583 EntityMob.attackEntityFrom:
	 * the attacker becomes the attack target on any damaging hit; EntityCreature.update
	 * PlayerActionState re-reads it every tick]. This is the value-form
	 * re-resolution of exactly that reference, once per AI tick:
	 *   - resolves -> chase/steer/acquire/LOS all measure to THAT actor;
	 *   - FAILS (guest left, was released, or rejoined under a new generation)
	 *     -> the role invalidates: identity back to the host and aggro_time to
	 *     0, i.e. no target, so the mob wanders and may re-acquire normally.
	 *     That is the Beta shape too -- EntityCreature drops entityToAttack the
	 *     moment it stops being a valid live target, and the retention timeout
	 *     is what a dropped target falls back to.
	 * No actor pointer is retained anywhere across ticks.
	 *
	 * With no guest target_actor resolves to the host. Fresh acquisition still
	 * queries the entity's world; angry wolves additionally enforce Beta's
	 * 16-block range below. Thus SP equivalence is scoped to the original
	 * single-world fixture, not a host that has transferred to another slot. */
	struct server_player* target
		= server_local_actor_resolve(s, e->data.monster.target_actor);
	struct server_player* wolf_owner = entity_wolf_owner_actor(e, s);
	if(entity_is_wolf(e)
	   && wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_TAMED)
	   && (e->data.monster.wolf_target_id == UINT32_MAX
		   || e->data.monster.wolf_target_id == 0))
		target = wolf_owner;
	/* Beta EntityMob.findPlayerToAttack -> World.getClosestPlayerToEntity
	 * scans only this world's playerEntities [EntityMob.findPlayerToAttack;
	 * World.getClosestPlayer:2166-2180]. A fresh target cannot default to the
	 * host once that body has committed to another dimension. A retained
	 * target is deliberately left alone: EntityCreature tests its live Java
	 * reference, and dimension transfer may preserve that reference. Ghast's
	 * existing per-tick target selection is kept at its existing cadence; only
	 * its candidate set becomes world-local. */
	const bool world_local_targeting = (!entity_is_wolf(e)
		&& !monster_is_passive(e->data.monster.id))
		|| (entity_is_wolf(e)
			&& wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_ANGRY));
	const bool fresh_target = world_local_targeting
		&& (e->data.monster.id == MONSTER_GHAST
			|| !target || e->data.monster.aggro_time <= 0);
	bool target_available = target != NULL;
	if(fresh_target) {
		struct server_world* owner = server_local_entity_world(s, e);
		struct server_player* candidates[SERVER_LOCAL_MAX_PRESENT_ACTORS];
		size_t count = server_local_eligible_actors_w(s, owner, candidates,
			SERVER_LOCAL_MAX_PRESENT_ACTORS);
		struct server_player* closest = NULL;
		double best_sq = 0.0;
		for(size_t i = 0; i < count; i++) {
			struct server_player* candidate = candidates[i];
			if(!candidate->has_pos)
				continue;
			/* Beta's World.getClosestPlayer scans that world's active player
			 * list (World.java:2166-2180). A positioned split guest may still be
			 * present before gameplay admission; it is not an active target. */
			if(candidate != &s->player
			   && server_local_actor_resolve(s,
				   server_local_actor_ident(s, candidate)) != candidate)
				continue;
			double dx = candidate->x - (double)e->pos[0];
			double dy = candidate->y - (double)e->pos[1];
			double dz = candidate->z - (double)e->pos[2];
			double distance_sq = dx * dx + dy * dy + dz * dz;
			/* Beta EntityWolf.findPlayerToAttack:291 passes 16.0D;
			 * World.getClosestPlayer:2173 uses strict distanceSq < range^2.
			 * Only fresh acquisition is bounded: EntityCreature:19-33 keeps
			 * a live held player reference across distance/world changes. */
			if(entity_is_wolf(e) && !(distance_sq < 16.0 * 16.0))
				continue;
			if(!closest || distance_sq < best_sq) {
				closest = candidate;
				best_sq = distance_sq;
			}
		}
		target = closest;
		target_available = closest != NULL;
	}
	/* S1-a3 (Astra call #8 Q2, "a zero-health guest must not remain a
	 * permanent harmless-hostile target"): RETIRE the role when the retained
	 * target is a NON-HOST body at zero health. Beta drops entityToAttack the
	 * moment it stops being a valid live target -- EntityCreature.update
	 * PlayerActionState clears it once the target is dead, and
	 * EntityMob.findPlayerToAttack only ever returns a live player [740c583]
	 * -- so a dead body is not a target, it is NO target, and the mob falls
	 * back to ordinary acquisition (which reaches the host).
	 *
	 * HOST-EXEMPT ON PURPOSE, and this is the one asymmetry in the file: the
	 * host at zero health is mid death-screen dwell and its respawn
	 * transaction already clears every aggro latch in the world
	 * (server_local_respawn_request). Adding a health test for the host here
	 * would change when those latches expire -- a host behaviour change, not
	 * a parameter move -- so the host keeps exactly the retention it had.
	 * With no guest (or the split switch off) target_actor is the zero value,
	 * actor_ident_is_host is true, and this whole clause is skipped: the OFF
	 * path stays byte-identical (test_split_off_equiv). The host's own Beta
	 * dead-target DROP (EntityCreature:24-25) is applied below, as the
	 * C-DEADTARGET block, after the target is resolved. */
	if(target && !actor_ident_is_host(e->data.monster.target_actor)
	   && target->health <= 0)
		target = NULL;
	if(!target) {
		e->data.monster.target_actor = ACTOR_IDENT_HOST;
		e->data.monster.aggro_time = 0;
		target = &s->player;
		target_available = false;
	}
	/* Whether this mob may actually LAND A HIT this tick. S1-a2 left this
	 * FAIL-CLOSED (host only) because the damage pipeline was host-singular
	 * top to bottom; S1-a3 made every step actor-aware, so the predicate is
	 * now the Beta one: a LIVE target can be hit. [740c583
	 * EntityLiving.attackEntityFrom:312 rejects a hit on health <= 0, so a
	 * corpse is never a damage recipient.]
	 *
	 * HOST EQUIVALENCE: this was literally `true` for the host before. The
	 * only case where it now differs is s->player.health <= 0 -- and in that
	 * case server_local_hurt_player_for returns on its own first statement,
	 * with no armor wear, no i-frame churn, no sound and no RNG draw. So no
	 * reachable host state or RPC changes; the attack_time reset that follows
	 * the call site is OUTSIDE this predicate and is untouched either way. */
	bool target_damageable = target_available && target->health > 0;
	/* C-DEADTARGET (2026-09-26): Beta's dead-target rule, for EVERY target
	 * including the host. EntityCreature.updatePlayerActionState:19-33 --
	 *     with no target it searches for one; with a target that is no longer
	 *     alive it clears the target; otherwise it runs the attack step
	 *     (direct or blocked-path attack).
	 * with EntityLiving.isEntityAlive:575-577 = not dead and health above 0. A held
	 * dead target is DROPPED on this tick (the null-target wander branch runs,
	 * with its nextInt(80) rolls); EntityMob.findPlayerToAttack:28-31 goes
	 * through World.getClosestPlayer:2166-2179, which has no alive test, and a
	 * dead player is never removed from playerEntities (World.updateEntities:
	 * 1211-1224 only drops it from loadedEntityList) -- so the corpse is
	 * RE-ACQUIRED on the next tick (acquire branch: path, no attack) and dropped
	 * again on the one after. attackEntity is reached on neither tick, so every
	 * attack site below additionally requires target_alive: no arrow
	 * (EntitySkeleton.attackEntity:34-53), no swing / attackTime re-arm
	 * (EntityMob.attackEntity:49-55), no pounce roll (EntitySpider.attackEntity:
	 * 41-59, EntityWolf.attackEntity:294-313), no fuse increment
	 * (EntityCreeper.attackEntity:89-117).
	 *
	 * The aggro latch is v2's playerToAttack; "held at tick start" is
	 * aggro_time > 0. This supersedes the old host exemption for the ALIVE
	 * test only: the guest-retire clause above is unchanged, and the host's
	 * respawn transaction still clears every latch. The wolf keeps its own
	 * state-driven latch and drops in its angry branch below. The skeleton that
	 * kept firing at a dead host (~120 live arrows, 3-10x entity tick cost,
	 * tmp/jobs/split-dimensions-plan-0924/out/TICKCOST-BISECT-0926.md) was this
	 * missing rule. EntityGhast is NOT an EntityCreature: its updatePlayer-
	 * ActionState:61-66 clears targetedEntity on isDead only and re-targets
	 * getClosestPlayerToEntity with range 100, which still returns the corpse -- so the
	 * ghast branch below is left firing at a dead player, as Beta does. */
	bool target_alive = target_damageable;
	bool dead_target_dropped = false;
	if(!target_alive && !entity_is_wolf(e)
	   && e->data.monster.aggro_time > 0) {
		e->data.monster.aggro_time = 0;
		dead_target_dropped = true;
	}

	// --- Beta EntityWolf action state --------------------------------------
	// Resolve the persistent owner in the wolf's world (EntityWolf.java:85). A*
	// is deliberately absent, so ordinary follow/chase uses the shared direct
	// steer below; a blocked >12 owner follow uses the literal Beta ring scan.
	bool is_wolf = entity_is_wolf(e);
	bool wolf_follow_owner = false;
	struct entity* wolf_entity_target = NULL;
	if(is_wolf) {
		uint8_t* wf = &e->data.monster.wolf_flags;
		e->data.monster.wolf_watched_health = e->health; // watcher 18, server-side
		bool tamed = wolf_flag_get(*wf, WOLF_FLAG_TAMED);
		bool angry = wolf_flag_get(*wf, WOLF_FLAG_ANGRY);
		bool owner_present = tamed && wolf_owner && wolf_owner->has_pos;
		const struct server_player* owner = wolf_owner ? wolf_owner : &s->player;
		float odx = (float)owner->x - e->pos[0];
		float ody = (float)(owner->y - PLAYER_EYE_HEIGHT) - e->pos[1];
		float odz = (float)owner->z - e->pos[2];
		float owner_dist_sq = odx * odx + ody * ody + odz * odz;

		if(tamed) {
			bool has_retaliation_target
				= e->data.monster.wolf_target_id != UINT32_MAX
				&& e->data.monster.wolf_target_id != 0;
			wolf_update_automatic_sit(wf, owner_present, body_in_water,
				has_retaliation_target);
			if(!owner_present && !has_retaliation_target) {
				e->data.monster.wolf_target_id = UINT32_MAX;
				e->data.monster.aggro_time = 0;
				e->data.monster.direction[0] = 0.0f;
				e->data.monster.direction[1] = 0.0f;
			} else if(!has_retaliation_target && owner_present
					  && !wolf_flag_get(*wf, WOLF_FLAG_SITTING)
					  && wolf_should_follow(owner_dist_sq)) {
				wolf_follow_owner = true;
				e->data.monster.target_actor = server_local_actor_ident(s, owner);
				e->data.monster.wolf_target_id = 0;
				e->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
				// With no A*, a blocked eye ray is the available "no path"
				// signal. The scan/order/placement itself remains exact Beta.
				if(wolf_should_teleport(owner_dist_sq)
				   && !mob_can_see_player(s, e, owner))
					(void)entity_wolf_try_teleport_to_owner(
						e, s, (float)owner->x,
						(float)(owner->y - PLAYER_EYE_HEIGHT),
						(float)owner->z);
			} else if(!has_retaliation_target
					  && e->data.monster.wolf_target_id == 0) {
				e->data.monster.wolf_target_id = UINT32_MAX;
				e->data.monster.aggro_time = 0;
			}
		} else if(angry) {
			/* C-DEADTARGET: EntityCreature:24-25 drop of a dead held player
			 * target; EntityWolf.findPlayerToAttack:290-292 (angry -> closest
			 * player, no alive test) re-acquires it on the next tick. */
			if(!target_available) {
				e->data.monster.wolf_target_id = UINT32_MAX;
				e->data.monster.aggro_time = 0;
			} else if(!target_alive && e->data.monster.wolf_target_id == 0
			   && e->data.monster.aggro_time > 0) {
				e->data.monster.wolf_target_id = UINT32_MAX;
				e->data.monster.aggro_time = 0;
			} else {
				e->data.monster.wolf_target_id = 0;
				e->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
				if(fresh_target)
					e->data.monster.target_actor = server_local_actor_ident(s, target);
			}
		} else if(e->data.monster.wolf_target_id == UINT32_MAX
				  && e->ai_state == AI_IDLE) {
			entity_wolf_try_find_sheep(e, s);
		}
		wolf_entity_target = entity_wolf_target(e, s);
		// looksWithInterest gating + the head-course smoothing are computed
		// AFTER the target-acquisition/chase/wander section below settles this
		// tick's path state (Beta's own ordering: EntityWolf.onLivingUpdate's
		// looksWithInterest check runs after the inherited onLivingUpdate, which is
		// what resolves the action-state update and path check for the tick) --
		// see the second `if(is_wolf)` block after the WANDER branch closes.
	}
	if(e->data.monster.id == MONSTER_WOLF
	   && e->data.monster.wolf_shake_active
	   && e->data.monster.wolf_shake_time > 0.4F) {
		int count = (int)(sinf((e->data.monster.wolf_shake_time - 0.4F)
			* 3.1415927F) * (float)WOLF_SHAKE_MAX_SPLASHES);
		if(count > 0)
			server_local_send_entity(s, e, (struct client_rpc) {
				.type = CRPC_WOLF_SPLASH,
				.payload.wolf_splash = {
					.pos = {e->pos[0], e->pos[1], e->pos[2]},
					.velocity = {e->vel[0], e->vel[1], e->vel[2]},
					.count = (uint8_t)count,
				},
			});
	}

    // Beta 1.7.3 target acquisition + approach. EntityMob.findPlayerToAttack ->
    // getClosestVulnerablePlayerToEntity within 16 blocks; the server has a single
    // player, so the closest player is always s->player. If the player is inside
    // the follow range, CHASE: steer the heading straight at them and let the
    // existing move/auto-jump pipeline carry the mob over; otherwise WANDER.
    // Simplifications (deferred follow-ups, documented in combat.h): direct
    // beeline rather than Beta A* pathfinding; distance-only acquisition (no
    // line-of-sight gate); same locomotion speed as wander (Beta aggro does not
    // speed a zombie up). Steering uses the mob's start-of-tick position; the
    // melee sweep below keeps its own post-move distance, intentionally.
    // Beta 1.7.3 spider daytime passivity: EntitySpider only INITIATES a chase
    // when it's dark enough (effective light <= 10); in daylight it stays passive.
    // (It would still retaliate if hit -- a persistent target latch is a later
    // slice.) Every other hostile acquires regardless of light.
    //
    // PASSIVE ANIMALS (Beta 1.7.3 EntityAnimal) NEVER acquire: can_acquire=false means
    // aggro_time is never armed -> latched stays false -> the chase/strafe branches are
    // skipped and the mob takes the existing WANDER else-branch + shared locomotion,
    // drawing byte-identical RNG to a wandering hostile. The id-gated attack sites
    // (zombie-melee/skeleton-fire/creeper-prime/spider-leap) never fire for an animal id
    // either. So a single false here gives the full passive AI -- no separate tick path.
    //
    // VERIFIED NEGATIVE (M2, vs the real MCP b1.7.3 decompile): Beta 1.7.3
    // animals do NOT flee when hit -- panic/flee-on-hurt is 1.8+ behavior.
    // Do NOT "fix" a hit cow standing still; that is faithful. (The mc-dev
    // repo that suggests otherwise is 1.8 mislabeled -- a known trap.)
    bool can_acquire = !monster_is_passive(e->data.monster.id);
	if(is_wolf)
		can_acquire = false; // wolf targeting is state-driven above
    // PIG ZOMBIE neutrality [740c583: EntityPigZombie.findPlayerToAttack
    // returns null while angerLevel == 0]: it never self-acquires; anger comes
    // only from being hit (the retaliation site arms anger_level 400+rand(400),
    // and b1.7.3 never decrements it -- angry until death).
    if (e->data.monster.id == MONSTER_PIG_ZOMBIE)
        can_acquire = e->data.monster.anger_level > 0;
    if (e->data.monster.id == MONSTER_SPIDER) {
        // AUDIT #18 ITEM 7: spider passivity is gated by the SAME Beta getEntityBrightness
        // the sun-burn uses -- floor(feet + 0.66*bodyHeight), NOT the foot cell. For the
        // 0.9-tall spider this is feet + 0.594, so usually still the feet block, but it
        // differs on a light gradient (the spider's torso point can be brighter/darker than
        // its feet). Re-floor the column at that Y and read its sky/torch light.
        w_coord_t lx = (w_coord_t)floorf(e->pos[0]);
        w_coord_t ly = (w_coord_t)floorf(
            mob_brightness_sample_y(e->pos[1], MONSTER_SPIDER)); // feet + 0.594
        w_coord_t lz = (w_coord_t)floorf(e->pos[2]);
        struct block_data here;
        if (server_world_get_block(block_world(s), lx, ly, lz, &here)) {
            // F1: getEntityBrightness -> getBlockLightValue (neighbour max + weather).
            int eff = server_local_block_light_value(s, lx, ly, lz);
            can_acquire = spider_hostile_at_light(eff);
        }
    }

    float chase_dx = (float)(target->x - e->pos[0]);
    float chase_dz = (float)(target->z - e->pos[2]);
    // 3D acquisition/leash distance (parity fix 2026-06-19): Beta getDistanceSq /
    // getDistanceToEntity include the Y term, so acquisition (findPlayerToAttack ->
    // getClosestPlayer 16) and the leash gate on a SPHERE, not a 16-radius cylinder -- a
    // player on a tower/ledge is acquired only inside the true 3D range. STEERING + the
    // skeleton-standoff normalization stay HORIZONTAL (chase_dist_sq) -- Beta's attackEntity
    // lunge math is XZ-only, and 1/sqrt(chase_dist_sq) must normalize the horizontal chase_dx/dz.
    //
    // Y-BASIS DEVIATION (canonical note; corrected comment 2026-06-19 -- the earlier wording
    // wrongly called feet-to-feet "what Beta's posY uses"). Beta's getDistanceToEntity reads
    // each entity's posY RAW, and the PLAYER's posY is the EYE (EntityPlayer.yOffset 1.62)
    // while a mob's posY is its FEET (EntityLiving.yOffset 0) -- so Beta's vertical term is
    // player_EYE - mob_feet (EYE-to-feet). CaveWii deliberately uses FEET-to-feet here
    // (subtract PLAYER_EYE_HEIGHT to drop s->player.y to the feet): it preserves the long-
    // established, test-pinned 2.0-block FLAT-GROUND reach (the pre-parity melee gate was
    // XZ-only == feet-to-feet at dy 0), matches mc173's feet-normalized distance gates, and
    // the slime branch below NEEDS this basis (its box-collision trigger, modelled as a
    // distance, would leave a medium slime unable to reach a standing player under the eye
    // basis). The eye basis would TIGHTEN the flat-ground melee reach to ~1.17 -- a combat
    // re-tune deferred to the maintainer (docs/wii_controller_playability.md). All the gates
    // (despawn, acquire/leash, spider band, skeleton/creeper range, melee) share this one
    // feet-to-feet basis for consistency.
    float chase_dy = (float)(target->y - PLAYER_EYE_HEIGHT - e->pos[1]);
	if(wolf_entity_target) {
		chase_dx = wolf_entity_target->pos[0] - e->pos[0];
		chase_dy = wolf_entity_target->pos[1] - e->pos[1];
		chase_dz = wolf_entity_target->pos[2] - e->pos[2];
	}
    float chase_dist_sq = chase_dx * chase_dx + chase_dz * chase_dz;   // HORIZONTAL
    float chase_dist3d_sq = chase_dist_sq + chase_dy * chase_dy;       // 3D (acquire/leash)

    // Beta 1.7.3 canEntityBeSeen line-of-sight gate (audit #13). EntityMob gates
    // BOTH target acquisition (findPlayerToAttack: the nearest player within 16 that is
    // also visible) AND every per-tick attack dispatch (EntityCreature: a visible
    // target gets attackEntity, a hidden one attackBlockedEntity) on an eye-to-eye
    // voxel ray. Without it a mob acquires/chases/shoots/primes/swings THROUGH a
    // solid wall -- a bug, not a Beta behavior; this is a faithfulness fix, not a
    // playability carve-out. Computed ONCE here (distance-gated so the per-tick DDA
    // only runs for in-range candidates -- LOS is meaningless past the 16-block
    // follow range) and reused by all FIVE gates below: acquisition (can_see, spider
    // exempted -- see below), the skeleton fire, the creeper prime, the zombie/spider
    // melee, and the spider leap (EntitySpider.attackEntity's pounce half). A mob out
    // of follow range is never a candidate, so its LOS is irrelevant -> default false
    // (no wasted raycast). The acquisition position is the mob's start-of-tick
    // position (shared with the chase steer); the sub-block drift from this tick's
    // movement before the attack sites is negligible (Beta itself evaluates the two
    // checks at slightly different positions).
    //
    // SPIDER ACQUISITION EXCEPTION (fix 2026-09-01): the paragraph above is Beta-
    // correct for zombie/skeleton/creeper -- they inherit EntityMob.findPlayerToAttack
    // unmodified: the closest player within 16 blocks, accepted only if the mob
    // can see it, otherwise no target [740c583: EntityMob]
    // -- but it is WRONG for the spider, which OVERRIDES findPlayerToAttack and never
    // calls canEntityBeSeen at all: when its own brightness is below 0.5 it takes
    // the closest player within 16 blocks, and in brighter light it takes no
    // target [740c583: EntitySpider.findPlayerToAttack]. So a spider in the dark
    // ACQUIRES (and starts pathing toward) a player it cannot yet see -- e.g. through
    // a thin cave wall -- purely on distance + darkness; EntityCreature.updatePlayer-
    // ActionState calls getPathToEntity unconditionally the instant findPlayerToAttack
    // returns non-null, before any LOS check runs [740c583: EntityCreature]. This
    // exception is ACQUISITION-ONLY (can_see_for_acquire, below): the later per-tick
    // attackEntity/attackBlockedEntity DISPATCH decision (skeleton fire, creeper
    // prime, zombie/spider melee, spider leap) is EntityCreature's own base-class
    // mechanism, which the spider does NOT override, so plain `can_see` stays STRICT
    // everywhere else in this function.
    // TARGET LATCH (audit #14): Beta EntityCreature keeps entityToAttack set across
    // ticks once acquired -- a brief canEntityBeSeen miss only suppresses the per-tick
    // attack (attackBlockedEntity is empty) while the mob STILL paths/walks toward the
    // latched target. So acquisition keys on can_see, but CHASE persists through LOS
    // flicker. Without this, #13's instantaneous-can_see acquisition makes a mob drop to
    // AI_IDLE and re-wander the instant the line breaks at a wall corner -- the jitter.
    bool can_see = false;
    if(wolf_entity_target)
		can_see = chase_dist3d_sq < MOB_FOLLOW_RANGE_SQ;
    else if (target_available && chase_dist3d_sq < MOB_FOLLOW_RANGE_SQ)
        can_see = mob_can_see_player(s, e, target);
    // Acquisition-only relaxation for the spider (see the block comment above): every
    // other can_see consumer in this function reads the strict `can_see` directly.
    bool can_see_for_acquire
        = can_see || e->data.monster.id == MONSTER_SPIDER;

    // TODO(perf): the redundant floorf triples for the foot/eye/torso cells. Deferred --
    // premature for a 0.3ms tick. (chase_dist_sq is now computed once, above.)

    // (Re)arm / refresh the latch on SUSTAINED sight: a fresh acquire (can_acquire &&
    // can_see && in follow range) sets full patience, and every subsequent tick the mob
    // still sees the player resets it to full -- exactly Beta's "while you keep seeing
    // the target you keep it". The spider day-passivity (can_acquire) gates only this
    // acquisition path, NOT the retained chase, so a spider that was latched (by night
    // sight or by retaliation in SRPC_ENTITY_ATTACK) keeps chasing into daylight. Uses
    // can_see_for_acquire (spider-exempt) rather than can_see (fix 2026-09-01).
    // C-DEADTARGET: a tick that just DROPPED a dead target does not also
    // re-acquire it -- Beta's if/else-if runs exactly one of the two per tick
    // [EntityCreature.updatePlayerActionState:19-25].
    if (can_acquire && can_see_for_acquire && !dead_target_dropped
        && target_available && chase_dist3d_sq < MOB_FOLLOW_RANGE_SQ) {
        e->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
		if(fresh_target)
			e->data.monster.target_actor = server_local_actor_ident(s, target);
	}

    // Hold the target while patience remains AND the player is inside the leash. The
    // leash drop frees a fled player (the v2 beeline equivalent of Beta's mob failing
    // to path past its search range and standing -- Beta has no explicit distance drop).
    bool latched = mob_should_hold_target(e->data.monster.aggro_time, chase_dist3d_sq,
                                          MOB_LEASH_RANGE_SQ);
	if(is_wolf) {
		bool sitting = wolf_flag_get(e->data.monster.wolf_flags,
								 WOLF_FLAG_SITTING);
		bool has_wolf_target = e->data.monster.wolf_target_id != UINT32_MAX;
		latched = !sitting && !e->data.monster.wolf_shake_active
			&& has_wolf_target && chase_dist3d_sq < MOB_LEASH_RANGE_SQ;
		if(wolf_follow_owner && chase_dist3d_sq <= 25.0f)
			latched = false;
	}

    float cdir_x = 0.0f, cdir_z = 0.0f;
    bool is_skeleton = e->data.monster.id == MONSTER_SKELETON;
    // HAZARD-GATE the skeleton back-off (audit #17e): only the too-close AWAY steer
    // drives the mob backward, and with no engine cliff/lava avoidance that could
    // back-pedal it off a ledge or into lava. Probe the cell behind the mob (the unit
    // AWAY-from-player heading) ONCE here and pass the result in; skeleton_steer orbits
    // in place instead of retreating when the cell behind is unsafe. Only relevant when
    // actually too close, but the probe is a cheap 2-3 cell lookup so it is unconditional
    // for the latched skeleton (keeps the call site branch-free).
    bool backoff_safe = true;
    bool path_action_branch = false;
    float path_swim_roll = 0.0F;
    // C-DEADTARGET: the standoff/strafe is EntitySkeleton.attackEntity's
    // hasAttacked set + rotationYaw write [EntitySkeleton:49-50]; with a dead
    // target attackEntity never runs, hasAttacked stays false, and the mob takes
    // the ordinary held-target path branch (its nextInt(20)/nextInt(80) draws)
    // [EntityCreature.updatePlayerActionState:35-41].
    if (latched && is_skeleton && target_alive && chase_dist_sq > 1.0e-4f) {
        float inv = 1.0f / sqrtf(chase_dist_sq);
        backoff_safe = skeleton_backoff_safe(block_world(s), (float)e->pos[0],
                                             (float)e->pos[1], (float)e->pos[2],
                                             -chase_dx * inv, -chase_dz * inv);
    }
    if (latched && is_skeleton && target_alive
        && skeleton_steer(chase_dx, chase_dz, chase_dist_sq, MOB_FOLLOW_RANGE_SQ,
                          SKELETON_STANDOFF_MIN_SQ, SKELETON_STANDOFF_MAX_SQ,
                          // Stable per-skeleton handedness: id parity picks a
                          // consistent circle direction so it doesn't vibrate
                          // (no RNG -> the mob-owned gameplay_rng stream is untouched).
                          (e->id & 1) ? 1 : -1, backoff_safe, &cdir_x, &cdir_z)) {
        // Skeleton KEEP-DISTANCE + circle-strafe (audit #17e): instead of the shared
        // beeline-into-melee chase, hold a shooting standoff band and orbit the
        // player. This SUPERSEDES the melee arrival-decel below -- a bow-only skeleton
        // must NOT coast into melee reach (where it can neither shoot well nor melee);
        // skeleton_steer steers it OUT below SKELETON_STANDOFF_MIN (best-effort: at the
        // Beta-parity move_speed it only SLOWS a forward-pressing player, it does not
        // hold a hard standoff) and circles in-band. The tangential strafe is FAITHFUL-
        // IN-KIND b1.7.3 (EntityCreature hasAttacked block; the constant in-band orbit
        // is a deliberate amplification -- see combat.h #17e); the min-distance back-off
        // is a documented Wii-playability add, hazard-gated above. The arrow fire block
        // (below) is unchanged and keeps firing while can_see && in SKELETON_SHOOT_RANGE_SQ
        // -- the band nests inside that range so the skeleton stays in shooting distance.
        e->ai_state = AI_CHASE;
        e->data.monster.direction[0] = cdir_x;
        e->data.monster.direction[1] = cdir_z;
        e->data.monster.direction_time = 2; // hold the steer; re-aimed next tick
        // Decay patience on a non-refreshing latched tick, same as the chase branch
        // below (a tick that did not re-top aggro_time counts down toward give-up).
        if (e->data.monster.aggro_time > 0)
            e->data.monster.aggro_time--;
    } else if (latched) {
        e->ai_state = AI_CHASE;
        // FH-13: Beta EntityCreature's path recalc gate [740c583:
        // updatePlayerActionState]. hasAttacked is always false here (a ceased wolf
        // never reaches this branch -- `latched` is forced false while sitting/
        // shaking, see the wolf latch computation above), so the gate reduces to:
        // KEEP the existing path 1-in-20 of the time it exists (draw ONLY when a
        // path is actually held, matching Beta's short-circuit: the roll happens only with a
        // path, and a non-zero 1-in-20 result keeps it); otherwise re-search NOW. While keeping the path,
        // ALSO roll a background 1-in-80 chance to overwrite it with a fresh wander
        // path anyway -- a real Beta idiosyncrasy (EntityCreature interleaves
        // updateWanderPath even mid-chase), not a v2 addition; do not "fix" it away.
        bool has_path = e->data.monster.path.length > 0
            && !path_entity_is_finished(&e->data.monster.path);
        bool keep_path = has_path && jrand_next_int_bound(&e->gameplay_rng, 20) != 0;
        if (has_path && keep_path) {
            if (jrand_next_int_bound(&e->gameplay_rng, 80) == 0)
                mob_acquire_wander_path(s, e, is_wolf);
        } else {
            mob_acquire_chase_path(s, e, e->pos[0] + chase_dx,
                                   e->pos[1] + chase_dy, e->pos[2] + chase_dz);
        }
        // Decay patience even while chasing on the latch: a tick that did NOT refresh
        // it above (can_see was false / mob is day-passive) is a tick of LOS-miss, so
        // it counts down toward give-up. A refreshed tick set it to full above, so the
        // decrement just trims it to RETENTION-1 (re-topped next sighted tick) -- the
        // mob never gives up while it keeps seeing the player.
        if (e->data.monster.aggro_time > 0)
            e->data.monster.aggro_time--;
        // FH-13: nextInt(100) follow gate + waypoint steer, replacing mob_chase_dir.
        // FIX (mob-AI dead-zone, see mob_path_follow_tail's own comment): the
        // chase-stop distance must be the CHASING mob's own melee reach, not
        // one shared constant -- a wolf's real bite gate (WOLF_MELEE_REACH_SQ,
        // 1.5F) is tighter than the generic MOB_ATTACK_REACH_SQ (2.0F) every
        // other mob on this roster shares, so passing the generic value for a
        // wolf stopped its approach 0.5 blocks before it could ever land a
        // bite -- a second, independent instance of the same dead-zone family
        // (arrival/stop distance > actual attack range), this time via the
        // in_reach gate rather than path_follow_steer's arrival radius.
        path_action_branch = mob_path_follow_tail(s, e, true, chase_dist_sq,
                             is_wolf ? WOLF_MELEE_REACH_SQ
                                     : MOB_ATTACK_REACH_SQ);
        if(path_action_branch)
            path_swim_roll = jrand_next_float(&e->gameplay_rng);
    } else {
        // No held target (latch expired / leashed out / degenerate) -> WANDER. Bleed off
        // any residual patience (e.g. leashed out with ticks left) so it re-acquires
        // cleanly later, and ONLY re-roll a wander target here (never while latched,
        // which would corrupt the chase path).
        if (e->data.monster.aggro_time > 0)
            e->data.monster.aggro_time--;
        /* S1-a2: a spent latch RETIRES the target identity. Beta clears
         * entityToAttack when it gives up [740c583 EntityCreature.update
         * PlayerActionState: the attack target is cleared once it is no
         * longer valid], and the next findPlayerToAttack picks a player afresh.
         * Without this a mob that lost a GUEST target would keep measuring
         * chase/acquire distance to the guest forever and could never acquire
         * the host again. */
        if (e->data.monster.aggro_time == 0)
            e->data.monster.target_actor = ACTOR_IDENT_HOST;
        e->ai_state = AI_IDLE;
		bool wolf_ceased = is_wolf
			&& (wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_SITTING)
				|| e->data.monster.wolf_shake_active);
		if(wolf_ceased) {
			// hasAttacked set [740c583: EntityWolf.isMovementCeased]: Beta draws
			// NO recalc-gate RNG at all on this branch (the outer condition's
			// has-attacked term short-circuits before any nextInt call, and the
			// tail's held-path gate is false too since a ceased
			// wolf's path is never populated below). No path touched, no steer --
			// the wolf simply stands, exactly as before this port.
			e->data.monster.direction[0] = 0.0f;
			e->data.monster.direction[1] = 0.0f;
		} else if (e->data.monster.id == MONSTER_SQUID) {
			// Beta EntitySquid.updatePlayerActionState is a COMPLETE override
			// that never defers to the inherited action-state update -- it has no
			// relationship whatsoever to EntityCreature's pathToEntity/
			// updateWanderPath machinery. A real squid draws ZERO nextInt(80)
			// wander rolls and runs ZERO A* searches, ever. [740c583:
			// EntitySquid.updatePlayerActionState] can_acquire is forced false
			// for every passive id (monsters_object.c: squid .passive=true),
			// so a squid always reaches this WANDER branch -- without this
			// exclusion it drew a fresh nextInt(80) wander-roll (and, on a
			// hit, ~30 more RNG draws for mob_acquire_wander_path's candidate
			// scan) from the mob-owned gameplay_rng EVERY tick, even though the
			// result was never consumed: the squid's own dedicated swim
			// branch below (M2 final units) fully overwrites e->vel and never
			// reads path/direction. direction stays zero, matching the
			// wolf_ceased precedent just above (harmless: unread by the swim
			// branch).
			e->data.monster.direction[0] = 0.0f;
			e->data.monster.direction[1] = 0.0f;
		} else {
            // FH-13: Beta EntityCreature's wander-acquire gate [740c583:
            // updatePlayerActionState, the no-playerToAttack case -- the outer
            // condition is TRUE via short-circuit here, so it draws NO nextInt(20)].
            // Without a held path: draw nextInt(80); on a miss, draw a SECOND
            // nextInt(80) (Beta's no-path-and-hit OR second-roll-hit
            // short-circuit shape). With a held path: draw exactly one nextInt(80).
            bool has_path = e->data.monster.path.length > 0
                && !path_entity_is_finished(&e->data.monster.path);
            bool do_wander_roll;
            if (!has_path) {
                do_wander_roll = jrand_next_int_bound(&e->gameplay_rng, 80) == 0;
                if (!do_wander_roll)
                    do_wander_roll = jrand_next_int_bound(&e->gameplay_rng, 80) == 0;
            } else {
                do_wander_roll = jrand_next_int_bound(&e->gameplay_rng, 80) == 0;
            }
            if (do_wander_roll) {
                bool is_animal_lineage
                    = monster_is_passive(e->data.monster.id) || is_wolf;
                mob_acquire_wander_path(s, e, is_animal_lineage);
            }
            // FH-13: nextInt(100) follow gate + waypoint steer. is_chase=false
            // short-circuits in_reach unconditionally, so the reach argument
            // is inert here -- MOB_ATTACK_REACH_SQ is passed only to keep one
            // consistent value at every call site (never actually read).
            path_action_branch = mob_path_follow_tail(
                s, e, false, 0.0f, MOB_ATTACK_REACH_SQ);
            if(path_action_branch)
                path_swim_roll = jrand_next_float(&e->gameplay_rng);
        }
    }

    if(is_wolf) {
        bool tamed = wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_TAMED);
        bool angry = wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_ANGRY);
        bool wolf_has_path = e->data.monster.path.length > 0
            && !path_entity_is_finished(&e->data.monster.path);
        /* lane C #20: the wolf's look-at/interest target is a player of the
         * wolf's OWN world. Beta EntityLiving.updatePlayerActionState
         * :695-710 latches the world's closest player within 8 blocks
         * (World.getClosestPlayer :2166-2180: that world's players, first
         * strict minimum) and EntityWolf.onLivingUpdate :115-130 reads the
         * LATCHED player's held item (bone untamed / favourite meat tamed).
         * Candidates: server_local_eligible_actors_w over the wolf's world
         * (A1c; host first, then the guest), nearest by the existing
         * feet-to-feet basis, strict < so the host wins a tie. An actor in
         * another world never interests it; with none, the latch sees "no
         * player" (the same inputs as a host without a position).
         * DOCUMENTED RESIDUAL (needs an entity.h field, #21 ownership debt):
         * Beta keeps the ORIGINAL latched player until it drops; the latch
         * has no actor identity yet, so while latched the nearest eligible
         * actor stands in. Identical whenever one actor is present. */
        struct server_player* wolf_near = NULL;
        float player_dist_sq = 0.0f;
        {
            struct server_player* cand[SERVER_LOCAL_MAX_PRESENT_ACTORS];
            size_t n = server_local_eligible_actors_w(
                s, server_local_entity_world(s, e), cand,
                SERVER_LOCAL_MAX_PRESENT_ACTORS);
            for (size_t i = 0; i < n; i++) {
                /* A body without a position is not in the world's player
                 * list (Fable C20 F2): skip it, never let it shadow a
                 * positioned actor as "nearest". */
                if (!cand[i]->has_pos)
                    continue;
                float pdx = (float)cand[i]->x - e->pos[0];
                float pdy = (float)(cand[i]->y - PLAYER_EYE_HEIGHT) - e->pos[1];
                float pdz = (float)cand[i]->z - e->pos[2];
                float d2 = pdx * pdx + pdy * pdy + pdz * pdz;
                if (!wolf_near || d2 < player_dist_sq) {
                    wolf_near = cand[i];
                    player_dist_sq = d2;
                }
            }
        }
        // Ambient look-at-nearby-player latch + looksWithInterest gate
        // [740c583: EntityWolf.onLivingUpdate; EntityLiving.hasCurrentTarget/
        // currentTarget]; see entity_wolf.c's wolf_update_current_target_latch
        // / wolf_looks_with_interest for the exact citations and the one
        // documented residual (the 1-in-100 skip-roll edge case).
        wolf_update_current_target_latch(e, &e->gameplay_rng, wolf_has_path,
            wolf_near && wolf_near->has_pos, player_dist_sq);

        struct item_data wolf_held = {0};
        bool has_held = wolf_near && inventory_get_hotbar_item(
            &wolf_near->inventory, &wolf_held);
        bool held_favorite = wolf_near && wolf_near->has_pos && has_held
            && ((!tamed && wolf_held.id == ITEM_BONE)
                || (tamed && (wolf_held.id == ITEM_PORKCHOP
                             || wolf_held.id == ITEM_PORKCHOP_COOKED)));
        e->data.monster.wolf_interested = wolf_looks_with_interest(
            e->data.monster.wolf_has_current_target, wolf_has_path, angry,
            held_favorite);
        // [740c583: EntityWolf.onUpdate -- verbatim 0.4 head-course smoothing]
        e->data.monster.wolf_head_rotation_old
            = e->data.monster.wolf_head_rotation;
        float interest = e->data.monster.wolf_interested ? 1.0f : 0.0f;
        e->data.monster.wolf_head_rotation
            += (interest - e->data.monster.wolf_head_rotation) * 0.4f;
        // [EntityWolf.onUpdate :151-153] while looking with interest the
        // chase/look counter is reset to 10 -- a wolf shown its favorite item
        // keeps the look latch (and so the head tilt) for as long as the item
        // is shown in range: the next latch check decrements 10 to 9, never
        // to <= 0 [EntityLiving.updatePlayerActionState :706-708]. Without
        // this pin the latch lapsed 10-29 ticks after forming and the tilt
        // flickered off until the next 2% re-roll (Fable C20 F3a).
        if(e->data.monster.wolf_interested)
            e->data.monster.wolf_current_target_hold = 10;
    }

    // M2 final units: the slime and the squid OWN their locomotion. The slime
    // hops (its block sits after the Y-move below, where this tick's on_ground
    // is fresh); the squid swims (its block replaces the gravity/water section
    // below). Neither takes the shared walk push -- Beta's EntitySlime zeroes
    // moveStrafing/moveForward while grounded between hops, and EntitySquid
    // overrides moveEntityWithHeading to bypass the walk pipeline entirely.
    const bool is_squid = e->data.monster.id == MONSTER_SQUID;
    const bool is_slime = monster_is_slime(e->data.monster.id);
    const bool is_ghast = e->data.monster.id == MONSTER_GHAST;

    // =====================================================================
    // MOB MOTION UNITS S2 (docs/intent/mob-motion-units.md; Astra rulings
    // 2026-09-27). Motion is blocks per tick, 1:1 with Beta, on the
    // authoritative doubles (mob_motion.h); e->pos / e->vel are write-only
    // mirrors. Beta's order within the tick:
    //   EntityCreature/EntityLiving.updatePlayerActionState: attackEntity
    //     (the spider/wolf leap) and isJumping;
    //   EntityLiving.onLivingUpdate:629-642: a jumping mob gains vertical
    //     0.04 (float widened) in water/lava, else jump() sets 0.42 (float widened) when
    //     onGround; moveEntityWithHeading: moveFlying push, the ladder clamp,
    //     moveEntity (mob_move_entity_d), then -- after moveEntity's own
    //     tail below -- the ladder ascend and the branch gravity/drag.
    // Retired with this card: the engine GRAVITY 2.0 / *0.9 integrator, the
    // x25 knockback bridge, the 3.3/3.2 engine-unit hops, the auto-jump
    // probe, the ySize jump bank and the surface-glide band. Kept (carve-out
    // #2, docs/wii_controller_playability.md): the CREEPER_ACCEL walk push
    // unit, the per-mob Wii speed cap and the knockback steer-suppression
    // window -- horizontal feel values, now applied to the same
    // blocks/tick motion Beta uses.
    // =====================================================================
    struct mob_motion* md = &e->data.monster.motion_d;
    const int monster_id = e->data.monster.id;
    // Beta's onGround field: the previous moveEntity's verdict (kept on the
    // entity across ticks now -- nothing clears it any more).
    const bool beta_on_ground = e->on_ground;
    // handleLavaMovement() for EVERY mob: the lava movement branch needs it
    // (the hazard block above scans only non-immune mobs, for the damage).
    if (!lava_scanned) {
        lava_movement = mob_body_in_lava(s, e);
        lava_scanned = true;
    }
    const bool liquid_branch = body_in_water || lava_movement;
    const int beta_block_under
        = (!liquid_branch && beta_on_ground) ? mob_block_under_id(s, e) : 0;
    const float ground_friction
        = mob_beta_ground_friction(beta_on_ground, beta_block_under);
    // Beta: input push is 0.1 times the land-acceleration scale on ground, else 0.02.
    // The engine's push unit is CREEPER_ACCEL where Beta's is 0.1F (fork
    // model + the Wii cap, carve-out #2), so the grounded/airborne SELECT and
    // its ratios are transcribed on that unit.
    const float beta_push_scale = beta_on_ground
        ? mob_beta_land_accel_scale(ground_friction)
        : (0.02F / 0.1F);
    bool wolf_ceased = is_wolf
		&& (wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_SITTING)
			|| e->data.monster.wolf_shake_active);

    // --- attackEntity leaps, PRE-move. EntityCreature.updatePlayerActionState
    // dispatches attackEntity (only with canEntityBeSeen) before the path
    // follow; EntitySpider/EntityWolf.attackEntity require a target distance
    // strictly between 2 and 6 blocks and a 1-in-10 roll, and only then read
    // onGround for the impulse [EntitySpider.java:46-54, EntityWolf.java:295-303]:
    //     each horizontal velocity becomes the unit direction to the target times 0.5, then times float
    //     0.8 widened to double, plus the old value times widened float 0.2; vertical is widened float 0.4.
    // consumed by THIS tick's moveEntity, then gravity. Beta has no wall
    // exclusion here (the old post-move `collision_xz` gate is gone); the
    // spider's wall climb is the ladder ascend further down.
    if (monster_id == MONSTER_SPIDER) {
        float ldx = (float)(target->x - e->pos[0]);
        float ldz = (float)(target->z - e->pos[2]);
        // 3D band on the shared FEET-to-feet Y basis (the canonical Y-BASIS
        // DEVIATION note at chase_dy); the impulse itself is horizontal.
        float ldy = (float)(target->y - PLAYER_EYE_HEIGHT - e->pos[1]);
        if (target_alive && can_see && (can_acquire || latched)
            && spider_leap_in_band(ldx * ldx + ldy * ldy + ldz * ldz)
            && jrand_next_int_bound(&e->gameplay_rng, SPIDER_LEAP_CHANCE_DEN) == 0
            && beta_on_ground)
            mob_leap_impulse_d(md, target->x - md->pos[0],
                               target->z - md->pos[2]);
    }
    if (is_wolf && e->data.monster.wolf_target_id != UINT32_MAX
        && !wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_SITTING)
        && !e->data.monster.wolf_shake_active) {
        struct entity* wt = entity_wolf_target(e, s);
        bool target_player = wolf_target_is_hostile_player(
            e->data.monster.wolf_target_id, e->data.monster.wolf_flags);
        float wdx, wdy, wdz;
        double wtx = 0.0, wtz = 0.0;
        if (target_player) {
            wdx = (float)target->x - e->pos[0];
            wdy = (float)(target->y - PLAYER_EYE_HEIGHT) - e->pos[1];
            wdz = (float)target->z - e->pos[2];
            wtx = target->x;
            wtz = target->z;
        } else if (wt) {
            wdx = wt->pos[0] - e->pos[0];
            wdy = wt->pos[1] - e->pos[1];
            wdz = wt->pos[2] - e->pos[2];
            wtx = wt->type == ENTITY_MONSTER
                ? wt->data.monster.motion_d.pos[0] : (double)wt->pos[0];
            wtz = wt->type == ENTITY_MONSTER
                ? wt->data.monster.motion_d.pos[2] : (double)wt->pos[2];
        } else {
            wdx = wdy = wdz = 0.0f;
        }
        bool wolf_can_see = target_player
            ? mob_can_see_player(s, e, target)
            : (wt && mob_can_see_point(s, e, wt->pos[0],
                    wt->pos[1] + mob_eye_height(wt->data.monster.id),
                    wt->pos[2]));
        float wd2 = wdx * wdx + wdy * wdy + wdz * wdz;
        bool wolf_target_live = target_player ? target_alive : (wt != NULL);
        // Beta draws the roll before reading onGround (the same nesting as
        // the spider); the old engine gate short-circuited it behind onGround.
        if (wolf_target_live && wolf_can_see && wolf_leap_in_band(wd2)
            && jrand_next_int_bound(&e->gameplay_rng, WOLF_LEAP_CHANCE_DEN) == 0
            && beta_on_ground)
            mob_leap_impulse_d(md, wtx - md->pos[0], wtz - md->pos[2]);
    }

    // --- GHAST course (EntityGhast.updatePlayerActionState, PRE-move): the
    // waypoint re-roll + the 0.1 acceleration toward it are consumed by this
    // tick's moveEntity, then EntityFlying's drag (below). The rolls keep
    // their place ahead of the fireball cycle further down the tick.
    if (is_ghast) {
        float* wp = e->data.monster.ghast_waypoint;
        float wdx = wp[0] - e->pos[0];
        float wdy = wp[1] - e->pos[1];
        float wdz = wp[2] - e->pos[2];
        float wdist = sqrtf(wdx * wdx + wdy * wdy + wdz * wdz);
        if (wdist < 1.0f || wdist > 60.0f) {
            wp[0] = e->pos[0]
                + (jrand_next_float(&e->gameplay_rng) * 2.0f - 1.0f) * 16.0f;
            wp[1] = e->pos[1]
                + (jrand_next_float(&e->gameplay_rng) * 2.0f - 1.0f) * 16.0f;
            wp[2] = e->pos[2]
                + (jrand_next_float(&e->gameplay_rng) * 2.0f - 1.0f) * 16.0f;
            wdx = wp[0] - e->pos[0];
            wdy = wp[1] - e->pos[1];
            wdz = wp[2] - e->pos[2];
            wdist = sqrtf(wdx * wdx + wdy * wdy + wdz * wdz);
        }
        if (e->data.monster.ghast_course_cooldown-- <= 0) {
            e->data.monster.ghast_course_cooldown
                += jrand_next_int_bound(&e->gameplay_rng, 5) + 2;
            // isCourseTraversable: sweep the 4x4x4 body along the straight
            // line in unit steps; any solid cell blocks the course.
            bool traversable = true;
            if (wdist >= 1.0f) {
                float ivx = wdx / wdist, ivy = wdy / wdist, ivz = wdz / wdist;
                for (int step = 1; (float)step < wdist && traversable;
                     step++) {
                    float cx = e->pos[0] + ivx * (float)step;
                    float cy = e->pos[1] + ivy * (float)step;
                    float cz = e->pos[2] + ivz * (float)step;
                    for (int oy = 0; oy < 4 && traversable; oy++)
                        for (int ox = -2; ox < 2 && traversable; ox++)
                            for (int oz = -2; oz < 2 && traversable; oz++) {
                                struct block_data cb;
                                if (server_world_get_block(
                                        block_world(s),
                                        (w_coord_t)floorf(cx + (float)ox),
                                        (w_coord_t)floorf(cy + (float)oy),
                                        (w_coord_t)floorf(cz + (float)oz), &cb)
                                    && block_is_normal_cube(cb.type))
                                    traversable = false;
                            }
                }
            }
            if (traversable && wdist >= 1.0f) {
                // adds 0.1 of the unit offset to the waypoint; the offset is
                // double, its length is taken by MathHelper.sqrt_double (float).
                double gx = (double)wp[0] - md->pos[0];
                double gy = (double)wp[1] - md->pos[1];
                double gz = (double)wp[2] - md->pos[2];
                double gd = (double)(float)sqrt(gx * gx + gy * gy + gz * gz);
                if (gd > 0.0) {
                    md->motion[0] += gx / gd * 0.1;
                    md->motion[1] += gy / gd * 0.1;
                    md->motion[2] += gz / gd * 0.1;
                }
            } else {
                wp[0] = e->pos[0];
                wp[1] = e->pos[1];
                wp[2] = e->pos[2];
            }
        }
    }

    // --- SQUID swim-vector repick (EntitySquid.updatePlayerActionState,
    // PRE-move): repick on a 1-in-50 roll, when out of water, or when the
    // vector is zero; the 1-in-50 roll is drawn EVERY tick (evaluated first). Gate on the PRE-move
    // loose-edge sample (Beta reads the cached inWater field).
    if (is_squid) {
        bool roll50_hit = jrand_next_int_bound(&e->gameplay_rng, 50) == 0;
        float* sv = e->data.monster.swim_vec;
        bool vec_zero = sv[0] == 0.0f && sv[1] == 0.0f && sv[2] == 0.0f;
        if (squid_should_repick(roll50_hit, squid_gate_wet, vec_zero)) {
            // EntitySquid.java:133-135 draws angle before vertical velocity.
            float angle_draw = jrand_next_float(&e->gameplay_rng);
            float y_draw = jrand_next_float(&e->gameplay_rng);
            squid_pick_swim_vector(angle_draw, y_draw, sv);
        }
    }

    // --- isJumping (updatePlayerActionState) and its consumption
    // (onLivingUpdate:629-637).
    //   * water/lava: jump when a float roll is below 0.8 (the one draw the
    //     engine already took in water; lava now draws it too, as Beta does);
    //   * on a path: the next waypoint above floor(boundingBox.minY + 0.5)
    //     [EntityCreature.java:43,65,95-97];
    //   * slime: its own hop timer (below).
    // EntityCreature.java:108 evaluates the float BEFORE the liquid gate
    // after entering the path branch, even if its final waypoint was consumed.
    // EntityLiving.java:721-723 idle fallback draws only in water/lava.
    // The separate collided-horizontal/final-waypoint jump debt is unchanged.
    bool is_jumping = false;
    if (!is_squid && !is_ghast && !is_slime) {
        if (path_action_branch) {
            if (e->data.monster.path.length > 0
                && !path_entity_is_finished(&e->data.monster.path)) {
                float wx, wy, wz;
                path_entity_get_position(&e->data.monster.path, e->width, &wx,
                                         &wy, &wz);
                int32_t feet_cell_y = mob_floor_d(md->bb.min[1] + 0.5);
                if ((double)wy - (double)feet_cell_y > 0.0)
                    is_jumping = true;
            }
            if (path_swim_roll < MOB_WATER_JUMP_CHANCE && liquid_branch)
                is_jumping = true;
        } else if (liquid_branch) {
            is_jumping = jrand_next_float(&e->gameplay_rng) < MOB_WATER_JUMP_CHANCE;
        }
    }

    // --- SLIME hop (EntitySlime.updatePlayerActionState, PRE-move): while
    // grounded slimeJumpDelay counts down; at <= 0 it re-rolls (rand(20)+10,
    // /3 with a player near), draws the strafe roll, and sets isJumping -> the
    // standard jump() (0.42, float widened). The horizontal hop keeps the engine's
    // size-scaled, Wii-capped heading model (carve-out #2; see the combat.h
    // slime notes); grounded between hops the horizontal motion is parked.
    // Knockback suppresses the hop; in water Beta's slime has no swim input
    // (its override never sets isJumping there) and simply sinks.
    if (is_slime && !knocked && !body_in_water && beta_on_ground) {
        if (--e->data.monster.jump_delay <= 0) {
            e->data.monster.jump_delay = slime_jump_delay(
                jrand_next_int_bound(&e->gameplay_rng, 20), latched);
            // S2 hop SOUND: only for getSlimeSize() > 1, vol 0.6F, pitch
            // wobble*0.8F. Both pitch floats are the slime's own Random and
            // fall BETWEEN the nextInt(20) delay re-roll and the strafe
            // nextFloat (LIVING-SOUND-RNG).
            // [server EntitySlime.updatePlayerActionState]
            if (slime_beta_size(monster_id) > 1)
                server_local_emit_sound(
                    SOUND_MOB_SLIME, (vec3) {e->pos[0], e->pos[1], e->pos[2]},
                    0.6f, mob_living_sound_pitch(e) * 0.8f);
            float strafe_roll = slime_strafe_roll(jrand_next_float(&e->gameplay_rng));
            is_jumping = true;
            int beta_size = slime_beta_size(monster_id);
            float faithful_base
                = monster_def_get(monster_id)->move_speed * (float)beta_size;
            float hop_speed = mob_speed_cap_wii(faithful_base);
            float steer_x, steer_z;
            slime_hop_steer_heading(e->data.monster.direction[0],
                                    e->data.monster.direction[1],
                                    e->data.monster.body_yaw, &steer_x,
                                    &steer_z);
            float wob_x, wob_z;
            slime_hop_wobble_dir(steer_x, steer_z, strafe_roll, beta_size,
                                 &wob_x, &wob_z);
            md->motion[0] = (double)(wob_x * hop_speed);
            md->motion[2] = (double)(wob_z * hop_speed);
        } else {
            md->motion[0] = 0.0; // grounded slime: zero move input
            md->motion[2] = 0.0;
        }
    }

    if (is_jumping) {
        if (liquid_branch)
            md->motion[1] += (double)0.04F;
        else if (beta_on_ground)
            md->motion[1] = (double)0.42F; // EntityLiving.jump:659-661
    }

    // --- moveFlying push (moveEntityWithHeading:458/469/489). While being
    // knocked back the walk push and the speed cap are skipped (the
    // steer-suppression window, carve-out #2) so the shove carries. The ghast
    // has no walk input (its motion is the course acceleration above); the
    // squid and the slime own their locomotion.
    if (!knocked && !is_squid && !is_slime && !is_ghast && !wolf_ceased) {
        if (liquid_branch) {
            // The flat 0.02F moveFlying factor along the steer heading; the
            // land accel and the Wii cap do not run in water or lava.
            md->motion[0] += (double)(MOB_WATER_INPUT
                                      * e->data.monster.direction[0]);
            md->motion[2] += (double)(MOB_WATER_INPUT
                                      * e->data.monster.direction[1]);
        } else {
            float accel = CREEPER_ACCEL * beta_push_scale;
            md->motion[0] += (double)(accel * e->data.monster.direction[0]);
            md->motion[2] += (double)(accel * e->data.monster.direction[1]);
            // Wii-controller PLAYABILITY: cap the mob's horizontal speed per
            // mob (mob_speed_cap_wii); the pig zombie's per-tick moveSpeed
            // [EntityPigZombie.onUpdate: 0.95F angry / 0.5F] feeds it.
            float base_speed = monster_def_get(monster_id)->move_speed;
            if (monster_id == MONSTER_PIG_ZOMBIE)
                base_speed = (e->data.monster.anger_level > 0
                              && e->data.monster.aggro_time > 0)
                    ? 0.95f
                    : 0.5f;
            double cap = (double)mob_speed_cap_wii(base_speed);
            double hm = sqrt(md->motion[0] * md->motion[0]
                             + md->motion[2] * md->motion[2]);
            if (hm > cap) {
                double sc = cap / hm;
                md->motion[0] *= sc;
                md->motion[2] *= sc;
            }
        }
    }

    // --- LADDER, PRE-move clamp (moveEntityWithHeading:499-524): only in the
    // DRY branch (Beta's water/lava branches have no ladder code), never for
    // the ghast/squid (their own moveEntityWithHeading). EntitySpider's
    // isOnLadder() is isCollidedHorizontally -- the PREVIOUS move's flag here.
    const bool ladder_branch = !liquid_branch && !is_ghast && !is_squid;
    const bool ladder_rules = ladder_branch && monster_id != MONSTER_SPIDER;
    if (ladder_branch
        && (monster_id == MONSTER_SPIDER ? e->data.monster.prev_collided_xz
                                         : mob_on_ladder(s, e))) {
        const double ladder_cap = (double)0.15F;
        if (md->motion[0] < -ladder_cap)
            md->motion[0] = -ladder_cap;
        if (md->motion[0] > ladder_cap)
            md->motion[0] = ladder_cap;
        if (md->motion[2] < -ladder_cap)
            md->motion[2] = -ladder_cap;
        if (md->motion[2] > ladder_cap)
            md->motion[2] = ladder_cap;
        e->data.monster.fall_distance = 0.0F;
        if (md->motion[1] < -0.15)
            md->motion[1] = -0.15;
    }

    // --- moveEntity (Entity.java:252-434, mob_move_entity_d): Y, X, Z clip
    // against the colliding boxes (blocks, moving pistons, boats/minecarts),
    // the stepHeight retry and its ySize bank, onGround / collided flags,
    // motion zeroed per clipped axis. The web scaling rides inside it.
    const double pre_move_y = md->pos[1];
    struct mob_collide_env env = mob_collide_env_for(s);
    struct mob_move_result mv = mob_move_entity_d(
        e, md, md->motion[0], md->motion[1], md->motion[2], &env);
    bool collision_xz = e->collided_horizontally;
    // Beta onGround / isCollidedHorizontally, kept until the next move;
    // mirrored into the persisted/networked fields.
    e->data.monster.prev_on_ground = e->on_ground;
    e->data.monster.prev_collided_xz = collision_xz;

    // --- Entity.updateFallState on the applied dy and onGround [Entity.java:505-515]:
    // airborne, fallDistance drops by the APPLIED dy while falling; on the
    // landing move fall(fallDistance) and reset. Water cancels: Beta's
    // handleWaterMovement zeroes fallDistance every wet tick; the post-move
    // foot probe additionally cancels a landing that ends in water this tick
    // (the pre-card rule, kept -- see the block comment history).
    bool landed_in_water = body_in_water;
    if (!landed_in_water) {
        struct block_data pfoot;
        if (server_world_get_block(block_world(s), mob_floor_d(md->pos[0]),
                                   mob_floor_d(md->pos[1]),
                                   mob_floor_d(md->pos[2]), &pfoot)
            && (pfoot.type == BLOCK_WATER_FLOW || pfoot.type == BLOCK_WATER_STILL))
            landed_in_water = true;
    }
    int pending_fall_damage = 0;
    if (landed_in_water) {
        e->data.monster.fall_distance = 0.0f;
    } else if (e->on_ground) {
        // Beta chicken overrides fall() to an empty body (monster_def
        // fall_immune); the reset still happens.
        if (!monster_def_get(monster_id)->fall_immune)
            pending_fall_damage
                = mob_fall_damage(e->data.monster.fall_distance);
        // S2 SLIME LAND squish on the airborne-to-onGround edge, size > 2 only,
        // vol 0.6F, pitch wobble/0.8F, on the slime's own Random
        // (LIVING-SOUND-RNG). RESIDUAL: Beta first draws size*8*2 landing-
        // particle floats on that same Random (EntitySlime.onUpdate); those
        // particle draws are not modelled server-side yet.
        // [server EntitySlime.onUpdate]
        if (e->data.monster.fall_distance > 0.0f
            && slime_beta_size(monster_id) > 2)
            server_local_emit_sound(
                SOUND_MOB_SLIME, (vec3) {e->pos[0], e->pos[1], e->pos[2]},
                0.6f, mob_living_sound_pitch(e) / 0.8f);
        e->data.monster.fall_distance = 0.0f;
    } else if (!mv.incomplete && mv.dy < 0.0) {
        e->data.monster.fall_distance
            = (float)((double)e->data.monster.fall_distance - mv.dy);
    }

    /* Beta's updateFallState runs after all three collision axes, followed by
     * the full-box block callback scan. Delay the already-computed landing hit
     * until this point so fall arbitrates before cactus, at the final position. */
    if (pending_fall_damage > 0) {
        (void)monster_apply_environment_damage(e, s, pending_fall_damage);
        // S2 landing THUD: Beta EntityLiving.fall plays the block-under-feet
        // STEP sound at half its volume and 0.75 of its pitch ONLY when the
        // fall dealt damage (> 3 blocks), right after the attackEntityFrom
        // (whose fresh edge just played hurt/death above). Cell: Beta
        // floor(posY - 0.2 - yOffset); mob yOffset is 0, posY is the feet.
        // No snow/liquid special-casing in fall() -- only blockId > 0.
        // [740c583: EntityLiving.fall:440-452]
        struct block_data under;
        if (server_world_get_block(block_world(s), (w_coord_t)floorf(e->pos[0]),
                                   (w_coord_t)floorf(e->pos[1] - 0.2f),
                                   (w_coord_t)floorf(e->pos[2]), &under))
            server_local_emit_sound(
                audio_block_step_sound_id(under.type),
                (vec3) {e->pos[0], e->pos[1], e->pos[2]},
                audio_block_sound_volume(under.type) * 0.5f,
                audio_block_sound_pitch(under.type) * 0.75f);
    }

    // --- S2 MOB SPLASH + FOOTSTEPS (post-move, full tick displacement) -----
    // Both are movement-driven and can fire for MANY live mobs per second, so
    // they carry a server-side 20-block earshot cull: at vol<=1 the client's
    // own audio_play3d culls beyond the Beta 16-block radius anyway (camera ~=
    // player eye; the +4 margin absorbs interpolation drift), so the cull is
    // output-identical -- it only spares the 16-slot CRPC queue the mob-farm
    // storm (the scout's rate hazard). Rare per-mob sounds (idle/hurt/fall)
    // stay uncalled, like Beta SMP.
    {
        // The latch and the odometer run EVERY tick (state must stay correct
        // for far mobs -- a stale dry latch would fake a splash the moment
        // the player walked into earshot of a long-swimming mob); ONLY the
        // emits are culled.
        struct server_world* sound_world = server_local_entity_world(s, e);
        struct server_player* listeners[SERVER_LOCAL_MAX_PRESENT_ACTORS];
        size_t listener_count = server_local_eligible_actors_w(
            s, sound_world, listeners, SERVER_LOCAL_MAX_PRESENT_ACTORS);
        bool in_earshot = false;
        for (size_t i = 0; i < listener_count; i++) {
            struct server_player* listener = listeners[i];
            if (listener != &s->player
                && (!listener->has_pos || server_local_actor_resolve(s,
                        server_local_actor_ident(s, listener)) != listener))
                continue;
            float pdx = (float)(listener->x - e->pos[0]);
            float pdy = (float)(listener->y - PLAYER_EYE_HEIGHT - e->pos[1]);
            float pdz = (float)(listener->z - e->pos[2]);
            if (pdx * pdx + pdy * pdy + pdz * pdz <= 20.0f * 20.0f) {
                in_earshot = true;
                break;
            }
        }

        // SPLASH: the dry->wet edge now runs at the tick top, on Beta's own
        // inWater verdict (the handleWaterMovement site above, next-gaps-0924/
        // W2 P3) -- the post-move foot-cell probe that lived here was the
        // second water definition that card removed.
        w_coord_t sfx = (w_coord_t)floorf(e->pos[0]);
        w_coord_t sfz = (w_coord_t)floorf(e->pos[2]);

        // FOOTSTEPS: the same Entity.moveEntity odometer as the player
        // (shared server_player_walk_step: 0.6-weighted horizontal distance,
        // one step per integer crossing with a non-air foot cell), through
        // the shared snow-override/liquid-silence tail. canTriggerWalking()
        // is FALSE for the spider (silent mover); every other roster id
        // inherits TRUE -- incl. the squid, whose permanently-liquid foot
        // cell silences it naturally.
        // [740c583: Entity.moveEntity:478-499; EntitySpider]
		if (e->data.monster.id != MONSTER_SPIDER
			&& e->data.monster.id != MONSTER_WOLF) {
            w_coord_t ffy = (w_coord_t)floorf(e->pos[1] - 0.2f);
            struct block_data footc, fence_chk;
            bool solid_below
                = server_world_get_block(block_world(s), sfx, ffy, sfz, &footc)
                && footc.type != BLOCK_AIR;
            // Beta's foot-block-id>0 gate uses the FENCE-substituted id: an air foot
            // cell over a fence still fires. [740c583: Entity.moveEntity:483-488]
            if (!solid_below
                && server_world_get_block(block_world(s), sfx, ffy - 1, sfz,
                                          &fence_chk)
                && audio_footstep_block_id(BLOCK_AIR, fence_chk.type)
                    != BLOCK_AIR)
                solid_below = true;
            if (server_player_walk_step(
                    &e->data.monster.walk_dist,
                    &e->data.monster.walk_next_step,
                    (double)(e->pos[0] - e->pos_old[0]),
                    (double)(e->pos[2] - e->pos_old[2]), solid_below)) {
                if (in_earshot)
                    server_local_emit_footstep_world(
                        sound_world, sfx, ffy, sfz,
                        (vec3) {e->pos[0], e->pos[1], e->pos[2]});
                // R1 (BLOCKS lane, one call site): Block.onEntityWalking after
                // the step sound, NOT earshot-culled -- it is world state.
                // [740c583: Entity.java:457]
                server_local_entity_walked_on_block_w(
                    s, server_local_entity_world(s, e), sfx, ffy, sfz,
                    BLOCK_WALKER_MOB); /* A2-T D4: the mob's world */
            }
        }
    }
    entity_monster_post_move_cactus_collisions(e, s);

    /* Entity.moveEntity's burning-box hit follows the earlier lava 4 and the
     * post-move block callbacks. World.isBoundingBoxBurning scans the body box
     * contracted by .001, including fire and both lava ids. Its inclusive
     * checkChunksExist preflight uses the exclusive scan's +1 bounds.
     * [mcb173-retromcp: Entity.java:480-490; World.java:1395-1415]. */
    {
        const struct monster_def* def = monster_def_get(e->data.monster.id);
        // Mob motion units S2: Beta's boundingBox on the doubles.
        const struct mob_aabb_d* bbx = &e->data.monster.motion_d.bb;
        (void)def;
        const w_coord_t x0 = (w_coord_t)floor(bbx->min[0] + 0.001);
        const w_coord_t x1 = (w_coord_t)floor(bbx->max[0] - 0.001 + 1.0);
        const w_coord_t y0 = (w_coord_t)floor(bbx->min[1] + 0.001);
        const w_coord_t y1 = (w_coord_t)floor(bbx->max[1] - 0.001 + 1.0);
        const w_coord_t z0 = (w_coord_t)floor(bbx->min[2] + 0.001);
        const w_coord_t z1 = (w_coord_t)floor(bbx->max[2] - 0.001 + 1.0);
        bool loaded = y1 >= 0 && y0 < WORLD_HEIGHT;
        for(w_coord_t cx = WCOORD_CHUNK_OFFSET(x0);
            loaded && cx <= WCOORD_CHUNK_OFFSET(x1); cx++)
            for(w_coord_t cz = WCOORD_CHUNK_OFFSET(z0);
                loaded && cz <= WCOORD_CHUNK_OFFSET(z1); cz++)
                loaded = server_world_is_chunk_loaded(block_world(s), cx, cz);
        bool burning_box = false;
        for(w_coord_t x = x0; loaded && !burning_box && x < x1; x++)
            for(w_coord_t y = y0; !burning_box && y < y1; y++)
                for(w_coord_t z = z0; z < z1; z++) {
                    struct block_data block;
                    if(server_world_get_block(block_world(s), x, y, z, &block)
                       && (block.type == BLOCK_FIRE
                           || block.type == BLOCK_LAVA_FLOW
                           || block.type == BLOCK_LAVA_STILL)) {
                        burning_box = true;
                        break;
                    }
                }
        if(burning_box) {
            if(!def->fire_immune)
                (void)monster_apply_environment_damage(e, s, MOB_FIRE_DAMAGE);
            if(def->fire_immune) {
                /* G6b F1: Beta EntityLiving.onEntityUpdate:117 zeroes an
                 * immune mob's fire every tick, and Entity.onEntityUpdate:
                 * 204-207 decays any leftover by 4 -- both BEFORE
                 * the burning flag (bit 0) is set from fire above 0 (Entity.java:226). The box's
                 * ++ below can therefore never surface as a burning flag;
                 * this port snapshots on_fire after the move, so keep the
                 * counter at the value Beta's flag actually sees. */
                e->data.monster.fire_time = 0;
            } else if(!server_entity_is_wet(s, body_in_water, e->pos[0],
                          e->pos[1], e->pos[2])) {
                e->data.monster.fire_time++;
                if(e->data.monster.fire_time == 0)
                    e->data.monster.fire_time = MOB_FIRE_TICKS;
            }
        } else if(e->data.monster.fire_time <= 0) {
            /* G6b F2: Entity.java:489-490 otherwise, a non-positive fire counter is set
             * to minus the fire resistance, fireResistance defaulting to 1 (Entity.java:52).
             * The idle -1 is what lets the box's ++ reach 0 -> 300 (:484-486). */
            e->data.monster.fire_time = -MOB_FIRE_RESISTANCE;
        }
    }

    /* EntityLiving.onUpdate calls onEntityUpdate before onLivingUpdate;
     * onLivingUpdate moves through moveEntity, whose isWet check follows
     * movement and block collisions. [Beta EntityLiving.java:203-205,642;
     * Entity.java:480-496,535-537] Keep the earlier water flag (inWater)
     * but sample rain at the final position of this move. */
    if (server_entity_is_wet(s, body_in_water, e->pos[0], e->pos[1],
            e->pos[2]) && e->data.monster.fire_time > 0) {
        struct random_gen* sg = server_local_sound_rng();
        server_local_emit_sound(SOUND_RANDOM_FIZZ,
            (vec3) {e->pos[0], e->pos[1], e->pos[2]}, 0.7f,
            1.6f + (rand_gen_flt(sg) - rand_gen_flt(sg)) * 0.4f);
        e->data.monster.fire_time = -MOB_FIRE_RESISTANCE; // Entity.java:52,493-496
    }

    // moveEntity is later than this tick's deathTime phase: onDeath runs now,
    // while death_time remains zero until the next server tick.
    if (e->health <= 0) {
        vec3 here = { e->pos[0], e->pos[1], e->pos[2] };
        entity_monster_begin_death(e, s, here);
        return false;
    }

    // GROUNDED SNAPSHOT (parity fix 2026-06-19): capture on_ground HERE, after this
    // tick's move (axis-1 at the Y-move set it; the XZ moves above don't touch it) but
    // BEFORE entity_apply_gravity unconditionally clears it. Beta reads its onGround field
    // -- the end-of-previous-move grounded state -- for BOTH the ground-friction slip
    // selection (EntityLiving.moveEntityWithHeading) AND the spider pounce gate
    // (EntitySpider.attackEntity's grounded check), which run AFTER moveEntity but
    // BEFORE the next move applies gravity. The old split entity_apply_gravity +
    // entity_apply_friction(e->on_ground?...) read on_ground AFTER gravity zeroed it,
    // so grounded mobs always got AIR friction and the spider leap was dead code (it
    // could never see on_ground==true). This snapshot restores Beta's ordering.
    bool grounded_pre_gravity = e->on_ground;
	if(is_wolf)
		// Shake trigger is Entity.isWet(), not "in water" alone: in water, or
		// the floored position's block can be rained on -- a wolf
		// standing outdoors under open sky during rain is also "wet" and
		// should shake, with no water contact at all. fx/fy/fz are this same
		// tick's already-floored foot cell (computed above for the hazard
		// block). [740c583: Entity.isWet; Entity.java:576-578]
		entity_wolf_shake_tick(e, s,
			wolf_is_wet(body_in_water,
				world_can_block_be_rained_on(s, fx, fy, fz)),
			e->ai_state != AI_CHASE, grounded_pre_gravity);

    // --- moveEntityWithHeading TAIL (after moveEntity), per branch, on the
    // authoritative doubles (mob motion units S2):
    if (is_ghast) {
        // EntityFlying.moveEntityWithHeading:15-17 / :21-23 / :46-48 -- water
        // 0.8 (float widened), lava 0.5, dry the ground-friction slipperiness chain (0.91
        // airborne) on ALL three axes; a ghast has no gravity.
        double gdrag = body_in_water ? (double)0.8F
            : (lava_movement ? 0.5 : (double)ground_friction);
        md->motion[0] *= gdrag;
        md->motion[1] *= gdrag;
        md->motion[2] *= gdrag;
    } else if (is_squid) {
        // EntitySquid.onLivingUpdate, AFTER super.onLivingUpdate's bare
        // moveEntity: the pulse phase advances by rotationVelocity and wraps
        // at 2PI with a 1-in-10 velocity re-roll; the motion for the NEXT
        // move is SET from randomMotionVec * randomMotionSpeed in water, else
        // horizontal motion is zeroed and vertical motion drops by 0.08 then scales by widened
        // float 0.98. The repick rolls ran pre-move (above), Beta's order.
        e->data.monster.squid_phase += e->data.monster.squid_phase_vel;
        if (e->data.monster.squid_phase > 2.0f * (float)M_PI) {
            e->data.monster.squid_phase -= 2.0f * (float)M_PI;
            if (jrand_next_int_bound(&e->gameplay_rng, 10) == 0)
                e->data.monster.squid_phase_vel
                    = 1.0f / (jrand_next_float(&e->gameplay_rng) + 1.0f) * 0.2f;
        }
        float* sv = e->data.monster.swim_vec;
        // SQUID MOTION PROBE -- LIVE post-move at the TIGHT edge (Beta
        // overrides isInWater() to a live -0.6-shrunk test after the move;
        // combat.h derivation).
        bool squid_motion_wet = false;
        if (squid_motion_loop_covers_cell(e->pos[1])) {
            struct block_data qb;
            if (server_world_get_block(
                    block_world(s), mob_floor_d(md->pos[0]),
                    (w_coord_t)squid_motion_probe_cell_y(e->pos[1]),
                    mob_floor_d(md->pos[2]), &qb)
                && (qb.type == BLOCK_WATER_FLOW || qb.type == BLOCK_WATER_STILL))
                squid_motion_wet = true;
        }
        if (squid_motion_wet) {
            e->data.monster.swim_speed = squid_pulse_speed(
                e->data.monster.squid_phase, e->data.monster.swim_speed);
            md->motion[0] = (double)(sv[0] * e->data.monster.swim_speed);
            md->motion[1] = (double)(sv[1] * e->data.monster.swim_speed);
            md->motion[2] = (double)(sv[2] * e->data.monster.swim_speed);
        } else {
            // Beached: flop in place, plain fall (EntityWaterMob has no
            // fall() override -- a verified-faithful quirk).
            md->motion[0] = 0.0;
            md->motion[1] -= 0.08;
            md->motion[1] *= (double)0.98F;
            md->motion[2] = 0.0;
        }
    } else if (liquid_branch) {
        // EntityLiving.moveEntityWithHeading:460-466 (water) / :471-477
        // (lava): drag 0.8 (float widened) / 0.5 on all axes, then vertical motion drops by 0.02;
        // a horizontally-collided swimmer whose up-and-forward offset box is
        // free (Entity.isOffsetPositionInLiquid) climbs out: vertical motion is set to
        // 0.3 (float widened). The probe uses the pre-move posY.
        double k = body_in_water ? (double)0.8F : 0.5;
        md->motion[0] *= k;
        md->motion[1] *= k;
        md->motion[2] *= k;
        md->motion[1] -= 0.02;
        if (collision_xz
            && mob_offset_in_liquid(
                e, md, md->motion[0],
                md->motion[1] + (double)0.6F - md->pos[1] + pre_move_y,
                md->motion[2], &env))
            md->motion[1] = (double)MOB_WATER_CLIMB_OUT_VEL; /* 0.3 float, widened */
    } else {
        // DRY branch tail (:527-535): the ladder ascend -- EntitySpider's
        // isOnLadder() is isCollidedHorizontally (its wall climb), every other
        // walker needs the ladder cell -- then gravity and the ground friction,
        // with that friction read BEFORE the move (W1B-b, Beta's exact tick).
        if (collision_xz
            && (monster_id == MONSTER_SPIDER
                || (ladder_rules && mob_on_ladder(s, e))))
            md->motion[1] = 0.2;
        md->motion[1] -= 0.08;
        md->motion[1] *= (double)0.98F;
        md->motion[0] *= (double)ground_friction;
        md->motion[2] *= (double)ground_friction;
    }
    (void)ladder_branch;

    // EntityChicken.onLivingUpdate (after super.onLivingUpdate, i.e. after
    // the move and the gravity above): while airborne and falling, vertical
    // motion is scaled by 0.6 -- any branch.
    if (monster_id == MONSTER_CHICKEN && !e->on_ground && md->motion[1] < 0.0)
        md->motion[1] *= 0.6;
    mob_motion_mirror(e, md);

    // next-gaps-0924/W2 P4: EntityLiving.onLivingUpdate:641-651 -- the
    // entity-entity push, the LAST statement of onLivingUpdate. Impulses land
    // through the mutation API (entity.c), so a mob ticking later this tick
    // moves with them; this mob's own half is consumed by its next move.
    entity_living_collide_with_nearby(e, s);

    // Knockback: the impulse itself is now composed AT THE HIT (EntityLiving.
    // knockBack from attackEntityFrom, mob_motion_knockback at every site).
    // Only the steer-suppression window (carve-out #2) counts down here.
    if (e->data.monster.knockback_time > 0)
        e->data.monster.knockback_time--;

    float dx = (float)(target->x - e->pos[0]);
    float dz = (float)(target->z - e->pos[2]);
    // Vertical term for the per-attack 3D range gates (parity fix 2026-06-19): Beta's
    // skeleton/creeper/melee gates (and the slime contact reach) all run on
    // getDistanceToEntity, the FULL 3D distance. This dy uses the shared FEET-to-feet Y
    // basis (subtract PLAYER_EYE_HEIGHT) -- see the canonical Y-BASIS DEVIATION note at
    // chase_dy for why CaveWii uses feet-to-feet rather than Beta's eye-to-feet posY here.
    // The arrow AIM (which DOES use the eye -- s->player.y - 0.08, the real projectile
    // target) and the melee swing math stay as-is; only the range GATES gain dy.
    float dy = (float)(target->y - PLAYER_EYE_HEIGHT - e->pos[1]);

	// Wolf melee is a 1.5-block 3D gate plus bounding-box Y overlap. The
	// established Wii 8-tick first-contact telegraph is the same documented
	// controller overlay used by other melee mobs; reach/damage/cadence remain
	// Beta. [740c583: EntityWolf.attackEntity]
	//
	// LOS GATE (parity fix 2026-09-02, see the leap block above for the full
	// citation): the bite half of EntityWolf.attackEntity is gated by the SAME
	// canEntityBeSeen dispatch check as the leap half -- both live in the one
	// Beta method, invoked only once LOS is confirmed. Without this a wolf bit
	// straight through a wall the instant a target was within 1.5 blocks.
	if(is_wolf && e->data.monster.wolf_target_id != UINT32_MAX
	   && !wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_SITTING)) {
		struct entity* wt = entity_wolf_target(e, s);
		bool target_player = wolf_target_is_hostile_player(
			e->data.monster.wolf_target_id, e->data.monster.wolf_flags);
		float wdx = 0.0f, wdy = 0.0f, wdz = 0.0f;
		float target_y = 0.0f, target_h = 0.0f;
		if(target_player) {
			wdx = (float)target->x - e->pos[0];
			wdy = (float)(target->y - PLAYER_EYE_HEIGHT) - e->pos[1];
			wdz = (float)target->z - e->pos[2];
			target_y = (float)target->y - PLAYER_EYE_HEIGHT;
			target_h = 1.8f;
		} else if(wt) {
			wdx = wt->pos[0] - e->pos[0];
			wdy = wt->pos[1] - e->pos[1];
			wdz = wt->pos[2] - e->pos[2];
			target_y = wt->pos[1];
			target_h = mob_body_height(wt->data.monster.id);
		}
		bool wolf_can_see = target_player
			? mob_can_see_player(s, e, target)
			: (wt && mob_can_see_point(s, e, wt->pos[0],
					wt->pos[1] + mob_eye_height(wt->data.monster.id),
					wt->pos[2]));
		float wolf_distance = sqrtf(wdx * wdx + wdy * wdy + wdz * wdz);
		bool wolf_target_live = target_player ? target_alive : (wt != NULL);
		bool in_reach = wolf_target_live && wolf_can_see
			&& entity_wolf_melee_in_range(e, wolf_distance, target_y,
				target_y + target_h);
		if(!in_reach) {
			e->data.monster.attack_windup = MOB_ATTACK_WINDUP_TICKS;
		} else if(e->data.monster.attack_windup > 0) {
			e->data.monster.attack_windup--;
			if(latched && e->data.monster.attack_windup > 0)
				e->data.monster.windup_cue.phase
					= windup_was_active ? 2 : 1;
		} else {
			int raw = 0;
			bool began = entity_wolf_begin_melee(e, wolf_distance, target_y,
				target_y + target_h, &raw);
			if(began && target_player && target_damageable) {
				server_local_hurt_player_for(s, target, e);
			} else if(began && wt) {
				enum monster_damage_result wr = entity_is_wolf(wt)
					? entity_wolf_attack_from(wt, s, raw, e->id,
						false, false, false, ACTOR_IDENT_HOST)
					: entity_monster_apply_damage(wt, raw, false);
				// EntityWolf.attackEntity -> the target's attackEntityFrom, wolf as source:
				// a fresh hit knocks the victim back from the wolf
				// [EntityLiving.java:336-346 -> knockBack:392-405] (mob motion
				// units S2, Q3 -- the bite had no knockback before).
				if(wr == MONSTER_DAMAGE_FRESH) {
					struct mob_motion* wmd = &wt->data.monster.motion_d;
					mob_motion_knockback(wt, wmd,
						e->data.monster.motion_d.pos[0] - wmd->pos[0],
						e->data.monster.motion_d.pos[2] - wmd->pos[2],
						server_local_sound_rng());
					wt->data.monster.knockback_time = MOB_KNOCKBACK_TICKS;
				}
			}
		}
	}

    // Beta 1.7.3 skeleton ranged attack: fire an arrow at the player when within
    // ~10 blocks and the shot cooldown (ai_timer) is ready. The skeleton HOLDS the
    // keep-distance band (audit #17e steer above) -- it does NOT melee (bow-only,
    // excluded from the melee branch below) and does NOT close into reach; it strafes
    // at range and fires. The band nests inside this fire range so it keeps shooting
    // while it orbits. The shot aims eye-to-eye with a vertical lead (+horiz*0.2) to
    // compensate for arrow gravity, exactly as EntitySkeleton.attackEntity does.
    // Beta gates the shot on canEntityBeSeen (EntityCreature dispatch only invokes
    // attackEntity when LOS is clear) -- a skeleton will NOT fire through a wall; the
    // `can_see` LOS gate (computed above) supplies that. The Beta deviation-12 SPREAD
    // is applied below (M2: laser-accurate skeletons were both unfaithful and
    // unplayable against an IR pointer). Remaining v2 simplification (combat.h):
    // vanish-on-impact arrow.
    if (e->data.monster.id == MONSTER_GHAST) {
        // --- Beta 1.7.3 GHAST attack cycle (EntityGhast.updatePlayerAction-
        // State, decompile-verified). Single-player reduction: "targetedEntity
        // = closest player within 100" collapses to a distance test (the
        // 20-tick aggroCooldown re-targeting is a no-op with one candidate).
        // Within 64 WITH line of sight the counter climbs: at 10 the charge
        // scream (and the fire-texture bit, counter > 10), at 20 the fireball
        // launches and the counter resets to -40. Losing sight (or range)
        // only DECAYS a positive counter -- the -40 recovery still climbs.
        float gpdx = (float)target->x - e->pos[0];
        // Player mid-body: player.y is the EYE; feet + height/2 = y-1.62+0.9.
        // Ghast mid-body: feet + 2.0 [740c583: EntityGhast target-offset local].
        float gpdy = (float)(target->y - PLAYER_EYE_HEIGHT + 0.9)
            - (e->pos[1] + 2.0f);
        float gpdz = (float)target->z - e->pos[2];
        float gdist_sq = gpdx * gpdx + gpdy * gpdy + gpdz * gpdz;
        bool g_in_range = target_available && gdist_sq < 64.0f * 64.0f
            && gdist_sq < 100.0f * 100.0f; /* target range implies shoot range */
        bool g_sees = g_in_range && mob_can_see_player(s, e, target);
        if (g_sees) {
            // Beta tests the attack counter for 10 BEFORE incrementing it
            // (EntityGhast.updatePlayerActionState: charge check, then ++,
            // then the ==20 fireball check). Both sounds' pitch floats are
            // the ghast's own Random (LIVING-SOUND-RNG).
            if (e->data.monster.ghast_attack_counter == 10)
                server_local_emit_sound(
                    SOUND_MOB_GHAST_CHARGE,
                    (vec3) {e->pos[0], e->pos[1], e->pos[2]}, 10.0f,
                    mob_living_sound_pitch(e));
            e->data.monster.ghast_attack_counter++;
            if (e->data.monster.ghast_attack_counter == 20) {
                server_local_emit_sound(
                    SOUND_MOB_GHAST_FIREBALL,
                    (vec3) {e->pos[0], e->pos[1], e->pos[2]}, 10.0f,
                    mob_living_sound_pitch(e));
                // Launch: aim = the player-mid delta (unnormalized; the ctor
                // normalizes to the 0.1 acceleration). Muzzle = ghast pos +
                // horizontal look * 4, at mid-height + 0.5 [740c583: getLook
                // * 4.0, posY + height/2 + 0.5]. (Beta's 0.4-sigma gaussian
                // aim jitter is omitted -- the bow/egg documented precedent.)
                float aim[3] = {gpdx, gpdy, gpdz};
                float hlen = sqrtf(gpdx * gpdx + gpdz * gpdz);
                float lx = hlen > 1.0e-4f ? gpdx / hlen : 0.0f;
                float lz = hlen > 1.0e-4f ? gpdz / hlen : 0.0f;
                vec3 muzzle = {e->pos[0] + lx * 4.0f, e->pos[1] + 2.5f,
                               e->pos[2] + lz * 4.0f};
                server_local_spawn_fireball(muzzle, aim, e->id, s);
                e->data.monster.ghast_attack_counter = -40;
            }
        } else if (e->data.monster.ghast_attack_counter > 0) {
            e->data.monster.ghast_attack_counter--;
        }
        // The dataWatcher fire-texture bit: attackCounter > 10.
        e->data.monster.ghast_attacking
            = e->data.monster.ghast_attack_counter > 10;
    }

    if (e->data.monster.id == MONSTER_SKELETON) {
        if (e->ai_timer > 0.0f)
            e->ai_timer -= 1.0f;
        // C-DEADTARGET: EntitySkeleton.attackEntity:34-53 (the only arrow
        // spawn) is never reached for a dead target; attackTime still counts
        // down above, as EntityLiving does every tick.
        if (target_alive && can_see
            && dx * dx + dy * dy + dz * dz < SKELETON_SHOOT_RANGE_SQ
            && e->ai_timer <= 0.0f) {
            // Beta launch geometry (parity fix 2026-06-19, target corrected 2026-06-19):
            // launch from skel feet + SKELETON_ARROW_LAUNCH_DY (2.43 = skel getEyeHeight
            // 1.53 - 0.1 EntityArrow ctor + 1.0 arrow posY bump), aiming at the player's POSY
            // minus SKELETON_ARROW_TARGET_DY (0.08). Beta EntitySkeleton.attackEntity aims
            // at the target's posY plus its eye height minus 0.2 = player.posY + 0.12 - 0.2 =
            // player.posY - 0.08, and the player's posY IS the eye/camera (yOffset 1.62) ==
            // CaveWii's s->player.y. So the target is s->player.y - 0.08 (the head region),
            // NOT s->player.y - PLAYER_EYE_HEIGHT - 0.08 (which aimed 0.08 BELOW the feet --
            // the arrow buried into the ground on flat terrain). Flat-ground ddy = 1.54 -
            // 2.43 = -0.89 (Beta), not the -2.51 the feet-target gave.
            vec3 from = {e->pos[0], e->pos[1] + SKELETON_ARROW_LAUNCH_DY, e->pos[2]};
            float ddx = (float)target->x - from[0];
            float ddy = (float)(target->y - SKELETON_ARROW_TARGET_DY) - from[1];
            float ddz = (float)target->z - from[2];
            float horiz = sqrtf(ddx * ddx + ddz * ddz);
            ddy += horiz * 0.2f; // gravity-compensation lead (Beta: vertical offset + horiz*0.2)
            float n2 = ddx * ddx + ddy * ddy + ddz * ddz;
            if (n2 > 1.0e-4f) {
                // Beta setArrowHeading order: normalize, add the deviation-12
                // gaussian spread per axis, THEN scale by arrow speed.
                float n = sqrtf(n2);
                float dir[3] = {ddx / n, ddy / n, ddz / n};
                // Sequenced through named locals -- do NOT inline these as
                // sibling call arguments (adversarial review D5, 2026-08-13).
                // C11 6.5.2.2p10 leaves the evaluation order of a function
                // call's arguments unspecified. That was harmless under the
                // old stateless Box-Muller (each call was independent; only
                // WHICH axis got which value could vary). It is NOT harmless
                // now that rand_gen_gaussian caches a second value across
                // calls on the shared stream: an unspecified argument order
                // means the compiler decides which axis gets a fresh
                // rejection-loop draw versus the free cached value, AND what
                // is left cached for the next unrelated caller of
                // s->rand_src. A PC/mingw build happening to evaluate
                // left-to-right would hide this; devkitPPC is a different
                // compiler under no such obligation, so the shipped Wii
                // build could silently draw a different stream than any PC
                // test proved.
                /* I02 (board #8): CFR EntitySkeleton.java:61-68 builds
                 * a new arrow from the skeleton -- whose constructor runs
                 * setArrowHeading at speed 1.5, spread 1.0: three gaussians
                 * (EntityArrow.java:66) -- then setArrowHeading again at speed 0.6,
                 * spread 12: three more, all on the ARROW's rand. The arrow
                 * gets its id and stream at the post-tick flush, so the
                 * spread (and the spawn point, which this engine advances by
                 * one tick of the velocity) is drawn there: discard 3, use
                 * 3. The world stream is no longer touched. */
                vec3 vel = {0.0F, 0.0F, 0.0F};
                vec3 spawn = {from[0], from[1], from[2]};
                // S0: the skeleton shot plays the bow's exact formula --
                // "random.bow" volume 1.0, pitch 1 over (0.4 x a float plus 0.8) -- at the
                // skeleton. [740c583: EntitySkeleton.attackEntity:43]
                // LIVING-SOUND-RNG: the bow pitch float is the SKELETON's
                // own Random (the skeleton itself in EntitySkeleton.attackEntity), drawn
                // after the arrow is constructed (arrow-owned draws).
                server_local_emit_sound(
                    SOUND_RANDOM_BOW,
                    (vec3) {e->pos[0], e->pos[1], e->pos[2]}, 1.0F,
                    1.0F
                        / (jrand_next_float(&e->gameplay_rng) * 0.4F
                           + 0.8F));
                const int arrows_before = s->pending_arrow_count;
                server_local_spawn_arrow(spawn, vel, e->id, ITEM_ARROW,
                                         ARROW_DAMAGE, false, s);
                server_local_pending_arrow_spread(s, arrows_before, dir,
                                                  ARROW_SPEED,
                                                  SKELETON_ARROW_DEVIATION, 3,
                                                  true);
                e->ai_timer = (float)SKELETON_ARROW_COOLDOWN_TICKS;
            }
        }
    }

    if (e->data.monster.id == MONSTER_CREEPER) {
        // Beta 1.7.3 creeper: ZERO contact melee. Instead it swells when the player
        // is close and detonates at CREEPER_FUSE_TICKS. Hysteresis (combat.h
        // creeper_should_prime): ignite within 3 blocks, stay primed until 7 blocks
        // away, and DEFLATE (surviving) if the player escapes -- the fuse field
        // (-1 idle, 0..30 swelling) replaces the old player-kill countdown.
        // Beta gates the swell on canEntityBeSeen (attackEntity only runs with LOS
        // clear): a creeper behind a wall does NOT prime/detonate through it -- the
        // `can_see` LOS gate supplies that. Losing LOS deflates a primed creeper just
        // like the player walking out of range (handled by the else-deflate below).
        //
        // TWO INDEPENDENT state variables, matching Beta exactly (parity fix
        // 2026-08-31, dossier finding "Proximity-fuse hysteresis re-arm threshold
        // after a partial deflate"): `fuse` is the ramp (Beta's timeSinceIgnited,
        // -1 idle/at-rest .. CREEPER_FUSE_TICKS swelling) and `creeper_state` is the
        // hysteresis SELECTOR (Beta's DataWatcher creeperState byte 16, -1/1). The
        // selector reads whether the creeper was ACTIVELY PRIMING last tick, NOT
        // whether any ramp is still banked -- attackEntity reads the creeper
        // state and picks the 3.0-block test when it is <=0 and the
        // 7.0-block test when it is >0. The else/deflate branch LATCHES it to -1 on
        // EVERY failing tick (attackEntity's else, attackBlockedEntity, and the
        // onUpdate null-target tail all identically set the state to -1 and
        // count the ignition timer down by one), so a creeper that deflated by
        // even ONE tick must pass the tight 3-block test to re-arm, regardless of how
        // much ramp remains banked. Deriving the selector from `fuse >= 0` (any
        // residual ramp) instead of this persisted flag let a creeper re-arm on the
        // loose 7-block test after only a partial retreat -- fixed by keeping the two
        // variables separate, as Beta does. [740c583: EntityCreeper.attackEntity /
        // .attackBlockedEntity / .onUpdate]
        bool was_primed = e->data.monster.creeper_state > 0;
        bool ready_to_explode = e->data.monster.fuse >= CREEPER_FUSE_TICKS;
        // C-DEADTARGET: with a dead target attackEntity never runs. On the
        // re-acquire tick (latched on the corpse) neither attackEntity nor
        // attackBlockedEntity nor the null-target tail touches the fuse; on the
        // drop tick (no target) EntityCreeper.onUpdate:63-69 deflates it.
        // An already-terminal fuse (admission retry) still detonates: Beta's
        // createExplosion ran inside the attackEntity that reached it.
        bool creeper_hold = !target_alive && latched;
        if (!ready_to_explode && target_alive && can_see
            && creeper_should_prime(dx * dx + dy * dy + dz * dz, was_primed,
                                    MOB_FOLLOW_RANGE_SQ)) {
            if (e->data.monster.fuse < 0) {
                e->data.monster.fuse = 0; // begin swelling
                // S0: "random.fuse" 1.0F pitch 0.5F exactly on the ignition
                // EDGE (Beta's timeSinceIgnited == 0 gate: a re-prime from a
                // partial deflate replays nothing, matching the fuse>=0
                // else-branch below). [740c583: EntityCreeper.attackEntity]
                server_local_emit_sound(
                    SOUND_RANDOM_FUSE,
                    (vec3) {e->pos[0], e->pos[1], e->pos[2]}, 1.0F, 0.5F);
            }
            e->data.monster.creeper_state = 1; // [740c583: setCreeperState with 1]
            ready_to_explode = ++e->data.monster.fuse >= CREEPER_FUSE_TICKS;
        } else if (!ready_to_explode && !creeper_hold) {
            // [740c583: creeper state set to -1, ignition timer decremented] -- the state latch
            // runs on EVERY failing tick, unconditionally, even one where the ramp is
            // already resting at its -1 floor (matching Beta's clamped-0 floor).
            e->data.monster.creeper_state = -1;
            if (e->data.monster.fuse >= 0)
                e->data.monster.fuse--; // player escaped -> deflate toward idle (-1)
        }
        if (ready_to_explode) {
            vec3 boom = {e->pos[0], e->pos[1], e->pos[2]};
            /* Mass creepers share the Wii explosion budget with primed TNT. If
             * admission is full, this entity remains the durable owner at the
             * terminal fuse and retries next update regardless of later LOS/range;
             * Beta's createExplosion + setDead pair must be one operation. */
            // Beta EntityCreeper.attackEntity explodes at strength 6.0F when powered
            // and 3.0F otherwise [740c583] -- a lightning-charged creeper
            // (weather slice: charged set by EntityCreeper.onStruckByLightning) detonates
            // at double strength.
            float power = e->data.monster.charged ? CREEPER_EXPLOSION_POWER_CHARGED
                                                    : CREEPER_EXPLOSION_POWER;
            // createExplosion with the creeper as exploder: the creeper is Beta's one non-null exploder --
            // excluded from its own blast's entity pass, and the source of the
            // attackEntityFrom knockBack every hit EntityLiving gets [740c583
            // EntityCreeper.attackEntity:101-103, Explosion.doExplosionA:84,100].
            if (server_world_try_explode_by((struct server_world*)e->world, e, boom, power)) {
                // AUDIT #18 item 3 + parity fix 2026-06-19: Beta EntityCreeper.explode() is
                // createExplosion + setEntityDead DIRECTLY -- NOT the onDeath/deathTime path
                // -- so a creeper that blows ITSELF up gets NO 20-tick flop AND drops
                // NOTHING (setEntityDead never calls dropFewItems). Only a creeper KILLED by
                // the player/arrow drops gunpowder, on the normal death edge in mob_die_tick.
                // The old code dropped a random 0-2 gunpowder stack here, which Beta
                // does not; removing the rand draw too matches Beta (explode() rolls no
                // entity rand at the blast).
                e->delay_destroy = 0;
                // HARDEN AUDIT #5: reap THIS tick (mirroring the death-flop reap's
                // `return true`). Without this the function fell through to the trailing
                // CRPC_ENTITY_MOVE below, sending a contradictory MOVE (swelling=true)
                // for a creeper that just EXPLODED and is being DESTROYed the same tick.
                // Returning true makes the main entity loop send only EXPLODE + DESTROY.
                return true;
            }
            e->data.monster.fuse = CREEPER_FUSE_TICKS;
			e->data.monster.explosion_pending = true;
        }
    } else if (e->data.monster.id != MONSTER_SKELETON
               && !monster_is_slime(e->data.monster.id)
               && !monster_is_passive(e->data.monster.id)
               && mob_melee_damage(e->data.monster.id) > 0
               // A NEUTRAL pig zombie never swings: Beta only dispatches
               // attackEntity against playerToAttack, which stays null while
               // angerLevel == 0 (the ghast is excluded via its 0 damage).
               && (e->data.monster.id != MONSTER_PIG_ZOMBIE
                   || e->data.monster.anger_level > 0)) {
        // Beta 1.7.3 skeletons are BOW-ONLY: EntitySkeleton.attackEntity fully
        // overrides EntityMob and never calls super, so a skeleton has NO contact
        // melee even point-blank -- it just keeps firing arrows. Excluding it here
        // (and the creeper above, which only detonates) leaves the zombie + spider on
        // the shared EntityMob melee path. SLIMES are excluded too (parity fix
        // 2026-06-19): a slime has no attackEntity/attackTime in Beta -- it hurts via
        // onCollideWithPlayer (collision contact), handled in its own branch below, NOT
        // through this 2.0-reach + 20-tick-cooldown + windup path.
        // !passive (M2 final units): this branch previously admitted EVERY
        // non-skeleton id -- a passive animal standing in reach with LOS for the
        // 8-tick windup would genuinely swing at the player (latent bug; Beta
        // animals have no attackEntity at all). The squid is covered too.
        // (mob_melee_damage(SKELETON)=16 stays for crash-safety but is now
        // unreachable in normal play.)
        // Beta 1.7.3 melee (zombie + spider): count attackTime
        // down, and when it hits 0 with the player in horizontal reach + vertical
        // overlap, swing -> server_local_hurt_player (damage + knockback, i-frame
        // gated) and reset attackTime to MOB_ATTACK_COOLDOWN_TICKS. The first swing
        // of an engagement is telegraphed by MOB_ATTACK_WINDUP_TICKS (Wii
        // playability); sustained in-reach swings keep Beta's bare 1.0s cadence.
        // Beta gates the swing on canEntityBeSeen (EntityCreature only dispatches
        // attackEntity with LOS clear): a mob on the far side of a wall does NOT hit
        // the player through it -- `can_see` (computed above) is ANDed into the reach
        // test, so a blocked mob re-arms its windup like an out-of-reach one.
        if (e->data.monster.attack_time > 0)
            e->data.monster.attack_time--;

        // Vertical melee window uses mob_melee_box_height (parity fix 2026-06-19): the
        // non-biped mobs keep their REAL per-id box (spider 0.9) so a squat spider bites
        // only within its 0.9-tall body, while the biped hostiles use the inherited 1.8 EntityMob
        // box -- the height the melee gate's Beta
        // citation requires. See mob_melee_box_height in combat.h for the why.
        // C-DEADTARGET: EntityMob.attackEntity:49-55 never runs on a dead
        // target, so it neither swings nor re-arms attackTime (the windup
        // telegraph resets as for any out-of-reach tick).
        bool in_reach = target_alive && can_see && melee_in_reach_sq(dx, dy, dz)
            && melee_y_overlap((float)target->y - PLAYER_EYE_HEIGHT, e->pos[1],
                               mob_melee_box_height(e->data.monster.id));
        if (!in_reach) {
            e->data.monster.attack_windup = MOB_ATTACK_WINDUP_TICKS;
        } else if (e->data.monster.attack_time == 0) {
            if (e->data.monster.attack_windup > 0) {
                e->data.monster.attack_windup--;
                if(latched && e->data.monster.attack_windup > 0
                   && (e->data.monster.id == MONSTER_ZOMBIE
                       || e->data.monster.id == MONSTER_PIG_ZOMBIE
                       || e->data.monster.id == MONSTER_SPIDER))
                    e->data.monster.windup_cue.phase
                        = windup_was_active ? 2 : 1;
            } else {
                if(target_damageable)
                    server_local_hurt_player_for(s, target, e);
                e->data.monster.attack_time = MOB_ATTACK_COOLDOWN_TICKS;
            }
        }
        // else: in reach but on cooldown -- hold windup (0 after the first hit).
    } else if (monster_is_slime(e->data.monster.id)
               && mob_melee_damage(e->data.monster.id) > 0) {
        // Beta EntitySlime.onCollideWithPlayer (NOT EntityMob.attackEntity -- a slime has
        // no attackEntity / attackTime). A size>1 slime that CAN SEE the player and is
        // within 0.6*size (3D center-to-center, getDistanceToEntity) hurts on CONTACT:
        // NO attack cooldown and NO windup telegraph. Re-hits are bounded ONLY by the
        // player's i-frames (PLAYER_HURT_RESIST_TICKS, ~10 ticks) -- roughly DOUBLE the
        // effective DPS of the 20-tick EntityMob swing the harmful slimes were wrongly
        // routed through before this 2026-06-19 parity fix. damage>0 already excludes the
        // harmless small slime (size 1). Distance is feet-to-feet 3D (dx/dy/dz above);
        // 0.6*size gives medium 1.2, big 2.4 blocks. server_local_hurt_player applies the
        // mob-id damage (16/32 = size*8) + knockback, i-frame gated inside (Beta routes
        // the slime hit through the same attackEntityFrom gate).
        float reach = 0.6f * (float)slime_beta_size(e->data.monster.id);
        if (can_see && dx * dx + dy * dy + dz * dz < reach * reach
            && target_damageable)
            server_local_hurt_player_for(s, target, e);
    }

    // RIDER FOLLOW (saddle-slice review fix, finding 1) [740c583: Entity.updateRidden
    // -> updateRiderPosition]: force the rider onto the mount EVERY tick. Placed here,
    // at the tail of the PIG's own tick, so e->pos is already final for this tick --
    // matches Beta's own order exactly (World.updateEntityWithOptionalForce calls the
    // rider's update recursively from WITHIN the mount's own dispatch, immediately
    // after the mount's position is set). Passive position-follow ONLY: EntityPig has
    // no moveEntityWithHeading override, so no rider input is read here (see the S4
    // negative-control test) -- this is not steering. Server-authoritative: this runs
    // AFTER server_local_update's svin_process_messages drains any same-tick
    // SRPC_PLAYER_POS (RPC-then-entity-pass order), so it unconditionally overwrites
    // whatever position an ordinary walk input tried to set -- a saddled mount cannot
    // be walked away from (finding 1's "gate free movement"). The lightweight
    // CRPC_PLAYER_POS/PLAYER_CORRECTION_POSITION_ONLY channel (the piston-push/bed
    // precedent) is a hard snap, not the minecart's smoothed camera_correction -- see
    // entity.h's pig_ridden comment for why that is not a docs/deviations.md gap.
    if (e->data.monster.id == MONSTER_PIG && e->data.monster.pig_ridden) {
        /* S1-a2: resolve the RIDER identity. A rider whose session ended (guest
         * leave, release, rejoin under a new generation) is force-DISMOUNTED --
         * Beta drops riddenByEntity the moment the rider stops existing
         * [CFR server decompile: World.java:977 updateEntity keeps a rider
         * only while that rider is alive and still names this entity as
         * its mount; same check in EntityBoat.java:269 /
         * EntityMinecart.java:449] -- instead of the mount snapping a
         * departed body, or worse, snapping the HOST onto a pig the guest was
         * riding. Host rider: the identity is the zero value, resolve returns
         * &s->player, and every line below is the one it replaced. */
        /* G8-VEHICLE-SEAMS-01: the relation must also name the rider wire id
         * and the pig world; a wrong-world or reused-id relation is dropped
         * like a departed rider. */
        struct server_player* rider = server_local_vehicle_rider(s, e,
            e->data.monster.pig_rider_actor, e->data.monster.pig_rider_id);
        if (!rider) {
            server_local_pig_clear_rider(e);
            return false;
        }
        rider->vel_x = 0.0f;
        rider->vel_y = 0.0f;
        rider->vel_z = 0.0f;
        double rx = (double)e->pos[0];
        double ry = (double)e->pos[1] + (double)PIG_MOUNTED_Y_OFFSET
            + (double)PLAYER_EYE_HEIGHT;
        double rz = (double)e->pos[2];
        bool seat_moved = rx != rider->x || ry != rider->y || rz != rider->z;
        if (seat_moved) {
            rider->x = rx;
            rider->y = ry;
            rider->z = rz;
        }
        if (rider != &s->player) {
            /* Guest rider: its own generation-tagged private correction. A
             * refused send (token pool full) is retried on the next ridden
             * tick even if the seat has not moved since. */
            if (seat_moved || e->data.monster.pig_seat_correction_pending)
                e->data.monster.pig_seat_correction_pending
                    = !server_local_guest_correct_position_for(s,
                        e->data.monster.pig_rider_actor.generation,
                        PLAYER_CORRECTION_POSITION_ONLY
                        | PLAYER_CORRECTION_ZERO_VEL_X
                        | PLAYER_CORRECTION_ZERO_VEL_Y
                        | PLAYER_CORRECTION_ZERO_VEL_Z);
        } else if (seat_moved) {
            server_local_next_player_correction(s);
            clin_rpc_send(&(struct client_rpc){
                .type = CRPC_PLAYER_POS,
                .payload.player_pos = {
                    .position = {s->player.x, s->player.y, s->player.z},
                    .rotation = {s->player.rx, s->player.ry},
                    .correction_seq = s->player.correction_seq,
                    .correction_flags = PLAYER_CORRECTION_POSITION_ONLY
                        | PLAYER_CORRECTION_ZERO_VEL_X
                        | PLAYER_CORRECTION_ZERO_VEL_Y
                        | PLAYER_CORRECTION_ZERO_VEL_Z,
                },
            });
        }
    }

    // Spider jockey RIDER FOLLOW -- the SAME per-tick hard-snap pattern as the
    // pig-rider block above, generalized to a MOB-ON-MOB mount [740c583:
    // Entity.updateRidden -> updateRiderPosition, the identical call the pig
    // precedent cites, but through the spider's OVERRIDDEN
    // getMountedYOffset -- see SPIDER_JOCKEY_MOUNTED_Y_OFFSET's citation in
    // combat.h]. Placed at the tail of the SPIDER's (the mount's) own tick so
    // e->pos is already final for this tick, matching the pig precedent's
    // ordering rationale. Zeroes the rider's velocity (Beta's updateRidden:
    // all three velocity components set to zero on the RIDER
    // before its own tick runs) and hard-snaps its position via the same
    // generic entity teleport primitive entity_wolf_try_teleport_to_owner
    // already uses for a cross-entity position write.
    //
    // Documented, deliberate scope cut: the rider's own AI/pathing this tick
    // is NOT separately suppressed. Fully replicating Beta's exact
    // zero-motion-THEN-tick-THEN-snap ridden-entity order for a SECOND
    // independent AI-driven entity, across this engine's dict-iteration-
    // order-dependent single-pass tick (the rider may tick before or after
    // its mount within the same server tick, exactly the existing
    // nondeterminism entity_wolf_attack_from's pack-alert scan already
    // tolerates), is disproportionate engineering cost for a mount that only
    // exists on 1% of spider spawns. The snap below always wins within one
    // tick's latency regardless of iteration order, so the rider can never
    // visibly drift from its mount for more than a single frame.
    if (e->data.monster.id == MONSTER_SPIDER
        && e->data.monster.jockey_rider_id != UINT32_MAX) {
        // A2-E: the rider must resolve inside the spider's own live world; a
        // rider of another world / a retired generation counts as gone.
        struct entity* rider
            = server_local_entity_ref(s, e, e->data.monster.jockey_rider_id);
        if (!rider || rider->delay_destroy == 0) {
            // Rider gone without going through the reap path above (e.g. a
            // chunk unload freed it) -- release the mount's rider slot.
            e->data.monster.jockey_rider_id = UINT32_MAX;
        } else {
            // Mob motion units S2: Entity.updateRidden zeroes the rider's
            // motion and updateRiderPosition setPosition()s it on the mount's
            // DOUBLE position -- through the mutation API, prevPos too (the
            // teleport-seed shape the float path used).
            struct mob_motion* rmd = &rider->data.monster.motion_d;
            const struct mob_motion* mmd = &e->data.monster.motion_d;
            vec3 ride_pos = {e->pos[0],
                e->pos[1] + SPIDER_JOCKEY_MOUNTED_Y_OFFSET, e->pos[2]};
            rider->teleport(rider, ride_pos);
            mob_motion_seed(rider, rmd, mmd->pos[0],
                            mmd->pos[1] + (double)SPIDER_JOCKEY_MOUNTED_Y_OFFSET,
                            mmd->pos[2], 0.0, 0.0, 0.0);
        }
    }

    // T1 PERF (change-gate the trailing per-tick CRPC_ENTITY_MOVE): the TODO
    // this replaces flagged that EVERY living mob sent this 784-byte
    // client_rpc through the 16-slot BLOCKING client inbox every single
    // tick, even a mob standing perfectly still with nothing networked
    // about it different from the tick before -- N idle mobs cost N
    // guaranteed sends/tick regardless of whether the client has anything
    // new to draw. This is pure wire-format bookkeeping: it changes zero
    // simulated bytes (e->pos and every monster.* field below are computed
    // exactly as before, by the exact same code, in the exact same order --
    // only whether THIS network message goes out is gated), so it needs no
    // decompile citation and no golden test; the correctness bar is that the
    // CLIENT's resulting entity state is bit-for-bit identical to the
    // unconditional-send baseline, which is what
    // test_entity_move_change_gate_equiv proves by replaying an identical
    // multi-mob multi-tick script through both an unconditional-send build
    // (via the raw payload comparison the test itself performs) and this
    // gated one and diffing every CRPC_ENTITY_MOVE the client actually
    // receives.
    //
    // Build the payload once, compare it field-by-field (not memcmp -- the
    // struct has no padding today, but a byte-comparison of a struct is a
    // silent trap the moment one is added, so this compares only named
    // fields, matching CODESTYLE's "no UB shortcuts") against the copy last
    // actually sent for this entity, and skip the send when identical.
    //
    // RESEND CASES:
    //  - Spawn / first tick: last_sent_move_valid is set false by the constructor (allocation is NOT zeroed), so
    //    the very first tick always sends, same as the unconditional code.
    //  - Teleport (wolf owner-teleport, minecart derail, etc.): a teleport
    //    IS a position change, already caught by the pos comparison below --
    //    no special case needed.
    //  - Client reconnect / world-load resync: server_local.c's world-load
    //    path re-sends a fresh CRPC_SPAWN_MONSTER per entity (its own
    //    .pos/.fleece_color/.sheared/.saddled/.wolf_*/.charged fields), but
    //    that event is NOT observable from here without touching
    //    source/network/ (no flag on struct server_local this file may read
    //    marks "a client just (re)synced"), and a SPAWN does not carry
    //    on_fire/swelling/hurt/fuse/ghast_attacking/pig_ridden/pig_rider_id
    //    -- so a client that resyncs while, say, a creeper is already
    //    swelling would see it as NOT swelling until this cache next detects
    //    a real change, which could in principle be never. Per the lane
    //    brief's own fallback: gate conservatively with a bounded periodic
    //    resend -- every MOB_MOVE_RESEND_INTERVAL_TICKS ticks this sends
    //    unconditionally even when nothing changed, which self-heals any
    //    missed resync within one second at 20 TPS. STOP note: closing this
    //    gap exactly (instead of bounding it) would need a hook this lane
    //    cannot reach -- e.g. a `bool just_resynced` flag server_local.c
    //    sets on its world-load/reconnect path and clears after one tick,
    //    which entity_monster.c could read to force an immediate resend
    //    instead of waiting up to 20 ticks.
    struct monster_move_snapshot fresh = {
        .windup_cue = e->data.monster.windup_cue,
        .pos = {e->pos[0], e->pos[1], e->pos[2]},
        .on_fire = monster_published_on_fire(e),
        .swelling = e->data.monster.fuse >= 0,
        .hurt = hurt_flash_active(e->data.monster.hurt_time),
        .fuse = (int8_t)e->data.monster.fuse,
        .sheared = e->data.monster.sheared,
        .saddled = e->data.monster.saddled,
        .fleece_color = e->data.monster.fleece_color,
        .ghast_attacking = e->data.monster.ghast_attacking,
        .wolf_flags = e->data.monster.wolf_flags,
        .wolf_health = e->data.monster.wolf_watched_health,
        .wolf_shake_armed = e->data.monster.wolf_shake_armed,
        .wolf_shake_active = e->data.monster.wolf_shake_active,
        .wolf_shake_time = e->data.monster.wolf_shake_time,
        .wolf_prev_shake_time = e->data.monster.wolf_prev_shake_time,
        .wolf_head_rotation = e->data.monster.wolf_head_rotation,
        .wolf_head_rotation_old = e->data.monster.wolf_head_rotation_old,
        .charged = e->data.monster.charged,
        .pig_ridden = e->data.monster.pig_ridden,
        .pig_rider_id = e->data.monster.pig_rider_id,
        .pig_zombie_angry = e->data.monster.id == MONSTER_PIG_ZOMBIE
            && e->data.monster.anger_level > 0,
    };
    const struct monster_move_snapshot* prev = &e->data.monster.last_sent_move;
    bool unchanged = e->data.monster.last_sent_move_valid
        && fresh.pos[0] == prev->pos[0] && fresh.pos[1] == prev->pos[1]
        && fresh.pos[2] == prev->pos[2]
        && fresh.on_fire == prev->on_fire
        && fresh.swelling == prev->swelling
        && fresh.hurt == prev->hurt
        && fresh.fuse == prev->fuse
        && fresh.sheared == prev->sheared
        && fresh.saddled == prev->saddled
        && fresh.fleece_color == prev->fleece_color
        && fresh.ghast_attacking == prev->ghast_attacking
        && fresh.wolf_flags == prev->wolf_flags
        && fresh.wolf_health == prev->wolf_health
        && fresh.wolf_shake_armed == prev->wolf_shake_armed
        && fresh.wolf_shake_active == prev->wolf_shake_active
        && fresh.wolf_shake_time == prev->wolf_shake_time
        && fresh.wolf_prev_shake_time == prev->wolf_prev_shake_time
        && fresh.wolf_head_rotation == prev->wolf_head_rotation
        && fresh.wolf_head_rotation_old == prev->wolf_head_rotation_old
        && fresh.charged == prev->charged
        && fresh.pig_ridden == prev->pig_ridden
        && fresh.pig_rider_id == prev->pig_rider_id
        && fresh.pig_zombie_angry == prev->pig_zombie_angry;
    bool resend_due
        = e->data.monster.ticks_since_move_send >= MOB_MOVE_RESEND_INTERVAL_TICKS;
    if(unchanged && !resend_due) {
        if(fresh.windup_cue.phase != prev->windup_cue.phase)
            monster_publish_windup_cue(e, s);
        e->data.monster.ticks_since_move_send++;
        return false;
    }

    server_local_send_entity(s, e, (struct client_rpc){
        .type = CRPC_ENTITY_MOVE,
        .payload.entity_move.entity_id = e->id,
        .payload.entity_move.windup_cue = fresh.windup_cue,
        .payload.entity_move.pos = {fresh.pos[0], fresh.pos[1], fresh.pos[2]},
        .payload.entity_move.on_fire = fresh.on_fire,
        .payload.entity_move.swelling = fresh.swelling,
        // audit #17f red hurt-flash: persisted Beta hurtTime is server-side, so
        // network it as a flag for the client's hurt-flash overlay.
        .payload.entity_move.hurt = fresh.hurt,
        // Networked so the client can drive the per-instance swell flash off THIS
        // creeper's real fuse phase (-1 idle .. 30), not a shared free-running clock.
        .payload.entity_move.fuse = fresh.fuse,
        // M4 S2: sheep sheared flag rides every MOVE so a live shear un-wools the
        // render within one tick (the on_fire/swelling precedent). fleece_color
        // rides with it so a live DYE recolors within one tick too (review F1:
        // it was SPAWN-only, so the recolor never reached a live client).
        .payload.entity_move.sheared = fresh.sheared,
        .payload.entity_move.saddled = fresh.saddled,
        .payload.entity_move.ghast_attacking = fresh.ghast_attacking,
        .payload.entity_move.fleece_color = fresh.fleece_color,
		.payload.entity_move.wolf_flags = fresh.wolf_flags,
		.payload.entity_move.wolf_health = fresh.wolf_health,
		.payload.entity_move.wolf_shake_armed = fresh.wolf_shake_armed,
		.payload.entity_move.wolf_shake_active = fresh.wolf_shake_active,
		.payload.entity_move.wolf_shake_time = fresh.wolf_shake_time,
		.payload.entity_move.wolf_prev_shake_time = fresh.wolf_prev_shake_time,
		.payload.entity_move.wolf_head_rotation = fresh.wolf_head_rotation,
		.payload.entity_move.wolf_head_rotation_old
			= fresh.wolf_head_rotation_old,
        // audit #18 item 3: a living mob is at death_time 0 (no flop). The dying-state
        // block at the top of the tick sends the non-zero death_time on the flop ticks.
        .payload.entity_move.death_time = 0,
        .payload.entity_move.charged = fresh.charged,
        .payload.entity_move.pig_ridden = fresh.pig_ridden,
        .payload.entity_move.pig_rider_id = fresh.pig_rider_id,
        .payload.entity_move.pig_zombie_angry = fresh.pig_zombie_angry,
    });
    e->data.monster.last_sent_move = fresh;
    e->data.monster.last_sent_move_valid = true;
    e->data.monster.ticks_since_move_send = 0;

    return false;
}

static bool entity_server_tick(struct entity* e, struct server_local* s) {
    // All hostile mobs share the generic wander/death tick. Per-type AI
    // (skeleton ranged, spider climb/leap, zombie melee) is the living-world
    // milestone's job; this slice gives every type the same baseline behaviour.
    if (e->type != ENTITY_MONSTER)
        return false;
    // Mob motion units (mob_motion.h, MOB_MOTION_ACTIVE): bracket the tick so
    // a writer that bypassed the mutation API since the last tick is counted
    // and its float write discarded (the doubles are authoritative), and the
    // float mirror is refreshed from the doubles after this tick.
    mob_motion_tick_begin(e, &e->data.monster.motion_d);
    bool remove = server_tick_monster(e, s);
    /* Some death edges have no MOVE until the following tick. Retire a
     * previously advertised pose immediately, touching only cue metadata.
     * The cached phase also prevents resending this cancellation twice. */
    if(!remove)
        monster_retire_dead_windup_cue(e, s);
    mob_motion_tick_end(e, &e->data.monster.motion_d);
    return remove;
}

static bool entity_client_tick(struct entity* e) {
    if (e->delay_destroy == 0) {
        return true;
    }

    // Every monster type uses the generic interpolation tick. They currently
    // render with the creeper model as a placeholder, so the placeholder body
    // is still oriented/animated correctly until per-type models land.
    if (e->type == ENTITY_MONSTER) {
        return client_tick_monster(e);
    }
    return false;
}

// Render creeper
static void entity_render(struct entity* e, mat4 view, float td) {
    assert(e);

    vec3 p;
    glm_vec3_lerp(e->pos_old, e->pos, td, p);

    vec3 delta;
    glm_vec3_sub(e->pos, e->pos_old, delta);
    // body_yaw + head_yaw are both ABSOLUTE world yaws (head_yaw = movement dir,
    // body_yaw lags toward it). Pass them through as-is -- the OLD code set
    // headYaw = bodyYaw + head_yaw (double-counting) AND passed them in swapped
    // order to render_entity_* (whose signature is (view, headYawDeg, bodyYawDeg)),
    // so every mob's body spun ~2x and its head never aligned.
    float bodyYaw = e->data.monster.body_yaw;
    float headYaw = e->data.monster.head_yaw;

    // update light
    struct block_data blk;
    entity_get_block(e, floorf(e->pos[0]),
        underwater_overlay_sample_y(e->pos[1], e->height, e->y_offset),
        floorf(e->pos[2]), &blk);
    const uint8_t model_light = (uint8_t)((blk.torch_light << 4) | blk.sky_light);
    render_entity_update_light(model_light);
    render_entity_set_limb_swing(e->data.monster.limb_swing); // gait amplitude

    mat4 model, mv;
    glm_translate_make(model, p);

    // AUDIT #18 ITEM 3 / next-gaps-0924/G8: Beta death tilt (RenderLiving.rotateCorpse).
    // While the mob is dying (networked death_time > 0) the whole model rolls about the
    // BODY's forward axis through the feet by min(1, sqrt((deathTime + td - 1)/20 * 1.6))
    // * getDeathMaxRotation -- 90, spider 180 (RenderSpider), squid none (RenderSquid's
    // rotateCorpse override has no deathTime term). Applied to the SHARED model matrix
    // (T(p) * tilt) so every submodel the render fn builds on top of mv, and the creeper/
    // slime/giant scales below, tilt together -- Beta also scales after rotateCorpse
    // (preRenderCallback). A living mob (death_time 0) gets roll 0 -> matrix untouched.
    if(e->type == ENTITY_MONSTER && e->data.monster.death_time > 0) {
        render_entity_death_tilt(model, glm_deg(bodyYaw),
            mob_death_roll_deg_for(e->data.monster.id, e->data.monster.death_time, td));
    }

    // AUDIT #18 ITEM 2: Beta creeper swell INFLATION. A primed creeper bulges WIDER than
    // taller as it nears detonation (RenderCreeper.updateCreeperScale, flash^4 curve:
    // ~1.0x for most of the fuse, hard inflate in the final ~15%, max 1.4x XZ / 1.1x Y).
    // Scale the SHARED model about the feet (post-multiplied T(p)*S) so every creeper
    // submodel inflates together. creeper_swell_scale_* return 1.0 for an idle/non-primed
    // creeper (fuse < 0), so a calm creeper is unscaled; only the creeper id path drives a
    // fuse, so guarding on the id keeps biped mobs untouched. The white swell tint (below)
    // is driven by the SAME fuse phase, so flash + inflation stay in lockstep.
    // POSITIVE id test (only MONSTER_CREEPER swells): with just the 4 hostiles today this is
    // identical to the old `!= ZOMBIE && != SKELETON && != SPIDER` blacklist, but it makes
    // any future id (the passive animals) default to NON-swelling instead of inflating like
    // a creeper.
    if(e->type == ENTITY_MONSTER && e->data.monster.id == MONSTER_CREEPER) {
        float sxz = creeper_swell_scale_xz(e->data.monster.fuse, td);
        float sy = creeper_swell_scale_y(e->data.monster.fuse, td);
        glm_scale(model, (vec3){sxz, sy, sxz});
    }

    // SLIME size scale (M2 final units): render_entity_slime draws the size-1
    // model; the Beta size (1/2/4) scales the SHARED model matrix about the
    // feet, exactly the creeper-swell pattern above (Beta RenderSlime scales
    // by getSlimeSize in scaleSlime). The squish squash-and-stretch component
    // is deferred with its un-networked state (see render_entity_slime).
    if(e->type == ENTITY_MONSTER && monster_is_slime(e->data.monster.id)) {
        float ssz = (float)slime_beta_size(e->data.monster.id);
        glm_scale(model, (vec3){ssz, ssz, ssz});
    }

    glm_mat4_mul(view, model, mv);

    // audit #17f emissive overlay: bracket the WHOLE per-mob draw in one tint so the
    // red hurt-flash (all mobs) and the white creeper swell-flash share a single apply
    // + a single mandatory reset (critical on GX: a leftover TEV stage risks a FIFO
    // hang). Precedence: the red hurt-flash wins over the white swell while the mob is
    // in its post-hit i-frame window (a creeper you just hit flashes red, not white),
    // matching Beta's RendererLivingEntity hurt overlay being drawn over the model.
    // amount==0 (default: not hurt, idle creeper, or off-pulse) == the model as today.
    float tint_r = 0.0f, tint_g = 0.0f, tint_b = 0.0f, tint_amount = 0.0f;
    // G8b: hurt flag OR the dying deathTime flop (Beta RenderLiving.java:67-75).
    if(entity_living_red_tint(e)) {
        // R1b: Beta's (brightness,0,0) at 0.4, brightness = the one this model is lit by
        // (model_light's lightmap entry, combat.h hurt_wash_for_brightness).
        struct hurt_wash w = hurt_wash_for_brightness(light_brightness_current(model_light));
        tint_r = w.r;
        tint_g = w.g;
        tint_b = w.b;
        tint_amount = w.amount;
    } else if(e->data.monster.id == MONSTER_CREEPER) {
        // creeper only: pulse the white swell wash off THIS creeper's fuse. (Positive id
        // test so future passive-animal ids don't white-flash -- see the swell-scale note.)
        tint_r = CREEPER_FLASH_TINT_R;
        tint_g = CREEPER_FLASH_TINT_G;
        tint_b = CREEPER_FLASH_TINT_B;
        tint_amount = creeper_flash_amount(e->data.monster.fuse, td);
    } else if(e->data.monster.id == MONSTER_CREEPER && e->data.monster.charged) {
        // weather slice minimal render pass (see combat.h CHARGED_TINT_*): a
        // persistent low blue wash for a lightning-charged creeper, below
        // hurt/swell precedence.
        tint_r = CHARGED_TINT_R;
        tint_g = CHARGED_TINT_G;
        tint_b = CHARGED_TINT_B;
        tint_amount = CHARGED_TINT_AMOUNT;
    }
    gfx_entity_tint(tint_r, tint_g, tint_b, tint_amount);

    // Per-type model: zombie + skeleton are the ModelBiped; the spider is its own
    // quadruped; the creeper (+ any unknown id) is the creeper, which pulses its
    // swell flash while primed (e->swelling).
    float windup_degrees = mob_windup_cue_degrees(
        e->data.monster.windup_cue, e->data.monster.id,
        e->health > 0 && e->data.monster.death_time == 0
            && config_mob_attack_windup_cue(&gstate.config_user));
    switch(e->data.monster.id) {
    case MONSTER_ZOMBIE:
        render_entity_zombie_cue(mv, glm_deg(headYaw), glm_deg(bodyYaw),
                             e->data.monster.frame, windup_degrees);
        break;
    case MONSTER_GIANT: {
        // Beta RenderGiantZombie: the plain zombie model with preRenderScale
        // a uniform 6x GL scale [740c583 RenderGiantZombie.preRenderScale] --
        // transcribed as a 6x scale of the entity model-view before the
        // shared biped draw. Animation frame/yaw pass through unchanged.
        mat4 giant_mv;
        glm_mat4_copy(mv, giant_mv);
        glm_scale(giant_mv, (vec3) {6.0f, 6.0f, 6.0f});
        render_entity_zombie(giant_mv, glm_deg(headYaw), glm_deg(bodyYaw),
                             e->data.monster.frame);
        break;
    }
    case MONSTER_SKELETON:
        render_entity_skeleton(mv, glm_deg(headYaw), glm_deg(bodyYaw),
                               e->data.monster.frame);
        break;
    case MONSTER_SPIDER:
        render_entity_spider_cue(mv, glm_deg(headYaw), glm_deg(bodyYaw),
                             e->data.monster.frame, windup_degrees);
        break;
	case MONSTER_PIG:
        render_entity_pig(mv, glm_deg(headYaw), glm_deg(bodyYaw),
                          e->data.monster.frame);
		break;
	case MONSTER_WOLF:
		render_entity_wolf_cue(mv, glm_deg(headYaw), glm_deg(bodyYaw),
			e->data.monster.frame, e->data.monster.wolf_flags,
			e->data.monster.wolf_watched_health,
			e->data.monster.wolf_shake_active,
			e->data.monster.wolf_prev_shake_time,
			e->data.monster.wolf_shake_time,
			e->data.monster.wolf_head_rotation_old,
			e->data.monster.wolf_head_rotation, td, windup_degrees);
		break;
    case MONSTER_COW:
        render_entity_cow(mv, glm_deg(headYaw), glm_deg(bodyYaw),
                          e->data.monster.frame);
        break;
    case MONSTER_CHICKEN: {
        // Beta wing-flap gate: the wings fold flat on the ground and flap when airborne. Beta
        // drives this from destPos (0 grounded, 1 falling); approximate it from the chicken's
        // interpolated vertical motion this frame (delta[1] < 0 => falling). Clamp to [0,1].
        float fall = -delta[1] * 8.0f;
        float wingDestPos = fall < 0.0f ? 0.0f : (fall > 1.0f ? 1.0f : fall);
        render_entity_chicken(mv, glm_deg(headYaw), glm_deg(bodyYaw),
                              e->data.monster.frame, wingDestPos);
        break;
    }
    case MONSTER_SHEEP:
        // Fleece colour is explicit entity state so archive reload can assign a fresh
        // runtime id without changing the rendered wool or its eventual drop. sheared
        // suppresses the whole wool overlay pass (Beta RenderSheep !getSheared()).
        render_entity_sheep(mv, glm_deg(headYaw), glm_deg(bodyYaw),
                            e->data.monster.frame, e->data.monster.fleece_color,
                            e->data.monster.sheared);
        break;
    case MONSTER_SQUID:
        render_entity_squid(mv, glm_deg(headYaw), glm_deg(bodyYaw),
                            e->data.monster.frame);
        break;
    case MONSTER_PIG_ZOMBIE:
        render_entity_pigzombie_cue(mv, glm_deg(headYaw), glm_deg(bodyYaw),
                                e->data.monster.frame, windup_degrees);
        break;
    case MONSTER_GHAST:
        render_entity_ghast(mv, glm_deg(headYaw), glm_deg(bodyYaw),
                            e->data.monster.frame,
                            e->data.monster.ghast_attacking);
        break;
    case MONSTER_SLIME_SMALL:
    case MONSTER_SLIME_MEDIUM:
    case MONSTER_SLIME_BIG:
        // mv already carries the per-size feet scale (the block above the
        // switch); the render fn draws the size-1 two-pass model.
        render_entity_slime(mv, glm_deg(headYaw), glm_deg(bodyYaw),
                            e->data.monster.frame);
        break;
    default: // MONSTER_CREEPER (+ any unknown id) -> creeper, with swell flash
        // Pass the networked fuse (-1 idle..30) + tick_delta so the flash is driven
        // by THIS creeper's real fuse phase, per-instance (Beta getCreeperFlash).
        render_entity_creeper(mv, glm_deg(headYaw), glm_deg(bodyYaw),
                              e->data.monster.frame, e->data.monster.fuse, td);
        break;
    }

    // audit #17f: RESET the emissive tint so the shadow + every following entity/world
    // draw is untinted. MANDATORY -- on GX a leftover 2-stage TEV pipeline would
    // mis-draw and risk a FIFO hang; on GL it clears the shared tint uniform. Harmless
    // when tint_amount was already 0 (early-out in the backend).
    gfx_entity_tint(0.0f, 0.0f, 0.0f, 0.0f);

    // shadow, doesn't seem to work currently
    struct AABB shadow_bb;
    aabb_setsize_centered(&shadow_bb, 0.25f, 0.25f, 0.25f);
    aabb_translate(&shadow_bb, p[0], p[1] - 0.04f, p[2]);
    entity_shadow(e, &shadow_bb, view);
}



// Raycast & interaction bounding box (world-translated). Kept in LOCKSTEP with the
// collision box (audit #17a): the spider gets its squat 1.4x0.9 box here too, so the
// player's attack/interact raycast hits where the spider physically is -- otherwise
// the click box (0.6x1.8) would float above the real 0.9-tall body and the player
// would hit air above a spider / miss its wide flanks. This fn-ptr already receives the
// entity, so the per-id branch is local -- no need for a separate getBoundingBox_spider.
// Built from the entity width/height (set from monster_def in the constructor),
// the same dims the authoritative double box uses -- one id->box mapping.
static size_t getBoundingBox_creeper(const struct entity* e, struct AABB* out) {
    assert(e && out);
	entity_make_world_bbox(e, out);
    return 1;
}

bool onLeftClick(struct entity *e) {

    svin_rpc_send(&(struct server_rpc){
        .type = SRPC_ENTITY_ATTACK,
        .payload.entity_attack.entity_id = e->id,
    });

    return true;
}

// Spider jockey roll [740c583: SpawnerAnimals.creatureSpecificInit]:
// a spider becomes a jockey on a 1-in-100 roll of the world Random. Drawn
// once per successfully spawned EntitySpider -- a pack anchor AND each pack
// member independently, exactly as Beta's performSpawning calls
// creatureSpecificInit right after EVERY spawned entity joins the world, not
// once per pack -- on the same World.rand stream the rest of natural
// spawning already shares (the caller's s->rand_src). Pure/testable: takes
// the rand source directly so a golden test can pin the exact seed/draw
// without standing up a full server_local spawn fixture.
bool spider_jockey_roll(struct random_gen* rand_src) {
    assert(rand_src);
    return rand_gen_int(rand_src, 100) == 0;
}

// Mount `rider` onto `mount` as a spider jockey [740c583:
// SpawnerAnimals.creatureSpecificInit creates a skeleton at the spider's
// spawn point facing the spider's yaw with zero pitch, adds it to the
// world, then mounts it on the spider].
// Sets both halves of the linkage (the mount's
// jockey_rider_id, the rider's jockey_mount_id -- Entity.riddenByEntity /
// Entity.ridingEntity) and the rider's facing to the mount's exact yaw with
// a level pitch (mount yaw, zero pitch) -- position is already the
// caller's, since Beta spawns the skeleton at the SAME coordinates the
// spider itself was just placed at. Does not create or place either
// entity; the caller (server_local.c's natural hostile spawn pass) already
// did that via server_local_spawn_monster{,_at}.
void entity_monster_mount_jockey(struct entity* mount, struct entity* rider) {
    assert(mount && rider);
    mount->data.monster.jockey_rider_id = rider->id;
    rider->data.monster.jockey_mount_id = mount->id;
    rider->data.monster.body_yaw = mount->data.monster.body_yaw;
}

// Factory
/* Entity.setPositionAndRotation for a mob: the engine's default teleport
 * (network/pos_old/y_size bookkeeping) plus the S1 double-state seed, so a
 * spawn, load or teleport always leaves the authoritative state valid. */
void entity_monster_teleport(struct entity* e, vec3 pos) {
    entity_default_teleport(e, pos);
    mob_motion_seed(e, &e->data.monster.motion_d, pos[0], pos[1], pos[2],
                    e->vel[0], e->vel[1], e->vel[2]);
}

void entity_monster(uint32_t id,
                    struct entity* e,
                    bool server,
                    void* world,
                    int monster_id) {
    assert(e && world);
    entity_default_init(e, server, world);
    e->id = id;
    e->type = ENTITY_MONSTER;
    if(server) {
        /* Reproducible lifetime seed from the declared world seed, dimension and
         * assigned ID (ID already set): no world/generation RNG-stream draw and
         * no clock/default-constructor seed. Draws then use Java's algorithms. */
        const struct server_world* w = e->world;
        entity_lifetime_rng_seed(&e->gameplay_rng, w->seed, w->dimension, id);
    }

    // Pull per-mob stats from the Beta-faithful table (crash-safe: an unknown
    // id clamps to zombie). This is what makes a skeleton drop arrows and a
    // spider drop string instead of every mob being a creeper.
    const struct monster_def* def = monster_def_get(monster_id);
	entity_set_size(e, def->box_w, def->box_h, 0.0F);
	e->step_height = 0.5F;
    e->drop_item = (struct item_data){
        .id         = def->drop,
        .durability = 0,
        .count      = def->drop_count
    };
    e->name = def->name;
    e->data.monster.id = monster_id;
	// EntityWolf watcher/runtime state. This entity allocation is intentionally
	// not memset-zeroed, so every archived/new field must be initialized here.
	// [740c583: EntityWolf constructor/entityInit]
	e->data.monster.wolf_flags = 0;
	e->data.monster.wolf_owner[0] = '\0';
	e->data.monster.wolf_watched_health = def->health;
	e->data.monster.wolf_target_id = UINT32_MAX;
	e->data.monster.wolf_shake_armed = false;
	// CRPC_ENTITY_MOVE change-gate (server_tick_monster): the "starts false
	// (calloc)" assumption in that gate is wrong for this non-zeroed
	// allocation -- UBSan caught a 0xBE load of this _Bool in the 8000-iter
	// soak. First tick after spawn/load must always send the full snapshot.
	e->data.monster.last_sent_move_valid = false;
	e->data.monster.wolf_shake_active = false;
	e->data.monster.wolf_shake_time = 0.0f;
	e->data.monster.wolf_prev_shake_time = 0.0f;
	e->data.monster.wolf_interested = false;
	e->data.monster.wolf_head_rotation = 0.0f;
	e->data.monster.wolf_head_rotation_old = 0.0f;
	e->data.monster.wolf_has_current_target = false; // no latch on spawn
	e->data.monster.wolf_current_target_hold = 0;
	e->data.monster.fleece_color = (uint8_t)sheep_fleece_color(id);
    e->data.monster.sheared = false; // fresh sheep spawn woolly (Beta default)
    e->data.monster.saddled = false; // pig: unsaddled (Beta dataWatcher default 0)
    e->data.monster.pig_ridden = false; // never spawns pre-mounted (entity_minecart precedent)
    e->data.monster.pig_rider_actor = ACTOR_IDENT_HOST; // S1-a2, same rule
    e->data.monster.pig_rider_id = 0;
    e->data.monster.pig_seat_correction_pending = false; // R02 W4
    e->data.monster.jockey_mount_id = UINT32_MAX; // not riding anything (wolf_target_id precedent)
    e->data.monster.jockey_rider_id = UINT32_MAX; // no rider mounted on this mob
    e->data.monster.egg_timer = 0;   // chicken: unrolled sentinel -- the first
                                     // server tick draws Beta's 6000..11999 roll
                                     // (retained, documented deferral like the
                                     // slime jump_delay below)
    e->data.monster.head_yaw = 0.0f;
    e->data.monster.network_head_yaw = 0.0f; // MP wire head-look residual item 2:
                                              // no armed wire look pre-spawn
    e->data.monster.network_head_increments = 0;
    e->data.monster.idle_has_target = false; // idle head-turn state (client
                                              // cosmetic; see entity.h)
    e->data.monster.idle_target_ticks = 0;
    e->data.monster.idle_random_yaw_vel = 0.0f;
    e->health     = def->health;
    e->data.monster.body_yaw = 0.0f;
    e->data.monster.direction[0] = 0.0f;
    e->data.monster.direction[1] = 0.0f;
    e->data.monster.direction_time= 0;
    e->data.monster.frame = 0;
    e->data.monster.fuse = -1;
    e->data.monster.creeper_state = -1; // Beta entityInit: watcher slot 16 starts at -1
	e->data.monster.explosion_pending = false;
	e->data.monster.charged = false; // uncharged until EntityCreeper.onStruckByLightning
    e->data.monster.attack_time = 0; // can swing on the first eligible tick (Beta)
    e->data.monster.attack_windup = MOB_ATTACK_WINDUP_TICKS; // telegraph the first swing too
    e->data.monster.windup_cue = (struct mob_windup_cue) {
        server ? MOB_WINDUP_CUE_VERSION : 0, 0};
    e->data.monster.fire_time = 0; // not burning until daylight ignites it
    e->data.monster.hurt_time = 0; // no hurt animation yet
    e->data.monster.hurt_resist_time = 0; // no damage-arbitration window yet
    e->data.monster.last_damage = 0;
    e->data.monster.knockback_time = 0; // not being knocked back
    e->data.monster.prev_on_ground = false;   // Beta Entity.onGround starts false
    e->data.monster.prev_collided_xz = false; // Beta isCollidedHorizontally starts false
    e->data.monster.aggro_time = 0; // no latched target on spawn (audit #14)
    /* S1-a2: no retained target identity on spawn either. struct entity is
     * NOT zeroed by server_entity_alloc (the sibling explicit zeroes here
     * exist for exactly that reason), so this must be written, not
     * assumed -- an uninitialised slot would make a fresh mob chase a
     * garbage actor. */
    e->data.monster.target_actor = ACTOR_IDENT_HOST;
    path_entity_clear(&e->data.monster.path); // FH-13: no path on spawn (Beta
                                    // pathToEntity starts null); MANDATORY -- this
                                    // allocation is not memset-zeroed (see the wolf
                                    // note above), so an uninitialised path.length
                                    // would be garbage, not 0.
    // Batch 1b: pig zombie anger + ghast flight state. The entity storage is
    // NOT zeroed (field-by-field init is mandatory here); garbage anger used
    // to fail the archive's TAG_SHORT range check and silently kill the whole
    // chunk-entity save.
    e->data.monster.anger_level = 0;       // calm until hit (Beta angerLevel)
    e->data.monster.pig_zombie_angry = false; // client mirror (attack assist)
    e->data.monster.anger_sound_delay = 0;
    e->data.monster.ghast_waypoint[0] = 0.0f;
    e->data.monster.ghast_waypoint[1] = 0.0f;
    e->data.monster.ghast_waypoint[2] = 0.0f; // dist>60 -> first tick re-rolls
    e->data.monster.ghast_course_cooldown = 0;
    e->data.monster.ghast_attack_counter = 0;
    e->data.monster.ghast_attacking = false;
    e->data.monster.limb_swing = 0.0f; // legs at rest until it walks
    // MUST be initialised: client mobs are malloc'''d, and an uninitialised true
    // here would swallow the first hurt-flail (rising edge never fires). Same
    // class of bug as the minecart pending_throttle miss.
    e->data.monster.hurt_prev = false;
    // S2 sound state (see entity.h): fresh odometer, ramp at 0 (Beta field
    // default), splash latch TRUE (the isFirstUpdate guard -- a squid spawning
    // in water must not splash).
    e->data.monster.walk_dist = 0.0f;
    e->data.monster.walk_next_step = 1;
    e->data.monster.living_sound_time = 0;
    e->data.monster.was_in_water = true;
    e->data.monster.fall_distance = 0.0f; // no accumulated fall on spawn (audit #15)
    e->data.monster.air = MOB_AIR_MAX; // full breath on spawn (Beta air = 300)
    e->data.monster.entity_age = 0; // fresh mob: despawn clock starts now (audit #16)
    e->data.monster.death_time = 0; // alive: no death animation yet (audit #18 item 3)
    e->data.monster.death_loot_dropped = false;
    e->data.monster.hit_by_skeleton_arrow = false;
    e->data.monster.jump_delay = 0; // slime: first grounded tick hops (Beta rolls in
                                    // the constructor; retained documented deferral)
    e->data.monster.swim_vec[0] = 0.0f; // squid: zero vec -> the Beta vec-zero clause
    e->data.monster.swim_vec[1] = 0.0f; // forces a fresh roll on the first tick
    e->data.monster.swim_vec[2] = 0.0f;
    e->data.monster.swim_speed = 0.0f;  // squid: first contraction kick sets 1.0
    e->data.monster.squid_phase = 0.0f;
    e->data.monster.squid_phase_vel = 0.15f; // Beta 1/(rand+1)*0.2 in (0.1,0.2]; the
                                    // midpoint seeds it (no RNG here), the 1-in-10
                                    // wrap re-roll converges to the Beta distribution
    e->ai_timer = 0.0f; // skeleton ranged shot cooldown (ready immediately)
    e->data.monster.frame_time_left = 1;
    e->detection_range = MOB_FOLLOW_RANGE; // Beta 16-block target-acquisition range
    e->ai_state = AI_IDLE;
    glm_vec3_copy(e->pos, e->network_pos);
    e->tick_server = entity_server_tick;
    e->tick_client = entity_client_tick;
    e->render      = entity_render;
    e->teleport    = entity_monster_teleport;
    e->getBoundingBox = getBoundingBox_creeper;
    e->onLeftClick = onLeftClick;
    e->leftClickText = "Attack";
    e->onRightClick = NULL;
    e->rightClickText = NULL;
    e->delay_destroy = -1;
    // spawn slightly higher, to prevent getting stuck
    if (server) {
        e->pos[1] += 0.1f;
        e->pos_old[1] = e->pos[1];
    }
    // Mob motion units S1: seed the authoritative double state from the
    // constructed position/motion (a spawn path then teleports, which seeds
    // again through entity_monster_teleport).
    memset(&e->data.monster.motion_d, 0, sizeof(e->data.monster.motion_d));
    mob_motion_seed(e, &e->data.monster.motion_d, e->pos[0], e->pos[1],
                    e->pos[2], e->vel[0], e->vel[1], e->vel[2]);
    for (int i = 0; i < 3; i++)
        e->data.monster.motion_d.prev_pos[i] = e->pos_old[i];
    // NOTE (audit #17a wide-box, needs a Dolphin/device playtest): the spider's
    // faithful box is 1.4 wide (half-extent 0.7) vs the 0.6 biped box (0.3), so it
    // can spawn embedded in a side wall the narrow box cleared. This vertical-only
    // nudge (+0.1Y) and entity_try_unstuck (which also only nudges +Y) do NOT
    // de-penetrate horizontally. The box MUST stay 1.4 (Beta EntitySpider.setSize),
    // so the fix -- ONLY IF a real device run shows spiders wedging in <=1-wide gaps
    // or flush against geometry -- is to add horizontal unstuck for the spider or
    // require a >=1.5-wide spawn cell, NOT to shrink the box. Deferred to a playtest
    // (CLAUDE.md: verify before changing) rather than added speculatively here.
    // (ai_timer is the skeleton shot cooldown, already set to 0 above so the first
    // arrow fires on the first eligible tick -- Beta attackTime starts at 0. The
    // old `ai_timer = CREEPER_WANDER_INTERVAL` here clobbered that; wander uses
    // data.monster.direction_time, not ai_timer.)
}

bool entity_monster_has_pending_explosion(const struct entity* e) {
	return e && e->type == ENTITY_MONSTER
		&& e->data.monster.id == MONSTER_CREEPER
		&& e->data.monster.explosion_pending
		&& e->data.monster.fuse >= CREEPER_FUSE_TICKS;
}

bool entity_monster_retry_pending_explosion(
	struct entity* e, struct server_local* s) {
	assert(entity_monster_has_pending_explosion(e) && s);
	vec3 center = {e->pos[0], e->pos[1], e->pos[2]};
	// PARITY FIX (audit register 2026-08-25, entities-mobs #1): this previously
	// hardcoded the UNPOWERED CREEPER_EXPLOSION_POWER (3.0F), never reading
	// e->data.monster.charged -- so a charged creeper whose FIRST detonation
	// attempt above got deferred by the Wii admission cap (fuse held at
	// CREEPER_FUSE_TICKS, explosion_pending=true) would, once the retry finally
	// succeeded, detonate at ordinary 3.0F strength instead of the charged
	// 6.0F Beta guarantees. Beta EntityCreeper.attackEntity [740c583] is one
	// unconditional, single attempt that picks strength 6.0F when powered and
	// 3.0F otherwise -- it never splits into a deferred retry,
	// so a charged creeper's blast strength can never silently downgrade there.
	// `charged` is a stable per-instance field (monsters_object.c inits false;
	// only the lightning-strike handler in server_weather.c sets it true; never
	// cleared; persisted as the "powered" NBT tag), so re-reading it here on the
	// retry is exactly as valid as reading it on the first attempt above.
	float power = e->data.monster.charged ? CREEPER_EXPLOSION_POWER_CHARGED
										   : CREEPER_EXPLOSION_POWER;
	return server_world_try_explode_by((struct server_world*)e->world, e, center, power);
}
