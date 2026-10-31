/* Split-dims B2 -> A2-T client contract (lane B, b2-a2t-0926), through the
 * real clin_process / clin_update_drain with world-bound payloads ON:
 *
 *  H1 player 0 switches world while the guest stays behind: the guest's
 *     world context, terrain, body, session (bound/attached/ready) and view
 *     are untouched; player 0 gets the new slot, view 0 and a new body.
 *  H2 player 0 switches world alone (no guest view on its old world): the
 *     old world is NOT torn down by the client -- its lifetime follows the
 *     server (unload traffic), per the B2 slot contract; it is just unviewed.
 *  H3 HOST JOIN: player 0 follows the guest into the guest's live world
 *     (same slot, same generation): no teardown, no new context; view 0
 *     joins it, player 0's body is created in its store (replacing the
 *     guest's remote copy of the host) and the guest keeps everything.
 *  H4 a reset naming the guest's slot at ANOTHER generation is a rebuild,
 *     not a join (the guest's view goes with the old world).
 *  B1 BEGIN_WORLD_SWITCH stamped for a world that is not player 0's is
 *     ignored (no host loading barrier); for player 0's world it applies.
 *  F2/F4/F5/F1 (Fable B2-A2T review): the load screen judges player 0's
 *     world; an older-generation reset is dropped; a host rebuild keeps a
 *     retried guest install; a stamped world retire releases only an
 *     unviewed, body-less, non-primary world at its exact generation.
 *  W1 A2-W (per-world streaming after COMMIT): the host is in the Nether
 *     (slot 1, view 0), the guest stays in the Overworld (slot 0, view 1).
 *     The host walks 64 blocks (4 columns east); the server's stamped
 *     slot-1 stream adds the columns entering its radius-4 ring and unloads
 *     the ones leaving it. Only the slot-1 payload changes: it ends holding
 *     exactly the ring around the new position; slot 0's context (pointer,
 *     epoch, generation, view 1), terrain, entities and the guest session are
 *     untouched -- also by slot-1 UNLOADs naming columns slot 0 holds -- and
 *     a stale-generation slot-1 column is dropped.
 *  F  flag OFF: a host reset still owns the whole client (every other slot
 *     retires, the guest session is cleared) -- unchanged.
 *
 * Producers: the stamped WORLD_RESET / BEGIN_WORLD_SWITCH are the A2-T
 * contract (A2-W sends them); a second LIVE server world does not exist
 * before A2-W, so these messages are the producer contract, as in
 * test_b2_gates. Non-aborting: ok/FAIL lines. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "entity/entity.h" /* D2: entity_local_player_walk_owned */
#include "platform/input_owner.h" /* D2: struct input_player_frame */
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "game/gui/screen.h"
#include "network/server_interface.h"
#include "world.h"

#define HOST_EID 77

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

static struct client_ctx_registry* reg;

static void host_reset(uint8_t index, uint32_t gen) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = index ? WORLD_DIM_NETHER
												  : WORLD_DIM_OVERWORLD,
			.local_entity = HOST_EID, .world_index = index,
			.world_generation = gen}});
}

static void guest_move(uint8_t index, uint32_t gen) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = index ? WORLD_DIM_NETHER
												  : WORLD_DIM_OVERWORLD,
			.local_entity = HOST_EID, .world_index = index,
			.world_generation = gen, .guest_world = true, .guest_view = 1}});
}

static struct clin_world_payload payload(unsigned slot) {
	struct clin_world_payload p;
	assert(clin_world_payload_of_slot(slot, &p));
	return p;
}

static void column(w_coord_t x, uint8_t id, uint8_t index, uint32_t gen) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	for(size_t lx = 0; lx < CHUNK_SIZE; lx++)
		for(size_t lz = 0; lz < CHUNK_SIZE; lz++)
			slot->ids[(lx * CHUNK_SIZE + lz) * WORLD_HEIGHT] = id;
	clin_process(&(struct client_rpc) {.type = CRPC_CHUNK,
		.world_index = index, .world_generation = gen,
		.payload.chunk = {.x = x, .y = 0, .z = 0, .sx = CHUNK_SIZE,
			.sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}});
}

static void column_at(w_coord_t cx, w_coord_t cz, uint8_t index, uint32_t gen) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	for(size_t lx = 0; lx < CHUNK_SIZE; lx++)
		for(size_t lz = 0; lz < CHUNK_SIZE; lz++)
			slot->ids[(lx * CHUNK_SIZE + lz) * WORLD_HEIGHT] = BLOCK_STONE;
	clin_process(&(struct client_rpc) {.type = CRPC_CHUNK,
		.world_index = index, .world_generation = gen,
		.payload.chunk = {.x = cx * CHUNK_SIZE, .y = 0, .z = cz * CHUNK_SIZE,
			.sx = CHUNK_SIZE, .sy = WORLD_HEIGHT, .sz = CHUNK_SIZE,
			.slot = slot}});
}

static bool has_chunk(unsigned slot, w_coord_t x) {
	return world_find_chunk(payload(slot).terrain, x, 0, 0) != NULL;
}

static bool has_entity(unsigned slot, uint32_t id) {
	struct entity** pp = dict_entity_get(payload(slot).entities, id);
	return pp && *pp;
}

static void spawn_item(uint32_t id, uint8_t index, uint32_t gen) {
	clin_process(&(struct client_rpc) {.type = CRPC_SPAWN_ITEM,
		.world_index = index, .world_generation = gen,
		.payload.spawn_item = {.entity_id = id,
			.item = {.id = 1, .durability = 0, .count = 1},
			.pos = {0.5F, 70.0F, 0.5F}}});
}

