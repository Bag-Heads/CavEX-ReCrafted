/* Copyright (c) 2026 CaveWii_v2 contributors
 * colcap-1006 / CC1 (docs/intent/split-column-cap-256.md; ruling
 * tmp/jobs/i07-1006/ASTRA-COLUMN-CAP-R1.md sec. 5): production-path
 * reproduction of the split 256-block separation stall.
 *
 * The REAL server_local update (server_local_test_update_once, one tick per
 * call, with a concurrent client consumer -- the owner-swap harness tick())
 * drives the REAL load stream with:
 *   - split enabled through the boot path (server_local_set_split_enabled),
 *     split.server_view_radius 4,
 *   - split.world_payloads ON (clin_world_payloads_set_enabled before any
 *     client world, main.c's boot order) -- so the A2-T transfer column budget
 *     (server_transfer_budget_tick) is ENFORCED,
 *   - REAL Beta generation (superflat OFF, seed 12345, Overworld), so every
 *     near generation goes through server_transfer_column_preflight's 3x3
 *     population-support reservation (server_world.c ensure_populated),
 *   - the synthetic second anchor of split_anchor_6m (anchor_sep, +X blocks),
 *     exactly the I07 i06/i06b configuration (-AnchorSep 256).
 *
 * Readiness is measured on BOTH sides, the way the hardware latch sees it:
 *   server : every column of each anchor's (2r+1)^2 view square is SENT
 *            (server_world_is_chunk_sent), and for two anchors the server's
 *            own anchor_near_owed[0..1] are 0;
 *   client : world_anchor_status on view 0's installed payload terrain
 *            reports missing[p] == 0 for every anchor (full columns installed;
 *            mesh_pending is not required -- this rig has no mesher workers).
 *
 * Cases (argv[1]):
 *   sep256  two DISJOINT radius-4 footprints. Steady demand under the current
 *           3x3 population-support contract is 2 x 11^2 = 242 live columns.
 *           MUST reach readiness within COLCAP_TICK_BUDGET ticks. At the
 *           current SERVER_TRANSFER_RESIDENT_COLUMN_CAP (176) this is RED:
 *           live_backings + reserved pins at the cap, cap_waits climb, the
 *           owed columns never drain. The failure line names that reason.
 *   sep256_budget_off  CONTROL: identical, but split.world_payloads OFF, so
 *           the A2-T column budget is OFF. Must reach readiness: proves the
 *           sep256 RED is the cap and not this rig (the RESIDENCY-ZERO-DIAG
 *           PC discriminator). Not a certification of anything shipped.
 *   sep0    same configuration, separation 0 (one footprint). PASSES now and
 *           prints the peak physical occupancy (live_backings + reserved) as
 *           the no-regression baseline for a raised ceiling.
 *
 * Separation 256 blocks = 16 chunks; two radius-4 support squares (11 wide)
 * are disjoint for any separation > 10 chunks. */
#include "support/owner_swap_harness.h"

#include "network/client_world_payload.h"
#include "network/server_transfer.h"
#include "world.h"
#include "platform/hw_telemetry.h"
#include "platform/mem2.h"

#include <inttypes.h>

#define TEST_NAME "test_split_column_cap"
#define COLCAP_RADIUS 4
/* Declared finite budget: 3000 server ticks = 150 s of game time. Generation
 * on the PC is not the limit (the 09-11 real-Wii split_anchor PASS reached
 * readiness at 242 live columns in ~41 heartbeats); a stalled cap never
 * drains in any budget. */
#define COLCAP_TICK_BUDGET 3000u
/* Stall diagnosis window: the last N ticks of an unready run. */
#define COLCAP_STALL_WINDOW 400u
#define COLCAP_SETTLE_TICKS 200u
/* CC1's measured sep0 peak (live_backings + reserved), radius 4: one 11x11
 * population-support footprint. */
#define COLCAP_SEP0_BASELINE 121u
/* After a one-chunk move: ticks for readiness to come back, and how long it
 * must then hold. */
#define COLCAP_MOVE_BUDGET 600u
#define COLCAP_MOVE_HOLD 100u

static struct server_local* colcap_world_fixture(void) {
	test_fixture_dir("test_split_column_cap", split_path, sizeof(split_path));
#ifdef _WIN32
	_mkdir(split_path);
#else
	mkdir(split_path, 0777);
#endif
	struct server_local* s = calloc(1, sizeof(*s));
	assert(s);
	/* superflat OFF: real Beta generation + population */
	server_local_test_create_without_thread(s, false, false, false, false,
											false, false, false);
	string_t path;
	string_init_set_str(path, split_path);
	assert(level_archive_create_new(&s->level, path, 12345, (vec3) {0, 100, 0}));
	assert(server_world_create_with_populate_scratch(
		&s->worlds[0], path, WORLD_DIM_OVERWORLD, 12345, false,
		s->world_populate_scratch));
	assert(s->worlds[0].generator && "real generation needs the Beta generator");
	string_set_str(s->level_name, split_path);
	string_clear(path);
	s->worlds[0].local = s;
	s->worlds[0].session = s;
	s->player.has_pos = true;
	s->player.finished_loading = false;
	s->player.x = s->player.z = 0.5;
	/* Above any Beta terrain (max height 127 is solid only on rare peaks),
	 * so the stationary host never suffocates during the run. */
	s->player.y = 120.0 + PLAYER_EYE_HEIGHT;
	s->player.health = MAX_PLAYER_HEALTH;
	s->player.oxygen = MAX_OXYGEN;
	s->autosave_ticks = 0;
	s->far_view_distance = MAX_VIEW_DISTANCE + 1;
	b1_bind_client_world(&s->worlds[0]);
	return s;
}

static struct server_local* colcap_fixture(int anchor_sep) {
	struct server_local* s = colcap_world_fixture();
	/* The boot path (main.c): split + radius dial + synthetic anchor, all
	 * before the server thread would start. */
	s->split_view_radius = COLCAP_RADIUS;
	assert(server_local_set_split_enabled(s, true) && s->split_enabled);
	assert(server_local_set_anchor_sep(s, anchor_sep));
	assert(server_local_view_radius(s) == COLCAP_RADIUS);
	world_set_anchor_sep(anchor_sep);
	return s;
}

/* Teardown that leaves no fixture folder behind. The harness teardown ->
 * split_destroy discards only slot 0 before test_fixture_dir_cleanup
 * (tests/support/split_residency_fixture.h, test_fixture_dir.h:133); a live
 * secondary world (the Nether a transfer admitted) still holds its region
 * files under the fixture's DIM-1, so on Windows the rm -rf could not remove
 * them and every two-body run leaked a test_split_column_cap_<pid> folder.
 * Discard every live slot >= 1 first, then wipe the folder with the shared
 * helper once more. */
static void colcap_discard_secondary(struct server_local* s) {
	for(unsigned k = 1; k < SERVER_LOCAL_WORLD_SLOTS; k++)
		if(server_local_world_live(&s->worlds[k]))
			server_world_discard(&s->worlds[k]);
}
static void colcap_teardown(struct server_local* s) {
	if(s->guest_actor)
		guest_release(s);
	s->paused = true;
	colcap_discard_secondary(s);
	split_destroy(s);
	test_fixture_dir_cleanup(split_path);
}

static void harness_init_payloads(bool payloads) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	recipe_init();
	assert(clin_init() && svin_init());
	assert(clin_world_payloads_set_enabled(payloads));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);
}

static uint32_t occupied(const struct server_local* s) {
	uint32_t live = 0, shared = 0;
	column_backing_get_stats(&live, &shared);
	return live + s->transfer_column_reservation_n;
}

static w_coord_t anchor_cx(const struct server_local* s, unsigned p) {
	return WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->player.x + p * s->anchor_sep));
}
static w_coord_t anchor_cz(const struct server_local* s) {
	return WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->player.z));
}

/* Columns of anchor p's view square the server has not sent yet. */
static unsigned server_unsent(struct server_local* s, unsigned p) {
	unsigned n = 0;
	w_coord_t cx = anchor_cx(s, p), cz = anchor_cz(s);
	for(int dz = -COLCAP_RADIUS; dz <= COLCAP_RADIUS; dz++)
		for(int dx = -COLCAP_RADIUS; dx <= COLCAP_RADIUS; dx++)
			n += !server_world_is_chunk_sent(&s->worlds[0], cx + dx, cz + dz);
	return n;
}

struct colcap_obs {
	unsigned anchors;
	unsigned unsent[2];
	uint32_t owed[2];
	int client_missing[2];
	bool client_bound;
};

static struct colcap_obs observe(struct server_local* s) {
	struct colcap_obs o;
	memset(&o, 0, sizeof(o));
	o.anchors = s->anchor_sep ? 2u : 1u;
	for(unsigned p = 0; p < o.anchors; p++) {
		o.unsent[p] = server_unsent(s, p);
		o.owed[p] = s->anchor_sep ?
			__atomic_load_n(&s->anchor_near_owed[p], __ATOMIC_RELAXED) : 0;
	}
	/* main.c view_terrain(0): the view's payload, else the legacy world */
	struct clin_world_payload pay;
	struct world* terrain = clin_world_payload_of_view(0, &pay) && pay.terrain ?
		pay.terrain : (clin_world_payloads_enabled() ? NULL : &gstate.world);
	o.client_bound = terrain != NULL;
	o.client_missing[0] = o.client_missing[1] = -1;
	if(o.client_bound) {
		int missing[2], pending[2], far[2], far_pending[2];
		bool collision = false;
		world_anchor_status(terrain, s->player.x, s->player.y, s->player.z,
							COLCAP_RADIUS, COLCAP_RADIUS, missing, pending, far,
							far_pending, &collision);
		o.client_missing[0] = missing[0];
		o.client_missing[1] = missing[1];
	}
	return o;
}

static bool obs_server_ready(const struct colcap_obs* o) {
	for(unsigned p = 0; p < o->anchors; p++)
		if(o->unsent[p] || o->owed[p])
			return false;
	return true;
}
static bool obs_client_ready(const struct colcap_obs* o) {
	if(!o->client_bound)
		return false;
	for(unsigned p = 0; p < o->anchors; p++)
		if(o->client_missing[p] != 0)
			return false;
	return true;
}

