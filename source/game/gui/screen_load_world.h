/* Loading-screen readiness seam (M3, mp-split-0912/18).
 *
 * screen_load_world.c's SP contract (the pure near-ring gate, the stall
 * tracker, lworld_prepare_presentation / _admit_gameplay / _reset_presentation)
 * stays declared in screen.h where every screen finds it. What lives HERE is
 * the REMOTE readiness gate and the provider seam it needs, kept in their own
 * header for one structural reason:
 *
 *   screen_load_world.c is an engine-core translation unit (CMake's
 *   CAVEX_CLIENT_SOURCES, pooled into cavex_test_core), and the MP client
 *   (CAVEX_MP_SOURCES: server_remote*.c, mp_session.c) is a SEPARATE pool that
 *   almost no test links. A direct call from this screen to
 *   server_remote_ready_census() would therefore be an undefined symbol in
 *   every full-engine test link that has no MP sources. So the dependency is
 *   INVERTED: the screen owns a provider slot, and the MP provider installs
 *   itself into it (server_remote.c's pump, which is the only code that runs
 *   exactly while a remote session is live). With no provider installed the
 *   remote branch reports "no census" and, per Astra gate-2 item 9, refuses to
 *   admit gameplay rather than guessing -- it fails visibly instead.
 *
 * The census TYPE is shared with the provider (server_remote.h is a plain
 * declarations header, so including it costs no link edge) rather than
 * mirrored, so the two can never drift.
 *
 * SLOT LIFETIME (2026-09-13, F3). The provider is armed only across the gap
 * BETWEEN pumps -- server_remote_pump clears the slot on entry and installs
 * it after its last unlock -- because the pump delivers client RPCs
 * SYNCHRONOUSLY while holding the transport's impl->lock (notably
 * remote_emit_load_completion_locked's FINISH_REMOTE_LOAD, which reaches
 * screen_set -> lworld_admit_gameplay -> lworld_prepare_presentation). An
 * armed slot in that window re-entered server_remote_ready_census and
 * deadlocked the main thread on a non-recursive mutex it already held. So
 * "no provider" is a NORMAL, transient state during a live session, not only
 * the no-session case, and lworld_remote_ready's no-census branch is what
 * defines it: never ready on the ledger's word, but the clock keeps running
 * and the client's own safe core can still admit at the backstop.
 */
#ifndef SCREEN_LOAD_WORLD_H
#define SCREEN_LOAD_WORLD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Local presentation only. Last measured stage fraction survives regressed
 * or unavailable readings. UINT32_MAX means no new measurement. */
struct lworld_loading_progress {
	unsigned high_water;
	int percent; /* -1 = indeterminate */
	bool converting; /* conversion and terrain have separate high-water stages */
};
void lworld_loading_progress_update(struct lworld_loading_progress* progress,
	uint32_t measured_percent);
/* UI thread, before queuing a local load; resolves LevelName read-only. */
void lworld_set_world_path(const char* path);
const char* lworld_world_name(void);
/* Main/UI thread only. Bounded near-ring census, same denominator/predicate
 * as local presentation admission; 100 only when that gate can open. */
uint32_t lworld_local_loading_percent(void);
/* UI thread. Owner-specific stage/fill; guests remain indeterminate and never
 * sample the host census, import progress or prepared gate. */
int lworld_local_loading_status(unsigned owner, bool generating,
	const char** stage);
/* Shared drawing seam: remote keeps the original heading/activity squares. */
void lworld_draw_loading_status(bool remote, int width, int height,
	float elapsed_s, const char* heading, const char* world_name,
	const char* stage, int percent);

#include "../../network/server_remote.h"

/* The safe core: the player's own column plus its 8 neighbours. Named so the
 * "9" in the gate, the census and the on-screen waiting text are one value. */
#define LWORLD_SAFE_CORE_RADIUS 1
#define LWORLD_SAFE_CORE_COLUMNS 9

/* Provider slot. `fn` is called with `remote` as its first argument, so
 * server_remote_ready_census installs directly with no adapter. Passing NULL
 * clears the slot. Set from the main thread only (server_remote_pump and the
 * tests), read from the main thread only (lworld_prepare_presentation). */
typedef bool (*lworld_ready_census_fn)(struct server_remote* remote, int cx,
									   int cz, int radius,
									   struct server_remote_ready_census* out);
/* Optional companion: returns the session's received-byte counter for the
 * loading screen's stall-progress token (REL-MP-FIX4a). Injected so the screen
 * never links network code; NULL means 0. Cleared together with `fn`. */
