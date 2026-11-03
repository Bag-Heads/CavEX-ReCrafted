/*
	Copyright (c) 2022 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>

#include "../cglm/cglm.h"
#include "../game/chat_login_script.h" /* SEAM S1: the scripted chat pad */
#include "../game/game_state.h"
#include "../game/menu_autopilot.h"
#include "gfx.h"
#include "input.h"
#include "service_observer.h"
#include "input_glyph.h"
#include "controller_events.h"
#include "input_inventory.h"
#include "input_owner.h"

static struct input_owner input_owners[INPUT_LOCAL_PLAYERS];
static struct input_inventory device_inventory;
static input_pointer_script_fn pointer_script;

static unsigned long menu_page_poll;
static ptime_t menu_page_now;
static struct menu_page_scope {
	struct input_device device;
	enum input_category category;
	const void* screen;
	const void* window;
	int mode;
	unsigned long poll;
	ptime_t start;
	bool valid, neutral, active, fired;
	struct input_menu_page result;
} menu_page_state[INPUT_LOCAL_PLAYERS];

void input_menu_page_cancel(unsigned player) {
	if(player < INPUT_LOCAL_PLAYERS)
		menu_page_state[player] = (struct menu_page_scope){0};
}

void input_menu_page_read(unsigned player, const struct input_player_frame* frame,
	struct input_menu_page* out) {
	*out = (struct input_menu_page){0};
	if(player >= INPUT_LOCAL_PLAYERS) return;
	struct menu_page_scope* s = &menu_page_state[player];
	if(!frame || !frame->connected) { input_menu_page_cancel(player); return; }
	struct input_device device = {0};
	input_player_device(player, &device);
	const struct client_session* session = &gstate.players[player];
	int window_id = session->visible_window;
	const void* window = session->menu_host.mode != LOCAL_MENU_NONE
		? session->menu_host.window_identity
		: session->windows[window_id >= 0 && window_id < 256 ? window_id : WINDOWC_INVENTORY];
	bool changed = !s->valid || !input_device_equal(s->device, device)
		|| s->category != frame->category || s->screen != session->current_screen
		|| s->mode != (int)session->menu_host.mode
		|| s->window != window
		|| (s->poll != menu_page_poll && s->poll + 1 != menu_page_poll);
	if(changed) {
		*s = (struct menu_page_scope){.valid = true, .device = device,
			.category = frame->category, .screen = session->current_screen,
			.mode = session->menu_host.mode, .window = window};
	} else if(s->poll == menu_page_poll) { *out = s->result; return; }
	s->poll = menu_page_poll;
	s->result = (struct input_menu_page){
		.pressed = frame->pressed[IB_GUI_PAGE_SWAP],
		.held = frame->down[IB_GUI_PAGE_SWAP],
		.released = frame->released[IB_GUI_PAGE_SWAP]};
	if(!s->neutral) {
		s->neutral = !s->result.held;
	} else if(s->result.pressed && s->result.released && !s->active) {
		s->result.page_swap = true;
	} else if(s->result.pressed && s->result.held && !s->active) {
		s->active = true;
		s->fired = false;
		s->start = menu_page_now;
	}
	if(s->active) {
		s->result.held_ms = (unsigned)time_diff_ms(s->start, menu_page_now);
		if(s->result.held && !s->fired
		   && s->result.held_ms >= INPUT_MENU_PAGE_HOLD_MS) {
			s->result.hold = s->fired = true;
		} else if(!s->result.held) {
			s->result.page_swap = s->result.released && !s->fired
				&& s->result.held_ms < INPUT_MENU_PAGE_HOLD_MS;
			s->active = false;
		}
	}
	*out = s->result;
}

void input_pointer_set_script(input_pointer_script_fn script) {
	pointer_script = script;
}

static int primary_look_sensitivity = 3;
static bool primary_look_inverted;

int input_primary_look_sensitivity(void) { return primary_look_sensitivity; }
bool input_primary_look_inverted(void) { return primary_look_inverted; }

bool input_primary_look_set(int sensitivity, bool inverted) {
	if(sensitivity < 1 || sensitivity > 5) return false;
	primary_look_sensitivity = sensitivity;
	primary_look_inverted = inverted;
	return true;
}

void input_primary_look_reload(void) {
	JSON_Object* controls = json_object(gstate.config_user.root);
	int sensitivity = 3;
	bool inverted = false;
	/* Validate before conversion: config_read_int's cast is unsuitable for
	 * unbounded/malformed JSON numbers. Missing keys retain native defaults. */
	if(json_object_dothas_value_of_type(controls, "controls.look_sensitivity",
										  JSONNumber)) {
		double value = json_object_dotget_number(controls, "controls.look_sensitivity");
		if(value >= 1 && value <= 5 && floor(value) == value)
			sensitivity = (int)value;
	}
	if(json_object_dothas_value_of_type(controls, "controls.invert_look", JSONNumber))
		inverted = json_object_dotget_number(controls, "controls.invert_look") == 1;
	(void)input_primary_look_set(sensitivity, inverted);
	int assist = 0; /* Off unless the user opted in */
	if(json_object_dothas_value_of_type(controls, "controls.attack_assist",
										  JSONNumber)) {
		double value = json_object_dotget_number(controls, "controls.attack_assist");
		if(value >= 0 && value <= 2 && floor(value) == value)
			assist = (int)value;
	}
	(void)input_attack_assist_set(assist);
}

static int attack_assist_level;

int input_attack_assist_level(void) { return attack_assist_level; }

bool input_attack_assist_set(int level) {
	if(level < 0 || level > 2) return false;
	attack_assist_level = level;
	return true;
}

void input_primary_look_apply(float* x, float* y) {
	if(!x || !y) return;
	/* Skip arithmetic at the native default, preserving even signed zero. */
	if(primary_look_sensitivity != 3) {
		float scale = .25F * (primary_look_sensitivity + 1);
		*x *= scale;
		*y *= scale;
	}
	if(primary_look_inverted) *y = -*y;
}

const struct input_inventory* input_devices(void) {
	return &device_inventory;
}
#ifdef PLATFORM_WII
static bool input_wpad_configured[4];
static bool input_wpad_fresh[4];
static bool input_wpad_neutral[4];
#include "input_sneak_toggle.h"
#include "input_tap_gesture.h"
/* b1-chat-opener-0919: the chat opener's tap/release gesture, the hardware
 * poll it was last advanced on, and the millisecond origin its tap window is
 * measured from. The poll serial and the epoch are shared with the hotbar
 * gestures below -- both gates are called from the same in-game frame, so a
 * poll they were NOT called on means the same thing for both. */
static struct input_tap_scope input_chat_open_state;
static unsigned long input_chat_poll_serial;
static ptime_t input_chat_epoch;
/* Slice 1 (Astra ruling 3 D2): the Wii Remote's hotbar taps. Index 0 is
 * scroll_left / "previous", 1 is scroll_right / "next"; each one's partner is
 * the other, which is what makes any overlap cancel BOTH.
 *
 * PER LOCAL PLAYER (review 09 finding 1): the split-screen guest holds its own
 * Remote, so the gesture has to run on THAT player's owner frame, device and
 * family. One shared pair read player 0's buttons, which left the guest on the
 * plain press edge the ruling exists to remove -- its Minus stepped a slot
 * before Plus was ever pressed, and a long hold stepped too. */
static struct input_tap_scope input_hotbar_state[INPUT_LOCAL_PLAYERS][2];
/* The order-sensitive F3 chord, per local player, plus the (hold, press) pair
 * the state belongs to so a re-binding or a family change starts over. */
static struct input_chord_order input_chord_state[INPUT_LOCAL_PLAYERS];
static int input_chord_keys[INPUT_LOCAL_PLAYERS][2];
/* Slice 2 (Astra ruling 3 D3): the Wii Remote sneak TOGGLE, one machine per
 * local player, plus the identity its state belongs to (owner, family) and the
 * poll the in-game screen last claimed. */
struct input_sneak_scope {
	struct input_sneak_toggle toggle;
	int kind;
	unsigned channel;
	int category;
	unsigned long gameplay_serial; /* the poll input_sneak_gameplay_tick marked */
	bool dead, resting;            /* that tick's clear levels */
	/* input_sneak_vehicle_mark's RIDING LEVEL: written by the 20 Hz client
	 * tick (both edges) and STICKY across the frame polls in between (review
	 * 09 finding 2). It used to be consumed and zeroed on every poll, so the
	 * level pulsed true/false/false at 60 Hz and the pure machine took a fresh
	 * ENTRY edge on every tick-frame -- which re-cleared the toggle forever
	 * and made sneaking in a boat impossible. */
	bool vehicle;
	bool identified;               /* the identity below has been seen once */
};
static struct input_sneak_scope input_sneak_state[INPUT_LOCAL_PLAYERS];
/* One per controller FAMILY (Astra ergonomics 1 and 3): the deviation is the
 * same, but the reason -- and therefore the opt-out -- belongs to each pad. */
static bool wiimote_sneak_toggle, classic_sneak_toggle, gamecube_sneak_toggle;
#endif
static void input_players_poll(void);

/* Arm the per-action edge swallow (defined with the merged-mask state
 * below): every action whose keys are DOWN when this is called produces
 * no rising edge until its keys are seen released. Both platforms'
 * input_text_enable(false) are the only callers -- see their comments. */
static void input_edges_swallow_held(void);

#ifdef PLATFORM_PC

#include <GLFW/glfw3.h>

extern GLFWwindow* window;

static bool input_pointer_enabled;
static double input_old_pointer_x, input_old_pointer_y;
static bool input_key_sampled[GLFW_KEY_LAST + 1];
static bool input_key_sample[GLFW_KEY_LAST + 1];

void input_init() {
	input_primary_look_reload();
	memset(menu_page_state, 0, sizeof(menu_page_state));
	memset(input_owners, 0, sizeof(input_owners));
	memset(&device_inventory, 0, sizeof(device_inventory));
	input_pointer_enabled = false;
	input_old_pointer_x = 0;
	input_old_pointer_y = 0;
}

void input_platform_poll() {
	SERVICE_OBSERVER(service_observer_poll(true));
	memset(input_key_sampled, 0, sizeof(input_key_sampled));
	SERVICE_OBSERVER(service_observer_receipt(8, window != NULL, window != NULL));
	SERVICE_OBSERVER(service_observer_poll(false));
}

/* Level read only -- the logical layer below owns the edges (see its "one merged
   mask per action" comment). The previous version derived pressed/held/released
   here from a per-key input_key_held[] that it MUTATED on every query, so the
   first consumer of a key in a frame saw "pressed" and every later one saw
   "held" -- which is what made `!any_held` swallow real press edges whenever two
   actions shared a key (the shipped PC config binds 87 to both player_forward
   and gui_up). That array is gone; state now lives in exactly one place. */
bool input_native_key_down(int key) {
	/* SEAM S1, site 2 (mp_chat_login_3m). The scripted profile answers ONLY the
	 * codes bound to the twelve chat actions, and only while armed; everything
	 * else falls straight through to the hardware read below. On PC this is the
	 * merged mask's only source, so it is what makes input_chat_open_edge --
	 * the real opener -- reachable with no human at the keyboard. Disarmed it
	 * returns false on its first statement: byte-identical. */
	{
		bool v;
		if(chat_login_pad_native_key(key, &v))
			return v;
	}
	/* Headless engine/soak tests link the real PC input layer without creating
	 * a GLFW window. No window means no physical input, not a legal GLFW call. */
	if(window == NULL)
		return false;

	// Mouse buttons ride the 1000+ range by existing convention; anything past the
	// GLFW ranges is not a key at all (an out-of-range code used to fall through to
	// glfwGetMouseButton(key-1000) and an OUT-OF-BOUNDS input_key_held[key] write).
	if(key >= 1000)
		return key - 1000 <= GLFW_MOUSE_BUTTON_LAST
			&& glfwGetMouseButton(window, key - 1000) == GLFW_PRESS;

	if(key < 0 || key > GLFW_KEY_LAST)
		return false;
	/* GLFW_STICKY_KEYS consumes a completed tap on its first read. Share
	 * that reading across owner frames, chords and legacy menu actions. */
	if(!input_key_sampled[key]) {
		input_key_sample[key] = glfwGetKey(window, key) == GLFW_PRESS;
		input_key_sampled[key] = true;
	}
	return input_key_sample[key];
}

/* PC control-hint glyph (task_0d54671b, 2026-09-03 root cause): from the
   day this function was first written [fa65056c, 2023-07-09, before
   controls.png's atlas even existed] it returned the SAME constant
   symbol/symbol_help (7) for EVERY key -- once the CC0 atlas rework gave
   index 7 a real meaning (the Guitar Hero fret cell, gen_cc0_controls.py),
   that meant every single PC control hint on every screen (WASD, Space,
   mouse buttons, everything) drew a Guitar Hero fret icon: "the wrong
   glyph" the maintainer flagged, not a per-key mismatch but a total
   absence of PC-side glyph selection. Beta has no keyboard-glyph art of
   its own to port and neither did the original CavEX atlas, so this
   distinguishes only the two PC-native input FAMILIES the mouse-buttons-
   ride-1000+ convention (input_native_key_down, above) already encodes --
   a keyboard key vs. a mouse button -- and points each at the generic
   cell gen_cc0_controls.py reserves for it (CELL_KEY_KEYBOARD=42,
   CELL_KEY_MOUSE=43; test_input_edges.c pins these two indices against
   that generator constant).
   PC glyphs (ctl1031-pc-glyphs, 10-31): a named key / mouse button now
   answers its PC glyph CODE (input_glyph_key(INPUT_CAT_NONE), a blank cap
   lettered with the key name, or a mouse picture); a code outside the GLFW
   ranges keeps the generic 42/43 cell. */
bool input_native_key_symbol(int key, int* symbol, int* symbol_help,
							 enum input_category* category, int* priority) {
	*category = INPUT_CAT_NONE;
	int code = input_glyph_key(INPUT_CAT_NONE, key);
	*symbol = *symbol_help = code >= 0 ? code : (key >= 1000) ? 43 : 42;
	*priority = 1;
	return true;
}

bool input_native_key_any(int* key) {
	return false;
}

void input_pointer_enable(bool enable) {
	// Headless integration tests exercise screen transitions without creating a
	// GLFW window. Keep the logical pointer mode in sync, but do not hand GLFW
	// its forbidden NULL handle. The game path is unchanged once gfx owns a
	// window.
	if(window == NULL) {
		input_pointer_enabled = enable;
		return;
	}

	glfwSetInputMode(window, GLFW_CURSOR,
					 enable ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);

	if(!input_pointer_enabled && enable)
		glfwSetCursorPos(window, gfx_width() / 2, gfx_height() / 2);

	if(input_pointer_enabled && !enable)
		glfwGetCursorPos(window, &input_old_pointer_x, &input_old_pointer_y);

	input_pointer_enabled = enable;
}

bool input_pointer(float* x, float* y, float* angle) {
	bool script_available;
	if(pointer_script && pointer_script(x, y, angle, &script_available))
		return script_available;
	// menus_dwell hardware profile: owns the ENTIRE scripted window (not only
	// the one tick it asserts a position), so no real cursor read below can
	// leak through and land an unintended click. See menu_autopilot.h.
	{
		bool available;
		if(menu_autopilot_force_pointer(x, y, angle, &available))
			return available;
	}

	if(window == NULL) {
		*x = 0.0F;
		*y = 0.0F;
		*angle = 0.0F;
		return false;
	}

	double x2, y2;
	glfwGetCursorPos(window, &x2, &y2);
	*x = x2;
	*y = y2;
	*angle = 0.0F;
	return input_pointer_enabled && x2 >= 0 && y2 >= 0 && x2 < gfx_width()
		&& y2 < gfx_height();
}

void input_native_joystick(float dt, float* dx, float* dy) {
	if(window == NULL) {
		*dx = 0.0F;
		*dy = 0.0F;
		return;
	}

	if(!input_pointer_enabled) {
		double x2, y2;
		glfwGetCursorPos(window, &x2, &y2);
		*dx = (x2 - input_old_pointer_x) * 0.001F;
		*dy = -(y2 - input_old_pointer_y) * 0.001F;
		input_old_pointer_x = x2;
		input_old_pointer_y = y2;
	} else {
		*dx = 0.0F;
		*dy = 0.0F;
	}
}

/* ---- Text-entry char queue (PC: the faithful Beta typing path) ----------
   GLFW delivers printables through the char callback and the edit keys
   through the key callback; both push into one small ring. Enabled only
   while a text screen is open, so gameplay keys never queue up. Callbacks
   are registered lazily on the first enable that has a window (headless
   tests enable/poll with window == NULL and just see an empty queue). */
#define INPUT_TEXT_QUEUE_CAP 32

static bool input_text_enabled;
static bool input_text_callbacks_registered;
static char input_text_queue[INPUT_TEXT_QUEUE_CAP];
static size_t input_text_head, input_text_tail;

static void input_text_push(char c) {
	size_t next = (input_text_tail + 1) % INPUT_TEXT_QUEUE_CAP;
	if(next == input_text_head)
		return; /* full: drop (a 32-deep backlog means nobody is reading) */
	input_text_queue[input_text_tail] = c;
	input_text_tail = next;
}

static void input_text_char_callback(GLFWwindow* w, unsigned int codepoint) {
	(void)w;
	if(input_text_enabled && codepoint >= 32 && codepoint <= 126)
		input_text_push((char)codepoint);
}

