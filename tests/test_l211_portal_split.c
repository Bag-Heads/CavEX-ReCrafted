/* L211: production portal contact and split ownership regressions.
 * RetroMCP Beta server BlockPortal.java:124-129 gates both relation directions;
 * Entity.java:913-914 is a no-op, EntityPlayer.java:750-756 arms player contact.
 * EntityPlayerSP.java:41-77 dismounts then ramps for the SP host;
 * EntityPlayerMP.java:152-181 holds dwell for the local MP guest.
 * EntityWolf.java:304 stores the interacting player's username; :329 checks it.
 * Terrain, committed slots and closed guest mounts are labelled fixture setup.
 * Contact/dwell, use requests, publications and entity archives are production.
 */
#include "support/owner_swap_harness.h"
#include "entity/entity_boat.h"
#include "entity/entity_minecart.h"
#include "entity/entity_wolf.h"
#include "network/entity_archive.h"
#include "network/client_world_payload.h"
#include "network/server_transfer.h"
#include "platform/audio.h"

static unsigned splash_count, splash_slot, trigger_count;
extern size_t server_local_test_vehicle_walks(bool arm);
static uint32_t splash_generation;
static void observe(const struct client_rpc* rpc, void* unused) {
	consumed(rpc, unused);
	if(rpc->type == CRPC_SOUND && rpc->payload.sound.sound_id == SOUND_PORTAL_TRIGGER)
		trigger_count++;
	if(rpc->type == CRPC_SOUND && rpc->payload.sound.sound_id == SOUND_RANDOM_SPLASH) {
		splash_count++;
		splash_slot = rpc->world_index;
		splash_generation = rpc->world_generation;
	}
}

static void cell(struct server_world* w, int x, int y, int z, uint8_t type) {
	(void)server_world_set_block_silent(w, x, y, z,
		(struct block_data){.type = type});
	struct block_data b;
	bool read = server_world_get_block(w, x, y, z, &b);
	if(!read || b.type != type)
		fprintf(stderr, "fixture cell failed slot=%u xyz=%d,%d,%d read=%d id=%u wanted=%u\n",
			w->world_index, x, y, z, read, read ? b.type : 255, type);
	assert(read && b.type == type);
}

static struct server_local* fixture(bool host_nether) {
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
	assert(clin_world_payload_prepare_landing(1, s->worlds[1].generation, WORLD_DIM_NETHER));
	assert(clin_world_payload_bind_view(0, host_nether ? 1 : 0));
	assert(clin_world_payload_bind_view(1, host_nether ? 0 : 1));
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	/* Retire admission's outer single-world ring through the real save path
	 * before loading the two-world footprint; do not bypass residency caps. */
	for(int x = -20; x <= 20; x++) for(int z = -20; z <= 20; z++)
		if((abs(x) > 1 || abs(z) > 1)
		   && server_world_is_chunk_loaded(&s->worlds[0], x, z))
			assert(server_world_save_chunk(&s->worlds[0], true, x, z));
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	/* No mesher runs in this fixture; release the old streamed terrain refs. */
	world_destroy(&gstate.world); world_create(&gstate.world);
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	for(unsigned slot = 0; slot < 2; slot++) {
		struct server_world* w = &s->worlds[slot];
		struct server_chunk* c = NULL;
		for(int x = -2; x <= 2; x++) for(int z = -2; z <= 2; z++) {
			server_world_begin_update(w);
			if(!server_world_is_chunk_loaded(w, x, z))
				assert(server_world_load_chunk(w, x, z, &c));
		}
		for(int x = 0; x < 16; x++) for(int z = 0; z < 16; z++) {
			struct server_chunk* home = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
			assert(home);
			/* Raw terrain is fixture setup; emitting thousands of SET_BLOCKs
			 * here would overwhelm the bounded gameplay publication queue. */
			home->ids[100 + (z + x * CHUNK_SIZE) * WORLD_HEIGHT] = BLOCK_STONE;
			for(int y = 101; y < 107; y++)
				home->ids[y + (z + x * CHUNK_SIZE) * WORLD_HEIGHT] = BLOCK_AIR;
			server_world_chunk_touch(home);
		}
		assert(server_world_light_drain(w));
	}
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	s->player.world_slot = host_nether ? 1 : 0;
	s->guest_actor->world_slot = host_nether ? 0 : 1;
	struct server_player* actors[] = {&s->player, s->guest_actor};
	for(unsigned i = 0; i < 2; i++) {
		actors[i]->dimension = s->worlds[actors[i]->world_slot].dimension;
		actors[i]->x = actors[i]->z = 8.5;
		actors[i]->y = 101.0 + PLAYER_EYE_HEIGHT;
		actors[i]->on_ground = true;
		actors[i]->time_until_portal = 0;
		actors[i]->time_in_portal = 0;
		actors[i]->in_portal = actors[i]->portal_travel_requested = false;
	}
	clin_test_set_consume_observer(observe, NULL);
	server_transfer_stream_enable(s, 1);
	return s;
}

