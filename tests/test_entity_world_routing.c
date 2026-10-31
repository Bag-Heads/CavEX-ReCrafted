/* Copyright (c) 2026 CaveWii_v2 contributors
 * Entity.moveEntity checks residency in worldObj before its collision sweep;
 * EntityWolf resolves owners and queries sheep/pack members in worldObj;
 * EntityBoat drops three separate planks, then two sticks into worldObj.
 * Beta 1.7.3 client decompile: Entity, EntityWolf, EntityBoat (RetroMCP).
 * Drive real ticks with slot 0 absent at slot 1's coordinates, in BOTH
 * dimension assignments. Expected cactus health is 5 - one hit per tick.
 */
#include <assert.h>
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
#include "entity/entity.h"
#include "entity/combat.h"
#include "entity/entity_boat.h"
#include "entity/entity_monster.h"
#include "entity/entity_wolf.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "platform/audio.h"

static struct server_local s;
static int draining;
static unsigned failures;
static uint32_t next_id = 100;
static unsigned steps;
static uint8_t step_world;
static uint32_t step_generation;

static void observe(const struct client_rpc* call, void* unused) {
	(void)unused;
	if(call->type == CRPC_SOUND
	   && call->payload.sound.sound_id == audio_block_step_sound_id(BLOCK_SAND)) {
		steps++;
		step_world = call->world_index;
		step_generation = call->world_generation;
	}
}

#define CHECK(cond, label) do { \
	if(!(cond)) { fprintf(stderr, "FAIL slot1 dimension %d: %s\n", \
		s.worlds[1].dimension, label); failures++; } \
} while(0)

static void* drain(void* unused) {
	(void)unused;
	while(__atomic_load_n(&draining, __ATOMIC_ACQUIRE)) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(struct server_world* w, int x, int y, int z, uint8_t type) {
	struct block_data b;
	if(!server_world_set_block(w, x, y, z, (struct block_data){.type = type})) {
		assert(server_world_get_block(w, x, y, z, &b));
		assert(b.type == type);
	}
}

static struct entity* make(uint8_t kind, uint8_t mob, struct server_world* w,
	float x, float y, float z) {
	struct entity* e = calloc(1, sizeof *e);
	assert(e);
	uint32_t id = next_id++;
	if(kind == ENTITY_ITEM) {
		entity_item(id, e, true, w,
			(struct item_data){.id = BLOCK_STONE, .count = 1});
		e->data.item.pickup_delay = 1000;
	} else if(kind == ENTITY_BOAT) {
		entity_boat(id, e, true, w);
	} else {
		entity_monster(id, e, true, w, mob);
	}
	e->teleport(e, (vec3){x, y, z});
	glm_vec3_zero(e->vel);
	*dict_entity_safe_get(s.entities, id) = e;
	return e;
}

static void erase(struct entity* e) {
	dict_entity_erase(s.entities, e->id);
	free(e);
}

static bool tick(struct entity* e) {
	struct server_world* prev = block_world_enter(&s,
		server_local_entity_world(&s, e));
	bool dead = e->tick_server(e, &s);
	block_world_leave(&s, prev);
	return dead;
}

static void contact(void) {
	struct server_world* w = &s.worlds[1];
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 0, 0));
	set(w, 8, 101, 8, BLOCK_CACTUS);
	struct entity* item = make(ENTITY_ITEM, 0, w, 7.9375F, 101.125F, 8.5F);
	CHECK(!tick(item) && item->health == 4, "real item tick reaches own cactus");
	erase(item);
	struct entity* pig = make(ENTITY_MONSTER, MONSTER_PIG, w,
		7.6125F, 101.0F, 8.5F);
	int hp = pig->health;
	CHECK(!tick(pig) && pig->health == hp - 1,
		"real mob tick reaches own cactus");
	erase(pig);
	set(w, 8, 101, 8, BLOCK_AIR);
	set(w, 6, 101, 6, BLOCK_COBWEB);
	item = make(ENTITY_ITEM, 0, w, 6.5F, 101.125F, 6.5F);
	CHECK(!tick(item) && item->in_web, "real item tick reaches own cobweb");
	erase(item);
	set(w, 6, 101, 6, BLOCK_AIR);
	set(w, 4, 101, 4, BLOCK_WOOD_PRESSURE_PLATE);
	item = make(ENTITY_ITEM, 0, w, 4.5F, 101.25F, 4.5F);
	CHECK(!tick(item), "item on wooden plate survives");
	struct block_data plate;
	assert(server_world_get_block(w, 4, 101, 4, &plate));
	CHECK(plate.metadata == 1, "real item tick presses own wooden plate");
	erase(item);
	set(w, 4, 101, 4, BLOCK_AIR);
	set(w, 4, 101, 4, BLOCK_STONE_PRESSURE_PLATE);
	pig = make(ENTITY_MONSTER, MONSTER_PIG, w, 4.5F, 101, 4.5F);
	CHECK(!tick(pig), "mob on stone plate survives");
	assert(server_world_get_block(w, 4, 101, 4, &plate));
	CHECK(plate.metadata == 1, "real mob tick presses own stone plate");
	erase(pig);
	set(w, 4, 101, 4, BLOCK_AIR);
}

