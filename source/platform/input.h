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

#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>

/* Approved Player 1 camera preferences. Level 3/non-inverted preserves the
 * native response exactly. These never affect menu pointers or Player 2. */
int input_primary_look_sensitivity(void);
bool input_primary_look_inverted(void);
bool input_primary_look_set(int sensitivity, bool inverted);
void input_primary_look_reload(void);
void input_primary_look_apply(float* x, float* y);
/* Soft-snap melee assist level (controls.attack_assist: 0 Off, 1 Low,
 * 2 Normal; docs/deviations.md "Soft-snap melee assist"). Off by default;
 * reloaded with the look preferences above and applied to every local pane. */
int input_attack_assist_level(void);
bool input_attack_assist_set(int level);

enum input_button {
	IB_FORWARD,
	IB_BACKWARD,
	IB_LEFT,
	IB_RIGHT,
	IB_ACTION1,
	IB_ACTION2,
	IB_JUMP,
	IB_SNEAK,
	IB_INVENTORY,
	IB_HOME,
	IB_SCROLL_LEFT,
	IB_SCROLL_RIGHT,
	IB_GUI_UP,
	IB_GUI_DOWN,
	IB_GUI_LEFT,
	IB_GUI_RIGHT,
	IB_GUI_CLICK,
	IB_GUI_CLICK_ALT,
	IB_SCREENSHOT,
	IB_DEBUG_OVERLAY,
	IB_DROP, // Beta 'Q': drop one of the held hotbar item into the world
	IB_GUI_SPLIT, // C / X: take half or place one in a container
	IB_GUI_DROP, // explicit cursor-stack drop, never Back
	IB_GUI_PAGE, // next storage page; does not move any items
	IB_GUI_PREVIOUS, // previous fixed container view
	IB_GUI_QUICK_MOVE, // server-owned transfer of the selected stack

	IB_CHAT, // gameplay: open the multiplayer chat/command composer (remote
			 // sessions only; Wii Remote 2 / Classic Plus / GameCube X since
			 // b1-chat-opener-0919 -- docs/wii_controller_playability.md #23)

	// gameplay: OPEN the inventory (or, in creative, the picker). Split out of
	// IB_INVENTORY by b1-chat-opener-0919/07 C, because that one role meant
	// BOTH this and "Close / Back / Cancel in a screen" -- 24 behavioural
	// reads across 13 files mean the second. The maintainer's CavEX layout
	// puts the gameplay open on Remote A, while the approved 2026-09-10
	// matrix keeps Close on Remote 1 with A as Select, so one role could not
	// carry both: a single A press in a chest would pick a slot AND close the
	// window. IB_INVENTORY therefore KEEPS every one of its Close call sites
	// and its codes; only the four "open" readers moved here (screen_ingame,
	// main.c's creative picker, the split guest's menu host, and the
	// menus_dwell walker's INVENTORY_OPEN step).
	//
	// `input.open_inventory`, shipped [4, 206, 407]. When a config omits the
	// key it falls back to THAT CONFIG'S OWN `input.inventory` codes, so every
	// existing config -- and the live card -- behaves exactly as it does
	// today. Added last so no existing enum value moves.
	IB_OPEN_INVENTORY,
	IB_GUI_PAGE_SWAP, // menu-only tap/hold control, separate from Previous/Next
	IB_COUNT, // sentinel: number of input buttons (the CAVEWII_AGENT bridge sizes its
			  // per-button latch arrays by this -- keep it last)
};

enum input_category {
	INPUT_CAT_WIIMOTE,
	INPUT_CAT_NUNCHUK,
	INPUT_CAT_CLASSIC_CONTROLLER,
	INPUT_CAT_GAMECUBE,
	INPUT_CAT_NONE,
};
/* critique-1028 glyph-proof hook: the controller family the menus walker
 * presents on player 1's prompts (Nunchuk default; GameCube / Classic via
 * debug.autopilot_device, set by menu_autopilot_arm). Presentation only. */
void input_walker_family_set(enum input_category family);

void input_init(void);
void input_poll(void);

// Raw WPAD_Probe(chan 0) code for telemetry (Wii builds; PC has no WPAD and
// reports the -99 sentinel from the heartbeat fill site instead). See
// input.c's comment: NOT_READY-forever vs NO_CONTROLLER separates a stuck
// Bluetooth enable from a healthy idle stack, no human required.
int input_wpad_probe0(void);

// Raw WPAD held-button mask, chan 0 (Wii; PC uses the overlay's sentinel
// path). Overlay diagnosis companion to input_wpad_probe0.
unsigned int input_wpad_raw_held0(void); // uint32_t-wide on both targets

// Raw WPADData diagnostics, chan 0 (Wii only): err code, data_present
// bitmask, battery_level (nonzero only after >=1 status report ever
// arrived). Overlay companions to the probe/held readouts.
void input_wpad_raw_diag0(int* err, unsigned int* data_present,
						  unsigned int* battery);

