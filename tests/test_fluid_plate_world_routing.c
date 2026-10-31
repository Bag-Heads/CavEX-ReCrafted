/* Copyright (c) 2026 CaveWii_v2 contributors
 * World routing only: both host/guest dimension assignments, real fluid and
 * plate scheduled queues, and byte-identical cells in the other world.
 * Beta b1.7.3 RetroMCP minecraft/src/net/minecraft/src/BlockPressurePlate.java
 * setStateIfMobInteractsWithPlate:69-107 queries entities through the SAME
 * World as the plate; tickRate:17-19 = 20. BlockFlowing.updateTick:21-85
 * reads/writes that same World and downward source flow has metadata 0+8.
 */
#include "support/owner_swap_harness.h"
#include "block/block_pressure_plate.h"
#include "entity/entity.h"
#include "entity/monsters_object.h"

#define X 8
#define Y 80
#define Z 8
#define WORLD_BYTES (32 * 32 * 128 * 4)

static uint8_t before[WORLD_BYTES], after[WORLD_BYTES];

static struct block_data get(struct server_world* w, int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(w, x, y, z, &b));
	return b;
}

static void put(struct server_world* w, int x, int y, int z, uint8_t type,
				uint8_t metadata) {
	(void)server_world_set_block_silent(w, x, y, z,
		(struct block_data) {.type = type, .metadata = metadata});
	struct block_data b = get(w, x, y, z);
	assert(b.type == type && b.metadata == metadata);
}

static void snapshot(struct server_world* w, uint8_t* bytes) {
	/* Exactly the four columns loaded below: this comparison never samples
	 * or silently omits an extra resident column. */
	assert(dict_server_chunks_size(w->chunks) == 4);
	size_t n = 0;
	for(int x = 0; x < 32; x++)
		for(int z = 0; z < 32; z++)
			for(int y = 0; y < 128; y++) {
				struct block_data b = get(w, x, y, z);
				bytes[n++] = b.type;
				bytes[n++] = b.metadata;
				bytes[n++] = b.sky_light;
				bytes[n++] = b.torch_light;
			}
	assert(n == WORLD_BYTES);
}

static void reset_ticks(struct server_world* w) {
	server_world_scheduled_ticks_destroy(w);
	server_world_scheduled_ticks_create(w, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);
}

static void scene(struct server_local* s) {
	for(unsigned i = 0; i < 2; i++) {
		struct server_world* w = &s->worlds[i];
		reset_ticks(w);
		for(int x = X - 2; x <= X + 2; x++)
			for(int z = Z - 2; z <= Z + 2; z++)
				for(int y = Y - 2; y <= Y + 2; y++)
					put(w, x, y, z, y < Y ? BLOCK_STONE : BLOCK_AIR, 0);
	}
}

static void pose(struct server_player* actor, bool on_plate) {
	actor->x = actor->z = on_plate ? 8.5 : 24.5;
	actor->y = Y + PLAYER_EYE_HEIGHT;
	actor->has_pos = actor->finished_loading = true;
}

static void world_tick(struct server_local* s, struct server_world* w) {
	/* The public world-tick path must route its own callbacks without help. */
	server_world_tick(w, s);
	assert(block_world(s) == &s->worlds[0]);
}

static void other_unchanged(struct server_world* other) {
	snapshot(other, after);
	check(memcmp(before, after, sizeof(before)) == 0,
		  "other world: all loaded block/metadata/light bytes unchanged");
}

static void fluid_case(struct server_local* s, struct server_player* actor,
					   uint8_t fluid, bool open) {
	struct server_world* w = server_local_world_of(s, actor);
	struct server_world* other = &s->worlds[1 - actor->world_slot];
	scene(s);
	pose(&s->player, false);
	pose(s->guest_actor, false);
	for(unsigned i = 0; i < 2; i++) {
		struct server_world* fw = &s->worlds[i];
		/* Same source, opposing downward route. Side walls block spread. */
		put(fw, X, Y, Z, fluid, 0);
		put(fw, X - 1, Y, Z, BLOCK_STONE, 0);
		put(fw, X + 1, Y, Z, BLOCK_STONE, 0);
		put(fw, X, Y, Z - 1, BLOCK_STONE, 0);
		put(fw, X, Y, Z + 1, BLOCK_STONE, 0);
	}
	put(open ? w : other, X, Y - 1, Z, BLOCK_AIR, 0);
	snapshot(other, before);
	assert(server_world_schedule_block_tick(w, X, Y, Z, fluid));
	/* Beta BlockFluid.tickRate: water=5, lava=30. */
	const unsigned delay = fluid == BLOCK_WATER_FLOW ? 5 : 30;
	assert(blocks[fluid]->tick_delay == delay);
	for(unsigned i = 0; i < delay; i++) {
		world_tick(s, w);
		struct block_data below = get(w, X, Y - 1, Z);
		check(!open ? below.type == BLOCK_STONE
			  : i + 1 < delay ? below.type == BLOCK_AIR
			  : below.type == fluid && below.metadata == 8,
			  open ? "fluid: source falls at its Beta scheduled tick, metadata 8"
				   : "fluid: own-world solid floor blocks flow despite foreign air");
	}
	other_unchanged(other);
}

