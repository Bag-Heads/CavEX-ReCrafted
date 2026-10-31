/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

#include <math.h>
#include <stdio.h>

#include "../../graphics/gui_util.h"
#include "../../graphics/gfx_settings.h"
#include "../../network/client_interface.h"
#include "../../network/client_world_payload.h" /* B2: player 0's payload */
#include "../../network/server_interface.h"
#include "../../network/level_archive.h"
#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/input_owner.h"
#include "../../platform/time.h"
#include "../game_state.h"
#include "screen_load_world.h"
#include "frontend_presentation.h"
#include "loading_indicator.h"
#include "loading_tips.h"

/* Load-world hold escape hatch (task_db613f13, 2026-08-31). REMOTE sessions
 * only: this screen has TWO legitimate triggers that both park it
 * indefinitely if a live connection stalls (TCP still open, peer just stops
 * sending) instead of dying outright --
 *   1. CRPC_BEGIN_WORLD_SWITCH (dimension switch / portal / respawn): the
 *      old world stays valid, gstate.world_switch_pending gates the return
 *      to gameplay.
 *   2. gstate.remote_terrain_hold (client_interface.c's CRPC_PLAYER_POS
 *      POSITION_ONLY branch, ~line 1459): a same-dim respawn/teleport lands
 *      the player in a chunk the client hasn't installed yet; normally
 *      released by clin_update() once the chunk streams in.
 * Both are unconditionally cleared by CRPC_WORLD_RESET (client_interface.c
 * ~line 1581) -- exactly what a genuinely DEAD transport gets via main.c's
 * non-graceful disconnect branch (main.c ~1504-1509). That branch only
 * fires when the transport itself notices the death; a silent stall never
 * trips it, so without an escape hatch here "Building terrain" never ends.
 *
 * Singleplayer is deliberately untouched: every new static/branch below is
 * gated on gstate.remote_session, which is only ever true inside main.c's
 * CAVEWII_MP_CLIENT remote-provider block (main.c ~1386, set alongside
 * mp_provider_remote). SP world loads legitimately take a while and have
 * no disconnect concept at all -- when remote_session is false this file's
 * behaviour is byte-identical to before. */

// No world_loaded_chunks() progress for this long (while remote) -- auto-
// fire the escape hatch even with zero player input.
#define LWORLD_REMOTE_STALL_TIMEOUT_S 30.0f
// Time spent on this screen (while remote) before the manual give-up
// hint/hold becomes available -- short enough to matter to a stuck player,
// long enough that an ordinary multi-second dimension load never shows it.
#define LWORLD_REMOTE_HOLD_HINT_S 10.0f
// Held-input duration to confirm the manual give-up once the hint is up
// (screen_death.c's hold-to-confirm idiom, carve-out #2: an accidental tap
// must not disconnect a session that is merely loading slowly).
#define LWORLD_REMOTE_HOLD_CONFIRM_S 2.0f
#define LWORLD_REMOTE_STALL_REASON "Connection stalled"
/* REL-MP-LOADHANG: the hard ceiling's disconnect reason (no installed,
 * meshed core under the player after LWORLD_REMOTE_HARD_CEILING_S). */
#define LWORLD_REMOTE_CEILING_REASON "Terrain never arrived"

static struct lworld_remote_progress s_remote_progress;
/* REL-MP-LOADHANG: set by the ceiling, consumed by
 * lworld_prepare_presentation (the one place that may admit). */
static bool s_core_force_admit;
static uint32_t s_core_force_admits;
static float s_remote_time_on_screen;
static bool s_remote_hold_release_seen; // fresh-press latch, screen_death.c's click_armed idiom
static float s_remote_hold_s;

/* Near-ring readiness gate (2026-09-03 maintainer ruling): "Unacceptable...
 * The building-terrain screen was too fast." gstate.world_loaded alone (the
 * pre-existing condition below) fires as soon as the SERVER has placed the
 * player -- it says nothing about whether the CLIENT has actually installed
 * and MESHED the ring around them, which is exactly the gap the field report
 * named ("chunks crawl in after the loading screen, x-ray holes... while
 * walking"). This tracks wall-clock time on THIS screen (SP and remote
 * alike, unlike s_remote_time_on_screen above which is remote-escape-hatch-
 * only) so the gate below has a bounded fallback: a genuinely stuck near
 * ring must not hang the loading screen forever, but 30s is long enough that
 * an ordinary cold spawn never hits it. */
#define LWORLD_NEAR_READY_TIMEOUT_S 30.0f
static float s_time_on_screen;
static bool s_presentation_pending;
static bool s_presentation_prepared;
// The measured duration of the most recently COMPLETED loading screen (0
// before any). Read by main.c's heartbeat (hw_telemetry's load_screen_ms)
// via lworld_load_screen_ms_result() below; stays at its last value once set
// until the NEXT screen_lworld_reset, so a heartbeat sampled a few seconds
// after the transition still sees it.
static uint32_t s_load_screen_ms_result;
/* REL-MP-COREFIRST: when (s_time_on_screen, ms, >= 1) the remote safe core
 * was first fully installed / installed and meshed this episode; 0 = not
 * yet. Latched by lworld_prepare_presentation's remote branch, reset with
 * the episode, kept after admission for the heartbeat. Telemetry only. */
static uint32_t s_core_resident_ms;
static uint32_t s_core_meshed_ms;
static ptime_t s_tip_epoch;
static bool s_tip_clock_started;
static char s_world_name[256];
static struct lworld_loading_progress s_local_progress[LOADING_TIPS_OWNERS] = {
	{.percent = -1}, {.percent = -1}
};
/* stale-99-1030 (round2 bug 06, 9:40 and 12:57): a new load opened at
 * "Loading level 99%" with a full bar and then fell to 0%. The census counts
 * the chunks around the camera, and until this load's world reset arrives
 * the client still holds the PREVIOUS scene there -- a complete ring that is
 * not this world. A load's census is only this world's once the reset has
 * started the switch (gstate.world_switch_pending), so the bar reads 0 until
 * the episode has seen that; readiness (100) is never held back. */
static bool s_episode_switch_seen;
/* Bounded fallback (review 10-30): the zeroing also ends once the ring's
 * centre leaves the chunk the episode started in (the new spawn) or after
 * LWORLD_STALE_RING_FRAMES readings, so a load that never shows the switch
 * flag still gets a real bar. */
#define LWORLD_STALE_RING_FRAMES 120
static bool s_episode_centre_set;
static w_coord_t s_episode_cx, s_episode_cz;
static int s_episode_readings;

void lworld_set_world_path(const char* path) {
	s_world_name[0] = 0;
	if(!path || !path[0]) return;
	const char* base = path;
	for(const char* c = path; *c; c++)
		if((*c == '/' || *c == '\\') && c[1]) base = c + 1;
	snprintf(s_world_name, sizeof s_world_name, "%s", base);
	string_t directory;
	string_init_set_str(directory, path);
	struct level_archive archive;
	if(level_archive_create(&archive, directory)) {
		char name[sizeof s_world_name];
		if(level_archive_read(&archive, LEVEL_NAME, name, sizeof name) && name[0])
			snprintf(s_world_name, sizeof s_world_name, "%s", name);
		level_archive_discard(&archive); /* presentation never saves metadata */
	}
	string_clear(directory);
}

