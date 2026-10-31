/* Split-dims B2 -> A2-T admission, client half (lane B, b2-a2t-cap-0927;
 * Astra astra-a2t-0927): capacity reserve/commit/release and landing ready.
 *
 *  R1 flag OFF: every reserve answers DISABLED, nothing is held; a landing
 *     cannot be armed.
 *  R2 reserve OK for the guest slot within budget; commit; release frees
 *     it; tickets are never reused; stale/unknown tickets change nothing.
 *  R3 refusals change nothing: player 0's slot, a busy slot, MEM1 short,
 *     MEM2 short (margins and other reservations counted), a context pool
 *     that cannot take another world.
 *  R4 a flag flip drops every reservation and the pending landing.
 *  L1 landing timing: armed before the world exists -> WAITING through the
 *     install, the view bind and all but the last near column; READY and
 *     reported exactly once on the frame the last column arrives.
 *  L2 a refused report retries on the next frame; never reported twice.
 *  L3 the world replaced after it was seen -> STALE, nothing reported;
 *     a landing armed while the slot still holds the PREVIOUS world waits.
 *  W  wire consumers: CRPC_CAPACITY_REQUEST (reserve/commit/release/refusal
 *     replies; a refused reply never strands a reservation) and
 *     CRPC_LANDING_ARM (arm from the stamp, report from the frame poll,
 *     cancel).
 *  L4 radius 0 needs only the landing column; negative landing coordinates
 *     map to the right column.
 *  F  final contract (lane A reply 2026-09-27): with no sender registered the
 *     replies go out as SRPC_CAPACITY_REPLY {request_id, result, ticket} and
 *     SRPC_LANDING_READY {token, slot, generation} through svin; a
 *     CRPC_WORLD_RELEASE for the landing's world stales the landing, keeps a
 *     drained (retiring=false) world's reservation and returns a retiring
 *     world's (retiring=true); a release for another generation is ignored. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_admission.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "network/server_interface.h"
#include "world.h"

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

static struct client_ctx_registry* reg;

static void host_reset(uint8_t index, uint32_t gen) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = 77, .world_index = index,
			.world_generation = gen}});
}

static void guest_move(uint8_t index, uint32_t gen) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_NETHER,
			.local_entity = 77, .world_index = index,
			.world_generation = gen, .guest_world = true, .guest_view = 1}});
}

static void column(w_coord_t cx, w_coord_t cz, uint8_t index, uint32_t gen) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	clin_process(&(struct client_rpc) {.type = CRPC_CHUNK,
		.world_index = index, .world_generation = gen,
		.payload.chunk = {.x = cx * CHUNK_SIZE, .y = 0, .z = cz * CHUNK_SIZE,
			.sx = CHUNK_SIZE, .sy = WORLD_HEIGHT, .sz = CHUNK_SIZE,
			.slot = slot}});
}

static void flag(bool on) {
	client_ctx_registry_retire(reg, 0);
	client_ctx_registry_retire(reg, 1);
	assert(clin_world_payloads_set_enabled(false));
	assert(clin_world_payloads_set_enabled(on));
}

static uint64_t s_mem1 = 64u << 20, s_mem2 = 32u << 20;
static void budget(struct clin_capacity_budget* out) {
	out->mem1_free = s_mem1;
	out->mem2_free = s_mem2;
}

static unsigned s_sends, s_refuse;
static uint32_t s_last_token, s_last_gen;
static unsigned s_last_slot;
static bool sender(uint32_t token, unsigned slot, uint32_t gen) {
	s_sends++;
	s_last_token = token;
	s_last_slot = slot;
	s_last_gen = gen;
	if(s_refuse) {
		s_refuse--;
		return false;
	}
	return true;
}

static bool held_is(uint64_t m1, uint64_t m2, unsigned n) {
	uint64_t a, b;
	unsigned c;
	clin_capacity_held(&a, &b, &c);
	return a == m1 && b == m2 && c == n;
}

static void test_capacity(void) {
	clin_capacity_set_budget_provider(budget);
	struct clin_capacity_need need = {.mem1_bytes = 8u << 20,
									  .mem2_bytes = 4u << 20};
	clin_capacity_ticket t = 99;

	flag(false);
	check(clin_capacity_reserve(1, &need, &t) == CLIN_CAP_DISABLED && t == 0
			  && held_is(0, 0, 0),
		  "R1 flag OFF: DISABLED, nothing held");

	flag(true);
	host_reset(0, 100); /* player 0 on slot 0 */
	check(clin_capacity_reserve(0, &need, &t) == CLIN_CAP_BAD_SLOT && t == 0,
		  "R3 player 0's own slot is refused");
	check(clin_capacity_reserve(1, &need, &t) == CLIN_CAP_OK && t != 0
			  && held_is(8u << 20, 4u << 20, 1),
		  "R2 the guest slot reserves within budget");
	clin_capacity_ticket t1 = t;
	check(clin_capacity_reserve(1, &need, &t) == CLIN_CAP_BUSY && t == 0
			  && held_is(8u << 20, 4u << 20, 1),
		  "R3 a second reserve on a held slot is BUSY, nothing changed");
	check(clin_capacity_commit(t1) && !clin_capacity_commit(t1),
		  "R2 commit once; a second commit changes nothing");
	check(!clin_capacity_release(t1 + 1000) && held_is(8u << 20, 4u << 20, 1),
		  "R2 an unknown ticket releases nothing");
	check(clin_capacity_release(t1) && held_is(0, 0, 0)
			  && !clin_capacity_release(t1),
		  "R2 release frees it; the ticket is dead afterwards");
	check(clin_capacity_reserve(1, &need, &t) == CLIN_CAP_OK && t != t1,
		  "R2 a new reservation gets a new ticket (never reused)");
	clin_capacity_ticket t2 = t;
	assert(clin_capacity_release(t2));

	/* MEM1: free 64 MB - 4 MB margin = 60 MB available. */
	struct clin_capacity_need big1 = {.mem1_bytes = 61u << 20};
	check(clin_capacity_reserve(1, &big1, &t) == CLIN_CAP_NO_MEM1 && t == 0
			  && held_is(0, 0, 0),
		  "R3 MEM1 short (margin counted): refused, nothing held");
	struct clin_capacity_need big2 = {.mem2_bytes = 31u << 20};
	check(clin_capacity_reserve(1, &big2, &t) == CLIN_CAP_NO_MEM2 && t == 0
			  && held_is(0, 0, 0),
		  "R3 MEM2 short (margin counted): refused, nothing held");
	struct clin_capacity_need edge = {.mem1_bytes = 60u << 20,
									  .mem2_bytes = 30u << 20};
	check(clin_capacity_reserve(1, &edge, &t) == CLIN_CAP_OK,
		  "R3 exactly the available bytes still fit");
	assert(clin_capacity_release(t));

	/* The context pool: fill it with leased, retiring contexts of slot 1. */
	struct client_world_context* pinned[CLIENT_CTX_POOL];
	unsigned n = 0;
	assert(client_ctx_registry_install(reg, WORLD_DIM_NETHER, 1, 500));
	while(client_ctx_registry_in_use(reg) < CLIENT_CTX_POOL) {
		struct client_world_context* c = client_ctx_registry_current(reg, 1);
		assert(client_ctx_lease_acquire(c, CLIENT_CTX_LEASE_MESHER));
		pinned[n++] = c;
		assert(client_ctx_registry_install(reg, WORLD_DIM_NETHER, 1, 500));
	}
	check(clin_capacity_reserve(1, &need, &t) == CLIN_CAP_NO_CONTEXT && t == 0
			  && held_is(0, 0, 0),
		  "R3 a full context pool refuses, nothing held");
	for(unsigned k = 0; k < n; k++)
		assert(client_ctx_registry_release(reg, pinned[k],
										   CLIENT_CTX_LEASE_MESHER));
	client_ctx_registry_retire(reg, 1);

	/* R5 (lane C, dwell_host_busy): two actors entering the SAME world back
	 * to back share one reservation instead of the second being BUSY. */
	check(clin_capacity_reserve_world(1, 300, &need, &t) == CLIN_CAP_OK
			  && held_is(8u << 20, 4u << 20, 1),
		  "R5 the first actor reserves world (slot 1, gen 300)");
	clin_capacity_ticket s1 = t;
	check(clin_capacity_reserve_world(1, 300, &need, &t) == CLIN_CAP_OK
			  && t != 0 && t != s1 && held_is(8u << 20, 4u << 20, 1),
		  "R5 a second reserve for the SAME world shares it: OK, own ticket, "
		  "no extra bytes or context");
	clin_capacity_ticket s2 = t;
	check(clin_capacity_reserve_world(1, 301, &need, &t) == CLIN_CAP_BUSY
			  && t == 0 && held_is(8u << 20, 4u << 20, 1),
		  "R5 another generation in the held slot stays BUSY");
	check(clin_capacity_reserve(1, &need, &t) == CLIN_CAP_BUSY && t == 0,
		  "R5 the legacy (generation-less) reserve never shares");
	s_mem1 = 8u << 20; /* now less is free than is held (plus margin) */
	check(clin_capacity_reserve_world(1, 300, &need, &t) == CLIN_CAP_NO_MEM1
			  && t == 0 && held_is(8u << 20, 4u << 20, 1),
		  "R5 a share still checks the budget: refused when what is held no "
		  "longer fits");
	s_mem1 = 64u << 20;
	check(clin_capacity_commit(s1) && clin_capacity_commit(s2),
		  "R5 each ticket commits on its own");
	check(clin_capacity_release(s1) && held_is(8u << 20, 4u << 20, 1),
		  "R5 releasing the owner hands the bytes to the remaining share");
	check(clin_capacity_release(s2) && held_is(0, 0, 0),
		  "R5 releasing the last ticket frees the world's reservation");
	check(clin_capacity_reserve_world(0, 100, &need, &t) == CLIN_CAP_OK
			  && held_is(0, 0, 0),
		  "R5 player 0's own world at its generation is a share (nothing new)");
	assert(clin_capacity_release(t));
	check(clin_capacity_reserve_world(0, 99, &need, &t) == CLIN_CAP_BAD_SLOT,
		  "R5 player 0's slot at another generation is still refused");

	assert(clin_capacity_reserve(1, &need, &t) == CLIN_CAP_OK);
	flag(true); /* a session flip */
	check(held_is(0, 0, 0) && !clin_capacity_release(t),
		  "R4 a flag flip drops every reservation");
	clin_capacity_set_budget_provider(NULL);
}

