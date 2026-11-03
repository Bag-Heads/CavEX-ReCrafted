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

#ifndef GAME_STATE_H
#define GAME_STATE_H

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "../config.h"
#include "../entity/entity.h"
#include "../item/window_container.h"
#include "../platform/time.h"
#include "../world.h"
#include "autopilot_keepup.h"
#include "autopilot_unstick.h"
#include "camera.h"
#include "client_session.h"
#include "gui/screen.h"

/* User-visible product name: the F3 overlay's first line and the PC window
 * title. Renamed from "fCavEX" 2026-08-04 at the maintainer's request — this
 * fork is CaveWii. Attribution is unaffected and deliberately left alone: the
 * per-file licence headers still read "part of CaveWii_v2 (a CavEX/fCavEX
 * derivative)", and meta.xml still names the full upstream chain
 * (jilleb/fCavEX <- ByteBit/fCavEX <- yyy257 <- ByteBit/xtreme8000 CavEX).
 * That is what the GPL requires preserved; the display name is not. */
#define GAME_NAME "CaveWii"
/* Player-facing product name (maintainer 2026-10-23). GAME_NAME stays
 * "CaveWii": the F3 header "CaveWii (impl. B1.7.3) <id>" is the DOL marker
 * tools/stage_wii_dol.ps1 checks. */
#define GAME_DISPLAY_NAME "CavEX"
#define VERSION_MAJOR 0
#define VERSION_MINOR 3
#define VERSION_PATCH 0
#define VERSION_FORK  2

#include "chat_ring.h"