static void plate_case(struct server_local* s, struct server_player* actor,
					   uint8_t plate) {
	struct server_world* w = server_local_world_of(s, actor);
	struct server_player* peer = actor == &s->player ? s->guest_actor : &s->player;
	struct server_world* other = server_local_world_of(s, peer);
	scene(s);
	put(w, X, Y, Z, plate, 0);
	put(other, X, Y, Z, plate, 0);
	pose(actor, false);
	pose(peer, true);
	snapshot(other, before);
	struct server_world* prev = block_world_enter(s, w);
	block_pressure_plate_on_entity_tick(s, X, Y, Z);
	block_world_leave(s, prev);
	check(get(w, X, Y, Z).metadata == 0,
		  "plate: player in the other dimension cannot press it");
	/* Arrival through server_world_tick must also use this world's guest
	 * when the host is elsewhere. No direct collision hook in this leg. */
	put(w, X, Y, Z, plate, 0);
	reset_ticks(w);
	pose(actor, true);
	pose(peer, false);
	world_tick(s, w);
	check(get(w, X, Y, Z).metadata == 1,
		  "plate: own-world actor arrives immediately through world tick");
	const uint64_t pressed_at = w->current_tick;
	/* Move only the own-world actor away; the other-world actor occupies
	 * the same coordinates, so the scheduled query must ignore it. */
	pose(actor, false);
	pose(peer, true);
	for(unsigned i = 1; i <= 20; i++) {
		world_tick(s, w);
		check(get(w, X, Y, Z).metadata == (i < 20 ? 1 : 0),
			  "plate: release on tick 20 despite foreign-world player");
	}
	assert(w->current_tick == pressed_at + 20);
	check(w->scheduled_tick_count == 0,
		  "plate: released state does not reschedule");
	other_unchanged(other);

	/* Real constructors carry generation-safe world bindings. Wood queries
	 * every entity; stone queries living entities (a pig, but not an item). */
	for(unsigned living = 0; living < 2; living++) {
		scene(s);
		put(w, X, Y, Z, plate, 1);
		pose(actor, false);
		pose(peer, false);
		struct entity e = {0};
		if(living)
			entity_monster(9001, &e, true, other, MONSTER_PIG);
		else
			entity_item(9001, &e, true, other,
				(struct item_data) {.id = BLOCK_STONE, .count = 1});
		glm_vec3_copy((vec3) {8.5F, Y + 0.125F, 8.5F}, e.pos);
		dict_entity_set_at(s->entities, e.id, &e);
		assert(server_local_entity_world(s, &e) == other);
		snapshot(other, before);
		assert(server_world_schedule_block_tick(w, X, Y, Z, plate));
		for(unsigned i = 0; i < 20; i++)
			world_tick(s, w);
		check(get(w, X, Y, Z).metadata == 0,
			  "plate: scheduled recheck ignores foreign-world item/mob");
		entity_bind_world(&e, w);
		prev = block_world_enter(s, w);
		block_pressure_plate_on_entity_tick(s, X, Y, Z);
		block_world_leave(s, prev);
		check(get(w, X, Y, Z).metadata == (living || plate == BLOCK_WOOD_PRESSURE_PLATE ? 1 : 0),
			  "plate: own-world mob presses both; own-world item presses wood only");
		other_unchanged(other);
		dict_entity_erase(s->entities, e.id);
	}
}

int main(void) {
	harness_init();
	struct server_local* s = split_fixture();
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(&s->worlds[1], path, WORLD_DIM_NETHER, 12345, true);
	assert(s->worlds[1].generator);
	string_clear(path);
	s->worlds[1].local = s;
	s->worlds[1].session = s;
	s->worlds[1].world_index = 1;
	struct server_player guest = {0};
	s->guest_actor = &guest;
	s->guest_generation = 77;
	s->scheduled_tick_budget = 0;
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	for(unsigned i = 0; i < 2; i++)
		for(int x = 0; x < 2; x++)
			for(int z = 0; z < 2; z++) {
				struct server_chunk* chunk = NULL;
				assert(server_world_load_chunk(&s->worlds[i], x, z, &chunk) && chunk);
			}
	for(unsigned swap = 0; swap < 2; swap++) {
		s->player.world_slot = swap;
		s->player.dimension = swap ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
		guest.world_slot = 1 - swap;
		guest.dimension = swap ? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER;
		printf("assignment: host=%s guest=%s\n", swap ? "Nether" : "Overworld",
			   swap ? "Overworld" : "Nether");
		struct server_player* actors[] = {&s->player, &guest};
		for(unsigned a = 0; a < 2; a++) {
			assert(server_local_world_of(s, actors[a]) == &s->worlds[actors[a]->world_slot]);
			printf("actor=%s slot=%u\n", a ? "guest" : "host", actors[a]->world_slot);
			fluid_case(s, actors[a], BLOCK_WATER_FLOW, true);
			fluid_case(s, actors[a], BLOCK_WATER_FLOW, false);
			fluid_case(s, actors[a], BLOCK_LAVA_FLOW, true);
			fluid_case(s, actors[a], BLOCK_LAVA_FLOW, false);
			plate_case(s, actors[a], BLOCK_STONE_PRESSURE_PLATE);
			plate_case(s, actors[a], BLOCK_WOOD_PRESSURE_PLATE);
		}
	}
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	s->guest_actor = NULL;
	s->guest_generation = 0;
	server_world_discard(&s->worlds[1]);
	split_destroy(s);
	return harness_result("test_fluid_plate_world_routing", "both dimension assignments");
}