static void run_case(int anchor_sep, const char* which) {
	struct server_local* s = colcap_fixture(anchor_sep);
	uint32_t peak = 0, peak_tick = 0;
	uint32_t server_ready_tick = 0, client_ready_tick = 0;
	bool server_ready = false, client_ready = false;
	bool over_cap = false;
	uint32_t window_waits0 = 0, window_occ_min = UINT32_MAX,
			 window_occ_max = 0;
	unsigned window_unsent0[2] = {0, 0};
	unsigned tick_n = 0;
	/* CC2 (ASTRA-CC-R2 item 6): readiness must be SIMULTANEOUS and HELD --
	 * server and installed client both ready on the same tick, then on every
	 * one of the next COLCAP_SETTLE_TICKS ticks (a latch of historical
	 * success is not enough). The recorded peak covers that hold. */
	unsigned streak = 0, held_at = 0, broke = 0;
	bool invariant_ok = true;
	struct colcap_obs o;
	memset(&o, 0, sizeof(o));
	for(tick_n = 1; tick_n <= COLCAP_TICK_BUDGET; tick_n++) {
		tick(s);
		client_acks_marker(s);
		uint32_t occ = occupied(s);
		if(occ > peak) {
			peak = occ;
			peak_tick = tick_n;
		}
		if(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED
		   && occ > SERVER_TRANSFER_RESIDENT_COLUMN_CAP)
			over_cap = true;
		invariant_ok &= !over_cap;
		o = observe(s);
		if(!server_ready && obs_server_ready(&o)) {
			server_ready = true;
			server_ready_tick = tick_n;
		}
		if(!client_ready && obs_client_ready(&o)) {
			client_ready = true;
			client_ready_tick = tick_n;
		}
		if(obs_server_ready(&o) && obs_client_ready(&o)) {
			if(++streak > COLCAP_SETTLE_TICKS) {
				held_at = tick_n;
				break;
			}
		} else if(streak) {
			broke++;
			streak = 0;
		}
		if(tick_n == COLCAP_TICK_BUDGET - COLCAP_STALL_WINDOW) {
			window_waits0 = s->transfer_column_cap_waits;
			window_unsent0[0] = o.unsent[0];
			window_unsent0[1] = o.unsent[1];
		}
		if(tick_n > COLCAP_TICK_BUDGET - COLCAP_STALL_WINDOW) {
			if(occ < window_occ_min)
				window_occ_min = occ;
			if(occ > window_occ_max)
				window_occ_max = occ;
		}
	}
	if(tick_n > COLCAP_TICK_BUDGET)
		tick_n = COLCAP_TICK_BUDGET;
	struct server_transfer_budget_snapshot snap;
	bool snapped = server_transfer_budget_snapshot_get(&snap);
	uint32_t live = 0, shared = 0;
	column_backing_get_stats(&live, &shared);
	printf("%s[%s]: sep=%d radius=%d cap=%u ticks=%u budget_phase=%u "
		   "live_backings=%" PRIu32 " reserved=%u dict=%zu peak_occupied=%" PRIu32
		   " (tick %" PRIu32 ") enforced_peak=%" PRIu32 " cap_waits=%" PRIu32
		   " server_ready=%d@%" PRIu32 " client_ready=%d@%" PRIu32
		   " unsent=[%u,%u] owed=[%" PRIu32 ",%" PRIu32 "] client_missing=[%d,%d]\n",
		   TEST_NAME, which, anchor_sep, COLCAP_RADIUS,
		   (unsigned)SERVER_TRANSFER_RESIDENT_COLUMN_CAP, tick_n,
		   (unsigned)s->transfer_budget_phase, live,
		   s->transfer_column_reservation_n,
		   (size_t)dict_server_chunks_size(s->worlds[0].chunks), peak, peak_tick,
		   snapped ? snap.enforced_peak : s->transfer_column_peak,
		   s->transfer_column_cap_waits, server_ready, server_ready_tick,
		   client_ready, client_ready_tick, o.unsent[0], o.unsent[1], o.owed[0],
		   o.owed[1], o.client_missing[0], o.client_missing[1]);
	if(clin_world_payloads_enabled())
		check(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED,
			  "the A2-T column budget is ENFORCED (split + world payloads)");
	else
		check(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_OFF,
			  "control: world payloads OFF leaves the column budget OFF");
	check(!over_cap, "live_backings + reserved never exceeds the cap while enforced");
	if(!(server_ready && client_ready)) {
		/* Classify the stall so a RED run states WHY. The cap-stall signature
		 * (I07 i06/i06b): occupancy pinned within one 3x3 reservation of the
		 * cap for the whole window, cap WAITs still climbing, and the owed
		 * columns not draining. */
		uint32_t waits = s->transfer_column_cap_waits - window_waits0;
		bool pinned = window_occ_min + SERVER_TRANSFER_COLUMN_RESERVATIONS
				>= SERVER_TRANSFER_RESIDENT_COLUMN_CAP
			&& window_occ_max <= SERVER_TRANSFER_RESIDENT_COLUMN_CAP;
		bool no_drain = o.unsent[0] + o.unsent[1]
			>= window_unsent0[0] + window_unsent0[1];
		/* WHICH cap: both anchors are in world slot 0, so the A2-W steady cap
		 * (A2W_STEADY_COLUMN_CAP, server_transfer_stream_step, slots >= 1
		 * only) is not on this path; the pin must sit ABOVE it, at the
		 * transfer resident cap enforced by server_transfer_column_preflight
		 * -> column_reserve (the only cap_waits producers here). */
		bool transfer_cap = window_occ_min > A2W_STEADY_COLUMN_CAP;
		if(pinned && waits > 0 && no_drain)
			printf("RED %s[%s]: CAP STALL on %s -- live_backings+reserved pinned at "
				   "%" PRIu32 "..%" PRIu32 " of cap %u over the last %u ticks, "
				   "cap_waits +%" PRIu32 ", owed columns never drain "
				   "(unsent [%u,%u] -> [%u,%u], owed [%" PRIu32 ",%" PRIu32
				   "]); disjoint radius-%d demand is 2 x %d^2 = %d\n",
				   TEST_NAME, which,
				   transfer_cap ?
					   "SERVER_TRANSFER_RESIDENT_COLUMN_CAP via "
					   "server_transfer_column_preflight/column_reserve (slot 0; "
					   "A2W_STEADY_COLUMN_CAP 162 not on this path)" :
					   "an occupancy at/below A2W_STEADY_COLUMN_CAP -- UNEXPECTED",
				   window_occ_min, window_occ_max,
				   (unsigned)SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
				   COLCAP_STALL_WINDOW, waits, window_unsent0[0],
				   window_unsent0[1], o.unsent[0], o.unsent[1], o.owed[0],
				   o.owed[1], COLCAP_RADIUS, 2 * COLCAP_RADIUS + 3,
				   2 * (2 * COLCAP_RADIUS + 3) * (2 * COLCAP_RADIUS + 3));
		else
			printf("RED %s[%s]: NOT READY for a reason OTHER than the cap stall "
				   "(occ window %" PRIu32 "..%" PRIu32 ", cap_waits +%" PRIu32
				   ", no_drain %d)\n",
				   TEST_NAME, which, window_occ_min, window_occ_max, waits,
				   no_drain);
	}
	char msg[200];
	snprintf(msg, sizeof(msg),
			 "separation %d: server AND installed-client readiness within %u ticks",
			 anchor_sep, COLCAP_TICK_BUDGET);
	check(server_ready && client_ready, msg);
	snprintf(msg, sizeof(msg),
			 "separation %d: simultaneous server+client readiness HELD for %u "
			 "consecutive ticks (held at tick %u, readiness lost %u times)",
			 anchor_sep, COLCAP_SETTLE_TICKS, held_at, broke);
	check(held_at > 0, msg);
	check(invariant_ok && peak <= SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
		  "occupancy invariant live_backings + reserved <= cap held every tick");
	if(anchor_sep == 0) {
		snprintf(msg, sizeof(msg),
				 "separation 0: peak occupancy %" PRIu32 " does not exceed the "
				 "CC1 baseline %u (a higher ceiling allocates no extra terrain)",
				 peak, COLCAP_SEP0_BASELINE);
		check(peak <= COLCAP_SEP0_BASELINE, msg);
	}
	printf("RESULT %s[%s]: ready@%" PRIu32 " held@%u peak=%" PRIu32 "\n",
		   TEST_NAME, which, server_ready_tick > client_ready_tick
		   ? server_ready_tick : client_ready_tick, held_at, peak);
	if(anchor_sep == 0)
		printf("BASELINE %s[%s]: peak physical occupancy (live_backings + "
			   "reserved) = %" PRIu32 " columns at radius %d; settled %" PRIu32
			   " after %u ticks past readiness\n",
			   TEST_NAME, which, peak, COLCAP_RADIUS, occupied(s),
			   COLCAP_SETTLE_TICKS);
	world_set_anchor_sep(0);
	colcap_teardown(s);
}


/* ======================================================================== */
/* CC2: TWO REAL BODIES (host + admitted guest), real generation, world
 * payloads ON, split ON through the production SRPC_SPLIT_SET. Dimensions
 * are reached by REAL A2 transfers (server_transfer_request driven to
 * COMMITTED, the residency suite's bs_transfer recipe). Bodies are then
 * placed by the residency suite's setup write (position + an echoing pose
 * sample) -- that placement is a teleport, after which ONLY the production
 * loaders (slot-0 near loop, A2-W stream, population preflight, the shared
 * cap) bring the terrain in.
 *
 * Readiness per body b (view b): server -- every column of b's (2r+1)^2
 * square SENT in b's committed world; client -- view b's bound payload
 * terrain holds every one of those columns as a full section. */
static struct server_local* colcap_guest_fixture(void) {
	struct server_local* s = colcap_world_fixture();
	s->paused = true;
	s->split_view_radius = COLCAP_RADIUS;
	stream_to_marker(s);
	acknowledge(s);
	s->thread_started = true;
	enable_split(s);
	guest_attach(s);
	guest_body_home(s);
	struct client_session* guest_client = &gstate.players[1];
	assert(client_session_request_local_gameplay(guest_client));
	pump_handshake(s);
	assert(s->guest_gameplay && guest_client->local_gameplay_ready);
	assert(s->guest_actor && s->guest_actor->finished_loading);
	assert(server_local_view_radius(s) == COLCAP_RADIUS);
	/* A paused, loaded game is frozen (server_local update): run it live,
	 * like the CC1 synthetic case, so the production loaders stream. */
	s->paused = false;
	return s;
}

static void body_keep_alive(struct server_player* p) {
	/* The case measures residency, not survival: a body parked in fresh
	 * terrain must not die and divert into the respawn flow mid-measure. */
	p->health = MAX_PLAYER_HEALTH;
	p->oxygen = MAX_OXYGEN;
	p->fall_distance = 0.0F;
	p->fire = -PLAYER_FIRE_RESISTANCE;
}

static void colcap_sample(struct server_local* s, struct server_player* p,
						  double x, double y, double z) {
	struct server_rpc call = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = x, .y = y, .z = z, .on_ground = true,
			.correction_seq = p->correction_seq}};
	if(p == s->guest_actor)
		assert(svin_rpc_send_from(guest_source(s), &call));
	else
		assert(svin_rpc_send(&call));
	drain_server(s);
}

static void body_preload(struct server_local* s, struct server_player* p,
						 double x, double z) {
	/* Make the target footprint resident FIRST. A pose written onto a
	 * non-resident footprint is legally snapped back to the body's last
	 * resident pin by residency admission (server_local.c:11058-11062,
	 * r->pin restore) -- the "guest reverts to (0.5,0.5)" seen when the
	 * placement raced the loaders. Loading (not populating) the 3x3 here is
	 * setup only; population and sending still go through the production
	 * stream, under the cap. */
	struct server_world* w = server_local_world_of(s, p);
	const w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(x));
	const w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(z));
	/* All-or-nothing, one pass, no waiting (waiting lets the Overworld home
	 * ring refill to the cap). If the shared load gate refuses any of the 9
	 * (the cap working: an Overworld/Overworld session keeps slot 0 near the
	 * ceiling while both bodies are home), the columns loaded here are
	 * evicted again and the body is placed exactly as before; run_two_body
	 * then asserts what the preload protects -- that no residency pin-restore
	 * fired for either body. */
	bool loaded_here[9] = {false};
	unsigned missing = 0;
	for(int i = 0; w && i < 9; i++) {
		const w_coord_t x0 = cx + i % 3 - 1, z0 = cz + i / 3 - 1;
		if(server_world_chunk_resident(w, x0, z0))
			continue;
		struct server_chunk* loaded = NULL;
		if(server_world_load_chunk(w, x0, z0, &loaded) && loaded)
			loaded_here[i] = true;
		else
			missing++;
	}
	if(missing) {
		for(int i = 0; i < 9; i++)
			if(loaded_here[i])
				(void)server_world_save_chunk(w, true, cx + i % 3 - 1,
											  cz + i / 3 - 1);
		printf("  [placement: %u footprint columns refused by the cap "
			   "(phase %u, occupied %" PRIu32 "); preload undone, placing as "
			   "before, pin-restore checked]\n", missing,
			   (unsigned)s->transfer_budget_phase, occupied(s));
	}
	check(w != NULL, "placement: the body has a committed world");
}

static void body_place(struct server_local* s, struct server_player* p,
					   double x, double z) {
	body_preload(s, p, x, z); /* a no-op when run_two_body preloaded it */
	p->x = x;
	p->z = z;
	p->vel_x = p->vel_y = p->vel_z = 0.0F;
	p->on_ground = true;
	body_keep_alive(p);
	colcap_sample(s, p, x, p->y, z);
}

/* Sol review #3: the occupancy invariant sampled on EVERY tick that goes
 * through two_tick -- also the ticks inside colcap_transfer -- so a transient
 * over-cap allocation during a transfer cannot slip between samples. */
