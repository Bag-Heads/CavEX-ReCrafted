/*
	Golden test for the Beta 1.7.3 TexturePortalFX port
	(source/graphics/portal_fx.c). Unlike lava/flame, the portal frames are
	PRECOMPUTED from a FIXED seed (Beta: new Random(100L)), so the whole
	32-frame strip is deterministic -- pinned here by an FNV-1a self-golden
	hash plus the load-bearing invariants:
	  (A) the exact per-texel colour mapping, INCLUDING the authentic byte
	      WRAP at intensity > 1 (Beta casts (int) values up to ~297 into a
	      byte, so the bright spiral core carries wrapped sparkle texels --
	      do not "fix" this);
	  (B) determinism: two generations are byte-identical;
	  (C) animation: distinct frames differ, and the spiral actually rotates
	      (frame 0 vs frame 16 = half a revolution);
	  (D) the purple reading: for in-range texels B >= 155 dominates and the
	      alpha is translucent-to-opaque (>= 155), never 0 at intensity <= 1.
*/

#include "../source/graphics/portal_fx.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check_color(float in, int r, int g, int b, int a) {
	uint8_t o[4];
	portal_fx_color(in, o);
	assert(o[0] == r && o[1] == g && o[2] == b && o[3] == a);
}

static uint32_t fnv1a(const uint8_t* p, size_t n) {
	uint32_t h = 2166136261u;
	for(size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

static uint8_t frames_a[PORTAL_FX_FRAMES * PORTAL_FX_BYTES];
static uint8_t frames_b[PORTAL_FX_FRAMES * PORTAL_FX_BYTES];

int main(void) {
	// (A) colour formula: i=0 and i=1 endpoints, a mid point, and the
	// authentic >1 wrap (i=1.05: R=(int)(275.5)=275 -> 19; B=A=(int)260 -> 4;
	// G=(int)(1.05^4*255)=(int)309.9=309 -> 53).
	check_color(0.0f, 55, 0, 155, 155);
	check_color(1.0f, 255, 255, 255, 255);
	check_color(0.5f, 105, 15, 205, 205);
	check_color(1.05f, 19, 53, 4, 4);

	// (B) fixed-seed determinism, whole strip.
	portal_fx_generate(frames_a);
	portal_fx_generate(frames_b);
	assert(memcmp(frames_a, frames_b, sizeof(frames_a)) == 0);

	// (C) animation: consecutive + opposite frames differ.
	assert(memcmp(frames_a, frames_a + PORTAL_FX_BYTES, PORTAL_FX_BYTES) != 0);
	assert(memcmp(frames_a, frames_a + 16 * PORTAL_FX_BYTES, PORTAL_FX_BYTES)
		   != 0);

	// (D) purple + translucency: every texel of frame 0 keeps B and A in the
	// 155..255 band OR is an authentic wrap texel (intensity > 1 -> small
	// values); no texel is fully transparent-black garbage.
	int wraps = 0;
	for(int t = 0; t < PORTAL_FX_TEXELS; t++) {
		uint8_t* px = frames_a + (size_t)t * 4;
		if(px[3] >= 155) {
			assert(px[2] >= 155); // blue floor -> reads purple
		} else {
			wraps++; // the bright-core wrap band
			assert(px[3] < 60); // wraps land small, never mid-band
		}
	}
	// The wrap band exists but is a small minority of the tile.
	assert(wraps < PORTAL_FX_TEXELS / 4);
	printf("  portal_fx: colour map + wrap, determinism, rotation, purple "
		   "band OK (frame0 wraps=%d)\n",
		   wraps);

	// Self-golden strip hash: pins every byte of all 32 frames. Regenerate
	// deliberately (and update here) only if the algorithm itself changes.
	uint32_t h = fnv1a(frames_a, sizeof(frames_a));
	printf("  portal_fx: strip fnv1a=0x%08x\n", h);
	const uint32_t PINNED = 0x0a81a8fbu; // pinned 2026-07-08 (seed-100 strip)
	if(PINNED != 0)
		assert(h == PINNED);

	printf("test_portal_fx: ALL OK\n");
	return 0;
}
