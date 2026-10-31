/*
	M4 "animals" slice end-to-end env test (the test_tnt_primed_env /
	test_entity_persistence_env harness pattern: threadless server, direct RPC
	processing, dict scans, real disk chunk save/load for archive round trips).

	Coverage (decompile 740c583 unless noted):
	  - SRPC_ENTITY_USE validation: out-of-reach (Beta NetServerHandler distSq <
	    36) and death-flop targets are rejected; an in-reach live target accepts.
	  - COW milking (EntityCow.interact): held empty bucket -> whole-slot
	    ITEM_MILK_BUCKET swap; other slots untouched; non-bucket held is a no-op.
	  - MILK bucket (ItemBucket isFull<0): use at any block -> empty bucket,
	    block untouched, nothing placed; not edible (no place/eat verb).
	  - SHEEP shear (EntitySheep.interact): sheared=true + 2+nextInt(3) wool of
	    the fleece colour + shears damageItem(1); second shear no-ops; death drop
	    suppressed when sheared (and present when not); sheared survives the real
	    disk archive round trip; the CRPC_ENTITY_MOVE sheared flag is emitted.
	  - DYE fallback (ItemDye useItemOnEntity): recolour ~damage&15 + consume 1;
	    same-colour consumes nothing.
	  - CHICKEN egg lay (EntityChicken.onLivingUpdate): the unrolled sentinel
	    rolls 6000..11999 on the first tick without laying; an expiring timer
	    lays EXACTLY at expiry and re-rolls; CaveWiiEggTimer round-trips.
	  - CAKE recipe (CraftingManager "AAA","BEB","CCC") + SlotCrafting container
	    items: crafting consumes the grid and returns 3 EMPTY buckets IN THE GRID.
	  - THROWN EGG: SRPC_USE_ITEM spawns ENTITY_EGG at 1.5 speed and consumes
	    one egg; an entity hit is a ZERO-damage standard-path hurt (hurt flash +
	    knockback + retaliation latch, health unchanged); a block hit despawns
	    the egg; the 1/8 + nested 1/32 chick rolls are pinned on empirical
	    xorshift32 seeds (seed 2 -> rolls (0,1) = 1 chick; seed 1 -> (0,0) = 4;
	    seed 2048 -> (1,-) = none).
*/

#include <assert.h>
#include <inttypes.h>
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
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "item/items.h"
#include "item/recipe.h"
#include "item/window_container.h"
#include "network/client_interface.h"
#include "network/entity_archive.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "support/clin_drain.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_animal_interact_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

extern struct item item_bucket_milk;
extern struct inventory_logic inventory_logic_player;
extern struct inventory_logic inventory_logic_crafting;

static struct server_local s;
static volatile int g_drain_run = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

/* CRPC_ENTITY_MOVE sheared-flag capture for one watched entity id. */
static uint32_t g_watch_move_id = 0;
static bool g_watch_move_seen = false;
static bool g_watch_move_sheared = false;

static void capture_move(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_ENTITY_MOVE
	   && call->payload.entity_move.entity_id == g_watch_move_id) {
		g_watch_move_seen = true;
		g_watch_move_sheared = call->payload.entity_move.sheared;
	}
}

static void clear_entities(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_reset(s.entities);
}

/* Count dropped-item entities matching id (+ optional durability filter). */
static size_t count_items(uint16_t item_id, int durability_or_negative) {
	size_t count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->data.item.item.id == item_id
		   && (durability_or_negative < 0
			   || e->data.item.item.durability
				   == (uint16_t)durability_or_negative))
			count += e->data.item.item.count;
		dict_entity_next(it);
	}
	return count;
}

static size_t count_monsters(int monster_id) {
	size_t count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MONSTER && e->data.monster.id == monster_id)
			count++;
		dict_entity_next(it);
	}
	return count;
}

static size_t count_type(enum entity_type type) {
	size_t count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == type)
			count++;
		dict_entity_next(it);
	}
	return count;
}

/* Find monsters of an id in dict order; index picks among multiple. */
static struct entity* find_monster(int monster_id, size_t index) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MONSTER && e->data.monster.id == monster_id) {
			if(index == 0)
				return e;
			index--;
		}
		dict_entity_next(it);
	}
	return NULL;
}

static struct entity* find_type(enum entity_type type) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == type)
			return e;
		dict_entity_next(it);
	}
	return NULL;
}

static void use_entity(uint32_t id) {
	struct server_rpc rpc = {
		.type = SRPC_ENTITY_USE,
		.payload.entity_use.entity_id = id,
	};
	server_local_test_process_rpc(&s, &rpc);
}

static struct item_data* hotbar_slot(size_t k) {
	return &s.player.inventory.items[INVENTORY_SLOT_HOTBAR + k];
}

static void assert_death_rng(const char* label, const struct entity* e,
								 struct java_random before) {
	const struct java_random* after = &e->gameplay_rng;
	fprintf(stderr, "  %s: owned seed %012" PRIx64 "->%012" PRIx64
		", cache %d/%g->%d/%g\n",
		label, before.seed, after->seed, before.have_next_gaussian,
		before.next_gaussian_cached, after->have_next_gaussian,
		after->next_gaussian_cached);
	assert(after->seed == before.seed);
	assert(after->have_next_gaussian == before.have_next_gaussian);
	assert(after->next_gaussian_cached == before.next_gaussian_cached);
}

