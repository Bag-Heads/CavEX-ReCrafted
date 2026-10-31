/* Secondary-world publication contract, through real transfer, client
 * contexts and SRPC_WORLD_RESEND. This is RED until the slot-1 streamer
 * publishes CHUNK -> signs -> resident entities with resumable backpressure. */
#include "support/owner_swap_harness.h"

#include "entity/entity.h"
#include "platform/input_owner.h"
#include "item/window_container.h"
#include "entity/entity_primed_tnt.h"
#include "entity/monsters_object.h"
#include "game/sign_text_store.h"
#include "network/client_world_payload.h"
#include "network/client_admission.h"
#include "network/client_frame_worlds.h"
#include "network/piston_moving_transport.h"
#include "network/server_transfer.h"
#include "game/gui/screen_death.h"
#include "chunk_mesher.h"
#include "particle.h"
#include "network/server_dispenser.h"
#include "network/inventory_logic.h"

#define PUB_X 5
#define PUB_Y 70
#define PUB_Z 5
#define PUB_SIGNS 18

static uint32_t monster_ids[2];
static unsigned pub_chunk, pub_signs, pub_monsters, pub_resets, pub_unloads;
static uint32_t live_item_id, live_monster_id, live_tnt_id;
static unsigned live_items, live_monsters, live_tnts;
static unsigned live_monster_moves, live_monster_destroys;
static uint8_t live_move_slot, live_destroy_slot;
static uint32_t live_move_gen, live_destroy_gen;
static unsigned guest_egg_spawns, guest_fish_spawns;
static uint32_t guest_egg_id, guest_fish_id;
static uint8_t guest_egg_slot, guest_fish_slot;
static uint32_t guest_egg_gen, guest_fish_gen;
static unsigned fireball_spawns, snowball_spawns, nether_explosions;
static unsigned projectile_spawns;
static struct client_rpc projectile_spawn;
static uint16_t projectile_watch_item;
static unsigned dispenser_items, dispenser_smoke;
static struct client_rpc dispenser_item, dispenser_puff;
static uint32_t fireball_id, fireball_gen, explosion_gen;
static uint8_t fireball_slot, explosion_slot;
static uint32_t snowball_id;
static uint32_t crossing_monster_id;
static unsigned crossing_spawns;
static uint32_t crossing_newer_id;
static unsigned crossing_newer_spawns;
static bool wrong_stamp, publication_out_of_order;
static uint32_t pub_gen, ow_gen;
static bool watch_vacant_target;
static w_coord_t vacant_target_cx, vacant_target_cz;
static unsigned vacant_target_chunks;
static bool secondary_watch;
static unsigned secondary_resets, secondary_markers, secondary_terminal;
static unsigned secondary_host_chunks, secondary_ow_chunks;

static void pub_observe(const struct client_rpc* call, void* user) {
	consumed(call, user);
	if(secondary_watch) {
		if(call->type == CRPC_STREAM_RESET) secondary_resets++;
		if(call->type == CRPC_PLAYER_POS && call->payload.player_pos.input_epoch)
			secondary_markers++;
		if(call->type == CRPC_LOAD_FAILED) secondary_terminal++;
		if(call->type == CRPC_CHUNK) {
			if(call->world_index == 1 && call->world_generation) secondary_host_chunks++;
			else secondary_ow_chunks++;
		}
	}
	if(call->type == CRPC_SPAWN_ARROW && !call->payload.spawn_arrow.fireball
	   && (!projectile_watch_item
		   || call->payload.spawn_arrow.item_id == projectile_watch_item)) {
		projectile_spawns++;
		projectile_spawn = *call;
	}
	if(call->type == CRPC_SPAWN_ITEM
	   && call->payload.spawn_item.item.id == ITEM_STICK) {
		dispenser_items++;
		dispenser_item = *call;
	}
	if(call->type == CRPC_SMOKE_PUFF
	   && call->payload.smoke_puff.intensity < 0.0F) {
		dispenser_smoke++;
		dispenser_puff = *call;
	}
	if(call->type == CRPC_STREAM_RESET && call->world_index == 1)
		pub_resets++;
	if(call->type == CRPC_UNLOAD_CHUNK && call->world_index == 1
	   && call->payload.unload_chunk.x == 0
	   && call->payload.unload_chunk.z == 0)
		pub_unloads++;
	if(call->type == CRPC_ENTITY_MOVE
	   && call->payload.entity_move.entity_id == live_monster_id) {
		live_monster_moves++;
		live_move_slot = call->world_index;
		live_move_gen = call->world_generation;
	}
	if(call->type == CRPC_ENTITY_DESTROY
	   && call->payload.entity_destroy.entity_id == live_monster_id) {
		live_monster_destroys++;
		live_destroy_slot = call->world_index;
		live_destroy_gen = call->world_generation;
	}
	if(call->type == CRPC_SPAWN_ARROW
	   && call->payload.spawn_arrow.item_id == ITEM_EGG) {
		guest_egg_spawns++;
		guest_egg_id = call->payload.spawn_arrow.entity_id;
		guest_egg_slot = call->world_index;
		guest_egg_gen = call->world_generation;
	}
	if(call->type == CRPC_SPAWN_ARROW
	   && call->payload.spawn_arrow.item_id == ITEM_FISHING_ROD) {
		guest_fish_spawns++;
		guest_fish_id = call->payload.spawn_arrow.entity_id;
		guest_fish_slot = call->world_index;
		guest_fish_gen = call->world_generation;
	}
	if(call->type == CRPC_SPAWN_ARROW
	   && call->payload.spawn_arrow.fireball
	   && call->world_generation == (call->world_index == 1
		? pub_gen : ow_gen)) {
		fireball_spawns++;
		fireball_id = call->payload.spawn_arrow.entity_id;
		fireball_slot = call->world_index;
		fireball_gen = call->world_generation;
	}
	if(call->type == CRPC_SPAWN_ARROW
	   && call->payload.spawn_arrow.item_id == ITEM_SNOW_BALL
	   && !call->payload.spawn_arrow.fireball
	   && call->world_index == 0) {
		snowball_spawns++;
		snowball_id = call->payload.spawn_arrow.entity_id;
	}
	if(call->type == CRPC_ENTITY_EXPLODE) {
		nether_explosions++;
		explosion_slot = call->world_index;
		explosion_gen = call->world_generation;
	}
	if(call->type == CRPC_SPAWN_MONSTER
	   && call->payload.spawn_monster.entity_id == crossing_monster_id)
		crossing_spawns++;
	if(call->type == CRPC_SPAWN_MONSTER
	   && call->payload.spawn_monster.entity_id == crossing_newer_id)
		crossing_newer_spawns++;
	if(call->type != CRPC_CHUNK && call->type != CRPC_SIGN_TEXT
	   && call->type != CRPC_SPAWN_MONSTER && call->type != CRPC_SPAWN_ITEM
	   && call->type != CRPC_SPAWN_PRIMED_TNT)
		return;
	if(watch_vacant_target && call->type == CRPC_CHUNK
	   && call->world_index == 0
	   && WCOORD_CHUNK_OFFSET(call->payload.chunk.x) == vacant_target_cx
	   && WCOORD_CHUNK_OFFSET(call->payload.chunk.z) == vacant_target_cz)
		vacant_target_chunks++;
	if(call->world_index == 1) {
		wrong_stamp |= call->world_generation != pub_gen;
		if(call->type == CRPC_CHUNK
		   && call->payload.chunk.x == 0 && call->payload.chunk.z == 0)
			pub_chunk++;
		if(call->type == CRPC_SIGN_TEXT
		   && call->payload.sign_text.x >= PUB_X
		   && call->payload.sign_text.x < PUB_X + 6
		   && call->payload.sign_text.z >= PUB_Z
		   && call->payload.sign_text.z < PUB_Z + 3) {
			publication_out_of_order |= pub_chunk == 0;
			pub_signs++;
		}
		if(call->type == CRPC_SPAWN_MONSTER
		   && (call->payload.spawn_monster.entity_id == monster_ids[0]
			   || call->payload.spawn_monster.entity_id == monster_ids[1])) {
			publication_out_of_order |= pub_chunk == 0;
			pub_monsters++;
		}
		if(call->type == CRPC_SPAWN_ITEM
		   && call->payload.spawn_item.entity_id == live_item_id)
			live_items++;
		if(call->type == CRPC_SPAWN_MONSTER
		   && call->payload.spawn_monster.entity_id == live_monster_id)
			live_monsters++;
		if(call->type == CRPC_SPAWN_PRIMED_TNT
		   && call->payload.spawn_primed_tnt.entity_id == live_tnt_id)
			live_tnts++;
	}
}

static struct clin_world_payload client_payload(unsigned slot) {
	struct clin_world_payload p;
	assert(clin_world_payload_of_slot(slot, &p));
	return p;
}

static bool client_sign(unsigned slot, int x, int z, char c) {
	char grid[SIGN_SIZE];
	struct sign_text_store* old = sign_text_store_select(client_payload(slot).signs);
	bool found = sign_text_store_get(x, PUB_Y, z, grid);
	sign_text_store_select(old);
	return found && grid[0] == c;
}

static bool server_sign(struct server_local* s, struct server_world* w,
	char c) {
	struct te_store store = server_world_tile_store(w);
	int row = te_store_find(&store, TE_KIND_SIGN, PUB_X, PUB_Y, PUB_Z);
	return row >= 0 && s->sign_texts[row][0] == c;
}

static bool client_monster(unsigned slot, uint32_t id) {
	struct entity** ptr = dict_entity_get(client_payload(slot).entities, id);
	return ptr && *ptr && (*ptr)->type == ENTITY_MONSTER;
}

static bool client_entity(unsigned slot, uint32_t id, enum entity_type type) {
	struct entity** ptr = dict_entity_get(client_payload(slot).entities, id);
	return ptr && *ptr && (*ptr)->type == type;
}

static bool send_scoped_resend(unsigned slot, uint32_t generation) {
	struct server_rpc call = {.type = SRPC_WORLD_RESEND,
		.payload.world_resend = {.world_index = slot,
			.world_generation = generation}};
	return svin_rpc_send(&call);
}

static void setup_server_signs(struct server_local* s, struct server_world* w,
	char letter, unsigned count) {
	struct server_chunk* sc = NULL;
	bool populated = false;
	for(unsigned n = 0; n < 1000 && !populated; n++) {
		server_world_begin_update(w);
		populated = server_world_ensure_populated(w, s, 0, 0, &sc);
	}
	assert(populated && sc);
	struct te_store store = server_world_tile_store(w);
	for(unsigned k = 0; k < count; k++) {
		int x = PUB_X + (int)(k % 6), z = PUB_Z + (int)(k / 6);
		assert(server_world_set_block(w, x, PUB_Y - 1, z,
			(struct block_data){.type = BLOCK_STONE}));
	}
	for(unsigned k = 0; k < count; k++) {
		int x = PUB_X + (int)(k % 6), z = PUB_Z + (int)(k / 6);
		assert(server_world_set_block(w, x, PUB_Y, z,
			(struct block_data){.type = BLOCK_SIGN_POST, .metadata = 0}));
		int row = te_store_claim_free(&store, TE_KIND_SIGN, x, PUB_Y, z);
		assert(row >= 0);
		memset(s->sign_texts[row], ' ', SIGN_SIZE);
		s->sign_texts[row][0] = letter;
	}
	printf("PUB fixture dim=%d sign_rows=%d\n", (int)w->dimension,
		te_store_count_owned(&store, TE_KIND_SIGN));
}

static void seed_overworld_client_sign(struct server_local* s) {
	struct client_rpc call = {.type = CRPC_SIGN_TEXT};
	clin_rpc_stamp(&call, 0, s->worlds[0].generation);
	call.payload.sign_text.x = PUB_X;
	call.payload.sign_text.y = PUB_Y;
	call.payload.sign_text.z = PUB_Z;
	memset(call.payload.sign_text.grid, ' ', SIGN_SIZE);
	call.payload.sign_text.grid[0] = 'O';
	clin_process(&call);
}

static void init_client(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	recipe_init();
	assert(clin_init() && svin_init());
	assert(clin_world_payloads_set_enabled(true));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(pub_observe, NULL);
}

/* Let the authentic split admission finish its source budget drain before
 * populating the unviewed destination. All archive access stays on the test's
 * server update thread after the initial streaming worker has finished. */
static struct server_local* publication_fixture(struct server_world** ne_out) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	struct server_world* ne = server_local_world_admit(s, WORLD_DIM_NETHER);
	assert(ne && ne->world_index == 1 && server_local_world_live(ne));
	bool was_paused = s->paused;
	s->paused = false;
	for(unsigned i = 0; i < 500
		&& s->transfer_budget_phase != SERVER_TRANSFER_BUDGET_ENFORCED; i++)
		tick(s);
	s->paused = was_paused;
	assert(s->transfer_budget_phase == SERVER_TRANSFER_BUDGET_ENFORCED);
	setup_server_signs(s, &s->worlds[0], 'O', 1);
	setup_server_signs(s, ne, 'N', PUB_SIGNS);
	*ne_out = ne;
	return s;
}

static bool drive_commit(struct server_local* s, uint32_t id) {
	s->paused = false;
	for(unsigned i = 0; i < 4000; i++) {
		enum server_transfer_state st = server_transfer_state_of(s, id);
		if(st == SERVER_TRANSFER_COMMITTED || st == SERVER_TRANSFER_ROLLED_BACK) {
			s->paused = true;
			return st == SERVER_TRANSFER_COMMITTED;
		}
		tick(s);
		client_acks_marker(s);
	}
	s->paused = true;
	return false;
}

/* Separate argv mode keeps the normal publication suite unchanged. Drive a
 * real dead P2's death-screen press while its view is Nether and P1 remains
 * in the Overworld; the production client readiness loop must finish the
 * Overworld respawn without reading the old Nether view. */
/* Bounded diagnostic: count free real admission entries without retaining
 * any probe ticket. The actual respawn still uses its default FIFO sender. */
