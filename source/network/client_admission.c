/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * Split-dims B2 -> A2-T admission, client half. Contract: client_admission.h.
 * Built only on the public payload/registry API (client_world_payload.h,
 * client_world_context.h); nothing here touches client_interface.c's
 * private state. */

#include "client_admission.h"

#include <stdint.h>
#include <string.h>

#include "../game/client_world_context.h"
#include "../platform/mem2.h"
#include "client_interface.h"
#include "client_world_payload.h"
#include "server_interface.h"

#ifdef PLATFORM_WII
#include <ogc/system.h>
#endif

/* ---- (1) capacity ---- */

enum cap_state { CAP_FREE = 0, CAP_RESERVED, CAP_COMMITTED };

/* One entry per TICKET. The first ticket of a world (slot + generation)
 * holds its bytes and its context-pool entry; a later ticket for the SAME
 * world -- a second actor entering the world the first is entering or has
 * entered (lane C's host-then-guest into the Nether) -- SHARES it: the
 * world's payload is consumed once however many views show it, so a shared
 * ticket holds no bytes of its own. Releasing the owner while shares remain
 * hands the bytes to one of them. */
struct cap_entry {
	enum cap_state state;
	clin_capacity_ticket ticket;
	unsigned slot;
	uint32_t generation; /* 0 = unknown (legacy reserve: never shared) */
	bool shared;
	struct clin_capacity_need need;
};

#define CAP_ENTRIES (CLIENT_CTX_SLOT_COUNT * 2u)
static struct cap_entry s_cap[CAP_ENTRIES];
static clin_capacity_ticket s_cap_next_ticket = 1;
static clin_capacity_budget_fn s_cap_budget;
/* The server admits only one guest respawn reserve at a time and will not
 * admit another guest while its answer is unresolved. Retain that answer if
 * the server inbox is full; releasing an unknown ticket and saying nothing
 * would make its timed-out request wait for a reply that cannot arrive. */
static struct {
	bool pending;
	uint32_t request_id;
	enum clin_capacity_result result;
	clin_capacity_ticket ticket;
} s_respawn_reply;


static void cap_default_budget(struct clin_capacity_budget* out) {
#ifdef PLATFORM_WII
	out->mem1_free = SYS_GetArena1Size();
	size_t carve = mem2_carve_free_bytes();
	/* SIZE_MAX = no carve to measure: do not gate on it (mem2.h). */
	out->mem2_free = carve == SIZE_MAX ? UINT64_MAX : (uint64_t)carve;
#else
	out->mem1_free = UINT64_MAX; /* PC: no fixed heaps to gate on */
	out->mem2_free = UINT64_MAX;
#endif
}

void clin_capacity_set_budget_provider(clin_capacity_budget_fn fn) {
	s_cap_budget = fn;
}

static bool cap_fits(uint64_t free_bytes, uint64_t held, uint64_t margin,
					 uint64_t need) {
	if(free_bytes == UINT64_MAX)
		return true;
	return free_bytes >= margin && free_bytes - margin >= held
		&& free_bytes - margin - held >= need;
}

static struct cap_entry* cap_free_entry(void) {
	for(unsigned i = 0; i < CAP_ENTRIES; i++)
		if(s_cap[i].state == CAP_FREE)
			return &s_cap[i];
	return NULL;
}

static clin_capacity_ticket cap_new_ticket(void) {
	clin_capacity_ticket t = s_cap_next_ticket++;
	if(t == 0) /* never reuse 0 (= none) */
		t = s_cap_next_ticket++;
	return t;
}

/* A ticket that shares what the slot's world already holds (or, for player
 * 0's own world, what it already is). The budget is still checked: what is
 * held must still fit what is free now (a shared ticket adds no bytes). */
