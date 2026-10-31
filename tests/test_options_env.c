/*
	Native Options/Display interaction tests and Beta pause/slider goldens.

	Beta expectations are hand-computed from the cited decompile; native
	navigation expectations come from native_rework_2026-09-04.md. Never
	derive expectations by running the unit under test.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define mkdir(path, mode) _mkdir(path)
#define chdir _chdir
#define getcwd _getcwd
#define rmdir _rmdir
#define GETPID() _getpid()
#else
#include <unistd.h>
#include <sys/stat.h>
#define GETPID() getpid()
#endif

#undef GLFW_DLL
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "config.h"
#include "graphics/gui_util.h"
#include "platform/input.h"
#include "platform/input_glyph.h"
#include "platform/input_owner.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "network/server_interface.h"
#include "platform/gfx.h"
#include "graphics/gfx_settings.h"
#include "block/blocks.h"
#include "item/items.h"

extern bool screen_options_test_target_rect(int target, struct gutil_button_bounds* out);
extern int screen_options_test_focus(void);
extern bool screen_options_test_target_enabled(int target);
extern bool screen_video_settings_test_target_rect(int target, struct gutil_button_bounds* out);
extern int screen_video_settings_test_focus(void);
extern bool screen_controller_settings_test_target_rect(int target, struct gutil_button_bounds* out);
extern int screen_controller_settings_test_focus(void);
extern bool screen_controller_settings_test_save_failed(void);
extern bool screen_controller_settings_test_target_enabled(int target);
extern int screen_controller_settings_test_sensitivity(void);
extern bool screen_controller_settings_test_inverted(void);
extern int screen_controller_settings_test_assist(void);
/* next-gaps-0924 W1F: the Difficulty row (screen_options.c) and the pending
 * server request it feeds (server_local.c, CAVEWII_SERVER_LOCAL_TEST). */
extern int screen_options_test_difficulty(void);
extern const char* screen_options_test_difficulty_label(void);
extern int server_local_test_requested_difficulty(void);
extern void server_local_request_difficulty(int d);

static bool feq(float a, float b) {
	return fabsf(a - b) < 0.000001F;
}

static void test_slider_math(void) {
	/* GuiSlider uses (mouseX - (x + 4)) / (width - 8), clamped to [0,1].
	 * With x=100 and width=150, the denominator is 142 and the midpoint is
	 * mouseX=175. The thumb uses x + (int)(value * 142).
	 * [740c583 GuiSlider.mousePressed]
	 * [740c583 GuiSlider.mouseDragged] */
	assert(feq(screen_options_test_slider_value(103.0F, 100, 150), 0.0F));
	assert(feq(screen_options_test_slider_value(104.0F, 100, 150), 0.0F));
	assert(feq(screen_options_test_slider_value(175.0F, 100, 150), 0.5F));
	assert(feq(screen_options_test_slider_value(246.0F, 100, 150), 1.0F));
	assert(feq(screen_options_test_slider_value(247.0F, 100, 150), 1.0F));
	assert(screen_options_test_slider_thumb(100, 150, 0.0F) == 100);
	assert(screen_options_test_slider_thumb(100, 150, 0.5F) == 171);
	assert(screen_options_test_slider_thumb(100, 150, 1.0F) == 242);
	puts("  options: slider math 1/1");
}

static void test_control_inventory(void) {
	/* Controller presentation deliberately exposes only working backends. */
	/* next-gaps-0924 W1F: + Difficulty, EnumOptions.DIFFICULTY ordinal 9 --
	 * Beta's GuiOptions.java:7 lists it on this screen. */
	static const int ids[] = {0, 1, 101, 150, 200, 102, 9};
	static const char* const names[] = {"Music", "Sound", "Display...", "Local split-screen", "Back", "Controller settings...", "Difficulty"};
	static const bool sliders[] = {true, true, false, false, false, false, false};
	static const bool enabled[] = {true, true, true, true, true, true, true};

	assert(screen_options_test_control_count()
		   == sizeof(ids) / sizeof(ids[0]));
	for(size_t k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
		int id = -1;
		const char* name = NULL;
		bool slider = false, live = false;
		assert(screen_options_test_control(k, &id, &name, &slider, &live));
		assert(id == ids[k]);
		assert(strcmp(name, names[k]) == 0);
		assert(slider == sliders[k]);
		assert(live == enabled[k]);
	}
	puts("  options: control inventory 1/1");
}

static void test_pause_button_inventory(void){
 const int ids[]={4,5,0,-1,1};const char* labels[]={"Back to game","Achievements","Options...","Texture packs...","Save and quit"};
 assert(screen_pause_test_button_count(true)==5);
 for(size_t k=0;k<5;k++){int id=-1;const char* label=NULL;assert(screen_pause_test_button(k,true,true,&id,&label));assert(id==ids[k]&&!strcmp(label,labels[k]));}
 assert(!screen_pause_test_button(5,true,true,NULL,NULL));
 puts("PASS approved pause actions retain their native action IDs");
}

static void test_video_control_inventory(void) {
	static const int ids[] = {5, 200};
	static const char* const names[] = {"View Bobbing", "Back"};
	static const bool enabled[] = {true, true};

	assert(screen_video_settings_test_control_count()
		   == sizeof(ids) / sizeof(ids[0]));
	for(size_t k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
		int id = -1;
		const char* name = NULL;
		bool live = false;
		assert(screen_video_settings_test_control(k, &id, &name, &live));
		assert(id == ids[k]);
		assert(strcmp(name, names[k]) == 0);
		assert(live == enabled[k]);
	}
	puts("  options: video control inventory 1/1");
}

