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

#ifndef SCREEN_H
#define SCREEN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../cglm/cglm.h"

struct screen {
	void (*reset)(struct screen* s, int width, int height);
	void (*update)(struct screen* s, float dt);
	void (*render2D)(struct screen* s, int width, int height);
	void (*render3D)(struct screen* s, mat4 view);
	bool render_world;
	/* Installed by screen_set; shared notice overlay follows native content. */
	void (*render2D_content)(struct screen* s, int width, int height);
};

/* True consumes this frame's screen update, including successful Resume. */
bool screen_controller_recovery(void);
void screen_controller_recovery_render(int width, int height);
void screen_pause_recovery_quit(void);
/* Save and quit settles on the server's answer (client_interface.c): true =
 * WORLD_RESET(local_entity=0) arrived -> title; false = RETURN_INGAME -> the
 * pause stays up showing the server's "Save failed: ..." toast. No-ops
 * unless an exit is pending. */
bool screen_pause_exit_pending(void);
void screen_pause_exit_settled(bool unloaded);
/* IMP-QUIT-UNSAVABLE: the pending Save and quit was refused ONLY because
 * `count` chunk(s) could not be saved (first: chunk_x, chunk_z). Settles the
 * exit as failed, then opens screen_confirm "Couldn't save part of the
 * world" -- "Quit anyway" re-sends the unload excusing exactly those chunks,
 * "Try again" (safe answer, also Back) re-sends the ordinary unload. */
void screen_pause_exit_unsavable(int32_t chunk_x, int32_t chunk_z,
								 uint32_t count);

/* READ-ONLY focus observers (push-0921/B4b, Astra gate 1 Lane B item 3:
 * "authorize only the minimal observation seams"). Both screens keep their
 * focus in file statics, so an unattended scripted driver cannot see it
 * without an accessor. Neither changes any state, and nothing in the game
 * branches on either; the full contract (and every -1 / false case) is on the
 * definitions in screen_pause.c and screen_inventory.c.
 *   - true when the DIRECTIONALLY focused pause row is the one that leaves a
 *     REMOTE session ("Leave game"); false in a local world, where that row is
 *     "Save and quit" and leaves no remote session.
 *   - the inventory's directionally focused slot as a PROTOCOL window-0 slot
 *     number (main 9..35, hotbar 36..44), or -1 when the focus is on armor /
 *     crafting / the craft output, when the pointer owns focus, or when this
 *     screen is not the one up. */
bool screen_pause_focus_is_leave(void);
/* menus_dwell walker (menus-dwell-walker-1026): the pause row the selection
 * is on, as WHAT IT DOES rather than where it is (the row order changed when
 * Achievements returned, 91bd8295c, and Texture packs exists only in the full
 * layout). SCREEN_PAUSE_FOCUS_NONE when the pause screen is not the one up.
 * Read-only. */
enum screen_pause_focus {
	SCREEN_PAUSE_FOCUS_NONE = -1,
	SCREEN_PAUSE_FOCUS_BACK = 0,
	SCREEN_PAUSE_FOCUS_ACHIEVEMENTS,
	SCREEN_PAUSE_FOCUS_OPTIONS,
	SCREEN_PAUSE_FOCUS_TEXPACKS,
	SCREEN_PAUSE_FOCUS_EXIT, /* host "Save and quit" / guest "Leave game" */
};
int screen_pause_focused_action(void);
int screen_inventory_focus_player_slot(void);
/* Publish the approved notice mount for THIS frame (menu_header.h). Called
 * from a menu's own render2D, before it returns; screen.c draws the live
 * controller notices there instead of the default viewport corner. */
void screen_notice_compose(int x, int y, int w, int h, bool in_header);
/* The single-player loss/recovery gate's current state, for the pause screen
 * and the approved recovery dialog. Never NULL. */
struct controller_recovery;
const struct controller_recovery* screen_controller_recovery_state(void);

struct client_session;
struct input_player_frame;
struct menu_rect;
/* The approved beige menu panel shared by the sign editor and the chat
 * composer. Defined in screen_sign.c. */
void screen_menu_panel_draw(const struct menu_rect* panel);
void screen_ingame_render3D_owned(struct client_session* owner, mat4 view);
void screen_ingame_render2D_owned(struct client_session* owner,
	const struct input_player_frame* input, int width, int height);
struct input_device;
/* S1-b (mp-split-0912): the in-game ACTION path for a NON-host local owner --
 * the split-screen guest. `slot` is the owner's transport identity (1 today);
 * `frame` is that owner's own latched controller frame. Player 0 keeps the
 * screen vtable's update, which is byte-identical to its pre-S1-b self.
 * Call it only for a frame screen_ingame_guest_action_frame armed. */
void screen_ingame_update_owned(struct client_session* owner, unsigned slot,
	const struct input_player_frame* frame, float dt);
/* Owner-1 admission + neutral-rearm latch. `admitted` is the caller's view/
 * world/menu/body/health gate; this adds the continuity rule (same generation,
 * same device, connected frame) and requires one all-action-buttons-released
 * frame before re-arming after any drop. Clears the owner's pending dig and
 * held-item animation on every drop, so a rejoin never resumes an action.
 * Returns true when the action path may run THIS frame. */
bool screen_ingame_guest_action_frame(struct client_session* owner,
	const struct input_player_frame* frame, const struct input_device* device,
	bool admitted);
/* Forget the latch and drop the owner's pending action state outright. */
void screen_ingame_guest_action_reset(struct client_session* owner);
extern struct screen screen_ingame;
extern struct screen screen_load_world;
/* Load-world hold escape hatch (task_db613f13, remote sessions only): a
 * pure, input/gfx-free stall detector so its core arithmetic is testable
 * headlessly (tests/test_mp_s0_env.c) without dragging in gfx/input.
 * screen_load_world.c feeds it the CURRENT world_loaded_chunks() count once
 * per screen_lworld_update() frame; the return value is the accumulated
 * no-progress duration, reset to 0 whenever the count advances -- the
 * caller compares that against its own stall timeout.
 *
 * CHIP A (task_11695e19, 2026-08-31): world_loaded_chunks()'s NET section
 * count alone is blind to in-flight streaming -- a same-dim teleport that
 * re-centers the ring can burn many frames where columns are genuinely
 * unloading/installing (or one destination column is inching through a
 * budgeted multi-frame zlib decode) while the net count never moves. The
 * tracker now also takes `installs`, a monotonic count of columns actually
 * installed (clin_chunk_install_seq(), client_interface.h) -- progress is
 * "the chunk count changed" OR "installs advanced" since the last tick,
 * either one resets the no-progress accumulator. */