static void test_return_to_guest_world_capacity(void) {
	/* Wii split_dims: the guest still views OW slot 0 after the host moves to
	 * Nether slot 1. The return landing is in an already installed OW column.
	 * With 6432 KiB arena free, charging both a second OW ring and the held
	 * Nether ticket crosses the 4 MiB safety floor, although no new client
	 * world is needed for the host's return. The need is the frozen Wii
	 * 81-column shared-install bound (1,247,880 bytes). */
	flag(true);
	clin_capacity_set_budget_provider(budget);
	s_mem1 = 6432u << 10;
	s_mem2 = 32u << 20;
	const struct clin_capacity_need need = {.mem1_bytes = 1247880u,
		.mem2_bytes = 1u << 20};
	host_reset(0, 100);
	assert(clin_world_payload_bind_view(0, 1));
	for(w_coord_t dz = -4; dz <= 4; dz++)
		for(w_coord_t dx = -4; dx <= 4; dx++)
			column(dx, dz, 0, 100);
	clin_capacity_ticket nether = 0, returned = 0;
	assert(clin_capacity_reserve_world(1, 200, &need, &nether) == CLIN_CAP_OK);
	assert(clin_capacity_commit(nether));
	host_reset(1, 200);
	struct client_world_context* ctx = client_ctx_registry_current(reg, 0);
	struct clin_world_payload ow;
	assert(clin_world_payload_of_slot(0, &ow) && ow.terrain);
	for(w_coord_t dz = -4; dz <= 4; dz++)
		for(w_coord_t dx = -4; dx <= 4; dx++)
			assert(dict_wsection_get(ow.terrain->sections,
				SECTION_TO_ID(dx, dz)));
	assert(clin_world_payload_primary_slot() == 1 && ctx
		&& ctx->server_generation == 100 && (ctx->bound_views & 2u)
		&& clin_world_payload_ready(0) && held_is(need.mem1_bytes,
			need.mem2_bytes, 1));
	check(clin_capacity_reserve_world(0, 100, &need, &returned) == CLIN_CAP_OK
			&& returned && held_is(need.mem1_bytes, need.mem2_bytes, 1),
		"return to guest-bound resident OW shares capacity at captured Wii budget");
	if(returned) {
		assert(clin_capacity_commit(returned));
		host_reset(0, 100); /* COMMIT's host publication joins the guest context */
		check(clin_world_payload_primary_slot() == 0
				&& client_ctx_registry_current(reg, 0) == ctx
				&& ctx->bound_views == 3u
				&& held_is(need.mem1_bytes, need.mem2_bytes, 1),
			"host return binds the existing guest OW context, not a new world");
		host_reset(1, 200); /* leave the fixture in the nonprimary OW state */
		check(clin_world_payload_primary_slot() == 1
				&& client_ctx_registry_current(reg, 0) == ctx
				&& ctx->bound_views == 2u,
			"subsequent travel preserves the guest OW context");
		check(clin_capacity_release(returned)
				&& held_is(need.mem1_bytes, need.mem2_bytes, 1),
			"return share commits and releases without stealing Nether ownership");
	}
	returned = 0;
	check(clin_capacity_reserve_world(0, 99, &need, &returned)
			== CLIN_CAP_NO_MEM1 && returned == 0,
		"wrong generation cannot reuse the resident OW context");
	s_mem1 = CLIN_CAP_MEM1_MARGIN + need.mem1_bytes - 1u;
	check(clin_capacity_reserve_world(0, 100, &need, &returned)
			== CLIN_CAP_NO_MEM1 && returned == 0,
		"resident-world share still enforces held capacity and safety margin");
	s_mem1 = 6432u << 10;
	ctx->quiescing = true;
	check(clin_capacity_reserve_world(0, 100, &need, &returned)
			== CLIN_CAP_NO_MEM1 && returned == 0,
		"a quiescing context cannot justify zero-additional admission");
	ctx->quiescing = false;
	ctx->retiring = true;
	check(clin_capacity_reserve_world(0, 100, &need, &returned)
			== CLIN_CAP_NO_MEM1 && returned == 0,
		"a retiring context cannot justify zero-additional admission");
	ctx->retiring = false;
	assert(client_ctx_unbind_view(ctx, 1));
	check(clin_capacity_reserve_world(0, 100, &need, &returned)
			== CLIN_CAP_NO_MEM1 && returned == 0,
		"an unviewed context cannot justify zero-additional admission");
	assert(client_ctx_registry_install(reg, WORLD_DIM_OVERWORLD, 0, 101));
	check(clin_capacity_reserve_world(0, 101, &need, &returned)
			== CLIN_CAP_NO_MEM1 && returned == 0,
		"a newly installed unviewed context cannot masquerade as resident");
	assert(clin_capacity_release(nether));
	check(held_is(0, 0, 0), "last owner release leaves no capacity held");
	clin_capacity_set_budget_provider(NULL);
	flag(false);
}

