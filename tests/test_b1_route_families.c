/* Split-dims B1 phase 2: the REAL client inbox consumer (clin_process /
 * clin_update_drain in source/network/client_interface.c) wired to the client
 * world contexts. One section per wiring commit, in out/B1-rpc-audit.md's
 * order:
 *  - lifecycle: CRPC_WORLD_RESET installs a new context (bound to the
 *    payload's server identity, or to the unstamped stream), retiring the
 *    predecessor, which stays owned only while it still holds leases.
 * Every later section drives one RPC family through the same consumer: before
 * any world exists (NO_CONTEXT: dropped), on an unstamped world (applied), on
 * a stamped world (current generation applied, any other dropped as STALE),
 * and while the context quiesces (the drain loop HOLDS the message, in FIFO
 * order, until the owner poll completes the quiesce). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "game/sign_text_store.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "particle.h"
#include "world.h"

#define LOCAL_EID 77

static struct client_ctx_registry* reg;

static void world_reset(uint8_t index, uint32_t generation) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = LOCAL_EID, .world_index = index,
			.world_generation = generation}});
}

/* Start a section with no client world context at all (lease-free contexts
 * are reclaimed on retirement). */
static void no_world(void) {
	client_ctx_registry_retire(reg, 0);
	client_ctx_registry_retire(reg, 1);
	assert(client_ctx_registry_in_use(reg) == 0);
	assert(!client_ctx_registry_current(reg, 0));
}

static void test_lifecycle(void) {
	no_world();
	world_reset(0, 0);
	struct client_world_context* a = client_ctx_registry_current(reg, 0);
	assert(a && a->unstamped && a->server_generation == 0);
	assert(a->bound_views == 1u);
	assert(client_ctx_registry_in_use(reg) == 1);

	world_reset(0, 42);
	struct client_world_context* b = client_ctx_registry_current(reg, 0);
	assert(b && !b->unstamped && b->server_generation == 42);
	assert(client_ctx_registry_in_use(reg) == 1
		   && "a lease-free predecessor is reclaimed at the reset");

	/* A predecessor with an in-flight mesher lease survives the reset,
	 * retired, until that lease comes back against IT. */
	assert(client_ctx_lease_acquire(b, CLIENT_CTX_LEASE_MESHER));
	uint32_t b_epoch = b->generation;
	world_reset(0, 43);
	struct client_world_context* c = client_ctx_registry_current(reg, 0);
	assert(c && c != b && c->server_generation == 43);
	assert(b->retiring && client_ctx_registry_owns(reg, b));
	assert(client_ctx_registry_in_use(reg) == 2);
	assert(!client_ctx_accept(c, 0, b_epoch));
	assert(client_ctx_registry_release(reg, b, CLIENT_CTX_LEASE_MESHER));
	assert(!client_ctx_registry_owns(reg, b)
		   && client_ctx_registry_in_use(reg) == 1);
	puts("PASS lifecycle: WORLD_RESET installs, retires and reclaims contexts");
}

static struct client_rpc smoke(uint8_t index, uint32_t generation) {
	/* Near the (default, origin) particle camera: particle_add culls
	 * anything past its spawn radius. */
	return (struct client_rpc) {.type = CRPC_SMOKE_PUFF,
		.world_index = index, .world_generation = generation,
		.payload.smoke_puff = {.pos = {0.5F, 0.5F, 0.5F}, .intensity = 1.0F}};
}

