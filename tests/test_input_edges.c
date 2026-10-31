/*
	Input-layer edge semantics regression test (2026-08-14).

	Guards the merged-mask model in source/platform/input.c: every binding source of
	one logical action ORs into ONE down-state per frame, and press/release edges are
	computed once per action against the previous frame's state.

	What it pins:
	  (a) a press still fires when a SIBLING binding of another action holds the same
	      key -- the swallow this replaced (the old native layer handed out edges per
	      QUERY and mutated its own held[] table, so the first consumer of a key in a
	      frame saw "pressed" and every later one saw "held", which `!any_held` then
	      turned into a dropped press; the shipped PC config binds 87 to BOTH
	      player_forward and gui_up);
	  (b) a release fires only once ALL sources have cleared;
	  (c) no double edge within one frame -- the answer is idempotent across repeated
	      queries in any order, and a press does not re-fire while held;
	  (d) chords latch as a unit and MASK their component keys while down, and a chord
	      now contributes a release edge (input_released used to skip chord decoding
	      entirely, unlike pressed/held);
	  (e) PC mouse codes 1000..1007 are mouse buttons, not chords (the old `code < 1000`
	      chord cutoff decoded "item_action_left": [1000] as hold-key-1 + press-key-0,
	      so mouse place/break could never fire through input_pressed/input_held).

	The real PLATFORM_PC body of input.c is the code under test; only GLFW itself is
	faked (below), so the native level-read, the config binding walk, the chord decode
	and the latch are all production code.

	Also covers (2026-09-01, GameCube pad wiring): input_stick_normalize, the pure
	analog-stick-to-(dx,dy,magnitude) normalizer input.c's PLATFORM_WII GameCube path
	calls (PAD_StickX/Y, PAD_SubStickX/Y). It lives OUTSIDE both #ifdef PLATFORM_*
	blocks (input.c's shared section) specifically so this PC-only test suite links
	and exercises the real production function -- the actual WPAD/PAD button-mask
	translation tables (input_wpad_translate/input_pad_translate) stay untestable
	here, same as before this addition: they only compile under PLATFORM_WII, which
	needs real libogc/wiiuse headers this harness does not build against. See
	input.h's declaration comment.

	(f) (2026-09-03, task_0d54671b root cause) PLATFORM_PC's
	input_native_key_symbol IS built and linked here (it needs no libogc/wiiuse
	headers) and is now exercised through the public input_symbol() -- the same
	entry point gutil_control_icon calls: a keyboard-bound action must pick
	controls.png's generic keyboard-key cell (42) and a mouse-button-bound
	action the generic mouse-click cell (43), pinned against
	tools/gen_cc0_controls.py's CELL_KEY_KEYBOARD/CELL_KEY_MOUSE constants so
	the lookup table and the atlas generator cannot drift apart again. Before
	the fix this function returned the SAME constant (7, the atlas's Guitar
	Hero fret cell) for every key -- every PC control hint on every screen
	drew a Guitar Hero fret icon.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#undef GLFW_DLL
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "game/chat_login_script.h"
#include "game/game_state.h"
#include "platform/input.h"
#include "platform/input_glyph.h"
#include "platform/input_inventory.h"
#include "platform/input_owner.h"

// ---- fake GLFW: a flat key/button table the test drives directly --------------
// input.c reads key state through glfwGetKey/glfwGetMouseButton only, so replacing
// those two is enough to author any physical input. Pointer tests below also
// use the fake cursor position through the real input_pointer path.

static bool fake_keys[GLFW_KEY_LAST + 1];
static bool fake_taps[GLFW_KEY_LAST + 1];
static bool fake_buttons[GLFW_MOUSE_BUTTON_LAST + 1];
static double fake_cursor_x, fake_cursor_y;

static GLFWwindow* const FAKE_WINDOW = (GLFWwindow*)0x1;
GLFWwindow* window = (GLFWwindow*)0x1; // non-NULL: take the real (non-headless) path

int glfwGetKey(GLFWwindow* w, int key) {
	assert(w == FAKE_WINDOW);
	assert(key >= 0 && key <= GLFW_KEY_LAST); // input.c must bounds-check before asking
	bool down = fake_keys[key] || fake_taps[key];
	fake_taps[key] = false; /* GLFW sticky keys consume a completed tap on read. */
	return down ? GLFW_PRESS : GLFW_RELEASE;
}

int glfwGetMouseButton(GLFWwindow* w, int button) {
	assert(w == FAKE_WINDOW);
	assert(button >= 0 && button <= GLFW_MOUSE_BUTTON_LAST);
	return fake_buttons[button] ? GLFW_PRESS : GLFW_RELEASE;
}

void glfwSetInputMode(GLFWwindow* w, int mode, int value) {
	(void)w;
	(void)mode;
	(void)value;
}

void glfwSetCursorPos(GLFWwindow* w, double x, double y) {
	(void)w;
	(void)x;
	(void)y;
}

void glfwGetCursorPos(GLFWwindow* w, double* x, double* y) {
	(void)w;
	*x = fake_cursor_x;
	*y = fake_cursor_y;
}

// Text-queue callback registration (input_text_enable): capture the
// registered functions so the test can drive them like GLFW would.
static GLFWcharfun captured_char_cb;
static GLFWkeyfun captured_key_cb;

GLFWcharfun glfwSetCharCallback(GLFWwindow* w, GLFWcharfun cb) {
	(void)w;
	GLFWcharfun prev = captured_char_cb;
	captured_char_cb = cb;
	return prev;
}

GLFWkeyfun glfwSetKeyCallback(GLFWwindow* w, GLFWkeyfun cb) {
	(void)w;
	GLFWkeyfun prev = captured_key_cb;
	captured_key_cb = cb;
	return prev;
}

// ---- engine symbols input.c references but this test never exercises ----------

struct game_state gstate;

int gfx_width(void) {
	return 640;
}

int gfx_height(void) {
	return 480;
}

bool autopilot_force_button(enum input_button b, bool* out) {
	(void)b;
	(void)out;
	return false; // gstate.autopilot.active is 0 here, so this is never reached
}


/* SEAM S1 (mp_chat_login_3m, source/game/chat_login_script.h): input.c calls
 * these three predicates at its two shared seam sites. The driver itself is
 * not linked here -- this target deliberately keeps a minimal object set -- so
 * these are link-only stubs returning the DISARMED answer, which is exactly
 * the behaviour this test is meant to measure: today's input.c, untouched. */