struct lworld_stall_tracker {
	size_t last_chunks;
	uint32_t last_installs;
	float no_progress_s;
	bool primed;
};
void lworld_stall_tracker_reset(struct lworld_stall_tracker* t);
float lworld_stall_tracker_tick(struct lworld_stall_tracker* t,
								size_t current_chunks, uint32_t installs,
								float dt);
/* Near-ring loading-screen gate (2026-09-03 maintainer ruling): pure so its
 * threshold arithmetic is unit-testable headlessly, same rationale as
 * lworld_stall_tracker above. screen_load_world.c feeds it the CLIENT's own
 * near-ring readiness (world_near_ring_status's combined missing+mesh-
 * pending count, NOT gstate.world_loaded -- that only means the SERVER
 * placed the player) plus wall-clock time spent on this screen; see its
 * definition in screen_load_world.c for the full contract. */
bool lworld_near_ring_ready(int near_missing, float time_on_screen_s,
							float timeout_s, bool* out_timed_out);
// H-3: the measured duration (ms) of the most recently COMPLETED loading
// screen; 0 before any / while one is still showing.
uint32_t lworld_load_screen_ms_result(void);
/* Shared screen_set admission owner. A loading episode remains pending
 * across pause/container restoration until gameplay is actually admitted. */
/* Latch near-mesh/fallback readiness without exposing input. Independent of
 * world_switch_pending: local input ACK waits for this, not the reverse. */
bool lworld_prepare_presentation(void);
bool lworld_admit_gameplay(void);
/* Rebase an already-open container for admitted split presentation only. */
bool screen_split_container_base(uint8_t window,int width,int height);
void lworld_reset_presentation(void);
extern struct screen screen_select_world;
extern struct screen screen_texturepacks;
extern struct screen screen_inventory;
extern struct screen screen_crafting;
extern struct screen screen_furnace;
extern struct screen screen_chest;
extern struct screen screen_dispenser;
extern struct screen screen_iron_chest;
extern struct screen screen_sign;
/* Multiplayer chat / command composer (remote sessions only, player 0). */
extern struct screen screen_chat;
extern struct screen screen_pause;
extern struct screen screen_achievements;
extern struct screen screen_options;
extern struct screen screen_video_settings;
extern struct screen screen_controller_settings;
extern struct screen screen_creative_picker;
// Beta GuiGameOver (deviation #18 closure): red gradient, "Game over!",
// the literal "Score: &e0" line, Respawn + Title menu buttons.
extern struct screen screen_death;
void screen_death_update_viewport(float dt, int width, int height);
// Beta GuiMainMenu with the CaveWii identity: dirt background, text logo,
// pulsing splash, the standalone five-button layout (Multiplayer remains
// disabled until its milestone). The boot entry screen.
extern struct screen screen_title;

// Interim multiplayer title-screen path (plan
// docs/mp_online_client_plan_2026-08-15.md section 3.3): the connecting/
// cancel/error screen server_remote drives. screen_multiplayer_connect_impl
// is the real (PC-only) screen object, always defined so main.c always has
// something to take the address of; screen_multiplayer_connect is a POINTER
// that main.c registers to &screen_multiplayer_connect_impl ONLY on PC,
// after mp_session_init() -- it stays NULL on Wii and inside every unit
// test (nothing there ever runs main()). screen_multiplayer_setup.c (below)
// is now screen_title.c's ONLY route to this screen -- see its own contract
// comment for how the two compose.
extern struct screen screen_multiplayer_connect_impl;
extern struct screen* screen_multiplayer_connect;
/* S4 disconnect UX (PC-only, defined in screen_multiplayer_connect.c's
 * PLATFORM_PC half): route a mid-play kick/drop to this screen's ERROR
 * state showing `reason`, instead of a silent fall to the title. Callers
 * live inside PC-only MP seams (main.c teardown). */
void screen_multiplayer_connect_show_disconnected(const char* reason);

// NEW multiplayer setup screen (carve-out #2, maintainer-requested
// 2026-08-31): Beta 1.7.3's GuiMultiplayer is just one address field with
// the username coming from the launcher [740c583 GuiMultiplayer] -- this
// adds a name field + a saved-server list ON TOP of that faithful base, per
// the "adapt for Wii-controller playability, document the deviation" rule
// (this file's header comment further up). screen_title.c's Multiplayer
// button now opens THIS screen; it hands off to the UNCHANGED
// screen_multiplayer_connect (mpc_begin_connect still reads mp.server/
// mp.username from gstate.config_user, screen_multiplayer_connect.c:143-199)
// by writing those same two scalars and calling screen_set(
// screen_multiplayer_connect) -- Quick Connect and a saved-row Connect are
// the only two paths that do this, so they stay the "active-target" keys
// documented in config.h. Same impl/pointer shape as
// screen_multiplayer_connect above, same reason (main.c always has an
// address to register; the pointer is NULL on any platform that never
// calls main.c's CAVEWII_MP_CLIENT registration block, e.g. every unit
// test).
extern struct screen screen_multiplayer_setup_impl;
extern struct screen* screen_multiplayer_setup;
// Beta GuiCreateWorld with the shared OSK widget (carve-out #2 text input;
// docs/ui_design/osk_create_world_2026-08-15.md). Opened from the world
// list; the "+new" instant create stays untouched for the capture rigs.
extern struct screen screen_create_world;

// The dialog's create entry (screen_select_world.c owns the saves-path +
// spawn plumbing): folder_base is the derived folder name pre-dedupe; the
// "-" dedupe runs inside the mkdir reservation. seed_set=false = random.
// `creative` is the dialog's Game Mode selection: it becomes the world's
// LEVEL_GAME_MODE tag (written only when true -- absent means survival) and
// the client-side creative state for this session.
// Returns true when the world was created AND the load request was sent.
bool screen_sworld_create_from_dialog(const char* display_name,
									  const char* folder_base, bool seed_set,
									  int64_t seed, bool creative);

// split_recovery_6m's rig world (docs/split_recovery_profile.md): create a
// world at EXACTLY `directory`, deleting any earlier copy of it first, so an
// unattended profile can reload the same save by name after a save-and-quit.
// Refuses -- and touches nothing -- unless the split_recovery_6m phase driver
// is armed and `directory` is its single fixed rig constant
// (split_recovery_rig_create_allowed), so this world-deleting path has no
// caller-chosen target. Otherwise identical to the dialog create above: the
// same sworld_finish_create tail, survival, and the same seed precedence.
bool screen_sworld_create_exact(const char* display_name, const char* directory,
								bool seed_set, int64_t seed);