static enum clin_capacity_result cap_share(unsigned slot, uint32_t generation,
										   clin_capacity_ticket* out) {
	struct cap_entry* e = cap_free_entry();
	if(!e)
		return CLIN_CAP_BUSY;
	uint64_t held1 = 0, held2 = 0;
	clin_capacity_held(&held1, &held2, NULL);
	struct clin_capacity_budget b;
	(s_cap_budget ? s_cap_budget : cap_default_budget)(&b);
	if(!cap_fits(b.mem1_free, held1, CLIN_CAP_MEM1_MARGIN, 0))
		return CLIN_CAP_NO_MEM1;
	if(!cap_fits(b.mem2_free, held2, CLIN_CAP_MEM2_MARGIN, 0))
		return CLIN_CAP_NO_MEM2;
	*e = (struct cap_entry) {.state = CAP_RESERVED, .ticket = cap_new_ticket(),
							 .slot = slot, .generation = generation,
							 .shared = true};
	if(out)
		*out = e->ticket;
	return CLIN_CAP_OK;
}

enum clin_capacity_result clin_capacity_reserve(unsigned slot,
	const struct clin_capacity_need* need, clin_capacity_ticket* out) {
	return clin_capacity_reserve_world(slot, 0, need, out);
}

enum clin_capacity_result clin_capacity_reserve_world(unsigned slot,
	uint32_t generation, const struct clin_capacity_need* need,
	clin_capacity_ticket* out) {
	if(out)
		*out = 0;
	if(!clin_world_payloads_enabled())
		return CLIN_CAP_DISABLED;
	if(slot >= CLIENT_CTX_SLOT_COUNT || !need)
		return CLIN_CAP_BAD_SLOT;
	if(slot == clin_world_payload_primary_slot()) {
		/* Player 0's own world at exactly this generation: the second actor
		 * joins a world the client already holds -- nothing new to reserve.
		 * Anything else naming player 0's slot is a bad request. */
		const struct client_world_context* ctx
			= client_ctx_registry_current(clin_world_contexts(), slot);
		if(generation != 0 && ctx && ctx->server_generation == generation)
			return cap_share(slot, generation, out);
		return CLIN_CAP_BAD_SLOT;
	}
	bool held_here = false, same_world = generation != 0;
	for(unsigned i = 0; i < CAP_ENTRIES; i++)
		if(s_cap[i].state != CAP_FREE && s_cap[i].slot == slot) {
			held_here = true;
			same_world = same_world && s_cap[i].generation == generation;
		}
	if(held_here)
		return same_world ? cap_share(slot, generation, out) : CLIN_CAP_BUSY;
	/* The host can leave its original world to the guest, making another slot
	 * primary without releasing that world's payload. A return to the exact
	 * guest-bound, ready context shares it just like a second ticket for the
	 * same world above; an unviewed prepared landing is not resident proof. */
	const struct client_world_context* ctx
		= client_ctx_registry_current(clin_world_contexts(), slot);
	if(generation != 0 && ctx && ctx->server_generation == generation
	   && ctx->bound_views && client_ctx_payload_ready(ctx)
	   && clin_world_payload_ready(slot))
		return cap_share(slot, generation, out);
	uint64_t held1 = 0, held2 = 0;
	unsigned held_ctx = 0;
	clin_capacity_held(&held1, &held2, &held_ctx);
	/* One pool entry per reserved world, on top of every entry in use
	   (current AND retiring-with-leases: both occupy the pool). */
	unsigned in_use = client_ctx_registry_in_use(clin_world_contexts());
	if(in_use + held_ctx + 1u > CLIENT_CTX_POOL)
		return CLIN_CAP_NO_CONTEXT;
	struct clin_capacity_budget b;
	(s_cap_budget ? s_cap_budget : cap_default_budget)(&b);
	if(!cap_fits(b.mem1_free, held1, CLIN_CAP_MEM1_MARGIN, need->mem1_bytes))
		return CLIN_CAP_NO_MEM1;
	if(!cap_fits(b.mem2_free, held2, CLIN_CAP_MEM2_MARGIN, need->mem2_bytes))
		return CLIN_CAP_NO_MEM2;
	struct cap_entry* e = cap_free_entry();
	if(!e)
		return CLIN_CAP_BUSY;
	*e = (struct cap_entry) {.state = CAP_RESERVED, .ticket = cap_new_ticket(),
							 .slot = slot, .generation = generation,
							 .need = *need};
	if(out)
		*out = e->ticket;
	return CLIN_CAP_OK;
}

