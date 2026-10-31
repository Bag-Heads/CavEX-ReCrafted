/* Vehicle world routing: admitted split bodies in DIFFERENT dimensions.
 * Terrain/body placement is fixture setup; inventory requests, ticks, LOS,
 * publication and archive gathering below are production paths.
 * Beta RetroMCP ItemMinecart.onItemUse / ItemBoat.onItemRightClick use the
 * supplied World for BOTH the tile query and the coordinate constructor. */
#include "support/owner_swap_harness.h"
#include "network/entity_archive.h"
#include "game/client_world_context.h"
#include "network/client_world_payload.h"
#include "entity/entity_boat.h"

static struct entity* client_entity(unsigned slot, uint32_t id) {
	struct clin_world_payload p;
	assert(clin_world_payload_of_slot(slot, &p));
	struct entity** e = dict_entity_get(p.entities, id);
	return e ? *e : NULL;
}

static unsigned spawn_slot;
static uint32_t spawn_generation;
static unsigned spawns;
static void flush_client_output(void) {
	/* The consumer returns inbox tokens; only the server producer flushes
	 * its retained output ring. A drain alone can leave the last spawn
	 * queued behind terrain replies. Call while the real consumer is running. */
	ptime_t start = time_get();
	while(!clin_rpc_flush_pending()) {
		assert(time_diff_s(start, time_get()) < 20);
		thread_msleep(1);
	}
}
static void observe(const struct client_rpc* rpc, void* unused) {
	consumed(rpc, unused);
	if(rpc->type == CRPC_SPAWN_BOAT || rpc->type == CRPC_SPAWN_MINECART) {
		spawns++;
		spawn_slot = rpc->world_index;
		spawn_generation = rpc->world_generation;
	}
}

static void set_w(struct server_world* w, int x, int y, int z,
	uint8_t type, uint8_t meta) {
	(void)server_world_set_block(w, x, y, z,
		(struct block_data){.type = type, .metadata = meta});
	struct block_data b;
	assert(server_world_get_block(w, x, y, z, &b) && b.type == type);
}

static struct server_local* worlds_fixture(bool guest_in_nether) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
#ifdef _WIN32
	_mkdir(test_fixture_join(split_path, "/DIM-1"));
	_mkdir(test_fixture_join(split_path, "/DIM-1/region"));
#else
	mkdir(test_fixture_join(split_path, "/DIM-1"), 0777);
	mkdir(test_fixture_join(split_path, "/DIM-1/region"), 0777);
#endif
	assert(server_local_world_admit(s, WORLD_DIM_NETHER) == &s->worlds[1]);
	assert(clin_world_payload_prepare_landing(1, s->worlds[1].generation,
		WORLD_DIM_NETHER));
	assert(clin_world_payload_bind_view(1, guest_in_nether ? 1 : 0));
	assert(clin_world_payload_bind_view(0, guest_in_nether ? 0 : 1));
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	/* Admission streamed the full single-world ring. Retire the unused
	 * columns through the real save path before the two-world budget starts;
	 * keep both bodies' pinned home footprint. Never bypass residency caps. */
	for(int x = -20; x <= 20; x++) for(int z = -20; z <= 20; z++)
		if((abs(x) > 1 || abs(z) > 1)
		   && server_world_is_chunk_loaded(&s->worlds[0], x, z))
			assert(server_world_save_chunk(&s->worlds[0], true, x, z));
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	/* This headless rig has no mesher to retire the streamed old ring.
	 * Destroy its unused client terrain to release those real backing refs. */
	world_destroy(&gstate.world);
	world_create(&gstate.world);
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	for(unsigned slot = 0; slot < 2; slot++) {
		struct server_world* w = &s->worlds[slot];
		struct server_chunk* c = NULL;
		/* Beta entity updates require their full +/-32-block area loaded. */
		for(int x = -2; x <= 2; x++) for(int z = -2; z <= 2; z++) {
			server_world_begin_update(w);
			if(!server_world_is_chunk_loaded(w, x, z))
				assert(server_world_load_chunk(w, x, z, &c) && c);
		}
		for(int x = 0; x < 16; x++) for(int z = 0; z < 16; z++) {
			for(int y = 100; y < 107; y++) set_w(w, x, y, z, BLOCK_AIR, 0);
			set_w(w, x, 100, z, BLOCK_STONE, 0);
		}
		assert(server_world_light_drain(w));
	}
	flush_client_output();
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
	s->player.world_slot = guest_in_nether ? 0 : 1;
	s->guest_actor->world_slot = guest_in_nether ? 1 : 0;
	struct server_player* players[] = {&s->player, s->guest_actor};
	for(unsigned i = 0; i < 2; i++) {
		players[i]->dimension = s->worlds[players[i]->world_slot].dimension;
		players[i]->x = 4.5; players[i]->z = 4.5;
		players[i]->y = 101.0 + PLAYER_EYE_HEIGHT;
		players[i]->rx = 0; players[i]->ry = 0;
	}
	clin_test_set_consume_observer(observe, NULL);
	server_transfer_stream_enable(s, 1);
	tick(s); /* retire the admission's per-update pose barrier normally */
	return s;
}

