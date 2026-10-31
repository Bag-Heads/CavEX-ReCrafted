/*
	Beta 1.7.3 storage (type 1) + furnace (type 2) minecart goldens
	(minecart-slice, MINECART_DISPATCH.md).

	Every expected value below is HAND-DERIVED from the cited decompile
	arithmetic, never by running CaveWii's own implementation and recording
	its output (CODESTYLE.md S4.2, the poisoned-golden rule).

	Ground truth (decompile 740c583):

	EntityMinecart.attackEntityFrom (destruction, order matters):
	  1. dropItemWithOffset(Item.minecartEmpty.shiftedIndex, 1, 0.0F) -- ALWAYS.
	  2. minecartType==1: drop every occupied cargoItems slot (setEntityDead's
	     drain loop is what actually spawns them; attackEntityFrom's own SECOND
	     loop runs after that drain already emptied every stack to 0, so it is
	     an observable no-op -- see docs/deviations.md), THEN
	     dropItemWithOffset(Block.chest.blockID, 1, 0.0F).
	  3. minecartType==2: dropItemWithOffset(Block.stoneOvenIdle.blockID, 1, 0.0F)
	     (the IDLE furnace id 61, never the lit id 62).
	  Entity.dropItemWithOffset(id,count,yOff) = entityDropItem(new
	  ItemStack(id,count,0), yOff): ONE EntityItem, no random XZ jitter.

	EntityMinecart.getSizeInventory() / TileEntityChest.getSizeInventory(): 27
	(both cited; MINECART_CHEST_SLOTS in entity.h).

	EntityMinecart.interact(player):
	  type 1: displayGUIChest(this) -- server-only (!multiplayerWorld guard).
	  type 2: held==coal -> --stackSize (0 -> clear slot), fuel += 1200
	          (UNCAPPED upward in Beta's own runtime int); UNCONDITIONALLY
	          (coal or not) pushX = posX-playerX, pushZ = posZ-playerZ.

	EntityMinecart.onUpdate, unridden else-branch (furnace push physics),
	source order: [1] type==2 push block precedes [2] the shared 0.96 friction,
	both apply on a pushing furnace cart:
	  [1] mag = sqrt(pushX^2+pushZ^2)
	      mag>0.01: pushX/=mag; pushZ/=mag (write-back);
	                motion = motion*0.800000011920929 + push*0.04
	      else:     motion = motion*0.8999999761581421
	  [2] motion *= 0.9599999785423279 (shared, unconditional, unridden)
	Later (after the rail-height energy exchange, before the powered boost),
	OUTSIDE the riddenByEntity branch entirely:
	  [3] mag>0.01 AND motion^2>0.001: renormalise push; dot = push.motion;
	      dot<0 -> push=(0,0); else -> push = motion (UNnormalised).
	Fuel (var6 mirrors "actively pushed this tick", i.e. branch [1]'s mag>0.01
	case only): var6 && rand.nextInt(4)==0 -> --fuel; fuel<0 -> push=(0,0).
	The `&&` short-circuits: the roll itself is skipped, not just its effect,
	whenever the cart did not just take the pushing branch.

	ItemMinecart(int id, int type): Item.minecartCrate = new
	ItemMinecart(86,1) -> id 342, type 1. Item.minecartPowered = new
	ItemMinecart(87,2) -> id 343, type 2.
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
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_minecart.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/death_drop_journal.h"
#include "network/entity_archive.h"
#include "network/inventory_logic.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "worldgen/java_random.h"
#include "support/clin_drain.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_minecart_types_env_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 /* stone floor */
#define RY 101 /* rail plane */
#define FAR_AWAY 5000.0 /* keeps the player-nudge section inert */

extern struct item item_minecart_chest;
extern struct item item_minecart_furnace;

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

static void set(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
								  (struct block_data) {
									  .type = (uint8_t)type,
									  .metadata = (uint8_t)(meta & 0x0F),
								  }));
}

/* A short straight NS rail (meta 0) on a stone floor, `len` tiles long
   starting at (x,z), so hx=0/hz=+1 seeds deterministically and every tile
   shares one flat shape (no slope-height/energy-exchange interference). */
static void make_ns_rail(int x, int z, int len) {
	for(int i = 0; i < len; i++) {
		set(x, YF, z + i, BLOCK_STONE, 0);
		set(x, RY, z + i, BLOCK_RAIL, 0);
	}
}

static void park_player_far(void) {
	s.player.x = FAR_AWAY;
	s.player.y = FAR_AWAY;
	s.player.z = FAR_AWAY;
}

/* Direct construction (test_entity_persistence_env.c's add_cart pattern):
   registers into s.entities without the full placement/publish pipeline, for
   sub-tests that hand-set physics state rather than exercising placement
   (placement itself is covered separately by test_item_ids_and_placement). */
static struct entity* make_cart(uint32_t id, uint8_t minecart_type, vec3 pos) {
	struct entity** pp = dict_entity_safe_get(s.entities, id);
	*pp = malloc(sizeof(**pp));
	assert(*pp);
	struct entity* e = *pp;
	memset(e, 0xBE, sizeof(*e)); /* poison: an uninit'd new field reads garbage */
	entity_minecart(id, e, true, &s.worlds[0]);
	e->data.minecart.minecart_type = minecart_type;
	e->teleport(e, pos);
	return e;
}

static void free_cart(struct entity* e) {
	uint32_t id = e->id;
	free(e);
	dict_entity_erase(s.entities, id);
}

/* ===================================================================== *
 * RPC observation (destruction drops, window open/slots, occupancy)
 * ===================================================================== */

#define MAX_OBSERVED_DROPS 64
static struct item_data g_dropped_items[MAX_OBSERVED_DROPS];
static size_t g_dropped_count;
static unsigned g_open_window_count;
static struct client_rpc g_last_open_window;
static unsigned g_inventory_slot_count;
static struct client_rpc g_last_inventory_slot;
static unsigned g_occupancy_count;
static bool g_last_occupied;

static void observe_rpc(const struct client_rpc* call, void* user) {
	(void)user;
	switch(call->type) {
	case CRPC_SPAWN_ITEM:
		if(g_dropped_count < MAX_OBSERVED_DROPS)
			g_dropped_items[g_dropped_count++] = call->payload.spawn_item.item;
		break;
	case CRPC_OPEN_WINDOW:
		g_open_window_count++;
		g_last_open_window = *call;
		break;
	case CRPC_INVENTORY_SLOT:
		g_inventory_slot_count++;
		g_last_inventory_slot = *call;
		break;
	case CRPC_MINECART_OCCUPANCY:
		g_occupancy_count++;
		g_last_occupied = call->payload.minecart_occupancy.occupied;
		break;
	default: break;
	}
}