static void expect_video_label(int id, bool value, const char* expected) {
	char label[64];
	screen_video_settings_test_format_label(id, value, label, sizeof(label));
	if(strcmp(label, expected) != 0) {
		fprintf(stderr, "video label id=%d value=%d: '%s', want '%s'\n", id,
				(int)value, label, expected);
		assert(0);
	}
}

static void test_video_label_composition(void) {
	expect_video_label(5, true, "View Bobbing: ON");
	expect_video_label(5, false, "View Bobbing: OFF");
	expect_video_label(200, false, "Back");
	puts("  options: video label composition 1/1");
}

static void expect_label(int id, float value, const char* expected) {
	char label[64];
	screen_options_test_format_label(id, value, label, sizeof(label));
	if(strcmp(label, expected) != 0) {
		fprintf(stderr, "label id=%d value=%f: '%s', want '%s'\n", id,
				value, label, expected);
		assert(0);
	}
}

static void test_label_composition(void) {
	/* GameSettings.getKeyBinding composes "<option>: <value>". Nonzero
	 * music/sound values truncate value*100.
	 * [740c583 GameSettings.getKeyBinding]
	 * [740c583 GameSettings.GameSettings] */
	expect_label(0, 1.0F, "Music: 100%");
	expect_label(1, 0.0F, "Sound: OFF");
	expect_label(1, 0.375F, "Sound: 37%");
	expect_label(1, 1.0F, "Sound: 100%");
	expect_label(101, 0.0F, "Display...");
	expect_label(200, 0.0F, "Back");
	puts("  options: label composition 1/1");
}

static void test_config_round_trip(void) {
	char path[96];
	snprintf(path, sizeof(path), "test_options_env_%ld.json", (long)GETPID());
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(fputs("{\"audio\":{\"volume\":100}}\n", f) >= 0);
	assert(fclose(f) == 0);

	struct config first;
	assert(config_create(&first, path));
	assert(config_set_int(&first, "audio.volume", 37));
	assert(config_save(&first, path));
	config_destroy(&first);

	struct config second;
	assert(config_create(&second, path));
	assert(config_read_int(&second, "audio.volume", -1) == 37);
	config_destroy(&second);
	assert(remove(path) == 0);
	puts("  options: config round-trip 1/1");
}

struct pause_capture {
	int toggles;
	int unloads;
};

static void capture_pause_rpc(struct server_rpc* call, void* user) {
	struct pause_capture* capture = user;
	if(call->type == SRPC_TOGGLE_PAUSE)
		capture->toggles++;
	else if(call->type == SRPC_UNLOAD_WORLD)
		capture->unloads++;
}

static void test_pause_edges(void) {
	assert(svin_init());
	memset(&gstate, 0, sizeof(gstate));
	gstate.config_user.root = json_value_init_object();
	assert(gstate.config_user.root);
	gstate.paused = true;
	gstate.players[0].current_screen = &screen_pause;

	/* The existing CaveWii controller seam gives HOME ownership when one
	 * report carries HOME+INVENTORY: resume exactly once and do not unload. */
	screen_pause_test_process_edges(true, true);
	assert(!gstate.paused);
	assert(gstate.players[0].current_screen == &screen_ingame);

	struct pause_capture capture = {0};
	svin_process_messages(capture_pause_rpc, &capture, false);
	assert(capture.toggles == 1);
	assert(capture.unloads == 0);

	/* The CLOSE role alone (2026-09-19 maintainer decision on Astra menu
	 * review item 8: "Close = Back / Resume, consistently; quitting only
	 * through the named menu row"). It used to be Beta's id-1 action --
	 * resume + unload + the title -- which made this screen the ONE place in
	 * the UI where Close was not Back. It is a plain resume now: one toggle,
	 * NO unload, gameplay, and no pending exit to settle. Beta's id-1 pair
	 * itself is unchanged and is asserted on the ROW by
	 * test_pause_approved_actions. */
	gstate.paused = true;
	gstate.players[0].current_screen = &screen_pause;
	screen_pause_test_process_edges(false, true);
	assert(!gstate.paused);
	assert(!screen_pause_exit_pending());
	assert(gstate.players[0].current_screen == &screen_ingame);
	memset(&capture, 0, sizeof(capture));
	svin_process_messages(capture_pause_rpc, &capture, false);
	assert(capture.toggles == 1);
	assert(capture.unloads == 0);
	config_destroy(&gstate.config_user);
	puts("  options: pause edges 1/1");
}

static void test_native_focus_and_repeat(void) {
	struct gutil_menu_focus focus = {0};
	assert(gutil_menu_pointer_focus(&focus, true, 100, 100, false));
	assert(!gutil_menu_pointer_focus(&focus, true, 100, 100, true));
	assert(!gutil_menu_pointer_focus(&focus, true, 103, 102, false));
	assert(!gutil_menu_pointer_focus(&focus, true, 99, 99, false));
	assert(gutil_menu_pointer_focus(&focus, true, 108, 100, false));
	assert(!gutil_menu_pointer_focus(&focus, false, 0, 0, false));
	/* Reacquiring elsewhere cannot steal retained selection. */
	assert(!gutil_menu_pointer_focus(&focus, true, 300, 200, false));
	assert(gutil_menu_pointer_focus(&focus, true, 310, 200, false));
	focus = (struct gutil_menu_focus) {0};
	assert(!gutil_menu_pointer_focus(&focus, false, 0, 0, true));
	assert(!gutil_menu_pointer_focus(&focus, true, 100, 100, false));
	assert(!gutil_menu_pointer_focus(&focus, true, 200, 100, true));
	assert(!gutil_menu_pointer_focus(&focus, true, 201, 100, false));

	struct gutil_menu_repeat repeat = {0};
	assert(gutil_menu_repeat_step(&repeat, 1, 0.016F) == 1);
	assert(gutil_menu_repeat_step(&repeat, 1, 0.3F) == 0);
	assert(gutil_menu_repeat_step(&repeat, 1, 0.051F) == 1);
	assert(gutil_menu_repeat_step(&repeat, 1, 0.05F) == 0);
	assert(gutil_menu_repeat_step(&repeat, 1, 0.051F) == 1);
	assert(gutil_menu_repeat_step(&repeat, -1, 0.016F) == -1);
	assert(gutil_menu_repeat_step(&repeat, -1, 10.0F) == -1);
	assert(gutil_menu_repeat_step(&repeat, -1, 0.016F) == 0);
	assert(gutil_menu_repeat_step(&repeat, 0, 0.0F) == 0);
	assert(gutil_menu_repeat_step(&repeat, -1, 0.016F) == -1);
	puts("  native menus: IR jitter/loss/priority and bounded repeat PASS");
}