// Read-only companion to the above: true once this boot has spent the
// process-lifetime "+new"/named autoload latch. split_recovery_6m's P4 arms
// that hook itself, so its driver asks before it deletes anything -- see
// screen_select_world.c.
bool screen_sworld_autoload_consumed(void);

// Harness fixed-seed override (hw_run --cw-seed, 2026-09-04): when `set` is
// true, sworld_finish_create's seed chain uses `seed` for every subsequent
// "+new"/dialog create in THIS process, at the HIGHEST precedence (even over
// the debug.world_seed capture-rig pin) -- see screen_select_world.c's
// sworld_finish_create doc comment. main.c calls this DIRECTLY from
// hw_run.seed/seed_set, never through a "debug.*" config key, so a
// hand-edited config can never accidentally arm it (same rationale as
// hw_run.h's autopilot_unstick/cworld_mode fields). `set == false` (the
// default, and every boot that never calls this) is byte-identical to
// before this override existed.
void screen_sworld_set_seed_override(bool set, int64_t seed);

// Beta GuiYesNo [740c583]: generic confirm screen. Call setup() (copies
// the strings), then screen_set(&screen_confirm); the callback fires
// exactly once with the answer and OWNS the follow-up screen_set (Beta's
// deleteWorld -> displayGuiScreen shape). Back-out (IB_INVENTORY/IB_HOME)
// answers no.
extern struct screen screen_confirm;
/* The two answers of a confirmation, BY MEANING. [740c583
 * GuiYesNo.actionPerformed]: id 0 is the yes button -- the one whose callback
 * performs the thing the dialog asks about -- and id 1 is no. Which of them
 * is NON-DESTRUCTIVE is a property of the DIALOG, so the opener states it
 * (review A6-5: it used to be a constant inside the confirm, and a future
 * dialog whose safe answer is YES would have got the wrong default silently). */
enum screen_confirm_answer {
	SCREEN_CONFIRM_ANSWER_YES = 0,
	SCREEN_CONFIRM_ANSWER_NO = 1,
};

/* `safe_answer` is the one that changes nothing; the modal OPENS on it. */
void screen_confirm_setup(const char* msg1, const char* msg2,
						  const char* yes_label, const char* no_label,
						  enum screen_confirm_answer safe_answer,
						  void (*cb)(bool yes));

/* ui-build-1026/T09: "Back up this world?" before a Minecraft Beta world is
 * loaded and converted (screen_convert_backup.c). setup() copies the world
 * path and display name and stores `proceed`, the opener's unchanged world
 * load (false = nothing sent); then screen_set(&screen_convert_backup).
 * Cancel returns to screen_select_world with nothing written; Back up copies
 * the world through world_copy first; convert-only (T10) then runs, and
 * `proceed` runs only on the player's Play world after the result screen;
 * a failed copy or conversion never runs it on its own. Returns false (and
 * arms nothing) for an overlong path or while a copy is still open. */
extern struct screen screen_convert_backup;
bool screen_convert_backup_setup(const char* world_path, const char* world_name,
								 bool (*proceed)(const char* path));
/* ui-build-1026/T16a: read-only observers for the world_copy_convert proof
 * route (the walker's copy hooks; values mirrored in menu_autopilot_script.h:
 * state 0 ask .. 8 conversion error, focused action 0 Back up .. 4 Back or -1
 * when no buttons, fail 0 none .. 4 cleanup, copy_error = world_copy_error,
 * outcome = svin_convert_outcome). No side effects. */
int screen_convert_backup_state(void);
int screen_convert_backup_focused_action(void);
int screen_convert_backup_fail(void);
int screen_convert_backup_copy_error(void);
int screen_convert_backup_percent(void);
bool screen_convert_backup_volume(uint64_t* free_bytes, uint32_t* cluster,
								  uint64_t* required);
int screen_convert_backup_convert_outcome(void);
bool screen_convert_backup_backup_published(void);
/* hw1028b: the failed/cancelled backup's world_copy cleanup provably ran to
 * its end (started, no cleanup_error, destination absent on disk). True even
 * when that cleanup fit inside one frame and "Cleaning up..." never showed. */
bool screen_convert_backup_cleanup_done(void);
/* T16a rig-only seams: called ONLY from main.c's hw_run world_copy_convert
 * block; never armed in any other run. mode 1 = injected low free space,
 * 2 = world_copy write fault; interrupt = abandon the running copy without
 * cleanup and freeze the screen (the on-disk shape of a power cut). */
void screen_convert_backup_rig_arm_fault(int mode);
void screen_convert_backup_rig_interrupt(void);

// Beta GuiRenameWorld [740c583]: one prefilled field + the shared OSK.
// setup() takes the world DIRECTORY (never changed by a rename) and the
// current resolved LevelName; the apply op rewrites LevelName (trimmed)
// in place. apply is exported for the env test.
extern struct screen screen_rename_world;
void screen_rename_world_setup(const char* directory_path,
							   const char* current_name);
bool screen_rworld_apply(const char* directory_path, const char* new_name);

// Java String.trim (<= 0x20 both ends) [740c583 String.trim] -- shared by
// the create dialog's folder derivation and the rename dialog's enable +
// save rules.
void screen_cworld_java_trim(const char* in, char* out, size_t out_cap);

// The texture-pack screen is reachable from BOTH the title screen and the
// world-select screen; the opener declares where Back returns to (defaults
// to world-select, the pre-title behaviour).
void screen_texturepacks_set_return(struct screen* s);
/* In-game mode (opened from the Game menu): the list draws over the paused
 * world and Apply changes textures only -- the live sound and music sets are
 * kept. Cleared when the screen returns. */
void screen_texturepacks_set_ingame(bool on);

// Beta GuiOptions is opened by both GuiMainMenu and GuiIngameMenu. The
// opener declares its parent so Done returns without losing pause ownership.
void screen_options_set_return(struct screen* s);
bool screen_options_from_pause(void);