static struct entity* prop(struct server_local* s, struct server_player* p, unsigned kind) {
	struct server_world* w = server_local_world_of(s, p);
	struct entity* e = calloc(1, sizeof(*e)); assert(e);
	uint32_t id = server_local_gen_id(s);
	if(kind == 0) entity_boat(id, e, true, w);
	else if(kind == 1) entity_monster(id, e, true, w, MONSTER_PIG);
	else if(kind == 2) entity_minecart(id, e, true, w);
	else entity_monster(id, e, true, w, MONSTER_WOLF);
	e->teleport(e, (vec3){(float)p->x, 101.0F, (float)p->z});
	*dict_entity_safe_get(s->entities, id) = e;
	assert(server_local_test_entity_join(s, id));
	if(kind == 1) e->data.monster.saddled = true;
	return e;
}

static void mount(struct server_local* s, struct server_player* p, struct entity* e, unsigned kind) {
	/* Fixture relation injection: guest-positive ingress remains closed. */
	struct actor_ident actor = server_local_actor_ident(s, p);
	uint32_t id = server_local_actor_wire_id(s, p);
	if(kind == 0) server_local_set_boat_occupancy_for(s, e, p, true);
	else if(kind == 1) {
		e->data.monster.pig_ridden = true;
		e->data.monster.pig_rider_id = id;
		e->data.monster.pig_rider_actor = actor;
	} else {
		e->data.minecart.occupied = true;
		e->data.minecart.occupant_id = id;
		e->data.minecart.rider_actor = actor;
		e->data.minecart.ride_epoch = 91;
		p->riding_minecart = true;
	}
	clin_update_drain();
	assert(server_local_actor_vehicle(s, p) == e);
}

static void cleanup(struct server_local* s) {
	server_local_release_actor_mounts(s, &s->player, NULL);
	server_local_release_actor_mounts(s, s->guest_actor, NULL);
	guest_release(s);
	server_local_worlds_discard_secondary(s);
	teardown(s);
}

static void settle_actor(struct server_local* s, struct server_player* p) {
	for(unsigned i = 0; i < 2000; i++) {
		if(p == s->guest_actor) {
			(void)client_session_request_local_gameplay(&gstate.players[1]);
			pump_handshake(s);
		}
		if(p->finished_loading && !server_transfer_actor_inert(s, p))
			return;
		s->paused = false; tick(s); s->paused = true;
		client_acks_marker(s);
		if(p == s->guest_actor && gstate.players[1].local_gameplay_ready) {
			guest_sample(s, p->x, p->y, p->z, p->on_ground,
				gstate.players[1].player_sync.correction_seq);
		}
	}
	assert(!"production transfer presentation did not settle");
}

static void drive_natural_travel(struct server_local* s, struct server_player* p,
	struct server_world* source) {
	for(unsigned i = 0; i < 4000 && server_local_world_of(s, p) == source; i++) {
		s->paused = false; tick(s); s->paused = true;
		client_acks_marker(s);
	}
}