static void reset_drop_observer(void) {
	g_dropped_count = 0;
}

static bool observed_drop(uint16_t id, uint8_t count) {
	for(size_t i = 0; i < g_dropped_count; i++)
		if(g_dropped_items[i].id == id && g_dropped_items[i].count == count)
			return true;
	return false;
}

/* ===================================================================== *
 * Test 1: item ids 342/343 wire to the right type via the real placement
 * path (item_get -> onItemPlace -> server_local_spawn_minecart_type).
 * ===================================================================== */

static bool place_item_on(struct item* it, uint16_t item_id, int x, int y, int z) {
	struct block_data blk_on, blk_where;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &blk_on));
	assert(server_world_get_block(&s.worlds[0], x, y + 1, z, &blk_where));
	struct block_info on = {.block = &blk_on, .neighbours = NULL, .x = x, .y = y, .z = z};
	struct block_info where
		= {.block = &blk_where, .neighbours = NULL, .x = x, .y = y + 1, .z = z};
	struct item_data held = {.id = item_id, .durability = 0, .count = 1};
	assert(item_get(&held) == it);
	return it->onItemPlace(&s, &s.player, &held, &where, &on, SIDE_TOP);
}

static struct entity* only_minecart(void) {
	struct entity* found = NULL;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MINECART) {
			assert(!found);
			found = e;
		}
		dict_entity_next(it);
	}
	assert(found);
	return found;
}

static void test_item_ids_and_placement(void) {
	assert(item_get(&(struct item_data) {.id = ITEM_MINECART_CHEST}) == &item_minecart_chest);
	assert(item_get(&(struct item_data) {.id = ITEM_MINECART_FURNACE}) == &item_minecart_furnace);
	assert(ITEM_MINECART_CHEST == 342);
	assert(ITEM_MINECART_FURNACE == 343);

	make_ns_rail(4, 4, 3);

	assert(place_item_on(&item_minecart_chest, ITEM_MINECART_CHEST, 4, RY, 4));
	struct entity* chest_cart = only_minecart();
	assert(chest_cart->data.minecart.minecart_type == 1);
	assert(strcmp(chest_cart->rightClickText, "Open") == 0);
	free_cart(chest_cart);

	assert(place_item_on(&item_minecart_furnace, ITEM_MINECART_FURNACE, 4, RY, 4));
	struct entity* furnace_cart = only_minecart();
	assert(furnace_cart->data.minecart.minecart_type == 2);
	assert(strcmp(furnace_cart->rightClickText, "Use") == 0);
	free_cart(furnace_cart);

	printf("  item ids 342/343 wire to type 1/2 via the real placement path OK\n");
}

/* ===================================================================== *
 * Test 2: destruction drop sets per type, exact item list.
 * 5 unarmed hits (fist=1 -> 10/hit) = 50 > 40, destroying on the 5th.
 * ===================================================================== */

static void attack(uint32_t id) {
	struct server_rpc rpc
		= {.type = SRPC_ENTITY_ATTACK, .payload.entity_attack.entity_id = id};
	server_local_test_process_rpc(&s, &rpc);
}

static void test_destruction_drops(void) {
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	/* Deviation-#18: zero-init health reads as DEAD (the cart-breaking
	 * ENTITY_ATTACKs below would drain inert); be explicitly alive. */
	s.player.health = MAX_PLAYER_HEALTH;

	make_ns_rail(5, 4, 3);
	vec3 pos = {5.5f, (float)RY + 0.5f, 4.5f};
	s.player.x = 5.5;
	s.player.y = (double)RY + 0.5 + PLAYER_EYE_HEIGHT;
	s.player.z = 4.5;

	/* --- type 0: UNCHANGED (regression sanity, not a new behaviour) --- */
	{
		struct entity* cart = make_cart(9001, 0, pos);
		reset_drop_observer();
		for(int i = 0; i < 5; i++) attack(9001);
		assert(!dict_entity_get(s.entities, 9001)); /* destroyed */
		assert(g_dropped_count == 1);
		assert(observed_drop(ITEM_MINECART, 1));
		(void)cart;
	}

	/* --- type 1: minecartEmpty + 2 occupied slots + chest block --- */
	{
		struct entity* cart = make_cart(9002, 1, pos);
		cart->data.minecart.inventory[0]
			= (struct item_data) {.id = ITEM_COAL, .count = 5, .durability = 0};
		cart->data.minecart.inventory[26]
			= (struct item_data) {.id = ITEM_DIAMOND, .count = 1, .durability = 0};
		reset_drop_observer();
		for(int i = 0; i < 5; i++) attack(9002);
		assert(!dict_entity_get(s.entities, 9002));
		assert(g_dropped_count == 4 && "minecartEmpty + coal + diamond + chest");
		assert(observed_drop(ITEM_MINECART, 1));
		assert(observed_drop(ITEM_COAL, 5));
		assert(observed_drop(ITEM_DIAMOND, 1));
		assert(observed_drop(BLOCK_CHEST, 1));
	}

	/* empty chest cart: minecartEmpty + chest block, NO extra item drop. */
	{
		struct entity* cart = make_cart(9003, 1, pos);
		(void)cart;
		reset_drop_observer();
		for(int i = 0; i < 5; i++) attack(9003);
		assert(!dict_entity_get(s.entities, 9003));
		assert(g_dropped_count == 2);
		assert(observed_drop(ITEM_MINECART, 1));
		assert(observed_drop(BLOCK_CHEST, 1));
	}

	/* --- type 2: minecartEmpty + the IDLE furnace id (61), never lit (62) --- */
	{
		struct entity* cart = make_cart(9004, 2, pos);
		(void)cart;
		reset_drop_observer();
		for(int i = 0; i < 5; i++) attack(9004);
		assert(!dict_entity_get(s.entities, 9004));
		assert(g_dropped_count == 2);
		assert(observed_drop(ITEM_MINECART, 1));
		assert(observed_drop(BLOCK_FURNACE, 1));
		assert(!observed_drop(BLOCK_FURNACE_LIT, 1));
		assert(BLOCK_FURNACE == 61 && BLOCK_FURNACE_LIT == 62);
	}

	printf("  destruction drop sets per type (0/1/2), exact item lists, OK\n");
}

/* ===================================================================== *
 * Test 3: chest cart -- entity-backed GUI open/populate/close, THEN
 * save/evict/reload persistence through entity_archive.c.
 * ===================================================================== */

static void use_entity(uint32_t id) {
	struct server_rpc rpc
		= {.type = SRPC_ENTITY_USE, .payload.entity_use.entity_id = id};
	server_local_test_process_rpc(&s, &rpc);
}

