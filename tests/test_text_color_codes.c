/* ci-1101/30: Beta colour codes in screen text (chat lines, disconnect
 * reasons, server list) -- gui_util.c's real gutil_text_rgba and
 * gutil_font_width, with only the graphics boundary wrapped.
 *
 * [mcb173-retromcp FontRenderer.java:115-117] drawStringWithShadow draws the
 * shadow pass at +1,+1 first; [:124-181] renderString: 167 + selector is a
 * colour change and draws nothing (upper case selects too, an unknown
 * selector is white, colour list +16 = the /4 shadow shade); [:183-202]
 * getStringWidth skips the 167 and its selector. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/gui/chat_composer_model.h"
#include "game/gui/widget_osk.h"
#include "graphics/gui_util.h"
#include "graphics/render_sign_text.h"
#include "lodepng/lodepng.h"
#include "platform/gfx.h"
#include "platform/texture.h"

void gutil_test_set_font_widths(const uint8_t widths[256]);

#define MAX_QUADS 64
static struct {
	int16_t x;
	uint8_t r, g, b, a;
	uint16_t u, v;
} quads[MAX_QUADS];
static size_t quad_count;

void __wrap_gfx_bind_texture(struct tex_gfx* t) {
	assert(t == &texture_font);
}

void __wrap_gfx_draw_quads(size_t n, const int16_t* v, const uint8_t* c,
						   const uint16_t* uv) {
	assert(n == 4 && quad_count < MAX_QUADS);
	for(size_t k = 1; k < 4; k++) /* one flat colour per glyph quad */
		assert(!memcmp(c, c + k * 4, 4));
	quads[quad_count].x = v[0];
	quads[quad_count].r = c[0];
	quads[quad_count].g = c[1];
	quads[quad_count].b = c[2];
	quads[quad_count].a = c[3];
	quads[quad_count].u = uv[0];
	quads[quad_count].v = uv[1];
	quad_count++;
}

static void check_glyph(size_t q, char ch, int x, uint8_t r, uint8_t g,
						uint8_t b) {
	unsigned char glyph = (unsigned char)ch;
	assert(quads[q].u == glyph % 16 * 16 && quads[q].v == glyph / 16 * 16);
	assert(quads[q].x == x);
	assert(quads[q].r == r && quads[q].g == g && quads[q].b == b);
}

/* Maintainer 11-01: signs and colour codes with EACH pack font. Metrics are
 * measured from the real PNG exactly like gutil_reset_font. Sign board
 * bounds, post sign, text-local units [TileEntitySignRenderer.java + SignModel
 * board addBox(-12,-14,-1, 24,12,2)]: model px p -> p*s/16 blocks, text unit
 * u -> u*s/60 blocks, so the board spans x in [-45,45] and, with the text
 * anchor 0.5*s above the post origin and the board at y px -14..-2 under
 * scale(s,-s,-s), text y in [-22.5,22.5]. */
static bool load_pack_widths(const char* rel, uint8_t widths[256]) {
	char path[1024];
	snprintf(path, sizeof path, "%s", __FILE__);
	char* base = strstr(path, "tests");
	assert(base);
	strcpy(base, rel);
	FILE* probe = fopen(path, "rb");
	if(!probe)
		return false;
	fclose(probe);
	unsigned char* px = NULL;
	unsigned w = 0, h = 0;
	assert(!lodepng_decode32_file(&px, &w, &h, path));
	assert(w >= 128 && h >= 128 && w % 16 == 0 && h % 16 == 0);
	unsigned cw = w / 16, ch = h / 16;
	for(unsigned cell = 0; cell < 256; cell++) {
		unsigned ink = 0;
		for(unsigned x = 0; x < cw; x++) {
			bool present = false;
			for(unsigned y = 0; y < ch && !present; y++) {
				const unsigned char* p
					= px + 4 * ((cell / 16 * ch + y) * w + cell % 16 * cw + x);
				present = p[0] || p[1] || p[2];
			}
			if(!present)
				break;
			ink++;
		}
		widths[cell] = (uint8_t)(ink * 8 / cw);
	}
	widths[' '] = 4;
	free(px);
	return true;
}