static void input_text_key_callback(GLFWwindow* w, int key, int scancode,
									int action, int mods) {
	(void)w;
	(void)scancode;
	(void)mods;
	if(!input_text_enabled
	   || (action != GLFW_PRESS && action != GLFW_REPEAT))
		return;
	if(key == GLFW_KEY_BACKSPACE)
		input_text_push('\b');
	else if(key == GLFW_KEY_TAB)
		input_text_push('\t');
	else if(key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)
		input_text_push('\r');
	else if(key == GLFW_KEY_ESCAPE)
		input_text_push(27); /* Beta GuiScreen keyTyped key 1 = close */
}

void input_text_enable(bool enable) {
	if(enable && !input_text_callbacks_registered && window != NULL) {
		/* Nothing else registers these callbacks (the rest of the input
		 * layer polls); still, GLFW returns the previous one -- assert the
		 * assumption instead of silently chaining. */
		GLFWcharfun prev_char = glfwSetCharCallback(window,
													input_text_char_callback);
		GLFWkeyfun prev_key = glfwSetKeyCallback(window,
												 input_text_key_callback);
		assert(prev_char == NULL && prev_key == NULL);
		(void)prev_char;
		(void)prev_key;
		input_text_callbacks_registered = true;
	}

	/* Leaving a text screen: keys the screen consumed through the queue
	 * must be RELEASED before they can drive IB actions again. The GLFW
	 * event pump delivers a queued char one frame BEFORE the polled mask
	 * sees the same key go down, so the ENTER that pressed a dialog's
	 * confirm would otherwise fire IB_HOME as a fresh rising edge on the
	 * ARRIVING screen (observed: rename -> ENTER -> title). */
	if(input_text_enabled && !enable)
		input_edges_swallow_held();

	input_text_enabled = enable;

	/* Audit finding 4: drain on BOTH edges, not just disable. A missed
	 * disable on some exit path (or ordinary gameplay keys typed while
	 * capture was briefly left on) can leave stale characters sitting in
	 * the 32-deep ring; draining only on disable lets them survive into
	 * the NEXT screen's enable and spill into that OSK field as if freshly
	 * typed. Draining on enable too makes every field start from a
	 * genuinely empty queue regardless of what happened before. */
	input_text_head = input_text_tail = 0;
}

bool input_text_poll(char* out) {
	assert(out);
	if(input_text_head == input_text_tail)
		return false;
	*out = input_text_queue[input_text_head];
	input_text_head = (input_text_head + 1) % INPUT_TEXT_QUEUE_CAP;
	return true;
}

bool input_text_active(void) {
	/* A real keyboard owns the screen only when the callbacks are actually
	 * wired to a window; headless tests (window == NULL) keep the IB path. */
	return input_text_enabled && input_text_callbacks_registered;
}

#endif

#ifdef PLATFORM_WII

#include <ogc/pad.h>
#include <wiiuse/wpad.h>

static bool input_wpad_report_neutral(const WPADData* data);

// joystick_input/js_emulated_btns_held index convention: 0=Nunchuk stick,
// 1=Classic left stick, 2=Classic right stick, 3=GameCube control stick,
// 4=GameCube C-stick. Every index feeds the SAME emulated-digital-dpad loop
// in input_platform_poll (js_emulated_btns_held[j][0..3] = up/down/left/right
// at code 900 + j*10 + offset -- see input_native_key_down/input_native_key_symbol).
static struct {
	float dx, dy;
	float magnitude;
	bool available;
} joystick_input[5];

static bool js_emulated_btns_held[5][4];

// GameCube pad liveness for chan 0, refreshed once per frame by
// input_platform_poll's PAD_ScanPads() connected-channel bitmask. Mirrors
// js_expansion_type's role for Nunchuk/Classic: input_native_key_symbol reads
// this to boost a GC glyph's priority only while a GC pad is actually the
// attached/active controller, exactly like the existing Nunchuk/Classic
// active-boost. SPLIT-SEAM: this is a single chan-0 boolean today (one shared
// input mask, one active controller). A future per-player split (P1 Wiimote /
// P2 GC) would key this -- and the whole GC read below -- by PAD channel
// instead of always chan 0; kept as its own distinct read path (not folded
// into js_expansion_type or blended into the WPAD mask) so that routing can
// be added later without re-plumbing this file.
static bool gc_pad_connected;
/* Connected GC channels in the PAD_CHAN0_BIT >> channel convention every
 * reader below uses (and PAD_Reset takes). NOT the raw PAD_ScanPads return:
 * see input_pad_scan_mask. */
static uint32_t pad_connected_mask;
/* Channels whose PAD_Read answered PAD_ERR_NONE on THIS scan (same bit
 * convention). Only these carry a new ButtonsDown edge: on a scan where a
 * present pad answered PAD_ERR_TRANSFER/NOT_READY, libogc leaves its
 * __pad_keys untouched, so PAD_ButtonsDown would repeat last scan's edge. */
static uint32_t pad_fresh_mask;

/* GameCube input lag/drops (maintainer, 7fd0d1b real Wii): libogc's
 * PAD_ScanPads (libogc/pad.c:738) sets bit i ONLY for PAD_ERR_NONE. A present,
 * enabled pad also answers PAD_ERR_TRANSFER whenever SI has not latched a new
 * poll response since the previous read (pad.c:570-572 via si.c:536
 * SI_GetResponse's consumed inputBufferValid) or the channel is busy
 * (pad.c:537) -- i.e. any frame that lands inside one SI poll period, and
 * around the SI traffic of a re-probe. ScanPads keeps that pad's last keys
 * (its `default:` case), but this file read the bit as "unplugged": the
 * device inventory saw an identity edge and DISARMED the pad
 * (input_inventory_poll), the owner frame went disconnected
 * (frame.connected &= armed), and it stayed dead until every button and
 * stick was seen neutral again -- dropped input / lag while playing.
 * A channel therefore stays present across a short run of non-NONE scans:
 * it is dropped only after INPUT_PAD_DROPOUT_SCANS consecutive misses
 * spanning at least INPUT_PAD_DROPOUT_MS. A real unplug reads as
 * PAD_ERR_NO_CONTROLLER, whose keys libogc zeroes at once (neutral, never
 * stale), and is reported as disconnected after that short hold. */
#define INPUT_PAD_DROPOUT_MS 200
#define INPUT_PAD_DROPOUT_SCANS 2
static struct {
	unsigned misses[4];
	ptime_t first_miss[4];
} pad_presence;

static uint32_t input_pad_presence(uint32_t present, uint32_t fresh,
								   ptime_t now) {
	for(unsigned channel = 0; channel < 4; channel++) {
		uint32_t bit = PAD_CHAN0_BIT >> channel;
		if(fresh & bit) {
			pad_presence.misses[channel] = 0;
			present |= bit;
			continue;
		}
		if(!(present & bit))
			continue;
		if(pad_presence.misses[channel]++ == 0)
			pad_presence.first_miss[channel] = now;
		if(pad_presence.misses[channel] >= INPUT_PAD_DROPOUT_SCANS
		   && time_diff_ms(pad_presence.first_miss[channel], now)
			   >= INPUT_PAD_DROPOUT_MS) {
			present &= ~bit;
			pad_presence.misses[channel] = 0;
		}
	}
	return present;
}

static uint16_t input_pad_buttons_down(unsigned channel) {
	if(channel >= 4 || !(pad_fresh_mask & (PAD_CHAN0_BIT >> channel)))
		return 0;
	return PAD_ButtonsDown(channel);
}

/* GameCube hot-plug (maintainer bug on 41e5752: a pad plugged in after boot
 * was never seen).
 *
 * 1. Mask convention. libogc's PAD_ScanPads (libogc/pad.c, the sidecar
 *    C:/tmp/logc256kr and stock alike) builds its return as
 *    `connected |= (1<<i)` -- channel 0 is bit 0 -- while PAD_Reset and
 *    PAD_CHAN0_BIT use 0x80000000 >> channel. Every reader here tested
 *    PAD_CHAN0_BIT >> channel against the raw return, so no GC channel ever
 *    read as connected: no P1 bind, no takeover, no device-inventory
 *    connect/reconnect, no sticks. Converted once here instead.
 * 2. Re-probe. libogc's own PAD_ScanPads already PAD_Reset()s every channel
 *    that PAD_Read reports PAD_ERR_NO_CONTROLLER, which is how the driver
 *    finds a newly plugged pad. As a bounded backstop (a channel can also sit
 *    in NOT_READY/TRANSFER, which ScanPads never re-probes), a channel absent
 *    from the scan for INPUT_PAD_REPROBE_MS straight is PAD_Reset by mask at
 *    most once per INPUT_PAD_REPROBE_MS. A channel present in the scan is
 *    never in the mask, so a live pad is never reset mid-play; PAD_Reset
 *    itself also skips channels libogc is still waiting/checking on. */
#define INPUT_PAD_REPROBE_MS 500
static struct {
	bool absent[4];
	ptime_t absent_since[4];
	bool reset_issued;
	ptime_t last_reset;
} pad_hotplug;

static uint32_t input_pad_scan_mask(uint32_t scanned) {
	uint32_t mask = 0;
	for(unsigned channel = 0; channel < 4; channel++) {
		if(scanned & (1u << channel))
			mask |= PAD_CHAN0_BIT >> channel;
	}
	return mask;
}

static void input_pad_hotplug(uint32_t connected, ptime_t now) {
	uint32_t missing = 0;
	for(unsigned channel = 0; channel < 4; channel++) {
		uint32_t bit = PAD_CHAN0_BIT >> channel;
		if(connected & bit) {
			pad_hotplug.absent[channel] = false;
		} else if(!pad_hotplug.absent[channel]) {
			pad_hotplug.absent[channel] = true;
			pad_hotplug.absent_since[channel] = now;
		} else if(time_diff_ms(pad_hotplug.absent_since[channel], now)
				  >= INPUT_PAD_REPROBE_MS) {
			missing |= bit;
		}
	}
	if(!missing)
		return;
	if(pad_hotplug.reset_issued
	   && time_diff_ms(pad_hotplug.last_reset, now) < INPUT_PAD_REPROBE_MS)
		return;
	PAD_Reset(missing);
	pad_hotplug.reset_issued = true;
	pad_hotplug.last_reset = now;
}

/* Expansion type as of the last poll. WPAD_Expansion copies the whole
   expansion_t (stick + accel + buttons) out of the driver; the key lookups below
   need only its `type`, and they used to pay that copy ~6 times per input query.
   Cached once per frame instead -- same answer, one call. */
static uint32_t js_expansion_type = WPAD_EXP_NONE;
static bool wiimote_ir_look;
static struct input_ir_look_state ir_look;
/* Experimental local split-screen: one IR-look integrator per NON-primary
 * player. Player 0 keeps `ir_look` above untouched, so its behaviour is
 * byte-identical to before this array existed. */
static struct input_ir_look_state owned_ir_look[INPUT_LOCAL_PLAYERS];

/* Controller-playability adaptation: hold the primary MENU cursor inside a
 * sub-pixel radius of its last emitted position. Larger moves are copied in
 * the same query; there is no averaging or time-based lag. Tune in screen
 * pixels; zero disables the deadzone. Raw owner frames and IR look stay raw.
 * Local split menus that read input_owner_pointer directly are unaffected. */
static const float menu_ir_deadzone_pixels = 0.75F;
static bool menu_ir_enabled;
static struct {
	bool valid;
	float x, y;
	int width, height;
	struct input_device device;
} menu_ir;

static bool input_menu_ir_filter(bool valid, float* x, float* y, float angle) {
	int width = gfx_width(), height = gfx_height();
	struct input_device device = input_owners[0].device;
	if(!input_device_equal(device, menu_ir.device)
	   || width != menu_ir.width || height != menu_ir.height)
		menu_ir.valid = false;
	menu_ir.device = device;
	menu_ir.width = width; menu_ir.height = height;
	if(!valid || !isfinite(*x) || !isfinite(*y) || !isfinite(angle)) {
		menu_ir.valid = false;
		return false;
	}
	float dx = *x - menu_ir.x, dy = *y - menu_ir.y;
	if(menu_ir.valid && menu_ir_deadzone_pixels > 0 && dx * dx + dy * dy
		   <= menu_ir_deadzone_pixels * menu_ir_deadzone_pixels) {
		*x = menu_ir.x; *y = menu_ir.y;
	} else {
		menu_ir.x = *x; menu_ir.y = *y;
		menu_ir.valid = true;
	}
	return true;
}

void input_init() {
	input_primary_look_reload();
	memset(menu_page_state, 0, sizeof(menu_page_state));
	memset(&menu_ir, 0, sizeof(menu_ir));
	menu_ir_enabled = false;
	memset(input_owners, 0, sizeof(input_owners));
	memset(&device_inventory, 0, sizeof(device_inventory));
	memset(input_wpad_configured, 0, sizeof(input_wpad_configured));
	WPAD_Init();
	WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
	WPAD_SetVRes(WPAD_CHAN_0, gfx_width(), gfx_height());
	/* New shipped bindings use Nunchuk movement. Existing configurations
	 * retain stick-look unless they explicitly enable this matching mode. */
	wiimote_ir_look = config_read_int(&gstate.config_user,
		"controls.wiimote_ir_look", 0) != 0;
	/* Astra ruling 3 D3: ABSENT means TOGGLE on the Remote family -- the
	 * shipped scheme's behaviour is also the fallback -- and an explicit 0 is
	 * the faithful HOLD.
	 *
	 * Review 09 finding 5: BOTH `controls.*` modes above are read exactly
	 * ONCE, HERE, at BOOT -- main.c calls input_init() from its start-up path
	 * and nothing calls it again, and no code path rewrites the config while
	 * the game runs. So there is no such thing as a run-time scheme-mode
	 * change to clear; the memset below is simply what "a fresh input layer
	 * owns nothing" means at boot. (Astra D3's scheme-mode clear is therefore
	 * satisfied vacuously, not by a mechanism -- reported to Astra.) */
	wiimote_sneak_toggle = config_read_int(&gstate.config_user,
		"controls.wiimote_sneak_toggle", INPUT_SNEAK_TOGGLE_DEFAULT) != 0;
	/* Astra ergonomics study 2026-09-19 recommendations 1 and 3, accepted by
	 * the maintainer ("All but 5, and we workshop 5 later"): the OTHER two
	 * families get the same deviation, each behind its own key so a player can
	 * opt any one of them back to Beta's hold on its own. Same rule as the
	 * Remote's: ABSENT means toggle. */
	classic_sneak_toggle = config_read_int(&gstate.config_user,
		"controls.classic_sneak_toggle", INPUT_SNEAK_TOGGLE_DEFAULT) != 0;
	gamecube_sneak_toggle = config_read_int(&gstate.config_user,
		"controls.gamecube_sneak_toggle", INPUT_SNEAK_TOGGLE_DEFAULT) != 0;
	memset(input_sneak_state, 0, sizeof(input_sneak_state));
	ir_look = (struct input_ir_look_state){0};
	/* A fresh input layer owns no gesture: whatever is held right now has to
	 * be seen neutral before the chat opener can arm (input_tap_gesture.h). */
	input_chat_open_state = (struct input_tap_scope){0};
	memset(input_hotbar_state, 0, sizeof(input_hotbar_state));
	memset(input_chord_state, 0, sizeof(input_chord_state));
	memset(input_chord_keys, 0, sizeof(input_chord_keys));
	input_chat_poll_serial = 0;
	input_chat_epoch = time_get();

	// GameCube pad: PAD_Init is harmless with nothing plugged into any SI port
	// (it just arms polling), same "safe to call unconditionally" contract as
	// WPAD_Init above. No PAD_Shutdown exists in libogc (SI/PAD holds no BT
	// resource like WPAD does, so main.c's WDM_SHUTDOWN_WPAD teardown has no
	// GC counterpart to add).
	PAD_Init();
	gc_pad_connected = false;
	pad_connected_mask = 0;
	pad_fresh_mask = 0;
	memset(&pad_presence, 0, sizeof(pad_presence));
	memset(&pad_hotplug, 0, sizeof(pad_hotplug));

	js_expansion_type = WPAD_EXP_NONE;

	for(int k = 0; k < 4; k++) {
		for(int j = 0; j < 5; j++)
			js_emulated_btns_held[j][k] = false;
	}
}

// Wiimote/BT liveness discriminator (wiimote-dead-direct-launch 2026-08-18):
// raw WPAD_Probe(chan 0) return code for the telemetry heartbeat. WPAD's
// enable path is async and app-invisible on failure (WPAD_Init -> ENABLING;
// only the BT core + stored-link-key callbacks flip it to ENABLED), so a
// stack stuck mid-enable answers WPAD_ERR_NOT_READY forever while a healthy
// idle stack answers WPAD_ERR_NO_CONTROLLER -- distinguishable WITHOUT a
// human pressing anything at the console.
int input_wpad_probe0(void) {
	u32 type = 0;
	return WPAD_Probe(WPAD_CHAN_0, &type);
}

