/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#ifndef MENU_SOUND_H
#define MENU_SOUND_H
#include "input_owner.h" /* struct input_device -- the identity the input
						  * layer already carries (input_inventory.[ch] keys
						  * every device status/notice off exactly this). */
#include "time.h"

/* The eight approved menu cues (docs/ui_design/approved/specs/sound-events.md,
 * maintainer-approved 2026-09-07). Nine events map onto them: the attention
 * SAMPLE serves both a controller-attention event and the low-battery notice,
 * which the spec gives its own gain. Client UI only -- no wire packet, no
 * world/simulation sound, so Beta 1.7.3 simulation parity is untouched. */
enum menu_sound_event {
	MENU_SOUND_FOCUS,	 /* changed user selection            -> menu.scroll */
	MENU_SOUND_SECTION,	 /* menu/page/set actually changed    -> menu.category */
	MENU_SOUND_SELECT,	 /* committed action succeeded        -> menu.select */
	MENU_SOUND_BACK,	 /* successful return/resume/cancel   -> menu.back */
	MENU_SOUND_ITEM,	 /* completed item transfer           -> menu.item */
	MENU_SOUND_TYPE,	 /* inserted/deleted character        -> menu.type */
	MENU_SOUND_CONNECTED, /* device detected/returned usable  -> menu.connected */
	MENU_SOUND_ATTENTION, /* lost/missing/join failed         -> menu.attention */
	/* Same attention sample, at the approved 65% low-battery gain. */
	MENU_SOUND_BATTERY_ATTENTION
};

/* The three CONTROLLER cues above are per-device; the six menu cues are not.
 * audio_menu_device_event is the only entry point for the controller three
 * (see the contract below). */
static inline bool menu_sound_is_device_event(enum menu_sound_event event) {
	return event == MENU_SOUND_CONNECTED || event == MENU_SOUND_ATTENTION
		|| event == MENU_SOUND_BATTERY_ATTENTION;
}

/* Client UI changes only. Shared across players; caller supplies the monotonic
 * clock. Events submitted before audio_update are one frame-arbitration batch:
 * each event is first tested against its OWN rate/latch guard, and only the
 * survivors are arbitrated -- the dispatcher keeps the highest-priority
 * GUARD-PASSING event of the frame and plays it from audio_update, so a cue
 * lands at most one frame (<=1 main-loop iteration) after its event. That
 * deferral IS the arbitration -- the spec requires "at most one cue" per
 * competing frame, which cannot be decided until the frame's events are all
 * in. Guards run BEFORE arbitration on purpose: a rate-limited high-priority
 * event must not consume the frame and silence a genuine lower-priority cue
 * (a Back inside its 90 ms navigation window must not swallow the same
 * frame's connection cue, which the spec requires "once for a genuine
 * detected/usable device event"). Skipped events are still discarded, never
 * queued to a later frame.
 *
 * Menu cues only. Passing a controller cue here is a CALLER-CONTRACT
 * violation: it aborts in a debug build and, in a release build, is counted in
 * audio_menu_contract_violations() and played against the "unidentified
 * device" slot so the cue is never silently lost. */
void audio_menu_event(enum menu_sound_event event, ptime_t now);

/* Approved Warm bell: Sound bus, one shared voice, no restart while playing.
 * Call only at a toast's first visible frame; a skipped/muted bell is consumed.
 * Independent of menu-cue arbitration; never touches the music voice. */
void audio_achievement_bell(void);

