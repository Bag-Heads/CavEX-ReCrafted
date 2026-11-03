/* Copyright (c) 2026 CaveWii_v2 contributors
 * ci-1101/25 -- DATA LOSS: a chest/furnace broken in the SECOND dimension
 * while a window is open (split-screen, two live worlds).
 *
 * Beta 1.7.3 (decompile, ../mcb173-retromcp/minecraft/src_original/net/
 * minecraft/src/): BlockChest.onBlockRemoval (BlockChest.java:129-157) and
 * BlockFurnace.onBlockRemoval (:142-171) read world.getBlockTileEntity(x,y,z)
 * of the world the block is removed FROM and scatter its contents into that
 * same world at the block. An open Container on that tile only stops being
 * usable (TileEntity.canInteractWith); a Container on another dimension's
 * tile at the same coordinates is a different TileEntity and is untouched.
 *
 * World slot 0 is the Overworld, slot 1 a hand-made Nether instance; both
 * hold a tile at the SAME (x,y,z) with different contents. Every case pins:
 * no item is lost, none is duplicated, and the drops land in the broken
 * block's world inside the broken block's cell.
 *
 * One case per process (argv[1]):
 *   guest_nether_open   (a) guest has the Nether chest open, it is broken
 *   host_overworld_open (b) host has the Overworld chest open at the same
 *                           cell; the (closed) Nether chest is broken
 *   both_open           (a)+(b) both windows open, the Nether chest breaks
 *   furnace_two_dims    furnace equivalent of both_open
 *   double_explosion    R5: both halves of an open Nether double chest break
 *                       (explosion), the second spill is forced to fail --
 *                       every item leaves exactly once (the window owns it)
 *   nether_retry        R5: a closed Nether chest spill fails once; the
 *                       orphan sweep retries it later in the Nether
 *   nether_close_retry  R5: an open Nether chest spill fails; the guest
 *                       window's deferred close scatters it in the Nether
 *   furnace_cursor_close R5c: guest cursor stack in a Nether furnace window,
 *                       explosion -> the close drops it in the Nether */
#include "support/owner_swap_harness.h"

#include "item/window_container.h"
#include "network/inventory_chest_shared.h"
#include "network/inventory_logic.h"
#include "network/server_furnace.h"
#include "network/tile_entities_store.h"

#define TEST_NAME "test_chest_break_two_dims"

extern void server_local_test_retry_orphaned_container_drops(
	struct server_local* s);

#define TX 2
#define TY 4
#define TZ 0

static struct thread g_consumer;
static void drain_on(void) {
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&g_consumer, consumer_entry, NULL, 64));
}
static void drain_off(void) {
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&g_consumer);
	do {
		clin_update_drain();
	} while(!clin_rpc_flush_pending());
	clin_update_drain();
}
#define WITH_DRAIN(stmt) do { drain_on(); stmt; drain_off(); } while(0)

static struct server_world* make_nether(struct server_local* s) {
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, WORLD_DIM_NETHER, 12345, false);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	struct server_chunk* c = NULL;
	for(unsigned n = 0; n < 200 && !c; n++) {
		server_world_begin_update(b);
		(void)server_world_load_chunk(b, 0, 0, &c);
	}
	assert(c);
	return b;
}

static long all_items(struct server_local* s, uint16_t id) {
	long n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->delay_destroy < 0
		   && e->data.item.item.id == id)
			n += e->data.item.item.count;
	}
	return n;
}

/* Item units of `id` that are live item entities bound to world `w`;
 * *outside counts units of `id` in `w` not inside the (TX,TY,TZ) cell. */
static long items_in(struct server_local* s, struct server_world* w,
					 uint16_t id, long* outside) {
	long n = 0;
	if(outside)
		*outside = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(!e || e->type != ENTITY_ITEM || e->delay_destroy >= 0
		   || e->data.item.item.id != id
		   || server_local_entity_world(s, e) != w)
			continue;
		n += e->data.item.item.count;
		bool inside = e->pos[0] >= TX && e->pos[0] < TX + 1
			&& e->pos[1] >= TY && e->pos[1] < TY + 1
			&& e->pos[2] >= TZ && e->pos[2] < TZ + 1;
		if(!inside && outside)
			*outside += e->data.item.item.count;
	}
	return n;
}

