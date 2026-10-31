/* G6 (vehicle-smoke-1004 O13): player death-drop scatter, Beta 1.7.3 golden.
 *
 * Spec (tmp/jobs/vehicle-smoke-1004/G6-citations.md):
 *   InventoryPlayer.dropAllItems [CFR :277-288]: mainInventory[0..35] then
 *     armorInventory[0..3] (boots..helmet), null slots skipped -- CaveWii
 *     slots 36..44, 9..35, 8, 7, 6, 5.
 *   EntityPlayer.dropPlayerItemWithRandomChoice(stack, true) [CFR :294-304]:
 *     new EntityItem(posX, posY - (double)0.3F + (double)getEyeHeight()=0.12F,
 *     posZ) -- its constructor draws 4 Math.random doubles (hoverStart, yaw,
 *     motionX, motionZ); delayBeforeCanPickup = 40; then from the PLAYER's
 *     rand: f = nextFloat()*0.5F; a = nextFloat()*(float)PI*2.0F;
 *     motionX = -MathHelper.sin(a)*f; motionZ = MathHelper.cos(a)*f;
 *     motionY = 0.2F. CaveWii's player-rand stand-in is s->rand_src (the
 *     same owner as the existing Q-drop scatter).
 *   EntityItem.onUpdate [CFR :66-102]: motionY -= 0.04F; move; air drag
 *     0.98F; motionY *= 0.98F.
 *
 * Real paths: server_local_set_player_health(0) + server_local_respawn_request
 * on (A) the journaled Nether death and (B) the Overworld death. Checks per
 * item spawn pos / motion / pickup delay / Beta order, the exact rand_src
 * and Math.random draw counts, inventory conservation, no duplicate on a
 * repeated respawn request, and 3 airborne ticks of item motion.
 */
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
#include "entity/combat.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/death_drop_journal.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/region_archive.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "worldgen/beta_math.h"
#include "worldgen/java_random.h"

static unsigned failures;
static void check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if(!ok)
		failures++;
}