static unsigned respawn_free_entries(struct server_local* s) {
	clin_capacity_ticket tickets[CLIENT_CTX_SLOT_COUNT * 2] = {0};
	struct clin_capacity_need need = {0};
	unsigned n = 0;
	for(; n < CLIENT_CTX_SLOT_COUNT * 2; n++)
		if(clin_capacity_reserve_world(0, s->worlds[0].generation,
			&need, &tickets[n]) != CLIN_CAP_OK) break;
	for(unsigned i = 0; i < n; i++) assert(clin_capacity_release(tickets[i]));
	return n;
}

static void test_nether_guest_respawn(struct server_local* s,
	struct server_world* ne, bool vacant_overworld,
	bool hidden_target_mesh, unsigned recovery) {
	struct client_session* guest = &gstate.players[1];
	/* main.c's per-frame local policy, omitted by the owner-swap harness. */
	world_set_server_view_radius(server_local_view_radius(s));
	assert(chunk_mesher_init());
	check(client_session_request_local_gameplay(guest),
		"Nether guest requests body before death");
	for(unsigned i = 0; i < 100 && !guest->local_gameplay_ready; i++) {
		pump_handshake(s);
		tick(s);
	}
	check(guest->local_gameplay_ready && guest->local_player
			&& s->guest_actor && s->guest_actor->finished_loading,
		"Nether guest has admitted client and server body");
	if(!guest->local_gameplay_ready || !guest->local_player
	   || !s->guest_actor) return;
	for(unsigned i = 0; i < 32
		&& server_transfer_actor_frozen(s, s->guest_actor); i++) {
		clin_update_drain(); tick(s);
	}
	guest_sample(s, s->guest_actor->x, s->guest_actor->y,
		s->guest_actor->z, s->guest_actor->on_ground,
		s->guest_actor->correction_seq);
	check(!server_transfer_actor_inert(s, s->guest_actor)
			&& server_local_world_of(s, s->guest_actor) == ne
			&& ne->dimension == WORLD_DIM_NETHER
			&& clin_world_view_slot(1) == 1
			&& server_local_world_of(s, &s->player)
				== (vacant_overworld ? ne : &s->worlds[0])
			&& s->worlds[0].dimension == WORLD_DIM_OVERWORLD,
		vacant_overworld
			? "both P1 and playable P2 occupy Nether; no actor owns Overworld"
			: "P2 is playable in Nether while P1 owns Overworld");
	if(vacant_overworld) {
		/* This fixture's saved OW spawn is outside P1's original radius-four
		 * stream. It is a normal player respawn-plan input, not a chunk, client
		 * context, or readiness injection. Both actors remain in Nether. */
		s->guest_actor->spawn_x = 8 * CHUNK_SIZE;
		s->guest_actor->spawn_z = 0;
		int missing = 0, mesh = 0;
		w_coord_t cx = WCOORD_CHUNK_OFFSET(s->guest_actor->spawn_x);
		w_coord_t cz = WCOORD_CHUNK_OFFSET(s->guest_actor->spawn_z);
		check(!server_world_is_chunk_sent(&s->worlds[0], cx, cz),
			"vacant OW target center was not already published by P1 stream");
		check(clin_world_payload_ready(0),
			"vacant OW retained a real target client context before death");
		if(!clin_world_payload_ready(0)) return;
		for(unsigned i = 0; i < 40; i++) {
			world_near_ring_status(client_payload(0).terrain, cx, cz,
				world_server_view_radius(), &missing, &mesh);
			s->paused = false;
			tick(s);
			clin_update_drain();
		}
		printf("RESPAWN vacant before request OW missing=%d mesh=%d radius=%d\n",
			missing, mesh, world_server_view_radius());
		check(missing > 0
				&& !server_world_is_chunk_sent(&s->worlds[0], cx, cz),
			"vacant Overworld respawn target has missing ring and unsent center before request");
		vacant_target_cx = cx;
		vacant_target_cz = cz;
		watch_vacant_target = true;
	}
	bool accepted = false;
	(void)server_local_damage_actor_result(s, s->guest_actor,
		MAX_PLAYER_HEALTH * 4, 0.0F, 0.0F, &accepted);
	clin_update_drain();
	check(accepted && s->guest_actor->health == 0
			&& guest->local_player->health == 0,
		"authoritative Nether death reaches P2 client body");
	if(!accepted || s->guest_actor->health != 0
	   || guest->local_player->health != 0) return;
	int hold_before = gstate.config_death_hold_respawn_ms;
	gstate.config_death_hold_respawn_ms = 0;
	screen_death_owner_enter(guest);
	struct input_player_frame frame = {.connected = true};
	(void)screen_death_owner_update(guest, 1, &frame, 0.5F,
		0, 240, 640, 240);
	frame.down[IB_GUI_CLICK] = frame.pressed[IB_GUI_CLICK] = true;
	(void)screen_death_owner_update(guest, 1, &frame, 0.02F,
		0, 240, 640, 240);
	gstate.config_death_hold_respawn_ms = hold_before;
	check(guest->death.commit_pending == DEATH_COMMIT_RESPAWN,
		"P2 death screen queues one owned respawn request");
	server_local_test_process_messages(s);
	if(recovery) {
		uint64_t before1 = 0, before2 = 0, after1 = 0, after2 = 0;
		unsigned before_ctx = 0, after_ctx = 0;
		clin_capacity_held(&before1, &before2, &before_ctx);
		unsigned before_free = vacant_overworld ? 0 : respawn_free_entries(s);
		uint32_t request = s->guest_respawn_cap.request_id;
		uint32_t epoch = s->input_epoch_counter;
		check(s->guest_respawn_phase == GUEST_RESPAWN_PREPARED
			&& s->guest_respawn_cap.pending && !s->guest_respawn_cap.ticket
			&& (request & 0xc0000000u) == GUEST_RESPAWN_CAP_ID_BIT
			&& epoch && s->player.finished_loading && !s->input_ack_pending,
			"real death-screen request awaits Overworld respawn reserve before recovery");
		struct server_rpc recover = {.type = SRPC_STREAM_RECOVER,
			.payload.stream_recover = {.epoch = recovery != 2 ? epoch : epoch + 1}};
		assert(svin_rpc_send(&recover));
		/* The default client sender queues actual reserve OK behind recovery. */
		clin_update_drain();
		clin_capacity_held(&after1, &after2, &after_ctx);
		printf("RESPAWN recovery reserve id=%08x epoch=%u held=%u/%u mem1=%llu/%llu mem2=%llu/%llu\n",
			request, epoch, before_ctx, after_ctx,
			(unsigned long long)before1, (unsigned long long)after1,
			(unsigned long long)before2, (unsigned long long)after2);
		check(vacant_overworld
			? after_ctx == before_ctx + 1 && after1 > before1 && after2 > before2
			: respawn_free_entries(s) + 1 == before_free,
			"real client RESERVE allocates one destination ticket (shared or budgeted)");
		server_local_test_process_messages(s);
		clin_update_drain();
		if(recovery != 2) {
			check(!s->player.finished_loading && s->stream_recovery_epoch == epoch,
				"valid public recovery closes host loading before default respawn answer");
			printf("RESPAWN recovery owner pending=%d reserved=%d ticket=%u\n",
				(int)s->guest_respawn_cap.pending, (int)s->guest_respawn_cap.reserved,
				s->guest_respawn_cap.ticket);
			check(!s->guest_respawn_cap.pending && s->guest_respawn_cap.reserved
				&& s->guest_respawn_cap.ticket,
				"default respawn answer transfers real ticket ownership across recovery");
			struct server_rpc unrelated = {.type = SRPC_CAPACITY_REPLY,
				.payload.capacity_reply = {.request_id = 7,
					.result = CLIN_CAP_OK, .ticket = s->guest_respawn_cap.ticket}};
			uint32_t owned_ticket = s->guest_respawn_cap.ticket;
			check(!svin_rpc_send_from(guest_source(s), &unrelated),
				"guest source cannot impersonate a capacity answer");
			assert(svin_rpc_send(&unrelated));
			server_local_test_process_messages(s);
			check(s->guest_respawn_cap.ticket == owned_ticket && !s->player.finished_loading,
				"ordinary capacity namespace cannot alter respawn owner or gameplay barrier");
			check(s->guest_actor->health == 0 && guest->local_player->health == 0
				&& !guest->local_respawn_loading,
				"recovery barrier keeps both dead guest bodies and death UI uncommitted");
			for(unsigned i = 0; i <= SERVER_TRANSFER_CLIENT_WAIT_TICKS; i++) {
				server_local_guest_respawn_preflight_tick(s);
				clin_update_drain();
			}
			clin_capacity_held(&after1, &after2, &after_ctx);
			printf("RESPAWN recovery timeout phase=%d unknown=%08x ticket=%u cleanup=%u held=%u mem1=%llu mem2=%llu\n",
				(int)s->guest_respawn_phase, s->guest_respawn_cap.timed_out_id,
				s->guest_respawn_cap.ticket, s->guest_respawn_cap.cleanup_ticket,
				after_ctx, (unsigned long long)after1, (unsigned long long)after2);
			check(s->guest_respawn_phase == GUEST_RESPAWN_IDLE
				&& !s->guest_respawn_cap.timed_out_id
				&& after_ctx == before_ctx && after1 == before1 && after2 == before2
				&& (vacant_overworld || respawn_free_entries(s) == before_free),
				"bounded production timeout consumes destination RELEASE and clears retry debt");
			check(owned_ticket && !clin_capacity_release(owned_ticket),
				"exact respawn ticket was already retired by production timeout RELEASE");
			check(!s->player.finished_loading && s->guest_actor->health == 0
				&& guest->local_player->health == 0 && !guest->local_respawn_loading,
				"ticket cleanup preserves host recovery and dead guest gameplay barriers");
			/* Separate observed limitation: live recovery in a committed
			 * secondary host world does not re-arm its presentation marker.
			 * This row proves ownership cleanup, not secondary-host recovery. */
			if(vacant_overworld) return;
			for(unsigned i = 0; i < 6500 && !s->input_ack_pending; i++) {
				s->paused = false;
				tick(s);
			}
			check(s->player.finished_loading && s->input_ack_pending,
				"real recovery restream emits a host readiness marker");
			client_acks_marker(s);
			server_local_test_process_messages(s);
			clin_update_drain();
			check(!s->input_ack_pending && !s->stream_recovery_epoch
				&& s->player.finished_loading,
				"real client readiness ACK recovers the host before guest retry");
			unsigned counter = s->guest_respawn_cap.counter;
			frame.down[IB_GUI_CLICK] = frame.pressed[IB_GUI_CLICK] = false;
			(void)screen_death_owner_update(guest, 1, &frame, 0.5F, 0, 240, 640, 240);
			frame.down[IB_GUI_CLICK] = frame.pressed[IB_GUI_CLICK] = true;
			int retry_hold = gstate.config_death_hold_respawn_ms;
			gstate.config_death_hold_respawn_ms = 0;
			(void)screen_death_owner_update(guest, 1, &frame, 0.02F, 0, 240, 640, 240);
			gstate.config_death_hold_respawn_ms = retry_hold;
			server_local_test_process_messages(s);
			clin_update_drain();
			check(s->guest_respawn_cap.counter == counter + 1
				&& s->guest_respawn_phase == GUEST_RESPAWN_PREPARED,
				"actual death-screen retry starts a fresh owned respawn reserve after recovery");
			if(s->guest_respawn_phase != GUEST_RESPAWN_PREPARED) return;
		} else {
			check(s->player.finished_loading && !s->stream_recovery_epoch,
				"invalid recovery epoch leaves healthy host and respawn admission unchanged");
		}
	} else clin_update_drain();
	/* With no committed OW actor, the destination requires the production
	 * reserve -> landing-arm -> READY handshake before the death is committed.
	 * Keep the same owned request pending while its messages round-trip. */
	for(unsigned i = 0; i < 80
		&& (s->guest_respawn_phase != GUEST_RESPAWN_LOADING
			|| !guest->local_respawn_loading); i++) {
		pump_handshake(s);
		s->paused = false;
		tick(s);
	}
	if(vacant_overworld)
		printf("RESPAWN vacant admission phase=%d pending=%d reserved=%d "
			"armed=%d ready=%d reply=%d\n",
			(int)s->guest_respawn_phase,
			(int)s->guest_respawn_cap.pending,
			(int)s->guest_respawn_cap.reserved,
			(int)s->guest_respawn_cap.landing_armed,
			(int)s->guest_respawn_cap.landing_ready,
			(int)guest->respawn_failure);
	check(s->guest_respawn_phase == GUEST_RESPAWN_LOADING
			&& guest->local_respawn_loading
			&& guest->local_respawn_epoch == s->guest_respawn_epoch,
		"server and client enter same Nether-to-Overworld respawn epoch");
	if(s->guest_respawn_phase != GUEST_RESPAWN_LOADING
	   || !guest->local_respawn_loading) return;
	struct entity* dead_body = guest->local_player;
	uint64_t respawn_epoch = guest->local_respawn_epoch;
	check(guest->local_respawn_world_slot == 0
			&& guest->local_respawn_world_generation == s->worlds[0].generation
			&& guest->local_respawn_dimension == WORLD_DIM_OVERWORLD,
		"LOADING names the live Overworld target instead of dead Nether view");
	/* This headless fixture has no rendering frame. Run the real mesher on
	 * the Overworld destination so readiness observes completed chunks,
	 * without injecting a ready ACK or clearing dirty flags by hand. */
	int target_radius = world_server_view_radius();
	w_coord_t target_cx = WCOORD_CHUNK_OFFSET(
		(w_coord_t)floor(guest->local_respawn_x));
	w_coord_t target_cz = WCOORD_CHUNK_OFFSET(
		(w_coord_t)floor(guest->local_respawn_z));
	int target_missing = 0, target_mesh = 0;
	ptime_t mesh_started = time_get();
	bool target_frame_prepared = false;
	size_t lowest_remaining = CHUNK_MESHER_QLENGTH;
	unsigned target_frame_steps = 0;
	if(vacant_overworld)
		world_pre_render_clear(client_payload(0).terrain);
	for(unsigned i = 0; i < 10000; i++) {
		world_near_ring_status(client_payload(0).terrain, target_cx,
			target_cz, target_radius, &target_missing, &target_mesh);
		if(target_missing == 0 && target_mesh == 0) break;
		if(vacant_overworld) {
			/* Slot 0 is streamed by the primary server update, not by A2W's
			 * slot >= 1 stream_step. Match the normal running server frame. */
			s->paused = false;
			tick(s);
		} else
			server_transfer_stream_step(s);
		clin_update_drain();
		if(vacant_overworld) {
			/* Mirror main's actual frame policy. The destination has no drawn
			 * view; the LOADING helper must still route its hidden camera through
			 * LOD and the shared build-token budget. */
			struct clin_frame_worlds fw;
			clin_frame_worlds_begin(&fw, 2);
			struct camera hidden;
			if(hidden_target_mesh)
				target_frame_prepared = clin_frame_prepare_respawn(&fw,
					guest, &hidden);
			struct camera* cameras[2] = {
				&gstate.players[0].camera, &guest->camera};
			clin_frame_lod(&fw, cameras, 2);
			w_coord_t centers[2][2];
			for(unsigned bi = 0; bi < fw.n_live; bi++) {
				unsigned cv = fw.view_terrain[1] == fw.live[bi] ? 1 : 0;
				centers[bi][0] = WCOORD_CHUNK_OFFSET(
					(w_coord_t)floorf(cameras[cv]->x));
				centers[bi][1] = WCOORD_CHUNK_OFFSET(
					(w_coord_t)floorf(cameras[cv]->z));
			}
			size_t remaining = clin_frame_build(&fw,
				CHUNK_MESHER_QLENGTH, centers, target_radius, NULL, NULL);
			if(remaining < lowest_remaining) lowest_remaining = remaining;
			target_frame_steps++;
			if(target_frame_steps % 1000 == 0)
				printf("RESPAWN vacant progress frames=%u missing=%d mesh=%d\n",
					target_frame_steps, target_missing, target_mesh);
		} else
			(void)world_build_chunks(client_payload(0).terrain,
				CHUNK_MESHER_QLENGTH, false);
		chunk_mesher_receive();
		if(time_diff_ms(mesh_started, time_get())
			>= (vacant_overworld ? 120000 : 15000)) break;
		thread_msleep(1);
	}
	if(vacant_overworld) {
		unsigned chunks = 0, lod[3] = {0}, dirty = 0, flight = 0;
		unsigned first = 0, wait = 0, retry = 0;
		for(int dz = -target_radius; dz <= target_radius; dz++)
			for(int dx = -target_radius; dx <= target_radius; dx++)
				for(int sy = 0; sy < COLUMN_HEIGHT; sy++) {
					struct chunk* c = world_find_chunk(
						client_payload(0).terrain,
						(target_cx + dx) * CHUNK_SIZE,
						sy * CHUNK_SIZE,
						(target_cz + dz) * CHUNK_SIZE);
					if(!c) continue;
					chunks++;
					if(c->lod < 3) lod[c->lod]++;
					dirty += c->rebuild_displist;
					flight += c->mesh_in_flight;
					first += c->first_meshed;
					wait += c->neighbour_wait != 0;
					retry += c->mesh_retry_skip_checks != 0;
				}
		printf("RESPAWN vacant mesh audit frames=%u missing=%d mesh=%d "
			"min_remaining=%zu "
			"chunks=%u lod=%u/%u/%u dirty=%u flight=%u "
			"first=%u wait=%u retry=%u hidden=(%.1f,%.1f)\n",
			target_frame_steps, target_missing, target_mesh,
			lowest_remaining, chunks,
			lod[0], lod[1], lod[2], dirty, flight, first, wait, retry,
			(double)guest->local_respawn_x,
			(double)guest->local_respawn_z);
	}
	check(target_missing == 0 && target_mesh == 0,
		"fixture: Overworld respawn target has a completed near ring");
	if(vacant_overworld && hidden_target_mesh)
		check(target_frame_prepared,
			"real frame helper accepts exact vacant respawn target handle");
	if(vacant_overworld)
		check(vacant_target_chunks > 0,
			"vacant Overworld streams target center CHUNK after respawn request");
	struct entity** source_body = dict_entity_get(
		client_payload(1).entities, dead_body->id);
	struct entity** target_body = dict_entity_get(
		client_payload(0).entities, dead_body->id);
	assert(source_body && *source_body == dead_body && !target_body);
	struct world* dead_world = dead_body->world;
	check(!clin_guest_respawn_handoff(0,
		guest->local_respawn_world_generation + 1,
		WORLD_DIM_OVERWORLD)
			&& clin_world_view_slot(1) == 1
			&& !guest->local_respawn_ack_send_pending
			&& dead_body->world == dead_world,
		"wrong target generation refuses handoff without moving body or ACK");
	struct entity collision = {.id = dead_body->id};
	struct entity** collision_slot = dict_entity_safe_get(
		client_payload(0).entities, dead_body->id);
	assert(collision_slot && !*collision_slot);
	*collision_slot = &collision;
	check(!clin_guest_respawn_handoff(0,
		guest->local_respawn_world_generation, WORLD_DIM_OVERWORLD)
			&& clin_world_view_slot(1) == 1
			&& !guest->local_respawn_ack_send_pending
			&& dead_body->world == dead_world
			&& *dict_entity_get(client_payload(1).entities,
				dead_body->id) == dead_body
			&& *dict_entity_get(client_payload(0).entities,
				dead_body->id) == &collision,
		"destination ID collision preserves source owner and refuses ACK");
	dict_entity_erase(client_payload(0).entities, dead_body->id);
	dead_body->world = client_payload(0).terrain;
	check(!clin_guest_respawn_handoff(0,
		guest->local_respawn_world_generation, WORLD_DIM_OVERWORLD)
			&& clin_world_view_slot(1) == 1
			&& !guest->local_respawn_ack_send_pending
			&& *dict_entity_get(client_payload(1).entities,
				dead_body->id) == dead_body
			&& !dict_entity_get(client_payload(0).entities,
				dead_body->id),
		"mismatched source body world refuses handoff without store mutation");
	dead_body->world = dead_world;
	/* The exact main.c guest readiness branch: use the LOADING target,
	 * atomically rebind the inert body before flushing the owned epoch ACK. */
	bool moved_before_ack = false;
	bool idempotent_before_ack = false;
	for(unsigned i = 0; i < 200
		&& (s->guest_actor->health == 0 || !guest->local_gameplay_ready); i++) {
		unsigned slot = guest->local_respawn_world_slot;
		const struct client_world_context* target_ctx
			= slot < CLIENT_CTX_SLOT_COUNT
			? client_ctx_registry_current(clin_world_contexts(), slot) : NULL;
		if(target_ctx && target_ctx->server_generation
				== guest->local_respawn_world_generation
		   && target_ctx->dimension == guest->local_respawn_dimension
		   && clin_world_payload_ready(slot)) {
			struct clin_view_world view;
			if(clin_world_slot_enter(slot, &view)) {
				if(client_session_local_respawn_readiness(guest,
					view.terrain, true, gstate.world_load_failed,
					gstate.remote_terrain_hold)
				   && !clin_guest_respawn_handoff(slot,
					guest->local_respawn_world_generation,
					guest->local_respawn_dimension))
					guest->local_respawn_ack_send_pending = false;
				clin_world_view_leave(&view);
			}
		}
		if(guest->local_respawn_ack_send_pending
		   && guest->local_player == dead_body
		   && clin_world_view_slot(1) == 0) {
			struct entity** ow_body = dict_entity_get(
				client_payload(0).entities, dead_body->id);
			struct entity** ne_body = dict_entity_get(
				client_payload(1).entities, dead_body->id);
			moved_before_ack = ow_body && *ow_body == dead_body
				&& (!ne_body || *ne_body != dead_body);
			if(moved_before_ack && !idempotent_before_ack)
				idempotent_before_ack = clin_guest_respawn_handoff(slot,
					guest->local_respawn_world_generation,
					guest->local_respawn_dimension)
					&& *dict_entity_get(client_payload(0).entities,
						dead_body->id) == dead_body
					&& !dict_entity_get(client_payload(1).entities,
						dead_body->id)
					&& clin_world_view_slot(1) == 0;
		}
		client_session_flush_local(guest);
		s->paused = false;
		tick(s);
	}
	frame.down[IB_GUI_CLICK] = frame.pressed[IB_GUI_CLICK] = false;
	(void)screen_death_owner_update(guest, 1, &frame, 0.02F,
		0, 240, 640, 240);
	int radius = world_server_view_radius();
	if(radius <= 0) radius = MAX_VIEW_DISTANCE;
	w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(guest->local_respawn_x));
	w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(guest->local_respawn_z));
	int ow_missing = 0, ow_mesh = 0, ne_missing = 0, ne_mesh = 0;
	world_near_ring_status(client_payload(0).terrain, cx, cz,
		radius, &ow_missing, &ow_mesh);
	world_near_ring_status(client_payload(1).terrain, cx, cz,
		radius, &ne_missing, &ne_mesh);
	printf("RESPAWN target radius=%d OW missing=%d mesh=%d Nether missing=%d mesh=%d "
		"phase=%d server_health=%d client_health=%d view=%d ack=%d\n",
		radius, ow_missing, ow_mesh, ne_missing, ne_mesh,
		(int)s->guest_respawn_phase, (int)s->guest_actor->health,
		guest->local_player ? (int)guest->local_player->health : -1,
		clin_world_view_slot(1), (int)guest->local_respawn_ack_send_pending);
	check(moved_before_ack,
		"same dead P2 body has sole Overworld store/view ownership before ACK");
	check(idempotent_before_ack,
		"same-world repeat handoff is idempotent before respawn ACK");
	check(s->guest_actor->health == MAX_PLAYER_HEALTH
			&& s->guest_actor->dimension == WORLD_DIM_OVERWORLD
			&& server_local_world_of(s, s->guest_actor) == &s->worlds[0]
			&& guest->local_player && guest->local_player->health > 0
			&& guest->local_gameplay_ready
			&& !guest->local_respawn_loading
			&& guest->local_respawn_completed_epoch == respawn_epoch
			&& guest->local_respawn_correction_installed
			&& clin_world_view_slot(1) == 0
			&& guest->death.commit_pending != DEATH_COMMIT_RESPAWN,
		"P2 respawns with Overworld body/view and death UI settled");
}