static long window_units(struct server_player* p, uint16_t id) {
	struct inventory* inv = p->active_inventory;
	long n = 0;
	if(!inv || inv == &p->inventory)
		return 0;
	for(size_t k = 0; k < inv->capacity; k++)
		if(inv->items[k].id == id)
			n += inv->items[k].count;
	return n;
}

static long row_units(struct server_world* w, uint16_t id) {
	int row = server_local_chest_row_w(w, TX, TY, TZ);
	if(row < 0)
		return 0;
	struct server_local* s = w->session;
	long n = 0;
	for(size_t k = 0; k < CHEST_SIZE_STORAGE; k++)
		if(s->chest_items[row][k].id == id)
			n += s->chest_items[row][k].count;
	return n;
}

static void put_chest_at(struct server_local* s, struct server_world* w,
						 int x, uint16_t id, uint8_t count) {
	WITH_DRAIN(assert(server_world_set_block(w, x, TY, TZ,
		(struct block_data) {.type = BLOCK_CHEST})));
	struct te_store st = server_world_tile_store(w);
	int row = te_store_claim(&st, TE_KIND_CHEST, x, TY, TZ, NULL);
	assert(row >= 0);
	memset(s->chest_items[row], 0, sizeof(s->chest_items[row]));
	s->chest_items[row][0] = (struct item_data) {.id = id, .count = count};
	s->chest_items[row][3] = (struct item_data) {.id = id, .count = 1};
}
static void put_chest(struct server_local* s, struct server_world* w,
					  uint16_t id, uint8_t count) {
	put_chest_at(s, w, TX, id, count);
}

static void open_in(struct server_local* s, struct server_world* w,
					struct server_player* p, bool furnace) {
	bool ok = false;
	WITH_DRAIN(ok = furnace
		? server_local_open_inventory_in(s, w, p, &inventory_logic_furnace,
			FURNACE_SIZE, TX, TY, TZ, WINDOWC_FURNACE, WINDOW_TYPE_FURNACE)
		: server_local_open_inventory_in(s, w, p, &inventory_logic_chest,
			CHEST_SIZE, TX, TY, TZ, WINDOWC_CHEST, WINDOW_TYPE_CHEST));
	assert(ok && p->active_inventory != &p->inventory);
	assert(server_local_window_bound_to(p->active_inventory, w));
}

/* The dig order (SRPC_BLOCK_DIG): the cell becomes air in the dig world, then
 * the old block's onBreak runs with block_world = that world. */
static void break_at(struct server_local* s, struct server_world* w, int x) {
	struct block_data b;
	assert(server_world_get_block(w, x, TY, TZ, &b));
	static struct block_data g_blk;
	g_blk = b;
	assert(blocks[b.type] && blocks[b.type]->onBreak);
	struct server_world* prev = block_world_enter(s, w);
	WITH_DRAIN(assert(server_world_set_block(w, x, TY, TZ,
		(struct block_data) {.type = BLOCK_AIR}));
		blocks[b.type]->onBreak(s, &(struct block_info) {
			.block = &g_blk, .x = x, .y = TY, .z = TZ}));
	block_world_leave(s, prev);
}
static void break_in(struct server_local* s, struct server_world* w) {
	break_at(s, w, TX);
}

static void close_window(struct server_local* s, struct server_player* p,
						 uint8_t window) {
	if(p->active_inventory == &p->inventory)
		return;
	struct server_rpc call = {.type = SRPC_WINDOW_CLOSE,
		.payload.window_close.window = window};
	if(p == &s->player)
		assert(svin_rpc_send(&call));
	else
		assert(svin_rpc_send_from(guest_source(s), &call));
	WITH_DRAIN(server_local_test_process_messages(s));
}