// Free bytes in the lwp kernel workspace (Wii): the pool wiiuse's attach-time
// queue allocation draws from. 0xFFFFFFFF = query failed.
unsigned int input_lwp_wkspace_free(void);
void input_wpad_shutdown0(void);
void input_lwp_wkspace_stats(unsigned int* total, unsigned int* used,
                             unsigned int* free_blocks);

bool input_symbol(enum input_button b, int* symbol, int* symbol_help,
				  enum input_category* category);

// Pure analog-stick normalizer: (x, y) is a signed raw reading centered at 0 with
// travel of +-range on each axis (e.g. a GameCube PADStatus.stickX/Y, s8, range
// ~128). Returns a unit DIRECTION (dx, dy) plus a separate deflection MAGNITUDE
// clamped to [0, 1] -- the same (dx, dy, magnitude) shape the Wii input.c
// joystick_input[] table already uses for the Nunchuk/Classic sticks (there,
// wiiuse hands back polar coordinates directly; this is the equivalent for a
// plain signed-cartesian source like libogc's PAD_StickX/PAD_StickY). At the
// center (magnitude ~0) dx/dy are 0 rather than an undefined unit vector.
// No platform headers -- callable from the PC test suite (tests/test_input_edges.c)
// without linking wiiuse or libogc. Production caller: input.c's PLATFORM_WII
// input_platform_poll (GameCube PAD_Init/PAD_ScanPads path).
void input_stick_normalize(int x, int y, int range, float* dx, float* dy,
						   float* magnitude);

/* Controller feel only: normalized IR coordinates, positive Y upward.
 * Caller owns tracking/filter state. Lost/invalid tracking stops immediately. */
struct input_ir_look_state {
	float x, y, center_x, center_y;
	unsigned valid_frames;
};
void input_ir_look(struct input_ir_look_state* s, bool valid, bool recenter,
	float x, float y, float dt, float* dx, float* dy);

bool input_pressed(enum input_button b);
bool input_released(enum input_button b);
bool input_held(enum input_button b);
/* S1-b owner-routed action edges: the same pressed/released/held semantics
 * read from ONE owner's latched frame (input_owner.h) instead of the merged
 * mask. The three functions above are unchanged and remain player 0's path --
 * they keep the autopilot/agent hooks and, on Wii, the primary-only mask feed.
 * The _owned forms resolve the frame through input_player_frame(player) and
 * report every action up when that player owns no device; the _frame forms
 * take an explicitly supplied frame, which is what the owner-routed in-game
 * action path passes down (and what a test scripts). */
struct input_player_frame;
/* One owner's menu gesture, latched for the input poll. Time uses the same
 * platform frame clock as the other input gestures. HOLD has no action here. */
#define INPUT_MENU_PAGE_HOLD_MS 800u
struct input_menu_page {
	bool pressed, held, released, page_swap, hold;
	unsigned held_ms;
};
void input_menu_page_read(unsigned player, const struct input_player_frame*,
	struct input_menu_page*);
void input_menu_page_cancel(unsigned player);
bool input_frame_pressed(const struct input_player_frame* frame,
						 enum input_button b);
bool input_frame_released(const struct input_player_frame* frame,
						  enum input_button b);
bool input_frame_held(const struct input_player_frame* frame,
					  enum input_button b);
bool input_pressed_owned(unsigned player, enum input_button b);
bool input_released_owned(unsigned player, enum input_button b);
bool input_held_owned(unsigned player, enum input_button b);
/* The composer's opener gate. Call it ONCE per frame from the in-game screen:
 * the answer is latched per hardware poll, and a frame the gate is NOT called
 * on (another screen owned it) cancels a gesture in progress.
 *
 * Wii (b1-chat-opener-0919, maintainer console report 2026-09-19): a binding
 * that shares its button with something else is a gesture rather than a press
 * edge -- Wii Remote 2 (code 7) opens on the RELEASE of a TAP shorter than
 * INPUT_CHAT_TAP_MS, because a HELD 2 is the pointer-look recenter; an
 * explicitly configured Classic Plus (212) opens on its RELEASE and is
 * cancelled by any overlap with Minus (213), because that pair is the
 * debug-overlay chord. The SHIPPED Classic opener is D-pad Up (200, Astra
 * ruling 4 D1) -- a plain press edge, so it works while Minus-sneak is held.
 * GameCube X, a remapped button and a legacy D-pad Up keep the plain press
 * edge too, Up still refused while Left or Right rocks the same cluster
 * (decision item 3 of docs/mp_chat_auth_research_2026-09-11.md).
 * PC returns the plain edge. See source/platform/input_tap_gesture.h. */
