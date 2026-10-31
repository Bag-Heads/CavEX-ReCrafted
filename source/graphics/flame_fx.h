/*
	Beta 1.7.3 TextureFlamesFX port -- a procedural ANIMATED fire tile.

	Faithful to net.minecraft.src.TextureFlamesFX.onTick (decompile 740c583): a
	16x20 upward-advection field with a random source row at the bottom, colouring a
	16x16 RGBA8 tile (the top 16 rows), advanced once per 20Hz client tick and blitted
	into the terrain-atlas fire slot in place.

	TWO INSTANCES (fire-anim-0924/F1): Beta registers TextureFlamesFX(0) and
	TextureFlamesFX(1) (Minecraft.java:282-283) for the fire cell and the cell one
	row below it (TextureFlamesFX.java:8, blockIndexInTexture + var1*16). Each owns
	its own field arrays but both draw from the one global Math.random() stream, in
	registration order (RenderEngine.java:346-349 ticks each as it is registered,
	:363-366 ticks the list 0 then 1 every client tick). struct flame_fx_pair below
	reproduces exactly that: two independent states, ONE shared RNG stream, 0 then 1.
	Like water_fx/lava_fx the RNG is a self-contained LCG (render-layer, non-parity:
	no Math.random() stream to match). Which consumers sample instance 1 is the
	renderer's business (RenderBlocks.renderBlockFire, ItemRenderer
	.renderFireInFirstPerson, Render.renderEntityOnFire); see docs/deviations.md.

	Algorithm (every constant load-bearing, matched to TextureFlamesFX.onTick):
	  - per texel (x in 0..16, y in 0..20): var4 = field[x + ((y+1)%20)*16] * 18, then
	    add the in-bounds cells of the 3x2 box (x-1..x+1, y..y+1); divide by 24*1.06.
	  - the bottom row (y>=19) is reseeded to (float)(r1*r2*r3*4.0D + r4*(double)0.1F
    + (double)0.2F) -- evaluated in DOUBLE like Java, 4 randoms left to right.
	  - swap field/field_next.
	  - colour (top 256 texels): v = clamp(field*1.8, 0, 1); R=v*155+100, G=v^2*255,
	    B=v^10*255, A = v<0.5 ? 0 : 255.
*/

#ifndef GRAPHICS_FLAME_FX_H
#define GRAPHICS_FLAME_FX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FLAME_FX_DIM 16
#define FLAME_FX_ROWS 20                       // Beta's 16x20 field (4 feed rows)
#define FLAME_FX_FIELD (FLAME_FX_DIM * FLAME_FX_ROWS) // 320
#define FLAME_FX_TEXELS (FLAME_FX_DIM * FLAME_FX_DIM)  // 256 visible
#define FLAME_FX_BYTES (FLAME_FX_TEXELS * 4)

struct flame_fx {
	float field[FLAME_FX_FIELD];      // current 16x20 advection field
	float field_next[FLAME_FX_FIELD]; // scratch written each tick
	uint32_t prng;                    // self-contained LCG state
	uint8_t rgba[FLAME_FX_BYTES];     // 16x16 RGBA8 output, refreshed every tick
};

// Pure sim (engine-free).
void flame_fx_init(struct flame_fx* w, uint32_t seed);
void flame_fx_tick(struct flame_fx* w); // advance one 20Hz tick + refresh rgba
const uint8_t* flame_fx_pixels(const struct flame_fx* w);

// Beta's two TextureFlamesFX instances. fx[0] = Block.fire.blockIndexInTexture,
// fx[1] = that + 16 (the row below). The per-instance prng fields are scratch: the
// ONE stream lives in `prng` and is threaded through fx[0] then fx[1] each tick.
#define FLAME_FX_INSTANCES 2
struct flame_fx_pair {
	struct flame_fx fx[FLAME_FX_INSTANCES];
	uint32_t prng; // the shared stream (Java's single global Math.random())
};

// Zero both fields, then run Beta's registration ticks (registerTextureFX calls
// onTick() as each is added: instance 0, then instance 1).
void flame_fx_pair_init(struct flame_fx_pair* p, uint32_t seed);
// One 20Hz client tick: instance 0 then instance 1, sharing one RNG stream.
void flame_fx_pair_tick(struct flame_fx_pair* p);
// 16x16 RGBA8 of instance `index` (0 = fire cell, 1 = fire cell + 16).
const uint8_t* flame_fx_pair_pixels(const struct flame_fx_pair* p, int index);

