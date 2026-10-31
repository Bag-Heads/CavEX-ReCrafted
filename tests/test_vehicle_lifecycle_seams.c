/* G8-VEHICLE-SEAMS-01: mounted save/Leave/death/transfer boundaries.
 * Beta Entity.writeToNBT writes pose/motion/rotation, NOT ridingEntity or
 * riddenByEntity; EntityPig writes Saddle, EntityMinecart writes cargo.
 * Existing serializers and committed lifecycle handlers are the witnesses.
 * One positive case/process, never WILL_FAIL; all injection is labelled. */
#include "support/owner_swap_harness.h"
#include "entity/entity_boat.h"
#include "entity/entity_minecart.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "network/server_transfer.h"
#include "network/entity_archive.h"
#include "network/client_world_payload.h"
#include "game/difficulty.h"

static void settle_guest_presentation(struct server_local* s) {
	/* COMMIT is followed by a real streamed body-admission handshake. Do not
	 * seed a mount into that still-inert phase and mislabel a readiness failure
	 * as vehicle RED. No fabricated capacity or gameplay-success replies. */
	struct client_session* guest = &gstate.players[1];
	for(unsigned i = 0; i < 2000 && !guest->local_gameplay_ready; i++) {
		(void)client_session_request_local_gameplay(guest);
		pump_handshake(s);
		s->paused = false; tick(s); s->paused = true;
	}
	assert(guest->local_gameplay_ready && guest->local_player
		&& s->guest_actor->finished_loading);
	if(s->a2w_guest_presentation) {
		/* Echo the actual admitted body's correction token via its real RPC
		 * path, exactly as the existing owner-swap transfer fixture does. */
		step(s); /* a new logical update retires the admission's pose-reject latch */
		guest_sample(s, s->guest_actor->x, s->guest_actor->y, s->guest_actor->z,
			s->guest_actor->on_ground, guest->player_sync.correction_seq);
	}
	assert(!server_transfer_actor_inert(s, s->guest_actor));
}

static enum server_transfer_state drive_transfer(struct server_local* s, uint32_t id) {
	s->paused = false;
	for(unsigned i = 0; i < 4000; i++) {
		enum server_transfer_state state = server_transfer_state_of(s, id);
		if(state == SERVER_TRANSFER_COMMITTED || state == SERVER_TRANSFER_ROLLED_BACK) {
			s->paused = true; return state;
		}
		tick(s);
		client_acks_marker(s);
	}
	s->paused = true;
	return server_transfer_state_of(s, id);
}

static struct entity* create_vehicle(struct server_local* s,
	struct server_player* p, unsigned kind) {
	struct server_world* w = server_local_world_of(s, p); assert(w);
	uint32_t id = server_local_gen_id(s);
	struct entity* e = calloc(1, sizeof(*e)); assert(e);
	/* Fixture injection through real constructors + existing membership seam:
	 * explicit owner-world vehicle avoids pretending slot-0 spawn APIs can
	 * create a boat/cart in the secondary world. */
	if(kind == 0) entity_boat(id, e, true, w);
	else if(kind == 1) entity_monster(id, e, true, w, MONSTER_PIG);
	else entity_minecart(id, e, true, w);
	e->teleport(e, (vec3){(float)p->x + 1.0F,
		(float)p->y - PLAYER_EYE_HEIGHT, (float)p->z});
	w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(e->pos[0]));
	w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floorf(e->pos[2]));
	/* load_chunk reports a NEW load, not residency; admission may already
	 * own this column. Resolve that column before requesting a new load. */
	struct server_chunk* loaded = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
	if(!loaded) assert(server_world_load_chunk(w, cx, cz, &loaded));
	assert(loaded);
	*dict_entity_safe_get(s->entities, id) = e;
	assert(server_local_test_entity_join(s, id));
	if(kind == 1) e->data.monster.saddled = true;
	return e;
}

static void seed_mount(struct server_local* s, struct entity* e,
	struct server_player* p, unsigned kind) {
	struct actor_ident actor = server_local_actor_ident(s, p);
	uint32_t id = p == &s->player ? SERVER_LOCAL_PLAYER_ENTITY_ID : s->guest_entity_id;
	/* Boat/pig lifecycle witnesses start at production admission. Cart
	 * admission remains host-only; its existing guest relation is a fixture
	 * for release/lifetime regressions, not a new guest cart feature. */
	if(kind < 2) {
		clin_update_drain();
		struct server_rpc rpc = {.type = SRPC_ENTITY_USE,
			.payload.entity_use = {.entity_id = e->id, .desired_occupied = true}};
		if(p == &s->player) host_send(s, &rpc);
		else assert(guest_send(s, &rpc));
		assert(kind == 0 ? e->data.boat.occupied && e->data.boat.occupant_id == id
			&& actor_ident_equal(e->data.boat.rider_actor, actor)
			: e->data.monster.pig_ridden && e->data.monster.pig_rider_id == id
			&& actor_ident_equal(e->data.monster.pig_rider_actor, actor));
	} else {
		e->data.minecart.occupied = true;
		e->data.minecart.occupant_id = id;
		e->data.minecart.rider_actor = actor; /* S1 seam (Opus, disclosed) */
		e->data.minecart.ride_epoch = 91;
		e->data.minecart.pending_throttle = 2;
		p->riding_minecart = true;
	}
	p->rider_vel_x = p == &s->player ? 0 : .125F;
	p->rider_vel_z = p == &s->player ? 0 : -.25F;
}

static bool mounted(const struct entity* e, unsigned kind) {
	return kind == 0 ? e->data.boat.occupied
		: kind == 1 ? e->data.monster.pig_ridden : e->data.minecart.occupied;
}
static bool released(const struct entity* e, unsigned kind) {
	if(kind == 0) return !e->data.boat.occupied && !e->data.boat.occupant_id
		&& actor_ident_is_host(e->data.boat.rider_actor)
		&& e->data.boat.rider_vel_x == 0 && e->data.boat.rider_vel_z == 0;
	if(kind == 1) return !e->data.monster.pig_ridden && !e->data.monster.pig_rider_id
		&& actor_ident_is_host(e->data.monster.pig_rider_actor);
	return !e->data.minecart.occupied && !e->data.minecart.occupant_id
		&& !e->data.minecart.ride_epoch && !e->data.minecart.pending_throttle;
}
static void host_retained(struct server_local* s, struct entity* h) {
	check(s->player.riding_boat && h->data.boat.occupied
		&& h->data.boat.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID
		&& actor_ident_is_host(h->data.boat.rider_actor),
		"lifecycle: host stays mounted throughout the guest operation");
}

