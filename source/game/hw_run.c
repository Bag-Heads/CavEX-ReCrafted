/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

#include "hw_run.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool hw_run_periodic_sd_trace(const struct hw_run_config* cfg) {
	if(!cfg || !cfg->enabled)
		return false;
	switch(cfg->profile) {
		case HW_RUN_PROFILE_FAR_IDLE_2M:
		case HW_RUN_PROFILE_FAR_WALK_6M:
		case HW_RUN_PROFILE_KEEPUP_WALK_6M:
		case HW_RUN_PROFILE_SPLIT_ANCHOR_6M:
		case HW_RUN_PROFILE_SPLIT_REGROW_8M:
		case HW_RUN_PROFILE_SPLIT_PLAY_2M:
		/* Maintainer 2026-10-08 trial: only these two additional exclusions. */
		case HW_RUN_PROFILE_SPLIT_PLAY_8M:
		case HW_RUN_PROFILE_SPLIT_ACTIONS_6M:
		case HW_RUN_PROFILE_SPLIT_RECOVERY_6M:
		case HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M:
		case HW_RUN_PROFILE_SPLIT_DIMS_6M:
		case HW_RUN_PROFILE_HOST_VEHICLE_SMOKE_8M:
		/* Same strict tel_drop==0 evidence profile as the vehicle smoke: the
		 * sender must not stall on SD trace writes during the boot burst. */
		case HW_RUN_PROFILE_SPLIT_OFFER_PISTON_2M:
		case HW_RUN_PROFILE_PERF_STRESS_5M:
		case HW_RUN_PROFILE_MP_LAN_WALK_10M:
			return false;
		default:
			return true;
	}
}

static void copy_text(char* dst, size_t dst_size, const char* src) {
	if(!dst || dst_size == 0)
		return;
	if(!src)
		src = "";
	snprintf(dst, dst_size, "%s", src);
}

