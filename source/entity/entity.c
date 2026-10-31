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

#include "entity.h"
#include <stdio.h> // fprintf/stderr for the entities_client_tick malloc-failure fallback
#include <string.h> /* memset: the water-flow proxy (G8-BLAST-VELOCITY v3) */
#include "../game/camera.h" // direct include (was transitive via world.h pre-split)
#include "../block/redstone_piston_server.h"
#include "../network/piston_moving_transport.h"
#include "../network/server_world.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "entity_painting.h" // painting_compute_geometry -- see
                              // entity_aabb_intersect_ray's ENTITY_PAINTING
                              // branch below (post-review finding #1)
#include "entity_local_player.h" // local_player_column_present: THE client
                                 // chunkExists gate (entity_chunk_exists)
#include "../block/blocks.h" // block_beta_material_is_solid (getFlowVector)
#include "combat.h" // PLAYER_EYE_HEIGHT (the host player's push body, P4)

#include "../platform/gfx.h"
#include "../platform/watchdog.h" // GP-wedge draw breadcrumb
#include "../graphics/render_entity_fire.h"
#include "../world.h"

static bool entity_piston_moving_lookup(void* ctx, w_coord_t x, w_coord_t y,
									w_coord_t z,
									struct piston_moving_state* out) {
	struct entity* entity = ctx;
	if(!entity)
		return false;
	if(entity->on_server) {
		struct server_world* world = entity->world;
		return world && world->local
			&& piston_moving_state_at(world->local, x, y, z, out);
	}
	return client_piston_moving_query(x, y, z, out);
}

void entity_bind_world(struct entity* e, void* world) {
	assert(e);
	e->world = world;
	e->world_slot = 0;
	e->world_generation = 0;
	if(e->on_server && world) {
		const struct server_world* w = world;
		e->world_slot = w->world_index;
		e->world_generation = w->generation;
	}
}

void entity_lifetime_rng_seed(struct java_random* r, int64_t world_seed,
                              int dimension, uint32_t id) {
	assert(r);
	uint64_t seed = (uint64_t)world_seed;
	uint32_t mixed = hash_u32((uint32_t)seed
		^ hash_u32((uint32_t)(seed >> 32))
		^ hash_u32((uint32_t)dimension ^ 0x9E3779B9u)
		^ hash_u32(id ^ 0x85EBCA6Bu));
	if(!mixed)
		mixed = 0x6C8E9CF5u;
	jrand_set_seed(r, (int64_t)(uint64_t)mixed); // zero-extend, never sign-extend
}

void entity_default_init(struct entity* e, bool server, void* world) {
	e->on_server = server;
	/* Factories assign their lifetime ID after this common initializer. */
	memset(&e->gameplay_rng, 0, sizeof(e->gameplay_rng));
	entity_bind_world(e, world);
	e->client_pub_slot = 0;
	e->client_pub_generation = 0;
	/* Entity(World) starts airborne; subclasses establish ground only after a
	 * real collision. */
	e->on_ground = false;
	e->width = 0.6F;
	e->height = 1.8F;
	e->y_offset = 0.0F;
	e->y_size = 0.0F;
	e->step_height = 0.0F;
	e->fall_distance = 0.0F;
	e->collided_horizontally = false;
	e->collided_vertically = false;
	e->collided = false;
	e->in_web = false;
	e->chunk_coord_x = 0;
	e->chunk_coord_y = 0;
	e->chunk_coord_z = 0;
	e->chunk_list_order = 0;
	e->added_to_chunk = false;
	e->delay_destroy = -1;

	glm_vec3_zero(e->pos);
	glm_vec3_zero(e->pos_old);
	glm_vec3_zero(e->network_pos);
	glm_vec3_zero(e->vel);
	glm_vec2_zero(e->orient);
	glm_vec2_zero(e->orient_old);

	// The client networks these per CRPC_ENTITY_MOVE, but client_tick_monster /
	// entity_render read them on the FIRST frame (before the first move RPC), so
	// they must start false -- else a fresh mob flickers spurious fire/smoke or a
	// swell/hurt flash from garbage.
	e->on_fire = false;
	e->swelling = false;
	e->hurt = false; // audit #17f red hurt-flash (networked); see above
}

void entity_set_size(struct entity* e, float width, float height,
					 float y_offset) {
	assert(e && width >= 0.0F && height >= 0.0F);
	e->width = width;
	e->height = height;
	e->y_offset = y_offset;
}

void entity_make_local_bbox(const struct entity* e, struct AABB* out) {
	assert(e && out);
	/* Entity.setPosition: the box bottom is position height minus eye offset plus step easing. Express that as a
	 * local centered box so existing collision helpers can still translate by
	 * logical pos. [740c583: Entity.setPosition] */
	aabb_setsize_centered_offset(out, e->width, e->height, e->width,
		0.0F, e->height * 0.5F - e->y_offset + e->y_size, 0.0F);
}

void entity_make_world_bbox(const struct entity* e, struct AABB* out) {
	entity_make_local_bbox(e, out);
	aabb_translate(out, e->pos[0], e->pos[1], e->pos[2]);
}

void entity_set_position_from_bbox(struct entity* e, const struct AABB* box) {
	assert(e && box);
	e->pos[0] = (box->x1 + box->x2) * 0.5F;
	e->pos[1] = box->y1 + e->y_offset - e->y_size;
	e->pos[2] = (box->z1 + box->z2) * 0.5F;
}

void entity_default_teleport(struct entity* e, vec3 pos) {
	e->on_ground = false;
	e->y_size = 0.0F;

	glm_vec3_copy(pos, e->pos);
	glm_vec3_copy(pos, e->pos_old);
	glm_vec3_copy(pos, e->network_pos);
	/* A teleport-seed is authoritative: any in-flight approach is void and the
	 * look target rebases to the current orientation (CRPC_SPAWN_* writes
	 * orient before calling teleport). */
	glm_vec2_copy(e->orient, e->network_orient);
	e->network_increments = 0;
}

/* Beta's client-side approach step, shared by every networked entity tick.
 * Positions approach the target by the remaining-distance fraction. Yaw takes the SHORTEST way
 * around: the delta is wrapped into [-180,180) BEFORE dividing, exactly
 * Beta's repeated full-turn adjustment at either end of that interval.
 * Decrement happens once per tick; one remaining increment therefore lands EXACTLY on
 * the target (no asymptotic tail -- the old 0.3f decay never landed).
 * [740c583 EntityLiving.onLivingUpdate] [740c583
 * EntityOtherPlayerMP.onLivingUpdate] (identical math in both). */
void entity_network_approach_tick(struct entity* e, bool apply_look) {
	if(e->network_increments == 0) {
		/* Local-provider snap: Beta SP applied entity motion directly each
		 * tick; the render layer's pos_old->pos lerp supplies the smoothing.
		 * Look is applied externally (CRPC handlers write orient directly on
		 * this path), so only position snaps here. */
		glm_vec3_copy(e->network_pos, e->pos);
		return;
	}
	float n = (float)e->network_increments;
	e->pos[0] += (e->network_pos[0] - e->pos[0]) / n;
	e->pos[1] += (e->network_pos[1] - e->pos[1]) / n;
	e->pos[2] += (e->network_pos[2] - e->pos[2]) / n;
	if(apply_look) {
		float dyaw = e->network_orient[0] - e->orient[0];
		while(dyaw < -180.0F)
			dyaw += 360.0F;
		while(dyaw >= 180.0F)
			dyaw -= 360.0F;
		e->orient[0] += dyaw / n;
		e->orient[1] += (e->network_orient[1] - e->orient[1]) / n;
	}
	e->network_increments--;
}

bool entity_default_client_tick(struct entity* e) {
	assert(e);

	glm_vec3_copy(e->pos, e->pos_old);
	glm_vec2_copy(e->orient, e->orient_old);
	entity_network_approach_tick(e, true);
	return false;
}

bool entity_get_block(struct entity* e, w_coord_t x, w_coord_t y, w_coord_t z,
					  struct block_data* blk) {
	assert(e && blk);

	if(e->on_server) {
		return server_world_get_block(e->world, x, y, z, blk);
	} else {
		*blk = world_get_block(e->world, x, y, z);
		return true;
	}
}

// Beta Entity.moveEntity post-move block-collide dispatch, reduced to the one Beta 1.7.3
// handler that alters motion: BlockSoulSand.onEntityCollidedWithBlock scales both horizontal
// motion components by 0.4 in double precision. Beta iterates every cell the entity's post-move AABB intersects --
// floored lower bounds inset by 0.001 through floored upper bounds inset by 0.001 -- and dispatches PER
// CELL, so a box straddling a seam between two soul-sand blocks is multiplied once per
// block (0.4^n): authentic Beta, kept on purpose. Standing on soul sand intersects its
// cell only because the collision box is 7/8 tall (the feet sit inside the top 1/8) --
// the box shave and this slowdown are one mechanism. `box` is the entity's ABSOLUTE
// post-move AABB. (BlockPortal.onEntityCollidedWithBlock -> setInPortal rides this same
// dispatch in Beta; wire portal contact here when M5 lands. Server mobs' move path does
// not call this yet -- a documented player-only scope, like the farmland-trample
// precedent -- wire entity_monster's post-move when mobs meet soul sand in M6.)
// [decompile 740c583 Entity.moveEntity + BlockSoulSand.onEntityCollidedWithBlock]
void entity_soulsand_slowdown(struct entity* e, const struct AABB* box,
							  float* vx, float* vz) {
	assert(e && box && vx && vz);

	w_coord_t x0 = (w_coord_t)floorf(box->x1 + 0.001F);
	w_coord_t y0 = (w_coord_t)floorf(box->y1 + 0.001F);
	w_coord_t z0 = (w_coord_t)floorf(box->z1 + 0.001F);
	w_coord_t x1 = (w_coord_t)floorf(box->x2 - 0.001F);
	w_coord_t y1 = (w_coord_t)floorf(box->y2 - 0.001F);
	w_coord_t z1 = (w_coord_t)floorf(box->z2 - 0.001F);

	for(w_coord_t x = x0; x <= x1; x++)
		for(w_coord_t y = y0; y <= y1; y++)
			for(w_coord_t z = z0; z <= z1; z++) {
				struct block_data blk;
				if(entity_get_block(e, x, y, z, &blk)) {
					if(blk.type == BLOCK_SOULSAND) {
						*vx *= 0.4F;
						*vz *= 0.4F;
					}
					/* next-gaps-0924/W2 P12: BlockWeb.onEntityCollidedWith-
					 * Block [BlockWeb.java:10-12] rides the same per-cell
					 * dispatch. */
					if(blk.type == BLOCK_COBWEB)
						e->in_web = true;
				}
			}
}

