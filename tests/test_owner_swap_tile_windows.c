/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924 lane C, C2 tiles: P2 inventory rows #9, #10,
 * #12, #13 (prep/C2inv-out.md), owner-swapped.
 *
 * Beta 1.7.3 (client Jad decompile,
 * ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   - BlockFurnace.blockActivated (BlockFurnace.java): any player who
 *     right-clicks opens displayGUIFurnace on the TileEntityFurnace of the
 *     world it clicked in (world.getBlockTileEntity). Nothing is host-only.
 *   - BlockDispenser.blockActivated (BlockDispenser.java): the same with
 *     displayGUIDispenser / TileEntityDispenser.
 *   - BlockChest.blockActivated (BlockChest.java:199-211): the same with the
 *     (possibly large) chest inventory.
 *   - Removal: onBlockRemoval of BlockFurnace (:142-171), BlockDispenser and
 *     BlockChest scatters the tile's CURRENT contents once, and the open
 *     Container stops being usable because canInteractWith fails
 *     (ContainerFurnace/ContainerDispenser/ContainerChest.isUsableByPlayer ->
 *     the TileEntity's canInteractWith: `worldObj.getBlockTileEntity(x,y,z)
 *     != this ? false : ...`). Only a Container whose tile lived in THAT world
 *     is affected: another dimension's tile at the same coordinates is a
 *     different TileEntity.
 *
 * Invariant pinned for every removal case: the stacks the window showed are
 * conserved -- they exist exactly once afterwards (world item entities plus
 * whatever the actor still holds), no duplication, no loss.
 *
 * One executable, one case per process (argv[1]):
 *   furnace_open, dispenser_open   rows #9 / #10
 *   chest_break                    row #12
 *   furnace_break, dispenser_break row #13
 *   cross_world                    same-coordinate other-world window survives
 *   stale, stale_guest             a window whose world instance is gone
 *                                  refuses clicks and writes nothing back
 *                                  (host leg / guest leg)
 *   furnace_two_viewers,           host + guest on ONE tile: interleaved
 *   dispenser_two_viewers          clicks, smelt ticks and closes conserve
 *                                  every stack (Fable H-1; Beta aliases one
 *                                  TileEntity inventory per Container)
 *   stale_close                    F-A2: a stale window close drops no
 *                                  storage copy
 *   furnace_quick_move_stale       Fable A-H1: a guest Quick Move after a smelt
 *                                  tick does not write the stale copy back
 *   furnace_stale_transaction      Fable A-M1: a stale click is refused
 *                                  against the TRUE pre-click stack */
#include "support/owner_swap_harness.h"

#include "item/window_container.h"
#include "network/inventory_logic.h"
#include "network/server_dispenser.h"
#include "network/server_furnace.h"

#define TEST_NAME "test_owner_swap_tile_windows"

/* Tile props one block east/west of the bodies' feet cell (y == 4). */
#define TY 4

static struct block_data g_blk;

/* Direct server-side calls publish to the bounded client inbox (window
 * snapshots, block updates, drops); with the server paused nothing else
 * drains it, so run each with the harness's temporary consumer thread
 * (tick()'s pattern). */
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

static void right_click(struct server_local* s, struct server_player* actor,
						int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s->worlds[0], x, y, z, &b));
	g_blk = b;
	struct block_info on = {.block = &g_blk, .x = x, .y = y, .z = z};
	struct block_info where = on;
	struct item_data held = {0};
	assert(blocks[b.type] && blocks[b.type]->onRightClick);
	WITH_DRAIN(blocks[b.type]->onRightClick(s, actor, &held, &where, &on,
											SIDE_TOP));
}

/* The dig handler's order (server_local.c SRPC_BLOCK_DIG): the cell becomes
 * air, then the old block's onBreak runs. */
static void break_tile(struct server_local* s, int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s->worlds[0], x, y, z, &b));
	g_blk = b;
	assert(blocks[b.type] && blocks[b.type]->onBreak);
	WITH_DRAIN(put_block(s, x, y, z, BLOCK_AIR, 0);
		blocks[b.type]->onBreak(s, &(struct block_info) {
			.block = &g_blk, .x = x, .y = y, .z = z}));
}

static bool window_open(struct server_player* p, struct inventory_logic* logic,
						int x, int y, int z, struct server_world* w) {
	struct inventory* inv = p->active_inventory;
	return inv && inv != &p->inventory && inv->logic == logic && inv->x == x
		&& inv->y == y && inv->z == z && server_local_window_bound_to(inv, w);
}

static long world_items(struct server_local* s, uint16_t id) {
	long n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s->entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->data.item.item.id == id
		   && e->delay_destroy < 0)
			n += e->data.item.item.count;
	}
	return n;
}

static long held_items(struct server_player* p, uint16_t id) {
	long n = 0;
	for(size_t k = 0; k < INVENTORY_SIZE; k++)
		if(p->inventory.items[k].id == id)
			n += p->inventory.items[k].count;
	if(p->active_inventory != &p->inventory) {
		struct inventory* inv = p->active_inventory;
		for(size_t k = 0; k < inv->capacity; k++)
			if(inv->items[k].id == id)
				n += inv->items[k].count;
	}
	return n;
}

/* The close path can publish more envelopes than the bounded inbox holds
 * (orphan spill + slot clears), so the server processes it with the consumer
 * thread running instead of the harness's drain-after send. */
static void guest_close(struct server_local* s, uint8_t window) {
	struct server_rpc call = {.type = SRPC_WINDOW_CLOSE,
		.payload.window_close.window = window};
	check(svin_rpc_send_from(guest_source(s), &call),
		"queue: the guest's WINDOW_CLOSE is admitted");
	WITH_DRAIN(server_local_test_process_messages(s));
}
static void host_close(struct server_local* s, uint8_t window) {
	struct server_rpc call = {.type = SRPC_WINDOW_CLOSE,
		.payload.window_close.window = window};
	assert(svin_rpc_send(&call));
	WITH_DRAIN(server_local_test_process_messages(s));
}
static bool guest_send_drained(struct server_local* s, struct server_rpc* call) {
	bool queued = svin_rpc_send_from(guest_source(s), call);
	WITH_DRAIN(server_local_test_process_messages(s));
	return queued;
}

static struct furnace_te* place_furnace(struct server_local* s, int x, int z,
										uint16_t id, uint8_t count) {
	WITH_DRAIN(put_block(s, x, TY, z, BLOCK_FURNACE, 2));
	assert(furnace_te_register_w(&s->worlds[0], x, TY, z) >= 0);
	struct furnace_te* f = furnace_te_lookup_w(&s->worlds[0], x, TY, z);
	assert(f);
	f->items[0] = (struct item_data) {.id = id, .count = count};
	return f;
}
static struct dispenser_te* place_dispenser(struct server_local* s, int x,
											int z, uint16_t id,
											uint8_t count) {
	WITH_DRAIN(put_block(s, x, TY, z, BLOCK_DISPENSER, 2));
	assert(dispenser_te_register_w(&s->worlds[0], x, TY, z) >= 0);
	struct dispenser_te* d = dispenser_te_lookup_w(&s->worlds[0], x, TY, z);
	assert(d);
	d->items[0] = (struct item_data) {.id = id, .count = count};
	return d;
}

/* ======================================================================== */
static void case_open(bool furnace) {
	struct server_local* s = admitted_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct inventory_logic* logic
		= furnace ? &inventory_logic_furnace : &inventory_logic_dispenser;
	uint8_t windowc = furnace ? WINDOWC_FURNACE : WINDOWC_DISPENSER;
	const char* what = furnace ? "furnace" : "dispenser";
	int slot = furnace ? FURNACE_SLOT_INPUT : DISPENSER_SLOT_STORAGE;
	if(furnace) {
		place_furnace(s, 2, 0, BLOCK_SAND, 7);
		place_furnace(s, -2, 0, BLOCK_GRAVEL, 5);
	} else {
		place_dispenser(s, 2, 0, ITEM_ARROW, 7);
		place_dispenser(s, -2, 0, ITEM_ARROW, 5);
	}

	right_click(s, h, 2, TY, 0);
	check(window_open(h, logic, 2, TY, 0, &s->worlds[0]),
		"host: right-click opens the tile window bound to the host's world");
	check(h->active_inventory != &h->inventory
		&& h->active_inventory->items[slot].count == 7,
		"host: the window shows THIS tile's contents");

	right_click(s, g, -2, TY, 0);
	char label[160];
	snprintf(label, sizeof label,
		"guest: right-click opens its own %s window in the guest's world "
		"(Block%s.blockActivated has no host-only leg)", what,
		furnace ? "Furnace" : "Dispenser");
	check(window_open(g, logic, -2, TY, 0, server_local_world_of(s, g)), label);
	check(g->active_inventory != &g->inventory
		&& g->active_inventory->items[slot].count == 5,
		"guest: the window shows the tile it clicked, not the host's");
	check(h->active_inventory->items[slot].count == 7,
		"host: the guest's open leaves the host window untouched");
	if(g->active_inventory != &g->inventory)
		guest_close(s, windowc);
	host_close(s, windowc);
	check(h->active_inventory == &h->inventory
		&& g->active_inventory == &g->inventory, "both windows closed");
	teardown(s);
}

static void case_chest_break(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* g = s->guest_actor;
	/* Placed through BlockChest's own item-place path (registers the row). */
	{
		struct block_data air = {.type = BLOCK_AIR};
		struct block_info where = {.block = &air, .x = 2, .y = TY, .z = 0};
		struct block_info on = where;
		struct item_data it = {.id = BLOCK_CHEST, .count = 1};
		bool placed = false;
		WITH_DRAIN(placed = blocks[BLOCK_CHEST]->block_item.onItemPlace(
			s, &s->player, &it, &where, &on, SIDE_TOP));
		assert(placed && block_at(s, 2, TY, 0) == BLOCK_CHEST);
	}
	right_click(s, g, 2, TY, 0);
	check(window_open(g, &inventory_logic_chest, 2, TY, 0, &s->worlds[0]),
		"setup: the guest opened the chest");
	if(g->active_inventory == &g->inventory) {
		teardown(s);
		return;
	}
	g->active_inventory->items[CHEST_SLOT_STORAGE]
		= (struct item_data) {.id = BLOCK_SAND, .count = 9};
	int row = server_local_chest_row_w(&s->worlds[0], 2, TY, 0);
	assert(row >= 0);
	s->chest_items[row][0] = (struct item_data) {.id = BLOCK_SAND, .count = 9};
	long before = world_items(s, BLOCK_SAND) + held_items(g, BLOCK_SAND);

	break_tile(s, 2, TY, 0);
	check(g->active_inventory == &g->inventory
		|| g->active_inventory->invalidated,
		"guest: removing the chest invalidates the guest's window "
		"(TileEntity.canInteractWith fails once the tile is gone)");
	check(world_items(s, BLOCK_SAND) == 9,
		"the chest's 9 stacks are scattered exactly once");
	guest_close(s, WINDOWC_CHEST);
	check(g->active_inventory == &g->inventory, "guest: the window is gone");
	check(world_items(s, BLOCK_SAND) + held_items(g, BLOCK_SAND) == before,
		"conserved: no duplication or loss across break + close");
	teardown(s);
}

static void case_tile_break(bool furnace) {
	struct server_local* s = admitted_fixture();
	struct server_player* g = s->guest_actor;
	struct inventory_logic* logic
		= furnace ? &inventory_logic_furnace : &inventory_logic_dispenser;
	uint8_t windowc = furnace ? WINDOWC_FURNACE : WINDOWC_DISPENSER;
	uint16_t id = furnace ? BLOCK_SAND : ITEM_ARROW;
	if(furnace)
		place_furnace(s, 2, 0, id, 9);
	else
		place_dispenser(s, 2, 0, id, 9);
	right_click(s, g, 2, TY, 0);
	check(window_open(g, logic, 2, TY, 0, &s->worlds[0]),
		"setup: the guest opened the tile window");
	long before = world_items(s, id) + held_items(g, id);
	if(g->active_inventory == &g->inventory) {
		check(false, "cannot exercise the break without the guest window");
		teardown(s);
		return;
	}

	break_tile(s, 2, TY, 0);
	check(g->active_inventory == &g->inventory
		|| g->active_inventory->invalidated,
		"guest: removing the tile invalidates the guest's window");
	check(world_items(s, id) == 9,
		"the tile's 9 stacks are scattered exactly once");
	guest_close(s, windowc);
	check(g->active_inventory == &g->inventory, "guest: the window is gone");
	check(world_items(s, id) + held_items(g, id) == before,
		"conserved: no duplication or loss across break + close");
	teardown(s);
}

/* Slot 1 instance (Nether), hand-made like test_c1_guest_window_bind. */
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

static void case_cross_world(void) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_player* h = &s->player;
	struct server_world* b = make_nether(s);
	/* Same cell in both worlds, different contents. */
	place_furnace(s, 2, 0, BLOCK_SAND, 9);
	assert(server_world_set_block_silent(b, 2, TY, 0,
		(struct block_data) {.type = BLOCK_FURNACE, .metadata = 2}));
	assert(furnace_te_register_w(b, 2, TY, 0) >= 0);
	struct furnace_te* fb = furnace_te_lookup_w(b, 2, TY, 0);
	assert(fb);
	fb->items[0] = (struct item_data) {.id = BLOCK_GRAVEL, .count = 4};
	/* The host's window is bound to the OTHER world's furnace. */
	bool opened = false;
	WITH_DRAIN(opened = server_local_open_inventory_in(s, b, h,
		&inventory_logic_furnace, FURNACE_SIZE, 2, TY, 0, WINDOWC_FURNACE,
		WINDOW_TYPE_FURNACE));
	check(opened, "setup: a furnace window opened in world slot 1");
	check(window_open(h, &inventory_logic_furnace, 2, TY, 0, b)
		&& h->active_inventory->items[FURNACE_SLOT_INPUT].id == BLOCK_GRAVEL,
		"setup: it shows slot 1's furnace, not slot 0's");

	break_tile(s, 2, TY, 0); /* slot 0's furnace */
	check(window_open(h, &inventory_logic_furnace, 2, TY, 0, b)
		&& !h->active_inventory->invalidated
		&& h->active_inventory->items[FURNACE_SLOT_INPUT].id == BLOCK_GRAVEL
		&& h->active_inventory->items[FURNACE_SLOT_INPUT].count == 4,
		"removing slot 0's furnace leaves the slot-1 window live and intact");
	check(world_items(s, BLOCK_SAND) == 9 && world_items(s, BLOCK_GRAVEL) == 0,
		"only slot 0's contents scatter; slot 1's stay in its furnace");
	check(furnace_te_lookup_w(b, 2, TY, 0) != NULL,
		"slot 1's furnace tile survives");
	host_close(s, WINDOWC_FURNACE);
	server_world_discard(b);
	teardown(s);
}

static void case_stale(bool use_guest) {
	struct server_local* s = admitted_fixture();
	struct server_player* p = use_guest ? s->guest_actor : &s->player;
	struct furnace_te* f = place_furnace(s, 2, 0, BLOCK_SAND, 9);
	right_click(s, p, 2, TY, 0);
	check(window_open(p, &inventory_logic_furnace, 2, TY, 0, &s->worlds[0]),
		"setup: the actor opened the furnace");
	if(p->active_inventory == &p->inventory) {
		teardown(s);
		return;
	}
	/* The instance the window was bound to is gone (a newer generation now
	 * holds the slot): the window has no backing and fails closed. */
	p->window_world.generation += 1;
	check(server_local_window_world(p->active_inventory) == NULL,
		"stale binding resolves to no world");
	struct server_rpc click = {.type = SRPC_WINDOW_CLICK,
		.payload.window_click = {.window = WINDOWC_FURNACE,
			.slot = FURNACE_SLOT_INPUT, .action_id = 77,
			.expected = p->active_inventory->items[FURNACE_SLOT_INPUT]}};
	if(use_guest)
		(void)guest_send_drained(s, &click);
	else {
		assert(svin_rpc_send(&click));
		WITH_DRAIN(server_local_test_process_messages(s));
	}
	check(f->items[0].id == BLOCK_SAND && f->items[0].count == 9,
		"a click on the stale window writes nothing into the live tile");
	check(p->active_inventory == &p->inventory
		|| p->active_inventory->picked_item.id == 0,
		"the stale click moved nothing onto the cursor");
	p->window_world.generation -= 1;
	if(p->active_inventory != &p->inventory) {
		if(use_guest)
			guest_close(s, WINDOWC_FURNACE);
		else
			host_close(s, WINDOWC_FURNACE);
	}
	teardown(s);
}


/* ---- two viewers on ONE tile (Fable H-1) -------------------------------- */
static uint16_t g_action = 500;
static void click(struct server_local* s, struct server_player* p,
				  uint8_t window, size_t slot) {
	struct inventory* inv = p->active_inventory;
	assert(inv != &p->inventory);
	struct server_rpc call = {.type = SRPC_WINDOW_CLICK,
		.payload.window_click = {.window = window, .slot = (uint8_t)slot,
			.action_id = ++g_action, .expected = inv->items[slot]}};
	if(p == &s->player) {
		assert(svin_rpc_send(&call));
		WITH_DRAIN(server_local_test_process_messages(s));
	} else {
		(void)guest_send_drained(s, &call);
	}
}
/* A player's holdings of `id`: while a window is open the authoritative
 * player rows and cursor live IN the window (its storage slots are a copy of
 * the tile and are NOT counted); closed, the player's own inventory. */
static long holdings(struct server_player* p, uint16_t id, size_t storage) {
	long n = 0;
	struct inventory* inv = p->active_inventory;
	if(inv != &p->inventory) {
		for(size_t k = storage; k < inv->capacity; k++)
			if(inv->items[k].id == id)
				n += inv->items[k].count;
		if(inv->picked_item.id == id)
			n += inv->picked_item.count;
	} else {
		for(size_t k = 0; k < INVENTORY_SIZE; k++)
			if(p->inventory.items[k].id == id)
				n += p->inventory.items[k].count;
		if(p->inventory.picked_item.id == id)
			n += p->inventory.picked_item.count;
	}
	return n;
}
static long furnace_count(struct server_local* s, struct furnace_te* f,
						  uint16_t id) {
	long n = world_items(s, id) + holdings(&s->player, id, 3)
		+ holdings(s->guest_actor, id, 3);
	for(int k = 0; f && k < 3; k++)
		if(f->items[k].id == id)
			n += f->items[k].count;
	return n;
}

static void case_furnace_two_viewers(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct furnace_te* f = place_furnace(s, 2, 0, BLOCK_IRON_ORE, 7);
	f->items[1] = (struct item_data) {.id = ITEM_COAL, .count = 8};
	f->items[2] = (struct item_data) {.id = ITEM_IRON, .count = 1};
	long ore0 = furnace_count(s, f, BLOCK_IRON_ORE);
	long ingot0 = furnace_count(s, f, ITEM_IRON);
	right_click(s, h, 2, TY, 0);
	right_click(s, g, 2, TY, 0);
	check(window_open(h, &inventory_logic_furnace, 2, TY, 0, &s->worlds[0])
		&& window_open(g, &inventory_logic_furnace, 2, TY, 0, &s->worlds[0]),
		"setup: host AND guest have a window on the same furnace");
	if(h->active_inventory == &h->inventory
	   || g->active_inventory == &g->inventory) {
		teardown(s);
		return;
	}
	/* Host takes the ingot; the guest click right after must see it gone
	 * (both windows alias one furnace) instead of taking a second one. */
	click(s, h, WINDOWC_FURNACE, FURNACE_SLOT_OUTPUT);
	click(s, g, WINDOWC_FURNACE, FURNACE_SLOT_OUTPUT);
	check(h->active_inventory->picked_item.id == ITEM_IRON
		&& g->active_inventory->picked_item.id == 0,
		"one ingot, one taker: the stale guest view cannot take it again");
	check(furnace_count(s, f, ITEM_IRON) == ingot0
		&& furnace_count(s, f, BLOCK_IRON_ORE) == ore0,
		"conserved after the interleaved output clicks");
	/* Smelt one ore (the push reaches only the host window today); then
	 * the guest takes the input stack and the host drops its ingot into the
	 * emptied input slot. */
	WITH_DRAIN(for(int t = 0; t < 200; t++) furnace_server_tick_w(&s->worlds[0]));
	check(f->items[2].id == ITEM_IRON && f->items[2].count == 1
		&& f->items[0].count == 6, "setup: one ore smelted into the output");
	click(s, g, WINDOWC_FURNACE, FURNACE_SLOT_INPUT);
	click(s, h, WINDOWC_FURNACE, FURNACE_SLOT_INPUT);
	click(s, g, WINDOWC_FURNACE, FURNACE_SLOT_OUTPUT);
	check(furnace_count(s, f, ITEM_IRON) + furnace_count(s, f, BLOCK_IRON_ORE)
		== ingot0 + ore0,
		"conserved (ore + ingots) across smelting and interleaved clicks");
	guest_close(s, WINDOWC_FURNACE);
	host_close(s, WINDOWC_FURNACE);
	check(h->active_inventory == &h->inventory
		&& g->active_inventory == &g->inventory, "both windows closed");
	check(furnace_count(s, f, ITEM_IRON) + furnace_count(s, f, BLOCK_IRON_ORE)
		== ingot0 + ore0,
		"conserved after both closes (no close writes a stale copy back)");
	teardown(s);
}

static long dispenser_count(struct server_local* s, struct dispenser_te* d,
							uint16_t id) {
	long n = world_items(s, id) + holdings(&s->player, id, DISPENSER_SIZE_STORAGE)
		+ holdings(s->guest_actor, id, DISPENSER_SIZE_STORAGE);
	for(int k = 0; d && k < DISPENSER_SIZE_STORAGE; k++)
		if(d->items[k].id == id)
			n += d->items[k].count;
	return n;
}

static void case_dispenser_two_viewers(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct dispenser_te* d = place_dispenser(s, 2, 0, ITEM_ARROW, 9);
	d->items[4] = (struct item_data) {.id = ITEM_ARROW, .count = 3};
	long total = dispenser_count(s, d, ITEM_ARROW);
	right_click(s, h, 2, TY, 0);
	right_click(s, g, 2, TY, 0);
	check(window_open(h, &inventory_logic_dispenser, 2, TY, 0, &s->worlds[0])
		&& window_open(g, &inventory_logic_dispenser, 2, TY, 0, &s->worlds[0]),
		"setup: host AND guest have a window on the same dispenser");
	if(h->active_inventory == &h->inventory
	   || g->active_inventory == &g->inventory) {
		teardown(s);
		return;
	}
	click(s, h, WINDOWC_DISPENSER, 0);
	click(s, g, WINDOWC_DISPENSER, 0);
	check(g->active_inventory->picked_item.id == 0,
		"the guest cannot take the stack the host already took");
	click(s, g, WINDOWC_DISPENSER, 4);
	click(s, h, WINDOWC_DISPENSER, 4); /* host drops its 9 onto the emptied slot */
	check(dispenser_count(s, d, ITEM_ARROW) == total,
		"conserved across interleaved clicks");
	host_close(s, WINDOWC_DISPENSER);
	guest_close(s, WINDOWC_DISPENSER);
	check(dispenser_count(s, d, ITEM_ARROW) == total,
		"conserved after both closes");
	teardown(s);
}

/* F-A2: closing a window whose world instance is gone drops only the cursor;
 * the storage copy is discarded (the tile keeps its stacks). */
static void case_stale_close(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* h = &s->player;
	struct furnace_te* f = place_furnace(s, 2, 0, BLOCK_SAND, 9);
	right_click(s, h, 2, TY, 0);
	check(window_open(h, &inventory_logic_furnace, 2, TY, 0, &s->worlds[0]),
		"setup: the host opened the furnace");
	h->window_world.generation += 1; /* the bound instance is gone */
	host_close(s, WINDOWC_FURNACE);
	h->window_world.generation -= 1;
	check(h->active_inventory == &h->inventory, "the stale window closed");
	check(world_items(s, BLOCK_SAND) == 0 && f->items[0].count == 9,
		"no orphan spill: the 9 sand stay in the furnace, none on the ground");
	teardown(s);
}


/* ---- Fable A-H1 / A-M1: stale guest copy vs Quick Move / transaction ---- */
static unsigned g_tx_msgs;
static bool g_tx_accepted;
static void tx_observer(const struct client_rpc* rpc, void* user) {
	consumed(rpc, user);
	if(rpc->type == CRPC_WINDOW_TRANSACTION) {
		g_tx_msgs++;
		g_tx_accepted = rpc->payload.window_transaction.accepted;
	}
}
static void quick_move(struct server_local* s, struct server_player* p,
					   uint8_t window, size_t slot) {
	struct inventory* inv = p->active_inventory;
	struct server_rpc call = {.type = SRPC_WINDOW_CLICK,
		.payload.window_click = {.window = window, .slot = (uint8_t)slot,
			.action_id = ++g_action, .quick_move = true,
			.expected = inv->items[slot]}};
	if(p == &s->player) {
		assert(svin_rpc_send(&call));
		WITH_DRAIN(server_local_test_process_messages(s));
	} else {
		(void)guest_send_drained(s, &call);
	}
}

/* A-H1: the guest's copy goes stale on a smelt tick; a Quick Move (which
 * never runs pre_action) must not write it back. */
static void case_furnace_quick_move_stale(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	g->inventory.items[INVENTORY_SLOT_HOTBAR]
		= (struct item_data) {.id = BLOCK_DIRT, .count = 5};
	struct furnace_te* f = place_furnace(s, 2, 0, BLOCK_IRON_ORE, 7);
	f->items[1] = (struct item_data) {.id = ITEM_COAL, .count = 8};
	long total0 = furnace_count(s, f, BLOCK_IRON_ORE) + furnace_count(s, f, ITEM_IRON);
	right_click(s, h, 2, TY, 0);
	right_click(s, g, 2, TY, 0);
	check(window_open(g, &inventory_logic_furnace, 2, TY, 0, &s->worlds[0]),
		"setup: the guest has the furnace window open");
	if(g->active_inventory == &g->inventory) {
		teardown(s);
		return;
	}
	WITH_DRAIN(for(int t = 0; t < 200; t++) furnace_server_tick_w(&s->worlds[0]));
	check(f->items[0].count == 6 && f->items[2].id == ITEM_IRON
		&& f->items[2].count == 1, "setup: one ore smelted");
	/* Stale the guest's copy BY HAND (the pre-smelt view: 7 ore, no ingot),
	 * so this pins the click-handler refresh itself -- with v3's per-body
	 * smelt push the tick alone would already have refreshed the copy. */
	g->active_inventory->items[FURNACE_SLOT_INPUT]
		= (struct item_data) {.id = BLOCK_IRON_ORE, .count = 7};
	g->active_inventory->items[FURNACE_SLOT_OUTPUT] = (struct item_data) {0};
	/* Quick-move the guest's hotbar dirt (player rows only). */
	quick_move(s, g, WINDOWC_FURNACE, FURNACE_SLOT_HOTBAR);
	check(f->items[0].id == BLOCK_IRON_ORE && f->items[0].count == 6
		&& f->items[2].id == ITEM_IRON && f->items[2].count == 1,
		"the TE keeps 6 ore / 1 ingot: the stale guest copy is not written "
		"back by a Quick Move");
	check(furnace_count(s, f, BLOCK_IRON_ORE) + furnace_count(s, f, ITEM_IRON)
		== total0, "ore + ingots conserved");
	guest_close(s, WINDOWC_FURNACE);
	host_close(s, WINDOWC_FURNACE);
	check(furnace_count(s, f, BLOCK_IRON_ORE) + furnace_count(s, f, ITEM_IRON)
		== total0, "conserved after both closes");
	teardown(s);
}

/* A-M1: a click from a stale guest copy is judged against the TRUE pre-click
 * stack: the transaction is refused (the client then gets the correction)
 * instead of "accepted" with a different server result (phantom cursor). */
static void case_furnace_stale_transaction(void) {
	struct server_local* s = admitted_fixture();
	clin_test_set_consume_observer(tx_observer, NULL);
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	struct furnace_te* f = place_furnace(s, 2, 0, BLOCK_IRON_ORE, 7);
	f->items[1] = (struct item_data) {.id = ITEM_COAL, .count = 8};
	right_click(s, h, 2, TY, 0);
	right_click(s, g, 2, TY, 0);
	check(window_open(g, &inventory_logic_furnace, 2, TY, 0, &s->worlds[0]),
		"setup: the guest has the furnace window open");
	if(g->active_inventory == &g->inventory) {
		teardown(s);
		return;
	}
	WITH_DRAIN(for(int t = 0; t < 200; t++) furnace_server_tick_w(&s->worlds[0]));
	/* The guest's copy (and so its client) still shows an EMPTY output. */
	g->active_inventory->items[FURNACE_SLOT_OUTPUT] = (struct item_data) {0};
	g_tx_msgs = 0;
	click(s, g, WINDOWC_FURNACE, FURNACE_SLOT_OUTPUT); /* expected = empty */
	check(g_tx_msgs >= 1 && !g_tx_accepted,
		"the stale click is REFUSED (expected empty, true stack 1 ingot) so the "
		"guest client is corrected, not told 'accepted'");
	guest_close(s, WINDOWC_FURNACE);
	host_close(s, WINDOWC_FURNACE);
	teardown(s);
}

int main(int argc, char** argv) {
	harness_init();
	const char* which = argc > 1 ? argv[1] : "furnace_open";
	if(!strcmp(which, "furnace_open"))
		case_open(true);
	else if(!strcmp(which, "dispenser_open"))
		case_open(false);
	else if(!strcmp(which, "chest_break"))
		case_chest_break();
	else if(!strcmp(which, "furnace_break"))
		case_tile_break(true);
	else if(!strcmp(which, "dispenser_break"))
		case_tile_break(false);
	else if(!strcmp(which, "cross_world"))
		case_cross_world();
	else if(!strcmp(which, "stale"))
		case_stale(false);
	else if(!strcmp(which, "stale_guest"))
		case_stale(true);
	else if(!strcmp(which, "furnace_two_viewers"))
		case_furnace_two_viewers();
	else if(!strcmp(which, "dispenser_two_viewers"))
		case_dispenser_two_viewers();
	else if(!strcmp(which, "stale_close"))
		case_stale_close();
	else if(!strcmp(which, "furnace_quick_move_stale"))
		case_furnace_quick_move_stale();
	else if(!strcmp(which, "furnace_stale_transaction"))
		case_furnace_stale_transaction();
	else {
		printf("unknown case %s\n", which);
		return 2;
	}
	return harness_result(TEST_NAME, which);
}