/* Cosmetic family: one SMOKE_PUFF per step, observed as a particle count. */
static void test_cosmetic_family(void) {
	/* No world: dropped as NO_CONTEXT. */
	no_world();
	particle_clear_all();
	uint32_t no_ctx = reg->routed[CLIENT_CTX_ROUTE_NO_CONTEXT];
	struct client_rpc m = smoke(0, 0);
	clin_process(&m);
	assert(particle_test_count() == 0);
	assert(reg->routed[CLIENT_CTX_ROUTE_NO_CONTEXT] == no_ctx + 1);

	/* Unstamped world: unstamped traffic applies. */
	world_reset(0, 0);
	m = smoke(0, 0);
	clin_process(&m);
	size_t after_unstamped = particle_test_count();
	assert(after_unstamped > 0);

	/* Stamped world 9: its own generation applies, a retired one is STALE. */
	world_reset(0, 9);
	particle_clear_all();
	m = smoke(0, 9);
	clin_process(&m);
	size_t after_current = particle_test_count();
	assert(after_current > 0);
	uint32_t stale = reg->routed[CLIENT_CTX_ROUTE_STALE];
	m = smoke(0, 8);
	clin_process(&m);
	assert(particle_test_count() == after_current);
	assert(reg->routed[CLIENT_CTX_ROUTE_STALE] == stale + 1);
	m = smoke(1, 9);
	clin_process(&m);
	assert(particle_test_count() == after_current
		   && "a slot with no context never receives another world's effect");

	/* Quiescing: the drain loop HOLDS the message (FIFO), neither applying
	 * nor dropping it, until the lease drains and the owner poll completes. */
	struct client_world_context* ctx = client_ctx_registry_current(reg, 0);
	assert(ctx && client_ctx_lease_acquire(ctx, CLIENT_CTX_LEASE_MESHER));
	assert(client_ctx_reset(ctx) && ctx->quiescing);
	particle_clear_all();
	m = smoke(0, 9);
	clin_rpc_send(&m);
	clin_update_drain();
	assert(clin_world_message_held() && particle_test_count() == 0);
	clin_update_drain(); /* still parked: still held, still not applied */
	assert(clin_world_message_held() && particle_test_count() == 0);
	assert(clin_world_quiesce_pending());
	assert(client_ctx_registry_release(reg, ctx, CLIENT_CTX_LEASE_MESHER));
	clin_update_drain();
	assert(!clin_world_message_held() && !clin_world_quiesce_pending());
	assert(particle_test_count() > 0);
	puts("PASS cosmetic family: no-context drop, unstamped + current apply, "
		 "stale drop, hold while quiescing");
}

static struct client_rpc spawn_item(uint32_t id, uint8_t index,
									uint32_t generation) {
	return (struct client_rpc) {.type = CRPC_SPAWN_ITEM,
		.world_index = index, .world_generation = generation,
		.payload.spawn_item = {.entity_id = id,
			.item = {.id = 1, .durability = 0, .count = 1},
			.pos = {0.5F, 70.0F, 0.5F}}};
}

static bool has_entity(uint32_t id) {
	struct entity** pp = dict_entity_get(gstate.entities, id);
	return pp && *pp;
}

/* Entity family: SPAWN_ITEM creates a client entity; ENTITY_DESTROY of a
 * foreign world must not remove this world's entity. */
static void test_entity_family(void) {
	no_world();
	struct client_rpc m = spawn_item(501, 0, 0);
	clin_process(&m);
	assert(!has_entity(501) && "no world: a spawn is dropped");

	world_reset(0, 0);
	m = spawn_item(502, 0, 0);
	clin_process(&m);
	assert(has_entity(502) && "unstamped world: an unstamped spawn applies");

	world_reset(0, 21);
	m = spawn_item(503, 0, 21);
	clin_process(&m);
	assert(has_entity(503) && "the current generation's spawn applies");
	m = spawn_item(504, 0, 20);
	clin_process(&m);
	assert(!has_entity(504) && "a retired generation's spawn is stale");
	m = (struct client_rpc) {.type = CRPC_ENTITY_DESTROY,
		.world_index = 0, .world_generation = 20,
		.payload.entity_destroy = {.entity_id = 503}};
	clin_process(&m);
	assert(has_entity(503)
		   && "a stale destroy never removes the current world's entity");
	m.world_generation = 21;
	clin_process(&m);
	assert(!has_entity(503) && "the current world's destroy applies");
	puts("PASS entity family: no-context drop, unstamped + current apply, "
		 "stale spawn and stale destroy dropped");
}

static void time_set(uint64_t t, uint8_t index, uint32_t generation) {
	struct client_rpc m = {.type = CRPC_TIME_SET, .world_index = index,
		.world_generation = generation, .payload.time_set = t};
	clin_process(&m);
}

/* Time family: each world context owns its clock, so a TIME_SET for another
 * (or a retired) world never moves this world's time. */
