/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/gui/loading_tips.h"
#include "lodepng/lodepng.h"

static struct loading_tip_context context;

/* No graphics context in this focused pure-logic test. These are the REAL
 * assets/font/default.png pixels, scanned exactly as gutil_reset_font scans
 * tex_gfx_lookup RGB in gui_util.c; widths then use gutil_font_width's
 * (ink+1)*16/8 integer advance. No average-character approximation. */
static void load_metrics(const char* path) {
	unsigned char* pixels = NULL;
	unsigned w = 0, h = 0;
	assert(lodepng_decode32_file(&pixels, &w, &h, path) == 0);
	assert(w >= 128 && h >= 128 && w % 16 == 0 && h % 16 == 0);
	unsigned cw = w / 16, ch = h / 16;
	for(unsigned cell = 0; cell < 256; cell++) {
		unsigned ink = 0;
		for(unsigned x = 0; x < cw; x++) {
			bool present = false;
			for(unsigned y = 0; y < ch; y++) {
				const unsigned char* p = pixels + 4 *
					((cell / 16 * ch + y) * w + cell % 16 * cw + x);
				if(p[0] || p[1] || p[2]) { present = true; break; }
			}
			if(!present) break;
			ink++;
		}
		context.font[cell] = (uint8_t)(ink * 8 / cw);
	}
	context.font[' '] = 4;
	free(pixels);
	context.width = 640;
	context.visible = context.enabled = true;
	for(unsigned k = 0; k < LOADING_TIP_ACTIONS; k++) {
		context.glyph[k] = (int)k;
		context.glyph_width[k] = 24;
	}
}

static const struct loading_tip* start(unsigned owner, enum loading_reason reason,
	const char* profile, uint64_t now) {
	loading_reason_begin(owner, reason);
	assert(loading_tips_set_identity(owner, profile));
	assert(!loading_tips_tick(owner, now, &context));
	return loading_tips_tick(owner, now + (reason == PORTAL_IN || reason == PORTAL_OUT
		? 4000 : 1250), &context);
}

static void test_dwell(void) {
	static const struct { unsigned words; uint32_t read, dwell; } cases[] = {
		{0,6000,8000}, {8,6000,8000}, {13,8500,8500}, {14,9000,9000},
		{18,11000,11000}, {22,13000,13000}, {24,14000,14000},
		{34,19000,19000}, {UINT32_MAX,UINT32_MAX,UINT32_MAX}
	};
	for(unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		assert(loading_tips_read_ms(cases[k].words) == cases[k].read);
		assert(loading_tips_dwell_ms(cases[k].words) == cases[k].dwell);
	}
	assert(loading_tips_word_count("{previous} and {next} change your hotbar slot.") == 9);
	assert(loading_tips_word_count("After loading, {inventory} opens your inventory.") == 7);
	assert(loading_tips_word_count(NULL) == 0);
	puts("PASS dwell: clamp edges, read floor, glyph words and overflow");
}

static void test_gates_and_headings(void) {
	static const char* headings[] = {"Generating level", "Loading level",
		"Entering the Nether", "Leaving the Nether", "Respawning",
		"Loading level", "Loading level", "Loading world"};
	for(unsigned r = NEW_WORLD; r <= UNKNOWN; r++) {
		loading_tips_reset();
		loading_reason_begin(0, (enum loading_reason)r);
		assert(loading_tips_set_identity(0, "gate"));
		assert(!strcmp(loading_reason_heading((enum loading_reason)r), headings[r]));
		assert(!strcmp(loading_reason_stage((enum loading_reason)r), "Building terrain"));
		assert(!loading_tips_tick(0, 100000, &context));
		unsigned gate = r == PORTAL_IN || r == PORTAL_OUT ? 4000 : 1250;
		assert(!loading_tips_tick(0, 100000 + gate - 10, &context));
		const struct loading_tip* t = loading_tips_tick(0, 100000 + gate, &context);
		if(r == RESPAWN || r == UNKNOWN) {
			assert(!t); assert(!loading_tips_tick(0, 200000, &context));
		} else assert(t);
		loading_reason_end(0);
		assert(loading_reason_get(0) == UNKNOWN);
		assert(!loading_tips_tick(0, 300000, &context));
	}
	assert(loading_reason_get(2) == UNKNOWN);
	assert(!loading_tips_set_identity(2, "bad"));
	puts("PASS gates: 1.24/1.25s, 3.99/4.0s, disabled reasons; Beta headings");
}