/* CALLER CONTRACT for the controller cues, now ENFORCED by this signature.
 * The spec puts edge detection at the device layer: "Controller notifications
 * are edge-triggered from debounced device state." Emit CONNECTED/ATTENTION/
 * BATTERY_ATTENTION on a stable NEW event only -- never once per frame while a
 * state persists. The device that the event is about must be named, because
 * every remaining spec rule for these cues is per-device:
 *   - "Collapse discovery and player assignment in the same connection flow
 *      into one cue" and "Do not repeat while waiting or retrying unchanged
 *      state" -- the dispatcher collapses a repeat of the SAME cue for the
 *      SAME device inside MENU_SOUND_NOTICE_COLLAPSE_MS, counted in
 *      audio_menu_notices_collapsed(). A different device is never collapsed:
 *      two Wiimotes syncing at once, or a second controller lost 1 ms after
 *      the first, both stay audible.
 *   - "Battery low: ... once per low-battery episode, rearmed only after a
 *      stable recovery/replacement" -- the latch is PER DEVICE, so one
 *      Wiimote's chirp can never swallow another's, and one device's recovery
 *      can never re-arm another's.
 *   - "never for a wired GameCube controller" -- a BATTERY_ATTENTION for an
 *      INPUT_DEVICE_GAMECUBE device is dropped and counted as a contract
 *      violation.
 * An unknown/out-of-range device is counted as a contract violation and folded
 * into the "unidentified device" slot rather than dropped.
 *
 * STANDING RULING (REVISED 2026-09-07, chest sound corrections lane; supersedes
 * the earlier "sort completion is silent" ruling, which was a placeholder held
 * until the authoritative completion point existed):
 *   - A sort that COMPLETES successfully plays SELECT, the approved
 *     confirmation cue -- crafting-chests.md:27, "Sound only on a real
 *     selection/menu change or completed successful craft/sort." The single
 *     emitter is client_session_sort_tick, at the point where every click of
 *     the plan has been ACCEPTED and the reconciled window equals the plan's
 *     target. Never on submit, never on the local prediction, never from the
 *     ACK callback, and exactly once per sort.
 *   - Sort rejection, admission refusal (a second viewer of the same chest),
 *     interruption, expiry and an UNCHANGED plan stay SILENT, as does every
 *     failed action: "Failed actions, unchanged boundaries, redraws and stew
 *     animation are silent" (crafting-chests.md:27) and "Empty slots, blocked
 *     moves, failed safe-close attempts and capacity failures are silent"
 *     (sound-events.md, Inventory/furnace/dispenser row).
 *   - A completed item TRANSFER plays ITEM, not SELECT, and is likewise
 *     commit-timed: "Item cue once after a successful pickup, placement,
 *     split-one or quick move; no second confirmation for the same transfer",
 *     under the runtime rule "Actual transfer/save success hooks must come
 *     from authoritative completed operations." container_menu.c arms the cue
 *     when the click is sent and plays it when that action's accept lands.
 *     ONE slot is excepted: the crafting RESULT slot (the player's 2x2 grid
 *     and the workbench's 3x3, keyed off inventory_click_is_craft_output --
 *     the same predicate that consumes the grid). Taking that stack IS a
 *     "completed successful craft", so it keeps SELECT, the cue the base tree
 *     played there: "Keep the approved crafting cues and recipes unchanged."
 *     Several transfers can be in flight at once -- nothing gates ordinary
 *     click pipelining -- so the arms are a small FIFO ring, one per action;
 *     each is resolved by its own accept and dropped on a rejection or a
 *     window reset, never overwritten by the next click.
 *   - A successful Back/close -- the safe-close that returned the held stack
 *     and sent the close, and the sort panel's own Back control -- plays BACK,
 *     which since the eight-cue land has its own sample rather than aliasing
 *     the category cue: "Back on successful return/resume." A blocked
 *     safe-close or a failed send plays nothing.
 * All four are shared menu cues: they carry no player identity and are bounded
 * by the dispatcher's shared 60 ms confirmation / 90 ms navigation / 90 ms
 * item-type guards, so two split-screen players competing in one frame still
 * emit at most one cue ("Use one menu-audio mixer and one volume/mute setting
 * shared by both split-screen players"). */
void audio_menu_device_event(enum menu_sound_event event,
							 struct input_device device, ptime_t now);

/* The same-device, same-cue collapse window (see above). Long enough to cover
 * one connection flow's discovery -> player-assignment pair and a retry of an
 * unchanged state, short enough that a real disconnect/reconnect of the SAME
 * device stays audible; a different device is never affected by it. */
#define MENU_SOUND_NOTICE_COLLAPSE_MS 500

/* Re-arm the low-battery notice for ONE device. The spec allows the battery
 * cue "once per low-battery episode, rearmed only after a stable recovery/
 * replacement", so the dispatcher latches after that device's first
 * BATTERY_ATTENTION of an episode and discards the rest ("No periodic chirp").
 * Only the device layer can see a recovery, so it calls this when that
 * controller's battery is stably good again or the controller is replaced.
 * Idempotent; safe to call when that device is not latched. */
void audio_menu_battery_rearm(struct input_device device);

/* Visible contract counters (release-safe halves of the two asserts above).
 * Cleared by audio_init. */
unsigned audio_menu_notices_collapsed(void);
unsigned audio_menu_contract_violations(void);

#endif
