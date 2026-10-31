/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* split_recovery_6m -- the scripted split-screen recovery phase driver.
 *
 * CONTRACT: docs/split_recovery_profile.md. That file, not this header, is the
 * authority on the phases, the marker key names/types/order, the timeouts and
 * the failure diagnostics; tools/wii_telemetry_server.py is written against it.
 *
 * WHAT THIS IS. One profile (HW_RUN_PROFILE_SPLIT_RECOVERY_6M) drives, with no
 * human at the console, the three split-screen transitions docs/
 * local_splitscreen.md describes but that no unattended profile ever
 * exercised: a guest's controller LOSS and its neutral-gated recovery, a
 * guest's deliberate LEAVE and rejoin, and the host's SAVE AND QUIT -> title ->
 * in-process RELOAD with world and inventory state verified across the
 * teardown. Every assertion is made from OBSERVED production state; a marker
 * never says "the command was issued" (Astra ruling 2026-09-17 item 5).
 *
 * WHY IT IS A DRIVER AND NOT MAIN.C CODE. The transitions are driven through
 * the REAL production handlers -- local_menu_host_recovery_frame (which is
 * what calls controller_recovery_step/commit), local_pause_update/
 * local_pause_frame, client_session_request_local_leave -- by feeding those
 * handlers a synthetic owned input frame at main.c's existing call sites. This
 * file owns the frames, the phase machine, the baselines and the markers;
 * main.c keeps thin call sites that substitute the frame and apply the two
 * gates (auto-rejoin suppression, guest suspension).
 *
 * INERTNESS. Nothing here runs unless split_recovery_arm() was handed an ARMED
 * hw_run config whose profile is split_recovery_6m. Unarmed -- which is normal
 * play and every other profile -- split_recovery_frame() writes a zeroed
 * actions struct and returns, split_recovery_armed() is false, and
 * split_recovery_rig_create_allowed() refuses, so the world-deleting create
 * path in screen_select_world.c cannot be reached at all.
 *
 * TEARDOWN SURVIVAL. The run identity, the marker sequence, the clock and all
 * baselines live in this file's own file-static storage, which no world
 * teardown frees -- that is exactly what P4 needs (Astra ruling item 1).
 */

#ifndef GAME_SPLIT_RECOVERY_SCRIPT_H
#define GAME_SPLIT_RECOVERY_SCRIPT_H

#include <stdbool.h>
#include <stdint.h>

#include "../platform/input_owner.h" /* struct input_player_frame */

struct client_session;
struct split_layout;
struct server_rpc;
struct hw_run_config;

/* The ONE disposable world this profile may create, delete and reload. Defined
 * here and nowhere else: screen_select_world.c's harness create refuses every
 * other directory name, so no caller can ever point that delete somewhere the
 * player cares about. */
#define SPLIT_RECOVERY_RIG_WORLD "cw_recovery_rig"
#define SPLIT_RECOVERY_RIG_DISPLAY "Recovery Rig"
/* The rig world's fixed seed lives in hw_run_apply_profile (cfg->seed), not
 * here: --cw-seed must still be able to override it, and main.c hands
 * hw_run.seed_set/seed straight to the create. One source of truth. */

#define SPLIT_RECOVERY_BLOCKS 3
/* Digs per actor at P0: each actor digs its OWN column's three superflat
 * layers (the only deterministic pickup, see sr_guest_dig_cell). The guest then
 * places two blocks and the host one, so the guest keeps 1 and the host 2: both
 * baselines are NON-EMPTY (a populated round trip, not two empty inventories)
 * and DIFFERENT (an ownership swap shows in the hashes). */
#define SPLIT_RECOVERY_GUEST_DIGS 3
#define SPLIT_RECOVERY_HOST_DIGS 3
/* Chebyshev blocks the guest must put between itself and the host before
 * either digs. The pickup box is the body box expanded 1.0 in x/z
 * (entity_item.c), so anything past ~2 removes the race; 6 is generous and
 * still inside a few seconds of walking. */
#define SPLIT_RECOVERY_SEPARATION 6.0F
/* Per-phase and total bounds (docs/split_recovery_profile.md section 8). The
 * total is inside the profile's 360 s duration on purpose, so the driver's own
 * profile_done always precedes the harness window closing. */
/* P0 grew from 90 s to 120 s when it took on the two actors' dig+pickup
 * sequences (7 digs, each with its own bounded pickup wait, plus the
 * separation walk and the three placements). The per-phase budgets now sum to
 * 370 s against a 355 s total, so the TOTAL is the binding cap: a pathological
 * run reports "run:total timeout" rather than a phase timeout. Every run still
 * reports. */
#define SPLIT_RECOVERY_P0_MS 120000u
#define SPLIT_RECOVERY_P1_MS 40000u
#define SPLIT_RECOVERY_P2_MS 60000u
#define SPLIT_RECOVERY_P3_MS 60000u
#define SPLIT_RECOVERY_P4_MS 90000u
#define SPLIT_RECOVERY_TOTAL_MS 355000u

enum split_recovery_phase {
	SPLIT_RECOVERY_IDLE = 0,
	SPLIT_RECOVERY_P0,
	SPLIT_RECOVERY_P1,
	SPLIT_RECOVERY_P2,
	SPLIT_RECOVERY_P3,
	SPLIT_RECOVERY_P4,
	SPLIT_RECOVERY_OK,
	SPLIT_RECOVERY_FAILED,
};

/* The four things the driver cannot do itself, supplied by main.c. Each one is
 * a seam a test replaces with a recorder, which is what makes the whole phase
 * machine drivable off-console. */