bool input_chat_open_edge(void);
/* READ-ONLY: which code opens chat for the family that OWNS `player`, and
 * whether it opens through the gesture rule rather than a plain press edge.
 * Presses nothing, consumes nothing, advances no gesture and touches no
 * state -- it exists so a SCREEN can word its chat hint from the effective
 * binding without duplicating input.c's fallback literals or re-reading the
 * config for itself (b1-chat-opener-0919/14b, lead ruling 1).
 *
 * Wii: resolves through the same family-code lookup the hotbar chord pair
 * uses -- this family's first accepted `input.chat` code, config first and
 * the shipped fallback second. `*is_tap` is true exactly when
 * input_chat_open_edge would run that code through its GESTURE (Wii Remote 2
 * and an explicitly configured Classic Plus): both of those open on the
 * RELEASE of a completed press, which is what a player has to be told, while
 * a plain press edge (the shipped Classic/GameCube D-pad Up, any remap) opens
 * on the way down. PC: the keyboard binding, never a tap, player 0 only --
 * the same rule input_prompt_frame's PC branch applies to prompts.
 *
 * Returns false, leaving both out-parameters untouched, when nobody owns
 * `player` or its family binds no acceptable chat code. */
bool input_player_chat_opener(unsigned player, int* code, bool* is_tap);
/* The hotbar's step gate, for IB_SCROLL_LEFT / IB_SCROLL_RIGHT, PER LOCAL
 * PLAYER. Call BOTH once per frame per player from the in-game screen, like
 * the opener above: the answer is latched per hardware poll and a frame the
 * gates are not called on cancels a gesture in progress. `player` resolves its
 * OWN owner frame, device and family, so the split guest gets the same rule on
 * its own Remote (review 09 finding 1) instead of player 0's buttons. `frame`
 * is the owner-routed action pass's own frame -- the same one act_pressed()
 * reads, so the hotbar is not the one branch that goes behind the caller's
 * back; NULL (player 0's caller, and any caller with no frame of its own)
 * means "that player's latched frame", which is what main.c hands down.
 *
 * Wii, CavEX default scheme (maintainer 2026-09-19 "I agree, cavex layout";
 * Astra ruling 3 D2): where a family binds the hotbar to the SAME two codes
 * that form a configured chord with each other -- Wii Remote Minus (9) and
 * Plus (8), which are also the F3 chord 9008 -- a press edge would move a slot
 * on every overlay toggle. There the step fires on the RELEASE of a clean tap:
 * one no longer than INPUT_HOTBAR_TAP_MS, OR one the game only ever saw down
 * on INPUT_HOTBAR_HITCH_POLLS polls or fewer (Astra ruling 4 D2's stall
 * tolerance -- across a hitch either a tap or a real hold may therefore
 * produce ONE release step; an OBSERVED long hold never steps, not even
 * late). Any overlap of the two cancels BOTH pending steps, and both must be
 * seen neutral before either re-arms; there is no hold-to-repeat. Every other
 * binding -- Classic full L/R, GameCube D-pad L/R, an explicitly configured
 * legacy D-pad hotbar -- keeps today's plain press edge. The rule is derived
 * from the bindings plus the chord table, not from the literal codes. PC
 * returns the plain edge. */
bool input_hotbar_step_edge(unsigned player,
							const struct input_player_frame* frame,
							enum input_button b);

/* ---- the Wii Remote SNEAK TOGGLE (Astra ruling 3 D3) --------------------
 * In the maintainer's CavEX layout there is no finger left to HOLD sneak
 * while the thumb walks the D-pad, so on the Wii Remote + Nunchuk family
 * sneak is a toggle -- a carve-out #2 deviation from Beta's held Shift,
 * opt-out through `controls.wiimote_sneak_toggle: 0`. Classic, GameCube and
 * PC keep the hold, where a shoulder button or a key is free while moving.
 *
 * The toggled level is substituted into the ORDINARY IB_SNEAK level, so every
 * consumer (entity_local_player.c's two movement paths, camera.c,
 * client_interface.c's two sneak reports) sees exactly what a held button
 * would have given it, with no change.
 *
 * Call input_sneak_gameplay_tick ONCE PER FRAME, per local player, from the
 * in-game screen -- and only from there. A frame it is NOT called on is a
 * frame some other screen owned: the toggle is PRESERVED across it (opening
 * the inventory, chat or pause must not silently remove ledge protection) but
 * its input is suppressed, and coming back costs a neutral frame, because
 * Remote 1 is both the menu Close and the sneak button. `dead` and `resting`
 * are levels, not edges: the machine takes the death/respawn change and the
 * vehicle/bed ENTRY edge itself. input.c owns the remaining clears Astra D3
 * lists -- world/session leave or replacement, controller disconnect, and an
 * owner or family change. (D3 also lists a scheme-mode change; `controls.*`
 * is read ONCE at boot in input_init and never re-read, so that clear is
 * satisfied because the change cannot happen at run time -- review 09
 * finding 5.) */
