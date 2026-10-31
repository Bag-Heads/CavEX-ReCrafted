/* Copyright (c) 2026 CaveWii_v2 contributors
 * Server-owned LIVE experimental-split transition (Astra option A, S-A1):
 * SRPC_SPLIT_SET / CRPC_SPLIT_STATE on a server whose thread has started,
 * applied between ticks, generation-tagged, idempotent, and never ejecting an
 * admitted guest.
 *
 * The world generation is the PRODUCTION one -- s->input_epoch_counter, armed
 * and acknowledged by the real input handoff (the same stamp SRPC_STREAM_RECOVER
 * and SRPC_MINECART_RIDE_INTENT carry). Nothing here invents a counter: the
 * generation advance in A4/A5 is a real production restream (SRPC_STREAM_RECOVER
 * -> CRPC_STREAM_RESET -> a fresh epoch marker -> the client's ACK), which is
 * the same input/voxel-generation barrier an unload/reload raises. The fixture
 * owns its world outside the SRPC_LOAD_WORLD/UNLOAD_WORLD lifecycle, so a
 * literal UNLOAD here would tear down the harness's own archive rather than
 * test anything about the switch.
 *
 * The residency block is counted by pointer identity through --wrap=mem2_alloc /
 * --wrap=mem2_free, so "exactly one block" and "no leak" are measured, not
 * inferred. Allocation failure is injected through the same existing
 * --wrap=mem2_alloc hook the column-dedup tests use; no test-only hook was
 * added to production code.
 *
 * LIMIT: this runs under ctest, not ASan, and the fixture's server has no
 * thread of its own -- every transition below is driven synchronously from the
 * test thread through the server's own mailbox. So A5 is named for what it
 * actually checks: MAILBOX ORDERING (R-A8 review note 6 -- it was called
 * "teardown races" and contains no race). The two orderings it pins are real
 * and are the ones that matter: a disable that precedes an admission in the same
 * drain, and a request left in the mailbox when the reload barrier goes up. A
 * use-after-free/double-free proof, and any proof about two threads racing,
 * needs the WSL ASan soak target and a threaded server; neither is part of this
 * run. Stated rather than implied.
 *
 * A8 closes the two SERVER-side leftovers R-A8 left open (S-A5): the
 * reserved-zero stamp that used to be dropped without an ack (A8-L7) and the
 * guest_gameplay clear inside server_local_unbind_guest that did not reconcile
 * the residency block / accept class / split policy (A8-L9).
 *
 * A7 adds the CLIENT half of "a live OFF never ejects a guest" (R-A8 A8-H3):
 * A1-A6 only ever exercised source/network, so the client-side starvation --
 * a guest ATTACHED-but-not-admitted left with its card unretired and its
 * controller claimed, and a frozen guest anchor -- was invisible to them.
 */
#ifndef CAVEWII_SERVER_LOCAL_TEST
#define CAVEWII_SERVER_LOCAL_TEST 1
#endif
#include "support/split_residency_fixture.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "item/items.h"
#include "item/recipe.h"
#include "network/residency.h"
#include "network/server_local_guest.h"
#include "platform/input.h"
#include <stdio.h>

/* ---- residency-block accounting + failure injection ---------------------- */
void* __real_mem2_alloc(size_t);
void __real_mem2_free(void*);
static size_t residency_bytes;
static unsigned res_allocs, res_frees, res_failures;
static bool fail_residency;
#define RES_TRACK_MAX 8
static void* res_live[RES_TRACK_MAX];
static unsigned res_live_n;

void* __wrap_mem2_alloc(size_t bytes) {
	if(bytes == residency_bytes && fail_residency) {
		fail_residency = false;
		res_failures++;
		return NULL;
	}
	void* p = __real_mem2_alloc(bytes);
	if(p && bytes == residency_bytes) {
		assert(res_live_n < RES_TRACK_MAX);
		res_live[res_live_n++] = p;
		res_allocs++;
	}
	return p;
}
void __wrap_mem2_free(void* ptr) {
	for(unsigned i = 0; i < res_live_n; i++)
		if(res_live[i] == ptr) {
			res_live[i] = res_live[--res_live_n];
			res_frees++;
			break;
		}
	__real_mem2_free(ptr);
}

/* ---- transport harness (shape shared with test_column_dedup_recovery) ---- */
static unsigned markers;
static struct client_rpc last_ack;
static unsigned acks;
static uint32_t update_done;

