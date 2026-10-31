/*
	Beta 1.7.3 TextureWatchFX port -- a 16x16 procedural ANIMATED clock dial.

	Faithful to net.minecraft.src.TextureWatchFX.onTick (decompile 740c583): each
	20Hz client tick, the dial's target angle is the world's celestial angle
	(negated and doubled to a full turn per day), the SAME damped-spring filter
	compass_fx.c uses smooths it into a displayed angle, and every "hand marker"
	texel of the static clock icon (identified by a colour-key: R==B, G==0, B>0)
	is replaced by sampling a rotated lookup into the separate dial texture,
	tinted by the marker's own red channel; every other texel passes through
	unchanged.

	Engine-free generator (host-testable, links standalone), mirroring
	compass_fx.h/water_fx.h. Runtime wiring lives in source/platform/texture.c.

	ASSET GAP (documented, not a bug): Beta's dial texture is misc/dial.png, an
	asset this repo does not bundle (Mojang IP -- same policy as every other
	Beta-exclusive asset absent from the fCavEX pack, e.g. texture_painting's
	art/kz.png). texture_dial.data stays NULL until a real Beta pack supplies it;
	the runtime consumer gates the whole clock-needle FX on texture_dial.data !=
	NULL and otherwise leaves the static icon in place (the existing documented
	deviation in items_object.h), exactly like texture_painting's render function
	checking .data before drawing.

	RENDER DEVIATION (documented, non-simulation, same rationale as compass_fx.h):
	the nether random-spin branch takes a caller-supplied already-drawn [0,1)
	value (`nether_rand01`) instead of owning an RNG stream.

	Algorithm (every constant load-bearing):
	  1. target angle: 0.0 if there is no world/player yet; else
	     `-celestial_angle * 2*PI` [740c583:
	     `-World.getCelestialAngle(1.0F) * PI * 2`]; overridden to
	     `nether_rand01 * 2*PI` when `is_nether`.
	  2. damped-spring smoothing: identical formula/state shape to compass_fx.c
	     (a separate `struct watch_fx_state` instance -- Beta uses separate fields
	     field_4222_j/field_4221_k, never shared with the compass's).
	  3. per-texel (256 of them), for the base clock icon pixel (r,g,b,a):
	       if NOT (r==b && g==0 && b>0): out texel = base texel, unchanged.
	       else (a hand-marker texel at column `i%16`, row `i/16`):
	         u = -( (i%16)/15.0 - 0.5 ), v = (i/16)/15.0 - 0.5
	         rotate by the smoothed angle: (u*cos+v*sin+0.5)*16 -> ix,
	                                       (v*cos-u*sin+0.5)*16 -> iy
	         dial_idx = (ix & 15) + (iy & 15) * 16
	         out = (dial.r * r / 255, dial.g * r / 255, dial.b * r / 255, dial.a)
	             -- all three colour channels tint by the marker's OWN red value
	                (Beta's var18, captured before var11 is overwritten -- same
	                 numeric value as the pre-overwrite var11 the R channel
	                 expression reads, see the .c file's citation).
*/

#ifndef GRAPHICS_WATCH_FX_H
#define GRAPHICS_WATCH_FX_H

#include <stdbool.h>
#include <stdint.h>

#define WATCH_FX_DIM 16
#define WATCH_FX_TEXELS (WATCH_FX_DIM * WATCH_FX_DIM) // 256
#define WATCH_FX_BYTES (WATCH_FX_TEXELS * 4)          // 1024, RGBA8

// Smoothing state carried between ticks -- Beta's TextureWatchFX private fields
// field_4222_j (current displayed angle, radians) and field_4221_k (its
// velocity). Kept in its own struct (never shared with compass_fx_state) because
// Beta's two FX instances own independent fields.
struct watch_fx_state {
	double current_angle; // field_4222_j
	double angle_delta;   // field_4221_k
};

void watch_fx_state_init(struct watch_fx_state* s);

// One 20Hz client tick [740c583: TextureWatchFX.onTick]. `base_icon` is the
// captured static clock icon (16x16 RGBA8, R,G,B,A per texel) read once from
// items.png; `dial` is the captured misc/dial.png (16x16 RGBA8, same layout) --
// pass NULL to leave every texel a verbatim copy of base_icon (the caller's
// asset-gap gate; see the header comment above). `out` receives the composited
// 256-texel RGBA8 frame. Aliasing `out == base_icon` is fine.
//
//   has_target      - mirrors `mc.theWorld != null && mc.thePlayer != null`.
//   is_nether       - mirrors `worldProvider.isNether` (has_target-gated).
//   nether_rand01   - caller-drawn Math.random()-equivalent in [0,1).
//   celestial_angle - World.getCelestialAngle(1.0F) (radians-normalized turn
//                     fraction the same way daytime_celestial_angle() already
//                     returns it for the sky renderer -- see daytime.h); ignored
//                     unless has_target.
void watch_fx_tick(struct watch_fx_state* s,
				   const uint8_t base_icon[WATCH_FX_BYTES],
				   const uint8_t* dial /* WATCH_FX_BYTES or NULL */,
				   bool has_target, bool is_nether, double nether_rand01,
				   double celestial_angle, uint8_t out[WATCH_FX_BYTES]);

#endif