static void portal_case(unsigned kind, bool guest, bool nether) {
	struct server_local* s = fixture(guest != nether);
	struct server_player* p = guest ? s->guest_actor : &s->player;
	struct server_world* w = server_local_world_of(s, p);
	struct entity* e = prop(s, p, kind);
	uint32_t mount_id = e->id;
	enum world_dim source_dim = w->dimension;
	/* A genuine AABB overlap: the eye is above the portal cell. */
	/* Portal volume is fixture placement, broad enough for the real pig seat
	 * and its wandering movement; every tick still uses production AABB scan.
	 * Raw fixture ids avoid unframed-block callbacks and client FX queue limits. */
	struct server_chunk* home = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
	assert(home);
	for(int x = 0; x < 16; x++) for(int z = 0; z < 16; z++)
		for(int y = 101; y < 105; y++)
			home->ids[y + (z + x * CHUNK_SIZE) * WORLD_HEIGHT] = BLOCK_PORTAL;
	/* Confine the wandering pig so all 100 ticks retain positive contact. */
	for(int x = 0; x < 16; x++) for(int z = 0; z < 16; z++)
		if(x == 0 || x == 15 || z == 0 || z == 15)
			for(int y = 101; y < 105; y++)
				home->ids[y + (z + x * CHUNK_SIZE) * WORLD_HEIGHT] = BLOCK_STONE;
	mount(s, p, e, kind);
	s->paused = false; tick(s); s->paused = true;
	check(!p->in_portal, "mounted AABB portal overlap never newly arms contact");
	if(p->in_portal) {
		p->time_in_portal = .25F;
		s->paused = false; tick(s); s->paused = true;
		check(!server_local_actor_vehicle(s, p) && p->time_in_portal == (guest ? .25F : .25F + .0125F),
			"prior-armed contact dismounts with SP host ramp / MP guest hold");
		cleanup(s); return; /* RED: avoid the omitted gate's faulty transfer */
	}
	bool blocked = true, contacting = true;
	for(unsigned i = 0; i < 100; i++) {
		s->paused = false; tick(s); s->paused = true;
		blocked &= !p->in_portal && p->time_in_portal == 0.0F
			&& !p->portal_travel_requested && !server_transfer_actor_inert(s, p)
			&& server_local_world_of(s, p) == w && server_local_actor_vehicle(s, p) == e;
		struct block_data contact;
		contacting &= server_world_get_block(w, (w_coord_t)floor(p->x),
			(w_coord_t)floor(p->y - PLAYER_EYE_HEIGHT), (w_coord_t)floor(p->z), &contact)
			&& contact.type == BLOCK_PORTAL;
	}
	check(contacting, "positive portal-cell overlap observed on every mounted tick");
	check(blocked, "over 100 mounted production ticks: no dwell, transfer, or contact dismount");
	p->time_until_portal = 10;
	for(unsigned i = 0; i < 12; i++) {
		s->paused = false; tick(s); s->paused = true;
	}
	check(!p->in_portal && p->time_until_portal == 0,
		"mounted contact cannot refresh even the arrival cooldown");
	/* SP host ramps after dismount; MP guest holds. Both decrement cooldown. */
	p->in_portal = true; p->time_in_portal = .25F; p->time_until_portal = 7;
	s->paused = false; tick(s); s->paused = true;
	check(!server_local_actor_vehicle(s, p) && p->time_in_portal == (guest ? .25F : .25F + .0125F),
		"prior-armed contact dismounts with SP host ramp / MP guest hold");
	check(p->time_until_portal == 10,
		"dismount consume decrements cooldown to 6, then unmounted portal scan pins it to 10");
	/* Direct consume isolates the decrement from the subsequent contact pin. */
	mount(s, p, e, kind);
	p->in_portal = true; p->time_until_portal = 7;
	server_local_actor_portal_tick(s, p);
	check(p->time_until_portal == 6, "prior-armed dismount decrements positive cooldown exactly once");
	/* Fresh dismounted natural contact re-arms in the same production scan. */
	p->x = p->z = 8.5; p->y = 101.0 + PLAYER_EYE_HEIGHT;
	p->time_in_portal = 0; p->time_until_portal = 0; p->in_portal = false;
	s->paused = false; tick(s); s->paused = true;
	check(p->in_portal, "deliberately dismounted player arms natural AABB contact");
	for(unsigned i = 0; i < 80; i++) {
		s->paused = false; tick(s); s->paused = true;
	}
	check(server_local_world_of(s, p) == w && p->time_in_portal < 1.0F,
		"Beta float dwell has not transferred before its 81st increment");
	drive_natural_travel(s, p, w);
	check(server_local_world_of(s, p) != w && p->dimension != w->dimension,
		"dismounted natural dwell commits the ordinary player transfer");
	struct chunk_entities saved = {0};
	assert(server_local_gather_chunk_entities_w(s, w, 0, 0, &saved));
	/* A vacant source world evicts its columns normally. Read the persisted
	 * owner rather than interpreting an empty live dictionary as lost state. */
	if(!saved.count && !server_world_is_chunk_loaded(w, 0, 0)) {
		chunk_entities_free(&saved);
		struct region_archive* archive = server_world_chunk_region(w, 0, 0);
		assert(archive);
		struct server_chunk disk = {0};
		assert(region_archive_get_chunk(archive, 0, 0, &disk, NULL, &saved));
		server_chunk_arrays_free(&disk);
	}
	unsigned source = 0;
	for(size_t i = 0; i < saved.count; i++) if(saved.entities[i].source_id == mount_id) source++;
	check(source == 1, "source mount remains exactly once in its original world archive");
	chunk_entities_free(&saved);
	settle_actor(s, p);
	struct server_world* destination = server_local_world_of(s, p);
	unsigned duplicates = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* candidate = dict_entity_cref(it)->value;
		bool same_kind = kind == 0 ? candidate->type == ENTITY_BOAT
			: kind == 2 ? candidate->type == ENTITY_MINECART
			: candidate->type == ENTITY_MONSTER && candidate->data.monster.id == MONSTER_PIG
				&& candidate->data.monster.saddled;
		/* The fixture mount is saddled. Ordinary destination pigs can spawn
		 * during real travel; they are not copies of that mount. Boats/carts
		 * have no natural spawner, so their kind alone identifies a copy. */
		if(same_kind && server_local_entity_world(s, candidate) == destination)
			duplicates++;
		dict_entity_next(it);
	}
	check(!duplicates, "destination has no spawned copy of the source mount");
	double arrival[] = {p->x, p->y, p->z};
	for(unsigned i = 0; i < 24; i++) {
		s->paused = false; tick(s); s->paused = true;
	}
	check(server_local_world_of(s, p) == destination && p->time_until_portal > 0,
		"dismounted arrival contact pins cooldown and cannot immediately return");
	/* Fixture movement off the portal: clear only the standing AABB at x+3. */
	p->x += 3;
	w_coord_t lx = (w_coord_t)floor(p->x), lz = (w_coord_t)floor(p->z);
	w_coord_t ly = (w_coord_t)floor(p->y - PLAYER_EYE_HEIGHT);
	struct server_chunk* landing = dict_server_chunks_get(destination->chunks,
		S_CHUNK_ID(WCOORD_CHUNK_OFFSET(lx), WCOORD_CHUNK_OFFSET(lz)));
	assert(landing);
	for(int y = ly; y < ly + 3; y++)
		landing->ids[y + (W2C_COORD(lz) + W2C_COORD(lx) * CHUNK_SIZE) * WORLD_HEIGHT] = BLOCK_AIR;
	for(unsigned i = 0; i < 12; i++) {
		s->paused = false; tick(s); s->paused = true;
	}
	check(!p->in_portal && p->time_until_portal == 0,
		"leaving the AABB portal contact releases its cooldown pin");
	p->x = arrival[0]; p->y = arrival[1]; p->z = arrival[2];
	drive_natural_travel(s, p, destination);
	check(p->dimension == source_dim && server_local_world_of(s, p) != destination,
		"natural dismounted recontact returns through ordinary dwell");
	check(!server_local_actor_vehicle(s, p), "return never reattaches the retained source mount");
	cleanup(s);
}