static void close_window(uint8_t window) {
	struct server_rpc rpc
		= {.type = SRPC_WINDOW_CLOSE, .payload.window_close.window = window};
	server_local_test_process_rpc(&s, &rpc);
}

static void test_chest_cart_gui_and_persistence(void) {
	make_ns_rail(6, 4, 3);
	vec3 pos = {6.5f, (float)RY + 0.5f, 4.5f};
	s.player.x = 6.5;
	s.player.y = (double)RY + 0.5 + PLAYER_EYE_HEIGHT;
	s.player.z = 4.5;

	struct entity* cart = make_cart(9101, 1, pos);

	g_open_window_count = 0;
	g_inventory_slot_count = 0;
	use_entity(9101);
	assert(g_open_window_count == 1);
	assert(g_last_open_window.payload.window_open.window == WINDOWC_MINECART_CHEST);
	assert(g_last_open_window.payload.window_open.type == WINDOW_TYPE_CHEST);
	assert(g_last_open_window.payload.window_open.slot_count == CHEST_SIZE);
	assert(s.player.active_inventory != &s.player.inventory);
	assert(s.player.active_inventory->logic == &inventory_logic_minecart_chest);
	assert(s.player.active_inventory->entity_id == 9101);

	/* Populate two storage slots directly on the open window (the click
	   PROTOCOL itself is pre-existing, generic, shared machinery -- not part
	   of this slice; what this test targets is on_create/on_close's
	   entity lookup + copy, which populating+closing exercises either way). */
	s.player.active_inventory->items[CHEST_SLOT_STORAGE + 0]
		= (struct item_data) {.id = ITEM_COAL, .count = 12, .durability = 0};
	s.player.active_inventory->items[CHEST_SLOT_STORAGE + 26]
		= (struct item_data) {.id = ITEM_DIAMOND, .count = 3, .durability = 0};

	close_window(WINDOWC_MINECART_CHEST);
	assert(s.player.active_inventory == &s.player.inventory); /* closed */
	assert(cart->data.minecart.inventory[0].id == ITEM_COAL);
	assert(cart->data.minecart.inventory[0].count == 12);
	assert(cart->data.minecart.inventory[26].id == ITEM_DIAMOND);
	assert(cart->data.minecart.inventory[26].count == 3);
	for(int k = 1; k < 26; k++)
		assert(cart->data.minecart.inventory[k].id == 0);

	/* Reopening must find the SAME two items (the entity is the single
	   source of truth, not a stale window copy). */
	use_entity(9101);
	assert(s.player.active_inventory->items[CHEST_SLOT_STORAGE + 0].id == ITEM_COAL);
	assert(s.player.active_inventory->items[CHEST_SLOT_STORAGE + 0].count == 12);
	close_window(WINDOWC_MINECART_CHEST);

	/* Cleanup: leaving this cart live would leak into
	   test_chest_cart_evict_reload's chunk-wide save/evict, which scans for
	   "any type-1 cart" and would then be unable to tell the two apart. */
	free_cart(cart);

	printf("  chest-cart entity-backed GUI open/populate/close round trip OK\n");
}

/* Direct entity_archive.c round trip: WORST CASE, all 27 slots occupied,
   proving entity_node_count/entity_wrap_count's budget covers it (an
   undercount makes chunk_entities_build return NULL here, per the dispatch's
   explicit warning). */
static void test_chest_archive_worst_case(void) {
	struct chunk_entities entities = {0};
	struct archived_entity record = {0};
	record.kind = ARCHIVED_ENTITY_MINECART;
	record.source_id = 55;
	record.pos[0] = 8.5f; record.pos[1] = 68.0f; record.pos[2] = 9.5f;
	record.data.minecart.minecart_type = 1;
	for(int i = 0; i < 27; i++)
		record.data.minecart.inventory[i] = (struct item_data) {
			.id = (uint16_t)(300 + i), .count = (uint8_t)(i + 1), .durability = 0};
	entities.entities = &record;
	entities.count = 1;

	void* arena = NULL;
	struct nbt_list* built = chunk_entities_build(&entities, &arena);
	assert(built && arena && "worst-case 27-slot chest cart must fit the budget");

	/* chunk_entities_build returns the raw sentinel wrap; chunk_entities_parse
	   wants it addressed through a TAG_LIST node (test_region_entities.c's own
	   established wrapping pattern). */
	nbt_node entities_list
		= {.type = TAG_LIST, .name = "Entities", .payload.tag_list = built};
	struct chunk_entities parsed = {0};
	bool ok = chunk_entities_parse(&entities_list, 0, 0, &parsed);
	assert(ok && parsed.count == 1);
	const struct archived_minecart_state* m = &parsed.entities[0].data.minecart;
	assert(m->minecart_type == 1);
	for(int i = 0; i < 27; i++) {
		assert(m->inventory[i].id == (uint16_t)(300 + i));
		assert(m->inventory[i].count == (uint8_t)(i + 1));
	}
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);

	printf("  entity_archive worst-case 27-slot chest-cart NBT round trip OK "
		   "(node/wrap budget sufficient)\n");
}

/* Full save/evict/reload through server_world, proving the type/push/fuel/
   inventory quintet all survive a REAL chunk eviction, not just the
   isolated archive functions above. */
static void test_chest_cart_evict_reload(void) {
	make_ns_rail(7, 4, 2);
	struct entity* cart = make_cart(9102, 1, (vec3) {7.5f, (float)RY + 0.5f, 4.5f});
	cart->data.minecart.inventory[3]
		= (struct item_data) {.id = ITEM_REDSTONE, .count = 9, .durability = 0};
	cart->data.minecart.fuel = 77; /* exercise the Fuel tag on a chest cart too --
	                                   written for every kind (see the archive's
	                                   own fixed-footprint comment) even though
	                                   Beta itself never populates it for type 1 */

	server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
	test_drain_quiesce();
	test_clin_drain();
	assert(server_world_save_chunk(&s.worlds[0], true, 0, 0));
	assert(!server_world_is_chunk_loaded(&s.worlds[0], 0, 0));
	assert(!dict_entity_get(s.entities, 9102)); /* evicted */

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);

	struct entity* reloaded = NULL;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MINECART && e->data.minecart.minecart_type == 1)
			reloaded = e;
		dict_entity_next(it);
	}
	assert(reloaded && "chest cart must survive save/evict/reload");
	assert(reloaded->data.minecart.inventory[3].id == ITEM_REDSTONE);
	assert(reloaded->data.minecart.inventory[3].count == 9);
	assert(reloaded->data.minecart.fuel == 77);
	assert(strcmp(reloaded->rightClickText, "Open") == 0);
	free_cart(reloaded);

	printf("  chest-cart inventory survives a REAL save/evict/reload cycle OK\n");
}

