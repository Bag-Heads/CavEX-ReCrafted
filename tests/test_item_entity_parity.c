/* Beta 1.7.3 EntityItem.java:71-75, 113-126 and EntityPlayer.java:196-203:
 * age 6000 is dead before the player's box query; the query expands X/Z by
 * one and excludes dead entities; a partial inventory fit returns false, so
 * no pop plays and the ground stack keeps its remainder; a dead player
 * (health <= 0) runs no pickup scan at all. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <m-lib/m-string.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "entity/entity.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "world.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

/* Overworld-only fixture: every world use is the live slot &s.worlds[0]. */
static struct server_local s;
static struct server_player guest;
static volatile int draining = 1;
static char world_dir[128];

static void* drain_entry(void* arg) {
	(void)arg;
	while(draining) { clin_drain_discard(); thread_msleep(1); }
	clin_drain_discard();
	return NULL;
}

static struct entity* item_at(float x, float y, float z, uint8_t count) {
	struct entity* e = calloc(1, sizeof *e);
	assert(e);
	entity_item(900, e, true, &s.worlds[0],
		(struct item_data) {.id = BLOCK_COBBLESTONE, .count = count});
	e->teleport(e, (vec3) {x, y, z});
	glm_vec3_zero(e->vel);
	return e;
}

static void clear_inventory(struct server_player* player) {
	for(size_t i = 0; i < INVENTORY_SIZE; i++)
		inventory_clear_slot(&player->inventory, i);
}

static int slot_count(struct server_player* player, size_t slot) {
	struct item_data it = {0};
	if(!inventory_get_slot(&player->inventory, slot, &it))
		return 0; // empty slot (id 0)
	assert(it.id == BLOCK_COBBLESTONE);
	return it.count;
}

static void test_item_rng_lifetime(void) {
	/* Entity.java:41 owns Random; the documented deterministic lifetime policy
	 * mixes world seed/dimension/ASSIGNED id. No constructor Math.random claim.
	 * Scratch calls use declared inputs, independently of the produced entity. */
	static const int64_t seeds[] = {INT64_C(4242), INT64_C(0x1122334455667788)};
	static const int dimensions[] = {WORLD_DIM_OVERWORLD, WORLD_DIM_NETHER};
	static const uint32_t ids[] = {UINT32_C(901), UINT32_C(0xfedcba98)};
	struct entity* e = malloc(sizeof *e);
	assert(e);
	uint32_t world0 = s.rand_src.seed, shared0 = s.entity_rng.seed;
	uint32_t sound0 = server_local_sound_rng()->seed;
	for(size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
		for(size_t d = 0; d < sizeof dimensions / sizeof dimensions[0]; d++) {
			struct server_world world = {0};
			world.seed = seeds[i];
			world.dimension = dimensions[d];
			for(size_t n = 0; n < sizeof ids / sizeof ids[0]; n++) {
				memset(e, 0xBE, sizeof *e);
				entity_item(ids[n], e, true, &world,
					(struct item_data) {.id = BLOCK_COBBLESTONE, .count = 1});
				struct java_random expected;
				memset(&expected, 0xBE, sizeof expected);
				entity_lifetime_rng_seed(&expected, seeds[i], dimensions[d], ids[n]);
				assert(e->id == ids[n] && e->world == &world && e->on_server);
				assert(e->gameplay_rng.seed == expected.seed);
				assert(!e->gameplay_rng.have_next_gaussian
					&& e->gameplay_rng.next_gaussian_cached == 0);
				/* Teleport retains both cursor and cache, not just the seed. */
				(void)jrand_next_float(&e->gameplay_rng);
				e->gameplay_rng.have_next_gaussian = true;
				e->gameplay_rng.next_gaussian_cached = -0.625;
				struct java_random continuation = e->gameplay_rng;
				e->teleport(e, (vec3) {8.5F, 110.8F, 9.5F});
				assert(e->gameplay_rng.seed == continuation.seed);
				assert(e->gameplay_rng.have_next_gaussian);
				assert(e->gameplay_rng.next_gaussian_cached == -0.625);
			}
		}
	}
	/* Client factory must clear every state byte even in poisoned raw storage. */
	static struct world client_world;
	memset(e, 0xBE, sizeof *e);
	entity_item(UINT32_C(901), e, false, &client_world,
		(struct item_data) {.id = BLOCK_COBBLESTONE, .count = 1});
	unsigned char zero[sizeof e->gameplay_rng] = {0};
	assert(!e->on_server && e->world == &client_world);
	assert(memcmp(&e->gameplay_rng, zero, sizeof zero) == 0);
	e->teleport(e, (vec3) {8.5F, 110.8F, 9.5F});
	assert(memcmp(&e->gameplay_rng, zero, sizeof zero) == 0);
	assert(s.rand_src.seed == world0 && s.entity_rng.seed == shared0);
	assert(server_local_sound_rng()->seed == sound0);
	free(e);
	puts("item lifetime: explicit seed/dimension/id, poisoned server/client, teleport PASS");
}