static void test_beginner_and_nether(void) {
	static const char* beginners[] = {"S07", "C01", "C04", "C02", "C03", "S04"};
	loading_tips_reset();
	uint64_t now = 1250;
	const struct loading_tip* t = start(0, NEW_WORLD, "beginner", 0);
	for(unsigned k = 0; k < 6; k++) {
		assert(t && !strcmp(t->id, beginners[k]));
		now += loading_tips_dwell_ms(loading_tips_word_count(t->text));
		t = loading_tips_tick(0, now, &context);
	}
	struct loading_tip_history h;
	assert(loading_tips_history("beginner", &h));
	assert(h.beginner_bits == 63);
	loading_reason_end(0);
	t = start(0, NEW_WORLD, "beginner", now);
	for(unsigned k = 0; k < 6; k++) assert(strcmp(t->id, beginners[k]));
	loading_tips_reset();
	now = 4000; t = start(0, PORTAL_IN, "nether", 0);
	for(unsigned k = 1; k <= 7; k++) {
		char id[4]; snprintf(id, sizeof id, "N%02u", k);
		assert(t && !strcmp(t->id, id));
		now += loading_tips_dwell_ms(loading_tips_word_count(t->text));
		t = loading_tips_tick(0, now, &context);
	}
	assert(t->category != LOADING_TIP_NETHER);
	loading_reason_end(0);
	t = start(0, PORTAL_OUT, "nether", now);
	assert(t && t->category != LOADING_TIP_NETHER && t->category != LOADING_TIP_SPLIT);
	now += 4000;
	for(unsigned k = 0; k < 24; k++) {
		now += loading_tips_dwell_ms(loading_tips_word_count(t->text));
		t = loading_tips_tick(0, now, &context);
		assert(t);
	}
	assert(loading_tips_history("nether", &h) && h.nether_bits == 127);
	loading_reason_end(0);
	t = start(0, PORTAL_IN, "nether", now);
	assert(t && t->category != LOADING_TIP_NETHER);
	puts("PASS ordering: beginner six; N01-N03 then N04-N06 then N07; general exit");
}

