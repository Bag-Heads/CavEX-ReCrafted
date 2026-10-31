/*
	FH-12 (docs/fork_hack_audit_2026-08-21.md): item pickup delay is a REAL
	per-instance Beta EntityItem.delayBeforeCanPickup field, not the flat
	`age >= 2*4` universal gate it used to be. Ground truth, exhaustively
	confirmed against every `delayBeforeCanPickup` write site in the decompile
	pack (740c583; `git grep delayBeforeCanPickup` over the whole tree turns
	up exactly six):

	  - EntityItem.onUpdate: `if(delayBeforeCanPickup>0) --delayBeforeCanPickup;`
	    -- unconditional, once per tick, BEFORE movement/pickup.
	  - EntityItem.onCollideWithPlayer: pickup gated on
	    `delayBeforeCanPickup == 0`.
	  - Block.dropBlockAsItem_do / Entity.entityDropItem / BlockCrops /
	    BlockJukeBox.func_28038_b_ / BlockSnow: all set 10 -- this is the
	    convention for ordinary block-break AND mob-death drops (ANY drop
	    that funnels through dropBlockAsItem_do or entityDropItem), NOT 0 as
	    the audit finding's summary loosely suggested.
	  - EntityPlayer.dropPlayerItemWithRandomChoice: sets 40 -- Q-drop
	    (dropCurrentItem), the GUI cursor drop (dropPlayerItem, both direct
	    and Container.onCraftGuiClosed), AND InventoryPlayer.dropAllItems
	    (player death drops go through this SAME function with var2=true, so
	    death drops are 40, not 0 either).
	  - BlockChest/BlockDispenser/BlockFurnace onBlockRemoval spill and
	    BlockDispenser.dispenseItem: none of these ever touch the field --
	    they stay at the Java implicit default 0.
	  - EntityItem.writeEntityToNBT/readEntityFromNBT: only Health/Age/Item
	    -- delayBeforeCanPickup is NOT persisted.

	Pinned here:
	  1. A real block-break drop (server_local_spawn_block_drops_after_break,
	     the actual Block.dropBlockAsItem_do choke point every block file
	     drops through) starts at pickup_delay == 10 and is genuinely NOT
	     collectible until exactly the 10th server tick -- not immediately
	     (the old flat-age gate's false claim) and not the old 0.4s window.
	  2. A real dispenser eject (server_local_dispenser_dispense) starts at
	     pickup_delay == 0 -- BlockDispenser.dispenseItem never sets the
	     field, so it stays grabbable on tick 1 (the old flat-age gate
	     wrongly held it back 0.4s -- this is the bug half of FH-12).
	  3. A real player Q-drop (server_local_drop_hotbar_item) starts at
	     pickup_delay == 40, decrements exactly 1 per server tick, and is
	     NOT collectible until exactly the 40th tick (2.0s) -- not 0.4s (the
	     old bug's other half: instant-hotbar-reshuffle exploit).
	  4. Save/evict/reload does not persist pickup_delay: a live item primed
	     at a nonzero delay always reloads at 0, regardless of its value
	     before the save.
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
#include "block/dispenser_logic.h"
#include "entity/entity.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_dispenser.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "support/clin_drain.h"
#include "support/drain_quiesce.h" /* quiesce-0926 */

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_item_pickup_delay_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100 // stone floor
#define Y 101  // working plane

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

static void set_block(int x, int y, int z, int type, int meta) {
	bool ok = server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)});
	assert(ok && "server_world_set_block refused the edit");
}

// --- entity-id bookkeeping: find the ONE new entity a spawn call created ---

static size_t snapshot_ids(uint32_t* out, size_t max) {
	size_t n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it) && n < max) {
		out[n++] = dict_entity_ref(it)->key;
		dict_entity_next(it);
	}
	return n;
}

static bool id_in(uint32_t id, const uint32_t* set, size_t n) {
	for(size_t i = 0; i < n; i++)
		if(set[i] == id)
			return true;
	return false;
}

