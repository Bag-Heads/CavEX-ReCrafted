/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
/* Approved FINAL-DESIGN/FINAL-NUMBERS, 2026-10-05. Real module queries and
 * screen navigation; no fabricated progress or replacement unlock rules. */
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define test_mkdir(d) mkdir(d)
#else
#include <sys/stat.h>
#define test_mkdir(d) mkdir(d, 0755)
#endif

#include "game/game_state.h"
#include "support/test_fixture_dir.h"
/* The dormant seam explicitly supports the engine-owned actor identity. */
#define CW_HAVE_ACTOR_IDENT
#include "block/blocks.h"
#include "game/achievements/achievements_api.h"
#include "game/gui/screen.h"
#include "game/gui/screen_achievements.h"
#include "graphics/gui_util.h"
#include "graphics/render_item.h"
#include "item/items.h"
#include "light_brightness.h"
#include "lodepng/lodepng.h"
#include "platform/gfx.h"
#include "platform/input_glyph.h"
#include "platform/menu_sound.h"

/* Only the graphics boundary is intercepted in headless checks. The same
 * wrappers pass through to real OpenGL in the native capture mode. */
static bool native_capture;
static int draw_min_y, draw_max_y, draw_calls, text_calls, browse_calls,
	back_calls;
static int drawn_requirement, drawn_back;
static int unknown_count_calls, zero_count_calls, retry_calls;
static int menu_cues;
static int approved_padlock_draws, corner_lock_draws, approved_padlock_borders;
static enum menu_sound_event last_cue;
void __real_audio_menu_event(enum menu_sound_event, ptime_t);
void __wrap_audio_menu_event(enum menu_sound_event cue, ptime_t now) {
	menu_cues++;
	last_cue = cue;
	if(native_capture)
		__real_audio_menu_event(cue, now);
}
void __real_gfx_texture(bool);
void __wrap_gfx_texture(bool enabled) {
	if(native_capture)
		__real_gfx_texture(enabled);
}
void __real_gfx_bind_texture(struct tex_gfx*);
void __wrap_gfx_bind_texture(struct tex_gfx* tex) {
	if(native_capture)
		__real_gfx_bind_texture(tex);
}
void __real_gfx_draw_quads(size_t, const int16_t*, const uint8_t*,
						   const uint16_t*);
void __wrap_gfx_draw_quads(size_t n, const int16_t* v, const uint8_t* c,
						   const uint16_t* uv) {
	if(native_capture) {
		__real_gfx_draw_quads(n, v, c, uv);
		return;
	}
	draw_calls++;
	if(c[0] == 200 && c[1] == 201 && c[2] == 184)
		approved_padlock_draws++;
	if(c[0] == 32 && c[1] == 32 && c[2] == 32 && v[3] - v[0] == 16
	   && v[7] - v[1] == 16)
		corner_lock_draws++;
	for(size_t i = 0; i < n; i++) {
		if(v[i * 3 + 1] < draw_min_y)
			draw_min_y = v[i * 3 + 1];
		if(v[i * 3 + 1] > draw_max_y)
			draw_max_y = v[i * 3 + 1];
	}
}
void __real_gutil_draw_item_sized(struct item_data*, int, int, int, int);
void __real_gfx_draw_quads_flt(size_t, const float*, const uint8_t*,
							   const float*);
void __wrap_gfx_draw_quads_flt(size_t n, const float* v, const uint8_t* c,
							   const float* uv) {
	if(native_capture) {
		__real_gfx_draw_quads_flt(n, v, c, uv);
		return;
	}
	if(c[0] == 200 && c[1] == 201 && c[2] == 184)
		approved_padlock_draws++;
	if(c[0] == 200 && c[1] == 201 && c[2] == 184 && v[3] - v[0] == 19.5F
	   && v[7] - v[1] == 2.25F)
		approved_padlock_borders++;
	for(size_t i = 0; i < n; i++)
		assert(v[i * 3 + 1] >= draw_min_y && v[i * 3 + 1] <= draw_max_y);
}
void __wrap_gutil_draw_item_sized(struct item_data* item, int x, int y,
								  int size, int layer) {
	if(native_capture)
		__real_gutil_draw_item_sized(item, x, y, size, layer);
}
void __real_gutil_text_rgb(int, int, const char*, int, bool, uint8_t, uint8_t,
						   uint8_t);
