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
	Beta 1.7.3 precipitation render (render-slice, RENDER_DISPATCH.md, base
	c5c696a): EntityRenderer.renderRainSnow / addRainParticles [740c583].
	Every constant below carries that citation form; see render_weather.h for
	the pure/testable building blocks' individual citations.
*/

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "render_weather.h"

#include "../block/blocks_data.h"
#include "../platform/audio.h" /* ambient.weather.rain playback (2026-08-31) */
#include "../daytime.h"
#include "../game/game_state.h"
#include "../light_brightness.h"
#include "../particle.h"
#include "../platform/gfx.h"
#include "../platform/texture.h"
#include "../util.h"
#include "../world.h"
#include "../worldgen/biome.h"
#include "../worldgen/java_random.h"
#include "texture_atlas.h"

// THE KEY RULING (RENDER_DISPATCH.md): Beta itself ships the perf lever --
// radius 5 (default) or 10 when gameSettings.fancyGraphics. Both are
// Beta-shipped values; shipping fast/5 as the Wii default is "Beta with fast
// graphics", not a T2 deviation. NOT a third, invented radius.
#define PRECIP_RADIUS_FAST 5
#define PRECIP_RADIUS_FANCY 10
#define PRECIP_MAX_RADIUS PRECIP_RADIUS_FANCY
#define PRECIP_BOX_SIDE (PRECIP_MAX_RADIUS * 2 + 1)
#define PRECIP_BOX_CELLS (PRECIP_BOX_SIDE * PRECIP_BOX_SIDE)

// =============================================================================
// Pure, testable building blocks (tests/test_precip_render_env.c). See
// render_weather.h for the citation on each.
// =============================================================================

int32_t precip_column_seed(int32_t x, int32_t z) {
	// Java int32 wraparound: do every multiply/add in uint32_t (well-defined
	// modular arithmetic in C, unlike signed overflow) and reinterpret the
	// final bit pattern as int32_t at the end -- this project's established
	// idiom (server_weather.c's position LCG: `pos_lcg * 3u + 1013904223u`).
	// Two's-complement +/-/* are ring-homomorphic between the signed and
	// unsigned interpretations of the same width, so computing entirely in
	// uint32_t and casting once at the end is bit-identical to Java
	// reinterpreting after every individual operation.
	uint32_t ux = (uint32_t)x, uz = (uint32_t)z;
	uint32_t t1 = ux * ux * 3121u;
	uint32_t t2 = ux * 45238971u;
	uint32_t t3 = uz * uz * 418711u;
	uint32_t t4 = uz * 13761u;
	return (int32_t)(t1 + t2 + t3 + t4);
}

void precip_seed_random(struct java_random* r, int32_t x, int32_t z) {
	// random.setSeed(long): Java implicitly sign-extends the int32 seed
	// expression to a long at the call site -- (int64_t)an int32_t does
	// exactly that in C.
	jrand_set_seed(r, (int64_t)precip_column_seed(x, z));
}

int32_t precip_snow_clamp_top(int32_t top) {
	return top < 0 ? 0 : top;
}

void precip_column_y_range(int32_t top, int32_t py, int32_t r, int32_t* y0,
						   int32_t* y1) {
	int32_t lo = py - r, hi = py + r;
	*y0 = lo < top ? top : lo;
	*y1 = hi < top ? top : hi;
}

bool precip_strength_active(float strength) {
	return strength > 0.0f;
}

float precip_dist_factor(float dx, float dz, float r) {
	return sqrtf(dx * dx + dz * dz) / r;
}

float precip_snow_alpha(float d, float strength) {
	return ((1.0f - d * d) * 0.3f + 0.5f) * strength;
}

float precip_rain_alpha(float d, float strength) {
	return ((1.0f - d * d) * 0.5f + 0.5f) * strength;
}

float precip_rain_brightness(float raw_light_brightness) {
	return raw_light_brightness * 0.85f + 0.15f;
}

void precip_snow_uv(float next_float_1, float gaussian_1, float next_float_2,
					float gaussian_2, float t, float v28, float* u, float* v) {
	*u = next_float_1 + t * 0.01f * gaussian_1;
	*v = next_float_2 + t * gaussian_2 * 0.001f + v28;
}