/* Drive the actual screen updates and input latch; fake only GLFW polling.
 * No rendering context, input remap or test-only screen handler is involved. */
extern GLFWwindow* window;
static bool keys[GLFW_KEY_LAST + 1];
static double cursor_x, cursor_y;

int glfwGetKey(GLFWwindow* w, int key) {
	(void)w;
	assert(key >= 0 && key <= GLFW_KEY_LAST);
	return keys[key] ? GLFW_PRESS : GLFW_RELEASE;
}
int glfwGetMouseButton(GLFWwindow* w, int button) {
	(void)w;
	(void)button;
	return GLFW_RELEASE;
}
void glfwSetInputMode(GLFWwindow* w, int mode, int value) {
	(void)w;
	(void)mode;
	(void)value;
}
void glfwSetCursorPos(GLFWwindow* w, double x, double y) {
	(void)w;
	cursor_x = x;
	cursor_y = y;
}
void glfwGetCursorPos(GLFWwindow* w, double* x, double* y) {
	(void)w;
	*x = cursor_x;
	*y = cursor_y;
}

#ifdef _WIN32
/* The pooled PC input object uses GLFW's DLL imports. Redirect those same
 * imports to the polling fixture rather than passing our sentinel to GLFW. */
int (*__imp_glfwGetKey)(GLFWwindow*, int) = glfwGetKey;
int (*__imp_glfwGetMouseButton)(GLFWwindow*, int) = glfwGetMouseButton;
void (*__imp_glfwSetInputMode)(GLFWwindow*, int, int) = glfwSetInputMode;
void (*__imp_glfwSetCursorPos)(GLFWwindow*, double, double) = glfwSetCursorPos;
void (*__imp_glfwGetCursorPos)(GLFWwindow*, double*, double*) = glfwGetCursorPos;
#endif

static void menu_frame(int key, float dt) {
	memset(keys, 0, sizeof(keys));
	if(key)
		keys[key] = true;
	input_poll();
	gstate.players[0].current_screen->update(gstate.players[0].current_screen, dt);
}

static void menu_press(int key) {
	menu_frame(0, 0.016F);
	menu_frame(key, 0.016F);
}