typedef uint32_t (*lworld_rx_bytes_fn)(struct server_remote* remote);
void lworld_set_ready_census_provider(lworld_ready_census_fn fn,
									  lworld_rx_bytes_fn rx_fn,
									  struct server_remote* remote);

/* Everything the remote gate decides on, gathered by the caller so the gate
 * itself needs neither gstate nor the world nor the network:
 *  - have_census: the provider answered. False means "the ledger could not be
 *    asked" (no session, or the pump has the slot cleared while it holds the
 *    transport lock -- see SLOT LIFETIME above) -- never treated as "nothing
 *    outstanding".
 *  - core_resident: the remote ledger says all 9 core columns are RESIDENT.
 *  - core_missing / core_mesh_pending: the CLIENT's own radius-1
 *    world_near_ring_status readings -- the same instrument the SP branch
 *    uses, so "meshed" means exactly what it means in single player
 *    (collision-ready, no rebuild or in-flight mesh outstanding).
 *  - announced_pending: in-radius obligations the peer has already announced
 *    for the epoch now loading. */
struct lworld_remote_ready_input {
	bool have_census;
	bool core_resident;
	int core_missing;
	int core_mesh_pending;
	size_t announced_pending;
};

/* REL-MP-LOADHANG: the remote stall escape's clock measures READINESS
 * progress, never liveness. REL-MP-FIX4a fed it a token that also hashed
 * received bytes and the raw mesh-pending count; a live Beta server always
 * sends time updates, keep-alives and entity moves, and near-ring rebuilds
 * churn mesh_pending up and down, so on hw1021f the clock reset every frame
 * and a session whose ledger never settled sat on the loading screen for
 * 300 s. Progress is now only: a completed install (the install serial
 * moved), or a NEW BEST on a readiness count -- more columns loaded, fewer
 * missing, fewer meshes pending, more census-resident, fewer announced
 * obligations. A count that falls back and rises again is churn, not
 * progress. rx_bytes is carried for the log line only.
 * Review r1: the mesh-pending and announced-pending lows RE-BASELINE to the
 * current reading whenever new work lands (an install, more columns
 * loaded, a new census-resident best). A fresh connect primes them at 0
 * before anything has arrived; without the re-baseline the later drain
 * from 326 -> 0 never beats that 0, so a slow mesher would read as a stall
 * and disconnect "Connection stalled". */
struct lworld_remote_progress_input {
	uint32_t installs;		 /* clin_chunk_install_seq() */
	size_t chunks;			 /* world_loaded_chunks() */
	int near_missing;		 /* world_near_ring_status, mp.ready_radius */
	int near_mesh_pending;
	bool have_census;
	size_t census_resident;	 /* server_remote_ready_census.resident_in_radius */
	size_t census_pending;	 /* .announced_pending */
	uint32_t rx_bytes;		 /* liveness only -- never progress */
};
struct lworld_remote_progress {
	bool primed;
	bool census_primed;
	uint32_t installs;
	size_t chunks_high;
	int missing_low;
	int mesh_low;
	size_t resident_high;
	size_t pending_low;
	float no_progress_s;
};
void lworld_remote_progress_reset(struct lworld_remote_progress* p);
/* Returns seconds since the last readiness progress (0 on the priming tick
 * and on any progress tick). */
float lworld_remote_progress_tick(struct lworld_remote_progress* p,
								  const struct lworld_remote_progress_input* in,
								  float dt);

/* REL-MP-LOADHANG hard bound (lead decision 2026-10-21): the remote loading
 * screen may never wait forever against a healthy connection whose ledger
 * never settles. */
#define LWORLD_REMOTE_HARD_CEILING_S 90.0f
enum lworld_remote_hold_action {
	LWORLD_HOLD_WAIT,
	LWORLD_HOLD_STALL_ESCAPE,	/* no readiness progress for the stall timeout */
	LWORLD_HOLD_FORCE_ADMIT,	/* ceiling reached, the client's own core is
								   installed + meshed: admit on it */
	LWORLD_HOLD_CEILING_ESCAPE, /* ceiling reached without that core:
								   disconnect cleanly to the menu */
};
/* Pure. core_present: the client's own radius-1 core has 0 MISSING columns.
 * Deliberately not "meshed": world_near_ring_status counts a dirty display
 * list (rebuild_displist || mesh_in_flight) as mesh-pending, and one applied
 * server block update, a fluid tick or a near-ring remesh makes that true for
 * a frame -- a healthy session must never be disconnected for it (review r1).
 * A present-but-remeshing core latches FORCE_ADMIT; the admission itself
 * (lworld_prepare_presentation) still waits for the core's meshes, so the
 * player never lands on unbuilt collision. admissible: nothing else holds
 * gameplay back (world loaded, no world switch, no terrain hold, no load
 * failure). The stall escape does not fire while the ceiling would ADMIT --
 * that case is a ledger that never settles, not a dead connection. */
