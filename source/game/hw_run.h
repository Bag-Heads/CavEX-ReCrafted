/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

#ifndef GAME_HW_RUN_H
#define GAME_HW_RUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --cw-far-view-distance override bounds (Stage-2 LOD reach gate,
// docs/stage2_lod_plan_2026-09-03.md 3, 2026-09-03). Mirrored, NOT shared --
// hw_run.h/.c are deliberately dependency-light (see CMakeLists.txt's
// test_hw_run_args_parse comment: it links hw_run.c alone, with no
// PLATFORM_PC define and none of the engine's heavy headers), so these
// cannot simply #include source/network/server_local_state.h's real
// MAX_VIEW_DISTANCE / MAX_FAR_VIEW_DISTANCE_CAP without dragging in world.h/
// entity.h/m-lib and breaking that standalone build. MUST stay in step with
// that header's values by hand -- same "mirrored, if either changes keep
// the other in step" discipline hw_run.c already applies to
// HW_RUN_ARGS_FILE_MAX_BYTES/HW_RUN_ARGS_FILE_MAX_ARGC below.
#ifdef PLATFORM_PC
#define HW_RUN_FAR_VIEW_DISTANCE_MIN 10 // == server_local_state.h MAX_VIEW_DISTANCE (PC)
#else
#define HW_RUN_FAR_VIEW_DISTANCE_MIN 5 // == server_local_state.h MAX_VIEW_DISTANCE (Wii)
#endif
// == server_local_state.h MAX_FAR_VIEW_DISTANCE_CAP: 16 chunks on BOTH
// platforms (2026-09-03, Stage-2 reach sweep -- matches Beta 1.7.3's "Far"
// render-distance setting). Split by platform the same way that header is,
// so each keeps its own "+K" offset from its own MIN while landing on the
// same absolute 16: PC's +6 already lands there unchanged; Wii's moves from
// +6 (11) to +11 (16).
#ifdef PLATFORM_PC
#define HW_RUN_FAR_VIEW_DISTANCE_MAX (HW_RUN_FAR_VIEW_DISTANCE_MIN + 6) // 10+6 == 16
#else
#define HW_RUN_FAR_VIEW_DISTANCE_MAX (HW_RUN_FAR_VIEW_DISTANCE_MIN + 11) // 5+11 == 16
#endif

// --cw-far-storage override (S2 measurement, docs/stage2_lod_plan_2026-09-03.md).
// Mirrors world.h's enum world_far_storage (WORLD_FAR_STORAGE_VOXEL/SKIN).
// Wired into source/main.c (2026-09-03, candidates-6 follow-up): consumed
// right after the far_view_distance clamp block / "lod.far_storage" config
// read, overriding it only when an armed hw_run profile explicitly set this
// field away from HW_RUN_FAR_STORAGE_UNSET -- the effective value (post-
// override) is logged via fprintf("[config] lod.far_storage = ... (effective:
// ...)"). No PASS.json/heartbeat field yet: hw_telemetry.h's far_view_distance
// neighbor would be the natural home if a future measurement needs one
// recorded per-run; not added here since no consumer reads it yet.
// --cw-far-send-near-owed upper bound (T2 far-send share, 2026-09-04). The
// near square holds (2*MAX_VIEW_DISTANCE+1)^2 columns, so any threshold at or
// above that is "always share" -- well past useful. 32 is a generous ceiling
// for a measurement sweep while keeping the near-owed scan's short-circuit
// (server_local.c: it stops at threshold+1 misses) cheap on the Broadway core.
#define HW_RUN_FAR_SEND_NEAR_OWED_MAX 32

// --cw-set key=value cap (run-scoped config override for staged hardware
// runs, 2026-09-08, lane/carve-observer-boot). 8 is generous for a paired
// A/B measurement line (the motivating case sets exactly one key) while
// keeping the fixed hw_run_config array small and every malformed/over-cap
// entry a cheap, non-fatal, counted no-op (see cw_set_malformed below).
#define HW_RUN_CW_SET_MAX 8

enum hw_run_far_storage {
	// Absent: config.json's "lod.far_storage" decides (default "voxel").
	HW_RUN_FAR_STORAGE_UNSET = 0,
	HW_RUN_FAR_STORAGE_VOXEL,
	HW_RUN_FAR_STORAGE_SKIN,
};

