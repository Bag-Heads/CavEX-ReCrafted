/*
	F3/F4: focused client-side minecart ride-intent + camera reconciliation tests.

	Drives the REAL entity_tick (local player) through the CAVEWII_AGENT bridge
	synthetic input layer so JUMP escape, pre-selection, and the epoch-zero
	optimistic mount path are exercised through production code, not direct
	helper calls.

	Compile: PLATFORM_PC CAVEWII_AGENT_BRIDGE CGLM_ALL_UNALIGNED
	          CAVEWII_CLIENT_INTERFACE_TEST
	Runtime: CAVEWII_AGENT=1 (set via putenv before the first armed check)
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include "support/test_fixture_dir.h"

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "entity/entity_minecart.h"
#include "entity/entity_local_player.h" /* D1 ride-action frame hook */
#include "game/camera.h"
#include "game/game_state.h"
#include "game/agent_input.h"
#include "item/inventory.h"
#include "network/inventory_logic.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "particle.h"
#include "platform/thread.h"
#include "platform/input_owner.h" /* O07 D1-2: input_player_bind */

#define CAVEWII_F1_DISPATCH_ONLY
#include "../source/main.c"
#undef CAVEWII_F1_DISPATCH_ONLY

/* ---- shared state ---- */

static struct server_local s;
static volatile int g_drain_run = 1;

#define RPC_INBOX_SIZE 16
#define SVIN_ACTION_PENDING_SIZE 64

struct transport_capture {
	struct server_rpc calls[256];
	size_t count;
};

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void capture_transport(struct server_rpc* call, void* user) {
	struct transport_capture* out = user;
	assert(out && out->count < sizeof(out->calls) / sizeof(out->calls[0]));
	out->calls[out->count++] = *call;
}

static void discard_transport(struct server_rpc* call, void* user) {
	(void)call;
	(void)user;
}

static void drain_all_transport(struct transport_capture* out) {
	while(1) {
		size_t before = out ? out->count : 0;
		svin_process_messages(
			out ? capture_transport : discard_transport, out, false);
		if(svin_pending_count() == 0)
			break;
		svin_rpc_flush_pending();
		if(out && out->count == before && svin_pending_count() > 0)
			continue;
	}
}

static size_t count_transport_type(
	const struct transport_capture* out, enum server_rpc_type type) {
	size_t count = 0;
	for(size_t i = 0; i < out->count; i++)
		if(out->calls[i].type == type)
			count++;
	return count;
}

static const struct server_rpc* last_transport_type(
	const struct transport_capture* out, enum server_rpc_type type) {
	for(size_t i = out->count; i > 0; i--)
		if(out->calls[i - 1].type == type)
			return &out->calls[i - 1];
	return NULL;
}

static void clear_transport(void) {
	drain_all_transport(NULL);
}

static bool nearly_equal(float a, float b) {
	return fabsf(a - b) < 0.0005F;
}

/* ---- helpers ---- */

static struct entity* spawn_client_minecart_state(
	uint32_t id, float x, float y, float z,
	bool occupied, uint32_t occupant_id, uint32_t ride_epoch) {
	struct client_rpc rpc = {
		.type = CRPC_SPAWN_MINECART,
		.payload.spawn_minecart = {
			.entity_id = id,
			.pos = {x, y, z},
			.vel = {0, 0, 0},
			.speed = 0.0f,
			.hx = 0, .hz = 1,
			.occupied = occupied,
			.occupant_id = occupant_id,
			.ride_epoch = ride_epoch,
		},
	};
	clin_process(&rpc);
	struct entity** pp = dict_entity_get(gstate.entities, id);
	assert(pp && *pp && (*pp)->type == ENTITY_MINECART);
	return *pp;
}

static struct entity* spawn_client_minecart(uint32_t id, float x, float y, float z) {
	return spawn_client_minecart_state(id, x, y, z, false, 0, 0);
}

static void publish_occupancy(
	uint32_t entity_id, bool occupied, uint32_t occupant_id,
	uint32_t ride_epoch, uint32_t input_epoch, uint32_t request_seq) {
	struct client_rpc rpc = {
		.type = CRPC_MINECART_OCCUPANCY,
		.payload.minecart_occupancy = {
			.entity_id = entity_id,
			.occupant_id = occupant_id,
			.ride_epoch = ride_epoch,
			.occupied = occupied,
			.input_epoch = input_epoch,
			.request_seq = request_seq,
		},
	};
	clin_process(&rpc);
}

static void send_snapshot(
	struct entity* cart, float dx, float dy, float dz,
	bool wire_occupied) {
	struct client_rpc rpc = {
		.type = CRPC_MINECART_SNAPSHOT,
		.payload.minecart_snapshot = {
			.entity_id = cart->id,
			.pos = {
				cart->pos[0] + dx,
				cart->pos[1] + dy,
				cart->pos[2] + dz,
			},
			.vel = {0, 0, 0},
			.ride_epoch = cart->data.minecart.ride_epoch,
			.occupied = wire_occupied,
			.occupant_id = wire_occupied ? gstate.players[0].local_player->id : 0,
		},
	};
	clin_process(&rpc);
}

static void destroy_client_entity(uint32_t id) {
	struct client_rpc rpc = {
		.type = CRPC_ENTITY_DESTROY,
		.payload.entity_destroy.entity_id = id,
	};
	clin_process(&rpc);
}

static void tick_local_player(void) {
	gstate.players[0].local_player->data.local_player.capture_input = true;
	assert(gstate.players[0].local_player->tick_client);
	gstate.players[0].local_player->tick_client(gstate.players[0].local_player);
}

static void seed_camera_correction(float x, float y, float z) {
	gstate.players[0].ride.camera_correction[0] = x;
	gstate.players[0].ride.camera_correction[1] = y;
	gstate.players[0].ride.camera_correction[2] = z;
	gstate.players[0].ride.camera_correction_start[0] = x;
	gstate.players[0].ride.camera_correction_start[1] = y;
	gstate.players[0].ride.camera_correction_start[2] = z;
	gstate.players[0].ride.camera_correction_elapsed = 0.0F;
	gstate.players[0].ride.camera_correction_active = true;
}

static void assert_wire_matches_body(const char* phase) {
	clear_transport();
	struct transport_capture out = {0};
	clin_update();
	const unsigned slots[] = {0};
	main_client_host_tick_dispatch(slots, 1, true, false, -1);
	drain_all_transport(&out);
	assert(count_transport_type(&out, SRPC_PLAYER_POS) == 1);
	const struct server_rpc* pos = last_transport_type(&out, SRPC_PLAYER_POS);
	if(!pos)
		fprintf(stderr, "missing SRPC_PLAYER_POS during %s\n", phase);
	assert(pos);
	assert(nearly_equal((float)pos->payload.player_pos.x,
						gstate.players[0].local_player->pos[0]));
	assert(nearly_equal((float)pos->payload.player_pos.y,
						gstate.players[0].local_player->pos[1]));
	assert(nearly_equal((float)pos->payload.player_pos.z,
						gstate.players[0].local_player->pos[2]));
}

/* Confirm a server-authoritative mount on the client side via the real
 * occupancy writer (CRPC_MINECART_OCCUPANCY), not a snapshot. Zero/zero
 * correlation is appropriate for unsolicited setup. */
static void confirm_mount(struct entity* cart, uint32_t player_id) {
	publish_occupancy(cart->id, true, player_id, 1, 0, 0);
}

/* Determine which of two entity IDs comes first in dict iteration order. */
static uint32_t first_in_dict_order(uint32_t id_a, uint32_t id_b) {
	dict_entity_it_t it;
	dict_entity_it(it, gstate.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && (e->id == id_a || e->id == id_b))
			return e->id;
		dict_entity_next(it);
	}
	assert(0 && "neither cart found in dict");
	return 0;
}

static void reset_ride_state(void) {
	minecart_ride_client_state_reset(&gstate.players[0].ride, NULL, false);
}