static void consumed(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == CRPC_PLAYER_POS && rpc->payload.player_pos.input_epoch)
		markers++;
	if(rpc->type == CRPC_SPLIT_STATE) {
		last_ack = *rpc;
		acks++;
	}
}
static void* update_server(void* ptr) {
	server_local_test_update_once(ptr);
	__atomic_store_n(&update_done, 1, __ATOMIC_RELEASE);
	return NULL;
}
static void step(struct server_local* s) {
	struct thread worker;
	update_done = 0;
	assert(thread_create(&worker, update_server, s, 64));
	ptime_t start = time_get();
	while(!__atomic_load_n(&update_done, __ATOMIC_ACQUIRE)) {
		clin_update_drain();
		assert(time_diff_s(start, time_get()) < 20);
		thread_msleep(1);
	}
	thread_join(&worker);
	clin_update_drain();
}
static void stream_to_marker(struct server_local* s) {
	unsigned before = markers;
	for(unsigned i = 0; i < 2000 && markers == before; i++)
		step(s);
	assert(markers == before + 1 && s->input_ack_pending);
}
static void acknowledge(struct server_local* s) {
	/* Real 30 s readiness fallback: no mesher workers exist in this rig. */
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	step(s);
	assert(!s->input_ack_pending && !s->stream_recovery_epoch);
	assert(s->input_epoch_counter != 0);
}
/* One request -> one processed transition -> one consumed ack. Everything the
 * live path does happens on the server thread's dispatch, between ticks. */
static uint32_t seq_counter;
static bool send_split(struct server_local* s, bool enabled, uint32_t generation) {
	struct server_rpc call = {.type = SRPC_SPLIT_SET,
		.payload.split_set = {.enabled = enabled, .world_generation = generation,
			.request_seq = ++seq_counter}};
	return svin_rpc_send(&call);
}
static void drain_server(struct server_local* s) {
	server_local_test_process_messages(s);
	clin_update_drain();
}
static uint32_t request_split(struct server_local* s, bool enabled,
							  uint32_t generation) {
	unsigned before = acks;
	assert(send_split(s, enabled, generation));
	drain_server(s);
	assert(acks == before + 1);
	assert(last_ack.payload.split_state.request_seq == seq_counter);
	return seq_counter;
}
/* The transport accept class is observable only through the queue boundary:
 * a slot-1 SRPC_PLAYER_POS is admitted iff the class is armed. The dispatcher
 * then discards it (no matching guest generation), so this probe cannot move
 * anything -- exactly the probe test_local_guest uses. */
static bool accept_armed(struct server_local* s, uint64_t generation) {
	struct server_rpc probe = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = s->player.x, .y = s->player.y,
			.z = s->player.z}};
	bool admitted
		= svin_rpc_send_from((struct server_rpc_source) {.slot = 1,
							  .generation = generation}, &probe);
	drain_server(s);
	return admitted;
}

/* ---- guest handshake (shape shared with test_local_guest) ---------------- */
static void pump_handshake(struct server_local* s) {
	client_session_flush_local(&gstate.players[1]);
	server_local_test_process_messages(s);
	clin_update_drain();
}
static void guest_attach(struct server_local* s) {
	struct client_session* client = &gstate.players[1];
	assert(client_session_request_local_join(client));
	pump_handshake(s);
	pump_handshake(s);
	pump_handshake(s);
	assert(client->local_phase == CLIENT_GUEST_ATTACHED && s->guest_confirmed);
}
/* The guest's NORMAL release. The client-driven Leave (SRPC_LOCAL_GUEST /
 * LOCAL_GUEST_LEAVE) first hard-checkpoints the live world, and that stage
 * fails in this fixture for a reason that has nothing to do with the split
 * switch: the hand-built world carries no level_name, so the legacy chest/sign
 * snapshot write has nowhere to go ("stage=chest_snapshot result=fail"). The
 * release itself -- retiring the body, the anchor-1 debt, the policy and the
 * scheduler -- is this function, and it is the very one the checkpointed Leave
 * calls once its checkpoint succeeds. */
static void guest_release(struct server_local* s) {
	assert(s->guest_owned && s->guest_actor);
	server_local_guest_release_checkpointed(s);
	assert(!s->guest_actor && !s->guest_gameplay && !s->guest_entity_id);
	/* Client-half lifetime boundary, same as test_local_guest: the session's
	 * provider tokens died with the actor, so the next Join is a fresh one.
	 *
	 * Retire the session's HEAP-OWNED windows FIRST (S-A8). The guest's
	 * inventory container is malloc'd by client_session_bind_local
	 * (source/game/client_session.c:242 container, :244 its inventories), and
	 * BOTH production release paths hand it back through
	 * client_session_clear_windows: the LOCAL_GUEST_LEFT ACK
	 * (client_session.c:670) and CRPC_WORLD_RESET, which sweeps every local
	 * player (source/network/client_interface.c:2164). This fixture instead
	 * releases the guest by calling server_local_guest_release_checkpointed()
	 * directly, which queues no status ACK, so the memset below would drop
	 * session->windows[] without freeing it -- 630 B per attach (176 B
	 * window_container + 184 B struct inventory + 270 B its 45 slots), which
	 * is exactly what LeakSanitizer reported in the S-A6 soak
	 * (tmp/jobs/mp-split-0912/sa6-asan-report.md: 3780 B in 18 objects over
	 * these six attaches). Harness omission, not a production leak. */
	client_session_clear_windows(&gstate.players[1]);
	memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
	gstate.players[1].visible_window = -1;
	clin_update_drain();
}
/* Place the guest body in the host's own published column so the production
 * readiness predicate can pass (same setup as test_local_guest). */