/* ---- Live "Local split-screen" row (Astra option A, S-A2) ----------------
 * The row applies the experimental split switch WITHOUT a restart: toggling it
 * while a local world is loaded sends SRPC_SPLIT_SET and waits for the
 * server's CRPC_SPLIT_STATE. Input arming follows the ACKNOWLEDGED value only,
 * never the value the row is asking for (the ruling's "acknowledged state
 * before input arming"); see docs/local_splitscreen.md.
 *
 * screen_options_split_boot: seed the acknowledged value with what the boot
 * path ACTUALLY applied (main.c's server_local_set_split_enabled result), so
 * the row starts truthful even when the boot allocation failed while the
 * config key still says on.
 * screen_options_split_poll: RECONCILE to the server against the ack the
 * client just consumed (NULL when none exists) and age the pending timeout by
 * `dt` seconds. Any ack that names the CURRENT world generation is adopted
 * whatever state the row is in -- every ack carries the server's own
 * `split_enabled`, refusals included, so it is the authority (R-A8 A8-H1/H2:
 * looking at an ack only while the row was PENDING stranded every answer that
 * arrived after a timeout or a refused send, leaving the server ON and the
 * client OFF for the rest of the session). Returns the acknowledged enabled
 * state -- what input_split_join_arm must follow. Main/client thread, once per
 * frame.
 *
 * `ack` MUST be a new server statement or NULL -- one ack, one call (R-A9
 * A9-H1/H2/M3). clin_split_last_ack takes the ack out of its slot, so the
 * production seam satisfies this by construction; a caller that re-fed the
 * same ack every frame would cancel the deferred config write one frame after
 * arming it, make the 5 s no-answer timeout unreachable, and pin one caption
 * over every later row state. */
struct clin_split_ack;
void screen_options_split_boot(bool applied);
bool screen_options_split_poll(const struct clin_split_ack* ack, float dt);
/* screen_options_split_flush: persist an acknowledged toggle that the deferred
 * A8-M6 commit has not written yet (main.c calls it at the world's teardown, so
 * a live toggle is never lost when the player leaves without reopening
 * Options). No-op and no file access when nothing is owed; returns false only
 * when the write itself failed, leaving the value owed to a later menu
 * commit. Main/client thread. */
bool screen_options_split_flush(void);
/* screen_options_split_world_seed: re-seed the live switch from the config FILE
 * once per WORLD GENERATION (R-A9 A9-M5). The boot path reads the key once,
 * before the main loop, so without this a title-screen edit reached the file
 * and the next launch but NOT any world this process loaded afterwards -- the
 * row snapped back to the boot-applied value, input stayed armed and a second
 * controller could still join after an explicit OFF.
 *
 * `force_on` is main.c's `hw_run.enabled && hw_run.split_guest` term (the same
 * disjunct the boot seed uses), so an armed split harness profile is never
 * asked OFF by a config file that says 0.
 *
 * Called every frame at the ack seam; a single epoch compare until the
 * generation changes. On a new generation it clears the per-world heartbeat
 * evidence and any pending toggle the old world left behind (no stale live
 * state crosses a generation), then -- when the file disagrees with the
 * acknowledged switch -- sends ONE SRPC_SPLIT_SET asking for the file's value.
 * The SERVER side is re-seeded through that request on purpose:
 * server_local_set_split_enabled refuses once the server thread is running, so
 * the live route is the only one that does not race the owning thread. Returns
 * the ACKNOWLEDGED switch, exactly like screen_options_split_poll -- arming
 * never follows the file. Main/client thread. */
bool screen_options_split_world_seed(bool force_on);
/* Leave-before-OFF coordinator. Main passes the same one-shot ack already
 * offered to screen_options_split_poll, after per-world seeding. Join remains
 * blocked from accepted confirmation through exact OFF settlement or epoch
 * retirement. */
void screen_options_split_off_tick(struct client_session* guest,
	uint32_t world_generation, const struct clin_split_ack* ack);
bool screen_options_split_off_blocks_join(void);
/* Presentation-neutral split control used by both the full Options screen and
 * Player 1's owner-local compact Options page. The coordinator remains the
 * sole owner of leave receipts, request sequences, retry and epoch retirement;
 * callers own only their focus and geometry. */
enum screen_options_split_off_phase {
	SCREEN_SPLIT_OFF_IDLE,
	SCREEN_SPLIT_OFF_CONFIRM,
	SCREEN_SPLIT_OFF_LEAVE_PENDING,
	SCREEN_SPLIT_OFF_LEAVE_FAILED,
	SCREEN_SPLIT_OFF_REQUEST_PENDING,
	SCREEN_SPLIT_OFF_REQUEST_FAILED,
};
bool screen_options_split_set(bool enabled);
const char* screen_options_split_label(void);
enum screen_options_split_off_phase screen_options_split_off_phase(void);
void screen_options_split_off_cancel(void);
void screen_options_split_off_confirm(struct client_session* guest);
bool screen_options_split_off_retry(struct client_session* guest);
bool screen_options_split_off_rearm_step(bool active);

/* Client-side guest maintenance under the LIVE switch (R-A8 A8-H3). Both are
 * PURE predicates over the ACKNOWLEDGED switch this file owns; main.c uses them
 * instead of gating its guest block on the switch directly, which used to wedge
 * a guest that was ATTACHED but not yet admitted and freeze the guest anchor.
 *
 * screen_options_split_guest_maintain(session_exists, admitted) -- keep running
 *   the client's guest handshake/settle/screen-pin/anchor work this frame. True
 *   while the switch is on, and while a guest session or body exists whatever
 *   the switch says. False with the switch off and no guest, so the
 *   single-player frame is byte-identical.
 * screen_options_split_guest_refuse(joining, admitted) -- settle the join card
 *   as REFUSED and release the controller claim: the switch is off and a join
 *   is still trying with no admitted body. An ADMITTED body is never refused
 *   (the server invariant), and the request/ack FIFO guarantees `admitted` is
 *   already true here for any admission the server granted before the OFF. */
bool screen_options_split_guest_maintain(bool session_exists, bool admitted);
bool screen_options_split_guest_refuse(bool joining, bool admitted);