static struct cap_entry* cap_find(clin_capacity_ticket ticket) {
	if(!ticket)
		return NULL;
	for(unsigned i = 0; i < CAP_ENTRIES; i++)
		if(s_cap[i].state != CAP_FREE && s_cap[i].ticket == ticket)
			return &s_cap[i];
	return NULL;
}

bool clin_capacity_commit(clin_capacity_ticket ticket) {
	struct cap_entry* e = cap_find(ticket);
	if(!e || e->state != CAP_RESERVED)
		return false;
	e->state = CAP_COMMITTED;
	return true;
}

bool clin_capacity_release(clin_capacity_ticket ticket) {
	struct cap_entry* e = cap_find(ticket);
	if(!e)
		return false;
	if(!e->shared) /* hand the world's bytes to a remaining share */
		for(unsigned i = 0; i < CAP_ENTRIES; i++)
			if(&s_cap[i] != e && s_cap[i].state != CAP_FREE && s_cap[i].shared
			   && s_cap[i].slot == e->slot
			   && s_cap[i].generation == e->generation) {
				s_cap[i].shared = false;
				s_cap[i].need = e->need;
				break;
			}
	memset(e, 0, sizeof(*e));
	return true;
}

void clin_capacity_held(uint64_t* mem1, uint64_t* mem2, unsigned* contexts) {
	uint64_t a = 0, b = 0;
	unsigned c = 0;
	for(unsigned i = 0; i < CAP_ENTRIES; i++)
		if(s_cap[i].state != CAP_FREE && !s_cap[i].shared) {
			a += s_cap[i].need.mem1_bytes;
			b += s_cap[i].need.mem2_bytes;
			c++;
		}
	if(mem1)
		*mem1 = a;
	if(mem2)
		*mem2 = b;
	if(contexts)
		*contexts = c;
}

void clin_capacity_reset(void) {
	memset(s_cap, 0, sizeof(s_cap));
	memset(&s_respawn_reply, 0, sizeof(s_respawn_reply));
}

/* ---- wire consumers ---- */

/* The default senders: the final A2-T wire (lane A reply 2026-09-27). */
static bool cap_reply_via_svin(uint32_t request_id,
							   enum clin_capacity_result result,
							   clin_capacity_ticket ticket) {
	struct server_rpc rpc = {.type = SRPC_CAPACITY_REPLY,
		.payload.capacity_reply = {.request_id = request_id,
			.result = (uint8_t)result, .ticket = ticket}};
	return svin_rpc_send(&rpc);
}

static bool landing_ready_via_svin(uint32_t token, unsigned slot,
								   uint32_t generation) {
	struct server_rpc rpc = {.type = SRPC_LANDING_READY,
		.payload.landing_ready = {.token = token, .slot = (uint8_t)slot,
			.generation = generation}};
	return svin_rpc_send(&rpc);
}

bool clin_landing_ready_send_default(uint32_t token, unsigned slot,
									 uint32_t generation) {
	return landing_ready_via_svin(token, slot, generation);
}

static clin_capacity_reply_sender s_cap_reply = cap_reply_via_svin;

static void respawn_reply_poll(void) {
	if(s_respawn_reply.pending && s_cap_reply
	   && s_cap_reply(s_respawn_reply.request_id,
		s_respawn_reply.result, s_respawn_reply.ticket))
		s_respawn_reply.pending = false;
}

void clin_capacity_set_reply_sender(clin_capacity_reply_sender sender) {
	s_cap_reply = sender ? sender : cap_reply_via_svin;
}