static volatile int drain_run = 1;
static void* drain_client(void* unused) {
	(void)unused;
	while(drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static char s_world[3][128];
static const int k_dim[2] = {WORLD_DIM_NETHER, WORLD_DIM_OVERWORLD};
static const double k_y[2] = {70.0, 110.0}; /* eye basis; B in open air */

static void remove_tree(const char* root) {
	char cmd[512];
#ifdef _WIN32
	snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\" 2>nul", root);
	for(char* c = cmd; *c; c++)
		if(*c == '/')
			*c = '\\';
#else
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
#endif
	(void)system(cmd);
}

/* dropAllItems order: hotbar 36.. ascending, main 9.. ascending, then armor
 * boots(8) legs chest helmet(5). Every (id, count) is unique. */
#define K_STACKS 8
static const int k_slots[K_STACKS] = {
	INVENTORY_SLOT_HOTBAR + 0, INVENTORY_SLOT_HOTBAR + 1, INVENTORY_SLOT_HOTBAR + 4,
	INVENTORY_SLOT_MAIN + 0, INVENTORY_SLOT_MAIN + 1, INVENTORY_SLOT_MAIN + 11,
	INVENTORY_SLOT_ARMOR + 3, INVENTORY_SLOT_ARMOR + 0};
static const struct item_data k_stacks[K_STACKS] = {
	{.id = ITEM_APPLE, .count = 1}, {.id = ITEM_APPLE, .count = 2},
	{.id = ITEM_APPLE, .count = 3}, {.id = BLOCK_COBBLESTONE, .count = 4},
	{.id = BLOCK_COBBLESTONE, .count = 5}, {.id = BLOCK_COBBLESTONE, .count = 17},
	{.id = ITEM_LEATHER_BOOTS, .count = 1}, {.id = ITEM_LEATHER_HELMET, .count = 1}};

static void make_world(const char* root, int dim, double y) {
	remove_tree(root);
	char path[256];
	MKDIR(root);
	snprintf(path, sizeof(path), "%s/region", root);
	MKDIR(path);
	snprintf(path, sizeof(path), "%s/DIM-1", root);
	MKDIR(path);
	snprintf(path, sizeof(path), "%s/DIM-1/region", root);
	MKDIR(path);
	string_t name;
	string_init_set_str(name, root);
	struct level_archive level;
	assert(level_archive_create_new(&level, name, 424242,
									(vec3) {8.5F, (float)y, 8.5F}));
	level_archive_destroy(&level);
	assert(level_archive_create(&level, name));
	struct item_data items[INVENTORY_SIZE] = {{0}};
	/* O16 (R06 P2): SEVERAL distinguishable stacks inside the hotbar AND the
	 * main grid (each (id, count) unique), plus armor, with empty slots between
	 * (skipped) -- so an adjacent swap or a reversal inside either group fails,
	 * not only a category swap. */
	for(int i = 0; i < K_STACKS; i++)
		items[k_slots[i]] = k_stacks[i];
	struct inventory inventory = {.items = items, .capacity = INVENTORY_SIZE};
	int16_t health = MAX_PLAYER_HEALTH;
	assert(level_archive_write_player(&level, (vec3) {8.5F, (float)y, 8.5F},
									  (vec2) {0}, NULL, dim));
	assert(level_archive_write_inventory(&level, &inventory));
	assert(level_archive_write(&level, LEVEL_PLAYER_HEALTH, &health));
	assert(level_archive_save(&level));
	level_archive_destroy(&level);
	string_clear(name);
}

static void init_server(struct server_local* s, const char* root, int dim,
						double y) {
	memset(s, 0, sizeof(*s));
	s->rand_src.seed = 0x5EED1234u;
	s->container_drop_rng.seed = 0x12345678u;
	/* O16: the host's OWN Random (Beta Entity.rand), deterministic here */
	jrand_set_seed(&s->player.entity_rand.r, 0xA11CE5ED); /* F4-RNG: Java Entity.rand */
	s->player.entity_rand.seeded = true;
	string_init_set_str(s->level_name, root);
	assert(level_archive_create(&s->level, s->level_name));
	if(!level_archive_read_death_tx_counter(&s->level, &s->death_tx_counter))
		s->death_tx_counter = CAVEWII_DEATH_TX_LEGACY_FLOOR;
	assert(inventory_create(&s->player.inventory, &inventory_logic_player, s,
							&s->player, INVENTORY_SIZE, 0, 0, 0));
	assert(level_archive_read_inventory(&s->level, &s->player.inventory));
	s->player.active_inventory = &s->player.inventory;
	s->player.x = 8.5;
	s->player.y = y;
	s->player.z = 8.5;
	s->player.spawn_x = 8;
	s->player.spawn_y = (int)y;
	s->player.spawn_z = 8;
	s->player.health = MAX_PLAYER_HEALTH;
	s->player.dimension = dim;
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.fire = -PLAYER_FIRE_RESISTANCE;
	s->player.oxygen = MAX_OXYGEN;
	s->world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(s))
		abort();
	piston_moving_store_init(&s->piston_moving);
	dict_entity_init(s->entities);
	s->world_populate_scratch = server_world_populate_scratch_create();
	assert(s->world_populate_scratch);
	assert(server_world_create_with_populate_scratch(
		&s->worlds[0], s->level_name, dim, 424242, true, s->world_populate_scratch));
	/* the fixture's single resident world is slot 0 (dimension-tagged by dim) */
	s->worlds[0].local = s;
	s->worlds[0].session = s;
	/* the entity pass ticks an entity only with its +-32 block box resident */
	for(int cx = -3; cx <= 3; cx++)
		for(int cz = -3; cz <= 3; cz++) {
			struct server_chunk* chunk = NULL;
			assert(server_world_load_chunk(&s->worlds[0], cx, cz, &chunk) && chunk);
		}
}