void entity_web_contact(struct entity* e, const struct AABB* box) {
	assert(e && box);
	w_coord_t x0 = (w_coord_t)floorf(box->x1 + 0.001F);
	w_coord_t y0 = (w_coord_t)floorf(box->y1 + 0.001F);
	w_coord_t z0 = (w_coord_t)floorf(box->z1 + 0.001F);
	w_coord_t x1 = (w_coord_t)floorf(box->x2 - 0.001F);
	w_coord_t y1 = (w_coord_t)floorf(box->y2 - 0.001F);
	w_coord_t z1 = (w_coord_t)floorf(box->z2 - 0.001F);
	for(w_coord_t x = x0; x <= x1; x++)
		for(w_coord_t y = y0; y <= y1; y++)
			for(w_coord_t z = z0; z <= z1; z++) {
				struct block_data blk;
				if(entity_get_block(e, x, y, z, &blk)
				   && blk.type == BLOCK_COBWEB)
					e->in_web = true;
			}
}

bool entity_web_consume(struct entity* e) {
	assert(e);
	if(!e->in_web)
		return false;
	e->in_web = false;
	e->vel[0] = (float)((double)e->vel[0] * 0.25);
	e->vel[1] = (float)((double)e->vel[1] * (double)0.05F);
	e->vel[2] = (float)((double)e->vel[2] * 0.25);
	return true;
}

/* ---- next-gaps-0924/W2 P3: Beta water-flow push ---------------------------
 * World.handleMaterialAcceleration [World.java:1418-1450] +
 * BlockFluid.getFlowVector [BlockFluid.java:86-166] transcribed for
 * Material.water, in Beta's doubles. Behavioral source anchors follow.
 * Every entity kind that reaches Entity.onEntityUpdate:168 gets its inWater
 * verdict AND its 0.014D impulse from here; the callers (player walk, mob
 * tick, item tick) run it BEFORE their move, as onUpdate orders it. */

/* World.chunkExists [World.java:350-352]. Server: the resident-chunk
 * dict (server_world_chunk_resident). Client: the column-present translation
 * every client-side Beta chunkExists already uses (entity_local_player.c,
 * F8b ruling) -- Beta SP's ChunkProviderLoadOrGenerate answers real
 * existence, and the client only ever holds columns the server sent. */
static bool entity_chunk_exists(struct entity* e, w_coord_t cx, w_coord_t cz) {
	if(e->on_server)
		return server_world_chunk_resident(e->world, cx, cz);
	return local_player_column_present(e->world, cx * CHUNK_SIZE,
									   cz * CHUNK_SIZE);
}

static bool water_block_id(uint8_t type) {
	return type == BLOCK_WATER_FLOW || type == BLOCK_WATER_STILL;
}

/* World.getBlockId / getBlockMetadata: an unreadable cell (out of range,
 * absent chunk on the server) is id 0 / meta 0 -- the convention every
 * hazard scan in this port uses for Beta's getBlockId 0. */
static void water_cell_read(struct entity* e, w_coord_t x, w_coord_t y,
							w_coord_t z, struct block_data* b) {
	if(!entity_get_block(e, x, y, z, b)) {
		b->type = BLOCK_AIR;
		b->metadata = 0;
	}
}

/* BlockFluid.getEffectiveFlowDecay [BlockFluid.java:35-45]:
 * Other materials return -1; falling metadata yields zero; otherwise use metadata.
 * Takes the already-read cell so the material test that follows a negative
 * answer (getFlowVector:110) costs no second read of the same cell. */
static int water_effective_decay(const struct block_data* b) {
	if(!water_block_id(b->type))
		return -1;
	int m = b->metadata;
	return m >= 8 ? 0 : m;
}

/* BlockFluid.getIsBlockSolid [BlockFluid.java:60-63]
 * for sides 2..5 (the only ones getFlowVector passes):
 * Water and ice are nonsolid; the upward-side exception is unreachable here.
 * Other sides use the material solidity verdict
 * [Block.java:239-241]. */
static bool water_fluid_side_solid(struct entity* e, w_coord_t x, w_coord_t y,
								   w_coord_t z) {
	struct block_data b;
	water_cell_read(e, x, y, z, &b);
	if(water_block_id(b.type) || b.type == BLOCK_ICE)
		return false;
	return block_beta_material_is_solid(b.type);
}

/* Vec3D.normalize [Vec3D.java:63-66]: length through MathHelper.sqrt_double,
 * which is the square root rounded to float [MathHelper.java:18-20] -- the float rounding of
 * the length is part of the value; < 1.0E-4 collapses to the zero vector. */
static void water_vec_normalize(double v[3]) {
	double len = (double)(float)sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if(len < 1.0E-4) {
		v[0] = v[1] = v[2] = 0.0;
		return;
	}
	v[0] /= len;
	v[1] /= len;
	v[2] /= len;
}

/* BlockFluid.getFlowVector [BlockFluid.java:86-166] for a
 * water cell whose block_data is `cell`. Neighbour order -X, -Z, +X, +Z
 * (:91-107); a non-water neighbour that is not a solid material looks one
 * cell DOWN and weights that decay against (decay - 8) (:110-116); a water
 * neighbour weights (its decay - decay) (:118-120). A falling cell (raw
 * metadata >= 8, :124) with any of the eight listed solid sides (:126-157,
 * ordered short-circuit tests kept) is normalised and pulled by
 * (0, -6, 0) (:159-161) before the final normalise (:164). The Y term of the
 * neighbour sum is zero because the vertical coordinates are equal. */
static void water_flow_vector(struct entity* e, w_coord_t x, w_coord_t y,
							  w_coord_t z, const struct block_data* cell,
							  double out[3]) {
	static const int dx[4] = {-1, 0, 1, 0};
	static const int dz[4] = {0, -1, 0, 1};
	double v[3] = {0.0, 0.0, 0.0};
	int decay = water_effective_decay(cell);

	for(int i = 0; i < 4; i++) {
		w_coord_t nx = x + dx[i];
		w_coord_t nz = z + dz[i];
		struct block_data nb;
		water_cell_read(e, nx, y, nz, &nb);
		int nd = water_effective_decay(&nb);
		if(nd < 0) {
			if(!block_beta_material_is_solid(nb.type)) {
				struct block_data ub;
				water_cell_read(e, nx, y - 1, nz, &ub);
				nd = water_effective_decay(&ub);
				if(nd >= 0) {
					int k = nd - (decay - 8);
					v[0] += (double)((nx - x) * k);
					v[2] += (double)((nz - z) * k);
				}
			}
		} else {
			int k = nd - decay;
			v[0] += (double)((nx - x) * k);
			v[2] += (double)((nz - z) * k);
		}
	}

	if(cell->metadata >= 8) {
		bool solid = water_fluid_side_solid(e, x, y, z - 1)
			|| water_fluid_side_solid(e, x, y, z + 1)
			|| water_fluid_side_solid(e, x - 1, y, z)
			|| water_fluid_side_solid(e, x + 1, y, z)
			|| water_fluid_side_solid(e, x, y + 1, z - 1)
			|| water_fluid_side_solid(e, x, y + 1, z + 1)
			|| water_fluid_side_solid(e, x - 1, y + 1, z)
			|| water_fluid_side_solid(e, x + 1, y + 1, z);
		if(solid) {
			water_vec_normalize(v);
			v[1] += -6.0;
		}
	}

	water_vec_normalize(v);
	out[0] = v[0];
	out[1] = v[1];
	out[2] = v[2];
}

static bool entity_water_accel_core(struct entity* e, double min_x,
	double min_y, double min_z, double max_x, double max_y, double max_z,
	double impulse[3], bool* has_impulse);

bool entity_water_acceleration(struct entity* e, double min_x, double min_y,
							   double min_z, double max_x, double max_y,
							   double max_z) {
	assert(e);
	double acc[3];
	bool has = false;
	bool hit = entity_water_accel_core(e, min_x, min_y, min_z, max_x, max_y,
									   max_z, acc, &has);
	if(has) {
		const double water_impulse_scale = 0.014;
		if(e->on_server && e->type == ENTITY_MONSTER) {
			/* Mob motion units S2: a mob's motion is the authoritative
			 * double, with normalized flow scaled by 0.014 added to motion. */
			mob_motion_add_velocity(e, &e->data.monster.motion_d,
									acc[0] * water_impulse_scale, acc[1] * water_impulse_scale,
									acc[2] * water_impulse_scale);
		} else {
			e->vel[0] = (float)((double)e->vel[0] + acc[0] * water_impulse_scale);
			e->vel[1] = (float)((double)e->vel[1] + acc[1] * water_impulse_scale);
			e->vel[2] = (float)((double)e->vel[2] + acc[2] * water_impulse_scale);
		}
	}
	return hit;
}

