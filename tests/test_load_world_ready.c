/*
	M3 remote readiness (mp-split-0912/18).

	Pins the loading screen's REMOTE admission gate and the census it reads:
	the safe core plus currently-announced in-radius obligations, later
	announcements never re-gating, and the 30 s backstop never bypassing a
	missing safe core (design
	docs/mp_admission_budget_design_2026-09-13.md 2.6; Astra gate-2 items 8
	and 9). Single-player readiness is pinned byte-identical to the pre-M3
	code by case F, which diffs lworld_near_ring_ready against a verbatim copy
	of the old function body over an exhaustive input grid.

	The census is driven against a real struct server_remote_impl whose entry
	table is populated by hand (the harness pattern of tests/test_mp_s0_env.c,
	which likewise includes server_remote_internal.h and inspects the impl
	directly). Hand-populating is deliberate: the states this gate cares about
	-- ANNOUNCED without a body, RETAINED behind the promotion gate,
	PROMOTING, and a stale-epoch entry -- are transport-timing-dependent and
	cannot be produced deterministically by feeding wire bytes.
*/

#include "game/gui/screen.h"
#include "game/gui/screen_load_world.h"
#include "network/server_remote.h"
#include "network/server_remote_internal.h"
#include "platform/thread.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- fake remote table ------------------------------------------------- */

static struct server_remote g_handle;
static struct server_remote_impl* g_impl;

static void table_begin(uint32_t applied_epoch) {
	memset(g_impl, 0, sizeof(*g_impl));
	assert(thread_mutex_init(&g_impl->lock));
	g_impl->applied_epoch = applied_epoch;
	g_impl->world_epoch = applied_epoch;
	g_handle.impl = g_impl;
	g_handle.last_status_valid = false;
}

static void table_end(void) {
	thread_mutex_destroy(&g_impl->lock);
	g_handle.impl = NULL;
}

/* Claim the next free slot for (cx, cz) in `state`, stamped with `epoch`.
 * Mirrors what remote_chunk_create_locked establishes for a fresh entry:
 * coordinates, state and epoch. Nothing else in the entry matters to a
 * reader that never touches bodies. */
static void table_put(int cx, int cz, enum remote_chunk_state state,
					  uint32_t epoch) {
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++) {
		if(g_impl->chunks[i].state != REMOTE_CHUNK_UNUSED)
			continue;
		g_impl->chunks[i].state = state;
		g_impl->chunks[i].chunk_x = cx;
		g_impl->chunks[i].chunk_z = cz;
		g_impl->chunks[i].epoch = epoch;
		g_impl->chunks[i].arrival = (uint64_t)(i + 1) << 16;
		return;
	}
	assert(!"entry table full");
}

static void table_put_core_resident(int cx, int cz, uint32_t epoch) {
	for(int dz = -1; dz <= 1; dz++)
		for(int dx = -1; dx <= 1; dx++)
			table_put(cx + dx, cz + dz, REMOTE_CHUNK_RESIDENT, epoch);
}

/* The gate input for a client whose own world already holds the whole core
 * (installed and meshed): the world-side half is then never what blocks, so
 * every case below isolates the REMOTE half of the predicate. */
static struct lworld_remote_ready_input gate_input(
	const struct server_remote_ready_census* c, bool have) {
	struct lworld_remote_ready_input in;
	memset(&in, 0, sizeof(in));
	in.have_census = have;
	in.core_resident = have && c->core_resident;
	in.announced_pending = have ? c->announced_pending : 0;
	in.core_missing = 0;
	in.core_mesh_pending = 0;
	return in;
}

#define TIMEOUT_S 30.0f
#define BEFORE_TIMEOUT_S 0.5f
#define PAST_TIMEOUT_S 31.0f

/* ---- A: safe core resident, nothing announced -> ready immediately ------ */

static void case_a_core_ready(void) {
	table_begin(3);
	table_put_core_resident(10, -4, 3);
	struct server_remote_ready_census c;
	assert(server_remote_ready_census(&g_handle, 10, -4, 2, &c));
	assert(c.core_resident && c.core_announced == 9);
	assert(c.announced_pending == 0 && c.resident_in_radius == 9);
	struct lworld_remote_ready_input in = gate_input(&c, true);
	bool timed_out = true, waiting = true;
	/* Ready at HALF A SECOND -- this is the M3 outcome: the join ends on real
	 * completion, not on the 30 s clock the A4 measurement found. */
	assert(lworld_remote_ready(&in, BEFORE_TIMEOUT_S, TIMEOUT_S, &timed_out,
							   &waiting));
	assert(!timed_out && !waiting);
	assert(lworld_ready_timeout_admits() == 0);
	/* And an unarmed provider is never mistaken for "nothing outstanding". */
	struct lworld_remote_ready_input none = gate_input(&c, false);
	assert(!lworld_remote_ready(&none, BEFORE_TIMEOUT_S, TIMEOUT_S, NULL,
								NULL));
	table_end();
	printf("PASS A: core resident + no announcements -> ready at %.1fs "
		   "(core_announced=%d resident_in_radius=%zu); no census -> refused\n",
		   (double)BEFORE_TIMEOUT_S, c.core_announced, c.resident_in_radius);
}