// HBC-direct attached-but-starved candidate fix (2026-08-23 field session):
// on real hardware launched directly from HBC, WPAD_Probe(0) reports a
// controller ATTACHED (0) yet no button/IR data ever arrives -- consistent
// with the per-attach "set data reporting mode" command to the remote being
// lost when this app's fresh BT stack takes over from HBC's still-live
// session. WPAD applies a channel's data format at attach time; re-issuing
// it ON the attach transition forces the reporting-mode command down the
// now-settled link. One-shot per attach (re-armed on detach) -- harmless on
// healthy paths (chainload/Dolphin), where it just re-states the format.
static bool wpad_kicked;
static void wpad_kick_on_attach(void) {
	u32 type = 0;
	if(WPAD_Probe(WPAD_CHAN_0, &type) == WPAD_ERR_NONE) {
		if(!wpad_kicked) {
			wpad_kicked = true;
			WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
			WPAD_SetVRes(WPAD_CHAN_0, gfx_width(), gfx_height());
		}
	} else
		wpad_kicked = false;
}

// Raw held-button mask for the on-screen overlay (diagnosis without any
// network): if this stays 0x0 while a human mashes buttons and probe0 == 0,
// the data pipe below WPAD is silent and the kick above did not take.
unsigned int input_wpad_raw_held0(void) {
	return WPAD_ButtonsHeld(WPAD_CHAN_0);
}

// lwp kernel workspace headroom (the 2026-08-23 prime suspect): wiiuse
// allocates its per-remote cmd queues from THIS pool AT ATTACH TIME, and a
// failed alloc there is silent by design (PR#266 dossier). If this reads ~0
// while the stage server (which accepts remotes fine) has plenty, the
// never-accepts-a-Wiimote defect is confirmed as workspace exhaustion.
#include <ogc/lwp_heap.h>
// ogc/lwp_wkspace.h is NOT self-contained from app include paths (its
// internal #include <lwp_heap.h> lacks the ogc/ prefix and only resolves
// inside libogc's own build) -- declare the workspace handle directly.
extern heap_cntrl __wkspace_heap;
// Full WPAD/BT teardown for the loader return -- see main.c's
// WDM_SHUTDOWN_WPAD site for the exit-wedge rationale.
void input_wpad_shutdown0(void) {
	WPAD_Shutdown();
}

unsigned int input_lwp_wkspace_free(void) {
	heap_iblock info;
	if(__lwp_heap_getinfo(&__wkspace_heap, &info) == 0)
		return info.free_size;
	return 0xFFFFFFFFu; // getinfo failed (distinct from genuine 0)
}

// Full pool picture: the 128KB round measured free growing only +10,192 of
// the +65,536 added, meaning boot-time used_size GREW ~55KB when room
// existed -- i.e. the 64KB pool was starving more allocs than the Wiimote
// queue alone. total/used/free_blocks settle who ate it and whether the
// free bytes are contiguous enough for the 18,432B queue.
extern u32 __lwp_wkspace_heapsize(void);
void input_lwp_wkspace_stats(unsigned int* total, unsigned int* used,
							 unsigned int* free_blocks) {
	heap_iblock info;
	*total = __lwp_wkspace_heapsize();
	if(__lwp_heap_getinfo(&__wkspace_heap, &info) == 0) {
		*used = info.used_size;
		*free_blocks = info.free_blocks;
	} else {
		*used = 0xFFFFFFFFu;
		*free_blocks = 0xFFFFFFFFu;
	}
}

// Deeper overlay diagnostics for the attached-but-starved hunt: the raw
// WPADData for chan 0. err (s16: 0=NONE, -1=NO_CONTROLLER, -2=NOT_READY,
// -3=TRANSFER) says whether the last event slot holds valid data;
// data_present is the per-source bitmask (buttons/accel/ir); battery_level
// only ever becomes nonzero after at least ONE status report has arrived
// from the remote -- a zero battery with err==0 means NO report has EVER
// been delivered, pinning the silence below wiiuse's event layer.
void input_wpad_raw_diag0(int* err, unsigned int* data_present,
						  unsigned int* battery) {
	WPADData* d = WPAD_Data(WPAD_CHAN_0);
	if(d) {
		*err = d->err;
		*data_present = d->data_present;
		*battery = d->battery_level;
	} else {
		*err = -99;
		*data_present = 0;
		*battery = 0;
	}
}

static void input_wpad_report(s32 channel, const WPADData* data) {
	if(channel < 0 || channel >= 4 || !data)
		return;
	/* ReadPending invokes this during MAIN's poll drain, after the driver
	 * queued the report. Observer v2 labels this boundary poll_drain; it does
	 * not measure Bluetooth/SI arrival or physical button actuation. */
	SERVICE_OBSERVER(service_observer_receipt((unsigned)channel, data->err == WPAD_ERR_NONE, data->err == WPAD_ERR_NONE && (data->data_present & WPAD_DATA_BUTTONS) && ((data->exp.type != WPAD_EXP_NUNCHUK && data->exp.type != WPAD_EXP_CLASSIC) || (data->data_present & WPAD_DATA_EXPANSION))));
	enum input_category category = data->exp.type == WPAD_EXP_CLASSIC ?
		INPUT_CAT_CLASSIC_CONTROLLER :
		data->exp.type == WPAD_EXP_NUNCHUK ? INPUT_CAT_NUNCHUK :
		data->exp.type == WPAD_EXP_NONE	   ? INPUT_CAT_WIIMOTE :
											 INPUT_CAT_NONE;
	const struct input_device_status* status
		= &device_inventory.devices[channel];
	if(data->err != WPAD_ERR_NONE || category != status->observed.category)
		input_inventory_interrupt(
			&device_inventory,
			(struct input_device) {INPUT_DEVICE_WPAD, channel});
	input_wpad_fresh[channel] = data->err == WPAD_ERR_NONE
		&& (data->data_present & WPAD_DATA_BUTTONS)
		&& ((data->exp.type != WPAD_EXP_NUNCHUK
			 && data->exp.type != WPAD_EXP_CLASSIC)
			|| (data->data_present & WPAD_DATA_EXPANSION));
	input_wpad_neutral[channel]
		= input_wpad_fresh[channel] && input_wpad_report_neutral(data);
}

void input_platform_poll() {
	SERVICE_OBSERVER(service_observer_poll(true));
	memset(input_wpad_fresh, 0, sizeof(input_wpad_fresh));
	/* WPAD_ScanPads is ReadPending(ALL, NULL). The callback only records
	 * freshness; it leaves the driver's coalesced button state unchanged. */
	WPAD_ReadPending(WPAD_CHAN_ALL, input_wpad_report);
	wpad_kick_on_attach();

	// Zero-init: WPAD_Expansion leaves `e` UNTOUCHED if the controller is absent or
	// mid-(re)connect (e.g. Dolphin re-initialising the Wii Remote on a controller-config
	// change, or a real Wiimote sleeping/resyncing) -> an uninitialised e.type read random
	// garbage that could match WPAD_EXP_NUNCHUK/CLASSIC and inject junk stick/button input.
	// {0} == WPAD_EXP_NONE -> the type checks below safely fall through to "no extension".
	expansion_t e = {0};
	WPAD_Expansion(WPAD_CHAN_0, &e);
	js_expansion_type = e.type;

	if(e.type == WPAD_EXP_NUNCHUK) {
		joystick_input[0].dx = sin(glm_rad(e.nunchuk.js.ang));
		joystick_input[0].dy = cos(glm_rad(e.nunchuk.js.ang));
		joystick_input[0].magnitude = e.nunchuk.js.mag;
		joystick_input[0].available = true;
	} else {
		joystick_input[0].available = false;
	}

	if(e.type == WPAD_EXP_CLASSIC) {
		joystick_input[1].dx = sin(glm_rad(e.classic.ljs.ang));
		joystick_input[1].dy = cos(glm_rad(e.classic.ljs.ang));
		joystick_input[1].magnitude = e.classic.ljs.mag;
		joystick_input[1].available = true;

		joystick_input[2].dx = sin(glm_rad(e.classic.rjs.ang));
		joystick_input[2].dy = cos(glm_rad(e.classic.rjs.ang));
		joystick_input[2].magnitude = e.classic.rjs.mag;
		joystick_input[2].available = true;
	} else {
		joystick_input[1].available = joystick_input[2].available = false;
	}

	// GameCube pad: a wholly separate subsystem from WPAD (SI polling, not
	// Bluetooth), so it gets its own scan + connected-bitmask read rather than
	// riding WPAD_ScanPads/js_expansion_type -- see the split-seam comment on
	// gc_pad_connected above. PAD_ScanPads returns which of the 4 SI channels
	// answered THIS scan (bit i = channel i; converted by input_pad_scan_mask);
	// chan 0 only for the legacy stick read, matching WPAD_CHAN_0's
	// single-controller scope; input_devices_poll covers all four.
	// A present pad that answered PAD_ERR_TRANSFER this scan stays present
	// with its last state (input_pad_presence); the re-probe below only ever
	// targets channels that are absent by that same hysteresis.
	{
		ptime_t pad_now = time_get();
		pad_fresh_mask = input_pad_scan_mask(PAD_ScanPads());
		pad_connected_mask
			= input_pad_presence(pad_connected_mask, pad_fresh_mask, pad_now);
		input_pad_hotplug(pad_connected_mask, pad_now);
	}
	SERVICE_OBSERVER(for(unsigned so_channel = 0; so_channel < 4; so_channel++) service_observer_receipt(4+so_channel, (pad_connected_mask & (PAD_CHAN0_BIT >> so_channel)) != 0, (pad_fresh_mask & (PAD_CHAN0_BIT >> so_channel)) != 0));
	gc_pad_connected = (pad_connected_mask & PAD_CHAN0_BIT) != 0;

	if(gc_pad_connected) {
		// PAD_StickX/Y and PAD_SubStickX/Y are signed cartesian (s8), unlike
		// WPAD's polar nunchuk/classic reports above -- input_stick_normalize
		// (input.h, platform-independent, unit-tested in tests/test_input_edges.c)
		// converts to the same (dx, dy, magnitude) shape the rest of this
		// table uses. GC_STICK_RANGE mirrors the
		// devkitPro PAD convention of treating the full s8 span as the travel
		// range; the shared 0.2F deadzone below (same threshold Nunchuk/
		// Classic already use) absorbs any centering noise.
		const int GC_STICK_RANGE = 128;
		input_stick_normalize(PAD_StickX(PAD_CHAN0), PAD_StickY(PAD_CHAN0),
							  GC_STICK_RANGE, &joystick_input[3].dx,
							  &joystick_input[3].dy,
							  &joystick_input[3].magnitude);
		joystick_input[3].available = true;

		input_stick_normalize(PAD_SubStickX(PAD_CHAN0),
							  PAD_SubStickY(PAD_CHAN0), GC_STICK_RANGE,
							  &joystick_input[4].dx, &joystick_input[4].dy,
							  &joystick_input[4].magnitude);
		joystick_input[4].available = true;
	} else {
		joystick_input[3].available = joystick_input[4].available = false;
	}

	for(int j = 0; j < 5; j++) {
		/* Level state only: the previous frame's copy used to live here purely to
		   derive edges, which the logical layer's merged mask now owns. */
		for(int k = 0; k < 4; k++)
			js_emulated_btns_held[j][k] = false;

		if(joystick_input[j].available) {
			float x = joystick_input[j].dx * joystick_input[j].magnitude;
			float y = joystick_input[j].dy * joystick_input[j].magnitude;

			if(x > 0.2F) {
				js_emulated_btns_held[j][3] = true;
			} else if(x < -0.2F) {
				js_emulated_btns_held[j][2] = true;
			}

			if(y > 0.2F) {
				js_emulated_btns_held[j][0] = true;
			} else if(y < -0.2F) {
				js_emulated_btns_held[j][1] = true;
			}
		}
	}
	SERVICE_OBSERVER(service_observer_poll(false));
}

static uint32_t input_wpad_translate(int key, uint32_t expansion_type) {
	switch(key) {
		case 0: return WPAD_BUTTON_UP;
		case 1: return WPAD_BUTTON_DOWN;
		case 2: return WPAD_BUTTON_LEFT;
		case 3: return WPAD_BUTTON_RIGHT;
		case 4: return WPAD_BUTTON_A;
		case 5: return WPAD_BUTTON_B;
		case 6: return WPAD_BUTTON_1;
		case 7: return WPAD_BUTTON_2;
		case 8: return WPAD_BUTTON_PLUS;
		case 9: return WPAD_BUTTON_MINUS;
		case 10: return WPAD_BUTTON_HOME;
		default: break;
	}

	if(expansion_type == WPAD_EXP_NUNCHUK) {
		switch(key) {
			case 100: return WPAD_NUNCHUK_BUTTON_Z;
			case 101: return WPAD_NUNCHUK_BUTTON_C;
			default: break;
		}
	} else if(expansion_type == WPAD_EXP_CLASSIC) {
		switch(key) {
			case 200: return WPAD_CLASSIC_BUTTON_UP;
			case 201: return WPAD_CLASSIC_BUTTON_DOWN;
			case 202: return WPAD_CLASSIC_BUTTON_LEFT;
			case 203: return WPAD_CLASSIC_BUTTON_RIGHT;
			case 204: return WPAD_CLASSIC_BUTTON_A;
			case 205: return WPAD_CLASSIC_BUTTON_B;
			case 206: return WPAD_CLASSIC_BUTTON_X;
			case 207: return WPAD_CLASSIC_BUTTON_Y;
			case 208: return WPAD_CLASSIC_BUTTON_ZL;
			case 209: return WPAD_CLASSIC_BUTTON_ZR;
			case 210: return WPAD_CLASSIC_BUTTON_FULL_L;
			case 211: return WPAD_CLASSIC_BUTTON_FULL_R;
			case 212: return WPAD_CLASSIC_BUTTON_PLUS;
			case 213: return WPAD_CLASSIC_BUTTON_MINUS;
			case 214: return WPAD_CLASSIC_BUTTON_HOME;
			default: break;
		}
	} else if(expansion_type == WPAD_EXP_GUITARHERO3) {
		switch(key) {
			case 300: return WPAD_GUITAR_HERO_3_BUTTON_YELLOW;
			case 301: return WPAD_GUITAR_HERO_3_BUTTON_GREEN;
			case 302: return WPAD_GUITAR_HERO_3_BUTTON_BLUE;
			case 303: return WPAD_GUITAR_HERO_3_BUTTON_RED;
			case 304: return WPAD_GUITAR_HERO_3_BUTTON_ORANGE;
			case 305: return WPAD_GUITAR_HERO_3_BUTTON_PLUS;
			case 306: return WPAD_GUITAR_HERO_3_BUTTON_MINUS;
			case 307: return WPAD_GUITAR_HERO_3_BUTTON_STRUM_UP;
			case 308: return WPAD_GUITAR_HERO_3_BUTTON_STRUM_DOWN;
			default: break;
		}
	}

	return 0;
}

/* ReadPending can synthesize a delayed edge into btns_h after its callback.
 * Rearm requires neutral in BOTH the fresh report and the coalesced frame. */
static bool input_wpad_report_neutral(const WPADData* data) {
	int first = data->exp.type == WPAD_EXP_CLASSIC ? 200 : 0;
	int last = first == 200 ? 214 : 10;
	for(int key = first; key <= last; key++)
		if(data->btns_h & input_wpad_translate(key, data->exp.type))
			return false;
	if(data->exp.type == WPAD_EXP_NUNCHUK) {
		if(data->btns_h & (WPAD_NUNCHUK_BUTTON_C | WPAD_NUNCHUK_BUTTON_Z))
			return false;
		return isfinite(data->exp.nunchuk.js.mag)
			&& data->exp.nunchuk.js.mag <= .2F;
	}
	if(data->exp.type == WPAD_EXP_CLASSIC)
		return isfinite(data->exp.classic.ljs.mag)
			&& data->exp.classic.ljs.mag <= .2F
			&& isfinite(data->exp.classic.rjs.mag)
			&& data->exp.classic.rjs.mag <= .2F
			&& isfinite(data->exp.classic.l_shoulder)
			&& data->exp.classic.l_shoulder <= .2F
			&& isfinite(data->exp.classic.r_shoulder)
			&& data->exp.classic.r_shoulder <= .2F;
	return false;
}

// GameCube pad code space: 400-411, one family below the 900-range emulated
// sticks (930-943, see joystick_input's index comment) and clear of every
// existing family (Wiimote 0-10, Nunchuk 100-101, Classic 200-214, Guitar
// Hero 300-308 -- the doc originally proposed reusing 300 for GC, but that
// collides with Guitar Hero; corrected to 400s here and in
// docs/wii_controller_playability.md). A separate translate function (rather
// than folding into input_wpad_translate above) because these read
// PAD_ButtonsHeld, a wholly different device/subsystem from WPAD_ButtonsHeld
// -- keeps the GC read path textually distinct, per the split-seam note on
// gc_pad_connected.
static uint32_t input_pad_translate(int key) {
	switch(key) {
		case 400: return PAD_BUTTON_UP;
		case 401: return PAD_BUTTON_DOWN;
		case 402: return PAD_BUTTON_LEFT;
		case 403: return PAD_BUTTON_RIGHT;
		case 404: return PAD_BUTTON_A;
		case 405: return PAD_BUTTON_B;
		case 406: return PAD_BUTTON_X;
		case 407: return PAD_BUTTON_Y;
		case 408: return PAD_TRIGGER_Z;	// GC Z shoulder (doc: SNEAK)
		case 409: return PAD_TRIGGER_L;	// L shoulder, full digital click (doc: PLACE)
		case 410: return PAD_TRIGGER_R;	// R shoulder, full digital click (doc: BREAK)
		case 411: return PAD_BUTTON_START; // aka PAD_BUTTON_MENU, same bit
		default: break;
	}

	return 0;
}