/* ===================================================================== */
/* Test 1: A-B/JUMP both dictionary orders (pending B wins)              */
/* ===================================================================== */
static void test_ab_jump_preselect(uint32_t id_a, uint32_t id_b) {
	reset_ride_state();
	clear_transport();

	struct entity* cart_a = spawn_client_minecart(id_a, 4.5f, 101.0f, 4.5f);
	struct entity* cart_b = spawn_client_minecart(id_b, 6.5f, 101.0f, 4.5f);

	printf("  test_ab_jump_preselect: A=%u B=%u (A first in dict: %s)\n",
		id_a, id_b,
		first_in_dict_order(id_a, id_b) == id_a ? "yes" : "no");

	/* Player is confirmed riding cart A via the real occupancy writer. */
	uint32_t pid = gstate.players[0].local_player->id;
	confirm_mount(cart_a, pid);
	assert(cart_a->data.minecart.occupied);
	assert(cart_a->data.minecart.occupant_id == pid);
	assert(cart_a->data.minecart.control_ready);
	assert(cart_a->data.minecart.ride_epoch == 1);

	/* Player position on top of cart A (follow-pin state). */
	gstate.players[0].local_player->pos[0] = cart_a->pos[0];
	gstate.players[0].local_player->pos[1] = cart_a->pos[1] + 1.0f;
	gstate.players[0].local_player->pos[2] = cart_a->pos[2];

	/* Issue a pending mount for cart B via onRightClick.
	 * This optimistically marks B as occupied by the player and sets
	 * gstate.players[0].ride.pending=true, desired_occupied=true, target_id=B. */
	assert(cart_b->onRightClick(cart_b));
	assert(gstate.players[0].ride.pending);
	assert(gstate.players[0].ride.desired_occupied);
	assert(gstate.players[0].ride.target_id == id_b);
	assert(cart_b->data.minecart.occupied);
	assert(cart_b->data.minecart.occupant_id == pid);
	/* B has no control_ready yet (pending server confirmation). */
	assert(!cart_b->data.minecart.control_ready);
	assert(cart_b->data.minecart.ride_epoch == 0);

	/* Feed JUMP=1 via the agent input bridge. */
	agent_input_feed_line("JUMP=1");
	agent_input_poll();

	/* Run entity_tick on the local player — this is the production path
	 * where JUMP escape is detected and the ride-intent is authored. The
	 * main.c per-frame ride-action hook (D1) runs first, as in production. */
	gstate.players[0].local_player->data.local_player.capture_input = true;
	entity_local_player_ride_action_frame();
	assert(gstate.players[0].local_player->tick_client);
	gstate.players[0].local_player->tick_client(gstate.players[0].local_player);

	/* The JUMP must have authored a dismount intent targeting B, not A. */
	assert(gstate.players[0].ride.target_id == id_b
		&& "JUMP must target pending mount B, not confirmed cart A");
	assert(!gstate.players[0].ride.desired_occupied
		&& "JUMP must author desired_occupied=false (dismount)");
	assert(gstate.players[0].ride.pending
		&& "JUMP escape must set pending=true for the dismount intent");
	assert(gstate.players[0].ride.send_dirty
		&& "JUMP escape must set send_dirty=true");

	/* Cart A must still be occupied (JUMP targeted B, not A). */
	assert(cart_a->data.minecart.occupied
		&& "cart A must remain occupied; JUMP targeted B");

	/* Cart B must be locally detached (optimistic dismount prediction). */
	assert(!cart_b->data.minecart.occupied
		&& "cart B must be detached; JUMP authored dismount for B");

	/* The real client update must emit exactly one desired=false intent for B.
	 * This closes the local-only loophole: neither hash order may send a false
	 * intent for A or retain B's optimistic desired=true request. */
	struct transport_capture transport = {0};
	clin_update();
	drain_all_transport(&transport);
	assert(count_transport_type(&transport, SRPC_MINECART_RIDE_INTENT) == 1);
	const struct server_rpc* intent
		= last_transport_type(&transport, SRPC_MINECART_RIDE_INTENT);
	assert(intent);
	assert(intent->payload.minecart_ride_intent.entity_id == id_b);
	assert(!intent->payload.minecart_ride_intent.desired_occupied);
	assert(intent->payload.minecart_ride_intent.input_epoch == 1);
	assert(intent->payload.minecart_ride_intent.request_seq
		   == gstate.players[0].ride.pending_request_seq);

	/* Clean up: remove both carts for the next test iteration. */
	destroy_client_entity(id_a);
	destroy_client_entity(id_b);

	/* Release JUMP so the next test starts clean. */
	agent_input_feed_line("JUMP=0");
	agent_input_poll();
}

/* ===================================================================== */
/* Test 2: Epoch-zero optimistic mount + JUMP                             */
/* ===================================================================== */
static void test_epoch_zero_jump(uint32_t id_a) {
	printf("  test_epoch_zero_jump: cart=%u\n", id_a);

	reset_ride_state();
	clear_transport();

	struct entity* cart = spawn_client_minecart(id_a, 4.5f, 101.0f, 4.5f);
	uint32_t pid = gstate.players[0].local_player->id;

	/* Epoch-zero optimistic mount: player right-clicks the cart.
	 * onRightClick sets occupied=true, control_ready=false, ride_epoch=0,
	 * and gstate.players[0].ride.pending=true with desired_occupied=true. */
	assert(cart->onRightClick(cart));
	assert(gstate.players[0].ride.pending);
	assert(gstate.players[0].ride.desired_occupied);
	assert(gstate.players[0].ride.target_id == id_a);
	assert(cart->data.minecart.occupied);
	assert(cart->data.minecart.occupant_id == pid);
	assert(!cart->data.minecart.control_ready);
	assert(cart->data.minecart.ride_epoch == 0);

	/* Player position on top of cart. */
	gstate.players[0].local_player->pos[0] = cart->pos[0];
	gstate.players[0].local_player->pos[1] = cart->pos[1] + 1.0f;
	gstate.players[0].local_player->pos[2] = cart->pos[2];

	/* Feed JUMP+FORWARD. JUMP must author the leave before the epoch gate,
	 * suppress cart throttle, then continue through ordinary player physics. */
	agent_input_feed_line("JUMP=1 FORWARD=1");
	agent_input_poll();
	float old_x = gstate.players[0].local_player->pos[0];
	float old_z = gstate.players[0].local_player->pos[2];
	uint32_t old_control_seq = cart->data.minecart.control_seq;

	/* Run entity_tick — JUMP should escape even with epoch zero. The main.c
	 * per-frame ride-action hook (D1) runs first, as in production. */
	gstate.players[0].local_player->data.local_player.capture_input = true;
	entity_local_player_ride_action_frame();
	tick_local_player();

	/* JUMP authored a dismount intent. */
	assert(gstate.players[0].ride.target_id == id_a
		&& "JUMP must target the epoch-zero cart");
	assert(!gstate.players[0].ride.desired_occupied
		&& "JUMP must author desired_occupied=false");
	assert(gstate.players[0].ride.pending
		&& "dismount intent must be pending");

	/* Cart must be locally detached. */
	assert(!cart->data.minecart.occupied
		&& "cart must be detached after JUMP escape");
	assert(cart->data.minecart.control_seq == old_control_seq
		&& "JUMP escape tick must not author throttle");
	assert(gstate.players[0].local_player->pos[1] >= gstate.players[0].ride.exit_floor_y);
	assert(gstate.players[0].local_player->pos_old[1] >= gstate.players[0].ride.exit_floor_y);
	assert(!nearly_equal(gstate.players[0].local_player->pos[0], old_x)
		   || !nearly_equal(gstate.players[0].local_player->pos[2], old_z)
		   || fabsf(gstate.players[0].local_player->vel[0]) > 0.0005F
		   || fabsf(gstate.players[0].local_player->vel[2]) > 0.0005F);

	/* Exercise the actual transport and the correlated authority retirement. */
	struct transport_capture transport = {0};
	clin_update();
	drain_all_transport(&transport);
	assert(count_transport_type(&transport, SRPC_MINECART_RIDE_INTENT) == 1);
	const struct server_rpc* intent
		= last_transport_type(&transport, SRPC_MINECART_RIDE_INTENT);
	assert(intent && intent->payload.minecart_ride_intent.entity_id == id_a);
	assert(!intent->payload.minecart_ride_intent.desired_occupied);
	assert(intent->payload.minecart_ride_intent.input_epoch == 1);
	publish_occupancy(
		id_a, false, 0, 0,
		intent->payload.minecart_ride_intent.input_epoch,
		intent->payload.minecart_ride_intent.request_seq);
	assert(!gstate.players[0].ride.pending && !gstate.players[0].ride.send_dirty);
	assert(!cart->data.minecart.occupied && !cart->data.minecart.control_ready);

	/* Clean up. */
	destroy_client_entity(id_a);
	agent_input_feed_line("JUMP=0 FORWARD=0");
	agent_input_poll();
}

/* Test 3 (duplicate RPC delivery) belongs in test_minecart_authority, not
 * here — it tests the server handler's idempotence to the same desired-state
 * RPC delivered twice, not client-side behavior. */

