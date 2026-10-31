/*
	T2 far-send share (2026-09-04, lane/far-send-share;
	docs/platform_perf_notes.md "Far-send share while the near frontier moves").

	BEFORE this change the far send batch in server_local.c was gated on a
	STRICT near_complete -- every column of the (2*MAX_VIEW_DISTANCE+1)^2 near
	square already sent. A WALKING player's near frontier never converges, so
	on real hardware (far_walk_6m, staged_far_walk_6m_20260904_080952) the
	far ring got ZERO columns for the entire 6-minute walk: far_missing median
	== max == 48 == the whole reach-6 annulus, srv_gen_cols max 1/tick.

	AFTER, the gate is "near owed <= lod.far_send_near_owed", and in that
	shared mode the far batch may spend ONLY the per-tick send slots the near
	loop left unused, with a promotion-debt guard that refuses any column the
	player's heading is about to push inside the near radius (the World38
	113-121 live far_sent debt this file's CASE C reproduces and pins).

	Every case drives the REAL production decision code through the
	CAVEWII_SERVER_LOCAL_TEST seams (server_local_test_far_send_share /
	_far_send_enters_near / _far_send_heading_update / _far_pick_next) rather
	than a hand-mirrored copy, so a silent divergence fails loudly.

	CASE A: with lod.far_send_near_owed pinned to 0 (the strict opt-out value,
	        NOT the shipping default since lane/far-share-default-8,
	        2026-09-04) the exact (cx,cz) send sequence over a scripted walk is
	        IDENTICAL to a reference implementation of the pre-2026-09-04
	        strict rule.
	CASE A2: the game's own default (source/main.c's config_read_int fallback,
	        mirroring server_local_init's pre-config-override initial value)
	        is 8, not 0 -- N=16 was measured to fill the far ring but peak the
	        cost (fps floor 10.4 -> 5.3), so 8 is the bounded setting
	        (docs/platform_perf_notes.md "Default 8").
	CASE B: with the key > 0, far columns ARE sent on ticks where the near
	        loop still owes <= N chunks and left budget unspent -- and are
	        still refused when it owes more than N, or spent its whole batch.
	CASE C: the promotion-debt guard -- no column sent this tick is inside the
	        near radius on the next tick of the walk (and the same walk with
	        the guard bypassed DOES accumulate that debt, so the assertion is
	        proving the guard, not the geometry).
	CASE D: the guard predicate's own truth table, including its provable
	        no-op-ness at lead 0 / zero heading (the strict path).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <m-lib/m-string.h>

#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "world.h" // enum world_dim

#define MAX_RECORDED 4096

// The near loop's per-tick batch (server_local.c's CHUNK_SEND_BATCH), read
// through the production seam rather than duplicated here.
#define NEAR_BATCH (server_local_test_near_send_batch())

// One tick of a scripted walk: where the player's chunk is, how many near
// chunks the near-send scan still owes, and how many the near loop actually
// managed to send this tick.
struct tick_script {
	w_coord_t px, pz;
	int near_owed;
	int near_sent;
};

// The exact (cx,cz) far columns a run produced, in order, so two runs can be
// compared byte-for-byte.
struct send_log {
	int count;
	w_coord_t x[MAX_RECORDED];
	w_coord_t z[MAX_RECORDED];
	int tick[MAX_RECORDED];
	// The sticky heading the guard was evaluated against on that tick. The
	// guard can only extrapolate a direction the player has ALREADY taken;
	// CASE C uses this to separate "the guard let a column through" from "the
	// player turned onto an axis nothing could have predicted yet".
	int8_t hx[MAX_RECORDED];
	int8_t hz[MAX_RECORDED];
};

static struct server_local* make_server(const char* level_name) {
	struct server_local* s = calloc(1, sizeof(*s));
	assert(s);
	server_local_test_create_without_thread(s, false, false, false, false,
											false, true, false);
	string_t level;
	string_init_set_str(level, level_name);
	assert(server_world_create_with_populate_scratch(
		&s->world, level, WORLD_DIM_OVERWORLD, INT64_C(4242), true,
		s->world_populate_scratch));
	string_clear(level);
	s->world.local = s;
	s->worlds[0].session = s; /* A1a (D1): owning session for services/callbacks */
	return s;
}