/* item entities in creation (id) order */
static size_t items_by_id(struct server_local* s, struct entity** out, size_t cap) {
	size_t n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->delay_destroy != 0 && n < cap)
			out[n++] = e;
	}
	for(size_t i = 1; i < n; i++)
		for(size_t j = i; j > 0 && out[j - 1]->id > out[j]->id; j--) {
			struct entity* t = out[j];
			out[j] = out[j - 1];
			out[j - 1] = t;
		}
	return n;
}

static void run_path(int path) {
	const char* label = path == 0 ? "journaled Nether" : "Overworld";
	const int dim = k_dim[path];
	const double y = k_y[path];
	make_world(s_world[path], dim, y);
	struct server_local* s = calloc(1, sizeof(*s));
	assert(s);
	init_server(s, s_world[path], dim, y);
	const int64_t math_seed = 0x1234ABCDLL + path;
	server_local_test_seed_math_random(math_seed);

	/* Beta order and the inventory before death */
	struct item_data before[K_STACKS];
	for(int i = 0; i < K_STACKS; i++)
		assert(inventory_get_slot(&s->player.inventory, (size_t)k_slots[i],
								  &before[i]) && before[i].id != 0);

	/* O16 (R06 P1): the reference is the PLAYER's own stream; the world
	 * stream must come out byte-exact. */
	struct java_random ref = s->player.entity_rand.r;
	const struct random_gen world_before = s->rand_src;
	const double px = s->player.x, py = s->player.y, pz = s->player.z;

	assert(server_local_set_player_health(s, 0));
	assert(server_local_respawn_request(s));

	struct entity* got[16];
	size_t n = items_by_id(s, got, 16);
	char msg[200];
	snprintf(msg, sizeof(msg), "%s: exactly %d death drops (empty slots skipped)", label,
			 K_STACKS);
	check(n == K_STACKS, msg);
	const float want_y = (float)(py - (double)0.3F + (double)0.12F);
	bool order_ok = true, pos_ok = true, vel_ok = true, delay_ok = true;
	for(size_t i = 0; i < n && i < K_STACKS; i++) {
		struct entity* e = got[i];
		order_ok = order_ok && e->data.item.item.id == before[i].id
			&& e->data.item.item.count == before[i].count;
		pos_ok = pos_ok && e->pos[0] == (float)px && e->pos[2] == (float)pz
			&& e->pos[1] == want_y;
		float f = jrand_next_float(&ref) * 0.5F;
		float a = jrand_next_float(&ref) * (float)M_PI * 2.0F;
		float vx = -mh_sin(a) * f, vz = mh_cos(a) * f;
		vel_ok = vel_ok && e->vel[0] == vx && e->vel[2] == vz
			&& e->vel[1] == 0.2F;
		delay_ok = delay_ok && e->data.item.pickup_delay == 40;
		if(getenv("G6_DBG"))
			printf("  item %zu id=%u pos=%.6f,%.6f,%.6f vel=%.6f,%.6f,%.6f want=%.6f,%.6f\n",
				   i, (unsigned)e->data.item.item.id, (double)e->pos[0],
				   (double)e->pos[1], (double)e->pos[2], (double)e->vel[0],
				   (double)e->vel[1], (double)e->vel[2], (double)vx, (double)vz);
	}
	snprintf(msg, sizeof(msg), "%s: Beta order hotbar asc -> main asc -> armor boots..helmet",
			 label);
	check(order_ok, msg);
	snprintf(msg, sizeof(msg), "%s: spawn at posY - 0.3F + eyeHeight 0.12F", label);
	check(pos_ok, msg);
	snprintf(msg, sizeof(msg), "%s: motion -sin(a)*f, 0.2F, cos(a)*f from the player's OWN rand",
			 label);
	check(vel_ok, msg);
	snprintf(msg, sizeof(msg), "%s: pickup delay 40", label);
	check(delay_ok, msg);
	/* exact draw counts: 2 player-rand floats and 4 Math.random per item */
	snprintf(msg, sizeof(msg), "%s: player rand advanced exactly 2 draws per item", label);
	check(s->player.entity_rand.r.seed == ref.seed, msg);
	snprintf(msg, sizeof(msg), "%s: the WORLD stream (rand_src) is byte-exact across the death",
			 label);
	check(memcmp(&s->rand_src, &world_before, sizeof(world_before)) == 0, msg);
	struct java_random mref;
	jrand_set_seed(&mref, math_seed);
	for(int i = 0; i < 4 * K_STACKS; i++)
		(void)jrand_next_double(&mref);
	struct server_local_math_random_state now;
	server_local_math_random_capture(&now);
	snprintf(msg, sizeof(msg), "%s: Math.random advanced exactly 4 draws per item", label);
	check(now.rng.seed == mref.seed, msg);
	/* conservation */
	bool empty = true;
	for(size_t slot = 0; slot < INVENTORY_SIZE; slot++) {
		struct item_data it;
		if(inventory_get_slot(&s->player.inventory, slot, &it) && it.id != 0)
			empty = false;
	}
	snprintf(msg, sizeof(msg), "%s: inventory conserved (all moved to drops, none left)", label);
	check(empty && n == K_STACKS, msg);
	/* a repeated respawn request never duplicates */
	(void)server_local_respawn_request(s);
	struct entity* again[16];
	snprintf(msg, sizeof(msg), "%s: a repeated respawn request spawns no duplicate", label);
	check(items_by_id(s, again, 16) == n, msg);
	/* item physics: 3 airborne ticks (Overworld, open air) */
	if(path == 1 && n > 0) {
		struct entity* e = got[0];
		double x = e->pos[0], yy = e->pos[1], vx = e->vel[0], vy = e->vel[1];
		for(int t = 0; t < 3; t++) {
			vy -= (double)0.04F;
			x += vx;
			yy += vy;
			vx *= (double)0.98F;
			vy *= (double)0.98F;
		}
		uint32_t id = e->id;
		/* the item's own server tick (EntityItem.onUpdate); the respawn
		 * committed a death world switch that pauses the shared entity pass */
		for(int t = 0; t < 3; t++)
			(void)e->tick_server(e, s);
		struct entity** ep = dict_entity_get(s->entities, id);
		if(getenv("G6_DBG") && ep && *ep)
			printf("  tick: got %.6f,%.6f want %.6f,%.6f\n", (double)(*ep)->pos[0],
				   (double)(*ep)->pos[1], x, yy);
		snprintf(msg, sizeof(msg), "%s: 3 airborne ticks follow EntityItem.onUpdate", label);
		check(ep && *ep && fabs((double)(*ep)->pos[0] - x) < 1e-4
				  && fabs((double)(*ep)->pos[1] - yy) < 1e-4, msg);
	}
}

