/* C1 (split-dimensions lane C -> A handoff, Astra next-0926 row "C1 -> A
 * handoff"): a guest tile window RESTORED from the guest checkpoint
 * (server_local_guest.c guest_private_read) is bound to the guest's
 * committed world BEFORE inventory_create[_linked] runs, exactly like the
 * live open path (server_local_open_inventory_in). Until this lands the
 * restored window is UNBOUND and only resolves through the inventoried
 * slot-0 fallback in server_local_window_world (lane A removes that fallback
 * after C1).
 *
 * Two world instances in one session (slot 0 Overworld, slot 1 Nether, the
 * slot-1 instance hand-made like test_a1b_tile_lifecycle) hold a chest at the
 * IDENTICAL cell with different contents. Cases:
 *  1. restore/rejoin (checkpoint -> archive save -> reopen from disk ->
 *     rejoin): the restored window's binding is the guest's committed world
 *     (slot 0 handle), it shows THAT world's chest (never slot 1's), and a
 *     stale generation of that world leaves it backing-less (an unbound
 *     window would silently fall back to slot 0);
 *  2. no live committed world: the restore is REFUSED, losslessly;
 *  3. create failure (backing chest gone): refused, the record, the private
 *     stacks and both worlds' chest rows are intact; a later rejoin restores
 *     everything;
 *  4. the restore step itself (test seam) restores the PRIOR binding when
 *     the create fails, and binds on success. */
#include "support/split_residency_fixture.h"

#include "item/items.h"
#include "item/window_container.h"
#include "network/inventory_chest_shared.h"
#include "network/inventory_logic.h"
#include "network/server_local_guest.h"
#include "network/tile_entities_store.h"
#include "network/window_registry.h"

#include <stdio.h>

/* Test-only seam in server_local_guest.c (CAVEWII_SERVER_LOCAL_TEST): runs
 * the guest window-restore step for `p` from a decoded CaveWiiWindow record
 * (windowc, wire type, x, y, z, x2, y2, z2). On success *out is the created,
 * unpublished window (p->active_inventory is not touched). */
bool server_local_guest_test_restore_window(struct server_local* s,
	struct server_player* p, const int32_t window[8], struct inventory** out);

#define TX 3
#define TY 10
#define TZ 5

static void observe(const struct client_rpc* call, void* user) {
	(void)call;
	(void)user;
	clin_drain_discard();
}

static void noop_reply(const struct client_rpc* call, void* user) {
	(void)call;
	(void)user;
}

static struct server_world* make_nether(struct server_local* s) {
	struct server_world* b = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(b, path, WORLD_DIM_NETHER, 12345, false);
	string_clear(path);
	b->local = s;
	b->session = s;
	b->world_index = 1;
	return b;
}

static void load(struct server_world* w, int cx, int cz) {
	struct server_chunk* c = NULL;
	for(unsigned n = 0; n < 200 && !c; n++) {
		server_world_begin_update(w);
		(void)server_world_load_chunk(w, cx, cz, &c);
	}
	assert(c);
}

static void put(struct server_world* w, uint8_t type) {
	assert(server_world_set_block_silent(w, TX, TY, TZ,
		(struct block_data) {.type = type}));
}

/* Drop the guest body WITHOUT checkpointing (simulated process loss), then
 * reopen the archive from disk so the next join reads the saved record. */
static void crash_guest(struct server_local* s) {
	struct server_player* g = s->guest_actor;
	assert(g);
	g->inventory.reply = noop_reply;
	if(g->active_inventory != &g->inventory) {
		g->active_inventory->reply = noop_reply;
		inventory_destroy_owned(g->active_inventory, g);
	}
	inventory_destroy(&g->inventory, g);
	free(g);
	s->guest_actor = NULL;
	s->guest_owned = false;
	s->guest_generation = 0;
	level_archive_discard(&s->level);
	string_t path;
	string_init_set_str(path, split_path);
	assert(level_archive_create(&s->level, path));
	string_clear(path);
}

static nbt_node* record_bytes(struct server_local* s, const char* leaf,
							  unsigned char* copy, size_t len) {
	char path[96];
	snprintf(path, sizeof(path), ".Data.CaveWiiGuest.%s", leaf);
	nbt_node* n = nbt_find_by_path(s->level.data, path);
	assert(n && n->type == TAG_BYTE_ARRAY
		   && (size_t)n->payload.tag_byte_array.length == len);
	if(copy)
		memcpy(copy, n->payload.tag_byte_array.data, len);
	return n;
}