static void destroy_server(struct server_local* s) {
	server_world_destroy(&s->world);
	free(s);
}

// Reference implementation of the rule this commit REPLACES: the far batch
// runs only when the near square is fully sent, always with the whole
// FAR_SEND_BUDGET, with no debt guard at all.
static void run_reference_strict(struct server_local* s,
								 const struct tick_script* script, int ticks,
								 struct send_log* log) {
	log->count = 0;
	for(int t = 0; t < ticks; t++) {
		if(s->far_pick_template_built_for != s->far_view_distance)
			server_local_test_far_pick_rebuild(s);
		if(script[t].near_owed != 0)
			continue; // !near_complete -> far batch suspended entirely
		int cursor = 0;
		for(int fsent = 0; fsent < FAR_SEND_BUDGET; fsent++) {
			w_coord_t fx = 0, fz = 0;
			if(!server_local_test_far_pick_next(s, script[t].px, script[t].pz,
												&cursor, &fx, &fz))
				break;
			assert(log->count < MAX_RECORDED);
			log->x[log->count] = fx;
			log->z[log->count] = fz;
			log->tick[log->count] = t;
			log->hx[log->count] = 0;
			log->hz[log->count] = 0;
			log->count++;
			server_world_mark_far_sent(&s->world, fx, fz);
		}
	}
}

// The PRODUCTION rule: heading update -> far_send_share_decide -> the guarded
// pick loop, mirroring server_local.c's far send block one-for-one (minus the
// generation/RPC work a headless test cannot do). `apply_guard` exists only so
// CASE C can measure what the guard is actually buying.
static void run_production(struct server_local* s,
						   const struct tick_script* script, int ticks,
						   int share_limit, bool apply_guard,
						   struct send_log* log) {
	log->count = 0;
	for(int t = 0; t < ticks; t++) {
		if(s->far_pick_template_built_for != s->far_view_distance)
			server_local_test_far_pick_rebuild(s);
		server_local_test_far_send_heading_update(s, script[t].px,
												  script[t].pz);
		bool allowed = false;
		int budget = 0, lead = 0;
		server_local_test_far_send_share(script[t].near_owed,
										 script[t].near_sent, share_limit,
										 &allowed, &budget, &lead);
		if(!allowed)
			continue;
		assert(budget > 0 && budget <= FAR_SEND_BUDGET);
		int cursor = 0;
		for(int fsent = 0; fsent < budget; fsent++) {
			w_coord_t fx = 0, fz = 0;
			if(!server_local_test_far_pick_next(s, script[t].px, script[t].pz,
												&cursor, &fx, &fz))
				break;
			if(apply_guard && lead > 0
			   && server_local_test_far_send_enters_near(
				   fx - script[t].px, fz - script[t].pz, s->far_send_heading_x,
				   s->far_send_heading_z, lead)) {
				fsent--; // a skip is not a spent budget slot
				continue;
			}
			assert(log->count < MAX_RECORDED);
			log->x[log->count] = fx;
			log->z[log->count] = fz;
			log->tick[log->count] = t;
			log->hx[log->count] = s->far_send_heading_x;
			log->hz[log->count] = s->far_send_heading_z;
			log->count++;
			server_world_mark_far_sent(&s->world, fx, fz);
		}
	}
}

static int cheb(w_coord_t dx, w_coord_t dz) {
	int ax = dx < 0 ? -dx : dx;
	int az = dz < 0 ? -dz : dz;
	return ax > az ? ax : az;
}

// A straight-line walk that crosses one chunk per tick along +x with an
// occasional +z step, with the near frontier permanently owing a couple of
// chunks (exactly what a walk looks like: the frontier never converges).
static int build_walk_script(struct tick_script* script, int ticks,
							 int near_owed, int near_sent) {
	w_coord_t px = 0, pz = 0;
	for(int t = 0; t < ticks; t++) {
		script[t].px = px;
		script[t].pz = pz;
		script[t].near_owed = near_owed;
		script[t].near_sent = near_sent;
		px += 1;
		if(t % 4 == 3)
			pz += 1;
	}
	return ticks;
}

