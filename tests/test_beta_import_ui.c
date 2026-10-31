/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * "Import Beta 1.7.3" UI (tmp/jobs/ingame-import-1030, coder C).
 *
 * The REAL Texture Packs screen and the REAL import progress screen, driven
 * through the REAL PLATFORM_WII input.c on the libogc doubles (the
 * test_frontend_no_pointer rig, #included whole as test_texturepacks_screen
 * does), against a scriptable beta_import.h backend
 * (tests/support/fake_beta_import.c) that plays the worker's side:
 *   1. no jar   -> no Import card, the one-line hint, the old walk;
 *   2. geometry -> the Import card is list row 0 in the approved card rect
 *                  and never a walker target; the progress screen's parts
 *                  inside the canvas at 4:3, 16:9 and split;
 *   3. every controller family (Remote+Nunchuk, Classic, GameCube):
 *      UP + activate reaches the card, the progress screen polls once per
 *      frame and never blocks, the bar follows done/total, Close cancels,
 *      OK returns, and a finished import comes back to Texture Packs with
 *      "Beta 1.7.3" applied for textures AND sounds through the real
 *      transactions (the Pack Helper's audio_select rule);
 *   4. the pointer: one click on the card, on Cancel, on the footer prompt;
 *   5. every backend state: scanning (no fill), textures, sounds, swapping,
 *      done with and without sounds, failed, cancelled, start refused;
 *   6. in-game: never offered, and the SD card is not even probed;
 *   7. re-import of the current pack forces the texture + sound reloads.
 */
#define main bi_unused_frontend_main
#include "test_frontend_no_pointer.c"
#undef main

#include "game/gui/screen_beta_import.h"
#include "support/fake_beta_import.h"
#include "texturepacks.h"

#ifdef _WIN32
#include <process.h>
#define GETPID() _getpid()
#else
#include <unistd.h>
#define GETPID() getpid()
#endif

#define BI_ROW_TARGET(i) (3 + (int)(i)) /* TEXPACKS_TARGET_ROW */
#define BI_PACK "texturepacks/Beta-1.7.3" /* beta_import_internal.h BI_PACK_NAME */

extern int screen_texpacks_test_import_override;
bool screen_texpacks_test_import_shown(void);
bool screen_texpacks_test_button_rect(int button, int width, int height,
									  int* x, int* y, int* w, int* h);
int screen_texpacks_test_import_row(void);
void screen_texpacks_test_seed(size_t extra);
const char* screen_texpacks_test_pack_name(size_t i);

/* Mount the fake SD root at this test's private working directory. */
const char* __wrap_path_from_sd_root(const char* path, char* buf, size_t size) {
	(void)buf;
	(void)size;
	return !strncmp(path, "sd:/", 4) ? path + 4 : path;
}

/* Every string the two screens hand to gutil_text_rgb, per render. */
#define BI_TEXT_LOG 64
static char bi_text[BI_TEXT_LOG][160];
static int bi_text_count;
void __real_gutil_text_rgb(int x, int y, const char* s, int size, bool shadow,
						   uint8_t r, uint8_t g, uint8_t b);
void __wrap_gutil_text_rgb(int x, int y, const char* s, int size, bool shadow,
						   uint8_t r, uint8_t g, uint8_t b) {
	if(bi_text_count < BI_TEXT_LOG)
		snprintf(bi_text[bi_text_count++], sizeof(bi_text[0]), "%s", s);
	__real_gutil_text_rgb(x, y, s, size, shadow, r, g, b);
}
static bool bi_drew(const char* s) {
	for(int k = 0; k < bi_text_count; k++)
		if(strstr(bi_text[k], s))
			return true;
	return false;
}
static void bi_render(void) {
	bi_text_count = 0;
	fe_render(fe_current);
}

/* Texture reloads / sound re-decodes the pack transactions asked for. */
static int bi_reloads, bi_sound_reloads;
bool __real_tex_init(void);
void screen_texturepacks_reload_frame(void);
unsigned screen_texpacks_test_reload_frames(void);
bool __wrap_tex_init(void) {
	bi_reloads++;
	/* What main's loader service does every 66 ms mid-reload. */
	screen_texturepacks_reload_frame();
	return __real_tex_init();
}
void __real_audio_registry_reload(void);
void __wrap_audio_registry_reload(void) {
	bi_sound_reloads++;
	__real_audio_registry_reload();
}

/* The rig's fe_current does not follow a screen_set made inside an update. */
static void bi_follow(void) {
	fe_current = gstate.players[0].current_screen;
}
static void bi_tap(int code) {
	fe_tap(code);
	bi_follow();
}
static void bi_frames(int n) {
	for(int k = 0; k < n; k++) {
		fe_step();
		bi_follow();
	}
}

static void bi_set(bi_state st, int done, int total, const char* msg) {
	fake_bi.progress.state = st;
	fake_bi.progress.done = done;
	fake_bi.progress.total = total;
	snprintf(fake_bi.progress.msg, sizeof(fake_bi.progress.msg), "%s",
			 msg ? msg : "");
}

/* UP until the ring is on the Import card (row 0). */
static void bi_to_card(const struct fe_roles* r) {
	for(int k = 0; k < 8 && screen_texpacks_test_highlight() != BI_ROW_TARGET(0); k++)
		fe_tap(r->up);
	assert(screen_texpacks_test_highlight() == BI_ROW_TARGET(0));
}

static void bi_open_packs(enum fe_family fam, bool jar) {
	fake_beta_import_reset();
	fake_bi.available = jar;
	fe_attach(fam);
	screen_texturepacks_set_return(&screen_title);
	screen_texturepacks_set_ingame(false);
	fe_open(&screen_texturepacks);
}

/* ---- 1. no jar: no card, the hint, the old walk ---------------------------- */
static void case_no_jar(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	bi_open_packs(FE_REMOTE, false);
	assert(fake_bi.available_calls == 1 && "the jar probe runs once, on entry");
	assert(!screen_texpacks_test_import_shown());
	assert(screen_texpacks_test_import_row() == -1);
	assert(screen_texpacks_test_row_count() == screen_texpacks_test_pack_count());
	bi_render();
	assert(bi_drew("To import Beta 1.7.3, put your b1.7.3.jar in sd:/beta/"));
	assert(!bi_drew("From your b1.7.3.jar"));
	fe_tap(r.right);
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_CATEGORY
		   && "without a jar the walk is the old one");
	for(int k = 0; k < 30; k++)
		fe_step();
	assert(fake_bi.available_calls == 1 && "the SD card is never probed per frame");
	assert(fake_bi.starts == 0);
	puts("PASS no jar: no Import card, one-line hint, unchanged walk, one "
		 "probe per entry");
}

