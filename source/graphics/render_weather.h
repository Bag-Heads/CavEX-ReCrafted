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

#ifndef GRAPHICS_RENDER_WEATHER_H
#define GRAPHICS_RENDER_WEATHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../cglm/cglm.h"

#include "../worldgen/biome.h"
#include "../worldgen/java_random.h"

// RENDER_DISPATCH.md: Beta 1.7.3 precipitation render (render-slice, base
// c5c696a). Citations throughout carry the `[740c583: EntityRenderer.<method>]`
// form used across this project (see .claude/skills/beta-decompile).
//
// -------------------------------------------------------------------------
// Entry points (called from main.c) -- these issue the actual GX/gfx draw
// calls and are not directly unit-testable; everything they call is.
// -------------------------------------------------------------------------

// [740c583: EntityRenderer.renderRainSnow] Called once per RENDERED FRAME.
// `partial` is this frame's client-tick interpolation fraction (main.c's
// `tick_delta`, Beta's `partialTicks`).
struct entity;
void render_rain_snow_owned(float partial, const struct entity* player, mat4 view);
void render_rain_snow(float partial);

// [740c583: EntityRenderer.addRainParticles] Called once per 20 Hz CLIENT
// TICK (main.c's `while(tick_delta >= 1.0F ...)` catch-up loop), alongside
// particle_update()/client_display_tick() -- Beta's own addRainParticles runs
// from updateRenderer(), itself a per-tick (not per-frame) call: its
// rendererUpdateCount increment and fogColor exponential-smoothing step only
// make sense at a fixed tick rate.
void client_add_rain_particles(void);

// -------------------------------------------------------------------------
// Pure, testable building blocks (tests/test_precip_render_env.c). None of
// these touch gfx/GX state, world storage, or gstate -- each is a hand-
// verifiable function of its arguments.
// -------------------------------------------------------------------------

// The per-column deterministic seed, IDENTICAL in both the snow and rain
// passes: x*x*3121 + x*45238971 + z*z*418711 + z*13761, evaluated with Java's
// int32 wraparound semantics at every intermediate operation (Java evaluates
// left-to-right; each binary op is itself a 32-bit wrapping op -- see
// server_weather.c's existing position LCG for this project's established
// idiom for reproducing that in C). The result is a genuine Java `int`
// (32-bit); passing it to `random.setSeed(long)` sign-extends it, which is
// exactly what int32_t -> int64_t conversion in C does.
// [740c583: EntityRenderer.renderRainSnow]
int32_t precip_column_seed(int32_t x, int32_t z);

// random.setSeed(precip_column_seed(x, z)) -- the sign-extending int32->int64
// widening Java performs implicitly at the call site.
void precip_seed_random(struct java_random* r, int32_t x, int32_t z);

// SNOW pass only: `top = findTopSolidBlock(x,z); if (top < 0) top = 0;`. The
// RAIN pass does NOT apply this clamp (a real, easy-to-miss asymmetry in the
// decompile). [740c583: EntityRenderer.renderRainSnow]
int32_t precip_snow_clamp_top(int32_t top);

// The column's Y draw range, IDENTICAL formula for both passes given each
// pass's own (possibly clamped) `top`: y0 = max(py - r, top), y1 =
// max(py + r, top). The caller must skip the column entirely when
// `*y0 == *y1` (both collapsed to `top` because the whole [py-r, py+r]
// window sits below the column's solid surface) -- this is how a silently
// dropped quad would show up as a quad-count regression.
// [740c583: EntityRenderer.renderRainSnow]
void precip_column_y_range(int32_t top, int32_t py, int32_t r, int32_t* y0,
						   int32_t* y1);

// Gate: `strength = world.getRainStrength(partial); if (strength <= 0)
// return;` -- true means "draw", matching Beta's `if (var2 > 0.0F) { ... }`
// (there is no lower-bound/negative special case; a non-positive strength,
// including exactly 0, always means "draw nothing"). [740c583:
// EntityRenderer.renderRainSnow]
bool precip_strength_active(float strength);

// d = sqrt(dx*dx + dz*dz) / r, where dx,dz = (column centre) - (player pos).
// [740c583: EntityRenderer.renderRainSnow]
float precip_dist_factor(float dx, float dz, float r);

// SNOW alpha: ((1 - d*d) * 0.3 + 0.5) * strength.
float precip_snow_alpha(float d, float strength);
// RAIN alpha: ((1 - d*d) * 0.5 + 0.5) * strength.
float precip_rain_alpha(float d, float strength);
// RAIN brightness: raw_light_brightness * 0.85 + 0.15 (SNOW uses the raw
// getLightBrightness value directly -- no transform).
float precip_rain_brightness(float raw_light_brightness);

