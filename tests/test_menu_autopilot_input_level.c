/*
	ui-build-1026 T15c: the menus_dwell walker's LEVEL and RELEASE edge
	(source/platform/input.c input_walker_level / input_walker_latch).

	What it pins, against the REAL PLATFORM_PC input.c (only GLFW and the
	walker's menu_autopilot_force_button are doubled, the latter shaped
	exactly like menu_autopilot.c's: it answers only for the one button the
	walker presses this frame):
	  (a) walker INACTIVE: input_held / input_released are exactly the
	      hardware state, frame by frame, for a bound key -- and so is a walker
	      that is ARMED but pressing a different button;
	  (b) ONE walker press of IB_GUI_PAGE_SWAP reads press+held on its frame and
	      released on the next, so the T11 detector (input_menu_page_read, fed
	      exactly as container_menu_read_input feeds it) fires page_swap EXACTLY
	      ONCE and never the hold; a second walker press fires a second swap.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#undef GLFW_DLL
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "game/chat_login_script.h"
#include "game/game_state.h"
#include "platform/input.h"
#include "platform/input_owner.h"

/* ---- fake GLFW (the test_input_edges shape) ---------------------------- */
static bool fake_keys[GLFW_KEY_LAST + 1];
static bool fake_buttons[GLFW_MOUSE_BUTTON_LAST + 1];
GLFWwindow* window = (GLFWwindow*)0x1;

int glfwGetKey(GLFWwindow* w, int key) {
	(void)w;
	assert(key >= 0 && key <= GLFW_KEY_LAST);
	return fake_keys[key] ? GLFW_PRESS : GLFW_RELEASE;
}
int glfwGetMouseButton(GLFWwindow* w, int button) {
	(void)w;
	return fake_buttons[button] ? GLFW_PRESS : GLFW_RELEASE;
}
void glfwSetInputMode(GLFWwindow* w, int mode, int value) {
	(void)w; (void)mode; (void)value;
}
void glfwSetCursorPos(GLFWwindow* w, double x, double y) {
	(void)w; (void)x; (void)y;
}
void glfwGetCursorPos(GLFWwindow* w, double* x, double* y) {
	(void)w;
	*x = 0.0;
	*y = 0.0;
}
GLFWcharfun glfwSetCharCallback(GLFWwindow* w, GLFWcharfun cb) {
	(void)w; (void)cb;
	return NULL;
}
GLFWkeyfun glfwSetKeyCallback(GLFWwindow* w, GLFWkeyfun cb) {
	(void)w; (void)cb;
	return NULL;
}

/* ---- engine symbols input.c references -------------------------------- */
struct game_state gstate;
int gfx_width(void) { return 640; }
int gfx_height(void) { return 480; }
bool autopilot_force_button(enum input_button b, bool* out) {
	(void)b; (void)out;
	return false;
}
bool chat_login_pad_for(unsigned player, int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION],
						const size_t* lengths, struct chat_login_pad* out) {
	(void)player; (void)map; (void)lengths; (void)out;
	return false;
}
bool chat_login_pad_key(void* user, int key) {
	(void)user; (void)key;
	return false;
}
bool chat_login_pad_native_key(int key, bool* value) {
	(void)key; (void)value;
	return false;
}

/* ---- the walker double: menu_autopilot.c's force_button, verbatim shape -- */
static bool walker_armed;
static enum input_button walker_button = IB_COUNT; /* this frame's press */

bool menu_autopilot_force_button(enum input_button b, bool* out) {
	if(!walker_armed)
		return false;
	if(walker_button != b)
		return false;
	*out = true;
	return true;
}
bool menu_autopilot_force_pointer(float* x, float* y, float* angle,
								  bool* available) {
	(void)x; (void)y; (void)angle; (void)available;
	return false;
}

/* ---- helpers ----------------------------------------------------------- */
#define PAGE_KEY 81 /* 'Q' */
static const char* BINDINGS
	= "{\"input\": {\"gui_page_swap\": [81], \"gui_up\": [87]}}";