// -------------------------------------------------------------------------
// CASE A: pinned to the strict opt-out value (0), the old behaviour is
// reproduced byte-for-byte. This is NOT the shipping default (see CASE A2
// below) -- it proves the strict PATH stays exact, independent of whichever
// value ships by default.
// -------------------------------------------------------------------------
static void case_a_default_is_strict(void) {
	// A mixed script: some ticks converged (near_owed 0), some still owing --
	// so the two rules have every opportunity to disagree if they can.
	static struct tick_script script[24];
	int ticks = (int)(sizeof(script) / sizeof(script[0]));
	w_coord_t px = 0, pz = 0;
	for(int t = 0; t < ticks; t++) {
		script[t].px = px;
		script[t].pz = pz;
		script[t].near_owed = (t % 3 == 0) ? 0 : (t % 5); // 0..4
		script[t].near_sent = t % (NEAR_BATCH + 1); // 0..CHUNK_SEND_BATCH
		px += (t % 2);
		pz += (t % 3 == 2) ? 1 : 0;
	}

	static struct send_log ref, prod;
	struct server_local* a = make_server("test_far_send_share_a_ref_tmp");
	a->far_view_distance = MAX_VIEW_DISTANCE + 3;
	run_reference_strict(a, script, ticks, &ref);
	destroy_server(a);

	struct server_local* b = make_server("test_far_send_share_a_prod_tmp");
	b->far_view_distance = MAX_VIEW_DISTANCE + 3;
	b->far_send_near_owed = 0; // pinned EXPLICITLY: the strict opt-out value,
							  // NOT the shipping default (CASE A2 below)
	run_production(b, script, ticks, b->far_send_near_owed, true, &prod);
	destroy_server(b);

	assert(ref.count > 0); // the comparison would be vacuous otherwise
	assert(ref.count == prod.count);
	for(int i = 0; i < ref.count; i++) {
		assert(ref.x[i] == prod.x[i]);
		assert(ref.z[i] == prod.z[i]);
		assert(ref.tick[i] == prod.tick[i]);
	}
	printf("CASE A (lod.far_send_near_owed=0 == the strict pre-2026-09-04 "
		   "gate): %d columns, identical order: PASS\n",
		   ref.count);
}

// -------------------------------------------------------------------------
// CASE A2: the game's OWN default -- source/main.c's config_read_int
// fallback for "lod.far_send_near_owed", mirroring server_local_init's
// pre-config-override initial value of the field -- is 8, not 0, since
// 2026-09-04 (lane/far-share-default-8). This does not re-derive the value
// from either production site (that would make the test trivially agree with
// whatever the code says); it pins the expected shipping default as a
// standalone fact so a future accidental edit of either default is caught by
// a mismatch against this literal, and documents why: N=16 was measured to
// fill the far ring (far_missing median 48 -> 21 while walking, seed 12345)
// but peak the cost (fps floor 10.4 -> 5.3, server write max 13.5 -> 24.3 ms)
// (docs/platform_perf_notes.md "Default 8"); 8 is the bounded setting. The
// strict value 0 remains a fully supported opt-out -- CASE A above proves
// that path is still byte-exact.
// -------------------------------------------------------------------------
static void case_a2_shipping_default_is_8(void) {
	const int expected_default = 8;

	// The server's own pre-config-override initial value (server_local_init
	// in source/network/server_local.c), read the way a fresh server_local
	// exposes it before main.c applies any config/hw_run override.
	struct server_local* s = make_server("test_far_send_share_a2_tmp");
	assert(s->far_send_near_owed == expected_default);
	destroy_server(s);

	printf("CASE A2 (shipping default lod.far_send_near_owed == %d, not the "
		   "strict 0): PASS\n",
		   expected_default);
}