/* R08 F1 + O21: the journaled (Nether) death closes the player's 2x2 grid and
 * cursor first [CFR Container.onCraftGuiClosed:179-183, ContainerPlayer:70-78]
 * through EntityPlayer.dropPlayerItem -> dropPlayerItemWithRandomChoice(
 * stack, false) [:290-315]: the PLAYER's rand, 4 draws per item (angle,
 * magnitude, y-a, y-b), never World.rand. Then dropAllItems' scatter, 2 per
 * stack, from the same player stream. The world stream stays byte-exact. */
static void run_close_sources(void) {
	make_world(s_world[2], k_dim[0], k_y[0]);
	struct server_local* s = calloc(1, sizeof(*s));
	assert(s);
	init_server(s, s_world[2], k_dim[0], k_y[0]);
	server_local_test_seed_math_random(0x0C105EDLL);
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_CRAFTING + 0,
					   (struct item_data) {.id = BLOCK_PLANKS, .count = 3});
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_CRAFTING + 2,
					   (struct item_data) {.id = BLOCK_DIRT, .count = 6});
	inventory_set_picked_item(&s->player.inventory,
							  (struct item_data) {.id = ITEM_APPLE, .count = 7});
	const struct random_gen world_before = s->rand_src;
	struct java_random ref = s->player.entity_rand.r;
	assert(server_local_set_player_health(s, 0));
	assert(server_local_respawn_request(s));
	struct entity* got[24];
	size_t n = items_by_id(s, got, 24);
	char msg[200];
	snprintf(msg, sizeof(msg), "F1: 3 close drops + %d death drops", K_STACKS);
	check(n == 3 + K_STACKS, msg);
	bool vel_ok = n >= 3;
	for(size_t i = 0; i < 3 && i < n; i++) {
		float yaw = s->player.rx * (float)M_PI / 180.0f;
		float pitch = s->player.ry * (float)M_PI / 180.0f;
		float vx = -sinf(yaw) * cosf(pitch) * 0.3f;
		float vz = cosf(yaw) * cosf(pitch) * 0.3f;
		float vy = -sinf(pitch) * 0.3f + 0.1f;
		float a = jrand_next_float(&ref), b = jrand_next_float(&ref);
		float c = jrand_next_float(&ref), d = jrand_next_float(&ref);
		float f1 = a * (float)M_PI * 2.0f, f2 = 0.02f * b;
		vx += cosf(f1) * f2;
		vy += (c - d) * 0.1f;
		vz += sinf(f1) * f2;
		vel_ok = vel_ok && fabsf(got[i]->vel[0] - vx) <= 1e-6F
			&& fabsf(got[i]->vel[1] - vy) <= 1e-6F
			&& fabsf(got[i]->vel[2] - vz) <= 1e-6F;
	}
	check(vel_ok, "F1: each close drop is Beta's dropPlayerItem from the PLAYER's rand (4 draws)");
	for(int i = 0; i < 2 * K_STACKS; i++)
		(void)jrand_next_float(&ref);
	check(s->player.entity_rand.r.seed == ref.seed,
		  "F1: player stream advanced 4 per close drop + 2 per death drop");
	check(memcmp(&s->rand_src, &world_before, sizeof(world_before)) == 0,
		  "F1: the world stream is byte-exact across a death with close sources");
}