void lworld_loading_progress_update(struct lworld_loading_progress* p,
	uint32_t measured_percent) {
	/* Keep the last measured fill through churn or an unavailable reading.
	 * A new presentation episode or conversion stage switch clears it. */
	if(measured_percent <= 100 && measured_percent >= p->high_water) {
		p->percent = (int)measured_percent;
		p->high_water = measured_percent;
	}
}

const char* lworld_world_name(void) {
	return s_world_name;
}

static void lworld_tip_context(unsigned owner, int width, bool split,
	bool blocked, struct loading_tip_context* out) {
	memset(out, 0, sizeof *out);
	out->width = width;
	out->split_pane = split;
	out->split_active = gstate.players[1].local_bound;
	out->creative = gstate.config_creative;
	out->peaceful = config_read_difficulty(&gstate.config_user) == 0;
	out->enabled = strcmp(config_read_string(&gstate.config_user,
		"gui.loading_tips", "on"), "off") != 0;
	out->visible = gutil_connection_notice_width(owner, split, false) == 0;
	out->blocked = blocked;
	memcpy(out->font, gutil_font_glyph_widths(), sizeof out->font);
	struct input_player_frame input;
	input_prompt_frame(owner, &input);
	out->gamecube = input.category == INPUT_CAT_GAMECUBE;
	static const enum input_button buttons[] = {IB_OPEN_INVENTORY,
		IB_ACTION1, IB_ACTION2, IB_SCROLL_LEFT, IB_SCROLL_RIGHT, IB_HOME};
	for(unsigned k = 0; k < LOADING_TIP_ACTIONS; k++) {
		int modifier, button;
		out->glyph[k] = input.glyphs[buttons[k]];
		if(input_glyph_cells(out->glyph[k], &modifier, &button)
		   && (texture_controls.data || (button < 42
			   && (modifier < 0 || modifier < 42) && texture_approved_controls.data)))
			out->glyph_width[k] = modifier < 0 ? 24 : 60;
		else out->glyph[k] = -1;
	}
	char profiles[2][CONFIG_PROFILE_ID_CAP];
	if(config_read_profiles(&gstate.config_user, profiles) == CONFIG_PROFILES_OK)
		loading_tips_set_identity(owner, profiles[owner]);
	else out->enabled = false;
}

/* REMOTE readiness state (M3, mp-split-0912/18) -- see the block comment on
 * lworld_remote_ready below for the full contract. Declared up here with the
 * other screen statics because lworld_reset_presentation (immediately after
 * this) has to clear the two presentation-side ones. */
static lworld_ready_census_fn s_census_fn;
static lworld_rx_bytes_fn s_rx_bytes_fn;
static struct server_remote* s_census_remote;
static uint32_t s_ready_timeout_admits;
static bool s_waiting_core;
static int s_waiting_core_have;
static int s_waiting_core_stage;
static float s_waiting_core_next_log_s;

/* Pure stall-detector (declared in screen.h so tests/test_mp_s0_env.c can
 * drive it headlessly with no gfx/input/screen dependency). The first tick
 * after reset only primes the baseline (no reading is "stalled" before a
 * baseline exists); every later tick either resets to 0 (the chunk count
 * advanced) or accumulates dt (it did not) and returns the running total. */
void lworld_stall_tracker_reset(struct lworld_stall_tracker* t) {
	t->last_chunks = 0;
	t->last_installs = 0;
	t->no_progress_s = 0.0f;
	t->primed = false;
}

float lworld_stall_tracker_tick(struct lworld_stall_tracker* t,
								size_t current_chunks, uint32_t installs,
								float dt) {
	if(!t->primed) {
		t->primed = true;
		t->last_chunks = current_chunks;
		t->last_installs = installs;
		t->no_progress_s = 0.0f;
		return 0.0f;
	}
	// CHIP A: either signal advancing is progress -- net cardinality (a
	// column left AND entered the ring) or a completed install (serial
	// wraparound is server-lifetime-implausible; a spurious reset on wrap is
	// still just one extra primed tick, never a false stall).
	if(current_chunks != t->last_chunks || installs != t->last_installs) {
		t->last_chunks = current_chunks;
		t->last_installs = installs;
		t->no_progress_s = 0.0f;
		return 0.0f;
	}
	t->no_progress_s += dt;
	return t->no_progress_s;
}

void lworld_remote_progress_reset(struct lworld_remote_progress* p) {
	memset(p, 0, sizeof(*p));
}

float lworld_remote_progress_tick(struct lworld_remote_progress* p,
								  const struct lworld_remote_progress_input* in,
								  float dt) {
	if(!p->primed) {
		lworld_remote_progress_reset(p);
		p->primed = true;
		p->installs = in->installs;
		p->chunks_high = in->chunks;
		p->missing_low = in->near_missing;
		p->mesh_low = in->near_mesh_pending;
		if(in->have_census) {
			p->census_primed = true;
			p->resident_high = in->census_resident;
			p->pending_low = in->census_pending;
		}
		return 0.0f;
	}
	bool progress = false;
	/* The install serial is monotone: any move is a completed install. */
	if(in->installs != p->installs) {
		p->installs = in->installs;
		progress = true;
	}
	/* Every count below only counts on a NEW BEST: falling back and
	 * recovering is churn (near-ring rebuilds, a re-bodied column), and a
	 * live-but-stuck session churns forever. */
	if(in->chunks > p->chunks_high) {
		p->chunks_high = in->chunks;
		progress = true;
	}
	bool new_work = progress;
	if(in->have_census && p->census_primed
	   && in->census_resident > p->resident_high)
		new_work = true;
	/* New work raised what is left to mesh / announce: measure the drain
	 * from here, not from a low set before that work existed. */
	if(new_work) {
		p->mesh_low = in->near_mesh_pending;
		if(in->have_census && p->census_primed)
			p->pending_low = in->census_pending;
	}
	if(in->near_missing < p->missing_low) {
		p->missing_low = in->near_missing;
		progress = true;
	}
	if(in->near_mesh_pending < p->mesh_low) {
		p->mesh_low = in->near_mesh_pending;
		progress = true;
	}
	if(in->have_census) {
		if(!p->census_primed) {
			/* The first census reading is a baseline, not progress: the
			 * provider slot can be briefly cleared (the pump's locked
			 * region), and its return must not reset the clock. */
			p->census_primed = true;
			p->resident_high = in->census_resident;
			p->pending_low = in->census_pending;
		} else {
			if(in->census_resident > p->resident_high) {
				p->resident_high = in->census_resident;
				progress = true;
			}
			if(in->census_pending < p->pending_low) {
				p->pending_low = in->census_pending;
				progress = true;
			}
		}
	}
	/* in->rx_bytes is deliberately not read: bytes are liveness. */
	if(progress) {
		p->no_progress_s = 0.0f;
		return 0.0f;
	}
	p->no_progress_s += dt;
	return p->no_progress_s;
}