bool entity_server_water_flow_impulse(struct server_world* w, double min_x,
	double min_y, double min_z, double max_x, double max_y, double max_z,
	double out[3]) {
	assert(w && out);
	/* A read-only proxy: the core only reads e->world / e->on_server. */
	static struct entity proxy;
	memset(&proxy, 0, sizeof(proxy));
	proxy.on_server = true;
	proxy.world = w;
	double acc[3];
	bool has = false;
	bool hit = entity_water_accel_core(&proxy, min_x, min_y, min_z, max_x,
									   max_y, max_z, acc, &has);
	out[0] = has ? acc[0] * 0.014 : 0.0;
	out[1] = has ? acc[1] * 0.014 : 0.0;
	out[2] = has ? acc[2] * 0.014 : 0.0;
	return hit;
}

/* The scan + flow sum + normalise of World.handleMaterialAcceleration; the
 * caller applies the impulse scaled by 0.014 in double precision (byte-identical split of the former
 * single function). */
static bool entity_water_accel_core(struct entity* e, double min_x,
	double min_y, double min_z, double max_x, double max_y, double max_z,
	double impulse[3], bool* has_impulse) {
	*has_impulse = false;
	/* :1419-1424 -- MathHelper.floor_double (a true floor) of min and of
	 * max + 1.0D; the cell loops below are half-open on these. */
	const w_coord_t x0 = (w_coord_t)floor(min_x);
	const w_coord_t x1 = (w_coord_t)floor(max_x + 1.0);
	const w_coord_t y0 = (w_coord_t)floor(min_y);
	const w_coord_t y1 = (w_coord_t)floor(max_y + 1.0);
	const w_coord_t z0 = (w_coord_t)floor(min_z);
	const w_coord_t z1 = (w_coord_t)floor(max_z + 1.0);

	/* :1425 checkChunksExist [World.java:327-348]:
	 * With a nonnegative upper Y and lower Y below 128, EVERY chunk of x0>>4 .. x1>>4,
	 * z0>>4 .. z1>>4 INCLUSIVE must exist, else false / no impulse. Note the
	 * span reaches floor(max + 1)'s chunk, one past the last scanned cell:
	 * an entity whose box ends inside the last cell of a chunk needs the next
	 * chunk resident too (pinned by the seam golden). */
	if(!(y1 >= 0 && y0 < WORLD_HEIGHT))
		return false;
	const w_coord_t cx0 = WCOORD_CHUNK_OFFSET(x0);
	const w_coord_t cx1 = WCOORD_CHUNK_OFFSET(x1);
	const w_coord_t cz0 = WCOORD_CHUNK_OFFSET(z0);
	const w_coord_t cz1 = WCOORD_CHUNK_OFFSET(z1);
	for(w_coord_t cx = cx0; cx <= cx1; cx++)
		for(w_coord_t cz = cz0; cz <= cz1; cz++)
			if(!entity_chunk_exists(e, cx, cz))
				return false;

	/* :1428-1443 -- every water cell of the span counts (the surface-height
	 * comparison uses the exclusive top of the scan, so each water surface
	 * lies below that limit for every scanned water cell; the original
	 * comparison is preserved) and adds its flow vector. */
	bool hit = false;
	double acc[3] = {0.0, 0.0, 0.0};
	for(w_coord_t x = x0; x < x1; x++)
		for(w_coord_t y = y0; y < y1; y++)
			for(w_coord_t z = z0; z < z1; z++) {
				struct block_data b;
				water_cell_read(e, x, y, z, &b);
				if(!water_block_id(b.type))
					continue;
				/* BlockFluid.getPercentAir [BlockFluid.java:18-25]. */
				int m = b.metadata;
				if(m >= 8)
					m = 0;
				float percent_air = (float)(m + 1) / 9.0F;
				double surface = (double)((float)(y + 1) - percent_air);
				if((double)y1 >= surface) {
					hit = true;
					double fv[3];
					water_flow_vector(e, x, y, z, &b, fv);
					acc[0] += fv[0];
					acc[1] += fv[1];
					acc[2] += fv[2];
				}
			}

	/* :1445-1450 -- a positive vector length (float-rounded sqrt, Vec3D.java:97-99)
	 * permits normalization and addition of the flow scaled by 0.014. Motion is float in this port
	 * (CODESTYLE 1.4, the recorded representation deviation): the double sum
	 * is formed as Beta forms it and rounded once on the store. */
	double len = (double)(float)sqrt(acc[0] * acc[0] + acc[1] * acc[1]
									 + acc[2] * acc[2]);
	if(len > 0.0) {
		water_vec_normalize(acc);
		impulse[0] = acc[0];
		impulse[1] = acc[1];
		impulse[2] = acc[2];
		*has_impulse = true;
	}
	return hit;
}

/* ---- next-gaps-0924/W2 P4: Beta entity-entity push -------------------------
 * Entity.applyEntityCollision [Entity.java:668-691]:
 * Horizontal separation is measured from the receiving body toward the mover.
 * Use the larger absolute component [MathHelper.java:36-46].
 * Separations below the double-promoted 0.01F threshold do not push.
 * Take its square root rounded through float, then divide both components.
 * Clamp the reciprocal divisor to at most one and scale both components.
 * Scale each by double-promoted 0.05F, then by one minus the float collision reduction.
 * The collision-reduction field defaults to zero [Entity.java:49];
 * no Beta 1.7.3 entity kind changes it.
 * Apply opposite horizontal impulses immediately to the two bodies.
 * Vertical motion is unaffected.
 *
 *
 * Note the Beta quirk kept verbatim: the divisor is the square root of the
 * LARGER |component|, not of the distance. */
bool entity_collision_impulse(double this_x, double this_z, double mover_x,
							  double mover_z, double* dx, double* dz) {
	assert(dx && dz);
	double delta_x = mover_x - this_x;
	double delta_z = mover_z - this_z;
	double a2 = delta_x < 0.0 ? -delta_x : delta_x;
	double a4 = delta_z < 0.0 ? -delta_z : delta_z;
	double separation_scale = a2 > a4 ? a2 : a4;
	if(!(separation_scale >= (double)0.01F))
		return false;
	separation_scale = (double)(float)sqrt(separation_scale);
	delta_x /= separation_scale;
	delta_z /= separation_scale;
	double reciprocal_clamp = 1.0 / separation_scale;
	if(reciprocal_clamp > 1.0)
		reciprocal_clamp = 1.0;
	delta_x *= reciprocal_clamp;
	delta_z *= reciprocal_clamp;
	delta_x *= (double)0.05F;
	delta_z *= (double)0.05F;
	delta_x *= (double)(1.0F - 0.0F);
	delta_z *= (double)(1.0F - 0.0F);
	*dx = delta_x;
	*dz = delta_z;
	return true;
}

/* One side of the loop: the mover's Entity.setPosition box (Beta doubles) and
 * position. A mob box extends half its width horizontally, from position height
 * minus eye offset plus step easing through its height. The player is 0.6F x 1.8F (server_player.y is
 * the eye, 1.62F above the feet). */
struct entity_push_body {
	double x, z;
	double min_x, min_y, min_z, max_x, max_y, max_z;
};

static void entity_push_body_of_entity(const struct entity* e,
									   struct entity_push_body* b) {
	const double half = (double)(e->width / 2.0F);
	b->x = (double)e->pos[0];
	b->z = (double)e->pos[2];
	b->min_x = b->x - half;
	b->max_x = b->x + half;
	b->min_z = b->z - half;
	b->max_z = b->z + half;
	b->min_y = (double)e->pos[1] - (double)e->y_offset + (double)e->y_size;
	b->max_y = b->min_y + (double)e->height;
}

static void entity_push_body_of_player(const struct server_player* p,
									   struct entity_push_body* b) {
	const double half = (double)(0.6F / 2.0F);
	b->x = p->x;
	b->z = p->z;
	b->min_x = b->x - half;
	b->max_x = b->x + half;
	b->min_z = b->z - half;
	b->max_z = b->z + half;
	b->min_y = p->y - (double)PLAYER_EYE_HEIGHT;
	b->max_y = b->min_y + (double)1.8F;
}

/* AxisAlignedBB.intersectsWith [AxisAlignedBB.java:185]: strict on all axes. */
static bool entity_push_boxes_intersect(const struct entity_push_body* a,
										double min_x, double min_y,
										double min_z, double max_x,
										double max_y, double max_z) {
	return a->max_x > min_x && a->min_x < max_x && a->max_y > min_y
		&& a->min_y < max_y && a->max_z > min_z && a->min_z < max_z;
}

/* EntityLiving.canBePushed [EntityLiving.java:85-87] requires a nondead body for the kinds
 * this card delivers: living mobs (a mob in its death flop is not yet isDead,
 * exactly like Beta's deathTime window) and the host player. Minecarts and
 * boats answer true in Beta too, through their own applyEntityCollision
 * overrides (EntityMinecart.java:650-700 / Entity's) -- their push lives with
 * their own tick code (entity_minecart.c's player nudge) and is recorded as a
 * follow-up in the intent, not silently dropped. Items, arrows, TNT,
 * paintings: false (Entity.java:714-716). */
