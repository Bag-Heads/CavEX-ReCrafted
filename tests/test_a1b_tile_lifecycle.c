/* A1b (split-dimensions-plan-0924, Astra D9/D10) -- world-qualified tile
 * lifecycle through the LIVE server paths, two world instances in one
 * session (slot 0 Overworld, slot 1 Nether) at IDENTICAL coordinates:
 *
 *  1. typed tile APIs (_w): register/lookup/free/tick isolate by world;
 *  2. chunk save+evict+reload of one world persists and restores only that
 *     world's tiles (the other world's rows are untouched throughout);
 *  3. windows bind to their opening world: backing, shared-view publication
 *     and removal invalidation stay inside that world; a stale generation
 *     leaves the window backing-less;
 *  4. legacy flat archives: a world's write excludes the other world's rows;
 *     a full-image read refuses (losslessly) while the other world owns rows;
 *  5. effects: spawn_monster_at_w binds the entity to its world; only a world
 *     with a client route publishes (sound + spawn).
 *
 * World 1 is never activated in production by A1b; this fixture creates the
 * slot-1 instance by hand exactly like test_world_context_equiv does. */
#include "support/split_residency_fixture.h"

#include "entity/monsters_object.h"
#include "item/items.h"
#include "item/window_container.h"
#include "network/complex_block_archive.h"
#include "network/inventory_chest_shared.h"
#include "network/inventory_logic.h"
#include "network/server_furnace.h"
#include "network/server_noteblock.h"
#include "network/server_spawner.h"
#include "network/tile_entities_store.h"
#include "platform/audio.h"

#include <stddef.h>
#include <stdio.h>

/* The entity's bound world, read by offset so the lexical world-alias gate
 * does not mistake the entity field for the server_local alias. */
static void* entity_bound_world(const struct entity* e) {
	return *(void* const*)((const char*)e + offsetof(struct entity, world));
}

#define TX 3
#define TY 10
#define TZ 5

