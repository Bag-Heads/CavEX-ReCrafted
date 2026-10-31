/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* host_vehicle_smoke_8m observation probe. See vehicle_smoke_probe.h for the
 * design note and tmp/jobs/vehicle-smoke-1004/CONTRACT-v1.md for the binding
 * wire contract. */

#include "vehicle_smoke_probe.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../entity/actor_ident.h"
#include "../entity/entity.h"
#include "../game/client_session.h"
#include "../game/client_world_context.h"
#include "../game/game_state.h"
#include "../item/inventory.h"
#include "../item/window_container.h"
#include "../platform/time.h"
#include "../world.h"
#include "client_interface.h"
#include "client_world_payload.h"
#include "server_interface.h"
#include "server_local.h"
#include "server_local_state.h"
#include "server_transfer.h"
#include "server_world.h"

_Static_assert(sizeof(struct vsp_record) <= 1024,
			   "CONTRACT-v1: each queued record is <= 1024 bytes");

bool vsp_g_armed;
/* server_interface.c's ingress observer slot (see there). */
extern void (*svin_vsp_ingress_observer)(const struct server_rpc* call,
	uint8_t slot, uint64_t generation, bool admitted);

static struct {
	enum vsp_kind kind;
	bool fixture_request;
	char build_id[64];
	ptime_t t0;
} s_cfg;

/* Cross-thread scalars: every access is an __atomic op. */
static uint32_t s_phase, s_op;
static uint32_t s_server_update, s_active_update, s_overflow, s_hook_mask;
/* O18: 32-bit storage -- Broadway (32-bit PPC) has no 8-byte atomic RMW
 * (__atomic_fetch_add_8 is undefined at the Wii link). Still a G64 on the
 * wire; a whole run allocates < 1e4 serials, and an impending wrap fails BY
 * NAME (CONTRACT: "impending wrap FAILS") instead of wrapping. */
static uint32_t s_serial;
/* O04: one push-order counter across both FIFOs (see vsp_pop). */
static uint32_t s_push_order;
static uint32_t s_cmd[VSP_CMD_COUNT];
static uint32_t s_cmd_refusal;
static uint32_t s_req_ride_id, s_req_ride_target, s_req_ride_open;
static uint32_t s_ack_ride_closed;
static uint32_t s_req_iso_op, s_req_iso_open, s_ack_iso_closed;
static uint32_t s_req_iso_target;
static uint32_t s_host_rest_since; /* server update the host came to rest; 0 = moving */
/* O14: guest rest (same convention) and the remote-home enforcement. */
static uint32_t s_guest_rest_since;
static uint32_t s_far_home_req;
static uint32_t s_far_home_min_mm; /* 0 = never sampled; else mm + 1 */
/* O14 diagnostic: nearest item to the enforced guest (horizontal Euclid and
 * per-axis Chebyshev, mm + 1; 0 = no item seen) and the items seen. */
static uint32_t s_far_item_min_mm, s_far_item_cheb_mm, s_far_item_seen;
static uint32_t s_expect_reload;		/* checkpoint id, 0 = none */
static uint32_t s_fixture_published;	/* rig targets readable on main */
static int32_t s_fixture_anchor[3];
static struct vsp_r s_fixture_targets[3];
static uint32_t s_fixture_hash;

/* SPSC server -> main FIFO. tail is the producer's, head the consumer's. */
static struct vsp_record s_srv_q[VSP_SRV_FIFO];
static uint32_t s_srv_head, s_srv_tail;
/* main-thread FIFO (single thread). */
static struct vsp_record s_main_q[VSP_MAIN_FIFO];
static uint32_t s_main_head, s_main_tail;

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

static uint32_t vsp_live_fixture_hash(struct server_local* s);

/* ---- server-thread-only state ------------------------------------------- */
static struct {
	/* ride window */
	uint32_t ride_id, ride_target;
	bool ride_open;
	uint32_t ride_eligible, ride_ticks, ride_missing, ride_duplicate;
	bool ride_detached;
	uint32_t ride_ticks_this_update;
	float ride_start[3], ride_disp_max;
	/* isolation bracket */
	uint32_t iso_op, iso_target;
	bool iso_open;
	uint32_t iso_follow, iso_attach;
	/* the authority's last APPLIED cart ride request (id + request_seq) */
	uint32_t cart_req_target, cart_req_seq;
	uint64_t iso_pose_gen; /* guest generation the pose baseline belongs to */
	float iso_pose_max, iso_pose0[3];
	uint32_t guest_rel_prev;
	/* fixture pig AI */
	uint32_t pig_ai_updates;
	/* refusal / progress */
	bool refusal_wanted, refusal_armed, refusal_cleared, barrier_released;
	uint32_t refusal_op;
	struct vsp_q q;
	bool q_valid;
	uint32_t clear_transfer_id;
	bool retained_pause_sent, retained_transfer_sent, blocked_sent;
	uint32_t installed_count, ready_count;
	/* checkpoint */
	bool ck_stage_ok[3];
	uint32_t checkpoint_id;
	/* respawn / transfer acknowledgements */
	bool respawn_ack_pending;
	uint32_t respawn_old_epoch;
	uint64_t respawn_release_serial;
	bool transfer_ack_pending;
	uint32_t transfer_ack_floor; /* host input epoch at the commit */
	uint32_t transfer_id, transfer_src_slot, transfer_dst_slot;
	/* reload */
	bool reload_disk_done, reload_inv_done, reload_entities_done;
	/* lethal */
	bool lethal_active;
	/* Q9: the open host use bracket (vsp_srv_use_pre..post) and the release
	 * witnesses the real shared release emitted inside it. */
	bool use_open;
	const struct server_player* use_actor;
	uint32_t use_release_count;
	uint64_t use_release_serial;
	/* O14: the guest's last authoritative pose (rest tracking) */
	double guest_last[3];
	uint64_t guest_last_gen;
	bool guest_last_valid;
} S;

/* ---- main-thread-only client counters ----------------------------------- */
static struct vsp_client_counters C;
static int s_client_ride_slot = -1; /* detached census: the target's store */
static uint32_t s_client_ride_target;
static struct vsp_dispatch_tap* s_tap; /* Q3: the one open dispatch tap */

/* ---- arming ------------------------------------------------------------- */
static void vsp_reset_failure(void);
static void vsp_await_reset(void); /* O11 */
static void vsp_rec_init(struct vsp_record* rec, enum vsp_event ev,
						 enum vsp_origin origin);
void vsp_reset(void) {
	vsp_await_reset();
	__atomic_store_n(&svin_vsp_ingress_observer, NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&vsp_g_armed, false, __ATOMIC_RELEASE);
	memset(&s_cfg, 0, sizeof(s_cfg));
	s_phase = s_op = 0;
	s_server_update = s_active_update = s_overflow = s_hook_mask = 0;
	s_serial = 0;
	__atomic_store_n(&s_push_order, 0u, __ATOMIC_RELEASE);
	memset(s_cmd, 0, sizeof(s_cmd));
	s_cmd_refusal = 0;
	s_req_ride_id = s_req_ride_target = s_req_ride_open = 0;
	s_ack_ride_closed = 0;
	s_req_iso_op = s_req_iso_open = s_ack_iso_closed = 0;
	s_host_rest_since = 0;
	s_guest_rest_since = 0;
	s_far_home_req = 0;
	s_far_home_min_mm = 0;
	s_far_item_min_mm = s_far_item_cheb_mm = s_far_item_seen = 0;
	s_req_iso_target = 0;
	s_expect_reload = 0;
	s_fixture_published = 0;
	memset(s_fixture_anchor, 0, sizeof(s_fixture_anchor));
	memset(s_fixture_targets, 0, sizeof(s_fixture_targets));
	s_fixture_hash = 0;
	s_srv_head = s_srv_tail = 0;
	s_main_head = s_main_tail = 0;
	memset(&S, 0, sizeof(S));
	memset(&C, 0, sizeof(C));
	s_client_ride_target = 0;
	s_client_ride_slot = -1;
	s_tap = NULL;
	vsp_reset_failure();
}

void vsp_arm(const struct vsp_arm_cfg* cfg) {
	vsp_reset();
	if(!cfg || cfg->kind >= VSP_KIND_COUNT)
		return;
	s_cfg.kind = cfg->kind;
	s_cfg.fixture_request = cfg->transfer_fixture_request;
	snprintf(s_cfg.build_id, sizeof(s_cfg.build_id), "%s",
			 cfg->build_id ? cfg->build_id : "unstamped");
	s_cfg.t0 = time_get();
	__atomic_store_n(&svin_vsp_ingress_observer, vsp_ingress_refused,
					 __ATOMIC_RELEASE);
	__atomic_store_n(&vsp_g_armed, true, __ATOMIC_RELEASE);
}

void vsp_disarm(void) {
	if(LOAD(s_far_home_req))
		vsp_far_home_report();
	vsp_hooks_reset();
	__atomic_store_n(&svin_vsp_ingress_observer, NULL, __ATOMIC_RELEASE);
	__atomic_store_n(&vsp_g_armed, false, __ATOMIC_RELEASE);
}

enum vsp_kind vsp_kind_now(void) {
	return s_cfg.kind;
}

const char* vsp_kind_name(enum vsp_kind kind) {
	switch(kind) {
		case VSP_BOAT: return "boat";
		case VSP_PIG: return "pig";
		case VSP_CART: return "cart";
		default: return "";
	}
}

uint32_t vsp_now_ms(void) {
	int32_t ms = time_diff_ms(s_cfg.t0, time_get());
	return ms > 0 ? (uint32_t)ms : 0u;
}

void vsp_set_phase(enum vsp_phase phase) {
	STORE(s_phase, (uint32_t)phase);
}
enum vsp_phase vsp_phase_now(void) {
	return (enum vsp_phase)LOAD(s_phase);
}
void vsp_set_op(uint32_t op_id) {
	STORE(s_op, op_id);
}
static uint32_t s_op_action_op, s_op_action;
void vsp_set_op_action(uint32_t op_id, uint8_t action) {
	STORE(s_op_action, (uint32_t)action);
	STORE(s_op_action_op, op_id);
}
/* The hinted action for the current op, else `fallback`. */
static uint8_t vsp_op_action_or(uint8_t fallback) {
	uint32_t op = LOAD(s_op);
	return op && LOAD(s_op_action_op) == op ? (uint8_t)LOAD(s_op_action)
											: fallback;
}
uint32_t vsp_op_now(void) {
	return LOAD(s_op);
}
uint32_t vsp_server_update(void) {
	return LOAD(s_server_update);
}
uint32_t vsp_active_update(void) {
	return LOAD(s_active_update);
}
uint32_t vsp_overflow_count(void) {
	return LOAD(s_overflow);
}
uint32_t vsp_hook_mask(void) {
	return LOAD(s_hook_mask);
}

static void vsp_hook_set(uint32_t bit, bool on) {
	if(on)
		__atomic_fetch_or(&s_hook_mask, bit, __ATOMIC_ACQ_REL);
	else
		__atomic_fetch_and(&s_hook_mask, ~bit, __ATOMIC_ACQ_REL);
}

void vsp_hooks_reset(void) {
	for(unsigned k = 0; k < VSP_CMD_COUNT; k++)
		STORE(s_cmd[k], 0u);
	STORE(s_cmd_refusal, 0u);
	/* The server-thread refusal latch is retired by the next server update
	 * (vsp_srv_update_begin sees the cleared mask); clearing the mask here
	 * makes the SG predicate inert from this instant. */
	STORE(s_hook_mask, 0u);
}

/* ---- FIFOs -------------------------------------------------------------- */
static void vsp_stamp(struct vsp_record* rec) {
	rec->push_order = __atomic_add_fetch(&s_push_order, 1u, __ATOMIC_ACQ_REL);
	if(!rec->ms)
		rec->ms = vsp_now_ms();
	rec->phase = (uint8_t)LOAD(s_phase);
	rec->server_update = LOAD(s_server_update);
	rec->active_update = LOAD(s_active_update);
}

static bool vsp_srv_push(struct vsp_record* rec) {
	vsp_stamp(rec);
	uint32_t tail = __atomic_load_n(&s_srv_tail, __ATOMIC_RELAXED);
	uint32_t head = LOAD(s_srv_head);
	if(tail - head >= VSP_SRV_FIFO) {
		__atomic_fetch_add(&s_overflow, 1u, __ATOMIC_ACQ_REL);
		return false;
	}
	s_srv_q[tail % VSP_SRV_FIFO] = *rec;
	STORE(s_srv_tail, tail + 1u);
	return true;
}

bool vsp_main_push(struct vsp_record* rec) {
	if(!rec)
		return false;
	vsp_stamp(rec);
	if(s_main_tail - s_main_head >= VSP_MAIN_FIFO) {
		__atomic_fetch_add(&s_overflow, 1u, __ATOMIC_ACQ_REL);
		return false;
	}
	s_main_q[s_main_tail % VSP_MAIN_FIFO] = *rec;
	s_main_tail++;
	return true;
}

/* ---- O11 / S03 #2: accepted-send receipts (main thread) ----------------- */
#define VSP_AWAIT_MAX 4u
static struct {
	struct vsp_p_ui_action ui[VSP_AWAIT_MAX];
	uint32_t op[VSP_AWAIT_MAX];
	unsigned head, tail; /* queue of expected receipts, production order */
	uint32_t done_mask;  /* bit per VSP_UI action already recorded */
	uint32_t reserved_ms;
} s_await;

void vsp_await_dispatch(uint8_t action, uint32_t player, uint8_t screen,
						uint8_t button, uint32_t edge_seq, uint32_t op) {
	if(s_await.tail - s_await.head >= VSP_AWAIT_MAX)
		return;
	struct vsp_p_ui_action* u = &s_await.ui[s_await.tail % VSP_AWAIT_MAX];
	memset(u, 0, sizeof(*u));
	u->action = action;
	u->player = player;
	u->screen = screen;
	u->button = button;
	u->edge_seq = edge_seq;
	u->delivered = true;
	s_await.op[s_await.tail % VSP_AWAIT_MAX] = op;
	s_await.tail++;
	if(action < 32u)
		s_await.done_mask &= ~(1u << action);
}

uint32_t vsp_dispatch_reserve(void) {
	if(!vsp_armed() || s_await.head == s_await.tail)
		return 0;
	s_await.reserved_ms = vsp_now_ms();
	uint32_t order = __atomic_add_fetch(&s_push_order, 1u, __ATOMIC_ACQ_REL);
	if(!order) /* 0 means "stamp at push": skip it on wrap */
		order = __atomic_add_fetch(&s_push_order, 1u, __ATOMIC_ACQ_REL);
	return order;
}

void vsp_dispatch_sent(uint8_t action, uint32_t order) {
	if(!order || action == VSP_UI_NONE_SENT || !vsp_armed()
	   || s_await.head == s_await.tail)
		return;
	const unsigned i = s_await.head % VSP_AWAIT_MAX;
	if(s_await.ui[i].action != action)
		return; /* not the receipt the driver awaits next */
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_UI_ACTION, VSP_SYNTHETIC);
	r.op_id = s_await.op[i];
	r.u.ui = s_await.ui[i];
	r.u.ui.input_epoch = r.u.ui.player ? vsp_client_actor_epoch(r.u.ui.player)
									   : clin_input_epoch();
	r.ms = s_await.reserved_ms;
	s_await.head++;
	if(action < 32u)
		s_await.done_mask |= 1u << action;
	/* stamped like any main row, then given the order reserved BEFORE the
	 * send: no main row was pushed in between (same thread, synchronous
	 * send), so the main FIFO stays in push order. */
	vsp_stamp(&r);
	r.push_order = order;
	if(s_main_tail - s_main_head >= VSP_MAIN_FIFO) {
		__atomic_fetch_add(&s_overflow, 1u, __ATOMIC_ACQ_REL);
		return;
	}
	s_main_q[s_main_tail % VSP_MAIN_FIFO] = r;
	s_main_tail++;
}

static void vsp_await_reset(void) {
	memset(&s_await, 0, sizeof(s_await));
}

bool vsp_dispatch_done(uint8_t action) {
	return action < 32u && (s_await.done_mask & (1u << action));
}

uint8_t vsp_ui_of_guest_action(int a) {
	switch(a) {
		case LOCAL_GUEST_JOIN: return VSP_UI_GUEST_JOIN;
		case LOCAL_GUEST_ACCEPT: return VSP_UI_ACCEPT;
		case LOCAL_GUEST_CONFIRM: return VSP_UI_CONFIRM;
		case LOCAL_GUEST_GAMEPLAY: return VSP_UI_GAMEPLAY;
		default: return VSP_UI_NONE_SENT;
	}
}

size_t vsp_pending_records(void) {
	return (size_t)(LOAD(s_srv_tail) - LOAD(s_srv_head))
		+ (size_t)(s_main_tail - s_main_head);
}

