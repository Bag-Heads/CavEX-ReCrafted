/*
	next-gaps-0924 W3 SIM s1: every heal path closes the hurt window the way
	Beta 1.7.3 EntityLiving.heal does.

	Every expected value below is derived by hand from the decompile
	(mcb173-retromcp/minecraft/src_original/net/minecraft/src/), never read
	back from CaveWii:

	  EntityLiving.java:296-305  heal(n): if(health > 0) { health += n;
	                             cap 20; heartsLife = heartsHalvesLife / 2; }
	  EntityLiving.java:6        heartsHalvesLife = 20  -> heal leaves 10
	  EntityLiving.java:317      attackEntityFrom absorbs only while
	                             (float)heartsLife > (float)heartsHalvesLife/2F,
	                             i.e. > 10 -- so after a heal the next hit is a
	                             FRESH hit: full damage, window re-armed (:326-331)
	  EntityLiving.java:151-152  heartsLife-- once per living tick
	  ItemFood.java:14-18        onItemRightClick -> heal(healAmount)
	  Item.java:12,34,49,71,72,74,101,102,109  heal amounts: apple 4, stew 10,
	                             bread 5, raw pork 3, cooked pork 8, golden
	                             apple 42, raw fish 2, cooked fish 5, cookie 1
	  BlockCake.java:67-68       eatCakeSlice: health < 20 -> heal(3)
	  EntityWolf.java:345-351    wolf fed its favourite meat -> this.heal(
	                             porkRaw.getHealAmount()) = heal(3)

	CaveWii counts only the part of heartsLife ABOVE 10 (hurt_resist_time,
	PLAYER_HURT_RESIST_TICKS / MOB_HURT_RESIST_TICKS = 10), so Beta's
	"heartsLife = 10" is hurt_resist_time = 0 in engine terms. Player health is
	Beta half-hearts x8 (MAX_PLAYER_HEALTH 160); wolf health is Beta units.

	The scripted sequence per heal source (all in one tick, no decay):
	  hit 32 (4 half-hearts, fresh) -> heal -> hit 32 again.
	Beta: the second hit is fresh (heartsLife 10 is not > 10) and lands in full.
	Before this card CaveWii kept the window open across food/cake and
	absorbed it.
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
#include "block/blocks_data.h"
#include "block/piston_moving.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_wolf.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 70 /* superflat: first air layer above the surface */

/* One Beta half-heart in engine player units. */
#define HH (HEALTH_PER_HEART / 2)
#define HIT (4 * HH) /* a 4-half-heart hit, any flat source */

static struct server_local s;
static volatile int g_drain_run = 1;
static int g_failures;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

#define CHECK(cond, ...)                                                   \
	do {                                                                   \
		if(!(cond)) {                                                      \
			g_failures++;                                                  \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
			printf(__VA_ARGS__);                                           \
			printf("\n");                                                  \
		}                                                                  \
	} while(0)

static void reset_player(void) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.active_inventory = &s.player.inventory;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.damage_remainder = 0;
	s.player.fire = 0;
	s.player.vel_x = s.player.vel_z = 0.0f;
	s.player.x = 8.5;
	s.player.y = Y + PLAYER_EYE_HEIGHT;
	s.player.z = 8.5;
	for(size_t slot = INVENTORY_SLOT_ARMOR;
		slot < INVENTORY_SLOT_ARMOR + INVENTORY_SIZE_ARMOR; slot++)
		inventory_clear_slot(&s.player.inventory, slot);
}

/* Beta heal amounts (Item.java) in half-hearts. */
static const struct {
	const char* name;
	int item_id;
	int beta_heal;
} FOODS[] = {
	{"apple", ITEM_APPLE, 4},
	{"mushroom stew", ITEM_MUSHROOM_STEW, 10},
	{"bread", ITEM_BREAD, 5},
	{"raw porkchop", ITEM_PORKCHOP, 3},
	{"cooked porkchop", ITEM_PORKCHOP_COOKED, 8},
	{"golden apple", ITEM_APPLE_GOLDEN, 42},
	{"raw fish", ITEM_FISH, 2},
	{"cooked fish", ITEM_FISH_COOKED, 5},
	{"cookie", ITEM_COOKIE, 1},
};