static void boat_drops(void) {
	struct entity* boat = make(ENTITY_BOAT, 0, &s.worlds[1], 8.5F, 104, 8.5F);
	boat->data.boat.damage_taken = 41;
	CHECK(tick(boat), "destroyed boat tick removes boat");
	unsigned planks = 0, sticks = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e->type != ENTITY_ITEM)
			continue;
		CHECK(server_local_entity_world(&s, e) == &s.worlds[1], "boat drop owner");
		CHECK(e->data.item.item.count == 1 && e->data.item.pickup_delay == 10,
			"separate boat drops with Beta pickup delay");
		planks += e->data.item.item.id == BLOCK_PLANKS;
		sticks += e->data.item.item.id == ITEM_STICK;
	}
	CHECK(planks == 3 && sticks == 2, "Beta boat drop multiset");
	erase(boat);
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it); dict_entity_next(it))
		free(dict_entity_ref(it)->value);
	dict_entity_reset(s.entities);
}

static void wolf_owner(void) {
	s.player.has_pos = true;
	s.player.x = 13.5;
	s.player.y = 101 + PLAYER_EYE_HEIGHT;
	s.player.z = 8.5;
	struct entity* wolf = make(ENTITY_MONSTER, MONSTER_WOLF, &s.worlds[1],
		5.5F, 101, 8.5F);
	wolf->data.monster.wolf_flags = WOLF_FLAG_TAMED;
	snprintf(wolf->data.monster.wolf_owner,
		sizeof wolf->data.monster.wolf_owner, "%s", WOLF_LOCAL_PLAYER_NAME);
	CHECK(!tick(wolf), "wolf remains alive");
	CHECK(wolf_flag_get(wolf->data.monster.wolf_flags, WOLF_FLAG_SITTING),
		"owner in another world is absent, dry wolf sits");
	CHECK(wolf->data.monster.wolf_target_id == UINT32_MAX,
		"wolf cannot follow owner across worlds");
	erase(wolf);
	s.player.has_pos = false;
}