static struct clin_landing landing(uint32_t token, unsigned slot, uint32_t gen,
								   w_coord_t x, w_coord_t z, int r) {
	return (struct clin_landing) {.token = token, .slot = slot,
								  .generation = gen, .x = x, .z = z,
								  .radius = r};
}

static void test_landing(void) {
	clin_landing_set_sender(sender);
	flag(false);
	struct clin_landing l = landing(7, 1, 300, 40, 40, 1);
	check(!clin_landing_arm(&l), "R1 flag OFF: a landing cannot be armed");

	flag(true);
	host_reset(0, 100);
	s_sends = s_refuse = 0;
	/* Landing at block (40,40) = column (2,2), radius 1 -> columns 1..3. */
	assert(clin_landing_arm(&l));
	check(clin_landing_poll() == CLIN_LANDING_WAITING && s_sends == 0,
		  "L1 armed before the world exists: WAITING");
	guest_move(1, 300); /* the world installs AND binds view 1 */
	check(clin_landing_poll() == CLIN_LANDING_WAITING && s_sends == 0,
		  "L1 world + view bound, no columns: WAITING");
	unsigned waited = 0;
	for(w_coord_t cx = 1; cx <= 3; cx++)
		for(w_coord_t cz = 1; cz <= 3; cz++) {
			if(cx == 3 && cz == 3)
				continue; /* the last column comes last */
			column(cx, cz, 1, 300);
			if(clin_landing_poll() == CLIN_LANDING_WAITING && s_sends == 0)
				waited++;
		}
	check(waited == 8, "L1 WAITING through all but the last near column");
	column(9, 9, 1, 300); /* an irrelevant far column */
	check(clin_landing_poll() == CLIN_LANDING_WAITING,
		  "L1 a column outside the radius does not count");
	column(3, 3, 1, 300);
	check(clin_landing_poll() == CLIN_LANDING_READY && s_sends == 1
			  && s_last_token == 7 && s_last_slot == 1 && s_last_gen == 300,
		  "L1 READY and reported on the frame the last column arrives");
	check(clin_landing_poll() == CLIN_LANDING_READY && s_sends == 1,
		  "L1 reported exactly once");

	/* L2: refused report retries. */
	s_sends = 0;
	s_refuse = 1;
	l = landing(8, 1, 300, 40, 40, 1);
	assert(clin_landing_arm(&l));
	check(clin_landing_poll() == CLIN_LANDING_REPORTING && s_sends == 1,
		  "L2 a refused report is retried, not lost");
	check(clin_landing_poll() == CLIN_LANDING_READY && s_sends == 2
			  && clin_landing_poll() == CLIN_LANDING_READY && s_sends == 2,
		  "L2 the retry reports once, never twice");

	/* L3: armed for a NEW generation while the slot still holds gen 300. */
	s_sends = 0;
	l = landing(9, 1, 301, 40, 40, 1);
	assert(clin_landing_arm(&l));
	check(clin_landing_poll() == CLIN_LANDING_WAITING,
		  "L3 armed while the previous world is still in the slot: WAITING");
	guest_move(1, 301);
	(void)clin_landing_poll(); /* seen; no columns yet */
	guest_move(1, 302);		   /* replaced before it was ready */
	check(clin_landing_poll() == CLIN_LANDING_STALE && s_sends == 0,
		  "L3 the world replaced after it was seen: STALE, nothing reported");
	column(2, 2, 1, 302);
	check(clin_landing_poll() == CLIN_LANDING_STALE && s_sends == 0,
		  "L3 a stale landing never reports later");

	/* L4: radius 0 and negative coordinates. */
	s_sends = 0;
	l = landing(10, 1, 302, -1, -17, 0); /* column (-1,-2) */
	assert(clin_landing_arm(&l));
	column(0, 0, 1, 302);
	check(clin_landing_poll() == CLIN_LANDING_WAITING,
		  "L4 the wrong column does not satisfy the landing");
	column(-1, -2, 1, 302);
	check(clin_landing_poll() == CLIN_LANDING_READY && s_sends == 1,
		  "L4 radius 0 + negative coordinates: the landing column alone");

	/* A flag flip cancels a pending landing. */
	l = landing(11, 1, 400, 0, 0, 0);
	assert(clin_landing_arm(&l));
	flag(true);
	check(clin_landing_state() == CLIN_LANDING_IDLE,
		  "R4 a flag flip cancels a pending landing");
	clin_landing_set_sender(NULL);
}