static void test_time_family(void) {
	no_world();
	/* Fable F3: an UNSTAMPED clock/weather update before any client world
	 * context (remote MP: Packet4/Packet70 ahead of the login reset) keeps
	 * its pre-B1 behaviour and applies; a STAMPED one has a named world and
	 * is dropped when that world has no context. */
	gstate.world_time = 1;
	time_set(1000, 0, 0);
	assert(gstate.world_time == 1000
		   && "no world: an unstamped clock update applies as pre-B1");
	time_set(1500, 0, 7);
	assert(gstate.world_time == 1000
		   && "no world: a stamped clock update is dropped");
	gstate.weather_raining = false;
	struct client_rpc w = {.type = CRPC_WEATHER_SET,
		.payload.weather_set = {.raining = true, .rain_strength = 1.0F}};
	clin_process_direct(&w);
	assert(gstate.weather_raining && gstate.weather_rain_strength == 1.0F
		   && "no world: unstamped weather (remote login order) applies");

	world_reset(0, 0);
	time_set(2000, 0, 0);
	assert(gstate.world_time == 2000 && "unstamped world: unstamped time applies");

	world_reset(0, 61);
	time_set(3000, 0, 61);
	assert(gstate.world_time == 3000 && "the current generation's time applies");
	time_set(4000, 0, 60);
	assert(gstate.world_time == 3000 && "a retired generation's time is stale");
	time_set(5000, 1, 61);
	assert(gstate.world_time == 3000
		   && "a world with no client context never sets this clock");
	puts("PASS time family: unstamped pre-world update applies (F3), stamped pre-world dropped, unstamped + current apply, "
		 "stale and foreign-slot updates dropped");
}

static struct client_rpc sign(int32_t x, uint8_t index, uint32_t generation) {
	struct client_rpc m = {.type = CRPC_SIGN_TEXT,
		.world_index = index, .world_generation = generation,
		.payload.sign_text = {.x = x, .y = 64, .z = 0}};
	memcpy(m.payload.sign_text.grid, "B1 terrain route", 16);
	return m;
}

static bool has_sign(int32_t x) {
	char grid[64];
	return sign_text_store_get(x, 64, 0, grid);
}

/* Terrain family through the consumer: SIGN_TEXT (a terrain-scoped payload
 * store) is dropped without a world, applied for the current generation,
 * dropped when stale and held while the context quiesces. */
static void test_terrain_family(void) {
	no_world();
	sign_text_store_clear();
	struct client_rpc m = sign(1, 0, 0);
	clin_process(&m);
	assert(!has_sign(1) && "no world: terrain is dropped");

	world_reset(0, 0);
	m = sign(2, 0, 0);
	clin_process(&m);
	assert(has_sign(2) && "unstamped world: unstamped terrain applies");

	world_reset(0, 31);
	m = sign(3, 0, 31);
	clin_process(&m);
	assert(has_sign(3) && "the current generation's terrain applies");
	m = sign(4, 0, 30);
	clin_process(&m);
	assert(!has_sign(4) && "a retired generation's terrain is stale");

	struct client_world_context* ctx = client_ctx_registry_current(reg, 0);
	assert(ctx && client_ctx_lease_acquire(ctx, CLIENT_CTX_LEASE_GPU));
	assert(client_ctx_reset(ctx));
	m = sign(5, 0, 31);
	clin_rpc_send(&m);
	clin_update_drain();
	assert(clin_world_message_held() && !has_sign(5));
	assert(client_ctx_registry_release(reg, ctx, CLIENT_CTX_LEASE_GPU));
	clin_update_drain();
	assert(!clin_world_message_held() && has_sign(5));
	puts("PASS terrain family: no-context drop, unstamped + current apply, "
		 "stale drop, hold while quiescing");
}

/* The world lease seam the mesher and the GPU hand-off use (world.h): a lease
 * taken against the current context publishes only while that context and
 * its epoch are still current, and always goes back to the exact context it
 * came from -- across a successor install and across a scoped reset. */