static void wolf_angry(void) {
	s.player.has_pos = true;
	s.player.x = 9.5;
	s.player.y = 101 + PLAYER_EYE_HEIGHT;
	s.player.z = 8.5;
	struct entity* wolf = make(ENTITY_MONSTER, MONSTER_WOLF, &s.worlds[1],
		4.5F, 101, 8.5F);
	wolf->data.monster.wolf_flags = WOLF_FLAG_ANGRY;
	CHECK(!tick(wolf), "angry wolf survives");
	CHECK(wolf->data.monster.wolf_target_id == UINT32_MAX
		&& wolf->data.monster.aggro_time == 0,
		"angry wolf cannot acquire foreign player");
	erase(wolf);
	struct server_player guest = {0};
	guest.world_slot = 1;
	guest.has_pos = guest.finished_loading = true;
	guest.health = MAX_PLAYER_HEALTH;
	guest.x = 7.5;
	guest.y = s.player.y;
	guest.z = 8.5;
	s.guest_actor = &guest;
	s.guest_gameplay = true;
	s.guest_generation = 1;
	assert(inventory_create(&guest.inventory, &inventory_logic_player, &s,
		&guest, INVENTORY_SIZE, 0, 0, 0));
	guest.active_inventory = &guest.inventory;
	wolf = make(ENTITY_MONSTER, MONSTER_WOLF, &s.worlds[1], 12.5F, 101, 8.5F);
	wolf->data.monster.wolf_flags = WOLF_FLAG_ANGRY;
	CHECK(!tick(wolf), "angry wolf with guest survives");
	CHECK(wolf->data.monster.target_actor.slot == ACTOR_SLOT_GUEST
		&& wolf->data.monster.target_actor.generation == s.guest_generation,
		"angry wolf acquires own-world guest over closer foreign host");
	CHECK(!tick(wolf) && wolf->data.monster.target_actor.slot == ACTOR_SLOT_GUEST,
		"angry wolf retains own-world guest identity");
	/* EntityCreature.updatePlayerActionState:19-33 retains a live reference;
	 * EntityWolf inherits it, even after the player's world changes. */
	guest.world_slot = 0;
	CHECK(!tick(wolf) && wolf->data.monster.target_actor.slot == ACTOR_SLOT_GUEST
		&& wolf->data.monster.wolf_target_id == 0
		&& wolf->data.monster.aggro_time > 0,
		"angry wolf retains live guest after world COMMIT");
	erase(wolf);
	guest.world_slot = 1;
	/* EntityWolf.findPlayerToAttack:291 -> closest player within 16.0D;
	 * World.getClosestPlayer:2173 uses STRICT squared distance < 16*16. */
	for(int role = 0; role < 2; role++) {
		struct server_player* candidate = role ? &s.player : &guest;
		s.player.world_slot = role ? 1 : 0;
		guest.world_slot = role ? 0 : 1;
		for(int range = 0; range < 3; range++) {
			candidate->x = 8.5 + (range == 0 ? 60 : range == 1 ? 16 : 15);
			candidate->y = 101; /* Equal Beta posY isolates the strict boundary. */
			candidate->z = 8.5;
			wolf = make(ENTITY_MONSTER, MONSTER_WOLF, &s.worlds[1],
				8.5F, 101, 8.5F);
			wolf->data.monster.wolf_flags = WOLF_FLAG_ANGRY;
			CHECK(!tick(wolf), "angry wolf range tick survives");
			CHECK((wolf->data.monster.wolf_target_id == 0) == (range == 2),
				"angry wolf fresh host/guest acquisition strictly within 16 blocks");
			erase(wolf);
		}
	}
	s.player.world_slot = 0;
	inventory_destroy(&guest.inventory, &guest);
	s.guest_actor = NULL;
	s.guest_gameplay = false;
	s.guest_generation = 0;
	s.player.has_pos = false;
}

