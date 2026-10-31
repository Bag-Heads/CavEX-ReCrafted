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

#include <assert.h>
#include <stdlib.h>

#include "game/game_state.h"
#include "entity/monsters_object.h"
#include "graphics/render_block.h"
#include "particle.h"
#include "platform/gfx.h"
#include "platform/mem2.h"
#include "graphics/texture_atlas.h"
#include "platform/texture.h"


#define PARTICLES_AREA 8
#define PARTICLES_VOLUME 64
#define PARTICLE_LAYER_LIMIT 4000

struct particle_ring {
	struct particle* data;
	size_t capacity;
	size_t count;
	size_t head;
};

/* Split-dims B2: one particle SYSTEM per client world payload (the two
 * atlas layers, the spawn-cull camera and the world whose light/collision
 * the particles read). Every legacy particle_* entry point operates on the
 * SELECTED system; the primary system is selected unless the client owner
 * explicitly selects another around one world's work, and it is bound to
 * gstate.world, so a process that never creates a second system behaves
 * exactly as the old file-scope rings did (same storage layout per layer,
 * same RNG draws, same world reads). */
struct particle_system {
	struct particle_ring layers[2];
#ifdef CAVEWII_PARTICLE_TEST
	size_t evictions[2];
#endif
	vec3 camera;
	struct world* light_world;
};

static struct particle_system s_particle_primary = {.light_world = &gstate.world};
static struct particle_system* s_particles = &s_particle_primary;

static struct particle* particle_ring_at(struct particle_ring* ring,
										 size_t logical_index) {
	assert(ring && logical_index < ring->count);
	return &ring->data[(ring->head + logical_index) % ring->count];
}

static bool particle_ring_grow(struct particle_ring* ring, size_t needed) {
	assert(ring && needed <= PARTICLE_LAYER_LIMIT);
	if(needed <= ring->capacity)
		return true;

	size_t capacity = ring->capacity ? ring->capacity * 2 : 128;
	if(capacity < needed)
		capacity = needed;
	if(capacity > PARTICLE_LAYER_LIMIT)
		capacity = PARTICLE_LAYER_LIMIT;
	void* grown = mem2_realloc(
		ring->data, ring->capacity * sizeof(*ring->data),
		capacity * sizeof(*ring->data));
	if(!grown)
		return false;
	ring->data = grown;
	ring->capacity = capacity;
	return true;
}

static struct particle* particle_ring_append(struct particle_ring* ring,
										 particle_atlas_t atlas) {
	assert(ring && (atlas == TEXTURE_ATLAS_TERRAIN
					|| atlas == TEXTURE_ATLAS_PARTICLES));
	if(ring->count < PARTICLE_LAYER_LIMIT) {
		/* head can become non-zero only while the ring is full. update() always
		 * linearizes before compacting below the cap. */
		assert(ring->head == 0);
		if(!particle_ring_grow(ring, ring->count + 1))
			return NULL;
		return &ring->data[ring->count++];
	}

	/* Beta EffectRenderer removes layer index zero before appending. A ring
	 * overwrite is the same logical eviction/append in O(1), without shifting
	 * up to 3,999 particle structs for every TNT smoke puff. */
	struct particle* out = &ring->data[ring->head];
	ring->head = (ring->head + 1) % ring->count;
#ifdef CAVEWII_PARTICLE_TEST
	s_particles->evictions[atlas]++;
#endif
	return out;
}

// M5: drop every live particle. Called by the client's CRPC_WORLD_RESET so a
// live dimension switch can't briefly render old-world particles at
// overlapping coordinates in the new world (memory-safe before, cosmetically
// wrong; the menu path's seconds of latency just hid it).
void particle_clear_all(void) {
	for(size_t i = 0; i < 2; i++) {
		s_particles->layers[i].count = 0;
		s_particles->layers[i].head = 0;
#ifdef CAVEWII_PARTICLE_TEST
		s_particles->evictions[i] = 0;
#endif
	}
}

static const float SPAWN_CULL_RADIUS = 64.0f;
static const float SPAWN_CULL_RADIUS2 = SPAWN_CULL_RADIUS * SPAWN_CULL_RADIUS;

static const uint8_t redstone_colors[16][3] = {
    {111,   0,  0},  // 0
    {120,   3,  0},  // 1
    {130,   7,  0},  // 2
    {139,  10,  0},  // 3
    {149,  13,  0},  // 4
    {158,  16,  0},  // 5
    {167,  20,  0},  // 6
    {177,  23,  0},  // 7
    {186,  26,  0},  // 8
    {196,  29,  0},  // 9
    {205,  33,  0},  // 10
    {214,  36,  0},  // 11
    {224,  39,  0},  // 12
    {233,  42,  0},  // 13
    {243,  46,  0},  // 14
    {252,  49,  0}   // 15
};


void particle_init() {
	particle_clear_all();
}

void particle_set_camera(vec3 p) {
	glm_vec3_copy(p, s_particles->camera);
}

static float rnd(void) {
    return rand_gen_flt(&gstate.rand_src);
}

/* drown-fx-0927: particle_add's body, returning the added particle (NULL =
 * culled or refused) so a caller can set per-type motion. */