static bool entity_push_can_be_pushed(const struct entity* e) {
	return e->type == ENTITY_MONSTER && e->delay_destroy != 0;
}

/* The host player as an EntityLiving: canBePushed / runs its own loop while
 * not marked dead -- alive, or dead for at most 20 ticks [EntityLiving.java:155-159
 * death counter advances; removal follows tick 20], see death_ticks. */
static bool entity_push_player_living(const struct server_player* p) {
	return p->has_pos && p->finished_loading
		&& (p->health > 0 || p->death_ticks <= 20);
}

/* Entity.java:669 excludes both directions of the rider/mount relationship
 * for a player body and a pig it rides (EntityPig.interact -> mountEntity:
 * the pig's riddenByEntity is the player, the player's ridingEntity the pig)
 * -- neither direction pushes. v4: any present body, not only the host. */
static bool entity_push_pig_ridden_by(struct server_local* s,
									  const struct entity* pig,
									  const struct server_player* p) {
	return p && pig->type == ENTITY_MONSTER
		&& pig->data.monster.id == MONSTER_PIG && pig->data.monster.pig_ridden
		&& server_local_actor_resolve(s, pig->data.monster.pig_rider_actor) == p;
}

/* The candidate walk shared by every mover: Beta's chunk/section span of the
 * expanded box [World.java:2009-2012: chunk bounds extend two blocks outward
 * before division by sixteen and flooring; Chunk.java:485-493: the same
 * vertical calculation is clamped to the section array], then every resident entity of that span in Beta's traversal
 * order (server_local_prepare_entity_query_snapshot_bounded: chunk X, chunk Z,
 * section, list order -- the exhaustive, ordered reference every local
 * broad-phase must reproduce), filtered by strict box intersection.
 * `mover_entity` (a mob) or `mover_player` (a player body; its own half is
 * added to *mover_mx / *mover_mz) is the mover -- exactly one of the two. */
static void entity_push_run_loop(struct server_local* s,
								 struct server_world* w,
								 const struct entity_push_body* mover_body,
								 struct entity* mover_entity,
								 const struct server_player* mover_player,
								 double* mover_mx, double* mover_mz) {
	const double ex = (double)0.2F;
	const double min_x = mover_body->min_x - ex, max_x = mover_body->max_x + ex;
	const double min_z = mover_body->min_z - ex, max_z = mover_body->max_z + ex;
	const double min_y = mover_body->min_y, max_y = mover_body->max_y;

	const int32_t cx0 = (int32_t)floor((min_x - 2.0) / 16.0);
	const int32_t cx1 = (int32_t)floor((max_x + 2.0) / 16.0);
	const int32_t cz0 = (int32_t)floor((min_z - 2.0) / 16.0);
	const int32_t cz1 = (int32_t)floor((max_z + 2.0) / 16.0);
	int32_t cy0 = (int32_t)floor((min_y - 2.0) / 16.0);
	int32_t cy1 = (int32_t)floor((max_y + 2.0) / 16.0);
	const int32_t cy_max = WORLD_HEIGHT / CHUNK_SIZE - 1;
	if(cy0 < 0)
		cy0 = 0;
	if(cy1 > cy_max)
		cy1 = cy_max;

	/* An allocation failure here is the world's reservation discipline
	 * (server_local_entity_query_reserve): the snapshot reports zero
	 * candidates and the pass is skipped for this mover -- never a truncated
	 * list. The buffer is sized to the whole dict once and then stays. */
	/* P4b: the LOCAL section-list query (Astra item 13); the exhaustive
	 * ordered snapshot is its equivalence reference, not the hot path. */
	if(!server_local_entity_query_local_w(s, w, cx0, cz0, cx1, cz1, cy0, cy1))
		return;
	size_t count = 0;
	const struct server_entity_query_ref* refs
		= server_local_entity_query_snapshot(s, &count);

	for(size_t i = 0; i < count; i++) {
		const struct server_entity_query_ref* r = &refs[i];
		struct entity_push_body pushed;
		struct entity* pe = NULL;
		struct server_player* pp = NULL;
		if(r->player) {
			/* v5 (ASTRA-V4-REVIEW guest query membership): the host OR the
			 * admitted guest body, each by its own section membership. A
			 * body never pushes itself (the exclusion in
			 * getEntitiesWithinAABBExcludingEntity). */
			pp = server_local_query_ref_actor(s, r);
			if(!pp || pp == mover_player || !entity_push_player_living(pp))
				continue;
			/* Entity.java:669: a pig ridden by the player never pushes it */
			if(mover_entity
			   && entity_push_pig_ridden_by(s, mover_entity, pp))
				continue;
			entity_push_body_of_player(pp, &pushed);
		} else {
			pe = r->entity;
			if(!pe || pe == mover_entity || !entity_push_can_be_pushed(pe))
				continue;
			/* Exclude both directions of the rider/mount relationship
			 * [Entity.java:669]: a mount never pushes its rider nor a rider
			 * its mount (spider jockeys; the player and its saddled pig). */
			if(mover_entity && mover_entity->type == ENTITY_MONSTER
			   && (mover_entity->data.monster.jockey_rider_id == pe->id
				   || mover_entity->data.monster.jockey_mount_id == pe->id))
				continue;
			if(entity_push_pig_ridden_by(s, pe, mover_player))
				continue;
			entity_push_body_of_entity(pe, &pushed);
		}
		/* Positions and boxes are read LIVE per candidate (Chunk.java:500
		 * reads boundingBox at query time; earlier pushes this tick have
		 * already moved nothing -- only motion changes -- but a candidate
		 * pushed by an earlier mover this tick carries that motion). */
		if(!entity_push_boxes_intersect(&pushed, min_x, min_y, min_z, max_x,
										max_y, max_z))
			continue;
		double dx, dz;
		if(!entity_collision_impulse(pushed.x, pushed.z, mover_body->x,
									 mover_body->z, &dx, &dz))
			continue;
		if(pe && pe->type == ENTITY_MONSTER) {
			/* Mob motion units S1: a mob's motion is written only through
			 * the mutation API (mob_motion.h); inactive, the identical
			 * float write. */
			mob_motion_add_velocity(pe, &pe->data.monster.motion_d, -dx, 0.0,
									-dz);
		} else if(pe) {
			pe->vel[0] = (float)((double)pe->vel[0] - dx);
			pe->vel[2] = (float)((double)pe->vel[2] - dz);
		} else {
			/* G8-BLAST-VELOCITY v3 (ASTRA-V2-REVIEW (b)): the player body's
			 * half (Entity.java:685 addVelocity on the receiving body) lands on its SERVER
			 * motion (EntityPlayerMP.motionX/Z, double) -- never sent by
			 * itself (addVelocity sets no beenAttacked), but the next
			 * tracker send carries it, as Beta's does. v5: host or guest. */
			pp->vel_x -= dx;
			pp->vel_z -= dz;
		}
		if(mover_entity && mover_entity->type == ENTITY_MONSTER) {
			mob_motion_add_velocity(mover_entity,
									&mover_entity->data.monster.motion_d, dx,
									0.0, dz);
		} else if(mover_entity) {
			mover_entity->vel[0] = (float)((double)mover_entity->vel[0] + dx);
			mover_entity->vel[2] = (float)((double)mover_entity->vel[2] + dz);
		} else if(mover_player) {
			/* The mover player's half (EntityLiving.onLivingUpdate:623-629 calls
			 * the neighbour collision handler, which adds the receiving impulse,
			 * Entity.java:685) onto the motion its packet is evolving. */
			*mover_mx += dx;
			*mover_mz += dz;
		}
	}
}

void entity_living_collide_with_nearby(struct entity* mover,
									   struct server_local* s) {
	assert(mover && s && mover->on_server);
	/* A2-E: the candidates are the mover's OWN world's section lists; a mover
	 * whose world no longer resolves (retired generation) pushes nothing. */
	struct server_world* w = server_local_entity_world(s, mover);
	if(!w)
		return;
	struct entity_push_body body;
	entity_push_body_of_entity(mover, &body);
	entity_push_run_loop(s, w, &body, mover, NULL, NULL, NULL);
}

void entity_actor_collide_with_nearby(struct server_local* s,
	struct server_world* w, const struct server_player* mover, double min_x,
	double min_y, double min_z, double max_x, double max_y, double max_z,
	double* mx, double* mz) {
	assert(s && w && mover && mx && mz);
	/* Entity.moveEntity:394-396 leaves posX/posZ at the box centre. */
	struct entity_push_body body = {
		.x = (min_x + max_x) / 2.0,
		.z = (min_z + max_z) / 2.0,
		.min_x = min_x, .min_y = min_y, .min_z = min_z,
		.max_x = max_x, .max_y = max_y, .max_z = max_z,
	};
	entity_push_run_loop(s, w, &body, NULL, mover, mx, mz);
}

bool entity_handle_water_movement(struct entity* e, double pos_x,
								  double box_min_y, double pos_z, float width,
								  float height) {
	/* Entity.setPosition [Entity.java]: half-width is rounded in float before
	 * conversion to double; the box top adds the double-promoted height.
	 * handleWaterMovement contracts the vertical bounds by float-promoted 0.4
	 * [AxisAlignedBB.java:90-98: expansion moves the bounds outward], then
	 * contracts every face by 0.001 [:210-218: func_28195_e]. */
	const double half = (double)(width / 2.0F);
	const double shrink_y = (double)-0.4F;
	const double eps = 0.001;
	return entity_water_acceleration(
		e, pos_x - half + eps, box_min_y - shrink_y + eps, pos_z - half + eps,
		pos_x + half - eps, box_min_y + (double)height + shrink_y - eps,
		pos_z + half - eps);
}