/* O04: merge the two FIFOs in PUSH order. Draining the whole server FIFO
 * first let a server authority/apply row that was pushed AFTER a main-thread
 * action_attempt -- both pending in one emit pass -- take the lower seq
 * (measured: PC boat/pig tapes 2026-10-04 09:03, authority seq 41 at ms 11676
 * before attempt seq 42 at ms 11660 -> vehicle_action_correlation). Each FIFO
 * is in push order on its own, so comparing the two heads restores the
 * causal order (attempt -> RPC -> server result). A server row whose order is
 * taken but whose tail store is not yet visible can only be LATER than an
 * attempt that caused it, so it can never be overtaken by its own cause. */
bool vsp_pop(struct vsp_record* out) {
	uint32_t head = __atomic_load_n(&s_srv_head, __ATOMIC_RELAXED);
	bool srv = head != LOAD(s_srv_tail);
	bool mainq = s_main_head != s_main_tail;
	if(srv && mainq
	   && (int32_t)(s_main_q[s_main_head % VSP_MAIN_FIFO].push_order
					- s_srv_q[head % VSP_SRV_FIFO].push_order) < 0)
		srv = false; /* the main head was pushed first */
	if(srv) {
		*out = s_srv_q[head % VSP_SRV_FIFO];
		STORE(s_srv_head, head + 1u);
		return true;
	}
	if(mainq) {
		*out = s_main_q[s_main_head % VSP_MAIN_FIFO];
		s_main_head++;
		return true;
	}
	return false;
}

static void vsp_rec_init(struct vsp_record* rec, enum vsp_event ev,
						 enum vsp_origin origin) {
	memset(rec, 0, sizeof(*rec));
	rec->event = (uint8_t)ev;
	rec->origin = (uint8_t)origin;
	rec->ok = true;
	rec->op_id = LOAD(s_op);
}

static uint64_t vsp_next_serial(void) {
	uint32_t v = __atomic_add_fetch(&s_serial, 1u, __ATOMIC_ACQ_REL);
	if(v == 0u || v > 0xFFFFFF00u) {
		vsp_srv_fixture_failed("serial_wrap");
		return 0xFFFFFFFFull;
	}
	return (uint64_t)v;
}

/* ---- commands / windows ------------------------------------------------- */
void vsp_cmd_fixture(uint32_t op_id) {
	vsp_hook_set(VSP_HOOK_FIXTURE, true);
	STORE(s_cmd[VSP_CMD_FIXTURE], op_id);
}
void vsp_cmd_lethal(uint32_t op_id) {
	vsp_hook_set(VSP_HOOK_LETHAL, true);
	STORE(s_cmd[VSP_CMD_LETHAL], op_id);
}
void vsp_cmd_transfer(uint32_t op_id) {
	vsp_hook_set(VSP_HOOK_TRANSFER, true);
	STORE(s_cmd[VSP_CMD_TRANSFER], op_id);
}
void vsp_cmd_refusal(uint32_t op_id) {
	vsp_hook_set(VSP_HOOK_REFUSAL, true);
	STORE(s_cmd_refusal, op_id);
}

uint32_t vsp_take_cmd(enum vsp_cmd cmd) {
	if(cmd >= VSP_CMD_COUNT)
		return 0;
	static const uint32_t bits[VSP_CMD_COUNT]
		= {VSP_HOOK_FIXTURE, VSP_HOOK_LETHAL, VSP_HOOK_TRANSFER};
	if(!(LOAD(s_hook_mask) & bits[cmd]))
		return 0;
	uint32_t op = __atomic_exchange_n(&s_cmd[cmd], 0u, __ATOMIC_ACQ_REL);
	/* One-shot: the hook bit drops with the command it carried. */
	if(op)
		vsp_hook_set(bits[cmd], false);
	return op;
}

void vsp_expect_reload(uint32_t checkpoint_id) {
	STORE(s_expect_reload, checkpoint_id);
}

bool vsp_fixture_targets(struct vsp_r out[3], int32_t anchor[3]) {
	if(!LOAD(s_fixture_published))
		return false;
	if(out)
		memcpy(out, s_fixture_targets, sizeof(s_fixture_targets));
	if(anchor)
		memcpy(anchor, s_fixture_anchor, sizeof(s_fixture_anchor));
	return true;
}

uint32_t vsp_fixture_hash_now(void) {
	return LOAD(s_fixture_hash);
}

static uint32_t s_req_ride_detached;
void vsp_ride_window_detached(uint32_t window_id, uint32_t target_id,
							  int world_slot, bool open) {
	STORE(s_req_ride_detached, 1u);
	C.ride_detached = true;
	s_client_ride_slot = world_slot;
	STORE(s_req_ride_target, target_id);
	STORE(s_req_ride_id, window_id);
	STORE(s_req_ride_open, open ? 1u : 0u);
}

void vsp_ride_window(uint32_t window_id, uint32_t target_id, bool open) {
	STORE(s_req_ride_detached, 0u);
	C.ride_detached = false;
	s_client_ride_slot = -1;
	STORE(s_req_ride_target, target_id);
	STORE(s_req_ride_id, window_id);
	STORE(s_req_ride_open, open ? 1u : 0u);
}

void vsp_iso_target(uint32_t target_id) {
	STORE(s_req_iso_target, target_id);
}

void vsp_iso_bracket(uint32_t attempt_op, bool open) {
	STORE(s_req_iso_op, attempt_op);
	STORE(s_req_iso_open, open ? 1u : 0u);
}

bool vsp_iso_server_closed(uint32_t attempt_op) {
	return LOAD(s_ack_iso_closed) == attempt_op;
}

bool vsp_ride_server_closed(uint32_t window_id) {
	return LOAD(s_ack_ride_closed) == window_id;
}

/* ---- FNV-1a ------------------------------------------------------------- */
uint32_t vsp_fnv_begin(void) {
	return 2166136261u;
}

uint32_t vsp_fnv_bytes(uint32_t h, const void* data, size_t n) {
	const uint8_t* p = data;
	for(size_t k = 0; k < n; k++) {
		h ^= p[k];
		h *= 16777619u;
	}
	return h;
}

/* CONTRACT-v1: the existing sr_inventory_hash byte order -- id LE16, count U8,
 * durability LE16 per slot, all INVENTORY_SIZE slots, empty normalized. */
static uint32_t vsp_items_hash(const struct item_data* items, size_t n,
							   uint32_t* used_out) {
	uint32_t h = vsp_fnv_begin(), used = 0;
	for(size_t k = 0; k < n; k++) {
		struct item_data it = items[k];
		if(!it.id || !it.count)
			it = (struct item_data) {0};
		else
			used++;
		uint8_t b[5] = {(uint8_t)(it.id & 0xFF), (uint8_t)(it.id >> 8), it.count,
						(uint8_t)(it.durability & 0xFF),
						(uint8_t)(it.durability >> 8)};
		h = vsp_fnv_bytes(h, b, sizeof(b));
	}
	if(used_out)
		*used_out = used;
	return h;
}

/* ---- server snapshots (server thread) ----------------------------------- */
static struct vsp_w vsp_w_of(const struct server_world* w) {
	struct vsp_w out = {0};
	if(!w || !server_local_world_live(w))
		return out;
	out.slot = w->world_index;
	out.generation = w->generation;
	out.dimension = (int32_t)w->dimension;
	return out;
}

static uint8_t vsp_kind_of_entity(const struct entity* e) {
	if(!e)
		return VSP_KIND_COUNT;
	if(e->type == ENTITY_BOAT)
		return VSP_BOAT;
	if(e->type == ENTITY_MINECART)
		return VSP_CART;
	if(e->type == ENTITY_MONSTER && e->data.monster.id == MONSTER_PIG)
		return VSP_PIG;
	return VSP_KIND_COUNT;
}

/* CONTRACT-v1.1 Q5: the named actor's OWN active input epoch, read on the
 * server (owner) thread. Only owner 0 has an input-epoch handoff in
 * production (server_local.input_epoch_counter, acknowledged through
 * input_ack_pending / clin_submit_input_ack). Slot 1 has none: its
 * handoffs are the guest route generation and guest_respawn_epoch, carried
 * separately in actor.generation. It is therefore 0 -- never the host
 * allocator's value. */
static uint32_t vsp_srv_actor_epoch(const struct server_local* s,
									const struct server_player* p) {
	return s && p == &s->player ? s->input_epoch_counter : 0u;
}

static struct vsp_a vsp_a_srv(struct server_local* s,
							  const struct server_player* p) {
	struct vsp_a a = {0};
	if(!s || !p)
		return a;
	struct actor_ident id = server_local_actor_ident(s, p);
	a.slot = id.slot;
	a.generation = id.generation;
	a.wire_id = server_local_actor_wire_id(s, p);
	a.input_epoch = vsp_srv_actor_epoch(s, p);
	return a;
}

/* Valid relations naming `p` (lifetime + wire id), every kind, every world. */
static uint32_t vsp_srv_mounts(struct server_local* s,
							   const struct server_player* p,
							   uint32_t* rel_mask) {
	uint32_t n = 0, mask = 0;
	if(!s || !p) {
		if(rel_mask)
			*rel_mask = 0;
		return 0;
	}
	struct actor_ident me = server_local_actor_ident(s, p);
	uint32_t wire = server_local_actor_wire_id(s, p);
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		const struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(!e)
			continue;
		if(e->type == ENTITY_BOAT && e->data.boat.occupied
		   && actor_ident_equal(e->data.boat.rider_actor, me)
		   && e->data.boat.occupant_id == wire) {
			n++;
			mask |= 1u;
		} else if(e->type == ENTITY_MONSTER && e->data.monster.id == MONSTER_PIG
				  && e->data.monster.pig_ridden
				  && actor_ident_equal(e->data.monster.pig_rider_actor, me)
				  && e->data.monster.pig_rider_id == wire) {
			n++;
			mask |= 2u;
		} else if(e->type == ENTITY_MINECART && e->data.minecart.occupied
				  && actor_ident_equal(e->data.minecart.rider_actor, me)
				  && e->data.minecart.occupant_id == wire) {
			n++;
			mask |= 4u;
		}
	}
	if(rel_mask)
		*rel_mask = mask;
	return n;
}

static struct vsp_s vsp_s_srv(struct server_local* s,
							  struct server_player* p) {
	struct vsp_s o = {0};
	if(!s || !p)
		return o;
	o.actor = vsp_a_srv(s, p);
	o.world = vsp_w_of(server_local_world_of(s, p));
	o.pos[0] = (float)p->x;
	o.pos[1] = (float)p->y;
	o.pos[2] = (float)p->z;
	o.steer[0] = p->rider_vel_x;
	o.steer[1] = p->rider_vel_z;
	o.health = p->health;
	o.mount_count = vsp_srv_mounts(s, p, NULL);
	o.riding_boat = p->riding_boat;
	o.riding_cart = p->riding_minecart;
	o.inv_hash = vsp_items_hash(p->inventory.items, INVENTORY_SIZE, &o.inv_used);
	if(p == &s->player) {
		o.ready = p->has_pos && p->finished_loading && !s->input_ack_pending;
		o.confirmed = p->has_pos;
	} else {
		o.ready = s->guest_gameplay && p->finished_loading;
		o.confirmed = s->guest_confirmed;
	}
	return o;
}

static struct vsp_v vsp_v_srv(struct server_local* s, uint32_t id) {
	struct vsp_v o = {0};
	o.target.id = id;
	o.target.kind = s_cfg.kind;
	struct entity** pp = s ? dict_entity_get(s->entities, id) : NULL;
	struct entity* e = pp ? *pp : NULL;
	if(!e) {
		/* keep the last known kind if this was a fixture target */
		for(unsigned k = 0; k < 3; k++)
			if(s_fixture_targets[k].id == id) {
				o.target = s_fixture_targets[k];
				o.target.world.generation = 0; /* dead: absent world */
			}
		return o;
	}
	o.alive = true;
	o.target.kind = vsp_kind_of_entity(e);
	o.target.world = vsp_w_of(server_local_entity_world(s, e));
	o.pos[0] = e->pos[0];
	o.pos[1] = e->pos[1];
	o.pos[2] = e->pos[2];
	struct actor_ident rider = ACTOR_IDENT_HOST;
	uint32_t wire = 0;
	if(e->type == ENTITY_BOAT) {
		o.occupied = e->data.boat.occupied;
		rider = e->data.boat.rider_actor;
		wire = e->data.boat.occupant_id;
		o.steer[0] = e->data.boat.rider_vel_x;
		o.steer[1] = e->data.boat.rider_vel_z;
	} else if(e->type == ENTITY_MINECART) {
		o.occupied = e->data.minecart.occupied;
		rider = e->data.minecart.rider_actor;
		wire = e->data.minecart.occupant_id;
		o.ride_epoch = e->data.minecart.ride_epoch;
		o.control_seq = e->data.minecart.control_seq;
		o.positive_steps = e->data.minecart.control_positive_steps;
		o.negative_steps = e->data.minecart.control_negative_steps;
		o.pending_throttle = e->data.minecart.pending_throttle;
	} else if(e->type == ENTITY_MONSTER) {
		o.occupied = e->data.monster.pig_ridden;
		rider = e->data.monster.pig_rider_actor;
		wire = e->data.monster.pig_rider_id;
		o.saddled = e->data.monster.saddled;
	}
	/* The writer's own zero default when unoccupied (CONTRACT-v1: not proof of
	 * occupation); the stored identity otherwise. */
	o.rider.slot = rider.slot;
	o.rider.generation = rider.generation;
	o.rider.wire_id = wire;
	if(o.occupied) {
		struct server_player* rp = server_local_actor_resolve(s, rider);
		o.rider.input_epoch = vsp_srv_actor_epoch(s, rp);
	}
	return o;
}

/* The vehicle the host validly rides (any kind), 0 = none. */
static uint32_t vsp_srv_vehicle_of(struct server_local* s,
								   const struct server_player* p) {
	if(!s || !p)
		return 0;
	struct actor_ident me = server_local_actor_ident(s, p);
	uint32_t wire = server_local_actor_wire_id(s, p);
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		const struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(!e)
			continue;
		if((e->type == ENTITY_BOAT && e->data.boat.occupied
			&& actor_ident_equal(e->data.boat.rider_actor, me)
			&& e->data.boat.occupant_id == wire)
		   || (e->type == ENTITY_MONSTER && e->data.monster.id == MONSTER_PIG
			   && e->data.monster.pig_ridden
			   && actor_ident_equal(e->data.monster.pig_rider_actor, me)
			   && e->data.monster.pig_rider_id == wire)
		   || (e->type == ENTITY_MINECART && e->data.minecart.occupied
			   && actor_ident_equal(e->data.minecart.rider_actor, me)
			   && e->data.minecart.occupant_id == wire))
			return e->id;
	}
	return 0;
}

static struct vsp_q vsp_q_srv(struct server_local* s) {
	struct vsp_q q = {0};
	q.ticket = s->guest_accept_publish.ticket;
	q.generation = s->guest_accept_publish.generation;
	struct server_world* w = server_local_world_from_handle(
		s, s->guest_accept_publish.world_handle);
	q.world = vsp_w_of(w);
	return q;
}

static bool vsp_q_equal(const struct vsp_q* a, const struct vsp_q* b) {
	return a->ticket == b->ticket && a->generation == b->generation
		&& a->world.slot == b->world.slot
		&& a->world.generation == b->world.generation;
}

static bool vsp_guest_present(struct server_local* s) {
	return s->guest_actor && s->guest_gameplay;
}

/* ---- server producers --------------------------------------------------- */
static void vsp_progress_push(struct server_local* s, enum vsp_event ev,
							  enum vsp_origin origin, uint8_t stage,
							  uint8_t cause) {
	struct vsp_record r;
	vsp_rec_init(&r, ev, origin);
	r.op_id = S.refusal_op;
	struct vsp_p_progress* p = &r.u.progress;
	p->tuple = S.q;
	p->serial = vsp_next_serial();
	p->stage = stage;
	p->cause = cause;
	p->paused = s->paused;
	p->loading = !s->player.finished_loading || s->input_ack_pending;
	p->transfer_open = server_transfer_active(s);
	p->refusal_armed = S.refusal_armed;
	p->pending = s->guest_accept_publish.pending;
	p->transfer_id = S.clear_transfer_id;
	p->installed_count = S.installed_count;
	p->host_ack_pending = s->input_ack_pending != 0;
	p->result = 0;
	(void)vsp_srv_push(&r);
}

bool vsp_server_host_at_rest(uint32_t min_updates) {
	uint32_t since = LOAD(s_host_rest_since);
	return since && LOAD(s_server_update) - since >= min_updates;
}

bool vsp_server_guest_at_rest(uint32_t min_updates) {
	uint32_t since = LOAD(s_guest_rest_since);
	return since && LOAD(s_server_update) - since >= min_updates;
}

double vsp_death_region_distance(int32_t ax, int32_t az, double x, double z) {
	const double x0 = ax + VSP_DEATH_D_X0, x1 = ax + VSP_DEATH_D_X1;
	const double z0 = az + VSP_DEATH_D_Z0, z1 = az + VSP_DEATH_D_Z1;
	const double dx = x < x0 ? x0 - x : (x > x1 ? x - x1 : 0.0);
	const double dz = z < z0 ? z0 - z : (z > z1 ? z - z1 : 0.0);
	return sqrt(dx * dx + dz * dz);
}