static struct particle* particle_add_p(vec3 pos,
                  vec3 vel,
                  uint8_t tex,
                  float size,
                  float lifetime,
                  bool gravity,
                  uint8_t r,
                  uint8_t g,
                  uint8_t b,
				  bool ignore_light,
                  particle_atlas_t atlas)
{
	// skip any new particle > spawn radius from camera
    if (glm_vec3_distance2(pos, s_particles->camera) > SPAWN_CULL_RADIUS2) {
        return NULL;
    }
	if(atlas != TEXTURE_ATLAS_TERRAIN
	   && atlas != TEXTURE_ATLAS_PARTICLES)
		return NULL;

    struct particle *p = particle_ring_append(&s_particles->layers[atlas], atlas);
    if (!p) return NULL;

    glm_vec3_copy(pos,    p->pos);
    glm_vec3_copy(pos,    p->pos_old);
    glm_vec3_copy(vel,    p->vel);

    p->tex      = tex;
    p->size     = size;
    p->age      = (int)lifetime;
    p->lifetime = (int)lifetime;
    p->gravity  = gravity;
    p->r = r;  p->g = g;  p->b = b;
    p->ignore_light = ignore_light;
    p->atlas    = atlas;
    p->bubble   = false;

    if (atlas == TEXTURE_ATLAS_TERRAIN) {
        float fx = (TEX_OFFSET(TEXTURE_X(tex)) + rnd() * 12.0f) / 256.0f;
        float fy = (TEX_OFFSET(TEXTURE_Y(tex)) + rnd() * 12.0f) / 256.0f;
        p->tex_uv[0] = fx;
        p->tex_uv[1] = fy;
    } else {
        p->tex_uv[0] = p->tex_uv[1] = 0.0f;
    }
    return p;
}

void particle_add(vec3 pos, vec3 vel, uint8_t tex, float size, float lifetime,
				  bool gravity, uint8_t r, uint8_t g, uint8_t b,
				  bool ignore_light, particle_atlas_t atlas) {
	(void)particle_add_p(pos, vel, tex, size, lifetime, gravity, r, g, b,
						 ignore_light, atlas);
}

void particle_generate_block(struct block_info* info) {
    assert(info && info->block && info->neighbours);
    if (!blocks[info->block->type]) return;

    size_t count = blocks[info->block->type]->getBoundingBox(info, false, NULL);
    if (!count) return;

    struct AABB aabb[count];
    blocks[info->block->type]->getBoundingBox(info, false, aabb);

    float volume = (aabb->x2 - aabb->x1)
                 * (aabb->y2 - aabb->y1)
                 * (aabb->z2 - aabb->z1);
    uint8_t tex = blocks[info->block->type]
                  ->getTextureIndex(info, SIDE_FRONT);

    for (int k = 0; k < volume * PARTICLES_VOLUME; k++) {
        float x = rand_gen_flt(&gstate.rand_src) * (aabb->x2 - aabb->x1) + aabb->x1;
        float y = rand_gen_flt(&gstate.rand_src) * (aabb->y2 - aabb->y1) + aabb->y1;
        float z = rand_gen_flt(&gstate.rand_src) * (aabb->z2 - aabb->z1) + aabb->z1;

        vec3 vel = {
            rand_gen_flt(&gstate.rand_src) - 0.5F,
            rand_gen_flt(&gstate.rand_src) - 0.5F,
            rand_gen_flt(&gstate.rand_src) - 0.5F
        };
        glm_vec3_normalize(vel);
        glm_vec3_scale(
            vel,
            (2.0F * rand_gen_flt(&gstate.rand_src) + 0.5F) * 0.05F,
            vel
        );

        vec3 pos = { info->x + x, info->y + y, info->z + z };
        float size = (rand_gen_flt(&gstate.rand_src) + 1.0F) * 0.03125F;
        float age  = 4.0F / (rand_gen_flt(&gstate.rand_src) * 0.9F + 0.1F);

        particle_add(pos, vel, tex, size, age, true, 255,255,255, false, TEXTURE_ATLAS_TERRAIN);
    }
}