struct game_state {
	struct chat_ring_state chat_ring;
	// Beta EntityPlayer sleep state, mirrored from CRPC_SLEEP_STATE. The bool
	// gates client input/physics; the timer drives the GuiIngame fade. They cannot
	// be conflated because timer 100 also occurs after sleeping becomes false.
	sig_atomic_t quit;
	struct random_gen rand_src;
	struct config config_user;
	struct {
		float dt, fps;
		float dt_gpu, dt_vsync;
		size_t chunks_rendered;
		// Per-phase main-thread CPU cost in ms, for the debug-overlay
		// profiler. The Wii is single-core, so these phases plus the
		// mesher/server worker threads all share one core — this is where
		// frame time actually goes once the GPU isn't the bottleneck.
		float ms_bfs, ms_light, ms_lod, ms_build, ms_recv, ms_render, ms_tick;
		float ms_bfs_max, ms_light_max, ms_lod_max, ms_build_max;
		uint32_t wd_hb_gap_max_ms;
	} stats;
	struct {
		float fov;
		float render_distance;
		float fog_distance;
		/* Beta GameSettings.viewBobbing, default true ("bobView")
		 * [740c583 GameSettings:21,331]; ours = "render.view_bobbing". */
		bool view_bobbing;
		/* TextureCompassFX/TextureWatchFX item-needle animation
		 * (platform/texture.c's item_needle_fx_tick). Default ON on PC and
		 * Wii for faithful animation; opt out with gfx.item_needle_anim=0. */
		bool item_needle_anim;
	} config;
	struct client_session players[CLIENT_LOCAL_PLAYERS];
	struct world world;
	/* debug.minecart_sound: the non-Beta cart rolling sound (default ON, at the
	 * maintainer's request). Set "off" to restore Beta 1.7.3's silent carts --
	 * required for any audio parity/oracle work. */
	bool config_minecart_sound;
	/* CLIENT half of creative mode (instant dig + the item picker in
	 * screen_ingame.c); the server half is server_local.creative_mode. Net-new
	 * addition, maintainer exception 2026-08-25 -- docs/creative_mode.md.
	 * PER-WORLD since 2026-09-01: armed from the loaded world's
	 * LEVEL_GAME_MODE tag when the client requests the load
	 * (screen_select_world.c), NOT from config -- the "gameplay.creative"
	 * switch is retired. Kept the `config_` name so the ~40 read sites do not
	 * churn; it means "the creative state for this session's world". */
	bool config_creative;
	/* gui.death_hold_respawn_ms / gui.death_hold_title_ms: deviation-#18
	 * controller adaptation -- the death screen's buttons fire on a HOLD of
	 * this duration (visible fill sweep) instead of a bare click, so a
	 * combat mash on the shared attack/GUI button cannot buy an accidental
	 * respawn or quit. 0 restores the faithful Beta single click. Clamped
	 * after parse in main.c. */
	int config_death_hold_respawn_ms;
	int config_death_hold_title_ms;
	dict_entity_t entities;
	uint64_t world_time;
	ptime_t world_time_start;
	/* Beta SMP Packet6 world spawn, retained for compass/map/respawn consumers. */
	w_coord_t world_spawn_x, world_spawn_y, world_spawn_z;
	// Beta 1.7.3 weather (server-authoritative; synced every non-loading tick
	// via CRPC_WEATHER_SET, mirroring the world_time/world_time_start
	// pattern above). raining/thundering are the current flags; the
	// strengths are the CURRENT server-interpolated values (see
	// CRPC_WEATHER_SET's own doc comment for why the wire only ever carries
	// ONE value per tick, not a prev/cur pair). Sim lands fully; render is
	// the render-slice (RENDER_DISPATCH.md).
	bool weather_raining;
	bool weather_thundering;
	float weather_rain_strength;
	float weather_thunder_strength;
	// render-slice: EntityRenderer.renderRainSnow's `getRainStrength(partial)`
	// [740c583: World.getRainStrength] genuinely needs BOTH the previous and
	// current per-tick values to interpolate by the render's own partial-tick
	// fraction (main.c's tick_delta) -- CRPC_WEATHER_SET's doc comment above
	// explains why the wire itself only carries the current value once per
	// tick (world_time uses the identical one-value-per-tick pattern). These
	// two fields are the client-local shadow that supplies the missing
	// "previous" half: client_interface.c's CRPC_WEATHER_SET handler copies
	// the OLD weather_rain_strength/weather_thunder_strength here BEFORE
	// overwriting them with the new tick's values, mirroring Beta's own
	// World.updateWeather doing `prevRainingStrength = rainingStrength;`
	// immediately before stepping `rainingStrength` -- same shadow-then-step
	// rhythm, just split across the RPC boundary instead of one in-process
	// field. This is a NEW client-side field, not a wire-protocol change (no
	// simulation file touched) -- see RENDER_SLICE_NOTES.md.
	float weather_prev_rain_strength;
	float weather_prev_thunder_strength;
	// [740c583: EntityRenderer.rendererUpdateCount] Beta increments this once
	// per CLIENT TICK -- CONFIRMED via Minecraft.runTick(), which calls
	// entityRenderer.updateRenderer() (rendererUpdateCount++ then
	// addRainParticles()) from INSIDE the same 20 Hz fixed-timestep
	// accumulator loop (Minecraft.run()'s `for(...elapsedTicks...)
	// runTick()`) that this engine's main.c catch-up loop mirrors -- not a
	// per-frame call. int32_t (not uint32_t): Beta's field is a genuine Java
	// `int`, and `(float)rendererUpdateCount` after a hypothetical wrap past
	// 2^31 would differ between the two representations -- astronomically
	// unlikely in a real session, but free to get exactly right. Both
	// renderRainSnow's snow-drift phase and addRainParticles' own RNG seed
	// read it. Advanced by client_add_rain_particles (render_weather.c),
	// called from main.c's existing 20 Hz catch-up loop alongside
	// particle_update/client_display_tick.
	int32_t precip_tick_count;
	// [740c583: EntityRenderer.rainSoundCounter] gates the "ambient.weather.
	// rain" splash sound in addRainParticles -- a plain per-tick counter, NOT
	// reset on world load/reconnect in Beta either (EntityRenderer is a
	// singleton for the client's lifetime).
	int32_t rain_sound_counter;
	// render-slice (RENDER_DISPATCH.md "THE KEY RULING"): Beta's own perf
	// lever for renderRainSnow -- radius 5 (default) or 10 when
	// gameSettings.fancyGraphics. Both values are Beta-shipped; this is NOT a
	// T2 deviation. Config "render.fancy_graphics" ("on"/"off", default
	// "off" = radius 5), read once at startup in main.c like force_debug_overlay.
	bool fancy_graphics;
	bool world_loaded;
	// Reversible pre-commit loading screen for portal travel / Nether death.
	// The old world remains fully owned until CRPC_WORLD_RESET commits teardown.
	bool world_switch_pending;
	/* Single writer: CRPC_BEGIN_WORLD_SWITCH in clin_process_observed. */
#define CAVEWII_WORLD_SWITCH_BEGIN_SEQ 1
	uint32_t world_switch_begin_seq;
	// Terminal client install failure reported after the bounded center-section
	// restream protocol is exhausted. The authoritative input barrier remains
	// armed; the load screen replaces its progress UI with a restart-required
	// message instead of looking like an ordinary infinite load.
	bool world_load_failed;
	/* G6 L1: the server's stated reason for a REFUSED load, shown on the
	 * failure card; empty = the generic terrain-memory line. */
	char world_load_failed_reason[64];
	// True while the REMOTE (multiplayer) provider is the active server
	// interface; set/cleared by main.c at provider swap. Lets core client
	// code branch on session kind without a layering edge into mp_session
	// (take-51 pre-world position echo is the first consumer).
	bool remote_session;
	/* Chat composer first-use hint (docs/mp_chat_auth_research_2026-09-11.md,
	 * decision item 4): drawn from the moment a remote world is playable until
	 * the composer is first opened this session. Reset with the session. */
	bool chat_hint_done;
	// Remote "Downloading terrain" hold, armed when a server position
	// correction lands the player in a not-yet-installed chunk (same-dim
	// respawn is the field case: mc173 teleports to spawn immediately, the Wii
	// installs the spawn chunks seconds later). Beta's client only holds on
	// WorldClient recreation (login/dim change) and trusts the vanilla
	// server's timing for everything else [740c583
	// NetClientHandler.handleRespawn; GuiDownloadTerrain;
	// NetClientHandler.handleFlying: doneLoadingTerrain]; mc173 gives no such
	// residency guarantee, so the SAME hold primitive is extended to this one
	// uncovered case (documented adaptation, not a rules change). While set:
	// client entity physics is inert (predicate below) and screen_load_world
	// stays up; released by clin_update once the chunk under the player is
	// installed, cleared unconditionally by CRPC_WORLD_RESET.
	bool remote_terrain_hold;
	// Cross-boundary handoff to the MP connecting/disconnected screen
	// (task_db613f13). screen_multiplayer_connect.c is a deliberately
	// ISOLATED translation unit -- CMakeLists.txt's CAVEX_MP_SOURCES comment
	// keeps it (and the socket provider it drags in) out of the shared
	// engine object pool every PC unit test links, so shared-pool code like
	// screen_load_world.c cannot call
	// screen_multiplayer_connect_show_disconnected() directly without
	// breaking every other test's link. These two fields are that function's
	// actual state, moved here so both callers -- main.c's non-graceful
	// disconnect branch (via the function, unchanged) and
	// screen_load_world.c's remote stall/give-up escape hatch (via these
	// fields + the existing screen_multiplayer_connect pointer, screen.h)
	// -- write the SAME state, and screen_multiplayer_connect.c's mpc_reset()
	// is the single reader. Never read/written by singleplayer code.
	bool remote_disconnect_requested;
	char remote_disconnect_reason[256];
	bool paused;
	bool debug_overlay; // F3-style perf HUD; init from config "debug.overlay",
						// toggled live via IB_DEBUG_OVERLAY
	// "debug.overlay":"force" -> re-assert debug_overlay=true EVERY frame so a
	// headless capture can't lose the HUD to a stray toggle/reset (the F3 toggle
	// is suppressed while forced). Normal "on"/"off" leave the live toggle intact.
	bool force_debug_overlay;
	// "debug.auto_screenshot":"on" -> the render loop periodically dumps the GX
	// framebuffer to a fixed SD file (screenshot.png) once a world is loaded, so a
	// headless harness can read the rendered frame (incl. the forced HUD) with no
	// input injection. Debug-only.
	bool auto_screenshot;
	// Debug ("debug.water_demo") showcase: floor the air bar so the Beta bubble
	// HUD stays visible for screenshots while the player is held underwater.
	bool water_demo;
	// Autonomous test autopilot (debug.autopilot=on; default off). Dormant globals the
	// input/camera hooks read so a headless PC capture run drives the real player physics
	// (forced movement + authored look) with no human input. See game/autopilot.h.
	struct {
		bool active;
		// 0 = walk (scripted survey, curving yaw drift); 1 = aim (hold exact
		// config look, force nothing); 2 = keepup (HW_RUN_PROFILE_KEEPUP_
		// WALK_6M -- fixed heading, no drift, bounded deterministic sidestep
		// only; see autopilot_keepup.h for why mode 0's drift is itself the
		// source of far_walk_6m's run-to-run displacement variance).
		int mode;
		int tick;
		float yaw, pitch; // radians; yaw=camera rx, pitch=camera ry (GLM_PI/2 = level)
		bool place_enabled; // aim mode: inject deterministic block-place pulses (no input)
		int place_period;   // ticks between place pulses (<=0 = a single one-shot place)
		int pending_place;  // place pulses awaiting consumption by screen_ingame
		// Last consumed pulse (rig diagnostics, 2026-09-11): main.c writes it
		// to the boot trace so a menus_dwell run names what each aimed
		// right-click actually hit.
		int pulse_count;
		int pulse_x, pulse_y, pulse_z, pulse_block;
		// Opt-in walk-mode stuck-detector (autopilot_unstick.h). armed ONLY
		// for HW_RUN_PROFILE_MP_LAN_WALK_10M (main.c reads hw_run.autopilot_unstick
		// directly, NOT a "debug.*" config key -- see the header's comment).
		// Every other profile/config path leaves this a dead struct.
		struct autopilot_unstick_state unstick;
		// Keep-up-mode (mode==2) deterministic sidestep detector
		// (autopilot_keepup.h). Armed ONLY when mode==2 (main.c). Every
		// other mode leaves this a dead struct, exactly like unstick above.
		struct autopilot_keepup_state keepup;
		/* Anchor measurement route only: controller inputs for swimming and
		 * cliff braking. Collision/world rules remain the ordinary ones. */
		bool terrain_safe, water, brake, route_jump;
		struct autopilot_route_state route;
		// Death recovery (walk mode only, autopilot.c): consecutive ticks
		// screen_death has been the active screen while autopilot_tick() ran.
		// 0 whenever the death screen isn't up. Root cause this addresses: a
		// scripted walk can die (fall/drown/mob damage) exactly like a human
		// player; Beta's GuiGameOver clears local_player.capture_input, so a
		// dead walker's forced FORWARD/JUMP can never reach movement again --
		// the 2026-08-31 mp_lan_walk_10m spawn-pocket "immobility" was this:
		// a fall shortly after MP connect killed the player and nothing ever
		// pressed Respawn. autopilot_force_button uses this counter to force
		// IB_GUI_CLICK released-then-held on that screen, the same input
		// path a human's held-click uses (screen_death.c's own grace/latch/
		// hold state machine, unmodified).
		int death_screen_ticks;
	} autopilot;
};