static struct server_local* fixture(bool separate) {
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	if(separate) {
		/* Real transaction BEFORE mount injection establishes the second world.
		 * No faked capacity replies or successful sends are used. */
		uint32_t id = 0;
		assert(server_transfer_request(s, s->guest_actor, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK);
		assert(drive_transfer(s, id) == SERVER_TRANSFER_COMMITTED);
		settle_guest_presentation(s);
		assert(server_local_world_of(s, s->guest_actor) != server_local_world_of(s, &s->player));
	}
	return s;
}

static void codec_roundtrip(const struct archived_entity* record, unsigned kind) {
	void* arena = NULL;
	struct nbt_list* built = chunk_entities_build(
		&(struct chunk_entities){.entities = (struct archived_entity*)record, .count = 1}, &arena);
	check(built && arena, "snapshot: actual vehicle NBT builder accepts the mounted record");
	if(!built || !arena) { chunk_entities_build_free(arena); return; }
	nbt_node list = {.type = TAG_LIST, .name = "Entities", .payload.tag_list = built};
	struct chunk_entities restored = {0};
	bool parsed = chunk_entities_parse(&list,
		WCOORD_CHUNK_OFFSET((w_coord_t)floorf(record->pos[0])),
		WCOORD_CHUNK_OFFSET((w_coord_t)floorf(record->pos[2])), &restored);
	check(parsed && restored.count == 1, "snapshot: actual vehicle NBT parser round trips one record");
	if(parsed && restored.count == 1) {
		const struct archived_entity* r = &restored.entities[0];
		check(r->kind == record->kind, "snapshot: vehicle kind survives NBT round trip");
		if(kind == 1) check(r->data.monster.saddled,
			"snapshot: actual NBT round trip preserves saddle on the pig");
		if(kind == 2) {
			check(!r->data.minecart.occupied && !r->data.minecart.occupant_source_id,
				"snapshot: serialized cart restores no runtime rider attachment");
			check(r->data.minecart.inventory[0].id == record->data.minecart.inventory[0].id
				&& r->data.minecart.inventory[0].count == record->data.minecart.inventory[0].count,
				"snapshot: actual cart NBT round trip preserves cargo");
		}
	}
	chunk_entities_free(&restored); chunk_entities_build_free(arena);
}
static void archive_vehicle(struct server_local* s, struct entity* e, unsigned kind) {
	struct archived_entity out = {0};
	server_local_test_archive_entity(e, &out);
	check(out.kind == (kind == 0 ? ARCHIVED_ENTITY_BOAT
		: kind == 1 ? ARCHIVED_ENTITY_MONSTER : ARCHIVED_ENTITY_MINECART),
		"snapshot: mounted entity uses the unchanged vehicle archive kind");
	if(kind == 1) check(out.data.monster.saddled,
		"snapshot: detached guest snapshot preserves the pig's saddle");
	codec_roundtrip(&out, kind);
	if(kind == 2) {
		/* Type-0 runtime occupancy is the seam under test; a separate type-1
		 * cart proves cargo serialization without claiming chest carts ride. */
		struct entity* cargo = create_vehicle(s, s->guest_actor, 2);
		cargo->data.minecart.minecart_type = 1;
		cargo->data.minecart.inventory[0] = (struct item_data){.id = ITEM_DIAMOND, .count = 7};
		server_local_test_archive_entity(cargo, &out);
		check(out.data.minecart.inventory[0].id == ITEM_DIAMOND
			&& out.data.minecart.inventory[0].count == 7,
			"snapshot: chest-cart cargo remains on the vehicle, never in player inventory");
		codec_roundtrip(&out, 2);
	}
}

static void case_save_leave(unsigned kind, bool separate, bool only_leave) {
	struct server_local* s = fixture(separate);
	struct entity* host = create_vehicle(s, &s->player, 0);
	struct entity* guest_mount = create_vehicle(s, s->guest_actor, kind);
	seed_mount(s, host, &s->player, 0);
	seed_mount(s, guest_mount, s->guest_actor, kind);
	struct server_player* g = s->guest_actor;
	struct server_rpc_source old = guest_source(s);
	g->inventory.items[INVENTORY_SLOT_MAIN] = (struct item_data){.id = ITEM_DIAMOND, .count = 13};
	g->health = 112;
	double x = g->x, y = g->y, z = g->z;
	enum world_dim dimension = g->dimension;
	if(!only_leave) {
		/* Real complete-world checkpoint. No direct replacement of serialized
		 * mount state or runtime actor record. Ordering needs an observable
		 * stage seam: see T01-result.md for the preimage ordering discrepancy. */
		bool saved = server_local_checkpoint_guests(s);
		check(saved, "save: ordinary complete-world checkpoint accepts a mounted guest");
		check(s->guest_actor == g && mounted(guest_mount, kind)
			&& g->x == x && g->y == y && g->z == z && g->health == 112,
			"save: detached SERIALIZABLE snapshot preserves the live authoritative mount/body");
		host_retained(s, host);
		archive_vehicle(s, guest_mount, kind);
		if(!saved) { server_local_worlds_discard_secondary(s); teardown(s); return; }
	}
	bool left = server_local_guest_leave(s, old);
	clin_update_drain();
	check(left && !s->guest_actor && !s->guest_generation,
		"Leave: committed checkpoint releases guest before generation/body invalidation");
	check(released(guest_mount, kind), "Leave: reciprocal vehicle relation/steering retires at the boundary");
	host_retained(s, host);
	if(left) {
		client_session_clear_windows(&gstate.players[1]);
		memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
		/* Actual disk reopen of level.dat, then existing saved-guest provider.
		 * This intentionally does not manufacture gameplay readiness. */
		level_archive_discard(&s->level);
		assert(level_archive_create(&s->level, s->level_name));
		struct server_rpc_source next;
		bool created = server_local_guest_create(s, &next);
		check(created, "reload: detached saved guest is readable from real disk");
		if(created) {
			struct server_player* p = s->guest_actor;
			check(next.generation > old.generation && !p->riding_boat && !p->riding_minecart
				&& !mounted(guest_mount, kind), "reload: new generation inherits no runtime attachment");
			/* Existing player archive API accepts vec3 (float); compare its
			 * defined storage representation, not unpersisted double low bits. */
			check(p->x == (double)(float)x && p->y == (double)(float)y && p->z == (double)(float)z && isfinite(p->x) && isfinite(p->y)
				&& isfinite(p->z) && p->dimension == dimension && p->health == 112
				&& p->inventory.items[INVENTORY_SLOT_MAIN].id == ITEM_DIAMOND
				&& p->inventory.items[INVENTORY_SLOT_MAIN].count == 13,
				"reload: finite owner-world pose, dimension, health and inventory survive");
			check(!server_local_guest_correct_position_for(s, old.generation, PLAYER_CORRECTION_POSITION_ONLY),
				"reload: departed generation cannot correct the replacement body");
		}
	}
	/* Secondary retirement requires no guest committed to that world. Release
	 * current provider first; this is teardown, not the Leave witness. */
	if(s->guest_actor) guest_release(s);
	server_local_worlds_discard_secondary(s);
	teardown(s);
}

struct publication_attempt {
	struct server_local* server;
	struct server_rpc_source source;
	uint32_t started, done;
	bool sent;
};
static void* publish_guest(void* user) {
	struct publication_attempt* attempt = user;
	__atomic_store_n(&attempt->started, 1, __ATOMIC_RELEASE);
	attempt->sent = server_local_guest_publish(attempt->server, attempt->source);
	__atomic_store_n(&attempt->done, 1, __ATOMIC_RELEASE);
	return NULL;
}
static void case_failure(unsigned kind, bool separate, bool allocation) {
	struct server_local* s = fixture(separate);
	struct entity* host = create_vehicle(s, &s->player, 0);
	struct entity* e = create_vehicle(s, s->guest_actor, kind);
	seed_mount(s, host, &s->player, 0); seed_mount(s, e, s->guest_actor, kind);
	struct server_rpc_source source = guest_source(s);
	struct server_player* g = s->guest_actor;
	double x = g->x, y = g->y, z = g->z;
	string_t path; string_init_set(path, s->level.file_name);
	if(allocation) {
		/* Existing entity-gather allocation seam is INSIDE the real chunk
		 * checkpoint, not a simulated failed send or guest record writer. */
		server_local_test_fail_next_entity_gather();
	} else string_set_str(s->level.file_name, test_fixture_join(split_path, "/absent/level.dat"));
	check(!server_local_guest_leave(s, source), "failure: real checkpoint failure refuses Leave");
	check(s->guest_actor == g && s->guest_generation == source.generation
		&& mounted(e, kind) && g->x == x && g->y == y && g->z == z,
		"failure: refused checkpoint retains usable mount, actor, pose and retry ownership");
	host_retained(s, host);
	string_set(s->level.file_name, path); string_clear(path);
	/* Actual token saturation, then private snapshot publication retry. */
	/* Accepted private envelope, stale ticket: real transport consumes tokens,
	 * while the real client rejects it without changing admission/body state. */
	struct client_rpc harmless = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.owner = 1, .generation = source.generation,
			.ticket = s->guest_ticket ^ UINT64_C(1), .result = LOCAL_GUEST_ATTACHED}};
	unsigned n = 0; while(clin_try_rpc_send_local(&harmless, 1, source.generation)) n++;
	assert(n > 0 && !clin_rpc_headroom(1));
	/* Preimage publication BLOCKS on a full inbox. Run the actual producer in
	 * a worker so that missing refusal is behavioral FAIL, not a deadlocked
	 * fixture. No consumer runs until the refusal observation is complete.
	 * The 100ms scheduling allowance is a liveness guard, not a perf claim. */
	struct publication_attempt attempt = {.server = s, .source = source};
	struct thread publisher;
	assert(thread_create(&publisher, publish_guest, &attempt, 64));
	ptime_t start = time_get();
	while(!__atomic_load_n(&attempt.started, __ATOMIC_ACQUIRE)) {
		assert(time_diff_s(start, time_get()) < 20); thread_msleep(1);
	}
	start = time_get();
	while(!__atomic_load_n(&attempt.done, __ATOMIC_ACQUIRE)
		&& time_diff_ms(start, time_get()) < 100) thread_msleep(1);
	bool refused = __atomic_load_n(&attempt.done, __ATOMIC_ACQUIRE) && !attempt.sent;
	check(refused, "failure: real full client inbox refuses guest snapshot publication without waiting for consumer");
	check(s->guest_actor == g && mounted(e, kind), "failure: refused publication leaves mount usable");
	/* Return actual tokens and join the real blocked producer before retry. */
	start = time_get();
	while(!__atomic_load_n(&attempt.done, __ATOMIC_ACQUIRE)) {
		clin_update_drain(); assert(time_diff_s(start, time_get()) < 20); thread_msleep(1);
	}
	thread_join(&publisher); clin_update_drain();
	check(server_local_guest_publish(s, source), "failure: private snapshot publication retries on actual released tokens");
	clin_update_drain();
	bool left = server_local_guest_leave(s, source); clin_update_drain();
	check(left && released(e, kind), "failure: successful retry settles precisely the guest mount");
	host_retained(s, host);
	if(s->guest_actor) guest_release(s);
	server_local_worlds_discard_secondary(s); teardown(s);
}