/* ---- 2. geometry ----------------------------------------------------------- */
static void case_geometry(void) {
	bi_open_packs(FE_REMOTE, true);
	assert(screen_texpacks_test_import_shown());
	/* The Import card is the FIRST row -- the list's own card rect, pitch and
	 * fonts (pinned by test_frontend_no_pointer) -- one row more than packs. */
	assert(screen_texpacks_test_import_row() == 0);
	assert(screen_texpacks_test_row_count()
		   == screen_texpacks_test_pack_count() + 1);
	bi_render();
	assert(bi_drew("From your b1.7.3.jar in sd:/beta/"));
	assert(!bi_drew("To import Beta 1.7.3") && "no hint when the card is there");
	/* Never a walker target: menus_dwell must not start an import. */
	float cx, cy;
	assert(!screen_texpacks_row_center(0, 640, 480, &cx, &cy));
	assert(screen_texpacks_row_center(1, 640, 480, &cx, &cy));

	static const int sizes[][2] = {{640, 480}, {854, 480}, {640, 240}};
	struct screen_beta_import_layout l;
	for(size_t k = 0; k < 3; k++) {
		int W = sizes[k][0], H = sizes[k][1];
		float u = (float)H / 480.0F;
		int ox = (W - (int)(640.0F * u + 0.5F)) / 2;
		screen_beta_import_layout(W, H, &l);
		/* Panel 32,86 576x276: bar and status inside it; button and footer
		 * below it, inside the screen. */
		int px = ox + (int)(32 * u + 0.5F), py = (int)(86 * u + 0.5F);
		int pr = px + (int)(576 * u + 0.5F), pb = py + (int)(276 * u + 0.5F);
		assert(l.bar.x - 2 >= px && l.bar.x + l.bar.w + 2 <= pr);
		assert(l.bar.y >= py && l.bar.y + l.bar.h <= pb);
		assert(l.status.x >= px && l.status.x + l.status.w <= pr);
		/* 10-31: the footer is the shared hint row (fixed 28 px strip, TV-safe
		 * bottom 24). This global screen only ever renders at gfx_height()
		 * == 480 (wii/gfx.c), so the scaled 240 canvas keeps only the
		 * inside-the-screen rule below. */
		assert(l.button.y >= pb);
		if(H >= 480) assert(l.button.y + l.button.h <= l.footer.y);
		assert(l.footer.y + l.footer.h <= H);
		assert(l.button.x >= ox
			   && l.button.x + l.button.w <= ox + (int)(640 * u + 0.5F));
		if(W == 640 && H == 480) {
			assert(l.bar.x == 120 && l.bar.y == 150 && l.bar.w == 400
				   && l.bar.h == 12);
			assert(l.button.x == 168 && l.button.y == 378 && l.button.w == 304
				   && l.button.h == 44);
		}
		/* 4 status lines at a 24 px pitch end inside the panel. */
		assert(l.status.y + 3 * (int)(24 * u + 0.5F) + l.status.h <= pb);
	}
	puts("PASS geometry: Import card = list row 0 in the approved card rect, "
		 "not a walker target; progress bar/status inside the panel, button "
		 "and footer below, 4:3, 16:9, split");
}

/* ---- 3. one controller family, start to finish ----------------------------- */
static void bi_pack_fixture(bool on) {
	if(on) {
		assert(fe_mkdir("texturepacks") == 0 || errno == EEXIST);
		assert(fe_mkdir(BI_PACK) == 0 || errno == EEXIST);
		assert(fe_mkdir(BI_PACK "/sound") == 0 || errno == EEXIST);
		assert(fe_mkdir(BI_PACK "/sound/fixture") == 0 || errno == EEXIST);
		assert(fe_mkdir("assets") == 0 || errno == EEXIST);
		FILE* m = fopen(BI_PACK "/.cavewii-pack.json", "wb");
		assert(m);
		fputs("{\"name\": \"Beta 1.7.3\"}\n", m);
		fclose(m);
		FILE* f = fopen(BI_PACK "/sound/fixture/tone0.ogg", "wb");
		assert(f);
		/* Generated bytes (a counter pattern), never a real sound: only
		 * presence is checked by the audio eligibility scan here. */
		for(int b = 0; b < 64; b++)
			fputc(b * 7 & 0xFF, f);
		fclose(f);
		return;
	}
	remove(BI_PACK "/sound/fixture/tone0.ogg");
	remove(BI_PACK "/.cavewii-pack.json");
	fe_rmdir(BI_PACK "/sound/fixture");
	fe_rmdir(BI_PACK "/sound");
	fe_rmdir(BI_PACK);
	fe_rmdir("texturepacks");
	fe_rmdir("assets");
}