/* Heartbeat view of the live toggle (Astra H3, S-A3). READ-ONLY: the row owns
 * every one of these, and the telemetry path must never be able to move them.
 * The heartbeat already carries mem2_carve_free_kb every second, so the MEM2
 * headroom either side of an enable comes out of the stream on its own -- what
 * it could not say was WHEN a toggle happened, whether one was still in
 * flight, how long the server took to answer it, and whether the answer was
 * yes. These four are exactly that, and nothing more.
 *
 *   live        -- the ACKNOWLEDGED state (what input arming follows), not the
 *                  value the row is currently asking for.
 *   pending     -- a request is outstanding (no ack, not yet timed out).
 *   toggle_ms   -- request -> matching-ack round trip of the LAST toggle that
 *                  was answered, in ms. 0 means "no toggle has been answered
 *                  this world" -- so a sub-millisecond round trip reports 1
 *                  rather than colliding with that sentinel. A request that
 *                  timed out never had a round trip and leaves this alone.
 *   last_ok     -- outcome of the last SETTLED request: 1 the server said yes,
 *                  0 it said no (or the transport refused it, or it was never
 *                  answered), -1 no request has settled this world yet. */
struct split_live_status {
	bool live;
	bool pending;
	uint32_t toggle_ms;
	int last_ok;
};
struct split_live_status screen_options_split_live_status(void);

void screen_set(struct screen* s);

// World-list resolve progress (screen_select_world.c). Surfaced in the hang report so a
// card that has gone merely SLOW is visible: since the enumeration became incremental only
// a single multi-second open can still trip the hang detector, and everything between
// "fast" and "wedged" would otherwise be a silent crawl. NOT an F3 sink -- the overlay only
// draws in-world, and these move only in the menu.
size_t world_list_pending_rows(void);
uint32_t world_list_resolve_max_ms(void);

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
bool screen_ingame_test_try_pause(void);
bool screen_ingame_test_gate_sleep_actions(void);
void screen_pause_test_process_edges(bool home, bool inventory);
/* The approved pause composition depends on the layout (Options... is
 * single-player only) and on world ownership (host "Save and quit" vs guest
 * "Leave game"), so both are parameters rather than read out of a live
 * gfx/session the test does not have. */
size_t screen_pause_test_button_count(bool full_layout);
struct menu_composition;
void screen_pause_test_composition(struct menu_composition*, int width, int height);
void screen_pause_test_activate_row(size_t row);
bool screen_pause_test_button(size_t idx, bool full_layout, bool host, int* id,
							  const char** label);
const char* screen_pause_test_controller_name(int category);
/* Death screen commit timing (specs/sound-events.md: "Confirmation only when
 * respawn actually commits"). */
bool screen_death_test_commit_armed(void);
bool screen_death_test_respawn_committed(bool alive, bool world_loaded,
										 bool switch_pending);
float screen_death_test_grace_s(void);
const char* screen_death_test_exit_label(void);
/* Sign editor: the approved discard workflow and the resolved layout. */
struct menu_composition;
struct menu_rect;
bool screen_sign_test_discard_open(void);
void screen_sign_test_layout(int width, int height, struct menu_composition* out,
							 struct menu_rect* face, struct menu_rect* keyboard);
size_t screen_options_test_control_count(void);
bool screen_options_test_control(size_t idx, int* id, const char** name,
								 bool* slider, bool* enabled);
float screen_options_test_slider_value(float pointer_x, int x, int width);
int screen_options_test_slider_thumb(int x, int width, float value);
void screen_options_test_format_label(int id, float value, char* out,
								  size_t out_size);
/* Live split row (S-A2). The toggle seam is the one entry point every input
 * path (A, Left, Right) funnels through, so the test drives the production
 * state machine without synthesising input frames or a GL context. */
bool screen_options_test_split_set(int wanted);
const char* screen_options_test_split_row(void);
const char* screen_options_test_split_status(void);
/* What the Options config transaction would write for
 * experimental.local_splitscreen right now -- the ACKNOWLEDGED value while a
 * request is outstanding, so the file never promises an ungranted state. */
int screen_options_test_split_config_value(void);
/* True while the acknowledged value has been moved into the in-memory config
 * but no config_save has persisted it yet (A8-M6's deferred commit). */
bool screen_options_test_split_key_dirty(void);
bool screen_options_test_split_off_open(void);
void screen_options_test_split_off_choose(bool turn_off,
	struct client_session* guest);
bool screen_options_test_split_off_retry(struct client_session* guest);
bool screen_options_test_split_off_rearm_step(bool active);
void screen_options_test_split_off_rearm_arm(void);
uint32_t screen_options_test_split_off_request_seq(void);
int screen_options_test_split_off_focus(void);
bool screen_options_test_split_off_destructive_dialog(int hovered);
bool screen_options_test_split_off_rect(int target,
	struct gutil_button_bounds* out);
/* Reopen the Options screen exactly the way screen_set does -- the production
 * reset() -- so the "which value does the row show again" decision (A8-M4) is
 * exercised rather than described. */
void screen_options_test_reopen(void);
size_t screen_video_settings_test_control_count(void);
bool screen_video_settings_test_control(size_t idx, int* id, const char** name,
										bool* enabled);
void screen_video_settings_test_format_label(int id, bool value, char* out,
											 size_t out_size);
/* Approved Player 1 Controller settings. Target ids are Invert, sensitivity
 * minus, sensitivity plus, Back (0..3), then Attack assist (4, the third row
 * above Back), matching the production focus graph. */
bool screen_controller_settings_test_target_rect(
	int target, struct gutil_button_bounds* out);
int screen_controller_settings_test_focus(void);
bool screen_controller_settings_test_target_enabled(int target);
int screen_controller_settings_test_sensitivity(void);
bool screen_controller_settings_test_inverted(void);
int screen_controller_settings_test_assist(void);
bool screen_controller_settings_test_save_failed(void);
// World-list incremental resolve, driven without a GL context. Deliberately excludes
// input_pressed / audio_play_ui / getenv so a test drives pure enumeration + resolve.
void screen_sworld_test_reset(int width, int height);
// The "+new" autoload sentinel gate in isolation (2026-08-17 leak fix). See
// screen_select_world.c's sworld_autoload_new_tick for the real call site;
// this is a thin, non-reimplementing passthrough to it.
void screen_sworld_test_reset_autoload_latch(void);
bool screen_sworld_test_autoload_new_tick(const char* autoload_name,
										   bool from_env);
void screen_sworld_test_set_owner_alloc_hook(void* (*alloc_fn)(size_t));
size_t screen_sworld_test_resolve_step(size_t budget); // returns archive opens made
size_t screen_sworld_test_resolve_frame(void);         // the REAL per-frame budgeted call
size_t screen_sworld_test_archive_opens(void);		   // cumulative since last reset
size_t screen_sworld_test_row_count(void);
/* The hidden tutorial's autoload route (maintainer 2026-10-31). */
bool screen_sworld_test_hidden_autoload_path(const char* name, char* out,
											 size_t cap);