/* Hand-evaluated Beta sequence: 20 -> hit 4 (fresh) -> heal(n) -> hit 4. */
static void beta_expect(int beta_heal, int* after_heal_hh, int* after_hit2_hh) {
	int h = 20 - 4;             /* fresh hit, heartsLife = 20 */
	h += beta_heal;             /* heal: health > 0 */
	if(h > 20)
		h = 20;                 /* cap; heartsLife = 10 */
	*after_heal_hh = h;
	*after_hit2_hh = h - 4;     /* 10 > 10.0F false -> fresh, full damage */
}

static void first_hit(const char* what) {
	reset_player();
	(void)server_local_damage_player(&s, HIT, 0.0f, 0.0f);
	CHECK(s.player.health == MAX_PLAYER_HEALTH - HIT
			  && s.player.hurt_resist_time == PLAYER_HURT_RESIST_TICKS,
		  "%s: setup hit (health %d, window %d)", what, s.player.health,
		  s.player.hurt_resist_time);
}

static void second_hit_and_check(const char* what, int after_heal_hh,
								 int after_hit2_hh) {
	CHECK(s.player.health == after_heal_hh * HH,
		  "%s: health after heal %d, want %d", what, s.player.health,
		  after_heal_hh * HH);
	CHECK(s.player.hurt_resist_time == 0,
		  "%s: heal must leave heartsLife 10 = hurt window closed (got %d)",
		  what, s.player.hurt_resist_time);
	(void)server_local_damage_player(&s, HIT, 0.0f, 0.0f);
	CHECK(s.player.health == after_hit2_hh * HH,
		  "%s: second equal hit after the heal must land in full: health %d, "
		  "want %d",
		  what, s.player.health, after_hit2_hh * HH);
	CHECK(s.player.hurt_resist_time == PLAYER_HURT_RESIST_TICKS,
		  "%s: second hit re-arms the window (got %d)", what,
		  s.player.hurt_resist_time);
}

static void test_food(void) {
	for(size_t i = 0; i < sizeof FOODS / sizeof FOODS[0]; i++) {
		int want_heal, want_hit2;
		beta_expect(FOODS[i].beta_heal, &want_heal, &want_hit2);
		first_hit(FOODS[i].name);
		struct item_data it = {.id = FOODS[i].item_id, .count = 1};
		assert(items[FOODS[i].item_id]
			   && items[FOODS[i].item_id]->onItemPlace);
		/* The stew hook drops its bowl at `where`, so pass a real cell. */
		struct block_data air = {.type = BLOCK_AIR};
		struct block_info where = {.block = &air, .x = 8, .y = Y, .z = 8};
		bool eaten = items[FOODS[i].item_id]->onItemPlace(
			&s, &s.player, &it, &where, &where, SIDE_TOP);
		CHECK(eaten, "%s: eaten below full health", FOODS[i].name);
		second_hit_and_check(FOODS[i].name, want_heal, want_hit2);
	}
	puts("  food: every ItemFood heal closes the hurt window (heartsLife 10)");
}

/* W3-s5 (Astra ruling B, docs/wii_controller_playability.md #30): at full
 * health every food is REFUSED -- a deliberate carve-out #2 deviation, since
 * Beta's ItemFood.onItemRightClick [ItemFood.java:14-18] has no gate and
 * always consumes. Pinned: not used (the caller keeps the stack), health
 * unchanged, the hurt window and last_damage untouched (no heal ran), and
 * no bowl for the stew. */
static void test_full_health_refusal(void) {
	for(size_t i = 0; i < sizeof FOODS / sizeof FOODS[0]; i++) {
		reset_player();
		s.player.hurt_resist_time = 6;
		s.player.last_damage = 24;
		struct item_data it = {.id = FOODS[i].item_id, .count = 1};
		struct item_data before = it;
		size_t entities_before = dict_entity_size(s.entities);
		struct block_data air = {.type = BLOCK_AIR};
		struct block_info where = {.block = &air, .x = 8, .y = Y, .z = 8};
		bool used = items[FOODS[i].item_id]->onItemPlace(
			&s, &s.player, &it, &where, &where, SIDE_TOP);
		CHECK(!used, "%s: must be refused at full health (carve-out #2)",
			  FOODS[i].name);
		CHECK(it.id == before.id && it.count == before.count,
			  "%s: held stack changed on a refusal", FOODS[i].name);
		CHECK(s.player.health == MAX_PLAYER_HEALTH,
			  "%s: health %d after refusal", FOODS[i].name, s.player.health);
		CHECK(s.player.hurt_resist_time == 6 && s.player.last_damage == 24,
			  "%s: a refusal must not touch the hurt window (%d/%d)",
			  FOODS[i].name, s.player.hurt_resist_time, s.player.last_damage);
		CHECK(dict_entity_size(s.entities) == entities_before,
			  "%s: a refusal spawned an entity", FOODS[i].name);
	}
	puts("  full health: all nine foods refused, nothing consumed "
		 "(carve-out #2, Astra ruling B)");
}