/* Level read only -- see the logical layer's merged-mask comment. WPAD_ButtonsHeld
   is the full current button state (it includes the frame a button goes down, which
   is exactly why the old code had to subtract ButtonsDown/ButtonsUp from it here). */
bool input_native_key_down(int key) {
	/* SEAM S1, site 2 (mp_chat_login_3m, docs/chat_login_profile.md 1.4). On
	 * Wii this function reaches ONLY input_chat_open_edge's D-pad chord gate
	 * (input_poll returns before the merged tail), so the gate stays live AND
	 * becomes exercisable: the driver can request Up+Left and observe the
	 * refusal. Answers only codes in the virtual pad's vocabulary and only
	 * while armed; disarmed it returns false on its first statement, so every
	 * real player's chord answer is the code that runs today. */
	{
		bool v;
		if(chat_login_pad_native_key(key, &v))
			return v;
	}
	// 900-923 = Nunchuk/Classic emulated-stick-as-dpad (existing); 930-943 =
	// GameCube control-stick/C-stick emulated-as-dpad (js index 3, 4 -- see
	// joystick_input's index comment). One range check covers all five js
	// slots since js_emulated_btns_held is indexed the same way throughout.
	if(key >= 900 && key < 944) {
		int js = (key - 900) / 10;
		int offset = (key - 900) % 10;
		if(offset < 4)
			return js_emulated_btns_held[js][offset];
	}

	if(key >= 400 && key < 412) {
		uint32_t mask = input_pad_translate(key);
		return mask && (PAD_ButtonsHeld(PAD_CHAN0) & mask);
	}

	uint32_t mask = input_wpad_translate(key, js_expansion_type);
	return mask && (WPAD_ButtonsHeld(WPAD_CHAN_0) & mask);
}

bool input_native_key_symbol(int key, int* symbol, int* symbol_help,
							 enum input_category* category, int* priority) {
	*category = ((key >= 400 && key <= 411) || (key >= 930 && key <= 943)) ?
		INPUT_CAT_GAMECUBE :
		((key >= 200 && key <= 214) || (key >= 910 && key <= 923)) ?
		INPUT_CAT_CLASSIC_CONTROLLER :
		INPUT_CAT_NUNCHUK;
	*symbol = *symbol_help = input_glyph_key(*category, key);
	*priority = 1;
	return *symbol >= 0;
}

bool input_native_key_any(int* key) {
	return false;
}

void input_pointer_enable(bool enable) {
	if(enable != menu_ir_enabled)
		menu_ir.valid = false;
	menu_ir_enabled = enable;
	ir_look.valid_frames = 0;
	ir_look.x = ir_look.y = 0.0F;
}

bool input_pointer(float* x, float* y, float* angle) {
	bool script_available;
	if(pointer_script && pointer_script(x, y, angle, &script_available)) {
		menu_ir.valid = false;
		return script_available;
	}
	// menus_dwell hardware profile: owns the ENTIRE scripted window (not only
	// the one tick it asserts a position). On real hardware a Wiimote can sit
	// somewhere that coincidentally produces a VALID IR reading throughout an
	// unattended run; without this, that stray reading could land a real
	// click on whatever it happens to point at. See menu_autopilot.h.
	{
		bool available;
		if(menu_autopilot_force_pointer(x, y, angle, &available)) {
			menu_ir.valid = false;
			return available;
		}
	}

	const struct input_player_frame* owner = input_player_frame(0);
	if(owner) {
		bool valid = input_owner_pointer(owner, 0, 0, gfx_width(), gfx_height(),
										 x, y, angle);
		return input_menu_ir_filter(valid, x, y, *angle);
	}
	// Zero-init: WPAD_IR leaves `ir` untouched when the Wii Remote is absent / mid-reconnect
	// or its data format hasn't been (re)applied -> an uninitialised ir.valid could read true
	// with garbage x/y, snapping the camera/cursor. {0} -> ir.valid=false (no pointer this frame).
	struct ir_t ir = {0};
	WPAD_IR(WPAD_CHAN_0, &ir);
	*x = ir.x;
	*y = ir.y;
	*angle = ir.angle;
	return input_menu_ir_filter(ir.valid, x, y, *angle);
}

void input_native_joystick(float dt, float* dx, float* dy) {
	// Smooth analog LOOK (camera.c's rate integrator) -- doc precedent: doc
	// #1 (Nunchuk stick) and #2 (Classic right stick). #4 (GC C-stick) is the
	// same kind of source (a real analog axis, not the digital dpad emulation
	// js_emulated_btns_held reads for GUI nav) so it belongs in this same
	// priority chain, not bolted on separately -- this is genuinely a smooth
	// look axis, unlike the emulated-dpad path, and needs no IB_CAMERA_X/Y
	// enum to work (Nunchuk/Classic already prove that).
	const struct input_player_frame* owner = input_player_frame(0);
	if(owner) {
		*dx = *dy = 0;
		if(!owner->connected) {
			ir_look = (struct input_ir_look_state){0};
			return;
		}
		if(wiimote_ir_look && owner->category == INPUT_CAT_NUNCHUK) {
			unsigned channel = input_owners[0].device.channel;
			input_ir_look(&ir_look, owner->pointer_valid,
				(WPAD_ButtonsHeld(channel) & WPAD_BUTTON_2) != 0,
				2 * owner->pointer_x - 1, 1 - 2 * owner->pointer_y, dt, dx, dy);
		} else if(hypotf(owner->look_x, owner->look_y) > .2F) {
			*dx = owner->look_x * dt; *dy = owner->look_y * dt;
		}
		return;
	}
	*dx = *dy = 0;
	ir_look = (struct input_ir_look_state) {0};
}

/* No keyboard on a Wii: the OSK is the text input (input.h contract). Text
 * mode itself is a no-op here, but the disable half of the contract still
 * must swallow held edges: the press that closes a text screen may not
 * replay as a fresh rising edge on the arriving screen (same guard as PC's
 * input_text_enabled && !enable). */
static bool wii_text_enabled = false;

void input_text_enable(bool enable) {
	if(wii_text_enabled && !enable)
		input_edges_swallow_held();
	wii_text_enabled = enable;
}

bool input_text_poll(char* out) {
	(void)out;
	return false;
}

bool input_text_active(void) {
	return false;
}

#endif

// Platform-independent (declared in input.h, no platform headers needed --
// see the declaration comment): converts a signed-cartesian analog stick
// reading into the (dx, dy, magnitude) shape the Wii joystick_input[] table
// uses. Lives outside both #ifdef PLATFORM_* blocks above so the PC test
// suite (test_input_edges) links and exercises the exact function the Wii
// GameCube-pad path calls, not a reimplementation.
void input_stick_normalize(int x, int y, int range, float* dx, float* dy,
						   float* magnitude) {
	assert(dx && dy && magnitude && range > 0);

	float nx = (float)x / (float)range;
	float ny = (float)y / (float)range;
	// Direction must come from the RAW (unclamped) magnitude: a diagonal
	// reading past the stick's circular travel (both axes near +-range at
	// once) has raw_mag > 1.0, and dividing by a magnitude already clamped
	// to 1.0 would corrupt dx/dy for exactly that case (it did in an earlier
	// version of this function -- caught by the diagonal-clamp assertion in
	// test_input_edges). Only the returned deflection scalar clamps.
	float raw_mag = sqrtf(nx * nx + ny * ny);

	if(raw_mag > 1e-4F) {
		*dx = nx / raw_mag;
		*dy = ny / raw_mag;
	} else {
		*dx = 0.0F;
		*dy = 0.0F;
	}

	*magnitude = raw_mag > 1.0F ? 1.0F : raw_mag;
}

void input_ir_look(struct input_ir_look_state* s, bool valid, bool recenter,
	float x, float y, float dt, float* dx, float* dy) {
	*dx = *dy = 0.0F;
	if(!valid || !isfinite(x) || !isfinite(y) || !isfinite(dt)
	   || dt <= 0.0F || fabsf(x) > 1.0F || fabsf(y) > 1.0F) {
		s->valid_frames = 0;
		s->x = s->y = 0.0F;
		return;
	}
	if(recenter) {
		s->center_x = x;
		s->center_y = y;
		s->valid_frames = 0;
		s->x = s->y = 0.0F;
		return;
	}
	if(s->valid_frames < 2) {
		s->valid_frames++;
		return;
	}
	/* Bound catch-up after a stalled frame; this changes view rotation only. */
	dt = fminf(dt, 0.05F);
	float alpha = 1.0F - expf(-dt / 0.04F);
	x = glm_clamp(x - s->center_x, -1.0F, 1.0F);
	y = glm_clamp(y - s->center_y, -1.0F, 1.0F);
	x = fabsf(x) <= 0.2F ? 0.0F : copysignf((fabsf(x) - 0.2F) / 0.8F, x);
	y = fabsf(y) <= 0.2F ? 0.0F : copysignf((fabsf(y) - 0.2F) / 0.8F, y);
	s->x += alpha * (x - s->x);
	s->y += alpha * (y - s->y);
	*dx = s->x * dt;
	*dy = s->y * dt;
}

#include "../game/autopilot.h"

#ifdef CAVEWII_AGENT_BRIDGE
#include "../game/agent_input.h"
#endif

static const char* input_config_translate(enum input_button key) {
	switch(key) {
		case IB_ACTION1: return "input.item_action_left";
		case IB_ACTION2: return "input.item_action_right";
		case IB_FORWARD: return "input.player_forward";
		case IB_BACKWARD: return "input.player_backward";
		case IB_LEFT: return "input.player_left";
		case IB_RIGHT: return "input.player_right";
		case IB_JUMP: return "input.player_jump";
		case IB_SNEAK: return "input.player_sneak";
		case IB_INVENTORY: return "input.inventory";
		case IB_OPEN_INVENTORY: return "input.open_inventory";
		case IB_HOME: return "input.open_menu";
		case IB_SCROLL_LEFT: return "input.scroll_left";
		case IB_SCROLL_RIGHT: return "input.scroll_right";
		case IB_GUI_UP: return "input.gui_up";
		case IB_GUI_DOWN: return "input.gui_down";
		case IB_GUI_LEFT: return "input.gui_left";
		case IB_GUI_RIGHT: return "input.gui_right";
		case IB_GUI_CLICK: return "input.gui_click";
		case IB_GUI_CLICK_ALT: return "input.gui_click_alt";
		case IB_SCREENSHOT: return "input.screenshot";
		case IB_DEBUG_OVERLAY: return "input.debug_overlay";
		case IB_DROP: return "input.player_drop";
		case IB_GUI_SPLIT: return "input.gui_split";
		case IB_GUI_DROP: return "input.gui_drop";
		case IB_GUI_PAGE: return "input.gui_page";
		case IB_GUI_PAGE_SWAP: return "input.gui_page_swap";
		case IB_GUI_PREVIOUS: return "input.gui_previous";
		case IB_GUI_QUICK_MOVE: return "input.gui_quick_move";
		case IB_CHAT: return "input.chat";
		default: return NULL;
	}
}

/* New container actions work with existing config files. An explicit empty
 * array disables an action. Polling and prompts use the same fallback map. */
static bool input_read_bindings(enum input_button b, int* mapping,
								size_t* length) {
	const char* key = input_config_translate(b);
	if(!key)
		return false;
	if(config_read_int_array(&gstate.config_user, key, mapping, length))
		return true;
	/* b1-chat-opener-0919/07 C: the gameplay OPEN falls back to this config's
	 * own CLOSE codes. That is what makes the role split invisible to every
	 * configuration written before it existed -- including the one on the
	 * live card -- because there 1 / X / Y opened and closed alike, and with
	 * the key absent they still do. A config that DOES name the key is
	 * authoritative, as always; code never rewrites a user's file. */
	if(b == IB_OPEN_INVENTORY)
		return config_read_int_array(&gstate.config_user, "input.inventory",
									 mapping, length);
#ifdef PLATFORM_WII
	static const int split[] = {5, 205, 405}; /* B / B / B */
	static const int drop[] = {7, 213, 408}; /* 2 / Minus / Z */
	static const int page[] = {8, 211, 410}; /* Plus / R / R */
	static const int page_swap[] = {101, 208, 942, 943}; /* C / ZL / C-stick L/R */
	static const int previous[] = {9, 210, 409}; /* Minus / L / L */
	static const int quick[] = {100, 207, 406}; /* Nunchuk Z / Y / X */
	/* b1-chat-opener-0919: D-pad Up collided with movement on the Wii Remote
	 * (maintainer console report 2026-09-19), so the Remote's opener is
	 * button 2, opened by the gesture in input_chat_open_edge.
	 *
	 * CLASSIC = D-pad UP (200), a plain PRESS edge (Astra ruling 4 D1). Plus
	 * (212) was this branch's own regression: on the Classic, sneak is Minus
	 * HELD and Plus+Minus is the F3 chord, so an opener on Plus could not be
	 * used while sneaking -- the chord ate it. The Classic D-pad is free in
	 * gameplay (movement is the left stick, the hotbar is full L/R), so Up
	 * costs nothing there and restores chat-while-sneaking. The legacy
	 * Left/Right rock guard still refuses it (input_chat_dpad_rocked), and a
	 * config that explicitly names 212 keeps the release/Minus-cancel gesture.
	 *
	 * DOUBLE-BOUND ON PURPOSE, and safe because the readers are
	 * SCREEN-DISJOINT (review finding 6, pinned by test_input_wii_adapter):
	 * Remote 2 is also gui_drop (`drop` above), Classic Up is also gui_up and
	 * GameCube X is also gui_quick_move (`quick`), but IB_CHAT is read ONLY by
	 * screen_ingame's opener gate and those ONLY by the container/composer
	 * screens -- and the composer's neutral re-arm blocks the opening press
	 * from acting there (test_chat_keyboard). */
	/* E-3 (Astra ergonomics 4): the GameCube opener moved X -> D-pad UP (400),
	 * because X became the hotbar NEXT so the left thumb can keep retreating
	 * while the right thumb selects. Same plain press edge and the same legacy
	 * Left/Right rock guard the Classic D-pad Up opener uses. */
	static const int chat[] = {7, 200, 400}; /* 2 / D-pad Up / D-pad Up */
#else
	static const int split[] = {82}; /* R */
	static const int drop[] = {81}; /* Q */
	static const int page[] = {70}; /* F */
	static const int page_swap[] = {67}; /* C */
	static const int previous[] = {71}; /* G */
	static const int quick[] = {84}; /* T */
	static const int chat[] = {84}; /* T -- Beta's chat key, gameplay only */
#endif
	const int* defaults = b == IB_GUI_SPLIT ? split
		: b == IB_GUI_DROP ? drop : b == IB_GUI_PAGE ? page
		: b == IB_GUI_PAGE_SWAP ? page_swap
		: b == IB_GUI_PREVIOUS ? previous : b == IB_GUI_QUICK_MOVE ? quick
		: b == IB_CHAT ? chat : NULL;
	size_t count = b == IB_GUI_PAGE_SWAP
		? sizeof(page_swap) / sizeof(page_swap[0])
		: sizeof(split) / sizeof(split[0]);
	if(!defaults || *length < count)
		return false;
	for(size_t k = 0; k < count; k++)
		mapping[k] = defaults[k];
	*length = count;
	return true;
}

#ifdef PLATFORM_PC
static bool input_proof_symbol(enum input_button b, int* symbol,
							   int* symbol_help,
							   enum input_category* category) {
	/* Capture-only artwork override shared by full and compact menus. It does
	 * not bind/emulate a controller; absent/unknown values use native input. */
	const char* proof = config_read_string(&gstate.config_user,
		"debug.container_glyphs", "auto");
	const enum input_button actions[] = {IB_GUI_QUICK_MOVE, IB_GUI_CLICK,
		IB_GUI_SPLIT, IB_GUI_DROP, IB_INVENTORY, IB_GUI_CLICK_ALT,
		IB_GUI_PREVIOUS, IB_GUI_PAGE, IB_HOME, IB_GUI_UP, IB_GUI_DOWN,
		IB_GUI_LEFT, IB_GUI_RIGHT};
	static const int wiimote[] = {8, 0, 1, 3, 2, 1, 6, 5, 4, 24, 24, 24, 24};
	static const int classic[] = {13, 10, 11, 6, 12, 11, 22, 23, 4, 24, 24, 24, 24};
	static const int gamecube[] = {34, 32, 33, 36, 35, 33, 37, 38, 39, 24, 24, 24, 24};
	bool wii = !strcmp(proof, "wiimote");
	bool cc = !strcmp(proof, "classic");
	const int* glyphs = wii ? wiimote : cc ? classic : !strcmp(proof, "gamecube") ? gamecube : NULL;
	if(glyphs) {
		for(size_t k = 0; k < sizeof(actions) / sizeof(actions[0]); k++) {
			if(b != actions[k]) continue;
			*symbol = *symbol_help = glyphs[k];
			*category = wii ? INPUT_CAT_NUNCHUK : cc ? INPUT_CAT_CLASSIC_CONTROLLER : INPUT_CAT_GAMECUBE;
			return true;
		}
	}
	return false;
}
#endif

/* critique-1028 glyph-proof hook: the family the menus walker presents
 * (menu_autopilot_arm sets it from debug.autopilot_device). Owned here so
 * every test double of the walker links unchanged. */