// SNOW drift u/v (Beta's drift u/v locals), decoupled from the RNG so the FORMULA
// is hand-verifiable independent of any nextFloat()/nextGaussian() sequence:
//   u = next_float_1 + t * 0.01F * gaussian_1
//   v = next_float_2 + t * gaussian_2 * 0.001F + v28
// (v28 folded in here since Beta always adds it alongside drift v at the UV
// site: v28*k + drift_v*k with k constant 1 -- see the geometry
// function below). draw order is next_float_1, gaussian_1, next_float_2,
// gaussian_2 -- Java evaluates `a.nextFloat() + x*y*(float)a.nextGaussian()`
// left-to-right. [740c583: EntityRenderer.renderRainSnow]
void precip_snow_uv(float next_float_1, float gaussian_1, float next_float_2,
					float gaussian_2, float t, float v28, float* u, float* v);

// RAIN fall v (Beta's var26): rc_seed_and31 = (rendererUpdateCount +
// precip_column_seed(x,z)) & 31 -- ALREADY masked by the caller (pure int
// masking, hand-verifiable on its own; see precip_rain_and31 below).
//   v = (rc_seed_and31 + partial) / 32 * (3 + next_float)
// [740c583: EntityRenderer.renderRainSnow]
float precip_rain_v(int32_t rc_seed_and31, float partial, float next_float);

// (rendererUpdateCount + precip_column_seed(x,z)) & 31, Java int32 wraparound
// on the sum before masking (masking itself is bit-pattern-exact regardless
// of sign, but spelled out as its own function so the sum's wraparound is
// independently verifiable). [740c583: EntityRenderer.renderRainSnow]
int32_t precip_rain_and31(int32_t renderer_update_count, int32_t seed);

// Emits ONE column's "double cross" -- TWO quads (8 vertices total, GX_QUADS
// order): one in the X-plane at z+0.5 spanning x..x+1, one in the Z-plane at
// x+0.5 spanning z..z+1, both from y0 to y1. u_bias is snow's `u` (0 for
// rain, which has no U jitter -- see the decompile's `0.0F*var37` == 0.0F
// term). v_bias is snow's `v28+v` or rain's `precip_rain_v(...)` result. The
// /4.0F per-Y-unit V pitch and the Beta `var26`/`var37` "scale" locals (both
// always exactly 1.0F when a quad is drawn -- read but never reassigned
// inside the `if` that guards the draw) are folded in as constants; they are
// NOT parameters because they never vary. Pure geometry -- no GX/gfx calls.
// Always returns 8 (a silent quality cut that dropped a quad would show up
// as this count changing). [740c583: EntityRenderer.renderRainSnow]
size_t precip_emit_column_geometry(int32_t x, int32_t z, int32_t y0,
								   int32_t y1, float u_bias, float v_bias,
								   float verts_out[8][3], float uv_out[8][2]);

// Beta BiomeGenBase's decision tree [740c583: BiomeGenBase.getBiome],
// reconstructed CLIENT-SIDE from a column's already-shipped grass/foliage
// colormap indices (grass_i/grass_j -- worldgen_climate, worldgen.c) because
// the client has no worldgen generator handle of its own (see
// RENDER_SLICE_NOTES.md, "biome reconstruction", for why: extending the wire
// protocol to carry biome directly would require touching server_local.c /
// server_world.c, off limits for this slice). biome_get's decision tree
// multiplies humidity by temperature as its FIRST statement and never uses
// raw humidity again -- so it is really a function of (temperature,
// temperature*humidity). worldgen_climate computes grass_i = (1-temp)*255
// and grass_j = (1-temp*humid)*255 (both truncated+clamped bytes) from the
// SAME temp/humid pair biome_classify uses -- recovering temp_approx and
// product_approx from each byte's BUCKET MIDPOINT (not its edge -- see
// precip_biome_from_tint's own comment) and running the SAME thresholds
// against (temp_approx, product_approx) DIRECTLY (no division, so no error
// amplification near temp=0) reproduces the server's true classification
// whenever the real (temp, temp*humid) isn't within one byte-quantization
// step of a threshold. MEASURED (test_precip_render_env.c, a uniform
// (temp,humid) sweep): ~95% exact-match against worldgen/biome.c's
// biome_classify (the ground truth this approximates) -- HIGHER than a
// naive "one threshold, ~0.4% band" estimate predicts, because
// BiomeGenBase.getBiome has ~9 distinct threshold segments in this 2D
// space, not one (confirmed: the decision tree was hand-diffed line-for-
// line against biome_get and is identical, and a midpoint-vs-edge
// dequantization fix only closed part of the gap -- see
// test_precip_render_env.c's own comment for the full measurement trail).
// In practice this reads as "a biome boundary's exact column can be off by
// one or two", not "columns render randomly wrong": real terrain has
// spatially correlated climate, so a precipitation box only ever straddles
// a genuine edge near an actual biome boundary.
enum biome precip_biome_from_tint(uint8_t grass_i, uint8_t grass_j);

// [740c583: BiomeGenBase.getEnableSnow] taiga/iceDesert/tundra only.
bool precip_biome_enable_snow(enum biome b);
// [740c583: BiomeGenBase.canSpawnLightningBolt] `enableSnow ? false :
// enableRain` -- collapses to "not a snow biome, and not desert/iceDesert".
bool precip_biome_enable_rain(enum biome b);

#endif