enum lworld_remote_hold_action lworld_remote_hold_decide(float time_on_screen_s,
	float ceiling_s, float stalled_s, float stall_timeout_s, bool core_present,
	bool admissible) {
	bool can_admit = core_present && admissible;
	if(time_on_screen_s >= ceiling_s)
		return can_admit ? LWORLD_HOLD_FORCE_ADMIT : LWORLD_HOLD_CEILING_ESCAPE;
	if(stalled_s >= stall_timeout_s && !can_admit)
		return LWORLD_HOLD_STALL_ESCAPE;
	return LWORLD_HOLD_WAIT;
}

uint32_t lworld_core_force_admits(void) {
	return s_core_force_admits;
}

/* PRESENTATION ONLY -- and it must stay that way (Astra gate-2 item 9,
 * "separate presentation resets from network-world identity"; code-map
 * constraint 10). Every field below is screen-episode state: the clock this
 * screen has been up, the measured result of the last completed screen, and
 * the two latches that say an episode is in progress / already decided.
 *
 * What this function must NEVER touch, and does not:
 *  - the remote entry table (the OBLIGATION LEDGER). It is cleared only by
 *    the read thread at a dimension epoch change and by server_remote_cancel
 *    (server_remote_transport.c) -- clearing it here would erase valid,
 *    still-outstanding announcements and buffered new-epoch bodies, which is
 *    exactly the failure item 9 names.
 *  - s_ready_timeout_admits, the cumulative telemetry counter: it records
 *    what already happened, so a new episode does not un-happen it.
 * The two waiting-state fields ARE reset: they are what this screen is
 * currently displaying, not an obligation -- a fresh episode starts by
 * showing "Building terrain" again and re-derives the waiting state from the
 * live table on its next evaluated frame. */
void lworld_reset_presentation(void) {
	s_time_on_screen = 0.0f;
	for(unsigned owner = 0; owner < LOADING_TIPS_OWNERS; owner++)
		s_local_progress[owner] = (struct lworld_loading_progress){.percent = -1};
	s_episode_switch_seen = false;
	s_episode_centre_set = false;
	s_episode_readings = 0;
	s_load_screen_ms_result = 0;
	s_core_resident_ms = 0;
	s_core_meshed_ms = 0;
	s_presentation_pending = true;
	s_presentation_prepared = false;
	s_waiting_core = false;
	s_waiting_core_have = 0;
	s_waiting_core_stage = LWORLD_WAIT_COLUMNS;
	s_waiting_core_next_log_s = 0.0f;
	s_core_force_admit = false;
}

static void screen_lworld_reset(struct screen* s, int width, int height) {
	/* A WORLD_RESET can reset readiness while this screen remains visible.
	 * Only entry resets the tips clock, never a terrain/stage update. */
	s_tip_clock_started = false;
	input_pointer_enable(false);

	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;

	lworld_remote_progress_reset(&s_remote_progress);
	s_remote_time_on_screen = 0.0f;
	s_remote_hold_release_seen = false;
	s_remote_hold_s = 0.0f;
	lworld_reset_presentation();
}

// Pure gate (2026-09-03 maintainer ruling): may the loading screen transition
// to gameplay yet? True once the near ring genuinely settled (near_missing
// == 0) OR the bounded fallback timeout elapsed -- a real near-ring stall
// (a permanently unreachable chunk, a wedged mesher) must not hang this
// screen forever. *out_timed_out reports which branch fired (true only on
// the timeout path, never on a normal settle) so the caller logs a warning
// exactly when the fallback actually had to cover for something.
bool lworld_near_ring_ready(int near_missing, float time_on_screen_s,
							float timeout_s, bool* out_timed_out) {
	bool timed_out = time_on_screen_s >= timeout_s;
	if(out_timed_out)
		*out_timed_out = timed_out && near_missing != 0;
	return near_missing == 0 || timed_out;
}

/* ---- REMOTE readiness (M3, mp-split-0912/18) -----------------------------
 * docs/mp_admission_budget_design_2026-09-13.md section 2.6. The gate above
 * stays the SINGLE-PLAYER gate, unchanged and still the one
 * lworld_prepare_presentation uses for a local server: it asks "is the whole
 * local view-radius disk installed and meshed", which is answerable because
 * the local server is the thing filling that disk.
 *
 * In a remote session that question is unanswerable, and the A4 measurement
 * (tmp/jobs/mp-split-0912/m3-readiness-report.md) showed why: the client
 * scanned 441 columns while the peer's own view loop had committed to ~36, so
 * near_missing sat at 411 and EVERY join -- and every dimension swap -- was
 * released by the 30 s backstop rather than by readiness. The remote gate
 * therefore asks the two questions that are answerable:
 *   1. is the ground under the player SAFE (the 9-column core installed and
 *      meshed, i.e. collision-ready), and
 *   2. has the peer finished delivering what it has ALREADY announced inside
 *      mp.ready_radius?
 * Question 2 is inherently bounded -- a server stops announcing when its own
 * view loop is done -- and per Astra item 8 a later announcement is NOT a
 * re-gate: it installs through the normal pump behind the loading screen.
 * Question 1 is the floor the backstop may never cross (Astra item 9). */
void lworld_set_ready_census_provider(lworld_ready_census_fn fn,
									  lworld_rx_bytes_fn rx_fn,
									  struct server_remote* remote) {
	s_census_fn = fn;
	s_rx_bytes_fn = fn ? rx_fn : NULL;
	s_census_remote = fn ? remote : NULL;
}

uint32_t lworld_ready_timeout_admits(void) {
	return s_ready_timeout_admits;
}

bool lworld_ready_waiting_core(int* out_core_have, int* out_stage) {
	if(out_core_have)
		*out_core_have = s_waiting_core_have;
	if(out_stage)
		*out_stage = s_waiting_core_stage;
	return s_waiting_core;
}

bool lworld_remote_ready(const struct lworld_remote_ready_input* in,
						 float time_on_screen_s, float timeout_s,
						 bool* out_timed_out, bool* out_waiting_core) {
	bool timed_out = time_on_screen_s >= timeout_s;
	if(out_timed_out)
		*out_timed_out = false;
	if(out_waiting_core)
		*out_waiting_core = false;
	if(!in)
		return false;
	/* The CLIENT's own half of the safe core: the 9 sections exist and
	 * nothing in them is still meshing, so there is collision-ready ground
	 * under the player. This is the floor the backstop may never cross
	 * (Astra item 9) and it is readable with no provider at all. */
	bool world_core_ready = in->core_missing == 0 && in->core_mesh_pending == 0;
	if(!in->have_census) {
		/* No live census to ask. This is NOT "nothing outstanding" -- the
		 * ledger's word is simply unavailable, so this branch may never admit
		 * on the strength of it. Two production situations land here, and
		 * both need the clock to keep running rather than an eternal refusal:
		 *   - the readiness gate evaluated re-entrantly from INSIDE
		 *     server_remote_pump, which clears the provider slot for the
		 *     duration of its own locked region (the 2026-09-13 deadlock fix
		 *     in server_remote.c: the transport delivers FINISH_REMOTE_LOAD
		 *     synchronously under impl->lock, and that emit reaches this
		 *     gate). The very next frame's evaluation has the census back.
		 *   - a remote session whose impl is gone (kicked/torn down) while
		 *     this screen is still up.
		 * So: hold like any other incomplete episode, and fall back to the
		 * same 30 s backstop the pre-M3 code used -- but ONLY when the
		 * client's own world already holds the safe core. A missing or
		 * unmeshed core keeps waiting visibly, forever if need be, exactly as
		 * it does with a census in hand. */
		if(!world_core_ready) {
			if(out_waiting_core)
				*out_waiting_core = timed_out;
			return false;
		}
		if(!timed_out)
			return false;
		if(out_timed_out)
			*out_timed_out = true;
		s_ready_timeout_admits++;
		return true;
	}
	/* The safe core needs BOTH ledgers to agree: the remote table's RESIDENT
	 * (the peer's body was installed, so the column is the server's state and
	 * not a locally generated guess) and the client's own world (the section
	 * exists and nothing in its column is still meshing, so collision and
	 * render are ready). Either one short means the body could land on an
	 * absent or unmeshed floor. */
	bool safe_core = in->core_resident && world_core_ready;
	if(!safe_core) {
		/* Astra item 9: the 30 s backstop must NEVER bypass missing safe-core
		 * terrain. It does not become an admission here -- it becomes a
		 * visible waiting state, and the gate keeps being evaluated. */
		if(out_waiting_core)
			*out_waiting_core = timed_out;
		return false;
	}
	if(in->announced_pending == 0)
		return true;
	/* Core safe, only announced obligations left: the documented bounded
	 * delay. Admit at the backstop and record that we did. */
	if(!timed_out)
		return false;
	if(out_timed_out)
		*out_timed_out = true;
	s_ready_timeout_admits++;
	return true;
}