/* ======================================================================== */
static void case_chest(bool guest_open, bool host_open) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_nether(s);
	/* Same cell: Overworld chest holds sand, Nether chest holds gravel. */
	put_chest(s, a, BLOCK_SAND, 9);
	put_chest(s, b, BLOCK_GRAVEL, 7);
	if(host_open)
		open_in(s, a, h, false);
	if(guest_open)
		open_in(s, b, g, false);
	check(row_units(a, BLOCK_SAND) == 10 && row_units(b, BLOCK_GRAVEL) == 8,
		"setup: one chest per world at the same cell");
	if(host_open)
		check(window_units(h, BLOCK_SAND) == 10,
			"setup: the host window shows the Overworld chest");
	if(guest_open)
		check(window_units(g, BLOCK_GRAVEL) == 8,
			"setup: the guest window shows the Nether chest");

	break_in(s, b);

	long out_b = 0;
	check(items_in(s, b, BLOCK_GRAVEL, &out_b) == 8,
		"the Nether chest's 8 units scatter exactly once into the Nether");
	check(out_b == 0, "every Nether drop is inside the broken block's cell");
	check(items_in(s, a, BLOCK_GRAVEL, NULL) == 0,
		"no Nether contents appear in the Overworld");
	check(items_in(s, a, BLOCK_SAND, NULL) == 0
		&& items_in(s, b, BLOCK_SAND, NULL) == 0,
		"the Overworld chest's contents do not spill anywhere");
	check(server_local_chest_row_w(b, TX, TY, TZ) < 0,
		"the Nether chest row is released");
	check(row_units(a, BLOCK_SAND) == 10,
		"the Overworld chest row is untouched");
	if(guest_open)
		check(window_units(g, BLOCK_GRAVEL) == 0
			&& g->active_inventory->invalidated,
			"guest: the broken chest's window is dead and empty (no phantom "
			"items)");
	if(host_open)
		check(window_units(h, BLOCK_SAND) == 10
			&& !h->active_inventory->invalidated
			&& inventory_chest_view_at_w(s, a, TX, TY, TZ)
				== h->active_inventory,
			"host: the Overworld window stays live with all its stacks");

	close_window(s, g, WINDOWC_CHEST);
	close_window(s, h, WINDOWC_CHEST);
	check(items_in(s, b, BLOCK_GRAVEL, NULL) + window_units(g, BLOCK_GRAVEL)
			+ items_in(s, a, BLOCK_GRAVEL, NULL) == 8,
		"conserved after close: Nether units exist exactly once");
	check(row_units(a, BLOCK_SAND) + items_in(s, a, BLOCK_SAND, NULL)
			+ items_in(s, b, BLOCK_SAND, NULL) == 10,
		"conserved after close: Overworld units exist exactly once");
	server_world_discard(b);
	teardown(s);
}

static void case_furnace_two_dims(void) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_nether(s);
	struct server_world* worlds[2] = {a, b};
	uint16_t ids[2] = {BLOCK_SAND, BLOCK_GRAVEL};
	for(int k = 0; k < 2; k++) {
		WITH_DRAIN(assert(server_world_set_block(worlds[k], TX, TY, TZ,
			(struct block_data) {.type = BLOCK_FURNACE, .metadata = 2})));
		assert(furnace_te_register_w(worlds[k], TX, TY, TZ) >= 0);
		struct furnace_te* f = furnace_te_lookup_w(worlds[k], TX, TY, TZ);
		assert(f);
		f->items[0] = (struct item_data) {.id = ids[k], .count = 9};
	}
	open_in(s, a, h, true);
	open_in(s, b, g, true);
	check(window_units(h, BLOCK_SAND) == 9
		&& window_units(g, BLOCK_GRAVEL) == 9,
		"setup: each window shows its own world's furnace");

	break_in(s, b);

	long out_b = 0;
	check(items_in(s, b, BLOCK_GRAVEL, &out_b) == 9 && out_b == 0,
		"the Nether furnace's 9 units scatter once into the Nether cell");
	check(items_in(s, a, BLOCK_GRAVEL, NULL) == 0
		&& items_in(s, a, BLOCK_SAND, NULL) == 0
		&& items_in(s, b, BLOCK_SAND, NULL) == 0,
		"nothing else spills, in either world");
	check(!furnace_te_lookup_w(b, TX, TY, TZ)
		&& furnace_te_lookup_w(a, TX, TY, TZ)
		&& furnace_te_lookup_w(a, TX, TY, TZ)->items[0].count == 9,
		"only the Nether furnace tile is removed");
	check(window_units(g, BLOCK_GRAVEL) == 0
		&& g->active_inventory->invalidated,
		"guest: the broken furnace's window is dead and empty");
	check(window_units(h, BLOCK_SAND) == 9 && !h->active_inventory->invalidated,
		"host: the Overworld furnace window stays live");
	close_window(s, g, WINDOWC_FURNACE);
	close_window(s, h, WINDOWC_FURNACE);
	check(items_in(s, b, BLOCK_GRAVEL, NULL) == 9,
		"conserved after close: no duplicate from the dead window");
	server_world_discard(b);
	teardown(s);
}