/* ===================================================================== *
 * Test 4: coal fueling. --stackSize (0 -> clear slot), fuel += 1200
 * (uncapped). Every interact (coal or not) repoints the push vector.
 * ===================================================================== */

static void test_coal_fueling(void) {
	make_ns_rail(8, 4, 2);
	vec3 pos = {8.5f, (float)RY + 0.5f, 4.5f};
	s.player.x = 8.5 - 2.0; /* offset so pushX/pushZ land on a checkable sign */
	s.player.y = (double)RY + 0.5 + PLAYER_EYE_HEIGHT;
	s.player.z = 4.5;

	struct entity* cart = make_cart(9201, 2, pos);
	assert(cart->data.minecart.fuel == 0);

	/* Stack of 5 coal in the hotbar slot the inventory logic reads. */
	size_t held_slot
		= INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&s.player.inventory);
	s.player.inventory.items[held_slot]
		= (struct item_data) {.id = ITEM_COAL, .count = 5, .durability = 0};

	use_entity(9201);
	assert(cart->data.minecart.fuel == 1200);
	assert(s.player.inventory.items[held_slot].id == ITEM_COAL);
	assert(s.player.inventory.items[held_slot].count == 4);
	/* pushX = posX - playerX = 8.5 - 6.5 = +2.0 (away from the player). */
	assert(fabsf(cart->data.minecart.push_x - 2.0f) < 1.0e-5f);
	assert(fabsf(cart->data.minecart.push_z - 0.0f) < 1.0e-5f);

	/* Down to the last coal: the slot must fully clear, not linger at 0. */
	s.player.inventory.items[held_slot].count = 1;
	use_entity(9201);
	assert(cart->data.minecart.fuel == 2400);
	assert(s.player.inventory.items[held_slot].id == 0
		   && "the slot must be CLEARED, not left at id=coal count=0");

	/* Non-coal interact: fuel/item untouched, but the push vector STILL
	   re-points -- move the player to the OTHER side first to prove the sign
	   flips with the player's position, not a stale cached direction. */
	s.player.x = 8.5 + 3.0;
	s.player.inventory.items[held_slot]
		= (struct item_data) {.id = ITEM_STICK, .count = 1, .durability = 0};
	use_entity(9201);
	assert(cart->data.minecart.fuel == 2400 && "non-coal must not add fuel");
	assert(s.player.inventory.items[held_slot].id == ITEM_STICK
		   && s.player.inventory.items[held_slot].count == 1
		   && "non-coal item must be untouched");
	assert(fabsf(cart->data.minecart.push_x - (-3.0f)) < 1.0e-5f
		   && "push re-points on EVERY interact, coal or not");
	free_cart(cart);

	printf("  coal fueling: consumes exactly one, +1200 fuel, uncapped; "
		   "push repoints on every interact OK\n");
}

/* ===================================================================== *
 * Test 5: furnace push physics -- one push tick vs one coasting tick,
 * hand-derived. Distinct branches, not a test that passes for both.
 * ===================================================================== */

/* Every physics sub-test below (test_furnace_push_physics through
   test_fuel_settles_at_minus_one) shares this ONE rail fixture rather than
   each placing its own adjacent segment. Beta's rail placement runs a real
   onBlockAdded/neighbour-notify cascade (block_rail.c's auto-shape/connect
   logic) -- placing a SECOND straight NS segment directly next to a first
   one reshapes both into a curve, which silently swapped a test cart's
   heading (hx,hz) out from under a hand-set (0,1) in early iteration of this
   file. One shared, isolated 3-tile segment sidesteps that entirely: every
   cart below is created, ticked/used, and freed sequentially (never two
   live at once), so reusing one position is safe. A repeated make_ns_rail
   call on an UNCHANGED cell is also a documented no-op in this engine
   (Chunk.setBlockIDWithMetadata parity: identical id+metadata writes
   nothing and skips the notify cascade), so calling it only the first time
   here is not just tidier, it is what keeps the fixture inert afterward. */
#define PHYSICS_RAIL_X 4
#define PHYSICS_RAIL_Z 10
#define PHYSICS_RAIL_LEN 3
#define PHYSICS_CART_X ((float)PHYSICS_RAIL_X + 0.5f)
#define PHYSICS_CART_Z ((float)PHYSICS_RAIL_Z + 1.5f) /* the segment's middle tile */