void particle_generate_side(struct block_info* info, enum side s) {
    assert(info && info->block && info->neighbours);
    if (!blocks[info->block->type]) return;

    size_t count = blocks[info->block->type]->getBoundingBox(info, false, NULL);
    if (!count) return;

    struct AABB aabb[count];
    blocks[info->block->type]->getBoundingBox(info, false, aabb);

    float area;
    switch (s) {
        case SIDE_RIGHT:
        case SIDE_LEFT:
            area = (aabb->y2 - aabb->y1) * (aabb->z2 - aabb->z1);
            break;
        case SIDE_BOTTOM:
        case SIDE_TOP:
            area = (aabb->x2 - aabb->x1) * (aabb->z2 - aabb->z1);
            break;
        case SIDE_FRONT:
        case SIDE_BACK:
            area = (aabb->x2 - aabb->x1) * (aabb->y2 - aabb->y1);
            break;
        default:
            return;
    }

    uint8_t tex = blocks[info->block->type]
                  ->getTextureIndex(info, s);
    float offset = 0.0625F;

    for (int k = 0; k < area * PARTICLES_AREA; k++) {
        float x = rand_gen_flt(&gstate.rand_src) * (aabb->x2 - aabb->x1) + aabb->x1;
        float y = rand_gen_flt(&gstate.rand_src) * (aabb->y2 - aabb->y1) + aabb->y1;
        float z = rand_gen_flt(&gstate.rand_src) * (aabb->z2 - aabb->z1) + aabb->z1;

        switch (s) {
            case SIDE_LEFT:   x = aabb->x1 - offset; break;
            case SIDE_RIGHT:  x = aabb->x2 + offset; break;
            case SIDE_BOTTOM: y = aabb->y1 - offset; break;
            case SIDE_TOP:    y = aabb->y2 + offset; break;
            case SIDE_FRONT:  z = aabb->z1 - offset; break;
            case SIDE_BACK:   z = aabb->z2 + offset; break;
            default: return;
        }

        vec3 vel = {
            rand_gen_flt(&gstate.rand_src) - 0.5F,
            rand_gen_flt(&gstate.rand_src) - 0.5F,
            rand_gen_flt(&gstate.rand_src) - 0.5F
        };
        glm_vec3_normalize(vel);
        glm_vec3_scale(
            vel,
            (2.0F * rand_gen_flt(&gstate.rand_src) + 0.5F) * 0.05F,
            vel
        );

        vec3 pos = { info->x + x, info->y + y, info->z + z };
        float size = (rand_gen_flt(&gstate.rand_src) + 1.0F) * 0.03125F;
        float age  = 4.0F / (rand_gen_flt(&gstate.rand_src) * 0.9F + 0.1F);

        particle_add(pos, vel, tex, size, age, true, 255,255,255, false, TEXTURE_ATLAS_TERRAIN);
        }
}

void particle_generate_explosion_flash(vec3 center, float intensity) {
    uint8_t tex_smoke_base = tex_atlas_lookup_particle(TEXAT_PARTICLE_EMBER);
    // roughly 12 smoke puffs per unit intensity
    int count = (int)ceilf(intensity * 12.0f);

    for (int i = 0; i < count; i++) {
        vec3 vel = {
            rnd()*2.0f - 1.0f,
            rnd()*2.0f - 1.0f,
            rnd()*2.0f - 1.0f
        };
        glm_vec3_normalize(vel);
        // give them a bit of speed outward
        glm_vec3_scale(vel, (0.3f + rnd()*0.2f) * intensity, vel);

        vec3 pos = {
            center[0] + (rnd()-0.5f)*0.2f,
            center[1] + (rnd()-0.5f)*0.2f,
            center[2] + (rnd()-0.5f)*0.2f
        };

        float size = 0.2f * intensity;

        float life = (8.0f + rnd()*8.0f) * intensity * 0.5f;

        uint8_t brightness = (uint8_t)(rnd() * 150.0f + 50.0f);

        //    no gravity => no damping, so they drift until age runs out
        particle_add(
            pos,
            vel,
            tex_smoke_base,            // start frame for smoke animation
            size,                      // particle size
            life,                      // lifetime in ticks
            false,                     // no gravity (no damping)
            brightness, brightness, brightness, // random gray brightness
			true,
            TEXTURE_ATLAS_PARTICLES    // use particle atlas
        );
    }
}

void particle_generate_redstone_torch(vec3 center) {
    const int count = 3;  // particles per tick
    const uint8_t baseR = 252, baseG = 49, baseB = 0;

    for (int i = 0; i < count; ++i) {
        uint8_t frame     = (uint8_t)(rnd() * 8.0f);
        uint8_t tex_smoke = tex_atlas_lookup_particle(TEXAT_PARTICLE_SMOKE_0 + frame);

        float factor = 0.5f + rnd() * 0.5f;
        uint8_t r = (uint8_t)(baseR * factor);
        uint8_t g = (uint8_t)(baseG * factor);
        uint8_t b = (uint8_t)(baseB * factor);

        vec3 pos = {
            center[0] + (rnd() - 0.5f) * 0.1f,
            center[1],
            center[2] + (rnd() - 0.5f) * 0.1f
        };

        vec3 vel = {
            (rnd() - 0.5f) * 0.01f,
            0.02f           + rnd() * 0.01f,
            (rnd() - 0.5f) * 0.01f
        };

        float size = 0.15f + rnd() * 0.05f;
        float life = 10.0f  + rnd() * 10.0f;

        particle_add(
            pos, vel,
            tex_smoke,
            size, life,
            false,       // no gravity
            r, g, b,     // tinted by brightness factor
			false,		// light
            TEXTURE_ATLAS_PARTICLES
        );
    }
}

