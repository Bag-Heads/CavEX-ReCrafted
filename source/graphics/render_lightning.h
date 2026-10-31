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

#ifndef GRAPHICS_RENDER_LIGHTNING_H
#define GRAPHICS_RENDER_LIGHTNING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../cglm/cglm.h"
#include "../worldgen/java_random.h"

/* Client-side lightning bolt VISUALS (Bucket B): the bolt mesh and the sky
 * flash. Citations are to the RetroMCP b1.7.3 client decompile
 * (../mcb173-retromcp/minecraft/src/net/minecraft/src/):
 *   RenderLightningBolt.java:7-100   func_27002_a (the mesh)
 *   EntityLightningBolt.java:10-87   constructor / onUpdate / isInRangeToRenderVec3D
 *   World.java:1050-1060             func_4079_a (sky colour) flash term
 *   client/Minecraft.java:1142-1147  runTick: --field_27172_i, then updateEntities
 *
 * Who feeds it: (1) ONLINE -- server_remote_protocol.c's Packet71 (type 1)
 * handler, which is NetClientHandler.handleWeather's `new
 * EntityLightningBolt` + addWeatherEffect; (2) LOCAL -- server_weather.c's
 * world_lightning_bolt_spawn_w sends CRPC_LIGHTNING_BOLT with the server
 * bolt's post-constructor render state (client_interface.c spawns it here).
 * The bolt's GAMEPLAY (fire, onStruckByLightning) belongs to the server
 * either way; this client copy keeps only the render lifecycle.
 *
 * Each bolt is tagged with the dimension it struck in (Beta keeps it in
 * that World's weatherEffects list): it is drawn, and its flash applied,
 * only in views showing that dimension, and every bolt is dropped when the
 * primary player's dimension changes (Beta builds a fresh World).
 *
 * Beta keeps an unbounded weatherEffects list. Here a fixed pool of
 * LIGHTNING_MAX_BOLTS: a bolt lives at most 2 + 3 restrikes * <=10 ticks, so
 * 8 concurrent bolts in one client's view is far beyond any real storm. A
 * spawn into a full pool replaces the OLDEST bolt and counts it in
 * `dropped` (visible, never silent). */

#define LIGHTNING_MAX_BOLTS 8
/* 4 width layers x (8 trunk + 3 + 3 branch segments) = 56 GL_TRIANGLE_STRIPs
 * of 10 vertices each (RenderLightningBolt.java:26-92). Each strip is drawn
 * here as its 4 planar quads (16 vertices) -- the same 8 triangles. */
#define LIGHTNING_LAYERS 4
#define LIGHTNING_STRIPS_PER_BOLT 56
#define LIGHTNING_QUADS_PER_BOLT (LIGHTNING_STRIPS_PER_BOLT * 4)
#define LIGHTNING_VERTS_PER_BOLT (LIGHTNING_QUADS_PER_BOLT * 4)
/* Tessellator.setColorRGBA_F truncates (int)(c*255.0F):
 * 0.9F*0.5F -> 114, 1.0F*0.5F -> 127, 0.3F -> 76. */
#define LIGHTNING_COLOR_R 114
#define LIGHTNING_COLOR_G 114
#define LIGHTNING_COLOR_B 127
#define LIGHTNING_COLOR_A 76

struct lightning_bolt {
	double x, y, z;
	int64_t render_seed;   /* field_27029_a */
	int32_t state;         /* field_27028_b: 2 at spawn */
	int32_t living_time;   /* field_27030_c: nextInt(3)+1 restrikes */
	struct java_random rng; /* the entity's own (unsynced) Random */
	int32_t dimension;      /* enum world_dim it struck in */
};

struct lightning_fx {
	struct lightning_bolt bolts[LIGHTNING_MAX_BOLTS];
	size_t count;
	int32_t flash;       /* World.field_27172_i */
	int32_t flash_dimension; /* the World whose field_27172_i that is */
	int32_t player_dimension; /* last primary-player dimension seen */
	bool player_dimension_known;
	uint32_t generation; /* remote world identity the bolts belong to */
	uint32_t spawned;
	uint32_t dropped;
};

/* --- pure, testable (tests/test_lightning_render.c) ---------------------- */

/* EntityLightningBolt.java:10-15 (render half): state 2, render seed =
 * rand.nextLong(), living_time = rand.nextInt(3)+1, rand seeded from
 * `rng_seed`. The constructor's fire placement is the server's. */