static void test_history(void) {
	loading_tips_reset();
	const struct loading_tip* t = start(0, NEW_WORLD, "unread", 0);
	assert(t && !strcmp(t->id, "S07"));
	(void)loading_tips_tick(0, 2249, &context);
	loading_reason_end(0);
	struct loading_tip_history h;
	assert(loading_tips_history("unread", &h));
	assert(h.read_count == 0 && h.beginner_bits == 0);
	t = start(0, NEW_WORLD, "unread", 3000);
	assert(!strcmp(t->id, "S07"));
	loading_reason_end(0);
	loading_tips_reset();
	for(unsigned k = 0; k < 8; k++) {
		char key[16]; snprintf(key, sizeof key, "identity%u", k);
		assert(loading_tips_set_identity(0, key));
	}
	assert(loading_tips_set_identity(0, "identity0")); /* refresh LRU */
	assert(loading_tips_set_identity(0, "identity8"));
	assert(!loading_tips_history("identity1", &h));
	assert(loading_tips_history("identity0", &h));
	for(unsigned k = 2; k < 9; k++) {
		char key[16]; snprintf(key, sizeof key, "identity%u", k);
		assert(loading_tips_history(key, &h));
	}
	loading_reason_begin(1, LOAD_SAVE);
	assert(loading_tips_set_identity(1, "identity0"));
	for(unsigned k = 9; k < 20; k++) {
		char key[16]; snprintf(key, sizeof key, "identity%u", k);
		assert(loading_tips_set_identity(0, key));
	}
	assert(loading_tips_history("identity0", &h)); /* active guest pins its history */
	loading_tips_reset();
	uint64_t now = 1250, seen = 0;
	struct loading_tip_context neutral = context;
	for(unsigned k = 0; k < LOADING_TIP_ACTIONS; k++) neutral.glyph[k] = -1;
	loading_reason_begin(0, LOAD_SAVE);
	assert(loading_tips_set_identity(0, "cycle"));
	assert(!loading_tips_tick(0, 0, &neutral));
	t = loading_tips_tick(0, now, &neutral);
	/* Every eligible neutral ID: 62 minus seven Nether, four split and five
	 * unbound controls. Control spacing is exercised separately below. */
	for(unsigned k = 0; k < 46; k++) {
		assert(t);
		unsigned id = (unsigned)(t - loading_tips);
		assert(!(seen & (UINT64_C(1) << id)));
		seen |= UINT64_C(1) << id;
		now += loading_tips_dwell_ms(loading_tips_word_count(t->text));
		t = loading_tips_tick(0, now, &neutral);
	}
	assert(t);
	assert(loading_tips_history("cycle", &h) && h.read_count == 16);
	for(unsigned k = 0; k < h.read_count; k++) assert(h.last_read[k] != t - loading_tips);
	puts("PASS history: eight-identity LRU, interrupted unread beginner, full cycle, last16 exclusion");
}

static void test_contexts(void) {
	loading_tips_reset();
	for(unsigned k = 0; k < 100; k++) {
		const struct loading_tip* t = start(0, MP_TERRAIN, "remote", k * 20000u);
		assert(t && t->category != LOADING_TIP_MOBS && t->category != LOADING_TIP_SPLIT);
		assert(strcmp(t->id, "S04") && strcmp(t->id, "S06"));
		assert(!loading_tips_tick(0, k * 20000u + 10000, &context));
		assert(!loading_tips_tick(0, k * 20000u + 11000, &context));
		loading_reason_end(0);
	}
	loading_tips_reset();
	context.gamecube = true;
	uint64_t now = 1250;
	const struct loading_tip* t = start(0, LOAD_SAVE, "gc", 0);
	unsigned since_control = 2;
	for(unsigned k = 0; k < 120; k++) {
		assert(t && strcmp(t->id, "W05"));
		if(t->category == LOADING_TIP_CONTROLS) {
			assert(since_control >= 2); since_control = 0;
		} else since_control++;
		now += loading_tips_dwell_ms(loading_tips_word_count(t->text));
		t = loading_tips_tick(0, now, &context);
	}
	context.gamecube = false;
	loading_tips_reset();
	t = start(0, SPLIT_START, "shared", 0);
	assert(t && t->category != LOADING_TIP_CONTROLS && t->category != LOADING_TIP_SPLIT);
	loading_tips_reset();
	context.creative = true; context.peaceful = true;
	t = start(0, LOAD_SAVE, "creative", 0);
	assert(t && t->category != LOADING_TIP_MOBS && t->category != LOADING_TIP_SURVIVAL);
	context.creative = context.peaceful = false;
	puts("PASS filters: MP death/mobs, sticky 10s hint, GC W05, control cooldown, shared/Creative");
}