/* Actual primary-slot terrain failure after a real host A2T commit.
 * The one-shot chunk fault is the existing portal/B2 consumer test seam;
 * the recovery request, RESET, updates and readiness ACK remain production. */
static void secondary_terrain_fault(struct server_world* ne, w_coord_t column) {
	/* An ordinary streamed column carries the live server snapshot and stamp.
	 * Choose a fresh outer column so the first client section allocation occurs. */
	struct server_chunk* sc = NULL;
	server_world_begin_update(ne);
	assert(server_world_load_chunk(ne, column, 0, &sc) && sc);
	struct chunk_rpc_slot* slot = clin_chunk_slot_try_acquire();
	assert(slot); clin_chunk_slot_copy_local(slot, sc);
	memset(slot->grass_i, 0, 256); memset(slot->grass_j, 0, 256);
	slot->piston_moving.count = 0;
	struct client_rpc failed = {.type = CRPC_CHUNK,
		.payload.chunk = {.x = column * CHUNK_SIZE, .z = 0,
			.sx = CHUNK_SIZE, .sy = WORLD_HEIGHT, .sz = CHUNK_SIZE, .slot = slot}};
	clin_rpc_stamp(&failed, ne->world_index, ne->generation);
	chunk_test_fail_alloc_after(1);
	clin_rpc_send(&failed); clin_update_drain();
	chunk_test_fail_alloc_after(0);
}