void entity_shadow(struct entity* e, struct AABB* a, mat4 view) {
	assert(e && a && view);

	// misc/shadow.png is absent on a default install (legal pack ships no
	// shadow yet) -- without it this whole pass would just bind a missing
	// texture and tint ground quads with whatever texture is stale in the
	// slot. Skip entirely; the renderer self-heals when a pack supplies it.
	if(!texture_shadow.data)
		return;

	// A degenerate box has no shadow to project: du/dv below are
	// 1/(x2-x1) and 1/(z2-z1), so a zero-width/zero-depth AABB would put
	// inf/NaN into the shadow quad's UVs (bug hunt B5 -- the only known
	// consumer a degenerate box could reach, and the only known source of
	// the zero-extent boxes that can break aabb_intersection_ray's slab
	// test). Refuse it here so the division is structurally safe for any
	// future caller too. All current callers pass a fixed 0.25 cube.
	if(a->x2 - a->x1 <= 0.0F || a->z2 - a->z1 <= 0.0F)
		return;

	w_coord_t min_x = floorf(a->x1);
	w_coord_t min_y = floorf(a->y1);
	w_coord_t min_z = floorf(a->z1);

	w_coord_t max_x = ceilf(a->x2) + 1;
	w_coord_t max_y = ceilf(a->y2) + 1;
	w_coord_t max_z = ceilf(a->z2) + 1;

	gfx_matrix_modelview(view);
	gfx_blending(MODE_BLEND);
	gfx_alpha_test(false);
	gfx_bind_texture(&texture_shadow);
	gfx_lighting(false);

	float offset = 0.01F;
	float du = 1.0F / (a->x2 - a->x1);
	float dv = 1.0F / (a->z2 - a->z1);

	for(w_coord_t x = min_x; x < max_x; x++) {
		for(w_coord_t z = min_z; z < max_z; z++) {
			for(w_coord_t y = min_y; y < max_y; y++) {
				struct block_data blk;

				if(entity_get_block(e, x, y, z, &blk) && blocks[blk.type]) {
					struct block_info blk_info = (struct block_info) {
						.block = &blk,
						.neighbours = NULL,
						.x = x,
						.y = y,
						.z = z,
						.get_piston_moving = entity_piston_moving_lookup,
						.get_piston_moving_ctx = e,
					};

					size_t count = blocks[blk.type]->getBoundingBox(&blk_info,
																	true, NULL);
					if(count > 0) {
						struct AABB bbox[count];
						blocks[blk.type]->getBoundingBox(&blk_info, true, bbox);

						for(size_t k = 0; k < count; k++) {
							aabb_translate(bbox + k, x, y, z);

							if(a->y2 > bbox[k].y2
							   && aabb_intersection(a, bbox + k)) {
								float u1 = (bbox[k].x1 - a->x1) * du;
								float u2 = (bbox[k].x2 - a->x1) * du;
								float v1 = (bbox[k].z1 - a->z1) * dv;
								float v2 = (bbox[k].z2 - a->z1) * dv;

								gfx_draw_quads_flt(
									4,
									(float[]) {bbox[k].x1, bbox[k].y2 + offset,
											   bbox[k].z1, bbox[k].x2,
											   bbox[k].y2 + offset, bbox[k].z1,
											   bbox[k].x2, bbox[k].y2 + offset,
											   bbox[k].z2, bbox[k].x1,
											   bbox[k].y2 + offset, bbox[k].z2},
									(uint8_t[]) {0xFF, 0xFF, 0xFF, 0x60, 0xFF,
												 0xFF, 0xFF, 0x60, 0xFF, 0xFF,
												 0xFF, 0x60, 0xFF, 0xFF, 0xFF,
												 0x60},
									(float[]) {u1, v1, u2, v1, u2, v2, u1, v2});
							}
						}
					}
				}
			}
		}
	}

	gfx_blending(MODE_OFF);
	gfx_alpha_test(true);
	gfx_lighting(true);
}

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))

bool entity_block_aabb_test(struct AABB* entity, struct block_info* blk_info) {
	assert(entity && blk_info);

	struct block* b = blocks[blk_info->block->type];
	size_t count = b->getBoundingBox(blk_info, true, NULL);

	if(count > 0) {
		struct AABB bbox[count];
		b->getBoundingBox(blk_info, true, bbox);

		for(size_t k = 0; k < count; k++) {
			aabb_translate(bbox + k, blk_info->x, blk_info->y, blk_info->z);

			if(aabb_intersection(entity, bbox + k))
				return true;
		}
	}

	return false;
}

bool entity_intersection(struct entity* e, struct AABB* a,
						 bool (*test)(struct AABB* entity,
									  struct block_info* blk_info)) {
	assert(e && a && test);
	// TODO(perf): chunk-pointer cache for the per-cell entity_get_block fetches in this
	// triple loop (and the arrow/LOS/hazard sweeps). Biggest raw payoff but touches the
	// core collision code; deferred -- not worth the risk for a 0.3ms tick (orchestrator).

	w_coord_t min_x = floorf(a->x1);
	// need to look one further, otherwise fence block breaks
	w_coord_t min_y = max((w_coord_t)(floorf(a->y1) - 1), 0);
	w_coord_t min_z = floorf(a->z1);

	w_coord_t max_x = ceilf(a->x2) + 1;
	// The y-loop below is EXCLUSIVE (`y < max_y`), so the clamp must be
	// WORLD_HEIGHT (one past the last valid index, 127), not WORLD_HEIGHT-1
	// -- the old `- 1` clamp made cells at y=127 unreachable from any AABB
	// whose top already required clamping, silently exempting the top world
	// layer from entity collision.
	w_coord_t max_y = min((w_coord_t)(ceilf(a->y2) + 1), WORLD_HEIGHT);
	w_coord_t max_z = ceilf(a->z2) + 1;

	for(w_coord_t x = min_x; x < max_x; x++) {
		for(w_coord_t z = min_z; z < max_z; z++) {
			for(w_coord_t y = min_y; y < max_y; y++) {
				struct block_data blk;

				if(entity_get_block(e, x, y, z, &blk) && blocks[blk.type]
				   && test(a,
						   &(struct block_info) {.block = &blk,
												 .neighbours = NULL,
											 .x = x,
											 .y = y,
											 .z = z,
											 .get_piston_moving
												 = entity_piston_moving_lookup,
											 .get_piston_moving_ctx = e}))
					return true;
			}
		}
	}

	return false;
}

bool entity_aabb_intersection(struct entity* e, struct AABB* a) {
	return entity_intersection(e, a, entity_block_aabb_test);
}

/* Movement clipping with Beta's PER-BOX rules (entity_clip_axis below). Cell
 * bounds (incl. the fence -1 Y rim and y127) and per-cell multi-box
 * enumeration mirror entity_intersection/entity_block_aabb_test, over the
 * SWEPT box.
 *
 * Both strictnesses are Beta's, verbatim [740c583: AxisAlignedBB.java]:
 *
 *  - STRICT candidate: calculateX/Y/ZOffset only act under STRICT overlap
 *    pre-conditions on the two non-moving axes (both intervals must overlap
 *    strictly, :105-106), and the approach branch on the moving
 *    axis clips TO face contact, never past it -- so a box merely SHARING A
 *    FACE with the mover does not block. This is what lets a mob standing
 *    exactly ON its floor walk horizontally (the floor only ever face-touches
 *    it), and what grounds a falling mover AT the face. The pre-fix code used
 *    the INCLUSIVE test here, which is why every entity had to rest floating
 *    +0.01 above its floor (entity_try_unstuck's lift) to be able to move at
 *    all -- and why a planted mob whose lift was vetoed by a second overlap
 *    (the spider embedded in a trunk) stayed XZ-frozen even once the trunk
 *    itself was excluded.
 *
 *  - STRICT exclusion at old: a box the mover is already INSIDE (strict
 *    overlap accepted by intersectsWith, :185) takes neither calculateOffset
 *    approach branch and never clips -- the embedded entity slides OUT
 *    (VID_20260808_085407's frozen spider), while everything it is NOT
 *    inside still clips: the floor under it still grounds it, the next wall
 *    still stops it, and the box it just escaped becomes an ordinary wall
 *    again the moment the strict overlap ends. */
static inline float entity_aabb_lo(const struct AABB* b, int axis) {
	return axis == 0 ? b->x1 : (axis == 1 ? b->y1 : b->z1);
}

static inline float entity_aabb_hi(const struct AABB* b, int axis) {
	return axis == 0 ? b->x2 : (axis == 1 ? b->y2 : b->z2);
}

// The 5mm embed/flush slack below; see entity_clip_axis.
#define ENTITY_EMBED_SLACK 0.005F