/* ===================================================================== */
/* Test 4: Client rollback on correlated rejection                        */
/* (The authority half — server out-of-range reach check — lives in      */
/*  test_minecart_authority section 18.)                                  */
/* ===================================================================== */
static void test_correlated_rejection_rollback(uint32_t id_a) {
	printf("  test_correlated_rejection_rollback: cart=%u\n", id_a);

	reset_ride_state();
	struct entity* cart = spawn_client_minecart(id_a, 4.5f, 101.0f, 4.5f);
	uint32_t pid = gstate.players[0].local_player->id;

	/* Optimistic mount: player right-clicks the cart. */
	assert(cart->onRightClick(cart));
	assert(gstate.players[0].ride.pending);
	assert(gstate.players[0].ride.desired_occupied);
	assert(cart->data.minecart.occupied);
	assert(cart->data.minecart.occupant_id == pid);
	assert(cart->rightClickText);
	assert(strcmp(cart->rightClickText, "Dismount") == 0
		&& "optimistic mount must set Dismount text");

	/* Server rejects (e.g. out-of-range). The correlated occupancy reply
	 * carries occupied=false, epoch=0, with the request's epoch/seq.
	 * For the client to retire the pending intent, the correlation
	 * must match. Set up the ride state's input_epoch to match. */
	gstate.players[0].ride.input_epoch = 1;
	struct client_rpc reject = {
		.type = CRPC_MINECART_OCCUPANCY,
		.payload.minecart_occupancy = {
			.entity_id = id_a,
			.occupant_id = 0,
			.ride_epoch = 0,
			.occupied = false,
			.input_epoch = gstate.players[0].ride.input_epoch,
			.request_seq = gstate.players[0].ride.pending_request_seq,
		},
	};
	clin_process(&reject);

	/* Prediction must be rolled back: cart is no longer occupied. */
	assert(!cart->data.minecart.occupied
		&& "rejected mount must roll back optimistic occupancy");
	assert(cart->data.minecart.occupant_id == 0
		&& "occupant_id must be cleared on rollback");
	assert(!cart->data.minecart.control_ready
		&& "control_ready must be false after rollback");
	assert(cart->data.minecart.ride_epoch == 0
		&& "ride_epoch must be 0 after rollback");
	assert(strcmp(cart->rightClickText, "Ride") == 0
		&& "rejected mount must restore Ride text");

	/* Pending intent must be retired by the correlated reply. */
	assert(!gstate.players[0].ride.pending
		&& "correlated rejection must retire pending intent");
	assert(!gstate.players[0].ride.send_dirty
		&& "correlated rejection must clear send_dirty");

	/* Preserve the pre-F3 ownership rule: clicking a cart occupied by a
	 * different entity must not author a leave intent or predict it empty. */
	uint32_t other_id = pid ^ 1U;
	cart->data.minecart.occupied = true;
	cart->data.minecart.occupant_id = other_id;
	cart->rightClickText = "Dismount";
	assert(!cart->onRightClick(cart)
		&& "another entity's occupied cart must reject local ride authoring");
	assert(cart->data.minecart.occupied);
	assert(cart->data.minecart.occupant_id == other_id);
	assert(!gstate.players[0].ride.pending && !gstate.players[0].ride.send_dirty);

	/* Clean up. */
	destroy_client_entity(id_a);
}

/* ===================================================================== */
/* Test 5: Stale-fence while pending + correlated empty retirement        */
/* (The authority half — server nonexistent-target — lives in             */
/*  test_minecart_authority section 19.)                                  */
/* ===================================================================== */
static void test_stale_fence_and_retire(uint32_t id_a) {
	printf("  test_stale_fence_and_retire: cart=%u\n", id_a);

	reset_ride_state();
	struct entity* cart = spawn_client_minecart(id_a, 4.5f, 101.0f, 4.5f);
	uint32_t pid = gstate.players[0].local_player->id;

	/* Optimistic mount. */
	assert(cart->onRightClick(cart));
	assert(gstate.players[0].ride.pending);
	gstate.players[0].ride.input_epoch = 1;
	uint32_t saved_seq = gstate.players[0].ride.pending_request_seq;

	/* Capture the optimistic cart state for unchanged assertions. */
	bool occ_before = cart->data.minecart.occupied;
	uint32_t occ_id_before = cart->data.minecart.occupant_id;
	uint32_t epoch_before = cart->data.minecart.ride_epoch;
	bool ctrl_before = cart->data.minecart.control_ready;
	const char* rct_before = cart->rightClickText;

	/* PART A: Stale-fence while pending is still true.
	 *
	 * Send a same-target nonmatching (older) correlation WHILE the
	 * newer intent is still pending. The production fence at
	 * client_interface.c:1587 is conditional on pending being true,
	 * so the stale reply must be silently dropped — cart state and
	 * centralized ride state remain unchanged.
	 *
	 * Sending the exact empty first would clear pending before the stale
	 * response arrives, so it would not exercise this fence. */
	struct client_rpc stale = {
		.type = CRPC_MINECART_OCCUPANCY,
		.payload.minecart_occupancy = {
			.entity_id = id_a,
			.occupant_id = pid,
			.ride_epoch = 99,
			.occupied = true,
			.input_epoch = gstate.players[0].ride.input_epoch,
			.request_seq = saved_seq + 999, /* different seq */
		},
	};
	clin_process(&stale);

	/* Cart state must be unchanged by the fenced stale reply. */
	assert(cart->data.minecart.occupied == occ_before
		&& "stale reply must not change occupied");
	assert(cart->data.minecart.occupant_id == occ_id_before
		&& "stale reply must not change occupant_id");
	assert(cart->data.minecart.ride_epoch == epoch_before
		&& "stale reply must not change ride_epoch");
	assert(cart->data.minecart.control_ready == ctrl_before
		&& "stale reply must not change control_ready");
	assert(cart->rightClickText == rct_before
		&& "stale reply must not change rightClickText");

	/* Centralized ride state must be unchanged. */
	assert(gstate.players[0].ride.pending
		&& "stale reply must not clear pending");
	assert(gstate.players[0].ride.send_dirty
		&& "stale reply must not clear send_dirty");
	assert(gstate.players[0].ride.target_id == id_a
		&& "stale reply must not change target_id");
	assert(gstate.players[0].ride.input_epoch == 1
		&& "stale reply must not change input_epoch");

	/* PART B: Exact correlated empty retires the pending intent.
	 *
	 * Now send the exact matching correlation. The retirement path
	 * fires: pending=false, send_dirty=false, optimistic prediction
	 * rolled back. */
	struct client_rpc exact_empty = {
		.type = CRPC_MINECART_OCCUPANCY,
		.payload.minecart_occupancy = {
			.entity_id = id_a,
			.occupant_id = 0,
			.ride_epoch = 0,
			.occupied = false,
			.input_epoch = gstate.players[0].ride.input_epoch,
			.request_seq = saved_seq,
		},
	};
	clin_process(&exact_empty);

	assert(!gstate.players[0].ride.pending
		&& "correlated empty must retire pending intent");
	assert(!gstate.players[0].ride.send_dirty
		&& "correlated empty must clear send_dirty");
	assert(!cart->data.minecart.occupied
		&& "correlated empty must roll back optimistic occupancy");
	assert(cart->data.minecart.occupant_id == 0
		&& "correlated empty must clear occupant_id");

	/* Clean up. */
	destroy_client_entity(id_a);
}