/* mp.ready_radius (design 2.6): the disk whose ALREADY-ANNOUNCED columns must
 * be installed before gameplay. Default = the LOD near radius (2 -> 25
 * columns), i.e. exactly the full-detail tier the player is about to look at;
 * clamped to [1, MAX_VIEW_DISTANCE] so a hand-edited config can neither
 * disable the check nor demand columns the client would never install. */
static int lworld_ready_radius(void) {
	int fallback = world_lod_near_radius();
	if(fallback < 1)
		fallback = 1;
	int radius
		= config_read_int(&gstate.config_user, "mp.ready_radius", fallback);
	if(radius < 1)
		radius = 1;
	if(radius > MAX_VIEW_DISTANCE)
		radius = MAX_VIEW_DISTANCE;
	return radius;
}

// H-3: main.c's heartbeat reads this for hw_telemetry's load_screen_ms.
uint32_t lworld_load_screen_ms_result(void) {
	return s_load_screen_ms_result;
}

void lworld_core_timing(uint32_t* out_resident_ms, uint32_t* out_meshed_ms) {
	if(out_resident_ms) *out_resident_ms = s_core_resident_ms;
	if(out_meshed_ms) *out_meshed_ms = s_core_meshed_ms;
}

/* Escape action: mirrors main.c's non-graceful remote-disconnect branch
 * EXACTLY (main.c ~1504-1509) -- clin_update_drain() then
 * clin_force_world_reset() run the client's own CRPC_WORLD_RESET teardown
 * synchronously on this (the client/main) thread, which unconditionally
 * clears remote_terrain_hold / world_switch_pending / world_loaded
 * regardless of which of the two triggers above parked this screen. That
 * call chain is the ALREADY-PROVEN main-thread-direct entry for exactly
 * this class of teardown (see client_interface.c's clin_force_world_reset
 * doc comment, S4 e246a79).
 *
 * The client-side teardown alone is not enough: main.c's mp_provider_remote
 * loop (main.c ~1402-1512) polls server_remote's status independently of
 * this screen every frame, and a session whose TRANSPORT never noticed the
 * stall still reads back REMOTE_PLAY -- which keeps re-arming main.c's own
 * 15s provider-swap deadline (main.c ~1457-1458, B5 finding 5) and never
 * restores server_local. So the remote session itself is torn down through
 * the SAME session API screen_pause.c's "Save and quit" (screen_pause.c
 * ~138-141) and screen_multiplayer_connect.c's mid-connect Cancel (~line
 * 240) already use: an SRPC_UNLOAD_WORLD queued via svin_rpc_send.
 * remote_process_srpc (server_remote_protocol.c ~221) handles that type
 * FIRST, unconditionally -- even outside REMOTE_PLAY -- by calling
 * server_remote_disconnect(), which queues a Packet255 disconnect frame and
 * sets graceful_stop=true + state=REMOTE_CLOSING under its own lock.
 *
 * That single state transition is what makes this race-safe against
 * main.c's provider loop, without this screen owning any of main.c's
 * private swap-deadline state:
 *   - main.c only re-arms its swap deadline while state==REMOTE_PLAY
 *     (main.c ~1457); leaving REMOTE_PLAY here starts that ALREADY-EXISTING
 *     15s belt ticking as a bounded backstop even if the graceful close's
 *     socket write never completes on a truly wedged transport (that hangs
 *     the WRITE thread in server_remote_transport.c, never this one --
 *     svin_rpc_send only enqueues into a bounded ring, it does not block);
 *   - once server_remote_pump()'s teardown-completion check (server_remote.c
 *     ~253) observes both worker threads exited (or the 15s belt forces a
 *     teardown via server_remote_cancel instead), main.c's
 *     REMOTE_DISCONNECTED branch (main.c ~1472-1486) restores server_local
 *     and clears mp_provider_remote/gstate.remote_session -- exactly like
 *     an ordinary quit-to-title;
 *   - because graceful_stop is now true, main.c's kick/drop branch
 *     (`!mp_status.graceful && mp_status.logged_in`, main.c ~1504) does NOT
 *     re-fire a second world-reset/screen-swap once DISCONNECTED lands --
 *     we already ran the client-side half of that exact sequence above, so
 *     there is no double teardown and no double screen transition.
 *
 * The final screen-swap step deliberately does NOT call
 * screen_multiplayer_connect_show_disconnected() (screen_multiplayer_connect.c)
 * even though that is main.c's equivalent call: that TU is kept OUT of the
 * shared engine object pool this file lives in (CMakeLists.txt's
 * CAVEX_MP_SOURCES comment -- it drags in the socket provider, which every
 * OTHER PC unit test's link must stay free of), so linking it here would
 * break those tests. Instead this writes the SAME gstate fields that
 * function writes (game_state.h's comment on remote_disconnect_requested/
 * _reason has the full rationale) and swaps screens through the shared
 * screen_multiplayer_connect POINTER (screen.h) -- the exact indirection
 * screen_title.c already uses to reach this same screen from the shared
 * pool. NULL only if MP was never initialized (main.c registers it right
 * after mp_session_init()), which cannot happen while remote_session is
 * true. */
static void screen_lworld_remote_escape(const char* reason) {
	loading_reason_end(0);
	clin_update_drain();
	clin_force_world_reset();

	// CHIP B: reliable delivery -- a saturated svin_rpc_send latches a
	// per-frame retry inside clin_update instead of being silently dropped
	// (this function moves on to screen_multiplayer_connect regardless, so
	// an ignored failure here used to leave server_remote stuck in
	// REMOTE_PLAY with no client-side sign anything was still wrong).
	clin_send_unload_world();

	snprintf(gstate.remote_disconnect_reason,
			sizeof(gstate.remote_disconnect_reason), "%s", reason);
	gstate.remote_disconnect_requested = true;
	if(screen_multiplayer_connect)
		screen_set(screen_multiplayer_connect);
}


