/* Split-dims B2 (PREPARATION, default OFF): world-bound client payloads,
 * driven through the REAL client consumer (clin_process / clin_update_drain in
 * source/network/client_interface.c) and the REAL mesher worker.
 *
 *  - flag gating: OFF by default (every slot maps to the gstate storage, the
 *    B1 single-payload rule holds); the flag flips only with no context in use;
 *  - coexistence: two stamped WORLD_RESETs leave two live contexts, each with
 *    its own terrain / entity / particle payload; a view renders one payload;
 *  - independent reset: resetting (or releasing) one slot's payload leaves the
 *    other slot's context, terrain, entities, particles and views untouched;
 *  - lease drain: real mesher jobs from BOTH worlds are in flight when one
 *    world resets; the reset world's job releases against its retired
 *    predecessor (rejected, predecessor reclaimed only then), the other
 *    world's job publishes;
 *  - memory accounting: measured MEM1/MEM2 per extra payload (printed as
 *    "B2-MEM" lines for the budget report).
 *
 * Stamps below are SYNTHETIC (the producers are not stamped yet): these tests
 * prove the client mechanism only, never production isolation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "chunk_mesher.h"
#include "entity/entity.h"
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "game/sign_text_store.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "network/piston_moving_transport.h"
#include "network/server_interface.h"
#include "particle.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "world.h"

#define LOCAL_EID 77

static struct client_ctx_registry* reg;

static void world_reset(uint8_t index, uint32_t generation) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = index ? WORLD_DIM_NETHER
												  : WORLD_DIM_OVERWORLD,
			.local_entity = LOCAL_EID, .world_index = index,
			.world_generation = generation}});
}

static struct clin_world_payload payload(unsigned slot) {
	struct clin_world_payload p;
	assert(clin_world_payload_of_slot(slot, &p));
	return p;
}

/* A full column whose (x..x+15, 0, 0..15) floor is `floor_id` and air above. */
static void column(w_coord_t x, uint8_t floor_id, uint8_t index,
				   uint32_t generation) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	for(size_t lx = 0; lx < CHUNK_SIZE; lx++)
		for(size_t lz = 0; lz < CHUNK_SIZE; lz++)
			slot->ids[(lx * CHUNK_SIZE + lz) * WORLD_HEIGHT] = floor_id;
	struct client_rpc m = {.type = CRPC_CHUNK,
		.world_index = index, .world_generation = generation,
		.payload.chunk = {.x = x, .y = 0, .z = 0, .sx = CHUNK_SIZE,
			.sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}};
	clin_process(&m);
}

static uint8_t floor_at(struct world* w, w_coord_t x) {
	struct chunk* c = world_find_chunk(w, x, 0, 0);
	return c ? world_get_block(w, x, 0, 0).type : 0xFF;
}

static void spawn_item(uint32_t id, uint8_t index, uint32_t generation) {
	struct client_rpc m = {.type = CRPC_SPAWN_ITEM,
		.world_index = index, .world_generation = generation,
		.payload.spawn_item = {.entity_id = id,
			.item = {.id = 1, .durability = 0, .count = 1},
			.pos = {0.5F, 70.0F, 0.5F}}};
	clin_process(&m);
}

static struct entity* tracked(unsigned slot, uint32_t id) {
	struct entity** pp = dict_entity_get(payload(slot).entities, id);
	return pp ? *pp : NULL;
}

static void smoke(uint8_t index, uint32_t generation) {
	struct client_rpc m = {.type = CRPC_SMOKE_PUFF,
		.world_index = index, .world_generation = generation,
		.payload.smoke_puff = {.pos = {0.5F, 0.5F, 0.5F}, .intensity = 1.0F}};
	clin_process(&m);
}

static size_t particles(unsigned slot) {
	return particle_system_count(payload(slot).particles);
}

/* No context in use, both storages empty, flag in the requested state. */
static void fresh(bool enabled) {
	client_ctx_registry_retire(reg, 0);
	client_ctx_registry_retire(reg, 1);
	assert(client_ctx_registry_in_use(reg) == 0);
	world_unload_all(&gstate.world);
	particle_system_clear(particle_system_primary());
	dict_entity_it_t it;
	for(dict_entity_it(it, gstate.entities); !dict_entity_end_p(it);
		dict_entity_next(it))
		free(dict_entity_ref(it)->value);
	dict_entity_reset(gstate.entities);
	gstate.players[0].local_player = NULL;
	gstate.players[1].local_player = NULL;
	/* Flip through OFF so the extra storage is always a fresh allocation. */
	assert(clin_world_payloads_set_enabled(false));
	assert(clin_world_payloads_set_enabled(enabled));
	assert(clin_world_payloads_enabled() == enabled);
}