/* Beta Entity.moveEntity single-axis clip [740c583: Entity.java:300-328 +
 * AxisAlignedBB.calculateX/Y/ZOffset, AxisAlignedBB.java:104-195]: every
 * collision box inside the SWEPT volume (old box extended by `d` on `axis`
 * only == boundingBox.addCoord) limits the displacement to face contact, so a
 * fast mover stops at the FIRST face and never skips a block (the old
 * destination-only test + 0.01 bisection tunneled through anything thinner
 * than the step and could settle past the first of two obstacles).
 * Returns the clipped displacement: same sign as `d`, never beyond it, never
 * backwards. *face gets the limiting face coordinate when *limited is set.
 * Single-precision float, not Beta's doubles: no bit-exact Beta claim.
 *
 * Per box, verbatim Beta rules:
 *  - the two fixed axes must overlap STRICTLY (each maximum strictly beyond the opposite minimum), so a
 *    box merely SHARING a face (the floor under a walking mob) never clips;
 *  - approach side only (the old moving face must not already have crossed the obstacle face), so a
 *    box the mover is already inside takes neither branch and never clips --
 *    an embedded entity slides OUT (VID_20260808_085407's frozen spider).
 *
 * ENTITY_EMBED_SLACK preserves the EXISTING float flush-landing safeguard,
 * nothing more. A free move aimed flush at a face can land ~3e-7 INSIDE it
 * (8.5f + 0.2f overshoots 9.0). The previous predicate treated a box as
 * "embedded" only when a 0.005-contracted copy of the old box strictly
 * overlapped it; a shallower overlap still blocked any movement further into
 * the box. Here: a box the contracted core strictly overlaps is skipped
 * (embedded, identical test), and the approach tests accept a moving-axis
 * penetration of up to 0.005 with the gap clamped to 0 -- so a hair-deep box
 * blocks movement into it (displacement 0) exactly as before instead of being
 * ghosted. This is not Beta clipping (Beta's doubles land flush exactly). */
static float entity_clip_axis(struct entity* e, const struct AABB* old,
							  int axis, float d, bool* limited, float* face) {
	*limited = false;
	*face = 0.0F;
	if(d == 0.0F)
		return 0.0F;

	struct AABB core = *old;
	core.x1 += ENTITY_EMBED_SLACK;
	core.y1 += ENTITY_EMBED_SLACK;
	core.z1 += ENTITY_EMBED_SLACK;
	core.x2 -= ENTITY_EMBED_SLACK;
	core.y2 -= ENTITY_EMBED_SLACK;
	core.z2 -= ENTITY_EMBED_SLACK;

	struct AABB swept = *old;
	if(d > 0.0F) {
		if(axis == 0) swept.x2 += d; else if(axis == 1) swept.y2 += d; else swept.z2 += d;
	} else {
		if(axis == 0) swept.x1 += d; else if(axis == 1) swept.y1 += d; else swept.z1 += d;
	}

	const int a1 = (axis + 1) % 3, a2 = (axis + 2) % 3;
	const float old_lo = entity_aabb_lo(old, axis);
	const float old_hi = entity_aabb_hi(old, axis);
	float clipped = d;

	w_coord_t min_x = floorf(swept.x1);
	// need to look one further, otherwise fence block breaks
	w_coord_t min_y = max((w_coord_t)(floorf(swept.y1) - 1), 0);
	w_coord_t min_z = floorf(swept.z1);

	w_coord_t max_x = ceilf(swept.x2) + 1;
	// Same exclusive-bound clamp fix as entity_intersection() above: the
	// y-loop is `y < max_y`, so the clamp must be WORLD_HEIGHT, not
	// WORLD_HEIGHT-1, or y=127 is never enumerated.
	w_coord_t max_y = min((w_coord_t)(ceilf(swept.y2) + 1), WORLD_HEIGHT);
	w_coord_t max_z = ceilf(swept.z2) + 1;

	for(w_coord_t x = min_x; x < max_x; x++) {
		for(w_coord_t z = min_z; z < max_z; z++) {
			for(w_coord_t y = min_y; y < max_y; y++) {
				struct block_data blk;
				if(!entity_get_block(e, x, y, z, &blk) || !blocks[blk.type])
					continue;

				struct block_info bi
					= (struct block_info) {.block = &blk,
										   .neighbours = NULL,
										   .x = x,
										   .y = y,
										   .z = z,
										   .get_piston_moving
										   = entity_piston_moving_lookup,
										   .get_piston_moving_ctx = e};
				struct block* b = blocks[blk.type];
				size_t count = b->getBoundingBox(&bi, true, NULL);
				if(!count)
					continue;

				struct AABB boxes[count];
				b->getBoundingBox(&bi, true, boxes);
				for(size_t k = 0; k < count; k++) {
					struct AABB* bx = boxes + k;
					aabb_translate(bx, x, y, z);
					if(!(entity_aabb_hi(old, a1) > entity_aabb_lo(bx, a1)
						 && entity_aabb_lo(old, a1) < entity_aabb_hi(bx, a1))
					   || !(entity_aabb_hi(old, a2) > entity_aabb_lo(bx, a2)
							&& entity_aabb_lo(old, a2) < entity_aabb_hi(bx, a2)))
						continue; // fixed axes: strict overlap or no clip
					if(aabb_intersection_strict(&core, bx))
						continue; // genuinely embedded: escape freely
					if(d > 0.0F
					   && old_hi <= entity_aabb_lo(bx, axis) + ENTITY_EMBED_SLACK) {
						float gap = entity_aabb_lo(bx, axis) - old_hi;
						if(gap < 0.0F)
							gap = 0.0F;
						if(gap < clipped) {
							clipped = gap;
							*limited = true;
							*face = entity_aabb_lo(bx, axis);
						}
					} else if(d < 0.0F
							  && old_lo
								  >= entity_aabb_hi(bx, axis) - ENTITY_EMBED_SLACK) {
						float gap = entity_aabb_hi(bx, axis) - old_lo;
						if(gap > 0.0F)
							gap = 0.0F;
						if(gap > clipped) {
							clipped = gap;
							*limited = true;
							*face = entity_aabb_hi(bx, axis);
						}
					}
				}
			}
		}
	}

	return clipped;
}