enum hw_run_profile {
	HW_RUN_PROFILE_FAR_IDLE_2M = 0,
	HW_RUN_PROFILE_HEARTBEAT_SELFTEST,
	HW_RUN_PROFILE_FAR_WALK_6M,
	HW_RUN_PROFILE_PERF_STRESS_5M,
	HW_RUN_PROFILE_WATCHDOG_INJECT,
	HW_RUN_PROFILE_REDSTONE_CLOCK_2M,
	HW_RUN_PROFILE_MENUS_DWELL,
	HW_RUN_PROFILE_SAVE_MIGRATE,
	HW_RUN_PROFILE_MP_CONNECT_SMOKE,
	HW_RUN_PROFILE_MP_LAN_WALK_10M,
	HW_RUN_PROFILE_MP_RECONNECT_SOAK,
	// menus_dwell_creative (2026-09-02): a DELIBERATE hardware reproduction
	// knob, not a new mechanism -- staged_menus_dwell_20260902_063552 hung
	// silently at STEP_PAUSE_RESUME after the menu autopilot's game-mode
	// selector accidentally created a CREATIVE world; the identical profile
	// with a Survival world (run E) passed. Same script, same timing, same
	// duration as HW_RUN_PROFILE_MENUS_DWELL -- the ONLY difference is
	// cfg->cworld_mode below, so a repro run can be requested on purpose
	// instead of relying on a selector bug to produce one by accident.
	HW_RUN_PROFILE_MENUS_DWELL_CREATIVE,
	// keepup_walk_6m (2026-09-04, deterministic keep-up profile): far_walk_6m's
	// autopilot walk is NOT run-to-run deterministic (PROJECT_MEMORY:
	// max displacement 82/24/67 blocks across three real-Wii runs of the same
	// duration/gate), which makes A/B tris/chunks_meshed comparisons between
	// two builds noise rather than signal. Root cause (autopilot_keepup.h):
	// far_walk_6m's curving-yaw walk driver (autopilot.c mode 0) never resets
	// its heading drift, so on flat ground it traces a constant-curvature
	// ~43-block-radius CIRCLE that loops back near spawn every ~63s -- and
	// because every run boots a brand-new randomly-seeded world ("+new"),
	// terrain-driven collision deflection (and the occasional unlucky
	// fall/drown -> death -> Respawn-to-spawn) perturbs that circle by a
	// different, terrain-dependent amount each run. This profile arms a
	// separate walk driver (autopilot.c mode 2 / autopilot_keepup.h) with a
	// FIXED heading that never drifts and a bounded, non-escalating,
	// deterministic sidestep-only stuck escape (no wobble, no accumulating
	// turn) -- see autopilot_keepup.h's top comment for the full RCA and the
	// documented residual limitation (this fixes the SCRIPT, not the
	// per-run random world seed).
	HW_RUN_PROFILE_KEEPUP_WALK_6M,
	HW_RUN_PROFILE_SPLIT_ANCHOR_6M,
	// split_play_2m (2026-09-07, lane/guest-body): the first profile with a
	// REAL second gameplay body rather than split_anchor_6m's synthetic +X
	// anchor. Arms "experimental.local_splitscreen", auto-joins the guest as
	// soon as the world is ready, and walks BOTH bodies for the measured
	// window so the two-anchor residency scheduler is exercised by two
	// independently moving players. The maintainer holds the second
	// controller for the eyeball; the telemetry is the anchor/carve record.
	HW_RUN_PROFILE_SPLIT_PLAY_2M,
	// split_play_8m (2026-09-10, Astra ruling): the CERTIFYING guest-body
	// profile. Same two real bodies as split_play_2m, but the measured
	// window is six minutes on the readiness epoch (like split_anchor_6m)
	// with a 120 s installed-readiness budget enforced by the collector.
	// split_play_2m stays the 120 s smoke (readiness by 90 s, >= 30 s
	// measured) plus the maintainer's second-controller eyeball.
	HW_RUN_PROFILE_SPLIT_PLAY_8M,
	// split_actions_6m (2026-09-14, mp-split-0912/68 S1-d): the per-owner
	// ACTION proof. Same two real bodies and the same six measured minutes on
	// the readiness epoch as split_play_8m, plus a scripted guest action
	// sequence (hotbar -> hotbar -> dig -> use -> place, one step every 40
	// client ticks) driven through the REAL slot-1 mailbox while the host
	// performs no action at all. The collector requires the guest's accepted
	// dig/place/hotbar/use tallies to clear their floors AND the host's to be
	// all zero, so the profile FAILS when the per-owner effects are absent --
	// which is the whole point of it existing next to split_play_8m rather
	// than inside it (Astra gate-3).
	HW_RUN_PROFILE_SPLIT_ACTIONS_6M,
	// split_recovery_6m (2026-09-17, robot-recovery-0917): the scripted
	// RECOVERY proof. Same superflat/force_overworld guest fixture as
	// split_actions_6m, but NOTHING walks: a phase driver
	// (source/game/split_recovery_script.[ch], contract in
	// docs/split_recovery_profile.md) drives controller LOSS -> neutral-gated
	// recovery, guest LEAVE -> rejoin, and host SAVE-AND-QUIT -> title ->
	// in-process RELOAD, asserting each phase from OBSERVED production state.
	// It is the ONLY profile whose generic duration-based profile_done "ok" is
	// disabled (main.c): only the driver may complete this run, and it fails
	// the run on any missed assertion or phase timeout. Astra ruling
	// 2026-09-17; the PASS label is "scripted real-Wii proxy PASS; physical
	// unplug still owed".
	HW_RUN_PROFILE_SPLIT_RECOVERY_6M,
	// mp_chat_login_3m (2026-09-17, b1-chat-keyboard-0917): the scripted MP
	// CHAT COMMAND DELIVERY proof. Base shape is mp_connect_smoke
	// (mp_autoconnect, no autoload) at 180 s; a phase driver
	// (source/game/chat_login_script.[ch], contract in
	// docs/chat_login_profile.md) binds a VIRTUAL pad as player 0, opens the
	// composer through the real D-pad-Up opener (chord gate included), walks
	// the on-screen keyboard to spell "/login hunter22", submits it once and
	// observes the server's reply arrive in gstate.chat_ring. Like
	// split_recovery_6m its generic duration-based profile_done "ok" is
	// DISABLED (main.c): only the driver may complete this run. The PASS label
	// is "scripted real-Wii OSK command delivery on LAN: PASS" -- three
	// controllers, ownership/reconnect, human timing, TV readability and
	// BetaMC all stay open.
	HW_RUN_PROFILE_MP_CHAT_LOGIN_3M,
	// mp_chat_chord_1m (2026-09-19, b1-chat-opener-0919, Astra ruling 2 D2):
	// the scripted DEBUG-CHORD leg, deliberately SEPARATE from
	// mp_chat_login_3m so a chord claim can never be read out of a login run
	// ("an 800 ms refusal must never be presented as hardware chord proof").
	// Same LAN join shape at 60 s, the overlay forced OFF first so the run
	// starts from a KNOWN state, then the Minus+Plus chord in BOTH press
	// orders with neutral gaps: the overlay action must fire each time, chat
	// must NOT open during or right after a chord, and after a neutral re-arm
	// the ordinary opener gesture must still open the composer (then close
	// it), leaving the overlay OFF. Driver-completed, like the profile above.
	// Contract: docs/chat_login_profile.md section 0.
	HW_RUN_PROFILE_MP_CHAT_CHORD_1M,
	// mp_play_10m (2026-09-21, push-0921/B4c): the unattended LAN PLAY +
	// REJOIN + STATE-VERIFY proof. Same join shape as the two profiles above
	// (mp_autoconnect, no autoload, driver-owned completion), stretched to a
	// 21-minute window. A phase driver (source/game/mp_play_core.h as the
	// pure machine + source/game/mp_play_script.[ch] as the device half,
	// contract in docs/mp_play_profile.md) joins, logs in through the SHARED
	// chat_login implementation, plays for at least 600 000 measured ms
	// (walks, digs, collects, places, moves inventory stacks), leaves through
	// the production pause menu, rejoins through the production title ->
	// Multiplayer screens, logs in again and verifies its server-owned
	// position, dimension and inventory are unchanged.
	//
	// APPENDED, never inserted: every value above keeps its ordinal (the
	// collector, the staged arg files and four PowerShell ValidateSets are
	// all keyed on the NAME, but tests/test_hw_telemetry.c's per-profile
	// tables are keyed on the enum ORDINAL, and a run staged before a rebuild
	// would silently change profile).
	//
	// What this profile deliberately does NOT arm: autopilot_walk /
	// autopilot_keepup / autopilot_aim / autopilot_place. The driver steers
	// the walker one leg at a time through autopilot.c's runtime seam
	// (autopilot_script_walk / _aim) instead, and a config-armed autopilot
	// would additionally script screen_death's Respawn hold
	// (autopilot_force_button) -- which Astra gate 1 Lane B item 5 forbids:
	// "No automatic respawn, teleport-assisted displacement or fixture
	// restoration may conceal failure." A death FAILS this run.
	HW_RUN_PROFILE_MP_PLAY_10M,
	HW_RUN_PROFILE_MP_PAUSE_MENU_3M,
	/* Scripted checklist 6b; appended to preserve every existing ordinal. */
	HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M,
	/* Profile foundation; reuses the portal-quit fixture until lane B wires
	 * the independent-dimensions script. Keep all earlier ordinals stable. */
	HW_RUN_PROFILE_SPLIT_DIMS_6M,
	/* motion-proof-0927 (lane C, mob motion units S2): the three-check mob
	 * motion fixture (TNT lift, Beta dry recurrence, path step jump) with its
	 * verdict on the heartbeat. Appended: every earlier ordinal is stable. */
	HW_RUN_PROFILE_MOB_MOTION_2M,
	/* vehicle-smoke-1004 (CONTRACT-v1, tmp/jobs/vehicle-smoke-1004): the
	 * functional host-vehicle smoke, one process per vehicle kind
	 * (--cw-vehicle-kind boat|pig|cart, REQUIRED for this profile and refused
	 * for every other). Peaceful, superflat, split ON, world payloads ON, its
	 * own disposable world cw_vehicle_rig; the phase driver
	 * (source/game/vehicle_smoke_script.c) owns completion. Appended: every
	 * earlier ordinal is stable. */
	HW_RUN_PROFILE_HOST_VEHICLE_SMOKE_8M,
	/* colcap-1006 (ASTRA-CC-R5 item 2): column-pool T1 proof ON THE WII.
	 * (a) at boot, before any world thread exists, churns the REAL MEM2
	 * column pool (grow / lowest-first reuse / drain / hand-back / forced
	 * heap-return refusal / regrow) and checks guards, ownership and slot
	 * addresses; (b) leaves a fixed fragmented ballast in the boot slabs,
	 * loads seed 12345 with no far ring and no input, waits for the scene
	 * to settle and hashes the ordered GX display-list submissions of that
	 * scene. --cw-pool-placement rotating|lowest selects the column take
	 * for THIS profile only (refused for any other); the same build is run
	 * once per placement and the collector compares the two hashes.
	 * Appended: every earlier ordinal is stable. */
	HW_RUN_PROFILE_POOL_SELFTEST_3M,
	/* colcap-1006 (ASTRA-CC-R5 item 3): split_anchor_6m's synthetic two-anchor
	 * walk (requires --cw-anchor-sep > 0) with a SEPARATE / REUNITE /
	 * SEPARATE route on the readiness clock: the second anchor stands at
	 * +sep for 90 s (load growth + mesh churn), at the player for 90 s (its
	 * columns evicted, column slabs drain and go back), then at +sep again
	 * for the rest of the 360 measured seconds -- so the pool must REGROW to
	 * 7/8 slabs in a warmed, display-list-fragmented heap. Appended. */
	HW_RUN_PROFILE_SPLIT_REGROW_8M,
	/* Bound offered body piston proof; appended, earlier ordinals stable. */
	HW_RUN_PROFILE_SPLIT_OFFER_PISTON_2M,
	/* ui-build-1026/T16a: the "Back up & convert" proof route on a
	 * disposable Beta fixture in a per-run worlds root (main.c). Appended. */
	HW_RUN_PROFILE_WORLD_COPY_CONVERT,
	/* SPEC R12 real-Wii proof: fresh seeded world, fixed fixture, Save and
	 * quit through the real pause menu, reopen the same folder through the
	 * real select-world load, hash compare (source/game/save_reload_script.h,
	 * source/network/save_reload_probe.h). Appended. */
	HW_RUN_PROFILE_SAVE_RELOAD_3M,
};

