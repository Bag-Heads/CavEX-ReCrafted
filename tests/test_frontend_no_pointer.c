/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * THE FRONT-END MENUS WITH NO POINTER (push-0921/A1, Astra "Lane A" 1-5).
 *
 * Every case below drives the REAL update() handler of a real pre-world
 * screen -- screen_select_world, screen_texturepacks, screen_multiplayer_setup,
 * screen_multiplayer_connect, screen_title, screen_confirm,
 * screen_creative_picker -- through the REAL PLATFORM_WII input.c, on the
 * libogc driver doubles in tests/support/wii_input. Nothing writes an IB_*
 * level by hand: a case names a PHYSICAL BUTTON, the report goes through
 * WPAD_ReadPending/PAD_ScanPads, and the shipped config_wii.json binding
 * table plus input_owner_accepts' per-family code filter decide which role
 * that press becomes. That is the only rig in which "one physical press must
 * cause exactly one intended action" is a real statement, because the answer
 * differs PER FAMILY.
 *
 * What is proven, in brief order:
 *   a. Close (IB_INVENTORY) closes Select World and Texture Packs to their
 *      parent; the title has no parent Back and the loading screen keeps its
 *      HELD cancel (Astra A-2).
 *   b. The three lists page on IB_GUI_PREVIOUS / IB_GUI_PAGE -- the canonical
 *      menu roles -- not on the hotbar roles IB_SCROLL_LEFT/RIGHT, at the
 *      first-page / last-page / empty / single-page boundaries, with the
 *      selected row preserved.
 *   c. With no pointer, direction + activate reaches every ENABLED action:
 *      Select World Create/Rename/Delete/Select, Multiplayer
 *      Add/Edit/Delete/Direct Connect/Refresh/name edit/Quick Connect, and
 *      the connection-error Retry. Disabled actions are skipped, never
 *      activated; Delete still goes through the confirm screen.
 *   d. IB_HOME keeps exactly today's behaviour everywhere (Astra A-3 / A1).
 *   e. The hotbar-role ALIAS collision test, per family.
 *   f. Item 13: a STATIONARY pointer does not overwrite directional focus,
 *      a deliberate MOVE takes it back; modal isolation and neutral re-arm.
 *
 * Each case runs for all three controller families' DEFAULT (shipped) table
 * and once more for a REMAPPED table, because a role read that is right for
 * one family's codes can be wrong for another's -- which is the whole of
 * Astra A-3.
 */

#include "support/frontend_screen_rig.h"

#include "graphics/gfx_settings.h"
/* The retained Play session the approved title's Play row resets on a FRESH
 * entry (GATE3-PLAY-PLAN.md item 5; VISUAL-COORDINATION.md section 11 links
 * the real module into this target). Real state, no double. */
#include "game/gui/play_session.h"
/* The pack screen's own drawing primitives, for the helper-level cases: the
 * same header screen_texturepacks.c draws through, not a copy of it.
 *
 * THE BACKDROP UV RECORDER (design-wiring/uv-record). fep_backdrop's whole
 * point is that a non-256 texture must reach gfx_draw_quads_flt with genuinely
 * NORMALIZED 0..1 source UVs -- pass its PIXEL rect instead and the fixed-1/256
 * grid drives u,v past 1.0 and the clamp smears the edge into stripes. The
 * shared rig's gfx_draw_quads_flt is a noop, so those numbers were unobserved.
 *
 * So, for THIS translation unit only, the call inside the header is redirected
 * to a local recorder: the macro is armed immediately before the include and
 * retired immediately after it, which is why nothing else in this file (or in
 * the rig, which is already included above and keeps the real noop) changes.
 * Nothing here re-implements the crop -- the arithmetic under test is the
 * header's own, executed; the recorder only keeps what it emitted. The real
 * screen_texturepacks.c compiles against the untouched header. */
static void fe_bd_record(size_t count, const float* v, const uint8_t* color,
						 const float* uv);
#define gfx_draw_quads_flt fe_bd_record
#include "game/gui/frontend_presentation.h"
#undef gfx_draw_quads_flt

#include <ctype.h>
#include <errno.h>
#include <math.h> /* fabsf: the recorded UVs are floats, compared to tolerance */
#include <stdint.h> /* SIZE_MAX: play_session's "no server index hint" */
#include <stdlib.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define fe_mkdir(p) _mkdir(p)
#define fe_rmdir(p) _rmdir(p)
#define fe_chdir(p) _chdir(p)
#else
#include <unistd.h>
#define fe_mkdir(p) mkdir(p, 0777)
#define fe_rmdir(p) rmdir(p)
#define fe_chdir(p) chdir(p)
#endif

/* ---- binding lookup: the config is the source of truth ------------------ */

/* The physical code THIS family is bound to for `input.<key>`, read from the
 * live config the game reads. Deliberately not a literal table: if
 * config_wii.json's menu bindings drift, the cases below start driving the
 * wrong button and fail by name instead of quietly testing nothing. The
 * ranges are input_owner_accepts' own (input_owner.h). */
static int fe_code_for(enum fe_family fam, const char* key) {
	char dotted[64];
	snprintf(dotted, sizeof(dotted), "input.%s", key);
	int codes[16];
	size_t length = sizeof(codes) / sizeof(codes[0]);
	if(!config_read_int_array(&gstate.config_user, dotted, codes, &length))
		return -1;
	for(size_t k = 0; k < length; k++) {
		int c = codes[k];
		if(fam == FE_REMOTE && ((c >= 0 && c <= 10) || c == 100 || c == 101))
			return c;
		if(fam == FE_CLASSIC && c >= 200 && c <= 214)
			return c;
		if(fam == FE_GAMECUBE && c >= 400 && c <= 411)
			return c;
	}
	return -1;
}

struct fe_roles {
	int close, home, click, up, down, left, right, page, previous;
	int scroll_left, scroll_right;
};

static struct fe_roles fe_roles_for(enum fe_family fam) {
	struct fe_roles r;
	r.close = fe_code_for(fam, "inventory");
	r.home = fe_code_for(fam, "open_menu");
	r.click = fe_code_for(fam, "gui_click");
	r.up = fe_code_for(fam, "gui_up");
	r.down = fe_code_for(fam, "gui_down");
	r.left = fe_code_for(fam, "gui_left");
	r.right = fe_code_for(fam, "gui_right");
	r.page = fe_code_for(fam, "gui_page");
	r.previous = fe_code_for(fam, "gui_previous");
	r.scroll_left = fe_code_for(fam, "scroll_left");
	r.scroll_right = fe_code_for(fam, "scroll_right");
	assert(r.close > 0 && r.home > 0 && r.click > 0 && r.up >= 0 && r.down > 0
		   && r.left > 0 && r.right > 0 && r.page > 0 && r.previous > 0
		   && "a menu role is unbound for this family");
	return r;
}

/* ---- configuration fixtures --------------------------------------------- */

#define FE_SAVES_DIR "saves"
#define FE_REMAP_CONFIG "test_frontend_remap.json"
#define FE_RUN_DIR "tmp_frontend_no_pointer_run"

/* A3: the suite now drives the REAL destructive paths (delete a world, remove
 * a saved server). Both of those reach a config_save(&gstate.config_user,
 * "config.json") with a HARD-CODED relative path
 * (screen_multiplayer_setup.c:545, and texpacks_apply's TEXPACKS_CONFIG_PATH),
 * and ctest runs this target with WORKING_DIRECTORY = the source root -- so a
 * RED run of those cases would rewrite the repository's own config.json. The
 * whole suite therefore runs inside a scratch directory: everything the test
 * reads from the source tree is read (or copied) BEFORE the chdir, and every
 * file any screen writes afterwards lands in the scratch tree. */
static void fe_copy_file(const char* from, const char* to) {
	FILE* in = fopen(from, "rb");
	assert(in && "source file missing; is the CWD the source root?");
	FILE* out = fopen(to, "wb");
	assert(out);
	char buf[4096];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), in)) > 0)
		assert(fwrite(buf, 1, n, out) == n);
	fclose(in);
	fclose(out);
}

/* The REMAPPED table. Review finding M-3: under config_wii.json the Wii Remote
 * binds scroll_left/gui_previous to the same physical code (9/9) and
 * scroll_right/gui_page likewise (8/8), and the Classic does the same
 * (210/210, 211/211) -- so on those two families a revert of the page-role
 * hunk to the HOTBAR roles is INVISIBLE, and only the GameCube leg guards it.
 * This table separates the two roles on ALL THREE families (Remote 2 /
 * Nunchuk Z, Classic ZR / ZL, GameCube R / L against hotbar Plus / Minus,
 * full R / full L, X / B), so every leg discriminates. Close moves to
 * Remote B / Classic B / GameCube X -- and 406 is that family's scroll_right,
 * which keeps the one-press-one-action collision Astra A-3 asks about. */
static const char fe_remap_json[] =
	"{\"input\":{"
	"\"inventory\":[5,205,406],"
	"\"open_menu\":[10,214,411],"
	"\"gui_click\":[4,204,404],"
	"\"gui_up\":[0,200,400],\"gui_down\":[1,201,401],"
	"\"gui_left\":[2,202,402],\"gui_right\":[3,203,403],"
	"\"gui_page\":[7,209,410],\"gui_previous\":[100,208,409],"
	"\"scroll_left\":[9,210,405],\"scroll_right\":[8,211,406],"
	"\"debug_overlay\":[],\"screenshot\":[]"
	"}}";

/* The COLLIDING table (review A6-4, lead ruling R-C): activate and Close on
 * the SAME physical button, per family (Remote A, Classic A, GameCube A). A
 * press then carries THREE role edges -- activate, Close and the page-forward
 * role (A9-4) -- so a press the activation rule consumed must not also close
 * the screen or scroll the list. Nothing ships this table -- it is exactly
 * the remap a player is free to write, and the only way to test the rule
 * against the rest of the update. */
static const char fe_collide_json[] =
	"{\"input\":{"
	"\"inventory\":[4,204,404],"
	"\"open_menu\":[10,214,411],"
	"\"gui_click\":[4,204,404],"
	"\"gui_up\":[0,200,400],\"gui_down\":[1,201,401],"
	"\"gui_left\":[2,202,402],\"gui_right\":[3,203,403],"
	"\"gui_page\":[4,204,404],\"gui_previous\":[9,210,409],"
	"\"scroll_left\":[9,210,405],\"scroll_right\":[8,211,406],"
	"\"debug_overlay\":[],\"screenshot\":[]"
	"}}";

static bool fe_config_live;

static void fe_config_open_json(const char* json) {
	if(fe_config_live)
		config_destroy(&gstate.config_user);
	fe_config_live = true;
	FILE* f = fopen(FE_REMAP_CONFIG, "w");
	assert(f);
	fputs(json, f);
	fclose(f);
	assert(config_create(&gstate.config_user, FE_REMAP_CONFIG));
	assert(config_set_string(&gstate.config_user, "paths.worlds",
							 FE_SAVES_DIR));
	assert(config_set_string(&gstate.config_user, "mp.username", ""));
}

static void fe_config_open(bool remapped) {
	if(fe_config_live)
		config_destroy(&gstate.config_user);
	fe_config_live = true;
	if(remapped) {
		FILE* f = fopen(FE_REMAP_CONFIG, "w");
		assert(f);
		fputs(fe_remap_json, f);
		fclose(f);
		assert(config_create(&gstate.config_user, FE_REMAP_CONFIG));
	} else {
		assert(config_create(&gstate.config_user, "config_wii.json"));
	}
	assert(config_set_string(&gstate.config_user, "paths.worlds",
							 FE_SAVES_DIR));
	/* No name chosen: every Join / Quick Connect therefore parks in the name
	 * editor instead of reaching mps_activate's config_save, which would
	 * rewrite the repo's own config.json from under a test run. */
	assert(config_set_string(&gstate.config_user, "mp.username", ""));
}

/* `count` world directories, rebuilt from scratch. No level.dat: the rig's
 * level_archive double resolves every row to a readable survival world. */
static void fe_saves(size_t count) {
	/* A REBUILT fixture is a fresh Play session (review item 1): the Load
	 * tab now RETAINS the selected world's directory across resets, so a row
	 * selected by an earlier case would otherwise be restored into the next
	 * case's freshly built list (same directory names) and shift its opening
	 * row / page. The retention cases that need a remembered world select it
	 * AFTER their fe_saves. */
	play_session_enter_fresh();
	for(size_t k = 0; k < 32; k++) {
		char path[128];
		snprintf(path, sizeof(path), FE_SAVES_DIR "/world%02zu", k);
		fe_rmdir(path);
	}
	fe_rmdir(FE_SAVES_DIR);
	if(count == 0)
		return;
	fe_mkdir(FE_SAVES_DIR);
	for(size_t k = 0; k < count; k++) {
		char path[128];
		snprintf(path, sizeof(path), FE_SAVES_DIR "/world%02zu", k);
		assert(fe_mkdir(path) == 0 || errno == EEXIST);
	}
}

/* THE LONG LIST THAT ACTUALLY FINDS THE DEFECT (push-0921 A15).
 * FE_LONG_LIST above is ten rows -- enough to page, which is all the page-role
 * cases need -- and that is exactly why "a button-only player can Rename or
 * Delete only the LAST world" survived this suite: at ten rows the
 * DOWN-off-the-last-row entry still reads like a route. The maintainer's card
 * holds ~645 worlds. This second fixture is 650, built ONCE for the whole run
 * (650 mkdirs per case would dominate it) in its own directory, so every
 * existing FE_SAVES_DIR case keeps the small list it rebuilds per case. */
#define FE_BIG_SAVES_DIR "saves_big"
#define FE_BIG_LIST 650
/* "the 300th of 650" -- the row the card's player cannot reach today. */
#define FE_BIG_ROW 300

static bool fe_big_saves_built;

static void fe_saves_big(void) {
	if(!fe_big_saves_built) {
		assert(fe_mkdir(FE_BIG_SAVES_DIR) == 0 || errno == EEXIST);
		for(size_t k = 0; k < FE_BIG_LIST; k++) {
			char path[192];
			snprintf(path, sizeof(path), FE_BIG_SAVES_DIR "/world%03zu", k);
			assert(fe_mkdir(path) == 0 || errno == EEXIST);
		}
		fe_big_saves_built = true;
	}
	assert(config_set_string(&gstate.config_user, "paths.worlds",
							 FE_BIG_SAVES_DIR));
}

/* Point the world list back at the small per-case fixture. */
static void fe_saves_small_dir(void) {
	assert(config_set_string(&gstate.config_user, "paths.worlds",
							 FE_SAVES_DIR));
}

static void fe_servers(size_t count) {
	play_session_enter_fresh();
	struct config_mp_server rows[CONFIG_MP_SERVER_LIST_MAX] = {{{0}, {0}}};
	assert(count <= CONFIG_MP_SERVER_LIST_MAX);
	for(size_t k = 0; k < count; k++) {
		snprintf(rows[k].name, sizeof(rows[k].name), "Server %02zu", k);
		snprintf(rows[k].address, sizeof(rows[k].address), "10.0.0.%zu:25565",
				 k + 1);
	}
	assert(config_write_server_list(&gstate.config_user, rows, count));
}

/* ---- screen driving ----------------------------------------------------- */

#define FE_DT (1.0F / 60.0F)
/* screen_creative_picker.c's own grid geometry: 9 columns of 18 GUI units. */
#define PICKER_CELL_PX (18 * GFX_GUI_SCALE)

static struct screen* fe_current;

static void fe_open(struct screen* s) {
	fe_screen_last = NULL;
	fe_screen_sets = 0;
	fe_rename_forget();
	screen_set(s);
	fe_current = s;
	/* One neutral hardware poll before the first press, exactly like the
	 * frame boundary production puts between screen_set and the arriving
	 * screen's first update. It also clears input.c's own edge SWALLOW,
	 * which a screen leaving text capture arms on purpose (a key the OSK
	 * consumed must be released before it can drive an IB action again) --
	 * so a case that opened an OSK subview first does not silently lose its
	 * next press. */
	fe_idle();
	fe_screen_last = NULL;
	fe_screen_sets = 0;
}

/* One frame with nothing held, then the screen's own update. */
static void fe_step(void) {
	fe_idle();
	fe_current->update(fe_current, FE_DT);
}

/* One TAP of a physical button: the down frame runs update (that is the frame
 * the press edge exists on), then a neutral frame re-arms the latch. The
 * screen is NOT updated on the neutral frame, because the down frame may have
 * handed off to a different screen. */
static void fe_tap(int code) {
	fe_hold(code);
	fe_current->update(fe_current, FE_DT);
	fe_idle();
}

/* TWO physical buttons down on the SAME poll -- one hardware report, one
 * update. The only way to ask what happens when navigation and activation
 * arrive together (Astra ruling 2 item 2's same-frame ordering). */
static void fe_tap2(int a, int b) {
	int codes[2] = {a, b};
	fe_poll_with(codes, 2);
	fe_current->update(fe_current, FE_DT);
	fe_idle();
}

static void fe_pointer(float x, float y) {
	fe_ir_armed = true;
	fe_ir_x = x;
	fe_ir_y = y;
}
static void fe_pointer_off(void) { fe_ir_armed = false; }

/* IR VALIDITY flicker with the sprite UNMOVED: source/platform/input.c has no
 * hysteresis, so at the edge of the sensor-bar cone `valid` alternates frame
 * by frame while the Remote is perfectly still. `frames` full valid/invalid
 * pairs run, then the pointer is left in `end_valid`'s phase -- the two phases
 * are different inputs to the rule and the ruling (item 3) requires both to be
 * tested. */
static void fe_flicker(int frames, bool end_valid, float x, float y) {
	for(int k = 0; k < frames; k++) {
		fe_pointer_off();
		fe_step();
		fe_pointer(x, y);
		fe_step();
	}
	if(!end_valid) {
		fe_pointer_off();
		fe_step();
	}
}

/* ---- hooks the owned screens publish for this suite --------------------- */

int screen_sworld_test_scroll(void);
size_t screen_sworld_test_gui_selection(void);
int screen_texpacks_test_scroll(void);
size_t screen_texpacks_test_selection(void);
size_t screen_texpacks_test_pack_count(void);
void screen_texpacks_test_seed(size_t extra);
/* PACK-only link double. screen_texturepacks.c's audio validation counts the
 * creative playlist as well as the survival list, and the shared rig defines
 * only the survival counter. Signature matches audio_music.h exactly. This is
 * a headless screen fixture: it says "this build registered no creative
 * tracks", which is true of a test process that never loaded a pack -- it is
 * NOT a claim that any audio succeeded. The real creative-count behaviour is
 * proven against the real registry in tests/test_pack_helper_audio.c. */
int audio_music_creative_track_count(void);
int audio_music_creative_track_count(void) { return 0; }

/* PACK-only link double, gate 2. screen_texturepacks.c's approved composition
 * draws an OPTIONAL scenic backdrop (texture.h's texture_menu_background); the
 * shared rig defines the always-present atlases only, and this suite links no
 * texture.c. Left ZEROED on purpose -- .data == NULL is exactly the "this pack
 * ships no backdrop" case, so every render here takes the documented fallback
 * path (fep_backdrop returns false, the screen draws the tiled-dirt menu
 * background). It is NOT a claim that any backdrop loaded. */
struct tex_gfx texture_menu_background;

/* PACK-only link doubles, gate 2 ROW PREVIEWS. This suite links no texture.c
 * and has no PNG decoder, so the four texture.h entry points the pack screen's
 * preview cache uses are doubled here. They decode NOTHING: they record which
 * installed root the screen asked for, how often, and how it bracketed its
 * frees -- which is exactly what these cases assert (identity, no repeat I/O,
 * bounded slots, fenced clears). Image content and the real nearest-downsample
 * are NOT covered by this suite; that is disclosed, not faked. */
#define FE_TP_PREVIEW_LOG 32
static char fe_tp_preview_root[FE_TP_PREVIEW_LOG][128];
static int fe_tp_preview_calls;
static int fe_tp_fence_pre, fe_tp_fence_post;
/* A root the pretend card has no usable image for -- the screen must remember
 * that miss instead of asking again every frame. */
static const char* fe_tp_preview_missing;
static uint8_t fe_tp_preview_pixels[4];

void tex_init_pre(void);
void tex_init_post(void);
void tex_gfx_clear(struct tex_gfx* tex);
bool tex_preview_load(struct tex_gfx* tex, const char* pack_root);

void tex_init_pre(void) { fe_tp_fence_pre++; }
void tex_init_post(void) { fe_tp_fence_post++; }

void tex_gfx_clear(struct tex_gfx* tex) {
	/* The real one frees tex->data; the pixels handed out below are static
	 * storage, so this double only unbinds them. */
	*tex = (struct tex_gfx) {0};
}

bool tex_preview_load(struct tex_gfx* tex, const char* pack_root) {
	tex_gfx_clear(tex); /* the real loader's documented first act */
	if(fe_tp_preview_calls < FE_TP_PREVIEW_LOG)
		snprintf(fe_tp_preview_root[fe_tp_preview_calls],
				 sizeof(fe_tp_preview_root[0]), "%s",
				 pack_root ? pack_root : "");
	fe_tp_preview_calls++;
	if(fe_tp_preview_missing && pack_root
	   && !strcmp(pack_root, fe_tp_preview_missing))
		return false; /* "this pack ships no preview": tex stays cleared */
	tex->data = fe_tp_preview_pixels;
	tex->width = TEX_PREVIEW_DIM;
	tex->height = TEX_PREVIEW_DIM;
	return true;
}


/* The pack screen's two independent choices (Astra gate 1 item 7): which
 * category the list is editing, how many rows that category shows, which pack
 * Apply would commit, and whether Apply is live at all. */
void screen_texpacks_test_seed_audio(size_t extra, bool with_music);
int screen_texpacks_test_category(void);
size_t screen_texpacks_test_row_count(void);
size_t screen_texpacks_test_pending(void);
bool screen_texpacks_test_apply_enabled(void);
int screen_texpacks_test_highlight(void);
/* screen_texturepacks.c's `enum texpacks_category` and `enum texpacks_button`,
 * mirrored: the ids the two hooks above report. The strip is three controls --
 * the category selector, Apply and Done -- and that ORDER is the ruled
 * left-to-right layout (gate 1 item 8). */
#define FE_TP_CAT_TEXTURES 0
#define FE_TP_CAT_SOUNDS 1
#define FE_TP_BTN_CATEGORY 0
#define FE_TP_BTN_APPLY 1
#define FE_TP_BTN_DONE 2
#define FE_TP_NO_PENDING 32 /* MAX_PACKS: "nothing chosen yet" */
int screen_mps_test_scroll(void);
size_t screen_mps_test_selection(void);
size_t screen_mps_test_server_count(void);
int screen_mps_test_view(void);
bool screen_mps_test_pending_join(char* address, size_t cap);
int screen_mpc_test_state(void);
const char* screen_mpc_test_error_title(void);
void screen_mpc_test_set_state(int state);
void screen_mps_test_set_ping_cursor(size_t cursor);
bool screen_mps_test_refresh_enabled(void);
int screen_mps_test_focus(void);
size_t screen_mps_test_page(void);
bool screen_mps_test_options_open(void);
bool screen_mps_test_target_rect(int target, struct gutil_button_bounds* bounds);
bool screen_mps_test_focus_enabled(void);
int screen_sworld_test_focus(void);
int screen_mpc_test_focus(void);
size_t screen_mpc_test_reason_page(void);
size_t screen_mpc_test_reason_pages(void);
bool screen_mpc_test_reason_line(size_t line, char* out, size_t cap);
bool screen_mpc_test_target_rect(int target, struct gutil_button_bounds* out);
/* A15: which world a Delete armed, which server Edit/Delete armed, and the
 * SLOTLIST selection each list screen actually draws highlighted. */
bool screen_sworld_test_delete_target(char* path, size_t cap);
size_t screen_sworld_test_list_selection(void);
bool screen_mps_test_edit_target(size_t* index, bool* is_new, char* name,
								 size_t name_cap, char* address,
								 size_t address_cap);
size_t screen_mps_test_delete_index(void);
int screen_mps_test_edit_field(void);
bool screen_mps_test_osk_writes_address(void);
bool screen_mps_test_editor_open(void);
int screen_mps_test_parent_focus(void);
bool screen_mps_test_parent_rect(int target, struct gutil_button_bounds* bounds);
int screen_mps_test_editor_target(void);
size_t screen_mps_test_editor_caret(void);
/* A19 (Astra ruling 6): the navigation hint exactly as the render draws it --
 * its text, its rectangle, and the two edges it must stay between. */
bool screen_sworld_test_hint(char* text, size_t cap, int* y, int* h,
							 int* list_bottom, int* strip_top);
bool screen_mps_test_hint(char* text, size_t cap, int* y, int* h,
						  int* list_bottom, int* strip_top);
/* A23 (review A20-3): is the sticky "Couldn't save multiplayer settings" line
 * up? It owns the hint's band while it is, so the hint must not be drawn. */
bool screen_mps_test_save_failed(void);
int screen_texpacks_test_list_bottom(void);
int screen_texpacks_test_strip_top(void);
size_t screen_mps_test_list_selection(void);
int screen_sworld_test_row(size_t idx, const char** name,
						   const char** directory, const char** path,
						   int64_t* last_access, int64_t* byte_size);
/* The target each screen DRAWS highlighted -- the very variable/predicate its
 * render2D reads, exposed so a headless case can assert "what is highlighted
 * is what fires" without calling render2D (which this rig refuses). */
int screen_title_test_highlight(void);
int screen_confirm_test_highlight(void);
int screen_picker_test_highlight(void);
int screen_sworld_test_highlight(void);
int screen_sworld_test_row_target(size_t row);
size_t screen_sworld_test_row_count(void);
/* Upper bound for the visible Join action ring. */
#define MPS_STRIP_LEN 7
/* screen_select_world.c's `enum sworld_button`, mirrored: the ids
 * screen_sworld_test_focus() and screen_sworld_test_highlight() report. The
 * RING walks the VISUAL order (Play, Create, Rename, Delete, Cancel), which is
 * deliberately not this declaration order. */

/* THE APPROVED TITLE (design-wiring/title-red, VISUAL-COORDINATION.md
 * sections 6 and 9): FOUR full-width rows, top to bottom Play game / Texture
 * packs / Options / Quit, replacing Beta's five-button GuiMainMenu layout
 * (Singleplayer / Multiplayer / Mods and Texture Packs / Options... + Quit
 * Game halves). Multiplayer is no longer a title row -- the Play row opens
 * the retained Play session (Load / Create / Join tabs, play_session.h), so
 * the standalone disabled Multiplayer control is GONE and its disabled-hover
 * coverage lives on the pack screen's in-game Sounds selector below
 * (case_pointer_over_disabled_control). These are the ordinals the title's
 * highlight seam reports; the ring walks them in this order. */
enum {
	FE_TB_PLAY = 0,
	FE_TB_TUTORIAL, /* ui-build-1026 T03: Tutorial world */
	FE_TB_PACKS,
	FE_TB_OPTIONS,
	FE_TB_QUIT,
	FE_TB_COUNT,
};

/* The approved rows' OBSERVED geometry, in 640x480 PIXELS (the package
 * composes in pixels, like the pack screen's FE_TP_* pins): every row is
 * (x 160, y 166 + 48 * i, w 320, h 44) since ui-build-1026 T03 (five rows,
 * Astra's approved title), so rows sit at y 166 / 214 / 262 / 310 / 358 with
 * a 4 px gap between them, and the hit test is lower-inclusive,
 * upper-exclusive (gutil_button_hit). A small test-local helper, not a
 * framework: the cases below ask for a row's rect or its centre and nothing
 * else. */
#define FE_TITLE_ROW_X 160
#define FE_TITLE_ROW_Y0 166
#define FE_TITLE_ROW_PITCH 48
#define FE_TITLE_ROW_W 320
#define FE_TITLE_ROW_H 44

struct fe_rect {
	int x, y, w, h;
};

static struct fe_rect fe_title_row(int row) {
	assert(row >= 0 && row < FE_TB_COUNT);
	return (struct fe_rect) {FE_TITLE_ROW_X,
							 FE_TITLE_ROW_Y0 + FE_TITLE_ROW_PITCH * row,
							 FE_TITLE_ROW_W, FE_TITLE_ROW_H};
}

/* The centre of a row: where every "the sprite rests on <row>" case parks. */
static float fe_title_cx(int row) {
	struct fe_rect r = fe_title_row(row);
	return (float)(r.x + r.w / 2);
}
static float fe_title_cy(int row) {
	struct fe_rect r = fe_title_row(row);
	return (float)(r.y + r.h / 2);
}

/* THE TITLE OBSERVER, asked through the NEW public query -- and, at every ask,
 * the legacy transition alias must answer the same (section 6 item 1: one
 * line, no second implementation). Every title observer case below goes
 * through this, so the alias-equality check is never skipped. */
static bool fe_title_play(void) {
	bool now = screen_title_focus_is_play();
	assert(screen_title_focus_is_multiplayer() == now
		   && "the legacy alias screen_title_focus_is_multiplayer disagrees "
			  "with screen_title_focus_is_play");
	return now;
}

/* mps view ordinals, mirrored from screen_multiplayer_setup.c's enum. */
enum { FE_MPS_MAIN = 0, FE_MPS_EDIT_NAME, FE_MPS_EDIT_SERVER, FE_MPS_DIRECT };
/* mps focus ordinals, likewise: the strip is 0..6 in its enum order, and the
 * negatives are the ring positions outside it. */
enum {
	MPS_BUTTON_JOIN_ORD = 0,
	MPS_BUTTON_DIRECT_ORD = 1,
	MPS_BUTTON_ADD_ORD = 2,
	MPS_BUTTON_EDIT_ORD = 3,
	MPS_BUTTON_DELETE_ORD = 4,
	MPS_BUTTON_REFRESH_ORD = 5,
	MPS_BUTTON_BACK_ORD = 6,
	MPS_FOCUS_LIST_ORD = -1,
	MPS_FOCUS_QUICK_ORD = -2,
	MPS_FOCUS_NAME_ORD = -3,
	MPS_FOCUS_NONE_ORD = -4,
};
/* mpc state ordinals, mirrored from screen_multiplayer_connect.c's enum. */
enum { FE_MPC_BRINGUP = 0, FE_MPC_CONNECTING, FE_MPC_ERROR };

/* ======================================================================== */
/* a. the Close role                                                         */
/* ======================================================================== */

static void case_close_role(enum fe_family fam, bool remapped) {
	struct fe_roles r = fe_roles_for(fam);

	/* Select World -> its parent, the title. */
	fe_saves(3);
	fe_attach(fam);
	fe_open(&screen_select_world);
	fe_tap(r.close);
	assert(fe_screen_last == &screen_title
		   && "Close did not close Select World to the title");

	/* Texture Packs -> whatever opener registered itself. */
	screen_texturepacks_set_return(&screen_title);
	fe_attach(fam);
	fe_open(&screen_texturepacks);
	fe_tap(r.close);
	assert(fe_screen_last == &screen_title
		   && "Close did not close Texture Packs to its parent");

	/* Astra A-2: the title has NO parent Back. Close must do nothing at all
	 * -- not quit, not navigate. */
	fe_attach(fam);
	fe_open(&screen_title);
	fe_tap(r.close);
	assert(fe_screen_sets == 0 && !gstate.quit
		   && "Close became an action on the title screen");

	(void)remapped;
}

/* Astra A-2's other half: the loading screen keeps its HELD cancel. That
 * screen is not linked here (it would drag the world/server/network stacks
 * into a focused input test), so the contract is pinned at its source: the
 * escape hatch reads input_held(IB_INVENTORY), and there is no
 * input_pressed(IB_INVENTORY) anywhere in the file that could have turned
 * Close into an immediate action while this card moved the role around. */
static void case_load_world_hold_unchanged(void) {
	FILE* f = fopen("source/game/gui/screen_load_world.c", "rb");
	assert(f && "screen_load_world.c not readable from the test's CWD");
	static char text[262144];
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	assert(n > 0 && n < sizeof(text) - 1);
	text[n] = '\0';
	/* Review finding L-2: this IS a SOURCE-TEXT GUARD, not a behavioural
	 * test, and it says so. A behavioural one would have to link
	 * screen_load_world.c, which pulls client_interface.c + server_local.c +
	 * server_local_state.c and the whole world/network stack into a focused
	 * input target -- past the build-graph's 19-TU ceiling for a test that
	 * does not link a pool, and outside this card's owned paths. The guard is
	 * hardened against the two bypasses the reviewer named: whitespace is
	 * stripped first (so `input_pressed( IB_INVENTORY )` cannot slip past),
	 * and EVERY read of the role is required to be the held form, so routing
	 * Close through a different reader shows up as a missing held read. */
	static char squashed[sizeof(text)];
	size_t m = 0;
	for(size_t k = 0; k < n; k++)
		if(!isspace((unsigned char)text[k]))
			squashed[m++] = text[k];
	squashed[m] = '\0';
	assert(strstr(squashed, "input_held(IB_INVENTORY)")
		   && "the loading screen's held cancel is gone");
	assert(!strstr(squashed, "input_pressed(IB_INVENTORY)")
		   && "Close became an immediate action on the loading screen");
	assert(!strstr(squashed, "input_released(IB_INVENTORY)")
		   && "Close became a release-edge action on the loading screen");
	/* Every READ of the role -- an occurrence in first-argument position,
	 * i.e. immediately after a '(' -- must be the held form. An occurrence
	 * after a ',' is a prompt-glyph argument (gutil_control_icon), which
	 * reads nothing. This is what catches "routed through a different
	 * reader": a new input_*_owned / input_frame_* read would land in
	 * first-argument position of something other than input_held. */
	static const char held[] = "input_held(";
	unsigned held_reads = 0;
	for(const char* p = strstr(squashed, "IB_INVENTORY"); p;
		p = strstr(p + 1, "IB_INVENTORY")) {
		if(p == squashed || p[-1] != '(')
			continue; /* an argument to a draw/prompt call, not a read */
		assert(p - squashed >= (long)(sizeof(held) - 1)
			   && !memcmp(p - (sizeof(held) - 1), held, sizeof(held) - 1)
			   && "a non-held read of the Close role appeared in the loading "
				  "screen");
		held_reads++;
	}
	assert(held_reads >= 2
		   && "the loading screen's fresh-press latch and its hold "
			  "accumulator are both supposed to read the held Close");
	puts("PASS Close scope (SOURCE-TEXT GUARD, not a behavioural test): the "
		 "loading screen reads Close only through input_held");
}

/* ======================================================================== */
/* b. the page roles, at every list boundary                                 */
/* ======================================================================== */