static void test_furnace_push_physics(void) {
	park_player_far();
	make_ns_rail(PHYSICS_RAIL_X, PHYSICS_RAIL_Z, PHYSICS_RAIL_LEN);

	/* --- push tick: mag(push)=1.0>0.01 branch --- */
	struct entity* cart = make_cart(9301, 2,
		(vec3) {PHYSICS_CART_X, (float)RY + 0.5f, PHYSICS_CART_Z});
	cart->data.minecart.hx = 0;
	cart->data.minecart.hz = 1;
	cart->data.minecart.speed = 0.0f;
	cart->data.minecart.push_x = 0.0f;
	cart->data.minecart.push_z = 1.0f; /* already unit magnitude */
	/* This tick is "actively pushing" (furnace_pushing), which ALSO gates the
	   1-in-4 fuel-decrement roll -- a genuinely separate mechanic under test
	   elsewhere (test_fuel_settles_at_minus_one). Give it headroom so a fuel
	   roll landing on THIS tick cannot also flip fuel negative and clear the
	   push vector this test is asserting about (fuel<0 => push=(0,0), which
	   would otherwise make this test flaky -- fail ~1-in-4 runs -- instead of
	   deterministic). */
	cart->data.minecart.fuel = 1000;

	assert(cart->tick_server(cart, &s) == false);

	/* [1] mag=1>0.01: pushZ stays 1 (already unit); dot_heading = 0*0+1*1=1
	       -> speed = 0*0.8 + 1*0.04 = 0.04
	   [2] shared friction: speed *= 0.9599999785423279 */
	float expected_push_speed = 0.0f * 0.800000011920929f + 1.0f * 0.04f;
	expected_push_speed *= 0.9599999785423279f;
	assert(fabsf(cart->data.minecart.speed - expected_push_speed) < 1.0e-6f);
	assert(expected_push_speed > 0.03f && expected_push_speed < 0.04f
		   && "sanity: must land in the 0.8/0.04 branch's range, not 0.9's");

	/* [3] push-vector update (mag>0.01 AND speed^2>0.001 both hold here):
	       motion = (speed*hx, speed*hz) = (0, speed); dot = 1*speed > 0
	       -> push := motion (UNnormalised) */
	assert(fabsf(cart->data.minecart.push_x - 0.0f) < 1.0e-6f);
	assert(fabsf(cart->data.minecart.push_z - expected_push_speed) < 1.0e-6f);
	free_cart(cart);

	/* --- coasting tick: mag(push)=0<=0.01 branch, DISTINCT from the above --- */
	struct entity* coast = make_cart(9302, 2,
		(vec3) {PHYSICS_CART_X, (float)RY + 0.5f, PHYSICS_CART_Z});
	coast->data.minecart.hx = 0;
	coast->data.minecart.hz = 1;
	coast->data.minecart.speed = 0.1f;
	coast->data.minecart.push_x = 0.0f;
	coast->data.minecart.push_z = 0.0f;
	uint32_t seed_before = s.rand_src.seed;
	uint32_t entity_seed_before = s.entity_rng.seed;

	assert(coast->tick_server(coast, &s) == false);

	/* [1] mag=0, NOT>0.01: speed *= 0.8999999761581421
	   [2] shared friction: speed *= 0.9599999785423279 */
	float expected_coast_speed = 0.1f * 0.8999999761581421f;
	expected_coast_speed *= 0.9599999785423279f;
	assert(fabsf(coast->data.minecart.speed - expected_coast_speed) < 1.0e-6f);
	assert(expected_coast_speed > 0.08f
		   && "sanity: 0.9-branch result, clearly above the 0.8/0.04 branch's range");
	/* [3] mag(push) still 0<=0.01: push-vector update SKIPPED, stays (0,0). */
	assert(coast->data.minecart.push_x == 0.0f && coast->data.minecart.push_z == 0.0f);
	/* furnace_pushing was false (coasting branch) -> Beta's `var6 &&` never
	   even calls nextInt(4): the RNG must not have been touched at all. */
	assert(s.rand_src.seed == seed_before
		   && "coasting must not draw fuel RNG at all (short-circuited, not just its effect)");
	assert(s.entity_rng.seed == entity_seed_before
		   && "coasting must not draw the cart's own Random either");
	free_cart(coast);

	printf("  furnace push physics: push (0.8/0.04) vs coast (0.9) branches "
		   "distinctly correct, coast draws no RNG, OK\n");
}

/* ===================================================================== *
 * Test 6: push-vector zeroing on reverse dot product; forward dot tracks
 * motion.
 * ===================================================================== */

static void test_push_vector_zeroing(void) {
	park_player_far();

	/* Cart moving -Z (speed<0 along hz=+1) while push points +Z: opposed. */
	struct entity* cart = make_cart(9401, 2,
		(vec3) {PHYSICS_CART_X, (float)RY + 0.5f, PHYSICS_CART_Z});
	cart->data.minecart.hx = 0;
	cart->data.minecart.hz = 1;
	cart->data.minecart.speed = -0.2f;
	cart->data.minecart.push_x = 0.0f;
	cart->data.minecart.push_z = 1.0f;
	cart->data.minecart.fuel = 1000; /* headroom -- see test_furnace_push_physics's
	                                     note on the fuel-decrement/push-clear
	                                     interaction (harmless here since this
	                                     case already expects push==(0,0), but
	                                     keeps the isolation honest) */

	assert(cart->tick_server(cart, &s) == false);

	/* [1] mag=1>0.01: dot_heading=1 -> speed = -0.2*0.8 + 1*0.04 = -0.12
	   [2] shared friction: speed *= 0.9599999785423279 */
	float expected_speed = -0.2f * 0.800000011920929f + 1.0f * 0.04f;
	expected_speed *= 0.9599999785423279f;
	assert(expected_speed < 0.0f && "must still be moving -Z after this tick");
	assert(fabsf(cart->data.minecart.speed - expected_speed) < 1.0e-6f);
	/* [3]: motion=(0,expected_speed); dot = 1*expected_speed < 0 -> ZEROED. */
	assert(cart->data.minecart.push_x == 0.0f && cart->data.minecart.push_z == 0.0f
		   && "push must be ZEROED when the cart is moving opposite to it");
	free_cart(cart);

	/* Same-direction (non-reverse) case: push tracks the resulting motion. */
	struct entity* fwd = make_cart(9402, 2,
		(vec3) {PHYSICS_CART_X, (float)RY + 0.5f, PHYSICS_CART_Z});
	fwd->data.minecart.hx = 0;
	fwd->data.minecart.hz = 1;
	fwd->data.minecart.speed = 0.2f;
	fwd->data.minecart.push_x = 0.0f;
	fwd->data.minecart.push_z = 1.0f;
	fwd->data.minecart.fuel = 1000; /* headroom -- see test_furnace_push_physics */

	assert(fwd->tick_server(fwd, &s) == false);

	float expected_fwd = 0.2f * 0.800000011920929f + 1.0f * 0.04f;
	expected_fwd *= 0.9599999785423279f;
	assert(expected_fwd > 0.0f);
	assert(fabsf(fwd->data.minecart.speed - expected_fwd) < 1.0e-6f);
	assert(fwd->data.minecart.push_x == 0.0f
		   && fabsf(fwd->data.minecart.push_z - expected_fwd) < 1.0e-6f
		   && "push must TRACK current motion when moving the same direction");
	free_cart(fwd);

	printf("  push-vector zeroing on reverse dot product (+ forward tracking) OK\n");
}

/* ===================================================================== *
 * Test 7: rider damping 0.997 is UNCHANGED (type 0 regression sanity --
 * this slice must not have leaked into the base cart's friction).
 * ===================================================================== */

static void test_rider_damping_unchanged(void) {
	park_player_far();
	struct entity* cart = make_cart(9501, 0,
		(vec3) {PHYSICS_CART_X, (float)RY + 0.5f, PHYSICS_CART_Z});
	cart->data.minecart.hx = 0;
	cart->data.minecart.hz = 1;
	cart->data.minecart.occupied = true;
	cart->data.minecart.occupant_id = 1;
	cart->data.minecart.speed = 0.2f;

	assert(cart->tick_server(cart, &s) == false);

	float expected = 0.2f * 0.996999979019165f;
	assert(fabsf(cart->data.minecart.speed - expected) < 1.0e-6f);
	free_cart(cart);

	printf("  rider damping 0.997 unchanged for type 0 OK\n");
}

/* ===================================================================== *
 * Test 8: type 1/2 never mount, even via a structurally valid
 * SRPC_MINECART_RIDE_INTENT (server rejects on its own, not just the
 * well-behaved client).
 * ===================================================================== */

