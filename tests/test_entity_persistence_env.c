/* End-to-end server chunk eviction/reload and process-style restart coverage. */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
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

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "entity/mob_motion.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_fish.h"
#include "entity/entity_primed_tnt.h"
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/entity_archive.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "support/clin_drain.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_entity_persistence_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
static struct server_local s;
static bool g_drain_run = true;

bool server_local_test_checkpoint_entity_owners(
	struct server_local* s, const char* reason);
bool server_local_test_tick_entities(struct server_local* s);

static void* drain_entry(void* arg) {
	(void)arg;
	while(__atomic_load_n(&g_drain_run, __ATOMIC_ACQUIRE)) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

/* An erase save may commit its record and still return false when the
 * nonblocking entity-destroy batch cannot claim a client-inbox token.  In that
 * case production deliberately retains the chunk and entities for retry.  The
 * real client consumer normally makes progress on the next frame; this
 * headless fixture's consumer is an async thread, so a positive
 * save-and-retire expectation first proves the inbox empty
 * (test_drain_quiesce, quiesce-0926) and then makes ONE strict attempt. This
 * replaced a 500 x 2 ms retry whose success depended on host scheduling. */
static bool save_and_retire_chunk(w_coord_t x, w_coord_t z) {
	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	test_drain_quiesce();
	test_clin_drain();
	return server_world_save_chunk(&s.worlds[0], true, x, z);
}

static void free_archive_probe_chunk(struct server_chunk* chunk) {
	mem2_free(chunk->ids);
	mem2_free(chunk->metadata);
	mem2_free(chunk->lighting_sky);
	mem2_free(chunk->lighting_torch);
	mem2_free(chunk->heightmap);
}

static struct entity* alloc_entity(uint32_t id) {
	struct entity** pp = dict_entity_safe_get(s.entities, id);
	*pp = malloc(sizeof(**pp));
	assert(*pp);
	return *pp;
}

static void mark_item_rng_continuation(struct entity* e) {
	/* Beta saves no Entity.rand. This cursor/cache marker must survive a
	 * teleport but must NOT be restored from an item archive. */
	jrand_set_seed(&e->gameplay_rng, INT64_C(0x1A2B3C4D));
	(void)jrand_next_float(&e->gameplay_rng);
	e->gameplay_rng.have_next_gaussian = true;
	e->gameplay_rng.next_gaussian_cached = -0.625;
}

static struct entity* add_item(uint32_t id, vec3 pos) {
	struct entity* e = alloc_entity(id);
	/* Production item entities are built in raw malloc storage. The in-game
	 * click path calls onLeftClick/onRightClick whenever the crosshair hits an
	 * entity, so either callback retaining heap bytes is an immediate indirect
	 * call through garbage when the player swings at a floating drop. */
	memset(e, 0xBE, sizeof(*e));
	entity_item(id, e, true, &s.worlds[0],
				(struct item_data) {.id = 278, .durability = 1500, .count = 1});
	assert(e->onLeftClick == NULL && e->onRightClick == NULL
		   && "dropped-item click callbacks must be initialized, not heap bytes");
	struct java_random lifetime;
	entity_lifetime_rng_seed(&lifetime, s.worlds[0].seed, s.worlds[0].dimension, id);
	assert(e->id == id && e->gameplay_rng.seed == lifetime.seed);
	assert(!e->gameplay_rng.have_next_gaussian
		&& e->gameplay_rng.next_gaussian_cached == 0);
	mark_item_rng_continuation(e);
	struct java_random continuation = e->gameplay_rng;
	e->teleport(e, pos);
	assert(e->gameplay_rng.seed == continuation.seed
		&& e->gameplay_rng.have_next_gaussian == continuation.have_next_gaussian
		&& e->gameplay_rng.next_gaussian_cached == continuation.next_gaussian_cached);
	glm_vec3_copy((vec3) {0.1f, -0.2f, 0.3f}, e->vel);
	e->data.item.age = 321;
	e->y_size = 0.125f;
	e->fall_distance = 4.25f;
	return e;
}

static struct entity* add_monster(uint32_t id, vec3 pos) {
	struct entity* e = alloc_entity(id);
	memset(e, 0xBE, sizeof(*e));
	entity_monster(id, e, true, &s.worlds[0], MONSTER_SKELETON);
	struct java_random lifetime;
	entity_lifetime_rng_seed(&lifetime, s.worlds[0].seed, s.worlds[0].dimension, id);
	assert(e->gameplay_rng.seed && e->gameplay_rng.seed == lifetime.seed);
	assert(!e->gameplay_rng.have_next_gaussian
		&& e->gameplay_rng.next_gaussian_cached == 0);
	/* Beta saves no Random. Deliberately mark a runtime-only continuation
	 * state; actual reload must construct a fresh, cache-clean lifetime. */
	jrand_set_seed(&e->gameplay_rng, INT64_C(0xF00DCAFE));
	e->gameplay_rng.have_next_gaussian = true;
	e->gameplay_rng.next_gaussian_cached = 0.375F;
	assert(!e->data.monster.hit_by_skeleton_arrow);
	assert(!e->data.monster.death_loot_dropped);
	assert(e->data.monster.hurt_resist_time == 0
		   && e->data.monster.last_damage == 0);
	struct java_random continuation = e->gameplay_rng;
	e->teleport(e, pos);
	assert(e->gameplay_rng.seed == continuation.seed
		&& e->gameplay_rng.have_next_gaussian == continuation.have_next_gaussian
		&& e->gameplay_rng.next_gaussian_cached == continuation.next_gaussian_cached);
	e->health = 7;
	e->data.monster.fire_time = 44;
	e->data.monster.hurt_time = 5;
	e->data.monster.hurt_resist_time = 5;
	e->data.monster.last_damage = 4;
	e->data.monster.entity_age = 444;
	e->data.monster.aggro_time = 71;
	e->data.monster.fall_distance = 1.75f;
	e->fall_distance = 1.75f;
	e->y_size = 0.2f;
	e->ai_state = AI_CHASE;
	e->ai_timer = 12.5f;
	return e;
}

static struct entity* add_sheep(uint32_t id, vec3 pos) {
	struct entity* e = alloc_entity(id);
	entity_monster(id, e, true, &s.worlds[0], MONSTER_SHEEP);
	e->teleport(e, pos);
	e->data.monster.fleece_color = 6; /* explicit pink marker */
	return e;
}

static struct entity* add_arrow(uint32_t id, uint32_t owner_id, vec3 pos) {
	struct entity* e = alloc_entity(id);
	entity_arrow(id, e, true, &s.worlds[0], owner_id, ITEM_ARROW, ARROW_DAMAGE,
				 false);
	e->teleport(e, pos);
	glm_vec3_copy((vec3) {0.6f, -0.03f, 0.1f}, e->vel);
	e->data.arrow.age = 2000;
	e->data.arrow.from_skeleton = true;
	e->data.arrow.in_ground = true;
	e->data.arrow.stuck_ticks = 99;
	e->data.arrow.shake_ticks = 2;
	e->data.arrow.stuck_block_x = 12;
	e->data.arrow.stuck_block_y = 70;
	e->data.arrow.stuck_block_z = -8;
	e->data.arrow.stuck_block_type = BLOCK_STONE;
	e->data.arrow.stuck_block_meta = 3;
	return e;
}

static struct entity* add_cart(uint32_t id, vec3 pos) {
	struct entity* e = alloc_entity(id);
	/* The constructor must initialize occupied before deriving rightClickText.
	 * Poisoning the allocation makes an ordering regression deterministic even
	 * without UBSan's invalid-_Bool check. */
	memset(e, 0xBE, sizeof(*e));
	entity_minecart(id, e, true, &s.worlds[0]);
	assert(!e->data.minecart.occupied);
	assert(strcmp(e->rightClickText, "Ride") == 0);
	e->teleport(e, pos);
	glm_vec3_copy((vec3) {0.35f, 0.0f, 0.0f}, e->vel);
	e->data.minecart.speed = 0.35f;
	e->data.minecart.hx = 1;
	e->data.minecart.last_meta = 9;
	e->data.minecart.occupied = true;
	e->data.minecart.occupant_id = 101;
	e->rightClickText = "Dismount";
	e->y_size = 0.3f;
	e->fall_distance = 2.5f;
	return e;
}

static struct entity* add_primed_tnt(uint32_t id, vec3 pos, int fuse) {
	struct entity* e = alloc_entity(id);
	entity_primed_tnt(id, e, true, &s.worlds[0], fuse);
	e->teleport(e, pos);
	glm_vec3_copy((vec3) {0.02f, 0.2f, -0.01f}, e->vel);
	e->y_size = 0.4f;
	e->fall_distance = 3.5f;
	return e;
}

static void init_local_tables(void) {
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
}

static void create_world(string_t level) {
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 0;
}

static void free_all_entities(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_reset(s.entities);
}

struct restored_set {
	struct entity* item;
	struct entity* monster;
	struct entity* arrow;
	struct entity* cart;
	struct entity* sheep;
	struct entity* tnt;
};

static struct restored_set find_restored(bool allow_extra) {
	struct restored_set r = {0};
	size_t supported = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e->type == ENTITY_ITEM && e->data.item.item.id == 278) {
			r.item = e;
			supported++;
		} else if(e->type == ENTITY_MONSTER
				  && e->data.monster.id == MONSTER_SKELETON) {
			r.monster = e;
			supported++;
		} else if(e->type == ENTITY_MONSTER
				  && e->data.monster.id == MONSTER_SHEEP) {
			r.sheep = e;
			supported++;
		} else if(e->type == ENTITY_ARROW) {
			r.arrow = e;
			supported++;
		} else if(e->type == ENTITY_MINECART) {
			r.cart = e;
			supported++;
		} else if(e->type == ENTITY_PRIMED_TNT) {
			r.tnt = e;
			supported++;
		}
		dict_entity_next(it);
	}
	assert(supported == 6);
	assert(r.item && r.monster && r.arrow && r.cart && r.sheep && r.tnt);
	assert(allow_extra || dict_entity_size(s.entities) == 6);
	return r;
}