void clin_admission_handle_capacity(const struct client_rpc* call) {
	const uint32_t id = call->payload.capacity_request.request_id;
	bool respawn_reserve = call->payload.capacity_request.op == CLIN_CAP_OP_RESERVE
		&& (id & 0xc0000000u) == CLIN_RESPAWN_CAP_ID_BIT;
	if(respawn_reserve && s_respawn_reply.pending) {
		/* Same ID is a retry, not a new ticket. A different ID cannot replace
		 * the sole outstanding answer. The server serializes these requests. */
		return;
	}
	enum clin_capacity_result res = CLIN_CAP_BUSY;
	clin_capacity_ticket ticket = call->payload.capacity_request.ticket;
	switch(call->payload.capacity_request.op) {
		case CLIN_CAP_OP_RESERVE: {
			struct clin_capacity_need need = {
				.mem1_bytes = call->payload.capacity_request.mem1_bytes,
				.mem2_bytes = call->payload.capacity_request.mem2_bytes};
			/* the request is stamped with the world it reserves for: a
			 * second actor entering that same world shares (not BUSY) */
			res = clin_capacity_reserve_world(
				call->payload.capacity_request.slot, call->world_generation,
				&need, &ticket);
			break;
		}
		case CLIN_CAP_OP_COMMIT:
			res = clin_capacity_commit(ticket) ? CLIN_CAP_OK : CLIN_CAP_BUSY;
			break;
		case CLIN_CAP_OP_RELEASE:
			res = clin_capacity_release(ticket) ? CLIN_CAP_OK : CLIN_CAP_BUSY;
			break;
		default: res = CLIN_CAP_BAD_SLOT; break;
	}
	if(s_cap_reply && !s_cap_reply(id, res, ticket)
	   && call->payload.capacity_request.op == CLIN_CAP_OP_RESERVE) {
		if(respawn_reserve) {
			s_respawn_reply.pending = true;
			s_respawn_reply.request_id = id;
			s_respawn_reply.result = res;
			s_respawn_reply.ticket = ticket;
		} else if(res == CLIN_CAP_OK)
			/* Other admission paths retain their existing timeout/re-ask rule. */
			(void)clin_capacity_release(ticket);
	}
}

static struct clin_landing s_landing;
static enum clin_landing_state s_landing_state = CLIN_LANDING_IDLE;
static bool s_landing_seen;
static clin_landing_ready_sender s_landing_sender = landing_ready_via_svin;

void clin_admission_handle_landing(const struct client_rpc* call) {
	if(call->payload.landing_arm.cancel) {
		/* blocker 3: the transaction did not commit; a context this client
		 * prepared for it (unviewed) goes with it. A delayed cancellation
		 * cannot retire a newer landing in the same slot. */
		if(s_landing_state == CLIN_LANDING_IDLE
		   || s_landing.token != call->payload.landing_arm.token
		   || s_landing.slot != call->world_index
		   || s_landing.generation != call->world_generation)
			return;
		clin_landing_cancel();
		clin_world_payload_unprepare_landing(call->world_index,
											 call->world_generation);
		return;
	}
	struct clin_landing l = {.token = call->payload.landing_arm.token,
							 .slot = call->world_index,
							 .generation = call->world_generation,
							 .x = call->payload.landing_arm.x,
							 .z = call->payload.landing_arm.z,
							 .radius = call->payload.landing_arm.radius};
	/* blocker 3: the arrival columns the server streams next (stamped with
	 * this world) need a context in that slot before any view shows it. The
	 * arm precedes them in the same FIFO. A refused prepare still arms: the
	 * landing then waits for the world to appear, or the server times out. */
	if(clin_world_payloads_enabled())
		(void)clin_world_payload_prepare_landing(
			l.slot, l.generation, (int)call->payload.landing_arm.dimension);
	(void)clin_landing_arm(&l);
}

/* ---- (2) landing ready ---- */

#define LANDING_RADIUS_MAX 8

void clin_landing_set_sender(clin_landing_ready_sender sender) {
	s_landing_sender = sender ? sender : landing_ready_via_svin;
}