void hw_run_config_init(struct hw_run_config* cfg) {
	if(!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->profile = HW_RUN_PROFILE_FAR_IDLE_2M;
	copy_text(cfg->profile_name, sizeof(cfg->profile_name),
			  hw_run_profile_name(cfg->profile));
	copy_text(cfg->run_id, sizeof(cfg->run_id), "manual");
	cfg->telemetry_port = 39017;
	cfg->far_view_distance = -1;
	cfg->far_storage = HW_RUN_FAR_STORAGE_UNSET;
	cfg->far_send_near_owed = -1; // config "lod.far_send_near_owed" stays in control
	hw_run_apply_profile(cfg, cfg->profile);
	cfg->enabled = false;
}

const char* hw_run_profile_name(enum hw_run_profile profile) {
	switch(profile) {
		case HW_RUN_PROFILE_HEARTBEAT_SELFTEST: return "heartbeat_selftest";
		case HW_RUN_PROFILE_FAR_IDLE_2M: return "far_idle_2m";
		case HW_RUN_PROFILE_FAR_WALK_6M: return "far_walk_6m";
		case HW_RUN_PROFILE_PERF_STRESS_5M: return "perf_stress_5m";
		case HW_RUN_PROFILE_WATCHDOG_INJECT: return "watchdog_inject";
		case HW_RUN_PROFILE_REDSTONE_CLOCK_2M: return "redstone_clock_2m";
		case HW_RUN_PROFILE_MENUS_DWELL: return "menus_dwell";
		case HW_RUN_PROFILE_SAVE_MIGRATE: return "save_migrate";
		case HW_RUN_PROFILE_MP_CONNECT_SMOKE: return "mp_connect_smoke";
		case HW_RUN_PROFILE_MP_LAN_WALK_10M: return "mp_lan_walk_10m";
		case HW_RUN_PROFILE_MP_RECONNECT_SOAK: return "mp_reconnect_soak";
		case HW_RUN_PROFILE_MENUS_DWELL_CREATIVE: return "menus_dwell_creative";
		case HW_RUN_PROFILE_KEEPUP_WALK_6M: return "keepup_walk_6m";
		case HW_RUN_PROFILE_SPLIT_ANCHOR_6M: return "split_anchor_6m";
		case HW_RUN_PROFILE_SPLIT_PLAY_2M: return "split_play_2m";
		case HW_RUN_PROFILE_SPLIT_PLAY_8M: return "split_play_8m";
		case HW_RUN_PROFILE_SPLIT_ACTIONS_6M: return "split_actions_6m";
		case HW_RUN_PROFILE_SPLIT_RECOVERY_6M: return "split_recovery_6m";
		case HW_RUN_PROFILE_MP_CHAT_LOGIN_3M: return "mp_chat_login_3m";
		case HW_RUN_PROFILE_MP_CHAT_CHORD_1M: return "mp_chat_chord_1m";
		case HW_RUN_PROFILE_MP_PLAY_10M: return "mp_play_10m";
		case HW_RUN_PROFILE_MP_PAUSE_MENU_3M: return "mp_pause_menu_3m";
		case HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M: return "split_portal_quit_4m";
		case HW_RUN_PROFILE_SPLIT_DIMS_6M: return "split_dims_6m";
		case HW_RUN_PROFILE_MOB_MOTION_2M: return "mob_motion_2m";
		case HW_RUN_PROFILE_HOST_VEHICLE_SMOKE_8M: return "host_vehicle_smoke_8m";
		case HW_RUN_PROFILE_POOL_SELFTEST_3M: return "pool_selftest_3m";
		case HW_RUN_PROFILE_SPLIT_REGROW_8M: return "split_regrow_8m";
		/* 120 s driver deadline, with 20 s completion/drain margin.
		 * Peaceful, split payloads and the disposable world are armed by main. */
		case HW_RUN_PROFILE_SPLIT_OFFER_PISTON_2M: return "split_offer_piston_2m";
		case HW_RUN_PROFILE_WORLD_COPY_CONVERT: return "world_copy_convert";
		case HW_RUN_PROFILE_SAVE_RELOAD_3M: return "save_reload_3m";
		default: return "far_idle_2m";
	}
}

const char* hw_run_vehicle_kind_name(enum hw_run_vehicle_kind kind) {
	switch(kind) {
		case HW_RUN_VEHICLE_BOAT: return "boat";
		case HW_RUN_VEHICLE_PIG: return "pig";
		case HW_RUN_VEHICLE_CART: return "cart";
		default: return "";
	}
}

bool hw_run_profile_forces_peaceful(enum hw_run_profile profile) {
	return profile == HW_RUN_PROFILE_SPLIT_DIMS_6M;
}

bool hw_run_profile_from_name(const char* name, enum hw_run_profile* out) {
	if(!name || !out)
		return false;
	if(strcmp(name, "heartbeat_selftest") == 0) {
		*out = HW_RUN_PROFILE_HEARTBEAT_SELFTEST;
		return true;
	}
	if(strcmp(name, "far_idle_2m") == 0 || strcmp(name, "far_idle") == 0) {
		*out = HW_RUN_PROFILE_FAR_IDLE_2M;
		return true;
	}
	if(strcmp(name, "far_walk_6m") == 0 || strcmp(name, "far_walk") == 0) {
		*out = HW_RUN_PROFILE_FAR_WALK_6M;
		return true;
	}
	if(strcmp(name, "perf_stress_5m") == 0 || strcmp(name, "perf_stress") == 0) {
		*out = HW_RUN_PROFILE_PERF_STRESS_5M;
		return true;
	}
	if(strcmp(name, "watchdog_inject") == 0) {
		*out = HW_RUN_PROFILE_WATCHDOG_INJECT;
		return true;
	}
	if(strcmp(name, "redstone_clock_2m") == 0
	   || strcmp(name, "redstone_clock") == 0) {
		*out = HW_RUN_PROFILE_REDSTONE_CLOCK_2M;
		return true;
	}
	if(strcmp(name, "menus_dwell") == 0) {
		*out = HW_RUN_PROFILE_MENUS_DWELL;
		return true;
	}
	if(strcmp(name, "save_migrate") == 0) {
		*out = HW_RUN_PROFILE_SAVE_MIGRATE;
		return true;
	}
	if(strcmp(name, "mp_connect_smoke") == 0
	   || strcmp(name, "mp_connect") == 0) {
		*out = HW_RUN_PROFILE_MP_CONNECT_SMOKE;
		return true;
	}
	if(strcmp(name, "mp_lan_walk_10m") == 0
	   || strcmp(name, "mp_lan_walk") == 0) {
		*out = HW_RUN_PROFILE_MP_LAN_WALK_10M;
		return true;
	}
	if(strcmp(name, "mp_reconnect_soak") == 0
	   || strcmp(name, "mp_reconnect") == 0) {
		*out = HW_RUN_PROFILE_MP_RECONNECT_SOAK;
		return true;
	}
	if(strcmp(name, "menus_dwell_creative") == 0) {
		*out = HW_RUN_PROFILE_MENUS_DWELL_CREATIVE;
		return true;
	}
	if(strcmp(name, "split_anchor_6m") == 0) {
		*out = HW_RUN_PROFILE_SPLIT_ANCHOR_6M;
		return true;
	}
	if(strcmp(name, "keepup_walk_6m") == 0 || strcmp(name, "keepup_walk") == 0) {
		*out = HW_RUN_PROFILE_KEEPUP_WALK_6M;
		return true;
	}
	if(strcmp(name, "split_play_2m") == 0 || strcmp(name, "split_play") == 0) {
		*out = HW_RUN_PROFILE_SPLIT_PLAY_2M;
		return true;
	}
	if(strcmp(name, "split_play_8m") == 0) {
		*out = HW_RUN_PROFILE_SPLIT_PLAY_8M;
		return true;
	}
	if(strcmp(name, "split_recovery_6m") == 0
	   || strcmp(name, "split_recovery") == 0) {
		*out = HW_RUN_PROFILE_SPLIT_RECOVERY_6M;
		return true;
	}
	if(strcmp(name, "split_actions_6m") == 0 || strcmp(name, "split_actions") == 0) {
		*out = HW_RUN_PROFILE_SPLIT_ACTIONS_6M;
		return true;
	}
	if(strcmp(name, "mp_chat_login_3m") == 0
	   || strcmp(name, "mp_chat_login") == 0) {
		*out = HW_RUN_PROFILE_MP_CHAT_LOGIN_3M;
		return true;
	}
	if(strcmp(name, "mp_chat_chord_1m") == 0
	   || strcmp(name, "mp_chat_chord") == 0) {
		*out = HW_RUN_PROFILE_MP_CHAT_CHORD_1M;
		return true;
	}
	/* mp_play_10m: NO short alias. The two-minute-ago precedent (far_walk /
	 * mp_connect / split_play) exists for names a human types; this one is
	 * only ever written by tools/stage_wii_dol.ps1 and the collector, both of
	 * which spell the canonical name, and "mp_play" would read as an alias
	 * for split_play to anyone at the console. */
	if(strcmp(name, "mp_play_10m") == 0) {
		*out = HW_RUN_PROFILE_MP_PLAY_10M;
		return true;
	}
	if(strcmp(name, "mp_pause_menu_3m") == 0) {		*out = HW_RUN_PROFILE_MP_PAUSE_MENU_3M;
		return true;
	}
	if(strcmp(name, "split_portal_quit_4m") == 0) {
		*out = HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M;
		return true;
	}
	if(strcmp(name, "split_dims_6m") == 0) {
		*out = HW_RUN_PROFILE_SPLIT_DIMS_6M;
		return true;
	}
	if(strcmp(name, "mob_motion_2m") == 0 || strcmp(name, "mob_motion") == 0) {
		*out = HW_RUN_PROFILE_MOB_MOTION_2M;
		return true;
	}
	/* No short alias: written only by the staging tools and the collector,
	 * which spell the contract's canonical name (CONTRACT-v1 envelope
	 * `profile` is exactly this string). */
	if(strcmp(name, "split_offer_piston_2m") == 0) {
		*out = HW_RUN_PROFILE_SPLIT_OFFER_PISTON_2M;
		return true;
	}
	if(strcmp(name, "save_reload_3m") == 0) {
		*out = HW_RUN_PROFILE_SAVE_RELOAD_3M;
		return true;
	}
	if(strcmp(name, "world_copy_convert") == 0) {
		*out = HW_RUN_PROFILE_WORLD_COPY_CONVERT;
		return true;
	}
	if(strcmp(name, "host_vehicle_smoke_8m") == 0) {
		*out = HW_RUN_PROFILE_HOST_VEHICLE_SMOKE_8M;
		return true;
	}
	/* No short alias: staging tools and the collector spell it in full. */
	if(strcmp(name, "pool_selftest_3m") == 0) {
		*out = HW_RUN_PROFILE_POOL_SELFTEST_3M;
		return true;
	}
	if(strcmp(name, "split_regrow_8m") == 0) {
		*out = HW_RUN_PROFILE_SPLIT_REGROW_8M;
		return true;
	}
	return false;
}

void hw_run_apply_profile(struct hw_run_config* cfg, enum hw_run_profile profile) {
	if(!cfg)
		return;
	cfg->profile = profile;
	copy_text(cfg->profile_name, sizeof(cfg->profile_name),
			  hw_run_profile_name(profile));
	cfg->duration_seconds = 120;
	cfg->driver_owns_completion = false;
	cfg->force_overlay = true;
	cfg->autoload_new = true;
	cfg->superflat = false;
	cfg->force_overworld = false;
	cfg->perf_stress = false;
	cfg->autopilot_walk = false;
	cfg->autopilot_keepup = false;
	cfg->autopilot_unstick = false;
	cfg->autopilot_aim = false;
	cfg->autopilot_pitch_deg = 130;
	cfg->autopilot_yaw_deg = 0;
	cfg->autopilot_place = false;
	cfg->watchdog_inject = false;
	cfg->redstone_clock_stress = false;
	cfg->mob_motion_rig = false;
	/* Same "naming the profile is the only way in" rule: a later
	 * --cw-profile never inherits the pool self-test. */
	cfg->pool_selftest = false;
	cfg->anchor_cycle = false;
	cfg->save_migrate = false;
	cfg->save_migrate_regions = 8;
	cfg->world_copy_convert = false;
	cfg->save_reload = false;
	cfg->menu_autopilot = false;
	cfg->cworld_mode = HW_RUN_CWORLD_MODE_SURVIVAL;
	cfg->mp_autoconnect = false;
	cfg->mp_reconnect_soak = false;
	/* Experimental local split-screen is OFF for every profile but the one
	 * that names it below (or an explicit --cw-split-guest AFTER the profile
	 * has been applied -- the same order every other override uses). */
	cfg->split_guest = false;
	cfg->split_guest_walk = false;
	cfg->split_guest_actions = false;
	/* Same rule, and the same reason it matters more here: an armed
	 * split_recovery run may delete a world directory and drive Save and quit,
	 * so a LATER --cw-profile must never inherit it. */
	cfg->split_recovery = false;
	/* Same rule for the vehicle smoke driver: it creates a world, drives Save
	 * and quit, a lethal fixture and a transfer -- a later --cw-profile must
	 * never inherit it. The kind/transfer flags are applied after the argv
	 * loop (hw_run_parse_args), so argv order cannot lose them. */
	cfg->vehicle_smoke = false;
	cfg->portal_remaining_ticks = 2;
	/* A profile-PINNED seed does not survive into the next profile either --
	 * split_recovery_6m pins 12345 to keep its own runs comparable, and
	 * far_walk_6m inheriting that would silently change its terrain. A seed
	 * the user asked for with --cw-seed is never cleared here, whichever side
	 * of the --cw-profile it was written on. */
	if(cfg->seed_from_profile) {
		cfg->seed_from_profile = false;
		cfg->seed_set = false;
		cfg->seed = 0;
	}
	cfg->split_guest_yaw_deg = 180;
	cfg->far_view_distance = 6;
	// Hands-free hardware batch: ON by default for every armed profile. The
	// watchdog_inject profile never reaches the "ok" profile_done that gates
	// this (it spins forever after its own "injecting_watchdog" event instead
	// -- see main.c), so leaving auto_exit true here does not change its
	// behavior; hw_run_auto_exit_ready() excludes it defensively anyway.
	cfg->auto_exit = true;
	cfg->auto_exit_grace_seconds = 5.0F;

	switch(profile) {
		case HW_RUN_PROFILE_HEARTBEAT_SELFTEST:
			cfg->duration_seconds = 60;
			cfg->superflat = true;
			cfg->force_overworld = true;
			cfg->far_view_distance = -1;
			break;
		case HW_RUN_PROFILE_FAR_IDLE_2M:
			cfg->duration_seconds = 120;
			/* perf-seed-pin (docs/intent/perf-seed-pin.md, 2026-09-26): every
			 * measurement profile pins world seed 12345 by default so
			 * cross-build/cross-run comparisons sit on the same terrain. The
			 * `!cfg->seed_set` guard means an explicit --cw-seed (written
			 * before OR after --cw-profile, per hw_run_parse_args ordering)
			 * still wins -- this is the identical mechanism already used
			 * below for split_recovery_6m/split_portal_quit_4m/split_dims_6m. */
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		// far_walk_6m / keepup_walk_6m / split_anchor_6m (2026-09-04, revised
		// 2026-09-26): SAME duration/reach envelope -- 360s window,
		// far_view_distance pinned to 6 -- so cross-BUILD tris/chunks_meshed
		// comparisons are meaningful; the only intended delta between
		// far_walk_6m and the keepup pair is the walk driver itself
		// (autopilot_walk vs autopilot_keepup -- see hw_run.h's enum comment
		// and autopilot_keepup.h for the full RCA).
		//
		// SEED: this comment used to say the seed was "deliberately NOT
		// hardcoded" here because "the profile must stay usable on any
		// world, and pinning a seed in the profile would silently change
		// every existing caller's terrain." That reasoning no longer holds:
		// the `!cfg->seed_set` guard (see FAR_IDLE_2M above and the
		// SPLIT_RECOVERY_6M/SPLIT_PORTAL_QUIT_4M/SPLIT_DIMS_6M cases below,
		// which already used this exact pattern) means an explicit
		// --cw-seed still overrides -- so the profile stays "usable on any
		// world" and nothing changes silently for a caller that already
		// pins its own seed. What DID change is the reason to pin one: G1's
		// perf_stress_5m baseline drifted between an unseeded time(NULL)
		// world and a fixed one (TICKCOST-BISECT-0926.md), so every
		// measurement profile now defaults to seed 12345 the same way.
		//
		// Chosen by measurement, not taste: nine seeds (1..8 and 12345) were
		// traced on the PC build for 120 s each with the identical profile
		// and the CAVEWII_TRACE=1 state export, and 12345 walks the longest
		// clean route by a wide margin -- 385 blocks of net displacement in
		// 120 s with zero deaths and zero grinding, against 48..246 blocks
		// for the rest (seeds 4 and 5 grind 57 s and 67 s of a 120 s run).
		// Over the full 360 s window seed 12345 covers 549 blocks of net
		// displacement / 899 blocks of path with 38 s of grinding and no
		// deaths. See docs/platform_perf_notes.md for the per-seed table and
		// the residual limitation (a fixed-heading walker eventually meets
		// terrain that no bounded escape can pass).
		case HW_RUN_PROFILE_FAR_WALK_6M:
			cfg->duration_seconds = 360;
			cfg->autopilot_walk = true;
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		case HW_RUN_PROFILE_SPLIT_REGROW_8M:
			/* split_anchor_6m's envelope (360 s on the readiness clock,
			 * keep-up walk, seed 12345) plus the anchor cycle. */
			cfg->anchor_cycle = true;
			/* fall through */
		case HW_RUN_PROFILE_KEEPUP_WALK_6M:
		case HW_RUN_PROFILE_SPLIT_ANCHOR_6M:
			cfg->duration_seconds = 360;
			cfg->autopilot_keepup = true;
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		// split_play_2m (2026-09-07): a REAL second body, not a synthetic
		// anchor. Same deterministic keep-up walk driver as the anchor sweep
		// for player 1, plus an auto-joined guest walking a fixed heading, so
		// both residency anchors move independently for the whole window.
		case HW_RUN_PROFILE_SPLIT_PLAY_2M:
			cfg->duration_seconds = 120;
			cfg->autopilot_keepup = true;
			cfg->split_guest = true;
			cfg->split_guest_walk = true;
			cfg->split_guest_yaw_deg = 180;
			break;
		// split_play_8m (2026-09-10): the same two bodies, six MEASURED
		// minutes on the readiness epoch (main.c switches the profile clock
		// like split_anchor_6m); the collector fails the run when installed
		// readiness is not reached within 120 s.
		case HW_RUN_PROFILE_SPLIT_PLAY_8M:
			cfg->duration_seconds = 360;
			cfg->autopilot_keepup = true;
			cfg->split_guest = true;
			cfg->split_guest_walk = true;
			cfg->split_guest_yaw_deg = 180;
			/* perf-seed-pin: measurement profile, see FAR_IDLE_2M above. */
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		// split_actions_6m (2026-09-14, S1-d): use the existing flat Overworld
		// fixture so the scripted guest has a deterministic real floor for its
		// dig/drop/collect/place sequence. Ordinary-terrain two-player coverage
		// remains in split_play_8m. The guest still walks; the script only
		// releases FORWARD for steps that need it to stand on its own item drop.
		case HW_RUN_PROFILE_SPLIT_ACTIONS_6M:
			cfg->duration_seconds = 360;
			cfg->autopilot_keepup = true;
			cfg->split_guest = true;
			cfg->split_guest_walk = true;
			cfg->split_guest_yaw_deg = 180;
			cfg->split_guest_actions = true;
			cfg->superflat = true;
			cfg->force_overworld = true;
			break;
		// split_recovery_6m (2026-09-17): the same flat Overworld fixture as
		// split_actions_6m -- the phase driver needs a deterministic floor to
		// dig and place in -- with bounded walks BEFORE the stationary baselines: the position
		// baselines it compares across a leave and a world reload are only
		// meaningful if neither body is moving. The fixed seed keeps two runs
		// of the profile comparable (--cw-seed still overrides it, because
		// hw_run_parse_args runs after the profile). duration_seconds stays
		// 360 for the harness duration tables, but main.c excludes this
		// profile from the generic duration-based completion: only the driver
		// sends profile_done, and its own total cap (355 s) fires first.
		case HW_RUN_PROFILE_SPLIT_RECOVERY_6M:
			cfg->duration_seconds = 360;
			cfg->split_guest = true;
			cfg->split_recovery = true;
			/* The driver creates its own rig world from the title and later
			 * spends the process-lifetime autoload latch ONCE, in P4. A live
			 * "+new" sentinel would race the driver's frame-1 create and, if
			 * that create were refused, make a junk world and spend the latch
			 * (delta review 2026-09-17). Same rule as the title-screen
			 * profiles above. */
			cfg->autoload_new = false;
			cfg->superflat = true;
			cfg->force_overworld = true;
			/* Only when the caller has not already asked for one: a
			 * --cw-seed written BEFORE the --cw-profile must still win. */
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		/* split_dims_6m (lane B, splitdims-profile-0926): independent split
		 * dimensions under load. The host walks in both owning world slots;
		 * the guest runs the split guest walk (the split_play_8m driver) at
		 * yaw 0 -- +z, straight into the production portal_demo SHOWCASE
		 * pane lit five blocks ahead of the host (the demo also builds its
		 * UNLIT frame around the host, never ignited, which the guest walks
		 * out of at once; measured on PC: yaw 180 walked -z, away from the
		 * pane) -- and keeps
		 * walking on the other side. Six measured minutes on the readiness
		 * epoch, generic completion (main.c's anchor clock) -- no phase
		 * driver: the recovery/quit script would spend the window on save
		 * and reload. The heartbeat carries each view's dimension
		 * (split_dims_fill) and the collector certifies ONLY a run whose
		 * views really showed two dimensions: until guest portal travel
		 * lands (A2) the run arms, measures and records dims_not_active,
		 * which is a FAIL, never a PASS. --cw-portal-remaining-ticks stays
		 * accepted (the harness passes it) and is inert here.
		 * Host switch FIRST (splitdims-hostswitch-0927): main.c sets
		 * debug.portal_demo=split_dims and split.world_payloads=on; the
		 * guest walks out of the frame around the host, the server lights
		 * it, the host transfers to the Nether and back (A2-T), the vacated
		 * world drains and is released, and only then the guest crosses --
		 * main.c split_dims_host_step, collector SplitDimsEvidence. */
		case HW_RUN_PROFILE_SPLIT_DIMS_6M:
			cfg->duration_seconds = 360;
			cfg->split_guest = true;
			cfg->split_guest_walk = true;
			cfg->split_guest_yaw_deg = 0;
			cfg->superflat = true;
			cfg->force_overworld = true;
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		case HW_RUN_PROFILE_SPLIT_OFFER_PISTON_2M:
			cfg->duration_seconds = 140;
			cfg->driver_owns_completion = true;
			cfg->split_guest = true;
			cfg->autoload_new = false;
			cfg->superflat = true;
			cfg->force_overworld = true;
			cfg->far_view_distance = -1;
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		/* host_vehicle_smoke_8m (vehicle-smoke-1004, CONTRACT-v1): the phase
		 * driver owns completion (its own 480 s global deadline fires inside
		 * this 500 s window, so a stuck run reports its own named failure).
		 * Superflat Overworld, seed 12345, guest auto-joined but NOT walked
		 * (the driver positions it), and no autoload sentinel: the driver
		 * creates the disposable cw_vehicle_rig itself and spends the
		 * process-lifetime named-autoload latch once, at the p2 reload.
		 * Difficulty (Peaceful) and split.world_payloads=on are applied by
		 * main.c from this flag, never from a config key. */
		case HW_RUN_PROFILE_HOST_VEHICLE_SMOKE_8M:
			cfg->duration_seconds = 500;
			cfg->driver_owns_completion = true;
			cfg->split_guest = true;
			cfg->vehicle_smoke = true;
			cfg->autoload_new = false;
			cfg->superflat = true;
			cfg->force_overworld = true;
			cfg->far_view_distance = -1;
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		case HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M:
			cfg->duration_seconds = 240;
			cfg->driver_owns_completion = true;
			cfg->split_guest = true;
			cfg->split_recovery = true;
			cfg->autoload_new = false;
			cfg->superflat = true;
			cfg->force_overworld = true;
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		case HW_RUN_PROFILE_PERF_STRESS_5M:
			cfg->duration_seconds = 300;
			cfg->perf_stress = true;
			/* perf-seed-pin (docs/intent/perf-seed-pin.md): this is the
			 * profile the G1 baseline drift was traced to -- an unseeded
			 * run got a time(NULL) world, so two "healthy" baselines were
			 * actually two different worlds (TICKCOST-BISECT-0926.md). See
			 * FAR_IDLE_2M above for the guard mechanism. */
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		case HW_RUN_PROFILE_WATCHDOG_INJECT:
			cfg->duration_seconds = 30;
			cfg->superflat = true;
			cfg->force_overworld = true;
			cfg->watchdog_inject = true;
			cfg->far_view_distance = -1;
			break;
		case HW_RUN_PROFILE_REDSTONE_CLOCK_2M:
			cfg->duration_seconds = 120;
			cfg->superflat = true;
			cfg->force_overworld = true;
			cfg->autopilot_aim = true;
			cfg->autopilot_pitch_deg = 108;
			cfg->redstone_clock_stress = true;
			cfg->far_view_distance = -1;
			/* perf-seed-pin: measurement profile, see FAR_IDLE_2M above.
			 * superflat's fixed layout means this seed mostly affects any
			 * non-superflat-derived randomness (e.g. mob/weather RNG seeding
			 * off the world seed), but it is pinned for the same
			 * comparability reason as every other profile in this list. */
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		case HW_RUN_PROFILE_POOL_SELFTEST_3M:
			/* Real terrain (seed 12345, Overworld), no far ring, no input:
			 * the scene must be the same in both placement runs, and real
			 * terrain gives every column distinct bytes (a slot read
			 * through the wrong neighbour would show). The self-test owns
			 * completion (main.c excludes it from the duration "ok"):
			 * profile_done follows the scene hash, or a failure; its own
			 * 170 s cap fires inside the 180 s window. */
			cfg->duration_seconds = 180;
			cfg->force_overworld = true;
			cfg->pool_selftest = true;
			cfg->far_view_distance = -1;
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		case HW_RUN_PROFILE_MOB_MOTION_2M:
			/* Superflat, Overworld, no far ring: the fixture builds its own
			 * geometry ~5 s after load (the deferred demo path) and publishes
			 * its verdict at rig tick 400 (~25 s in); the rest of the run is
			 * the stability tail. Seed pinned like every measurement profile. */
			cfg->duration_seconds = 120;
			cfg->superflat = true;
			cfg->force_overworld = true;
			cfg->mob_motion_rig = true;
			cfg->far_view_distance = -1;
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		case HW_RUN_PROFILE_MENUS_DWELL:
		case HW_RUN_PROFILE_MENUS_DWELL_CREATIVE:
			// 240s: comfortably covers the scripted title -> texture-packs ->
			// title -> select-world -> create-world(OSK) -> [world creation +
			// load, budgeted up to 90s on its own] -> sign(soft) -> pause ->
			// options -> pause -> resume sequence with real hardware slack,
			// per menu_autopilot_script.c's own per-step timeouts.
			cfg->duration_seconds = 240;
			// Fast, flat, deterministic terrain: the sign demo (server_local.c
			// debug.menu_dwell_sign_demo) needs a known, obstruction-free
			// floor near spawn, exactly like the other fresh-world profiles.
			cfg->superflat = true;
			cfg->force_overworld = true;
			// The ONLY profile that must NOT auto-skip the menus -- this is
			// the whole point of menus_dwell, so override the default above.
			cfg->autoload_new = false;
			// Aim mode holds a fixed look; the target is the demo sign block
			// server_local.c places at the SAME offset/height convention
			// redstone_clock_2m already frames (z=player_z+5, y=player_y),
			// so this reuses the same distance/angle neighborhood rather
			// than inventing new geometry.
			cfg->autopilot_aim = true;
			cfg->autopilot_pitch_deg = 108;
			cfg->autopilot_yaw_deg = 0;
			// One-shot deterministic "right-click" once the aim lands on the
			// sign -- see hw_run.h's field comment; first hw_run integration
			// of this mechanism (hardware-unconfirmed until a real-Wii PASS).
			cfg->autopilot_place = true;
			cfg->menu_autopilot = true;
			// menus_dwell_creative (2026-09-02): the ONLY delta from
			// menus_dwell -- see hw_run.h's HW_RUN_PROFILE_MENUS_DWELL_
			// CREATIVE comment for why this exists (a deliberate,
			// on-purpose repro of the staged_menus_dwell_20260902_063552
			// scenario instead of relying on a selector bug to produce one
			// by accident). Everything else about this profile (duration,
			// superflat, aim/place, the script itself) is byte-identical.
			if(profile == HW_RUN_PROFILE_MENUS_DWELL_CREATIVE)
				cfg->cworld_mode = HW_RUN_CWORLD_MODE_CREATIVE;
			cfg->far_view_distance = -1;
			break;
		case HW_RUN_PROFILE_MP_CONNECT_SMOKE:
			// S5.3: hands-free "the Wii joins a LAN server" smoke. 120s
			// covers Wii DHCP settle (~25s measured) + connect + world
			// stream with slack. NO autoload -- the connect screen is the
			// boot target (main.c) and the remote install is the ONLY way a
			// world can appear, which is the collector's scenario
			// discriminator. View distance stays at the platform default:
			// the server drives what the client receives.
			cfg->duration_seconds = 120;
			cfg->autoload_new = false;
			cfg->mp_autoconnect = true;
			cfg->far_view_distance = -1;
			break;
		case HW_RUN_PROFILE_MP_LAN_WALK_10M:
			// S5.3-era hunt profile (PROJECT_MEMORY): the >3s main-loop stall the
			// maintainer's 2026-08-30 field session hit under real walking/chunk
			// churn never showed up in mp_connect_smoke's short idle-at-spawn
			// window (16-778ms gaps only). This profile arms BOTH parents at
			// once: mp_autoconnect (mp_connect_smoke's real connect-screen flow,
			// zero autoload -- a loaded world can only be the remote install)
			// PLUS autopilot_walk (far_walk_6m's forced-forward + curving-yaw
			// driver). The walk driver only ticks once
			// game_state_client_entities_active() is true (world_loaded &&
			// !world_switch_pending && !remote_terrain_hold -- game_state.h),
			// so it naturally waits for the remote world install to land before
			// the first step, exactly like the "arm after world_loaded" ask --
			// no extra plumbing needed beyond setting both existing flags.
			// 600s total (not padded like far_idle_2m/redstone_clock_2m/
			// heartbeat_selftest's connect-lag margin): mirrors far_walk_6m and
			// mp_connect_smoke, which both run un-padded at their raw duration.
			// The ~25-30s DHCP/connect/login/chunk-stream settle measured for
			// mp_connect_smoke eats into this budget the same way, leaving
			// ~570s of continuous walking under load -- comfortably past the
			// stressor this profile exists to reproduce.
			cfg->duration_seconds = 600;
			cfg->autoload_new = false;
			cfg->mp_autoconnect = true;
			cfg->autopilot_walk = true;
			// 2026-08-31 maiden-run follow-up: the walker terrain-pinned ~5
			// blocks from the mc173 MP spawn for 570 heartbeats (16 positions
			// inside a 2.7x4.7 pocket) because the forced-forward + curving-yaw
			// walk driver has no notion of progress. ONLY this profile arms
			// the opt-in stuck-detector (autopilot_unstick.h) -- every other
			// consumer of autopilot_walk (far_walk_6m first) must keep its
			// established hardware-evidence trajectory byte-identical.
			cfg->autopilot_unstick = true;
			cfg->far_view_distance = -1;
			break;
		case HW_RUN_PROFILE_MP_CHAT_LOGIN_3M:
			// mp_chat_login_3m (b1-chat-keyboard-0917): mp_connect_smoke's
			// shape -- the real connect-screen flow, NO autoload, so a loaded
			// world can only be the remote install -- with the 180 s window the
			// contract's phase budgets are cut to fit. The phase driver owns
			// completion (main.c excludes this profile from the generic
			// duration-based "ok", exactly as it excludes split_recovery_6m),
			// and its own 180 s total cap is the binding one, so a stuck run
			// still reports. View distance stays at the platform default: the
			// server drives what the client receives.
			cfg->duration_seconds = 180;
			cfg->driver_owns_completion = true;
			cfg->autoload_new = false;
			cfg->mp_autoconnect = true;
			cfg->far_view_distance = -1;
			break;
		case HW_RUN_PROFILE_MP_CHAT_CHORD_1M:
			// mp_chat_chord_1m (b1-chat-opener-0919): mp_chat_login_3m's join
			// shape at 60 s. force_overlay is OFF on purpose -- "debug.overlay"
			// = "force" re-asserts the HUD every frame and SUPPRESSES the live
			// F3/chord toggle (main.c), which is the very thing this profile
			// exists to observe. The driver forces the overlay off itself at
			// P0, so the run starts from a known state whatever the card says.
			cfg->duration_seconds = 60;
			cfg->driver_owns_completion = true;
			cfg->force_overlay = false;
			cfg->autoload_new = false;
			cfg->mp_autoconnect = true;
			cfg->far_view_distance = -1;
			break;
		case HW_RUN_PROFILE_MP_PLAY_10M:
			// mp_play_10m (push-0921/B4c): mp_chat_login_3m's join shape --
			// the real connect-screen flow, NO autoload (so a loaded world
			// can only be the remote install, which is what the readiness
			// marker is measured from), driver-owned completion -- at 1260 s.
			//
			// WHY 1260 (21 min) and not the driver's own cap: the phase
			// machine's total cap is 1 200 000 ms (MPP_TOTAL_CAP_MS,
			// mp_play_core.h) and it must fire INSIDE the profile window, so
			// a stuck run reports its own named failure rather than being cut
			// off by the profile clock with no verdict. 60 s of headroom
			// covers the driver's own drain window (MPP_DONE_DRAIN_MS) and
			// the completion tail main.c runs after it. A typical passing run
			// is ~13 min, so the cap is a backstop a legitimate run never
			// reaches (design section 7, "Known accepted gap").
			//
			// NOTHING here arms autopilot_walk / autopilot_keepup /
			// autopilot_aim / autopilot_place, and that is load-bearing, not
			// an omission: main.c writes "debug.autopilot" from exactly those
			// fields, and a config-armed autopilot is what makes
			// autopilot_force_button script screen_death's Respawn hold. This
			// profile must never respawn (Astra gate 1 Lane B item 5) -- the
			// driver fails the run on a death instead. It drives the walker
			// itself through autopilot.c's runtime seam, one leg at a time.
			//
			// View distance stays at the platform default (-1 = leave
			// config.json alone): the server drives what the client receives.
			cfg->duration_seconds = 1260;
			cfg->driver_owns_completion = true;
			cfg->autoload_new = false;
			cfg->mp_autoconnect = true;
			cfg->far_view_distance = -1;
			// force_overlay OFF, and for this profile that is FUNCTIONAL, not
			// a preference (push-0921/B9). The default above is true, and true
			// means main.c writes "debug.overlay" = "force" into config_user
			// (main.c:1808-1810) OVER the card's own config.json, which was
			// loaded first -- so no rig config can opt out of it. The resulting
			// gstate.force_debug_overlay then REPURPOSES the hotbar scroll:
			// screen_ingame.c:717 discards the hotbar step
			// (`!gstate.force_debug_overlay && step_right`) and main.c:4782-4787
			// spends the very same IB_SCROLL_RIGHT press on the far-ring LOD
			// reach dial.
			//
			// This profile's driver presses MPP_ACT_HOTBAR_NEXT (-> IB_SCROLL_
			// RIGHT) to bring a placeable block into the held slot after an
			// inventory move has emptied it, so with the HUD forced it can
			// never place: five PC runs finished with places 0 of the
			// contract's 8 and the device failing "action floors not met" at
			// p2_end. Nothing in that chain is platform-gated -- it fails the
			// same way on real hardware. MP_CHAT_CHORD_1M above is the
			// precedent (it clears the flag because a forced HUD suppresses
			// the F3/chord toggle it measures); test_hw_run_args_parse pins
			// this value, and every other profile's, as a table column.
			cfg->force_overlay = false;
			break;
		case HW_RUN_PROFILE_MP_PAUSE_MENU_3M:
			cfg->duration_seconds = 180;
			cfg->driver_owns_completion = true;
			cfg->autoload_new = false;
			cfg->mp_autoconnect = true;
			cfg->far_view_distance = -1;
			cfg->force_overlay = false;
			break;
		case HW_RUN_PROFILE_MP_RECONNECT_SOAK:
			// S5.3 hunt plumbing (docs/mp_s5_wii_transport_design_2026-08-26.md
			// 7b.3's "reconnect-cycle soak", the design doc's own "100 connect/
			// disconnect cycles" S4 gate ported to hardware at a smaller count
			// first). Rides the SAME boot-time connect entry mp_connect_smoke/
			// mp_lan_walk_10m use (mp_autoconnect below); source/game/main_
			// c's mp_reconnect_soak_arm() then drives repeated dwell/
			// disconnect/reconnect cycles once world_loaded (source/game/
			// mp_reconnect_soak.h). 480s (8 min): MP_RECONNECT_SOAK_CYCLES_
			// TARGET cycles at a ~15s dwell + up to ~20s teardown + up to
			// ~90s reconnect-wait budget each (first cycle's DHCP settle is
			// the slow one; net_bringup_request() is idempotent/cached after
			// that, so later cycles are far faster in practice) fit inside
			// this window with real margin; a run that has not completed all
			// cycles by the ceiling reads as an honest FAIL (too few cycles),
			// not a padded pass.
			cfg->duration_seconds = 480;
			cfg->autoload_new = false;
			cfg->mp_autoconnect = true;
			cfg->mp_reconnect_soak = true;
			cfg->far_view_distance = -1;
			break;
		case HW_RUN_PROFILE_WORLD_COPY_CONVERT:
			// ui-build-1026/T16a: the fixture is staged by the rig into a
			// per-run worlds root (main.c); the route reaches the title with
			// no autoload. 300 s covers boot, a ~155 KB copy + convert and
			// the dwells with wide margin; the route's own budgets end it.
			cfg->duration_seconds = 300;
			cfg->autoload_new = false;
			cfg->world_copy_convert = true;
			cfg->far_view_distance = -1;
			break;
		case HW_RUN_PROFILE_SAVE_RELOAD_3M:
			// SPEC R12: title -> fresh seeded world (the dialog's create
			// entry, no autoload so the named-autoload latch stays free for
			// the reload) -> fixture -> real pause Save and quit -> title ->
			// named reload of the same folder -> hash verdict. The driver
			// owns completion; its 170 s cap fires inside this 180 s window.
			// Superflat + Overworld keep the fixture cells on a known floor;
			// no far ring, no walk, no extra --cw-set needed.
			cfg->duration_seconds = 180;
			cfg->driver_owns_completion = true;
			cfg->autoload_new = false;
			cfg->superflat = true;
			cfg->force_overworld = true;
			cfg->save_reload = true;
			cfg->far_view_distance = -1;
			if(!cfg->seed_set) {
				cfg->seed_set = true;
				cfg->seed_from_profile = true;
				cfg->seed = 12345;
			}
			break;
		case HW_RUN_PROFILE_SAVE_MIGRATE:
			// Runtime-built legacy fixture: eight changed region files, four
			// physical wall orientations per region. The 300s hard window covers
			// fixture generation, the measured production reopen, and post-load
			// validation; exceeding it is a profile failure, not a reason to relax it.
			cfg->duration_seconds = 300;
			cfg->superflat = true;
			cfg->force_overworld = true;
			cfg->save_migrate = true;
			cfg->far_view_distance = -1;
			break;
	}
}

/* See hw_run.h for the contract. Deliberately NOT a config read: this module
 * is linked ALONE by test_hw_run_args_parse (its <stdio.h>/<string.h>-only
 * include set is a property of the build graph, not an accident), so the KEY
 * is read by the caller that already owns the config and its value is passed
 * in. `enabled` is not consulted either -- main.c's own `if(hw_run.enabled)`
 * block is the one gate on that, and duplicating it here would be a second
 * place to keep in step. */
bool hw_run_force_overlay(const struct hw_run_config* cfg, int hw_overlay_key) {
	/* Only an explicit 0 declines: an absent key arrives as the caller's
	 * default 1, and any other value is left meaning "yes" rather than
	 * silently turning a typo into a changed run. */
	return cfg && cfg->force_overlay && hw_overlay_key != 0;
}

static bool need_value(int i, int argc, const char* opt, char* err,
					   size_t err_size) {
	if(i + 1 < argc)
		return true;
	if(err && err_size)
		snprintf(err, err_size, "missing value after %s", opt);
	return false;
}

// TOTAL-ness fix (2026-09-02, cw_run_args hardening lane): every error exit
// from hw_run_parse_args used to just `return false` after possibly having
// already mutated cfg (enabled=true, a profile applied via --cw-profile,
// etc.) from EARLIER flags in the SAME call, before hitting a later bad
// flag. hw_run_config_init() at the top of a *fresh* call always resets
// cfg, but main.c's file-based fallback path deliberately IGNORES this
// function's return value (a parse error must never brick a console boot --
// see main.c's "wii_after_fat" comment), so a partially-mutated cfg with
// enabled left true (or a profile's side-effect fields like
// autopilot_unstick left true, which main.c reads UNCONDITIONALLY, not
// gated on cfg->enabled) could leak into a boot that is supposed to be
// plain, disabled, manual play. parse_fail() makes every error path fully
// re-initialize cfg to the pristine disabled default (not just flip one
// bool) before returning false, so "malformed file" and "no file" are
// byte-identical in their effect on cfg -- the TOTAL function this hardening
// exists to guarantee. Safe to call with cfg == NULL (hw_run_config_init
// itself no-ops on NULL).
static bool parse_fail(struct hw_run_config* cfg) {
	hw_run_config_init(cfg);
	return false;
}

// --cw-mp-endpoint validator (2026-09-12). Deliberately NOT a call into
// mp_codec.c's mp_address_parse: this module is linked ALONE by
// test_hw_run_args_parse (CMakeLists.txt's comment above that target -- "links
// only that one module" -- and hw_run.c's <stdio.h>/<string.h>-only include
// set are a deliberate property of the build graph, not an accident), so
// reaching into source/network here would drag mp_codec's whole link closure
// into a parser regression suite. Instead this accepts a STRICT SUBSET of what
// mp_address_parse accepts, so the connect screen downstream is guaranteed to
// re-derive the SAME host/port from the string stored here:
//   * exactly the two-segment forms mp_address_parse splits on -- "host:port"
//     with a single ':' outside brackets, or the bracketed "[v6]:port" -- so
//     its >2-segment "collapse to the whole input with the default port"
//     fallback and its bare-"host" default-port path are both out of reach;
//   * an EXPLICIT, all-digit port in [1, 65535]. mp_address_parse DEFAULTS a
//     missing/garbage/out-of-range port to 25565 (parse_int_with_default,
//     faithful to Beta's parseIntWithDefault), which is exactly the silent
//     "you got a different server than you asked for" this rejects instead.
//   * a non-empty host (mp_address_parse accepts ":25565" with an EMPTY one --
//     see main.c's own ac_host[0] guard) of printable, non-space characters.
// Keep the two in step if mp_address_parse's shapes ever change.
// --cw-mp-username validator (2026-09-17, mp_chat_login_3m lead ruling 1).
// Deliberately NOT a call into mp_codec.c's mp_login_username_valid, for the
// same build-graph reason mp_endpoint_arg_valid gives below: this module is
// linked ALONE by test_hw_run_args_parse. The accepted set is the STRICT
// [A-Za-z0-9_]{1,16} the ruling names -- a subset of what the login path
// accepts, so anything that passes here is guaranteed to survive downstream.
// A rejected value is a hard parse failure: a run whose receipt cannot be
// bound to its own nonce is worse than a run that refuses to start.
static bool mp_username_arg_valid(const char* value) {
	if(!value)
		return false;
	size_t len = strlen(value);
	if(len == 0 || len > 16)
		return false;
	for(size_t k = 0; k < len; k++) {
		char c = value[k];
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			|| (c >= '0' && c <= '9') || c == '_';
		if(!ok)
			return false;
	}
	return true;
}

static bool mp_endpoint_arg_valid(const char* value, size_t capacity) {
	if(!value)
		return false;
	size_t len = strlen(value);
	// capacity is sizeof(cfg->mp_endpoint): the value must fit WITH its NUL,
	// so 63 characters is the ceiling for a 64-byte field -- never a silent
	// snprintf truncation into a different host.
	if(len == 0 || len + 1 > capacity)
		return false;

	const char* colon = NULL;
	const char* host_start = value;
	const char* host_end = NULL;
	if(value[0] == '[') {
		const char* close = strchr(value, ']');
		if(!close || close[1] != ':')
			return false;
		host_start = value + 1;
		host_end = close;
		colon = close + 1;
		if(host_end == host_start)
			return false;
		for(const char* p = host_start; p < host_end; p++) {
			// Bracketed form is IPv6 (or an IPv4-mapped tail): hex, ':', '.'.
			bool hex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')
				|| (*p >= 'A' && *p <= 'F');
			if(!hex && *p != ':' && *p != '.')
				return false;
		}
	} else {
		colon = strchr(value, ':');
		if(!colon)
			return false; // bare host: mp_address_parse would default the port
		if(strchr(colon + 1, ':'))
			return false; // >2 segments: mp_address_parse collapses the whole
						  // input to the host and defaults the port
		host_end = colon;
		if(host_end == host_start)
			return false; // ":25565" -- empty host
		for(const char* p = host_start; p < host_end; p++) {
			unsigned char c = (unsigned char)*p;
			if(c <= 0x20 || c >= 0x7f || c == '[' || c == ']')
				return false;
		}
	}

	const char* port_text = colon + 1;
	size_t port_len = strlen(port_text);
	if(port_len == 0 || port_len > 5)
		return false; // 5 digits is the whole uint16_t range; longer can only
					  // be an out-of-range port (or leading-zero padding that
					  // mp_address_parse's own 16-byte buffer would reject)
	for(const char* p = port_text; *p; p++) {
		if(*p < '0' || *p > '9')
			return false;
	}
	long port = strtol(port_text, NULL, 10);
	return port >= 1 && port <= 65535;
}

bool hw_run_parse_args(struct hw_run_config* cfg, int argc, char** argv,
					   char* err, size_t err_size) {
	if(!cfg)
		return parse_fail(cfg);
	if(err && err_size)
		err[0] = '\0';

	hw_run_config_init(cfg);
	// --cw-far-view-distance (Stage-2 LOD reach gate, 2026-09-03): tracked
	// separately from cfg->far_view_distance rather than written straight
	// into it, so the override applies AFTER the whole argv is parsed
	// (below the loop) REGARDLESS of whether it appears before or after
	// --cw-profile on the line -- hw_run_apply_profile (the --cw-profile
	// branch below) overwrites cfg->far_view_distance as one of its many
	// profile-default side effects, so writing directly into cfg here would
	// make the override silently lose to a --cw-profile that comes later in
	// argv. INT_MIN sentinel: "not requested" (distinct from the field's own
	// -1 "leave the normal config/default in control" sentinel, which a
	// caller could legitimately pass to force that behavior on a profile
	// that otherwise pins a value, e.g. far_idle_2m/far_walk_6m's 6).
	bool far_view_distance_requested = false;
	int far_view_distance_value = 0;
	// --cw-mp-endpoint (2026-09-12): tracked separately and applied AFTER the
	// loop for the same argv-order reason as far_view_distance above -- the
	// "mp_connect_smoke only" rule has to be judged against the FINAL profile,
	// so `--cw-mp-endpoint host:1 --cw-profile mp_connect_smoke` and the
	// reverse order behave identically instead of the gate depending on which
	// token the harness happened to emit first.
	bool mp_endpoint_requested = false;
	const char* mp_endpoint_value = NULL;
	// --cw-mp-username: same argv-order treatment, same MP-profile gate.
	bool mp_username_requested = false;
	const char* mp_username_value = NULL;
	bool wcc_variant_requested = false;
	int wcc_variant_value = 0;
	const char* wcc_root_value = NULL;
	bool portal_remaining_requested = false;
	int portal_remaining_value = 2;
	/* --cw-vehicle-kind / --cw-vehicle-transfer: same argv-order treatment as
	 * --cw-mp-endpoint -- judged against the FINAL profile after the loop. */
	enum hw_run_vehicle_kind vehicle_kind_value = HW_RUN_VEHICLE_NONE;
	/* --cw-pool-placement: judged against the FINAL profile after the loop
	 * (pool_selftest_3m only). */
	bool pool_placement_requested = false;
	bool pool_placement_rotating = false;
	bool vehicle_transfer_requested = false;
	enum hw_run_vehicle_transfer vehicle_transfer_value
		= HW_RUN_VEHICLE_TRANSFER_FIXTURE_REQUEST;
	for(int i = 1; i < argc; i++) {
		const char* arg = argv[i];
		if(strcmp(arg, "--cw-run-id") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			copy_text(cfg->run_id, sizeof(cfg->run_id), argv[++i]);
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-tel-host") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			copy_text(cfg->telemetry_host, sizeof(cfg->telemetry_host), argv[++i]);
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-tel-port") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			int port = 0;
			if(sscanf(argv[++i], "%d", &port) != 1 || port <= 0
			   || port > 65535) {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-tel-port value");
				return parse_fail(cfg);
			}
			cfg->telemetry_port = port;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-netmask") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			copy_text(cfg->telemetry_netmask, sizeof(cfg->telemetry_netmask),
					  argv[++i]);
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-tel-mode") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* mode = argv[++i];
			if(strcmp(mode, "udp") == 0) {
				cfg->telemetry_pull = false;
			} else if(strcmp(mode, "pull") == 0
					  || strcmp(mode, "tcp_pull") == 0
					  || strcmp(mode, "tcp-listen") == 0) {
				cfg->telemetry_pull = true;
				if(cfg->telemetry_host[0] == '\0')
					copy_text(cfg->telemetry_host, sizeof(cfg->telemetry_host),
							  "pull");
			} else {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-tel-mode value");
				return parse_fail(cfg);
			}
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-profile") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			enum hw_run_profile p;
			const char* name = argv[++i];
			if(!hw_run_profile_from_name(name, &p)) {
				if(err && err_size)
					snprintf(err, err_size, "unknown --cw-profile '%s'", name);
				return parse_fail(cfg);
			}
			hw_run_apply_profile(cfg, p);
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-tps-cpu") == 0) {
			cfg->tps_cpu = true;
		} else if(strcmp(arg, "--cw-settle") == 0) {
			cfg->settle = true;
		} else if(strcmp(arg, "--cw-carve-census") == 0) {
		cfg->carve_census = true;
		} else if(strcmp(arg, "--cw-tel-nonblock") == 0) {
			/* Diagnostic transport experiment, not a run mode: it does not
			 * set cfg->enabled, exactly like --cw-tps-cpu/--cw-settle. */
			cfg->telemetry_nonblock = true;
		} else if(strcmp(arg, "--cw-column-census") == 0) {
			cfg->column_census = true;
		} else if(strcmp(arg, "--cw-split-guest") == 0) {
			/* Experimental local split-screen for THIS run: arm the switch,
			 * auto-join the guest and walk it. No effect on any other
			 * profile's behaviour -- main.c reads these fields directly. */
			cfg->split_guest = true;
			cfg->split_guest_walk = true;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-split-actions") == 0) {
			/* S1-d: arm the scripted guest ACTION sequence for THIS run.
			 * Implies --cw-split-guest -- a script with no guest body has
			 * nothing to act with, and silently arming half of it would be
			 * the "quietly does nothing" failure mode this flag exists to
			 * make impossible. */
			cfg->split_guest = true;
			cfg->split_guest_walk = true;
			cfg->split_guest_actions = true;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-set") == 0) {
			// Run-scoped INTEGER config override ("key=value", repeatable up
			// to HW_RUN_CW_SET_MAX -- hw_run.h's cw_set field comment has the
			// full story). Deliberately non-fatal on a malformed entry: a
			// bad harness-generated line should degrade to "fewer overrides
			// applied", not a parse_fail() that bricks the whole boot --
			// UNLIKE every other --cw-* flag above/below, whose malformed
			// value calls parse_fail(cfg). Does not set cfg->enabled (same
			// as --cw-settle/--cw-carve-census/--cw-column-census above).
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* kv = argv[++i];
			const char* eq = strchr(kv, '=');
			bool ok = eq != NULL && eq != kv;
			size_t klen = ok ? (size_t)(eq - kv) : 0;
			if(ok && klen >= sizeof(cfg->cw_set[0].key))
				ok = false;
			for(size_t ci = 0; ok && ci < klen; ci++) {
				char c = kv[ci];
				ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
					|| (c >= '0' && c <= '9') || c == '.' || c == '_';
			}
			long value = 0;
			if(ok) {
				const char* vstr = eq + 1;
				if(vstr[0] == '\0') {
					ok = false;
				} else {
					char* endp = NULL;
					value = strtol(vstr, &endp, 10);
					if(!endp || *endp != '\0')
						ok = false;
				}
			}
			if(ok && cfg->cw_set_n >= HW_RUN_CW_SET_MAX)
				ok = false; // cap reached: counted below, not silently dropped
			if(ok && klen == strlen("debug.weather_force_int")
			   && strncmp(kv, "debug.weather_force_int", klen) == 0
			   && (value < 0 || value > 2))
				ok = false;
			if(!ok) {
				fprintf(stderr,
						"[hw_run] --cw-set '%s' malformed or over the "
						"%d-entry cap, ignored\n",
						kv, HW_RUN_CW_SET_MAX);
				cfg->cw_set_malformed++;
			} else {
				memcpy(cfg->cw_set[cfg->cw_set_n].key, kv, klen);
				cfg->cw_set[cfg->cw_set_n].key[klen] = '\0';
				cfg->cw_set[cfg->cw_set_n].value = (int)value;
				cfg->cw_set_n++;
			}
		} else if(strcmp(arg, "--cw-anchor-sep") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* value = argv[++i];
			unsigned long n = 0;
			bool valid = *value != 0;
			for(const char* c = value; *c && valid; c++) {
				valid = *c >= '0' && *c <= '9';
				if(valid) {
					n = n * 10 + (unsigned)(*c - '0');
					valid = n <= 1048576;
				}
			}
			if(!valid) {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-anchor-sep value");
				return parse_fail(cfg);
			}
			cfg->anchor_sep = (int)n;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-duration") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			int seconds = 0;
			if(sscanf(argv[++i], "%d", &seconds) != 1 || seconds <= 0) {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-duration value");
				return parse_fail(cfg);
			}
			cfg->duration_seconds = seconds;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-pin-cameras") == 0) {
			/* Debug capture lever (hw_run.h pin_cameras). */
			cfg->pin_cameras = true;
		} else if(strcmp(arg, "--cw-auto-exit") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* v = argv[++i];
			if(strcmp(v, "0") == 0 || strcmp(v, "off") == 0
			   || strcmp(v, "false") == 0) {
				cfg->auto_exit = false;
			} else if(strcmp(v, "1") == 0 || strcmp(v, "on") == 0
					  || strcmp(v, "true") == 0) {
				cfg->auto_exit = true;
			} else {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-auto-exit value");
				return parse_fail(cfg);
			}
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-exit-grace") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			float seconds = 0.0F;
			if(sscanf(argv[++i], "%f", &seconds) != 1 || seconds < 0.0F) {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-exit-grace value");
				return parse_fail(cfg);
			}
			cfg->auto_exit_grace_seconds = seconds;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-autoload-name") == 0
				  || strcmp(arg, "--cw-autoload-name-hex") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* name = argv[++i];
			/* Staged argument files split on whitespace. Encode the directory
			 * bytes rather than silently loading a different, truncated name. */
			char decoded[sizeof(cfg->autoload_name)];
			if(strcmp(arg, "--cw-autoload-name-hex") == 0) {
				size_t encoded_len = strlen(name);
				bool valid = encoded_len > 0 && encoded_len % 2 == 0
					&& encoded_len / 2 < sizeof(decoded);
				for(size_t j = 0; valid && j < encoded_len; j += 2) {
					unsigned value = 0;
					for(size_t k = 0; k < 2; k++) {
						unsigned char c = (unsigned char)name[j + k];
						unsigned digit = c >= '0' && c <= '9' ? c - '0'
							: c >= 'a' && c <= 'f' ? c - 'a' + 10
							: c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
						if(digit == 16) { valid = false; break; }
						value = value * 16 + digit;
					}
					if(value < 0x20 || value == 0x7f) valid = false;
					decoded[j / 2] = (char)value;
				}
				if(!valid) {
					if(err && err_size)
						snprintf(err, err_size, "invalid --cw-autoload-name-hex value");
					return parse_fail(cfg);
				}
				decoded[encoded_len / 2] = '\0';
				name = decoded;
			}
			size_t len = strlen(name);
			bool ok = len > 0 && len < sizeof(cfg->autoload_name)
				&& strcmp(name, "+new") != 0 && strcmp(name, ".") != 0
				&& strcmp(name, "..") != 0;
			for(size_t ci = 0; ok && ci < len; ci++) {
				unsigned char c = (unsigned char)name[ci];
				// world DIRECTORY name: no path separators or control chars
				ok = c >= 0x20 && c != '/' && c != '\\';
			}
			if(!ok) {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-autoload-name value");
				return parse_fail(cfg);
			}
			memcpy(cfg->autoload_name, name, len + 1);
		} else if(strcmp(arg, "--cw-wcc-variant") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			/* menu_ap_copy_variant's names, restated: this module is linked
			 * ALONE by test_hw_run_args_parse. */
			static const char* const wcc_names[] = {
				"ok", "low_space", "write_error", "interrupt", "recover"};
			const char* v = argv[++i];
			wcc_variant_value = -1;
			for(int k = 0; k < 5; k++)
				if(strcmp(v, wcc_names[k]) == 0)
					wcc_variant_value = k;
			if(wcc_variant_value < 0) {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-wcc-variant value");
				return parse_fail(cfg);
			}
			wcc_variant_requested = true;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-wcc-root") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* v = argv[++i];
			size_t len = strlen(v);
			bool ok = len >= 1 && len <= 40;
			for(size_t k = 0; ok && k < len; k++) {
				char c = v[k];
				ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
					|| (c >= '0' && c <= '9') || c == '_';
			}
			if(!ok) {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-wcc-root value");
				return parse_fail(cfg);
			}
			wcc_root_value = v;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-save-migrate-regions") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			int regions = 0;
			if(sscanf(argv[++i], "%d", &regions) != 1 || regions < 2
			   || regions > 32) {
				if(err && err_size)
					snprintf(err, err_size,
							 "invalid --cw-save-migrate-regions value");
				return parse_fail(cfg);
			}
			cfg->save_migrate_regions = regions;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-cworld-mode") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* mode = argv[++i];
			if(strcmp(mode, "survival") == 0) {
				cfg->cworld_mode = HW_RUN_CWORLD_MODE_SURVIVAL;
			} else if(strcmp(mode, "creative") == 0) {
				cfg->cworld_mode = HW_RUN_CWORLD_MODE_CREATIVE;
			} else {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-cworld-mode value");
				return parse_fail(cfg);
			}
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-far-storage") == 0) {
			// S2 measurement (docs/stage2_lod_plan_2026-09-03.md): stored on
			// cfg only -- see hw_run.h's enum hw_run_far_storage comment for
			// why source/main.c has nothing to consume this with yet (the
			// lod.far_storage/world_set_far_storage lineage is unmerged on
			// this branch). Byte-identical to before this flag existed when
			// absent (cfg->far_storage stays HW_RUN_FAR_STORAGE_UNSET).
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* storage = argv[++i];
			if(strcmp(storage, "voxel") == 0) {
				cfg->far_storage = HW_RUN_FAR_STORAGE_VOXEL;
			} else if(strcmp(storage, "skin") == 0) {
				cfg->far_storage = HW_RUN_FAR_STORAGE_SKIN;
			} else {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-far-storage value");
				return parse_fail(cfg);
			}
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-far-view-distance") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			int fvd = 0;
			if(sscanf(argv[++i], "%d", &fvd) != 1) {
				if(err && err_size)
					snprintf(err, err_size,
							 "invalid --cw-far-view-distance value");
				return parse_fail(cfg);
			}
			far_view_distance_requested = true;
			far_view_distance_value = fvd;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-far-send-near-owed") == 0) {
			// T2 far-send share (2026-09-04, lane/far-send-share). Unlike
			// --cw-far-view-distance above, NO profile writes this field, so
			// there is nothing for a later --cw-profile to clobber and the
			// value can be stored straight into cfg here (same shape as
			// --cw-far-storage). Absent -> cfg->far_send_near_owed stays -1
			// and source/main.c leaves "lod.far_send_near_owed" (default 8
			// since 2026-09-04, lane/far-share-default-8; was 0/strict before
			// that) in control -- pass 0 explicitly to force the strict pre-
			// 2026-09-04 gate on an armed run. Negative values other than -1
			// are rejected rather than silently clamped -- "-1" is a
			// documented sentinel, "-3" is a typo.
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			int owed = 0;
			if(sscanf(argv[++i], "%d", &owed) != 1 || owed < -1) {
				if(err && err_size)
					snprintf(err, err_size,
							 "invalid --cw-far-send-near-owed value");
				return parse_fail(cfg);
			}
			if(owed > HW_RUN_FAR_SEND_NEAR_OWED_MAX) {
				fprintf(stderr,
						"[hw_run] --cw-far-send-near-owed %d clamped to %d "
						"(valid range [0, %d], or -1 to leave the config in "
						"control)\n",
						owed, HW_RUN_FAR_SEND_NEAR_OWED_MAX,
						HW_RUN_FAR_SEND_NEAR_OWED_MAX);
				owed = HW_RUN_FAR_SEND_NEAR_OWED_MAX;
			}
			cfg->far_send_near_owed = owed;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-mp-endpoint") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* endpoint = argv[++i];
			if(!mp_endpoint_arg_valid(endpoint, sizeof(cfg->mp_endpoint))) {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-mp-endpoint value");
				return parse_fail(cfg);
			}
			mp_endpoint_requested = true;
			mp_endpoint_value = endpoint;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-mp-username") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* username = argv[++i];
			if(!mp_username_arg_valid(username)) {
				if(err && err_size)
					snprintf(err, err_size,
							 "invalid --cw-mp-username value "
							 "(expected [A-Za-z0-9_]{1,16})");
				return parse_fail(cfg);
			}
			mp_username_requested = true;
			mp_username_value = username;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-portal-remaining-ticks") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* value = argv[++i];
			if(value[1] != '\0' || (value[0] != '0' && value[0] != '1'
			   && value[0] != '2' && value[0] != '3' && value[0] != '5')) {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-portal-remaining-ticks");
				return parse_fail(cfg);
			}
			portal_remaining_requested = true;
			portal_remaining_value = value[0] - '0';
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-pool-placement") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* place = argv[++i];
			if(strcmp(place, "rotating") == 0)
				pool_placement_rotating = true;
			else if(strcmp(place, "lowest") == 0)
				pool_placement_rotating = false;
			else {
				if(err && err_size)
					snprintf(err, err_size,
							 "invalid --cw-pool-placement value (rotating|lowest)");
				return parse_fail(cfg);
			}
			pool_placement_requested = true;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-vehicle-kind") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* kind = argv[++i];
			if(strcmp(kind, "boat") == 0)
				vehicle_kind_value = HW_RUN_VEHICLE_BOAT;
			else if(strcmp(kind, "pig") == 0)
				vehicle_kind_value = HW_RUN_VEHICLE_PIG;
			else if(strcmp(kind, "cart") == 0)
				vehicle_kind_value = HW_RUN_VEHICLE_CART;
			else {
				if(err && err_size)
					snprintf(err, err_size,
							 "invalid --cw-vehicle-kind value (boat|pig|cart)");
				return parse_fail(cfg);
			}
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-vehicle-transfer") == 0) {
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* mode = argv[++i];
			if(strcmp(mode, "fixture_request") == 0)
				vehicle_transfer_value = HW_RUN_VEHICLE_TRANSFER_FIXTURE_REQUEST;
			else if(strcmp(mode, "contact") == 0)
				vehicle_transfer_value = HW_RUN_VEHICLE_TRANSFER_CONTACT;
			else {
				if(err && err_size)
					snprintf(err, err_size,
							 "invalid --cw-vehicle-transfer value "
							 "(contact|fixture_request)");
				return parse_fail(cfg);
			}
			vehicle_transfer_requested = true;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-rz-fault") == 0) {
			/* residency-1005/4 item 12: <host|guest>@<seconds>, seconds in
			 * 1..3600. The whole value must parse; anything else refuses
			 * the run (hw_run disabled with an error), never arms. */
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* v = argv[++i];
			int actor = -1;
			const char* at = NULL;
			if(strncmp(v, "host@", 5) == 0) {
				actor = 0;
				at = v + 5;
			} else if(strncmp(v, "guest@", 6) == 0) {
				actor = 1;
				at = v + 6;
			}
			char* endp = NULL;
			long secs = at && *at ? strtol(at, &endp, 10) : -1;
			if(actor < 0 || !endp || *endp != '\0' || secs < 1
			   || secs > 3600) {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-rz-fault value "
						"(host@<1..3600> | guest@<1..3600>)");
				return parse_fail(cfg);
			}
			cfg->rz_fault_set = true;
			cfg->rz_fault_actor = actor;
			cfg->rz_fault_delay_s = (int)secs;
			cfg->enabled = true;
		} else if(strcmp(arg, "--cw-seed") == 0) {
			// Fixed worldgen seed for the "+new" autoload sentinel (harness
			// fixed-seed lane, 2026-09-04) -- see hw_run.h's cfg->seed
			// comment for the full plumbing story. strtoll (not sscanf
			// "%lld"): mirrors mp_codec.c's strtol-based numeric-field
			// parsing style, and sidesteps relying on a devkitPPC newlib
			// scanf build actually supporting long long conversions (never
			// exercised elsewhere in this codebase). Full string must be
			// consumed and non-empty -- "12345x" or "" are rejected, not
			// silently truncated to the numeric prefix.
			if(!need_value(i, argc, arg, err, err_size))
				return parse_fail(cfg);
			const char* seed_text = argv[++i];
			char* endp = NULL;
			long long value = strtoll(seed_text, &endp, 10);
			if(seed_text[0] == '\0' || !endp || *endp != '\0') {
				if(err && err_size)
					snprintf(err, err_size, "invalid --cw-seed value");
				return parse_fail(cfg);
			}
			cfg->seed = (int64_t)value;
			cfg->seed_set = true;
			/* An explicitly requested seed outranks, and outlives, any
			 * profile pin (hw_run_apply_profile's preamble). */
			cfg->seed_from_profile = false;
			cfg->enabled = true;
		} else if(strncmp(arg, "--cw-", 5) == 0) {
			if(err && err_size)
				snprintf(err, err_size, "unknown hardware-run option '%s'", arg);
			return parse_fail(cfg);
		}
	}
	if(portal_remaining_requested) {
		if(cfg->profile != HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M
		   && cfg->profile != HW_RUN_PROFILE_SPLIT_DIMS_6M) {
			if(err && err_size)
				snprintf(err, err_size,
					"--cw-portal-remaining-ticks requires split_portal_quit_4m or split_dims_6m");
			return parse_fail(cfg);
		}
		cfg->portal_remaining_ticks = portal_remaining_value;
	}

	/* host_vehicle_smoke_8m: the kind is REQUIRED for that profile and a hard
	 * error for any other (a harness that asks far_walk_6m for a boat has a
	 * bug in its argument construction; a quietly ignored flag would look
	 * honoured). The transfer mode is likewise vehicle-profile-only. */
	bool vehicle_profile = cfg->profile == HW_RUN_PROFILE_HOST_VEHICLE_SMOKE_8M;
	if(vehicle_profile && vehicle_kind_value == HW_RUN_VEHICLE_NONE) {
		if(err && err_size)
			snprintf(err, err_size,
					 "host_vehicle_smoke_8m requires --cw-vehicle-kind "
					 "boat|pig|cart");
		return parse_fail(cfg);
	}
	if(!vehicle_profile
	   && (vehicle_kind_value != HW_RUN_VEHICLE_NONE
		   || vehicle_transfer_requested)) {
		if(err && err_size)
			snprintf(err, err_size,
					 "--cw-vehicle-kind/--cw-vehicle-transfer require "
					 "host_vehicle_smoke_8m (got '%s')",
					 hw_run_profile_name(cfg->profile));
		return parse_fail(cfg);
	}
	cfg->vehicle_kind = vehicle_kind_value;
	cfg->vehicle_transfer = vehicle_transfer_value;

	/* pool_selftest_3m: the placement switch is a self-test instrument and
	 * a hard error for any other profile (a release play session must never
	 * run the pre-R4 placement because of a stray flag). */
	if(pool_placement_requested
	   && cfg->profile != HW_RUN_PROFILE_POOL_SELFTEST_3M) {
		if(err && err_size)
			snprintf(err, err_size,
					 "--cw-pool-placement requires pool_selftest_3m (got '%s')",
					 hw_run_profile_name(cfg->profile));
		return parse_fail(cfg);
	}
	cfg->pool_placement_rotating
		= cfg->pool_selftest && pool_placement_rotating;
	/* split_regrow_8m moves the synthetic anchor: without a separation there
	 * is nothing to move, and the run would silently measure one anchor. */
	if(cfg->profile == HW_RUN_PROFILE_SPLIT_REGROW_8M && cfg->anchor_sep <= 0) {
		if(err && err_size)
			snprintf(err, err_size, "split_regrow_8m requires --cw-anchor-sep > 0");
		return parse_fail(cfg);
	}

	// Apply the --cw-far-view-distance override (if any) AFTER the loop, so
	// it always wins regardless of argv order relative to --cw-profile --
	// see far_view_distance_requested's declaration comment above. -1 passes
	// through unclamped (the field's own documented "leave the normal
	// config/default in control" sentinel -- lets a caller force that
	// behavior even on far_idle_2m/far_walk_6m, which otherwise pin 6);
	// every other requested value is clamped into
	// [HW_RUN_FAR_VIEW_DISTANCE_MIN, HW_RUN_FAR_VIEW_DISTANCE_MAX] with a
	// logged line when clamping actually changed the value, per the
	// Stage-2 LOD reach gate (docs/stage2_lod_plan_2026-09-03.md 3).
	if(far_view_distance_requested) {
		int clamped = far_view_distance_value;
		if(clamped != -1) {
			if(clamped < HW_RUN_FAR_VIEW_DISTANCE_MIN)
				clamped = HW_RUN_FAR_VIEW_DISTANCE_MIN;
			else if(clamped > HW_RUN_FAR_VIEW_DISTANCE_MAX)
				clamped = HW_RUN_FAR_VIEW_DISTANCE_MAX;
			if(clamped != far_view_distance_value)
				fprintf(stderr,
						"[hw_run] --cw-far-view-distance %d clamped to %d "
						"(valid range [%d, %d])\n",
						far_view_distance_value, clamped,
						HW_RUN_FAR_VIEW_DISTANCE_MIN,
						HW_RUN_FAR_VIEW_DISTANCE_MAX);
		}
		cfg->far_view_distance = clamped;
	}

	// Apply --cw-mp-endpoint AFTER the loop for the same argv-order reason,
	// and gate it on the FINAL profile: this override exists for the
	// unattended MP profiles ONLY (mp_connect_smoke, mp_lan_walk_10m,
	// mp_reconnect_soak -- widened from smoke-only on 2026-09-12 because the
	// card's saved mp.server may point at a public host while a LAN
	// regression run needs the local mc173 peer). Any other profile is a hard parse
	// failure rather than a silently ignored flag -- a harness that asks
	// far_walk_6m to join a server has a bug in its argument construction, and
	// a run that quietly ignored the request would look like it honored it.
	bool mp_profile = cfg->profile == HW_RUN_PROFILE_MP_CONNECT_SMOKE
		|| cfg->profile == HW_RUN_PROFILE_MP_LAN_WALK_10M
		|| cfg->profile == HW_RUN_PROFILE_MP_RECONNECT_SOAK
		/* mp_chat_login_3m joins a run-local mc173-diag fixture, so it needs
		 * the same endpoint override the other unattended MP profiles use --
		 * and mp_chat_chord_1m joins the same fixture the same way. */
		|| cfg->profile == HW_RUN_PROFILE_MP_CHAT_LOGIN_3M
		|| cfg->profile == HW_RUN_PROFILE_MP_CHAT_CHORD_1M
		/* mp_play_10m joins the same run-local fixture, and its username IS
		 * the run nonce every receipt is matched against (design section 2),
		 * so it needs BOTH flags more than any profile above it. */
		|| cfg->profile == HW_RUN_PROFILE_MP_PLAY_10M
		|| cfg->profile == HW_RUN_PROFILE_MP_PAUSE_MENU_3M;
	if(mp_endpoint_requested) {
		if(!mp_profile) {
			if(err && err_size)
				snprintf(err, err_size,
						 "--cw-mp-endpoint requires an MP profile "
						 "(mp_connect_smoke, mp_lan_walk_10m, "
						 "mp_reconnect_soak, mp_chat_login_3m, "
						 "mp_chat_chord_1m or mp_play_10m; got '%s')",
						 hw_run_profile_name(cfg->profile));
			return parse_fail(cfg);
		}
		copy_text(cfg->mp_endpoint, sizeof(cfg->mp_endpoint),
				  mp_endpoint_value);
	}

	// --cw-mp-username: the identity half of the same override, gated on the
	// SAME final-profile rule for the same reason -- a harness that asks
	// far_walk_6m to join under a chosen name has a bug in its argument
	// construction, and a quietly ignored request would look honored.
	if(mp_username_requested) {
		if(!mp_profile) {
			if(err && err_size)
				snprintf(err, err_size,
						 "--cw-mp-username requires an MP profile "
						 "(mp_connect_smoke, mp_lan_walk_10m, "
						 "mp_reconnect_soak, mp_chat_login_3m, "
						 "mp_chat_chord_1m or mp_play_10m; got '%s')",
						 hw_run_profile_name(cfg->profile));
			return parse_fail(cfg);
		}
		copy_text(cfg->mp_username, sizeof(cfg->mp_username),
				  mp_username_value);
	}

	// --cw-wcc-variant / --cw-wcc-root: world_copy_convert ONLY (the final
	// profile, same rule as the MP flags above), and that profile needs a root.
	bool wcc_profile = cfg->profile == HW_RUN_PROFILE_WORLD_COPY_CONVERT;
	if((wcc_variant_requested || wcc_root_value) && !wcc_profile) {
		if(err && err_size)
			snprintf(err, err_size,
					 "--cw-wcc-variant/--cw-wcc-root require world_copy_convert "
					 "(got '%s')", hw_run_profile_name(cfg->profile));
		return parse_fail(cfg);
	}
	if(wcc_profile) {
		if(!wcc_root_value) {
			if(err && err_size)
				snprintf(err, err_size,
						 "world_copy_convert requires --cw-wcc-root");
			return parse_fail(cfg);
		}
		cfg->wcc_variant = wcc_variant_value;
		copy_text(cfg->wcc_root, sizeof(cfg->wcc_root), wcc_root_value);
	}

	if(cfg->enabled && cfg->telemetry_host[0] == '\0') {
		if(err && err_size)
			snprintf(err, err_size, "--cw-tel-host is required for hardware runs");
		return parse_fail(cfg);
	}
	if(strcmp(cfg->telemetry_host, "pull") == 0
	   || strcmp(cfg->telemetry_host, "tcp-listen") == 0)
		cfg->telemetry_pull = true;
	return true;
}