static void test_hidden_and_owners(void) {
	loading_tips_reset();
	const struct loading_tip* t = start(0, LOAD_SAVE, "host", 0);
	assert(t);
	struct loading_tip_context hidden = context;
	hidden.visible = false;
	assert(!loading_tips_tick(0, 2000, &hidden));
	assert(!loading_tips_tick(0, 9000, &context));
	assert(!loading_tips_tick(0, 10240, &context));
	assert(loading_tips_tick(0, 10250, &context) == t);
	struct loading_tip_history h;
	assert(loading_tips_history("host", &h) && h.read_count == 0);
	context.split_pane = context.split_active = true;
	const struct loading_tip* guest = start(1, LOAD_SAVE, "guest", 11000);
	assert(guest && guest != t);
	loading_reason_end(1);
	assert(loading_reason_get(0) == LOAD_SAVE);
	assert(loading_tips_current_layout(0));
	hidden = context; hidden.blocked = true;
	assert(!loading_tips_tick(0, 14000, &hidden));
	assert(!loading_tips_tick(0, 20000, &context));
	context.split_pane = context.split_active = false;
	loading_tips_reset();
	uint64_t now = 1250;
	t = start(0, LOAD_SAVE, "remap", 0);
	for(unsigned k = 0; t->category != LOADING_TIP_CONTROLS; k++) {
		assert(k < 200);
		now += loading_tips_dwell_ms(loading_tips_word_count(t->text));
		t = loading_tips_tick(0, now, &context);
		assert(t);
	}
	struct loading_tip_context unbound = context;
	for(unsigned k = 0; k < LOADING_TIP_ACTIONS; k++) unbound.glyph[k] = -1;
	assert(!loading_tips_tick(0, now + 1, &unbound));
	assert(!loading_tips_tick(0, now + 1241, &unbound));
	t = loading_tips_tick(0, now + 1251, &unbound);
	assert(t && t->category != LOADING_TIP_CONTROLS);
	unbound.enabled = false;
	assert(!loading_tips_tick(0, now + 1300, &unbound));
	puts("PASS visibility: hidden dwell/gate, owner isolation/duplicate avoidance, sticky failure");
}

static void test_fit(void) {
	static const int widths[] = {640,854};
	unsigned cases = 0;
	for(unsigned width = 0; width < 2; width++)
		for(unsigned split = 0; split < 2; split++)
			for(unsigned chord = 0; chord < 2; chord++) {
				struct loading_tip_context c = context;
				c.width = widths[width]; c.split_pane = split != 0;
				for(unsigned a = 0; a < LOADING_TIP_ACTIONS; a++) {
					c.glyph[a] = chord ? 64 : 0;
					c.glyph_width[a] = chord ? 60 : 24;
				}
				for(unsigned k = 0; k < LOADING_TIPS_COUNT; k++) {
					struct loading_tip_layout l;
					assert(loading_tips_layout(&loading_tips[k], &c, &l));
					assert(l.x >= 32 && l.x + l.width <= c.width - 32);
					assert(l.y >= (split ? 128 : 360));
					assert(l.y + l.height == (split ? 224 : 448));
					assert(l.icon == (!split && loading_tips[k].item != 0));
					int room = l.width - 32 - (l.icon ? 56 : 0);
					for(unsigned line = 0; line < l.lines; line++) {
						unsigned words = 0;
						assert(l.line_width[line] <= room);
						for(unsigned n = 0; n < l.count; n++) words += l.tokens[n].line == line;
						assert(words >= 2); /* no widow, including last line */
					}
					assert(loading_tips_word_count(loading_tips[k].text) <= 22);
					cases++;
				}
			}
	/* Unbound glyph tokens cannot enter a viewport's pool. */
	struct loading_tip_context unbound = context;
	unbound.glyph[LOADING_TIP_INVENTORY] = -1;
	struct loading_tip_layout l;
	assert(!loading_tips_layout(&loading_tips[53], &unbound, &l));
	printf("PASS fit: %u cases, actual font PNG metrics, no overflow/widows at 640/854 and panes\n",cases);
}

int main(int argc, char** argv) {
	assert(argc == 2);
	load_metrics(argv[1]);
	test_dwell();
	test_gates_and_headings();
	test_beginner_and_nether();
	test_history();
	test_contexts();
	test_hidden_and_owners();
	test_fit();
	return 0;
}
