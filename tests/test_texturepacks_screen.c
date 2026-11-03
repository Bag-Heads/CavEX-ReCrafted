/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 * Real screens + real Wii input; only hardware/render/archive dependencies
 * use the existing frontend rig. Rename replaces that rig's unused stub. */
#define screen_rename_world t06_unused_rename
#define screen_rename_world_setup t06_unused_rename_setup
#define main t06_unused_frontend_main
#define gfx_scissor t06_unused_scissor
#define gfx_draw_quads t06_unused_quads
#define loading_notice_draw_strip t06_unused_strip
#include "test_frontend_no_pointer.c"
#undef gfx_scissor
#undef gfx_draw_quads
#undef loading_notice_draw_strip
#undef main
#undef screen_rename_world
#undef screen_rename_world_setup

static unsigned busy_list_draws, busy_quads, busy_notices;
void gfx_scissor(bool enable, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
	(void)x; (void)y; (void)w; (void)h;
	if(enable) busy_list_draws++;
}
void gfx_draw_quads(size_t count, const int16_t* v, const uint8_t* color,
					const uint16_t* uv) {
	(void)uv;
	if(count == 4 && v[0] == 0 && v[1] == 0 && color[3] == 255
	   && v[6] == gfx_width() && v[7] == gfx_height())
		busy_quads++;
}
void loading_notice_draw_strip(int width, int height, const char* primary) {
	assert(width > 0 && height > 0 && primary[0]);
	busy_notices++;
}
static void assert_busy_takeover(void) {
	assert(screen_texpacks_busy());
	busy_list_draws = busy_quads = busy_notices = 0;
	fe_render(&screen_texturepacks);
	/* No row rectangles are emitted beneath the notice: the list viewport
	 * is never entered, and the frame is an opaque takeover. */
	assert(busy_list_draws == 0 && busy_quads == 1 && busy_notices == 1);
}

#ifdef _WIN32
#include <process.h>
#define GETPID() _getpid()
#else
#include <unistd.h>
#define GETPID() getpid()
#endif

extern struct screen screen_rename_world;
void screen_rename_world_setup(const char*, const char*);
/* Mount the fake SD root at this test's private working directory. */
const char* __wrap_path_from_sd_root(const char* path, char* buf, size_t size) {
	(void)buf; (void)size;
	return !strncmp(path, "sd:/", 4) ? path + 4 : path;
}
static int type_glyph, prompt_count;
int __real_gutil_prompt_glyph(int, int, int, int);
int __wrap_gutil_prompt_glyph(int x, int y, int size, int glyph) {
	if(y == 412) type_glyph = glyph;
	if(glyph >= 0) prompt_count++;
	return __real_gutil_prompt_glyph(x, y, size, glyph);
}
/* 10-31: the rename keyboard's prompts are the OSK's centred A1 hint row
 * (osk_frontend_hint_row); its glyphs are drawn inside gui_util.c, so the
 * row itself is observed: "Type" carries the Remote A cell. */
void __real_gutil_hint_row_draw(const struct gutil_hint_row* row);
void __wrap_gutil_hint_row_draw(const struct gutil_hint_row* row) {
	for(int k = 0; row && k < row->count; k++) {
		if(!strcmp(row->cells[k].label, "Type")) type_glyph = row->cells[k].glyph[0];
		for(int g = 0; g < row->cells[k].glyph_count; g++)
			if(row->cells[k].glyph[g] >= 0) prompt_count++;
	}
	__real_gutil_hint_row_draw(row);
}

static void packs_open(void) {
	fe_attach(FE_REMOTE);
	screen_texturepacks_set_return(&screen_title);
	screen_texturepacks_set_ingame(false);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(3);
}