static bool fake_profile_pad;
bool chat_login_pad_for(unsigned player, int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION],
						const size_t* lengths, struct chat_login_pad* out) {
	(void)map; (void)lengths;
	if(!fake_profile_pad || player != 0)
		return false;
	*out = (struct chat_login_pad){.connected = true};
	return true;
}
bool chat_login_pad_key(void* user, int key) {
	(void)user; (void)key;
	return false;
}
bool chat_login_pad_native_key(int key, bool* value) {
	(void)key; (void)value;
	return false;
}

bool menu_autopilot_force_button(enum input_button b, bool* out) {
	(void)b;
	(void)out;
	return false; // menu_autopilot_arm() is never called here, so never owned
}

bool menu_autopilot_force_pointer(float* x, float* y, float* angle,
								   bool* available) {
	(void)x;
	(void)y;
	(void)angle;
	(void)available;
	return false; // menu_autopilot_arm() is never called here, so never owned
}

static bool script_owns_pointer;
static bool test_pointer_script(float* x, float* y, float* angle,
							 bool* available) {
	(void)x; (void)y; (void)angle;
	if(!script_owns_pointer)
		return false;
	*available = false;
	return true;
}

// ---- helpers -----------------------------------------------------------------

// Deliberately overlapping bindings, mirroring the real configs:
//   player_forward + gui_up SHARE key 87 (config_pc.json does exactly this);
//   player_forward has two sources (87 and 200) -- the sibling case;
//   debug_overlay is the 9008 chord whose halves are the two scroll keys
//   (config_wii.json's couch chord), so the component-masking rule is under test;
//   item_action_left is the PC mouse button that the old chord cutoff swallowed.
static const char* BINDINGS = "{\"input\": {"
							  "\"player_forward\": [87, 200],"
							  "\"gui_up\": [87],"
							  "\"scroll_left\": [9],"
							  "\"scroll_right\": [8],"
							  "\"debug_overlay\": [9008],"
							  "\"item_action_left\": [1000]"
							  "}}";

static void key(int code, bool down) {
	if(code >= 1000)
		fake_buttons[code - 1000] = down;
	else
		fake_keys[code] = down;
}

static void all_up(void) {
	memset(fake_keys, 0, sizeof(fake_keys));
	memset(fake_buttons, 0, sizeof(fake_buttons));
}

// One frame: latch the current physical state, exactly as main.c's end-of-loop
// input_poll() does.
static void frame(void) {
	input_poll();
}

// A press edge must be stable no matter how often, or in what order, consumers ask
// within one frame -- the "no double edge" half of (c). Every query in this test
// goes through here so the property is checked everywhere, not just once.
static bool pressed(enum input_button b) {
	bool first = input_pressed(b);
	for(int k = 0; k < 4; k++)
		assert(input_pressed(b) == first);
	return first;
}

static bool released(enum input_button b) {
	bool first = input_released(b);
	for(int k = 0; k < 4; k++)
		assert(input_released(b) == first);
	return first;
}

static void test_ir_look_tracking(void) {
	struct input_ir_look_state s = {0};
	float x, y;
	for(int k = 0; k < 4; k++) {
		input_ir_look(&s, true, false, 0.1F, -0.1F, 0.016F, &x, &y);
		assert(x == 0 && y == 0); /* center dead zone */
	}
	input_ir_look(&s, true, false, 1, -1, 0.016F, &x, &y);
	assert(x > 0 && x <= 0.016F && y < 0 && y >= -0.016F);
	input_ir_look(&s, false, false, 1, -1, 0.016F, &x, &y);
	assert(x == 0 && y == 0 && s.valid_frames == 0);
	for(int k = 0; k < 2; k++) {
		input_ir_look(&s, true, false, 1, -1, 0.016F, &x, &y);
		assert(x == 0 && y == 0); /* reacquisition hysteresis */
	}
	input_ir_look(&s, true, true, 0.15F, -0.1F, 0.016F, &x, &y);
	for(int k = 0; k < 4; k++) {
		input_ir_look(&s, true, false, 0.15F, -0.1F, 0.016F, &x, &y);
		assert(x == 0 && y == 0);
	}
	input_ir_look(&s, true, false, 1, 1, 2, &x, &y);
	assert(x <= 0.05F && y <= 0.05F);
	input_ir_look(&s, true, false, NAN, 0, 0.016F, &x, &y);
	assert(x == 0 && y == 0 && s.valid_frames == 0);
	puts("R5 PASS: IR dead zone, bounded turning, recenter, tracking loss and reacquisition");
}

static bool owner_test_key(void* user, int keycode) {
	return keycode >= 0 && keycode < 1008 && ((bool*)user)[keycode];
}