extern struct game_state gstate;

/* Client entity physics owns the live gameplay world only after loading and
 * the FIFO input-handoff ACK gate are both complete. Cosmetic ticks may keep
 * running on the loading screen, but advancing the player here would diverge
 * from the frozen authoritative server body. */
static inline bool game_state_client_entities_active(
	const struct game_state* state) {
	return state && state->world_loaded && !state->world_switch_pending
		&& !state->remote_terrain_hold;
}

/* F3: centralized reset for the ride-intent and camera presentation state.
 * cam: the stored camera to fold the active presentation vector out of
 *   before clearing. May be NULL after a fresh-body handoff has already
 *   overwritten the camera with the replacement body's authority.
 * preserve_seq=true: target-destroy teardown inside the same input epoch.
 *   next_request_seq is monotonic across target destroy; resetting it would
 *   let a new intent for a different cart reuse seq1 and be coalesced as an
 *   already-represented retry (criterion #13). The active presentation
 *   vector is folded out of the stored camera before clearing, so target
 *   destroy between attaches does not leave a false wire position.
 * preserve_seq=false: world/body-generation reset (clin_init, CRPC_WORLD_RESET,
 *   death handoff, terminal load failure). Full zero. The camera fold is
 *   still performed so a death/world reset between attaches does not bake
 *   a stale presentation offset into the stored camera. */
static inline void minecart_ride_client_state_reset(
	struct minecart_ride_client_state* s, struct camera* cam,
	bool preserve_seq) {
	if(cam && s->camera_correction_active) {
		cam->x -= s->camera_correction[0];
		cam->y -= s->camera_correction[1];
		cam->z -= s->camera_correction[2];
	}
	uint32_t saved_seq = s->next_request_seq;
	*s = (struct minecart_ride_client_state) {0};
	if(preserve_seq)
		s->next_request_seq = saved_seq;
}


#endif