static void wolf_queries(void) {
	struct entity* wolf = make(ENTITY_MONSTER, MONSTER_WOLF, &s.worlds[1],
		8.5F, 101, 8.5F);
	struct entity* foreign = make(ENTITY_MONSTER, MONSTER_WOLF, &s.worlds[0],
		9.5F, 101, 8.5F);
	struct entity* pack = make(ENTITY_MONSTER, MONSTER_WOLF, &s.worlds[1],
		9.5F, 101, 8.5F);
	CHECK(entity_wolf_attack_from(wolf, &s, 1, 0, true, false, false,
		ACTOR_IDENT_HOST) == MONSTER_DAMAGE_FRESH, "wolf attack accepted");
	CHECK(!wolf_flag_get(foreign->data.monster.wolf_flags, WOLF_FLAG_ANGRY),
		"pack retaliation excludes another world");
	CHECK(wolf_flag_get(pack->data.monster.wolf_flags, WOLF_FLAG_ANGRY),
		"pack retaliation includes own world");
	erase(foreign);
	erase(pack);
	struct entity* sheep = make(ENTITY_MONSTER, MONSTER_SHEEP, &s.worlds[0],
		9.5F, 101, 8.5F);
	wolf->data.monster.wolf_target_id = sheep->id;
	CHECK(entity_wolf_target(wolf, &s) == NULL, "foreign wolf target invalidates");
	wolf->data.monster.wolf_flags = 0;
	wolf->data.monster.wolf_target_id = UINT32_MAX;
	/* Exercise acquisition with a deterministic passing 1-in-100 roll.
	 * This searches INPUT seeds, not output goldens. No eligible sheep means
	 * no second random draw (EntityWolf.updatePlayerActionState). */
	for(int seed = 0; ; seed++) {
		jrand_set_seed(&wolf->gameplay_rng, seed);
		struct java_random probe = wolf->gameplay_rng;
		if(jrand_next_int_bound(&probe, 100) == 0)
			break;
	}
	struct java_random expected = wolf->gameplay_rng;
	(void)jrand_next_int_bound(&expected, 100);
	entity_wolf_try_find_sheep(wolf, &s);
	CHECK(wolf->data.monster.wolf_target_id == UINT32_MAX,
		"sheep acquisition excludes another world");
	CHECK(wolf->gameplay_rng.seed == expected.seed,
		"no selection RNG draw for foreign sheep");
	/* Force the foreign sheep FIRST in dictionary iteration. With one eligible
	 * sheep, nextInt(1) picks zero; removing only the selection-loop world
	 * filter must then select the foreign sheep (not survive by iteration luck). */
	struct entity* own = make(ENTITY_MONSTER, MONSTER_SHEEP, &s.worlds[1],
		9.5F, 101, 8.5F);
	dict_entity_it_t it;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* first = dict_entity_ref(it)->value;
		if(first != sheep && first != own)
			continue;
		if(first == own) {
			entity_bind_world(sheep, &s.worlds[1]);
			entity_bind_world(own, &s.worlds[0]);
			struct entity* swap = sheep;
			sheep = own;
			own = swap;
		}
		break;
	}
	for(int seed = 0; ; seed++) {
		jrand_set_seed(&wolf->gameplay_rng, seed);
		struct java_random probe = wolf->gameplay_rng;
		if(jrand_next_int_bound(&probe, 100) == 0)
			break;
	}
	expected = wolf->gameplay_rng;
	(void)jrand_next_int_bound(&expected, 100);
	(void)jrand_next_int_bound(&expected, 1);
	entity_wolf_try_find_sheep(wolf, &s);
	CHECK(wolf->data.monster.wolf_target_id == own->id,
		"mixed-world sheep selection chooses own sheep after foreign sheep");
	CHECK(wolf->gameplay_rng.seed == expected.seed,
		"mixed-world sheep selection draws nextInt(1)");
	erase(own);
	erase(sheep);
	/* Teleport queries use the wolf's world even without an ambient dispatch.
	 * First ring site is (6,100,6): solid below, two non-normal cubes above. */
	CHECK(entity_wolf_try_teleport_to_owner(wolf, &s, 8.5F, 101, 8.5F),
		"wolf teleport queries own terrain outside dispatch");
	CHECK(wolf->pos[0] == 6.5F && wolf->pos[1] == 101 && wolf->pos[2] == 6.5F,
		"Beta teleport ring scan order");
	erase(wolf);
}

static void footsteps(void) {
	static struct server_player guest;
	memset(&guest, 0, sizeof guest);
	s.player.x = 7.5;
	s.player.y = 101 + PLAYER_EYE_HEIGHT;
	s.player.z = 7.5;
	s.player.has_pos = true;
	guest.x = 7.5;
	guest.y = s.player.y;
	guest.z = 7.5;
	guest.has_pos = true;
	guest.finished_loading = true;
	guest.health = MAX_PLAYER_HEALTH;
	guest.world_slot = 1;
	/* No guest, attached-only guest, then gameplay-admitted guest. */
	for(int state = 0; state < 3; state++) {
		s.guest_actor = state ? &guest : NULL;
		s.guest_gameplay = state == 2;
		s.guest_generation = state ? 1 : 0;
		struct entity* pig = make(ENTITY_MONSTER, MONSTER_PIG, &s.worlds[1],
			7.5F, 101, 7.5F);
		/* One pending integer crossing in the production odometer. */
		pig->data.monster.walk_dist = 2;
		pig->data.monster.walk_next_step = 1;
		unsigned before = steps;
		CHECK(!tick(pig), "footstep mob tick survives");
		CHECK(steps == before + (unsigned)(state == 2),
			"footstep cull considers only own-world admitted listeners");
		if(state == 2)
			CHECK(step_world == 1 && step_generation == s.worlds[1].generation,
				"mob footstep reads and publishes own world");
		erase(pig);
	}
	s.guest_actor = NULL;
	s.guest_gameplay = false;
	s.guest_generation = 0;
	s.player.has_pos = false;
}