static struct entity* find_new_item_entity(const uint32_t* before,
										   size_t before_n) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e->type == ENTITY_ITEM && !id_in(e->id, before, before_n))
			return e;
		dict_entity_next(it);
	}
	assert(0 && "spawn call did not produce a new ENTITY_ITEM");
	return NULL;
}

static void free_all_item_entities(void) {
	enum { MAX = 256 };
	uint32_t ids[MAX];
	size_t n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it) && n < MAX) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e->type == ENTITY_ITEM)
			ids[n++] = e->id;
		dict_entity_next(it);
	}
	for(size_t i = 0; i < n; i++) {
		struct entity** pp = dict_entity_get(s.entities, ids[i]);
		if(pp && *pp) {
			free(*pp);
			dict_entity_erase(s.entities, ids[i]);
		}
	}
}

// True once the player's hotbar slot 0 holds a nonzero stack (the collect
// target every scenario below drops into an otherwise-empty inventory).
static bool player_holds_any(void) {
	struct item_data it;
	return inventory_get_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR, &it)
		&& it.id != 0 && it.count != 0;
}

static void clear_player_inventory(void) {
	for(size_t i = 0; i < INVENTORY_SIZE; i++)
		inventory_clear_slot(&s.player.inventory, i);
}

/* item_hold_still note -- fixture pin (next-gaps-0924/W1C; ITEM-RNG-TEST-02):
 * the delay counter is what is under test, so each stack is placed RESTING
 * on the stone floor top under the player (posY = floor top + y_offset, not
 * embedded) with zero velocity. An embedded stack would run
 * Entity.pushOutOfBlocks [Entity.java:1220] every tick with a random escape
 * speed from the item's own Random, and on some seeds float above the
 * pickup box -- that would make this delay test depend on dice rolls. */
static void item_rest_at_player(struct entity* it) {
	it->teleport(it, (vec3) {s.player.x, (float)(YF + 1) + it->y_offset,
							 s.player.z});
	glm_vec3_zero(it->vel);
}