/* host_vehicle_smoke_8m's run kind. NONE = not given (a parse error for that
 * profile, the only legal value for any other). */
enum hw_run_vehicle_kind {
	HW_RUN_VEHICLE_NONE = 0,
	HW_RUN_VEHICLE_BOAT,
	HW_RUN_VEHICLE_PIG,
	HW_RUN_VEHICLE_CART,
};

/* host_vehicle_smoke_8m's PREDECLARED p4 transfer mode (CONTRACT-v1 ruling 7:
 * declared before the run, never chosen after a timed-out contact attempt). */
enum hw_run_vehicle_transfer {
	HW_RUN_VEHICLE_TRANSFER_FIXTURE_REQUEST = 0,
	HW_RUN_VEHICLE_TRANSFER_CONTACT,
};

// The create-world Game Mode the menus_dwell script's selector should land
// on (and verify, via screen_cworld_mode_is_survival()/the form model)
// before pressing Create. Default is SURVIVAL for every profile -- this
// knob only ever changes behavior when a caller explicitly asks for
// creative, either via HW_RUN_PROFILE_MENUS_DWELL_CREATIVE or the
// --cw-cworld-mode CLI flag.
enum hw_run_cworld_mode {
	HW_RUN_CWORLD_MODE_SURVIVAL = 0,
	HW_RUN_CWORLD_MODE_CREATIVE,
};