int main(void) {
	test_fixture_dir("test_animal_interact_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	recipe_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = UINT32_C(0xA1B2C3D4);
	jrand_set_seed(&s.worlds[0].rand, 4242);
	server_local_test_seed_body_rand(&s, &s.player, 13579);
	server_local_test_seed_math_random(1357);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;

	struct server_chunk* sc = NULL;
	/* Entity updates use Beta's position +/-32 checkChunksExist gate. Keep the
	 * focused interaction arena fully active while preserving its flat geometry. */
	for(w_coord_t cx = -2; cx <= 2; cx++)
		for(w_coord_t cz = -2; cz <= 2; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	/* Superflat surface probe: highest non-air block in the (8, *, 8) column. */
	int gy = -1;
	for(int y = 127; y >= 0; y--) {
		struct block_data probe;
		if(server_world_get_block(&s.world, 8, y, 8, &probe)
		   && probe.type != BLOCK_AIR) {
			gy = y;
			break;
		}
	}
	assert(gy > 0);
	const float feet = (float)(gy + 1);
	const double eye = (double)(gy + 1) + PLAYER_EYE_HEIGHT;

	/* ================= S1: ENTITY_USE validation + cow milking ============ */
	struct entity* cow
		= server_local_spawn_monster_at((vec3) {8.5f, feet, 8.5f}, MONSTER_COW,
										&s);
	assert(cow);
	inventory_set_hotbar(&s.player.inventory, 0);
	*hotbar_slot(0) = (struct item_data) {.id = ITEM_BUCKET, .count = 1};
	*hotbar_slot(1) = (struct item_data) {.id = ITEM_BUCKET, .count = 1};

	/* Reach reject: Beta server gate distSq < 36 (6 blocks). */
	s.player.x = 30.5;
	s.player.y = eye;
	s.player.z = 8.5;
	use_entity(cow->id);
	assert(hotbar_slot(0)->id == ITEM_BUCKET);

	/* Liveness reject: a death-flop corpse is not interactable. */
	s.player.x = 10.5;
	s.player.z = 8.5;
	cow->data.monster.death_time = 5;
	use_entity(cow->id);
	assert(hotbar_slot(0)->id == ITEM_BUCKET);
	cow->data.monster.death_time = 0;

	/* Accept: in-reach live cow + held empty bucket -> whole-slot milk swap
	 * [EntityCow.interact]; the OTHER bucket slot is untouched. */
	use_entity(cow->id);
	assert(hotbar_slot(0)->id == ITEM_MILK_BUCKET);
	assert(hotbar_slot(0)->count == 1);
	assert(hotbar_slot(1)->id == ITEM_BUCKET && hotbar_slot(1)->count == 1);

	/* Held milk (not an empty bucket) -> the cow interact is a no-op. */
	use_entity(cow->id);
	assert(hotbar_slot(0)->id == ITEM_MILK_BUCKET);
	puts("  S1: ENTITY_USE reach/liveness validation + cow milk swap OK");

	/* Milk bucket empties against ANY block its OWN ray hits (ItemBucket
	 * isFull<0 branch [ItemBucket.java:51-53]): the block is untouched, the
	 * stack becomes an empty bucket. ItemBucket has no onItemUse, so the real
	 * at-a-block click (SRPC_BLOCK_PLACE) runs that ray from the eye, aimed
	 * here at the ground's top-face centre (8.5, gy+1, 8.5). Not edible or
	 * placeable: no place verb exists on the definition. */
	{
		struct block_data ground;
		assert(server_world_get_block(&s.world, 8, gy, 8, &ground));
		assert(!item_bucket_milk.onItemPlace); /* not edible/placeable */
		assert(hotbar_slot(0)->id == ITEM_MILK_BUCKET);
		double dx = 8.5 - s.player.x, dy = (gy + 1.0) - s.player.y,
			   dz = 8.5 - s.player.z;
		/* Beta look: (-sin yaw cos pitch, -sin pitch, cos yaw cos pitch). */
		s.player.rx = (float)(atan2(-dx, dz) * 180.0 / 3.141592653589793);
		s.player.ry = (float)(atan2(-dy, sqrt(dx * dx + dz * dz)) * 180.0
			/ 3.141592653589793);
		struct server_rpc click = {.type = SRPC_BLOCK_PLACE,
			.payload.block_place = {.x = 8, .y = gy, .z = 8, .side = SIDE_TOP,
				.held_item = *hotbar_slot(0), .has_held_item = true}};
		server_local_test_process_rpc(&s, &click);
		assert(hotbar_slot(0)->id == ITEM_BUCKET && hotbar_slot(0)->count == 1);
		struct block_data after;
		assert(server_world_get_block(&s.world, 8, gy, 8, &after));
		assert(after.type == ground.type && after.metadata == ground.metadata);
		assert(server_world_get_block(&s.worlds[0], 8, gy + 1, 8, &after));
		assert(after.type == BLOCK_AIR); /* nothing placed */
		assert(item_bucket_milk.container_item == ITEM_BUCKET);
	}
	puts("  S1: milk bucket empties on block use, block untouched");

	/* ================= S2: sheep shear + dye ============================== */
	clear_entities();
	struct entity* sheep = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_SHEEP, &s);
	assert(sheep && !sheep->data.monster.sheared);
	sheep->data.monster.fleece_color = 11; /* explicit blue marker */
	*hotbar_slot(0)
		= (struct item_data) {.id = ITEM_SHEARS, .durability = 0, .count = 1};
	s.player.x = 10.5;
	s.player.z = 8.5;

	/* Predict the wool count with a duplicate of the seeded stream: the shear
	 * branch's FIRST draw is Beta's 2 + nextInt(3). */
	struct java_random probe = sheep->gameplay_rng;
	int expected_wool = 2 + jrand_next_int_bound(&probe, 3);
	assert(expected_wool >= 2 && expected_wool <= 4);

	/* I02 (board #8): each wool unit's scatter is the SHEEP's own Random --
	 * CFR EntitySheep.java:66-68: motionY += nextFloat()*0.05F, then motionX
	 * += (nextFloat()-nextFloat())*0.1F, then motionZ likewise: five floats
	 * per unit on this.rand, after the count draw. The world stream is not
	 * touched (entityDropItem's EntityItem motion is Math.random). */
	for(int i = 0; i < expected_wool * 5; i++)
		(void)jrand_next_float(&probe);
	const struct random_gen world_before_shear = s.rand_src;
	use_entity(sheep->id);
	assert(sheep->data.monster.sheared);
	assert(count_items(BLOCK_WOOL, 11) == (size_t)expected_wool);
	assert(memcmp(&world_before_shear, &s.rand_src, sizeof s.rand_src) == 0
		   && "I02: shearing draws nothing from the world stream");
	assert(sheep->gameplay_rng.seed == probe.seed
		   && "I02: count + 5 floats per wool unit on the sheep's own stream");
	assert(hotbar_slot(0)->id == ITEM_SHEARS
		   && hotbar_slot(0)->durability == 1); /* damageItem(1) */

	/* CRPC flag: the sheep's living-tick trailing MOVE carries sheared=true. */
	g_watch_move_id = sheep->id;
	g_watch_move_seen = false;
	g_watch_move_sheared = false;
	clin_test_set_rpc_observer(capture_move, NULL);
	assert(!sheep->tick_server(sheep, &s));
	clin_test_set_rpc_observer(NULL, NULL);
	assert(g_watch_move_seen && g_watch_move_sheared);
	puts("  S2: shear -> sheared + 2..4 fleece wool + durability + MOVE flag");

	/* Second shear no-ops: no drops, no durability, still sheared. */
	size_t wool_before = count_items(BLOCK_WOOL, -1);
	use_entity(sheep->id);
	assert(count_items(BLOCK_WOOL, -1) == wool_before);
	assert(hotbar_slot(0)->durability == 1);
	assert(sheep->data.monster.sheared);

	/* Dye on a sheared sheep: no recolour, nothing consumed. */
	*hotbar_slot(0)
		= (struct item_data) {.id = ITEM_DYE, .durability = 1, .count = 3};
	use_entity(sheep->id);
	assert(sheep->data.monster.fleece_color == 11);
	assert(hotbar_slot(0)->count == 3);

	/* Dye on an UNSHEARED white sheep: fleece = ~damage & 15, consume 1;
	 * repeating the same colour consumes nothing [ItemDye useItemOnEntity]. */
	struct entity* sheep2 = server_local_spawn_monster_at(
		(vec3) {10.5f, feet, 10.5f}, MONSTER_SHEEP, &s);
	assert(sheep2);
	sheep2->data.monster.fleece_color = 0;
	s.player.x = 10.5;
	s.player.z = 12.5;
	use_entity(sheep2->id);
	assert(sheep2->data.monster.fleece_color == ((~1) & 15)); /* 14 = red */
	assert(hotbar_slot(0)->count == 2);
	use_entity(sheep2->id);
	assert(sheep2->data.monster.fleece_color == 14);
	assert(hotbar_slot(0)->count == 2); /* same colour: nothing consumed */
	puts("  S2: dye recolour + consume; sheared/same-colour no-ops");

	/* ================= S3 archive: sheared + egg timer round-trip ========= */
	struct entity* chicken = server_local_spawn_monster_at(
		(vec3) {12.5f, feet, 12.5f}, MONSTER_CHICKEN, &s);
	assert(chicken && chicken->data.monster.egg_timer == 0);
	chicken->data.monster.egg_timer = 4321;

	size_t wool_saved = count_items(BLOCK_WOOL, -1);
	test_drain_quiesce();
	test_clin_drain();
	assert(server_world_save_chunk(&s.world, true, 0, 0));
	assert(!server_world_is_chunk_loaded(&s.world, 0, 0));
	assert(count_monsters(MONSTER_SHEEP) == 0);
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);

	/* Re-resolve the restored entities (fresh runtime ids). The sheared flag
	 * distinguishes the two sheep. */
	sheep = NULL;
	sheep2 = NULL;
	for(size_t i = 0; i < 2; i++) {
		struct entity* cand = find_monster(MONSTER_SHEEP, i);
		assert(cand);
		if(cand->data.monster.sheared)
			sheep = cand;
		else
			sheep2 = cand;
	}
	chicken = find_monster(MONSTER_CHICKEN, 0);
	assert(sheep && sheep2 && chicken);
	assert(sheep->data.monster.sheared
		   && sheep->data.monster.fleece_color == 11);
	assert(!sheep2->data.monster.sheared
		   && sheep2->data.monster.fleece_color == 14);
	assert(chicken->data.monster.egg_timer == 4321);
	assert(count_items(BLOCK_WOOL, -1) == wool_saved);
	puts("  S2/S3: CaveWiiSheared + CaveWiiEggTimer survive the disk round trip");

	/* Death drops: the SHEARED sheep drops NO wool; the unsheared one drops
	 * exactly 1 of its fleece colour [EntitySheep.dropFewItems:23-28]. The
	 * override consumes no sheep RNG. The full tick still consumes the corpse
	 * sound-scheduler roll. Keep its timer negative so no voice pitch fires;
	 * arm a cache sentinel without replacing the restored lifetime seed. */
	sheep->data.monster.living_sound_time = -120;
	sheep2->data.monster.living_sound_time = -120;
	sheep->gameplay_rng.have_next_gaussian = true;
	sheep->gameplay_rng.next_gaussian_cached = 0.375F;
	struct java_random sheared_rng = sheep->gameplay_rng;
	wool_before = count_items(BLOCK_WOOL, -1);
	sheep->health = 0;
	assert(!sheep->tick_server(sheep, &s));
	assert(count_items(BLOCK_WOOL, -1) == wool_before);
	assert(sheep->data.monster.death_loot_dropped);
	/* EntityLiving.java:108 precedes deathTime; c5927d951 restored this
	 * unconditional scheduler draw. The sheep override adds no loot draw. */
	(void)jrand_next_int_bound(&sheared_rng, 1000);
	assert_death_rng("sheared first death", sheep, sheared_rng);
	assert(!sheep->tick_server(sheep, &s));
	assert(count_items(BLOCK_WOOL, -1) == wool_before);
	(void)jrand_next_int_bound(&sheared_rng, 1000);
	assert_death_rng("sheared repeat death", sheep, sheared_rng);
	sheep2->gameplay_rng.have_next_gaussian = true;
	sheep2->gameplay_rng.next_gaussian_cached = -0.625F;
	struct java_random unsheared_rng = sheep2->gameplay_rng;
	size_t red_before = count_items(BLOCK_WOOL, 14);
	sheep2->health = 0;
	assert(!sheep2->tick_server(sheep2, &s));
	assert(count_items(BLOCK_WOOL, 14) == red_before + 1);
	assert(sheep2->data.monster.death_loot_dropped);
	(void)jrand_next_int_bound(&unsheared_rng, 1000);
	assert_death_rng("unsheared first death", sheep2, unsheared_rng);
	assert(!sheep2->tick_server(sheep2, &s));
	assert(count_items(BLOCK_WOOL, 14) == red_before + 1);
	(void)jrand_next_int_bound(&unsheared_rng, 1000);
	assert_death_rng("unsheared repeat death", sheep2, unsheared_rng);
	puts("  S2: sheep death emits 0/1 wool once; only the scheduler consumes RNG");

	/* ================= S3: chicken lays exactly at expiry ================= */
	/* A FRESH chicken's first tick rolls the unrolled sentinel into Beta's
	 * 6000..11999 without laying. */
	struct entity* chicken2 = server_local_spawn_monster_at(
		(vec3) {4.5f, feet, 4.5f}, MONSTER_CHICKEN, &s);
	assert(chicken2 && chicken2->data.monster.egg_timer == 0);
	assert(count_items(ITEM_EGG, -1) == 0);
	assert(!chicken2->tick_server(chicken2, &s));
	assert(chicken2->data.monster.egg_timer >= 6000
		   && chicken2->data.monster.egg_timer <= 11999);
	assert(count_items(ITEM_EGG, -1) == 0);

	/* Expiry: timer 3 -> lays on the THIRD tick exactly, then re-rolls. */
	chicken->data.monster.egg_timer = 3;
	assert(!chicken->tick_server(chicken, &s));
	assert(count_items(ITEM_EGG, -1) == 0);
	assert(!chicken->tick_server(chicken, &s));
	assert(count_items(ITEM_EGG, -1) == 0);
	assert(!chicken->tick_server(chicken, &s));
	assert(count_items(ITEM_EGG, -1) == 1);
	assert(chicken->data.monster.egg_timer >= 6000
		   && chicken->data.monster.egg_timer <= 11999);
	puts("  S3: egg laid exactly at expiry + 6000..11999 re-roll");

	/* ================= S4: cake recipe + container-item return =========== */
	{
		/* Stock the player inventory so the crafting window copy is clean. */
		for(size_t k = 0; k < INVENTORY_SIZE_HOTBAR; k++)
			*hotbar_slot(k) = (struct item_data) {0};
		/* Heap-allocated like production windows: the crafting logic's
		 * on_destroy returns true, making inventory_destroy free the struct. */
		struct inventory* craft = malloc(sizeof(*craft));
		assert(craft);
		assert(inventory_create(craft, &inventory_logic_crafting, &s, &s.player,
								CRAFTING_SIZE, 0, 0, 0));
		/* "AAA","BEB","CCC": A milk, B sugar, E egg, C wheat. */
		static const struct item_data grid[9] = {
			{.id = ITEM_MILK_BUCKET, .count = 1},
			{.id = ITEM_MILK_BUCKET, .count = 1},
			{.id = ITEM_MILK_BUCKET, .count = 1},
			{.id = ITEM_SUGAR, .count = 1},
			{.id = ITEM_EGG, .count = 1},
			{.id = ITEM_SUGAR, .count = 1},
			{.id = ITEM_WHEAT, .count = 1},
			{.id = ITEM_WHEAT, .count = 1},
			{.id = ITEM_WHEAT, .count = 1},
		};
		set_inv_slot_t changes;
		set_inv_slot_init(changes);
		for(size_t k = 0; k < 9; k++)
			inventory_set_slot(craft, CRAFTING_SLOT_INPUT + k, grid[k]);
		inventory_logic_crafting.post_action(craft, &s.player,
											 CRAFTING_SLOT_INPUT,
											 false, true, changes);
		struct item_data output;
		assert(inventory_get_slot(craft, CRAFTING_SLOT_OUTPUT, &output));
		assert(output.id == ITEM_CAKE && output.count == 1);

		/* Pick up the craft: inputs decrement; the 3 milk buckets become 3
		 * EMPTY buckets IN THE GRID [SlotCrafting.onPickupFromSlot]. */
		assert(!inventory_logic_crafting.pre_action(
			craft, &s.player, CRAFTING_SLOT_OUTPUT, false, changes));
		struct item_data picked;
		assert(inventory_get_picked_item(craft, &picked));
		assert(picked.id == ITEM_CAKE && picked.count == 1);
		for(size_t k = 0; k < 3; k++) {
			struct item_data slot;
			assert(inventory_get_slot(craft, CRAFTING_SLOT_INPUT + k, &slot));
			assert(slot.id == ITEM_BUCKET && slot.count == 1);
		}
		for(size_t k = 3; k < 9; k++)
			assert(!inventory_get_slot(craft, CRAFTING_SLOT_INPUT + k, NULL));
		inventory_clear_picked_item(craft);
		set_inv_slot_clear(changes);
		inventory_destroy_owned(craft, &s.player);
	}
	puts("  S4: cake crafts; 3 empty buckets returned IN the grid");

	/* ================= S5: thrown egg ==================================== */
	clear_entities();

	/* Throw via the production SRPC_USE_ITEM path: consumes one egg and spawns
	 * ENTITY_EGG at 1.5 speed along the look ray (rx=0, ry=0 -> +Z). */
	*hotbar_slot(0) = (struct item_data) {.id = ITEM_EGG, .count = 2};
	inventory_set_hotbar(&s.player.inventory, 0);
	s.player.x = 8.5;
	s.player.y = eye;
	s.player.z = 4.5;
	s.player.rx = 0.0f;
	s.player.ry = 0.0f;
	struct server_rpc use = {
		.type = SRPC_USE_ITEM,
		.payload.use_item.charge = 1.0f,
	};
	server_local_test_process_rpc(&s, &use);
	assert(hotbar_slot(0)->id == ITEM_EGG && hotbar_slot(0)->count == 1);
	struct entity* egg = find_type(ENTITY_EGG);
	assert(egg);
	assert(fabsf(egg->vel[2] - EGG_LAUNCH_SPEED) < 1.0e-3f);
	assert(fabsf(egg->vel[0]) < 1.0e-3f);
	puts("  S5: SRPC_USE_ITEM egg throw consumes 1 + spawns ENTITY_EGG @1.5");
	clear_entities();

	/* Entity hit = ZERO damage through the standard path: hurt flash +
	 * knockback + hostile retaliation latch, health unchanged; the egg dies
	 * even though it dealt nothing. Java seed 0 pins nextInt(8)=5 (no chick). */
	struct entity* zombie = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_ZOMBIE, &s);
	assert(zombie && zombie->health == 20);
	zombie->vel[0] = zombie->vel[1] = zombie->vel[2] = 0.0f;
	{
		uint32_t id = server_local_gen_id(&s);
		struct entity** pp = dict_entity_safe_get(s.entities, id);
		*pp = malloc(sizeof(**pp));
		assert(*pp);
		struct entity* thrown = *pp;
		entity_egg(id, thrown, true, &s.world, 0);
		thrown->teleport(thrown, (vec3) {8.5f, feet + 0.9f, 6.5f});
		glm_vec3_copy((vec3) {0.0f, 0.0f, 0.8f}, thrown->vel);
		assert(server_local_test_entity_join(&s, id));
		jrand_set_seed(&thrown->gameplay_rng, 0); /* Java48 nextInt(8)=5 */
		struct random_gen world_before = s.rand_src;
		bool hit = false;
		for(int i = 0; i < 5 && !hit; i++)
			hit = thrown->tick_server(thrown, &s);
		assert(hit);
		assert(thrown->gameplay_rng.seed == UINT64_C(205749139540596));
		assert(memcmp(&world_before, &s.rand_src, sizeof world_before) == 0);
		free(thrown);
		dict_entity_erase(s.entities, id);
	}
	assert(zombie->health == 20); /* zero damage */
	assert(zombie->data.monster.hurt_time == MOB_HURT_ANIMATION_TICKS);
	assert(zombie->data.monster.hurt_resist_time == MOB_HURT_RESIST_TICKS);
	assert(zombie->data.monster.last_damage == 0);
	assert(zombie->data.monster.knockback_time == MOB_KNOCKBACK_TICKS);
	assert(zombie->data.monster.aggro_time == MOB_TARGET_RETENTION_TICKS);
	assert(s.pending_monster_count == 0);
	puts("  S5: egg entity hit = 0-damage hurt/knockback/retaliate, no chick");
	clear_entities();

	/* PIG ZOMBIE SELF-ANGER (parity fix 2026-09-02, decompile-verified,
	   740c583:EntityPigZombie.attackEntityFrom + becomeAngryAt): the thrown
	   egg's thrower is always the player (Item.egg is player-only), so
	   `instanceof EntityPlayer` is unconditional -- a thrown-egg hit on a
	   pig zombie must arm its own anger_level (400..799), matching the same
	   self-anger the arrow path now ports (entity_arrow.c). Only the SELF
	   half; the GROUP alert to nearby pig zombies has no exported
	   server_local.c hook yet for a non-melee attacker. */
	struct entity* pig_zombie = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_PIG_ZOMBIE, &s);
	assert(pig_zombie && pig_zombie->data.monster.anger_level == 0);
	pig_zombie->vel[0] = pig_zombie->vel[1] = pig_zombie->vel[2] = 0.0f;
	{
		uint32_t id = server_local_gen_id(&s);
		struct entity** pp = dict_entity_safe_get(s.entities, id);
		*pp = malloc(sizeof(**pp));
		assert(*pp);
		struct entity* thrown = *pp;
		entity_egg(id, thrown, true, &s.world, 0);
		thrown->teleport(thrown, (vec3) {8.5f, feet + 0.9f, 6.5f});
		glm_vec3_copy((vec3) {0.0f, 0.0f, 0.8f}, thrown->vel);
		assert(server_local_test_entity_join(&s, id));
		bool hit = false;
		for(int i = 0; i < 5 && !hit; i++)
			hit = thrown->tick_server(thrown, &s);
		assert(hit);
		free(thrown);
		dict_entity_erase(s.entities, id);
	}
	assert(pig_zombie->data.monster.anger_level >= 400
		   && pig_zombie->data.monster.anger_level < 800);
	puts("  S5: thrown-egg hit on a pig zombie arms its own anger_level");
	clear_entities();

	/* Block hit: the egg despawns; the chick roll is pinned per seed through
	 * the REAL entity pass (the pending-monster flush spawns the chicks). */
	static const struct {
		uint32_t seed;
		size_t chicks;
	} rolls[3] = {
		{UINT32_C(4096), 1}, /* Java48: nextInt(8)=0, nextInt(32)=28 */
		{UINT32_C(4104), 4}, /* Java48: nextInt(8)=0, nextInt(32)=0 */
		{UINT32_C(0), 0},    /* Java48: nextInt(8)=5, no second draw */
	};
	for(size_t c = 0; c < 3; c++) {
		clear_entities();
		uint32_t id = server_local_gen_id(&s);
		struct entity** pp = dict_entity_safe_get(s.entities, id);
		*pp = malloc(sizeof(**pp));
		assert(*pp);
		struct entity* drop_egg = *pp;
		entity_egg(id, drop_egg, true, &s.world, 0);
		drop_egg->teleport(drop_egg, (vec3) {8.5f, feet + 0.6f, 8.5f});
		glm_vec3_copy((vec3) {0.0f, -1.0f, 0.0f}, drop_egg->vel);
		assert(server_local_test_entity_join(&s, id));
		/* CFR EntityEgg.java:153-155: egg-owned, nested 8 then 32. */
		jrand_set_seed(&drop_egg->gameplay_rng, rolls[c].seed);
		struct java_random expected = drop_egg->gameplay_rng;
		if(jrand_next_int_bound(&expected, 8) == 0)
			(void)jrand_next_int_bound(&expected, 32);
		struct random_gen world_before = s.rand_src;
		/* Tick directly before the flush frees the projectile. */
		assert(drop_egg->tick_server(drop_egg, &s));
		assert(drop_egg->gameplay_rng.seed == expected.seed);
		assert(memcmp(&world_before, &s.rand_src, sizeof world_before) == 0);
		drop_egg->delay_destroy = 0;
		server_world_begin_update(&s.world);
		assert(server_local_test_tick_entities(&s));
		assert(count_type(ENTITY_EGG) == 0); /* block hit despawned it */
		assert(count_monsters(MONSTER_CHICKEN) == rolls[c].chicks);
	}
	puts("  S5: block hit despawns; chick rolls 1/4/0 pinned on Java seeds 4096/4104/0");

	/* ================= S6: thrown snowball (batch 1a) ==================== */
	/* [740c583: ItemSnowball.onItemRightClick == ItemEgg's line-for-line;
	 * EntitySnowball == EntityEgg minus the impact spawn]. The sections below
	 * mirror S5 with the ONE decompile delta pinned: a snowball impact
	 * consumes ZERO rng draws and never spawns anything. */
	clear_entities();

	/* Throw via the production SRPC_USE_ITEM path. */
	*hotbar_slot(0) = (struct item_data) {.id = ITEM_SNOW_BALL, .count = 2};
	inventory_set_hotbar(&s.player.inventory, 0);
	s.player.x = 8.5;
	s.player.y = eye;
	s.player.z = 4.5;
	s.player.rx = 0.0f;
	s.player.ry = 0.0f;
	struct server_rpc use_snow = {
		.type = SRPC_USE_ITEM,
		.payload.use_item.charge = 1.0f,
	};
	server_local_test_process_rpc(&s, &use_snow);
	assert(hotbar_slot(0)->id == ITEM_SNOW_BALL && hotbar_slot(0)->count == 1);
	struct entity* snow = find_type(ENTITY_SNOWBALL);
	assert(snow);
	assert(fabsf(snow->vel[2] - EGG_LAUNCH_SPEED) < 1.0e-3f);
	assert(fabsf(snow->vel[0]) < 1.0e-3f);
	puts("  S6: SRPC_USE_ITEM snowball throw consumes 1 + spawns "
		 "ENTITY_SNOWBALL @1.5");
	clear_entities();

	/* Entity hit: the same zero-damage standard-path hurt as the egg -- but
	 * the impact draws NOTHING from the rng (Beta EntitySnowball has no spawn
	 * branch), pinned by the seed being bit-identical after the hit. */
	struct entity* zombie2 = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_ZOMBIE, &s);
	assert(zombie2 && zombie2->health == 20);
	zombie2->vel[0] = zombie2->vel[1] = zombie2->vel[2] = 0.0f;
	{
		uint32_t id = server_local_gen_id(&s);
		struct entity** pp = dict_entity_safe_get(s.entities, id);
		*pp = malloc(sizeof(**pp));
		assert(*pp);
		struct entity* thrown = *pp;
		entity_snowball(id, thrown, true, &s.world, 0);
		thrown->teleport(thrown, (vec3) {8.5f, feet + 0.9f, 6.5f});
		glm_vec3_copy((vec3) {0.0f, 0.0f, 0.8f}, thrown->vel);
		assert(server_local_test_entity_join(&s, id));
		s.rand_src.seed = UINT32_C(1); /* the egg's QUAD seed: 4 chicks there */
		bool hit = false;
		for(int i = 0; i < 5 && !hit; i++)
			hit = thrown->tick_server(thrown, &s);
		assert(hit);
		assert(s.rand_src.seed == UINT32_C(1)
			   && "snowball impact must consume ZERO rng draws");
		free(thrown);
		dict_entity_erase(s.entities, id);
	}
	assert(zombie2->health == 20); /* zero damage */
	assert(zombie2->data.monster.hurt_time == MOB_HURT_ANIMATION_TICKS);
	assert(zombie2->data.monster.knockback_time == MOB_KNOCKBACK_TICKS);
	assert(count_monsters(MONSTER_CHICKEN) == 0); /* egg-only behavior */
	puts("  S6: snowball entity hit = 0-damage hurt, ZERO rng draws, no spawn");
	clear_entities();

	/* Block hit on the egg's 4-chick seed: despawns, still hatches nothing. */
	{
		uint32_t id = server_local_gen_id(&s);
		struct entity** pp = dict_entity_safe_get(s.entities, id);
		*pp = malloc(sizeof(**pp));
		assert(*pp);
		struct entity* drop_snow = *pp;
		entity_snowball(id, drop_snow, true, &s.world, 0);
		drop_snow->teleport(drop_snow, (vec3) {8.5f, feet + 0.6f, 8.5f});
		glm_vec3_copy((vec3) {0.0f, -1.0f, 0.0f}, drop_snow->vel);
		assert(server_local_test_entity_join(&s, id));
		s.rand_src.seed = UINT32_C(1);
		server_world_begin_update(&s.world);
		assert(server_local_test_tick_entities(&s));
		assert(count_type(ENTITY_SNOWBALL) == 0); /* block hit despawned it */
		assert(count_monsters(MONSTER_CHICKEN) == 0);
	}
	puts("  S6: snowball block hit despawns; no hatch on the egg quad seed");
	clear_entities();

	/* Archive round trip: a mid-flight snowball survives the chunk unload
	 * boundary as NBT id "Snowball" with owner + age intact (the egg S3
	 * precedent). */
	{
		uint32_t id = server_local_gen_id(&s);
		struct entity** pp = dict_entity_safe_get(s.entities, id);
		*pp = malloc(sizeof(**pp));
		assert(*pp);
		struct entity* mid = *pp;
		entity_snowball(id, mid, true, &s.world, 0);
		mid->teleport(mid, (vec3) {12.5f, feet + 6.0f, 12.5f});
		glm_vec3_copy((vec3) {0.0f, 0.4f, 0.0f}, mid->vel);
		mid->data.egg.age = 2000;
		assert(server_local_test_entity_join(&s, id));
	}
	test_drain_quiesce();
	test_clin_drain();
	assert(server_world_save_chunk(&s.world, true, 0, 0));
	assert(!server_world_is_chunk_loaded(&s.world, 0, 0));
	assert(count_type(ENTITY_SNOWBALL) == 0);
	assert(server_world_load_chunk(&s.world, 0, 0, &sc) && sc);
	{
		struct entity* back = find_type(ENTITY_SNOWBALL);
		assert(back && "snowball must survive the disk round trip");
		assert(back->data.egg.age == 2000);
		assert(back->data.egg.owner_id == 0); /* local player, remapped */
	}
	puts("  S6: mid-flight snowball survives the disk round trip (age+owner)");

	clear_entities();
	/* EntityLiving.dropFewItems:424-433 draws nextInt(3) only for id > 0.
	 * Wolf:82-84 returns -1 (our no-item sentinel 0); Slime:130-131 returns
	 * 0 above size 1. Actual first/repeat death ticks precede slime's tick-20
	 * split, whose separate child/yaw draws are outside this count witness. */
	const int no_drop_ids[] = {MONSTER_WOLF, MONSTER_SLIME_MEDIUM};
	for(size_t i = 0; i < sizeof no_drop_ids / sizeof no_drop_ids[0]; i++) {
		struct entity* no_drop = server_local_spawn_monster_at(
			(vec3) {2.5F + 12.0F * (float)i, feet, 2.5F}, no_drop_ids[i], &s);
		assert(no_drop && no_drop->drop_item.id == 0);
		no_drop->data.monster.living_sound_time = -120;
		no_drop->gameplay_rng.have_next_gaussian = true;
		no_drop->gameplay_rng.next_gaussian_cached = 0.25F;
		struct java_random before = no_drop->gameplay_rng;
		size_t items_before = count_type(ENTITY_ITEM);
		no_drop->health = 0;
		for(int tick = 1; tick <= 2; tick++) {
			(void)jrand_next_int_bound(&before, 1000);
			assert(!no_drop->tick_server(no_drop, &s));
			assert(no_drop->data.monster.death_loot_dropped);
			assert(no_drop->data.monster.death_time == tick);
			assert(count_type(ENTITY_ITEM) == items_before);
			assert_death_rng(no_drop_ids[i] == MONSTER_WOLF
				? "wolf no-drop death" : "medium-slime no-drop death", no_drop, before);
		}
	}
	puts("  S7: wolf/slime deaths consume only the corpse sound-scheduler roll");

	/* Positive base-drop control: scheduler then one count draw, actual pork
	 * quantity/id, then only the scheduler on repeat. Item construction uses a separate stream;
	 * this does not assert world RNG invariance or Java RNG equivalence. */
	struct entity* pig_drop = server_local_spawn_monster_at(
		(vec3) {2.5F, feet, 14.5F}, MONSTER_PIG, &s);
	assert(pig_drop && pig_drop->drop_item.id == ITEM_PORKCHOP);
	/* This seed's scheduler-then-count order yields two pork, not zero. */
	jrand_set_seed(&pig_drop->gameplay_rng, INT64_C(0xA1B2C3D5));
	pig_drop->data.monster.living_sound_time = -120;
	pig_drop->gameplay_rng.have_next_gaussian = true;
	pig_drop->gameplay_rng.next_gaussian_cached = -0.75;
	struct java_random expected = pig_drop->gameplay_rng;
	(void)jrand_next_int_bound(&expected, 1000);
	int pork_count = jrand_next_int_bound(&expected, MOB_DROP_COUNT_DEN);
	assert(pork_count > 0);
	size_t pork_before = count_items(ITEM_PORKCHOP, -1);
	size_t item_count_before = count_type(ENTITY_ITEM);
	pig_drop->health = 0;
	for(int tick = 1; tick <= 2; tick++) {
		if(tick > 1) (void)jrand_next_int_bound(&expected, 1000);
		assert(!pig_drop->tick_server(pig_drop, &s));
		assert(pig_drop->data.monster.death_loot_dropped);
		assert(pig_drop->data.monster.death_time == tick);
		assert(count_items(ITEM_PORKCHOP, -1) == pork_before + (size_t)pork_count);
		assert(count_type(ENTITY_ITEM) == item_count_before + 1);
		assert_death_rng("pig positive-drop death", pig_drop, expected);
	}
	puts("  S7: pig retains exactly one owned count draw and one pork drop");

	/* ======= S8: the trailing use after an entity interact ===============
	 * Beta's clickMouse(1) runs sendUseItem after interactWithEntity
	 * [Minecraft.java:799-806,834-838]; the client (screen_ingame.c) now
	 * follows SRPC_ENTITY_USE with SRPC_USE_ITEM carrying the stack AS
	 * CLICKED. The server must act on the stack it HOLDS after the interact
	 * (getCurrentItem), so a consumed saddle/dye is never used twice and the
	 * stale payload is ignored. */
	clear_entities();
	s.player.x = 4.5;
	s.player.y = eye;
	s.player.z = 6.5;
	struct entity* pig_s = server_local_spawn_monster_at(
		(vec3) {4.5f, feet, 4.5f}, MONSTER_PIG, &s);
	assert(pig_s && !pig_s->data.monster.saddled);
	struct item_data saddle_stack = {.id = ITEM_SADDLE, .count = 1};
	*hotbar_slot(0) = saddle_stack;
	/* Beta 1.7.3 saddles on the HIT (ItemSaddle.hitEntity -> saddleEntity);
	 * ItemSaddle has no useItemOnEntity, so the right-click leaves both the
	 * pig and the saddle alone, and so must the trailing use. */
	use_entity(pig_s->id);
	assert(!pig_s->data.monster.saddled);
	assert(hotbar_slot(0)->id == ITEM_SADDLE && hotbar_slot(0)->count == 1);
	size_t ents_before = count_type(ENTITY_ARROW) + count_type(ENTITY_ITEM);
	struct server_rpc trail = {
		.type = SRPC_USE_ITEM,
		.payload.use_item.charge = 1.0f,
		.payload.use_item.held_item = saddle_stack, /* stale client copy */
		.payload.use_item.has_held_item = true,
	};
	server_local_test_process_rpc(&s, &trail);
	assert(hotbar_slot(0)->id == ITEM_SADDLE && hotbar_slot(0)->count == 1);
	assert(!pig_s->data.monster.saddled);
	assert(count_type(ENTITY_ARROW) + count_type(ENTITY_ITEM) == ents_before);
	/* Saddled by the hit (one consumed), the trailing use of the stale
	 * saddle copy then finds an empty hand and does nothing. */
	struct server_rpc hit = {.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = pig_s->id};
	server_local_test_process_rpc(&s, &hit);
	assert(pig_s->data.monster.saddled);
	assert(hotbar_slot(0)->id == 0 || hotbar_slot(0)->count == 0);
	server_local_test_process_rpc(&s, &trail);
	assert(hotbar_slot(0)->id == 0 || hotbar_slot(0)->count == 0);
	assert(count_type(ENTITY_ARROW) + count_type(ENTITY_ITEM) == ents_before);
	puts("  S8: saddle: right-click + trailing use touch nothing; after the "
		 "saddling hit the stale saddle payload is ignored");

	struct entity* sheep_d = server_local_spawn_monster_at(
		(vec3) {6.5f, feet, 4.5f}, MONSTER_SHEEP, &s);
	assert(sheep_d);
	sheep_d->data.monster.fleece_color = 0;
	sheep_d->data.monster.sheared = false;
	s.player.x = 6.5;
	/* Last dye of the stack: consumed by the interact. */
	struct item_data dye_stack = {.id = ITEM_DYE, .durability = 1, .count = 1};
	*hotbar_slot(0) = dye_stack;
	use_entity(sheep_d->id);
	assert(sheep_d->data.monster.fleece_color == ((~1) & 15));
	assert(hotbar_slot(0)->id == 0 || hotbar_slot(0)->count == 0);
	trail.payload.use_item.held_item = dye_stack;
	server_local_test_process_rpc(&s, &trail);
	assert(hotbar_slot(0)->id == 0 || hotbar_slot(0)->count == 0);
	assert(sheep_d->data.monster.fleece_color == ((~1) & 15));
	/* Dye left over: the trailing use reaches Item.onItemRightClick (base:
	 * nothing), so the remaining dye is untouched. */
	sheep_d->data.monster.fleece_color = 0;
	dye_stack.count = 2;
	*hotbar_slot(0) = dye_stack;
	use_entity(sheep_d->id);
	assert(hotbar_slot(0)->id == ITEM_DYE && hotbar_slot(0)->count == 1);
	trail.payload.use_item.held_item = dye_stack;
	ents_before = count_type(ENTITY_ARROW) + count_type(ENTITY_ITEM);
	server_local_test_process_rpc(&s, &trail);
	assert(hotbar_slot(0)->id == ITEM_DYE && hotbar_slot(0)->count == 1);
	assert(count_type(ENTITY_ARROW) + count_type(ENTITY_ITEM) == ents_before);
	puts("  S8: dye on a sheep consumed once; the trailing use reads the "
		 "held stack, not the stale payload");
	clear_entities();
	g_drain_run = 0;
	thread_join(&drain);
	puts("test_animal_interact_env: all sections OK");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