static uint64_t cells_hash(struct server_world* w) {
	uint64_t h = UINT64_C(14695981039346656037);
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++)
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				struct block_data b;
				assert(server_world_get_block(w, x, y, z, &b));
				uint32_t v = ((uint32_t)b.type << 24)
					| ((uint32_t)b.metadata << 16)
					| ((uint32_t)b.sky_light << 8) | b.torch_light;
				for(int k = 0; k < 4; k++) {
					h ^= (v >> (k * 8)) & 255;
					h *= UINT64_C(1099511628211);
				}
			}
	return h;
}

static void corpse_water(void) {
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	set(&s.worlds[0], 8, 102, 8, BLOCK_WATER_STILL);
	uint64_t before = cells_hash(&s.worlds[0]);
	struct entity* corpse = make(ENTITY_MONSTER, MONSTER_PIG, &s.worlds[1],
		8.5F, 103, 8.5F);
	corpse->health = 0;
	corpse->data.monster.death_time = 1;
	corpse->data.monster.fall_distance = 5;
	corpse->data.monster.motion_d.motion[1] = -0.25;
	mob_motion_mirror(corpse, &corpse->data.monster.motion_d);
	CHECK(!tick(corpse), "corpse motion tick continues");
	CHECK(corpse->data.monster.fall_distance > 5,
		"foreign water cannot reset corpse fall distance");
	CHECK(cells_hash(&s.worlds[0]) == before, "foreign cells byte-identical");
	erase(corpse);
	set(&s.worlds[0], 8, 102, 8, BLOCK_AIR);
	set(&s.worlds[1], 8, 102, 8, BLOCK_WATER_STILL);
	before = cells_hash(&s.worlds[0]);
	corpse = make(ENTITY_MONSTER, MONSTER_PIG, &s.worlds[1], 8.5F, 103, 8.5F);
	corpse->health = 0;
	corpse->data.monster.death_time = 1;
	corpse->data.monster.fall_distance = 5;
	corpse->data.monster.motion_d.motion[1] = -0.25;
	mob_motion_mirror(corpse, &corpse->data.monster.motion_d);
	CHECK(!tick(corpse) && corpse->data.monster.fall_distance == 0,
		"own post-move foot water resets corpse fall distance");
	CHECK(cells_hash(&s.worlds[0]) == before, "own water leaves foreign cells intact");
	erase(corpse);
	set(&s.worlds[1], 8, 102, 8, BLOCK_AIR);
	set(&s.worlds[0], 8, 100, 8, BLOCK_STONE);
	before = cells_hash(&s.worlds[0]);
	corpse = make(ENTITY_MONSTER, MONSTER_PIG, &s.worlds[1], 8.5F, 101, 8.5F);
	corpse->health = 0;
	corpse->data.monster.death_time = 1;
	corpse->data.monster.fall_distance = 5;
	corpse->data.monster.motion_d.motion[1] = -0.25;
	mob_motion_mirror(corpse, &corpse->data.monster.motion_d);
	unsigned sound_before = steps;
	CHECK(!tick(corpse) && corpse->on_ground, "corpse lands on own sand");
	CHECK(steps == sound_before + 1, "corpse landing thud uses own block material");
	CHECK(cells_hash(&s.worlds[0]) == before, "landing leaves foreign cells intact");
	erase(corpse);
}