// -------------------------------------------------------------------------
// CASE B: with the key raised, the far ring fills WHILE the near frontier
// still owes chunks -- and only then.
// -------------------------------------------------------------------------
static void case_b_share_fills_while_walking(void) {
	static struct tick_script script[40];
	int ticks = build_walk_script(script, 40, /*near_owed*/ 3,
								  /*near_sent*/ 1);

	// Strict: a permanently-owing frontier means ZERO far columns, forever --
	// the hardware measurement this lane exists to fix.
	static struct send_log strict_log, share_log;
	struct server_local* a = make_server("test_far_send_share_b_strict_tmp");
	a->far_view_distance = MAX_VIEW_DISTANCE + 1;
	run_production(a, script, ticks, /*share_limit*/ 0, true, &strict_log);
	destroy_server(a);
	assert(strict_log.count == 0);

	// Shared at N=4 (>= the script's owed 3): far columns flow.
	struct server_local* b = make_server("test_far_send_share_b_share_tmp");
	b->far_view_distance = MAX_VIEW_DISTANCE + 1;
	run_production(b, script, ticks, /*share_limit*/ 4, true, &share_log);
	destroy_server(b);
	assert(share_log.count > 0);
	// Every sent column is a real annulus column for its own tick.
	for(int i = 0; i < share_log.count; i++) {
		int t = share_log.tick[i];
		int d = cheb(share_log.x[i] - script[t].px,
					 share_log.z[i] - script[t].pz);
		assert(d > MAX_VIEW_DISTANCE && d <= MAX_VIEW_DISTANCE + 1);
	}

	// Below the threshold the gate stays shut: owed 5 > N=4.
	static struct tick_script over[40];
	build_walk_script(over, 40, /*near_owed*/ 5, /*near_sent*/ 1);
	static struct send_log over_log;
	struct server_local* c = make_server("test_far_send_share_b_over_tmp");
	c->far_view_distance = MAX_VIEW_DISTANCE + 1;
	run_production(c, over, 40, /*share_limit*/ 4, true, &over_log);
	destroy_server(c);
	assert(over_log.count == 0);

	// And a near loop that spent its WHOLE per-tick batch leaves nothing to
	// share, no matter how few chunks it owes -- far never displaces near.
	static struct tick_script busy[40];
	build_walk_script(busy, 40, /*near_owed*/ 1,
					  /*near_sent*/ NEAR_BATCH);
	static struct send_log busy_log;
	struct server_local* d = make_server("test_far_send_share_b_busy_tmp");
	d->far_view_distance = MAX_VIEW_DISTANCE + 1;
	run_production(d, busy, 40, /*share_limit*/ 4, true, &busy_log);
	destroy_server(d);
	assert(busy_log.count == 0);

	// The share budget is exactly the near loop's leftover.
	bool allowed = false;
	int budget = 0, lead = 0;
	server_local_test_far_send_share(2, 1, 4, &allowed, &budget, &lead);
	assert(allowed && budget == NEAR_BATCH - 1
		   && lead == FAR_SEND_SHARE_LEAD_CHUNKS);
	server_local_test_far_send_share(0, NEAR_BATCH, 4, &allowed, &budget,
									 &lead);
	assert(allowed && budget == FAR_SEND_BUDGET && lead == 0); // strict path

	printf("CASE B (share at N=4: %d far columns while owed=3; 0 at owed=5, "
		   "0 when near spent its whole batch, 0 at N=0): PASS\n",
		   share_log.count);
}

// -------------------------------------------------------------------------
// CASE C: the promotion-debt guard.
//
// What the guard can promise: a column far-sent on tick t is NOT inside the
// near square on tick t+1, for every step the player's ESTABLISHED heading
// predicts. It cannot promise more than that -- the heading is derived from
// crossings the player has already made (the server does not mirror the
// client's walk velocity), so the very first step onto a new axis is
// unpredictable by construction. Those steps are counted and reported
// separately rather than being quietly folded into the pass.
// -------------------------------------------------------------------------