static void test_secondary_host_recovery(struct server_local* s,
	struct server_world* ne, unsigned mode) {
	bool vacant = mode != 2;
	bool late_fault = mode == 3, center_failure = mode == 4;
	/* drive_commit ends at authoritative COMMIT. A playable frame follows
	 * the actual own-world marker/ACK, so settle that public handshake first. */
	for(unsigned i = 0; i < 2000 && (s->input_ack_pending
		|| server_transfer_actor_inert(s, &s->player)
		|| !gstate.world_loaded || gstate.world_switch_pending); i++) {
		s->paused = false; tick(s); client_acks_marker(s);
		server_local_test_process_messages(s); clin_update_drain();
	}
	s->paused = true;
	printf("SECONDARY setup epoch=%u finished=%d pending=%u inert=%d loaded=%d switching=%d\n",
		s->input_epoch_counter, (int)s->player.finished_loading,
		s->input_ack_pending, (int)server_transfer_actor_inert(s, &s->player),
		(int)gstate.world_loaded, (int)gstate.world_switch_pending);
	struct entity* host_body = gstate.players[0].local_player;
	struct entity* guest_body = gstate.players[1].local_player;
	struct window_container* inventory = gstate.players[0].windows[WINDOWC_INVENTORY];
	uint32_t epoch = s->input_epoch_counter;
	const struct client_world_context* initial_host_ctx
		= client_ctx_registry_current(clin_world_contexts(), 1);
	assert(initial_host_ctx);
	uint32_t context_epoch = initial_host_ctx->generation;
	uint64_t guest_generation = gstate.players[1].local_generation;
	const struct client_world_context* peer_ctx
		= client_ctx_registry_current(clin_world_contexts(), 0);
	uint32_t peer_epoch = peer_ctx ? peer_ctx->generation : 0;
	check(host_body && guest_body && gstate.players[1].local_gameplay_ready
		&& !server_transfer_actor_inert(s, s->guest_actor),
		"both admitted bodies and playable guest transfer are settled before recovery fault");
	if(!host_body || !guest_body || !gstate.players[1].local_gameplay_ready
	   || server_transfer_actor_inert(s, s->guest_actor)) return;
	w_coord_t guest_cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_actor->x));
	w_coord_t guest_cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_actor->z));
	check(s->guest_actor->finished_loading
		&& guest_body->world == client_payload(vacant ? 1 : 0).terrain
		&& world_find_chunk(guest_body->world, guest_cx * CHUNK_SIZE, 0, guest_cz * CHUNK_SIZE),
		"playable guest has its own admitted body and collision terrain before the recovery fault");
	w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->player.x));
	w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->player.z));
	bool ow_sent = server_world_is_chunk_sent(&s->worlds[0], 0, 0);
	uint32_t ne_generation = ne->generation, ow_generation = s->worlds[0].generation;
	server_world_mark_far_sent(ne, 60, 60);
	server_world_mark_far_sent(&s->worlds[0], 80, 80);
	check(server_local_world_of(s, &s->player) == ne
		&& clin_world_payload_primary_slot() == 1 && clin_world_view_slot(0) == 1
		&& s->player.finished_loading && !s->input_ack_pending
		&& !server_transfer_actor_inert(s, &s->player)
		&& gstate.world_loaded && !gstate.world_switch_pending && epoch,
		"real committed host is playable in primary Nether before terrain fault");
	check(server_world_is_chunk_sent(ne, cx, cz)
		&& world_find_chunk(client_payload(1).terrain, cx * CHUNK_SIZE, 0, cz * CHUNK_SIZE),
		"actual host-world collision center was installed before fault");
	secondary_terrain_fault(ne, 32);
	check(!gstate.world_loaded && gstate.world_switch_pending
		&& gstate.players[0].current_screen == &screen_load_world,
		"stamped primary CHUNK allocation failure authors actual live recovery request");
	secondary_watch = true;
	server_local_test_process_messages(s);
	check(!s->player.finished_loading && s->stream_recovery_epoch == epoch
		&& !s->input_ack_pending,
		"actual client recovery request is accepted at its current host epoch");
	check(!server_world_is_chunk_sent(ne, cx, cz),
		"accepted recovery invalidates the actual host-world center sent claim");
	check(server_world_is_chunk_sent(&s->worlds[0], 0, 0) == ow_sent,
		"accepted recovery preserves unrelated Overworld sent ownership");
	struct server_chunk* center = dict_server_chunks_get(ne->chunks, S_CHUNK_ID(cx, cz));
	check(center && !center->a2w_pub_stage && !center->a2w_sign_cursor
		&& !center->a2w_entity_after_id && !server_world_is_far_sent(ne, 60, 60)
		&& server_world_is_far_sent(&s->worlds[0], 80, 80)
		&& ne->generation == ne_generation && s->worlds[0].generation == ow_generation,
		"recovery resets host publication/far claims and preserves other world generation/far ownership");
	clin_update_drain();
	check(secondary_resets == 1 && !world_loaded_chunks(client_payload(1).terrain),
		"actual recovery RESET clears primary Nether terrain before replacement chunks");
	check(gstate.players[0].local_player == host_body
		&& gstate.players[1].local_player == guest_body
		&& gstate.players[0].windows[WINDOWC_INVENTORY] == inventory
		&& clin_world_view_slot(0) == 1
		&& clin_world_view_slot(1) == (vacant ? 1 : 0),
		"voxel reset retains both body identities, host inventory and view ownership");
	if(center_failure) {
		/* Remove the actual saved server center through its owner API, then
		 * exercise the existing load/generation refusal seam while recovery
		 * must keep collision input closed. No sent flag or barrier injection. */
		assert(server_world_save_chunk(ne, true, cx, cz));
		clin_update_drain();
		server_world_test_fail_gen_attempts(100000);
	}
	bool injected_late = false;
	unsigned steps = 0;
	for(; steps < 20000 && !s->load_failure_terminal; steps++) {
		tick(s);
		if(late_fault && !injected_late && secondary_markers && s->input_ack_pending) {
			secondary_terrain_fault(ne, 33);
			injected_late = true;
		}
		client_acks_marker(s);
		server_local_test_process_messages(s);
		clin_update_drain();
		if(s->player.finished_loading && !s->input_ack_pending
			&& !s->stream_recovery_epoch && gstate.world_loaded
			&& !gstate.world_switch_pending) break;
	}
	printf("SECONDARY recovery vacant=%d steps=%u hostsent=%d owsent=%d "
		"reset=%u markers=%u hostchunks=%u owchunks=%u terminal=%u "
		"serverterminal=%d retry=%u finished=%d pending=%u recovery=%u "
		"hostsections=%zu loaded=%d switching=%d failed=%d\n",
		(int)vacant, steps, (int)server_world_is_chunk_sent(ne, cx, cz),
		(int)server_world_is_chunk_sent(&s->worlds[0], 0, 0),
		secondary_resets, secondary_markers, secondary_host_chunks, secondary_ow_chunks,
		secondary_terminal, (int)s->load_failure_terminal, s->input_handoff_retry_count,
		(int)s->player.finished_loading, s->input_ack_pending, s->stream_recovery_epoch,
		world_loaded_chunks(client_payload(1).terrain), (int)gstate.world_loaded,
		(int)gstate.world_switch_pending, (int)gstate.world_load_failed);
	if(center_failure) {
		server_world_test_fail_gen_attempts(0);
		check(s->load_failure_terminal && secondary_terminal == 1
			&& !secondary_markers && !s->player.finished_loading
			&& !gstate.world_loaded && gstate.world_switch_pending,
			"missing actual host center reaches bounded terminal failure with no late marker");
		unsigned markers_before = secondary_markers;
		check(!server_local_host_world_marker(s),
			"terminal marker helper refuses a new epoch after the recovery episode is cleared");
		clin_update_drain();
		check(secondary_markers == markers_before,
			"direct terminal marker refusal publishes no client marker");
		struct server_rpc stale = {.type = SRPC_INPUT_READY,
			.payload.input_ready = {.epoch = epoch, .retry = false}};
		assert(svin_rpc_send(&stale)); server_local_test_process_messages(s);
		for(unsigned i = 0; i < 8; i++) tick(s);
		check(s->load_failure_terminal && !s->player.finished_loading
			&& secondary_markers == markers_before && gstate.world_switch_pending,
			"stale readiness ACK and later updates cannot escape terminal loading");
		secondary_watch = false;
		return;
	}
	if(late_fault)
		check(injected_late && secondary_resets == 2 && secondary_markers == 2
			&& s->input_handoff_retry_count == 2,
			"second real CHUNK failure after marker authors NACK then own-world RESET/marker retry");
	check(secondary_host_chunks && secondary_markers && !secondary_terminal
		&& !s->load_failure_terminal && s->player.finished_loading
		&& !s->input_ack_pending && !s->stream_recovery_epoch
		&& gstate.world_loaded && !gstate.world_switch_pending
		&& world_find_chunk(client_payload(1).terrain, cx * CHUNK_SIZE, 0, cz * CHUNK_SIZE),
		"actual host-world restream and exact readiness ACK recover after terrain failure");
	check(gstate.players[0].local_player == host_body
		&& gstate.players[1].local_player == guest_body
		&& gstate.players[0].windows[WINDOWC_INVENTORY] == inventory
		&& clin_world_view_slot(0) == 1
		&& clin_world_view_slot(1) == (vacant ? 1 : 0),
		"completed recovery retains exact body/inventory/view owners");
	struct clin_frame_worlds fw;
	clin_frame_worlds_begin(&fw, 2);
	const struct client_world_context* host_ctx
		= client_ctx_registry_current(clin_world_contexts(), 1);
	w_coord_t gcx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_actor->x));
	w_coord_t gcz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_actor->z));
	check(host_ctx && host_ctx->generation > context_epoch
		&& host_ctx->server_generation == ne_generation
		&& (host_ctx->bound_views & (vacant ? 3u : 1u)) == (vacant ? 3u : 1u)
		&& client_ctx_registry_current(clin_world_contexts(), 0) == peer_ctx
		&& (!peer_ctx || peer_ctx->generation == peer_epoch)
		&& gstate.players[1].local_generation == guest_generation
		&& (vacant ? fw.shared
			&& fw.view_terrain[0] == fw.view_terrain[1]
			&& fw.view_terrain[1] == client_payload(1).terrain
			: !fw.shared && fw.n_live == 2
				&& fw.view_terrain[1] == client_payload(0).terrain)
		&& gstate.players[1].local_gameplay_ready && s->guest_actor->finished_loading
		&& guest_body->world == client_payload(vacant ? 1 : 0).terrain
		&& world_find_chunk(guest_body->world, gcx * CHUNK_SIZE, 0, gcz * CHUNK_SIZE),
		"shared successor terrain/readiness is restored for both actors; separate guest world remains isolated");
	double move_x = s->player.x + 0.03125;
	host_sample(s, move_x, s->player.y, s->player.z, s->player.on_ground,
		s->player.correction_seq);
	check(s->player.x == move_x,
		"public host movement resumes only after actual recovery readiness ACK");
	secondary_watch = false;
}