void particle_generate_redstone_wire(vec3 center, uint8_t power) {
    if (power == 0) return;               // no puffs at zero

    float strength = (float)power / 15.0f;

    int count = 1 + (int)(3.0f * strength);
    for (int i = 0; i < count; ++i) {
        uint8_t frame     = (uint8_t)(rnd() * 8.0f);
        uint8_t tex_smoke = tex_atlas_lookup_particle(
            TEXAT_PARTICLE_SMOKE_0 + frame
        );

        const uint8_t *baseCol = redstone_colors[power];
        float brightFactor     = 0.6f + rnd() * 0.4f;  // 60–100%
        uint8_t r = (uint8_t)(baseCol[0] * brightFactor);
        uint8_t g = (uint8_t)(baseCol[1] * brightFactor);
        uint8_t b = (uint8_t)(baseCol[2] * brightFactor);

        vec3 pos = {
            center[0] + (rnd() - 0.5f) * 0.12f,
            center[1] + 0.125f + rnd() * 0.03f,
            center[2] + (rnd() - 0.5f) * 0.12f
        };

        float baseVel = 0.008f + 0.012f * strength;
        vec3 vel = {
            (rnd() - 0.5f) * baseVel,
            (0.015f + rnd() * 0.01f) * strength,
            (rnd() - 0.5f) * baseVel
        };

        // size & lifetime: small at low power, bigger at high power
        float size = (0.05f + rnd() * 0.04f) * (0.5f + 0.5f * strength);
        float life = (5.0f  + rnd() * 5.0f)  * (0.5f + 0.5f * strength);

        particle_add(
            pos, vel,
            tex_smoke,
            size, life,
            false,    // no gravity
            r, g, b,  // tint by power
			false,
            TEXTURE_ATLAS_PARTICLES
        );
    }
}

void particle_generate_explosion_smoke(vec3 center, float intensity) {
    // number of smoke puffs
    int count = (int)ceilf(intensity * 12.0f);

    for (int i = 0; i < count; i++) {
        // 1) Choose a random smoke frame (0…7) for variety
        uint8_t frame = (uint8_t)(rnd() * 8.0f);
        uint8_t tex_smoke = tex_atlas_lookup_particle(TEXAT_PARTICLE_SMOKE_0 + frame);

        // 2) Random brightness between 100 (dark gray) and 255 (light gray)
        uint8_t brightness = (uint8_t)(rnd() * 155.0f + 100.0f);

        // 3) Random direction in a sphere
        vec3 vel = {
            rnd()*2.0f - 1.0f,
            rnd()*2.0f - 1.0f,
            rnd()*2.0f - 1.0f
        };
        glm_vec3_normalize(vel);
        // scale velocity so puffs stay reasonably close
        glm_vec3_scale(vel, (0.2f + rnd()*0.1f) * intensity, vel);

        // 4) Spawn very close to center, with small jitter
        vec3 pos = {
            center[0] + (rnd()-0.5f)*0.15f,
            center[1] + (rnd()-0.5f)*0.15f,
            center[2] + (rnd()-0.5f)*0.15f
        };

        // 5) Randomize lifetime between 10–20 ticks × intensity
        float life = (10.0f + rnd()*10.0f) * intensity;

        // 6) Add particle with no gravity (so no damping), random gray shade
        particle_add(
            pos,
            vel,
            tex_smoke,             // random smoke frame
            0.15f * intensity,     // size
            life,                  // lifetime
            false,                 // no gravity => no damping
            brightness,brightness,brightness,// random gray brightness
			false,
            TEXTURE_ATLAS_PARTICLES
        );
    }
}

// render single takes the brightness of a particle into account and is also used for animation of smoke particles
static void render_single(struct particle* p, vec3 camera, float delta) {
    // Cull any particles beyond 32 blocks
    if (glm_vec3_distance2(p->pos, camera) > 32.0f * 32.0f)
        return;

    // Interpolate position for smooth motion
    vec3 pos_lerp;
    glm_vec3_lerp(p->pos_old, p->pos, delta, pos_lerp);

    // Build billboarding axes
    vec3 view_dir, axis_s, axis_t;
    glm_vec3_sub(pos_lerp, camera, view_dir);
    glm_vec3_crossn(view_dir, (vec3){0,1,0}, axis_s);
    glm_vec3_crossn(view_dir, axis_s, axis_t);
    glm_vec3_scale(axis_s, p->size, axis_s);
    glm_vec3_scale(axis_t, p->size, axis_t);

    // Determine the correct texture tile (for animated smoke)
    uint8_t tile = p->tex;
    if (p->atlas == TEXTURE_ATLAS_PARTICLES
     && tile >= tex_atlas_lookup_particle(TEXAT_PARTICLE_SMOKE_0)
     && tile <= tex_atlas_lookup_particle(TEXAT_PARTICLE_SMOKE_7))
    {
        float t      = (float)p->age / (float)p->lifetime;  // 1.0→0.0
        int   frame  = (int)(t * 7.0f + 0.5f);
        tile         = tex_atlas_lookup_particle(TEXAT_PARTICLE_SMOKE_0 + frame);
    }

    // Compute UV coordinates
    float u0, v0, u1, v1;
    if (p->atlas == TEXTURE_ATLAS_TERRAIN) {
        u0 = p->tex_uv[0];
        v0 = p->tex_uv[1];
        u1 = u0 + (4.0f  / 256.0f);
        v1 = v0 + (4.0f  / 256.0f);
    } else {
        float uv[4];
        if (!tex_atlas_tile_uv(texture_particles.width, texture_particles.height,
                               tile, uv))
            return;
        u0 = uv[0];
        v0 = uv[1];
        u1 = uv[2];
        v1 = uv[3];
    }

    // Lookup world light at the particle's block position
    uint8_t light;
    if (p->ignore_light) {
        light = 255;  // full brightness
    } else {
        struct block_data in_block = world_get_block(
            s_particles->light_world,
            floorf(pos_lerp[0]),
            floorf(pos_lerp[1]),
            floorf(pos_lerp[2])
        );
        light = roundf(
            gfx_lookup_light((in_block.torch_light<<4)|in_block.sky_light)
            *255.0F*0.8F
        );
    }


    // Modulate our RGBA color by that light
    uint8_t cols[16];
    for (int i = 0; i < 4; ++i) {
        cols[i*4 + 0] = (p->r * light) >> 8;
        cols[i*4 + 1] = (p->g * light) >> 8;
        cols[i*4 + 2] = (p->b * light) >> 8;
        cols[i*4 + 3] = 255;
    }

    // Finally draw the quad
    gfx_draw_quads_flt(
        4,
        (float[]){
            -axis_s[0] - axis_t[0] + pos_lerp[0],
            -axis_s[1] - axis_t[1] + pos_lerp[1],
            -axis_s[2] - axis_t[2] + pos_lerp[2],

             axis_s[0] - axis_t[0] + pos_lerp[0],
             axis_s[1] - axis_t[1] + pos_lerp[1],
             axis_s[2] - axis_t[2] + pos_lerp[2],

             axis_s[0] + axis_t[0] + pos_lerp[0],
             axis_s[1] + axis_t[1] + pos_lerp[1],
             axis_s[2] + axis_t[2] + pos_lerp[2],

            -axis_s[0] + axis_t[0] + pos_lerp[0],
            -axis_s[1] + axis_t[1] + pos_lerp[1],
            -axis_s[2] + axis_t[2] + pos_lerp[2]
        },
        cols,
        (float[]){
            u0, v0,
            u1, v0,
            u1, v1,
            u0, v1
        }
    );
}