/* Review finding M-3, made visible instead of implicit: state, per family and
 * per table, whether this leg can tell the CANONICAL page roles apart from the
 * HOTBAR roles at all. Under config_wii.json the Remote and the Classic bind
 * both pairs to one physical button, so those legs are DEGENERATE -- they
 * cannot fail on a revert of the page-role hunk, and only the GameCube guards
 * it. The remapped table separates them on all three, so every leg
 * discriminates. If config_wii.json ever changes, this assertion -- not a
 * silent hole -- is what fails. */
static void case_page_role_discrimination(enum fe_family fam, bool remapped) {
	struct fe_roles r = fe_roles_for(fam);
	bool separate = r.page != r.scroll_right && r.previous != r.scroll_left;
	if(remapped) {
		assert(separate
			   && "the remapped table must bind the page roles and the hotbar "
				  "roles to DIFFERENT physical buttons on every family, or "
				  "this leg cannot detect a revert of the page-role change");
		return;
	}
	if(fam == FE_GAMECUBE) {
		assert(separate
			   && "the GameCube is the only shipped leg that discriminates "
				  "the page-role change; config_wii.json has drifted");
		return;
	}
	assert(!separate
		   && "config_wii.json used to bind this family's page role and hotbar "
			  "role to the SAME physical button, which is why the shipped leg "
			  "is degenerate; if that changed, drop this note");
}

/* A list long enough that the viewport cannot hold it: 10 rows of 36 units
 * in a 480-pixel screen leaves the slotlist scrollable in both directions. */
#define FE_LONG_LIST 10
/* The approved Load page (four 48 px rows): defined with the rest of the
 * FE_LW_* pins further down; repeated here (identically) because this case
 * sits above them. The compiler rejects a mismatch. */
#define FE_LW_ROWS_VISIBLE 4
#define FE_LW_ROW_PITCH 48

static void case_page_roles_select_world(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	/* FIRST PAGE: already at the top, previous-page cannot move it. */
	fe_saves(FE_LONG_LIST);
	fe_attach(fam);
	fe_open(&screen_select_world);
	assert(screen_sworld_test_scroll() == 0);
	size_t selected = screen_sworld_test_gui_selection();
	fe_tap(r.previous);
	assert(screen_sworld_test_scroll() == 0
		   && "the previous-page role moved a list already at its first page");
	assert(screen_sworld_test_gui_selection() == selected
		   && "paging lost the selected row");

	/* The page role moves it, and exactly once per physical press. APPROVED
	 * LOAD PAGING (LOAD-NATIVE-REVIEW item 1): a page step is one FULL page
	 * of four 48 px rows (192), and the selection is REVEALED on the shown
	 * page -- it moves onto that page's first row instead of staying hidden
	 * on the page that scrolled away (Beta's GuiSlot kept it off-screen; the
	 * screen-local paging keeps the focused row visible). So the claim is
	 * "the selection is on the shown page", not "unchanged". */
	fe_tap(r.page);
	int after_one = screen_sworld_test_scroll();
	assert(after_one == FE_LW_ROWS_VISIBLE * FE_LW_ROW_PITCH
		   && "the page role did not page the world list by one full page");
	assert(screen_sworld_test_gui_selection() == (size_t)FE_LW_ROWS_VISIBLE
		   && "paging did not reveal the selection on the shown page");
	fe_tap(r.page);
	assert(screen_sworld_test_scroll() == after_one * 2
		   && "the second page step did not move by the same amount");

	/* ... and the previous role steps back by exactly the same amount. */
	fe_tap(r.previous);
	assert(screen_sworld_test_scroll() == after_one);

	/* LAST PAGE: paging past the end clamps and stays there, with the
	 * selection on it. */
	for(int k = 0; k < 40; k++)
		fe_tap(r.page);
	int bottom = screen_sworld_test_scroll();
	fe_tap(r.page);
	assert(screen_sworld_test_scroll() == bottom
		   && "the page role ran off the end of the list");
	assert(bottom == (int)((FE_LONG_LIST - 1) / FE_LW_ROWS_VISIBLE)
				   * FE_LW_ROWS_VISIBLE * FE_LW_ROW_PITCH);
	assert(screen_sworld_test_gui_selection() * FE_LW_ROW_PITCH >= (size_t)bottom
		   && "the selection was left on a page that scrolled away");
	(void)selected;

	/* SINGLE PAGE: one row fits, so neither role moves anything. */
	fe_saves(1);
	fe_attach(fam);
	fe_open(&screen_select_world);
	int single = screen_sworld_test_scroll();
	fe_tap(r.page);
	assert(screen_sworld_test_scroll() == single);
	fe_tap(r.previous);
	assert(screen_sworld_test_scroll() == single
		   && "a single-page list scrolled");

	/* EMPTY LIST: neither role moves anything, and nothing is activated. */
	fe_saves(0);
	fe_attach(fam);
	fe_open(&screen_select_world);
	fe_tap(r.page);
	fe_tap(r.previous);
	assert(fe_screen_sets == 0 && "paging an empty world list did something");
}

static void case_page_roles_texture_packs(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	/* SINGLE PAGE first: the bundled default pack is always row 0, and on a
	 * host with no texturepacks/ directory it is the whole list. */
	fe_attach(fam);
	screen_texturepacks_set_return(&screen_title);
	fe_open(&screen_texturepacks);
	assert(screen_texpacks_test_pack_count() >= 1);
	int single = screen_texpacks_test_scroll();
	fe_tap(r.page);
	fe_tap(r.previous);
	assert(screen_texpacks_test_scroll() == single
		   && "a single-page pack list scrolled");

	/* Then a list long enough to page. */
	screen_texpacks_test_seed(FE_LONG_LIST);
	assert(screen_texpacks_test_pack_count() == FE_LONG_LIST + 1);
	size_t selected = screen_texpacks_test_selection();
	fe_tap(r.previous);
	assert(screen_texpacks_test_scroll() == 0
		   && "the previous-page role moved a list already at its first page");
	fe_tap(r.page);
	int after_one = screen_texpacks_test_scroll();
	assert(after_one > 0 && "the page role did not page the pack list");
	assert(screen_texpacks_test_selection() == selected
		   && "paging lost the selected pack");
	for(int k = 0; k < 40; k++)
		fe_tap(r.page);
	int bottom = screen_texpacks_test_scroll();
	fe_tap(r.page);
	assert(screen_texpacks_test_scroll() == bottom
		   && "the page role ran off the end of the pack list");
	fe_tap(r.previous);
	assert(screen_texpacks_test_scroll() < bottom);

	/* The DISABLED "Open texture pack folder" button (id 5 -- there is no OS
	 * folder-open operation on Wii) has no directional route and must gain
	 * none: with no pointer, the whole D-pad plus both page roles move the
	 * selection and the viewport and nothing else -- no navigation, and no
	 * pack applied. */
	const char* active_before = config_read_string(&gstate.config_user,
												   "paths.texturepack", "");
	char active_copy[128];
	snprintf(active_copy, sizeof(active_copy), "%s", active_before);
	int walk[] = {r.down, r.up, r.left, r.right, r.page, r.previous};
	for(size_t k = 0; k < sizeof(walk) / sizeof(walk[0]); k++)
		fe_tap(walk[k]);
	assert(fe_screen_sets == 0
		   && "a directional input navigated away from Texture Packs");
	assert(!strcmp(config_read_string(&gstate.config_user,
									  "paths.texturepack", ""),
				   active_copy)
		   && "a directional input applied a texture pack");
}

static void fe_mps_content(const struct fe_roles* r);

static void case_page_roles_multiplayer(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	fe_servers(FE_LONG_LIST);
	fe_attach(fam);
	fe_mps_content(&r);
	assert(screen_mps_test_scroll() == 0);
	size_t selected = screen_mps_test_selection();
	fe_tap(r.previous);
	assert(screen_mps_test_scroll() == 0
		   && "the previous-page role moved a list already at its first page");
	fe_tap(r.page);
	int after_one = screen_mps_test_scroll();
	assert(after_one > 0 && "the page role did not page the server list");
	assert(screen_mps_test_selection() == selected
		   && "paging lost the selected server");
	for(int k = 0; k < 40; k++)
		fe_tap(r.page);
	int bottom = screen_mps_test_scroll();
	fe_tap(r.page);
	assert(screen_mps_test_scroll() == bottom
		   && "the page role ran off the end of the server list");

	/* Empty content has no page movement. */
	fe_servers(0);
	fe_attach(fam);
	fe_mps_content(&r);
	int empty = screen_mps_test_scroll();
	fe_tap(r.page);
	fe_tap(r.previous);
	assert(screen_mps_test_scroll() == empty
		   && screen_mps_test_view() == FE_MPS_MAIN
		   && "paging an empty server list did something");
}

/* ======================================================================== */
/* e. the hotbar-role alias collision test, per family                       */
/* ======================================================================== */

/* Astra A-3: a hotbar alias may survive only where ONE physical press causes
 * exactly ONE intended action. On the shipped Wii table the alias cannot: the
 * Wii Remote binds scroll_left AND gui_previous to Minus (9) and scroll_right
 * AND gui_page to Plus (8); the Classic binds both pairs to full L (210) and
 * full R (211). A screen that reads both roles therefore pages TWICE on one
 * press. The GameCube's codes differ (B/X vs L/R triggers), but a role read is
 * not per-family -- keeping the alias in the source keeps it for the two
 * families it breaks -- so the only reading that passes is: no hotbar role is
 * read on a menu list at all. */
static void case_hotbar_alias_collision(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	fe_saves(FE_LONG_LIST);
	fe_attach(fam);
	fe_open(&screen_select_world);

	/* One page step, measured on the canonical role alone. */
	fe_tap(r.page);
	int step = screen_sworld_test_scroll();
	assert(step > 0);

	/* The same physical button pressed once more must move by exactly one
	 * more step -- never two, whichever roles that code happens to carry. */
	fe_tap(r.page);
	assert(screen_sworld_test_scroll() == step * 2
		   && "one press of the page button paged more than once");

	if(r.scroll_right >= 0 && r.scroll_right != r.page) {
		/* A hotbar code that is NOT the page code must not page at all. */
		int before = screen_sworld_test_scroll();
		fe_tap(r.scroll_right);
		assert(screen_sworld_test_scroll() == before
			   && "the hotbar role still scrolls a menu list");
	}
	if(r.scroll_left >= 0 && r.scroll_left != r.previous) {
		int before = screen_sworld_test_scroll();
		fe_tap(r.scroll_left);
		assert(screen_sworld_test_scroll() == before
			   && "the hotbar role still scrolls a menu list");
	}

	/* And a hotbar code that IS a directional or Close code must not steal
	 * focus or page. The GameCube remap puts Close on X (406), the same code
	 * as that family's scroll_right. */
	if(r.scroll_right == r.close) {
		fe_saves(3);
		fe_attach(fam);
		fe_open(&screen_select_world);
		fe_tap(r.close);
		assert(fe_screen_last == &screen_title
			   && "Close on a code shared with the hotbar stopped closing");
	}
}

/* ======================================================================== */
/* c. reachability by direction + activate, with no pointer                  */
/* ======================================================================== */

/* Walk DOWN into the action strip, then RIGHT `steps` times, then activate. */
static void fe_strip(const struct fe_roles* r, int down, int right) {
	for(int k = 0; k < down; k++)
		fe_tap(r->down);
	for(int k = 0; k < right; k++)
		fe_tap(r->right);
	fe_tap(r->click);
}

/* Enter retained Join content through the actual header, never a focus setter. */
static void fe_mps_content(const struct fe_roles* r) {
	fe_open(&screen_multiplayer_setup_impl);
	assert(screen_mps_test_focus() == 12);
	fe_tap(r->down);
}

/* Walk the visible action ring and its local options modal. */
static void fe_mps_focus_action(const struct fe_roles* r, int target) {
	if(screen_mps_test_options_open() && target != 3 && target != 4 && target != 5)
		fe_tap(r->close);
	if(!screen_mps_test_options_open()) {
		if(screen_mps_test_focus() == 12) fe_tap(r->down);
		if(screen_mps_test_focus() == MPS_FOCUS_LIST_ORD) fe_tap(r->right);
		int outer = target >= 3 && target <= 5 ? 9 : target;
		for(int n = 0; screen_mps_test_focus() != outer && n < 8; n++)
			fe_tap(r->right);
		assert(screen_mps_test_focus() == outer);
		if(outer == 9 && target != 9) fe_tap(r->click);
	}
	for(int n = 0; screen_mps_test_focus() != target && n < 4; n++)
		fe_tap(r->right);
	assert(screen_mps_test_focus() == target);
}
static void fe_mps_action(const struct fe_roles* r, int target) {
	fe_mps_focus_action(r, target);
	fe_tap(r->click);
	if(screen_mps_test_view() != FE_MPS_MAIN) fe_step();
}

/* THE APPROVED LOAD TAB (design-wiring/load-red; GATE3-PLAY-PLAN.md, the
 * Load composition ratified by root): Select World IS the Load tab of the
 * retained Play session. In 640x480 canvas pixels (height/480 scaled, centred
 * like the pack screen and the title):
 *   title "Play game"      88,14   26 px
 *   tabs Load/Create/Join  88+156*i, 50   152x46
 *   panel                  88,94   464x214
 *   rows (four visible)    100, 110+48*i  440x46
 *   Prev / Next            100,310 44x44 / 208,310 44x44
 *   Play                   100,366 276x44
 *   World options          392,366 148x44
 * There is NO legacy action strip (Cancel / Create / Rename / Delete): Create
 * is the Create tab, Rename and Delete live in the local "World options" modal
 * (Rename / Delete / Back, opening on Back), and the footer Back returns to
 * the title. A row press SELECTS and focuses Play; only Play launches.
 *
 * Focus ring (one gutil_menu_activate_decide path, same ids for pointer and
 * pad): header tabs <-> DOWN -> the selected row (or Play when there are no
 * rows) ; DOWN from the last row -> Prev ; RIGHT/LEFT on the page row -> Next
 * / Prev ; DOWN from the page row -> Play ; RIGHT/LEFT on the action row ->
 * World options / Play ; UP from the first row -> the Load tab ; LEFT/RIGHT on
 * the header -> Load / Create / Join, and activating a non-active tab switches
 * screens with the session preserved. */
enum {
	FE_LW_TAB_LOAD = 0,
	FE_LW_TAB_CREATE,
	FE_LW_TAB_JOIN,
	FE_LW_PREV,
	FE_LW_NEXT,
	FE_LW_PLAY,
	FE_LW_OPTIONS,
	FE_LW_BUTTON_COUNT,
};
/* screen_select_world.c world_row_state: resolved/readable. */
#define FE_LW_ROW_OK 1
#define FE_LW_ROW(i) (FE_LW_BUTTON_COUNT + (int)(i))
/* The modal's three rows, in their visual order; it opens on Back. */
enum { FE_LW_OPT_RENAME = 0, FE_LW_OPT_DELETE, FE_LW_OPT_BACK };
#define FE_LW_TITLE_X 88
#define FE_LW_TITLE_Y 14
#define FE_LW_TITLE_FONT 26
#define FE_LW_TAB_X0 88
#define FE_LW_TAB_PITCH 156
#define FE_LW_TAB_Y 50
#define FE_LW_TAB_W 152
#define FE_LW_TAB_H 46
#define FE_LW_PANEL_X 88
#define FE_LW_PANEL_Y 94
#define FE_LW_PANEL_W 464
#define FE_LW_PANEL_H 214
#define FE_LW_ROW_X 100
#define FE_LW_ROW_Y0 110
#define FE_LW_ROW_PITCH 48
#define FE_LW_ROW_W 440
#define FE_LW_ROW_H 46
#define FE_LW_ROWS_VISIBLE 4
#define FE_LW_PREV_X 100
#define FE_LW_NEXT_X 208
#define FE_LW_PAGE_Y 310
#define FE_LW_PAGE_S 44
#define FE_LW_PLAY_X 100
#define FE_LW_ACTION_Y 366
#define FE_LW_PLAY_W 276
#define FE_LW_OPTIONS_X 392
#define FE_LW_OPTIONS_W 148
#define FE_LW_ACTION_H 44

/* The rect of each Load control, in 640x480 pixels (rows: the i-th VISIBLE
 * row). Test-local; the screen's own rect function is the thing under test. */
static struct fe_rect fe_load_rect(int target) {
	if(target >= FE_LW_BUTTON_COUNT)
		return (struct fe_rect) {FE_LW_ROW_X,
								 FE_LW_ROW_Y0
									 + FE_LW_ROW_PITCH * (target - FE_LW_BUTTON_COUNT),
								 FE_LW_ROW_W, FE_LW_ROW_H};
	switch(target) {
		case FE_LW_TAB_LOAD:
		case FE_LW_TAB_CREATE:
		case FE_LW_TAB_JOIN:
			return (struct fe_rect) {FE_LW_TAB_X0 + FE_LW_TAB_PITCH * target,
									 FE_LW_TAB_Y, FE_LW_TAB_W, FE_LW_TAB_H};
		case FE_LW_PREV:
			return (struct fe_rect) {FE_LW_PREV_X, FE_LW_PAGE_Y, FE_LW_PAGE_S,
									 FE_LW_PAGE_S};
		case FE_LW_NEXT:
			return (struct fe_rect) {FE_LW_NEXT_X, FE_LW_PAGE_Y, FE_LW_PAGE_S,
									 FE_LW_PAGE_S};
		case FE_LW_PLAY:
			return (struct fe_rect) {FE_LW_PLAY_X, FE_LW_ACTION_Y, FE_LW_PLAY_W,
									 FE_LW_ACTION_H};
		default:
			return (struct fe_rect) {FE_LW_OPTIONS_X, FE_LW_ACTION_Y,
									 FE_LW_OPTIONS_W, FE_LW_ACTION_H};
	}
}
static float fe_load_cx(int target) {
	struct fe_rect b = fe_load_rect(target);
	return (float)(b.x + b.w / 2);
}
static float fe_load_cy(int target) {
	struct fe_rect b = fe_load_rect(target);
	return (float)(b.y + b.h / 2);
}

/* RIGHT enters Play without changing the selected row, then World options. */
static void fe_load_options(const struct fe_roles* r, int rows) {
	(void)rows;
	size_t selected = screen_sworld_test_list_selection();
	fe_tap(r->right); fe_tap(r->right); fe_tap(r->click);
	assert(screen_sworld_test_list_selection() == selected);
	assert(fe_screen_sets == 0
		&& "World options opened a SCREEN instead of the local modal");
}
/* Delete through the modal: World options, UP from Back onto Delete, A. */
static void fe_load_delete(const struct fe_roles* r, int rows) {
	fe_load_options(r, rows);
	fe_tap(r->up);
	fe_tap(r->click);
}
/* Rename through the modal: UP twice from Back. */
static void fe_load_rename(const struct fe_roles* r, int rows) {
	fe_load_options(r, rows);
	fe_tap(r->up);
	fe_tap(r->up);
	fe_tap(r->click);
}

static void case_reach_select_world(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	const int rows = 3;
	play_session_enter_fresh(); /* independent fixture: no retained selection */

	/* Every action must be reachable by direction + activate on the approved
	 * Load tab: Play launches the selected world, World options opens the
	 * modal whose Rename / Delete reach their screens, the Create and Join
	 * tabs switch screens, and the Load tab (already active) does nothing. */
	fe_saves(rows);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	assert(screen_sworld_test_focus() == FE_LW_ROW(0)
		   && "the Load tab does not open focused on the selected row");
	fe_strip(&r, rows, 0);
	assert(fe_screen_last == &screen_load_world
		   && "Play is not reachable by direction + activate");

	fe_saves(rows);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	fe_load_rename(&r, rows);
	assert(fe_screen_last == &screen_rename_world
		   && "Rename is not reachable through World options");

	fe_saves(rows);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	fe_load_delete(&r, rows);
	assert(fe_screen_last == &screen_confirm
		   && "Delete is not reachable through World options");

	/* The header: UP from the first row lands on the Load tab; RIGHT walks
	 * Create then Join; activating a non-active tab switches screens. */
	fe_saves(rows);
	fe_attach(fam);
	fe_open(&screen_select_world);
	fe_tap(r.up);
	assert(screen_sworld_test_focus() == FE_LW_TAB_LOAD
		   && "UP from the first row does not reach the Load tab");
	fe_tap(r.click);
	assert(fe_screen_sets == 0 && "activating the ACTIVE Load tab did something");
	fe_tap(r.right);
	assert(screen_sworld_test_focus() == FE_LW_TAB_CREATE);
	fe_tap(r.click);
	assert(fe_screen_last == &screen_create_world
		   && play_session_tab() == PLAY_TAB_CREATE
		   && "the Create tab is not reachable by direction + activate");

	fe_saves(rows);
	fe_attach(fam);
	fe_open(&screen_select_world);
	fe_tap(r.up);
	fe_tap(r.right);
	fe_tap(r.right);
	assert(screen_sworld_test_focus() == FE_LW_TAB_JOIN);
	fe_tap(r.click);
	assert(fe_screen_last == screen_multiplayer_setup
		   && play_session_tab() == PLAY_TAB_JOIN
		   && "the Join tab is not reachable by direction + activate");

	/* ONE PAGE (<= 4 rows, approved): there are NO page controls -- DOWN from
	 * the last row is Play, RIGHT is World options, LEFT is Play again, and
	 * UP from the action row returns to the SELECTED row. */
	fe_saves(rows);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int k = 0; k < rows; k++)
		fe_tap(r.down);
	assert(screen_sworld_test_focus() == FE_LW_PLAY
		   && "DOWN from the last row of a one-page list does not reach Play");
	fe_tap(r.right);
	assert(screen_sworld_test_focus() == FE_LW_OPTIONS);
	fe_tap(r.left);
	assert(screen_sworld_test_focus() == FE_LW_PLAY);
	fe_tap(r.up);
	assert(screen_sworld_test_focus() == FE_LW_ROW(rows - 1)
		   && "UP from the action row does not return to the selected row");

	/* MORE THAN ONE PAGE (6 rows): DOWN walks the rows ACROSS the page break
	 * (the selection is revealed on page 2 as it passes row 4), and DOWN
	 * from the LAST row of the list lands on the enabled page control -- on
	 * the last page that is Prev (Next is boundary-disabled and RIGHT cannot
	 * reach it). DOWN is Play, RIGHT is World options, LEFT is Play, UP
	 * returns to the page control; Prev then pages back and hands the ring
	 * to Next, the arrow that is enabled on the first page. */
	fe_saves(FE_LW_ROWS_VISIBLE + 2);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int k = 0; k < FE_LW_ROWS_VISIBLE; k++)
		fe_tap(r.down);
	assert(screen_sworld_test_focus() == FE_LW_ROW(FE_LW_ROWS_VISIBLE)
		   && screen_sworld_test_scroll() == FE_LW_ROWS_VISIBLE * FE_LW_ROW_PITCH
		   && "DOWN across the page break did not reveal the next page");
	fe_tap(r.down);
	fe_tap(r.down);
	assert(screen_sworld_test_focus() == FE_LW_PREV
		   && "DOWN from the last row does not reach the enabled page control");
	fe_tap(r.right);
	assert(screen_sworld_test_focus() == FE_LW_PREV
		   && "RIGHT reached the boundary-disabled Next on the last page");
	fe_tap(r.down);
	assert(screen_sworld_test_focus() == FE_LW_PLAY);
	fe_tap(r.right);
	assert(screen_sworld_test_focus() == FE_LW_OPTIONS);
	fe_tap(r.left);
	assert(screen_sworld_test_focus() == FE_LW_PLAY);
	fe_tap(r.up);
	assert(screen_sworld_test_focus() == FE_LW_PREV
		   && "UP from the action row does not return to the page control");
	fe_tap(r.click);
	assert(screen_sworld_test_scroll() == 0
		   && screen_sworld_test_focus() == FE_LW_NEXT
		   && "Prev did not page back, or left the ring on the disabled arrow");

	/* Footer Back (the Close role) returns to the title from anywhere. */
	fe_saves(rows);
	fe_attach(fam);
	fe_open(&screen_select_world);
	fe_tap(r.close);
	assert(fe_screen_last == &screen_title);

	/* NO ROWS (an EXISTING, empty saves directory): the default focus is the
	 * Load tab header; the panel shows the approved "Create a world" action
	 * in the Play slot (100,242 440x44) and NO Play / World options; DOWN from
	 * the header lands on that action, RIGHT does not reach a (non-existent)
	 * options, and activating it opens the Create tab -- never a launch.
	 * (Adjusted from the RED's "Play disabled, nothing opens": root ratified
	 * the empty-state Create action, load-green.) */
	fe_saves(0);
	assert(fe_mkdir(FE_SAVES_DIR) == 0 || errno == EEXIST);
	fe_attach(fam);
	fe_open(&screen_select_world);
	assert(screen_sworld_test_focus() == FE_LW_TAB_LOAD
		   && "an empty Load tab does not open on its header");
	fe_tap(r.down);
	assert(screen_sworld_test_focus() == FE_LW_PLAY);
	fe_tap(r.right);
	assert(screen_sworld_test_focus() == FE_LW_PLAY
		   && "RIGHT reached World options on an empty list");
	fe_tap(r.click);
	assert(fe_screen_last == &screen_create_world
		   && play_session_tab() == PLAY_TAB_CREATE
		   && "the empty list's Create a world did not open the Create tab");
	fe_rmdir(FE_SAVES_DIR);

	/* Delete stays CANCEL-SAFE: it goes through the confirm screen, and
	 * Close there changes nothing. */
	fe_saves(rows);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	fe_load_delete(&r, rows);
	assert(fe_screen_last == &screen_confirm);
	fe_current = &screen_confirm;
	fe_tap(r.close);
	char probe[128];
	snprintf(probe, sizeof(probe), FE_SAVES_DIR "/world00");
	struct stat st;
	assert(stat(probe, &st) == 0
		   && "Cancel on the delete confirmation deleted the world anyway");
}

/* ROW PRESS SELECTS, PLAY LAUNCHES (Load contract): a press on a row -- pad
 * or pointer -- selects it and moves focus to the explicit Play button; it
 * NEVER launches. The selected world's full directory is remembered in the
 * session at that moment. */
static void case_load_row_press_requires_play(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	const int rows = 3;

	play_session_enter_fresh(); /* independent fixture: no retained selection */
	fe_saves(rows);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	fe_tap(r.down); /* row 1 */
	fe_tap(r.click);
	assert(fe_screen_sets == 0 && fe_screen_last != &screen_load_world
		   && "a row press LAUNCHED the world");
	assert(screen_sworld_test_list_selection() == 1
		   && screen_sworld_test_focus() == FE_LW_PLAY
		   && "a row press did not select the row and focus Play");
	const char *name, *directory, *path;
	int64_t la, bs;
	assert(screen_sworld_test_row(1, &name, &directory, &path, &la, &bs) == FE_LW_ROW_OK);
	assert(play_session_world_dir() && !strcmp(play_session_world_dir(), directory)
		   && "selecting a row did not remember its directory in the session");
	fe_tap(r.click); /* Play */
	assert(fe_screen_last == &screen_load_world
		   && "Play did not launch the selected world");

	if(fam == FE_REMOTE) {
		/* The pointer route: a press on row 2 is a focus-only (it is not the
		 * highlighted target), the second selects it and focuses Play, and
		 * the Play button then launches. */
		fe_saves(rows);
		fe_attach(fam);
		fe_open(&screen_select_world);
		for(int f = 0; f < 8; f++)
			fe_step();
		fe_pointer(fe_load_cx(FE_LW_ROW(2)), fe_load_cy(FE_LW_ROW(2)));
		fe_step();
		fe_step();
		fe_tap(r.click);
		fe_tap(r.click);
		assert(fe_screen_sets == 0 && screen_sworld_test_list_selection() == 2
			   && screen_sworld_test_focus() == FE_LW_PLAY
			   && "a pointer press on a row did not select it and focus Play");
		fe_pointer(fe_load_cx(FE_LW_PLAY), fe_load_cy(FE_LW_PLAY));
		fe_step();
		fe_step();
		fe_tap(r.click);
		assert(fe_screen_last == &screen_load_world
			   && "the pointer's Play press did not launch");
		fe_pointer_off();
	}
}

/* RETAINED SELECTION ACROSS A RESET ROUND TRIP: the actual screen, opened,
 * row 1 selected, left for the Create tab (a real screen_set) and re-entered
 * (a real reset that re-enumerates). The SAME DIRECTORY -- not the same index
 * -- is selected again, with focus on it; when the remembered world has been
 * deleted meanwhile the list lands on row 0. A fresh title entry forgets. */
static void case_load_retains_selected_world(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	fe_saves(3);
	play_session_enter_fresh();
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	fe_tap(r.down);
	fe_tap(r.click); /* select row 1, focus Play */
	const char *name, *directory, *path;
	int64_t la, bs;
	assert(screen_sworld_test_row(1, &name, &directory, &path, &la, &bs) == FE_LW_ROW_OK);
	char remembered[256];
	snprintf(remembered, sizeof(remembered), "%s", directory);
	/* DOWN from the action row reaches the header without changing the row. */
	fe_tap(r.down);
	assert(screen_sworld_test_focus() == FE_LW_TAB_LOAD);
	assert(screen_sworld_test_list_selection() == 1);
	assert(play_session_world_dir() && !strcmp(play_session_world_dir(), remembered));
	fe_tap(r.right);
	fe_tap(r.click);
	assert(fe_screen_last == &screen_create_world);
	assert(play_session_world_dir() && !strcmp(play_session_world_dir(), remembered)
		   && "the handoff to the Create tab lost the remembered world");
	/* Re-enumeration with a DIFFERENT index (review item 3, no readdir-order
	 * assumption): the world enumerated BEFORE the remembered one is removed,
	 * so the remembered directory now sits at a smaller index. Identity must
	 * be retained and the index must have changed. */
	{
		const char* before_dir;
		assert(screen_sworld_test_row(0, NULL, &before_dir, NULL, NULL, NULL) == FE_LW_ROW_OK);
		char gone[512];
		snprintf(gone, sizeof(gone), FE_SAVES_DIR "/%s", before_dir);
		assert(fe_rmdir(gone) == 0 && "could not remove the preceding world");
	}
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	size_t sel = screen_sworld_test_list_selection();
	assert(screen_sworld_test_row(sel, &name, &directory, &path, &la, &bs) == FE_LW_ROW_OK
		   && !strcmp(directory, remembered)
		   && "re-entering the Load tab did not re-select the remembered world "
			  "by DIRECTORY after re-enumeration");
	assert(sel != 1 && "the preceding world's removal did not change the index, "
					   "so identity-vs-index is not being distinguished");
	assert(screen_sworld_test_focus() == FE_LW_ROW((int)sel)
		   && "the re-selected world is not the focused row");

	/* The remembered world is DELETED meanwhile: row 0, no crash. */
	{
		char gone[512];
		snprintf(gone, sizeof(gone), FE_SAVES_DIR "/%s", remembered);
		fe_rmdir(gone);
	}
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	assert(screen_sworld_test_list_selection() == 0
		   && "a deleted remembered world did not fall back to row 0");

	/* A FRESH title entry forgets the selection. */
	fe_saves(3);
	fe_attach(fam);
	fe_open(&screen_select_world);
	fe_tap(r.down);
	fe_tap(r.click);
	fe_attach(fam);
	fe_open(&screen_title);
	if(fe_live_pointer) {
		fe_tap(r.down);
		fe_tap(r.up);
	}
	fe_tap(r.click); /* Play game: fresh */
	assert(fe_screen_last == &screen_select_world);
	assert(play_session_world_dir() == NULL);
	fe_attach(fam);
	fe_open(&screen_select_world);
	assert(screen_sworld_test_list_selection() == 0
		   && "a fresh Play entry kept the old selection");
}

/* WORLD OPTIONS is a LOCAL MODAL: it opens on Back, its rows are Rename /
 * Delete / Back, the opening press is consumed by the modal (no press leaks
 * to the list or the actions), Back closes it with nothing changed, and a
 * pointer over the list behind it cannot select or launch. */
/* Approved two-column World options card: Rename/Delete above Back. */
static float fe_modal_row_cx(int row) {
	return row == FE_LW_OPT_RENAME ? 208.0F : 432.0F;
}
static float fe_modal_row_cy(int row) {
	return row == FE_LW_OPT_BACK ? 330.0F : 274.0F;
}