/* W: the wire consumers (CRPC_CAPACITY_REQUEST / CRPC_LANDING_ARM). */
static unsigned s_rep_n, s_rep_refuse;
static uint32_t s_rep_id;
static enum clin_capacity_result s_rep_res;
static clin_capacity_ticket s_rep_ticket;
static bool reply(uint32_t id, enum clin_capacity_result res,
				  clin_capacity_ticket t) {
	s_rep_n++;
	s_rep_id = id;
	s_rep_res = res;
	s_rep_ticket = t;
	if(s_rep_refuse) {
		s_rep_refuse--;
		return false;
	}
	return true;
}

static void cap_rpc(uint32_t id, uint8_t op, uint8_t slot, uint32_t m1,
					uint32_t m2, uint32_t ticket) {
	clin_process(&(struct client_rpc) {.type = CRPC_CAPACITY_REQUEST,
		.payload.capacity_request = {.request_id = id, .op = op, .slot = slot,
			.mem1_bytes = m1, .mem2_bytes = m2, .ticket = ticket}});
}

static void test_wire(void) {
	clin_capacity_set_budget_provider(budget);
	clin_capacity_set_reply_sender(reply);
	flag(true);
	host_reset(0, 100);
	s_rep_n = 0;
	cap_rpc(41, CLIN_CAP_OP_RESERVE, 1, 1u << 20, 1u << 20, 0);
	check(s_rep_n == 1 && s_rep_id == 41 && s_rep_res == CLIN_CAP_OK
			  && s_rep_ticket != 0 && held_is(1u << 20, 1u << 20, 1),
		  "W1 RESERVE over the wire: reply OK + ticket, capacity held");
	clin_capacity_ticket t = s_rep_ticket;
	cap_rpc(42, CLIN_CAP_OP_COMMIT, 0, 0, 0, t);
	check(s_rep_id == 42 && s_rep_res == CLIN_CAP_OK, "W1 COMMIT replies OK");
	cap_rpc(43, CLIN_CAP_OP_RELEASE, 0, 0, 0, t);
	check(s_rep_id == 43 && s_rep_res == CLIN_CAP_OK && held_is(0, 0, 0),
		  "W1 RELEASE replies OK and frees it");
	cap_rpc(44, CLIN_CAP_OP_RELEASE, 0, 0, 0, t);
	check(s_rep_id == 44 && s_rep_res == CLIN_CAP_BUSY,
		  "W1 a stale ticket answers BUSY");
	cap_rpc(45, CLIN_CAP_OP_RESERVE, 1, 200u << 20, 0, 0);
	check(s_rep_id == 45 && s_rep_res == CLIN_CAP_NO_MEM1 && s_rep_ticket == 0
			  && held_is(0, 0, 0),
		  "W2 a refusal is replied with its reason, nothing held");
	s_rep_refuse = 1;
	cap_rpc(46, CLIN_CAP_OP_RESERVE, 1, 1u << 20, 0, 0);
	check(s_rep_id == 46 && s_rep_res == CLIN_CAP_OK && held_is(0, 0, 0),
		  "W3 a reservation whose reply the transport refused is not "
		  "stranded");
	/* Respawn has one outstanding capacity request. A refused reply must be
	 * retained because the server will wait for this exact ID before Join or
	 * another respawn, even when the answer is NO_MEM. */
	s_rep_refuse = 1;
	cap_rpc(0x40000001u, CLIN_CAP_OP_RESERVE, 1, 1u << 20, 0, 0);
	clin_capacity_ticket respawn_ticket = s_rep_ticket;
	check(s_rep_id == 0x40000001u && s_rep_res == CLIN_CAP_OK
			  && respawn_ticket && held_is(1u << 20, 0, 1),
		  "W6 respawn reservation survives a refused reply");
	cap_rpc(0x40000001u, CLIN_CAP_OP_RESERVE, 1, 1u << 20, 0, 0);
	check(s_rep_ticket == respawn_ticket && held_is(1u << 20, 0, 1),
		  "W6 duplicate respawn ID does not reserve twice");
	(void)clin_landing_poll();
	check(s_rep_id == 0x40000001u && s_rep_res == CLIN_CAP_OK
			  && s_rep_ticket == respawn_ticket && held_is(1u << 20, 0, 1),
		  "W6 frame retry delivers the same successful reply and ticket");
	cap_rpc(47, CLIN_CAP_OP_RELEASE, 1, 0, 0, respawn_ticket);
	check(held_is(0, 0, 0), "W6 acknowledged ticket releases once");
	s_rep_refuse = 1;
	cap_rpc(0x40000002u, CLIN_CAP_OP_RESERVE, 1, 200u << 20, 0, 0);
	unsigned refusal_calls = s_rep_n;
	(void)clin_landing_poll();
	check(s_rep_n == refusal_calls + 1 && s_rep_id == 0x40000002u
			  && s_rep_res == CLIN_CAP_NO_MEM1 && held_is(0, 0, 0),
		  "W6 a refused capacity result is also retried");

	/* Landing over the wire, stamped with the landing world. */
	clin_landing_set_sender(sender);
	s_sends = 0;
	clin_process(&(struct client_rpc) {.type = CRPC_LANDING_ARM,
		.world_index = 1, .world_generation = 700,
		.payload.landing_arm = {.token = 5, .x = 0, .z = 0, .radius = 0}});
	check(clin_landing_state() == CLIN_LANDING_WAITING,
		  "W4 CRPC_LANDING_ARM arms the stamped world's landing");
	guest_move(1, 700);
	column(0, 0, 1, 700);
	clin_update_drain(); /* the frame poll reports it */
	check(clin_landing_state() == CLIN_LANDING_READY && s_sends == 1
			  && s_last_token == 5 && s_last_gen == 700,
		  "W4 the per-frame poll reports READY for the armed landing");
	clin_process(&(struct client_rpc) {.type = CRPC_LANDING_ARM,
		.world_index = 1, .world_generation = 701,
		.payload.landing_arm = {.token = 6}});
	clin_process(&(struct client_rpc) {.type = CRPC_LANDING_ARM,
		.world_index = 1, .world_generation = 701,
		.payload.landing_arm = {.token = 5, .cancel = true}});
	check(clin_landing_state() == CLIN_LANDING_WAITING,
		  "W5 an old token cannot cancel the newer landing");
	clin_process(&(struct client_rpc) {.type = CRPC_LANDING_ARM,
		.world_index = 1, .world_generation = 701,
		.payload.landing_arm = {.token = 6, .cancel = true}});
	check(clin_landing_state() == CLIN_LANDING_IDLE,
		  "W5 a cancel disarms the landing");
	clin_capacity_set_reply_sender(NULL);
	clin_capacity_set_budget_provider(NULL);
	clin_landing_set_sender(NULL);
}