void input_sneak_gameplay_tick(unsigned player, bool dead, bool resting);
/* The RIDING LEVEL, published by the 20 Hz client tick that already knows
 * whether this player is aboard -- entity_local_player.c's pig, cart and boat
 * branches pass true, and the walking path that follows them passes false.
 * BOTH edges must be published, and the level is STICKY in between: input.c
 * does NOT consume it per frame poll, because the pure machine takes the
 * ENTRY edge itself. That is what lets a rider tap sneak and KEEP it for the
 * whole ride (review 09 finding 2 -- a per-poll consume re-fired the entry
 * edge on every tick-frame and made the toggle unreachable in a boat), while
 * a dismount and a fresh boarding still clear it exactly once. */
void input_sneak_vehicle_mark(unsigned player, bool riding);
/* READ-ONLY: is THAT player's sneak TOGGLE currently latched on. Presses
 * nothing, consumes nothing, advances no machine and touches no state -- it
 * exists so the in-game HUD can show the marker Astra ruling 6 Q2 requires
 * without a second "is sneaking" rule growing beside input_sneak_apply's.
 *
 * True ONLY for a latched toggle. A HELD non-toggle sneak is false, on every
 * family and on every opt-out path: input_sneak_apply zeroes this machine on
 * the frame a family is not running the toggle (an explicit
 * controls.*_sneak_toggle = 0, a bare Remote, a disconnect, a world or owner
 * change), so `engaged` can only be true while the toggle itself owns the
 * level. PC keeps Beta's held Shift and always answers false. */
bool input_sneak_toggle_latched(unsigned player);
/* Release this player's toggle and require a neutral frame before rearming.
 * PC's held Shift is unaffected. */
void input_sneak_toggle_release(unsigned player);
bool input_joystick(float dt, float* x, float* y);
/* Per-player LOOK delta for THIS frame. Player 0 is exactly input_joystick
 * (byte-identical legacy path); any other player resolves its look from its
 * OWN latched owner frame and never reads another player's device. Zero when
 * that player owns nothing. Experimental local split-screen uses this for the
 * guest camera; nothing else calls it. */
void input_player_look(unsigned player, float dt, float* dx, float* dy);
/* Experimental local split-screen: arm the SECOND intentional controller
 * claim. While armed, an armed/usable device that owns no player and presses
 * its Confirm button claims player 1 (Wii only -- PC has one keyboard). OFF
 * by default; main.c arms it from "experimental.local_splitscreen". */
void input_split_join_arm(bool armed);
/* True once this frame's poll accepted such a claim; consumed by the caller
 * that drives the Join handshake, so a claim is acted on exactly once. */
bool input_split_join_claimed(void);
/* Settle the claim with the REAL admission result: true retires the join
 * card, false publishes "Could not join" AND releases player 1's device. */
void input_split_join_settle(bool admitted);
void input_pointer_enable(bool enable);
bool input_pointer(float* x, float* y, float* angle);
/* One ephemeral scripted owner may answer pointer queries before native
 * mouse/IR. Return true only when the script owns this query; then `available`
 * is its answer. NULL restores the normal pointer path. No cursor mode change. */
typedef bool (*input_pointer_script_fn)(float* x, float* y, float* angle,
									   bool* available);
void input_pointer_set_script(input_pointer_script_fn script);

/* Text-entry char stream for the OSK-bearing screens (create-world; later
 * sign + MP address). PC: GLFW char/key callbacks feed a small queue while
 * enabled -- physical typing is Beta's faithful input path [740c583
 * GuiScreen.handleKeyboardInput -> keyTyped]. Wii: both are stubs (the OSK
 * is the input there). Printables arrive as themselves; BACKSPACE/TAB/
 * ENTER/ESC arrive as 8/9/13/27. Enable only while a text screen is open
 * so gameplay keys never leak into the queue.
 *
 * input_text_active(): true when a REAL keyboard is feeding the queue (PC
 * with a window, enabled). While active, screens must ignore IB_* edges
 * that alias printable/typing keys (the shipped PC config binds
 * W/S/A/D/E/ENTER to the gui_ actions, inventory and open_menu) -- a
 * focused Beta text screen owns the WHOLE keyboard, no gameplay binding
 * fires [740c583 GuiScreen.handleKeyboardInput]. Wii and headless-test
 * builds return false, keeping the IB path live there. */
void input_text_enable(bool enable);
bool input_text_poll(char* out);
bool input_text_active(void);
/* PC text screens own keyboard bindings. These frame-stable edges contain
 * only mouse bindings, independent of keyboard alternatives for the action.
 * Wii returns false; callers keep their ordinary owner path outside text mode. */
bool input_text_mouse_pressed(enum input_button b);

#endif