static bool send_actor(struct server_local* s, struct server_player* actor,
	struct server_rpc* rpc) {
	bool queued = actor == &s->player ? svin_rpc_send(rpc)
		: svin_rpc_send_from(guest_source(s), rpc);
	if(!queued) return false;
	/* Window snapshots can exceed the bounded inbox; keep the client on its
	 * own consumer thread, as in the running game, while the handler emits. */
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	server_local_test_process_messages(s);
	flush_client_output();
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
	return true;
}

static struct entity* find_vehicle(struct server_local* s, bool boat) {
	struct entity* found = NULL;
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_cref(it)->value;
		if(e->type == (boat ? ENTITY_BOAT : ENTITY_MINECART)) {
			assert(!found); found = e;
		}
		dict_entity_next(it);
	}
	return found;
}

static void request_ride(struct server_local* s, struct server_player* actor,
	struct entity* e, bool boat, bool occupied) {
	static uint32_t seq;
	struct server_rpc rpc = boat
		? (struct server_rpc){.type = SRPC_ENTITY_USE,
			.payload.entity_use = {.entity_id = e->id, .desired_occupied = occupied}}
		: (struct server_rpc){.type = SRPC_MINECART_RIDE_INTENT,
			.payload.minecart_ride_intent = {.entity_id = e->id,
				.input_epoch = s->input_epoch_counter, .request_seq = ++seq,
				.desired_occupied = occupied}};
	/* Guest cart ride ingress deliberately remains closed. */
	(void)send_actor(s, actor, &rpc);
}

static bool occupied(struct entity* e, bool boat) {
	return boat ? e->data.boat.occupied : e->data.minecart.occupied;
}