/* The terrain the loading screen judges: PLAYER 0's world. With world-bound
 * payloads OFF that is gstate.world, exactly as before. ON, player 0 may be in
 * the extra slot while the guest's world occupies gstate.world (A2-T: the host
 * leaves for the Nether, the guest stays in the Overworld) -- judging
 * gstate.world there would read the GUEST's columns around the host's
 * coordinates (Fable B2-A2T F2). */
static struct world* lworld_terrain(void) {
	struct clin_world_payload p;
	if(clin_world_payloads_enabled()
	   && clin_world_payload_of_slot(clin_world_payload_primary_slot(), &p)
	   && p.terrain)
		return p.terrain;
	return &gstate.world;
}

uint32_t lworld_local_loading_percent(void) {
	/* The SAME bounded census and radius as the local admission gate below.
	 * Missing columns owe COLUMN_HEIGHT mesh slots; installed columns owe
	 * only their pending lod-0 meshes (empty/far slots satisfy the gate).
	 * These dictionaries/flags are main-thread owned: workers deliver results
	 * through chunk_mesher_receive, never a seqlock or a worker-side scan. */
	int radius = world_server_view_radius() ? world_server_view_radius()
		: MAX_VIEW_DISTANCE;
	int missing = 0, pending = 0;
	w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.x));
	w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.z));
	world_near_ring_status(lworld_terrain(), cx, cz, radius, &missing, &pending);
	uint32_t total = (uint32_t)(2 * radius + 1) * (2 * radius + 1) * COLUMN_HEIGHT;
	uint32_t remaining = (uint32_t)missing * COLUMN_HEIGHT + (uint32_t)pending;
	uint32_t percent = remaining >= total ? 0 : (total - remaining) * 100 / total;
	bool ready = gstate.world_loaded && !gstate.world_load_failed
		&& !gstate.remote_terrain_hold && !gstate.world_switch_pending
		&& (s_presentation_prepared || lworld_near_ring_ready(missing + pending,
			s_time_on_screen, LWORLD_NEAR_READY_TIMEOUT_S, NULL));
	/* The existing timeout may admit incomplete terrain. Conversely, even a
	 * complete census cannot finish before the server's load/position FIFO. */
	return ready ? 100 : (percent == 100 ? 99 : percent);
}

/* The two escape triggers (task_db613f13 design): a no-progress timeout
 * that fires with zero player input, and a manual held-back give-up once a
 * grace period has passed. Kept to plain scalars in/out of the pure
 * lworld_stall_tracker (screen.h) so the interesting arithmetic is unit-
 * testable headlessly (tests/test_mp_s0_env.c); only screen/input glue
 * lives in this function. */
static void screen_lworld_remote_update(struct screen* s, float dt) {
	(void)s;
	s_remote_time_on_screen += dt;

	/* REL-MP-LOADHANG: readiness progress only (see
	 * lworld_remote_progress_tick). Received bytes are read for the log
	 * line, never as progress. */
	struct lworld_remote_progress_input prog;
	memset(&prog, 0, sizeof(prog));
	prog.installs = clin_chunk_install_seq();
	prog.chunks = world_loaded_chunks(lworld_terrain());
	prog.rx_bytes = (s_rx_bytes_fn && s_census_remote)
		? s_rx_bytes_fn(s_census_remote) : 0;
	w_coord_t pcx = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.x));
	w_coord_t pcz = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.z));
	int ready_radius = lworld_ready_radius();
	world_near_ring_status(lworld_terrain(), pcx, pcz, ready_radius,
						   &prog.near_missing, &prog.near_mesh_pending);
	struct server_remote_ready_census census;
	if(s_census_fn
	   && s_census_fn(s_census_remote, (int)pcx, (int)pcz, ready_radius,
					  &census)) {
		prog.have_census = true;
		prog.census_resident = census.resident_in_radius;
		prog.census_pending = census.announced_pending;
	}
	float stalled_s = lworld_remote_progress_tick(&s_remote_progress, &prog, dt);
	/* The client's OWN safe core, the instrument lworld_prepare_presentation
	 * uses: installed and meshed means collision-ready ground. */
	int core_missing = 0, core_mesh_pending = 0;
	world_near_ring_status(lworld_terrain(), pcx, pcz, LWORLD_SAFE_CORE_RADIUS,
						   &core_missing, &core_mesh_pending);
	/* Present, not "meshed": see lworld_remote_hold_decide. */
	bool core_present = core_missing == 0;
	bool admissible = gstate.world_loaded && !gstate.world_switch_pending
		&& !gstate.remote_terrain_hold && !gstate.world_load_failed;
	enum lworld_remote_hold_action action = lworld_remote_hold_decide(
		s_remote_time_on_screen, LWORLD_REMOTE_HARD_CEILING_S, stalled_s,
		LWORLD_REMOTE_STALL_TIMEOUT_S, core_present, admissible);
	/* Once the ceiling latched an admission, only the player's own held
	 * give-up may still disconnect; a later frame that reads the core short
	 * (or a hold flicker) must not undo it into "Terrain never arrived". */
	if(s_core_force_admit && action != LWORLD_HOLD_FORCE_ADMIT)
		action = LWORLD_HOLD_WAIT;

	bool hint_visible = s_remote_time_on_screen >= LWORLD_REMOTE_HOLD_HINT_S;

	// Fresh-press latch: a back/inventory button already held when this
	// screen appeared (unlikely mid-loading, but cheap insurance -- the
	// same idiom screen_death.c's click_armed uses) must not insta-fire.
	if(!input_held(IB_INVENTORY))
		s_remote_hold_release_seen = true;

	if(hint_visible && s_remote_hold_release_seen && input_held(IB_INVENTORY))
		s_remote_hold_s += dt;
	else
		s_remote_hold_s = 0.0f;

	bool manual_escape = s_remote_hold_s >= LWORLD_REMOTE_HOLD_CONFIRM_S;

	if(action == LWORLD_HOLD_FORCE_ADMIT && !s_core_force_admit) {
		/* Admission itself stays in lworld_prepare_presentation; this only
		 * lifts the census half of the gate for this episode. */
		s_core_force_admit = true;
		fprintf(stderr,
			"[screen_load_world] remote readiness: ledger never settled "
			"after %.1fs (census=%s, %zu resident / %zu announced pending "
			"within radius %d, rx=%u B, core meshing %d) -- admitting on "
			"the installed core once it is meshed (hard ceiling %.0fs)\n",
			(double)s_remote_time_on_screen, prog.have_census ? "yes" : "none",
			prog.census_resident, prog.census_pending, ready_radius,
			(unsigned)prog.rx_bytes, core_mesh_pending,
			(double)LWORLD_REMOTE_HARD_CEILING_S);
	}
	if(manual_escape) {
		screen_lworld_remote_escape(LWORLD_REMOTE_STALL_REASON);
	} else if(action == LWORLD_HOLD_STALL_ESCAPE) {
		fprintf(stderr,
			"[screen_load_world] remote readiness: no readiness progress "
			"for %.1fs (rx=%u B, core %d missing / %d meshing) -- "
			"disconnecting: %s\n",
			(double)stalled_s, (unsigned)prog.rx_bytes, core_missing,
			core_mesh_pending, LWORLD_REMOTE_STALL_REASON);
		screen_lworld_remote_escape(LWORLD_REMOTE_STALL_REASON);
	} else if(action == LWORLD_HOLD_CEILING_ESCAPE) {
		fprintf(stderr,
			"[screen_load_world] remote readiness: hard ceiling %.0fs "
			"reached without an installed, meshed core (%d of %d core "
			"columns missing, %d meshing, world_loaded=%d switch=%d hold=%d) "
			"-- disconnecting: %s\n",
			(double)LWORLD_REMOTE_HARD_CEILING_S, core_missing,
			LWORLD_SAFE_CORE_COLUMNS, core_mesh_pending,
			(int)gstate.world_loaded, (int)gstate.world_switch_pending,
			(int)gstate.remote_terrain_hold, LWORLD_REMOTE_CEILING_REASON);
		screen_lworld_remote_escape(LWORLD_REMOTE_CEILING_REASON);
	}
}