/* ---- R5 ----------------------------------------------------------------- */
static void case_double_explosion(void) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_player* g = s->guest_actor;
	struct server_world* b = make_nether(s);
	/* Halves at TX (gravel, 8 units) and TX+1 (sand, 6 units). */
	put_chest_at(s, b, TX, BLOCK_GRAVEL, 7);
	put_chest_at(s, b, TX + 1, BLOCK_SAND, 5);
	bool ok = false;
	WITH_DRAIN(ok = server_local_open_linked_inventory_in(s, b, g,
		&inventory_logic_double_chest, IRON_CHEST_SIZE, TX, TY, TZ, TX + 1,
		TY, TZ, WINDOWC_IRON_CHEST, WINDOW_TYPE_CHEST));
	assert(ok && g->active_inventory != &g->inventory);
	check(window_units(g, BLOCK_GRAVEL) == 8 && window_units(g, BLOCK_SAND) == 6,
		"setup: the guest double window shows both Nether halves");

	/* One explosion removes both halves: the first spill succeeds and marks
	 * the window dead; the second spill fails (EntityItem OOM). */
	break_at(s, b, TX);
	server_local_test_fail_next_container_drop();
	break_at(s, b, TX + 1);
	check(all_items(s, BLOCK_GRAVEL) == 8,
		"first half: its 8 units scatter once");
	check(all_items(s, BLOCK_SAND) == 0 && window_units(g, BLOCK_SAND) == 6,
		"second half: the failed spill leaves its stacks in the window only");
	check(server_local_chest_row_w(b, TX + 1, TY, TZ) < 0,
		"second half: the backing row is released, not dropped as well");
	server_local_test_retry_orphaned_container_drops(s);
	check(all_items(s, BLOCK_SAND) == 0,
		"the orphan sweep does not drop the window-owned half again");
	close_window(s, g, WINDOWC_IRON_CHEST);
	check(g->active_inventory == &g->inventory, "guest: the window closed");
	check(items_in(s, b, BLOCK_SAND, NULL) == 6 && all_items(s, BLOCK_SAND) == 6,
		"close: the second half's 6 units drop exactly once, in the Nether");
	check(all_items(s, BLOCK_GRAVEL) == 8,
		"close: the first half is not dropped again");
	server_world_discard(b);
	teardown(s);
}

static void case_nether_retry(void) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_nether(s);
	put_chest(s, a, BLOCK_SAND, 9);
	put_chest(s, b, BLOCK_GRAVEL, 7);
	server_local_test_fail_next_container_drop();
	break_in(s, b);
	check(all_items(s, BLOCK_GRAVEL) == 0 && row_units(b, BLOCK_GRAVEL) == 8,
		"failed closed spill: the Nether row is retained for the retry");
	/* The sweep runs outside any block dispatch (block_world = slot 0). */
	server_local_test_retry_orphaned_container_drops(s);
	long out_b = 0;
	check(items_in(s, b, BLOCK_GRAVEL, &out_b) == 8 && out_b == 0,
		"retry: the 8 units scatter in the Nether at the block");
	check(items_in(s, a, BLOCK_GRAVEL, NULL) == 0,
		"retry: nothing lands in the Overworld");
	check(server_local_chest_row_w(b, TX, TY, TZ) < 0,
		"retry: the Nether row is released");
	check(row_units(a, BLOCK_SAND) == 10 && all_items(s, BLOCK_SAND) == 0,
		"the Overworld chest is untouched");
	server_local_test_retry_orphaned_container_drops(s);
	check(all_items(s, BLOCK_GRAVEL) == 8, "a second sweep drops nothing more");
	server_world_discard(b);
	teardown(s);
}

