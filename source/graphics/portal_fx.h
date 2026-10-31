/*
	Beta 1.7.3 TexturePortalFX port -- the procedural ANIMATED nether-portal tile.

	Faithful to net.minecraft.src.TexturePortalFX (decompile 740c583): unlike the
	lava/flame FX (live per-tick sims), the portal effect is PRECOMPUTED -- the
	constructor builds all 32 16x16 RGBA frames once from a Random with the FIXED
	seed 100, and onTick just cycles them (tick & 31). That makes this port fully
	deterministic and byte-reproducible: same seed, same frames, every run.

	Algorithm per frame f (0..31), per texel (x,y):
	  - two spiral layers l = 0,1 with centres (0,0) and (8,8): wrap
	    u=(x-8l)/16*2, v=(y-8l)/16*2 into [-1,1); r2=u*u+v*v;
	    a = atan2(v,u) + (f/32*2PI - r2*10 + 2l) * (2l-1)   [layer 0 counter-
	    rotates, layer 1 rotates; the -r2*10 term twists the spiral arms];
	    intensity += ((mh_sin(a)+1)/2 / (r2+1)) * 0.5       [MathHelper sin TABLE].
	  - + nextFloat()*0.1 sparkle (ONE java Random draw per texel, frame-major,
	    x outer / y inner -- draw order load-bearing for reproducibility).
	  - colour: R=(int)(i*i*200+55), G=(int)(i^4*255), B=(int)(i*100+155),
	    A=(int)(i*100+155)  [the purple ramp; alpha 155..255 = translucent core].
	  - texel index y*16+x.

	Like lava_fx/flame_fx this is engine-free and app-UNUSED at runtime: the
	offline generator (tools/gen_anim_fx) bakes the frames into a pack's anim.png
	filmstrip (portal cell = columns 0 & 7), which world.c's matrix_anim scroll
	plays back. The 28-frame strip samples the 32-frame cycle evenly (floor(f*8/7),
	skipping every 8th frame) so the strip spans EXACTLY one full spiral revolution
	-- a seamless loop. Documented T2 cadence deviation (shared with the fire/lava
	strips' STRIDE): the scroll runs 28 frames at 8.75 fps (3.2 s/rev) vs Beta's
	32 at 20 fps (1.6 s/rev); every FRAME shown is a byte-authentic Beta frame.
*/

#ifndef GRAPHICS_PORTAL_FX_H
#define GRAPHICS_PORTAL_FX_H

#include <stdint.h>

#define PORTAL_FX_DIM 16
#define PORTAL_FX_TEXELS (PORTAL_FX_DIM * PORTAL_FX_DIM)
#define PORTAL_FX_BYTES (PORTAL_FX_TEXELS * 4)
#define PORTAL_FX_FRAMES 32

// Generate all 32 frames exactly as Beta's constructor does (fixed seed 100).
// `out` receives PORTAL_FX_FRAMES * PORTAL_FX_BYTES bytes, frame-major RGBA.
void portal_fx_generate(uint8_t* out);

// Same constructor field before colouring: PORTAL_FX_FRAMES * PORTAL_FX_TEXELS
// floats, frame-major, texel y*16+x. Intensities above 1 are preserved.
void portal_fx_generate_intensity(float* out);

// Pure per-texel colour mapping (exposed for the golden test): intensity i ->
// R=(int)(i*i*200+55), G=(int)(i*i*i*i*255), B=(int)(i*100+155),
// A=(int)(i*100+155). out must hold 4 bytes.
void portal_fx_color(float intensity, uint8_t out[4]);

#endif