static void run_actor(bool use_guest) {
	struct server_player* player = use_guest ? &guest : &s.player;
	struct server_player* other = use_guest ? &s.player : &guest;
	const char* who = use_guest ? "guest" : "host";
	player->x = 8.5; player->y = 102.62; player->z = 8.5;
	other->x = 40.5; other->y = 102.62; other->z = 40.5;
	clear_inventory(player);

	/* Age is incremented by onUpdate; player collision follows it in Beta. */
	struct entity* e = item_at(8.5F, 101.125F, 8.5F, 2);
	e->data.item.age = 5999;
	assert(e->tick_server(e, &s));
	assert(e->data.item.age == 6000 && e->data.item.item.count == 2);
	assert(slot_count(player, INVENTORY_SLOT_HOTBAR) == 0);
	free(e);

	/* Outside expanded X edge despite distance < 2; strict box intersection. */
	e = item_at(10.0F, 101.125F, 8.5F, 1);
	assert(!e->tick_server(e, &s));
	assert(e->data.item.item.count == 1 && slot_count(player, INVENTORY_SLOT_HOTBAR) == 0);
	free(e);

	/* Intersects the top/corner of the expanded box, but sphere distance > 2. */
	e = item_at(9.79F, 102.90F, 9.79F, 1);
	assert(!e->tick_server(e, &s));
	assert(e->delay_destroy == 1);
	assert(slot_count(player, INVENTORY_SLOT_HOTBAR) == 1);
	free(e);
	clear_inventory(player);

	/* InventoryPlayer.addItemStackToInventory returns false with one left. */
	for(size_t i = 0; i < INVENTORY_SIZE; i++)
		inventory_set_slot(&player->inventory, i,
			(struct item_data) {.id = BLOCK_STONE, .count = 64});
	inventory_set_slot(&player->inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = BLOCK_COBBLESTONE, .count = 63});
	e = item_at(8.5F, 101.125F, 8.5F, 2);
	uint32_t sound_before = server_local_sound_rng()->seed;
	assert(!e->tick_server(e, &s));
	assert(slot_count(player, INVENTORY_SLOT_HOTBAR) == 64);
	assert(e->data.item.item.count == 1 && e->delay_destroy < 0); // no destroy armed
	assert(server_local_sound_rng()->seed == sound_before);
	free(e);
	clear_inventory(player);

	/* EntityPlayer.onLivingUpdate runs the pickup scan only under
	 * `if(this.health > 0)` [EntityPlayer.java:196]: a dead player standing
	 * on an item never collects it; the same player alive does. */
	player->health = 0;
	e = item_at(8.5F, 101.125F, 8.5F, 1);
	assert(!e->tick_server(e, &s));
	assert(e->data.item.item.count == 1 && e->delay_destroy < 0);
	assert(slot_count(player, INVENTORY_SLOT_HOTBAR) == 0);
	player->health = MAX_PLAYER_HEALTH;
	assert(!e->tick_server(e, &s));
	assert(e->delay_destroy == 1);
	assert(slot_count(player, INVENTORY_SLOT_HOTBAR) == 1);
	free(e);
	printf("%s: despawn, box edges, partial without pop, dead/alive gate PASS\n", who);
}

int main(void) {
	test_fixture_dir("test_item_entity_parity_tmp", world_dir, sizeof world_dir);
	assert(clin_init()); blocks_init(); items_init();
	MKDIR(world_dir);
	MKDIR(test_fixture_join(world_dir, "/region"));
	string_t level; string_init_set_str(level, world_dir);
	memset(&s, 0, sizeof s);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; s.worlds[0].local = &s;
	s.scheduled_tick_budget = 128;
	s.entity_rng.seed = 0x13579BDFu;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
		&s.player, INVENTORY_SIZE, 0, 0, 0));
	assert(inventory_create(&guest.inventory, &inventory_logic_player, &s,
		&guest, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	guest.active_inventory = &guest.inventory;
	s.player.has_pos = guest.has_pos = true;
	s.player.finished_loading = guest.finished_loading = true;
	s.player.health = guest.health = MAX_PLAYER_HEALTH;
	s.guest_actor = &guest; s.guest_gameplay = true;
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 2; cx++)
		for(int cz = 0; cz <= 2; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
	test_item_rng_lifetime();
	run_actor(false);
	run_actor(true);
	draining = 0; thread_join(&drain);
	puts("test_item_entity_parity: PASS");
	return 0;
}