void __wrap_gutil_text_rgb(int x, int y, const char* s, int size, bool shadow,
						   uint8_t r, uint8_t g, uint8_t b) {
	if(native_capture) {
		__real_gutil_text_rgb(x, y, s, size, shadow, r, g, b);
		return;
	}
	assert(size >= 16);
	text_calls++;
	if(!strcmp(s, "Browse"))
		browse_calls++;
	if(!strcmp(s, "Back"))
		back_calls++;
	if(!strcmp(s, "/16"))
		unknown_count_calls++;
	if(!strcmp(s, "0/16"))
		zero_count_calls++;
	if(!strcmp(s, "Retry"))
		retry_calls++;
	assert(y >= draw_min_y && y + size <= draw_max_y);
}
int __real_gutil_prompt_glyph(int, int, int, int);
int __wrap_gutil_prompt_glyph(int x, int y, int size, int glyph) {
	if(native_capture)
		return __real_gutil_prompt_glyph(x, y, size, glyph);
	if(y == 430 || y == 190 || y == 414) {
		if(x == 351 || x == 361 || x == 254)
			drawn_requirement = glyph;
		if(x == 682 || x == 691 || x == 503 || x == 512)
			drawn_back = glyph;
	}
	return glyph < 0 ? 0 : size;
}

static void rect(struct menu_rect r, int x, int y, int w, int h) {
	assert(r.x == x && r.y == y && r.w == w && r.h == h);
}

static void layout(void) {
	for(int wide = 0; wide < 2; wide++) {
		int w = wide ? 854 : 640, px = wide ? 67 : 32;
		struct achievements_layout l;
		screen_achievements_layout(&l, 0, 0, w, 480);
		rect(l.panel, px, 24, wide ? 720 : 576, 432);
		rect(l.header, px + 16, 40, wide ? 688 : 544, 28);
		rect(l.chips[0], px + 16, 78, 32, 32);
		rect(l.chips[1], wide ? 739 : 560, 78, 32, 32);
		for(int i = 0; i < 4; i++)
			rect(l.tabs[i], (wide ? 123 : 88) + i * (wide ? 154 : 118), 76,
				 wide ? 146 : 110, 36);
		rect(l.list_header, px + 16, 120, wide ? 340 : 300, 16);
		for(int i = 0; i < 6; i++)
			rect(l.rows[i], px + 16, 144 + i * 44, wide ? 340 : 300, 40);
		rect(l.detail, wide ? 439 : 364, 144, wide ? 332 : 228, 260);
		rect(l.requires, wide ? 439 : 364, 236, wide ? 332 : 228, 60);
		rect(l.legend, px + 16, 404, wide ? 688 : 544, 44);
		for(int owner = 0; owner < 2; owner++) {
			screen_achievements_layout(&l, 0, owner * 240, w, 240);
			rect(l.panel, px, owner * 240 + 12, wide ? 720 : 576, 216);
			rect(l.header, px + 16, owner * 240 + 12, wide ? 688 : 544, 24);
			for(int i = 0; i < 7; i++)
				rect(l.tiles[i], (wide ? 223 : 136) + i * (wide ? 60 : 54),
					 owner * 240 + 44, wide ? 48 : 44, wide ? 48 : 44);
			rect(l.arrows[0], wide ? 171 : 84, owner * 240 + (wide ? 46 : 44),
				 44, 44);
			rect(l.arrows[1], wide ? 639 : 512, owner * 240 + (wide ? 46 : 44),
				 44, 44);
			rect(l.legend, px + 16, owner * 240 + 180, wide ? 688 : 544, 44);
		}
	}
}

static enum achievements_action press(struct achievements_board* b, int button,
									  bool split) {
	struct input_player_frame f
		= {.connected = true, .category = INPUT_CAT_GAMECUBE};
	if(button >= 0)
		f.pressed[button] = true;
	return screen_achievements_update_owned(b, &f, 0, split ? 240 : 0, 854,
											split ? 240 : 480);
}

