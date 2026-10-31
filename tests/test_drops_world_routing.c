/* DROPS-WORLD: exercise real death, chicken tick and ENTITY_USE shear sites.
 * Beta b1.7.3 RetroMCP src_original/net/minecraft/src/:
 * EntityLiving.java:407-433 onDeath -> dropFewItems -> dropItem;
 * Entity.java:832-844 dropItem -> entityDropItem constructs AND joins using
 * this.worldObj (pickup delay 10). EntitySheep.interact uses that same route.
 * Slot-0 TRACE is a preimage/candidate equivalence tape, not a Beta golden.
 * Arena chunks are superflat in both slots; dimensions are assigned after
 * loading, so this tests ownership independently of terrain generation. */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity_monster.h"
#include "entity/combat.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "platform/thread.h"

static struct server_local s;
static int draining;
static void* drain(void* unused) {
	(void)unused;
	while(__atomic_load_n(&draining, __ATOMIC_ACQUIRE)) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void field(const char* label, const void* p, size_t n) {
	const unsigned char* bytes = p;
	printf(" %s=", label);
	for(size_t i = 0; i < n; ++i)
		printf("%02x", bytes[i]);
}
#define FIELD(p, member) field(#member, &(p)->member, sizeof((p)->member))
static void rng_trace(const struct java_random* r) {
	FIELD(r, seed); FIELD(r, have_next_gaussian); FIELD(r, next_gaussian_cached);
}

struct drop_case { const char* name; int mob; uint16_t item; };
static const struct drop_case cases[] = {
	{"cow", MONSTER_COW, ITEM_LEATHER},
	{"chicken", MONSTER_CHICKEN, ITEM_FEATHER},
	{"pig_zombie", MONSTER_PIG_ZOMBIE, ITEM_PORKCHOP_COOKED},
	{"ghast", MONSTER_GHAST, ITEM_GUNPOWDER},
	{"spider", MONSTER_SPIDER, ITEM_STRING},
	{"sheep", MONSTER_SHEEP, BLOCK_WOOL},
	{"skeleton", MONSTER_SKELETON, ITEM_BONE},
	{"creeper", MONSTER_CREEPER, ITEM_RECORD_13},
	{"egg", MONSTER_CHICKEN, ITEM_EGG},
	{"shear", MONSTER_SHEEP, BLOCK_WOOL},
	{"pig", MONSTER_PIG, ITEM_PORKCHOP},
	{"pig_burning", MONSTER_PIG, ITEM_PORKCHOP_COOKED},
	{"squid", MONSTER_SQUID, ITEM_DYE},
	{"zombie", MONSTER_ZOMBIE, ITEM_FEATHER},
	{"sand", MONSTER_COW, BLOCK_SAND},
	{"gravel", MONSTER_COW, BLOCK_GRAVEL},
	{"sand_timeout", MONSTER_COW, BLOCK_SAND},
	{"leaves", MONSTER_COW, BLOCK_LEAVES},
	{"unloaded", MONSTER_COW, ITEM_LEATHER},
	{"stale", MONSTER_COW, ITEM_LEATHER},
};

int main(int argc, char** argv) {
	assert(argc == 3);
	unsigned slot = (unsigned)strtoul(argv[2], NULL, 10);
	assert(slot < 2);
	const struct drop_case* c = NULL;
	for(size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i)
		if(!strcmp(argv[1], cases[i].name)) c = &cases[i];
	assert(c);
	bool egg = !strcmp(c->name, "egg"), shear = !strcmp(c->name, "shear");
	bool falling = !strcmp(c->name, "sand") || !strcmp(c->name, "gravel")
		|| !strcmp(c->name, "sand_timeout");
	bool leaves = !strcmp(c->name, "leaves");
	bool unloaded = !strcmp(c->name, "unloaded");
	bool stale = !strcmp(c->name, "stale");
	bool no_drop = unloaded || stale;
	char path[2][256];
	assert(clin_init() && svin_init());
	struct thread consumer;
	__atomic_store_n(&draining, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, drain, NULL, 64));
	blocks_init(); items_init();
	dict_entity_init(s.entities);
	s.next_entity_id = 100;
	for(unsigned i = 0; i < 2; ++i) {
		test_fixture_dir(i ? "drops_world_1" : "drops_world_0", path[i], sizeof path[i]);
		string_t level;
		string_init_set_str(level, path[i]);
		server_world_create(&s.worlds[i], level, WORLD_DIM_OVERWORLD, 4242, true);
		string_clear(level);
		s.worlds[i].session = &s;
		s.worlds[i].world_index = (uint8_t)i;
		struct server_chunk* chunk = NULL;
		assert(server_world_load_chunk(&s.worlds[i], 0, 0, &chunk) && chunk);
	}
	/* Test slot-1 Nether hostiles and slot-1 Overworld animals. */
	bool nether = c->mob == MONSTER_PIG_ZOMBIE || c->mob == MONSTER_GHAST;
	s.worlds[slot].dimension = nether ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
	s.worlds[1 - slot].dimension = nether ? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER;
	struct server_world* w = &s.worlds[slot];
	struct entity* mob = server_local_spawn_monster_w(w, (vec3){8.5F, 4.0F, 8.5F}, c->mob);
	assert(mob && server_local_entity_world(&s, mob) == w);
	/* Find a reproducible positive generic AND bone roll, independently of
	 * production. This ensures every targeted site actually emits an item. */
	struct java_random expected;
	int count, bones;
	for(int64_t seed = 0;; ++seed) {
		jrand_set_seed(&mob->gameplay_rng, seed);
		expected = mob->gameplay_rng;
		count = jrand_next_int_bound(&expected, 3);
		bones = jrand_next_int_bound(&expected, 3);
		if(count > 0 && bones > 0) break;
	}
	mob->data.monster.fleece_color = 11;
	mob->data.monster.egg_timer = 1;
	mob->data.monster.hit_by_skeleton_arrow = c->mob == MONSTER_CREEPER;
	if(!strcmp(c->name, "pig_burning")) mob->data.monster.fire_time = 20;
	s.rand_src.seed = 0xA1B2C3D4u;
	server_local_test_seed_math_random(1234567);
	server_local_sound_rng()->seed = 0xCAFEBABEu;
	s.player.has_pos = s.player.finished_loading = true;
	s.player.health = 20;
	s.player.x = 10.5; s.player.y = 4.0 + PLAYER_EYE_HEIGHT; s.player.z = 8.5;
	s.player.world_slot = slot;
	s.player.dimension = w->dimension;
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
		&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	inventory_set_hotbar(&s.player.inventory, 0);
	const uint32_t first = s.next_entity_id + 1;
	struct java_random owned_expected = mob->gameplay_rng;
	if(!egg && !falling && !leaves) {
		if(shear) {
			int wool = 2 + jrand_next_int_bound(&owned_expected, 3);
			for(int i = 0; i < 5 * wool; ++i) (void)jrand_next_float(&owned_expected);
		} else if(c->mob != MONSTER_SHEEP) {
			(void)jrand_next_int_bound(&owned_expected, 3);
			if(c->mob == MONSTER_SKELETON) (void)jrand_next_int_bound(&owned_expected, 3);
			if(c->mob == MONSTER_CREEPER) (void)jrand_next_int_bound(&owned_expected, 2);
		}
	}
	printf("TRACE %s slot=%u BEFORE", c->name, slot);
	rng_trace(&mob->gameplay_rng); FIELD(&s, rand_src.seed);
	putchar('\n');
	if(no_drop) {
		/* Stamped slot 0 now obeys slot 1's lifetime rule: no fallback
		 * into an unloaded world or a successor generation. This deliberately
		 * differs from the pre-DROPS-WORLD legacy slot-0 drop route. */
		uint64_t generation = w->generation;
		if(unloaded) server_world_discard(w);
		else ++w->generation;
		assert(!server_local_entity_world(&s, mob));
		struct random_gen world_rng = s.rand_src;
		struct server_local_math_random_state before, after;
		server_local_math_random_capture(&before);
		mob->health = 0;
		entity_monster_begin_death(mob, &s, mob->pos);
		server_local_math_random_capture(&after);
		assert(s.next_entity_id + 1 == first);
		assert(!memcmp(&world_rng, &s.rand_src, sizeof world_rng));
		assert(before.initialized == after.initialized
			&& before.rng.seed == after.rng.seed
			&& before.rng.have_next_gaussian == after.rng.have_next_gaussian
			&& before.rng.next_gaussian_cached == after.rng.next_gaussian_cached);
		if(stale) w->generation = generation;
	} else if(falling) {
		/* RetroMCP b1.7.3 EntityFallingSand.java:62-67: failed placement
		 * and fallTime > 100 dropItem(blockID, 1). Entity.java:840-844
		 * constructs and joins in this.worldObj, pickup delay 10. */
		struct server_world* prev = block_world_enter(&s, w);
		bool timeout = !strcmp(c->name, "sand_timeout");
		if(!timeout) assert(server_world_set_block(w, 8, 4, 8,
			(struct block_data){.type = BLOCK_WATER_STILL}));
		struct entity* faller = server_local_spawn_falling_sand(&s,
			(vec3){8.5F, timeout ? 20.0F : 5.0F, 8.5F}, c->item);
		assert(faller);
		if(timeout) faller->data.falling_sand.fall_time = 100;
		bool dead = false;
		for(int tick = 0; tick < 40 && !dead; ++tick)
			dead = faller->tick_server(faller, &s);
		assert(dead);
		block_world_leave(&s, prev);
	} else if(leaves) {
		/* RetroMCP b1.7.3 BlockLeaves.java:159-164 harvestBlock:
		 * shears -> dropBlockAsItem_do(var1, ..., leaves, 1, metadata & 3). */
		assert(server_world_set_block(w, 8, 4, 8,
			(struct block_data){.type = BLOCK_LEAVES, .metadata = 13}));
		s.player.inventory.items[INVENTORY_SLOT_HOTBAR] =
			(struct item_data){.id = ITEM_SHEARS, .count = 1};
		struct server_rpc rpc = {.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {.x = 8, .y = 4, .z = 8, .finished = true}};
		server_local_test_process_rpc(&s, &rpc);
		struct block_data at;
		assert(server_world_get_block(w, 8, 4, 8, &at) && at.type == BLOCK_AIR);
	} else if(shear) {
		s.player.inventory.items[INVENTORY_SLOT_HOTBAR] =
			(struct item_data){.id = ITEM_SHEARS, .count = 1};
		struct server_rpc rpc = {.type = SRPC_ENTITY_USE,
			.payload.entity_use.entity_id = mob->id};
		server_local_test_process_rpc(&s, &rpc);
		assert(mob->data.monster.sheared);
	} else if(egg) {
		struct server_world* prev = block_world_enter(&s, w);
		assert(!mob->tick_server(mob, &s));
		block_world_leave(&s, prev);
		assert(mob->data.monster.egg_timer >= 6000);
	} else {
		mob->health = 0;
		entity_monster_begin_death(mob, &s, mob->pos);
		uint32_t after = s.next_entity_id;
		struct java_random once = mob->gameplay_rng;
		entity_monster_begin_death(mob, &s, mob->pos);
		assert(after == s.next_entity_id && once.seed == mob->gameplay_rng.seed);
	}
	unsigned target_units = 0, nitems = 0, wrong = 0;
	if(!egg && !falling && !leaves) assert(mob->gameplay_rng.seed == owned_expected.seed);
	for(uint32_t id = first; id <= s.next_entity_id; ++id) {
		struct entity** pp = dict_entity_get(s.entities, id);
		assert(pp && *pp);
		if(falling && (*pp)->type == ENTITY_FALLING_SAND) continue;
		assert((*pp)->type == ENTITY_ITEM);
		struct entity* e = *pp;
		++nitems;
		if(server_local_entity_world(&s, e) != w || e->world != w
			|| e->world_slot != slot || e->world_generation != w->generation)
			++wrong;
		assert(e->added_to_chunk && e->chunk_coord_x == 0 && e->chunk_coord_z == 0);
		assert(e->data.item.pickup_delay == 10);
		if(e->data.item.item.id == c->item || (c->mob == MONSTER_CREEPER
			&& e->data.item.item.id == ITEM_RECORD_CAT))
			target_units += e->data.item.item.count;
		if(c->mob == MONSTER_SHEEP) assert(e->data.item.item.durability == 11);
		if(leaves) assert(e->data.item.item.durability == 1);
		printf("TRACE %s ITEM", c->name);
		FIELD(e, id); FIELD(e, type); FIELD(e, pos); FIELD(e, pos_old);
		FIELD(e, vel); FIELD(e, orient); FIELD(e, orient_old); FIELD(e, health);
		FIELD(e, on_server); FIELD(e, world_slot); FIELD(e, world_generation);
		FIELD(e, delay_destroy); FIELD(e, added_to_chunk);
		FIELD(e, chunk_coord_x); FIELD(e, chunk_coord_y); FIELD(e, chunk_coord_z);
		FIELD(e, chunk_list_order); FIELD(e, width); FIELD(e, height);
		FIELD(e, y_offset); FIELD(e, y_size); FIELD(e, step_height);
		FIELD(e, on_ground); FIELD(e, on_fire); FIELD(e, fall_distance);
		FIELD(e, collided_horizontally); FIELD(e, collided_vertically);
		FIELD(e, collided); FIELD(e, in_web); FIELD(e, network_pos);
		FIELD(e, data.item.item.id); FIELD(e, data.item.item.count);
		FIELD(e, data.item.item.durability); FIELD(e, data.item.age);
		FIELD(e, data.item.pickup_delay); FIELD(e, data.item.death_transaction);
		FIELD(e, data.item.death_sequence); FIELD(e, data.item.was_in_water);
		FIELD(e, data.item.persisted_fire); FIELD(e, data.item.persisted_air);
		FIELD(e, data.item.pickup_flight_active); FIELD(e, data.item.pickup_flight_age);
		FIELD(e, data.item.pickup_flight_start);
		rng_trace(&e->gameplay_rng); putchar('\n');
	}
	assert(no_drop ? nitems == 0 : nitems > 0 && target_units > 0);
	if(falling || leaves) assert(nitems == 1 && target_units == 1);
	if(!no_drop && !falling && !leaves && !egg && !shear && c->mob != MONSTER_CREEPER) {
		unsigned wanted = c->mob == MONSTER_SHEEP ? 1 :
			c->mob == MONSTER_SKELETON ? (unsigned)bones :
			c->mob == MONSTER_SQUID ? (unsigned)count + 1 : (unsigned)count;
		assert(target_units == wanted);
	}
	if(egg || c->mob == MONSTER_CREEPER) assert(target_units == 1);
	if(shear) assert(target_units == 2 + (unsigned)count);
	/* Query the real per-world section index as well as each entity's stamp. */
	for(unsigned i = 0; i < 2; ++i) {
		if(unloaded && i == slot) {
			assert(!server_local_world_live(&s.worlds[i]));
			continue;
		}
		assert(server_local_entity_query_local_w(&s, &s.worlds[i], 0, 0, 0, 0, 0, 15));
		unsigned indexed = 0;
		for(size_t j = 0; j < s.entity_query_count; ++j)
			if(!s.entity_query_refs[j].player
				&& s.entity_query_refs[j].entity->type == ENTITY_ITEM) ++indexed;
		if(indexed != (i == slot ? nitems : 0)) ++wrong;
	}
	printf("TRACE %s AFTER", c->name);
	rng_trace(&mob->gameplay_rng); FIELD(&s, rand_src.seed);
	FIELD(&s, rand_src.has_next_gaussian); FIELD(&s, rand_src.next_gaussian);
	FIELD(&s, next_entity_id); FIELD(mob, data.monster.egg_timer);
	FIELD(mob, data.monster.sheared);
	struct server_local_math_random_state math;
	server_local_math_random_capture(&math);
	rng_trace(&math.rng); FIELD(&math, initialized); putchar('\n');
	printf("%s %s slot=%u items=%u units=%u wrong_world=%u\n",
		wrong ? "FAIL" : "PASS", c->name, slot, nitems, target_units, wrong);
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) { free(dict_entity_ref(it)->value); dict_entity_next(it); }
	dict_entity_clear(s.entities);
	inventory_destroy(&s.player.inventory, &s.player);
	for(unsigned i = 0; i < 2; ++i) {
		if(!(unloaded && i == slot)) server_world_discard(&s.worlds[i]);
		test_fixture_dir_cleanup(path[i]);
	}
	__atomic_store_n(&draining, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	return wrong ? 1 : 0;
}