size_t screen_sworld_test_pending(void);
// Returns the row's enum world_row_state; out-params may be NULL.
int screen_sworld_test_row(size_t idx, const char** name, const char** directory,
						   const char** path, int64_t* last_access,
						   int64_t* byte_size);
// The row's resolved LEVEL_GAME_MODE (false for PENDING/UNREADABLE rows).
bool screen_sworld_test_row_creative(size_t idx);
size_t screen_sworld_test_next_selection(size_t cur, size_t count, bool up, bool down);
bool screen_sworld_test_normalize_directory_path(const char* path, char* result,
										 size_t result_size);
bool screen_sworld_test_reserve_directory(const char* saves_path,
										uint32_t first, char* result,
										size_t result_size);
// Beta-naming reservation (the dialog's "-" dedupe scheme).
bool screen_sworld_test_reserve_beta(const char* saves_path,
									 const char* folder_base, char* result,
									 size_t result_size);
// Recursive world-directory delete [740c583 ISaveFormat.deleteWorldDirectory].
void screen_sworld_test_delete_tree(const char* path);
bool screen_sworld_test_persist_new(const char* directory, int64_t seed,
									vec3 world_spawn, vec3 player_spawn,
									bool start_nether);
// Create-world dialog pure logic (screen_create_world.c): Beta's folder
// derivation [740c583 GuiCreateWorld.calcSaveDirName] and seed resolution
// [.actionPerformed] -- the test goldens' targets.
void screen_cworld_test_derive_folder(const char* name, char* out,
									  size_t out_cap);
bool screen_cworld_test_resolve_seed(const char* text, int64_t* out);

// screen_multiplayer_setup.c pure logic (server-list add/edit/delete +
// username validation), thin passthroughs to the exact static helpers
// update() calls -- same "cannot drift from what actually ships" contract
// as the screen_sworld_test_* hooks above. All operate on the screen's own
// file-scope `servers[]` array purely IN MEMORY: no gstate.config_user, no
// disk I/O (config_read_server_list/config_write_server_list's own
// migration + round-trip behavior is tested directly against a temp
// `struct config`, not through these). screen_mps_test_reset_servers must
// precede the others in a fresh test case (mirrors
// screen_sworld_test_reset_autoload_latch's per-case reset).
void screen_mps_test_reset_servers(void);
size_t screen_mps_test_server_count(void);
bool screen_mps_test_server(size_t idx, char* name, size_t name_cap,
							char* address, size_t address_cap);
bool screen_mps_test_add_server(const char* name, const char* address);
bool screen_mps_test_edit_server(size_t idx, const char* name,
								 const char* address);
bool screen_mps_test_delete_server(size_t idx);
bool screen_mps_test_username_valid(const char* text);
bool screen_mps_test_join_gate(const char* name, bool* editor_opened, bool* parked);
bool screen_mps_test_join_gate_cancel(void);
// v3 (2026-08-31 maintainer upgrade, GENERAL KNOWLEDGE later-Minecraft-
// styling -- see screen_multiplayer_setup.c's own header comment): the
// 0xFF/string16 legacy ping-response parser ("MOTD\247online\247max" ->
// motd/online/max) and the latency->signal-bar mapping, both pure and
// socket-free. `data`/`len` is a raw wire buffer the test hand-builds (no
// live socket -- see the parser's own doc comment for the wire shape).
bool screen_mps_test_parse_ping_response(const uint8_t* data, size_t len,
										 char* motd_out, size_t motd_cap,
										 int32_t* online_out,
										 int32_t* max_out);
int screen_mps_test_ping_bars(int32_t latency_ms);
// Audit finding 1a (2026-08-31): Refresh must stay disabled while a ping
// sweep is still in flight -- a pure predicate over the file-scope
// ping_cursor/server_count. screen_mps_test_set_ping_cursor fakes "sweep in
// progress" without spinning any real thread/socket (this file's own header
// comment's "no live socket anywhere in this test" rule).
void screen_mps_test_set_ping_cursor(size_t cursor);
bool screen_mps_test_refresh_enabled(void);
// Audit finding 2 (2026-08-31): the pure TOCTOU-revalidation decision
// mps_ping_tick's timeout branch consults after re-locking and re-reading
// the handoff a second time. Returns 0 (wait -- phase advanced and its
// recomputed deadline has not hit yet), 1 (consume -- the worker finished
// in the gap), or 2 (abandon -- still not done and the deadline stands).
int screen_mps_test_ping_toctou_decide(bool done_now,
									   bool deadline_now_reached);
#endif

// menus_dwell hardware profile (menu_autopilot.c): the bottom action-button
// strip (Play/Rename/Delete/Create/Cancel) is pointer-only by design -- a
// D-pad-only GUI_CLICK always activates the row-selected world instead
// (sworld_activate_button's default path has no keyboard/D-pad route to
// SWORLD_BUTTON_CREATE). Exposes "Create New World"'s on-screen center so a
// synthetic IR-pointer click -- the same input shape a real Wiimote
// produces -- can reach it with no new interaction semantics added to this
// screen. Always returns true (present in every layout); width/height are
// the physical framebuffer size, matching gfx_width()/gfx_height().
bool screen_sworld_create_button_center(int width, int height, float* cx,
										 float* cy);

// menus_dwell hardware profile (menu_autopilot.c, 2026-09-02 hardening):
// exposes the create-world form's live Game Mode selection so the script can
// VERIFY a GUI_LEFT/RIGHT press actually landed before proceeding, instead of
// trusting a single bare one-frame press -- see menu_autopilot_script.h's
// design note (staged_menus_dwell_20260902_063552's adversarial review named
// the old bare-press step as the #1 suspect for creating the world CREATIVE
// instead of SURVIVAL). Meaningless (but harmless to call) while
// screen_create_world is not the active screen.
bool screen_cworld_mode_is_survival(void);
// menus_dwell walker (menus-dwell-walker-1026): side-effect-free reads of the
// live create-world form. editing = the name/seed editor modal is open.
// create_ready = the modal is closed and the focus ring is on an ENABLED
// Create, so an activate press would attempt the create. Meaningless (but
// harmless) while screen_create_world is not the active screen.
bool screen_cworld_editing(void);
bool screen_cworld_create_ready(void);