static void test_input_owners(void) {
	struct input_owner owners[2] = {0};
	struct input_device remote = {INPUT_DEVICE_WPAD, 0};
	struct input_device pad = {INPUT_DEVICE_GAMECUBE, 0};
	assert(input_owner_bind(owners, 2, 0, remote));
	assert(!input_owner_bind(owners, 2, 1, remote));
	assert(input_owner_bind(owners, 2, 1, pad)); /* separate buses, same channel */
	struct input_owner saved[2];
	memcpy(saved, owners, sizeof(saved));
	assert(!input_owner_bind(owners, 2, 2, pad));
	assert(!input_owner_bind(owners, 2, 1, (struct input_device){INPUT_DEVICE_WPAD, 4}));
	assert(!input_owner_bind(owners, 2, 1, (struct input_device){INPUT_DEVICE_NONE, 0}));
	assert(!input_owner_bind(owners, 2, 1, (struct input_device){INPUT_DEVICE_KEYBOARD, 1}));
	assert(!memcmp(saved, owners, sizeof(saved)));
	assert(input_owner_bind(owners, 2, 1, (struct input_device){INPUT_DEVICE_WPAD, 3}));
	assert(input_owner_bind(owners, 2, 1, pad));

	struct input_player_frame a = {.connected = true, .category = INPUT_CAT_NUNCHUK};
	struct input_player_frame b = {.connected = true, .category = INPUT_CAT_GAMECUBE};
	a.down[IB_GUI_DROP] = true; /* held at join cannot throw a carried item */
	input_owner_latch(&owners[0], &a);
	input_owner_latch(&owners[1], &b);
	assert(!owners[0].frame.down[IB_GUI_DROP]);
	a.down[IB_GUI_DROP] = false;
	input_owner_latch(&owners[0], &a);
	a.down[IB_GUI_DROP] = true;
	input_owner_latch(&owners[0], &a);
	assert(owners[0].frame.pressed[IB_GUI_DROP]);
	assert(!owners[1].frame.pressed[IB_GUI_DROP]);
	assert(input_owner_bind(owners, 2, 0, remote)); /* no-op does not reset latch */
	input_owner_latch(&owners[0], &a);
	assert(owners[0].frame.down[IB_GUI_DROP] && !owners[0].frame.pressed[IB_GUI_DROP]);
	a.connected = false;
	input_owner_latch(&owners[0], &a);
	for(int k = 0; k < IB_COUNT; k++)
		assert(!owners[0].frame.down[k] && !owners[0].frame.pressed[k]
			&& !owners[0].frame.released[k]);
	a.connected = true;
	input_owner_latch(&owners[0], &a);
	assert(!owners[0].frame.down[IB_GUI_DROP]);
	a.down[IB_GUI_DROP] = false;
	input_owner_latch(&owners[0], &a);
	a.down[IB_GUI_DROP] = true;
	input_owner_latch(&owners[0], &a);
	assert(owners[0].frame.pressed[IB_GUI_DROP]);
	a.category = INPUT_CAT_CLASSIC_CONTROLLER;
	input_owner_latch(&owners[0], &a);
	assert(!owners[0].frame.down[IB_GUI_DROP] && !owners[0].frame.released[IB_GUI_DROP]);
	a.down[IB_GUI_DROP] = false;
	input_owner_latch(&owners[0], &a);
	a.down[IB_GUI_DROP] = true;
	input_owner_latch(&owners[0], &a);
	a.down[IB_GUI_DROP] = false;
	input_owner_latch(&owners[0], &a);
	assert(owners[0].frame.released[IB_GUI_DROP]);

	for(int k = 0; k < 1008; k++) {
		bool gc = input_owner_accepts(INPUT_CAT_GAMECUBE, k);
		bool cc = input_owner_accepts(INPUT_CAT_CLASSIC_CONTROLLER, k);
		bool nc = input_owner_accepts(INPUT_CAT_NUNCHUK, k);
		assert(!(gc && cc) && !(gc && nc) && !(cc && nc));
	}
	assert(input_owner_accepts(INPUT_CAT_NUNCHUK, 6));
	assert(input_owner_accepts(INPUT_CAT_NUNCHUK, 101));
	assert(input_owner_accepts(INPUT_CAT_CLASSIC_CONTROLLER, 207));
	assert(input_owner_accepts(INPUT_CAT_GAMECUBE, 407));
	assert(!input_owner_accepts(INPUT_CAT_GAMECUBE, 934));
	assert(!input_owner_accepts(INPUT_CAT_NUNCHUK, -1));

	int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION] = {0};
	size_t lengths[IB_COUNT] = {0};
	bool keys_a[1008] = {0}, keys_b[1008] = {0}, levels[IB_COUNT];
	map[IB_DEBUG_OVERLAY][0] = 9008; lengths[IB_DEBUG_OVERLAY] = 1;
	map[IB_GUI_PAGE][0] = 8; lengths[IB_GUI_PAGE] = 1;
	keys_a[9] = true; keys_b[8] = true;
	input_binding_levels(map, lengths, owner_test_key, keys_a, levels);
	assert(!levels[IB_DEBUG_OVERLAY] && !levels[IB_GUI_PAGE]);
	input_binding_levels(map, lengths, owner_test_key, keys_b, levels);
	assert(!levels[IB_DEBUG_OVERLAY] && levels[IB_GUI_PAGE]);
	keys_a[8] = true;
	input_binding_levels(map, lengths, owner_test_key, keys_a, levels);
	assert(levels[IB_DEBUG_OVERLAY] && !levels[IB_GUI_PAGE]);
	input_binding_levels(map, lengths, owner_test_key, keys_b, levels);
	assert(levels[IB_GUI_PAGE]); /* another player's chord cannot consume this */

	float x, y, angle;
	a.pointer_valid = a.pointer_normalized = true;
	a.pointer_x = .25F; a.pointer_y = .75F; a.pointer_angle = 15;
	assert(input_owner_pointer(&a, 0, 240, 640, 240, &x, &y, &angle));
	assert(x == 160 && y == 180 && angle == 15);
	a.pointer_normalized = false; a.pointer_x = 160; a.pointer_y = 420;
	assert(input_owner_pointer(&a, 0, 240, 640, 240, &x, &y, &angle));
	assert(x == 160 && y == 180);
	assert(!input_owner_pointer(&a, 0, 0, 640, 240, &x, &y, &angle));
	a.pointer_x = 640;
	assert(!input_owner_pointer(&a, 0, 240, 640, 240, &x, &y, &angle));
	a.pointer_x = NAN;
	input_owner_latch(&owners[0], &a);
	assert(!owners[0].frame.pointer_valid);
	assert(!input_owner_pointer(&a, 0, 240, 640, 240, &x, &y, &angle));
	puts("PASS input owners: exclusive devices, isolated edges/chords, safe reconnect, viewport pointers");

	/* Exercise the real PC poll/config/glyph path, not only the pure latch. */
	all_up(); frame();
	assert(!input_player_frame(0) && !input_player_frame(1));
	assert(!input_player_bind(0, remote)); /* PC cannot pretend a Wii device exists */
	assert(input_player_bind(1, (struct input_device){INPUT_DEVICE_KEYBOARD, 0}));
	assert(!input_player_bind(0, (struct input_device){INPUT_DEVICE_KEYBOARD, 0}));
	key(87, true); frame();
	assert(input_pressed(IB_GUI_UP)); /* legacy single-player path stays intact */
	assert(!input_player_frame(1)->down[IB_GUI_UP]);
	key(87, false); frame();
	key(87, true); frame();
	assert(input_player_frame(1)->pressed[IB_GUI_UP]);
	assert(input_player_frame(1)->glyphs[IB_GUI_UP]
		   == INPUT_GLYPH_PC_KEY_BASE + 87); /* W cap */
	assert(input_player_frame(1)->glyphs[IB_ACTION1]
		   == INPUT_GLYPH_PC_MOUSE_LEFT);
	struct input_player_frame before = *input_player_frame(1);
	for(int k = 0; k < 10; k++) assert(!memcmp(&before, input_player_frame(1), sizeof(before)));
	window = NULL; frame();
	assert(!input_player_frame(1)->connected && !input_player_frame(1)->released[IB_GUI_UP]);
	window = FAKE_WINDOW; frame();
	assert(!input_player_frame(1)->down[IB_GUI_UP]);
	input_player_unbind(1);
	assert(!input_player_frame(1) && !input_player_frame(INPUT_LOCAL_PLAYERS));
	input_player_unbind(INPUT_LOCAL_PLAYERS);
	all_up(); frame();
	puts("PASS native owned input: real polling/bindings/glyphs, immutable reads, legacy input unchanged");
}