void vsp_far_home_enforce(void) {
	STORE(s_far_home_req, 1u);
}

double vsp_far_home_min_m(void) {
	uint32_t mm = LOAD(s_far_home_min_mm);
	return mm ? (double)(mm - 1u) / 1000.0 : -1.0;
}

/* O14: from the settle ack to disarm, the guest stays >= N from D (H2 dwell,
 * the declared Leave/rejoin placement and p5). Server thread. */
static void vsp_srv_far_home(struct server_local* s) {
	if(!LOAD(s_far_home_req) || !LOAD(s_fixture_published))
		return;
	const struct server_player* g = s->guest_actor;
	if(!g || !g->has_pos || g->dimension != WORLD_DIM_OVERWORLD)
		return; /* absent (declared Leave gap) or elsewhere: no pickup here */
	double d = vsp_death_region_distance(s_fixture_anchor[0], s_fixture_anchor[2],
										 g->x, g->z);
	uint32_t mm = (uint32_t)(d * 1000.0) + 1u;
	uint32_t prev = LOAD(s_far_home_min_mm);
	if(!prev || mm < prev)
		STORE(s_far_home_min_mm, mm);
	if(d < VSP_DROP_N_M)
		vsp_srv_fixture_failed("guest_inside_drop_envelope");
	/* Diagnostic evidence (stderr at disarm, never on the wire): the nearest
	 * item entity in the guest's world over the whole enforced span. */
	const struct server_world* gw = server_local_world_of(s, s->guest_actor);
	uint32_t seen = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		const struct entity* e = dict_entity_ref(it)->value;
		if(!e || e->type != ENTITY_ITEM || (gw && server_local_entity_world(s, e) != gw))
			continue;
		seen++;
		const double ex = fabs((double)e->pos[0] - g->x);
		const double ez = fabs((double)e->pos[2] - g->z);
		const uint32_t eu = (uint32_t)(sqrt(ex * ex + ez * ez) * 1000.0) + 1u;
		const uint32_t ch = (uint32_t)((ex > ez ? ex : ez) * 1000.0) + 1u;
		const uint32_t pe = LOAD(s_far_item_min_mm), pc = LOAD(s_far_item_cheb_mm);
		if(!pe || eu < pe)
			STORE(s_far_item_min_mm, eu);
		if(!pc || ch < pc)
			STORE(s_far_item_cheb_mm, ch);
	}
	if(seen > LOAD(s_far_item_seen))
		STORE(s_far_item_seen, seen);
}

void vsp_far_home_report(void) {
	/* main thread at disarm; the server thread has stopped producing */
	fprintf(stderr, "[vehicle_smoke] O14 far-home: guest-to-D min %.3f m (N %.1f); "
			"nearest item %.3f m euclid / %.3f m per-axis (pickup 1.425); "
			"max items seen %u\n",
			vsp_far_home_min_m(), VSP_DROP_N_M,
			LOAD(s_far_item_min_mm) ? (LOAD(s_far_item_min_mm) - 1u) / 1000.0 : -1.0,
			LOAD(s_far_item_cheb_mm) ? (LOAD(s_far_item_cheb_mm) - 1u) / 1000.0 : -1.0,
			(unsigned)LOAD(s_far_item_seen));
}

void vsp_srv_update_begin(struct server_local* s) {
	__atomic_add_fetch(&s_server_update, 1u, __ATOMIC_ACQ_REL);
	/* The SERVER's own rest fact for the driver's negative probes: host
	 * steering and its mount's steering both zero (the server's rider
	 * steering lags the client's; measured -0.02 for >10 updates). */
	if(s) {
		struct vsp_s h = vsp_s_srv(s, &s->player);
		uint32_t vid = vsp_srv_vehicle_of(s, &s->player);
		struct vsp_v v = vid ? vsp_v_srv(s, vid) : (struct vsp_v) {0};
		bool rest = h.steer[0] == 0.0F && h.steer[1] == 0.0F
			&& v.steer[0] == 0.0F && v.steer[1] == 0.0F;
		if(!rest)
			STORE(s_host_rest_since, 0u);
		else if(!LOAD(s_host_rest_since))
			STORE(s_host_rest_since, LOAD(s_server_update));
		/* O14 settling ack: the authoritative guest body has not moved
		 * (< 0.0001 m) since the previous update, same lifetime. */
		const struct server_player* g = s->guest_actor;
		bool g_rest = false;
		if(g && g->has_pos) {
			if(S.guest_last_valid && S.guest_last_gen == s->guest_generation) {
				double dx = g->x - S.guest_last[0], dy = g->y - S.guest_last[1];
				double dz = g->z - S.guest_last[2];
				g_rest = dx * dx + dy * dy + dz * dz < 1e-8;
			}
			S.guest_last[0] = g->x;
			S.guest_last[1] = g->y;
			S.guest_last[2] = g->z;
			S.guest_last_gen = s->guest_generation;
			S.guest_last_valid = true;
		} else
			S.guest_last_valid = false;
		if(!g_rest)
			STORE(s_guest_rest_since, 0u);
		else if(!LOAD(s_guest_rest_since))
			STORE(s_guest_rest_since, LOAD(s_server_update));
	}
	/* A cleared hook mask retires the server-side refusal latch. */
	if(S.refusal_armed && !(LOAD(s_hook_mask) & VSP_HOOK_REFUSAL)) {
		S.refusal_armed = false;
		S.refusal_wanted = false;
	}
	S.ride_ticks_this_update = 0;
}

void vsp_srv_update_held(struct server_local* s, bool paused_hold) {
	if(!s || !S.q_valid || !s->guest_accept_publish.pending)
		return;
	if(paused_hold && !S.retained_pause_sent) {
		S.retained_pause_sent = true;
		vsp_progress_push(s, VSP_EV_PENDING, VSP_PRODUCTION, VSP_PD_RETAINED,
						  VSP_CAUSE_GLOBAL_PAUSE);
	}
}

static void vsp_srv_ride_census(struct server_local* s, bool loading,
								bool entity_tick_completed) {
	uint32_t req_open = LOAD(s_req_ride_open);
	uint32_t req_id = LOAD(s_req_ride_id);
	if(req_open && (!S.ride_open || S.ride_id != req_id)) {
		S.ride_open = true;
		S.ride_id = req_id;
		S.ride_target = LOAD(s_req_ride_target);
		S.ride_detached = LOAD(s_req_ride_detached) != 0;
		S.ride_eligible = S.ride_ticks = S.ride_missing = S.ride_duplicate = 0;
		S.ride_disp_max = 0;
		struct vsp_v v = vsp_v_srv(s, S.ride_target);
		memcpy(S.ride_start, v.pos, sizeof(S.ride_start));
		/* The opening update's entity pass ran before the window existed:
		 * count from the next update on. */
		return;
	}
	if(S.ride_open && !req_open && S.ride_id == req_id) {
		struct vsp_record r;
		vsp_rec_init(&r, VSP_EV_RIDE_SAMPLE, VSP_PRODUCTION);
		struct vsp_p_ride_sample* p = &r.u.ride;
		p->stage = VSP_STAGE_SERVER;
		p->u.server.vehicle = vsp_v_srv(s, S.ride_target);
		p->target = p->u.server.vehicle.target;
		p->window_id = S.ride_id;
		p->samples = S.ride_eligible;
		p->eligible_updates = S.ride_eligible;
		p->tick_calls = S.ride_ticks;
		p->missing_calls = S.ride_missing;
		p->duplicate_calls = S.ride_duplicate;
		p->displacement_m = S.ride_disp_max;
		p->u.server.host = vsp_s_srv(s, &s->player);
		p->seat_error_max_m = 0;
		/* Follow is a CLIENT observation (Q3): the server has none. */
		p->follow_count = 0;
		/* A cart window correlates with the request the authority ACCEPTED
		 * for this cart's current ride; boat/pig rides carry no request. */
		/* O06: the p5 DETACHED census has no live ride: no request, the
		 * same 0 the client's detached sample carries (first reached once
		 * the cart could complete p1-p4). */
		p->request_seq = p->target.kind == VSP_CART && !S.ride_detached
			&& S.cart_req_target == S.ride_target ? S.cart_req_seq : 0u;
		if(vsp_srv_push(&r)) {
			S.ride_open = false;
			STORE(s_ack_ride_closed, S.ride_id);
		}
		return;
	}
	if(!S.ride_open || loading || !entity_tick_completed)
		return;
	/* eligible = the target ticked this update while the host validly rode
	 * it; a mounted update without a tick is missing, two are duplicate.
	 * Detached census: eligible = the released target is alive and NOT the
	 * host's mount (it keeps being ticked by its own world). */
	if(S.ride_detached) {
		if(vsp_srv_vehicle_of(s, &s->player) == S.ride_target
		   || !vsp_v_srv(s, S.ride_target).alive)
			return;
	} else if(vsp_srv_vehicle_of(s, &s->player) != S.ride_target)
		return;
	S.ride_eligible++;
	S.ride_ticks += S.ride_ticks_this_update;
	if(S.ride_ticks_this_update == 0)
		S.ride_missing++;
	else if(S.ride_ticks_this_update > 1)
		S.ride_duplicate++;
	struct vsp_v v = vsp_v_srv(s, S.ride_target);
	float dx = v.pos[0] - S.ride_start[0], dz = v.pos[2] - S.ride_start[2];
	float d = sqrtf(dx * dx + dz * dz);
	if(d > S.ride_disp_max)
		S.ride_disp_max = d;
}

static void vsp_iso_push(struct server_local* s, uint8_t stage) {
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_ISOLATION, VSP_PRODUCTION);
	r.op_id = S.iso_op;
	struct vsp_p_isolation* p = &r.u.iso;
	p->stage = stage;
	p->owner = VSP_STAGE_SERVER;
	p->attempt_op = S.iso_op;
	p->u.server.host = vsp_s_srv(s, &s->player);
	p->u.server.guest = vsp_s_srv(s, s->guest_actor);
	/* The bracket's own vehicle: a probe names its target (the same R the
	 * attempt and the client bracket carry); otherwise the host's mount. */
	uint32_t vid = S.iso_target ? S.iso_target : vsp_srv_vehicle_of(s, &s->player);
	if(!vid && s_fixture_targets[s_cfg.kind].id)
		vid = s_fixture_targets[s_cfg.kind].id;
	p->u.server.vehicle = vsp_v_srv(s, vid);
	p->guest_pose_delta_max_m = S.iso_pose_max;
	p->guest_follow_count = S.iso_follow;
	p->unexpected_attach_count = S.iso_attach;
	(void)vsp_srv_push(&r);
}

/* O11: an item entity in the guest's world within VSP_COLLECT_GUARD_M
 * (horizontal) of the guest body: Beta's pickup reach (player box expanded
 * 1.0 on X/Z, 0.3 + 1.0 + the item's 0.125) plus a settle margin.
 * O14: that reach is an axis-aligned BOX (1.425 m per axis), so its corner
 * is 1.425 * sqrt 2 = 2.016 m away; the old 2.0 m radius missed it. Guard =
 * 2.016 + 0.5 margin. */
#define VSP_COLLECT_GUARD_M (VSP_PICKUP_REACH_EUCLID_M + 0.5)
bool vsp_srv_collectible_near_guest(struct server_local* s) {
	if(!s || !s->guest_actor || !s->guest_actor->has_pos)
		return false;
	const struct server_world* gw = server_local_world_of(s, s->guest_actor);
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(!e || e->type != ENTITY_ITEM || e->delay_destroy == 0)
			continue;
		if(gw && server_local_entity_world(s, e) != gw)
			continue;
		const double dx = (double)e->pos[0] - s->guest_actor->x;
		const double dz = (double)e->pos[2] - s->guest_actor->z;
		const double dy = (double)e->pos[1] - s->guest_actor->y;
		if(dx * dx + dz * dz < VSP_COLLECT_GUARD_M * VSP_COLLECT_GUARD_M
		   && dy > -4.0 && dy < 2.0)
			return true;
	}
	return false;
}

static void vsp_srv_iso_census(struct server_local* s) {
	uint32_t req_open = LOAD(s_req_iso_open);
	uint32_t req_op = LOAD(s_req_iso_op);
	if(req_open && (!S.iso_open || S.iso_op != req_op)) {
		S.iso_open = true;
		S.iso_op = req_op;
		S.iso_target = LOAD(s_req_iso_target);
		S.iso_follow = S.iso_attach = 0;
		S.iso_pose_max = 0;
		S.iso_pose_gen = 0;
		if(s->guest_actor) {
			S.iso_pose0[0] = (float)s->guest_actor->x;
			S.iso_pose0[1] = (float)s->guest_actor->y;
			S.iso_pose0[2] = (float)s->guest_actor->z;
			S.iso_pose_gen = s->guest_generation;
		}
		(void)vsp_srv_mounts(s, s->guest_actor, &S.guest_rel_prev);
		vsp_iso_push(s, VSP_STAGE_BEFORE);
		/* O11 / S03 #3: the P2 inventory invariant is only meaningful if
		 * nothing collectible lies within the guest's reach when the window
		 * opens; a fixture that leaves one there fails BY NAME (never a
		 * silent pickup that the invariant then blames on a misroute). */
		if(vsp_srv_collectible_near_guest(s))
			vsp_srv_fixture_failed("isolation_collectible_near_guest");
		return;
	}
	if(!S.iso_open)
		return;
	vsp_srv_guest_edge_census(s);
	/* A readmission (declared Leave -> new ACCEPT) is a NEW guest lifetime
	 * at its own spawn: the excursion is measured from that lifetime's first
	 * installed pose, never across the gap (contract: absent bodies have no
	 * fabricated pose). Same rule as the client bracket. */
	if(s->guest_actor && s->guest_generation != S.iso_pose_gen) {
		S.iso_pose0[0] = (float)s->guest_actor->x;
		S.iso_pose0[1] = (float)s->guest_actor->y;
		S.iso_pose0[2] = (float)s->guest_actor->z;
		S.iso_pose_gen = s->guest_generation;
	}
	if(s->guest_actor) {
		float dx = (float)s->guest_actor->x - S.iso_pose0[0];
		float dy = (float)s->guest_actor->y - S.iso_pose0[1];
		float dz = (float)s->guest_actor->z - S.iso_pose0[2];
		float d = sqrtf(dx * dx + dy * dy + dz * dz);
		if(d > S.iso_pose_max)
			S.iso_pose_max = d;
	}
	if(!req_open && S.iso_op == req_op) {
		vsp_iso_push(s, VSP_STAGE_AFTER);
		S.iso_open = false;
		STORE(s_ack_iso_closed, S.iso_op);
	}
}

void vsp_srv_guest_edge_census(struct server_local* s) {
	if(!s || !S.iso_open)
		return;
	uint32_t mask = 0;
	(void)vsp_srv_mounts(s, s->guest_actor, &mask);
	/* each distinct false->true native relation edge, counted once */
	uint32_t gained = mask & ~S.guest_rel_prev;
	for(unsigned b = 0; b < 3; b++)
		if(gained & (1u << b))
			S.iso_attach++;
	if(s->guest_actor && (s->guest_actor->riding_boat
						  || s->guest_actor->riding_minecart)
	   && !(S.guest_rel_prev & 8u)) {
		S.iso_attach++;
		mask |= 8u;
	}
	S.guest_rel_prev = mask;
}

static void vsp_srv_reload_entities(struct server_local* s) {
	if(!LOAD(s_expect_reload) || S.reload_entities_done
	   || !S.reload_disk_done || !S.reload_inv_done || !vsp_guest_present(s))
		return;
	/* Reacquire remapped entities by unique fixture kind near the saved
	 * fixture location -- never by a stale id. */
	struct vsp_r found[3] = {0};
	unsigned count[3] = {0};
	bool saddle = false;
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		const struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		uint8_t k = vsp_kind_of_entity(e);
		if(k >= VSP_KIND_COUNT)
			continue;
		if(k == VSP_CART && e->data.minecart.minecart_type != 0)
			continue;
		float dx = e->pos[0] - (float)s_fixture_anchor[0];
		float dz = e->pos[2] - (float)s_fixture_anchor[2];
		if(dx * dx + dz * dz > 12.0F * 12.0F)
			continue;
		count[k]++;
		found[k].id = e->id;
		found[k].kind = k;
		found[k].world = vsp_w_of(server_local_entity_world(s, e));
		if(k == VSP_PIG)
			saddle = e->data.monster.saddled;
	}
	if(!count[0] || !count[1] || !count[2])
		return; /* not all restored yet; the script's deadline bounds it */
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_RELOAD, VSP_PRODUCTION);
	struct vsp_p_reload* p = &r.u.reload;
	p->stage = VSP_RL_ENTITIES;
	p->checkpoint_id = LOAD(s_expect_reload);
	p->u.entities.host = vsp_s_srv(s, &s->player);
	p->u.entities.guest = vsp_s_srv(s, s->guest_actor);
	memcpy(p->u.entities.targets, found, sizeof(found));
	p->u.entities.fixture_hash = vsp_live_fixture_hash(s);
	p->u.entities.saddle = saddle;
	for(unsigned k = 0; k < 3; k++)
		if(count[k] > 1)
			p->u.entities.ambiguous_matches++;
	if(vsp_srv_push(&r)) {
		S.reload_entities_done = true;
		memcpy(s_fixture_targets, found, sizeof(found));
	}
}