static void exercise(struct server_local* s, struct entity* e, bool boat,
	unsigned type, bool guest) {
	struct server_player* actor = guest ? s->guest_actor : &s->player;
	struct server_world* w = server_local_world_of(s, actor);
	unsigned other_slot = 1u - w->world_index;
	uint32_t id = e->id;
	/* Supplemental LOS fixture: put the legitimately spawned target beyond
	 * a distinct wall cell; its world identity is NEVER repaired by hand. */
	e->teleport(e, (vec3){7.5F, 101.5F, 4.5F});
	struct server_rpc use = {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = id, .desired_occupied = true}};
	struct server_rpc attack = {.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = id};
	for(int y = 101; y <= 104; y++) set_w(w, 5, y, 4, BLOCK_STONE, 0);
	int damage = boat ? e->data.boat.damage_taken : e->data.minecart.damage_taken;
	send_actor(s, actor, &attack);
	check((boat ? e->data.boat.damage_taken : e->data.minecart.damage_taken) == damage,
		"actor-world wall blocks real vehicle attack without mutation");
	actor->inventory.items[INVENTORY_SLOT_HOTBAR] = (struct item_data){.id = ITEM_COAL, .count = 2};
	if(!boat && type) {
		send_actor(s, actor, &use);
		check(actor->active_inventory == &actor->inventory && e->data.minecart.fuel == 0,
			"actor-world wall blocks chest/furnace use");
	} else if(boat || !guest) {
		request_ride(s, actor, e, boat, true);
		check(!occupied(e, boat), "actor-world wall blocks the acting player's mount");
	}
	for(int y = 101; y <= 104; y++) {
		set_w(w, 5, y, 4, BLOCK_AIR, 0);
		set_w(&s->worlds[other_slot], 5, y, 4, BLOCK_STONE, 0);
	}
	if(!boat && type == 2) {
		send_actor(s, actor, &use);
		check(e->data.minecart.fuel == 1200
			&& actor->inventory.items[INVENTORY_SLOT_HOTBAR].count == 1,
			"partner-world wall cannot block actor's furnace-cart fuel use");
	} else if(!boat && type == 1) {
		send_actor(s, actor, &use);
		check(actor->active_inventory != &actor->inventory,
			"partner-world wall cannot block actor's chest-cart open");
		struct server_rpc close = {.type = SRPC_WINDOW_CLOSE,
			.payload.window_close.window = WINDOWC_MINECART_CHEST};
		send_actor(s, actor, &close);
		e->data.minecart.inventory[0] = (struct item_data){.id = ITEM_DIAMOND, .count = 37};
	} else {
		request_ride(s, actor, e, boat, true);
		check(occupied(e, boat) == (boat || !guest),
			"boat admission uses actor-world LOS for either owner; guest cart stays closed");
		if(boat && occupied(e, true)) {
			tick(s); /* new logical update retires the admission's pose barrier */
			/* The owned client authors its mounted seat, rather than the
			 * old standing pose three blocks behind this LOS fixture's hull. */
			struct server_rpc steer = pose_sample(
				(double)e->pos[0] + cos((double)e->orient[0]) * BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET,
				(double)e->pos[1] + BOAT_MOUNTED_Y_OFFSET + PLAYER_EYE_HEIGHT,
				(double)e->pos[2] + sin((double)e->orient[0]) * BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET,
				actor->on_ground, actor->correction_seq);
			steer.payload.player_pos.vel_x = .12F;
			assert(send_actor(s, actor, &steer));
			assert(server_world_light_drain(w));
			float x = e->pos[0];
			for(unsigned n = 0; n < 12; n++)
				assert(server_local_test_tick_entities_w(s, w));
			clin_update_drain();
			check(e->pos[0] != x,
				"actual acting-owner pose RPC steers the mounted boat in its own world");
			struct entity* client = client_entity(w->world_index, e->id);
			check(client && client->pos[0] != x,
				"own-world client receives authoritative boat displacement");
		}
	}
	/* Both checkpoints keep one owner in the right dimension, including
	 * mounted saves. Cold chunk reload must clear transient occupancy. */
	struct chunk_entities saved = {0}, other = {0};
	assert(server_local_gather_chunk_entities_w(s, w, 0, 0, &saved));
	assert(server_local_gather_chunk_entities_w(s, &s->worlds[other_slot], 0, 0, &other));
	check(saved.count == 1 && other.count == 0,
		"per-dimension archive has exactly one owning-world vehicle and zero partner-world vehicles");
	chunk_entities_free(&saved); chunk_entities_free(&other);
	assert(server_world_light_drain(w));
	assert(server_world_save_chunk_coherent(w, 0, 0));
	assert(server_world_save_chunk_coherent(w, 0, 0));
	if(occupied(e, boat)) request_ride(s, actor, e, boat, false);
	check(!occupied(e, boat), "own-world dismount clears authoritative occupancy");
	/* EVICT admits a destroy batch plus unload atomically. Keep the real
	 * consumer running while that bounded transport transaction is checked. */
	struct thread evict_consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&evict_consumer, consumer_entry, NULL, 64));
	flush_client_output();
	bool evicted;
	ptime_t evict_start = time_get();
	while(!(evicted = server_world_save_chunk(w, true, 0, 0))) {
		assert(w->evict_retire_deferred && time_diff_s(evict_start, time_get()) < 20);
		flush_client_output(); thread_msleep(1);
	}
	flush_client_output();
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&evict_consumer); clin_update_drain();
	assert(evicted);
	check(!find_vehicle(s, boat), "chunk unload retires sole live vehicle owner");
	server_world_begin_update(w);
	struct server_chunk* loaded = NULL;
	assert(server_world_load_chunk(w, 0, 0, &loaded) && loaded);
	clin_update_drain();
	e = find_vehicle(s, boat);
	check(e && server_local_entity_world(s, e) == w && !occupied(e, boat),
		"disk chunk reload restores vehicle only in its owning world, detached");
	if(!e) return;
	if(!boat && type == 1) check(e->data.minecart.inventory[0].count == 37,
		"chest-cart cargo survives repeated checkpoint and disk reload");
	if(!boat && type == 2) check(e->data.minecart.fuel == 1200,
		"furnace-cart fuel survives repeated checkpoint and disk reload");
	id = e->id;
	attack.payload.entity_attack.entity_id = id;
	for(unsigned i = 0; i < 8 && dict_entity_get(s->entities, id); i++)
		send_actor(s, actor, &attack);
	check(!dict_entity_get(s->entities, id), "real attack destroys reloaded own-world vehicle");
	/* Secondary item joins publish through the existing resumable column
	 * cursor, after a real stream update, rather than the immediate legacy
	 * slot-0 item sender. */
	assert(server_world_light_drain(&s->worlds[0]));
	assert(server_world_light_drain(w));
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	for(unsigned n = 0; n < 100; n++) {
		server_world_begin_update(w);
		server_transfer_stream_step(s);
		flush_client_output();
		struct server_chunk* c = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
		if(c && c->sent && c->a2w_pub_stage == 3) break;
		thread_msleep(1);
	}
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
	unsigned drops = 0, diamonds = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* drop = dict_entity_cref(it)->value;
		if(drop->type == ENTITY_ITEM) {
			drops++;
			check(server_local_entity_world(s, drop) == w
				&& client_entity(w->world_index, drop->id) && !client_entity(other_slot, drop->id),
				"vehicle destruction drop belongs/publishes only to its owning world");
			if(drop->data.item.item.id == ITEM_DIAMOND) diamonds += drop->data.item.item.count;
		}
		dict_entity_next(it);
	}
	check(drops >= (boat ? 5u : type ? 2u : 1u), "vehicle destruction preserves required drop count");
	if(!boat && type == 1) check(diamonds == 37, "vehicle destruction preserves every cargo item");
}