/* ---- B: one outstanding announcement, inside then outside the radius ---- */

static void case_b_radius(void) {
	/* Inside radius 2: a RETAINED body still waiting for the promotion pump. */
	table_begin(1);
	table_put_core_resident(0, 0, 1);
	table_put(2, 0, REMOTE_CHUNK_RETAINED, 1);
	struct server_remote_ready_census in_r;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &in_r));
	assert(in_r.core_resident && in_r.announced_pending == 1);
	struct lworld_remote_ready_input gin = gate_input(&in_r, true);
	assert(!lworld_remote_ready(&gin, BEFORE_TIMEOUT_S, TIMEOUT_S, NULL, NULL));
	/* PROMOTING and ANNOUNCED are obligations too -- all three pending
	 * states, not just the one with a body in hand. */
	table_put(-1, 2, REMOTE_CHUNK_PROMOTING, 1);
	table_put(0, -2, REMOTE_CHUNK_ANNOUNCED, 1);
	struct server_remote_ready_census three;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &three));
	assert(three.announced_pending == 3);
	table_end();

	/* The SAME entry one column farther out is not an in-radius obligation. */
	table_begin(1);
	table_put_core_resident(0, 0, 1);
	table_put(3, 0, REMOTE_CHUNK_RETAINED, 1);
	struct server_remote_ready_census out_r;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &out_r));
	assert(out_r.core_resident && out_r.announced_pending == 0);
	struct lworld_remote_ready_input gout = gate_input(&out_r, true);
	bool timed_out = true, waiting = true;
	assert(lworld_remote_ready(&gout, BEFORE_TIMEOUT_S, TIMEOUT_S, &timed_out,
							   &waiting));
	assert(!timed_out && !waiting);
	/* A wider dial pulls it back in: the radius is the dial, not the gate. */
	struct server_remote_ready_census wide;
	assert(server_remote_ready_census(&g_handle, 0, 0, 3, &wide));
	assert(wide.announced_pending == 1);
	table_end();
	assert(lworld_ready_timeout_admits() == 0);
	printf("PASS B: RETAINED at Chebyshev 2 inside radius 2 -> not ready "
		   "(pending=%zu, all 3 pending states counted=%zu); the same entry at "
		   "3 -> ready (pending=%zu, radius 3 -> %zu)\n",
		   in_r.announced_pending, three.announced_pending,
		   out_r.announced_pending, wide.announced_pending);
}

/* ---- C: safe core short -> the backstop must NOT admit ----------------- */

static void case_c_core_never_bypassed(void) {
	table_begin(0);
	/* Eight of nine: the column north-west of the player never arrived. */
	for(int dz = -1; dz <= 1; dz++)
		for(int dx = -1; dx <= 1; dx++) {
			if(dx == -1 && dz == -1)
				continue;
			table_put(dx, dz, REMOTE_CHUNK_RESIDENT, 0);
		}
	struct server_remote_ready_census c;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &c));
	assert(!c.core_resident && c.core_announced == 8);
	assert(c.announced_pending == 0); /* nothing outstanding -- and still not ready */
	struct lworld_remote_ready_input in = gate_input(&c, true);
	bool timed_out = true, waiting = false;
	assert(!lworld_remote_ready(&in, PAST_TIMEOUT_S, TIMEOUT_S, &timed_out,
								&waiting));
	assert(!timed_out);   /* a refusal is never reported as a timeout admit */
	assert(waiting);      /* ... it is reported as the visible waiting state */
	/* Still refused far past the backstop, and the waiting state persists. */
	waiting = false;
	assert(!lworld_remote_ready(&in, 600.0f, TIMEOUT_S, NULL, &waiting));
	assert(waiting);
	/* Before the backstop the screen shows the ordinary "Building terrain" --
	 * the waiting state is the ESCALATION, not the default. */
	waiting = true;
	assert(!lworld_remote_ready(&in, BEFORE_TIMEOUT_S, TIMEOUT_S, NULL,
								&waiting));
	assert(!waiting);
	/* A resident ledger whose WORLD side is short is refused just the same:
	 * an absent section, and a section still meshing. */
	table_end();
	table_begin(0);
	table_put_core_resident(0, 0, 0);
	struct server_remote_ready_census full;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &full));
	assert(full.core_resident);
	struct lworld_remote_ready_input wmiss = gate_input(&full, true);
	wmiss.core_missing = 1;
	assert(!lworld_remote_ready(&wmiss, PAST_TIMEOUT_S, TIMEOUT_S, NULL, NULL));
	struct lworld_remote_ready_input wmesh = gate_input(&full, true);
	wmesh.core_mesh_pending = 1;
	assert(!lworld_remote_ready(&wmesh, PAST_TIMEOUT_S, TIMEOUT_S, NULL, NULL));
	table_end();
	assert(lworld_ready_timeout_admits() == 0);
	printf("PASS C: 8 of 9 core columns resident -> refused at %.1fs and at "
		   "600.0s with the waiting state set (quiet before the backstop); "
		   "world-side core_missing=1 and core_mesh_pending=1 refused too\n",
		   (double)PAST_TIMEOUT_S);
}