/* ===================================================================== */
/* Test 6: real clin_update retry after cross-epoch reserve pressure      */
/* ===================================================================== */
static void test_clin_update_retry_after_transport_rejection(void) {
	printf("  test_clin_update_retry_after_transport_rejection\n");
	clear_transport();
	assert(svin_pending_count() == 0);

	/* Fill the 16-slot inbox, then the 64 ordinary-action budget. */
	for(int i = 0; i < RPC_INBOX_SIZE; i++) {
		struct server_rpc pos = {.type = SRPC_PLAYER_POS};
		assert(svin_rpc_send(&pos));
	}
	for(int i = 0; i < SVIN_ACTION_PENDING_SIZE; i++) {
		struct server_rpc dig = {
			.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {
				.x = (w_coord_t)i, .y = 64, .z = 0, .finished = false,
			},
		};
		assert(svin_rpc_send(&dig));
	}

	/* Occupy the dedicated ride reserve with a different input epoch. The
	 * current-epoch request cannot coalesce into it and must remain dirty. */
	struct server_rpc old_ride = {
		.type = SRPC_MINECART_RIDE_INTENT,
		.payload.minecart_ride_intent = {
			.entity_id = 700,
			.input_epoch = 99,
			.request_seq = 1,
			.desired_occupied = true,
		},
	};
	assert(svin_rpc_send(&old_ride));
	assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 1);

	reset_ride_state();
	gstate.players[0].ride.target_id = 701;
	gstate.players[0].ride.next_request_seq = 7;
	gstate.players[0].ride.pending_request_seq = 7;
	gstate.players[0].ride.desired_occupied = false;
	gstate.players[0].ride.pending = true;
	gstate.players[0].ride.send_dirty = true;
	clin_update();
	assert(gstate.players[0].ride.send_dirty
		&& "rejected clin_update ride send must remain dirty");

	struct transport_capture pressure = {0};
	drain_all_transport(&pressure);
	assert(count_transport_type(&pressure, SRPC_MINECART_RIDE_INTENT) == 1);
	const struct server_rpc* queued
		= last_transport_type(&pressure, SRPC_MINECART_RIDE_INTENT);
	assert(queued && queued->payload.minecart_ride_intent.input_epoch == 99);

	/* With pressure gone, the same production clin_update retries and accepts
	 * the exact current transaction rather than rotating or dropping it. */
	struct transport_capture retry = {0};
	clin_update();
	assert(!gstate.players[0].ride.send_dirty);
	drain_all_transport(&retry);
	assert(count_transport_type(&retry, SRPC_MINECART_RIDE_INTENT) == 1);
	const struct server_rpc* sent
		= last_transport_type(&retry, SRPC_MINECART_RIDE_INTENT);
	assert(sent);
	assert(sent->payload.minecart_ride_intent.entity_id == 701);
	assert(sent->payload.minecart_ride_intent.input_epoch == 1);
	assert(sent->payload.minecart_ride_intent.request_seq == 7);
	assert(!sent->payload.minecart_ride_intent.desired_occupied);
	reset_ride_state();
	clear_transport();
}

/* ===================================================================== */
/* Test 7: pending leave blocks snapshot/follow re-pin; lift is one-shot  */
/* ===================================================================== */
static void test_pending_leave_and_exit_lift(uint32_t id) {
	printf("  test_pending_leave_and_exit_lift: cart=%u\n", id);
	reset_ride_state();
	struct entity* cart = spawn_client_minecart(id, 8.5F, 101.0F, 8.5F);
	confirm_mount(cart, gstate.players[0].local_player->id);
	gstate.players[0].local_player->pos[0] = cart->pos[0];
	gstate.players[0].local_player->pos[1] = cart->pos[1] + 1.0F;
	gstate.players[0].local_player->pos[2] = cart->pos[2];
	glm_vec3_copy(gstate.players[0].local_player->pos, gstate.players[0].local_player->pos_old);

	assert(cart->onRightClick(cart));
	assert(gstate.players[0].ride.pending && !gstate.players[0].ride.desired_occupied);
	assert(!cart->data.minecart.occupied);
	float lifted_y = gstate.players[0].local_player->pos[1];
	float lifted_old_y = gstate.players[0].local_player->pos_old[1];
	float cart_x = cart->pos[0];

	/* Two stale occupied snapshots may update cart physics only. They cannot
	 * restore occupancy, move the detached body/camera, or consume the lift. */
	for(int i = 0; i < 2; i++) {
		send_snapshot(cart, 0.10F, 0.0F, 0.0F, true);
		assert(!cart->data.minecart.occupied);
		assert(cart->data.minecart.occupant_id == 0);
		assert(nearly_equal(gstate.players[0].local_player->pos[1], lifted_y));
		assert(nearly_equal(gstate.players[0].local_player->pos_old[1], lifted_old_y));
		assert(gstate.players[0].ride.pending && gstate.players[0].ride.exit_lift_pending);
	}
	assert(nearly_equal(cart->pos[0], cart_x + 0.20F));

	/* Simulate a chunk respawn that reinstalled the old occupied spawn while
	 * the leave is pending, then deliver another same-session snapshot. The
	 * pending-leave guard must suppress rider/camera correction. */
	cart->data.minecart.occupied = true;
	cart->data.minecart.occupant_id = gstate.players[0].local_player->id;
	float body_x_before = gstate.players[0].local_player->pos[0];
	float body_y_before = gstate.players[0].local_player->pos[1];
	send_snapshot(cart, 0.10F, 0.0F, 0.0F, true);
	assert(nearly_equal(cart->pos[0], cart_x + 0.30F));
	assert(nearly_equal(gstate.players[0].local_player->pos[0], body_x_before));
	assert(nearly_equal(gstate.players[0].local_player->pos[1], body_y_before));
	assert(gstate.players[0].ride.pending && gstate.players[0].ride.exit_lift_pending);

	/* A client tick during the pending leave must use walking physics, not copy
	 * the body back to the cart even if a stale local claim is injected. */
	float detached_x = gstate.players[0].local_player->pos[0] + 2.0F;
	gstate.players[0].local_player->pos[0] = detached_x;
	gstate.players[0].local_player->pos_old[0] = detached_x;
	agent_input_feed_line("JUMP=0 FORWARD=0");
	agent_input_poll();
	tick_local_player();
	assert(!nearly_equal(gstate.players[0].local_player->pos[0], cart->pos[0]));

	/* Natural fall below the remembered floor is not evidence of a re-pin. */
	cart->data.minecart.occupied = false;
	cart->data.minecart.occupant_id = 0;
	gstate.players[0].local_player->pos[1] = gstate.players[0].ride.exit_floor_y - 0.50F;
	gstate.players[0].local_player->pos_old[1] = gstate.players[0].ride.exit_floor_y - 0.50F;
	gstate.players[0].ride.input_epoch = 1;
	uint32_t seq = gstate.players[0].ride.pending_request_seq;
	float natural_y = gstate.players[0].local_player->pos[1];
	publish_occupancy(id, false, 0, 0, 1, seq);
	assert(!gstate.players[0].ride.pending && !gstate.players[0].ride.exit_lift_pending);
	assert(nearly_equal(gstate.players[0].local_player->pos[1], natural_y));

	/* Positive reconciliation fixture: only an explicit observed-repin latch
	 * permits the matching empty reply to restore the floor, and only once. */
	gstate.players[0].ride.target_id = id;
	gstate.players[0].ride.input_epoch = 1;
	gstate.players[0].ride.pending_request_seq = seq + 1;
	gstate.players[0].ride.pending = true;
	gstate.players[0].ride.desired_occupied = false;
	gstate.players[0].ride.exit_floor_y = cart->pos[1] + 2.2F;
	gstate.players[0].ride.exit_lift_pending = true;
	gstate.players[0].ride.exit_repin_eligible = true;
	gstate.players[0].local_player->pos[1] = gstate.players[0].ride.exit_floor_y - 0.75F;
	gstate.players[0].local_player->pos_old[1] = gstate.players[0].ride.exit_floor_y - 1.0F;
	publish_occupancy(id, false, 0, 0, 1, seq + 1);
	float floor_y = cart->pos[1] + 2.2F;
	assert(nearly_equal(gstate.players[0].local_player->pos[1], floor_y));
	assert(nearly_equal(gstate.players[0].local_player->pos_old[1], floor_y));
	assert(!gstate.players[0].ride.exit_lift_pending && !gstate.players[0].ride.exit_repin_eligible);

	gstate.players[0].local_player->pos[1] = floor_y - 0.25F;
	gstate.players[0].local_player->pos_old[1] = floor_y - 0.25F;
	publish_occupancy(id, false, 0, 0, 1, seq + 1);
	assert(nearly_equal(gstate.players[0].local_player->pos[1], floor_y - 0.25F));
	destroy_client_entity(id);
}