static void test_controller_settings(void) {
	char previous[4096], scratch[96];
	assert(getcwd(previous, sizeof(previous)));
	snprintf(scratch, sizeof(scratch), "test_controller_options_%ld", (long)GETPID());
	assert(mkdir(scratch, 0700) == 0 && chdir(scratch) == 0);
	memset(&gstate, 0, sizeof(gstate));
	gstate.config_user.root = json_parse_string(
		"{\"input\":{\"gui_up\":[87],\"gui_down\":[83],\"gui_left\":[65],"
		"\"gui_right\":[68],\"gui_click\":[32],\"gui_click_alt\":[66]}}");
	assert(gstate.config_user.root);
	window = (GLFWwindow*)1;
	input_init();
	screen_options_set_return(&screen_title);
	screen_set(&screen_options);
	cursor_x = 300; cursor_y = 296;
	menu_press(' ');
	assert(gstate.players[0].current_screen == &screen_controller_settings);
	assert(!screen_controller_settings.render_world);
	cursor_x = -1; menu_frame(0, .016F);
	assert(input_primary_look_sensitivity() == 3 && !input_primary_look_inverted());
	/* aim-snap-1005: the attack assist row defaults Off and sits at
	 * (476, 296), the Invert button pattern one row under Sensitivity. */
	assert(input_attack_assist_level() == 0);
	assert(screen_controller_settings_test_assist() == 0);
	{
		struct gutil_button_bounds r;
		assert(screen_controller_settings_test_target_rect(4, &r));
		assert(r.x == 476 && r.y == 296 && r.width == 108 && r.height == 44);
		assert(screen_controller_settings_test_target_enabled(4));
	}
	const int xs[] = {476, 332, 524};
	const int ys[] = {128, 212, 212};
	for(int i = 0; i < 3; i++) {
		struct gutil_button_bounds r;
		assert(screen_controller_settings_test_target_rect(i, &r));
		assert(r.x == xs[i] && r.y == ys[i] && r.height == 44);
		assert(r.width == (i == 0 ? 108 : 60));
	}
	menu_press(' ');
	assert(screen_controller_settings_test_inverted());
	assert(!input_primary_look_inverted()); /* draft applies on Back */
	/* Actual pointer targets apply exactly once and stop at the endpoints. */
	cursor_x = 550; cursor_y = 232;
	menu_press(' '); menu_press(' ');
	assert(screen_controller_settings_test_sensitivity() == 5);
	assert(!screen_controller_settings_test_target_enabled(2));
	menu_press(' ');
	assert(screen_controller_settings_test_sensitivity() == 5);
	cursor_x = 350;
	for(int i = 0; i < 6; i++) menu_press(' ');
	assert(screen_controller_settings_test_sensitivity() == 1);
	assert(!screen_controller_settings_test_target_enabled(1));
	/* Attack assist: A cycles Off -> Low -> Normal -> Off; staged until Back. */
	cursor_x = 530; cursor_y = 318;
	menu_press(' ');
	assert(screen_controller_settings_test_assist() == 1);
	menu_press(' ');
	assert(screen_controller_settings_test_assist() == 2);
	menu_press(' ');
	assert(screen_controller_settings_test_assist() == 0);
	menu_press(' '); menu_press(' ');
	assert(screen_controller_settings_test_assist() == 2);
	assert(input_attack_assist_level() == 0); /* draft applies on Back */
	cursor_x = -1; menu_press('B');
	assert(gstate.players[0].current_screen == &screen_options);
	assert(input_attack_assist_level() == 2);
	assert(input_primary_look_sensitivity() == 1 && input_primary_look_inverted());
	struct config saved;
	assert(config_create(&saved, "config.json"));
	assert(config_read_int(&saved, "controls.look_sensitivity", -1) == 1);
	assert(config_read_int(&saved, "controls.invert_look", -1) == 1);
	assert(config_read_int(&saved, "controls.attack_assist", -1) == 2);
	config_destroy(&saved);
	/* Reload honours a valid level and rejects out-of-range values (Off). */
	assert(input_attack_assist_set(0) && input_attack_assist_level() == 0);
	input_primary_look_reload();
	assert(input_attack_assist_level() == 2);
	assert(config_set_int(&gstate.config_user, "controls.attack_assist", 7));
	input_primary_look_reload();
	assert(input_attack_assist_level() == 0);
	assert(!input_attack_assist_set(3) && !input_attack_assist_set(-1));
	assert(config_set_int(&gstate.config_user, "controls.attack_assist", 2));
	input_primary_look_reload();
	screen_options_set_return(&screen_pause);
	screen_set(&screen_options); screen_set(&screen_controller_settings);
	assert(screen_controller_settings.render_world);
	/* Failed save must restore the exact JSON tree, including absence of
	 * the parent controls object, and leave the live preference unchanged. */
	assert(json_object_remove(json_object(gstate.config_user.root), "controls") == JSONSuccess);
	input_primary_look_reload();
	char* before = json_serialize_to_string(gstate.config_user.root);
	assert(before && remove("config.json") == 0 && mkdir("config.json", 0700) == 0);
	screen_set(&screen_controller_settings);
	menu_press(' ');
	menu_press('B');
	assert(screen_controller_settings_test_save_failed());
	assert(!input_primary_look_inverted() && input_primary_look_sensitivity() == 3);
	char* after = json_serialize_to_string(gstate.config_user.root);
	assert(after && !strcmp(before, after));
	json_free_serialized_string(before); json_free_serialized_string(after);
	assert(rmdir("config.json") == 0);
	menu_press('B');
	assert(gstate.players[0].current_screen == &screen_options && input_primary_look_inverted());
	assert(remove("config.json") == 0);
	window = NULL;
	screen_options_set_return(NULL);
	config_destroy(&gstate.config_user);
	assert(chdir(previous) == 0 && rmdir(scratch) == 0);
	puts("PASS Controller settings: native route/geometry/endpoints, persistence, exact rollback, retry and pause context");
}