/* ---- D: safe core + announced pending + backstop -> bounded admit ------ */

static void case_d_bounded_admit(void) {
	table_begin(7);
	table_put_core_resident(-32, 64, 7);
	table_put(-30, 64, REMOTE_CHUNK_RETAINED, 7);
	table_put(-32, 66, REMOTE_CHUNK_ANNOUNCED, 7);
	struct server_remote_ready_census c;
	assert(server_remote_ready_census(&g_handle, -32, 64, 2, &c));
	assert(c.core_resident && c.announced_pending == 2);
	struct lworld_remote_ready_input in = gate_input(&c, true);
	/* Not before the backstop. */
	bool timed_out = true, waiting = true;
	assert(!lworld_remote_ready(&in, 29.9f, TIMEOUT_S, &timed_out, &waiting));
	assert(!timed_out && !waiting);
	assert(lworld_ready_timeout_admits() == 0);
	/* At the backstop: admitted, flagged, counted once. */
	assert(lworld_remote_ready(&in, PAST_TIMEOUT_S, TIMEOUT_S, &timed_out,
							   &waiting));
	assert(timed_out && !waiting);
	assert(lworld_ready_timeout_admits() == 1);
	table_end();
	printf("PASS D: core resident + %zu announced in radius 2 -> held at "
		   "29.9s, admitted at %.1fs with ready_timeout_admits=%u\n",
		   c.announced_pending, (double)PAST_TIMEOUT_S,
		   lworld_ready_timeout_admits());
}

/* ---- E: stale-epoch entries are not obligations ------------------------ */

static void case_e_stale_epoch(void) {
	table_begin(4);
	table_put_core_resident(0, 0, 4);
	/* The read thread retained the NEXT dimension's bodies at the RESPAWN's
	 * stream position, before the consumer applied that epoch. They are not
	 * obligations of the episode now loading. */
	table_put(1, 1, REMOTE_CHUNK_RETAINED, 5);
	table_put(-2, 0, REMOTE_CHUNK_ANNOUNCED, 5);
	table_put(0, 2, REMOTE_CHUNK_PROMOTING, 3); /* and a PREVIOUS epoch */
	struct server_remote_ready_census c;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &c));
	assert(c.core_resident && c.announced_pending == 0);
	struct lworld_remote_ready_input in = gate_input(&c, true);
	bool timed_out = true;
	assert(lworld_remote_ready(&in, BEFORE_TIMEOUT_S, TIMEOUT_S, &timed_out,
							   NULL));
	assert(!timed_out);
	/* Once the consumer applies epoch 5, the same two entries ARE obligations
	 * -- proof that the filter is the epoch, not the coordinate. */
	g_impl->applied_epoch = 5;
	struct server_remote_ready_census after;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &after));
	assert(after.announced_pending == 2);
	table_end();
	/* No live impl: refused, zero-filled, never "nothing outstanding". */
	struct server_remote_ready_census dead;
	memset(&dead, 0xAB, sizeof(dead));
	assert(!server_remote_ready_census(&g_handle, 0, 0, 2, &dead));
	assert(dead.announced_pending == 0 && !dead.core_resident
		   && dead.core_announced == 0 && dead.resident_in_radius == 0);
	assert(!server_remote_ready_census(NULL, 0, 0, 2, &dead));
	assert(lworld_ready_timeout_admits() == 1);
	/* Extreme wire coordinates: the distance math must not overflow signed
	 * int (chunk_x/_z are int32 straight off Packet50/51). Both entries are
	 * astronomically far from the player, so neither may be counted as an
	 * in-radius obligation nor corrupt the core grid. */
	table_begin(0);
	table_put_core_resident(0, 0, 0);
	table_put(INT32_MAX, INT32_MAX, REMOTE_CHUNK_RETAINED, 0);
	table_put(INT32_MIN, INT32_MIN, REMOTE_CHUNK_ANNOUNCED, 0);
	struct server_remote_ready_census wild;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &wild));
	assert(wild.core_resident && wild.core_announced == 9);
	assert(wild.announced_pending == 0 && wild.resident_in_radius == 9);
	/* ... and centring the scan ON one of them still reads a sane census. */
	struct server_remote_ready_census at_edge;
	assert(server_remote_ready_census(&g_handle, INT32_MAX, INT32_MAX, 2,
									  &at_edge));
	assert(!at_edge.core_resident && at_edge.core_announced == 1);
	assert(at_edge.announced_pending == 1);
	table_end();
	printf("PASS E: entries stamped epoch 5 and 3 ignored at applied_epoch=4 "
		   "(pending=%zu); after applying epoch 5 -> pending=%zu; no live "
		   "impl -> census refused and zero-filled; INT32_MIN/MAX coordinates "
		   "counted sanely from both centres\n",
		   c.announced_pending, after.announced_pending);
}