int main(int argc, char** argv) {
	unsigned secondary = argc == 2 && strcmp(argv[1], "secondary-recovery-vacant") == 0 ? 1
		: argc == 2 && strcmp(argv[1], "secondary-recovery-peer") == 0 ? 2
		: argc == 2 && strcmp(argv[1], "secondary-recovery-late") == 0 ? 3
		: argc == 2 && strcmp(argv[1], "secondary-recovery-center") == 0 ? 4 : 0;
	bool fireball_mode = argc == 2 && strcmp(argv[1], "fireball") == 0;
	bool projectiles_mode = argc == 2 && strcmp(argv[1], "projectiles") == 0;
	bool respawn_mode = argc == 2 && strcmp(argv[1], "respawn") == 0;
	bool respawn_vacant_mode
		= argc == 2 && strcmp(argv[1], "respawn-vacant") == 0;
	bool respawn_vacant_no_hidden
		= argc == 2 && strcmp(argv[1], "respawn-vacant-no-hidden") == 0;
	unsigned recovery = argc == 2 && strcmp(argv[1], "respawn-recovery") == 0 ? 1
		: argc == 2 && strcmp(argv[1], "respawn-recovery-invalid") == 0 ? 2
		: argc == 2 && strcmp(argv[1], "respawn-recovery-host-ow") == 0 ? 3 : 0;
	bool vacant_overworld = respawn_vacant_mode || respawn_vacant_no_hidden || recovery == 1 || recovery == 2;
	assert(argc == 1 || fireball_mode || projectiles_mode || respawn_mode
		   || vacant_overworld || recovery || secondary);
	init_client();
	struct server_world* ne = NULL;
	struct server_local* s = publication_fixture(&ne);
	pub_gen = ne->generation;
	ow_gen = s->worlds[0].generation;
	seed_overworld_client_sign(s);
	check(client_sign(0, PUB_X, PUB_Z, 'O'),
		  "fixture: Overworld sign at identical coordinate is visible");
	/* Transfer drives real world ticks before the center is published. Keep
	 * the two publication witnesses resident in (0,0) with ordinary collision
	 * terrain; otherwise their real wander can leave the asserted column. */
	for(int x = 4; x <= 9; x++)
		for(int z = 4; z <= 8; z++)
			for(int y = 3; y <= 8; y++) {
				struct block_data wanted = {.type = y == 3 || x == 4 || x == 9
					|| z == 4 || z == 8 ? BLOCK_STONE : BLOCK_AIR};
				(void)server_world_set_block(ne, x, y, z, wanted);
				struct block_data actual;
				assert(server_world_get_block(ne, x, y, z, &actual)
					&& actual.type == wanted.type);
			}
	for(unsigned k = 0; k < 2; k++) {
		struct entity* e = server_local_spawn_monster_at_w(ne,
			(vec3){5.5F + 2.0F * (float)k, 6.1F, 5.5F}, MONSTER_PIG_ZOMBIE);
		assert(e);
		monster_ids[k] = e->id;
	}
	uint32_t id = 0;
	if(secondary) {
		if(secondary != 2) {
			check(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &id)
				== SERVER_TRANSFER_OK && drive_commit(s, id),
				"real P2 transfer joins Nether before host recovery with vacant OW");
			check(client_session_request_local_gameplay(&gstate.players[1]),
				"committed Nether guest requests actual destination body admission");
			for(unsigned i = 0; i < 100 && !gstate.players[1].local_gameplay_ready; i++) {
				pump_handshake(s); tick(s);
			}
			for(unsigned i = 0; i < 300 && server_transfer_actor_frozen(s, s->guest_actor); i++) {
				server_transfer_stream_step(s); clin_update_drain();
			}
			guest_sample(s, s->guest_actor->x, s->guest_actor->y,
				s->guest_actor->z, s->guest_actor->on_ground, s->guest_actor->correction_seq);
			check(gstate.players[1].local_gameplay_ready && gstate.players[1].local_player
				&& !server_transfer_actor_inert(s, s->guest_actor),
				"actual guest body/HOLD_END/accepted pose settle before host joins shared terrain");
		}
		check(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id)
			== SERVER_TRANSFER_OK && drive_commit(s, id),
			"real host A2T transfer commits primary Nether before recovery");
		test_secondary_host_recovery(s, ne, secondary);
		clin_test_set_consume_observer(NULL, NULL); teardown(s);
		printf("%s secondary host recovery (%u failures)\n", failures ? "FAIL" : "PASS", failures);
		return failures ? 1 : 0;
	}
	check(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &id)
			  == SERVER_TRANSFER_OK, "guest transfer request accepted");
	check(drive_commit(s, id), "real guest transfer reaches Nether COMMIT");
	struct te_store postcommit_signs = server_world_tile_store(ne);
	printf("PUB after COMMIT sign_rows=%d\n",
		te_store_count_owned(&postcommit_signs, TE_KIND_SIGN));
	check(clin_world_view_slot(1) == 1
			  && server_local_world_of(s, &s->player) == &s->worlds[0],
		  "Nether is the guest's bound secondary view while host remains in slot0");
	for(unsigned i = 0; i < 300 && !server_world_is_chunk_sent(ne, 0, 0); i++) {
		server_transfer_stream_step(s);
		clin_update_drain();
	}
	check(server_world_is_chunk_sent(ne, 0, 0),
		  "secondary center column completes its ordinary publication");
	if(respawn_mode || vacant_overworld || recovery) {
		if(vacant_overworld) {
			uint32_t host_id = 0;
			check(server_transfer_request(s, &s->player,
				WORLD_DIM_NETHER, &host_id) == SERVER_TRANSFER_OK,
				"P1 real transfer to P2 Nether world accepted");
			check(drive_commit(s, host_id),
				"P1 real transfer to Nether COMMIT leaves OW vacant");
			check(server_local_world_of(s, &s->player) == ne
					&& server_local_world_of(s, s->guest_actor) == ne
					&& clin_world_view_slot(0) == 1,
				"both authoritative actors and P1 client view occupy Nether");
		}
		test_nether_guest_respawn(s, ne, vacant_overworld,
			!respawn_vacant_no_hidden, recovery);
		chunk_mesher_shutdown();
		clin_test_set_consume_observer(NULL, NULL);
		teardown(s);
		printf("%s Nether guest respawn%s (%u failures)\n",
			failures ? "FAIL" : "PASS",
			vacant_overworld ? " with vacant target" : "", failures);
		return failures ? 1 : 0;
	}
	struct server_chunk* center
		= dict_server_chunks_get(ne->chunks, S_CHUNK_ID(0, 0));
	printf("PUB initial chunk=%u signs=%u monsters=%u stage=%u sign_cursor=%u "
		   "entity_after=%u client_sign=%u client_e0=%u client_e1=%u\n",
		   pub_chunk, pub_signs, pub_monsters,
		   center ? center->a2w_pub_stage : 0,
		   center ? center->a2w_sign_cursor : 0,
		   center ? center->a2w_entity_after_id : 0,
		   client_sign(1, PUB_X, PUB_Z, 'N'),
		   client_monster(1, monster_ids[0]),
		   client_monster(1, monster_ids[1]));
	check(pub_chunk >= 1, "secondary center CHUNK reached client");
	check(!publication_out_of_order,
		  "initial secondary signs and entities follow their center CHUNK");
	check(pub_signs == PUB_SIGNS,
		  "initial secondary publication delivers every sign after CHUNK");
	if(!fireball_mode && !projectiles_mode)
		check(pub_monsters == 2,
			  "initial secondary publication delivers each resident monster once");
	check(!wrong_stamp, "all secondary payloads carry exact world generation");
	bool all_signs = true;
	for(unsigned k = 0; k < PUB_SIGNS; k++)
		all_signs &= client_sign(1, PUB_X + (int)(k % 6),
			PUB_Z + (int)(k / 6), 'N');
	if(!fireball_mode && !projectiles_mode)
		check(all_signs && client_monster(1, monster_ids[0])
				  && client_monster(1, monster_ids[1]),
			  "client slot1 owns all sign grids and both resident entities");
	check(client_sign(0, PUB_X, PUB_Z, 'O')
		  && !client_monster(0, monster_ids[0]),
		  "same-coordinate Overworld store remains isolated");
	if(projectiles_mode) {
		vec3 shot = {5.5F, 100.0F, 5.5F};
		vec3 velocity = {0.1F, 0.0F, 0.0F};
		projectile_spawns = 0;
		projectile_watch_item = ITEM_ARROW;
		uint32_t arrow_before = s->next_entity_id;
		server_local_spawn_arrow_w(ne, shot, velocity, 0, ITEM_ARROW,
			ARROW_DAMAGE, true);
		s->paused = false; tick(s); s->paused = true;
		clin_update_drain();
		uint32_t arrow_id = projectile_spawn.payload.spawn_arrow.entity_id;
		struct entity** arrow = dict_entity_get(s->entities, arrow_id);
		check(projectile_spawns == 1 && arrow_id == arrow_before + 1
			  && projectile_spawn.world_index == 1
			  && projectile_spawn.world_generation == pub_gen
			  && projectile_spawn.payload.spawn_arrow.item_id == ITEM_ARROW
			  && arrow && *arrow && server_local_entity_world(s, *arrow) == ne
			  && client_entity(1, arrow_id, ENTITY_ARROW)
			  && !client_entity(0, arrow_id, ENTITY_ARROW),
			"queued arrow publishes once in its live Nether world");
		projectile_spawns = 0;
		server_local_spawn_arrow_w(ne, shot, velocity, 0, ITEM_ARROW,
			ARROW_DAMAGE, true);
		uint32_t before_stale_id = s->next_entity_id;
		check(s->pending_arrow_count == 1,
			"fixture: stale Nether arrow is queued before world generation changes");
		if(s->pending_arrow_count)
			s->pending_arrows[0].world_h.generation = pub_gen - 1;
		s->paused = false; tick(s); s->paused = true;
		clin_update_drain();
		check(s->next_entity_id == before_stale_id,
			"stale-generation queued arrow never allocates or publishes");
		projectile_spawns = 0;
		server_local_spawn_arrow_w(&s->worlds[0], shot, velocity, 0,
			ITEM_ARROW, ARROW_DAMAGE, true);
		s->paused = false; tick(s); s->paused = true;
		clin_update_drain();
		check(projectile_spawns == 1 && projectile_spawn.world_index == 0
			  && projectile_spawn.world_generation == ow_gen
			  && client_entity(0, projectile_spawn.payload.spawn_arrow.entity_id,
				  ENTITY_ARROW)
			  && !client_entity(1, projectile_spawn.payload.spawn_arrow.entity_id,
				  ENTITY_ARROW),
			"queued Overworld arrow remains isolated in slot0");
		for(unsigned i = 0; i < 300
			&& server_transfer_actor_frozen(s, s->guest_actor); i++) {
			server_transfer_stream_step(s);
			clin_update_drain();
		}
		check(!server_transfer_actor_frozen(s, s->guest_actor),
			"guest destination HOLD_END clears before projectile uses");
		guest_sample(s, s->guest_actor->x, s->guest_actor->y,
			s->guest_actor->z, s->guest_actor->on_ground,
			s->guest_actor->correction_seq);
		check(!server_transfer_actor_inert(s, s->guest_actor),
			"accepted guest pose releases transfer presentation hold");
		size_t guest_hotbar = INVENTORY_SLOT_HOTBAR
			+ inventory_get_hotbar(&s->guest_actor->inventory);
		struct server_rpc use = {.type = SRPC_USE_ITEM,
			.payload.use_item = {.charge = 1.0F}};
		const uint16_t thrown[] = {ITEM_EGG, ITEM_SNOW_BALL,
			ITEM_FISHING_ROD};
		for(unsigned k = 0; k < 3; k++) {
			s->guest_actor->inventory.items[guest_hotbar]
				= (struct item_data){.id = thrown[k], .count = 2};
			projectile_spawns = 0;
			projectile_watch_item = thrown[k];
			uint32_t use_before = s->next_entity_id;
			check(guest_send(s, &use),
				"guest projectile use follows public owner route");
			check(projectile_spawns == 1
				  && projectile_spawn.payload.spawn_arrow.entity_id
					  == use_before + 1,
				"guest use publishes its new projectile before streamer replay");
			uint32_t id = projectile_spawn.payload.spawn_arrow.entity_id;
			struct entity** server = dict_entity_get(s->entities, id);
			check(projectile_spawns == 1 && id == use_before + 1
				  && projectile_spawn.world_index == 1
				  && projectile_spawn.world_generation == pub_gen
				  && projectile_spawn.payload.spawn_arrow.item_id == thrown[k]
				  && !projectile_spawn.payload.spawn_arrow.fireball
				  && server && *server
				  && (*server)->type == (k == 0 ? ENTITY_EGG
					  : k == 1 ? ENTITY_SNOWBALL : ENTITY_FISH)
				  && server_local_entity_world(s, *server) == ne
				  && client_entity(1, id, ENTITY_ARROW)
				  && !client_entity(0, id, ENTITY_ARROW),
				"guest egg, snowball or hook publishes only in Nether");
		}
		/* A same-coordinate pair catches a tile lookup that silently falls
		 * back to slot 0. The ordinary dispenser producer runs in its block
		 * callback's world context, as it does during redstone dispatch. */
		const w_coord_t dx = 10, dy = 70, dz = 10;
		check(dispenser_te_register_w(ne, dx, dy, dz) >= 0
			  && dispenser_te_register_w(&s->worlds[0], dx, dy, dz) >= 0,
			"fixture: same-coordinate dispensers register in both worlds");
		struct dispenser_te* ne_te = dispenser_te_lookup_w(ne, dx, dy, dz);
		struct dispenser_te* ow_te = dispenser_te_lookup_w(&s->worlds[0],
			dx, dy, dz);
		check(ne_te && ow_te && ne_te != ow_te,
			"fixture: dispenser tile stores are distinct across worlds");
		if(ne_te && ow_te) {
			const uint16_t dispensed[] = {ITEM_ARROW, ITEM_EGG,
				ITEM_SNOW_BALL, ITEM_STICK};
			for(unsigned k = 0; k < 4; k++) {
				memset(ne_te->items, 0, sizeof(ne_te->items));
				memset(ow_te->items, 0, sizeof(ow_te->items));
				ne_te->items[0] = (struct item_data){.id = dispensed[k],
					.count = 1};
				ow_te->items[0] = (struct item_data){.id = ITEM_BED,
					.count = 1};
				ne_te->dispenser_random.seed = 1;
				projectile_spawns = dispenser_items = dispenser_smoke = 0;
				projectile_watch_item = dispensed[k];
				uint32_t disp_before = s->next_entity_id;
			s->block_world = ne;
			server_local_dispenser_dispense(s, dx, dy, dz, 5);
			s->block_world = NULL;
			if(k == 0) {
				s->paused = false; tick(s); s->paused = true;
			} else
				clin_update_drain();
			check(ne_te->items[0].count == 0
				  && ow_te->items[0].id == ITEM_BED
				  && ow_te->items[0].count == 1,
					"Nether dispense consumes only its own same-coordinate tile");
			check(dispenser_smoke >= 1 && dispenser_puff.world_index == 1
				  && dispenser_puff.world_generation == pub_gen
				  && fabsf(dispenser_puff.payload.smoke_puff.pos[0]
					  - ((float)dx + 1.1F)) < 0.01F
				  && fabsf(dispenser_puff.payload.smoke_puff.pos[1]
					  - ((float)dy + 0.5F)) < 0.01F,
					"dispenser puff follows its Nether world");
			if(k < 3) {
				uint32_t id = projectile_spawn.payload.spawn_arrow.entity_id;
				struct entity** server = dict_entity_get(s->entities, id);
				check(projectile_spawns == 1 && id == disp_before + 1
					  && projectile_spawn.world_index == 1
					  && projectile_spawn.world_generation == pub_gen
					  && projectile_spawn.payload.spawn_arrow.item_id == dispensed[k]
					  && !projectile_spawn.payload.spawn_arrow.fireball
					  && server && *server
					  && (*server)->type == (k == 0 ? ENTITY_ARROW
						  : k == 1 ? ENTITY_EGG : ENTITY_SNOWBALL)
					  && server_local_entity_world(s, *server) == ne
					  && client_entity(1, id, ENTITY_ARROW)
					  && !client_entity(0, id, ENTITY_ARROW),
						"dispenser arrow, egg or snowball reaches only Nether client");
			} else {
				uint32_t id = dispenser_item.payload.spawn_item.entity_id;
				struct entity** server = dict_entity_get(s->entities, id);
				check(dispenser_items == 1 && id == disp_before + 1
					  && dispenser_item.world_index == 1
					  && dispenser_item.world_generation == pub_gen
					  && server && *server && (*server)->type == ENTITY_ITEM
					  && server_local_entity_world(s, *server) == ne
					  && client_entity(1, id, ENTITY_ITEM)
					  && !client_entity(0, id, ENTITY_ITEM),
						"generic dispenser item joins already-sent Nether column");
			}
			}
			projectile_spawns = dispenser_smoke = 0;
			projectile_watch_item = ITEM_EGG;
			memset(ow_te->items, 0, sizeof(ow_te->items));
			ow_te->items[0] = (struct item_data){.id = ITEM_EGG, .count = 1};
			ow_te->dispenser_random.seed = 1;
			s->block_world = &s->worlds[0];
			server_local_dispenser_dispense(s, dx, dy, dz, 5);
			s->block_world = NULL;
			clin_update_drain();
			check(projectile_spawns == 1 && projectile_spawn.world_index == 0
				  && projectile_spawn.world_generation == ow_gen
				  && client_entity(0, projectile_spawn.payload.spawn_arrow.entity_id,
					  ENTITY_ARROW)
				  && !client_entity(1, projectile_spawn.payload.spawn_arrow.entity_id,
					  ENTITY_ARROW)
				  && dispenser_smoke >= 1 && dispenser_puff.world_index == 0
				  && ne_te->items[0].count == 0,
				"same-coordinate slot0 dispenser remains isolated");
			/* Bind real dispenser windows in both worlds, then mutate only the
			 * Nether tile. The wrong-world window must keep its own stack. */
			ne_te->items[0] = (struct item_data){.id = ITEM_EGG, .count = 3};
			ow_te->items[0] = (struct item_data){.id = ITEM_SNOW_BALL,
				.count = 4};
			check(server_world_set_block_silent(ne, dx, dy, dz,
				  (struct block_data){.type = BLOCK_DISPENSER, .metadata = 5})
				  && server_world_set_block_silent(&s->worlds[0], dx, dy, dz,
				  (struct block_data){.type = BLOCK_DISPENSER, .metadata = 5}),
				"fixture: same-coordinate dispenser blocks back both windows");
			struct thread window_consumer;
			__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
			assert(thread_create(&window_consumer, consumer_entry, NULL, 64));
			bool host_open = server_local_open_inventory_in(s, &s->worlds[0],
				&s->player, &inventory_logic_dispenser, DISPENSER_SIZE,
				dx, dy, dz, WINDOWC_DISPENSER, WINDOW_TYPE_DISPENSER);
			bool guest_open = server_local_open_inventory_in(s, ne,
				s->guest_actor, &inventory_logic_dispenser, DISPENSER_SIZE,
				dx, dy, dz, WINDOWC_DISPENSER, WINDOW_TYPE_DISPENSER);
			__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
			thread_join(&window_consumer);
			clin_update_drain();
			check(host_open && guest_open
				  && server_local_window_bound_to(s->player.active_inventory,
					  &s->worlds[0])
				  && server_local_window_bound_to(
					  s->guest_actor->active_inventory, ne)
				  && s->player.active_inventory->items[DISPENSER_SLOT_STORAGE].count == 4
				  && s->guest_actor->active_inventory->items[DISPENSER_SLOT_STORAGE].count == 3,
				"same-coordinate open windows bind to their own dispenser worlds");
			s->block_world = ne;
			server_local_dispenser_dispense(s, dx, dy, dz, 5);
			s->block_world = NULL;
			clin_update_drain();
			check(host_open && guest_open && ne_te->items[0].count == 2
				  && ow_te->items[0].count == 4
				  && s->guest_actor->active_inventory->items[DISPENSER_SLOT_STORAGE].count == 2
				  && s->player.active_inventory->items[DISPENSER_SLOT_STORAGE].count == 4,
				"Nether dispense refreshes only its bound open window");
			struct server_rpc close = {.type = SRPC_WINDOW_CLOSE,
				.payload.window_close.window = WINDOWC_DISPENSER};
			__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
			assert(thread_create(&window_consumer, consumer_entry, NULL, 64));
			assert(svin_rpc_send(&close));
			assert(svin_rpc_send_from(guest_source(s), &close));
			server_local_test_process_messages(s);
			__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
			thread_join(&window_consumer);
			clin_update_drain();
		}
		clin_test_set_consume_observer(NULL, NULL);
		teardown(s);
		printf("%s test_a2w_publication projectiles (%u failures)\n",
			failures ? "FAIL" : "PASS", failures);
		return failures ? 1 : 0;
	}
	if(fireball_mode) {
	/* A Nether-mob-owned shot is constructed in slot 1. The production
	 * flush must publish its display sprite with that world's stamp. */
	struct entity** shooter = dict_entity_get(s->entities, monster_ids[0]);
	check(shooter && *shooter && server_local_entity_world(s, *shooter) == ne
		  && server_world_is_chunk_sent(ne, 0, 0)
		  && clin_world_view_slot(1) == 1,
		  "fixture: live Nether owner and guest view are ready for fireball");
	server_local_spawn_fireball((vec3){5.5F, 100.0F, 5.5F},
		(vec3){1.0F, 0.0F, 0.0F}, monster_ids[0], s);
	s->paused = false;
	tick(s);
	s->paused = true;
	clin_update_drain();
	check(fireball_spawns == 1 && fireball_slot == 1
		  && fireball_gen == pub_gen,
		  "live Nether fireball publishes one exact-generation sprite spawn");
	struct entity** server_fireball = dict_entity_get(s->entities, fireball_id);
	struct entity** client_fireball = dict_entity_get(
		client_payload(1).entities, fireball_id);
	check(server_fireball && *server_fireball
		  && (*server_fireball)->type == ENTITY_FIREBALL
		  && server_local_entity_world(s, *server_fireball) == ne
		  && client_fireball && *client_fireball
		  && (*client_fireball)->type == ENTITY_ARROW
		  && (*client_fireball)->data.arrow.fireball
		  && !client_entity(0, fireball_id, ENTITY_ARROW),
		  "fireball simulation and visible sprite share ID in Nether only");
	uint32_t nether_fireball_id = fireball_id;
	struct client_rpc remove_sprite = {.type = CRPC_ENTITY_DESTROY};
	clin_rpc_stamp(&remove_sprite, 1, pub_gen);
	remove_sprite.payload.entity_destroy.entity_id = nether_fireball_id;
	clin_process(&remove_sprite);
	check(!client_entity(1, nether_fireball_id, ENTITY_ARROW),
		  "fixture: client sprite removed before resident snapshot replay");
	if(server_fireball && *server_fireball)
		check(server_local_try_publish_entity_w(s, ne, *server_fireball),
			  "resident fireball snapshot admitted to secondary publisher");
	clin_update_drain();
	check(fireball_spawns == 2 && fireball_slot == 1
		  && fireball_gen == pub_gen
		  && client_entity(1, nether_fireball_id, ENTITY_ARROW)
		  && (*dict_entity_get(client_payload(1).entities,
			nether_fireball_id))->data.arrow.fireball,
		  "resident snapshot restores fireball visual kind and exact world stamp");
	struct client_rpc stale_fireball = {.type = CRPC_SPAWN_ARROW};
	clin_rpc_stamp(&stale_fireball, 1, pub_gen - 1);
	stale_fireball.payload.spawn_arrow.entity_id = nether_fireball_id + 1000000;
	stale_fireball.payload.spawn_arrow.item_id = ITEM_SNOW_BALL;
	stale_fireball.payload.spawn_arrow.fireball = true;
	stale_fireball.payload.spawn_arrow.pos[0] = 5.5F;
	stale_fireball.payload.spawn_arrow.pos[1] = 100.0F;
	stale_fireball.payload.spawn_arrow.pos[2] = 5.5F;
	clin_process(&stale_fireball);
	check(!client_entity(1, stale_fireball.payload.spawn_arrow.entity_id,
		ENTITY_ARROW), "stale Nether sprite spawn cannot enter live client world");
	server_local_spawn_fireball((vec3){5.5F, 100.0F, 5.5F},
		(vec3){1.0F, 0.0F, 0.0F}, 0, s);
	s->paused = false;
	tick(s);
	s->paused = true;
	clin_update_drain();
	check(fireball_spawns == 3 && fireball_slot == 0
		  && client_entity(0, fireball_id, ENTITY_ARROW)
		  && !client_entity(1, fireball_id, ENTITY_ARROW),
		  "slot-0 fireball publication remains intact");
	size_t host_hotbar_now = INVENTORY_SLOT_HOTBAR
		+ inventory_get_hotbar(&s->player.inventory);
	s->player.inventory.items[host_hotbar_now]
		= (struct item_data){.id = ITEM_SNOW_BALL, .count = 2};
	struct server_rpc host_throw = {.type = SRPC_USE_ITEM,
		.payload.use_item = {.charge = 1.0F}};
	host_send(s, &host_throw);
	clin_update_drain();
	struct entity** client_snowball = dict_entity_get(
		client_payload(0).entities, snowball_id);
	check(snowball_spawns == 1 && client_snowball && *client_snowball
		  && (*client_snowball)->type == ENTITY_ARROW
		  && !(*client_snowball)->data.arrow.fireball
		  && !client_entity(1, snowball_id, ENTITY_ARROW),
		  "real snowball spawn keeps fireball visual flag clear");
	/* The terminal cosmetic event uses the same world route. Keep both
	 * particle cameras near the blast so this verifies delivery, not culling. */
	particle_system_set_camera(client_payload(0).particles,
		(vec3){5.5F, 100.0F, 5.5F});
	particle_system_set_camera(client_payload(1).particles,
		(vec3){5.5F, 100.0F, 5.5F});
	size_t ow_particles = particle_system_count(client_payload(0).particles);
	size_t ne_particles = particle_system_count(client_payload(1).particles);
	server_world_explode_flaming(ne, (vec3){5.5F, 100.0F, 5.5F}, 1.0F);
	clin_update_drain();
	check(nether_explosions == 1 && explosion_slot == 1
		  && explosion_gen == pub_gen
		  && particle_system_count(client_payload(1).particles) > ne_particles
		  && particle_system_count(client_payload(0).particles) == ow_particles,
		  "near Nether explosion produces world-bound flash and smoke only there");
	clin_test_set_consume_observer(NULL, NULL);
	teardown(s);
	printf("%s test_a2w_publication fireball (%u failures)\n",
		failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
	}
	assert(pub_gen > 1);
	bool live_sign_before_stale = client_sign(1, PUB_X, PUB_Z, 'N');
	struct client_rpc stale_sign = {.type = CRPC_SIGN_TEXT};
	clin_rpc_stamp(&stale_sign, 1, pub_gen - 1);
	stale_sign.payload.sign_text.x = PUB_X;
	stale_sign.payload.sign_text.y = PUB_Y;
	stale_sign.payload.sign_text.z = PUB_Z;
	memset(stale_sign.payload.sign_text.grid, ' ', SIGN_SIZE);
	stale_sign.payload.sign_text.grid[0] = 'S';
	clin_process(&stale_sign);
	check(client_sign(1, PUB_X, PUB_Z, 'N') == live_sign_before_stale
			  && !client_sign(1, PUB_X, PUB_Z, 'S')
			  && client_sign(0, PUB_X, PUB_Z, 'O'),
		  "stale secondary generation cannot replace either sign store");
	unsigned stale_reset_before = pub_resets;
	struct server_rpc stale_resend = {.type = SRPC_WORLD_RESEND,
		.payload.world_resend = {.world_index = 1,
			.world_generation = pub_gen - 1}};
	check(svin_rpc_send(&stale_resend), "stale secondary resend queued");
	server_local_test_process_messages(s);
	clin_update_drain();
	check(pub_resets == stale_reset_before,
		  "stale secondary resend cannot reset the live world");

	/* The real world-scoped resend must reset only slot1 and make its column
	 * publishable again. Pack the 16-token inbox so a single column with 18
	 * signs cannot finish in one server step. */
	unsigned reset_before = pub_resets;
	clin_world_payload_set_resend_sender(send_scoped_resend);
	struct piston_moving_state sidecar = {.stored_id = BLOCK_STONE,
		.facing = 1};
	struct client_rpc bad_sidecar = {.type = CRPC_PISTON_MOVING,
		.world_index = 1, .world_generation = pub_gen,
		.payload.piston_moving = {.x = 0, .y = -1, .z = 0}};
	assert(piston_moving_wire_encode(&bad_sidecar.payload.piston_moving.state,
		&sidecar));
	clin_process(&bad_sidecar);
	check(clin_world_payload_resend_pending(1),
		  "failed secondary sidecar install raises a real scoped resend marker");
	clin_update_drain();
	check(clin_world_payload_resend_requested(1),
		  "client enqueues one exact-generation secondary resend request");
	struct client_rpc filler = {.type = CRPC_TRANSFER_HOLD,
		.payload.transfer_hold = {.actor_slot = 2}};
	unsigned queued = 0;
	while(queued < 1024 && clin_try_rpc_send(&filler)) queued++;
	check(queued > 0 && queued < 1024,
		  "fixture: real client inbox token pool saturated before resend reply");
	server_local_test_process_messages(s);
	check(pub_resets == reset_before && server_world_is_chunk_sent(ne, 0, 0),
		  "full inbox defers RESET without invalidating secondary publication");
	clin_update_drain();
	pub_chunk = pub_signs = pub_monsters = 0;
	publication_out_of_order = false;
	s->paused = false;
	tick(s); /* server retries its pending reset once client tokens return */
	s->paused = true;
	check(pub_resets == reset_before + 1,
		  "secondary resend produces a stamped STREAM_RESET");
	check(!server_world_is_chunk_sent(ne, 0, 0),
		  "resend invalidates secondary column publication state");
	queued = 0;
	while(queued < 1024 && clin_try_rpc_send(&filler)) queued++;
	check(queued > 0 && queued < 1024,
		  "fixture: real client inbox token pool saturated");
	server_transfer_stream_step(s); /* must defer, never block */
	check(!server_world_is_chunk_sent(ne, 0, 0),
		  "full inbox cannot mark secondary column sent");
	clin_update_drain();
	for(unsigned i = 0; i < 300 && !server_world_is_chunk_sent(ne, 0, 0); i++) {
		server_transfer_stream_step(s);
		clin_update_drain();
	}
	check(server_world_is_chunk_sent(ne, 0, 0),
		  "CHUNK, 18 signs and entities finish across bounded retry steps");
	printf("PUB resend chunk=%u signs=%u monsters=%u stage=%u sign_cursor=%u "
		   "entity_after=%u client_sign=%u client_e0=%u client_e1=%u\n",
		   pub_chunk, pub_signs, pub_monsters,
		   center ? center->a2w_pub_stage : 0,
		   center ? center->a2w_sign_cursor : 0,
		   center ? center->a2w_entity_after_id : 0,
		   client_sign(1, PUB_X, PUB_Z, 'N'),
		   client_monster(1, monster_ids[0]),
		   client_monster(1, monster_ids[1]));
	check(pub_chunk == 1 && pub_signs == PUB_SIGNS && pub_monsters == 2,
		  "resend emits each accepted payload exactly once despite backpressure");
	check(!publication_out_of_order,
		  "resend signs and entities follow their republished center CHUNK");
	check(client_sign(0, PUB_X, PUB_Z, 'O') && !wrong_stamp,
		  "resend does not alter other world or reuse stale generation");
	check(client_sign(1, PUB_X, PUB_Z, 'N')
			  && client_monster(1, monster_ids[0])
			  && client_monster(1, monster_ids[1]),
		  "secondary client context fully restored after resend");
	/* A fully sent column must still publish new joins in that same world;
	 * replay is not the only path that needs a world-stamped entity packet. */
	struct item_data drop = {.id = ITEM_DIAMOND, .count = 1};
	struct entity* live_item = server_local_spawn_item_w(ne,
		(vec3){5.5F, 6.1F, 5.5F}, &drop, false);
	struct entity* live_monster = server_local_spawn_monster_at_w(ne,
		(vec3){7.5F, 6.1F, 5.5F}, MONSTER_PIG_ZOMBIE);
	struct entity* reserved_tnt = server_local_reserve_primed_tnt(s);
	struct entity* live_tnt = reserved_tnt
		? server_local_commit_reserved_chain_tnt(ne, reserved_tnt,
			(vec3){9.5F, 6.1F, 5.5F}) : NULL;
	check(live_item && live_monster && live_tnt,
		  "fixture: item, monster and TNT join the live secondary column");
	if(live_item && live_monster && live_tnt) {
		live_item_id = live_item->id;
		live_monster_id = live_monster->id;
		live_tnt_id = live_tnt->id;
		for(unsigned i = 0; i < 16
			&& (live_items != 1 || live_monsters != 1 || live_tnts != 1); i++) {
			server_transfer_stream_step(s);
			clin_update_drain();
		}
		check(live_items == 1 && live_monsters == 1 && live_tnts == 1,
			  "post-publication item, monster and TNT each reach slot1 exactly once");
		check(client_entity(1, live_item_id, ENTITY_ITEM)
				  && client_entity(1, live_monster_id, ENTITY_MONSTER)
				  && client_entity(1, live_tnt_id, ENTITY_PRIMED_TNT)
				  && !client_entity(0, live_item_id, ENTITY_ITEM),
			  "post-publication joins inhabit only the secondary client store");
		struct entity** client_live
			= dict_entity_get(client_payload(1).entities, live_monster_id);
		float client_x_before = client_live && *client_live
			? (*client_live)->network_pos[0] : 0.0F;
		vec3 moved = {live_monster->pos[0] + 1.25F,
			live_monster->pos[1], live_monster->pos[2]};
		live_monster->teleport(live_monster, moved);
		(void)live_monster->tick_server(live_monster, s);
		clin_update_drain();
		client_live = dict_entity_get(client_payload(1).entities,
			live_monster_id);
		check(live_monster_moves >= 1 && live_move_slot == 1
				  && live_move_gen == pub_gen,
			  "secondary monster's real server tick emits a world-stamped MOVE");
		check(client_live && *client_live
				  && fabsf((*client_live)->network_pos[0] - moved[0]) < 0.5F
				  && fabsf((*client_live)->network_pos[0] - client_x_before) > 0.5F
				  && !client_entity(0, live_monster_id, ENTITY_MONSTER),
			  "secondary client monster receives movement without touching slot0");
		/* Keep the previously spawned TNT from detonating while the death
		 * animation exercises the production entity reaper. */
		live_tnt->data.primed_tnt.fuse = TNT_FUSE_DEFAULT;
		for(unsigned i = 0; i < 300
			&& !server_world_area_loaded_radius(ne, 7, 0, 5, 32); i++) {
			server_transfer_stream_step(s);
			clin_update_drain();
		}
		check(server_world_area_loaded_radius(ne, 7, 0, 5, 32),
			  "fixture: secondary monster has an active entity-tick area");
		live_monster->health = 0;
		for(unsigned i = 0; i < 30
			&& dict_entity_get(s->entities, live_monster_id); i++) {
			s->paused = false;
			tick(s);
			s->paused = true;
		}
		check(!dict_entity_get(s->entities, live_monster_id)
				  && live_monster_destroys == 1
				  && live_destroy_slot == 1
				  && live_destroy_gen == pub_gen,
			  "secondary monster death publishes one stamped DESTROY");
		check(!client_entity(1, live_monster_id, ENTITY_MONSTER)
				  && !client_entity(0, live_monster_id, ENTITY_MONSTER),
			  "monster removal clears only its secondary client entity");
	}
	/* A monster created outside the guest's wanted ring is not initially
	 * published. Once it moves into an already-sent center, that column must
	 * replay the entity spawn exactly once without resending terrain/signs. */
	struct entity* crossing = server_local_spawn_monster_at_w(ne,
		(vec3){(float)(CHUNK_SIZE * (server_local_view_radius(s) + 3)) + 0.5F,
			6.1F, 5.5F}, MONSTER_PIG_ZOMBIE);
	check(crossing != NULL, "fixture: monster joins unsent secondary support");
	if(crossing) {
		crossing_monster_id = crossing->id;
		struct entity* newer = server_local_spawn_monster_at_w(ne,
			(vec3){12.5F, 6.1F, 11.5F}, MONSTER_PIG_ZOMBIE);
		check(newer && newer->id > crossing_monster_id,
			"fixture: newer resident monster has a higher entity ID");
		if(newer)
			crossing_newer_id = newer->id;
		bool filled_fifteen_crossing = true;
		for(unsigned i = 0; i < 15; i++)
			filled_fifteen_crossing &= clin_try_rpc_send(&filler);
		check(filled_fifteen_crossing,
			"fixture: one inbox token remains for newer secondary spawn");
		server_transfer_stream_step(s);
		clin_update_drain();
		check(crossing_newer_spawns == 1
				  && client_entity(1, crossing_newer_id, ENTITY_MONSTER),
			"newer center monster is accepted before older support monster moves");
		check(!client_entity(1, crossing_monster_id, ENTITY_MONSTER),
			"unsent support monster has no premature client spawn");
		vec3 center_pos = {11.5F, 6.1F, 11.5F};
		crossing->teleport(crossing, center_pos);
		server_local_refresh_entity_membership(s, crossing);
		unsigned saturated = 0;
		while(saturated < 1024 && clin_try_rpc_send(&filler)) saturated++;
		check(saturated > 0 && saturated < 1024,
			"fixture: crossing rearm encounters a full client inbox");
		server_transfer_stream_step(s);
		check(crossing_spawns == 0,
			"older crossing spawn is retained while client inbox is full");
		clin_update_drain();
		for(unsigned i = 0; i < 16 && crossing_spawns == 0; i++) {
			server_transfer_stream_step(s);
			clin_update_drain();
		}
		check(crossing_spawns == 1
				  && crossing_newer_spawns == 1
				  && client_entity(1, crossing_monster_id, ENTITY_MONSTER)
				  && !client_entity(0, crossing_monster_id, ENTITY_MONSTER),
			"older crossing retries once without replaying accepted newer spawn");
	}
	/* The same public guest input route casts projectiles in the committed
	 * secondary world. The host's matching hotbar item is only an isolation
	 * sentinel; the guest must pay for its own throw. */
	/* COMMIT first publishes HOLD_END and a fresh correction token. A guest
	 * action remains intentionally inert until the client has consumed that
	 * hold and its first tagged destination pose has been accepted. Complete
	 * the public handoff before testing USE_ITEM, just as gameplay does. */
	for(unsigned i = 0; i < 32
		&& server_transfer_actor_frozen(s, s->guest_actor); i++) {
		clin_update_drain();
		tick(s);
	}
	check(!server_transfer_actor_frozen(s, s->guest_actor),
		  "guest transfer HOLD_END clears before destination input");
	guest_sample(s, s->guest_actor->x, s->guest_actor->y, s->guest_actor->z,
		s->guest_actor->on_ground, s->guest_actor->correction_seq);
	check(!server_transfer_actor_inert(s, s->guest_actor),
		  "accepted tagged destination pose releases guest presentation hold");
	/* R17: real client hold -> tagged RPC -> server AIR -> secondary client
	 * AIR. A mailbox-only test missed the console's final-cracks/no-break. */
	struct client_session* miner = &gstate.players[1];
	check(client_session_request_local_gameplay(miner), "guest requests body for mining after transfer");
	for(unsigned i = 0; i < 32 && !miner->local_player; i++) {
		client_session_flush_local(miner); tick(s);
	}
	assert(miner->local_player && miner->local_gameplay_ready);
	struct input_player_frame mining_frame = {.connected = true};
	struct input_device mining_device = {.kind = INPUT_DEVICE_WPAD, .channel = 1};
	struct inventory* mining_inventory = windowc_get_latest(
		miner->windows[WINDOWC_INVENTORY]);
	mining_inventory->items[INVENTORY_SLOT_HOTBAR
		+ inventory_get_hotbar(mining_inventory)] = (struct item_data){0};
	check(server_world_set_block(ne, 2, 6, 2,
		(struct block_data){.type = BLOCK_NETHERRACK}),
		"fixture: secondary Netherrack placed for real client mining");
	clin_update_drain();
	check(world_get_block(client_payload(1).terrain, 2, 6, 2).type
		== BLOCK_NETHERRACK, "guest sees secondary Netherrack before mining");
	/* Seed the pre-existing snapshot even on RED, so removal is independently tested. */
	world_set_block(client_payload(1).terrain, 2, 6, 2,
		(struct block_data){.type = BLOCK_NETHERRACK}, false);
	miner->camera_hit.hit = true;
	miner->camera_hit.entity_hit = false;
	miner->camera_hit.x = 2; miner->camera_hit.y = 6; miner->camera_hit.z = 2;
	miner->camera_hit.side = SIDE_TOP;
	miner->local_player->on_ground = true;
	miner->in_water = false;
	screen_ingame_guest_action_reset(miner);
	(void)screen_ingame_guest_action_frame(miner, &mining_frame, &mining_device, true);
	mining_frame.down[IB_ACTION1] = mining_frame.pressed[IB_ACTION1] = true;
	if(screen_ingame_guest_action_frame(miner, &mining_frame, &mining_device, true))
		screen_ingame_update_owned(miner, 1, &mining_frame, 0.05F);
	tick(s);
	check(miner->digging.active, "real guest hold starts Netherrack mining");
	mining_frame.pressed[IB_ACTION1] = false;
	/* Advance only the dig's elapsed time, keeping actual emission and
	 * authoritative/client mutation paths; no finished RPC is injected. */
	miner->digging.start.tv_sec -= 10;
	screen_ingame_update_owned(miner, 1, &mining_frame, 0.05F);
	tick(s);
	struct block_data mined;
	check(server_world_get_block(ne, 2, 6, 2, &mined)
		&& mined.type == BLOCK_AIR, "guest client hold actually removes server Netherrack");
	check(world_get_block(client_payload(1).terrain, 2, 6, 2).type == BLOCK_AIR,
		"guest client receives AIR after secondary Netherrack mining");
	/* G9 (ASTRA-G9-REVIEW Required 5): the mining ran in a PAUSED update --
	 * RPC phase only, no World.tick and so no light drain -- so the Nether
	 * centre is pinned by pending light. The production streamer must defer
	 * it (no snapshot of an unsettled column), and the next unpaused
	 * production update must settle it at its prescribed drain. No manual
	 * drain: the fixture uses the same pause/tick idiom as above. */
	{
		const bool pinned = server_world_light_pinned(ne, 0, 0)
			&& server_world_light_pending(ne);
		const unsigned stage_before = dict_server_chunks_get(ne->chunks,
			S_CHUNK_ID(0, 0))->a2w_pub_stage;
		server_transfer_stream_step(s);
		clin_update_drain();
		const unsigned stage_after = dict_server_chunks_get(ne->chunks,
			S_CHUNK_ID(0, 0))->a2w_pub_stage;
		check(pinned && server_world_light_pending(ne)
				&& stage_after == stage_before,
			"paused mining leaves the Nether centre pending; the streamer "
			"defers it (no unsettled snapshot)");
		s->paused = false; tick(s); s->paused = true;
		check(!server_world_light_pending(ne)
				&& !server_world_light_pinned(ne, 0, 0),
			"the next unpaused production update drains it at the prescribed "
			"boundary (retry makes progress)");
	}
	screen_ingame_guest_action_reset(miner);
	size_t guest_hotbar = INVENTORY_SLOT_HOTBAR
		+ inventory_get_hotbar(&s->guest_actor->inventory);
	size_t host_hotbar = INVENTORY_SLOT_HOTBAR
		+ inventory_get_hotbar(&s->player.inventory);
	s->guest_actor->inventory.items[guest_hotbar]
		= (struct item_data){.id = ITEM_EGG, .count = 3};
	s->player.inventory.items[host_hotbar]
		= (struct item_data){.id = ITEM_EGG, .count = 3};
	struct server_rpc use = {.type = SRPC_USE_ITEM,
		.payload.use_item = {.charge = 1.0F}};
	check(guest_send(s, &use), "guest egg throw uses public owner route");
	for(unsigned i = 0; i < 16 && guest_egg_spawns == 0; i++) {
		server_transfer_stream_step(s);
		clin_update_drain();
	}
	struct entity** egg = dict_entity_get(s->entities, guest_egg_id);
	check(guest_egg_spawns == 1 && guest_egg_slot == 1
			  && guest_egg_gen == pub_gen && egg && *egg
			  && server_local_entity_world(s, *egg) == ne
			  && client_entity(1, guest_egg_id, ENTITY_ARROW)
			  && !client_entity(0, guest_egg_id, ENTITY_ARROW),
		  "guest egg spawns once in server/client Nether, stamped to slot1");
	check(s->guest_actor->inventory.items[guest_hotbar].count == 2
			  && s->player.inventory.items[host_hotbar].count == 3,
		  "guest throw consumes only guest's held egg");
	s->guest_actor->inventory.items[guest_hotbar]
		= (struct item_data){.id = ITEM_FISHING_ROD, .count = 1};
	check(guest_send(s, &use), "guest rod cast uses public owner route");
	for(unsigned i = 0; i < 16 && guest_fish_spawns == 0; i++) {
		server_transfer_stream_step(s);
		clin_update_drain();
	}
	struct entity** hook = dict_entity_get(s->entities, guest_fish_id);
	check(guest_fish_spawns == 1 && guest_fish_slot == 1
			  && guest_fish_gen == pub_gen
			  && s->guest_actor->fish_entity_id == guest_fish_id
			  && !s->player.fish_entity_id && hook && *hook
			  && server_local_entity_world(s, *hook) == ne
			  && client_entity(1, guest_fish_id, ENTITY_ARROW)
			  && !client_entity(0, guest_fish_id, ENTITY_ARROW),
		  "guest hook spawns once in its Nether and keeps guest angler identity");
	check(guest_send(s, &use) && !s->guest_actor->fish_entity_id,
		  "guest reel clears its hook before later checkpoint");
	/* A second authentic recovery leaves exactly one inbox token for RESET,
	 * then one for CHUNK. The column is client-claimed but only at stage 1;
	 * moving its owner away must send UNLOAD before saving it. */
	clin_process(&bad_sidecar);
	clin_update_drain();
	check(clin_world_payload_resend_requested(1),
		  "second client recovery request is outstanding");
	bool filled_fifteen = true;
	for(unsigned i = 0; i < 15; i++)
		filled_fifteen &= clin_try_rpc_send(&filler);
	check(filled_fifteen,
		  "fixture: one client inbox token remains for second RESET");
	unsigned second_reset_before = pub_resets;
	server_local_test_process_messages(s);
	clin_update_drain();
	check(pub_resets == second_reset_before + 1
			  && !server_world_is_chunk_sent(ne, 0, 0),
		  "second scoped RESET clears center publication before partial replay");
	filled_fifteen = true;
	for(unsigned i = 0; i < 15; i++)
		filled_fifteen &= clin_try_rpc_send(&filler);
	check(filled_fifteen,
		  "fixture: one client inbox token remains for partial CHUNK");
	server_transfer_stream_step(s);
	center = dict_server_chunks_get(ne->chunks, S_CHUNK_ID(0, 0));
	printf("PUB partial guest=(%.1f,%.1f) center_stage=%u center_sent=%u "
		   "center_loaded=%u\n", s->guest_actor->x, s->guest_actor->z,
		   center ? center->a2w_pub_stage : 0,
		   center ? center->sent : 0,
		   server_world_is_chunk_loaded(ne, 0, 0));
	check(center && center->a2w_pub_stage == 1 && !center->sent,
		  "center CHUNK accepted while sign publication waits for inbox tokens");
	clin_update_drain();
	check(pub_unloads == 0 && server_world_is_chunk_loaded(ne, 0, 0),
		  "partial center is still resident before owner moves away");
	/* Fixture pose change only selects the eviction candidate; the eviction,
	 * backpressure and UNLOAD all run through the production streamer. */
	s->guest_actor->x += 16.0 * (server_local_view_radius(s) + 10);
	/* residency-1005/2: a production far move passes a barrier that drops
	 * the accepted-pose pin (respawn/transfer); this direct pose write must
	 * too, or the evictor rightly protects the pinned old center. */
	s->guest_actor->rz.pin.valid = false;
	queued = 0;
	while(queued < 1024 && clin_try_rpc_send(&filler)) queued++;
	server_transfer_stream_step(s);
	check(server_world_is_chunk_loaded(ne, 0, 0),
		  "full inbox keeps client-claimed partial center resident for retry");
	clin_update_drain();
	for(unsigned i = 0; i < 100 && server_world_is_chunk_loaded(ne, 0, 0); i++) {
		server_transfer_stream_step(s);
		clin_update_drain();
	}
	check(pub_unloads == 1 && !server_world_is_chunk_loaded(ne, 0, 0),
		  "partial CHUNK gets one UNLOAD before server evicts its old center");
	check(!world_find_chunk(client_payload(1).terrain, 0, 0, 0)
			  && !client_sign(1, PUB_X, PUB_Z, 'N')
			  && client_sign(0, PUB_X, PUB_Z, 'O'),
		  "client UNLOAD drops only secondary center terrain and sign grids");
	/* Save and quit with the guest still attached. The production unload must
	 * checkpoint both worlds and the guest's Nether dimension, then a fresh
	 * host load and public guest Join must restore secondary publication. */
	s->guest_actor->x = 1.0;
	s->guest_actor->z = 0.5;
	s->guest_actor->rx = s->guest_actor->ry = 0;
	for(unsigned i = 0; i < 300 && !server_world_is_chunk_sent(ne, 0, 0); i++) {
		server_transfer_stream_step(s);
		clin_update_drain();
	}
	check(server_world_is_chunk_sent(ne, 0, 0),
		  "fixture: guest center republished before save and quit");
	uint32_t old_secondary_gen = pub_gen;
	check(server_local_unload_world_impl(s, true),
		  "production save and quit checkpoints attached guest and both worlds");
	clin_update_drain();
	string_t save_name;
	string_init_set_str(save_name, split_path);
	bool reopened = server_local_load_world_impl(s, save_name);
	string_clear(save_name);
	check(reopened, "production load reopens the saved host world");
	if(reopened) {
		unsigned marker_before = markers;
		for(unsigned i = 0; i < 2000 && markers == marker_before; i++)
			tick(s);
		check(markers == marker_before + 1 && s->input_ack_pending,
			  "reopened host streams to its authentic input marker");
		screen_load_world.update(&screen_load_world, 30.0F);
		clin_update_input_readiness();
		tick(s);
		check(!s->input_ack_pending,
			  "reopened host acknowledges its production input handoff");
		enable_split(s);
		struct client_session* rejoin = &gstate.players[1];
		check(client_session_request_local_join(rejoin),
			  "public guest Join starts after saved host load");
		for(unsigned i = 0; i < 500
			&& rejoin->local_phase != CLIENT_GUEST_ATTACHED; i++) {
			client_session_flush_local(rejoin);
			tick(s);
		}
		check(rejoin->local_phase == CLIENT_GUEST_ATTACHED
				  && s->guest_actor
				  && s->guest_actor->dimension == WORLD_DIM_NETHER,
			  "saved guest re-admits in Nether through public Join");
		struct server_world* restored = server_local_world_of(s, s->guest_actor);
		check(restored && restored->world_index == 1
				  && restored->generation > old_secondary_gen,
			  "rejoined guest owns a new-generation secondary world");
		if(restored) {
			pub_gen = restored->generation;
			for(unsigned i = 0; i < 400
				&& !server_world_is_chunk_sent(restored, 0, 0); i++) {
				server_transfer_stream_step(s);
				clin_update_drain();
			}
			check(server_world_is_chunk_sent(restored, 0, 0)
					  && client_sign(1, PUB_X, PUB_Z, 'N'),
				  "saved Nether signs republish to newly bound guest view");
			check(client_sign(0, PUB_X, PUB_Z, 'O'),
				  "same-coordinate host sign survives secondary save and reopen");
			/* A restored entity gets a FRESH id (server_local_restore_chunk_
			 * entities_w -> server_local_gen_id), so the pre-save ids only
			 * matched while nothing else drew ids in between; I03's spawn
			 * candidates (Beta newInstance takes an id even when refused)
			 * now do. Identify the saved pig zombies by kind and position. */
			bool saved_monster = false;
			{
				dict_entity_it_t it;
				for(dict_entity_it(it, client_payload(1).entities);
					!dict_entity_end_p(it); dict_entity_next(it)) {
					const struct entity* m = dict_entity_ref(it)->value;
					if(!m || m->type != ENTITY_MONSTER
					   || m->data.monster.id != MONSTER_PIG_ZOMBIE)
						continue;
					for(unsigned k = 0; k < 2; k++)
						if(fabsf(m->pos[0] - (5.5F + 2.0F * (float)k)) < 1.0F
						   && fabsf(m->pos[2] - 5.5F) < 1.0F)
							saved_monster = true;
				}
			}
			check(saved_monster,
				  "saved secondary monster republishes to new client context");
			/* Last, isolate the budget-deferred outer-ring eviction contract
			 * from all earlier publication/entity assertions. This is the
			 * exact pending state ensure_populated leaves after deferral; the
			 * production streamer still selects and saves its own victims. */
			const w_coord_t edge = server_local_view_radius(s);
			const w_coord_t support = edge + 1;
			struct server_chunk* support_chunk = NULL;
			check(s->guest_actor && s->guest_actor->x >= 0.0
					  && s->guest_actor->x < 16.0
					  && s->guest_actor->z >= 0.0
					  && s->guest_actor->z < 16.0,
				  "fixture: restored secondary actor center is column zero");
			if(!server_world_is_chunk_loaded(restored, support, 0))
				(void)server_world_load_chunk(restored, support, 0,
					&support_chunk);
			check(server_world_is_chunk_loaded(restored, support, 0),
				  "fixture: outer-ring support is resident");
			restored->gen_pending_x[0] = restored->gen_pending_x[1] = edge;
			restored->gen_pending_z[0] = restored->gen_pending_z[1] = 0;
			restored->gen_pending[0] = true;
			restored->gen_pending[1] = false;
			server_transfer_stream_step(s);
			check(server_world_is_chunk_loaded(restored, support, 0),
				  "secondary eviction retains stream-0 pending support");
			restored->gen_pending[0] = false;
			restored->gen_pending[1] = true;
			server_transfer_stream_step(s);
			check(server_world_is_chunk_loaded(restored, support, 0),
				  "secondary eviction retains stream-1 pending support");
			restored->gen_pending[0] = restored->gen_pending[1] = false;
			for(unsigned i = 0; i < 16
				&& server_world_is_chunk_loaded(restored, support, 0); i++)
				server_transfer_stream_step(s);
			check(!server_world_is_chunk_loaded(restored, support, 0),
				  "secondary eviction releases support after pending clears");
			/* A second real trip returns the guest to the host's live world.
			 * Then checkpoint both distinct sign edits, reload the guest as an
			 * Overworld body, and re-enter the Nether to read its saved sign. */
			check(client_session_request_local_gameplay(rejoin),
				  "restored Nether guest requests public gameplay admission");
			for(unsigned i = 0; i < 100
				&& (!rejoin->local_gameplay_ready || !s->guest_gameplay); i++) {
				pump_handshake(s);
				tick(s);
			}
			check(rejoin->local_gameplay_ready && s->guest_gameplay
					  && s->guest_actor->finished_loading,
				  "restored Nether guest has an admitted playable body");
			check(server_sign(s, &s->worlds[0], 'O')
					  && server_sign(s, restored, 'N'),
				  "both distinct server sign edits exist before guest return");
			struct item_data saved_guest_item
				= s->guest_actor->inventory.items[INVENTORY_SLOT_HOTBAR
					+ inventory_get_hotbar(&s->guest_actor->inventory)];
			check(saved_guest_item.id != 0 && saved_guest_item.count != 0,
				  "fixture: guest has a nonempty held item before return save");
			uint32_t saved_nether_generation = restored->generation;
			uint32_t return_id = 0;
			check(server_transfer_request(s, s->guest_actor,
				WORLD_DIM_OVERWORLD, &return_id) == SERVER_TRANSFER_OK,
				  "guest return to the host's Overworld is accepted");
			check(drive_commit(s, return_id),
				  "guest Nether-to-Overworld transfer COMMITs");
			check(server_local_world_of(s, s->guest_actor) == &s->worlds[0]
					  && s->guest_actor->dimension == WORLD_DIM_OVERWORLD
					  && clin_world_view_slot(1) == 0,
				  "returned guest body and view bind host's live Overworld");
			check(client_session_request_local_gameplay(rejoin),
				  "returned guest requests public gameplay admission");
			for(unsigned i = 0; i < 100 && !rejoin->local_gameplay_ready; i++) {
				pump_handshake(s);
				tick(s);
			}
			check(rejoin->local_gameplay_ready
					  && client_sign(0, PUB_X, PUB_Z, 'O'),
				  "returned guest sees the Overworld's original sign");
			for(unsigned i = 0; i < 32
				&& server_transfer_actor_frozen(s, s->guest_actor); i++) {
				clin_update_drain();
				tick(s);
			}
			check(!server_transfer_actor_frozen(s, s->guest_actor),
				  "return HOLD_END clears before guest destination input");
			guest_sample(s, s->guest_actor->x, s->guest_actor->y,
				s->guest_actor->z, s->guest_actor->on_ground,
				s->guest_actor->correction_seq);
			check(!server_transfer_actor_inert(s, s->guest_actor),
				  "tagged Overworld pose releases guest return presentation");
			/* drive_commit leaves this fixture paused; the production vacant
			 * world drain runs only in active server updates. */
			s->paused = false;
			for(unsigned i = 0; i < 500
				&& (!restored->client_retired
					|| dict_server_chunks_size(restored->chunks) != 0); i++)
				tick(s);
			s->paused = true;
			check(restored->client_retired
					  && dict_server_chunks_size(restored->chunks) == 0,
				  "vacated Nether drains and client releases slot before checkpoint");
			check(server_local_unload_world_impl(s, true),
				  "production checkpoint saves both worlds after guest return");
			clin_update_drain();
			string_t return_save;
			string_init_set_str(return_save, split_path);
			bool return_reopened = server_local_load_world_impl(s, return_save);
			string_clear(return_save);
			check(return_reopened,
				  "named load reopens checkpoint made after guest return");
			if(return_reopened) {
				unsigned before_return_marker = markers;
				for(unsigned i = 0; i < 2000
					&& markers == before_return_marker; i++)
					tick(s);
				check(markers == before_return_marker + 1
						  && s->input_ack_pending,
					  "return checkpoint reload reaches host input marker");
				screen_load_world.update(&screen_load_world, 30.0F);
				clin_update_input_readiness();
				tick(s);
				check(!s->input_ack_pending,
					  "return checkpoint reload acknowledges host input");
				enable_split(s);
				check(client_session_request_local_join(rejoin),
					  "saved Overworld guest requests public Join");
				for(unsigned i = 0; i < 500
					&& rejoin->local_phase != CLIENT_GUEST_ATTACHED; i++) {
					client_session_flush_local(rejoin);
					tick(s);
				}
				check(rejoin->local_phase == CLIENT_GUEST_ATTACHED
						  && s->guest_actor
						  && s->guest_actor->dimension == WORLD_DIM_OVERWORLD
						  && server_local_world_of(s, s->guest_actor)
							 == &s->worlds[0],
					  "saved guest identity reloads in the Overworld");
				struct item_data loaded_guest_item
					= s->guest_actor->inventory.items[INVENTORY_SLOT_HOTBAR
						+ inventory_get_hotbar(&s->guest_actor->inventory)];
				check(saved_guest_item.id == loaded_guest_item.id
						  && saved_guest_item.count == loaded_guest_item.count
						  && saved_guest_item.durability == loaded_guest_item.durability
						  && saved_guest_item.nbt_record == loaded_guest_item.nbt_record,
					  "guest held inventory survives return checkpoint");
				check(server_sign(s, &s->worlds[0], 'O')
						  && client_sign(0, PUB_X, PUB_Z, 'O'),
					  "reloaded Overworld sign exists in server tile store and client");
				check(client_session_request_local_gameplay(rejoin),
					  "reloaded Overworld guest requests public gameplay");
				for(unsigned i = 0; i < 100
					&& (!rejoin->local_gameplay_ready || !s->guest_gameplay); i++) {
					pump_handshake(s);
					tick(s);
				}
				check(rejoin->local_gameplay_ready && s->guest_gameplay,
					  "reloaded Overworld guest gameplay is admitted");
				uint32_t revisit_id = 0;
				check(server_transfer_request(s, s->guest_actor,
					WORLD_DIM_NETHER, &revisit_id) == SERVER_TRANSFER_OK,
					  "saved guest requests real return to Nether sign world");
				check(drive_commit(s, revisit_id),
					  "saved guest re-enters Nether through real transfer COMMIT");
				struct server_world* revisited
					= server_local_world_of(s, s->guest_actor);
				check(revisited && revisited->dimension == WORLD_DIM_NETHER
						  && revisited->generation > saved_nether_generation
						  && clin_world_view_slot(1) == revisited->world_index,
					  "Nether sign world re-admits with new generation");
				if(revisited) {
					pub_gen = revisited->generation;
					for(unsigned i = 0; i < 400
						&& !server_world_is_chunk_sent(revisited, 0, 0); i++) {
						server_transfer_stream_step(s);
						clin_update_drain();
					}
					check(server_sign(s, revisited, 'N')
							  && client_sign(revisited->world_index,
								PUB_X, PUB_Z, 'N')
							  && server_sign(s, &s->worlds[0], 'O')
							  && client_sign(0, PUB_X, PUB_Z, 'O'),
						  "both distinct saved signs survive guest return and reload");
				}
			}
		}
	}
	clin_world_payload_set_resend_sender(NULL);
	clin_test_set_consume_observer(NULL, NULL);
	teardown(s);
	printf("%s test_a2w_publication (%u failures)\n",
		failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
