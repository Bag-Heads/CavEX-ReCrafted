/* L214: ItemMap.onUpdate and ItemPainting/EntityPainting use the acting
 * world (Beta src_original ItemMap.java, ItemPainting.java, EntityPainting.java).
 * Production inventory dispatch / placement, both owners and assignments.
 * RED prints every witness rather than aborting at the first mismatch. */
#include "support/owner_swap_harness.h"
#include "entity/entity_painting.h"
#include "item/items/item_map.h"
#include "network/map_data_archive.h"
#include "network/server_transfer.h"
#include "network/region_archive.h"

static unsigned marker_count, painting_msgs, map_msgs;
static struct client_rpc last_marker, last_painting;
static uint8_t map_colors[MAP_DATA_COLORS_SIZE], map_world;
static uint32_t map_generation;
/* A consumer returns inbox tokens, but deferred envelopes are producer-owned.
 * After joining the consumer, settle each seeding batch before actions. */
static void quiesce(void) {
	for(unsigned n = 0; n < 100; n++) {
		clin_update_drain();
		if(clin_rpc_flush_pending()) {
			clin_update_drain();
			return;
		}
	}
	assert(!"client queue did not drain");
}
static void observe(const struct client_rpc* call, void* unused) {
	(void)unused;
	if(call->type == CRPC_MAP_MARKERS) {
		marker_count++;
		last_marker = *call;
	}
	if(call->type == CRPC_SPAWN_PAINTING) {
		painting_msgs++;
		last_painting = *call;
	}
	if(call->type == CRPC_MAP_DATA) {
		map_msgs++;
		map_world = call->world_index;
		map_generation = call->world_generation;
		memcpy(map_colors, call->payload.map_data.slot->colors, sizeof(map_colors));
	}
}
static size_t count_type(struct server_local* s, struct server_world* w, enum entity_type type) {
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == type && server_local_entity_world(s, e) == w)
			n++;
	}
	return n;
}
static uint64_t terrain_hash(struct server_world* w) {
	uint64_t hash = UINT64_C(14695981039346656037);
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++)
			for(int y = 0; y < 128; y++) {
				struct block_data b;
				assert(server_world_get_block(w, x, y, z, &b));
				uint32_t v = (uint32_t)b.type << 24 | (uint32_t)b.metadata << 16
					| (uint32_t)b.sky_light << 8 | b.torch_light;
				for(unsigned byte = 0; byte < 4; byte++) {
					hash = (hash ^ (uint8_t)v) * UINT64_C(1099511628211);
					v >>= 8;
				}
			}
	return hash;
}
static void wall(struct server_world* w, bool solid) {
	for(int x = 4; x <= 12; x++)
		for(int z = 5; z <= 8; z++)
			for(int y = 96; y <= 104; y++)
				(void)server_world_set_block_silent(w, x, y, z,
					(struct block_data){.type = solid && z == 8 ? BLOCK_STONE : BLOCK_AIR});
}
static void place(struct server_local* s, struct server_player* actor) {
	actor->inventory.items[INVENTORY_SLOT_HOTBAR] =
		(struct item_data){.id = ITEM_PAINTING, .count = 2};
	struct server_rpc call = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = 8, .y = 100, .z = 8, .side = SIDE_FRONT}};
	if(actor == &s->player) host_send(s, &call);
	else assert(guest_send(s, &call));
}
static struct entity* newest_painting(struct server_local* s) {
	struct entity* found = NULL;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_PAINTING && (!found || e->id > found->id)) found = e;
	}
	return found;
}
static struct entity* cold_painting(struct server_local* s,
	struct server_world* w, struct entity* p) {
	uint32_t old_id = p->id;
	struct entity_painting saved = p->data.painting;
	struct random_gen rng = s->rand_src;
	quiesce(); painting_msgs = 0;
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
	check(!server_world_save_chunk(w, true, 0, 0)
		&& server_world_is_chunk_loaded(w, 0, 0)
		&& dict_entity_get(s->entities, old_id)
		&& server_local_entity_world(s, p) == w
		&& !memcmp(&saved, &p->data.painting, sizeof(saved))
		&& !memcmp(&rng, &s->rand_src, sizeof(rng)) && painting_msgs == 0,
		"slot-1 painting save failure retains live entity, chunk and RNG");
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	bool erased = server_world_save_chunk(w, true, 0, 0);
	bool retired = !dict_entity_get(s->entities, old_id)
		&& !server_world_is_chunk_loaded(w, 0, 0);
	struct server_chunk* chunk = NULL;
	bool loaded = erased && server_world_load_chunk(w, 0, 0, &chunk);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	quiesce();
	check(erased && retired && loaded, "slot-1 painting chunk saves, evicts and cold reloads");
	struct entity* restored = newest_painting(s);
	/* Secondary restore intentionally defers spawn until the streamed chunk
	 * publisher. Exercise that same production snapshot API after cold load. */
	if(restored)
		check(server_local_try_publish_entity_w(s, w, restored),
			"cold painting stream snapshot is admitted");
	quiesce();
	printf("cold painting old=%u restored=%u art=%d/%d msgs=%u stamp=%u/%u expected=%u/%u rng_equal=%d\n",
		old_id, restored ? restored->id : 0,
		restored ? restored->data.painting.art_index : -1, saved.art_index,
		painting_msgs, last_painting.world_index, last_painting.world_generation,
		w->world_index, w->generation, !memcmp(&rng, &s->rand_src, sizeof(rng)));
	check(restored && restored->id != old_id
		&& server_local_entity_world(s, restored) == w
		&& restored->data.painting.art_index == saved.art_index
		&& restored->data.painting.direction == saved.direction
		&& restored->data.painting.tile_x == saved.tile_x
		&& restored->data.painting.tile_y == saved.tile_y
		&& restored->data.painting.tile_z == saved.tile_z
		&& painting_msgs == 1 && last_painting.world_index == w->world_index
		&& last_painting.world_generation == w->generation
		&& !memcmp(&rng, &s->rand_src, sizeof(rng)),
		"cold slot-1 painting keeps art, backing coordinates and routed spawn without RNG");
	return restored;
}
/* R2: the Overworld provider is genuinely recreated in slot 1. The
 * production dispatch must sample it, even though slot 0 has opposite bytes. */