int32_t precip_rain_and31(int32_t renderer_update_count, int32_t seed) {
	uint32_t sum = (uint32_t)renderer_update_count + (uint32_t)seed;
	return (int32_t)(sum & 31u);
}

float precip_rain_v(int32_t rc_seed_and31, float partial, float next_float) {
	return ((float)rc_seed_and31 + partial) / 32.0f * (3.0f + next_float);
}

size_t precip_emit_column_geometry(int32_t x, int32_t z, int32_t y0,
								   int32_t y1, float u_bias, float v_bias,
								   float verts_out[8][3], float uv_out[8][2]) {
	float fx = (float)x, fz = (float)z;
	float fy0 = (float)y0, fy1 = (float)y1;
	// Beta's `var26`/`var37` "scale" locals are assigned 1.0F right before
	// the draw and never reassigned inside it -- folded in as the /4.0F
	// constant below rather than threaded as a parameter (see the header).
	float v_y0 = fy0 * 0.25f + v_bias;
	float v_y1 = fy1 * 0.25f + v_bias;

	// Quad 1: X-plane at z+0.5, spanning x..x+1.
	verts_out[0][0] = fx + 0.0f; verts_out[0][1] = fy0; verts_out[0][2] = fz + 0.5f;
	uv_out[0][0] = 0.0f + u_bias; uv_out[0][1] = v_y0;
	verts_out[1][0] = fx + 1.0f; verts_out[1][1] = fy0; verts_out[1][2] = fz + 0.5f;
	uv_out[1][0] = 1.0f + u_bias; uv_out[1][1] = v_y0;
	verts_out[2][0] = fx + 1.0f; verts_out[2][1] = fy1; verts_out[2][2] = fz + 0.5f;
	uv_out[2][0] = 1.0f + u_bias; uv_out[2][1] = v_y1;
	verts_out[3][0] = fx + 0.0f; verts_out[3][1] = fy1; verts_out[3][2] = fz + 0.5f;
	uv_out[3][0] = 0.0f + u_bias; uv_out[3][1] = v_y1;

	// Quad 2: Z-plane at x+0.5, spanning z..z+1.
	verts_out[4][0] = fx + 0.5f; verts_out[4][1] = fy0; verts_out[4][2] = fz + 0.0f;
	uv_out[4][0] = 0.0f + u_bias; uv_out[4][1] = v_y0;
	verts_out[5][0] = fx + 0.5f; verts_out[5][1] = fy0; verts_out[5][2] = fz + 1.0f;
	uv_out[5][0] = 1.0f + u_bias; uv_out[5][1] = v_y0;
	verts_out[6][0] = fx + 0.5f; verts_out[6][1] = fy1; verts_out[6][2] = fz + 1.0f;
	uv_out[6][0] = 1.0f + u_bias; uv_out[6][1] = v_y1;
	verts_out[7][0] = fx + 0.5f; verts_out[7][1] = fy1; verts_out[7][2] = fz + 0.0f;
	uv_out[7][0] = 0.0f + u_bias; uv_out[7][1] = v_y1;

	return 8;
}

// [740c583: BiomeGenBase] -- see the citation in render_weather.h for the
// full derivation. This is biome.c's `biome_get` decision tree with its
// `humidity *= temperature;` first statement removed (the caller already
// supplies that product directly, recovered without division).
static enum biome precip_biome_decide(double temp, double product) {
	if(temp < 0.1)
		return BIOME_TUNDRA;

	if(product < 0.2) {
		if(temp < 0.5)
			return BIOME_TUNDRA;
		if(temp < 0.95)
			return BIOME_SAVANNA;
		return BIOME_DESERT;
	}

	if(product > 0.5 && temp < 0.7)
		return BIOME_SWAMPLAND;

	if(temp < 0.5)
		return BIOME_TAIGA;

	if(temp < 0.97) {
		if(product < 0.35)
			return BIOME_SHRUBLAND;
		return BIOME_FOREST;
	}

	if(product < 0.45)
		return BIOME_PLAINS;
	if(product < 0.9)
		return BIOME_SEASONAL_FOREST;
	return BIOME_RAINFOREST;
}