/* ===================================================================== */
/* Test 8: camera-only reconciliation, frame timing, and wire feedback    */
/* ===================================================================== */
static void test_camera_reconciliation(uint32_t id) {
	printf("  test_camera_reconciliation: cart=%u\n", id);
	reset_ride_state();
	struct entity* cart = spawn_client_minecart(id, 12.5F, 101.0F, 12.5F);
	confirm_mount(cart, gstate.players[0].local_player->id);
	gstate.players[0].local_player->pos[0] = cart->pos[0];
	gstate.players[0].local_player->pos[1] = cart->pos[1] + 1.0F;
	gstate.players[0].local_player->pos[2] = cart->pos[2];
	glm_vec3_copy(gstate.players[0].local_player->pos, gstate.players[0].local_player->pos_old);
	camera_attach(&gstate.players[0].camera, gstate.players[0].local_player, 1.0F, 0.0F);
	float displayed_x = gstate.players[0].camera.x;

	/* A 0.35-block correction installs body/cart immediately but preserves the
	 * first displayed sample with an inverse camera tail. Zero, one, and three
	 * client catch-up ticks cannot consume rendered-frame convergence time. */
	send_snapshot(cart, 0.35F, 0.0F, 0.0F, true);
	assert(nearly_equal(gstate.players[0].local_player->pos[0], displayed_x + 0.35F));
	assert(nearly_equal(gstate.players[0].ride.camera_correction[0], -0.35F));
	assert(nearly_equal(gstate.players[0].ride.camera_correction_elapsed, 0.0F));
	assert_wire_matches_body("before first camera attach");
	for(int ticks = 0; ticks < 4; ticks++) {
		if(ticks == 1 || ticks > 1)
			tick_local_player();
		if(ticks == 0 || ticks == 1 || ticks == 3)
			assert(nearly_equal(gstate.players[0].ride.camera_correction_elapsed, 0.0F));
	}

	camera_attach(&gstate.players[0].camera, gstate.players[0].local_player, 1.0F, 0.19F);
	assert(nearly_equal(gstate.players[0].camera.x, displayed_x));
	assert(nearly_equal(gstate.players[0].ride.camera_correction_elapsed, 0.19F));
	assert_wire_matches_body("after first camera attach");
	camera_attach(&gstate.players[0].camera, gstate.players[0].local_player, 1.0F, 0.02F);
	assert(gstate.players[0].camera.x > displayed_x);
	assert(gstate.players[0].camera.x < gstate.players[0].local_player->pos[0]);
	assert(nearly_equal(gstate.players[0].ride.camera_correction_elapsed, 0.21F));
	camera_attach(&gstate.players[0].camera, gstate.players[0].local_player, 1.0F, 0.0F);
	assert(!gstate.players[0].ride.camera_correction_active);
	assert(nearly_equal(gstate.players[0].ride.camera_correction[0], 0.0F));
	assert(nearly_equal(gstate.players[0].camera.x, gstate.players[0].local_player->pos[0]));

	/* Deadband while a tail is active: apply the tiny body delta directly and
	 * retain the old tail. The no-tail case remains an exact body correction. */
	seed_camera_correction(-0.20F, 0.0F, 0.0F);
	gstate.players[0].camera.x = gstate.players[0].local_player->pos[0] - 0.20F;
	float camera_before = gstate.players[0].camera.x;
	send_snapshot(cart, 0.01F, 0.0F, 0.0F, true);
	assert(gstate.players[0].ride.camera_correction_active);
	assert(nearly_equal(gstate.players[0].ride.camera_correction[0], -0.20F));
	assert(nearly_equal(gstate.players[0].camera.x, camera_before + 0.01F));

	minecart_ride_client_state_reset(&gstate.players[0].ride, &gstate.players[0].camera, false);
	gstate.players[0].camera.x = gstate.players[0].local_player->pos[0];
	float raw_before = gstate.players[0].camera.x;
	send_snapshot(cart, 0.01F, 0.0F, 0.0F, true);
	assert(!gstate.players[0].ride.camera_correction_active);
	assert(nearly_equal(gstate.players[0].camera.x, raw_before + 0.01F));
	assert_wire_matches_body("deadband without tail");

	/* A raw correction over 0.50 blocks hard-snaps and clears any old tail. */
	seed_camera_correction(-0.10F, 0.0F, 0.0F);
	gstate.players[0].camera.x = gstate.players[0].local_player->pos[0] - 0.10F;
	send_snapshot(cart, 0.51F, 0.0F, 0.0F, true);
	assert(!gstate.players[0].ride.camera_correction_active);
	assert(nearly_equal(gstate.players[0].ride.camera_correction[0], 0.0F));
	assert(nearly_equal(gstate.players[0].camera.x, gstate.players[0].local_player->pos[0]));

	/* Two smoothable corrections arriving before an attach may accumulate past
	 * 0.50; that aggregate also hard-snaps instead of hiding a large offset. */
	minecart_ride_client_state_reset(&gstate.players[0].ride, &gstate.players[0].camera, false);
	gstate.players[0].camera.x = gstate.players[0].local_player->pos[0];
	send_snapshot(cart, 0.30F, 0.0F, 0.0F, true);
	assert(gstate.players[0].ride.camera_correction_active);
	assert(nearly_equal(gstate.players[0].ride.camera_correction[0], -0.30F));
	send_snapshot(cart, 0.25F, 0.0F, 0.0F, true);
	assert(!gstate.players[0].ride.camera_correction_active);
	assert(nearly_equal(gstate.players[0].camera.x, gstate.players[0].local_player->pos[0]));

	/* Ownership/session transition folds the applied tail out immediately. */
	seed_camera_correction(-0.20F, 0.0F, 0.0F);
	gstate.players[0].camera.x = gstate.players[0].local_player->pos[0] - 0.20F;
	publish_occupancy(id, false, 0, 0, 0, 0);
	assert(!cart->data.minecart.occupied);
	assert(!gstate.players[0].ride.camera_correction_active);
	assert(nearly_equal(gstate.players[0].camera.x, gstate.players[0].local_player->pos[0]));
	assert_wire_matches_body("after ownership transition");
	destroy_client_entity(id);
}

/* ===================================================================== */
/* Test 9: ridden cart entity eviction + spawn re-establishes occupancy   */
/* ===================================================================== */
static void test_ridden_cart_chunk_reload(uint32_t id) {
	printf("  test_ridden_cart_chunk_reload: cart=%u\n", id);
	reset_ride_state();
	/* Establish the pre-eviction ride through the reliable writer. This makes
	 * the negative control specific: disabling spawn occupancy still permits
	 * the initial mount, then fails only when reload must restore from spawn. */
	struct entity* cart = spawn_client_minecart(
		id, 16.5F, 101.0F, 16.5F);
	publish_occupancy(id, true, gstate.players[0].local_player->id, 41, 0, 0);
	assert(cart->data.minecart.occupied);
	assert(cart->data.minecart.occupant_id == gstate.players[0].local_player->id);
	assert(cart->data.minecart.ride_epoch == 41);
	assert(cart->data.minecart.control_ready);
	assert(strcmp(cart->rightClickText, "Dismount") == 0);

	gstate.players[0].local_player->pos[0] = 1.0F;
	gstate.players[0].local_player->pos[1] = 120.0F;
	gstate.players[0].local_player->pos[2] = 1.0F;
	tick_local_player();
	assert(nearly_equal(gstate.players[0].local_player->pos[0], cart->pos[0]));
	assert(nearly_equal(gstate.players[0].local_player->pos[1], cart->pos[1] + 1.0F));

	/* Chunk eviction destroys the client entity; its later spawn is the only
	 * occupancy re-install. There is deliberately no snapshot healing step. */
	gstate.players[0].ride.target_id = id;
	gstate.players[0].ride.pending = true;
	destroy_client_entity(id);
	assert(!dict_entity_get(gstate.entities, id));
	assert(!gstate.players[0].ride.pending && gstate.players[0].ride.target_id == 0);
	cart = spawn_client_minecart_state(
		id, 18.5F, 101.0F, 16.5F, true, gstate.players[0].local_player->id, 42);
	assert(cart->data.minecart.occupied);
	assert(cart->data.minecart.occupant_id == gstate.players[0].local_player->id);
	assert(cart->data.minecart.ride_epoch == 42);
	assert(cart->data.minecart.control_ready);
	assert(strcmp(cart->rightClickText, "Dismount") == 0);
	gstate.players[0].local_player->pos[0] = 2.0F;
	gstate.players[0].local_player->pos[1] = 120.0F;
	gstate.players[0].local_player->pos[2] = 2.0F;
	tick_local_player();
	assert(nearly_equal(gstate.players[0].local_player->pos[0], cart->pos[0]));
	assert(nearly_equal(gstate.players[0].local_player->pos[1], cart->pos[1] + 1.0F));

	/* A save/restored ride may have no preceding client intent. Once a
	 * snapshot creates presentation state, it must still become the ride
	 * state's target so entity eviction clears that correction. */
	send_snapshot(cart, 0.25F, 0.0F, 0.0F, true);
	assert(gstate.players[0].ride.camera_correction_active);
	assert(gstate.players[0].ride.target_id == id);
	destroy_client_entity(id);
	assert(!gstate.players[0].ride.camera_correction_active);
	assert(gstate.players[0].ride.target_id == 0);
}

static void assert_ride_fully_cleared(void) {
	assert(gstate.players[0].ride.target_id == 0);
	assert(gstate.players[0].ride.input_epoch == 0);
	assert(gstate.players[0].ride.next_request_seq == 0);
	assert(gstate.players[0].ride.pending_request_seq == 0);
	assert(!gstate.players[0].ride.send_dirty && !gstate.players[0].ride.pending);
	assert(!gstate.players[0].ride.exit_lift_pending);
	assert(!gstate.players[0].ride.exit_repin_eligible);
	assert(!gstate.players[0].ride.camera_correction_active);
	assert(nearly_equal(gstate.players[0].ride.camera_correction[0], 0.0F));
}