struct split_recovery_hooks {
	/* svin_rpc_send_from({slot 1, guest generation}, call) -- the existing
	 * guest action mailbox, never a new transport. */
	bool (*rpc_guest)(void* user, struct server_rpc* call);
	/* svin_rpc_send_from({slot 0, generation 0}, call) -- the host owner
	 * source main.c already builds for player 0. The host needs it to dig its
	 * own baseline blocks; there is no give/creative shortcut. */
	bool (*rpc_host)(void* user, struct server_rpc* call);
	/* hw_telemetry_emit_observer(line). May refuse (ring full / quiesced);
	 * the driver then retries the SAME line with the SAME seq next frame. */
	bool (*emit)(void* user, const char* line);
	/* hw_telemetry_emit_profile_done(status). Called exactly once per run. */
	void (*profile_done)(void* user, const char* status);
	/* screen_sworld_create_exact(SPLIT_RECOVERY_RIG_DISPLAY, directory, ...) */
	bool (*create_world)(void* user, const char* directory);
	/* ephemeral config "autoload" = directory, then screen_set(select_world) */
	bool (*arm_autoload)(void* user, const char* directory);
	/* NULL when P4 will be able to arm the named-autoload hook, or the config
	 * value that already consumed it at boot. screen_select_world.c latches
	 * `autoload_done` for the process, so a card whose config carries an
	 * "autoload" key spends the latch before the driver ever runs -- P4 would
	 * then arm nothing and time out in silence. Checked at P0 BEFORE any world
	 * is deleted. */
	const char* (*autoload_conflict)(void* user);
	/* world_get_block on the CLIENT world -- the same client-visible state the
	 * renderer reads, never a server_local read from the main thread. Returns
	 * false when the cell is not resident yet (the caller waits). */
	bool (*block_at)(void* user, int x, int y, int z, uint32_t* id,
					 uint32_t* meta);
	void* user;
};

/* Everything the driver observes. All of it is client-visible state or an
 * existing safe snapshot -- never an unsynchronised read of server_local from
 * the main thread (Astra ruling item 5). */
struct split_recovery_ctx {
	struct client_session* host;  /* gstate.players[0] */
	struct client_session* guest; /* gstate.players[1] */
	const struct split_layout* layout;
	unsigned view_count;
	bool world_loaded;
	bool world_switch_pending;
	bool host_body;	 /* players[0].local_player != NULL */
	bool at_title;	 /* players[0].current_screen == &screen_title */
	/* The server's own simulation tick counter (g_srv_sim_ticks), read with an
	 * atomic relaxed load -- the same snapshot main.c already publishes as
	 * `server_time`. NOT gstate.world_time: that is a BASE stamp refreshed
	 * only when the server happens to send CRPC_TIME_SET (world load, sleep,
	 * the debug rigs), so it does not move during ordinary play and cannot
	 * witness that the world kept ticking. Measured 2026-09-17: P1 sat on it
	 * for its whole 40 s budget in the real PC app. */
	uint32_t sim_ticks;
	uint32_t epoch;	 /* clin_input_epoch() */
	uint32_t now_ms; /* any monotonic ms source; the driver rebases it itself */
};

/* What main.c must apply this frame. Zeroed when the driver is unarmed, which
 * is what keeps every other profile and normal play byte-identical. */
struct split_recovery_actions {
	/* Do NOT let the existing `claimed |= world_ready` auto-join fire: the
	 * leave phase owns when the guest may come back (Astra ruling item 3). */
	bool suppress_autojoin;
	/* Substitute these for input_player_frame(0/1) at main.c's owner-menu
	 * call sites, and force the guest's menu/recovery call to run. */
	/* P0 only: the scripted guest has no controller, and main.c only marks a
	 * body "driven" (and so only keeps its capture_input, and so only runs its
	 * physics) while something is driving it. Without this the body could not
	 * FALL into the hole it just dug, so it stayed a block above its own drop
	 * -- outside the pickup box, which is the body box expanded 1.0 in x/z and
	 * ZERO in y -- and P0 failed "pickup never observed" with the block
	 * correctly removed. Measured in the real PC app, 2026-09-17: dig applied
	 * (cell 0,3,7 grass -> air), guest y stuck at 5.62, count 0.
	 * Cleared from P1 on, where the driver needs main.c recovery consumer to
	 * see the guest frame instead. */
	bool drive_guest_body;
	/* P0 separation step only: ALSO hold FORWARD/JUMP, so the guest walks out
	 * of the host pickup box before either of them digs. Nothing walks once
	 * the baselines are taken. */
	bool drive_guest_walk;
	/* An unarmed driver must not release another profile's host walk. */
	bool host_walk_valid;
	bool drive_host_walk;
	bool guest_frame_valid;
	bool host_frame_valid;
	struct input_player_frame guest_frame;
	struct input_player_frame host_frame;
};

void split_recovery_arm(const struct hw_run_config* cfg,
						const struct split_recovery_hooks* hooks);
bool split_recovery_armed(void);
void split_recovery_frame(const struct split_recovery_ctx* ctx,
						  struct split_recovery_actions* out);

/* The gate screen_sworld_create_exact() consults before it deletes anything:
 * true only while the driver is armed AND `directory` is the fixed rig world
 * above. No caller-chosen delete target, ever (lead ruling F1 #1). */
bool split_recovery_rig_create_allowed(const char* directory);

/* Observation for tests and for main.c's suspension gate. */
enum split_recovery_phase split_recovery_phase_now(void);
uint32_t split_recovery_seq_now(void);
unsigned split_recovery_step_now(void); /* the sub-step, for test diagnostics */
/* Full teardown of the file-static state. Production never calls this (a run
 * is one process); tests call it between cases. */
void split_recovery_reset(void);

#endif