static void case_death(unsigned kind, bool separate) {
	struct server_local* s = fixture(separate);
	struct entity* host = create_vehicle(s, &s->player, 0);
	struct entity* e = create_vehicle(s, s->guest_actor, kind);
	seed_mount(s, host, &s->player, 0); seed_mount(s, e, s->guest_actor, kind);
	struct server_player* g = s->guest_actor;
	bool accepted = false;
	(void)server_local_damage_actor_result(s, g, MAX_PLAYER_HEALTH * 2, 0, 0, &accepted);
	clin_update_drain();
	check(accepted && g->health == 0, "death: production damage commits a real guest death");
	check(released(e, kind) && !g->riding_boat && !g->riding_minecart
		&& g->rider_vel_x == 0 && g->rider_vel_z == 0,
		"death: matching vehicle reciprocal relation/body/steering clears eagerly");
	host_retained(s, host);
	check(server_local_checkpoint_guests(s), "death: zero-health detached body can checkpoint in the death tick");
	/* Real respawn request. The headless rig has no mesher workers; use the
	 * existing backend readiness fixture used by test_local_guest rather than
	 * pretend rendering completed. The READY still traverses the real owned
	 * queue/handler, and its exact lifetime/epoch is production-generated. */
	struct server_rpc respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = s->guest_generation}};
	check(guest_send(s, &respawn), "respawn: dead guest request enters the owned queue");
	/* Cross-world death asks the REAL client for capacity first. Drive that
	 * exchange via full updates; never synthesize a successful capacity send. */
	for(unsigned i = 0; i < 2000 && s->guest_respawn_phase != GUEST_RESPAWN_LOADING; i++) {
		s->paused = false; tick(s); s->paused = true;
	}
	bool loading = s->guest_respawn_phase == GUEST_RESPAWN_LOADING;
	check(loading, "respawn: real planning/capacity exchange reaches LOADING");
	if(loading) {
		struct server_world* destination = server_local_guest_respawn_target(s); assert(destination);
		w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_x));
		w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_z));
		struct server_chunk* column = dict_server_chunks_get(destination->chunks, S_CHUNK_ID(cx, cz));
		if(!column) assert(server_world_load_chunk(destination, cx, cz, &column));
		assert(column);
		/* Fixture injection: publish the backend target-column readiness bit.
		 * This does not certify native/mesher/controller readiness. */
		column->sent = true;
		struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
			.payload.local_guest = {.ticket = s->guest_ticket,
				.action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
				.generation = s->guest_generation, .respawn_epoch = s->guest_respawn_epoch}};
		check(guest_send(s, &ready), "respawn: exact owned READY enters real transport");
		server_local_guest_respawn_poll(s); clin_update_drain();
	}
	check(g->health == MAX_PLAYER_HEALTH && !g->riding_boat && !g->riding_minecart
		&& !mounted(e, kind), "respawn: fresh detached body never reattaches to the dead mount");
	host_retained(s, host);
	guest_release(s); server_local_worlds_discard_secondary(s); teardown(s);
}