static void test_native_screen_interaction(void) {
	char previous[4096], scratch[96];
	assert(getcwd(previous, sizeof(previous)));
	snprintf(scratch, sizeof(scratch), "test_native_options_%ld", (long)GETPID());
	assert(mkdir(scratch, 0700) == 0);
	assert(chdir(scratch) == 0);
	memset(&gstate, 0, sizeof(gstate));
	gstate.config_user.root = json_parse_string(
		"{\"audio\":{\"volume\":100},\"music\":{\"volume\":100},"
		"\"input\":{\"gui_up\":[87],\"gui_down\":[83],\"gui_left\":[65],"
		"\"gui_right\":[68],\"gui_click\":[32],\"gui_click_alt\":[66]}}");
	assert(gstate.config_user.root);
	gstate.config.view_bobbing = true;
	window = (GLFWwindow*)1;
	input_init();
	struct screen parent = {0};
	screen_options_set_return(&parent);
	screen_set(&screen_options);
	cursor_x = 200;
	cursor_y = 110; /* Music; remains here while controller selects Display. */
	menu_frame(0, 0.016F);
	menu_press('S'); /* Sound */
	menu_press('A'); /* 95% */
	menu_frame('A', 0.36F); /* held repeat: 90% */
	menu_press('S'); /* Display */
	cursor_x += 2; /* IR jitter must not undo the controller's focus. */
	menu_press(' ');
	assert(gstate.players[0].current_screen == &screen_video_settings);
	assert(config_read_int(&gstate.config_user, "audio.volume", -1) == 90);
	assert(config_read_int(&gstate.config_user, "music.volume", -1) == 100);
	menu_press('S'); /* Back, pointer still over View Bobbing */
	menu_press(' ');
	assert(gstate.players[0].current_screen == &screen_options);
	assert(gstate.config.view_bobbing);
	/* Offscreen pointer still allows the focused Music slider to reach 0
	 * and 100 in exact 5% steps, without changing Sound. */
	cursor_x = -1;
	menu_press('A');
	for(int k = 0; k < 25; k++)
		menu_frame('A', 0.4F);
	menu_press('B');
	assert(gstate.players[0].current_screen == &parent);
	assert(config_read_int(&gstate.config_user, "music.volume", -1) == 0);
	screen_set(&screen_options);
	menu_press('D');
	for(int k = 0; k < 25; k++)
		menu_frame('D', 0.4F);
	menu_press('B');
	assert(gstate.players[0].current_screen == &parent);
	assert(config_read_int(&gstate.config_user, "music.volume", -1) == 100);
	assert(config_read_int(&gstate.config_user, "audio.volume", -1) == 90);

	/* The approved composition has actual 60x44 adjustment targets; labels
	 * and percentages between them are not invisible slider hit areas. */
	screen_set(&screen_options);
	/* next-gaps-0924 W1F: Display (target 2) gives the right half of its row
	 * to Difficulty (target 8), Beta's GuiSmallButton pair form; every other
	 * approved rect is unchanged. */
	const int xs[] = {332,332,56,476,0,524,524,56,326};
	/* Target 4 (Back) is the A1 hint row's Back cell (10-31): 428-456. */
	const int ys[] = {98,160,222,338,428,98,160,276,222};
	const int ws[] = {60,60,258,108,0,60,60,528,258};
	/* The row names bound roles only: with no Back glyph in this env the
	 * Back cell (and its pointer target) is honestly absent. */
	struct input_player_frame pf;
	input_prompt_frame(0, &pf);
	int bm, bc;
	bool back_bound = input_glyph_cells(pf.glyphs[IB_INVENTORY], &bm, &bc);
	for(int i = 0; i < 9; i++) {
		struct gutil_button_bounds r;
		bool has = screen_options_test_target_rect(i, &r);
		if(i == 4 && !back_bound) { assert(!has); continue; }
		assert(has);
		assert(r.y == ys[i]);
		assert(r.height == (i == 4 ? 28 : 44));
		if(i != 4) { assert(r.x == xs[i]); assert(r.width == ws[i]); }
		else assert(r.x >= 32 && r.x + r.width <= 608);
	}
	assert(!screen_options_test_target_rect(-1, NULL));
	assert(!screen_options_test_target_rect(9, NULL));
	/* Approved split-OFF confirmation: exact 640x480 panel and safe-default
	 * button geometry. These are the production hit rectangles. */
	{
		const int ids[] = {-1, 0, 1};
		const int mx[] = {84, 108, 332};
		const int my[] = {138, 280, 280};
		const int mw[] = {472, 196, 196};
		const int mh[] = {220, 44, 44};
		for(int i = 0; i < 3; i++) {
			struct gutil_button_bounds modal;
			assert(screen_options_test_split_off_rect(ids[i], &modal));
			assert(modal.x == mx[i] && modal.y == my[i]
				&& modal.width == mw[i] && modal.height == mh[i]);
		}
		assert(!screen_options_test_split_off_rect(2, NULL));
	}
	assert(!screen_options_test_target_enabled(5)); /* Music already 100%. */
	cursor_x = 360; cursor_y = 120; /* Music minus. */
	menu_press(' ');
	assert(screen_options_test_focus() == 0);
	assert(screen_options_test_target_enabled(5));
	/* Hold while crossing to Sound plus, losing tracking and reacquiring:
	 * only the first fresh edge adjusts; there is no drag/reacquire write. */
	cursor_x = 550; cursor_y = 180;
	menu_frame(' ', 0.016F);
	cursor_x = -1; menu_frame(' ', 0.016F);
	cursor_x = 550; menu_frame(' ', 0.016F);
	menu_press('B');
	assert(gstate.players[0].current_screen == &parent);
	assert(config_read_int(&gstate.config_user, "music.volume", -1) == 95);
	assert(config_read_int(&gstate.config_user, "audio.volume", -1) == 90);

	/* Percentage padding must not activate the default minus button. */
	screen_set(&screen_options);
	cursor_x = 440; cursor_y = 120;
	menu_press(' ');
	menu_press('B');
	assert(config_read_int(&gstate.config_user, "music.volume", -1) == 95);

	/* A directional Sound selection wins over a resting Music sprite;
	 * Click+Close cannot escape in the same frame. */
	screen_set(&screen_options);
	cursor_x = 360; cursor_y = 120; menu_frame(0, 0.016F);
	menu_press('S'); /* Sound, with directional ownership. */
	cursor_x = -1; menu_frame(0, 0.016F);
	cursor_x = 550; cursor_y = 120; menu_frame(0, 0.016F);
	assert(screen_options_test_focus() == 1);
	keys[' '] = keys['B'] = true; input_poll();
	screen_options.update(&screen_options, 0.016F);
	assert(gstate.players[0].current_screen == &screen_options);
	assert(screen_options_test_focus() == 1);
	menu_frame(' ', 0.016F);
	menu_press('B');
	assert(config_read_int(&gstate.config_user, "music.volume", -1) == 95);
	assert(config_read_int(&gstate.config_user, "audio.volume", -1) == 85);
	assert(config_set_int(&gstate.config_user, "audio.volume", 90));

	/* Fresh plus restores 100; endpoint clicks and simultaneous movement
	 * cannot overflow or apply a second setting change. */
	screen_set(&screen_options);
	cursor_x = 550; cursor_y = 120; menu_press(' ');
	assert(!screen_options_test_target_enabled(5));
	menu_press(' ');
	cursor_x = -1; menu_frame(0, 0.016F);
	keys['A'] = keys[' '] = true; input_poll();
	screen_options.update(&screen_options, 0.016F); /* Left alone gives 95. */
	menu_press('B');
	assert(config_read_int(&gstate.config_user, "music.volume", -1) == 95);
	assert(config_read_int(&gstate.config_user, "audio.volume", -1) == 90);
	screen_set(&screen_options);
	menu_press('D'); menu_press('B');
	assert(config_read_int(&gstate.config_user, "music.volume", -1) == 100);
	cursor_x = -1;

	/* An asynchronously settled split modal owns its activation/Close edge
	 * until a neutral frame. Exercise the real screen update: the same-frame A
	 * cannot adjust the underlying Music row, and Back cannot exit Options. */
	assert(config_set_int(&gstate.config_user, "music.volume", 95));
	screen_set(&screen_options);
	menu_frame(0, 0.016F);
	screen_options_test_split_off_rearm_arm();
	memset(keys, 0, sizeof(keys)); keys[' '] = true; input_poll();
	screen_options.update(&screen_options, 0.016F);
	assert(gstate.players[0].current_screen == &screen_options);
	menu_frame(0, 0.016F); /* neutral releases the quarantine */
	menu_press('B');
	assert(gstate.players[0].current_screen == &parent);
	assert(config_read_int(&gstate.config_user, "music.volume", -1) == 95);
	screen_set(&screen_options);
	menu_frame(0, 0.016F);
	screen_options_test_split_off_rearm_arm();
	memset(keys, 0, sizeof(keys)); keys['B'] = true; input_poll();
	screen_options.update(&screen_options, 0.016F);
	assert(gstate.players[0].current_screen == &screen_options);
	menu_frame(0, 0.016F);
	menu_press('B');
	assert(gstate.players[0].current_screen == &parent);
	/* Restore the legacy fixture baseline for the Display/save-failure cases
	 * below; the quarantine assertion deliberately used 95% as its sentinel. */
	assert(config_set_int(&gstate.config_user, "music.volume", 100));

	/* Toggle through the real screen, save with B, and reopen. */
	screen_set(&screen_video_settings);
	menu_press(' ');
	assert(!gstate.config.view_bobbing);
	menu_press('B');
	assert(config_read_int(&gstate.config_user, "render.view_bobbing", -1) == 0);
	/* Canonical activation consumes a simultaneous Close after toggling. */
	screen_set(&screen_video_settings);
	menu_frame(0, 0.016F);
	keys[' '] = keys['B'] = true;
	input_poll();
	screen_video_settings.update(&screen_video_settings, 0.016F);
	assert(gstate.players[0].current_screen == &screen_video_settings);
	assert(gstate.config.view_bobbing);
	menu_press('B');
	assert(gstate.players[0].current_screen == &screen_options);
	assert(config_read_int(&gstate.config_user, "render.view_bobbing", -1) == 1);

	/* The Display control is the approved toggle, and pause context must
	 * retain the world in BOTH screens instead of replacing it with scenery. */
	screen_options_set_return(&screen_pause);
	screen_set(&screen_options); assert(screen_options.render_world);
	screen_set(&screen_video_settings); assert(screen_video_settings.render_world);
	struct gutil_button_bounds bobbing;
	assert(screen_video_settings_test_target_rect(0, &bobbing));
	assert(bobbing.x == 476 && bobbing.y == 132 && bobbing.width == 108 && bobbing.height == 44);
	menu_press('A'); assert(!gstate.config.view_bobbing);
	menu_press('D'); assert(gstate.config.view_bobbing);
	menu_press('B');
	screen_options_set_return(&parent);
	screen_set(&screen_options); assert(!screen_options.render_world);
	screen_set(&screen_video_settings); assert(!screen_video_settings.render_world);
	menu_press('B');

	/* Save failure retains the screen and restores the previous audio value. */
	assert(remove("config.json") == 0);
	assert(mkdir("config.json", 0700) == 0);
	menu_press('A');
	menu_press('B');
	assert(gstate.players[0].current_screen == &screen_options);
	assert(config_read_int(&gstate.config_user, "music.volume", -1) == 100);
	assert(rmdir("config.json") == 0);
	window = NULL;
	screen_options_set_return(NULL);
	config_destroy(&gstate.config_user);
	assert(chdir(previous) == 0);
	assert(rmdir(scratch) == 0);
	puts("  native menus: explicit volume buttons, arbitration, independent values, pause context and rollback PASS");
}