static void case_load_world_options_modal(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	const int rows = 3;

	play_session_enter_fresh(); /* independent fixture: no retained selection */
	fe_saves(rows);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	/* Review item 2: the route to World options walks the rows, so capture
	 * the selection the modal opens ON and assert it never moves. */
	fe_load_options(&r, rows);
	size_t selected = screen_sworld_test_list_selection();
	assert(screen_sworld_test_highlight() == FE_LW_OPT_BACK
		   && "World options did not open focused on Back");
	/* Back: nothing happened, focus returns to World options. */
	fe_tap(r.click);
	assert(fe_screen_sets == 0 && screen_sworld_test_focus() == FE_LW_OPTIONS
		   && screen_sworld_test_list_selection() == selected
		   && "Back on World options changed something");
	/* Reopen; UP once is Delete, UP twice is Rename; Close closes it. */
	fe_tap(r.click);
	fe_tap(r.up);
	assert(screen_sworld_test_highlight() == FE_LW_OPT_DELETE);
	fe_tap(r.up);
	assert(screen_sworld_test_highlight() == FE_LW_OPT_RENAME);
	fe_tap(r.close);
	assert(fe_screen_sets == 0 && screen_sworld_test_focus() == FE_LW_OPTIONS
		   && screen_sworld_test_list_selection() == selected
		   && "Close did not close the modal in place");
	/* Nothing leaked: the selection did not move, no screen opened, and a
	 * DOWN + A while the modal is up does not reach the list or Play. */
	fe_tap(r.click); /* reopen */
	fe_tap(r.down);
	fe_tap(r.down);
	assert(screen_sworld_test_list_selection() == selected && fe_screen_sets == 0
		   && "a press inside the modal reached the list behind it");
	fe_tap(r.close);

	/* CANONICAL RULE INSIDE THE MODAL (load-fix1): UP + A on ONE frame from
	 * the initial Back moves the ring to Delete and opens NOTHING (R-B); the
	 * next lone press then opens the confirmation. */
	fe_tap(r.click); /* reopen, on Back */
	fe_screen_last = NULL;
	fe_tap2(r.up, r.click);
	assert(fe_screen_sets == 0 && fe_screen_last == NULL
		   && "UP + A on one frame opened the Delete confirmation from Back");
	assert(screen_sworld_test_highlight() == FE_LW_OPT_DELETE
		   && "the navigation half of UP + A was swallowed inside the modal");
	fe_tap(r.click);
	assert(fe_screen_last == &screen_confirm
		   && "a lone press on Delete did not open the confirmation");
	fe_current = &screen_confirm;
	fe_tap(r.close); /* cancel: nothing deleted */
	fe_current = &screen_select_world;
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	assert(screen_sworld_test_row_count() == (size_t)rows
		   && "the cancelled confirmation deleted a world");

	/* HELD press: one hold across the modal's focus-only cannot supply the
	 * second press (ruling 2 item 2). */
	fe_load_options(&r, rows);
	selected = screen_sworld_test_list_selection();
	fe_screen_last = NULL;
	fe_screen_sets = 0;
	for(int k = 0; k < 12; k++) {
		fe_hold(r.click); /* the SAME press, never released, on Back */
		fe_current->update(fe_current, FE_DT);
	}
	/* The first frame of the hold is Back (closes the modal); no later frame
	 * of that hold may open anything or move the selection. */
	assert(fe_screen_sets == 0 && screen_sworld_test_list_selection() == selected
		   && "a held press inside the modal activated more than once");
	fe_idle();

	if(fam == FE_REMOTE) {
		/* The pointer rests on Delete while the ring is on Back. Back wins,
		 * closing the modal without opening the destructive confirmation. */
		fe_attach(fam);
		fe_open(&screen_select_world);
		for(int f = 0; f < 8; f++)
			fe_step();
		fe_load_options(&r, rows);
		fe_pointer(fe_modal_row_cx(FE_LW_OPT_DELETE), fe_modal_row_cy(FE_LW_OPT_DELETE));
		fe_step();
		fe_step();
		fe_tap(r.down); /* pad returns to Back; stationary sprite stays on Delete */
		fe_screen_last = NULL;
		fe_screen_sets = 0;
		fe_tap(r.click);
		assert(fe_screen_sets == 0
			   && screen_sworld_test_highlight() != FE_LW_OPT_DELETE
			   && "a resting pointer on Delete overrode the modal Back ring");
		fe_pointer_off();

		/* The pointer over a row BEHIND the modal: consumed by the modal --
		 * neither a selection nor a launch, and the modal stays up. */
		fe_attach(fam);
		fe_open(&screen_select_world);
		for(int f = 0; f < 8; f++)
			fe_step();
		fe_load_options(&r, rows);
		selected = screen_sworld_test_list_selection();
		fe_pointer(fe_load_cx(FE_LW_ROW(1)), fe_load_cy(FE_LW_ROW(1)));
		fe_step();
		fe_step();
		fe_screen_sets = 0;
		fe_tap(r.click);
		fe_tap(r.click);
		assert(screen_sworld_test_list_selection() == selected
			   && fe_screen_sets == 0
			   && screen_sworld_test_highlight() == FE_LW_OPT_BACK
			   && "a pointer press behind the World options modal selected, "
				  "launched, or moved the modal's ring");
		fe_pointer_off();
	}
}

/* GEOMETRY BY POINTER (Remote only, both tables): each Load control's rect
 * is probed at its centre and at its inclusive top-left / exclusive
 * bottom-right edges, and the highlight must be that target inside and the
 * opening focus outside. The title's 88,14 / 26 px text has no hit box and is
 * pinned by capture, not here. */
static void case_load_geometry_pointer(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	assert(gfx_width() == 640 && gfx_height() == 480);
	play_session_enter_fresh(); /* review item 1: no retained row leaks in */

	/* ROW HIT CLIP (load-fix1): the rows are clipped to the drawn list band
	 * 110..300, not the paper panel. After one Next (a 48 px scroll step on
	 * this screen's slotlist) the first row is drawn at 62..108 -- entirely
	 * above the band -- so a press at its old centre (y 100) is INVISIBLE
	 * and must select nothing; a press at y 112 lands on the first row that
	 * is visible. The lower edge: y 299 is inside the band, y 300 is not. */
	{
		fe_saves(FE_LW_ROWS_VISIBLE + 4);
		fe_attach(FE_REMOTE);
		fe_open(&screen_select_world);
		for(int f = 0; f < 8; f++)
			fe_step();
		fe_tap(r.page); /* Next: the canonical page role scrolls one step */
		int scroll = screen_sworld_test_scroll();
		assert(scroll > 0 && "the page role did not scroll the Load list");
		size_t before = screen_sworld_test_list_selection();
		fe_pointer(320.0F, 100.0F); /* row 0's old centre, now clipped out */
		fe_step();
		fe_step();
		fe_screen_sets = 0;
		fe_tap(r.click);
		fe_tap(r.click);
		assert(screen_sworld_test_list_selection() == before && fe_screen_sets == 0
			   && "a press ABOVE the list band selected a clipped-out row");
		fe_pointer(320.0F, 299.0F); /* the band's last inside pixel */
		fe_step();
		fe_step();
		fe_tap(r.click);
		fe_tap(r.click);
		size_t low = screen_sworld_test_list_selection();
		assert(low != before && fe_screen_sets == 0
			   && "a press on the band's last visible pixel selected nothing");
		play_session_enter_fresh(); /* the row just selected is retained */
		fe_attach(FE_REMOTE);
		fe_open(&screen_select_world);
		for(int f = 0; f < 8; f++)
			fe_step();
		fe_pointer(320.0F, 300.0F); /* one past the band */
		fe_step();
		fe_step();
		fe_screen_sets = 0;
		fe_tap(r.click);
		fe_tap(r.click);
		assert(screen_sworld_test_list_selection() == 0 && fe_screen_sets == 0
			   && "a press BELOW the list band selected a clipped-out row");
		fe_pointer_off();
	}
	static const int targets[] = {
		FE_LW_TAB_LOAD, FE_LW_TAB_CREATE, FE_LW_TAB_JOIN, FE_LW_PREV,
		FE_LW_NEXT,     FE_LW_PLAY,       FE_LW_OPTIONS,  FE_LW_ROW(0),
		FE_LW_ROW(1),   FE_LW_ROW(2),     FE_LW_ROW(3),
	};
	for(size_t k = 0; k < sizeof(targets) / sizeof(targets[0]); k++) {
		int t = targets[k];
		struct fe_rect b = fe_load_rect(t);
		/* PIXEL CENTRES, not integer edges. The real IR path normalizes the
		 * pointer to 0..1 floats (input.c: y / 480 -> 254/480 = 0.52916664f)
		 * and input_owner.h multiplies back (253.99998f), so an integer edge
		 * probe can land one ulp OUTSIDE the inclusive edge it means to hit --
		 * rows at 110 / 158 / 206 round-trip exactly, 254 does not. A pixel's
		 * centre (+0.5) never sits on a boundary, so the probe tests the pixel
		 * the player actually points at. The rect contract itself (lower
		 * inclusive / upper exclusive) is pinned at helper level by
		 * fep_rect_hit and is not weakened here. */
		const float inside[][2] = {
			{(float)b.x + 0.5F, (float)b.y + 0.5F},               /* first pixel */
			{(float)(b.x + b.w - 1) + 0.5F, (float)(b.y + b.h - 1) + 0.5F}, /* last */
			{(float)(b.x + b.w / 2) + 0.5F, (float)(b.y + b.h / 2) + 0.5F},
		};
		const float outside[][2] = {
			{(float)(b.x - 1) + 0.5F, (float)(b.y + b.h / 2) + 0.5F},
			{(float)(b.x + b.w) + 0.5F, (float)(b.y + b.h / 2) + 0.5F},
			{(float)(b.x + b.w / 2) + 0.5F, (float)(b.y - 1) + 0.5F},
			{(float)(b.x + b.w / 2) + 0.5F, (float)(b.y + b.h) + 0.5F},
		};
		for(size_t p = 0; p < 3; p++) {
			play_session_enter_fresh(); /* a row selected below is retained */
			fe_saves(FE_LW_ROWS_VISIBLE + 2); /* enough rows to page */
			fe_attach(FE_REMOTE);
			fe_open(&screen_select_world);
			for(int f = 0; f < 8; f++)
				fe_step();
			/* Prev is a BOUNDARY-DISABLED arrow on the first page (approved),
			 * and a disabled control is never followed onto: probe it from
			 * the second page, where it is the enabled one. */
			if(t == FE_LW_PREV)
				fe_tap(r.page);
			fe_pointer(inside[p][0], inside[p][1]);
			fe_step();
			fe_step();
			/* Rows are not followed by hover (Beta's GuiSlot selects on the
			 * click); every other control is. */
			if(t < FE_LW_BUTTON_COUNT)
				assert(screen_sworld_test_focus() == t
					   && "a pointer inside a Load control did not focus it");
			else {
				fe_tap(r.click);
				fe_tap(r.click);
				if(screen_sworld_test_list_selection()
					   != (size_t)(t - FE_LW_BUTTON_COUNT)
				   || fe_screen_sets != 0)
					fprintf(stderr,
							"[geometry] row target %d point %zu (%.0f,%.0f): "
							"selection %zu focus %d sets %d\n",
							t - FE_LW_BUTTON_COUNT, p, inside[p][0], inside[p][1],
							screen_sworld_test_list_selection(),
							screen_sworld_test_focus(), fe_screen_sets);
				assert(screen_sworld_test_list_selection()
						   == (size_t)(t - FE_LW_BUTTON_COUNT)
					   && fe_screen_sets == 0
					   && "a pointer press inside a row did not select it");
			}
		}
		for(size_t p = 0; p < 4; p++) {
			float x = outside[p][0], y = outside[p][1];
			/* A point one pixel outside THIS rect may sit inside a neighbour
			 * (the 2 px between rows is not; Prev/Next and Play/options are
			 * separated). Only points inside NO control are asserted. */
			bool in_other = false;
			for(size_t o = 0; o < sizeof(targets) / sizeof(targets[0]); o++) {
				struct fe_rect ob = fe_load_rect(targets[o]);
				if(x >= (float)ob.x && x < (float)(ob.x + ob.w)
				   && y >= (float)ob.y && y < (float)(ob.y + ob.h))
					in_other = true;
			}
			if(in_other)
				continue;
			play_session_enter_fresh();
			fe_saves(FE_LW_ROWS_VISIBLE + 2);
			fe_attach(FE_REMOTE);
			fe_open(&screen_select_world);
			for(int f = 0; f < 8; f++)
				fe_step();
			fe_pointer(x, y);
			fe_step();
			fe_step();
			assert(screen_sworld_test_focus() == FE_LW_ROW(0)
				   && "a pointer outside every Load control moved the focus");
		}
	}
	fe_pointer_off();
}

/* First launch creates a missing saves directory and offers Create a world.
 * A real enumeration error (a file in the way) must still expose Retry. */
static void case_load_missing_dir_and_file_retry(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	char hint[128];
	int hy, hh, lb, st_;

	fe_saves(0); /* removes FE_SAVES_DIR entirely */
	fe_attach(fam);
	fe_open(&screen_select_world);
	assert(screen_sworld_test_row_count() == 0);
	struct stat st;
	assert(stat(FE_SAVES_DIR, &st) == 0 && S_ISDIR(st.st_mode)
		   && "Load did not create the missing saves directory");
	if(screen_sworld_test_hint(hint, sizeof(hint), &hy, &hh, &lb, &st_))
		assert(!strstr(hint, "Retry")
			   && "a newly created empty saves directory was presented as an error");
	assert(screen_sworld_test_focus() == FE_LW_TAB_LOAD);
	fe_tap(r.down);
	assert(screen_sworld_test_focus() == FE_LW_PLAY);
	fe_tap(r.click);
	assert(fe_screen_last == &screen_create_world
		   && play_session_tab() == PLAY_TAB_CREATE
		   && "first-launch empty list did not offer Create a world");
	fe_saves(0);
	FILE* blocked = fopen(FE_SAVES_DIR, "wb");
	assert(blocked && fclose(blocked) == 0);
	fe_attach(fam);
	fe_open(&screen_select_world);
	assert(screen_sworld_test_row_count() == 0);
	/* The failed enumeration is DISCLOSED in the screen's own status text (the
	 * existing exported hint line) and names the Retry action. */
	assert(screen_sworld_test_hint(hint, sizeof(hint), &hy, &hh, &lb, &st_)
		   && strstr(hint, "Retry")
		   && "a file blocking the saves directory was presented as an empty list");
	assert(screen_sworld_test_focus() == FE_LW_TAB_LOAD);
	/* DOWN from the header lands on Retry (in Play's place). Retry with the
	 * path still blocked keeps the error and opens nothing. */
	fe_tap(r.down);
	assert(screen_sworld_test_focus() == FE_LW_PLAY);
	fe_tap(r.click);
	assert(fe_screen_sets == 0 && screen_sworld_test_row_count() == 0);
	assert(screen_sworld_test_hint(hint, sizeof(hint), &hy, &hh, &lb, &st_)
		   && strstr(hint, "Retry"));
	/* Create the directory with two worlds; Retry -- WITHOUT re-opening the
	 * screen -- lists them. A disabled Play could never do that. */
	assert(remove(FE_SAVES_DIR) == 0);
	fe_saves(2);
	if(fe_live_pointer)
		fe_tap(r.left);
	fe_tap(r.click);
	for(int f = 0; f < 8; f++)
		fe_step();
	assert(screen_sworld_test_row_count() == 2
		   && "Retry did not re-enumerate the saves directory");
	assert(!screen_sworld_test_hint(hint, sizeof(hint), &hy, &hh, &lb, &st_)
		   && "the error status survived a successful Retry");
	/* An EXISTING but EMPTY directory is a genuine "no worlds": no Retry. */
	fe_saves(0);
	assert(fe_mkdir(FE_SAVES_DIR) == 0 || errno == EEXIST);
	fe_attach(fam);
	fe_open(&screen_select_world);
	assert(screen_sworld_test_row_count() == 0);
	if(screen_sworld_test_hint(hint, sizeof(hint), &hy, &hh, &lb, &st_))
		assert(!strstr(hint, "Retry")
			   && "an empty saves directory was presented as an error");
	fe_rmdir(FE_SAVES_DIR);
}

/* Actual production hit rectangles pin the approved four-row Join layout. */
static void case_join_geometry(enum fe_family fam) {
	struct fe_roles r=fe_roles_for(fam);
	fe_servers(9); fe_attach(fam); fe_mps_content(&r);
	struct gutil_button_bounds b;
	for(int i=0;i<4;i++) {
		assert(screen_mps_test_target_rect(15+i,&b));
		assert(b.x==100 && b.y==110+48*i && b.width==440 && b.height==46);
	}
	assert(!screen_mps_test_target_rect(19,&b));
	const int ids[]={2,1,0,9};
	const int xs[]={264,396,100,392}, ys[]={310,310,366,366}, ws[]={128,144,276,148};
	for(int i=0;i<4;i++) {
		assert(screen_mps_test_target_rect(ids[i],&b));
		assert(b.x==xs[i] && b.y==ys[i] && b.width==ws[i] && b.height==44);
	}
	fe_tap(r.page);
	assert(screen_mps_test_page()==1 && screen_mps_test_selection()==0);
	assert(screen_mps_focus_target()==SCREEN_MPS_FOCUS_NEXT);
	assert(!screen_mps_test_target_rect(15,&b));
	assert(screen_mps_test_target_rect(19,&b) && b.y==110);
	fe_tap(r.up); /* reveal retained row */
	assert(screen_mps_test_page()==0 && screen_mps_test_selection()==0);
	fe_mps_action(&r,9);
	assert(screen_mps_test_options_open() && screen_mps_test_focus()==6);
	assert(screen_mps_focus_target()==SCREEN_MPS_FOCUS_BACK);
	for(int i=0;i<4;i++) assert(!screen_mps_test_target_rect(15+i,&b));
	fe_tap2(r.right,r.click); /* Back -> Edit, navigation consumes activation. */
	assert(screen_mps_test_options_open() && screen_mps_test_view()==FE_MPS_MAIN);
	assert(screen_mps_test_focus()==MPS_BUTTON_EDIT_ORD && !fe_screen_sets);
	fe_tap(r.page); /* the local modal consumes page input */
	assert(screen_mps_test_page()==0 && screen_mps_test_selection()==0);
	fe_hold(r.close); fe_current->update(fe_current,FE_DT);
	assert(!screen_mps_test_options_open() && screen_mps_test_focus()==9);
	for(int i=0;i<12;i++) {fe_hold(r.close);fe_current->update(fe_current,FE_DT);}
	assert(!fe_screen_sets && screen_mps_test_focus()==9);
	fe_idle();
	assert(screen_mps_focus_target()==SCREEN_MPS_FOCUS_OPTIONS);
	fe_tap(r.close);
	assert(fe_screen_last==&screen_title);
}

static void case_reach_multiplayer(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	const int rows = 3;

	/* Primary actions and local Server options are reachable by walking. */
	struct {
		int right;
		int expect_view;
		struct screen* opens;
	} targets[] = {
		{1, FE_MPS_DIRECT, NULL},      /* Direct Connect */
		{2, FE_MPS_EDIT_SERVER, NULL}, /* Add Server */
		{3, FE_MPS_EDIT_SERVER, NULL}, /* Edit */
		{4, FE_MPS_MAIN, &screen_confirm}, /* Delete -> confirm */
		{6, FE_MPS_MAIN, &screen_title},   /* Back */
	};
	for(size_t k = 0; k < sizeof(targets) / sizeof(targets[0]); k++) {
		fe_servers(rows);
		fe_attach(fam);
		fe_mps_content(&r);
		fe_mps_action(&r, targets[k].right);
		if(targets[k].opens)
			assert(fe_screen_last == targets[k].opens
				   && "a Multiplayer action is not reachable by direction + activate");
		else
			assert(fe_screen_last == NULL
				   && screen_mps_test_view() == targets[k].expect_view
				   && "a Multiplayer sub-editor is not reachable by "
					  "direction + activate");
	}

	/* Empty content starts on Add and the ring skips disabled Join/Options. */
	fe_servers(0); fe_attach(fam); fe_mps_content(&r);
	assert(screen_mps_test_focus() == MPS_BUTTON_ADD_ORD);
	const int stops[] = {1,6,2};
	for(size_t i=0;i<3;i++) {
		fe_tap(r.right);
		assert(screen_mps_test_focus() == stops[i]);
		assert(screen_mps_test_focus_enabled());
	}
	/* Explicit Join without a name parks in the name editor with its row. */
	fe_servers(rows); fe_attach(fam); fe_mps_content(&r);
	fe_mps_action(&r, MPS_BUTTON_JOIN_ORD);
	char pending[CONFIG_MP_SERVER_ADDRESS_CAP];
	assert(screen_mps_test_view() == FE_MPS_EDIT_NAME
		&& screen_mps_test_pending_join(pending,sizeof(pending))
		&& !strcmp(pending,"10.0.0.1:25565"));
	/* The header's name field opens without any pending connection. */
	fe_servers(rows); fe_attach(fam); fe_mps_content(&r);
	fe_tap(r.up); fe_tap(r.up); fe_tap(r.click);
	assert(screen_mps_test_view() == FE_MPS_EDIT_NAME
		&& !screen_mps_test_pending_join(pending,sizeof(pending)));
	/* A row press selects and focuses Join; only a second explicit press joins. */
	fe_servers(rows); fe_attach(fam); fe_mps_content(&r);
	fe_tap(r.down); fe_tap(r.click);
	assert(screen_mps_test_view() == FE_MPS_MAIN
		&& screen_mps_test_focus() == MPS_BUTTON_JOIN_ORD
		&& !screen_mps_test_pending_join(pending,sizeof(pending)));
	fe_tap(r.click);
	assert(screen_mps_test_view() == FE_MPS_EDIT_NAME
		&& screen_mps_test_pending_join(pending,sizeof(pending))
		&& !strcmp(pending,"10.0.0.2:25565"));
	fe_servers(0); fe_attach(fam); fe_mps_content(&r);
	assert(!screen_mps_test_pending_join(pending,sizeof(pending)));

}

/* ======================================================================== */
/* g. the action strip acts on the SELECTED row (push-0921 A15)              */
/* ======================================================================== */

/* THE DEFECT. Both list screens could be entered ONLY by DOWN off the LAST row
 * (screen_select_world.c's `else if(gui_down)`, and its Multiplayer twin), and
 * that press has already moved the selection there -- so Rename / Delete /
 * Edit could act on the LAST row and on nothing else. On the maintainer's
 * 645-world card the 300th world was unreachable with a controller, and
 * reaching Create cost one press per row. The LEAD RULING (push-0921 A15,
 * implementing Astra ruling 2's MUST item 7 for the selected row): GUI RIGHT
 * from ANY list row enters the strip on its first ENABLED button WITHOUT
 * moving the selection; the way back (UP) returns to that same row; the old
 * entries keep working.
 *
 * Both list screens read LEFT/RIGHT only INSIDE the strip today -- in the list
 * branch they reach nothing but `navigating` -- which is what makes RIGHT the
 * free direction here rather than a stolen one. */
/* The selected middle world remains the action target on all controller tables.
 * Pages now keep the selected row visible; management lives in World options. */
static void case_strip_from_selected_row_sworld(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	char row_name[160], row_path[512];
	fe_saves_big();
	play_session_enter_fresh();
	fe_attach(fam);
	fe_open(&screen_select_world);
	assert(screen_sworld_test_row_count() == FE_BIG_LIST);
	for(int k = 0; k < FE_BIG_ROW; k++)
		fe_tap(r.down);
	assert(screen_sworld_test_gui_selection() == FE_BIG_ROW);
	assert(screen_sworld_test_focus() == FE_LW_ROW(FE_BIG_ROW));
	assert(screen_sworld_test_highlight() == screen_sworld_test_row_target(FE_BIG_ROW));
	const char *name, *directory, *path;
	assert(screen_sworld_test_row(FE_BIG_ROW, &name, &directory, &path, NULL, NULL)
		   == FE_LW_ROW_OK);
	snprintf(row_name, sizeof(row_name), "%s", name);
	snprintf(row_path, sizeof(row_path), "%s", path);
	fe_tap(r.right);
	assert(screen_sworld_test_focus() == FE_LW_PLAY);
	assert(screen_sworld_test_list_selection() == FE_BIG_ROW);
	assert(screen_sworld_test_highlight() == FE_LW_PLAY);
	fe_tap(r.right);
	assert(screen_sworld_test_focus() == FE_LW_OPTIONS);
	fe_tap(r.click);
	assert(screen_sworld_test_highlight() == FE_LW_OPT_BACK && fe_screen_sets == 0);
	fe_tap(r.up);
	fe_tap(r.click);
	assert(fe_screen_last == &screen_confirm);
	assert(screen_confirm_test_highlight() == SCREEN_CONFIRM_ANSWER_NO);
	char armed[512];
	assert(screen_sworld_test_delete_target(armed, sizeof(armed))
		   && !strcmp(armed, row_path));
	fe_current = &screen_confirm;
	fe_tap(r.close);
	assert(fe_screen_last == &screen_select_world);
	fe_current = &screen_select_world;
	assert(screen_sworld_test_row_count() == FE_BIG_LIST);
	struct stat st;
	assert(stat(row_path, &st) == 0);
	assert(screen_sworld_test_gui_selection() == FE_BIG_ROW
		   && "cancelled Delete lost the retained world");

	/* A real reset retains the identity; Rename must target that same row. */
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int k = 0; k < FE_BIG_LIST && screen_sworld_test_row(FE_BIG_ROW,
		NULL,NULL,NULL,NULL,NULL) == 0; k++) fe_step();
	assert(screen_sworld_test_row(FE_BIG_ROW,NULL,NULL,NULL,NULL,NULL) == FE_LW_ROW_OK);
	assert(screen_sworld_test_gui_selection() == FE_BIG_ROW);
	fe_tap(r.right);
	fe_tap(r.right);
	fe_tap(r.click);
	fe_tap(r.up);
	fe_tap(r.up);
	fe_tap(r.click);
	assert(fe_screen_last == &screen_rename_world && fe_rename_setups == 1
		   && !strcmp(fe_rename_path, row_path) && !strcmp(fe_rename_name, row_name));

	/* On a paged list UP crosses the enabled pager, then returns to the row. */
	fe_attach(fam);
	fe_open(&screen_select_world);
	fe_tap(r.right);
	fe_tap(r.up);
	assert(screen_sworld_test_focus() == FE_LW_NEXT);
	fe_tap(r.up);
	assert(screen_sworld_test_focus() == FE_LW_ROW(FE_BIG_ROW)
		   && screen_sworld_test_list_selection() == FE_BIG_ROW);
	fe_tap(r.right);
	fe_tap(r.left);
	assert(screen_sworld_test_focus() == FE_LW_PLAY
		   && screen_sworld_test_gui_selection() == FE_BIG_ROW);

	/* Same-frame navigation consumes activation, but preserves the move. */
	play_session_enter_fresh();
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int k = 0; k < 8; k++) fe_step();
	fe_tap2(r.right, r.click);
	assert(fe_screen_last == NULL && screen_sworld_test_focus() == FE_LW_PLAY);
	fe_tap(r.click);
	assert(fe_screen_last == &screen_load_world);

	fe_saves_small_dir();
	fe_saves(3);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int k = 0; k < 3; k++) fe_tap(r.down);
	assert(screen_sworld_test_focus() == FE_LW_PLAY
		   && screen_sworld_test_gui_selection() == 2);
	/* With no rows the Create header is still reachable without a pointer. */
	fe_saves(0);
	assert(fe_mkdir(FE_SAVES_DIR) == 0 || errno == EEXIST);
	fe_attach(fam);
	fe_open(&screen_select_world);
	fe_tap(r.right);
	assert(screen_sworld_test_focus() == FE_LW_TAB_CREATE);
	fe_tap(r.click);
	assert(fe_screen_last == &screen_create_world);
	fe_saves(3);
}

/* The Multiplayer twin. Its list is capped at CONFIG_MP_SERVER_LIST_MAX (16)
 * rows -- more than the viewport holds, so it pages -- and Edit / Delete are
 * row-gated exactly like Select World's Rename / Delete. BEFORE this card the
 * only button-only entry was DOWN off the last row (UP at row 0 goes to Quick
 * Connect and then the name field, never the strip), so Edit and Delete could
 * act on the LAST server and on no other. */
static void case_strip_from_selected_row_mps(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	/* One short of the cap: at exactly CONFIG_MP_SERVER_LIST_MAX rows the Add
	 * button goes disabled (mps_button_enabled), which changes the strip walk
	 * this case counts presses through -- a different, existing behaviour, not
	 * the one under test. Fifteen rows still overflow the viewport, so the
	 * list pages. */
	const size_t rows = CONFIG_MP_SERVER_LIST_MAX - 1;
	const size_t probes[3] = {0, rows / 2, rows - 1}; /* first, middle, last */

	for(size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
		size_t row = probes[p];
		char want_name[CONFIG_MP_SERVER_NAME_CAP];
		snprintf(want_name, sizeof(want_name), "Server %02zu", row);

		fe_servers(rows);
		fe_attach(fam);
		fe_mps_content(&r);
		assert(screen_mps_test_server_count() == rows);
		for(size_t k = 0; k < row; k++)
			fe_tap(r.down);
		assert(screen_mps_test_selection() == row);
		assert(screen_mps_test_focus() == MPS_FOCUS_LIST_ORD
			   && "the ring left the server list on the way down");

		/* RIGHT: into the strip, selection unmoved, on the first enabled
		 * button (Join, in the strip's own enum order). */
		fe_tap(r.right);
		assert(screen_mps_test_selection() == row
			   && screen_mps_test_list_selection() == row
			   && "RIGHT into the action strip moved the selected server");
		assert(screen_mps_test_focus() == MPS_BUTTON_JOIN_ORD
			   && "RIGHT did not land on the strip's first enabled button");

		/* EDIT in Server options acts on the selected row. */
		fe_mps_focus_action(&r, MPS_BUTTON_EDIT_ORD);
		assert(screen_mps_test_focus() == MPS_BUTTON_EDIT_ORD);
		fe_tap(r.click);
		size_t index = (size_t)-1;
		bool is_new = true;
		char got_name[CONFIG_MP_SERVER_NAME_CAP];
		char got_address[CONFIG_MP_SERVER_ADDRESS_CAP];
		assert(screen_mps_test_view() == FE_MPS_EDIT_SERVER
			   && "Edit did not open the server editor");
		assert(screen_mps_test_edit_target(&index, &is_new, got_name,
										   sizeof(got_name), got_address,
										   sizeof(got_address)));
		assert(index == row && !is_new && !strcmp(got_name, want_name)
			   && "Edit opened on a different server than the selected row");
		fe_step(); /* parent sees neutral input before the next deliberate press */
		fe_tap(r.close);
		assert(screen_mps_test_view() == FE_MPS_MAIN
			   && "the server editor did not close");

		/* DELETE in Server options acts on the same row. */
		fe_mps_focus_action(&r, MPS_BUTTON_DELETE_ORD);
		assert(screen_mps_test_focus() == MPS_BUTTON_DELETE_ORD);
		assert(screen_mps_test_selection() == row);
		fe_tap(r.click);
		assert(fe_screen_last == &screen_confirm
			   && "Delete did not open the confirmation");
		assert(screen_confirm_test_highlight() == 1
			   && "the remove confirmation did not open on its safe answer");
		assert(screen_mps_test_delete_index() == row
			   && "Delete armed a different server than the selected row");
		fe_current = &screen_confirm;
		fe_tap(r.close);
		assert(fe_screen_last == &screen_multiplayer_setup_impl);
		fe_current = &screen_multiplayer_setup_impl;
		assert(screen_mps_test_server_count() == rows
			   && "a cancelled Delete changed the server count");
		char still_name[CONFIG_MP_SERVER_NAME_CAP];
		char still_address[CONFIG_MP_SERVER_ADDRESS_CAP];
		assert(screen_mps_test_server(row, still_name, sizeof(still_name),
									  still_address, sizeof(still_address))
			   && !strcmp(still_name, want_name)
			   && "a cancelled Delete removed the selected server");

		/* THE WAY BACK, on this screen too. */
		fe_attach(fam);
		fe_mps_content(&r);
		assert(screen_mps_test_selection() == row);
		fe_tap(r.right);
		fe_tap(r.up);
		assert(screen_mps_test_focus() == MPS_FOCUS_LIST_ORD
			   && screen_mps_test_selection() == row
			   && "UP out of the strip lost the row the player had selected");

		/* Navigation + activate in one frame still decides nothing here. */
		fe_attach(fam);
		fe_mps_content(&r);
		fe_tap2(r.right, r.click);
		assert(screen_mps_test_view() == FE_MPS_MAIN && fe_screen_last == NULL
			   && "RIGHT + activate in one frame activated something");
		assert(screen_mps_test_focus() == MPS_BUTTON_JOIN_ORD
			   && "the RIGHT half of a navigate+activate frame was swallowed");
	}

	/* DOWN off the last row enters Join; UP at row 0 reaches the header. */
	fe_servers(3);
	fe_attach(fam);
	fe_mps_content(&r);
	for(int k = 0; k < 3; k++)
		fe_tap(r.down);
	assert(screen_mps_test_focus() == MPS_BUTTON_JOIN_ORD
		   && screen_mps_test_selection() == 2
		   && "DOWN off the last row stopped entering the action strip");
	fe_servers(3);
	fe_attach(fam);
	fe_mps_content(&r);
	fe_tap(r.up);
	assert(screen_mps_test_focus() == 12
		   && "UP at row 0 stopped reaching the Join header");

	/* An EMPTY list: RIGHT skips the row-gated buttons, exactly like DOWN. */
	fe_servers(0);
	fe_attach(fam);
	fe_mps_content(&r);
	fe_tap(r.right);
	assert(screen_mps_test_focus() == MPS_BUTTON_DIRECT_ORD
		   && "RIGHT on an empty server list did not skip the disabled "
			  "buttons");
}

/* REVIEW A16-7: entering the strip must leave the selected row VISIBLE.
 *
 * The canonical page roles move the VIEWPORT and deliberately keep the
 * selection (case_page_roles_select_world pins that), so the two can be far
 * apart. Before A15 the only button-only way into the strip was DOWN off the
 * last row, which MOVED the selection and therefore revealed it; RIGHT does
 * not move it, so it has to reveal it -- otherwise Delete and Rename arm a
 * world that is nowhere on screen. */
static void case_strip_entry_reveals_the_row(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	fe_saves_big();
	play_session_enter_fresh();
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int k = 0; k < FE_BIG_ROW; k++) fe_tap(r.down);
	assert(screen_sworld_test_gui_selection() == FE_BIG_ROW);
	assert(screen_sworld_test_scroll() == (FE_BIG_ROW / 4) * 192);
	/* Full-page navigation changes selection to the shown page, so no action
	 * can secretly target an off-screen world. Verify both directions. */
	fe_tap(r.previous);
	size_t selected = FE_BIG_ROW - 4;
	assert(screen_sworld_test_gui_selection() == selected
		   && screen_sworld_test_list_selection() == selected);
	assert(screen_sworld_test_scroll() == (int)(selected / 4) * 192);
	fe_tap(r.page);
	selected = FE_BIG_ROW;
	assert(screen_sworld_test_gui_selection() == selected);
	fe_tap(r.right);
	assert(screen_sworld_test_focus() == FE_LW_PLAY
		   && screen_sworld_test_list_selection() == selected
		   && screen_sworld_test_scroll() == (int)(selected / 4) * 192);
	const char *name, *directory, *path;
	assert(screen_sworld_test_row(selected, &name, &directory, &path, NULL, NULL)
		   == FE_LW_ROW_OK);
	char row_path[512];
	snprintf(row_path, sizeof(row_path), "%s", path);
	fe_tap(r.right);
	fe_tap(r.click);
	fe_tap(r.up);
	fe_tap(r.click);
	char armed[512];
	assert(fe_screen_last == &screen_confirm
		   && screen_sworld_test_delete_target(armed, sizeof(armed))
		   && !strcmp(armed, row_path));
	fe_current = &screen_confirm;
	fe_tap(r.close);
	fe_current = &screen_select_world;
	assert(screen_sworld_test_list_selection() == selected);
	fe_saves_small_dir();
	int guard;

	/* The Multiplayer twin, on its own 15-row list. */
	const size_t rows = CONFIG_MP_SERVER_LIST_MAX - 1;
	fe_servers(rows);
	fe_attach(fam);
	fe_mps_content(&r);
	for(size_t k = 0; k + 1 < rows; k++)
		fe_tap(r.down);
	assert(screen_mps_test_selection() == rows - 1);
	guard = 0;
	while(screen_mps_test_scroll() > 0 && guard++ < 4000)
		fe_tap(r.previous);
	assert(screen_mps_test_scroll() == 0);
	assert(screen_mps_test_selection() == rows - 1);
	assert(screen_mps_test_page() != (rows-1)/4);
	fe_tap(r.up); /* paging control -> retained row, revealed */
	fe_tap(r.right);
	assert(screen_mps_test_selection() == rows - 1
		   && screen_mps_test_list_selection() == rows - 1
		   && "revealing the row moved the selection");
	assert(screen_mps_test_focus() == MPS_BUTTON_JOIN_ORD);
	assert(screen_mps_test_page() == (rows-1)/4
		&& "action entry did not reveal the retained server");
}