/* W3-s6: the split GUEST body. heal() is per EntityLiving instance
 * [EntityLiving.java:296-305], so the guest's own bite closes the GUEST's
 * window only, the reply rides the guest's envelope, and the host's open
 * window is untouched. Same Beta sequence as the host rows. */
static struct server_player s_guest;
static int s_guest_health_replies;
static void guest_reply_probe(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_PLAYER_SET_HEALTH)
		s_guest_health_replies++;
}

static void test_guest_food(void) {
	assert(inventory_create(&s_guest.inventory, &inventory_logic_player, &s,
							&s_guest, INVENTORY_SIZE, 0, 0, 0));
	s_guest.inventory.reply = guest_reply_probe;
	s_guest.active_inventory = &s_guest.inventory;
	s_guest.has_pos = s_guest.finished_loading = true;
	s_guest.x = 12.5;
	s_guest.y = Y + PLAYER_EYE_HEIGHT;
	s_guest.z = 12.5;
	s_guest.health = MAX_PLAYER_HEALTH;
	s.guest_actor = &s_guest;

	reset_player();
	s.player.hurt_resist_time = 7; /* the host's own open window */
	s.player.last_damage = 40;

	int want_heal, want_hit2;
	beta_expect(4 /* apple */, &want_heal, &want_hit2);
	(void)server_local_damage_actor(&s, &s_guest, HIT, 0.0f, 0.0f);
	CHECK(s_guest.health == MAX_PLAYER_HEALTH - HIT
			  && s_guest.hurt_resist_time == PLAYER_HURT_RESIST_TICKS,
		  "guest: setup hit (health %d, window %d)", s_guest.health,
		  s_guest.hurt_resist_time);
	s_guest_health_replies = 0;
	struct item_data it = {.id = ITEM_APPLE, .count = 1};
	CHECK(items[ITEM_APPLE]->onItemPlace(&s, &s_guest, &it, NULL, NULL,
										 SIDE_TOP),
		  "guest: apple eaten below full health");
	CHECK(s_guest.health == want_heal * HH, "guest: health after heal %d, "
		  "want %d", s_guest.health, want_heal * HH);
	CHECK(s_guest.hurt_resist_time == 0,
		  "guest: heal must close the GUEST window (got %d)",
		  s_guest.hurt_resist_time);
	CHECK(s_guest_health_replies == 1,
		  "guest: health reply must ride the guest envelope (%d)",
		  s_guest_health_replies);
	CHECK(s.player.hurt_resist_time == 7 && s.player.last_damage == 40
			  && s.player.health == MAX_PLAYER_HEALTH,
		  "guest heal touched the HOST (window %d, last %d, health %d)",
		  s.player.hurt_resist_time, s.player.last_damage, s.player.health);
	(void)server_local_damage_actor(&s, &s_guest, HIT, 0.0f, 0.0f);
	CHECK(s_guest.health == want_hit2 * HH,
		  "guest: second equal hit must land in full: health %d, want %d",
		  s_guest.health, want_hit2 * HH);

	s.guest_actor = NULL;
	inventory_destroy(&s_guest.inventory, &s_guest);
	puts("  guest: the guest's own bite closes the guest window only");
}

static void test_cake(void) {
	const w_coord_t cx = 12, cy = Y, cz = 12;
	server_world_set_block(&s.worlds[0], cx, cy, cz,
						   (struct block_data) {.type = BLOCK_CAKE,
												.metadata = 0});
	int want_heal, want_hit2;
	beta_expect(3, &want_heal, &want_hit2);
	first_hit("cake");
	struct block_data cake = {.type = BLOCK_CAKE, .metadata = 0};
	struct block_info on = {.block = &cake, .x = cx, .y = cy, .z = cz};
	struct item_data held = {0};
	assert(blocks[BLOCK_CAKE] && blocks[BLOCK_CAKE]->onRightClick);
	blocks[BLOCK_CAKE]->onRightClick(&s, &s.player, &held, &on, &on,
									 SIDE_TOP);
	struct block_data after;
	CHECK(server_world_get_block(&s.worlds[0], cx, cy, cz, &after)
			  && after.type == BLOCK_CAKE && after.metadata == 1,
		  "cake: one bite taken");
	second_hit_and_check("cake", want_heal, want_hit2);
	puts("  cake: BlockCake.eatCakeSlice heal(3) closes the hurt window");
}