static void test_flag_gating(void) {
	fresh(false);
	assert(!clin_world_payloads_enabled() && "default OFF");
	/* OFF: every slot is the single gstate storage. */
	assert(payload(0).terrain == &gstate.world && payload(1).terrain == &gstate.world);
	assert(payload(1).entities == gstate.entities);
	assert(payload(1).particles == particle_system_primary());
	/* OFF keeps the B1 single-payload rule: a slot-1 reset retires slot 0. */
	world_reset(0, 11);
	world_reset(1, 12);
	assert(!client_ctx_registry_current(reg, 0));
	assert(gstate.world.lease_owner && gstate.world.lease_owner->user == NULL
		   && "OFF: the one owner that follows the payload slot");
	/* The flag never flips under a live or draining context. */
	assert(!clin_world_payloads_set_enabled(true));
	assert(!clin_world_payloads_enabled());
	fresh(true);
	assert(payload(1).terrain != &gstate.world && payload(0).terrain == &gstate.world);
	assert(payload(1).entities != gstate.entities);
	assert(payload(1).particles != particle_system_primary());
	world_reset(0, 13);
	assert(!clin_world_payloads_set_enabled(false)
		   && "ON cannot be turned off under a live context");
	puts("PASS flag gating: default OFF = single gstate storage + B1 rule; "
		 "flips only with no context in use");
}

/* Host in the overworld (slot 0) enters the Nether (slot 1) while the guest
 * view stays on the overworld: both payloads coexist. */
static void two_worlds(void) {
	fresh(true);
	world_reset(0, 100);
	world_reset(1, 200);
	assert(clin_world_payload_primary_slot() == 1);
	struct client_world_context* ow = client_ctx_registry_current(reg, 0);
	struct client_world_context* ne = client_ctx_registry_current(reg, 1);
	assert(ow && ow->server_generation == 100 && !ow->retiring);
	assert(ne && ne->server_generation == 200 && !ne->retiring);
	assert(clin_world_payload_ready(0) && clin_world_payload_ready(1));
	assert(clin_world_payload_bind_view(0, 1));
}

static void test_coexist(void) {
	two_worlds();
	struct world* w0 = payload(0).terrain;
	struct world* w1 = payload(1).terrain;
	assert(w0->dimension == WORLD_DIM_OVERWORLD && w1->dimension == WORLD_DIM_NETHER);
	/* Each world's lease owner is bound to its own slot. */
	assert(w0->lease_owner && w1->lease_owner && w0->lease_owner != w1->lease_owner);

	/* Terrain: the same coordinates hold different blocks per world. */
	column(0, BLOCK_STONE, 0, 100);
	column(0, BLOCK_NETHERRACK, 1, 200);
	assert(floor_at(w0, 0) == BLOCK_STONE && floor_at(w1, 0) == BLOCK_NETHERRACK);
	column(16, BLOCK_DIRT, 0, 100);
	assert(floor_at(w0, 16) == BLOCK_DIRT && floor_at(w1, 16) == 0xFF
		   && "a column for one world never lands in the other");

	/* Entities: each spawn lands in its own world's store, bound to it. */
	spawn_item(601, 0, 100);
	spawn_item(602, 1, 200);
	assert(tracked(0, 601) && !tracked(1, 601));
	assert(tracked(1, 602) && !tracked(0, 602));
	struct entity* e = tracked(0, 601);
	assert(e && e->world == w0);
	e = tracked(1, 602);
	assert(e && e->world == w1);
	assert(gstate.players[0].local_player == tracked(1, LOCAL_EID)
		   && !tracked(0, LOCAL_EID)
		   && "player 0's body moved to its new world, no orphan left behind");

	/* Particles: each effect lands in its own world's system. */
	smoke(0, 100);
	size_t p0 = particles(0);
	assert(p0 > 0 && particles(1) == 0);
	smoke(1, 200);
	assert(particles(1) > 0 && particles(0) == p0);
	assert(particle_system_selected() == particle_system_primary()
		   && "dispatch restores the primary selection");

	/* Views: view 0 follows player 0 (slot 1), view 1 is the guest's (slot 0). */
	struct clin_world_payload v;
	assert(clin_world_payload_of_view(0, &v) && v.terrain == w1);
	assert(clin_world_payload_of_view(1, &v) && v.terrain == w0);
	assert(clin_world_payload_bind_view(1, 1));
	assert(clin_world_payload_of_view(1, &v) && v.terrain == w1
		   && !(client_ctx_registry_current(reg, 0)->bound_views & 2u)
		   && "a view renders exactly one payload");
	assert(clin_world_payload_bind_view(0, 1));

	/* Still unstamped with two live slots: AMBIGUOUS, applied nowhere. */
	uint32_t ambiguous = reg->routed[CLIENT_CTX_ROUTE_AMBIGUOUS];
	spawn_item(603, 0, 0);
	assert(reg->routed[CLIENT_CTX_ROUTE_AMBIGUOUS] == ambiguous + 1);
	assert(!tracked(0, 603) && !tracked(1, 603));
	puts("PASS coexist: two live contexts, terrain / entities / particles land "
		 "in their own world, one payload per view, unstamped still AMBIGUOUS");
}