static enum input_category input_walker_family = INPUT_CAT_NUNCHUK;
void input_walker_family_set(enum input_category family) {
	input_walker_family = family == INPUT_CAT_GAMECUBE
			|| family == INPUT_CAT_CLASSIC_CONTROLLER
		? family : INPUT_CAT_NUNCHUK;
}

#ifdef PLATFORM_WII
static void input_prompt_frame_walker(struct input_player_frame* frame);
#endif

void input_prompt_frame(unsigned player, struct input_player_frame* frame) {
	const struct input_player_frame* owner = input_player_frame(player);
	if(owner) {
		*frame = *owner;
		return;
	}
	*frame = (struct input_player_frame) {.category = INPUT_CAT_NONE};
#ifdef PLATFORM_WII
	if(player == 0 && menu_autopilot_active()) {
		/* Rig-only (2026-09-11): the menus_dwell walker drives player 1
		 * with no controller paired, so every footer would carry the join
		 * prompt; present the floor controller (Wii Remote + Nunchuk) so
		 * the console photographs the approved prompts. Same family as the
		 * autopilot-driven host's capture exemption (main.c, be3296dd).
		 * Presentation only: owner frames, notices and input are untouched. */
		input_prompt_frame_walker(frame);
		return;
	}
	frame->join_prompt = player == 0;
#endif
	for(int b = 0; b < IB_COUNT; b++) {
		frame->glyphs[b] = -1;
#ifdef PLATFORM_PC
		if(player != 0)
			continue;
		int keys[INPUT_BINDINGS_PER_ACTION];
		size_t count = INPUT_BINDINGS_PER_ACTION;
		if(input_read_bindings(b, keys, &count))
			frame->glyphs[b] = input_glyph_binding(INPUT_CAT_NONE, keys, count);
		int symbol, help;
		enum input_category category;
		if(input_proof_symbol(b, &symbol, &help, &category)) {
			frame->glyphs[b] = help;
			frame->category = category;
		}
#endif
	}
}

bool input_symbol(enum input_button b, int* symbol, int* symbol_help,
				  enum input_category* category) {
	if(b < 0 || b >= IB_COUNT)
		return false;
	struct input_player_frame frame;
	input_prompt_frame(0, &frame);
	if(frame.glyphs[b] < 0)
		return false;
	*symbol = *symbol_help = frame.glyphs[b];
	*category = frame.category;
	return true;
}

/* BUTTON COMBOS (2026-07-30, maintainer request: a way to hide the F3 overlay from the
   couch). A mapping entry >= 1008 encodes a two-button chord: HOLD*1000 + PRESS -- the
   binding is down while BOTH halves are down. E.g. 9008 = hold Wiimote MINUS (9) + PLUS
   (8). While a chord is down its two component keys are CONSUMED (input_poll below), so
   the chord no longer leaks a stray scroll on each half; the old design instead relied
   on the two leaked scrolls cancelling each other out.

   1000..1007 are NOT chords: they are the PC mouse buttons (input.h convention, e.g.
   "item_action_left": [1000] in config_pc.json). The old `code < 1000` cutoff decoded
   left-click as "hold key 1, press key 0" -- two codes GLFW rejects -- so mouse
   place/break/gui_click could never fire through input_pressed/input_held at all. */
static bool input_combo_decode(int code, int* hold_key, int* press_key) {
	if(code < 1008)
		return false;
	*hold_key = code / 1000;
	*press_key = code % 1000;
	return true;
}

/* ---- Merged per-action input state (2026-08-14) -----------------------------
   Every binding source for one logical action -- Wiimote button, Classic button,
   emulated stick direction, chord -- is ORed into ONE down-state per action here,
   once per frame, and the press/release edges are computed against the previous
   frame's state. An action is down if ANY source asks; it releases when none do.

   This replaces a per-binding edge model whose `any_pressed && !any_held &&
   !any_released` anti-ghost rule swallowed real press edges: the native layer used
   to hand out edges per QUERY, so on PC the first consumer of a key in a frame saw
   "pressed" and every later one saw "held" -- and the shipped PC config binds 87 to
   both player_forward and gui_up. Whichever action asked second lost its press.
   With one mask there is nothing left to ghost, and no reason for the rule.

   It is also the prerequisite for a second edge generator (a GameCube PAD path):
   that would have made the per-binding model strictly worse, while here it is one
   more source ORed into the same mask.

   Cost: the binding arrays are read from the JSON config once per frame instead of
   once per query (~30 queries/frame, each re-parsing a dot-path and, on Wii,
   re-copying the WPAD expansion struct). */
// 8 -> 12 (2026-09 GC pad wiring): config_read_int_array silently TRUNCATES a
// JSON array longer than the length passed in (source/config.c), so an
// undersized cap here is a silent-drop hazard, not a loud failure. Adding a
// third controller family pushed the busiest actions (gui_up/down/left/
// right: raw dpad + two controllers' emulated-stick-as-dpad sources) to
// exactly 8 -- headroom, not just today's count, is the point.
#define INPUT_MAX_BINDINGS INPUT_BINDINGS_PER_ACTION
#define INPUT_MAX_CONSUMED 16

static int input_map[IB_COUNT][INPUT_MAX_BINDINGS];
static size_t input_map_length[IB_COUNT];

#ifdef PLATFORM_WII
/* ---- the DOUBLE-DUTY pair (b1-chat-opener-0919 slice 1) -----------------
 *
 * Astra ruling 3 D2 applies to "the Remote codes that form the debug chord
 * with each other", and the brief forbids writing that as the literal 9 and 8.
 * So it is derived, once, here: this family's FIRST accepted scroll_left and
 * scroll_right codes, but only when some action's chord binding is made of
 * exactly those two (in either orientation). With the shipped config that is
 * the Wii Remote's Minus/Plus and the 9008 overlay chord; Classic's 212213
 * (its hotbar is full L/R, 210/211) and GameCube's 409410 (D-pad L/R,
 * 402/403) do not match, so nothing about them changes. A config that puts
 * the hotbar back on the D-pad drops out the same way.
 *
 * Returns false, leaving the two out-parameters untouched, when the family
 * binds no such pair. */
static bool input_family_code(enum input_category family, enum input_button b,
							  int* code) {
	for(size_t k = 0; k < input_map_length[b]; k++) {
		int key = input_map[b][k];
		if(key >= 0 && key < 1008 && input_owner_accepts(family, key)) {
			*code = key;
			return true;
		}
	}
	return false;
}

/* ---- the Wii Remote SNEAK TOGGLE (b1-chat-opener-0919 slice 2) ----------
 *
 * Astra ruling 3 D3. The machine itself is pure (input_sneak_toggle.h); this
 * is the scoping it needs and the clears it cannot see for itself.
 *
 * WHERE THE LEVEL GOES: straight into the ORDINARY IB_SNEAK level, computed
 * right after input_binding_levels and before input_owner_latch. Every
 * consumer -- entity_local_player.c's walking and boat paths, camera.c's
 * crouch offset, client_interface.c's two sneak reports -- therefore sees
 * exactly what a held button would have given it, and none of them changes.
 * The alternative (a second "is sneaking" query at five call sites) would have
 * been five chances for the two answers to drift apart. */
void input_sneak_gameplay_tick(unsigned player, bool dead, bool resting) {
	if(player >= INPUT_LOCAL_PLAYERS)
		return;
	/* Claiming THIS poll is what makes the next one a gameplay frame. A poll
	 * nobody claimed is a poll some other screen owned. */
	input_sneak_state[player].gameplay_serial = input_chat_poll_serial;
	input_sneak_state[player].dead = dead;
	input_sneak_state[player].resting = resting;
}

void input_sneak_vehicle_mark(unsigned player, bool riding) {
	if(player < INPUT_LOCAL_PLAYERS)
		input_sneak_state[player].vehicle = riding;
}

/* READ-ONLY (Astra ruling 6 Q2's marker). One field read, nothing else: the
 * machine is zeroed by input_sneak_apply on every frame this family is not
 * running the toggle, so `engaged` is true only while the TOGGLE owns the
 * sneak level -- a held non-toggle sneak can never report true here. */
bool input_sneak_toggle_latched(unsigned player) {
	return player < INPUT_LOCAL_PLAYERS
		&& input_sneak_state[player].toggle.engaged;
}

void input_sneak_toggle_release(unsigned player) {
	if(player < INPUT_LOCAL_PLAYERS)
		input_sneak_state[player].toggle = (struct input_sneak_toggle){0};
}

static bool input_sneak_apply(unsigned player, enum input_category category,
							  bool connected, bool raw) {
	if(player >= INPUT_LOCAL_PLAYERS)
		return raw;
	struct input_sneak_scope* s = &input_sneak_state[player];
	struct input_device device = {INPUT_DEVICE_NONE, 0};
	(void)input_player_device(player, &device);
	/* A LEVEL the client tick owns, NOT consumed here: the pure machine takes
	 * its rising edge, so the toggle clears once per boarding and the player
	 * may then sneak while riding for as many polls as the ride lasts. */
	bool vehicle = s->vehicle;
	/* The rest of Astra D3's clear list, none of which the pure machine can
	 * see: a world or session LEAVE or REPLACEMENT (no in-game frame runs to
	 * report it, so the flags are read here), a controller DISCONNECT, and an
	 * OWNER or FAMILY change. (`controls.*` is read ONCE at boot -- see
	 * input_init -- so there is no run-time scheme-mode change to clear.) */
	bool world = gstate.world_loaded && !gstate.world_switch_pending;
	if(!s->identified || s->kind != (int)device.kind
	   || s->channel != device.channel || s->category != (int)category
	   || !connected || !world) {
		s->toggle = (struct input_sneak_toggle) {0};
		/* Nothing survives a replacement, including the ride level: the next
		 * client tick re-publishes it either way. */
		s->vehicle = false;
		vehicle = false;
		s->kind = (int)device.kind;
		s->channel = device.channel;
		s->category = (int)category;
		s->identified = true;
	}
	/* ALL THREE gameplay families, each behind its own key (Astra ergonomics
	 * study 2026-09-19 recommendations 1 and 3, maintainer-accepted; this
	 * replaces the earlier "a shoulder button is free there" reading, which
	 * the study showed to be wrong for the hands that actually hold the pad):
	 *
	 *   Remote + Nunchuk -- the right thumb has to leave the D-pad to reach 1
	 *     at all, so a held sneak costs movement.
	 *   CLASSIC -- the right thumb owns the CAMERA STICK. Holding Y means
	 *     letting go of the camera at exactly the moment a ledge needs it.
	 *   GAMECUBE -- the right index finger owns R (break). Holding Z while
	 *     mining a block under your feet needs a special grip.
	 *
	 * A bare Remote (INPUT_CAT_WIIMOTE) is inventory state, never a gameplay
	 * family, so it falls through to the raw level like any other category. */
	bool family_toggle = category == INPUT_CAT_NUNCHUK ? wiimote_sneak_toggle
		: category == INPUT_CAT_CLASSIC_CONTROLLER ? classic_sneak_toggle
		: category == INPUT_CAT_GAMECUBE ? gamecube_sneak_toggle
										 : false;
	if(!(family_toggle && connected)) {
		s->toggle = (struct input_sneak_toggle) {0};
		return raw;
	}
	return input_sneak_toggle_step(&s->toggle, raw,
								   s->gameplay_serial + 1
									   == input_chat_poll_serial,
								   s->dead, s->resting || vehicle);
}

static bool input_hotbar_chord_pair(enum input_category family, int* prev,
									int* next) {
	int left, right;
	if(!input_family_code(family, IB_SCROLL_LEFT, &left)
	   || !input_family_code(family, IB_SCROLL_RIGHT, &right)
	   || left == right)
		return false;
	for(int b = 0; b < IB_COUNT; b++) {
		for(size_t k = 0; k < input_map_length[b]; k++) {
			int key = input_map[b][k];
			if(key < 1008 || key / 1000 >= 1008)
				continue;
			int hold = key / 1000, press = key % 1000;
			if((hold == left && press == right)
			   || (hold == right && press == left)) {
				*prev = left;
				*next = right;
				return true;
			}
		}
	}
	return false;
}

/* Refuse a chord that was not entered in its CONFIGURED order. Layered on top
 * of input_binding_levels (which stays pure and still consumes both halves, so
 * a wrong-order chord moves no hotbar slot either) and scoped to the one chord
 * made of this family's hotbar pair -- every other chord is untouched. The
 * gate only ever clears a level: a chord that is not fully down was already
 * false, and another binding of the same action is never overwritten. */
static void input_chord_order_apply(unsigned player, input_key_reader read,
									void* user, enum input_category family,
									bool* levels) {
	int prev = 0, next = 0, hold = 0, press = 0;
	if(input_hotbar_chord_pair(family, &prev, &next)) {
		for(int b = 0; b < IB_COUNT && !hold; b++) {
			for(size_t k = 0; k < input_map_length[b] && !hold; k++) {
				int key = input_map[b][k];
				if(key < 1008 || key / 1000 >= 1008)
					continue;
				int h = key / 1000, p = key % 1000;
				if((h == prev && p == next) || (h == next && p == prev)) {
					hold = h;
					press = p;
				}
			}
		}
	}
	if(player >= INPUT_LOCAL_PLAYERS)
		return;
	if(hold != input_chord_keys[player][0]
	   || press != input_chord_keys[player][1]) {
		input_chord_state[player] = (struct input_chord_order) {0};
		input_chord_keys[player][0] = hold;
		input_chord_keys[player][1] = press;
	}
	if(!hold)
		return;
	bool hold_down = read(user, hold), press_down = read(user, press);
	bool ordered = input_chord_order_step(&input_chord_state[player], hold_down,
										  press_down);
	if(ordered || !(hold_down && press_down))
		return;
	for(int b = 0; b < IB_COUNT; b++) {
		for(size_t k = 0; k < input_map_length[b]; k++) {
			int key = input_map[b][k];
			if(key >= 1008 && key / 1000 == hold && key % 1000 == press) {
				levels[b] = false;
				break;
			}
		}
	}
}
#endif

#ifdef PLATFORM_PC
static bool input_mouse_down[IB_COUNT], input_mouse_pressed[IB_COUNT];

static bool input_mouse_key(void* user, int key) {
	(void)user;
	return key >= 1000 && key <= 1000 + GLFW_MOUSE_BUTTON_LAST
		&& input_native_key_down(key);
}
#endif

bool input_text_mouse_pressed(enum input_button b) {
#ifdef PLATFORM_PC
	return b >= 0 && b < IB_COUNT && input_mouse_pressed[b];
#else
	(void)b;
	return false;
#endif
}

#ifdef PLATFORM_WII
/* The menus_dwell walker's player 1 (no controller paired) presented as the
 * floor controller so footers photograph the approved prompts; see
 * input_prompt_frame. Lives below the binding table it reads. */
static void input_prompt_frame_walker(struct input_player_frame* frame) {
	frame->connected = true;
	/* critique-1028: debug.autopilot_device picks the presented family
	 * (default Nunchuk, the floor controller) -- glyph proof only. */
	frame->category = input_walker_family;
	input_glyph_frame(frame, input_map, input_map_length);
}
#endif
static bool input_action_down[IB_COUNT];
static bool input_action_prev[IB_COUNT];

/* ui-build-1026 T15c: the menus_dwell walker's LEVEL and RELEASE edge. The
 * walker's one-frame press reaches input_pressed() through
 * menu_autopilot_force_button; menu_autopilot_force_level gives the same
 * press its held level on that frame and its falling edge on the next, so a
 * press-held-release detector (input_menu_page_read's page-swap TAP) sees a
 * real tap. Built on menu_autopilot_force_button alone (no new walker symbol,
 * so the test doubles that stub it link unchanged): main.c ticks the walker
 * BEFORE the screens read input and input_poll() runs AFTER them, so
 * input_walker_prev latched at the top of input_poll() is exactly "the
 * walker pressed this on the frame just read". A disarmed or finished walker
 * forces nothing, input_walker_prev stays all-false, and both hooks answer
 * false -- input_held/input_released fall through to the merged hardware
 * mask unchanged. */
static bool input_walker_prev[IB_COUNT];

static bool input_walker_forced(enum input_button b) {
	bool v = false;
	return menu_autopilot_force_button(b, &v) && v;
}

static bool input_walker_level(enum input_button b, bool* down,
							   bool* released) {
	if(b < 0 || b >= IB_COUNT)
		return false;
	bool now = input_walker_forced(b);
	if(!now && !input_walker_prev[b])
		return false; /* nothing of the walker's on this button */
	*down = now;
	*released = !now && input_walker_prev[b];
	return true;
}

static void input_walker_latch(void) {
	for(int b = 0; b < IB_COUNT; b++)
		input_walker_prev[b] = input_walker_forced((enum input_button)b);
}

/* The autopilot twin of input_action_prev: "was this button autopilot-FORCED
 * at the previous input_poll". input_pressed() edges the live forced level
 * against it (autopilot_force_edge_pressed); input_poll() advances it exactly
 * once per frame (input_autopilot_edges_advance below). */
static bool input_autopilot_forced_prev[IB_COUNT];

bool input_player_bind(unsigned player, struct input_device device) {
#ifdef PLATFORM_PC
	if(device.kind != INPUT_DEVICE_KEYBOARD) return false;
#else
	if(device.kind != INPUT_DEVICE_WPAD && device.kind != INPUT_DEVICE_GAMECUBE)
		return false;
#endif
	if(player >= INPUT_LOCAL_PLAYERS) return false;
	bool changed = !input_device_equal(input_owners[player].device, device);
	if(!input_owner_bind(input_owners, INPUT_LOCAL_PLAYERS, player, device)) return false;
	if(changed) input_menu_page_cancel(player);
#ifdef PLATFORM_WII
	if(changed && device.kind == INPUT_DEVICE_WPAD)
		input_wpad_configured[device.channel] = false;
#else
	(void)changed;
#endif
	return true;
}