static void slot1_terrain(struct server_local* s, struct server_player* actor) {
	server_world_discard(&s->worlds[1]);
	string_t path;
	string_init_set_str(path, split_path);
	string_cat_str(path, "/slot1");
#ifdef _WIN32
	assert(_mkdir(string_get_cstr(path)) == 0);
#else
	assert(mkdir(string_get_cstr(path), 0777) == 0);
#endif
	server_world_create(&s->worlds[1], path, WORLD_DIM_OVERWORLD, 6789, true);
	assert(s->worlds[1].generator && server_world_chunks_ready(&s->worlds[1]));
	string_clear(path);
	struct server_world* w = &s->worlds[1];
	w->world_index = 1; w->session = w->local = s;
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	struct server_chunk* c = NULL;
	assert(server_world_load_chunk(w, 0, 0, &c));
	for(unsigned wi = 0; wi < 2; wi++)
		for(int z = 7; z <= 8; z++)
			for(int y = 1; y < 128; y++)
				(void)server_world_set_block_silent(&s->worlds[wi], 8, y, z,
					(struct block_data){.type = y == 4 ? (wi ? BLOCK_GRASS : BLOCK_STONE)
						: y < 4 ? BLOCK_STONE : BLOCK_AIR});
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer); quiesce();
	actor->world_slot = 1; actor->dimension = WORLD_DIM_OVERWORLD;
	actor->x = actor->z = 8;
	struct item_data stack = {.id = ITEM_MAP, .count = 1};
	assert(item_map.onCreated(s, actor, &stack));
	struct map_data* md = item_map_cache_resident(s, (int16_t)stack.durability);
	md->scale = 0; md->step = 15;
	actor->inventory.items[INVENTORY_SLOT_HOTBAR] = stack;
	map_msgs = 0;
	server_local_test_tick_item_updates_w(s, w); quiesce();
	check(md->step == 16 && md->colors[64 + 64 * MAP_DATA_DIM] == 5
		&& map_msgs == 1 && map_world == 1 && map_generation == w->generation,
		"production slot-1 Overworld scan reads grass=5, not slot-0 stone=45");
}
/* Beta MapData.java:71-104: first update inserts, inventory loss/death
 * removes, and range/dimension only suppress an icon. */