/* REVIEW A16-10: Texture Packs' strip was pointer-only while run_family's
 * banner claimed every enabled action on all three list screens is reachable
 * by direction + activate. It now has the same RIGHT entry -- and this case is
 * what backs the banner for this screen.
 *
 * The strip is two buttons: "Open texture pack folder" (a NAMED parity gap,
 * permanently disabled -- there is no OS folder-open on a Wii) and Done. So
 * the observable claims are: RIGHT lands on Done and not on the disabled one,
 * the selected pack does not move, LEFT clamps instead of falling back into
 * the list, and an activate from there leaves the screen WITHOUT applying the
 * selected pack (which is what a fall-through to the row would have done). */
static void case_reach_texture_packs(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	char active_before[128];

	fe_attach(fam);
	screen_texturepacks_set_return(&screen_title);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(FE_LONG_LIST);
	assert(screen_texpacks_test_pack_count() == FE_LONG_LIST + 1);
	snprintf(active_before, sizeof(active_before), "%s",
			 config_read_string(&gstate.config_user, "paths.texturepack", ""));

	/* Select a pack that is NOT the active one, so "the strip did not apply
	 * the selected row" is a statement with something behind it. */
	fe_tap(r.down);
	size_t selected = screen_texpacks_test_selection();
	assert(selected == 1);

	fe_tap(r.right);
	assert(screen_texpacks_test_selection() == selected
		   && "RIGHT into the strip moved the selected pack");
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_CATEGORY
		   && "RIGHT from a row does not enter the strip on the category "
			  "selector");
	/* LEFT at the leftmost ENABLED button clamps -- it must not fall into the
	 * list. With nothing pending Apply is disabled, so the walk from the
	 * category selector skips it and lands on Done. */
	fe_tap(r.left);
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_CATEGORY);
	assert(screen_texpacks_test_selection() == selected);
	assert(!screen_texpacks_test_apply_enabled()
		   && "Apply is live with nothing selected");
	fe_tap(r.right);
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_DONE
		   && "a disabled Apply was not skipped by the strip walk");
	fe_tap(r.click);
	assert(fe_screen_last == &screen_title
		   && "Done is not reachable by direction + activate on Texture Packs");
	assert(!strcmp(config_read_string(&gstate.config_user,
									  "paths.texturepack", ""),
				   active_before)
		   && "the strip activate applied the selected pack instead of Done");

	/* UP/DOWN still belong to the LIST: they take the ring back out of the
	 * strip, and an activate then reaches the selected PACK (it applies it)
	 * instead of leaving the screen -- which is the discriminator, since a
	 * ring left on Done would have gone to the title.
	 *
	 * Row 0 is the ACTIVE pack (screen_texpacks_reset pre-selects whichever is
	 * live), so what this applies is what is already applied: the case is
	 * about WHERE the ring was, and it must leave nothing behind. The deferred
	 * apply is then DRAINED here rather than left armed for the next case --
	 * screen_texpacks_reset does not clear apply_pending_frames, so an armed
	 * one would fire inside whatever case opened this screen next. */
	fe_attach(fam);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(FE_LONG_LIST);
	assert(screen_texpacks_test_selection() == 0);
	fe_tap(r.right);
	fe_tap(r.up);
	fe_tap(r.click);
	assert(fe_screen_last == NULL
		   && "UP out of the strip did not return the ring to the pack list");
	/* The row press RETAINS the choice as PENDING and applies nothing: the
	 * active pack is untouched, and no deferred apply is left armed for the
	 * next case (which is what the four steps below would have run). */
	assert(screen_texpacks_test_pending() == 0
		   && "a row press did not record the pending selection");
	for(int f = 0; f < 4; f++)
		fe_step();
	assert(!strcmp(config_read_string(&gstate.config_user,
									  "paths.texturepack", ""),
				   active_before)
		   && "a row press applied the pack instead of only selecting it");
}

/* Astra gate 1 item 7, on all three controller families: a press on a pack ROW
 * SELECTS it and does nothing else; the pack is applied only by an EXPLICIT
 * Apply on the action strip. The strip the ruling sizes is three controls --
 * the category selector (Textures/Sounds), Apply, Done -- so from a list row
 * RIGHT enters the strip on the CATEGORY button, a second RIGHT reaches Apply
 * and a third reaches Done. A pack pick focuses Apply; LEFT then reaches
 * Category without arming the apply.
 *
 * WHAT IS OBSERVABLE HEADLESS, and why the assertions are shaped this way:
 * this target is built with PLATFORM_WII, so texturepacks_apply stats
 * "sd:/texturepacks/<pack>" and no host has one -- an apply here always FAILS,
 * and paths.texturepack therefore cannot witness "did it apply". What can:
 * screen_texpacks_update FREEZES the screen while an apply is armed
 * (apply_pending_frames > 0 returns before any navigation), so a DOWN on the
 * frame after an activation is swallowed exactly when that activation armed an
 * apply. That freeze is the probe used below, in both directions -- it must be
 * absent after a ROW press and present after an APPLY press. No new symbol is
 * introduced for this stage. */
static void case_pack_row_selects_apply_applies(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	/* The pack in force when the case begins: every "nothing was applied"
	 * assertion below is against THIS, across all three sub-sequences. */
	char active_before[128];
	snprintf(active_before, sizeof(active_before), "%s",
			 config_read_string(&gstate.config_user, "paths.texturepack", ""));

	/* --- a press on a ROW selects it, and arms nothing ---------------------- */
	fe_attach(fam);
	screen_texturepacks_set_return(&screen_title);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(FE_LONG_LIST);
	assert(screen_texpacks_test_pack_count() == FE_LONG_LIST + 1);
	assert(screen_texpacks_test_selection() == 0);

	fe_tap(r.down);
	assert(screen_texpacks_test_selection() == 1);
	fe_tap(r.click); /* activate the selected row */
	assert(fe_screen_last == NULL
		   && "a press on a pack row left the screen");
	assert(screen_texpacks_test_selection() == 1
		   && "a press on a pack row moved the selection off it");
	/* THE CLAIM: nothing was armed, so the very next press still navigates. */
	fe_tap(r.down);
	assert(screen_texpacks_test_selection() == 2
		   && "a press on a pack row applied it instead of only selecting it");

	/* --- the strip: category, Apply, Done ---------------------------------- */
	fe_attach(fam);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(FE_LONG_LIST);
	fe_tap(r.down);
	assert(screen_texpacks_test_selection() == 1);
	fe_tap(r.click); /* the pending TEXTURE choice */
	assert(screen_texpacks_test_pending() == 1);
	assert(screen_texpacks_test_apply_enabled()
		   && "Apply stayed disabled with a pending choice that differs from "
			  "the current pack");

	fe_tap(r.left); /* from Apply to the CATEGORY selector */
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_CATEGORY);
	fe_tap(r.click);
	assert(fe_screen_last == NULL
		   && "the first strip button is not the category selector -- the "
			  "press left the screen, so RIGHT landed on Done");
	assert(screen_texpacks_test_category() == FE_TP_CAT_SOUNDS
		   && "the category selector did not switch to Sounds");
	/* The Sounds list is a DIFFERENT list (only packs that ship audio), so the
	 * texture row index means nothing here -- which is why the assertion about
	 * the remembered texture row is made after toggling BACK (review
	 * correction, RED-ACCEPTED.md). Switching category applies nothing. */
	assert(!strcmp(config_read_string(&gstate.config_user,
									  "paths.texturepack", ""),
				   active_before)
		   && "switching category applied the pending pack");
	fe_tap(r.click);
	assert(screen_texpacks_test_category() == FE_TP_CAT_TEXTURES);
	assert(screen_texpacks_test_pending() == 1
		   && "the Textures category forgot its pending row across a toggle");
	assert(screen_texpacks_test_selection() == 1
		   && "the ring did not return to the remembered pending row");

	fe_tap(r.right); /* Apply */
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_APPLY
		   && "the second strip control is not Apply");
	fe_tap(r.click);
	assert(fe_screen_last == NULL && "Apply left the screen");
	/* Apply DID arm the deferred apply: the screen is frozen behind the
	 * loading strip, so this DOWN is swallowed. */
	fe_tap(r.down);
	assert(screen_texpacks_test_selection() == 1
		   && "Apply did not arm the deferred apply (the screen kept "
			  "navigating)");
	/* Drain it rather than leave it armed for the next case (the deferred
	 * apply fires on whichever screen owns the frame -- see
	 * case_reach_texture_packs' note). The apply itself FAILS here: the seeded
	 * rows name no directory any host has (see this case's header), so what is
	 * proven is the arming, not a load. */
	for(int f = 0; f < 4; f++)
		fe_step();

	/* --- Done is the THIRD control, and discards the pending choice -------- */
	fe_attach(fam);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(FE_LONG_LIST);
	assert(screen_texpacks_test_pending() == FE_TP_NO_PENDING
		   && "entering the screen kept a pending selection from last time");
	fe_tap(r.down);
	fe_tap(r.click); /* pending again, with focus on Apply */
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_APPLY);
	fe_tap(r.left); /* Category, the first strip control */
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_CATEGORY);
	fe_tap(r.right);
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_APPLY);
	fe_tap(r.right);
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_DONE
		   && "Done is not the third strip control");
	fe_tap(r.click);
	assert(fe_screen_last == &screen_title);
	assert(!strcmp(config_read_string(&gstate.config_user,
									  "paths.texturepack", ""),
				   active_before)
		   && "Done applied the pending selection instead of discarding it");
	/* Re-entering starts clean: Done discarded it (gate 1 item 7). */
	fe_attach(fam);
	fe_open(&screen_texturepacks);
	assert(screen_texpacks_test_pending() == FE_TP_NO_PENDING
		   && "Done did not discard the unapplied selection");
}

/* A pending row further down the list must still be VISIBLE when the player
 * comes back to it. The Sounds list is much shorter, so entering it clamps the
 * scroll to that list's bottom; restoring the Textures list used to put the
 * highlight back on the remembered row without moving the viewport to it, so
 * the ring was off-screen (screen review 2). Nothing may be applied on the way.
 *
 * Visibility uses widget_slotlist's own row placement, with THIS screen's
 * viewport -- which gate 2 moves to the approved composition
 * (front-end-refined/studio.js:77-85, proofs/earth--packs--default.png): in
 * 640x480 at GUI scale 2 the pack list runs top 94 to bottom 312, its rows are
 * drawn at 102 / 172 / 242 (slotlist's own 4-unit inset below the top, then a
 * 70 px pitch) and each row's action art is 60 px tall inside that pitch. The
 * three other list screens keep their 32-unit / 36-unit geometry untouched.
 * The real bottom edge is still read back from the screen rather than assumed.
 *
 * These are PIXELS, not GUI units: the approved package composes in 640x480
 * directly, and the whole point of the pin is that the numbers match the
 * proof. case_nav_hint asserts the surface really is 640x480 at scale 2. */
#define FE_TP_LIST_TOP_PX 94
#define FE_TP_LIST_BOTTOM_PX 312
#define FE_TP_ROW_PITCH_PX 70
#define FE_TP_ROW_ACTION_PX 60
#define FE_TP_FOOTER_TOP_PX 428 /* 10-31: the shared hint row strip top (480-24-28), was 432 */
/* The drawn top of row `i`: the viewport top plus slotlist's 4-unit inset
 * (94 + 8 = 102), then the pitch -- 102, 172, 242 for the first three. */
#define FE_TP_ROW_TOP_PX(i) \
	(FE_TP_LIST_TOP_PX + 4 * GFX_GUI_SCALE + (int)(i) * FE_TP_ROW_PITCH_PX)

static bool fe_tp_row_visible(int scroll, size_t row) {
	int top = FE_TP_LIST_TOP_PX;
	int bottom = screen_texpacks_test_list_bottom();
	int row_h = FE_TP_ROW_PITCH_PX;
	int y = top + 4 * GFX_GUI_SCALE - scroll + (int)row * row_h;
	return y + row_h > top && y < bottom;
}

static void case_pack_pending_row_stays_visible(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	char active_before[128];
	snprintf(active_before, sizeof(active_before), "%s",
			 config_read_string(&gstate.config_user, "paths.texturepack", ""));

	fe_attach(fam);
	screen_texturepacks_set_return(&screen_title);
	screen_texturepacks_set_ingame(false);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(24);			  /* a list that really pages */
	screen_texpacks_test_seed_audio(1, true); /* one eligible sound pack */

	/* Walk to a row well below the first page and select it. */
	for(int k = 0; k < 20; k++)
		fe_tap(r.down);
	size_t deep = screen_texpacks_test_selection();
	assert(deep == 20 && "the list did not walk to row 20");
	fe_tap(r.click);
	assert(screen_texpacks_test_pending() == 20);
	assert(fe_tp_row_visible(screen_texpacks_test_scroll(), deep)
		   && "the row the player just selected is off-screen");

	/* Into the short Sounds list (clamps the scroll) and straight back. */
	fe_tap(r.left); /* Apply -> Category */
	fe_tap(r.click);
	assert(screen_texpacks_test_category() == FE_TP_CAT_SOUNDS);
	fe_tap(r.click);
	assert(screen_texpacks_test_category() == FE_TP_CAT_TEXTURES);

	assert(screen_texpacks_test_pending() == 20
		   && "the Textures category forgot its pending row");
	assert(screen_texpacks_test_selection() == deep
		   && "the ring did not return to the remembered row");
	assert(fe_tp_row_visible(screen_texpacks_test_scroll(), deep)
		   && "the remembered pending row came back off-screen");
	assert(!strcmp(config_read_string(&gstate.config_user, "paths.texturepack",
									  ""),
				   active_before)
		   && "the category round trip applied the pending pack");
	assert(fe_screen_last == NULL);
}

/* The bounded per-row PREVIEW cache (gate 2). What this case can honestly
 * assert, given the doubles above decode nothing, is the LIFECYCLE: which root
 * each row asked for, that an unchanged viewport asks again for nothing, that a
 * pack with no image is remembered as a miss, that scrolling and a category
 * change evict, and that every free is bracketed by the GPU fence. Image
 * content and the real top-left-cell downsample are texture.c's and are NOT
 * covered here -- disclosed, not faked. */
size_t screen_texpacks_test_preview_slots(void);
size_t screen_texpacks_test_preview_pack(size_t slot);
bool screen_texpacks_test_preview_loaded(size_t slot);

/* The root the screen MUST ask for, for pack `idx`: that row's own installed
 * value -- row 0 is the bundled default, the seeded rows are "texturepacks/
 * packNN". Never the active pack's config key. */
static void fe_tp_expect_root(size_t idx, char* out, size_t cap) {
	if(idx == 0)
		snprintf(out, cap, "sd:/assets");
	else
		snprintf(out, cap, "sd:/texturepacks/pack%02zu", idx - 1);
}

static bool fe_tp_root_logged(const char* root) {
	int n = fe_tp_preview_calls < FE_TP_PREVIEW_LOG ? fe_tp_preview_calls
													: FE_TP_PREVIEW_LOG;
	for(int k = 0; k < n; k++)
		if(!strcmp(fe_tp_preview_root[k], root))
			return true;
	return false;
}

static size_t fe_tp_preview_occupied(void) {
	size_t used = 0;
	for(size_t s = 0; s < screen_texpacks_test_preview_slots(); s++)
		if(screen_texpacks_test_preview_pack(s) < FE_TP_NO_PENDING)
			used++;
	return used;
}

static void fe_tp_preview_reset_log(void) {
	fe_tp_preview_calls = 0;
	fe_tp_fence_pre = 0;
	fe_tp_fence_post = 0;
	fe_tp_preview_missing = NULL;
}

static void case_pack_row_previews(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	char root[128];

	fe_tp_preview_reset_log();
	fe_attach(fam);
	screen_texturepacks_set_return(&screen_title);
	screen_texturepacks_set_ingame(false);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(24); /* a list that really pages */
	fe_step();					   /* the sync runs from update, never render */

	/* BOUNDED: at most the four slots exist, and one load per occupied slot --
	 * a 25-row list does not decode 25 images. */
	size_t used = fe_tp_preview_occupied();
	assert(used > 0 && "the visible rows asked for no preview at all");
	assert(used <= screen_texpacks_test_preview_slots());
	assert(fe_tp_preview_calls == (int)used
		   && "the preview cache loaded more images than it kept slots for");

	/* IDENTITY: each slot asked for ITS OWN row's installed root. */
	for(size_t s = 0; s < screen_texpacks_test_preview_slots(); s++) {
		size_t idx = screen_texpacks_test_preview_pack(s);
		if(idx >= FE_TP_NO_PENDING)
			continue;
		fe_tp_expect_root(idx, root, sizeof(root));
		assert(fe_tp_root_logged(root)
			   && "a preview slot did not ask for its own pack's root");
	}
	assert(fe_tp_fence_pre == fe_tp_fence_post
		   && "an unbalanced GPU fence around the first loads");

	/* UNCHANGED VIEWPORT: more frames, no I/O. (render2D is not callable in
	 * this rig; the point asserted here is that the cache is idle per update,
	 * which is the frame cost that would otherwise repeat.) */
	int calls_settled = fe_tp_preview_calls;
	for(int k = 0; k < 5; k++)
		fe_step();
	assert(fe_tp_preview_calls == calls_settled
		   && "a frame with the same visible rows re-decoded previews");

	/* SCROLL: rows that left are evicted, rows that arrived are loaded, and
	 * the cache never grows past its slots. */
	for(int k = 0; k < 20; k++)
		fe_tap(r.down);
	fe_step();
	assert(fe_tp_preview_calls > calls_settled
		   && "scrolling a paging list loaded no new preview");
	assert(fe_tp_preview_occupied() <= screen_texpacks_test_preview_slots());
	for(size_t s = 0; s < screen_texpacks_test_preview_slots(); s++)
		assert(screen_texpacks_test_preview_pack(s) != 0
			   && "the first row's preview survived scrolling 20 rows away");
	assert(fe_tp_fence_pre == fe_tp_fence_post
		   && "eviction left the GPU fence unbalanced");

	/* CATEGORY CHANGE: the whole cache is dropped (the other list is other
	 * packs), fenced, and slots come back free. */
	fe_tap(r.right);
	fe_tap(r.click);
	assert(screen_texpacks_test_category() == FE_TP_CAT_SOUNDS);
	assert(fe_tp_fence_pre == fe_tp_fence_post
		   && "the category change freed preview images outside the fence");

	/* MISS CACHING: a pack whose folder ships no usable image is remembered as
	 * a miss -- the screen must not retry the decode every frame. */
	fe_tp_preview_reset_log();
	fe_tp_expect_root(1, root, sizeof(root));
	fe_tp_preview_missing = root;
	fe_attach(fam);
	screen_texturepacks_set_ingame(false);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(24);
	fe_step();
	bool saw_miss = false;
	for(size_t s = 0; s < screen_texpacks_test_preview_slots(); s++)
		if(screen_texpacks_test_preview_pack(s) == 1) {
			saw_miss = true;
			assert(!screen_texpacks_test_preview_loaded(s)
				   && "a pack with no image reported a loaded preview");
		}
	assert(saw_miss && "the missing-image pack was never visible");
	int calls_with_miss = fe_tp_preview_calls;
	for(int k = 0; k < 5; k++)
		fe_step();
	assert(fe_tp_preview_calls == calls_with_miss
		   && "a pack with no preview was re-opened every frame");
	fe_tp_preview_missing = NULL;
	assert(fe_screen_last == NULL);
}

/* The SOUNDS half, on all three families: the second category is a list of its
 * own (only packs that ship loadable audio), it keeps its own pending row, and
 * it is UNREACHABLE while a world is loaded -- the live sound and music sets
 * are not swapped under a running world, and a choice recorded as "current"
 * that only took effect next launch would be a lie (gate 1 item 6). */
static void case_pack_sound_category(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	fe_attach(fam);
	screen_texturepacks_set_return(&screen_title);
	screen_texturepacks_set_ingame(false);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(4);		   /* texture-only rows */
	screen_texpacks_test_seed_audio(2, true); /* two eligible sound packs */
	size_t all_rows = screen_texpacks_test_row_count();
	assert(all_rows == 1 + 4 + 2 && "the Textures list dropped installed packs");

	/* A pending TEXTURE choice, then into Sounds. */
	fe_tap(r.down);
	fe_tap(r.click);
	assert(screen_texpacks_test_pending() == 1);
	fe_tap(r.left); /* Apply -> Category */
	fe_tap(r.click);
	assert(screen_texpacks_test_category() == FE_TP_CAT_SOUNDS);
	/* Only the bundled default plus the two audio-carrying packs: a
	 * texture-only pack is not an audio choice, so offering it would be an
	 * apply that could only fail. */
	assert(screen_texpacks_test_row_count() == 3
		   && "the Sounds list is not restricted to packs that ship audio");
	assert(screen_texpacks_test_pending() == FE_TP_NO_PENDING
		   && "the Sounds category inherited the Textures pending choice");

	/* Its own pending row, kept across a toggle in both directions. */
	fe_tap(r.up); /* back into the list */
	fe_tap(r.down);
	fe_tap(r.click);
	size_t sound_pending = screen_texpacks_test_pending();
	assert(sound_pending != FE_TP_NO_PENDING);
	fe_tap(r.left); /* Apply -> Category */
	fe_tap(r.click); /* -> Textures */
	assert(screen_texpacks_test_pending() == 1);
	fe_tap(r.click); /* -> Sounds */
	assert(screen_texpacks_test_pending() == sound_pending
		   && "the Sounds category forgot its own pending row");

	/* IN-GAME: the selector is a DISABLED target, so the directional walk
	 * skips it exactly as it skips a disabled Apply -- RIGHT from the list
	 * cannot land on it at all, and Sounds is therefore unreachable while a
	 * world is loaded. Asserted on the traversal, not by pressing: with
	 * nothing pending the ring here rests on Done, and activating Done is a
	 * legitimate leave, so a press would prove nothing about the selector. */
	fe_attach(fam);
	screen_texturepacks_set_ingame(true);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(4);
	assert(screen_texpacks_test_category() == FE_TP_CAT_TEXTURES);
	fe_tap(r.right);
	assert(screen_texpacks_test_highlight() != FE_TP_BTN_CATEGORY
		   && "the Sounds selector is focusable while a world is loaded");
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_DONE);
	fe_tap(r.left);
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_DONE
		   && "LEFT fell onto the disabled category selector");
	assert(screen_texpacks_test_category() == FE_TP_CAT_TEXTURES);
	assert(screen_texpacks_test_pending() == FE_TP_NO_PENDING);
	assert(fe_screen_last == NULL);
	screen_texturepacks_set_ingame(false);
}

/* GATE 2, the approved pack ROW is a full-width card, and the pointer must
 * reach ALL of it. studio.js:77-85 draws each row at x 48, width 544 -- so
 * 48..592 in 640x480 -- while widget_slotlist's shared hit test is Beta's
 * GuiSlot band, width/2 +/- 110 units = x 100..540 at scale 2. The approved
 * card therefore has 52 px of dead art down its left edge and 52 px down its
 * right: a player aiming at the pack tile (which is drawn at the row's LEFT
 * edge) or at the "Current" badge (its RIGHT edge) presses on the row and
 * nothing happens. This case presses at x 60 and x 570 -- both inside the
 * approved card, both outside the old band -- at the drawn MIDDLE of rows 1
 * and 2 (172+30 = 202, 242+30 = 272), which is also what pins the 94 top and
 * the 70 pitch that no seam exports.
 *
 * REMOTE ONLY, like every other pointer case in this suite: input_owner_latch
 * refuses an IR pointer to the Classic and the GameCube has none, so this is
 * the one family on which a pointer press exists to be measured. The three
 * families' DIRECTIONAL coverage of this screen is unchanged and still runs in
 * run_family.
 *
 * WHAT A PRESS DOES: the hovered row is not the highlighted one, so
 * gutil_menu_activate_decide returns FOCUS_ONLY (gui_util.c:320-333) and the
 * screen moves gui_selection to it. Selecting is ALL it may do -- the gate 1
 * item 7 transaction is untouched here: nothing is applied and nothing is
 * armed, asserted the same way case_pack_row_selects_apply_applies asserts it
 * (the next press still navigates, and paths.texturepack never moves). */
static void case_pack_row_pointer_reach(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	char active_before[128];
	snprintf(active_before, sizeof(active_before), "%s",
			 config_read_string(&gstate.config_user, "paths.texturepack", ""));

	fe_attach(FE_REMOTE);
	screen_texturepacks_set_return(&screen_title);
	screen_texturepacks_set_ingame(false);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(FE_LONG_LIST);
	assert(screen_texpacks_test_pack_count() == FE_LONG_LIST + 1);
	assert(screen_texpacks_test_selection() == 0);
	assert(screen_texpacks_test_scroll() == 0
		   && "the row-index arithmetic below assumes an unscrolled viewport");

	/* Near the LEFT edge of the approved card: x 60 is 12 px inside it and 40
	 * px outside the old 100..540 band. Row 1's drawn middle. */
	fe_pointer(60.0F, (float)(FE_TP_ROW_TOP_PX(1) + FE_TP_ROW_ACTION_PX / 2));
	fe_step();
	fe_tap(r.click);
	assert(fe_screen_last == NULL && "a press on a pack row left the screen");
	assert(screen_texpacks_test_selection() == 1
		   && "a press at the LEFT of the approved pack card (x 60, row 1) did "
			  "not select that row -- the hit test is still Beta's 100..540 "
			  "band, not the 48..592 card studio.js:79 draws");

	/* Near the RIGHT edge -- x 570, 22 px inside the card and 30 px outside the
	 * old band -- on the NEXT row down, so "it selected SOME row" cannot pass
	 * for "it selected the right one". */
	fe_pointer(570.0F, (float)(FE_TP_ROW_TOP_PX(2) + FE_TP_ROW_ACTION_PX / 2));
	fe_step();
	fe_tap(r.click);
	assert(fe_screen_last == NULL);
	assert(screen_texpacks_test_selection() == 2
		   && "a press at the RIGHT of the approved pack card (x 570, row 2) "
			  "selected the wrong row -- the card is 48..592 wide and its rows "
			  "sit at 102 / 172 / 242 on a 70 px pitch");

	/* A press BELOW the viewport's approved bottom edge is still outside the
	 * list: 312 is the last pixel of the band the rows may occupy, and the 322
	 * status line underneath it belongs to the status + Apply band. */
	fe_pointer(320.0F, (float)(FE_TP_LIST_BOTTOM_PX + 10));
	fe_step();
	fe_tap(r.click);
	assert(screen_texpacks_test_selection() == 2
		   && "a press on the status line under the list selected a pack row");

	/* The transaction is untouched: the pointer only SELECTED. Nothing was
	 * armed, so the very next press still navigates, and the live pack never
	 * moved (the same two probes the directional case uses). */
	fe_pointer_off();
	fe_step();
	fe_tap(r.down);
	assert(screen_texpacks_test_selection() == 3
		   && "a pointer press on a pack row applied it instead of only "
			  "selecting it");
	assert(!strcmp(config_read_string(&gstate.config_user, "paths.texturepack",
									  ""),
				   active_before)
		   && "a pointer press on a pack row applied the pack");
	fe_pointer_off();
}

/* ======================================================================== */
/* h. the contextual navigation hint (Astra ruling 6, push-0921 A19)         */
/* ======================================================================== */

/* THE PACK SCREEN'S TEXT-FIT HELPER, directly (visual-fix1 item 2).
 *
 * fep_text_fit is what every pack row, sub-line and status line runs its label
 * through, so its two edge defects are pack-screen defects: a name that filled
 * the caller's buffer EXACTLY was reported cut and ellipsised although nothing
 * had been dropped (`len >= cap - 1`), and a room too narrow for even the
 * first glyph still returned one character -- ink drawn outside the control.
 *
 * FONT FIXTURE, stated plainly: this suite links the real source/graphics/
 * gui_util.c but never loads font/default.png, so gutil_reset_font has not run
 * and its font_char_width[] table is all zeroes. That does NOT make widths
 * uncheckable -- gutil_font_width still charges every character
 * (0 + 1) * size / 8 -- it makes them UNIFORM: at size 16 each character is
 * exactly 2 px and "..." is 6 px, which is what the numbers below use. The
 * realistic per-glyph metrics would need gutil_reset_font on a synthetic
 * texture, and font_char_width[] is a process-global that every other case in
 * this binary measures its menu text against, so this case deliberately does
 * not touch it. What is proven here is the helper's ARITHMETIC, and it is
 * proven against gutil_font_width itself (never a hard-coded pixel count for
 * the result), so it stays true whatever the table holds. */
static void case_pack_text_fit(void) {
	enum { FE_FIT_SIZE = 16, FE_FIT_CH = FE_FIT_SIZE / 8 }; /* 2 px / char */
	char buf[32];
	bool cut;
	assert(gutil_font_width("a", FE_FIT_SIZE) == FE_FIT_CH
		   && "font fixture moved: this case's room numbers assume the "
			  "zeroed-table uniform advance");

	/* cap 0: nothing may be written, and nothing was cut. */
	char canary[2] = {'Z', 'Z'};
	cut = fep_text_fit(canary, 0, "anything", FE_FIT_SIZE, 1000);
	assert(!cut);
	assert(canary[0] == 'Z' && canary[1] == 'Z');

	/* cap 1: only the NUL fits, so the result is EMPTY -- never one glyph. */
	cut = fep_text_fit(buf, 1, "abc", FE_FIT_SIZE, 1000);
	assert(cut);
	assert(buf[0] == '\0');

	/* EXACT fill of the buffer, with room to spare: cap - 1 characters plus
	 * the NUL is a whole string, not a truncation. This is the `>=` bug. */
	cut = fep_text_fit(buf, 4, "abc", FE_FIT_SIZE, 1000);
	assert(!cut);
	assert(strcmp(buf, "abc") == 0);

	/* EXACT fit of the ROOM, to the pixel, both inclusive edges. */
	cut = fep_text_fit(buf, sizeof(buf), "abcd", FE_FIT_SIZE, 4 * FE_FIT_CH);
	assert(!cut);
	assert(strcmp(buf, "abcd") == 0);
	cut = fep_text_fit(buf, sizeof(buf), "abcd", FE_FIT_SIZE,
					   4 * FE_FIT_CH - 1);
	assert(cut);
	assert(strcmp(buf, "abcd") != 0);

	/* NO room at all, and room narrower than a single glyph: empty, cut. */
	cut = fep_text_fit(buf, sizeof(buf), "abcd", FE_FIT_SIZE, 0);
	assert(cut);
	assert(buf[0] == '\0');
	cut = fep_text_fit(buf, sizeof(buf), "abcd", FE_FIT_SIZE, FE_FIT_CH - 1);
	assert(cut);
	assert(buf[0] == '\0');

	/* An empty source is not a cut, and never leaves the buffer dirty. */
	cut = fep_text_fit(buf, sizeof(buf), "", FE_FIT_SIZE, 0);
	assert(!cut);
	assert(buf[0] == '\0');
	cut = fep_text_fit(buf, sizeof(buf), NULL, FE_FIT_SIZE, 1000);
	assert(!cut);
	assert(buf[0] == '\0');

	/* The INVARIANT, swept: whatever the caps and rooms, the result is
	 * NUL-terminated inside the buffer and never wider than the room it was
	 * given -- the property the row draw actually depends on. */
	static const char* const srcs[]
		= {"", "a", "ab", "Isabella II", "a very long pack name indeed"};
	for(size_t s = 0; s < sizeof(srcs) / sizeof(srcs[0]); s++) {
		for(size_t cap = 1; cap <= sizeof(buf); cap++) {
			for(int room = 0; room <= 20 * FE_FIT_CH; room++) {
				char b[32];
				memset(b, 'Z', sizeof(b));
				cut = fep_text_fit(b, cap, srcs[s], FE_FIT_SIZE, room);
				assert(memchr(b, '\0', cap)
					   && "fep_text_fit left no NUL in the buffer");
				assert(gutil_font_width(b, FE_FIT_SIZE) <= room
					   && "fep_text_fit returned text wider than its room");
				if(!cut)
					assert(strcmp(b, srcs[s]) == 0
						   && "fep_text_fit reported no cut but changed the "
							  "text");
			}
		}
	}
	puts("PASS visual-fix1/2: fep_text_fit keeps an exactly-buffer-sized name "
		 "whole, empties rather than overflow when no glyph fits, and never "
		 "returns text wider than its room");
}

/* THE SCENIC BACKDROP'S ACTUAL UVs (design-wiring/uv-record).
 *
 * What the recorder above catches is the stripe defect itself: fep_backdrop
 * emitting SOURCE PIXELS where gfx_draw_quads_flt wants normalized 0..1. The
 * numbers below are the header's own output, captured -- the crop is never
 * recomputed here.
 *
 * FIXTURE, stated plainly: texture_menu_background's .data is pointed at a
 * SENTINEL byte, not decoded image pixels (this suite links no texture.c and no
 * PNG decoder). That is sound because fep_backdrop reads .data only as
 * "loaded / not loaded" and crops from .width/.height -- it samples nothing. No
 * GL or GX context exists or is needed. Image CONTENT is not covered. The
 * struct is saved and restored so the link double stays zeroed (the documented
 * "this pack ships no backdrop" state) for every other case in this binary. */
static int fe_bd_calls;
static size_t fe_bd_count;
static float fe_bd_vtx[12];
static float fe_bd_uv[8];

static void fe_bd_record(size_t count, const float* v, const uint8_t* color,
						 const float* uv) {
	(void)color;
	fe_bd_calls++;
	fe_bd_count = count;
	if(v)
		memcpy(fe_bd_vtx, v, sizeof(fe_bd_vtx));
	if(uv)
		memcpy(fe_bd_uv, uv, sizeof(fe_bd_uv));
}

#define FE_BD_EPS 1.0e-6F

static void fe_bd_arm(size_t w, size_t h) {
	static uint8_t sentinel;
	fe_bd_calls = 0;
	fe_bd_count = 0;
	memset(fe_bd_vtx, 0, sizeof(fe_bd_vtx));
	memset(fe_bd_uv, 0, sizeof(fe_bd_uv));
	texture_menu_background = (struct tex_gfx) {0};
	texture_menu_background.width = w;
	texture_menu_background.height = h;
	texture_menu_background.data = w && h ? &sentinel : NULL;
}