void vsp_srv_update_stable(struct server_local* s, bool loading,
						   bool entity_tick_completed) {
	if(!s)
		return;
	/* The seeded refusal is cleared exactly once, only while a real transfer
	 * is demonstrably open (CONTRACT-v1 ACCEPT REFUSAL). */
	if(S.refusal_armed && server_transfer_active(s)) {
		S.refusal_armed = false;
		S.refusal_cleared = true;
		for(size_t i = 0; i < 2; i++)
			if(s->transfer[i].open)
				S.clear_transfer_id = s->transfer[i].id;
		vsp_hook_set(VSP_HOOK_REFUSAL, false);
		vsp_progress_push(s, VSP_EV_ADMISSION_CLEAR, VSP_SYNTHETIC, 0, 0);
	}
	vsp_srv_ride_census(s, loading, entity_tick_completed);
	vsp_srv_iso_census(s);
	vsp_srv_far_home(s);
	vsp_srv_reload_entities(s);
}

void vsp_srv_retry_pass(struct server_local* s) {
	if(!s || s->paused || server_transfer_active(s))
		return;
	uint32_t active = __atomic_add_fetch(&s_active_update, 1u, __ATOMIC_ACQ_REL);
	(void)active;
	if(S.refusal_cleared && !S.barrier_released && S.q_valid
	   && s->guest_accept_publish.pending) {
		struct vsp_q now = vsp_q_srv(s);
		if(vsp_q_equal(&now, &S.q)) {
			S.barrier_released = true;
			vsp_progress_push(s, VSP_EV_BARRIER_RELEASE, VSP_PRODUCTION, 0, 0);
		}
	}
}

void vsp_srv_vehicle_ticked(struct server_local* s, struct entity* e) {
	(void)s;
	if(!e)
		return;
	if(S.ride_open && e->id == S.ride_target)
		S.ride_ticks_this_update++;
	if(e->id == s_fixture_targets[VSP_PIG].id && s_fixture_targets[VSP_PIG].id)
		S.pig_ai_updates++;
	if(S.iso_open && s && s->guest_actor) {
		/* S tick dispatcher: a ticked vehicle whose valid rider is the guest
		 * is one guest mounted-follow application. */
		struct actor_ident g = server_local_actor_ident(s, s->guest_actor);
		uint32_t gw = server_local_actor_wire_id(s, s->guest_actor);
		bool rides = (e->type == ENTITY_BOAT && e->data.boat.occupied
					  && actor_ident_equal(e->data.boat.rider_actor, g)
					  && e->data.boat.occupant_id == gw)
			|| (e->type == ENTITY_MONSTER && e->data.monster.pig_ridden
				&& actor_ident_equal(e->data.monster.pig_rider_actor, g)
				&& e->data.monster.pig_rider_id == gw)
			|| (e->type == ENTITY_MINECART && e->data.minecart.occupied
				&& actor_ident_equal(e->data.minecart.rider_actor, g)
				&& e->data.minecart.occupant_id == gw);
		if(rides)
			S.iso_follow++;
	}
}

uint32_t vsp_pig_ai_updates(void) {
	return S.pig_ai_updates;
}

static char s_fixture_failure[49];
static uint32_t s_fixture_failure_set;

void vsp_srv_fixture_failed(const char* code) {
	if(LOAD(s_fixture_failure_set))
		return; /* first failure wins; never overwritten */
	snprintf(s_fixture_failure, sizeof(s_fixture_failure), "%s",
			 code ? code : "fixture_failed");
	STORE(s_fixture_failure_set, 1u);
}

const char* vsp_fixture_failure(void) {
	return LOAD(s_fixture_failure_set) ? s_fixture_failure : NULL;
}

static bool s_transfer_fixture;

void vsp_srv_transfer_fixture(bool on) {
	s_transfer_fixture = on;
}

bool vsp_srv_transfer_fixture_active(void) {
	return s_transfer_fixture;
}

static void vsp_reset_failure(void) {
	s_fixture_failure[0] = 0;
	STORE(s_fixture_failure_set, 0u);
	s_transfer_fixture = false;
}

static uint32_t (*s_fixture_live_hash)(struct server_local* s);

static uint32_t vsp_live_fixture_hash(struct server_local* s) {
	return s_fixture_live_hash ? s_fixture_live_hash(s) : LOAD(s_fixture_hash);
}

void vsp_srv_fixture_built(struct server_local* s, uint32_t op,
						   const int32_t anchor[3], const uint32_t ids[3],
						   uint32_t fixture_hash,
						   uint32_t (*live_hash)(struct server_local* s)) {
	(void)op;
	s_fixture_live_hash = live_hash;
	memcpy(s_fixture_anchor, anchor, sizeof(s_fixture_anchor));
	for(unsigned k = 0; k < 3; k++) {
		struct entity** pp = dict_entity_get(s->entities, ids[k]);
		s_fixture_targets[k].id = ids[k];
		s_fixture_targets[k].kind = (uint8_t)k;
		s_fixture_targets[k].world = vsp_w_of(
			pp && *pp ? server_local_entity_world(s, *pp) : NULL);
	}
	STORE(s_fixture_hash, fixture_hash);
	S.pig_ai_updates = 0;
	STORE(s_fixture_published, 1u);
}

void vsp_srv_rig_ready(struct server_local* s, uint32_t op, bool water_ok,
					   bool support_ok, bool rails_ok, bool ray_ok,
					   bool saddle_consumed) {
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_RIG_READY, VSP_FIXTURE);
	r.op_id = op;
	struct vsp_p_rig_ready* p = &r.u.rig_ready;
	p->fixture_hash = s_fixture_hash;
	memcpy(p->targets, s_fixture_targets, sizeof(p->targets));
	p->host = vsp_s_srv(s, &s->player);
	p->guest = vsp_s_srv(s, s->guest_actor);
	p->water_ok = water_ok;
	p->support_ok = support_ok;
	p->rails_ok = rails_ok;
	p->ray_ok = ray_ok;
	p->saddle_consumed = saddle_consumed;
	p->pig_ai_updates = S.pig_ai_updates;
	(void)vsp_srv_push(&r);
}

bool vsp_srv_use_pre(struct server_local* s, struct server_player* actor,
					 uint32_t target_id, uint32_t request_seq,
					 struct vsp_use_pre* pre) {
	if(!s || !actor || !pre)
		return false;
	memset(pre, 0, sizeof(*pre));
	pre->target_id = target_id;
	pre->request_seq = request_seq;
	pre->actor_is_host = actor == &s->player;
	S.use_open = true;
	S.use_actor = actor;
	S.use_release_count = 0;
	S.use_release_serial = 0;
	pre->old_id = vsp_srv_vehicle_of(s, actor);
	if(pre->old_id)
		pre->old_before = vsp_v_srv(s, pre->old_id);
	struct entity** pp = dict_entity_get(s->entities, target_id);
	if(pp && *pp) {
		const struct entity* e = *pp;
		pre->old_kind = vsp_kind_of_entity(e);
		if(e->type == ENTITY_MONSTER)
			pre->target_saddled_before = e->data.monster.saddled;
	}
	return true;
}

static void vsp_result_push(struct server_local* s, struct server_player* actor,
							const struct vsp_use_pre* pre, enum vsp_rpc rpc,
							enum vsp_action action, enum vsp_status status,
							const char* reason, uint64_t serial) {
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_ACTION_RESULT, VSP_PRODUCTION);
	snprintf(r.reason, sizeof(r.reason), "%s", reason ? reason : "");
	struct vsp_p_action_result* p = &r.u.result;
	p->action = (uint8_t)action;
	p->source = vsp_a_srv(s, actor);
	p->u.authority.vehicle = vsp_v_srv(s, pre->target_id);
	p->target = p->u.authority.vehicle.target;
	p->site = VSP_SITE_AUTHORITY;
	p->status = (uint8_t)status;
	p->request_seq = pre->request_seq;
	p->ride_epoch = p->u.authority.vehicle.ride_epoch;
	p->serial = serial ? serial : vsp_next_serial();
	p->rpc = (uint8_t)rpc;
	p->u.authority.host = vsp_s_srv(s, &s->player);
	(void)vsp_srv_push(&r);
}

void vsp_srv_release_witness(struct server_local* s,
							 struct server_player* actor, uint32_t released_id) {
	if(!s || !actor || !S.use_open || actor != S.use_actor)
		return;
	/* Allocated HERE, at the release, never retroactively at the result. */
	uint64_t serial = vsp_next_serial();
	S.use_release_count++;
	S.use_release_serial = serial;
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_DISMOUNT, VSP_PRODUCTION);
	r.u.dismount.host = vsp_s_srv(s, actor);
	r.u.dismount.old = vsp_v_srv(s, released_id);
	r.u.dismount.serial = serial;
	(void)vsp_srv_push(&r);
}

void vsp_srv_use_post(struct server_local* s, struct server_player* actor,
					  const struct vsp_use_pre* pre, enum vsp_rpc rpc) {
	uint32_t release_count = S.use_release_count;
	uint64_t release_witness = S.use_release_serial;
	S.use_open = false;
	S.use_actor = NULL;
	S.use_release_count = 0;
	S.use_release_serial = 0;
	if(!s || !actor || !pre)
		return;
	uint32_t now_id = vsp_srv_vehicle_of(s, actor);
	struct entity** pp = dict_entity_get(s->entities, pre->target_id);
	const struct entity* t = pp ? *pp : NULL;
	/* ATTACK: the only result this profile drives is saddle-by-hit. */
	if(rpc == VSP_RPC_ENTITY_ATTACK) {
		bool saddled = t && t->type == ENTITY_MONSTER && t->data.monster.saddled;
		if(vsp_kind_of_entity(t) != VSP_PIG)
			return;
		vsp_result_push(s, actor, pre, rpc, VSP_ACT_SADDLE,
						saddled && !pre->target_saddled_before ? VSP_ST_APPLIED
																: VSP_ST_REFUSED,
						saddled && !pre->target_saddled_before ? ""
						: saddled ? "already_saddled" : "not_saddled", 0);
		return;
	}
	if(!pre->actor_is_host) {
		/* A guest attempt: its result is a refusal unless the authority
		 * actually created a relation (which the isolation census also
		 * counts as an unexpected attach). */
		vsp_srv_guest_edge_census(s);
		bool applied = now_id && now_id != pre->old_id;
		vsp_result_push(s, actor, pre, rpc,
						rpc == VSP_RPC_MINECART_RIDE_INTENT
							? VSP_ACT_GUEST_CART_INTENT
							: vsp_op_action_or(VSP_ACT_GUEST_USE),
						applied ? VSP_ST_APPLIED : VSP_ST_REFUSED,
						applied ? "guest_relation_created"
								: rpc == VSP_RPC_ENTITY_USE
								&& vsp_kind_of_entity(t) == VSP_CART
								? "cart_use_unchanged_authority"
								: "guest_mount_fail_closed",
						0);
		return;
	}
	if(now_id == pre->old_id) {
		/* No relation change: refused / idempotent. */
		vsp_result_push(s, actor, pre, rpc, VSP_ACT_MOUNT, VSP_ST_REFUSED,
						"no_relation_change", 0);
		return;
	}
	if(pre->old_id && !now_id) {
		/* Explicit dismount. The production dismount writers (boat/pig
		 * occupancy false, the cart dismount branch) clear the relation
		 * directly, not through the shared release; the observation is
		 * taken once the handler returned -- the relation is already gone
		 * -- and the serial is allocated with that observation. A release
		 * witness the shared release emitted inside this bracket is the
		 * transaction itself and is referenced, never duplicated. */
		uint64_t serial = release_witness;
		if(release_count == 0) {
			serial = vsp_next_serial();
			struct vsp_record r;
			vsp_rec_init(&r, VSP_EV_DISMOUNT, VSP_PRODUCTION);
			r.u.dismount.host = vsp_s_srv(s, actor);
			r.u.dismount.old = vsp_v_srv(s, pre->old_id);
			r.u.dismount.serial = serial;
			(void)vsp_srv_push(&r);
		}
		vsp_result_push(s, actor, pre, rpc, VSP_ACT_DISMOUNT, VSP_ST_APPLIED,
						release_count > 1 ? "multiple_release_witnesses" : "",
						serial);
		return;
	}
	if(now_id && now_id != pre->old_id && rpc == VSP_RPC_MINECART_RIDE_INTENT) {
		S.cart_req_target = now_id;
		S.cart_req_seq = pre->request_seq;
	}
	if(!pre->old_id && now_id) {
		vsp_result_push(s, actor, pre, rpc, VSP_ACT_MOUNT, VSP_ST_APPLIED, "", 0);
		return;
	}
	/* old -> new: server_local_release_actor_mounts ran before the new
	 * reciprocal relation was written and emitted the release witness (with
	 * its serial) at that moment. The switch only REFERENCES it; a missing
	 * (0) or repeated witness is published as-is so the gate fails closed --
	 * never patched up here. The mount serial is allocated now, after. */
	uint64_t release_serial = release_count == 1 ? release_witness : 0;
	uint64_t mount_serial = vsp_next_serial();
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_SWITCH, VSP_PRODUCTION);
	struct vsp_p_switch* p = &r.u.sw;
	p->old = vsp_v_srv(s, pre->old_id);
	p->nu = vsp_v_srv(s, now_id);
	p->from = p->old.target.kind;
	p->to = p->nu.target.kind;
	p->host = vsp_s_srv(s, actor);
	p->release_serial = release_serial;
	p->mount_serial = mount_serial;
	(void)vsp_srv_push(&r);
	vsp_result_push(s, actor, pre, rpc, VSP_ACT_SWITCH, VSP_ST_APPLIED, "",
					mount_serial);
}

void vsp_srv_pause_toggled(struct server_local* s) {
	if(!s)
		return;
	vsp_progress_push(s, s->paused ? VSP_EV_PAUSE : VSP_EV_RESUME,
					  VSP_PRODUCTION, 0, 0);
	if(!s->paused)
		S.retained_pause_sent = false;
}

void vsp_srv_input_ack(struct server_local* s, uint32_t epoch,
					   bool host_presentation_cleared) {
	if(!s)
		return;
	/* The transfer's ACK is the accepted INPUT_READY for EXACTLY the
	 * commit's transition epoch. Whether the host presented through the
	 * two-world path (a2w_host_presentation: only with an admitted, confirmed
	 * guest in gameplay at publish time) is a presentation mode, not the
	 * correlation -- a guest mid-readmission legitimately leaves it false.
	 * Measured on the PC end-to-end run: commit at input epoch 2, transfer
	 * handoff ACK at epoch 3, transition_epoch 1 (O02-result.md). */
	(void)host_presentation_cleared;
	if(S.transfer_ack_pending && epoch != 0
	   && (int32_t)(epoch - S.transfer_ack_floor) > 0) {
		S.transfer_ack_pending = false;
		struct vsp_record r;
		vsp_rec_init(&r, VSP_EV_TRANSFER_ACK, VSP_PRODUCTION);
		struct vsp_p_transfer_ack* p = &r.u.tack;
		p->stage = VSP_STAGE_SERVER;
		p->transfer_id = S.transfer_id;
		p->actor = vsp_a_srv(s, &s->player);
		p->dst = vsp_w_of(server_local_world_of(s, &s->player));
		p->input_ack_epoch = epoch;
		p->ready = s->player.finished_loading && !s->input_ack_pending;
		uint32_t src_vehicle = s_fixture_targets[s_cfg.kind].id;
		p->u.source_vehicle = vsp_v_srv(s, src_vehicle);
		p->destination_mount_count = vsp_srv_mounts(s, &s->player, NULL);
		(void)vsp_srv_push(&r);
	}
	if(S.respawn_ack_pending) {
		S.respawn_ack_pending = false;
		struct vsp_record r;
		vsp_rec_init(&r, VSP_EV_RESPAWN, VSP_PRODUCTION);
		struct vsp_p_respawn* p = &r.u.respawn;
		p->stage = VSP_RS_ACK;
		p->old_input_epoch = S.respawn_old_epoch;
		p->new_input_epoch = epoch;
		p->u.server.host = vsp_s_srv(s, &s->player);
		p->u.server.old = vsp_v_srv(s, s_fixture_targets[s_cfg.kind].id);
		p->u.server.serial = vsp_next_serial();
		p->u.server.health = s->player.health;
		p->u.server.ack = true;
		(void)vsp_srv_push(&r);
	}
}

void vsp_srv_lethal_begin(bool on) {
	S.lethal_active = on;
}