static void armed_case(bool nether) {
	struct server_local* s = fixture(nether);
	struct server_player* p = &s->player;
	struct entity* e = prop(s, p, 2);
	mount(s, p, e, 2);
	p->in_portal = true; p->time_in_portal = 0; p->time_until_portal = 7;
	trigger_count = 0;
	s->paused = false; tick(s); s->paused = true;
	check(!server_local_actor_vehicle(s, p), "SP armed-then-mounted host dismounts");
	check(p->time_in_portal == .0125F && p->time_until_portal == 6 && !p->in_portal,
		"EntityPlayerSP:41-77 ramps on the dismount tick and decrements cooldown");
	check(trigger_count == 1, "SP armed-then-mounted host plays portal.trigger exactly once");
	s->paused = false; tick(s); s->paused = true;
	check(trigger_count == 1 && p->time_in_portal == 0,
		"off-portal next tick decays without another trigger");
	cleanup(s);
}

static void walks_case(bool nether) {
	struct server_local* s = fixture(nether);
	for(unsigned k = 0; k < 4; k++) prop(s, &s->player, k);
	server_local_test_vehicle_walks(true);
	for(unsigned i = 0; i < 10; i++) {
		s->paused = false; tick(s); s->paused = true;
		check(!s->player.in_portal && !s->guest_actor->in_portal,
			"no portal cells: neither actor arms contact");
	}
	size_t walks = server_local_test_vehicle_walks(false);
	printf("vehicle dictionary walks / 10 portal-free two-actor ticks: %zu\n", walks);
	check(walks == 0, "T1: portal-free scans eliminate the baseline 20 dictionary walks");
	cleanup(s);
}