struct hw_run_config {
	bool tps_cpu; /* --cw-tps-cpu: qualified thread CPU observer */
	// --cw-settle: post-world_loaded settle observer (source/main.c arms
	// settle_observer_start() on this field alone). Split from tps_cpu
	// (2026-09-07, lane/observer-arm-flags) so the tps census and the
	// settle observer can be bisected independently on real hardware --
	// before this field existed, main.c armed settle_observer_start() on
	// hw_run.tps_cpu, so the two observers could never be run one-without-
	// the-other on a real Wii. Byte-identical for every existing caller
	// that never passes --cw-settle: this field defaults false (memset in
	// hw_run_config_init), so an unarmed run and a --cw-tps-cpu-only run
	// both leave the settle observer OFF, same as before this field
	// existed for the tps-only case, and turn it on only when explicitly
	// requested.
	bool settle;
	bool carve_census; /* --cw-carve-census: allocation census */
	// --cw-tel-nonblock: the opt-in non-blocking telemetry-send EXPERIMENT
	// (2026-09-08 sender-wedge lane; see hw_telemetry.h's
	// hw_telemetry_set_nonblock_experiment doc for the mechanism and why the
	// blocking socket remains the default). main.c passes this field, and
	// nothing else, into hw_telemetry before init. Defaults false via
	// hw_run_config_init's memset, so every existing profile and caller is
	// byte-identical to before this field existed.
	bool telemetry_nonblock;
	bool column_census; /* --cw-column-census: read-only Slice-1 observer */
	int anchor_sep; /* Fixed +X separation in blocks; zero keeps one anchor. */
	// --cw-split-guest / split_play_2m: arm the EXPERIMENTAL local
	// split-screen switch for this run and auto-admit a guest body once the
	// world is ready, so a headless harness can measure two real anchors.
	// main.c reads this DIRECTLY (never through a "debug.*"/"experimental.*"
	// config key) -- same rationale as autopilot_unstick/cworld_mode: a
	// hand-edited or capture config can never arm the auto-join.
	bool split_guest;
	// Walk the guest body on a fixed heading (degrees) while split_guest is
	// armed, so the harness moves BOTH bodies. Ignored without split_guest.
	bool split_guest_walk;
	/* --cw-pin-cameras (debug capture lever, default off; B2 T4 2026-09-26):
	 * main.c renders every view from a fixed pose (latched at load) with
	 * a fixed clock and clear weather once the world is loaded, so two
	 * builds' frames compare pixel for pixel (turn the forced F3 overlay off
	 * with --cw-set debug.hw_overlay=0). The profile's scripted
	 * drivers still run (they keep the split views "driven"); only the
	 * RENDER pose is pinned. Nothing changes unless this flag is on. */
	bool pin_cameras;
	int split_guest_yaw_deg;
	// --cw-split-actions / split_actions_6m (S1-d): run the scripted GUEST
	// action sequence once installed readiness latches. Ignored without
	// split_guest (there is no guest body to act with). Like split_guest
	// itself this is read DIRECTLY from the run config, never from a
	// "debug.*" config key, so no hand-edited or capture config can arm a
	// harness that mutates the world.
	bool split_guest_actions;
	// split_recovery_6m: arm the scripted recovery phase driver. Set ONLY by
	// that profile's hw_run_apply_profile case -- there is deliberately no
	// --cw-* flag for it, and no "debug.*"/"experimental.*" config key either,
	// because arming it lets a run DELETE a world directory and drive the
	// host's Save and quit. Naming the profile is the only way in.
	// split_recovery_arm() additionally requires split_guest: without a guest
	// body there is nothing to lose, leave or re-admit.
	bool split_recovery;
	// host_vehicle_smoke_8m: arm the vehicle smoke phase driver. Set ONLY by
	// that profile's hw_run_apply_profile case (same "naming the profile is
	// the only way in" rule as split_recovery: the driver creates a world and
	// drives Save and quit, death and a dimension transfer). vehicle_kind is
	// the --cw-vehicle-kind value; vehicle_transfer the predeclared p4 mode.
	bool vehicle_smoke;
	enum hw_run_vehicle_kind vehicle_kind;
	enum hw_run_vehicle_transfer vehicle_transfer;
	/* One dwell threshold per run: 0, 1, 2, 3 or 5 additions remaining. */
	int portal_remaining_ticks;
	/* residency-1005/4 closure item 12 (route E): the default-off runtime
	 * residency fault arm, --cw-rz-fault <host|guest>@<seconds>. After that
	 * many seconds of the run, the named actor's CURRENT footprint columns
	 * are made unavailable for ONE bounded recovery attempt (see
	 * server_local_rz_fault_request). Same release artifact, no test build. */
	bool rz_fault_set;
	int rz_fault_actor;		/* 0 host, 1 guest */
	int rz_fault_delay_s;
	bool enabled;
	enum hw_run_profile profile;
	char profile_name[32];
	char run_id[64];
	char telemetry_host[64];
	char telemetry_netmask[16];
	int telemetry_port;
	bool telemetry_pull;
	int duration_seconds;
	// The scripted phase driver -- not the generic duration timer -- decides
	// when this run is over and what its verdict is (main.c). Set by
	// hw_run_apply_profile for exactly the driver-owned profiles, so adding
	// one never means editing main.c again.
	bool driver_owns_completion;
	// Pin the F3-style HUD on for the whole run. TRUE BY DEFAULT for every
	// profile (hw_run_apply_profile), and it does more than draw text -- read
	// this before leaving it on for a profile whose driver presses buttons.
	//
	// main.c writes "debug.overlay" = "force" into config_user from this field,
	// AFTER the card's config.json has been loaded, so it overrides the card
	// rather than defaulting behind it. The resulting
	// gstate.force_debug_overlay then takes the HOTBAR SCROLL away from the
	// hotbar: screen_ingame.c discards the step and main.c re-reads the same
	// IB_SCROLL_LEFT/RIGHT press as the far-ring LOD reach dial, which is the
	// point of the mode (crank the reach, watch the fps, find the real-Wii GPU
	// ceiling Dolphin cannot show).
	//
	// So a profile whose driver cycles the held slot, or measures the F3
	// toggle, must clear it: mp_chat_chord_1m (the toggle is what it observes)
	// and mp_play_10m (its driver cannot reach a placeable block without the
	// hotbar step, push-0921/B9) both do. Every profile's value is pinned as a
	// column in test_hw_run_args_parse's profile table.
	bool force_overlay;
	bool autoload_new;
	// Non-empty: autoload THIS existing world by directory name instead of
	// the "+new" sentinel (StormWorld-class re-timing of real saves).
	// Overrides the profile's autoload_new. screen_select_world treats any
	// non-"+new" autoload value as a directory name to find and load.
	char autoload_name[64];
	bool superflat;
	bool force_overworld;
	bool perf_stress;
	bool autopilot_walk;
	// Deterministic keep-up walk driver (autopilot.c mode 2 /
	// autopilot_keepup.h). Set ONLY by HW_RUN_PROFILE_KEEPUP_WALK_6M's
	// hw_run_apply_profile case -- a SEPARATE flag from autopilot_walk above
	// (never both true for the same profile) so far_walk_6m's established
	// mode-0 trajectory is untouched by this field's existence. main.c reads
	// this DIRECTLY (never through a "debug.*" config key, same rationale as
	// autopilot_unstick below) and sets debug.autopilot="keepup".
	bool autopilot_keepup;
	// Opt-in walker stuck-detector (source/game/autopilot_unstick.h). Set
	// ONLY by HW_RUN_PROFILE_MP_LAN_WALK_10M's hw_run_apply_profile case --
	// the >3s-stall/terrain-pin hunt profile (see mp_autoconnect's comment
	// above and PROJECT_MEMORY's 2026-08-31 maiden-run entry: the walker
	// terrain-pinned ~5 blocks from spawn for 570 heartbeats with no notion
	// of progress). main.c reads this field DIRECTLY into
	// gstate.autopilot.unstick.armed, never through a "debug.*" config key,
	// so a hand-edited or capture config can never enable it and every other
	// profile's autopilot_walk trajectory (far_walk_6m first among them)
	// stays byte-identical to before this field existed.
	bool autopilot_unstick;
	bool autopilot_aim;
	int autopilot_pitch_deg;
	int autopilot_yaw_deg;
	// One-shot deterministic block-place/activate pulse riding the aim-mode
	// camera lock (see autopilot.h / main.c's "debug.autopilot_place"). Only
	// menus_dwell sets this so far -- the mechanism itself predates hw_run
	// (the Dolphin capture rig has used it since the piston/redstone
	// showcases), but this is its first hw_run/real-Wii integration.
	bool autopilot_place;
	bool watchdog_inject;
	bool redstone_clock_stress;
	// mob_motion_2m: arm "debug.mob_motion_rig" (server_local.c fixture).
	bool mob_motion_rig;
	// pool_selftest_3m (colcap-1006 R5 item 2): run source/game/
	// pool_selftest.c. Set ONLY by that profile's apply case;
	// pool_placement_rotating is the --cw-pool-placement value (false =
	// production lowest-index-first), honoured only under that profile.
	bool pool_selftest;
	bool pool_placement_rotating;
	// split_regrow_8m (colcap-1006 R5 item 3): run main.c's anchor cycle.
	bool anchor_cycle;
	bool save_migrate;
	int save_migrate_regions;
	// world_copy_convert (ui-build-1026/T16a): arm the copy route
	// (menu_autopilot_arm_copy_route) instead of menus_dwell. wcc_variant is
	// enum menu_ap_copy_variant (0 ok, 1 low_space, 2 write_error,
	// 3 interrupt, 4 recover; --cw-wcc-variant); wcc_root names the per-run
	// worlds folder sd:/apps/CavEX/wcc_runs/<wcc_root> (--cw-wcc-root,
	// [A-Za-z0-9_]{1,40}, REQUIRED by the profile; recover reuses the
	// interrupt run's root). Both flags are refused on every other profile.
	bool world_copy_convert;
	// save_reload_3m (R12): arm the save/reload phase driver + server probe.
	// Set ONLY by that profile's apply case (no --cw-* flag, no config key):
	// the driver creates a world and drives Save and quit.
	bool save_reload;
	int wcc_variant;
	char wcc_root[48];
	// menus_dwell: arm the menu_autopilot script (source/game/menu_autopilot.h)
	// that drives title/select-world/texture-packs/create-world/pause/options
	// (and, opportunistically, the sign editor) with zero human input.
	bool menu_autopilot;
	// The target Game Mode the menu_autopilot script's create-world selector
	// should land on and verify before pressing Create (see hw_run_cworld_
	// mode above). Defaults to HW_RUN_CWORLD_MODE_SURVIVAL for every profile
	// (byte-identical to before this field existed); only HW_RUN_PROFILE_
	// MENUS_DWELL_CREATIVE's hw_run_apply_profile case and the
	// --cw-cworld-mode CLI flag ever set it to CREATIVE. main.c reads this
	// field DIRECTLY into menu_autopilot_arm()'s target parameter, never
	// through a "debug.*" config key -- same rationale as autopilot_unstick
	// above: a hand-edited or capture config can never enable it.
	enum hw_run_cworld_mode cworld_mode;
	// mp_connect_smoke (S5.3): boot directly into the multiplayer connect
	// screen (config-driven mp.server/mp.username, the real user flow incl.
	// the Wii net_bringup gate) with zero input. No autoload rides with it,
	// so a loaded world in telemetry can ONLY mean the remote install
	// succeeded -- the collector's scenario discriminator. Also armed by
	// mp_lan_walk_10m (the >3s-stall hunt profile), which pairs this with
	// autopilot_walk below -- the walk driver is gated on
	// game_state_client_entities_active() (world_loaded), so it naturally
	// waits for this flag's remote install to land before taking a step.
	bool mp_autoconnect;
	// mp_reconnect_soak (S5.3 hunt plumbing, docs/mp_s5_wii_transport_design_
	// 2026-08-26.md 7b's third profile): arms the minimal hw_run-driven
	// connect/dwell/disconnect/reconnect cycle (source/game/mp_reconnect_
	// soak.h) that mirrors screen_pause.c's "Save and quit to title" (SRPC_
	// UNLOAD_WORLD -> server_remote_disconnect, graceful) for teardown and
	// mp_autoconnect's own screen_set(screen_multiplayer_connect) entry for
	// each reconnect -- catches the LWP-thread-bookkeeping and MEM2-margin
	// accumulation risks that only surface ACROSS repeated reconnects, never
	// a single session (design doc 7b.3). Only HW_RUN_PROFILE_MP_RECONNECT_
	// SOAK's hw_run_apply_profile case sets this true, always paired with
	// mp_autoconnect (the first cycle's connect rides the existing boot-time
	// entry, exactly like mp_lan_walk_10m pairs autopilot_walk with it).
	bool mp_reconnect_soak;
	// --cw-mp-endpoint <host:port> (run-local server override, 2026-09-12):
	// which server an UNATTENDED mp_connect_smoke run joins, so a harness can
	// point one staged card at a chosen host without editing the card's
	// config.json "mp.server" (the stage protocol writes only the .dol and
	// cw_run_args.txt -- the same gap --cw-set exists for). Empty (every
	// profile's default, and the only possible value when the flag is absent)
	// = the saved config's mp.server stays in control, byte-identical to
	// before this field existed. hw_run_parse_args accepts it ONLY on
	// HW_RUN_PROFILE_MP_CONNECT_SMOKE -- smoke-only by construction, so it can
	// never quietly redirect mp_lan_walk_10m/mp_reconnect_soak (whose
	// measurements are tied to a known server) or a non-MP profile.
	// Consumed in main.c's hw_run config-injection block as an EPHEMERAL
	// "mp.server" (hw_run_config_set_string -> config_mark_ephemeral, so
	// config_save can never bake it onto the card -- the GB18 "+new"-sentinel
	// leak guard), which keeps the run on the REAL user surface: the
	// mp_connect_smoke screen_set() below still enters screen_multiplayer_
	// connect, whose own mp.server read (and, on Wii, its net_bringup gate)
	// is the very flow this profile exists to exercise.
	char mp_endpoint[64];
	// --cw-mp-username <name> (2026-09-17, lead ruling 1 of the mp_chat_login_3m
	// contract). The SAME shape and the SAME gate as --cw-mp-endpoint above,
	// for the other half of the connect identity: the collector binds a
	// mc173-diag `[login-gate]` receipt to a specific run by PLAYER NAME, and
	// the card's saved "mp.username" cannot be set per run (the stage protocol
	// writes only the .dol and cw_run_args.txt; --cw-set is integer-only). The
	// rig passes "cw" + the first 6 hex of the run id. Accepted ONLY on an MP
	// profile, exactly like the endpoint; the value must match
	// [A-Za-z0-9_]{1,16} (Beta's own username limit) or the parse fails loudly
	// rather than silently joining as somebody else. Empty (every profile's
	// default, and the only possible value when the flag is absent) leaves the
	// saved config's mp.username in control, byte-identical to before this
	// field existed. Consumed in main.c's hw_run config-injection block as an
	// EPHEMERAL "mp.username" (hw_run_config_set_string -> config_mark_
	// ephemeral), so config_save can never bake a run nonce onto the card.
	char mp_username[32];
	// -1 leaves the normal config/default in control. Most profiles set this
	// to -1 (own the default) except far_idle_2m/far_walk_6m, which pin it
	// to 6 -- see hw_run_apply_profile. --cw-far-view-distance overrides
	// whatever the profile set, clamped to [HW_RUN_FAR_VIEW_DISTANCE_MIN,
	// HW_RUN_FAR_VIEW_DISTANCE_MAX] (hw_run_parse_args), for the Stage-2 LOD
	// reach gate (docs/stage2_lod_plan_2026-09-03.md 3: measure reach 6, 8,
	// and the cap -- 16 as of 2026-09-03, matching Beta 1.7.3's "Far" render
	// distance; see HW_RUN_FAR_VIEW_DISTANCE_MAX above).
	int far_view_distance;
	// --cw-far-storage override (S2 measurement). HW_RUN_FAR_STORAGE_UNSET
	// (default) leaves config.json's "lod.far_storage" in control -- see the
	// enum's declaration comment above for why main.c has nothing to
	// consume this field with yet (the target lineage is unmerged).
	enum hw_run_far_storage far_storage;
	// --cw-far-send-near-owed override (T2 far-send share, 2026-09-04,
	// lane/far-send-share). -1 (default, every profile) leaves config.json's
	// "lod.far_send_near_owed" in control, whose own default is 8 since
	// 2026-09-04 (lane/far-share-default-8; 0, the strict pre-2026-09-04 far
	// gate, is still a fully supported opt-out). A value >= 0 forces that many
	// still-unsent near chunks to be tolerated before the far ring may take the
	// near loop's LEFTOVER per-tick send slots, so a hardware run can A/B the
	// share without editing the staged card's config.json -- same override-
	// only-when-armed pattern as far_view_distance/far_storage above. Clamped
	// to [0, HW_RUN_FAR_SEND_NEAR_OWED_MAX] in hw_run_parse_args.
	int far_send_near_owed;
	// Hands-free hardware batch (2026-08-18): once an ARMED run (hw_run.enabled)
	// has sent its "ok" profile_done, self-exit back to the loader after this
	// many seconds of grace (lets the final telemetry line and any trailing
	// heartbeat actually flush before the socket/process tear down). Default ON
	// for every armed profile except watchdog_inject (which owns a deliberate
	// non-returning spin -- see hw_run_auto_exit_ready). Tunable/disable-able
	// via --cw-auto-exit so a human-observed armed run can still linger on
	// purpose; NEVER consulted outside the hw_run.enabled branch, so normal
	// (unarmed) play can never auto-exit -- see hw_run_auto_exit_ready.
	bool auto_exit;
	float auto_exit_grace_seconds;
	// --cw-seed (harness fixed-seed lane, 2026-09-04): a fixed worldgen seed
	// for the "+new" autoload sentinel, so two harness runs of the SAME
	// profile see identical terrain instead of a fresh random seed each boot
	// (screen_select_world.c's sworld_finish_create mixes the clock with a
	// session salt on every create -- the noise source this flag exists to
	// silence for A/B throughput comparisons). main.c reads this field
	// DIRECTLY into screen_select_world.c's harness seed override
	// (screen_sworld_set_seed_override, screen.h), never through a
	// "debug.*" config key -- same rationale as autopilot_unstick/
	// cworld_mode above: config_set_int/config_read_int (config.h) are
	// 32-bit only, and a hand-edited or capture config can never
	// accidentally arm this. Measurement profiles and a few scripted profiles
	// default to seed 12345; --cw-seed overrides that pin. An unpinned profile
	// with seed_set == false retains the random per-create mix.
	bool seed_set;
	// True when seed_set/seed came from a PROFILE pin rather than --cw-seed.
	// hw_run_apply_profile's preamble clears a profile-pinned seed (so a
	// second --cw-profile does not inherit the previous profile's terrain)
	// but never a user-supplied one, in either argument order. See the
	// preamble and the --cw-seed parse for the two halves of that rule.
	bool seed_from_profile;
	int64_t seed;
	// --cw-set key=value (repeatable, up to HW_RUN_CW_SET_MAX): run-scoped
	// INTEGER config override for staged hardware runs (2026-09-08,
	// lane/carve-observer-boot). Motivating case: the two-anchor scene
	// loses the MEM2 carve reserve to display-list grow slack, and
	// "memory.displaylist_compact" (main.c, source/platform/displaylist_
	// stats.h) is the cheapest fix candidate -- but a staged console run
	// cannot edit the card's config.json (the stage protocol writes only
	// the .dol and cw_run_args.txt), so a paired A/B (compact 0 vs 1, same
	// build/seed) needs a run-arg instead. `key` is a dotted config path
	// (e.g. "memory.displaylist_compact"); `value` is a plain base-10
	// integer (config_set_int/config_read_int are 32-bit only, same as
	// every other config key this codebase reads). Applied in main.c via
	// the existing hw_run_config_set_int wrapper (config_set_int +
	// config_mark_ephemeral) -- same "run-scoped only" guarantee as every
	// other hw_run config injection above: config_save() never persists
	// it back to config.json (the GB18 "+new"-sentinel-leak class of bug,
	// 2026-08-17). Does NOT set cfg->enabled by itself -- same pattern as
	// --cw-settle/--cw-carve-census/--cw-column-census above -- pair with
	// --cw-tel-host (or a --cw-profile) to arm telemetry for the run.
	// Malformed entries (no '=', empty/oversized/invalid-char key, a value
	// that isn't a clean base-10 integer) and entries past the
	// HW_RUN_CW_SET_MAX cap are IGNORED (not fatal to the whole parse --
	// unlike most other --cw-* flags): each is counted in cw_set_malformed
	// and logged to stderr, so a bad harness-generated line degrades to
	// "fewer overrides applied", never a bricked boot.
	struct hw_run_cw_set_kv {
		char key[64];
		int value;
	} cw_set[HW_RUN_CW_SET_MAX];
	int cw_set_n;			// successfully parsed/applied overrides (0..HW_RUN_CW_SET_MAX)
	int cw_set_malformed;	// malformed or over-cap --cw-set occurrences, ignored+counted
};