static void attach_guest(void) {
	struct client_session* g = &gstate.players[1];
	g->local_bound = true;
	g->local_ticket = 5;
	g->local_generation = 9;
	g->local_phase = CLIENT_GUEST_ATTACHED;
}
static void guest_ready(uint32_t eid) {
	clin_process(&(struct client_rpc) {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 5, .generation = 9, .owner = 1,
			.result = LOCAL_GUEST_GAMEPLAY_READY, .entity_id = eid,
			.health = MAX_PLAYER_HEALTH,
			.x = 0.5, .y = 70.0, .z = 0.5}});
}
static bool guest_session_intact(void) {
	const struct client_session* g = &gstate.players[1];
	return g->local_bound && g->local_ticket == 5
		&& g->local_phase == CLIENT_GUEST_ATTACHED && g->local_gameplay_ready
		&& g->local_player;
}

static void flag(bool on) {
	client_ctx_registry_retire(reg, 0);
	client_ctx_registry_retire(reg, 1);
	assert(clin_world_payloads_set_enabled(false));
	gstate.players[1] = (struct client_session) {0};
	gstate.players[1].visible_window = -1;
	assert(clin_world_payloads_set_enabled(on));
}

static void begin_switch(uint8_t index, uint32_t gen) {
	clin_process(&(struct client_rpc) {.type = CRPC_BEGIN_WORLD_SWITCH,
		.world_index = index, .world_generation = gen});
}

/* Host on the overworld (slot 0, gen 100) with the guest beside it. */
static void shared_overworld(void) {
	flag(true);
	host_reset(0, 100);
	assert(clin_world_payload_bind_view(0, 1));
	attach_guest();
	guest_ready(601);
	assert(guest_session_intact() && has_entity(0, 601));
	column(0, BLOCK_STONE, 0, 100);
	spawn_item(901, 0, 100);
}

static void test_host_leaves_guest_behind(void) {
	shared_overworld();
	struct client_world_context* ow = client_ctx_registry_current(reg, 0);
	const uint32_t epoch = ow->generation;
	struct entity* guest_body = gstate.players[1].local_player;
	host_reset(1, 200); /* the host goes to the Nether */
	check(client_ctx_registry_current(reg, 0) == ow && ow->generation == epoch
			  && ow->bound_views == 2u,
		  "H1 the guest's world context stays, bound to view 1 only");
	check(has_chunk(0, 0) && has_entity(0, 901) && has_entity(0, 601),
		  "H1 the guest's terrain, entities and body stay");
	check(guest_session_intact() && gstate.players[1].local_player == guest_body,
		  "H1 the guest session (bound/attached/ready/body) is untouched");
	struct client_world_context* nether = client_ctx_registry_current(reg, 1);
	check(nether && nether->server_generation == 200 && nether->bound_views == 1u
			  && clin_world_payload_primary_slot() == 1,
		  "H1 player 0 gets the Nether slot and view 0");
	check(gstate.players[0].local_player && has_entity(1, HOST_EID)
			  && !has_entity(0, HOST_EID),
		  "H1 player 0's new body is in the Nether store only");
	column(0, BLOCK_DIRT, 0, 100);
	check(world_get_block(payload(0).terrain, 0, 0, 0).type == BLOCK_DIRT,
		  "H1 the guest's world keeps streaming after the host's switch");
}

static void unload_col(w_coord_t cx, w_coord_t cz, uint8_t index, uint32_t gen) {
	clin_process(&(struct client_rpc) {.type = CRPC_UNLOAD_CHUNK,
		.world_index = index, .world_generation = gen,
		.payload.unload_chunk = {.x = cx, .z = cz}});
}
static bool has_col(unsigned slot, w_coord_t cx, w_coord_t cz) {
	return world_find_chunk(payload(slot).terrain, cx * CHUNK_SIZE, 0,
							cz * CHUNK_SIZE) != NULL;
}
static void test_w1_host_walks_in_nether(void) {
	shared_overworld();
	column_at(3, 3, 0, 100); /* more of the guest world */
	host_reset(1, 200);		 /* the host COMMITs into the Nether */
	struct client_world_context* ow = client_ctx_registry_current(reg, 0);
	const uint32_t ow_epoch = ow->generation;
	struct entity* guest_body = gstate.players[1].local_player;
	const size_t ow_sections = dict_wsection_size(payload(0).terrain->sections);
	const int r = 4;
	for(w_coord_t cx = -r; cx <= r; cx++)
		for(w_coord_t cz = -r; cz <= r; cz++)
			column_at(cx, cz, 1, 200);
	check(dict_wsection_size(payload(1).terrain->sections)
			  == (size_t)(2 * r + 1) * (2 * r + 1),
		  "W1 the host ring (radius 4) streamed into the Nether payload");
	/* 64 blocks east = 4 columns, one column step at a time */
	for(w_coord_t step = 1; step <= 4; step++)
		for(w_coord_t cz = -r; cz <= r; cz++) {
			column_at(step + r, cz, 1, 200);
			unload_col(step - 1 - r, cz, 1, 200);
		}
	bool ring = true;
	for(w_coord_t cx = -r - 2; cx <= 4 + r + 2; cx++)
		for(w_coord_t cz = -r; cz <= r; cz++) {
			bool want = cx >= 4 - r && cx <= 4 + r;
			ring = ring && has_col(1, cx, cz) == want;
		}
	check(ring && dict_wsection_size(payload(1).terrain->sections)
					  == (size_t)(2 * r + 1) * (2 * r + 1),
		  "W1 after 64 blocks the Nether payload holds exactly the new ring");
	/* slot-1 unloads naming columns slot 0 holds never reach slot 0 */
	unload_col(3, 3, 1, 200);
	unload_col(0, 0, 1, 200);
	check(!has_col(1, 3, 3) && !has_col(1, 0, 0),
		  "W1 the slot-1 unloads applied to the Nether payload");
	/* a stale generation of slot 1 is dropped */
	column_at(-20, -20, 1, 199);
	check(!has_col(1, -20, -20), "W1 a stale-generation slot-1 column is dropped");
	check(client_ctx_registry_current(reg, 0) == ow && ow->generation == ow_epoch
			  && ow->server_generation == 100 && ow->bound_views == 2u,
		  "W1 the guest world context is untouched (same epoch, view 1)");
	check(dict_wsection_size(payload(0).terrain->sections) == ow_sections
			  && has_col(0, 0, 0) && has_col(0, 3, 3) && !has_col(0, 8, 0),
		  "W1 the guest terrain is untouched (no slot-1 column or unload "
		  "reached it)");
	check(guest_session_intact() && gstate.players[1].local_player == guest_body
			  && has_entity(0, 601) && has_entity(0, 901),
		  "W1 the guest body, entities and session are untouched");
	struct client_world_context* nether = client_ctx_registry_current(reg, 1);
	check(nether && nether->bound_views == 1u
			  && clin_world_payload_primary_slot() == 1,
		  "W1 view 0 still shows the Nether");
	/* camera_attach (camera.c) takes player 0's collision/eye world from its
	 * body: that must be the Nether terrain, never slot 0's storage */
	check(gstate.players[0].local_player
			  && gstate.players[0].local_player->world == payload(1).terrain
			  && payload(1).terrain != &gstate.world,
		  "W1 player 0's body (camera/collision world) is bound to the Nether");
}