static bool particle_update_one(struct particle* p) {
	assert(p);
	glm_vec3_copy(p->pos, p->pos_old);

	/* Beta EntityBubbleFX.onUpdate: motionY += 0.002 BEFORE moveEntity. */
	if(p->bubble)
		p->vel[1] += 0.002F;

	vec3 new_pos;
	glm_vec3_add(p->pos, p->vel, new_pos);

	w_coord_t bx = floorf(new_pos[0]);
	w_coord_t by = floorf(new_pos[1]);
	w_coord_t bz = floorf(new_pos[2]);
	struct block_data in_block = world_get_block(s_particles->light_world, bx, by, bz);

    bool intersect = false;
	if(blocks[in_block.type]) {
		struct block_info blk = (struct block_info) {
			.block = &in_block,
			.neighbours = NULL,
			.x = bx,
			.y = by,
			.z = bz,
		};

		size_t count
			= blocks[in_block.type]->getBoundingBox(&blk, true, NULL);
		if(count > 0) {
			struct AABB aabb[count];
			blocks[in_block.type]->getBoundingBox(&blk, true, aabb);

			for(size_t k = 0; k < count; k++) {
				aabb_translate(aabb + k, bx, by, bz);
				intersect = aabb_intersection_point(aabb + k, new_pos[0],
													new_pos[1], new_pos[2]);
				if(intersect)
					break;
			}
		}
	}

	if(!intersect) {
		glm_vec3_copy(new_pos, p->pos);
	} else {
		glm_vec3_zero(p->vel);
	}

    if (p->gravity) {
        p->vel[1] -= 0.04F;
        glm_vec3_scale(p->vel, 0.98F, p->vel);
	}
	if(p->bubble) {
		/* ... motion *= 0.85F, then setEntityDead outside water. */
		glm_vec3_scale(p->vel, 0.85F, p->vel);
		struct block_data at = world_get_block(s_particles->light_world,
			(w_coord_t)floorf(p->pos[0]), (w_coord_t)floorf(p->pos[1]),
			(w_coord_t)floorf(p->pos[2]));
		if(at.type != BLOCK_WATER_STILL && at.type != BLOCK_WATER_FLOW)
			return false;
	}
	p->age--;
	return p->age > 0;
}

static void particle_swap(struct particle* a, struct particle* b) {
	struct particle tmp = *a;
	*a = *b;
	*b = tmp;
}

static void particle_ring_reverse(struct particle_ring* ring, size_t first,
										  size_t end) {
	while(first < end && first < --end)
		particle_swap(&ring->data[first++], &ring->data[end]);
}

static void particle_ring_linearize(struct particle_ring* ring) {
	assert(ring);
	if(ring->head == 0 || ring->count < 2)
		return;
	/* Rotate the wrapped ring left by head using three in-place reversals. This
	 * is O(N) once per update, replacing the old O(N) shift for every expired or
	 * cap-evicted particle. */
	particle_ring_reverse(ring, 0, ring->head);
	particle_ring_reverse(ring, ring->head, ring->count);
	particle_ring_reverse(ring, 0, ring->count);
	ring->head = 0;
}

// S5.3 hunt PROBE (temporary, ROUND 7 cycle 3): takes 26/27 (byte-identical
// traces) both died in the narrow gap between the "tick_iter" marker and the
// "tick_particle" marker -- i.e. inside this exact call -- with every canary
// clean and healthy stack headroom, twice. particle_update_one() calls
// world_get_block(&gstate.world, ...) for every LIVE particle with NO
// world_loaded guard (unlike client_display_tick's matching early-return),
// so the leading suspect is stale particles surviving the provider swap
// (never cleared, since CRPC_WORLD_RESET/particle_clear_all is proven to
// never run on this path -- MP_EVENT_LOGIN never fires here) touching a
// gstate.world that may be mid-teardown. This accessor answers the load-
// bearing question directly instead of guessing: are there actually any
// live particles at the moment of death? particle_test_count() below is
// CAVEWII_PARTICLE_TEST-gated (test-only); this is unconditionally built so
// main.c's probe can call it in a release/hardware build.
size_t particle_debug_total_count(void) {
	return s_particles->layers[0].count + s_particles->layers[1].count;
}