static void test_independent_reset(void) {
	two_worlds();
	struct world* w0 = payload(0).terrain;
	struct world* w1 = payload(1).terrain;
	column(0, BLOCK_STONE, 0, 100);
	column(0, BLOCK_NETHERRACK, 1, 200);
	spawn_item(701, 0, 100);
	spawn_item(702, 1, 200);
	smoke(0, 100);
	smoke(1, 200);
	size_t p0 = particles(0);
	struct client_world_context* ow = client_ctx_registry_current(reg, 0);

	/* The host respawns in the Nether: a new server generation for slot 1. */
	world_reset(1, 201);
	assert(client_ctx_registry_current(reg, 1)->server_generation == 201);
	assert(floor_at(w1, 0) == 0xFF && !tracked(1, 702) && particles(1) == 0
		   && "the reset world's payload is torn down");
	assert(client_ctx_registry_current(reg, 0) == ow && ow->server_generation == 100
		   && !ow->retiring && (ow->bound_views & 2u)
		   && "the other world's context and view are untouched");
	assert(floor_at(w0, 0) == BLOCK_STONE && tracked(0, 701) && particles(0) == p0
		   && "the other world's terrain, entities and particles survive");
	column(0, BLOCK_SOULSAND, 1, 201);
	assert(floor_at(w1, 0) == BLOCK_SOULSAND && floor_at(w0, 0) == BLOCK_STONE);
	column(0, BLOCK_DIRT, 1, 200);
	assert(floor_at(w1, 0) == BLOCK_SOULSAND && "the old generation is STALE");

	/* The guest's overworld resets (new generation): the Nether survives. */
	world_reset(0, 101);
	assert(floor_at(w0, 0) == 0xFF && !tracked(0, 701) && particles(0) == 0);
	assert(floor_at(w1, 0) == BLOCK_SOULSAND);
	assert(clin_world_payload_primary_slot() == 0);
	/* Host back in slot 0; release the Nether (the guest left it). */
	assert(!clin_world_payload_release(0) && "the primary is WORLD_RESET's");
	spawn_item(703, 1, 201);
	assert(clin_world_payload_release(1));
	assert(!client_ctx_registry_current(reg, 1) && !clin_world_payload_ready(1));
	assert(floor_at(w1, 0) == 0xFF && !tracked(1, 703));
	column(0, BLOCK_STONE, 0, 101);
	assert(floor_at(w0, 0) == BLOCK_STONE && clin_world_payload_ready(0));

	/* An UNSTAMPED reset (a slot-unaware producer: a new single-world
	 * session) owns the whole client and releases the other slot. */
	world_reset(1, 300);
	column(16, BLOCK_NETHERRACK, 1, 300);
	world_reset(0, 0);
	assert(!client_ctx_registry_current(reg, 1) && floor_at(w1, 16) == 0xFF);
	column(32, BLOCK_DIRT, 0, 0);
	assert(floor_at(w0, 32) == BLOCK_DIRT
		   && "one live slot: unstamped traffic routes again");
	puts("PASS independent reset: resetting or releasing one world preserves "
		 "the other's context, view, terrain, entities and particles");
}