static void test_host_leaves_alone(void) {
	flag(true);
	host_reset(0, 100);
	column(0, BLOCK_STONE, 0, 100);
	struct client_world_context* ow = client_ctx_registry_current(reg, 0);
	const uint32_t epoch = ow->generation;
	host_reset(1, 200);
	check(client_ctx_registry_current(reg, 0) == ow && ow->generation == epoch
			  && ow->bound_views == 0 && has_chunk(0, 0),
		  "H2 player 0's old world stays (unviewed) until the server empties it");
	check(client_ctx_registry_current(reg, 1)
			  && clin_world_payload_primary_slot() == 1,
		  "H2 player 0 is on the new world");
}

static void test_host_joins_guest(void) {
	flag(true);
	host_reset(0, 100);
	attach_guest();
	guest_move(1, 300); /* the guest goes to the Nether first */
	guest_ready(602);
	assert(guest_session_intact() && has_entity(1, 602));
	column(0, BLOCK_NETHERRACK, 1, 300);
	spawn_item(902, 1, 300);
	/* The guest's store already shows the host as a remote entity. */
	spawn_item(HOST_EID, 1, 300);
	struct client_world_context* nether = client_ctx_registry_current(reg, 1);
	const uint32_t epoch = nether->generation;
	host_reset(1, 300); /* the host follows into the same live Nether */
	check(client_ctx_registry_current(reg, 1) == nether
			  && nether->generation == epoch && nether->bound_views == 3u,
		  "H3 host join: same context, same epoch, both views on it");
	check(has_chunk(1, 0) && has_entity(1, 902) && has_entity(1, 602)
			  && clin_world_payload_ready(1),
		  "H3 the guest's terrain, entities and body are kept");
	check(guest_session_intact(), "H3 the guest session is untouched");
	struct entity** host = dict_entity_get(payload(1).entities, HOST_EID);
	check(host && *host == gstate.players[0].local_player
			  && (*host)->type == ENTITY_LOCAL_PLAYER,
		  "H3 player 0's local body replaces the remote copy in that store");
	check(clin_world_payload_primary_slot() == 1
			  && !(client_ctx_registry_current(reg, 0)->bound_views & 3u),
		  "H3 player 0's old world is left unviewed");
}

static void test_other_generation_is_rebuild(void) {
	flag(true);
	host_reset(0, 100);
	attach_guest();
	guest_move(1, 300);
	guest_ready(603);
	column(0, BLOCK_NETHERRACK, 1, 300);
	/* The pool may reuse an entry's address: compare epochs. */
	const uint32_t epoch = client_ctx_registry_current(reg, 1)->generation;
	host_reset(1, 301); /* a NEW generation of that slot */
	struct client_world_context* fresh = client_ctx_registry_current(reg, 1);
	check(fresh && fresh->generation != epoch && fresh->server_generation == 301
			  && fresh->bound_views == 1u && !has_chunk(1, 0),
		  "H4 another generation rebuilds the slot (no join)");
}

static void test_begin_switch_scope(void) {
	shared_overworld();
	host_reset(1, 200);
	/* Mark the host as settled in the Nether for the begin's precondition. */
	gstate.world_switch_pending = false;
	const uint32_t seq = gstate.world_switch_begin_seq;
	begin_switch(0, 100); /* stamped for the guest's world */
	check(gstate.world_switch_begin_seq == seq && !gstate.world_switch_pending,
		  "B1 a begin stamped for the guest's world is not player 0's barrier");
	begin_switch(1, 200); /* player 0's own world */
	check(gstate.world_switch_begin_seq == seq + 1,
		  "B1 a begin for player 0's world applies");
	begin_switch(0, 0); /* unstamped: player 0's, as always */
	check(gstate.world_switch_begin_seq == seq + 2,
		  "B1 an unstamped begin applies unchanged");
}