struct container_capture {
	unsigned clicks, closes, drops;
	size_t slot;
};

static void capture_container_rpc(struct server_rpc* call, void* user) {
	struct container_capture* c = user;
	if(call->type == SRPC_WINDOW_CLICK) {
		c->clicks++;
		c->slot = call->payload.window_click.slot;
	} else if(call->type == SRPC_WINDOW_CLOSE)
		c->closes++;
	else if(call->type == SRPC_WINDOW_DROP)
		c->drops++;
}

static void test_container_input_order(void) {
	blocks_init();
	items_init();
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	gstate.world_loaded = true;
	gstate.config_user.root = json_parse_string(
		"{\"input\":{\"inventory\":[69],\"gui_click\":[32],"
		"\"gui_click_alt\":[66],\"gui_right\":[68]}}");
	assert(gstate.config_user.root);
	window = (GLFWwindow*)1;
	input_init();
	struct window_container wc[4];
	struct screen* screens[] = {&screen_inventory, &screen_crafting,
		&screen_furnace, &screen_chest};
	const int ids[] = {WINDOWC_INVENTORY, WINDOWC_CRAFTING,
		WINDOWC_FURNACE, WINDOWC_CHEST};
	const enum window_type types[] = {WINDOW_TYPE_INVENTORY, WINDOW_TYPE_WORKBENCH,
		WINDOW_TYPE_FURNACE, WINDOW_TYPE_CHEST};
	const size_t sizes[] = {INVENTORY_SIZE, CRAFTING_SIZE, FURNACE_SIZE, CHEST_SIZE};
	const size_t main_slots[] = {INVENTORY_SLOT_MAIN, CRAFTING_SLOT_MAIN,
		FURNACE_SLOT_MAIN, CHEST_SLOT_MAIN};
	for(int k = 0; k < 4; k++) {
		assert(windowc_create(&wc[k], types[k], sizes[k]));
		gstate.players[0].windows[ids[k]] = &wc[k];
	}
	screen_crafting_set_windowc(WINDOWC_CRAFTING);
	screen_furnace_set_windowc(WINDOWC_FURNACE);
	screen_chest_set_windowc(WINDOWC_CHEST);
	const int first_x[] = {176, 290, 176, 176};
	const int first_y[] = {238, 152, 238, 198};
	const int panel_left[] = {134, 32, 134, 144};
	for(int k = 0; k < 4; k++) {
		screen_set(screens[k]);
		/* Workbench opens Recipes; use its public Manual Grid action. */
		if(k == 1) menu_press('T');
		cursor_x = first_x[k];
		cursor_y = first_y[k];
		menu_frame(0, 0.016F);
		/* Move to a DIFFERENT slot on the same frame as Confirm. */
		cursor_x += 36;
		menu_frame(' ', 0.016F);
		struct container_capture capture = {0};
		svin_process_messages(capture_container_rpc, &capture, false);
		assert(capture.clicks == 1 && capture.slot == main_slots[k] + 1);
		windowc_discard_pending_actions(&wc[k]);
		/* A pointer over panel padding must not click the old focused slot. */
		menu_frame(0, 0.016F);
		cursor_x = panel_left[k] + 2;
		menu_frame(' ', 0.016F);
		capture = (struct container_capture){0};
		svin_process_messages(capture_container_rpc, &capture, false);
		assert(capture.clicks == 0 && capture.drops == 0);
		/* Lost IR retains controller focus, including same-frame navigation. */
		cursor_x = -1;
		menu_frame(0, 0.016F);
		keys['D'] = keys[' '] = true;
		input_poll();
		screens[k]->update(screens[k], 0.016F);
		svin_process_messages(capture_container_rpc, &capture, false);
		assert(capture.clicks == 0 && "same-frame navigation consumes A");
		menu_frame(0, 0.016F);
		menu_press(' ');
		svin_process_messages(capture_container_rpc, &capture, false);
		assert(capture.clicks == 1 && capture.slot == main_slots[k] + 2);
		windowc_discard_pending_actions(&wc[k]);
		menu_frame(0, 0.016F);
		keys['E'] = keys[' '] = true;
		input_poll();
		screens[k]->update(screens[k], 0.016F);
		capture = (struct container_capture){0};
		svin_process_messages(capture_container_rpc, &capture, false);
		if(k==1){
			assert(!capture.closes&&!capture.clicks&&!capture.drops);
			assert(gstate.players[0].recipes.active&&!gstate.players[0].manual_grid.active);
			menu_press('E');capture=(struct container_capture){0};
			svin_process_messages(capture_container_rpc,&capture,false);
		}
		assert(capture.closes == 1 && capture.clicks == 0 && capture.drops == 0);
	}
	for(int k = 0; k < 4; k++) {
		windowc_destroy(&wc[k]);
		gstate.players[0].windows[ids[k]] = NULL;
	}
	world_destroy(&gstate.world);
	config_destroy(&gstate.config_user);
	window = NULL;
	puts("R6 PASS: current pointer, blank padding, lost IR/navigation and close+click across all four containers");
}