static void catalog(void) {
	const int expected[4][6]
		= {{0, 1, 2}, {3, 4, 5, 9, 10, 11}, {6, 7, 8}, {12, 13, 14, 15}};
	const int count[] = {3, 6, 3, 4};
	const char* names[] = {"Basics", "Mining", "Farming", "Combat"};
	for(int t = 0; t < 4; t++) {
		assert(!strcmp(screen_achievements_tab_name(t), names[t]));
		assert(screen_achievements_tab_count(t) == count[t]);
		for(int row = 0; row < count[t]; row++) {
			int id = screen_achievements_tab_id(t, row);
			assert(id == expected[t][row]);
			assert(screen_achievements_tab_of(id) == t);
		}
		assert(screen_achievements_tab_id(t, count[t]) == -1);
	}
	assert(!strcmp(screen_achievements_name(0), "Taking Inventory"));
	assert(!strcmp(screen_achievements_name(15), "When Pigs Fly"));
}

static void award(int id) {
	struct game_event ev = {.kind = GE_REMOTE_STAT,
							.authority = GE_AUTH_PACKET200,
							.who = {.slot = 0, .generation = 17}};
	ev.u.remote.stat_id = CW_ACH_BASE + id;
	ev.u.remote.amount = 1;
	if(id == 0) {
		ev.kind = GE_INVENTORY_OPENED;
		ev.authority = GE_AUTH_CLIENT;
	}
	game_event_emit(&ev);
	achievements_tick();
}

