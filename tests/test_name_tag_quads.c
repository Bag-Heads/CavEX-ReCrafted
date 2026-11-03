/*
	Golden test for render_name_tag.c's PURE quad builder -- Beta 1.7.3
	remote-player name tags [740c583 RenderPlayer.renderName + RenderLiving.
	renderLivingLabel]. Pure math only: no GL/GX context, no loaded
	texture_font -- this test supplies its own synthetic glyph-width table
	(init_widths below), mirroring tests/test_sign_text_quads.c's own rig, so
	it links render_name_tag.c ALONE.

	Six properties, mirroring the sign-text golden's shape:

	  1. DISTANCE CULL -- beyond NAME_TAG_RENDER_DISTANCE (64 blocks) the
	     builder returns 0 (no quads at all, bg_out untouched); just inside
	     it returns real quads.
	  2. EMPTY-NAME GUARD -- an empty username returns 0.
	  3. TEXT CENTERING -- a single glyph whose (width+1) exactly equals the
	     8-unit glyph cell size sits symmetric about the anchor's local X==0
	     column when the camera looks with zero yaw/level pitch (identity
	     billboard).
	  4. BACKGROUND SPAN -- the background quad's local-space X/Y span
	     matches [-half-1,-1]..[half+1,8] exactly (recovered by inverting the
	     same identity-billboard transform test 3 uses).
	  5. BILLBOARD YAW -- rotating the camera's rx by 90 degrees rotates the
	     label's plane by the SAME 90 degrees about Y (probing two corners
	     that are NOT on the rotation axis).
	  6. GLYPH UV -- two known characters land on the exact 16x16-cell atlas
	     coordinates gui_util.c's gutil_text_rgb (and render_sign_text.c's
	     own golden) already use.

	expected_world() is a DELIBERATE independent re-derivation of
	render_name_tag.c's transform chain using plain scalar trig (not cglm,
	not any production code) -- see its own comment for the chain it mirrors.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "graphics/render_name_tag.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Synthetic glyph-width table -- NOT read from any real texture. Lives in
// the same 8-unit "ink width, no +1" space render_sign_text.h/render_name_tag.h
// document for glyph_width[].
static uint8_t g_width[256];

static void init_widths(void) {
	memset(g_width, 6, sizeof(g_width)); // arbitrary default ink width
	g_width[(unsigned char) ' '] = 4;	  // matches gui_util's real space width
	// 'X': width+1 == 8 == the fixed glyph cell size (GLYPH_CELL_PX), so a
	// single-char "X" name centers with pen_x == -4 exactly (total_width=8,
	// -8/2=-4, no truncation ambiguity) -- the probe glyph for tests 3/4/5,
	// same trick tests/test_sign_text_quads.c uses.
	g_width[(unsigned char) 'X'] = 7;
}

static float deg2rad(float d) {
	return d * (float) M_PI / 180.0f;
}

// Independent re-derivation of render_name_tag.c's build_model_matrix chain
// for a single label-local point, in plain scalar trig -- mirrors, in order:
//   r = anchor + Ry(camera_rx) * Rx(camera_ry - PI/2) * S(local)
// where S negates x/y and keeps z (NAME_TAG_SCALE). See render_name_tag.c's
// build_model_matrix / render_name_tag.h's header comment for the citation.
static void expected_world(float lx, float ly, float lz, float anchor_x,
						   float anchor_y, float anchor_z, float camera_rx,
						   float camera_ry, float out[3]) {
	float s = NAME_TAG_SCALE;
	float sx = -s * lx;
	float sy = -s * ly;
	float sz = s * lz;

	// Rx(pitch) applied first (innermost -- matches glm_rotate_x being the
	// SECOND call in build_model_matrix, which post-multiplies, so it acts
	// on the vertex before Ry).
	float pitch = camera_ry - (float) M_PI / 2.0f;
	float cp = cosf(pitch), sp = sinf(pitch);
	float rx0 = sx;
	float ry0 = sy * cp - sz * sp;
	float rz0 = sy * sp + sz * cp;

	// Ry(yaw) applied second (outermost of the two rotations).
	float cy = cosf(camera_rx), sy_ = sinf(camera_rx);
	float px = rx0 * cy + rz0 * sy_;
	float py = ry0;
	float pz = -rx0 * sy_ + rz0 * cy;

	out[0] = anchor_x + px;
	out[1] = anchor_y + py;
	out[2] = anchor_z + pz;
}

static void test_distance_cull(void) {
	vec3 entity_pos = {0.0f, 64.0f, 0.0f};
	vec3 camera_near = {0.0f, 64.0f, 30.0f}; // 30 blocks: inside the 64 cap
	vec3 camera_far = {0.0f, 64.0f, 65.0f};  // 65 blocks: outside the cap

	struct name_tag_vertex text[NAME_TAG_MAX_TEXT_VERTS];
	struct name_tag_vertex bg[NAME_TAG_BG_VERTS];

	size_t n_near = name_tag_build_quads("X", entity_pos, camera_near, 0.0f,
										 (float) M_PI / 2.0f, g_width, false, text,
										 bg);
	assert(n_near == 4);

	size_t n_far = name_tag_build_quads("X", entity_pos, camera_far, 0.0f,
										(float) M_PI / 2.0f, g_width, false, text,
										bg);
	assert(n_far == 0);
}

static void test_empty_name_guard(void) {
	vec3 entity_pos = {0.0f, 64.0f, 0.0f};
	vec3 camera_pos = {0.0f, 64.0f, 5.0f};

	struct name_tag_vertex text[NAME_TAG_MAX_TEXT_VERTS];
	struct name_tag_vertex bg[NAME_TAG_BG_VERTS];

	size_t n = name_tag_build_quads("", entity_pos, camera_pos, 0.0f,
									(float) M_PI / 2.0f, g_width, false, text, bg);
	assert(n == 0);
}

static void test_text_centering_symmetric(void) {
	vec3 entity_pos = {10.0f, 64.0f, 20.0f};
	vec3 camera_pos = {10.0f, 64.0f, 25.0f}; // 5 blocks away, in range

	struct name_tag_vertex text[NAME_TAG_MAX_TEXT_VERTS];
	struct name_tag_vertex bg[NAME_TAG_BG_VERTS];

	// camera_rx=0, camera_ry=PI/2 (level) -> playerViewY=0, playerViewX=0 ->
	// identity billboard rotation, so world == anchor + S(local) exactly.
	size_t n = name_tag_build_quads("X", entity_pos, camera_pos, 0.0f,
									(float) M_PI / 2.0f, g_width, false, text, bg);
	assert(n == 4);

	float anchor_y = entity_pos[1] + NAME_TAG_EYE_OFFSET;

	// Quad corner order: top-left, top-right, bottom-right, bottom-left
	// (render_sign_text.c's convention, mirrored). local x: pen_x==-4
	// (left), pen_x+8==+4 (right); S negates x, so world x DECREASES as
	// local x increases.
	float left_x = text[0].x;  // local x=-4 -> world x = anchor - s*(-4)
	float right_x = text[1].x; // local x=+4 -> world x = anchor - s*(+4)

	assert(fabsf((left_x - entity_pos[0]) + (right_x - entity_pos[0]))
		   < 1e-4f);
	assert(fabsf(left_x - (entity_pos[0] + 4.0f * NAME_TAG_SCALE)) < 1e-4f);
	assert(fabsf(right_x - (entity_pos[0] - 4.0f * NAME_TAG_SCALE)) < 1e-4f);
	assert(fabsf(text[0].y - anchor_y) < 1e-4f); // local y=0 -> S(y)=0
}

static void test_background_span(void) {
	vec3 entity_pos = {0.0f, 64.0f, 0.0f};
	vec3 camera_pos = {0.0f, 64.0f, 5.0f};

	struct name_tag_vertex text[NAME_TAG_MAX_TEXT_VERTS];
	struct name_tag_vertex bg[NAME_TAG_BG_VERTS];

	// "XX": total_width = 2*(7+1) = 16, half = 8.
	size_t n = name_tag_build_quads("XX", entity_pos, camera_pos, 0.0f,
									(float) M_PI / 2.0f, g_width, false, text, bg);
	assert(n == 8);

	float anchor_x = entity_pos[0], anchor_y = entity_pos[1]
		+ NAME_TAG_EYE_OFFSET, anchor_z = entity_pos[2];

	float expect[3];
	// Background local (-half-1,-1) == (-9,-1) at identity billboard.
	expected_world(-9.0f, -1.0f, 0.0f, anchor_x, anchor_y, anchor_z, 0.0f,
				  (float) M_PI / 2.0f, expect);
	assert(fabsf(bg[0].x - expect[0]) < 1e-3f);
	assert(fabsf(bg[0].y - expect[1]) < 1e-3f);
	assert(fabsf(bg[0].z - expect[2]) < 1e-3f);

	// Background local (half+1,8) == (9,8) -- the opposite (bottom-right)
	// corner, bg[2] per the TL/TR/BR/BL winding.
	expected_world(9.0f, 8.0f, 0.0f, anchor_x, anchor_y, anchor_z, 0.0f,
				  (float) M_PI / 2.0f, expect);
	assert(fabsf(bg[2].x - expect[0]) < 1e-3f);
	assert(fabsf(bg[2].y - expect[1]) < 1e-3f);
	assert(fabsf(bg[2].z - expect[2]) < 1e-3f);
}

static void test_billboard_yaw(void) {
	vec3 entity_pos = {0.0f, 64.0f, 0.0f};
	vec3 camera_pos = {0.0f, 64.0f, 5.0f};

	static const float yaws_deg[3] = {0.0f, 90.0f, -45.0f};

	for(int i = 0; i < 3; i++) {
		float camera_rx = deg2rad(yaws_deg[i]);

		struct name_tag_vertex text[NAME_TAG_MAX_TEXT_VERTS];
		struct name_tag_vertex bg[NAME_TAG_BG_VERTS];
		size_t n = name_tag_build_quads("X", entity_pos, camera_pos,
										camera_rx, (float) M_PI / 2.0f,
										g_width, false, text, bg);
		assert(n == 4);

		float anchor_y = entity_pos[1] + NAME_TAG_EYE_OFFSET;

		// Probe: top-left corner, local (pen_x=-4, pen_y=0, z=0).
		float expect[3];
		expected_world(-4.0f, 0.0f, 0.0f, entity_pos[0], anchor_y,
					  entity_pos[2], camera_rx, (float) M_PI / 2.0f, expect);

		assert(fabsf(text[0].x - expect[0]) < 1e-3f);
		assert(fabsf(text[0].y - expect[1]) < 1e-3f);
		assert(fabsf(text[0].z - expect[2]) < 1e-3f);
	}
}

static void test_glyph_uv(void) {
	vec3 entity_pos = {0.0f, 64.0f, 0.0f};
	vec3 camera_pos = {0.0f, 64.0f, 5.0f};

	struct name_tag_vertex text[NAME_TAG_MAX_TEXT_VERTS];
	struct name_tag_vertex bg[NAME_TAG_BG_VERTS];

	// 'A' (0x41==65 -> atlas cell (1,4)), '0' (0x30==48 -> atlas cell (0,3)).
	size_t n = name_tag_build_quads("A0", entity_pos, camera_pos, 0.0f,
									(float) M_PI / 2.0f, g_width, false, text, bg);
	assert(n == 8); // 2 glyphs * 4 verts

	// 'A' quad (text[0..3]): u in [16/256, 32/256], v in [64/256, 80/256] --
	// same cell gui_util.c's gutil_text_rgb / render_sign_text.c's own
	// golden compute via `tex_x = ch%16*16; tex_y = ch/16*16;`.
	assert(fabsf(text[0].u - 16.0f / 256.0f) < 1e-6f);
	assert(fabsf(text[0].v - 64.0f / 256.0f) < 1e-6f);
	assert(fabsf(text[1].u - 32.0f / 256.0f) < 1e-6f);
	assert(fabsf(text[2].v - 80.0f / 256.0f) < 1e-6f);

	// '0' quad (text[4..7]): tex_x=0, tex_y=48 -> u in [0,16/256], v in
	// [48/256, 64/256].
	assert(fabsf(text[4].u - 0.0f) < 1e-6f);
	assert(fabsf(text[4].v - 48.0f / 256.0f) < 1e-6f);
	assert(fabsf(text[5].u - 16.0f / 256.0f) < 1e-6f);
	assert(fabsf(text[6].v - 64.0f / 256.0f) < 1e-6f);
}

// MP visual-parity pass (2026-08-31) item 2: is_sneaking halves the render
// distance cap to NAME_TAG_RENDER_DISTANCE_SNEAK (32 blocks) [740c583
// RenderPlayer.renderName]. Probe a distance strictly between the two caps
// (40 blocks): a standing player is still drawn, a sneaking one is not.
static void test_sneak_distance_cap(void) {
	vec3 entity_pos = {0.0f, 64.0f, 0.0f};
	vec3 camera_pos = {0.0f, 64.0f, 40.0f}; // 40 blocks: inside 64, outside 32

	struct name_tag_vertex text[NAME_TAG_MAX_TEXT_VERTS];
	struct name_tag_vertex bg[NAME_TAG_BG_VERTS];

	size_t n_standing = name_tag_build_quads("X", entity_pos, camera_pos, 0.0f,
											 (float) M_PI / 2.0f, g_width,
											 false, text, bg);
	assert(n_standing == 4);

	size_t n_sneaking = name_tag_build_quads("X", entity_pos, camera_pos, 0.0f,
											 (float) M_PI / 2.0f, g_width,
											 true, text, bg);
	assert(n_sneaking == 0);

	// Just inside the 32-block sneak cap: drawn either way.
	vec3 camera_close = {0.0f, 64.0f, 20.0f};
	size_t n_sneaking_close = name_tag_build_quads(
		"X", entity_pos, camera_close, 0.0f, (float) M_PI / 2.0f, g_width,
		true, text, bg);
	assert(n_sneaking_close == 4);
}

// ci-1101/30: Beta colour codes in a player name. [mcb173-retromcp
// RenderLiving.java:197-208] the label is getStringWidth-centred and drawn
// twice through FontRenderer.drawString: 553648127 (0x20FFFFFF) with depth
// test off, then -1 (opaque white) depth-tested; RenderPlayer.java:92 draws
// a sneaking name once at 0x20FFFFFF. [FontRenderer.java:142,147-161] the
// base colour is glColor4f(r,g,b,a) but a code calls the colour list, a
// glColor3f -- which sets alpha to 1.0 -- so text after a code is the code's
// colour at FULL alpha in every pass (the dim through-walls pass included).
static void test_colour_codes(void) {
	vec3 entity_pos = {10.0f, 64.0f, 20.0f};
	vec3 camera_pos = {10.0f, 64.0f, 25.0f};
	struct name_tag_vertex text[NAME_TAG_MAX_TEXT_VERTS];
	struct name_tag_vertex bg[NAME_TAG_BG_VERTS];

	// "X\247cX": glyphs X, X only; width 16 (codes 0) -> pen_x -8.
	size_t n = name_tag_build_quads("X\247cX", entity_pos, camera_pos, 0.0f,
									(float) M_PI / 2.0f, g_width, false, text, bg);
	assert(n == 8);
	assert(fabsf(text[0].x - (entity_pos[0] + 8.0f * NAME_TAG_SCALE)) < 1e-4f);
	assert(fabsf(text[4].x - entity_pos[0]) < 1e-4f); // 2nd X at local 0
	assert(fabsf(bg[0].x - (entity_pos[0] + 9.0f * NAME_TAG_SCALE)) < 1e-4f);

	uint8_t c[4];
	// Uncoded glyph: the pass's own colour (white at the pass alpha).
	name_tag_text_color(&text[0], 32, c);
	assert(c[0] == 255 && c[1] == 255 && c[2] == 255 && c[3] == 32);
	name_tag_text_color(&text[0], 255, c);
	assert(c[0] == 255 && c[1] == 255 && c[2] == 255 && c[3] == 255);
	// Coded glyph: colour 12 (255,85,85) at alpha 255 in BOTH passes.
	for(int k = 4; k < 8; k++) {
		name_tag_text_color(&text[k], 32, c);
		assert(c[0] == 255 && c[1] == 85 && c[2] == 85 && c[3] == 255);
		name_tag_text_color(&text[k], 255, c);
		assert(c[0] == 255 && c[1] == 85 && c[2] == 85 && c[3] == 255);
	}

	// A 16-char name whose codes push the byte count past 16 keeps every
	// visible char: "\2476" + 14 X = 16 bytes -> 14 glyphs.
	n = name_tag_build_quads("\2476XXXXXXXXXXXXXX", entity_pos, camera_pos,
							 0.0f, (float) M_PI / 2.0f, g_width, false, text, bg);
	assert(n == 14 * 4);
	name_tag_text_color(&text[0], 32, c);
	assert(c[0] == 255 && c[1] == 170 && c[2] == 0 && c[3] == 255);
}

int main(void) {
	init_widths();

	test_distance_cull();
	test_empty_name_guard();
	test_text_centering_symmetric();
	test_background_span();
	test_billboard_yaw();
	test_glyph_uv();
	test_sneak_distance_cap();
	test_colour_codes();

	printf("test_name_tag_quads: all assertions passed\n");
	return 0;
}