/* next-gaps-0924 W1F: the Difficulty row. Beta: GameSettings.difficulty
 * default 2 (GameSettings.java:41); a click cycles `difficulty + 1 & 3`
 * (GameSettings.java:125 via GuiOptions.actionPerformed); the label is
 * "Difficulty: <name>" (GameSettings.java:172); it is saved as a game option
 * (GameSettings.java:287) and read back at the next start (:234-235). */
static void test_difficulty_row(void) {
	char previous[4096], scratch[96];
	assert(getcwd(previous, sizeof(previous)));
	snprintf(scratch, sizeof(scratch), "test_difficulty_options_%ld", (long)GETPID());
	assert(mkdir(scratch, 0700) == 0 && chdir(scratch) == 0);
	memset(&gstate, 0, sizeof(gstate));
	gstate.config_user.root = json_parse_string(
		"{\"input\":{\"gui_up\":[87],\"gui_down\":[83],\"gui_left\":[65],"
		"\"gui_right\":[68],\"gui_click\":[32],\"gui_click_alt\":[66]}}");
	assert(gstate.config_user.root);
	window = (GLFWwindow*)1;
	input_init();
	struct screen parent = {0};
	screen_options_set_return(&parent);
	server_local_request_difficulty(2);

	/* No key yet: the row shows the Beta default. */
	screen_set(&screen_options);
	cursor_x = -1;
	menu_frame(0, 0.016F);
	assert(screen_options_test_difficulty() == 2);
	assert(!strcmp(screen_options_test_difficulty_label(), "Difficulty: Normal"));
	/* Astra W1F focus order: Music -> Sound -> Display/Difficulty ->
	 * Controller settings -> Split -> Back; the paired row is entered on
	 * Display from either side, and Up/Down leave either half identically. */
	menu_press('S');
	assert(screen_options_test_focus() == 1);
	menu_press('S');
	assert(screen_options_test_focus() == 2);
	menu_press('S');
	assert(screen_options_test_focus() == 7);
	menu_press('S');
	assert(screen_options_test_focus() == 3);
	menu_press('S');
	assert(screen_options_test_focus() == 4);
	menu_press('W');
	menu_press('W');
	assert(screen_options_test_focus() == 7);
	menu_press('W');
	assert(screen_options_test_focus() == 2); /* entered on Display */
	menu_press('D');
	assert(screen_options_test_focus() == 8);
	menu_press('S');
	assert(screen_options_test_focus() == 7); /* Down from Difficulty */
	menu_press('W');
	menu_press('D');
	menu_press('W');
	assert(screen_options_test_focus() == 1); /* Up from Difficulty */
	menu_press('S');
	menu_press('D');
	assert(screen_options_test_focus() == 8);
	/* Held Right: focus only, never a value change; a fresh A cycles
	 * Normal -> Hard, live. */
	menu_frame('D', 0.5F);
	assert(screen_options_test_difficulty() == 2);
	menu_press(' ');
	assert(screen_options_test_difficulty() == 3);
	assert(!strcmp(screen_options_test_difficulty_label(), "Difficulty: Hard"));
	assert(server_local_test_requested_difficulty() == 3);
	/* Holding A does not keep cycling: only the fresh edge counts. */
	keys[' '] = true; input_poll();
	for(int k = 0; k < 30; k++)
		screen_options.update(&screen_options, 0.1F);
	assert(screen_options_test_difficulty() == 3);
	memset(keys, 0, sizeof(keys)); input_poll();
	/* Left returns to Display without touching the value. */
	menu_press('A');
	assert(screen_options_test_focus() == 2);
	assert(screen_options_test_difficulty() == 3);
	menu_press('D');
	menu_press(' '); /* Hard -> Peaceful (the & 3 wrap) */
	assert(screen_options_test_difficulty() == 0);
	assert(!strcmp(screen_options_test_difficulty_label(), "Difficulty: Peaceful"));
	assert(server_local_test_requested_difficulty() == 0);
	menu_press(' ');
	assert(!strcmp(screen_options_test_difficulty_label(), "Difficulty: Easy"));
	menu_press(' ');
	menu_press(' ');
	menu_press(' ');
	assert(screen_options_test_difficulty() == 0); /* full cycle */
	menu_press('B');
	assert(gstate.players[0].current_screen == &parent);
	assert(config_read_difficulty(&gstate.config_user) == 0);
	/* Persisted across a restart: the file on disk carries it. */
	{
		struct config saved;
		assert(config_create(&saved, "config.json"));
		assert(config_read_difficulty(&saved) == 0);
		config_destroy(&saved);
	}

	/* Pointer route: the exact Difficulty rect; the row reopens on the file. */
	screen_set(&screen_options);
	assert(screen_options_test_difficulty() == 0);
	cursor_x = 455; cursor_y = 244;
	menu_press(' ');
	assert(screen_options_test_focus() == 8);
	assert(screen_options_test_difficulty() == 1);
	assert(server_local_test_requested_difficulty() == 1);
	cursor_x = -1;
	menu_press('B');
	assert(config_read_difficulty(&gstate.config_user) == 1);

	/* A remote multiplayer server owns its difficulty: inside a remote
	 * session the row is disabled -- neither the option nor the local
	 * request moves. */
	gstate.remote_session = true;
	screen_set(&screen_options);
	menu_frame(0, 0.016F);
	assert(!screen_options_test_target_enabled(8));
	assert(!strcmp(screen_options_test_difficulty_label(), "Controlled by server"));
	menu_press('S'); menu_press('S');
	assert(screen_options_test_focus() == 2);
	menu_press('D'); /* the disabled half is not a focus stop */
	assert(screen_options_test_focus() == 2);
	/* A pointer press on the disabled button is consumed: no mutation. */
	cursor_x = 455; cursor_y = 244;
	menu_press(' ');
	assert(gstate.players[0].current_screen == &screen_options);
	assert(screen_options_test_difficulty() == 1);
	assert(server_local_test_requested_difficulty() == 1);
	cursor_x = -1;
	menu_press('B');
	assert(config_read_difficulty(&gstate.config_user) == 1);
	gstate.remote_session = false;
	server_local_request_difficulty(2);

	/* Failed save: the file keeps the old value and so does the world. */
	assert(config_set_difficulty(&gstate.config_user, 1));
	server_local_request_difficulty(1);
	assert(remove("config.json") == 0 && mkdir("config.json", 0700) == 0);
	screen_set(&screen_options);
	menu_frame(0, 0.016F);
	menu_press('S'); menu_press('S'); menu_press('D');
	menu_press(' ');
	assert(server_local_test_requested_difficulty() == 2);
	menu_press('B');
	assert(gstate.players[0].current_screen == &screen_options);
	assert(config_read_difficulty(&gstate.config_user) == 1);
	assert(server_local_test_requested_difficulty() == 1);
	assert(screen_options_test_difficulty() == 1);
	assert(rmdir("config.json") == 0);

	/* A corrupt stored value reads as Normal. */
	assert(config_set_int(&gstate.config_user, "game.difficulty", 9));
	screen_set(&screen_options);
	assert(screen_options_test_difficulty() == 2);
	cursor_x = -1;
	menu_press('B');
	assert(remove("config.json") == 0);

	server_local_request_difficulty(2);
	window = NULL;
	screen_options_set_return(NULL);
	config_destroy(&gstate.config_user);
	assert(chdir(previous) == 0 && rmdir(scratch) == 0);
	puts("PASS Difficulty row: default Normal, +1&3 cycle by A/pointer, live request, persisted, remote-safe, save rollback");
}

int main(void) {
	test_slider_math();
	test_control_inventory();
	test_video_control_inventory();
	test_pause_button_inventory();
	test_label_composition();
	test_video_label_composition();
	test_config_round_trip();
	test_pause_edges();
	test_native_focus_and_repeat();
	test_native_screen_interaction();
	test_controller_settings();
	test_difficulty_row();
	test_container_input_order();
	puts("test_options_env: PASS");
	return 0;
}