static void pick_focus(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	packs_open();
	fe_tap(r.down);
	fe_tap(r.click);
	assert(screen_texpacks_test_pending() == 1);
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_APPLY);
	/* This seeded pack does not exist: apply must preserve the old pack. */
	char old[256];
	snprintf(old, sizeof(old), "%s", config_read_string(&gstate.config_user,
		"paths.texturepack", "assets"));
	fe_tap(r.click);
	assert_busy_takeover(); /* pending: before the synchronous reload */
	for(int f = 0; f < 5; f++) fe_step();
	assert(!strcmp(old, config_read_string(&gstate.config_user,
		"paths.texturepack", "assets")));
	assert(screen_texpacks_test_apply_enabled());
	/* Choosing the active pack keeps Apply disabled. */
	fe_tap(r.up);
	fe_tap(r.click);
	assert(!screen_texpacks_test_apply_enabled());
	assert(screen_texpacks_test_highlight() != FE_TP_BTN_APPLY);
}

static void pointer_handoff(bool load) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	if(load) {
		fe_saves(3); fe_attach(FE_REMOTE); fe_open(&screen_select_world);
		for(int f = 0; f < 8; f++) fe_step();
		fe_tap(r.right); /* directional Play */
		assert(screen_sworld_test_focus() == FE_LW_PLAY);
	} else {
		packs_open(); fe_tap(r.down); fe_tap(r.click);
		/* Reach Apply on both the old and new navigation. */
		for(int k = 0; k < 3 && screen_texpacks_test_highlight() != FE_TP_BTN_APPLY; k++)
			fe_tap(r.right);
		assert(screen_texpacks_test_highlight() == FE_TP_BTN_APPLY);
	}
	/* Establish a pointer sample off the rows, then deliberately MOVE it.
	 * A validity transition alone must not steal directional focus. */
	fe_pointer(620, 470); fe_step();
	float x = load ? fe_load_cx(FE_LW_ROW(2)) : 320;
	float y = load ? fe_load_cy(FE_LW_ROW(2))
		: FE_TP_ROW_TOP_PX(2) + FE_TP_ROW_ACTION_PX / 2;
	fe_pointer(x, y); fe_step(); fe_step();
	fe_tap(r.click);
	assert(fe_screen_sets == 0 && "row press launched Play");
	if(load) assert(screen_sworld_test_list_selection() == 2);
	else {
		assert(screen_texpacks_test_pending() == 2 && "row press fired Apply");
		assert(screen_texpacks_test_highlight() == FE_TP_BTN_APPLY);
	}
	fe_tap(r.click); /* repeated row press must also only select */
	assert(fe_screen_sets == 0);
	if(!load) assert(screen_texpacks_test_pending() == 2);
}

static void apply_success(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	assert(fe_mkdir("assets") == 0 || errno == EEXIST);
	config_set_string(&gstate.config_user, "paths.texturepack", "texturepacks/pack00");
	packs_open();
	fe_tap(r.up); fe_tap(r.click);
	/* Reach Apply even before the focus fix, isolating the post-apply test. */
	for(int k = 0; k < 3 && screen_texpacks_test_highlight() != FE_TP_BTN_APPLY; k++)
		fe_tap(r.right);
	assert(screen_texpacks_test_highlight() == FE_TP_BTN_APPLY);
	fe_tap(r.click);
	for(int f = 0; f < 5; f++) fe_step();
	assert(!strcmp(config_read_string(&gstate.config_user,
		"paths.texturepack", ""), "assets"));
	assert(!screen_texpacks_test_apply_enabled());
	assert(screen_texpacks_test_highlight() != FE_TP_BTN_APPLY);
	fe_tap(r.up); fe_tap(r.click); fe_tap(r.click);
	assert(!screen_texpacks_test_apply_enabled() && fe_screen_sets == 0);
	fe_rmdir("assets");
}