static void test_flag_off_unchanged(void) {
	flag(false);
	host_reset(0, 100);
	attach_guest();
	gstate.players[1].local_gameplay_ready = true;
	host_reset(1, 200);
	check(!client_ctx_registry_current(reg, 0)
			  && client_ctx_registry_current(reg, 1),
		  "F flag OFF: a host reset retires every other slot");
	check(!gstate.players[1].local_bound
			  && !gstate.players[1].local_gameplay_ready,
		  "F flag OFF: the guest session is cleared as before");
}

/* ---- Fable B2-A2T review fixes (RED first) ---------------------------- */

/* Mark every installed section of a slot's terrain meshed, so the near-ring
   gate reads it as drawable (no mesher runs in this test). */
static void mark_meshed(unsigned slot) {
	struct world* w = payload(slot).terrain;
	dict_wsection_it_t it;
	for(dict_wsection_it(it, w->sections); !dict_wsection_end_p(it);
		dict_wsection_next(it)) {
		struct world_section* sec = &dict_wsection_ref(it)->value;
		for(size_t k = 0; k < COLUMN_HEIGHT; k++)
			if(sec->column[k]) {
				sec->column[k]->rebuild_displist = false;
				sec->column[k]->mesh_in_flight = false;
			}
	}
}

/* F2: the loading screen judges PLAYER 0's world, not gstate.world. */
static void test_f2_load_gate_judges_host_world(void) {
	shared_overworld(); /* guest + host on slot 0 (gstate storage) */
	world_set_server_view_radius(1);
	for(w_coord_t cx = -1; cx <= 1; cx++)
		for(w_coord_t cz = -1; cz <= 1; cz++)
			column_at(cx, cz, 0, 100);
	mark_meshed(0);
	host_reset(1, 200); /* host -> Nether (extra slot), guest stays */
	gstate.players[0].camera.x = 8.0F;
	gstate.players[0].camera.z = 8.0F;
	gstate.world_loaded = true;
	gstate.world_load_failed = false;
	gstate.remote_terrain_hold = false;
	lworld_reset_presentation();
	check(!lworld_prepare_presentation(),
		  "F2 the host's EMPTY Nether is not ready, whatever the guest's "
		  "overworld holds");
	for(w_coord_t cx = -1; cx <= 1; cx++)
		for(w_coord_t cz = -1; cz <= 1; cz++)
			column_at(cx, cz, 1, 200);
	mark_meshed(1);
	check(lworld_prepare_presentation(),
		  "F2 ready once the host's own near ring is installed and meshed");
	world_set_server_view_radius(0);
	gstate.world_loaded = false;
	lworld_reset_presentation();
}

/* F4: a stamped reset OLDER than the slot's live world is dropped. */
static void test_f4_superseded_reset_dropped(void) {
	shared_overworld();
	host_reset(1, 200); /* the guest keeps slot 0 at gen 100 */
	struct client_world_context* ow = client_ctx_registry_current(reg, 0);
	const uint32_t epoch = ow->generation;
	guest_move(0, 99); /* an older guest world for the same slot */
	check(client_ctx_registry_current(reg, 0) == ow && ow->generation == epoch
			  && has_chunk(0, 0) && guest_session_intact(),
		  "F4 an older-generation guest reset is dropped");
	host_reset(0, 99); /* an older host reset naming the guest's slot */
	check(client_ctx_registry_current(reg, 0) == ow && ow->generation == epoch
			  && has_chunk(0, 0) && clin_world_payload_primary_slot() == 1,
		  "F4 an older-generation host reset is dropped (primary unchanged)");
}

/* F5: the host rebuilding a slot whose guest install is being retried at the
   same generation binds the guest's view too. */
static void test_f5_host_rebuild_keeps_guest_install(void) {
	flag(true);
	host_reset(0, 100);
	attach_guest();
	/* Fill the context pool with leased, retiring slot-1 contexts. */
	struct client_world_context* pinned[CLIENT_CTX_POOL];
	unsigned n = 0;
	assert(client_ctx_registry_install(reg, WORLD_DIM_NETHER, 1, 290));
	while(client_ctx_registry_in_use(reg) < CLIENT_CTX_POOL) {
		struct client_world_context* c = client_ctx_registry_current(reg, 1);
		assert(client_ctx_lease_acquire(c, CLIENT_CTX_LEASE_MESHER));
		pinned[n++] = c;
		assert(client_ctx_registry_install(reg, WORLD_DIM_NETHER, 1, 290));
	}
	struct client_world_context* last = client_ctx_registry_current(reg, 1);
	assert(client_ctx_lease_acquire(last, CLIENT_CTX_LEASE_MESHER));
	pinned[n++] = last;
	guest_move(1, 300); /* gate 1: install fails, retry armed for gen 300 */
	assert(!client_ctx_registry_current(reg, 1));
	for(unsigned k = 0; k < n; k++)
		assert(client_ctx_registry_release(reg, pinned[k],
										   CLIENT_CTX_LEASE_MESHER));
	host_reset(1, 300); /* the host lands in the guest's pending world */
	struct client_world_context* nether = client_ctx_registry_current(reg, 1);
	check(nether && nether->server_generation == 300
			  && nether->bound_views == 3u,
		  "F5 the host's rebuild keeps the guest's pending install: both "
		  "views on the new context");
	clin_update_drain(); /* the retry must not re-install over it */
	check(client_ctx_registry_current(reg, 1) == nether
			  && nether->bound_views == 3u,
		  "F5 no stale retry replaces it afterwards");
}

/* F1 (client half): an explicit, stamped world retire. */
static void retire(uint8_t index, uint32_t gen) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RELEASE,
		.world_index = index, .world_generation = gen});
}