static void sign(int32_t x, const char* text, uint8_t index, uint32_t gen) {
	struct client_rpc m = {.type = CRPC_SIGN_TEXT, .world_index = index,
		.world_generation = gen, .payload.sign_text = {.x = x, .y = 1, .z = 0}};
	memset(m.payload.sign_text.grid, ' ', sizeof m.payload.sign_text.grid);
	memcpy(m.payload.sign_text.grid, text, strlen(text));
	clin_process(&m);
}

/* The sign text stored at (x, 1, 0) in a slot's own store, or "" if none. */
static bool sign_is(unsigned slot, int32_t x, const char* text) {
	char grid[64];
	struct sign_text_store* prev = sign_text_store_select(payload(slot).signs);
	bool have = sign_text_store_get(x, 1, 0, grid);
	sign_text_store_select(prev);
	if(!text) return !have;
	return have && memcmp(grid, text, strlen(text)) == 0;
}

static void piston(w_coord_t x, uint8_t stored, uint8_t index, uint32_t gen) {
	struct piston_moving_state st = {.stored_id = stored, .stored_meta = 0,
		.facing = 1, .flags = 0, .current_progress = 0.5F,
		.last_progress = 0.0F};
	struct client_rpc m = {.type = CRPC_PISTON_MOVING, .world_index = index,
		.world_generation = gen,
		.payload.piston_moving = {.x = x, .y = 1, .z = 0}};
	assert(piston_moving_wire_encode(&m.payload.piston_moving.state, &st));
	clin_process(&m);
}

static bool piston_is(unsigned slot, w_coord_t x, int stored) {
	struct piston_moving_store* prev
		= client_piston_moving_store_select(payload(slot).pistons);
	struct piston_moving_state st;
	bool have = client_piston_moving_query(x, 1, 0, &st);
	client_piston_moving_store_select(prev);
	return stored < 0 ? !have : (have && st.stored_id == stored);
}

static struct clin_world_payload_clock clock_of(unsigned slot) {
	struct clin_world_payload_clock k;
	assert(clin_world_payload_clock(slot, &k));
	return k;
}

static void weather(bool rain, float strength, uint8_t index, uint32_t gen) {
	clin_process(&(struct client_rpc) {.type = CRPC_WEATHER_SET,
		.world_index = index, .world_generation = gen,
		.payload.weather_set = {.raining = rain, .rain_strength = strength}});
}