/* ===================================================================== */
/* Test 10: target/body/load/world lifecycle resets fold or clear safely  */
/* ===================================================================== */
static void test_ride_lifecycle_resets(uint32_t id) {
	printf("  test_ride_lifecycle_resets: cart=%u\n", id);
	reset_ride_state();
	struct entity* cart = spawn_client_minecart(id, 20.5F, 101.0F, 20.5F);
	gstate.players[0].ride.target_id = id;
	gstate.players[0].ride.next_request_seq = 9;
	gstate.players[0].ride.pending_request_seq = 9;
	gstate.players[0].ride.pending = true;
	seed_camera_correction(-0.20F, 0.0F, 0.0F);
	gstate.players[0].camera.x = 30.0F - 0.20F;
	destroy_client_entity(id);
	assert(!dict_entity_get(gstate.entities, id));
	assert(gstate.players[0].ride.next_request_seq == 9);
	assert(!gstate.players[0].ride.pending && gstate.players[0].ride.target_id == 0);
	assert(!gstate.players[0].ride.camera_correction_active);
	assert(nearly_equal(gstate.players[0].camera.x, 30.0F));
	(void)cart;

	/* Fresh-body handoff overwrites the camera first, then clears the old ride
	 * session without subtracting its dead-body presentation tail. */
	gstate.players[0].ride.target_id = 1234;
	gstate.players[0].ride.next_request_seq = 10;
	gstate.players[0].ride.pending = true;
	seed_camera_correction(-0.15F, 0.0F, 0.0F);
	gstate.players[0].camera.x = 30.0F - 0.15F;
	world_set_block(
		&gstate.world, 5, 105, 5,
		(struct block_data) {.type = BLOCK_STONE}, false);
	struct client_rpc fresh = {
		.type = CRPC_PLAYER_POS,
		.payload.player_pos = {
			.position = {5.5F, 105.0F, 5.5F},
			.input_epoch = 3, /* O07: Test 11 used 2 */
			.oxygen = MAX_OXYGEN,
			.selected_hotbar = 0,
			.fresh_body = true,
		},
	};
	clin_process(&fresh);
	assert_ride_fully_cleared();
	assert(nearly_equal(gstate.players[0].camera.x, 5.5F));
	assert(nearly_equal(gstate.players[0].camera.y, 105.0F));
	assert(nearly_equal(gstate.players[0].local_player->pos[0], 5.5F));
	clear_transport();

	/* Terminal load failure folds the currently applied tail, then clears all
	 * ride state. WORLD_RESET does the same before replacing world owners. */
	gstate.players[0].ride.target_id = 77;
	gstate.players[0].ride.next_request_seq = 11;
	gstate.players[0].ride.pending = true;
	seed_camera_correction(-0.10F, 0.0F, 0.0F);
	gstate.players[0].camera.x = 8.0F - 0.10F;
	struct client_rpc failed = {.type = CRPC_LOAD_FAILED};
	clin_process(&failed);
	assert_ride_fully_cleared();
	assert(nearly_equal(gstate.players[0].camera.x, 8.0F));

	gstate.players[0].ride.target_id = 88;
	gstate.players[0].ride.next_request_seq = 12;
	gstate.players[0].ride.pending = true;
	seed_camera_correction(-0.25F, 0.0F, 0.0F);
	gstate.players[0].camera.x = 9.0F - 0.25F;
	struct client_rpc reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {
			.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = SERVER_LOCAL_PLAYER_ENTITY_ID,
		},
	};
	clin_process(&reset);
	assert_ride_fully_cleared();
	assert(nearly_equal(gstate.players[0].camera.x, 9.0F));
	assert(gstate.players[0].local_player);
}


/* ===================================================================== */
/* Test 11 (D1, ASTRA-DEFECTS-RULING items 5-6): host ride-action        */
/* frame-to-tick handoff through the REAL production chain:              */
/*   agent bridge input_poll -> entity_local_player_ride_action_frame    */
/*   (the main.c per-frame hook) -> host entity_tick -> F3-1 JUMP site ->*/
/*   SRPC_MINECART_RIDE_INTENT(desired_occupied=false) via clin_update.  */
/* ===================================================================== */
static int d1_failures;
static size_t d1_dismounts;   /* desired=false ride intents for d1_cart */
static size_t d1_mounts;      /* desired=true ride intents (must stay 0) */
static size_t d1_online_uses; /* online Packet7 dismounts for d1_cart */
static uint32_t d1_cart;
static struct minecart_ride_client_state d1_p2_before;

#define D1_CHECK(cond, what) do { \
	if(!(cond)) { \
		fprintf(stderr, "    D1 FAIL: %s (%s:%d)\n", what, __func__, __LINE__); \
		d1_failures++; \
	} \
} while(0)

static void d1_drain(void) {
	struct transport_capture cap = {0};
	clin_update();
	drain_all_transport(&cap);
	for(size_t i = 0; i < cap.count; i++) {
		if(cap.calls[i].type == SRPC_ENTITY_USE
		   && cap.calls[i].payload.entity_use.entity_id == d1_cart)
			d1_online_uses++;
		if(cap.calls[i].type != SRPC_MINECART_RIDE_INTENT)
			continue;
		if(cap.calls[i].payload.minecart_ride_intent.desired_occupied)
			d1_mounts++;
		else if(cap.calls[i].payload.minecart_ride_intent.entity_id == d1_cart)
			d1_dismounts++;
	}
}

/* One 60 Hz frame in the main.c order. The input_poll that ENDED the
 * previous frame publishes this frame's edge (the agent bridge is the
 * production input_pressed source in this build); clin_update runs; the
 * per-frame ride-action hook runs; then `ticks` host ticks (0 = a zero-tick
 * frame, >1 = catch-up), each through the production tick_client, with
 * capture_input = `captured` (false = menu/focus/controller loss). */
static void d1_frame(bool jump, int ticks, bool captured) {
	agent_input_feed_line(jump ? "JUMP=1" : "JUMP=0");
	agent_input_poll();
	struct entity* lp = gstate.players[0].local_player;
	lp->data.local_player.capture_input = captured;
	d1_drain();
	entity_local_player_ride_action_frame();
	for(int t = 0; t < ticks; t++) {
		assert(lp->tick_client);
		lp->tick_client(lp);
	}
	entity_local_player_ride_action_frame_end(); /* main.c, before input_poll */
	d1_drain();
}

/* A frame whose JUMP edge is raised AFTER the tick section (how the vehicle
 * smoke driver and the autopilot script apply host buttons: mid-frame, after
 * the catch-up). Only the end-of-frame capture can see it. */
static void d1_frame_late_edge(int ticks) {
	agent_input_feed_line("JUMP=0");
	agent_input_poll();
	struct entity* lp = gstate.players[0].local_player;
	lp->data.local_player.capture_input = true;
	d1_drain();
	entity_local_player_ride_action_frame();
	for(int t = 0; t < ticks; t++)
		lp->tick_client(lp);
	agent_input_feed_line("JUMP=1"); /* the mid-frame producer */
	agent_input_poll();
	entity_local_player_ride_action_frame_end();
	d1_drain();
}

static struct entity* d1_ride(uint32_t id) {
	/* Neutral frames first so no earlier edge is visible. */
	agent_input_feed_line("JUMP=0");
	agent_input_poll();
	agent_input_poll();
	reset_ride_state();
	clear_transport();
	struct entity* lp = gstate.players[0].local_player;
	lp->health = 20;
	gstate.paused = false;
	struct entity* cart = spawn_client_minecart(id, 4.5f, 101.0f, 4.5f);
	confirm_mount(cart, lp->id);
	assert(cart->data.minecart.occupied
		   && cart->data.minecart.occupant_id == lp->id);
	lp->pos[0] = cart->pos[0];
	lp->pos[1] = cart->pos[1] + 1.0f;
	lp->pos[2] = cart->pos[2];
	d1_cart = id;
	d1_dismounts = d1_mounts = d1_online_uses = 0;
	d1_p2_before = gstate.players[1].ride;
	/* One settled ridden tick with no input. */
	d1_frame(false, 1, true);
	assert(d1_dismounts == 0);
	/* O07 / R04 coverage: every case starts from LIVE, eligible gameplay, so
	 * no negative below can pass vacuously on a dead gate. */
	lp->data.local_player.capture_input = true;
	D1_CHECK(entity_local_player_ride_action_eligible(),
			 "case setup: host ride action eligible in live gameplay");
	return cart;
}