static void case_destroy(unsigned kind, bool separate) {
	assert(kind < 2);
	struct server_local* s = fixture(separate);
	struct entity* host = create_vehicle(s, &s->player, 0);
	struct entity* e = create_vehicle(s, s->guest_actor, kind);
	seed_mount(s, host, &s->player, 0); seed_mount(s, e, s->guest_actor, kind);
	uint32_t id = e->id;
	struct server_world* w = server_local_entity_world(s, e); assert(w);
	if(kind == 1) {
		/* Fixture selects a deterministic terminal hit and nonempty Beta
		 * nextInt(3) loot roll; damage, death, loot and release are production. */
		e->health = 1;
		struct java_random probe;
		int64_t seed = 0;
		for(;; seed++) {
			jrand_set_seed(&probe, seed);
			/* Fresh fatal hit: EntityLiving death pitch consumes two floats
			 * (:340), then the first corpse tick's living-sound roll (:104)
			 * precedes the drop roll. Select a silent schedule and two pork. */
			(void)jrand_next_float(&probe);
			(void)jrand_next_float(&probe);
			int voice = jrand_next_int_bound(&probe, 1000);
			if(voice >= e->data.monster.living_sound_time
			   && jrand_next_int_bound(&probe, 3) == 2) break;
		}
		jrand_set_seed(&e->gameplay_rng, seed);
	}
	struct server_rpc attack = {.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack = {.entity_id = id}};
	for(unsigned n = 0; n < (kind == 0 ? 5u : 1u); n++) assert(guest_send(s, &attack));
	if(kind == 1) {
		/* Pig death/loot begins on its first server tick after the fatal hit. */
		server_world_begin_update(w);
		assert(server_local_test_tick_entities_w(s, w)); clin_update_drain();
	}
	/* Inspect death-edge loot before its real pickup delay expires during
	 * the pig's 20-tick death animation; later pickup is not missing loot. */
	unsigned planks = 0, sticks = 0, pork = 0, drops = 0;
	dict_entity_it_t it; dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* drop = dict_entity_ref(it)->value;
		if(drop->type == ENTITY_ITEM) {
			drops++;
			check(server_local_entity_world(s, drop) == w,
				"destroy: each drop belongs to the destroyed mount's world");
			if(drop->data.item.item.id == BLOCK_PLANKS) planks += drop->data.item.item.count;
			if(drop->data.item.item.id == ITEM_STICK) sticks += drop->data.item.item.count;
			if(drop->data.item.item.id == ITEM_PORKCHOP) pork += drop->data.item.item.count;
		}
		dict_entity_next(it);
	}
	check(kind == 0 ? drops == 5 && planks == 3 && sticks == 2
		: drops == 1 && pork == 2,
		"destroy: Beta boat 3-planks/2-sticks or pig nextInt(3) loot is preserved");
	for(unsigned n = 0; n < 25 && dict_entity_get(s->entities, id); n++) {
		server_world_begin_update(w);
		assert(server_local_test_tick_entities_w(s, w)); clin_update_drain();
	}
	check(!dict_entity_get(s->entities, id), "destroy: real owned attack and entity ticks retire the admitted mount");
	check(!s->guest_actor->riding_boat && !s->guest_actor->riding_minecart
		&& !server_local_actor_vehicle(s, s->guest_actor),
		"destroy: no guest reciprocal ride survives destruction");
	host_retained(s, host);
	guest_release(s); server_local_worlds_discard_secondary(s); teardown(s);
}

static void case_host_respawn(unsigned kind, bool separate) {
	struct server_local* s = fixture(separate);
	struct entity* host = create_vehicle(s, &s->player, 0);
	struct entity* guest = create_vehicle(s, s->guest_actor, kind);
	seed_mount(s, host, &s->player, 0); seed_mount(s, guest, s->guest_actor, kind);
	struct server_respawn_plan plan;
	bool ready = server_local_prepare_respawn_plan(s, &plan);
	check(ready, "host respawn: real respawn plan prepares");
	if(ready) server_local_respawn_fresh_body(s, &plan);
	clin_update_drain();
	check(released(host, 0) && !s->player.riding_boat,
		"host respawn: host relation settles before fresh-body reset");
	check(mounted(guest, kind) && server_local_actor_resolve(s, guest_ident(s)) == s->guest_actor,
		"host respawn: guest lifetime and mount survive the host boundary");
	guest_release(s); server_local_worlds_discard_secondary(s); teardown(s);
}

static void case_transfer(unsigned kind, bool separate, bool refuse) {
	struct server_local* s = fixture(separate);
	struct entity* host = create_vehicle(s, &s->player, 0);
	struct entity* e = create_vehicle(s, s->guest_actor, kind);
	seed_mount(s, host, &s->player, 0); seed_mount(s, e, s->guest_actor, kind);
	struct server_player* g = s->guest_actor;
	if(kind == 1) {
		/* Measure refusal from the authoritative seat established by a real pig tick. */
		assert(e->tick_server && !e->tick_server(e, s));
		assert(isfinite(g->x) && isfinite(g->y) && isfinite(g->z));
	}
	enum world_dim original = g->dimension;
	enum world_dim destination = original == WORLD_DIM_OVERWORLD ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
	double x = g->x, y = g->y, z = g->z;
	uint32_t id = 0;
	if(refuse) server_transfer_test_fault(SERVER_TRANSFER_FAULT_TELEPORTER);
	enum server_transfer_rc rc = server_transfer_request(s, g, destination, &id);
	check(rc == SERVER_TRANSFER_OK, "transfer: existing transaction accepts an otherwise valid mounted actor");
	if(rc != SERVER_TRANSFER_OK) {
		server_transfer_test_fault(SERVER_TRANSFER_FAULT_NONE);
		guest_release(s); server_local_worlds_discard_secondary(s); teardown(s); return;
	}
	enum server_transfer_state result = drive_transfer(s, id);
	if(refuse) {
		/* Pig (kind 1): the rider legitimately follows its wandering mount, so
		 * the preserved pose is the live seat: one more real pig tick reseats
		 * the rider, then it must sit exactly on the pig. Boat/cart keep the
		 * exact pose (no server reseat). (Opus, T02b) */
		bool pose_kept = kind == 1
			? (!e->tick_server(e, s) && g->x == (double)e->pos[0]
			   && g->z == (double)e->pos[2] && isfinite(g->y))
			: (g->x == x && g->y == y && g->z == z);
		check(result == SERVER_TRANSFER_ROLLED_BACK && mounted(e, kind)
			&& g->dimension == original && pose_kept,
			"transfer refusal: actual PREPARE failure preserves source mount/pose and retryability");
		host_retained(s, host);
		server_transfer_test_fault(SERVER_TRANSFER_FAULT_NONE);
		id = 0; rc = server_transfer_request(s, g, destination, &id);
		check(rc == SERVER_TRANSFER_OK, "transfer refusal: same live actor can retry");
		if(rc == SERVER_TRANSFER_OK) result = drive_transfer(s, id);
	}
	check(result == SERVER_TRANSFER_COMMITTED && released(e, kind)
		&& !g->riding_boat && !g->riding_minecart && g->dimension == destination,
		"transfer: COMMIT settles source relation before installing destination membership");
	host_retained(s, host);
	if(result == SERVER_TRANSFER_COMMITTED) {
		settle_guest_presentation(s);
		id = 0; rc = server_transfer_request(s, g, original, &id);
		check(rc == SERVER_TRANSFER_OK, "round trip: detached guest can return");
		if(rc == SERVER_TRANSFER_OK) check(drive_transfer(s, id) == SERVER_TRANSFER_COMMITTED
			&& g->dimension == original && !mounted(e, kind),
			"round trip: returning does not resurrect source mount attachment");
		host_retained(s, host);
	}
	guest_release(s); server_local_worlds_discard_secondary(s); teardown(s);
}