static void rename_glyphs(bool attached) {
	fe_attach(FE_REMOTE);
	/* Replace the Nunchuk report with a bare Remote hardware report. */
	input_init();
	fe_wpad[0].type = WPAD_EXP_NONE;
	for(int k = 0; k < 4; k++) {
		WPADData data = {.err = WPAD_ERR_NONE,
			.data_present = WPAD_DATA_BUTTONS | WPAD_DATA_EXPANSION,
			.exp = {.type = WPAD_EXP_NONE},
			.btns_h = attached && k == 1 ? WPAD_BUTTON_A : 0};
		fe_wpad[0].queue[fe_wpad[0].count++] = data;
		fe_now += 16; input_poll();
	}
	if(attached) assert(input_devices()->devices[0].published.category == INPUT_CAT_WIIMOTE);
	else { fe_wpad[0].attached = false; input_init(); input_poll();
		assert(!input_player_frame(0)); }
	screen_rename_world_setup("unused-world", "Before");
	screen_rename_world.reset(&screen_rename_world, 640, 480);
	/* Poll the same bare hardware, then drive the actual screen. */
	screen_rename_world.update(&screen_rename_world, .016F);
	type_glyph = -1; prompt_count = 0;
	fe_render(&screen_rename_world);
	assert(type_glyph == 0 && "Rename Type must show Remote A");
	assert(prompt_count >= 5 && "Rename keyboard action glyphs missing");
}


/* ---- BG-BAKE-1028/3: the pack screen's backdrop job, through fake hooks ---- */
#include "game/gui/backdrop_bake.h"
void screen_texturepacks_set_backdrop_hooks(const struct backdrop_job_hooks* h);
void screen_texturepacks_backdrop_service(bool at_title_idle);
int screen_texturepacks_test_backdrop_job(void);
const char* screen_texturepacks_test_backdrop_reason(void);
void screen_texturepacks_test_backdrop_reset(void);
enum { BJ_IDLE, BJ_QUEUED, BJ_BAKING, BJ_FAILED };

static struct {
	int state;           /* texture.c tex_backdrop_state: 2 cached, 3 missing */
	int starts, releases, publishes, reloads, frees;
	enum backdrop_bake_state poll;
	enum backdrop_bake_error error;
	int publish_result;
	int scenes_remaining;
	bool uploaded, retired;
	bool defer_second;
	int sounds;
	uint8_t px[4];
} fj;
static int fj_state(void) {
	return fj.defer_second && fj.publishes == 1 ? 2 : fj.state;
}
static bool fj_start(void) { fj.starts++; fj.poll = BACKDROP_BAKE_TERRAIN; return true; }
static enum backdrop_bake_state fj_poll(void) { return fj.poll; }
static enum backdrop_bake_error fj_error(void) { return fj.error; }
static uint8_t* fj_take(void) { return fj.px; }
static void fj_release(void) { fj.releases++; }
static void fj_free(uint8_t* p) { assert(p == fj.px); fj.frees++; }
static int fj_publish(const uint8_t* p) {
	assert(p == fj.px);
	fj.publishes++;
	if(fj.publish_result == 0) fj.state = --fj.scenes_remaining > 0 ? 3 : 2;
	return fj.publish_result;
}
static const char* fj_reason(int r) { return r ? "SD card is locked or full." : "ok"; }
static void fj_reload(void) { fj.reloads++; }
static bool fj_uploaded(void) { return fj.uploaded; }
static bool fj_retired(void) { return fj.retired; }
static int fj_sounds(void) { return fj.sounds; }
static const char* fj_sounds_reason(void) { return "Required sound unavailable: menu.test"; }
static bool fj_sounds_retry(void) { fj.sounds = AUDIO_REGISTRY_READY; return true; }
static const struct backdrop_job_hooks fj_hooks = {
	fj_state, fj_start, fj_poll, fj_error, fj_take, fj_release, fj_free,
	fj_publish, fj_reason, fj_reload, fj_uploaded, fj_retired, fj_sounds,
	fj_sounds_reason, fj_sounds_retry,
};
static void fj_reset(int state) {
	memset(&fj, 0, sizeof(fj));
	fj.state = state;
	fj.scenes_remaining = 1;
	fj.uploaded = fj.retired = true;
	fj.sounds = AUDIO_REGISTRY_READY;
	screen_texturepacks_test_backdrop_reset();
	screen_texturepacks_set_backdrop_hooks(&fj_hooks);
}
static void fj_frames(bool title, int n) {
	for(int k = 0; k < n; k++) screen_texturepacks_backdrop_service(title);
}