// TOTAL-ness fix (2026-09-02, cw_run_args hardening lane): mirrors the
// stage-side PUT-time validator's limits (tools/wii_stage_server/source/
// dol_runprep.h CW_RUN_ARGS_MAX_BYTES/CW_RUN_ARGS_MAX_ARGC) so a
// hand-edited, version-skewed, or corrupted cw_run_args.txt gets the SAME
// rejection here, at READ time, that a well-behaved staging tool would have
// applied at WRITE time -- defense in depth, never assume the file on the
// card was actually produced by that validator. Not #include'd directly:
// that header belongs to the PC-only stage-server tool build, a separate
// target from this Wii/PC game build. Mirrored, not shared -- if either
// changes, keep the other in step (both size buf/argv below).
#define HW_RUN_ARGS_FILE_MAX_BYTES 511u
#define HW_RUN_ARGS_FILE_MAX_ARGC 32u

bool hw_run_parse_file(struct hw_run_config* cfg, const char* path, char* err,
					   size_t err_size) {
	if(!cfg || !path)
		return false;
	if(err && err_size)
		err[0] = '\0';
	FILE* f = fopen(path, "rb");
	if(!f)
		return true;

	// Root fix for the missing length check: the old fread(buf, 1,
	// sizeof(buf)-1, f) silently accepted (and truncated) a file bigger
	// than the buffer, potentially cutting a token/value in half and
	// parsing whatever fragment survived as if it were the whole,
	// deliberate content. Peeking one more byte after an exactly-full read
	// distinguishes "the file was exactly this size" from "there was more"
	// without needing fseek/ftell or a second pass.
	char buf[HW_RUN_ARGS_FILE_MAX_BYTES + 1];
	size_t n = fread(buf, 1, HW_RUN_ARGS_FILE_MAX_BYTES, f);
	bool overflowed = n == HW_RUN_ARGS_FILE_MAX_BYTES && fgetc(f) != EOF;
	fclose(f);
	if(overflowed) {
		if(err && err_size)
			snprintf(err, err_size, "cw_run_args file exceeds %u bytes",
					 HW_RUN_ARGS_FILE_MAX_BYTES);
		hw_run_config_init(cfg);
		return false;
	}
	buf[n] = '\0';

	// Reject NUL/control bytes outside the token-delimiter set UP FRONT,
	// exactly like the stage-side validator's args_contains_nul /
	// args_control_char checks, instead of letting a NUL silently truncate
	// the effective C-string mid-file (safe -- no overrun/hang -- but
	// silent) or letting a stray low control byte ride along as opaque
	// token content. Scanning the raw byte count `n` (not strlen(buf)) is
	// what actually catches an embedded NUL: strlen would stop at the very
	// byte being checked for.
	for(size_t bi = 0; bi < n; bi++) {
		unsigned char c = (unsigned char)buf[bi];
		if(c == '\0') {
			if(err && err_size)
				snprintf(err, err_size, "cw_run_args contains a NUL byte");
			hw_run_config_init(cfg);
			return false;
		}
		if(c < 0x20 && c != '\t' && c != '\r' && c != '\n') {
			if(err && err_size)
				snprintf(err, err_size,
						 "cw_run_args contains a control byte (0x%02x)", c);
			hw_run_config_init(cfg);
			return false;
		}
	}

	char* argv[HW_RUN_ARGS_FILE_MAX_ARGC];
	int argc = 0;
	argv[argc++] = (char*)"cw_run_args";
	bool too_many = false;
	for(char* tok = strtok(buf, " \t\r\n"); tok; tok = strtok(NULL, " \t\r\n")) {
		if(tok[0] == '#')
			break;
		if(argc >= (int)HW_RUN_ARGS_FILE_MAX_ARGC) {
			too_many = true;
			break;
		}
		argv[argc++] = tok;
	}
	if(too_many) {
		if(err && err_size)
			snprintf(err, err_size, "cw_run_args has more than %u tokens",
					 HW_RUN_ARGS_FILE_MAX_ARGC - 1u);
		hw_run_config_init(cfg);
		return false;
	}
	if(argc == 1)
		return true;
	return hw_run_parse_args(cfg, argc, argv, err, err_size);
}