void particle_update() {
	for(size_t layer = 0; layer < 2; layer++) {
		struct particle_ring* ring = &s_particles->layers[layer];
		particle_ring_linearize(ring);
		size_t survivors = 0;
		for(size_t i = 0; i < ring->count; i++) {
			struct particle* p = &ring->data[i];
			if(!particle_update_one(p))
				continue;
			if(survivors != i)
				ring->data[survivors] = *p;
			survivors++;
		}
		ring->count = survivors;
	}
}

void particle_generate_smoke(vec3 center, float intensity) {
    int count = (int)ceilf(intensity * 4.0f);
    // always start at the largest smoke-frame
    uint8_t tex_smoke_base = tex_atlas_lookup_particle(TEXAT_PARTICLE_SMOKE_7);

    for (int i = 0; i < count; i++) {
        // Position jitter around center
        vec3 pos = {
            center[0] + (rnd() - 0.5f) * 0.3f,
            center[1] + 0.7f + rnd() * 0.2f,
            center[2] + (rnd() - 0.5f) * 0.3f
        };

        // Upward drift + slight horizontal drift
        vec3 vel = {
            (rnd() - 0.5f) * 0.01f,
            0.1f + rnd() * 0.02f,
            (rnd() - 0.5f) * 0.01f
        };

        // Shorter, randomized lifetime (10–20 ticks) × intensity
        float life = (10.0f + rnd() * 10.0f) * intensity;

        // Random gray shade: brightness between 50 (dark) and 200 (light)
        uint8_t brightness = (uint8_t)(rnd() * 150.0f + 50.0f);

        // Add particle without gravity (no damping), now with full RGBA:
        particle_add(
            pos,
            vel,
            tex_smoke_base,           // enum index for frame 7
            0.15f * intensity,        // size
            life,                     // lifetime in ticks
            false,                    // no gravity => no damping
            brightness,               // r
            brightness,               // g
            brightness,               // b
			false,
            TEXTURE_ATLAS_PARTICLES   // atlas
        );
    }
}

void particle_generate_wolf_tame_fx(vec3 center, bool hearts) {
	/* Beta spawns SEVEN particles for EITHER outcome -- "heart" or "smoke" is
	 * the only difference, the position/velocity formula is shared:
	 *   x/z = pos +- width          (width == height == 0.8 for the wolf's
	 *                                 own box; EntityWolf ctor setSize(0.8F,
	 *                                 0.8F))
	 *   y   = pos + 0.5 .. pos + 0.5 + height
	 *   vel = independent zero-mean Gaussian(sigma=0.02) per axis
	 * drawn in Beta's own left-to-right argument-evaluation order: the three
	 * velocity gaussians (X,Y,Z) are locals assigned BEFORE the
	 * spawnParticle(...) call, then its position arguments are evaluated
	 * left-to-right (X,Y,Z) as part of that same call.
	 * [740c583: EntityWolf.showHeartsOrSmokeFX; EntityWolf ctor]
	 *
	 * (Prior code gave the smoke case an unrelated generic puff via
	 * particle_generate_smoke() and gave the heart case a too-narrow +-0.4
	 * uniform spread with an upward-biased, non-Gaussian velocity -- neither
	 * matched this method.) */
	const struct monster_def* wolf_def = monster_def_get(MONSTER_WOLF);
	float size = wolf_def->box_w; // == box_h, both 0.8F
	uint8_t tex = tex_atlas_lookup_particle(
		hearts ? TEXAT_PARTICLE_HEART : TEXAT_PARTICLE_SMOKE_7);
	for(int i = 0; i < 7; i++) {
		vec3 vel = {
			rand_gen_gaussian(&gstate.rand_src) * 0.02F,
			rand_gen_gaussian(&gstate.rand_src) * 0.02F,
			rand_gen_gaussian(&gstate.rand_src) * 0.02F,
		};
		vec3 pos = {
			center[0] + rnd() * size * 2.0F - size,
			center[1] + 0.5F + rnd() * size,
			center[2] + rnd() * size * 2.0F - size,
		};
		if(hearts) {
			particle_add(pos, vel, tex, 0.16F, 20.0F, false,
				255, 255, 255, true, TEXTURE_ATLAS_PARTICLES);
		} else {
			// Cosmetic-only (not Beta-cited): a random gray shade per puff,
			// matching the visual character the old particle_generate_smoke
			// delegate gave this branch, now under the correct geometry.
			uint8_t brightness = (uint8_t)(rnd() * 150.0F + 50.0F);
			particle_add(pos, vel, tex, 0.15F, 20.0F, false,
				brightness, brightness, brightness, false,
				TEXTURE_ATLAS_PARTICLES);
		}
	}
}