static void test_types_never_mount(void) {
	/* No rail needed: this test only exercises the RPC-level reject path,
	   never ticks the cart. */
	s.input_epoch_counter = 1;
	s.player.x = PHYSICS_CART_X; s.player.y = (double)RY + 0.5 + PLAYER_EYE_HEIGHT;
	s.player.z = PHYSICS_CART_Z;

	for(uint8_t type = 1; type <= 2; type++) {
		uint32_t id = 9600 + type;
		struct entity* cart = make_cart(id, type,
			(vec3) {PHYSICS_CART_X, (float)RY + 0.5f, PHYSICS_CART_Z});
		g_occupancy_count = 0;
		struct server_rpc rpc = {
			.type = SRPC_MINECART_RIDE_INTENT,
			.payload.minecart_ride_intent = {
				.entity_id = id,
				.input_epoch = s.input_epoch_counter,
				.request_seq = 1,
				.desired_occupied = true,
			},
		};
		server_local_test_process_rpc(&s, &rpc);
		assert(!cart->data.minecart.occupied);
		assert(g_occupancy_count == 1 && !g_last_occupied
			   && "server must republish unchanged (still unoccupied) authority");
		free_cart(cart);
	}

	printf("  storage/furnace carts reject SRPC_MINECART_RIDE_INTENT server-side OK\n");
}

/* ===================================================================== *
 * Test 9: fuel decrement settles at exactly -1 and stops (deterministic
 * endpoint regardless of the RNG seed / which tick the 1-in-4 roll lands
 * on -- see the derivation in the file header).
 * ===================================================================== */

static void test_fuel_settles_at_minus_one(void) {
	park_player_far();
	struct entity* cart = make_cart(9700, 2,
		(vec3) {PHYSICS_CART_X, (float)RY + 0.5f, PHYSICS_CART_Z});
	cart->data.minecart.hx = 0;
	cart->data.minecart.hz = 1;
	cart->data.minecart.fuel = 0;
	cart->data.minecart.push_x = 0.0f;
	cart->data.minecart.push_z = 1.0f;

	bool went_negative = false;
	for(int tick = 0; tick < 600; tick++) {
		/* Re-center every tick (position only, never speed/push/fuel) so
		   speed converging toward its ~0.166 blocks/tick fixed point never
		   drifts off the shared rail segment -- the rail lookup only ever
		   needs to cover the one center tile plus its immediate neighbours. */
		cart->pos[2] = PHYSICS_CART_Z;
		cart->pos_old[2] = PHYSICS_CART_Z;
		assert(cart->tick_server(cart, &s) == false);
		assert(cart->data.minecart.fuel <= 0
			   && "fuel must never be positive once decrement is possible from 0");
		if(cart->data.minecart.fuel < 0)
			went_negative = true;
	}
	assert(went_negative && "600 ticks at 1-in-4/active-tick must have fired at least once");
	assert(cart->data.minecart.fuel == -1
		   && "fuel settles at EXACTLY -1: push clears the instant it goes "
			  "negative, which stops furnace_pushing (and so the roll) forever after");
	assert(cart->data.minecart.push_x == 0.0f && cart->data.minecart.push_z == 0.0f);
	free_cart(cart);

	printf("  furnace fuel deterministically settles at exactly -1 and stops OK\n");
}

/* ===================================================================== *
 * Test 11b (next-gaps-0924 W3-s2): the fuel roll draws the CART'S OWN
 * Random, never World.rand.
 *
 * Beta: while pushing, fuel drops by one when the cart's rand.nextInt(4) == 0
 * [EntityMinecart.java:485-490], where `rand` is the per-entity field
 * java.util.Random each entity owns [Entity.java:50] -- NOT
 * worldObj.rand. CaveWii maps every per-entity Random to s->entity_rng
 * (W1C-b, CODESTYLE 2.3) so s->rand_src stays aligned to Beta's
 * world-stream draw count. Expected fuel per tick is replayed on a COPY of
 * the entity stream: exactly one nextInt(4) per actively pushing tick
 * (Beta's pushing flag = push magnitude > 0.01), none otherwise, and the world
 * stream must not move at all.
 * ===================================================================== */

static void test_furnace_fuel_draws_entity_rng(void) {
	park_player_far();
	struct entity* cart = make_cart(9750, 2,
		(vec3) {PHYSICS_CART_X, (float)RY + 0.5f, PHYSICS_CART_Z});
	cart->data.minecart.hx = 0;
	cart->data.minecart.hz = 1;
	cart->data.minecart.fuel = 1000;
	cart->data.minecart.push_x = 0.0f;
	cart->data.minecart.push_z = 1.0f;

	const uint32_t world_seed = 0xA5A5F00Du;
	s.rand_src.seed = world_seed;
	s.rand_src.has_next_gaussian = false;
	s.entity_rng.seed = 0x0924C0DEu;
	s.entity_rng.has_next_gaussian = false;
	const struct random_gen shared_before = s.entity_rng;
	/* I03 (board #8): the roll is the CART's own lifetime Random now, not
	 * the shared s->entity_rng [CFR EntityMinecart.java:452]. */
	struct java_random replay = cart->gameplay_rng;

	int decrements = 0, pushing_ticks = 0;
	for(int tick = 0; tick < 64; tick++) {
		cart->pos[2] = PHYSICS_CART_Z;
		cart->pos_old[2] = PHYSICS_CART_Z;
		float px = cart->data.minecart.push_x, pz = cart->data.minecart.push_z;
		bool var6 = sqrtf(px * px + pz * pz) > 0.01f; /* Beta var6 */
		int want_fuel = cart->data.minecart.fuel;
		if(var6) {
			pushing_ticks++;
			if(jrand_next_int_bound(&replay, 4) == 0) {
				want_fuel--;
				decrements++;
			}
		}
		assert(cart->tick_server(cart, &s) == false);
		if(cart->data.minecart.fuel != want_fuel
		   || s.rand_src.seed != world_seed) {
			printf("  FAIL tick %d: fuel %d want %d (entity-stream replay); "
				   "world seed %08x want %08x (World.rand must not move)\n",
				   tick, cart->data.minecart.fuel, want_fuel,
				   (unsigned)s.rand_src.seed, (unsigned)world_seed);
			fflush(stdout);
			assert(false && "furnace fuel roll must draw the cart's own Random");
		}
	}
	assert(cart->gameplay_rng.seed == replay.seed
		   && "exactly one own-stream draw per pushing tick");
	assert(memcmp(&shared_before, &s.entity_rng, sizeof shared_before) == 0
		   && "the shared entity stream is no longer drawn");
	assert(pushing_ticks == 64 && decrements > 0 && decrements < 64);
	free_cart(cart);

	printf("  furnace fuel roll draws the entity Random (%d/%d decrements), "
		   "World.rand untouched OK\n", decrements, pushing_ticks);
}