bool hw_run_auto_exit_ready(const struct hw_run_config* cfg,
							bool profile_done_sent,
							float seconds_since_profile_done) {
	// Hard gate #1: only ever consulted for an ARMED run. cfg->enabled is
	// false for every normal/manual launch (hw_run_config_init's default),
	// so normal play can never satisfy this even if a caller mistakenly
	// asked outside the hw_run.enabled branch.
	if(!cfg || !cfg->enabled || !cfg->auto_exit)
		return false;
	// Hard gate #2: watchdog_inject's whole point is the deliberate,
	// non-returning stall that exercises the watchdog dump path -- it sends
	// "injecting_watchdog" (not "ok") and then spins forever in a SEPARATE
	// main.c branch this function never reaches, but excluding it here too
	// means the gate can never be satisfied for that profile even if that
	// call order ever changes.
	if(cfg->watchdog_inject)
		return false;
	if(!profile_done_sent)
		return false;
	return seconds_since_profile_done >= cfg->auto_exit_grace_seconds;
}

bool hw_run_duration_reached_ms(uint32_t elapsed_ms, int duration_seconds) {
	// "No duration timer" -- the driver-owned and watchdog profiles reach
	// their end some other way, and a negative value can only be a caller
	// bug. Same refusal the `duration_seconds > 0` guard in main.c made.
	if(duration_seconds <= 0)
		return false;
	// 64-bit on both sides: see the header for why neither a 32-bit product
	// nor the uint32 counter alone is safe at an arbitrary --cw-duration.
	return (uint64_t)elapsed_ms >= (uint64_t)duration_seconds * UINT64_C(1000);
}