static void fe_bd_expect_uv(float u0, float v0, float u1, float v1) {
	const float want[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
	for(size_t k = 0; k < 8; k++)
		assert(fabsf(fe_bd_uv[k] - want[k]) < FE_BD_EPS
			   && "fep_backdrop emitted a source UV that is not the normalized "
				  "centre-cover crop -- a value outside 0..1 is the stripe "
				  "defect itself");
}

static void case_pack_backdrop_uv(void) {
	const struct tex_gfx saved = texture_menu_background;
	const int fw = 640, fh = 480;

	/* 1024x576 into 640x480: the image is WIDER than the frame's 4:3, so the
	 * crop takes the full height and 768 of the 1024 columns, centred at 128 --
	 * u .125 .. .875, v 0 .. 1. Not 128..896 and not 0..576. */
	fe_bd_arm(1024, 576);
	assert(fep_backdrop(fw, fh, 0) && "a loaded backdrop drew nothing");
	assert(fe_bd_calls == 1 && "the backdrop is one quad per frame");
	assert(fe_bd_count == 4 && "the backdrop quad is four vertices");
	fe_bd_expect_uv(0.125F, 0.0F, 0.875F, 1.0F);

	/* The geometry that goes with it: the whole frame in PHYSICAL pixels, all
	 * four vertices at z = -2 (behind every panel, in front of nothing). */
	const float want_vtx[12] = {0.0F,	   0.0F,	 -2.0F, (float)fw, 0.0F,
								-2.0F,	   (float)fw, (float)fh, -2.0F, 0.0F,
								(float)fh, -2.0F};
	for(size_t k = 0; k < 12; k++)
		assert(fabsf(fe_bd_vtx[k] - want_vtx[k]) < FE_BD_EPS
			   && "the backdrop quad is not the whole frame at z = -2");

	/* An image the frame's own aspect is not cropped at all: 0..1 both axes. */
	fe_bd_arm(640, 480);
	assert(fep_backdrop(fw, fh, 0));
	assert(fe_bd_calls == 1 && fe_bd_count == 4);
	fe_bd_expect_uv(0.0F, 0.0F, 1.0F, 1.0F);

	/* PORTRAIT: taller than the frame, so the crop is VERTICAL instead --
	 * 512x1024 keeps all 512 columns and 384 of the 1024 rows, centred at 320,
	 * i.e. v .3125 .. .6875 with u untouched. The axis must not swap. */
	fe_bd_arm(512, 1024);
	assert(fep_backdrop(fw, fh, 0));
	assert(fe_bd_calls == 1 && fe_bd_count == 4);
	fe_bd_expect_uv(0.0F, 0.3125F, 1.0F, 0.6875F);

	/* No asset (the link double's own zeroed state): false, and NOT ONE quad --
	 * the caller's tiled-dirt fallback owns the frame instead. */
	fe_bd_arm(0, 0);
	assert(!fep_backdrop(fw, fh, 0)
		   && "an absent backdrop reported that it drew");
	assert(fe_bd_calls == 0 && "an absent backdrop emitted a quad anyway");

	/* A zero-sized frame is likewise nothing, with the texture present. */
	fe_bd_arm(1024, 576);
	assert(!fep_backdrop(0, fh, 0) && !fep_backdrop(fw, 0, 0));
	assert(fe_bd_calls == 0 && "a zero-sized frame emitted a backdrop quad");

	/* Temporal regression: both display aspects must move, stay inside the
	 * texture and reverse without a discontinuity at the 64-second endpoint. */
	for(int wide = 0; wide < 2; wide++) {
		int width = wide ? 854 : 640;
		float first[8], end[8];
		fe_bd_arm(1024, 576);
		assert(fep_backdrop_at(width, 480, 0, 0.0F, true));
		memcpy(first, fe_bd_uv, sizeof(first));
		fe_bd_arm(1024, 576);
		assert(fep_backdrop_at(width, 480, 0, 64.0F, true));
		memcpy(end, fe_bd_uv, sizeof(end));
		assert(fabsf(first[0] - end[0]) > 0.05F);
		for(int k = 0; k < 8; k++) {
			assert(first[k] >= 0 && first[k] <= 1);
			assert(end[k] >= 0 && end[k] <= 1);
		}
		fe_bd_arm(1024, 576);
		assert(fep_backdrop_at(width, 480, 0, 128.0F, true));
		for(int k = 0; k < 8; k++) assert(fabsf(fe_bd_uv[k] - first[k]) < FE_BD_EPS);
		fe_bd_arm(1024, 576);
		assert(fep_backdrop_at(width, 480, 0, 63.999F, true));
		for(int k = 0; k < 8; k++) assert(fabsf(fe_bd_uv[k] - end[k]) < 0.0001F);
	}
	texture_menu_background = saved;
	puts("PASS design-wiring/uv-record: fep_backdrop's RECORDED source UVs are "
		 "the normalized centre-cover crop (1024x576 -> u .125/.875, portrait "
		 "-> v .3125/.6875), never pixel coordinates");
}

/* "RIGHT: actions" while the ring is on a list row, "UP: list" while it is on
 * the strip, nothing while the POINTER owns the selection -- text only, in the
 * free band between the list viewport and the strip, touching neither.
 *
 * Every assertion below reads the SCREEN's own hint function and rect (the
 * ones its render2D calls), and the band is checked against the real viewport
 * and button geometry rather than against the literals in the ruling, so a
 * layout change breaks this instead of silently moving the text onto a
 * control. */
static void case_nav_hint(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	char text[64], drawn[64];
	struct gutil_button_bounds dr;

	/* Approved Load replaces directional hints with the count/status band.
	 * Ordinary row/action/pointer focus must not resurrect obsolete hints;
	 * the real renderer is checked as well as the status query. */
	fe_saves(3);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int k = 0; k < 8; k++) fe_step();
	assert(screen_sworld_row_count() == 3);
	unsigned icons_before = fe_load_icon_draws;
	assert(!screen_sworld_test_hint(text, sizeof(text), NULL, NULL, NULL, NULL));
	fe_render(&screen_select_world);
	assert(!fe_hint_drawn(drawn, sizeof(drawn), &dr));
	assert(fe_load_icon_draws == icons_before + 3);
	fe_tap(r.right);
	assert(screen_sworld_test_focus() == FE_LW_PLAY);
	assert(!screen_sworld_test_hint(text, sizeof(text), NULL, NULL, NULL, NULL));
	fe_render(&screen_select_world);
	assert(!fe_hint_drawn(drawn, sizeof(drawn), &dr));
	fe_tap(r.up);
	assert(screen_sworld_test_focus() == FE_LW_ROW(0));
	if(fam == FE_REMOTE) {
		/* Reacquisition alone cannot steal pad focus; a real move can. */
		fe_pointer(2,2); fe_step();
		fe_pointer(fe_load_cx(FE_LW_TAB_CREATE), fe_load_cy(FE_LW_TAB_CREATE));
		fe_step(); fe_step();
		assert(screen_sworld_test_focus() == FE_LW_TAB_CREATE);
		assert(!screen_sworld_test_hint(text, sizeof(text), NULL, NULL, NULL, NULL));
		fe_render(&screen_select_world);
		assert(!fe_hint_drawn(drawn, sizeof(drawn), &dr));
		fe_pointer_off(); fe_step();
	}
	/* Existing empty storage is not an enumeration failure. */
	fe_saves(0);
	assert(fe_mkdir(FE_SAVES_DIR) == 0 || errno == EEXIST);
	fe_attach(fam);
	fe_open(&screen_select_world);
	assert(screen_sworld_row_count() == 0);
	assert(!screen_sworld_test_hint(text, sizeof(text), NULL, NULL, NULL, NULL));
	fe_tap(r.down);
	assert(screen_sworld_test_focus() == FE_LW_PLAY);
	fe_render(&screen_select_world);
	assert(!fe_hint_drawn(drawn, sizeof(drawn), &dr));
	fe_saves(3);

	/* Approved Join uses status/counts instead of legacy direction hints. */
	fe_servers(3); fe_attach(fam); fe_mps_content(&r);
	for(int action=0;action<2;action++) {
		assert(!screen_mps_test_hint(text,sizeof(text),NULL,NULL,NULL,NULL));
		fe_render(&screen_multiplayer_setup_impl);
		assert(!fe_hint_drawn(drawn,sizeof(drawn),&dr));
		fe_tap(r.right);
	}
	/* A real failed persistence remains sticky; a later successful save clears it. */
	remove("config.json"); fe_rmdir("config.json");
	assert(fe_mkdir("config.json") == 0);
	for(int attempt=0;attempt<2;attempt++) {
		fe_mps_action(&r,MPS_BUTTON_DELETE_ORD);
		assert(fe_screen_last == &screen_confirm);
		fe_current=&screen_confirm;
		fe_tap(r.left);
		assert(screen_confirm_test_highlight() == SCREEN_CONFIRM_ANSWER_YES);
		fe_tap(r.click);
		fe_current=&screen_multiplayer_setup_impl;
		assert(screen_mps_test_server_count() == (size_t)(2-attempt));
		assert(screen_mps_test_save_failed() == (attempt==0));
		assert(!screen_mps_test_hint(text,sizeof(text),NULL,NULL,NULL,NULL));
		fe_render(&screen_multiplayer_setup_impl);
		assert(!fe_hint_drawn(drawn,sizeof(drawn),&dr));
		if(!attempt) fe_rmdir("config.json");
	}

	/* --- Texture Packs, gate 2: the band between the list and the footer is
	 * not EMPTY space that a hint could have, it is the approved composition's
	 * own two elements. front-end-refined/studio.js:77-85 (and
	 * proofs/earth--packs--default.png) place, in 640x480 at GUI scale 2: the
	 * list viewport at top 94 / bottom 312, the status line at y 322 ("Changes
	 * the look of blocks and items." / "Applying texture pack..." / the error
	 * wording), the primary Apply at 168,378 304x44 -- so 378..422 -- and the
	 * glyph footer row (Done/Back) at top 432. The band is therefore 120 px
	 * tall and FULLY spoken for: status at its top, Apply through its bottom,
	 * with the footer closing it. That is why no navigation hint is drawn here
	 * -- not "there is no room" (the pre-gate-2 6 px reason), but "this band is
	 * the status + Apply column of the approved design". Ruling 6 named three
	 * screens and still has not been asked about this one; the deviation goes
	 * to Astra with THESE numbers behind it.
	 *
	 * Pinned from the two edges the screen exports (list bottom, footer row
	 * top). The viewport TOP (94) and the 70 px row pitch have no exported
	 * seam, so they are pinned BEHAVIOURALLY instead, by
	 * case_pack_row_pointer_reach's presses at the rows' drawn middles -- no
	 * test-only seam is invented for them here. --- */
	assert(gfx_width() == 640 && gfx_height() == 480 && GFX_GUI_SCALE == 2
		   && "the approved pack composition is pinned in 640x480 pixels at "
			  "GUI scale 2");
	fe_attach(fam);
	screen_texturepacks_set_return(&screen_title);
	fe_open(&screen_texturepacks);
	assert(screen_texpacks_test_list_bottom() == FE_TP_LIST_BOTTOM_PX
		   && "the pack list viewport does not end at the approved 312");
	assert(screen_texpacks_test_strip_top() == FE_TP_FOOTER_TOP_PX
		   && "the pack footer row is not the hint row strip (428)");
	int gap = screen_texpacks_test_strip_top()
		- screen_texpacks_test_list_bottom();
	assert(gap == FE_TP_FOOTER_TOP_PX - FE_TP_LIST_BOTTOM_PX && gap == 116
		   && "the status + Apply band between the pack list and the footer is "
			  "not 116 px (list 312 .. strip 428)");
	fe_render(&screen_texturepacks);
	assert(!fe_hint_drawn(drawn, sizeof(drawn), &dr)
		   && "Texture Packs DREW a navigation hint INTO the status + Apply "
			  "band -- ruling 6 has not been asked about this screen yet");
}

/* Recovery retains the Play session and exposes every line through paging. */
static void case_connect_recovery_layout(enum fe_family fam) {
	struct fe_roles r=fe_roles_for(fam);
	fe_servers(3);
	assert(play_session_remember_world("retained-world"));
	assert(play_session_remember_server("Server 01","10.0.0.2:25565",1));
	screen_multiplayer_setup=&screen_multiplayer_setup_impl;
	fe_attach(fam);
	char reason[256]="";
	for(int i=0;i<12;i++) {
		size_t used=strlen(reason);
		int n=snprintf(reason+used,sizeof(reason)-used,"Reason line %02d\n",i);
		assert(n>0 && (size_t)n<sizeof(reason)-used);
	}
	screen_multiplayer_connect_show_disconnected(reason);
	fe_current=&screen_multiplayer_connect_impl; fe_idle();
	fe_screen_sets=0; fe_screen_last=NULL;
	assert(screen_mpc_test_state()==FE_MPC_ERROR);
	/* critique-1028: a mid-play kick reads "Disconnected", not a join failure. */
	assert(!strcmp(screen_mpc_test_error_title(),"Disconnected"));
	assert(screen_mpc_test_reason_pages()==3 && screen_mpc_test_reason_page()==0);
	for(size_t i=0;i<12;i++) {
		char line[64], want[64]; snprintf(want,sizeof(want),"Reason line %02zu",i);
		assert(screen_mpc_test_reason_line(i,line,sizeof(line)) && !strcmp(line,want));
	}
	struct gutil_button_bounds b;
	for(int i=0;i<2;i++) {
		assert(screen_mpc_test_target_rect(i,&b));
		assert(b.x==104+228*i && b.y==368 && b.width==204 && b.height==44);
	}
	fe_render(&screen_multiplayer_connect_impl);
	fe_tap2(r.page,r.click);
	assert(screen_mpc_test_reason_page()==1 && !fe_screen_sets);
	fe_hold(r.page); fe_current->update(fe_current,FE_DT);
	assert(screen_mpc_test_reason_page()==2);
	for(int i=0;i<10;i++){fe_hold(r.page);fe_current->update(fe_current,FE_DT);}
	assert(screen_mpc_test_reason_page()==2 && !fe_screen_sets);
	fe_idle(); fe_tap(r.page);
	assert(screen_mpc_test_reason_page()==2);
	fe_tap(r.previous); assert(screen_mpc_test_reason_page()==1);
	fe_tap(r.previous); assert(screen_mpc_test_reason_page()==0);
	fe_tap(r.previous); assert(screen_mpc_test_reason_page()==0);
	fe_tap(r.right); fe_tap(r.click);
	assert(fe_screen_last==&screen_multiplayer_setup_impl);
	assert(play_session_tab()==PLAY_TAB_JOIN);
	assert(!strcmp(play_session_world_dir(),"retained-world"));
	assert(screen_mps_test_selection()==1);
	assert(!strcmp(play_session_server_address(),"10.0.0.2:25565"));
	screen_multiplayer_setup=NULL;
}

static void case_reach_connect_retry(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	screen_multiplayer_connect = &screen_multiplayer_connect_impl;

	/* An unchosen player name is a real, reachable connect error. */
	fe_attach(fam);
	fe_open(&screen_multiplayer_connect_impl);
	assert(screen_mpc_test_state() == FE_MPC_ERROR);

	/* A16-9: this used to assert `state == ERROR && screen_sets unchanged &&
	 * last == NULL` around the press -- every one of which is true whether
	 * Retry fired or the press did NOTHING, because Retry calls mpc_reset
	 * DIRECTLY instead of screen_set. The observable difference is the
	 * enabled-control click sound gutil's activate path plays, so that is what
	 * this measures now.
	 *
	 * A16-5: the two-button ERROR page starts ORIGIN_DEFAULT (only the
	 * one-control CONNECTING page keeps the DIRECTIONAL start), so with a
	 * pointer live and resting on nothing, rule (2) REFUSES the first press --
	 * the exact A9-2 shape this round removes, on the page a player lands on
	 * after being dropped from a server. With no pointer at all, rule (3)
	 * still activates the displayed selection, which is untouched. */
	unsigned before = fe_screen_sets;
	unsigned sounds = fe_ui_sounds;
	fe_tap(r.click);
	if(fe_ir_armed) {
		assert(fe_ui_sounds == sounds
			   && "a press with the sprite over empty space fired the error "
				  "page's Retry");
	} else {
		assert(fe_ui_sounds == sounds + 1
			   && "with no pointer, the error page's displayed selection did "
				  "not activate");
	}
	assert(screen_mpc_test_state() == FE_MPC_ERROR && fe_screen_sets == before
		   && fe_screen_last == NULL);

	/* ONE direction press, then activate: Retry, on every family and with or
	 * without a pointer. */
	fe_tap(r.up);
	sounds = fe_ui_sounds;
	fe_tap(r.click);
	assert(fe_ui_sounds == sounds + 1 && screen_mpc_test_state() == FE_MPC_ERROR
		   && fe_screen_sets == before && fe_screen_last == NULL
		   && "the error page's Retry is not reachable by direction + activate");

	/* And the second row still leaves to the title. */
	fe_attach(fam);
	fe_open(&screen_multiplayer_connect_impl);
	fe_tap(r.down);
	fe_tap(r.click);
	assert(fe_screen_last == &screen_title
		   && "Back to Title is not reachable by direction + activate");

	/* Close keeps its existing meaning on this screen. */
	fe_attach(fam);
	fe_open(&screen_multiplayer_connect_impl);
	fe_tap(r.close);
	assert(fe_screen_last == &screen_title);
	screen_multiplayer_connect = NULL;
}

/* ======================================================================== */
/* d. IB_HOME is unchanged everywhere                                        */
/* ======================================================================== */

static void case_home_unchanged(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	fe_saves(3);
	fe_attach(fam);
	fe_open(&screen_select_world);
	fe_tap(r.home);
	assert(fe_screen_last == &screen_title && "HOME stopped cancelling");

	screen_texturepacks_set_return(&screen_title);
	fe_attach(fam);
	fe_open(&screen_texturepacks);
	fe_tap(r.home);
	assert(fe_screen_last == &screen_title && "HOME stopped closing");

	fe_servers(2);
	fe_attach(fam);
	fe_mps_content(&r);
	fe_tap(r.home);
	assert(fe_screen_last == &screen_title && "HOME stopped going Back");

	/* The title's HOME is still the Quit mirror. */
	fe_attach(fam);
	fe_open(&screen_title);
	gstate.quit = false;
	fe_tap(r.home);
	assert(gstate.quit && "HOME stopped quitting from the title");
	gstate.quit = false;
}

/* ======================================================================== */
/* f. item 13 -- stationary pointer vs. directional focus                    */
/* ======================================================================== */

/* Only the Wii Remote family has an IR pointer at all (input_owner_latch
 * refuses one for the Classic, and the GameCube has none), so this is the
 * family the arbitration can be measured on. */
static void case_pointer_arbitration_title(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	fe_attach(FE_REMOTE);

	/* The approved five rows (fe_title_row): the title opens on Play game, and
	 * every row is enabled, so DOWN steps Play game -> Tutorial world. The
	 * fixture's saves dir has no tutorial world, so its press opens the
	 * honest "not on this SD card" dialog (screen_confirm). */
	fe_open(&screen_title);

	/* (a) The pointer is live but rests on NO control. One deliberate D-pad
	 * step hands focus to the D-pad; several stationary frames must not hand
	 * it back, so the activate lands on Tutorial world -- rule (2)'s
	 * directional half. */
	fe_pointer(2.0F, 2.0F);
	fe_step();
	fe_step();
	fe_tap(r.down);
	fe_step();
	fe_step();
	fe_step();
	fe_tap(r.click);
	assert(fe_screen_last == &screen_confirm
		   && "a stationary pointer overwrote directional focus on the title");

	/* (b) Astra ruling 5 D2: after a direction press, the ring wins over a
	 * stationary sprite resting on another enabled row. */
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer(fe_title_cx(FE_TB_PLAY), fe_title_cy(FE_TB_PLAY));
	fe_step();
	fe_step();
	fe_tap(r.down); /* directional focus to Tutorial world */
	fe_step();
	fe_step();
	fe_step();
	fe_screen_last = NULL;
	fe_screen_sets = 0;
	assert(screen_title_test_highlight() == FE_TB_TUTORIAL
		   && "the stationary pointer moved the HIGHLIGHT, not just the press");
	fe_tap(r.click);
	assert(fe_screen_last == &screen_confirm && fe_screen_sets == 1
		   && "the stationary sprite stole the ring's Tutorial world row");
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer(fe_title_cx(FE_TB_PLAY), fe_title_cy(FE_TB_PLAY));
	fe_step();
	fe_tap(r.down);
	fe_step();
	fe_pointer(fe_title_cx(FE_TB_PLAY) + 12, fe_title_cy(FE_TB_PLAY));
	fe_step();
	assert(screen_title_test_highlight() == FE_TB_PLAY);
	fe_tap(r.click);
	assert(fe_screen_last == &screen_select_world
		   && "a moved sprite did not take the Play row back");

	/* ... and a deliberate MOVE takes focus back: onto Quit, the bottom row. */
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer(fe_title_cx(FE_TB_PLAY), fe_title_cy(FE_TB_PLAY));
	fe_step();
	fe_tap(r.down); /* focus to the D-pad, selection on Tutorial world */
	fe_step();
	fe_pointer(fe_title_cx(FE_TB_QUIT), fe_title_cy(FE_TB_QUIT));
	fe_step();
	fe_step();
	gstate.quit = false;
	fe_tap(r.click);
	assert(gstate.quit
		   && "a deliberate pointer move did not take focus back");
	gstate.quit = false;
	fe_pointer_off();
}

/* ======================================================================== */
/* A3 C-1 / H-2 -- the pointer DROPS and REACQUIRES somewhere else           */
/* ======================================================================== */

/* THE INVARIANT under test, for every screen that arbitrates:
 *
 *   the target the screen HIGHLIGHTS is the target an activate FIRES, on
 *   every frame and through every ownership transition -- and a pointer that
 *   REAPPEARS re-arms the hover assignment on its first valid frame, so the
 *   sprite, the highlight and the activation can never name three different
 *   things.
 *
 * source/platform/input.c:861-885 has no IR hysteresis: one frame outside the
 * sensor-bar cone is enough for `valid` to go false, and gui_util.c:271-276
 * then hands the reacquired pointer back as a NON-owner anchored at wherever
 * it reappeared -- so without a re-arm the last hovered target stays armed
 * forever while the sprite sits somewhere else. On a delete confirmation that
 * armed target is the DESTRUCTIVE answer. */

/* Drive `frames` polls with the pointer invalid, then bring it back at (x,y). */
static void fe_pointer_drop_reacquire(int frames, float x, float y) {
	fe_pointer_off();
	for(int k = 0; k < frames; k++)
		fe_step();
	fe_pointer(x, y);
	fe_step();
}

static bool fe_confirm_answered, fe_confirm_answer;
static void fe_confirm_cb(bool yes) {
	fe_confirm_answered = true;
	fe_confirm_answer = yes;
}

/* Astra D2-destructive ruling B (tmp/jobs/next-gaps-0924/astra-D2-destructive.raw)
 * on the generic destructive confirmation (its openers: Delete world, Remove
 * saved server). Table: both mismatch directions x stationary / reappearing
 * sprite. The sprite arms button X on the fresh screen, one toggle puts the
 * ring on Y; the first A only moves focus to X and answers nothing, HOLDING A
 * answers nothing, and the next FRESH A answers X. */
static void case_confirm_destructive_reconciles_focus(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	const int s = GFX_GUI_SCALE;
	const float yes_x = (float)(640 / 2 - 155 * s + 20);
	const float no_x = yes_x + (float)(160 * s);
	const float button_y = (float)(480 / 6 + 96 * s + 10);
	static const struct {
		bool sprite_yes, reappear;
	} rows[] = {{true, false}, {true, true}, {false, false}, {false, true}};
	for(size_t k = 0; k < sizeof rows / sizeof rows[0]; k++) {
		fe_attach(FE_REMOTE);
		fe_confirm_answered = false;
		fe_confirm_answer = false;
		screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
							 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
		fe_open(&screen_confirm);
		float x = rows[k].sprite_yes ? yes_x : no_x;
		enum screen_confirm_answer sprite = rows[k].sprite_yes
			? SCREEN_CONFIRM_ANSWER_YES : SCREEN_CONFIRM_ANSWER_NO;
		fe_pointer(x, button_y);
		fe_step();
		fe_step();
		assert(screen_confirm_test_highlight() == sprite);
		fe_tap(r.left); /* the ring toggles onto the OTHER answer */
		assert(screen_confirm_test_highlight() != sprite);
		if(rows[k].reappear) {
			fe_pointer_off();
			fe_step();
			fe_pointer(x, button_y);
			fe_step();
			assert(screen_confirm_test_highlight() != sprite
				   && "a reacquired stationary sprite took focus by itself");
		}
		fe_hold(r.click);
		fe_current->update(fe_current, FE_DT);
		assert(!fe_confirm_answered
			   && screen_confirm_test_highlight() == sprite
			   && "a destructive confirm acted on a sprite/ring mismatch "
				  "(ruling B)");
		fe_hold(r.click);
		fe_current->update(fe_current, FE_DT);
		fe_hold(r.click);
		fe_current->update(fe_current, FE_DT);
		assert(!fe_confirm_answered && "holding A answered the confirmation");
		fe_step();
		fe_tap(r.click);
		assert(fe_confirm_answered
			   && fe_confirm_answer == rows[k].sprite_yes
			   && "the fresh, reconciled press did not answer the hovered "
				  "button");
	}
	fe_pointer_off();
	puts("PASS ruling B on the destructive confirm: both mismatch directions, "
		 "stationary and reappearing sprite, hold inert, fresh A answers");
}

static void case_pointer_arbitration_confirm(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	/* [740c583 GuiYesNo.initGui]: Yes at w/2-155, No 160 units right of it,
	 * both at h/6+96, 150 units wide. */
	const int s = GFX_GUI_SCALE;
	const float yes_x = (float)(640 / 2 - 155 * s + 20);
	const float button_y = (float)(480 / 6 + 96 * s + 10);

	fe_attach(FE_REMOTE);
	fe_confirm_answered = false;
	screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
						 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
	fe_open(&screen_confirm);
	/* (a) The screen defaults to the SAFE choice (No). With the pointer live
	 * but resting on NO control, one deliberate directional step must reach
	 * the destructive answer and activate it -- the D-pad user of A3 NEW-4,
	 * who could not confirm a deletion at all while IR flickered. */
	fe_pointer(2.0F, 2.0F);
	fe_step();
	fe_step();
	fe_tap(r.left);
	fe_step();
	fe_step();
	fe_step();
	fe_tap(r.click);
	assert(fe_confirm_answered && fe_confirm_answer
		   && "a live pointer resting on nothing swallowed the D-pad's answer");

	/* (b) The stationary pointer rests on Delete while the directional ring
	 * selects Cancel. Astra D2-destructive ruling B: in a destructive
	 * confirmation that mismatch makes the first A a focus-only onto Delete
	 * (neither answer executes); the next FRESH A, focus reconciled,
	 * answers Delete. */
	fe_attach(FE_REMOTE);
	fe_confirm_answered = false;
	screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
						 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
	fe_open(&screen_confirm);
	fe_pointer(yes_x, button_y);
	fe_step();
	fe_step();
	fe_tap(r.left); /* directional focus to the SAFE answer */
	fe_step();
	fe_step();
	fe_step();
	fe_tap(r.click);
	assert(!fe_confirm_answered
		   && screen_confirm_test_highlight() == SCREEN_CONFIRM_ANSWER_YES
		   && "a destructive confirm acted on a sprite/ring mismatch "
			  "(ruling B)");
	fe_tap(r.click);
	assert(fe_confirm_answered && fe_confirm_answer
		   && "the reconciled fresh press did not answer the hovered button");

	/* ... and a deliberate pointer MOVE takes focus back. */
	fe_attach(FE_REMOTE);
	fe_confirm_answered = false;
	screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
						 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
	fe_open(&screen_confirm);
	fe_pointer(yes_x, button_y);
	fe_step();
	fe_tap(r.left); /* focus to the D-pad, selection on No */
	fe_step();
	fe_pointer(yes_x + 40.0F, button_y); /* well past the 4-unit threshold */
	fe_step();
	fe_step();
	fe_tap(r.click);
	assert(fe_confirm_answered && fe_confirm_answer
		   && "a deliberate pointer move did not take focus back on a "
			  "confirmation");
	fe_pointer_off();
}

/* C-1 -- THE FIRST REPORTED SEQUENCE (Astra packet 2, fact 2): aim at Delete,
 * one dropped IR frame, the pointer REAPPEARS over Cancel, press A.
 *
 * Under ruling 2 the reacquisition changes nothing by itself (the round-2
 * re-arm is gone, so a pointer that reappears is still a NON-owner and the
 * highlight stays where it was): the press is decided by rule (1), the sprite
 * is on Cancel while the highlight is on Delete, so NEITHER answer is given --
 * the press moves the highlight to Cancel -- and the SECOND press cancels.
 * Run for both real destructive callbacks. */
static void case_confirm_reacquire(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	const int s = GFX_GUI_SCALE;
	const float yes_x = (float)(640 / 2 - 155 * s + 20);
	const float no_x = (float)(640 / 2 - 155 * s + 160 * s + 20);
	const float button_y = (float)(480 / 6 + 96 * s + 10);
	static const int drop_frames[] = {1, 5};

	for(unsigned d = 0; d < 2; d++) {
		/* (i) the bare screen, with this suite's own callback. */
		fe_attach(FE_REMOTE);
		fe_confirm_answered = false;
		fe_confirm_answer = true;
		screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
							 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
		fe_open(&screen_confirm);
		fe_pointer(yes_x, button_y); /* aim at Delete */
		fe_step();
		fe_step();
		fe_pointer_drop_reacquire(drop_frames[d], no_x, button_y);
		fe_step(); /* and hold perfectly still over Cancel */
		fe_step();
		fe_tap(r.click);
		assert(!fe_confirm_answered
			   && "the confirmation ANSWERED on a press whose sprite and "
				  "highlight named different buttons, after an IR drop and "
				  "reacquire");
		assert(screen_confirm_test_highlight() == 1
			   && "the consumed press did not move the highlight onto Cancel");
		fe_tap(r.click);
		assert(fe_confirm_answered && !fe_confirm_answer
			   && "the confirmation answered YES with the pointer resting on "
				  "Cancel, after an IR drop and reacquire");

		/* (ii) DELETE A WORLD, through the real Select World -> Delete ->
		 * confirm wiring and the real sworld_delete_confirmed callback. */
		fe_saves(3);
		fe_attach(FE_REMOTE);
		fe_open(&screen_select_world);
		for(int f = 0; f < 8; f++)
			fe_step();
		fe_load_delete(&r, 3); /* World options -> Delete (Load contract) */
		assert(fe_screen_last == &screen_confirm);
		fe_current = &screen_confirm;
		fe_pointer(yes_x, button_y);
		fe_step();
		fe_step();
		fe_pointer_drop_reacquire(drop_frames[d], no_x, button_y);
		fe_step();
		fe_screen_last = NULL;
		fe_tap(r.click); /* consumed: the highlight moves to Cancel */
		assert(fe_screen_last == NULL
			   && "the delete confirmation LEFT on a press whose sprite and "
				  "highlight named different buttons");
		fe_tap(r.click); /* and this one cancels */
		struct stat st;
		assert(stat(FE_SAVES_DIR "/world00", &st) == 0
			   && "a world was DELETED with the pointer resting on Cancel, "
				  "after an IR drop and reacquire");
		assert(fe_screen_last == &screen_select_world
			   && "the cancelled confirmation did not return to the list");

		/* (iii) REMOVE A SAVED SERVER, same shape, real mps_delete_confirmed
		 * (which also persists the list, so a wrong answer is durable). */
		fe_servers(3);
		fe_attach(FE_REMOTE);
		fe_mps_content(&r);
		fe_mps_action(&r, 4); /* Join, Direct, Add, Edit, Delete */
		assert(fe_screen_last == &screen_confirm);
		fe_current = &screen_confirm;
		fe_pointer(yes_x, button_y);
		fe_step();
		fe_step();
		fe_pointer_drop_reacquire(drop_frames[d], no_x, button_y);
		fe_step();
		fe_screen_last = NULL;
		fe_tap(r.click); /* consumed */
		assert(fe_screen_last == NULL
			   && "the delete confirmation LEFT on a consumed press");
		fe_tap(r.click); /* cancels */
		fe_current = &screen_multiplayer_setup_impl;
		assert(screen_mps_test_server_count() == 3
			   && "a saved server was REMOVED with the pointer resting on "
				  "Cancel, after an IR drop and reacquire");
	}
	fe_pointer_off();
}

/* H-2, title half: Quit must never fire from a stale target. */
static void case_title_reacquire(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	static const int drop_frames[] = {1, 5};

	for(unsigned d = 0; d < 2; d++) {
		fe_attach(FE_REMOTE);
		fe_open(&screen_title);
		/* Aim at Quit (the bottom row). */
		fe_pointer(fe_title_cx(FE_TB_QUIT), fe_title_cy(FE_TB_QUIT));
		fe_step();
		fe_step();
		gstate.quit = false;
		/* Drop, then reacquire over PLAY GAME and hold still. */
		fe_pointer_drop_reacquire(drop_frames[d], fe_title_cx(FE_TB_PLAY),
								  fe_title_cy(FE_TB_PLAY));
		fe_step();
		fe_step();
		fe_screen_last = NULL;
		fe_screen_sets = 0;
		fe_tap(r.click);
		assert(!gstate.quit
			   && "the title QUIT with the pointer resting on Play game, "
				  "after an IR drop and reacquire");
		assert(fe_screen_sets == 0
			   && "the title activated on a press whose sprite and highlight "
				  "named different buttons");
		fe_tap(r.click);
		assert(!gstate.quit && fe_screen_last == &screen_select_world
			   && "the title did not start the button the pointer rests on "
				  "after a reacquire");
		gstate.quit = false;
	}
	fe_pointer_off();
}

/* THE SECOND REPORTED SEQUENCE (Astra packet 2, fact 3): the D-pad moves
 * focus off Quit, then IR VALIDITY flickers with the sprite unmoved, then
 * A. Round 2's re-arm handed ownership back on every valid frame and the game
 * QUIT; under ruling 2 a validity transition alone can never move focus.
 *
 * Both phases of the flicker are inputs to the rule and both are tested
 * (ruling 2 item 3 withdrew "flicker cannot fire anything"):
 *   VALID phase   -> Astra ruling 5 D2: the sprite is stationary on Quit,
 *                    so the ring's Options... selection activates.
 *   INVALID phase -> rule (3): the press activates the HIGHLIGHTED control,
 *                    which is still the one the D-pad chose. */