/* ---- F: single-player gate is byte-identical to the pre-M3 code -------- */

/* VERBATIM copy of lworld_near_ring_ready's body as it stood at 8a3fe626,
 * before this slice (screen_load_world.c:160-166). The local branch of
 * lworld_prepare_presentation still calls the production function with the
 * same arguments it always did, so proving the function itself is unchanged
 * over its whole input domain proves SP admission is unchanged. */
static bool near_ring_ready_reference(int near_missing, float time_on_screen_s,
									  float timeout_s, bool* out_timed_out) {
	bool timed_out = time_on_screen_s >= timeout_s;
	if(out_timed_out)
		*out_timed_out = timed_out && near_missing != 0;
	return near_missing == 0 || timed_out;
}

static void case_f_sp_identical(void) {
	static const float times[]
		= {0.0f, 0.016f, 1.0f, 15.0f, 29.999f, 30.0f, 30.001f, 120.0f};
	static const int missings[] = {0, 1, 2, 30, 411, 441};
	unsigned checked = 0;
	for(size_t t = 0; t < sizeof(times) / sizeof(times[0]); t++)
		for(size_t m = 0; m < sizeof(missings) / sizeof(missings[0]); m++) {
			bool got_to = true, ref_to = false;
			bool got = lworld_near_ring_ready(missings[m], times[t], TIMEOUT_S,
											  &got_to);
			bool ref = near_ring_ready_reference(missings[m], times[t],
												 TIMEOUT_S, &ref_to);
			assert(got == ref);
			assert(got_to == ref_to);
			/* NULL out-param path too. */
			assert(lworld_near_ring_ready(missings[m], times[t], TIMEOUT_S,
										  NULL)
				   == near_ring_ready_reference(missings[m], times[t],
												TIMEOUT_S, NULL));
			checked++;
		}
	/* The SP gate is also unaffected by anything M3 added: no provider is
	 * armed here, and the counter did not move. */
	assert(lworld_ready_timeout_admits() == 1);
	printf("PASS F: lworld_near_ring_ready identical to the pre-M3 reference "
		   "over %u (near_missing x time) points, return and out_timed_out\n",
		   checked);
}

/* ---- presentation-reset invariant (Astra item 9) ----------------------- */

static void case_reset_keeps_obligations(void) {
	table_begin(2);
	table_put_core_resident(5, 5, 2);
	table_put(7, 5, REMOTE_CHUNK_RETAINED, 2);
	struct server_remote_ready_census before;
	assert(server_remote_ready_census(&g_handle, 5, 5, 2, &before));
	uint32_t admits_before = lworld_ready_timeout_admits();
	lworld_reset_presentation();
	struct server_remote_ready_census after;
	assert(server_remote_ready_census(&g_handle, 5, 5, 2, &after));
	assert(memcmp(&before, &after, sizeof(before)) == 0);
	assert(lworld_ready_timeout_admits() == admits_before);
	int have = -1, stage = -1;
	assert(!lworld_ready_waiting_core(&have, &stage));
	assert(have == 0 && stage == LWORLD_WAIT_COLUMNS);
	table_end();
	printf("  presentation reset: census byte-identical across "
		   "lworld_reset_presentation (pending=%zu), cumulative "
		   "ready_timeout_admits=%u preserved, waiting state cleared\n",
		   after.announced_pending, admits_before);
}

/* ---- G: no provider armed -- the window the deadlock fix creates -------
 *
 * REGRESSION for the 2026-09-13 self-deadlock (F3). The first W3 land armed
 * the census provider at the TOP of server_remote_pump, so the slot was armed
 * across the pump's own locked tail -- and that tail delivers
 * FINISH_REMOTE_LOAD synchronously under impl->lock
 * (remote_emit_load_completion_locked -> clin_process_direct ->
 * clin_process_observed -> screen_set(&screen_ingame) ->
 * lworld_admit_gameplay -> lworld_prepare_presentation), whose remote branch
 * then called server_remote_ready_census, which re-locked the same
 * non-recursive mutex. The main thread hung for good on the FIRST remote
 * world load; tests/test_mp_s0_env.c's test_mc173_partial_join_burst is the
 * end-to-end guard (it wedged forever at d75443fb) and this case pins the
 * gate half of the fix.
 *
 * server_remote_pump now clears the slot for its whole body and installs it
 * after its last unlock, so "remote session, no provider armed" is a NORMAL
 * transient state and the gate must give it defined, bounded behaviour: never
 * ready on the ledger's word (there is none), the clock still running, and
 * the CLIENT's own safe core as the only thing that may release the screen at
 * the backstop. A missing core still waits visibly, forever if need be. */