/* ===================================================================== *
 * Test 12 (coordinator review fix M1/M2, minecart-slice): a player must be
 * able to DIE IN THE NETHER while a storage-minecart GUI is open.
 *
 * Before this fix, server_local_plan_death_drops (source/network/
 * server_local.c) fell into its final `else { return false; }` for
 * inventory_logic_minecart_chest, so server_local_begin_death_drop_journal
 * returned DEATH_DROP_BEGIN_FAILED, health was restored, and
 * server_local_set_player_health returned false BEFORE
 * server_local_close_active_inventory ever ran. The window stayed open, so
 * EVERY subsequent lethal hit repeated the identical revert -- a permanent
 * softlock (stand in lava indefinitely with the GUI open). The dormant twin
 * (server_local_project_post_close_inventory's own missing branch) was masked
 * only because plan_death_drops always bailed out first in the same call
 * chain; both are fixed together here.
 *
 * This scenario needs a REAL level archive + Nether world (journaled_death
 * requires s->worlds[0].dimension==WORLD_DIM_NETHER AND a real s->level.data), so
 * it cannot use this file's own lightweight shared `s` (main()'s Overworld
 * world, never backed by a level archive -- journaled_death can never be true
 * through it). The harness below mirrors tests/test_death_drop_journal_env.c's
 * own create_level_with_hotbar/init_server/discard_server shape (same
 * worktree, safe to read), trimmed to only what a single Nether death needs --
 * deliberately NOT modeled on any sibling worktree's test file, per the
 * minecart-slice's worktree-isolation rule.
 * ===================================================================== */

static char s_fixture_DEATH_TMP_WORLD[128]; /* was #define DEATH_TMP_WORLD='test_minecart_types_env_death_tmp' (a literal) */
#define DEATH_TMP_WORLD s_fixture_DEATH_TMP_WORLD

static void death_make_world_dirs(void) {
	char path[256];
	MKDIR(DEATH_TMP_WORLD);
	snprintf(path, sizeof(path), "%s/region", DEATH_TMP_WORLD);
	MKDIR(path);
	snprintf(path, sizeof(path), "%s/DIM-1", DEATH_TMP_WORLD);
	MKDIR(path);
	snprintf(path, sizeof(path), "%s/DIM-1/region", DEATH_TMP_WORLD);
	MKDIR(path);
	/* Reset to the bundled Beta template every run, mirroring main()'s own
	   TMP_WORLD stale-file removal above. */
	const char* stale[] = {
		"level.dat", "level.dat_new", "level.dat_old",
		"death_drops.dat", "death_drops.dat_new", "death_drops.dat_old",
		"region/r.0.0.mcr", "DIM-1/region/r.0.0.mcr",
	};
	for(size_t i = 0; i < sizeof(stale) / sizeof(stale[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", DEATH_TMP_WORLD, stale[i]);
		remove(path);
	}
}

static void death_create_level(void) {
	string_t name;
	string_init_set_str(name, DEATH_TMP_WORLD);
	struct level_archive level;
	assert(level_archive_create_new(
		&level, name, 424242, (vec3) {8.5F, 70.0F, 8.5F}));
	level_archive_destroy(&level);
	assert(level_archive_create(&level, name));
	struct item_data items[INVENTORY_SIZE] = {{0}};
	struct inventory inventory = {.items = items, .capacity = INVENTORY_SIZE};
	int16_t health = MAX_PLAYER_HEALTH;
	assert(level_archive_write_player(
		&level, (vec3) {8.5F, 70.0F, 8.5F}, (vec2) {0}, NULL,
		WORLD_DIM_NETHER));
	assert(level_archive_write_inventory(&level, &inventory));
	assert(level_archive_write(&level, LEVEL_PLAYER_HEALTH, &health));
	assert(level_archive_save(&level));
	level_archive_destroy(&level);
	string_clear(name);
}

/* Trimmed to what a single Nether-death scenario needs; shape mirrors (does
   not literally copy -- distinct names, distinct scope) the sibling death-
   drop-journal suite's own init_server/discard_server. */
static void death_init_server(struct server_local* server) {
	memset(server, 0, sizeof(*server));
	rand_gen_seed(&server->rand_src);
	server->container_drop_rng.seed = 0x12345678u;
	string_init_set_str(server->level_name, DEATH_TMP_WORLD);
	assert(level_archive_create(&server->level, server->level_name));
	if(!level_archive_read_death_tx_counter(
		   &server->level, &server->death_tx_counter))
		server->death_tx_counter = CAVEWII_DEATH_TX_LEGACY_FLOOR;
	assert(inventory_create(
		&server->player.inventory, &inventory_logic_player, server,
		&server->player,
		INVENTORY_SIZE, 0, 0, 0));
	assert(level_archive_read_inventory(
		&server->level, &server->player.inventory));
	server->player.active_inventory = &server->player.inventory;
	server->player.x = 8.5;
	server->player.y = 70.0;
	server->player.z = 8.5;
	server->player.spawn_x = 8;
	server->player.spawn_y = 70;
	server->player.spawn_z = 8;
	server->player.health = MAX_PLAYER_HEALTH;
	server->player.dimension = WORLD_DIM_NETHER;
	server->player.has_pos = true;
	server->player.finished_loading = true;
	server->player.fire = -PLAYER_FIRE_RESISTANCE;
	server->player.oxygen = MAX_OXYGEN;
	server->world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(server))
		abort();
	piston_moving_store_init(&server->piston_moving);
	dict_entity_init(server->entities);
	string_t level;
	string_init_set_str(level, DEATH_TMP_WORLD);
	server_world_create(&server->world, level, WORLD_DIM_NETHER, 424242, true);
	server->worlds[0].session = server; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	string_clear(level);
	server->world.local = server;
	server->worlds[0].session = server; /* A1a (D1): owning session for services/callbacks */
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&server->world, 0, 0, &chunk) && chunk);
}

