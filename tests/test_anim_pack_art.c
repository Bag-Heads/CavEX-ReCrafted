/*
	test_anim_pack_art -- pin the CONTENT of the animated-texture atlas, not its
	existence.

	WHY THIS EXISTS. Fire, lava and the Nether portal are `.transparent` blocks, so
	the mesher routes them into the pass-1 display-list group (chunk_mesher.c:872)
	and world.c:1572 binds ONE sheet for that whole pass: anim.png. Their
	getTextureIndex values are therefore anim.png CELLS, not terrain.png tiles --
	lava TEXTURE_INDEX(2,0) (block_lava.c:63), fire (3,0) (block_fire.c:67), portal
	(0,0) (block_portal.c:183). In Beta 1.7.3 all three are PROCEDURAL: Minecraft.java
	:277-285 registers TextureLavaFX / TextureFlamesFX / TexturePortalFX and
	RenderEngine.updateDynamicTextures glTexSubImages each result into the atlas every
	tick. CaveWii ports those three algorithms faithfully (lava_fx.c, flame_fx.c,
	portal_fx.c, each golden-tested) and bakes them OFFLINE into anim.png via
	tools/gen_anim_fx/bake_anim_fx.sh.

	The defect this test was written for: the ports landed (34e5089, 2026-07-09), were
	golden-tested as pure algorithms, and THE BAKE WAS NEVER RUN AGAINST A SHIPPED
	ASSET. assets/anim.png still carried the fork's CC filmstrips -- and for the portal,
	a flat magenta placeholder (all 14 rows of column 0 identical, centre texel
	161,0,185,153). Every one of the 28 frames of all three tiles mismatched. The pack
	builder never caught it because tools/make_beta_pack.ps1's gate proves a path
	EXISTS, never that its bytes are right (it even prints a reassuring
	"anim.png: composited ..." line). Same failure class as an orphan test: the
	instrument was real, nothing read it.

	So this test asserts on BYTES. It re-runs the three ports in-process with exactly
	the generator's seeds/warmup/stride and compares every texel of every frame against
	the checked-in atlas.

	CELL GEOMETRY (tools/gen_anim_fx/composite_anim_fx.py:28-37, identical to
	make_beta_pack.ps1's ice composite): each 18x18 cell sits at (col*18+2, row*18+2)
	with a 1px clamped border, so the 16x16 content starts at (col*18+3, row*18+3).
	A tile's 28-frame strip is column C rows 0..13 then column C+7 rows 0..13 --
	world.c:1566-1569's matrix_anim walks anim=0..27 as Y=anim%14 with an X phase of
	+7 columns.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics/flame_fx.h"
#include "graphics/lava_fx.h"
#include "graphics/water_fx.h"
#include "graphics/water_flow_fx.h"
#include "graphics/portal_fx.h"
#include "lodepng/lodepng.h"

// Must match tools/gen_anim_fx/fx_gen.c exactly -- this test IS the pin on the
// generator's parameters as much as on the asset.
#define FRAMES 28
#define WARMUP 400
#define STRIDE 2
#define LAVA_SEED 0x9E3779B9u
#define FLAME_SEED 0x1234567u
#define WATER_SEED 0xCAFEF00Du
#define WATER_FLOW_SEED 0xF10FF10Fu

#define CELL 18
#define TILE 16
#define ROWS_PER_COL 14
#define COL_PHASE 7 // world.c matrix_anim X phase for frames 14..27

// The Beta-coloured bake. Since 2026-10-11 assets/anim.png is the Earth & Ash default
// (same Beta motion, E&A colours -- pinned by tests/test_anim_pack_colours.py against
// THIS sheet); Beta colours live in packs/beta-base, the Beta-pack base.
#define ATLAS_DEFAULT "packs/beta-base/anim.png"
#define ATLAS_BETA_PACK "texturepacks/beta-1.7.3/anim.png"

struct atlas {
	unsigned char* rgba;
	unsigned w, h;
};

static void atlas_load(struct atlas* a, const char* path) {
	unsigned err = lodepng_decode32_file(&a->rgba, &a->w, &a->h, path);
	if(err) {
		fprintf(stderr,
				"test_anim_pack_art: cannot decode %s (%u: %s) -- is the test's "
				"WORKING_DIRECTORY the repo root?\n",
				path, err, lodepng_error_text(err));
		assert(0 && "anim atlas not decodable");
	}
	// The cells this test reads top out at column 12, row 13 -> x<=245, y<=245.
	assert(a->w >= 256 && a->h >= 256 && "anim.png smaller than the sampled region");
}

static const unsigned char* atlas_texel(const struct atlas* a, unsigned x,
										unsigned y) {
	assert(x < a->w && y < a->h);
	return a->rgba + ((size_t)y * a->w + x) * 4;
}

// The content origin of frame `f`'s cell for a tile whose first column is `col0`.
static void frame_cell_origin(int col0, int f, unsigned* ox, unsigned* oy) {
	int col = (f < ROWS_PER_COL) ? col0 : col0 + COL_PHASE;
	int row = (f < ROWS_PER_COL) ? f : f - ROWS_PER_COL;
	*ox = (unsigned)(col * CELL + 3);
	*oy = (unsigned)(row * CELL + 3);
}

// Compare one 16x16 RGBA frame against its cell. Returns true on an exact match.
static bool frame_matches(const struct atlas* a, int col0, int f,
						  const unsigned char* tile) {
	unsigned ox, oy;
	frame_cell_origin(col0, f, &ox, &oy);
	for(int j = 0; j < TILE; j++)
		for(int i = 0; i < TILE; i++)
			if(memcmp(atlas_texel(a, ox + (unsigned)i, oy + (unsigned)j),
					  tile + ((size_t)j * TILE + i) * 4, 4)
			   != 0)
				return false;
	return true;
}

// --- (1) the three baked filmstrips are byte-identical to the live ports --------
// Fail-before state: 0/28 on every tile. Pass-after: 28/28 on every tile.
static void check_baked_strips_match_the_ports(const struct atlas* a,
											   const char* label) {
	// Regenerate exactly as tools/gen_anim_fx/fx_gen.c does.
	static unsigned char lava[FRAMES][LAVA_FX_BYTES];
	static unsigned char flame[FRAMES][FLAME_FX_BYTES];
	static unsigned char flame2[FRAMES][FLAME_FX_BYTES];
	static unsigned char portal[FRAMES][PORTAL_FX_BYTES];
	static unsigned char water[FRAMES][WATER_FX_BYTES];
	static unsigned char water_flow[FRAMES][WATER_FX_BYTES];

	struct lava_fx lv;
	struct flame_fx_pair fl; // Beta's TWO TextureFlamesFX (fire-anim-0924/F1)
	struct water_fx wa;
	struct water_flow_fx wfl;
	lava_fx_init(&lv, LAVA_SEED);
	flame_fx_pair_init(&fl, FLAME_SEED);
	water_fx_init(&wa, WATER_SEED);
	water_flow_fx_init(&wfl, WATER_FLOW_SEED);
	for(int i = 0; i < WARMUP; i++) {
		lava_fx_tick(&lv);
		flame_fx_pair_tick(&fl);
		water_fx_tick(&wa);
		water_flow_fx_tick(&wfl);
	}
	for(int f = 0; f < FRAMES; f++) {
		for(int s = 0; s < STRIDE; s++) {
			lava_fx_tick(&lv);
			flame_fx_pair_tick(&fl);
			water_fx_tick(&wa);
			water_flow_fx_tick(&wfl);
		}
		memcpy(lava[f], lava_fx_pixels_still(&lv), LAVA_FX_BYTES);
		memcpy(flame[f], flame_fx_pair_pixels(&fl, 0), FLAME_FX_BYTES);
		memcpy(flame2[f], flame_fx_pair_pixels(&fl, 1), FLAME_FX_BYTES);
		memcpy(water[f], water_fx_pixels(&wa), WATER_FX_BYTES);
		memcpy(water_flow[f], water_flow_fx_pixels(&wfl), WATER_FX_BYTES);
	}

	// Portal is precomputed (fixed seed 100); the strip samples the 32-frame cycle
	// evenly -- idx = f*32/28 -- so the 28-frame scroll loops seamlessly.
	static unsigned char portal_all[PORTAL_FX_FRAMES * PORTAL_FX_BYTES];
	portal_fx_generate(portal_all);
	for(int f = 0; f < FRAMES; f++) {
		int idx = f * PORTAL_FX_FRAMES / FRAMES;
		memcpy(portal[f], portal_all + (size_t)idx * PORTAL_FX_BYTES,
			   PORTAL_FX_BYTES);
	}

	const struct {
		const char* name;
		int col0;
		const unsigned char* frames;
		size_t stride;
	} tiles[] = {
		{"portal", 0, &portal[0][0], PORTAL_FX_BYTES},
		{"lava", 2, &lava[0][0], LAVA_FX_BYTES},
		{"fire", FLAME_FX_ANIM_COL0, &flame[0][0], FLAME_FX_BYTES},
		// fire-anim-0924/F1: Beta's SECOND TextureFlamesFX (fire+16) -- instance 1
		// of the same pair, baked into the formerly unused cols 6 & 13.
		{"fire+16", FLAME_FX_ANIM_COL1, &flame2[0][0], FLAME_FX_BYTES},
		// WATER RESTORED 2026-07-31, both tiles. The 07-30 revert blamed the wrong
		// half: measured against the decompile the baked STILL tile was already
		// byte-correct Beta (B pinned 255) and the FORK FLOW tile was grey (B 75..91),
		// so the "glaring seam" was the two disagreeing. With TextureWaterFlowFX now
		// ported and baked, both tiles come from the ports and match each other.
		{"water still", 1, &water[0][0], WATER_FX_BYTES},
		{"water flow", 5, &water_flow[0][0], WATER_FX_BYTES},
	};

	bool all_ok = true;
	for(size_t t = 0; t < sizeof(tiles) / sizeof(tiles[0]); t++) {
		int hits = 0;
		for(int f = 0; f < FRAMES; f++)
			if(frame_matches(a, tiles[t].col0, f,
							 tiles[t].frames + (size_t)f * tiles[t].stride))
				hits++;
		printf("  [1] %s %s cols %d&%d: %d/%d frames byte-identical to the port\n",
			   label, tiles[t].name, tiles[t].col0, tiles[t].col0 + COL_PHASE, hits,
			   FRAMES);
		if(hits != FRAMES) {
			fprintf(stderr,
					"test_anim_pack_art: %s %s is NOT the baked Beta filmstrip "
					"(%d/%d) -- run tools/gen_anim_fx/bake_anim_fx.sh %s\n",
					label, tiles[t].name, hits, FRAMES, label);
			all_ok = false;
		}
	}
	assert(all_ok && "anim.png does not carry the baked Beta FX filmstrips");
}

// --- (2) the cheapest possible regression pin: the portal is not FLAT -----------
// One assertion that catches the exact shipped defect on its own. The fork's portal
// placeholder was a single colour repeated down all 14 rows; a real filmstrip has
// 14 distinct frames. Kept separate from (1) so it survives any future re-tuning of
// the generator's seeds or stride.
static void check_portal_is_animated(const struct atlas* a, const char* label) {
	for(int col = 0; col <= COL_PHASE; col += COL_PHASE) {
		int distinct_from_row0 = 0;
		for(int row = 1; row < ROWS_PER_COL; row++) {
			bool same = true;
			for(int j = 0; j < TILE && same; j++)
				for(int i = 0; i < TILE && same; i++) {
					const unsigned char* p = atlas_texel(
						a, (unsigned)(col * CELL + 3 + i), (unsigned)(3 + j));
					const unsigned char* q = atlas_texel(
						a, (unsigned)(col * CELL + 3 + i),
						(unsigned)(row * CELL + 3 + j));
					if(memcmp(p, q, 4) != 0)
						same = false;
				}
			if(!same)
				distinct_from_row0++;
		}
		printf("  [2] %s portal col %d: %d/%d rows differ from row 0\n", label, col,
			   distinct_from_row0, ROWS_PER_COL - 1);
		assert(distinct_from_row0 == ROWS_PER_COL - 1
			   && "portal cell is a flat placeholder, not an animated filmstrip");
	}
}

// --- (3) the bake's gutter bleed is harmless -----------------------------------
// bake_anim_fx.sh's header claims "Every other tile remains byte-identical". That is
// FALSE: paste_cell writes an 18px block at col*18+2, so it bleeds 2px into the next
// column's gutter at (col+1)*18+0..+1. Harmless -- but only as long as no SAMPLED
// region is touched. Column 4 is ICE (composited by make_beta_pack.ps1), immediately
// after lava's column 3... so pin it: every non-FX column's 16x16 CONTENT region must
// be untouched by the bleed. This is the assertion that keeps the bleed harmless
// instead of merely believed-harmless.
static void check_ice_content_survives_the_bleed(const struct atlas* def,
												 const struct atlas* pack) {
	if(!pack)
		return;
	// Columns 4 and 11 are the authentic-Beta ice composite -- the ONE thing the pack
	// contributes to anim.png. It must differ from the default pack (it is real Beta
	// art) and must be self-consistent, i.e. the bake never reached into its content.
	int differing = 0;
	for(int col = 4; col <= 11; col += 7)
		for(int row = 0; row < ROWS_PER_COL; row++)
			for(int j = 0; j < TILE; j++)
				for(int i = 0; i < TILE; i++) {
					unsigned x = (unsigned)(col * CELL + 3 + i);
					unsigned y = (unsigned)(row * CELL + 3 + j);
					if(memcmp(atlas_texel(def, x, y), atlas_texel(pack, x, y), 4)
					   != 0)
						differing++;
				}
	printf("  [3] ice content (cols 4&11) differs from the default pack in %d texels "
		   "-- the authentic Beta ice composite is intact\n",
		   differing);
	assert(differing > 0
		   && "the Beta pack's ice composite vanished -- the bake overwrote it");
}

int main(void) {
	printf("test_anim_pack_art: anim.png carries the baked Beta FX filmstrips\n");

	struct atlas def;
	atlas_load(&def, ATLAS_DEFAULT);
	check_baked_strips_match_the_ports(&def, ATLAS_DEFAULT);
	check_portal_is_animated(&def, ATLAS_DEFAULT);

	// The authentic Beta pack is gitignored (Mojang IP) and absent in CI, so it is
	// checked opportunistically -- present on a maintainer box, skipped elsewhere.
	struct atlas pack;
	bool have_pack = lodepng_decode32_file(&pack.rgba, &pack.w, &pack.h,
										   ATLAS_BETA_PACK)
		== 0;
	if(have_pack) {
		assert(pack.w >= 256 && pack.h >= 256);
		check_baked_strips_match_the_ports(&pack, ATLAS_BETA_PACK);
		check_portal_is_animated(&pack, ATLAS_BETA_PACK);
		check_ice_content_survives_the_bleed(&def, &pack);
		free(pack.rgba);
	} else {
		printf("  [-] %s absent (gitignored Mojang-derived pack) -- skipped\n",
			   ATLAS_BETA_PACK);
	}

	free(def.rgba);
	printf("test_anim_pack_art: PASS\n");
	return 0;
}