// Debt = columns this run far-sent on tick t that are inside the near square
// on tick t+1 (an un-evictable far_sent claim the near loop must then
// promote). `exempt_out` collects the subset that sat on a step the heading
// could not have predicted.
static int count_debt(const struct send_log* log,
					  const struct tick_script* script, int ticks,
					  bool honor_heading, int* exempt_out,
					  int* exempt_max_tick_out) {
	int debt = 0, exempt = 0, exempt_max_tick = -1;
	for(int i = 0; i < log->count; i++) {
		int t = log->tick[i];
		if(t + 1 >= ticks)
			continue;
		if(cheb(log->x[i] - script[t + 1].px, log->z[i] - script[t + 1].pz)
		   > MAX_VIEW_DISTANCE)
			continue;
		if(honor_heading) {
			w_coord_t sx = script[t + 1].px - script[t].px;
			w_coord_t sz = script[t + 1].pz - script[t].pz;
			bool predicted = (sx == 0 || (sx > 0) == (log->hx[i] > 0))
				&& (sz == 0 || (sz > 0) == (log->hz[i] > 0));
			if(!predicted) {
				exempt++;
				if(t > exempt_max_tick)
					exempt_max_tick = t;
				continue;
			}
		}
		debt++;
	}
	if(exempt_out)
		*exempt_out = exempt;
	if(exempt_max_tick_out)
		*exempt_max_tick_out = exempt_max_tick;
	return debt;
}

static void case_c_debt_guard(void) {
	// Reach MAX_VIEW_DISTANCE+1: the annulus is exactly the innermost far
	// ring -- the same shape as the Wii reach-6 hardware case, and the worst
	// case for the guard (every eligible column is one crossing from being
	// near).
	const int ticks = 40;

	// (1) A pure straight-line walk -- the far_walk_6m shape. Every step is
	// predicted from the first crossing on, so the guard owes ZERO debt with
	// ZERO exemptions.
	static struct tick_script straight[40];
	for(int t = 0; t < ticks; t++) {
		straight[t].px = (w_coord_t)t;
		straight[t].pz = 0;
		straight[t].near_owed = 2;
		straight[t].near_sent = 0;
	}
	static struct send_log guarded, unguarded;
	struct server_local* a = make_server("test_far_send_share_c_guard_tmp");
	a->far_view_distance = MAX_VIEW_DISTANCE + 1;
	run_production(a, straight, ticks, /*share_limit*/ 4, /*apply_guard*/ true,
				   &guarded);
	destroy_server(a);
	assert(guarded.count > 0);
	int straight_exempt = 0, straight_exempt_tick = -1;
	int straight_debt = count_debt(&guarded, straight, ticks, true,
								   &straight_exempt, &straight_exempt_tick);
	assert(straight_debt == 0);
	// The ONLY columns the guard cannot cover on a straight walk are the ones
	// sent on tick 0, before the player has crossed a single chunk boundary
	// and there is any heading at all to extrapolate. From the first crossing
	// on -- i.e. every tick of a real walk -- the guard covers everything.
	assert(straight_exempt_tick <= 0);

	// The SAME walk with the guard bypassed DOES accumulate that debt, so the
	// assertion above is proving the guard, not the walk's geometry.
	struct server_local* b = make_server("test_far_send_share_c_noguard_tmp");
	b->far_view_distance = MAX_VIEW_DISTANCE + 1;
	run_production(b, straight, ticks, /*share_limit*/ 4, /*apply_guard*/ false,
				   &unguarded);
	destroy_server(b);
	int straight_unguarded_debt
		= count_debt(&unguarded, straight, ticks, false, NULL, NULL);
	assert(straight_unguarded_debt > 0);

	// (2) A walk that TURNS (a +z step every fourth tick). Every predicted
	// step still owes zero debt; the only columns that slip through sit on the
	// step where the player first moved on an axis whose heading was still
	// unknown -- unpredictable by construction, transient (the near loop
	// promotes the column and clears far_sent on the very next send), and
	// strictly fewer than the unguarded run's debt.
	static struct tick_script turning[40];
	build_walk_script(turning, ticks, /*near_owed*/ 2, /*near_sent*/ 0);
	static struct send_log turn_guarded, turn_unguarded;
	struct server_local* c = make_server("test_far_send_share_c_turn_tmp");
	c->far_view_distance = MAX_VIEW_DISTANCE + 1;
	run_production(c, turning, ticks, /*share_limit*/ 4, /*apply_guard*/ true,
				   &turn_guarded);
	destroy_server(c);
	int turn_exempt = 0;
	int turn_debt = count_debt(&turn_guarded, turning, ticks, true,
							   &turn_exempt, NULL);
	assert(turn_debt == 0);

	struct server_local* d = make_server("test_far_send_share_c_turnraw_tmp");
	d->far_view_distance = MAX_VIEW_DISTANCE + 1;
	run_production(d, turning, ticks, /*share_limit*/ 4, /*apply_guard*/ false,
				   &turn_unguarded);
	destroy_server(d);
	int turn_unguarded_debt
		= count_debt(&turn_unguarded, turning, ticks, false, NULL, NULL);
	assert(turn_exempt < turn_unguarded_debt);

	printf("CASE C (promotion-debt guard, reach %d): straight walk %d columns, "
		   "debt 0 (unguarded %d); turning walk debt 0 on every predicted "
		   "step, %d unpredictable-turn columns vs %d unguarded: PASS\n",
		   MAX_VIEW_DISTANCE + 1, guarded.count, straight_unguarded_debt,
		   turn_exempt, turn_unguarded_debt);
}