/* Signs, moving pistons, clock and weather are per-world payload data. */
static void test_world_stores(void) {
	/* OFF: one store/clock for every slot (the pre-B2 objects). */
	fresh(false);
	assert(payload(1).signs == sign_text_store_primary()
		   && payload(1).pistons == client_piston_moving_store_primary());

	two_worlds();
	assert(payload(1).signs != sign_text_store_primary()
		   && payload(1).pistons != client_piston_moving_store_primary());
	column(0, BLOCK_STONE, 0, 100);
	column(0, BLOCK_NETHERRACK, 1, 200);
	/* Same coordinates, different content per world. */
	sign(0, "OVERWORLD", 0, 100);
	sign(0, "NETHER", 1, 200);
	assert(sign_is(0, 0, "OVERWORLD") && sign_is(1, 0, "NETHER"));
	piston(1, BLOCK_STONE, 0, 100);
	piston(1, BLOCK_NETHERRACK, 1, 200);
	assert(piston_is(0, 1, BLOCK_STONE) && piston_is(1, 1, BLOCK_NETHERRACK));
	assert(world_get_block(payload(0).terrain, 1, 1, 0).type == BLOCK_PISTON_MOVING
		   && world_get_block(payload(1).terrain, 1, 1, 0).type == BLOCK_PISTON_MOVING);
	assert(sign_text_store_select(NULL) == sign_text_store_primary()
		   && client_piston_moving_store_select(NULL)
			  == client_piston_moving_store_primary()
		   && "dispatch restores the primary selections");

	/* Clock + weather follow each world, not player 0. */
	gstate.world_time = 1;
	clin_process(&(struct client_rpc) {.type = CRPC_TIME_SET, .world_index = 0,
		.world_generation = 100, .payload.time_set = 6000});
	clin_process(&(struct client_rpc) {.type = CRPC_TIME_SET, .world_index = 1,
		.world_generation = 200, .payload.time_set = 18000});
	assert(clock_of(0).world_time == 6000 && clock_of(1).world_time == 18000);
	assert(gstate.world_time == 6000 && "slot 0's clock is the gstate field");
	weather(true, 1.0F, 0, 100);
	weather(false, 0.0F, 1, 200);
	assert(clock_of(0).raining && clock_of(0).rain_strength == 1.0F);
	assert(!clock_of(1).raining && clock_of(1).rain_strength == 0.0F);
	clin_process(&(struct client_rpc) {.type = CRPC_TIME_SET, .world_index = 1,
		.world_generation = 199, .payload.time_set = 1});
	assert(clock_of(1).world_time == 18000 && "a stale clock update is dropped");

	/* Reset of the Nether: its signs/pistons go, the overworld's stay. */
	world_reset(1, 201);
	assert(sign_is(1, 0, NULL) && piston_is(1, 1, -1));
	assert(sign_is(0, 0, "OVERWORLD") && piston_is(0, 1, BLOCK_STONE));
	assert(clock_of(0).world_time == 6000 && clock_of(0).raining);
	/* Reset of the overworld: the (new) Nether's survive. */
	column(0, BLOCK_NETHERRACK, 1, 201);
	sign(0, "NETHER2", 1, 201);
	piston(1, BLOCK_SOULSAND, 1, 201);
	world_reset(0, 101);
	assert(sign_is(0, 0, NULL) && piston_is(0, 1, -1));
	assert(sign_is(1, 0, "NETHER2") && piston_is(1, 1, BLOCK_SOULSAND));
	/* Release of the (non-primary) Nether clears only its stores. */
	column(0, BLOCK_STONE, 0, 101);
	sign(0, "OW2", 0, 101);
	assert(clin_world_payload_release(1));
	assert(sign_is(1, 0, NULL) && piston_is(1, 1, -1) && sign_is(0, 0, "OW2"));
	puts("PASS world stores: signs, moving pistons, clock and weather are "
		 "per-world; resetting or releasing one world preserves the other's");
}

/* A lost column in a NON-primary world raises that world's resend marker
 * only; the primary stream's NACK latch is untouched. */
static void test_resend_marker(void) {
	two_worlds(); /* primary = slot 1 */
	assert(!clin_world_payload_resend_pending(0));
	/* A moving-piston sidecar the client cannot store (y outside the
	 * world) is an install failure of that world's terrain stream. */
	struct piston_moving_state st = {.stored_id = BLOCK_STONE, .facing = 1};
	struct client_rpc m = {.type = CRPC_PISTON_MOVING, .world_index = 0,
		.world_generation = 100,
		.payload.piston_moving = {.x = 0, .y = -1, .z = 0}};
	assert(piston_moving_wire_encode(&m.payload.piston_moving.state, &st));
	clin_process(&m);
	assert(clin_world_payload_install_failures(0) == 1
		   && clin_world_payload_resend_pending(0)
		   && "the non-primary world marks its own hole");
	assert(!clin_world_payload_resend_pending(1)
		   && !clin_test_stream_install_failed()
		   && "never folded into the primary stream's NACK latch");
	world_reset(0, 102);
	assert(!clin_world_payload_resend_pending(0) && "cleared by its reset");
	puts("PASS resend marker: a non-primary install failure marks only that "
		 "world; its reset clears it");
}

static void* fail_alloc(size_t n) {
	(void)n;
	return NULL;
}

/* Fable F1: readiness is per payload. Player 0's reset of ITS world failing
 * (owner allocation refused) must not silence the other world's traffic,
 * and the failed world must not borrow the other's readiness. */