void input_player_unbind(unsigned player) {
	if(player >= INPUT_LOCAL_PLAYERS) return;
	input_menu_page_cancel(player);
	struct input_device device = input_owners[player].device;
	/* Leave was commonly confirmed with A. That held A must not become
	 * another Join when the successful leave reply releases this claim. */
	if((device.kind == INPUT_DEVICE_WPAD || device.kind == INPUT_DEVICE_GAMECUBE)
	   && device.channel < 4) {
		unsigned index = device.channel + (device.kind == INPUT_DEVICE_GAMECUBE ? 4 : 0);
		device_inventory.devices[index].armed = false;
	}
	input_owners[player] = (struct input_owner){0};
}

const struct input_player_frame* input_player_frame(unsigned player) {
	if(player >= INPUT_LOCAL_PLAYERS
	   || input_owners[player].device.kind == INPUT_DEVICE_NONE) return NULL;
	return &input_owners[player].frame;
}

/* The device a player owns, for callers that need the DEBOUNCED hardware
 * state behind the frame (the controller-recovery gate distinguishes "the
 * controller is back" from "the controller is back AND neutral", which the
 * frame's single connected flag deliberately conflates). */
bool input_player_device(unsigned player, struct input_device* device) {
	if(player >= INPUT_LOCAL_PLAYERS || !device
	   || input_owners[player].device.kind == INPUT_DEVICE_NONE) return false;
	*device = input_owners[player].device;
	return true;
}

/* Consume one action for this player: it reads as already-held until the
 * device reports it released, exactly like the attach/reconnect latch in
 * input_owner_latch. The recovery gate uses this so the deliberate Resume
 * press cannot also land on the menu that was retained underneath it. */
void input_player_block(unsigned player, enum input_button button) {
	if(player >= INPUT_LOCAL_PLAYERS || button < 0 || button >= IB_COUNT
	   || input_owners[player].device.kind == INPUT_DEVICE_NONE) return;
	struct input_owner* owner = &input_owners[player];
	owner->blocked[button] = true;
	owner->frame.down[button] = false;
	owner->frame.pressed[button] = false;
	owner->frame.released[button] = false;
}

struct input_device_sample {
	enum input_category category;
#ifdef PLATFORM_WII
	uint32_t buttons, expansion_type;
	float sticks[5][2];
	/* GameCube C-stick as four digital directions (codes 940-943, the menu
	 * page swap), latched with hysteresis: see input_cstick_latch. */
	bool cstick[4];
#endif
};

#ifdef PLATFORM_WII
/* BUGS.md #07 (round 2): the C-stick is the GameCube page swap ("brief
 * C-stick left or right, then let it centre", docs/controls_sheet.md 2).
 * Read with the plain 0.2 stick threshold, a stiff C-stick springing back
 * overshoots past centre to the OTHER side (a second tap -> the page swaps
 * back), and a worn stick resting near 0.2 flickers on/off by itself, so the
 * inventory toggled between Inventory and Crafting with no deliberate input.
 * A direction now engages only past INPUT_CSTICK_ENGAGE and releases only
 * back inside INPUT_CSTICK_RELEASE, so one flick is one press. Feel layer
 * only (sanctioned deviation 2): the analog C-stick look path is unchanged,
 * and 940-943 have no gameplay binding. */
#define INPUT_CSTICK_ENGAGE .5F
#define INPUT_CSTICK_RELEASE .25F
static void input_cstick_axis(bool* negative, bool* positive, float v) {
	if(!isfinite(v)) v = 0;
	if(*positive) *positive = v > INPUT_CSTICK_RELEASE;
	else if(*negative) *negative = v < -INPUT_CSTICK_RELEASE;
	else if(v > INPUT_CSTICK_ENGAGE) *positive = true;
	else if(v < -INPUT_CSTICK_ENGAGE) *negative = true;
}
/* latch: up, down, left, right (the 9x0..9x3 offset order). */
static void input_cstick_latch(bool latch[4], float x, float y) {
	input_cstick_axis(&latch[1], &latch[0], y);
	input_cstick_axis(&latch[2], &latch[3], x);
}
#endif

static bool input_device_accepts(const struct input_device_sample* sample, int key) {
#ifdef PLATFORM_PC
	(void)sample;
	return (key >= GLFW_KEY_SPACE && key <= GLFW_KEY_LAST)
		|| (key >= 1000 && key <= 1000 + GLFW_MOUSE_BUTTON_LAST);
#else
	return input_owner_accepts(sample->category, key);
#endif
}

static bool input_device_key(void* user, int key) {
	const struct input_device_sample* sample = user;
	if(!input_device_accepts(sample, key)) return false;
#ifdef PLATFORM_PC
	return input_native_key_down(key);
#else
	if(key >= 940 && key <= 943)
		return sample->cstick[key - 940];
	if(key >= 900) {
		int stick = (key - 900) / 10, direction = (key - 900) % 10;
		float x = sample->sticks[stick][0], y = sample->sticks[stick][1];
		return direction == 0 ? y > .2F : direction == 1 ? y < -.2F
			: direction == 2 ? x < -.2F : x > .2F;
	}
	uint32_t mask = sample->category == INPUT_CAT_GAMECUBE
		? input_pad_translate(key) : input_wpad_translate(key, sample->expansion_type);
	return (sample->buttons & mask) != 0;
#endif
}

#ifdef PLATFORM_WII
static void input_device_stick(float* xy, float angle, float magnitude) {
	xy[0] = sinf(glm_rad(angle)) * magnitude;
	xy[1] = cosf(glm_rad(angle)) * magnitude;
}
#endif

#ifdef PLATFORM_WII
static struct input_device_sample device_samples[INPUT_DEVICE_COUNT];
static bool gc_cstick_latch[4][4]; /* per GC channel; input_cstick_latch */
static struct input_player_frame device_frames[INPUT_DEVICE_COUNT];

/* Reuse the scan and the existing Probe/Data reads, once per channel, including
 * unassigned devices. No ownership or glyph selection is inferred from
 * presence. */
static void input_devices_poll(void) {
	ptime_t now = time_get();
	for(unsigned index = 0; index < INPUT_DEVICE_COUNT; index++) {
		struct input_device_sample sample = {.category = INPUT_CAT_NONE};
		struct input_player_frame frame = {.category = INPUT_CAT_NONE};
		struct input_identity identity = {false, INPUT_CAT_NONE};
		unsigned channel = index % 4;
		/* Wireless battery, read from the SAME WPADData this loop already
		 * pulls -- no extra driver call, no per-frame allocation. */
		bool battery_supported = false;
		unsigned battery_percent = 0;
		if(index >= 4) {
			frame.connected = (pad_connected_mask & (PAD_CHAN0_BIT >> channel)) != 0;
			sample.category = INPUT_CAT_GAMECUBE;
			identity
				= (struct input_identity) {frame.connected, INPUT_CAT_GAMECUBE};
			if(frame.connected) {
				sample.buttons = PAD_ButtonsHeld(channel);
				float dx, dy, magnitude;
				input_stick_normalize(PAD_StickX(channel), PAD_StickY(channel),
					128, &dx, &dy, &magnitude);
				sample.sticks[3][0] = dx * magnitude;
				sample.sticks[3][1] = dy * magnitude;
				input_stick_normalize(PAD_SubStickX(channel), PAD_SubStickY(channel),
					128, &dx, &dy, &magnitude);
				sample.sticks[4][0] = dx * magnitude;
				sample.sticks[4][1] = dy * magnitude;
				input_cstick_latch(gc_cstick_latch[channel],
					sample.sticks[4][0], sample.sticks[4][1]);
			} else {
				memset(gc_cstick_latch[channel], 0,
					sizeof(gc_cstick_latch[channel]));
			}
			memcpy(sample.cstick, gc_cstick_latch[channel], sizeof(sample.cstick));
		} else {
			uint32_t type = WPAD_EXP_NONE;
			bool attached = WPAD_Probe(channel, &type) == WPAD_ERR_NONE;
			SERVICE_OBSERVER(if(!attached || !input_wpad_fresh[channel]) service_observer_receipt(channel, attached, false));
			identity = (struct input_identity) {
				attached,
				type == WPAD_EXP_CLASSIC	 ? INPUT_CAT_CLASSIC_CONTROLLER :
					type == WPAD_EXP_NUNCHUK ? INPUT_CAT_NUNCHUK :
					type == WPAD_EXP_NONE	 ? INPUT_CAT_WIIMOTE :
											   INPUT_CAT_NONE};
			if(!attached)
				input_wpad_configured[channel] = false;
			/* A returning Remote may occupy a different channel. Configure
			 * continuous reports before ownership: claiming requires a fresh
			 * neutral report, which event-only defaults need not deliver. */
			if(attached && !input_wpad_configured[channel]) {
				int format = WPAD_SetDataFormat(channel, WPAD_FMT_BTNS_ACC_IR);
				int resolution = WPAD_SetVRes(channel, gfx_width(), gfx_height());
				input_wpad_configured[channel] = format == WPAD_ERR_NONE
					&& resolution == WPAD_ERR_NONE;
			}
			const WPADData* data = attached ? WPAD_Data(channel) : NULL;
			/* wiiuse fills battery_level only once a STATUS report has
			 * arrived, so a zero level with err == NONE is "never reported",
			 * not "empty" (see input_wpad_raw_diag0's comment). Its full
			 * reading is 0xC0, which is the scale used here. */
			if(data && data->err == WPAD_ERR_NONE && data->battery_level > 0) {
				battery_supported = true;
				battery_percent = (unsigned)data->battery_level * 100u / 192u;
				if(battery_percent > 100)
					battery_percent = 100;
			}
			frame.connected = data && data->err == WPAD_ERR_NONE
				&& (data->data_present & WPAD_DATA_BUTTONS)
				&& (uint32_t)data->exp.type == type;
			if(frame.connected && (type == WPAD_EXP_NUNCHUK || type == WPAD_EXP_CLASSIC))
				frame.connected = (data->data_present & WPAD_DATA_EXPANSION) != 0;
			if(frame.connected) {
				sample.buttons = data->btns_h;
				sample.expansion_type = data->exp.type;
				sample.category = data->exp.type == WPAD_EXP_CLASSIC ?
					INPUT_CAT_CLASSIC_CONTROLLER :
					data->exp.type == WPAD_EXP_NUNCHUK ? INPUT_CAT_NUNCHUK :
					type == WPAD_EXP_NONE			   ? INPUT_CAT_WIIMOTE :
														 INPUT_CAT_NONE;
				if(sample.category == INPUT_CAT_NUNCHUK)
					input_device_stick(sample.sticks[0], data->exp.nunchuk.js.ang,
						data->exp.nunchuk.js.mag);
				if(sample.category == INPUT_CAT_CLASSIC_CONTROLLER) {
					input_device_stick(sample.sticks[1], data->exp.classic.ljs.ang,
						data->exp.classic.ljs.mag);
					input_device_stick(sample.sticks[2], data->exp.classic.rjs.ang,
						data->exp.classic.rjs.mag);
				}
				frame.pointer_valid = sample.category != INPUT_CAT_CLASSIC_CONTROLLER
					&& (data->data_present & WPAD_DATA_IR) && data->ir.valid
					&& gfx_width() > 0 && gfx_height() > 0;
				if(frame.pointer_valid) {
					frame.pointer_normalized = true;
					frame.pointer_x = data->ir.x / gfx_width();
					frame.pointer_y = data->ir.y / gfx_height();
					frame.pointer_angle = data->ir.angle;
				}
			}
		}
		frame.category = sample.category;
		unsigned look = sample.category == INPUT_CAT_GAMECUBE ? 4 :
			sample.category == INPUT_CAT_CLASSIC_CONTROLLER	  ? 2 :
																0;
		frame.look_x = sample.sticks[look][0];
		frame.look_y = sample.sticks[look][1];
		/* Classic ignores the resting Remote's buttons and IR. All physical
		 * controls in its own family, including unbound ones, must be neutral.
		 */
		bool neutral = true;
		int first = sample.category == INPUT_CAT_GAMECUBE	? 400 :
			sample.category == INPUT_CAT_CLASSIC_CONTROLLER ? 200 :
															  0;
		int last = first == 400 ? 411 : first == 200 ? 214 : 10;
		for(int key = first; key <= last; key++)
			if(input_device_key(&sample, key))
				neutral = false;
		if(sample.category == INPUT_CAT_NUNCHUK
		   && (input_device_key(&sample, 100)
			   || input_device_key(&sample, 101)))
			neutral = false;
		for(unsigned j = 0; j < 5; j++)
			for(unsigned axis = 0; axis < 2; axis++)
				if(!isfinite(sample.sticks[j][axis])
				   || fabsf(sample.sticks[j][axis]) > .2F)
					neutral = false;
		struct input_device device
			= {index < 4 ? INPUT_DEVICE_WPAD : INPUT_DEVICE_GAMECUBE, channel};
		/* ---- SEAM S1, site 3 (docs/chat_login_profile.md 1.1) -------------
		 * The virtual pad has to be visible to the device INVENTORY too, not
		 * just to the owner frame: screen_controller_recovery() decides "is
		 * player 0's owner present" from published.connected, so without this
		 * the recovery modal would own the whole update from the moment the
		 * driver bound. Substitutes only the four inventory inputs for the
		 * driver's own channel; device_frames/device_samples are untouched, so
		 * the claim loops and input_split_reclaim see exactly what they see
		 * today. False when disarmed or unbound: byte-identical. */
		bool cl_connected = frame.connected;
		bool cl_fresh = index >= 4 || input_wpad_fresh[channel];
		bool cl_neutral = neutral && (index >= 4 || input_wpad_neutral[channel]);
		(void)chat_login_pad_inventory(device, &identity, &cl_connected,
									   &cl_fresh, &cl_neutral);
		input_inventory_poll(&device_inventory, device, identity, cl_connected,
							 cl_fresh, cl_neutral, now, input_owners,
							 INPUT_LOCAL_PLAYERS);
		/* A wired GameCube pad passes supported=false on both feeds: the
		 * approved map forbids an impossible GameCube battery or IR event. */
		input_inventory_battery(&device_inventory, device,
								index < 4 && battery_supported, battery_percent,
								now, input_owners, INPUT_LOCAL_PLAYERS);
		bool owned = false;
		for(unsigned p = 0; p < INPUT_LOCAL_PLAYERS; p++)
			owned |= input_device_equal(input_owners[p].device, device);
		/* "Pointer out of view is only relevant to the pointer controller":
		 * a Classic or GameCube owner has none, and an unassigned Remote has
		 * nobody to tell. */
		input_inventory_pointer(&device_inventory, device,
								index < 4 && owned && frame.connected
									&& sample.category != INPUT_CAT_CLASSIC_CONTROLLER,
								frame.pointer_valid, now, input_owners,
								INPUT_LOCAL_PLAYERS);
		device_samples[index] = sample;
		device_frames[index] = frame;
	}
}
#endif

static void input_players_poll(void) {
	for(unsigned player = 0; player < INPUT_LOCAL_PLAYERS; player++) {
		struct input_owner* owner = &input_owners[player];
		if(owner->device.kind == INPUT_DEVICE_NONE)
			continue;
		struct input_player_frame frame = {.category = INPUT_CAT_NONE};
		struct input_device_sample sample = {.category = INPUT_CAT_NONE};
#ifdef PLATFORM_PC
		frame.connected = window != NULL;
		if(frame.connected && input_pointer_enabled) {
			double x, y;
			glfwGetCursorPos(window, &x, &y);
			frame.pointer_valid = true;
			frame.pointer_x = x;
			frame.pointer_y = y;
		}
#else
		unsigned index = owner->device.channel
			+ (owner->device.kind == INPUT_DEVICE_GAMECUBE ? 4 : 0);
		frame = device_frames[index];
		sample = device_samples[index];
		frame.connected &= device_inventory.devices[index].armed;
#endif
		/* ---- SEAM S1 (mp_chat_login_3m, docs/chat_login_profile.md 1.4) ----
		 * On an unattended console nobody owns player 0, so the chat opener and
		 * the composer are both dead. The scripted profile therefore substitutes
		 * the frame SEED and the KEY READER here -- upstream of every derivation
		 * -- and production still resolves levels and chords through
		 * input_binding_levels, still picks glyphs, and still runs the UNCHANGED
		 * input_owner_latch below, which is where pressed/down/released, the
		 * attach block and the neutral re-arm really come from. The driver
		 * supplies a button-CODE mask only.
		 *
		 * chat_login_pad_for() returns false on its first statement unless that
		 * profile is armed AND owns this player, so `read`/`user` keep today's
		 * values and every frame, level, glyph, edge and inventory decision is
		 * byte-identical for normal play and every other profile. */
		input_key_reader read = input_device_key;
		void* user = &sample;
		struct chat_login_pad chat_login_vpad;
		if(chat_login_pad_for(player, input_map, input_map_length,
							  &chat_login_vpad)) {
			frame = (struct input_player_frame) {
				.connected = chat_login_vpad.connected,
				/* PC profile codes are keyboard bindings; only the Wii
				 * virtual controller has Remote + Nunchuk codes. */
#ifdef PLATFORM_PC
				.category = INPUT_CAT_NONE};
#else
				.category = INPUT_CAT_NUNCHUK};
#endif
			read = chat_login_pad_key;
			user = &chat_login_vpad;
		}
		for(int b = 0; b < IB_COUNT; b++)
			frame.glyphs[b] = -1;
		if(frame.connected) {
			input_binding_levels(input_map, input_map_length, read, user,
								 frame.down);
#ifdef PLATFORM_WII
			/* Slice 1: only the chord made of this family's hotbar pair is
			 * order-sensitive; see input_chord_order_apply. */
			input_chord_order_apply(player, read, user, frame.category,
									frame.down);
#endif
			input_glyph_frame(&frame, input_map, input_map_length);
		}
#ifdef PLATFORM_WII
		/* Slice 2: the toggle replaces the sneak LEVEL itself, so it is also
		 * advanced on a poll where nobody is connected -- that is where the
		 * disconnect clear lands. */
		frame.down[IB_SNEAK] = input_sneak_apply(player, frame.category,
												 frame.connected,
												 frame.down[IB_SNEAK]);
#endif
		input_owner_latch(owner, &frame);
	}
}