/* CROSS-LANE OBSERVERS (push-0921 A5, Astra ruling 2 item 7; consumer: lane
 * B's mp_play_10m walker). Each answers ONE question -- "would an activate
 * press THIS frame open Multiplayer / start the join?" -- by asking the SAME
 * gutil_menu_activate_decide the screen's own press asks, with the same
 * inputs. They are side-effect-free: they press nothing, consume nothing and
 * advance no state, and asking twice answers twice the same.
 *
 * They are therefore FALSE, not merely "focused elsewhere", whenever the
 * press would not do that thing: while the sprite rests on a different
 * control (the press is consumed by the mismatch rule), while the control is
 * disabled, while the activate button is still held (no fresh edge can
 * arrive), while another screen or a sub-editor is up -- and, for the join,
 * whenever the press would park in the player-name editor instead of joining
 * (no chosen name) or there is no server to join. */
/* THE TITLE (approved four-row title, design-wiring/title-green): "would an
 * activate press THIS frame open the PLAY path" -- the Play game row, which
 * resets the retained Play session and opens the Load tab (screen_select_world;
 * the multiplayer path is now its Join tab). Rows top to bottom: Play game /
 * Texture packs / Options / Quit game; the title OPENS on Play game, so from
 * the opening focus the Play row is reached by ZERO GUI_DOWN steps (a core
 * navigating by GUI_DOWN + re-ask finds it true before its first step; UP from
 * any lower row walks back onto it). */
bool screen_title_focus_is_play(void);
/* menus_dwell walker (menus-dwell-walker-1026): the title row the selection
 * is on (Play game / Tutorial world / Texture packs / Options / Quit game,
 * ui-build-1026 T03), or SCREEN_TITLE_FOCUS_NONE when the title is not the
 * screen up. Read-only. The ordinals ARE the visual order, top to bottom:
 * the walker (menu_autopilot_script.h menu_ap_title_focus, pinned equal by
 * menu_autopilot.c's _Static_assert) steps DOWN when below its target and UP
 * when past it. */
enum screen_title_focus {
	SCREEN_TITLE_FOCUS_NONE = -1,
	SCREEN_TITLE_FOCUS_PLAY = 0,
	SCREEN_TITLE_FOCUS_TUTORIAL,
	SCREEN_TITLE_FOCUS_PACKS,
	SCREEN_TITLE_FOCUS_OPTIONS,
	SCREEN_TITLE_FOCUS_QUIT,
};
int screen_title_focused_row(void);
/* The Tutorial world row (ui-build-1026 T03) opens the SHIPPED tutorial world:
 * the folder tools/make_release.sh stages as saves/<this>/ (build_tutorial.py
 * DEFAULT_OUT), looked up under "paths.worlds". It RESUMES that folder through
 * the ordinary Load-tab world load; a missing folder opens an honest
 * explanation instead. */
#define SCREEN_TITLE_TUTORIAL_DIR "CaveWii Tutorial"
/* TRANSITION ALIAS of screen_title_focus_is_play -- the standalone Multiplayer
 * row no longer exists. Kept DEFINED (one line in screen_title.c) so main.c's
 * mpp_hook_menu_focus links unchanged until the joint seam commit (Load-tab
 * phase in mp_play_core + retire this alias) lands together with the title;
 * new callers use screen_title_focus_is_play. */
bool screen_title_focus_is_multiplayer(void);
bool screen_mps_focus_is_join(void);
/* THE LOAD TAB (design-wiring/load-green): "would a fresh activate press THIS
 * frame open the JOIN tab" -- false when another screen or the World options
 * modal is live, false while the activate button is still held, otherwise the
 * screen's own decision on the last update's inputs with target = the Join
 * tab header. Route: the Load tab opens on the selected row (or its header
 * with no worlds); GUI_UP from the first row = header; GUI_RIGHT on the header
 * walks Load -> Create -> Join (wrapping). screen_load_focus_is_join (the
 * server row + Join inside the Join tab) belongs to the Join slice and is NOT
 * defined yet. */
bool screen_load_focus_is_join_tab(void);

/* WHERE DIRECTIONAL FOCUS RESTS on the two list screens, named by MEANING
 * (push-0921 A8 add-on; consumer: lane B's menu walker, which has to know
 * which control it is on without duplicating either screen's private enums).
 * Both observers are PRODUCTION-visible and side-effect-free: they press
 * nothing, consume nothing and advance no state, and each is a pure mapping of
 * the same private focus-target function the screen's own render and
 * activation decision read -- so an observer can never describe a different
 * screen than the one that ships.
 *
 * _NONE means "no actionable focus": an empty list under the ring, or the
 * ruled no-safe-destination state. Calling either while its screen is not the
 * live one is harmless but meaningless. */
enum screen_sworld_focus {
	SCREEN_SWORLD_FOCUS_NONE = 0,
	SCREEN_SWORLD_FOCUS_LIST, /* a world row; the activate SELECTS it and
							   * moves focus to Play (Load contract) */
	/* LEGACY strip names (Beta's five-button strip). The approved Load tab
	 * has no such strip, so these are never reported any more; kept only so
	 * existing consumers compile until they adopt the names below. */
	SCREEN_SWORLD_FOCUS_SELECT,
	SCREEN_SWORLD_FOCUS_RENAME,
	SCREEN_SWORLD_FOCUS_DELETE,
	SCREEN_SWORLD_FOCUS_CREATE, /* reported for the empty list's "Create a world" */
	SCREEN_SWORLD_FOCUS_CANCEL,
	/* The approved Load tab's controls (design-wiring/load-green). */
	SCREEN_SWORLD_FOCUS_TAB_LOAD,
	SCREEN_SWORLD_FOCUS_TAB_CREATE,
	SCREEN_SWORLD_FOCUS_TAB_JOIN,
	SCREEN_SWORLD_FOCUS_PREV,
	SCREEN_SWORLD_FOCUS_NEXT,
	SCREEN_SWORLD_FOCUS_PLAY,
	SCREEN_SWORLD_FOCUS_RETRY, /* the Play slot while the enumeration failed */
	SCREEN_SWORLD_FOCUS_OPTIONS,
	SCREEN_SWORLD_FOCUS_MODAL_RENAME,
	SCREEN_SWORLD_FOCUS_MODAL_DELETE,
	SCREEN_SWORLD_FOCUS_MODAL_BACK,
};
int screen_sworld_focus_target(void);