bool hw_run_tps_duration_end(const struct hw_run_config* cfg, float elapsed) {
	return cfg->duration_seconds > 0 && elapsed >=
		(float)(cfg->duration_seconds > HW_RUN_TPS_SEAL_MARGIN_SECONDS ?
			cfg->duration_seconds - HW_RUN_TPS_SEAL_MARGIN_SECONDS : 0);
}

bool hw_run_tps_window_end(const struct hw_run_config* cfg, float run_elapsed,
	bool anchor_measuring, uint32_t anchor_ms) {
	/* split_anchor_6m has never ended its window on a clock: its measured
	 * window is the readiness epoch, which the run clock knows nothing about,
	 * so the completion block's own tps_observer_frame(false) seals it. */
	if(cfg->profile == HW_RUN_PROFILE_SPLIT_ANCHOR_6M
	   || cfg->profile == HW_RUN_PROFILE_SPLIT_REGROW_8M)
		return false;
	/* The other readiness-epoch profiles close where they opened. Their
	 * duration is counted on the readiness clock (hw_run_apply_profile's 360,
	 * main.c's completion clock) and their window opens on the readiness latch
	 * (hw_run_tps_measuring), so reading the END on the run clock spent the
	 * whole load on the measurement: with installed readiness at +25 s a
	 * split_play_8m window sealed at run 358 s having measured 333 s of the
	 * 358 s it is graded on. Same seal margin as every other profile -- two
	 * seconds for the collector/sender to receive the final aggregate -- and
	 * the same integer millisecond the heartbeat publishes as "anchor_ms". */
	if(cfg->profile == HW_RUN_PROFILE_SPLIT_PLAY_8M
	   || cfg->profile == HW_RUN_PROFILE_SPLIT_ACTIONS_6M
	   || cfg->profile == HW_RUN_PROFILE_SPLIT_DIMS_6M) {
		if(!anchor_measuring || cfg->duration_seconds <= 0)
			return false;
		/* The same margin clamp hw_run_tps_duration_end applies, so a
		 * duration at or below the margin behaves the same on both clocks.
		 * Not textually identical: that one compares against the unsigned
		 * HW_RUN_TPS_SEAL_MARGIN_SECONDS (so its `int` is converted), this
		 * one casts the macro to int and compares signed. The two agree for
		 * every input that reaches here because `duration_seconds <= 0` is
		 * refused two lines above; they would diverge for a negative
		 * duration, which cannot get past that guard. */
		int measured = cfg->duration_seconds
				> (int)HW_RUN_TPS_SEAL_MARGIN_SECONDS ?
			cfg->duration_seconds - (int)HW_RUN_TPS_SEAL_MARGIN_SECONDS : 0;
		return (uint64_t)anchor_ms >= (uint64_t)measured * UINT64_C(1000);
	}
	return hw_run_tps_duration_end(cfg, run_elapsed);
}