static void guest_body_home(struct server_local* s) {
	struct server_player* body = s->guest_actor;
	assert(body);
	body->x = s->player.x;
	body->y = s->player.y;
	body->z = s->player.z;
	body->rx = body->ry = 0;
	if(!server_world_is_chunk_loaded(&s->world, 0, 0)) {
		struct server_chunk* loaded = NULL;
		assert(server_world_load_chunk(&s->world, 0, 0, &loaded));
	}
	struct server_chunk* home
		= dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(0, 0));
	assert(home);
	home->sent = true;
	/* The production readiness predicate is "the body's own column has been
	 * PUBLISHED to the client", which is what this pins. */
	assert(server_world_is_chunk_sent(&s->world,
		WCOORD_CHUNK_OFFSET((int)floor(body->x)),
		WCOORD_CHUNK_OFFSET((int)floor(body->z))));
}

/* ======================================================================== */
static void a1_enable_after_thread_start(struct server_local* s) {
	uint32_t gen = s->input_epoch_counter;
	/* The BOOT setter is the one that must now refuse: the server thread owns
	 * these fields once it is running, so there is no cross-thread write. */
	assert(s->thread_started);
	assert(!server_local_set_split_enabled(s, true));
	assert(!s->split_enabled && !s->residency);
	assert(!accept_armed(s, 7));

	unsigned allocs_before = res_allocs;
	request_split(s, true, gen);
	assert(last_ack.payload.split_state.ok);
	assert(last_ack.payload.split_state.enabled);
	assert(last_ack.payload.split_state.generation == gen);
	assert(last_ack.payload.split_state.reason[0] == '\0');
	assert(s->split_enabled && s->residency);
	assert(res_allocs == allocs_before + 1 && res_live_n == 1
		   && res_live[0] == (void*)s->residency);
	assert(accept_armed(s, 7));
	/* Idempotent repeat: acknowledged, no second allocation. */
	request_split(s, true, gen);
	assert(last_ack.payload.split_state.ok
		   && last_ack.payload.split_state.enabled);
	assert(res_allocs == allocs_before + 1 && s->residency == res_live[0]);
	puts("PASS A1 live enable after thread start: boot setter refuses, "
		 "SRPC_SPLIT_SET acked ok on the server thread, accept class armed, "
		 "exactly one residency block, repeat request is a no-op ack");
}

static void a2_toggle_cycle(struct server_local* s) {
	uint32_t gen = s->input_epoch_counter;
	unsigned allocs = res_allocs, frees = res_frees;

	request_split(s, false, gen);
	assert(last_ack.payload.split_state.ok
		   && !last_ack.payload.split_state.enabled);
	assert(!s->split_enabled && !s->residency);
	assert(res_frees == frees + 1 && res_live_n == 0);
	assert(!accept_armed(s, 7));

	request_split(s, true, gen);
	assert(last_ack.payload.split_state.ok && s->split_enabled && s->residency);
	assert(res_allocs == allocs + 1);

	/* 50 rapid toggles: the last request decides, every request is answered,
	 * at most one block is live at any point, and nothing leaks. The switch is
	 * already ON here, so the first request is also an idempotent repeat and
	 * the last one (i == 49) lands on OFF. */
	bool want = false;
	for(unsigned i = 0; i < 50; i++) {
		want = (i % 2) == 0;
		unsigned before = acks;
		request_split(s, want, gen);
		assert(acks == before + 1 && last_ack.payload.split_state.ok);
		assert(last_ack.payload.split_state.enabled == want);
		assert(s->split_enabled == want);
		assert((s->residency != NULL) == want);
		assert(res_live_n == (want ? 1u : 0u));
	}
	assert(!want && !s->split_enabled && !s->residency);
	assert(res_allocs == res_frees && res_live_n == 0);
	assert(!accept_armed(s, 7));
	puts("PASS A2 disable / re-enable / 50 rapid toggles: final state matches "
		 "the last request, at most one residency block live, allocs == frees "
		 "(no leak), accept class follows the switch");
}