enum biome precip_biome_from_tint(uint8_t grass_i, uint8_t grass_j) {
	// worldgen_climate truncates (C `(int)` on a non-negative double): byte
	// `ci` covers true temp in a half-open bucket, `ci <= (1-temp)*255 <
	// ci+1`. Reconstructing from the bucket's EDGE (grass_i/255) biases the
	// recovered value systematically HIGH by up to a full 1/255 (never low)
	// -- every bucket's worth of true values reconstructs to the SAME
	// edge-biased point. Reconstructing from the bucket's MIDPOINT
	// ((grass_i+0.5)/255) instead halves the worst case AND zero-centres it
	// (a true value uniformly distributed within its bucket now
	// reconstructs with ~0 average error) -- measured to roughly halve
	// test_precip_render_env's mismatch rate against biome_classify.
	double temp = 1.0 - ((double)grass_i + 0.5) / 255.0;
	double product = 1.0 - ((double)grass_j + 0.5) / 255.0;
	return precip_biome_decide(temp, product);
}

bool precip_biome_enable_snow(enum biome b) {
	// [740c583: BiomeGenBase.getEnableSnow] Duplicated from
	// server_weather.c's identically-named/identically-cited static
	// function on PURPOSE: this is client render code and server_weather.c
	// is simulation code RENDER_DISPATCH.md places off limits for this
	// slice (a parallel dispenser slice is actively editing server_local.c;
	// server_weather.c is adjacent enough to carry the same collision risk).
	// Both copies cite the SAME Beta source and are each covered by their
	// own test (server_weather.c's weather goldens; this one by
	// test_precip_render_env.c) -- see RENDER_SLICE_NOTES.md.
	return b == BIOME_TAIGA || b == BIOME_ICE_DESERT || b == BIOME_TUNDRA;
}

bool precip_biome_enable_rain(enum biome b) {
	// [740c583: BiomeGenBase.canSpawnLightningBolt] `enableSnow ? false :
	// enableRain`; enableRain (the FIELD, not this function) is false only
	// for desert/iceDesert (BiomeGenBase.desert/.iceDesert .setDisableRain()
	// -- hell/sky also disable it but are unreachable from the overworld
	// classifier, see biome.c). See precip_biome_enable_snow's citation note.
	if(precip_biome_enable_snow(b))
		return false;
	return b != BIOME_DESERT && b != BIOME_ICE_DESERT;
}

// =============================================================================
// GX/gfx-touching glue (not unit-testable; everything it calls is).
// =============================================================================

// [740c583: World.getLightBrightness -> getBlockLightValue] Beta's real
// getBlockLightValue_do has an extra neighbor-max special case for 4 specific
// sub-full-height block types (a half-slab, farmland, and two others the
// decompile names stairCompactCobblestone/stairCompactPlanks) that this port
// does NOT reproduce -- see RENDER_SLICE_NOTES.md "scoped cuts". It cannot
// trigger for the RAIN pass (y is always clamped to 127, always open air in
// an ordinary world); it can only rarely trigger for the SNOW pass (the
// sampled Y is "top of solid ground or the camera's own floor, whichever is
// higher" -- normally open air just above the surface, not the surface
// block's own cell).
static float precip_light_brightness(struct world* w, int32_t x, int32_t y,
									 int32_t z, float sun_brightness,
									 float rain_strength,
									 float thunder_strength) {
	if(y < 0)
		// [740c583] y<0 -> getBlockLightValue_do returns 0 OUTRIGHT (not a
		// per-component clamp) before ever touching chunk data.
		return light_lookup_overworld[0];
	if(y >= 128)
		// [740c583] y>=128 clamps to 127 and samples the REAL block there --
		// NOT this engine's world_get_block "out of range -> sky 15"
		// synthetic default (see the citation in RENDER_SLICE_NOTES.md).
		y = 127;

	struct block_data blk = world_get_block(w, x, y, z);
	// render-slice: uses the SAME table unconditionally for both dimensions.
	// This is only ever reached in the overworld: main.c's render_rain_snow
	// call is dimension-gated. (NOTE: it is NOT the weather sim that keeps it
	// out -- server_weather_tick is INERT in the Nether, which leaves the
	// strengths FROZEN at their last overworld value, not 0. The dimension
	// gate at the call site is the real guard. Same reason main.c's
	// gfx_update_light call gates its own strengths.)
	int sky_sub = daytime_skylight_subtracted(sun_brightness, rain_strength,
											  thunder_strength);
	int eff_sky = (int)blk.sky_light - sky_sub;
	int eff
		= ((int)blk.torch_light > eff_sky) ? (int)blk.torch_light : eff_sky;
	if(eff < 0)
		eff = 0;
	if(eff > 15)
		eff = 15;
	return light_lookup_overworld[eff];
}