static void test_f1_world_retire(void) {
	flag(true);
	host_reset(0, 100);
	column(0, BLOCK_STONE, 0, 100);
	host_reset(1, 200); /* the host leaves alone: slot 0 unviewed */
	retire(0, 99);
	check(client_ctx_registry_current(reg, 0) && has_chunk(0, 0),
		  "F1 a retire for another generation is ignored");
	retire(1, 200);
	check(client_ctx_registry_current(reg, 1)
			  && clin_world_payload_primary_slot() == 1,
		  "F1 player 0's own world is never retired");
	retire(0, 100);
	check(!client_ctx_registry_current(reg, 0) && !clin_world_payload_ready(0),
		  "F1 the vacated, unviewed, body-less world is released");

	shared_overworld();
	host_reset(1, 200); /* the guest stays in slot 0 */
	retire(0, 100);
	check(client_ctx_registry_current(reg, 0) && guest_session_intact()
			  && has_chunk(0, 0),
		  "F1 a world a view still shows is never retired");

	flag(false);
	host_reset(0, 100);
	struct client_world_context* c = client_ctx_registry_current(reg, 0);
	retire(0, 100);
	check(client_ctx_registry_current(reg, 0) == c,
		  "F1 flag OFF: a retire is ignored");
}


/* ===== D2 (ASTRA-DEFECTS-RULING 7-10): authority-validated guest binding ===
 * The production order the PC smoke recorded (boat tape
 * vs_pc_boat_20261004_082010 seq 187-207): the guest Leaves and re-Joins
 * (ACCEPT) while the host is still in the Overworld, the HOST transfers to
 * the Nether before the guest's GAMEPLAY_READY, and the server admits the
 * guest into the Overworld {slot 0}. The client, finding no view-1 slot
 * (LEFT unbound it), bound view 1 and installed the body in player 0's
 * CURRENT world -- the Nether -- so the guest's prediction collided with
 * Nether terrain (a 0.4-0.6 m push-out) while the server kept it in the
 * Overworld. READY now names the authoritative world; the client installs
 * and binds there or not at all. */
static void guest_leave(void) {
	struct client_session* g = &gstate.players[1];
	g->local_phase = CLIENT_GUEST_LEAVING;
	clin_process(&(struct client_rpc) {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = g->local_ticket,
			.generation = g->local_generation, .owner = 1,
			.result = LOCAL_GUEST_LEFT}});
}

static void attach_guest_gen(uint64_t gen) {
	attach_guest();
	gstate.players[1].local_generation = gen;
	gstate.players[1].local_gameplay_ready = false;
}

/* The production READY: stamped with the guest actor's world. */
static void guest_ready_at(uint32_t eid, uint64_t gen, uint8_t wslot,
						   uint32_t wgen) {
	clin_process(&(struct client_rpc) {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 5, .generation = gen, .owner = 1,
			.result = LOCAL_GUEST_GAMEPLAY_READY, .entity_id = eid,
			.health = MAX_PLAYER_HEALTH, .x = 8.5, .y = 10.0, .z = 8.5,
			.world_slot = wslot, .world_generation = wgen,
			.world_dimension = wslot ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD}});
}

/* Chunk (0,0) of that world solid stone at every height for x 0..8 (the
 * guest's READY cell x=8 included), open from x=9: a body predicting against
 * it is pushed out sideways (+X), exactly the smoke's push-out signature. */
static void solid_column(uint8_t index, uint32_t gen) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	for(size_t lx = 0; lx <= 8; lx++)
		for(size_t lz = 0; lz < CHUNK_SIZE; lz++)
			for(size_t y = 0; y < WORLD_HEIGHT; y++)
				slot->ids[(lx * CHUNK_SIZE + lz) * WORLD_HEIGHT + y] = BLOCK_STONE;
	clin_process(&(struct client_rpc) {.type = CRPC_CHUNK,
		.world_index = index, .world_generation = gen,
		.payload.chunk = {.x = 0, .y = 0, .z = 0, .sx = CHUNK_SIZE,
			.sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}});
}

/* The guest body lives in `slot`'s store AND predicts/collides against that
 * world's terrain. */
static bool guest_body_in(unsigned slot) {
	struct entity* body = gstate.players[1].local_player;
	if(!body)
		return false;
	struct entity** pp = dict_entity_get(payload(slot).entities, body->id);
	return pp && *pp == body && body->world == payload(slot).terrain;
}

/* Production guest walk (main.c's guest path), neutral connected frame: a
 * resting guest must not be shoved horizontally by foreign terrain. */
static bool guest_pose_stable(unsigned ticks) {
	struct entity* b = gstate.players[1].local_player;
	if(!b)
		return false;
	const float x = b->pos[0], z = b->pos[2];
	struct input_player_frame f;
	memset(&f, 0, sizeof(f));
	f.connected = true;
	for(unsigned i = 0; i < ticks; i++)
		(void)entity_local_player_walk_owned(b, &gstate.players[1], &f);
	return fabsf(b->pos[0] - x) < 1e-4F && fabsf(b->pos[2] - z) < 1e-4F;
}

static bool host_intact_in(unsigned slot) {
	return clin_world_view_slot(0) == (int)slot
		&& gstate.players[0].local_player && has_entity(slot, HOST_EID);
}