void lightning_bolt_init(struct lightning_bolt* b, double x, double y, double z,
						 int64_t rng_seed);
/* EntityLightningBolt.onUpdate (java:36-73) minus sound (played by the
 * packet handler), fire and onStruckByLightning (server-owned). Returns
 * false when the bolt died (setEntityDead); sets *flash = 2 when the bolt is
 * active this tick (state >= 0). */
bool lightning_bolt_tick(struct lightning_bolt* b, int32_t* flash);
/* isInRangeToRenderVec3D (java:85-87): drawn only while state >= 0. */
bool lightning_bolt_visible(const struct lightning_bolt* b);

void lightning_fx_reset(struct lightning_fx* fx);
void lightning_fx_spawn(struct lightning_fx* fx, double x, double y, double z,
						int64_t rng_seed, uint32_t generation,
						int32_t dimension);
/* Local-authority bolt (CRPC_LIGHTNING_BOLT): adopt the server bolt's
 * post-constructor render state verbatim -- living_time, render seed and
 * the raw 48-bit Entity.rand state -- so restrikes replay identically. */
void lightning_fx_spawn_state(struct lightning_fx* fx, double x, double y,
							  double z, int32_t living_time,
							  int64_t render_seed, uint64_t rng_state,
							  uint32_t generation, int32_t dimension);
/* Minecraft.runTick order: `if(field_27172_i > 0) --field_27172_i;` THEN
 * World.updateEntities ticks the weather effects (which may set it to 2).
 * A generation change (new remote world / disconnect) drops everything,
 * like Beta's fresh WorldClient with an empty weatherEffects list; so does
 * a change of the primary player's dimension (local sessions keep remote
 * generation 0 across a portal trip). */
void lightning_fx_tick(struct lightning_fx* fx, uint32_t generation,
					   int32_t player_dimension);
/* The flash amount a view of `dimension` sees: 0 unless the flash belongs
 * to that dimension's World. */
float lightning_fx_flash(const struct lightning_fx* fx, float partial,
						 int32_t dimension);
/* Does a view of `dimension` draw bolt `b`? (visible AND same dimension) */
bool lightning_bolt_drawn_in(const struct lightning_bolt* b, int32_t dimension);

/* World.func_4079_a flash term: f = flash - partial, clamped to <= 1,
 * times 0.45F; 0 when flash <= 0. */
float lightning_flash_amount(int32_t flash, float partial);
/* rgb = rgb*(1-a) + (0.8, 0.8, 1.0)*a, rgb in Beta's 0..1 sky units. */
void lightning_flash_apply(float amount, float rgb[3]);

/* RenderLightningBolt.func_27002_a geometry for `seed`, relative to the
 * bolt position (x/z already include Beta's +0.5 centring). Writes exactly
 * LIGHTNING_VERTS_PER_BOLT vertices (quad order) and returns that count. */
size_t lightning_bolt_geometry(int64_t seed,
							   float verts_out[LIGHTNING_VERTS_PER_BOLT][3]);

/* --- client entry points (main.c / server_remote_protocol.c) ------------ */

/* Packet71 type 1: spawn a client render bolt (client/main thread only). */
void lightning_client_spawn(double x, double y, double z, uint32_t generation,
							int32_t dimension);
/* CRPC_LIGHTNING_BOLT (local storms), client/main thread only. */
void lightning_client_spawn_state(int32_t x, int32_t y, int32_t z,
								  int32_t living_time, int64_t render_seed,
								  uint64_t rng_state, uint32_t generation,
								  int32_t dimension);
/* Once per 20 Hz client tick; `player_dimension` = the primary player's. */
void lightning_client_tick(uint32_t generation, int32_t player_dimension);
/* Flash amount for a view of `dimension`'s sky/fog colour (0 when none). */
float lightning_client_flash(float partial, int32_t dimension);
/* Draws every visible bolt. `view` is the camera view matrix, `cam` the
 * camera eye position it was built from. Restores the gfx state the
 * precipitation pass also restores (cull BACK, blend OFF, alpha test on,
 * lighting on, texture on, modelview = view). No-op with no visible bolt. */
void render_lightning_bolts(mat4 view, const vec3 cam, int32_t dimension);
const struct lightning_fx* lightning_client_state(void);

#endif