/* O16 (R06 P1): allocation failure on the Overworld death path. A refused
 * slot keeps its stack; its drop consumes no draw from EITHER stream; the
 * world stream stays byte-exact and the player stream advances exactly two
 * draws per drop that was actually made. */
static void run_oom(void) {
	const double y = k_y[1];
	make_world(s_world[2], k_dim[1], y);
	struct server_local* s = calloc(1, sizeof(*s));
	assert(s);
	init_server(s, s_world[2], k_dim[1], y);
	server_local_test_seed_math_random(0x0BADC0DELL);
	const struct random_gen world_before = s->rand_src;
	struct java_random ref = s->player.entity_rand.r;
	assert(server_local_set_player_health(s, 0));
	server_local_test_fail_entity_alloc_after(2);
	(void)server_local_respawn_request(s);
	server_local_test_fail_entity_alloc_after(0);
	struct entity* got[16];
	size_t n = items_by_id(s, got, 16);
	size_t kept = 0;
	for(size_t slot = 0; slot < INVENTORY_SIZE; slot++) {
		struct item_data it;
		if(inventory_get_slot(&s->player.inventory, slot, &it) && it.id != 0)
			kept++;
	}
	printf("oom: drops=%zu kept=%zu\n", n, kept);
	check(kept >= 1 && n + kept == K_STACKS,
		  "OOM: every stack is a drop or retained (none lost, none doubled)");
	for(size_t i = 0; i < 2 * n; i++)
		(void)jrand_next_float(&ref);
	check(s->player.entity_rand.r.seed == ref.seed,
		  "OOM: the player stream advanced exactly 2 draws per MADE drop");
	check(memcmp(&s->rand_src, &world_before, sizeof(world_before)) == 0,
		  "OOM: the world stream is byte-exact");
}

/* O17 (R07 F1): a REFUSED first death-drop attempt restores the actor's
 * Random to its exact pre-call state -- including the never-seeded state
 * (seed 0) -- on both the helper path (allocation refused) and the
 * journaled planner's rollback (region payload write refused). Seeded
 * controls restore their seeded state. The world stream is byte-exact. */