static void death_discard_server(struct server_local* server) {
	if(server->player.has_pos)
		server_world_discard(&server->world);
	piston_moving_store_destroy(&server->piston_moving);
	dict_entity_it_t it;
	dict_entity_it(it, server->entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_clear(server->entities);
	if(server->level.data)
		level_archive_discard(&server->level);
	if(server->player.active_inventory
	   && server->player.active_inventory != &server->player.inventory)
		inventory_destroy_owned(server->player.active_inventory, &server->player);
	inventory_destroy(&server->player.inventory, &server->player);
	mem2_free_sized(
		server->entity_tick_keys,
		server->entity_tick_capacity * sizeof(*server->entity_tick_keys));
	death_drop_journal_free(server->death_drop_journal);
	string_clear(server->level_name);
}

static void test_nether_death_with_minecart_chest_open(void) {
	death_make_world_dirs();
	death_create_level();
	struct server_local server;
	death_init_server(&server);

	struct entity* cart = server_local_spawn_minecart_type(
		(vec3) {8.5F, 70.0F, 8.5F}, &server, 1);
	assert(cart && cart->data.minecart.minecart_type == 1);
	uint32_t cart_id = cart->id;

	struct server_rpc use_rpc
		= {.type = SRPC_ENTITY_USE, .payload.entity_use.entity_id = cart_id};
	server_local_test_process_rpc(&server, &use_rpc);
	assert(server.player.active_inventory != &server.player.inventory);
	assert(server.player.active_inventory->logic
		   == &inventory_logic_minecart_chest);
	struct inventory* win = server.player.active_inventory;

	/* Two occupied storage slots (test_chest_cart_gui_and_persistence's own
	   idiom) plus a held cursor item: the exact combination M1's fix and the
	   inv_on_close journal-aware-cursor fix both need exercised together. */
	win->items[CHEST_SLOT_STORAGE + 0]
		= (struct item_data) {.id = ITEM_COAL, .count = 5, .durability = 0};
	win->items[CHEST_SLOT_STORAGE + 26]
		= (struct item_data) {.id = ITEM_DIAMOND, .count = 2, .durability = 0};
	inventory_set_picked_item(
		win,
		(struct item_data) {.id = ITEM_APPLE, .count = 1, .durability = 0});

	/* THE M1 assertion. Pre-fix the RESPAWN transaction returned false
	   forever: the window never closed, so every retry hit the identical
	   revert. Deviation-#18 split: the lethal hit commits the dead state,
	   and the close/journal/respawn transaction runs at the explicit
	   request. */
	assert(server_local_set_player_health(&server, 0)
		   && "the lethal Nether hit must commit the dead state");
	assert(server_local_respawn_request(&server)
		   && "M1: the respawn must succeed while the minecart-chest GUI "
			  "is open server-side, not silently revert");
	assert(server.player.health == MAX_PLAYER_HEALTH);
	assert(server.player.dimension == WORLD_DIM_OVERWORLD);
	/* Direct proof the softlock's root cause is gone: the window actually
	   closed. Pre-fix, plan_death_drops's early false meant
	   server_local_close_active_inventory never ran at all. */
	assert(server.player.active_inventory == &server.player.inventory);

	/* Documented M1 design: the cart's 27 slots are deliberately NOT
	   recorded into journal->container_slots[] (death_drop_recovery.c's
	   apply_container_slots only understands WINDOWC_FURNACE/CHEST/
	   IRON_CHEST and would treat any other window as corruption), so
	   container_count must stay 0 even though a real container GUI was open
	   and closed during this death. */
	assert(server.death_drop_journal);
	assert(server.death_drop_journal->container_count == 0);

	/* Storage itself is NOT lost: inv_on_close's ordinary commit into the
	   live entity still runs (a plain memcpy, not a spawn call). */
	assert(cart->data.minecart.inventory[0].id == ITEM_COAL);
	assert(cart->data.minecart.inventory[0].count == 5);
	assert(cart->data.minecart.inventory[26].id == ITEM_DIAMOND);
	assert(cart->data.minecart.inventory[26].count == 2);

	/* The cursor item DID journal-transfer (the inv_on_close fix): a live
	   EntityItem tagged with this death's transaction id must exist. */
	uint32_t tx = server.death_drop_journal->transaction_id;
	bool found_cursor_drop = false;
	dict_entity_it_t it;
	dict_entity_it(it, server.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->data.item.death_transaction == tx
		   && e->data.item.item.id == ITEM_APPLE)
			found_cursor_drop = true;
		dict_entity_next(it);
	}
	assert(found_cursor_drop
		   && "cursor item must journal-transfer via inv_on_close, not be "
			  "silently dropped or block the close");

	/* Settle the deferred world switch (Nether -> Overworld), matching
	   test_death_drop_journal_env.c's own test_empty_transaction. */
	assert(server_local_test_death_world_reload(&server));
	assert(!server.death_drop_active);
	assert(!server.death_drop_journal);

	/* Second assertion the coordinator asked for: a REPEAT lethal hit must
	   not revert. Pre-fix, the window never closed, so a second call hit
	   the identical `else { return false; }` forever -- the "stand in lava
	   indefinitely" symptom; both calls would have returned false. The
	   window closed on the first hit now, so this second, independent
	   lethal hit (an ordinary post-respawn Overworld death) must succeed. */
	assert(server_local_set_player_health(&server, 0)
		   && "repeat lethal hit must not revert (softlock regression)");
	/* Harness completion: the death reload above left the loading epoch
	 * armed (finished_loading=false awaits the client's collision-ready
	 * ack this fixture never drives); the respawn gate requires a settled
	 * player exactly like every ordinary action. */
	server.player.finished_loading = true;
	server.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	assert(server_local_respawn_request(&server)
		   && "the repeat respawn must succeed too");
	assert(server.player.health == MAX_PLAYER_HEALTH);

	death_discard_server(&server);
	printf("  Nether death with minecart-chest GUI open kills the player "
		   "and journals the drops, repeat hit does not revert OK\n");
}

int main(void) {
	test_fixture_dir("test_minecart_types_env_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	test_fixture_dir("test_minecart_types_env_death_tmp", s_fixture_DEATH_TMP_WORLD, sizeof s_fixture_DEATH_TMP_WORLD);
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	clin_test_set_rpc_observer(observe_rpc, NULL);

	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	string_clear(level);
	s.worlds[0].local = &s;
	s.worlds[0].session = &s; /* A1a (D1): owning session for services/callbacks */
	s.scheduled_tick_budget = 0;

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);

	test_item_ids_and_placement();
	test_destruction_drops();
	test_chest_cart_gui_and_persistence();
	test_chest_archive_worst_case();
	test_chest_cart_evict_reload();
	test_coal_fueling();
	test_furnace_push_physics();
	test_push_vector_zeroing();
	test_rider_damping_unchanged();
	test_types_never_mount();
	test_fuel_settles_at_minus_one();
	test_furnace_fuel_draws_entity_rng();
	test_nether_death_with_minecart_chest_open();

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	dict_entity_clear(s.entities);
	printf("test_minecart_types_env: OK\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	test_fixture_dir_cleanup(DEATH_TMP_WORLD);
	return 0;
}