/* Post-world_loaded settle: tools/wii_telemetry_server.py PROFILES.
 * Split-anchor uses its existing readiness gate instead. */
#define HW_RUN_TPS_SETTLE_MS 5000u
/* Leave the collector/sender two seconds to receive the final aggregate seal. */
#define HW_RUN_TPS_SEAL_MARGIN_SECONDS 2u
bool hw_run_tps_duration_end(const struct hw_run_config* cfg, float elapsed);
struct hw_run_tps_window {
	uint64_t loaded_ms;
	bool loaded, measuring;
};
bool hw_run_tps_measuring(struct hw_run_tps_window* window,
	const struct hw_run_config* cfg, bool world_loaded, bool anchor_measuring,
	bool done, uint64_t now_ms);
/* Does a CLOCK end the TPS window now? (Completion and telemetry quiescence
 * close it separately, in main.c.) The answer has to be read on the same clock
 * the window OPENED on, or the measurement loses whatever the other clock had
 * already spent: the readiness-epoch profiles open at installed readiness, so
 * ending them at `run clock >= duration - seal margin` subtracted the entire
 * loading/readiness time from the measured window (readiness at +25 s cut 25 s
 * off a 358 s window). `run_elapsed` is the run clock in seconds and
 * `anchor_ms` the readiness clock in integer milliseconds -- the same value
 * the heartbeat publishes as "anchor_ms" and the completion decision uses, so
 * all three answers come from one number. Pure/deterministic. */