/* container_menu_read_input's page frame, verbatim. */
static struct input_menu_page read_page(void) {
	struct input_player_frame f = {.connected = true, .category = INPUT_CAT_NONE};
	f.down[IB_GUI_PAGE_SWAP] = input_held(IB_GUI_PAGE_SWAP);
	f.pressed[IB_GUI_PAGE_SWAP] = input_pressed(IB_GUI_PAGE_SWAP);
	f.released[IB_GUI_PAGE_SWAP] = input_released(IB_GUI_PAGE_SWAP);
	struct input_menu_page out;
	input_menu_page_read(0, &f, &out);
	return out;
}

/* One main.c frame: walker tick (sets this frame's press), consumers read,
 * then input_poll() at the bottom. */
struct frame_read {
	bool held, released;
	struct input_menu_page page;
};
static struct frame_read run_frame(enum input_button walker_press) {
	walker_button = walker_press;
	struct frame_read r = {
		.held = input_held(IB_GUI_PAGE_SWAP),
		.released = input_released(IB_GUI_PAGE_SWAP),
	};
	r.page = read_page();
	input_poll();
	return r;
}

/* (a) Hardware levels with the walker inactive, then armed-but-elsewhere. */
static void test_inactive_is_hardware(bool armed_elsewhere) {
	walker_armed = armed_elsewhere;
	enum input_button other = armed_elsewhere ? IB_GUI_UP : IB_COUNT;
	memset(fake_keys, 0, sizeof(fake_keys));
	input_poll();
	input_poll();
	/* key: up, down x3, up x2 */
	const bool key_down[] = {false, true, true, true, false, false};
	const bool want_held[] = {false, true, true, true, false, false};
	const bool want_rel[] = {false, false, false, false, true, false};
	for(size_t k = 0; k < sizeof(key_down) / sizeof(key_down[0]); k++) {
		fake_keys[PAGE_KEY] = key_down[k];
		input_poll(); /* the hardware latch for this frame */
		walker_button = other;
		assert(input_held(IB_GUI_PAGE_SWAP) == want_held[k]);
		assert(input_released(IB_GUI_PAGE_SWAP) == want_rel[k]);
		assert(input_pressed(IB_GUI_PAGE_SWAP) == (k == 1));
	}
	fake_keys[PAGE_KEY] = false;
	input_poll();
	walker_armed = false;
	walker_button = IB_COUNT;
	printf("PASS walker %s: input_held/input_released are the hardware state\n",
		   armed_elsewhere ? "armed on another button" : "inactive");
}

/* (b) One walker press = one tap = exactly one page_swap, never the hold. */
static void test_walker_tap_fires_once(void) {
	memset(fake_keys, 0, sizeof(fake_keys));
	walker_armed = true;
	input_menu_page_cancel(0);
	int swaps = 0, holds = 0;
	struct frame_read r;
	for(int k = 0; k < 3; k++) { /* neutral first, as on a fresh screen */
		r = run_frame(IB_COUNT);
		swaps += r.page.page_swap;
		holds += r.page.hold;
	}
	r = run_frame(IB_GUI_PAGE_SWAP); /* THE press frame */
	assert(r.held && !r.released);
	swaps += r.page.page_swap;
	holds += r.page.hold;
	r = run_frame(IB_COUNT); /* the release frame */
	assert(!r.held && r.released);
	swaps += r.page.page_swap;
	assert(swaps == 1 && "one walker press must fire page_swap exactly once");
	for(int k = 0; k < 60; k++) { /* a full second idle: nothing more */
		r = run_frame(IB_COUNT);
		assert(!r.held && !r.released);
		swaps += r.page.page_swap;
		holds += r.page.hold;
	}
	assert(swaps == 1 && holds == 0);
	(void)run_frame(IB_GUI_PAGE_SWAP);
	r = run_frame(IB_COUNT);
	swaps += r.page.page_swap;
	assert(swaps == 2 && "a second walker press fires a second swap");
	walker_armed = false;
	walker_button = IB_COUNT;
	input_poll();
	printf("PASS one walker press of PAGE_SWAP fires exactly one page swap\n");
}

int main(void) {
	gstate.config_user.root = json_parse_string(BINDINGS);
	assert(gstate.config_user.root);
	input_init();
	test_inactive_is_hardware(false);
	test_inactive_is_hardware(true);
	test_walker_tap_fires_once();
	printf("test_menu_autopilot_input_level: ALL PASS\n");
	return 0;
}