static void case_g_no_provider(void) {
	uint32_t admits_before = lworld_ready_timeout_admits();

	/* G1: no census, but the client's own 9 core columns are installed AND
	 * meshed -- the pre-M3 backstop still admits, and only at the backstop. */
	struct lworld_remote_ready_input core_ok;
	memset(&core_ok, 0, sizeof(core_ok));
	/* have_census stays false; core_missing/core_mesh_pending are 0. */
	bool timed_out = true, waiting = true;
	assert(!lworld_remote_ready(&core_ok, BEFORE_TIMEOUT_S, TIMEOUT_S,
							    &timed_out, &waiting));
	assert(!timed_out && !waiting); /* quiet while the clock runs */
	assert(lworld_ready_timeout_admits() == admits_before);
	assert(!lworld_remote_ready(&core_ok, 29.999f, TIMEOUT_S, NULL, NULL));
	assert(lworld_remote_ready(&core_ok, TIMEOUT_S, TIMEOUT_S, &timed_out,
							   &waiting));
	assert(timed_out && !waiting);
	assert(lworld_ready_timeout_admits() == admits_before + 1);

	/* G2: no census and a core SECTION absent -- the backstop may not bypass
	 * it. Refused at the backstop and far past it, with the visible waiting
	 * state set, exactly as the census-in-hand refusal behaves (case C). */
	struct lworld_remote_ready_input core_missing = core_ok;
	core_missing.core_missing = 1;
	timed_out = true;
	waiting = false;
	assert(!lworld_remote_ready(&core_missing, PAST_TIMEOUT_S, TIMEOUT_S,
								&timed_out, &waiting));
	assert(!timed_out && waiting);
	waiting = false;
	assert(!lworld_remote_ready(&core_missing, 600.0f, TIMEOUT_S, NULL,
								&waiting));
	assert(waiting);
	/* ... and quiet before the backstop: the waiting state is the escalation. */
	waiting = true;
	assert(!lworld_remote_ready(&core_missing, BEFORE_TIMEOUT_S, TIMEOUT_S,
								NULL, &waiting));
	assert(!waiting);

	/* G3: no census, all 9 sections present but geometry/collision still
	 * building -- not safe ground yet, so it waits just the same. */
	struct lworld_remote_ready_input core_meshing = core_ok;
	core_meshing.core_mesh_pending = 3;
	waiting = false;
	assert(!lworld_remote_ready(&core_meshing, PAST_TIMEOUT_S, TIMEOUT_S, NULL,
								&waiting));
	assert(waiting);

	/* Neither refusal touched the telemetry counter -- only G1's admit did. */
	assert(lworld_ready_timeout_admits() == admits_before + 1);

	/* And the ledger, when it IS available, still overrides a complete client
	 * world: a census that says the core is not RESIDENT is refused past the
	 * backstop even though core_missing/core_mesh_pending are 0. The no-census
	 * fallback must not have become a way to admit on local terrain the peer
	 * never delivered. */
	table_begin(1);
	for(int dz = -1; dz <= 1; dz++)
		for(int dx = -1; dx <= 1; dx++) {
			if(dx == 1 && dz == 1)
				continue;
			table_put(dx, dz, REMOTE_CHUNK_RESIDENT, 1);
		}
	struct server_remote_ready_census short_core;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &short_core));
	assert(!short_core.core_resident);
	struct lworld_remote_ready_input ledger_short
		= gate_input(&short_core, true);
	waiting = false;
	assert(!lworld_remote_ready(&ledger_short, PAST_TIMEOUT_S, TIMEOUT_S, NULL,
								&waiting));
	assert(waiting);
	table_end();
	assert(lworld_ready_timeout_admits() == admits_before + 1);

	printf("PASS G: no provider armed (the window server_remote_pump keeps "
		   "while it holds impl->lock) -> held at 29.999s, admitted at the "
		   "%.1fs backstop only with the client's own core installed+meshed "
		   "(admits %u -> %u); core_missing=1 and core_mesh_pending=3 wait "
		   "visibly at 600.0s instead; an available census saying "
		   "core_resident=false still refuses\n",
		   (double)TIMEOUT_S, admits_before, lworld_ready_timeout_admits());
}