// main.c's `daytime` local, recomputed here (render_rain_snow's signature is
// pinned to Beta's own `renderRainSnow(float partial)` -- one argument).
static float precip_current_daytime_frac(void) {
	float t = daytime_get_time();
	float day_ticks = (float)DAY_LENGTH_TICKS;
	float frac = fmodf(t, day_ticks) / day_ticks;
	if(frac < 0.0f)
		frac += 1.0f;
	return frac;
}

static void precip_color_bytes(float r, float g, float b, float a,
							   uint8_t out[4]) {
	if(r < 0.0f) r = 0.0f; else if(r > 1.0f) r = 1.0f;
	if(g < 0.0f) g = 0.0f; else if(g > 1.0f) g = 1.0f;
	if(b < 0.0f) b = 0.0f; else if(b > 1.0f) b = 1.0f;
	if(a < 0.0f) a = 0.0f; else if(a > 1.0f) a = 1.0f;
	out[0] = (uint8_t)(r * 255.0f);
	out[1] = (uint8_t)(g * 255.0f);
	out[2] = (uint8_t)(b * 255.0f);
	out[3] = (uint8_t)(a * 255.0f);
}

// Subtracts the camera-relative origin from a column's 8 emitted vertices
// (Beta's per-column `setTranslationD(-x9,-y11,-z13)`: Tessellator applies
// its current translation to each vertex AS IT IS ADDED, so this is exactly
// equivalent -- there is nothing to separately "reset" once the vertices are
// already computed) and draws it with a uniform per-column colour (Beta's
// ONE glColor4f call covering both quads, replicated per-vertex to match
// this engine's gfx_draw_quads_flt contract).
static void precip_draw_column(float verts[8][3], const float uv[8][2],
							   vec3 cam_origin, const uint8_t color[4]) {
	float translated[8][3];
	uint8_t colors[8][4];
	for(int i = 0; i < 8; i++) {
		translated[i][0] = verts[i][0] - cam_origin[0];
		translated[i][1] = verts[i][1] - cam_origin[1];
		translated[i][2] = verts[i][2] - cam_origin[2];
		memcpy(colors[i], color, 4);
	}
	gfx_draw_quads_flt(8, &translated[0][0], &colors[0][0], &uv[0][0]);
}

// Fills biome_box[(2r+1)*(2r+1)] (x outer, z inner, matching the walk order
// both passes use) ONCE for the whole box -- RENDER_DISPATCH.md: "ONE biome
// fetch for the whole box... walked with a single incrementing index in the
// SAME (x outer, z inner) order by both passes". Unresident columns default
// to BIOME_PLAINS (getEnableSnow=false, canSpawnLightningBolt=true) -- an
// arbitrary "ordinary temperate" choice for a column that shouldn't be
// reachable anyway (the precipitation radius stays inside the resident near
// view distance in practice); it can never be BOTH wrong in a way that draws
// something Beta wouldn't (Beta doesn't special-case missing biome data
// either -- WorldChunkManager always returns SOME biome).
static void precip_fill_biome_box(struct world* w, int32_t px, int32_t pz,
								  int32_t r, enum biome* biome_box) {
	int idx = 0;
	for(int32_t x = px - r; x <= px + r; x++) {
		for(int32_t z = pz - r; z <= pz + r; z++) {
			uint8_t gi, gj;
			enum biome b = world_get_section_tint_at(w, x, z, &gi, &gj)
				? precip_biome_from_tint(gi, gj)
				: BIOME_PLAINS;
			biome_box[idx++] = b;
		}
	}
}

void render_rain_snow(float partial) {
	render_rain_snow_owned(partial, gstate.players[0].local_player,
		gstate.players[0].camera.view);
}