// --- where the two tiles live, and who samples the second -----------------
// anim.png strips baked by tools/gen_anim_fx from ONE flame_fx_pair: frame f of
// a strip whose first column is C sits at (C, f) for f < 14, else (C + 7, f - 14)
// (world.c matrix_anim). fx[0] = Beta's fire cell, fx[1] = Beta's fire+16 cell.
#define FLAME_FX_ANIM_COL0 3 // cols 3 & 10 (block_fire.c TEXTURE_INDEX(3,0))
#define FLAME_FX_ANIM_COL1 6 // cols 6 & 13 (no other anim.png user)

// PACK-LOAD FALLBACK. A pack may ship its own anim.png without the second
// strip: fully transparent at cols 6 & 13 (the CC0 Earth & Ash pack) or the
// magenta placeholder the bundled atlas carried before 2026-09-24. tex_init
// repairs the DECODED image before upload (flame_fx_anim_prepare_pack_atlas:
// cols 3/10 copied into 6/13), so every consumer samples cols 6/13 for fire+16
// UNCONDITIONALLY. No runtime flag on purpose: textures can be re-applied
// in-game while chunk display lists hold baked fire UVs, and the mesher runs on
// its own thread -- a repaired texture is right for every list, old or new.

// Strip (0 fire, 1 fire+16) -> its first anim.png column (frames 14..27 are 7
// columns right).
static inline int flame_fx_anim_col(int strip) {
	return strip ? FLAME_FX_ANIM_COL1 : FLAME_FX_ANIM_COL0;
}

// SCALE-AWARE GEOMETRY. The renderer addresses anim.png in 1/256 atlas units
// (s,t bytes; an 18-unit cell pitch, content at +3), whatever the image size:
// on an n-pixel axis, pixel p covers units [p*256/n, (p+1)*256/n) and its
// centre sits at (2p+1)*128/n. A cell region [a, b) (atlas units) is the set of
// pixels whose CENTRE lies in it -- for n a multiple of 256 that is exactly
// pixels [a*n/256, b*n/256), the old integer-scale layout, byte for byte. For
// n >= 128 it also holds every pixel a nearest (GX_NEAR, tex_gfx_load linear =
// false) sample of a content texel [c+3, c+19) can land on: that pixel's centre
// is within 128/n <= 1 unit of the sample, inside the cell's 1-unit border.
// Below 128 px per axis a cell is under 9 px; such an atlas is left alone.
#define FLAME_FX_ANIM_MIN_PX 128

// First pixel on an n-pixel axis whose centre is at or right of atlas unit `u`
// (smallest p with (2p+1)*128 >= u*n), clamped to n.
static inline size_t flame_fx_anim_px_from(size_t u, size_t n) {
	size_t num = u * n;
	size_t p = num <= 128 ? 0 : (num - 128 + 255) / 256;
	return p < n ? p : n;
}

// True iff the decoded RGBA8 atlas carries a real second strip: EVERY one of the
// 28 16x16 content cells at cols 6 & 13 (rows 0..13, content at +3 in the 18-unit
// pitch, mapped to pixels by the scale-aware rule above, any image size >= 128)
// holds at least one texel that is neither fully transparent (alpha 0) nor one of
// the placeholder's two magenta colours (107,63,127) / (214,127,255). A flame
// texel can be neither (alpha 255 needs v >= 0.5, i.e. G >= 63 with R >= 177).
// Partly blank -> false, so a strip never flickers to nothing on some frames.
static inline bool flame_fx_anim_second_strip_present(const uint8_t* rgba,
													  size_t w, size_t h) {
	if(!rgba || w < FLAME_FX_ANIM_MIN_PX || h < FLAME_FX_ANIM_MIN_PX)
		return false;
	for(int c = 0; c < 2; c++) {
		size_t cu = (size_t)(FLAME_FX_ANIM_COL1 + c * 7) * 18 + 3;
		size_t x0 = flame_fx_anim_px_from(cu, w);
		size_t x1 = flame_fx_anim_px_from(cu + 16, w);
		for(int row = 0; row < 14; row++) {
			size_t rv = (size_t)row * 18 + 3;
			size_t y0 = flame_fx_anim_px_from(rv, h);
			size_t y1 = flame_fx_anim_px_from(rv + 16, h);
			bool content = false;
			for(size_t y = y0; !content && y < y1; y++)
				for(size_t x = x0; x < x1; x++) {
					const uint8_t* px = rgba + (y * w + x) * 4;
					bool magenta = px[3] == 255
						&& ((px[0] == 107 && px[1] == 63 && px[2] == 127)
							|| (px[0] == 214 && px[1] == 127 && px[2] == 255));
					if(px[3] != 0 && !magenta) {
						content = true;
						break;
					}
				}
			if(!content)
				return false;
		}
	}
	return true;
}