static uint32_t s_colcap_inv_over, s_colcap_inv_peak;
static void two_tick(struct server_local* s) {
	body_keep_alive(&s->player);
	if(s->guest_actor)
		body_keep_alive(s->guest_actor);
	struct server_player* g = s->guest_actor;
	const double gx0 = g ? g->x : 0, gz0 = g ? g->z : 0;
	client_acks_marker(s);
	pump_handshake(s);
	tick(s);
	if(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED) {
		const uint32_t occ = occupied(s);
		if(occ > s_colcap_inv_peak)
			s_colcap_inv_peak = occ;
		if(occ > SERVER_TRANSFER_RESIDENT_COLUMN_CAP)
			s_colcap_inv_over++;
	}
	g = s->guest_actor;
	if(getenv("COLCAP_DEBUG") && g
	   && (fabs(g->x - gx0) > 8.0 || fabs(g->z - gz0) > 8.0)) {
		struct server_world* gw = server_local_world_of(s, g);
		printf("  [guest JUMP %.1f,%.1f -> %.1f,%.1f slot %d rz rec %d fail %d "
			   "fails %u restores %u health %d resp_phase %d]\n", gx0, gz0,
			   g->x, g->z,
			   gw ? (int)gw->world_index : -1, (int)g->rz.recovering,
			   (int)g->rz.failed, (unsigned)g->rz.recovery_failures, (unsigned)g->rz.restores,
			   (int)g->health, (int)s->guest_respawn_phase);
	}
}

static bool colcap_transfer(struct server_local* s, struct server_player* mover,
							enum world_dim dim, const char* what) {
	uint32_t id = 0;
	bool was = s->paused;
	s->paused = false;
	const uint32_t waits0 = s->transfer_column_cap_waits;
	enum server_transfer_rc rc = server_transfer_request(s, mover, dim, &id);
	/* BUSY is transient (a presentation hold, hold-end or landing-cancel
	 * still pending from the previous trip): retry once per tick, bounded,
	 * the way a player re-entering the portal would. */
	unsigned busy_ticks = 0;
	while(rc == SERVER_TRANSFER_REFUSED_BUSY && busy_ticks < 600) {
		two_tick(s);
		busy_ticks++;
		rc = server_transfer_request(s, mover, dim, &id);
	}
	if(busy_ticks)
		printf("  [%s: BUSY for %u ticks before the request was %s; host_pres "
			   "%d guest_pres %d respawn %d switch %d portal_job %d origin %d "
			   "death_evict %d]\n", what, busy_ticks,
			   rc == SERVER_TRANSFER_OK ? "accepted" : "still refused",
			   (int)s->a2w_host_presentation, (int)s->a2w_guest_presentation,
			   (int)s->guest_respawn_phase, (int)s->world_switch_phase,
			   s->portal_job != NULL, (int)s->portal_origin.valid,
			   (int)s->death_world_evict);
	enum server_transfer_state st = SERVER_TRANSFER_ROLLED_BACK;
	/* COLCAP_DEBUG: watch the destination's 5x5 chunk neighbourhood in the
	 * destination world (Beta Teleporter base = pose x8 / /8) every tick of
	 * the transfer and log residency changes against the transfer state. */
	struct server_world* src_w = server_local_world_of(s, mover);
	const double scale = src_w && src_w->dimension == WORLD_DIM_NETHER
		&& dim == WORLD_DIM_OVERWORLD ? 8.0
		: (dim == WORLD_DIM_NETHER ? 0.125 : 1.0);
	const w_coord_t dcx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(mover->x * scale));
	const w_coord_t dcz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(mover->z * scale));
	uint32_t dmask_prev = 0;
	if(rc == SERVER_TRANSFER_OK)
		for(unsigned i = 0; i < 4000; i++) {
			st = server_transfer_state_of(s, id);
			if(getenv("COLCAP_DEBUG")) {
				struct server_world* dw = server_local_world_for_dimension(s, dim);
				uint32_t m = 0;
				for(int k = 0; dw && k < 25; k++)
					m |= (uint32_t)server_world_chunk_resident(dw,
						dcx + k % 5 - 2, dcz + k / 5 - 2) << k;
				if(m != dmask_prev)
					printf("  [dst t%u state %d occ %u: resident 5x5 around "
						   "(%d,%d) = %07x (lost %07x)]\n", i, (int)st,
						   (unsigned)occupied(s), (int)dcx, (int)dcz, m,
						   dmask_prev & ~m);
				dmask_prev = m;
			}
			if(st == SERVER_TRANSFER_COMMITTED || st == SERVER_TRANSFER_ROLLED_BACK)
				break;
			two_tick(s);
		}
	s->paused = was;
	struct server_world* w = server_local_world_of(s, mover);
	const bool ok = rc == SERVER_TRANSFER_OK && st == SERVER_TRANSFER_COMMITTED
		&& w && w->dimension == dim;
	unsigned acked = 0;
	while(ok && server_transfer_actor_frozen(s, mover) && acked < 600) {
		screen_load_world.update(&screen_load_world, 30.0f);
		clin_update_input_readiness();
		two_tick(s);
		acked++;
	}
	/* The guest client's echo that ends its post-COMMIT presentation: its
	 * first pose sample on the fresh correction token at the committed pose
	 * (the same echo test_a2_transfer_transaction drives; without it the
	 * next request from this body is REFUSED_BUSY forever). */
	for(unsigned i = 0; ok && mover == s->guest_actor && i < 200
		&& s->a2w_guest_presentation; i++) {
		colcap_sample(s, mover, mover->x, mover->y, mover->z);
		two_tick(s);
	}
	printf("  [%s: cap WAITs during the transfer +%u, occupied %u]\n", what,
		   (unsigned)(s->transfer_column_cap_waits - waits0),
		   (unsigned)occupied(s));
	char label[200];
	snprintf(label, sizeof label, "%s by a REAL A2 transfer (rc %d, state %d, "
			 "now slot %d dim %d, acked in %u ticks)", what, (int)rc, (int)st,
			 w ? (int)w->world_index : -1, w ? (int)w->dimension : -99, acked);
	check(ok && !server_transfer_actor_frozen(s, mover), label);
	return ok && !server_transfer_actor_frozen(s, mover);
}

static unsigned body_server_unsent(struct server_local* s,
								   struct server_player* p) {
	struct server_world* w = server_local_world_of(s, p);
	if(!w)
		return (2 * COLCAP_RADIUS + 1) * (2 * COLCAP_RADIUS + 1);
	unsigned n = 0;
	w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(p->x));
	w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(p->z));
	for(int dz = -COLCAP_RADIUS; dz <= COLCAP_RADIUS; dz++)
		for(int dx = -COLCAP_RADIUS; dx <= COLCAP_RADIUS; dx++)
			n += !server_world_is_chunk_sent(w, cx + dx, cz + dz);
	return n;
}

/* -1: the view is not bound to a payload yet. */
static int body_client_missing(unsigned view, const struct server_player* p) {
	struct clin_world_payload pay;
	if(!clin_world_payload_of_view(view, &pay) || !pay.terrain)
		return -1;
	int n = 0;
	w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(p->x));
	w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(p->z));
	for(int dz = -COLCAP_RADIUS; dz <= COLCAP_RADIUS; dz++)
		for(int dx = -COLCAP_RADIUS; dx <= COLCAP_RADIUS; dx++) {
			struct world_section* sec = dict_wsection_get(
				pay.terrain->sections, SECTION_TO_ID(cx + dx, cz + dz));
			bool full = sec && !sec->is_far_lod;
			for(size_t k = 0; full && k < COLUMN_HEIGHT; k++)
				full = sec->column[k] != NULL;
			n += !full;
		}
	return n;
}

struct two_obs {
	unsigned unsent[2];
	int missing[2];
	unsigned debt; /* unsent + missing over both bodies (unbound = 81) */
	bool ready;
};
static struct two_obs two_observe(struct server_local* s) {
	struct two_obs o;
	memset(&o, 0, sizeof o);
	struct server_player* b[2] = {&s->player, s->guest_actor};
	o.ready = b[1] != NULL;
	for(unsigned v = 0; v < 2 && b[1]; v++) {
		o.unsent[v] = body_server_unsent(s, b[v]);
		o.missing[v] = body_client_missing(v, b[v]);
		o.debt += o.unsent[v]
			+ (o.missing[v] < 0 ? (2 * COLCAP_RADIUS + 1) * (2 * COLCAP_RADIUS + 1)
							   : (unsigned)o.missing[v]);
		o.ready &= o.unsent[v] == 0 && o.missing[v] == 0;
	}
	return o;
}

struct two_run {
	/* peak: the ENFORCED occupancy peak (what the ceiling governs);
	 * prebudget_peak: occupancy before enforcement began -- the legacy
	 * single-player near ring the budget's DRAINING phase sheds first. */
	uint32_t peak, peak_tick, prebudget_peak, ready_tick, held_tick;
	unsigned max_debt, broke;
	bool invariant_ok, enforced;
};
/* Tick until simultaneous readiness has HELD for `hold` consecutive ticks or
 * `budget` ticks pass; the occupancy invariant is checked EVERY tick. */
static struct two_run two_until_held(struct server_local* s, unsigned budget,
									 unsigned hold) {
	struct two_run r;
	memset(&r, 0, sizeof r);
	r.invariant_ok = true;
	unsigned streak = 0;
	for(unsigned t = 1; t <= budget; t++) {
		two_tick(s);
		uint32_t occ = occupied(s);
		r.enforced = s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED;
		if(r.enforced && occ > r.peak) {
			r.peak = occ;
			r.peak_tick = t;
		}
		if(!r.enforced && occ > r.prebudget_peak)
			r.prebudget_peak = occ;
		if(r.enforced && occ > SERVER_TRANSFER_RESIDENT_COLUMN_CAP)
			r.invariant_ok = false;
		struct two_obs o = two_observe(s);
		if(o.debt > r.max_debt)
			r.max_debt = o.debt;
		if(o.ready) {
			if(!streak && !r.ready_tick)
				r.ready_tick = t;
			if(++streak >= hold) {
				r.held_tick = t;
				return r;
			}
		} else if(streak) {
			r.broke++;
			if(getenv("COLCAP_DEBUG")) {
				printf("  [lost readiness t%u after %u: unsent %u,%u missing "
					   "%d,%d]", t, streak, o.unsent[0], o.unsent[1],
					   o.missing[0], o.missing[1]);
				struct server_player* bb[2] = {&s->player, s->guest_actor};
				for(unsigned v = 0; v < 2; v++) {
					struct server_world* w = server_local_world_of(s, bb[v]);
					if(!w || !o.unsent[v])
						continue;
					w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(bb[v]->x));
					w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(bb[v]->z));
					for(int dz = -COLCAP_RADIUS; dz <= COLCAP_RADIUS; dz++)
						for(int dx = -COLCAP_RADIUS; dx <= COLCAP_RADIUS; dx++)
							if(!server_world_is_chunk_sent(w, cx + dx, cz + dz)) {
								struct server_chunk* sc = dict_server_chunks_get(
									w->chunks, S_CHUNK_ID(cx + dx, cz + dz));
								printf(" v%u slot%u dim%d d(%d,%d) res%d stage%d", v,
									   (unsigned)w->world_index,
									   (int)w->dimension, dx, dz,
									   (int)server_world_chunk_resident(
										   w, cx + dx, cz + dz),
									   sc ? (int)sc->a2w_pub_stage : -1);
							}
				}
				printf("\n");
			}
			streak = 0;
		}
	}
	return r;
}

/* Move one or both bodies by (dx,dz) blocks in 0.5-block owner samples, one
 * per tick, then require the readiness to come back and HOLD. */