static void d1_end(const char* name, struct entity* cart, size_t want_dismounts) {
	char what[160];
	snprintf(what, sizeof what, "%s: exactly %zu dismount intent(s), got %zu",
			 name, want_dismounts, d1_dismounts);
	D1_CHECK(d1_dismounts == want_dismounts, what);
	snprintf(what, sizeof what, "%s: no mount/replay intent (got %zu)",
			 name, d1_mounts);
	D1_CHECK(d1_mounts == 0, what);
	if(want_dismounts) {
		snprintf(what, sizeof what, "%s: cart predicted detached", name);
		D1_CHECK(!cart->data.minecart.occupied, what);
		snprintf(what, sizeof what, "%s: ride intent is a leave", name);
		D1_CHECK(gstate.players[0].ride.pending
				 && !gstate.players[0].ride.desired_occupied
				 && gstate.players[0].ride.target_id == d1_cart, what);
	} else {
		snprintf(what, sizeof what, "%s: host still riding", name);
		D1_CHECK(cart->data.minecart.occupied
				 && cart->data.minecart.occupant_id
					== gstate.players[0].local_player->id, what);
	}
	snprintf(what, sizeof what, "%s: no pending latch left", name);
	D1_CHECK(!gstate.players[0].ride.action_latched, what);
	snprintf(what, sizeof what, "%s: P2 session untouched", name);
	D1_CHECK(!memcmp(&d1_p2_before, &gstate.players[1].ride,
					 sizeof d1_p2_before)
			 && !gstate.players[1].local_player, what);
	destroy_client_entity(cart->id);
	d1_frame(false, 1, true);
	gstate.players[0].local_player->health = 20;
	gstate.paused = false;
}

/* 60 Hz / 20 TPS: one tick every third frame. `phase` picks which frame of
 * the 3-cycle carries the tick relative to the press frame (frame 1): phase 2
 * = tick in the press frame itself, 0 = one frame later, 1 = two frames
 * later. The press is a SHORT TAP: down for frame 1 only, released at frame 2
 * -- i.e. released before consumption whenever the press frame runs no tick. */
static void d1_phase_offset(uint32_t id, int phase) {
	struct entity* cart = d1_ride(id);
	for(int f = 0; f < 9; f++)
		d1_frame(f == 1, ((f + phase) % 3 == 0) ? 1 : 0, true);
	char name[48];
	snprintf(name, sizeof name, "phase offset %d", phase);
	d1_end(name, cart, 1);
}

static void test_d1_ride_action_handoff(void) {
	printf("  test_d1_ride_action_handoff\n");
	struct entity* cart;

	/* (1) All three 60 Hz / 20 TPS phase offsets, short tap. */
	d1_phase_offset(1200, 0);
	d1_phase_offset(1201, 1);
	d1_phase_offset(1202, 2);

	/* (2) Zero-tick press released before consumption, then SEVERAL
	 * zero-tick frames (a >60 Hz host), then ticks. */
	cart = d1_ride(1203);
	d1_frame(true, 0, true);
	for(int f = 0; f < 6; f++)
		d1_frame(false, 0, true);
	D1_CHECK(d1_dismounts == 0, "multi zero-tick: nothing before a tick");
	d1_frame(false, 1, true);
	d1_frame(false, 1, true);
	d1_end("multi zero-tick", cart, 1);

	/* (3) Catch-up: zero-tick press, then a 3-tick catch-up frame. */
	cart = d1_ride(1204);
	d1_frame(true, 0, true);
	d1_frame(false, 3, true);
	d1_end("catch-up after zero-tick press", cart, 1);

	/* (3b) Press inside a 3-tick catch-up frame: the same edge is visible to
	 * every tick of the frame but must act once. */
	cart = d1_ride(1205);
	d1_frame(true, 3, true);
	d1_frame(false, 1, true);
	d1_end("press inside catch-up frame", cart, 1);

	/* (4) Held: JUMP down for 12 frames across ticks = one edge, one
	 * dismount, and no later re-trigger while still held. */
	cart = d1_ride(1206);
	for(int f = 0; f < 12; f++)
		d1_frame(true, (f % 3 == 1) ? 1 : 0, true);
	d1_frame(false, 1, true);
	d1_end("held", cart, 1);

	/* (5) Repeated presses before consumption (tap, release, tap in
	 * zero-tick frames) = one dismount; a later press after the dismount
	 * (no longer riding) authors nothing. */
	cart = d1_ride(1207);
	d1_frame(true, 0, true);
	d1_frame(false, 0, true);
	d1_frame(true, 0, true);
	d1_frame(false, 1, true);
	d1_frame(false, 1, true);
	d1_frame(true, 1, true);
	d1_frame(false, 1, true);
	d1_end("repeated presses", cart, 1);

	/* (6) Lifecycle invalidation: a zero-tick press, then before any tick
	 * consumes it the input/ride lifetime ends -> never acted on later. */
	/* (6a) menu/focus/controller loss: capture_input drops for a frame. */
	cart = d1_ride(1208);
	d1_frame(true, 0, true);
	d1_frame(false, 0, false);
	d1_frame(false, 1, true);
	d1_frame(false, 1, true);
	d1_end("menu/focus loss", cart, 0);

	/* (6b) death. */
	cart = d1_ride(1209);
	d1_frame(true, 0, true);
	gstate.players[0].local_player->health = 0;
	d1_frame(false, 0, true);
	gstate.players[0].local_player->health = 20;
	d1_frame(false, 1, true);
	d1_frame(false, 1, true);
	d1_end("death", cart, 0);

	/* (6c) pause. */
	cart = d1_ride(1210);
	d1_frame(true, 0, true);
	gstate.paused = true;
	d1_frame(false, 0, true);
	gstate.paused = false;
	d1_frame(false, 1, true);
	d1_frame(false, 1, true);
	d1_end("pause", cart, 0);

	/* (6d) ride teardown/replacement: the ridden cart is destroyed and the
	 * host is put on a DIFFERENT cart (server-published occupancy, no client
	 * intent, so no request-seq bump) before any tick -- the press must not
	 * escape into that later ride. */
	cart = d1_ride(1211);
	d1_frame(true, 0, true);
	destroy_client_entity(1211);
	struct entity* next = spawn_client_minecart(1212, 4.5f, 101.0f, 4.5f);
	confirm_mount(next, gstate.players[0].local_player->id);
	d1_cart = 1212;
	d1_frame(false, 1, true);
	d1_frame(false, 1, true);
	d1_end("teardown into a later ride", next, 0);
	(void)cart;

	/* (6e) body replacement through the REAL handler (O07 / R04 coverage:
	 * no direct helper call): a fresh-body CRPC_PLAYER_POS handoff between
	 * the zero-tick tap and the next tick runs the production ride reset. */
	cart = d1_ride(1213);
	d1_frame(true, 0, true);
	D1_CHECK(gstate.players[0].ride.action_latched,
			 "fresh-body: the zero-tick tap is latched before the handoff");
	{
		struct client_rpc fresh = {
			.type = CRPC_PLAYER_POS,
			.payload.player_pos = {
				.position = {4.5F, 102.0F, 4.5F},
				.input_epoch = 2,
				.oxygen = MAX_OXYGEN,
				.selected_hotbar = 0,
				.fresh_body = true,
			},
		};
		clin_process(&fresh);
		/* finish the handoff exactly as the harness setup does (input
		 * readiness ACK), so the following cases run in live gameplay */
		screen_load_world.update(&screen_load_world, 30.0f);
		clin_update_input_readiness();
		clear_transport();
		clin_update();
		clear_transport();
	}
	D1_CHECK(!gstate.players[0].ride.action_latched,
			 "fresh-body: the real handoff handler clears the latch");
	confirm_mount(cart, gstate.players[0].local_player->id);
	gstate.players[0].local_player->pos[0] = cart->pos[0];
	gstate.players[0].local_player->pos[1] = cart->pos[1] + 1.0f;
	gstate.players[0].local_player->pos[2] = cart->pos[2];
	d1_frame(false, 1, true);
	d1_frame(false, 1, true);
	d1_end("fresh-body ride reset", cart, 0);

	/* (6f) O07 / R04 D1-1: an AUTHORITATIVE end and restart of the SAME
	 * cart's ride between the tap and the tick -- the real occupancy
	 * handler delivers empty/epoch 0, then occupied/epoch 3 (unsolicited
	 * zero/zero correlation). Cart id, input epoch and request seq are all
	 * unchanged; the tap belongs to the ended ride and must not dismount the
	 * new one. */
	cart = d1_ride(1217);
	d1_frame(true, 0, true);
	publish_occupancy(cart->id, false, 0, 0, 0, 0);
	publish_occupancy(cart->id, true, gstate.players[0].local_player->id, 3,
					  0, 0);
	d1_frame(false, 1, true);
	d1_frame(false, 1, true);
	d1_end("authoritative end/restart of the same cart's ride", cart, 0);

	/* (6g) O07 / R04 D1-1: the latched cart is destroyed (real
	 * CRPC_ENTITY_DESTROY) -- the latch goes with it. */
	cart = d1_ride(1218);
	d1_frame(true, 0, true);
	destroy_client_entity(1218);
	D1_CHECK(!gstate.players[0].ride.action_latched,
			 "destroy: the latched cart's destruction clears the latch");
	cart = spawn_client_minecart(1218, 4.5f, 101.0f, 4.5f);
	confirm_mount(cart, gstate.players[0].local_player->id);
	d1_frame(false, 1, true);
	d1_end("latched cart destroyed and respawned", cart, 0);

	/* (6h) O07 / R04 D1-2: controller loss published by the input layer
	 * BEFORE recovery refreshes capture_input. The zero-tick tap is latched;
	 * the next poll publishes player 0's owned device as DISCONNECTED (a real
	 * input_player_bind whose device has not been polled live: the PC build
	 * binds the keyboard, the Wii build a WPAD/GameCube channel), while the body's cached
	 * capture_input is still true. Neither capture nor the tick may consume
	 * the old tap. */
	cart = d1_ride(1219);
	d1_frame(true, 0, true);
	D1_CHECK(input_player_bind(0, (struct input_device) {INPUT_DEVICE_KEYBOARD, 0}),
			 "controller loss: player 0 owns a device");
	{
		const struct input_player_frame* owner = input_player_frame(0);
		D1_CHECK(owner && !owner->connected,
				 "controller loss: the owned frame reads disconnected");
	}
	d1_frame(false, 1, true);
	d1_frame(false, 1, true);
	input_player_unbind(0);
	d1_end("controller loss before recovery", cart, 0);

	/* (6i) O07 / R04 D1-3: the begin hook latched this frame's edge, then
	 * the SAME frame's screen section dropped input capture (a menu or
	 * recovery opened) before the end hook. The end hook must still clear
	 * the now-invalid latch, not only skip the duplicate capture. */
	cart = d1_ride(1220);
	agent_input_feed_line("JUMP=1");
	agent_input_poll();
	gstate.players[0].local_player->data.local_player.capture_input = true;
	d1_drain();
	entity_local_player_ride_action_frame(); /* begin: latched */
	gstate.players[0].local_player->data.local_player.capture_input = false;
	entity_local_player_ride_action_frame_end();
	D1_CHECK(!gstate.players[0].ride.action_latched,
			 "end-of-press-frame invalidation: the end hook clears it");
	d1_drain();
	d1_frame(false, 1, true);
	d1_frame(false, 1, true);
	d1_end("ineligible later in the press frame", cart, 0);

	/* (7) Late edge: raised after the tick section of its frame (a frame with
	 * a tick, and one without), held afterwards -> exactly one dismount on
	 * the next tick. */
	cart = d1_ride(1214);
	d1_frame_late_edge(1);
	d1_frame(true, 0, true);
	d1_frame(true, 1, true);
	d1_frame(false, 1, true);
	d1_end("late (post-tick) edge, tick frame", cart, 1);
	cart = d1_ride(1215);
	d1_frame_late_edge(0);
	d1_frame(true, 1, true);
	d1_frame(false, 1, true);
	d1_end("late (post-tick) edge, zero-tick frame", cart, 1);

	/* (8) Online (remote session): the dismount is ONE Packet7 use-entity and
	 * the cart stays occupied until the server answers, so the same frame
	 * edge must never be latched twice -- by the catch-up ticks or by the
	 * end-of-frame capture -- or the second Packet7 would toggle the rider
	 * straight back on. */
	cart = d1_ride(1216);
	gstate.remote_session = true;
	d1_frame(true, 3, true);
	d1_frame(true, 1, true);
	d1_frame(false, 1, true);
	gstate.remote_session = false;
	{
		char what[120];
		snprintf(what, sizeof what,
				 "online: exactly one Packet7 dismount, got %zu", d1_online_uses);
		D1_CHECK(d1_online_uses == 1, what);
	}
	d1_end("online single Packet7", cart, 0);

	fprintf(stderr, "  test_d1_ride_action_handoff: %d failure(s)\n", d1_failures);
	assert(d1_failures == 0);
}