/* The PC-smoke order: Leave, host transfer, re-READY into the Overworld. */
static void d2_host_transfer_guest_retained(void) {
	shared_overworld();
	guest_leave();
	check(!gstate.players[1].local_bound && !gstate.players[1].local_player
			  && clin_world_view_slot(1) < 0,
		  "D2 setup: LEFT retires the guest body and unbinds view 1");
	host_reset(1, 200); /* the host transfers to the Nether */
	solid_column(1, 200);
	attach_guest_gen(10); /* the re-Join reached ATTACHED */
	guest_ready_at(602, 10, 0, 100);
	check(clin_world_view_slot(1) == 0,
		  "D2 host transfer + readmission: view 1 binds the guest's "
		  "authoritative world (slot 0), not the host's Nether");
	check(guest_body_in(0) && !has_entity(1, 602),
		  "D2 host transfer + readmission: body + collision terrain are the "
		  "Overworld's, nothing in the Nether store");
	check(gstate.players[1].local_gameplay_ready,
		  "D2 host transfer + readmission: gameplay admitted");
	check(guest_pose_stable(20),
		  "D2 host transfer + readmission: resting guest pose is stable "
		  "(no push-out by Nether terrain)");
	check(host_intact_in(1),
		  "D2 host transfer + readmission: host view 0 + body stay in the Nether");
}

static void d2_duplicate_stale_and_reuse(void) {
	d2_host_transfer_guest_retained();
	struct entity* body = gstate.players[1].local_player;
	guest_ready_at(602, 10, 0, 100); /* duplicate READY */
	check(gstate.players[1].local_player == body && clin_world_view_slot(1) == 0
			  && guest_body_in(0) && gstate.players[1].local_gameplay_ready,
		  "D2 duplicate READY: idempotent, same body, same world");
	guest_ready_at(602, 10, 0, 99); /* stale generation of the guest's slot */
	check(gstate.players[1].local_player == body && clin_world_view_slot(1) == 0
			  && guest_body_in(0) && gstate.players[1].local_gameplay_ready,
		  "D2 stale-generation READY never moves an admitted guest");
	guest_move(0, 99); /* a stale guest-world reset (older than live) */
	check(clin_world_view_slot(1) == 0 && guest_body_in(0),
		  "D2 stale guest-world reset is dropped");
	/* slot-generation reuse: a fresh admission stamped for an identity the
	 * slot no longer has (slot 1 was gen 200, now rebuilt as gen 201) */
	guest_leave();
	host_reset(1, 201);
	attach_guest_gen(11);
	guest_ready_at(606, 11, 1, 200);
	check(!gstate.players[1].local_player && !gstate.players[1].local_gameplay_ready
			  && clin_world_view_slot(1) < 0 && !has_entity(1, 606),
		  "D2 slot-generation reuse: READY for a retired generation installs "
		  "nothing and binds no view");
	check(host_intact_in(1), "D2 slot-generation reuse: host untouched");
}

/* READY delivered while the guest's world has no live client context yet
 * (paused/pending delivery): held, then admitted into that world once it is
 * installed -- never into player 0's world meanwhile. */
static void d2_pending_delivery(void) {
	shared_overworld();
	guest_leave();
	host_reset(1, 200);
	solid_column(1, 200);
	retire(0, 100); /* the vacated Overworld is released client-side */
	check(!client_ctx_registry_current(reg, 0),
		  "D2 pending setup: the guest's old world is gone client-side");
	attach_guest_gen(12);
	guest_ready_at(607, 12, 0, 101);
	check(!gstate.players[1].local_player && !gstate.players[1].local_gameplay_ready
			  && clin_world_view_slot(1) < 0 && !has_entity(1, 607),
		  "D2 pending delivery: READY before its world exists is held "
		  "(no body, no view, nothing in the host's world)");
	guest_move(0, 101); /* the guest world's gate-1 install arrives */
	column(0, BLOCK_STONE, 0, 101);
	guest_ready_at(607, 12, 0, 101); /* the client's GAMEPLAY retry */
	check(clin_world_view_slot(1) == 0 && guest_body_in(0)
			  && gstate.players[1].local_gameplay_ready,
		  "D2 pending delivery: the retried READY lands in the guest's world");
	check(guest_pose_stable(20), "D2 pending delivery: resting pose stable");
	check(host_intact_in(1), "D2 pending delivery: host untouched");
}

/* The host returns to the guest's world (HOST JOIN): both share slot 0. */
static void d2_return_transfer(void) {
	d2_host_transfer_guest_retained();
	struct entity* body = gstate.players[1].local_player;
	host_reset(0, 100);
	check(clin_world_view_slot(0) == 0 && clin_world_view_slot(1) == 0
			  && gstate.players[1].local_player == body && guest_body_in(0),
		  "D2 return transfer: host joins the guest's world; guest binding, "
		  "body and terrain unchanged");
	check(host_intact_in(0), "D2 return transfer: host body in slot 0");
	check(guest_pose_stable(10), "D2 return transfer: guest pose stable");
}

/* A legitimate GUEST transfer into the host's world. */
static void d2_guest_transfer(void) {
	d2_host_transfer_guest_retained();
	guest_move(1, 200); /* gate 1, join variant: into player 0's world */
	check(!gstate.players[1].local_player && clin_world_view_slot(1) == 1,
		  "D2 guest transfer: gate 1 moves view 1 to the host's world");
	guest_ready_at(608, 10, 1, 200);
	check(clin_world_view_slot(1) == 1 && guest_body_in(1)
			  && !has_entity(0, 608) && gstate.players[1].local_gameplay_ready,
		  "D2 guest transfer: body + terrain follow the authoritative move");
	check(host_intact_in(1), "D2 guest transfer: host untouched");
}

/* ===== O07 / R04 D2-1..D2-3 (real handlers) ===============================
 * The production READY/reset carry the server's monotonic guest-binding
 * sequence (server_local_guest_bind_seq): the admission bumps it, every guest
 * gate-1 reset bumps it, READY carries the current value. */