static void test_per_payload_readiness(void) {
	two_worlds(); /* player 0 in slot 1, guest view on slot 0 */
	column(0, BLOCK_STONE, 0, 100);
	clin_set_alloc_hook(fail_alloc);
	world_reset(1, 210);
	clin_set_alloc_hook(NULL);
	assert(!clin_world_payload_ready(1) && clin_world_payload_ready(0));
	spawn_item(801, 0, 100);
	assert(tracked(0, 801) && "the ready guest world still spawns");
	clin_process(&(struct client_rpc) {.type = CRPC_SET_BLOCK, .world_index = 0,
		.world_generation = 100, .payload.set_block = {.x = 3, .y = 0, .z = 3,
			.block = {.type = BLOCK_DIRT}}});
	assert(world_get_block(payload(0).terrain, 3, 0, 3).type == BLOCK_DIRT
		   && "the ready guest world still takes block edits");
	spawn_item(802, 1, 210);
	assert(!tracked(1, 802) && "the failed world stays shut");
	puts("PASS per-payload readiness: a failed reset of player 0's world "
		 "neither silences nor lends readiness to the other world");
}

/* Fable F4: a slot-1 particle system culls against its own view's camera. */
static void test_particle_camera(void) {
	two_worlds(); /* view 0 (player 0) -> slot 1 */
	gstate.players[0].camera.x = 500.0F;
	gstate.players[0].camera.y = 70.0F;
	gstate.players[0].camera.z = 500.0F;
	clin_world_payload_sync_particle_cameras();
	struct client_rpc m = {.type = CRPC_SMOKE_PUFF, .world_index = 1,
		.world_generation = 200,
		.payload.smoke_puff = {.pos = {500.5F, 70.5F, 500.5F}, .intensity = 1.0F}};
	clin_process(&m);
	size_t near = particles(1);
	assert(near > 0 && "spawned next to the slot-1 view's camera");
	smoke(1, 200); /* at the origin: ~700 blocks from that camera */
	assert(particles(1) == near && "culled against the slot-1 camera");
	gstate.players[0].camera.x = gstate.players[0].camera.y
		= gstate.players[0].camera.z = 0.0F;
	clin_world_payload_sync_particle_cameras();
	puts("PASS particle camera: slot-1 spawns cull against its own view");
}


/* Ghast fireballs and snowballs share an icon, never a visual identity.
 * Exercise the actual stamped consumer in both coexisting world payloads. */
static void test_fireball_and_blast_payloads(void) {
    two_worlds();
    gstate.players[0].camera.x = gstate.players[0].camera.y
        = gstate.players[0].camera.z = 0.0F;
    clin_world_payload_sync_particle_cameras();
    struct client_rpc shot = {.type = CRPC_SPAWN_ARROW,
        .world_index = 1, .world_generation = 200,
        .payload.spawn_arrow = {.entity_id = 910, .item_id = ITEM_SNOW_BALL,
            .fireball = true, .pos = {0.5F, 2.0F, 0.5F}}};
    clin_process(&shot);
    struct entity* fireball = tracked(1, 910);
    assert(fireball && fireball->render && fireball->data.arrow.fireball);
    assert(fireball->world == payload(1).terrain && !tracked(0, 910));
    shot.payload.spawn_arrow.entity_id = 911;
    shot.payload.spawn_arrow.fireball = false;
    clin_process(&shot);
    assert(tracked(1, 911) && !tracked(1, 911)->data.arrow.fireball);
    shot.world_index = 0; shot.world_generation = 100;
    shot.payload.spawn_arrow.entity_id = 912;
    shot.payload.spawn_arrow.fireball = true;
    clin_process(&shot);
    assert(tracked(0, 912) && tracked(0, 912)->data.arrow.fireball);
    assert(!tracked(1, 912));
    shot.world_index = 1; shot.world_generation = 199;
    shot.payload.spawn_arrow.entity_id = 913;
    clin_process(&shot);
    assert(!tracked(1, 913) && !tracked(0, 913));
    size_t p0 = particles(0), p1 = particles(1);
    struct client_rpc blast = {.type = CRPC_ENTITY_EXPLODE,
        .world_index = 1, .world_generation = 200,
        .payload.entity_explode = {.pos = {0.5F, 2.0F, 0.5F}, .power = 1.0F}};
    clin_process(&blast);
    assert(particles(1) == p1 + 24 && particles(0) == p0);
    blast.world_generation = 199;
    clin_process(&blast);
    assert(particles(1) == p1 + 24 && particles(0) == p0);
    blast.world_index = 0; blast.world_generation = 100;
    clin_process(&blast);
    assert(particles(0) == p0 + 24 && particles(1) == p1 + 24);
    assert(particle_system_selected() == particle_system_primary());
    puts("PASS fireball/blast payloads: explicit fireball vs snowball, both worlds, stale rejection, isolated 24-particle impacts");
}