static unsigned retry_installed, retry_ready;
static uint64_t retry_ticket, retry_generation, retry_epoch;
static void retry_consumed(const struct client_rpc* call, void* user) {
	consumed(call, user);
	if(call->type != CRPC_LOCAL_GUEST || call->payload.local_guest.ticket != retry_ticket
	   || call->payload.local_guest.generation != retry_generation) return;
	if(call->payload.local_guest.result == LOCAL_GUEST_INSTALLED) retry_installed++;
	if(call->payload.local_guest.result == LOCAL_GUEST_GAMEPLAY_READY
	   && call->payload.local_guest.respawn_epoch == retry_epoch) retry_ready++;
}
static void retry_pressure(struct server_local* s) {
	struct client_rpc filler = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.owner = 1, .generation = s->guest_generation,
			.ticket = s->guest_ticket ^ UINT64_C(1), .result = LOCAL_GUEST_ATTACHED}};
	unsigned n = 0;
	while(clin_try_rpc_send_local(&filler, 1, s->guest_generation)) n++;
	assert(n == 16 && !clin_rpc_headroom(1));
}
static void case_accept_host_transfer(void) {
	struct server_local* s = split_fixture();
	string_set_str(s->level_name, split_path);
	s->paused = true; s->split_view_radius = 4;
	stream_to_marker(s); acknowledge(s); s->thread_started = true; enable_split(s);
	struct client_session* guest = &gstate.players[1];
	assert(client_session_request_local_join(guest));
	pump_handshake(s); /* real OFFER binds the guest and schedules ACCEPT */
	assert(guest->local_phase == CLIENT_GUEST_INSTALLING && guest->local_pending);
	retry_ticket = s->guest_ticket; retry_generation = s->guest_generation;
	retry_installed = 0; clin_test_set_consume_observer(retry_consumed, NULL);
	struct server_world_handle world = server_world_handle_of(server_local_world_of(s, s->guest_actor));
	retry_pressure(s);
	client_session_flush_local(guest); /* client relinquishes retry on enqueue */
	assert(!guest->local_pending);
	server_local_test_process_messages(s);
	check(s->guest_accept_publish.pending && !s->guest_snapshot_sent && !retry_installed,
		"F5: full inbox retains a real ACCEPT after the client's successful enqueue");
	uint32_t id = 0;
	assert(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &id) == SERVER_TRANSFER_OK);
	assert(server_transfer_active(s) && s->transfer[0].open && !s->transfer[1].open);
	server_local_guest_join_capacity_tick(s);
	check(s->guest_accept_publish.pending && !retry_installed,
		"F5: unrelated open host transfer defers pending guest ACCEPT without cancellation");
	clin_update_drain();
	check(drive_transfer(s, id) == SERVER_TRANSFER_COMMITTED,
		"F5: unrelated host transfer reaches real COMMIT");
	for(unsigned i = 0; i < 2000 && !retry_installed; i++) {
		client_acks_marker(s); s->paused = false; tick(s); s->paused = true;
	}
	check(retry_installed == 1 && s->guest_snapshot_sent && !s->guest_accept_publish.pending,
		"F5: drain and active updates eventually deliver exactly one INSTALLED without re-asking ACCEPT");
	struct server_world_handle current = server_world_handle_of(server_local_world_of(s, s->guest_actor));
	check(s->guest_ticket == retry_ticket && s->guest_generation == retry_generation
		&& current.world_index == world.world_index && current.generation == world.generation,
		"F5: guest lifetime, ticket and committed world survive the host transfer");
	assert(guest->local_pending && guest->local_pending_action == LOCAL_GUEST_CONFIRM);
	pump_handshake(s); /* complete the existing CONFIRM, never re-ask ACCEPT */
	for(unsigned i = 0; i < 3; i++) { s->paused = false; tick(s); s->paused = true; }
	check(retry_installed == 1 && !guest->local_pending && s->guest_confirmed
		&& guest->local_phase == CLIENT_GUEST_ATTACHED,
		"F5: later server updates neither duplicate INSTALLED nor require client retry");
	guest_release(s); server_local_worlds_discard_secondary(s); teardown(s);
}

static void case_respawn_hazards(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* g = s->guest_actor;
	string_set_str(s->level_name, split_path);
	retry_ticket = s->guest_ticket; retry_generation = s->guest_generation;
	clin_test_set_consume_observer(retry_consumed, NULL);
	for(unsigned hazard = 0; hazard < 2; hazard++) {
		/* Dead-body seam injection; planning, READY, fresh-body commit and each
		 * environmental pass use production mailbox/update handlers. */
		g->health = 0; g->finished_loading = true; g->has_death_pos = true;
		g->death_x = g->x; g->death_y = g->y; g->death_z = g->z;
		struct server_rpc respawn = {.type = SRPC_RESPAWN,
			.payload.respawn = {.owner = 1, .generation = retry_generation}};
		assert(guest_send(s, &respawn));
		assert(s->guest_respawn_phase == GUEST_RESPAWN_LOADING);
		struct server_world* w = server_local_guest_respawn_target(s); assert(w);
		w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_x));
		w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_z));
		struct server_chunk* column = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
		if(!column) assert(server_world_load_chunk(w, cx, cz, &column));
		column->sent = true; /* existing backend target-column readiness seam */
		retry_epoch = s->guest_respawn_epoch; retry_ready = 0;
		retry_pressure(s);
		struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
			.payload.local_guest = {.owner = 1, .ticket = retry_ticket,
				.generation = retry_generation, .action = LOCAL_GUEST_RESPAWN_READY,
				.respawn_epoch = retry_epoch}};
		assert(svin_rpc_send_from(guest_source(s), &ready));
		server_local_test_process_messages(s);
		assert(s->guest_respawn_publish.pending && g->health == MAX_PLAYER_HEALTH && !retry_ready);
		/* Hold the already-refused completion for deterministic real updates:
		 * temporarily withhold snapshot validity, rather than racing a second
		 * producer against batch admission. Hazards and their tick gates stay on. */
		/* 8bc24398c (L214) ticks the guest's held map too; an out-of-range
		 * hotbar index is no longer a safe snapshot-refusal injection.
		 * Invalidation is an existing publication gate, with all slots valid. */
		bool invalidated = g->inventory.invalidated;
		g->inventory.invalidated = true;
		clin_update_drain();
		if(hazard == 0) g->fire = 20; /* the next live cadence applies burn damage */
		else g->y = VOID_KILL_Y - 1; /* deterministic world-independent void contact */
		for(unsigned i = 0; i < 3; i++) { s->paused = false; tick(s); s->paused = true; }
		check(s->guest_respawn_publish.pending && !retry_ready && !g->finished_loading
			&& g->health == MAX_PLAYER_HEALTH,
			hazard == 0 ? "W2: pending completion survives real fire-enabled updates without pre-READY damage"
			: "W2: pending completion survives real void-enabled updates without pre-READY damage");
		g->inventory.invalidated = invalidated;
		server_local_guest_respawn_poll(s); clin_update_drain();
		check(retry_ready == 1 && g->finished_loading && !s->guest_respawn_publish.pending
			&& s->guest_respawn_phase == GUEST_RESPAWN_IDLE && g->health == MAX_PLAYER_HEALTH,
			"W2: one queued READY releases the committed body without repeating fresh-body effects");
		g->hurt_resist_time = 0;
		s->paused = false; tick(s); s->paused = true;
		check(g->health < MAX_PLAYER_HEALTH,
			hazard == 0 ? "W2 control: same real update applies fire damage after READY is queued"
			: "W2 control: same real update applies void damage after READY is queued");
	}
	teardown(s);
}

/* N1: observe admission, not just client application. This callback only
 * records values; it never sends or consumes a token. */
static unsigned melee_health, melee_velocity, melee_order_count;
static int melee_order[16];
static void melee_enqueued(const struct client_rpc* call, void* unused) {
	(void)unused;
	if(call->type == CRPC_PLAYER_SET_HEALTH) melee_health++;
	if(call->type == CRPC_LOCAL_PLAYER_VELOCITY
	   && call->payload.local_player_velocity.owner == 1) melee_velocity++;
	int entry = call->type;
	if(call->type == CRPC_LOCAL_GUEST) {
		if(call->payload.local_guest.ticket != retry_ticket
		   || call->payload.local_guest.generation != retry_generation
		   || call->payload.local_guest.respawn_epoch != retry_epoch) return;
		if(call->payload.local_guest.result == LOCAL_GUEST_CORRECTION) entry = -1;
		else if(call->payload.local_guest.result == LOCAL_GUEST_GAMEPLAY_READY) entry = -2;
		else return;
	} else if(call->type != CRPC_PLAYER_SET_HEALTH && call->type != CRPC_WINDOW_ITEMS
		&& call->type != CRPC_PLAYER_HOTBAR_SLOT && call->type != CRPC_INVENTORY_SLOT) return;
	if(melee_order_count < sizeof(melee_order) / sizeof(*melee_order))
		melee_order[melee_order_count] = entry;
	melee_order_count++;
}