static void test_classic_attachment_recovery(void) {
	struct input_owner owner = {0};
	assert(input_owner_bind(&owner, 1, 0, (struct input_device){INPUT_DEVICE_WPAD, 2}));
	struct input_player_frame sample = {.connected = true,
		.category = INPUT_CAT_CLASSIC_CONTROLLER, .pointer_valid = true,
		.pointer_x = .4F, .pointer_y = .6F, .look_x = .8F};
	sample.glyphs[IB_GUI_CLICK] = 10;
	input_owner_latch(&owner, &sample);
	assert(owner.frame.connected && !owner.frame.pointer_valid && owner.frame.look_x == 0);
	sample.look_x = 0; input_owner_latch(&owner, &sample);
	sample.look_x = .8F; sample.down[IB_GUI_CLICK] = true;
	input_owner_latch(&owner, &sample);
	assert(owner.frame.look_x == .8F && owner.frame.pressed[IB_GUI_CLICK]);
	sample.category = INPUT_CAT_WIIMOTE;
	sample.down[IB_GUI_DROP] = true;
	input_owner_latch(&owner, &sample);
	assert(!owner.frame.connected && !owner.frame.down[IB_GUI_DROP]);
	assert(!owner.frame.released[IB_GUI_CLICK] && !owner.frame.pointer_valid);
	assert(owner.frame.category == INPUT_CAT_CLASSIC_CONTROLLER && owner.frame.glyphs[IB_GUI_CLICK] == 10);
	sample.category = INPUT_CAT_CLASSIC_CONTROLLER;
	input_owner_latch(&owner, &sample);
	assert(owner.frame.connected && !owner.frame.down[IB_GUI_CLICK] && owner.frame.look_x == 0);
	sample.down[IB_GUI_CLICK] = sample.down[IB_GUI_DROP] = false;
	sample.look_x = 0; input_owner_latch(&owner, &sample);
	sample.down[IB_GUI_CLICK] = true; sample.look_x = .7F;
	input_owner_latch(&owner, &sample);
	assert(owner.frame.pressed[IB_GUI_CLICK] && owner.frame.look_x == .7F);
	puts("PASS Classic recovery: one Remote channel, ignored IR, no bare-Remote takeover, neutral button/stick rearm");
}

static void test_device_inventory(void) {
	struct input_inventory inventory = {0};
	struct input_owner owners[2] = {0};
	struct input_device remote = {INPUT_DEVICE_WPAD, 2};
	assert(input_owner_bind(owners, 2, 0, remote));
	struct input_identity identity = {true, INPUT_CAT_NUNCHUK};
	ptime_t now = {0};
	input_inventory_poll(&inventory, remote, identity, true, false, false, now,
						 owners, 2);
	const struct input_device_status* state
		= input_inventory_device(&inventory, remote);
	assert(state && !state->published.connected && !state->armed);
	/* Repeated polls are not reports, even after arbitrarily many frames. */
	for(int k = 0; k < 1000; k++)
		input_inventory_poll(&inventory, remote, identity, true, false, true,
							 time_add_ms(now, 249), owners, 2);
	assert(!state->published.connected && !state->armed);
	input_inventory_poll(&inventory, remote, identity, true, false, true,
						 time_add_ms(now, 1000), owners, 2);
	assert(!state->published.connected && !state->armed);
	input_inventory_poll(&inventory, remote, identity, true, true, true,
						 time_add_ms(now, 1001), owners, 2);
	assert(state->published.connected && state->armed);
	assert(inventory.notices[0].active && !inventory.notices[1].active);
	input_inventory_poll(&inventory, remote, identity, true, true, true,
						 time_add_ms(now, 2000), owners, 2);
	assert(!inventory.notices[1].active);

	struct input_player_frame frame
		= {.connected = state->armed, .category = INPUT_CAT_NUNCHUK};
	input_owner_latch(&owners[0], &frame);
	frame.down[IB_ACTION1] = true;
	input_owner_latch(&owners[0], &frame);
	assert(owners[0].frame.pressed[IB_ACTION1]);
	identity.connected = false;
	input_inventory_poll(&inventory, remote, identity, false, false, false,
						 time_add_ms(now, 2010), owners, 2);
	assert(!state->armed && state->published.connected);
	frame.connected = state->armed;
	input_owner_latch(&owners[0], &frame);
	assert(!owners[0].frame.down[IB_ACTION1]
		   && !owners[0].frame.released[IB_ACTION1]);
	assert(input_device_equal(owners[0].device, remote));
	/* A sub-250ms loss leaves the published identity and assignment intact. */
	identity.connected = true;
	input_inventory_poll(&inventory, remote, identity, true, false, true,
						 time_add_ms(now, 2020), owners, 2);
	assert(!state->armed);
	input_inventory_poll(&inventory, remote, identity, true, true, false,
						 time_add_ms(now, 2021), owners, 2);
	assert(!state->armed);
	input_inventory_poll(&inventory, remote, identity, false, true, true,
						 time_add_ms(now, 2022), owners, 2);
	assert(!state->armed);
	input_inventory_poll(&inventory, remote, identity, true, false, true,
						 time_add_ms(now, 2023), owners, 2);
	assert(!state->armed);
	input_inventory_poll(&inventory, remote, identity, true, true, true,
						 time_add_ms(now, 2024), owners, 2);
	assert(state->armed && !inventory.notices[1].active);
	frame.connected = state->armed;
	frame.down[IB_ACTION1] = false;
	input_owner_latch(&owners[0], &frame);
	frame.down[IB_ACTION1] = true;
	input_owner_latch(&owners[0], &frame);
	assert(owners[0].frame.pressed[IB_ACTION1]);
	puts("PASS inventory: clock-driven stability, retained-poll rejection, "
		 "immediate loss suppression, fresh valid neutral rearm, assignment "
		 "retained");

	identity.category = INPUT_CAT_CLASSIC_CONTROLLER;
	input_inventory_poll(&inventory, remote, identity, true, true, false,
						 time_add_ms(now, 3000), owners, 2);
	assert(!state->armed && state->published.category == INPUT_CAT_NUNCHUK);
	input_inventory_poll(&inventory, remote, identity, true, true, true,
						 time_add_ms(now, 3249), owners, 2);
	assert(state->published.category == INPUT_CAT_NUNCHUK);
	input_inventory_poll(&inventory, remote, identity, true, false, true,
						 time_add_ms(now, 3250), owners, 2);
	assert(state->published.category == INPUT_CAT_CLASSIC_CONTROLLER);
	puts("PASS inventory: a still controller publishes at 250ms from recorded "
		 "fresh identity evidence, without manufacturing a fresh input report");
	assert(inventory.notices[1].player == 0 && inventory.notices[1].assigned);
	assert(input_device_equal(owners[0].device, remote));
	assert(owners[1].device.kind == INPUT_DEVICE_NONE);
	identity.category = INPUT_CAT_WIIMOTE;
	input_inventory_poll(&inventory, remote, identity, true, true, true,
						 time_add_ms(now, 3260), owners, 2);
	assert(!state->armed);
	input_inventory_poll(&inventory, remote, identity, true, true, true,
						 time_add_ms(now, 3510), owners, 2);
	assert(state->published.category == INPUT_CAT_WIIMOTE && !state->armed);
	char text[96];
	/* Approved wording for the required-attachment state (menus.html's
	 * recovery title). One authority: input_notice_text is the card's text. */
	input_notice_text(&inventory.notices[2], text, sizeof(text));
	assert(!strcmp(text, "Attach Nunchuk or Classic"));
	assert(inventory.notices[2].event == CONTROLLER_EVENT_ATTACHMENT_MISSING);
	struct input_owner bare = {.device = remote};
	frame = (struct input_player_frame) {.connected = true,
										 .category = INPUT_CAT_WIIMOTE};
	input_owner_latch(&bare, &frame);
	assert(!bare.frame.connected);
	puts("PASS inventory: Classic swap remains the same player; bare Remote "
		 "publishes needs-Nunchuk and never arms gameplay");
	identity.category = INPUT_CAT_NUNCHUK;
	input_inventory_poll(&inventory, remote, identity, true, true, true,
						 time_add_ms(now, 4000), owners, 2);
	input_inventory_interrupt(&inventory, remote);
	input_inventory_poll(&inventory, remote, identity, true, true, true,
						 time_add_ms(now, 4100), owners, 2);
	input_inventory_poll(&inventory, remote, identity, true, true, true,
						 time_add_ms(now, 4250), owners, 2);
	assert(state->published.category == INPUT_CAT_WIIMOTE);
	input_inventory_poll(&inventory, remote, identity, true, true, true,
						 time_add_ms(now, 4350), owners, 2);
	assert(state->published.category == INPUT_CAT_NUNCHUK);
	puts("PASS inventory: an identity change inside one drained report queue "
		 "restarts the stable-time deadline");
}