/* M2 equivalence tape: fixed-width portal outputs, no pointers or padding.
 * Run this identical fixture against the pre/post scan order and compare bytes. */
static void scan_case(bool nether) {
	struct server_local* s = fixture(nether);
	struct server_player* actors[] = {&s->player, s->guest_actor};
	struct entity* carts[] = {prop(s, actors[0], 2), prop(s, actors[1], 2)};
	const char* trace_path = getenv("L211_PORTAL_TRACE");
	FILE* trace = trace_path ? fopen(trace_path, "wb") : NULL;
	assert(!trace_path || trace);
	for(unsigned contact = 0; contact < 2; contact++)
		for(unsigned mounted = 0; mounted < 2; mounted++)
			for(unsigned cooldown = 0; cooldown < 2; cooldown++) {
				for(unsigned actor = 0; actor < 2; actor++) {
					struct server_player* p = actors[actor];
					server_local_release_actor_mounts(s, p, NULL);
					p->x = p->z = 8.5; p->y = 101.0 + PLAYER_EYE_HEIGHT;
					carts[actor]->teleport(carts[actor], (vec3){8.5F, 101.0F, 8.5F});
					struct server_world* w = server_local_world_of(s, p);
					struct server_chunk* home = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
					assert(home);
					for(int x = 1; x < 15; x++) for(int z = 1; z < 15; z++)
						for(int y = 101; y < 105; y++)
							home->ids[y + (z + x * CHUNK_SIZE) * WORLD_HEIGHT]
								= contact ? BLOCK_PORTAL : BLOCK_AIR;
					if(mounted) mount(s, p, carts[actor], 2);
					p->time_until_portal = cooldown ? 7 : 0;
					p->time_in_portal = 0; p->in_portal = p->portal_travel_requested = false;
				}
				float dwell = 0;
				for(unsigned i = 0; i < 4; i++) {
					s->paused = false; tick(s); s->paused = true;
					if(i && contact && !mounted && !cooldown) dwell += .0125F;
					for(unsigned actor = 0; actor < 2; actor++) {
						struct server_player* p = actors[actor];
						int expected_cooldown = cooldown
							? (contact && !mounted ? 10 : 6 - (int)i) : 0;
						check(p->in_portal == (contact && !mounted && !cooldown)
							&& p->time_in_portal == dwell
							&& p->time_until_portal == expected_cooldown
							&& !p->portal_travel_requested,
							"scan matrix: exact contact/dwell/cooldown/travel outputs");
						uint32_t bits;
						memcpy(&bits, &p->time_in_portal, sizeof bits);
						uint32_t cd = p->time_until_portal;
						unsigned char record[12];
						for(unsigned b = 0; b < 4; b++) {
							record[b] = (unsigned char)(bits >> (8 * b));
							record[4 + b] = (unsigned char)(cd >> (8 * b));
						}
						record[8] = p->in_portal; record[9] = p->portal_travel_requested;
						record[10] = p->world_slot;
						record[11] = server_local_actor_vehicle(s, p) != NULL;
						if(trace) assert(fwrite(record, 1, sizeof record, trace) == sizeof record);
					}
				}
			}
	if(trace) assert(fclose(trace) == 0);
	cleanup(s);
}