static void test_world_lease_owner(void) {
	no_world();
	world_reset(0, 51);
	const struct world_lease_owner* owner = gstate.world.lease_owner;
	assert(owner && "WORLD_RESET installs the lease owner on the client world");

	struct world_lease lease = {0};
	assert(owner->acquire(owner->user, WORLD_LEASE_MESHER, &lease) && lease.held);
	assert(owner->release(owner->user, WORLD_LEASE_MESHER, &lease)
		   && "a lease given back under the same epoch may publish");
	assert(!lease.held);

	/* Successor install: the predecessor's completion may not publish, the
	 * lease still goes back to (and reclaims) the predecessor. */
	assert(owner->acquire(owner->user, WORLD_LEASE_MESHER, &lease));
	struct client_world_context* pred = lease.ctx;
	world_reset(0, 52);
	assert(client_ctx_registry_owns(reg, pred) && pred->retiring);
	assert(!owner->release(owner->user, WORLD_LEASE_MESHER, &lease)
		   && "a replaced world's completion may not publish");
	assert(!client_ctx_registry_owns(reg, pred)
		   && "the predecessor is reclaimed on its last lease");

	/* Scoped reset: producers parked while the old lease is out; the old
	 * completion may not publish; after the drain + owner poll they resume. */
	assert(owner->acquire(owner->user, WORLD_LEASE_GPU, &lease));
	struct client_world_context* cur = client_ctx_registry_current(reg, 0);
	assert(client_ctx_reset(cur) && cur->quiescing);
	struct world_lease parked = {0};
	assert(!owner->acquire(owner->user, WORLD_LEASE_MESHER, &parked)
		   && "a parked context grants no mesher lease");
	assert(!owner->release(owner->user, WORLD_LEASE_GPU, &lease)
		   && "an old-epoch completion may not publish after a reset");
	clin_update_drain();
	assert(!cur->quiescing);
	assert(owner->acquire(owner->user, WORLD_LEASE_MESHER, &parked));
	assert(owner->release(owner->user, WORLD_LEASE_MESHER, &parked));
	puts("PASS world lease owner: publish only under the current epoch, "
		 "release to the originating context, park during a quiesce");
}

/* Lead ruling Option A: CRPC_STREAM_RESET with a mesher job still pinned (its
 * lease held on the current context) installs a SUCCESSOR bound to the same
 * server identity. Terrain for the new stream applies at once; the pinned
 * job's completion releases against the predecessor and may not publish; the
 * predecessor stays owned until that release, then is reclaimed. */
static void test_stream_reset_reinstall_while_pinned(void) {
	no_world();
	sign_text_store_clear();
	world_reset(0, 71);
	const struct world_lease_owner* owner = gstate.world.lease_owner;
	struct client_world_context* pred = client_ctx_registry_current(reg, 0);
	assert(owner && pred);
	struct world_lease pinned = {0};
	assert(owner->acquire(owner->user, WORLD_LEASE_MESHER, &pinned));
	assert(pinned.ctx == pred);

	clin_test_arm_stream_reset();
	struct client_rpc reset = {.type = CRPC_STREAM_RESET};
	clin_process(&reset);
	struct client_world_context* succ = client_ctx_registry_current(reg, 0);
	assert(succ && succ != pred && "the re-stream installs a successor");
	assert(succ->server_generation == 71 && !succ->quiescing
		   && "same server identity, never parked");
	assert(succ->generation > pinned.epoch && "fresh epoch");
	assert(succ->bound_views == 1u && "the view follows the successor");
	assert(pred->retiring && client_ctx_registry_owns(reg, pred)
		   && "the pinned predecessor stays alive through completion");
	assert(client_ctx_registry_in_use(reg) == 2);

	/* The new stream's terrain installs immediately (no hold). */
	struct client_rpc m = sign(9, 0, 71);
	clin_rpc_send(&m);
	clin_update_drain();
	assert(!clin_world_message_held() && has_sign(9));
	/* The successor grants leases (producers resume at once). */
	struct world_lease fresh = {0};
	assert(owner->acquire(owner->user, WORLD_LEASE_MESHER, &fresh));
	assert(fresh.ctx == succ);

	/* The pinned job completes: released against its own context, rejected
	 * by epoch, and the predecessor is freed only now. */
	assert(!owner->release(owner->user, WORLD_LEASE_MESHER, &pinned)
		   && "an old completion cannot publish into the successor");
	assert(!client_ctx_registry_owns(reg, pred)
		   && client_ctx_registry_in_use(reg) == 1
		   && "the predecessor is freed only after its lease drains");
	assert(owner->release(owner->user, WORLD_LEASE_MESHER, &fresh)
		   && "the successor's own completion publishes");
	puts("PASS stream reset reinstall while pinned: successor installs, old "
		 "completion rejected, predecessor freed after drain");
}

static struct client_rpc column(w_coord_t x, uint8_t index, uint32_t generation) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	return (struct client_rpc) {.type = CRPC_CHUNK,
		.world_index = index, .world_generation = generation,
		.payload.chunk = {.x = x, .y = 0, .z = 0, .sx = CHUNK_SIZE,
			.sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}};
}

static unsigned input_acks, input_retries;
static void capture_input(struct server_rpc* rpc, void* unused) {
	(void)unused;
	if(rpc->type == SRPC_INPUT_READY) {
		if(rpc->payload.input_ready.retry) input_retries++;
		else input_acks++;
	}
}
static void noop_rpc(struct server_rpc* rpc, void* unused) {
	(void)rpc; (void)unused;
}