static void a3_allocation_failure(struct server_local* s) {
	uint32_t gen = s->input_epoch_counter;
	unsigned allocs = res_allocs, frees = res_frees;
	assert(!s->split_enabled && !s->residency);

	fail_residency = true;
	request_split(s, true, gen);
	assert(!fail_residency && res_failures == 1);
	assert(!last_ack.payload.split_state.ok);
	assert(!last_ack.payload.split_state.enabled);
	assert(last_ack.payload.split_state.reason[0] != '\0');
	assert(last_ack.payload.split_state.generation == gen);
	/* Nothing half-applied: OFF, no block, accept class closed. */
	assert(!s->split_enabled && !s->residency);
	assert(res_allocs == allocs && res_frees == frees && res_live_n == 0);
	assert(!accept_armed(s, 7));
	printf("PASS A3 allocation failure: ack ok=false reason=\"%s\", state stays "
		   "OFF, no block allocated, accept class closed\n",
		   last_ack.payload.split_state.reason);
}

/* Advance the world's input/voxel generation with production code only: the
 * stable-world restream (SRPC_STREAM_RECOVER -> CRPC_STREAM_RESET -> a fresh
 * epoch marker -> the client's ACK). This is the same input-generation barrier
 * an unload/reload raises, and it is the only one the fixture's hand-built
 * world can take without tearing down its own archive. */
static void reload_generation(struct server_local* s) {
	uint32_t before = s->input_epoch_counter;
	struct server_rpc recover = {.type = SRPC_STREAM_RECOVER,
		.payload.stream_recover = {.epoch = before}};
	server_local_test_process_rpc(s, &recover);
	assert(!s->player.finished_loading && s->stream_recovery_epoch == before);
	stream_to_marker(s);
	acknowledge(s);
	assert(s->input_epoch_counter == before + 1);
}

static void a4_stale_generation(struct server_local* s) {
	uint32_t stale = s->input_epoch_counter;
	assert(!s->split_enabled);

	/* (a) the enable request is AUTHORED in generation N ... */
	struct server_rpc call = {.type = SRPC_SPLIT_SET,
		.payload.split_set = {.enabled = true, .world_generation = stale,
			.request_seq = ++seq_counter}};
	uint32_t stale_seq = seq_counter;

	/* (b) ... the world reloads to N+1 before it is processed ... */
	reload_generation(s);
	uint32_t fresh = s->input_epoch_counter;
	assert(fresh == stale + 1);

	/* (c) ... and it is rejected, reporting the generation that actually
	 * answered so the requester can resynchronise instead of retrying blind. */
	unsigned before = acks;
	server_local_test_process_rpc(s, &call);
	clin_update_drain();
	assert(acks == before + 1);
	assert(last_ack.payload.split_state.request_seq == stale_seq);
	assert(!last_ack.payload.split_state.ok);
	assert(!last_ack.payload.split_state.enabled);
	assert(!strcmp(last_ack.payload.split_state.reason, "stale generation"));
	assert(last_ack.payload.split_state.generation == fresh);
	assert(!s->split_enabled && !s->residency);
	assert(!accept_armed(s, 7));

	/* (d) the replacement generation is consistent and usable. */
	request_split(s, true, fresh);
	assert(last_ack.payload.split_state.ok
		   && last_ack.payload.split_state.enabled
		   && last_ack.payload.split_state.generation == fresh);
	assert(s->split_enabled && s->residency);
	printf("PASS A4 stale generation: request stamped %u rejected after the "
		   "world reloaded to %u (reason=\"stale generation\"), state OFF and "
		   "consistent with %u, a request stamped %u then applies\n",
		   stale, fresh, fresh, fresh);
}

/* Mailbox ORDERING around the transition, not a thread race (see the LIMIT
 * note at the top of this file). */