static void assert_record_intact(struct server_local* s,
								 const unsigned char priv[75],
								 const unsigned char win[32]) {
	nbt_node* p = record_bytes(s, "CaveWiiPrivate", NULL, 75);
	nbt_node* w = record_bytes(s, "CaveWiiWindow", NULL, 32);
	assert(!memcmp(p->payload.tag_byte_array.data, priv, 75));
	assert(!memcmp(w->payload.tag_byte_array.data, win, 32));
	assert(!s->guest_actor && !s->guest_owned);
}

static bool handles_equal(struct server_world_handle x,
						  struct server_world_handle y) {
	return x.world_index == y.world_index && x.generation == y.generation;
}

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* a = &s->worlds[0];
	struct server_world* b = make_nether(s);
	load(a, 0, 0);
	load(b, 0, 0);
	assert(a->generation && b->generation && a->generation != b->generation);
	s->player.finished_loading = true;
	clin_test_set_rpc_observer(observe, NULL);

	/* Same cell, two worlds, different loot. */
	put(a, BLOCK_CHEST);
	put(b, BLOCK_CHEST);
	struct te_store sa = server_world_tile_store(a);
	struct te_store sb = server_world_tile_store(b);
	int ca = te_store_claim(&sa, TE_KIND_CHEST, TX, TY, TZ, NULL);
	int cb = te_store_claim(&sb, TE_KIND_CHEST, TX, TY, TZ, NULL);
	assert(ca >= 0 && cb >= 0 && ca != cb);
	memset(s->chest_items[ca], 0, sizeof(s->chest_items[ca]));
	memset(s->chest_items[cb], 0, sizeof(s->chest_items[cb]));
	s->chest_items[ca][0] = (struct item_data) {.id = ITEM_DIAMOND, .count = 1};
	s->chest_items[cb][0] = (struct item_data) {.id = ITEM_COAL, .count = 33};

	/* A fresh guest opens world 0's chest (live, bound open path) with a
	 * private cursor and a base crafting stack, then checkpoints. */
	struct server_rpc_source src;
	assert(server_local_guest_create(s, &src));
	struct server_player* g = s->guest_actor;
	assert(g && g->dimension == s->player.dimension);
	/* Main storage first: an open window owns the live copy of it. */
	g->inventory.items[INVENTORY_SLOT_MAIN]
		= (struct item_data) {.id = BLOCK_DIRT, .count = 13};
	assert(server_local_open_inventory_in(s, a, g, &inventory_logic_chest,
		CHEST_SIZE, TX, TY, TZ, WINDOWC_CHEST, WINDOW_TYPE_CHEST));
	assert(g->active_inventory != &g->inventory);
	g->active_inventory->picked_item
		= (struct item_data) {.id = ITEM_STICK, .count = 3};
	g->inventory.items[INVENTORY_SLOT_CRAFTING]
		= (struct item_data) {.id = BLOCK_LOG, .count = 2};
	assert(server_local_guest_checkpoint(s));
	assert(level_archive_save(&s->level));
	unsigned char priv[75], win[32];
	record_bytes(s, "CaveWiiPrivate", priv, sizeof(priv));
	record_bytes(s, "CaveWiiWindow", win, sizeof(win));
	crash_guest(s);
	assert_record_intact(s, priv, win);

	/* ---- 2: no live committed world -> refused, losslessly ------------- */
	a->chunks_ready = false;
	assert(!server_local_guest_create(s, &src)
		   && "a restored window needs a live committed world to bind to");
	a->chunks_ready = true;
	assert_record_intact(s, priv, win);
	assert(s->chest_items[ca][0].id == ITEM_DIAMOND
		   && s->chest_items[cb][0].count == 33);
	puts("PASS NULL committed world: restore refused, record intact");

	/* ---- 3: create failure (backing gone) -> refused, nothing lost ------ */
	put(a, BLOCK_AIR);
	assert(!server_local_guest_create(s, &src));
	put(a, BLOCK_CHEST);
	assert_record_intact(s, priv, win);
	assert(server_local_chest_row_w(a, TX, TY, TZ) == ca
		   && server_local_chest_row_w(b, TX, TY, TZ) == cb);
	assert(s->chest_items[ca][0].id == ITEM_DIAMOND
		   && s->chest_items[cb][0].id == ITEM_COAL);
	puts("PASS create failure: refused, record + both chest rows intact");

	/* ---- 1: restore/rejoin binds the guest's committed world ----------- */
	assert(server_local_guest_create(s, &src));
	g = s->guest_actor;
	assert(g);
	struct inventory* inv = g->active_inventory;
	assert(inv != &g->inventory && inv->logic == &inventory_logic_chest);
	assert(handles_equal(g->window_world, server_world_handle_of(a))
		   && "restored window must be bound to the guest's committed world");
	assert(server_local_world_of(s, g) == a);
	assert(server_local_window_world(inv) == a);
	assert(!server_local_window_bound_to(inv, b));
	/* World 0's chest, never world 1's same-cell chest. */
	assert(inv->items[CHEST_SLOT_STORAGE].id == ITEM_DIAMOND
		   && inv->items[CHEST_SLOT_STORAGE].count == 1);
	assert(inventory_chest_view_at_w(s, a, TX, TY, TZ) == inv);
	assert(inventory_chest_view_at_w(s, b, TX, TY, TZ) != inv);
	/* Private state restored with it: nothing lost across the refusals. */
	assert(inv->picked_item.id == ITEM_STICK && inv->picked_item.count == 3);
	assert(g->inventory.items[INVENTORY_SLOT_CRAFTING].id == BLOCK_LOG
		   && g->inventory.items[INVENTORY_SLOT_CRAFTING].count == 2);
	assert(g->inventory.items[INVENTORY_SLOT_MAIN].count == 13);
	/* The binding is real, not the slot-0 fallback: a stale generation of
	 * the bound world leaves this window backing-less (an unbound window
	 * would still resolve to slot 0). */
	uint32_t gen = a->generation;
	a->generation = gen + 1;
	assert(!server_local_window_world(inv)
		   && "a stale bound world must not fall back to slot 0");
	a->generation = gen;
	assert(server_local_window_world(inv) == a);
	puts("PASS restore/rejoin: window bound to the guest's world, same-cell "
		 "chest resolves to world 0, stale generation fails closed");
	crash_guest(s);

	/* ---- 4: restore step keeps/restores the prior binding -------------- */
	struct server_player probe = {0};
	assert(inventory_create(&probe.inventory, &inventory_logic_player, s,
							&probe, INVENTORY_SIZE, 0, 0, 0));
	probe.inventory.reply = noop_reply;
	probe.active_inventory = &probe.inventory;
	probe.has_pos = true;
	probe.health = MAX_PLAYER_HEALTH;
	probe.dimension = s->player.dimension;
	struct server_world_handle prior = server_world_handle_of(b);
	probe.window_world = prior;
	int32_t rec[8] = {WINDOWC_CHEST, WINDOW_TYPE_CHEST, TX + 7, TY, TZ, 0, 0, 0};
	struct inventory* out = NULL;
	assert(!server_local_guest_test_restore_window(s, &probe, rec, &out));
	assert(!out && handles_equal(probe.window_world, prior)
		   && probe.active_inventory == &probe.inventory);
	rec[2] = TX;
	a->chunks_ready = false; /* no committed world: refused, binding kept */
	assert(!server_local_guest_test_restore_window(s, &probe, rec, &out));
	a->chunks_ready = true;
	assert(!out && handles_equal(probe.window_world, prior));
	assert(server_local_guest_test_restore_window(s, &probe, rec, &out));
	assert(out && handles_equal(probe.window_world, server_world_handle_of(a)));
	assert(server_local_window_world(out) == a
		   && out->items[CHEST_SLOT_STORAGE].id == ITEM_DIAMOND);
	out->reply = noop_reply;
	inventory_destroy_owned(out, &probe);
	inventory_destroy(&probe.inventory, &probe);
	puts("PASS restore step: create failure restores the prior binding, "
		 "success binds the committed world");

	clin_test_set_rpc_observer(NULL, NULL);
	server_world_discard(b);
	clin_drain_discard();
	split_destroy(s);
	puts("C1 guest window bind: all checks passed");
	return 0;
}