/* Per-action edge swallow (armed by input_text_enable(false) on PC): while
 * set, a DOWN action reads as already-held (no rising edge); the first
 * polled frame that sees it UP clears it. Actions that are up when armed
 * clear on their next poll, so the swallow costs nothing there. */
static bool input_edge_swallow[IB_COUNT];

static void input_edges_swallow_held(void) {
	for(int b = 0; b < IB_COUNT; b++)
		input_edge_swallow[b] = true;
}

/* Advance the autopilot-forced latch: sample what the autopilot forces THIS
 * frame so input_pressed() can report a one-frame rising edge out of
 * autopilot_force_button()'s level. Called from input_poll BEFORE the
 * platform-specific sampling, because the Wii path returns early from three
 * places -- this way both platforms advance it exactly once per frame, the
 * same cadence as the input_action_prev advance further down. */
static void input_autopilot_edges_advance(void) {
	for(int b = 0; b < IB_COUNT; b++) {
		bool forced = false, val;
		if(gstate.autopilot.active
		   && autopilot_force_button((enum input_button)b, &val))
			forced = val;
		autopilot_force_edge_advance(input_autopilot_forced_prev,
									 (enum input_button)b, forced);
	}
}

static bool input_key_consumed(const int* consumed, size_t n, int key) {
	for(size_t k = 0; k < n; k++) {
		if(consumed[k] == key)
			return true;
	}
	return false;
}

/* Experimental local split-screen: the SECOND intentional controller claim.
 * Off by default; main.c arms it only when "experimental.local_splitscreen"
 * is on, so with the switch off input_poll below is byte-identical. */
static bool split_join_armed;
static bool split_join_claim;

void input_split_join_arm(bool armed) {
	split_join_armed = armed;
	device_inventory.split_join_armed = armed;
	if(!armed) split_join_claim = false;
}

bool input_split_join_claimed(void) {
	bool claimed = split_join_claim;
	split_join_claim = false;
	return claimed;
}

void input_split_join_settle(bool admitted) {
#ifdef PLATFORM_WII
	input_inventory_join_settle(&device_inventory, admitted, time_get());
#endif
	/* A refused admission must not leave the device holding a player slot:
	 * the approved card says "Could not join", and the controller goes back
	 * to being unassigned hardware that may try again. */
	if(!admitted) input_player_unbind(1);
}

#ifdef PLATFORM_WII
/* A new bus/channel can explicitly reclaim the only missing owner's seat.
 * Never guess between two missing owners or displace a connected controller.
 * Ordinary reconnect on the assigned channel needs no rebinding. */
static void input_split_reclaim(void) {
 if(!split_join_armed || !input_player_frame(0) || !input_player_frame(1)) return;
 unsigned missing=0,count=0;
 for(unsigned p=0;p<2;p++) {
  struct input_device d=input_owners[p].device;
  unsigned index=d.channel+(d.kind==INPUT_DEVICE_GAMECUBE?4:0);
  if(index>=INPUT_DEVICE_COUNT)return;
  enum input_category cat=device_frames[index].category;
  if(!device_frames[index].connected || (cat!=INPUT_CAT_NUNCHUK
    && cat!=INPUT_CAT_CLASSIC_CONTROLLER && cat!=INPUT_CAT_GAMECUBE)) {missing=p;count++;}
 }
 if(count!=1)return;
 for(unsigned index=0;index<INPUT_DEVICE_COUNT;index++) {
  struct input_device d={index<4?INPUT_DEVICE_WPAD:INPUT_DEVICE_GAMECUBE,index%4};
  if(input_device_equal(d,input_owners[0].device)||input_device_equal(d,input_owners[1].device))continue;
  enum input_category cat=device_frames[index].category;
  if(!device_frames[index].connected || !device_inventory.devices[index].armed
    || (cat!=INPUT_CAT_NUNCHUK&&cat!=INPUT_CAT_CLASSIC_CONTROLLER&&cat!=INPUT_CAT_GAMECUBE))continue;
  bool confirm=false;
  if(index>=4)confirm=(input_pad_buttons_down(d.channel)&PAD_BUTTON_A)!=0;
  else {
   const WPADData* data=WPAD_Data(d.channel);
   uint32_t button=cat==INPUT_CAT_CLASSIC_CONTROLLER?WPAD_CLASSIC_BUTTON_A:WPAD_BUTTON_A;
   confirm=input_wpad_fresh[d.channel]&&data&&(data->btns_d&button);
  }
  if(confirm && input_player_bind(missing,d)) {
   /* The binding press belongs to recovery, never gameplay. */
   device_inventory.devices[index].armed=false;
   return;
  }
 }
}

/* critique-1028 P1 takeover (AUDIT-GC-GLYPHS.md section 2, maintainer ask):
 * with ONLY player 1 bound (no player 2, no split seat offered), a different
 * playable pad that was seen neutral and now presses its own A becomes player
 * 1's device -- single player and online alike. input_player_bind resets the
 * owner (every button blocked until released, look blocked until neutral,
 * glyphs re-derived next poll), so the confirming A and anything held cannot
 * leak into gameplay; the old pad simply stops controlling. The inventory
 * records it silently (no connected/reconnected card, the taking pad's Join
 * offer retired). Split (split_join_armed or a bound P2) keeps Join-P2.
 *
 * ui1031/p1-takeover (maintainer, real Wii, 8b32466): `vacant` != 0 names the
 * 1-based seat whose bound device the inventory has published GONE. That seat
 * is taken regardless of split_join_armed or a bound P2 -- in game the split
 * join is armed, so the old rule let the pad's A start split as P2 while the
 * unplugged GC pad kept P1. input_poll skips the split join that frame. */
static void input_p1_takeover(unsigned vacant) {
	unsigned seat = vacant ? vacant - 1 : 0;
	if(!input_player_frame(seat) || (!vacant && input_player_frame(1))) return;
	struct input_device from = input_owners[seat].device;
	for(unsigned index = 0; index < INPUT_DEVICE_COUNT; index++) {
		struct input_device d = {index < 4 ? INPUT_DEVICE_WPAD
			: INPUT_DEVICE_GAMECUBE, index % 4};
		enum input_category cat = index < 4 ? device_samples[index].category
			: INPUT_CAT_GAMECUBE;
		bool present = index < 4 ? device_frames[index].connected
			: (pad_connected_mask & (PAD_CHAN0_BIT >> d.channel)) != 0;
		if(!present) continue;
		bool confirm;
		if(index >= 4)
			confirm = (input_pad_buttons_down(d.channel) & PAD_BUTTON_A) != 0;
		else {
			uint32_t button = cat == INPUT_CAT_CLASSIC_CONTROLLER
				? WPAD_CLASSIC_BUTTON_A : WPAD_BUTTON_A;
			confirm = input_wpad_fresh[d.channel]
				&& (device_samples[index].buttons & button);
		}
		bool armed = device_inventory.devices[index].armed;
		if(vacant ? !input_seat_takeover_eligible(input_owners,
					   INPUT_LOCAL_PLAYERS, vacant, d, armed, cat, confirm)
				  : !input_p1_takeover_eligible(input_owners,
					   INPUT_LOCAL_PLAYERS, split_join_armed, d, armed, cat,
					   confirm))
			continue;
		if(!input_player_bind(seat, d)) continue;
		/* Republish the identity now so prompts follow on the next frame;
		 * blocked[] stays all-true (input_owner_bind) until each button is
		 * seen released -- the takeover press is never a gameplay press. */
		input_owners[seat].frame.connected = true;
		input_owners[seat].frame.category = cat;
		input_inventory_takeover(&device_inventory, from, d, seat, time_get());
		return;
	}
}
#endif

void input_poll() {
	input_walker_latch(); /* T15c: before anything re-reads the frame */
	menu_page_poll++;
	menu_page_now = time_get();
	input_autopilot_edges_advance();
	input_platform_poll();
#ifdef PLATFORM_WII
	/* One tick per hardware poll: input_chat_open_edge reads this to latch its
	 * answer for the frame and to notice the frames it was NOT called on. */
	input_chat_poll_serial++;
	input_devices_poll();
	input_split_reclaim();
	device_inventory.vacant_seat = (uint8_t)input_inventory_vacant_seat(
		&device_inventory, input_owners, INPUT_LOCAL_PLAYERS);
#endif
#ifdef PLATFORM_WII
	/* Initial ownership follows an intentional Confirm press, never merely
	 * a connected pad. Subsequent attach/unplug does not reassign the player.
	 * Connection notices are independent of this explicit assignment. */
	if(!input_player_frame(0)) {
		for(unsigned channel = 0; channel < 4 && !input_player_frame(0); channel++) {
			const struct input_device_sample* sample = &device_samples[channel];
			enum input_category category = sample->category;
			uint32_t confirm = category == INPUT_CAT_CLASSIC_CONTROLLER ?
				WPAD_CLASSIC_BUTTON_A :
				WPAD_BUTTON_A;
			if(input_wpad_fresh[channel] && device_frames[channel].connected
			   && device_inventory.devices[channel].armed
			   && (sample->buttons & confirm)
			   && input_player_bind(
				   0, (struct input_device) {INPUT_DEVICE_WPAD, channel})) {
				input_owners[0].frame.connected = true;
				input_owners[0].frame.category = category;
				input_owners[0].blocked[IB_GUI_CLICK] = false;
			}
			if(!input_player_frame(0)
			   && device_inventory.devices[4 + channel].armed
			   && (pad_connected_mask & (PAD_CHAN0_BIT >> channel))
			   && (input_pad_buttons_down(channel) & PAD_BUTTON_A)
			   && input_player_bind(
				   0, (struct input_device) {INPUT_DEVICE_GAMECUBE, channel})) {
				input_owners[0].frame.connected = true;
				input_owners[0].frame.category = INPUT_CAT_GAMECUBE;
				input_owners[0].blocked[IB_GUI_CLICK] = false;
			}
		}
	}
	else input_p1_takeover(device_inventory.vacant_seat);
	/* Player 2's claim: the SAME intentional-Confirm rule as player 1's above,
	 * never a hard-coded channel and never merely a connected pad. Armed only
	 * by the experimental split-screen switch, and only once player 1 owns a
	 * device -- input_owner_bind itself refuses a device another player holds.
	 * A bare Remote is not a playable family, so it can never claim a slot.
	 * A vacant seat (P1's device published gone) makes A a takeover, never a
	 * join (ui1031/p1-takeover). */
	if(split_join_armed && input_player_frame(0) && !input_player_frame(1)
	   && !device_inventory.vacant_seat) {
		for(unsigned channel = 0; channel < 4 && !input_player_frame(1); channel++) {
			const struct input_device_sample* sample = &device_samples[channel];
			enum input_category category = sample->category;
			uint32_t confirm = category == INPUT_CAT_CLASSIC_CONTROLLER ?
				WPAD_CLASSIC_BUTTON_A :
				WPAD_BUTTON_A;
			if(input_wpad_fresh[channel] && device_frames[channel].connected
			   && device_inventory.devices[channel].armed
			   && category != INPUT_CAT_WIIMOTE
			   && (sample->buttons & confirm)
			   && input_player_bind(
				   1, (struct input_device) {INPUT_DEVICE_WPAD, channel})) {
				input_owners[1].frame.connected = true;
				input_owners[1].frame.category = category;
				input_owners[1].blocked[IB_GUI_CLICK] = false;
				split_join_claim = true;
				/* Drive the approved join card lifecycle (menus.md): the
				 * offer this device already holds becomes JOINING until the
				 * caller settles it with the REAL admission result. */
				(void)input_inventory_join_request(&device_inventory,
					(struct input_device) {INPUT_DEVICE_WPAD, channel},
					time_get());
			}
			if(!input_player_frame(1)
			   && device_inventory.devices[4 + channel].armed
			   && (pad_connected_mask & (PAD_CHAN0_BIT >> channel))
			   && (input_pad_buttons_down(channel) & PAD_BUTTON_A)
			   && input_player_bind(
				   1, (struct input_device) {INPUT_DEVICE_GAMECUBE, channel})) {
				input_owners[1].frame.connected = true;
				input_owners[1].frame.category = INPUT_CAT_GAMECUBE;
				input_owners[1].blocked[IB_GUI_CLICK] = false;
				split_join_claim = true;
				(void)input_inventory_join_request(&device_inventory,
					(struct input_device) {INPUT_DEVICE_GAMECUBE, channel},
					time_get());
			}
		}
	}
#endif

	for(int b = 0; b < IB_COUNT; b++) {
		size_t length = INPUT_MAX_BINDINGS;

		input_map_length[b] = input_read_bindings((enum input_button)b,
												input_map[b], &length) ?
			length :
			0;
	}
#ifdef PLATFORM_PC
	/* Edge the mouse sources separately: a held printable alternative must
	 * neither click on a later text frame nor hide a fresh real mouse press.
	 * Poll even outside text mode so opening a field cannot invent an edge. */
	bool mouse_down[IB_COUNT];
	input_binding_levels(input_map, input_map_length, input_mouse_key, NULL,
		mouse_down);
	for(int b = 0; b < IB_COUNT; b++) {
		input_mouse_pressed[b] = mouse_down[b] && !input_mouse_down[b];
		input_mouse_down[b] = mouse_down[b];
	}
#endif
	input_players_poll();
	#ifdef PLATFORM_WII
	/* Edge-triggered from the debounced state above, once per frame: every
	 * notice raised by this poll gets exactly one cue decision. */
	controller_events_announce(&device_inventory, time_get());
	const struct input_player_frame* primary = input_player_frame(0);
	if(primary) {
		for(int b = 0; b < IB_COUNT; b++) {
			input_action_down[b] = primary->down[b];
			input_action_prev[b] = primary->down[b] ? !primary->pressed[b] : primary->released[b];
			if(input_edge_swallow[b]) {
				if(primary->down[b]) input_action_prev[b] = true;
				else input_edge_swallow[b] = false;
			}
		}
		return;
	}
	memset(input_action_down, 0, sizeof(input_action_down));
	memset(input_action_prev, 0, sizeof(input_action_prev));
	return;
#endif

	// Pass 1: a chord that is fully down consumes both of its component keys, so
	// holding MINUS+PLUS for the overlay chord does not also scroll the hotbar.
	int consumed[INPUT_MAX_CONSUMED];
	size_t consumed_count = 0;

	for(int b = 0; b < IB_COUNT; b++) {
		for(size_t k = 0; k < input_map_length[b]; k++) {
			int hold_key, press_key;
			if(!input_combo_decode(input_map[b][k], &hold_key, &press_key))
				continue;
			if(!input_native_key_down(hold_key)
			   || !input_native_key_down(press_key))
				continue;
			if(consumed_count + 2 <= INPUT_MAX_CONSUMED) {
				consumed[consumed_count++] = hold_key;
				consumed[consumed_count++] = press_key;
			}
		}
	}

	// Pass 2: OR the sources together, then edge the result exactly once.
	for(int b = 0; b < IB_COUNT; b++) {
		bool down = false;

		for(size_t k = 0; k < input_map_length[b] && !down; k++) {
			int hold_key, press_key;
			if(input_combo_decode(input_map[b][k], &hold_key, &press_key)) {
				down = input_native_key_down(hold_key)
					&& input_native_key_down(press_key);
			} else {
				down = input_native_key_down(input_map[b][k])
					&& !input_key_consumed(consumed, consumed_count,
										   input_map[b][k]);
			}
		}

		input_action_prev[b] = input_action_down[b];
		input_action_down[b] = down;

		/* Swallowed action: pretend it was already held so no rising edge
		 * fires; the first UP frame re-arms it (see input_edge_swallow). */
		if(input_edge_swallow[b]) {
			if(down)
				input_action_prev[b] = true;
			else
				input_edge_swallow[b] = false;
		}
	}
}