static void sp_host_slot1(void) {
	/* Reachable after host portal COMMIT (server_transfer.c:commit), with
	 * split OFF. Budget activation explicitly includes player.world_slot != 0;
	 * server_local_update still ticks slot 0 without a local listener. */
	assert(!s.split_enabled && !s.guest_actor && !s.guest_gameplay);
	s.player.world_slot = 1;
	s.player.has_pos = true;
	s.player.x = s.player.z = 7.5;
	s.player.y = 101 + PLAYER_EYE_HEIGHT;
	set(&s.worlds[0], 7, 100, 7, BLOCK_SAND);
	for(int y = 101; y <= 106; y++)
		set(&s.worlds[0], 7, y, 7, BLOCK_AIR);
	for(unsigned slot = 0; slot < 2; slot++) {
		struct entity* wolf = make(ENTITY_MONSTER, MONSTER_WOLF, &s.worlds[slot],
			7.5F, 101, 7.5F);
		wolf->data.monster.wolf_flags = WOLF_FLAG_TAMED;
		snprintf(wolf->data.monster.wolf_owner,
			sizeof wolf->data.monster.wolf_owner, "%s", WOLF_LOCAL_PLAYER_NAME);
		CHECK(!tick(wolf), "SP transferred host wolf tick survives");
		CHECK(wolf_flag_get(wolf->data.monster.wolf_flags, WOLF_FLAG_SITTING)
			== (slot == 0), "SP wolf sits only in world without its owner");
		erase(wolf);
		struct entity* pig = make(ENTITY_MONSTER, MONSTER_PIG, &s.worlds[slot],
			7.5F, 101, 7.5F);
		pig->data.monster.walk_dist = 2;
		pig->data.monster.walk_next_step = 1;
		unsigned before = steps;
		CHECK(!tick(pig), "SP slot 0 still ticks after host transfer");
		CHECK(steps == before + (unsigned)(slot == 1),
			"SP footsteps audible only in transferred host world");
		erase(pig);
	}
	s.player.world_slot = 0;
	s.player.has_pos = false;
}

static void run(int dim0, int dim1) {
	char fixture[128];
	test_fixture_dir(dim1 == WORLD_DIM_NETHER ? "test_entity_world_nether"
		: "test_entity_world_overworld", fixture, sizeof fixture);
	MKDIR(fixture);
	MKDIR(test_fixture_join(fixture, "/region"));
	MKDIR(test_fixture_join(fixture, "/DIM-1"));
	MKDIR(test_fixture_join(fixture, "/DIM-1/region"));
	string_t level;
	string_init_set_str(level, fixture);
	memset(&s, 0, sizeof s);
	dict_entity_init(s.entities);
	s.rand_src.seed = 12345;
	s.entity_rng.seed = 54321;
	s.scheduled_tick_budget = 128;
	s.player.health = MAX_PLAYER_HEALTH;
	server_world_create(&s.worlds[0], level, dim0, 4242, true);
	server_world_create(&s.worlds[1], level, dim1, 4242, true);
	for(unsigned i = 0; i < 2; i++) {
		s.worlds[i].world_index = i;
		s.worlds[i].session = &s;
	}
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
		&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[1], 0, 0, &sc) && sc);
	for(int x = 2; x <= 14; x++)
		for(int z = 2; z <= 14; z++) {
			set(&s.worlds[1], x, 100, z, BLOCK_SAND);
			for(int y = 101; y <= 106; y++)
				set(&s.worlds[1], x, y, z, BLOCK_AIR);
		}
	unsigned before = failures;
	contact();
	boat_drops();
	wolf_owner();
	wolf_angry();
	wolf_queries();
	footsteps();
	CHECK(dict_server_chunks_size(s.worlds[0].chunks) == 0,
		"slot 0 residency unchanged");
	corpse_water();
	sp_host_slot1();
	inventory_destroy(&s.player.inventory, &s.player);
	server_world_discard(&s.worlds[1]);
	server_world_discard(&s.worlds[0]);
	dict_entity_clear(s.entities);
	string_clear(level);
	test_fixture_dir_cleanup(fixture);
	printf("slot0 %d / slot1 %d: %s\n", dim0, dim1,
		failures == before ? "PASS" : "FAIL");
}

int main(void) {
	assert(clin_init());
	assert(svin_init());
	blocks_init();
	items_init();
	clin_test_set_rpc_observer(observe, NULL);
	__atomic_store_n(&draining, 1, __ATOMIC_RELEASE);
	struct thread worker;
	assert(thread_create(&worker, drain, NULL, 64));
	run(WORLD_DIM_OVERWORLD, WORLD_DIM_NETHER);
	run(WORLD_DIM_NETHER, WORLD_DIM_OVERWORLD);
	__atomic_store_n(&draining, 0, __ATOMIC_RELEASE);
	thread_join(&worker);
	printf("test_entity_world_routing: %s (%u failures)\n",
		failures ? "FAIL" : "PASS", failures);
	return failures ? 1 : 0;
}