static void run_first_refusal(void) {
	for(int seeded = 0; seeded < 2; seeded++) {
		const char* label = seeded ? "seeded control" : "never-seeded";
		/* (1) helper: allocation refused on the first item */
		make_world(s_world[2], k_dim[1], k_y[1]);
		struct server_local* s = calloc(1, sizeof(*s));
		assert(s);
		init_server(s, s_world[2], k_dim[1], k_y[1]);
		s->player.entity_rand = (struct server_body_rand) {0};
		if(seeded) {
			jrand_set_seed(&s->player.entity_rand.r, 0x51EED);
			s->player.entity_rand.seeded = true;
		}
		const struct server_body_rand player_before = s->player.entity_rand;
		const struct random_gen world_before = s->rand_src;
		struct item_data stack = {.id = BLOCK_COBBLESTONE, .count = 5};
		server_local_test_fail_entity_alloc_after(1);
		struct entity* e = server_local_spawn_death_item_w(&s->worlds[0], &s->player,
			s->player.x, s->player.y, s->player.z, &stack);
		server_local_test_fail_entity_alloc_after(0);
		char msg[160];
		snprintf(msg, sizeof(msg), "F1 helper (%s): the refused drop returns NULL", label);
		check(e == NULL, msg);
		snprintf(msg, sizeof(msg),
				 "F1 helper (%s): the actor Random is its exact pre-call state", label);
		check(memcmp(&s->player.entity_rand, &player_before, sizeof(player_before)) == 0,
			  msg);
		snprintf(msg, sizeof(msg), "F1 helper (%s): the world stream is byte-exact", label);
		check(memcmp(&s->rand_src, &world_before, sizeof(world_before)) == 0, msg);

		/* (2) journaled planner: the PREPARED payload write is refused, the
		 * whole death transaction rolls back */
		make_world(s_world[2], k_dim[0], k_y[0]);
		s = calloc(1, sizeof(*s));
		assert(s);
		init_server(s, s_world[2], k_dim[0], k_y[0]);
		s->player.entity_rand = (struct server_body_rand) {0};
		if(seeded) {
			jrand_set_seed(&s->player.entity_rand.r, 0x51EED);
			s->player.entity_rand.seeded = true;
		}
		const struct server_body_rand jplayer_before = s->player.entity_rand;
		const struct random_gen jworld_before = s->rand_src;
		assert(server_local_set_player_health(s, 0));
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
		bool accepted = server_local_respawn_request(s);
		struct entity* got[16];
		size_t n = items_by_id(s, got, 16);
		printf("F1 journal (%s): accepted=%d drops=%zu\n", label, accepted, n);
		snprintf(msg, sizeof(msg),
				 "F1 journal (%s): the refused transaction made no drop", label);
		check(!accepted && n == 0, msg);
		snprintf(msg, sizeof(msg),
				 "F1 journal (%s): the actor Random is its exact pre-call state", label);
		check(memcmp(&s->player.entity_rand, &jplayer_before, sizeof(jplayer_before)) == 0,
			  msg);
		snprintf(msg, sizeof(msg), "F1 journal (%s): the world stream is byte-exact",
				 label);
		check(memcmp(&s->rand_src, &jworld_before, sizeof(jworld_before)) == 0, msg);
	}
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_death_scatter_golden_n", s_world[0], sizeof s_world[0]);
	test_fixture_dir("test_death_scatter_golden_o", s_world[1], sizeof s_world[1]);
	test_fixture_dir("test_death_scatter_golden_m", s_world[2], sizeof s_world[2]);
	assert(clin_init());
	assert(svin_init());
	blocks_init();
	items_init();
	struct thread drain;
	assert(thread_create(&drain, drain_client, NULL, 64));
	run_path(0);
	run_path(1);
	run_oom();
	run_first_refusal();
	run_close_sources();
	drain_run = 0;
	thread_join(&drain);
	printf("%s test_death_scatter_golden (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