static void wait_held(void) {
	ptime_t start = time_get();
	while(!chunk_mesher_test_worker_held()) {
		assert(time_diff_s(start, time_get()) < 5);
		thread_msleep(1);
	}
}

static void test_lease_drain(void) {
	two_worlds();
	struct world* w0 = payload(0).terrain;
	struct world* w1 = payload(1).terrain;
	column(0, BLOCK_STONE, 0, 100);
	column(0, BLOCK_NETHERRACK, 1, 200);
	struct chunk* c0 = world_find_chunk(w0, 0, 0, 0);
	struct chunk* c1 = world_find_chunk(w1, 0, 0, 0);
	struct chunk* c = c0;
	assert(c && c->world == w0);
	c = c1;
	assert(c && c->world == w1);
	struct client_world_context* ow = client_ctx_registry_current(reg, 0);
	struct client_world_context* ne = client_ctx_registry_current(reg, 1);

	assert(chunk_mesher_init());
	chunk_mesher_test_hold_worker(true);
	/* The Nether job pins the worker; the overworld job queues behind it. */
	assert(chunk_mesher_send(c1));
	c1->mesh_in_flight = true;
	wait_held();
	assert(chunk_mesher_send(c0));
	c0->mesh_in_flight = true;
	assert(ne->leases[CLIENT_CTX_LEASE_MESHER] == 1
		   && ow->leases[CLIENT_CTX_LEASE_MESHER] == 1
		   && "each job leases against its OWN world's context");

	/* The Nether resets while its job is pinned. */
	uint32_t stale = mesh_stale_discards;
	world_reset(1, 202);
	struct client_world_context* ne2 = client_ctx_registry_current(reg, 1);
	assert(ne2 && ne2 != ne && ne->retiring && client_ctx_registry_owns(reg, ne)
		   && "the predecessor stays owned while its lease is out");
	assert(client_ctx_registry_current(reg, 0) == ow && !ow->quiescing
		   && ow->leases[CLIENT_CTX_LEASE_MESHER] == 1
		   && "the other world's lease and context are untouched");
	assert(!clin_world_payloads_set_enabled(false)
		   && "storage never freed under an outstanding lease");

	chunk_mesher_test_hold_worker(false);
	ptime_t start = time_get();
	while(chunk_mesher_pending()) {
		assert(time_diff_s(start, time_get()) < 5);
		chunk_mesher_receive();
		thread_msleep(1);
	}
	assert(mesh_stale_discards == stale + 1
		   && "exactly the reset world's completion is rejected");
	assert(!client_ctx_registry_owns(reg, ne)
		   && "the predecessor is reclaimed once its lease drained");
	assert(ow->leases[CLIENT_CTX_LEASE_MESHER] == 0);
	bool published = false;
	for(size_t k = 0; k < CHUNK_MESH_LISTS; k++)
		published |= c0->has_displist[k];
	assert(published && "the surviving world's completion publishes");
	assert(client_ctx_registry_in_use(reg) == 2);

	/* The GPU hand-off lease of either world drains the same way. */
	struct world_lease gpu = {0};
	const struct world_lease_owner* o1 = w1->lease_owner;
	assert(o1->acquire(o1->user, WORLD_LEASE_GPU, &gpu) && gpu.ctx == ne2);
	world_reset(1, 203);
	assert(client_ctx_registry_owns(reg, ne2) && ne2->retiring);
	assert(!o1->release(o1->user, WORLD_LEASE_GPU, &gpu));
	assert(!client_ctx_registry_owns(reg, ne2));
	chunk_mesher_shutdown();
	puts("PASS lease drain: in-flight jobs from both worlds; the reset world's "
		 "completion is rejected and its predecessor reclaimed after the drain, "
		 "the other world's publishes");
}