static void assert_state(struct restored_set r) {
	assert(r.item->data.item.item.id == 278);
	assert(r.item->data.item.item.durability == 1500);
	assert(r.item->data.item.age == 321);
	assert(fabsf(r.item->vel[2] - 0.3f) < 1.0e-5f);
	assert(fabsf(r.item->pos[1] - 72.625f) < 1.0e-5f);
	assert(r.item->y_size == 0.0f);
	assert(fabsf(r.item->fall_distance - 4.25f) < 1.0e-5f);
	struct java_random lifetime, continuation;
	entity_lifetime_rng_seed(&lifetime, s.worlds[0].seed, s.worlds[0].dimension, r.item->id);
	jrand_set_seed(&continuation, INT64_C(0x1A2B3C4D));
	(void)jrand_next_float(&continuation);
	assert(r.item->gameplay_rng.seed == lifetime.seed);
	assert(r.item->gameplay_rng.seed != continuation.seed);
	assert(!r.item->gameplay_rng.have_next_gaussian
		&& r.item->gameplay_rng.next_gaussian_cached == 0);

	assert(r.monster->health == 7);
	entity_lifetime_rng_seed(&lifetime, s.worlds[0].seed, s.worlds[0].dimension, r.monster->id);
	jrand_set_seed(&continuation, INT64_C(0xF00DCAFE));
	assert(r.monster->gameplay_rng.seed
		&& r.monster->gameplay_rng.seed == lifetime.seed);
	assert(r.monster->gameplay_rng.seed != continuation.seed);
	assert(!r.monster->gameplay_rng.have_next_gaussian
		&& r.monster->gameplay_rng.next_gaussian_cached == 0);
	assert(r.monster->data.monster.fire_time == 44);
	/* FH-18 (docs/fork_hack_audit_2026-08-21.md:192): the generic client-
	 * visible on_fire flag is not itself archived for a live server-side
	 * monster (see server_entity_archive_copy's comment) -- it must be
	 * re-derived from the just-restored fire_time on every chunk reload,
	 * end-to-end through the REAL server_world_save_chunk/load_chunk cycle
	 * (not just the entity_archive.c NBT layer -- see test_region_entities
	 * for that). Beta's own condition: fire > 0 [740c583 Entity.isBurning]. */
	assert(r.monster->on_fire);
	assert(r.monster->data.monster.hurt_time == 5);
	/* Beta persists HurtTime, but not heartsLife or field_9346_af. Reloading
	 * keeps the red flash while reopening the damage window. */
	assert(r.monster->data.monster.hurt_resist_time == 0);
	assert(r.monster->data.monster.last_damage == 0);
	assert(r.monster->data.monster.entity_age == 444);
	assert(r.monster->data.monster.aggro_time == 71);
	assert(fabsf(r.monster->data.monster.fall_distance - 1.75f) < 1.0e-5f);
	assert(fabsf(r.monster->fall_distance - 1.75f) < 1.0e-5f);
	assert(fabsf(r.monster->pos[1] - 70.3f) < 1.0e-5f);
	assert(r.monster->y_size == 0.0f);
	assert(r.monster->ai_state == AI_CHASE);

	assert(r.arrow->data.arrow.owner_id == r.monster->id);
	assert(r.arrow->data.arrow.from_skeleton);
	assert(r.arrow->data.arrow.in_ground);
	assert(r.arrow->data.arrow.stuck_ticks == 99);
	assert(r.arrow->data.arrow.age == 2000);
	assert(r.arrow->data.arrow.stuck_block_x == 12);
	assert(r.arrow->data.arrow.stuck_block_y == 70);
	assert(r.arrow->data.arrow.stuck_block_z == -8);
	assert(r.arrow->data.arrow.stuck_block_type == BLOCK_STONE);
	assert(r.arrow->data.arrow.stuck_block_meta == 3);
	assert(r.arrow->data.arrow.shake_ticks == 2);
	assert(r.sheep->data.monster.fleece_color == 6);

	assert(fabsf(r.cart->data.minecart.speed - 0.35f) < 1.0e-5f);
	assert(fabsf(r.cart->pos[1] - 68.3f) < 1.0e-5f);
	assert(r.cart->y_size == 0.0f);
	assert(fabsf(r.cart->fall_distance - 2.5f) < 1.0e-5f);
	assert(r.cart->data.minecart.hx == 1);
	assert(r.cart->data.minecart.last_meta == 9);
	assert(r.cart->data.minecart.occupied);
	assert(r.cart->data.minecart.occupant_id == r.monster->id);
	assert(strcmp(r.cart->rightClickText, "Dismount") == 0);
	assert(r.tnt->data.primed_tnt.fuse == 37
		   || r.tnt->data.primed_tnt.fuse == 36);
	assert(r.tnt->y_size == 0.0f);
	assert(fabsf(r.tnt->fall_distance - 3.5f) < 1.0e-5f);
	assert(r.tnt->vel[0] > 0.0f && r.tnt->vel[0] <= 0.02001f);
}