bool hw_run_tps_window_end(const struct hw_run_config* cfg, float run_elapsed,
	bool anchor_measuring, uint32_t anchor_ms);

void hw_run_config_init(struct hw_run_config* cfg);
/* Performance profiles must not add synchronous per-second SD traffic to
 * the gameplay they measure. Boot/shutdown/crash records are independent. */
bool hw_run_periodic_sd_trace(const struct hw_run_config* cfg);
const char* hw_run_profile_name(enum hw_run_profile profile);
bool hw_run_profile_from_name(const char* name, enum hw_run_profile* out);
/* Profiles whose mechanics need Peaceful (no Nether mob hits/pushes on the
 * host). main.c forces difficulty 0 for these, so a runner no longer has to
 * remember -Set game.difficulty=0. split_dims_6m only. */
bool hw_run_profile_forces_peaceful(enum hw_run_profile profile);
/* host_vehicle_smoke_8m kind names: "boat" / "pig" / "cart" (NONE -> ""). */
const char* hw_run_vehicle_kind_name(enum hw_run_vehicle_kind kind);
void hw_run_apply_profile(struct hw_run_config* cfg, enum hw_run_profile profile);

/* The ONE run-scoped switch over the forced F3 overlay (brief 25 item 4).
 * EVERY armed profile forces it -- hw_run_apply_profile sets
 * force_overlay = true, and only mp_chat_chord_1m turns it back off, because
 * "debug.overlay" = "force" suppresses the live F3 toggle that profile exists
 * to observe -- so before this no hardware run could produce a clean console
 * HUD photograph.
 *
 * `hw_overlay_key` is the value of the config key "debug.hw_overlay", read
 * where hw_run is applied (main.c, right after the --cw-set loop) with a
 * DEFAULT OF 1, so an absent key and an explicit 1 are today's behaviour
 * exactly. 0 declines the force for this run and the overlay then follows
 * "debug.overlay" like normal play. A pure READ of cfg: it writes nothing, so
 * no profile's fields -- and therefore no profile's verdict rules -- can
 * move, and the chord profile's own override still stands. */