/* Fable B1-phase2 F1 (+ round-2 NACK scope), through the real handlers: a
 * stamped slot-1 WORLD_RESET (the host enters the Nether) retires slot 0 as
 * well (single payload). Then:
 *  - a stamped slot-0 column (NO_CONTEXT) and a FOREIGN one (slot 0 live but
 *    not the payload) are never written into the Nether payload and do NOT
 *    arm the NACK latch: they belong to another world and leave no hole here
 *    (arming would restream-livelock while that stream continues);
 *  - an unstamped column while two slots are live (AMBIGUOUS) may belong to
 *    this payload: dropped, and it DOES arm the latch, so the epoch marker
 *    NACKs instead of ACKing a server-marked-sent hole;
 *  - an unstamped column with one live slot reaches the payload. */
static void test_single_payload_no_cross_world(void) {
	no_world();
	svin_process_messages(noop_rpc, NULL, false);
	clin_test_reset_input_epoch_guard();
	world_reset(0, 81);
	world_reset(1, 90);
	assert(!client_ctx_registry_current(reg, 0)
		   && "a reset of the single payload retires every other slot");
	struct client_world_context* nether = client_ctx_registry_current(reg, 1);
	assert(nether && nether->server_generation == 90);
	assert(!clin_test_stream_install_failed());

	uint32_t no_ctx = reg->routed[CLIENT_CTX_ROUTE_NO_CONTEXT];
	struct client_rpc m = column(0, 0, 81);
	clin_process(&m);
	assert(reg->routed[CLIENT_CTX_ROUTE_NO_CONTEXT] == no_ctx + 1);
	assert(!world_find_chunk(&gstate.world, 0, 64, 0)
		   && "an overworld column never lands in the nether payload");
	assert(!clin_test_stream_install_failed()
		   && "a NO_CONTEXT column (another world) does not arm the NACK latch");

	/* A live non-payload slot (what a second client payload would add):
	 * its stamped column is FOREIGN -- dropped, latch untouched. */
	assert(client_ctx_registry_install(reg, 0, 0, 81));
	uint32_t foreign = reg->routed[CLIENT_CTX_ROUTE_FOREIGN];
	m = column(0, 0, 81);
	clin_process(&m);
	assert(reg->routed[CLIENT_CTX_ROUTE_FOREIGN] == foreign + 1);
	assert(!world_find_chunk(&gstate.world, 0, 64, 0));
	assert(!clin_test_stream_install_failed()
		   && "a FOREIGN column does not arm the NACK latch");

	/* Unstamped with two live slots: AMBIGUOUS, may be this payload's. */
	uint32_t ambiguous = reg->routed[CLIENT_CTX_ROUTE_AMBIGUOUS];
	m = column(32, 0, 0);
	clin_process(&m);
	assert(reg->routed[CLIENT_CTX_ROUTE_AMBIGUOUS] == ambiguous + 1);
	assert(!world_find_chunk(&gstate.world, 32, 64, 0));
	assert(clin_test_stream_install_failed()
		   && "an AMBIGUOUS column arms the NACK latch");
	client_ctx_registry_retire(reg, 0);

	m = column(16, 0, 0);
	clin_process(&m);
	assert(world_find_chunk(&gstate.world, 16, 64, 0)
		   && "an unstamped column reaches the single live payload");

	input_acks = input_retries = 0;
	clin_process(&(struct client_rpc) {.type = CRPC_PLAYER_POS,
		.payload.player_pos = {.position = {16.5, 65.62, 0.5},
			.input_epoch = 1}});
	svin_process_messages(capture_input, NULL, false);
	assert(input_retries == 1 && input_acks == 0
		   && "the marker NACKs the AMBIGUOUS hole: never ACKed as sent");
	assert(!clin_test_stream_install_failed());
	puts("PASS single payload: slot-1 reset retires slot 0; NO_CONTEXT and "
		 "FOREIGN columns dropped without arming the latch; AMBIGUOUS column "
		 "arms it and the marker NACKs");
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
	reg = clin_world_contexts();
	test_cosmetic_family();
	test_entity_family();
	test_time_family();
	test_terrain_family();
	test_world_lease_owner();
	test_stream_reset_reinstall_while_pinned();
	test_single_payload_no_cross_world();
	test_lifecycle();
	puts("PASS test_b1_route_families");
	return 0;
}