int main(void) {
	test_fixture_dir("test_entity_persistence_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));

	memset(&s, 0, sizeof(s));
	init_local_tables();
	dict_entity_init(s.entities);
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	create_world(level);
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);

	add_item(100, (vec3) {1.25f, 72.5f, 2.75f});
	add_monster(101, (vec3) {5.5f, 70.1f, 6.5f});
	add_arrow(102, 101, (vec3) {7.0f, 71.0f, 8.0f});
	add_cart(103, (vec3) {10.5f, 68.0f, 11.5f});
	add_sheep(104, (vec3) {12.5f, 70.0f, 13.5f});
	add_primed_tnt(105, (vec3) {14.5f, 64.5f, 15.5f}, 37);
	/* An independently seeded key in another chunk proves restore cannot
	 * overwrite an occupied id even when next_entity_id is stale. */
	struct entity* sentinel = add_item(5, (vec3) {20.5f, 72.0f, 2.5f});
	sentinel->data.item.item.id = ITEM_STICK;
	s.next_entity_id = 4;

	assert(save_and_retire_chunk(0, 0));
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 0, 0));
	assert(dict_entity_size(s.entities) == 1); /* only the chunk-1 sentinel */
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	assert(sc->persisted_entities_nonempty);
	struct restored_set first = find_restored(true);
	assert_state(first);
	assert(fabsf(first.tnt->pos[1] - 64.9f) < 1.0e-5f);
	assert(first.tnt->data.primed_tnt.fuse == 37);
	assert(!first.tnt->tick_server(first.tnt, &s));
	assert(first.tnt->data.primed_tnt.fuse == 36);
	const float moved_tnt_y = first.tnt->pos[1];
	assert(first.item->id >= 6 && first.monster->id >= 6);
	assert(first.item->id != 100 && first.monster->id != 101);
	printf("  eviction reload: state + relation remap + occupied-id skip OK\n");
	/* Mob motion units (Fable S1 F1): the restore path seeds the mob's
	 * authoritative doubles -- the position through the teleport seed, the
	 * saved Motion through the mutation API -- so the float mirror equals the
	 * doubles and the tick bracket counts no bypass. */
	{
		struct mob_motion* rmd = &first.monster->data.monster.motion_d;
		assert(rmd->valid);
		for(int k = 0; k < 3; k++) {
			assert((float)rmd->pos[k] == first.monster->pos[k]);
			assert((float)rmd->motion[k] == first.monster->vel[k]);
		}
		mob_motion_bypass_reset();
		mob_motion_tick_begin(first.monster, rmd);
		assert(mob_motion_bypass_count() == 0);
		printf("  restore path: mob motion doubles seeded, no bypass OK\n");
	}

	/* Restore is transactional: an allocation failure after one constructed
	 * record must roll that record back, leave the disk chunk authoritative, and
	 * permit a clean retry without duplicate or partially restored entities. */
	mark_item_rng_continuation(first.item);
	assert(save_and_retire_chunk(0, 0));
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 0, 0));
	assert(dict_entity_size(s.entities) == 1); /* chunk-1 sentinel only */
	server_local_test_fail_entity_restore_after(1);
	assert(!server_world_load_chunk(&s.worlds[0], 0, 0, &sc));
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 0, 0));
	assert(dict_entity_size(s.entities) == 1);
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	assert(sc->persisted_entities_nonempty);
	first = find_restored(true);
	assert_state(first);
	assert(fabsf(first.tnt->pos[1] - moved_tnt_y) < 1.0e-5f);
	assert(first.tnt->data.primed_tnt.fuse == 36);
	printf("  restore OOM: partial entities rolled back; retry clean OK\n");

	/* A process-style restart resets the dictionary/id counter, then reloads the
	 * same chunk from disk. This is also the lifecycle used on menu reload. */
	mark_item_rng_continuation(first.item);
	server_world_destroy(&s.worlds[0]);
	piston_moving_store_destroy(&s.piston_moving);
	free_all_entities();
	s.next_entity_id = 0;
	create_world(level);
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	assert(sc->persisted_entities_nonempty);
	struct restored_set restarted = find_restored(false);
	assert_state(restarted);
	assert(fabsf(restarted.tnt->pos[1] - moved_tnt_y) < 1.0e-5f);
	assert(restarted.tnt->data.primed_tnt.fuse == 36);
	assert(restarted.item->id > 0 && restarted.monster->id > 0);
	assert(restarted.item->id != restarted.monster->id);
	assert(restarted.arrow->id != restarted.cart->id);
	printf("  restart reload: six fresh noncolliding runtime ids OK\n");

	/* One staged build wrote a primed TNT as a metadata block plus an archive
	 * record. Restore must migrate that pair once: remove the old block, create a
	 * moving entity with the saved fuse, and never require the deleted side table. */
	assert(server_world_set_block(
		&s.worlds[0], 3, 80, 3,
		(struct block_data) {.type = BLOCK_TNT, .metadata = 1}));
	struct archived_entity legacy_tnt = {0};
	legacy_tnt.kind = ARCHIVED_ENTITY_PRIMED_TNT;
	legacy_tnt.pos[0] = 3.5f;
	legacy_tnt.pos[1] = 80.5f;
	legacy_tnt.pos[2] = 3.5f;
	legacy_tnt.data.primed_tnt = (struct archived_primed_tnt_state) {
		.x = 3,
		.y = 80,
		.z = 3,
		.fuse = 12,
		.legacy_block_backed = true,
	};
	assert(server_local_restore_chunk_entities(
		&s, &(struct chunk_entities) {.entities = &legacy_tnt, .count = 1}));
	struct block_data migrated_block;
	assert(server_world_get_block(&s.worlds[0], 3, 80, 3, &migrated_block));
	assert(migrated_block.type == BLOCK_AIR);
	uint32_t migrated_id = 0;
	dict_entity_it_t migrated_it;
	dict_entity_it(migrated_it, s.entities);
	while(!dict_entity_end_p(migrated_it)) {
		struct entity* e = dict_entity_ref(migrated_it)->value;
		if(e && e->type == ENTITY_PRIMED_TNT
		   && e->data.primed_tnt.fuse == 12) {
			migrated_id = e->id;
			break;
		}
		dict_entity_next(migrated_it);
	}
	assert(migrated_id != 0);
	struct entity** migrated_pp = dict_entity_get(s.entities, migrated_id);
	assert(migrated_pp && *migrated_pp);
	free(*migrated_pp);
	dict_entity_erase(s.entities, migrated_id);
	printf("  legacy block-backed TNT: migrated once to a moving entity OK\n");

	/* A relation to a record that fails restoration must not alias player id 0. */
	free_all_entities();
	struct archived_entity failed_records[2] = {0};
	failed_records[0].kind = ARCHIVED_ENTITY_MONSTER;
	failed_records[0].source_id = 900;
	failed_records[0].data.monster.monster_id = MONSTER_COUNT;
	failed_records[1].kind = ARCHIVED_ENTITY_ARROW;
	failed_records[1].source_id = 901;
	glm_vec3_copy((vec3) {7.0f, 71.0f, 8.0f}, failed_records[1].pos);
	failed_records[1].health = 1;
	failed_records[1].data.arrow.owner_source_id = 900;
	failed_records[1].data.arrow.item_id = ITEM_ARROW;
	failed_records[1].data.arrow.damage = ARROW_DAMAGE;
	server_local_restore_chunk_entities(
		&s, &(struct chunk_entities) {
			.entities = failed_records,
			.count = 2,
		});
	assert(dict_entity_size(s.entities) == 1);
	dict_entity_it_t failed_it;
	dict_entity_it(failed_it, s.entities);
	assert(!dict_entity_end_p(failed_it));
	struct entity* orphan_arrow = dict_entity_ref(failed_it)->value;
	assert(orphan_arrow->type == ENTITY_ARROW);
	assert(orphan_arrow->data.arrow.owner_id == UINT32_MAX);

	/* UINT32_MAX is the persisted orphan sentinel, not player id 0. Exercise a
	 * complete save/load after the failed-owner remap so the NBT codec must carry
	 * the high bit pattern rather than silently defaulting it to zero. */
	assert(save_and_retire_chunk(0, 0));
	assert(dict_entity_size(s.entities) == 0);
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	assert(sc->persisted_entities_nonempty);
	dict_entity_it(failed_it, s.entities);
	assert(!dict_entity_end_p(failed_it));
	orphan_arrow = dict_entity_ref(failed_it)->value;
	assert(orphan_arrow->type == ENTITY_ARROW);
	assert(orphan_arrow->data.arrow.owner_id == UINT32_MAX);
	printf("  failed-owner save/reload: orphan never aliases player id 0 OK\n");

	/* A historical block-side-table limit made 608 the archive ceiling. Real
	 * primed TNT is an ordinary entity, and a dense chain may put record 609 in
	 * the same chunk. Exercise the complete production gather -> region write ->
	 * eviction/release -> parse -> transactional restore lifecycle. */
	free_all_entities();
	enum { DENSE_TNT_COUNT = 609 };
	for(uint32_t i = 0; i < DENSE_TNT_COUNT; i++) {
		float x = 0.5f + (float)(i % CHUNK_SIZE);
		float z = 0.5f + (float)((i / CHUNK_SIZE) % CHUNK_SIZE);
		add_primed_tnt(UINT32_C(10000) + i, (vec3) {x, 70.5f, z},
					   10 + (int)(i % 20));
	}
	assert(dict_entity_size(s.entities) == DENSE_TNT_COUNT);
	assert(save_and_retire_chunk(0, 0));
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 0, 0));
	assert(dict_entity_size(s.entities) == 0);
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	assert(sc->persisted_entities_nonempty);
	assert(dict_entity_size(s.entities) == DENSE_TNT_COUNT);
	size_t restored_tnt = 0;
	dict_entity_it_t dense_it;
	dict_entity_it(dense_it, s.entities);
	while(!dict_entity_end_p(dense_it)) {
		struct entity* e = dict_entity_ref(dense_it)->value;
		assert(e && e->type == ENTITY_PRIMED_TNT);
		assert(e->data.primed_tnt.fuse >= 10
			   && e->data.primed_tnt.fuse <= 29);
		restored_tnt++;
		dict_entity_next(dense_it);
	}
	assert(restored_tnt == DENSE_TNT_COUNT);
	printf("  dense TNT: 609 entities save/evict/reload without truncation OK\n");

	/* F7(a) (weather-slice adversarial review): a charged creeper's
	 * "powered" flag must survive save/evict/reload. This is ALSO a standing
	 * regression guard for the entity_node_count/entity_wrap_count arena-
	 * budget bug the same review caught: an undercounted budget silently
	 * fails ANY monster save past the fixed node/wrap ceiling (not just
	 * creepers) once the arena fills -- see WEATHER_SLICE_NOTES.md's
	 * post-review section. If a future field is added to
	 * archived_monster_state without bumping the budget, this whole save
	 * silently fails and this test starts failing loudly instead of the bug
	 * hiding until a live save corrupts something. */
	free_all_entities();
	struct entity* creeper = alloc_entity(9001);
	memset(creeper, 0xBE, sizeof(*creeper));
	entity_monster(9001, creeper, true, &s.worlds[0], MONSTER_CREEPER);
	creeper->teleport(creeper, (vec3) {5.5f, 70.0f, 5.5f});
	creeper->data.monster.charged = true;
	/* Beta Entity.moveEntity:493-496 extinguishes to -fireResistance (-1);
	 * Entity.writeToNBT/readFromNBT:755,786 preserves signed Fire. */
	creeper->data.monster.fire_time = -1;
	assert(dict_entity_size(s.entities) == 1);

	/* Deterministically exercise the retry contract which triggered the
	 * original failure.  Stop the asynchronous consumer, fill the existing
	 * transport through its public nonblocking seam, and prove the refusal
	 * retains all live ownership.  STAGE22 (G7): an evicting save of a chunk
	 * with live entities checks the client inbox BEFORE writing, so a full
	 * inbox is a DEFERRAL -- nothing is committed (the old contract here,
	 * "refusal commits the archive record", was the committed-then-discarded
	 * write the real-Wii stage-22 failure came from) and it is not a save
	 * failure.  The committed record's fields are checked below at the save
	 * that does commit it. */
	__atomic_store_n(&g_drain_run, false, __ATOMIC_RELEASE);
	thread_join(&drain);
	bool inbox_saturated = false;
	for(unsigned admitted = 0; admitted < 64; admitted++) {
		struct client_rpc pressure = {
			.type = CRPC_ENTITY_DESTROY,
			.payload.entity_destroy.entity_id = UINT32_MAX,
		};
		if(!clin_try_rpc_send(&pressure)) {
			inbox_saturated = true;
			break;
		}
	}
	assert(inbox_saturated);
	uint32_t save_fail_before = region_save_failure_count();
	assert(!server_world_save_chunk(&s.worlds[0], true, 0, 0));
	assert(s.worlds[0].evict_retire_deferred
		   && "a full inbox defers the eviction (STAGE22)");
	assert(region_save_failure_count() == save_fail_before
		   && "a deferral is not a save failure");
	assert(server_world_is_chunk_loaded(&s.worlds[0], 0, 0));
	assert(dict_entity_size(s.entities) == 1);
	assert(server_world_region_count(&s.worlds[0]) == 1);
	{
		/* Nothing committed: no record names the creeper yet. */
		struct server_chunk prior_chunk = {0};
		struct chunk_entities prior_entities = {0};
		if(region_archive_get_chunk(server_world_region_at(&s.worlds[0], 0), 0, 0,
			   &prior_chunk, NULL, &prior_entities)) {
			for(size_t i = 0; i < prior_entities.count; i++)
				assert(!(prior_entities.entities[i].kind
							 == ARCHIVED_ENTITY_MONSTER
						 && prior_entities.entities[i].data.monster.monster_id
							 == MONSTER_CREEPER)
					   && "a deferred eviction commits no record");
			chunk_entities_free(&prior_entities);
			free_archive_probe_chunk(&prior_chunk);
		}
	}
	clin_drain_discard();
	__atomic_store_n(&g_drain_run, true, __ATOMIC_RELEASE);
	thread_create(&drain, drain_entry, NULL, 64);
	assert(save_and_retire_chunk(0, 0));
	/* The record the retry committed carries every archived field. */
	assert(server_world_region_count(&s.worlds[0]) == 1);
	struct server_chunk committed_chunk = {0};
	struct chunk_entities committed_entities = {0};
	assert(region_archive_get_chunk(server_world_region_at(&s.worlds[0], 0), 0, 0,
		&committed_chunk, NULL, &committed_entities));
	assert(committed_entities.count == 1);
	assert(committed_entities.entities[0].kind == ARCHIVED_ENTITY_MONSTER);
	assert(committed_entities.entities[0].data.monster.monster_id
		   == MONSTER_CREEPER);
	assert(committed_entities.entities[0].data.monster.charged);
	assert(committed_entities.entities[0].data.monster.fire_time == -1);
	chunk_entities_free(&committed_entities);
	free_archive_probe_chunk(&committed_chunk);
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 0, 0));
	assert(dict_entity_size(s.entities) == 0);
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	assert(sc->persisted_entities_nonempty);
	struct entity* reloaded_creeper = NULL;
	dict_entity_it_t creeper_it;
	dict_entity_it(creeper_it, s.entities);
	while(!dict_entity_end_p(creeper_it)) {
		struct entity* e = dict_entity_ref(creeper_it)->value;
		if(e && e->type == ENTITY_MONSTER && e->data.monster.id == MONSTER_CREEPER)
			reloaded_creeper = e;
		dict_entity_next(creeper_it);
	}
	assert(reloaded_creeper
		   && "a charged creeper must round-trip through save/evict/reload "
			  "at all (arena-budget regression guard)");
	assert(reloaded_creeper->data.monster.charged
		   && "\"powered\" must survive the NBT round-trip (entity_archive.c's "
			  "Beta-named \"powered\" tag)");
	assert(reloaded_creeper->data.monster.fire_time == -1
		   && "quenched mob Fire=-1 must survive save/evict/reload");
	printf("  charged-creeper archive round-trip: \"powered\" survives "
		   "save/evict/reload OK\n");

	/* Negative control: an UNcharged creeper must round-trip as uncharged --
	 * proves the actual boolean VALUE survives, not merely tag presence. */
	free_all_entities();
	struct entity* creeper2 = alloc_entity(9002);
	entity_monster(9002, creeper2, true, &s.worlds[0], MONSTER_CREEPER);
	creeper2->teleport(creeper2, (vec3) {8.5f, 70.0f, 8.5f});
	assert(!creeper2->data.monster.charged);
	assert(save_and_retire_chunk(0, 0));
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	struct entity* reloaded_creeper2 = NULL;
	dict_entity_it_t creeper_it2;
	dict_entity_it(creeper_it2, s.entities);
	while(!dict_entity_end_p(creeper_it2)) {
		struct entity* e = dict_entity_ref(creeper_it2)->value;
		if(e && e->type == ENTITY_MONSTER && e->data.monster.id == MONSTER_CREEPER)
			reloaded_creeper2 = e;
		dict_entity_next(creeper_it2);
	}
	assert(reloaded_creeper2 && !reloaded_creeper2->data.monster.charged
		   && "an UNcharged creeper must round-trip uncharged too (not just "
			  "tag presence)");
	printf("  uncharged-creeper archive round-trip: \"powered\" stays false "
		   "OK\n");

	/* Beta World.updateEntityWithOptionalForce does not tick an entity unless
	 * every chunk intersecting its position +/-32 blocks exists. This is also
	 * the ownership barrier: an item at the resident edge must not advance into
	 * an absent destination, lose added_to_chunk, and become impossible to
	 * checkpoint. Exercise the real entity pass, then the complete production
	 * save/evict/reload path; the tick must not load or generate a chunk. */
	free_all_entities();
	assert(server_world_is_chunk_loaded(&s.worlds[0], 0, 0));
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 1, 0));
	size_t edge_chunks_before = dict_server_chunks_size(s.worlds[0].chunks);
	struct entity* edge_item = add_item(
		9003, (vec3) {15.75f, 70.0f, 8.5f});
	edge_item->y_size = 0.0f;
	edge_item->data.item.age = 77;
	edge_item->health = 3;
	edge_item->data.item.persisted_fire = -1;
	edge_item->data.item.persisted_air = 300;
	glm_vec3_copy((vec3) {0.75f, 0.0f, 0.0f}, edge_item->vel);
	server_local_refresh_entity_membership(&s, edge_item);
	assert(edge_item->added_to_chunk && edge_item->chunk_coord_x == 0
		   && edge_item->chunk_coord_z == 0);
	const float edge_x = edge_item->pos[0];
	assert(server_local_test_tick_entities(&s));
	assert(edge_item->pos[0] == edge_x);
	assert(edge_item->data.item.age == 77);
	assert(edge_item->added_to_chunk && edge_item->chunk_coord_x == 0
		   && edge_item->chunk_coord_z == 0);
	assert(dict_server_chunks_size(s.worlds[0].chunks) == edge_chunks_before);
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 1, 0));
	assert(save_and_retire_chunk(0, 0));
	assert(dict_entity_size(s.entities) == 0);
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	assert(dict_entity_size(s.entities) == 1);
	dict_entity_it_t edge_it;
	dict_entity_it(edge_it, s.entities);
	assert(!dict_entity_end_p(edge_it));
	struct entity* reloaded_edge_item = dict_entity_ref(edge_it)->value;
	assert(reloaded_edge_item && reloaded_edge_item->type == ENTITY_ITEM);
	assert(reloaded_edge_item->data.item.age == 77);
	assert(reloaded_edge_item->health == 3);
	assert(reloaded_edge_item->data.item.persisted_fire == -1);
	assert(reloaded_edge_item->data.item.persisted_air == 300);
	assert(reloaded_edge_item->pos[0] == edge_x);
	assert(reloaded_edge_item->added_to_chunk
		   && reloaded_edge_item->chunk_coord_x == 0
		   && reloaded_edge_item->chunk_coord_z == 0);
	printf("  edge activation: incomplete Beta radius freezes owner; "
		   "save/evict/reload exact OK\n");

	/* EntityArrow.java:107-125 and World.updateEntityWithOptionalForce:
	 * an old airborne arrow still waits for the same chunk-residency gate. */
	free_all_entities();
	struct entity* edge_arrow = add_arrow(
		9005, 0, (vec3) {15.75f, 100.0f, 8.5f});
	edge_arrow->data.arrow.in_ground = false;
	edge_arrow->data.arrow.age = 2000;
	server_local_refresh_entity_membership(&s, edge_arrow);
	assert(edge_arrow->added_to_chunk);
	assert(server_local_test_tick_entities(&s));
	assert(edge_arrow->data.arrow.age == 2000);
	assert(edge_arrow->pos[0] == 15.75f);
	puts("  edge arrow: unloaded neighbor freezes old airborne arrow OK");

	/* Fishing hooks are intentionally transient across chunk/world unloads:
	 * Beta cannot relink their angler after NBT restore. A live hook must not
	 * poison the hard-checkpoint owner preflight, and the preflight itself must
	 * leave it intact in case a later durability stage fails. */
	free_all_entities();
	struct entity* fish = alloc_entity(9004);
	entity_fish(9004, fish, true, &s.worlds[0]);
	fish->teleport(fish, (vec3) {9.5f, 70.0f, 9.5f});
	s.player.fish_entity_id = fish->id;
	assert(server_local_test_checkpoint_entity_owners(&s, "fish_transient"));
	assert(dict_entity_get(s.entities, fish->id)
		   && *dict_entity_get(s.entities, fish->id) == fish);
	assert(s.player.fish_entity_id == fish->id);
	fish->teleport(fish, (vec3) {1600.5f, 70.0f, 1600.5f});
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 100, 100));
	assert(server_local_test_checkpoint_entity_owners(
		&s, "off_resident_fish_transient"));
	free(fish);
	dict_entity_erase(s.entities, s.player.fish_entity_id);
	s.player.fish_entity_id = 0;

	/* Negative controls: accepting the documented bobber transient must neither
	 * load/generate off-resident owner chunks nor turn unsupported/corrupt
	 * entities into ignored save loss. */
	struct entity* off_resident_item = add_item(
		9005, (vec3) {1600.5f, 70.0f, 1600.5f});
	assert(!server_local_test_checkpoint_entity_owners(
		&s, "off_resident_archivable"));
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 100, 100));
	free(off_resident_item);
	dict_entity_erase(s.entities, 9005);
	struct entity* invalid_tnt = add_primed_tnt(
		9006, (vec3) {10.5f, 70.0f, 10.5f}, TNT_FUSE_DEFAULT + 1);
	assert(!server_local_test_checkpoint_entity_owners(&s, "invalid_tnt"));
	free(invalid_tnt);
	dict_entity_erase(s.entities, 9006);
	printf("  owner preflight: transient bobber accepted intact; invalid TNT "
		   "still fails closed OK\n");

	server_world_destroy(&s.worlds[0]);
	piston_moving_store_destroy(&s.piston_moving);
	free_all_entities();
	dict_entity_clear(s.entities);
	string_clear(level);
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	__atomic_store_n(&g_drain_run, false, __ATOMIC_RELEASE);
	thread_join(&drain);
	printf("test_entity_persistence_env: PASS\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