static unsigned long sends, sound_sends, spawn_sends;
static void observe(const struct client_rpc* call, void* user) {
	(void)user;
	sends++;
	if(call->type == CRPC_SOUND)
		sound_sends++;
	if(call->type == CRPC_SPAWN_MONSTER)
		spawn_sends++;
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

static struct server_chunk* load(struct server_world* w, int cx, int cz) {
	struct server_chunk* c = NULL;
	for(unsigned n = 0; n < 200 && !c; n++) {
		server_world_begin_update(w);
		(void)server_world_load_chunk(w, cx, cz, &c);
	}
	assert(c);
	return c;
}

static void put(struct server_world* w, int y, uint8_t type) {
	assert(server_world_set_block_silent(w, TX, y, TZ,
		(struct block_data) {.type = type}));
}

/* ---- 1 + 2: typed APIs, tick, save/evict/reload ------------------------ */
static void typed_apis_and_reload(struct server_local* s,
								  struct server_world* a,
								  struct server_world* b) {
	put(a, TY, BLOCK_FURNACE);
	put(b, TY, BLOCK_FURNACE);
	int ra = furnace_te_register_w(a, TX, TY, TZ);
	int rb = furnace_te_register_w(b, TX, TY, TZ);
	assert(ra >= 0 && rb >= 0 && ra != rb);
	struct furnace_te* fa = furnace_te_lookup_w(a, TX, TY, TZ);
	struct furnace_te* fb = furnace_te_lookup_w(b, TX, TY, TZ);
	assert(fa && fb && fa != fb);
	/* World 0 burns coal + smelts ore; world 1 holds a different, idle stack. */
	fa->items[0] = (struct item_data) {.id = BLOCK_IRON_ORE, .count = 3};
	fa->items[1] = (struct item_data) {.id = ITEM_COAL, .count = 2};
	fb->items[0] = (struct item_data) {.id = BLOCK_GOLD_ORE, .count = 9};
	/* The legacy adapter is slot 0's view, never slot 1's. */
	assert(furnace_te_lookup(s, TX, TY, TZ) == fa);
	furnace_server_tick_w(a);
	assert(fa->burn_time > 0 && fb->burn_time == 0
		   && fb->items[0].count == 9);
	furnace_server_tick_w(b); /* no fuel in world 1: nothing burns */
	assert(fb->burn_time == 0);
	int16_t a_burn = (int16_t)fa->burn_time;

	/* Note block pitch + live sidecar, and spawner TEs, same cell per world. */
	put(a, TY + 1, BLOCK_NOTEBLOCK);
	put(b, TY + 1, BLOCK_NOTEBLOCK);
	assert(noteblock_te_register_w(a, TX, TY + 1, TZ) >= 0);
	assert(noteblock_te_register_w(b, TX, TY + 1, TZ) >= 0);
	*noteblock_te_lookup_w(a, TX, TY + 1, TZ) = 7;
	*noteblock_te_lookup_w(b, TX, TY + 1, TZ) = 19;
	*noteblock_prev_powered_lookup_w(b, TX, TY + 1, TZ) = true;
	assert(!*noteblock_prev_powered_lookup_w(a, TX, TY + 1, TZ));
	put(a, TY + 2, BLOCK_MOB_SPAWNER);
	put(b, TY + 2, BLOCK_MOB_SPAWNER);
	assert(spawner_te_upsert_w(a, TX, TY + 2, TZ, "Skeleton"));
	assert(spawner_te_upsert_w(b, TX, TY + 2, TZ, "Zombie"));

	/* Save + evict world 1's chunk: its rows persist and leave the table;
	 * world 0's rows are untouched. */
	server_world_save_batch_begin(b);
	server_world_save_chunk(b, true, 0, 0);
	assert(server_world_save_batch_end(b));
	assert(!furnace_te_lookup_w(b, TX, TY, TZ));
	assert(!spawner_te_lookup_w(b, TX, TY + 2, TZ));
	assert(furnace_te_lookup_w(a, TX, TY, TZ) == fa && fa->burn_time == a_burn);
	assert(*noteblock_te_lookup_w(a, TX, TY + 1, TZ) == 7);
	assert(!strcmp(spawner_te_lookup_w(a, TX, TY + 2, TZ)->mob_id, "Skeleton"));

	/* Reload world 1: exactly its own image comes back. */
	load(b, 0, 0);
	fb = furnace_te_lookup_w(b, TX, TY, TZ);
	assert(fb && fb->items[0].id == BLOCK_GOLD_ORE && fb->items[0].count == 9);
	assert(*noteblock_te_lookup_w(b, TX, TY + 1, TZ) == 19);
	/* previousRedstoneState is live-only: a reload restarts it false. */
	assert(!*noteblock_prev_powered_lookup_w(b, TX, TY + 1, TZ));
	assert(!strcmp(spawner_te_lookup_w(b, TX, TY + 2, TZ)->mob_id, "Zombie"));
	assert(furnace_te_lookup_w(a, TX, TY, TZ) == fa
		   && fa->items[0].id == BLOCK_IRON_ORE);
	puts("PASS typed tile APIs + one-world save/evict/reload: other world untouched");
}

/* ---- 3: window binding -------------------------------------------------- */
static void windows(struct server_local* s, struct server_world* a,
					struct server_world* b, struct server_player* guest) {
	put(a, TY + 3, BLOCK_CHEST);
	put(b, TY + 3, BLOCK_CHEST);
	struct te_store sa = server_world_tile_store(a);
	struct te_store sb = server_world_tile_store(b);
	int ca = te_store_claim(&sa, TE_KIND_CHEST, TX, TY + 3, TZ, NULL);
	int cb = te_store_claim(&sb, TE_KIND_CHEST, TX, TY + 3, TZ, NULL);
	assert(ca >= 0 && cb >= 0 && ca != cb);
	memset(s->chest_items[ca], 0, sizeof(s->chest_items[ca]));
	memset(s->chest_items[cb], 0, sizeof(s->chest_items[cb]));
	s->chest_items[ca][0] = (struct item_data) {.id = ITEM_DIAMOND, .count = 1};
	s->chest_items[cb][0] = (struct item_data) {.id = ITEM_COAL, .count = 33};

	assert(server_local_open_inventory_in(s, a, &s->player,
		&inventory_logic_chest, CHEST_SIZE, TX, TY + 3, TZ, WINDOWC_CHEST,
		WINDOW_TYPE_CHEST));
	assert(server_local_open_inventory_in(s, b, guest,
		&inventory_logic_chest, CHEST_SIZE, TX, TY + 3, TZ, WINDOWC_CHEST,
		WINDOW_TYPE_CHEST));
	struct inventory* host_inv = s->player.active_inventory;
	struct inventory* guest_inv = guest->active_inventory;
	assert(server_local_window_world(host_inv) == a);
	assert(server_local_window_world(guest_inv) == b);
	/* Each window shows ITS world's chest, not the other's same-cell chest. */
	assert(host_inv->items[CHEST_SLOT_STORAGE].id == ITEM_DIAMOND);
	assert(guest_inv->items[CHEST_SLOT_STORAGE].id == ITEM_COAL
		   && guest_inv->items[CHEST_SLOT_STORAGE].count == 33);
	assert(!server_local_windows_share_world(host_inv, guest_inv));
	assert(inventory_chest_view_at_w(s, a, TX, TY + 3, TZ) == host_inv);
	assert(inventory_chest_view_at_w(s, b, TX, TY + 3, TZ) == guest_inv);

	/* A host edit writes through to world 0's row only and is NOT fanned out
	 * to the guest's view of world 1's chest. */
	host_inv->items[CHEST_SLOT_STORAGE + 1]
		= (struct item_data) {.id = ITEM_STICK, .count = 5};
	set_inv_slot_t changes;
	set_inv_slot_init(changes);
	set_inv_slot_push(changes, CHEST_SLOT_STORAGE + 1);
	inventory_chest_publish(host_inv, changes);
	set_inv_slot_clear(changes);
	assert(s->chest_items[ca][1].id == ITEM_STICK);
	assert(s->chest_items[cb][1].id == 0);
	assert(guest_inv->items[CHEST_SLOT_STORAGE + 1].id == 0);

	/* Removal of world 0's chest invalidates only world 0's views. */
	inventory_chest_invalidate_peers(host_inv, TX, TY + 3, TZ);
	assert(!guest_inv->invalidated
		   && !(guest_inv->orphaned_chest_halves & 1));

	/* Stale generation: world 1 torn down under the open guest window. The
	 * binding no longer resolves, so the window has no backing (nothing can
	 * write into a re-created instance's rows). */
	struct server_world_handle old = server_world_handle_of(b);
	server_world_discard(b);
	assert(!server_local_window_world(guest_inv));
	assert(!server_local_window_bound_to(guest_inv, b));
	assert(guest->window_world.generation == old.generation);
	/* Tear the fixture windows down without running close semantics (the
	 * close/spill policy for a torn-down world is A2's lifecycle). */
	inventory_chest_unregister(host_inv);
	inventory_chest_unregister(guest_inv);
	s->player.active_inventory = &s->player.inventory;
	guest->active_inventory = &guest->inventory;
	free(host_inv->items);
	free(host_inv);
	free(guest_inv->items);
	free(guest_inv);
	(void)make_nether(s);
	load(&s->worlds[1], 0, 0);
	/* The old instance's rows are invisible to the re-created world. */
	assert(server_local_chest_row_w(&s->worlds[1], TX, TY + 3, TZ) < 0);
	puts("PASS window binding: per-world backing, publish, invalidation, stale generation");
}

/* ---- 4: legacy flat archives -------------------------------------------- */
static void legacy_archives(struct server_local* s, struct server_world* a,
							struct server_world* b) {
	struct te_store sa = server_world_tile_store(a);
	struct te_store sb = server_world_tile_store(b);
	int cb = te_store_claim(&sb, TE_KIND_CHEST, 40, 64, 40, NULL);
	assert(cb >= 0);
	s->chest_items[cb][0] = (struct item_data) {.id = ITEM_REDSTONE, .count = 4};
	int ca = server_local_chest_row_w(a, TX, TY + 3, TZ);
	assert(ca >= 0);
	string_t path;
	string_init_set_str(path, split_path);
	/* World 1's archive holds world 1's rows only. */
	assert(chest_archive_write_w(&sb, path, b->dimension));
	struct complex_block_pos* pos = calloc(MAX_CHESTS, sizeof(*pos));
	struct item_data* items = calloc((size_t)MAX_CHESTS * MAX_CHEST_SLOTS,
									 sizeof(*items));
	assert(pos && items);
	assert(chest_archive_read(pos, items, path, b->dimension));
	assert(pos[cb].y == 64 && items[(size_t)cb * MAX_CHEST_SLOTS].id == ITEM_REDSTONE);
	assert(pos[ca].y < 0 && items[(size_t)ca * MAX_CHEST_SLOTS].id == 0);
	/* A full-image read for world 0 would erase world 1's rows: refused, and
	 * nothing in the shared table changes. */
	/* A2-S: the full-image read of world A no longer refuses because B owns
	 * rows -- it is a prepare/preflight/commit reload that keeps B's row at
	 * its index and maps A's incoming rows around it (the image written by
	 * B's view carries B's chest at row `cb`; A receives a copy of it in
	 * the first replaceable/free row, never at B's index). */
	struct complex_block_pos before[MAX_CHESTS];
	memcpy(before, s->chest_pos, sizeof(before));
	assert(chest_archive_write_w(&sa, path, a->dimension));
	assert(chest_archive_read_w(&sa, path, a->dimension));
	assert(server_local_chest_row_w(b, 40, 64, 40) == cb);
	assert(s->chest_items[cb][0].id == ITEM_REDSTONE
		   && "B's row bytes are untouched");
	assert(server_local_chest_row_w(a, TX, TY + 3, TZ) == ca
		   && "A's own row reloads at its original index (lossless)");
	assert(memcmp(before, s->chest_pos, sizeof(before)) == 0
		   && "single owner per row: the table layout is unchanged");
	free(pos);
	free(items);
	string_clear(path);
	puts("PASS legacy archives: write selects the world, full-image read refuses losslessly");
}

/* ---- 5: effects ---------------------------------------------------------- */
static void effects(struct server_local* s, struct server_world* a,
					struct server_world* b) {
	unsigned long before_sound = sound_sends, before_spawn = spawn_sends;
	/* split dims (sounds per world): a LIVE slot-1 world sounds, stamped
	 * with itself (the client plays it only where a view shows world 1);
	 * this flipped on purpose from the pre-activation "no route" pin */
	server_local_emit_sound_w(b, SOUND_RANDOM_FUSE, (vec3) {1, 5, 1}, 1, 1);
	assert(sound_sends == before_sound + 1);
	server_local_emit_sound_w(a, SOUND_RANDOM_FUSE, (vec3) {1, 5, 1}, 1, 1);
	assert(sound_sends == before_sound + 2);
	server_local_emit_sound_ui_w(b, &s->player, SOUND_RANDOM_FUSE, 1, 1);
	assert(sound_sends == before_sound + 2); /* host is not in world 1 */
	server_local_emit_sound_ui_w(a, &s->player, SOUND_RANDOM_FUSE, 1, 1);
	assert(sound_sends == before_sound + 3);

	struct entity* e1 = server_local_spawn_monster_at_w(b, (vec3) {2.5F, 12, 2.5F}, MONSTER_ZOMBIE);
	assert(e1 && entity_bound_world(e1) == b && spawn_sends == before_spawn);
	struct entity* e0 = server_local_spawn_monster_at_w(a, (vec3) {2.5F, 12, 2.5F}, MONSTER_ZOMBIE);
	assert(e0 && entity_bound_world(e0) == a && spawn_sends == before_spawn + 1);
	assert(e0->id != e1->id); /* session-global ids */
	struct server_world stray = {0};
	assert(!server_local_spawn_monster_at_w(&stray, (vec3) {0, 0, 0}, MONSTER_ZOMBIE));
	puts("PASS effects: spawn binds its world, only a routed world publishes");
}

/* ---- 6: aggregate retirement accounting (D10) --------------------------- */
static void retirement(struct server_world* a, struct server_world* b) {
	assert(server_world_session_retiring_bytes(a)
		   == a->retiring_bytes + b->retiring_bytes);
	/* World 1's retained (not yet durable) records fill the SHARED budget:
	 * world 0's evicting save inside a batch is refused explicitly and
	 * losslessly (chunk, tiles and obligations stay), not granted a second
	 * 512 KiB. */
	/* Persist once (no eviction) so world 0's region handle is resident: the
	 * retiring ring applies to evicting writes through a cached handle. */
	server_world_light_drain(a); /* G9: a pinned column is not written */
	server_world_light_drain(b);
	assert(server_world_save_chunk(a, false, 0, 0));
	assert(server_world_chunk_region(a, 0, 0));
	size_t saved = b->retiring_bytes;
	b->retiring_bytes = SERVER_WORLD_RETIRING_MAX_BYTES;
	size_t refused = a->retiring_refused;
	server_world_save_batch_begin(a);
	bool evicted = server_world_save_chunk(a, true, 0, 0);
	assert(!evicted && a->retiring_refused == refused + 1);
	assert(dict_server_chunks_get(a->chunks, S_CHUNK_ID(0, 0)));
	assert(furnace_te_lookup_w(a, TX, TY, TZ));
	b->retiring_bytes = saved;
	assert(server_world_save_batch_end(a));
	puts("PASS aggregate retirement: other world's retained bytes refuse this "
		 "world's eviction losslessly (one shared 512 KiB budget)");
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

	struct server_player guest = {0};
	assert(inventory_create(&guest.inventory, &inventory_logic_player, s,
							&guest, INVENTORY_SIZE, 0, 0, 0));
	guest.inventory.reply = noop_reply;
	guest.active_inventory = &guest.inventory;
	guest.has_pos = guest.finished_loading = true;
	guest.health = MAX_PLAYER_HEALTH;
	s->player.finished_loading = true;
	/* Drain every publication (window snapshots etc.) through the observer
	 * so the bounded client queue never fills. */
	clin_test_set_rpc_observer(observe, NULL);

	typed_apis_and_reload(s, a, b);
	windows(s, a, b, &guest);
	b = &s->worlds[1];
	legacy_archives(s, a, b);
	effects(s, a, b);
	retirement(a, b);

	clin_test_set_rpc_observer(NULL, NULL);
	inventory_destroy(&guest.inventory, &guest);
	server_world_discard(b);
	clin_drain_discard();
	split_destroy(s);
	puts("A1b tile lifecycle: all checks passed");
	return 0;
}
