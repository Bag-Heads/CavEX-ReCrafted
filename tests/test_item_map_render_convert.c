/*
	Golden test for item_map_colors_to_rgba, the pure display-conversion half
	of the Beta 1.7.3 held-map render slice (part 2b) -- item_map.c's
	item_map_color_index_for_block / item_map_update_data (part 2a) build a
	map's colour BYTES; this function turns those bytes into RGBA8888 pixels
	for on-screen display. Transcribed 1:1 from MapItemRenderer.renderMap's
	intArray[] fill loop [decompile 740c583 MapItemRenderer.renderMap]; see
	source/item/items/item_map_render_convert.c for the full citation and
	source/item/items/item_map.c:291-306 for the item_map_color_rgb[] palette
	table this test's hand-computed values are read from.

	What's pinned here, hand-computed against the decompile (not derived from
	the implementation under test):

	  (a) The UNEXPLORED checkerboard: for c/4==0, alpha = ((i + i/128) & 1)
	      * 8 + 16, RGB always 0,0,0. Checked at indices 0 and 129 (as
	      specified), plus index 1 to show the alternate (24) value the
	      checkerboard also produces.

	  (b) The explored-pixel palette lookup + per-band brightness multiply
	      (mul = 180 band0 / 220 band1 / 255 band2 / 220 band3 -- band 1 AND
	      3 share the SAME 220 default, verified against the live decompile),
	      for four different palette indices spanning all four bands each:
	      grass(1), sand(2), stone(11), water(12). Every expected byte below
	      is computed by hand from item_map_color_rgb[]'s literal RGB (shown
	      in comments as hex + decimal channels) times the multiplier,
	      integer-divided by 255 -- exactly MapItemRenderer.renderMap's
	      `(channel * mul) / 255` -- and re-derived independently in this
	      comment block, not copied from the .c file under test.

	  (c) A full 16384-pixel sweep property: every EXPLORED pixel (c/4 != 0)
	      has alpha == 255; every UNEXPLORED pixel (c/4 == 0) has r=g=b=0.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "item/items/item_map.h" // item_map_colors_to_rgba, item_map_color_rgb_for_index
#include "network/map_data.h"	 // MAP_DATA_DIM / MAP_DATA_COLORS_SIZE

// ---------------------------------------------------------------------------
// (a) Unexplored checkerboard alphas.
//
//   index 0:   i=0,   i/128=0   -> (0+0)   & 1 = 0 -> alpha = 0*8+16 = 16
//   index 1:   i=1,   i/128=0   -> (1+0)   & 1 = 1 -> alpha = 1*8+16 = 24
//   index 129: i=129, i/128=1   -> (129+1) & 1 = (130 & 1) = 0 -> alpha = 16
//              (130 = 0b10000010, low bit 0 -- even, so this index lands on
//              the SAME 16 as index 0, not the alternate 24; hand-verified,
//              not assumed just because it's a different index)
// ---------------------------------------------------------------------------
static void test_checkerboard_alpha(void) {
	uint8_t colors[MAP_DATA_COLORS_SIZE];
	memset(colors, 0, sizeof(colors)); // every byte < 4 -> every pixel unexplored
	uint8_t rgba[MAP_DATA_COLORS_SIZE * 4];

	item_map_colors_to_rgba(colors, rgba);

	// index 0 -> alpha 16, RGB black.
	assert(rgba[0 * 4 + 0] == 0);
	assert(rgba[0 * 4 + 1] == 0);
	assert(rgba[0 * 4 + 2] == 0);
	assert(rgba[0 * 4 + 3] == 16);

	// index 1 -> alpha 24 (the alternate checkerboard value), RGB black.
	assert(rgba[1 * 4 + 0] == 0);
	assert(rgba[1 * 4 + 1] == 0);
	assert(rgba[1 * 4 + 2] == 0);
	assert(rgba[1 * 4 + 3] == 24);

	// index 129 -> alpha 16 (same parity as index 0), RGB black.
	assert(rgba[129 * 4 + 0] == 0);
	assert(rgba[129 * 4 + 1] == 0);
	assert(rgba[129 * 4 + 2] == 0);
	assert(rgba[129 * 4 + 3] == 16);
}

// ---------------------------------------------------------------------------
// (b) Explored-pixel palette + band multiply, hand-computed.
//
// A colour byte is `color_index * 4 + band` (item_map.c's own encoding, the
// only shape item_map_update_data ever writes). item_map_color_rgb[] literal
// RGBs (item_map.c:291-306, decimal colorValue -> hex -> per-channel decimal,
// re-derived here independently):
//
//   grass (index 1)  = 8368696  = 0x7FB238 -> r=127 g=178 b=56
//   sand  (index 2)  = 16247203 = 0xF7E9A3 -> r=247 g=233 b=163
//   stone (index 11) = 7368816  = 0x707070 -> r=112 g=112 b=112
//   water (index 12) = 4210943  = 0x4040FF -> r=64  g=64  b=255
//
// Multiplier per band [decompile]: band0=180, band1=220, band2=255, band3=220
// (1 and 3 share the SAME default -- this is the point of testing all four).
//
// channel_out = (channel_in * mul) / 255, C integer division (truncates
// toward zero; all operands here are non-negative, so this is floor
// division) -- hand-multiplied below, not taken from the implementation:
//
//   grass  band0 (mul180): r=127*180/255=89  (22860/255=89.64->89)
//                            g=178*180/255=125 (32040/255=125.6->125)
//                            b=56*180/255=39   (10080/255=39.5->39)
//          band1 (mul220): r=127*220/255=109 (27940/255=109.6->109)
//                            g=178*220/255=153 (39160/255=153.5->153)
//                            b=56*220/255=48   (12320/255=48.3->48)
//          band2 (mul255): r=127 g=178 b=56 (mul==255 -> identity)
//          band3 (mul220): SAME as band1 -> r=109 g=153 b=48
//
//   sand   band0 (mul180): r=247*180/255=174 (44460/255=174.35->174)
//                            g=233*180/255=164 (41940/255=164.47->164)
//                            b=163*180/255=115 (29340/255=115.06->115)
//          band1 (mul220): r=247*220/255=213 (54340/255=213.1->213)
//                            g=233*220/255=201 (51260/255=201.02->201)
//                            b=163*220/255=140 (35860/255=140.6->140)
//          band2 (mul255): r=247 g=233 b=163 (identity)
//          band3 (mul220): SAME as band1 -> r=213 g=201 b=140
//
//   stone  band0 (mul180): r=g=b=112*180/255=79 (20160/255=79.06->79)
//          band1 (mul220): r=g=b=112*220/255=96 (24640/255=96.6->96)
//          band2 (mul255): r=g=b=112 (identity)
//          band3 (mul220): SAME as band1 -> r=g=b=96
//
//   water  band0 (mul180): r=64*180/255=45  (11520/255=45.18->45)
//                            g=45 (same as r, g==r for water)
//                            b=255*180/255=180 (mul==255 base component, so
//                              255*180/255=180 exactly)
//          band1 (mul220): r=g=64*220/255=55 (14080/255=55.2->55)
//                            b=255*220/255=220 (exact)
//          band2 (mul255): r=g=64 b=255 (identity)
//          band3 (mul220): SAME as band1 -> r=g=55 b=220
// ---------------------------------------------------------------------------

struct band_case {
	const char* name;
	uint8_t palette_index;
	uint8_t r[4], g[4], b[4]; // indexed by band 0..3
};

static void test_palette_band_multiply(void) {
	static const struct band_case cases[] = {
		{"grass", 1, {89, 109, 127, 109}, {125, 153, 178, 153}, {39, 48, 56, 48}},
		{"sand", 2, {174, 213, 247, 213}, {164, 201, 233, 201}, {115, 140, 163, 140}},
		{"stone", 11, {79, 96, 112, 96}, {79, 96, 112, 96}, {79, 96, 112, 96}},
		{"water", 12, {45, 55, 64, 55}, {45, 55, 64, 55}, {180, 220, 255, 220}},
	};

	uint8_t colors[MAP_DATA_COLORS_SIZE];
	memset(colors, 0, sizeof(colors));

	// Place all 16 (palette_index, band) combinations at distinct pixel
	// indices so a single conversion call exercises every hand-computed case.
	int px = 0;
	int case_px[4][4];
	for(int ci = 0; ci < 4; ci++) {
		for(int band = 0; band < 4; band++) {
			case_px[ci][band] = px;
			colors[px] = (uint8_t)(cases[ci].palette_index * 4 + band);
			px++;
		}
	}

	uint8_t rgba[MAP_DATA_COLORS_SIZE * 4];
	item_map_colors_to_rgba(colors, rgba);

	for(int ci = 0; ci < 4; ci++) {
		for(int band = 0; band < 4; band++) {
			int i = case_px[ci][band];
			assert(rgba[i * 4 + 0] == cases[ci].r[band]);
			assert(rgba[i * 4 + 1] == cases[ci].g[band]);
			assert(rgba[i * 4 + 2] == cases[ci].b[band]);
			// Every explored pixel is fully opaque regardless of band.
			assert(rgba[i * 4 + 3] == 255);
		}
	}

	// Cross-check band0/band2 against a mul==255 identity and the palette
	// table's own literal RGB directly, as an independent sanity check that
	// the hand-derived hex hasn't been mis-transcribed above.
	uint32_t grass_rgb = item_map_color_rgb_for_index(1);
	assert(grass_rgb == 8368696u);
	assert(((grass_rgb >> 16) & 0xFF) == 127);
	assert(((grass_rgb >> 8) & 0xFF) == 178);
	assert((grass_rgb & 0xFF) == 56);
	uint32_t water_rgb = item_map_color_rgb_for_index(12);
	assert(water_rgb == 4210943u);
	assert(((water_rgb >> 16) & 0xFF) == 64);
	assert(((water_rgb >> 8) & 0xFF) == 64);
	assert((water_rgb & 0xFF) == 255);
}

// ---------------------------------------------------------------------------
// (c) Full-array sweep property: over EVERY pixel of a fully-populated
//     colour buffer (all 14 palette indices x all 4 bands, cycling), every
//     explored pixel must read alpha==255 and every unexplored pixel must
//     read r=g=b=0. This is a property check (not a specific value pin) that
//     exercises the entire 16384-pixel domain, including the edge_band /
//     row-wrap arithmetic at i/128 boundaries the hand-picked cases above
//     don't reach.
// ---------------------------------------------------------------------------
static void test_full_array_sweep_property(void) {
	uint8_t colors[MAP_DATA_COLORS_SIZE];
	for(int i = 0; i < MAP_DATA_COLORS_SIZE; i++) {
		// Cycle color_index 0..13 and band 0..3 independently so both
		// unexplored (color_index==0, any band) and every explored
		// (color_index, band) combination appear many times across the
		// sweep.
		uint8_t color_index = (uint8_t)(i % 14);
		uint8_t band = (uint8_t)((i / 14) % 4);
		colors[i] = (uint8_t)(color_index * 4 + band);
	}

	uint8_t rgba[MAP_DATA_COLORS_SIZE * 4];
	item_map_colors_to_rgba(colors, rgba);

	int unexplored_seen = 0, explored_seen = 0;
	for(int i = 0; i < MAP_DATA_COLORS_SIZE; i++) {
		uint8_t r = rgba[i * 4 + 0];
		uint8_t g = rgba[i * 4 + 1];
		uint8_t b = rgba[i * 4 + 2];
		uint8_t a = rgba[i * 4 + 3];

		if(colors[i] / 4 == 0) {
			assert(r == 0 && g == 0 && b == 0);
			// Alpha must be exactly 16 or 24 (the only two checkerboard
			// values), never anything else.
			assert(a == 16 || a == 24);
			unexplored_seen++;
		} else {
			assert(a == 255);
			explored_seen++;
		}
	}
	// Sanity: the sweep pattern actually produced both categories in bulk --
	// an implementation that silently treated everything as one branch would
	// still pass a property check with zero counterexamples on one side.
	assert(unexplored_seen > 0);
	assert(explored_seen > 0);
	assert(unexplored_seen + explored_seen == MAP_DATA_COLORS_SIZE);
}

int main(void) {
	test_checkerboard_alpha();
	test_palette_band_multiply();
	test_full_array_sweep_property();

	printf("test_item_map_render_convert: OK (MapItemRenderer.renderMap "
		   "colour->RGBA8888 conversion matches hand-computed goldens: "
		   "checkerboard alpha, 4 palette indices x 4 bands, full-array "
		   "sweep property)\n");
	return 0;
}