static void test_text_mouse_sources(void) {
	JSON_Value* saved = gstate.config_user.root;
	gstate.config_user.root = json_parse_string(
		"{\"input\":{\"gui_click\":[88,1000],"
		"\"gui_click_alt\":[90,1001],\"gui_page\":[1000065]}}");
	assert(gstate.config_user.root);
	all_up(); frame();
	key(88, true); frame();
	assert(input_pressed(IB_GUI_CLICK));
	assert(!input_text_mouse_pressed(IB_GUI_CLICK));
	key(1000, true); frame();
	assert(!input_pressed(IB_GUI_CLICK)); /* keyboard alternative still held */
	assert(input_text_mouse_pressed(IB_GUI_CLICK));
	assert(input_text_mouse_pressed(IB_GUI_CLICK)); /* frame-stable */
	frame();
	assert(!input_text_mouse_pressed(IB_GUI_CLICK));
	key(1000, false); frame();
	key(1000, true); frame();
	assert(input_text_mouse_pressed(IB_GUI_CLICK));
	all_up(); frame();
	key(90, true); frame();
	assert(!input_text_mouse_pressed(IB_GUI_CLICK_ALT));
	key(1001, true); frame();
	assert(input_text_mouse_pressed(IB_GUI_CLICK_ALT));
	/* Keyboard-containing chords belong to typing, not the mouse channel. */
	all_up(); frame();
	key(65, true); key(1000, true); frame();
	assert(!input_text_mouse_pressed(IB_GUI_PAGE));
	assert(input_text_mouse_pressed(IB_GUI_CLICK));
	/* Enabling/re-enabling the queue cannot turn a held mouse into a click. */
	input_text_enable(true); frame();
	assert(!input_text_mouse_pressed(IB_GUI_CLICK));
	input_text_enable(false); input_text_enable(true); frame();
	assert(!input_text_mouse_pressed(IB_GUI_CLICK));
	input_text_enable(false);
	all_up(); frame();
	config_destroy(&gstate.config_user);
	gstate.config_user.root = saved;
	frame();
	puts("PASS text mouse sources: printable alternatives isolated, fresh mouse survives held alias");
}

static void test_primary_look_preferences(void) {
	const char* configs[] = {
		"{}", "{\"controls\":{\"look_sensitivity\":1e100}}",
		"{\"controls\":{\"look_sensitivity\":2.5}}",
		"{\"controls\":{\"look_sensitivity\":\"5\",\"invert_look\":2}}"
	};
	for(size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
		gstate.config_user.root = json_parse_string(configs[i]);
		input_init();
		assert(input_primary_look_sensitivity() == 3);
		assert(!input_primary_look_inverted());
		float x = -0.0F, y = .173F, old_x = x, old_y = y;
		input_primary_look_apply(&x, &y);
		assert(!memcmp(&x, &old_x, sizeof(x)));
		assert(!memcmp(&y, &old_y, sizeof(y)));
		config_destroy(&gstate.config_user);
	}
	gstate.config_user.root = json_parse_string(
		"{\"controls\":{\"look_sensitivity\":5,\"invert_look\":1}}");
	input_init();
	assert(input_primary_look_sensitivity() == 5);
	assert(input_primary_look_inverted());
	fake_cursor_x = 100; fake_cursor_y = 50;
	float x, y;
	assert(input_joystick(.016F, &x, &y));
	assert(fabsf(x - .15F) < .000001F && fabsf(y - .075F) < .000001F);
	assert(input_primary_look_set(1, false));
	fake_cursor_x += 100; fake_cursor_y += 50;
	input_player_look(0, .016F, &x, &y);
	assert(fabsf(x - .05F) < .000001F && fabsf(y + .025F) < .000001F);
	assert(!input_primary_look_set(0, true));
	assert(!input_primary_look_set(6, true));
	assert(input_primary_look_sensitivity() == 1);
	assert(!input_primary_look_inverted());
	/* Shared transform also receives the Wii native stick/IR delta after
	 * its existing deadzone/integration; it must not add a new threshold. */
	for(int level = 1; level <= 5; level++) {
		assert(input_primary_look_set(level, true));
		x = .0001F; y = -.032F;
		input_primary_look_apply(&x, &y);
		float scale = .25F * (level + 1);
		assert(x == .0001F * scale && y == .032F * scale);
	}
	config_destroy(&gstate.config_user);
	fake_cursor_x = fake_cursor_y = 0;
	puts("PASS primary look: safe config, native mouse wrappers, five levels, inversion, default byte identity");
}