void render_rain_snow_owned(float partial, const struct entity* player, mat4 view) {
	// Gate: strength = world.getRainStrength(partial); if (strength <= 0)
	// return. [740c583: World.getRainStrength] prev/cur interpolated by the
	// render's own partial-tick fraction -- see the weather_prev_rain_strength
	// citation in game_state.h for why the client shadows a prev value at
	// all (the wire only ever carries the current per-tick value).
	float strength = gstate.weather_prev_rain_strength
		+ (gstate.weather_rain_strength - gstate.weather_prev_rain_strength)
			* partial;
	if(!precip_strength_active(strength))
		return;
	if(!player || !gstate.world_loaded)
		return;

	// A texture that never loaded (default install ships no environment/
	// rain.png or snow.png -- assets/environment/ only has clouds.png; a real
	// Beta pack supplies both) has tex_gfx_bind's documented NULL-data
	// contract: binding it is a silent no-op that leaves whatever texture the
	// PRIOR draw call left bound. render_rain_snow runs right after the
	// world-translucent/entity/sign passes (main.c), so an unguarded bind
	// here draws the precip quads textured with THAT leftover texture (terrain
	// atlas, a mob skin, ...) sampled with this function's own V-scroll
	// coordinates -- a wrongly-textured, wrongly-"animating" overlay instead
	// of cleanly not drawing. Same precedent as entity_shadow's texture_shadow
	// guard (source/entity/entity.c) and render_item.c's texture_map_bg check.
	// Nothing to draw at all if NEITHER loaded -- skip before touching any GL
	// state, matching entity_shadow's early return.
	if(!texture_snow.data && !texture_rain.data)
		return;

	// Vertices below already subtract the camera position. Preserve only the
	// active camera rotation (including bob/roll), then restore its full view.
	mat4 rotation_view;
	glm_mat4_copy(view, rotation_view);
	rotation_view[3][0] = 0.0f;
	rotation_view[3][1] = 0.0f;
	rotation_view[3][2] = 0.0f;
	gfx_matrix_modelview(rotation_view);

	int32_t r
		= gstate.fancy_graphics ? PRECIP_RADIUS_FANCY : PRECIP_RADIUS_FAST;

	int32_t px = (int32_t)floorf(player->pos[0]);
	int32_t py = (int32_t)floorf(player->pos[1]);
	int32_t pz = (int32_t)floorf(player->pos[2]);

	// Camera-interpolated origin: lastTickPos + (pos - lastTickPos) *
	// partial -- the ENTITY's own feet-position lerp, deliberately NOT
	// gstate.players[0].camera.x/y/z (camera_attach adds an additional presentation-
	// correction term on top of this same lerp; renderRainSnow uses the raw
	// entity interpolation only). [740c583: EntityRenderer.renderRainSnow]
	vec3 cam_origin;
	glm_vec3_lerp((vec3){player->pos_old[0], player->pos_old[1], player->pos_old[2]},
		(vec3){player->pos[0], player->pos[1], player->pos[2]}, partial, cam_origin);
	int32_t interp_floor_y = (int32_t)floorf(cam_origin[1]);

	float sun_brightness = daytime_brightness(precip_current_daytime_frac());
	// The block-light lookup below goes through World.skylightSubtracted, which
	// Beta recomputes once per TICK at partial == 1.0F -- so it uses the CURRENT
	// strengths, deliberately NOT the `strength` lerp above (that one is the
	// curtain ALPHA, which Beta really does interpolate per frame). Two different
	// Beta quantities; see daytime.h. [740c583: World.java:1763]
	float light_rain = gstate.weather_rain_strength;
	float light_thunder = gstate.weather_thunder_strength;

	static enum biome biome_box[PRECIP_BOX_CELLS];
	precip_fill_biome_box(world_view_current(), px, pz, r, biome_box);

	// GL state, in order [740c583]. glNormal3f(0,1,0) has no equivalent in
	// this engine's gfx abstraction (no per-vertex/global normal state on the
	// unlit textured-quad path gfx_draw_quads_flt uses) -- inert in Beta's
	// own pipeline too, since GL_LIGHTING is off throughout world rendering
	// (Minecraft bakes brightness into vertex colour, like this draw does).
	gfx_cull_func(MODE_NONE);
	gfx_blending(MODE_BLEND);
	gfx_alpha_test_ref(true, 0.01f);
	// CLR0=DIRECT precondition for gfx_draw_quads_flt (the GX-wedge class main.c's
	// entity-pass precondition note documents): this runs right after
	// entities_client_render, which requires and leaves CLR0=INDEX8. Without the
	// switch, every rain/snow vertex pushes 4 direct colour bytes into a stream
	// the CP parses expecting a 1-byte index -- FIFO desync, GP wedge, one 500ms
	// bounded-drawdone abort per frame, and the starved single-core scheduler
	// then freezes the server tick (the task_b126e4e2 storm-load clock freeze).
	// Every sibling gfx_draw_quads_flt caller (particles, entity shadow, stars)
	// already brackets with the same pair.
	gfx_lighting(false);

	int32_t tick = gstate.precip_tick_count;
	float t = (float)tick + partial;
	// `& 511u` on the uint32_t bit pattern: well-defined regardless of tick's
	// sign, and matches Java's `int & 511` (bitwise AND on the two's-
	// complement bit pattern -- always non-negative for this mask).
	float v28 = ((float)((uint32_t)tick & 511u) + partial) / 512.0f;

	// --- SNOW pass first, texture snow.png, gated on getEnableSnow(). ---
	// Guarded on texture_snow.data (see this function's top-of-body comment):
	// a pack/default install missing snow.png must skip the whole pass, not
	// bind-as-no-op-and-draw-with-whatever-was-last-bound.
	gfx_texture(true);
	int idx = 0;
	if(texture_snow.data) {
		gfx_bind_texture(&texture_snow);
		for(int32_t x = px - r; x <= px + r; x++) {
			for(int32_t z = pz - r; z <= pz + r; z++) {
				enum biome b = biome_box[idx++];
				if(!precip_biome_enable_snow(b))
					continue;

				int32_t top = precip_snow_clamp_top(
					(int32_t)world_get_height(world_view_current(), x, z));
				int32_t light_y = top > interp_floor_y ? top : interp_floor_y;
				int32_t y0, y1;
				precip_column_y_range(top, py, r, &y0, &y1);
				if(y0 == y1)
					continue;

				struct java_random rng;
				precip_seed_random(&rng, x, z);
				float nf1 = jrand_next_float(&rng);
				float g1 = (float)jrand_next_gaussian(&rng);
				float nf2 = jrand_next_float(&rng);
				float g2 = (float)jrand_next_gaussian(&rng);
				float u, v;
				precip_snow_uv(nf1, g1, nf2, g2, t, v28, &u, &v);

				float dx = ((float)x + 0.5f) - player->pos[0];
				float dz = ((float)z + 0.5f) - player->pos[2];
				float d = precip_dist_factor(dx, dz, (float)r);

				float brightness = precip_light_brightness(
					world_view_current(), x, light_y, z, sun_brightness, light_rain,
					light_thunder);
				float alpha = precip_snow_alpha(d, strength);
				uint8_t color[4];
				precip_color_bytes(brightness, brightness, brightness, alpha,
								   color);

				float verts[8][3], uv[8][2];
				precip_emit_column_geometry(x, z, y0, y1, u, v, verts, uv);
				precip_draw_column(verts, uv, cam_origin, color);
			}
		}
	}

	// --- RAIN pass second, texture rain.png, gated on canSpawnLightningBolt(). ---
	// Same texture_rain.data guard as the SNOW pass above, and independent of
	// it: a pack can legitimately supply one and not the other (the sound/
	// gap-reporting precedent in make_beta_pack.ps1 treats each loader
	// read-path as its own coverage question).
	if(texture_rain.data) {
		gfx_bind_texture(&texture_rain);
		idx = 0;
		for(int32_t x = px - r; x <= px + r; x++) {
			for(int32_t z = pz - r; z <= pz + r; z++) {
				enum biome b = biome_box[idx++];
				if(!precip_biome_enable_rain(b))
					continue;

				// RAIN pass does NOT clamp top<0 -- a real, deliberate asymmetry
				// vs the SNOW pass above (see precip_snow_clamp_top's citation).
				int32_t top = (int32_t)world_get_height(world_view_current(), x, z);
				int32_t y0, y1;
				precip_column_y_range(top, py, r, &y0, &y1);
				if(y0 == y1)
					continue;

				struct java_random rng;
				precip_seed_random(&rng, x, z);
				int32_t seed = precip_column_seed(x, z);
				int32_t mask = precip_rain_and31(tick, seed);
				float nf = jrand_next_float(&rng);
				float v26 = precip_rain_v(mask, partial, nf);

				float dx = ((float)x + 0.5f) - player->pos[0];
				float dz = ((float)z + 0.5f) - player->pos[2];
				float d = precip_dist_factor(dx, dz, (float)r);

				float raw_brightness = precip_light_brightness(
					world_view_current(), x, 128, z, sun_brightness, light_rain,
					light_thunder);
				float brightness = precip_rain_brightness(raw_brightness);
				float alpha = precip_rain_alpha(d, strength);
				uint8_t color[4];
				precip_color_bytes(brightness, brightness, brightness, alpha,
								   color);

				float verts[8][3], uv[8][2];
				precip_emit_column_geometry(x, z, y0, y1, 0.0f, v26, verts, uv);
				precip_draw_column(verts, uv, cam_origin, color);
			}
		}
	}

	// Restore what was disabled/changed [740c583]. gfx_alpha_test(true)
	// restores this engine's own fixed threshold (~0.0625) rather than
	// Beta's literal 0.1F -- see gfx.h's gfx_alpha_test_ref citation for why
	// a second persistent "0.1" state doesn't exist on this engine outside
	// of this call.
	gfx_cull_func(MODE_BACK);
	gfx_blending(MODE_OFF);
	gfx_alpha_test(true);
	gfx_lighting(true); // restore the CLR0=INDEX8 world/entity contract
	gfx_matrix_modelview(view);
}