// True if there is a block under the entity's bbox after offsetting it by (dx, dz) and one
// block down -- i.e. stepping (dx,dz) keeps ground beneath the feet. Mirrors Beta's
// nonempty collision-box query after a horizontal offset and a one-block drop.
// `bbox` is the entity LOCAL box (centered, pre-pos), matching the move callers.
// Blocks count only on Beta's STRICT overlap: Block.getCollidingBoundingBoxes admits a
// box on intersectsWith [Block.java:259-263, AxisAlignedBB.java:185-187], so a probe that
// merely shares the top face of a lower step is NOT ground. (With bodies landing exactly
// on the face, b0931e6e, the inclusive test let a sneaking player walk off a one-block
// step-down: the probe's minY equals the lower step's top.)
static bool entity_block_aabb_test_strict(struct AABB* entity,
										  struct block_info* blk_info) {
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

static bool entity_ledge_has_ground(struct entity* e, struct AABB* bbox, float dx,
									float dz) {
	struct AABB probe = *bbox;
	aabb_translate(&probe, e->pos[0] + dx, e->pos[1] - 1.0F, e->pos[2] + dz);
	return entity_intersection(e, &probe, entity_block_aabb_test_strict);
}

void entity_sneak_ledge_clamp(struct entity* e, struct AABB* bbox) {
	assert(e && bbox);
	const float step = 0.05F;

	// MODEL NOTE: Beta's Entity.moveEntity shrinks a COPY of the move delta and
	// leaves motionX/Z untouched -- it does NOT zero the velocity here. The later
	// horizontal velocity reset belongs to WALL clipping after the sneak clamp,
	// after sneaking. This engine folds "velocity" and "per-tick move delta" into one
	// e->vel (the same reason entity_try_move zeroes e->vel on a wall hit == Beta's horizontal motion reset),
	// so the faithful port clamps e->vel directly. The ledge-stop INVARIANT (the bbox never
	// ends over a gap) is identical either way; only the residual momentum model differs, and
	// only on the toward-the-drop axis (walking ALONG an edge keeps its momentum -- that axis
	// has ground, so it isn't clamped). isfinite guards keep a corrupt vel from spinning forever.

	// X axis: probe beneath the X-shifted box. Reduce e->vel[0] toward 0 while the destination
	// has no ground beneath it; stop once a step would still have ground (or vel hits 0).
	while(isfinite(e->vel[0]) && e->vel[0] != 0.0F
		  && !entity_ledge_has_ground(e, bbox, e->vel[0], 0.0F)) {
		if(e->vel[0] < step && e->vel[0] >= -step)
			e->vel[0] = 0.0F;
		else if(e->vel[0] > 0.0F)
			e->vel[0] -= step;
		else
			e->vel[0] += step;
	}

	// Z axis: probe one block beneath the Z-shifted box.
	while(isfinite(e->vel[2]) && e->vel[2] != 0.0F
		  && !entity_ledge_has_ground(e, bbox, 0.0F, e->vel[2])) {
		if(e->vel[2] < step && e->vel[2] >= -step)
			e->vel[2] = 0.0F;
		else if(e->vel[2] > 0.0F)
			e->vel[2] -= step;
		else
			e->vel[2] += step;
	}
}

void entity_try_move(struct entity* e, vec3 pos, vec3 vel, struct AABB* bbox,
					 size_t coord, bool* collision_xz, bool* on_ground) {
	assert(e && pos && vel && bbox && collision_xz && on_ground);

	const float d = vel[coord];
	/* C safety guard (no Beta claim): check the moving velocity component and
	 * position coordinate before collision arithmetic. Skip this move; zero a
	 * non-finite velocity component so it cannot re-enter next tick. Collision
	 * flags are left untouched. */
	if(!isfinite(d) || !isfinite(pos[coord])) {
		if(!isfinite(d))
			vel[coord] = 0.0F;
		return;
	}

	struct AABB box_old = *bbox;
	aabb_translate(&box_old, pos[0], pos[1], pos[2]);
	bool limited;
	float face;
	const float clipped
		= entity_clip_axis(e, &box_old, (int)coord, d, &limited, &face);

	if(clipped != d) {
		if(coord == 1)
			*on_ground = d < 0.0F;
		if(coord == 0 || coord == 2)
			*collision_xz = true;

		vel[coord] = 0.0F;
	} else if(coord == 1) {
		*on_ground = false;
	}

	if(clipped == d) {
		// Unblocked (incl. d == 0): pos + vel, the same float result as the
		// former pos*(1-1) + (pos+vel)*1.
		pos[coord] += d;
		return;
	}
	if(clipped == 0.0F)
		return; // blocked at contact: position unchanged, as before

	/* Place the body's moving face directly on the limiting block face. Adding
	 * a gap to a distant origin can lose the low bits (E8: -18 + 17.7), so
	 * adjust the represented position instead of iterating that sum. In this
	 * float model, subtracting a local face offset rounds by at most half a
	 * position ulp; if reconstruction crosses the block face, one outward
	 * nextafterf reaches the safe side. Beta uses doubles, so this is a float
	 * representation choice, not a claim of full Java-double equivalence. */
	const float lo = entity_aabb_lo(bbox, (int)coord);
	const float hi = entity_aabb_hi(bbox, (int)coord);
	assert(limited);
	float np = face - (d > 0.0F ? hi : lo);
	if(d > 0.0F ? np + hi > face : np + lo < face)
		np = nextafterf(np, d > 0.0F ? -INFINITY : INFINITY);
	/* Face arithmetic must not move backward or exceed the requested travel. */
	const float requested = pos[coord] + d;
	if(d > 0.0F) {
		if(np < pos[coord]) np = pos[coord];
		if(np > requested) np = requested;
	} else {
		if(np > pos[coord]) np = pos[coord];
		if(np < requested) np = requested;
	}
	pos[coord] = np;
}

// O(N) full-dict max-key scan, returning max+1 (id 0 reserved for the local player).
// HARDEN AUDIT #3: the server hot path no longer calls this -- it uses the O(1)
// monotonic server_local_gen_id() (a never-reused running counter) instead, because
// this scanned the whole entity dict on EVERY spawn. Kept here for the generic
// engine API (declared in entity.h) in case a non-server caller ever needs a
// dict-derived id; it is correct but should not be used on a per-tick spawn path.
uint32_t entity_gen_id(dict_entity_t dict) {
	assert(dict);

	dict_entity_it_t it;
	dict_entity_it(it, dict);

	// id = 0 is reserved for local player

	uint32_t id = 0;

	while(!dict_entity_end_p(it)) {
		uint32_t key = dict_entity_ref(it)->key;

		if(key > id)
			id = key;

		dict_entity_next(it);
	}

	return id + 1;
}

void entities_client_tick(dict_entity_t dict) {
    dict_entity_it_t it;
    dict_entity_it(it, dict);

    // Erase-safe walk: m-dict.h's open-addressing _erase() can trigger a
    // resize-DOWN that reshuffles the whole backing array (m-dict.h:851-857,
    // the IT_REMOVE note: "the dict may perform a resize down operation ...
    // This operation renders the computation of the 'next' element
    // impossible"). Calling dict_entity_next() before dict_entity_erase()
    // (the previous code) does not protect against this -- the iterator was
    // already advanced against the OLD array layout, so a resize invalidates
    // it regardless of erase/next order. This can skip live entities or
    // double-tick a relocated one. Keep expired entities alive and reachable
    // until the walk ends, since a later tick may scan the dictionary. Collect
    // keys first, then erase and free each entity in a second pass.
    size_t max_erase = dict_entity_size(dict);
    uint32_t stack_buf[32];
    uint32_t* to_erase = stack_buf;
    if (max_erase > 32) {
        to_erase = malloc(max_erase * sizeof(uint32_t));
        if (!to_erase) {
            fprintf(stderr,
                    "[entity] WARNING: malloc(%zu) failed in "
                    "entities_client_tick; skipping expired-entity cleanup "
                    "this tick (will retry next tick).\n",
                    max_erase * sizeof(uint32_t));
            return;
        }
    }
    size_t erase_count = 0;

    while (!dict_entity_end_p(it)) {
        uint32_t key = dict_entity_ref(it)->key;
        struct entity *e = dict_entity_ref(it)->value;

        if (e->tick_client) {
            bool remove = (e->delay_destroy == 0) || e->tick_client(e);

            if (remove) {
                assert(erase_count < max_erase);
                to_erase[erase_count++] = key;
            }
        }

        dict_entity_next(it);
    }

    for (size_t i = 0; i < erase_count; i++) {
        struct entity **slot = dict_entity_get(dict, to_erase[i]);
        assert(slot && *slot);
        struct entity *e = *slot;
        dict_entity_erase(dict, to_erase[i]);
        free(e);
    }

    if (to_erase != stack_buf) {
        free(to_erase);
    }
}

void entities_client_render(dict_entity_t dict, struct camera* c,
							float tick_delta) {
	dict_entity_it_t it;
	dict_entity_it(it, dict);

	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		// ENTITY_REMOTE_PLAYER renders out to Beta's own 64-block player
		// render distance [740c583 RenderManager render distance for
		// players -- the same cap render_name_tag.h's NAME_TAG_RENDER_
		// DISTANCE cites for the name tag above the model], not the tighter
		// 32-block generic entity cull below -- otherwise the model
		// disappears at 32 blocks while its name tag (a separate, camera-
		// driven pass) keeps rendering out to 64, a model/tag mismatch Beta
		// never has.
		float render_cap2 = e->type == ENTITY_REMOTE_PLAYER ? glm_pow2(64.0F)
															 : glm_pow2(32.0F);
		// Beta Entity.isInRangeToRenderDist: ghast's 4x4 bounds * 64
		// = 256 blocks. The generic 32-block cap hid ghasts inside their 64-block
		// attack range. Keep the camera far plane and terrain depth test;
		// only ghasts bypass the generic entity cutoff.
		if(e->type == ENTITY_MONSTER && e->data.monster.id == MONSTER_GHAST)
			render_cap2 = glm_pow2(256.0F);
		/* [740c583 EntityFireball.isInRangeToRenderDist]: one-block bounds
		 * times 4 times 64. Snowballs share the icon, not this render range. */
		if(e->type == ENTITY_ARROW && e->data.arrow.fireball)
			render_cap2 = glm_pow2(256.0F);
		if(e->render
		   && glm_vec3_distance2(e->pos, (vec3) {c->x, c->y, c->z})
			   < render_cap2) {
			// GP-wedge breadcrumb: name THIS entity before its draw enters the
			// FIFO (type<<8 | monster id in aux) -- see watchdog.h.
			watchdog_draw_breadcrumb(
				WDB_ENTITY, e->id,
				((uint32_t)e->type << 8)
					| (e->type == ENTITY_MONSTER
						   ? (uint32_t)(e->data.monster.id & 0xFF)
						   : 0u));
			if(e->type == ENTITY_ARROW && e->data.arrow.fireball) {
				entity_fireball_render(e, c, tick_delta);
				/* [740c583 EntityFireball.onUpdate] sets the fire counter to ten each tick;
				 * RenderManager draws the ordinary entity fire overlay. Keep
				 * this display-only so client physics and snowballs stay intact. */
				struct entity display = *e;
				display.on_fire = true;
				display.width = display.height = 1.0f;
				display.y_offset = display.y_size = 0.0f;
				render_entity_fire_draw(&display, c, tick_delta);
			} else if(e->type == ENTITY_ARROW
					  && (e->data.arrow.item_id == ITEM_EGG
						  || e->data.arrow.item_id == ITEM_SNOW_BALL)) {
				entity_thrown_render(e, c, tick_delta);
				if(e->on_fire)
					render_entity_fire_draw(e, c, tick_delta);
			} else {
				e->render(e, c->view, tick_delta);
				if(e->on_fire && e->type != ENTITY_LOCAL_PLAYER)
					render_entity_fire_draw(e, c, tick_delta);
			}
		}
		dict_entity_next(it);
	}
}

