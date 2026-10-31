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

#ifndef PARTICLE_H
#define PARTICLE_H

#include "cglm/cglm.h"
#include "platform/time.h"
#include "world.h"


typedef enum {
  TEXTURE_ATLAS_TERRAIN,
  TEXTURE_ATLAS_PARTICLES
} particle_atlas_t;


struct particle {
    vec3             pos;        // current position
    vec3             pos_old;    // previous position for interpolation
    vec3             vel;        // velocity
    vec2             tex_uv;     // only for terrain‐atlas random offset
    float            size;       // half‐quad size
    int              age;        // remaining life in ticks
    int              lifetime;   // initial life in ticks, for animating smoke
    uint8_t          tex;        // base tile index
    bool             gravity;    // apply gravity?
    uint8_t     	 r, g, b;	 // rgb colorisation of a particle
    bool             ignore_light;    // apply gravity?
    /* drown-fx-0927: Beta EntityBubbleFX motion (onUpdate: motionY += 0.002,
     * move, motion *= 0.85, dies outside water). */
    bool             bubble;
    particle_atlas_t atlas;      // which atlas to sample
};


void particle_add(vec3 pos,
                  vec3 vel,
                  uint8_t tex,
                  float size,
                  float lifetime,
                  bool gravity,
                  uint8_t r,
                  uint8_t g,
                  uint8_t b,
				  bool ignore_light,
				  particle_atlas_t atlas);

void particle_init(void);
// M5: drop every live particle (client world reset / dimension switch).
void particle_clear_all(void);
void particle_set_camera(vec3 p);
void particle_generate_block(struct block_info* info);
void particle_generate_side(struct block_info* info, enum side s);
void particle_update(void);
/* S5.3 hunt PROBE (temporary, ROUND 7 cycle 3): unconditionally-built count
 * accessor -- see particle.c's declaration comment. Delete with the rest of
 * the S5.3 hunt probes once the crash inside particle_update() is
 * root-caused and fixed. */
size_t particle_debug_total_count(void);
void particle_render(mat4 view, vec3 camera, float delta);
void particle_generate_explosion_flash(vec3 center, float intensity);
void particle_generate_explosion_smoke(vec3 center, float intensity);
void particle_generate_smoke(vec3 center, float intensity);
void particle_generate_wolf_tame_fx(vec3 center, bool hearts);
void particle_generate_wolf_splash(vec3 center, vec3 velocity, int count);
/* drown-fx-0927: Beta EntityLiving.onEntityUpdate:127-132 + EntityBubbleFX --
 * the eight "bubble" particles an actor spawns when its air runs out. Each at
 * pos + (r-r, r-r, r-r); EntityBubbleFX: motion = vel*0.2 + U(-0.02,0.02) per
 * axis, particleScale (r*0.5+0.5)*2 * (r*0.6+0.2), particleMaxAge
 * (int)(8/(r*0.8+0.2)); rises (+0.002/tick), drags x0.85, dies outside
 * water or when its age runs out. Cosmetic draws come from the particle
 * stream (the documented cosmetic-RNG rule), not the actor's. */
#define DROWN_BUBBLE_COUNT 8
void particle_generate_drown_bubbles(vec3 pos, vec3 vel);
/*
	Beta EntityTNTPrimed emits exactly one smoke particle per live update at
	(posX, posY + 0.5, posZ).  Keep this separate from the ambient/explosion
	smoke helper, which deliberately emits a multi-particle jittered puff.
*/
void particle_generate_tnt_smoke(vec3 entity_pos);
void particle_generate_torch(vec3 pos);
void particle_generate_redstone_torch(vec3 center);
void particle_generate_redstone_wire(vec3 center, uint8_t power);

/* Split-dims B2: particle SYSTEMS, one per client world payload. Every
 * particle_* call above (add, generate_*, set_camera, update, render,
 * clear_all, debug/test counts) operates on the SELECTED system. The primary
 * system is static, bound to gstate.world, and selected by default, so code
 * that never selects another system is unchanged. The client owner selects a
 * payload's system around that world's work and restores the previous one
 * (particle_system_select returns it); main thread only, like every
 * particle call. particle_system_create binds a new system to w (the world
 * its particles read light/collision from); destroy refuses the primary and
 * the selected system. mem2_bytes = ring capacity (MEM2; rings never shrink). */
struct particle_system;
struct particle_system* particle_system_primary(void);
struct particle_system* particle_system_create(struct world* w);
void particle_system_destroy(struct particle_system* sys);
struct particle_system* particle_system_select(struct particle_system* sys);
struct particle_system* particle_system_selected(void);
void particle_system_clear(struct particle_system* sys);
size_t particle_system_count(const struct particle_system* sys);
size_t particle_system_mem2_bytes(const struct particle_system* sys);
/* sizeof(struct particle_system): the fixed MEM1 part a created system costs. */
size_t particle_system_struct_bytes(void);
/* Spawn-cull camera of one system (particle_set_camera sets the SELECTED
 * system's; main.c sets the primary's every frame). */
void particle_system_set_camera(struct particle_system* sys, vec3 p);

#ifdef CAVEWII_PARTICLE_TEST
size_t particle_test_count(void);
bool particle_test_get(size_t index, struct particle* out);
size_t particle_test_evictions(particle_atlas_t atlas);
#endif
#endif