void client_add_rain_particles(void) {
	// [740c583: EntityRenderer.updateRenderer] ++rendererUpdateCount happens
	// BEFORE addRainParticles is called, in the SAME per-tick call -- this
	// tick's particle seed already sees the incremented value.
	gstate.precip_tick_count++;

	// [740c583: EntityRenderer.addRainParticles] strength = getRainStrength
	// (1.0F), which collapses to the CURRENT value exactly (prev + (cur-prev)
	// * 1 == cur) -- gstate.weather_rain_strength IS that current value.
	float strength = gstate.weather_rain_strength;
	if(!gstate.fancy_graphics)
		strength /= 2.0f;
	if(strength == 0.0f)
		return;
	if(!gstate.players[0].local_player || !gstate.world_loaded)
		return;

	struct entity* player = gstate.players[0].local_player;
	struct java_random rng;
	jrand_set_seed(&rng,
				   (int64_t)gstate.precip_tick_count * 312987231LL);

	int32_t px = (int32_t)floorf(player->pos[0]);
	int32_t py = (int32_t)floorf(player->pos[1]);
	int32_t pz = (int32_t)floorf(player->pos[2]);
	const int32_t spread = 10;

	int32_t count = (int32_t)(100.0f * strength * strength);
	double sound_x = 0.0, sound_y = 0.0, sound_z = 0.0;
	int32_t splash_count = 0;

	for(int32_t i = 0; i < count; i++) {
		int32_t x = px + jrand_next_int_bound(&rng, spread)
			- jrand_next_int_bound(&rng, spread);
		int32_t z = pz + jrand_next_int_bound(&rng, spread)
			- jrand_next_int_bound(&rng, spread);
		int32_t top = (int32_t)world_get_height(world_view_current(), x, z);
		struct block_data below
			= world_get_block(world_view_current(), x, top - 1, z);

		if(top > py + spread || top < py - spread)
			continue;

		uint8_t gi, gj;
		enum biome b = world_get_section_tint_at(world_view_current(), x, z, &gi,
												  &gj)
			? precip_biome_from_tint(gi, gj)
			: BIOME_PLAINS;
		if(!precip_biome_enable_rain(b))
			continue;

		float rx = jrand_next_float(&rng);
		float rz = jrand_next_float(&rng);
		if(below.type == BLOCK_AIR)
			continue;

		// render-slice scoped cut (RENDER_SLICE_NOTES.md): Beta offsets the
		// splash Y by `top+0.1F - block.minY` (the struck block's own
		// bounding-box top, e.g. a slab/snow-layer sits higher than 0). This
		// port uses minY=0 for every block type -- imperceptible for a
		// ~0.01-size splash sprite, and this engine has no generic per-block
		// minY accessor to query faithfully without a larger, unrelated change.
		double spawn_x = (double)((float)x + rx);
		double spawn_y = (double)((float)top + 0.1f);
		double spawn_z = (double)((float)z + rz);

		if(below.type == BLOCK_LAVA_FLOW || below.type == BLOCK_LAVA_STILL) {
			// [740c583: EntitySmokeFX over lava] render-slice scoped cut: this
			// engine's particle integrator is simpler than EntitySmokeFX (see
			// particle_generate_tnt_smoke's own established precedent for
			// this exact tradeoff) -- spawn count/origin match; per-tick
			// physics (drift/scale-over-life) is this engine's generic model.
			vec3 pos = {(float)spawn_x, (float)spawn_y, (float)spawn_z};
			vec3 vel = {0.0f, 0.02f, 0.0f};
			uint8_t tex = tex_atlas_lookup_particle(TEXAT_PARTICLE_SMOKE_0);
			particle_add(pos, vel, tex, 0.15f, 16.0f, false, 180, 180, 180,
						false, TEXTURE_ATLAS_PARTICLES);
		} else {
			splash_count++;
			if(jrand_next_int_bound(&rng, splash_count) == 0) {
				sound_x = spawn_x;
				sound_y = spawn_y;
				sound_z = spawn_z;
			}
			// [740c583: EntityRainFX] render-slice scoped cut: spawn
			// position/count are exact; the per-tick physics (gravity 0.06,
			// 0.98 air damping, onGround/liquid-surface early death) is this
			// engine's generic gravity-particle model, not a bit-exact port
			// -- see the SAME simpler-integrator precedent noted above.
			// particleTextureIndex = 19 + rand.nextInt(4) in Beta; this
			// engine's atlas uses named DROP_TINY tiles (4 variants, matching
			// Beta's own choice count) rather than Beta's raw numeric index.
			vec3 pos = {(float)spawn_x, (float)spawn_y, (float)spawn_z};
			vec3 vel = {0.0f, 0.1f + rand_gen_flt(&gstate.rand_src) * 0.05f,
					   0.0f};
			int variant = (int)(rand_gen_flt(&gstate.rand_src) * 4.0f);
			if(variant > 3)
				variant = 3;
			uint8_t tex = tex_atlas_lookup_particle(
				(enum tex_atlas_entry)(TEXAT_PARTICLE_DROP_TINY_0 + variant));
			particle_add(pos, vel, tex, 0.01f, 8.0f, true, 255, 255, 255,
						false, TEXTURE_ATLAS_PARTICLES);
		}
	}

	// [740c583] rainSoundCounter gate: fires when a splash happened THIS
	// tick and the counter (post-increment) exceeds a fresh nextInt(3).
	if(splash_count > 0
	   && jrand_next_int_bound(&rng, 3) < gstate.rain_sound_counter++) {
		gstate.rain_sound_counter = 0;
		// [740c583 EntityRenderer.renderRainSnow:625-630]: splash landing
		// ABOVE the listener (and the listener's own column's top solid
		// above their feet -- findTopSolidBlock > floor(posY)) plays muffled
		// (0.1F, 0.5F); otherwise the ordinary patter (0.2F, 1.0F). The key
		// was registered 2026-08-31 (audio_sound_table.h); the RNG/counter
		// above were already byte-exact.
		vec3 sp = {(float)sound_x, (float)sound_y, (float)sound_z};
		bool above = sound_y > gstate.players[0].camera.y + 1.0
			&& (double)world_get_height(world_view_current(),
					(w_coord_t)floor(gstate.players[0].camera.x),
					(w_coord_t)floor(gstate.players[0].camera.z))
				> floor(gstate.players[0].camera.y);
		if(above)
			audio_play3d(SOUND_AMBIENT_WEATHER_RAIN, sp, 0.1F, 0.5F);
		else
			audio_play3d(SOUND_AMBIENT_WEATHER_RAIN, sp, 0.2F, 1.0F);
	}
}
