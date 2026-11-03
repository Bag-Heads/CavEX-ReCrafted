#ifndef CAVEWII_F1_DISPATCH_ONLY
#include "column_observer.h"
#include "column_view.h"
#include "settle_observer.h"
#include "tps_observer.h"
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
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <stdbool.h>
#include "platform/pc/heap_stats.h"
#endif

#ifdef PLATFORM_WII
#include <fat.h>
#include <gccore.h>
#include <malloc.h>
#include "platform/wii/net_bringup.h" // C1: net_bringup_shutdown() at process teardown
#endif

#include "platform/remote_capture.h"
#include "block/block_leaves.h" // leaves follow render.fancy_graphics
#include "chunk_mesher.h"
#include "daytime.h"
#include "game/attack_assist.h"
#include "game/difficulty.h"
#include "game/game_state.h"
#include "game/split_layout.h"
#include "game/split_guest_eye.h"
#include "game/gui/screen.h"
#include "game/gui/backdrop_bake.h"
#include "game/gui/spawn_search_wait.h" // hw1030n: heartbeat during the spawn search
#include "game/gui/backdrop_settle.h"
#include "game/gui/startup_credits.h"
#include "game/camera.h"
#include "game/gui/achievement_notice.h"
#include "game/gui/screen_stats_save.h"
#include "game/achievements/achievements.h"
#include "game/gui/loading_notice.h"
#include "game/sign_text_render_feed.h"
#include "game/sign_text_store.h"
#include "graphics/gfx_util.h"
#include "graphics/fire_overlay.h"
#include "graphics/gui_util.h"
#include "graphics/gfx_settings.h"
#include "graphics/render_entity.h"
#include "graphics/render_item.h" /* ci-1101/31: per-viewport needle */
#include "graphics/render_entity_fire.h"
#include "graphics/render_name_tag_draw.h"
#include "graphics/render_piston_moving.h"
#include "graphics/render_weather.h"
#include "graphics/render_lightning.h"
#include "item/item_nbt.h"
#include "item/recipe.h"
#include "game/autopilot.h"
#include "game/client_world_context.h"
#include "game/client_display_tick.h"
#include "game/far_ring_keepup.h"
#include "game/hw_run.h"
#include "game/pool_selftest.h"
#include "platform/displaylist_scene_hash.h"
#include "game/weather_force_config.h"
#include "game/chat_login_script.h"
#include "entity/entity_local_player.h" // THE Beta blockExists gate, for mp_play
#include "game/mp_play_script.h"
#include "game/mp_pause_menu_script.h"
#include "game/split_recovery_script.h"
#include "game/split_offer_piston_script.h"
#include "game/save_reload_script.h"
#include "game/vehicle_smoke_script.h"
#include "game/split_portal_quit_script.h"
// lworld_ready_timeout_admits() for the heartbeat's lw_to_admits field. The
// SP half of this screen's contract lives in screen.h (already reachable);
// the REMOTE gate and its cumulative counter live here -- see that header's
// top comment for why the two are split.
#include "game/gui/screen_load_world.h"
#include "carve_observer.h"
#include "game/menu_autopilot.h"
#include "platform/input_glyph.h" // input_prompt_frame: per-viewport HUD prompts
#include "platform/input_owner.h"
#include "game/near_ring_keepup.h"
#include "game/mp_reconnect_soak.h"
#include "game/state_export_emit.h"
#include "game/agent_input.h"
#include "network/client_interface.h"
#include "network/client_frame_worlds.h"
#include "network/far_lod_mode.h"
#include "network/mp_session.h"
#include "network/entity_archive.h" // chunk_entities_preserve_index_fallbacks (heartbeat save_idx_fb)
#include "network/region_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_transfer.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_remote.h"
#include "platform/audio.h"
#include "platform/audio_music.h"
#include "platform/audio_deferred.h"
#include "platform/displaylist.h"
#include "platform/watchdog.h"
#include "profile.h"
#include "particle.h"
#include "platform/gfx.h"
#include "platform/hw_telemetry.h"
#include "platform/guard_scan.h"
#include "platform/input.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "platform/g9_probe.h"
#include "platform/g9_capture.h"
#include "platform/service_observer.h"
#include "platform/g9_worst_tick.h"
#include "platform/texture.h"
#include "platform/beta_import.h"
#include "platform/cw_canary.h"
#include "world.h"

/* cw_canary.h's SD-trace kill switch: declared extern there so every TU that
 * includes the header (client_interface.c, server_remote*.c, hw_telemetry.c,
 * net_socket.c) can check it without a new translation unit; defined once,
 * here. Stays permanently false in production -- nothing in this build ever
 * sets it true, so CW_SD_TRACE_OFF() is always a no-op and every guarded
 * tracer runs exactly as if the switch did not exist. */
#ifdef PLATFORM_WII
volatile bool g_cw_sd_trace_off = false;
#endif

#include "entity/mob_motion.h" // mob-move cost counters (Fable S2 F2)
#ifdef PLATFORM_WII
#include <ogc/lwp_watchdog.h> // gettime / ticks_to_nanosecs (as platform/time.c)
#endif
/* The monotonic nanosecond clock the mob-move cost counters take (injected so
 * the entity stratum keeps no platform edge). Wii: the timebase; PC: the
 * monotonic clock. Integer throughout -- a float seconds clock loses the
 * microsecond deltas after a few minutes of uptime. */
static uint64_t cw_mob_cost_now_ns(void) {
#ifdef PLATFORM_WII
	return ticks_to_nanosecs(gettime());
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
#endif
}

#include "cNBT/nbt.h"
#include "cglm/cglm.h"
#include "lighting.h"
#include "stack.h"
#include "light_brightness.h"
#include "lodepng/lodepng.h"

#ifndef CW_BUILD_ID
#define CW_BUILD_ID "unstamped"
#endif

#endif /* ordinary main includes */

#include "game/game_state.h"
#include "game/client_session.h"
#include "entity/entity.h"
#include "entity/entity_local_player.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "network/vehicle_smoke_probe.h"
#include "platform/input_owner.h"

/* F1: the exact owner dispatch used by the catch-up loop below. The focused
 * guard includes this TU with only these functions enabled and links the real
 * entity physics/transport; no fixture substitutes for this dispatch. */
void main_client_host_tick_dispatch(const unsigned* tick_slots, unsigned tick_n,
	bool host_entities_active, bool guest_only_active, int guest_tick_slot) {
	/* Keep the retained host corpse completely inert while still
	 * ticking the admitted survivor and the shared world. */
	bool (*host_tick)(struct entity*)=NULL;
	if(gstate.players[0].local_player
	   &&(!host_entities_active
		  ||!client_session_host_gameplay_active(&gstate.players[0])
		  || clin_transfer_actor_held(0))) {
		host_tick=gstate.players[0].local_player->tick_client;
		gstate.players[0].local_player->tick_client=NULL;
	}
	/* V6 F1 round 3 (ASTRA-F1-DESIGN-RULING 1, 10): a LOCAL
	 * host tick first reserves its movement credit; without
	 * one this host tick is SUSPENDED (its physics and its
	 * sample do not happen, nothing is owed) while the world,
	 * the guest, render, menus and both RPC pumps go on. */
	bool host_credit=false;
	if(!gstate.remote_session && !host_tick
	   && gstate.players[0].local_player
	   && gstate.players[0].local_player->tick_client) {
		host_credit=clin_local_tick_begin(0);
		if(!host_credit) {
			host_tick=gstate.players[0].local_player->tick_client;
			gstate.players[0].local_player->tick_client=NULL;
		}
	}
	for(unsigned ti = 0; ti < tick_n; ti++) {
		if(guest_only_active&&tick_slots[ti]!=(unsigned)guest_tick_slot)continue;
		struct clin_view_world ts;
		clin_world_slot_enter(tick_slots[ti], &ts);
		/* vehicle-smoke-1004 (CONTRACT-v1.1 Q3): count the
		 * REAL dispatch of the ride target inside this pass;
		 * dormant (one relaxed load) unless armed. */
		struct vsp_dispatch_tap vs_tap;
		bool vs_tap_on = vsp_armed();
		if(vs_tap_on)
			vsp_client_dispatch_begin(&vs_tap, ts.entities,
									  (int)ts.slot);
#ifndef CAVEWII_F1_DISPATCH_ONLY
		SERVICE_OBSERVER(if(gstate.players[0].local_player && gstate.players[0].local_player->tick_client && gstate.players[0].local_player->data.local_player.capture_input) {
			struct input_device d; if(input_player_device(0, &d)) service_observer_device(0, 2, &d);
		});
#endif
		entities_client_tick(ts.entities);
		if(vs_tap_on)
			vsp_client_dispatch_end(&vs_tap, ts.entities);
		clin_world_view_leave(&ts);
	}
	if(host_tick&&gstate.players[0].local_player)
		gstate.players[0].local_player->tick_client=host_tick;
	/* ...and the executed host tick publishes exactly one
	 * immutable sample of its final pose (each catch-up tick
	 * its own), the remote path's wire convention. */
	if(host_credit) {
		if(gstate.players[0].local_player && clin_local_tick_physics_ran(0)) {
			struct camera sample = gstate.players[0].camera;
			camera_attach(&sample, gstate.players[0].local_player,
				1.0F, 0.0F);
			clin_local_tick_end(0, &sample);
		} else
			clin_local_tick_cancel(0);
	}
	/* Beta EntityClientPlayerMP.onUpdate:31-35 sends after
	 * each player update, including both ticks of a catch-up
	 * frame. Sample position at this tick's final state. */
	if(host_entities_active&&gstate.remote_session
	   && client_session_host_gameplay_active(&gstate.players[0])) {
		clin_remote_movement_tick_executed();
		struct camera sample = gstate.players[0].camera;
		camera_attach(&sample, gstate.players[0].local_player,
			1.0F, 0.0F);
		clin_remote_player_tick(&sample);
	}
}

#ifndef CAVEWII_F1_DISPATCH_ONLY
/* Set by the frame loop for each guest dispatch: whether this frame's guest
 * input is physical device input (the G9 service observer's consumer). */
static bool s_guest_observe_input;
#endif
void main_client_guest_tick_dispatch(const struct input_player_frame* guest_frame) {
	/* V6 F1 round 3: the guest's tick reserves its credit
	 * first; suspended = no walk, no sample this tick. */
	if(!clin_transfer_actor_held(1)
	   && clin_local_tick_begin(1)) {
		struct entity* gb = gstate.players[1].local_player;
		/* The guest uses the owned walk directly, so apply the same Beta
		 * blockExists boundary that the host's entity tick applies. */
		if(gb && local_player_column_present(gb->world,
			(w_coord_t)floorf(gb->pos[0]), (w_coord_t)floorf(gb->pos[2]))) {
#ifndef CAVEWII_F1_DISPATCH_ONLY
			/* Recorded at the actual consumer: credited, column present. */
			SERVICE_OBSERVER(if(s_guest_observe_input) {
				struct input_device d; if(input_player_device(1, &d)) service_observer_device(1, 2, &d);
			});
#endif
			(void)entity_local_player_walk_owned(gb, &gstate.players[1], guest_frame);
		}
		else if(gb) {
			glm_vec3_copy(gb->pos, gb->pos_old);
			glm_vec2_copy(gb->orient, gb->orient_old);
			glm_vec3_zero(gb->vel);
		}
		if(gb && clin_local_tick_physics_ran(1)) {
			struct camera gs = gstate.players[1].camera;
			gs.x = gb->pos[0];
			gs.y = gb->pos[1];
			gs.z = gb->pos[2];
			memset(&gs.ysize, 0, sizeof gs.ysize);
			clin_local_tick_end(1, &gs);
		} else
			clin_local_tick_cancel(1);
	}
}

#ifndef CAVEWII_F1_DISPATCH_ONLY
static void stats_record_max(float value, float* max_value) {
	if(value > *max_value)
		*max_value = value;
}

static uint32_t stats_ms_x10(float ms) {
	if(ms <= 0.0F)
		return 0;
	if(ms >= 429496729.0F)
		return UINT32_MAX;
	return (uint32_t)(ms * 10.0F + 0.5F);
}

/* BREAKER-#2 LAYOUT GUARD (2026-08-27, SIZED BY SWEEP) -- explicit,
 * measured mitigation, NOT a quiet hack. Hardware facts, all heartbeat-oracle proven on Broadway:
 * the held-map build (image end/__bss_end 0x80589454, __Arena1Lo 12
 * bytes above) died PRE-FAT under BOTH loaders with cw_run_args
 * unconsumed and no boot trace; a ~100-byte code delta had earlier moved
 * the death site; THIS 4KB .bss pad flips it to a clean 123-heartbeat
 * far_idle_2m PASS (staged_far_idle_2m_20260827_152217). Conclusion AT THE
 * TIME (see the 2026-09-03 MECHANISM CORRECTED note below -- this framing
 * is now known to be wrong): a boot-time boundary collision near the image
 * end. PADDING SWEEP (2026-08-27 pm, one staged far_idle_2m per size): pad
 * 0 = FAIL x2; 1024 / 2048 / 4096 / 8192 = PASS (123 heartbeats each) --
 * read AT THE TIME as "the failing layout sits within <1KB of the boundary
 * and ANY >=1KB margin escapes it" (WRONG: see below -- what a bigger pad
 * actually does is shove every later .bss object that much further from
 * the fatal address, not create "margin" at the image end). Mechanism
 * candidate examined and later RULED OUT (2026-09-03: "no loader path
 * writes below the image end"): __Arena1Lo derives from __bss_end
 * (libogc_common.ld) and libogc __CheckARGV memmoves the loader argv
 * block to Arena1Lo, 12 bytes past this image's bss end. Separate REAL
 * defect found en route (fixed 2026-08-27): the stage
 * server wrote its argv command line into the START of .bss, which crt0
 * zeroes -- the staged argv path never delivered args; the SD-file
 * fallback always did the real work. It now stages args just past the
 * image end, HBC/wiiload-style (tools/wii_stage_server dol_runprep.c).
 * This guard stays at 4KB (4x the measured minimum).
 *
 * LTO-DROP FINDING (2026-09-03): a peer session diffed build/CaveWii_v2.elf.map
 * across two release builds and found this pad in NEITHER -- no 4096-byte .bss
 * entry, 0 `cw_layout_probe_pad` symbols in either linked .elf. `-flto=auto`
 * (the release CFLAGS) apparently treats a `static` array as dead once GCC's
 * LTO partitioning can see nothing in the TU calls it, and drops it at LINK
 * time despite `__attribute__((used))` -- `used` only protects the object
 * through COMPILATION; it does not force the linker to retain an unreferenced
 * static past LTO's whole-program analysis. Net effect: the 08-27 guard above
 * was a NO-OP in every release .dol since it landed -- build 738a2fa died
 * pre-FAT with zero trace because its image end sat 36B below the same
 * boundary this pad was supposed to push clear of.
 *
 * FIX (2026-09-03): make the pad un-droppable three ways at once, so any one
 * of them holding is enough: (1) external linkage instead of `static`, so it
 * is a real symbol the linker can `--undefined=` for; (2) a dedicated
 * `.bss.zz_cw_layout_guard` section -- the "zz_" prefix is a placement bid,
 * not a guarantee: libogc's linker script (and default `.bss.*` orphan
 * handling) does not document an alphabetical sort of `.bss.*` input
 * sections, and there is no `--sort-section=name` in the Wii link recipe, so
 * verify actual placement (map file + nm) per build rather than trust the
 * name; (3) a real load-bearing store through a volatile pointer, early in
 * main() before FAT (see below) -- the previous `used`-only static had no
 * such store precisely because it existed only for the linker's benefit, and
 * that is the exact case LTO now proves it will not honor. The Wii LDFLAGS
 * (both release and dev, Makefile LDFLAGS) additionally carry
 * `-Wl,--undefined=cw_layout_probe_pad` so the linker sees a hard external
 * reference to the symbol independent of any C-level call site.
 * build-verify.sh gates on `powerpc-eabi-nm` finding this symbol in the
 * linked .elf (both build-wii/scripts copies) -- a build that silently drops
 * the guard again fails loudly instead of shipping unprotected.
 *
 * MECHANISM CORRECTED (2026-09-03, later the same day, a same-directory
 * linker-map diff -- C:/tmp/cw-mapdiff, maps archived under
 * C:/tmp/cw-mapdiff-out/<sha>/): every claim above that calls this pad a
 * "margin" or a boundary escape is WRONG, and every option below that
 * proposed placing a margin AT THE IMAGE END is a non-fix for the same
 * reason. This symbol sits MID-.bss, not at the image end -- inserting it
 * pushes EVERY .bss object linked after it up by exactly its own size
 * (+0x1000 for the 4KB pad in the current layout), so libogc's own EXI/SD
 * buffers (`_ioResponse`, `s_buf`) land 0x1000 bytes higher than they
 * otherwise would and now occupy the address window that used to be the
 * death site -- a RELOCATION of what lives where, not a margin added past
 * the end of anything. Confirmed by a control run: 738a2fa, rebuilt
 * byte-for-byte except for the build directory (path-string drift shifted
 * bss end by +0x20, to 0x805ec3fc), BOOTED and passed far_idle_2m (122 hb)
 * with the SAME code and NO guard pad at all -- so the death is
 * exact-address specific (0x805ec3dc dies, 0x805ec3fc boots, a 32-byte
 * window), not a "within 1KB of a boundary" rule, and no image-end margin
 * of any size is what separates the two outcomes. Also confirmed: NO
 * loader path (HBC, wiiload, __CheckARGV, runprep) writes below the image
 * end on either build, so the "loader argv memmove" mechanism theory two
 * paragraphs up is RULED OUT, and the peer session's "runprep argv >= 4KB
 * margin" proposal, and the DISABLED `__Arena1Lo=__bss_end+0x1000` option
 * below, would NOT address this failure mode -- both still frame the fix
 * as image-end margin. THE ACTUAL WRITER of whatever lands on
 * 0x805ec3dc and kills boot is still UNKNOWN; this pad is a working,
 * measured, gated MITIGATION by accidental relocation, not a diagnosed
 * fix, and must not be described as one. Next step (not done here): plant
 * canary bytes at the dying address range across builds and check every
 * boot-time absolute-address writer (framebuffer/arena setup, VI, IPC
 * buffers, stage-server residue) for whichever one lands there.
 *
 * NOT changed here (out of scope for this fix, owned by the peer session):
 * tools/wii_stage_server's dol_runprep.c stages args past the image end
 * (the 08-27 real defect fix above), which is a correctness fix in its own
 * right (the staged-argv path could not deliver args before it), but per
 * the MECHANISM CORRECTED note above it is NOT "the definitive fix" for
 * the boot death -- no loader path ever wrote below the image end, so
 * placing argv further past it cannot be what the pad's relocation effect
 * is standing in for. DOCUMENTED, DISABLED, AND NOW KNOWN INEFFECTIVE FOR
 * THIS FAILURE (not applied): libogc_common.ld derives `__Arena1Lo` from
 * `__bss_end`, so `-Wl,--defsym,__Arena1Lo=__bss_end+0x1000` would move
 * where the loader's argv block goes, but since no loader path writes
 * below the image end in the first place, it would not touch the actual
 * (still unknown) writer either; left as a comment-only historical note,
 * not a candidate fix. */
volatile uint8_t cw_layout_probe_pad[4096]
	__attribute__((used, section(".bss.zz_cw_layout_guard")));

/* When the boot "Loading sounds..." notice was presented (lane C); the later
   boot-trace marks report their distance from it. */
static ptime_t s_boot_notice_time;
static bool s_boot_notice_time_set;

static void hw_boot_trace_write(const char* phase,
								const struct hw_run_config* hw_run,
								const char* detail, bool reset) {
#ifdef PLATFORM_WII
	if(!hw_run || !hw_run->enabled)
		return;
	FILE* f = fopen("sd:/cw_boot_trace.txt", reset ? "w" : "a");
	if(!f)
		return;
	fprintf(f,
			"phase=%s build=%s run_id=%s profile=%s tel_host=%s "
			"tel_port=%d pull=%d netmask=%s",
			phase ? phase : "", CW_BUILD_ID, hw_run->run_id,
			hw_run->profile_name, hw_run->telemetry_host,
			hw_run->telemetry_port, hw_run->telemetry_pull ? 1 : 0,
			hw_run->telemetry_netmask);
	if(detail && detail[0])
		fprintf(f, " detail=%s", detail);
	fputc('\n', f);
	fflush(f);
	fclose(f);
#else
	(void)phase;
	(void)hw_run;
	(void)detail;
	(void)reset;
#endif
}

/* The unattended named-save proof is useful on Wii only when its transitions
 * survive a reboot, but the same lines on stderr are the PC validation oracle.
 * Keep each caller edge-triggered: a failed save must not fill the SD while it
 * waits for a person to retry from the still-live pause screen. */
static void hw_save_quit_trace(const char* phase,
							   const struct hw_run_config* hw_run,
							   const char* detail) {
	hw_boot_trace_write(phase, hw_run, detail, false);
	fprintf(stderr, "[named-save-quit] phase=%s detail=%s\n",
			phase ? phase : "", detail ? detail : "");
}

// OUTER telemetry DHCP retry wiring (Batch E item 5, 2026-08-23). Supplies
// hw_telemetry_retry_bringup (hw_telemetry.h) with the real attempt (the
// existing, unmodified hw_telemetry_init) and the real sleep (thread_msleep
// plus a console progress line, so the wait is visibly "retrying", never a
// silent black screen) -- see the call site below for the full design note.
struct hw_tel_retry_ctx {
	const struct hw_telemetry_config* cfg;
	int attempt; // 1-based; bumped by the attempt fn just before each call
};

static bool hw_run_tel_attempt(void* user) {
	struct hw_tel_retry_ctx* ctx = user;
	ctx->attempt++;
	return hw_telemetry_init(ctx->cfg);
}

static void hw_run_tel_sleep(uint32_t ms, void* user) {
	struct hw_tel_retry_ctx* ctx = user;
	fprintf(stderr,
			"[hw-run] telemetry DHCP attempt %d failed, retrying in %ums "
			"(outer budget ~30s; cadence mirrors the stage server's "
			"sleep(2) -- tools/wii_stage_server/source/main.c:671)...\n",
			ctx->attempt, (unsigned)ms);
	thread_msleep(ms);
}

static struct hw_frame_evidence s_release_frames;
static uint32_t s_guard_scan_us[2];
// Copied once from hw_run.cw_set_n right after config load / override
// application (main()) -- emit_hw_heartbeat below has no access to the
// main()-local hw_run struct, same reason s_release_frames/s_guard_scan_us
// are file-scope statics rather than parameters. Read-only after that copy.
static uint32_t s_cw_set_n_applied;
/* Split render reach: the single-camera fog/far plane, restored when the
 * second camera goes away. */
static float s_single_render_distance, s_single_fog_distance;
static bool s_split_fog_applied;
/* GUEST_WALK_UNSTICK (harness only): quarter turns added to the forced guest
 * heading each time the walker stops making progress (stg32: 146 blocks, then
 * 316 s still against something the 12-tick hop cannot clear). */
static int s_guest_heading_extra_deg;

static void split_render_reach_apply(bool two_cameras) {
	int reach = world_split_render_reach();
	bool want = two_cameras && reach > 0;
	if(want == s_split_fog_applied)
		return;
	s_split_fog_applied = want;
	if(want) {
		gstate.config.fog_distance = reach * 16.0F;
		gstate.config.render_distance = reach * 16.0F + 32.0F;
	} else {
		gstate.config.fog_distance = s_single_fog_distance;
		gstate.config.render_distance = s_single_render_distance;
	}
}
static struct hw_anchor_evidence s_anchor
	= {.carve_load_min = -1, .carve_run_min = -1};
static struct server_local* s_anchor_server;
static const struct server_local* s_completion_heartbeat_server;
static ptime_t s_anchor_started;

/* Defined in platform/hw_telemetry.c (C1); not exported by its header. */
void hw_telemetry_split_guest_eye(float y, int head_block, int air);

/* split_dims_6m: the dimension each split view shows. View 0 is player 0's
 * (its world-bound payload when B2 binds one, else gstate.world); view 1 is
 * the guest's own payload when bound, else it shares view 0's world. With no
 * admitted guest body there is no guest view (-128). Client-side truth: what
 * each viewport actually renders, not what the server intends. */
#define SPLIT_DIMS_NO_VIEW (-128)
/* The guest walk's portal step (split_dims_6m). The profile lights the
 * production "debug.portal_demo" SHOWCASE pane -- a real ignited frame five
 * blocks ahead (+z) of the standing host; the demo also builds its UNLIT
 * obsidian frame around the host first and skips only that frame's
 * ignition, so the host never travels -- and the guest walks into it:
 *  "pane_wait": walk released until a portal block is seen on the guest's
 *    walk line (the server lights the pane ~5 s after load; measured on PC,
 *    readiness came first and the guest walked past the empty spot), bounded
 *    by SPLIT_DIMS_PANE_WAIT_MS;
 *  "seek": walking to it;
 *  "hold": the body stands in a portal block, walk released (Beta's 81-tick
 *    ramp needs the contact held), bounded by SPLIT_DIMS_HOLD_MS;
 *  "across": the two views diverged; the walk resumes on the other side;
 *  "timeout": no pane or no crossing in time; the walk resumes in the
 *    Overworld and the collector records dims_not_active.
 * Pure driver input. */
/* Host switch first (splitdims-hostswitch-0927, "debug.portal_demo" =
 * "split_dims"; the server half is server_local_portal_demo_split_step):
 *  "clear": the guest (admitted inside the frame around the host) walks +z
 *    until its body is past the frame plane; the server ignites the host
 *    frame only once the guest is clear, so only the host travels;
 *  "host_trip": guest walk released while the host step below runs: the
 *    host transfers to the Nether (A2-T COMMIT 1), the demo re-arms its
 *    arrival-portal cooldown after ~60 s, it transfers back (COMMIT 2), the
 *    vacated Nether (world slot 1) drains to zero resident columns and its
 *    CRPC_WORLD_RELEASE is applied here. Then "pane_wait" as above: the
 *    guest crosses, so the run ENDS with the players in different worlds. */
#define SPLIT_DIMS_PANE_WAIT_MS 30000u
#define SPLIT_DIMS_HOLD_MS 30000u
#define SPLIT_DIMS_PANE_LOOKAHEAD 8
#define SPLIT_DIMS_CLEAR_MS 20000u
#define SPLIT_DIMS_HOST_WAIT_MS 60000u
#define SPLIT_DIMS_HOST_AWAY_MS 150000u
#define SPLIT_DIMS_HOST_BACK_MS 60000u
enum split_dims_phase { SPLIT_DIMS_PANE_WAIT, SPLIT_DIMS_SEEK, SPLIT_DIMS_HOLD,
						SPLIT_DIMS_ACROSS, SPLIT_DIMS_TIMEOUT, SPLIT_DIMS_CLEAR,
						SPLIT_DIMS_HOST_TRIP };
enum split_dims_host_phase { SPLIT_DIMS_HOST_WAIT, SPLIT_DIMS_HOST_AWAY,
							 SPLIT_DIMS_HOST_BACK, SPLIT_DIMS_HOST_DONE,
							 SPLIT_DIMS_HOST_TIMEOUT };
static enum split_dims_host_phase s_split_dims_host_phase;
static ptime_t s_split_dims_host_started;
static unsigned s_split_dims_host_geom_mask;
static struct autopilot_portal_return_anchor s_split_dims_return_anchor;
static bool s_split_dims_return_anchor_valid, s_split_dims_return_walking;
static bool s_split_dims_return_entered, s_split_dims_return_invalid;
static int s_split_dims_return_slot;
static uint32_t s_split_dims_return_generation, s_split_dims_return_tick;
static ptime_t s_split_dims_return_started, s_split_dims_return_progress;
static float s_split_dims_return_best;
static unsigned s_split_dims_patrol_endpoint, s_split_dims_patrol_legs;
static bool s_split_dims_slot0_walk_started, s_split_dims_slot0_walk_done;
static ptime_t s_split_dims_slot0_walk_start;
static float s_split_dims_slot0_x, s_split_dims_slot0_z;
static float s_split_dims_frame_z;
static enum split_dims_phase s_split_dims_phase;
/* hw_run is main()'s local: set once from it after the run is parsed. */
static bool s_split_dims_armed;
static bool s_split_dims_started;
static ptime_t s_split_dims_phase_started;
static const char* split_dims_phase_name(void) {
	switch(s_split_dims_phase) {
		case SPLIT_DIMS_SEEK: return "seek";
		case SPLIT_DIMS_HOLD: return "hold";
		case SPLIT_DIMS_ACROSS: return "across";
		case SPLIT_DIMS_TIMEOUT: return "timeout";
		case SPLIT_DIMS_CLEAR: return "clear";
		case SPLIT_DIMS_HOST_TRIP: return "host_trip";
		default: return "pane_wait";
	}
}
static const char* split_dims_host_phase_name(void) {
	switch(s_split_dims_host_phase) {
		case SPLIT_DIMS_HOST_AWAY: return "away";
		case SPLIT_DIMS_HOST_BACK: return "back";
		case SPLIT_DIMS_HOST_DONE: return "done";
		case SPLIT_DIMS_HOST_TIMEOUT: return "timeout";
		default: return "wait";
	}
}
/* True while the guest walk must stand still (waiting for the pane, or
 * dwelling in it). */
static bool split_dims_walk_released(void) {
	return s_split_dims_armed && (s_split_dims_phase == SPLIT_DIMS_PANE_WAIT
								  || s_split_dims_phase == SPLIT_DIMS_HOLD
								  || s_split_dims_phase == SPLIT_DIMS_HOST_TRIP);
}
static struct world* view_terrain(unsigned v);
static void split_dims_fill(struct hw_split_dims_evidence* out);
static void split_dims_enter(enum split_dims_phase phase, ptime_t now) {
	s_split_dims_phase = phase;
	s_split_dims_phase_started = now;
}
static void split_dims_host_enter(enum split_dims_host_phase phase,
	ptime_t now) {
	s_split_dims_host_phase = phase;
	s_split_dims_host_started = now;
	if(phase == SPLIT_DIMS_HOST_AWAY)
	{
		extern uint32_t g_srv_sim_ticks;
		s_split_dims_host_geom_mask = 0;
		s_split_dims_patrol_endpoint = s_split_dims_patrol_legs = 0;
		s_split_dims_return_anchor_valid = false;
		s_split_dims_return_walking = false;
		s_split_dims_return_entered = false;
		s_split_dims_return_invalid = false;
		s_split_dims_return_slot = -1;
		s_split_dims_return_tick = __atomic_load_n(&g_srv_sim_ticks,
			__ATOMIC_RELAXED);
	}
}
/* Read-only native proof diagnosis. Four bounded samples describe the real
 * client body and its owning terrain; no position, velocity, input or world
 * state changes. The server's separate rearm log provides the tick boundary. */
static void split_dims_host_geometry_log(unsigned checkpoint_ms) {
	const struct entity* body = gstate.players[0].local_player;
	struct autopilot_portal_geometry geom;
	if(!autopilot_portal_geometry_sample(body, &geom)) {
		fprintf(stderr, "[split_dims_geom] at_ms=%u body unavailable\n",
			checkpoint_ms);
		return;
	}
	int slot = clin_world_view_slot(0);
	const struct client_world_context* ctx = slot >= 0
		? client_ctx_registry_current(clin_world_contexts(), (unsigned)slot)
		: NULL;
	struct world* viewed = view_terrain(0);
	fprintf(stderr, "[split_dims_geom] at_ms=%u slot=%d gen=%u dim=%d "
		"body=(%.3f,%.3f,%.3f) camera=(%.3f,%.3f,%.3f) "
		"body_box_x=(%.3f,%.3f) body_box_z=(%.3f,%.3f) "
		"feet_y=%d axis=%c collision=%d owner_view=%d\n",
		checkpoint_ms, slot, ctx ? ctx->server_generation : 0,
		viewed ? (int)viewed->dimension : -128,
		body->pos[0], body->pos[1], body->pos[2],
		gstate.players[0].camera.x, gstate.players[0].camera.y,
		gstate.players[0].camera.z,
		body->pos[0] - 0.3F, body->pos[0] + 0.3F,
		body->pos[2] - 0.3F, body->pos[2] + 0.3F,
		geom.feet_y, geom.portal_axis, geom.body_collision,
		body->world == viewed);
	for(unsigned level = 0; level < 3; level++)
		fprintf(stderr, "[split_dims_geom] at_ms=%u y=%d xcenter=%d zcenter=%d "
			"rows=%s/%s/%s/%s/%s\n", checkpoint_ms,
			geom.feet_y + (int)level - 1, geom.x, geom.z,
			geom.cells[level][0], geom.cells[level][1], geom.cells[level][2],
			geom.cells[level][3], geom.cells[level][4]);
}
/* The profile's return is ordinary controller-equivalent walking into the
 * original pane. It never changes the server cooldown, position or terrain.
 * Every attempt is bounded by the actual server-tick rearm and a checked path. */
static bool split_dims_host_return_step(ptime_t now) {
	if(s_split_dims_return_invalid)
		goto unsafe;
	struct entity* body = gstate.players[0].local_player;
	int slot = clin_world_view_slot(0);
	const struct client_world_context* ctx = slot >= 0
		? client_ctx_registry_current(clin_world_contexts(), (unsigned)slot)
		: NULL;
	if(!body || !ctx || body->world != view_terrain(0)) {
		if(!s_split_dims_return_anchor_valid
		   && time_diff_ms(s_split_dims_host_started, now) < 5000)
			return true; /* post-COMMIT view and body may still be installing */
		goto unsafe;
	}
	if(body->health <= 0
	   || !client_session_host_gameplay_active(&gstate.players[0]))
		goto unsafe;
	if(!s_split_dims_return_anchor_valid) {
		if(!autopilot_portal_return_anchor(body, &s_split_dims_return_anchor)) {
			if(time_diff_ms(s_split_dims_host_started, now) < 5000)
				return true; /* allow the first installed arrival columns */
			goto unsafe;
		}
		s_split_dims_return_anchor_valid = true;
		s_split_dims_return_slot = slot;
		s_split_dims_return_generation = ctx->server_generation;
	} else if(slot != s_split_dims_return_slot
		  || ctx->server_generation != s_split_dims_return_generation
		  || body->world != s_split_dims_return_anchor.world)
		goto unsafe;
	extern uint32_t g_srv_sim_ticks;
	uint32_t ticks = __atomic_load_n(&g_srv_sim_ticks, __ATOMIC_RELAXED);
	/* Sixty full crossings plus initial alignment. Forty yielded only
	 * 328/319 admitted dm on R9/R10 Wii tapes. Allow real margin above
	 * 320 dm for endpoint tolerance, sampling and per-RPC dm rounding.
	 * Keep the same safe corridor, physics, RPCs and 1200-tick deadline. */
	if(s_split_dims_patrol_legs < 61) {
		if(autopilot_portal_return_rearmed(s_split_dims_return_tick, ticks))
			goto unsafe;
		unsigned end = s_split_dims_patrol_endpoint;
		float dx = body->pos[0] - (s_split_dims_return_anchor.x[end] + 0.5F);
		float dz = body->pos[2] - (s_split_dims_return_anchor.z[end] + 0.5F);
		if(dx * dx + dz * dz < 0.01F) {
			s_split_dims_patrol_legs++;
			s_split_dims_patrol_endpoint ^= 1;
		}
		if(s_split_dims_patrol_legs < 61) {
			s_split_dims_return_walking = true;
			autopilot_script_portal_patrol(&s_split_dims_return_anchor,
				s_split_dims_patrol_endpoint);
			return true;
		}
		autopilot_script_portal_walk(false, NULL);
		s_split_dims_return_walking = false;
	}
	if(!autopilot_portal_return_rearmed(s_split_dims_return_tick, ticks))
		return true;
	struct autopilot_portal_return_step route;
	if(!autopilot_portal_return_step(body, &s_split_dims_return_anchor,
									 &route))
		goto unsafe;
	if(s_split_dims_return_entered) {
		if(!route.inside)
			goto unsafe; /* blast/velocity escaped during the dwell */
		return true;
	}
	if(route.inside) {
		if(s_split_dims_return_walking)
			autopilot_script_portal_walk(false, NULL);
		s_split_dims_return_walking = false;
		s_split_dims_return_entered = true;
		fprintf(stderr, "[split_dims] host re-entered original Nether pane "
			"by normal input: awaiting COMMIT2\n");
		return true;
	}
	float dx = route.target_x - body->pos[0];
	float dz = route.target_z - body->pos[2];
	float distance = sqrtf(dx * dx + dz * dz);
	if(!s_split_dims_return_walking) {
		s_split_dims_return_started = now;
		s_split_dims_return_progress = now;
		s_split_dims_return_best = distance;
		s_split_dims_return_walking = true;
		autopilot_script_portal_walk(true, &s_split_dims_return_anchor);
		fprintf(stderr, "[split_dims] host return input after %u server ticks: "
			"body=(%.3f,%.3f) pane=(%.1f,%.1f)\n",
			ticks - s_split_dims_return_tick, body->pos[0], body->pos[2],
			route.target_x, route.target_z);
	} else if(distance < s_split_dims_return_best - 0.1F) {
		s_split_dims_return_best = distance;
		s_split_dims_return_progress = now;
	}
	if(time_diff_ms(s_split_dims_return_started, now) > 15000
	   || time_diff_ms(s_split_dims_return_progress, now) > 3000)
		goto unsafe;
	return true;
unsafe:
	if(s_split_dims_return_walking)
		autopilot_script_portal_walk(false, NULL);
	s_split_dims_return_walking = false;
	fprintf(stderr, "[split_dims] host return input refused: dead, changed "
		"world/generation, unsafe corridor or no progress\n");
	return false;
}
/* Client RPCs may change the view between host-trip's previous frame and the
 * next physics tick. Revoke direct input before autopilot_tick can use a new
 * generation with the old arrival anchor. The ordinary host-step logs FAIL. */
static void split_dims_host_return_pre_tick(void) {
	if(!s_split_dims_return_walking)
		return;
	int slot = clin_world_view_slot(0);
	const struct client_world_context* ctx = slot >= 0
		? client_ctx_registry_current(clin_world_contexts(), (unsigned)slot)
		: NULL;
	struct entity* body = gstate.players[0].local_player;
	if(body && body->health > 0 && ctx
	   && slot == s_split_dims_return_slot
	   && ctx->server_generation == s_split_dims_return_generation
	   && body->world == s_split_dims_return_anchor.world)
		return;
	autopilot_script_portal_walk(false, NULL);
	s_split_dims_return_walking = false;
	s_split_dims_return_invalid = true;
}
/* The host switch, stepped once per frame during "host_trip". The transition
 * reads published commit/view/release evidence; its bounded proof-only return
 * input walks the real host back to the original pane after 1200 server ticks. */
static bool split_dims_host_step(ptime_t now,
								 const struct hw_split_dims_evidence* d) {
	const int32_t in_phase = time_diff_ms(s_split_dims_host_started, now);
	switch(s_split_dims_host_phase) {
		case SPLIT_DIMS_HOST_WAIT:
			if(d->host_commits >= 1 && d->dim_v0 == WORLD_DIM_NETHER) {
				split_dims_host_enter(SPLIT_DIMS_HOST_AWAY, now);
				fprintf(stderr, "[split_dims] host in the Nether (commit %u, "
								"view slot %d): dwelling\n",
						d->host_commits, d->wslot_v0);
			} else if(in_phase >= (int32_t)SPLIT_DIMS_HOST_WAIT_MS) {
				split_dims_host_enter(SPLIT_DIMS_HOST_TIMEOUT, now);
				fprintf(stderr, "[split_dims] host never reached the Nether "
								"(commits %u): host switch timeout\n",
						d->host_commits);
			}
			return false;
		case SPLIT_DIMS_HOST_AWAY:
			{
				static const unsigned checkpoints[] = {0, 25000, 65000, 75000};
				for(unsigned i = 0; i < sizeof(checkpoints) / sizeof(checkpoints[0]); i++)
					if(!(s_split_dims_host_geom_mask & (1u << i))
					   && in_phase >= (int32_t)checkpoints[i]) {
						s_split_dims_host_geom_mask |= 1u << i;
						split_dims_host_geometry_log(checkpoints[i]);
					}
			}
			if(d->host_commits >= 2 && d->dim_v0 == WORLD_DIM_OVERWORLD) {
				if(s_split_dims_return_walking)
					autopilot_script_portal_walk(false, NULL);
				s_split_dims_return_walking = false;
				split_dims_host_enter(SPLIT_DIMS_HOST_BACK, now);
				fprintf(stderr, "[split_dims] host back in the Overworld "
								"(commit %u): waiting for the drain\n",
						d->host_commits);
			} else if(d->host_commits >= 2) {
				/* The server committed before the client view caught up. */
				if(s_split_dims_return_walking)
					autopilot_script_portal_walk(false, NULL);
				s_split_dims_return_walking = false;
				if(in_phase >= (int32_t)SPLIT_DIMS_HOST_AWAY_MS) {
					split_dims_host_enter(SPLIT_DIMS_HOST_TIMEOUT, now);
					fprintf(stderr, "[split_dims] host COMMIT2 view "
						"never arrived: host switch timeout\n");
				}
			} else if(!split_dims_host_return_step(now)
				  || in_phase >= (int32_t)SPLIT_DIMS_HOST_AWAY_MS) {
				if(s_split_dims_return_walking)
					autopilot_script_portal_walk(false, NULL);
				s_split_dims_return_walking = false;
				split_dims_host_enter(SPLIT_DIMS_HOST_TIMEOUT, now);
				fprintf(stderr, "[split_dims] host never came back (commits "
								"%u): host switch timeout\n", d->host_commits);
			}
			return false;
		case SPLIT_DIMS_HOST_BACK:
			if(d->slot1_cols == 0 && d->rel_rx >= 1) {
				split_dims_host_enter(SPLIT_DIMS_HOST_DONE, now);
				fprintf(stderr, "[split_dims] vacated world drained and "
								"released (slot %d, %u applied): guest next\n",
						d->rel_slot, d->rel_rx);
				return true;
			}
			if(in_phase >= (int32_t)SPLIT_DIMS_HOST_BACK_MS) {
				split_dims_host_enter(SPLIT_DIMS_HOST_TIMEOUT, now);
				fprintf(stderr, "[split_dims] no drain/release (slot1 %d "
								"columns, %u applied): host switch timeout\n",
						d->slot1_cols, d->rel_rx);
				return true;
			}
			return false;
		default:
			return true;
	}
}
/* Once per frame, before the guest walk inputs are built. */
static void split_dims_portal_step(ptime_t now) {
	if(!s_split_dims_armed)
		return;
	struct entity* gb = gstate.players[1].local_player;
	if(s_split_dims_phase == SPLIT_DIMS_ACROSS && !s_split_dims_slot0_walk_done) {
		struct entity* hb = gstate.players[0].local_player;
		if(!s_split_dims_slot0_walk_started && hb) {
			s_split_dims_slot0_walk_started = true;
			s_split_dims_slot0_walk_start = now;
			s_split_dims_slot0_x = hb->pos[0];
			s_split_dims_slot0_z = hb->pos[2];
		}
		bool arrived = hb && hypotf(hb->pos[0] - s_split_dims_slot0_x,
			hb->pos[2] - s_split_dims_slot0_z) >= 40.0F;
		bool valid = hb && hb->health > 0 && clin_world_view_slot(0) == 0
			&& !gstate.world_switch_pending;
		bool expired = s_split_dims_slot0_walk_started
			&& time_diff_ms(s_split_dims_slot0_walk_start, now) >= 30000;
		autopilot_script_walk(valid && !arrived && !expired, 180);
		if(arrived || expired || !valid) {
			s_split_dims_slot0_walk_done = true;
			if(!arrived) split_dims_enter(SPLIT_DIMS_TIMEOUT, now);
		}
	}
	if(!gb || s_split_dims_phase == SPLIT_DIMS_ACROSS
	   || s_split_dims_phase == SPLIT_DIMS_TIMEOUT)
		return;
	struct entity* hb0 = gstate.players[0].local_player;
	if(!s_split_dims_started) {
		if(!hb0)
			return;
		s_split_dims_started = true;
		s_split_dims_frame_z = floorf(hb0->pos[2]);
		split_dims_enter(SPLIT_DIMS_CLEAR, now);
	}
	struct hw_split_dims_evidence d;
	split_dims_fill(&d);
	if(s_split_dims_phase == SPLIT_DIMS_CLEAR) {
		/* body (half-width 0.3) past the frame plane, with a half-block
		 * margin; the lit showcase pane is 5 blocks ahead */
		if(gb->pos[2] - 0.3F >= s_split_dims_frame_z + 1.5F
		   || time_diff_ms(s_split_dims_phase_started, now)
			   >= (int32_t)SPLIT_DIMS_CLEAR_MS) {
			split_dims_enter(SPLIT_DIMS_HOST_TRIP, now);
			split_dims_host_enter(SPLIT_DIMS_HOST_WAIT, now);
			fprintf(stderr, "[split_dims] guest at z %.2f (frame %.0f): host "
							"switch\n", gb->pos[2], s_split_dims_frame_z);
		}
		return;
	}
	if(s_split_dims_phase == SPLIT_DIMS_HOST_TRIP) {
		if(split_dims_host_step(now, &d))
			split_dims_enter(SPLIT_DIMS_PANE_WAIT, now);
		return;
	}
	if(d.dims_active) {
		split_dims_enter(SPLIT_DIMS_ACROSS, now);
		fprintf(stderr, "[split_dims] views diverged (v0 %d, v1 %d): walking "
						"on\n", d.dim_v0, d.dim_v1);
		return;
	}
	const int32_t in_phase = time_diff_ms(s_split_dims_phase_started, now);
	if(s_split_dims_phase == SPLIT_DIMS_HOLD) {
		if(in_phase >= (int32_t)SPLIT_DIMS_HOLD_MS) {
			split_dims_enter(SPLIT_DIMS_TIMEOUT, now);
			fprintf(stderr, "[split_dims] no crossing after %u ms in the "
							"portal: dims_not_active, walking on\n",
					SPLIT_DIMS_HOLD_MS);
		}
		return;
	}
	struct clin_world_payload p;
	struct world* w = clin_world_payload_of_view(1, &p) && p.terrain
		? p.terrain : &gstate.world;
	const w_coord_t bx = (w_coord_t)floorf(gb->pos[0]);
	const w_coord_t by = (w_coord_t)floorf(gb->pos[1] - 1.62F + 0.01F);
	const w_coord_t bz = (w_coord_t)floorf(gb->pos[2]);
	if(s_split_dims_phase == SPLIT_DIMS_PANE_WAIT) {
		for(int dz = 0; dz <= SPLIT_DIMS_PANE_LOOKAHEAD; dz++)
			if(world_get_block(w, bx, by, bz + dz).type == BLOCK_PORTAL) {
				split_dims_enter(SPLIT_DIMS_SEEK, now);
				fprintf(stderr, "[split_dims] pane seen %d block(s) ahead of "
								"the guest: walking to it\n", dz);
				return;
			}
		if(in_phase >= (int32_t)SPLIT_DIMS_PANE_WAIT_MS) {
			split_dims_enter(SPLIT_DIMS_TIMEOUT, now);
			fprintf(stderr, "[split_dims] no portal pane within %d blocks after "
							"%u ms: dims_not_active, walking on\n",
					SPLIT_DIMS_PANE_LOOKAHEAD, SPLIT_DIMS_PANE_WAIT_MS);
		}
		return;
	}
	if(world_get_block(w, bx, by, bz).type == BLOCK_PORTAL) {
		split_dims_enter(SPLIT_DIMS_HOLD, now);
		fprintf(stderr, "[split_dims] guest in the portal at (%.2f, %.2f, "
						"%.2f): holding\n", gb->pos[0], gb->pos[1], gb->pos[2]);
	}
}
/* colcap-1006 (ASTRA-CC-R3 item 2): the physical column pool + A2-T transfer
 * budget, shared by the split_dims block and the anchor pool block. `budget`
 * receives the whole snapshot so the dims caller can read its transfer
 * fields from the SAME sample. Cost per heartbeat (1 Hz): one mem2_lock
 * acquire for the pool counters (+ <= 16 popcounts on the anchor path) plus
 * the budget snapshot copy and two relaxed display-list counter loads. */
static void pool_column_fields(struct hw_pool_budget_evidence* out,
							   const struct mem2_pool_stat* column_pool) {
	out->column_pool_used = column_pool->used;
	out->column_pool_peak = column_pool->peak;
	out->column_pool_slabs = column_pool->slabs;
	out->column_pool_overflow = column_pool->overflow;
	out->column_pool_carve_fail = column_pool->carve_fail;
}
static void budget_fields_fill(struct hw_pool_budget_evidence* out,
							   struct server_transfer_budget_snapshot* budget) {
	out->budget_valid = server_transfer_budget_snapshot_get(budget);
#ifdef PLATFORM_WII
	out->budget_wii = true;
#else
	out->budget_wii = false;
#endif
	out->budget_resident = budget->resident;
	out->budget_live_backings = budget->live_backings;
	out->budget_reserved = budget->reserved;
	out->budget_prepeak = budget->prebudget_peak;
	out->budget_peak = budget->enforced_peak;
	out->budget_waits = budget->cap_waits;
	out->budget_phase = budget->budget_phase;
}
static void pool_budget_fill(struct hw_pool_budget_evidence* out,
							 struct server_transfer_budget_snapshot* budget) {
	struct mem2_pool_stat column_pool;
	mem2_pool_column_stats(&column_pool);
	pool_column_fields(out, &column_pool);
	budget_fields_fill(out, budget);
}

/* colcap-1006 (ASTRA-CC-R3 item 2): every anchor run carries the pool/budget
 * evidence without arming the split_dims scenario (s_split_dims_armed stays
 * profile-exact). When the dims block is present it already holds `pb`, so
 * the snapshot is not taken twice. */
/* colcap-1006 (ASTRA-CC-R4 point 7): display-list bytes by owner for the
 * anchor heartbeat (non-dims). Main thread, read-only. Cost: one pass over
 * each distinct view world's sections and GPU-busy list per heartbeat. */
static void anchor_dl_owner_fill(struct hw_anchor_pool_evidence* out) {
	const uint32_t now = chunk_mesher_clock_ms();
	struct world* w0 = view_terrain(0);
	struct world* w1 = view_terrain(1);
	struct world_dl_census c0, c1 = {0};
	world_dl_census(w0, now, &c0);
	if(w1 && w1 != w0)
		world_dl_census(w1, now, &c1);
	uint32_t done_b, done_n;
	chunk_mesher_completed_stats(&done_b, &done_n);
	const uint32_t v[HW_DLO_FIELDS] = {
		c0.installed_bytes, c0.installed_lists,
		c1.installed_bytes, c1.installed_lists,
		c0.pinned_bytes + c1.pinned_bytes, c0.pinned_lists + c1.pinned_lists,
		done_b, done_n,
		c0.age_bytes[0] + c1.age_bytes[0], c0.age_bytes[1] + c1.age_bytes[1],
		c0.age_bytes[2] + c1.age_bytes[2],
		c0.own_alloc_lists + c1.own_alloc_lists,
	};
	memcpy(out->dlo, v, sizeof v);
	out->dlo_valid = true;
}

_Static_assert(HW_POOL_SLAB_REPORT_MAX == MEM2_POOL_MAX_SLABS,
	"heartbeat per-slab occupancy must cover every possible slab");
static void anchor_pool_fill(struct hw_anchor_pool_evidence* out,
							 struct hw_split_dims_evidence* dims) {
	memset(out, 0, sizeof *out);
	out->enabled = s_anchor.enabled;
	if(!out->enabled)
		return;
	if(!s_split_dims_armed) {
		struct server_transfer_budget_snapshot budget = {0};
		budget_fields_fill(&out->pb, &budget);
	}
	/* item 3: per-slab occupancy sampled WITH the column counters (one
	 * lock), and those counters are what both blocks report -- so the
	 * collector can require sum(cps) == column_pool_used exactly. The dims
	 * block's budget/transfer fields keep their own single snapshot. */
	/* R4 point 9: deliver any pool event the telemetry ring refused earlier
	 * BEFORE the snapshot, so the stamped seq is as fresh as the wire. */
	mem2_pool_column_events_flush();
	struct mem2_pool_stat column_pool;
	out->slab_count = mem2_pool_column_occupancy(&column_pool, out->slab_used,
		HW_POOL_SLAB_REPORT_MAX, &out->event_seq, &out->events_lost);
	out->shrinks = column_pool.shrinks;
	out->shrink_fail = column_pool.shrink_fail;
	pool_column_fields(&out->pb, &column_pool);
	if(s_split_dims_armed && dims->enabled)
		pool_column_fields(&dims->pb, &column_pool);
	displaylist_storage_bytes(&out->dl_lists, &out->dl_bytes);
	if(!s_split_dims_armed) {
		anchor_dl_owner_fill(out);
		/* R5 item 3: allocator CPU/lock cost since the last heartbeat. */
		mem2_cost_take(out->pac);
		out->pac_valid = true;
	}
}
_Static_assert(HW_PAC_FIELDS == MEM2_COST_FIELDS,
	"heartbeat pac must carry every mem2 cost field");

static void split_dims_fill(struct hw_split_dims_evidence* out) {
	out->enabled = s_split_dims_armed;
	if(!out->enabled)
		return;
	struct clin_world_payload p;
	out->dim_v0 = clin_world_payload_of_view(0, &p) && p.terrain
		? p.terrain->dimension : gstate.world.dimension;
	const struct client_session* guest = &gstate.players[1];
	const bool guest_live = guest->local_player && guest->local_gameplay_ready;
	out->views_live = guest_live ? 2u : 1u;
	if(!guest_live)
		out->dim_v1 = SPLIT_DIMS_NO_VIEW;
	else
		out->dim_v1 = clin_world_payload_of_view(1, &p) && p.terrain
			? p.terrain->dimension : out->dim_v0;
	out->dims_active = guest_live && out->dim_v1 != out->dim_v0;
	out->phase = split_dims_phase_name();
	extern int32_t g_srv_host_dim, g_srv_guest_dim;
	out->srv_dim_host = __atomic_load_n(&g_srv_host_dim, __ATOMIC_RELAXED);
	const int32_t guest_dim = __atomic_load_n(&g_srv_guest_dim, __ATOMIC_RELAXED);
	out->srv_dim_guest = guest_dim == INT32_MIN ? SPLIT_DIMS_NO_VIEW : guest_dim;
	out->host_phase = split_dims_host_phase_name();
	extern uint32_t g_srv_host_commits, g_srv_release_sent;
	extern int32_t g_srv_slot1_cols;
	/* The server publishes the drained count before enqueueing RELEASE.
	 * Observe the applied release first so this sample cannot pair its new
	 * count with a column count read before that publication. */
	out->rel_rx = clin_world_release_applied(&out->rel_slot);
	out->host_commits = __atomic_load_n(&g_srv_host_commits, __ATOMIC_RELAXED);
	out->slot1_cols = __atomic_load_n(&g_srv_slot1_cols, __ATOMIC_ACQUIRE);
	out->rel_sent = __atomic_load_n(&g_srv_release_sent, __ATOMIC_RELAXED);
	out->wslot_v0 = clin_world_view_slot(0);
	out->wslot_v1 = guest_live ? clin_world_view_slot(1) : -1;
	struct server_transfer_budget_snapshot budget = {0};
	pool_budget_fill(&out->pb, &budget);
	out->transfer_request = budget.request_id;
	out->guest_commits = budget.guest_commits;
	out->host_rb_id = budget.rollback_id[0];
	out->guest_rb_id = budget.rollback_id[1];
	out->host_rb_reason = budget.rollback_reason[0];
	out->guest_rb_reason = budget.rollback_reason[1];
	out->transfer_actor = budget.actor_slot;
	out->transfer_state = budget.transfer_state;
	out->transfer_held = budget.held;
	out->transfer_source_draining = budget.source_draining;
	out->transfer_restoring = budget.restoring;
}

/* A2-W: the terrain view `v` shows (player 0's world for view 0, the guest's
 * for view 1). Flag OFF, or nothing bound: gstate.world, exactly as before. */
static struct world* view_terrain(unsigned v) {
	struct clin_world_payload p;
	return clin_world_payload_of_view(v, &p) && p.terrain ? p.terrain
														  : &gstate.world;
}

static int hw_mem2_free_kb_probe(void) {
	watchdog_main_census_phase(3);
	int free_kb = mem2_stat_carve_free_kb();
	watchdog_main_census_phase(0);
	return free_kb;
}

/* Temporary freeze evidence, sampled before the heartbeat's heap census.
 * Server authority and saved thread context are separate bounded snapshots;
 * neither the saved PC nor this record claims to observe after main stops. */
#ifdef PLATFORM_WII
static const struct thread* s_freeze_server_thread;
static char* freeze_u32(char* p, uint32_t value) {
	char digits[10]; unsigned n = 0;
	do { digits[n++] = (char)('0' + value % 10); value /= 10; } while(value);
	while(n) *p++ = digits[--n];
	return p;
}
static void emit_watchdog_contexts(void) {
	struct thread_saved_context contexts[3];
	static uint32_t sequence;
	uint32_t seq = ++sequence;
	uint64_t begin = gettime();
	(void)thread_main_context_snapshot(&contexts[0]);
	(void)thread_saved_context_snapshot(s_freeze_server_thread, &contexts[1]);
	(void)hw_telemetry_sender_context_snapshot(&contexts[2]);
	uint32_t sample_us = (uint32_t)ticks_to_microsecs(gettime() - begin);
	const char* roles[] = {"main", "server", "sender"};
	for(unsigned role = 0; role < 3; role++) {
		const struct thread_saved_context* ctx = &contexts[role];
		char line[1024];
		_Static_assert(THREAD_SAVED_CONTEXT_CALLERS * 11 + 500 < sizeof(line),
			"watchdog context record bounded size");
		/* Fixed literals and <=10 decimal digits per scalar; no libc formatter,
		 * allocation, lock or I/O on this high-priority diagnostic path. */
		char* p = line;
#define FREEZE_LITERAL(s) do { memcpy(p, (s), sizeof(s) - 1); p += sizeof(s) - 1; } while(0)
		FREEZE_LITERAL("{\"event\":\"thread_diag\",\"v\":1,\"seq\":");
		p = freeze_u32(p, seq);
		FREEZE_LITERAL(",\"role\":\"");
		size_t role_len = strlen(roles[role]);
		memcpy(p, roles[role], role_len); p += role_len;
		FREEZE_LITERAL("\",\"data\":[");
		uint32_t data[] = {ctx->valid, ctx->pc, ctx->lr, ctx->sp,
			ctx->state, ctx->suspend, ctx->priority, ctx->wait_queue,
			ctx->wait_token, watchdog_phase_main_current(),
			watchdog_phase_srv_current(), sample_us};
		for(unsigned i = 0; i < sizeof(data) / sizeof(data[0]); i++) {
			if(i) *p++ = ',';
			p = freeze_u32(p, data[i]);
		}
		FREEZE_LITERAL("],\"callers\":[");
		for(unsigned i = 0; i < ctx->caller_count; i++) {
			if(i) *p++ = ',';
			p = freeze_u32(p, ctx->callers[i]);
		}
		FREEZE_LITERAL("]}"); *p = '\0';
#undef FREEZE_LITERAL
		(void)hw_telemetry_emit_watchdog_start(line);
	}
}
#endif

static void emit_freeze_observer(const struct thread* server_thread, uint32_t tick) {
	struct server_portal_diag_snapshot auth = {0};
	bool auth_valid = server_local_portal_diag_get(&auth);
	struct thread_saved_context ctx;
	ptime_t begin = time_get();
	(void)thread_saved_context_snapshot(server_thread, &ctx);
	unsigned sample_us = (unsigned)(time_diff_s(begin, time_get()) * 1000000.0F);
	char line[2048];
	_Static_assert(SPD_FIELD_COUNT * 11 + THREAD_SAVED_CONTEXT_CALLERS * 11
		+ 512 < sizeof(line), "freeze observer worst-case decimal size");
	int n = snprintf(line, sizeof(line),
		"{\"event\":\"freeze_diag\",\"build_id\":\"%s\",\"v\":1,\"tick\":%u,"
		"\"stage\":%u,\"av\":%u,\"cv\":%u,\"pc\":%u,\"lr\":%u,\"sp\":%u,"
		"\"state\":%u,\"suspend\":%u,\"prio\":%u,\"queue\":%u,\"token\":%u,"
		"\"sample_us\":%u,\"callers\":[",
		CW_BUILD_ID, (unsigned)tick, (unsigned)server_local_diag_stage(),
		(unsigned)auth_valid, (unsigned)ctx.valid, (unsigned)ctx.pc,
		(unsigned)ctx.lr, (unsigned)ctx.sp, (unsigned)ctx.state,
		(unsigned)ctx.suspend, (unsigned)ctx.priority,
		(unsigned)ctx.wait_queue, (unsigned)ctx.wait_token, sample_us);
	if(n < 0 || (size_t)n >= sizeof(line)) return;
	size_t off = (size_t)n;
	for(unsigned i = 0; i < ctx.caller_count; i++) {
		n = snprintf(line + off, sizeof(line) - off, "%s%u",
			i ? "," : "", (unsigned)ctx.callers[i]);
		if(n < 0 || (size_t)n >= sizeof(line) - off) return;
		off += (size_t)n;
	}
	n = snprintf(line + off, sizeof(line) - off, "],\"auth\":[");
	if(n < 0 || (size_t)n >= sizeof(line) - off) return;
	off += (size_t)n;
	for(unsigned i = 0; i < SPD_FIELD_COUNT; i++) {
		n = snprintf(line + off, sizeof(line) - off, "%s%u",
			i ? "," : "", (unsigned)auth.word[i]);
		if(n < 0 || (size_t)n >= sizeof(line) - off) return;
		off += (size_t)n;
	}
	n = snprintf(line + off, sizeof(line) - off, "]}");
	if(n < 0 || (size_t)n >= sizeof(line) - off) return;
#ifdef PLATFORM_WII
	/* Reuse the existing no-wake, try-once ring producer. Despite its name,
	 * it accepts any observer JSON; unlike the normal path it takes no
	 * sender-wake mutex. Format and publish only after IRQs are restored. */
	(void)hw_telemetry_emit_watchdog_start(line);
#else
	(void)hw_telemetry_emit_observer(line);
#endif
}

/* O20: tentative definitions; the vehicle hooks below own these. */
static char s_vehicle_status[96];
static bool s_vehicle_done_pending;
/* colcap-1006 (ASTRA-CC-R5 item 2b): the pool self-test's "scene settled"
 * predicate -- in game, near ring fully streamed; mesh cleanliness gates
 * individual hash frames separately.
 * The self-test additionally requires the camera unchanged for its whole
 * settle window. Only evaluated under pool_selftest_3m. */
/* R5 item 2b: a rendered chunk's CONTENT witness -- exactly the
 * (CHUNK_SIZE + 2)^3 cells the mesher gathers (type, metadata, sky, torch;
 * bitfield padding never hashed). Registered only under pool_selftest_3m and
 * called only on armed frames, from the render pass (main thread). */
static uint64_t pool_selftest_chunk_content(const void* chunk) {
	static struct block_data cells[(CHUNK_SIZE + 2) * (CHUNK_SIZE + 2)
								   * (CHUNK_SIZE + 2)];
	chunk_mesher_gather((struct chunk*)chunk, cells);
	uint64_t h = DL_SCENE_FNV64_INIT;
	for(size_t k = 0; k < sizeof cells / sizeof *cells; k++) {
		uint8_t v[4] = {cells[k].type, cells[k].metadata, cells[k].sky_light,
						cells[k].torch_light};
		h = dl_scene_fnv64(h, v, sizeof v);
	}
	return h;
}

/* colcap-1006 (ASTRA-CC-R5 item 3): split_regrow_8m's anchor route on the
 * readiness clock -- separated [0, 90 s), reunited [90, 180 s), separated
 * again from 180 s to the end of the 360 measured seconds. Each transition
 * is announced once ("anchor_cycle") with the column-pool slab count at
 * that instant, so the collector can tie drain and regrowth to the route. */
#define ANCHOR_CYCLE_REUNITE_MS 90000u
#define ANCHOR_CYCLE_RESEPARATE_MS 180000u
static int s_anchor_cycle_phase;
static void anchor_cycle_step(struct server_local* s, int sep, uint32_t ms) {
	static const char* const names[3] = {"separated", "reunited", "reseparated"};
	int want = ms >= ANCHOR_CYCLE_RESEPARATE_MS ? 2
		: ms >= ANCHOR_CYCLE_REUNITE_MS ? 1 : 0;
	if(want == s_anchor_cycle_phase)
		return;
	int offset = want == 1 ? 0 : sep;
	bool ok = server_local_set_anchor_offset(s, offset);
	world_set_anchor_offset(offset);
	s_anchor_cycle_phase = want;
	struct mem2_pool_stat pool;
	mem2_pool_column_stats(&pool);
	char line[192];
	snprintf(line, sizeof line,
		"{\"event\":\"anchor_cycle\",\"phase\":\"%s\",\"anchor_ms\":%u,"
		"\"offset\":%d,\"ok\":%s,\"slabs\":%u,\"used\":%u}",
		names[want], (unsigned)ms, offset, ok ? "true" : "false",
		(unsigned)pool.slabs, (unsigned)pool.used);
	(void)hw_telemetry_emit_observer_retry(line);
	/* Normal streaming retains only 2 * 9^2 columns (six slabs). A warm
	 * voxel restream at the new anchor also needs the real population-support
	 * rings. Exercise that production RESET/ACK transaction, after emitting
	 * the phase boundary; never forge finished_loading or hold dummy columns. */
	if(want == 2 && ok)
		clin_begin_live_stream_recovery();
}

static bool pool_selftest_scene_settled(bool* clean) {
	*clean = false;
	if(!gstate.world_loaded || gstate.world_load_failed
	   || gstate.players[0].current_screen != &screen_ingame)
		return false;
	w_coord_t cx
		= WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.x));
	w_coord_t cz
		= WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.z));
	int missing = 0, mesh_pending = 0;
	world_near_ring_status(view_terrain(0), cx, cz,
		world_server_view_radius() ? world_server_view_radius() : MAX_VIEW_DISTANCE,
		&missing, &mesh_pending);
	*clean = mesh_pending == 0 && chunk_mesher_pending() == 0;
	return missing == 0;
}

static bool s_perf_guard_observe;
/* SPLIT-SPILL (2026-10-14): overlay fallback + carve-spill attribution at
 * heartbeat cadence (record format and rationale: hw_telemetry.h
 * struct hw_overlay_spill). Kept out of line so its 192 B buffer is a
 * separate frame, sequential with -- never nested inside -- the
 * hw_telemetry_emit_heartbeat line frame. Main thread only. */
static __attribute__((noinline)) void emit_overlay_spill(uint32_t tick,
	uint32_t fallback_live, uint32_t fallback_total) {
	static struct hw_overlay_spill sent; // last ADMITTED values; zero = nothing to say
	struct hw_overlay_spill cur = {
		.tick = tick,
		.fallback_live = fallback_live,
		.fallback_total = fallback_total,
		.spill = (uint32_t)mem2_stat_carve_spill(),
		.last_spill_bytes = mem2_stat_last_spill_bytes(),
	};
	if(!hw_telemetry_overlay_spill_due(&cur, &sent)) return;
	char line[HW_TELEMETRY_OVERLAY_SPILL_LINE_SIZE];
	if(!hw_telemetry_format_overlay_spill(line, sizeof line, &cur)) return;
	/* Refusal is a deferral (not tel_drop); retried next heartbeat. */
	if(hw_telemetry_emit_observer_retry(line)) sent = cur;
}
static void emit_hw_heartbeat(uint32_t tick, int far_view_distance, int far_effective,
							  uint32_t frame_counter) {
	if(!hw_telemetry_active())
		return;
	static struct hw_telemetry_fps_window fps_window;
	if(tick == 0)
		fps_window = (struct hw_telemetry_fps_window){0};
	/* Sample count and the device's monotonic clock at the same point, before
	 * heartbeat work. The window includes every intervening frame and stall. */
	struct hw_telemetry_fps_sample fps_sample = hw_telemetry_sample_fps(
		&fps_window, frame_counter, cw_mob_cost_now_ns() / UINT64_C(1000));
	/* C1 -> B1 gy hand-off: sample the guest body ONCE per heartbeat for every
	 * profile (split_dims_6m's collector rejects a guest heartbeat without a
	 * real, finite gy). This runs after the frame's owned guest walk (the walk
	 * refreshes client oxygen, entity_local_player.c), so the sample is the
	 * body as it stands now: join, every Y change and a respawned body are all
	 * picked up here, and a departed guest reports the neutral sample. Policy:
	 * source/game/split_guest_eye.h; pinned by test_b1_guest_eye_heartbeat. */
	{
		struct entity* body = gstate.players[1].local_player;
		int head_block = 0;
		if(body)
			head_block = world_get_block(view_terrain(1), floorf(body->pos[0]),
										 floorf(body->pos[1]),
										 floorf(body->pos[2]))
							 .type;
		struct split_guest_eye_sample eye = split_guest_eye_heartbeat(
			body ? body->pos : NULL, head_block,
			body ? gstate.players[1].oxygen : 0);
		hw_telemetry_split_guest_eye(eye.y, eye.head_block, eye.air);
	}
	extern int g_srv_tick_rt_us, g_srv_tick_wt_us;
	extern uint32_t g_srv_sim_ticks;
	// Far-skin generation cost + backlog, sampled once per SERVER tick by the
	// FAR_SEND_BUDGET loop (server_local.c) -- same single-tick-sample
	// convention as g_srv_tick_rt_us/g_srv_tick_wt_us above.
	extern int g_srv_far_gen_us, g_srv_far_gen_cols, g_srv_far_gen_backlog;
	// Server-loop wedge instrumentation (2026-09-08, lane/carve-observer-boot):
	// written by the server thread with relaxed atomics (server_local.c);
	// read here the same way, unlike the g_srv_far_gen_* trio above (plain
	// loads, an existing convention this lane leaves untouched).
	extern uint32_t g_srv_loop_it, g_srv_ticks_pend, g_srv_lean_it;
	extern int g_srv_elapsed_ms, g_srv_partial_m, g_srv_sleep_ms,
		g_srv_far_us_max;
	// The two neighbour-notify dispatch totals (server_world.c + blocks.c),
	// declared locally like the g_srv_tick_* externs above; their sum is the
	// heartbeat "ndisp" scenario signal (redstone_clock_2m gate).
	uint32_t server_world_notify_dispatch_total(void);
	uint32_t blocks_notify_neighbours_dispatch_total(void);
#ifdef PLATFORM_WII
	extern uint32_t g_displaylist_malformed, g_displaylist_badcall;
	watchdog_main_census_phase(1);
	struct mallinfo mi = mallinfo();
	watchdog_main_census_phase(0);
#endif
	struct server_local_save_migrate_metrics save_migrate;
	server_local_save_migrate_metrics_read(&save_migrate);
	// Far-ring keep-up probe (2026-09-03 reach-sweep gate 4): far_missing is a
	// scan (world_far_missing_count, world.c) throttled to THIS function's own
	// <=1 Hz caller-side cadence (the main loop only calls emit_hw_heartbeat
	// once/sec) -- see that function's doc comment for the cost bound and why
	// this is a documented throttled scan, not a receive/evict-only counter.
	// far_ring_keepup_state (source/game/far_ring_keepup.h) then folds it into
	// the running missing_max/settle_ms the reach-sweep gate reads. Reset
	// whenever no world is loaded so a later (re)load starts clean instead of
	// carrying a stale crossing/max from a previous session.
	static struct far_ring_keepup_state s_far_keepup;
	static ptime_t s_far_keepup_epoch;
	static bool s_far_keepup_epoch_set = false;
	int far_missing_now = 0;
	if(gstate.world_loaded) {
		w_coord_t center_cx
			= WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.x));
		w_coord_t center_cz
			= WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.z));
		far_missing_now = world_far_missing_count(
			view_terrain(0), center_cx, center_cz, MAX_VIEW_DISTANCE,
			far_view_distance);
		/* Split render reach: with two cameras and the reach inside the near
		 * radius the far annulus is invisible and the server no longer
		 * streams it (server_local far_invisible) -- a policy exclusion, not
		 * missing terrain. Without this the readiness latch (`playable`
		 * below needs far_missing == 0) never fired: 42/256 sat at 48
		 * missing far columns for 19 minutes with every anchor counter at 0. */
		if(s_split_fog_applied
		   && world_split_render_reach()
			  <= (world_server_view_radius() ? world_server_view_radius() : MAX_VIEW_DISTANCE))
			far_missing_now = 0;
		if(!s_far_keepup_epoch_set) {
			s_far_keepup_epoch = time_get();
			s_far_keepup_epoch_set = true;
		}
		uint32_t now_ms = (uint32_t)(
			time_diff_s(s_far_keepup_epoch, time_get()) * 1000.0F);
		far_ring_keepup_update(&s_far_keepup, center_cx, center_cz,
							   far_missing_now, now_ms);
	} else {
		far_ring_keepup_reset(&s_far_keepup);
		s_far_keepup_epoch_set = false;
	}
	// H-2/H-3 (2026-09-03 maintainer ruling): near-ring mirror of the far-ring
	// block above -- same <=1 Hz heartbeat cadence, same epoch/reset
	// discipline, world_near_ring_status instead of world_far_missing_count
	// (see near_ring_keepup.h's doc comment for why the KEEP-UP state is fed
	// the COMBINED missing+mesh-pending count).
	//
	// 2026-09-04: the two terms are now ALSO reported separately on the
	// heartbeat (near_missing = columns not yet streamed; near_mesh_pending =
	// resident lod==0 sub-chunks still awaiting a (re)build). They fail for
	// completely different reasons -- one is the server/streaming path, the
	// other the mesher -- and the far_walk_6m run that root-caused the band-2
	// stall could not tell them apart from the single summed field, which is
	// exactly the diagnosis the next run must not have to guess at.
	static struct near_ring_keepup_state s_near_keepup;
	static ptime_t s_near_keepup_epoch;
	static bool s_near_keepup_epoch_set = false;
	int near_cols_missing_now = 0;
	int near_mesh_pending_now = 0;
	int near_missing_now = 0;
	if(gstate.world_loaded) {
		w_coord_t center_cx
			= WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.x));
		w_coord_t center_cz
			= WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.z));
		int missing = 0, mesh_pending = 0;
		world_near_ring_status(view_terrain(0), center_cx, center_cz,
							   world_server_view_radius() ? world_server_view_radius() : MAX_VIEW_DISTANCE,
							   &missing, &mesh_pending);
		near_cols_missing_now = missing;
		near_mesh_pending_now = mesh_pending;
		near_missing_now = missing + mesh_pending;
		if(!s_near_keepup_epoch_set) {
			s_near_keepup_epoch = time_get();
			s_near_keepup_epoch_set = true;
		}
		uint32_t now_ms = (uint32_t)(
			time_diff_s(s_near_keepup_epoch, time_get()) * 1000.0F);
		near_ring_keepup_update(&s_near_keepup, center_cx, center_cz,
								near_missing_now, now_ms);
	} else {
		near_ring_keepup_reset(&s_near_keepup);
		s_near_keepup_epoch_set = false;
	}
	// P2-c (2026-09-03 round-2 root-cause doc, "#3 Instrumentation gap"): the
	// mesher worker's own build wall-clock, previously F3-only (screen_
	// ingame.c). Read on the main thread; chunk_mesher_build_ms's own
	// comment covers the cross-thread __atomic float read.
	float hb_ms_mesh_worker = 0.0F, hb_ms_mesh_worker_max = 0.0F;
	chunk_mesher_build_ms(&hb_ms_mesh_worker, &hb_ms_mesh_worker_max);
	struct hw_telemetry_heartbeat hb = {
		.frame_evidence = s_release_frames,
		.far_storage = world_get_far_storage(),
		.server_time = __atomic_load_n(&g_srv_sim_ticks, __ATOMIC_RELAXED),
		.build_id = CW_BUILD_ID,
		.tick = tick,
		.fps = fps_sample.fps,
		.fps_window_present = true,
		.gpu_ms = gstate.stats.dt_gpu * 1000.0F,
		.vsync_ms = gstate.stats.dt_vsync * 1000.0F,
		.ms_bfs = gstate.stats.ms_bfs,
		.ms_light = gstate.stats.ms_light,
		.ms_lod = gstate.stats.ms_lod,
		.ms_build = gstate.stats.ms_build,
		.ms_recv = gstate.stats.ms_recv,
		.ms_render = gstate.stats.ms_render,
		.ms_tick = gstate.stats.ms_tick,
		.ms_bfs_max = gstate.stats.ms_bfs_max,
		.ms_light_max = gstate.stats.ms_light_max,
		.ms_lod_max = gstate.stats.ms_lod_max,
		.ms_build_max = gstate.stats.ms_build_max,
		.chunks_rendered = (int)gstate.stats.chunks_rendered,
		.mesh_pending = chunk_mesher_pending(),
		// CR-08: each of these four is a plain uint32_t written on another
		// thread (server or mesher worker; see their declarations), so the
		// read goes through __atomic_load_n here rather than a bare load.
		.mesh_stale = __atomic_load_n(&mesh_stale_discards, __ATOMIC_RELAXED),
		.mesh_trunc = __atomic_load_n(&g_mesh_truncations, __ATOMIC_RELAXED),
		.evict_stuck = __atomic_load_n(&g_evict_stuck, __ATOMIC_RELAXED),
		.near_evict_defer =
			__atomic_load_n(&g_near_evict_deferred, __ATOMIC_RELAXED),
		.srv_rt_us = g_srv_tick_rt_us,
		.srv_wt_us = g_srv_tick_wt_us,
		.srv_gen_us = g_srv_far_gen_us,
		.srv_gen_cols = g_srv_far_gen_cols,
		.srv_gen_backlog = g_srv_far_gen_backlog,
		.srv_loop_it = __atomic_load_n(&g_srv_loop_it, __ATOMIC_RELAXED),
		.srv_lean = __atomic_load_n(&g_srv_lean_it, __ATOMIC_RELAXED),
		.srv_elapsed_ms = __atomic_load_n(&g_srv_elapsed_ms, __ATOMIC_RELAXED),
		.srv_ticks_pend = __atomic_load_n(&g_srv_ticks_pend, __ATOMIC_RELAXED),
		.srv_partial_m = __atomic_load_n(&g_srv_partial_m, __ATOMIC_RELAXED),
		.srv_sleep_ms = __atomic_load_n(&g_srv_sleep_ms, __ATOMIC_RELAXED),
		.srv_far_us_max =
			__atomic_load_n(&g_srv_far_us_max, __ATOMIC_RELAXED),
		.heap_used_kb = -1,
		.heap_free_kb = -1,
		.heap_blocks = -1,
		.heap_top_kb = -1,
		.wpad_probe0 = -99, // PC sentinel; Wii overwrites below
		.wkspace_free = -1,
		.wkspace_total = -1,
		.wkspace_used = -1,
		.wkspace_free_blocks = -1,
		.mem1_free_kb = -1,
		.mem2_free_kb = -1,
		// T-c: mem2_stat_carve_free_kb is portable (defined on both
		// platforms, like mem2_stat_carve_spill/_oom just below) and already
		// returns -1 on PC, so it is called directly here rather than
		// defaulted-then-overwritten under the PLATFORM_WII block below.
		.mem2_carve_free_kb = hw_mem2_free_kb_probe(),
		.mem2_spill = (uint32_t)mem2_stat_carve_spill(),
		.mem2_oom = (uint32_t)mem2_stat_oom(),
		.item_nbt_exhaustions = item_nbt_exhaustions(),
		.dlbad = 0,
		.dlcall = 0,
		.dlcalls = 0, // overwritten below, both platforms
		.tris = 0,
		.x = gstate.players[0].camera.x,
		.y = gstate.players[0].camera.y,
		.z = gstate.players[0].camera.z,
		.rx_deg = glm_deg(gstate.players[0].camera.rx),
		.ry_deg = glm_deg(gstate.players[0].camera.ry),
		.guest = gstate.players[1].local_player != NULL,
		.gx = gstate.players[1].camera.x,
		.gz = gstate.players[1].camera.z,
		.near_radius = world_lod_near_radius(),
		.far_view_distance = far_view_distance,
		// Ring-active width: far_vd is clamped to >= MAX_VIEW_DISTANCE even
		// when the ring is OFF, so the raw value cannot express "off"; the
		// collector's far-profile gate needs this explicit annulus width.
		// ... from the server's EFFECTIVE far distance: the split policy lowers it
		// to the split radius while two anchors are active (ring OFF -> width 0);
		// single player passes the configured value, so this is unchanged there.
		.far_ring_width = far_effective > MAX_VIEW_DISTANCE
			? far_effective - MAX_VIEW_DISTANCE
			: 0,
		.far_missing = far_missing_now,
		.far_missing_max = far_ring_keepup_missing_max(&s_far_keepup),
		.far_settle_ms = far_ring_keepup_settle_ms_max(&s_far_keepup),
		.cflood = lighting_client_flood_max_pops(),
		.rskip = world_remote_unchanged_skips(),
		.rns = world_remote_relight_no_slots(),
		.clq = {lighting_client_ceiling_parks(), lighting_client_oom_parks(),
			world_light_repair_pending(&gstate.world), world_light_repairs_done()},
		.notify_dispatch_total = server_world_notify_dispatch_total()
			+ blocks_notify_neighbours_dispatch_total(),
		.wd_main = watchdog_phase_main_current(),
		.wd_srv = watchdog_phase_srv_current(),
		.wd_worker = watchdog_phase_worker_current(),
		.wd_hb_gap_max_ms = watchdog_heartbeat_gap_max_ms(),
		.world_loaded = gstate.world_loaded,
		.menu_ap_step = menu_autopilot_step(),
		.menu_ap_max_step = menu_autopilot_max_step(),
		.menu_ap_sign_reached = menu_autopilot_sign_reached(),
		// push-0921 A11 (review A9-3): what the no-pointer leg actually DID,
		// not just how far its step counter got. See hw_telemetry.h for the
		// bit table; the masks are 7-bit and np_flags 7-bit by construction,
		// which two _Static_asserts in source/game/menu_autopilot.c keep true
		// (they are there, not "below" in this file -- corrected, A16-8).
		.np_entered = (uint8_t)menu_autopilot_np_entered(),
		.np_skipped = (uint8_t)menu_autopilot_np_skipped(),
		.np_focus = (uint8_t)menu_autopilot_np_focus_reached(),
		.np_flags = (uint8_t)menu_autopilot_np_flags(),
		.np_rows = menu_autopilot_np_rows(),
		.save_migrate_state = save_migrate.state,
		.save_migrate_ms = save_migrate.elapsed_ms,
		.save_migrate_regions = save_migrate.regions_scanned,
		.save_migrate_chunks = save_migrate.chunks_scanned,
		.save_migrate_signs = save_migrate.signs_rewritten,
		.save_migrate_expected = save_migrate.signs_expected,
		.save_migrate_verified = save_migrate.signs_verified,
		.save_migrate_chests_rewritten = save_migrate.chests_rewritten,
		.save_migrate_chests_expected = save_migrate.chests_expected,
		.save_migrate_chests_verified = save_migrate.chests_verified,
		.save_migrate_spawners_synthesized = save_migrate.spawners_synthesized,
		.save_migrate_spawners_expected = save_migrate.spawners_expected,
		.save_migrate_spawners_verified = save_migrate.spawners_verified,
		// lava-aabb-0924/H1: chunk-save failure visibility (relaxed-atomic
		// getters over counters the saving thread writes; region_archive.h).
		.save_fail_n = region_save_failure_count(),
		.save_fail_stage = (uint32_t)region_save_failure_stage(),
		.save_fail_sub = region_save_failure_sub(),
		.save_skip_n = region_save_skip_unchanged_count(),
		.save_idx_fb = chunk_entities_preserve_index_fallbacks(),
		.wl_pending = gstate.world_switch_pending ? 1u : 0u,
		.wl_failed = gstate.world_load_failed ? 1u : 0u,
		// S3 streaming-voice proof (debug.music_force lever): numeric-only
		// query surface, audio_music.h -- see hw_telemetry.h's field
		// comments for the value meanings.
		.music_key = audio_music_telemetry_key(),
		.music_ms = audio_music_telemetry_ms(),
		.music_underruns = audio_music_underrun_count(),
		.music_starts = audio_music_probe_starts(),
		.music_rejects = audio_music_probe_rejects(),
		.music_dur = audio_music_probe_dur(),
		.mp_reconnect_cycles = (uint32_t)mp_reconnect_soak_cycles(),
		.mp_reconnect_target = (uint32_t)mp_reconnect_soak_target(),
		.mp_reconnect_wedge = mp_reconnect_soak_wedged(),
		// menus_dwell hardware-risk probe (see hw_telemetry.h's field
		// comment for why gstate.config_creative, not server_local.
		// creative_mode).
		.creative = gstate.config_creative,
		.ms_mesh_worker = hb_ms_mesh_worker,
		.ms_mesh_worker_max = hb_ms_mesh_worker_max,
		.mem2_lock_wait = mem2_stat_lock_wait(),
		// T-a: the send-fail state machine's own session view (0/0 on PC,
		// where hw_telemetry never has a real socket to fail). tel_quiesced
		// mirrors hw_telemetry_quiesced() -- true once no further heartbeat
		// will send this session, for ANY quiesce reason.
		.tel_fail = hw_telemetry_tel_fail_count(),
		/* O20: the vehicle driver's verdict rides every later heartbeat */
		.vs_status = s_vehicle_done_pending ? s_vehicle_status : NULL,
		.tel_last_errno = hw_telemetry_tel_last_errno(),
		.tel_quiesced = hw_telemetry_quiesced(),
		// Sender self-telemetry (2026-09-07 starvation fix): filled in just
		// below via hw_telemetry_sender_stats_take(), which RESETS the
		// windowed counters -- so it must be called exactly once per
		// heartbeat, from this thread.
		// T-b: monotonic main-loop-iteration count, passed in by the caller
		// (the while(!gstate.quit) loop owns the counter; see its low-rate
		// boot-trace write for the independent post-hoc liveness channel).
		.frame_counter = frame_counter,
		// H-1 (2026-09-03 responsiveness lane): mesher worker throughput.
		.chunks_meshed = chunk_mesher_meshed_count(),
		// H-2/H-3 (2026-09-03 maintainer ruling): near-ring keep-up + band-0
		// priority instruments -- see hw_telemetry.h's field comments.
		.near_missing = near_cols_missing_now,
		.near_mesh_pending = near_mesh_pending_now,
		.near_missing_max = near_ring_keepup_missing_max(&s_near_keepup),
		.near_settle_ms = near_ring_keepup_settle_ms_max(&s_near_keepup),
		.edit_to_visible_frames = world_edit_to_visible_frames_max(&gstate.world),
		.load_screen_ms = lworld_load_screen_ms_result(),
		.player_health = gstate.players[0].local_player
			? gstate.players[0].local_player->health : -1,
		.loading_screen = gstate.players[0].current_screen == &screen_load_world,
	};
	clin_remote_movement_stats(&hb.mp_move_ticks, &hb.mp_move_samples);
	/* G9 R6 (ASTRA-G9-REVIEW-2 item 5): the server pending-light queue as
	 * the heartbeat v2 "lq" array (hw_telemetry.h fixes the order): per
	 * slot the peak allocated seed/undo/pin/frontier/checkpoint-snapshot
	 * KiB, seeds, drain us/cells and post-drain debt; the per-update and
	 * checkpoint maxima; the stop / OOM / ceiling / drop counters. */
	{
		struct server_world_light_hb lq;
		server_world_light_hb(&lq);
		const uint32_t v[HW_TELEMETRY_LQ_N] = {
			(uint32_t)(((uint64_t)lq.seed_b[0] + 1023u) / 1024u), (uint32_t)(((uint64_t)lq.seed_b[1] + 1023u) / 1024u),
			(uint32_t)(((uint64_t)lq.undo_b[0] + 1023u) / 1024u), (uint32_t)(((uint64_t)lq.undo_b[1] + 1023u) / 1024u),
			(uint32_t)(((uint64_t)lq.pin_b[0] + 1023u) / 1024u), (uint32_t)(((uint64_t)lq.pin_b[1] + 1023u) / 1024u),
			(uint32_t)(((uint64_t)lq.front_b[0] + 1023u) / 1024u), (uint32_t)(((uint64_t)lq.front_b[1] + 1023u) / 1024u),
			(uint32_t)(((uint64_t)lq.ckpt_b[0] + 1023u) / 1024u), (uint32_t)(((uint64_t)lq.ckpt_b[1] + 1023u) / 1024u),
			lq.seeds_pk[0], lq.seeds_pk[1], lq.drain_us[0], lq.drain_us[1],
			lq.drain_pops[0], lq.drain_pops[1], lq.debt[0], lq.debt[1],
			lq.tick_us, lq.ckpt_us, lq.faults, lq.ceil, lq.oom, lq.ckpt_fail,
			lighting_flood_ceiling_hits(), stack_oom_drops(),
		};
		for(int k = 0; k < HW_TELEMETRY_LQ_N; k++)
			hb.lq[k] = v[k] > 65535u ? 65535u : (uint16_t)v[k];
		/* G9 R6 v3: ONE paired winning drain per world, elapsed total,
		 * yield calls, lock waits and recovery sleeps (not CPU time). */
		struct server_world_light_drain_hb d;
		server_world_light_drain_hb_take(&d);
		/* ci-1101/37: the longest >= 10 ms drain mutex wait this window, with
		 * the contended lock named and its holder (hw1101c: 71,206 us inside
		 * the server tick's light drain). Emitted only when one happened. */
		hb.lqd_wait_present = d.wait_attrib.wait_us > 0;
		hb.lqd_wait_us = d.wait_attrib.wait_us;
		hb.lqd_wait_mutex = d.wait_attrib.mutex;
		hb.lqd_wait_holder_id = d.wait_attrib.holder_id;
		hb.lqd_wait_holder_tag = d.wait_attrib.holder_tag;
		hb.lqd_wait_stripe = d.wait_attrib.stripe;
		memcpy(hb.lqd_wait_name, d.wait_attrib.name, sizeof hb.lqd_wait_name);
		/* R5 points 9-10: the window maxima of rt/wt (every tick graded),
		 * the checkpoint split and the RPC token-pool wait of this window */
		uint32_t rt_max = 0, wt_max = 0, rpc_sum = 0, rpc_max = 0, rpc_n = 0;
		server_local_tick_window_max_take(&rt_max, &wt_max);
		clin_rpc_wait_take(&rpc_sum, &rpc_max, &rpc_n);
		const uint32_t dv[HW_TELEMETRY_LQD_N] = {
			d.win[0].slices,         d.win[1].slices,
			d.win[0].pops,		   d.win[1].pops,
			d.win[0].recovery_us,    d.win[1].recovery_us,
			d.win[0].us,		   d.win[1].us,
			d.win[0].yield_us,       d.win[1].yield_us,
			d.win[0].lock_us,        d.win[1].lock_us,
			3,                     0, /* lqd layout v3 */
			rt_max,				   wt_max,
			d.ckpt.us,			   d.ckpt.snap_us,
			d.ckpt.drain_us,	   d.ckpt.save_us,
			d.ckpt.restore_us,	   rpc_sum,
		};
		(void)rpc_max;
		(void)rpc_n; /* F3 / tests only (line budget) */
		memcpy(hb.lqd, dv, sizeof hb.lqd);
		hb.g9p_present = thread_main_pacing_snapshot(hb.g9p);
	}
#ifdef _WIN32
	if(getenv("CAVEWII_RESPAWN_STATS")) {
		struct pc_heap_stats memory = pc_heap_stats();
		fprintf(stderr, "[respawn-memory] tick=%u health=%d loaded=%d "
			"sections=%zu heap_blocks=%zu heap_bytes=%zu ws_bytes=%zu valid=%d\n",
			(unsigned)tick, hb.player_health, hb.world_loaded,
			world_loaded_chunks(&gstate.world), memory.blocks, memory.bytes,
			memory.working_set, memory.valid);
	}
#endif
	// Per-frame GP-load gauge (both platforms): counted in displaylist_render
	// (chunk.c's per-side draw loop) since it was last reset above, before the
	// world render pass this heartbeat is reporting.
	displaylist_frame_counters_get(&hb.dlcalls, &hb.tris);
	// MP world-install probe (take-49 follow-up): name the stuck stage of
	// login -> world reset -> chunk promotion right in the heartbeat.
	{
		struct server_remote* mp_remote = mp_session_remote();
		if(mp_remote) {
			struct server_remote_status mp_status;
			server_remote_get_status(mp_remote, &mp_status);
			hb.mp_state = (uint32_t)mp_status.state;
			hb.mp_gen = mp_status.generation;
			hb.mp_logged_in = mp_status.logged_in ? 1u : 0u;
			hb.mp_ret_kb = (uint32_t)(mp_status.retained_bytes / 1024);
			hb.mp_ret_entries = (uint32_t)mp_status.retained_entries;
			hb.mp_ready = (uint32_t)mp_status.ready_entries;
			hb.mp_backlog = (uint32_t)mp_status.promotion_backlog;
			hb.mp_evt_q = (uint32_t)mp_status.queued_events;
			hb.mp_evict = mp_status.retention_evictions;
			hb.mp_alloc_fail = mp_status.retention_allocation_failures;
			hb.mp_reconn = mp_status.forced_reconnects;
			hb.mp_rx_kb = mp_status.rx_bytes / 1024;
			// Terminal-event latch (mp-split-0912/5): WHY this connect
			// generation ended, from the same status snapshot as the fields
			// above. Zero for a healthy live session; latched at the kick /
			// EOF / receive error / connect failure and held until the next
			// connect, so the last heartbeat of a run that came back
			// disconnected still carries the cause.
			hb.mp_end = mp_status.mp_end;
			hb.mp_end_phase = mp_status.mp_end_phase;
			hb.mp_end_rx = mp_status.mp_end_rx;
			// mp-split-0912/25: and the reason TEXT, so an AFK kick is
			// distinguishable from a protocol complaint without someone
			// reading the disconnect screen off the TV. Same snapshot, and
			// the same teardown latch keeps it readable after the impl is
			// freed: server_remote_status_latch_capture memcpy's the WHOLE
			// status (reason[256] included) into the outer handle, and
			// get_status hands that back while impl == NULL.
			//
			// Gated on mp_end: while a session is live `reason` can hold a
			// non-terminal diagnostic, and emitting that as mp_end_reason
			// would assert a cause the collector has no way to disbelieve.
			// A long Beta kick reason loses its tail rather than the heartbeat
			// losing the field -- 47 characters is enough to tell the classes
			// apart. The bound is a "%.*s" PRECISION rather than snprintf's
			// size argument doing the cutting: both produce the same 47
			// characters plus NUL, but the precision makes the truncation part
			// of the conversion, so this call cannot overflow its destination
			// by construction and does not raise -Wformat-truncation for a
			// truncation that is deliberate (reason is 256 B, this field 48).
			if(mp_status.mp_end)
				snprintf(hb.mp_end_reason, sizeof(hb.mp_end_reason), "%.*s",
						 (int)(sizeof(hb.mp_end_reason) - 1),
						 mp_status.reason);
			else
				hb.mp_end_reason[0] = '\0';
			hb.mp_dec_phase = mp_status.dec_phase;
			hb.mp_dec_pkt = mp_status.dec_packet_id;
			hb.mp_dec_have = mp_status.dec_chunk_have;
			hb.mp_dec_need = mp_status.dec_chunk_need;
			// hw1029b/G1: per-packet-kind wire counters (server_remote.h
			// enum server_remote_kind) as their OWN observer line, so the
			// size-gated heartbeat is untouched. Edge-triggered: emitted
			// only when a count changed since the last line (counters are
			// per connect generation, so the collector takes the max).
			{
				static uint32_t s_kind_last[SR_KIND_COUNT];
				/* First row is sent even when every count is 0 (hw1029c: a
				 * run with none of these packets left mp_kinds null). */
				static bool s_kind_sent;
				static uint32_t s_kind_gen;
				if(mp_status.generation != s_kind_gen) {
					s_kind_gen = mp_status.generation;
					s_kind_sent = false; // one first row per connection
				}
				static const char* const kind_names[SR_KIND_COUNT]
					= SERVER_REMOTE_KIND_NAMES;
				uint32_t kinds[SR_KIND_COUNT];
				if(server_remote_get_kind_counts(mp_remote, kinds)
				   && (!s_kind_sent
				       || memcmp(kinds, s_kind_last, sizeof(kinds)) != 0)) {
					char line[384];
					int off = snprintf(line, sizeof line,
						"{\"event\":\"mp_kinds\",\"v\":1,\"tick\":%u,"
						"\"mp_gen\":%u", tick, mp_status.generation);
					for(int k = 0; k < SR_KIND_COUNT && off > 0
						&& (size_t)off < sizeof line; k++)
						off += snprintf(line + off, sizeof line - (size_t)off,
							",\"%s\":%u", kind_names[k], kinds[k]);
					if(off > 0 && (size_t)off + 2 <= sizeof line) {
						line[off] = '}';
						line[off + 1] = '\0';
						if(hw_telemetry_emit_observer(line)) {
							memcpy(s_kind_last, kinds, sizeof(kinds));
							s_kind_sent = true;
						}
					}
				}
			}
			// MP join-burst admission budget (W4, design 2.7): the counters
			// W1/W2 maintain, from the SAME status snapshot as everything
			// above so a heartbeat's residency, backing and refusal numbers
			// describe one consistent moment.
			hb.mp_adm_radius = mp_status.admit_radius > 0
				? (uint32_t)mp_status.admit_radius : 0u;
			hb.mp_resident_cols = (uint32_t)mp_status.resident_entries;
			hb.mp_backed_cols = (uint32_t)mp_status.backed_entries;
			hb.mp_backing_kb = (uint32_t)(mp_status.backing_bytes / 1024);
			hb.mp_backing_peak_kb
				= (uint32_t)(mp_status.backing_peak_bytes / 1024);
			hb.mp_demotions = mp_status.demotions;
			hb.mp_compactions = mp_status.compactions;
			hb.mp_backing_refusals = mp_status.backing_refusals;
			// W3 readiness census, for the ready radius, and ONLY while the
			// loading screen is the thing deciding -- the field's contract
			// is "outstanding obligations of the episode now loading", and
			// after admission later announcements never re-gate (see
			// screen_load_world.c's s_presentation_prepared latch), so a
			// nonzero value in gameplay would misreport a normal streaming
			// backlog as a stalled join. hb.loading_screen is the same
			// predicate the rest of this heartbeat already reports, and it
			// is read here rather than calling lworld_admit_gameplay(),
			// which LATCHES presentation state and must never be driven from
			// a telemetry sample.
			//
			// The census is a pure bounded reader (no allocation, no writes,
			// documented safe from the main thread every frame) and the
			// radius mirrors screen_load_world.c's lworld_ready_radius():
			// mp.ready_radius, defaulting to the LOD near radius, clamped to
			// [1, MAX_VIEW_DISTANCE]. That helper is static to the screen and
			// is not exported, so the three lines are duplicated rather than
			// reached for; if a third reader ever appears, export it instead.
			if(hb.loading_screen) {
				int ready_radius = world_lod_near_radius();
				if(ready_radius < 1)
					ready_radius = 1;
				ready_radius = config_read_int(&gstate.config_user,
											   "mp.ready_radius", ready_radius);
				if(ready_radius < 1)
					ready_radius = 1;
				if(ready_radius > MAX_VIEW_DISTANCE)
					ready_radius = MAX_VIEW_DISTANCE;
				struct server_remote_ready_census census;
				w_coord_t ready_cx = WCOORD_CHUNK_OFFSET(
					(w_coord_t)floorf(gstate.players[0].camera.x));
				w_coord_t ready_cz = WCOORD_CHUNK_OFFSET(
					(w_coord_t)floorf(gstate.players[0].camera.z));
				if(server_remote_ready_census(mp_remote, (int)ready_cx,
											  (int)ready_cz, ready_radius,
											  &census))
					hb.mp_ready_pending
						= (uint32_t)census.announced_pending;
			}
		}
	}
#ifdef PLATFORM_WII
	hb.heap_used_kb = mi.uordblks / 1024;
	hb.heap_free_kb = mi.fordblks / 1024;
	// Fragmentation triage (see hw_telemetry.h): pieces the free space is in +
	// the top chunk. keepcost stays bytes->KB; ordblks is a raw count.
	hb.heap_blocks = mi.ordblks;
	hb.heap_top_kb = mi.keepcost / 1024;
	hb.mem1_free_kb = (int)(SYS_GetArena1Size() / 1024);
	hb.mem2_free_kb = (int)(SYS_GetArena2Size() / 1024);
	hb.dlbad = g_displaylist_malformed;
	hb.dlcall = g_displaylist_badcall;
	hb.wpad_probe0 = input_wpad_probe0();
	watchdog_main_census_phase(2);
	hb.wkspace_free = (int)input_lwp_wkspace_free();
	{
		unsigned int wt, wu, wb;
		input_lwp_wkspace_stats(&wt, &wu, &wb);
		hb.wkspace_total = (int)wt;
		hb.wkspace_used = (int)wu;
		hb.wkspace_free_blocks = (int)wb;
	}
	watchdog_main_census_phase(0);
#endif
	_Static_assert(sizeof(hb.server_work_us) == SERVER_WORK_COUNT * sizeof(uint32_t),
		"server profiler and telemetry schema must have the same number of phases");
	server_local_work_stats(hb.server_work_us);
	server_world_work_stats(hb.world_work_us);
	memcpy(hb.guard_scan_us, s_guard_scan_us, sizeof(s_guard_scan_us));
	(void)server_local_death_heartbeat(!gstate.remote_session
		&& gstate.players[0].local_player != NULL, &hb);
	(void)server_local_perf_guard_heartbeat(!gstate.remote_session
		&& gstate.players[0].local_player != NULL, &hb);
	if(s_completion_heartbeat_server) {
		(void)server_local_completion_heartbeat(s_completion_heartbeat_server, &hb);
		s_completion_heartbeat_server = NULL;
	}
	s_anchor.deaths = hb.anchor.deaths;
	s_anchor.death_cause = hb.anchor.death_cause;
	if(s_anchor.enabled) {
		extern uint32_t g_srv_tick_backlog;
		static ptime_t last_sample;
		static uint32_t last_ticks;
		static bool sampled;
		ptime_t now = time_get();
		int missing[2], pending[2], far_missing[2], far_pending[2];
		world_anchor_status(view_terrain(0), gstate.players[0].camera.x,
			gstate.players[0].camera.y, gstate.players[0].camera.z,
			world_server_view_radius() ? world_server_view_radius() : MAX_VIEW_DISTANCE,
			far_view_distance, missing, pending, far_missing,
			far_pending, &s_anchor.collision);
		/* The legacy observer measures both anchors in one storage. When
		 * the guest views another world, replace only its result with that
		 * world's observation, at the guest's own height for collision.
		 * This diagnostic runs at heartbeat cadence, never in simulation. */
		if(gstate.players[1].local_gameplay_ready
		   && gstate.players[1].local_player
		   && view_terrain(1) != view_terrain(0)) {
			int gm[2], gp[2], gf[2], gfp[2];
			world_anchor_status(view_terrain(1), gstate.players[1].camera.x,
				gstate.players[1].camera.y, gstate.players[1].camera.z,
				world_server_view_radius() ? world_server_view_radius() : MAX_VIEW_DISTANCE,
				far_view_distance, gm, gp, gf, gfp, &s_anchor.collision);
			missing[1] = gm[1];
			pending[1] = gp[1];
			far_missing[1] = gf[1];
			far_pending[1] = gfp[1];
		}
		/* Experimental local split-screen: an admitted guest body is a
		 * real second anchor even with no synthetic separation, so the
		 * receipt records both frontiers rather than one. */
		unsigned anchor_count = (s_anchor.separation
			|| (gstate.players[1].local_gameplay_ready
				&& gstate.players[1].local_player)) ? 2u : 1u;
		for(unsigned p = 0; p < anchor_count; p++) {
			s_anchor.missing[p] = missing[p];
			s_anchor.mesh_pending[p] = pending[p];
			s_anchor.far_missing[p] = far_missing[p];
			s_anchor.far_mesh_pending[p] = far_pending[p];
		}
		bool playable = game_state_client_entities_active(&gstate)
			&& !gstate.world_load_failed
			&& gstate.players[0].current_screen == &screen_ingame
			&& hb.near_missing == 0 && hb.near_mesh_pending == 0
			&& hb.far_missing == 0;
		uint32_t faults[3] = {hb.mem2_oom, hb.mem2_spill, hb.mesh_trunc};
		if(hw_anchor_evidence_begin(&s_anchor, playable, faults)) {
			carve_observer_frame(&gstate.world, frame_counter, tick,
				watchdog_phase_srv_current(), true);
			s_anchor_started = now;
			hw_anchor_evidence_note(&s_anchor, 0, 0, hb.mem2_carve_free_kb);
		}
		float seconds = sampled ? time_diff_s(last_sample, now) : 0;
		s_anchor.actual_tps
			= seconds > 0 ? (hb.server_time - last_ticks) / seconds : 0;
		last_ticks = hb.server_time;
		last_sample = now;
		sampled = true;
		s_anchor.tick_backlog
			= __atomic_load_n(&g_srv_tick_backlog, __ATOMIC_RELAXED);
		s_anchor.damage_cause = server_player_damage_cause_name(__atomic_load_n(
			&s_anchor_server->player_damage_cause, __ATOMIC_RELAXED));
		s_anchor.walk_tick = gstate.autopilot.tick;
		s_anchor.walk_stage = gstate.autopilot.keepup.stage;
		s_anchor.walk_brake = gstate.autopilot.brake;
		s_anchor.walk_water = gstate.autopilot.water;
		s_anchor.walk_capture = gstate.players[0].local_player
			&& gstate.players[0].local_player->data.local_player.capture_input;
		for(unsigned p = 0; p < 2; p++) {
			s_anchor.near_owed[p] = (s_anchor.separation || s_anchor.guest) ?
				__atomic_load_n(&s_anchor_server->anchor_near_owed[p],
								__ATOMIC_RELAXED) :
				s_anchor.missing[0];
			s_anchor.oldest[p] = __atomic_load_n(
				&s_anchor_server->anchor_oldest[p], __ATOMIC_RELAXED);
		}
		/* S1-d (mp-split-0912/68): the two owners' accepted-action tallies,
		 * read from the server's main-thread MIRROR -- never by walking
		 * s->guest_actor, which the server thread frees on leave. Same
		 * RELAXED-load shape as the two anchor counters above.
		 *
		 * The _Static_assert is the whole reason the wire array can carry a
		 * duplicated literal: hw_telemetry.h is platform code and cannot
		 * include the server state header, so THIS copy site -- the only
		 * place both constants are visible -- pins them together. */
		_Static_assert((unsigned)SERVER_ACTION_COUNT == HW_ANCHOR_ACTION_COUNT,
			"heartbeat act0/act1 must carry every server_player_action slot");
		for(unsigned p = 0; p < 2; p++)
			for(unsigned k = 0; k < HW_ANCHOR_ACTION_COUNT; k++)
				s_anchor.actions[p][k] = __atomic_load_n(
					&s_anchor_server->actor_actions[p][k], __ATOMIC_RELAXED);
		/* Per-OWNER health, -1 when that owner has no body (the same sentinel
		 * hb.player_health uses for owner 0). Read from the CLIENT bodies,
		 * like player_health, so both numbers come from one consistent
		 * snapshot rather than mixing a client and a server view. */
		for(unsigned p = 0; p < 2; p++)
			s_anchor.health[p] = gstate.players[p].local_player
				? gstate.players[p].local_player->health : -1;
		hb.anchor = s_anchor;
	}
	struct column_view_stats storage;
	column_view_get_stats(&storage);
	hb.storage_enabled = true;
	hb.op = storage.pages;
	hb.oph = storage.peak_pages;
	hb.opc = COLUMN_VIEW_POOL_PAGES;
	hb.opf = storage.page_failures;
	hb.oer = storage.emergency_uses;
	hb.orc = storage.recovery_entries;
	hb.ocy = storage.recovery_cycles;
	hb.oex = storage.recovery_expiries;
	hb.owu = storage.recovery_wait_max_us;
	column_backing_get_stats(&hb.db, &hb.ds);
	displaylist_storage_stats(&hb.dln, &hb.dls, &hb.dlp);
	hb.dlc = displaylist_compact_enabled();
	displaylist_block_counters(&hb.dlbp, &hb.dlbo, &hb.dlbf);
	_Static_assert(sizeof(hb.srv_ph) / sizeof(hb.srv_ph[0]) >= (size_t)WDS_MAX,
				   "srv_ph must hold every wd_phase_srv");
	watchdog_srv_phase_take(hb.srv_ph);
	{
		/* server_local.c TICK_CLOCK_TRACE, read under its seqlock so one
		 * heartbeat never mixes two published windows (Astra round 8, 4). */
		extern uint32_t g_srv_tclk[], g_srv_tclk_seq; /* TCLK_N entries; unsized so the bound lives in one place */
		uint32_t s1, s2;
		unsigned tries = 0;
		do {
			s1 = __atomic_load_n(&g_srv_tclk_seq, __ATOMIC_RELAXED);
			__atomic_thread_fence(__ATOMIC_SEQ_CST);
			for(unsigned k = 0; k < 14; k++)
				hb.srv_tclk[k] = k < 13 ? __atomic_load_n(&g_srv_tclk[k], __ATOMIC_RELAXED) : 0u;
			__atomic_thread_fence(__ATOMIC_SEQ_CST);
			s2 = __atomic_load_n(&g_srv_tclk_seq, __ATOMIC_RELAXED);
		} while((s1 != s2 || (s1 & 1u)) && ++tries < 8);
	}
	/* S-A3 (Astra H3): the live toggle's own state, straight off the row's
	 * read-only getter -- the allocation-latency half of "what did a live
	 * enable cost"; the MEM2-headroom half is mem2_carve_free_kb above. */
	{
		struct split_live_status sl = screen_options_split_live_status();
		hb.split_live = sl.live;
		hb.split_pending = sl.pending;
		hb.split_toggle_ms = sl.toggle_ms;
		hb.split_last_ok = sl.last_ok;
	}
	hb.split_reach = s_split_fog_applied ? (uint32_t)world_split_render_reach() : 0u;
	hb.server_radius = (uint32_t)(world_server_view_radius() ? world_server_view_radius() : MAX_VIEW_DISTANCE);
	{
		struct displaylist_block_stats bs;
		displaylist_block_stats(&bs);
		hb.dlbm = (bs.mode_requested ? 2u : 0u) | (bs.mode_effective ? 1u : 0u);
		hb.dlbs = bs.scratch_fail;
		hb.dlbc = bs.copy_bytes >> 10;
		hb.dlbx = bs.spill_bytes >> 10;
	}
	hb.cw_set_n = s_cw_set_n_applied;
	/* The loading screen's CUMULATIVE backstop-admission counter, read fresh
	 * on EVERY heartbeat of EVERY profile (Astra gate 1 Lane B item 2:
	 * "Export the cumulative timeout counter without resetting away
	 * failures... Missing telemetry fails acceptance"). Never cached here: it
	 * is a process-lifetime counter the screen owns and only ever increments,
	 * so a stale copy could report a zero the run has already lost. 0 from
	 * the first heartbeat to the last is what a healthy run looks like --
	 * including across a rejoin, which is the whole point of it being
	 * cumulative rather than per-episode. */
	hb.lw_to_admits = lworld_ready_timeout_admits();
	/* REL-MP-COREFIRST: the remote safe-core gauges ("lwc"), remote sessions
	 * only so every local/split line keeps its bytes. Radius 1 = the gate's
	 * own LWORLD_SAFE_CORE_RADIUS, 9 lookups. */
	if(gstate.remote_session) {
		int lwc_missing = 0, lwc_pending = 0;
		if(gstate.world_loaded) {
			w_coord_t lwc_cx = WCOORD_CHUNK_OFFSET(
				(w_coord_t)floorf(gstate.players[0].camera.x));
			w_coord_t lwc_cz = WCOORD_CHUNK_OFFSET(
				(w_coord_t)floorf(gstate.players[0].camera.z));
			world_near_ring_status(view_terrain(0), lwc_cx, lwc_cz,
				LWORLD_SAFE_CORE_RADIUS, &lwc_missing, &lwc_pending);
		} else {
			lwc_missing = LWORLD_SAFE_CORE_COLUMNS;
		}
		hb.lwc_present = true;
		hb.lwc[0] = (uint32_t)lwc_missing;
		hb.lwc[1] = (uint32_t)lwc_pending;
		lworld_core_timing(&hb.lwc[2], &hb.lwc[3]);
	}
	/* nether-dark/1: client vs server block-light census ("blc"). Telemetry
	 * only, read-only: at most once per 5 s, player 0's 3x3 near columns of
	 * the viewed world (its current dimension). Shared (local-authority)
	 * columns count both the presentation the mesher reads and the server's
	 * backing arrays under one stripe hold each; owned-voxel (remote) columns
	 * count the client copy only. The last result rides every heartbeat. */
	{
		static bool s_blc_have;
		static ptime_t s_blc_at;
		static int32_t s_blc_dim;
		static uint32_t s_blc[5];
		if(gstate.world_loaded
		   && (!s_blc_have || time_diff_s(s_blc_at, time_get()) >= 5.0F)) {
			struct world* bw = view_terrain(0);
			ptime_t blc_begin = time_get();
			w_coord_t bcx = WCOORD_CHUNK_OFFSET(
				(w_coord_t)floorf(gstate.players[0].camera.x));
			w_coord_t bcz = WCOORD_CHUNK_OFFSET(
				(w_coord_t)floorf(gstate.players[0].camera.z));
			uint32_t cols = 0, shared = 0, client_lit = 0, server_lit = 0;
			for(w_coord_t dz = -1; dz <= 1; dz++)
				for(w_coord_t dx = -1; dx <= 1; dx++) {
					struct world_section* s = dict_wsection_get(bw->sections,
						SECTION_TO_ID(bcx + dx, bcz + dz));
					if(!s || s->is_far_lod)
						continue;
					cols++;
					struct chunk* c0 = s->column[0];
					if(c0 && c0->ownership == LOCAL_SHARED_VIEW && c0->view) {
						uint32_t vl = 0, bl = 0;
						column_view_torch_census(c0->view, &vl, &bl);
						shared++;
						client_lit += vl;
						server_lit += bl;
						continue;
					}
					for(size_t k = 0; k < COLUMN_HEIGHT; k++) {
						struct chunk* c = s->column[k];
						if(!c || c->ownership != OWNED_VOXEL || !c->blocks)
							continue;
						/* chunk_get_block_voxel layout: 5 bytes per cell
						 * pair, light bytes at +2/+3, torch = high nibble. */
						for(size_t i = 0; i < CHUNK_SIZE * CHUNK_SIZE
								* CHUNK_SIZE / 2; i++)
							client_lit += (c->blocks[i * 5 + 2] >> 4 != 0)
								+ (c->blocks[i * 5 + 3] >> 4 != 0);
					}
				}
			s_blc_at = time_get();
			s_blc_dim = (int32_t)bw->dimension;
			s_blc[0] = cols;
			s_blc[1] = shared;
			s_blc[2] = client_lit;
			s_blc[3] = server_lit;
			s_blc[4] = (uint32_t)(time_diff_s(blc_begin, s_blc_at) * 1000000.0F);
			s_blc_have = true;
		}
		if(s_blc_have) {
			hb.blc_present = true;
			hb.blc_dim = s_blc_dim;
			hb.blc[0] = s_blc[0];
			hb.blc[1] = s_blc[1];
			hb.blc[2] = s_blc[2];
			hb.blc[3] = s_blc[3];
			hb.blc[4] = (uint32_t)time_diff_s(s_blc_at, time_get());
			hb.blc[5] = s_blc[4];
		}
	}
	/* hw1030n: "ssw" only while main waits on the new-world spawn search. */
	hb.ssw_present = server_local_spawn_search_progress(&hb.ssw[0], &hb.ssw[1]);
	{
		struct server_local_tnt_evidence tnt = {0};
		server_local_tnt_evidence_take(&tnt);
		hb.tnt_valid = tnt.valid;
		hb.tnt_st = tnt.st;
		hb.tnt_bits = tnt.bits;
		hb.tnt_primed_left = tnt.primed_left;
		hb.tnt_carved = tnt.carved;
		hb.tnt_pad = tnt.pad;
		hb.tnt_gen = tnt.gen;
		hb.tnt_tick = tnt.tick;
	}
	{
		/* mob_motion_2m verdict + the mob-move cost counters (lane C, mob
		 * motion units S2 proof / Fable S2 F2). Both are emitted only when
		 * live (hw_telemetry_format_heartbeat); the counters are enabled only
		 * on mob_motion_2m / perf_stress_5m, so every other profile keeps its
		 * wire bytes (the collector's summary gains the mm_terminal/mm_cost
		 * keys for every run -- null / zero where unused). */
		struct server_local_mob_motion_evidence mm = {0};
		(void)server_local_mob_motion_evidence_take(&mm);
		hb.mm_valid = mm.valid;
		hb.mm_st = mm.st;
		hb.mm_bits = mm.bits;
		hb.mm_recur_ok = mm.recur_ok;
		hb.mm_recur_bad = mm.recur_bad;
		hb.mm_rise_milli = mm.rise_milli;
		hb.mm_step_milli = mm.step_milli;
		hb.mm_gen = mm.gen;
		hb.mm_tick = mm.tick;
		struct mob_motion_cost cost;
		mob_motion_cost_take(&cost);
		hb.mm_moves = cost.moves;
		hb.mm_move_us = cost.move_us;
		hb.mm_env_walks = cost.env_walks;
		hb.mm_env_ents = cost.env_entities;
	}
	{
		/* residency-1005/2: player residency counters ("rz"). */
		_Static_assert(HW_TELEMETRY_RZA_FIELDS == SERVER_LOCAL_RZA_FIELDS,
			"heartbeat rza rows must mirror enum server_local_rza_field");
		_Static_assert(HW_TELEMETRY_RZI_MAX == SERVER_LOCAL_RZI_MAX
			&& HW_TELEMETRY_RZI_FIELDS == SERVER_LOCAL_RZI_FIELDS,
			"heartbeat rzi marks must mirror server_local_rz_wire");
		struct server_local_rz_wire rzw;
		server_local_rz_actor_telemetry(&rzw);
		memcpy(hb.rza, rzw.rza, sizeof hb.rza);
		memcpy(hb.rze, rzw.rze, sizeof hb.rze);
		memcpy(hb.rzi, rzw.rzi, sizeof hb.rzi);
		hb.rz_tick = rzw.tick;
		hb.rzi_count = rzw.rzi_count;
		hb.rzf = rzw.rzf;
	}
	tps_observer_snapshot(&hb.cpu);
	hw_telemetry_sender_stats_take(&hb.tel);
	SERVICE_OBSERVER(service_observer_decode_underruns(hb.music_underruns));
	SERVICE_OBSERVER(service_observer_request(false));
	split_dims_fill(&hb.dims);
	anchor_pool_fill(&hb.pool, &hb.dims);
	if(thread_g9_lease_enabled()) thread_g9_lease_state(&hb.g9_lease, NULL);
	/* Publish context and heartbeat together; its priority lane must not
	 * overtake or lose the required guard record in the observer FIFO. */
	hb.perf_guard_present = s_perf_guard_observe;
	hw_telemetry_emit_heartbeat(&hb);
	emit_overlay_spill(tick, storage.fallback_live, storage.fallback_total);
	if(column_light_enabled()) {
		struct column_light_stats reuse;
		column_light_get_stats(&reuse);
		char line[768];
		snprintf(line, sizeof line,
			"{\"event\":\"g9_settled_light_reuse\",\"tick\":%u,\"published\":%u,"
			"\"hits\":%u,\"avoided_pops\":%u,\"probe_pops\":%u,\"live\":%u,"
			"\"peak_live\":%u,\"bytes\":%u,\"peak_bytes\":%u,"
			"\"copy_elapsed_us\":%u,\"validation_elapsed_us\":%u,"
			"\"fallback\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]}", tick,
			reuse.published, reuse.hits, reuse.avoided_pops, reuse.probe_pops,
			reuse.live, reuse.peak_live, reuse.bytes, reuse.peak_bytes,
			reuse.copy_us, reuse.validate_us, reuse.fallback[0], reuse.fallback[1],
			reuse.fallback[2], reuse.fallback[3], reuse.fallback[4], reuse.fallback[5],
			reuse.fallback[6], reuse.fallback[7], reuse.fallback[8], reuse.fallback[9],
			reuse.fallback[10]);
		(void)hw_telemetry_emit_observer(line);
	}
	if(thread_g9_tick_boost_enabled()) {
		struct g9_tick_boost_stats tb;
		bool boosted = false;
		thread_g9_tick_boost_snapshot(&tb, &boosted);
		struct audio_music_promotion_stats mp;
		audio_music_decode_promotion_snapshot(&mp);
		char line[640];
		snprintf(line, sizeof line,
			"{\"event\":\"g9_tick_boost\",\"v\":1,\"tick\":%u,\"ticks\":%u,"
			"\"capped\":%u,\"rejected\":%u,\"max_us\":%u,\"last_us\":%u,"
			"\"over_cap\":%u,\"sender_sets\":%u,\"sender_fail\":%u,"
			"\"priority_sets\":%u,\"boosted\":%u,\"music_promotions\":%u,"
			"\"music_demotions\":%u,\"music_promoted_chunks\":%u,"
			"\"music_promoted_max_us\":%u,\"music_promoted\":%u}", tick,
			tb.ticks, tb.capped, tb.rejected, tb.max_us, tb.last_us, tb.over_cap,
			tb.sender_sets, tb.sender_fail, tb.priority_sets, boosted,
			mp.promotions, mp.demotions, mp.promoted_chunks, mp.promoted_max_us,
			mp.promoted);
		(void)hw_telemetry_emit_observer(line);
	}
	if(g9_worst_tick_enabled()) {
		/* R20 Q3: the window's max-wt tick, formatted here on MAIN at the
		 * heartbeat -- never inside the server's timed span. */
		struct g9_worst_tick_rec wr;
		if(g9_worst_tick_take(&wr)) {
			char line[G9WT_LINE_CAP];
			if(g9_worst_tick_format(&wr, line, sizeof line))
				(void)hw_telemetry_emit_observer(line);
		}
	}
	if(thread_g9_lease_enabled()) {
		struct g9_priority_lease state;
		thread_g9_lease_state(&state, NULL);
		struct g9_lease_stats ls = state.stats;
		char line[1024];
		snprintf(line, sizeof line,
			"{\"event\":\"g9_lease\",\"v\":2,\"tick\":%u,\"acquired\":%u,"
			"\"restored\":%u,\"rejected\":%u,\"services\":%u,\"late_ack\":%u,"
			"\"stalls\":%u,\"audio_reject\":%u,\"cancelled\":%u,\"timer_restores\":%u,"
			"\"lease_max_us\":%u,\"service_max_us\":%u,\"overshoot_us\":%u,"
			"\"reserve_min_us\":%u,\"faults\":%u,\"generation\":%u,\"turn\":%u,"
			"\"active\":%u,\"boosted\":%u,\"failed\":%u,\"waiting\":%u,"
			"\"max_us\":%u,\"service_us\":%u,\"wait_us\":%u,"
			"\"priority_sets\":%u,\"sender_sets\":%u,\"lease_pops\":%u,"
			"\"pops_max\":%u,\"pops_last\":%u,\"parked\":%u,\"cancel_latched\":%u}", tick,
			ls.acquired, ls.restored, ls.rejected, ls.services, ls.late_ack,
			ls.stalls, ls.audio_reject, ls.cancelled, ls.timer_restores,
			ls.lease_max_us, ls.service_max_us, ls.overshoot_us,
			ls.reserve_min_us, ls.faults, state.generation, state.turn,
			state.active, state.boosted, state.failed, state.waiting,
			state.max_us, state.service_us, state.wait_us,
			ls.priority_sets, ls.sender_sets, ls.lease_pops, ls.pops_max,
			ls.pops_last, state.parked, state.cancelled);
		(void)hw_telemetry_emit_observer(line);
	}
	settle_observer_publish(hw_telemetry_emit_observer);
	if(carve_observer_active()) carve_observer_anchor(
		gstate.players[0].camera.x, gstate.players[0].camera.z, s_anchor.separation);
	/* Stage only -- the twelve-record census set is drained one record per
	 * frame by carve_observer_emit_next below. Publishing it here filled the
	 * four-slot telemetry FIFO inside one frame and lost nine of twelve
	 * records every second (2026-09-07 hardware finding). */
	if(carve_observer_active() && !hw_telemetry_quiesced()
	   && !carve_observer_stage(&gstate.world, tick, s_anchor.measuring))
		fprintf(stderr, "[carve-census] packet failure\n");
	s_anchor.frame_count = 0;
}

/* ===== SPLIT_ACTION_SCRIPT (S1-d, mp-split-0912/68) =========================
 * The scripted GUEST action sequence behind the split_actions_6m console
 * profile. It exists to make one claim machine-checkable on real hardware:
 * the guest dig/place/hotbar/use actions land on the GUEST body, on its own
 * inventory, while the host performs no action at all.
 *
 * HOW IT ACTS. Every step is an ordinary SRPC posted through the REAL slot-1
 * mailbox with source {1, players[1].local_generation} -- byte-identical in
 * shape and route to what screen_ingame.c sends when the second controller
 * presses a button (S1-b). Nothing here calls into the server directly, and
 * nothing bypasses server_local_process_owned generation/ticket/body gates:
 * if owner routing were broken these requests would be dropped exactly like a
 * real guest, which is what makes the collector "act1 must be non-zero" check
 * meaningful rather than self-fulfilling.
 *
 * THE CYCLE, one step every SPLIT_ACTION_STEP_TICKS (40) client ticks:
 *   0 HOTBAR -> slot 1   (a real selection change, away from the work slot)
 *   1 HOTBAR -> slot 0   (back on the slot a collected block lands in --
 *                         inventory_player.c collect priority is hotbar 0
 *                         first, and the guest body starts with an EMPTY
 *                         inventory)
 *   2 DIG(finished) at the floor cell under the guest feet; the cell is
 *     remembered for step 4
 *   3 USE (air use, charge 0)
 *   4 PLACE back into the remembered cell, by clicking the block BELOW it
 *     with SIDE_TOP
 *
 * WHY THE GUEST SETTLES BEFORE DIG AND STAYS STILL THROUGH PICKUP. The guest
 * body owns no items at admission -- server_local_guest_create calloc
 * allocates it and only a saved guest record can fill it -- so the block it
 * places is the block it just dug. That means the drop has to be COLLECTED,
 * and Beta pickup needs the body within ~1 block of the item for a tick after
 * EntityItem's 10-tick pickup_delay. The script releases FORWARD (and JUMP)
 * for the existing interval before DIG so prior real movement settles, then
 * keeps them released for the existing interval after DIG so the guest stays
 * on its own drop through the pickup delay. Everything else in the profile --
 * the fixed-heading walk, the unstick, the two-anchor residency -- is
 * unchanged from split_play_8m.
 *
 * WHY THE PLACE COMES TWO STEPS AFTER THE DIG. Placing into the cell while the
 * body is standing in it would be refused by the two-body placement
 * obstruction sweep (6bde02c7), and rightly so. After USE the guest resumes
 * walking for the same full pre-PLACE interval as before, so the remembered
 * cell is empty of bodies. The server applies no reach gate to BLOCK_PLACE/
 * BLOCK_DIG (reach is a client concern), so the distance costs nothing.
 *
 * NO NEW RNG DRAWS. Every value here is derived from the body own position and
 * a step counter; the script never touches a simulation RNG stream. The blocks
 * it removes and restores are ordinary player edits on the throwaway harness
 * world, exactly like the walk own terrain interaction.
 *
 * A step that cannot be posted (the RPC queue is momentarily full) is RETRIED
 * on the next boundary rather than skipped -- a silently dropped step would
 * show up as a missing per-owner effect, i.e. as a FAIL that blames the game
 * for a harness hiccup. */
#define SPLIT_ACTION_STEP_TICKS 40u
enum split_action_step {
	SPLIT_ACTION_HOTBAR_AWAY = 0,
	SPLIT_ACTION_HOTBAR_BACK,
	SPLIT_ACTION_DIG,
	SPLIT_ACTION_USE,
	SPLIT_ACTION_PLACE,
	SPLIT_ACTION_STEP_COUNT,
};
static uint32_t s_split_action_tick;
static enum split_action_step s_split_action_step;
static w_coord_t s_split_action_x, s_split_action_y, s_split_action_z;
static bool s_split_action_have_target;

static bool split_action_send(struct server_rpc* call) {
	struct server_rpc_source guest = {
		.slot = 1, .generation = gstate.players[1].local_generation};
	return svin_rpc_send_from(guest, call);
}

/* Drives one client tick of the script, and may clear the guest own walk
 * inputs for the collect window. `frame` is the guest owned input frame,
 * already filled by the forced-walk block that calls this. */
static void split_action_script_tick(struct input_player_frame* frame) {
	const struct entity* body = gstate.players[1].local_player;
	if(!body || !frame)
		return;
	/* Settle for one interval before DIG, then remain stopped through the
	 * existing post-DIG collect interval. PLACE remains a walking interval. */
	if(s_split_action_step == SPLIT_ACTION_DIG
	   || s_split_action_step == SPLIT_ACTION_USE) {
		frame->down[IB_FORWARD] = false;
		frame->down[IB_JUMP] = false;
	}
	if(++s_split_action_tick % SPLIT_ACTION_STEP_TICKS)
		return;
	/* EYE-basis position (entity_local_player.c convention): pos[1] is the
	 * eye, so the block the body stands ON is one below the feet. The 0.2F is
	 * the same footing epsilon that file step-sound probe uses. */
	w_coord_t fx = (w_coord_t)floorf(body->pos[0]);
	/* 1.62F is Beta EntityPlayer eye height -- entity_local_player.c EYE_HEIGHT
	 * and combat.h PLAYER_EYE_HEIGHT are the same constant; neither header is
	 * on main.c include path, and server_local.c nether rig spells it
	 * literally here for the same reason. */
	w_coord_t fy = (w_coord_t)floorf(body->pos[1] - 1.62F - 0.2F);
	w_coord_t fz = (w_coord_t)floorf(body->pos[2]);
	bool posted = false;
	switch(s_split_action_step) {
		case SPLIT_ACTION_HOTBAR_AWAY:
		case SPLIT_ACTION_HOTBAR_BACK: {
			struct server_rpc call = {
				.type = SRPC_HOTBAR_SLOT,
				.payload.hotbar_slot.slot
				= s_split_action_step == SPLIT_ACTION_HOTBAR_AWAY ? 1 : 0,
			};
			posted = split_action_send(&call);
			break;
		}
		case SPLIT_ACTION_DIG: {
			if(fy <= 0 || fy >= WORLD_HEIGHT)
				break; /* Retry next boundary; never post an out-of-range cell. */
			struct server_rpc call = {
				.type = SRPC_BLOCK_DIG,
				.payload.block_dig = {.finished = true, .x = fx, .y = fy,
									  .z = fz, .side = SIDE_TOP},
			};
			posted = split_action_send(&call);
			if(posted) {
				s_split_action_x = fx;
				s_split_action_y = fy;
				s_split_action_z = fz;
				s_split_action_have_target = true;
			}
			break;
		}
		case SPLIT_ACTION_USE: {
			struct server_rpc call = {
				.type = SRPC_USE_ITEM,
				.payload.use_item = {.charge = 0.0F, .has_held_item = false},
			};
			posted = split_action_send(&call);
			break;
		}
		case SPLIT_ACTION_PLACE: {
			if(!s_split_action_have_target || s_split_action_y < 1) {
				/* No dug cell to restore (the dig step never got posted):
				 * move to the next cycle rather than clicking a cell nobody
				 * chose. */
				posted = true;
				break;
			}
			struct server_rpc call = {
				.type = SRPC_BLOCK_PLACE,
				.payload.block_place = {.x = s_split_action_x,
										.y = (w_coord_t)(s_split_action_y - 1),
										.z = s_split_action_z,
										.side = SIDE_TOP,
										.has_held_item = false},
			};
			posted = split_action_send(&call);
			if(posted)
				s_split_action_have_target = false;
			break;
		}
		default:
			posted = true;
			break;
	}
	if(posted)
		s_split_action_step = (enum split_action_step)
			((s_split_action_step + 1) % SPLIT_ACTION_STEP_COUNT);
}

// hw_run injects EPHEMERAL, capture/hardware-proof-only config values (e.g.
// "autoload" -> "+new" so a headless profile can force a scenario without
// touching the user's real config.json). These wrappers pair the set with
// config_mark_ephemeral so config_save() (source/config.c) strips exactly
// what THIS process injected before anything ever reaches disk -- closes the
// "autoload +new sentinel leak" amplifier, where any Options visit during an
// hw-run-armed session would otherwise bake every hw_run-injected key into
// config.json permanently (options_commit -> config_save serializes the
// WHOLE tree). Used ONLY inside the `if(hw_run.enabled)` block below --
// everywhere else in the codebase keeps calling config_set_string/
// config_set_int directly, because those are the user's own real settings
// and must persist normally.
static bool hw_run_config_set_string(struct config* c, const char* key,
									 const char* value) {
	bool ok = config_set_string(c, key, value);
	config_mark_ephemeral(c, key);
	return ok;
}

static bool hw_run_config_set_int(struct config* c, const char* key,
								  int value) {
	bool ok = config_set_int(c, key, value);
	config_mark_ephemeral(c, key);
	return ok;
}

/* ---- split_recovery_6m: the five hooks the phase driver cannot own ------
 * source/game/split_recovery_script.[ch] holds the phase machine, the
 * baselines and the markers; these are the engine calls it makes through
 * main. Every one of them is unreachable unless that driver is ARMED, which
 * only HW_RUN_PROFILE_SPLIT_RECOVERY_6M does. */
static struct split_recovery_actions s_split_recovery_act;
static struct server_local* s_spq_server;
static uint32_t s_spq_unload_requests;
static bool s_spq_request_after_switch;
static uint32_t s_spq_last_switch_begin_seq;
static char s_spq_world_path[256];
static bool sr_hook_arm_autoload(void* user, const char* directory);
static void sr_hook_profile_done(void* user, const char* status);

/* server_local.c's per-tick counter, the same one main publishes as the
 * heartbeat's server_time (see its other extern below). Read with an atomic
 * relaxed load: it is the safe snapshot of "the simulation advanced", never an
 * unsynchronised read of server_local from the main thread. */
static uint32_t split_recovery_sim_ticks(void) {
	extern uint32_t g_srv_sim_ticks;
	return __atomic_load_n(&g_srv_sim_ticks, __ATOMIC_RELAXED);
}

static bool s_split_recovery_seed_set;
static int64_t s_split_recovery_seed;
static char s_split_recovery_status[96];
static bool s_split_recovery_done_pending;

static bool sr_hook_rpc_guest(void* user, struct server_rpc* call) {
	(void)user;
	/* The existing slot-1 guest mailbox -- the same source split_action_send
	 * uses -- never a new transport. */
	struct server_rpc_source guest
		= {.slot = 1, .generation = gstate.players[1].local_generation};
	return svin_rpc_send_from(guest, call);
}

/* The HOST owner source main.c already builds for player 0 everywhere else
 * (slot 0, generation 0). The host digs its own P0 baseline blocks through it;
 * nothing gives the host an item. */
static bool sr_hook_rpc_host(void* user, struct server_rpc* call) {
	(void)user;
	struct server_rpc_source host = {.slot = 0, .generation = 0};
	return svin_rpc_send_from(host, call);
}

/* world_get_block answers a NON-RESIDENT column with block id 1 (STONE) for
 * any y below WORLD_HEIGHT -- world.c's "no chunk" fallback, which exists so
 * the renderer treats unloaded space as solid rather than as a hole. Reading
 * that as a real block would make P0 publish a block_placed nobody observed and
 * P4 FAIL "block lost: got 1/0" on terrain that simply had not streamed in yet.
 * So residency is the gate: not resident -> no answer, and the driver waits. */
static bool sr_hook_block_at(void* user, int x, int y, int z, uint32_t* id,
							 uint32_t* meta) {
	(void)user;
	if(!world_find_chunk(&gstate.world, (w_coord_t)x, (w_coord_t)y,
						 (w_coord_t)z))
		return false;
	struct block_data b = world_get_block(&gstate.world, (w_coord_t)x,
										  (w_coord_t)y, (w_coord_t)z);
	*id = b.type;
	*meta = b.metadata;
	return true;
}

/* NULL while P4 will still be able to arm the named-autoload hook. */
static const char* sr_hook_autoload_conflict(void* user) {
	(void)user;
	if(!screen_sworld_autoload_consumed())
		return NULL;
	const char* value
		= config_read_string(&gstate.config_user, "autoload", "");
	return value && value[0] ? value : "(env CAVEX_AUTOLOAD)";
}

static bool sr_hook_emit(void* user, const char* line) {
	(void)user;
	split_portal_quit_capture_recovery_marker(line);
	return hw_telemetry_emit_observer(line);
}

/* Arm the existing debug portal build only after the guest has moved away and
 * both inventory/block baselines exist. Join the server thread before writing
 * its debug defer field; a live cross-thread store would race its tick. The
 * normal server thread and portal contact/dwell path run after restart. */
static bool spq_hook_arm_portal(void) {
	if(!s_spq_server || !s_spq_server->thread_started) return false;
	server_local_shutdown(s_spq_server);
	snprintf(s_spq_world_path, sizeof s_spq_world_path, "%s",
		string_get_cstr(s_spq_server->level_name));
	if(!s_spq_world_path[0]) {
		(void)server_local_start(s_spq_server);
		return false;
	}
	s_spq_server->portal_demo_defer = 1;
	if(!server_local_start(s_spq_server)) return false;
	return true;
}
static bool spq_hook_emit(const char* line) {
	return hw_telemetry_emit_observer(line);
}
static bool spq_hook_autoload(const char* name) {
	return sr_hook_arm_autoload(NULL, name);
}
static bool spq_hook_block(int x, int y, int z, uint32_t* id,
	uint32_t* meta) {
	return sr_hook_block_at(NULL, x, y, z, id, meta);
}
static bool spq_hook_saved_block(int x, int y, int z, uint32_t* id,
	uint32_t* meta) {
	if(!s_spq_world_path[0] || y < 0 || y >= WORLD_HEIGHT) return false;
	uint8_t* ids = malloc(CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT);
	uint8_t* data = malloc(CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
	if(!ids || !data) { free(ids); free(data); return false; }
	string_t path;
	string_init_set_str(path, s_spq_world_path);
	struct region_archive archive;
	int cx = (int)floorf((float)x / CHUNK_SIZE);
	int cz = (int)floorf((float)z / CHUNK_SIZE);
	bool opened = region_archive_create(&archive, path,
		(w_coord_t)floorf((float)cx / REGION_SIZE),
		(w_coord_t)floorf((float)cz / REGION_SIZE), WORLD_DIM_OVERWORLD);
	string_clear(path);
	bool ok = false;
	if(opened) {
		ok = region_archive_get_block_ids_metadata(&archive,
			(w_coord_t)cx, (w_coord_t)cz, ids, data)
			== REGION_ARCHIVE_READ_OK;
		(void)region_archive_destroy(&archive);
		if(ok) {
			unsigned index = (((unsigned)x & 15u) * 16u
				+ ((unsigned)z & 15u)) * WORLD_HEIGHT + (unsigned)y;
			*id = ids[index];
			*meta = (data[index / 2] >> ((index & 1u) * 4u)) & 15u;
		}
	}
	free(ids); free(data);
	return ok;
}
static void spq_hook_done(const char* status) {
	sr_hook_profile_done(NULL, status);
}

/* The driver decides the verdict; main owns the SAME completion tail every
 * other profile uses (final heartbeat -> profile_done -> quiesce -> the
 * hands-free auto-exit), so a recovery run tears down exactly like the rest. */
static void sr_hook_profile_done(void* user, const char* status) {
	(void)user;
	snprintf(s_split_recovery_status, sizeof(s_split_recovery_status), "%s",
			 status && status[0] ? status : "ok");
	s_split_recovery_done_pending = true;
}

/* The create deletes an existing rig world first, and sworld_delete_tree over
 * a used save on libfat is seconds of synchronous SD work on the MAIN thread.
 * Stamp the phase around it so the watchdog reports where it is rather than
 * blaming whatever was stamped last -- the same treatment
 * screen_texturepacks.c:222 gives its pack reload, and WDM_SCREEN_UPDATE is
 * the phase main.c:3946 already stamps around the ordinary create path
 * (screen_sworld_update -> screen_sworld_create_new). */
static bool sr_hook_create_world(void* user, const char* directory) {
	(void)user;
	watchdog_phase_main(WDM_SCREEN_UPDATE);
	bool ok = screen_sworld_create_exact(SPLIT_RECOVERY_RIG_DISPLAY, directory,
										 s_split_recovery_seed_set,
										 s_split_recovery_seed);
	watchdog_phase_main(WDM_IDLE);
	return ok;
}

/* ---- host_vehicle_smoke_8m: the hooks its phase driver cannot own -------
 * source/game/vehicle_smoke_script.[ch] holds the phase machine and the
 * serializer; source/network/vehicle_smoke_probe.[ch] the owner-thread
 * records. These are the engine calls the driver makes through main. Every
 * one is unreachable unless vehicle_smoke_arm() armed the driver, which only
 * HW_RUN_PROFILE_HOST_VEHICLE_SMOKE_8M does (with a valid --cw-vehicle-kind). */
static struct vehicle_smoke_actions s_vehicle_act;
static char s_vehicle_status[96];
static bool s_vehicle_done_pending;
static bool s_vehicle_look_applied;
static float s_vehicle_last_yaw, s_vehicle_last_pitch = 1.5707964F;
static bool s_vehicle_prev_loaded;
static uint32_t s_vehicle_reset_count;

static bool vs_hook_emit(void* user, const char* line) {
	(void)user;
	/* O22: vehicle_smoke_emit keeps a refused line and resends it next
	 * frame, so a refusal is a deferral, not a lost record (tel_drop). */
	return hw_telemetry_emit_observer_retry(line);
}

/* O22: the stream can never accept another line this session. */
static bool vs_hook_telemetry_lost(void* user) {
	(void)user;
	return hw_telemetry_quiesced();
}

static void vs_hook_profile_done(void* user, const char* status) {
	(void)user;
	snprintf(s_vehicle_status, sizeof(s_vehicle_status), "%s",
			 status && status[0] ? status : "ok");
	s_vehicle_done_pending = true;
}

static bool vs_hook_create_world(void* user, const char* directory) {
	(void)user;
	watchdog_phase_main(WDM_SCREEN_UPDATE);
	bool ok = screen_sworld_create_vehicle_rig(VEHICLE_SMOKE_RIG_DISPLAY,
		directory, s_split_recovery_seed_set, s_split_recovery_seed);
	watchdog_phase_main(WDM_IDLE);
	return ok;
}

/* save_reload_3m (R12): the engine calls its phase driver makes through
 * main. Unreachable unless save_reload_arm() ran (that profile only). */
static ptime_t s_save_reload_started;
static bool srl_hook_emit(const char* line) {
	return hw_telemetry_emit_observer_retry(line);
}
static bool srl_hook_create_world(void) {
	watchdog_phase_main(WDM_SCREEN_UPDATE);
	/* The create dialog's own entry: fresh folder (deduped), seeded,
	 * survival -- the same sworld_finish_create tail a player's Create runs. */
	bool ok = screen_sworld_create_from_dialog("CW Save Reload",
		"cw_save_reload", s_split_recovery_seed_set, s_split_recovery_seed,
		false);
	watchdog_phase_main(WDM_IDLE);
	return ok;
}
static bool srl_hook_arm_reload(const char* folder) {
	return sr_hook_arm_autoload(NULL, folder);
}

static bool sop_hook_create_world(void* user, const char* directory) {
	(void)user;
	watchdog_phase_main(WDM_SCREEN_UPDATE);
	bool ok = screen_sworld_create_split_offer_piston_rig("Offered Piston Rig",
		directory, s_split_recovery_seed_set, s_split_recovery_seed);
	watchdog_phase_main(WDM_IDLE);
	return ok;
}

/* The block a VIEW's client terrain holds (LOS for the attempt records).
 * Not resident -> no answer (world_get_block's "no chunk = stone" fallback
 * must not be read as an occluder). */
static bool vs_hook_view_block(unsigned view, int x, int y, int z,
							   uint8_t* type) {
	struct world* terrain = &gstate.world;
	if(clin_world_payloads_enabled()) {
		struct clin_world_payload payload;
		if(!clin_world_payload_of_view(view, &payload) || !payload.terrain)
			return false;
		terrain = payload.terrain;
	}
	if(y < 0 || y >= WORLD_HEIGHT
	   || !world_find_chunk(terrain, (w_coord_t)x, (w_coord_t)y, (w_coord_t)z))
		return false;
	*type = world_get_block(terrain, (w_coord_t)x, (w_coord_t)y,
							(w_coord_t)z).type;
	return true;
}

/* Apply the driver's host look + button LEVELS to the real host input path
 * (autopilot.c's script seam). The look is held whenever the buttons are
 * armed: input.c consults autopilot_force_button only while the autopilot is
 * active, which the aim lock is. */
/* O17 (R07 F3): the scripted pointer owner -- "no pointer", like a Wiimote
 * aimed off-screen. Installed only while the driver asks for it; NULL restores
 * the native pointer path exactly. */
static bool vs_pointer_off_screen(float* x, float* y, float* angle,
								  bool* available) {
	*x = *y = *angle = 0.0F;
	*available = false;
	return true;
}
static bool s_vehicle_pointer_owned;

static void vs_apply_host(const struct vehicle_smoke_actions* act) {
	if(act->host_pointer_owned != s_vehicle_pointer_owned) {
		input_pointer_set_script(act->host_pointer_owned ? vs_pointer_off_screen
														 : NULL);
		s_vehicle_pointer_owned = act->host_pointer_owned;
	}
	bool want = act->host_look || act->host_buttons_armed;
	if(act->host_look) {
		s_vehicle_last_yaw = act->host_yaw;
		s_vehicle_last_pitch = act->host_pitch;
	}
	if(want) {
		autopilot_script_look(true, s_vehicle_last_yaw, s_vehicle_last_pitch);
		autopilot_script_buttons(act->host_buttons_armed, act->host_buttons);
		s_vehicle_look_applied = true;
	} else if(s_vehicle_look_applied) {
		autopilot_script_buttons(false, NULL);
		autopilot_script_look(false, 0.0F, 0.0F);
		s_vehicle_look_applied = false;
	}
}

/* ---- mp_chat_login_3m: the three hooks its phase driver cannot own ------
 * source/game/chat_login_script.[ch] holds the phase machine, the OSK route
 * and the markers; these are the engine calls it makes through main. Every one
 * of them is unreachable unless that driver is ARMED, which only
 * HW_RUN_PROFILE_MP_CHAT_LOGIN_3M does.
 *
 * SHARED WITH mp_play_10m (push-0921/B4c). The two statics below are the ONE
 * driver-completion latch main's completion block reads, for both scripted MP
 * drivers, because the two can never be armed at the same time:
 *   - chat_login_arm() refuses any profile but mp_chat_login_3m /
 *     mp_chat_chord_1m, and mp_play_arm() is called only for mp_play_10m;
 *   - inside an mp_play_10m run the login machine IS used (twice), but it is
 *     armed through chat_login_arm_play(), which takes mp_play's OWN hook set
 *     and never calls profile_done -- chat_login_play_end() exists precisely
 *     so the play driver owns the verdict.
 * So exactly one writer exists per run, and mpp_hook_profile_done below
 * delegates to cl_hook_profile_done rather than keeping a second copy. */
static char s_chat_login_status[96];
static bool s_chat_login_done_pending;

static bool cl_hook_emit(void* user, const char* line) {
	(void)user;
	return hw_telemetry_emit_observer(line);
}

static void cl_hook_profile_done(void* user, const char* status) {
	(void)user;
	snprintf(s_chat_login_status, sizeof(s_chat_login_status), "%s",
			 status && status[0] ? status : "ok");
	s_chat_login_done_pending = true;
}

/* The EXISTING production snapshot, wrapped in mp_session.c so the driver (a
 * shared-engine translation unit) never references the app-only MP module. */
static bool cl_hook_chat_lines_sent(void* user, uint32_t* out) {
	(void)user;
#ifdef CAVEWII_MP_CLIENT
	return mp_session_chat_lines_sent(out);
#else
	(void)out;
	return false;
#endif
}

/* ---- mp_play_10m: the hooks ITS phase driver cannot own ------------------
 * source/game/mp_play_core.h is the pure phase machine and
 * source/game/mp_play_script.[ch] is the device half; these are the engine
 * calls it makes through main. Every one is unreachable unless mp_play_arm()
 * ran, which only HW_RUN_PROFILE_MP_PLAY_10M does. Deliberately the same
 * shape as the chat-login block above -- same emit, same completion latch,
 * same chat_lines_sent -- so one run can never observe two different answers
 * to the same question. */

static bool mpp_hook_emit(void* user, const char* line) {
	(void)user;
	return hw_telemetry_emit_observer(line);
}

/* ONE writer of the shared latch (see its declaration above). */
static void mpp_hook_profile_done(void* user, const char* status) {
	cl_hook_profile_done(user, status);
}

/* The EXISTING production snapshot, the same one the heartbeat's mp_* fields
 * and the connect-screen diagnostics already read once a frame
 * (mp_session_remote() + server_remote_get_status). A hook, not a direct
 * call, because mp_play_script.c lives in the shared engine pool and may not
 * reference the APP-only MP module -- exactly like cl_hook_chat_lines_sent.
 * FALSE means there is no remote provider at all, and the driver then
 * confirms nothing and fails by name on its own readiness budget rather than
 * counting a dig or a place it could not see the server acknowledge. */
/* THE ONLY CALLER of server_remote_get_play_obs, by design (push-0921/B18,
 * review B17 H2). The three observation rings + the world-install generation
 * used to ride inside struct server_remote_status, which is a stack local in
 * emit_hw_heartbeat above, in main()'s own frame, in mp_session and on every
 * frame of the connect screen -- 560 B memcpy'd under the remote lock for six
 * readers that never look at it. They now have their own struct and their own
 * accessor, and this hook -- reached only by the mp_play_10m driver -- is what
 * pays for them.
 *
 * It is also the driver's ONLY remote-observation hook (push-0921/B19 item 0).
 * A sibling mpp_hook_remote_status stood here until the driver stopped reading
 * anything out of struct server_remote_status; its guard was identical to this
 * one, so the "is there a remote provider" question this `false` answers is
 * exactly the question that hook used to answer. emit_hw_heartbeat's own
 * server_remote_get_status call (above) is untouched -- that one really does
 * read the status. */
static bool mpp_hook_remote_play_obs(void* user,
									 struct server_remote_play_obs* out) {
	(void)user;
#ifdef CAVEWII_MP_CLIENT
	struct server_remote* remote = mp_session_remote();
	if(!remote || !remote->impl || !out)
		return false;
	server_remote_get_play_obs(remote, out);
	return true;
#else
	(void)out;
	return false;
#endif
}

/* push-0921/B19: the driver naming the one cell it is waiting on. Same handle,
 * same lock, same "no impl, nothing to do" shape as the observation hook above
 * -- and, like it, reachable only through mp_play_arm's hook table, so no
 * other profile and no ordinary player ever arms a watch. */
static void mpp_hook_remote_watch(void* user, bool on, int32_t x, int32_t y,
								  int32_t z) {
	(void)user;
#ifdef CAVEWII_MP_CLIENT
	struct server_remote* remote = mp_session_remote();
	if(!remote || !remote->impl)
		return;
	server_remote_play_watch(remote, on, x, y, z);
#else
	(void)on;
	(void)x;
	(void)y;
	(void)z;
#endif
}

/* THE REJOIN's menu focus, for the two front-end screens the driver walks back
 * through (mp_play_core.h's MPP_PH_REJOIN_TITLE and MPP_PH_REJOIN_SETUP). Both
 * answers are lane A's PRODUCTION observers (source/game/gui/screen.h), each of
 * which asks the SAME source/graphics/gui_util.h gutil_menu_activate_decide the
 * screen's own activate press asks, with the same inputs -- so the driver can
 * never be told "this press opens Multiplayer" by anything but the code that
 * would actually open it. They are side-effect-free: nothing is pressed,
 * nothing is consumed, no state advances, and asking twice answers twice the
 * same.
 *
 * FALSE means the press would NOT do that thing -- a different control under
 * the sprite, a disabled control, the activate button still held with no fresh
 * edge available, a sub-editor up, no chosen player name (the press would open
 * the name editor instead of joining) or no server row to join. The core then
 * presses GUI_DOWN and asks again next frame, which is exactly what those
 * observers are for. A press and a navigation are never requested on one frame
 * (mpp_press holds for MPP_PRESS_MS and then leaves a MPP_GAP_MS released gap),
 * so lane A's rule (2) "navigation + activate in the same frame = nothing"
 * cannot swallow the driver's press.
 *
 * MPP_FOCUS_UNKNOWN -- "not observed" -- on every OTHER screen, rather than a
 * confident MPP_FOCUS_OTHER: there is no observer for those, and the core's
 * rejoin phases only ever act on the two named answers anyway. */
static enum mpp_focus mpp_hook_menu_focus(void* user) {
	const struct screen* sc = gstate.players[0].current_screen;
	(void)user;
	if(sc == &screen_title)
		return screen_title_focus_is_multiplayer() ? MPP_FOCUS_TITLE_MULTIPLAYER
												   : MPP_FOCUS_OTHER;
#ifdef CAVEWII_MP_CLIENT
	/* Through the registered POINTER, which is NULL until main registers the MP
	 * pool's screen above -- the same NULL-until-registered shape every other
	 * reader of it uses. screen_mps_focus_is_join() itself lives in
	 * screen_multiplayer_setup.c (CAVEX_MP_SOURCES), which is why the call is
	 * inside the same guard that registers the pointer. */
	if(screen_multiplayer_setup && sc == screen_multiplayer_setup)
		return screen_mps_focus_is_join() ? MPP_FOCUS_MPS_JOIN : MPP_FOCUS_OTHER;
#endif
	return MPP_FOCUS_UNKNOWN;
}

/* THE Beta blockExists gate (source/entity/entity_local_player.h), not a copy
 * of it: "has the local player's body been released to tick at all". The
 * driver must never re-derive this -- two copies of the chunkExists gate is
 * the drift the F8b ruling closed, and a copy that disagreed would report a
 * body ticking that is frozen, or the reverse. No body -> not ticking, which
 * the core reads as "not observed" and waits on. */
static bool mpp_hook_body_ticking(void* user) {
	(void)user;
	const struct entity* body = gstate.players[0].local_player;
	if(!body || !body->world)
		return false;
	return local_player_column_present(body->world,
									   (w_coord_t)floorf(body->pos[0]),
									   (w_coord_t)floorf(body->pos[2]));
}

/* P4 arms the EXISTING named-autoload hook, once. The profile deliberately
 * sets neither autoload_new nor autoload_name, so screen_select_world.c
 * process-lifetime latch is still unconsumed here and this is its first and
 * only use in the run. */
static bool sr_hook_arm_autoload(void* user, const char* directory) {
	(void)user;
	if(!hw_run_config_set_string(&gstate.config_user, "autoload", directory))
		return false;
	screen_set(&screen_select_world);
	return true;
}

#ifdef PLATFORM_WII
int usleep(useconds_t); // newlib hides it under -std=c99 (gfx.c/watchdog.c declare it too)
// Pre-FAT boot-phase paint (RCA 2026-08-25, docs/parity: chainloaded boots died
// inside the UNRETRIED fatInitDefault with zero observable output -- no SD, no
// VIDEO_Init, no watchdog thread exist yet, so the ONLY working output channel
// this early is the XFB the chainloader/HBC is still scanning out. The live
// framebuffer address comes from the VI TFBL register directly because libogc's
// VIDEO_GetCurrentFramebuffer() reads this process's own (still-NULL) state
// pre-VIDEO_Init. Wrong stride just dithers the band -- still visible. Bands,
// top down: slot 0 = main() entered; slot 1 = about to mount FAT; slot 2 =
// mount verdict (green ok / red = a failed attempt, repainted green on a later
// success). A wedge leaves the last band standing for the human at the TV.
static void boot_phase_paint(uint32_t yuyv, int slot) {
	uint32_t tfbl = *(volatile uint32_t*)0xCC00201C;
	uint32_t phys = (tfbl & 0x00FFFFFFu) << 5;
	bool mem1 = phys && phys < 0x01800000u;
	bool mem2 = phys >= 0x10000000u && phys < 0x14000000u;
	if(!mem1 && !mem2)
		return; // implausible scanout base: no VI running (bare boot) -- skip
	volatile uint32_t* fb = (volatile uint32_t*)(phys | 0xC0000000u);
	for(int y = slot * 24; y < (slot + 1) * 24; y++)
		for(int x = 0; x < 320; x++) // 640px assumed; two YUYV px per u32
			fb[y * 320 + x] = yuyv;
}
#define BOOT_PAINT_BLUE 0x29F0296Eu
#define BOOT_PAINT_YELLOW 0xD210D292u
#define BOOT_PAINT_GREEN 0x91369122u
#define BOOT_PAINT_RED 0x4C544CFFu
#endif

/* Container presentation follows the existing body admission. Unsupported
 * global screens retain their existing single-view path until owner-local. */
static bool local_respawn_presentation(const struct client_session* guest) {
 return guest&&guest->local_bound&&guest->local_ticket&&guest->local_generation
  &&guest->local_phase==CLIENT_GUEST_ATTACHED&&guest->local_respawn_loading
  &&guest->local_respawn_epoch>guest->local_respawn_completed_epoch
  &&guest->local_player;
}
static bool local_session_gameplay_active(const struct client_session* owner,
 unsigned player) {
 return player==0?client_session_host_gameplay_active(owner)
  :client_session_local_gameplay_active(owner);
}
/* A hold BEGIN may drain with its END in one frame. Keep the accepted request
 * identity until the corresponding owner's body and gameplay resume. */
static bool s_guest_transfer_pane;
static uint64_t s_guest_transfer_generation;
static uint32_t s_guest_transfer_request_seen;
static void local_transfer_panes_update(void) {
  const struct client_session* guest=&gstate.players[1];
 if(!clin_world_payloads_enabled()||gstate.remote_session||!guest->local_bound
    ||guest->local_phase!=CLIENT_GUEST_ATTACHED||guest->local_reply_failed) {
  s_guest_transfer_pane=false;
  s_guest_transfer_generation=0;
  s_guest_transfer_request_seen=0;
  return;
 }
 if(s_guest_transfer_generation!=guest->local_generation) {
  s_guest_transfer_generation=guest->local_generation;
  s_guest_transfer_request_seen=0;
  s_guest_transfer_pane=false;
 }
 uint32_t request=clin_transfer_actor_request_id(1);
 if(request&&request!=s_guest_transfer_request_seen) {
  s_guest_transfer_request_seen=request;
  s_guest_transfer_pane=true;
  }
  else if(!clin_transfer_actor_held(1)
          &&guest->local_gameplay_ready&&!guest->local_respawn_loading
          &&guest->local_player)s_guest_transfer_pane=false;
}
static struct clin_frame_loading_policy local_frame_loading_policy(void) {
 const struct client_session* host=&gstate.players[0];
 const struct client_session* guest=&gstate.players[1];
 struct clin_frame_loading_state state={
  .payloads=clin_world_payloads_enabled(),
  .remote=gstate.remote_session,.paused=gstate.paused,
  .failed=gstate.world_load_failed,
  .host_transfer_episode=clin_host_transfer_pane_active(),
  .host_pending=gstate.world_switch_pending,
  .host_loading_screen=host->current_screen==&screen_load_world,
  .host_ingame_screen=host->current_screen==&screen_ingame,
  .host_death_screen=host->current_screen==&screen_death,
  .host_loaded=gstate.world_loaded,
  .host_active=client_session_host_gameplay_active(host),
  .host_established=host->local_player!=NULL,
  .guest_transfer_episode=s_guest_transfer_pane,
  .guest_held=clin_transfer_actor_held(1),
  .guest_bound=guest->local_bound,
  .guest_attached=guest->local_phase==CLIENT_GUEST_ATTACHED,
  .guest_active=client_session_local_gameplay_active(guest),
  .guest_established=guest->local_bound&&guest->local_generation
    &&guest->local_phase==CLIENT_GUEST_ATTACHED&&!guest->local_reply_failed
    &&(guest->local_gameplay_ready||guest->death.active
      ||guest->local_respawn_loading),
  .guest_respawn_loading=guest->local_respawn_loading,
  .guest_view_bound=clin_world_view_slot(1)>=0,
 };
 return clin_frame_loading_policy(&state);
}
static void local_guest_transfer_loading_render(int width,int height,
 float elapsed_s,struct world* terrain) {
 lworld_render_loading_viewport(width,height,elapsed_s,
  terrain?world_loaded_chunks(terrain):0);
}
static bool local_render_other_player(unsigned viewport) {
 return viewport==1?client_session_host_gameplay_active(&gstate.players[0])
  :local_session_gameplay_active(&gstate.players[1],1);
}
static bool local_death_view(const struct client_session* owner,unsigned player,
  const struct clin_frame_loading_policy* loading) {
  bool dead=owner&&owner->local_player&&owner->local_player->health<=0;
  return loading&&player<2&&loading->death_view_allowed[player]&&owner
  &&(owner->death.active||dead||(player==1&&owner->local_respawn_loading));
}
/* Small production seam for the ordering-sensitive split modal arbitration.
 * Recovery owns the frame first; an existing menu/pause defers death; only the
 * affected owner loses capture. The seam performs the real update call. */
static bool local_death_arbitrate(struct client_session* owner,unsigned player,
 const struct input_player_frame* frame,struct split_layout_rect rect,
 bool death_view,bool* pointer) {
 if(death_view&&local_menu_host_recovery_frame(owner,player,frame,0,rect.y,
    gfx_width(),240)) {
  if(owner->local_player)owner->local_player->data.local_player.capture_input=false;
  *pointer=true;return true;
 }
 if(player==1&&!local_session_gameplay_active(owner,player)&&!death_view) {
  if(owner->local_player)owner->local_player->data.local_player.capture_input=false;
  return true;
 }
 if(death_view&&!local_menu_host_active(owner)&&!local_pause_active(&owner->pause)) {
  if(owner->local_player)owner->local_player->data.local_player.capture_input=false;
  if(player==0)screen_death_update_viewport(gstate.stats.dt,gfx_width(),rect.height);
  else (void)screen_death_owner_update(owner,player,frame,gstate.stats.dt,
    0,rect.y,gfx_width(),rect.height);
  *pointer=true;return true;
 }
 return false;
}

static void local_render_other_player_draw(unsigned viewport,
 struct camera* camera,float tick_delta) {
 if(local_render_other_player(viewport)) {
  struct client_session* partner=&gstate.players[1-viewport];
  entity_local_player_render_other(partner,camera->view,tick_delta);
  if(clin_local_body_fire_visible(partner))
   render_entity_fire_draw(partner->local_player,camera,tick_delta);
 }
}
static unsigned local_container_views(bool* converted, bool transitioning) {
 *converted=false;
 struct client_session* primary=&gstate.players[0];
 struct client_session* guest=&gstate.players[1];
 struct clin_frame_loading_policy loading=local_frame_loading_policy();
 if(gfx_height()>=480&&loading.keep_split)
  return 2;
 bool admitted=guest->local_bound
  &&(guest->local_gameplay_ready||local_respawn_presentation(guest))
  &&guest->local_player&&gfx_height()>=480;
 if(admitted&&!gstate.paused&&lworld_admit_gameplay()) {
  if(primary->current_screen==&screen_death)return 2;
  if(primary->current_screen==&screen_ingame)return 2;
  if(primary->current_screen==&screen_creative_picker
   &&local_menu_host_open_creative(primary,0,gfx_width(),240)) {
   screen_creative_picker_to_compact(&primary->menu_host.picker);
   primary->current_screen=&screen_ingame;
   if(primary->local_player)primary->local_player->data.local_player.capture_input=false;
   input_pointer_enable(true);*converted=true;return 2;
  }
  struct screen* current=primary->current_screen;
  bool inventory=current==&screen_inventory;
  bool supported=inventory||current==&screen_crafting||current==&screen_chest
   ||current==&screen_furnace||current==&screen_dispenser||current==&screen_iron_chest||current==&screen_sign;
  int window=inventory?WINDOWC_INVENTORY:primary->visible_window;
  if(supported&&window>=0&&window<=UINT8_MAX&&screen_split_container_base((uint8_t)window,gfx_width(),gfx_height()/2)) {
   *converted=true;return 2;
  }
 }
 if(!transitioning&&local_menu_host_active(primary)&&primary->menu_host.split_owned
  &&primary->current_screen==&screen_ingame&&gstate.world_loaded&&!gstate.world_switch_pending&&!gstate.paused&&lworld_admit_gameplay()) {
  if(primary->menu_host.mode==LOCAL_MENU_CREATIVE) {
   struct creative_picker picker=primary->menu_host.picker;
   local_menu_host_reset(primary);
   screen_set(&screen_creative_picker);
   screen_creative_picker_from_compact(&picker);
  } else {
   uint8_t window=primary->menu_host.window;bool valid=primary->windows[window]==primary->menu_host.window_identity;
   int visible=primary->visible_window;local_menu_host_reset(primary);
   if(valid&&clin_show_window(window))primary->visible_window=visible;
   else primary->visible_window=-1;
  }
  *converted=true;
 }
 if(local_menu_host_active(primary)&&primary->menu_host.split_owned&&primary->local_player)
  primary->local_player->data.local_player.capture_input=false;
 if(local_menu_host_active(&gstate.players[1])&&gstate.players[1].local_player)
  gstate.players[1].local_player->data.local_player.capture_input=false;
 return 1;
}
static bool local_container_send(void* user,struct server_rpc* rpc) {
 return svin_rpc_send_from(*(const struct server_rpc_source*)user,rpc);
}

static void retained_stats_answer(void* user, enum stats_exit_result result) {
 unsigned mask = (unsigned)(uintptr_t)user;
 if(result == STATS_EXIT_WITHOUT_SAVE)
  for(unsigned i = 0; i < 2; i++) if(mask & (1u << i)) achievements_request_rescue(i);
}

static void local_pause_stats_saved(void* user, enum stats_exit_result result) {
 struct client_session* owner = user;
 if(result == STATS_EXIT_BACK) { owner->pause.exit_pending = owner->pause.exit_failed = false; return; }
 if(result == STATS_EXIT_WITHOUT_SAVE)
  for(unsigned i = 0; i < 2; i++) achievements_request_rescue(i);
 const uint32_t vs_order = vsp_dispatch_reserve();
 bool sent = svin_rpc_send(&(struct server_rpc){.type = SRPC_UNLOAD_WORLD});
 if(sent) vsp_dispatch_sent(VSP_UI_SAVE_EXIT, vs_order);
 if(!sent) local_pause_host_exit_settled(&owner->pause, false);
 else if(split_portal_quit_armed()) {
  s_spq_unload_requests++;
  split_portal_quit_confirm_sent(split_recovery_sim_ticks(), hw_telemetry_portal_quit_up_ms());
  if(s_spq_server && __atomic_load_n(&s_spq_server->world_switch_phase, __ATOMIC_RELAXED) != SERVER_WORLD_SWITCH_IDLE)
   s_spq_request_after_switch = true;
 }
}

static bool local_pause_frame(struct client_session* owner,unsigned player,
 const struct input_player_frame* frame,int y,int height) {
 if(!frame)return local_pause_active(&owner->pause);
 if(!local_pause_active(&owner->pause)) {
  if(!frame->connected||!frame->pressed[IB_HOME])return false;
  local_pause_open(&owner->pause,player==0);return true;
 }
 enum local_pause_result result=local_pause_update(&owner->pause,owner,frame,0,y,gfx_width(),height);
	if(result==LOCAL_PAUSE_HOST_EXIT) {
  /* Save and quit is a REQUEST, not a transition (the 2026-09-11 single-player
   * finding, screen_pause.c): the server can refuse the unload (inventory close,
   * checkpoint stage, unsettled guest) and answers RETURN_INGAME with a "Save
   * failed: <stage>" toast, or WORLD_RESET(local_entity 0) when the world is
   * really gone. client_interface settles this pause on either reply and picks
   * the title itself. Only a refused ENQUEUE settles failure here (retry stays
   * one click away); a queued request leaves exit_pending set. */
	  /* O11 / S03 #2: the armed vehicle smoke's Save receipt is recorded at
	   * THIS accepted send, ordered before the server's checkpoint. */
	  screen_stats_save_open(player, 3u, local_pause_stats_saved, owner);
 }
 if(result==LOCAL_PAUSE_BACK||result==LOCAL_PAUSE_HOST_EXIT)
  for(int b=0;b<IB_COUNT;b++)if(frame->down[b]||frame->pressed[b]||frame->released[b])input_player_block(player,b);
 return true;
}
/* Save-path attribution census (2026-09-11): the journaled-publication
 * counters used to reach only the shutdown stderr line, which no Wii run
 * captures. One observer line every 5 s carries all 32 slots, cumulative; the
 * analysis differences consecutive lines. "us":0 says the microsecond slots
 * (12, 19, 20, 24-27, 29) are unmeasured on this build (PC, or the Wii
 * profile off) -- they must not be read as zero cost then. */
/* G9 R19: "rio" = [deferred save IO on (experimental.g9_save_io_int), then
 * region_archive_deferred_stats slots 0..11 -- ops, writes, publishes, syncs,
 * backpressure waits, barrier waits, barrier wait us (profile on), failed,
 * peak queued bytes, peak incomplete ops, read-after-write hits, locked
 * reads], cumulative like the rest of the line. */
static bool hw_emit_region_journal(uint32_t tick) {
	uint32_t rj[24], rx[8], rio[REGION_DEFERRED_STATS], ckio[SERVER_CHECKPOINT_STATS];
	region_archive_journal_stats(rj);
	region_archive_journal_stats_ex(rx);
	region_archive_deferred_stats(rio);
	server_world_checkpoint_stats(ckio);
	char line[1280];
	int n = snprintf(line, sizeof(line),
		"{\"event\":\"region_journal\",\"tick\":%u,\"us\":%d,"
		"\"publishes\":%u,\"chunks\":%u,\"sectors\":%u,\"journals\":%u,"
		"\"fsyncs\":%u,\"site\":[%u,%u,%u,%u,%u],"
		"\"preserve\":[%u,%u,%u],\"jhandles\":[%u,%u],"
		"\"read_backs\":%u,\"canon\":[%u,%u],\"payload_bytes\":%u,"
		"\"io\":[%u,%u,%u,%u,%u],"
		"\"commit_write_us\":%u,\"inline_pub_us\":%u,\"io_wait_us\":%u,"
		"\"index_read_us\":%u,\"inline_pubs\":%u,\"rebuild_us\":%u,"
		"\"rio\":[%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u],"
		"\"ckio\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]}",
		tick, region_archive_work_profile_enabled() ? 1 : 0, rj[0], rj[1],
		rj[2], rj[3], rj[4], rj[5], rj[6], rj[7], rj[8], rj[9], rj[10], rj[11],
		rj[12], rj[13], rj[14], rj[15], rj[16], rj[17], rj[18], rj[19], rj[20],
		rj[21], rj[22], rj[23], rx[0], rx[1], rx[2], rx[3], rx[4], rx[5],
		region_archive_deferred_io_enabled() ? 1 : 0, rio[0], rio[1], rio[2],
		rio[3], rio[4], rio[5], rio[6], rio[7], rio[8], rio[9], rio[10],
		rio[11], ckio[0], ckio[1], ckio[2], ckio[3], ckio[4], ckio[5],
		ckio[6], ckio[7], ckio[8], ckio[9], ckio[10], ckio[11]);
	if(n <= 0 || (size_t)n >= sizeof(line))
		return false;
	return hw_telemetry_emit_observer(line);
}

static void render_first_person_fire(const struct camera* camera,
										int64_t anim_elapsed_ms) {
	/* ItemRenderer.renderFireInFirstPerson [b1.7.3]: the two opposed quads
	 * remain in perspective after the hand/world pass and before water/HUD.
	 * Quad 0 samples Beta's fire cell, quad 1 its fire+16 cell (ItemRenderer
	 * :385): anim.png bakes the two TextureFlamesFX instances as separate
	 * strips and fire_overlay_quad picks the strip per quad. */
	static const float vertices[12] = {
		-0.5f, -0.5f, -0.5f,  0.5f, -0.5f, -0.5f,
		 0.5f,  0.5f, -0.5f, -0.5f,  0.5f, -0.5f,
	};
	static const uint8_t colors[16] = {
		255, 255, 255, FIRE_OVERLAY_ALPHA,
		255, 255, 255, FIRE_OVERLAY_ALPHA,
		255, 255, 255, FIRE_OVERLAY_ALPHA,
		255, 255, 255, FIRE_OVERLAY_ALPHA,
	};
	int frame = fire_overlay_frame_ms(anim_elapsed_ms);
	gfx_matrix_projection((vec4*)camera->projection, true);
	gfx_matrix_texture(false, NULL);
	gfx_texture(true);
	gfx_alpha_test(false);
	gfx_blending(MODE_BLEND);
	gfx_write_buffers(true, false, false);
	gfx_cull_func(MODE_NONE);
	gfx_bind_texture(&texture_anim);
	for(int side = 0; side < 2; side++) {
		struct fire_overlay_quad q = fire_overlay_quad(frame, side);
		mat4 model;
		glm_translate_make(model, (vec3){q.x_offset, -0.3f, 0.0f});
		glm_rotate_y(model, glm_rad(q.rotation_y_deg), model);
		gfx_matrix_modelview(model);
		gfx_draw_quads_flt(4, vertices, colors, q.uv);
	}
	/* gfx_mode_gui_viewport is the next call: it restores the orthographic
	 * projection, identity modelview, alpha test, blend and depth state.
	 * Restore culling explicitly because the GUI mode does not own it. */
	gfx_cull_func(MODE_BACK);
}

/* --cw-pin-cameras (debug capture lever, hw_run.h): a fixed render pose per
 * view above the world spawn, a fixed clock (6000 ticks, no interpolation
 * drift) and clear client weather, so two builds' frames are pixel-
 * comparable regardless of spawn fuzz, body motion, time and weather. The
 * bodies, the server and the input are untouched. */
static struct camera* pin_capture_camera(const struct camera* src,
										 unsigned view) {
	/* A render COPY: the player's own camera (look, input, autopilot
	 * steering) is never written. The pose is latched from the view's first
	 * loaded frame (block-rounded, deterministic for a seed) and then held,
	 * with a fixed yaw and a slightly downward pitch. */
	static struct camera pinned[2];
	static bool latched[2];
	static float px[2], py[2], pz[2];
	unsigned v = view & 1u;
	if(!latched[v]) {
		px[v] = floorf(src->x) + 0.5F;
		py[v] = floorf(src->y) + 0.5F;
		pz[v] = floorf(src->z) + 0.5F;
		latched[v] = true;
	}
	struct camera* c = &pinned[v];
	*c = *src;
	c->x = px[v];
	c->y = py[v];
	c->z = pz[v];
	c->rx = 0.0F;
	c->ry = glm_rad(100.0F);
	c->bob_active = false; /* no walk bob from the still-scripted body */
	return c;
}

static void pin_capture_clock(ptime_t now) {
	gstate.world_time = 6000;
	gstate.world_time_start = now;
	gstate.weather_raining = gstate.weather_thundering = false;
	gstate.weather_rain_strength = gstate.weather_prev_rain_strength = 0.0F;
	gstate.weather_thunder_strength = gstate.weather_prev_thunder_strength
		= 0.0F;
}

/* Self-test rows use a separate event; heartbeat profile gates stay intact. */
static struct audio_service_witness g9_audio_witness;
static bool g9_audio_wait_guard(uint32_t proposed_wait_us, uint32_t remaining_service_us) {
	/* Waiting larger than the unchanged frame allowance remains unadmitted.
	 * S7 owns the cumulative wait integration; this guard cannot enlarge it. */
	if(proposed_wait_us > 4000) return false;
	struct audio_service_budget b;
	audio_service_get_budget(&b, remaining_service_us);
	uint32_t reserve;
	return audio_service_wait_reserve_budget(&g9_audio_witness, &b, proposed_wait_us, &reserve);
}
static void g9_lease_audio_publish(uint32_t remaining_service_us) {
	if(!thread_g9_lease_local_present() && !g9_capture_enabled()) return;
	bool worker;
	uint32_t reserve = 0;
	if(thread_g9_handoff_enabled()) {
		struct audio_service_witness w = {0};
		struct audio_service_budget b;
		audio_service_get_budget(&b, remaining_service_us);
		bool valid = false;
		if(audio_service_enter(&b, AUDIO_UNIT_STATE) == AUDIO_SERVICE_DONE) {
			ptime_t entered = time_get();
			valid = audio_music_service_witness(&w);
			valid = audio_service_leave(&b, AUDIO_UNIT_STATE, entered) && valid;
		}
		worker = !valid || !b.limits->margins_verified
			|| !audio_service_reserve_aged(&w, &w, b.limits, time_get(), 4000, &reserve);
		g9_audio_witness = w;
		thread_g9_handoff_audio_guard(g9_audio_wait_guard);
		if(worker) reserve = 0;
	} else reserve = audio_music_lease_reserve_us(&worker);
	/* ON evidence already used the guarded deferred snapshot; do not follow
	 * it with an unbudgeted blocking deferred-state query. OFF is unchanged. */
	enum audio_deferred_state fx = thread_g9_handoff_enabled()
		? AUDIO_DEFERRED_UNAVAILABLE : audio_deferred_state();
	g9_capture_audio(reserve, worker, (uint32_t)fx);
	thread_g9_lease_audio(reserve, worker || fx == AUDIO_DEFERRED_REQUESTED
		|| fx == AUDIO_DEFERRED_LOADING);
}
static bool g9_audio_bounded_service(void) {
	struct audio_service_budget b;
	audio_service_get_budget(&b, thread_g9_handoff_audio_remaining());
	enum audio_service_result r = audio_update_service((vec3){gstate.players[0].camera.x,
		gstate.players[0].camera.y, gstate.players[0].camera.z},
		glm_deg(gstate.players[0].camera.rx), &b);
	if(r == AUDIO_SERVICE_DONE) r = audio_music_update_service(&b);
	return r == AUDIO_SERVICE_DONE && b.limits->margins_verified && !b.calibration;
}
/* R18's ONE shared calibration is armed by its existing run identity, with
 * both waiting knobs OFF and capture CLOSED. Cumulative counters ride the
 * existing telemetry transport at heartbeat cadence; no new runner/profile.
 * This path NEVER acknowledges AUDIO or installs an engineering allowance. */
static bool g9_audio_calibration_service(const char* run_id, const char* profile,
	bool waiting_off) {
	if(!waiting_off || !run_id || strcmp(run_id, "G9_R18_native_bounds")
		|| !profile || strcmp(profile, "perf_stress_5m") || g9_capture_enabled()) return false;
	struct audio_service_budget b;
	audio_service_get_budget(&b, 500);
	b.calibration = true;
	enum audio_service_result r = audio_update_service((vec3){gstate.players[0].camera.x,
		gstate.players[0].camera.y, gstate.players[0].camera.z},
		glm_deg(gstate.players[0].camera.rx), &b);
	if(r == AUDIO_SERVICE_DONE) r = audio_music_update_service(&b);
	/* Exercise the same composite native witness snapshot used at wait
	 * entry, WITHOUT calling the wait predicate or installing margins. */
	if(audio_service_enter(&b, AUDIO_UNIT_STATE) == AUDIO_SERVICE_DONE) {
		ptime_t entered = time_get();
		struct audio_service_witness w;
		bool valid = audio_music_service_witness(&w);
		audio_service_cover(&b, AUDIO_UNIT_STATE, 0, 32u | (valid ? (w.inactive ? 64u : 128u) : 256u));
		(void)audio_service_leave(&b, AUDIO_UNIT_STATE, entered);
	}
	static ptime_t emitted;
	static bool have_emitted;
	if(!have_emitted || time_diff_us(emitted, time_get()) >= 1000000u) {
		struct audio_service_metrics m;
		audio_service_get_metrics(&m);
		char line[1024];
#ifdef PLATFORM_WII
		unsigned native = 1;
#else
		unsigned native = 0;
#endif
		int n = snprintf(line, sizeof line,
			"{\"event\":\"g9_audio_bounds\",\"run_id\":\"G9_R18_native_bounds\","
			"\"build\":\"%s\",\"native\":%u,\"result\":%u,\"units\":[", CW_BUILD_ID, native, (unsigned)r);
		for(unsigned k = 0; k < AUDIO_UNIT_COUNT && n > 0 && (size_t)n < sizeof line; k++) {
			int added = snprintf(line+n, sizeof line-(size_t)n, "%s[%u,%u,%u,%u,%u]",
				k ? "," : "", m.maximum_us[k], m.count[k], m.overruns[k],
				m.maximum_bytes[k], m.coverage[k]);
			if(added < 0) { n = -1; break; }
			n += added;
		}
		if(n > 0 && (size_t)n+3 < sizeof line) {
			int added = snprintf(line+n, sizeof line-(size_t)n,
				"],\"bookkeeping\":[%u,%u]}\n", m.bookkeeping_maximum_us, m.bookkeeping_count);
			if(added < 0 || (size_t)added >= sizeof line-(size_t)n) return r == AUDIO_SERVICE_DONE;
			/* A refused snapshot is re-offered next frame (latest metrics), so
			 * a full ring defers it -- it must not count as a drop each frame
			 * (real-Wii split_offer_piston_2m tel_drop=14 at boot). */
			if(hw_telemetry_emit_observer_retry(line)) { emitted = time_get(); have_emitted = true; }
		}
	}
	return r == AUDIO_SERVICE_DONE; /* service only, ALWAYS zero wait credit */
}
static void g9_handoff_audio_service(void) {
	struct g9_lease_token token = thread_g9_handoff_work_begin(G9_HANDOFF_AUDIO);
	if(!token.generation) return;
	g9_capture_main(12, true, 0);
	bool complete = g9_audio_bounded_service();
	g9_capture_main(12, false, 1);
	g9_lease_audio_publish(thread_g9_handoff_audio_remaining());
	thread_g9_handoff_work_end(token, G9_HANDOFF_AUDIO, complete);
	if(!complete) {
		thread_g9_handoff_audio_abort();
		/* Unsupported/expired admission cannot starve actual refill. No
		 * credit is given for this ordinary escape, even on inactive audio. */
		audio_update((vec3){gstate.players[0].camera.x, gstate.players[0].camera.y,
			gstate.players[0].camera.z}, glm_deg(gstate.players[0].camera.rx));
		audio_music_update();
	}
}
static void g9_frame_input_poll(bool split_experimental) {
	struct g9_lease_token token = thread_g9_handoff_work_begin(G9_HANDOFF_INPUT);
	input_split_join_arm(split_experimental && gstate.world_loaded
		&& !screen_options_split_off_blocks_join()
		&& !gstate.world_switch_pending && gstate.players[0].local_player);
	g9_capture_main(11, true, 0);
	input_poll();
	g9_capture_main(11, false, 1);
	thread_g9_handoff_work_end(token, G9_HANDOFF_INPUT, true);
}
static uint32_t g9_service_remaining(ptime_t start, uint32_t budget) {
	uint64_t used = time_diff_us(start, time_get());
	return used < budget ? budget-(uint32_t)used : 0;
}
static void g9_lease_main_service(struct g9_lease_token token, ptime_t start,
	uint32_t budget) {
	if(!thread_g9_lease_local_present()) return;
	bool complete = true;
	if(token.generation) {
		/* Only here: this frame's last draw batch and real input poll have
		 * finished. R19 never retires a mesh from inside a lighting flood or
		 * GX draw batch. L205 fences the submitted lists' backing storage. */
		complete = clin_update_drain_budget(g9_service_remaining(start, budget));
		if(complete) complete = chunk_mesher_receive_budget(
			g9_service_remaining(start, budget));
		if(complete && g9_service_remaining(start, budget)) {
			g9_capture_main(12, true, 0);
			audio_update((vec3){gstate.players[0].camera.x,
				gstate.players[0].camera.y, gstate.players[0].camera.z},
				glm_deg(gstate.players[0].camera.rx));
			audio_music_update();
			g9_capture_main(12, false, 1);
		} else complete = false;
	}
	g9_lease_audio_publish(500);
	(void)thread_g9_lease_service_end(token,
		(uint32_t)time_diff_us(start, time_get()), complete);
}

static const char* g9_diag_run_id;
static bool emit_g9_diagnostic(const char* fields) {
	char line[800];
	int n = snprintf(line, sizeof line,
		"{\"event\":\"g9_drain_diag\",\"run_id\":\"%s\",%s",
		g9_diag_run_id, fields);
	return n > 0 && (size_t)n < sizeof line
		&& hw_telemetry_emit_observer_retry(line);
}

/* ---- BG-BAKE-1028 task 2: menu-backdrop scene bake host -----------------
 * The renderer side of game/gui/backdrop_bake.c (which owns the storage-free
 * scene columns and the crop/scale). A bake runs only at the title with no
 * client/server world alive, into its OWN scratch struct world: the columns
 * it installs, the meshes the shared mesher builds for them, and the one
 * frame it draws all belong to that world and are released by scene_end.
 * The recipe's view globals (fov, fog/render distance, fog clip, LOD near
 * radius, world clock) are applied only for the duration of each host call
 * and restored before it returns, so no menu frame or later world load ever
 * sees them. The draw reuses the in-game pass in its order (sky box,
 * opaque terrain, fancy-liquid pass, clouds) with HUD, hand, entities,
 * particles and weather omitted; liquid and cloud phase are frozen (anim
 * timer = draw time -> frame 0; cloud clock = world_time 6000 at zero
 * elapsed). Telemetry: one {"event":"backdrop_bake"} observer line per state
 * change with the memory ledger (FEASIBILITY blocker 1). */
struct bake_host_state {
	struct world scene_world;
	bool world_live;
	bool pre_rendered;
	unsigned ready_streak;
	struct camera cam;
	const struct backdrop_bake_recipe* recipe;
	/* saved view globals while a host call runs */
	float saved_fov, saved_render, saved_fog, saved_fog_clip;
	int saved_lod_near;
	uint64_t saved_world_time;
	ptime_t saved_world_time_start;
	/* memory ledger */
	size_t world_mem1_peak, world_mem2_peak, mesh_peak;
	size_t mem1_free_min, carve_free_min, carve_largest_min;
	bool mem_sampled;
	enum backdrop_bake_state last_state;
	bool last_state_valid;
	bool selftest_pending, selftest_owned;
	bool retire_pending; /* scene ended, world kept until the mesher drains */
	/* review-2 #2: the user's lod.geomip state, frozen OFF (the recipe's,
	 * Astra's capture default) from scene_begin until the scratch world is
	 * retired -- i.e. after every mesh job that could read it has landed. */
	bool geomip_saved, geomip_enabled;
	float geomip_budget;
	unsigned frames;
	/* BOOT-CREDITS-1028 T5: settle split (telemetry) and the fast path.
	 * legacy = debug.backdrop_bake_legacy=1: the pre-T5 settle (8 ms per
	 * frame, repair-only light loop, meshing from the first step), kept for
	 * the byte-identity A/B. */
	bool legacy;
	uint32_t budget_override_us; /* debug.backdrop_bake_budget_us (0 = off) */
	uint64_t light_us, vis_us, build_us;
	unsigned light_calls, mesh_deferred, budget_max_us;
};
static struct bake_host_state s_bake;

static void bake_apply_view(void) {
	const struct backdrop_bake_recipe* r = s_bake.recipe;
	s_bake.saved_fov = gstate.config.fov;
	s_bake.saved_render = gstate.config.render_distance;
	s_bake.saved_fog = gstate.config.fog_distance;
	s_bake.saved_fog_clip = world_get_fog_clip();
	s_bake.saved_lod_near = world_lod_near_radius();
	s_bake.saved_world_time = gstate.world_time;
	s_bake.saved_world_time_start = gstate.world_time_start;
	gstate.config.fov = r->fov_deg;
	gstate.config.render_distance = r->render_distance;
	gstate.config.fog_distance = r->fog_distance;
	world_set_fog_clip(r->fog_clip);
	world_lod_set_near_radius(r->lod_near_radius);
}

static void bake_restore_view(void) {
	gstate.config.fov = s_bake.saved_fov;
	gstate.config.render_distance = s_bake.saved_render;
	gstate.config.fog_distance = s_bake.saved_fog;
	world_set_fog_clip(s_bake.saved_fog_clip);
	world_lod_set_near_radius(s_bake.saved_lod_near);
	gstate.world_time = s_bake.saved_world_time;
	gstate.world_time_start = s_bake.saved_world_time_start;
}

/* Ownership facts main() publishes each frame before the bake service
 * (its `server` / `mp_provider_remote` are main()-local). */
static bool s_bake_remote_provider, s_bake_server_owns_world;
/* Boot credits or an opaque pack Apply: a front-end load, never a world. */
static bool s_bake_loading_frontend;

static bool bake_host_world_alive(void* user) {
	(void)user;
	const struct backdrop_bake_world_facts f = {
		.remote_provider = s_bake_remote_provider || gstate.remote_session,
		.server_owns_world = s_bake_server_owns_world,
		.client_sections = world_loaded_chunks(&gstate.world),
		.at_title = gstate.players[0].current_screen == &screen_title,
		.loading_frontend = s_bake_loading_frontend,
	};
	return backdrop_bake_world_alive(&f);
}

static bool bake_host_scene_begin(void* user,
	const struct backdrop_bake_recipe* r) {
	(void)user;
	if(s_bake.world_live) return false;
	s_bake.recipe = r;
	world_create(&s_bake.scene_world);
	s_bake.scene_world.dimension = r->dimension;
	s_bake.world_live = true;
	s_bake.pre_rendered = false;
	s_bake.ready_streak = 0;
	s_bake.cam = (struct camera) {.x = r->eye[0], .y = r->eye[1],
		.z = r->eye[2], .rx = glm_rad(r->yaw_deg), .ry = glm_rad(r->pitch_deg)};
	bake_apply_view();
	bool ok = camera_update_viewport(&s_bake.cam, false, r->frame_w, r->frame_h);
	bake_restore_view();
	if(!ok) { /* nothing meshed yet: no job can hold a ref */
		world_destroy(&s_bake.scene_world);
		s_bake.world_live = false;
		return false;
	}
	/* Freeze geomip OFF for the scene's whole life (the request snapshots
	 * the flag; with it off the worker never reads budget/k). */
	s_bake.geomip_saved = true;
	s_bake.geomip_enabled = world_lod_geomip_enabled();
	s_bake.geomip_budget = world_lod_geomip_error_budget_px();
	world_lod_set_geomip(false, s_bake.geomip_budget, gstate.config.fov,
						 (float)gfx_height());
	return true;
}

/* Restore the user's geomip exactly as main() configured it at startup
 * (same setter inputs: the user's FOV and the screen height). */
static void bake_geomip_restore(void) {
	if(!s_bake.geomip_saved) return;
	world_lod_set_geomip(s_bake.geomip_enabled, s_bake.geomip_budget,
						 gstate.config.fov, (float)gfx_height());
	s_bake.geomip_saved = false;
}

static bool bake_host_install(void* user, int cx, int cz, const uint8_t* ids,
	const uint8_t* metadata, const uint8_t* sky, const uint8_t* torch,
	const uint8_t* heightmap, const uint8_t* grass_i, const uint8_t* grass_j,
	bool visible) {
	(void)user;
	(void)visible; /* readiness walks what the frame actually draws */
	if(!s_bake.world_live) return false;
	if(!world_build_full_section(&s_bake.scene_world, cx * CHUNK_SIZE, cz * CHUNK_SIZE,
		   ids, metadata, sky, torch, heightmap, grass_i, grass_j))
		return false;
	/* Settle cross-column light to its fixed point (the lossless client
	 * repair) before anything is meshed for capture. */
	world_light_repair_mark(&s_bake.scene_world, cx, cz);
	return true;
}

static void bake_mem_sample(bool largest) {
	if(s_bake.world_live) {
		struct world_memory wm;
		world_memory_usage(&s_bake.scene_world, &wm);
		if(wm.mem1_bytes > s_bake.world_mem1_peak) s_bake.world_mem1_peak = wm.mem1_bytes;
		if(wm.mem2_bytes > s_bake.world_mem2_peak) s_bake.world_mem2_peak = wm.mem2_bytes;
		if(wm.mesh_bytes > s_bake.mesh_peak) s_bake.mesh_peak = wm.mesh_bytes;
	}
	size_t mem1_free = 0;
#ifdef PLATFORM_WII
	struct mallinfo mi = mallinfo();
	mem1_free = (size_t)mi.fordblks + (size_t)SYS_GetArena1Size();
#endif
	size_t carve_free = mem2_carve_free_bytes();
	if(!s_bake.mem_sampled || mem1_free < s_bake.mem1_free_min)
		s_bake.mem1_free_min = mem1_free;
	if(!s_bake.mem_sampled || carve_free < s_bake.carve_free_min)
		s_bake.carve_free_min = carve_free;
	if(largest) {
		size_t big = 0;
		if(mem2_carve_largest_allocatable_bytes(&big)
		   && (!s_bake.carve_largest_min || big < s_bake.carve_largest_min))
			s_bake.carve_largest_min = big;
	}
	s_bake.mem_sampled = true;
}

/* Every chunk the frame will draw holds its current mesh, nothing is queued
 * or in flight, and light has reached its fixed point. */
static bool bake_scene_complete(struct world* w) {
	if(world_light_repair_pending(w) || w->light_spill_active
	   || !stack_empty(&w->lighting_updates)
	   || !lighting_flood_queue_empty(&w->lighting_flood_queue))
		return false;
	if(chunk_mesher_pending())
		return false;
	size_t seen = 0;
	size_t cap = world_ilist_walk_cap_current(w);
	ilist_chunks_it_t it;
	ilist_chunks_it(it, w->render);
	while(!ilist_chunks_end_p(it)) {
		struct chunk* c = ilist_chunks_ref(it);
		if(++seen > cap) return false;
		if(c->rebuild_displist || c->mesh_in_flight || c->mesh_retry_skip_checks
		   || c->mesh_oom_streak)
			return false;
		ilist_chunks_next(it);
	}
	return seen > 0;
}

static enum backdrop_bake_settle bake_host_settle(void* user, uint32_t budget_us) {
	(void)user;
	if(!s_bake.world_live) return BACKDROP_BAKE_SETTLE_FAILED;
	struct world* w = &s_bake.scene_world;
	bake_apply_view();
	ptime_t start = time_get();
	if(budget_us > s_bake.budget_max_us) s_bake.budget_max_us = budget_us;
	/* Last step's render-list tokens (this runs after the frame's flip). */
	world_render_completed(w, s_bake.pre_rendered);
	s_bake.pre_rendered = false;
	/* T5: light runs to its fixed point BEFORE the first mesh, on all of its
	 * queues, inside the budget less the visibility reserve. Each section is
	 * then meshed once from final light instead of ~1.9x (hw1028c); the
	 * completion test is unchanged, so the captured frame is the same.
	 * Policy shared with test_backdrop_settle_equiv (backdrop_settle.h). */
	ptime_t t0 = time_get();
	s_bake.light_calls += backdrop_settle_light(w, start, budget_us,
		s_bake.legacy);
	ptime_t t1 = time_get();
	struct camera* cams[1] = {&s_bake.cam};
	world_update_lod_views(w, cams, 1);
	world_pre_render(w, &s_bake.cam, s_bake.cam.view);
	s_bake.pre_rendered = true;
	ptime_t t2 = time_get();
	if(!backdrop_settle_may_mesh(w, s_bake.legacy))
		s_bake.mesh_deferred++;
	else
		(void)world_build_chunks(w, CHUNK_MESHER_QLENGTH, true);
	ptime_t t3 = time_get();
	s_bake.light_us += time_diff_us(t0, t1);
	s_bake.vis_us += time_diff_us(t1, t2);
	s_bake.build_us += time_diff_us(t2, t3);
	bool complete = bake_scene_complete(w);
	bake_restore_view();
	if((++s_bake.frames % 60) == 0) bake_mem_sample(false);
	/* Two consecutive complete observations: a mesh landing can open new
	 * BFS paths, which the next pre_render must also see settled. */
	s_bake.ready_streak = complete ? s_bake.ready_streak + 1 : 0;
	return s_bake.ready_streak >= 2 ? BACKDROP_BAKE_SETTLE_READY
									: BACKDROP_BAKE_SETTLE_PENDING;
}

static bool bake_host_render_capture(void* user, backdrop_bake_rows_fn sink,
	void* sink_user) {
	(void)user;
	if(!s_bake.world_live) return false;
	struct world* w = &s_bake.scene_world;
	const struct backdrop_bake_recipe* r = s_bake.recipe;
	bake_apply_view();
	world_render_completed(w, s_bake.pre_rendered);
	s_bake.pre_rendered = false;
	world_pre_render(w, &s_bake.cam, s_bake.cam.view);
	s_bake.pre_rendered = true;
	if(!bake_scene_complete(w)) { /* never a partial frame */
		bake_restore_view();
		return false;
	}
	bake_mem_sample(true);
	ptime_t now = time_get();
	gstate.world_time = r->world_time;
	gstate.world_time_start = now; /* cloud clock: zero elapsed */
	float daytime = (float)(r->world_time % DAY_LENGTH_TICKS)
		/ (float)DAY_LENGTH_TICKS;
	vec3 top, bottom, atmos;
	/* Beta WorldProviderHell.func_4096_a: fog (0.2,0.03,0.03);
	 * use the existing dimension-aware engine colours and no Nether sky/clouds. */
	daytime_sky_colors_dimension(daytime, r->dimension, top, bottom, atmos);
	gfx_scissor(false, 0, 0, 0, 0);
	gfx_set_viewport(0, 0, gfx_width(), gfx_height());
	gfx_fog_color(atmos[0], atmos[1], atmos[2]);
	/* Open air first (the clear colour belongs to the menu frame). */
	gfx_mode_gui();
	gfx_texture(false);
	gfx_blending(MODE_OFF);
	gfx_alpha_test(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, gfx_width(), gfx_height(), atmos[0],
		atmos[1], atmos[2], 255);
	gfx_texture(true);
	gfx_mode_world();
	gfx_matrix_projection(s_bake.cam.projection, true);
	gfx_matrix_modelview(s_bake.cam.view);
	gfx_lighting(true);
	gfx_blending(MODE_BLEND);
	gfx_alpha_test(true);
	gfx_fog(false);
	gfx_update_light(daytime_light_sun_dimension(daytime, r->dimension),
		0.0F, 0.0F, world_dimension_light(w));
	if(r->dimension == WORLD_DIM_OVERWORLD) gutil_sky_box_owned(s_bake.cam.view, daytime_celestial_angle(daytime), top,
		bottom, &s_bake.cam);
	(void)world_render(w, &s_bake.cam, false);
	gfx_fog(true);
#ifdef GFX_FANCY_LIQUIDS
	w->anim_timer = time_get(); /* liquid animation frame 0 */
	(void)world_render(w, &s_bake.cam, true);
#endif
#ifdef GFX_CLOUDS
	if(r->dimension == WORLD_DIM_OVERWORLD) gutil_clouds_owned(s_bake.cam.view,
		daytime_brightness_dimension(daytime, r->dimension), &s_bake.cam,
		now);
#endif
	gfx_mode_gui();
	bool ok = gfx_capture_frame_rows(sink, sink_user);
	bake_restore_view();
	return ok;
}

/* Free the scratch world once no mesher job can still reference it. The
 * bake only runs with no other world alive, so the shared mesher's queue is
 * ours alone. Returns true when the world is gone. */
static bool bake_try_retire(uint32_t wait_ms) {
	if(!s_bake.world_live) return true;
	ptime_t deadline = time_add_ms(time_get(), wait_ms);
	for(;;) {
		chunk_mesher_receive();
		if(!chunk_mesher_pending()) break;
		if(time_reached(time_get(), deadline)) return false;
		thread_msleep(1);
	}
	/* No pre_render runs after scene_end: drain the GPU-busy list once
	 * more (a no-op after scene_end's own hand-off), then free. */
	world_render_completed(&s_bake.scene_world, false);
	world_destroy(&s_bake.scene_world);
	s_bake.world_live = false;
	s_bake.retire_pending = false;
	bake_geomip_restore(); /* no job can read it any more */
	return true;
}

static void bake_host_scene_end(void* user) {
	(void)user;
	if(!s_bake.world_live || s_bake.retire_pending) return;
	struct world* w = &s_bake.scene_world;
	bake_mem_sample(true);
	/* world_finish_local_teardown's order: fence the GPU, hand back this
	 * world's frame tokens, receive every outstanding mesher job (they hold
	 * chunk refs into this world) -- then free. A worker that does not drain
	 * within 500 ms keeps the world alive (retire_pending) and the frame
	 * service retries; nothing is freed under a live job. */
	gfx_wait_idle();
	world_render_completed(w, s_bake.pre_rendered);
	s_bake.pre_rendered = false;
	world_render_completed(w, false);
	if(!bake_try_retire(500)) {
		fprintf(stderr, "[bake] mesher drain incomplete after 500 ms; world retained until it drains\n");
		s_bake.retire_pending = true;
	}
}

static const char* bake_state_name(enum backdrop_bake_state s) {
	static const char* names[] = {"terrain", "populate", "install", "settle",
		"capture", "done", "failed", "cancelled"};
	return (unsigned)s < sizeof names / sizeof *names ? names[s] : "?";
}

static void bake_emit(struct backdrop_bake* b) {
	struct backdrop_bake_report rep;
	backdrop_bake_get_report(b, &rep);
	char line[1024];
	snprintf(line, sizeof line,
		"{\"event\":\"backdrop_bake\",\"v\":%d,\"state\":\"%s\",\"error\":%d,"
		"\"cols\":%u,\"visible\":%u,\"quads\":%u,\"installed\":%u,"
		"\"settle_steps\":%u,\"owned_bytes\":%lu,\"owned_peak\":%lu,"
		"\"world_mem1_peak\":%lu,\"world_mem2_peak\":%lu,\"mesh_peak\":%lu,"
		"\"mem1_free_min\":%lu,\"carve_free_min\":%lu,\"carve_largest_min\":%lu,"
		"\"phase_us\":[%u,%u,%u,%u,%u],\"step_max_us\":%u,"
		"\"settle_us\":[%llu,%llu,%llu],\"light_calls\":%u,"
		"\"mesh_deferred\":%u,\"budget_max_us\":%u,\"legacy\":%d}",
		BACKDROP_BAKE_RECIPE_VERSION, bake_state_name(rep.state),
		(int)rep.error, rep.columns_total, rep.columns_visible, rep.quads_total,
		rep.columns_installed, rep.settle_steps, (unsigned long)rep.owned_bytes,
		(unsigned long)rep.owned_peak_bytes,
		(unsigned long)s_bake.world_mem1_peak,
		(unsigned long)s_bake.world_mem2_peak, (unsigned long)s_bake.mesh_peak,
		(unsigned long)s_bake.mem1_free_min, (unsigned long)s_bake.carve_free_min,
		(unsigned long)s_bake.carve_largest_min, rep.phase_us[0],
		rep.phase_us[1], rep.phase_us[2], rep.phase_us[3], rep.phase_us[4],
		rep.step_max_us, (unsigned long long)s_bake.light_us,
		(unsigned long long)s_bake.vis_us, (unsigned long long)s_bake.build_us,
		s_bake.light_calls, s_bake.mesh_deferred, s_bake.budget_max_us,
		s_bake.legacy ? 1 : 0);
	printf("[bake] %s\n", line);
	if(s_bake.selftest_owned) fflush(stdout);
	(void)hw_telemetry_emit_observer(line);
}

/* BG-BAKE-1028/3: the pack screen's backdrop job reaches the bake and the
 * generated-backdrop cache through these hooks (texture.h entry points). */
void screen_texturepacks_set_backdrop_hooks(const struct backdrop_job_hooks* h);

static const char* bake_job_pack(void) {
	return config_read_path(&gstate.config_user, "paths.texturepack", "assets");
}
static bool s_bg_nether;
static struct tex_gfx* bake_job_texture(void) {
 return s_bg_nether ? &texture_nether_background : &texture_menu_background;
}
/* hw1102c: the job polls this every frame; re-hashing the cached 1.1 MB
 * PNG on the SD card each frame ran the Wii title at ~2 fps. Keep the beach
 * answer per pack until this job publishes or reloads. */
static char s_bg_state_pack[700];
static int s_bg_state_beach = -1;
static int bake_job_state(void) {
 const char* pack = bake_job_pack();
 if(s_bg_state_beach < 0 || strncmp(s_bg_state_pack, pack, sizeof(s_bg_state_pack))) {
  s_bg_state_beach = tex_scene_backdrop_state_of(pack, false);
  snprintf(s_bg_state_pack, sizeof(s_bg_state_pack), "%s", pack);
 }
 int beach = s_bg_state_beach;
 /* Optional portal art never holds boot credits or an opaque Apply. */
 s_bg_nether = beach < 3 && !s_bake_loading_frontend
     && gstate.players[0].current_screen == &screen_title;
#ifdef PLATFORM_WII
 /* hw1102c: on Broadway the Nether scene bake (Nether light drain is 10-15x
  * the Overworld's, tmp/jobs/nether-1030) ran for minutes at ~3 fps on the
  * title and held the bake world, so no world could start. Packs without a
  * shipped nether-background.png use the tiled netherrack fallback on Wii. */
 s_bg_nether = false;
#endif
 return s_bg_nether ? tex_scene_backdrop_state_of(bake_job_pack(), true) : beach;
}
/* A bake that SUCCEEDED but could not be saved to the SD card (texture.c
 * publish: ALLOC 3 encode workspace, DIR 4, OPEN 5, WRITE 6, CLOSE 7,
 * RENAME 8). Ruling 10-28: never a boot refusal -- the title is admitted with
 * the pack's dirt; the pack screen shows the note; Apply Pack retries. */
static bool s_bg_cache_write_failed;
static char s_bg_cache_note[160];
static bool s_bg_pixels_uploaded; /* this job's upload came from the pixels */
static bool s_bg_upload_failed;   /* saved, but no GPU upload this session */
static bool bake_job_start(void) {
	enum backdrop_bake_error err;
	s_bg_cache_write_failed = false; /* a new attempt */
	s_bg_upload_failed = false;
	s_bg_pixels_uploaded = false;
	return backdrop_bake_request_recipe(s_bg_nether ? backdrop_bake_nether_recipe() : NULL, &err);
}
static enum backdrop_bake_state bake_job_poll(void) {
	struct backdrop_bake* b = backdrop_bake_active();
	return b ? backdrop_bake_state(b) : BACKDROP_BAKE_FAILED;
}
static enum backdrop_bake_error bake_job_error(void) {
	struct backdrop_bake* b = backdrop_bake_active();
	return b ? backdrop_bake_error(b) : BACKDROP_BAKE_ERR_SCENE;
}
static uint8_t* bake_job_take(void) {
	return backdrop_bake_take_pixels(backdrop_bake_active());
}
static bool bake_publish_is_cache_write(int r) {
	return r >= 3 && r <= 8;
}
static void boot_gate_mem_open(void);
static void boot_gate_mem_close(const char* where);
/* hw1028c: after a cold-boot bake, re-decoding the just-saved stored-DEFLATE
 * PNG (2.37 MB) needs file + IDAT copy + scanlines + RGBA = ~7.1 MB of MEM1
 * in four ~2.37 MB blocks; MEM1 had 9.18 MB free in total (heap grown and
 * holed by the bake), so the decode failed and nothing was uploaded. The
 * baked pixels are already in memory and ARE the saved image (lossless
 * PNG): upload them directly -- one 2.36 MB MEM1 copy (tex_gfx_load owns and
 * frees it) -- and let the reload skip the decode. */
static bool bake_job_upload_pixels(const uint8_t* rgba) {
	size_t n = (size_t)BACKDROP_BAKE_OUT_W * BACKDROP_BAKE_OUT_H * 4;
	gfx_wait_idle(); /* the menu drew the old slot last frame */
	boot_gate_mem_open();
	uint8_t* copy = malloc(n);
	bool ok = false;
	if(copy) {
		memcpy(copy, rgba, n);
		ok = tex_gfx_load(bake_job_texture(), copy, BACKDROP_BAKE_OUT_W,
						  BACKDROP_BAKE_OUT_H, TEX_FMT_RGBA16, false);
	}
	boot_gate_mem_close("job_pixels");
	return ok && bake_job_texture()->data != NULL;
}
static int bake_job_publish(const uint8_t* rgba) {
	s_bg_state_beach = -1; /* the cache files are about to change */
	int r = tex_scene_backdrop_publish(bake_job_pack(), rgba, BACKDROP_BAKE_OUT_W,
								 BACKDROP_BAKE_OUT_H, s_bg_nether);
	s_bg_pixels_uploaded = false;
	if(r == 0 && rgba) /* PUBLISHED: the cache now holds exactly these pixels */
		s_bg_pixels_uploaded = bake_job_upload_pixels(rgba);
	if(bake_publish_is_cache_write(r)) {
		s_bg_cache_write_failed = true;
		loading_notice_cache_write_note(tex_backdrop_publish_reason(r),
			s_bg_cache_note, sizeof(s_bg_cache_note));
		char line[256];
		snprintf(line, sizeof(line),
				 "{\"event\":\"backdrop_cache_unsaved\",\"code\":%d,"
				 "\"reason\":\"%.150s\"}", r, tex_backdrop_publish_reason(r));
		printf("[credits] %s\n", line);
		(void)hw_telemetry_emit_observer(line);
	}
	return r;
}
/* The job shows this reason on the pack screen ("... Apply Pack retries."). */
static const char* bake_job_publish_reason(int r) {
	return bake_publish_is_cache_write(r) ? s_bg_cache_note
										  : tex_backdrop_publish_reason(r);
}
static void bake_job_reload(void) {
	s_bg_state_beach = -1;
	if(s_bg_pixels_uploaded) { /* already on the GPU from the pixels */
		s_bg_pixels_uploaded = false;
		return;
	}
	gfx_wait_idle(); /* the menu drew the old slot last frame */
	boot_gate_mem_open();
	/* The result is the witness below: a loaded slot has GPU data. */
	(void)tex_scene_background_load(s_bg_nether);
	boot_gate_mem_close("job_reload");
	/* The job fails with "couldn't be uploaded" next; the save itself worked,
	 * so the boot gate admits the title with the dirt (never a hang). */
	s_bg_upload_failed = bake_job_texture()->data == NULL;
	if(s_bg_upload_failed) {
		const char* line = "{\"event\":\"backdrop_upload_failed\"}";
		printf("[credits] %s\n", line);
		(void)hw_telemetry_emit_observer(line);
	}
}
/* T2b witnesses: the background really is on the GPU; the scratch world and
 * its mesher jobs are really retired. */
static bool bake_job_uploaded(void) {
	return bake_job_texture()->data != NULL;
}
static bool bake_job_retired(void) {
	return !s_bake.world_live && !s_bake.retire_pending;
}
static int bake_job_sounds_state(void) {
	return (int)audio_registry_ready_state();
}

static void bake_host_register(bool selftest) {
	static const struct backdrop_bake_host host = {
		.world_alive = bake_host_world_alive,
		.scene_begin = bake_host_scene_begin,
		.install_column = bake_host_install,
		.settle_step = bake_host_settle,
		.render_capture = bake_host_render_capture,
		.scene_end = bake_host_scene_end,
	};
	backdrop_bake_register_host(&host);
	s_bake.selftest_pending = selftest;
	s_bake.legacy = config_read_int(&gstate.config_user,
		"debug.backdrop_bake_legacy", 0) == 1;
	int bo = config_read_int(&gstate.config_user, "debug.backdrop_bake_budget_us", 0);
	s_bake.budget_override_us = bo > 0 && bo <= 100000 ? (uint32_t)bo : 0;
	static const struct backdrop_job_hooks job = {
		.state = bake_job_state,
		.bake_start = bake_job_start,
		.bake_poll = bake_job_poll,
		.bake_error = bake_job_error,
		.bake_take = bake_job_take,
		.bake_release = backdrop_bake_release_active,
		.pixels_free = backdrop_bake_pixels_free,
		.publish = bake_job_publish,
		.publish_reason = bake_job_publish_reason,
		.reload = bake_job_reload,
		.uploaded = bake_job_uploaded,
		.retired = bake_job_retired,
		.sounds_state = bake_job_sounds_state,
		.sounds_reason = audio_registry_failure_reason,
		.sounds_retry = audio_registry_retry,
	};
	screen_texturepacks_set_backdrop_hooks(&job);
}

/* Once per frame, after the flip's token hand-off, mesh reception and the
 * frame clear, before anything else is drawn. Bounded: one bake step.
 * noinline: keeps the telemetry line buffer out of main()'s frame (the Wii
 * stack-frame gate). */
#define BAKE_CARDS_BUDGET_US 25000u
static bool boot_gate_cards_only(void);
static __attribute__((noinline)) void bake_frame_service(void) {
	if(s_bake.retire_pending) (void)bake_try_retire(0);
	if(s_bake.selftest_pending && !backdrop_bake_active()
	   && !bake_host_world_alive(NULL)) {
		enum backdrop_bake_error err;
		s_bake.selftest_pending = false;
		s_bake.selftest_owned = backdrop_bake_request(&err);
		if(!s_bake.selftest_owned)
			printf("[bake] selftest request refused (%d)\n", (int)err);
	}
	struct backdrop_bake* b = backdrop_bake_active();
	if(!b) {
		s_bake.last_state_valid = false;
		return;
	}
	if(!s_bake.last_state_valid) {
		s_bake.mem_sampled = false;
		s_bake.world_mem1_peak = s_bake.world_mem2_peak = s_bake.mesh_peak = 0;
		s_bake.carve_largest_min = 0;
		s_bake.frames = 0;
		s_bake.light_us = s_bake.vis_us = s_bake.build_us = 0;
		s_bake.light_calls = s_bake.mesh_deferred = s_bake.budget_max_us = 0;
		bake_mem_sample(true);
	}
	/* T5: while only the boot credit cards are up (no world, no menu
	 * input), the bake may take ~25 ms of a frame (cards present at 30 Hz
	 * instead of 60; input, watchdog and present stay once per frame). */
	uint32_t budget = !s_bake.legacy && boot_gate_cards_only()
		? BAKE_CARDS_BUDGET_US : 8000;
	/* debug only: emulate a slower CPU's per-frame pacing on PC */
	if(s_bake.budget_override_us) budget = s_bake.budget_override_us;
	backdrop_bake_service(budget);
	enum backdrop_bake_state st = backdrop_bake_state(b);
	if(!s_bake.last_state_valid || st != s_bake.last_state) {
		if(st != BACKDROP_BAKE_DONE && st != BACKDROP_BAKE_FAILED
		   && st != BACKDROP_BAKE_CANCELLED)
			bake_mem_sample(true);
		bake_emit(b);
	}
	s_bake.last_state = st;
	s_bake.last_state_valid = true;
	if(s_bake.selftest_owned && (st == BACKDROP_BAKE_DONE
		|| st == BACKDROP_BAKE_FAILED || st == BACKDROP_BAKE_CANCELLED)) {
		uint8_t* px = backdrop_bake_take_pixels(b);
#ifdef PLATFORM_PC
		/* debug.backdrop_bake_selftest on PC only: the measured output for
		 * review, written to the working directory (never a pack/save). */
		if(px && lodepng_encode32_file("backdrop_bake_selftest.png", px,
			   BACKDROP_BAKE_OUT_W, BACKDROP_BAKE_OUT_H))
			printf("[bake] selftest png write failed\n");
#endif
		backdrop_bake_pixels_free(px);
		backdrop_bake_release_active();
		s_bake.selftest_owned = false;
		s_bake.last_state_valid = false;
	}
}

/* ---- BOOT-CREDITS-1028 task 3: the boot gate --------------------------
 * The credit cards (game/gui/startup_credits.c) cover startup and the title
 * is admitted only when loading_notice_boot_decide says READY, from task 2's
 * readiness API: tex_ready_state, audio_registry_ready_state and the pack
 * screen's backdrop job (screen_texturepacks_backdrop_ready_state: queued
 * work, upload and scratch retirement). The loaders call boot_gate_service
 * between their steps, so the cards keep animating while sounds load. Any
 * FAILED readiness is a Retry (A) / Exit (B) footer using that loader's own
 * retry entry -- never a silent dirt or silent title. */
struct boot_gate {
	bool active;            /* cards up, title not admitted */
	bool title_a_blocked;   /* the A that ended the cards is still held */
	struct startup_credits_data data;
	struct startup_credits sc;
	struct tex_gfx tex[SC_MAX_TEXTURES];
	char marker[512];
	struct hw_run_config* run; /* main()'s run config, for boot-trace marks */
	bool in_service;        /* re-entry guard for the loader callback */
	bool svc_seen;
	ptime_t svc_last, last_present;
	uint64_t service_gap_max_us; /* loader callback START to START */
	uint32_t presents_in_service;
	/* Backdrop PNG load window: MEM1/MEM2 free minima, sampled on every
	 * loader-service call while open (decode may peak ~7 MB). */
	bool mem_window;
	size_t mem1_free_min, mem2_free_min;
	bool trace; /* debug.boot_credits_trace: one line per presented frame */
	/* T5: card presents, START to START, from BOTH the main-loop frames and
	 * the loader-service presents (hw1028d: a 1 s main-loop frame that
	 * presented 15x inside the audio scan is not a 1 s card freeze). */
	bool frame_seen;
	ptime_t frame_last;
	uint64_t frame_gap_max_us;
};
static struct boot_gate s_gate;
static void boot_gate_note_present(void) {
	ptime_t now = time_get();
	if(s_gate.frame_seen) {
		uint64_t gap = time_diff_us(s_gate.frame_last, now);
		if(gap > s_gate.frame_gap_max_us) s_gate.frame_gap_max_us = gap;
	}
	s_gate.frame_seen = true;
	s_gate.frame_last = now;
}
static bool boot_gate_cards_only(void) {
	return s_gate.active;
}
static ptime_t s_gate_t0;
static uint32_t boot_gate_now(void) {
	return (uint32_t)time_diff_ms(s_gate_t0, time_get());
}

static void boot_gate_quad(void* user, int tex, int sx, int sy, int sw, int sh,
	int dx, int dy, int dw, int dh, uint8_t r, uint8_t g, uint8_t b,
	uint8_t a) {
	(void)user;
	struct tex_gfx* t = &s_gate.tex[tex];
	if(!t->data) return;
	gfx_bind_texture(t);
	/* GUI 640x480 -> the real GUI size, centred (4:3 and 16:9). */
	float scale = (float)gfx_height() / 480.0F;
	float ox = ((float)gfx_width() - 640.0F * scale) / 2.0F;
	float x0 = ox + dx * scale, y0 = dy * scale;
	float x1 = ox + (dx + dw) * scale, y1 = (dy + dh) * scale;
	float u0 = (float)sx / t->width, v0 = (float)sy / t->height;
	float u1 = (float)(sx + sw) / t->width, v1 = (float)(sy + sh) / t->height;
	float v[12] = {x0, y0, -1, x1, y0, -1, x1, y1, -1, x0, y1, -1};
	float uv[8] = {u0, v0, u1, v0, u1, v1, u0, v1};
	uint8_t c[16] = {r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a};
	gfx_draw_quads_flt(4, v, c, uv);
}

static void boot_gate_paint(void* user) {
	(void)user;
	gfx_blending(MODE_BLEND);
	gfx_alpha_test(false);
	startup_credits_draw(&s_gate.sc, boot_gate_quad, NULL);
	gfx_alpha_test(true);
}

/* The loaders' main-thread service (audio_registry_set_service /
 * tex_set_load_service). Runs between keys / I/O chunks, possibly many times
 * per display frame: it records the gap, and at most every 66 ms it polls
 * input, advances and presents the current card from the independent
 * credits textures, and pets the watchdog. It never re-enters a loader,
 * changes config, frees anything or steps the bake. */
#define BOOT_GATE_PRESENT_MS 66
static void boot_gate_mem_sample(void) {
	size_t mem1 = 0;
#ifdef PLATFORM_WII
	struct mallinfo mi = mallinfo();
	mem1 = (size_t)mi.fordblks + (size_t)SYS_GetArena1Size();
#endif
	size_t mem2 = mem2_carve_free_bytes();
	if(mem1 < s_gate.mem1_free_min) s_gate.mem1_free_min = mem1;
	if(mem2 < s_gate.mem2_free_min) s_gate.mem2_free_min = mem2;
}
static void boot_gate_mem_open(void) {
	s_gate.mem1_free_min = s_gate.mem2_free_min = (size_t)-1;
	s_gate.mem_window = true;
	boot_gate_mem_sample();
}
static void boot_gate_mem_close(const char* where) {
	if(!s_gate.mem_window) return;
	boot_gate_mem_sample();
	s_gate.mem_window = false;
	char line[256];
	snprintf(line, sizeof(line),
			 "{\"event\":\"backdrop_png_load\",\"where\":\"%s\","
			 "\"mem1_free_min\":%lu,\"mem2_free_min\":%lu,\"uploaded\":%d}",
			 where, (unsigned long)s_gate.mem1_free_min,
			 (unsigned long)s_gate.mem2_free_min,
			 texture_menu_background.data != NULL);
	printf("[credits] %s\n", line);
	(void)hw_telemetry_emit_observer(line);
}

/* Frame-time proof log (debug only): presented time, card, alpha, idle-loop
 * step, and whether the frame came from inside a loader (in_load=1). */
static void boot_gate_trace(int in_load) {
	if(!s_gate.trace) return;
	struct sc_visible v[1];
	int n = startup_credits_visible(&s_gate.sc, v);
	int step = -1;
	if(n) {
		const struct sc_card* c = &s_gate.data.cards[v[0].card];
		for(int i = 0; i < c->nitems; i++)
			if(c->items[i].kind == SC_ITEM_ANIM) {
				uint32_t start = v[0].card * (s_gate.data.fade_in_ms
					+ s_gate.data.dwell_ms + s_gate.data.fade_out_ms);
				step = startup_credits_anim_step(&c->items[i],
					s_gate.sc.clock_ms > start ? s_gate.sc.clock_ms - start : 0);
				break;
			}
	}
	printf("[credits-trace] wall_ms=%u clock_ms=%u card=%d alpha=%d step=%d "
		   "in_load=%d\n", boot_gate_now(), (unsigned)s_gate.sc.clock_ms,
		   n ? v[0].card : -1, n ? v[0].alpha : 0, step, in_load);
}

static void boot_gate_service(void* ctx) {
	(void)ctx;
	if(s_gate.mem_window) boot_gate_mem_sample();
	ptime_t now = time_get();
	if(s_gate.svc_seen) {
		uint64_t gap = time_diff_us(s_gate.svc_last, now);
		if(gap > s_gate.service_gap_max_us) s_gate.service_gap_max_us = gap;
	}
	s_gate.svc_seen = true;
	s_gate.svc_last = now;
	if(s_gate.in_service || time_diff_ms(s_gate.last_present, now)
							   < BOOT_GATE_PRESENT_MS)
		return;
	s_gate.in_service = true;
	watchdog_heartbeat();
	if(s_gate.active) {
		input_poll();
		startup_credits_update(&s_gate.sc, boot_gate_now(), false, false,
							   false);
		(void)loading_notice_present_next(boot_gate_paint, NULL);
		boot_gate_note_present();
		s_gate.presents_in_service++;
		boot_gate_trace(1);
	}
	/* The loaders run under WDM_IDLE (a long scan is not a GPU hang). */
	watchdog_phase_main(WDM_IDLE);
	s_gate.last_present = time_get();
	s_gate.in_service = false;
}

static void boot_gate_release(void) {
	/* The last card frame may still be in flight on the GP: drain it before
	 * mem2_free hands the texel storage back to the heap the display lists
	 * reuse (same fence as bake_job_upload_pixels; DIAG-GPU 10-28). */
	gfx_wait_idle();
	for(int k = 0; k < SC_MAX_TEXTURES; k++)
		tex_gfx_clear(&s_gate.tex[k]);
	startup_credits_data_free(&s_gate.data);
}

static void boot_gate_preinit(void) {
	memset(&s_gate, 0, sizeof(s_gate));
	s_gate_t0 = time_get();
	s_gate.last_present = s_gate_t0;
	audio_registry_set_service(boot_gate_service, NULL);
	tex_set_load_service(boot_gate_service, NULL);
	boot_gate_mem_open();
}

/* After gfx_setup + input_init, before the sound scan. False: no cards
 * (assets missing) -- the caller keeps the old boot notice. Registers the
 * loader service either way (watchdog/gap only without cards). */
static bool boot_gate_begin(struct hw_run_config* run) {
	/* boot_gate_preinit installed the services before gfx_setup; its PNG
	 * window covered the boot tex_init (textures + cached background). */
	boot_gate_mem_close("boot_tex_init");
	s_gate.run = run;
	/* ci-1101/18: the same bundled roots the texture resolver walks (Wii:
	 * sd:/assets, then the app-folder assets/; PC: assets/), so a player who
	 * installed only apps/CavEX/ still gets the credit cards. */
	const char* roots[2];
	size_t nroots = tex_bundled_roots(roots, 2);
	int r = SC_LOAD_ERR_MANIFEST;
	char dir[96] = "";
	for(size_t k = 0; k < nroots; k++) {
		snprintf(dir, sizeof(dir), "%s/gui/startup-credits", roots[k]);
		r = startup_credits_load(&s_gate.data, dir);
		if(r == SC_LOAD_OK)
			break;
		fprintf(stderr, "[credits] %s not loaded (%d)%s\n", dir, r,
				k + 1 < nroots ? "; trying the next bundled root" : "");
	}
	if(r != SC_LOAD_OK) {
		fprintf(stderr, "[credits] no bundled root had the credits; boot "
				"notice instead\n");
		return false;
	}
	for(int k = 0; k < s_gate.data.ntextures; k++) {
		uint8_t* px = startup_credits_take_texture(&s_gate.data, k);
		/* tex_gfx_load owns px from here, on success and failure. */
		if(!px || !tex_gfx_load(&s_gate.tex[k], px, s_gate.data.tex[k].w,
								s_gate.data.tex[k].h, TEX_FMT_RGBA32, false)) {
			fprintf(stderr, "[credits] texture %d upload failed\n", k);
			boot_gate_release();
			return false;
		}
	}
	bool first_run = true;
	if(startup_credits_marker_path("config.json", s_gate.marker,
								   sizeof(s_gate.marker)))
		first_run = !startup_credits_marker_seen(s_gate.marker);
	startup_credits_begin(&s_gate.sc, &s_gate.data, first_run, boot_gate_now());
	startup_credits_set_phase(&s_gate.sc, "sounds");
	s_gate.active = true;
	s_gate.trace = config_read_int(&gstate.config_user,
								   "debug.boot_credits_trace", 0) == 1;
	/* The first frame is presented for real now (card 1 starts its fade-in);
	 * the loader service keeps presenting from here. */
	(void)loading_notice_present_custom(boot_gate_paint, NULL);
	s_gate.last_present = time_get();
	char detail[48];
	snprintf(detail, sizeof(detail), "first_run=%d", first_run ? 1 : 0);
	hw_boot_trace_write("credits_first_card", s_gate.run, detail, false);
	return true;
}

/* -- readiness adapter: task 2's API -- */
/* Audio: LOADING / READY / FAILED (transient, retryable); any other state is
 * T2c's DISABLED (permanent: no MEM2 arena, the pre-T2b silent boot) and
 * counts as ready, reported once by log + telemetry, never a footer. */
static bool s_gate_audio_disabled_noted;
static enum audio_registry_ready boot_gate_audio_state(void) {
	enum audio_registry_ready a = audio_registry_ready_state();
	if(a != AUDIO_REGISTRY_DISABLED)
		return a;
	if(!s_gate_audio_disabled_noted) {
		s_gate_audio_disabled_noted = true;
		const char* why = audio_registry_failure_reason();
		char line[192];
		snprintf(line, sizeof(line),
				 "{\"event\":\"boot_audio_disabled\",\"state\":%d,\"reason\":\"%.120s\"}",
				 (int)a, why ? why : "");
		printf("[credits] %s\n", line);
		(void)hw_telemetry_emit_observer(line);
	}
	return AUDIO_REGISTRY_READY; /* disabled = ready, silently playable */
}
/* Retry is unavailable while a stalled pass-C worker still owns the arena
 * (audio_registry_retry would refuse): Exit only. */
static bool boot_gate_audio_retry_allowed(void) {
	struct audio_async_status st;
	audio_registry_async_status(&st);
	return st.state != AUDIO_ASYNC_RUNNING && st.state != AUDIO_ASYNC_FAILED;
}

static void boot_gate_inputs(struct loading_notice_boot_inputs* in) {
	enum tex_ready t = tex_ready_state();
	enum audio_registry_ready a = boot_gate_audio_state();
	enum backdrop_job_readiness b = screen_texturepacks_backdrop_ready_state();
	*in = (struct loading_notice_boot_inputs) {
		.textures_done = t == TEX_READY,
		.textures_failed = t == TEX_FAILED,
		.textures_reason = tex_failure_reason(),
		.audio_done = a == AUDIO_REGISTRY_READY,
		.audio_disabled = audio_registry_ready_state() == AUDIO_REGISTRY_DISABLED,
		.audio_failed = a == AUDIO_REGISTRY_FAILED,
		.audio_reason = audio_registry_failure_reason(),
		.background = b == BACKDROP_JOB_READY ? LOADING_NOTICE_BG_READY
			: b == BACKDROP_JOB_FAILED ? LOADING_NOTICE_BG_FAILED
									   : LOADING_NOTICE_BG_PENDING,
		.background_reason = screen_texturepacks_backdrop_failure_reason(),
		.background_cache_write_failed = s_bg_cache_write_failed,
		.background_upload_failed = s_bg_upload_failed,
	};
}

/* Between frames only: the failed loader's own retry entry. */
static void boot_gate_retry(const struct loading_notice_boot_inputs* in) {
	if(in->textures_failed)
		(void)tex_init(); /* its bool and tex_ready_state agree */
	else if(in->audio_failed)
		(void)audio_registry_retry(); /* refuses until the arena retired */
	else if(in->background == LOADING_NOTICE_BG_FAILED) {
		s_bg_cache_write_failed = false;
		s_bg_upload_failed = false;
		screen_texturepacks_backdrop_retry();
	}
}

static void boot_gate_emit(void) {
	char line[256];
	snprintf(line, sizeof(line),
			 "{\"event\":\"boot_credits\",\"first_run\":%d,\"credits_ms\":%u,"
			 "\"boot_service_gap_max_us\":%llu,\"audio_service_gap_max_us\":%llu,"
			 "\"presents_in_service\":%u,\"present_gap_max_us\":%llu}",
			 s_gate.sc.first_run ? 1 : 0, (unsigned)s_gate.sc.clock_ms,
			 (unsigned long long)s_gate.service_gap_max_us,
			 (unsigned long long)audio_registry_service_gap_max_us(),
			 (unsigned)s_gate.presents_in_service,
			 (unsigned long long)s_gate.frame_gap_max_us);
	printf("[credits] %s\n", line);
	(void)hw_telemetry_emit_observer(line);
	if(s_gate.run) {
		char detail[96];
		snprintf(detail, sizeof(detail),
				 "credits_ms=%u boot_service_gap_max_us=%llu",
				 (unsigned)s_gate.sc.clock_ms,
				 (unsigned long long)s_gate.service_gap_max_us);
		hw_boot_trace_write("title_admitted", s_gate.run, detail, false);
	}
}

/* After admission the loaders (pack Apply) keep a service that only pets the
 * watchdog and records gaps: the credits textures are gone, and the normal
 * textures are being replaced, so nothing is drawn from it. */
void screen_texturepacks_reload_frame(void);
static void boot_gate_apply_service(void* ctx) {
	(void)ctx;
	if(s_gate.mem_window) boot_gate_mem_sample();
	ptime_t now = time_get();
	if(time_diff_ms(s_gate.last_present, now) >= BOOT_GATE_PRESENT_MS) {
		watchdog_heartbeat();
		/* pack-lag-1030: a pack Apply's live activity frame (untextured;
		 * a no-op unless the Texture Packs screen is mid-reload). */
		screen_texturepacks_reload_frame();
		s_gate.last_present = now;
	}
}

/* Main loop, once per frame before the screen update. */
static void boot_gate_update(void) {
	bool a_held = input_held(IB_GUI_CLICK);
	if(!s_gate.active) {
		if(s_gate.title_a_blocked)
			s_gate.title_a_blocked = startup_credits_title_input_blocked(
				&s_gate.sc, a_held);
		return;
	}
	bool a_edge = input_pressed(IB_GUI_CLICK);
	boot_gate_note_present(); /* this frame's cards present at its flip */
	struct loading_notice_boot_inputs in;
	boot_gate_inputs(&in);
	const char *phase, *reason;
	enum loading_notice_boot_state st = loading_notice_boot_decide(&in, &phase,
																   &reason);
	if(st == LOADING_NOTICE_BOOT_ERROR) {
		bool retry_ok = !(in.audio_failed && !in.textures_failed)
			|| boot_gate_audio_retry_allowed();
		char line[160];
		snprintf(line, sizeof(line), retry_ok ? "%s A: Retry  B: Exit"
											  : "%s B: Exit", reason);
		startup_credits_set_error(&s_gate.sc, line);
		switch(loading_notice_boot_error_choice(
			a_edge, input_pressed(IB_GUI_CLICK_ALT), retry_ok)) {
			case LOADING_NOTICE_BOOT_RETRY:
				boot_gate_retry(&in);
				a_edge = false; /* the retry press is consumed */
				break;
			case LOADING_NOTICE_BOOT_EXIT: gstate.quit = true; break;
			default: break;
		}
	} else {
		startup_credits_set_error(&s_gate.sc, NULL);
		startup_credits_set_phase(&s_gate.sc, phase);
	}
	startup_credits_update(&s_gate.sc, boot_gate_now(),
						   st == LOADING_NOTICE_BOOT_READY, a_edge, a_held);
	if(!startup_credits_done(&s_gate.sc))
		return;
	/* Title admitted: no loader is in flight (all three READY). */
	s_gate.active = false;
	s_gate.title_a_blocked = startup_credits_title_input_blocked(&s_gate.sc,
																 a_held);
	if(startup_credits_marker_due(&s_gate.sc)) {
		int mr = startup_credits_marker_write(s_gate.marker);
		if(mr != SC_MARKER_OK) /* reported, never blocks play */
			fprintf(stderr, "[credits] marker %s not written (%d); cards play "
					"in full again next boot\n", s_gate.marker, mr);
		startup_credits_marker_noted(&s_gate.sc);
	}
	boot_gate_emit();
	audio_registry_set_service(boot_gate_apply_service, NULL);
	tex_set_load_service(boot_gate_apply_service, NULL);
	boot_gate_release();
}

/* True while the title screen must not see input: the cards are up, or the
 * A that ended them is still held. */
static bool boot_gate_blocks_title(void) {
	return s_gate.active || s_gate.title_a_blocked;
}

/* Main loop, after every GUI draw of the frame: the cards cover it all. */
static void boot_gate_draw(void) {
	if(!s_gate.active) return;
	gfx_mode_gui();
	gfx_texture(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, gfx_width(), gfx_height(), 0, 0, 0,
					  255);
	gfx_texture(true);
	boot_gate_paint(NULL);
	boot_gate_trace(0);
}

/* hw1030n: main's heartbeat state, borrowed by the spawn-search wait
 * (spawn_search_wait_idle) so a long Beta walk keeps the run's 1 s heartbeat
 * instead of falling silent for its whole length. Main thread only; armed
 * once main's loop locals exist, for hw_run profiles only. */
static struct {
	bool armed;
	uint32_t* tick;
	uint32_t* frame_counter;
	ptime_t* last;
	bool* sent;
	int far_vd;
	struct server_local* server;
} s_wait_hb;

static void wait_heartbeat_service(void) {
	if(!s_wait_hb.armed)
		return;
	ptime_t now = time_get();
	if(*s_wait_hb.sent && time_diff_s(*s_wait_hb.last, now) < 1.0F)
		return;
	watchdog_phase_main(WDM_HEARTBEAT);
	emit_hw_heartbeat((*s_wait_hb.tick)++, s_wait_hb.far_vd,
					  s_wait_hb.server->far_view_distance,
					  *s_wait_hb.frame_counter);
	*s_wait_hb.last = now;
	*s_wait_hb.sent = true;
}

static bool wait_spawn_probes(uint32_t* probes) {
	return server_local_spawn_search_progress(probes, NULL);
}

int main(int argc, char** argv) {
	achievement_notice_init();
	// daytime is only (re)computed on unpaused frames (the world-time formula in
	// the main loop) but read unconditionally for sky/fog colors -- start it at
	// noon-ish 0 so a paused-from-the-first-frame session never feeds an
	// uninitialized float into daytime_sky_colors/gfx_fog_color.
	float daytime = 0.0F, tick_delta;
	/* Split-dims B2 H1: each view's own daytime (flag OFF: == daytime). */
	float daytime_view[2] = {0.0F, 0.0F};
	float paused_tick_delta = 0.0F;
	bool render_world;
	struct hw_run_config hw_run;
	char hw_run_err[128];
	if(!hw_run_parse_args(&hw_run, argc, argv, hw_run_err,
						  sizeof(hw_run_err))) {
		fprintf(stderr, "[hw-run] %s\n", hw_run_err);
		return EXIT_FAILURE;
	}
#ifndef PLATFORM_WII
	// PC: the run-args file is CWD-relative and the CWD is already valid at
	// startup, so it can be read here. On Wii the SD is not mounted until
	// fatInitDefault() below; that read is deferred to just after the mount
	// (an absolute sd:/ path) because a pre-mount CWD-relative open could never
	// find the staged file -- the HBC-launch CWD is the device root sd:/.
	if(!hw_run.enabled
	   && !hw_run_parse_file(&hw_run, "cw_run_args.txt", hw_run_err,
							 sizeof(hw_run_err))) {
		fprintf(stderr, "[hw-run] %s\n", hw_run_err);
		return EXIT_FAILURE;
	}
#endif

	gstate.quit = false;
	gstate.players[0].camera = (struct camera) {
		.x = 0, .y = 0, .z = 0, .rx = 0, .ry = 0, .controller = {0, 0, 0}};
	gstate.config.fov = 70.0F;
	// Beta default: viewBobbing = true [740c583 GameSettings:21]. The user
	// override is read after config_user loads (next to the audio keys).
	gstate.config.view_bobbing = true;
	// Scale fog + far plane to the loaded view (MAX_VIEW_DISTANCE chunks): fog
	// sits at the load edge so chunks fade instead of popping, and the far
	// plane sits just beyond so the fog hides the clip. PC loads 10 chunks
	// (~160 units), Wii 5 (~80). Previously fog was hardcoded to 5 chunks, so
	// the PC build still cut off at 80 units no matter how much was loaded.
	// M1 deep reach: scale fog + far plane to the FAR view, so the far ring (the
	// annulus past MAX_VIEW_DISTANCE) is not render-culled by the fog gate
	// (world.c FOG_DIST_LESS). On Wii MAX_FAR_VIEW_DISTANCE == MAX_VIEW_DISTANCE so
	// this is unchanged there. NOTE: the +1 ring's diagonal CORNERS sit at Euclidean
	// ~sqrt(2)*radius and are still fog-culled at this fog_distance -- the ring shows
	// as axis-facing strips (accepted for the first increment; widen fog to draw the
	// corners once the deeper rings land).
	gstate.config.render_distance = MAX_FAR_VIEW_DISTANCE * 16.0F + 32.0F;
	gstate.config.fog_distance = MAX_FAR_VIEW_DISTANCE * 16.0F;
	gstate.world_loaded = false;
	gstate.players[0].held_item_animation.punch.start = time_get();
	gstate.players[0].held_item_animation.switch_item.start = time_get();
	gstate.players[0].digging.cooldown = time_get();
	gstate.players[0].digging.active = false;
	gstate.paused = false;

	rand_gen_seed(&gstate.rand_src);

#ifdef PLATFORM_WII
	boot_phase_paint(BOOT_PAINT_BLUE, 0); // band 0: main() reached
	// BREAKER-#2 LAYOUT GUARD un-droppability (2026-09-03, see the comment on
	// cw_layout_probe_pad above): a real store through a volatile pointer,
	// executed before FAT mount, so the array is genuinely live at runtime and
	// not just "used" for the compiler's benefit -- LTO's whole-program
	// analysis proved it will drop a static that has only the latter.
	((volatile uint8_t*)cw_layout_probe_pad)[0] = 0;
#ifdef CAVEWII_IOS_RELOAD_BOOT
	// EXPERIMENT (chip task_5bf4684b, 2026-08-24 lessons review): no app or
	// libogc path ever reloads IOS on ENTRY -- an HBC-direct launch inherits
	// HBC's live IOS state wholesale (open BT/OH1 handles, net, /dev fds),
	// the suspected common cause of the direct-launch pathologies. A fresh
	// IOS_ReloadIOS here completes the hardware takeover the way channels
	// get it. Build with make EXTRA_DEFS=-DCAVEWII_IOS_RELOAD_BOOT; default
	// builds omit it (this path is hardware-experiment-only until proven).
	IOS_ReloadIOS(IOS_GetPreferredVersion());
#endif
	// Reserve the MEM2 texture heap before anything spills into MEM2 (FAT
	// buffers, config, textures all come after). Frees the scarce MEM1 fast pool
	// that framebuffers / GX FIFO / display lists actually need.
	mem2_init();
	// RCA 2026-08-25: fatInitDefault was called ONCE with its return DISCARDED,
	// while the stage server -- which survives every launch context this call
	// died in five times today -- retries its own identical mount 10x/1s
	// (tools/wii_stage_server/source/main.c configure_fat). The SD/DI IOS
	// negotiation is transiently unreliable after a chainload handoff; give the
	// game the same margin the loader always had. Attempts land in the
	// wii_after_fat trace detail so the rig can see how marginal a boot was.
	boot_phase_paint(BOOT_PAINT_YELLOW, 1); // band 1: entering FAT mount
	int fat_attempts = 0;
	bool fat_ok = false;
	while(fat_attempts < 10) {
		fat_attempts++;
		if(fatInitDefault()) {
			fat_ok = true;
			break;
		}
		boot_phase_paint(BOOT_PAINT_RED, 2); // band 2 red: attempt failed
		usleep(1000 * 1000);
	}
	boot_phase_paint(fat_ok ? BOOT_PAINT_GREEN : BOOT_PAINT_RED, 2);
	// Override libogc's exception panic fn ASAP (the SD is mounted) so even a fault during
	// world creation dumps registers to hang_report.txt instead of silently spinning.
	watchdog_install_fault_handler();
	// The SD is mounted now, so read the staged run-args by ABSOLUTE path. A
	// wiiload/argv launch already enabled hw_run above (this is skipped); a
	// manual/staged HBC launch reaches here disabled because the HBC CWD is the
	// device root sd:/ (not the app dir), so the old pre-mount CWD-relative read
	// could never open the staged file. Try the app dir first, then sd:/ root.
	// Must precede the wii_after_fat trace so that, once enabled, the trace is
	// truncated fresh (reset=true). A parse error leaves telemetry off and boot
	// continues -- never brick a console boot on a malformed proof-args file.
	if(!hw_run.enabled)
		hw_run_parse_file(&hw_run, "sd:/apps/CavEX/cw_run_args.txt",
						  hw_run_err, sizeof(hw_run_err));
	if(!hw_run.enabled)
		hw_run_parse_file(&hw_run, "sd:/cw_run_args.txt", hw_run_err,
						  sizeof(hw_run_err));
	// Consume-once: a staged cw_run_args.txt arms EXACTLY the boot it was
	// staged for. Delete both candidate paths the moment a run is armed, so a
	// later manual HBC launch gets normal play -- never a ghost re-run of the
	// last profile (which for watchdog_inject deliberately wedges the
	// console). The rig re-stages the file for every run, so nothing is lost.
	if(hw_run.enabled) {
		remove("sd:/apps/CavEX/cw_run_args.txt");
		remove("sd:/cw_run_args.txt");
	}
	char fat_detail[96];
	snprintf(fat_detail, sizeof(fat_detail),
#ifdef CAVEWII_IOS_RELOAD_BOOT
			 "fatInitDefault %s attempts=%d (IOS reloaded at boot)",
#else
			 "fatInitDefault %s attempts=%d",
#endif
			 fat_ok ? "ok" : "FAILED-ALL", fat_attempts);
	hw_boot_trace_write("wii_after_fat", &hw_run, fat_detail, true);
	#ifndef NDEBUG
		SYS_STDIO_Report(true);
		SYS_Report("[INIT] STDIO redirection is now active\n");
	#endif
#endif

	/* R6 temporary counterfactual: retain the autojoined guest at spawn
	 * with normal physics/POS publishing, restoring two-actor portal
	 * contact while the successful-bind position timer remains fixed. */
	if(hw_run.enabled && hw_run.split_guest
	   && hw_run.profile == HW_RUN_PROFILE_FAR_IDLE_2M)
		hw_run.split_guest_walk = false;

#ifdef PLATFORM_WII
	const char* default_config = "config_wii.json";
#else
	const char* default_config = "config_pc.json";
#endif
	if(hw_run.enabled) {
		server_local_work_profile_enable(true);
		server_world_work_profile_enable(true);
		clin_work_profile_enable(true);
		struct hw_telemetry_config tel_cfg = {
			.enabled = true,
			.run_id = hw_run.run_id,
			.profile = hw_run.profile_name,
			.host = hw_run.telemetry_host,
			.netmask = hw_run.telemetry_netmask,
			.port = hw_run.telemetry_port,
			.pull = hw_run.telemetry_pull,
			.suppress_sd_probes = !hw_run_periodic_sd_trace(&hw_run),
		};
		// Opt-in non-blocking send experiment (--cw-tel-nonblock). Must be
		// armed BEFORE init, because it decides the blocking flag the
		// accepted client socket is verified against. Unarmed runs never
		// call this with true, so the blocking default is untouched.
		hw_telemetry_set_nonblock_experiment(hw_run.telemetry_nonblock);
		hw_boot_trace_write("telemetry_init_begin", &hw_run, "", false);
		// OUTER DHCP retry (Batch E item 5, 2026-08-23 RCA design note --
		// docs/hwproof_audit_2026-06-30.md:112, hw_telemetry.h:17-20). The
		// INNER cap (HW_TELEMETRY_DHCP_MAX_RETRIES=3, unmodified) stays
		// deliberately small -- raising it directly would hide a genuine
		// boot hang behind a long SILENT black screen, the exact failure
		// mode that comment protects against. This wraps the whole (still
		// fast-failing) hw_telemetry_init() call in a bounded OUTER retry
		// instead: each attempt still fails fast, hw_run_tel_sleep prints a
		// console line every retry so the wait reads as "retrying" rather
		// than "hung" (the same intent, preserved one layer out), and the
		// ~30s total budget comfortably spans the ~25s DHCP settle measured
		// on the 2026-08-23 HBC-direct investigation run.
		//
		// A background/deferred bring-up -- continuing to retry AFTER
		// gfx_setup, off the boot-blocking path entirely, with the Wii-side
		// listener only starting on a late success and every send before
		// that staying a no-op -- was the design this task specified first.
		// JUDGMENT CALL, stated explicitly per CLAUDE.md's "no silent
		// shortcuts" rule: set aside for THIS batch. It needs a second
		// thread that can race hw_telemetry_shutdown()'s mutex/socket
		// teardown (called from three sites: config-load failure, re-init,
		// and normal program exit) AND wii/panic.c's exception-context
		// crash-mailbox send -- the single riskiest code path in the
		// project, since it runs from inside a live CPU fault handler.
		// Getting that interaction right deserves its own dedicated change
		// and real-Wii proof round, not a rider inside a five-item bundled
		// batch. This synchronous-but-VISIBLE retry is the smaller,
		// honestly-labeled fallback the task's own design note sanctions
		// ("the minimal honest alternative"). hw_run.enabled gates this
		// whole block, so normal untelemetered play NEVER sees the extra
		// wait -- only armed capture/proof runs do.
		struct hw_tel_retry_ctx tel_ctx = {.cfg = &tel_cfg, .attempt = 0};
		int tel_attempts = 0;
		bool tel_ok = hw_telemetry_retry_bringup(
			hw_run_tel_attempt, hw_run_tel_sleep, &tel_ctx, 30000u, 2000u,
			&tel_attempts);
		if(tel_ok) {
			// lfl = the listen socket's F_GETFL flags: ONE hardware run reading
			// this line proves whether the IOS non-blocking bit (0x04) latched
			// (the emit_boot blocking-accept root cause -- CW_IOS_O_NONBLOCK).
			char tel_detail[64];
			snprintf(tel_detail, sizeof(tel_detail), "%s lfl=0x%x attempts=%d",
					 hw_telemetry_local_ip(),
					 (unsigned)hw_telemetry_debug_listen_flags(), tel_attempts);
			hw_boot_trace_write("telemetry_init_ok", &hw_run, tel_detail, false);
			hw_telemetry_emit_boot(CW_BUILD_ID);
			hw_boot_trace_write("telemetry_boot_emit", &hw_run, "", false);
		} else {
			char tel_detail[24];
			snprintf(tel_detail, sizeof(tel_detail), "attempts=%d", tel_attempts);
			hw_boot_trace_write("telemetry_init_failed", &hw_run, tel_detail,
								false);
			fprintf(stderr,
					"[hw-run] telemetry init failed for %s:%d mode=%s after "
					"%d attempts (~30s)\n",
					hw_run.telemetry_host, hw_run.telemetry_port,
					hw_run.telemetry_pull ? "pull" : "udp", tel_attempts);
		}
	}
	/* colcap-1006 (R5 item 2a): pool_selftest_3m churns the REAL column
	 * pool here -- telemetry is up, no world/server/mesher thread exists
	 * yet, so the pool is idle and every take/free is the self-test's. */
	if(hw_run.enabled && hw_run.pool_selftest) {
		char st_why[96];
		bool st_ok = pool_selftest_boot(hw_run.pool_placement_rotating, st_why,
										sizeof st_why);
		dl_scene_hash_set_content(pool_selftest_chunk_content);
		hw_boot_trace_write(st_ok ? "pool_selftest_churn_ok"
								  : "pool_selftest_churn_failed",
							&hw_run, st_why, false);
	}
	if(hw_run.carve_census && !carve_observer_start())
		return EXIT_FAILURE;
	if(hw_run.tps_cpu && !tps_observer_start("tps_"))
		return EXIT_FAILURE;
	if(hw_run.tps_cpu) tps_observer_output(hw_telemetry_emit_observer);
	if(hw_run.settle && !settle_observer_start())
		fprintf(stderr, "[settle] observer initialization failed\n");
	if(hw_run.column_census && !column_observer_start("column_", false, false))
		return EXIT_FAILURE;
	hw_boot_trace_write("config_load_begin", &hw_run, default_config, false);
	if(!config_load_user_or_default(&gstate.config_user, "config.json",
									default_config, stderr)) {
		hw_boot_trace_write("config_load_failed", &hw_run, "", false);
		if(hw_run.enabled)
			hw_telemetry_emit_profile_done("config_load_failed");
		hw_telemetry_shutdown();
		return EXIT_FAILURE;
	}
	hw_boot_trace_write("config_load_ok", &hw_run, "", false);
	// --cw-set overrides (run-scoped, ephemeral): applied HERE, right after
	// config load and BEFORE the earliest reader of any config key below
	// (displaylist_set_compact's "memory.displaylist_compact" read is the
	// very next line) -- see hw_run.h's cw_set field comment for why a
	// staged hardware run needs this (the stage protocol writes only the
	// .dol and cw_run_args.txt, never config.json). hw_run_config_set_int
	// marks each key ephemeral, so config_save() never persists the
	// override back to disk (the same GB18 "+new"-sentinel-leak guard every
	// other hw_run config injection in this block already relies on).
	// Independent of hw_run.enabled: --cw-set does not arm telemetry by
	// itself (hw_run.h's field comment), so an override must still apply on
	// a run that only passes --cw-set plus a manual/unarmed launch.
	for(int cwsi = 0; cwsi < hw_run.cw_set_n; cwsi++)
		hw_run_config_set_int(&gstate.config_user, hw_run.cw_set[cwsi].key,
							  hw_run.cw_set[cwsi].value);
	s_cw_set_n_applied = (uint32_t)hw_run.cw_set_n;
	/* Freeze the absolute data root before the server changes world cwd.
	 * Init/key failures remain visible; they never change gameplay/provider IO. */
	char achievement_profiles[2][CONFIG_PROFILE_ID_CAP];
	bool achievement_ready = achievements_init(".");
	achievements_set_cooperative_saves(true);
	bool achievement_keys = config_read_profiles(&gstate.config_user,
		achievement_profiles) == CONFIG_PROFILES_OK;
	if(achievement_ready)
		achievement_ready = achievements_set_profiles(
			achievement_keys ? achievement_profiles[0] : NULL,
			achievement_keys ? achievement_profiles[1] : NULL);
	if(!achievement_ready)
		fprintf(stderr, "[achievements] initialization/profile failure (%d)\n",
			(int)achievements_init_status());
	displaylist_set_compact(config_read_int(&gstate.config_user,
		"memory.displaylist_compact", 0) == 1);
	/* M2 mesh blocks (Wii): default ON since the 2026-09-10 split_anchor_6m
	 * 42/256 A/B (zero ladder slack, spills 292 vs 9,134, readiness 6 s
	 * earlier, keep-up debt 72 vs 167 ticks; Astra ruling: reverse only on a
	 * reproducible >5% p99 frame-time regression). memory.mesh_blocks=0
	 * restores the per-list ladder path. */
	displaylist_set_block_mode(config_read_int(&gstate.config_user,
		"memory.mesh_blocks", 1) == 1);
	if(hw_run.enabled) {
		// debug.hw_overlay (brief 25 item 4): the ONE run-scoped switch over
		// the forced F3 overlay. Absent (the default below) or non-zero is
		// today's behaviour exactly; 0 declines the force for this run so a
		// console capture can photograph a clean HUD, and the overlay then
		// follows "debug.overlay" like normal play. Read HERE, after the
		// --cw-set loop above, so a staged run can pass
		// "+set:debug.hw_overlay=0" without touching config.json. The
		// predicate is a pure read of hw_run -- no profile field moves, so no
		// profile's verdict rules do either.
		if(hw_run_force_overlay(&hw_run,
								config_read_int(&gstate.config_user,
												"debug.hw_overlay", 1)))
			hw_run_config_set_string(&gstate.config_user, "debug.overlay", "force");
		if(hw_run.autoload_name[0])
			hw_run_config_set_string(&gstate.config_user, "autoload",
									 hw_run.autoload_name);
		else if(hw_run.autoload_new)
			hw_run_config_set_string(&gstate.config_user, "autoload", "+new");
		// --cw-seed (harness fixed-seed lane, 2026-09-04): read DIRECTLY into
		// screen_select_world.c's harness seed override, never through a
		// "debug.*" config key -- see hw_run.h's cfg->seed comment for why.
		// hw_run.seed_set == false (absent flag) leaves the override unset,
		// byte-identical to before this field existed.
		if(hw_run.seed_set)
			screen_sworld_set_seed_override(true, hw_run.seed);
		/* split_recovery_6m: arm the phase driver. A profile that is not
		 * split_recovery_6m leaves it disarmed, and a disarmed driver is a
		 * no-op everywhere below (and refuses the rig world create). */
		s_split_recovery_seed_set = hw_run.seed_set;
		s_split_recovery_seed = hw_run.seed;
		split_recovery_arm(&hw_run, &(struct split_recovery_hooks) {
			.rpc_guest = sr_hook_rpc_guest, .rpc_host = sr_hook_rpc_host,
			.emit = sr_hook_emit,
			.profile_done = sr_hook_profile_done,
			.create_world = sr_hook_create_world,
			.arm_autoload = sr_hook_arm_autoload,
			.block_at = sr_hook_block_at,
			.autoload_conflict = sr_hook_autoload_conflict});
		/* host_vehicle_smoke_8m: arm the vehicle phase driver (and its
		 * probe). Any other profile, or a missing kind, leaves it disarmed. */
		vehicle_smoke_arm(&hw_run, &(struct vehicle_smoke_hooks) {
			.emit = vs_hook_emit, .profile_done = vs_hook_profile_done,
			.telemetry_lost = vs_hook_telemetry_lost,
			.create_world = vs_hook_create_world,
			.arm_autoload = sr_hook_arm_autoload,
			.autoload_conflict = sr_hook_autoload_conflict,
			.rpc_host = sr_hook_rpc_host, .rpc_guest = sr_hook_rpc_guest},
			CW_BUILD_ID, strlen(hw_run.run_id));
		split_offer_piston_arm(&hw_run, &(struct vehicle_smoke_hooks) {
			.emit = vs_hook_emit, .profile_done = vs_hook_profile_done,
			.telemetry_lost = vs_hook_telemetry_lost,
			.create_world = sop_hook_create_world,
			.autoload_conflict = sr_hook_autoload_conflict});
		if(hw_run.save_reload) {
			s_save_reload_started = time_get();
			save_reload_arm(&(struct save_reload_hooks) {
				.emit = srl_hook_emit, .create_world = srl_hook_create_world,
				.arm_reload = srl_hook_arm_reload}, hw_run.run_id);
		}
		if(vehicle_smoke_armed() || split_offer_piston_armed())
			/* the profile's split payload policy, like split_dims_6m */
			hw_run_config_set_string(&gstate.config_user,
									 "split.world_payloads", "on");
		split_portal_quit_arm(&hw_run, &(struct split_portal_quit_hooks) {
			.emit = spq_hook_emit, .arm_portal = spq_hook_arm_portal,
			.arm_autoload = spq_hook_autoload, .block_at = spq_hook_block,
			.saved_block_at = spq_hook_saved_block,
			.done = spq_hook_done});
		/* A BEGIN from an earlier profile or phase is outside this run. */
		s_spq_last_switch_begin_seq = gstate.world_switch_begin_seq;
		/* mp_chat_login_3m: arm the chat/OSK phase driver. Same rule --
		 * any other profile leaves it disarmed, and a disarmed driver is a
		 * no-op everywhere (both input.c seam sites answer false on their
		 * first statement, so nothing binds and nothing is emitted). */
		chat_login_arm(&hw_run, &(struct chat_login_hooks) {
			.emit = cl_hook_emit,
			.profile_done = cl_hook_profile_done,
			.chat_lines_sent = cl_hook_chat_lines_sent});
		/* mp_play_10m: arm the play/rejoin phase driver. Same rule again --
		 * any other profile leaves it disarmed and mp_play_frame() returns on
		 * its first statement, so nothing is observed, no action is
		 * requested, autopilot's seam is never asserted and not one marker is
		 * emitted.
		 *
		 * chat_login_arm() above left the login machine DISARMED for this
		 * profile (it accepts only mp_chat_login_3m / mp_chat_chord_1m),
		 * which is exactly what this driver needs: it arms the SAME login
		 * implementation itself, through chat_login_arm_play(), once per
		 * connection generation, with its own hook set and its own verdict
		 * (Astra ruling 1 answer B3: CALL the shared login, do not copy it).
		 *
		 * The username is whatever --cw-mp-username delivered -- the same
		 * value injected as the ephemeral "mp.username" below, so the name
		 * the fixture counts logins for and the nonce every marker carries
		 * are ONE string and can never disagree. An empty one is passed
		 * through as empty: the run then joins under the card's saved name
		 * and the collector's nonce match fails honestly, rather than main
		 * inventing an identity nothing on the wire agrees with. */
		if(hw_run.profile == HW_RUN_PROFILE_MP_PLAY_10M)
			mp_play_arm(&(struct mp_play_hooks) {
				.emit = mpp_hook_emit,
				.profile_done = mpp_hook_profile_done,
				/* The observation rings, through their OWN accessor so the
				 * heartbeat and the connect screen stop carrying them
				 * (push-0921/B18, review B17 H2). The driver's only
				 * remote-observation hook since B19 item 0. */
				.remote_play_obs = mpp_hook_remote_play_obs,
				/* The watched cell (push-0921/B19): the one cell whose echo a
				 * terrain burst must not be able to evict. */
				.remote_watch = mpp_hook_remote_watch,
				/* The rejoin half (push-0921/B9). Lane A's title and
				 * Multiplayer focus observers arrived with its merge, so this
				 * is no longer NULL -- see mpp_hook_menu_focus above for what
				 * each answer means, and why those two PRODUCTION observers
				 * are the only acceptable source for it. */
				.menu_focus = mpp_hook_menu_focus,
				.body_ticking = mpp_hook_body_ticking,
				/* The SAME hook the login driver gets, so one run cannot
				 * observe two different chat-line counts. */
				.chat_lines_sent = cl_hook_chat_lines_sent},
				hw_run.mp_username, hw_run.run_id);
		if(hw_run.profile == HW_RUN_PROFILE_MP_PAUSE_MENU_3M)
			mp_pause_menu_arm(&(struct mp_pause_menu_hooks) {
				.emit = mpp_hook_emit,
				.profile_done = mpp_hook_profile_done,
				.chat_lines_sent = cl_hook_chat_lines_sent},
				hw_run.mp_username);
#ifndef PLATFORM_WII
		/* PC-ONLY: debug.mpp_short_play_ms cuts measured play and waives the
		 * floors so the rejoin half can be exercised in minutes. Compiled out
		 * of every Wii build; the session marker states the floor used and the
		 * collector refuses any but the contract's. */
		if(hw_run.profile == HW_RUN_PROFILE_MP_PLAY_10M) {
			int sp = config_read_int(&gstate.config_user,
									 "debug.mpp_short_play_ms", 0);
			if(sp > 0)
				mp_play_set_short_play((uint32_t)sp);
		}
#endif
		if(hw_run.superflat)
			hw_run_config_set_string(&gstate.config_user, "debug.superflat", "on");
		/* split_dims_6m: the production showcase pane (lit, five blocks
		 * ahead of the standing host, none through the host) is the
		 * guest's portal -- see split_dims_portal_step. */
		s_split_dims_armed = hw_run.enabled
			&& hw_run.profile == HW_RUN_PROFILE_SPLIT_DIMS_6M;
		if(s_split_dims_armed) {
			/* host switch first, then the guest crossing */
			hw_run_config_set_string(&gstate.config_user, "debug.portal_demo",
									 "split_dims");
			/* each view renders its own world (split.world_payloads) */
			hw_run_config_set_string(&gstate.config_user,
									 "split.world_payloads", "on");
		}
		// Superflat proof profiles are authored as cheap Overworld stages. Do not
		// inherit a developer's persistent start_nether setting: that combination
		// creates grass terrain in DIM-1 and used to poison it with skylight.
		if(hw_run.force_overworld)
			hw_run_config_set_string(&gstate.config_user, "debug.start_nether", "off");
		if(hw_run.perf_stress)
			hw_run_config_set_string(&gstate.config_user, "debug.perf_stress", "on");
		if(hw_run.autopilot_walk)
			hw_run_config_set_string(&gstate.config_user, "debug.autopilot", "walk");
		if(hw_run.autopilot_keepup)
			hw_run_config_set_string(&gstate.config_user, "debug.autopilot", "keepup");
		if(hw_run.autopilot_aim) {
			hw_run_config_set_string(&gstate.config_user, "debug.autopilot", "aim");
			hw_run_config_set_int(&gstate.config_user, "debug.autopilot_pitch_deg",
								  hw_run.autopilot_pitch_deg);
			hw_run_config_set_int(&gstate.config_user, "debug.autopilot_yaw_deg",
								  hw_run.autopilot_yaw_deg);
			if(hw_run.autopilot_place)
				hw_run_config_set_string(&gstate.config_user,
										 "debug.autopilot_place", "on");
		}
		if(hw_run.redstone_clock_stress)
			hw_run_config_set_string(&gstate.config_user,
									 "debug.redstone_clock_stress", "on");
		if(hw_run.mob_motion_rig)
			hw_run_config_set_string(&gstate.config_user,
									 "debug.mob_motion_rig", "on");
		/* Fable S2 F2 (Opus review B1): the mob-move cost counters run ONLY
		 * on the profiles that read them -- mob_motion_2m (the fixture) and
		 * perf_stress_5m (the cost vs the G7 baseline). Every other profile
		 * keeps its heartbeat bytes and pays no per-move timebase reads. */
		if(hw_run.profile == HW_RUN_PROFILE_MOB_MOTION_2M
		   || hw_run.profile == HW_RUN_PROFILE_PERF_STRESS_5M)
			mob_motion_cost_enable(cw_mob_cost_now_ns);
		if(hw_run.save_migrate) {
			hw_run_config_set_string(&gstate.config_user,
									 "debug.save_migrate", "on");
			hw_run_config_set_int(&gstate.config_user,
								  "debug.save_migrate_regions",
								  hw_run.save_migrate_regions);
		}
		if(hw_run.menu_autopilot) {
			hw_run_config_set_string(&gstate.config_user,
									 "debug.menu_dwell_sign_demo", "on");
			// --cw-cworld-mode / menus_dwell_creative (2026-09-02): read
			// DIRECTLY from the struct field, never through a "debug.*"
			// config key -- see hw_run.h's cworld_mode comment for why.
			menu_autopilot_arm(hw_run.cworld_mode == HW_RUN_CWORLD_MODE_CREATIVE);
			/* push-0921 lane A: the pointer-suppressed leg walks the
			 * Multiplayer strip on OBSERVED focus. screen_multiplayer_setup.c
			 * lives in the MP source pool, which the engine-core walker may
			 * not link against, so its two read-only observers are handed
			 * over here with the saved-row availability answer. Without this line the walker latches
			 * np_mps_unobserved and skips the Multiplayer half by name. */
			menu_autopilot_set_mps_observers(screen_mps_focus_target,
											 screen_mps_view,
											 screen_mps_server_count);
		}
		// world_copy_convert (ui-build-1026/T16a): the "Back up & convert"
		// proof route. The worlds root is a PER-RUN folder that holds only the
		// rig-staged disposable fixture, so no user world is ever listed, let
		// alone pressed (the perf_guard_root precedent below). wcc_root was
		// validated [A-Za-z0-9_]{1,40} by hw_run_parse_args. The rig-only
		// fault seams are armed here and nowhere else.
		if(hw_run.world_copy_convert) {
			char wcc_worlds[96]; /* copied by the setter; arm only compares */
			snprintf(wcc_worlds, sizeof(wcc_worlds),
					 "sd:/apps/CavEX/wcc_runs/%s", hw_run.wcc_root);
#ifndef PLATFORM_WII
			/* PC rehearsal: a relative per-run root beside the app. */
			snprintf(wcc_worlds, sizeof(wcc_worlds), "wcc_runs/%s",
					 hw_run.wcc_root);
#endif
			/* REVIEW ROUND 1 (Sol B1): isolation is a precondition, not a
			 * hope. A failed setter passes an empty expected root, and the
			 * arm reads "paths.worlds" BACK: unless it is exactly this
			 * per-run folder the route is refused by name (no presses, no
			 * listing, no disk writes) and no fault seam is armed. */
			bool wcc_set = hw_run_config_set_string(&gstate.config_user,
													"paths.worlds", wcc_worlds);
			/* copied into the route's heap block by the arm */
			const struct menu_autopilot_copy_hooks wcc_hooks = {
				.sworld_settled = screen_sworld_settled,
				.sworld_selected_row = screen_sworld_selected_row,
				.sworld_row_of_directory = screen_sworld_row_of_directory,
				.sworld_row_needs_convert = screen_sworld_row_needs_convert,
				.sworld_partial_copy_visible = screen_sworld_partial_copy_visible,
				.cb_state = screen_convert_backup_state,
				.cb_focused_action = screen_convert_backup_focused_action,
				.cb_fail = screen_convert_backup_fail,
				.cb_copy_error = screen_convert_backup_copy_error,
				.cb_percent = screen_convert_backup_percent,
				.cb_volume = screen_convert_backup_volume,
				.cb_convert_outcome = screen_convert_backup_convert_outcome,
				.cb_backup_published = screen_convert_backup_backup_published,
				.cb_interrupt = screen_convert_backup_rig_interrupt,
			};
			if(menu_autopilot_arm_copy_route(hw_run.wcc_variant,
											 "cw_wcc_fixture",
											 wcc_set ? wcc_worlds : "",
											 &wcc_hooks)) {
				/* enum menu_ap_copy_variant: 1 low_space, 2 write_error */
				screen_convert_backup_rig_arm_fault(
					hw_run.wcc_variant == 1 ? 1
					: hw_run.wcc_variant == 2 ? 2 : 0);
				fprintf(stderr, "[hw_run] world_copy_convert: worlds root %s\n",
						wcc_worlds);
			} else {
				fprintf(stderr, "[hw_run] world_copy_convert REFUSED: worlds "
						"root not isolated (setter %s); route not run\n",
						wcc_set ? "ok" : "FAILED");
			}
		}
		// mp_reconnect_soak (S5.3 hunt plumbing): arm the connect/dwell/
		// disconnect/reconnect cycle driver. Cycle 1's connect rides the
		// SAME boot-time mp_autoconnect screen_set() below (this profile
		// always sets both flags -- hw_run.c's HW_RUN_PROFILE_MP_RECONNECT_
		// SOAK case), so no extra action is needed here beyond arming the
		// state machine that watches for it.
		// --cw-mp-endpoint (2026-09-12): the run-local server override for an
		// unattended mp_connect_smoke run. hw_run_parse_args accepts the flag
		// ONLY on that profile (hw_run.c's post-loop gate), so a non-empty
		// field here can only mean mp_connect_smoke asked for it -- no second
		// profile test is needed, and every other profile keeps reading the
		// card's own mp.server byte-identically to before this flag existed.
		//
		// Injected as an EPHEMERAL "mp.server" rather than handed straight to
		// mp_session_request_connect: the connect SCREEN (screen_multiplayer_
		// connect.c mpc_begin_connect) is what reads mp.server, and routing
		// the override through it keeps the run on the REAL user surface this
		// profile exists to exercise -- including, on Wii, the screen's
		// net_bringup gate ahead of the connect request. Pre-queueing a
		// session request here instead would be OVERWRITTEN by the screen's
		// own request anyway (mp_session.c's single g_pending slot).
		// hw_run_config_set_string pairs the set with config_mark_ephemeral,
		// so config_save() strips it from the scratch copy before anything
		// reaches the card -- the override can never be baked into the staged
		// config.json by an Options visit mid-run (the GB18 "+new"-sentinel
		// leak guard every other injection in this block relies on).
		if(hw_run.mp_endpoint[0])
			hw_run_config_set_string(&gstate.config_user, "mp.server",
									 hw_run.mp_endpoint);
		/* --cw-mp-username: the identity half of the same override, through the
		 * SAME ephemeral path and the same connect screen (which reads
		 * mp.username next to mp.server). The value is a per-run nonce the
		 * collector binds the server's [login-gate] receipt to, so baking it
		 * onto the card would poison every later run -- config_mark_ephemeral
		 * is what makes that impossible. */
		if(hw_run.mp_username[0])
			hw_run_config_set_string(&gstate.config_user, "mp.username",
									 hw_run.mp_username);
		if(hw_run.mp_reconnect_soak)
			mp_reconnect_soak_arm();
	}
	{
		char detail[96];
		snprintf(detail, sizeof(detail), "start_nether=%s superflat=%s autoload=%s",
				 config_read_string(&gstate.config_user, "debug.start_nether", "off"),
				 config_read_string(&gstate.config_user, "debug.superflat", "off"),
				 config_read_string(&gstate.config_user, "autoload", ""));
		hw_boot_trace_write("config_effective", &hw_run, detail, false);
	}

	// ONE loud line naming the RESOLVED texture-pack path and whether it
	// exists. The pack path used to be consumed CWD-relative, so an
	// HBC-direct launch silently missed it and EVERYTHING (textures, sounds,
	// music) fell back to bundled defaults with no hint why -- the
	// two-config/two-CWD trap (PROJECT_MEMORY parts 9-11). Same detail goes
	// to the boot trace for staged runs (the config_effective precedent).
	{
		const char* pack = config_read_path(&gstate.config_user,
											"paths.texturepack", "assets");
		struct stat pack_st;
		bool pack_found = stat(pack, &pack_st) == 0;
		fprintf(stderr, "[BOOT] texture pack \"%s\" %s\n", pack,
				pack_found ? "found" : "MISSING -- bundled defaults in use");
		char pack_detail[192];
		snprintf(pack_detail, sizeof(pack_detail), "texturepack=%s exists=%s",
				 pack, pack_found ? "yes" : "NO");
		hw_boot_trace_write("texturepack_effective", &hw_run, pack_detail,
							false);
	}

	// F3-style perf HUD. Off unless "debug": {"overlay": "on"} is set in the
	// active config — the reliable way to enable it on the Wii build under
	// Dolphin, where the Nunchuk profile leaves no spare button to bind.
	// IB_DEBUG_OVERLAY (F3 on PC) toggles it live at runtime. The "force" value
	// pins it ON every frame (see force_debug_overlay) for headless captures, where
	// no controller is bound to toggle it and a stray reset must not lose the HUD.
	{
		const char* ov = config_read_string(&gstate.config_user, "debug.overlay",
											"off");
		gstate.force_debug_overlay
			= strcmp(ov, "force") == 0 || strcmp(ov, "always") == 0;
		gstate.debug_overlay
			= gstate.force_debug_overlay || strcmp(ov, "on") == 0;
	}

	// render-slice (RENDER_DISPATCH.md "THE KEY RULING"): Beta's own
	// renderRainSnow radius lever -- 5 (default) or 10 when gameSettings.
	// fancyGraphics [740c583]. Both are Beta-shipped values; defaulting to
	// "off" (radius 5) is "Beta with fast graphics", not a T2 deviation.
	{
		const char* fg = config_read_string(&gstate.config_user,
											"render.fancy_graphics", "off");
		gstate.fancy_graphics = strcmp(fg, "on") == 0;
	}

	// Leaves follow the same Beta graphics level (BlockLeaves.setGraphicsLevel):
	// fast = opaque tile + culled inner faces, fancy = cut-out + all faces.
	// Set before any chunk is meshed.
	block_leaves_set_fancy(gstate.fancy_graphics);

	// M1 far-ring LOD reach dial: the full-detail core radius in chunks. Chunks
	// within it keep the full faithful mesh; beyond it they render the cheap
	// surface skin + skirts. Persisted via "lod":{"high_detail_radius":N}, and
	// retunable live with the hotbar +/- buttons while the F3 overlay is up (see
	// the input handler below; the hotbar scroll is suppressed in that mode).
	// Clamped to the loaded view distance -- you can't skin past what is loaded.
	// Default 4 on Wii (whole view on PC). 2 skinned rings 3-5 inside Beta's own
	// reach, so buildings popped in (maintainer betamc report 2026-11-01).
	// Real-Wii 53a039f (hw1101f/g, same stats): r2 perf_stress 27 frames >30 ms
	// / far_walk 49; r4 24 / 0; r5 71 / 49; oom/trunc/spill 0 at every radius.
	// Maintainer picked 4.
	{
#ifdef PLATFORM_PC
		int lod_default = MAX_VIEW_DISTANCE;
#else
		int lod_default = 4;
#endif
		int r = config_read_int(&gstate.config_user, "lod.high_detail_radius",
								lod_default);
		if(r < 0)
			r = 0;
		if(r > MAX_VIEW_DISTANCE)
			r = MAX_VIEW_DISTANCE;
		world_lod_set_near_radius(r);
	}

	// M1 deep-reach far-ring distance ("lod.far_view_distance"): the outer skin ring
	// streams chunks in the annulus (MAX_VIEW_DISTANCE, far_vd]. Default per platform
	// (PC = MAX_VIEW_DISTANCE+1; Wii = MAX_VIEW_DISTANCE, i.e. the ring is OFF until the
	// on-device MEM1 measurement clears raising it). == MAX_VIEW_DISTANCE -> empty
	// annulus (off). Overrides the fog/far plane set earlier (so the ring isn't
	// fog-culled) and drives the server annulus scan. Clamped to [view, CAP].
	int far_vd = config_read_int(&gstate.config_user, "lod.far_view_distance",
								 MAX_FAR_VIEW_DISTANCE);
	if(hw_run.enabled && hw_run.far_view_distance >= MAX_VIEW_DISTANCE)
		far_vd = hw_run.far_view_distance;
	if(far_vd < MAX_VIEW_DISTANCE) {
		// The platform default is never below MAX_VIEW_DISTANCE, so a value here
		// means the config EXPLICITLY set the ring below the near view -- clamping
		// silently turned the headline far-ring feature OFF for anyone who set it
		// (config.json 8 / config_pc.json 6 vs PC MAX 10 did exactly that). Say so.
		fprintf(stderr,
				"[config] lod.far_view_distance %d is below the near view distance "
				"%d; clamping to %d (far ring OFF). Set it above %d to enable the "
				"far ring (cap %d).\n",
				far_vd, MAX_VIEW_DISTANCE, MAX_VIEW_DISTANCE, MAX_VIEW_DISTANCE,
				MAX_FAR_VIEW_DISTANCE_CAP);
		far_vd = MAX_VIEW_DISTANCE;
	}
	if(far_vd > MAX_FAR_VIEW_DISTANCE_CAP)
		far_vd = MAX_FAR_VIEW_DISTANCE_CAP;
	gstate.config.render_distance = far_vd * 16.0F + 32.0F;
	gstate.config.fog_distance = far_vd * 16.0F;
	s_single_render_distance = gstate.config.render_distance;
	s_single_fog_distance = gstate.config.fog_distance;
	/* Split render reach (T2, docs/local_splitscreen.md): chunks beyond this
	 * many chunks from both cameras are dormant while two cameras are active;
	 * fog is pulled to the reach. Default 4 (rings 5+6 = ~6.5 MiB of the two-
	 * anchor readiness burst). 0 = full rings (the pre-2026-09-10 behaviour). */
	world_set_split_render_reach(config_read_int(&gstate.config_user,
		"split.render_reach", 4));
	server_local_set_split_reach(world_split_render_reach());
	/* next-gaps-0924 W1F: the persisted Beta difficulty option (GameSettings
	 * loadOptions, GameSettings.java:234-235) seeds the local server before
	 * any world runs; the Options row keeps it live afterwards. */
	server_local_request_difficulty(config_read_difficulty(&gstate.config_user));
	/* host_vehicle_smoke_8m: the explicit Peaceful functional fixture
	 * (CONTRACT-v1 run_start difficulty 0), from the armed profile only. */
	if(vehicle_smoke_armed() || split_offer_piston_armed())
		server_local_request_difficulty(DIFFICULTY_PEACEFUL);
	/* split_dims_6m: Nether mobs hit/push the host on Normal and the run
	 * reports a misleading dims_not_active. The profile forces Peaceful no
	 * matter what the card config or -Set says ("difficulty forced to
	 * Peaceful by profile"; the collector summary states it). */
	if(hw_run.enabled && hw_run_profile_forces_peaceful(hw_run.profile))
		server_local_request_difficulty(DIFFICULTY_PEACEFUL);

	// Stage-2 LOD S2 far-storage compression (docs/stage2_lod_plan_2026-09-03.md;
	// world.h's enum world_far_storage): default flipped to "skin" in the
	// far-reach program Wave 1 (docs/far_reach_program_2026-09-03.md W1-a,
	// 2026-09-03) -- proven byte-identical emitted display lists to the old
	// "voxel" default (tests/test_far_storage_equiv.c; chunk_get_block
	// recomputes the identical cell from the ~1 KB column skin,
	// world_build_far_section / chunk.h's struct far_column_skin) while
	// cutting the dominant far-column MEM2 term ~20x (8x10240B block buffers
	// -> 8x513B skins). "voxel" remains selectable via config for a fallback
	// to the pre-S2 real block buffers. Render-only storage choice, read once
	// at startup like world_lod_set_near_radius above -- never touched by the
	// mesher worker itself.
	const char* far_storage_str
		= config_read_string(&gstate.config_user, "lod.far_storage", "voxel"); // RESTORE 2026-09-03: 2026-09-02 default (skin re-lands behind a hardware gate)
	enum world_far_storage far_storage = strcmp(far_storage_str, "voxel") == 0
		? WORLD_FAR_STORAGE_VOXEL
		: WORLD_FAR_STORAGE_SKIN;
	// --cw-far-storage override (S2 measurement, docs/stage2_lod_plan_2026-09-03.md):
	// HW_RUN_FAR_STORAGE_UNSET (default) leaves the config value above in
	// control; an armed hw_run profile can force VOXEL/SKIN to measure a
	// specific storage mode independent of whatever config.json the staged
	// card carries -- same override-only-when-armed pattern as far_vd above.
	if(hw_run.enabled && hw_run.far_storage != HW_RUN_FAR_STORAGE_UNSET)
		far_storage = hw_run.far_storage == HW_RUN_FAR_STORAGE_SKIN
			? WORLD_FAR_STORAGE_SKIN
			: WORLD_FAR_STORAGE_VOXEL;
	world_set_far_storage(far_storage);
	fprintf(stderr, "[config] lod.far_storage = %s (effective: %s)\n",
			far_storage_str,
			far_storage == WORLD_FAR_STORAGE_VOXEL ? "voxel" : "skin");

	// W1-b (far-reach program Wave 1, docs/far_reach_program_2026-09-03.md
	// "Fog-end clipping"): "lod.fog_clip" default "1.13" is the historical
	// faithful BFS render-reach multiplier; "1.0" clips exactly at the fog-
	// opaque radius R (world.h's world_set_fog_clip doc comment has the
	// analytic proof pointer). String-keyed like the toggles above --
	// config.h has no float reader, and this is a two-value dial, not a
	// tunable continuum. Any value other than "1.0" keeps the faithful
	// default rather than silently misparsing a typo to the clipped value.
	const char* fog_clip_str
		= config_read_string(&gstate.config_user, "lod.fog_clip", "1.13");
	world_set_fog_clip(strcmp(fog_clip_str, "1.0") == 0 ? 1.0F : 1.13F);

	// T2 SPIKE (opt-in, PC-measured only; hardware-unconfirmed -- see
	// docs/far_ring_geomip_design_2026-08-26.md): far-ring geomip error-metric
	// merge. "lod.geomip" default "off" is the faithful path -- byte-identical
	// emitted far-ring vertices to before this spike (test_far_geomip_flagoff_
	// equiv). "lod.geomip_error_px" is the screen-space error budget in WHOLE
	// pixels (an int, matching config.h's int-only reader; de Boer-style
	// thresholds are small integers, so this doesn't need a float config key).
	// world_lod_set_geomip is called AFTER gfx_setup() below, once gfx_height()
	// resolves the real resolution (see the input_init() comment a few lines
	// down for why -- gfx_height() is stale until then).
	const char* geomip_str
		= config_read_string(&gstate.config_user, "lod.geomip", "off");
	bool geomip_on = strcmp(geomip_str, "on") == 0;
	int geomip_budget_px
		= config_read_int(&gstate.config_user, "lod.geomip_error_px", 2);
	if(geomip_budget_px < 0)
		geomip_budget_px = 0;

	if(hw_run.enabled && hw_run.profile == HW_RUN_PROFILE_PERF_STRESS_5M
	   && config_read_int(&gstate.config_user, "debug.service_diagnostic_int", 0) == 1) {
		/* Named human diagnostic: no synthetic actions, same simulation
		 * fixture/cadence. Music forcing is an explicit diagnostic setting. */
		hw_run_config_set_string(&gstate.config_user, "debug.autopilot", "off");
		hw_run_config_set_string(&gstate.config_user, "debug.autopilot_place", "off");
		hw_run_config_set_string(&gstate.config_user, "debug.music_force", "on");
	}
	service_observer_configure(config_read_int(&gstate.config_user, "debug.service_observer_int", 0) == 1, CW_BUILD_ID, config_read_int(&gstate.config_user, "debug.service_observer_int", 0) == 1 ? service_observer_config_hash(gstate.config_user.root) : 0);
	if(!g9_capture_configure(json_object_dotget_number(json_object(gstate.config_user.root),
		"debug.g9_capture_int") == 1.0)) {
		hw_telemetry_emit_profile_done("g9_capture_reservation_failed");
		return 1;
	}
	g9_probe_configure(!g9_capture_enabled() && config_read_int(&gstate.config_user,
		"debug.g9_cpu_probe_int", 0) == 1, config_read_int(&gstate.config_user,
		"debug.g9_cpu_sampler_int", 0) == 1);
	g9_probe_main_configure(!g9_capture_enabled() && config_read_int(&gstate.config_user,
		"debug.g9_main_trace_int", 0) == 1);
	struct config_g9_pacing pacing = config_read_g9_pacing(&gstate.config_user);
	thread_main_pacing_configure(pacing.preset != 0, pacing.slice_us,
		pacing.sleep_ms, pacing.cap_ms);
	struct config_g9_lease lease = config_read_g9_lease(&gstate.config_user);
	thread_g9_lease_configure(lease.enabled, lease.max_us, lease.service_us);
	thread_g9_handoff_configure(lease.main_handoff);
	struct config_g9_tick_boost tick_boost = config_read_g9_tick_boost(&gstate.config_user);
	thread_g9_tick_boost_configure(tick_boost.enabled, tick_boost.cap_ms);
	/* G9 R20 Q3 worst-tick attribution probe (diagnostic, default OFF). */
	g9_worst_tick_configure(config_read_int(&gstate.config_user,
		"debug.g9_worst_tick_int", 0) == 1);
	/* G9 decode deadline promotion rides the same opt-in knob; it is active
	 * only while a local server owns the boost (audio_music_ring.h). */
	audio_music_decode_promotion_configure(tick_boost.enabled);
	column_light_configure(config_g9_settled_light_reuse(&gstate.config_user));
	/* G9 R19: deferred region save IO (T2, default OFF). Fixed for the whole
	 * process, set before any world (and so any region handle) opens. */
	region_archive_deferred_io_enable(config_g9_save_io(&gstate.config_user));
	region_archive_save_zlib_level_configure(config_g9_save_zlib_level(&gstate.config_user));
	thread_g9_lease_sender(hw_telemetry_lease_priority);
	blocks_init();
	items_init();
	bool g9_diag_channels = hw_run.enabled && config_read_int(
		&gstate.config_user, "debug.g9_drain_isolated_int", 0) == 1;
	if(g9_diag_channels) {
		g9_diag_run_id = hw_run.run_id;
		if(!clin_init() || !svin_init()
		   || !server_world_g9_diagnostic(emit_g9_diagnostic)) {
			hw_telemetry_emit_profile_done("g9_drain_diagnostic_failed");
			hw_telemetry_shutdown();
			return EXIT_FAILURE;
		}
#ifdef PLATFORM_PC
		while(g9_probe_pump(hw_telemetry_emit_observer_retry)) {}
#endif
	}
	render_entity_init();
	render_piston_moving_init();

	recipe_init();
	/* BOOT-CREDITS-1028: the loader service is installed before gfx_setup's
	 * tex_init, so the boot's first cached-backdrop PNG decode is inside a
	 * measured MEM1/MEM2 window (no cards yet: it only samples + pets). */
	/* ingame-import-1030: finish or roll back an import swap a power cut
	 * interrupted, before any texture is read (beta_import.h). */
	beta_import_recover();
	boot_gate_preinit();
	if(!gfx_setup()) {
		fprintf(stderr, "[startup] graphics initialization failed\n");
		if(hw_run.enabled)
			hw_telemetry_emit_profile_done("gfx_init_failed");
		hw_telemetry_shutdown();
		return EXIT_FAILURE;
	}

	/* Load-world cards render real inventory blocks before the first world
	 * frame. Seed the existing lightmap now; otherwise its zero-initialized
	 * entries turn those fullbright menu icons black. World rendering still
	 * supplies its own dimension/weather lightmap every frame. */
	gfx_update_light(1.0F, 0.0F, 0.0F, light_lookup_overworld);

	// T2 SPIKE geomip activation (see the config-read block above): deferred to
	// here because it needs gfx_height() resolved to the REAL resolution, which
	// (per the input_init() comment immediately below) is only true after
	// gfx_setup() returns. Underwater's FOV scale (camera.c's 6/7 multiplier)
	// and any future live FOV/resolution change are NOT re-applied to k after
	// this -- a documented spike-scope limitation, not a bug (see the design
	// doc).
	world_lod_set_geomip(geomip_on, (float)geomip_budget_px, gstate.config.fov,
						 (float)gfx_height());

	// input_init() must run AFTER gfx_setup(): on Wii it calls
	// WPAD_SetVRes(gfx_width(), gfx_height()), and gfx_width() returns the
	// stale static initializer (802) until gfx_setup() resolves the real
	// 4:3=640 / 16:9=802. Calibrating the IR pointer to 802 on a 4:3 console
	// skews GUI hit-testing by ~25% (802/640 ~= 1.2531). The init functions
	// above read no input, so the reorder is safe. [C8]
	input_init();
#ifdef CAVEWII_AGENT_BRIDGE
	// CAVEWII_AGENT bridge ACT-half: start the stdin command reader (no-op unless armed
	// by CAVEWII_AGENT=1). Pairs with the PERCEIVE-half state_export_emit in the tick loop.
	agent_input_init();
#endif

	// S0 audio: claim the PCM arena, decode-at-load the sound registry from
	// <paths.texturepack>/sound/ with per-file fallback to assets/sound/,
	// and open the platform backend (AESND / miniaudio). Before world load.
	// "audio.enabled" ("on" default) + "audio.volume" (0..100, default 100);
	// Beta only initializes the audio library when the volume option is
	// nonzero [740c583: SoundManager.loadSoundSettings] -- audio_init
	// mirrors that gate internally.
	{
		bool audio_on = strcmp(config_read_string(&gstate.config_user,
												  "audio.enabled", "on"),
							   "on")
			== 0;
		int audio_vol
			= config_read_int(&gstate.config_user, "audio.volume", 100);
		// Lane C "Loading sounds..." (Astra ruling 1, c3-loading-sounds-0919;
		// seam handed over by lane A for this card). The registry scan inside
		// audio_init decodes every sound file on THIS thread -- ~31 s on the
		// console (measured audio_load ms=30815), and it runs muted or not:
		// audio_init scans even with audio off. Until now the TV showed one
		// plain white frame for all of it. Present the static notice FIRST,
		// for real, so it is on screen before the first byte is decoded and
		// stays there as the last presented frame. No dwell, no progress.
		// The boot-trace marks (armed hw_run only) carry their own ms stamps:
		// present_ms (cost of the notice), scan_ms (the decode itself) and
		// since_notice_ms on audio_init_done AND on main_loop_enter, so the
		// notice, the scan and the first menu frame are timed SEPARATELY and
		// the early frame can never pass for a faster boot.
		// Debug proof capture of that frame (path prefix; absent = off).
		loading_notice_set_capture(config_read_string(
			&gstate.config_user, "debug.loading_notice_capture", NULL));
		{
			char notice_detail[48];
			s_boot_notice_time = time_get();
			s_boot_notice_time_set = true;
			/* BOOT-CREDITS-1028: card 1 (BagHeads) replaces the routine
			 * "Loading sounds..." frame; the old notice only when the card
			 * assets are missing. */
			if(boot_gate_begin(&hw_run)) {
				snprintf(notice_detail, sizeof(notice_detail), "credits=1");
			} else {
				snprintf(notice_detail, sizeof(notice_detail), "present_ms=%d",
						 loading_notice_present_boot());
			}
			hw_boot_trace_write("loading_notice_presented", &hw_run,
								notice_detail, false);
		}
		ptime_t scan_start = time_get();
		audio_init(audio_on, audio_vol);
		{
			char scan_detail[64];
			snprintf(scan_detail, sizeof(scan_detail),
					 "scan_ms=%d since_notice_ms=%d",
					 (int)time_diff_ms(scan_start, time_get()),
					 (int)time_diff_ms(s_boot_notice_time, time_get()));
			hw_boot_trace_write("audio_init_done", &hw_run, scan_detail,
								false);
		}

		// S3: background music (docs/finish_line_plan.md S3). A SEPARATE
		// GameSettings VOLUME from "audio.volume"
		// [740c583: SoundManager.onSoundOptionsChanged/
		// playRandomMusicIfReady both gate on options.musicVolume
		// independently of options.soundVolume -- either one nonzero keeps
		// the shared backend device open, audio.c's audio_backend_wanted];
		// "music.volume" mirrors "audio.volume"'s 0..100/default-100 shape.
		// "audio.enabled" stays the ONE shared outer gate for both (a
		// CaveWii headless/dev switch with no Beta equivalent, not a
		// per-slider thing) -- audio_music_init still always scans the
		// registry + starts the decode worker so audio_music_track_count()
		// and later a config toggle work correctly, it just cannot open the
		// device while audio.enabled is off, mirroring audio_init itself.
		int music_vol
			= config_read_int(&gstate.config_user, "music.volume", 100);
		audio_music_init(music_vol);
	}
	// Beta view bobbing toggle (GameSettings.viewBobbing default true
	// [740c583 GameSettings:21]); 0 disables, matching Beta's Video Settings
	// switch until that sub-screen exists (screen_options.c keeps GuiOptions'
	// faithful row set, and View Bobbing lives in GuiVideoSettings).
	gstate.config.view_bobbing
		= config_read_int(&gstate.config_user, "render.view_bobbing", 1) != 0;
	// Beta TextureCompassFX.onTick / TextureWatchFX.onTick always animate.
	// gfx.item_needle_anim=0 is the explicit visual/performance opt-out.
	gstate.config.item_needle_anim
		= json_object_dothas_value_of_type(json_value_get_object(gstate.config_user.root),
			"gfx.item_needle_anim", JSONBoolean)
		? json_object_dotget_boolean(json_value_get_object(gstate.config_user.root),
			"gfx.item_needle_anim") == 1
		: config_read_int(&gstate.config_user, "gfx.item_needle_anim", 1) != 0;
#ifdef PLATFORM_WII
	if(hw_run.enabled) gfx_guard_scan_benchmark(s_guard_scan_us);
#endif
	if(!watchdog_init()) { // prio-100; gfx is up so the panic banner can paint
		fprintf(stderr, "[startup] watchdog initialization failed\n");
		if(hw_run.enabled)
			hw_telemetry_emit_profile_done("watchdog_init_failed");
		hw_telemetry_shutdown();
		return EXIT_FAILURE;
	}

	/* Boot entry is the title screen (Beta GuiMainMenu with the CaveWii
	 * identity); Singleplayer leads to world-select, the old boot target.
	 * Exception: an autoload-armed hw-run profile has no hand to press
	 * Singleplayer -- eac3f9e's title screen stalled every headless
	 * profile at boot (found 2026-08-17, two independent traces). Armed
	 * rigs enter world-select directly, where the "+new" sentinel
	 * (screen_select_world.c) resumes the pre-eac3f9e flow; a profile
	 * that wants the title screen sets autoload_new=false. */
	if(hw_run.enabled && (hw_run.autoload_new || hw_run.autoload_name[0]))
		screen_set(&screen_select_world);
	else
		screen_set(&screen_title);

	world_create(&gstate.world);
	/* BG-BAKE-1028: the menu-backdrop bake host. debug.backdrop_bake_selftest=1
	 * runs one measured bake at the title (telemetry line; PC also writes
	 * backdrop_bake_selftest.png to the working directory). Off by default. */
	bake_host_register(config_read_int(&gstate.config_user,
		"debug.backdrop_bake_selftest", 0) == 1);

	for(size_t k = 0; k < 256; k++)
		gstate.players[0].windows[k] = NULL;

	if((!g9_diag_channels && (!clin_init() || !svin_init()))
	   || !chunk_mesher_init()) {
		fprintf(stderr, "[startup] worker channel initialization failed\n");
		if(hw_run.enabled)
			hw_telemetry_emit_profile_done("worker_init_failed");
		/* M2: bounded region I/O quiesce alongside the telemetry sender-s.
		 * The helper parks forever and is never joined, so an abnormal exit
		 * has to stop it touching ra->file before the world tears down. */
		region_archive_io_quiesce();
		hw_telemetry_shutdown();
		// C1 (candidate, hardware-unconfirmed): watchdog_init() above already
		// succeeded, so its detector thread is armed -- stop it before this
		// startup-failure return, same as every other EXIT_FAILURE path past
		// that point (idempotent/no-op on PC and if it somehow never armed).
		watchdog_shutdown();
		return EXIT_FAILURE;
	}
	particle_init();

	dict_entity_init(gstate.entities);
	sign_text_store_init();
	gstate.players[0].local_player = NULL;
	gstate.players[0].in_water = false;
	gstate.players[0].oxygen = MAX_OXYGEN;

	// Debug test kit: when "debug": {"give": "on"} is set in the active config,
	// the server fills the hotbar after an ordinary/death load with the current
	// dimension's mechanic kit (see server_local_give_test_kit). Live portal
	// crossings preserve the player's exact inventory.
	bool give_test_kit
		= strcmp(config_read_string(&gstate.config_user, "debug.give", "off"),
				 "on")
			== 0
		|| config_read_int(&gstate.config_user, "debug.give", 0) == 1;
	/* ^ integer 1 = the cw_run_args "--cw-set debug.give=1" form, so a console
	 * capture run can show a filled hotbar (item rows at y424) -- 2026-09-11. */
	// CREATIVE MODE is a PROPERTY OF THE WORLD, not a config
	// (maintainer decision 1, 2026-09-01 --
	// docs/ui_design/create_world_console_2026-09-01.md section 8). The old
	// "gameplay.creative" session-global switch is RETIRED: the mode is chosen
	// in the create-world dialog, stored as LEVEL_GAME_MODE in level.dat, and
	// armed per world -- gstate.config_creative by the client at the moment it
	// requests the load (screen_select_world.c), server_local.creative_mode by
	// the server when it opens that same level.dat. Both default to survival,
	// so a world with no tag (every world created before 2026-09-01, and every
	// imported Beta save) behaves exactly as it did.
	// Debug ("debug.chest_selftest"): run the no-input chest-persistence self-test
	// once after world load; result -> chest_selftest_result.txt on the SD card.
	bool chest_selftest
		= strcmp(config_read_string(&gstate.config_user, "debug.chest_selftest",
								   "off"),
				 "on")
		== 0;
	// Debug ("debug.fluid_demo"): build a no-input water/lava showcase at spawn
	// for fluid verification / screenshots (esp. on real Wii / Dolphin).
	bool fluid_demo
		= strcmp(config_read_string(&gstate.config_user, "debug.fluid_demo",
								   "off"),
				 "on")
		== 0;

	// Debug ("debug.water_demo"): seal the spawn in a glass water tank so the
	// player is submerged with no input -- shows the Beta air-bubble HUD.
	bool water_demo = strcmp(config_read_string(&gstate.config_user,
												"debug.water_demo", "off"),
							 "on") == 0;
	gstate.water_demo = water_demo;

	// Debug ("debug.zombie_demo"): spawn a zombie next to the idle player so the
	// Beta mob->player melee damage + knockback can be screenshotted with no input.
	bool zombie_demo = strcmp(config_read_string(&gstate.config_user,
												 "debug.zombie_demo", "off"),
							  "on") == 0;

	// Debug ("debug.superflat"): generate a flat bedrock/dirt/grass world (no
	// caves/trees/ores) -- a clean stage for testing mobs.
	bool superflat = strcmp(config_read_string(&gstate.config_user,
											   "debug.superflat", "off"),
							"on") == 0;

	// Debug ("debug.minecart_sound"): the DELIBERATE non-Beta minecart rolling
	// sound. Defaults ON because the maintainer asked for it; "off" restores
	// Beta 1.7.3's genuinely silent carts (EntityMinecart makes no playSound
	// call and no "minecart.*" key exists in 740c583) for audio parity work.
	gstate.config_minecart_sound
		= strcmp(config_read_string(&gstate.config_user, "debug.minecart_sound",
									"on"),
				 "off")
		!= 0;

	// GUI ("gui.death_hold_*_ms"): the death screen's hold-to-confirm
	// durations (deviation-#18 controller adaptation; see screen_death.c's
	// header comment). 0 restores the faithful Beta single click. Clamp after
	// parse -- the GameSettings.cpp discipline the borrow decisions adopted:
	// a hand-edited config must degrade to a sane value, never wrap or hang
	// the hold forever.
	gstate.config_death_hold_respawn_ms = config_read_int(
		&gstate.config_user, "gui.death_hold_respawn_ms", 750);
	if(gstate.config_death_hold_respawn_ms < 0)
		gstate.config_death_hold_respawn_ms = 0;
	if(gstate.config_death_hold_respawn_ms > 10000)
		gstate.config_death_hold_respawn_ms = 10000;
	gstate.config_death_hold_title_ms = config_read_int(
		&gstate.config_user, "gui.death_hold_title_ms", 1500);
	if(gstate.config_death_hold_title_ms < 0)
		gstate.config_death_hold_title_ms = 0;
	if(gstate.config_death_hold_title_ms > 10000)
		gstate.config_death_hold_title_ms = 10000;

	// Debug ("debug.perf_stress"): spawn the mob cap + one explosion at world load
	// so the P0 perf instruments can be read off F3 with no input (see server_local).
	bool perf_stress = strcmp(config_read_string(&gstate.config_user,
												 "debug.perf_stress", "off"),
							  "on") == 0;

	// Debug ("debug.auto_screenshot"): periodically dump the GX framebuffer to a
	// fixed SD file (screenshot.png) once in-world, so a headless harness reads the
	// rendered frame (with the forced HUD) without injecting the screenshot button.
	// Also armed by the INTEGER 1: the hardware harness injects run-scoped
	// config through cw_run_args.txt "--cw-set key=value", which is int-only
	// (hw_run.c), and the card's root config.json is outside the stage
	// server's put/fetch allow-list -- so a console capture run arms this
	// with "--cw-set debug.auto_screenshot=1" (2026-09-11).
	gstate.auto_screenshot
		= strcmp(config_read_string(&gstate.config_user, "debug.auto_screenshot",
									"off"),
				 "on")
			== 0
		|| config_read_int(&gstate.config_user, "debug.auto_screenshot", 0) == 1;
	remote_capture_init(config_read_int(&gstate.config_user,
		"debug.remote_capture", 0) == 1, CW_BUILD_ID);
	/* Dormant proof hook for an explicitly named, existing local world. The
	 * profile/name predicates keep a stray debug key inert in ordinary play and
	 * in every create-new, multiplayer and split-screen run. */
	int hw_save_quit_after_seconds = config_read_int(
		&gstate.config_user, "debug.hw_save_quit_after_seconds", 0);
	bool hw_save_quit_capture = config_read_int(
		&gstate.config_user, "debug.hw_save_quit_capture", 0) == 1;
	bool hw_save_quit_enabled = hw_run.enabled
		&& hw_run.profile == HW_RUN_PROFILE_FAR_IDLE_2M
		&& hw_run.autoload_name[0] != '\0'
		&& strcmp(hw_run.autoload_name, "1") != 0 && !hw_run.split_guest
		&& hw_save_quit_after_seconds > 0;

	// Debug ("debug.autopilot"): drive the local player on a scripted walk + look with no
	// human input, so a headless PC capture run can survey the world (esp. the distant LOD
	// skin) unaided. Drives the REAL physics via forced input + authored camera. Default off.
	// "debug.autopilot": "walk"/"on" = scripted survey (forced walk + authored look);
	// "aim"/"look" = hold an EXACT config-set look (debug.autopilot_pitch_deg / _yaw_deg,
	// ry=90 level, larger=look down) while forcing NO buttons -- lets synthetic input
	// (computer-use) drive move/mine/place when Dolphin's emulated analog stick (look)
	// cannot be deflected by synthetic input. See tools/dolphin_synthetic_input.md.
	// "keepup" (2026-09-04, HW_RUN_PROFILE_KEEPUP_WALK_6M): mode 2 -- a
	// deterministic keep-up-load walk (fixed heading, bounded sidestep-only
	// escape) that does NOT drift like "walk"'s curving yaw -- see hw_run.c's
	// autopilot_keepup field comment and autopilot_keepup.h for why "walk"
	// itself is non-deterministic across real-Wii runs.
	const char* ap_mode
		= config_read_string(&gstate.config_user, "debug.autopilot", "off");
	bool ap_walk = strcmp(ap_mode, "on") == 0 || strcmp(ap_mode, "walk") == 0;
	bool ap_aim = strcmp(ap_mode, "aim") == 0 || strcmp(ap_mode, "look") == 0;
	bool ap_keepup = strcmp(ap_mode, "keepup") == 0;
	gstate.autopilot.active = ap_walk || ap_aim || ap_keepup;
	gstate.autopilot.mode = ap_aim ? 1 : (ap_keepup ? 2 : 0);
	gstate.autopilot.tick = 0;
	gstate.autopilot.yaw = 0.0F;
	gstate.autopilot.pitch = GLM_PI * 0.5F + 0.45F;
	gstate.autopilot.place_enabled = false;
	gstate.autopilot.place_period = 0;
	gstate.autopilot.pending_place = 0;
	// Opt-in walker stuck-detector (autopilot_unstick.h): wired DIRECTLY from
	// hw_run.autopilot_unstick, never through a "debug.*" config key, so a
	// hand-edited or capture config can never enable it (capture configs must
	// stay deterministic -- see the header's comment). Only
	// HW_RUN_PROFILE_MP_LAN_WALK_10M sets this true (hw_run.c); every other
	// profile/manual launch leaves it false and the detector never
	// accumulates state.
	autopilot_unstick_reset(&gstate.autopilot.unstick, hw_run.autopilot_unstick);
	// Keep-up mode's deterministic sidestep detector (autopilot_keepup.h):
	// armed exactly when mode==2 (ap_keepup), mirroring the unstick reset
	// above. Every other mode leaves this a dead struct.
	autopilot_keepup_reset(&gstate.autopilot.keepup, ap_keepup);
	// Promo capture only: walk while looking down at a held map (0 = unchanged).
	autopilot_walk_pitch_set_deg(config_read_int(
		&gstate.config_user, "debug.autopilot_walk_pitch_deg", 0));
	if(ap_aim) {
		int pitch_deg
			= config_read_int(&gstate.config_user, "debug.autopilot_pitch_deg", 130);
		int yaw_deg
			= config_read_int(&gstate.config_user, "debug.autopilot_yaw_deg", 0);
		gstate.autopilot.pitch = (float)pitch_deg * GLM_PI / 180.0F;
		gstate.autopilot.yaw = (float)yaw_deg * GLM_PI / 180.0F;
		// Deterministic block-place ("debug.autopilot_place":"on"): the camera authoring
		// already locks an exact aim; this makes the game inject the place(s) itself, so a
		// piston/redstone showcase needs NO synthetic input (immune to Dolphin Nunchuk/focus
		// flakiness). period<=0 = one place ~2s after spawn; period>0 = one place every N ticks.
		gstate.autopilot.place_enabled
			= strcmp(config_read_string(&gstate.config_user, "debug.autopilot_place",
										"off"), "on")
			== 0;
		gstate.autopilot.place_period
			= config_read_int(&gstate.config_user, "debug.autopilot_place_period", 0);
	}

	// static, NOT a stack local: struct server_local is ~100KB+ (map cache,
	// entity dict), and libogc's main-thread stack is only 128KB (ogc.ld puts
	// __stack_addr 0x20000 above BSS). As a local it pushed main()'s frame to
	// 188KB -- the prologue's stwu dropped SP ~57KB below the stack floor into
	// .bss, and boot died at FAT init (the 2026-08-25 8/8 pre-FAT red-band
	// regression, first-bad 87e2029). Keep huge objects out of stack frames.
	static struct server_local server;
	server_local_init(&server, give_test_kit, chest_selftest, fluid_demo,
					  water_demo, zombie_demo, superflat, perf_stress);
	/* Diagnostic v2: default OFF. Each named run gets its own save root;
	 * a manual/debug-only launch cannot arm this simulation exception. */
	bool perf_guard_requested = config_read_int(&gstate.config_user,
		"debug.perf_stress_hold_health_int", 0) == 1;
	bool perf_guard_local = hw_run.enabled
		&& hw_run.profile == HW_RUN_PROFILE_PERF_STRESS_5M && perf_stress;
	s_perf_guard_observe = perf_guard_requested;
	char perf_guard_root[256] = {0};
	if(perf_guard_requested && perf_guard_local) {
		for(const char* p = hw_run.run_id; *p; p++)
			if(!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
				|| (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
				perf_guard_local = false;
		if(!hw_run.run_id[0] || !strcmp(hw_run.run_id, "manual")) perf_guard_local = false;
		if(perf_guard_local) {
			snprintf(perf_guard_root, sizeof(perf_guard_root),
				"saves/perf_stress_guard_v2/%s", hw_run.run_id);
			hw_run_config_set_string(&gstate.config_user, "paths.worlds", perf_guard_root);
		}
	}
	server_local_perf_guard_configure(&server, perf_guard_requested,
		perf_guard_local, perf_guard_root);
	/* R6: freeze the comparison input BEFORE the first server tick. This is
	 * a profile instrument, never a config/play-mode setting. Loading,
	 * lighting and meshing keep draining through their ordinary paths. */
	server.pool_selftest_freeze = hw_run.enabled && hw_run.pool_selftest;
	s_spq_server = &server;
	// server.creative_mode is NOT set here any more: it is a per-world value
	// the server reads from the loaded level.dat (server_local_load_world_impl).
	// `server` is a zero-initialised static, so it starts survival.
	// debug.weather_force ("rain"/"thunder"/"off") wins when present;
	// debug.weather_force_int (0/1/2) is the --cw-set hardware alias.
	// Armed main.c-side
	// like every debug.* key so the server never reads client config
	// (layering); applied once in server_weather_seed. Same two-phase
	// contract as creative_mode above.
	weather_force_from_config(&gstate.config_user, &server.weather.debug_force);
	// Interim multiplayer title-screen path (plan
	// docs/mp_online_client_plan_2026-08-15.md section 3.3). true from the
	// frame the provider swap below honors a connect request until the
	// remote engine has fully torn down and server_local has resumed --
	// see the #ifdef CAVEWII_MP_CLIENT block right before clin_update().
	bool mp_provider_remote = false;
#ifdef CAVEWII_MP_CLIENT
	/* B5 (2026-08-20 redesign): last time the remote provider was seen in
	 * REMOTE_PLAY (or the swap started). While remote is active but not
	 * playing past MP_SWAP_DEADLINE_MS, the seam force-cancels and restores
	 * server_local -- singleplayer can never be left silently dead. */
	ptime_t mp_swap_nonplay_since = time_get();
#define MP_SWAP_DEADLINE_MS 15000u
#endif

	// M1 far ring mode ("lod.far_mode"), set before the server thread starts:
	//   "trees"               = cheap skin gen (1 chunk/send, ~10x cheaper -> the ring
	//                fills with a walk) + an APPROXIMATE trunk silhouette so distant
	//                forests aren't bald (render-only LOD; trunks reposition on approach);
	//   "terrain"   (default) = the same cheap skin without approximate trunks;
	//   "populated" = the faithful full populate cascade per send (real decoration in
	//                the distant skin, slower fill). All keep the sim byte-identical.
	{
		const char* far_mode
			= config_read_string(&gstate.config_user, "lod.far_mode", "terrain");
		enum far_lod_mode mode;
		if(!far_lod_mode_parse(far_mode, &mode))
			fprintf(stderr,
					"[config] unknown lod.far_mode \"%s\"; using \"terrain\" "
					"(compact tree stubs disabled). Expected trees, terrain, or "
					"populated.\n",
					far_mode ? far_mode : "(null)");
		server.far_lod_terrain_only = mode != FAR_LOD_MODE_POPULATED;
		server.far_lod_trees = mode == FAR_LOD_MODE_TREES;
	}
	/* MAINTAINER RULING 2026-09-11: split-screen server load radius (default
	 * 4; single player keeps MAX_VIEW_DISTANCE). Read at every use through
	 * server_local_view_radius(); read BEFORE the anchor separation below so
	 * the setter applies the split policy with the dial live. */
	server.split_view_radius = config_read_int(&gstate.config_user,
		"split.server_view_radius", 4);
	if(server.split_view_radius > MAX_VIEW_DISTANCE)
		server.split_view_radius = MAX_VIEW_DISTANCE;
	/* MP ADMISSION RADIUS ("mp.install_radius", W1 /
	 * docs/mp_admission_budget_design_2026-09-13.md section 2.1): how far
	 * from the server-authoritative player column a REMOTE server's columns
	 * are installed at full detail. Distinct from the near-LOD radius (mesh
	 * detail) and from server.split_view_radius / MAX_VIEW_DISTANCE (the
	 * LOCAL server's own coverage, which streams nothing in an MP session).
	 * Read here, beside the split radius it is capped against; handed to the
	 * provider at connect time below. */
	_Static_assert(SERVER_REMOTE_ADMIT_RADIUS_MAX == MAX_VIEW_DISTANCE,
				   "the MP admission cap must track MAX_VIEW_DISTANCE; "
				   "server_remote.h mirrors it because the MP client "
				   "deliberately does not include server_local_state.h");
	/* F4: the DEFAULT is sized from the measured carve, not from the render
	 * cap -- 4 on the Wii, 10 on PC (SERVER_REMOTE_ADMIT_RADIUS_DEFAULT in
	 * server_remote.h carries the byte arithmetic and the betamc_adm_0913
	 * reading behind it). MAX_VIEW_DISTANCE remains the CEILING, asserted
	 * equal to SERVER_REMOTE_ADMIT_RADIUS_MAX just above, so a player can
	 * still dial the ring back out to the render distance. */
	int mp_install_radius = config_read_int(&gstate.config_user,
		"mp.install_radius", SERVER_REMOTE_ADMIT_RADIUS_DEFAULT);
	/* Under local split-screen the console is already paying for two anchors
	 * at split.server_view_radius (09-11 ruling), so an MP session must not
	 * claim a deeper install ring than the split budget allows. Same
	 * predicate split_experimental below is built from (the config key OR
	 * the harness flag) -- evaluated here because the admission radius
	 * belongs next to the radius it is capped against. */
	if((config_read_int(&gstate.config_user, "experimental.local_splitscreen", 0)
			!= 0
		|| (hw_run.enabled && hw_run.split_guest))
	   && server.split_view_radius > 0
	   && mp_install_radius > server.split_view_radius)
		mp_install_radius = server.split_view_radius;
	/* SPLIT_REACH_CAPPED_TO_RADIUS (Astra round 7): a render reach beyond the
	 * server radius draws nothing (nothing past the radius is loaded) and would
	 * make the far-ring diagnostic count terrain the server never streams. */
	if(server.split_view_radius > 0 && world_split_render_reach() > server.split_view_radius) {
		fprintf(stderr,
				"[config] split.render_reach %d is beyond split.server_view_radius %d; "
				"using %d\n",
				world_split_render_reach(), server.split_view_radius,
				server.split_view_radius);
		world_set_split_render_reach(server.split_view_radius);
		server_local_set_split_reach(world_split_render_reach());
	}
	if(hw_run.enabled
	   && !server_local_set_anchor_sep(&server, hw_run.anchor_sep)) {
		fprintf(stderr, "anchor scheduler allocation/setup failed\n");
		return 1;
	}
	/* Experimental local split-screen ("experimental.local_splitscreen",
	 * default OFF; --cw-split-guest arms it for a harness run). ON only
	 * pre-allocates the residency scheduler and arms the second controller
	 * claim -- nothing renders, joins or moves until a controller claims
	 * player 2 AND the server admits a body. OFF is byte-identical to the
	 * pre-split build: server_local_guest_gameplay refuses every admission,
	 * so local_gameplay_ready -- the two-view loop's gate -- stays false. */
	bool split_experimental
		= config_read_int(&gstate.config_user, "experimental.local_splitscreen", 0)
			  != 0
		|| (hw_run.enabled && hw_run.split_guest);
	if(split_experimental && !server_local_set_split_enabled(&server, true)) {
		fprintf(stderr,
				"[config] experimental.local_splitscreen: residency allocation "
				"failed; split-screen stays OFF\n");
		split_experimental = false;
	}
	input_split_join_arm(split_experimental);
	/* The Options row owns the switch from here (S-A2, Astra option A): seed it
	 * with what this boot path ACTUALLY applied, not with the config key -- a
	 * failed residency allocation above left split_experimental false while the
	 * key still says on, and the row must show the truth.
	 *
	 * This read of experimental.local_splitscreen is the BOOT one, and it is no
	 * longer the only one: screen_options_split_world_seed re-reads the key at
	 * every world generation in the loop below (R-A9 A9-M5), because
	 * server_local_set_split_enabled above can only be used while the server
	 * thread is stopped -- a later world load re-seeds through the live
	 * SRPC_SPLIT_SET route instead. */
	screen_options_split_boot(split_experimental);
	/* Falling-edge latch for the deferred-config flush at the ack seam below
	 * (A8-M6 / S-A5). Seeded false: no world is loaded yet here. */
	bool split_world_was_loaded = false;
	/* One settle per claim: the approved join card is retired exactly once,
	 * whether the admission succeeded or failed. */
	bool split_join_settled = false;
	bool split_guest_logged = false;
	bool split_final_logged = false;
	struct split_layout split_layout;
	split_layout_reset(&split_layout);
	ptime_t split_layout_clock = time_get();
	if(split_experimental)
		fprintf(stderr,
				"[config] experimental.local_splitscreen = on (EXPERIMENTAL: a "
				"second controller may press Join; see docs/local_splitscreen.md)\n");
	world_set_anchor_sep(hw_run.enabled ? hw_run.anchor_sep : 0);
	s_anchor.enabled = hw_run.enabled
		&& (hw_run.anchor_sep > 0
			|| hw_run.profile == HW_RUN_PROFILE_SPLIT_ANCHOR_6M
			|| hw_run.profile == HW_RUN_PROFILE_SPLIT_REGROW_8M
			|| hw_run.profile == HW_RUN_PROFILE_SPLIT_PLAY_2M
			|| hw_run.profile == HW_RUN_PROFILE_SPLIT_PLAY_8M
			/* S1-d: split_actions_6m carries the same two-anchor evidence
			 * shape as split_play_8m -- only the scripted guest actions are
			 * new. */
			|| hw_run.profile == HW_RUN_PROFILE_SPLIT_ACTIONS_6M
			/* split_dims_6m (C1) is an anchor + guest-body certifying
			 * profile: hw_run.c's measured window and the collector's
			 * ANCHOR_PROFILES both expect this observer's readiness latch. */
			|| hw_run.profile == HW_RUN_PROFILE_SPLIT_DIMS_6M);
	s_anchor.separation = hw_run.anchor_sep;
	/* A guest-body profile has two real anchors at synthetic separation 0
	 * (Astra ruling 2026-09-10): the readiness latch must wait for both. */
	s_anchor.guest = hw_run.enabled && hw_run.split_guest;
	/* The client mirror of the effective server load radius (the dial is read
	 * above, before the hw_run anchor separation is applied -- SPLIT_DIAL_BEFORE_SEP). */
	world_set_server_view_radius(server_local_view_radius(&server));
	gstate.autopilot.terrain_safe
		= hw_run.profile == HW_RUN_PROFILE_SPLIT_ANCHOR_6M
		|| hw_run.profile == HW_RUN_PROFILE_SPLIT_REGROW_8M
		|| hw_run.profile == HW_RUN_PROFILE_SPLIT_PLAY_2M
		|| hw_run.profile == HW_RUN_PROFILE_SPLIT_PLAY_8M
		|| hw_run.profile == HW_RUN_PROFILE_SPLIT_ACTIONS_6M; /* S1-d */
	s_anchor_server = &server;
	if(hw_run.profile == HW_RUN_PROFILE_SPLIT_ANCHOR_6M
	   || hw_run.profile == HW_RUN_PROFILE_SPLIT_REGROW_8M
	   || hw_run.profile == HW_RUN_PROFILE_SPLIT_PLAY_2M
	   || hw_run.profile == HW_RUN_PROFILE_SPLIT_PLAY_8M
	   || hw_run.profile == HW_RUN_PROFILE_SPLIT_ACTIONS_6M) { /* S1-d */
		world_lod_set_near_radius(2);
		world_set_far_storage(WORLD_FAR_STORAGE_VOXEL);
	}
	server.far_view_distance = far_vd; // the runtime far-ring distance (above)
	// T2 far-send share ("lod.far_send_near_owed", 2026-09-04, lane/far-send-share;
	// default flipped to 8 2026-09-04, lane/far-share-default-8;
	// docs/platform_perf_notes.md). How many still-unsent NEAR chunks the server
	// tolerates before letting the far ring take the near send loop's LEFTOVER
	// per-tick budget. Default 8: N=16 filled the far ring (far_missing median
	// 48 -> 21 while walking, seed 12345) but cost peaked (fps floor 10.4 -> 5.3,
	// server write max 13.5 -> 24.3 ms); 8 was chosen as the bounded setting --
	// see docs/platform_perf_notes.md "Default 8". 0 is still the strict
	// pre-2026-09-04 gate (the far batch runs only when the whole near square is
	// sent) and remains a fully supported opt-out. Raising or lowering it
	// changes only how much the far annulus fills while the near frontier is
	// still moving, guarded so no column the player is about to walk into is
	// far-sent. --cw-far-send-near-owed overrides it on an armed hardware run
	// (same pattern as far_vd/far_storage).
	{
		int far_owed = config_read_int(&gstate.config_user,
									   "lod.far_send_near_owed", 8);
		if(hw_run.enabled && hw_run.far_send_near_owed >= 0)
			far_owed = hw_run.far_send_near_owed;
		if(far_owed < 0)
			far_owed = 0;
		server.far_send_near_owed = far_owed;
		fprintf(stderr, "[config] lod.far_send_near_owed = %d (%s)\n", far_owed,
				far_owed == 0 ? "strict: far ring waits for near convergence"
							  : "shared: far ring uses leftover near budget");
	}
	// Per-game-tick FLOWING-FLUID fire budget. Default per platform
	// (FCAVEX_SCHEDULED_TICK_BUDGET); 0 = unlimited. Tuning it up trades
	// smoothness for faster settling. Non-fluid timing callbacks pass through
	// and may overtake capped fluid: an explicit Wii deviation documented in
	// wii_controller_playability.md. Negative/garbage -> 0 (unlimited).
	{
		int budget = config_read_int(&gstate.config_user,
									 "sim.scheduled_tick_budget",
									 FCAVEX_SCHEDULED_TICK_BUDGET);
		server.scheduled_tick_budget = budget < 0 ? 0 : (uint32_t)budget;
	}
	// Autosave interval (seconds; 0 disables). See server_local.h: the cheap
	// state checkpoints each interval, the chunk sweep spreads at 1/tick.
	{
		int autosave_s
			= config_read_int(&gstate.config_user, "sim.autosave_seconds", 30);
		server.autosave_ticks
			= autosave_s <= 0 ? 0 : (uint32_t)autosave_s * 20;
	}
	// Debug pig render demo (set after create, before the server thread starts).
	server.pig_demo = strcmp(config_read_string(&gstate.config_user,
												"debug.pig_demo", "off"),
							 "on")
		== 0;
	server.spider_demo = strcmp(config_read_string(&gstate.config_user,
												   "debug.spider_demo", "off"),
								"on")
		== 0;
	server.furnace_demo = strcmp(config_read_string(&gstate.config_user,
													"debug.furnace_demo", "off"),
								 "on")
		== 0;
	server.ice_demo = strcmp(config_read_string(&gstate.config_user,
												"debug.ice_demo", "off"),
							 "on")
		== 0;
	server.farmland_demo = strcmp(config_read_string(&gstate.config_user,
													 "debug.farmland_demo", "off"),
								  "on")
		== 0;
	server.fire_demo = strcmp(config_read_string(&gstate.config_user,
											 "debug.fire_demo", "off"),
							  "on")
		== 0;
	server.mob_lava_demo = strcmp(config_read_string(&gstate.config_user,
											 "debug.mob_lava_demo", "off"),
							  "on")
		== 0;
	server.nether_demo = strcmp(config_read_string(&gstate.config_user,
												   "debug.nether_demo", "off"),
								"on")
		== 0;
	{
		const char* pd = config_read_string(&gstate.config_user,
											"debug.portal_demo", "off");
		server.portal_demo = strcmp(pd, "off") != 0; // "on"/"showcase"/"split_dims"
		/* "split_dims" = the showcase pane plus the host frame lit once the
		 * guest is clear of it (split_dims_6m host switch). */
		server.portal_demo_split = strcmp(pd, "split_dims") == 0;
		server.portal_demo_showcase = strcmp(pd, "showcase") == 0
			|| server.portal_demo_split;
	}
	server.redstone_demo = strcmp(config_read_string(&gstate.config_user,
													 "debug.redstone_demo", "off"),
								  "on")
		== 0;
	server.redstone_drop_anim = strcmp(config_read_string(&gstate.config_user,
														  "debug.redstone_drop", "off"),
									   "on")
		== 0;
	server.redstone_clock_stress
		= strcmp(config_read_string(&gstate.config_user,
									"debug.redstone_clock_stress", "off"),
				 "on")
		  == 0;
	server.mob_motion_rig
		= strcmp(config_read_string(&gstate.config_user,
									"debug.mob_motion_rig", "off"),
				 "on")
		  == 0;
	// next-gaps-0924/G5 cascade fixture: the card config's string key, or the run-scoped INT key a
	// staged rig run sets with --cw-set debug.redstone_cascade_stress_int=1 (--cw-set is int-only).
	server.redstone_cascade_stress
		= strcmp(config_read_string(&gstate.config_user,
									"debug.redstone_cascade_stress", "off"),
				 "on")
			== 0
		|| config_read_int(&gstate.config_user, "debug.redstone_cascade_stress_int", 0) == 1;
	// G9 item 6 engaged pending-light workload: same string / run-scoped int pair.
	server.light_queue_stress
		= strcmp(config_read_string(&gstate.config_user,
									"debug.light_queue_stress", "off"),
				 "on")
			== 0
		|| config_read_int(&gstate.config_user, "debug.light_queue_stress_int", 0) == 1;
	// next-gaps-0924/w5 sleep-spawn fixture: same string / run-scoped int pair as the cascade one.
	server.sleep_spawn_stress
		= strcmp(config_read_string(&gstate.config_user,
									"debug.sleep_spawn_stress", "off"),
				 "on")
			== 0
		|| config_read_int(&gstate.config_user, "debug.sleep_spawn_stress_int", 0) == 1;
	// Unlike the visual demos, this profile deliberately downgrades and rewrites
	// a save fixture.  Never let a persistent config.json key arm it during a
	// normal run: require the parsed, ephemeral hardware-run profile as well.
	server.save_migrate_profile
		= hw_run.enabled && hw_run.save_migrate
		  && strcmp(config_read_string(&gstate.config_user,
									 "debug.save_migrate", "off"),
					"on")
				 == 0;
	server.save_migrate_regions = config_read_int(
		&gstate.config_user, "debug.save_migrate_regions", 8);
	if(server.save_migrate_regions < 2 || server.save_migrate_regions > 32)
		server.save_migrate_regions = 8;
	// Debug ("debug.menu_dwell_sign_demo"): the menus_dwell hardware profile's
	// sign-OSK evidence -- see server_local.c server_local_menu_dwell_sign_demo.
	server.menu_dwell_sign_demo
		= strcmp(config_read_string(&gstate.config_user,
									"debug.menu_dwell_sign_demo", "off"),
				 "on")
		  == 0;
	// Debug ("debug.sign_demo"): wave-3 in-world sign TEXT capture stage --
	// see server_local.c server_local_sign_demo.
	server.sign_demo = strcmp(config_read_string(&gstate.config_user,
												 "debug.sign_demo", "off"),
							  "on")
		== 0;
	// Debug ("debug.painting_demo"): PROJECT_MEMORY 2026-09-01 deferred item
	// #1 visual-proof stage -- see server_local.c server_local_painting_demo.
	server.painting_demo
		= strcmp(config_read_string(&gstate.config_user,
									"debug.painting_demo", "off"),
				 "on")
		  == 0;
	// Debug ("debug.map_demo"): replace the selected slot with a fresh map
	// after an ordinary load (server_local.c's deferred map give).
	server.map_demo = strcmp(config_read_string(&gstate.config_user,
												"debug.map_demo", "off"),
							 "on")
		== 0
		|| config_read_int(&gstate.config_user, "debug.map_demo", 0) == 1;
	server.piston_demo = strcmp(config_read_string(&gstate.config_user,
												   "debug.piston_demo", "off"),
								"on")
		== 0;
	// debug.piston_anim: a TIMED extend/retract showcase -- builds a regular + a sticky piston unpowered,
	// then the server toggles their power on a slow loop so a capture catches placed->extended->retracted
	// (the sticky pull vs the regular leaving its block). Shares the piston_demo deferred build path.
	server.piston_anim = strcmp(config_read_string(&gstate.config_user,
												   "debug.piston_anim", "off"),
								"on")
		== 0;
	// debug.piston_blast_demo: the D1+D3 explosion-vs-piston PROOF -- builds ONE up-facing piston
	// with a planks payload via the piston_demo deferred path, powers it after a ~5s settle window,
	// blasts the mid-move id-36 planks carrier, then writes piston_blast_result.txt (SD root) with
	// a server-computed PASS/FAIL verdict. Run it alone (piston_demo/piston_anim off).
	server.piston_blast_demo
		= strcmp(config_read_string(&gstate.config_user,
									"debug.piston_blast_demo", "off"),
				 "on")
		  == 0;
	// debug.nether_roundtrip_demo: the saved-chunk + DIM-1 round-trip PROOF (2026-07-27 hardware
	// report) -- builds byte-exact markers in both dimensions, drives the REAL portal travel
	// Over->Nether->Over->Nether, and re-verifies each marker set after its region-file reload;
	// verdict -> nether_roundtrip_result.txt (SD root). Run it alone (one rig per run, see
	// server_local.h) with debug.autopilot=aim so the dwell contact holds.
	server.nether_roundtrip_demo
		= strcmp(config_read_string(&gstate.config_user,
									"debug.nether_roundtrip_demo", "off"),
				 "on")
		  == 0;
	// debug.tnt_chain_demo: the TNT chain-reaction PROOF -- a 6-TNT line, ONE real prime, a
	// bounded chain window, then all-consumed / no-orphan-primed / pad-carved checks; verdict ->
	// tnt_chain_result.txt (SD root). Run it alone.
	server.tnt_chain_demo
		= strcmp(config_read_string(&gstate.config_user,
									"debug.tnt_chain_demo", "off"),
				 "on")
		  == 0 || config_read_int(&gstate.config_user,
									"debug.tnt_chain_demo_int", 0) != 0;
	// debug.piston_sticky_demo: the sticky-piston retraction PROOF -- extend pushes the wool,
	// unpower PULLS it back (Beta sticky retraction) with the moving store left empty; verdict ->
	// piston_sticky_result.txt (SD root). Run it alone.
	server.piston_sticky_demo
		= strcmp(config_read_string(&gstate.config_user,
									"debug.piston_sticky_demo", "off"),
				 "on")
		  == 0;
	// debug.world_reload_demo: the COLD-BOOT world reload PROOF (2026-07-27 hardware report:
	// "loading already-saved worlds is not confirmed"). "create" (boot 1, autoload "+new")
	// builds markers + a dropped item and runs a full checkpoint; "verify" (boot 2, autoload
	// "1", saves preserved by the prover) re-checks them byte-exact after the cold load;
	// verdict -> world_reload_result.txt (SD root). Run it alone.
	{
		const char* wr_mode = config_read_string(
			&gstate.config_user, "debug.world_reload_demo", "off");
		server.world_reload_create = strcmp(wr_mode, "create") == 0;
		server.world_reload_verify = strcmp(wr_mode, "verify") == 0;
	}
	server.render_demo_mob
		= config_read_int(&gstate.config_user, "debug.render_mob", -1);
	/* "split.world_payloads" (Split-dims B2, opt-in, default "off"; shipping
	 * it ON needs an Astra ruling): each split view renders its own
	 * world-bound client payload instead of both sharing gstate.world.
	 * Flipped once at boot, before any world context exists; "off" makes no
	 * call at all, so the default path is unchanged. split_dims_6m forces it
	 * on. */
	if(strcmp(config_read_string(&gstate.config_user, "split.world_payloads",
								 "off"),
			  "on")
	   == 0) {
		if(!clin_world_payloads_set_enabled(true))
			fprintf(stderr, "[startup] split.world_payloads=on refused: world "
							"payloads stay OFF\n");
		else {
			fprintf(stderr, "[startup] split.world_payloads=on: world-bound "
							"views enabled\n");
			fprintf(stderr, "[startup] split column budget: configured radius=%d, "
				"effective radius=%d (bounded mode supports 1..4; 0/full "
				"or larger values resolve to 4)\n",
				server.split_view_radius, server_local_view_radius(&server));
		}
	}
	/* Publish the complete server configuration before its thread can observe a
	 * field. Equal-priority Wii scheduling used to hide this startup data race. */
	if(!server_local_start(&server)) {
		fprintf(stderr, "[startup] server thread creation failed\n");
		if(hw_run.enabled)
			hw_telemetry_emit_profile_done("server_thread_failed");
		region_archive_io_quiesce(); /* M2: see the first EXIT_FAILURE above */
		hw_telemetry_shutdown();
		// C1 (candidate, hardware-unconfirmed): watchdog_init() above already
		// succeeded, so its detector thread is armed -- stop it before this
		// startup-failure return. chunk_mesher_init() has already spawned its
		// worker by this point too (a few lines above); joined here rather
		// than left to run past an EXIT_FAILURE the same way the shutdown
		// path below joins it past a clean exit.
		chunk_mesher_shutdown();
		watchdog_shutdown();
		return EXIT_FAILURE;
	}
#ifdef PLATFORM_WII
	if(hw_run.enabled && hw_run.profile == HW_RUN_PROFILE_FAR_IDLE_2M
	   && thread_main_context_register()) {
		s_freeze_server_thread = &server.thread;
		watchdog_context_observer_set(emit_watchdog_contexts);
	}
#endif
#ifdef CAVEWII_MP_CLIENT
	// Interim multiplayer title-screen path (plan section 3.3): construct
	// the (idle, socket-less) remote-provider object and register the
	// connecting screen so screen_title.c can offer it.
	mp_session_init();
	// S5.2: registered on BOTH platforms. On Wii the screen's MPC_BRINGUP
	// phase (net_bringup, background if_config) gates the connect request --
	// and with it the 15s provider-swap clock -- behind a live IP, so DHCP
	// settle can never trip the swap deadline.
	screen_multiplayer_connect = &screen_multiplayer_connect_impl;
	// The setup screen (name + saved servers) is screen_title.c's ONLY
	// route to the connect screen now (screen_title.c title_button_enabled/
	// title_activate); registered right alongside it, same NULL-until-
	// registered shape, so the two pointers are never out of sync.
	screen_multiplayer_setup = &screen_multiplayer_setup_impl;
	// Headless-verification lever (same spirit as the autopilot rigs): with
	// CAVEWII_MP_AUTOCONNECT set, queue the config-derived connect request at
	// boot exactly as the connecting screen's reset() would, so soak/E2E
	// tooling can join a server with zero synthesized input. The provider
	// swap still happens at the ONE main-loop seam below; gameplay behavior
	// is otherwise untouched (no env var, no effect).
	/* Round-5 measurement lever (same spirit as the rigs above): put the
	 * region save path back on the SYNCHRONOUS publication so a before/after
	 * far_walk comparison can be made with ONE binary, which removes every
	 * build difference from the measurement. No env var, no effect -- the
	 * asynchronous helper thread is the default. */
	if(getenv("CAVEWII_REGION_IO_SYNC"))
		region_archive_io_async_enable(false);
	if(getenv("CAVEWII_MP_AUTOCONNECT")) {
		const char* mp_cfg = config_read_string(&gstate.config_user,
												"mp.server", "");
		const char* mp_user_cfg = config_read_string(&gstate.config_user,
													 "mp.username", "");
		char ac_host[MP_ADDRESS_HOST_CAPACITY];
		uint16_t ac_port;
		/* ac_host[0]: mp_address_parse accepts ":25565" with an EMPTY host,
		 * and mp_session_request_connect would then silently no-op (its
		 * return was discarded) -- the mpc_reset parity guard, hostile-
		 * review low 14. */
		if(mp_cfg[0]
		   && mp_address_parse(mp_cfg, ac_host, sizeof(ac_host), &ac_port)
		   && ac_host[0] && mp_username_chosen(mp_user_cfg)) {
			if(!mp_session_request_connect(ac_host, ac_port, mp_user_cfg))
				fprintf(stderr,
						"[mp] CAVEWII_MP_AUTOCONNECT request rejected\n");
		} else
			fprintf(stderr,
					"[mp] CAVEWII_MP_AUTOCONNECT set but mp.server/mp.username "
					"invalid, empty, or not a chosen name (never CaveWii)\n");
	}
	// mp_connect_smoke (S5.3): boot straight into the connect screen -- the
	// exact flow a player's Multiplayer press takes (on Wii the screen runs
	// net_bringup BEFORE the connect request, so the swap clock still starts
	// only at READY), zero input. Unlike the env path above, this goes
	// THROUGH the screen so the run exercises the real user surface. The
	// screen reads mp.server from the user config; a missing/bad value lands
	// on its error screen and the collector's scenario gate FAILs the run
	// (a world can only appear via the remote install -- this profile arms
	// no autoload). An unattended run can redirect THAT read to a chosen
	// host:port with --cw-mp-endpoint, applied as an ephemeral "mp.server" in
	// the hw_run config-injection block above (search hw_run.mp_endpoint) --
	// this entry point itself is unchanged, so the override rides the exact
	// same screen/bringup flow a player's Multiplayer press does.
	if(hw_run.enabled && hw_run.mp_autoconnect && screen_multiplayer_connect)
		screen_set(screen_multiplayer_connect);
#endif

	ptime_t last_frame = time_get();
	ptime_t last_tick = last_frame;
	ptime_t hw_started = last_frame;
	ptime_t hw_last_heartbeat = last_frame;
	bool hw_heartbeat_sent = false;
	bool hw_profile_done_sent = false;
	// Hands-free hardware batch: timestamp of the "ok" profile_done send, used
	// only to measure the auto-exit grace window (hw_run_auto_exit_ready).
	// Initialized to last_frame (not left indeterminate) so the very first
	// time_diff_s call below -- taken before hw_profile_done_sent is true and
	// therefore always discarded by hw_run_auto_exit_ready's early-out -- is
	// still a well-defined ptime_t subtraction, not garbage.
	ptime_t hw_profile_done_at = last_frame;
	bool hw_watchdog_injected = false;
	uint32_t hw_tick = 0;
	uint32_t mpm_client_tick = 0;
	// T-b (2026-09-03, Round 3 (3) instrumentation-gap fix): monotonic count
	// of main-loop iterations since boot -- an independent liveness proof
	// that survives telemetry going fully silent (quiesced socket, dead
	// collector, or a genuine network drop): see the low-rate SD boot-trace
	// write a few lines into the loop below, and hb.frame_counter in the
	// heartbeat itself for the socket-connected view of the same number.
	uint32_t hw_frame_counter = 0;
	if(hw_run.enabled) {
		s_wait_hb.tick = &hw_tick;
		s_wait_hb.frame_counter = &hw_frame_counter;
		s_wait_hb.last = &hw_last_heartbeat;
		s_wait_hb.sent = &hw_heartbeat_sent;
		s_wait_hb.far_vd = far_vd;
		s_wait_hb.server = &server;
		s_wait_hb.armed = true;
	}
	spawn_search_wait_set_hooks(wait_heartbeat_service, wait_spawn_probes);
	bool previous_frame_playable = false;
	struct hw_frame_settle release_settle = {0};
	float frame_clin_ms = 0, frame_screen_ms = 0, frame_draw_ms = 0;
	float frame_heartbeat_ms = 0, frame_completed_ms = 0;
	float frame_input_ms = 0, frame_finish_ms = 0;
	// menus_dwell hardware profile: has this run's STEP_WAIT_SIGN soft-timeout
	// already been logged? A one-shot edge guard around menu_autopilot_
	// sign_wait_timed_out()'s own one-shot latch, so the trace line below
	// fires exactly once per run instead of every remaining frame.
	bool hw_sign_gate_logged = false;
	enum {
		HW_SAVE_QUIT_OFF,
		HW_SAVE_QUIT_WAIT_WORLD,
		HW_SAVE_QUIT_PAUSE_QUEUED,
		HW_SAVE_QUIT_EXIT_PENDING,
		HW_SAVE_QUIT_SUCCEEDED,
		HW_SAVE_QUIT_FAILED,
		HW_SAVE_QUIT_CAPTURE_WAIT,
		HW_SAVE_QUIT_CAPTURE_ARMED,
	} hw_save_quit_state = hw_save_quit_enabled ? HW_SAVE_QUIT_WAIT_WORLD
											 : HW_SAVE_QUIT_OFF;
	ptime_t hw_save_quit_world_ready_at = last_frame;
	ptime_t hw_save_quit_capture_at = last_frame;
	bool hw_save_quit_world_ready_seen = false;
	{
		char enter_detail[48] = "";
		if(s_boot_notice_time_set)
			snprintf(enter_detail, sizeof(enter_detail), "since_notice_ms=%d",
					 (int)time_diff_ms(s_boot_notice_time, time_get()));
		hw_boot_trace_write("main_loop_enter", &hw_run, enter_detail, false);
	}
	// The main loop is running now (the network has long since settled after telemetry
	// init at boot) -- allow the deferred first net_accept. Skipping it during the
	// early-boot emit_boot send is what avoids the Broadway net_accept hard-fault
	// (see hw_telemetry_arm_accept / the cw_boot_trace send_before_accept pinpoint).
	hw_telemetry_arm_accept();

	struct hw_run_tps_window tps_window = {0};
 bool settle_allow_far = false;
	size_t settle_tokens = 0;
resume_main_loop:
	while(!gstate.quit) {
		SERVICE_OBSERVER(service_observer_frame(gstate.world_switch_pending || gstate.players[0].current_screen == &screen_load_world ? SO_LOADING : gstate.world_loaded && !gstate.paused && gstate.players[0].current_screen == &screen_ingame ? SO_GAMEPLAY : SO_MENU));
		bool handoff_input_polled = false;
		uint64_t handoff_input_us = 0;
		g9_probe_main_frame(true);
		if(settle_observer_active()) {
			int32_t sx = WCOORD_CHUNK_OFFSET(
				(w_coord_t)floorf(gstate.players[0].camera.x));
			int32_t sz = WCOORD_CHUNK_OFFSET(
				(w_coord_t)floorf(gstate.players[0].camera.z));
			settle_observer_frame(
				&gstate.world,
				gstate.world_loaded && !gstate.world_switch_pending, sx, sz,
				world_server_view_radius() ? world_server_view_radius() : MAX_VIEW_DISTANCE,
				settle_allow_far, settle_tokens);
		}
		bool tps_was_measuring = tps_window.measuring;
		/* Which clock may end the window is the profile's own business
		 * (hw_run_tps_window_end): the run clock for most, the readiness clock
		 * for the profiles that OPEN on it, no clock at all for
		 * split_anchor_6m. s_anchor.elapsed_ms is this run's published
		 * "anchor_ms" as of the previous frame's note -- a one-frame lag
		 * against a 358 s threshold, and the same integer the completion
		 * decision reads, so the window and the run cannot disagree. */
		bool tps_done = hw_profile_done_sent || hw_telemetry_quiesced() ||
			(hw_run.tps_cpu
			 && hw_run_tps_window_end(&hw_run,
									  time_diff_s(hw_started, time_get()),
									  s_anchor.measuring, s_anchor.elapsed_ms));
		bool tps_measuring = hw_run_tps_measuring(&tps_window, &hw_run,
			gstate.world_loaded, s_anchor.measuring, tps_done,
			hw_run.tps_cpu ? tps_wall_now_ns() / UINT64_C(1000000) : 0);
		if(tps_measuring && !tps_was_measuring
		   && hw_run.profile != HW_RUN_PROFILE_SPLIT_ANCHOR_6M)
			hw_telemetry_emit_observer("{\"event\":\"tps_window\","
				"\"kind\":\"single_anchor\",\"settle_ms\":5000}");
		if(tps_measuring && !tps_was_measuring) g9_capture_arm();
		tps_observer_frame(tps_measuring);
		watchdog_heartbeat(); // THE stall signal: a stopped heartbeat for ~2s fires the dump
		hw_frame_counter++;
		if(hw_run.enabled) hw_telemetry_note_frame(hw_frame_counter);
		/* Loading-phase cadence (2026-09-08): the observer thins itself to
		 * <= 1 record/s while the world is still loading or the anchors are
		 * not ready -- s_anchor.measuring latches only after every anchor
		 * has been playable for HW_ANCHOR_READY_HEARTBEATS, so on the split
		 * scene the whole two-anchor reload is the loading phase. Without an
		 * armed anchor profile there is no readiness signal beyond
		 * world_loaded, and asking for more would leave such a run thinned
		 * forever. See carve_observer_loading. */
		carve_observer_loading(!gstate.world_loaded
			|| (s_anchor.enabled && !s_anchor.measuring));
		carve_observer_frame(&gstate.world, hw_frame_counter, hw_tick,
			watchdog_phase_srv_current(), s_anchor.measuring);
		/* One retained record before heartbeat bursts; retry a full ring next frame. */
		if(hw_telemetry_active() && !hw_telemetry_quiesced())
			(void)carve_observer_emit_next(hw_telemetry_emit_observer);
		/* asyncsnd-0926 (opt-in audio.async_load): join a finished pass-C
		 * loader / declare its progress timeout. One static test when off. */
		audio_registry_async_poll();
		/* Same rule for the BOOT audio scan's record: it happened inside
		 * audio_init, before the observer existed. Self-gating and one-shot. */
		audio_registry_emit_load_stats();
		/* Save-path census every 5 s (300 frames) once the world is up. */
		if(hw_run.enabled && gstate.world_loaded && hw_frame_counter % 300 == 0
		   && hw_telemetry_active() && !hw_telemetry_quiesced())
			(void)hw_emit_region_journal(hw_tick);
		// Diagnostic profiles retain a periodic SD proof-of-life record
		// independent of the socket. Performance profiles omit it: measured
		// synchronous flushes exceeded 60 ms and created gameplay hitches.
		// Network/RAM liveness and boot/shutdown/crash records remain active.
		float sd_trace_seconds = 0.0F;
		if(hw_run_periodic_sd_trace(&hw_run) && (hw_frame_counter % 60u) == 0u) {
			ptime_t trace_start = time_get();
			char fc_detail[384];
			const struct input_player_frame* guest_input = input_player_frame(1);
			struct input_device guest_device = {0};
			(void)input_player_device(1, &guest_device);
			snprintf(fc_detail, sizeof(fc_detail),
				"frame=%u split=%u guest_phase=%u guest_ready=%u p2_device=%u:%u connected=%u look=%.4f,%.4f",
				hw_frame_counter, (unsigned)split_experimental,
				(unsigned)gstate.players[1].local_phase,
				(unsigned)gstate.players[1].local_gameplay_ready,
				(unsigned)guest_device.kind, (unsigned)guest_device.channel,
				(unsigned)(guest_input && guest_input->connected),
				guest_input ? guest_input->look_x : 0.0,
				guest_input ? guest_input->look_y : 0.0);
			hw_boot_trace_write("frame_alive", &hw_run, fc_detail, false);
			if(gstate.players[1].menu_host.recovery.phase != CONTROLLER_RECOVERY_NONE) {
				const struct input_inventory* devices = input_devices();
				for(unsigned channel = 0; channel < 4; channel++) {
					const struct input_device_status* d = &devices->devices[channel];
					snprintf(fc_detail, sizeof fc_detail,
						"channel=%u observed=%u:%u published=%u:%u armed=%u reported=%u recovery=%u",
						channel, (unsigned)d->observed.connected, (unsigned)d->observed.category,
						(unsigned)d->published.connected, (unsigned)d->published.category,
						(unsigned)d->armed, (unsigned)d->identity_reported,
						(unsigned)gstate.players[1].menu_host.recovery.phase);
					hw_boot_trace_write("controller_recovery", &hw_run, fc_detail, false);
				}
			}
			sd_trace_seconds = time_diff_s(trace_start, time_get());
		}
#ifdef CAVEWII_AGENT_BRIDGE
		// Advance the agent button edge latch once per frame, before any input consumer
		// (camera look, screen handler) reads input this frame.
		agent_input_poll();
#endif
		watchdog_phase_main(WDM_CLIN_UPDATE);
		thread_main_pacing_frame();
		ptime_t this_frame = time_get();
		gstate.stats.dt = time_diff_s(last_frame, this_frame);
		/* Start-to-start timing includes render completion and VI waiting.
		 * Exclude transitions: both ends must belong to admitted gameplay.
		 * This runs before any gameplay dt cap and never resets on a heartbeat. */
		bool frame_playable = gstate.world_loaded && !gstate.world_switch_pending
			&& !gstate.world_load_failed && !gstate.remote_terrain_hold
			&& gstate.players[0].current_screen == &screen_ingame;
		if(s_anchor.enabled) {
			hw_anchor_evidence_note(&s_anchor, s_anchor.measuring ?
				(uint32_t)time_diff_ms(s_anchor_started, this_frame) : 0,
				(uint32_t)(gstate.stats.dt * 1e6f), mem2_stat_carve_free_kb());
			/* Every split profile gates the host walk on the readiness latch
			 * (Astra debate 2026-09-11, F: G measured with 850 host steps
			 * already walked). */
			if(hw_run.profile == HW_RUN_PROFILE_SPLIT_ANCHOR_6M
			   || hw_run.profile == HW_RUN_PROFILE_SPLIT_REGROW_8M
			   || hw_run.profile == HW_RUN_PROFILE_SPLIT_PLAY_2M
			   || hw_run.profile == HW_RUN_PROFILE_SPLIT_PLAY_8M
			   || hw_run.profile == HW_RUN_PROFILE_SPLIT_ACTIONS_6M) /* S1-d */
				gstate.autopilot.active = s_anchor.measuring;
			/* R5 item 3: split_regrow_8m's separate/reunite/separate route. */
			if(hw_run.anchor_cycle && s_anchor.measuring)
				anchor_cycle_step(&server, hw_run.anchor_sep,
					(uint32_t)time_diff_ms(s_anchor_started, this_frame));
		}
		/* Release-gate admission (maintainer 2026-10-31 option 2): both frame
		 * ends in admitted gameplay AND the post-load settle window elapsed
		 * (HW_FRAME_EVIDENCE_SETTLE_S). Nothing else reads this flag. */
		bool release_admit = hw_frame_settle_admit(&release_settle,
			previous_frame_playable && frame_playable, gstate.stats.dt);
		if(hw_run.enabled) {
			hw_frame_evidence_note_trace(&s_release_frames,
				release_admit, sd_trace_seconds);
			struct gfx_wait_timing wt = gfx_frame_wait_timing(true);
			hw_frame_evidence_note_wait(&s_release_frames,
				release_admit,
				wt.vi_ready_us, wt.gpu_poll_late_us);
		}
		if(hw_run.enabled) {
			float rpc_ms[4];
			clin_work_stats(rpc_ms);
			const float parts_ms[20] = {gstate.stats.ms_tick, gstate.stats.ms_recv,
				gstate.stats.ms_render, gstate.stats.ms_bfs, gstate.stats.ms_light,
				gstate.stats.ms_lod, gstate.stats.ms_build,
				gstate.stats.dt_gpu * 1000.0f, gstate.stats.dt_vsync * 1000.0f,
				frame_clin_ms, frame_screen_ms, frame_draw_ms,
				frame_heartbeat_ms, frame_completed_ms, frame_input_ms, frame_finish_ms,
				rpc_ms[0], rpc_ms[1], rpc_ms[2], rpc_ms[3]};
			hw_frame_evidence_note_parts(&s_release_frames,
				release_admit, gstate.stats.dt, parts_ms);
		}
		if(hw_run.enabled)
			hw_frame_evidence_note(&s_release_frames,
				release_admit,
				gstate.stats.dt, gstate.stats.dt_gpu);
		previous_frame_playable = frame_playable;
		/* Two frames can share one timestamp (coarse clock, or the first
		 * frame sampled against itself); 1/0 would put inf into the F3
		 * overlay and the telemetry stream. Keep the last measured FPS
		 * instead -- it is the freshest real ratio (bug hunt B6). */
		if(gstate.stats.dt > 0.0F)
			gstate.stats.fps = 1.0F / gstate.stats.dt;
		last_frame = this_frame;

		// menus_dwell hardware profile: advance the scripted menu navigation once
		// per frame, before any screen's update() reads input this frame (mirrors
		// agent_input_poll's documented contract above). A no-op unless
		// menu_autopilot_arm() was called (hw_run.menu_autopilot).
		menu_autopilot_set_front_end_blocked(boot_gate_blocks_title());
		menu_autopilot_frame_tick();
		/* save_reload_3m (R12): its one-frame menu press rides the same
		 * input seam (menu_autopilot_force_button). No-op unless armed. */
		if(save_reload_armed()) {
			const struct screen* cur = gstate.players[0].current_screen;
			save_reload_frame(&(struct srs_obs) {
				.ms = (uint32_t)time_diff_ms(s_save_reload_started, time_get()),
				.title_ready = cur == &screen_title && !boot_gate_blocks_title(),
				.world_loaded = gstate.world_loaded,
				.ingame = cur == &screen_ingame,
				.pause = cur == &screen_pause,
				.focus_exit = screen_pause_focused_action()
					== SCREEN_PAUSE_FOCUS_EXIT,
				.exit_pending = screen_pause_exit_pending(),
				.stats_modal = screen_stats_save_active(0)}, NULL);
		}

		// menus_dwell diagnostics (2026-09-11): one boot-trace line per walker
		// step transition (with the screen the driver classified) and per
		// consumed aim pulse (with the block the crosshair held), so a run
		// whose container legs misbehave explains itself from
		// cw_boot_trace.txt instead of from frame timing guesswork.
		if(hw_run.menu_autopilot) {
			static int hw_ap_last_step = -1, hw_ap_last_pulses = 0;
			int ap_step = menu_autopilot_step();
			if(ap_step != hw_ap_last_step) {
				hw_ap_last_step = ap_step;
				char ap_detail[96];
				snprintf(ap_detail, sizeof(ap_detail), "step=%d screen=%d",
						 ap_step, menu_autopilot_screen());
				hw_boot_trace_write("menu_ap_step", &hw_run, ap_detail, false);
			}
			if(gstate.autopilot.pulse_count != hw_ap_last_pulses) {
				hw_ap_last_pulses = gstate.autopilot.pulse_count;
				char pulse_detail[96];
				snprintf(pulse_detail, sizeof(pulse_detail),
						 "n=%d hit=(%d,%d,%d) block=%d", gstate.autopilot.pulse_count,
						 gstate.autopilot.pulse_x, gstate.autopilot.pulse_y,
						 gstate.autopilot.pulse_z, gstate.autopilot.pulse_block);
				hw_boot_trace_write("menu_ap_pulse", &hw_run, pulse_detail, false);
			}
		}

		// menus_dwell hardware-risk gate (2026-09-02, staged_menus_dwell_
		// 20260902_063552's adversarial review): STEP_WAIT_SIGN's timeout is
		// SOFT -- the script itself just skips ahead to pause/options (see
		// menu_autopilot_script.h) -- so a run that never opened the sign
		// otherwise explains nothing about WHY on its own; the next person
		// is left re-deriving it from scratch. Log the actual knobs that
		// gate the sign demo the instant the one-shot latch fires, so the
		// next run's cw_boot_trace.txt names the failing gate directly
		// instead of just a missing map_sign in telemetry.
		if(!hw_sign_gate_logged && menu_autopilot_sign_wait_timed_out()) {
			hw_sign_gate_logged = true;
			char sign_gate_detail[128];
			snprintf(sign_gate_detail, sizeof(sign_gate_detail),
					 "autopilot_aim=%d autopilot_place=%d pitch=%d yaw=%d",
					 hw_run.autopilot_aim ? 1 : 0, hw_run.autopilot_place ? 1 : 0,
					 hw_run.autopilot_pitch_deg, hw_run.autopilot_yaw_deg);
			hw_boot_trace_write("menu_ap_sign_gate_timeout", &hw_run,
								 sign_gate_detail, false);
		}

#ifdef CAVEWII_MP_CLIENT
		// Interim multiplayer title-screen path (plan section 3.2/3.3): the
		// ONLY seam that switches the active provider. server_local's thread
		// runs continuously from server_local_start() above and drains
		// svin_process_messages() every tick regardless of which screen is
		// showing, so server_remote_pump() (which drains the SAME mailbox)
		// may never run concurrently with it -- provider selection means
		// stopping one before starting the other, never both [plan 3.1].
		//
		// screen_multiplayer_connect.c's reset() only QUEUES the request
		// (mp_session_request_connect); this is the one place that actually
		// performs the swap, so it always happens between frames, never in
		// the middle of a server_local tick.
		{
			// ROUND 7 (main()-frame fix, PERMANENT -- these are the real
			// game's own MP-path locals, not hunt probes; kept static after
			// this round, same a91e593 boot-death precedent as
			// `static struct server_local server` above). This whole block
			// runs EVERY frame under CAVEWII_MP_CLIENT (not just while
			// connecting), so these ~194 bytes (129 + 65) were previously
			// reserved in main()'s frame unconditionally; mp_session_take_
			// pending_connect always fully overwrites both before any read,
			// so static (BSS-resident, main() is single-instance/single-
			// threaded) is behaviorally identical and frame-size-free.
			static char mp_host[MP_ADDRESS_HOST_CAPACITY];
			static char mp_user[MP_SESSION_USERNAME_CAPACITY];
			uint16_t mp_port;
			/* 2026-08-20 redesign, finding 3: a pending request the seam
			 * cannot serve RIGHT NOW is consumed and DROPPED, never
			 * retained -- a retained one used to fire frames later against
			 * a live singleplayer world. The connect screen queues a fresh
			 * request per visit, so nothing is lost. Finding 9's idle gate
			 * rides the same rule: no provider swap while a world is
			 * loaded/loading (back-to-title teardown still in flight), so
			 * server_local_shutdown below always runs against an idle,
			 * worldless server -- and the post-session restart resumes that
			 * same idle state. */
			/* Re-review finding 2: the busy gate must read the SERVER's own
			 * world-ownership flag (player.has_pos: set on load, cleared by
			 * server_local_release_world_impl), NOT gstate.world_loaded /
			 * world_switch_pending -- both client flags latch true across a
			 * quit-to-title (switch_pending only clears when the NEXT world
			 * load completes), which turned Multiplayer into a
			 * one-shot-per-process feature. Benign cross-thread bool read:
			 * worst case is one dropped request, which is now VISIBLE via
			 * the drop notice below (finding 3). */
			bool mp_swap_busy = mp_provider_remote || server.player.has_pos;
			if(mp_session_take_pending_connect(mp_host, sizeof(mp_host),
											   &mp_port, mp_user,
											   sizeof(mp_user))) {
				/* asyncsnd-0926 world-entry gate (Astra ruling 3): no remote
				 * connection starts while the opt-in async sound load is
				 * still pending. The request is CONSUMED (finding 3: never
				 * retained) and the sound gate screen takes over -- it
				 * re-enters the connect screen, whose reset queues a fresh
				 * request, once the registry is whole; Back / a stalled
				 * loader's Retry-or-Back live on that screen. With the
				 * opt-in off audio_registry_async_pending() is a constant
				 * false and this branch never runs. */
				if(audio_registry_async_pending()) {
					(void)loading_notice_gate_remote_connect();
				} else if(mp_swap_busy) {
					fprintf(stderr,
							"[mp] connect request dropped: provider busy "
							"(remote=%d world_owned=%d)\n",
							(int)mp_provider_remote,
							(int)server.player.has_pos);
					/* Finding 3: a silent drop left the connect screen on
					 * "Connecting..." forever (its generation baseline
					 * never moves). Tell it. */
					mp_session_note_request_dropped(
						mp_provider_remote
							? "Busy: previous session still closing"
							: "Busy: a world is still loaded");
				} else {
#ifdef PLATFORM_WII
					watchdog_context_observer_set(NULL);
#endif
					server_local_shutdown(&server);
					/* Reconnect root fix (2026-08-27 live "remote event
					 * queue overflow"): gstate.world_loaded LATCHES across a
					 * quit-to-title (see the busy-gate comment above), so a
					 * reconnect used to carry the ENTIRE prior session's
					 * client world into the new session's CRPC_WORLD_RESET
					 * -- a whole-world teardown on this thread, inside the
					 * login/install window, starving server_remote_pump()
					 * past push_event_locked's backpressure patience while
					 * the rejoin burst (all tracked entities + accumulated
					 * ground items) refilled the 64-slot ring. Pay that
					 * teardown HERE instead, while no socket exists and no
					 * event can queue. Same drain-then-reset sequence as the
					 * non-graceful teardown branch below; cheap no-op when
					 * no world is latched (first connect from boot). */
					clin_update_drain();
					clin_force_world_reset();
					clin_frame_teardown_extra(); /* B2 H14: no-op flag OFF */
					bool local_retired = world_finish_local_teardown(&gstate.world);
					if(!local_retired) {
						hw_telemetry_emit_dump("local_teardown_incomplete",
							"Local worker drain/storage retirement incomplete; retaining current provider", true);
						mp_session_note_request_dropped("Busy: local workers still retiring");
					}
					if(local_retired && mp_session_prepare_remote()
					   && server_remote_start(mp_session_remote(), mp_host,
										   mp_port, mp_user)) {
						/* W1: this connection's admission radius. AFTER start
						 * on purpose -- server_remote_start's per-connection
						 * reset defaults the field, so setting it earlier
						 * would be clobbered. */
						server_remote_set_admit_radius(mp_session_remote(),
													   mp_install_radius);
						mp_provider_remote = true;
						gstate.remote_session = true; // take-51 pre-world echo gate
						clin_remote_movement_reset();
						/* A fresh session shows the chat composer's first-use
						 * hint again (screen_chat.c retires it on open). */
						gstate.chat_hint_done = false;
						mp_swap_nonplay_since = time_get();
					} else {
						// Every start failure now records a reason + bumps
						// the status generation (redesign M6), so the
						// connecting screen reports it. Resume local
						// immediately so singleplayer is never left
						// unavailable.
						mp_session_release_remote();
						if(!server_local_start(&server))
							fprintf(stderr,
									"[mp] server_local_start FAILED after "
									"rejected remote start -- no server "
									"thread\n");
					}
				}
			}
			if(mp_provider_remote) {
				server_remote_pump(mp_session_remote());
				struct server_remote_status mp_status;
				server_remote_get_status(mp_session_remote(), &mp_status);
				/* REL-MP-FIX4b: GuiDownloadTerrain keep-alive (client_interface.c;
				 * client_interface.h was outside the task's file list). */
				extern bool clin_remote_hold_keep_alive_due(ptime_t now);
				if(mp_status.state == REMOTE_PLAY && !mp_status.dying
				   && clin_remote_hold_keep_alive_due(time_get()))
					(void)server_remote_send_keep_alive(mp_session_remote());
				client_session_achievements_remote(&gstate.players[0], mp_status.generation,
					mp_status.state == REMOTE_PLAY && !mp_status.dying);
				/* Surface terminal remote-state transitions on stderr: the
				 * connecting screen still lacks an in-GUI error display (the
				 * wiring slice's recorded residual), and this one line is
				 * what diagnosed both live-burst disconnect bugs on
				 * 2026-08-19. Cheap (fires only on state CHANGES), PC-only. */
				{
					static enum server_remote_state mp_last_state
						= REMOTE_DISCONNECTED;
					if(mp_status.state != mp_last_state) {
						fprintf(stderr,
								"[mp] state=%d reason=\"%s\" bp_waits=%u\n",
								(int)mp_status.state, mp_status.reason,
								mp_status.event_backpressure_waits);
						mp_last_state = mp_status.state;
					}
				}
				/* B5 (finding 5): while remote is active but NOT in play
				 * (connecting, handshake, login, closing), a deadline keeps
				 * singleplayer from being silently dead forever behind a
				 * wedged session. In REMOTE_PLAY the clock re-arms every
				 * frame (live sessions run for hours). 15s sits above the
				 * 10s bounded TCP connect (net_socket.c) plus handshake
				 * slack, so it only fires on genuinely stuck sessions. */
				bool mp_deadline_expired = false;
				/* Re-arm only while the session is HEALTHY in play --
				 * state == REMOTE_PLAY with stop_now set is a dying
				 * session whose deadline must keep running (re-review
				 * finding 1's belt; the root fix is kill_connection_locked
				 * delivering the error). time_reached is THE deadline
				 * primitive (time.h: unsigned Wii ptime_t wrecks the naive
				 * diff form -- re-review finding 8; S5 moves this block). */
				/* Fix C (2026-08-30, MP stuck-respawn symptom guard):
				 * screen_death has no way off itself except its own two
				 * buttons (Beta's GuiGameOver keyTyped swallows everything
				 * else) -- if the session dies while the player is stuck
				 * there, the *general* teardown-routing below (which lands
				 * on the connect screen's honest error state) still fires
				 * eventually, but only after riding out the FULL
				 * MP_SWAP_DEADLINE_MS (15s) belt below, sized for a
				 * still-negotiating CONNECTING/HANDSHAKE/LOGIN session, not
				 * a screen the player is actively mashing Respawn against.
				 * That is the "does not fire" the death screen appeared to
				 * exhibit: not a missing route, a route too slow to read as
				 * live from a screen with no other way out. Once we already
				 * know the session is dying (mp_status.dying -- true the
				 * instant stop_now/graceful_stop commits, well before both
				 * worker threads finish exiting to REMOTE_DISCONNECTED),
				 * skip the wait and let the SAME routing below run this
				 * frame instead of up to 15s from now. */
				bool mp_death_screen_wedged
					= gstate.players[0].current_screen == &screen_death && mp_status.dying;
				if(mp_status.state == REMOTE_PLAY && !mp_status.dying)
					mp_swap_nonplay_since = time_get();
				else if(mp_death_screen_wedged
						|| time_reached(time_get(),
									 time_add_ms(mp_swap_nonplay_since,
												 MP_SWAP_DEADLINE_MS))) {
					fprintf(stderr,
							"[mp] provider swap deadline (%ums) expired in "
							"state=%d -- forcing teardown, restoring local%s\n",
							(unsigned)MP_SWAP_DEADLINE_MS,
							(int)mp_status.state,
							mp_death_screen_wedged
								? " (death-screen fast path)" : "");
					mp_deadline_expired = true;
				}
				if(mp_status.state == REMOTE_DISCONNECTED
				   || mp_deadline_expired) {
					// Fully torn down (clean quit, cancel, or a kick/error)
					// -- or force-torn-down by the deadline above.
					// Release joins workers before freeing the remote allocation;
					// the UI receives its reason through the session notice.
					mp_session_release_remote();
					mp_provider_remote = false;
					gstate.remote_session = false;
					if(!server_local_start(&server))
						fprintf(stderr,
								"[mp] server_local_start FAILED after remote "
								"teardown -- no server thread\n");
					/* S4 disconnect UX: a NON-graceful end after login (kick,
					 * protocol error, transport death) must be SHOWN, not
					 * silently dropped -- Beta always lands the player on a
					 * reason screen [740c583
					 * NetClientHandler.handleKickDisconnect]. The connecting
					 * screen handles its own pre-play failures (it is the
					 * active screen then and polls the reason itself); this
					 * branch covers the mid-play window where no screen was
					 * watching. The dead session's world lives CLIENT-side
					 * only -- the freshly restored server_local never loaded
					 * it, so SRPC_UNLOAD_WORLD would trip the release impl's
					 * has_pos duplicate-unload assert (caught LIVE by the
					 * first kick capture). Instead: drain the dead
					 * generation's queued CRPCs, then run the client's own
					 * WORLD_RESET teardown synchronously (clin_process runs
					 * on this thread -- the same handler a dimension switch
					 * uses), then land on the reason screen. */
					if(!mp_status.graceful && mp_status.logged_in) {
						clin_update_drain();
						clin_force_world_reset();
						screen_multiplayer_connect_show_disconnected(
							mp_status.reason[0] ? mp_status.reason
												: "Connection lost");
					}
				}
			}
		}
#endif

		ptime_t profile_clin = hw_run.enabled ? time_get() : (ptime_t){0};
		/* An active drain uses the bounded consumer path. Defer ordinary
		 * frame authoring/timeouts until the FIFO can be serviced normally;
		 * partial drains must not time out a reply still waiting in the FIFO. */
		if(thread_g9_handoff_enabled()) clin_g9_handoff_poll(false);
		if(thread_g9_handoff_enabled() && thread_g9_lease_defer()) {
			/* The cursor owns the bounded FIFO quantum while waiting. */
		} else if(thread_g9_lease_defer())
			(void)clin_update_drain_budget((uint32_t)lease.service_us);
		else clin_update();
		clin_g9_handoff_poll(false); /* RPC returned every stripe/MQ lock. */
		achievements_tick();
		client_session_achievements_sync(&gstate.players[0], 0);
		client_session_achievements_sync(&gstate.players[1], 1);
  /* An init failure is not a failed save. Publish per-owner warnings only
   * when their HUD is free; server save errors retain priority. */
  static bool achievement_error_shown[2], achievement_departed_error_shown[2];
  for(unsigned p = 0; p < 2; p++) {
   bool error = !achievement_ready || achievements_slot_save_failed(p);
   struct client_session* owner = &gstate.players[p];
   if(error && !achievement_error_shown[p]
      && client_session_achievements_warning(owner, p, achievement_ready))
    achievement_error_shown[p] = true;
   if(!error) achievement_error_shown[p] = false;
   struct ach_status st;
   bool departed_failure = achievements_slot_save_failed(p)
    && !achievements_save_error_acknowledged(p)
    && achievements_debug_status(p, &st) && !st.bound;
   if(!departed_failure) achievement_departed_error_shown[p] = false;
   else if(!achievement_departed_error_shown[p]) {
    unsigned viewer = p && gstate.players[1].local_player ? 1u : 0u;
    struct client_session* host = &gstate.players[viewer];
    /* A departed owner's failure must not take input from live play.
     * Keep the warning on the surviving HUD until a visible menu boundary. */
    if(viewer != p && client_session_achievements_warning(host, p, achievement_ready)) {
     snprintf(host->hud_message, sizeof(host->hud_message), "P%u achievements save failed. Pause to retry.", p + 1);
    }
    bool boundary = gstate.paused
     || local_pause_active(&host->pause)
     || host->current_screen == &screen_achievements
     || host->current_screen == &screen_title;
    if(boundary && !screen_stats_save_active(viewer)) {
     screen_stats_save_error(viewer, 1u << p, retained_stats_answer, (void*)(uintptr_t)(1u << p));
     achievement_departed_error_shown[p] = true;
    }
   }
  }
		thread_main_pacing_checkpoint_at(THREAD_PACING_RPC);
		screen_death_host_respawn_poll();
		if(hw_run.enabled) frame_clin_ms = time_diff_s(profile_clin, time_get()) * 1000.0f;
		/* Live "Local split-screen" switch (Astra option A, S-A2). clin_update
		 * above is where CRPC_SPLIT_STATE is consumed, so this is where the
		 * Options row's pending toggle is retired -- once per frame, on this
		 * thread, at the seam every other CRPC effect lands on. The row owns
		 * the state machine, the readable failure text and the config commit;
		 * main.c owns only the arming, and it follows the ACKNOWLEDGED value
		 * rather than the value the row asked for (the ruling's "acknowledged
		 * state before input arming"). Cheap no-op with nothing pending.
		 *
		 * clin_split_last_ack TAKES the ack (R-A9 A9-H1/H2/M3), so the row
		 * sees each server statement exactly once and every frame without a
		 * new one passes NULL. While the slot stayed valid this seam re-fed
		 * one ack forever: the row's deferred config write was armed and then
		 * cancelled on the next frame, its 5 s no-answer timeout could never
		 * age, a settled caption was pinned over every later state, and a
		 * dotted-JSON config walk plus an OPTIONS_COUNT label reformat ran in
		 * every gameplay frame for the rest of the session. */
		struct clin_split_ack split_ack;
		const struct clin_split_ack* fresh_split_ack
			= clin_split_last_ack(&split_ack) ? &split_ack : NULL;
		split_experimental = screen_options_split_poll(
			fresh_split_ack, gstate.stats.dt);
		/* ...and the FILE gets its say once per world (R-A9 A9-M5). The boot
		 * seed above the loop was the only read of
		 * experimental.local_splitscreen, so a title-screen OFF was ignored by
		 * every world this process loaded afterwards -- the row snapped back to
		 * the boot value, this line stayed armed, and a second controller could
		 * still join. The seed runs AFTER the poll on purpose: a fresh ack for
		 * the new generation is adopted first, so the seed compares the file
		 * against what the server has actually stated for THIS world. It
		 * returns the same ACKNOWLEDGED value the poll does (arming never
		 * follows the file: it asks, and the ack settles it), and it is one
		 * epoch compare on every frame that is not a generation boundary.
		 * hw_run.split_guest is passed through because an armed split profile's
		 * forced ON must not be asked away by a config file that says 0. */
		split_experimental = screen_options_split_world_seed(
			hw_run.enabled && hw_run.split_guest);
		/* Seed the world before the leave-before-OFF coordinator can author
		 * a request. Both observers receive this frame's one consumed ACK;
		 * the coordinator must match its own sequence, not infer settlement
		 * from the row's reconciled ON/OFF presentation. */
		screen_options_split_off_tick(&gstate.players[1], clin_input_epoch(),
			fresh_split_ack);
		/* ...and the other end of the A8-M6 deferral (S-A5). The ack above moves
		 * the config key IN MEMORY and leaves the file write to the next menu
		 * commit, so a player who toggles in-world and then LEAVES without
		 * reopening Options would lose the acknowledged value. The world's
		 * teardown is the last moment it can still be written, and every exit
		 * route reaches it: the pause menu's Leave (screen_pause.c), the split
		 * pause menu's Leave (local_pause_frame above), the death screen, and a
		 * multiplayer disconnect all end in the SRPC_UNLOAD_WORLD /
		 * CRPC_WORLD_RESET that clears gstate.world_loaded. Edge-triggered on
		 * that flag rather than hooked into one route, so no route can be
		 * forgotten. A dimension switch and a stream restream clear the same
		 * flag and therefore also flush; that is
		 * a redundant write on a loading frame at worst, bounded at one per
		 * dirty toggle, and strictly safer than losing the value. Costs one bool
		 * compare per frame; screen_options_split_flush touches no file when
		 * nothing is owed.
		 *
		 * The screen test is the second half of the guarantee (R-A9 A9-L9).
		 * Almost every clear of world_loaded happens with the title or a load
		 * screen already installed, but ONE does not:
		 * clin_begin_live_stream_recovery's early-return branch clears the
		 * flag and returns WITHOUT screen_set, and it explicitly supports
		 * being called while screen_pause / screen_options-from-pause is up --
		 * so the falling edge can fire with screen_ingame still current, which
		 * is exactly the blocking libfat SD write inside a gameplay frame the
		 * deferral exists to remove. Skipping the flush there costs nothing:
		 * the key stays dirty and owed, so the next menu commit or the world's
		 * real teardown still writes it. */
		if(split_world_was_loaded && !gstate.world_loaded
		   && gstate.players[0].current_screen != &screen_ingame)
			(void)screen_options_split_flush();
		split_world_was_loaded = gstate.world_loaded;

		/* Existing-world Save and quit proof. This deliberately enters through
		 * the same admitted TOGGLE_PAUSE followed by screen_pause's production
		 * Save-and-quit transaction as a HOME press plus the named menu row. It
		 * runs after clin_update so an unload result received this frame is
		 * assessed before any new request. Merely queueing either RPC is never a
		 * success: only the pause screen's WORLD_RESET settlement is. */
		bool hw_save_quit_eligible = hw_save_quit_enabled
			&& !gstate.remote_session && !hw_run.split_guest
			&& gstate.world_loaded && !gstate.world_switch_pending
			&& gstate.players[0].current_screen == &screen_ingame
			&& !gstate.paused;
		if(hw_save_quit_state == HW_SAVE_QUIT_WAIT_WORLD) {
			if(hw_save_quit_eligible && !hw_save_quit_world_ready_seen) {
				hw_save_quit_world_ready_seen = true;
				hw_save_quit_world_ready_at = time_get();
				char detail[112];
				snprintf(detail, sizeof(detail), "world=%s delay_s=%d",
						 hw_run.autoload_name, hw_save_quit_after_seconds);
				hw_save_quit_trace("named_save_quit_armed", &hw_run, detail);
			}
			if(hw_save_quit_eligible && hw_save_quit_world_ready_seen
			   && time_diff_s(hw_save_quit_world_ready_at, time_get())
					  >= (float)hw_save_quit_after_seconds) {
				struct server_rpc toggle = {.type = SRPC_TOGGLE_PAUSE};
				if(!svin_rpc_send(&toggle)) {
					hw_save_quit_trace("named_save_quit_result", &hw_run,
									   "failed=pause_enqueue world_retryable=1");
					hw_save_quit_state = HW_SAVE_QUIT_FAILED;
				} else {
					gstate.paused = true;
					screen_set(&screen_pause);
					hw_save_quit_trace("named_save_quit_pause_request", &hw_run,
									   "queued=1");
					hw_save_quit_state = HW_SAVE_QUIT_PAUSE_QUEUED;
				}
			}
		} else if(hw_save_quit_state == HW_SAVE_QUIT_PAUSE_QUEUED) {
			/* A whole main-loop iteration separates pause admission from quit. */
			if(!gstate.world_loaded || gstate.world_switch_pending
			   || gstate.remote_session || !gstate.paused
			   || gstate.players[0].current_screen != &screen_pause) {
				hw_save_quit_trace("named_save_quit_result", &hw_run,
								   "failed=pause_state_changed world_retryable=1");
				hw_save_quit_state = HW_SAVE_QUIT_FAILED;
			} else {
				screen_pause_recovery_quit();
				if(screen_pause_exit_pending()) {
					hw_save_quit_trace("named_save_quit_unload_request", &hw_run,
									   "pending=1");
					hw_save_quit_state = HW_SAVE_QUIT_EXIT_PENDING;
				} else {
					hw_save_quit_trace("named_save_quit_result", &hw_run,
									   "failed=quit_enqueue world_retryable=1");
					hw_save_quit_state = HW_SAVE_QUIT_FAILED;
				}
			}
		} else if(hw_save_quit_state == HW_SAVE_QUIT_EXIT_PENDING
				  && !screen_pause_exit_pending()) {
			if(!gstate.world_loaded
			   && gstate.players[0].current_screen == &screen_title) {
				hw_save_quit_trace("named_save_quit_result", &hw_run,
								   "success=world_reset_title");
				hw_save_quit_state = HW_SAVE_QUIT_SUCCEEDED;
				if(hw_save_quit_capture) {
					screen_set(&screen_select_world);
					hw_save_quit_capture_at = time_get();
					hw_save_quit_state = HW_SAVE_QUIT_CAPTURE_WAIT;
				}
			} else {
				char detail[112];
				snprintf(detail, sizeof(detail),
						 "failed=unload_rejected world_loaded=%d paused=%d "
						 "world_retryable=1",
						 gstate.world_loaded ? 1 : 0, gstate.paused ? 1 : 0);
				hw_save_quit_trace("named_save_quit_result", &hw_run, detail);
				hw_save_quit_state = HW_SAVE_QUIT_FAILED;
			}
		} else if(hw_save_quit_state == HW_SAVE_QUIT_CAPTURE_WAIT
				  && time_diff_s(hw_save_quit_capture_at, time_get()) >= 1.0F) {
			gstate.auto_screenshot = true;
			hw_save_quit_trace("named_save_quit_capture_armed", &hw_run,
							   "screen=select_world delay_s=1");
			hw_save_quit_state = HW_SAVE_QUIT_CAPTURE_ARMED;
		}
		// mp_reconnect_soak (S5.3 hunt plumbing): advance the connect/dwell/
		// disconnect/reconnect cycle driver once per frame, AFTER clin_update()
		// so this frame's world_loaded/remote_session state is current. A
		// no-op unless mp_reconnect_soak_arm() was called (hw_run.
		// mp_reconnect_soak) -- mirrors menu_autopilot_frame_tick's
		// documented contract/placement precedent above.
		mp_reconnect_soak_frame_tick();
		/* clin_update may expose an epoch and rebase world_time_start using a newer
		 * time_get(). Wii ptime_t is unsigned, so interpolate against a timestamp
		 * sampled afterwards rather than underflowing start -> this_frame. */
		ptime_t world_frame = time_get();

		if(hw_run.enabled && hw_run.pin_cameras && gstate.world_loaded
		   && !gstate.world_switch_pending) {
			pin_capture_clock(world_frame);
			/* A frameless scripted guest (the split profiles' walk driver)
			 * picks up a "Controller disconnected" card while its viewport
			 * animates in (the recovery step sees no device) and nothing
			 * ever clears it; under the capture lever only, keep that card
			 * down so viewport 1 is photographed as admitted gameplay. */
			if(hw_run.split_guest && !input_player_frame(1))
				gstate.players[1].menu_host.recovery.phase
					= CONTROLLER_RECOVERY_NONE;
		}
		if(!gstate.paused) daytime
			= (float)((gstate.world_time
					 + time_diff_ms(gstate.world_time_start, world_frame)
						 / DAY_TICK_MS)
						% DAY_LENGTH_TICKS)
			/ (float)DAY_LENGTH_TICKS;

		tick_delta = time_diff_s(last_tick, time_get()) / 0.05F;

		// Separates the 20Hz client tick catch-up from clin_update above; without
		// it WDM_CLIN_UPDATE spanned both and a hang in entity/particle/piston
		// client tick was reported as an RPC-drain hang.
		watchdog_phase_main(WDM_CLIENT_TICK);
		/* D1 (ASTRA-DEFECTS-RULING 2-6): capture this frame's host
		 * ride-action edge (F3-1 JUMP dismount) ONCE, before the catch-up
		 * below, so the next host tick consumes it even when this frame runs
		 * no tick. Ineligible frames clear it. entity_local_player.h. */
		entity_local_player_ride_action_frame();
		/* Creative double-tap uses frame edges, independently for both
		 * owners, even when this frame executes no 20 Hz physics tick. */
		struct input_player_frame flight_host = {.connected = true};
		const struct input_player_frame* flight_owner = input_player_frame(0);
		flight_host.connected = gstate.autopilot.active
			|| !flight_owner || flight_owner->connected;
		flight_host.pressed[IB_JUMP] = input_pressed(IB_JUMP);
		ptime_t flight_now = time_get();
		entity_local_player_flight_input(0, &flight_host, flight_now);
		entity_local_player_flight_input(1, input_player_frame(1), flight_now);
		int catch_up = 0;
		ptime_t t_tick = time_get();
		while(tick_delta >= 1.0F && catch_up < 3) {
			last_tick = time_add_ms(last_tick, 50);
			tick_delta -= 1.0F;
			catch_up++;
			if(!gstate.paused) {
				mpm_client_tick++;
				/* Split-dims B2 H2/H3: every live payload's particles and
				 * display tick, player 0's first. Flag OFF: one payload (the
				 * gstate storage), nothing re-selected -- the pre-B2 calls. */
				unsigned tick_slots[2];
				unsigned tick_n = clin_world_live_slots(tick_slots);
				for(unsigned ti = 0; ti < tick_n; ti++) {
					struct clin_view_world ts;
					clin_world_slot_enter(tick_slots[ti], &ts);
					particle_update();
					client_display_tick();
					clin_world_view_leave(&ts);
				}
				// Beta-style client random display tick (torch flames etc.) --
				// cosmetic block effects, CLIENT thread (replaces the old
				// server-side torch onWorldTick layer violation). Runs in the
				// per-payload loop above, right after particle_update().
				// Beta TextureWaterFX: advance the animated water tile once per
				// 20Hz client tick (gated by !paused, like Beta's runTick FX update).
				// P5(c): under GFX_FANCY_LIQUIDS water renders from anim.png (pass 1
				// binds anim, world.c), so this terrain-atlas upload was NEVER SAMPLED --
				// a per-tick ~1KB sim + tex_gfx_subimage burning Broadway cycles for
				// invisible pixels (the "invisible 20Hz water-FX upload" from the
				// 2026-07-01 perf audit). Kept alive on the non-fancy path, where
				// TEXAT_WATER_STATIC -- now at Beta's real (13,12) -- is what water
				// actually samples.
#ifndef GFX_FANCY_LIQUIDS
				water_fx_anim_tick();
#endif
				// Beta TextureCompassFX/TextureWatchFX: default ON on PC and Wii.
				// Opt out with gfx.item_needle_anim=0; the call is then a no-op
				// (checked first thing inside).
				item_needle_fx_tick(gstate.config.item_needle_anim);
				// Chat lines age every client tick regardless of the active
				// screen or the entities gate [740c583 GuiIngame.updateTick].
				chat_ring_tick(&gstate.chat_ring);
#ifdef PLATFORM_PC
				// CAVEWII_AGENT bridge: emit one JSON state line per 20Hz tick to
				// stdout (no-op unless armed by the CAVEWII_AGENT=1 env). PC dev rig
				// only; a normal build/run is byte-identical.
				state_export_emit();
#endif
				bool host_entities_active=game_state_client_entities_active(&gstate);
				bool guest_only_active=!host_entities_active
					&&local_frame_loading_policy().guest_continues;
				int guest_tick_slot=guest_only_active?clin_world_view_slot(1):-1;
				if(host_entities_active||guest_only_active) {
					for(unsigned ti = 0; ti < tick_n; ti++) {
						if(guest_only_active&&tick_slots[ti]!=(unsigned)guest_tick_slot)continue;
						struct clin_view_world ts;
						clin_world_slot_enter(tick_slots[ti], &ts);
						client_piston_moving_tick();
						clin_world_view_leave(&ts);
					}
					/* The terrain route authors heading and buttons for each
					 * physics tick, including every tick in a catch-up frame. */
						if(host_entities_active)split_dims_host_return_pre_tick();
						if(host_entities_active&&gstate.autopilot.active && gstate.autopilot.terrain_safe)
						autopilot_tick();
					main_client_host_tick_dispatch(tick_slots, tick_n,
						host_entities_active, guest_only_active, guest_tick_slot);
					/* Experimental local split-screen: the guest body is
					 * deliberately NOT given entity_local_player's tick_client
					 * (that tick is primary-only -- it reads player zero's
					 * session, input and ride state), so the walk above
					 * skipped it. Drive it here from the guest's OWN owned
					 * input frame through the same physics the primary runs.
					 * Unreachable with the experimental switch off. */
					if(local_session_gameplay_active(&gstate.players[1], 1)) {
						struct input_player_frame guest_frame = {0};
						const struct input_player_frame* owned
							= input_player_frame(1);
						if(owned) guest_frame = *owned;
						/* host_vehicle_smoke_8m: the scripted P2 frame
						 * (positioning walk, neutral otherwise). */
						if(s_vehicle_act.guest_frame_valid)
							guest_frame = s_vehicle_act.guest_frame;
						/* Astra 2026-09-10 (D): the forced guest walk waits for
						 * the readiness latch like the primary's autopilot --
						 * with the split render reach every chunk entering the
						 * reach is a fresh build, so a walker moving from tick
						 * 0 never yields three clean readiness heartbeats
						 * (first ready at tick 627 on the band build). */
						/* split_recovery_6m borrows this same forced walk for
						 * ONE bounded P0 step: a guest admitted at the host
						 * position shares its column, and both bodies sit
						 * inside the +-1.0 pickup box of every item either
						 * drops, so they would race for each other blocks.
						 * The driver walks the guest clear, then stops --
						 * nothing walks once the baselines are taken. */
						if((hw_run.enabled && hw_run.split_guest_walk
						    && (!s_anchor.enabled || s_anchor.measuring))
						   || s_split_recovery_act.drive_guest_body) {
							/* Headless harness: a deterministic fixed-heading
							 * walk so BOTH residency anchors move. */
							guest_frame.connected = true;
							/* split_recovery_6m drives the BODY for all of P0
							 * (so it falls into its own hole and can collect
							 * the drop) but only WALKS during its separation
							 * step. Every other caller sets split_guest_walk,
							 * for which this is the unchanged expression. */
							split_dims_portal_step(time_get());
							if((hw_run.split_guest_walk && !split_dims_walk_released())
							   || s_split_recovery_act.drive_guest_walk) {
							guest_frame.down[IB_FORWARD] = true;
							/* GUEST_WALK_JUMP_PULSE: the primary autopilot's rule
							 * (autopilot.c IB_JUMP) -- hop 1-block rises every 12th
							 * tick after tick 20, auto-step does the rest. Without it
							 * the guest stood at the first rise (maintainer eyeball,
							 * stg30 split_play_8m, 2026-09-11). */
							static uint32_t s_guest_walk_tick;
							s_guest_walk_tick++;
							guest_frame.down[IB_JUMP]
								= s_guest_walk_tick > 20 && (s_guest_walk_tick % 12) == 0;
							/* GUEST_WALK_SWIM (guestdeath-0925): Beta swimming is jump
							 * HELD while in water (EntityLiving.java:629-631, +0.04F
							 * motionY per tick). The 1-in-12 hop pulse above cannot lift
							 * a body out of a lake, so on seed 12345 the yaw-180 guest
							 * walked the bed of the lake at z -146 (y 57.6, six blocks
							 * under) and drowned 58 heartbeats into every measured
							 * window (Wii 0917 tape, Wii 0925 G2, PC repro). Same feet
							 * probe as the primary autopilot (autopilot.c
							 * autopilot_probe_route: gstate.autopilot.water ->
							 * route_jump). Pure driver input; no simulation value
							 * changes. */
							if(gstate.players[1].local_player) {
								struct entity* gb = gstate.players[1].local_player;
								float gfeet = gb->pos[1] - 1.62F;
								struct block_data gfb = world_get_block(view_terrain(1),
									(w_coord_t)floorf(gb->pos[0]),
									(w_coord_t)floorf(gfeet + 0.5F),
									(w_coord_t)floorf(gb->pos[2]));
								if(gfb.type == BLOCK_WATER_FLOW
								   || gfb.type == BLOCK_WATER_STILL)
									guest_frame.down[IB_JUMP] = true;
							}
							/* GUEST_WALK_UNSTICK: no progress for 40 walk ticks (2 s)
							 * -> a quarter turn on the forced heading and jump held
							 * for 20 ticks; a wall becomes a wall-follow, a pit an
							 * exit. */
							{
								static float s_gw_x, s_gw_z;
								static uint32_t s_gw_still, s_gw_jump_hold;
								float gwx = gstate.players[1].camera.x;
								float gwz = gstate.players[1].camera.z;
								if(fabsf(gwx - s_gw_x) + fabsf(gwz - s_gw_z) < 0.05F) {
									if(++s_gw_still >= 40) {
										s_guest_heading_extra_deg += 90;
										s_gw_jump_hold = 20;
										s_gw_still = 0;
									}
								} else
									s_gw_still = 0;
								s_gw_x = gwx;
								s_gw_z = gwz;
								if(s_gw_jump_hold) {
									guest_frame.down[IB_JUMP] = true;
									s_gw_jump_hold--;
								}
							}
							} /* end of the WALK inputs */
						}
						/* SPLIT_ACTION_SCRIPT (S1-d): the scripted guest
						 * action sequence, armed only by split_actions_6m /
						 * --cw-split-actions and only after installed
						 * readiness latches -- the same gate the forced walk
						 * above uses, for the same reason (a body acting
						 * before the readiness streak would poison the
						 * measured window it is supposed to fill). It may
						 * clear this frame's walk inputs for its collect
						 * window, so it runs BEFORE the walk is applied. */
						if(hw_run.enabled && hw_run.split_guest_actions
						   && (!s_anchor.enabled || s_anchor.measuring))
							split_action_script_tick(&guest_frame);
						/* G9 observer: physical guest input only (no vehicle /
						 * script frame); sampled inside the dispatch, only when
						 * the owned walk actually consumes it. */
						s_guest_observe_input = owned
							&& !s_vehicle_act.guest_frame_valid
							&& !hw_run.split_guest_walk;
						main_client_guest_tick_dispatch(&guest_frame);
					}
					/* The P2 heartbeat fields (gy / head block / air) are
					 * sampled in emit_hw_heartbeat for every profile (C1 -> B1
					 * hand-off), after this walk refreshed client oxygen. */
					// View-bob per-tick state, fed by the freshly ticked
					// local player [740c583 EntityPlayer.onLivingUpdate].
					if(host_entities_active)
						camera_bob_client_tick(&gstate.players[0].camera,
							gstate.players[0].local_player);
					/* The other view's body uses this owner's walked distance
					 * too. Feed P2 after its movement, just as for P1. */
					if(gstate.players[1].local_player)
						camera_bob_client_tick(&gstate.players[1].camera,
							gstate.players[1].local_player);
					if(host_entities_active&&gstate.autopilot.active && !gstate.autopilot.terrain_safe)
						autopilot_tick();
					// render-slice (RENDER_DISPATCH.md): [740c583:
					// EntityRenderer.addRainParticles], called once per 20 Hz
					// client tick from Beta's own updateRenderer (see
					// render_weather.h) -- same gate as the other
					// local_player-dependent per-tick calls above.
					if(host_entities_active)client_add_rain_particles();
					/* Online Packet71 bolts: Minecraft.runTick's
					 * --field_27172_i then the weather-effect onUpdate
					 * (render_lightning.h). No-op with no bolt/flash. */
					lightning_client_tick(clin_remote_world_generation(),
						(int32_t)gstate.world.dimension);
				}
			}
		}
		if(tick_delta >= 1.0F) {
			// We fell behind; reset accumulator so we don't try to catch up forever.
			last_tick = time_get();
			tick_delta = 0.0F;
		}
		/* Freeze render interpolation while the client tick bodies are paused. */
		if(gstate.paused) tick_delta = paused_tick_delta;
		else paused_tick_delta = tick_delta;
		gstate.stats.ms_tick = time_diff_s(t_tick, time_get()) * 1000.0F;
		ptime_t profile_hb = hw_run.enabled ? time_get() : (ptime_t){0};
		if(hw_run.enabled) {
			ptime_t now = time_get();
			if(!hw_heartbeat_sent
			   || time_diff_s(hw_last_heartbeat, now) >= 1.0F) {
				// T-b: stamp a phase specific to heartbeat building/sending
				// right before the call, so a hang inside it (mallinfo, the
				// mem2/mesher stat reads, the JSON format, the non-blocking
				// net_send) is attributed here instead of inheriting
				// WDM_CLIENT_TICK's stale label from earlier this frame --
				// see WDM_HEARTBEAT's doc comment in watchdog.h.
				watchdog_phase_main(WDM_HEARTBEAT);
				emit_freeze_observer(&server.thread, hw_tick);
				emit_hw_heartbeat(hw_tick++, far_vd, server.far_view_distance, hw_frame_counter);
				/* Diagnostic only: a server stopped inside owned-RPC dispatch still
				 * leaves the main frame/heartbeat running for several seconds. */
				{
					uint32_t rpc = svin_active_owned_rpc_marker();
					size_t stack_left = thread_stack_lowwater_bytes(&server.thread);
					extern uint32_t g_srv_tclk_seq;
					char line[512];
					int line_len = snprintf(line, sizeof(line),
						"{\"event\":\"rpc_watch\",\"build_id\":\"%s\",\"active\":%u,\"type\":%u,\"slot\":%u,\"server_stage\":%u,\"stack_left\":%u,\"stack_guard\":%u,\"mem2_op\":%u,\"mem2_thread\":%u,\"mem2_bad\":%u,\"mem2_scan\":%u,\"mem2_scan_total_us\":%u,\"mem2_scan_max_us\":%u,\"wd_seq\":%u,\"srv_seq\":%u,\"regionio\":%u,\"main_census\":%u}",
						CW_BUILD_ID, rpc != 0, rpc ? (rpc & 0xffffu) - 1u : 0u,
						rpc ? (rpc >> 16) & 0xffu : 0u,
						server_local_diag_stage(), (unsigned)stack_left,
						(unsigned)(stack_left != 0 && stack_left < 8192u),
						(unsigned)mem2_diag_op(), (unsigned)mem2_diag_thread(),
						(unsigned)mem2_diag_bad(), (unsigned)mem2_diag_scan_count(),
						(unsigned)mem2_diag_scan_total_us(),
						(unsigned)mem2_diag_scan_max_us(),
						(unsigned)watchdog_loop_seq(),
						(unsigned)__atomic_load_n(&g_srv_tclk_seq, __ATOMIC_RELAXED),
						(unsigned)region_archive_io_busy(),
						(unsigned)watchdog_main_census_current());
					if(line_len > 0 && (size_t)line_len < sizeof(line))
						(void)hw_telemetry_emit_observer(line);
				}
				{ /* tel_drop attribution: names of refused observer events.
				   Last, so it never takes the slot rpc_watch needs (f_g9 build12:
				   emitted first, it starved rpc_watch every heartbeat, 46 drops). */
					static uint32_t drop_names_seen;
					char dl[400];
					uint32_t before = drop_names_seen;
					if(hw_telemetry_drop_names_line(dl, sizeof dl, &drop_names_seen)
					   && !hw_telemetry_emit_observer_retry(dl))
						drop_names_seen = before; /* re-offer next heartbeat */
				}
				hw_last_heartbeat = now;
				hw_heartbeat_sent = true;
			}
			/* The heartbeat can start the anchor clock. Sample afterwards:
			 * Wii's unsigned timebase wraps if now predates that new epoch. */
			now = time_get();
			float elapsed = time_diff_s(hw_started, now);
			/* The certifying split profiles run their window on the
			 * readiness epoch: loading never consumes measured time. The
			 * 2m smoke keeps the run clock (its readiness budget is part of
			 * the 120 s by design -- Astra ruling 2026-09-10). */
			bool anchor_clock
				= hw_run.profile == HW_RUN_PROFILE_SPLIT_ANCHOR_6M
				|| hw_run.profile == HW_RUN_PROFILE_SPLIT_REGROW_8M
				|| hw_run.profile == HW_RUN_PROFILE_SPLIT_PLAY_8M
				/* S1-d: same readiness-epoch clock as split_play_8m. */
				|| hw_run.profile == HW_RUN_PROFILE_SPLIT_ACTIONS_6M
				/* C1: six measured minutes on the readiness epoch
				 * (hw_run.c duration 360, collector split_dims_6m). */
				|| hw_run.profile == HW_RUN_PROFILE_SPLIT_DIMS_6M;
			if(anchor_clock)
				elapsed = s_anchor.measuring ?
					time_diff_s(s_anchor_started, now) :
					0;
			/* THE completion clock: the same INTEGER MILLISECOND the final
			 * heartbeat below serializes, not the float seconds above.
			 * s_anchor.elapsed_ms is literally the value that heartbeat
			 * reports as "anchor_ms" -- it was written this frame from
			 * `this_frame` (hw_anchor_evidence_note, above), and nothing
			 * between here and emit_hw_heartbeat rewrites it -- while
			 * `elapsed` is sampled from `now`, LATER in the same frame. Two
			 * timestamps, one contract: real-Wii run 0917_092103 crossed
			 * 360.000 float seconds while the heartbeat still said 359999
			 * ms, sent "profile_done ok", and the collector failed the run
			 * on its integer gate ("need six measured minutes after
			 * installed readiness", wii_telemetry_server.py). Deciding on
			 * the emitted integer makes "profile_done ok" imply the final
			 * heartbeat has reached duration*1000, by construction.
			 * The float above survives only for the watchdog-injection
			 * delay, whose profile is never an anchor-clock one. */
			uint32_t elapsed_ms = anchor_clock ?
				s_anchor.elapsed_ms :
				(uint32_t)time_diff_ms(hw_started, now);
			if(hw_run.watchdog_inject && !hw_watchdog_injected
			   && elapsed >= 10.0F) {
				hw_watchdog_injected = true;
				tps_observer_close(false);
				hw_telemetry_emit_profile_done("injecting_watchdog");
				// The volatile spin write IS the payload: an un-optimizable
				// hang so the hardware watchdog fires. The set-but-unused
				// warning is by design; do not "fix" this variable away.
				for(volatile uint32_t spin = 0;; spin++) {
				}
			}
			/* split_recovery_6m: the generic duration-based "ok" is DISABLED
			 * for this profile (Astra ruling 2026-09-17 item 7). Only the
			 * phase driver may complete it -- after EVERY phase assertion
			 * passed, or with the failing phase and its diagnostic -- and its
			 * own 355 s total cap fires inside the 360 s duration, so a stuck
			 * run still reports rather than running out the window. */
			if(hw_run.split_recovery && s_split_recovery_done_pending
			   && !hw_profile_done_sent) {
				hw_profile_done_sent = true;
				tps_observer_frame(false);
				hw_profile_done_at = now;
				emit_hw_heartbeat(hw_tick++, far_vd, server.far_view_distance,
								  hw_frame_counter);
				hw_telemetry_emit_profile_done(s_split_recovery_status);
				hw_telemetry_quiesce();
				region_archive_io_quiesce();
			}
			/* mp_chat_login_3m / mp_chat_chord_1m / mp_play_10m: the SAME
			 * exclusion, for the same reason -- only the phase driver may
			 * complete the run, and each driver's own total cap fires inside
			 * its profile duration (180 s in 180 s, 60 s in 60 s, and
			 * mp_play's 1 200 000 ms in 1260 s), so a stuck run always
			 * reports rather than running the window out with no verdict.
			 * Keyed on driver_owns_completion, not on a profile list, and the
			 * latch below has exactly one writer per run (see its
			 * declaration): the two drivers are mutually exclusive by
			 * profile, and inside an mp_play run the login machine is armed
			 * through chat_login_arm_play, which never reports. */
			/* These mutually exclusive functional drivers share completion
			 * hooks. Only their drained run_ok/fail completes the profile. */
			if(split_offer_piston_armed() && s_vehicle_done_pending)
				hw_telemetry_save_path_finish();
			bool save_path_drained = hw_telemetry_save_path_pump(
				(uint32_t)time_diff_ms(hw_started, time_get()));
			if((hw_run.vehicle_smoke || split_offer_piston_armed()) && s_vehicle_done_pending
			   && !hw_profile_done_sent && save_path_drained) {
				hw_profile_done_sent = true;
				tps_observer_frame(false);
				hw_profile_done_at = now;
				emit_hw_heartbeat(hw_tick++, far_vd, server.far_view_distance,
								  hw_frame_counter);
				hw_telemetry_emit_profile_done(s_vehicle_status);
				/* O20: also persist the verdict on the card (fetched by the
				 * next staging as previous_cw_boot_trace), so a stream that
				 * dies before it drains still names the reason. */
				hw_boot_trace_write(hw_run.vehicle_smoke ? "vehicle_done"
					: "split_offer_piston_done", &hw_run, s_vehicle_status,
									false);
				hw_telemetry_quiesce();
				region_archive_io_quiesce();
			}
			/* save_reload_3m (R12): only its phase driver completes the run
			 * (driver_owns_completion), after the verdict line was sent; its
			 * 170 s cap fires inside the 180 s window. */
			if(save_reload_armed() && save_reload_status()
			   && !hw_profile_done_sent) {
				hw_profile_done_sent = true;
				tps_observer_frame(false);
				hw_profile_done_at = now;
				emit_hw_heartbeat(hw_tick++, far_vd, server.far_view_distance,
								  hw_frame_counter);
				hw_telemetry_emit_profile_done(save_reload_status());
				hw_boot_trace_write("save_reload_done", &hw_run,
									save_reload_status(), false);
				hw_telemetry_quiesce();
				region_archive_io_quiesce();
			}
			/* pool_selftest_3m (R5 item 2): only the self-test completes
			 * the run -- after the scene hash and the ballast/guard check,
			 * or with its failure class; its 170 s cap fires inside the
			 * 180 s window. */
			if(hw_run.pool_selftest && pool_selftest_status()
			   && !hw_profile_done_sent) {
				hw_profile_done_sent = true;
				tps_observer_frame(false);
				hw_profile_done_at = now;
				emit_hw_heartbeat(hw_tick++, far_vd, server.far_view_distance,
								  hw_frame_counter);
				hw_telemetry_emit_profile_done(pool_selftest_status());
				hw_boot_trace_write("pool_selftest_done", &hw_run,
									pool_selftest_status(), false);
				hw_telemetry_quiesce();
				region_archive_io_quiesce();
			}
			if(hw_run.driver_owns_completion
			   && s_chat_login_done_pending && !hw_profile_done_sent) {
				hw_profile_done_sent = true;
				tps_observer_frame(false);
				hw_profile_done_at = now;
				emit_hw_heartbeat(hw_tick++, far_vd, server.far_view_distance,
								  hw_frame_counter);
				hw_telemetry_emit_profile_done(s_chat_login_status);
				hw_telemetry_quiesce();
				region_archive_io_quiesce();
			}
			if(!hw_run.watchdog_inject && !hw_run.split_recovery
			   && !hw_run.driver_owns_completion && !hw_run.pool_selftest
			   && !hw_profile_done_sent
			   && hw_run_duration_reached_ms(elapsed_ms,
											 hw_run.duration_seconds)) {
				bool checkpoint_ok = true;
				const bool drain_checkpoints = hw_run.profile == HW_RUN_PROFILE_PERF_STRESS_5M
					&& region_archive_deferred_io_enabled();
				if(drain_checkpoints) {
					SERVICE_OBSERVER(service_observer_state(SO_SHUTDOWN));
					/* R24: end admission, join, then cover/reconcile tokens
					 * BEFORE profile_done or IO/telemetry quiescence. This
					 * lifecycle wait is outside the earlier gameplay ticks. */
					ptime_t drain_start = time_get();
					watchdog_phase_main(WDM_SHUTDOWN_SERVER);
					watchdog_heartbeat();
					server_local_shutdown(&server);
					/* No damage can follow the join. The live session is retired;
					 * this one heartbeat uses its exact terminal interval record. */
					s_completion_heartbeat_server = &server;
					for(unsigned i = 0; i < SERVER_LOCAL_WORLD_SLOTS; i++)
						checkpoint_ok &= !server.worlds[i].checkpoint_failed;
					uint32_t final_ckio[SERVER_CHECKPOINT_STATS];
					server_world_checkpoint_stats(final_ckio);
					checkpoint_ok &= final_ckio[0] == final_ckio[1]
						&& final_ckio[2] == 0 && final_ckio[9] == 0;
					char line[160];
					snprintf(line, sizeof line,
						"{\"event\":\"region_journal\",\"checkpoint_shutdown_us\":%llu,\"ok\":%u}",
						(unsigned long long)time_diff_us(drain_start, time_get()),
						(unsigned)checkpoint_ok);
					/* The collector seals the run on this line; a full observer
					 * ring at shutdown dropped it (f_g9 build12). The server is
					 * joined, so wait up to 2 s for the sender to free a slot. */
					ptime_t emit_start = time_get();
					while(!hw_telemetry_emit_observer_retry(line)) {
						if(time_diff_us(emit_start, time_get()) >= 2000000) {
							(void)hw_telemetry_emit_observer(line); /* counted drop */
							break;
						}
						thread_msleep(1);
					}
				}
				hw_profile_done_sent = true;
				tps_observer_frame(false);
				hw_profile_done_at = drain_checkpoints ? time_get() : now;
				/* Flush the cumulative tail before completion: a last-second
				 * hitch must not hide behind the previous 1 Hz heartbeat. */
				emit_hw_heartbeat(hw_tick++, far_vd, server.far_view_distance, hw_frame_counter);
				hw_telemetry_emit_profile_done(checkpoint_ok ? "ok" : "checkpoint_drain_failed");
				// Close admission; the sender drains this final heartbeat and
				// profile_done before transport quiescence. Main never waits
				// for IOS during the auto-exit grace window.
				hw_telemetry_quiesce();
				/* M2. Same seam, same reason: after profile_done the run is
				 * over and an auto-exit follows, so no publication may still
				 * be in flight when the teardown starts. Bounded to 250 ms,
				 * then abandoned and counted (journal slot 23). */
				region_archive_io_quiesce();
			}
			// Hands-free hardware batch (2026-08-18): the console used to keep
			// ticking forever after profile_done, forcing a human to
			// hard-reboot -> HBC -> relaunch for every profile switch. Once the
			// grace window has elapsed, exit through the SAME clean path the
			// title screen's "Quit Game" button and the HOME-button shortcut
			// already use (gstate.quit = true -> server_local_shutdown ->
			// audio_shutdown -> hw_telemetry_shutdown -> return 0 -> the
			// standard devkitPPC return-to-loader) -- no new exit mechanism,
			// just reusing the one this codebase already exercises on every
			// hardware run. hw_run_auto_exit_ready hard-gates this to ARMED
			// runs only (cfg->enabled), so normal/manual play -- where
			// hw_run.enabled is false and this whole "if(hw_run.enabled)"
			// block never executes -- can never auto-exit.
			bool hw_save_quit_blocks_exit = hw_save_quit_enabled
				&& hw_save_quit_state != HW_SAVE_QUIT_SUCCEEDED
				&& hw_save_quit_state != HW_SAVE_QUIT_CAPTURE_ARMED;
			if(!hw_save_quit_blocks_exit
			   && (!g9_capture_pending() || hw_telemetry_quiesced())
			   && hw_run_auto_exit_ready(&hw_run, hw_profile_done_sent,
									 time_diff_s(hw_profile_done_at, now))) {
				hw_boot_trace_write("auto_exit", &hw_run,
									"profile_done grace elapsed", false);
				gstate.quit = true;
			}
		}

		if(hw_run.enabled) frame_heartbeat_ms = time_diff_s(profile_hb, time_get()) * 1000.0f;
		/* residency-1005/4 item 12 (route E): the default-off runtime
		 * residency fault, requested once when the run reaches its configured
		 * time (wall clock since the run started); the server thread arms it
		 * (server_local_rz_fault_request). Deliberately OUTSIDE the completion
		 * block above, which test_anchor_measurement_clock slices verbatim. */
		if(hw_run.enabled && hw_run.rz_fault_set && gstate.world_loaded) {
			static bool s_rz_fault_requested;
			if(!s_rz_fault_requested
			   && time_diff_ms(hw_started, time_get())
				   >= hw_run.rz_fault_delay_s * 1000) {
				s_rz_fault_requested = true;
				server_local_rz_fault_request(hw_run.rz_fault_actor);
			}
		}
		bool container_converted=false;
		local_transfer_panes_update();
		unsigned view_count=local_container_views(&container_converted, split_layout.boundary != 1);
		/* split_recovery_6m: one call per frame, BEFORE the guest-maintenance
		 * block below, so this frame auto-join gate, suspension gate and owned
		 * frames all come from the same driver step. The layout it reads is the
		 * one last frame split_layout_step produced -- a one-frame lag that is
		 * inside the driver own 10-frame timing tolerance. Zeroed and inert for
		 * every other profile and for normal play. */
		if(split_portal_quit_armed()
		   && split_recovery_phase_now() == SPLIT_RECOVERY_P3) {
			/* Observe only a CRPC_BEGIN_WORLD_SWITCH delivered in P3. */
			split_portal_quit_observe_switch_begin(&s_spq_last_switch_begin_seq,
				gstate.world_switch_begin_seq, split_recovery_sim_ticks(),
				hw_telemetry_portal_quit_up_ms());
			float portal_progress = 0.0f;
			/* One aligned 32-bit observation of the server's production dwell
			 * field. The decision machine never modifies the dwell or switch. */
			__atomic_load(&server.player.time_in_portal, &portal_progress,
				__ATOMIC_RELAXED);
			int switch_phase = __atomic_load_n(&server.world_switch_phase,
				__ATOMIC_RELAXED);
			struct split_portal_quit_actions act;
			split_portal_quit_frame(&(struct split_portal_quit_ctx) {
				.host = &gstate.players[0], .guest = &gstate.players[1],
				.world_loaded = gstate.world_loaded,
				.world_switch_pending = gstate.world_switch_pending,
				.at_title = gstate.players[0].current_screen == &screen_title,
				.switch_started = switch_phase != SERVER_WORLD_SWITCH_IDLE,
				.request_after_switch = s_spq_request_after_switch,
				.portal_progress = portal_progress,
				.dimension = gstate.world.dimension,
				.tick = split_recovery_sim_ticks(),
				.up_ms = hw_telemetry_portal_quit_up_ms(),
				.unload_requests = s_spq_unload_requests}, &act);
			memset(&s_split_recovery_act, 0, sizeof s_split_recovery_act);
			s_split_recovery_act.suppress_autojoin = act.suppress_autojoin;
			s_split_recovery_act.guest_frame_valid = act.guest_frame_valid;
			s_split_recovery_act.host_frame_valid = act.host_frame_valid;
			s_split_recovery_act.guest_frame = act.guest_frame;
			s_split_recovery_act.host_frame = act.host_frame;
		} else {
			enum split_recovery_phase before = split_recovery_phase_now();
			split_recovery_frame(&(struct split_recovery_ctx) {
			.host = &gstate.players[0], .guest = &gstate.players[1],
			.layout = &split_layout,
			.view_count = view_count, .world_loaded = gstate.world_loaded,
			.world_switch_pending = gstate.world_switch_pending,
			.host_body = gstate.players[0].local_player != NULL,
			.at_title = gstate.players[0].current_screen == &screen_title,
			/* The SERVER's simulation tick counter, atomically -- the same
			 * snapshot the heartbeat publishes as server_time. gstate.world_time
			 * is a base stamp the server refreshes only on a CRPC_TIME_SET and
			 * cannot witness a running world. */
			.sim_ticks = split_recovery_sim_ticks(),
			.epoch = clin_input_epoch(),
			.now_ms = (uint32_t)time_diff_ms(hw_started, time_get())},
			&s_split_recovery_act);
			if(s_split_recovery_act.host_walk_valid)
				autopilot_script_walk(s_split_recovery_act.drive_host_walk, 180);
			/* P2 enters P3 inside split_recovery_frame. Drop any BEGIN
			 * observed before this frame's P3 entry. */
			if(split_portal_quit_armed() && before != SPLIT_RECOVERY_P3
			   && split_recovery_phase_now() == SPLIT_RECOVERY_P3)
				split_portal_quit_p3_enter(&s_spq_last_switch_begin_seq,
					gstate.world_switch_begin_seq);
		}

		/* host_vehicle_smoke_8m: one driver step per frame, after the
		 * previous input_poll latch and before this frame's screen/gameplay
		 * consumers below. Zeroed and inert for every other profile. */
		if(vehicle_smoke_armed()) {
			if(s_vehicle_prev_loaded && !gstate.world_loaded)
				s_vehicle_reset_count++;
			s_vehicle_prev_loaded = gstate.world_loaded;
			vehicle_smoke_frame(&(struct vehicle_smoke_ctx) {
				.host = &gstate.players[0], .guest = &gstate.players[1],
				.world_loaded = gstate.world_loaded,
				.world_switch_pending = gstate.world_switch_pending,
				.at_title = gstate.players[0].current_screen == &screen_title,
				/* Split shows the host's death UI in its viewport (owner
				 * death.active, local_death_arbitrate) without switching
				 * current_screen; single view switches to screen_death. */
				.host_death_screen
					= gstate.players[0].current_screen == &screen_death
					|| gstate.players[0].death.active,
				.host_loading_screen
					= gstate.players[0].current_screen == &screen_load_world,
				.view_count = view_count,
				.epoch = clin_input_epoch(),
				.now_ms = (uint32_t)time_diff_ms(hw_started, time_get()),
				.world_reset_count = s_vehicle_reset_count,
				.block_at = vs_hook_view_block}, &s_vehicle_act);
			vs_apply_host(&s_vehicle_act);
		}

		if(split_offer_piston_armed()) {
			split_offer_piston_frame(&(struct vehicle_smoke_ctx) {
				.host = &gstate.players[0], .guest = &gstate.players[1],
				.world_loaded = gstate.world_loaded,
				.world_switch_pending = gstate.world_switch_pending,
				.view_count = view_count,
				.now_ms = (uint32_t)time_diff_ms(hw_started, time_get())},
				&s_vehicle_act);
			vs_apply_host(&s_vehicle_act);
		}

		/* ---- Experimental local split-screen: the Join handshake ----------
		 * Entirely inside this switch. With it off, not one line below runs
		 * and view_count can never leave 1 (local_gameplay_ready has no other
		 * production setter). On hardware the ONLY starter is a second
		 * controller's intentional claim; an armed harness run auto-joins so
		 * a headless two-anchor measurement needs no human. */
		/* The block runs while the switch is on OR while a guest session or
		 * body still exists (R-A8 A8-H3). Gating it on the switch alone meant a
		 * live OFF stopped maintaining a guest that was already mid-join: its
		 * card was never retired, its controller stayed claimed, and there was
		 * no failure path out -- "blocking a new join" turned into wedging one.
		 * With the switch off and no guest at all the predicate is false, so
		 * the single-player frame is untouched. */
		struct client_session* guest = &gstate.players[1];
		if(clin_world_payloads_enabled()) {
			/* A dead guest can still view the Nether while the authoritative
			 * respawn target is the Overworld. Check the exact LOADING handle,
			 * then move the inert body/view before allowing its READY ACK. */
			unsigned target=guest->local_respawn_world_slot;
			const struct client_world_context* target_ctx
				= guest->local_respawn_loading && target<CLIENT_CTX_SLOT_COUNT
				? client_ctx_registry_current(clin_world_contexts(),target):NULL;
			if(target_ctx
			   && target_ctx->server_generation
				==guest->local_respawn_world_generation
			   && target_ctx->dimension==guest->local_respawn_dimension
			   && clin_world_payload_ready(target)) {
				struct clin_view_world guest_world;
				if(clin_world_slot_enter(target,&guest_world)) {
					if(client_session_local_respawn_readiness(guest,
						guest_world.terrain,true,gstate.world_load_failed,
						gstate.remote_terrain_hold)
					   && !clin_guest_respawn_handoff(target,
						guest->local_respawn_world_generation,
						guest->local_respawn_dimension))
						guest->local_respawn_ack_send_pending=false;
					clin_world_view_leave(&guest_world);
				}
			}
		} else {
			(void)client_session_local_respawn_readiness(guest, &gstate.world,
				gstate.world_loaded && !gstate.world_switch_pending,
				gstate.world_load_failed, gstate.remote_terrain_hold);
		}
		(void)client_session_host_respawn_readiness(&gstate.players[0],
			&gstate.world,gstate.world_loaded&&!gstate.world_switch_pending,
			gstate.world_load_failed,gstate.remote_terrain_hold);
		client_session_flush_host_respawn(&gstate.players[0]);
		bool guest_session = guest->local_phase != CLIENT_GUEST_IDLE;
		/* A join that is still TRYING: it owns (or is acquiring) the window but
		 * has no admitted body yet. LEAVING is excluded -- it is already on its
		 * way out and must not be re-refused every frame. */
		bool guest_joining = guest->local_phase == CLIENT_GUEST_REQUESTED
			|| guest->local_phase == CLIENT_GUEST_INSTALLING
			|| guest->local_phase == CLIENT_GUEST_CONFIRMING
			|| guest->local_phase == CLIENT_GUEST_ATTACHED;
		/* Release the one-settle latch when the guest is fully back to IDLE.
		 * This is pure latch CLEANUP, not guest maintenance, so it runs
		 * OUTSIDE the maintain block and BEFORE a new join can be requested
		 * (R-A9 A9-M4). Inside the block it was unreachable on exactly the
		 * path that needs it: a refuse-and-release with the switch OFF ends in
		 * LOCAL_GUEST_LEFT -> CLIENT_GUEST_IDLE inside clin_update, i.e.
		 * earlier in the same frame, and on that frame there is no session, no
		 * body and no switch -- maintain(false,false) is false, so the whole
		 * block including the reset was skipped and the latch stayed set for
		 * the rest of the session. The next join then never called
		 * input_split_join_settle(true) (it is gated on !split_join_settled)
		 * and its approved join card was never retired. Ordering matters as
		 * much as placement: the old site sat AFTER the join request, which
		 * had already moved the phase off IDLE, so even a reachable frame
		 * could not clear it. IDLE means nothing is claimed or in flight, so
		 * clearing here can never retire a card early. */
		if(guest->local_phase == CLIENT_GUEST_IDLE)
			split_join_settled = false;
		/* admit-retry-0925 (Astra review A, item 2): a first-body admission
		 * retirement goes LEAVING -> IDLE, never through FAILED, so settle the
		 * join card as refused here ("Could not join", Retry/Back) and show
		 * the reason on the host HUD -- the guest has no viewport without a
		 * body. Once per retirement. */
		if(client_session_local_admit_failure_present(guest, &gstate.players[0])
		   && !split_join_settled) {
			split_join_settled = true;
			input_split_join_settle(false);
		}
		if(screen_options_split_guest_maintain(guest_session,
				guest->local_gameplay_ready)) {
			bool claimed = input_split_join_claimed();
			bool world_ready = gstate.world_loaded && !gstate.world_switch_pending
				&& gstate.players[0].local_player;
			/* split_recovery_6m LEAVE phase owns when the guest may come
			 * back: the synthetic rejoin stays shut until the leave ack, the
			 * retirement and the host-only layout have all been OBSERVED, and
			 * again across the reload until the loaded state is verified. */
			if(hw_run.enabled && hw_run.split_guest
			   && !s_split_recovery_act.suppress_autojoin
			   && !s_vehicle_act.suppress_autojoin)
				claimed |= world_ready;
			/* A NEW join still needs the switch ON: OFF blocks new joins. */
			if(split_experimental && claimed && world_ready
			   && !screen_options_split_off_blocks_join()
			   && (guest->local_phase == CLIENT_GUEST_IDLE
				   || guest->local_phase == CLIENT_GUEST_FAILED)
			   && client_session_request_local_join(guest))
				/* admit-retry-0925: a claim is a NEW card ("Joining Player
				 * 2..."), including the Retry that re-sends a refused
				 * admission Leave; it owes its own settle. */
				split_join_settled = false;
			if(screen_options_split_guest_refuse(guest_joining,
					guest->local_gameplay_ready)) {
				/* The switch went off under a join that never got a body. The
				 * server's admission gate refuses every NEW body while OFF, and
				 * an admission it had already granted would have reached this
				 * client ahead of the OFF ack (one FIFO each way, drained whole
				 * by clin_update), so local_gameplay_ready being false here is
				 * final. Retire the card as refused, hand the controller back,
				 * and release the session instead of abandoning it. */
				if(!split_join_settled) {
					split_join_settled = true;
					input_split_join_settle(false);
				}
				(void)client_session_request_local_leave(guest);
			} else if(guest->local_phase == CLIENT_GUEST_ATTACHED
					  && guest->local_join_failed)
				/* residency-1005/6 R2: the bounded Join terrain wait failed
				 * (card settled as refused, reason on the host HUD). Release
				 * the session through the authoritative Leave; the card's
				 * Retry is then an ordinary fresh Join. */
				(void)client_session_request_local_leave(guest);
			else if(guest->local_phase == CLIENT_GUEST_ATTACHED
					  && !guest->local_gameplay_ready)
				/* ATTACHED certifies WINDOW ownership only. Keep asking for the
				 * BODY: the server denies while the guest's terrain is still
				 * streaming, and grants once it is resident. */
				(void)client_session_request_local_gameplay(guest);
			if(guest->local_gameplay_ready && !split_join_settled) {
				split_join_settled = true;
				input_split_join_settle(true);
			} else if(guest->local_phase == CLIENT_GUEST_FAILED
					  && !split_join_settled) {
				split_join_settled = true;
				input_split_join_settle(false);
			}
			/* (the IDLE arm of this chain moved above the block -- A9-M4) */
			/* The two-view draw loop dereferences EVERY owner's screen
			 * (render3D/render_world). Player 2 has no screen stack of its
			 * own -- screen_set() is the primary's -- so its viewport is
			 * pinned to the in-game screen for exactly as long as it holds a
			 * body, and released with it. Without this the very first split
			 * frame dereferenced a NULL current_screen (main.c's per-viewport
			 * render3D test) and crashed. */
			guest->current_screen
				= (guest->local_gameplay_ready
				   || local_respawn_presentation(guest)) ? &screen_ingame : NULL;
			/* screen_ingame's reset() is what arms capture_input for the
			 * primary body; the guest has no screen stack of its own to run
			 * that through, so its viewport's pin above carries the same
			 * meaning here. Without it the body ignores every button:
			 * entity_local_player_walk_owned gates ALL movement on this flag,
			 * so the guest stood still while its camera tracked it perfectly
			 * (measured: PC harness FINAL position identical to ADMITTED). */
			if(guest->local_player)
				guest->local_player->data.local_player.capture_input
					= guest->local_gameplay_ready;
			/* One line per admission edge. This is the receipt a harness run
			 * is checked against: without it a "split-screen" log is just a
			 * single-player log that happened to have the switch on. */
			if(guest->local_gameplay_ready != split_guest_logged) {
				split_guest_logged = guest->local_gameplay_ready;
				if(split_guest_logged)
					fprintf(stderr,
							"[splitscreen] guest body ADMITTED: entity %u at "
							"%.2f %.2f %.2f (two viewports, two residency anchors)\n",
							gstate.players[1].local_player
								? gstate.players[1].local_player->id : 0u,
							gstate.players[1].camera.x, gstate.players[1].camera.y,
							gstate.players[1].camera.z);
				else
					fprintf(stderr, "[splitscreen] guest body RETIRED\n");
			}
			/* Where the guest BODY ended up, once per armed run. Paired
			 * with the ADMITTED line this is the receipt that a second
			 * player actually MOVED for the measured window, rather than a
			 * joined body standing still while player one walked. Emitted
			 * here, not in the profile-completion block: that block is
			 * extracted verbatim by test_anchor_measurement_clock and
			 * compiled against a stub gstate with no players[]. */
			if(hw_profile_done_sent && !split_final_logged
			   && guest->local_gameplay_ready && guest->local_player) {
				split_final_logged = true;
				fprintf(stderr,
						"[splitscreen] guest body FINAL: entity %u at "
						"%.2f %.2f %.2f\n",
						guest->local_player->id, guest->camera.x,
						guest->camera.y, guest->camera.z);
			}
			client_session_flush_local(guest);
		}

		/* Diagnostics follow the real body (or nothing when there is none).
		 * Gated on the same "switch on OR a guest exists" predicate as the
		 * block above (R-A8 A8-H3): keying it on the switch alone froze
		 * s_guest_anchor_x/z at the position the body held when a live OFF
		 * landed, so world_anchor_status went on reporting anchor-1
		 * missing/pending/far around a stale point and every later anchor-1
		 * measurement was silently wrong. No guest and the switch off: never
		 * touched, so the single-player frame stays byte-identical
		 * (s_guest_anchor keeps its zeroed default), and the last call a
		 * retiring guest makes is the releasing one. */
		if(screen_options_split_guest_maintain(guest_session,
				gstate.players[1].local_gameplay_ready))
			world_set_guest_anchor(gstate.players[1].local_gameplay_ready
					&& gstate.players[1].local_player,
				gstate.players[1].camera.x, gstate.players[1].camera.z);
		/* view_count comes from local_container_views above (main's approved
		 * UI admission, a superset of the old local_bound/ready/player test).
		 * Two cameras = a real second viewport OR the synthetic second
		 * anchor of the split hardware profiles (world_update_lod uses both). */
		world_set_server_view_radius(server_local_view_radius(&server));
		split_render_reach_apply(view_count == 2 || s_anchor.separation > 0
								 || s_anchor.guest);
		/* Session count still governs residency and owner updates. Controller
		 * loss changes presentation only; it never retires a player. */
		ptime_t layout_now = time_get();
		float layout_dt = time_diff_s(split_layout_clock, layout_now);
		split_layout_clock = layout_now;
		bool suspended[2] = {false, false};
		for(unsigned p = 0; p < 2; p++) {
			const struct input_player_frame* frame = input_player_frame(p);
			suspended[p] = !frame || !frame->connected
#ifdef PLATFORM_WII
				|| (p == 1 && frame->category != INPUT_CAT_NUNCHUK
					&& frame->category != INPUT_CAT_CLASSIC_CONTROLLER
					&& frame->category != INPUT_CAT_GAMECUBE)
#endif
				|| gstate.players[p].menu_host.recovery.phase != CONTROLLER_RECOVERY_NONE;
			/* A frameless scripted guest is normally forced un-suspended (it
			 * has no device and never loses one). split_recovery_6m is the
			 * one profile that DOES lose one on purpose, so there the real
			 * recovery machine decides. Disarmed, this is the same false it
			 * has always been. */
			if(hw_run.enabled && hw_run.split_guest && !frame)
				suspended[p] = split_recovery_armed()
					&& gstate.players[p].menu_host.recovery.phase
						!= CONTROLLER_RECOVERY_NONE;
		}
		struct clin_frame_loading_policy loading=local_frame_loading_policy();
		bool host_loading_pane=view_count==2&&!loading.draw_view[0];
		bool guest_loading_pane=view_count==2&&loading.guest_transfer_loading;
		static float guest_loading_elapsed=0.0F;
		guest_loading_elapsed=guest_loading_pane
			?guest_loading_elapsed+gstate.stats.dt:0.0F;
		bool host_loading_guest_active=view_count==2&&loading.guest_continues;
		bool global_layout_hold=!gstate.world_loaded||gstate.world_switch_pending
			||gstate.paused;
		if((global_layout_hold&&!host_loading_pane)
		   || (gstate.players[0].current_screen != &screen_ingame
		       && gstate.players[0].current_screen != &screen_death
		       && !host_loading_pane))
			split_layout_reset(&split_layout);
		else split_layout_step(&split_layout, view_count == 2, suspended, layout_dt);
		struct split_layout_rect view_rect[2] = {
			split_layout_rect(&split_layout, 0, gfx_height()),
			split_layout_rect(&split_layout, 1, gfx_height())};
		struct camera* render_cameras[2] = {
			client_session_local_render_camera(&gstate.players[0]),
			client_session_local_render_camera(&gstate.players[1])};
		struct camera hidden_respawn_camera[2];
		struct camera* lod_cameras[2] = {render_cameras[0], render_cameras[1]};
		unsigned lod_view_count = view_count;
		if(client_session_host_respawn_hidden_camera(
			&gstate.players[0],&hidden_respawn_camera[0])) {
			lod_cameras[0]=&hidden_respawn_camera[0];
			if(lod_view_count<1)lod_view_count=1;
		}
		if(client_session_local_respawn_hidden_camera(
			&gstate.players[1], &hidden_respawn_camera[1])) {
			lod_cameras[1] = &hidden_respawn_camera[1];
			lod_view_count = 2;
		}
		if(client_session_host_gameplay_active(&gstate.players[0]))
			camera_attach(&gstate.players[0].camera, gstate.players[0].local_player, tick_delta,
							gstate.stats.dt);
		// update particle‐system with current camera pos for spawn‐culling
		particle_set_camera((vec3){
		    gstate.players[0].camera.x,
		    gstate.players[0].camera.y,
		    gstate.players[0].camera.z
		});
		clin_world_payload_sync_particle_cameras(); /* B2 H4: no-op flag OFF */
		/* Split-dims B2: this frame's worlds -- which terrain each view draws
		 * and which payloads are live. Flag OFF: gstate.world everywhere. */
		struct clin_frame_worlds fw;
		clin_frame_worlds_begin(&fw, view_count >= 2 ? 2u : 1u);
		(void)clin_frame_prepare_respawn(&fw, &gstate.players[1],
			&hidden_respawn_camera[1]);

		fw.draw_view[0]=loading.draw_view[0];
		fw.draw_view[1]=loading.draw_view[1];
		render_world
			= (gstate.players[0].current_screen->render_world && gstate.world_loaded)
			  ||host_loading_guest_active;
		bool in_water_new = false;

		if(render_world&&fw.draw_view[0]) {
			struct block_data blk = world_get_block(
				fw.view_terrain[0], floorf(gstate.players[0].camera.x),
				floorf(gstate.players[0].camera.y + 0.1F), floorf(gstate.players[0].camera.z));
			in_water_new		
				= blk.type == BLOCK_WATER_FLOW || blk.type == BLOCK_WATER_STILL;
			#ifndef GFX_FANCY_LIQUIDS
			if(gstate.players[0].in_water != in_water_new) {
			#endif
				gstate.players[0].in_water = in_water_new;	
			#ifndef GFX_FANCY_LIQUIDS
				world_redraw_chunks(fw.view_terrain[0]);
			}
			#endif
		}

		if(view_count == 1) {
			if(hw_run.enabled && hw_run.pin_cameras && gstate.world_loaded
			   && !gstate.world_switch_pending)
				render_cameras[0] = pin_capture_camera(render_cameras[0], 0);
			if(view_rect[0].height == gfx_height())
				camera_update(render_cameras[0], gstate.players[0].in_water);
			else camera_update_viewport(render_cameras[0], gstate.players[0].in_water,
				gfx_width(), view_rect[0].height > 0 ? view_rect[0].height : 1);
		}
		else {
			struct client_session* guest = &gstate.players[1];
			/* The guest's LOOK, from its OWN device only (input_player_look
			 * never reads another player's controller). Same rate integrator
			 * and the same *2.0F the primary's camera_attach applies, so both
			 * players turn at an identical speed. */
			struct camera* guest_render=render_cameras[1];
			float guest_yaw = guest_render->rx, guest_pitch = guest_render->ry;
			float guest_dx = 0, guest_dy = 0;
			/* A retained respawn viewport keeps the last safe camera, but its
			 * controller cannot steer that camera until gameplay is READY. */
			if(!suspended[1] && local_session_gameplay_active(guest, 1))
				input_player_look(1, gstate.stats.dt, &guest_dx, &guest_dy);
			guest_yaw -= guest_dx * 2.0F;
			guest_pitch -= guest_dy * 2.0F;
			if(hw_run.enabled && hw_run.split_guest_walk)
				guest_yaw = glm_rad((float)(hw_run.split_guest_yaw_deg
										   + s_guest_heading_extra_deg)); /* GUEST_WALK_UNSTICK */
			/* host_vehicle_smoke_8m: the scripted P2 look (aim at a target). */
			if(s_vehicle_act.guest_look) {
				guest_yaw = s_vehicle_act.guest_yaw;
				guest_pitch = s_vehicle_act.guest_pitch;
			}
			struct camera_player_view camera_owner = {.world=fw.view_terrain[1],
				.ride=&guest->ride,.sleeping=guest->sleeping,.yaw=guest_yaw,.pitch=guest_pitch};
			if(local_session_gameplay_active(guest,1))
				camera_attach_owned(&guest->camera,guest->local_player,tick_delta,
					gstate.stats.dt,&camera_owner);
			struct block_data water = world_get_block(fw.view_terrain[1],
				floorf(guest_render->x),floorf(guest_render->y+.1F),floorf(guest_render->z));
			guest->in_water = water.type == BLOCK_WATER_FLOW || water.type == BLOCK_WATER_STILL;
			if(hw_run.enabled && hw_run.pin_cameras && gstate.world_loaded
			   && !gstate.world_switch_pending)
				for(unsigned v = 0; v < view_count; v++)
					render_cameras[v] = pin_capture_camera(render_cameras[v], v);
			for(unsigned v = 0; v < view_count; v++)
				camera_update_viewport(render_cameras[v], gstate.players[v].in_water,
					gfx_width(), view_rect[v].height > 0 ? view_rect[v].height : gfx_height()/2);
		}

		// S0 audio: the once-per-frame listener update -- reaps finished
		// voices (poll model, no IRQ callbacks) and recomputes every live
		// voice's attenuation + pan from THIS frame's camera. Sits right
		// after camera_update so a voice started by clin_update above is
		// re-gained against the freshest listener on the same frame.
		struct g9_lease_token audio_turn = thread_g9_handoff_work_begin(G9_HANDOFF_AUDIO);
		g9_capture_main(12, true, 0);
		bool audio_complete = audio_turn.generation ? g9_audio_bounded_service() : false;
		bool audio_calibrated_turn = !audio_turn.generation && g9_audio_calibration_service(
			hw_run.run_id, hw_run.profile_name, !lease.enabled && !lease.main_handoff);
		if(audio_turn.generation && !audio_complete) {
			thread_g9_handoff_work_end(audio_turn, G9_HANDOFF_AUDIO, false);
			thread_g9_handoff_audio_abort();
		}
		if((!audio_turn.generation || !audio_complete) && !audio_calibrated_turn) {
		audio_update((vec3) {gstate.players[0].camera.x, gstate.players[0].camera.y,
							 gstate.players[0].camera.z},
					 glm_deg(gstate.players[0].camera.rx));
		// S3: pump the streaming double buffer + advance/stop the music
		// voice. Deliberately OUTSIDE any `if(!paused)` gate -- Beta's
		// paulscode "BgMusic" source keeps sounding through a paused game
		// (nothing in SoundManager checks isGamePaused); only the TIMER
		// that picks a NEW track (audio_music_tick, client_display_tick.c)
		// is paused-gated, matching PlayerControllerSP.updateController.
		audio_music_update();
		}
		g9_capture_main(12, false, 1);
		g9_lease_audio_publish(audio_turn.generation && audio_complete
			? thread_g9_handoff_audio_remaining() : 500);
		if(audio_complete) thread_g9_handoff_work_end(audio_turn, G9_HANDOFF_AUDIO, true);
		clin_g9_handoff_poll(false);

		gstate.stats.ms_bfs = 0.0F;
		if(render_world) {
			watchdog_phase_main(WDM_PRERENDER);
			ptime_t t_bfs = time_get();
			/* B2 H7: flag OFF = the pre-B2 single/union pre-render on
			 * gstate.world (clin_frame_pre_render keeps the failure clear). */
			clin_frame_pre_render(&fw, render_cameras, &render_world);
			gstate.stats.ms_bfs = time_diff_s(t_bfs, time_get()) * 1000.0F;
			stats_record_max(gstate.stats.ms_bfs, &gstate.stats.ms_bfs_max);

			for(unsigned v = 0; v < view_count; v++) {
				struct client_session* owner = &gstate.players[v];
				if(!fw.draw_view[v]) {
					owner->camera_hit.hit=false;
					owner->camera_hit.entity_hit=false;
					owner->camera_hit.entity_id=0;
					owner->camera_hit.assist_hit=false;
					owner->camera_hit.assist_entity_id=0;
					continue;
				}
				/* The per-pane crosshair pick = Beta EntityRenderer.
				 * getMouseOver (critique-1028): the 4.0 block ray, then the
				 * nearest collidable entity within min(block hit, 3.0) --
				 * local AND online (test_attack_assist (1)/(13)). The opt-in
				 * soft snap (docs/deviations.md "Soft-snap melee assist")
				 * only fills assist_hit on a complete miss, from THIS pane's
				 * camera, entities and world; never on a remote server. */
				/* Local split only: the other pane's body is a melee target
				 * and occluder (Beta excludes only the viewer). NULL on a
				 * remote session (no local peer exists there). */
				const struct entity* peer
					= (!gstate.remote_session && v < 2)
					? gstate.players[1 - v].local_player : NULL;
				attack_pick_world((dict_entity_t*)fw.view_entities[v],
								  owner->local_player, peer, render_cameras[v],
								  fw.view_terrain[v],
								  input_attack_assist_level(),
								  gstate.remote_session, &owner->camera_hit);
			}
		} else {
		    clin_frame_pre_render_clear(&fw);
		    gstate.players[0].camera_hit.hit        = false;
		    gstate.players[0].camera_hit.entity_hit = false;
		    gstate.players[0].camera_hit.entity_id  = 0;
		    gstate.players[0].camera_hit.assist_hit = false;
		    gstate.players[0].camera_hit.assist_entity_id = 0;
		}

		watchdog_phase_main(WDM_LIGHTING);
		clin_g9_handoff_poll(false);
		thread_main_pacing_checkpoint_at(THREAD_PACING_LIGHTING);
		ptime_t t_light = time_get();
		clin_frame_lighting(&fw); /* B2 H9a */
		if(hw_run.column_census) {
			column_observer_window(s_anchor.measuring);
			column_observer_client(&gstate.world, -1);
		}
		ptime_t t_build = time_get();
		gstate.stats.ms_light = time_diff_s(t_light, t_build) * 1000.0F;
		stats_record_max(gstate.stats.ms_light, &gstate.stats.ms_light_max);
		// M1 far-ring LOD: assign each chunk its render tier from the camera
		// distance and re-dirty any that crossed a boundary, before the build
		// pass below picks the dirty chunks up and (re)meshes them full or skin.
		watchdog_phase_main(WDM_LOD);
		clin_g9_handoff_poll(false);
		ptime_t t_lod = time_get();
		if(!thread_g9_lease_defer())
			clin_frame_lod(&fw, lod_cameras, lod_view_count); /* B2 H9b */
		ptime_t t_build_chunks = time_get();
		gstate.stats.ms_lod = time_diff_s(t_lod, t_build_chunks) * 1000.0F;
		stats_record_max(gstate.stats.ms_lod, &gstate.stats.ms_lod_max);
		watchdog_phase_main(WDM_BUILD_CHUNKS);
		clin_g9_handoff_poll(false);
		thread_main_pacing_checkpoint_at(THREAD_PACING_BUILD);
		// H-2 band 2 admission gate (2026-09-03 maintainer ruling; BOUNDED
		// 2026-09-04). Near still goes first, but no longer all-or-nothing:
		// world_band2_allow_far admits far (lod==1) chunks once the near
		// backlog fits inside one mesher queue, and unconditionally one frame
		// in WORLD_BAND2_STARVE_RELEASE_FRAMES so a persistent near backlog
		// can never starve band 2 indefinitely (the real-Wii failure the
		// original `pending == 0` edge produced -- see world.c).
		// world_near_ring_status is a bounded disk scan (cost independent of
		// total loaded chunks -- see its doc comment), cheap enough to call
		// every frame; only out_mesh_pending is needed here (out_missing is a
		// streaming-completeness signal, not a scheduling one -- an absent
		// column has nothing to mesh, so it cannot itself starve the mesher).
		{
			/* B2 H9c: each live payload centred on the camera of the first
			 * view that shows it (else player 0's); flag OFF = one payload
			 * centred on player 0 with the whole token budget, as before. */
			w_coord_t bc_centers[2][2];
			for(unsigned bi = 0; bi < fw.n_live; bi++) {
				unsigned cv = 0;
				for(unsigned v = 0; v < 2; v++)
					if(fw.view_terrain[v] == fw.live[bi]) { cv = v; break; }
				bc_centers[bi][0]
					= WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[cv].camera.x));
				bc_centers[bi][1]
					= WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[cv].camera.z));
			}
			int bc_near_mesh_pending = 0;
			int bc_radius = world_server_view_radius() ? world_server_view_radius() : MAX_VIEW_DISTANCE;
			/* REL-MP-COREFIRST (maintainer scope 2026-10-22): nearest-first,
			 * safe core first, ONLY while a remote (online) session's loading
			 * screen is up, and only for player 0's world. Every other frame
			 * -- local single-player loading, all in-game frames, any second
			 * payload -- clears the focus, i.e. the original order. */
			for(unsigned bi = 0; bi < fw.n_live; bi++)
				world_set_build_focus(fw.live[bi],
					bi == 0 && world_build_focus_wanted(gstate.remote_session,
						gstate.players[0].current_screen == &screen_load_world),
					bc_centers[bi][0], bc_centers[bi][1], bc_radius);
			size_t remaining_tokens = clin_frame_build(&fw, CHUNK_MESHER_QLENGTH, bc_centers,
				bc_radius, NULL, &bc_near_mesh_pending);
			if(settle_observer_active()) {
				settle_tokens = remaining_tokens;
				settle_allow_far = world_band2_allow_far(
					bc_near_mesh_pending, fw.live[0]->build_frame - 1);
			}
		}
		gstate.stats.ms_build = time_diff_s(t_build_chunks, time_get()) * 1000.0F;
		stats_record_max(gstate.stats.ms_build, &gstate.stats.ms_build_max);
		watchdog_main_timings_x10(
			stats_ms_x10(gstate.stats.ms_bfs),
			stats_ms_x10(gstate.stats.ms_light),
			stats_ms_x10(gstate.stats.ms_lod),
			stats_ms_x10(gstate.stats.ms_build),
			stats_ms_x10(gstate.stats.ms_bfs_max),
			stats_ms_x10(gstate.stats.ms_light_max),
			stats_ms_x10(gstate.stats.ms_lod_max),
			stats_ms_x10(gstate.stats.ms_build_max));
		gstate.stats.wd_hb_gap_max_ms = watchdog_heartbeat_gap_max_ms();

		// Distinguishes the screen update from world_build_chunks: the phase set at
		// WDM_BUILD_CHUNKS above otherwise stayed stamped across this call, so a
		// hang inside a screen's update read as a chunk-build hang (the 2026-07-27
		// report's phase=6 covered both).
		watchdog_phase_main(WDM_SCREEN_UPDATE);
		clin_g9_handoff_poll(false);
		thread_main_pacing_checkpoint_at(THREAD_PACING_SCREEN);
		g9_probe_main_mark(9, true, 0, 0, 0);
		ptime_t profile_screen = hw_run.enabled ? time_get() : (ptime_t){0};
		/* An established transfer keeps two owner-local panes even while the
		 * guest COMMIT has no body yet. The legacy single-player recovery gate
		 * would otherwise treat that short gap as a solo session and pause both
		 * worlds; each owner already has its own recovery consumer below. */
		bool recovery_modal = view_count==2&&loading.keep_split
			? false : screen_controller_recovery();
		bool menu_consumed[2]={container_converted||(view_count==1&&gstate.players[0].current_screen==&screen_ingame&&local_menu_host_active(&gstate.players[0])&&gstate.players[0].menu_host.split_owned),false};
  bool stats_modal[2] = {false, false};
  for(unsigned p = 0; p < view_count; p++) {
   stats_modal[p] = screen_stats_save_update(p, input_player_frame(p), 0,
    view_rect[p].y, gfx_width(), view_rect[p].height);
   if(stats_modal[p]) {
    menu_consumed[p] = true;
    if(gstate.players[p].local_player) gstate.players[p].local_player->data.local_player.capture_input = false;
   }
  }
  if(stats_modal[0] && !screen_stats_save_active(0)
     && gstate.players[0].current_screen == &screen_ingame
     && !local_pause_active(&gstate.players[0].pause) && !local_menu_host_active(&gstate.players[0]))
   input_pointer_enable(false);
  if(view_count==2&&!recovery_modal&&!gstate.paused
     &&(host_loading_pane||gstate.players[0].current_screen==&screen_ingame
        ||gstate.players[0].current_screen==&screen_death)) {
   bool pointer=screen_stats_save_active(0)||screen_stats_save_active(1);
   for(unsigned p=0;p<2;p++) {
    struct client_session* owner=&gstate.players[p];const struct input_player_frame* frame=input_player_frame(p);
    if(stats_modal[p]) continue;
    if(host_loading_pane&&p==0) {
     if(owner->local_player)owner->local_player->data.local_player.capture_input=false;
     menu_consumed[p]=true;
     continue;
    }
    if(guest_loading_pane&&p==1) {
     if(owner->local_player)owner->local_player->data.local_player.capture_input=false;
     menu_consumed[p]=true;
     continue;
    }
    /* split_recovery_6m drives the PRODUCTION handlers below (recovery,
     * pause, leave) with its own owned frame -- the loss, the neutral pass,
     * the deliberate Resume and the menu presses are all just this frame. */
    if(p==1&&s_split_recovery_act.guest_frame_valid)frame=&s_split_recovery_act.guest_frame;
    else if(p==0&&s_split_recovery_act.host_frame_valid)frame=&s_split_recovery_act.host_frame;
    /* host_vehicle_smoke_8m: the scripted owned GUI frames (Save and quit,
     * the guest's own Leave) through the same production handlers. */
    if(p==1&&s_vehicle_act.guest_frame_valid)frame=&s_vehicle_act.guest_frame;
    else if(p==0&&s_vehicle_act.host_frame_valid)frame=&s_vehicle_act.host_frame;
    bool death_view=local_death_view(owner,p,&loading);
    if(local_death_arbitrate(owner,p,frame,view_rect[p],death_view,&pointer)) {
     menu_consumed[p]=true;continue;
    }
    struct server_rpc_source source={.slot=(uint8_t)p,.generation=p?owner->local_generation:0};
    /* Retain compact menus in a centered 240px region shared by input
     * and drawing. A hidden owner's Resume is button-operated. */
    int vh=240,vy=view_rect[p].y+(view_rect[p].height>vh?(view_rect[p].height-vh)/2:0);
    struct input_player_frame recovery_frame;
    if(frame && view_rect[p].height < vh) {
     recovery_frame=*frame;recovery_frame.pointer_valid=false;frame=&recovery_frame;
    }
    bool pause=local_pause_active(&owner->pause);
    bool opening=frame&&frame->connected&&frame->pressed[IB_HOME]
     &&owner->menu_host.mode!=LOCAL_MENU_CREATIVE;
    if(!menu_consumed[p]&&(pause||opening||view_rect[p].height<240)) {
     menu_consumed[p]=local_menu_host_recovery_frame(owner,p,frame,0,vy,gfx_width(),vh);
     if(!menu_consumed[p]&&(pause||opening))menu_consumed[p]=local_pause_frame(owner,p,frame,vy,vh);
    }
    if(!host_loading_pane&&gstate.players[0].current_screen!=&screen_ingame
       &&gstate.players[0].current_screen!=&screen_death)break;
    if(view_rect[p].height<240)menu_consumed[p]=true;
    /* An armed controllerless guest supplies its own frame above. Exempt only
     * plain gameplay from BOTH the real-device recovery consumer and the
     * driven gate; every reply/menu/window/pause guard remains authoritative. */
    bool synthetic_guest=hw_run.enabled&&p==1&&(hw_run.split_guest_walk||s_split_recovery_act.drive_guest_body||s_vehicle_act.drive_guest_body)
     &&!owner->local_reply_failed&&!local_menu_host_active(owner)
     &&owner->visible_window<0&&!local_pause_active(&owner->pause);
    if(!menu_consumed[p]&&!synthetic_guest)menu_consumed[p]=local_menu_host_frame(owner,p,frame,0,vy,gfx_width(),vh,gstate.stats.dt,local_container_send,&source);
    /* The hardware harness drives player 0 through the autopilot with NO
     * physical controller (wpad -1): its steering rides the same capture
     * path, so "a device is connected" must not be the gate for it -- the
     * merged approved-UI rule parked the host at spawn for the whole
     * split_play run (stg49, 2026-09-11; the split lane had no such gate). */
    bool driven=(frame&&frame->connected)||(p==0&&gstate.autopilot.active)||synthetic_guest;
    if(owner->local_player)owner->local_player->data.local_player.capture_input=driven&&local_session_gameplay_active(owner,p)&&!owner->local_reply_failed&&!menu_consumed[p]&&!local_menu_host_active(owner)&&!local_pause_active(&owner->pause);
    pointer|=local_menu_host_active(owner)||local_pause_active(&owner->pause)||owner->menu_host.recovery.phase!=CONTROLLER_RECOVERY_NONE;
   }
   if(host_loading_pane||gstate.players[0].current_screen==&screen_ingame
      ||gstate.players[0].current_screen==&screen_death)input_pointer_enable(pointer);
  }
  /* A guest leaving must not dismiss the host's still-open local Pause. */
  if(view_count==1&&!stats_modal[0]&&local_pause_active(&gstate.players[0].pause)) {
   menu_consumed[0]=true;
   int ph=view_rect[0].height<gfx_height()?240:gfx_height();
   int py=(view_rect[0].height>ph?(view_rect[0].height-ph)/2:0);
   if(!recovery_modal)local_pause_frame(&gstate.players[0],0,
    s_vehicle_act.host_frame_valid?&s_vehicle_act.host_frame
    :s_split_recovery_act.host_frame_valid?&s_split_recovery_act.host_frame:input_player_frame(0),py,ph);
   if(gstate.players[0].local_player)gstate.players[0].local_player->data.local_player.capture_input=false;
   input_pointer_enable(true);
  }
  boot_gate_update(); /* BOOT-CREDITS-1028: cards, readiness, admission */
  /* While the cards are up (or the A that ended them is held), the cards own
   * player 0's input: the title screen must not see it this frame. */
  if(boot_gate_blocks_title()) menu_consumed[0] = true;
  if(!recovery_modal&&!menu_consumed[0]&&view_count==2&&gstate.players[0].current_screen==&screen_death)
   screen_death_update_viewport(gstate.stats.dt,gfx_width(),view_rect[0].height);
  /* Loading consumes player input, but its own timer/readiness update must
   * continue. Keep ordinary gameplay and modal input suppression intact. */
  else if(!recovery_modal
          && (!menu_consumed[0]
              || (!stats_modal[0] && host_loading_pane
                  && gstate.players[0].current_screen == &screen_load_world))
          && gstate.players[0].current_screen->update) {
			SERVICE_OBSERVER(struct input_device d; if(input_player_device(0, &d)) service_observer_device(0, 4|8, &d));
			gstate.players[0].current_screen->update(gstate.players[0].current_screen,
											gstate.stats.dt);
		}

		/* mp_chat_login_3m: one call per rendered frame, right AFTER player 0's
		 * screen update and BEFORE the input_poll at the bottom of this loop.
		 * That is the only point where the driver can both (a) observe the very
		 * owner frame the screen just consumed, and (b) post the request the
		 * next poll turns into the following frame's owner frame. Zeroed and
		 * inert for every other profile and for normal play. */
		if(chat_login_armed()) {
			struct entity* cl_body = gstate.players[0].local_player;
			chat_login_frame(&(struct chat_login_ctx) {
				.remote_session = gstate.remote_session,
				.world_loaded = gstate.world_loaded,
				.world_switch_pending = gstate.world_switch_pending,
				.local_player = cl_body != NULL,
				.screen_is_ingame
					= gstate.players[0].current_screen == &screen_ingame,
				.screen_is_chat
					= gstate.players[0].current_screen == &screen_chat,
				/* main.c's own single evaluation for this frame -- the driver
				 * never re-steps the recovery machine. */
				.recovery_modal = recovery_modal,
				/* A compact id for the screen, so a driver TIMEOUT can name
				 * what was on it. screen_multiplayer_connect is the _impl
				 * object screen.h exports. */
				.screen_id
					= gstate.players[0].current_screen == &screen_ingame
						? CHAT_LOGIN_SCREEN_INGAME
					: gstate.players[0].current_screen == &screen_chat
						? CHAT_LOGIN_SCREEN_CHAT
					: gstate.players[0].current_screen == &screen_load_world
						? CHAT_LOGIN_SCREEN_LOAD_WORLD
					: gstate.players[0].current_screen == &screen_title
						? CHAT_LOGIN_SCREEN_TITLE
					: gstate.players[0].current_screen
							== &screen_multiplayer_connect_impl
						? CHAT_LOGIN_SCREEN_MP_CONNECT
					: gstate.players[0].current_screen == &screen_pause
						? CHAT_LOGIN_SCREEN_PAUSE
					: gstate.players[0].current_screen == &screen_death
						? CHAT_LOGIN_SCREEN_DEATH
						: CHAT_LOGIN_SCREEN_OTHER,
				.x = cl_body ? cl_body->pos[0] : 0.0F,
				.y = cl_body ? cl_body->pos[1] : 0.0F,
				.z = cl_body ? cl_body->pos[2] : 0.0F,
				.epoch = clin_input_epoch(),
				.now_ms = (uint32_t)time_diff_ms(hw_started, time_get()),
				.ring = &gstate.chat_ring});
		}

		/* mp_play_10m: ONE call per rendered frame, at the SAME point and for
		 * the same reason as the hunk above -- after player 0's screen update
		 * and before the input_poll at the bottom of this loop, the only
		 * point where a driver can both observe the owner frame the screen
		 * just consumed and post the request the next poll turns into the
		 * following frame's owner frame.
		 *
		 * AFTER chat_login_frame, never instead of it: in play mode the login
		 * machine is armed (chat_login_arm_play), so chat_login_armed() is
		 * true and the hunk above steps it exactly once per frame.
		 * mp_play_frame() deliberately does NOT step it again -- doing so
		 * would halve every login budget -- it only READS the phase the step
		 * above just produced. Zeroed and inert for every other profile and
		 * for normal play (mp_play_frame returns on its first statement while
		 * unarmed). */
		if(mp_play_armed()) {
			struct entity* mpp_body = gstate.players[0].local_player;
			mp_play_frame(&(struct mp_play_ctx) {
				.remote_session = gstate.remote_session,
				.world_loaded = gstate.world_loaded,
				.world_switch_pending = gstate.world_switch_pending,
				.local_player = mpp_body != NULL,
				/* The screen OBJECT, not a compact id: mp_play_screen_of()
				 * owns the screen-identity table, next to the core that
				 * branches on it (and it reaches the MP screens through
				 * screen.h's pointers, which this file's _impl comparisons
				 * cannot do from the engine pool). */
				.current_screen = gstate.players[0].current_screen,
				/* main's own single evaluation for this frame -- the driver
				 * never re-steps the recovery machine, and requests no
				 * action on a frame where the modal owns the update. */
				.recovery_modal = recovery_modal,
				.epoch = clin_input_epoch(),
				.now_ms = (uint32_t)time_diff_ms(hw_started, time_get()),
				.ring = &gstate.chat_ring});
		}
		if(mp_pause_menu_armed()) {
			struct entity* body = gstate.players[0].local_player;
			struct screen* current = gstate.players[0].current_screen;
			mp_pause_menu_frame(&(struct mpm_obs) {
				.ms = (uint32_t)time_diff_ms(hw_started, time_get()),
				.tick = mpm_client_tick,
				.x = body ? body->pos[0] : 0.0f,
				.y = body ? body->pos[1] - 1.62f : 0.0f,
				.z = body ? body->pos[2] : 0.0f,
				.vy = body ? body->vel[1] : 0.0f,
				.yaw_deg = body ? body->orient[0] * 180.0f / GLM_PI : 0.0f,
				.remote = gstate.remote_session,
				.world = gstate.world_loaded && !gstate.world_switch_pending,
				.body = body != NULL,
				.ground = body && body->on_ground,
				.ingame = current == &screen_ingame,
				.pause = current == &screen_pause,
				.title = current == &screen_title,
				.leave_focus = current == &screen_pause
					&& screen_pause_focus_is_leave()});
		}

		/* S1-b (mp-split-0912): the GUEST's own in-game action pass, right after
		 * the host's. Evaluated AFTER that update on purpose -- a host press that
		 * just opened the pause/inventory/death screen closes this gate on the
		 * same frame, so the guest can never act into a screen the host left.
		 * Runs every frame, admitted or not: the gate is also what CLEARS the
		 * guest's pending dig/animation on a controller drop, a leave, a new
		 * generation or an ownership change, and what demands a neutral frame
		 * before re-arming. The RPC source is built inside the action path from
		 * players[1].local_generation (never clin_input_epoch, Astra call #6
		 * item 1). */
		{
			struct client_session* guest = &gstate.players[1];
			struct input_device guest_device = {0};
			bool guest_owns = input_player_device(1, &guest_device);
			const struct input_player_frame* guest_input = input_player_frame(1);
			/* host_vehicle_smoke_8m: the controllerless scripted P2 acts
			 * through its own frame with one stable scripted device identity
			 * (the gate tracks identity changes; no hardware is read). */
			if(s_vehicle_act.guest_frame_valid && !guest_owns) {
				guest_device = (struct input_device) {
					.kind = INPUT_DEVICE_GAMECUBE, .channel = 3};
				guest_owns = true;
				guest_input = &s_vehicle_act.guest_frame;
			}
			bool guest_admitted = view_count == 2 && !recovery_modal
				&& !gstate.paused && !menu_consumed[1]
				&& (loading.guest_continues
				    ||gstate.players[0].current_screen==&screen_ingame
				    ||gstate.players[0].current_screen==&screen_death)
				&& (loading.guest_continues
				    ||(gstate.world_loaded&&!gstate.world_switch_pending))
				&& guest->local_bound && guest->local_gameplay_ready
				&& guest->windows[WINDOWC_INVENTORY]
				/* Astra call #8 (iv): zero health rejects guest gameplay. The
				 * value is the one the tagged CRPC_PLAYER_SET_HEALTH reply
				 * maintains on players[1]'s own body. */
				&& guest->local_player && guest->local_player->health > 0
				&& !guest->death.active
				&& !local_menu_host_active(guest)
				&& !local_pause_active(&guest->pause)
				&& guest->menu_host.recovery.phase == CONTROLLER_RECOVERY_NONE
				&& guest_owns;
			if(screen_ingame_guest_action_frame(guest, guest_input,
					guest_owns ? &guest_device : NULL, guest_admitted)) {
				SERVICE_OBSERVER(if(!s_vehicle_act.guest_frame_valid) service_observer_device(1, 4|8, &guest_device));
				screen_ingame_update_owned(guest, 1, guest_input, gstate.stats.dt);
			}
		}

		g9_probe_main_mark(9, false, view_count, 0, 0);
		if(hw_run.enabled) frame_screen_ms = time_diff_s(profile_screen, time_get()) * 1000.0f;
		if(thread_g9_handoff_enabled()) {
			/* Gameplay/menu consumers above have finished. Poll once; this
			 * local-only T2 moves the next input sample ahead of draw/flip.
			 * Retire the previous ride edge before advancing the input serial. */
			entity_local_player_ride_action_frame_end();
			ptime_t input_start = time_get();
			g9_frame_input_poll(split_experimental);
			handoff_input_us = time_diff_us(input_start, time_get());
			handoff_input_polled = true;
			clin_g9_handoff_poll(false);
		}
		watchdog_phase_main(WDM_GFX_FLIP);
		g9_probe_main_mark(7, true, 0, 0, 0);
		gfx_flip_buffers(&gstate.stats.dt_gpu, &gstate.stats.dt_vsync);
		g9_probe_main_mark(7, false, 1, 0, 0);

		// Render-token bookkeeping must run EVERY frame, paused or not. The
		// screen update above can flip gstate.paused mid-frame (HOME press)
		// AFTER world_pre_render already chunk_ref'd this frame's visible set;
		// gating this call on !paused orphaned those refs (refcount never
		// returns to 0 -> the chunk leaks past eviction: struct chunk in MEM1
		// + ~10KB blocks/displaylists in MEM2, every pause press). Safe here
		// by the established pattern: gfx_flip_buffers has completed, and on
		// paused frames render_world is false so this only drains gpu_busy.
		ptime_t profile_completed = hw_run.enabled ? time_get() : (ptime_t){0};
		clin_frame_render_completed(&fw, render_world); /* B2 H10 */
		clin_g9_handoff_poll(true); /* GPU/render leases returned; no new draw. */
		/* A relight OOM may have arrived after this frame acquired render
		 * tokens. Transfer them above, then retire before drawing anything. */
		clin_recover_column_storage();
		if(column_view_recovery_pending()) {
			render_world = false;
			clin_frame_pre_render_clear(&fw);
		}
		if(hw_run.enabled) frame_completed_ms = time_diff_s(profile_completed, time_get()) * 1000.0f;
		g9_probe_main_mark(10, true, 0, 0, 0);
		ptime_t profile_draw = hw_run.enabled ? time_get() : (ptime_t){0};
		achievement_notice_update(time_get());

		/* Pause stops simulation ticks, not drawing the world backdrop. The
		 * completed render-token handoff above still precedes mesh reception. */
		/* Split-dims B1: a context parked after a scoped reset resumes only
		 * once its mesher leases are back, and they come back HERE -- so a
		 * paused frame must still receive while one is parked, or the held
		 * world traffic would wait forever. Safe at this point for the same
		 * reason as ever: the render-token hand-off above has completed. */
		bool draw_viewports = !gstate.paused || render_world || clin_world_quiesce_pending();
		if(draw_viewports) {
			// must not modify displaylists while still rendering!
			watchdog_phase_main(WDM_MESH_RECV);
			ptime_t t_recv = time_get();
			if(thread_g9_handoff_enabled() && thread_g9_lease_defer())
				clin_g9_handoff_poll(true);
			else if(thread_g9_lease_defer())
				(void)chunk_mesher_receive_budget((uint32_t)lease.service_us);
			else chunk_mesher_receive();
			gstate.stats.ms_recv = time_diff_s(t_recv, time_get()) * 1000.0F;

			vec3 top_plane_color, bottom_plane_color, atmosphere_color;
			/* World.func_4079_a's lightning flash term (render_lightning.h);
			 * 0 with no online bolt, which is the unchanged colour path. */
			daytime_sky_colors_flash(daytime, gstate.world.dimension,
									 lightning_client_flash(tick_delta,
										 (int32_t)gstate.world.dimension),
									 top_plane_color, bottom_plane_color,
									 atmosphere_color);

			if(render_world) {
				gfx_clear_buffers(atmosphere_color[0], atmosphere_color[1],
									atmosphere_color[2]);
			} else {
				gfx_clear_buffers(128, 128, 128);
			}
			/* BG-BAKE-1028: one bounded backdrop-bake step (title only, no
			 * world alive). After the token hand-off + mesh reception above
			 * and this frame's clear; a capture frame resets the buffers to
			 * this clear before the menu draws. */
			s_bake_remote_provider = mp_provider_remote;
			s_bake_server_owns_world = server.player.has_pos;
			/* Boot credits / opaque Apply are front-end loads: the job may
			 * run there, every world/provider exclusion still applies. */
			s_bake_loading_frontend = s_gate.active || screen_texpacks_busy();
			screen_texturepacks_backdrop_service(!bake_host_world_alive(NULL));
			bake_frame_service();

			gfx_fog_color(atmosphere_color[0], atmosphere_color[1],
							atmosphere_color[2]);

			// Per-frame GP-load gauge (2026-09-03 far-ring frame-drop root-cause
			// doc's "instrument caveats": the heartbeat's existing `dlcall` field
			// is g_displaylist_badcall, a corruption counter that reads 0 on any
			// healthy run -- there was no actual per-frame draw-call/triangle
			// count to correlate against the vsync/GP telemetry dips). Reset
			// unconditionally once per frame (not just inside the render_world
			// branch below) so a no-render frame reports zero rather than a
			// stale count from whenever the world last drew.

			displaylist_frame_counters_reset();
			gstate.stats.chunks_rendered = 0;
			gstate.stats.ms_render = 0;
			ptime_t draw_time = time_get();
			for(unsigned viewport = 0; viewport < view_count; viewport++) {
				struct client_session* owner = &gstate.players[viewport];
				/* ci-1101/31: item draws in this pass pick this player's
				 * compass/clock needle (render_item_flat). */
				render_item_set_viewport(viewport);
				bool render_view=render_world&&fw.draw_view[viewport];
				struct camera* render_camera=render_cameras[viewport];
				int view_y = view_rect[viewport].y;
				int view_height = view_rect[viewport].height;
				if(view_height <= 0) continue;
				/* Split-dims B2 H11-H13: this view's world -- terrain,
				 * entities, selected particle/sign/piston stores, clock,
				 * dimension. Flag OFF: gstate.world / gstate.entities / the
				 * primary stores, nothing re-selected, venv.daytime ==
				 * daytime and the weather is the gstate fields. */
				struct clin_view_world vscope;
				/* Enter by the slot resolved ONCE for this frame (fw), so
				 * the draw uses exactly what was pre-rendered even if an
				 * owned screen update re-bound a view since (Fable LOW-4). */
				clin_world_slot_enter(fw.view_slot[viewport], &vscope);
				struct clin_view_env venv;
				clin_frame_view_env(&vscope, daytime, gstate.paused, draw_time,
									daytime_view[viewport], &venv);
				daytime_view[viewport] = venv.daytime;
				vec3 vtop, vbottom, vatmos;
				glm_vec3_copy(top_plane_color, vtop);
				glm_vec3_copy(bottom_plane_color, vbottom);
				glm_vec3_copy(atmosphere_color, vatmos);
				/* Flag ON: every viewport sets its own sky + fog colours from
				 * its own daytime, so no view inherits the previous view's
				 * fog (Fable MED-2). Flag OFF: skipped -- the pre-B2 colours
				 * computed once above stand, no extra gfx call. */
				if(clin_world_payloads_enabled()) {
					daytime_sky_colors_flash(venv.daytime, venv.dimension,
											 lightning_client_flash(tick_delta,
												 (int32_t)venv.dimension),
											 vtop, vbottom, vatmos);
					gfx_fog_color(vatmos[0], vatmos[1], vatmos[2]);
				}
				// Own the viewport even for one full-screen player: a centered
				// menu may have left a 240px viewport on the preceding frame.
				gfx_set_viewport(0, view_y, gfx_width(), view_height);
				// Top-origin; the PC backend handles the GL bottom-origin flip.
				gfx_scissor(true, 0, view_y, gfx_width(), view_height);
				if(render_view && clin_world_payloads_enabled()) {
					/* GX copy-clear is frame-wide. Paint this view's open air before
					 * geometry, inside its scissor, so two dimensions can coexist. */
					gfx_mode_gui_viewport(gfx_width(), view_height);
					gfx_texture(false);
					gfx_blending(MODE_OFF);
					gfx_alpha_test(false);
					gutil_texquad_col(0, 0, 0, 0, 0, 0, gfx_width(),
										view_height, vatmos[0], vatmos[1], vatmos[2], 255);
					gfx_texture(true);
					gfx_mode_world();
					gfx_matrix_projection(render_camera->projection, true);
					gfx_matrix_modelview(render_camera->view);
					gfx_lighting(true);
					gfx_blending(MODE_BLEND);
					gfx_alpha_test(true);
					gfx_fog(false);
				}
				gfx_mode_world();
				gfx_matrix_projection(render_camera->projection, true);


				if(render_view) {
					// Beta World.calculateSkylightSubtracted folds the weather
					// darkening into the SAME integer sky-subtract as the day/night
					// curve, so a storm dims every block, not just the
					// precipitation quads. Current (not prev/cur-interpolated)
					// strengths: Beta evaluates this at partial == 1.0F only --
					// see daytime.h. [740c583: World.java:999/1763]
					//
					// Nether gate: Beta's Nether is a separate World instance whose
					// strengths are never set. This engine SHARES one weather struct
					// across a dimension switch and server_weather_tick leaves the
					// strengths FROZEN (not zeroed) when you portal in mid-storm, so
					// without this gate an overworld thunderstorm would darken the
					// Nether. Matches the sibling dimension gates below.
					gfx_update_light(daytime_light_sun_dimension(venv.daytime, venv.dimension), /* w2: calculateSkylightSubtracted sun term */
									 venv.rain_strength,
									 venv.thunder_strength,
									 world_dimension_light(vscope.terrain));

					if(venv.sky)
						gutil_sky_box_owned(render_camera->view,
										daytime_celestial_angle(venv.daytime), vtop,
										vbottom, render_camera);

					watchdog_phase_main(WDM_RENDER);
					watchdog_draw_breadcrumb(WDB_WORLD, 0, 0);
					/* R5 item 2b: arm the ordered GX scene hash for this
					 * frame's chunk lists once the scene has settled. */
					if(hw_run.pool_selftest && viewport == 0) {
						const float st_cam[5] = {render_camera->x,
							render_camera->y, render_camera->z,
							render_camera->rx, render_camera->ry};
						bool st_clean = false;
						bool st_settled = pool_selftest_scene_settled(&st_clean);
						pool_selftest_frame_begin(st_settled, st_clean, st_cam,
							(uint32_t)time_diff_ms(hw_started, time_get()));
					}
					ptime_t t_render = time_get();
					gstate.stats.chunks_rendered += (view_count == 1 || !fw.shared
						||!fw.draw_view[0]||!fw.draw_view[1])
						? world_render(vscope.terrain, render_camera, false)
						: world_render_view(vscope.terrain, render_camera, viewport, false);
					render_piston_moving_all(render_camera->view, tick_delta);
					gstate.stats.ms_render
						+= time_diff_s(t_render, time_get()) * 1000.0F;
				} else {
					gstate.stats.chunks_rendered = 0;
				}

				/* A retained transfer pane has no gameplay screen while its
				 * body is being rebound; its loading UI is drawn below. */
				if(owner->current_screen && owner->current_screen->render3D) {
					gfx_fog(false);
					watchdog_draw_breadcrumb(WDB_SCREEN3D, 0, 0);
					if(view_count == 1)
						owner->current_screen->render3D(owner->current_screen,render_camera->view);
					else if(local_session_gameplay_active(owner,viewport))
						screen_ingame_render3D_owned(owner,render_camera->view);
				}

				if(render_view) {
					gfx_fog(false);
					watchdog_draw_breadcrumb(WDB_PARTICLES, 0, 0);
					particle_render(
						render_camera->view,
						(vec3){render_camera->x,render_camera->y,render_camera->z},
						tick_delta);
					// ENTITY-PASS PRECONDITION (GX-wedge audit 2026-06-11): the
					// per-mob display lists emit GX_Color1x8 and REQUIRE
					// CLR0=INDEX8. render3D (held item / bare hand) exits with
					// CLR0=DIRECT, and until now the precondition was healed only
					// by particle_render HAPPENING to end with gfx_lighting(true)
					// -- an accidental convention, the exact class this hang
					// lives in. Own it explicitly here instead.
					gfx_lighting(true);
					entities_client_render(vscope.entities,render_camera,tick_delta);
					if(view_count==2 && fw.shared)
						local_render_other_player_draw(viewport,render_camera,tick_delta);
					// Remote-player name tags [740c583 RenderPlayer.renderName]:
					// Beta draws a living entity's label as the tail of its own
					// doRenderLiving call, i.e. right after that entity's model --
					// this batched pass instead runs ONCE per frame, immediately
					// after the whole entity pass (NOT per-entity), mirroring the
					// sign-text feed's deferred/lazy state discipline right below.
					// Iterates gstate.entities itself (independent of entity.c's
					// own render cull) so the 64-block cap holds even though the
					// per-entity cull above is now raised to 64 too, keeping tag
					// and model visibility in lockstep -- see entity.c's
					// entities_client_render comment.
					render_name_tag_draw_pass(vscope.entities,render_camera,
											  tick_delta);
					// Sign text (wave 3): Beta draws its TileEntitySpecialRenderers
					// right after the entity pass, so this sits here rather than
					// with the block mesh -- the board is chunk geometry, the four
					// text lines are their own pass. Early-outs to nothing when no
					// sign is within reading distance.
					sign_text_render_feed_owned(render_camera->view,render_camera);
					gfx_fog(true);

					#ifdef GFX_FANCY_LIQUIDS
					watchdog_draw_breadcrumb(WDB_WORLD_T, 0, 0);
					if(view_count==1||!fw.shared||!fw.draw_view[0]||!fw.draw_view[1])
						world_render(vscope.terrain,render_camera,true);
					else world_render_view(vscope.terrain,render_camera,viewport,true);
					#endif

					// render-slice (RENDER_DISPATCH.md): [740c583:
					// EntityRenderer.renderRainSnow], right after the translucent
					// world pass and before clouds -- Beta's own order (see
					// render_weather.h). Overworld only -- the strength gate
					// already returns immediately in the Nether (weather sim's
					// hasNoSky keeps rain_strength at 0 there), but the dimension
					// check is cheap and matches the sibling GFX_CLOUDS gate below.
					if(venv.sky) {
						watchdog_draw_breadcrumb(WDB_WEATHER, 0, 0);
						/* Online lightning bolts [RenderLightningBolt via
						 * RenderGlobal.renderEntities' weatherEffects loop];
						 * restores the same state precip expects. */
						render_lightning_bolts(render_camera->view,
							(vec3){render_camera->x, render_camera->y,
								   render_camera->z},
							(int32_t)venv.dimension);
						render_rain_snow_owned(tick_delta, owner->local_player,
								render_camera->view);
					}

					#ifdef GFX_CLOUDS
					if(venv.sky) {
						watchdog_draw_breadcrumb(WDB_CLOUDS, 0, 0);
						if(view_count==1)gutil_clouds(render_camera->view,daytime_brightness_dimension(venv.daytime, venv.dimension));
						else gutil_clouds_owned(render_camera->view,daytime_brightness_dimension(venv.daytime, venv.dimension),render_camera,draw_time);
					}
					#endif
				}

				watchdog_draw_breadcrumb(WDB_GUI, 0, 0);
				gfx_mode_gui_viewport(gfx_width(), view_height);
				if(owner->local_player
					 && fire_overlay_visible(owner->local_player->on_fire,
										 owner->sleeping, render_view)
				   && texture_anim.data && texture_anim.width == 256
				   && texture_anim.height == 256) {
					render_first_person_fire(render_camera,
						time_diff_ms(vscope.terrain->anim_timer, time_get()));
					gfx_mode_gui_viewport(gfx_width(), view_height);
				}

				// misc/water.png is absent on a default install; without it this
				// overlay would draw a fullscreen quad textured by whatever is
				// stale in the slot. Skip until a pack supplies it.
				if(render_view&&owner->local_player&&owner->in_water&&texture_water.data) {
					struct entity* player = owner->local_player;
					int light_y = underwater_overlay_sample_y_local_player(
						player->pos[1], player->height);
					struct block_data light = world_get_block(vscope.terrain,
						floorf(player->pos[0]), light_y, floorf(player->pos[2]));
					int sky_subtract = daytime_skylight_subtracted(
						daytime_light_sun_dimension(venv.daytime, venv.dimension),
						venv.rain_strength,
						venv.thunder_strength);
					struct underwater_overlay_color tint = underwater_overlay_tint(
						light.sky_light, light.torch_light, sky_subtract,
						world_dimension_light(vscope.terrain));
					gfx_bind_texture(&texture_water);
					gutil_texquad_col(0,0,-render_camera->rx/GLM_PI*256,
										render_camera->ry/GLM_PI*256,512,
										512 * (float)view_height / (float)gfx_width(),
										gfx_width(), view_height, tint.r, tint.g,
										tint.b, tint.a);
				}

				/* Never squeeze menu text into an arriving/departing strip. */
				if(view_height < 240) {
					clin_world_view_leave(&vscope);
					continue;
				}
				if(local_menu_host_active(owner)
				   || ((view_count > 1 || view_height != gfx_height())
				       && local_pause_active(&owner->pause))
				   || (view_count > 1 && owner->menu_host.recovery.phase != CONTROLLER_RECOVERY_NONE)) {
					int menu_y = view_y + (view_height - 240)/2;
					gfx_set_viewport(0, menu_y, gfx_width(), 240);
					view_height = 240;
					gfx_mode_gui_viewport(gfx_width(), view_height);
				}
				if(host_loading_pane&&viewport==0) {
					if(owner->current_screen->render2D)
						owner->current_screen->render2D(owner->current_screen,gfx_width(),view_height);
				} else if(guest_loading_pane&&viewport==1) {
					local_guest_transfer_loading_render(gfx_width(),view_height,
						guest_loading_elapsed,fw.view_terrain[1]);
				} else if(view_count==1&&!local_menu_host_active(owner)) {
					if(owner->current_screen->render2D)
						owner->current_screen->render2D(owner->current_screen, gfx_width(), view_height);
				} else if(local_menu_host_active(owner)) {
     struct container_glyphs glyphs;container_compact_player_glyphs(&glyphs,input_player_frame(viewport));
     local_menu_host_render(owner,&glyphs);
				} else if(local_pause_active(&owner->pause)) {
					local_pause_render(&owner->pause,viewport,
						input_player_frame(viewport),0,0,gfx_width(),view_height);
				} else if(owner->death.active
				   ||(viewport==1&&owner->local_respawn_loading)) {
					screen_death_owner_render(owner,viewport,gfx_width(),view_height);
				} else {
					/* Approved split pause hides the gameplay HUD (menus.html:945;
					 * Astra ui1): local pause owns this viewport, nothing under it.
					 * Same prompt adapter the single-view path uses: a player
					 * that owns no device yet must still get the fallback
					 * frame (glyphs -1, the join prompt on Wii), never a raw
					 * NULL that silently blanks its whole control row.
					 * (merge 2026-09-11: the split lane's adapter inside the
					 * approved-UI menu-host chain from main.) */
					struct input_player_frame prompt;
					input_prompt_frame(viewport, &prompt);
					 screen_ingame_render2D_owned(owner, &prompt, gfx_width(),
											 view_height);
				}
				if(view_count==1)local_pause_render(&owner->pause,viewport,
					input_player_frame(viewport),0,0,gfx_width(),view_height);
    if(view_count>1)local_menu_host_recovery_render(owner,input_player_frame(viewport),gfx_width(),view_height);
    else if(local_pause_active(&owner->pause))screen_controller_recovery_render(gfx_width(),view_height);
				screen_stats_save_render(viewport, input_player_frame(viewport), gfx_width(), view_height);
				clin_world_view_leave(&vscope);
			} /* viewports */
			render_item_set_viewport(0);
			// Menu centering can change the viewport even with view_count=1.
			gfx_scissor(false, 0, 0, 0, 0);
			gfx_set_viewport(0, 0, gfx_width(), gfx_height());
			gfx_mode_gui();
			if(view_count > 1 || view_rect[0].height != gfx_height()) {
				for(unsigned p = 0; p < 2 && view_count == 2; p++) {
					if(!suspended[p] || view_rect[p].height >= 240) continue;
					const struct controller_recovery* recovery = &gstate.players[p].menu_host.recovery;
					const char* action = recovery->phase == CONTROLLER_RECOVERY_READY
						? "Press A to resume" : recovery->phase == CONTROLLER_RECOVERY_NEUTRAL
						? "Release buttons and centre both sticks" : "Press 1+2 on the Remote, then A on the controller";
					char notice[100];
					snprintf(notice, sizeof notice, "P%u: %s - world continues", p+1, action);
					gfx_texture(false);
					gutil_texquad_col(8, 8, 0, 0, 0, 0, gfx_width()-16, 32, 0, 0, 0, 210);
					gfx_texture(true);
					gutil_text(16, 16, notice, 14, true);
				}
			}
		} else if(gstate.players[0].current_screen->render2D)
			gstate.players[0].current_screen->render2D(gstate.players[0].current_screen,
				gfx_width(), gfx_height());
		boot_gate_draw(); /* BOOT-CREDITS-1028: the cards cover the frame */

  if(!draw_viewports) screen_stats_save_render(0, input_player_frame(0), gfx_width(), gfx_height());
		g9_probe_main_mark(10, false, gstate.stats.chunks_rendered, 0, 0);
		clin_g9_handoff_poll(false); /* COMPLETE draw batch; no mesh retirement. */
		if(hw_run.enabled) frame_draw_ms = time_diff_s(profile_draw, time_get()) * 1000.0f;
		// Forced overlay (headless): re-assert every frame so nothing toggles it
		// off mid-capture; otherwise the live F3 toggle behaves normally.
		if(gstate.force_debug_overlay)
			gstate.debug_overlay = true;
		else if(input_pressed(IB_DEBUG_OVERLAY))
			gstate.debug_overlay = !gstate.debug_overlay;

		/* Menus can omit the gameplay HUD. Forced capture overlays always
		 * identify the artifact, including transient split/recovery frames.
		 * Keep the stamp in the unused bottom margin. */
		/* A split local pause hides that viewport's gameplay HUD too (the
		 * approved composition), so the stamp must also survive both players
		 * pausing (Astra ui3). */
		if(gstate.debug_overlay && gstate.world_loaded
		   && (gstate.force_debug_overlay
			   || gstate.players[0].current_screen != &screen_ingame
			   || local_menu_host_active(&gstate.players[0])
			   || gstate.players[0].menu_host.recovery.phase != CONTROLLER_RECOVERY_NONE
			   || local_pause_active(&gstate.players[0].pause)
			   || (view_count > 1 && (local_menu_host_active(&gstate.players[1])
					|| gstate.players[1].menu_host.recovery.phase != CONTROLLER_RECOVERY_NONE
									  || local_pause_active(&gstate.players[1].pause)))))
			gutil_text(4, gfx_height() - 10, "UI " CW_BUILD_ID, 8, true);

		// LOD reach dial: in FORCED-overlay mode (config "debug.overlay":"force",
		// the capture/tuning rig) the hotbar +/- buttons retune the far-ring
		// full-detail radius live (overlay row 12 shows it; hotbar scroll is
		// suppressed -- see screen_ingame). Crank it and watch the fps to find
		// the real-Wii GPU ceiling Dolphin can't show. Gated on FORCE, not on
		// the plain F3 toggle: +/- is the HOTBAR control (the core Wiimote
		// gameplay binding), and play-testing always runs with the HUD on --
		// the old gate made hotbar scroll appear broken in every test session.
		if(gstate.force_debug_overlay) {
			int r = world_lod_near_radius();
			if(input_pressed(IB_SCROLL_RIGHT) && r < MAX_VIEW_DISTANCE)
				world_lod_set_near_radius(r + 1);
			else if(input_pressed(IB_SCROLL_LEFT) && r > 0)
				world_lod_set_near_radius(r - 1);
		}

		if(input_pressed(IB_SCREENSHOT)) {
			size_t width, height;
			gfx_copy_framebuffer(NULL, &width, &height);

			void* image = malloc(width * height * 4);

			if(image) {
				gfx_copy_framebuffer(image, &width, &height);

				char name[64];
				snprintf(name, sizeof(name), "%ld.png", (long)time(NULL));

				lodepng_encode32_file(name, image, width, height);
				free(image);
			}
		}

		// Headless auto-capture: once a world is loaded, dump the just-rendered EFB
		// (this runs after render2D, so the forced HUD is included) to a numbered SD
		// file every AUTO_SCREENSHOT_FRAMES frames. The file cap is CONFIG-driven
		// ("debug.auto_screenshot_max", 0 = unlimited, the default) so the
		// pre-existing indefinite-capture workflows (redstone_drop, piston_anim)
		// are unchanged, while a bounded prover run (piston_blast) sets a finite
		// cap to keep its EARLY standing-rig frames alongside the post-event
		// frames without ever filling the SD. Gated on world_loaded so it never
		// captures a blank world-select frame. Reuses the proven screenshot path.
		/* "debug.auto_screenshot_menus":"on" lifts the world_loaded gate so a
		 * headless proof rig can capture MENU screens too (mp-s4: the
		 * disconnect/error screen -- the OS-side window copy reads blank for
		 * a flip-model GL client area, so the engine's own framebuffer
		 * readback is the only reliable PC capture). Default off: the
		 * world_loaded gate's no-blank-world-select-frame rationale stands
		 * for every existing workflow. */
		static int auto_ss_menus = -1;
		if(auto_ss_menus < 0)
			auto_ss_menus
				= strcmp(config_read_string(&gstate.config_user,
											"debug.auto_screenshot_menus",
											"off"),
						 "on")
					== 0
				|| config_read_int(&gstate.config_user,
								   "debug.auto_screenshot_menus", 0)
					== 1; /* --cw-set form, see gstate.auto_screenshot */
		if(gstate.auto_screenshot && (gstate.world_loaded || auto_ss_menus)) {
			/* "debug.auto_screenshot_period": frames between captures
			 * (default 60 = once a second at 60 fps); --cw-set-able so a
			 * console run can trade cadence for fetch time (2026-09-11). */
			static int auto_ss_period = -1;
			if(auto_ss_period < 0) {
				auto_ss_period = config_read_int(
					&gstate.config_user, "debug.auto_screenshot_period", 60);
				if(auto_ss_period < 1)
					auto_ss_period = 1;
			}
			static unsigned auto_ss_frame = 0;
			static unsigned capture_seq = 0;
			static int auto_ss_max = -1; // -1 = config not read yet
			if(auto_ss_max < 0)
				auto_ss_max = config_read_int(&gstate.config_user,
											  "debug.auto_screenshot_max", 0);
			if((auto_ss_frame++ % (unsigned)auto_ss_period) == 0
			   && (auto_ss_max == 0 || capture_seq < (unsigned)auto_ss_max)) {
#ifdef PLATFORM_WII
				/* Banded raw dump under sd:/apps/ (the stage server's fetch
				 * allow-list; the SD root, the game's CWD, is not reachable
				 * over the wire) -- no 1.2 MB transients, no lodepng on the
				 * console (2026-09-11, see gfx_write_framebuffer_raw). */
				char ssname[64];
				snprintf(ssname, sizeof(ssname),
						 "sd:/apps/CavEX/ss_%06u.rgba", capture_seq++);
				gfx_write_framebuffer_raw(ssname);
#else
				size_t width, height;
				gfx_copy_framebuffer(NULL, &width, &height);
				void* image = malloc(width * height * 4);
				if(image) {
					gfx_copy_framebuffer(image, &width, &height);
					// Numbered stream (was a single overwritten "screenshot.png"): lets the
					// autonomous PC test harness read frames in ORDER to see motion across a
					// walk, with no read-while-writing race. Debug-only (auto_screenshot off
					// by default). ss_000000.png, ss_000001.png, ...
					char ssname[32];
					snprintf(ssname, sizeof(ssname), "ss_%06u.png", capture_seq++);
					lodepng_encode32_file(ssname, image, width, height);
					free(image);
				}
#endif
			}
		}

		remote_capture_frame();
		/* D1: a ride-action edge raised after this frame's tick section
		 * (scripted host buttons are applied mid-frame) is captured here,
		 * before input_poll retires it; the begin capture's edge is never
		 * taken twice. entity_local_player.h. */
		if(!handoff_input_polled) entity_local_player_ride_action_frame_end();
		watchdog_phase_main(WDM_GFX_FINISH);
		ptime_t profile_input = hw_run.enabled ? time_get() : (ptime_t){0};
		struct g9_lease_token lease_turn = thread_g9_handoff_enabled()
			? (struct g9_lease_token){0} : thread_g9_lease_service_begin();
		ptime_t lease_service_start = lease.enabled ? time_get() : (ptime_t){0};
		if(!handoff_input_polled) g9_frame_input_poll(split_experimental);
		if(thread_g9_handoff_enabled()) {
			/* A request after the regular audio service still requires real
			 * refill/listener service; never credit a pre-request sample. */
			g9_handoff_audio_service();
			clin_g9_handoff_poll(false);
		} else g9_lease_main_service(lease_turn, lease_service_start, (uint32_t)lease.service_us);
		ptime_t profile_finish = hw_run.enabled ? time_get() : (ptime_t){0};
		if(hw_run.pool_selftest) /* after every pass of this frame */
			pool_selftest_frame_end((uint32_t)time_diff_ms(hw_started, time_get()));
		g9_probe_main_mark(8, true, 0, 0, 0);
		gfx_finish(true);
#ifdef PLATFORM_PC
		SERVICE_OBSERVER(service_observer_proxy());
#endif
		g9_probe_main_mark(8, false, 1, 0, 0);
		g9_probe_main_frame(false);
		if(hw_run.enabled) {
			frame_input_ms = handoff_input_polled ? handoff_input_us / 1000.0f
				: time_diff_s(profile_input, profile_finish) * 1000.0f;
			frame_finish_ms = time_diff_s(profile_finish, time_get()) * 1000.0f;
		}
	}
	SERVICE_OBSERVER(service_observer_state(SO_SHUTDOWN));

#ifdef CAVEWII_MP_CLIENT
	// Mirrors the local-provider join below: quiesce/generation-fence the
	// remote workers before the platform layers tear down [plan 8's teardown
	// risk register]. Harmless no-op if server_local is the active provider
	// (mp_provider_remote false, remote already REMOTE_DISCONNECTED).
	server_remote_cancel(mp_session_remote());
	client_session_achievements_remote(&gstate.players[0], 0, false);
	mp_session_destroy();
#endif
#ifdef PLATFORM_WII
 watchdog_context_observer_set(NULL);
#endif
 remote_capture_shutdown();
 tps_observer_close(false); /* early quit is partial; close before joining */
	// Clean quit: join the server thread FIRST, so no region/level write is
	// in flight while the platform layers tear down and the process exits
	// (the amahpour-shared shutdown race -- on Wii this was a save-corruption
	// window on exit-to-loader). Harmless no-op if server_remote was the
	// active provider at quit time (server_local_shutdown is idempotent
	// pre-start too).
	// Breadcrumbs (armed runs only): if a wedge ever lands INSIDE this
	// teardown, sd:/cw_boot_trace.txt names the last step reached instead of
	// leaving "auto-exit never fired" indistinguishable from "shutdown hung".
	//
	// WATCHDOG COVERAGE (Batch E item 1, 2026-08-23 HBC-direct RCA finding
	// (d)): the render loop's last phase stamp was WDM_GFX_FINISH (set once
	// per frame, above) and nothing updated it or the heartbeat again for the
	// rest of the process -- so a shutdown step that legitimately took >3s
	// (a large region flush, a slow SD close) painted a stale "hung in
	// GFX_FINISH" panic that named the wrong subsystem. Each step below now
	// gets its own phase tag AND a heartbeat immediately before it starts,
	// so (a) a genuine >3s stall inside one of these three calls is reported
	// under its OWN honest label instead of GFX_FINISH's, and (b) a shutdown
	// that is merely slow -- not hung -- keeps refreshing the liveness clock
	// and never fires the panic at all. The 3s fire-once threshold
	// (wii/watchdog.c) is unchanged; this is label + liveness coverage only.
	hw_boot_trace_write("shutdown_begin", &hw_run, "", false);
	watchdog_phase_main(WDM_SHUTDOWN_SERVER);
	watchdog_heartbeat();
	server_local_shutdown(&server);
	server_local_set_anchor_sep(&server, 0);
	achievements_tick();
 if(!screen_stats_save_quit(hw_run.auto_exit)) {
  fprintf(stderr, "[achievements] quit canceled; retained progress; returning to title\n");
  gstate.quit = false;
#ifdef CAVEWII_MP_CLIENT
  mp_session_init(); /* title may connect again after the canceled quit */
#endif
  remote_capture_init(config_read_int(&gstate.config_user,
   "debug.remote_capture", 0) == 1, CW_BUILD_ID);
  if(!gstate.remote_session) {
   if(!server_local_start(&server))
    fprintf(stderr, "[achievements] local provider restart failed after canceled quit\n");
   if(server.player.has_pos) clin_send_unload_world();
  } else clin_force_world_reset();
  screen_set(&screen_title);
  last_frame = time_get();
  goto resume_main_loop;
 }
	hw_boot_trace_write("shutdown_server_done", &hw_run, "", false);
	// C1 (candidate, hardware-unconfirmed, 2026-09-02): the chunk mesher worker
	// had no stop API and ran forever past this point. It consumes chunk data
	// the world owns, so it is stopped HERE -- right after the server, before
	// anything downstream could free or tear down chunks (nothing later in
	// this function does today; the ordering is the contract for whatever is
	// added later). Left tagged WDM_SHUTDOWN_SERVER: this span and the server
	// join just above are both "stop the threads that still touch the world",
	// and a stall in either is diagnosed from the same neighborhood -- adding
	// a dedicated phase for a call site with no prior watchdog coverage at
	// all (there was nothing to join before this commit) is deferred rather
	// than growing the enum for this surgical fix.
	chunk_mesher_shutdown();
	bool tps_evidence_ok = !hw_run.tps_cpu || tps_observer_stop();
	if(hw_run.tps_cpu) {
		/* Journaled-publication census for the pace work (round 2, 2026-09-07;
		 * round 3 added the chest-sort preserve counters, the PRESERVE
		 * microseconds the legacy heartbeat cannot carry (Wii only), and the
		 * journal handle open/destroy counts -- the directory-transaction
		 * cost that stayed per-publication is now per-HANDLE, and these two
		 * are what prove it). Printed only for an armed measurement run; see
		 * region_archive_journal_stats for the slot meanings. */
		uint32_t rj[24];
		region_archive_journal_stats(rj);
		/* Slots 19/20 are accumulated ONLY under the Wii microsecond profile
		 * (region_archive.c gates them on s_region_profile), so printing them
		 * as numbers when it is off reports a real zero for an unmeasured
		 * quantity. Gate the print the same way the counter is gated. */
		char io_us[64];
		if(region_archive_work_profile_enabled())
			snprintf(io_us, sizeof(io_us), "io_busy_us=%u io_max_us=%u",
					 rj[19], rj[20]);
		else
			snprintf(io_us, sizeof(io_us),
					 "io_busy_us=off io_max_us=off");
		fprintf(stderr,
				"[region-journal] publishes=%u chunks=%u sectors=%u "
				"journals=%u fsyncs=%u | batch_end=%u sync=%u destroy=%u "
				"immediate=%u overflow=%u | preserve_calls=%u short=%u "
				"preserve_us=%u | journal_opens=%u journal_destroys=%u"
				" | read_backs=%u canon_skips=%u canon_writes=%u "
				"payload_bytes=%u | %s io_async=%u "
				"io_waits=%u io_abandoned=%u\n",
				rj[0], rj[1], rj[2], rj[3], rj[4], rj[5], rj[6], rj[7], rj[8],
				rj[9], rj[10], rj[11], rj[12], rj[13], rj[14], rj[15], rj[16],
				rj[17], rj[18], io_us, rj[21], rj[22], rj[23]);
	}
	if(!tps_evidence_ok)
		fprintf(stderr, "[tps] incomplete CPU evidence\n");
	if(hw_run.column_census && !column_observer_stop())
		fprintf(stderr, "[column-observer] incomplete evidence\n");
	hw_boot_trace_write("shutdown_mesher_done", &hw_run, "", false);
	watchdog_phase_main(WDM_SHUTDOWN_AUDIO);
	watchdog_heartbeat();
	// S3 first: stops the voice, signals + joins the decode worker thread
	// and closes any open file, BEFORE audio_shutdown() below tears down
	// the shared device both subsystems' voices live on.
	audio_music_shutdown();
	/* asyncsnd-0926: a pass-C loader still filling the arena is cancelled and
	 * joined before the device (and the arena's readers) go away. No-op with
	 * the opt-in off or the scan complete. */
	audio_registry_async_join();
	audio_shutdown();
	SERVICE_OBSERVER(service_observer_request(true));
	achievement_notice_shutdown();
	hw_boot_trace_write("shutdown_audio_done", &hw_run, "", false);
#ifdef PLATFORM_WII
	// C1 (candidate, hardware-unconfirmed): the background network bring-up
	// worker (net_bringup.c) also had no shutdown-and-join operation -- only
	// an advisory cancel sampled between DHCP attempts, up to ~25s late.
	// Cancelled + bounded-wait-joined (or, past the bound, honestly
	// abandoned) here, before telemetry tears down -- same
	// "no dedicated phase for a previously-uncovered call site" reasoning as
	// the mesher above; a stall here still reports under WDM_SHUTDOWN_AUDIO's
	// neighborhood rather than the silent no-coverage this call site had
	// before this commit.
	net_bringup_shutdown();
	hw_boot_trace_write("shutdown_net_bringup_done", &hw_run, "", false);
#endif
	watchdog_phase_main(WDM_SHUTDOWN_TELEMETRY);
	watchdog_heartbeat();
#ifndef PLATFORM_WII
	/* Preserve the run ID until the final synchronous PC observer drain. */
	settle_observer_stop(hw_telemetry_emit_observer);
#endif
	hw_telemetry_shutdown();
#ifdef PLATFORM_WII
	/* Bracketed because this stop drains in-flight observer callbacks on
	 * worker threads: a trace that ends at the begin marker names the drain,
	 * not the telemetry shutdown before it. */
	hw_boot_trace_write("shutdown_settle_stop_begin", &hw_run, "", false);
	settle_observer_stop(hw_telemetry_emit_observer);
	hw_boot_trace_write("shutdown_settle_stop_end", &hw_run, "", false);
#endif
	hw_boot_trace_write("shutdown_telemetry_done", &hw_run, "", false);
#ifdef PLATFORM_WII
	// Exit-wedge candidate (HBC-direct dossier residual (d) + 2026-08-24
	// lessons review): NOTHING in this codebase ever called WPAD_Shutdown, so
	// a boot whose Wiimote attached returns to the loader with the BT/OH1
	// device still DMA-active into MEM -- exactly the class of live-hardware
	// handoff the resident HBC stub cannot survive. Chainload exits never had
	// an attached remote (autopilot masked it), so this only ever bites the
	// HBC-direct human path. Torn down AFTER telemetry (the heartbeat sender
	// does not touch WPAD) and immediately before the loader return.
	watchdog_phase_main(WDM_SHUTDOWN_WPAD);
	watchdog_heartbeat();
	input_wpad_shutdown0();
	hw_boot_trace_write("shutdown_wpad_done", &hw_run, "", false);
#endif
	// C1 (candidate, hardware-unconfirmed, 2026-09-02): the watchdog's own
	// prio-100 detector thread had no stop API and stayed armed straight
	// through the loader handoff -- after the LAST heartbeat above, it can
	// still fire, draw a panic band over GX, and write a report against a
	// process that is exiting cleanly. Stopped LAST, immediately before the
	// pre-loader-return breadcrumb, so every shutdown phase above (including
	// this one) stays covered by a live watchdog until the moment it is
	// asked to stop; the WDM_SHUTDOWN_WATCHDOG tag names a stall inside that
	// stop-and-join itself rather than letting it inherit WPAD's label.
	watchdog_phase_main(WDM_SHUTDOWN_WATCHDOG);
	watchdog_heartbeat();
	watchdog_shutdown();
	hw_boot_trace_write("shutdown_watchdog_done", &hw_run, "", false);
	// PRE-LOADER-RETURN BREADCRUMB (Batch E item 3): the literal last
	// statement before the devkitPPC return-to-loader. The 2026-08-23
	// HBC-direct dossier's residual (d) is that ALL FOUR shutdown
	// breadcrumbs above already land on a failing boot, yet HBC never
	// receives control back -- i.e. the hang (if any) is AFTER this
	// function returns, in the exit-stub/loader-return handoff itself, not
	// in anything main() still controls. This line is what lets the next
	// instrumented HBC-direct run tell "stuck before return" (this line
	// missing from cw_boot_trace.txt) apart from "stuck in the loader
	// handoff" (this line present, but HBC still never regains control).
	hw_boot_trace_write("pre_loader_return", &hw_run, "", false);
	return tps_evidence_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif /* CAVEWII_F1_DISPATCH_ONLY */