static void print_mem(const char* label, unsigned slot) {
	struct clin_world_payload_memory m;
	assert(clin_world_payload_memory(slot, &m));
	printf("B2-MEM %s slot=%u sections=%zu chunks=%zu voxel_chunks=%zu "
		   "entities=%zu fixed_mem1=%zu world_mem1=%zu (dict=%zu) "
		   "entity_mem1=%zu mem1=%zu voxel_mem2=%zu mesh_mem2=%zu "
		   "particle_mem2=%zu mem2=%zu\n",
		   label, slot, m.terrain.sections, m.terrain.chunks, m.terrain.voxel_chunks,
		   m.entities, m.fixed_mem1, m.terrain.mem1_bytes, m.terrain.dict_bytes,
		   m.entity_mem1, m.mem1_bytes, m.terrain.voxel_bytes, m.terrain.mesh_bytes,
		   m.particle_mem2, m.mem2_bytes);
}

static void test_memory_accounting(void) {
	fresh(true);
	struct clin_world_payload_memory m;
	assert(clin_world_payload_memory(1, &m));
	assert(m.fixed_mem1 > sizeof(struct world) && m.terrain.sections == 0
		   && m.terrain.mem2_bytes == 0 && m.entities == 0);
	print_mem("empty", 1);
	size_t fixed = m.fixed_mem1;

	/* Guest world in slot 1, player 0 (primary) in slot 0. */
	world_reset(1, 600);
	world_reset(0, 500);
	/* 9 columns (a 3x3 near disk) in the extra payload. */
	for(int cx = -1; cx <= 1; cx++)
		for(int cz = -1; cz <= 1; cz++) {
			struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
			assert(slot);
			memset(slot, 0, sizeof(*slot));
			clin_process(&(struct client_rpc) {.type = CRPC_CHUNK,
				.world_index = 1, .world_generation = 600,
				.payload.chunk = {.x = cx * CHUNK_SIZE, .y = 0,
					.z = cz * CHUNK_SIZE, .sx = CHUNK_SIZE,
					.sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}});
		}
	for(uint32_t id = 900; id < 932; id++)
		spawn_item(id, 1, 600);
	for(int i = 0; i < 64; i++)
		smoke(1, 600);
	assert(clin_world_payload_memory(1, &m));
	print_mem("9col+32ent", 1);
	assert(m.terrain.sections == 9 && m.terrain.chunks == 9 * COLUMN_HEIGHT);
	assert(m.terrain.voxel_chunks == m.terrain.chunks
		   && m.terrain.voxel_bytes == m.terrain.chunks
			  * (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 5 / 2));
	assert(m.entities == 32 && "player 0's body left with player 0");
	assert(m.entity_mem1 >= 32 * sizeof(struct entity));
	assert(m.particle_mem2 > 0 && m.fixed_mem1 == fixed);
	assert(m.mem1_bytes == m.fixed_mem1 + m.terrain.mem1_bytes + m.entity_mem1);
	assert(m.mem2_bytes == m.terrain.mem2_bytes + m.particle_mem2);
	/* The primary payload is not billed as extra storage. */
	struct clin_world_payload_memory m0;
	assert(clin_world_payload_memory(0, &m0) && m0.fixed_mem1 == 0
		   && m0.terrain.sections == 0 && m0.entities == 1);

	/* Per-section and per-entity marginal costs, for the budget report. */
	size_t sections = m.terrain.sections;
	printf("B2-MEM per-section mem1=%zu mem2=%zu (voxel %u B x %d chunks); "
		   "per-entity mem1~%zu; particle ring cap mem2 max=%zu\n",
		   (m.terrain.mem1_bytes - m.terrain.dict_bytes) / sections,
		   m.terrain.voxel_bytes / sections,
		   (unsigned)(CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 5 / 2), COLUMN_HEIGHT,
		   sizeof(struct entity), (size_t)2 * 4000 * sizeof(struct particle));

	/* Release returns the dynamic storage; only the fixed part remains. */
	assert(clin_world_payload_release(1));
	assert(clin_world_payload_memory(1, &m));
	print_mem("released", 1);
	assert(m.terrain.sections == 0 && m.terrain.voxel_bytes == 0 && m.entities == 0);
	puts("PASS memory accounting: measured per-payload MEM1/MEM2 (B2-MEM lines)");
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
	test_flag_gating();
	test_coexist();
	test_independent_reset();
	test_world_stores();
	test_resend_marker();
	test_per_payload_readiness();
	test_particle_camera();
	test_fireball_and_blast_payloads();
	test_lease_drain();
	test_memory_accounting();
	fresh(false);
	puts("PASS test_b2_world_payloads");
	return 0;
}