static void two_move_leg(struct server_local* s, const char* which,
						 const char* leg, bool host, bool guest, double dx,
						 double dz, unsigned* fails) {
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	const double hx0 = h->x, hz0 = h->z, gx0 = g->x, gz0 = g->z;
	const double hx1 = hx0 + (host ? dx : 0), hz1 = hz0 + (host ? dz : 0);
	const double gx1 = gx0 + (guest ? dx : 0), gz1 = gz0 + (guest ? dz : 0);
	const unsigned steps = (unsigned)(fmax(fabs(dx), fabs(dz)) / 0.5);
	uint32_t walk_peak = 0;
	unsigned walk_debt = 0;
	bool walk_inv = true;
	for(unsigned i = 1; i <= steps; i++) {
		double f = (double)i / steps;
		if(host)
			colcap_sample(s, h, hx0 + (hx1 - hx0) * f, h->y,
						  hz0 + (hz1 - hz0) * f);
		if(guest)
			colcap_sample(s, g, gx0 + (gx1 - gx0) * f, g->y,
						  gz0 + (gz1 - gz0) * f);
		two_tick(s);
		uint32_t occ = occupied(s);
		if(occ > walk_peak)
			walk_peak = occ;
		if(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED
		   && occ > SERVER_TRANSFER_RESIDENT_COLUMN_CAP)
			walk_inv = false;
		struct two_obs o = two_observe(s);
		if(o.debt > walk_debt)
			walk_debt = o.debt;
	}
	bool arrived = (!host || (fabs(h->x - hx1) < 1e-6 && fabs(h->z - hz1) < 1e-6))
		&& (!guest || (fabs(g->x - gx1) < 1e-6 && fabs(g->z - gz1) < 1e-6));
	struct two_run back = two_until_held(s, COLCAP_MOVE_BUDGET, COLCAP_MOVE_HOLD);
	uint32_t peak = walk_peak > back.peak ? walk_peak : back.peak;
	unsigned debt = walk_debt > back.max_debt ? walk_debt : back.max_debt;
	/* One chunk crossing exposes at most 2(2r+1)-1 = 17 new view columns per
	 * moving body (diagonal); counted on BOTH sides (server unsent + client
	 * missing), so the bound per moving body is 2 x 17. */
	const unsigned debt_bound
		= (unsigned)(host + guest) * 2u * (2u * (2u * COLCAP_RADIUS + 1u) - 1u);
	printf("LEG %s[%s/%s]: steps %u arrived %d, ready@%u held@%u (budget %u, "
		   "hold %u), peak %" PRIu32 ", max debt %u (bound %u), lost %u\n",
		   TEST_NAME, which, leg, steps, (int)arrived, back.ready_tick,
		   back.held_tick, COLCAP_MOVE_BUDGET, COLCAP_MOVE_HOLD, peak, debt,
		   debt_bound, back.broke);
	char label[260];
	snprintf(label, sizeof label, "%s %s: moved by %u owner samples, readiness "
			 "back and held %u ticks within %u (held@%u), debt %u <= %u, "
			 "occupancy <= cap every tick (peak %" PRIu32 ")", which, leg,
			 steps, COLCAP_MOVE_HOLD, COLCAP_MOVE_BUDGET, back.held_tick, debt,
			 debt_bound, peak);
	bool ok = arrived && back.held_tick > 0 && debt <= debt_bound && walk_inv
		&& back.invariant_ok && peak <= SERVER_TRANSFER_RESIDENT_COLUMN_CAP;
	check(ok, label);
	*fails += !ok;
}

static double chunk_center(int c) { return c * 16 + 8.5; }

/* host_dim/guest_dim: the world each body ends in. far_host: the HOST (not
 * the guest) is the body placed at the +256 block separation. coincident:
 * both bodies at the SAME chunk coordinates (across worlds), in a fresh area
 * so each world needs its own full 11x11 support footprint. */
static void run_two_body(const char* which, enum world_dim host_dim,
						 enum world_dim guest_dim, bool far_host,
						 bool coincident) {
	struct server_local* s = colcap_guest_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	if(guest_dim == WORLD_DIM_NETHER
	   && !colcap_transfer(s, g, WORLD_DIM_NETHER, "guest -> Nether"))
		goto out;
	if(host_dim == WORLD_DIM_NETHER
	   && !colcap_transfer(s, h, WORLD_DIM_NETHER, "host -> Nether"))
		goto out;
	{
		struct server_world* hw = server_local_world_of(s, h);
		struct server_world* gw = server_local_world_of(s, g);
		char label[260];
		snprintf(label, sizeof label, "%s: host in dim %d (slot %d), guest in "
				 "dim %d (slot %d)", which, hw ? (int)hw->dimension : -99,
				 hw ? (int)hw->world_index : -1, gw ? (int)gw->dimension : -99,
				 gw ? (int)gw->world_index : -1);
		check(hw && gw && hw->dimension == host_dim
			  && gw->dimension == guest_dim, label);
		/* Placement: chunk-centred so every later leg crosses exactly one
		 * boundary. Coincident: both at chunk (16,16) of their own worlds.
		 * Separated: the near body at chunk (2,2), the far one 256 blocks
		 * (16 chunks) further +X. */
		int ncx = 2, ncz = 2, fcx = ncx + 16, fcz = ncz;
		if(coincident)
			ncx = ncz = fcx = fcz = getenv("COLCAP_COINCIDENT_CHUNK")
				? atoi(getenv("COLCAP_COINCIDENT_CHUNK")) : 16;
		struct server_player* nearb = far_host ? g : h;
		struct server_player* farb = far_host ? h : g;
		/* Both footprints are preloaded BEFORE either body moves: once the
		 * first body's 11x11 load is under way, a test-side preload of the
		 * second would compete with it for the same 256 slots (the cap
		 * correctly refusing the 257th). */
		body_preload(s, nearb, chunk_center(ncx), chunk_center(ncz));
		body_preload(s, farb, chunk_center(fcx), chunk_center(fcz));
		body_place(s, nearb, chunk_center(ncx), chunk_center(ncz));
		body_place(s, farb, chunk_center(fcx), chunk_center(fcz));
		if(getenv("COLCAP_DEBUG"))
			printf("  [placed: host %.1f,%.1f guest %.1f,%.1f]\n", h->x, h->z,
				   g->x, g->z);
		const uint32_t restores0 = h->rz.restores + g->rz.restores;
		struct two_run r = two_until_held(s, COLCAP_TICK_BUDGET,
										  COLCAP_SETTLE_TICKS);
		snprintf(label, sizeof label, "%s: no residency pin-restore moved a "
				 "placed body back (restores +%u; server_local.c:11058)", which,
				 (unsigned)(h->rz.restores + g->rz.restores - restores0));
		check(h->rz.restores + g->rz.restores == restores0, label);
		uint32_t live = 0, shared = 0;
		column_backing_get_stats(&live, &shared);
		printf("RESULT %s[%s]: host dim %d @chunk(%d,%d) guest dim %d "
			   "@chunk(%d,%d); ready@%u held@%u (budget %u, hold %u) enforced "
			   "peak %" PRIu32 " @%u cap %u (pre-budget drain peak %" PRIu32
			   "); now live %" PRIu32 " reserved %u slot0 %zu slot1 %zu; lost "
			   "%u cap_waits %" PRIu32 "\n",
			   TEST_NAME, which, (int)host_dim,
			   (int)WCOORD_CHUNK_OFFSET((w_coord_t)floor(h->x)),
			   (int)WCOORD_CHUNK_OFFSET((w_coord_t)floor(h->z)), (int)guest_dim,
			   (int)WCOORD_CHUNK_OFFSET((w_coord_t)floor(g->x)),
			   (int)WCOORD_CHUNK_OFFSET((w_coord_t)floor(g->z)), r.ready_tick,
			   r.held_tick, COLCAP_TICK_BUDGET, COLCAP_SETTLE_TICKS, r.peak,
			   r.peak_tick, (unsigned)SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
			   r.prebudget_peak, live,
			   s->transfer_column_reservation_n,
			   (size_t)dict_server_chunks_size(s->worlds[0].chunks),
			   server_local_world_live(&s->worlds[1])
				   ? (size_t)dict_server_chunks_size(s->worlds[1].chunks) : 0,
			   r.broke, s->transfer_column_cap_waits);
		check(r.enforced, "the A2-T column budget is ENFORCED (split + world "
						  "payloads, two real bodies)");
		snprintf(label, sizeof label, "%s: simultaneous server+installed-client "
				 "readiness of BOTH bodies reached (tick %u) and HELD %u ticks "
				 "within %u", which, r.ready_tick, COLCAP_SETTLE_TICKS,
				 COLCAP_TICK_BUDGET);
		check(r.held_tick > 0, label);
		snprintf(label, sizeof label, "%s: occupancy invariant live_backings + "
				 "reserved <= %u every tick (peak %" PRIu32 ")", which,
				 (unsigned)SERVER_TRANSFER_RESIDENT_COLUMN_CAP, r.peak);
		check(r.invariant_ok && r.peak <= SERVER_TRANSFER_RESIDENT_COLUMN_CAP,
			  label);
		if(coincident) {
			/* Same chunk coordinates, DIFFERENT worlds: keyed by world slot +
			 * generation + coords, both worlds' view squares are resident at
			 * once and EVERY one is a live backing of its own -- live >=
			 * slot0 + slot1 resident columns, each >= 81 (a coordinate dedupe
			 * would show one 81-column set). MEASURED, not assumed: a fresh
			 * Nether footprint peaks well below 121 here (see RESULT), so the
			 * 2 x 11^2 = 242 load figure is an OVERWORLD-pair figure; this
			 * case asserts the no-dedupe accounting, and the load peak is
			 * reported. */
			const size_t d0 = dict_server_chunks_size(s->worlds[0].chunks);
			const size_t d1 = server_local_world_live(&s->worlds[1])
				? dict_server_chunks_size(s->worlds[1].chunks) : 0;
			snprintf(label, sizeof label, "%s: coincident coordinates in "
					 "different worlds COUNT TWICE (live %" PRIu32 " >= slot0 "
					 "%zu + slot1 %zu, each >= 81; load peak %" PRIu32 ")",
					 which, live, d0, d1, r.peak);
			check(d0 >= 81 && d1 >= 81 && live >= d0 + d1 && r.peak >= 162,
				  label);
		}
		if(r.held_tick) {
			unsigned fails = 0;
			two_move_leg(s, which, "host axial +X", true, false, 16.0, 0.0,
						 &fails);
			two_move_leg(s, which, "guest axial +Z", false, true, 0.0, 16.0,
						 &fails);
			two_move_leg(s, which, "both diagonal +X+Z", true, true, 16.0, 16.0,
						 &fails);
			two_move_leg(s, which, "both diagonal -X-Z", true, true, -16.0,
						 -16.0, &fails);
		}
	}
out:
	colcap_teardown(s);
}

/* Reservation failure -> retry at the RAISED ceiling (R1 item 5): with the
 * session at cap - 1 live backings, a nine-column Nether population preflight
 * refuses as a WAIT with no reservation kept, no column inserted and the
 * Nether population RNG untouched; once nine source backings retire, the
 * same target completes and the session never exceeds the cap. (The twin at
 * the old ceiling is test_a2_transfer_transaction cap_population.) */
static void run_cap_retry_rng(const char* which) {
	struct server_local* s = split_fixture();
	string_set_str(s->level_name, split_path);
	s->player.finished_loading = true;
	s->split_enabled = true;
	s->split_view_radius = COLCAP_RADIUS;
	struct server_world* dst = server_local_world_admit(s, WORLD_DIM_NETHER);
	check(dst && dst == &s->worlds[1], "cap retry: Nether world admitted");
	if(!dst) {
		split_destroy(s);
		return;
	}
	for(w_coord_t x = 1000; occupied(s) < SERVER_TRANSFER_RESIDENT_COLUMN_CAP - 1;
		x++) {
		struct server_chunk* sc = NULL;
		if(!server_world_load_chunk(&s->worlds[0], x, 1000, &sc) || !sc) {
			check(false, "cap retry: source preload succeeds");
			break;
		}
	}
	server_transfer_budget_tick(s);
	char label[200];
	snprintf(label, sizeof label, "cap retry: preloaded to %" PRIu32
			 " = cap - 1 occupied, budget ENFORCED", occupied(s));
	check(occupied(s) == SERVER_TRANSFER_RESIDENT_COLUMN_CAP - 1
		  && s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED, label);
	uint32_t id = 0;
	check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id)
			  == SERVER_TRANSFER_OK,
		  "cap retry: transfer opened against the saturated session");
	struct server_chunk* target = NULL;
	struct java_random rng_before = dst->hell_populate_rng;
	uint32_t waits_before = s->transfer_column_cap_waits;
	server_world_begin_update(dst);
	check(!server_world_ensure_populated(dst, s, 200, 200, &target),
		  "cap retry: a nine-column populate refuses at cap - 1");
	check(s->transfer_column_cap_waits > waits_before
			  && s->transfer_column_reservation_n == 0
			  && dict_server_chunks_size(dst->chunks) == 0
			  && memcmp(&rng_before, &dst->hell_populate_rng,
						sizeof(rng_before)) == 0,
		  "cap retry: the refusal is a WAIT -- no reservation kept, no column "
		  "inserted, Nether population RNG byte-identical");
	for(w_coord_t x = 1000; x < 1009; x++)
		check(server_world_save_chunk(&s->worlds[0], true, x, 1000),
			  "cap retry: a source backing is released for retry");
	bool ready = false;
	uint32_t peak = occupied(s);
	unsigned tries = 0;
	for(; tries < 512 && !ready; tries++) {
		server_world_begin_update(dst);
		ready = server_world_ensure_populated(dst, s, 200, 200, &target);
		if(occupied(s) > peak)
			peak = occupied(s);
	}
	snprintf(label, sizeof label, "cap retry: the same target completes after "
			 "nine backings retire (%u tries), peak %" PRIu32 " <= %u",
			 tries, peak, (unsigned)SERVER_TRANSFER_RESIDENT_COLUMN_CAP);
	check(ready && target && target->populated
			  && s->transfer_column_reservation_n == 0
			  && peak <= SERVER_TRANSFER_RESIDENT_COLUMN_CAP, label);
	printf("RESULT %s[%s]: refused at %u, completed after %u tries, peak %"
		   PRIu32 "\n", TEST_NAME, which,
		   (unsigned)SERVER_TRANSFER_RESIDENT_COLUMN_CAP - 1, tries, peak);
	s->paused = true;
	colcap_discard_secondary(s);
	split_destroy(s);
	test_fixture_dir_cleanup(split_path);
}