static void screen_lworld_update(struct screen* s, float dt) {
	// A live dimension switch deliberately shows this screen while the old
	// world is still valid. Do not bounce back to gameplay until the server
	// either cancels the pre-commit transition or finishes the reset/load.
	// remote_terrain_hold: the same-dim respawn "Downloading terrain" hold
	// keeps this screen up until the chunk under the player installs
	// (released by clin_update; see game_state.h).
	//
	// Remote-only escape hatch (task_db613f13): gstate.world_load_failed
	// already has its own signposted terminal message ("Restart CaveWii to
	// continue") for a client allocation failure -- a different problem
	// from a stalled connection -- so it is left alone here rather than
	// being pre-empted by a generic "Connection stalled" screen.
	if(gstate.remote_session && !gstate.world_load_failed) {
		screen_lworld_remote_update(s, dt);
		// The escape hatch may have just torn this world down and swapped
		// to the disconnected screen synchronously
		// (screen_multiplayer_connect_show_disconnected calls screen_set()
		// inline) -- if so, gstate.players[0].current_screen is no longer this screen
		// and nothing below should run against its now-stale state.
		if(gstate.players[0].current_screen != s)
			return;
	}

	s_time_on_screen += dt;
	/* The next frame's early clin_update offers readiness before main chooses
	 * its render-world path. Sending the ACK here would resume simulation after
	 * that decision was already cached for the still-visible loading screen. */
	screen_set(&screen_ingame);
}

/* Preparing the view is independent of transport acknowledgement. The local
 * server must not resume simulation until this decision passes. Retain it
 * across ACK backpressure so later mesh dirtiness cannot reopen the load screen
 * after the server has already resumed. BEGIN/RESET starts a new episode. */
bool lworld_prepare_presentation(void) {
	if(!gstate.world_loaded || gstate.world_load_failed || gstate.remote_terrain_hold)
		return false;
	if(s_presentation_prepared) return true;
	w_coord_t near_cx
		= WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.x));
	w_coord_t near_cz
		= WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.z));
	/* M3: a REMOTE session cannot be judged by the LOCAL view-radius disk --
	 * see lworld_remote_ready's comment and the A4 measurement. Single player
	 * keeps the branch below, byte-identical (same scan, same radius
	 * expression, same gate, same warning): gstate.remote_session is the only
	 * thing that can route away from it, and it is set exclusively inside
	 * main.c's remote-provider block. */
	if(gstate.remote_session) {
		/* Safe core, from the client's own world, with the SAME instrument
		 * the local branch uses -- radius 1 instead of the view radius. */
		int core_missing = 0, core_mesh_pending = 0;
		world_near_ring_status(lworld_terrain(), near_cx, near_cz,
			LWORLD_SAFE_CORE_RADIUS, &core_missing, &core_mesh_pending);
		int ready_radius = lworld_ready_radius();
		struct server_remote_ready_census census;
		struct lworld_remote_ready_input in;
		memset(&in, 0, sizeof(in));
		in.core_missing = core_missing;
		in.core_mesh_pending = core_mesh_pending;
		/* REL-MP-COREFIRST telemetry: first frame each half held. */
		uint32_t now_ms = (uint32_t)(s_time_on_screen * 1000.0f);
		if(now_ms == 0) now_ms = 1;
		if(core_missing == 0 && !s_core_resident_ms)
			s_core_resident_ms = now_ms;
		if(core_missing == 0 && core_mesh_pending == 0 && !s_core_meshed_ms)
			s_core_meshed_ms = now_ms;
		/* Evaluated every deciding frame against the CURRENT table: an
		 * announcement that arrives while this screen is still up simply
		 * delays readiness; one that arrives after admission never re-gates,
		 * because s_presentation_prepared latches above. */
		if(s_census_fn
		   && s_census_fn(s_census_remote, (int)near_cx, (int)near_cz,
						  ready_radius, &census)) {
			in.have_census = true;
			in.core_resident = census.core_resident;
			in.announced_pending = census.announced_pending;
		}
		bool remote_timed_out = false, waiting_core = false;
		bool ready = lworld_remote_ready(&in, s_time_on_screen,
			LWORLD_NEAR_READY_TIMEOUT_S, &remote_timed_out, &waiting_core);
		s_waiting_core = waiting_core;
		s_waiting_core_have = LWORLD_SAFE_CORE_COLUMNS - core_missing;
		/* Which half of the safe core is short, so the on-screen text can
		 * never claim a complete core while the gate is still refusing: a
		 * missing SECTION is the column count; otherwise it is either the
		 * mesh (section present, geometry/collision not built) or the ledger
		 * (the peer's body for a core column has not been installed yet). */
		s_waiting_core_stage = core_missing > 0
			? LWORLD_WAIT_COLUMNS
			: (core_mesh_pending > 0 ? LWORLD_WAIT_MESHING
									 : LWORLD_WAIT_INSTALLING);
		/* The gate is evaluated every frame, so the diagnostic is rate-
		 * limited to the rising edge plus one line every 5s -- a held
		 * loading screen must be explainable in the log without writing
		 * 60 lines a second into it. */
		if(waiting_core
		   && s_time_on_screen >= s_waiting_core_next_log_s) {
			s_waiting_core_next_log_s = s_time_on_screen + 5.0f;
			fprintf(stderr,
				"[screen_load_world] remote readiness: safe core still "
				"incomplete after %.1fs (%d of %d core columns installed, "
				"%d core sub-chunks meshing, census=%s) -- HOLDING the "
				"loading screen; the backstop does not bypass missing "
				"terrain\n",
				(double)s_time_on_screen, s_waiting_core_have,
				LWORLD_SAFE_CORE_COLUMNS, core_mesh_pending,
				in.have_census ? "yes" : "none");
		}
		if(!ready && s_core_force_admit && core_missing == 0
		   && core_mesh_pending == 0) {
			/* REL-MP-LOADHANG hard ceiling (lead decision 2026-10-21): the
			 * client's own core is installed and meshed, only the ledger
			 * never settled. Admit on it and count it. */
			s_core_force_admits++;
			fprintf(stderr,
				"[screen_load_world] remote readiness: admitting on the "
				"installed core after %.1fs (core_resident=%d, %zu "
				"announced pending) -- lw_core_force_admits=%u\n",
				(double)s_time_on_screen, (int)in.core_resident,
				in.announced_pending, (unsigned)s_core_force_admits);
			ready = true;
		}
		if(!ready)
			return false;
		if(remote_timed_out && !in.have_census)
			fprintf(stderr,
				"[screen_load_world] remote readiness: no census available "
				"(no provider armed) but the client's own safe core is "
				"installed and meshed -- admitting after the %.1fs backstop\n",
				(double)s_time_on_screen);
		else if(remote_timed_out)
			fprintf(stderr,
				"[screen_load_world] remote readiness: safe core ready, "
				"admitting after the %.1fs backstop with %zu announced "
				"column(s) within radius %d still installing -- they keep "
				"streaming in the background\n",
				(double)s_time_on_screen, in.announced_pending, ready_radius);
		s_presentation_prepared = true;
		return true;
	}
	int missing = 0, mesh_pending = 0;
	world_near_ring_status(lworld_terrain(), near_cx, near_cz,
		world_server_view_radius() ? world_server_view_radius() : MAX_VIEW_DISTANCE,
		&missing, &mesh_pending);
	int near_missing = missing + mesh_pending;
	bool timed_out = false;
	if(!lworld_near_ring_ready(near_missing, s_time_on_screen,
		LWORLD_NEAR_READY_TIMEOUT_S, &timed_out)) return false;
	if(timed_out)
		fprintf(stderr,
			"[screen_load_world] near-ring readiness timed out "
			"after %.1fs with near_missing=%d -- entering "
			"gameplay after input acknowledgement (fallback), terrain will "
			"keep streaming/meshing in the background\n",
			(double)s_time_on_screen, near_missing);
	s_presentation_prepared = true;
	return true;
}