static void case_title_validity_flicker(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	const float quit_x = fe_title_cx(FE_TB_QUIT);
	const float quit_y = fe_title_cy(FE_TB_QUIT);

	for(int phase = 0; phase < 2; phase++) {
		bool end_valid = phase == 0;
		fe_attach(FE_REMOTE);
		fe_open(&screen_title);
		fe_pointer(quit_x, quit_y); /* the sprite parks on Quit */
		fe_step();
		fe_step();
		fe_tap(r.up); /* ... and the D-pad walks focus OFF it, to Options */
		fe_step();
		gstate.quit = false;
		fe_screen_last = NULL;
		fe_screen_sets = 0;
		fe_flicker(20, end_valid, quit_x, quit_y);
		assert(screen_title_test_highlight() == FE_TB_OPTIONS
			   && "IR validity flicker alone moved the HIGHLIGHT");
		fe_tap(r.click);
		assert(!gstate.quit
			   && "IR validity flicker with an UNMOVED sprite quit the game");
		assert(fe_screen_last == &screen_options
			   && "IR validity flicker over a stationary sprite stole the ring's "
				  "highlighted Options control");
	}
	gstate.quit = false;
	fe_pointer_off();
}

/* H-2, picker half + L-3: the highlighted cell is the cell that is granted,
 * including when the pointer owns focus but rests on empty space. */
static void case_picker_reacquire(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	int gx = (640 - 9 * PICKER_CELL_PX) / 2, gy = 40 * GFX_GUI_SCALE;
	static const int drop_frames[] = {1, 5};

	for(unsigned d = 0; d < 2; d++) {
		fe_attach(FE_REMOTE);
		fe_open(&screen_creative_picker);
		fe_pointer((float)(gx + 4), (float)(gy + 4)); /* cell 0 */
		fe_step();
		fe_step();
		/* Drop, reacquire over cell 3, hold still, grant. */
		fe_pointer_drop_reacquire(drop_frames[d],
								  (float)(gx + 3 * PICKER_CELL_PX + 4),
								  (float)(gy + 4));
		fe_step();
		fe_rpcs = 0;
		fe_rpc_item = -1;
		fe_tap(r.click);
		assert(fe_rpcs == 0
			   && "the creative picker GRANTED on a press whose sprite and "
				  "highlight named different cells");
		assert(screen_picker_test_highlight() == 3
			   && "the consumed press did not move the highlight onto the "
				  "cell the pointer came back to");
		fe_tap(r.click);
		assert(fe_rpcs == 1 && fe_rpc_item == 4
			   && "the creative picker granted the cell the pointer LEFT, not "
				  "the one it came back to");
	}

	/* A3 NEW-3: the pointer OWNS focus and rests on empty space. Ruling 2 (2):
	 * a pointer user who clicks the background activates nothing -- round 2
	 * fired whichever control the sprite had just left. */
	fe_attach(FE_REMOTE);
	fe_open(&screen_creative_picker);
	fe_pointer((float)(gx + 4), (float)(gy + 4)); /* cell 0, owns focus */
	fe_step();
	fe_step();
	fe_pointer(2.0F, 2.0F); /* a real move, onto nothing at all */
	fe_step();
	fe_step();
	fe_rpcs = 0;
	fe_rpc_item = -1;
	fe_screen_sets = 0;
	fe_tap(r.click);
	assert(fe_rpcs == 0 && fe_screen_sets == 0
		   && "a click on empty space fired the control the pointer LEFT");
	fe_pointer_off();
}

/* Constant sub-threshold IR jitter is NOT movement: it must never take focus
 * away from the D-pad. gui_util.c's threshold is 4 GUI units (8 px at
 * GFX_GUI_SCALE 2); a real Wii Remote wobbles by about a pixel. */
static void case_pointer_jitter(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	const float play_x = fe_title_cx(FE_TB_PLAY);
	const float play_y = fe_title_cy(FE_TB_PLAY);

	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer(play_x, play_y); /* on Play game */
	fe_step();
	fe_tap(r.down);             /* focus to the D-pad: Tutorial world */
	for(int k = 0; k < 40; k++) {
		/* One pixel each way, every frame, for a long time. */
		fe_pointer(play_x + (float)(k & 1), play_y + (float)((k >> 1) & 1));
		fe_step();
	}
	/* What the highlight IS, measured rather than inferred: drop the pointer
	 * and press. Rule (3) activates the highlighted control, so if the jitter
	 * had stolen focus this would open Play game instead. */
	fe_pointer_off();
	fe_step();
	fe_tap(r.click);
	assert(fe_screen_last == &screen_confirm /* no tutorial in fixture */
		   && "1-px IR jitter stole directional focus");
	fe_pointer_off();
}

/* A pointer reading and a directional press arriving on the SAME poll: the
 * directional press wins that frame (navigating is checked before the
 * movement test in gui_util.c), so the pointer does not own focus and the
 * highlight is the ring's. */
static void case_pointer_and_dpad_same_frame(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);

	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer(fe_title_cx(FE_TB_PLAY), fe_title_cy(FE_TB_PLAY));
	fe_step();
	/* The D-pad goes down on a frame where the pointer ALSO jumped onto Quit.
	 * The press owns the frame, so selection steps to Tutorial world. */
	fe_pointer(fe_title_cx(FE_TB_QUIT), fe_title_cy(FE_TB_QUIT));
	fe_tap(r.down);
	gstate.quit = false;
	/* The sprite now rests on Quit while the ring is on Tutorial world (no
	 * tutorial in the fixture: its press opens the explanation dialog). */
	fe_screen_sets = 0;
	fe_tap(r.click);
	assert(!gstate.quit && fe_screen_last == &screen_confirm
		   && "a same-frame pointer jump beat the directional press");
	gstate.quit = false;
	fe_pointer_off();
}

/* NAVIGATION AND ACTIVATION ON THE SAME FRAME was A5's displayed-snapshot
 * case: it asserted that the frame activated the PREVIOUSLY displayed target.
 * Lead ruling R-B replaced that rule outright -- the press is consumed -- and
 * case_navigate_and_activate_consumes below is the same scenario under the new
 * contract, on a confirm (where the old rule could still delete) as well as on
 * the title. Removed rather than left asserting a withdrawn rule. */

/* A HELD activate across a DEFAULT-to-pointer focus-only consumption:
 * the sprite rests on Quit while the opening default remains Play. */
static void case_held_activate_across_focus_only(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);

	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer(fe_title_cx(FE_TB_PLAY), fe_title_cy(FE_TB_PLAY));
	fe_step();
	fe_pointer_off();
	fe_step();
	fe_pointer(fe_title_cx(FE_TB_QUIT), fe_title_cy(FE_TB_QUIT));
	fe_step();
	fe_step();
	assert(screen_title_test_highlight() == FE_TB_PLAY);
	gstate.quit = false;
	fe_screen_sets = 0;
	for(int k = 0; k < 12; k++) {
		fe_hold(r.click); /* the SAME press, never released */
		fe_current->update(fe_current, FE_DT);
	}
	assert(!gstate.quit && fe_screen_sets == 0
		   && "one held activate supplied both the focus move and the press "
			  "that follows it");
	fe_idle(); /* released */
	fe_tap(r.click);
	assert(gstate.quit
		   && "a fresh press after the focus-only consumption did not "
			  "activate what the sprite rests on");
	gstate.quit = false;
	fe_pointer_off();
}

/* A DISABLED control under the pointer consumes the press (ruling 2 item 2):
 * "A pointer over a disabled control must consume activation, not fall back to
 * another selected control."
 *
 * RELOCATED (section 9): the approved title has no disabled row -- the
 * standalone Multiplayer control this case used to hover is gone -- so the
 * coverage moves to a GENUINELY disabled control: the pack screen's
 * Textures/Sounds CATEGORY SELECTOR while a world is loaded
 * (screen_texturepacks.c texpacks_target_enabled: `!ingame_mode`, the gate 1
 * item 6 lock -- sound sets cannot be swapped under a running world). The
 * selector is the one strip control whose disablement is a real game state
 * rather than a test fixture. The sprite rests on it (right-aligned on the
 * panel: x 468..608, y 18..50 in 640x480, fe_tp_category_*), the ring is on
 * the selected row 0. The assertions are the same three: the press activates
 * NEITHER the disabled hover NOR the selected target, it stays consumed on
 * the second press, and the highlight never moved -- proven by dropping the
 * pointer and pressing, which then does exactly what the ring's row does
 * (select it: pending becomes row 0's pack, nothing applied, nothing left). */
#define FE_TP_CATEGORY_X_PX 468 /* TP_PANEL_X + TP_PANEL_W - TP_CAT_W */
#define FE_TP_CATEGORY_Y_PX 18  /* TP_CAT_Y */
#define FE_TP_CATEGORY_W_PX 140 /* TP_CAT_W */
#define FE_TP_CATEGORY_H_PX 32  /* TP_CAT_H */
static float fe_tp_category_cx(void) {
	return (float)(FE_TP_CATEGORY_X_PX + FE_TP_CATEGORY_W_PX / 2);
}
static float fe_tp_category_cy(void) {
	return (float)(FE_TP_CATEGORY_Y_PX + FE_TP_CATEGORY_H_PX / 2);
}

static void case_pointer_over_disabled_control(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	char active_before[128];
	snprintf(active_before, sizeof(active_before), "%s",
			 config_read_string(&gstate.config_user, "paths.texturepack", ""));

	fe_attach(FE_REMOTE);
	screen_texturepacks_set_return(&screen_title);
	screen_texturepacks_set_ingame(true); /* the Sounds selector is DISABLED */
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(4);
	assert(screen_texpacks_test_category() == FE_TP_CAT_TEXTURES);
	assert(screen_texpacks_test_pending() == FE_TP_NO_PENDING);
	fe_pointer(fe_tp_category_cx(), fe_tp_category_cy()); /* the selector */
	fe_step();
	fe_step();
	assert(screen_texpacks_test_highlight() != FE_TP_BTN_CATEGORY
		   && "the pointer FOLLOWED onto a disabled control");
	fe_screen_last = NULL;
	fe_screen_sets = 0;
	fe_ui_sounds = 0;
	fe_tap(r.click);
	assert(fe_screen_sets == 0 && fe_ui_sounds == 0
		   && screen_texpacks_test_category() == FE_TP_CAT_TEXTURES
		   && screen_texpacks_test_pending() == FE_TP_NO_PENDING
		   && "a press over a DISABLED control fell back to the selected one, "
			  "or switched the category");
	/* ... and it stays consumed: the fallback must not appear on the second
	 * press either, because nothing about the screen has changed. */
	fe_tap(r.click);
	assert(fe_screen_sets == 0 && fe_ui_sounds == 0
		   && screen_texpacks_test_category() == FE_TP_CAT_TEXTURES
		   && screen_texpacks_test_pending() == FE_TP_NO_PENDING
		   && "a press over a DISABLED control activated on the second press");
	/* The highlight never moved: drop the pointer and the ring's own target
	 * (row 0) is what the press reaches -- it SELECTS that row and nothing
	 * else, the gate 1 item 7 contract. */
	fe_pointer_off();
	fe_step();
	fe_tap(r.click);
	assert(screen_texpacks_test_pending() == 0 && fe_screen_sets == 0
		   && screen_texpacks_test_category() == FE_TP_CAT_TEXTURES
		   && "the disabled control stole the highlight");
	assert(!strcmp(config_read_string(&gstate.config_user, "paths.texturepack",
									  ""),
				   active_before)
		   && "a press on the ring's row APPLIED a pack");
	screen_texturepacks_set_ingame(false);
	fe_pointer_off();
}

/* A pointer that is ALREADY over a button when the screen opens owns focus on
 * its first valid frame (gui_util.c:272's !initialized branch), so the
 * highlight and the activate are that button from the very first press. */
static void case_pointer_enters_over_button(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);

	fe_live_pointer = true;
	fe_attach(FE_REMOTE);
	fe_pointer(fe_title_cx(FE_TB_QUIT), fe_title_cy(FE_TB_QUIT));
	fe_open(&screen_title); /* opens with the pointer already on Quit */
	fe_step();
	gstate.quit = false;
	fe_tap(r.click);
	assert(gstate.quit
		   && "a pointer already over a button when the screen opened did not "
			  "own it");
	gstate.quit = false;
	fe_live_pointer = false;
	fe_pointer_off();
}

static void case_pointer_arbitration_picker(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	fe_attach(FE_REMOTE);
	fe_open(&screen_creative_picker);

	/* Park the pointer over the very first grid cell. */
	int gx = (640 - 9 * PICKER_CELL_PX) / 2, gy = 40 * GFX_GUI_SCALE;
	fe_pointer((float)(gx + 4), (float)(gy + 4));
	fe_step();
	fe_step();

	/* Walk the cursor RIGHT twice. With the pointer stationary the cursor
	 * must move; if the pointer still owned focus it would snap the cursor
	 * back under itself on every update and the grant below would be the
	 * rig catalogue's item 1 (cell 0) instead of item 3 (cell 2). */
	fe_tap(r.right);
	fe_step();
	fe_tap(r.right);
	fe_step();
	/* Measure the highlight rather than infer it: drop the pointer, so rule
	 * (3) activates whatever is highlighted. A stationary pointer that had
	 * stolen the cursor back would grant cell 0's item (1) instead of
	 * cell 2's (3). */
	fe_pointer_off();
	fe_step();
	fe_rpcs = 0;
	fe_rpc_last = -1;
	fe_rpc_item = -1;
	fe_tap(r.click);
	assert(fe_rpcs == 1 && fe_rpc_last == SRPC_CREATIVE_GIVE
		   && "the creative picker granted nothing on a directional activate");
	assert(fe_rpc_item == 3
		   && "a stationary pointer overwrote the creative picker's grid "
			  "cursor");

	/* ... and a deliberate MOVE takes focus back: onto cell 4. */
	fe_attach(FE_REMOTE);
	fe_open(&screen_creative_picker);
	fe_pointer((float)(gx + 4), (float)(gy + 4));
	fe_step();
	fe_tap(r.right); /* focus to the D-pad */
	fe_step();
	fe_pointer((float)(gx + 4 * PICKER_CELL_PX + 4), (float)(gy + 4));
	fe_step();
	fe_step();
	fe_rpcs = 0;
	fe_rpc_item = -1;
	fe_tap(r.click);
	assert(fe_rpcs == 1 && fe_rpc_item == 5
		   && "a deliberate pointer move did not take the picker cursor back");

	fe_pointer_off();
}

/* Modal isolation + neutral re-arm. */
static void case_modal_and_rearm(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	const int rows = 3;

	fe_saves(rows);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	fe_tap(r.down); /* row 0 -> row 1 */
	assert(screen_sworld_test_gui_selection() == 1);
	/* The list screen's highlight is the SAME pure predicate its render and
	 * its activation decision read (A3 NEW-7), in one target id space that
	 * spans the rows and the action strip. */
	assert(screen_sworld_test_highlight() == screen_sworld_test_row_target(1)
		   && "the select-world highlight is not the row the ring selected");

	/* Reach Delete and open the confirmation with the activate button still
	 * HELD: the confirm screen must not act on that same hold. */
	fe_tap(r.right); /* Play, selection unchanged */
	fe_tap(r.right); /* World options */
	fe_tap(r.click); /* local modal opens on Back */
	fe_tap(r.up); /* Delete */
	fe_hold(r.click);
	fe_current->update(fe_current, FE_DT);
	assert(fe_screen_last == &screen_confirm);
	fe_current = &screen_confirm;
	fe_screen_last = NULL;
	fe_hold(r.click); /* still down across the screen change */
	fe_current->update(fe_current, FE_DT);
	assert(fe_screen_last == NULL
		   && "a held activate fired again on the screen it just opened");
	fe_idle();

	/* MODAL INPUT ISOLATION (review finding L-1, Astra ruling 2 item 8:
	 * "replace the tautological isolation assertion with screen-stack/
	 * transition evidence"). The old form asserted the list underneath did not
	 * move, which only one screen being updated per frame already guaranteed.
	 * The evidence that is NOT a tautology is the SCREEN STACK: across every
	 * frame the modal owns, the live screen stays the confirmation and NOT ONE
	 * transition is requested -- the list underneath receives nothing, not
	 * even the return trip -- while the same presses demonstrably reach the
	 * modal, because they change the answer it finally gives. */
	size_t list_selection_before = screen_sworld_test_gui_selection();
	int list_scroll_before = screen_sworld_test_scroll();
	fe_screen_sets = 0;
	fe_screen_last = NULL;
	/* The confirmation opens on the SAFE answer; one LEFT toggles it to the
	 * destructive one, a second toggles it back. Two presses therefore leave
	 * it safe again -- and an odd number would not, which is what makes
	 * "consumed" measurable rather than assumed. */
	fe_tap(r.left);
	fe_tap(r.left);
	assert(gstate.players[0].current_screen == &screen_confirm
		   && fe_screen_sets == 0 && fe_screen_last == NULL
		   && "a modal frame requested a screen transition");
	assert(screen_sworld_test_gui_selection() == list_selection_before
		   && screen_sworld_test_scroll() == list_scroll_before
		   && "the list underneath a modal moved while the modal had focus");
	/* Prove the modal really did consume them: one more LEFT lands on the
	 * destructive answer, and Close still refuses it. That Close is the ONE
	 * transition of the whole modal, and it goes back to the list. */
	fe_tap(r.left);
	fe_tap(r.close);
	assert(fe_screen_sets == 1 && fe_screen_last == &screen_select_world
		   && "the modal's only transition was not the single return to the "
			  "screen underneath");
	char probe[128];
	snprintf(probe, sizeof(probe), FE_SAVES_DIR "/world00");
	struct stat st;
	assert(stat(probe, &st) == 0 && "Close on a confirmation destroyed data");
	assert(screen_sworld_test_gui_selection() == list_selection_before
		   && "returning from the confirmation lost the retained selection");
}

/* ======================================================================== */
/* M-2 -- focus may never be stranded on a control that just went disabled   */
/* ======================================================================== */

/* MPS_BUTTON_REFRESH is enabled only while the ping sweep has finished
 * (screen_multiplayer_setup.c:1124-1132), and activating it RESTARTS the
 * sweep, which disables it again in the same frame. gutil_button_draw ignores
 * `hover` for a disabled button (gui_util.c:689, ty = enabled ? … : 46), so
 * focus left sitting there is invisible AND a second activate is refused --
 * a dead press with no feedback. After any activation the focus must land
 * back on something enabled. */
static void case_focus_never_stranded(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	fe_servers(3);
	fe_attach(fam);
	fe_mps_content(&r);
	/* Let the rig's (unreachable) ping sweep resolve so Refresh is enabled. */
	screen_mps_test_set_ping_cursor(3);
	assert(screen_mps_test_refresh_enabled()
		   && "the Refresh precondition this case needs did not hold");

	fe_mps_focus_action(&r, MPS_BUTTON_REFRESH_ORD);
	assert(screen_mps_test_focus() == MPS_BUTTON_REFRESH_ORD
		   && "the walk did not reach Refresh");
	fe_tap(r.click); /* restarts the sweep -> Refresh becomes disabled */
	assert(!screen_mps_test_refresh_enabled()
		   && "activating Refresh did not restart the sweep");
	fe_step();
	int focus = screen_mps_test_focus();
	assert(focus != MPS_BUTTON_REFRESH_ORD
		   && "focus is stranded on Refresh, which just became disabled");
	/* Astra ruling 2 item 6 REPLACES round 2's "the next A must do something"
	 * (which sent focus to Back, so a second tap on Refresh left the screen --
	 * A3 NEW-2). The destination must be an ENABLED control that is
	 * non-destructive AND does not leave the screen, join, or open another
	 * modal; if there is none, there must be NO actionable selection until the
	 * player navigates. On this strip every enabled button either leaves
	 * (Join, Back), opens an editor (Direct Connect, Add, Edit) or destroys
	 * (Delete), so "no actionable selection" is the correct, deterministic
	 * answer -- and the next activate is then a deliberate no-op, not a
	 * surprise. */
	assert(focus == MPS_FOCUS_NONE_ORD
		   && "the refresh fallback did not land on 'no actionable selection' "
			  "although every enabled destination is unsafe");
	assert(!screen_mps_test_focus_enabled()
		   && "'no actionable selection' reported an enabled focus");
	fe_screen_last = NULL;
	fe_tap(r.click);
	assert(fe_screen_last == NULL && screen_mps_test_view() == FE_MPS_MAIN
		   && "an activate with no actionable selection did something");
	/* ... and one directional press recovers, deterministically. */
	fe_tap(r.up);
	assert(screen_mps_test_focus_enabled() && screen_mps_test_options_open()
		   && "navigation did not recover enabled modal focus");
	fe_tap(r.close);
	assert(!screen_mps_test_options_open() && screen_mps_test_focus() == 9);
}

/* ======================================================================== */
/* A8 -- review A6-1, and the lead rulings R-A / R-B / R-C                    */
/* ======================================================================== */

/* A6-1, the reported CRITICAL, exactly: the pointer arms the DESTRUCTIVE
 * answer, IR drops for one frame, the sprite comes back over EMPTY SPACE --
 * not over a button -- and A is pressed. Under A5 rule (2) asked
 * pointer_owns_focus, which a reacquired pointer does not have, so the
 * "directional half" ran and activated what the POINTER had armed: the world
 * was deleted. R-A asks the selection's ORIGIN instead, which is POINTER and
 * stays POINTER across any number of validity transitions.
 *
 * The control is the same sprite path with validity NEVER lost: the two must
 * give the SAME answer, because the only difference is the IR blip. */
static void case_a6_1_empty_space_after_drop(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	const int s = GFX_GUI_SCALE;
	/* [740c583 GuiYesNo.initGui]: Yes at w/2-155 (150 wide), No 160 right of
	 * it -- so x = w/2 is the gap BETWEEN them, over no control at all. */
	const float yes_x = (float)(640 / 2 - 155 * s + 20);
	const float empty_x = 320.0F;
	const float button_y = (float)(480 / 6 + 96 * s + 10);
	static const int drop_frames[] = {0, 1, 2, 30};

	for(unsigned d = 0; d < sizeof(drop_frames) / sizeof(drop_frames[0]);
		d++) {
		/* (i) the bare screen. drop_frames[0] == 0 is the CONTROL: the sprite
		 * slides to the same empty spot without IR ever going invalid. */
		fe_attach(FE_REMOTE);
		fe_confirm_answered = false;
		fe_confirm_answer = true;
		screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
							 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
		fe_open(&screen_confirm);
		fe_pointer(yes_x, button_y); /* the pointer ARMS Delete */
		fe_step();
		fe_step();
		if(drop_frames[d] > 0)
			fe_pointer_drop_reacquire(drop_frames[d], empty_x, button_y);
		else
			fe_pointer(empty_x, button_y);
		fe_step();
		fe_step();
		fe_tap(r.click);
		assert(!fe_confirm_answered
			   && "a press over EMPTY SPACE answered the confirmation with "
				  "what the POINTER had armed");

		/* (ii) DELETE A WORLD, through the real wiring and the real
		 * sworld_delete_confirmed callback. */
		fe_saves(3);
		fe_attach(FE_REMOTE);
		fe_open(&screen_select_world);
		for(int f = 0; f < 8; f++)
			fe_step();
		fe_load_delete(&r, 3); /* World options -> Delete (Load contract) */
		assert(fe_screen_last == &screen_confirm);
		fe_current = &screen_confirm;
		fe_pointer(yes_x, button_y);
		fe_step();
		fe_step();
		if(drop_frames[d] > 0)
			fe_pointer_drop_reacquire(drop_frames[d], empty_x, button_y);
		else
			fe_pointer(empty_x, button_y);
		fe_step();
		fe_tap(r.click);
		/* The four DOWNs walked the list to row 2 before entering the strip,
		 * so world02 is the one Delete is aimed at -- checking world00 would
		 * pass for the wrong reason. All three must survive. */
		struct stat st;
		assert(stat(FE_SAVES_DIR "/world02", &st) == 0
			   && stat(FE_SAVES_DIR "/world00", &st) == 0
			   && "a world was DELETED by a press over EMPTY SPACE after an "
				  "IR drop");

		/* (iii) REMOVE A SAVED SERVER, same shape, real mps_delete_confirmed
		 * (which also persists the list, so a wrong answer is durable). */
		fe_servers(3);
		fe_attach(FE_REMOTE);
		fe_mps_content(&r);
		fe_mps_action(&r, 4); /* Join, Direct, Add, Edit, Delete */
		assert(fe_screen_last == &screen_confirm);
		fe_current = &screen_confirm;
		fe_pointer(yes_x, button_y);
		fe_step();
		fe_step();
		if(drop_frames[d] > 0)
			fe_pointer_drop_reacquire(drop_frames[d], empty_x, button_y);
		else
			fe_pointer(empty_x, button_y);
		fe_step();
		fe_tap(r.click);
		fe_current = &screen_multiplayer_setup_impl;
		assert(screen_mps_test_server_count() == 3
			   && "a saved server was REMOVED by a press over EMPTY SPACE "
				  "after an IR drop");
	}
	fe_pointer_off();
}

/* R-A, the other half: the ORIGIN is sticky, so a DIRECTIONAL selection stays
 * activatable however the IR behaves -- a player on a Classic-style D-pad with
 * the Remote lying on the table, sprite parked over nothing, must still be
 * able to press A. Validity flicker must not change that either way. */
static void case_origin_is_sticky(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);

	/* DIRECTIONAL origin + sprite on empty space: the press activates. */
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer(2.0F, 2.0F);
	fe_step();
	fe_tap(r.down); /* the D-pad chooses: origin DIRECTIONAL */
	fe_step();
	fe_tap(r.click);
	assert(fe_screen_last == &screen_confirm /* Tutorial world, none here */
		   && "a D-pad user with the sprite on empty space could not "
			  "activate");

	/* ... and 30 frames of validity flicker do not change that. */
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer(2.0F, 2.0F);
	fe_step();
	fe_tap(r.down);
	fe_flicker(30, true, 2.0F, 2.0F);
	fe_tap(r.click);
	assert(fe_screen_last == &screen_confirm /* Tutorial world, none here */
		   && "validity flicker took the press away from a DIRECTIONAL "
			  "selection");

	/* POINTER origin survives the same flicker in the other direction: the
	 * sprite armed Quit, then rests on nothing. Nothing may fire. */
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer(fe_title_cx(FE_TB_QUIT), fe_title_cy(FE_TB_QUIT));
	fe_step();
	fe_step(); /* the pointer ARMS Quit */
	fe_pointer(2.0F, 2.0F); /* a real move, onto nothing */
	fe_step();
	fe_step();
	gstate.quit = false;
	fe_screen_sets = 0;
	fe_flicker(30, true, 2.0F, 2.0F);
	fe_tap(r.click);
	assert(!gstate.quit && fe_screen_sets == 0
		   && "a press over empty space fired what the POINTER had armed, "
			  "after 30 validity transitions");
	fe_pointer_off();
}

/* R-B: a frame carrying BOTH a direction edge and an activate edge consumes
 * the press. On a confirm that is the difference between cancelling and
 * deleting (A6-6); on the title it is the difference between opening the
 * screen you were leaving and opening nothing. */
static void case_navigate_and_activate_consumes(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);

	/* The confirm opens on the SAFE answer; one LEFT moves to the
	 * destructive one. A frame that carries LEFT and A together must answer
	 * NEITHER -- not the destructive one it is moving to, not the safe one it
	 * is leaving. */
	fe_attach(FE_REMOTE);
	fe_confirm_answered = false;
	screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
						 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
	fe_open(&screen_confirm);
	fe_step();
	fe_tap2(r.left, r.click);
	assert(!fe_confirm_answered
		   && "a frame carrying navigation AND activation answered the "
			  "confirmation");
	/* The navigation still happened, and the NEXT press answers what is now
	 * highlighted. */
	assert(screen_confirm_test_highlight() == 0
		   && "the navigation half of the frame was swallowed too");
	fe_tap(r.click);
	assert(fe_confirm_answered && fe_confirm_answer
		   && "the press after a consumed same-frame navigation did not "
			  "answer the highlighted button");

	/* The title: DOWN + A together opens nothing at all. */
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_step();
	fe_screen_sets = 0;
	fe_tap2(r.down, r.click);
	assert(fe_screen_sets == 0
		   && "a frame carrying navigation AND activation activated on the "
			  "title");
	assert(screen_title_test_highlight() == FE_TB_TUTORIAL
		   && "the navigation half of the frame was swallowed too");
	fe_tap(r.click);
	assert(fe_screen_last == &screen_confirm
		   && "the press after a consumed same-frame navigation did not open "
			  "the highlighted button");
}

/* R-C (review A6-4): a press the activation rule CONSUMED must not reach any
 * other handler in the same update. Driven on the COLLIDING table, where one
 * physical button carries both the activate role and Close. */
static void case_consumed_press_reaches_nothing_else(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	assert(r.click == r.close
		   && "this case needs the colliding table, where activate IS Close");

	/* Title: nothing to close to, so the observable is `quit` plus the
	 * transition count -- and the FOCUS-ONLY half must still have happened. */
	fe_attach(fam);
	fe_open(&screen_title);
	fe_step();
	fe_screen_sets = 0;
	gstate.quit = false;
	fe_tap2(r.down, r.click); /* R-B consumes it; Close must not follow */
	assert(fe_screen_sets == 0 && !gstate.quit
		   && "a consumed press also reached the title Close/HOME branch");

	/* Select World: Close would go to the title and the shared page role
	 * would scroll the list. A consumed press must leave the screen exactly
	 * where it is, in both senses (A9-4). */
	fe_saves(FE_LONG_LIST);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	fe_screen_sets = 0;
	fe_screen_last = NULL;
	int scroll_before = screen_sworld_test_scroll();
	fe_tap2(r.down, r.click);
	assert(fe_screen_sets == 0 && fe_screen_last == NULL
		   && "a consumed press also CLOSED Select World");
	assert(screen_sworld_test_scroll() == scroll_before
		   && "a consumed press also PAGED the world list");

	/* The confirmation: Close is the safe answer, so a consumed press that
	 * leaked into it would answer the dialog -- silently, on a frame the
	 * player meant as a navigation. */
	fe_attach(fam);
	fe_confirm_answered = false;
	screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
						 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
	fe_open(&screen_confirm);
	fe_step();
	fe_tap2(r.left, r.click);
	assert(!fe_confirm_answered
		   && "a consumed press also reached the confirmation Close branch");

	/* The server editor's Back ring wins over a stationary sprite on a text
	 * field; it leaves the sub-editor without moving the caret. */
	if(fam == FE_REMOTE) {
		fe_servers(1);
		fe_attach(fam);
		fe_mps_content(&r);
		fe_mps_action(&r, 2); /* Add Server */
		assert(screen_mps_view() == SCREEN_MPS_VIEW_ADD);
		fe_pointer(320,174);
		fe_step();
		fe_step();
		fe_tap(r.up); /* field A -> Back; pointer stays on the field */
		assert(screen_mps_test_parent_focus()==2);
		fe_tap(r.click);
		assert(!screen_mps_test_editor_open()
			   && screen_mps_view() == SCREEN_MPS_VIEW_MAIN
			   && "a stationary pointer on a field overrode the Back ring");
		fe_pointer_off();
		assert(screen_mps_view() == SCREEN_MPS_VIEW_MAIN
			   && screen_mps_test_server_count() == 1);
	}

	/* A press the rule CONSUMED still cannot move the caret (restored
	 * coverage, Fable G4 review item 5). The mismatch is against the
	 * sub-editor's ENTRY selection -- Back, seeded by the screen on entry,
	 * never a direction press -- with the sprite reacquired, unmoved and so
	 * not the focus owner, over field A. The first press is a focus-only that
	 * moves only the ring; the second is what focuses the field. */
	if(fam == FE_REMOTE) {
		fe_servers(1);
		fe_attach(fam);
		fe_mps_content(&r);
		fe_mps_action(&r, 2); /* Add Server */
		assert(screen_mps_view() == SCREEN_MPS_VIEW_ADD);
		fe_pointer(320,120); /* blank: first valid frame owns, assigns nothing */
		fe_step();
		fe_step();
		fe_pointer_off();
		fe_step();
		fe_pointer(320,174); /* field A: reacquired without movement */
		fe_step();
		fe_step();
		assert(screen_mps_test_parent_focus()==2
			   && "an unmoved reacquired sprite took the entry selection");
		fe_ui_sounds = 0;
		fe_tap(r.click);
		assert(fe_ui_sounds == 0 && !screen_mps_test_editor_open()
			   && screen_mps_view() == SCREEN_MPS_VIEW_ADD
			   && "a consumed press moved the caret, or left the editor");
		assert(screen_mps_test_parent_focus()==3
			   && "the consumed press did not move the ring under the sprite");
		fe_step();
		fe_tap(r.click);
		assert(fe_ui_sounds == 1 && screen_mps_test_editor_open()
			   && screen_mps_view() == SCREEN_MPS_VIEW_ADD
			   && "the second press did not focus the field");
		fe_pointer_off();
		fe_step(); fe_tap(r.home);
		assert(!screen_mps_test_editor_open() && screen_mps_view()==SCREEN_MPS_VIEW_ADD);
		fe_step(); fe_tap(r.home);
		assert(screen_mps_view() == SCREEN_MPS_VIEW_MAIN
			   && screen_mps_test_server_count() == 1);
	}

	/* And the press is only consumed when the RULE consumed it: with no
	 * navigation, the same colliding button still activates. */
	fe_attach(fam);
	fe_open(&screen_title);
	fe_step();
	fe_screen_last = NULL;
	fe_tap(r.click);
	assert(fe_screen_last == &screen_select_world
		   && "the colliding button stopped activating altogether");
}

/* Review A6-5: the safe answer is the OPENER's word, not a constant inside
 * the confirm. The proof is a dialog that names the OTHER one: it must open on
 * YES and a stray press must then answer YES, which is the whole point of the
 * parameter existing. (No shipping caller does this -- both real ones destroy
 * on YES -- so this is the case that keeps the parameter honest.) */
static void case_confirm_safe_answer_is_the_callers_word(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);

	fe_attach(FE_REMOTE);
	fe_confirm_answered = false;
	screen_confirm_setup("Keep this?", "nothing is lost either way", "Keep",
						 "Discard", SCREEN_CONFIRM_ANSWER_YES, fe_confirm_cb);
	fe_open(&screen_confirm);
	fe_step();
	assert(screen_confirm_test_highlight() == SCREEN_CONFIRM_ANSWER_YES
		   && "the modal did not open on the answer the opener called safe");
	fe_tap(r.click);
	assert(fe_confirm_answered && fe_confirm_answer
		   && "the press did not answer the opener's safe answer");

	/* ... and the shipping shape is unchanged: NO stays the default there. */
	fe_attach(FE_REMOTE);
	fe_confirm_answered = false;
	screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
						 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
	fe_open(&screen_confirm);
	fe_step();
	assert(screen_confirm_test_highlight() == SCREEN_CONFIRM_ANSWER_NO
		   && "a destructive dialog stopped opening on Cancel");
}