static void placement(bool boat, unsigned type, bool guest, bool fail, unsigned slot) {
	struct server_local* s = worlds_fixture(guest == (slot == 1));
	struct server_player* actor = guest ? s->guest_actor : &s->player;
	struct server_player* partner = guest ? &s->player : s->guest_actor;
	struct inventory partner_before = partner->inventory;
	double partner_pose[] = {partner->x, partner->y, partner->z};
	uint8_t partner_health = partner->health;
	struct server_world* w = &s->worlds[slot];
	const uint16_t ids[] = {ITEM_MINECART, ITEM_MINECART_CHEST, ITEM_MINECART_FURNACE};
	uint16_t id = boat ? ITEM_BOAT : ids[type];
	actor->inventory.items[INVENTORY_SLOT_HOTBAR] = (struct item_data){.id = id, .count = 1};
	actor->inventory.hotbar_slot = 0;
	/* Only the actor's Nether has a target. No invented Nether water. */
	if(boat) set_w(w, 4, 102, 6, BLOCK_SNOW, 0);
	else set_w(w, 4, 101, 5, BLOCK_RAIL, 0);
	struct server_rpc rpc = boat ? (struct server_rpc){.type = SRPC_USE_ITEM}
		: (struct server_rpc){.type = SRPC_BLOCK_PLACE,
			.payload.block_place = {.x = 4, .y = 101, .z = 5, .side = SIDE_TOP}};
	if(fail) server_local_test_fail_next_entity_alloc();
	spawns = 0;
	assert(send_actor(s, actor, &rpc));
	struct entity* e = find_vehicle(s, boat);
	if(fail) {
		check(!e && !spawns && actor->inventory.items[INVENTORY_SLOT_HOTBAR].count == 1,
			"allocation refusal: no entity/publication/consumption in either world");
	} else {
		check(e != NULL, "real item request spawns vehicle on actor's dry terrain");
		if(e) {
			check(server_local_entity_world(s, e) == w && e->world_slot == slot
				&& e->world_generation == w->generation,
				"vehicle belongs only to actor's live world slot/generation");
			check(spawns && spawn_slot == slot && spawn_generation == w->generation,
				"client spawn publication carries owning world stamp");
			check(client_entity(slot, e->id) && !client_entity(1u - slot, e->id),
				"client installs vehicle only in actor's world payload");
			check(actor->inventory.items[INVENTORY_SLOT_HOTBAR].count == 0,
				"successful placement consumes only acting inventory");
			if(boat) check(e->pos[1] == 102.0F,
				"snow lookup uses actor's world: Beta snow tile decrements spawn Y");
			else {
				e->data.minecart.speed = .1F;
				float z = e->pos[2];
				assert(server_local_test_tick_entities_w(s, w));
				check(e->pos[2] != z && e->pos[1] == 101.5F,
					"authoritative secondary-world tick samples owning rail and moves cart");
			}
			if(server_local_entity_world(s, e) == w)
				exercise(s, e, boat, type, guest);
		}
	}
	check(!memcmp(&partner_before, &partner->inventory, sizeof partner_before),
		"different-dimension partner inventory remains byte-identical");
	check(partner->x == partner_pose[0] && partner->y == partner_pose[1]
		&& partner->z == partner_pose[2] && partner->health == partner_health,
		"different-dimension partner pose and health remain unchanged");
	/* Release live entities before discarding their world. */
	struct chunk_entities saved = {0};
	assert(server_local_gather_chunk_entities_w(s, w, 0, 0, &saved));
	assert(server_local_try_release_chunk_entities_w(s, w, &saved));
	chunk_entities_free(&saved);
	server_world_discard(&s->worlds[1]);
	teardown(s);
}

int main(int argc, char** argv) {
	assert(argc == 5 || argc == 6);
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init(); items_init(); recipe_init();
	assert(clin_init() && svin_init());
	assert(clin_world_payloads_set_enabled(true));
	world_create(&gstate.world); dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);
	placement(atoi(argv[1]) != 0, (unsigned)atoi(argv[2]), atoi(argv[3]) != 0,
		atoi(argv[4]) != 0, argc == 6 ? (unsigned)atoi(argv[5]) : 1u);
	return harness_result("test_vehicle_world_routing", "placement");
}