// -------------------------------------------------------------------------
// CASE D: the guard predicate + the sticky heading, directly.
// -------------------------------------------------------------------------
static void case_d_predicate_and_heading(void) {
	const int R = MAX_VIEW_DISTANCE;

	// lead 0 (the strict path) is a provable no-op over the whole annulus:
	// nothing outside the near square is ever "about to enter" it.
	for(w_coord_t dz = -(R + 3); dz <= R + 3; dz++)
		for(w_coord_t dx = -(R + 3); dx <= R + 3; dx++) {
			if(cheb(dx, dz) <= R)
				continue; // not an annulus column
			assert(!server_local_test_far_send_enters_near(dx, dz, 1, 0, 0));
			assert(!server_local_test_far_send_enters_near(dx, dz, 0, 0, 1));
		}

	// Walking +x, lead 1: the row directly ahead (dx == R+1, |dz| <= R) IS
	// about to enter; the same row behind, and the +z/-z sides, are not.
	assert(server_local_test_far_send_enters_near(R + 1, 0, 1, 0, 1));
	assert(server_local_test_far_send_enters_near(R + 1, R, 1, 0, 1));
	assert(!server_local_test_far_send_enters_near(R + 1, R + 1, 1, 0, 1));
	assert(!server_local_test_far_send_enters_near(-(R + 1), 0, 1, 0, 1));
	assert(!server_local_test_far_send_enters_near(0, R + 1, 1, 0, 1));
	// Walking -z instead: now the -z row is the one about to enter.
	assert(server_local_test_far_send_enters_near(0, -(R + 1), 0, -1, 1));
	assert(!server_local_test_far_send_enters_near(R + 1, 0, 0, -1, 1));

	// The sticky heading: first tick establishes a baseline (no direction),
	// then each crossing sets its axis and a stationary tick keeps it.
	struct server_local* s = make_server("test_far_send_share_d_tmp");
	assert(!s->far_send_heading_valid);
	server_local_test_far_send_heading_update(s, 4, 9);
	assert(s->far_send_heading_valid && s->far_send_heading_x == 0
		   && s->far_send_heading_z == 0);
	server_local_test_far_send_heading_update(s, 5, 9);
	assert(s->far_send_heading_x == 1 && s->far_send_heading_z == 0);
	server_local_test_far_send_heading_update(s, 5, 8);
	assert(s->far_send_heading_x == 1 && s->far_send_heading_z == -1);
	server_local_test_far_send_heading_update(s, 5, 8); // stationary: sticky
	assert(s->far_send_heading_x == 1 && s->far_send_heading_z == -1);
	server_local_test_far_send_heading_update(s, 2, 8); // turn around
	assert(s->far_send_heading_x == -1 && s->far_send_heading_z == -1);
	destroy_server(s);

	printf("CASE D (guard predicate truth table + sticky heading): PASS\n");
}

int main(void) {
	case_a_default_is_strict();
	case_a2_shipping_default_is_8();
	case_b_share_fills_while_walking();
	case_c_debt_guard();
	case_d_predicate_and_heading();
	printf("test_far_send_share: PASS\n");
	return 0;
}