/* ===================================================================== */
/* main                                                                   */
/* ===================================================================== */
int main(void) {
	static char env[] = "CAVEWII_AGENT=1";
	putenv(env);
	assert(agent_input_armed());

	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));

	blocks_init();
	items_init();

	char s_fixture_TMP_WORLD[128];
	test_fixture_dir("test_minecart_client_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	MKDIR(s_fixture_TMP_WORLD);
	MKDIR(test_fixture_join(s_fixture_TMP_WORLD, "/region"));
	remove(test_fixture_join(s_fixture_TMP_WORLD, "/region/r.0.0.mcr"));

	string_t level;
	string_init_set_str(level, s_fixture_TMP_WORLD);

	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	string_clear(level);
	s.world.local = &s;
	s.worlds[0].session = &s; /* A1a (D1): owning session for services/callbacks */
	s.input_epoch_counter = 1;

	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();

	struct client_rpc client_reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {
			.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = SERVER_LOCAL_PLAYER_ENTITY_ID,
		},
	};
	clin_process(&client_reset);
	assert(gstate.players[0].local_player);
	world_set_block(
		&gstate.world, 4, 103, 4,
		(struct block_data) {.type = BLOCK_STONE}, false);
	struct client_rpc handoff = {
		.type = CRPC_PLAYER_POS,
		.payload.player_pos = {
			.position = {4.5F, 103.0F, 4.5F},
			.input_epoch = 1,
			.oxygen = MAX_OXYGEN,
			.selected_hotbar = 0,
		},
	};
	clin_process(&handoff);
	assert(gstate.world_loaded);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	clear_transport();
	clin_update();
	clear_transport();

	printf("test_minecart_client: starting\n");

	printf("== Test 1: A-B/JUMP pre-selection ==\n");
	test_ab_jump_preselect(100, 200);
	test_ab_jump_preselect(200, 100);
	test_ab_jump_preselect(10, 60000);
	test_ab_jump_preselect(60000, 10);

	printf("== Test 2: Epoch-zero mount + JUMP ==\n");
	test_epoch_zero_jump(300);
	test_epoch_zero_jump(301);

	printf("== Test 4: Client rollback on correlated rejection ==\n");
	test_correlated_rejection_rollback(500);
	test_correlated_rejection_rollback(501);

	printf("== Test 5: Stale-fence while pending + correlated empty ==\n");
	test_stale_fence_and_retire(600);
	test_stale_fence_and_retire(601);

	printf("== Test 6: clin_update transport rejection + retry ==\n");
	test_clin_update_retry_after_transport_rejection();

	printf("== Test 7: Pending-leave snapshots + exit lift ==\n");
	test_pending_leave_and_exit_lift(800);

	printf("== Test 8: Camera reconciliation + wire feedback ==\n");
	test_camera_reconciliation(900);

	printf("== Test 9: Ridden-cart chunk reload ==\n");
	test_ridden_cart_chunk_reload(1000);

	printf("== Test 11 (D1): host ride-action frame-to-tick handoff ==\n");
	test_d1_ride_action_handoff();

	printf("== Test 10: Ride lifecycle resets ==\n");
	test_ride_lifecycle_resets(1100);

	g_drain_run = 0;
	thread_join(&drain);
	clin_drain_discard();
	for(size_t i = 0; i < 256; i++) {
		if(gstate.players[0].windows[i]) {
			windowc_destroy(gstate.players[0].windows[i]);
			free(gstate.players[0].windows[i]);
			gstate.players[0].windows[i] = NULL;
		}
	}
	dict_entity_it_t client_it;
	dict_entity_it(client_it, gstate.entities);
	while(!dict_entity_end_p(client_it)) {
		free(dict_entity_ref(client_it)->value);
		dict_entity_next(client_it);
	}
	gstate.players[0].local_player = NULL;
	dict_entity_clear(gstate.entities);
	world_destroy(&gstate.world);
	server_world_destroy(&s.world);
	dict_entity_it_t server_it;
	dict_entity_it(server_it, s.entities);
	while(!dict_entity_end_p(server_it)) {
		free(dict_entity_ref(server_it)->value);
		dict_entity_next(server_it);
	}
	dict_entity_clear(s.entities);

	printf("test_minecart_client: all tests passed\n");
	test_fixture_dir_cleanup(s_fixture_TMP_WORLD);
	return 0;
}