static void check_pack(const char* name, const char* rel, bool optional) {
	uint8_t widths[256];
	if(!load_pack_widths(rel, widths)) {
		assert(optional && "a shipped pack font is missing");
		printf("pack %s: %s absent locally (user-owned Beta font) -- skipped\n",
			   name, rel);
		return;
	}
	gutil_test_set_font_widths(widths);
	const uint8_t* gw = gutil_font_glyph_widths();

	/* Chat: codes never drawn, colours, code-free width from THIS font. */
	const char* line = "\247cRed \247fWhite";
	int expect = 0;
	for(const char* c = "Red White"; *c; c++)
		expect += gw[(unsigned char)*c] + 1;
	assert(gutil_font_width(line, 8) == expect);
	quad_count = 0;
	gutil_text_rgba(0, 0, line, 8, false, 0xFF, 0xFF, 0xFF, 0xFF);
	assert(quad_count == 9);
	for(size_t q = 0; q < 9; q++) {
		unsigned char g = (unsigned char)"Red White"[q];
		assert(quads[q].u == g % 16 * 16 && quads[q].v == g / 16 * 16);
		assert(quads[q].r == 255 && quads[q].g == (q < 4 ? 85 : 255));
	}

	/* Sign: three full 15-char lines plus a coded line; post meta 0. */
	char grid[64];
	memset(grid, ' ', sizeof grid);
	memcpy(grid + 0, "Welcome, friend", 15);
	memcpy(grid + 16, "Spawn this way!", 15);
	memcpy(grid + 32, "\2474Keep \2470out!!!", 15);
	memcpy(grid + 48, "x=128 y=64 z=-9", 15);
	struct sign_text_vertex v[SIGN_TEXT_MAX_VERTS];
	size_t n = sign_text_build_quads(grid, 0, false, gw, v, SIGN_TEXT_MAX_VERTS);
	size_t glyphs = 0;
	for(int i = 0; i < 64; i++) {
		char c = grid[i];
		if(c == '\247') { i++; continue; }
		if(c != ' ') glyphs++;
	}
	assert(n == glyphs * 4); /* codes never become quads */
	float s = SIGN_TEXT_MODEL_SCALE, t = 0.016666668f * s;
	float max_x = 0, min_y = 1e9f, max_y = -1e9f;
	/* Bound the INK, not the 8-unit glyph cell: Beta draws the same full
	 * cell (FontRenderer.java:69-77, 7.99 units), whose blank right part
	 * may overhang the board edge without anything visible there. */
	for(size_t q = 0; q < n / 4; q++) {
		const struct sign_text_vertex* c = &v[q * 4];
		unsigned glyph = (unsigned)(c->u * 16.0f + 0.5f)
			+ 16 * (unsigned)(c->v * 16.0f + 0.5f);
		assert(glyph != 0xA7);
		float left = (c[0].x - 0.5f) / t;
		float right = left + gw[glyph];
		if(fabsf(left) > max_x) max_x = fabsf(left);
		if(fabsf(right) > max_x) max_x = fabsf(right);
		for(int k = 0; k < 4; k++) {
			float ly = (0.75f * s + 0.5f * s - c[k].y) / t;
			if(ly < min_y) min_y = ly;
			if(ly > max_y) max_y = ly;
		}
	}
	/* "Keep " dark red (4), "out!!!" black (0); the other lines black. */
	bool saw_red = false;
	for(size_t k = 0; k < n; k++)
		if(v[k].r == 0xAA && v[k].g == 0 && v[k].b == 0) saw_red = true;
	assert(saw_red);
	printf("pack %s: sign ink max |x| %.2f of 45, y %.2f..%.2f of +-22.5\n", name,
		   max_x, min_y, max_y);
	assert(max_x <= 45.0f + 1e-3f && "a 15-char line overflows the board");
	assert(min_y >= -22.5f - 1e-3f && max_y <= 22.5f + 1e-3f);
}