// Copy the fire strip's 28 cell blocks (18x18 units incl. the 1-unit border)
// from cols 3/10 into cols 6/13 when the second strip is absent, so the fire+16
// users show the fire tile (the pre-2026-09-24 picture) instead of transparent/
// magenta cells. Scale-aware for ANY image size >= 128 px per axis: every
// destination pixel whose centre lies in the cols-6/13 band (units [c*18+2,
// c*18+20) x [2, 254)) takes the source pixel containing that centre shifted
// 54 units left, i.e. floor(((2x+1)*128 - 54*w) / 256) -- exactly x - 54*w/256
// when w is a multiple of 256 (the old integer-scale copy). Rows are the same
// pixels (same atlas rows). Returns true iff it copied. Leaves an atlas with a
// real second strip, or one under 128 px on an axis, untouched.
static inline bool flame_fx_anim_prepare_pack_atlas(uint8_t* rgba, size_t w,
													size_t h) {
	if(!rgba || w < FLAME_FX_ANIM_MIN_PX || h < FLAME_FX_ANIM_MIN_PX
	   || flame_fx_anim_second_strip_present(rgba, w, h))
		return false;
	const size_t shift_u = (size_t)(FLAME_FX_ANIM_COL1 - FLAME_FX_ANIM_COL0) * 18;
	const size_t y0 = flame_fx_anim_px_from(2, h);
	const size_t y1 = flame_fx_anim_px_from(14 * 18 + 2, h);
	for(int c = 0; c < 2; c++) {
		size_t du = (size_t)(FLAME_FX_ANIM_COL1 + c * 7) * 18 + 2;
		size_t x0 = flame_fx_anim_px_from(du, w);
		size_t x1 = flame_fx_anim_px_from(du + 18, w);
		for(size_t y = y0; y < y1; y++) {
			uint8_t* row = rgba + y * w * 4;
			for(size_t x = x0; x < x1; x++) {
				// (2x+1)*128 >= du*w > shift_u*w, so this never underflows.
				size_t sx = ((2 * x + 1) * 128 - shift_u * w) / 256;
				memcpy(row + x * 4, row + sx * 4, 4);
			}
		}
	}
	return true;
}

// Each returns the INSTANCE (0 = fire, 1 = fire+16) Beta samples there.
// RenderBlocks.renderBlockFire :798-803, the branch where fire clings to
// flammable neighbours (CaveWii's is_floating): EVERY side quad uses fire+16 iff
// (x+y+z & 1) == 1 (Java precedence: the sum, then & 1; two's complement).
static inline int flame_fx_block_side_cell(int x, int y, int z) {
	return (x + y + z) & 1;
}
// :865-874, under a flammable ceiling: quad 0 fire, quad 1 fire+16.
static inline int flame_fx_block_ceiling_cell(int quad) {
	return quad ? 1 : 0;
}
// :907-944, free-standing fire's 8 planes: inner-X (1-2) fire, inner-Z (3-4)
// fire+16, outer-X (5-6) still fire+16 (no reset), outer-Z (7-8) fire again.
// z_facing = the plane's normal is Z (constant-Z quad); outer = its foot sits on
// the block edge (Beta's 0.5+-0.5 foot, 0.5+-0.4 top) rather than inside it
// (0.5+-0.2 foot, 0.5-+0.3 top).
static inline int flame_fx_block_free_cell(bool z_facing, bool outer) {
	return z_facing != outer;
}
// ItemRenderer.renderFireInFirstPerson :385: var5 = fire + var4*16.
static inline int flame_fx_overlay_cell(int quad) {
	return quad ? 1 : 0;
}
// Render.renderEntityOnFire :49-64: var23 % 2 == 0 -> fire, else fire+16.
static inline int flame_fx_entity_layer_cell(size_t layer) {
	return (int)(layer % 2);
}

// Pure per-texel colour mapping (exposed for the golden test): v = clamp(field*1.8,
// 0, 1); R=v*155+100, G=v^2*255, B=v^10*255, A = v<0.5 ? 0 : 255. out holds 4 bytes.
void flame_fx_color(float field_value, uint8_t out[4]);

// Read-only colour input for visible texel i (0..FLAME_FX_TEXELS-1).
float flame_fx_field_value(const struct flame_fx* w, int i);
float flame_fx_pair_field_value(const struct flame_fx_pair* p, int index, int i);

#endif
