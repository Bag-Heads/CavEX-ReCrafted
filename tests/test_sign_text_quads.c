/*
	Golden test for render_sign_text.c's PURE quad builder -- Beta 1.7.3
	in-world sign text [740c583 TileEntitySignRenderer.renderTileEntitySignAt].
	Pure math only: no GL/GX context, no loaded texture_font -- this test
	supplies its own synthetic glyph-width table (init_widths below) instead
	of gutil_font_glyph_widths(), so it links render_sign_text.c ALONE.

	Six properties, matching the porting task's brief:

	  1. QUAD COUNT excludes spaces -- a space glyph still advances the pen
	     (like Beta) but never emits a quad (it is definitionally ink-free).
	  2. LINE CENTERING -- a single glyph whose (width+1) exactly equals the
	     8-unit glyph cell size sits symmetric about the block's centre
	     column (world x == 0.5 at local pen x == 0).
	  3. ROW SPACING -- consecutive lines are exactly Beta's `line*10 -
	     signText.length*5` apart in font-pixel space, i.e. `text_scale*10`
	     apart in world Y.
	  4. POST rotation -- metas 0/4/8/12 (0/90/180/270 degrees) rotate a
	     probe glyph exactly like `glRotatef(-meta*22.5, 0,1,0)`.
	  5. WALL rotation -- Beta metas 2/3/4/5 (FRONT/BACK/LEFT/RIGHT)
	     rotate the SAME probe glyph per the capture-confirmed table in
	     render_sign_text.c's header comment, AND the wall-only extra
	     translate(0,-0.3125,-0.4375) is folded in before the rotation.
	  6. GLYPH UV -- two known characters land on the exact 16x16-cell atlas
	     coordinates gui_util.c's gutil_text_rgb already uses.

	expected_world() below is a DELIBERATE independent re-derivation of
	render_sign_text.c's transform chain using plain scalar trig (not cglm,
	not any production code) -- see its own comment for the chain it mirrors
	-- so properties 2/4/5 are a real cross-check, not a tautology.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "graphics/render_sign_text.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Synthetic glyph-width table -- NOT read from any real texture. Lives in
// the same 8-unit "ink width, no +1" space render_sign_text.h documents for
// glyph_width[].
static uint8_t g_width[256];

static void init_widths(void) {
	memset(g_width, 6, sizeof(g_width)); // arbitrary default ink width
	g_width[(unsigned char) ' '] = 4;	  // matches gui_util's real space width
	// 'X': width+1 == 8 == the fixed glyph cell size (GLYPH_CELL_PX), so a
	// SINGLE 'X' line centers with pen_x0 == -4 exactly (total_width=8,
	// -8/2=-4, no truncation ambiguity) -- the probe glyph for tests 2/4/5.
	g_width[(unsigned char) 'X'] = 7;
}

static float deg2rad(float d) {
	return d * (float) M_PI / 180.0f;
}

// Independent re-derivation of render_sign_text.c's model chain for a
// single point, in plain scalar trig -- mirrors, in order:
//   r  = T2 * S * local                          (text-anchor T2, text S)
//   r += wall-only translate(0,-0.3125,-0.4375)   (wall signs only)
//   p  = R_y(-rotate_deg) * r
//   world = p + T1                                (outer translate T1)
// See render_sign_text.c's header comment for every cited decompile literal.
static void expected_world(float lx, float ly, float lz, float rotate_deg,
						   bool is_wall_sign, float out[3]) {
	float f = SIGN_TEXT_MODEL_SCALE;
	float s = 0.016666668f * f;

	float rx = s * lx;
	float ry = -s * ly + 0.5f * f;
	float rz = s * lz + 0.07f * f;

	if(is_wall_sign) {
		ry += -0.3125f;
		rz += -0.4375f;
		// 2026-10-31: the old +0.115 port "clearance" is gone -- text is
		// Beta-exact again and the wall board is Beta's SignModel board
		// (see test_wall_board_and_text_per_beta below).
	}

	float theta = deg2rad(-rotate_deg);
	float c = cosf(theta), sn = sinf(theta);

	float px = rx * c + rz * sn;
	float py = ry;
	float pz = -rx * sn + rz * c;

	out[0] = 0.5f + px;
	out[1] = 0.75f * f + py;
	out[2] = 0.5f + pz;
}

static void test_quad_count_excludes_spaces(void) {
	char grid[64];
	memset(grid, ' ', sizeof(grid));
	// line0: "HELLO WORLD" (11 chars, 10 non-space -- the interior space
	// between the two words counts toward width but emits no quad).
	memcpy(grid + 0 * SIGN_TEXT_QUAD_COLS, "HELLO WORLD", 11);
	// line1: left all-spaces -- a blank line, zero quads.
	// line2: "A" (1 non-space char).
	memcpy(grid + 2 * SIGN_TEXT_QUAD_COLS, "A", 1);
	// line3: "AB CD" (5 chars, 4 non-space).
	memcpy(grid + 3 * SIGN_TEXT_QUAD_COLS, "AB CD", 5);

	struct sign_text_vertex verts[SIGN_TEXT_MAX_VERTS];
	size_t n = sign_text_build_quads(grid, 0, false, g_width, verts,
									 SIGN_TEXT_MAX_VERTS);

	size_t expect_glyphs = 10 + 0 + 1 + 4;
	assert(n == expect_glyphs * 4);
}

static void test_line_centering_symmetric(void) {
	char grid[64];
	memset(grid, ' ', sizeof(grid));
	grid[0] = 'X'; // line0, col0 -- see init_widths()'s comment

	struct sign_text_vertex verts[SIGN_TEXT_MAX_VERTS];
	size_t n = sign_text_build_quads(grid, 0, false, g_width, verts,
									 SIGN_TEXT_MAX_VERTS);
	assert(n == 4);

	float f = SIGN_TEXT_MODEL_SCALE;
	float s = 0.016666668f * f;

	// meta==0, is_wall_sign==false -> rotate_deg==0 -> R_y(0)==identity, so
	// world_x == 0.5 + s*local_x with no rotation mixing X/Z. Quad corners
	// (order: top-left, top-right, bottom-right, bottom-left -- see
	// render_sign_text.c) local x: pen_x==-4 (left) and pen_x+8==+4 (right).
	float left_x = verts[0].x;
	float right_x = verts[1].x;

	// Symmetric about the block-centre column (world x==0.5 at local x==0):
	assert(fabsf((left_x - 0.5f) + (right_x - 0.5f)) < 1e-4f);
	assert(fabsf(left_x - (0.5f - 4.0f * s)) < 1e-4f);
	assert(fabsf(right_x - (0.5f + 4.0f * s)) < 1e-4f);
}

static void test_line_row_spacing(void) {
	char grid[64];
	memset(grid, ' ', sizeof(grid));
	for(int line = 0; line < SIGN_TEXT_QUAD_LINES; line++)
		grid[line * SIGN_TEXT_QUAD_COLS] = 'X';

	struct sign_text_vertex verts[SIGN_TEXT_MAX_VERTS];
	size_t n = sign_text_build_quads(grid, 0, false, g_width, verts,
									 SIGN_TEXT_MAX_VERTS);
	assert(n == 4 * SIGN_TEXT_QUAD_LINES); // one quad/line, meta 0 (post)

	float f = SIGN_TEXT_MODEL_SCALE;
	float s = 0.016666668f * f;

	// [TileEntitySignRenderer]: `var14*10 - signText.length*5` -- consecutive
	// lines are exactly 10 apart in font-pixel space; the text scale's Y is
	// NEGATED (glScalef(var12,-var12,var12)), so world Y DECREASES by s*10
	// per line (later lines sit lower, matching a top-to-bottom sign).
	for(int line = 0; line + 1 < SIGN_TEXT_QUAD_LINES; line++) {
		float y0 = verts[4 * line + 0].y;	   // top-left of this line's quad
		float y1 = verts[4 * (line + 1) + 0].y; // top-left of the next line
		assert(fabsf((y0 - y1) - s * 10.0f) < 1e-4f);
	}
}

static void test_post_rotation_metas(void) {
	// [TileEntitySignRenderer var11]: (float)(meta*360)/16.0F == meta*22.5.
	static const uint8_t metas[4] = {0, 4, 8, 12};
	static const float expect_deg[4] = {0.0f, 90.0f, 180.0f, 270.0f};

	for(int i = 0; i < 4; i++) {
		char grid[64];
		memset(grid, ' ', sizeof(grid));
		grid[0] = 'X';

		struct sign_text_vertex verts[SIGN_TEXT_MAX_VERTS];
		size_t n = sign_text_build_quads(grid, metas[i], false, g_width,
										 verts, SIGN_TEXT_MAX_VERTS);
		assert(n == 4);

		// Probe: top-left corner, local (pen_x=-4, pen_y=line0's -20, z=0).
		float expect[3];
		expected_world(-4.0f, -20.0f, 0.0f, expect_deg[i], false, expect);

		assert(fabsf(verts[0].x - expect[0]) < 1e-3f);
		assert(fabsf(verts[0].y - expect[1]) < 1e-3f);
		assert(fabsf(verts[0].z - expect[2]) < 1e-3f);
	}
}

static void test_wall_rotation_facings(void) {
	// [740c583 ItemSign/TileEntitySignRenderer] raw metadata 2/3/4/5 ==
	// FRONT/BACK/LEFT/RIGHT in this port's side naming.
	//
	// 2026-08-20: these expectations were CORRECTED BY IN-GAME CAPTURE. The
	// paper derivation in render_sign_text.c's header gave {0, 180, -90, 90}
	// -- reasoning from the face the sign is ATTACHED to. Beta's rotate
	// actually aims the sign's local +Z (the direction the text anchor's
	// forward offset pushes) along the direction the board LOOKS, which is
	// the opposite. The first capture settled it: legacy meta 4 (now raw
	// Beta meta 2) put its glyphs
	// BEHIND the plank and rendered a blank sign. This golden now pins the
	// table that demonstrably draws readable text
	// (docs proof: sign-text-in-world-zoom-8441e49.png on :8099).
	static const uint8_t metas[4] = {2, 3, 4, 5};
	static const float expect_deg[4] = {180.0f, 0.0f, 90.0f, -90.0f};

	for(int i = 0; i < 4; i++) {
		char grid[64];
		memset(grid, ' ', sizeof(grid));
		grid[0] = 'X';

		struct sign_text_vertex verts[SIGN_TEXT_MAX_VERTS];
		size_t n = sign_text_build_quads(grid, metas[i], true, g_width, verts,
										 SIGN_TEXT_MAX_VERTS);
		assert(n == 4);

		float expect[3];
		expected_world(-4.0f, -20.0f, 0.0f, expect_deg[i], true, expect);

		assert(fabsf(verts[0].x - expect[0]) < 1e-3f);
		assert(fabsf(verts[0].y - expect[1]) < 1e-3f);
		assert(fabsf(verts[0].z - expect[2]) < 1e-3f);
	}
}

// [TileEntitySignRenderer.java:35-61 + SignModel board addBox(-12,-14,-1,
// 24,12,2)] and [BlockSign.setBlockBoundsBasedOnState]: for wall metas 2..5
// the meshed board (SIGN_WALL_BOARD_*, used by render_block.c) must be
// Beta's board within the 1/256 vertex grid, sit inside Beta's wall bounds
// against the right wall, and every glyph must lie ON the board's front
// face (Beta: 0.07*s anchor vs the 1/16*s half-thickness, ~0.004 proud) and
// inside its outline. hw 7fd0d1b: text floated ~0.16 block off the board.
static void test_wall_board_and_text_per_beta(void) {
	static const uint8_t metas[4] = {2, 3, 4, 5};
	static const float yaw[4] = {180.0f, 0.0f, 90.0f, -90.0f};
	// Beta wall bounds: the axis/edge the board hugs (x=0/z=2, wall side).
	static const int axis[4] = {2, 2, 0, 0};
	static const float lo[4] = {0.875f, 0.0f, 0.875f, 0.0f};
	static const float hi[4] = {1.0f, 0.125f, 1.0f, 0.125f};
	// Outward (away from the wall) direction along that axis.
	static const float out_dir[4] = {-1.0f, 1.0f, -1.0f, 1.0f};

	float f = SIGN_TEXT_MODEL_SCALE;
	// Beta board in the rotated frame (block-centre x/z, block-bottom y).
	float bx = 12.0f / 16.0f * f;
	float by0 = 0.75f * f - 0.3125f + 2.0f / 16.0f * f;
	float by1 = 0.75f * f - 0.3125f + 14.0f / 16.0f * f;
	float bz0 = -0.4375f - 1.0f / 16.0f * f, bz1 = -0.4375f + 1.0f / 16.0f * f;
	const float grid_eps = 0.5f / 256.0f + 1e-5f;
	assert(fabsf(SIGN_WALL_BOARD_X0 / 256.0f + bx) <= grid_eps);
	assert(fabsf(SIGN_WALL_BOARD_X1 / 256.0f - bx) <= grid_eps);
	assert(fabsf(SIGN_WALL_BOARD_Y0 / 256.0f - by0) <= grid_eps);
	assert(fabsf(SIGN_WALL_BOARD_Y1 / 256.0f - by1) <= grid_eps);
	assert(fabsf(SIGN_WALL_BOARD_Z0 / 256.0f - bz0) <= grid_eps);
	assert(fabsf(SIGN_WALL_BOARD_Z1 / 256.0f - bz1) <= grid_eps);

	for(int i = 0; i < 4; i++) {
		// World AABB of the meshed board after Beta's yaw.
		float wmin[3] = {9, SIGN_WALL_BOARD_Y0 / 256.0f, 9};
		float wmax[3] = {-9, SIGN_WALL_BOARD_Y1 / 256.0f, -9};
		float th = deg2rad(-yaw[i]);
		float c = cosf(th), sn = sinf(th);
		for(int cx = 0; cx < 2; cx++)
			for(int cz = 0; cz < 2; cz++) {
				float x = (cx ? SIGN_WALL_BOARD_X1 : SIGN_WALL_BOARD_X0) / 256.0f;
				float z = (cz ? SIGN_WALL_BOARD_Z1 : SIGN_WALL_BOARD_Z0) / 256.0f;
				float wx = 0.5f + x * c + z * sn;
				float wz = 0.5f - x * sn + z * c;
				wmin[0] = fminf(wmin[0], wx); wmax[0] = fmaxf(wmax[0], wx);
				wmin[2] = fminf(wmin[2], wz); wmax[2] = fmaxf(wmax[2], wz);
			}
		int a = axis[i], l = 2 - a; // l = the lateral (width) axis
		// Inside Beta's wall-sign bounds on the wall axis, full width.
		assert(wmin[a] >= lo[i] - 1e-4f && wmax[a] <= hi[i] + 1e-4f);
		assert(fabsf(wmin[l] - 0.0f) < 1e-4f && fabsf(wmax[l] - 1.0f) < 1e-4f);
		float front = out_dir[i] > 0 ? wmax[a] : wmin[a];

		char grid[64];
		memset(grid, ' ', sizeof(grid));
		for(int line = 0; line < SIGN_TEXT_QUAD_LINES; line++)
			memcpy(grid + line * SIGN_TEXT_QUAD_COLS, "XXXXXX", 6);
		struct sign_text_vertex verts[SIGN_TEXT_MAX_VERTS];
		size_t n = sign_text_build_quads(grid, metas[i], true, g_width, verts,
										 SIGN_TEXT_MAX_VERTS);
		assert(n == 4 * 6 * SIGN_TEXT_QUAD_LINES);
		for(size_t v = 0; v < n; v++) {
			float p[3] = {verts[v].x, verts[v].y, verts[v].z};
			// On the front face: in front of it by Beta's (0.07 - 1/16)*s
			// margin (+ grid rounding), never behind it, never floating.
			float proud = (p[a] - front) * out_dir[i];
			assert(proud > 0.0f && proud < 0.01f);
			// Inside the board outline.
			assert(p[l] > wmin[l] && p[l] < wmax[l]);
			assert(p[1] > wmin[1] && p[1] < wmax[1]);
		}
		printf("wall meta %d: board %c [%.4f,%.4f] y [%.4f,%.4f], text %.4f "
			   "proud of front %.4f\n",
			   metas[i], a == 0 ? 'x' : 'z', wmin[a], wmax[a], wmin[1], wmax[1],
			   (verts[0].x * (a == 0) + verts[0].z * (a == 2) - front)
				   * out_dir[i],
			   front);
	}
}

static void test_glyph_uv(void) {
	char grid[64];
	memset(grid, ' ', sizeof(grid));
	grid[0] = 'A'; // 0x41 == 65 -> atlas cell (65%16, 65/16) == (1, 4)
	grid[1] = '0'; // 0x30 == 48 -> atlas cell (48%16, 48/16) == (0, 3)

	struct sign_text_vertex verts[SIGN_TEXT_MAX_VERTS];
	size_t n = sign_text_build_quads(grid, 0, false, g_width, verts,
									 SIGN_TEXT_MAX_VERTS);
	assert(n == 8); // 2 glyphs * 4 verts

	// 'A' quad (verts[0..3]): u in [16/256, 32/256], v in [64/256, 80/256] --
	// same cell gui_util.c's gutil_text_rgb computes via
	// `tex_x = ch%16*16; tex_y = ch/16*16;` (ch==65 -> tex_x=16, tex_y=64).
	assert(fabsf(verts[0].u - 16.0f / 256.0f) < 1e-6f);
	assert(fabsf(verts[0].v - 64.0f / 256.0f) < 1e-6f);
	assert(fabsf(verts[1].u - 32.0f / 256.0f) < 1e-6f);
	assert(fabsf(verts[2].v - 80.0f / 256.0f) < 1e-6f);

	// '0' quad (verts[4..7]): tex_x=0, tex_y=48 -> u in [0,16/256], v in
	// [48/256, 64/256].
	assert(fabsf(verts[4].u - 0.0f) < 1e-6f);
	assert(fabsf(verts[4].v - 48.0f / 256.0f) < 1e-6f);
	assert(fabsf(verts[5].u - 16.0f / 256.0f) < 1e-6f);
	assert(fabsf(verts[6].v - 64.0f / 256.0f) < 1e-6f);
}

// ci-1101/30: Beta colour codes on a sign line. [mcb173-retromcp
// FontRenderer.java:147-161 renderString, :183-202 getStringWidth;
// TileEntitySignRenderer.java:56-58 centres with getStringWidth, base colour
// 0]. "\247cRed \247fWhite" must draw glyphs for "Red White" ONLY, Red in
// colour 12 (255,85,85), White in colour 15 (255,255,255), centred on the
// code-free width; a code-free line stays Beta's black.
static void test_colour_codes(void) {
	char grid[64];
	memset(grid, ' ', sizeof(grid));
	const char* line0 = "\247cRed \247fWhite";
	memcpy(grid, line0, strlen(line0));
	memcpy(grid + 1 * SIGN_TEXT_QUAD_COLS, "AB", 2);
	// uppercase selector selects too; an unknown one (1.4+ 'l') is white.
	memcpy(grid + 2 * SIGN_TEXT_QUAD_COLS, "\247Ax\247ly", 6);
	// "\247\247c": the 2nd 167 is the first code's selector (unknown ->
	// white), so 'c' is DRAWN.
	memcpy(grid + 3 * SIGN_TEXT_QUAD_COLS, "\247\247" "c", 3);

	struct sign_text_vertex verts[SIGN_TEXT_MAX_VERTS];
	size_t n = sign_text_build_quads(grid, 0, false, g_width, verts,
									 SIGN_TEXT_MAX_VERTS);
	// glyphs: "Red"+"White" (8) + "AB" (2) + "xy" (2) + "c" (1)
	assert(n == 13 * 4);

	const char* drawn = "RedWhiteABxyc";
	for(size_t q = 0; q < 13; q++) {
		unsigned char ch = (unsigned char)drawn[q];
		assert(fabsf(verts[q * 4].u - (float)(ch % 16) * 16.0f / 256.0f) < 1e-6f);
		assert(fabsf(verts[q * 4].v - (float)(ch / 16) * 16.0f / 256.0f) < 1e-6f);
	}
	for(size_t k = 0; k < 4; k++) {
		const struct sign_text_vertex* red = &verts[0 * 4 + k];
		const struct sign_text_vertex* white = &verts[3 * 4 + k];
		const struct sign_text_vertex* plain = &verts[8 * 4 + k];
		const struct sign_text_vertex* green_a = &verts[10 * 4 + k];
		const struct sign_text_vertex* unk = &verts[11 * 4 + k];
		const struct sign_text_vertex* c = &verts[12 * 4 + k];
		assert(red->r == 255 && red->g == 85 && red->b == 85);
		assert(white->r == 255 && white->g == 255 && white->b == 255);
		assert(plain->r == 0 && plain->g == 0 && plain->b == 0);
		assert(green_a->r == 85 && green_a->g == 255 && green_a->b == 85);
		assert(unk->r == 255 && unk->g == 255 && unk->b == 255);
		assert(c->r == 255 && c->g == 255 && c->b == 255);
	}

	// Width: R,e,d,W,h,i,t,e at 6+1 and one space at 4+1 = 61; codes add 0.
	// pen_x0 = -61/2 = -30 (Java int division).
	float f = SIGN_TEXT_MODEL_SCALE;
	float s = 0.016666668f * f;
	assert(fabsf(verts[0].x - (0.5f - 30.0f * s)) < 1e-4f);
	// 'W' starts after "Red " = 21 + 5 = 26 units.
	assert(fabsf(verts[3 * 4].x - (0.5f + (-30.0f + 26.0f) * s)) < 1e-4f);
}

int main(void) {
	init_widths();

	test_quad_count_excludes_spaces();
	test_line_centering_symmetric();
	test_line_row_spacing();
	test_post_rotation_metas();
	test_wall_rotation_facings();
	test_wall_board_and_text_per_beta();
	test_glyph_uv();
	test_colour_codes();

	printf("test_sign_text_quads: all assertions passed\n");
	return 0;
}