static void case_family(enum fe_family fam) {
	struct fe_roles r = fe_roles_for(fam);
	bi_pack_fixture(true);
	config_set_string(&gstate.config_user, "paths.texturepack", "assets");
	config_set_string(&gstate.config_user, "paths.audiopack", "assets");

	/* --- cancel path --- */
	bi_open_packs(fam, true);
	assert(screen_texpacks_test_highlight() == BI_ROW_TARGET(1)
		   && "entry still rests on the current pack");
	fe_tap(r.right);
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_CATEGORY
		   && "the strip walk is unchanged");
	fe_tap(r.up);
	fe_tap(r.up);
	assert(screen_texpacks_test_highlight() == BI_ROW_TARGET(0)
		   && "UP reaches the Import card");
	bi_render();
	assert(bi_drew("Makes a Beta 1.7.3 pack from your own b1.7.3.jar."));
	bi_tap(r.click);
	assert(fe_current == &screen_beta_import && fake_bi.starts == 1);
	assert(fake_bi.selector && "the selector is registered before start");
	assert(screen_beta_import_test_view() == SCREEN_BETA_IMPORT_RUNNING);
	assert(!strcmp(screen_beta_import_test_button_label(), "Cancel"));

	/* The held activate press must not reach the new screen. */
	int polls = fake_bi.polls;
	bi_frames(20);
	assert(fake_bi.polls == polls + 20 && "one non-blocking poll per frame");
	assert(fake_bi.cancels == 0 && fe_current == &screen_beta_import);
	assert(screen_beta_import_test_bar_permille() == -1
		   && "scanning has no known total: no fill");
	bi_render();
	assert(bi_drew("Looking for your Beta files"));
	assert(bi_drew("Cancel"));

	bi_set(BI_TEXTURES, 10, 40, "terrain.png");
	bi_frames(1);
	assert(screen_beta_import_test_bar_permille() == 250);
	bi_render();
	assert(bi_drew("Step 1 of 3: Copying textures") && bi_drew("10 of 40 files")
		   && bi_drew("terrain.png"));

	bi_set(BI_SOUNDS, 3, 4, "");
	bi_frames(1);
	assert(screen_beta_import_test_bar_permille() == 750);
	bi_render();
	assert(bi_drew("Step 2 of 3: Converting sounds") && bi_drew("3 of 4 sounds"));

	bi_tap(r.close);
	assert(fake_bi.cancels == 1);
	assert(screen_beta_import_test_view() == SCREEN_BETA_IMPORT_CANCELLING);
	assert(!screen_beta_import_test_button_enabled());
	bi_tap(r.click); /* a disabled Cancel does nothing */
	bi_tap(r.close);
	assert(fake_bi.cancels == 1 && "cancel is asked once");
	bi_render();
	assert(bi_drew("Stopping the import"));
	bi_set(BI_CANCELLED, 0, 0, "");
	bi_frames(1);
	assert(screen_beta_import_test_view() == SCREEN_BETA_IMPORT_CANCELLED);
	assert(!strcmp(screen_beta_import_test_button_label(), "OK"));
	bi_render();
	assert(bi_drew("Import cancelled") && bi_drew("Nothing was changed."));
	bi_tap(r.click);
	assert(fe_current == &screen_texturepacks);
	assert(!screen_texpacks_busy() && "a cancelled import applies nothing");
	assert(!strcmp(config_read_string(&gstate.config_user, "paths.texturepack",
									  ""), "assets"));

	/* --- done path, with sounds: textures AND sounds become active --- */
	bi_open_packs(fam, true);
	bi_to_card(&r);
	bi_tap(r.click);
	assert(fe_current == &screen_beta_import && fake_bi.starts == 1);
	bi_set(BI_SWAPPING, 0, 0, "");
	bi_frames(1);
	assert(screen_beta_import_test_bar_permille() == -1);
	bi_render();
	assert(bi_drew("Step 3 of 3: Finishing up"));
	assert(!fake_bi.selector_called && "nothing selected before the swap ends");
	bi_set(BI_DONE, 0, 0, "");
	fake_bi.progress.sounds_imported = true;
	fake_bi.progress.sounds_count = 175;
	bi_frames(1);
	assert(fake_bi.selector_called && fake_bi.selector_result);
	assert(screen_beta_import_test_view() == SCREEN_BETA_IMPORT_DONE);
	bi_render();
	assert(bi_drew("Beta 1.7.3 imported"));
	assert(bi_drew("Beta 1.7.3 textures and 175 sounds are now active."));
	int reloads = bi_reloads, sound_reloads = bi_sound_reloads;
	bi_tap(r.click);
	assert(fe_current == &screen_texturepacks);
	assert(screen_texpacks_busy() && "the imported pack applies at once");
	assert(screen_texpacks_test_pending() < FE_TP_NO_PENDING);
	bi_frames(12);
	assert(!screen_texpacks_busy());
	assert(bi_reloads == reloads + 1 && bi_sound_reloads == sound_reloads + 1);
	assert(texturepacks_value_eq(config_read_string(&gstate.config_user,
													"paths.texturepack", ""),
								 BI_PACK)
		   && "returns with Beta 1.7.3 textures selected");
	{
		/* pack-lag-1030: the reload presented live frames; outside a reload
		 * the service draws nothing. */
		unsigned live = screen_texpacks_test_reload_frames();
		assert(live > 0 && "no live frame during the reload");
		screen_texturepacks_reload_frame();
		assert(screen_texpacks_test_reload_frames() == live);
	}
	assert(texturepacks_value_eq(config_read_string(&gstate.config_user,
													"paths.audiopack", ""),
								 BI_PACK)
		   && "and Beta 1.7.3 sounds selected");
	assert(screen_texpacks_test_category() == FE_TP_CAT_TEXTURES);
	{
		/* The list shows the manifest name, the value stays the folder. */
		bool named = false;
		for(size_t i = 0; i < screen_texpacks_test_pack_count(); i++)
			named = named || !strcmp(screen_texpacks_test_pack_name(i), "Beta 1.7.3");
		assert(named && "Beta-1.7.3 is listed as Beta 1.7.3");
	}
	bi_render();
	assert(bi_drew("Beta 1.7.3 textures and sounds applied."));

	/* --- re-import of the current pack: both reloads forced --- */
	bi_open_packs(fam, true);
	bi_to_card(&r);
	bi_tap(r.click);
	bi_set(BI_DONE, 0, 0, "");
	bi_frames(1);
	bi_render();
	assert(bi_drew("Beta 1.7.3 textures are now active."));
	assert(bi_drew("No sounds found: put your .minecraft resources folder in "
				   "sd:/beta/resources to add them."));
	reloads = bi_reloads;
	sound_reloads = bi_sound_reloads;
	bi_tap(r.close); /* Close also leaves a result */
	assert(fe_current == &screen_texturepacks && screen_texpacks_busy());
	bi_frames(12);
	assert(!screen_texpacks_busy());
	assert(bi_reloads == reloads + 1 && bi_sound_reloads == sound_reloads + 1
		   && "same pack name, new files: both reloads are forced");
	assert(texturepacks_value_eq(config_read_string(&gstate.config_user,
													"paths.texturepack", ""),
								 BI_PACK));

	/* --- failed path, left with HOME --- */
	bi_open_packs(fam, true);
	bi_to_card(&r);
	bi_tap(r.click);
	bi_set(BI_FAILED, 0, 0, "The jar in sd:/beta/ is damaged.");
	bi_frames(1);
	assert(screen_beta_import_test_view() == SCREEN_BETA_IMPORT_FAILED);
	bi_render();
	assert(bi_drew("The import didn't work"));
	assert(bi_drew("The jar in sd:/beta/ is damaged."));
	assert(bi_drew("Your texture pack was not changed."));
	assert(!fake_bi.selector_called);
	bi_tap(r.home);
	assert(fe_current == &screen_texturepacks && !screen_texpacks_busy());

	config_set_string(&gstate.config_user, "paths.texturepack", "assets");
	config_set_string(&gstate.config_user, "paths.audiopack", "assets");
	bi_pack_fixture(false);
	printf("PASS family %d: UP reaches the Import card, one poll per frame, "
		   "bar = done/total, Close cancels once, OK/Close/HOME leave, done -> "
		   "Beta 1.7.3 textures+sounds applied (re-import forces both "
		   "reloads), failed/cancelled change nothing\n",
		   (int)fam);
}