enum screen_mps_focus {
	SCREEN_MPS_FOCUS_NONE = 0,
	SCREEN_MPS_FOCUS_LIST, /* a row press selects and focuses explicit Join */
	SCREEN_MPS_FOCUS_NAME, /* the player-name field */
	SCREEN_MPS_FOCUS_QUICK,
	SCREEN_MPS_FOCUS_JOIN,
	SCREEN_MPS_FOCUS_DIRECT,
	SCREEN_MPS_FOCUS_ADD,
	SCREEN_MPS_FOCUS_EDIT,
	SCREEN_MPS_FOCUS_DELETE,
	SCREEN_MPS_FOCUS_REFRESH,
	SCREEN_MPS_FOCUS_BACK,
	SCREEN_MPS_FOCUS_OPTIONS,
	SCREEN_MPS_FOCUS_TAB_LOAD,
	SCREEN_MPS_FOCUS_TAB_CREATE,
	SCREEN_MPS_FOCUS_TAB_JOIN,
	SCREEN_MPS_FOCUS_PREV,
	SCREEN_MPS_FOCUS_NEXT,
};
int screen_mps_focus_target(void);
int screen_mps_server_count(void);

/* Which of the multiplayer screen's views is up. ADD and EDIT are one view
 * internally (the server editor, entered new or on a row); they are reported
 * apart because that is the distinction a walker acts on. */
enum screen_mps_view {
	SCREEN_MPS_VIEW_MAIN = 0,
	SCREEN_MPS_VIEW_NAME,
	SCREEN_MPS_VIEW_ADD,
	SCREEN_MPS_VIEW_EDIT,
	SCREEN_MPS_VIEW_DIRECT,
};
int screen_mps_view(void);

/* How many world rows Select World is currently listing. Read-only,
 * production-visible, same rationale as the observers above (push-0921 A10
 * item 1; review A9-5): the walker's delete sub-leg proves "no world was
 * destroyed" by checking that focus still reports a LIST row, which is only a
 * real proof on a ONE-world card -- with several worlds, deleting one still
 * leaves rows. A COUNT taken before and after is the proof that holds on the
 * maintainer's real card. 0 when the list is empty or not built. */
int screen_sworld_row_count(void);
/* ui-build-1026/T16a: read-only row observers for the world_copy_convert
 * route. settled: every row's level.dat has been resolved. selected_row: the
 * selected row index, -1 none. row_of_directory: the row whose DIRECTORY is
 * exactly `directory`, -1 absent. row_needs_convert: that resolved row carries
 * "Must be converted". partial_copy_visible: an UNREADABLE row named
 * "... (backup...)" (a world_copy reservation/partial) is listed. */
bool screen_sworld_settled(void);
int screen_sworld_selected_row(void);
int screen_sworld_row_of_directory(const char* directory);
bool screen_sworld_row_needs_convert(int row);
bool screen_sworld_partial_copy_visible(void);

/* The confirmation is the live screen AND its highlighted answer is the
 * NON-DESTRUCTIVE one the opener declared (screen_confirm_setup's
 * safe_answer). Read-only (review A9-6: the walker backs out of the delete
 * confirm with Close, which answers NO unconditionally -- if that ever
 * regressed to answering the highlight, the leg has no way to see it. This is
 * that way). False whenever the confirmation is not up. */
bool screen_confirm_focus_is_safe(void);

/* ui-build-1026 T15: menus_dwell walker observers for the approved screens.
 * Every one is a side-effect-free read of state the screen's own update and
 * render already use: no locks, no allocation, nothing drawn, nothing
 * consumed. Meaningless (but harmless) while their screen is not up; the
 * walker gates on the classified screen first.
 *
 * TEXTURE PACKS. Rows are the visible list rows (the same index the screen's
 * one-click decision selects). focused_row is the row the ring rests on, or
 * -1 when a strip button (Category / Apply / Done) has it. pending_selected:
 * the pending choice Apply would commit is the selected row's pack. busy: an
 * Apply has been accepted and the deferred reload is in flight. row_center:
 * the centre of a row's card at this size, false when the row is not fully
 * inside the list viewport (texpacks_row_rect, the render/hit-test rect). */
int screen_texpacks_row_count(void);
int screen_texpacks_selected_row(void);
int screen_texpacks_focused_row(void);
bool screen_texpacks_apply_focused(void);
bool screen_texpacks_apply_enabled(void);
bool screen_texpacks_pending_selected(void);
bool screen_texpacks_busy(void);
bool screen_texpacks_row_center(int row, int width, int height, float* cx,
								float* cy);
/* CREATE WORLD. The focus target BY MEANING (values pinned to the screen's
 * private enum by _Static_assert in screen_create_world.c), whether the
 * Details card is up, and the OSK modal: its profile (enum
 * osk_frontend_profile) and focused key target (enum osk_frontend_target),
 * both -1 while no editor is open. text_signature hashes the committed name
 * and seed fields (FNV-1a, a NUL between them): while the editor is open the
 * edited field holds the draft, so a typed key changes it and a Cancel
 * restores it. */
enum screen_cworld_focus {
	SCREEN_CWORLD_FOCUS_NAME = 0,
	SCREEN_CWORLD_FOCUS_SEED,
	SCREEN_CWORLD_FOCUS_NEW_SEED,
	SCREEN_CWORLD_FOCUS_MODE_SURVIVAL,
	SCREEN_CWORLD_FOCUS_MODE_CREATIVE,
	SCREEN_CWORLD_FOCUS_CREATE,
	SCREEN_CWORLD_FOCUS_DETAILS,
	SCREEN_CWORLD_FOCUS_CANCEL,
	SCREEN_CWORLD_FOCUS_DETAILS_BACK,
};
int screen_cworld_focus_target(void);
bool screen_cworld_details_open(void);
int screen_cworld_osk_profile(void);
int screen_cworld_osk_target(void);
unsigned screen_cworld_text_signature(void);
/* INVENTORY. The page (0 Inventory, 1 Crafting) and the Crafting page's
 * category tab, each -1 while screen_inventory is not up (tab: also -1 on
 * the Inventory page). */
int screen_inventory_page(void);
int screen_inventory_crafting_tab(void);

void screen_crafting_set_windowc(uint8_t container);
void screen_furnace_set_windowc(uint8_t container);
void screen_chest_set_windowc(uint8_t container);
bool screen_chest_sort_open(void); /* the chest's Sort dialog is up (menus_dwell walker) */
void screen_dispenser_set_windowc(uint8_t container);
void screen_iron_chest_set_windowc(uint8_t container);
void screen_sign_set_windowc(uint8_t container);

#endif