/* One continuous interval after the first loaded-world settle, including
 * subsequent menu/reconnect work, ending before the collector deadline. */
bool hw_run_tps_measuring(struct hw_run_tps_window* window,
	const struct hw_run_config* cfg, bool world_loaded, bool anchor_measuring,
	bool done, uint64_t now_ms) {
	if(!cfg->tps_cpu || done) {
		window->measuring = false;
		return false;
	}
	/* The profiles main.c arms the two-anchor observer for (s_anchor.enabled)
	 * gate the window on the installed-readiness latch instead of the
	 * post-load settle, so loading and the guest Join handshake never consume
	 * measured time. split_play_8m and split_actions_6m belong here for the
	 * same reason split_anchor_6m does -- they run the SAME six measured
	 * minutes on the readiness epoch (main.c's completion clock, hw_run.c's
	 * duration 360) and the collector already grades them that way
	 * (wii_telemetry_server.py: tps_settle_ms is None for every split
	 * profile, pinned by tests/test_analyze_tps_walk.py). split_recovery_6m
	 * is deliberately NOT here: main.c arms no anchor observer for it, so
	 * anchor_measuring could never rise and the window would never open. */
	if(cfg->profile == HW_RUN_PROFILE_SPLIT_ANCHOR_6M
	   /* colcap-1006 R5 item 3: split_anchor_6m's readiness clock. */
	   || cfg->profile == HW_RUN_PROFILE_SPLIT_REGROW_8M
	   || cfg->profile == HW_RUN_PROFILE_SPLIT_PLAY_2M
	   || cfg->profile == HW_RUN_PROFILE_SPLIT_PLAY_8M
	   || cfg->profile == HW_RUN_PROFILE_SPLIT_ACTIONS_6M
	   || cfg->profile == HW_RUN_PROFILE_SPLIT_DIMS_6M) {
		/* Record the answer, exactly like the settle branch below and the
		 * completion early-out above. main.c's ONE-per-window `tps_window`
		 * observer record is emitted on the rising edge of THIS field
		 * (main.c: `tps_was_measuring = tps_window.measuring` vs this
		 * frame's answer), so returning true without writing it made every
		 * measuring frame look like the first one: ~10,800 observer records
		 * per run at 30 fps into a ring with three observer-usable slots
		 * that real hardware drains at ~2 records/s, which pushes
		 * `anchor_frames` packets out of it -- and a dropped anchor_frames
		 * packet is a hard collector FAIL ("missing per-frame samples"). */
		window->measuring = anchor_measuring;
		return window->measuring;
	}
	if(!window->loaded && world_loaded) {
		window->loaded = true;
		window->loaded_ms = now_ms;
	}
	if(window->loaded && now_ms - window->loaded_ms >= HW_RUN_TPS_SETTLE_MS)
		window->measuring = true;
	return window->measuring;
}
