/*
	Beta 1.7.3 TextureCompassFX port -- a 16x16 procedural ANIMATED compass needle.

	Faithful to net.minecraft.src.TextureCompassFX.onTick (decompile 740c583): each
	20Hz client tick, the needle's target angle is recomputed from the player's yaw
	and position relative to the world spawn point, a damped-spring filter smooths
	that target into a displayed angle (the same two-field filter TextureWatchFX
	uses), and the needle is redrawn over a copy of the static compass icon.

	This header declares the PURE, engine-free generator (host-testable, links
	standalone -- no cglm/game_state, matching flame_fx.h/water_fx.h). The runtime
	wiring (capturing the static icon from texture_items, calling this once per
	20Hz tick, and sub-uploading the result back into texture_items) lives in
	source/platform/texture.c, mirroring the water_fx_anim_init/_tick split.

	RENDER DEVIATION (documented, non-simulation): the nether random-spin branch
	needs a genuine RNG draw (Beta calls Math.random()); like water_fx/flame_fx's
	self-contained LCG, this is a render-layer, never-parity-checked cosmetic, so
	the caller supplies an already-drawn [0,1) value (`nether_rand01`) rather than
	this module owning a third RNG stream (CODESTYLE.md's two-RNG rule). Also: Beta
	spells PI two ways across this one method -- the wraparound/offset math uses
	the double literal 3.141592653589793 (== M_PI to full double precision), but
	the nether branch multiplies by 3.1415927410125732 (a FLOAT pi widened to
	double). Both are used here as a single COMPASS_FX_PI (== the first, more
	precise literal) -- the two differ by ~1e-8 radians, invisible on a 16px
	needle and moot on a branch that is already non-reproducible RNG output.

	Algorithm (every constant load-bearing):
	  1. out[] = a verbatim copy of the captured static icon (Beta re-packs every
	     texel unchanged apart from an anaglyph/stereo-3D branch CaveWii never
	     enables).
	  2. target angle: 0.0 if there is no world/player yet; else
	     `player_yaw_rad - PI/2 - atan2(spawn_z - player_z, spawn_x - player_x)`
	     [740c583: `(rotationYaw - 90) * PI/180 - atan2(dz, dx)`; player_yaw_rad is
	     ALREADY rotationYaw*PI/180 in the same atan2(dz,dx) convention documented
	     at entity_local_player.c's boat-mount code and entity_boat.c's turn-
	     toward-motion yaw, verified against EntityBoat.onUpdate's own
	     `atan2(dz,dx)*180/PI -> rotationYaw` citation]; overridden to
	     `nether_rand01 * 2*PI` when `is_nether`.
	  3. damped-spring smoothing (state persists across ticks):
	       delta = wrap(target - current_angle) into [-PI, PI), clamped to [-1, 1]
	       angle_delta = (angle_delta + delta * 0.1) * 0.8
	       current_angle += angle_delta
	  4. needle draw, two overlapping strokes at sin/cos(current_angle):
	       tail (south, step i in [-4,4]): x=(int)(8.5+cos*i*0.3),
	         y=(int)(7.5-sin*i*0.3*0.5), colour (100,100,100,255) gray.
	       head (step i in [-8,16]): x=(int)(8.5+sin*i*0.3),
	         y=(int)(7.5+cos*i*0.3*0.5), colour (255,20,20,255) red when
	         i>=0 (the north tip) else (100,100,100,255) gray.
	     Both loops write straight into out[] at (y*16+x)*4, overwriting the icon
	     copy pixel-by-pixel exactly like Beta's in-place imageData writes.
*/

#ifndef GRAPHICS_COMPASS_FX_H
#define GRAPHICS_COMPASS_FX_H

#include <stdbool.h>
#include <stdint.h>

#define COMPASS_FX_DIM 16
#define COMPASS_FX_TEXELS (COMPASS_FX_DIM * COMPASS_FX_DIM) // 256
#define COMPASS_FX_BYTES (COMPASS_FX_TEXELS * 4)            // 1024, RGBA8

// Smoothing state carried between ticks -- Beta's TextureCompassFX private fields
// field_4229_i (current displayed angle, radians) and field_4228_j (its velocity).
struct compass_fx_state {
	double current_angle; // field_4229_i
	double angle_delta;   // field_4228_j
};

void compass_fx_state_init(struct compass_fx_state* s);

// One 20Hz client tick [740c583: TextureCompassFX.onTick]. Pure function of its
// arguments plus `s` (read AND written -- the damped-spring filter is stateful in
// Beta too). `base_icon` is the captured static compass icon (16x16 RGBA8, R,G,B,A
// per texel) read once from items.png at texture load time; `out` receives the
// same 256-texel RGBA8 layout with the needle composited on top. Aliasing `out ==
// base_icon` is fine (the icon copy happens first, into `out`, before any needle
// pixel is touched).
//
//   has_target   - mirrors `mc.theWorld != null && mc.thePlayer != null`; false
//                  pins the target angle at 0.0, matching Beta's declared default.
//   is_nether    - mirrors `worldProvider.isNether` (consulted only when
//                  has_target, matching Beta's nesting).
//   nether_rand01- the caller-drawn Math.random()-equivalent in [0,1); ignored
//                  unless has_target && is_nether.
//   spawn_x/z    - World.getSpawnPoint(), block coordinates widened to double
//                  (ChunkCoordinates.x/z are ints in Beta too).
//   player_x/z   - EntityPlayer.posX/posZ (X/Z are eye/feet-height invariant).
//   player_yaw_rad - EntityPlayer.rotationYaw already in radians (see the header
//                  comment above for the exact convention this repo already
//                  relies on for boats).
void compass_fx_tick(struct compass_fx_state* s,
					 const uint8_t base_icon[COMPASS_FX_BYTES], bool has_target,
					 bool is_nether, double nether_rand01, double spawn_x,
					 double spawn_z, double player_x, double player_z,
					 double player_yaw_rad, uint8_t out[COMPASS_FX_BYTES]);

#endif