void vsp_srv_health_committed(struct server_local* s, short old_health,
							  uint32_t cause) {
	if(!s || old_health <= 0 || s->player.health > 0)
		return;
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_DEATH,
				 S.lethal_active ? VSP_FIXTURE : VSP_PRODUCTION);
	struct vsp_p_death* p = &r.u.death;
	p->stage = VSP_DEATH_SERVER;
	p->u.server.host = vsp_s_srv(s, &s->player);
	p->u.server.before_host = p->u.server.host;
	p->u.server.before_host.health = old_health;
	uint32_t vid = vsp_srv_vehicle_of(s, &s->player);
	p->u.server.vehicle
		= vsp_v_srv(s, vid ? vid : s_fixture_targets[s_cfg.kind].id);
	uint32_t after = __atomic_load_n(&s->player_deaths, __ATOMIC_ACQUIRE);
	p->u.server.deaths_after = after;
	p->u.server.deaths_before = after ? after - 1u : 0u;
	snprintf(p->u.server.cause, sizeof(p->u.server.cause), "%s",
			 S.lethal_active ? "fixture_lethal"
							 : server_player_damage_cause_name(cause));
	/* Q4: every respawn stage carries the host's input epoch AT DEATH as
	 * its old epoch, read here on the owner thread at the commit. */
	S.respawn_old_epoch = vsp_srv_actor_epoch(s, &s->player);
	(void)vsp_srv_push(&r);
}

void vsp_srv_respawn_stage(struct server_local* s, enum vsp_stage_respawn st) {
	if(!s)
		return;
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_RESPAWN, VSP_PRODUCTION);
	struct vsp_p_respawn* p = &r.u.respawn;
	p->stage = (uint8_t)st;
	p->new_input_epoch = vsp_srv_actor_epoch(s, &s->player);
	/* Q4: RELEASED precedes the transaction (0); REQUESTED is the accepted
	 * LOADING entry and FRESH the committed body, both inside the epoch the
	 * production request allocated. The legacy whole-world path has none. */
	p->respawn_epoch = st == VSP_RS_RELEASED ? 0u : s->host_respawn_epoch;
	p->u.server.host = vsp_s_srv(s, &s->player);
	p->u.server.old = vsp_v_srv(s, s_fixture_targets[s_cfg.kind].id);
	p->u.server.serial = vsp_next_serial();
	p->u.server.health = s->player.health;
	p->u.server.ack = false;
	if(st == VSP_RS_FRESH && s->input_ack_pending) {
		/* A legacy (whole-world) respawn owns a fresh input epoch; its
		 * matching non-retry INPUT_READY is the ACK. */
		S.respawn_ack_pending = true;
	}
	p->old_input_epoch = S.respawn_old_epoch;
	(void)vsp_srv_push(&r);
}

/* The split-owned host respawn's readiness ACK is SRPC_RESPAWN carrying the
 * transaction's own epoch (server_local_host_respawn_request, LOADING): the
 * real producer of the loading handshake, not SRPC_INPUT_READY. Called only
 * on the MATCHING epoch; a stale or zero epoch never reaches here. */
void vsp_srv_respawn_ack(struct server_local* s, uint64_t epoch) {
	if(!s || !epoch || epoch != s->host_respawn_epoch)
		return;
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_RESPAWN, VSP_PRODUCTION);
	struct vsp_p_respawn* p = &r.u.respawn;
	p->stage = VSP_RS_ACK;
	p->old_input_epoch = S.respawn_old_epoch;
	p->new_input_epoch = vsp_srv_actor_epoch(s, &s->player);
	p->respawn_epoch = epoch;
	p->u.server.host = vsp_s_srv(s, &s->player);
	p->u.server.old = vsp_v_srv(s, s_fixture_targets[s_cfg.kind].id);
	p->u.server.serial = vsp_next_serial();
	p->u.server.health = s->player.health;
	p->u.server.ack = true;
	(void)vsp_srv_push(&r);
}

void vsp_srv_checkpoint_stage(int stage_index, bool ok) {
	if(stage_index >= 0 && stage_index < 3)
		S.ck_stage_ok[stage_index] = ok;
}

void vsp_srv_checkpoint(struct server_local* s, enum vsp_stage_ck stage,
						bool ok) {
	if(!s)
		return;
	if(stage == VSP_CK_BEGIN) {
		memset(S.ck_stage_ok, 0, sizeof(S.ck_stage_ok));
		S.checkpoint_id++;
	}
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_CHECKPOINT, VSP_PRODUCTION);
	struct vsp_p_checkpoint* p = &r.u.ck;
	p->stage = (uint8_t)stage;
	p->checkpoint_id = S.checkpoint_id;
	p->host = vsp_s_srv(s, &s->player);
	p->guest = vsp_s_srv(s, s->guest_actor);
	uint32_t vid = vsp_srv_vehicle_of(s, &s->player);
	p->vehicle = vsp_v_srv(s, vid ? vid : s_fixture_targets[s_cfg.kind].id);
	float pose[3] = {(float)s->player.x, (float)s->player.y,
					 (float)s->player.z};
	memcpy(p->pose_bits, pose, sizeof(p->pose_bits));
	p->fixture_hash = vsp_live_fixture_hash(s);
	if(stage == VSP_CK_DURABLE) {
		p->level_ok = S.ck_stage_ok[0];
		p->chunks_ok = S.ck_stage_ok[1];
		p->regions_synced = S.ck_stage_ok[2];
		p->save_ok = ok && p->level_ok && p->chunks_ok && p->regions_synced;
	}
	(void)vsp_srv_push(&r);
}

uint32_t vsp_checkpoint_id(void) {
	return S.checkpoint_id;
}

void vsp_srv_world_teardown(struct server_local* s) {
	(void)s;
	/* Every world teardown resets ALL mutation hooks; captured records and
	 * run counters survive. Fixture ids name the old world only. */
	vsp_hooks_reset();
	S.refusal_armed = false;
	S.refusal_wanted = false;
	S.ride_open = false;
	S.reload_disk_done = S.reload_inv_done = S.reload_entities_done = false;
}

void vsp_srv_reload_disk(struct server_local* s, const float pos[3],
						 bool player_read_ok, bool fallback_used,
						 bool override_used) {
	uint32_t ck = LOAD(s_expect_reload);
	if(!s || !ck || S.reload_disk_done)
		return;
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_RELOAD, VSP_PRODUCTION);
	struct vsp_p_reload* p = &r.u.reload;
	p->stage = VSP_RL_DISK;
	p->checkpoint_id = ck;
	p->u.disk.world = vsp_w_of(&s->worlds[0]);
	memcpy(p->u.disk.pose_bits, pos, sizeof(p->u.disk.pose_bits));
	p->u.disk.disk_reopen = true;
	p->u.disk.player_read_ok = player_read_ok;
	p->u.disk.fallback_used = fallback_used;
	p->u.disk.override_used = override_used;
	if(vsp_srv_push(&r))
		S.reload_disk_done = true;
}

void vsp_srv_reload_inventory(struct server_local* s, bool read_ok) {
	uint32_t ck = LOAD(s_expect_reload);
	if(!s || !ck || !S.reload_disk_done || S.reload_inv_done)
		return;
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_RELOAD, VSP_PRODUCTION);
	struct vsp_p_reload* p = &r.u.reload;
	p->stage = VSP_RL_INVENTORY;
	p->checkpoint_id = ck;
	p->u.inventory.world = vsp_w_of(&s->worlds[0]);
	p->u.inventory.read_ok = read_ok;
	p->u.inventory.inv_hash = vsp_items_hash(
		s->player.inventory.items, INVENTORY_SIZE, &p->u.inventory.inv_used);
	if(vsp_srv_push(&r))
		S.reload_inv_done = true;
}

void vsp_srv_guest_accept(struct server_local* s) {
	if(!s || !s->guest_accept_publish.pending)
		return;
	struct vsp_q q = vsp_q_srv(s);
	if(S.q_valid && vsp_q_equal(&q, &S.q))
		return; /* a re-sent ACCEPT is the same tuple */
	uint32_t op = __atomic_exchange_n(&s_cmd_refusal, 0u, __ATOMIC_ACQ_REL);
	if(!op || !(LOAD(s_hook_mask) & VSP_HOOK_REFUSAL))
		return; /* only the profile-requested ACCEPT is observed/refused */
	S.q = q;
	S.q_valid = true;
	S.refusal_op = op;
	S.refusal_armed = true;
	S.refusal_cleared = S.barrier_released = false;
	S.retained_pause_sent = S.retained_transfer_sent = S.blocked_sent = false;
	S.installed_count = 0;
	vsp_progress_push(s, VSP_EV_PENDING, VSP_SYNTHETIC, VSP_PD_ARMED,
					  VSP_CAUSE_SYNTHETIC_ADMISSION);
}

bool vsp_srv_refuse_publish(struct server_local* s) {
	if(!s || !S.refusal_armed || !(LOAD(s_hook_mask) & VSP_HOOK_REFUSAL))
		return false;
	struct vsp_q now = vsp_q_srv(s);
	if(!S.q_valid || !vsp_q_equal(&now, &S.q)) {
		/* tuple invalidated: cancel, never affect another tuple */
		S.refusal_armed = false;
		vsp_hook_set(VSP_HOOK_REFUSAL, false);
		return false;
	}
	if(!S.blocked_sent) {
		S.blocked_sent = true;
		vsp_progress_push(s, VSP_EV_PENDING, VSP_SYNTHETIC, VSP_PD_BLOCKED,
						  VSP_CAUSE_SYNTHETIC_ADMISSION);
	}
	return true;
}

void vsp_srv_publish_blocked(struct server_local* s, enum vsp_cause cause) {
	if(!s || !S.q_valid || cause != VSP_CAUSE_TRANSFER)
		return;
	/* The natural barrier, observed only after the synthetic refusal was
	 * cleared (CONTRACT-v1: later production retained event has
	 * refusal=false and transfer_open=true). */
	if(!S.refusal_cleared || S.retained_transfer_sent)
		return;
	struct vsp_q now = vsp_q_srv(s);
	if(!vsp_q_equal(&now, &S.q))
		return;
	S.retained_transfer_sent = true;
	vsp_progress_push(s, VSP_EV_PENDING, VSP_PRODUCTION, VSP_PD_RETAINED,
					  VSP_CAUSE_TRANSFER);
}

void vsp_srv_installed(struct server_local* s) {
	if(!s || !S.q_valid)
		return;
	S.installed_count++;
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_INSTALLED, VSP_PRODUCTION);
	r.op_id = S.refusal_op;
	struct vsp_p_progress* p = &r.u.progress;
	p->tuple = S.q;
	p->serial = vsp_next_serial();
	p->installed_count = S.installed_count;
	p->result = 1;
	p->pending = false;
	p->paused = s->paused;
	p->loading = !s->player.finished_loading || s->input_ack_pending;
	p->transfer_open = server_transfer_active(s);
	(void)vsp_srv_push(&r);
}

void vsp_srv_guest_ready(struct server_local* s) {
	if(!s || !S.q_valid || S.ready_count)
		return;
	S.ready_count++;
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_GAMEPLAY_READY, VSP_PRODUCTION);
	r.op_id = S.refusal_op;
	struct vsp_p_gameplay_ready* p = &r.u.ready;
	p->stage = VSP_STAGE_SERVER;
	p->tuple = S.q;
	p->u.server_guest = vsp_s_srv(s, s->guest_actor);
	p->confirmed = s->guest_confirmed;
	p->ready_count = S.ready_count;
	p->installed_count = S.installed_count;
	(void)vsp_srv_push(&r);
}

uint32_t vsp_srv_installed_count(void) {
	return S.installed_count;
}

static struct vsp_a vsp_txn_actor(struct server_local* s,
								  struct server_player* actor) {
	return vsp_a_srv(s, actor);
}

void vsp_srv_transfer_requested(struct server_local* s,
								struct server_player* actor, uint32_t id,
								bool fixture) {
	if(!s || !actor || actor != &s->player)
		return;
	const struct server_transfer_txn* t = &s->transfer[ACTOR_SLOT_HOST];
	S.transfer_id = id;
	S.transfer_src_slot = t->src_slot;
	S.transfer_dst_slot = t->dst_slot;
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_TRANSFER_BEGIN,
				 fixture ? VSP_FIXTURE : VSP_PRODUCTION);
	struct vsp_p_transfer* p = &r.u.transfer;
	p->transfer_id = id;
	p->actor = vsp_txn_actor(s, actor);
	p->src = vsp_w_of(&s->worlds[t->src_slot]);
	p->dst = vsp_w_of(&s->worlds[t->dst_slot]);
	uint32_t vid = vsp_srv_vehicle_of(s, actor);
	p->vehicle = vsp_v_srv(s, vid ? vid : s_fixture_targets[s_cfg.kind].id);
	p->mode = fixture ? 1 : 0;
	p->serial = vsp_next_serial();
	(void)vsp_srv_push(&r);
}

void vsp_srv_transfer_released(struct server_local* s,
							   struct server_player* actor, uint32_t id,
							   uint8_t src_slot, uint8_t dst_slot) {
	if(!s || actor != &s->player)
		return;
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_TRANSFER_RELEASE, VSP_PRODUCTION);
	struct vsp_p_transfer* p = &r.u.transfer;
	p->transfer_id = id;
	p->actor = vsp_txn_actor(s, actor);
	p->src = vsp_w_of(&s->worlds[src_slot]);
	p->dst = vsp_w_of(&s->worlds[dst_slot]);
	p->host = vsp_s_srv(s, actor);
	p->vehicle = vsp_v_srv(s, s_fixture_targets[s_cfg.kind].id);
	p->serial = vsp_next_serial();
	(void)vsp_srv_push(&r);
}

void vsp_srv_transfer_committed(struct server_local* s,
								struct server_player* actor, uint32_t id,
								uint8_t src_slot, uint8_t dst_slot,
								uint32_t transition_epoch) {
	if(!s || actor != &s->player)
		return;
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_TRANSFER_COMMIT, VSP_PRODUCTION);
	struct vsp_p_transfer* p = &r.u.transfer;
	p->transfer_id = id;
	p->actor = vsp_txn_actor(s, actor);
	p->src = vsp_w_of(&s->worlds[src_slot]);
	p->dst = vsp_w_of(&s->worlds[dst_slot]);
	p->host = vsp_s_srv(s, actor);
	p->vehicle = vsp_v_srv(s, s_fixture_targets[s_cfg.kind].id);
	p->destination_mount_count = vsp_srv_mounts(s, actor, NULL);
	p->serial = vsp_next_serial();
	p->transition_epoch = transition_epoch;
	S.transfer_ack_pending = true;
	/* The commit precedes the host's input handoff for its new world: the
	 * transfer's ACK is the first accepted INPUT_READY for an epoch the
	 * server allocated AFTER this point (transition_epoch is the transfer
	 * subsystem's own per-transaction counter, not an input epoch). */
	S.transfer_ack_floor = s->input_epoch_counter;
	(void)vsp_srv_push(&r);
}

/* ---- main/client producers ---------------------------------------------- */
static bool vsp_rpc_is_vehicle(uint8_t type) {
	switch(type) {
		case CRPC_SPAWN_BOAT:
		case CRPC_BOAT_CORRECTION:
		case CRPC_MINECART_OCCUPANCY:
		case CRPC_SPAWN_MINECART:
		case CRPC_MINECART_SNAPSHOT:
		case CRPC_SPAWN_MONSTER:
		case CRPC_ENTITY_MOVE:
		case CRPC_ATTACH_ENTITY:
		case CRPC_PLAYER_POS:
			return true;
		default:
			return false;
	}
}

static struct dict_entity_s* vsp_view_entities(unsigned view) {
	if(clin_world_payloads_enabled()) {
		struct clin_world_payload payload;
		if(!clin_world_payload_of_view(view, &payload))
			return NULL;
		return payload.entities;
	}
	return world_view_entities();
}

/* Native relations naming `body` in a view's payload (bit0 boat, bit1 pig,
 * bit2 cart). */
static uint32_t vsp_client_relations(unsigned view, uint32_t body_id,
									 uint32_t* count) {
	uint32_t mask = 0, n = 0;
	struct dict_entity_s* ents = vsp_view_entities(view);
	if(ents) {
		dict_entity_it_t it;
		dict_entity_it(it, ents);
		while(!dict_entity_end_p(it)) {
			const struct entity* e = dict_entity_ref(it)->value;
			dict_entity_next(it);
			if(!e)
				continue;
			if(e->type == ENTITY_BOAT && e->data.boat.occupied
			   && e->data.boat.occupant_id == body_id) {
				mask |= 1u;
				n++;
			} else if(e->type == ENTITY_MONSTER
					  && e->data.monster.id == MONSTER_PIG
					  && e->data.monster.pig_ridden
					  && e->data.monster.pig_rider_id == body_id) {
				mask |= 2u;
				n++;
			} else if(e->type == ENTITY_MINECART && e->data.minecart.occupied
					  && e->data.minecart.occupant_id == body_id) {
				mask |= 4u;
				n++;
			}
		}
	}
	if(count)
		*count = n;
	return mask;
}