void clin_admission_world_released(unsigned slot, uint32_t generation,
								   bool retiring) {
	if((s_landing_state == CLIN_LANDING_WAITING
		|| s_landing_state == CLIN_LANDING_REPORTING)
	   && s_landing.slot == slot && s_landing.generation == generation)
		s_landing_state = CLIN_LANDING_STALE;
	if(retiring && slot < CLIENT_CTX_SLOT_COUNT)
		for(unsigned i = 0; i < CAP_ENTRIES; i++)
			if(s_cap[i].state != CAP_FREE && s_cap[i].slot == slot)
				memset(&s_cap[i], 0, sizeof(s_cap[i]));
}

bool clin_landing_arm(const struct clin_landing* landing) {
	if(!landing || !clin_world_payloads_enabled()
	   || landing->slot >= CLIENT_CTX_SLOT_COUNT || landing->generation == 0
	   || landing->radius < 0 || landing->radius > LANDING_RADIUS_MAX)
		return false;
	s_landing = *landing;
	s_landing_seen = false;
	s_landing_state = CLIN_LANDING_WAITING;
	return true;
}

void clin_landing_cancel(void) {
	s_landing_state = CLIN_LANDING_IDLE;
}

enum clin_landing_state clin_landing_state(void) {
	return s_landing_state;
}

static w_coord_t floor_div16(w_coord_t v) {
	return v >= 0 ? v / 16 : -((-v + 15) / 16);
}

/* Every column within the radius has at least one installed chunk. Columns
   stream whole (CRPC_CHUNK sy = WORLD_HEIGHT), so any installed section
   proves the column arrived. */
static bool landing_columns_present(struct world* terrain) {
	w_coord_t cx = floor_div16(s_landing.x), cz = floor_div16(s_landing.z);
	for(w_coord_t dx = -s_landing.radius; dx <= s_landing.radius; dx++)
		for(w_coord_t dz = -s_landing.radius; dz <= s_landing.radius; dz++) {
			bool any = false;
			for(w_coord_t y = 0; y < WORLD_HEIGHT && !any; y += CHUNK_SIZE)
				any = world_find_chunk(terrain, (cx + dx) * CHUNK_SIZE, y,
									   (cz + dz) * CHUNK_SIZE)
					!= NULL;
			if(!any)
				return false;
		}
	return true;
}

enum clin_landing_state clin_landing_poll(void) {
	respawn_reply_poll();
	if(s_landing_state != CLIN_LANDING_WAITING
	   && s_landing_state != CLIN_LANDING_REPORTING)
		return s_landing_state;
	if(!clin_world_payloads_enabled()) {
		s_landing_state = CLIN_LANDING_STALE;
		return s_landing_state;
	}
	const struct client_world_context* ctx
		= client_ctx_registry_current(clin_world_contexts(), s_landing.slot);
	if(!ctx || ctx->server_generation != s_landing.generation) {
		/* Before the world is installed the slot may hold nothing or the
		   previous world: keep waiting. Once this generation HAS been seen,
		   losing it means the landing can never complete (no ordering of
		   generations is assumed). */
		if(s_landing_seen)
			s_landing_state = CLIN_LANDING_STALE;
		return s_landing_state;
	}
	s_landing_seen = true;
	/* blocker 3: no view may show the destination before COMMIT (the
	 * transferring player is still in its source world), so readiness is the
	 * slot's payload and its arrival columns only; the view binds at COMMIT. */
	if(!clin_world_payload_ready(s_landing.slot))
		return s_landing_state;
	struct clin_world_payload p;
	if(!clin_world_payload_of_slot(s_landing.slot, &p) || !p.terrain
	   || !landing_columns_present(p.terrain))
		return s_landing_state;
	if(s_landing_sender
	   && s_landing_sender(s_landing.token, s_landing.slot,
						   s_landing.generation))
		s_landing_state = CLIN_LANDING_READY;
	else
		s_landing_state = CLIN_LANDING_REPORTING; /* retried next frame */
	return s_landing_state;
}