void particle_generate_wolf_splash(vec3 center, vec3 velocity, int count) {
	/* The local RPC boundary is not a trust boundary today, but cap the public
	 * helper at EntityWolf's mathematical maximum so a corrupt packet cannot
	 * turn one status event into an unbounded render-thread append burst. */
	if(count <= 0)
		return;
	if(count > WOLF_SHAKE_MAX_SPLASHES)
		count = WOLF_SHAKE_MAX_SPLASHES;
	uint8_t tex = tex_atlas_lookup_particle(TEXAT_PARTICLE_SPLASH_0);
	for(int i = 0; i < count; i++) {
		vec3 pos = {
			center[0] + (rnd() - 0.5F) * 0.8F,
			center[1] + 0.8F,
			center[2] + (rnd() - 0.5F) * 0.8F,
		};
		vec3 vel = {velocity[0], velocity[1], velocity[2]};
		particle_add(pos, vel, tex, 0.12F, 20.0F, true,
			255, 255, 255, false, TEXTURE_ATLAS_PARTICLES);
	}
}

void particle_generate_drown_bubbles(vec3 pos, vec3 vel) {
	uint8_t tex = tex_atlas_lookup_particle(TEXAT_PARTICLE_BUBBLE);
	for(int i = 0; i < DROWN_BUBBLE_COUNT; i++) {
		/* EntityLiving.onEntityUpdate:128-131: var2/var3/var4 = r - r, in
		 * Java's left-to-right order (C leaves `rnd() - rnd()` unsequenced,
		 * so every draw is its own statement). */
		float d[3];
		for(int k = 0; k < 3; k++) {
			float a = rnd();
			float b = rnd();
			d[k] = a - b;
		}
		vec3 at = {pos[0] + d[0], pos[1] + d[1], pos[2] + d[2]};
		/* EntityBubbleFX ctor: particleScale *= r*0.6+0.2 on the EntityFX
		 * base (r*0.5+0.5)*2 -- the 0.15 size unit maps particleScale 2.0,
		 * as particle_generate_tnt_smoke does. */
		float base = rnd() * 0.5F + 0.5F;
		float size = base * 0.15F * (rnd() * 0.6F + 0.2F);
		vec3 v;
		for(int k = 0; k < 3; k++)
			v[k] = vel[k] * 0.2F + (rnd() * 2.0F - 1.0F) * 0.02F;
		/* particleMaxAge = (int)(8/(r*0.8+0.2)); `if(particleMaxAge-- <= 0)`
		 * kills on update maxAge+1, our ring kills after `lifetime` updates. */
		int max_age = (int)(8.0F / (rnd() * 0.8F + 0.2F));
		struct particle* p = particle_add_p(at, v, tex, size,
			(float)(max_age + 1), false, 255, 255, 255, false,
			TEXTURE_ATLAS_PARTICLES);
		if(p)
			p->bubble = true;
	}
}

void particle_generate_tnt_smoke(vec3 entity_pos) {
	/*
		EntityTNTPrimed.onUpdate passes one zero-velocity "smoke" request at
		the exact entity position + 0.5Y. EntitySmokeFX then gives that particle
		a small randomized drift, dark-gray tint, 0.75..1.5 particle scale and
		an 8..40-tick lifetime. CaveWii's particle integrator is simpler than
		EntitySmokeFX, but matching the spawn count/origin and constructor
		distributions avoids the old four-puff, heavily-jittered trail.
	*/
	vec3 pos = {entity_pos[0], entity_pos[1] + 0.5f, entity_pos[2]};
	float mx = (rnd() * 2.0f - 1.0f) * 0.4f;
	float my = (rnd() * 2.0f - 1.0f) * 0.4f;
	float mz = (rnd() * 2.0f - 1.0f) * 0.4f;
	float speed = (rnd() + rnd() + 1.0f) * 0.15f;
	float length = sqrtf(mx * mx + my * my + mz * mz);
	if(length < 1.0e-7f) {
		mx = 0.0f;
		my = 1.0f;
		mz = 0.0f;
		length = 1.0f;
	}
	vec3 vel = {
		mx / length * speed * 0.04f,
		(my / length * speed * 0.4f + 0.1f) * 0.1f,
		mz / length * speed * 0.04f,
	};
	uint8_t gray = (uint8_t)(rnd() * 0.3f * 255.0f);
	float size = (rnd() * 0.5f + 0.5f) * 0.15f;
	float lifetime = 8.0f / (rnd() * 0.8f + 0.2f);

	particle_add(pos, vel,
				 tex_atlas_lookup_particle(TEXAT_PARTICLE_SMOKE_7), size,
				 lifetime, false, gray, gray, gray, false,
				 TEXTURE_ATLAS_PARTICLES);
}

#ifdef CAVEWII_PARTICLE_TEST
size_t particle_test_count(void) {
	return s_particles->layers[TEXTURE_ATLAS_TERRAIN].count
		+ s_particles->layers[TEXTURE_ATLAS_PARTICLES].count;
}

bool particle_test_get(size_t index, struct particle* out) {
	if(!out)
		return false;
	for(size_t layer = 0; layer < 2; layer++) {
		struct particle_ring* ring = &s_particles->layers[layer];
		if(index < ring->count) {
			*out = *particle_ring_at(ring, index);
			return true;
		}
		index -= ring->count;
	}
	return false;
}