static void guest_ready_full(uint32_t eid, uint64_t gen, uint8_t wslot,
							 uint32_t wgen, int8_t dim, uint32_t bind_seq,
							 uint32_t correction) {
	clin_process(&(struct client_rpc) {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 5, .generation = gen, .owner = 1,
			.result = LOCAL_GUEST_GAMEPLAY_READY, .entity_id = eid,
			.health = MAX_PLAYER_HEALTH, .x = 8.5, .y = 10.0, .z = 8.5,
			.correction_seq = correction,
			.world_slot = wslot, .world_generation = wgen,
			.world_dimension = dim, .guest_bind_seq = bind_seq}});
}

static void guest_move_seq(uint8_t index, uint32_t gen, uint32_t bind_seq) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = index ? WORLD_DIM_NETHER
												  : WORLD_DIM_OVERWORLD,
			.local_entity = HOST_EID, .world_index = index,
			.world_generation = gen, .guest_world = true, .guest_view = 1,
			.guest_bind_seq = bind_seq}});
}

/* D2-1: a live world identity is not current ownership. After a REAL guest
 * transfer (gate-1 + READY into slot 1), the OLD READY and the OLD reset --
 * both naming the still-live Overworld -- are replayed; neither may move the
 * guest back. A NEWER reset/READY (a legitimate move back) still applies. */
static void d2_stale_replay_after_guest_transfer(void) {
	flag(true);
	host_reset(0, 100);
	attach_guest_gen(9);
	guest_ready_full(601, 9, 0, 100, WORLD_DIM_OVERWORLD, 5, 0);
	check(clin_world_view_slot(1) == 0 && guest_body_in(0),
		  "D2-1 setup: stamped admission into the Overworld");
	guest_move_seq(1, 200, 6); /* the guest's own transfer to the Nether */
	guest_ready_full(602, 9, 1, 200, WORLD_DIM_NETHER, 6, 0);
	struct entity* moved = gstate.players[1].local_player;
	check(clin_world_view_slot(1) == 1 && guest_body_in(1),
		  "D2-1 setup: the guest transferred into slot 1");
	guest_ready_full(601, 9, 0, 100, WORLD_DIM_OVERWORLD, 5, 0); /* old READY */
	check(clin_world_view_slot(1) == 1 && gstate.players[1].local_player == moved
			  && guest_body_in(1) && !has_entity(0, 601),
		  "D2-1 stale READY replay (older binding) leaves the guest in slot 1");
	guest_move_seq(0, 100, 5); /* old reset, join-primary variant */
	check(clin_world_view_slot(1) == 1 && gstate.players[1].local_player == moved
			  && guest_body_in(1),
		  "D2-1 stale guest reset replay (older binding) is dropped");
	guest_move_seq(0, 100, 7); /* a NEWER, legitimate move back */
	guest_ready_full(603, 9, 0, 100, WORLD_DIM_OVERWORLD, 7, 0);
	check(clin_world_view_slot(1) == 0 && guest_body_in(0),
		  "D2-1 a newer reset + READY still moves the guest (both directions)");
	check(host_intact_in(0), "D2-1 host untouched");
}

/* D2-2: a READY rejected as stale must not touch the movement token. */
static void d2_stale_ready_keeps_token(void) {
	flag(true);
	host_reset(0, 100);
	attach_guest_gen(9);
	guest_ready_full(604, 9, 0, 100, WORLD_DIM_OVERWORLD, 11, 20); /* bind seqs only grow (one server) */
	check(gstate.players[1].player_sync.correction_seq == 20
			  && gstate.players[1].local_gameplay_ready,
		  "D2-2 setup: admitted with movement token 20");
	guest_ready_full(604, 9, 0, 99, WORLD_DIM_OVERWORLD, 11, 0); /* dead gen */
	check(gstate.players[1].player_sync.correction_seq == 20
			  && gstate.players[1].local_gameplay_ready && guest_body_in(0),
		  "D2-2 stale-world READY does not rewind the token (stays 20)");
	guest_ready_full(604, 9, 0, 100, WORLD_DIM_OVERWORLD, 10, 0); /* old seq */
	check(gstate.players[1].player_sync.correction_seq == 20,
		  "D2-2 stale-binding READY does not rewind the token (stays 20)");
}

/* D2-3: the announced dimension must agree with the live world. */
static void d2_dimension_mismatch_rejected(void) {
	flag(true);
	host_reset(0, 100);
	attach_guest_gen(9);
	guest_ready_full(605, 9, 0, 100, WORLD_DIM_NETHER, 12, 0);
	check(!gstate.players[1].local_player && !gstate.players[1].local_gameplay_ready
			  && clin_world_view_slot(1) < 0 && !has_entity(0, 605),
		  "D2-3 READY naming slot 0/gen 100 as the Nether is rejected");
	check(host_intact_in(0), "D2-3 host untouched");
}

/* ===== O10 / R05 (real handlers) =========================================
 * The client fence is process-wide (clin_init resets it), so these run last
 * and keep their sequences above every earlier case. */
static uint32_t token(void) {
	return gstate.players[1].player_sync.correction_seq;
}

/* R05 D2-1a (P1): a DUPLICATE of the reset that already installed the
 * guest's own world (a world only the guest views) is idempotent: same body,
 * same terrain, gameplay kept. */