/* The two R2 item-5 telemetry contracts that are checkable on PC: the libogc
 * request rule behind mem2_carve_largest_allocatable_bytes, and the
 * column_pool_grow event's wire shape (both validity states). */
static void run_telemetry_contract(const char* which) {
	/* __lwp_heap_allocate: dsize = roundup(size, pg) + pg + 8; a free block
	 * of physical size B satisfies it iff B >= dsize. */
	const size_t pg = 32, ovh = 8;
	check(mem2_lwp_allocatable_from_block(0, pg, ovh) == 0
		  && mem2_lwp_allocatable_from_block(71, pg, ovh) == 0
		  && mem2_lwp_allocatable_from_block(72, pg, ovh) == 32
		  && mem2_lwp_allocatable_from_block(103, pg, ovh) == 32
		  && mem2_lwp_allocatable_from_block(104, pg, ovh) == 64,
		  "contiguity: small-block edges of the libogc request rule");
	/* exhaustive cross-check against the allocator's own dsize formula */
	bool rule_ok = true;
	for(size_t b = 0; b < 2048 && rule_ok; b++) {
		size_t a = mem2_lwp_allocatable_from_block(b, pg, ovh);
		for(size_t req = 1; req <= 2048; req++) {
			size_t dsize = req + pg + ovh + (req % pg ? pg - req % pg : 0);
			if((b >= dsize) != (req <= a)) {
				rule_ok = false;
				break;
			}
		}
	}
	check(rule_ok, "contiguity: allocatable(B) is EXACTLY the largest request "
				   "the __lwp_heap_allocate dsize admits into a B-byte block");
	/* the one request that matters: a column slab (lead + 32 x stride) */
	/* 256 = MEM2_GUARD (mem2.c, private): the slab lead and per-slot guard */
	const size_t guard = 256u;
	const size_t slab = guard + 32u * (MEM2_POOL_COLUMN_OBJECT + guard);
	check(slab == 2638080u
		  && mem2_lwp_allocatable_from_block(slab + pg + ovh, pg, ovh) >= slab
		  && mem2_lwp_allocatable_from_block(slab + pg + ovh - 1, pg, ovh) < slab,
		  "contiguity: a 2,638,080 B slab needs a 2,638,120 B free block");
	size_t v = 1;
	check(!mem2_carve_largest_allocatable_bytes(&v) && v == 0,
		  "contiguity: PC has no carve -- the reading is explicitly INVALID");
	char line[384];
	struct hw_column_pool_grow ev = {.seq = 7, .requested_bytes = 2638080u,
		.free_bytes_before = 9834496u, .largest_allocatable_bytes_before = 4194272u,
		.slabs_before = 6, .slabs_after = 7, .before_valid = true,
		.success = true};
	size_t n = hw_telemetry_format_column_pool_grow(line, sizeof line, &ev);
	printf("GROW %s[%s]: %s (%zu B)\n", TEST_NAME, which, line, n);
	check(n == strlen(line) && !strcmp(line,
		  "{\"event\":\"column_pool_grow\",\"seq\":7,\"requested_bytes\":2638080,"
		  "\"before_valid\":true,\"free_bytes_before\":9834496,"
		  "\"largest_allocatable_bytes_before\":4194272,\"slabs_before\":6,"
		  "\"slabs_after\":7,\"success\":true}"),
		  "grow event: exact wire bytes of a successful seventh slab");
	ev = (struct hw_column_pool_grow) {.seq = UINT32_MAX,
		.requested_bytes = UINT32_MAX, .free_bytes_before = 123,
		.largest_allocatable_bytes_before = 456, .slabs_before = UINT32_MAX,
		.slabs_after = UINT32_MAX, .before_valid = false, .success = false};
	n = hw_telemetry_format_column_pool_grow(line, sizeof line, &ev);
	printf("GROW %s[%s]: widest %zu B\n", TEST_NAME, which, n);
	check(n < sizeof line && strstr(line, "\"before_valid\":false,"
		  "\"free_bytes_before\":0,\"largest_allocatable_bytes_before\":0,")
		  && strstr(line, "\"success\":false}"),
		  "grow event: an untrusted walk publishes before_valid false and "
		  "zeros, never stale bytes");
	/* colcap-1006 (ASTRA-CC-R3 item 4): the hand-back event's wire shape,
	 * pinned byte-for-byte (tests/test_anchor_pool_collector.py grades this
	 * exact line through the collector). */
	struct hw_column_pool_shrink sev = {.seq = 9, .slab = 7,
		.released_bytes = 2638080u, .slabs_before = 8, .slabs_after = 7};
	n = hw_telemetry_format_column_pool_shrink(line, sizeof line, &sev);
	printf("SHRINK %s[%s]: %s (%zu B)\n", TEST_NAME, which, line, n);
	check(n == strlen(line) && !strcmp(line,
		  "{\"event\":\"column_pool_shrink\",\"seq\":9,\"slab\":7,"
		  "\"released_bytes\":2638080,\"slabs_before\":8,\"slabs_after\":7}"),
		  "shrink event: exact wire bytes of an eighth slab handed back");
	sev = (struct hw_column_pool_shrink) {.seq = UINT32_MAX, .slab = UINT32_MAX,
		.released_bytes = UINT32_MAX, .slabs_before = UINT32_MAX,
		.slabs_after = UINT32_MAX};
	n = hw_telemetry_format_column_pool_shrink(line, sizeof line, &sev);
	check(n < 256 && n == strlen(line) && line[n - 1] == '}',
		  "shrink event: the widest line fits the 256 B emit frame");
}


/* CC2 (lead decision after the flicker diagnosis): an entity that crosses a
 * chunk line into an already-published column of a SECONDARY (slot >= 1)
 * world must be republished WITHOUT that column ever reading as unsent.
 * `sent` means "terrain published" to every production reader
 * (server_world_is_chunk_sent: guest admission/respawn gates, residency
 * debt, the A2-W marker); the entity republish is driven by a2w_pub_stage
 * alone (server_transfer_secondary_entity_joined). Production path: a death
 * item spawned in an UNSENT (support) column of the guest's Nether world is
 * not published; moving it into a sent view column and refreshing its chunk
 * membership (server_local_refresh_entity_membership -> ..._entity_joined)
 * must keep the destination sent EVERY tick while the stream step publishes
 * the entity (pub stage back to 3, entity stamped with the world). */
static void run_republish_keeps_sent(const char* which) {
	struct server_local* s = colcap_guest_fixture();
	struct server_player* g = s->guest_actor;
	if(!colcap_transfer(s, g, WORLD_DIM_NETHER, "guest -> Nether")) {
		colcap_teardown(s);
		return;
	}
	struct server_world* nw = server_local_world_of(s, g);
	struct two_run r0 = two_until_held(s, COLCAP_TICK_BUDGET, 20);
	check(r0.held_tick > 0, "republish: guest Nether view ready before the move");
	const w_coord_t gcx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(g->x));
	const w_coord_t gcz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(g->z));
	/* source: one column OUTSIDE the view (never sent), target: the view
	 * edge column next to it (sent) */
	const w_coord_t scx = gcx + COLCAP_RADIUS + 1, tcx = gcx + COLCAP_RADIUS;
	if(!server_world_chunk_resident(nw, scx, gcz)) {
		struct server_chunk* loaded = NULL;
		check(server_world_load_chunk(nw, scx, gcz, &loaded) && loaded,
			  "republish: source support column loaded");
	}
	check(!server_world_is_chunk_sent(nw, scx, gcz)
		  && server_world_is_chunk_sent(nw, tcx, gcz),
		  "republish: source column unsent, target view column sent");
	struct item_data it = {.id = 4, .count = 1};
	struct entity* e = server_local_spawn_death_item_w(nw, g,
		scx * 16 + 8.5, g->y, gcz * 16 + 8.5, &it);
	check(e != NULL, "republish: death item spawned in the source column");
	if(!e) {
		colcap_teardown(s);
		return;
	}
	const uint32_t eid = e->id;
	const bool pub_before = e->client_pub_slot == nw->world_index
		&& e->client_pub_generation == nw->generation;
	check(!pub_before, "republish: the item in the unsent column is not "
					   "published yet (precondition)");
	/* cross the chunk line into the sent target column */
	e->pos[0] = (float)(tcx * 16 + 8.5);
	e->vel[0] = e->vel[1] = e->vel[2] = 0.0F;
	server_local_refresh_entity_membership(s, e);
	struct server_chunk* tc = dict_server_chunks_get(nw->chunks,
		S_CHUNK_ID(tcx, gcz));
	bool sent_always = server_world_is_chunk_sent(nw, tcx, gcz);
	printf("REPUBLISH %s[%s]: right after the crossing sent=%d stage=%d\n",
		   TEST_NAME, which, (int)server_world_is_chunk_sent(nw, tcx, gcz),
		   tc ? (int)tc->a2w_pub_stage : -1);
	unsigned done_at = 0;
	for(unsigned t = 1; t <= 200 && !done_at; t++) {
		two_tick(s);
		sent_always &= server_world_is_chunk_sent(nw, tcx, gcz);
		struct entity* now = dict_entity_get(s->entities, eid)
			? *dict_entity_get(s->entities, eid) : NULL;
		tc = dict_server_chunks_get(nw->chunks, S_CHUNK_ID(tcx, gcz));
		if(now && tc && tc->a2w_pub_stage == 3
		   && now->client_pub_slot == nw->world_index
		   && now->client_pub_generation == nw->generation)
			done_at = t;
	}
	printf("REPUBLISH %s[%s]: sent held every tick %d, republish completed at "
		   "tick %u\n", TEST_NAME, which, (int)sent_always, done_at);
	check(sent_always, "republish: the target column stays SENT (terrain "
					   "published) through the entity republish");
	check(done_at > 0, "republish: the entity republish still completes "
					   "(pub stage 3, entity stamped with the Nether world)");
	colcap_teardown(s);
}


/* Sol review #1: the free-extent walker over a CORRUPTED boundary-tag image
 * returns INVALID without reading off alignment or out of the arena. The
 * image is a hand-built libogc layout (word 0 back_flag, word 1 front_flag =
 * size | USED): used 32 B, free 64 B, then the final dummy header. */
static void run_heap_walk_corrupt(const char* which) {
	static uint32_t img[64] __attribute__((aligned(32)));
	const uintptr_t base = (uintptr_t)img, final = base + 96u;
#define CC_RESET() do { \
		memset(img, 0, sizeof img); \
		img[0] = 1u; img[1] = 32u | 1u; /* A: used, 32 B */ \
		img[8] = 32u | 1u; img[9] = 64u; /* B: free, 64 B */ \
		img[24] = 64u; img[25] = 1u; /* final dummy */ \
	} while(0)
	size_t fr = 0, lg = 0;
	CC_RESET();
	bool ok = mem2_heap_free_extent_raw(base, final, &fr, &lg);
	check(ok && fr == 64 && lg == 64, "heap walk: an intact image is VALID "
		  "(free 64, largest 64)");
	static const struct { uint32_t front; const char* what; } bad[] = {
		{3u, "size 2 (the review's case: undersized, misaligned next header)"},
		{1u, "size 0"},
		{18u | 1u, "size 18 (not a multiple of 4)"},
		{8u | 1u, "size 8 (below one heap_block)"},
		{4096u | 1u, "size past the arena's final header"},
	};
	for(size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
		CC_RESET();
		img[1] = bad[k].front;
		fr = lg = 1;
		ok = mem2_heap_free_extent_raw(base, final, &fr, &lg);
		char label[200];
		snprintf(label, sizeof label, "heap walk: corrupted front_flag, %s, "
				 "is INVALID with zeroed outputs", bad[k].what);
		check(!ok && fr == 0 && lg == 0, label);
	}
	CC_RESET();
	img[8] = 40u | 1u; /* B's back tag no longer repeats A's front tag */
	ok = mem2_heap_free_extent_raw(base, final, &fr, &lg);
	check(!ok, "heap walk: a mismatched back tag is INVALID");
	CC_RESET();
	ok = mem2_heap_free_extent_raw(base + 2u, final, &fr, &lg);
	check(!ok, "heap walk: a misaligned base is INVALID");
	printf("HEAPWALK %s[%s]: all corruptions rejected without a crash\n",
		   TEST_NAME, which);