/* Apply a real pack through the real screen (in-game or at the title). */
static void fj_apply_assets(bool ingame) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	assert(fe_mkdir("assets") == 0 || errno == EEXIST);
	config_set_string(&gstate.config_user, "paths.texturepack", "texturepacks/pack00");
	fe_attach(FE_REMOTE);
	screen_texturepacks_set_return(&screen_title);
	screen_texturepacks_set_ingame(ingame);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(3);
	fe_tap(r.up); fe_tap(r.click);
	for(int k = 0; k < 3 && screen_texpacks_test_highlight() != FE_TP_BTN_APPLY; k++)
		fe_tap(r.right);
	fe_tap(r.click);
	for(int f = 0; f < 5; f++) fe_step();
	assert(!strcmp(config_read_string(&gstate.config_user, "paths.texturepack", ""), "assets"));
	screen_texturepacks_set_ingame(false);
}

static void backdrop_job(void) {
	/* Boot: an already-selected uncached pack bakes once at the title. */
	fj_reset(3);
	assert(screen_texturepacks_backdrop_ready_state() == BACKDROP_JOB_LOADING);
	fj_frames(false, 5);
	assert(fj.starts == 0 && screen_texturepacks_test_backdrop_job() == BJ_IDLE);
	fj_frames(true, 1);
	assert(fj.starts == 1 && screen_texturepacks_test_backdrop_job() == BJ_BAKING);
	fj.poll = BACKDROP_BAKE_DONE;
	fj_frames(true, 1);
	assert(fj.publishes == 1 && fj.frees == 1 && fj.releases == 1 && fj.reloads == 1);
	assert(screen_texturepacks_test_backdrop_job() == BJ_IDLE);
	assert(screen_texturepacks_backdrop_ready_state() == BACKDROP_JOB_READY);
	fj.retired = false;
	assert(screen_texturepacks_backdrop_ready_state() == BACKDROP_JOB_LOADING);
	fj.retired = true;
	fj.uploaded = false;
	assert(screen_texturepacks_backdrop_ready_state() == BACKDROP_JOB_FAILED);
	assert(screen_texturepacks_backdrop_failure_reason()[0]);
	screen_texturepacks_backdrop_retry();
	assert(screen_texturepacks_backdrop_ready_state() == BACKDROP_JOB_LOADING);
	fj.uploaded = true;
	fj_frames(true, 1);
	assert(screen_texturepacks_backdrop_ready_state() == BACKDROP_JOB_READY);
	fj_frames(true, 50); /* warm: never bakes again */
	assert(fj.starts == 1);

 /* Boot exposes only the beach; optional Nether becomes eligible on title. */
 fj_reset(3); fj.scenes_remaining = 2;
 fj.defer_second = true;
 fj_frames(true, 1); assert(fj.starts == 1);
 fj.poll = BACKDROP_BAKE_DONE; fj_frames(true, 1);
 assert(screen_texturepacks_test_backdrop_job() == BJ_IDLE);
 assert(screen_texturepacks_backdrop_ready_state() == BACKDROP_JOB_READY);
 fj_frames(true, 3); assert(fj.starts == 1);
 fj.defer_second = false; /* title admitted */
 fj.retired = false; fj_frames(true, 3); assert(fj.starts == 1);
 assert(screen_texturepacks_test_backdrop_job() == BJ_QUEUED);
 fj.retired = true; fj_frames(false, 3); assert(fj.starts == 1);
 fj_frames(true, 1); assert(fj.starts == 2);
 /* Even a completed frame is discarded if the player starts/loads first. */
 fj.poll = BACKDROP_BAKE_DONE; fj_frames(false, 1);
 assert(fj.publishes == 1 && fj.releases == 2);
 assert(screen_texturepacks_test_backdrop_job() == BJ_QUEUED);
 fj_frames(false, 3); assert(fj.starts == 2);
 fj.retired = false; fj_frames(true, 3); assert(fj.starts == 2);
 fj.retired = true; fj_frames(true, 1); assert(fj.starts == 3);
 fj.poll = BACKDROP_BAKE_DONE; fj_frames(true, 1);
 assert(fj.publishes == 2 && fj.releases == 3);
 assert(screen_texturepacks_backdrop_ready_state() == BACKDROP_JOB_READY);
 fj_frames(true, 10); assert(fj.starts == 3); /* cached once per pack */

	/* Author art / verified cache: no bake at all. */
	fj_reset(1); fj_frames(true, 10); assert(fj.starts == 0);
	fj_reset(2); fj_frames(true, 10); assert(fj.starts == 0);

	/* In-game Apply defers to the title. */
	fj_reset(2);
	fj_frames(true, 1); /* boot check: cached */
	fj.state = 3;
	fj_apply_assets(true);
	assert(screen_texturepacks_test_backdrop_job() == BJ_QUEUED);
	fj_frames(false, 20);
	assert(fj.starts == 0);
	fj_frames(true, 1);
	assert(fj.starts == 1);
	/* Leaving the title mid-bake (world-alive refusal) resumes there. */
	fj.poll = BACKDROP_BAKE_FAILED; fj.error = BACKDROP_BAKE_ERR_WORLD_ALIVE;
	fj_frames(false, 1);
	assert(screen_texturepacks_test_backdrop_job() == BJ_QUEUED && fj.releases == 1);
	fj_frames(true, 1);
	assert(fj.starts == 2);

	/* Publish failure: visible reason, no automatic retry, explicit retry. */
	fj.poll = BACKDROP_BAKE_DONE; fj.publish_result = 5;
	fj_frames(true, 1);
	assert(screen_texturepacks_test_backdrop_job() == BJ_FAILED);
	assert(screen_texturepacks_backdrop_ready_state() == BACKDROP_JOB_FAILED);
	assert(!strcmp(screen_texturepacks_test_backdrop_reason(), "SD card is locked or full."));
	fj_frames(true, 200);
	assert(fj.starts == 2 && "no automatic retry loop");
	/* The active pack's Apply is the retry. */
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	fe_attach(FE_REMOTE);
	screen_texturepacks_set_return(&screen_title);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(3);
	fe_tap(r.up); fe_tap(r.click);
	assert(screen_texpacks_test_apply_enabled() && "retry offered on the active pack");
	for(int k = 0; k < 3 && screen_texpacks_test_highlight() != FE_TP_BTN_APPLY; k++)
		fe_tap(r.right);
	fe_tap(r.click);
	for(int f = 0; f < 5; f++) fe_step();
	/* pack-lag-1030: the retry no longer holds the pack screen; the bake
	 * is queued and runs at the title. */
	assert(!screen_texpacks_busy());
	assert(screen_texturepacks_test_backdrop_job() == BJ_QUEUED);
	fj.publish_result = 0;
	fj_frames(true, 1);
	assert(fj.starts == 3 && screen_texturepacks_test_backdrop_job() == BJ_BAKING);
	fj.poll = BACKDROP_BAKE_DONE;
	fj_frames(true, 1);
	assert(screen_texturepacks_test_backdrop_job() == BJ_IDLE && fj.state == 2);
	assert(!screen_texpacks_test_apply_enabled()); /* nothing left to retry */
	assert(!screen_texpacks_busy());

	/* pack-lag-1030: the real Apply stays opaque/busy while SOUNDS load (they
	 * belong to the apply), then releases the screen at once. The menu
	 * backdrop bake is deferred to the title (it was ~36 s of black screen on
	 * the console). Phases still never reorder: no bake before sounds. */
	fj_reset(3);
	fj.sounds = AUDIO_REGISTRY_LOADING;
	fj_apply_assets(false);
	assert(screen_texpacks_busy() && fj.starts == 0);
	assert_busy_takeover(); /* audio readiness */
	fj_frames(true, 2); /* even an external service cannot reorder phases */
	assert(fj.starts == 0);
	fj.sounds = AUDIO_REGISTRY_READY;
	fe_step();
	assert(!screen_texpacks_busy() && "the bake no longer holds Apply");
	assert(fj.starts == 0 && screen_texturepacks_test_backdrop_job() == BJ_QUEUED);
	fj_frames(false, 10); /* still on the pack screen: no bake */
	assert(fj.starts == 0);
	fj_frames(true, 1); /* back at the title: the bake runs there */
	assert(fj.starts == 1);
	fj.retired = false; fj.poll = BACKDROP_BAKE_DONE;
	fj_frames(true, 1);
	assert(fj.publishes == 1);
	fj.retired = true;
	fj_frames(true, 1);

	/* A completed degraded audio load must not strand Apply or the bake. */
	fj_reset(3);
	fj.sounds = AUDIO_REGISTRY_DISABLED;
	fj_apply_assets(false);
	assert(!screen_texpacks_busy() && "a degraded audio load does not strand Apply");
	fj_frames(true, 1);
	assert(fj.starts == 1);
	fj.poll = BACKDROP_BAKE_DONE;
	fj_frames(true, 1);
	assert(fj.publishes == 1);
	assert(screen_texturepacks_backdrop_ready_state() == BACKDROP_JOB_READY);

	/* A bake failure (not a world refusal) also stops with a reason. */
	fj_reset(3);
	fj_frames(true, 1);
	fj.poll = BACKDROP_BAKE_FAILED; fj.error = BACKDROP_BAKE_ERR_NOT_READY;
	fj_frames(true, 1);
	assert(screen_texturepacks_test_backdrop_job() == BJ_FAILED);
	assert(screen_texturepacks_test_backdrop_reason()[0]);
	fj_frames(true, 100);
	assert(fj.starts == 1);

	/* The pack changes under a running bake: cancelled and re-queued. */
	fj_reset(3);
	fj_frames(true, 1);
	assert(fj.starts == 1);
	config_set_string(&gstate.config_user, "paths.texturepack", "texturepacks/pack01");
	fj_frames(false, 1);
	assert(fj.releases == 1 && screen_texturepacks_test_backdrop_job() == BJ_QUEUED);
	fj.retired = false;
	fj_frames(true, 2);
	assert(fj.starts == 1 && "replacement waits for the old scratch world");
	fj.retired = true;
	fj_frames(true, 1);
	assert(fj.starts == 2);
	screen_texturepacks_set_backdrop_hooks(NULL);
	screen_texturepacks_test_backdrop_reset();
	fe_rmdir("assets");
	puts("PASS backdrop job: boot service, warm/author skip, in-game deferral, "
		 "world-refusal resume, failure reason + no auto-retry + explicit retry, "
		 "pack change cancels");
}