/* A9-1, the reported HIGH: "aim at Delete, LOWER THE REMOTE, press A". IR
 * goes invalid the moment the Remote points away from the sensor bar -- the
 * single most ordinary thing a Wii player does -- and rule (3) then activated
 * whatever was highlighted, which the pointer itself had armed. Nothing on
 * screen changes between the two presses below; only the pointer's validity
 * does. R-D refuses the first and keeps the second, because a direction press
 * is the player saying which target they mean. */
/* D1 (Astra ruling 5, closing review A16-1): on a two-answer confirmation the
 * SAFE answer is activatable with a valid sprite over empty space, so A
 * DISMISSES the dialog instead of being dead -- and the exception is never
 * transferred to the destructive answer.
 *
 * The reviewer measured the defect on a real world: A did nothing, the only
 * navigation this screen has TOGGLES onto Delete and stamps it DIRECTIONAL,
 * and the next A deleted. The first press closing the dialog is what takes
 * that sequence away. */
static void case_d1_confirm_safe_answer_is_directional(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	const int s = GFX_GUI_SCALE;
	const float yes_x = (float)(640 / 2 - 155 * s + 20);
	const float button_y = (float)(480 / 6 + 96 * s + 10);
	/* Above the buttons and off both of them: a valid sprite on NO control. */
	const float empty_x = 8.0F, empty_y = 8.0F;

	/* The sprite halves need an IR pointer, which only the Wii Remote has
	 * (input_owner_latch refuses the Classic one and the GameCube has none),
	 * so for the other two families they would silently test rule (3)
	 * instead. Every family still runs the no-pointer half at the end. */
	if(fam != FE_REMOTE)
		goto no_pointer_half;

	/* (a) fresh confirm, sprite valid over nothing, A -> the SAFE answer
	 * fires and the dialog closes. */
	fe_attach(fam);
	fe_confirm_answered = false;
	fe_confirm_answer = true;
	screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
						 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
	fe_open(&screen_confirm);
	fe_pointer(empty_x, empty_y);
	fe_step();
	assert(screen_confirm_test_highlight() == SCREEN_CONFIRM_ANSWER_NO);
	fe_tap(r.click);
	assert(fe_confirm_answered && !fe_confirm_answer
		   && "a press with the sprite over empty space did not dismiss the "
			  "dialog through its safe answer");

	/* (b) the reviewer's sequence can no longer delete by accident: the FIRST
	 * A already closed the dialog, so there is nothing left for RIGHT + A to
	 * act on. Driven through the REAL world path, and the world survives. */
	fe_saves_small_dir();
	fe_saves(3);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	fe_load_delete(&r, 3); /* World options -> Delete (Load contract) */
	assert(fe_screen_last == &screen_confirm);
	fe_current = &screen_confirm;
	fe_pointer(empty_x, empty_y);
	fe_step();
	fe_tap(r.click); /* A: dismisses, safely */
	assert(fe_screen_last == &screen_select_world
		   && "the safe answer did not return to the list");
	fe_current = &screen_select_world;
	struct stat st;
	assert(stat(FE_SAVES_DIR "/world00", &st) == 0);
	assert(screen_sworld_test_row_count() == 3
		   && "the A, RIGHT, A sequence still destroyed a world");

	/* (c) the DESTRUCTIVE answer keeps needing deliberate intent -- and still
	 * fires when the player toggles onto it, sprite over nothing. */
	fe_attach(fam);
	fe_confirm_answered = false;
	fe_confirm_answer = false;
	screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
						 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
	fe_open(&screen_confirm);
	fe_pointer(empty_x, empty_y);
	fe_step();
	fe_tap(r.left); /* toggle: Cancel -> Delete, DIRECTIONAL by the player */
	assert(screen_confirm_test_highlight() == SCREEN_CONFIRM_ANSWER_YES);
	fe_tap(r.click);
	assert(fe_confirm_answered && fe_confirm_answer
		   && "a deliberate toggle onto Delete stopped being activatable");

	/* (d) Ruling B: a sprite resting on Delete with the ring on Cancel only
	 * moves focus on the first A; nothing is answered by it. */
	fe_attach(fam);
	fe_confirm_answered = false;
	fe_confirm_answer = false;
	screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
						 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
	fe_open(&screen_confirm);
	fe_pointer(yes_x, button_y);
	fe_step();
	fe_step();
	/* A pointer that becomes valid on a fresh screen OWNS focus for that
	 * frame (gui_util.c:271-276), so it arms Delete -- the existing
	 * case_pointer_enters_over_button pins that. One LEFT takes focus back to
	 * the D-pad and puts the ring on the safe answer, which is the state this
	 * half is about: sprite resting on Delete, highlight on Cancel. */
	fe_tap(r.left);
	assert(screen_confirm_test_highlight() == SCREEN_CONFIRM_ANSWER_NO);
	fe_tap(r.click);
	assert(!fe_confirm_answered
		   && screen_confirm_test_highlight() == SCREEN_CONFIRM_ANSWER_YES
		   && "a destructive confirm acted on a sprite/ring mismatch "
			  "(ruling B)");
	/* Back-out still answers the SAFE side after the focus move. */
	fe_tap(r.close);
	assert(fe_confirm_answered && !fe_confirm_answer
		   && "Back after the ruling-B focus move did not answer Cancel");

	/* (e) BY MEANING, NOT BY INDEX: a dialog whose declared safe answer is
	 * YES gets the same treatment on YES, and its NO stays guarded. */
	fe_attach(fam);
	fe_confirm_answered = false;
	fe_confirm_answer = false;
	screen_confirm_setup("Keep this?", "nothing is lost", "Keep", "Discard",
						 SCREEN_CONFIRM_ANSWER_YES, fe_confirm_cb);
	fe_open(&screen_confirm);
	fe_pointer(empty_x, empty_y);
	fe_step();
	assert(screen_confirm_test_highlight() == SCREEN_CONFIRM_ANSWER_YES);
	fe_tap(r.click);
	assert(fe_confirm_answered && fe_confirm_answer
		   && "the safe answer was decided by INDEX, not by the opener's "
			  "declaration");

no_pointer_half:
	/* With NO pointer at all, rule (3) has always activated the displayed
	 * selection whatever the origin -- D1 changes an INPUT to the helper, not
	 * that rule, and this is what pins that it did not disturb it. */
	fe_attach(fam);
	fe_pointer_off();
	fe_confirm_answered = false;
	fe_confirm_answer = true;
	screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
						 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
	fe_open(&screen_confirm);
	fe_tap(r.click);
	assert(fe_confirm_answered && !fe_confirm_answer
		   && "with no pointer the confirmation's safe answer stopped firing");
}

static void case_a9_1_destructive_needs_intent(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	const int s = GFX_GUI_SCALE;
	const float yes_x = (float)(640 / 2 - 155 * s + 20);
	const float button_y = (float)(480 / 6 + 96 * s + 10);

	/* (i) the bare screen. */
	fe_attach(FE_REMOTE);
	fe_confirm_answered = false;
	fe_confirm_answer = true;
	screen_confirm_setup("Are you sure?", "forever", "Delete", "Cancel",
						 SCREEN_CONFIRM_ANSWER_NO, fe_confirm_cb);
	fe_open(&screen_confirm);
	fe_pointer(yes_x, button_y); /* the pointer ARMS Delete */
	fe_step();
	fe_step();
	assert(screen_confirm_test_highlight() == SCREEN_CONFIRM_ANSWER_YES
		   && "the pointer did not arm the destructive answer");
	fe_pointer_off(); /* the Remote is lowered: IR invalid */
	fe_step();
	fe_step();
	fe_tap(r.click);
	assert(!fe_confirm_answered
		   && "lowering the Remote and pressing A answered the DESTRUCTIVE "
			  "button the pointer had armed");
	/* The same screen, the same highlight: a D-pad user who MOVED there still
	 * deletes with one press. */
	fe_tap(r.left); /* Delete -> Cancel */
	fe_tap(r.left); /* Cancel -> Delete, this time on purpose */
	assert(screen_confirm_test_highlight() == SCREEN_CONFIRM_ANSWER_YES);
	fe_tap(r.click);
	assert(fe_confirm_answered && fe_confirm_answer
		   && "a D-pad user who walked to Delete could not confirm");

	/* (ii) DELETE A WORLD, through the real wiring. */
	fe_saves(3);
	fe_attach(FE_REMOTE);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	fe_load_delete(&r, 3); /* World options -> Delete (Load contract) */
	assert(fe_screen_last == &screen_confirm);
	fe_current = &screen_confirm;
	fe_pointer(yes_x, button_y);
	fe_step();
	fe_step();
	fe_pointer_off();
	fe_step();
	fe_tap(r.click);
	struct stat st;
	assert(stat(FE_SAVES_DIR "/world02", &st) == 0
		   && stat(FE_SAVES_DIR "/world00", &st) == 0
		   && "a world was DELETED by lowering the Remote and pressing A");

	/* (iii) REMOVE A SAVED SERVER, same shape. */
	fe_servers(3);
	fe_attach(FE_REMOTE);
	fe_mps_content(&r);
	fe_mps_action(&r, 4); /* Join, Direct, Add, Edit, Delete */
	assert(fe_screen_last == &screen_confirm);
	fe_current = &screen_confirm;
	fe_pointer(yes_x, button_y);
	fe_step();
	fe_step();
	fe_pointer_off();
	fe_step();
	fe_tap(r.click);
	fe_current = &screen_multiplayer_setup_impl;
	assert(screen_mps_test_server_count() == 3
		   && "a saved server was REMOVED by lowering the Remote and pressing "
			  "A");
	fe_pointer_off();
}

/* A9-2, the other reported HIGH: a pointer click on the BACKGROUND of a
 * sub-editor fired its DEFAULT Cancel and discarded the typing. Round 3
 * introduced it by passing the background to the helper as "no hover" while
 * the ring still carried the DEFAULT origin; R-E refuses that. The same
 * question is asked of every screen that can be sitting on a DEFAULT
 * selection: a background click must change nothing. */
static void case_a9_2_background_click_changes_nothing(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	/* Well clear of the OSK, the fields and the button row. */
	const float bg_x = 12.0F, bg_y = 12.0F;

	static const int views[3] = {SCREEN_MPS_VIEW_ADD, SCREEN_MPS_VIEW_NAME,
								 SCREEN_MPS_VIEW_DIRECT};
	for(unsigned k = 0; k < 3; k++) {
		fe_servers(1);
		fe_attach(FE_REMOTE);
		fe_mps_content(&r);
		if(views[k] == SCREEN_MPS_VIEW_ADD)
			fe_mps_action(&r, 2); /* Add Server */
		else if(views[k] == SCREEN_MPS_VIEW_DIRECT)
			fe_mps_action(&r, 1); /* Direct Connect */
		else {
			fe_tap(r.up);
			fe_tap(r.up);
			fe_tap(r.click); /* the player-name editor */
		}
		assert(screen_mps_view() == views[k]
			   && "the sub-editor under test did not open");
		fe_pointer(bg_x, bg_y);
		fe_step();
		fe_step();
		fe_ui_sounds = 0;
		fe_tap(r.click);
		assert(screen_mps_view() == views[k] && fe_ui_sounds == 0
			   && "a click on the sub-editor BACKGROUND closed it, "
				  "discarding the draft");
		/* The draft survives because the editor is still up and its buffer is
		 * untouched; leaving deliberately is still one Close. */
		fe_pointer_off();
		fe_tap(r.close);
		assert(screen_mps_view() == SCREEN_MPS_VIEW_MAIN
			   && screen_mps_test_server_count() == 1
			   && "Close stopped leaving the sub-editor, or it saved");
	}

	/* The title, sitting on its DEFAULT selection: a background click must
	 * not open Play game. */
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer(bg_x, bg_y);
	fe_step();
	fe_step();
	fe_screen_sets = 0;
	fe_tap(r.click);
	assert(fe_screen_sets == 0
		   && "a background click opened the title's DEFAULT selection");
	/* ... and with the pointer gone, rule (3) still lets a pad press A on a
	 * freshly opened screen. */
	fe_pointer_off();
	fe_step();
	fe_tap(r.click);
	assert(fe_screen_last == &screen_select_world
		   && "a pad with no pointer lost its press on a fresh screen");
	fe_pointer_off();
}

/* Review A6-2: the CONNECTING / BRINGUP page's Cancel was a bare sprite hit
 * test -- a second activation path in a screen the card routed, and the M-1
 * defect still live there: with no pointer (Classic, GameCube, or a dropout)
 * A did nothing at all on that page. */
static void case_connecting_cancel_is_ruled(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	fe_attach(fam);
	fe_open(&screen_multiplayer_connect_impl);
	screen_mpc_test_set_state(FE_MPC_CONNECTING);
	fe_screen_last = NULL;
	fe_tap(r.click);
	assert(fe_screen_last == &screen_title
		   && "the connecting page's Cancel is not reachable by direction + "
			  "activate");
	assert(screen_mpc_test_state() == FE_MPC_CONNECTING
		   || fe_screen_last == &screen_title);

	/* Close keeps working, unchanged. */
	fe_attach(fam);
	fe_open(&screen_multiplayer_connect_impl);
	screen_mpc_test_set_state(FE_MPC_CONNECTING);
	fe_screen_last = NULL;
	fe_tap(r.close);
	assert(fe_screen_last == &screen_title
		   && "Close stopped leaving the connecting page");
}

static void fe_mps_read_edit(char* name, size_t name_cap, char* address,
							 size_t address_cap) {
	size_t index = SIZE_MAX;
	bool is_new = true;
	assert(screen_mps_test_edit_target(&index, &is_new, name, name_cap,
								  address, address_cap));
	assert(index == 0 && !is_new);
}

static void fe_mps_open_name_field(const struct fe_roles* r) {
	/* Edit opens on safe Back. The real parent ring reaches Address, then Name. */
	fe_tap(r->up);
	fe_step();
	fe_tap(r->up);
	fe_step();
	assert(screen_mps_test_parent_focus() == 3);
	fe_tap(r->click);
	fe_step();
	assert(screen_mps_test_editor_open() && screen_mps_test_edit_field() == 0);
	/* Establish OSK directional ownership while returning to the first key. */
	fe_tap(r->right);
	fe_step();
	fe_tap(r->left);
	fe_step();
	assert(screen_mps_test_editor_target() == OSK_FRONT_CHAR_FIRST);
}

static void fe_osk_move_down_to(const struct fe_roles* r, int target) {
	for(int n = 0; screen_mps_test_editor_target() != target && n < 8; n++) {
		int before = screen_mps_test_editor_target();
		fe_tap(r->down);
		fe_step();
		assert(screen_mps_test_editor_target() != before);
	}
	assert(screen_mps_test_editor_target() == target);
}

/* The approved MP forms are two layers: field Done retains a draft and returns
 * to the parent; only parent Save persists. Field Cancel restores its opening
 * snapshot. Run this through every family/table and the live-pointer matrix so
 * directional ownership still works while Remote IR rests on blank panel space. */
static void case_mps_parent_field_done_cancel(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	char before_name[CONFIG_MP_SERVER_NAME_CAP], before_addr[CONFIG_MP_SERVER_ADDRESS_CAP];
	char draft_name[CONFIG_MP_SERVER_NAME_CAP], draft_addr[CONFIG_MP_SERVER_ADDRESS_CAP];

	fe_servers(1);
	fe_attach(fam);
	fe_mps_content(&r);
	fe_mps_action(&r, 3);
	assert(screen_mps_view() == SCREEN_MPS_VIEW_EDIT
		   && screen_mps_test_parent_focus() == 2);
	fe_mps_read_edit(before_name, sizeof(before_name), before_addr, sizeof(before_addr));

	/* Pointer-origin blank stays inert; the following directional route owns. */
	if(fam == FE_REMOTE && fe_live_pointer) {
		fe_tap(r.click);
		assert(screen_mps_view() == SCREEN_MPS_VIEW_EDIT
			   && screen_mps_test_parent_focus() == 2
			   && !screen_mps_test_editor_open());
	}
	fe_mps_open_name_field(&r);
	size_t caret = screen_mps_test_editor_caret();
	fe_tap(r.click); /* the selected real Letters key inserts 'q' */
	fe_step();
	assert(screen_mps_test_editor_caret() == caret + 1);
	fe_mps_read_edit(draft_name, sizeof(draft_name), draft_addr, sizeof(draft_addr));
	assert(strlen(draft_name) == strlen(before_name) + 1
		   && !strncmp(draft_name, before_name, strlen(before_name))
		   && draft_name[strlen(before_name)] == 'q'
		   && !strcmp(draft_addr, before_addr));

	/* Geometry-derived navigation reaches Cancel below the key grid, then Done. */
	fe_osk_move_down_to(&r, OSK_FRONT_CANCEL);
	fe_tap(r.right);
	fe_step();
	assert(screen_mps_test_editor_target() == OSK_FRONT_DONE);
	fe_hold(r.click);
	fe_current->update(fe_current, FE_DT);
	assert(!screen_mps_test_editor_open() && screen_mps_view() == SCREEN_MPS_VIEW_EDIT);
	fe_hold(r.click); /* held Done cannot reach parent Save */
	fe_current->update(fe_current, FE_DT);
	assert(screen_mps_view() == SCREEN_MPS_VIEW_EDIT);
	fe_idle();
	fe_step();
	fe_mps_read_edit(draft_name, sizeof(draft_name), draft_addr, sizeof(draft_addr));
	assert(draft_name[strlen(draft_name) - 1] == 'q');

	/* Name -> Address -> Back -> Save; persistence happens only here. */
	fe_tap(r.down); fe_step();
	fe_tap(r.down); fe_step();
	assert(screen_mps_test_parent_focus() == 2);
	fe_tap(r.right); fe_step();
	assert(screen_mps_test_parent_focus() == 1);
	fe_tap(r.click);
	assert(screen_mps_view() == SCREEN_MPS_VIEW_MAIN);

	/* Re-open the saved row, mutate once more, then Cancel the field snapshot. */
	fe_attach(fam);
	fe_mps_content(&r);
	fe_mps_action(&r, 3);
	fe_mps_read_edit(before_name, sizeof(before_name), before_addr, sizeof(before_addr));
	assert(before_name[strlen(before_name) - 1] == 'q');
	fe_mps_open_name_field(&r);
	fe_tap(r.click);
	fe_step();
	fe_mps_read_edit(draft_name, sizeof(draft_name), draft_addr, sizeof(draft_addr));
	assert(strlen(draft_name) == strlen(before_name) + 1);
	fe_osk_move_down_to(&r, OSK_FRONT_CANCEL);
	fe_hold(r.click);
	fe_current->update(fe_current, FE_DT);
	assert(!screen_mps_test_editor_open() && screen_mps_view() == SCREEN_MPS_VIEW_EDIT);
	fe_hold(r.click); /* held Cancel cannot become parent Back */
	fe_current->update(fe_current, FE_DT);
	assert(screen_mps_view() == SCREEN_MPS_VIEW_EDIT);
	fe_idle();
	fe_step();
	fe_mps_read_edit(draft_name, sizeof(draft_name), draft_addr, sizeof(draft_addr));
	assert(!strcmp(draft_name, before_name) && !strcmp(draft_addr, before_addr));

	fe_tap(r.down); fe_step();
	fe_tap(r.down); fe_step();
	assert(screen_mps_test_parent_focus() == 2);
	fe_tap(r.close);
	assert(screen_mps_view() == SCREEN_MPS_VIEW_MAIN);
}

/* Review A6-3: the three sub-editors activated Save/Cancel on a bare pointer
 * hit test -- no focus-only step, no rule. Save now needs the same two
 * deliberate presses any other target needs, and the ring opens on the
 * NON-committing answer, so a stray click cannot write the list. The
 * directional route is unchanged and is the OSK's own accept plus the Close
 * role, which this case pins as well. */
/* D3 (Astra ruling 5, closing review A16-6): in the server editor the caret
 * AND the text-input destination move together on the FOCUS-ONLY press.
 *
 * The defect: a field has no button to light, so the mandatory focus-only was
 * invisible -- no sound, no caret movement, nothing lit -- while the OSK kept
 * writing into the PREVIOUS field. A player who clicked Address and typed put
 * the address into the Name field. */
static void case_d3_editor_field_focus_moves_the_caret(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	const float addr_x=320, addr_y=258;

	fe_servers(1);
	fe_attach(FE_REMOTE);
	fe_mps_content(&r);
	fe_mps_action(&r, 3); /* DOWN off the single row into the strip, then Edit */
	assert(screen_mps_test_view() == FE_MPS_EDIT_SERVER);
	/* Let the pointer choose Name, then reacquire over Address without
	 * movement ownership. This remains a genuine focus-only press. */
	fe_pointer(320,174); fe_step(); fe_step();
	fe_pointer_off(); fe_step();
	fe_pointer(addr_x,addr_y); fe_step();
	assert(!screen_mps_test_editor_open());
	assert(screen_mps_test_edit_field() == 0
		   && !screen_mps_test_osk_writes_address());


	fe_ui_sounds = 0;
	fe_tap(r.click); /* the FOCUS-ONLY press */
	assert(screen_mps_test_edit_field() == 1
		   && "the focus-only press did not move the caret to Address");
	assert(screen_mps_test_osk_writes_address()
		   && "the caret moved but typing still landed in the OTHER field");
	/* The pinned cost is UNCHANGED: the focus-only is still silent and still
	 * opens nothing -- the ruling moved the caret, it did not add a sound or
	 * promote the press to an activate. */
	assert(!screen_mps_test_editor_open());
	assert(fe_ui_sounds == 0
		   && "the focus-only press started making a sound");
	assert(screen_mps_test_view() == FE_MPS_EDIT_SERVER
		   && "the focus-only press left the editor");

	/* The SECOND press is the activate: it reaches the field action (the
	 * click sound) and leaves the caret where the first press put it. */
	fe_step();
	fe_tap(r.click);
	assert(fe_ui_sounds == 1 && screen_mps_test_editor_open()
		&& "the second press did not open the field keyboard");
	assert(screen_mps_test_edit_field() == 1
		   && screen_mps_test_osk_writes_address());

	/* Leave without saving. */
	fe_pointer_off();
	fe_step(); fe_tap(r.close);
	assert(!screen_mps_test_editor_open() && screen_mps_test_view()==FE_MPS_EDIT_SERVER);
	fe_step(); fe_tap(r.close);
	assert(screen_mps_test_view() == FE_MPS_MAIN
		   && screen_mps_test_server_count() == 1);
}

static void case_subeditor_save_is_ruled(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	const float save_x=428, button_y=368;

	/* ADD SERVER: a click on Save is a focus-only first. */
	fe_servers(1);
	fe_attach(FE_REMOTE);
	fe_mps_content(&r);
	fe_mps_action(&r, 2); /* Add Server */
	assert(screen_mps_view() == SCREEN_MPS_VIEW_ADD);
	fe_pointer(save_x, button_y);
	fe_step();
	fe_step();
	/* The DISCRIMINATOR is the click sound: this screen plays it exactly when
	 * a press reaches the Save action [740c583 GuiScreen.mouseClicked]. Empty
	 * fields make the save itself refuse, so the VIEW alone would pass for the
	 * wrong reason. */
	fe_ui_sounds = 0;
	fe_tap(r.click);
	assert(fe_ui_sounds == 0 && screen_mps_view() == SCREEN_MPS_VIEW_ADD
		   && "the first click on a sub-editor Save reached the action");
	fe_step();
	fe_tap(r.click);
	assert(fe_ui_sounds == 0 && screen_mps_test_server_count()==1
		   && "the disabled Save action accepted a repeated press");
	fe_pointer_off();

	/* Close leaves WITHOUT saving, from the same state. */
	fe_tap(r.close);
	assert(screen_mps_view() == SCREEN_MPS_VIEW_MAIN
		   && screen_mps_test_server_count() == 1
		   && "Close on a sub-editor saved, or did not leave");

	/* A valid pointer resting on Commit cannot override a Back ring press. */
	fe_servers(1); fe_attach(FE_REMOTE); fe_mps_content(&r);
	fe_mps_action(&r,3);
	fe_pointer(save_x,button_y); fe_step(); fe_step();
	fe_tap(r.right); /* Commit -> Back, sprite remains on Commit */
	assert(screen_mps_test_parent_focus()==2);
	fe_tap(r.click);
	assert(screen_mps_view()==SCREEN_MPS_VIEW_MAIN
		&& screen_mps_test_server_count()==1);

	/* The player-name parent starts on safe Back; empty Save stays disabled. */
	fe_attach(FE_REMOTE);
	fe_mps_content(&r);
	fe_tap(r.up);
	fe_tap(r.up);
	fe_tap(r.click); /* the name editor */
	assert(screen_mps_view() == SCREEN_MPS_VIEW_NAME);
	fe_pointer(save_x, button_y);
	fe_step();
	fe_step();
	fe_ui_sounds = 0;
	fe_tap(r.click);
	assert(fe_ui_sounds == 0 && screen_mps_view() == SCREEN_MPS_VIEW_NAME
		   && "the first click on the name editor's Save reached the action");
	fe_pointer_off();
	fe_tap(r.close);
	assert(screen_mps_view() == SCREEN_MPS_VIEW_MAIN
		   && "Close did not leave the name editor");

	/* DIRECT CONNECT, same shape. */
	fe_attach(FE_REMOTE);
	fe_mps_content(&r);
	fe_mps_action(&r, 1); /* Direct Connect */
	assert(screen_mps_view() == SCREEN_MPS_VIEW_DIRECT);
	fe_pointer(save_x, button_y);
	fe_step();
	fe_step();
	fe_screen_sets = 0;
	fe_ui_sounds = 0;
	fe_tap(r.click);
	assert(fe_ui_sounds == 0 && screen_mps_view() == SCREEN_MPS_VIEW_DIRECT
		   && fe_screen_sets == 0
		   && "the first click on Direct Connect reached the action");
	fe_pointer_off();
	fe_tap(r.close);
	assert(screen_mps_view() == SCREEN_MPS_VIEW_MAIN
		   && "Close did not leave Direct Connect");
}

/* ======================================================================== */
/* A8 add-on -- the PRODUCTION focus-target / view observers for the walker   */
/* ======================================================================== */

/* These are not test hooks: screen.h publishes them and lane B's walker links
 * the real screens. The contract is that each equals what the screen itself
 * reports through the test-only accessors -- i.e. the observer is a NAMING of
 * the live focus, never a second opinion about it -- at every ring position,
 * and that it follows the highlight while the POINTER owns focus. */
static int fe_sworld_expected_focus(void) {
	int target = screen_sworld_test_focus();
	if(target >= FE_LW_BUTTON_COUNT) return SCREEN_SWORLD_FOCUS_LIST;
	switch(target) {
		case FE_LW_TAB_LOAD: return SCREEN_SWORLD_FOCUS_TAB_LOAD;
		case FE_LW_TAB_CREATE: return SCREEN_SWORLD_FOCUS_TAB_CREATE;
		case FE_LW_TAB_JOIN: return SCREEN_SWORLD_FOCUS_TAB_JOIN;
		case FE_LW_PREV: return SCREEN_SWORLD_FOCUS_PREV;
		case FE_LW_NEXT: return SCREEN_SWORLD_FOCUS_NEXT;
		case FE_LW_PLAY: return SCREEN_SWORLD_FOCUS_PLAY;
		case FE_LW_OPTIONS: return SCREEN_SWORLD_FOCUS_OPTIONS;
		default: return SCREEN_SWORLD_FOCUS_NONE;
	}
}

static int fe_mps_expected_focus(void) {
	int focus = screen_mps_test_focus();
	switch(focus) {
		case MPS_FOCUS_NONE_ORD: return SCREEN_MPS_FOCUS_NONE;
		case MPS_FOCUS_NAME_ORD: return SCREEN_MPS_FOCUS_NAME;
		case MPS_FOCUS_QUICK_ORD: return SCREEN_MPS_FOCUS_QUICK;
		case MPS_FOCUS_LIST_ORD:
			return screen_mps_test_server_count() > 0 ? SCREEN_MPS_FOCUS_LIST
													  : SCREEN_MPS_FOCUS_NONE;
		case 0: return SCREEN_MPS_FOCUS_JOIN;
		case 1: return SCREEN_MPS_FOCUS_DIRECT;
		case 2: return SCREEN_MPS_FOCUS_ADD;
		case 3: return SCREEN_MPS_FOCUS_EDIT;
		case 4: return SCREEN_MPS_FOCUS_DELETE;
		case 5: return SCREEN_MPS_FOCUS_REFRESH;
		case 6: return SCREEN_MPS_FOCUS_BACK;
		case 7: return SCREEN_MPS_FOCUS_NAME;
		case 9: return SCREEN_MPS_FOCUS_OPTIONS;
		case 10: return SCREEN_MPS_FOCUS_TAB_LOAD;
		case 11: return SCREEN_MPS_FOCUS_TAB_CREATE;
		case 12: return SCREEN_MPS_FOCUS_TAB_JOIN;
		case 13: return SCREEN_MPS_FOCUS_PREV;
		case 14: return SCREEN_MPS_FOCUS_NEXT;
		default: return SCREEN_MPS_FOCUS_NONE;
	}
}

static void case_walker_observers(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	/* A10 item 1: the row COUNT, which is what makes "no world was destroyed"
	 * a real proof on a card with several worlds (review A9-5), and the
	 * confirmation's safe-highlight observer (A9-6). Both are compared with
	 * the test-only accessors, so they are a naming of the live state. */
	for(size_t rows = 0; rows <= 3; rows++) {
		fe_saves(rows);
		fe_attach(fam);
		fe_open(&screen_select_world);
		for(int f = 0; f < 8; f++)
			fe_step();
		assert(screen_sworld_row_count() == (int)rows
			   && screen_sworld_row_count()
				   == (int)screen_sworld_test_row_count()
			   && "the row-count observer disagrees with the list");
	}
	assert(!screen_confirm_focus_is_safe()
		   && "the confirm observer answered while another screen was live");
	{
		/* Walk the confirmation: it opens on the declared safe answer, one
		 * directional step leaves it, another comes back. */
		fe_saves(3);
		fe_attach(fam);
		fe_open(&screen_select_world);
		for(int f = 0; f < 8; f++)
			fe_step();
		int before = screen_sworld_row_count();
		fe_load_delete(&r, 3); /* World options -> Delete (Load contract) */
		assert(fe_screen_last == &screen_confirm);
		fe_current = &screen_confirm;
		assert(screen_confirm_focus_is_safe()
			   && "the confirmation did not open on the safe answer");
		assert(screen_confirm_test_highlight()
				   == (int)SCREEN_CONFIRM_ANSWER_NO
			   && "the observer and the highlight disagree");
		fe_tap(r.left);
		assert(!screen_confirm_focus_is_safe()
			   && "the observer stayed safe after the highlight moved");
		fe_tap(r.left);
		assert(screen_confirm_focus_is_safe()
			   && "the observer did not follow the highlight back");
		fe_tap(r.close); /* Close answers NO and returns to the list */
		fe_current = &screen_select_world;
		assert(!screen_confirm_focus_is_safe()
			   && "the observer answered after the modal closed");
		assert(screen_sworld_row_count() == before
			   && "backing out of the delete confirm changed the row count");
	}

	/* Load: every header, row, pager and action observer names live focus. */
	fe_saves(6);
	fe_attach(fam);
	fe_open(&screen_select_world);
	for(int k = 0; k < 8; k++) fe_step();
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_LIST);
	fe_tap(r.up);
	for(int k = 0; k < 3; k++) {
		assert(screen_sworld_focus_target() == fe_sworld_expected_focus());
		fe_tap(r.right);
	}
	fe_tap(r.down);
	fe_tap(r.page); /* page 2, row 4; Prev enabled */
	fe_tap(r.right);
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_PLAY);
	fe_tap(r.up);
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_PREV);
	fe_tap(r.click); /* page 1, Next enabled */
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_NEXT);
	fe_tap(r.down);
	assert(screen_sworld_focus_target() == fe_sworld_expected_focus());
	fe_tap(r.right);
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_OPTIONS);
	fe_tap(r.click);
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_MODAL_BACK);
	fe_tap(r.up);
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_MODAL_DELETE);
	fe_tap(r.up);
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_MODAL_RENAME);
	fe_tap(r.close);
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_OPTIONS);
	fe_saves(0);
	fe_attach(fam);
	fe_open(&screen_select_world);
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_TAB_LOAD);
	fe_tap(r.down);
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_CREATE);
	/* Retry is an error action; a missing folder is now created successfully. */
	fe_saves(0);
	FILE* blocked_focus = fopen(FE_SAVES_DIR, "wb");
	assert(blocked_focus && fclose(blocked_focus) == 0);
	fe_attach(fam);
	fe_open(&screen_select_world);
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_TAB_LOAD);
	fe_tap(r.down);
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_RETRY);
	assert(remove(FE_SAVES_DIR) == 0);

	/* Multiplayer: the ring above the list, the list, and the strip. */
	fe_servers(3);
	fe_attach(fam);
	fe_mps_content(&r);
	assert(screen_mps_view() == SCREEN_MPS_VIEW_MAIN);
	assert(screen_mps_focus_target() == SCREEN_MPS_FOCUS_LIST
		   && "the server list does not report itself as the focus");
	fe_tap(r.up); /* Join header */
	assert(screen_mps_focus_target() == fe_mps_expected_focus()
		   && screen_mps_focus_target() == SCREEN_MPS_FOCUS_TAB_JOIN);
	fe_tap(r.up); /* the player-name field */
	assert(screen_mps_focus_target() == fe_mps_expected_focus()
		   && screen_mps_focus_target() == SCREEN_MPS_FOCUS_NAME);
	fe_tap(r.down);
	fe_tap(r.down); /* back onto the list */
	for(int k = 0; k < 3; k++)
		fe_tap(r.down);
	for(int k = 0; k < MPS_STRIP_LEN; k++) {
		assert(screen_mps_focus_target() == fe_mps_expected_focus()
			   && "the multiplayer observer disagrees with the live focus");
		fe_tap(r.right);
	}
	/* The sub-views, named apart where a walker acts on the difference. */
	fe_servers(3);
	fe_attach(fam);
	fe_mps_content(&r);
	fe_mps_action(&r, 2); /* Add Server */
	assert(screen_mps_test_view() == FE_MPS_EDIT_SERVER
		   && screen_mps_view() == SCREEN_MPS_VIEW_ADD
		   && "the server editor entered NEW did not report ADD");
	fe_servers(3);
	fe_attach(fam);
	fe_mps_content(&r);
	fe_mps_action(&r, 3); /* Edit, on the selected row */
	assert(screen_mps_test_view() == FE_MPS_EDIT_SERVER
		   && screen_mps_view() == SCREEN_MPS_VIEW_EDIT
		   && "the server editor entered on a row did not report EDIT");
	fe_servers(3);
	fe_attach(fam);
	fe_mps_content(&r);
	fe_mps_action(&r, 1); /* Direct Connect */
	assert(screen_mps_view() == SCREEN_MPS_VIEW_DIRECT);
	fe_attach(fam);
	fe_mps_content(&r);
	fe_tap(r.up);
	fe_tap(r.up);
	fe_tap(r.click); /* the name editor */
	assert(screen_mps_view() == SCREEN_MPS_VIEW_NAME);
}