#undef CC_RESET
}

/* Sol review #3: transfers and return trips AFTER both views have loaded
 * their 2 x 11^2 footprint (taken at the first simultaneous readiness, at
 * the load peak), then separation -> reunion -- with the occupancy
 * invariant sampled on EVERY tick, inside colcap_transfer too (two_tick's
 * global sampler), and every phase bounded by a declared tick budget. */
#define COLCAP_TRANSFER_BUDGET 4000u
static void run_saturated_transfers(const char* which) {
	struct server_local* s = colcap_guest_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	char label[260];
	s_colcap_inv_over = 0;
	s_colcap_inv_peak = 0;
	body_preload(s, h, chunk_center(2), chunk_center(2));
	/* COLCAP_SAT_NEAR (debug): the same legs without the 256 separation --
	 * the discriminator between occupancy pressure and anything else. */
	const int gfx = getenv("COLCAP_SAT_GFX") ? atoi(getenv("COLCAP_SAT_GFX"))
		: getenv("COLCAP_SAT_NEAR") ? 3 : 18;
	body_preload(s, g, chunk_center(gfx), chunk_center(2));
	body_place(s, h, chunk_center(2), chunk_center(2));
	body_place(s, g, chunk_center(gfx), chunk_center(2));
	struct two_run r = two_until_held(s, COLCAP_TICK_BUDGET, 1);
	printf("SAT %s[%s]: separated 256 ready@%u, enforced peak %" PRIu32
		   ", occupied now %" PRIu32 "\n", TEST_NAME, which, r.ready_tick,
		   r.peak, occupied(s));
	check(r.ready_tick > 0, "saturated: both views reached readiness at 256 "
							"blocks before the transfers");
	static const struct {
		bool host;
		enum world_dim dim;
		const char* what;
	} legs[] = {
		{false, WORLD_DIM_NETHER, "guest -> Nether"},
		{false, WORLD_DIM_OVERWORLD, "guest -> Overworld (return)"},
		{true, WORLD_DIM_NETHER, "host -> Nether"},
		{true, WORLD_DIM_OVERWORLD, "host -> Overworld (return)"},
	};
	for(size_t k = 0; k < sizeof legs / sizeof legs[0]; k++) {
		struct server_player* mover = legs[k].host ? h : s->guest_actor;
		const uint32_t over0 = s_colcap_inv_over;
		bool ok = mover && colcap_transfer(s, mover, legs[k].dim, legs[k].what);
		struct two_run after = ok
			? two_until_held(s, COLCAP_TRANSFER_BUDGET, COLCAP_MOVE_HOLD)
			: (struct two_run) {0};
		printf("SAT %s[%s/%s]: committed %d, ready@%u held@%u (budget %u), "
			   "peak %" PRIu32 ", over-cap ticks +%" PRIu32 "\n", TEST_NAME,
			   which, legs[k].what, (int)ok, after.ready_tick, after.held_tick,
			   COLCAP_TRANSFER_BUDGET, after.peak, s_colcap_inv_over - over0);
		snprintf(label, sizeof label, "saturated %s: completes, both views "
				 "ready and held within %u ticks, live_backings + reserved <= "
				 "%u on every tick including the transfer", legs[k].what,
				 COLCAP_TRANSFER_BUDGET,
				 (unsigned)SERVER_TRANSFER_RESIDENT_COLUMN_CAP);
		check(ok && after.held_tick > 0 && s_colcap_inv_over == over0, label);
		if(!ok || !after.held_tick)
			break;
	}
	/* reunion: the far body comes back next to the other one */
	g = s->guest_actor;
	if(g && server_local_world_of(s, g) == server_local_world_of(s, h)) {
		const uint32_t over0 = s_colcap_inv_over;
		body_preload(s, g, chunk_center(3), chunk_center(2));
		body_place(s, g, chunk_center(3), chunk_center(2));
		struct two_run re = two_until_held(s, COLCAP_TRANSFER_BUDGET,
										   COLCAP_SETTLE_TICKS);
		printf("SAT %s[%s/reunion]: ready@%u held@%u, occupied after %" PRIu32
			   "\n", TEST_NAME, which, re.ready_tick, re.held_tick,
			   occupied(s));
		check(re.held_tick > 0 && s_colcap_inv_over == over0,
			  "saturated reunion: separation -> reunion reaches and holds "
			  "readiness within budget, occupancy <= cap every tick");
		check(occupied(s) <= 2u * 81u, "saturated reunion: the reunited "
			  "views settle within two view squares (no stranded columns)");
	} else
		check(false, "saturated: both bodies back in one world for the reunion");
	/* Sol review 2 (C): transfers AFTER the reunion too -- both actors
	 * through the Nether and back once more, every phase bounded, the
	 * occupancy invariant sampled per tick and at every allocation point. */
	for(size_t k = 0; k < sizeof legs / sizeof legs[0]; k++) {
		struct server_player* mover = legs[k].host ? h : s->guest_actor;
		const uint32_t over0 = s_colcap_inv_over;
		bool ok = mover && colcap_transfer(s, mover, legs[k].dim, legs[k].what);
		struct two_run after = ok
			? two_until_held(s, COLCAP_TRANSFER_BUDGET, COLCAP_MOVE_HOLD)
			: (struct two_run) {0};
		printf("SAT %s[%s/after reunion: %s]: committed %d, held@%u, peak %"
			   PRIu32 "\n", TEST_NAME, which, legs[k].what, (int)ok,
			   after.held_tick, after.peak);
		snprintf(label, sizeof label, "saturated after reunion %s: completes "
				 "and holds within %u ticks, occupancy <= cap every tick",
				 legs[k].what, COLCAP_TRANSFER_BUDGET);
		check(ok && after.held_tick > 0 && s_colcap_inv_over == over0, label);
		if(!ok || !after.held_tick)
			break;
	}
	{
		uint32_t exc = 0, peak = 0;
		server_transfer_cap_excursions(&exc, &peak);
		snprintf(label, sizeof label, "saturated: no allocation-time excursion "
				 "over the ceiling anywhere in the case (count %u, peak %u)",
				 (unsigned)exc, (unsigned)peak);
		check(exc == 0, label);
	}
	printf("SAT %s[%s]: max occupancy sampled on every enforced tick %" PRIu32
		   ", over-cap ticks %" PRIu32 "\n", TEST_NAME, which,
		   s_colcap_inv_peak, s_colcap_inv_over);
	check(s_colcap_inv_over == 0 && s_colcap_inv_peak
		  <= SERVER_TRANSFER_RESIDENT_COLUMN_CAP, "saturated: the occupancy "
		  "invariant held on every tick of the whole case");
	colcap_teardown(s);
}


/* Teleporter parity of the slot-0 eviction pin (colcap-1006). Beta runs the
 * whole Teleporter inside chunkProviderServer.chunkLoadOverride
 * (ServerConfigurationManager.java:209-211; ChunkProviderServer.java:86):
 * every chunk it touches is force-loaded and NOTHING unloads mid-call. The
 * pin (server_transfer_teleporter_pin) must reproduce exactly that, so the
 * same seed and the same guest trip must produce the same portal:
 *   leg "pin":  host at home, far from the guest's Overworld return
 *               destination -- the area survives only because of the pin;
 *   leg "near": the host stands on that destination first, so its own ring
 *               keeps the area resident and nothing there is ever evicted
 *               (the no-eviction reference).
 * Compared byte for byte: the guest's landed pose, and the exact
 * coordinates + id + metadata of every OBSIDIAN and PORTAL cell in a 33x33 x
 * full-height box around the landing -- the frame the Teleporter wrote
 * (site, orientation, height). Other cells are NOT compared: the "near" leg
 * keeps the area loaded and simulated for hundreds of extra ticks (random
 * ticks, fluids, leaf decay), which legitimately changes unrelated blocks;
 * measured: a whole-box digest differs while the frame and pose match. The
 * Teleporter's private RNG is not reachable from here; its observable
 * consequences (orientation draw, site choice, frame cells) are compared. One process per leg (fixtures are process-global); the "near"
 * leg reads the "pin" leg's digest file and compares. */