/* ---- 4. pointer ------------------------------------------------------------ */
static void case_pointer(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	const float x = 320.0F,
				y = (float)(FE_TP_ROW_TOP_PX(0) + FE_TP_ROW_ACTION_PX / 2);
	bi_open_packs(FE_REMOTE, true);
	fe_pointer(320, 470);
	fe_step();
	fe_pointer(x, y);
	fe_step();
	fe_step();
	bi_tap(r.click);
	assert(fe_current == &screen_beta_import && "one click on the Import card");

	struct screen_beta_import_layout l;
	screen_beta_import_layout(640, 480, &l);
	fe_pointer((float)(l.button.x + l.button.w / 2),
			   (float)(l.button.y + l.button.h / 2));
	bi_frames(2);
	bi_tap(r.click);
	assert(fake_bi.cancels == 1 && "one click on Cancel");
	bi_set(BI_CANCELLED, 0, 0, "");
	bi_frames(1);
	screen_beta_import_layout(640, 480, &l);
	fe_pointer((float)(l.footer.x + l.footer.w / 2),
			   (float)(l.footer.y + l.footer.h / 2));
	bi_frames(2);
	bi_tap(r.click);
	assert(fe_current == &screen_texturepacks && "the footer prompt is the button");

	/* A pointer resting on nothing: a press does nothing (no stray cancel). */
	bi_open_packs(FE_REMOTE, true);
	fe_pointer(320, 470);
	fe_step();
	fe_pointer(x, y);
	fe_step();
	fe_step();
	bi_tap(r.click);
	assert(fe_current == &screen_beta_import);
	fe_pointer(600, 300);
	bi_frames(2);
	bi_tap(r.click);
	assert(fake_bi.cancels == 0 && fe_current == &screen_beta_import);
	fe_pointer_off();
	bi_set(BI_FAILED, 0, 0, "x");
	bi_frames(1);
	bi_tap(r.click);
	assert(fe_current == &screen_texturepacks);
	puts("PASS pointer: one click each on the Import card, Cancel and the "
		 "footer prompt; a click on empty space never cancels");
}