/* ---- H: M6 -- the census clamps the ready radius to the admit radius --- */
/* mp.ready_radius was clamped only to [1, MAX_VIEW_DISTANCE]. With
 * mp.install_radius = 2 and mp.ready_radius = 5, every entry at Chebyshev 3-5
 * counted as an obligation the admission gate refuses BY DESIGN, so
 * announced_pending could never reach 0 and every join was released by the
 * 30 s backstop -- the exact defect this gate exists to remove (v1 hostile
 * review M6). The clamp lives in the census because that is the only reader
 * holding both numbers. */
static void case_h_ready_radius_clamped(void) {
	table_begin(1);
	table_put_core_resident(0, 0, 1);
	/* One announced column at Chebyshev 4: outside install radius 2, so the
	 * gate will never install it. */
	table_put(4, 0, REMOTE_CHUNK_RETAINED, 1);
	g_impl->admit_radius = 2;
	struct server_remote_ready_census c;
	assert(server_remote_ready_census(&g_handle, 0, 0, 5, &c));
	assert(c.admit_radius == 2 && c.radius_used == 2
		   && "the requested radius 5 was clamped to the install radius");
	assert(c.announced_pending == 0
		   && "so the unreachable column is not an obligation");
	struct lworld_remote_ready_input in = gate_input(&c, true);
	bool timed_out = true, waiting = true;
	assert(lworld_remote_ready(&in, BEFORE_TIMEOUT_S, TIMEOUT_S, &timed_out,
							   &waiting));
	assert(!timed_out && !waiting
		   && "the join completes on real readiness, not the backstop");
	uint32_t admits_after_clamp = lworld_ready_timeout_admits();
	/* Unclamped (admit_radius unset, as in a hand-built impl), the same
	 * table at radius 5 DOES see the obligation -- i.e. the clamp is what
	 * changed the outcome, not the fixture. */
	g_impl->admit_radius = 0;
	struct server_remote_ready_census wide;
	assert(server_remote_ready_census(&g_handle, 0, 0, 5, &wide));
	assert(wide.admit_radius == 0 && wide.radius_used == 5);
	assert(wide.announced_pending == 1);
	struct lworld_remote_ready_input held = gate_input(&wide, true);
	assert(!lworld_remote_ready(&held, BEFORE_TIMEOUT_S, TIMEOUT_S, NULL,
								NULL));
	/* An in-radius column is still an obligation after the clamp: the clamp
	 * removes the unsatisfiable half only. */
	g_impl->admit_radius = 2;
	table_put(2, 0, REMOTE_CHUNK_ANNOUNCED, 1);
	struct server_remote_ready_census inr;
	assert(server_remote_ready_census(&g_handle, 0, 0, 5, &inr));
	assert(inr.radius_used == 2 && inr.announced_pending == 1);
	/* L12: the census centres on the SERVER's admission centre when it has
	 * one, so the screen is judged against the disk the gate is filling. */
	remote_admission_centre_set(g_impl, 0.0, 0.0, true);
	struct server_remote_ready_census centred;
	assert(server_remote_ready_census(&g_handle, 40, 40, 5, &centred));
	assert(centred.centre_authoritative && centred.centre_x == 0
		   && centred.centre_z == 0
		   && "a camera column 40 away does not move the census");
	assert(centred.core_resident && centred.announced_pending == 1);
	assert(lworld_ready_timeout_admits() == admits_after_clamp
		   && "no case above reached the backstop");
	table_end();
	printf("PASS H: ready_radius 5 clamped to install radius 2 -> the "
		   "Chebyshev-4 column is not an obligation and the join completes at "
		   "%.1fs; unclamped it still holds (pending 1); the in-radius "
		   "announcement still counts; census centres on the server's "
		   "admission centre, not the camera\n",
		   (double)BEFORE_TIMEOUT_S);
}

/* ---- I: INCOMPLETE delivery still admits without the backstop ----------
 *
 * push-0921/B4c item 5(b) (Astra gate 1 Lane B item 1: "add explicit MP
 * delayed/incomplete-delivery and dimension-generation cases").
 *
 * The A4 measurement (tmp/jobs/mp-split-0912/m3-readiness-report.md, quoted
 * in lworld_remote_ready's own comment) is the case this pins: the client
 * scanned 441 columns while the peer's view loop had committed to far fewer,
 * so near_missing sat at 411 and EVERY join was released by the 30 s
 * backstop. The M3 gate's answer is that an undelivered column the peer never
 * announced inside the admit radius is NOT an obligation -- so a session
 * whose wider disk is still mostly empty admits IMMEDIATELY on the safe core,
 * and the backstop counter never moves.
 *
 * Deliberately built at that scale (289 delivered of a 441-column ready
 * radius) rather than with a toy table: the defect it guards was a
 * count-driven one, and a 3x3 fixture cannot show a 441-column scan reaching
 * zero obligations.
 *
 * The OTHER half of the pair -- delayed delivery, i.e. the safe core itself
 * still short past 30 s -- is already pinned by case C above (census in hand,
 * 8 of 9 core columns, refused at 31.0s and at 600.0s, counter unchanged) and
 * by case G's ledger_short leg, so it is CITED rather than duplicated here. */