bool entity_aabb_intersect_ray(const vec3 origin,
                               const vec3 dir,
                               const struct entity *e,
                               float *out_t)
{
    assert(origin && dir && e && out_t);

    // 1) Vraag de AABB van de entity op via de functiepointer -- EXCEPT
    // ENTITY_PAINTING (post-review finding #1, adversarial review
    // 2026-08-13). Beta's EntityHanging/EntityPainting keeps click/attack
    // targeting on its own always-present PHYSICAL boundingBox field -- a
    // DIFFERENT Java member from the nullable Entity.getBoundingBox()
    // accessor, which is used only for the solid/pushable-object concept
    // (block_place_obstructed, the piston push sweep -- see
    // entity_painting.c's entity_painting(), which correctly nulls
    // e->getBoundingBox to model THAT meaning, post-review findings #1/#2
    // of the FIRST review round). This engine has one function-pointer slot
    // where Beta has two independent fields; recompute the click-target box
    // directly from the painting's own placement fields here instead of
    // resurrecting a non-null getBoundingBox, which would wrongly re-expose
    // a live painting to those other, unrelated consumers. Identical
    // formula entity_painting_validate_surface's own checks already use --
    // single source of truth for "where is this painting's frame."
    struct AABB box;
    if(e->type == ENTITY_PAINTING) {
        painting_compute_geometry(e->data.painting.art_index,
            e->data.painting.direction, e->data.painting.tile_x,
            e->data.painting.tile_y, e->data.painting.tile_z, NULL, &box);
    } else {
        assert(e->getBoundingBox != NULL);
        if(e->getBoundingBox(e, &box) == 0)
            return false;
    }

    // 2) Voer de slab‐methode uit op box.x1..box.z2
    float invDx = 1.0f / (fabsf(dir[0]) > GLM_FLT_EPSILON
                          ? dir[0]
                          : copysignf(GLM_FLT_EPSILON, dir[0]));
    float invDy = 1.0f / (fabsf(dir[1]) > GLM_FLT_EPSILON
                          ? dir[1]
                          : copysignf(GLM_FLT_EPSILON, dir[1]));
    float invDz = 1.0f / (fabsf(dir[2]) > GLM_FLT_EPSILON
                          ? dir[2]
                          : copysignf(GLM_FLT_EPSILON, dir[2]));

    float t1 = (box.x1 - origin[0]) * invDx;
    float t2 = (box.x2 - origin[0]) * invDx;
    float t3 = (box.y1 - origin[1]) * invDy;
    float t4 = (box.y2 - origin[1]) * invDy;
    float t5 = (box.z1 - origin[2]) * invDz;
    float t6 = (box.z2 - origin[2]) * invDz;

    float tmin = fmaxf(fmaxf(fminf(t1, t2), fminf(t3, t4)), fminf(t5, t6));
    float tmax = fminf(fminf(fmaxf(t1, t2), fmaxf(t3, t4)), fmaxf(t5, t6));

    if (tmax < 0.0f || tmin > tmax) {
        return false;
    }
    *out_t = (tmin >= 0.0f ? tmin : tmax);
    return true;
}



//-----------------------------------------------------------------------------
// Raycasts against all entities stored in dict_entity_t. Returns the closest
// hit entity within maxDist; *out_tNear is set to that hit distance.
// Returns NULL if no entity is hit.
//-----------------------------------------------------------------------------
struct entity *
raycast_entity(dict_entity_t *entities,
               const vec3 origin,
               const vec3 dir,
               float maxDist,
               float *out_tNear)
{
    struct entity *closest = NULL;
    float closestT = maxDist + 1.0f;

    // Use the dict_entity iterator API to loop through all entities
    dict_entity_it_t it;
    dict_entity_it(it, *entities);

    while (!dict_entity_end_p(it)) {
        struct entity *e = dict_entity_ref(it)->value;

		// ENTITY_PAINTING is exempt from the getBoundingBox gate (post-review
		// finding #1): entity_aabb_intersect_ray above supplies its
		// click-target box directly for that one type, so a null
		// getBoundingBox (correct for painting -- see entity_painting.c's
		// entity_painting()) must not skip it here the way it correctly
		// does for every other entity kind (a real null getBoundingBox on
		// any OTHER type means "this entity has no box at all," which IS a
		// legitimate raycast skip).
		if (!e || e->type == ENTITY_LOCAL_PLAYER || e->id == 0
		   || (!e->getBoundingBox && e->type != ENTITY_PAINTING)) {
            dict_entity_next(it);
            continue;
        }

        float tHit;
        if (entity_aabb_intersect_ray(origin, dir, e, &tHit)) {
            if (tHit >= 0.0f && tHit < closestT && tHit <= maxDist) {
                closestT = tHit;
                closest  = e;
            }
        }

        dict_entity_next(it);
    }

    if (closest && out_tNear) {
        *out_tNear = closestT;
    }
    return closest;
}


void entity_move_in_direction(struct entity* e, float accel, vec2 dir) {
    e->vel[0] += accel * dir[0];
    e->vel[2] += accel * dir[1];
}

void entity_clamp_speed(struct entity* e, float max_speed) {
    float vx = e->vel[0], vz = e->vel[2];
    float mag = sqrtf(vx * vx + vz * vz);
    if (mag > max_speed) {
        float scale = max_speed / mag;
        e->vel[0] *= scale;
        e->vel[2] *= scale;
    }
}

void entity_apply_gravity(struct entity* e, float gravity) {
    e->vel[1] -= gravity;
    e->on_ground = false;
}

void entity_apply_friction(struct entity* e, float slip) {
    e->vel[0] *= slip * 0.8f;
    e->vel[2] *= slip * 0.8f;
    e->vel[1] *= 0.9f;
}

void entity_apply_gravity_and_friction(struct entity* e, float gravity, float slip_ground, float slip_air) {
    e->vel[1] -= gravity;
    float slip = e->on_ground ? slip_ground : slip_air;
    e->vel[0] *= slip * 0.8f;
    e->vel[2] *= slip * 0.8f;
    e->vel[1] *= 0.9f;
    e->on_ground = false;
}

bool entity_try_auto_jump(struct entity* e, float jump_force, float threshold) {
    float dx = e->vel[0];
    float dz = e->vel[2];
    float mag = sqrtf(dx * dx + dz * dz);
    if (mag < threshold) return false;

    dx /= mag;
    dz /= mag;

    int tx = (int)floorf(e->pos[0] + dx);
    /* + y_size: the physics box sits at pos + y_size (entity_try_move_axis).
     * While a ySize ease is draining, raw pos is up to a block LOW -- probing
     * that row reads the step just climbed as "blocked ahead" and re-fires the
     * jump. Beta's own block probes use the bbox, which carries ySize. */
    int ty = (int)floorf(e->pos[1] + e->y_size);
    int tz = (int)floorf(e->pos[2] + dz);

    struct block_data blk1, blk2;
    bool has_front = entity_get_block(e, tx, ty,     tz, &blk1);
    bool has_top   = entity_get_block(e, tx, ty + 1, tz, &blk2);

    // Air (blocks[0]) and any unregistered id have NO block struct, so blocks[type]
    // is NULL -- treat that as see-through/passable. The probe legitimately hits air
    // ABOVE a 1-block step on essentially every real auto-jump (and air AHEAD whenever
    // the obstacle is off-axis), so dereferencing blocks[type]->can_see_through raw
    // here is a NULL read that crashes the instant any mob tries to hop an obstacle
    // (or another mob it is colliding with). NULL ahead -> not blocked; NULL above ->
    // space to jump into. Matches the blocks[type] NULL guards in the collision +
    // random-tick paths.
    bool blocked_ahead = has_front && blocks[blk1.type]
        && !blocks[blk1.type]->can_see_through;
    bool space_above   = !has_top || !blocks[blk2.type]
        || blocks[blk2.type]->can_see_through;

    if (blocked_ahead && space_above) {
        e->vel[1] = jump_force;
        return true;
    }
    return false;
}


void entity_try_unstuck(struct entity* e, void (*make_bbox)(struct AABB*)) {
    struct AABB b0, b1;
    make_bbox(&b0);
	aabb_translate(&b0, e->pos[0], e->pos[1] + e->y_size, e->pos[2]);
    b1 = b0;
    aabb_translate(&b1, 0.0f, 0.01f, 0.0f);

    if (entity_aabb_intersection(e, &b0) && !entity_aabb_intersection(e, &b1)) {
        e->pos[1] += 0.01f;
    }
}

void entity_try_move_axis(struct entity* e, int axis, struct AABB (*make_bbox)(void), bool* collision_flag, bool* on_ground_flag) {
    struct AABB bb = make_bbox();
	aabb_translate(&bb, 0.0F, e->y_size, 0.0F);
    entity_try_move(e, e->pos, e->vel, &bb, axis, collision_flag, on_ground_flag);
    if (axis == 1 && *on_ground_flag) {
        e->vel[1] = 0.0f;
    }
}

void entity_damp_velocity(struct entity* e, float threshold) {
    if (fabsf(e->vel[0]) < threshold) e->vel[0] = 0.0f;
    if (fabsf(e->vel[2]) < threshold) e->vel[2] = 0.0f;
// don't touch Y unless we really need it
    }

void entity_get_delta(struct entity* e, vec3 out_delta) {
    glm_vec3_sub(e->pos, e->pos_old, out_delta);
}

float entity_get_horizontal_speed(vec3 delta) {
    return sqrtf(delta[0]*delta[0] + delta[2]*delta[2]);
}


void entity_blend_body_to_head(float* body_yaw, float head_yaw, float factor) {
    float diff = head_yaw - *body_yaw;
    while (diff >  M_PI) diff -= 2.0f * M_PI;
    while (diff < -M_PI) diff += 2.0f * M_PI;
    *body_yaw += diff * factor;
}

void entity_tick_animation(struct entity* e, float walk_speed, int max_frame) {
    /* Monster-only by construction: the sole caller is client_tick_monster
     * (entity_monster.c), dispatched only when e->type == ENTITY_MONSTER,
     * and data.monster is the union member monsters construct. The assert
     * pins that invariant -- a new caller on another entity type would
     * corrupt whatever union member that entity actually owns (bug hunt
     * Q3; reachability verified 2026-08-05). */
    assert(e && e->type == ENTITY_MONSTER && max_frame > 0);
    if (walk_speed > 0.001f) {
        e->data.monster.frame_time_left--;
        if (e->data.monster.frame_time_left <= 0) {
            e->data.monster.frame = (e->data.monster.frame + 1) % max_frame;
            e->data.monster.frame_time_left = 1;
        }
    }
}