bool hw_run_force_overlay(const struct hw_run_config* cfg, int hw_overlay_key);
bool hw_run_parse_args(struct hw_run_config* cfg, int argc, char** argv,
					   char* err, size_t err_size);
bool hw_run_parse_file(struct hw_run_config* cfg, const char* path, char* err,
					   size_t err_size);

// Pure gating check for the hands-free auto-exit (source/main.c calls this
// once per frame after an armed run's profile_done). Returns true exactly
// when: cfg is an ARMED run (cfg->enabled) AND auto_exit is on AND the
// profile isn't watchdog_inject (that profile's "injecting_watchdog" event
// deliberately spins forever afterward -- a SEPARATE main.c code path this
// function never reaches, but excluded here too so this gate can never fire
// early for it if a future caller checks it before the spin starts) AND
// profile_done_sent is true AND seconds_since_profile_done has reached the
// configured grace window. Pure/deterministic -- no time_get(), no I/O -- so
// it is fully unit-testable off Wii.
bool hw_run_auto_exit_ready(const struct hw_run_config* cfg,
							bool profile_done_sent,
							float seconds_since_profile_done);

// Has an armed run reached its configured duration? Decided on the INTEGER
// MILLISECOND the run reports, never on a float-seconds sample, because the
// collector's own completion gate is an integer one: for the readiness-epoch
// split profiles it requires the final heartbeat's "anchor_ms" to have reached
// duration*1000 (tools/wii_telemetry_server.py: "need six measured minutes
// after installed readiness"). main.c used to end the run on a float seconds
// value sampled from a DIFFERENT timestamp than the one the heartbeat
// serializes, so a run could send "profile_done ok" with a final heartbeat
// still one millisecond short -- real-Wii run 0917_092103 reported anchor_ms
// 359999 against a 360000 gate and FAILED a complete, healthy eight minutes.
// Feed this the same integer the final heartbeat will carry and that class of
// failure cannot come back.
//
// duration_seconds <= 0 means "no duration timer" (never reached), matching
// the `duration_seconds > 0` guard this replaces. The comparison is 64-bit on
// purpose: --cw-duration accepts any positive int, so duration_seconds*1000
// would overflow a 32-bit int past ~2.1e6 s, and elapsed_ms is a uint32
// millisecond counter (~49.7 days of range) -- a duration beyond what that
// counter can express is simply never reached instead of wrapping into a
// false completion. Pure/deterministic -- no time_get(), no I/O.
bool hw_run_duration_reached_ms(uint32_t elapsed_ms, int duration_seconds);

#endif