static void case_i_incomplete_delivery(void) {
	uint32_t admits_before = lworld_ready_timeout_admits();
	table_begin(11);
	/* 17x17 = 289 columns delivered around the player, RESIDENT. */
	int delivered = 0;
	for(int dz = -8; dz <= 8; dz++)
		for(int dx = -8; dx <= 8; dx++) {
			table_put(dx, dz, REMOTE_CHUNK_RESIDENT, 11);
			delivered++;
		}
	assert(delivered == 289);
	/* The rest of a 21x21 (441-column) ready disk has not been announced at
	 * all -- the peer's own view loop has not got to it. Nothing represents
	 * those 152 columns in the ledger, which is exactly the point: absence is
	 * not an obligation. What IS in the table beyond the admit radius is a
	 * handful of announcements the gate must not count either. */
	g_impl->admit_radius = 2;
	table_put(9, 0, REMOTE_CHUNK_ANNOUNCED, 11);
	table_put(0, -9, REMOTE_CHUNK_RETAINED, 11);
	table_put(-10, 6, REMOTE_CHUNK_PROMOTING, 11);
	struct server_remote_ready_census c;
	/* Radius 10 = the 441-column disk the A4 client scanned. */
	assert(server_remote_ready_census(&g_handle, 0, 0, 10, &c));
	assert(c.radius_used == 2 && c.admit_radius == 2);
	assert(c.core_resident && c.core_announced == 9);
	assert(c.announced_pending == 0
		   && "an un-announced column is not an obligation");
	assert(c.resident_in_radius == 25 /* the clamped 5x5 disk */);
	struct lworld_remote_ready_input in = gate_input(&c, true);
	bool timed_out = true, waiting = true;
	/* Admitted at HALF A SECOND, without the backstop, with 152 columns of
	 * the scanned disk still undelivered. */
	assert(lworld_remote_ready(&in, BEFORE_TIMEOUT_S, TIMEOUT_S, &timed_out,
							   &waiting));
	assert(!timed_out && !waiting);
	assert(lworld_ready_timeout_admits() == admits_before
		   && "a normal release must not touch the backstop counter");
	/* ... and it is still a normal release at 29.9 s, i.e. the admission is
	 * on readiness, not on a clock that happened not to have expired. */
	assert(lworld_remote_ready(&in, 29.9f, TIMEOUT_S, &timed_out, &waiting));
	assert(!timed_out && !waiting);
	assert(lworld_ready_timeout_admits() == admits_before);
	table_end();
	printf("PASS I: %d of 441 scanned columns delivered, safe core resident, "
		   "announced_pending=%zu -> admitted at %.1fs WITHOUT the backstop; "
		   "ready_timeout_admits unchanged at %u\n",
		   delivered, c.announced_pending, (double)BEFORE_TIMEOUT_S,
		   lworld_ready_timeout_admits());
}

/* ---- J: a second install / dimension change re-arms the gate ----------
 *
 * push-0921/B4c item 5(d). mp_play_10m installs a remote world TWICE in one
 * process (join, play, leave, rejoin), and the collector requires the
 * backstop counter to be 0 from the first heartbeat to the last -- across
 * BOTH generations. Two claims have to hold for that to mean anything:
 *
 *   1. the gate RE-ARMS: the second install is judged from scratch, so an
 *      admission already granted in generation 1 can never release
 *      generation 2's loading screen; and
 *   2. the counter is CUMULATIVE across installs -- it records what
 *      happened, so a backstop admission in generation 1 cannot be erased by
 *      the reset that starts generation 2 (which is what would let a run
 *      report "no backstop" after taking one).
 *
 * The episode boundary modelled here is lworld_reset_presentation(), which is
 * what screen_lworld_reset() calls on every fresh visit to the loading
 * screen; the LEDGER boundary is a new applied_epoch with an empty table,
 * which is what the read thread leaves behind at a dimension epoch change
 * (see lworld_reset_presentation's own comment: the entry table is cleared
 * only by the read thread at a dimension epoch change and by
 * server_remote_cancel). Counts are asserted as DELTAS because the counter is
 * process-cumulative by design and earlier cases in this file have already
 * moved it. */