static void a5_teardown_ordering(struct server_local* s) {
	uint32_t gen = s->input_epoch_counter;
	assert(s->split_enabled && s->residency);

	/* (i) a disable ORDERED AHEAD of an admission: the disable is first in the
	 * mailbox, so the admission that follows it must be refused -- and must not
	 * leave a body half-admitted behind a freed scheduler. The admission here is
	 * a direct server_local_guest_gameplay call made AFTER drain_server returns,
	 * i.e. the ordering is established by construction rather than by two
	 * threads contending; that is exactly what this leg claims. */
	guest_attach(s);
	guest_body_home(s);
	struct server_rpc_source guest
		= {.slot = 1, .generation = s->guest_generation};
	assert(send_split(s, false, gen));
	drain_server(s);
	assert(last_ack.payload.split_state.ok
		   && !last_ack.payload.split_state.enabled);
	assert(!s->split_enabled && !s->residency && !s->guest_gameplay);
	assert(!server_local_guest_gameplay(s, guest));
	assert(!s->guest_gameplay && !s->guest_entity_id && s->guest_actor);
	assert(!server_local_two_anchor(s));

	/* (ii) a request left IN THE MAILBOX when the teardown barrier goes up: the
	 * reload drains it mid-flight (the update loop's own
	 * svin_process_owned_messages), so it is answered "world reloading" rather
	 * than applied across the barrier. Nothing is applied, nothing crashes, and
	 * the block accounting stays balanced. */
	assert(send_split(s, true, gen));
	uint32_t pending_seq = seq_counter;
	unsigned before = acks;
	reload_generation(s);
	assert(acks == before + 1);
	assert(last_ack.payload.split_state.request_seq == pending_seq);
	assert(!last_ack.payload.split_state.ok && !s->split_enabled);
	assert(!strcmp(last_ack.payload.split_state.reason, "world reloading"));
	assert(!s->residency && res_live_n == 0 && res_allocs == res_frees);

	/* ...and the guest that was attached across all of that still releases
	 * cleanly, freeing nothing twice. */
	guest_release(s);
	assert(!s->residency && res_allocs == res_frees);
	puts("PASS A5 teardown ordering: a disable ordered ahead of an admission "
		 "refuses the body with no scheduler left behind; a request left in the "
		 "mailbox across the reload barrier is answered \"world reloading\", "
		 "never applied; allocs == frees, no crash (LIMIT: mailbox ORDERING, "
		 "not a thread race -- no ASan soak and no server thread in this run)");
}

static void a6_active_guest_lock(struct server_local* s) {
	uint32_t gen = s->input_epoch_counter;
	int configured_far = s->far_view_distance;
	assert(!s->split_enabled);

	request_split(s, true, gen);
	assert(last_ack.payload.split_state.ok && s->residency);
	void* block = s->residency;
	unsigned frees = res_frees;

	guest_attach(s);
	guest_body_home(s);
	struct server_rpc_source guest
		= {.slot = 1, .generation = s->guest_generation};
	assert(server_local_guest_gameplay(s, guest));
	assert(s->guest_gameplay && server_local_two_anchor(s));
	int split_far = s->far_view_distance;
	assert(split_far == server_local_view_radius(s) && split_far < configured_far);

	/* OFF with a body admitted: the guest is NOT ejected. Its scheduler block,
	 * its radius/far-ring policy and its transport class all survive. */
	request_split(s, false, gen);
	assert(last_ack.payload.split_state.ok
		   && !last_ack.payload.split_state.enabled);
	assert(!s->split_enabled);
	assert(s->guest_gameplay && s->guest_actor && s->guest_entity_id);
	assert(s->residency == block && res_frees == frees);
	assert(server_local_two_anchor(s) && s->far_view_distance == split_far);
	assert(accept_armed(s, s->guest_generation));
	/* ...including the retry the client keeps sending while its terrain streams:
	 * a duplicate request that races the live OFF must still be GRANTED for the
	 * body already playing. Answering DENIED here would clear the client's
	 * local_gameplay_ready and freeze the second view while the server went on
	 * simulating that body -- an ejection in all but name. */
	assert(server_local_guest_gameplay(s, guest));
	assert(s->guest_gameplay && s->far_view_distance == split_far);

	/* Normal release frees the block and restores the single-player policy. */
	guest_release(s);
	assert(!s->guest_gameplay && !s->guest_actor);
	assert(!s->residency && res_frees == frees + 1);
	assert(!server_local_two_anchor(s)
		   && s->far_view_distance == configured_far);
	assert(!accept_armed(s, 7));

	/* A NEW join while the switch is off reaches ATTACHED and is then refused
	 * gameplay -- "no new joins", enforced at the admission gate. */
	guest_attach(s);
	guest_body_home(s);
	struct server_rpc_source rejoin
		= {.slot = 1, .generation = s->guest_generation};
	assert(!server_local_guest_gameplay(s, rejoin));
	assert(!s->guest_gameplay && !s->residency);
	guest_release(s);
	puts("PASS A6 active-guest lock: OFF keeps the admitted guest, its "
		 "scheduler block and its radius/far-ring policy; the block is freed "
		 "and the policy restored at the guest's normal release; a new join is "
		 "refused gameplay while the switch is off");
}

/* The client-side guest decision main.c takes each frame, expressed exactly as
 * main.c expresses it (screen_options_split_guest_maintain /
 * screen_options_split_guest_refuse over the live session's own phase). */