bool input_pressed(enum input_button b) {
#ifdef CAVEWII_AGENT_BRIDGE
	// CAVEWII_AGENT bridge owns the action buttons when armed. The rising edge is
	// frame-latched (idempotent within a frame), so the place/use/bow code that reads
	// this several times per frame still fires exactly once -- the must-fix that forcing
	// input_held alone could not satisfy.
	{
		bool agent_val;
		if(agent_input_pressed(b, &agent_val))
			return agent_val;
	}
#endif

	// menus_dwell hardware profile (armed only via hw_run.menu_autopilot):
	// synthesizes the GUI_*/HOME/INVENTORY presses that drive the title/select-
	// world/texture-packs/create-world/pause/options screens with no human input.
	// A no-op (never returns true) unless menu_autopilot_arm() was called.
	{
		bool menu_val;
		if(menu_autopilot_force_button(b, &menu_val))
			return menu_val;
	}

	// Autopilot (debug.autopilot=on) owns the buttons it forces here the same way
	// it owns them in input_held -- but as a PRESS, i.e. the single rising edge of
	// that forced level (autopilot.h). Without this hook a forced button was held
	// forever and never pressed, so screen_death's Respawn hold was never seeded
	// and a walker that died stayed dead (real-Wii lan_walk_0912). Placed after
	// the menus_dwell hook so that profile (which runs the autopilot in aim mode,
	// where autopilot_force_button forces nothing) is byte-identical.
	if(gstate.autopilot.active) {
		bool forced;
		if(autopilot_force_button(b, &forced))
			return autopilot_force_edge_pressed(input_autopilot_forced_prev, b,
												forced);
	}

	// Rising edge of the merged mask, latched by input_poll: stable no matter how
	// many consumers ask, or in what order, within one frame.
	return b >= 0 && b < IB_COUNT && input_action_down[b]
		&& !input_action_prev[b];
}

#ifdef PLATFORM_WII
/* b1-chat-opener-0919 (maintainer console report 2026-09-19, Astra gate 2 D2
 * plus the maintainer's 13:5x amendment). The two openers that cannot be a
 * plain press edge, and the window/partner each needs -- input_tap_gesture.h
 * carries the full rationale:
 *     7  Wii Remote 2   TAP,     no partner: a HELD 2 is the look recenter.
 *   212  Classic Plus   RELEASE, partner 213: Plus+Minus is the debug chord.
 * Everything else -- GameCube X, a remapped button, an explicitly configured
 * legacy D-pad Up -- keeps the press edge it has today. */
/* Astra ruling 4 D1: 212 is no longer the SHIPPED Classic opener (that is
 * D-pad Up, a plain press edge), but the rule stays -- a config that
 * explicitly binds chat to Plus still gets the release gesture and the
 * Minus cancel, because on that pad Plus is still half of the F3 chord. */
static bool input_chat_opener_rule(int code, int* partner, uint32_t* hold_ms) {
	switch(code) {
		case 7: *partner = 0; *hold_ms = INPUT_CHAT_TAP_MS; return true;
		case 212: *partner = 213; *hold_ms = 0; return true;
		default: return false;
	}
}

/* The OWNING device's physical level for one code. The scripted
 * mp_chat_login_3m pad answers first, exactly as it does inside
 * input_native_key_down (SEAM S1, site 2) -- that is what keeps this gate
 * exercisable with nobody at the console. The hardware read below is the
 * owner's own channel rather than chan 0, because the gesture belongs to the
 * owning device and a returning Remote may hold a different channel. */
static bool input_chat_key_down(unsigned index, int code) {
	bool value;
	if(chat_login_pad_native_key(code, &value))
		return value;
	return index < INPUT_DEVICE_COUNT
		&& input_device_key(&device_samples[index], code);
}

/* Decision item 3 (docs/mp_chat_auth_research_2026-09-11.md), kept for the
 * configs that explicitly ask for the legacy D-pad Up binding: a held Left or
 * Right on that SAME D-pad at the press instant refuses the opening. A
 * refused press needs a fresh Up press -- nothing is synthesized when the
 * Left/Right is released. */
static bool input_chat_dpad_rocked(unsigned index, int code) {
	int left, right;
	if(code == 0) {
		left = 2, right = 3; /* Wii Remote D-pad */
	} else if(code == 200) {
		left = 202, right = 203; /* Classic Controller D-pad */
	} else if(code == 400) {
		left = 402, right = 403; /* GameCube D-pad */
	} else
		return false;
	return input_chat_key_down(index, left) || input_chat_key_down(index, right);
}
#endif

bool input_chat_open_edge(void) {
#ifndef PLATFORM_WII
	return input_pressed(IB_CHAT);
#else
	const struct input_player_frame* owner = input_player_frame(0);
	bool connected = owner && owner->connected;
	/* Bindings are evaluated in the OWNING family only, which is the same
	 * filter input_binding_levels used to produce the action level itself --
	 * so a GameCube pad's D-pad can never refuse a Wii Remote's opener. */
	enum input_category family = connected ? owner->category : INPUT_CAT_NONE;
	struct input_device device = {INPUT_DEVICE_NONE, 0};
	(void)input_player_device(0, &device);
	unsigned index = device.channel
		+ (device.kind == INPUT_DEVICE_GAMECUBE ? 4u : 0u);

	/* 1. the gesture bindings: at most one of them is this family's opener. */
	int opener = 0, partner = 0;
	uint32_t hold_ms = 0;
	for(size_t k = 0; k < input_map_length[IB_CHAT] && !opener; k++) {
		int code = input_map[IB_CHAT][k], mate = 0;
		uint32_t hold = 0;
		if(input_owner_accepts(family, code)
		   && input_chat_opener_rule(code, &mate, &hold)) {
			opener = code;
			partner = mate;
			hold_ms = hold;
		}
	}
	/* Hitch tolerance 0: Astra ruling 4 D2 leaves the CHAT tap on the pure
	 * wall-clock rule -- tapping 2 again is a free retry, so trading a
	 * possible false open across a stall for it would be a bad bargain. */
	if(input_tap_gesture_tick(&input_chat_open_state, input_chat_poll_serial,
							   (int)device.kind, device.channel, opener,
							   hold_ms, 0u, connected,
							   opener && input_chat_key_down(index, opener),
							   partner && input_chat_key_down(index, partner),
							   (uint32_t)time_diff_ms(input_chat_epoch,
													  time_get())))
		return true;

	/* 2. everything else keeps today's press edge. */
	if(!input_pressed(IB_CHAT))
		return false;
	bool physical = false;
	for(size_t k = 0; k < input_map_length[IB_CHAT]; k++) {
		int code = input_map[IB_CHAT][k], mate = 0;
		uint32_t hold = 0;
		if(!input_owner_accepts(family, code)
		   || !input_chat_key_down(index, code))
			continue;
		physical = true;
		if(input_chat_opener_rule(code, &mate, &hold))
			continue; /* owned by the gesture above: never a press edge */
		if(input_chat_dpad_rocked(index, code))
			continue;
		return true;
	}
	if(physical)
		return false;
	/* Review finding 3: FAIL CLOSED. An edge with no physical key of the
	 * owning family behind it used to pass through, which meant an UNOWNED or
	 * disconnected player 0 -- family INPUT_CAT_NONE, every code rejected --
	 * opened the composer on any latched edge. The only legitimate
	 * code-less sources are the two that force the ACTION itself, and they
	 * are named here rather than assumed: the headless autopilot and the
	 * CAVEWII_AGENT bridge. Anything else is unexplained and stays shut. */
	{
		bool value = false;
#ifdef CAVEWII_AGENT_BRIDGE
		if(agent_input_pressed(IB_CHAT, &value) && value)
			return true;
#endif
		if(menu_autopilot_force_button(IB_CHAT, &value) && value)
			return true;
		if(gstate.autopilot.active
		   && autopilot_force_button(IB_CHAT, &value) && value)
			return true;
	}
	return false;
#endif
}

/* b1-chat-opener-0919/14b H-1. A pure READ of the binding tables -- no poll,
 * no edge, no gesture state, nothing written. See input.h for the contract. */
bool input_player_chat_opener(unsigned player, int* code, bool* is_tap) {
	if(!code || !is_tap)
		return false;
#ifndef PLATFORM_WII
	/* A keyboard is one family with no gesture, and only player 0 has one --
	 * the same restriction input_prompt_frame's PC branch already makes. */
	if(player != 0 || !input_map_length[IB_CHAT])
		return false;
	*code = input_map[IB_CHAT][0];
	*is_tap = false;
	return true;
#else
	const struct input_player_frame* owner = input_player_frame(player);
	if(!owner)
		return false;
	int resolved = 0;
	/* The SAME resolution input_hotbar_chord_pair uses: this family's first
	 * accepted code out of the already-resolved map (config first, the
	 * shipped fallback second), never a literal repeated here. */
	if(!input_family_code(owner->category, IB_CHAT, &resolved))
		return false;
	int partner = 0;
	uint32_t hold_ms = 0;
	*code = resolved;
	*is_tap = input_chat_opener_rule(resolved, &partner, &hold_ms);
	return true;
#endif
}

#ifndef PLATFORM_WII
/* PC keeps Beta's held Shift: a keyboard has no finger shortage, so the
 * carve-out that justifies the toggle does not apply. Both entry points exist
 * so the in-game screen and the local-player tick compile unchanged. */
void input_sneak_gameplay_tick(unsigned player, bool dead, bool resting) {
	(void)player;
	(void)dead;
	(void)resting;
}

void input_sneak_vehicle_mark(unsigned player, bool riding) {
	(void)player;
	(void)riding;
}

/* No toggle on PC, so nothing is ever latched (input.h). */
bool input_sneak_toggle_latched(unsigned player) {
	(void)player;
	return false;
}

void input_sneak_toggle_release(unsigned player) {
	(void)player;
}
#endif

bool input_hotbar_step_edge(unsigned player,
							const struct input_player_frame* frame,
							enum input_button b) {
	if(b != IB_SCROLL_LEFT && b != IB_SCROLL_RIGHT)
		return false;
	/* The PLAIN press edge this gate degrades to, for the player that asked --
	 * byte-for-byte what screen_ingame's act_pressed(slot, frame, b) answered
	 * before this gate existed. Player 0 keeps the merged mask (the autopilot
	 * and agent hooks live there, and its caller passes no frame); any other
	 * local player reads THE FRAME IT WAS GIVEN, because the whole owner-routed
	 * action pass acts on that frame and the hotbar must not be the one branch
	 * that goes behind its back. NULL falls back to that player's own latched
	 * frame, which is what main.c hands in anyway. */
	bool plain = player == 0
		? input_pressed(b)
		: (frame ? input_frame_pressed(frame, b)
				 : input_pressed_owned(player, b));
#ifndef PLATFORM_WII
	return plain;
#else
	if(player >= INPUT_LOCAL_PLAYERS)
		return plain;
	/* Review 09 finding 1: THIS player's owner frame, device and family --
	 * never player 0's. A guest holding the second Remote gets the same
	 * release-tap rule Astra D2 ruled for the host, on its own buttons. */
	const struct input_player_frame* owner = frame ? frame
												   : input_player_frame(player);
	bool connected = owner && owner->connected;
	enum input_category family = connected ? owner->category : INPUT_CAT_NONE;
	int prev = 0, next = 0;
	if(!input_hotbar_chord_pair(family, &prev, &next))
		/* Not the double-duty pair (Classic full L/R, GameCube D-pad L/R, an
		 * explicitly configured legacy D-pad hotbar, or nobody owning this
		 * player): today's plain press edge, byte-for-byte. The gestures are
		 * left un-advanced, so the poll gap their scope already watches for
		 * clears whatever was in flight. */
		return plain;

	struct input_device device = {INPUT_DEVICE_NONE, 0};
	(void)input_player_device(player, &device);
	unsigned index = device.channel
		+ (device.kind == INPUT_DEVICE_GAMECUBE ? 4u : 0u);
	unsigned slot = b == IB_SCROLL_RIGHT ? 1u : 0u;
	int opener = slot ? next : prev, partner = slot ? prev : next;
	/* Physical levels, never action levels: input_binding_levels CONSUMES both
	 * halves while the chord is down, which a gesture reading actions would
	 * mistake for a release and fire on (the same trap input_chat_open_edge
	 * documents). INPUT_HOTBAR_HITCH_POLLS is Astra ruling 4 D2's stall
	 * tolerance -- see input_tap_gesture.h for what it costs. */
	return input_tap_gesture_tick(&input_hotbar_state[player][slot],
								  input_chat_poll_serial, (int)device.kind,
								  device.channel, opener, INPUT_HOTBAR_TAP_MS,
								  INPUT_HOTBAR_HITCH_POLLS, connected,
								  input_chat_key_down(index, opener),
								  input_chat_key_down(index, partner),
								  (uint32_t)time_diff_ms(input_chat_epoch,
														 time_get()));
#endif
}

bool input_released(enum input_button b) {
#ifdef CAVEWII_AGENT_BRIDGE
	{
		bool agent_val;
		if(agent_input_released(b, &agent_val))
			return agent_val;
	}
#endif

	{
		bool down, released;
		if(input_walker_level(b, &down, &released))
			return released;
	}

	// Falling edge of the merged mask: fires only once EVERY source has cleared.
	// (This path used to skip chord decoding entirely -- unlike pressed/held -- so a
	// chord binding could never contribute a release at all.)
	return b >= 0 && b < IB_COUNT && !input_action_down[b]
		&& input_action_prev[b];
}

bool input_held(enum input_button b) {
#ifdef CAVEWII_AGENT_BRIDGE
	// CAVEWII_AGENT bridge owns the movement/action buttons when armed (CAVEWII_AGENT=1).
	// Gated on the agent flag, NOT gstate.autopilot.active, so the two rigs never collide.
	{
		bool agent_val;
		if(agent_input_held(b, &agent_val))
			return agent_val;
	}
#endif

	// Autopilot (debug.autopilot=on) forces the movement buttons for headless testing.
	if(gstate.autopilot.active) {
		bool forced;
		if(autopilot_force_button(b, &forced))
			return forced;
	}

	/* After the autopilot hook, so its aim mode is unchanged (T15c). */
	{
		bool down, released;
		if(input_walker_level(b, &down, &released))
			return down;
	}

	// Level of the merged mask. As before, "held" includes the frame the action
	// goes down (the old rule returned any_pressed || any_held).
	return b >= 0 && b < IB_COUNT && input_action_down[b];
}

/* S1-b owner-routed action edges. The merged-mask functions above stay
 * BYTE-IDENTICAL (player 0's autopilot/agent hooks and the Wii primary-only
 * mask feed are untouched, see the input-merged-mask note); these read the
 * per-owner latched frame instead, which input_owner_latch already edges with
 * exactly the merged mask's rule -- pressed = down && !was_down,
 * released = !down && was_down, held = down -- plus the attach/reconnect
 * blocking that keeps a disconnect from synthesizing an edge. No autopilot or
 * agent hook: those drive player 0 only, and a split guest is never scripted.
 * A frame that belongs to nobody (NULL owner frame) has every action up. */
bool input_frame_pressed(const struct input_player_frame* frame,
						 enum input_button b) {
	return frame && b >= 0 && b < IB_COUNT && frame->pressed[b];
}

bool input_frame_released(const struct input_player_frame* frame,
						  enum input_button b) {
	return frame && b >= 0 && b < IB_COUNT && frame->released[b];
}

bool input_frame_held(const struct input_player_frame* frame,
						  enum input_button b) {
	return frame && b >= 0 && b < IB_COUNT && frame->down[b];
}

bool input_pressed_owned(unsigned player, enum input_button b) {
	return input_frame_pressed(input_player_frame(player), b);
}

bool input_released_owned(unsigned player, enum input_button b) {
	return input_frame_released(input_player_frame(player), b);
}

bool input_held_owned(unsigned player, enum input_button b) {
	return input_frame_held(input_player_frame(player), b);
}

bool input_joystick(float dt, float* x, float* y) {
	SERVICE_OBSERVER(service_observer_input(0, 1, input_owners));
	input_native_joystick(dt, x, y);
	input_primary_look_apply(x, y);
	return true;
}

void input_player_look(unsigned player, float dt, float* dx, float* dy) {
	SERVICE_OBSERVER(service_observer_input(player, 1, input_owners));
	if(!dx || !dy) return;
	if(player == 0) {
		/* Preferences transform the completed native delta exactly once. */
		input_native_joystick(dt, dx, dy);
		input_primary_look_apply(dx, dy);
		return;
	}
	*dx = *dy = 0;
	if(player >= INPUT_LOCAL_PLAYERS) return;
	const struct input_player_frame* owner = input_player_frame(player);
	if(!owner || !owner->connected) {
#ifdef PLATFORM_WII
		owned_ir_look[player] = (struct input_ir_look_state) {0};
#endif
		return;
	}
#ifdef PLATFORM_WII
	/* Wiimote + Nunchuk is the floor (CLAUDE.md): that family looks with the
	 * IR pointer, so a second player holding one gets the same integrator the
	 * primary does -- resolved through ITS OWN owned channel. */
	struct input_device device;
	if(wiimote_ir_look && owner->category == INPUT_CAT_NUNCHUK
	   && input_player_device(player, &device)
	   && device.kind == INPUT_DEVICE_WPAD) {
		input_ir_look(&owned_ir_look[player], owner->pointer_valid,
			(WPAD_ButtonsHeld(device.channel) & WPAD_BUTTON_2) != 0,
			2 * owner->pointer_x - 1, 1 - 2 * owner->pointer_y, dt, dx, dy);
		return;
	}
#endif
	/* Match the WPAD neutral gate. A measured Classic resting bias of
	 * -0.125 was admitted as neutral but exceeded the old 0.1 look gate. */
	if(hypotf(owner->look_x, owner->look_y) > .2F) {
		*dx = owner->look_x * dt;
		*dy = owner->look_y * dt;
	}
}