/* The pointer half: while the POINTER owns focus the observer must report
 * what the highlight shows, not a stale ring position. Wii Remote only. */
static void case_walker_observers_pointer(void) {
	fe_saves(3);
	fe_attach(FE_REMOTE);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	float cx = 0, cy = 0;
	assert(screen_sworld_create_button_center(640, 480, &cx, &cy));
	fe_pointer(cx, cy); /* the sprite lands on Create New World */
	fe_step();
	fe_step();
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_TAB_CREATE
		   && screen_sworld_focus_target() == fe_sworld_expected_focus()
		   && "the observer did not follow the highlight under the sprite");
	fe_pointer_off();
	fe_step();
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_TAB_CREATE
		   && "losing the pointer moved the reported focus");
}

/* ======================================================================== */

static void run_family(enum fe_family fam, bool remapped) {
	fe_config_open(remapped);
	case_page_role_discrimination(fam, remapped);
	case_close_role(fam, remapped);
	case_page_roles_select_world(fam);
	case_page_roles_texture_packs(fam);
	case_page_roles_multiplayer(fam);
	case_hotbar_alias_collision(fam);
	case_reach_select_world(fam);
	/* design-wiring/load-red: the approved Load tab's own contract. */
	case_load_row_press_requires_play(fam);
	case_load_retains_selected_world(fam);
	case_load_world_options_modal(fam);
	case_load_missing_dir_and_file_retry(fam);
	case_join_geometry(fam);
	case_reach_multiplayer(fam);
	case_strip_from_selected_row_sworld(fam);
	case_strip_from_selected_row_mps(fam);
	case_strip_entry_reveals_the_row(fam);
	case_d1_confirm_safe_answer_is_directional(fam);
	case_nav_hint(fam);
	case_reach_texture_packs(fam);
	case_pack_row_selects_apply_applies(fam);
	case_pack_sound_category(fam);
	case_pack_pending_row_stays_visible(fam);
	case_pack_row_previews(fam);
	case_connect_recovery_layout(fam);
	case_reach_connect_retry(fam);
	case_mps_parent_field_done_cancel(fam);
	case_home_unchanged(fam);
	case_modal_and_rearm(fam);
	case_focus_never_stranded(fam);
	case_walker_observers(fam);
	case_connecting_cancel_is_ruled(fam);
	printf("PASS %s (%s bindings, pointer %s): Close closes, the canonical "
		   "page roles page once per press, every enabled action is reachable "
		   "by direction + activate ON THE SELECTED ROW (650-world list, row "
		   "300), disabled ones are skipped and never stranded, HOME is "
		   "unchanged\n",
		   fe_family_name[fam], remapped ? "remapped" : "shipped",
		   fe_live_pointer ? "LIVE" : "absent");
}

/* ======================================================================== */
/* A3 item 5 -- the read-only cross-lane seam for lane B's mp_play_10m       */
/* ======================================================================== */

/* CONTRACT (Astra ruling 2 item 7 -- the prototypes and the full wording are
 * in source/game/gui/screen.h): each predicate answers "would an activate
 * press THIS frame open Multiplayer / start the join", by asking the SAME
 * decision helper the press itself asks. Every case below ASKS the predicate
 * and then actually presses the button, so the two can never drift: the
 * assertion is always "the observer equals what happened". */

static void case_focus_seam(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);

	/* --- title: focus on the Play game row --------------------------------- */
	/* THE APPROVED TITLE (VISUAL-COORDINATION.md section 6): the standalone
	 * Multiplayer row is GONE, and the observer's question becomes "would a
	 * fresh press open the PLAY path" (the Load / Create / Join session behind
	 * screen_select_world). Every ask goes through fe_title_play: the NEW
	 * public screen_title_focus_is_play, with the legacy transition alias
	 * screen_title_focus_is_multiplayer asserted equal at every point (section
	 * 6 item 1: one line, retired by the joint seam commit). Row order is Play game /
	 * Texture packs / Options / Quit; the title OPENS on Play game, so from the
	 * opening focus the Play row is reached by ZERO GUI_DOWN steps, and a core
	 * that navigates by GUI_DOWN + re-ask finds it true before its first step.
	 * With no pointer the opening selection is a DEFAULT one, which rule (3)
	 * lets a pad press -- so the observer is TRUE at open. */
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	assert(fe_title_play()
		   && "the title does not open with a pad press landing on Play game");
	/* The predicate is exactly the activate: pressing A here opens it. */
	fe_tap(r.click);
	assert(fe_screen_last == &screen_select_world
		   && "the seam said Play but the activate went elsewhere");

	/* ... and false on every OTHER row, including at the clamp (a DOWN past
	 * Quit moves nothing and must not turn the answer around). */
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	for(int k = 1; k <= FE_TB_COUNT; k++) {
		fe_tap(r.down);
		assert(!fe_title_play()
			   && "the seam claimed Play with focus on another row");
	}
	/* ... and true again once UP walks it back onto the top row. */
	for(int k = 0; k < FE_TB_COUNT - 1; k++)
		fe_tap(r.up);
	assert(fe_title_play()
		   && "UP back to the top row does not rest on Play game");

	/* A8 / R-A: once the POINTER has armed a selection and the sprite then
	 * rests on NO control, the press does nothing -- so the observer must say
	 * false, and keep saying it through any number of IR validity
	 * transitions. This is the title's twin of the A6-1 sequence. */
	{
		fe_attach(FE_REMOTE);
		fe_open(&screen_title);
		fe_pointer(fe_title_cx(FE_TB_PLAY), fe_title_cy(FE_TB_PLAY));
		fe_step();
		fe_step();
		assert(fe_title_play()
			   && "the pointer did not arm Play game");
		fe_pointer(2.0F, 2.0F); /* a real move, onto no control */
		fe_step();
		fe_step();
		assert(!fe_title_play()
			   && "the observer promised a press that lands on nothing");
		fe_flicker(20, true, 2.0F, 2.0F);
		assert(!fe_title_play()
			   && "IR validity flicker changed the observer's answer");
		fe_screen_sets = 0;
		fe_tap(r.click);
		assert(fe_screen_sets == 0
			   && "the observer and the press disagree over empty space");
		fe_pointer_off();
	}

	/* A3 NEW-5, the FALSE NEGATIVE: with the POINTER owning focus and resting
	 * ON Play game, an activate DOES open it, so the observer must say so.
	 * Round 2's version returned false whenever the pointer owned. */
	{
		fe_attach(FE_REMOTE);
		fe_open(&screen_title);
		fe_pointer(fe_title_cx(FE_TB_PLAY), fe_title_cy(FE_TB_PLAY));
		fe_step();
		fe_step();
		assert(fe_title_play()
			   && "the observer denied a press that the pointer would land on "
				  "Play game");
		fe_tap(r.click);
		assert(fe_screen_last == &screen_select_world
			   && "the observer and the press disagree under pointer "
				  "ownership");

		/* A stationary sprite rests on a different control from the ring's;
		 * the observer must predict the ring's Play action. The pointer first arms
		 * Tutorial world, the row below Play (it owns focus on its first valid
		 * frame), then UP walks the ring to Play game with the sprite unmoved. */
		fe_attach(FE_REMOTE);
		fe_open(&screen_title);
		fe_pointer(fe_title_cx(FE_TB_TUTORIAL), fe_title_cy(FE_TB_TUTORIAL));
		fe_step();
		fe_step();
		assert(!fe_title_play()
			   && "the seam claimed Play with the sprite armed on Tutorial world");
		fe_tap(r.up); /* ring walks to Play game, sprite unmoved */
		fe_step();
		assert(screen_title_test_highlight() == FE_TB_PLAY);
		assert(fe_title_play()
			   && "the observer missed a ring Play press under a stationary sprite");
		fe_screen_sets = 0;
		fe_tap(r.click);
		assert(fe_screen_last == &screen_select_world
			   && "the observer and the ring Play press disagree");

		/* NOT YET RE-ARMED. The frames below run NO update, so nothing about
		 * the screen changes between them -- the only thing that moves is
		 * whether the activate button carries a fresh edge. */
		fe_attach(FE_REMOTE);
		fe_open(&screen_title);
		fe_pointer_off();
		fe_step();
		fe_tap(r.down);
		fe_tap(r.up); /* ring on Play game, no pointer, button released */
		assert(fe_title_play());
		fe_hold(r.click); /* down: a fresh edge is pending */
		assert(fe_title_play()
			   && "the observer denied a press whose edge was already there");
		fe_hold(r.click); /* still down: the edge is spent, none can arrive */
		assert(!fe_title_play()
			   && "the observer promised a press while the button was still "
				  "held from the last one");
		fe_idle();
		fe_tap(r.click);
		assert(fe_screen_last == &screen_select_world
			   && "a fresh press after the hold did not open Play game");
	}
	fe_pointer_off();

	/* Join's default header and selectable rows are never connection actions. */
	fe_servers(3); fe_attach(FE_REMOTE);
	fe_open(&screen_multiplayer_setup_impl);
	assert(!screen_mps_focus_is_join());
	fe_pointer(2,40); fe_step(); fe_step();
	fe_tap(r.click);
	assert(!fe_screen_sets && !screen_mps_focus_is_join());
	fe_pointer_off(); fe_step(); fe_tap(r.down);
	assert(!screen_mps_focus_is_join());
	fe_tap(r.click); /* selected row -> explicit Join */
	assert(screen_mps_test_focus() == MPS_BUTTON_JOIN_ORD);
	assert(!screen_mps_focus_is_join()); /* missing player name */
	char pending[CONFIG_MP_SERVER_ADDRESS_CAP];
	fe_tap(r.click);
	assert(screen_mps_test_view() == FE_MPS_EDIT_NAME
		&& screen_mps_test_pending_join(pending,sizeof(pending)));

	assert(config_set_string(&gstate.config_user,"mp.username","Tester"));
	screen_multiplayer_connect=&screen_multiplayer_connect_impl;
	/* Rejoin driver's continuous abstract press: on the real two-row screen,
	 * DOWN enters retained row zero, then changing the held request to RIGHT
	 * reaches Join without ever selecting row one. Keeping RIGHT asserted for
	 * another frame must be inert until the driver's neutral gap. */
	fe_servers(2); fe_attach(FE_REMOTE);
	fe_open(&screen_multiplayer_setup_impl);
	assert(screen_mps_test_selection() == 0 && screen_mps_test_focus() == 12);
	fe_hold(r.down); fe_current->update(fe_current, FE_DT);
	assert(screen_mps_test_selection() == 0
		&& screen_mps_test_focus() == MPS_FOCUS_LIST_ORD);
	fe_hold(r.right); fe_current->update(fe_current, FE_DT);
	assert(screen_mps_test_selection() == 0
		&& screen_mps_test_focus() == MPS_BUTTON_JOIN_ORD
		&& "DOWN->RIGHT rejoin route changed the retained endpoint");
	fe_hold(r.right); fe_current->update(fe_current, FE_DT);
	assert(screen_mps_test_selection() == 0
		&& screen_mps_test_focus() == MPS_BUTTON_JOIN_ORD);
	fe_idle();
	fe_tap(r.click);
	assert(fe_screen_last == &screen_multiplayer_connect_impl
		&& screen_mps_test_selection() == 0);

	fe_servers(3); fe_attach(FE_REMOTE); fe_mps_content(&r);
	assert(!screen_mps_focus_is_join());
	fe_tap(r.down);
	assert(!screen_mps_focus_is_join());
	fe_tap(r.click);
	assert(screen_mps_focus_is_join());
	fe_tap(r.click);
	assert(fe_screen_last == &screen_multiplayer_connect_impl);
	/* Availability, other actions, empty list and editors cannot claim Join. */
	fe_servers(3); fe_attach(FE_REMOTE); fe_mps_content(&r);
	fe_mps_focus_action(&r,0);
	screen_multiplayer_connect=NULL;
	assert(!screen_mps_focus_is_join());
	screen_multiplayer_connect=&screen_multiplayer_connect_impl;
	fe_mps_focus_action(&r,1); assert(!screen_mps_focus_is_join());
	fe_mps_action(&r,2); assert(!screen_mps_focus_is_join());
	fe_servers(0); fe_attach(FE_REMOTE); fe_mps_content(&r);
	assert(!screen_mps_focus_is_join());

	/* Directional intent remains valid over empty space; held A is not an edge. */
	fe_servers(3); fe_attach(FE_REMOTE); fe_mps_content(&r);
	fe_mps_focus_action(&r,0);
	fe_pointer(2,40); fe_step(); fe_step();
	assert(screen_mps_focus_is_join());
	fe_pointer_off(); fe_step();
	assert(screen_mps_focus_is_join());
	fe_hold(r.click); assert(screen_mps_focus_is_join());
	fe_hold(r.click); assert(!screen_mps_focus_is_join());
	fe_idle(); fe_tap(r.click);
	assert(fe_screen_last == &screen_multiplayer_connect_impl);

	/* Pointer-owned Join stays inert over empty space, including IR flicker. */
	fe_servers(3); fe_attach(FE_REMOTE); fe_mps_content(&r);
	fe_pointer(2,40); fe_step();
	fe_pointer(238,388); fe_step(); fe_step();
	assert(screen_mps_focus_is_join());
	fe_pointer(2,40); fe_step(); fe_step();
	assert(!screen_mps_focus_is_join());
	fe_flicker(20,true,2,40);
	assert(!screen_mps_focus_is_join());
	fe_screen_sets=0; fe_tap(r.click); assert(!fe_screen_sets);
	/* A different row under the stationary pointer consumes the first press. */
	fe_pointer(320,178); fe_step(); fe_step();
	assert(!screen_mps_focus_is_join());
	fe_tap(r.click);
	assert(screen_mps_test_selection()==1 && !fe_screen_sets);
	fe_pointer(2,40); fe_step(); fe_step();
	assert(!screen_mps_focus_is_join());
	fe_tap(r.click); assert(!fe_screen_sets);
	fe_pointer_off();
	assert(config_set_string(&gstate.config_user,"mp.username",""));
	screen_multiplayer_connect=NULL;
	screen_multiplayer_setup=NULL;

	puts("PASS focus seam: screen_title_focus_is_play (and its transition "
		 "alias screen_title_focus_is_multiplayer, asserted equal at every ask) "
		 "and screen_mps_focus_is_join are true exactly when a directional "
		 "activate would open Play game / start the join");
}

/* ======================================================================== */
/* design-wiring/title-red -- the approved title's Play row and geometry     */
/* ======================================================================== */

/* THE FRESH PLAY ENTRY (GATE3-PLAY-PLAN.md item 5; VISUAL-COORDINATION.md
 * section 11): a real title activation of Play game must CLEAR the retained
 * Play session -- the owned world identity, the remembered server (name,
 * address, index hint) and the create-world draft -- and enter
 * screen_select_world on the LOAD tab. Driven against the REAL play_session
 * module (linked, not doubled), pre-populated with all four pieces of state so
 * a partial reset fails by name rather than by luck. Every other title row
 * leaves the session exactly as populated: only Play resets it.
 *
 * Runs for all three controller families (the pad route), plus the two
 * observer answers section 6 item 3 asks of every query: "another screen is
 * live" = false and "the activate button is still held" = false.
 *
 * GREEN: every observer ask here and in case_focus_seam goes through
 * fe_title_play, which asks the NEW screen_title_focus_is_play and asserts the
 * legacy alias screen_title_focus_is_multiplayer answers the same, at every
 * point (open, after DOWN, after UP, under the held button, with another
 * screen live, and under the pointer). */
static void fe_play_session_populate(void) {
	struct cworld_form draft, back;
	assert(play_session_set_tab(PLAY_TAB_JOIN));
	assert(play_session_remember_world(FE_SAVES_DIR "/world02"));
	assert(play_session_remember_server("Server 01", "10.0.0.2:25565", 1));
	cworld_form_reset(&draft);
	snprintf(draft.name, sizeof(draft.name), "%s", "Retained Draft");
	snprintf(draft.seed, sizeof(draft.seed), "%s", "12345");
	draft.mode = CWORLD_MODE_CREATIVE;
	draft.focus = CWORLD_ROW_SEED;
	play_session_store_draft(&draft);
	/* The fixture really is populated -- otherwise "cleared" proves nothing. */
	assert(play_session_tab() == PLAY_TAB_JOIN);
	assert(play_session_world_dir()
		   && !strcmp(play_session_world_dir(), FE_SAVES_DIR "/world02"));
	assert(!strcmp(play_session_server_name(), "Server 01")
		   && !strcmp(play_session_server_address(), "10.0.0.2:25565")
		   && play_session_server_index() == 1);
	cworld_form_reset(&back);
	assert(play_session_restore_draft(&back)
		   && !strcmp(back.name, "Retained Draft")
		   && !strcmp(back.seed, "12345") && back.mode == CWORLD_MODE_CREATIVE
		   && back.focus == CWORLD_ROW_SEED);
}

/* The fresh state play_session_enter_fresh promises, asserted piece by piece
 * so the failing assertion names WHICH piece a partial reset kept. */
static void fe_assert_play_session_fresh(void) {
	struct cworld_form back;
	cworld_form_reset(&back);
	assert(play_session_tab() == PLAY_TAB_LOAD
		   && "Play game did not enter the session on the Load tab");
	assert(play_session_world_dir() == NULL
		   && "Play game kept the remembered world identity");
	assert(!strcmp(play_session_server_name(), "")
		   && !strcmp(play_session_server_address(), "")
		   && play_session_server_index() == SIZE_MAX
		   && "Play game kept the remembered server");
	assert(!play_session_restore_draft(&back)
		   && "Play game kept the create-world draft");
}

static void fe_assert_play_session_populated(void) {
	struct cworld_form back;
	cworld_form_reset(&back);
	/* EVERY populated field, not just the tab / name / has-draft (independent
	 * review gap): a partial clear of the address or of the draft's seed, mode
	 * or focus must fail here by name. */
	assert(play_session_tab() == PLAY_TAB_JOIN
		   && "a title row other than Play game moved the retained tab");
	assert(play_session_world_dir()
		   && !strcmp(play_session_world_dir(), FE_SAVES_DIR "/world02")
		   && "a title row other than Play game touched the remembered world");
	assert(!strcmp(play_session_server_name(), "Server 01")
		   && !strcmp(play_session_server_address(), "10.0.0.2:25565")
		   && play_session_server_index() == 1
		   && "a title row other than Play game touched the remembered server");
	assert(play_session_restore_draft(&back)
		   && !strcmp(back.name, "Retained Draft") && !strcmp(back.seed, "12345")
		   && back.mode == CWORLD_MODE_CREATIVE && back.focus == CWORLD_ROW_SEED
		   && "a title row other than Play game touched the create-world draft");
}

static void case_title_play_fresh_entry(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);

	/* (1) The pad route: the title opens on Play game, the observer says so,
	 * one press enters Select World and the session is fresh. */
	fe_play_session_populate();
	fe_attach(fam);
	fe_open(&screen_title);
	assert(screen_title_test_highlight() == FE_TB_PLAY
		   && "the title does not open on Play game");
	assert(fe_title_play()
		   && "the observer denied the opening Play game focus");
	fe_tap(r.click);
	assert(fe_screen_last == &screen_select_world && fe_screen_sets == 1
		   && "Play game did not enter Select World");
	fe_assert_play_session_fresh();

	/* (2) Reached by re-ask: DOWN leaves it (observer false), UP returns
	 * (observer true), and the press clears the session the same way. */
	fe_play_session_populate();
	fe_attach(fam);
	fe_open(&screen_title);
	fe_tap(r.down);
	assert(!fe_title_play()
		   && "one DOWN from Play game still claims Play");
	fe_assert_play_session_populated(); /* navigating is not entering */
	fe_tap(r.up);
	assert(fe_title_play()
		   && "UP back onto Play game is not Play");
	fe_tap(r.click);
	assert(fe_screen_last == &screen_select_world
		   && "Play game reached by DOWN + UP did not enter Select World");
	fe_assert_play_session_fresh();

	/* (3) The OTHER four rows never touch the session (Tutorial world with no
	 * world in the fixture only opens its explanation dialog). */
	{
		struct {
			int downs;
			struct screen* opens; /* NULL: Quit, observed on gstate.quit */
		} others[] = {
			{1, &screen_confirm},
			{2, &screen_texturepacks},
			{3, &screen_options},
			{4, NULL},
		};
		for(size_t k = 0; k < sizeof(others) / sizeof(others[0]); k++) {
			fe_play_session_populate();
			fe_attach(fam);
			fe_open(&screen_title);
			for(int d = 0; d < others[k].downs; d++)
				fe_tap(r.down);
			assert(!fe_title_play());
			gstate.quit = false;
			fe_tap(r.click);
			if(others[k].opens)
				assert(fe_screen_last == others[k].opens
					   && "a title row is not reachable by direction + activate");
			else
				assert(gstate.quit && "Quit is not reachable by direction + activate");
			gstate.quit = false;
			fe_assert_play_session_populated();
		}
	}

	/* (4) HELD activate = no fresh edge (section 6 item 3). The frames below
	 * run NO update, so the session stays populated until a real press. */
	fe_play_session_populate();
	fe_attach(fam);
	fe_open(&screen_title);
	fe_hold(r.click); /* down: a fresh edge is pending */
	assert(fe_title_play()
		   && "the observer denied a press whose edge was already there");
	fe_hold(r.click); /* still down: the edge is spent */
	assert(!fe_title_play()
		   && "the observer promised a press while the button was still held");
	fe_assert_play_session_populated();
	fe_idle();
	fe_tap(r.click);
	assert(fe_screen_last == &screen_select_world
		   && "a fresh press after the hold did not enter Play");
	fe_assert_play_session_fresh();

	/* (5) Another screen is live: its own update owns the press, so the title
	 * observer must answer false whatever the title's last state was. */
	fe_play_session_populate();
	fe_attach(fam);
	fe_open(&screen_title);
	assert(fe_title_play());
	fe_saves(1);
	fe_open(&screen_select_world);
	assert(!fe_title_play()
		   && "the title observer answered while another screen was live");
	fe_saves(0);
	play_session_enter_fresh(); /* leave the module as the next case expects */

	printf("PASS title Play fresh entry (%s): Play game clears the retained "
		   "world / server / draft and enters Select World on the Load tab; "
		   "Texture packs / Options / Quit leave it untouched; the observer "
		   "is false under a held button and with another screen live\n",
		   fe_family_name[fam]);
}

/* THE FIVE ROWS' HIT BOUNDS (ui-build-1026 T03: (160, 166 + 48 * i, 320, 44)
 * in 640x480). gutil_button_hit is lower-inclusive / upper-exclusive, so each
 * row is probed at its four INSIDE edges (x 160 and 479, y top and top + 45 --
 * the 44th pixel, which is what pins the 44 px height) plus its centre, and
 * at one pixel PAST each edge (x 159 and 480, y top - 1 and top + 44) plus the
 * far side of the 4 px gap below it. Inside: the pointer owns focus on its
 * first valid frame, the highlight is that row, and one press does that row's
 * action -- which is also the four-row reachability proof by pointer:
 * Play game -> Select World, Texture packs -> Texture Packs, Options ->
 * Options, Quit -> gstate.quit. Outside: the highlight stays on the opening
 * DEFAULT selection and the press is refused (R-E: a DEFAULT selection fires
 * only while the pointer is INVALID, never on a background click).
 *
 * REMOTE ONLY, like every pointer case in this suite. */
static void case_title_row_geometry(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	assert(gfx_width() == 640 && gfx_height() == 480
		   && "the approved title rows are pinned in 640x480 pixels");
	static const struct {
		struct screen* opens; /* NULL: Quit, observed on gstate.quit */
	} action[FE_TB_COUNT] = {
		{&screen_select_world},
		{&screen_confirm}, /* Tutorial world: fixture has none -> dialog */
		{&screen_texturepacks},
		{&screen_options},
		{NULL},
	};

	for(int row = 0; row < FE_TB_COUNT; row++) {
		struct fe_rect b = fe_title_row(row);
		assert(b.x == 160 && b.y == 166 + 48 * row && b.w == 320 && b.h == 44);
		const float cx = (float)(b.x + b.w / 2), cy = (float)(b.y + b.h / 2);
		const float inside[][2] = {
			{(float)b.x, cy},             /* left edge, inclusive */
			{(float)(b.x + b.w - 1), cy}, /* last column inside */
			{cx, (float)b.y},             /* top edge, inclusive */
			{cx, (float)(b.y + b.h - 1)}, /* the 44th pixel row */
			{cx, cy},
		};
		const float outside[][2] = {
			{(float)(b.x - 1), cy},   /* one past the left edge */
			{(float)(b.x + b.w), cy}, /* the exclusive right edge */
			{cx, (float)(b.y - 1)},   /* one above: the gap, or above row 0 */
			{cx, (float)(b.y + b.h)}, /* the exclusive bottom edge */
			{cx, (float)(b.y + b.h + 3)}, /* the far side of the 4 px gap */
		};

		for(size_t k = 0; k < sizeof(inside) / sizeof(inside[0]); k++) {
			fe_attach(FE_REMOTE);
			fe_open(&screen_title);
			fe_pointer(inside[k][0], inside[k][1]);
			fe_step();
			fe_step();
			assert(screen_title_test_highlight() == row
				   && "a pointer INSIDE a title row did not highlight it");
			gstate.quit = false;
			fe_screen_last = NULL;
			fe_screen_sets = 0;
			fe_tap(r.click);
			if(action[row].opens)
				assert(fe_screen_last == action[row].opens && fe_screen_sets == 1
					   && !gstate.quit
					   && "a press inside a title row did not do that row's "
						  "action");
			else
				assert(gstate.quit && fe_screen_sets == 0
					   && "a press inside the Quit row did not quit");
			gstate.quit = false;
		}

		for(size_t k = 0; k < sizeof(outside) / sizeof(outside[0]); k++) {
			fe_attach(FE_REMOTE);
			fe_open(&screen_title);
			fe_pointer(outside[k][0], outside[k][1]);
			fe_step();
			fe_step();
			assert(screen_title_test_highlight() == FE_TB_PLAY
				   && "a pointer OUTSIDE every title row moved the highlight");
			gstate.quit = false;
			fe_screen_last = NULL;
			fe_screen_sets = 0;
			fe_tap(r.click);
			assert(!gstate.quit && fe_screen_sets == 0
				   && "a press one pixel outside a title row activated "
					  "something (the row's bounds, or R-E, is wrong)");
		}
	}
	fe_pointer_off();
	gstate.quit = false;
}

int main(void) {
	/* Pure arithmetic on the pack screen's own text helper: no screen, no
	 * input, no files, so it runs before the sandbox moves the cwd. */
	case_pack_text_fit();
	/* Likewise pure arithmetic: the backdrop crop, recorded off the header's
	 * own emit. It restores the zeroed link double before it returns, so the
	 * screen cases below still take the no-backdrop fallback. */
	case_pack_backdrop_uv();

	/* Read the one source file this suite pins as TEXT before anything moves
	 * the working directory (see case_load_world_hold_unchanged's note). */
	case_load_world_hold_unchanged();

	/* Then sandbox everything: the destructive paths below really delete
	 * worlds and really persist the server list through
	 * config_save(…, "config.json"), whose path is a hard-coded relative
	 * literal. ctest runs this target from the SOURCE ROOT, so without this
	 * the suite would rewrite the repository's own config.json. */
	/* Review finding NEW-6: fe_rmdir is rmdir, which removes only an EMPTY
	 * directory, so an ABORTED run left its populated scratch tree behind and
	 * the next run silently continued inside it -- config.json, the remap
	 * config and any non-world<NN> leftovers included. Purge the exact set of
	 * files this suite can create before re-creating the directory, so a reuse
	 * is clean by construction rather than by luck. */
	{
		static const char* const stale[] = {
			FE_RUN_DIR "/config.json", FE_RUN_DIR "/config_wii.json",
			FE_RUN_DIR "/" FE_REMAP_CONFIG,
		};
		for(size_t k = 0; k < sizeof(stale) / sizeof(stale[0]); k++) {
			remove(stale[k]);
			/* A23: case_nav_hint makes config.json a DIRECTORY on purpose (a
			 * save that really fails, for the A20-3 overlap rule). Windows'
			 * remove() will not delete a directory, so an aborted run would
			 * leave one where the next run's texture-pack apply expects to be
			 * able to write -- the exact "silently continued inside it" trap
			 * finding NEW-6 is about, one file later. */
			fe_rmdir(stale[k]);
		}
		for(size_t k = 0; k < 32; k++) {
			char path[192];
			snprintf(path, sizeof(path),
					 FE_RUN_DIR "/" FE_SAVES_DIR "/world%02zu", k);
			fe_rmdir(path);
		}
		fe_rmdir(FE_RUN_DIR "/" FE_SAVES_DIR);
		/* The Retry case can leave a regular file here after an assertion. */
		remove(FE_RUN_DIR "/" FE_SAVES_DIR);
		/* The A15 long-list fixture, same reasoning: rmdir removes only an
		 * EMPTY directory, so an aborted run must not leave 650 rows for the
		 * next one to inherit silently. */
		for(size_t k = 0; k < FE_BIG_LIST; k++) {
			char path[192];
			snprintf(path, sizeof(path),
					 FE_RUN_DIR "/" FE_BIG_SAVES_DIR "/world%03zu", k);
			fe_rmdir(path);
		}
		fe_rmdir(FE_RUN_DIR "/" FE_BIG_SAVES_DIR);
	}
	fe_rmdir(FE_RUN_DIR);
	assert((fe_mkdir(FE_RUN_DIR) == 0 || errno == EEXIST)
		   && "could not create the scratch run directory");
	fe_copy_file("config_wii.json", FE_RUN_DIR "/config_wii.json");
	assert(fe_chdir(FE_RUN_DIR) == 0 && "could not enter the scratch run dir");

	/* The pointer cases run under BOTH binding tables: the activation rule is
	 * role-driven, so a remap must not change one answer (and the Remote is
	 * the only family with an IR pointer at all). */
	for(int remapped = 0; remapped < 2; remapped++) {
		fe_config_open(remapped != 0);
		case_pointer_arbitration_title();
		case_pointer_arbitration_confirm();
		case_confirm_destructive_reconciles_focus();
		case_pointer_arbitration_picker();

		case_confirm_reacquire();
		case_title_reacquire();
		case_title_validity_flicker();
		case_picker_reacquire();
		case_pointer_jitter();
		case_pointer_and_dpad_same_frame();
		case_held_activate_across_focus_only();
		case_pointer_over_disabled_control();
		case_pointer_enters_over_button();
		/* design-wiring/title-red: the four approved rows' hit bounds and the
		 * pointer route to each row's action (Remote-only, like the rest of
		 * this loop; the families' pad route is case_title_play_fresh_entry). */
		case_title_row_geometry();
		/* design-wiring/load-red: the Load tab's control rects by pointer. */
		case_load_geometry_pointer();
		/* Gate 2: the approved pack card is wider than Beta's GuiSlot hit
		 * band, and the pointer must reach all of it (Remote-only, like every
		 * case in this loop -- the three families' directional coverage of
		 * this screen is in run_family and is unchanged). */
		case_pack_row_pointer_reach();
		case_a6_1_empty_space_after_drop();
		case_a9_1_destructive_needs_intent();
		case_a9_2_background_click_changes_nothing();
		case_origin_is_sticky();
		case_navigate_and_activate_consumes();
		printf("PASS activation rule (%s bindings): a valid pointer on another "
			   "target activates NEITHER, a disabled control consumes the "
			   "press, empty space activates nothing, IR validity alone never "
			   "moves focus (both phases), same-frame navigation cannot "
			   "activate an undisplayed target, and one hold cannot supply "
			   "the press after its own focus move\n",
			   remapped ? "remapped" : "shipped");
	}


	/* Review finding H-1: every case above used to run only with the pointer
	 * ABSENT, so a whole class of defect -- focus state that exists only when
	 * there is no pointer -- was untested. The identical cases now run a
	 * second time with a LIVE Wii Remote pointer parked where it hits no
	 * control, so directional focus has to ARBITRATE with it rather than be
	 * partitioned off by it. */
	for(int live = 0; live < 2; live++) {
		fe_live_pointer = live != 0;
		for(int remapped = 0; remapped < 2; remapped++)
			for(int fam = 0; fam < FE_FAMILY_COUNT; fam++)
				run_family((enum fe_family)fam, remapped != 0);
	}
	fe_live_pointer = false;

	/* R-C needs a table nothing ships: activate and Close on ONE button. */
	fe_config_open_json(fe_collide_json);
	for(int fam = 0; fam < FE_FAMILY_COUNT; fam++)
		case_consumed_press_reaches_nothing_else((enum fe_family)fam);
	puts("PASS R-C: a press the activation rule consumed reaches no other "
		 "handler in that frame, on a table where activate IS Close");

	fe_config_open(false);
	case_walker_observers_pointer();
	case_d3_editor_field_focus_moves_the_caret();
	case_subeditor_save_is_ruled();
	case_confirm_safe_answer_is_the_callers_word();
	case_focus_seam();
	/* design-wiring/title-red: the Play row's fresh entry against the REAL
	 * play_session, on all three families' shipped tables. */
	for(int fam = 0; fam < FE_FAMILY_COUNT; fam++)
		case_title_play_fresh_entry((enum fe_family)fam);

	fe_saves(0);
	for(size_t k = 0; k < FE_BIG_LIST; k++) {
		char path[192];
		snprintf(path, sizeof(path), FE_BIG_SAVES_DIR "/world%03zu", k);
		fe_rmdir(path);
	}
	fe_rmdir(FE_BIG_SAVES_DIR);
	remove(FE_REMAP_CONFIG);
	remove("config_wii.json");
	remove("config.json");
	config_destroy(&gstate.config_user);
	assert(fe_chdir("..") == 0);
	fe_rmdir(FE_RUN_DIR);
	return 0;
}