/* ---- 5. start refused, in-game ------------------------------------------ */
static void case_refused_and_ingame(void) {
	struct fe_roles r = fe_roles_for(FE_GAMECUBE);
	bi_open_packs(FE_GAMECUBE, true);
	fake_bi.start_refuse = true;
	bi_to_card(&r);
	bi_tap(r.click);
	assert(fe_current == &screen_beta_import);
	assert(screen_beta_import_test_view() == SCREEN_BETA_IMPORT_FAILED);
	bi_render();
	assert(bi_drew("The import couldn't start."));
	bi_tap(r.click);
	assert(fe_current == &screen_texturepacks);

	fake_beta_import_reset();
	fake_bi.available = true;
	fe_attach(FE_GAMECUBE);
	screen_texturepacks_set_ingame(true);
	fe_open(&screen_texturepacks);
	assert(fake_bi.available_calls == 0 && "in-game: the card is not probed");
	assert(!screen_texpacks_test_import_shown());
	assert(screen_texpacks_test_import_row() == -1);
	bi_render();
	assert(!bi_drew("To import Beta 1.7.3") && !bi_drew("From your b1.7.3.jar"));
	screen_texturepacks_set_ingame(false);
	puts("PASS refused start says so plainly; in-game: no card, no hint, "
		 "no SD probe");
}

/* ---- 6. pointer-1030: D-pad + A makes a NEW selection on every pad ------- */
static int bi_pointer_binds;
void __real_gfx_bind_texture(struct tex_gfx* t);
void __wrap_gfx_bind_texture(struct tex_gfx* t) {
	if(t == &texture_pointer)
		bi_pointer_binds++;
	__real_gfx_bind_texture(t);
}

static void case_pad_new_selection(enum fe_family fam, bool card) {
	struct fe_roles r = fe_roles_for(fam);
	bi_open_packs(fam, card);
	screen_texpacks_test_seed(3); /* pack00..pack02 after the default */
	int base = card ? 1 : 0;      /* row of the default pack */
	assert(screen_texpacks_test_highlight() == BI_ROW_TARGET(base));
	/* Pick pack00 with D-pad + A. */
	fe_tap(r.down);
	assert(screen_texpacks_test_highlight() == BI_ROW_TARGET(base + 1));
	fe_tap(r.click);
	assert(screen_texpacks_test_pending() == 1 && "D-pad + A selects");
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_APPLY);
	/* The maintainer's case: a NEW selection after the first one. */
	fe_tap(r.down); /* the ring leaves Apply for the list ... */
	fe_tap(r.down); /* ... and walks on */
	fe_tap(r.click);
	assert(screen_texpacks_test_pending() == 3 && "a second D-pad + A selects again");
	fe_tap(r.up);
	fe_tap(r.click);
	assert(screen_texpacks_test_pending() == 2);
	/* All the way up: the Import card (when shown) is reachable, the
	 * default pack is selectable. */
	for(int k = 0; k < 6; k++)
		fe_tap(r.up);
	assert(screen_texpacks_test_highlight() == BI_ROW_TARGET(0));
	if(!card) {
		fe_tap(r.click);
		assert(screen_texpacks_test_pending() == 0);
	}
}

static void case_pointer_visible(void) {
	/* A LIVE pointer is drawn on both screens; an absent one is not. */
	bi_open_packs(FE_REMOTE, true);
	fe_pointer(600, 300);
	fe_step();
	bi_pointer_binds = 0;
	bi_render();
	assert(bi_pointer_binds == 1 && "Texture Packs draws the IR cursor");
	fe_pointer_off();
	fe_step();
	bi_pointer_binds = 0;
	bi_render();
	assert(bi_pointer_binds == 0);
	screen_beta_import_open();
	bi_follow();
	fe_pointer(600, 300);
	bi_frames(1);
	bi_pointer_binds = 0;
	bi_render();
	assert(bi_pointer_binds == 1 && "the import screen draws the IR cursor");
	fe_pointer_off();
	bi_set(BI_FAILED, 0, 0, "x");
	bi_frames(1);
	bi_tap(fe_roles_for(FE_REMOTE).click);
	assert(fe_current == &screen_texturepacks);
	puts("PASS pointer-1030: the IR cursor is drawn on Texture Packs and the "
		 "import screen whenever it is live");
}