static bool (*melee_real_tick)(struct entity*, struct server_local*);
static unsigned melee_ticks, melee_guest_swings;
static bool melee_tick(struct entity* e, struct server_local* s) {
	int previous = e->data.monster.attack_time;
	bool removed = melee_real_tick(e, s);
	melee_ticks++;
	if(!removed && previous <= 1
	   && e->data.monster.attack_time == MOB_ATTACK_COOLDOWN_TICKS
	   && e->data.monster.target_actor.slot == ACTOR_SLOT_GUEST) melee_guest_swings++;
	return removed;
}

/* Keep five free tokens: the valid completion needs six. Ordinary world/mob
 * traffic can use the five, but no consumer runs during a successful update.
 * A watchdog drains only on a blocked regression so RED can report damage
 * instead of hanging. GREEN must never need that emergency consumer. */
static unsigned melee_emergency_drains;
static int melee_update_done;
static void* melee_watchdog(void* unused) {
	(void)unused;
	ptime_t start = time_get();
	while(!__atomic_load_n(&melee_update_done, __ATOMIC_ACQUIRE)) {
		if(time_diff_s(start, time_get()) > 2) {
			melee_emergency_drains++;
			while(!__atomic_load_n(&melee_update_done, __ATOMIC_ACQUIRE)) {
				clin_update_drain(); thread_msleep(1);
			}
			break;
		}
		thread_msleep(1);
	}
	return NULL;
}
static void melee_pressure_tick(struct server_local* s) {
	clin_update_drain();
	struct client_rpc filler = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.owner = 1, .generation = retry_generation,
			.ticket = retry_ticket ^ UINT64_C(1), .result = LOCAL_GUEST_ATTACHED}};
	for(unsigned i = 0; i < 11; i++)
		assert(clin_try_rpc_send_local(&filler, 1, retry_generation));
	assert(clin_rpc_headroom(5) && !clin_rpc_headroom(6));
	struct thread watchdog;
	__atomic_store_n(&melee_update_done, 0, __ATOMIC_RELEASE);
	assert(thread_create(&watchdog, melee_watchdog, NULL, 64));
	s->paused = false;
	server_local_test_update_once(s); /* archive stays on its owning thread */
	s->paused = true;
	__atomic_store_n(&melee_update_done, 1, __ATOMIC_RELEASE);
	thread_join(&watchdog);
}

static void case_respawn_melee(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* g = s->guest_actor;
	string_set_str(s->level_name, split_path);
	server_local_request_difficulty(DIFFICULTY_NORMAL);
	/* The saved guest default spawn is y=80; this superflat fixture's actual
	 * ground is y=4. Seed a grounded spawn before real death/respawn planning. */
	g->spawn_x = g->spawn_z = 0; g->spawn_y = 4;
	body_stand_at(&s->player, 10.5, 10.5);
	/* Fixture setup: suppress unrelated natural animals and settle streaming;
	 * the zombie's production targeting, movement and melee remain enabled. */
	s->render_demo_mob = MONSTER_ZOMBIE;
	for(unsigned i = 0; i < 120; i++) { s->paused = false; tick(s); s->paused = true; }
	assert(server_local_difficulty(s) == DIFFICULTY_NORMAL);
	bool accepted = false;
	(void)server_local_damage_actor_result(s, g, MAX_PLAYER_HEALTH * 2, 0, 0, &accepted);
	clin_update_drain();
	check(accepted && g->health == 0 && g->has_death_pos,
		"N1: production damage commits a real guest death");
	struct server_rpc respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = s->guest_generation}};
	assert(guest_send(s, &respawn) && s->guest_respawn_phase == GUEST_RESPAWN_LOADING);
	struct server_world* w = server_local_guest_respawn_target(s); assert(w);
	w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_x));
	w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_z));
	struct server_chunk* column = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
	if(!column) assert(server_world_load_chunk(w, cx, cz, &column));
	column->sent = true; /* existing backend readiness seam; no fabricated status */
	retry_ticket = s->guest_ticket; retry_generation = s->guest_generation;
	retry_epoch = s->guest_respawn_epoch; retry_ready = 0;
	clin_test_set_consume_observer(retry_consumed, NULL);
	retry_pressure(s);
	struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.owner = 1, .ticket = retry_ticket,
			.generation = retry_generation, .action = LOCAL_GUEST_RESPAWN_READY,
			.respawn_epoch = retry_epoch}};
	assert(svin_rpc_send_from(guest_source(s), &ready));
	server_local_test_process_messages(s);
	check(s->guest_respawn_publish.pending && !g->finished_loading
		&& g->health == MAX_PLAYER_HEALTH && !retry_ready,
		"N1: full inbox refuses completion after the real fresh-body commit");
	clin_update_drain();
	/* Worn armor is fixture state on the committed fresh body. The snapshot
	 * remains valid throughout; each update refuses exclusively on capacity. */
	struct item_data armor = {.id = ITEM_DIAMOND_CHESTPLATE, .count = 1, .durability = 0};
	const unsigned armor_slot = INVENTORY_SLOT_ARMOR + ARMOR_TYPE_CHESTPLATE;
	inventory_set_slot(&g->inventory, armor_slot, armor);
	int roof_x = (int)floor(g->x), roof_z = (int)floor(g->z);
	for(int x = roof_x - 3; x <= roof_x + 3; x++)
		for(int z = roof_z - 3; z <= roof_z + 3; z++) {
			put_block(s, x, 7, z, BLOCK_STONE, 0); /* shade without obstructing eye LOS */
			clin_update_drain();
		}
	struct entity* zombie = server_local_spawn_monster_at(
		(vec3){(float)g->x + 1.0F, (float)(g->y - PLAYER_EYE_HEIGHT), (float)g->z},
		MONSTER_ZOMBIE, s);
	assert(zombie && zombie->tick_server && zombie->added_to_chunk);
	melee_real_tick = zombie->tick_server; zombie->tick_server = melee_tick;
	clin_update_drain();
	clin_test_set_rpc_observer(melee_enqueued, NULL);
	for(unsigned i = 0; i < MOB_ATTACK_WINDUP_TICKS + 2 * MOB_ATTACK_COOLDOWN_TICKS + 2; i++) {
		if(s->guest_respawn_publish.pending) melee_pressure_tick(s);
		else { s->paused = false; tick(s); s->paused = true; }
	}
	struct item_data worn; inventory_get_slot(&g->inventory, armor_slot, &worn);
	printf("N1 pose guest=(%.3f,%.3f,%.3f) zombie=(%.3f,%.3f,%.3f) target=%u attack=%d windup=%d\n",
		g->x, g->y, g->z, zombie->pos[0], zombie->pos[1], zombie->pos[2],
		zombie->data.monster.target_actor.slot, zombie->data.monster.attack_time,
		zombie->data.monster.attack_windup);
	printf("N1 witness ticks=%u guest_swings=%u health=%d armor_wear=%u health_replies=%u velocity_replies=%u emergency_drains=%u\n",
		melee_ticks, melee_guest_swings, g->health, worn.durability, melee_health,
		melee_velocity, melee_emergency_drains);
	check(melee_ticks >= MOB_ATTACK_COOLDOWN_TICKS && melee_guest_swings >= 2,
		"N1: real Normal zombie attempts repeated melee through real server updates");
	check(s->guest_respawn_publish.pending && !g->finished_loading && !retry_ready
		&& g->health == MAX_PLAYER_HEALTH && worn.id == armor.id && worn.count == armor.count
		&& worn.durability == armor.durability,
		"N1: capacity-refused guest keeps MAX health and unworn armor before READY");
	check(!melee_health && !melee_velocity && !melee_order_count && !melee_emergency_drains,
		"N1: pending melee enqueues no health/velocity/snapshot reply and never blocks");
	clin_test_set_rpc_observer(NULL, NULL);
	clin_update_drain();
	/* Same live attacker, real damage entry, drained transport: host routing
	 * still applies while guest completion is pending. */
	int host_health = s->player.health;
	s->player.hurt_resist_time = 0;
	server_local_hurt_player_for(s, &s->player, zombie); clin_update_drain();
	check(s->player.health < host_health, "N1 control: the same zombie still damages the host");
	check(s->guest_respawn_publish.pending && !retry_ready,
		"N1: host damage does not admit the pending guest completion");
	melee_health = melee_velocity = melee_order_count = 0;
	clin_test_set_rpc_observer(melee_enqueued, NULL);
	server_local_guest_respawn_poll(s);
	const int ordered[] = {CRPC_PLAYER_SET_HEALTH, CRPC_WINDOW_ITEMS,
		CRPC_PLAYER_HOTBAR_SLOT, CRPC_INVENTORY_SLOT, -1, -2};
	check(melee_order_count == 6 && !memcmp(melee_order, ordered, sizeof(ordered)),
		"N1: drain admits health, inventory, correction, then READY in exact order");
	clin_update_drain();
	for(unsigned i = 0; i < 3; i++) { server_local_guest_respawn_poll(s); clin_update_drain(); }
	check(retry_ready == 1 && g->finished_loading && !s->guest_respawn_publish.pending,
		"N1: retry consumes exactly one matching epoch READY");
	clin_test_set_rpc_observer(NULL, NULL);
	for(unsigned i = 0; i < 2 * MOB_ATTACK_COOLDOWN_TICKS && g->health == MAX_PLAYER_HEALTH; i++) {
		s->paused = false; tick(s); s->paused = true;
	}
	inventory_get_slot(&g->inventory, armor_slot, &worn);
	check(g->health < MAX_PLAYER_HEALTH && worn.durability > armor.durability,
		"N1 control: the same zombie damages guest health and armor after READY");
	check(retry_ready == 1, "N1: later combat updates do not duplicate READY");
	teardown(s);
}