int main(void) {
	test_fixture_dir("test_item_pickup_delay_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	blocks_init();
	items_init();

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
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
					   true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 128;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);

	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	/* A living host (full health, port units: Beta's 20 = MAX_PLAYER_HEALTH).
	 * The memset above leaves health 0, which Beta's EntityPlayer pickup scan
	 * (`if(this.health > 0)`) treats as dead -- itement-fix-0925. */
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.x = 8.5;
	s.player.y = (float)Y;
	s.player.z = 8.5;

	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));

	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	for(int x = 0; x <= 31; x++)
		for(int z = 0; z <= 31; z++)
			set_block(x, YF, z, BLOCK_STONE, 0);

	printf("test_item_pickup_delay:\n");
	/* This fixture is the no-guest half of the owner-routing equivalence gate:
	 * every delay case below must keep using the original host path. */
	assert(!s.guest_actor && !s.guest_gameplay && !s.guest_entity_id);

	// ========================================================================
	// 1. BLOCK DROP: server_local_spawn_block_drops_after_break -- the real
	//    Block.dropBlockAsItem_do choke point. Starts at 10 (NOT 0), decrements
	//    1/tick, and is collectible on the 10th tick, not before.
	// ========================================================================
	{
		clear_player_inventory();
		set_block(3, Y, 3, BLOCK_LEVER, 5); // floor lever: 1-for-1, no RNG
		struct block_data blk;
		assert(server_world_get_block(&s.worlds[0], 3, Y, 3, &blk));
		struct block_info info = {.x = 3, .y = Y, .z = 3, .block = &blk};

		uint32_t before[64];
		size_t before_n = snapshot_ids(before, 64);
		server_local_spawn_block_drops_after_break(&s, &info);
		struct entity* drop = find_new_item_entity(before, before_n);
		assert(drop->data.item.item.id == BLOCK_LEVER);

		// [740c583: Block.dropBlockAsItem_do -- `delayBeforeCanPickup = 10;`]
		assert(drop->data.item.pickup_delay == 10);

		// Teleport it onto the player so the ONLY remaining gate is the
		// delay itself, not the distance check.
		item_rest_at_player(drop); // fixture pin (see item_hold_still note)
		const struct java_random rng_before = drop->gameplay_rng;

		for(int tick = 1; tick <= 9; tick++) {
			bool destroyed = drop->tick_server(drop, &s);
			assert(!destroyed);
			// Resting, not embedded: no gameplay draw in this interval.
			assert(drop->gameplay_rng.seed == rng_before.seed
				&& drop->gameplay_rng.have_next_gaussian
					== rng_before.have_next_gaussian
				&& drop->gameplay_rng.next_gaussian_cached
					== rng_before.next_gaussian_cached);
			assert(drop->data.item.pickup_delay == 10 - tick);
			assert(!player_holds_any()); // NOT collectible yet
		}
		assert(drop->data.item.pickup_delay == 1);
		drop->tick_server(drop, &s); // the 10th tick
		// Still alive (pickup only marks it for destroy); no draw on tick 10.
		assert(drop->gameplay_rng.seed == rng_before.seed
			&& drop->gameplay_rng.have_next_gaussian
				== rng_before.have_next_gaussian
			&& drop->gameplay_rng.next_gaussian_cached
				== rng_before.next_gaussian_cached);
		assert(drop->data.item.pickup_delay == 0);
		assert(player_holds_any()); // collectible on tick 10, exactly
		struct item_data got;
		assert(inventory_get_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
								  &got));
		assert(got.id == BLOCK_LEVER && got.count == 1);
		printf("  1. block drop: pickup_delay starts 10, decrements 1/tick, "
			   "collectible on tick 10 exactly OK\n");
	}

	// ========================================================================
	// 2. DISPENSER: server_local_dispenser_dispense -- BlockDispenser
	//    .dispenseItem never touches delayBeforeCanPickup. Starts at 0 and is
	//    collectible on the VERY FIRST tick (this is the other bug half:
	//    dispenser ejects used to be wrongly HELD BACK 0.4s by the old flat
	//    age gate).
	// ========================================================================
	{
		clear_player_inventory();
		set_block(10, Y, 10, BLOCK_DISPENSER, 5); // facing +X (meta 5)
		int idx = dispenser_te_register(&s, 10, Y, 10);
		assert(idx >= 0);
		struct dispenser_te* d = dispenser_te_lookup(&s, 10, Y, 10);
		assert(d);
		memset(d->items, 0, sizeof(d->items));
		d->items[0]
			= (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 1};

		uint32_t before[64];
		size_t before_n = snapshot_ids(before, 64);
		server_local_dispenser_dispense(&s, 10, Y, 10, 5);
		struct entity* eject = find_new_item_entity(before, before_n);
		assert(eject->data.item.item.id == ITEM_STICK);

		// [740c583: BlockDispenser.dispenseItem -- no delayBeforeCanPickup write]
		assert(eject->data.item.pickup_delay == 0);

		item_rest_at_player(eject); // fixture pin (see item_hold_still note)
		bool destroyed = eject->tick_server(eject, &s);
		assert(!destroyed);
		assert(player_holds_any()); // collectible on tick 1
		printf("  2. dispenser eject: pickup_delay == 0, collectible on tick "
			   "1 OK\n");
	}

	// ========================================================================
	// 3. PLAYER Q-DROP: server_local_drop_hotbar_item -- EntityPlayer
	//    .dropPlayerItemWithRandomChoice sets delayBeforeCanPickup = 40. NOT
	//    collectible until the 40th tick (2.0s, not the old bug's 0.4s
	//    instant-hotbar-reshuffle window).
	// ========================================================================
	{
		clear_player_inventory();
		inventory_set_slot(
			&s.player.inventory, INVENTORY_SLOT_HOTBAR,
			(struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 1});

		struct entity* dropped = server_local_drop_hotbar_item(&s, false);
		assert(dropped);
		assert(dropped->data.item.item.id == ITEM_STICK);

		// [740c583: EntityPlayer.dropPlayerItemWithRandomChoice:260 --
		//  `delayBeforeCanPickup = 40;`]
		assert(dropped->data.item.pickup_delay == 40);
		assert(!player_holds_any()); // Q-drop cleared the source slot

		item_rest_at_player(dropped); // fixture pin (see item_hold_still note)

		for(int tick = 1; tick <= 39; tick++) {
			bool destroyed = dropped->tick_server(dropped, &s);
			assert(!destroyed);
			assert(dropped->data.item.pickup_delay == 40 - tick);
			assert(!player_holds_any()); // NOT collectible yet
		}
		assert(dropped->data.item.pickup_delay == 1);
		dropped->tick_server(dropped, &s); // the 40th tick
		assert(dropped->data.item.pickup_delay == 0);
		assert(player_holds_any()); // collectible on tick 40, exactly
		printf("  3. player Q-drop: pickup_delay starts 40, decrements "
			   "1/tick, collectible on tick 40 exactly OK\n");
	}

	// ========================================================================
	// 4. NOT PERSISTED: Beta's writeEntityToNBT/readEntityFromNBT touch only
	//    Health/Age/Item -- a live item primed at a nonzero delay must always
	//    reload at 0, regardless of its value before the save.
	// ========================================================================
	{
		free_all_item_entities();
		uint32_t id = server_local_gen_id(&s);
		struct entity** pp = dict_entity_safe_get(s.entities, id);
		*pp = malloc(sizeof(**pp));
		assert(*pp);
		struct entity* e = *pp;
		memset(e, 0xBE, sizeof(*e));
		entity_item(id, e, true, &s.worlds[0],
					(struct item_data) {.id = ITEM_STICK, .count = 1});
		e->teleport(e, (vec3) {5.5f, (float)Y, 5.5f});
		e->data.item.pickup_delay = 40; // any nonzero value -- picked to
										// match a fresh Q-drop's start value,
										// deliberately mid-countdown.
		assert(dict_entity_size(s.entities) == 1);

		server_world_light_drain(&s.worlds[0]); /* G9: unload waits for the drain */
		test_drain_quiesce();
		test_clin_drain();
		assert(server_world_save_chunk(&s.worlds[0], true, 0, 0));
		assert(!server_world_is_chunk_loaded(&s.worlds[0], 0, 0));
		assert(dict_entity_size(s.entities) == 0); // evicted

		assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
		assert(sc->persisted_entities_nonempty);
		assert(dict_entity_size(s.entities) == 1);

		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		assert(!dict_entity_end_p(it));
		struct entity* reloaded = dict_entity_ref(it)->value;
		assert(reloaded->type == ENTITY_ITEM);
		assert(reloaded->data.item.item.id == ITEM_STICK);
		// NOT the 40 it was saved with -- entity_item()'s own default (0),
		// because the archive/restore path never touches pickup_delay at all
		// (server_entity_archive_copy copies only item/age/death_transaction/
		// death_sequence -- see entity.h's field doc).
		assert(reloaded->data.item.pickup_delay == 0);
		printf("  4. save/evict/reload: pickup_delay does NOT survive (saved "
			   "at 40, reloaded at 0) OK\n");
	}

	free_all_item_entities();
	server_world_destroy(&s.worlds[0]);
	piston_moving_store_destroy(&s.piston_moving);
	inventory_destroy(&s.player.inventory, &s.player);
	dict_entity_clear(s.entities);
	string_clear(level);
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	g_drain_run = 0;
	thread_join(&drain);
	printf("test_item_pickup_delay: PASS (host-only delay path unchanged)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