static void marker_order(struct server_local* s, struct server_player** actors) {
	struct item_data stack = {.id = ITEM_MAP, .count = 1};
	assert(item_map.onCreated(s, actors[0], &stack));
	struct map_data* md = item_map_cache_resident(s, (int16_t)stack.durability);
	for(unsigned a = 0; a < 2; a++) {
		actors[a]->inventory.items[INVENTORY_SLOT_HOTBAR] = stack;
		actors[a]->x = md->x_center + 8 * (a + 1);
		actors[a]->z = md->z_center;
	}
	item_map.onUpdateFor(s, actors[1], &stack, INVENTORY_SLOT_HOTBAR, false);
	item_map.onUpdateFor(s, actors[0], &stack, INVENTORY_SLOT_HOTBAR, true);
	quiesce();
	check(last_marker.payload.map_markers.length == 7
		&& last_marker.payload.map_markers.bytes[2] == 4
		&& last_marker.payload.map_markers.bytes[5] == 2,
		"guest first updater draws before host (including non-held update)");
	actors[1]->x = md->x_center + 10000;
	item_map.onUpdateFor(s, actors[0], &stack, INVENTORY_SLOT_HOTBAR, true);
	quiesce();
	check(last_marker.payload.map_markers.length == 4,
		"out-of-range holder icon hides");
	actors[1]->x = md->x_center + 16;
	item_map.onUpdateFor(s, actors[0], &stack, INVENTORY_SLOT_HOTBAR, true);
	quiesce();
	check(last_marker.payload.map_markers.bytes[2] == 4,
		"returning in-range holder retains Beta insertion order");
	for(unsigned death = 0; death < 2; death++) {
		if(death) actors[1]->health = 0;
		else actors[1]->inventory.items[INVENTORY_SLOT_HOTBAR] = (struct item_data){0};
		item_map.onUpdateFor(s, actors[0], &stack, INVENTORY_SLOT_HOTBAR, true);
		quiesce();
		check(last_marker.payload.map_markers.length == (death ? 4 : 1),
			"dead/non-carrying holder leaves list (Beta skips shifted icon this update)");
		actors[1]->health = MAX_PLAYER_HEALTH;
		actors[1]->inventory.items[INVENTORY_SLOT_HOTBAR] = stack;
		item_map.onUpdateFor(s, actors[1], &stack, INVENTORY_SLOT_HOTBAR, true);
		quiesce();
		check(last_marker.payload.map_markers.length == 7
			&& last_marker.payload.map_markers.bytes[2] == 2
			&& last_marker.payload.map_markers.bytes[5] == 4,
			"returning dead/non-carrying holder re-adds at tail");
	}
}
int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init(); items_init(); recipe_init();
	assert(clin_init() && svin_init());
	assert(clin_world_payloads_set_enabled(true));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET});
	clin_test_set_consume_observer(consumed, NULL);
	struct server_local* s = admitted_fixture();
	string_set_str(s->level_name, split_path);
	uint32_t transfer;
	assert(server_transfer_request(s, &s->player, WORLD_DIM_NETHER, &transfer)
		== SERVER_TRANSFER_OK);
	s->paused = false;
	for(unsigned n = 0; n < 4000
		&& server_transfer_state_of(s, transfer) != SERVER_TRANSFER_COMMITTED; n++) {
		tick(s);
		client_acks_marker(s);
	}

	assert(server_transfer_state_of(s, transfer) == SERVER_TRANSFER_COMMITTED);
	for(unsigned n = 0; n < 4000 && (s->input_ack_pending
		|| server_transfer_actor_inert(s, &s->player)); n++) {
		step(s);
		client_acks_marker(s);
	}
	assert(!s->input_ack_pending);
	assert(!server_transfer_actor_inert(s, &s->player));
	s->paused = true;
	struct server_player* actors[] = {&s->player, s->guest_actor};
	/* Seeding publishes columns: a consumer prevents bounded-inbox deadlock. */
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	for(unsigned wi = 0; wi < 2; wi++) {
		struct server_chunk* c = NULL;
		if(!server_world_is_chunk_loaded(&s->worlds[wi], 0, 0))
			assert(server_world_load_chunk(&s->worlds[wi], 0, 0, &c));
	}
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	quiesce();
	clin_test_set_rpc_observer(observe, NULL);
	for(unsigned swap = 0; swap < 2; swap++) {
		for(unsigned a = 0; a < 2; a++) {
			actors[a]->world_slot = a ^ swap;
			actors[a]->dimension = (a ^ swap) ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
			actors[a]->x = 8.5; actors[a]->z = 6.5; actors[a]->y = 101.62;
			actors[a]->rx = 90 * a;
			actors[a]->health = MAX_PLAYER_HEALTH;
			actors[a]->has_pos = actors[a]->finished_loading = true;
			actors[a]->active_inventory = &actors[a]->inventory;
		}
		for(unsigned a = 0; a < 2; a++) {
			struct server_player* actor = actors[a];
			struct server_world* w = server_local_world_of(s, actor);
			struct server_world* foreign = server_local_world_of(s, actors[1-a]);
			assert(w && foreign && w != foreign);
			printf("owner %u world %u\n", a, w->world_index);
			assert(!server_local_light_stopped(s));
			struct item_data map = {.id = ITEM_MAP, .count = 1};
			assert(item_map.onCreated(s, actor, &map));
			actor->inventory.items[INVENTORY_SLOT_HOTBAR] = map;
			struct map_data* md = item_map_cache_resident(s, (int16_t)map.durability);
			assert(md && md->dimension == actor->dimension);
			struct item_data foreign_inv[INVENTORY_SIZE];
			memcpy(foreign_inv, actors[1-a]->inventory.items, sizeof(foreign_inv));
			struct random_gen rng = s->rand_src;
			uint64_t foreign_hash = terrain_hash(foreign);
			quiesce();
			marker_count = map_msgs = 0;
			server_local_test_tick_item_updates_w(s, w);
			clin_update_drain();
			check(md->step == 1, "held map scanned once in acting world");
			check(map_msgs == 1 && map_world == w->world_index
				&& map_generation == w->generation
				&& !memcmp(map_colors, md->colors, sizeof(map_colors)),
				"terrain payload delivers acting map bytes with owning world stamp");
			check(marker_count == 1 && last_marker.world_index == w->world_index
				&& last_marker.world_generation == w->generation,
				"marker delivered with acting world stamp");
			printf("  markers=%u stamp=%u/%u expected=%u/%u len=%u\n", marker_count,
				last_marker.world_index, last_marker.world_generation,
				w->world_index, w->generation, last_marker.payload.map_markers.length);
			struct item_map_marker expected;
			assert(item_map_player_marker(md, actor->x, actor->z, actor->rx,
				actor->dimension, true, &expected));
			check(marker_count && last_marker.payload.map_markers.length == 4
				&& last_marker.payload.map_markers.bytes[1] == (uint8_t)(expected.rotation * 16),
				"marker uses holder yaw before scan");
			check(!memcmp(foreign_inv, actors[1-a]->inventory.items, sizeof(foreign_inv))
				&& !memcmp(&rng, &s->rand_src, sizeof(rng))
				&& terrain_hash(foreign) == foreign_hash,
				"map leaves foreign inventory, terrain/light and RNG intact");
			/* Nether maps retain their deterministic fake columns; Overworld
			 * sampling is exercised separately against opposite terrain below. */
			item_map_cache_flush_all(s);
			struct map_data cold;
			check(map_data_archive_load(s->level_name, (int16_t)map.durability, &cold)
				&& !memcmp(cold.colors, md->colors, sizeof(cold.colors)), "map colors survive cold archive reload");
			__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
			assert(thread_create(&consumer, consumer_entry, NULL, 64));
			wall(w, true); wall(foreign, false);
			__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
			thread_join(&consumer);
			quiesce();
			/* Beta World.getEntitiesWithinAABB is world-local. A foreign
			 * painting at identical coordinates must not reject any art. */
			uint32_t fid = server_local_gen_id(s);
			struct entity* fp = calloc(1, sizeof(*fp));
			assert(fp);
			entity_painting(fid, fp, true, foreign, 0, PAINTING_DIR_NORTH, 8, 100, 8);
			dict_entity_set_at(s->entities, fid, fp);
			struct entity scratch = {0};
			entity_painting(0, &scratch, true, w, 0, PAINTING_DIR_NORTH, 8, 100, 8);
			check(entity_painting_on_valid_surface(&scratch, s, 0, PAINTING_DIR_NORTH,
				8, 100, 8, UINT32_MAX), "foreign painting overlap is ignored");
			free(fp); dict_entity_erase(s->entities, fid);
			size_t own_before = count_type(s, w, ENTITY_PAINTING);
			size_t other_before = count_type(s, foreign, ENTITY_PAINTING);
			foreign_hash = terrain_hash(foreign);
			painting_msgs = 0;
			place(s, actor);
			check(count_type(s, w, ENTITY_PAINTING) == own_before + 1
				&& count_type(s, foreign, ENTITY_PAINTING) == other_before
				&& terrain_hash(foreign) == foreign_hash,
				"painting placement joins acting world with foreign backing absent");
			check(actor->inventory.items[INVENTORY_SLOT_HOTBAR].count == 1,
				"successful painting consumes one item");
			check(painting_msgs == 1 && last_painting.world_index == w->world_index,
				"painting spawn carries owning world stamp");
			struct entity* p = newest_painting(s);
			if(p && server_local_entity_world(s, p) == w) {
				struct java_random pick_rng;
				entity_lifetime_rng_seed(&pick_rng, w->seed, w->dimension, p->id);
				int expected_art = jrand_next_int_bound(&pick_rng, PAINTING_ART_COUNT);
				check(p->data.painting.art_index == expected_art,
					"art uses one lifetime RNG pick in owning world");
				if(w->world_index == 1)
					p = cold_painting(s, w, p);
				assert(p);
				check(!p->tick_server(p, s), "foreign missing wall does not break own painting");
				place(s, actor);
				check(count_type(s, w, ENTITY_PAINTING) == own_before + 1,
					"own painting overlap refuses a second placement");
				size_t drops = count_type(s, w, ENTITY_ITEM);
				size_t other_drops = count_type(s, foreign, ENTITY_ITEM);
				entity_painting_mark_broken(p);
				check(p->tick_server(p, s), "hit painting breaks on next tick");
				check(count_type(s, w, ENTITY_ITEM) == drops + 1
					&& count_type(s, foreign, ENTITY_ITEM) == other_drops, "painting drops in its own world");
				uint32_t id = p->id;
				free(p); dict_entity_erase(s->entities, id);
			}
			/* Invalid own backing, solid foreign backing: refuse without
			 * spending stack/RNG. Allocation failure also leaves the stack. */
			__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
			assert(thread_create(&consumer, consumer_entry, NULL, 64));
			wall(w, false); wall(foreign, true);
			__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
			thread_join(&consumer);
			quiesce();
			place(s, actor);
			check(actor->inventory.items[INVENTORY_SLOT_HOTBAR].count == 2,
				"own missing backing refuses despite solid foreign wall");
			__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
			assert(thread_create(&consumer, consumer_entry, NULL, 64));
			wall(w, true); wall(foreign, false);
			__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
			thread_join(&consumer);
			quiesce();
			size_t paintings_before_failure = count_type(s, w, ENTITY_PAINTING);
			rng = s->rand_src;
			server_local_test_fail_next_entity_alloc();
			place(s, actor);
			check(actor->inventory.items[INVENTORY_SLOT_HOTBAR].count == 2
				&& count_type(s, w, ENTITY_PAINTING) == paintings_before_failure
				&& !memcmp(&rng, &s->rand_src, sizeof(rng)),
				"painting allocation refusal preserves stack/world RNG");
			place(s, actor);
			p = newest_painting(s);
			check(p && server_local_entity_world(s, p) == w
				&& count_type(s, w, ENTITY_PAINTING) == paintings_before_failure + 1,
				"painting placement succeeds after allocation refusal");
			if(p && server_local_entity_world(s, p) == w) {
				__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
				assert(thread_create(&consumer, consumer_entry, NULL, 64));
				wall(w, false); wall(foreign, true);
				__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
				thread_join(&consumer);
				quiesce();
				p->data.painting.tick_counter = 100;
				size_t drops = count_type(s, w, ENTITY_ITEM);
				check(p->tick_server(p, s) && p->data.painting.tick_counter == 0
					&& count_type(s, w, ENTITY_ITEM) == drops + 1,
					"101st validity check drops for own wall gap despite solid foreign wall");
				uint32_t id = p->id;
				free(p); dict_entity_erase(s->entities, id);
			}
		}
	}
	clin_test_set_rpc_observer(NULL, NULL);
	actors[0]->world_slot = actors[1]->world_slot = 0;
	actors[0]->dimension = actors[1]->dimension = WORLD_DIM_OVERWORLD;
	/* A shared identity remains one record and advances once per held stack.
	 * Both markers survive a holder update; another dimension contributes
	 * neither a marker nor a terrain scan to this Overworld map. */
	struct item_data shared = {.id = ITEM_MAP, .count = 1};
	assert(item_map.onCreated(s, actors[0], &shared));
	struct map_data* shared_md = item_map_cache_resident(s, (int16_t)shared.durability);
	actors[0]->inventory.items[INVENTORY_SLOT_HOTBAR] = shared;
	actors[1]->inventory.items[INVENTORY_SLOT_HOTBAR] = shared;
	clin_test_set_rpc_observer(observe, NULL);
	quiesce(); marker_count = 0;
	server_local_test_tick_item_updates_w(s, &s->worlds[0]);
	check(shared_md->step == 2 && marker_count == 2
		&& last_marker.payload.map_markers.length == 7,
		"shared holders use one map ID, two scans and both markers");
	check(last_marker.payload.map_markers.bytes[1] == 64
		&& last_marker.payload.map_markers.bytes[4] == 64,
		"Beta shared markers use updating actor yaw for both icons");
	quiesce(); marker_count = 0;
	s->guest_gameplay = false;
	server_local_test_tick_item_updates_w(s, &s->worlds[0]);
	check(shared_md->step == 3 && marker_count == 1
		&& last_marker.payload.map_markers.length == 4,
		"attached guest without gameplay body neither scans nor contributes markers");
	s->guest_gameplay = true;
	actors[1]->world_slot = 1; actors[1]->dimension = WORLD_DIM_NETHER;
	quiesce(); marker_count = 0;
	server_local_test_tick_item_updates_w(s, &s->worlds[1]);
	check(shared_md->step == 3 && marker_count == 1
		&& last_marker.payload.map_markers.length == 4,
		"foreign holder preserves map bytes/step and same-dimension marker");
	actors[1]->world_slot = 0; actors[1]->dimension = WORLD_DIM_OVERWORLD;
	/* R2: different identities must settle independently. Dimension-
	 * mismatched maps never scan, so only the send gate is under test. */
	for(unsigned a = 0; a < 2; a++) {
		struct item_data separate = {.id = ITEM_MAP, .count = 1};
		assert(item_map.onCreated(s, actors[a], &separate));
		struct map_data* md = item_map_cache_resident(s, (int16_t)separate.durability);
		md->dimension = WORLD_DIM_NETHER;
		memset(md->colors, 30 + a, sizeof(md->colors));
		actors[a]->inventory.items[INVENTORY_SLOT_HOTBAR] = separate;
	}
	quiesce(); map_msgs = 0;
	server_local_test_tick_item_updates_w(s, &s->worlds[0]);
	quiesce();
	check(map_msgs == 2, "distinct held maps sync once for each player");
	for(unsigned n = 0; n < 3; n++) {
		server_local_test_tick_item_updates_w(s, &s->worlds[0]);
		quiesce();
	}
	printf("distinct map sends after four ticks: %u (expected 2)\n", map_msgs);
	check(map_msgs == 2, "distinct held maps stop sending after first sync");
	marker_order(s, actors);
	slot1_terrain(s, actors[1]);
	actors[1]->world_slot = 0; actors[1]->dimension = WORLD_DIM_OVERWORLD;
	/* The legacy wrapper allowed spawning before chunks_ready. Keep the
	 * dictionary constructed while testing that observer flag. */
	bool ready = s->worlds[0].chunks_ready;
	s->worlds[0].chunks_ready = false;
	struct entity* legacy = server_local_spawn_painting(s, 0,
		PAINTING_DIR_NORTH, 8, 100, 8);
	s->worlds[0].chunks_ready = ready;
	check(legacy != NULL, "legacy painting wrapper retains pre-ready SP behavior");
	if(legacy) {
		uint32_t id = legacy->id;
		free(legacy); dict_entity_erase(s->entities, id);
	}
	clin_test_set_rpc_observer(NULL, NULL);
	item_map_cache_flush_all(s);
	/* Guest release checkpoints both live worlds; release before discarding. */
	guest_release(s);
	server_world_discard(&s->worlds[1]);
	teardown(s);
	return harness_result("test_map_painting_world_routing", "both_assignments");
}