/* ---- drift-1030: a slowly drifting Remote never steals the D-pad's Apply - */
static void case_drift(bool slow) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	bi_open_packs(FE_REMOTE, false);
	screen_texpacks_test_seed(3);
	/* The cursor rests on the default pack's row (row 0), as in the footage. */
	float x = 320.0F, y = (float)(FE_TP_ROW_TOP_PX(0) + FE_TP_ROW_ACTION_PX / 2);
	fe_pointer(x, y);
	for(int k = 0; k < 10; k++)
		fe_step();
	fe_tap(r.down);
	fe_tap(r.down);
	fe_tap(r.click);
	assert(screen_texpacks_test_pending() == 2 && "D-pad + A chose pack01");
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_APPLY);
	if(slow) {
		/* 0.3 px a frame for 2 s: 36 px of hand drift, still over rows. */
		for(int k = 0; k < 120; k++) {
			y += 0.3F;
			fe_pointer(x, y);
			fe_step();
		}
		assert(screen_texpacks_test_highlight() == FE_TP_BTN_APPLY
			   && "drift took the ring off Apply");
		fe_tap(r.click);
		assert(screen_texpacks_busy() && "A on the ringed Apply applies");
		for(int k = 0; k < 5; k++)
			fe_step();
	} else {
		/* A real point: 20 px in two frames onto row 1 -- the pointer wins. */
		fe_pointer(x, y + 10.0F);
		fe_step();
		fe_pointer(x, y + 70.0F);
		fe_step();
		fe_tap(r.click);
		assert(!screen_texpacks_busy());
		assert(screen_texpacks_test_pending() == 1 && "a deliberate point still selects");
	}
	fe_pointer_off();
}

/* ---- toast-1030: controller notices never cover a heading ----------------- */
static int bi_notice_x, bi_notice_y, bi_notice_w, bi_notice_h, bi_notice_calls;
void __real_screen_notice_compose(int x, int y, int w, int h, bool in_header);
void __wrap_screen_notice_compose(int x, int y, int w, int h, bool in_header) {
	bi_notice_x = x; bi_notice_y = y; bi_notice_w = w; bi_notice_h = h;
	bi_notice_calls++;
	__real_screen_notice_compose(x, y, w, h, in_header);
}
void screen_texpacks_test_notice_rect(int width, int height, int* x, int* y,
									  int* w, int* h);

static void case_notice_mounts(void) {
	/* Texture Packs: the note row, between the heading and the panel. */
	bi_open_packs(FE_REMOTE, false);
	bi_notice_calls = 0;
	bi_render();
	assert(bi_notice_calls == 1 && "Texture Packs mounts its notice");
	int x, y, w, h;
	screen_texpacks_test_notice_rect(640, 480, &x, &y, &w, &h);
	assert(bi_notice_x == x && bi_notice_y == y && bi_notice_w == w);
	assert(y >= 22 + 28 && "below the 28 px heading at y 22");
	assert(y + h <= 86 && "above the panel");
	assert(h >= 28 && w >= 300 && "a whole notice card fits");
	int cx, cy, cw, ch;
	assert(screen_texpacks_test_button_rect(FE_TP_BTN_CATEGORY, 640, 480, &cx,
											&cy, &cw, &ch));
	assert(y >= cy + ch && "clear of the category toggle");

	/* Play game: the header field right of the title, above the tabs. */
	fe_saves(0);
	fe_attach(FE_REMOTE);
	fe_open(&screen_select_world);
	fe_step();
	bi_notice_calls = 0;
	bi_render();
	assert(bi_notice_calls == 1 && "Play game mounts its notice");
	assert(bi_notice_x >= 326 && "right of the \"Play game\" title (88..326)");
	assert(bi_notice_y >= 14 && bi_notice_y + bi_notice_h <= 50 && "above the tabs");
	assert(bi_notice_h >= 28 && bi_notice_w >= 280);
	puts("PASS toast-1030: Texture Packs and Play game mount controller notices "
		 "clear of their headings, toggles and tabs");
}