/* Payloads are OFF from process start. Server identity/world setup only. */
static void wolf_payload_off_case(void) {
	struct server_local* s = calloc(1, sizeof(*s)); assert(s);
	s->guest_actor = calloc(1, sizeof(*s->guest_actor)); assert(s->guest_actor);
	s->guest_gameplay = true; s->guest_generation = 91;
	for(unsigned i = 0; i < 2; i++) {
		s->worlds[i].chunks_ready = true;
		s->worlds[i].generation = 10 + i;
		s->worlds[i].world_index = i;
		s->worlds[i].session = s;
	}
	struct entity e = {0}; e.type = ENTITY_MONSTER; e.data.monster.id = MONSTER_WOLF;
	wolf_flag_set(&e.data.monster.wolf_flags, WOLF_FLAG_TAMED, true);
	memcpy(e.data.monster.wolf_owner, WOLF_LOCAL_GUEST_NAME, sizeof WOLF_LOCAL_GUEST_NAME);
	assert(!clin_world_payloads_enabled());
	s->guest_actor->world_slot = 0;
	check(entity_wolf_owner_actor(&e, s) == s->guest_actor,
		"payloads off: same-world guest owner resolves");
	s->guest_actor->world_slot = 1;
	check(!entity_wolf_owner_actor(&e, s),
		"payloads off: foreign-world guest owner cannot resolve");
	free(s->guest_actor); free(s);
}

static void splash_case(bool host_nether) {
	struct server_local* s = fixture(host_nether);
	struct server_player* p = s->guest_actor;
	struct server_world* w = server_local_world_of(s, p);
	struct entity* e = prop(s, p, 1);
	cell(w, 8, 101, 8, BLOCK_WATER_STILL);
	s->player.x = 80.5;
	e->pos_old[1] = 102.0F;
	bool wet = false;
	splash_count = 0;
	server_local_splash_tick(s, e, &wet); clin_update_drain();
	check(wet && splash_count == 1, "guest near own-world entity hears its water-entry splash with host far away");
	check(splash_slot == w->world_index && splash_generation == w->generation,
		"splash publication carries the entity's own live world generation");
	/* Host at same coordinates in another dimension must not pass earshot. */
	s->player.x = 8.5; p->x = 80.5;
	wet = false; splash_count = 0;
	server_local_splash_tick(s, e, &wet); clin_update_drain();
	check(wet && !splash_count, "foreign-world host cannot authorize a splash emission");
	p->x = 8.5; s->guest_gameplay = false;
	wet = false; splash_count = 0;
	server_local_splash_tick(s, e, &wet); clin_update_drain();
	check(!splash_count, "attached but unadmitted guest cannot authorize a splash emission");
	s->guest_gameplay = true;
	e->world_generation++;
	wet = false; splash_count = 0;
	server_local_splash_tick(s, e, &wet); clin_update_drain();
	check(!splash_count, "stale entity world generation cannot publish a splash into a replacement world");
	e->world_generation--;
	cleanup(s);
}