/* Coordinator 11-01 (2): chat wrap/clip never split a colour pair.
 * [mcb173-retromcp GuiIngame.java:381-389 addChatMessage] grows the head
 * while getStringWidth(s[0..i+1]) <= 320; 0xA7 and its selector add 0
 * width (FontRenderer.java:190-191), so the loop always steps past BOTH and
 * a pair is never split. The continuation is a plain substring: NO colour
 * is carried (it draws in the default colour), and chat_wrap matches. */
static int width8(const char* s) {
	return gutil_font_width(s, 8);
}

static void check_chat_pairs(void) {
	uint8_t widths[256];
	memset(widths, 6, sizeof widths); /* 7 units per char */
	widths[' '] = 4;
	gutil_test_set_font_widths(widths);

	/* 45 chars = 315 fit 320; the 46th visible char would not. The code
	 * sits exactly on the boundary. */
	char line[64];
	memset(line, 'a', 45);
	memcpy(line + 45, "\247" "cbcd", 6);
	char out[8][CHAT_LINE_BYTES];
	assert(chat_wrap(line, 320, width8, out, 8) == 2);
	assert(strlen(out[0]) == 47 && out[0][45] == '\247' && out[0][46] == 'c');
	assert(!strcmp(out[1], "bcd")); /* Beta: continuation uncoloured */

	/* A second code right after: both pairs stay whole in the head. */
	memcpy(line + 45, "\247" "c" "\247" "9xy", 7);
	assert(chat_wrap(line, 320, width8, out, 8) == 2);
	assert(strlen(out[0]) == 49 && !strcmp(out[1], "xy"));

	/* Every split of a coded line: no piece starts with an orphan selector
	 * (the byte after a 0xA7 that ended the previous piece). */
	for(int len = 40; len <= 50; len++) {
		char l2[80];
		memset(l2, 'k', (size_t)len);
		memcpy(l2 + len, "\247" "e" "end" "\247" "ftail", 10);
		size_t n = chat_wrap(l2, 320, width8, out, 8);
		for(size_t k = 0; k + 1 < n; k++) {
			size_t pl = strlen(out[k]);
			assert(pl > 0 && out[k][pl - 1] != '\247');
		}
	}

	/* Open-band clip (chat_hud.c chat_clip_display): a cut can never leave
	 * a 0xA7 without its selector, so nothing swallows/leaks a glyph. */
	for(int clip = 300; clip <= 330; clip++) {
		char cut[CHAT_LINE_BYTES];
		size_t n = chat_clip_display(line, cut, sizeof cut, clip, width8);
		assert(width8(cut) <= clip);
		size_t scan = 0;
		while(scan < n)
			scan += cut[scan] == '\247' ? 2 : 1;
		assert(scan == n); /* every 0xA7 in `cut` has its selector */
	}

	/* The draft (screen_chat.c tail scroll) cannot hold a 0xA7 at all:
	 * osk_feed_char takes only 32..126, so a scroll can never start on a
	 * selector. */
	char buf[8] = "";
	struct osk_state o;
	osk_reset(&o, buf, sizeof buf, 0);
	osk_feed_char(&o, '\247');
	assert(buf[0] == 0);
	puts("PASS chat wrap/clip keep colour pairs whole; draft rejects 0xA7");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); /* per-pack lines survive an assert */
	uint8_t widths[256];
	memset(widths, 6, sizeof widths);
	widths[' '] = 4;
	gutil_test_set_font_widths(widths);

	/* The reported line: glyphs for "Red White" only. */
	const char* line = "\247cRed \247fWhite";
	assert(gutil_font_width(line, 8) == 61); /* 8*7 + 5, codes add 0 */
	quad_count = 0;
	gutil_text_rgba(0, 0, line, 8, true, 0xFF, 0xFF, 0xFF, 0xFF);
	assert(quad_count == 9 * 2); /* shadow + face per drawn glyph */
	const char* drawn = "Red White";
	int x = 0;
	for(size_t i = 0; i < 9; i++) {
		bool red = i < 4; /* the space after "Red" is still red */
		uint8_t fr = 255, fg = red ? 85 : 255, fb = red ? 85 : 255;
		/* Beta shadow list entry = colour / 4 (FontRenderer.java:102-106) */
		check_glyph(i * 2, drawn[i], x + 1, fr / 4, fg / 4, fb / 4);
		check_glyph(i * 2 + 1, drawn[i], x, fr, fg, fb);
		assert(quads[i * 2].a == 0xFF && quads[i * 2 + 1].a == 0xFF);
		x += drawn[i] == ' ' ? 5 : 7;
	}

	/* Upper-case selector, unknown 1.4+ selector (-> white), the 167 that is
	 * itself a selector ("\247\247c" draws 'c' in white), and a trailing lone
	 * 167 (draws nothing, width 0). Base colour 64,64,64 survives until the
	 * first code. */
	const char* edge = "a\247Ab\247lc\247\247" "d\247";
	assert(gutil_font_width(edge, 8) == 4 * 7);
	quad_count = 0;
	gutil_text_rgba(0, 0, edge, 8, false, 64, 64, 64, 0xFF);
	assert(quad_count == 4);
	check_glyph(0, 'a', 0, 64, 64, 64);
	check_glyph(1, 'b', 7, 85, 255, 85);
	check_glyph(2, 'c', 14, 255, 255, 255);
	check_glyph(3, 'd', 21, 255, 255, 255);

	/* Review R8: gold. [FontRenderer.java:83-106] list 6 = (170,170,0) with
	 * `if(var9 == 6) var11 += 85` -> (255,170,0); its shadow is list 22,
	 * where var9 != 6, so NO boost: (170,170,0)/4 = (42,42,0). An explicit
	 * base colour's shadow is (c & 0xFC) >> 2 [FontRenderer.java:127-131]. */
	quad_count = 0;
	gutil_text_rgba(0, 0, "g\2476G", 8, true, 0xFF, 0x7F, 0x03, 0xFF);
	assert(quad_count == 4);
	check_glyph(0, 'g', 1, 0x3F, 0x1F, 0x00);
	check_glyph(1, 'g', 0, 0xFF, 0x7F, 0x03);
	check_glyph(2, 'G', 8, 42, 42, 0);
	check_glyph(3, 'G', 7, 255, 170, 0);
	for(int k = 0; k < 16; k++) { /* every shadow = Beta's list k+16 */
		char s[4] = {'\247', "0123456789abcdef"[k], 'x', 0};
		quad_count = 0;
		gutil_text_rgba(0, 0, s, 8, true, 1, 2, 3, 0xFF);
		int base = (k >> 3 & 1) * 85;
		int r = ((k >> 2 & 1) * 170 + base + (k == 6 ? 85 : 0));
		int g = (k >> 1 & 1) * 170 + base, b = (k & 1) * 170 + base;
		int sr = ((k >> 2 & 1) * 170 + base) / 4;
		assert(quad_count == 2);
		assert(quads[0].r == sr && quads[0].g == g / 4 && quads[0].b == b / 4);
		assert(quads[1].r == r && quads[1].g == g && quads[1].b == b);
	}

	/* A faded line (chat fade-out): the colour lists are glColor3f, which
	 * resets alpha to 1.0 [FontRenderer.java:109], so only the uncoded
	 * prefix fades. */
	quad_count = 0;
	gutil_text_rgba(0, 0, "a\247cb", 8, true, 0xFF, 0xFF, 0xFF, 0x40);
	assert(quad_count == 4);
	assert(quads[0].a == 0x40 && quads[1].a == 0x40);
	assert(quads[2].a == 0xFF && quads[3].a == 0xFF);

	/* Doubled-scale chat (GFX_GUI_SCALE 2 => scale 16) keeps per-glyph
	 * rounding and still drops both code chars from the width. */
	assert(gutil_font_width("\2476ab", 16) == 2 * 14);

	check_chat_pairs();
	check_pack("Earth & Ash (bundled default)", "assets/font/default.png", false);
	check_pack("Isabella II", "packs/isabella-ii/font/default.png", false);
	check_pack("Beta 1.7.3 (user-owned)", "texturepacks/beta-1.7.3/font/default.png", true);

	puts("PASS colour codes: no glyphs drawn, Beta colours + shadow, code-free width");
	return 0;
}