static void r05_duplicate_reset_idempotent(void) {
	flag(true);
	host_reset(0, 100);
	attach_guest_gen(13);
	guest_ready_full(611, 13, 0, 100, WORLD_DIM_OVERWORLD, 20, 0);
	guest_move_seq(1, 200, 21); /* the guest's own move to the Nether */
	column_at(0, 0, 1, 200);
	guest_ready_full(612, 13, 1, 200, WORLD_DIM_NETHER, 21, 30);
	struct entity* body = gstate.players[1].local_player;
	check(body && clin_world_view_slot(1) == 1 && guest_body_in(1)
			  && has_col(1, 0, 0) && gstate.players[1].local_gameplay_ready,
		  "R05 D2-1a setup: guest body, terrain and gameplay in slot 1");
	guest_move_seq(1, 200, 21); /* the identical reset, delivered again */
	check(gstate.players[1].local_player == body && guest_body_in(1)
			  && has_col(1, 0, 0) && gstate.players[1].local_gameplay_ready
			  && clin_world_view_slot(1) == 1 && token() == 30,
		  "R05 D2-1a duplicate reset is idempotent (body, terrain, gameplay, "
		  "view and token kept)");
	check(host_intact_in(0), "R05 D2-1a host untouched");
}

/* R05 D2-1b (P3): a reset the handler REFUSES never advances the binding
 * fence -- the next valid binding still applies. Each refusal branch:
 * superseded generation, join-primary identity refusal, generic drop. */
static void r05_rejected_reset_keeps_fence(void) {
	/* continues from the duplicate case: guest in {1,200}, fence 21 */
	guest_move_seq(1, 199, 40); /* superseded generation of the live slot */
	check(clin_world_view_slot(1) == 1 && guest_body_in(1),
		  "R05 D2-1b superseded-world reset (seq 40) is refused");
	guest_move_seq(0, 101, 41); /* names player 0's slot at a foreign gen */
	check(clin_world_view_slot(1) == 1 && guest_body_in(1),
		  "R05 D2-1b join-primary reset for a foreign generation is refused");
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_NETHER,
			.local_entity = HOST_EID, .world_index = 2,
			.world_generation = 300, .guest_world = true, .guest_view = 2,
			.guest_bind_seq = 42}}); /* no such guest view: dropped */
	check(clin_world_view_slot(1) == 1 && guest_body_in(1),
		  "R05 D2-1b malformed guest reset is refused");
	guest_move_seq(0, 100, 22); /* the next VALID binding: back home */
	guest_ready_full(613, 13, 0, 100, WORLD_DIM_OVERWORLD, 22, 32);
	check(clin_world_view_slot(1) == 0 && guest_body_in(0) && token() == 32
			  && gstate.players[1].local_gameplay_ready,
		  "R05 D2-1b refused resets did not poison the fence: the valid "
		  "return (seq 22) applies, token 32");
	check(host_intact_in(0), "R05 D2-1b host untouched");
}

/* R05 P3 wrap: the producer wraps UINT32_MAX -> 1 (0 = unstamped); the
 * consumer orders sequences by serial-number arithmetic, so the successor
 * of the maximum is NEWER and the maximum, replayed, is older. */
static void r05_bind_seq_wraps(void) {
	/* Walk the fence up to the maximum in steps a real producer could make
	 * (each within 2^31 of the last: serial order needs that window). */
	guest_move_seq(1, 200, UINT32_C(0x7FFFFFF0));
	guest_ready_full(616, 13, 1, 200, WORLD_DIM_NETHER, 0x7FFFFFF0u, 33);
	guest_move_seq(0, 100, UINT32_C(0xFFFFFFE0));
	guest_ready_full(617, 13, 0, 100, WORLD_DIM_OVERWORLD, 0xFFFFFFE0u, 34);
	check(clin_world_view_slot(1) == 0 && guest_body_in(0) && token() == 34,
		  "R05 wrap setup: stepped bindings up to 0xFFFFFFE0 apply");
	guest_move_seq(1, 200, UINT32_MAX);
	guest_ready_full(614, 13, 1, 200, WORLD_DIM_NETHER, UINT32_MAX, 40);
	check(clin_world_view_slot(1) == 1 && guest_body_in(1) && token() == 40,
		  "R05 wrap setup: binding UINT32_MAX applied");
	guest_move_seq(0, 100, 1); /* the producer's successor of UINT32_MAX */
	guest_ready_full(615, 13, 0, 100, WORLD_DIM_OVERWORLD, 1, 41);
	check(clin_world_view_slot(1) == 0 && guest_body_in(0) && token() == 41,
		  "R05 wrap: successor 1 of UINT32_MAX applies (view 0, token 41)");
	guest_move_seq(1, 200, UINT32_MAX); /* the pre-wrap binding, replayed */
	check(clin_world_view_slot(1) == 0 && guest_body_in(0) && token() == 41,
		  "R05 wrap: the pre-wrap UINT32_MAX reset is now stale");
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
	test_host_leaves_guest_behind();
	test_host_leaves_alone();
	test_w1_host_walks_in_nether();
	test_host_joins_guest();
	test_other_generation_is_rebuild();
	test_begin_switch_scope();
	test_flag_off_unchanged();
	test_f2_load_gate_judges_host_world();
	test_f4_superseded_reset_dropped();
	test_f5_host_rebuild_keeps_guest_install();
	test_f1_world_retire();
	/* D2 (ASTRA-DEFECTS-RULING 7-10) */
	d2_host_transfer_guest_retained();
	d2_duplicate_stale_and_reuse();
	d2_pending_delivery();
	d2_return_transfer();
	d2_guest_transfer();
	/* O07 / R04 */
	d2_stale_replay_after_guest_transfer();
	d2_stale_ready_keeps_token();
	d2_dimension_mismatch_rejected();
	/* O10 / R05 */
	r05_duplicate_reset_idempotent();
	r05_rejected_reset_keeps_fence();
	r05_bind_seq_wraps();
	flag(false);
	printf("%s test_b2_a2t_client (%u failure%s)\n", failures ? "FAIL" : "PASS",
		   failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