void vsp_client_witness_begin(const struct client_rpc* call,
							  struct vsp_cw_pre* pre) {
	memset(pre, 0, sizeof(*pre));
	if(!call || !vsp_rpc_is_vehicle(call->type))
		return;
	const struct client_session* g = &gstate.players[1];
	if(!g->local_player)
		return;
	pre->valid = true;
	pre->guest_rel = vsp_client_relations(1, g->local_player->id, NULL);
	memcpy(pre->guest_pos, g->local_player->pos, sizeof(pre->guest_pos));
	pre->guest_pred = g->ride.pending && g->ride.desired_occupied;
}

void vsp_client_witness_end(const struct client_rpc* call,
							const struct vsp_cw_pre* pre) {
	if(!call || !pre || !pre->valid)
		return;
	const struct client_session* g = &gstate.players[1];
	if(!g->local_player)
		return;
	uint32_t rel = vsp_client_relations(1, g->local_player->id, NULL);
	uint32_t gained = rel & ~pre->guest_rel;
	bool pred = g->ride.pending && g->ride.desired_occupied;
	bool moved = memcmp(pre->guest_pos, g->local_player->pos,
						sizeof(pre->guest_pos)) != 0;
	/* An untagged CRPC_PLAYER_POS is the HOST's correction route; applying it
	 * to the guest body is a misroute. Every other vehicle RPC counts only
	 * when its application attached the guest (one per applied RPC). */
	bool host_pos = call->type == CRPC_PLAYER_POS
		&& call->payload.player_pos.input_epoch == 0;
	bool misapplied = gained || (pred && !pre->guest_pred)
		|| (host_pos && moved);
	if(!misapplied)
		return;
	C.unexpected_route_count++;
	for(unsigned b = 0; b < 3; b++)
		if(gained & (1u << b))
			C.unexpected_attach_count++;
	if(pred && !pre->guest_pred)
		C.unexpected_attach_count++;
	C.route_violation_sticky = true;
}

/* Q5, client (main/owner) thread: the host's acknowledged input epoch is
 * the client interface's last accepted handoff; the guest view has no
 * input-epoch protocol and reports 0. */
uint32_t vsp_client_actor_epoch(unsigned view) {
	return view == 0 ? clin_input_epoch() : 0u;
}

void vsp_client_follow(const struct client_session* session,
					   const struct entity* body, const struct entity* vehicle,
					   float seat_error) {
	if(!session || !body)
		return;
	if(session == &gstate.players[1]) {
		C.guest_follow_count++;
		C.route_violation_sticky = true;
		return;
	}
	if(vehicle) {
		C.host_follow_target = vehicle->id;
		if(vehicle->id == s_client_ride_target) {
			C.host_follow_count++;
			if(seat_error > C.host_seat_error_max)
				C.host_seat_error_max = seat_error;
		}
	}
}

void vsp_client_counters(struct vsp_client_counters* out) {
	if(out)
		*out = C;
}

void vsp_client_counters_window_reset(bool iso, bool ride,
									  uint32_t ride_target) {
	if(iso) {
		C.guest_follow_count = 0;
		C.unexpected_route_count = 0;
		C.unexpected_attach_count = 0;
	}
	if(ride) {
		C.host_follow_count = 0;
		C.host_seat_error_max = 0;
		C.ride_eligible = C.ride_tick_calls = 0;
		C.ride_missing = C.ride_duplicate = C.ride_calls_max = 0;
		C.ride_disp_max = 0;
		memset(C.ride_start, 0, sizeof(C.ride_start));
		C.ride_started = false;
		s_client_ride_target = ride_target;
	}
}

/* ---- Q3: actual client vehicle dispatch --------------------------------- */

static bool vsp_dispatch_tramp(struct entity* e) {
	struct vsp_dispatch_tap* t = s_tap;
	/* Only the tapped target ever carries this pointer, and only between
	 * begin and end; the original is called exactly as the dispatcher would
	 * have called it. */
	if(t && e == t->e) {
		t->calls++;
		return t->orig(e);
	}
	return false;
}

/* body_id 0 is valid: the host's local player is entity 0 (the reserved
 * local-player id); "no body" is excluded by the caller (no host view). */
static bool vsp_rides_target(const struct entity* v, uint32_t body_id) {
	if(!v)
		return false;
	switch(v->type) {
		case ENTITY_BOAT:
			return v->data.boat.occupied && v->data.boat.occupant_id == body_id;
		case ENTITY_MINECART:
			return v->data.minecart.occupied
				&& v->data.minecart.occupant_id == body_id;
		case ENTITY_MONSTER:
			return v->data.monster.id == MONSTER_PIG
				&& v->data.monster.pig_ridden
				&& v->data.monster.pig_rider_id == body_id;
		default:
			return false;
	}
}

void vsp_client_dispatch_begin_for(struct vsp_dispatch_tap* t,
								   struct dict_entity_s* ents,
								   struct dict_entity_s* host_view,
								   uint32_t host_body_id) {
	vsp_client_dispatch_begin_slot(t, ents, host_view, host_body_id, -1);
}

/* `slot`: the client world slot `ents` belongs to (-1 unknown). A detached
 * census only counts the store of the target's OWN world: entity ids are
 * per-world, so the same id in another store is a different entity. */
void vsp_client_dispatch_begin_slot(struct vsp_dispatch_tap* t,
									struct dict_entity_s* ents,
									struct dict_entity_s* host_view,
									uint32_t host_body_id, int slot) {
	memset(t, 0, sizeof(*t));
	uint32_t target = s_client_ride_target;
	if(!target || !ents || s_tap || (!C.ride_detached && ents != host_view))
		return;
	if(C.ride_detached && (s_client_ride_slot < 0 || slot != s_client_ride_slot))
		return;
	struct entity** pp = dict_entity_get(ents, target);
	if(!pp || !*pp)
		return;
	/* Detached census: any view whose store holds the released target is
	 * eligible while the host body is NOT its rider; mounted census: the
	 * host's own view, target natively ridden by the host body. */
	if(C.ride_detached ? vsp_rides_target(*pp, host_body_id)
					   : !vsp_rides_target(*pp, host_body_id))
		return;
	t->eligible = true;
	t->id = target;
	t->e = *pp;
	if(t->e->tick_client) {
		t->orig = t->e->tick_client;
		t->e->tick_client = vsp_dispatch_tramp;
		t->tapped = true;
	}
	s_tap = t;
}

void vsp_client_dispatch_begin(struct vsp_dispatch_tap* t,
							   struct dict_entity_s* ents, int slot) {
	/* The host's view is the dict that holds the host body itself (the
	 * local player is ticked by this same dispatcher -- main.c nulls its
	 * tick_client around the call when held). A pointer comparison against
	 * the payload accessor is not stable across the payload/slot paths. */
	const struct entity* body = gstate.players[0].local_player;
	struct entity** bp = body && ents ? dict_entity_get(ents, body->id) : NULL;
	vsp_client_dispatch_begin_slot(t, ents, bp && *bp == body ? ents : NULL,
								   body ? body->id : 0, slot);
}

void vsp_client_dispatch_end(struct vsp_dispatch_tap* t,
							 struct dict_entity_s* ents) {
	if(!t || !t->eligible)
		return;
	s_tap = NULL;
	struct entity** pp = ents ? dict_entity_get(ents, t->id) : NULL;
	bool alive = pp && *pp == t->e;
	if(t->tapped && alive && t->e->tick_client == vsp_dispatch_tramp)
		t->e->tick_client = t->orig;
	C.ride_eligible++;
	C.ride_tick_calls += t->calls;
	if(t->calls == 0)
		C.ride_missing++;
	else if(t->calls > 1)
		C.ride_duplicate++;
	if(t->calls > C.ride_calls_max)
		C.ride_calls_max = t->calls;
	if(alive) {
		if(!C.ride_started) {
			memcpy(C.ride_start, t->e->pos, sizeof(C.ride_start));
			C.ride_started = true;
		}
		float dx = t->e->pos[0] - C.ride_start[0];
		float dz = t->e->pos[2] - C.ride_start[2];
		float d = sqrtf(dx * dx + dz * dz);
		if(d > C.ride_disp_max)
			C.ride_disp_max = d;
	}
}

void vsp_ingress_refused(const struct server_rpc* call, uint8_t slot,
						 uint64_t generation, bool admitted) {
	if(!call || slot != 1)
		return;
	enum vsp_rpc rpc;
	enum vsp_action action;
	uint32_t target = 0, request_seq = 0;
	uint8_t kind = VSP_CART;
	if(call->type == SRPC_ENTITY_USE || call->type == SRPC_ENTITY_ATTACK) {
		rpc = call->type == SRPC_ENTITY_USE ? VSP_RPC_ENTITY_USE
											: VSP_RPC_ENTITY_ATTACK;
		/* the delivering channel (frame press vs owned RPC) is the op's */
		action = (enum vsp_action)vsp_op_action_or(VSP_ACT_GUEST_USE);
		target = call->type == SRPC_ENTITY_USE
			? call->payload.entity_use.entity_id
			: call->payload.entity_attack.entity_id;
		kind = VSP_KIND_COUNT;
		for(unsigned k = 0; k < 3; k++)
			if(s_fixture_targets[k].id == target)
				kind = (uint8_t)k;
		if(kind == VSP_KIND_COUNT)
			return; /* not a vehicle-smoke target: not this probe's row */
	} else if(call->type == SRPC_MINECART_RIDE_INTENT) {
		rpc = VSP_RPC_MINECART_RIDE_INTENT;
		action = VSP_ACT_GUEST_CART_INTENT;
		target = call->payload.minecart_ride_intent.entity_id;
		request_seq = call->payload.minecart_ride_intent.request_seq;
	} else if(call->type == SRPC_MINECART_CONTROL) {
		rpc = VSP_RPC_MINECART_CONTROL;
		action = VSP_ACT_GUEST_CART_CONTROL;
		target = call->payload.minecart_control.entity_id;
	} else
		return;
	struct vsp_record r;
	vsp_rec_init(&r, VSP_EV_ACTION_RESULT, VSP_PRODUCTION);
	snprintf(r.reason, sizeof(r.reason), "slot1_mailbox_closed");
	struct vsp_p_action_result* p = &r.u.result;
	p->action = (uint8_t)action;
	p->source.slot = slot;
	p->source.generation = generation;
	p->source.wire_id = gstate.players[1].local_player
		? gstate.players[1].local_player->id : 0;
	p->source.input_epoch = vsp_client_actor_epoch(slot);
	p->request_seq = request_seq;
	p->target.id = target;
	p->target.kind = kind;
	/* The target belongs to the published fixture world, which need not be
	 * the guest's bound view (or even have a bound guest context yet). A
	 * refused ingress must retain the real target identity for the wire. */
	for(unsigned k = 0; k < 3; k++)
		if(s_fixture_targets[k].id == target)
			p->target.world = s_fixture_targets[k].world;
	p->site = VSP_SITE_INGRESS;
	p->status = admitted ? VSP_ST_SENT : VSP_ST_REFUSED;
	if(admitted)
		r.reason[0] = '\0';
	p->rpc = (uint8_t)rpc;
	(void)vsp_main_push(&r);
}

uint32_t vsp_client_inventory_hash(const struct client_session* session,
								   uint32_t* used) {
	struct item_data items[INVENTORY_SIZE];
	memset(items, 0, sizeof(items));
	struct inventory* inv = session && session->windows[WINDOWC_INVENTORY]
		? windowc_get_latest(session->windows[WINDOWC_INVENTORY])
		: NULL;
	for(size_t k = 0; inv && k < INVENTORY_SIZE; k++)
		(void)inventory_get_slot(inv, k, &items[k]);
	if(!inv) {
		/* never hash absent inventory as a successful empty baseline */
		if(used)
			*used = 0;
		return 0;
	}
	return vsp_items_hash(items, INVENTORY_SIZE, used);
}

static bool vsp_client_bound(unsigned view, struct vsp_w* out) {
	memset(out, 0, sizeof(*out));
	int vslot = clin_world_view_slot(view);
	if(vslot < 0)
		return false;
	const struct client_world_context* ctx
		= client_ctx_registry_current(clin_world_contexts(), (unsigned)vslot);
	if(!ctx || !ctx->server_generation)
		return false;
	out->slot = (uint8_t)vslot;
	out->generation = ctx->server_generation;
	out->dimension = ctx->dimension;
	return true;
}

bool vsp_snap_client(const struct client_session* session, unsigned view,
					 struct vsp_c* out) {
	memset(out, 0, sizeof(*out));
	if(!session || !session->local_player || view > 1)
		return false;
	if(!vsp_client_bound(view, &out->bound))
		return false;
	out->present = true;
	out->actor.slot = (uint8_t)view;
	out->actor.generation = view ? session->local_generation : 0;
	out->actor.wire_id = session->local_player->id;
	out->actor.input_epoch = vsp_client_actor_epoch(view);
	memcpy(out->pos, session->local_player->pos, sizeof(out->pos));
	out->steer[0] = session->local_player->data.local_player.boat_rider_vel_x;
	out->steer[1] = session->local_player->data.local_player.boat_rider_vel_z;
	out->ready = view ? client_session_local_gameplay_active(session)
					  : client_session_host_gameplay_active(session);
	out->mount_count = 0;
	(void)vsp_client_relations(view, session->local_player->id,
							   &out->mount_count);
	out->ride_pending = session->ride.pending;
	out->ride_target = session->ride.target_id;
	out->ride_input_epoch = session->ride.input_epoch;
	out->ride_request_seq = session->ride.pending_request_seq;
	out->ride_desired_occupied = session->ride.desired_occupied;
	out->ride_send_dirty = session->ride.send_dirty;
	out->ride_exit_lift_pending = session->ride.exit_lift_pending;
	out->ride_exit_repin_eligible = session->ride.exit_repin_eligible;
	out->inv_hash = vsp_client_inventory_hash(session, &out->inv_used);
	return true;
}

void vsp_snap_client_absent(const struct client_session* session,
							struct vsp_c* out) {
	memset(out, 0, sizeof(*out));
	out->present = false;
	if(session) {
		out->c0_generation = session->local_generation;
		out->c0_ticket = session->local_ticket;
	}
}

static bool vsp_snap_client_vehicle_in(struct dict_entity_s* ents,
										uint32_t id, struct vsp_vc* out);

bool vsp_client_entity_box(unsigned view, uint32_t id, float out[6]) {
	struct dict_entity_s* ents = vsp_view_entities(view);
	struct entity** ep = ents ? dict_entity_get(ents, id) : NULL;
	struct entity* e = ep ? *ep : NULL;
	struct AABB box;
	if(!e || !e->getBoundingBox || e->getBoundingBox(e, &box) == 0)
		return false;
	out[0] = box.x1;
	out[1] = box.y1;
	out[2] = box.z1;
	out[3] = box.x2;
	out[4] = box.y2;
	out[5] = box.z2;
	return true;
}

bool vsp_snap_client_vehicle(unsigned view, uint32_t id, uint8_t kind,
							 struct vsp_vc* out) {
	memset(out, 0, sizeof(*out));
	out->target.id = id;
	out->target.kind = kind;
	if(!vsp_client_bound(view, &out->target.world))
		return false;
	return vsp_snap_client_vehicle_in(vsp_view_entities(view), id, out);
}

/* The vehicle as the client store of ITS OWN world slot holds it, whichever
 * view (if any) presents that slot -- entity ids are per world. */
bool vsp_snap_client_vehicle_slot(unsigned slot, uint32_t id, uint8_t kind,
								  struct vsp_vc* out) {
	memset(out, 0, sizeof(*out));
	out->target.id = id;
	out->target.kind = kind;
	const struct client_world_context* ctx
		= client_ctx_registry_current(clin_world_contexts(), slot);
	if(!ctx || !ctx->server_generation)
		return false;
	out->target.world.slot = (uint8_t)slot;
	out->target.world.generation = ctx->server_generation;
	out->target.world.dimension = ctx->dimension;
	struct clin_world_payload payload;
	struct dict_entity_s* ents = clin_world_payload_of_slot(slot, &payload)
		? payload.entities : NULL;
	return vsp_snap_client_vehicle_in(ents, id, out);
}