static void case_nether_close_retry(void) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_player* g = s->guest_actor;
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_nether(s);
	put_chest(s, b, BLOCK_GRAVEL, 7);
	open_in(s, b, g, false);
	server_local_test_fail_next_container_drop();
	break_in(s, b);
	check(all_items(s, BLOCK_GRAVEL) == 0 && window_units(g, BLOCK_GRAVEL) == 8
		&& g->active_inventory->invalidated,
		"failed open spill: the dead window keeps the 8 units");
	check(server_local_chest_row_w(b, TX, TY, TZ) < 0,
		"the window owns them: the row is released, not dropped");
	close_window(s, g, WINDOWC_CHEST);
	long out_b = 0;
	check(items_in(s, b, BLOCK_GRAVEL, &out_b) == 8 && out_b == 0,
		"deferred close: the 8 units scatter in the Nether at the block");
	check(items_in(s, a, BLOCK_GRAVEL, NULL) == 0,
		"deferred close: nothing lands in the Overworld");
	server_world_discard(b);
	teardown(s);
}

/* R5c: no failure needed -- a guest holding a cursor stack in a Nether
 * furnace window; an explosion removes the furnace; the window's close drops
 * the cursor (dropPlayerItem) in the Nether at the player, not slot 0. */
static void case_furnace_cursor_close(void) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_player* g = s->guest_actor;
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_nether(s);
	WITH_DRAIN(assert(server_world_set_block(b, TX, TY, TZ,
		(struct block_data) {.type = BLOCK_FURNACE, .metadata = 2})));
	assert(furnace_te_register_w(b, TX, TY, TZ) >= 0);
	furnace_te_lookup_w(b, TX, TY, TZ)->items[0]
		= (struct item_data) {.id = BLOCK_GRAVEL, .count = 9};
	open_in(s, b, g, true);
	g->active_inventory->picked_item
		= (struct item_data) {.id = ITEM_STICK, .count = 5};
	break_in(s, b);
	close_window(s, g, WINDOWC_FURNACE);
	check(g->active_inventory == &g->inventory, "guest: the window closed");
	check(items_in(s, b, BLOCK_GRAVEL, NULL) == 9
		&& items_in(s, a, BLOCK_GRAVEL, NULL) == 0,
		"the furnace contents scatter in the Nether");
	check(items_in(s, b, ITEM_STICK, NULL) == 5,
		"the cursor's 5 sticks drop in the Nether");
	check(items_in(s, a, ITEM_STICK, NULL) == 0,
		"no cursor item lands in the Overworld");
	bool near_player = true;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->data.item.item.id == ITEM_STICK
		   && (fabs(e->pos[0] - g->x) > 1.5 || fabs(e->pos[1] - g->y) > 2.0
			   || fabs(e->pos[2] - g->z) > 1.5))
			near_player = false;
	}
	check(near_player, "the cursor drops at the player");
	server_world_discard(b);
	teardown(s);
}

/* R5d: the player's OWN inventory (window 0) close never inherits the
 * binding retained from the last tile window: close a Nether furnace, then
 * close window 0 holding a cursor stack -> the drop keeps the ambient world
 * (slot 0 here), exactly like the direct own-inventory close. */