static void core_land(struct server_local* s, struct server_player* p) {
	/* Resolved six-block airborne displacement followed by a landing, through
	 * the production sampler (no fabricated damage amount or fall scalar). */
	p->y = 4.0 + PLAYER_EYE_HEIGHT; p->on_ground = false; p->fall_distance = 0;
	assert(server_local_player_vertical_sample_for(s, p, p->y + 6.0, -1.0F, false));
	assert(p->fall_distance == 6.0F);
	assert(server_local_player_vertical_sample_for(s, p, p->y, 0.0F, true));
	p->on_ground = true;
}
static void core_pressure_update(struct server_local* s, bool landing) {
	clin_update_drain();
	struct client_rpc filler = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.owner = 1, .generation = retry_generation,
			.ticket = retry_ticket ^ UINT64_C(1), .result = LOCAL_GUEST_ATTACHED}};
	for(unsigned i = 0; i < 11; i++)
		assert(clin_try_rpc_send_local(&filler, 1, retry_generation));
	assert(clin_rpc_headroom(5) && !clin_rpc_headroom(6));
	struct thread watchdog;
	__atomic_store_n(&melee_update_done, 0, __ATOMIC_RELEASE);
	assert(thread_create(&watchdog, melee_watchdog, NULL, 64));
	if(landing) core_land(s, s->guest_actor);
	s->paused = false; server_local_test_update_once(s); s->paused = true;
	__atomic_store_n(&melee_update_done, 1, __ATOMIC_RELEASE);
	thread_join(&watchdog);
}
static struct entity* core_arrow(struct server_local* s) {
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ARROW && e->data.arrow.from_skeleton) return e;
		dict_entity_next(it);
	}
	return NULL;
}
static void core_aim_arrow(struct entity* arrow, const struct server_player* p) {
	/* Deterministic impact trajectory of an actual AI-fired arrow: preserve
	 * its production shooter attribution, constructor and sweep/damage code. */
	arrow->teleport(arrow, (vec3){(float)p->x, 4.8F, (float)p->z - 1.5F});
	glm_vec3_copy((vec3){0, 0, 2}, arrow->vel);
	arrow->data.arrow.in_ground = false;
}
static struct entity* core_creeper(struct server_local* s, const struct server_player* target) {
	/* Vertical airburst leaves terrain/LOS intact; it still runs the complete
	 * creeper explosion and density/damage passes. No horizontal shove hides
	 * the health/armor guard behind unrelated position-correction traffic. */
	struct entity* e = server_local_spawn_monster_at(
		(vec3){(float)target->x, (float)target->y + 4.5F, (float)target->z}, MONSTER_CREEPER, s);
	assert(e && e->added_to_chunk);
	e->data.monster.fuse = CREEPER_FUSE_TICKS; /* existing terminal-fuse fixture seam */
	return e;
}
static void case_respawn_damage_core(unsigned kind) {
	const char* cause = kind == 0 ? "skeleton-arrow" : kind == 1 ? "creeper-explosion" : "fall-landing";
	struct server_local* s = admitted_fixture();
	struct server_player* g = s->guest_actor;
	string_set_str(s->level_name, split_path);
	server_local_request_difficulty(DIFFICULTY_NORMAL);
	g->spawn_x = g->spawn_z = 0; g->spawn_y = 4; /* grounded superflat spawn */
	body_stand_at(&s->player, 10.5, 10.5);
	s->render_demo_mob = MONSTER_ZOMBIE; /* only suppress unrelated natural animals */
	for(unsigned i = 0; i < 120; i++) { s->paused = false; tick(s); s->paused = true; }
	assert(server_local_difficulty(s) == DIFFICULTY_NORMAL);
	bool accepted = false;
	(void)server_local_damage_actor_result(s, g, MAX_PLAYER_HEALTH * 2, 0, 0, &accepted);
	clin_update_drain(); assert(accepted && !g->health && g->has_death_pos);
	struct server_rpc respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = s->guest_generation}};
	assert(guest_send(s, &respawn) && s->guest_respawn_phase == GUEST_RESPAWN_LOADING);
	struct server_world* w = server_local_guest_respawn_target(s); assert(w);
	struct server_chunk* column = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
	assert(column); column->sent = true; /* existing backend readiness seam */
	retry_ticket = s->guest_ticket; retry_generation = s->guest_generation;
	retry_epoch = s->guest_respawn_epoch; retry_ready = 0;
	clin_test_set_consume_observer(retry_consumed, NULL);
	retry_pressure(s);
	struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.owner = 1, .ticket = retry_ticket,
			.generation = retry_generation, .action = LOCAL_GUEST_RESPAWN_READY,
			.respawn_epoch = retry_epoch}};
	assert(svin_rpc_send_from(guest_source(s), &ready));
	server_local_test_process_messages(s);
	assert(s->guest_respawn_publish.pending && !g->finished_loading && g->health == MAX_PLAYER_HEALTH);
	clin_update_drain();
	const unsigned slot = INVENTORY_SLOT_ARMOR + ARMOR_TYPE_CHESTPLATE;
	struct item_data armor = {.id = ITEM_DIAMOND_CHESTPLATE, .count = 1};
	inventory_set_slot(&g->inventory, slot, armor);
	struct entity* arrow = NULL;
	uint32_t creeper_id = 0;
	if(kind == 0) {
		/* Actual skeleton AI shot and production arrow flush, with shade to
		 * avoid sun damage and deterministic trajectory only at impact. */
		for(int x = 4; x <= 7; x++) {
			put_block(s, x, 7, 0, BLOCK_STONE, 0); clin_update_drain();
		}
		assert(server_local_spawn_monster_at((vec3){5.5F, 4.0F, 0.5F}, MONSTER_SKELETON, s));
		clin_update_drain();
		core_pressure_update(s, false);
		arrow = core_arrow(s); assert(arrow && arrow->data.arrow.owner_id && !arrow->data.arrow.from_player);
		core_aim_arrow(arrow, g);
	} else if(kind == 1) { creeper_id = core_creeper(s, g)->id; clin_update_drain(); }
	clin_test_set_rpc_observer(melee_enqueued, NULL);
	core_pressure_update(s, kind == 2);
	struct item_data worn; inventory_get_slot(&g->inventory, slot, &worn);
	if(kind == 0) {
		check(core_arrow(s) == arrow && arrow->pos[2] > (float)g->z,
			"R02c arrow: real skeleton-fired projectile sweeps across the unfinished guest body");
		/* arrow_test_actor already excludes unfinished bodies before damage_core;
		 * the core-only RED must not be misreported as an arrow regression. */
	} else if(kind == 1) {
		struct server_world_explosion_cost cost; server_world_explosion_last_cost(&cost);
		check(!dict_entity_get(s->entities, creeper_id) && cost.entities >= 1,
			"R02c explosion: real creeper tick detonates and reaches the guest victim pass");
	} else check(g->fall_distance == 0, "R02c fall: production landing bills and clears real sampled displacement");
	printf("R02c %s pending health=%d armor_wear=%u health_replies=%u velocity_replies=%u private_replies=%u emergency_drains=%u\n",
		cause, g->health, worn.durability, melee_health, melee_velocity, melee_order_count, melee_emergency_drains);
	check(s->guest_respawn_publish.pending && !g->finished_loading && !retry_ready
		&& g->health == MAX_PLAYER_HEALTH && worn.id == armor.id && worn.count == armor.count
		&& worn.durability == armor.durability && !g->hurt_resist_time && !g->last_damage,
		"R02c: pending guest health, armor and damage bookkeeping stay unchanged");
	check(!melee_health && !melee_velocity && !melee_order_count && !melee_emergency_drains,
		"R02c: capacity-refused hit enqueues no private damage/snapshot reply and never blocks");
	clin_test_set_rpc_observer(NULL, NULL); clin_update_drain();
	/* Host control while completion still waits; use the same real caller. */
	int host_health = s->player.health; s->player.hurt_resist_time = 0;
	if(kind == 0) {
		struct entity probe = *arrow; core_aim_arrow(&probe, &s->player);
		check(probe.tick_server(&probe, s), "R02c host: skeleton arrow sweep accepts the host impact");
	} else if(kind == 1) {
		struct entity* host_creeper = core_creeper(s, &s->player); clin_update_drain();
		server_world_begin_update(w);
		check(host_creeper->tick_server(host_creeper, s), "R02c host: real creeper tick detonates at the host");
	} else core_land(s, &s->player);
	clin_update_drain();
	check(s->player.health < host_health && s->guest_respawn_publish.pending && !retry_ready,
		"R02c host: damage still applies while guest completion remains pending");
	melee_health = melee_velocity = melee_order_count = 0;
	clin_test_set_rpc_observer(melee_enqueued, NULL);
	server_local_guest_respawn_poll(s);
	const int ordered[] = {CRPC_PLAYER_SET_HEALTH, CRPC_WINDOW_ITEMS,
		CRPC_PLAYER_HOTBAR_SLOT, CRPC_INVENTORY_SLOT, -1, -2};
	check(melee_order_count == 6 && !memcmp(melee_order, ordered, sizeof(ordered)),
		"R02c: drain queues health, inventory, correction, then READY in exact order");
	clin_update_drain();
	for(unsigned i = 0; i < 3; i++) { server_local_guest_respawn_poll(s); clin_update_drain(); }
	check(retry_ready == 1 && g->finished_loading && !s->guest_respawn_publish.pending,
		"R02c: exactly one matching READY finishes the pending completion");
	clin_test_set_rpc_observer(NULL, NULL);
	int before = g->health; inventory_get_slot(&g->inventory, slot, &armor);
	g->hurt_resist_time = g->last_damage = 0; g->vel_x = g->vel_z = 0;
	g->y = 4.0 + PLAYER_EYE_HEIGHT;
	if(kind == 0) core_aim_arrow(arrow, g);
	else if(kind == 1) { core_creeper(s, g); clin_update_drain(); }
	else core_land(s, g);
	s->paused = false; tick(s); s->paused = true;
	inventory_get_slot(&g->inventory, slot, &worn);
	printf("R02c %s after_READY health=%d->%d armor_wear=%u->%u\n", cause, before, g->health, armor.durability, worn.durability);
	check(g->health < before && worn.durability > armor.durability,
		"R02c control: the same production cause damages guest health and armor after READY");
	check(retry_ready == 1, "R02c: later damage does not duplicate READY");
	teardown(s);
}