static bool vsp_snap_client_vehicle_in(struct dict_entity_s* ents,
										uint32_t id, struct vsp_vc* out) {
	struct entity** pp = ents ? dict_entity_get(ents, id) : NULL;
	const struct entity* e = pp ? *pp : NULL;
	if(!e)
		return true; /* alive=false */
	out->alive = true;
	memcpy(out->pos, e->pos, sizeof(out->pos));
	if(e->type == ENTITY_BOAT) {
		out->target.kind = VSP_BOAT;
		out->occupied = e->data.boat.occupied;
		out->occupant_id = e->data.boat.occupant_id;
		out->steer[0] = e->data.boat.rider_vel_x;
		out->steer[1] = e->data.boat.rider_vel_z;
	} else if(e->type == ENTITY_MONSTER) {
		out->target.kind = VSP_PIG;
		out->pig_ridden = e->data.monster.pig_ridden;
		out->pig_rider_id = e->data.monster.pig_rider_id;
		out->saddled = e->data.monster.saddled;
	} else if(e->type == ENTITY_MINECART) {
		out->target.kind = VSP_CART;
		out->occupied = e->data.minecart.occupied;
		out->occupant_id = e->data.minecart.occupant_id;
		out->cart_type = (uint32_t)e->data.minecart.minecart_type;
		out->ride_epoch = e->data.minecart.ride_epoch;
		out->control_ready = e->data.minecart.control_ready;
		out->control_dirty = e->data.minecart.control_dirty;
		out->control_seq = e->data.minecart.control_seq;
		out->positive_steps = e->data.minecart.control_positive_steps;
		out->negative_steps = e->data.minecart.control_negative_steps;
		out->pending_throttle = e->data.minecart.pending_throttle;
		out->replay_throttle = e->data.minecart.replay_throttle;
	}
	return true;
}

/* ---- serializer --------------------------------------------------------- */
struct jw {
	char* buf;
	size_t cap, n;
	const char* err;
};

static void jw_raw(struct jw* w, const char* s) {
	size_t len = strlen(s);
	if(w->err)
		return;
	if(w->n + len >= w->cap) {
		w->err = "oversize";
		return;
	}
	memcpy(w->buf + w->n, s, len);
	w->n += len;
	w->buf[w->n] = 0;
}

static void jw_fmt(struct jw* w, const char* fmt, ...) {
	if(w->err)
		return;
	va_list ap;
	va_start(ap, fmt);
	int k = vsnprintf(w->buf + w->n, w->cap - w->n, fmt, ap);
	va_end(ap);
	if(k < 0 || (size_t)k >= w->cap - w->n) {
		w->err = "oversize";
		return;
	}
	w->n += (size_t)k;
}

static void jw_key(struct jw* w, const char* key) {
	if(w->n && w->buf[w->n - 1] != '{' && w->buf[w->n - 1] != '[')
		jw_raw(w, ",");
	jw_fmt(w, "\"%s\":", key);
}