int main(int argc, char** argv) {
	/* Under the system temp dir, never the repo (legal hook, 10-30). */
	char scratch[512], config_path[600];
	const char* tmp = getenv("TMP");
	if(!tmp || !tmp[0]) tmp = getenv("TEMP");
	if(!tmp || !tmp[0]) tmp = "/tmp";
	snprintf(scratch, sizeof(scratch), "%s/cw_texturepacks_screen_run_%ld", tmp, (long)GETPID());
	snprintf(config_path, sizeof(config_path), "%s/config_wii.json", scratch);
	assert(fe_mkdir(scratch) == 0 || errno == EEXIST);
	fe_copy_file("config_wii.json", config_path);
	assert(fe_chdir(scratch) == 0);
	fe_config_open(false);
	if(argc == 1 || !strcmp(argv[1], "pick")) pick_focus();
	if(argc == 1 || !strcmp(argv[1], "success")) apply_success();
	if(argc == 1 || !strcmp(argv[1], "load")) pointer_handoff(true);
	if(argc == 1 || !strcmp(argv[1], "packs")) pointer_handoff(false);
	if(argc == 1 || !strcmp(argv[1], "rename")) rename_glyphs(true);
	if(argc == 1 || !strcmp(argv[1], "unassigned")) rename_glyphs(false);
	if(argc == 1 || !strcmp(argv[1], "backdrop")) backdrop_job();
	fe_saves(0); config_destroy(&gstate.config_user);
	remove("config.json"); remove("config_wii.json");
	assert(fe_chdir("..") == 0);
	assert(fe_rmdir(scratch) == 0 && "sandbox left behind");
	puts("PASS texturepacks, row pointer handoff and Rename glyphs");
	return 0;
}