bool lworld_admit_gameplay(void) {
	/* Ordinary menu closes do not start a fresh terrain wait. Even a prepared
	 * view or timeout cannot override a failure or the transport's FIFO hold. */
	if(gstate.world_switch_pending || gstate.world_load_failed
	   || gstate.remote_terrain_hold) return false;
	if(!s_presentation_pending && gstate.players[0].current_screen != &screen_load_world)
		return true;
	if(!lworld_prepare_presentation()) return false;
	s_load_screen_ms_result = (uint32_t)(s_time_on_screen * 1000.0f);
	s_presentation_pending = false;
	loading_reason_end(0);
	return true;
}

/* Presentation only. Admission, the remote stall timer and held disconnect
 * above retain their existing semantics. Unknown work has no fake percentage. */
static void lworld_center_text(int width,int y,const char* text,int size,bool dark) {
    gutil_text_rgb((width-gutil_font_width(text,size))/2,y,text,size,!dark,
        dark?0x30:0xFF,dark?0x30:0xFF,dark?0x30:0xFF);
}
static void lworld_render_tip(unsigned owner, const struct loading_tip* tip) {
	const struct loading_tip_layout* l = loading_tips_current_layout(owner);
	if(!tip || !l) return;
	/* Approved paper/2px edges, then a fully opaque inner text well. */
	fep_fill(fep_rect(l->x,l->y,l->width,l->height),0x64,0x60,0x57,255);
	fep_fill(fep_rect(l->x,l->y,l->width,2),0x25,0x25,0x25,255);
	fep_fill(fep_rect(l->x,l->y,2,l->height),0x25,0x25,0x25,255);
	fep_fill(fep_rect(l->x+2,l->y+2,l->width-4,2),0xE2,0xDE,0xD3,255);
	fep_fill(fep_rect(l->x+2,l->y+2,2,l->height-4),0xE2,0xDE,0xD3,255);
	fep_fill(fep_rect(l->x+l->width-2,l->y,2,l->height),0x45,0x41,0x3B,255);
	fep_fill(fep_rect(l->x,l->y+l->height-2,l->width,2),0x45,0x41,0x3B,255);
	fep_fill(fep_rect(l->x+8,l->y+6,l->width-16,l->height-12),0x20,0x24,0x1F,255);
	if(l->icon) {
		struct item_data item = {.id=tip->item,.durability=tip->metadata,.count=1};
		gutil_draw_item_sized(&item,l->x+16,l->y+8,40,0);
	}
	gutil_text_rgb(l->body_x,l->y+8,"Tip",16,false,0xFF,0xF0,0xB0);
	int x=l->body_x, line=0;
	int gap=gutil_font_width(" ",16);
	for(unsigned k=0;k<l->count;k++) {
		const struct loading_tip_token* t=&l->tokens[k];
		if(t->line!=line) { line=t->line; x=l->body_x; }
		int y=l->y+32+line*24, glyph=0;
		if(t->glyph>=0) glyph=gutil_prompt_glyph(x,y-4,24,t->glyph);
		gutil_text_rgb(x+glyph,y,t->text,16,false,0xFF,0xF8,0xE7);
		x+=t->width+gap;
	}
}

/* Beta client decompile: ../mcb173-retromcp/minecraft/src_original/net/
 * minecraft/src/LoadingScreenRenderer.java, setLoadingProgress (GuiProgress):
 * var1 >= 0 draws the 100x2 track first (8421504 = 0x808080), then
 * var1-wide fill (8454016 = 0x80FF80), both untextured quads. Fable's approved
 * TV presentation scales that geometry to 400x12 and adds a 2px surround. */
void lworld_draw_loading_status(bool remote, int width, int height,
	float elapsed_s, const char* heading, const char* world_name,
	const char* stage, int percent) {
	bool split = height <= 240;
	if(remote) {
		lworld_center_text(width,split?24:170,heading,22,false);
    /* Activity, not completion: installation and meshing have no aggregate
     * denominator. The supplied episode clock steps the lit block. */
    struct loading_indicator ind;
    loading_indicator_layout(width,height,elapsed_s,&ind);
    for(int k=0;k<LOADING_INDICATOR_SQUARES;k++) {
        const struct loading_indicator_square* q=&ind.squares[k];
        int b=LOADING_INDICATOR_BORDER;
        unsigned rgb=k==ind.lit?LOADING_INDICATOR_LIT_RGB:LOADING_INDICATOR_UNLIT_RGB;
        fep_fill(fep_rect(q->x-b,q->y-b,q->size+2*b,q->size+2*b),
            (LOADING_INDICATOR_BORDER_RGB>>16)&0xFF,(LOADING_INDICATOR_BORDER_RGB>>8)&0xFF,
            LOADING_INDICATOR_BORDER_RGB&0xFF,255);
        fep_fill(fep_rect(q->x,q->y,q->size,q->size),
            (rgb>>16)&0xFF,(rgb>>8)&0xFF,rgb&0xFF,255);
    }
		return;
	}
	lworld_center_text(width,split?24:150,"Building terrain",24,false);
	int name_size = 18;
	if(gutil_font_width(world_name,name_size) > width-32) name_size = 12;
	char fitted_name[sizeof s_world_name];
	fep_text_fit(fitted_name,sizeof fitted_name,world_name,name_size,width-32);
	lworld_center_text(width,split?54:186,fitted_name,name_size,false);
	int bar_width = width < 432 ? width-32 : 400;
	int x = (width-bar_width)/2, y = split?82:222;
	fep_fill(fep_rect(x-2,y-2,bar_width+4,16),0x20,0x20,0x20,255);
	fep_fill(fep_rect(x,y,bar_width,12),0x80,0x80,0x80,255);
	char label[128];
	if(percent >= 0 && percent <= 100) {
		fep_fill(fep_rect(x,y,bar_width*percent/100,12),0x80,0xFF,0x80,255);
		snprintf(label,sizeof label,"%s %d%%",stage,percent);
	} else {
		/* No denominator: no fill and no numeric completion claim. */
		snprintf(label,sizeof label,"%s...",stage);
	}
	lworld_center_text(width,split?108:248,label,16,false);
}