static void init(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init(); items_init(); recipe_init(); assert(clin_init() && svin_init());
	assert(clin_world_payloads_set_enabled(true));
	world_create(&gstate.world); dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);
}
int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "save";
	unsigned kind = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
	bool separate = argc > 3 && atoi(argv[3]) != 0;
	if(kind > 2) return 2;
	init();
	if(!strcmp(which, "save") || !strcmp(which, "leave")) case_save_leave(kind, separate, !strcmp(which, "leave"));
	else if(!strcmp(which, "checkpoint-failure") || !strcmp(which, "allocation-failure"))
		case_failure(kind, separate, !strcmp(which, "allocation-failure"));
	else if(!strcmp(which, "death")) case_death(kind, separate);
	else if(!strcmp(which, "destroy") && kind < 2) case_destroy(kind, separate);
	else if(!strcmp(which, "host-respawn")) case_host_respawn(kind, separate);
	else if(!strcmp(which, "transfer") || !strcmp(which, "transfer-refusal"))
		case_transfer(kind, separate, !strcmp(which, "transfer-refusal"));
	else if(!strcmp(which, "accept-host-transfer")) case_accept_host_transfer();
	else if(!strcmp(which, "respawn-hazards")) case_respawn_hazards();
	else if(!strcmp(which, "respawn-melee")) case_respawn_melee();
	else if(!strcmp(which, "respawn-damage-core")) case_respawn_damage_core(kind);
	else { fprintf(stderr, "unknown vehicle lifecycle case: %s\n", which); return 2; }
	return harness_result("test_vehicle_lifecycle_seams", which);
}