static void jw_u32(struct jw* w, const char* k, uint32_t v) {
	jw_key(w, k);
	jw_fmt(w, "%lu", (unsigned long)v);
}
static void jw_i32(struct jw* w, const char* k, int32_t v) {
	jw_key(w, k);
	jw_fmt(w, "%ld", (long)v);
}
static void jw_bool(struct jw* w, const char* k, bool v) {
	jw_key(w, k);
	jw_raw(w, v ? "true" : "false");
}
static void jw_g64(struct jw* w, const char* k, uint64_t v) {
	jw_key(w, k);
	jw_fmt(w, "\"%llu\"", (unsigned long long)v);
}
static void jw_fval(struct jw* w, float v) {
	if(!isfinite(v)) {
		if(!w->err)
			w->err = "nonfinite";
		return;
	}
	jw_fmt(w, "%.9g", (double)v);
}
static void jw_f(struct jw* w, const char* k, float v) {
	jw_key(w, k);
	jw_fval(w, v);
}
static bool vsp_text_ok(const char* s, bool id) {
	size_t n = strlen(s);
	if(id ? (n < 1 || n > 63) : n > 48)
		return false;
	for(size_t i = 0; i < n; i++) {
		char c = s[i];
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			|| (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'
			|| (id ? c == '+' : c == ':');
		if(!ok)
			return false;
	}
	return true;
}
static void jw_str(struct jw* w, const char* k, const char* v) {
	jw_key(w, k);
	jw_fmt(w, "\"%s\"", v);
}
static void jw_text(struct jw* w, const char* k, const char* v) {
	if(!vsp_text_ok(v, false)) {
		if(!w->err)
			w->err = "bad_text";
		return;
	}
	jw_str(w, k, v);
}
static void jw_open(struct jw* w, const char* k) {
	jw_key(w, k);
	jw_raw(w, "{");
}
static void jw_close(struct jw* w) {
	jw_raw(w, "}");
}
static void jw_P(struct jw* w, const char* k, const float p[3]) {
	jw_key(w, k);
	jw_raw(w, "[");
	jw_fval(w, p[0]);
	jw_raw(w, ",");
	jw_fval(w, p[1]);
	jw_raw(w, ",");
	jw_fval(w, p[2]);
	jw_raw(w, "]");
}
static void jw_F2(struct jw* w, const char* k, const float p[2]) {
	jw_key(w, k);
	jw_raw(w, "[");
	jw_fval(w, p[0]);
	jw_raw(w, ",");
	jw_fval(w, p[1]);
	jw_raw(w, "]");
}
static void jw_U3(struct jw* w, const char* k, const uint32_t p[3]) {
	jw_key(w, k);
	jw_fmt(w, "[%lu,%lu,%lu]", (unsigned long)p[0], (unsigned long)p[1],
		   (unsigned long)p[2]);
}
static void jw_A(struct jw* w, const char* k, const struct vsp_a* a) {
	jw_open(w, k);
	jw_u32(w, "slot", a->slot);
	jw_g64(w, "generation", a->generation);
	jw_u32(w, "wire_id", a->wire_id);
	jw_u32(w, "input_epoch", a->input_epoch);
	jw_close(w);
}
static void jw_W(struct jw* w, const char* k, const struct vsp_w* v) {
	if(!v->generation && !w->err)
		w->err = "invalid_world"; /* absent world never as a fake generation */
	jw_open(w, k);
	jw_u32(w, "slot", v->slot);
	jw_u32(w, "generation", v->generation);
	jw_i32(w, "dimension", v->dimension);
	jw_close(w);
}
static void jw_R(struct jw* w, const char* k, const struct vsp_r* r) {
	if(!r->id && !w->err)
		w->err = "invalid_target";
	jw_open(w, k);
	jw_u32(w, "id", r->id);
	jw_str(w, "kind", vsp_kind_name((enum vsp_kind)r->kind));
	jw_W(w, "world", &r->world);
	jw_close(w);
}
static void jw_S(struct jw* w, const char* k, const struct vsp_s* s) {
	jw_open(w, k);
	jw_A(w, "actor", &s->actor);
	jw_W(w, "world", &s->world);
	jw_P(w, "pos", s->pos);
	jw_i32(w, "health", s->health);
	jw_bool(w, "ready", s->ready);
	jw_bool(w, "confirmed", s->confirmed);
	jw_u32(w, "mount_count", s->mount_count);
	jw_bool(w, "riding_boat", s->riding_boat);
	jw_bool(w, "riding_cart", s->riding_cart);
	jw_F2(w, "steer", s->steer);
	jw_u32(w, "inv_hash", s->inv_hash);
	jw_u32(w, "inv_used", s->inv_used);
	jw_close(w);
}
static void jw_V(struct jw* w, const char* k, const struct vsp_v* v) {
	jw_open(w, k);
	jw_R(w, "target", &v->target);
	jw_bool(w, "alive", v->alive);
	jw_P(w, "pos", v->pos);
	jw_bool(w, "occupied", v->occupied);
	jw_A(w, "rider", &v->rider);
	jw_u32(w, "ride_epoch", v->ride_epoch);
	jw_u32(w, "control_seq", v->control_seq);
	jw_u32(w, "positive_steps", v->positive_steps);
	jw_u32(w, "negative_steps", v->negative_steps);
	jw_i32(w, "pending_throttle", v->pending_throttle);
	jw_bool(w, "saddled", v->saddled);
	jw_F2(w, "steer", v->steer);
	jw_close(w);
}
static void jw_C(struct jw* w, const char* k, const struct vsp_c* c) {
	jw_open(w, k);
	if(!c->present) { /* C0: genuinely absent pending guest body only */
		jw_bool(w, "present", false);
		jw_g64(w, "generation", c->c0_generation);
		jw_g64(w, "pending_ticket", c->c0_ticket);
		jw_close(w);
		return;
	}
	jw_bool(w, "present", true);
	jw_A(w, "actor", &c->actor);
	jw_W(w, "bound", &c->bound);
	jw_P(w, "pos", c->pos);
	jw_bool(w, "ready", c->ready);
	jw_u32(w, "mount_count", c->mount_count);
	jw_bool(w, "ride_pending", c->ride_pending);
	jw_u32(w, "ride_target", c->ride_target);
	jw_u32(w, "ride_input_epoch", c->ride_input_epoch);
	jw_u32(w, "ride_request_seq", c->ride_request_seq);
	jw_bool(w, "ride_desired_occupied", c->ride_desired_occupied);
	jw_bool(w, "ride_send_dirty", c->ride_send_dirty);
	jw_bool(w, "ride_exit_lift_pending", c->ride_exit_lift_pending);
	jw_bool(w, "ride_exit_repin_eligible", c->ride_exit_repin_eligible);
	jw_F2(w, "steer", c->steer);
	jw_u32(w, "inv_hash", c->inv_hash);
	jw_u32(w, "inv_used", c->inv_used);
	jw_close(w);
}
static void jw_VC(struct jw* w, const char* k, const struct vsp_vc* v) {
	jw_open(w, k);
	jw_R(w, "target", &v->target);
	jw_bool(w, "alive", v->alive);
	jw_P(w, "pos", v->pos);
	switch(v->target.kind) {
		case VSP_BOAT:
			jw_bool(w, "occupied", v->occupied);
			jw_u32(w, "occupant_id", v->occupant_id);
			jw_F2(w, "steer", v->steer);
			break;
		case VSP_PIG:
			jw_bool(w, "pig_ridden", v->pig_ridden);
			jw_u32(w, "pig_rider_id", v->pig_rider_id);
			jw_bool(w, "saddled", v->saddled);
			break;
		default:
			jw_bool(w, "occupied", v->occupied);
			jw_u32(w, "occupant_id", v->occupant_id);
			jw_u32(w, "cart_type", v->cart_type);
			jw_u32(w, "ride_epoch", v->ride_epoch);
			jw_bool(w, "control_ready", v->control_ready);
			jw_bool(w, "control_dirty", v->control_dirty);
			jw_u32(w, "control_seq", v->control_seq);
			jw_u32(w, "positive_steps", v->positive_steps);
			jw_u32(w, "negative_steps", v->negative_steps);
			jw_i32(w, "pending_throttle", v->pending_throttle);
			jw_i32(w, "replay_throttle", v->replay_throttle);
			break;
	}
	jw_close(w);
}
static void jw_Q(struct jw* w, const char* k, const struct vsp_q* q) {
	jw_open(w, k);
	jw_g64(w, "ticket", q->ticket);
	jw_g64(w, "generation", q->generation);
	jw_W(w, "world", &q->world);
	jw_close(w);
}
static void jw_targets(struct jw* w, const char* k, const struct vsp_r t[3]) {
	jw_key(w, k);
	jw_raw(w, "[");
	for(unsigned i = 0; i < 3; i++) {
		if(i)
			jw_raw(w, ",");
		/* array elements: open without a key */
		jw_raw(w, "{");
		jw_u32(w, "id", t[i].id);
		jw_str(w, "kind", vsp_kind_name((enum vsp_kind)t[i].kind));
		jw_W(w, "world", &t[i].world);
		jw_raw(w, "}");
		if(!t[i].id && !w->err)
			w->err = "invalid_target";
	}
	jw_raw(w, "]");
}

static const char* const k_event_names[VSP_EV_COUNT] = {
	"run_start", "phase_begin", "phase_end", "rig_ready", "action_attempt",
	"ui_action", "action_result", "ride_sample", "dismount", "switch",
	"isolation", "checkpoint", "world_reset", "title", "reload", "death",
	"respawn", "transfer_begin", "transfer_release", "transfer_commit",
	"transfer_ack", "pending", "pause", "resume", "admission_clear",
	"barrier_release", "installed", "gameplay_ready", "run_ok", "fail",
};
static const char* const k_phase[] = {"p0", "p1", "p2", "p3", "p4",
									  "p5", "done", "failed"};
static const char* const k_origin[] = {"production", "fixture", "synthetic"};
static const char* const k_action[] = {
	"saddle", "mount", "dismount", "switch", "guest_ui", "guest_use",
	"guest_cart_intent", "guest_cart_control"};
static const char* const k_channel[] = {"host_buttons", "guest_frame",
										"owned_rpc"};
static const char* const k_button[] = {
	"none", "IB_ACTION1", "IB_ACTION2", "IB_JUMP", "IB_HOME", "IB_GUI_UP",
	"IB_GUI_DOWN", "IB_GUI_CLICK"};
static const char* const k_screen[] = {"none", "ingame", "pause",
									   "death", "title", "loading"};
static const char* const k_ui[] = {"save_exit", "respawn", "guest_leave",
								   "guest_join", "accept", "confirm",
								   "gameplay"};
static const char* const k_site[] = {"ingress", "authority", "client"};
static const char* const k_status[] = {"sent", "applied", "refused",
									   "client_noop"};
static const char* const k_rpc[] = {"ENTITY_ATTACK", "ENTITY_USE",
									"MINECART_RIDE_INTENT", "MINECART_CONTROL"};
static const char* const k_cause[] = {"synthetic_admission", "global_pause",
									  "transfer", "loading",
									  "status_backpressure"};
static const char* const k_sc[] = {"server", "client"};
static const char* const k_ba[] = {"before", "after"};
static const char* const k_ck[] = {"begin", "durable"};
static const char* const k_rl[] = {"disk", "inventory", "entities",
								   "client_ready"};
static const char* const k_death[] = {"server", "ui"};
static const char* const k_rs[] = {"requested", "released", "fresh", "ack",
								   "client_ready"};
static const char* const k_pd[] = {"armed", "blocked", "retained"};

#define NAME(tab, i) ((size_t)(i) < sizeof(tab) / sizeof(tab[0]) ? tab[i] : "?")

static void vsp_payload(struct jw* w, const struct vsp_record* r) {
	switch((enum vsp_event)r->event) {
		case VSP_EV_RUN_START:
			jw_str(w, "seed", "12345");
			jw_u32(w, "difficulty", 0);
			jw_str(w, "game_mode", "survival");
			jw_bool(w, "superflat", true);
			jw_bool(w, "split", true);
			jw_bool(w, "world_payloads", true);
			jw_str(w, "world", VSP_RIG_WORLD);
			jw_str(w, "transfer_mode",
				   r->u.run_start.transfer_mode ? "fixture_request" : "contact");
			jw_u32(w, "deadline_ms", 480000u);
			break;
		case VSP_EV_PHASE_BEGIN:
		case VSP_EV_PHASE_END:
			jw_u32(w, "phase_index", r->u.phase.phase_index);
			jw_u32(w, "pending_markers", r->u.phase.pending_markers);
			jw_u32(w, "open_ops", r->u.phase.open_ops);
			break;
		case VSP_EV_RIG_READY: {
			const struct vsp_p_rig_ready* p = &r->u.rig_ready;
			jw_u32(w, "fixture_hash", p->fixture_hash);
			jw_targets(w, "targets", p->targets);
			jw_S(w, "host", &p->host);
			jw_S(w, "guest", &p->guest);
			jw_bool(w, "water_ok", p->water_ok);
			jw_bool(w, "support_ok", p->support_ok);
			jw_bool(w, "rails_ok", p->rails_ok);
			jw_bool(w, "ray_ok", p->ray_ok);
			jw_bool(w, "saddle_consumed", p->saddle_consumed);
			jw_u32(w, "pig_ai_updates", p->pig_ai_updates);
			break;
		}
		case VSP_EV_ACTION_ATTEMPT: {
			const struct vsp_p_action_attempt* p = &r->u.attempt;
			jw_str(w, "action", NAME(k_action, p->action));
			jw_str(w, "channel", NAME(k_channel, p->channel));
			jw_A(w, "source", &p->source);
			jw_R(w, "target", &p->target);
			jw_str(w, "button", NAME(k_button, p->button));
			jw_u32(w, "edge_seq", p->edge_seq);
			jw_u32(w, "held_ms", p->held_ms);
			jw_u32(w, "request_seq", p->request_seq);
			jw_bool(w, "desired_occupied", p->desired_occupied);
			jw_f(w, "reach_m", p->reach_m);
			jw_bool(w, "los", p->los);
			jw_u32(w, "camera_hit", p->camera_hit);
			break;
		}
		case VSP_EV_UI_ACTION: {
			const struct vsp_p_ui_action* p = &r->u.ui;
			jw_str(w, "action", NAME(k_ui, p->action));
			jw_u32(w, "player", p->player);
			jw_str(w, "screen", NAME(k_screen, p->screen));
			jw_str(w, "button", NAME(k_button, p->button));
			jw_u32(w, "held_ms", p->held_ms);
			jw_u32(w, "edge_seq", p->edge_seq);
			jw_bool(w, "delivered", p->delivered);
			jw_u32(w, "input_epoch", p->input_epoch);
			break;
		}
		case VSP_EV_ACTION_RESULT: {
			const struct vsp_p_action_result* p = &r->u.result;
			jw_str(w, "action", NAME(k_action, p->action));
			jw_A(w, "source", &p->source);
			jw_R(w, "target", &p->target);
			jw_str(w, "site", NAME(k_site, p->site));
			jw_str(w, "status", NAME(k_status, p->status));
			jw_u32(w, "request_seq", p->request_seq);
			jw_u32(w, "ride_epoch", p->ride_epoch);
			if(p->site == VSP_SITE_AUTHORITY) {
				jw_g64(w, "serial", p->serial);
				jw_S(w, "host", &p->u.authority.host);
				jw_V(w, "vehicle", &p->u.authority.vehicle);
			} else if(p->site == VSP_SITE_CLIENT) {
				jw_C(w, "host", &p->u.client.host);
				jw_VC(w, "vehicle", &p->u.client.vehicle);
			} else
				jw_str(w, "rpc", NAME(k_rpc, p->rpc));
			break;
		}
		case VSP_EV_RIDE_SAMPLE: {
			const struct vsp_p_ride_sample* p = &r->u.ride;
			jw_str(w, "stage", NAME(k_sc, p->stage));
			jw_R(w, "target", &p->target);
			jw_u32(w, "window_id", p->window_id);
			jw_u32(w, "samples", p->samples);
			jw_u32(w, "eligible_updates", p->eligible_updates);
			jw_u32(w, "tick_calls", p->tick_calls);
			jw_u32(w, "missing_calls", p->missing_calls);
			jw_u32(w, "duplicate_calls", p->duplicate_calls);
			jw_f(w, "displacement_m", p->displacement_m);
			if(p->stage == VSP_STAGE_SERVER) {
				jw_S(w, "host", &p->u.server.host);
				jw_V(w, "vehicle", &p->u.server.vehicle);
			} else {
				jw_C(w, "host", &p->u.client.host);
				jw_VC(w, "vehicle", &p->u.client.vehicle);
			}
			jw_f(w, "seat_error_max_m", p->seat_error_max_m);
			jw_u32(w, "follow_count", p->follow_count);
			jw_u32(w, "request_seq", p->request_seq);
			break;
		}
		case VSP_EV_DISMOUNT:
			jw_S(w, "host", &r->u.dismount.host);
			jw_V(w, "old", &r->u.dismount.old);
			jw_g64(w, "serial", r->u.dismount.serial);
			break;
		case VSP_EV_SWITCH: {
			const struct vsp_p_switch* p = &r->u.sw;
			jw_str(w, "from", vsp_kind_name((enum vsp_kind)p->from));
			jw_str(w, "to", vsp_kind_name((enum vsp_kind)p->to));
			jw_S(w, "host", &p->host);
			jw_V(w, "old", &p->old);
			jw_V(w, "new", &p->nu);
			jw_g64(w, "release_serial", p->release_serial);
			jw_g64(w, "mount_serial", p->mount_serial);
			break;
		}
		case VSP_EV_ISOLATION: {
			const struct vsp_p_isolation* p = &r->u.iso;
			jw_str(w, "stage", NAME(k_ba, p->stage));
			jw_str(w, "owner", NAME(k_sc, p->owner));
			jw_u32(w, "attempt_op", p->attempt_op);
			if(p->owner == VSP_STAGE_SERVER) {
				jw_S(w, "host", &p->u.server.host);
				jw_S(w, "guest", &p->u.server.guest);
				jw_V(w, "vehicle", &p->u.server.vehicle);
			} else {
				jw_C(w, "host", &p->u.client.host);
				jw_C(w, "guest", &p->u.client.guest);
				jw_VC(w, "vehicle", &p->u.client.vehicle);
			}
			jw_f(w, "guest_pose_delta_max_m", p->guest_pose_delta_max_m);
			jw_u32(w, "guest_follow_count", p->guest_follow_count);
			if(p->owner == VSP_STAGE_CLIENT)
				jw_u32(w, "unexpected_route_count", p->unexpected_route_count);
			jw_u32(w, "unexpected_attach_count", p->unexpected_attach_count);
			break;
		}
		case VSP_EV_CHECKPOINT: {
			const struct vsp_p_checkpoint* p = &r->u.ck;
			jw_str(w, "stage", NAME(k_ck, p->stage));
			jw_u32(w, "checkpoint_id", p->checkpoint_id);
			jw_S(w, "host", &p->host);
			jw_S(w, "guest", &p->guest);
			jw_V(w, "vehicle", &p->vehicle);
			jw_U3(w, "pose_bits", p->pose_bits);
			jw_u32(w, "fixture_hash", p->fixture_hash);
			jw_bool(w, "save_ok", p->save_ok);
			jw_bool(w, "level_ok", p->level_ok);
			jw_bool(w, "chunks_ok", p->chunks_ok);
			jw_bool(w, "regions_synced", p->regions_synced);
			break;
		}
		case VSP_EV_WORLD_RESET:
			jw_u32(w, "world_reset_count", r->u.reset.world_reset_count);
			jw_u32(w, "local_entity", r->u.reset.local_entity);
			jw_u32(w, "checkpoint_id", r->u.reset.checkpoint_id);
			jw_W(w, "source_world", &r->u.reset.source_world);
			jw_bool(w, "world_loaded", r->u.reset.world_loaded);
			break;
		case VSP_EV_TITLE:
			jw_bool(w, "world_loaded", r->u.title.world_loaded);
			jw_u32(w, "world_reset_count", r->u.title.world_reset_count);
			jw_u32(w, "checkpoint_id", r->u.title.checkpoint_id);
			jw_str(w, "screen", "title");
			jw_bool(w, "autoload_consumed", r->u.title.autoload_consumed);
			break;
		case VSP_EV_RELOAD: {
			const struct vsp_p_reload* p = &r->u.reload;
			jw_str(w, "stage", NAME(k_rl, p->stage));
			jw_str(w, "world_name", VSP_RIG_WORLD);
			jw_u32(w, "reload_count", 1);
			jw_u32(w, "checkpoint_id", p->checkpoint_id);
			switch(p->stage) {
				case VSP_RL_DISK:
					jw_W(w, "world", &p->u.disk.world);
					jw_U3(w, "pose_bits", p->u.disk.pose_bits);
					jw_bool(w, "disk_reopen", p->u.disk.disk_reopen);
					jw_bool(w, "player_read_ok", p->u.disk.player_read_ok);
					jw_bool(w, "fallback_used", p->u.disk.fallback_used);
					jw_bool(w, "override_used", p->u.disk.override_used);
					break;
				case VSP_RL_INVENTORY:
					jw_W(w, "world", &p->u.inventory.world);
					jw_bool(w, "read_ok", p->u.inventory.read_ok);
					jw_u32(w, "inv_hash", p->u.inventory.inv_hash);
					jw_u32(w, "inv_used", p->u.inventory.inv_used);
					break;
				case VSP_RL_ENTITIES:
					jw_S(w, "host", &p->u.entities.host);
					jw_S(w, "guest", &p->u.entities.guest);
					jw_u32(w, "fixture_hash", p->u.entities.fixture_hash);
					jw_bool(w, "saddle", p->u.entities.saddle);
					jw_targets(w, "targets", p->u.entities.targets);
					jw_u32(w, "ambiguous_matches",
						   p->u.entities.ambiguous_matches);
					break;
				default:
					jw_C(w, "host", &p->u.client_ready.host);
					jw_C(w, "guest", &p->u.client_ready.guest);
					jw_u32(w, "input_ack_epoch", p->u.client_ready.input_ack_epoch);
					break;
			}
			break;
		}
		case VSP_EV_DEATH: {
			const struct vsp_p_death* p = &r->u.death;
			jw_str(w, "stage", NAME(k_death, p->stage));
			if(p->stage == VSP_DEATH_SERVER) {
				jw_S(w, "before_host", &p->u.server.before_host);
				jw_S(w, "host", &p->u.server.host);
				jw_V(w, "vehicle", &p->u.server.vehicle);
				jw_text(w, "cause", p->u.server.cause);
				jw_u32(w, "deaths_before", p->u.server.deaths_before);
				jw_u32(w, "deaths_after", p->u.server.deaths_after);
			} else {
				jw_C(w, "host", &p->u.ui.host);
				jw_str(w, "screen", "death");
			}
			break;
		}
		case VSP_EV_RESPAWN: {
			const struct vsp_p_respawn* p = &r->u.respawn;
			jw_str(w, "stage", NAME(k_rs, p->stage));
			jw_u32(w, "old_input_epoch", p->old_input_epoch);
			jw_u32(w, "new_input_epoch", p->new_input_epoch);
			jw_g64(w, "respawn_epoch", p->respawn_epoch);
			if(p->stage == VSP_RS_CLIENT_READY) {
				jw_C(w, "host", &p->u.client.host);
				jw_bool(w, "ready", p->u.client.ready);
			} else {
				jw_S(w, "host", &p->u.server.host);
				jw_V(w, "old", &p->u.server.old);
				jw_g64(w, "serial", p->u.server.serial);
				jw_i32(w, "health", p->u.server.health);
				jw_bool(w, "ack", p->u.server.ack);
			}
			break;
		}
		case VSP_EV_TRANSFER_BEGIN: {
			const struct vsp_p_transfer* p = &r->u.transfer;
			jw_u32(w, "transfer_id", p->transfer_id);
			jw_A(w, "actor", &p->actor);
			jw_W(w, "src", &p->src);
			jw_W(w, "dst", &p->dst);
			jw_V(w, "vehicle", &p->vehicle);
			jw_str(w, "mode", p->mode ? "fixture_request" : "contact");
			jw_str(w, "state", "requested");
			jw_g64(w, "serial", p->serial);
			break;
		}
		case VSP_EV_TRANSFER_RELEASE: {
			const struct vsp_p_transfer* p = &r->u.transfer;
			jw_u32(w, "transfer_id", p->transfer_id);
			jw_A(w, "actor", &p->actor);
			jw_W(w, "src", &p->src);
			jw_W(w, "dst", &p->dst);
			jw_S(w, "host", &p->host);
			jw_V(w, "old", &p->vehicle);
			jw_g64(w, "release_serial", p->serial);
			break;
		}
		case VSP_EV_TRANSFER_COMMIT: {
			const struct vsp_p_transfer* p = &r->u.transfer;
			jw_u32(w, "transfer_id", p->transfer_id);
			jw_A(w, "actor", &p->actor);
			jw_W(w, "src", &p->src);
			jw_W(w, "dst", &p->dst);
			jw_S(w, "host", &p->host);
			jw_V(w, "source_vehicle", &p->vehicle);
			jw_u32(w, "destination_mount_count", p->destination_mount_count);
			jw_g64(w, "commit_serial", p->serial);
			jw_u32(w, "transition_epoch", p->transition_epoch);
			jw_str(w, "state", "committed");
			break;
		}
		case VSP_EV_TRANSFER_ACK: {
			const struct vsp_p_transfer_ack* p = &r->u.tack;
			jw_str(w, "stage", NAME(k_sc, p->stage));
			jw_u32(w, "transfer_id", p->transfer_id);
			jw_A(w, "actor", &p->actor);
			jw_W(w, "dst", &p->dst);
			jw_u32(w, "input_ack_epoch", p->input_ack_epoch);
			jw_bool(w, "ready", p->ready);
			if(p->stage == VSP_STAGE_SERVER) {
				jw_V(w, "source_vehicle", &p->u.source_vehicle);
				jw_u32(w, "destination_mount_count", p->destination_mount_count);
			} else
				jw_C(w, "host", &p->u.host);
			break;
		}
		case VSP_EV_PENDING: {
			const struct vsp_p_progress* p = &r->u.progress;
			jw_str(w, "stage", NAME(k_pd, p->stage));
			jw_str(w, "cause", NAME(k_cause, p->cause));
			jw_Q(w, "tuple", &p->tuple);
			jw_str(w, "result", p->result ? "installed" : "none");
			jw_bool(w, "paused", p->paused);
			jw_bool(w, "loading", p->loading);
			jw_bool(w, "transfer_open", p->transfer_open);
			jw_bool(w, "refusal_armed", p->refusal_armed);
			jw_bool(w, "pending", p->pending);
			jw_g64(w, "serial", p->serial);
			break;
		}
		case VSP_EV_PAUSE:
		case VSP_EV_RESUME: {
			const struct vsp_p_progress* p = &r->u.progress;
			jw_Q(w, "tuple", &p->tuple);
			jw_bool(w, "paused", p->paused);
			jw_bool(w, "loading", p->loading);
			jw_bool(w, "pending", p->pending);
			jw_bool(w, "refusal_armed", p->refusal_armed);
			jw_g64(w, "serial", p->serial);
			break;
		}
		case VSP_EV_ADMISSION_CLEAR: {
			const struct vsp_p_progress* p = &r->u.progress;
			jw_Q(w, "tuple", &p->tuple);
			jw_bool(w, "refusal_armed", false);
			jw_bool(w, "pending", p->pending);
			jw_bool(w, "transfer_open", p->transfer_open);
			jw_u32(w, "transfer_id", p->transfer_id);
			jw_bool(w, "loading", p->loading);
			jw_g64(w, "serial", p->serial);
			break;
		}
		case VSP_EV_BARRIER_RELEASE: {
			const struct vsp_p_progress* p = &r->u.progress;
			jw_Q(w, "tuple", &p->tuple);
			jw_u32(w, "transfer_id", p->transfer_id);
			jw_bool(w, "pending", p->pending);
			jw_bool(w, "paused", p->paused);
			jw_bool(w, "transfer_open", p->transfer_open);
			jw_bool(w, "loading", p->loading);
			jw_bool(w, "host_ack_pending", p->host_ack_pending);
			jw_g64(w, "serial", p->serial);
			break;
		}
		case VSP_EV_INSTALLED: {
			const struct vsp_p_progress* p = &r->u.progress;
			jw_Q(w, "tuple", &p->tuple);
			jw_u32(w, "installed_count", p->installed_count);
			jw_str(w, "result", "installed");
			jw_bool(w, "pending", false);
			jw_g64(w, "serial", p->serial);
			jw_bool(w, "paused", p->paused);
			jw_bool(w, "loading", p->loading);
			jw_bool(w, "transfer_open", p->transfer_open);
			break;
		}
		case VSP_EV_GAMEPLAY_READY: {
			const struct vsp_p_gameplay_ready* p = &r->u.ready;
			jw_str(w, "stage", NAME(k_sc, p->stage));
			jw_Q(w, "tuple", &p->tuple);
			if(p->stage == VSP_STAGE_SERVER)
				jw_S(w, "guest", &p->u.server_guest);
			else
				jw_C(w, "guest", &p->u.client_guest);
			jw_bool(w, "confirmed", p->confirmed);
			jw_u32(w, "ready_count", p->ready_count);
			jw_u32(w, "installed_count", p->installed_count);
			break;
		}
		case VSP_EV_RUN_OK:
			jw_u32(w, "completed_phases", 6);
			jw_u32(w, "pending_markers", 0);
			jw_u32(w, "open_ops", 0);
			jw_u32(w, "overflow_count", 0);
			jw_u32(w, "hook_mask", 0);
			break;
		case VSP_EV_FAIL:
			jw_text(w, "code", r->u.fail.code);
			jw_str(w, "failed_phase", NAME(k_phase, r->u.fail.failed_phase));
			jw_u32(w, "overflow_count", r->u.fail.overflow_count);
			jw_u32(w, "hook_mask", 0);
			break;
		default:
			w->err = "unknown_event";
			break;
	}
}

size_t vsp_serialize(const struct vsp_record* rec, uint32_t seq,
					 size_t reserve, char* out, size_t cap,
					 const char** err) {
	if(err)
		*err = NULL;
	if(!rec || !out || !cap || rec->event >= VSP_EV_COUNT || !seq) {
		if(err)
			*err = "bad_record";
		return 0;
	}
	struct jw w = {.buf = out, .cap = cap, .n = 0, .err = NULL};
	out[0] = 0;
	if(!vsp_text_ok(s_cfg.build_id, true))
		w.err = "bad_build_id";
	jw_raw(&w, "{");
	jw_u32(&w, "schema", VSP_SCHEMA);
	jw_str(&w, "build_id", s_cfg.build_id);
	jw_str(&w, "profile", VSP_PROFILE);
	jw_str(&w, "kind", vsp_kind_name(s_cfg.kind));
	jw_u32(&w, "seq", seq);
	jw_str(&w, "phase", NAME(k_phase, rec->phase));
	jw_u32(&w, "op_id", rec->op_id);
	jw_u32(&w, "ms", rec->ms);
	jw_u32(&w, "server_update", rec->server_update);
	jw_u32(&w, "active_update", rec->active_update);
	jw_key(&w, "event");
	jw_fmt(&w, "\"vehicle_smoke.%s\"", k_event_names[rec->event]);
	jw_str(&w, "origin", NAME(k_origin, rec->origin));
	jw_bool(&w, "ok", rec->ok);
	jw_text(&w, "reason", rec->reason);
	vsp_payload(&w, rec);
	jw_raw(&w, "}");
	if(!w.err && w.n + reserve > VSP_LINE_MAX)
		w.err = "oversize";
	if(w.err) {
		if(err)
			*err = w.err;
		return 0;
	}
	return w.n;
}