/* Shared with the draw-capture test: only the host owns the local census
 * and prepared latch. Guest transfer readiness has no measured fraction. */
int lworld_local_loading_status(unsigned owner, bool generating,
	const char** stage) {
	*stage = generating ? "Generating level" : "Loading level";
	if(owner >= LOADING_TIPS_OWNERS) return -1;
	struct lworld_loading_progress* progress = &s_local_progress[owner];
	uint32_t measured = UINT32_MAX;
	if(owner == 0) {
		measured = lworld_local_loading_percent();
		w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.x));
		w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(gstate.players[0].camera.z));
		if(!s_episode_centre_set) {
			s_episode_centre_set = true;
			s_episode_cx = cx;
			s_episode_cz = cz;
		}
		if(gstate.world_switch_pending || cx != s_episode_cx || cz != s_episode_cz
		   || ++s_episode_readings > LWORLD_STALE_RING_FRAMES)
			s_episode_switch_seen = true;
		if(!s_episode_switch_seen && measured < 100)
			measured = 0; /* the old scene's ring, not this load's */
		uint32_t imported = UINT32_MAX;
		bool converting = server_local_import_progress(&imported);
		if(converting != progress->converting)
			*progress = (struct lworld_loading_progress){
				.percent = -1, .converting = converting};
		if(converting) {
			*stage = "Converting world";
			/* Conversion completion does not replace the terrain gate. */
			if(imported <= 100 && measured < 100 && imported > measured)
				measured = imported == 100 ? 99 : imported;
		}
	}
	lworld_loading_progress_update(progress, measured);
	return progress->percent;
}

static void lworld_render_loading_art(unsigned owner,int width,int height,
	uint64_t now_ms,float elapsed_s, const char* stage) {
	(void)stage; /* remote counts remain in log/F3, as before */
	if(!fep_backdrop_at(width,height,72,elapsed_s,true)) gutil_bg();
	bool split = height <= 240;
	enum loading_reason reason = loading_reason_get(owner);
	const char* local_stage = "Loading level";
	int percent = -1;
	if(!gstate.remote_session)
		percent = lworld_local_loading_status(owner, reason == NEW_WORLD, &local_stage);
	lworld_draw_loading_status(gstate.remote_session,width,height,elapsed_s,
		loading_reason_heading(reason),lworld_world_name(),local_stage,percent);
	struct loading_tip_context context;
	lworld_tip_context(owner,width,split,false,&context);
	lworld_render_tip(owner,loading_tips_tick(owner,now_ms,&context));
}

void lworld_render_loading_owner(unsigned owner,int width,int height,
	uint64_t now_ms,size_t loaded_chunks) {
	if(owner>=LOADING_TIPS_OWNERS) return;
	char stage[96];
	snprintf(stage,sizeof stage,"Preparing terrain (%u chunks loaded)",
		(unsigned)loaded_chunks);
	lworld_render_loading_art(owner,width,height,now_ms,now_ms/1000.0F,stage);
}

void lworld_render_loading_viewport(int width,int height,float elapsed_s,
                                   size_t loaded_chunks) {
    char stage[96];
    snprintf(stage,sizeof(stage),"Preparing terrain (%u chunks loaded)",
        (unsigned)loaded_chunks);
    /* Compatibility guest renderer. TIPS-2 supplies the explicit owner and
     * continuous wall-clock through lworld_render_loading_owner. */
    lworld_render_loading_art(1,width,height,(uint64_t)(elapsed_s*1000.0F),elapsed_s,stage);
}

static void screen_lworld_render2D(struct screen* s,int width,int height) {
    (void)s;
    float scale=(float)height/480.0F;
    int ox=(width-(int)(640*scale))/2;
    if(gstate.world_load_failed) {
		loading_reason_end(0);
        if(!fep_backdrop_at(width,height,72,s_time_on_screen,false)) gutil_bg();
        fep_paper(fep_rect(ox+(int)(80*scale),(int)(108*scale),
            (int)(480*scale),(int)(256*scale)));
        lworld_center_text(width,(int)(128*scale),"Could not open world",(int)(24*scale),true);
        lworld_center_text(width,(int)(208*scale),
            gstate.world_load_failed_reason[0]?gstate.world_load_failed_reason
                                              :"Terrain memory could not be recovered",
            (int)(16*scale),true);
        lworld_center_text(width,(int)(256*scale),"Restart CavEX to continue",(int)(16*scale),true);
        return;
    }
    char stage[96];
    int core_have=0,core_stage=LWORLD_WAIT_COLUMNS;
    if(lworld_ready_waiting_core(&core_have,&core_stage)) {
        const char* action=core_stage==LWORLD_WAIT_MESHING?"Building":
            core_stage==LWORLD_WAIT_INSTALLING?"Installing":"Waiting for";
        snprintf(stage,sizeof(stage),"%s terrain (%d of %d core columns)",
            action,core_have,LWORLD_SAFE_CORE_COLUMNS);
    } else {
        snprintf(stage,sizeof(stage),"Preparing terrain (%u chunks loaded)",
            (unsigned)world_loaded_chunks(lworld_terrain()));
    }
    if(!s_tip_clock_started) {
		s_tip_epoch=time_get(); s_tip_clock_started=true;
	}
	uint64_t now_ms=time_diff_us(s_tip_epoch,time_get())/1000;
	bool blocked=gstate.remote_session&&s_remote_time_on_screen>=LWORLD_REMOTE_HOLD_HINT_S;
	if(blocked) {
		struct loading_tip_context context;
		lworld_tip_context(0,width,height<=240,true,&context);
		(void)loading_tips_tick(0,now_ms,&context);
	}
    lworld_render_loading_art(0,width,height,now_ms,s_time_on_screen,stage);
    if(gstate.remote_session&&s_remote_time_on_screen>=LWORLD_REMOTE_HOLD_HINT_S) {
        lworld_center_text(width,(int)(330*scale),"Connection stalled? You can give up and disconnect",
            (int)(16*scale),false);
        /* The centred hint row (controller UI redesign 10-31, A1 helper). */
        struct input_player_frame hint_f;input_prompt_frame(0,&hint_f);
        const struct gutil_hint_item hint_item={{IB_INVENTORY},1,
            s_remote_hold_s>0.0F?"Hold to give up":"Give up",0,true};
        struct gutil_hint_row hint_row;
        if(gutil_hint_row_layout(&hint_f,0,0,width,height,false,
            GUTIL_HINT_ALIGN_CENTRE,&hint_item,1,&hint_row))
            gutil_hint_row_draw(&hint_row);
    }
}

struct screen screen_load_world = {
	.reset = screen_lworld_reset,
	.update = screen_lworld_update,
	.render2D = screen_lworld_render2D,
	.render3D = NULL,
	.render_world = false,
};