static uint64_t parity_fnv(uint64_t h, uint32_t v) {
	for(int k = 0; k < 4; k++) {
		h ^= (v >> (8 * k)) & 0xffu;
		h *= 1099511628211ull;
	}
	return h;
}
static uint64_t parity_pose(uint64_t h, const struct server_player* p);
static void run_teleporter_parity(const char* which, bool near, bool existing) {
	struct server_local* s = colcap_guest_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	body_preload(s, h, chunk_center(2), chunk_center(2));
	body_preload(s, g, chunk_center(18), chunk_center(2));
	body_place(s, h, chunk_center(2), chunk_center(2));
	body_place(s, g, chunk_center(18), chunk_center(2));
	(void)two_until_held(s, COLCAP_TICK_BUDGET, 1);
	bool ok = colcap_transfer(s, g, WORLD_DIM_NETHER, "parity: guest -> Nether");
	(void)two_until_held(s, COLCAP_TRANSFER_BUDGET, COLCAP_MOVE_HOLD);
	g = s->guest_actor;
	double dx = floor(g->x * 8.0) + 0.5, dz = floor(g->z * 8.0) + 0.5;
	w_coord_t ex0 = 0;
	if(existing) {
		/* An EXISTING Overworld portal 4+ chunks from the return base: frame
		 * x0..x0+3 with x0 on a chunk's last column, so the bottom portal
		 * cell sits at local x 0 of the next chunk and the winner needs TWO
		 * chunks (tp_job_prepare_winner_chunks' seam rule). Built once,
		 * identically in both legs, then left to the evictor. */
		struct server_world* ow0 = &s->worlds[0];
		ex0 = (WCOORD_CHUNK_OFFSET((w_coord_t)floor(dx)) + 4) * 16 + 15;
		const w_coord_t ez = (w_coord_t)floor(dz), ey = 70;
		for(w_coord_t c = ex0; c <= ex0 + 16; c += 16) {
			struct server_chunk* loaded = NULL;
			if(!server_world_chunk_resident(ow0, WCOORD_CHUNK_OFFSET(c),
											WCOORD_CHUNK_OFFSET(ez)))
				check(server_world_load_chunk(ow0, WCOORD_CHUNK_OFFSET(c),
							WCOORD_CHUNK_OFFSET(ez), &loaded) && loaded,
					  "parity: existing-portal columns loaded");
		}
		if(getenv("COLCAP_DEBUG"))
			printf("  [existing portal: frame x %d..%d z %d y %d; columns "
				   "resident %d %d]\n", (int)ex0, (int)ex0 + 3, (int)ez, (int)ey,
				   (int)server_world_chunk_resident(ow0, WCOORD_CHUNK_OFFSET(ex0),
					   WCOORD_CHUNK_OFFSET(ez)),
				   (int)server_world_chunk_resident(ow0,
					   WCOORD_CHUNK_OFFSET(ex0 + 16), WCOORD_CHUNK_OFFSET(ez)));
		ow0->editing_blocks = true;
		bool built = true;
		for(int pass = 0; pass < 2; pass++)
			for(w_coord_t a = 0; a < 4; a++)
				for(w_coord_t y = ey; y <= ey + 4; y++) {
					bool frame = a == 0 || a == 3 || y == ey || y == ey + 4;
					if(frame != (pass == 0))
						continue;
					built &= server_world_set_block(ow0, ex0 + a, y, ez,
						(struct block_data) {.type = frame ? BLOCK_OBSIDIAN
							: BLOCK_PORTAL});
					/* each write queues client updates; outside tick() no
					 * consumer runs, so drain here or the inbox fills */
					clin_update_drain();
				}
		ow0->editing_blocks = false;
		check(built, "parity: existing portal frame written");
		if(getenv("COLCAP_DEBUG"))
			printf("  [existing portal written: %d]\n", (int)built);
		dx = ex0 + 2.5;
		dz = ez + 0.5;
		for(int i = 0; i < 40; i++)
			two_tick(s);
	}
	if(near) {
		/* the host takes up position on the return destination first */
		body_preload(s, h, dx, dz);
		body_place(s, h, dx, dz);
		(void)two_until_held(s, COLCAP_TRANSFER_BUDGET, COLCAP_MOVE_HOLD);
	}
	ok = ok && colcap_transfer(s, g, WORLD_DIM_OVERWORLD,
							   "parity: guest -> Overworld");
	g = s->guest_actor;
	struct server_world* ow = server_local_world_of(s, g);
	uint64_t digest = 1469598103934665603ull;
	unsigned portal_cells = 0;
	const w_coord_t lx = (w_coord_t)floor(g->x), lz = (w_coord_t)floor(g->z);
	digest = parity_pose(digest, g); /* exact pos/rot/vel bytes */
	for(w_coord_t x = lx - 16; ok && ow && x <= lx + 16; x++)
		for(w_coord_t z = lz - 16; z <= lz + 16; z++)
			for(w_coord_t y = 0; y < WORLD_HEIGHT; y++) {
				struct block_data b = {0};
				if(!server_world_get_block(ow, x, y, z, &b)) {
					ok = false;
					break;
				}
				portal_cells += b.type == BLOCK_PORTAL;
				if(b.type != BLOCK_PORTAL && b.type != BLOCK_OBSIDIAN)
					continue;
				digest = parity_fnv(digest, (uint32_t)(x - lx));
				digest = parity_fnv(digest, (uint32_t)y);
				digest = parity_fnv(digest, (uint32_t)(z - lz));
				digest = parity_fnv(digest, (uint32_t)b.type << 8 | b.metadata);
			}
	printf("PARITY %s[%s]: landed %.3f %.3f %.3f portal cells %u digest "
		   "%016llx\n", TEST_NAME, which, g->x, g->y, g->z, portal_cells,
		   (unsigned long long)digest);
	check(ok && portal_cells > 0, "parity: the return trip committed onto a "
								  "built portal in a fully readable area");
	if(existing) {
		char lbl[200];
		snprintf(lbl, sizeof lbl, "parity: the guest landed at the EXISTING "
				 "portal (x %.2f vs frame %d..%d), not a newly built one",
				 g->x, (int)ex0, (int)ex0 + 3);
		check(g->x > ex0 && g->x < ex0 + 3, lbl);
	}
	/* CW_PARITY_FILE_TAG (colcap-1006 R4): lets the cross-placement pair
	 * (CMake test_column_placement_parity_*) use its own digest file, so it
	 * never races the near leg of the ordinary pair, which removes it. */
	const char* file_tag = getenv("CW_PARITY_FILE_TAG");
	char path[128];
	snprintf(path, sizeof path, "%s_teleporter_parity%s%s.txt", TEST_NAME,
			 existing ? "_existing" : "", file_tag ? file_tag : "");
	if(!near) {
		FILE* f = fopen(path, "w");
		check(f != NULL, "parity: pin-leg digest recorded");
		if(f) {
			fprintf(f, "%016llx %u\n", (unsigned long long)digest, portal_cells);
			fclose(f);
		}
	} else {
		unsigned long long want = 0;
		unsigned want_cells = 0;
		FILE* f = fopen(path, "r");
		bool have = f && fscanf(f, "%llx %u", &want, &want_cells) == 2;
		if(f)
			fclose(f);
		char label[200];
		snprintf(label, sizeof label, "parity: pinned leg == no-eviction leg "
				 "(digest %016llx vs %016llx, portal cells %u vs %u)",
				 want, (unsigned long long)digest, want_cells, portal_cells);
		check(have && want == digest && want_cells == portal_cells, label);
		remove(path);
	}
	colcap_teardown(s);
}


/* Write a 4x5 obsidian frame with a 2x3 portal sheet into `w` at frame x
 * x0..x0+3, z, y0..y0+4 (columns loaded first). Shared by the existing-portal
 * fixtures; client updates are drained per write (no consumer runs outside
 * tick()). */
static bool colcap_build_portal(struct server_world* w, w_coord_t x0,
								w_coord_t z, w_coord_t y0) {
	/* release-1031 full-ctest-2: this returned false once under -j8 load
	 * (secondary_near) and never alone; say WHICH call refused. */
	for(w_coord_t c = x0; c <= x0 + 16; c += 16)
		if(!server_world_chunk_resident(w, WCOORD_CHUNK_OFFSET(c),
										WCOORD_CHUNK_OFFSET(z))) {
			struct server_chunk* loaded = NULL;
			bool rc = server_world_load_chunk(w, WCOORD_CHUNK_OFFSET(c),
											  WCOORD_CHUNK_OFFSET(z), &loaded);
			if(!rc || !loaded) {
				printf("  [build_portal: server_world_load_chunk(%d,%d) "
					   "returned %d, chunk %p (dim %d)]\n",
					   (int)WCOORD_CHUNK_OFFSET(c), (int)WCOORD_CHUNK_OFFSET(z),
					   (int)rc, (void*)loaded, (int)w->dimension);
				return false;
			}
		}
	bool built = true;
	w->editing_blocks = true;
	for(int pass = 0; pass < 2; pass++)
		for(w_coord_t a = 0; a < 4; a++)
			for(w_coord_t y = y0; y <= y0 + 4; y++) {
				bool frame = a == 0 || a == 3 || y == y0 || y == y0 + 4;
				if(frame != (pass == 0))
					continue;
				bool rc = server_world_set_block(w, x0 + a, y, z,
					(struct block_data) {.type = frame ? BLOCK_OBSIDIAN
						: BLOCK_PORTAL});
				if(!rc)
					printf("  [build_portal: server_world_set_block(%d,%d,%d,"
						   "%s) returned 0; chunk (%d,%d) resident %d "
						   "(dim %d)]\n", (int)(x0 + a), (int)y, (int)z,
						   frame ? "obsidian" : "portal",
						   (int)WCOORD_CHUNK_OFFSET(x0 + a),
						   (int)WCOORD_CHUNK_OFFSET(z),
						   (int)server_world_chunk_resident(w,
							   WCOORD_CHUNK_OFFSET(x0 + a),
							   WCOORD_CHUNK_OFFSET(z)),
						   (int)w->dimension);
				built &= rc;
				clin_update_drain();
			}
	w->editing_blocks = false;
	return built;
}

/* Exact pose bytes (Sol review 2, D): position, rotation and velocity fields
 * hashed through their object representation -- no float->integer
 * conversion, so negative coordinates are well defined. */
static uint64_t parity_pose(uint64_t h, const struct server_player* p) {
	const double d[3] = {p->x, p->y, p->z};
	const float f[5] = {p->rx, p->ry, p->vel_x, p->vel_y, p->vel_z};
	const unsigned char* b = (const unsigned char*)d;
	for(size_t i = 0; i < sizeof d; i++) {
		h ^= b[i];
		h *= 1099511628211ull;
	}
	b = (const unsigned char*)f;
	for(size_t i = 0; i < sizeof f; i++) {
		h ^= b[i];
		h *= 1099511628211ull;
	}
	return h;
}

/* Frame cells (obsidian/portal: coordinates relative to the landing, id,
 * metadata) in a 33x33 x full-height box of `w` around the body. */
static bool parity_frame(struct server_world* w, const struct server_player* g,
						 uint64_t* digest, unsigned* portal_cells) {
	const w_coord_t lx = (w_coord_t)floor(g->x), lz = (w_coord_t)floor(g->z);
	for(w_coord_t x = lx - 16; x <= lx + 16; x++)
		for(w_coord_t z = lz - 16; z <= lz + 16; z++)
			for(w_coord_t y = 0; y < WORLD_HEIGHT; y++) {
				struct block_data b = {0};
				if(!server_world_get_block(w, x, y, z, &b))
					return false;
				if(b.type != BLOCK_PORTAL && b.type != BLOCK_OBSIDIAN)
					continue;
				*portal_cells += b.type == BLOCK_PORTAL;
				*digest = parity_fnv(*digest, (uint32_t)(x - lx));
				*digest = parity_fnv(*digest, (uint32_t)y);
				*digest = parity_fnv(*digest, (uint32_t)(z - lz));
				*digest = parity_fnv(*digest,
					(uint32_t)b.type << 8 | b.metadata);
			}
	return true;
}

static void parity_record_or_compare(const char* which, const char* tag,
									 bool near, uint64_t digest,
									 unsigned cells) {
	char path[96];
	snprintf(path, sizeof path, "%s_teleporter_parity_%s.txt", TEST_NAME, tag);
	if(!near) {
		FILE* f = fopen(path, "w");
		check(f != NULL, "parity: pin-leg digest recorded");
		if(f) {
			fprintf(f, "%016llx %u\n", (unsigned long long)digest, cells);
			fclose(f);
		}
		return;
	}
	unsigned long long want = 0;
	unsigned want_cells = 0;
	FILE* f = fopen(path, "r");
	bool have = f && fscanf(f, "%llx %u", &want, &want_cells) == 2;
	if(f)
		fclose(f);
	char label[220];
	snprintf(label, sizeof label, "%s: pinned leg == no-eviction leg (pose + "
			 "frame digest %016llx vs %016llx, portal cells %u vs %u)", which,
			 want, (unsigned long long)digest, want_cells, cells);
	check(have && want == digest && want_cells == cells, label);
	remove(path);
}

/* Sol review 2 (B): an EXISTING portal in a STREAMED SECONDARY world (the
 * Nether, slot 1), outside the pose box, with the partner far away in that
 * same world -- the a2w evictor must keep the winner chunks (Beta
 * chunkLoadOverride parity). "pin": the host stands 16 chunks from the
 * portal; "near": the host stands on it (nothing there is ever evicted). */
static void run_teleporter_parity_secondary(const char* which, bool near) {
	struct server_local* s = colcap_guest_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	body_preload(s, h, chunk_center(2), chunk_center(2));
	body_preload(s, g, chunk_center(34), chunk_center(2));
	body_place(s, h, chunk_center(2), chunk_center(2));
	body_place(s, g, chunk_center(34), chunk_center(2));
	(void)two_until_held(s, COLCAP_TICK_BUDGET, 1);
	bool ok = colcap_transfer(s, h, WORLD_DIM_NETHER, "parity2: host -> Nether");
	(void)two_until_held(s, COLCAP_TRANSFER_BUDGET, COLCAP_MOVE_HOLD);
	struct server_world* nw = server_local_world_of(s, h);
	g = s->guest_actor;
	/* the guest's Nether base (OW chunk 34 / 8 = Nether chunk 4), and an
	 * existing portal 3 chunks +X of it with its frame on a chunk seam (two
	 * winner chunks): outside the pose box (radius 2) yet nearer than the
	 * host's own arrival portal near the Nether origin, so FIND picks it. */
	const w_coord_t bx = (w_coord_t)floor(g->x / 8.0);
	const w_coord_t ex0 = (WCOORD_CHUNK_OFFSET(bx) + 3) * 16 + 15;
	const w_coord_t ez = (w_coord_t)floor(g->z / 8.0), ey = 70;
	const double hx = near ? ex0 + 2.5 : chunk_center(WCOORD_CHUNK_OFFSET(ex0) + 16);
	body_preload(s, h, hx, ez + 0.5);
	body_place(s, h, hx, ez + 0.5);
	(void)two_until_held(s, COLCAP_TRANSFER_BUDGET, COLCAP_MOVE_HOLD);
	check(ok && nw && nw->dimension == WORLD_DIM_NETHER
		  && colcap_build_portal(nw, ex0, ez, ey),
		  "parity2: existing Nether portal written in the streamed slot");
	for(int i = 0; i < 40; i++)
		two_tick(s);
	ok = ok && colcap_transfer(s, g, WORLD_DIM_NETHER, "parity2: guest -> Nether");
	g = s->guest_actor;
	uint64_t digest = parity_pose(1469598103934665603ull, g);
	unsigned cells = 0;
	ok = ok && parity_frame(server_local_world_of(s, g), g, &digest, &cells);
	printf("PARITY %s[%s]: landed %.3f %.3f %.3f (frame x %d..%d) portal cells "
		   "%u digest %016llx\n", TEST_NAME, which, g->x, g->y, g->z, (int)ex0,
		   (int)ex0 + 3, cells, (unsigned long long)digest);
	check(ok && g->x > ex0 && g->x < ex0 + 3 && cells > 0,
		  "parity2: the guest landed on the EXISTING Nether portal");
	parity_record_or_compare(which, "secondary", near, digest, cells);
	uint32_t exc = 0, peak = 0;
	server_transfer_cap_excursions(&exc, &peak);
	check(exc == 0, "parity2: no allocation-time excursion over the ceiling");
	colcap_teardown(s);
}