static void module_and_navigation(void) {
	struct achievements_board b = {0}, other = {0};
	struct achievements_data d;
	screen_achievements_open(&b, 0);
	assert(b.open && b.selected == 0 && b.depth == 0);
	screen_achievements_read(0, &d);
	assert(d.state == ACHIEVEMENTS_UNAVAILABLE && d.earned == -1);
	draw_min_y = 10000;
	draw_max_y = -1;
	browse_calls = unknown_count_calls = zero_count_calls = retry_calls = 0;
	screen_achievements_render_owned(&b, NULL, 0, 0, 640, 480);
	/* No owner frame: the A1 hint row draws nothing (a label naming no
	 * button would be a lie, 10-31), so no Retry/Browse label either. */
	assert(unknown_count_calls == 1 && zero_count_calls == 0 && retry_calls == 0
		   && browse_calls == 0 && screen_achievements_test_row()->count == 0);
	char dir[256];
	test_fixture_dir("test_achievements_screen_tmp", dir, sizeof dir);
	assert(test_mkdir(dir) == 0);
	assert(achievements_init(dir));
	assert(achievements_bind(0, 17, "screen0", ACH_MODE_REMOTE_SERVER)
		   == ACH_BIND_OK);
	assert(achievements_bind(1, 18, "screen1", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	screen_achievements_read(0, &d);
	assert(d.state == ACHIEVEMENTS_READY && d.earned == 0);
	assert(screen_achievements_state(&d, 0) == ACHIEVEMENT_AVAILABLE);
	assert(screen_achievements_state(&d, 1) == ACHIEVEMENT_LOCKED);
	award(0);
	award(1);
	award(2);
	screen_achievements_read(0, &d);
	assert(d.earned == 3
		   && screen_achievements_state(&d, 0) == ACHIEVEMENT_EARNED);
	assert(screen_achievements_state(&d, 3) == ACHIEVEMENT_AVAILABLE);
	assert(screen_achievements_state(&d, 4) == ACHIEVEMENT_LOCKED);
	assert(d.rows[11].special && d.rows[15].special);
	screen_achievements_open(&other, 1);
	struct achievements_board untouched = other;
	press(&b, IB_GUI_PAGE, false);
	assert(b.selected == 3 && !b.detail && last_cue == MENU_SOUND_SECTION);
	press(&b, IB_GUI_DOWN, false);
	assert(b.selected == 4);
	press(&b, IB_GUI_RIGHT, false);
	assert(b.detail && b.selected == 4);
	press(&b, IB_GUI_CLICK, false);
	assert(b.selected == 3 && b.depth == 1);
	press(&b, IB_GUI_CLICK, false);
	assert(b.selected == 2 && b.depth == 2
		   && screen_achievements_tab_of(b.selected) == 0);
	assert(press(&b, IB_INVENTORY, false) == ACHIEVEMENTS_NONE
		   && b.selected == 3);
	assert(press(&b, IB_INVENTORY, false) == ACHIEVEMENTS_NONE
		   && b.selected == 4 && b.detail);
	assert(press(&b, IB_INVENTORY, false) == ACHIEVEMENTS_BACK && !b.open);
	screen_achievements_open(&b, 0);
	assert(b.selected == 4);
	assert(press(&b, IB_HOME, false) == ACHIEVEMENTS_RESUME);
	screen_achievements_open(&b, 0);
	b.selected = 0;
	int before = menu_cues;
	press(&b, IB_GUI_LEFT, true);
	assert(b.selected == 0
		   && menu_cues == before); /* silent endpoint; no wrapping */
	for(int i = 0; i < 15; i++)
		press(&b, IB_GUI_RIGHT, true);
	assert(b.selected == 15);
	before = menu_cues;
	press(&b, IB_GUI_RIGHT, true);
	assert(b.selected == 15 && menu_cues == before);
	assert(screen_achievements_ribbon_first(0) == 0);
	assert(screen_achievements_ribbon_first(8) == 5);
	assert(screen_achievements_ribbon_first(15) == 9);
	assert(!memcmp(&other, &untouched, sizeof other));
	screen_achievements_read(1, &d);
	assert(d.earned == 0); /* selecting/jumping NEVER awards */
	achievements_unbind(1, 18);
	screen_achievements_read(1, &d);
	assert(d.state == ACHIEVEMENTS_UNAVAILABLE && d.earned == -1);
	achievements_shutdown();
	assert(!achievements_init(NULL));
	screen_achievements_read(0, &d);
	assert(d.state == ACHIEVEMENTS_ERROR && d.earned == -1 && d.message[0]);
	assert(press(&b, IB_GUI_CLICK, true)
		   == ACHIEVEMENTS_NONE); /* Retry rereads; no lifecycle side effects */
	assert(press(&b, IB_INVENTORY, true) == ACHIEVEMENTS_BACK);
	achievements_shutdown();
	test_fixture_dir_cleanup(dir);
}

static void controllers_and_ir(void) {
	char dir[256];
	test_fixture_dir("test_achievements_controller_tmp", dir, sizeof dir);
	assert(test_mkdir(dir) == 0 && achievements_init(dir));
	assert(achievements_bind(0, 17, "controller", ACH_MODE_REMOTE_SERVER)
		   == ACH_BIND_OK);
	for(int family = INPUT_CAT_NUNCHUK; family <= INPUT_CAT_GAMECUBE;
		family++) {
		struct achievements_board b = {0};
		screen_achievements_open(&b, 0);
		struct input_player_frame f = {.connected = true, .category = family};
		for(int i = 0; i < IB_COUNT; i++)
			f.glyphs[i] = -1;
		int a = family == INPUT_CAT_NUNCHUK		   ? 4 :
			family == INPUT_CAT_CLASSIC_CONTROLLER ? 204 :
													 404;
		int back = family == INPUT_CAT_NUNCHUK	   ? 6 :
			family == INPUT_CAT_CLASSIC_CONTROLLER ? 206 :
													 407;
		f.glyphs[IB_GUI_CLICK] = input_glyph_key(family, a);
		f.glyphs[IB_INVENTORY] = input_glyph_key(family, back);
		/* Browse = the D-pad cell (the hint row names bound roles only). */
		f.glyphs[IB_GUI_UP] = input_glyph_key(family,
			family == INPUT_CAT_NUNCHUK ? 0 :
			family == INPUT_CAT_CLASSIC_CONTROLLER ? 200 : 400);
		f.glyphs[IB_GUI_PREVIOUS]
			= input_glyph_key(family,
							  family == INPUT_CAT_NUNCHUK				 ? 9 :
								  family == INPUT_CAT_CLASSIC_CONTROLLER ? 210 :
																		   409);
		f.glyphs[IB_GUI_PAGE]
			= input_glyph_key(family,
							  family == INPUT_CAT_NUNCHUK				 ? 8 :
								  family == INPUT_CAT_CLASSIC_CONTROLLER ? 211 :
																		   410);
		struct achievements_glyphs glyphs = screen_achievements_glyphs(&f);
		assert(glyphs.requirement
			   == (family == INPUT_CAT_NUNCHUK				  ? 0 :
					   family == INPUT_CAT_CLASSIC_CONTROLLER ? 10 :
																32));
		assert(glyphs.back
			   == (family == INPUT_CAT_NUNCHUK				  ? 2 :
					   family == INPUT_CAT_CLASSIC_CONTROLLER ? 12 :
																35));
		assert(glyphs.previous
			   == (family == INPUT_CAT_NUNCHUK				  ? 6 :
					   family == INPUT_CAT_CLASSIC_CONTROLLER ? 22 :
																37));
		assert(glyphs.next
			   == (family == INPUT_CAT_NUNCHUK				  ? 5 :
					   family == INPUT_CAT_CLASSIC_CONTROLLER ? 23 :
																38));
		draw_min_y = 10000;
		draw_max_y = -1;
		draw_calls = text_calls = browse_calls = back_calls = 0;
		drawn_requirement = drawn_back = -1;
		screen_achievements_render_owned(&b, &f, 0, 240, 854, 240);
		assert(approved_padlock_draws > 0 && approved_padlock_borders > 0
			   && corner_lock_draws == 0);
		assert(draw_calls > 0 && text_calls > 0 && draw_min_y == 240
			   && draw_max_y == 480);
		/* The centred A1 hint row in the pane (10-31): Browse /
		 * Requirement / Back from the owner's bound glyphs, inside the
		 * 240-480 view, its strip at the split 196-224 band. */
		{
			const struct gutil_hint_row* row = screen_achievements_test_row();
			assert(row->count == 3 && row->strip.y == 240 + 196
				   && row->strip.y + row->strip.height == 240 + 224);
			assert(!strcmp(row->cells[0].label, "Browse")
				   && row->cells[0].glyph[0] == glyphs.browse);
			assert(!strcmp(row->cells[1].label, "Requirement")
				   && row->cells[1].glyph[0] == glyphs.requirement);
			assert(!strcmp(row->cells[2].label, "Back")
				   && row->cells[2].glyph[0] == glyphs.back);
			(void)drawn_requirement; (void)drawn_back;
		}
		f.pressed[IB_GUI_PAGE] = true;
		screen_achievements_update_owned(&b, &f, 0, 0, 854, 480);
		assert(b.selected == 3);
		memset(f.pressed, 0, sizeof f.pressed);
		if(family == INPUT_CAT_NUNCHUK) {
			f.pointer_valid = true;
			f.pointer_x = 100;
			f.pointer_y = 150;
			screen_achievements_update_owned(&b, &f, 0, 0, 854, 480);
			f.pressed[IB_GUI_DOWN] = true;
			screen_achievements_update_owned(&b, &f, 0, 0, 854, 480);
			assert(b.selected == 4);
			memset(f.pressed, 0, sizeof f.pressed);
			screen_achievements_update_owned(&b, &f, 0, 0, 854, 480);
			assert(b.selected == 4); /* stationary IR cannot steal focus */
			f.pointer_x = 120;
			screen_achievements_update_owned(&b, &f, 0, 0, 854, 480);
			assert(b.selected == 3);
			b.selected = 5;
			screen_achievements_open(&b, 0);
			screen_achievements_update_owned(&b, &f, 0, 0, 854, 480);
			assert(b.selected == 5
				   && b.depth
					   == 0); /* first stationary report is not hover intent */
			f.pressed[IB_GUI_CLICK] = true;
			screen_achievements_update_owned(&b, &f, 0, 0, 854, 480);
			assert(b.selected == 3
				   && b.depth == 0); /* first click focuses its row, never the
										old row's parent */
			f.pressed[IB_GUI_CLICK] = false;
		}
		f.pressed[IB_INVENTORY] = true;
		assert(screen_achievements_update_owned(&b, &f, 0, 0, 854, 480)
			   == ACHIEVEMENTS_BACK);
	}
	achievements_shutdown();
	test_fixture_dir_cleanup(dir);
}

static void capture_frame(const char* name, const struct achievements_board* b,
						  const struct input_player_frame* f, bool split) {
	int view_y = split && b->owner == 1 ? 240 : 0, other_y = view_y ? 0 : 240;
	gfx_write_buffers(true, true, true);
	gfx_clear_buffers(80, 110, 128);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	gfx_matrix_texture(false, NULL);
	gfx_mode_gui();
	if(split) {
		/* Deliberately different lower pane; byte comparison below proves that
		 * the real board renderer did not touch it. This is a native-render
		 * fixture, not evidence of an admitted second player or a live gameplay
		 * session. */
		gfx_texture(false);
		gutil_texquad_col(0, other_y, 0, 0, 0, 0, 854, 240, 30, 65, 120, 255);
		gfx_texture(true);
		gutil_text(24, other_y + 24,
				   view_y ? "Player 1: untouched native capture fixture" :
							"Player 2: untouched native capture fixture",
				   16, true);
	}
	uint8_t *before = malloc(854 * 480 * 4), *after = malloc(854 * 480 * 4);
	assert(before && after);
	size_t w, h;
	gfx_copy_framebuffer(before, &w, &h);
	assert(w == 854 && h == 480);
	/* The app already scissors each view before painting menus. Exercise that
	 * same boundary, including an IR cursor which straddles the pane edge. */
	gfx_scissor(true, 0, view_y, 854, split ? 240 : 480);
	screen_achievements_render_owned(b, f, 0, view_y, 854, split ? 240 : 480);
	gfx_scissor(false, 0, 0, 0, 0);
	gfx_copy_framebuffer(after, &w, &h);
	if(split)
		assert(!memcmp(before + 854 * other_y * 4, after + 854 * other_y * 4,
					   854 * 240 * 4));
	assert(lodepng_encode32_file(name, after, (unsigned)w, (unsigned)h) == 0);
	free(before);
	free(after);
}
static void native_icon_equivalence(void) {
	uint8_t *normal = malloc(854 * 480 * 4), *gray = malloc(854 * 480 * 4);
	assert(normal && gray);
	struct achievements_data d;
	screen_achievements_read(0, &d);
	assert(d.state == ACHIEVEMENTS_READY);
	for(int id = 0; id < 16; id++)
		for(int size = 16; size <= 32; size += 8) {
			for(int locked = 0; locked < 2; locked++) {
				gfx_write_buffers(true, true, true);
				gfx_clear_buffers(211, 17, 203);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
				gfx_matrix_texture(false, NULL);
				gfx_mode_gui();
				gfx_texture(true);
				screen_achievements_test_icon(d.rows[id].icon_item, locked, 64,
											  80, size);
				size_t w, h;
				gfx_copy_framebuffer(locked ? gray : normal, &w, &h);
				assert(w == 854 && h == 480);
			}
			int mismatch = 0, pixels = 0;
			for(int i = 0; i < 854 * 480; i++) {
				bool n = normal[i * 4] != 211 || normal[i * 4 + 1] != 17
					|| normal[i * 4 + 2] != 203;
				bool g = gray[i * 4] != 211 || gray[i * 4 + 1] != 17
					|| gray[i * 4 + 2] != 203;
				mismatch += n != g;
				pixels += n;
			}
			if(mismatch)
				fprintf(stderr,
						"native icon silhouette mismatch id=%d item=%u size=%d "
						"pixels=%d\n",
						id, d.rows[id].icon_item, size, mismatch);
			assert(pixels > 0 && mismatch == 0);
		}
	free(normal);
	free(gray);
	puts("PASS native grayscale silhouettes: all 16 real catalog icons at "
		 "16/24/32 match the normal inventory renderer");
}
static int captures(const char* pack, const char* out) {
	native_capture = true;
	gstate.config_user.root = json_value_init_object();
	assert(json_object_dotset_string(
			   json_value_get_object(gstate.config_user.root),
			   "paths.texturepack", pack)
		   == JSONSuccess);
	assert(gfx_setup());
	glfwSetWindowSize(glfwGetCurrentContext(), 854, 480);
	glfwPollEvents();
	assert(tex_init());
	blocks_init();
	items_init();
	render_item_init();
	gfx_update_light(1.0F, 0.0F, 0.0F, light_lookup_overworld);
	char dir[256];
	test_fixture_dir("test_achievements_capture_tmp", dir, sizeof dir);
	assert(test_mkdir(dir) == 0);
	assert(achievements_init(dir));
	assert(achievements_bind(0, 17, "capture", ACH_MODE_REMOTE_SERVER)
		   == ACH_BIND_OK);
	assert(achievements_bind(1, 18, "capture2", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	native_icon_equivalence();
	award(0);
	award(1);
	award(2);
	award(3);
	award(4);
	award(9);
	struct achievements_board b = {0};
	screen_achievements_open(&b, 0);
	struct input_player_frame f
		= {.connected = true, .category = INPUT_CAT_NUNCHUK};
	for(int i = 0; i < IB_COUNT; i++)
		f.glyphs[i] = -1;
	f.glyphs[IB_GUI_UP] = input_glyph_key(INPUT_CAT_NUNCHUK, 900);
	f.glyphs[IB_GUI_CLICK] = input_glyph_key(INPUT_CAT_NUNCHUK, 4);
	f.glyphs[IB_INVENTORY] = input_glyph_key(INPUT_CAT_NUNCHUK, 6);
	f.glyphs[IB_GUI_PREVIOUS] = input_glyph_key(INPUT_CAT_NUNCHUK, 9);
	f.glyphs[IB_GUI_PAGE] = input_glyph_key(INPUT_CAT_NUNCHUK, 8);
	char file[1024];
	b.selected = 3;
	snprintf(file, sizeof file, "%s/native-full-854.png", out);
	capture_frame(file, &b, &f, false);
	b.selected = 8;
	snprintf(file, sizeof file, "%s/native-split-854.png", out);
	capture_frame(file, &b, &f, true);
	b.pointer_available = true;
	b.pointer_x = 80;
	b.pointer_y = 239;
	b.pointer_angle = 45;
	snprintf(file, sizeof file, "%s/native-split-edge-pointer-854.png", out);
	capture_frame(file, &b, &f, true);
	b.pointer_available = false;
	struct achievements_board second = {0};
	screen_achievements_open(&second, 1);
	second.selected = 8;
	snprintf(file, sizeof file, "%s/native-split-owner2-854.png", out);
	capture_frame(file, &second, &f, true);
	achievements_unbind(0, 17);
	snprintf(file, sizeof file, "%s/native-unavailable-854.png", out);
	capture_frame(file, &b, &f, false);
	achievements_shutdown();
	test_fixture_dir_cleanup(dir);
	puts("PASS native OpenGL captures: real API fixtures P1 6/16 and P2 0/16; "
		 "other 854x240 RGBA pane untouched for each opener and edge cursor; "
		 "unavailable honest");
	return 0;
}

/* Drive the live split parent, with main's absolute input pane and local GUI
 * projection. The board is real; only graphics/audio boundaries are wrapped. */
static void split_pause_entry(void) {
	/* Recovery can expand this owner-local menu to a full-height pane. */
	struct menu_composition expanded;
	local_pause_compose(&expanded, 0, 0, 640, 480, 0, 4);
	struct menu_rect status = menu_pause_status(&expanded, 4);
	struct menu_rect status_copy = {status.x, status.y, status.w, 40};
	assert(expanded.window.y + expanded.window.h >= status_copy.y + status_copy.h);
	char dir[256];
	test_fixture_dir("test_achievements_split_pause_tmp", dir, sizeof dir);
	assert(test_mkdir(dir) == 0 && achievements_init(dir));
	assert(achievements_bind(0, 17, "split0", ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	assert(achievements_bind(1, 18, "split1", ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	for(unsigned owner = 0; owner < 2; owner++) {
		struct client_session* s = &gstate.players[owner];
		local_pause_reset(&s->pause);
		struct achievements_board* b = screen_achievements_owner(owner);
		*b = (struct achievements_board){0};
		assert(local_pause_open(&s->pause, owner == 0));
		for(int width = 640; width <= 854; width += 214) {
			struct menu_composition c;
			local_pause_compose(&c, 0, owner * 240, width, 240, 80, 4);
			assert(c.action_count == 4 && c.notice_in_header);
			const struct menu_rect expected = {
				width == 640 ? 326 : 433, owner * 240 + 68,
				width == 640 ? 266 : 338, 44};
			assert(!memcmp(&c.actions[1], &expected, sizeof expected));
			for(int k = 0; k < 4; k++)
				assert(menu_rect_contains(&c.view, &c.actions[k]));
		}
		struct input_player_frame f = {.connected = true,
			.category = INPUT_CAT_GAMECUBE};
		f.pressed[IB_GUI_RIGHT] = true;
		local_pause_update(&s->pause, s, &f, 0, owner * 240, 854, 240);
		f.pressed[IB_GUI_RIGHT] = false;
		f.pressed[IB_GUI_CLICK] = true;
		assert(local_pause_update(&s->pause, s, &f, 0, owner * 240, 854, 240)
			   == LOCAL_PAUSE_NONE);
		assert(b->open && b->owner == owner && b->selected == 0);
		assert(s->pause.open && !s->pause.exit_pending);
		f.pressed[IB_GUI_CLICK] = false;
		f.pressed[IB_GUI_RIGHT] = true;
		local_pause_update(&s->pause, s, &f, 0, owner * 240, 854, 240);
		assert(b->selected == 1);
		f.pressed[IB_GUI_RIGHT] = false;
		f.pointer_valid = true;
		f.pointer_x = 400;
		f.pointer_y = owner * 240 + 60;
		struct input_player_frame original = f;
		local_pause_update(&s->pause, s, &f, 0, owner * 240, 854, 240);
		assert(b->pointer_available && b->pointer_x == 400 && b->pointer_y == 60);
		assert(!memcmp(&f, &original, sizeof f));
		f.pointer_valid = false;
		local_pause_update(&s->pause, s, &f, 0, owner * 240, 854, 240);
		draw_min_y = 10000; draw_max_y = -1;
		local_pause_render(&s->pause, owner, &f, 0, 0, 854, 240);
		assert(draw_min_y == 0 && draw_max_y == 240);
		f.pressed[IB_INVENTORY] = true;
		assert(local_pause_update(&s->pause, s, &f, 0, owner * 240, 854, 240)
			   == LOCAL_PAUSE_NONE);
		assert(s->pause.page == LOCAL_PAUSE_MENU && s->pause.selected == 1
			   && s->pause.open && !b->open);
		f.pressed[IB_INVENTORY] = false;
		f.pressed[IB_GUI_CLICK] = true;
		local_pause_update(&s->pause, s, &f, 0, owner * 240, 854, 240);
		assert(b->open && b->selected == 1);
		f.pressed[IB_GUI_CLICK] = false;
		f.pressed[IB_HOME] = true;
		assert(local_pause_update(&s->pause, s, &f, 0, owner * 240, 854, 240)
			   == LOCAL_PAUSE_BACK);
		assert(!s->pause.open && !b->open && !s->pause.exit_pending);
	}
	achievements_shutdown();
	test_fixture_dir_cleanup(dir);
	puts("PASS real split Pause -> Achievements: both owners, local projection, Back cell 1, remembered selection, Home resume");
}

static void pause_entry(void) {
	int id;
	const char* label;
	for(int wide = 0; wide < 2; wide++) {
		int width = wide ? 854 : 640, pw = wide ? 720 : 576;
		struct menu_composition c;
		screen_pause_test_composition(&c, width, 480);
		assert(c.action_count == 5);
		for(int i = 0; i < 5; i++)
			rect(c.actions[i], (width - 320) / 2, 104 + i * 46, 320, 44);
		screen_pause_test_composition(&c, width, 240);
		rect(c.window, (width - pw) / 2, 12, pw, 216);
		assert(c.action_count == 4);
		for(int i = 0; i < 4; i++)
			rect(c.actions[i],
				 (width - pw) / 2 + 16 + (i % 2) * ((pw - 44) / 2 + 12),
				 68 + (i / 2) * 52, (pw - 44) / 2, 44);
	}
	assert(screen_pause_test_button_count(true) == 5);
	assert(screen_pause_test_button_count(false) == 4);
	assert(screen_pause_test_button(1, true, true, &id, &label));
	assert(id == 5 && !strcmp(label, "Achievements"));
	assert(screen_pause_test_button(2, false, true, &id, &label));
	assert(id == 0 && !strcmp(label, "Options..."));
	assert(screen_pause_test_button(1, false, true, &id, &label));
	assert(id == 5 && !strcmp(label, "Achievements"));
	screen_set(&screen_pause);
	screen_pause_test_activate_row(1);
	assert(gstate.players[0].current_screen == &screen_achievements);
	screen_achievements_return_pause();
	assert(gstate.players[0].current_screen == &screen_pause
		   && screen_pause_achievements_focused());
}

int main(int argc, char** argv) {
	if(argc == 4 && !strcmp(argv[1], "--capture"))
		return captures(argv[2], argv[3]);
	layout();
	catalog();
	module_and_navigation();
	controllers_and_ir();
	split_pause_entry();
	pause_entry();
	puts("PASS approved achievements: exact layouts, branches, real owner "
		 "data, states, back stack, controller glyphs, deliberate IR, owner "
		 "isolation, pause entry/focus");
	return 0;
}