static bool client_maintains(const struct client_session* guest) {
	return screen_options_split_guest_maintain(
		guest->local_phase != CLIENT_GUEST_IDLE, guest->local_gameplay_ready);
}
static bool client_refuses(const struct client_session* guest) {
	bool joining = guest->local_phase == CLIENT_GUEST_REQUESTED
		|| guest->local_phase == CLIENT_GUEST_INSTALLING
		|| guest->local_phase == CLIENT_GUEST_CONFIRMING
		|| guest->local_phase == CLIENT_GUEST_ATTACHED;
	return screen_options_split_guest_refuse(joining,
											 guest->local_gameplay_ready);
}
/* Retire the row's pending toggle against the ack the client just consumed --
 * the same one line main.c runs after clin_update. Returns the ACKNOWLEDGED
 * state, i.e. what input_split_join_arm follows. */
static bool client_consume_ack(uint32_t expect_seq, bool expect_enabled) {
	struct clin_split_ack ack;
	assert(clin_split_last_ack(&ack));
	assert(ack.valid && ack.ok && ack.request_seq == expect_seq
		   && ack.enabled == expect_enabled);
	return screen_options_split_poll(&ack, 0.0F);
}

static void a7_client_guest_under_off(struct server_local* s) {
	uint32_t gen = s->input_epoch_counter;
	struct client_session* guest = &gstate.players[1];
	assert(!s->split_enabled);
	/* With the switch off and no guest at all the client does nothing: the
	 * single-player frame never enters the block. */
	screen_options_split_boot(false);
	assert(!client_maintains(guest));

	/* ---- (i) ATTACHED, never admitted, then a LIVE OFF ------------------ */
	uint32_t on_seq = request_split(s, true, gen);
	assert(s->split_enabled && s->residency);
	assert(client_consume_ack(on_seq, true));
	guest_attach(s);
	assert(guest->local_phase == CLIENT_GUEST_ATTACHED
		   && !guest->local_gameplay_ready);
	/* The controller claim the approved join card represents. */
	assert(input_player_bind(1,
		(struct input_device) {INPUT_DEVICE_KEYBOARD, 0}));
	assert(input_player_frame(1));

	uint32_t off_seq = request_split(s, false, gen);
	assert(!s->split_enabled && !s->residency);
	assert(!client_consume_ack(off_seq, false)); /* new joins disarmed */
	/* The block MUST still run -- this is the A8-H3 fix. Before it, the whole
	 * guest half (settle, screen pin, anchor) went dark with the switch and this
	 * session sat in ATTACHED forever. */
	assert(client_maintains(guest));
	/* ...and the unfinished join is REFUSED rather than abandoned. The server
	 * has already closed its admission gate for every new body, and any
	 * admission it had granted earlier would have reached this client ahead of
	 * the OFF ack, so there is nothing left to wait for. */
	assert(client_refuses(guest));
	input_split_join_settle(false);
	assert(!input_player_frame(1)); /* the claim goes back to the hardware */
	assert(client_session_request_local_leave(guest));
	assert(guest->local_phase == CLIENT_GUEST_LEAVING);
	/* One refusal per join: LEAVING is on its way out and is not re-refused
	 * every frame (which would re-arm a pending action forever). */
	assert(!client_refuses(guest));
	assert(client_maintains(guest)); /* still maintained until it is IDLE */
	/* The fixture's world carries no level_name, so the client-driven Leave's
	 * hard checkpoint cannot complete here (see guest_release's comment); the
	 * release itself is the function that Leave calls once it does. */
	guest_release(s);
	assert(guest->local_phase == CLIENT_GUEST_IDLE);
	/* Back to a single-player frame: nothing to maintain, so the anchor is
	 * released by the last maintained frame and then left alone. */
	assert(!client_maintains(guest));
	assert(!s->residency && res_allocs == res_frees);

	/* ---- (ii) an ADMITTED body under a LIVE OFF: it STAYS --------------- */
	on_seq = request_split(s, true, gen);
	assert(s->split_enabled && s->residency);
	assert(client_consume_ack(on_seq, true));
	guest_attach(s);
	guest_body_home(s);
	assert(input_player_bind(1,
		(struct input_device) {INPUT_DEVICE_KEYBOARD, 0}));
	/* Admitted through the REAL reply, so the CLIENT's local_gameplay_ready is
	 * set by the only production setter rather than by the test. */
	assert(client_session_request_local_gameplay(guest));
	pump_handshake(s);
	pump_handshake(s);
	assert(guest->local_gameplay_ready && s->guest_gameplay);

	off_seq = request_split(s, false, gen);
	assert(!s->split_enabled);
	assert(!client_consume_ack(off_seq, false));
	/* Server half (A6): the body, its block and its policy all survive. */
	assert(s->guest_gameplay && s->guest_actor && s->guest_entity_id);
	assert(s->residency && server_local_two_anchor(s));
	/* Client half: still maintained (so the guest anchor keeps tracking the
	 * body instead of freezing), NOT refused, and the claim is NOT released. */
	assert(client_maintains(guest));
	assert(!client_refuses(guest));
	assert(input_player_frame(1));
	assert(guest->local_gameplay_ready);

	guest_release(s);
	input_player_unbind(1);
	assert(guest->local_phase == CLIENT_GUEST_IDLE);
	assert(!client_maintains(guest));
	assert(!s->residency && res_allocs == res_frees);
	puts("PASS A7 client half of the live OFF: an ATTACHED-not-admitted guest "
		 "is REFUSED and its controller claim released (once, not every frame) "
		 "instead of sitting in ATTACHED forever, the guest block/anchor keep "
		 "running while any guest exists, an ADMITTED body is left completely "
		 "alone with its claim intact, and a single-player frame maintains "
		 "nothing");
}