static void case_own_inventory_after_tile(void) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_player* g = s->guest_actor;
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_nether(s);
	WITH_DRAIN(assert(server_world_set_block(b, TX, TY, TZ,
		(struct block_data) {.type = BLOCK_FURNACE, .metadata = 2})));
	assert(furnace_te_register_w(b, TX, TY, TZ) >= 0);
	open_in(s, b, g, true);
	close_window(s, g, WINDOWC_FURNACE);
	assert(g->active_inventory == &g->inventory);
	check(server_local_window_bound_to(&g->inventory, b),
		"setup: the guest's binding still names the Nether furnace world");
	g->inventory.picked_item = (struct item_data) {.id = ITEM_STICK, .count = 5};
	struct server_rpc call = {.type = SRPC_WINDOW_CLOSE,
		.payload.window_close.window = 0};
	assert(svin_rpc_send_from(guest_source(s), &call));
	WITH_DRAIN(server_local_test_process_messages(s));
	check(items_in(s, b, ITEM_STICK, NULL) == 0,
		"window 0 close: no cursor item lands in the stale window world");
	check(items_in(s, a, ITEM_STICK, NULL) == 5,
		"window 0 close: the cursor drops in the ambient world");
	server_world_discard(b);
	teardown(s);
}

/* R9: P2 (slot 1) standing in the Nether closes its OWN inventory (window 0)
 * with a cursor stack and crafting-grid inputs. Beta drops them through the
 * player (Container.onCraftGuiClosed -> dropPlayerItem ->
 * joinEntityItemWithWorld on the player's worldObj): in the Nether at P2,
 * via the RPC close (direct == false) and the direct close (direct == true). */
static void case_own_inventory_nether(bool direct) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_player* g = s->guest_actor;
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_nether(s);
	g->world_slot = 1; /* P2's body is committed to the Nether */
	assert(server_local_world_of(s, g) == b);
	g->inventory.picked_item = (struct item_data) {.id = ITEM_STICK, .count = 5};
	g->inventory.items[INVENTORY_SLOT_CRAFTING]
		= (struct item_data) {.id = BLOCK_GRAVEL, .count = 3};
	if(direct) {
		bool ok = false;
		WITH_DRAIN(ok = server_local_close_active_inventory_for(s, g));
		check(ok, "direct close succeeds");
	} else {
		struct server_rpc call = {.type = SRPC_WINDOW_CLOSE,
			.payload.window_close.window = 0};
		assert(svin_rpc_send_from(guest_source(s), &call));
		WITH_DRAIN(server_local_test_process_messages(s));
	}
	check(items_in(s, b, ITEM_STICK, NULL) == 5
		&& items_in(s, b, BLOCK_GRAVEL, NULL) == 3,
		"window 0 close: cursor and grid items drop in the Nether");
	check(items_in(s, a, ITEM_STICK, NULL) == 0
		&& items_in(s, a, BLOCK_GRAVEL, NULL) == 0,
		"window 0 close: nothing lands in the Overworld");
	bool near_player = true;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM
		   && (fabs(e->pos[0] - g->x) > 1.5 || fabs(e->pos[1] - g->y) > 2.0
			   || fabs(e->pos[2] - g->z) > 1.5))
			near_player = false;
	}
	check(near_player, "window 0 close: the drops are at P2");
	g->world_slot = 0;
	server_world_discard(b);
	teardown(s);
}

int main(int argc, char** argv) {
	harness_init();
	const char* which = argc > 1 ? argv[1] : "guest_nether_open";
	if(!strcmp(which, "guest_nether_open"))
		case_chest(true, false);
	else if(!strcmp(which, "host_overworld_open"))
		case_chest(false, true);
	else if(!strcmp(which, "both_open"))
		case_chest(true, true);
	else if(!strcmp(which, "furnace_two_dims"))
		case_furnace_two_dims();
	else if(!strcmp(which, "double_explosion"))
		case_double_explosion();
	else if(!strcmp(which, "nether_retry"))
		case_nether_retry();
	else if(!strcmp(which, "nether_close_retry"))
		case_nether_close_retry();
	else if(!strcmp(which, "furnace_cursor_close"))
		case_furnace_cursor_close();
	else if(!strcmp(which, "own_inventory_after_tile"))
		case_own_inventory_after_tile();
	else if(!strcmp(which, "own_inventory_nether_rpc"))
		case_own_inventory_nether(false);
	else if(!strcmp(which, "own_inventory_nether_direct"))
		case_own_inventory_nether(true);
	else {
		printf("unknown case %s\n", which);
		return 2;
	}
	return harness_result(TEST_NAME, which);
}