static void empty_case(bool host_nether) {
	struct server_local* s = fixture(host_nether);
	struct server_player* p = &s->player;
	struct server_world* w = server_local_world_of(s, p);
	uint32_t ids[4];
	for(unsigned kind = 0; kind < 4; kind++) ids[kind] = prop(s, p, kind)->id;
	struct server_chunk* home = dict_server_chunks_get(w->chunks, S_CHUNK_ID(0, 0));
	assert(home);
	for(int y = 101; y < 105; y++)
		home->ids[y + (8 + 8 * CHUNK_SIZE) * WORLD_HEIGHT] = BLOCK_PORTAL;
	/* Both player AABBs deliberately miss the portal; entities overlap it. */
	p->x = s->guest_actor->x = 12.5;
	for(unsigned i = 0; i < 100; i++) {
		s->paused = false; tick(s); s->paused = true;
	}
	check(!p->in_portal && p->time_in_portal == 0.0F
		&& !s->guest_actor->in_portal && !s->guest_actor->portal_travel_requested,
		"no player AABB overlap: nearby entity portal contact cannot arm a player");
	for(unsigned i = 0; i < 4; i++) {
		struct entity** e = dict_entity_get(s->entities, ids[i]);
		check(e && server_local_entity_world(s, *e) == w,
			"empty boat/cart/pig/mob remains solely in its source world");
	}
	cleanup(s);
}