static void case_j_second_install_rearms(void) {
	const uint32_t base = lworld_ready_timeout_admits();

	/* -- generation 1: core resident, one announced column outstanding, so
	 *    this install is released by the backstop and counted. */
	table_begin(20);
	table_put_core_resident(0, 0, 20);
	table_put(2, 0, REMOTE_CHUNK_ANNOUNCED, 20);
	struct server_remote_ready_census g1;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &g1));
	assert(g1.core_resident && g1.announced_pending == 1);
	struct lworld_remote_ready_input in1 = gate_input(&g1, true);
	assert(!lworld_remote_ready(&in1, BEFORE_TIMEOUT_S, TIMEOUT_S, NULL, NULL));
	bool timed_out = false;
	assert(lworld_remote_ready(&in1, PAST_TIMEOUT_S, TIMEOUT_S, &timed_out,
							   NULL));
	assert(timed_out);
	assert(lworld_ready_timeout_admits() == base + 1);
	table_end();

	/* -- the episode boundary: the screen resets its presentation, and the
	 *    read thread has cleared the ledger for the new dimension epoch. */
	lworld_reset_presentation();
	assert(lworld_ready_timeout_admits() == base + 1
		   && "the reset records history, it does not un-record it");
	int have = -1, stage = -1;
	assert(!lworld_ready_waiting_core(&have, &stage));

	/* -- generation 2, frame 0: NOTHING of the new dimension has arrived. The
	 *    gate must re-arm -- generation 1's admission grants nothing here --
	 *    and it must refuse past the backstop, because the safe core is the
	 *    floor the backstop may never cross (Astra item 9). */
	table_begin(21);
	struct server_remote_ready_census g2_empty;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &g2_empty));
	assert(!g2_empty.core_resident && g2_empty.core_announced == 0);
	struct lworld_remote_ready_input in2 = gate_input(&g2_empty, true);
	bool waiting = false;
	assert(!lworld_remote_ready(&in2, BEFORE_TIMEOUT_S, TIMEOUT_S, NULL,
								&waiting));
	assert(!waiting); /* quiet before the backstop */
	assert(!lworld_remote_ready(&in2, PAST_TIMEOUT_S, TIMEOUT_S, NULL,
								&waiting));
	assert(waiting); /* and visibly waiting past it, not admitted */
	assert(!lworld_remote_ready(&in2, 600.0f, TIMEOUT_S, NULL, NULL));
	assert(lworld_ready_timeout_admits() == base + 1
		   && "a refused second install must not count an admission");

	/* -- generation 2, the new dimension's core arrives with one column of
	 *    the disk still announced: held before the backstop, admitted at it,
	 *    and the counter CONTINUES rather than restarting. */
	table_put_core_resident(0, 0, 21);
	table_put(-2, 1, REMOTE_CHUNK_RETAINED, 21);
	struct server_remote_ready_census g2;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &g2));
	assert(g2.core_resident && g2.announced_pending == 1);
	struct lworld_remote_ready_input in3 = gate_input(&g2, true);
	assert(!lworld_remote_ready(&in3, 29.9f, TIMEOUT_S, NULL, NULL));
	timed_out = false;
	assert(lworld_remote_ready(&in3, PAST_TIMEOUT_S, TIMEOUT_S, &timed_out,
							   NULL));
	assert(timed_out);
	assert(lworld_ready_timeout_admits() == base + 2
		   && "cumulative across installs -- generation 2's backstop admit is "
			  "counted on top of generation 1's, never instead of it");
	table_end();

	/* -- a THIRD install that is genuinely ready (nothing announced) admits
	 *    immediately and leaves the counter alone: the counter measures
	 *    backstop admissions, not installs. This is the shape BOTH
	 *    generations of a passing mp_play_10m run must have. */
	lworld_reset_presentation();
	table_begin(22);
	table_put_core_resident(0, 0, 22);
	struct server_remote_ready_census g3;
	assert(server_remote_ready_census(&g_handle, 0, 0, 2, &g3));
	assert(g3.core_resident && g3.announced_pending == 0);
	struct lworld_remote_ready_input in4 = gate_input(&g3, true);
	timed_out = true;
	waiting = true;
	assert(lworld_remote_ready(&in4, BEFORE_TIMEOUT_S, TIMEOUT_S, &timed_out,
							   &waiting));
	assert(!timed_out && !waiting);
	assert(lworld_ready_timeout_admits() == base + 2);
	table_end();

	printf("PASS J: install 2 re-arms (an empty new-epoch ledger is refused at "
		   "%.1fs and at 600.0s despite install 1 having been admitted), the "
		   "counter is cumulative across installs (%u -> %u -> %u), and a "
		   "clean third install admits at %.1fs without moving it\n",
		   (double)PAST_TIMEOUT_S, base, base + 1,
		   lworld_ready_timeout_admits(), (double)BEFORE_TIMEOUT_S);
}

int main(void) {
	g_impl = malloc(sizeof(*g_impl));
	assert(g_impl);
	case_a_core_ready();
	case_b_radius();
	case_c_core_never_bypassed();
	case_d_bounded_admit();
	case_e_stale_epoch();
	case_f_sp_identical();
	case_reset_keeps_obligations();
	case_g_no_provider();
	case_h_ready_radius_clamped();
	case_i_incomplete_delivery();
	case_j_second_install_rearms();
	free(g_impl);
	printf("test_load_world_ready: all cases passed\n");
	return 0;
}