/* The two SERVER-side leftovers R-A8 left open (S-A5).
 *
 * (i) A8-L7 -- the reserved-zero stamp was the ONE request the handler dropped
 *     in silence, against its own "exactly one CRPC_SPLIT_STATE per request"
 *     contract. It is now answered {ok=false, "invalid request"}.
 *
 * (ii) A8-L9 -- server_local_unbind_guest clears s->guest_gameplay, which the
 *     live switch made load-bearing for the residency block's lifetime, the
 *     transport accept class and the split radius/far-ring policy. The review
 *     asked for a RUNTIME check rather than the reachability argument, and this
 *     is it, in two legs: the production APIs show the path refuses while a
 *     body is admitted (so no reachable call arrives with guest_gameplay true),
 *     and the forced state shows the clear reconciles anyway, so the invariant
 *     no longer depends on that argument holding forever. */
static void a8_invalid_request_and_guest_clear_reconcile(struct server_local* s) {
	uint32_t gen = s->input_epoch_counter;
	int configured_far = s->far_view_distance;
	assert(!s->split_enabled && !s->residency);

	/* ---- (i) A8-L7: generation == 0 is ANSWERED, not dropped ------------ */
	struct server_rpc zero_gen = {.type = SRPC_SPLIT_SET,
		.payload.split_set = {.enabled = true, .world_generation = 0,
			.request_seq = ++seq_counter}};
	uint32_t zero_gen_seq = seq_counter;
	unsigned before = acks;
	server_local_test_process_rpc(s, &zero_gen);
	clin_update_drain();
	assert(acks == before + 1); /* the silent drop would leave this at `before` */
	assert(last_ack.payload.split_state.request_seq == zero_gen_seq);
	assert(!last_ack.payload.split_state.ok);
	assert(!strcmp(last_ack.payload.split_state.reason, "invalid request"));
	/* The ack still reports the SERVER's generation and its unchanged state. */
	assert(last_ack.payload.split_state.generation == gen);
	assert(!last_ack.payload.split_state.enabled);
	assert(!s->split_enabled && !s->residency && !accept_armed(s, 7));

	/* ...and so is request_seq == 0, whose seq the ack ECHOES (zero included:
	 * a requester that never sends zero cannot confuse it with its own). */
	struct server_rpc zero_seq = {.type = SRPC_SPLIT_SET,
		.payload.split_set = {.enabled = true, .world_generation = gen,
			.request_seq = 0}};
	before = acks;
	server_local_test_process_rpc(s, &zero_seq);
	clin_update_drain();
	assert(acks == before + 1);
	assert(last_ack.payload.split_state.request_seq == 0);
	assert(!last_ack.payload.split_state.ok);
	assert(!strcmp(last_ack.payload.split_state.reason, "invalid request"));
	assert(!s->split_enabled && !s->residency && !accept_armed(s, 7));
	/* The rejection is about the STAMP, not a wedged handler: the very next
	 * well-formed request applies normally. */
	request_split(s, true, gen);
	assert(last_ack.payload.split_state.ok && s->split_enabled && s->residency);

	/* ---- (ii) A8-L9, leg 1: unreachable through the production APIs ----- */
	guest_attach(s);
	guest_body_home(s);
	struct server_rpc_source guest
		= {.slot = 1, .generation = s->guest_generation};
	uint64_t guest_gen = s->guest_generation;
	assert(server_local_guest_gameplay(s, guest));
	assert(s->guest_gameplay && s->guest_owned && s->residency);
	void* block = s->residency;
	unsigned frees = res_frees;
	/* THE check the reviewer asked for: while the body is admitted this path
	 * REFUSES (it requires !s->guest_owned; admission requires s->guest_owned),
	 * so it cannot be reached with guest_gameplay true. Nothing is touched. */
	assert(!server_local_unbind_guest(s, &guest));
	assert(s->guest_gameplay && s->guest_actor && s->residency == block
		   && res_frees == frees);

	/* ---- (ii) A8-L9, leg 2: the clear reconciles even so ---------------- */
	/* Set up the state that made the finding interesting: the switch is OFF
	 * while an admitted body keeps the block, the accept class and the split
	 * policy alive (A6's invariant). */
	request_split(s, false, gen);
	assert(!s->split_enabled && s->guest_gameplay);
	assert(s->residency == block && res_frees == frees);
	assert(accept_armed(s, guest_gen) && server_local_two_anchor(s));
	int split_far = s->far_view_distance;
	assert(split_far < configured_far);
	/* Now FORCE the combination no production path produces -- guest_owned
	 * false with guest_gameplay true -- so the clear inside unbind actually
	 * runs. Stated plainly: this is a test poke at the struct, not a reachable
	 * sequence; leg 1 above is the proof of unreachability and this leg is the
	 * proof that the reconcile does not DEPEND on it. */
	struct server_player* actor = s->guest_actor;
	s->guest_owned = false;
	assert(server_local_unbind_guest(s, &guest));
	/* Before the fix this returned true having cleared guest_gameplay and
	 * NOTHING else: the block leaked for the session, svin_split_accept stayed
	 * armed for slot-1 SRPC_PLAYER_POS, and the far ring stayed clamped. */
	assert(!s->guest_gameplay && !s->guest_actor && !s->guest_entity_id);
	assert(!s->residency && res_frees == frees + 1);
	assert(!accept_armed(s, guest_gen));
	assert(!server_local_two_anchor(s)
		   && s->far_view_distance == configured_far);

	/* Restore the fixture to a state the PRODUCTION teardown can take, and let
	 * that teardown free the body (it owns the actor allocation and the client
	 * session lifetime; the forced unbind above only detached it). */
	s->guest_actor = actor;
	s->guest_owned = true;
	s->guest_generation = guest_gen;
	guest_release(s);
	assert(!s->residency && res_allocs == res_frees);
	puts("PASS A8 review leftovers: a {generation==0} and a {request_seq==0} "
		 "SPLIT_SET are ANSWERED ok=false \"invalid request\" instead of "
		 "dropped silently, and the next valid request still applies; "
		 "server_local_unbind_guest REFUSES while a body is admitted (so the "
		 "guest_gameplay clear is unreachable in production) and, forced into "
		 "that state anyway, reconciles -- block freed, accept class closed, "
		 "far ring restored");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	residency_bytes = sizeof(struct residency_scheduler);
	blocks_init();
	items_init();
	recipe_init();
	assert(clin_init() && svin_init());
	/* A7 drives the REAL Options row, whose ack moves
	 * experimental.local_splitscreen in the in-memory config. An empty object
	 * keeps that off the repo's own config.json -- and config_save is NOT
	 * wrapped in this target, so the A8-M6 deferral (no save from the ack seam)
	 * is what makes driving the row from here safe at all. */
	gstate.config_user.root = json_value_init_object();
	assert(gstate.config_user.root);
	gstate.config_user.ephemeral_key_count = 0;
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);

	struct server_local* s = split_fixture();
	/* Preserve simulation/owner state while the transport is stepped, exactly
	 * like the other split-residency fixtures. */
	s->paused = true;
	/* The policy dial: the residency fixtures run at 0, which means "no split
	 * policy at all", so A6 could not observe the far-ring transition. */
	s->split_view_radius = 4;

	/* One real load handoff establishes generation 1. */
	stream_to_marker(s);
	acknowledge(s);
	/* From here the server thread OWNS the switch. The fixture builds its
	 * server without a thread; arming the flag the boot gate keys on is what
	 * makes "after thread start" real for the boot-path refusal in A1. Every
	 * transition below still runs on a dispatch of the server's own mailbox
	 * (server_local_test_process_messages == svin_process_owned_messages, the
	 * same call server_local_update makes between ticks). */
	s->thread_started = true;

	a1_enable_after_thread_start(s);
	a2_toggle_cycle(s);
	a3_allocation_failure(s);
	a4_stale_generation(s);
	a5_teardown_ordering(s);
	a6_active_guest_lock(s);
	a7_client_guest_under_off(s);
	a8_invalid_request_and_guest_clear_reconcile(s);

	/* Leave the switch off so the fixture teardown sees a released block, and
	 * prove the whole run balanced. */
	if(s->split_enabled)
		request_split(s, false, s->input_epoch_counter);
	assert(!s->residency && res_live_n == 0 && res_allocs == res_frees);
	s->thread_started = false;
	clin_test_set_consume_observer(NULL, NULL);
	split_destroy(s);
	printf("test_split_live_toggle: PASS (%u acks, %u residency allocs == %u "
		   "frees, %u injected allocation failures)\n",
		   acks, res_allocs, res_frees, res_failures);
	return 0;
}