/* ---- toast-1030: every rig-reachable menu leaves a notice spot ----------- */
void gutil_test_set_font_widths(const uint8_t widths[256]);
static const uint8_t bi_font_widths[256] = { /* assets/font/default.png */
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	4,3,4,5,5,6,5,2,3,3,3,3,2,3,1,4,5,4,5,5,5,5,5,5,5,5,1,0,3,3,3,5,
	0,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,7,5,5,5,3,4,3,5,5,
	2,5,5,4,4,4,4,4,4,1,4,4,3,7,4,4,4,4,4,4,3,4,5,7,4,4,4,4,1,4,7,2,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
extern struct screen screen_multiplayer_setup_impl;
extern struct screen screen_multiplayer_connect_impl;
void screen_confirm_setup(const char*, const char*, const char*, const char*,
						  enum screen_confirm_answer, void (*)(bool));
static void bi_confirm_cb(bool yes) { (void)yes; }

/* What screen.c's overlay does around a menu's content: record its text and
 * art, then either the screen's own mount or a free slot must hold the card
 * without touching any of it. */
void gutil_test_force_notice_width(int width);
static void bi_walk_one(const char* name, int width, int height) {
	bi_notice_calls = 0;
	/* A notice IS up: screens that make room for one (a caption that steps
	 * aside) must show that layout. */
	gutil_test_force_notice_width(260);
	gutil_text_log_begin();
	fe_current->render2D(fe_current, width, height);
	gutil_text_log_end();
	gutil_test_force_notice_width(0);
	const int card_w = 260, card_h = 28; /* "Player 1 Connected" + picture */
	/* notice-spot-1031: whatever the screen composes, the floating card is at
	 * the ONE top-left anchor -- same left edge on every menu, nudged down only
	 * below a heading, inside the top band, never another corner. (A screen's
	 * own non-header mount is ignored by screen.c now.) */
	(void)bi_notice_calls;
	int x, y;
	bool free = gutil_notice_free_spot(width, height, card_w, card_h, &x, &y);
	if(x != GUTIL_NOTICE_ANCHOR_X || y < GUTIL_NOTICE_ANCHOR_Y
	   || y + card_h > height / 4)
		fprintf(stderr, "%s: notice at %d,%d (%dx%d)\n", name, x, y, width, height);
	assert(x == GUTIL_NOTICE_ANCHOR_X && "one left edge on every menu");
	assert(y >= GUTIL_NOTICE_ANCHOR_Y && y + card_h <= height / 4
		   && "top band only: never bottom-left");
	if(!gutil_notice_spot_hits(GUTIL_NOTICE_ANCHOR_X, GUTIL_NOTICE_ANCHOR_Y,
							   card_w, card_h))
		assert(y == GUTIL_NOTICE_ANCHOR_Y && free);
	if(free)
		assert(!gutil_notice_spot_hits(x, y, card_w, card_h));
	/* The title (the maintainer's repro): its logo keep-out sits under the
	 * anchor and the buttons sit under the logo, so the card stays AT the
	 * anchor -- top-left, as in gameplay -- not bottom-left. */
	if(!strcmp(name, "title"))
		assert(x == GUTIL_NOTICE_ANCHOR_X && y == GUTIL_NOTICE_ANCHOR_Y);
	printf("notice-spot %s %dx%d -> %d,%d%s\n", name, width, height, x, y,
		   free ? "" : " (anchor, overlaps art)");
}

static void case_notice_walk(void) {
	gutil_test_set_font_widths(bi_font_widths);
	static const int sizes[][2] = {{854, 480}, {640, 480}};
	int screens = 0;
	for(int k = 0; k < 2; k++) {
		int W = sizes[k][0], H = sizes[k][1];
		fe_attach(FE_REMOTE);
		fe_open(&screen_title); fe_step();
		bi_walk_one("title", W, H); screens++;
		fe_saves(3); fe_open(&screen_select_world); fe_step();
		bi_walk_one("play game", W, H); screens++;
		bi_open_packs(FE_REMOTE, true);
		bi_walk_one("texture packs", W, H); screens++;
		screen_texturepacks_set_ingame(true);
		fe_open(&screen_texturepacks);
		bi_walk_one("texture packs in-game", W, H); screens++;
		screen_texturepacks_set_ingame(false);
		screen_beta_import_open(); bi_follow();
		bi_walk_one("import", W, H); screens++;
		bi_set(BI_FAILED, 0, 0, "This jar isn't Beta 1.7.3 (it's a newer version). Use b1.7.3.jar.");
		bi_frames(1);
		bi_walk_one("import failed", W, H); screens++;
		screen_confirm_setup("Are you sure?", "This world will be lost forever!",
							 "Delete", "Cancel", SCREEN_CONFIRM_ANSWER_NO, bi_confirm_cb);
		fe_open(&screen_confirm); fe_step();
		bi_walk_one("confirm", W, H); screens++;
		fe_open(&screen_multiplayer_setup_impl); fe_step();
		bi_walk_one("multiplayer", W, H); screens++;
		fe_open(&screen_multiplayer_connect_impl); fe_step();
		bi_walk_one("connect", W, H); screens++;
		/* screen_creative_picker draws real items, which this rig cannot
		 * render; it is covered by the engine-linked suites. */
	}
	fe_saves(0);
	static const uint8_t zero[256];
	gutil_test_set_font_widths(zero);
	printf("PASS toast-1030 walk: %d screen renders (9 menus x 4:3 + 16:9), "
		   "each notice mounted or placed clear of every heading, tab and "
		   "label\n", screens);
}

static void case_pointer_toast_menus(void) {
	/* #02: "Pointer out of view - Use stick" is gameplay advice. */
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	/* Aim at the screen long enough to count (750 ms), then away long
	 * enough to be lost (1500 ms): 16 ms per rig frame. */
	for(int k = 0; k < 600; k++) fe_step(); /* let attach notices expire */
	gutil_notices_menu_context(false);
	assert(gutil_connection_notice_width(0, false, false) == 0);
	fe_pointer(300, 300);
	for(int k = 0; k < 60; k++) fe_step();
	fe_pointer_off();
	for(int k = 0; k < 110; k++) fe_step();
	gutil_notices_menu_context(false);
	int gameplay = gutil_connection_notice_width(0, false, false);
	gutil_notices_menu_context(true);
	int menu = gutil_connection_notice_width(0, false, false);
	gutil_notices_menu_context(false);
	printf("pointer-lost notice width: gameplay %d, menu %d\n", gameplay, menu);
	assert(gameplay > 0 && "the pointer-lost notice exists in gameplay");
	assert(menu == 0 && "and is hidden while a menu is current");
	puts("PASS toast-1030: the pointer-out-of-view notice shows in gameplay only");
}

/* ---- 7. pack names (maintainer ruling 2026-10-30) ------------------------ */
static const char* const bi_ea_files[] = {
	"terrain.png", "gui/items.png", "gui/gui.png", "anim.png",
	"font/default.png", "mob/char.png", "pointer.png",
};
static void bi_write(const char* path, int seed) {
	FILE* f = fopen(path, "wb");
	assert(f);
	for(int b = 0; b < 300; b++)
		fputc((b * 13 + seed) & 0xFF, f); /* generated bytes, not art */
	fclose(f);
}
static void bi_ea_tree(const char* root, bool on) {
	static const char* const dirs[] = {"gui", "font", "mob"};
	char p[256];
	if(on) {
		assert(fe_mkdir(root) == 0 || errno == EEXIST);
		for(int k = 0; k < 3; k++) {
			snprintf(p, sizeof(p), "%s/%s", root, dirs[k]);
			assert(fe_mkdir(p) == 0 || errno == EEXIST);
		}
		for(size_t k = 0; k < sizeof(bi_ea_files) / sizeof(*bi_ea_files); k++) {
			snprintf(p, sizeof(p), "%s/%s", root, bi_ea_files[k]);
			bi_write(p, (int)k);
		}
		return;
	}
	for(size_t k = 0; k < sizeof(bi_ea_files) / sizeof(*bi_ea_files); k++) {
		snprintf(p, sizeof(p), "%s/%s", root, bi_ea_files[k]);
		remove(p);
	}
	for(int k = 0; k < 3; k++) {
		snprintf(p, sizeof(p), "%s/%s", root, dirs[k]);
		fe_rmdir(p);
	}
	fe_rmdir(root);
}
static bool bi_listed(const char* name) {
	for(size_t i = 0; i < screen_texpacks_test_pack_count(); i++)
		if(!strcmp(screen_texpacks_test_pack_name(i), name))
			return true;
	return false;
}
static void case_pack_names(void) {
	assert(fe_mkdir("texturepacks") == 0 || errno == EEXIST);
	bi_ea_tree("assets", true);
	bi_ea_tree("texturepacks/earth-and-ash", true);
	assert(fe_mkdir("texturepacks/isabella-ii") == 0 || errno == EEXIST);
	FILE* m = fopen("texturepacks/isabella-ii/.cavewii-pack.json", "wb");
	assert(m);
	fputs("{\"name\": \"Isabella II (CavEX Legacy)\"}", m);
	fclose(m);

	bi_open_packs(FE_REMOTE, false);
	assert(!strcmp(screen_texpacks_test_pack_name(0), "Earth & Ash (default)"));
	assert(!bi_listed("earth-and-ash") && !bi_listed("Earth & Ash (pack)")
		   && "a byte-identical copy of the default is hidden");
	assert(bi_listed("Isabella II (CavEX Legacy)"));
	assert(screen_texpacks_test_pack_count() == 2);

	bi_write("texturepacks/earth-and-ash/terrain.png", 99); /* now differs */
	bi_open_packs(FE_REMOTE, false);
	assert(bi_listed("Earth & Ash (pack)") && !bi_listed("earth-and-ash"));
	assert(screen_texpacks_test_pack_count() == 3);

	remove("texturepacks/isabella-ii/.cavewii-pack.json");
	fe_rmdir("texturepacks/isabella-ii");
	bi_ea_tree("texturepacks/earth-and-ash", false);
	bi_ea_tree("assets", false);
	fe_rmdir("texturepacks");
	puts("PASS pack names: \"Earth & Ash (default)\", an identical earth-and-ash "
		 "copy hidden, a different one \"Earth & Ash (pack)\", Isabella II by "
		 "its manifest name");
}

int main(void) {
	/* The sandbox lives under the system temp dir, never in the repo (it
	 * holds a generated pack folder), and is removed at the end. */
	char scratch[512], config_path[600];
	const char* tmp = getenv("TMP");
	if(!tmp || !tmp[0])
		tmp = getenv("TEMP");
	if(!tmp || !tmp[0])
		tmp = "/tmp";
	snprintf(scratch, sizeof(scratch), "%s/cw_beta_import_ui_run_%ld", tmp,
			 (long)GETPID());
	snprintf(config_path, sizeof(config_path), "%s/config_wii.json", scratch);
	assert(fe_mkdir(scratch) == 0 || errno == EEXIST);
	fe_copy_file("config_wii.json", config_path);
	assert(fe_chdir(scratch) == 0);
	fe_config_open(false);
	case_no_jar();
	case_geometry();
	case_family(FE_REMOTE);
	case_family(FE_CLASSIC);
	case_family(FE_GAMECUBE);
	case_pointer();
	case_refused_and_ingame();
	for(int fam = FE_REMOTE; fam <= FE_GAMECUBE; fam++) {
		case_pad_new_selection((enum fe_family)fam, false);
		case_pad_new_selection((enum fe_family)fam, true);
	}
	puts("PASS pointer-1030: D-pad + A makes a first AND a new selection on "
		 "Remote+Nunchuk, Classic and GameCube, with and without the Import card");
	case_pointer_visible();
	case_pack_names();
	case_notice_mounts();
	case_notice_walk();
	case_pointer_toast_menus();
	case_drift(true);
	case_drift(false);
	puts("PASS drift-1030: 36 px of slow hand drift keeps the D-pad's ring on "
		 "Apply and A applies; a deliberate point still takes the row");
	fe_saves(0);
	config_destroy(&gstate.config_user);
	remove("config.json");
	remove("config_wii.json");
	assert(fe_chdir("..") == 0);
	assert(fe_rmdir(scratch) == 0 && "sandbox left behind");
	puts("PASS test_beta_import_ui");
	return 0;
}