static void wolf_case(bool host_nether) {
	struct server_local* s = fixture(host_nether);
	struct server_player* p = s->guest_actor;
	struct server_world* w = server_local_world_of(s, p);
	struct entity* e = prop(s, p, 3);
	/* Seed 0 is the JRE8 nextInt(3)==0 positive control, before any AI draw. */
	jrand_set_seed(&e->gameplay_rng, 0);
	p->inventory.hotbar_slot = 0;
	p->inventory.items[INVENTORY_SLOT_HOTBAR] = (struct item_data){.id = ITEM_BONE, .count = 1};
	struct server_rpc use = {.type = SRPC_ENTITY_USE, .payload.entity_use.entity_id = e->id};
	assert(guest_send(s, &use));
	check(wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_TAMED)
		&& !strcmp(e->data.monster.wolf_owner, WOLF_LOCAL_GUEST_NAME),
		"production P2 bone use stores persistent guest owner name (Beta username)");
	check(p->inventory.items[INVENTORY_SLOT_HOTBAR].count == 0,
		"successful P2 tame consumes only P2's bone");
	assert(guest_send(s, &use));
	check(!wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_SITTING),
		"persistent guest owner can toggle its own wolf's sit state");
	struct chunk_entities saved = {0};
	assert(server_local_gather_chunk_entities_w(s, w, 0, 0, &saved));
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(&saved, &arena); assert(list && arena);
	nbt_node tag = {.type = TAG_LIST, .name = "Entities", .payload.tag_list = list};
	struct chunk_entities parsed = {0}; assert(chunk_entities_parse(&tag, 0, 0, &parsed));
	check(parsed.count == 1 && !strcmp(parsed.entities[0].data.monster.wolf_owner, WOLF_LOCAL_GUEST_NAME),
		"production archive and NBT round trip preserve guest owner name");
	chunk_entities_free(&parsed); chunk_entities_build_free(arena); chunk_entities_free(&saved);
	/* Opposing owners make a wrong host-follow observable. */
	s->player.x = 2.5; p->x = 14.5;
	/* First tick must measure to the owner before target_actor is rewritten.
	 * Opposite host direction makes removing target=wolf_owner observable. */
	e->data.monster.target_actor = ACTOR_IDENT_HOST;
	e->data.monster.direction_time = 0;
	assert(!e->tick_server(e, s));
	check(e->data.monster.direction[0] > 0 && e->pos[0] > 8.5F,
		"first follow tick steers toward guest despite stale host target_actor");
	bool followed_guest = false;
	for(unsigned i = 0; i < 8; i++) {
		assert(!e->tick_server(e, s));
		followed_guest |= actor_ident_equal(e->data.monster.target_actor, guest_ident(s))
			&& e->data.monster.wolf_target_id == 0;
	}
	check(followed_guest && e->pos[0] > 8.5F
		&& !wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_SITTING),
		"P2-tamed wolf follows the admitted persistent owner rather than host");
	/* EntityWolf.attackEntityFrom:279-283: own username suppresses retaliation,
	 * after normal damage. A different player must still become the target. */
	e->data.monster.wolf_target_id = UINT32_MAX;
	e->data.monster.target_actor = ACTOR_IDENT_HOST;
	e->data.monster.hurt_resist_time = 0;
	int before = e->health;
	check(entity_wolf_attack_from(e, s, 1, s->guest_entity_id, true, false,
		false, guest_ident(s)) != MONSTER_DAMAGE_ABSORBED && e->health == before - 1,
		"guest owner's hit still applies full damage");
	check(e->data.monster.wolf_target_id == UINT32_MAX
		&& actor_ident_is_host(e->data.monster.target_actor),
		"guest hitting own wolf does not arm retaliation");
	e->data.monster.hurt_resist_time = 0;
	(void)entity_wolf_attack_from(e, s, 1, SERVER_LOCAL_PLAYER_ENTITY_ID, true,
		false, false, ACTOR_IDENT_HOST);
	check(e->data.monster.wolf_target_id == 0,
		"non-owner host hit does arm retaliation");
	e->data.monster.wolf_target_id = UINT32_MAX;
	struct item_data empty = {0};
	check(entity_wolf_interact_for(e, s, &s->player, &empty) == WOLF_INTERACT_NONE,
		"host name cannot toggle the guest's tamed wolf");
	s->guest_gameplay = false;
	assert(!e->tick_server(e, s));
	check(!entity_wolf_owner_actor(e, s)
		&& wolf_flag_get(e->data.monster.wolf_flags, WOLF_FLAG_SITTING),
		"absent persistent guest owner makes its wolf sit instead of following host");
	s->guest_gameplay = true;
	check(entity_wolf_owner_actor(e, s) == p,
		"durable name re-resolves when its owner is admitted again");
	cleanup(s);
}

int main(int argc, char** argv) {
	assert(argc >= 3);
	setvbuf(stdout, NULL, _IONBF, 0);
	if(!strcmp(argv[1], "wolf_payload_off")) {
		wolf_payload_off_case();
		return harness_result("test_l211_portal_split", argv[1]);
	}
	blocks_init(); items_init(); recipe_init();
	assert(clin_init() && svin_init());
	assert(clin_world_payloads_set_enabled(true));
	world_create(&gstate.world); dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);
	if(!strcmp(argv[1], "portal")) {
		assert(argc == 5);
		portal_case((unsigned)atoi(argv[2]), atoi(argv[3]) != 0, atoi(argv[4]) != 0);
	} else if(!strcmp(argv[1], "splash")) splash_case(atoi(argv[2]) != 0);
	else if(!strcmp(argv[1], "wolf")) wolf_case(atoi(argv[2]) != 0);
	else if(!strcmp(argv[1], "empty")) empty_case(atoi(argv[2]) != 0);
	else if(!strcmp(argv[1], "armed")) armed_case(atoi(argv[2]) != 0);
	else if(!strcmp(argv[1], "walks")) walks_case(atoi(argv[2]) != 0);
	else if(!strcmp(argv[1], "scan")) scan_case(atoi(argv[2]) != 0);
	else assert(!"unknown case");
	return harness_result("test_l211_portal_split", argv[1]);
}