int main(void) {
	test_primary_look_preferences();
	test_ir_look_tracking();
	test_classic_attachment_recovery();
	gstate.config_user.root = json_parse_string(BINDINGS);
	assert(gstate.config_user.root);

	input_init();
	all_up();
	frame();

	// (c) baseline: one press edge, a held level, one release edge.
	key(87, true);
	frame();
	assert(pressed(IB_GUI_UP));
	assert(input_held(IB_GUI_UP));
	assert(!released(IB_GUI_UP));

	frame(); // still down -> the edge must NOT repeat
	assert(!pressed(IB_GUI_UP));
	assert(input_held(IB_GUI_UP));

	key(87, false);
	frame();
	assert(!input_held(IB_GUI_UP));
	assert(released(IB_GUI_UP));

	frame(); // release does not repeat either
	assert(!released(IB_GUI_UP));

	// (a) THE BUG: key 87 belongs to two actions. Both must see the press in the same
	// frame, whichever one asks first -- the old per-query native layer gave the edge
	// to the first caller and reported "held" to the second, whose `!any_held` rule
	// then swallowed it.
	all_up();
	frame();
	key(87, true);
	frame();
	assert(pressed(IB_FORWARD));
	assert(pressed(IB_GUI_UP));
	assert(pressed(IB_FORWARD)); // and again, after the sibling consumed nothing
	assert(input_held(IB_FORWARD) && input_held(IB_GUI_UP));

	// (b) player_forward has two sources. It stays down while EITHER is down and
	// releases only when both clear.
	key(200, true); // second source joins while the first is held
	frame();
	assert(input_held(IB_FORWARD));
	assert(!pressed(IB_FORWARD)); // already down: one continuous press, not a new edge
	assert(!released(IB_FORWARD));

	key(87, false); // first source leaves, second still held
	frame();
	assert(input_held(IB_FORWARD));
	assert(!released(IB_FORWARD)); // NOT released: a source is still asking
	assert(released(IB_GUI_UP));   // gui_up had only that source, so it did release

	key(200, false); // now every source is clear
	frame();
	assert(!input_held(IB_FORWARD));
	assert(released(IB_FORWARD));

	// (d) chord: 9008 = hold key 9 + key 8. Its halves are the scroll keys.
	all_up();
	frame();

	key(9, true); // half a chord -> plain binding only
	frame();
	assert(pressed(IB_SCROLL_LEFT));
	assert(!pressed(IB_DEBUG_OVERLAY));

	key(8, true); // chord completes
	frame();
	assert(pressed(IB_DEBUG_OVERLAY));
	assert(input_held(IB_DEBUG_OVERLAY));
	// ...and it MASKS its components: no stray scroll in either direction while held.
	assert(!input_held(IB_SCROLL_LEFT));
	assert(!input_held(IB_SCROLL_RIGHT));
	assert(!pressed(IB_SCROLL_RIGHT));
	assert(released(IB_SCROLL_LEFT)); // the half that was down before the chord formed

	frame(); // chord still down -> no repeat edge
	assert(!pressed(IB_DEBUG_OVERLAY));
	assert(input_held(IB_DEBUG_OVERLAY));

	key(8, false);
	key(9, false);
	frame();
	assert(!input_held(IB_DEBUG_OVERLAY));
	assert(released(IB_DEBUG_OVERLAY)); // input_released used to ignore chords entirely

	// (e) 1000 is the left mouse button, not a chord.
	all_up();
	frame();
	key(1000, true);
	frame();
	assert(pressed(IB_ACTION1));
	assert(input_held(IB_ACTION1));
	key(1000, false);
	frame();
	assert(released(IB_ACTION1));

	// An action with no binding in this config never fires and never crashes.
	assert(!pressed(IB_JUMP) && !input_held(IB_JUMP) && !released(IB_JUMP));

	// ---- text-entry char queue (input_text_enable/input_text_poll) ----------
	// Drive the captured GLFW callbacks exactly like the real window would.
	{
		char c;

		// Disabled (never enabled yet): callbacks not even registered.
		assert(captured_char_cb == NULL && captured_key_cb == NULL);
		assert(!input_text_poll(&c));
		assert(!input_text_active());

		input_text_enable(true);
		assert(captured_char_cb != NULL && captured_key_cb != NULL);
		assert(input_text_active()); // real window (fake non-NULL) + enabled

		// Printables queue FIFO; the 32..126 filter drops the rest.
		captured_char_cb(FAKE_WINDOW, 'h');
		captured_char_cb(FAKE_WINDOW, 'i');
		captured_char_cb(FAKE_WINDOW, 31);   // control: dropped
		captured_char_cb(FAKE_WINDOW, 0x263A); // non-ASCII: dropped
		// Edit keys arrive as 8/9/13 through the key callback (press+repeat).
		captured_key_cb(FAKE_WINDOW, GLFW_KEY_BACKSPACE, 0, GLFW_PRESS, 0);
		captured_key_cb(FAKE_WINDOW, GLFW_KEY_TAB, 0, GLFW_REPEAT, 0);
		captured_key_cb(FAKE_WINDOW, GLFW_KEY_ENTER, 0, GLFW_PRESS, 0);
		captured_key_cb(FAKE_WINDOW, GLFW_KEY_ENTER, 0, GLFW_RELEASE, 0); // ignored
		captured_key_cb(FAKE_WINDOW, GLFW_KEY_ESCAPE, 0, GLFW_PRESS, 0);

		assert(input_text_poll(&c) && c == 'h');
		assert(input_text_poll(&c) && c == 'i');
		assert(input_text_poll(&c) && c == '\b');
		assert(input_text_poll(&c) && c == '\t');
		assert(input_text_poll(&c) && c == '\r');
		assert(input_text_poll(&c) && c == 27); // ESC -> Beta close
		assert(!input_text_poll(&c));

		// While disabled, callbacks drop everything; disable clears leftovers.
		captured_char_cb(FAKE_WINDOW, 'x');
		input_text_enable(false);
		assert(!input_text_active());
		captured_char_cb(FAKE_WINDOW, 'y');
		assert(!input_text_poll(&c));

		// Re-enable: registration is once-only (GLFW would return the
		// previously registered callback; input.c asserts against chaining).
		input_text_enable(true);
		captured_char_cb(FAKE_WINDOW, 'z');
		assert(input_text_poll(&c) && c == 'z');
		input_text_enable(false);
	}

	// ---- edge swallow on leaving a text screen ------------------------------
	// The GLFW pump delivers a queued char one frame BEFORE the polled mask
	// sees the key down, so the ENTER that confirmed a dialog would fire a
	// fresh IB edge on the ARRIVING screen (the rename -> ENTER -> title
	// leak). Leaving text entry swallows every action's next rising edge
	// until its keys are seen released.
	{
		all_up();
		frame();
		input_text_enable(true);
		input_text_enable(false); // the transition arms the swallow

		key(87, true); // the key that "closed the dialog", polled a frame late
		frame();
		assert(!pressed(IB_GUI_UP)); // swallowed: no rising edge
		assert(input_held(IB_GUI_UP)); // honest level state
		frame();
		assert(!pressed(IB_GUI_UP)); // still held, still swallowed

		key(87, false);
		frame(); // released: swallow cleared

		key(87, true);
		frame();
		assert(pressed(IB_GUI_UP)); // re-armed: a genuine new press fires
		key(87, false);
		frame();

		// an action that was UP when armed is unaffected on its next press
		input_text_enable(true);
		input_text_enable(false);
		frame(); // all up: swallow self-clears
		key(87, true);
		frame();
		assert(pressed(IB_GUI_UP));
		key(87, false);
		frame();
	}

	// ---- (f) PC control-hint glyph selection (task_0d54671b) ---------------
	// Through the SAME public input_symbol() gutil_control_icon calls (not
	// the internal input_native_key_symbol directly, which input.h does not
	// expose) -- BINDINGS (still loaded: config_destroy is below) binds
	// "gui_up" to keyboard key 87 and "item_action_left" to mouse button
	// 1000, the same two IB_* actions the edge tests above already use.
	{
		// Mirrors tools/gen_cc0_controls.py's CELL_KEY_KEYBOARD/CELL_KEY_MOUSE
		// (controls.png's generic keyboard-key/mouse-click cells) -- a change
		// to either side without the other fails here, the same drift guard
		// audio_music_table.h's real-file-count test uses.
		const int CELL_KEY_KEYBOARD = 42;
		const int CELL_KEY_MOUSE = 43;

		int symbol, symbol_help;
		enum input_category category;

		// gui_up -> key 87, an ordinary keyboard key: the generic keyboard
		// cell, not controls.png's Guitar Hero fret cell (7) this function
		// used to hard-code for every key.
		// Since the PC glyphs (ctl1031-pc-glyphs) a NAMED key is a PC glyph
		// code that decodes to the approved-controls blank 1u cap (cell 44,
		// "W" drawn on it); controls.png's generic cells 42/43 stay the answer
		// for an unnamed key / a side mouse button.
		assert(input_symbol(IB_GUI_UP, &symbol, &symbol_help, &category));
		assert(symbol == INPUT_GLYPH_PC_KEY_BASE + 87);
		assert(symbol_help == symbol);
		assert(category == INPUT_CAT_NONE);
		{
			int modifier, button;
			assert(input_glyph_cells(symbol, &modifier, &button));
			assert(modifier == -1 && button == INPUT_GLYPH_CELL_KEY_1U);
			assert(button != 7); /* never the Guitar Hero fret cell */
		}
		assert(input_glyph_key(INPUT_CAT_NONE, 161) == CELL_KEY_KEYBOARD);

		// item_action_left -> mouse button 1000 (the 1000+ convention
		// input_native_key_down decodes): the generic mouse cell, distinct
		// from the keyboard one -- proving the selection is not simply
		// another constant hard-coded for every key.
		assert(input_symbol(IB_ACTION1, &symbol, &symbol_help, &category));
		assert(symbol == INPUT_GLYPH_PC_MOUSE_LEFT);
		assert(symbol_help == symbol);
		{
			int modifier, button;
			assert(input_glyph_cells(symbol, &modifier, &button));
			assert(modifier == -1 && button == INPUT_GLYPH_CELL_MOUSE_LEFT);
			assert(input_glyph_cells(input_glyph_key(INPUT_CAT_NONE, 1004),
									 &modifier, &button)
				   && button == CELL_KEY_MOUSE);
		}

		printf("PASS test_input_edges: (f) PC key-glyph atlas mapping (named "
			   "key -> 1u cap cell %d, LMB -> cell %d; unnamed key -> %d, side "
			   "mouse button -> %d; never the Guitar Hero fret cell 7)\n",
			   INPUT_GLYPH_CELL_KEY_1U, INPUT_GLYPH_CELL_MOUSE_LEFT,
			   CELL_KEY_KEYBOARD, CELL_KEY_MOUSE);
	}

	/* A completed tap must reach sibling actions AND the owner/legacy paths. */
	all_up(); frame();
	assert(input_player_bind(0, (struct input_device){INPUT_DEVICE_KEYBOARD, 0}));
	frame();
	fake_taps[87] = true;
	frame();
	assert(input_pressed(IB_FORWARD) && input_pressed(IB_GUI_UP));
	assert(input_player_frame(0)->pressed[IB_GUI_UP]);
	frame();
	assert(!input_held(IB_GUI_UP) && input_released(IB_GUI_UP));
	input_player_unbind(0);
	puts("PASS sticky key tap: shared by sibling actions and owner/legacy menus");
	test_text_mouse_sources();
	test_input_owners();
	test_device_inventory();
	config_destroy(&gstate.config_user);

	// ---- input_stick_normalize (GameCube PAD_StickX/Y normalizer) ----------
	// Pure function, no engine state -- exercised directly, not through a frame.
	{
		float dx, dy, mag;

		#define ASSERT_NEAR(a, b) assert(fabsf((a) - (b)) < 0.001F)

		// Centered stick: zero deflection, direction undefined -> (0, 0).
		input_stick_normalize(0, 0, 100, &dx, &dy, &mag);
		ASSERT_NEAR(mag, 0.0F);
		ASSERT_NEAR(dx, 0.0F);
		ASSERT_NEAR(dy, 0.0F);

		// Full deflection on each axis: unit direction, magnitude 1.
		input_stick_normalize(100, 0, 100, &dx, &dy, &mag);
		ASSERT_NEAR(mag, 1.0F);
		ASSERT_NEAR(dx, 1.0F);
		ASSERT_NEAR(dy, 0.0F);

		input_stick_normalize(-100, 0, 100, &dx, &dy, &mag);
		ASSERT_NEAR(mag, 1.0F);
		ASSERT_NEAR(dx, -1.0F);
		ASSERT_NEAR(dy, 0.0F);

		input_stick_normalize(0, 100, 100, &dx, &dy, &mag);
		ASSERT_NEAR(mag, 1.0F);
		ASSERT_NEAR(dy, 1.0F);

		input_stick_normalize(0, -100, 100, &dx, &dy, &mag);
		ASSERT_NEAR(mag, 1.0F);
		ASSERT_NEAR(dy, -1.0F);

		// Half deflection: magnitude scales proportionally, direction unchanged.
		input_stick_normalize(50, 0, 100, &dx, &dy, &mag);
		ASSERT_NEAR(mag, 0.5F);
		ASSERT_NEAR(dx, 1.0F);
		ASSERT_NEAR(dy, 0.0F);

		// Diagonal past the stick's circular travel: magnitude clamps to 1.0
		// (a real PAD_StickX/Y pair can report a corner past the physical
		// circular gate), direction stays the true 45 degree unit vector.
		input_stick_normalize(100, 100, 100, &dx, &dy, &mag);
		ASSERT_NEAR(mag, 1.0F);
		ASSERT_NEAR(dx, 0.70710678F);
		ASSERT_NEAR(dy, 0.70710678F);

		// A tiny but nonzero reading is NOT snapped to the centered case --
		// only true center (or noise within the internal epsilon) is.
		input_stick_normalize(1, 0, 100, &dx, &dy, &mag);
		ASSERT_NEAR(mag, 0.01F);
		ASSERT_NEAR(dx, 1.0F);

		#undef ASSERT_NEAR

		printf("PASS test_input_edges: input_stick_normalize (GC stick normalizer)\n");
	}

	// (g) b1-chat-opener-0919/14b H-1: the read-only chat-opener accessor, PC
	// half. There is no controller family and no gesture on a keyboard, so it
	// answers the keyboard binding with is_tap FALSE -- for player 0 only,
	// the same rule input_prompt_frame's PC branch already applies. BINDINGS
	// names no "input.chat", so this is the shipped PC fallback (84 = T).
	{
		int code = -12345;
		bool is_tap = true;
		assert(input_player_chat_opener(0, &code, &is_tap));
		assert(code == 84 && "the PC accessor lost Beta's T chat key");
		assert(!is_tap && "a keyboard binding reported the tap gesture");
		code = -12345;
		is_tap = true;
		assert(!input_player_chat_opener(1, &code, &is_tap)
			   && "PC resolved an opener for a player that has no input");

		// An explicit PC binding is authoritative, exactly as everywhere else.
		JSON_Value* saved = gstate.config_user.root;
		gstate.config_user.root = json_parse_string("{\"input\": {\"chat\": [70]}}");
		assert(gstate.config_user.root);
		frame();
		assert(input_player_chat_opener(0, &code, &is_tap));
		assert(code == 70 && !is_tap && "the PC accessor ignored the config");
		json_value_free(gstate.config_user.root);
		gstate.config_user.root = saved;
		frame();
		printf("PASS test_input_edges: chat-opener accessor (PC keyboard "
			   "binding, never a tap, player 0 only)\n");
	}

	/* An armed virtual pad still uses PC keyboard codes on this build. Its
	 * presented family and prompt must agree with the real chat opener. */
	{
		JSON_Value* saved = gstate.config_user.root;
		gstate.config_user.root = json_parse_string("{\"input\": {\"chat\": [89]}}");
		assert(gstate.config_user.root);
		assert(input_player_bind(0, (struct input_device){INPUT_DEVICE_KEYBOARD, 0}));
		fake_profile_pad = true;
		frame();
		struct input_player_frame shown;
		input_prompt_frame(0, &shown);
		assert(shown.connected && shown.category == INPUT_CAT_NONE);
		assert(shown.glyphs[IB_CHAT] == INPUT_GLYPH_PC_KEY_BASE + 89); /* Y cap */
		int code = -1;
		bool tap = true;
		assert(input_player_chat_opener(0, &code, &tap));
		assert(code == 89 && !tap);
		fake_taps[89] = true;
		frame();
		assert(input_chat_open_edge());
		json_value_free(gstate.config_user.root);
		gstate.config_user.root = json_parse_string("{\"input\": {\"chat\": []}}");
		assert(gstate.config_user.root);
		frame();
		input_prompt_frame(0, &shown);
		assert(shown.glyphs[IB_CHAT] == -1);
		assert(!input_player_chat_opener(0, &code, &tap));
		fake_profile_pad = false;
		frame();
		input_prompt_frame(0, &shown);
		assert(shown.category == INPUT_CAT_NONE && shown.glyphs[IB_CHAT] == -1);
		input_player_unbind(0);
		json_value_free(gstate.config_user.root);
		gstate.config_user.root = saved;
		frame();
		puts("PASS test_input_edges: virtual PC pad chat hint matches keyboard opener");
	}

	/* Profile script owns only the pointer query it explicitly answers. A
	 * false predicate and unregister both recover the actual live cursor. */
	{
		float x = -1, y = -1, angle = -1;
		fake_cursor_x = 451; fake_cursor_y = 371;
		input_pointer_enable(true);
		assert(input_pointer(&x, &y, &angle) && x == 451 && y == 371);
		input_pointer_set_script(test_pointer_script);
		script_owns_pointer = true;
		assert(!input_pointer(&x, &y, &angle));
		script_owns_pointer = false;
		assert(input_pointer(&x, &y, &angle) && x == 451 && y == 371);
		input_pointer_set_script(NULL);
		assert(input_pointer(&x, &y, &angle) && x == 451 && y == 371);
		puts("PASS test_input_edges: scripted pointer ownership and native restoration");
	}
	printf("PASS test_input_edges: merged-mask edges, sibling press, chord mask, text queue\n");
	return 0;
}