enum lworld_remote_hold_action lworld_remote_hold_decide(float time_on_screen_s,
	float ceiling_s, float stalled_s, float stall_timeout_s, bool core_present,
	bool admissible);
/* Process-lifetime count of LWORLD_HOLD_FORCE_ADMIT admissions. */
uint32_t lworld_core_force_admits(void);

/* The remote admission gate (design 2.6). NOT a pure predicate like
 * lworld_near_ring_ready: it owns the ready_timeout_admits telemetry counter
 * and bumps it on the one branch that admits on the clock. Call it once per
 * deciding frame; a true return is latched by lworld_prepare_presentation's
 * s_presentation_prepared, so it cannot double-count an episode.
 *
 *  safe core missing  -> NEVER ready, whatever the clock says. Past the
 *                        timeout *out_waiting_core goes true so the screen can
 *                        say what it is waiting for and keep waiting.
 *  safe core + 0 pending -> ready immediately (the point of M3: loading ends
 *                        on real completion, not on 30 s).
 *  safe core + pending   -> ready only at the timeout (the bounded delay
 *                        Astra allowed), *out_timed_out true, counter bumped.
 *
 * With have_census FALSE the ledger half is simply unknown, so the gate falls
 * back to the client's own world:
 *  no census + world core short    -> NEVER ready; past the timeout
 *                        *out_waiting_core goes true and it keeps waiting,
 *                        identical to the census-in-hand refusal.
 *  no census + world core complete -> not ready before the timeout, then the
 *                        pre-M3 backstop admits (*out_timed_out true, counter
 *                        bumped). Terrain safety is still honoured -- the 9
 *                        core columns are installed and meshed -- but nothing
 *                        is claimed about the peer's outstanding work.
 */
bool lworld_remote_ready(const struct lworld_remote_ready_input* in,
						 float time_on_screen_s, float timeout_s,
						 bool* out_timed_out, bool* out_waiting_core);

/* Cumulative count of gameplay admissions that had to fall back to the 30 s
 * backstop with in-radius obligations still outstanding (design 2.7's
 * mp_ready_pending companion). Process-lifetime, never reset by a
 * presentation reset: it records what happened, not what is pending. */
uint32_t lworld_ready_timeout_admits(void);

/* REL-MP-COREFIRST telemetry: ms after the current/last loading screen
 * appeared at which the remote safe core was first fully installed /
 * installed and meshed (0 = not yet this episode). Either pointer may be
 * NULL. Reset with the presentation episode; kept after admission. */
void lworld_core_timing(uint32_t* out_resident_ms, uint32_t* out_meshed_ms);

/* Which half of the safe core is short. The on-screen text must never claim a
 * complete core while the gate is still refusing, so "9 of 9 columns" is only
 * ever printed together with the reason it is still not enough. */
enum lworld_wait_stage {
	LWORLD_WAIT_COLUMNS,    /* a core SECTION is absent -- the N-of-9 count */
	LWORLD_WAIT_MESHING,    /* all 9 present, geometry/collision still building */
	LWORLD_WAIT_INSTALLING, /* all 9 present and meshed; the remote ledger has
							   not marked every core column RESIDENT yet */
};

/* The visible waiting state (Astra item 9 "fail visibly"): true once the
 * backstop has elapsed while the safe core is still incomplete.
 * *out_core_have receives how many of the 9 core columns the client actually
 * holds and *out_stage which half is short, for the on-screen text. Either
 * out-pointer may be NULL. */
bool lworld_ready_waiting_core(int* out_core_have, int* out_stage);

/* Draw the ordinary loading presentation in the caller's viewport. The caller
 * supplies its own episode clock and terrain count; no host loading state is
 * read or advanced. The viewport origin is established by the caller. */
void lworld_render_loading_viewport(int width, int height,
                                   float elapsed_s, size_t loaded_chunks);

/* TIPS-2: travelling owner's reason is set at the accepted transfer boundary.
 * Caller supplies real elapsed milliseconds since this pane became visible,
 * calls on its first visible frame, and loading_reason_end on pane dismissal.
 * Geometry is local to the caller's already-clipped viewport. */
void lworld_render_loading_owner(unsigned owner, int width, int height,
	uint64_t now_ms, size_t loaded_chunks);

#endif