static struct server_rpc s_srpc[8];
static unsigned s_srpc_n;
static void grab(struct server_rpc* rpc, void* user) {
	(void)user;
	if(s_srpc_n < 8)
		s_srpc[s_srpc_n++] = *rpc;
}
static void drain_srpc(void) {
	s_srpc_n = 0;
	svin_rpc_flush_pending();
	svin_process_messages(grab, NULL, false);
}
static void release(uint8_t index, uint32_t gen, bool retiring) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RELEASE,
		.world_index = index, .world_generation = gen,
		.payload.world_release = {.retiring = retiring}});
}

static void test_final_contract(void) {
	clin_capacity_set_budget_provider(budget);
	clin_capacity_set_reply_sender(NULL); /* the default svin sender */
	clin_landing_set_sender(NULL);
	flag(true);
	host_reset(0, 100);
	drain_srpc();
	cap_rpc(77, CLIN_CAP_OP_RESERVE, 1, 1u << 20, 1u << 20, 0);
	drain_srpc();
	check(s_srpc_n == 1 && s_srpc[0].type == SRPC_CAPACITY_REPLY
			  && s_srpc[0].payload.capacity_reply.request_id == 77
			  && s_srpc[0].payload.capacity_reply.result == CLIN_CAP_OK
			  && s_srpc[0].payload.capacity_reply.ticket != 0,
		  "F1 default reply: SRPC_CAPACITY_REPLY {request_id, result, ticket}");
	clin_capacity_ticket t = s_srpc_n ? s_srpc[0].payload.capacity_reply.ticket : 0;

	clin_process(&(struct client_rpc) {.type = CRPC_LANDING_ARM,
		.world_index = 1, .world_generation = 900,
		.payload.landing_arm = {.token = 9, .x = 0, .z = 0, .radius = 0}});
	guest_move(1, 900);
	column(0, 0, 1, 900);
	clin_update_drain();
	drain_srpc();
	check(clin_landing_state() == CLIN_LANDING_READY && s_srpc_n == 1
			  && s_srpc[0].type == SRPC_LANDING_READY
			  && s_srpc[0].payload.landing_ready.token == 9
			  && s_srpc[0].payload.landing_ready.slot == 1
			  && s_srpc[0].payload.landing_ready.generation == 900,
		  "F2 default landing report: SRPC_LANDING_READY {token, slot, "
		  "generation}");

	/* A drained LIVE world: slot 1 shown to nobody, a landing armed into it. */
	host_reset(1, 910);
	host_reset(0, 100); /* the host leaves slot 1 alone: unviewed */
	clin_process(&(struct client_rpc) {.type = CRPC_LANDING_ARM,
		.world_index = 1, .world_generation = 910,
		.payload.landing_arm = {.token = 10, .x = 0, .z = 0, .radius = 0}});
	release(1, 909, false);
	check(client_ctx_registry_current(reg, 1)
			  && clin_landing_state() == CLIN_LANDING_WAITING,
		  "F3 a release for another generation is ignored");
	release(1, 910, false);
	check(!client_ctx_registry_current(reg, 1)
			  && clin_landing_state() == CLIN_LANDING_STALE
			  && held_is(1u << 20, 1u << 20, 1),
		  "F3 drained release: slot freed, landing stale, reservation kept");
	host_reset(1, 920);
	host_reset(0, 100);
	release(1, 920, true);
	check(!client_ctx_registry_current(reg, 1) && held_is(0, 0, 0)
			  && !clin_capacity_commit(t),
		  "F4 retiring release: slot freed and its reservation returned");
	clin_capacity_set_budget_provider(NULL);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	assert(svin_init());
	assert(clin_init());
	memset(&gstate, 0, sizeof gstate);
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	gstate.players[0].visible_window = -1;
	reg = clin_world_contexts();
	test_capacity();
	test_return_to_guest_world_capacity();
	test_landing();
	test_wire();
	test_final_contract();
	flag(false);
	printf("%s test_b2_a2t_admission (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