size_t particle_test_evictions(particle_atlas_t atlas) {
	if(atlas != TEXTURE_ATLAS_TERRAIN
	   && atlas != TEXTURE_ATLAS_PARTICLES)
		return 0;
	return s_particles->evictions[atlas];
}
#endif

void particle_generate_torch(vec3 pos) {
    uint8_t tex_flame      = tex_atlas_lookup_particle(TEXAT_PARTICLE_FLAME);
    uint8_t tex_smoke_base = tex_atlas_lookup_particle(TEXAT_PARTICLE_SMOKE_7);

    // spawn 2 sparks per call
    for (int i = 0; i < 2; i++) {
        // flickering flame spark
        vec3 vel_f = {
            (rnd() - 0.5f) * 0.015f,
            0.015f + rnd() * 0.01f,
            (rnd() - 0.5f) * 0.015f
        };
        float size_f = 0.04f + rnd() * 0.04f;   // size 0.04–0.08
        float life_f = 6.0f  + rnd() * 2.0f;    // lifetime 6–8 ticks

        particle_add(
            pos, vel_f,
            tex_flame,
            size_f, life_f,
            false,    // no gravity (no damping)
			255, 255, 255,     // full brightness for flame
			true,
            TEXTURE_ATLAS_PARTICLES
        );

        // occasional smoke: about 1 in 10 sparks
        if (rnd() < 0.1f) {
            // a) smaller smoke puff
            vec3 vel_s = {
                (rnd() - 0.5f) * 0.006f,
                0.008f + rnd() * 0.005f,
                (rnd() - 0.5f) * 0.006f
            };
            vec3 spawn = {
                pos[0] + (rnd() - 0.5f) * 0.03f,
                pos[1] + (rnd() - 0.5f) * 0.03f,
                pos[2] + (rnd() - 0.5f) * 0.03f
            };
            float size_s = 0.06f + rnd() * 0.04f;   // now 0.06–0.10
            float life_s = 10.0f + rnd() * 10.0f;
            uint8_t brightness_s = (uint8_t)(rnd() * 100.0f + 50.0f);

            particle_add(
                spawn, vel_s,
                tex_smoke_base,
                size_s, life_s,
                false,          // no gravity => no damping
	            brightness_s,               // r
	            brightness_s,               // g
	            brightness_s,               // b,   // varied gray brightness
				false,
                TEXTURE_ATLAS_PARTICLES
            );
        }
    }
}

void particle_render(mat4 view, vec3 camera, float delta) {
    gfx_matrix_modelview(view);
    gfx_lighting(false);

    // Terrain/block particles
    gfx_bind_texture(&texture_terrain);
	struct particle_ring* terrain
		= &s_particles->layers[TEXTURE_ATLAS_TERRAIN];
	for(size_t i = 0; i < terrain->count; i++)
		render_single(particle_ring_at(terrain, i), camera, delta);

    // Particle-atlas particles (sparks, smoke, etc)
    gfx_bind_texture(&texture_particles);
	struct particle_ring* fx
		= &s_particles->layers[TEXTURE_ATLAS_PARTICLES];
	for(size_t i = 0; i < fx->count; i++)
		render_single(particle_ring_at(fx, i), camera, delta);

    gfx_lighting(true);
}

/* ---- Split-dims B2: particle systems -------------------------------------- */

struct particle_system* particle_system_primary(void) {
	return &s_particle_primary;
}

struct particle_system* particle_system_create(struct world* w) {
	assert(w);
	struct particle_system* sys = calloc(1, sizeof(*sys));
	if(sys)
		sys->light_world = w;
	return sys;
}

void particle_system_destroy(struct particle_system* sys) {
	if(!sys)
		return;
	/* The primary is static storage bound to gstate.world for the process
	 * lifetime, and a selected system is still in use: refuse both. */
	assert(sys != &s_particle_primary && sys != s_particles);
	if(sys == &s_particle_primary || sys == s_particles)
		return;
	for(size_t i = 0; i < 2; i++)
		mem2_free(sys->layers[i].data);
	free(sys);
}

struct particle_system* particle_system_select(struct particle_system* sys) {
	struct particle_system* previous = s_particles;
	s_particles = sys ? sys : &s_particle_primary;
	return previous;
}

struct particle_system* particle_system_selected(void) {
	return s_particles;
}

void particle_system_clear(struct particle_system* sys) {
	struct particle_system* previous = particle_system_select(sys);
	particle_clear_all();
	particle_system_select(previous);
}

size_t particle_system_count(const struct particle_system* sys) {
	return sys ? sys->layers[0].count + sys->layers[1].count : 0;
}

size_t particle_system_mem2_bytes(const struct particle_system* sys) {
	/* The rings are the system's only variable storage and they live in MEM2
	 * (mem2_realloc above); the fixed struct itself is counted by the owner
	 * that allocated it. Capacity, not count: rings never shrink. */
	return sys ? (sys->layers[0].capacity + sys->layers[1].capacity)
			* sizeof(struct particle)
			   : 0;
}

size_t particle_system_struct_bytes(void) {
	return sizeof(struct particle_system);
}

void particle_system_set_camera(struct particle_system* sys, vec3 p) {
	if(sys)
		glm_vec3_copy(p, sys->camera);
}