/* The Peaceful regen path already closed the window (W1F); pin it through
 * the same shared heal entry so the single writer cannot regress. */
static void test_heal_entry_direct(void) {
	first_hit("heal entry");
	CHECK(server_local_heal_actor(&s, &s.player,
								  (short)(s.player.health + HH)),
		  "heal entry: accepted");
	second_hit_and_check("heal entry", 17, 13);

	/* A corpse is not healed and its state is untouched
	 * (EntityLiving.heal: `if(this.health > 0)`). */
	reset_player();
	s.player.health = 0;
	s.player.hurt_resist_time = 4;
	CHECK(!server_local_heal_actor(&s, &s.player, (short)HH),
		  "corpse: heal rejected");
	CHECK(s.player.health == 0 && s.player.hurt_resist_time == 4,
		  "corpse: health %d window %d unchanged", s.player.health,
		  s.player.hurt_resist_time);
	puts("  heal entry: closes the window; a corpse is untouched");
}

#define WOLF_ID 0x7FFF0924u /* clear of the ids the stew bowl drop allocates */
static void test_wolf(void) {
	struct entity* wolf = calloc(1, sizeof(*wolf));
	assert(wolf);
	entity_monster(WOLF_ID, wolf, true, &s.worlds[0], MONSTER_WOLF);
	struct entity** slot = dict_entity_safe_get(s.entities, WOLF_ID);
	assert(slot && !*slot);
	*slot = wolf;
	wolf->data.monster.wolf_flags = WOLF_FLAG_TAMED;
	strcpy(wolf->data.monster.wolf_owner, WOLF_LOCAL_PLAYER_NAME);

	/* Tamed, hurt (health 10), inside the arbitration window. */
	wolf->health = 10;
	wolf->data.monster.wolf_watched_health = 10;
	wolf->data.monster.hurt_resist_time = 7;
	wolf->data.monster.last_damage = 4;
	struct item_data pork = {.id = ITEM_PORKCHOP, .count = 2};
	CHECK(entity_wolf_interact(wolf, &s, &pork) == WOLF_INTERACT_HANDLED,
		  "wolf: fed");
	CHECK(pork.count == 1, "wolf: one porkchop consumed");
	CHECK(wolf->health == 13, "wolf: heal(3) -> 13, got %d", wolf->health);
	CHECK(wolf->data.monster.hurt_resist_time == 0,
		  "wolf: heal must close the window (heartsLife 10), got %d",
		  wolf->data.monster.hurt_resist_time);

	/* Near-full: cap at 20, window still closed. */
	wolf->health = 19;
	wolf->data.monster.wolf_watched_health = 19;
	wolf->data.monster.hurt_resist_time = 9;
	CHECK(entity_wolf_interact(wolf, &s, &pork) == WOLF_INTERACT_HANDLED,
		  "wolf: fed near full");
	CHECK(wolf->health == 20, "wolf: heal caps at 20, got %d", wolf->health);
	CHECK(wolf->data.monster.hurt_resist_time == 0,
		  "wolf: capped heal also closes the window, got %d",
		  wolf->data.monster.hurt_resist_time);

	dict_entity_erase(s.entities, WOLF_ID);
	free(wolf);
	puts("  wolf: EntityWolf.interact heal(3) caps at 20, closes the window");
}

int main(void) {
	test_fixture_dir("test_heal_hurt_window_golden_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	puts("test_heal_hurt_window_golden: Beta EntityLiving.heal hurt window "
		 "(next-gaps-0924 W3 s1)");
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.-1.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0x5E1A0924u;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	struct server_chunk* sc = NULL;
	for(w_coord_t cx = -1; cx <= 1; cx++)
		for(w_coord_t cz = -1; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	test_food();
	test_full_health_refusal();
	test_cake();
	test_heal_entry_direct();
	test_guest_food();
	test_wolf();

	inventory_destroy(&s.player.inventory, &s.player);
	g_drain_run = 0;
	thread_join(&drain);
	if(g_failures) {
		printf("test_heal_hurt_window_golden: FAIL (%d mismatches)\n",
			   g_failures);
		return 1;
	}
	puts("test_heal_hurt_window_golden: PASS");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