/* Sol review 2 (A): eviction never stalls on a pinned winner and PREPARING is
 * bounded. Overworld slot 0 filled to cap - 1 with evictable columns, an
 * existing portal whose seam chunks are NOT resident (missing winner):
 *  1. with PREPARING capped at 3 slices the return trip must roll back
 *     cleanly, counted (server_transfer_prepare_timeouts), source restored;
 *  2. with the default bound the same trip must COMPLETE onto that portal
 *     within the declared budget (eviction proceeds around the pin). */
static void run_prepare_bounded(const char* which) {
	struct server_local* s = colcap_guest_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	body_preload(s, h, chunk_center(2), chunk_center(2));
	body_preload(s, g, chunk_center(18), chunk_center(2));
	body_place(s, h, chunk_center(2), chunk_center(2));
	body_place(s, g, chunk_center(18), chunk_center(2));
	(void)two_until_held(s, COLCAP_TICK_BUDGET, 1);
	bool ok = colcap_transfer(s, g, WORLD_DIM_NETHER, "bounded: guest -> Nether");
	(void)two_until_held(s, COLCAP_TRANSFER_BUDGET, COLCAP_MOVE_HOLD);
	g = s->guest_actor;
	struct server_world* ow = &s->worlds[0];
	const w_coord_t bx = (w_coord_t)floor(g->x * 8.0);
	const w_coord_t ex0 = (WCOORD_CHUNK_OFFSET(bx) + 4) * 16 + 15;
	const w_coord_t ez = (w_coord_t)floor(g->z * 8.0);
	check(ok && colcap_build_portal(ow, ex0, ez, 70),
		  "bounded: existing Overworld portal written");
	/* let the evictor retire the portal's columns (saved), then fill */
	for(int i = 0; i < 200; i++)
		two_tick(s);
	unsigned filled = 0;
	for(w_coord_t x = 1000; occupied(s) < SERVER_TRANSFER_RESIDENT_COLUMN_CAP - 1
		&& filled < 400; x++, filled++) {
		struct server_chunk* sc = NULL;
		if(!server_world_load_chunk(ow, x, 1000, &sc) || !sc)
			break;
	}
	const bool seam_missing = !server_world_chunk_resident(ow,
		WCOORD_CHUNK_OFFSET(ex0), WCOORD_CHUNK_OFFSET(ez))
		|| !server_world_chunk_resident(ow, WCOORD_CHUNK_OFFSET(ex0 + 1),
			WCOORD_CHUNK_OFFSET(ez));
	printf("BOUNDED %s[%s]: occupied %" PRIu32 " of %u, winner seam missing %d\n",
		   TEST_NAME, which, occupied(s),
		   (unsigned)SERVER_TRANSFER_RESIDENT_COLUMN_CAP, (int)seam_missing);
	check(seam_missing && occupied(s) >= SERVER_TRANSFER_RESIDENT_COLUMN_CAP - 9,
		  "bounded: setup -- near-full occupancy and a missing winner chunk");
	/* 1. bounded failure */
	const uint32_t to0 = server_transfer_prepare_timeouts();
	server_transfer_test_set_prepare_max_ticks(3);
	uint32_t id = 0;
	enum server_transfer_rc rc
		= server_transfer_request(s, g, WORLD_DIM_OVERWORLD, &id);
	enum server_transfer_state st = SERVER_TRANSFER_ROLLED_BACK;
	unsigned t1 = 0;
	for(; rc == SERVER_TRANSFER_OK && t1 < COLCAP_TRANSFER_BUDGET; t1++) {
		st = server_transfer_state_of(s, id);
		if(st == SERVER_TRANSFER_COMMITTED || st == SERVER_TRANSFER_ROLLED_BACK)
			break;
		two_tick(s);
	}
	server_transfer_test_set_prepare_max_ticks(0);
	struct server_world* gw = server_local_world_of(s, s->guest_actor);
	char label[220];
	snprintf(label, sizeof label, "bounded: a Teleporter capped at 3 slices "
			 "rolls back cleanly in %u ticks (rc %d, state %d, timeouts +%u), "
			 "the guest still in the Nether", t1, (int)rc, (int)st,
			 (unsigned)(server_transfer_prepare_timeouts() - to0));
	check(rc == SERVER_TRANSFER_OK && st == SERVER_TRANSFER_ROLLED_BACK
		  && server_transfer_prepare_timeouts() == to0 + 1 && gw
		  && gw->dimension == WORLD_DIM_NETHER, label);
	(void)two_until_held(s, COLCAP_TRANSFER_BUDGET, COLCAP_MOVE_HOLD);
	/* 2. the same trip with the default bound completes */
	ok = colcap_transfer(s, s->guest_actor, WORLD_DIM_OVERWORLD,
						 "bounded: guest -> Overworld (default bound)");
	g = s->guest_actor;
	snprintf(label, sizeof label, "bounded: with the default bound the trip "
			 "COMPLETES onto the existing portal under full occupancy (x %.2f "
			 "vs frame %d..%d)", g->x, (int)ex0, (int)ex0 + 3);
	check(ok && g->x > ex0 && g->x < ex0 + 3, label);
	uint32_t exc = 0, peak = 0;
	server_transfer_cap_excursions(&exc, &peak);
	snprintf(label, sizeof label, "bounded: no allocation-time excursion over "
			 "the ceiling (count %u, peak %u)", (unsigned)exc, (unsigned)peak);
	check(exc == 0, label);
	colcap_teardown(s);
}

/* R5 route regression: real server/client RESET/ACK, generation, population
 * and the Wii column allocator emulation. The harness has no mesher: this
 * proves column demand and resumed simulation; Wii proves warmed GX/MEM2.
 * Run compact tick legs here; the device retains its 90/180-second schedule. */
static unsigned regrow_slabs(void) {
	struct mem2_pool_stat st;
	uint8_t occ[16];
	mem2_pool_selftest_stats(&st, occ, 16);
	return st.slabs;
}
static void run_regrow(const char* which, bool restream) {
	struct server_local* s = colcap_fixture(256);
	unsigned peak[3] = {0}, slabs_max[3] = {0}, slabs_min[3] = {99, 99, 99};
	unsigned phase = 0;
	for(unsigned t = 1; t <= 1200; t++) {
		if(t == 401) {
			phase = 1;
			assert(server_local_set_anchor_offset(s, 0));
			world_set_anchor_offset(0);
		} else if(t == 801) {
			phase = 2;
			assert(s->player.finished_loading && !s->input_ack_pending);
			assert(clin_input_epoch() != 0);
			assert(server_local_set_anchor_offset(s, 256));
			world_set_anchor_offset(256);
			if(restream)
				clin_begin_live_stream_recovery();
		}
		/* Host walks only when the ordinary loading barrier permits it. */
		if(s->player.finished_loading && !s->input_ack_pending)
			s->player.z += 0.1;
		tick(s);
		client_acks_marker(s);
		unsigned occ = occupied(s), sl = regrow_slabs();
		if(occ > peak[phase]) peak[phase] = occ;
		if(sl > slabs_max[phase]) slabs_max[phase] = sl;
		if(sl < slabs_min[phase]) slabs_min[phase] = sl;
	}
	printf("REGROW %s[%s]: peak columns [%u,%u,%u], slabs min [%u,%u,%u] "
		   "max [%u,%u,%u]\n", TEST_NAME, which, peak[0], peak[1], peak[2],
		   slabs_min[0], slabs_min[1], slabs_min[2],
		   slabs_max[0], slabs_max[1], slabs_max[2]);
	check(mem2_column_pool_emulated(), "real column pool emulation active");
	check(slabs_min[1] <= 6, "reunited: pool drains to <= 6 slabs");
	check(slabs_max[0] > slabs_min[1], "reunited: previously grown slabs returned");
	check(peak[2] > 6 * 32, "reseparated: real demand exceeds six slabs");
	check(slabs_max[2] >= 7, "reseparated: warmed regrowth to >= 7 slabs");
	check(s->player.finished_loading && !s->input_ack_pending
		  && !s->stream_recovery_epoch, "production restream completes RESET/ACK");
	uint64_t before = s->world_time;
	tick(s);
	check(s->world_time > before, "world simulation resumes after ACK");
	world_set_anchor_sep(0);
	colcap_teardown(s);
}

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "sep256";
	harness_init_payloads(strcmp(which, "sep256_budget_off") != 0);
	if(!strcmp(which, "sep256") || !strcmp(which, "sep256_budget_off"))
		run_case(256, which);
	else if(!strcmp(which, "sep0"))
		run_case(0, which);
	else if(!strcmp(which, "ow_ow_sep256"))
		run_two_body(which, WORLD_DIM_OVERWORLD, WORLD_DIM_OVERWORLD, false,
					 false);
	else if(!strcmp(which, "ow_ow_sep256_hostfar"))
		run_two_body(which, WORLD_DIM_OVERWORLD, WORLD_DIM_OVERWORLD, true,
					 false);
	else if(!strcmp(which, "nether_nether_sep256"))
		run_two_body(which, WORLD_DIM_NETHER, WORLD_DIM_NETHER, false, false);
	else if(!strcmp(which, "nether_nether_sep256_hostfar"))
		run_two_body(which, WORLD_DIM_NETHER, WORLD_DIM_NETHER, true, false);
	else if(!strcmp(which, "mixed_host_ow_guest_nether_sep256"))
		run_two_body(which, WORLD_DIM_OVERWORLD, WORLD_DIM_NETHER, false,
					 false);
	else if(!strcmp(which, "mixed_host_nether_guest_ow_sep256"))
		run_two_body(which, WORLD_DIM_NETHER, WORLD_DIM_OVERWORLD, true,
					 false);
	else if(!strcmp(which, "mixed_host_ow_guest_nether_coincident"))
		run_two_body(which, WORLD_DIM_OVERWORLD, WORLD_DIM_NETHER, false, true);
	else if(!strcmp(which, "mixed_host_nether_guest_ow_coincident"))
		run_two_body(which, WORLD_DIM_NETHER, WORLD_DIM_OVERWORLD, false, true);
	else if(!strcmp(which, "republish_keeps_sent"))
		run_republish_keeps_sent(which);
	else if(!strcmp(which, "heap_walk_corrupt"))
		run_heap_walk_corrupt(which);
	else if(!strcmp(which, "saturated_transfers_reunion"))
		run_saturated_transfers(which);
	else if(!strcmp(which, "teleporter_parity_pin"))
		run_teleporter_parity(which, false, false);
	else if(!strcmp(which, "teleporter_parity_near"))
		run_teleporter_parity(which, true, false);
	else if(!strcmp(which, "teleporter_parity_existing_pin"))
		run_teleporter_parity(which, false, true);
	else if(!strcmp(which, "teleporter_parity_existing_near"))
		run_teleporter_parity(which, true, true);
	else if(!strcmp(which, "teleporter_parity_secondary_pin"))
		run_teleporter_parity_secondary(which, false);
	else if(!strcmp(which, "teleporter_parity_secondary_near"))
		run_teleporter_parity_secondary(which, true);
	else if(!strcmp(which, "prepare_bounded"))
		run_prepare_bounded(which);
	else if(!strcmp(which, "cap_retry_rng"))
		run_cap_retry_rng(which);
	else if(!strcmp(which, "regrow_route"))
		run_regrow(which, true);
	else if(!strcmp(which, "regrow_route_control"))
		run_regrow(which, false);
	else if(!strcmp(which, "telemetry_contract"))
		run_telemetry_contract(which);
	else {
		fprintf(stderr, "unknown case %s\n", which);
		return 2;
	}
	return harness_result(TEST_NAME, which);
}
