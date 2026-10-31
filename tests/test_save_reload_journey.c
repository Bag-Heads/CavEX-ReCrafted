/* Copyright (c) 2026 CaveWii_v2 contributors
 * SPEC R12 "Save and reload keep your stuff, including after an interrupted
 * save": a scripted journey played through the production world open
 * (server_local_load_world_impl) and save-and-quit (server_local_unload_world_
 * impl), reopened by a FRESH PROCESS, and the same save killed at every traced
 * persistence transition.
 *
 * The journey (real worldgen, seed 4242; no input layer -- the controller
 * journey on hardware is REL-11 / survival_journey_30m):
 *   gen 1: mine 6 natural blocks at home (drops go to the inventory and two
 *          stay on the ground as item entities), place a cobblestone pillar,
 *          place a chest and fill it, place a furnace with iron ore + coal and
 *          SMELT for real (furnace_server_tick_w, 450 ticks: 2 ingots + a
 *          partial cook), armour + hotbar + main inventory, a sheep, health 13,
 *          then travel 640 blocks east (a second region file, freshly
 *          generated chunks) and place a marker there. Save and quit.
 *   gen 2: reopen in the same process, change the chest, smelt 150 more
 *          ticks, mine 3 more blocks at the far site, swap armour, walk back
 *          30 blocks, health 9. Save and quit (traced).
 *
 * Cases (argv[1]):
 *   journey  -- census child plays gen 1 + gen 2 and records every traced
 *               transition of the gen-2 save; a fresh verify process reopens
 *               the gen-1 and gen-2 images and must recover the in-memory
 *               snapshot EXACTLY: player inventory (armour included),
 *               position, health, dimension; chest and furnace contents and
 *               timers; every mined/placed cell; per-chunk block hash and
 *               item/mob entity counts. The verify child then saves again and
 *               reopens again (idempotent).
 *   powercut -- one child per traced gen-2 transition is KILLED right after
 *               it (TerminateProcess / SIGKILL: no flush, no atexit). On the
 *               killed image: every region record is byte-identical to its
 *               gen-1 or its gen-2 record (never a third/mixed record inside
 *               one chunk; a chunk first written in gen 2 may be absent);
 *               no dangling index; a fresh process opens the world through
 *               the production loader, the player record is wholly gen 1 or
 *               wholly gen 2 (never lost), every chunk's blocks + TE + entity
 *               counts match ONE generation, and a subsequent save succeeds
 *               and reopens to the same state.
 *
 * Cross-FILE atomicity is NOT promised (level.dat publishes before the
 * regions; see test_g9_save_io_durability "powercut, part 2"): a level.dat
 * from gen 2 with chunks from gen 1 is a recorded outcome, counted below. A
 * process kill keeps what reached the OS; fsync ordering under power loss is
 * pinned by test_g9_save_io_durability powercut part 1. */
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define JR_MKDIR(p) _mkdir(p)
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#define JR_MKDIR(p) mkdir(p, 0777)
#endif
#include <m-lib/m-string.h>

#include "support/g9_save_io_rig.h"
#include "support/test_fixture_dir.h"

#include "block/blocks.h"
#include "block/furnace_logic.h"
#include "entity/combat.h" // PLAYER_EYE_HEIGHT
#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/entity_archive.h"
#include "network/level_archive.h"
#include "network/region_archive.h"
#include "network/server_furnace.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/tile_entities_store.h"
#include "platform/thread.h"

#define JR_SEED 4242
#define JR_RAND INT64_C(0x5EEDBEEF)
#define JR_KILL_RC 137
#define JR_MAX_OPS 512
#define JR_MAX_CHUNKS 32
#define JR_MAX_CELLS 32
#define JR_FAR_X 640
#define CLOCK_STAMP 0x5F5E1234u

static unsigned failures;
static void check(bool cond, const char* label) {
	if(!cond) {
		failures++;
		printf("  FAIL: %s\n", label);
	}
}

static struct server_local S;
static const char* s_self;

/* ---- snapshot -------------------------------------------------------- */
struct jr_cell {
	int x, y, z;
	uint8_t type, meta;
};
struct jr_chunk {
	int cx, cz;
	uint64_t blocks; /* FNV-1a-64 of ids + metadata */
	uint64_t te;	 /* FNV of chest + furnace rows owned by this chunk */
	unsigned items, mobs;
};
struct jr_snap {
	uint32_t magic;
	struct item_data inv[INVENTORY_SIZE];
	double x, y, z;
	int16_t health;
	int32_t dim;
	int chest_x, chest_y, chest_z, furn_x, furn_y, furn_z;
	struct item_data chest[MAX_CHEST_SLOTS];
	struct furnace_te furnace;
	unsigned ncells;
	struct jr_cell cells[JR_MAX_CELLS];
	unsigned nchunks;
	struct jr_chunk chunks[JR_MAX_CHUNKS];
};
#define JR_MAGIC 0x4A524E59u

static uint64_t fnv(uint64_t h, const void* p, size_t n) {
	const uint8_t* b = p;
	for(size_t k = 0; k < n; k++)
		h = (h ^ b[k]) * UINT64_C(0x100000001B3);
	return h;
}
#define FNV0 UINT64_C(0xCBF29CE484222325)

static bool item_eq(struct item_data a, struct item_data b) {
	if(!a.count || !a.id)
		return !b.count || !b.id;
	return a.id == b.id && a.count == b.count && a.durability == b.durability;
}
static uint64_t item_hash(uint64_t h, struct item_data it) {
	uint16_t v[3] = {0, 0, 0};
	if(it.count && it.id) {
		v[0] = it.id;
		v[1] = it.durability;
		v[2] = it.count;
	}
	return fnv(h, v, sizeof v);
}

/* The home and far chunk sets the journey touches (and loads). */
static const int HOME[][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {-1, 0}, {0, -1}};
static const int AWAY[][2] = {{40, 0}, {41, 0}, {40, 1}, {41, 1}, {39, 0}};
#define NHOME (sizeof HOME / sizeof *HOME)
#define NAWAY (sizeof AWAY / sizeof *AWAY)

static struct server_world* W(void) { return &S.worlds[0]; }
/* Resident first: the loader is for absent columns. */
static struct server_chunk* chunk_at(int cx, int cz) {
	struct server_chunk* sc = dict_server_chunks_get(W()->chunks, S_CHUNK_ID(cx, cz));
	if(!sc) {
		bool ok = server_world_load_chunk(W(), cx, cz, &sc);
		assert(ok && sc);
	}
	return sc;
}
static void load_chunk(int cx, int cz) { (void)chunk_at(cx, cz); }
/* Each tracked chunk with its full 3x3 ring: every populate quadrant that
 * can write into a tracked chunk runs before the first snapshot, so a later
 * load can never decorate it further (that would be Beta population, not a
 * save defect). */
static void load_all(void) {
	for(size_t k = 0; k < NHOME + NAWAY; k++) {
		int cx = k < NHOME ? HOME[k][0] : AWAY[k - NHOME][0];
		int cz = k < NHOME ? HOME[k][1] : AWAY[k - NHOME][1];
		for(int dz = -1; dz <= 1; dz++)
			for(int dx = -1; dx <= 1; dx++)
				load_chunk(cx + dx, cz + dz);
	}
}
static struct block_data get(int x, int y, int z) {
	struct block_data b = {0};
	assert(server_world_get_block(W(), x, y, z, &b));
	return b;
}
static void put(int x, int y, int z, int type, int meta) {
	struct block_data now = get(x, y, z);
	if(now.type == type && now.metadata == meta)
		return;
	assert(server_world_set_block(
		W(), x, y, z,
		(struct block_data) {.type = (uint8_t)type, .metadata = (uint8_t)meta}));
}
static int surface(int x, int z) {
	for(int y = WORLD_HEIGHT - 2; y > 1; y--) {
		uint8_t t = get(x, y, z).type;
		if(t != BLOCK_AIR && t != BLOCK_LEAVES && t != BLOCK_LOG
		   && t != BLOCK_WATER_FLOW && t != BLOCK_WATER_STILL)
			return y;
	}
	return 64;
}
static struct item_data I(uint16_t id, unsigned count, uint16_t dur) {
	return (struct item_data) {.id = id, .count = (uint8_t)count, .durability = dur};
}
static void give(struct item_data it) {
	for(int k = INVENTORY_SLOT_MAIN; k < INVENTORY_SIZE; k++) {
		struct item_data* s = &S.player.inventory.items[k];
		if(s->count && s->id == it.id && s->durability == it.durability
		   && s->count + it.count <= 64) {
			s->count += it.count;
			return;
		}
	}
	for(int k = INVENTORY_SLOT_MAIN; k < INVENTORY_SIZE; k++)
		if(!S.player.inventory.items[k].count) {
			S.player.inventory.items[k] = it;
			return;
		}
	assert(!"inventory full");
}
/* Mine (x,y,z): the block becomes air, its id goes to the inventory. */
static void mine(int x, int y, int z) {
	struct block_data b = get(x, y, z);
	uint16_t drop = b.type == BLOCK_GRASS ? BLOCK_DIRT
		: b.type == BLOCK_STONE			  ? BLOCK_COBBLESTONE
										  : b.type;
	put(x, y, z, BLOCK_AIR, 0);
	if(drop != BLOCK_AIR)
		give(I(drop, 1, 0));
}
static void add_entity_item(struct item_data it, double x, double y, double z) {
	uint32_t id = server_local_gen_id(&S);
	struct entity* e = malloc(sizeof *e);
	assert(e);
	entity_item(id, e, true, W(), it);
	e->teleport(e, (vec3) {(float)x, (float)y, (float)z});
	glm_vec3_zero(e->vel);
	*dict_entity_safe_get(S.entities, id) = e;
}
static void add_sheep(double x, double y, double z) {
	uint32_t id = server_local_gen_id(&S);
	struct entity* e = malloc(sizeof *e);
	assert(e);
	entity_monster(id, e, true, W(), MONSTER_SHEEP);
	e->teleport(e, (vec3) {(float)x, (float)y, (float)z});
	e->data.monster.fleece_color = 6;
	*dict_entity_safe_get(S.entities, id) = e;
}

static void take_snap(struct jr_snap* o, const struct jr_snap* cells_from) {
	memset(o, 0, sizeof *o);
	o->magic = JR_MAGIC;
	for(int k = 0; k < INVENTORY_SIZE; k++)
		o->inv[k] = S.player.inventory.items[k];
	o->x = S.player.x;
	o->y = S.player.y;
	o->z = S.player.z;
	o->health = S.player.health;
	o->dim = (int32_t)S.player.dimension;
	o->chest_x = cells_from->chest_x;
	o->chest_y = cells_from->chest_y;
	o->chest_z = cells_from->chest_z;
	o->furn_x = cells_from->furn_x;
	o->furn_y = cells_from->furn_y;
	o->furn_z = cells_from->furn_z;
	int row = server_local_chest_row_w(W(), o->chest_x, o->chest_y, o->chest_z);
	if(row >= 0)
		for(int k = 0; k < MAX_CHEST_SLOTS; k++)
			o->chest[k] = S.chest_items[row][k];
	struct furnace_te* fu
		= furnace_te_lookup_w(W(), o->furn_x, o->furn_y, o->furn_z);
	if(fu)
		o->furnace = *fu;
	else
		o->furnace.burn_time = -1;
	o->ncells = cells_from->ncells;
	for(unsigned k = 0; k < o->ncells; k++) {
		o->cells[k] = cells_from->cells[k];
		struct block_data b = get(o->cells[k].x, o->cells[k].y, o->cells[k].z);
		o->cells[k].type = b.type;
		o->cells[k].meta = b.metadata;
	}
	for(size_t k = 0; k < NHOME + NAWAY; k++) {
		int cx = k < NHOME ? HOME[k][0] : AWAY[k - NHOME][0];
		int cz = k < NHOME ? HOME[k][1] : AWAY[k - NHOME][1];
		struct server_chunk* sc = chunk_at(cx, cz);
		struct jr_chunk* c = &o->chunks[o->nchunks++];
		c->cx = cx;
		c->cz = cz;
		size_t n = CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
		c->blocks = fnv(fnv(FNV0, sc->ids, n), sc->metadata, n / 2);
		c->te = FNV0;
		int bx = cx * CHUNK_SIZE, bz = cz * CHUNK_SIZE;
		if(o->chest_x >= bx && o->chest_x < bx + CHUNK_SIZE && o->chest_z >= bz
		   && o->chest_z < bz + CHUNK_SIZE)
			for(int s = 0; s < MAX_CHEST_SLOTS; s++)
				c->te = item_hash(c->te, o->chest[s]);
		if(o->furn_x >= bx && o->furn_x < bx + CHUNK_SIZE && o->furn_z >= bz
		   && o->furn_z < bz + CHUNK_SIZE) {
			int t[3] = {o->furnace.burn_time, o->furnace.cook_time,
						o->furnace.current_item_burn_time};
			c->te = fnv(c->te, t, sizeof t);
			for(int s = 0; s < 3; s++)
				c->te = item_hash(c->te, o->furnace.items[s]);
		}
	}
	dict_entity_it_t it;
	for(dict_entity_it(it, S.entities); !dict_entity_end_p(it);
		dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(!e)
			continue;
		int cx = (int)floorf(e->pos[0] / CHUNK_SIZE);
		int cz = (int)floorf(e->pos[2] / CHUNK_SIZE);
		for(unsigned k = 0; k < o->nchunks; k++)
			if(o->chunks[k].cx == cx && o->chunks[k].cz == cz) {
				o->chunks[k].items += e->type == ENTITY_ITEM;
				o->chunks[k].mobs += e->type == ENTITY_MONSTER;
			}
	}
}
static void record_cell(struct jr_snap* c, int x, int y, int z) {
	for(unsigned k = 0; k < c->ncells; k++)
		if(c->cells[k].x == x && c->cells[k].y == y && c->cells[k].z == z)
			return;
	assert(c->ncells < JR_MAX_CELLS);
	c->cells[c->ncells++] = (struct jr_cell) {x, y, z, 0, 0};
}
static void print_snap(const char* tag, const struct jr_snap* s) {
	unsigned items = 0, mobs = 0, inv = 0, armour = 0;
	for(unsigned k = 0; k < s->nchunks; k++) {
		items += s->chunks[k].items;
		mobs += s->chunks[k].mobs;
	}
	for(int k = INVENTORY_SLOT_ARMOR; k < INVENTORY_SIZE; k++)
		if(s->inv[k].count) {
			inv++;
			armour += k < INVENTORY_SLOT_ARMOR + INVENTORY_SIZE_ARMOR;
		}
	printf("  %s: player (%.3f, %.3f, %.3f) hp %d dim %d, %u stacks (%u armour);"
		   " furnace in %ux%u fuel %ux%u out %ux%u burn %d cook %d; %u cells;"
		   " %u chunks, %u item + %u mob entities\n", tag, s->x, s->y, s->z,
		   s->health, s->dim, inv, armour, s->furnace.items[0].id,
		   s->furnace.items[0].count, s->furnace.items[1].id,
		   s->furnace.items[1].count, s->furnace.items[2].id,
		   s->furnace.items[2].count, s->furnace.burn_time,
		   s->furnace.cook_time, s->ncells, s->nchunks, items, mobs);
}
static bool write_snap(const char* path, const struct jr_snap* s) {
	FILE* f = fopen(path, "wb");
	if(!f)
		return false;
	bool ok = fwrite(s, sizeof *s, 1, f) == 1;
	return !fclose(f) && ok;
}
static bool read_snap(const char* path, struct jr_snap* s) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	bool ok = fread(s, sizeof *s, 1, f) == 1 && s->magic == JR_MAGIC;
	fclose(f);
	return ok;
}

/* ---- production open / save-and-quit ---------------------------------- */
static void* drain_entry(void* arg) {
	(void)arg;
	for(;;) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}
static void boot(void) {
	assert(clin_init() && svin_init());
	static struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	region_archive_test_clock(CLOCK_STAMP);
	server_local_test_set_last_played(INT64_C(1791532400000));
	server_local_test_create_without_thread(&S, false, false, false, false,
											false, false, false);
	S.paused = true;
}
static bool open_world(const char* root) {
	string_t lvl;
	string_init_set_str(lvl, root);
	server_world_test_seed_runtime_jrand(&S.worlds[0].rand, JR_RAND);
	bool ok = server_local_load_world_impl(&S, lvl);
	string_clear(lvl);
	if(!ok)
		return false;
	server_local_test_finish_load_inventory(&S);
	S.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	S.player.finished_loading = true;
	load_all();
	return true;
}
static bool quit_world(void) { return server_local_unload_world_impl(&S, true); }

/* ---- the journey ------------------------------------------------------ */
static struct jr_snap s_cells; /* cell list + TE coordinates (positions only) */

static void play_gen1(void) {
	struct jr_snap* c = &s_cells;
	memset(c, 0, sizeof *c);
	S.player.health = MAX_PLAYER_HEALTH;
	for(int k = 0; k < INVENTORY_SIZE; k++)
		S.player.inventory.items[k] = I(0, 0, 0);
	/* Mine a 3x2 trench at home. */
	for(int dx = 0; dx < 3; dx++) {
		int x = 4 + dx, z = 4;
		int y = surface(x, z);
		mine(x, y, z);
		mine(x, y - 1, z);
		record_cell(c, x, y, z);
		record_cell(c, x, y - 1, z);
	}
	/* Place a 3-high cobblestone pillar from the mined stock. */
	int py = surface(9, 9);
	for(int k = 1; k <= 3; k++) {
		put(9, py + k, 9, BLOCK_COBBLESTONE, 0);
		record_cell(c, 9, py + k, 9);
	}
	/* Chest. */
	c->chest_x = 12;
	c->chest_z = 3;
	c->chest_y = surface(12, 3) + 1;
	put(c->chest_x, c->chest_y, c->chest_z, BLOCK_CHEST, 0);
	record_cell(c, c->chest_x, c->chest_y, c->chest_z);
	struct te_store st = server_world_tile_store(W());
	bool existed = false;
	int row = te_store_claim(&st, TE_KIND_CHEST, c->chest_x, c->chest_y,
							 c->chest_z, &existed);
	assert(row >= 0);
	for(int k = 0; k < MAX_CHEST_SLOTS; k++)
		S.chest_items[row][k] = I(0, 0, 0);
	S.chest_items[row][0] = I(BLOCK_DIRT, 5, 0);
	S.chest_items[row][4] = I(ITEM_DIAMOND, 3, 0);
	S.chest_items[row][13] = I(ITEM_IRON_PICKAXE, 1, 37);
	S.chest_items[row][26] = I(BLOCK_SAPLING, 2, 1);
	/* Furnace: 4 iron ore + 2 coal, smelt 450 ticks. */
	c->furn_x = 3;
	c->furn_z = 12;
	c->furn_y = surface(3, 12) + 1;
	put(c->furn_x, c->furn_y, c->furn_z, BLOCK_FURNACE, 2);
	record_cell(c, c->furn_x, c->furn_y, c->furn_z);
	assert(furnace_te_register_w(W(), c->furn_x, c->furn_y, c->furn_z) >= 0);
	struct furnace_te* fu = furnace_te_lookup_w(W(), c->furn_x, c->furn_y, c->furn_z);
	assert(fu);
	fu->items[0] = I(BLOCK_IRON_ORE, 4, 0);
	fu->items[1] = I(ITEM_COAL, 2, 0);
	for(int t = 0; t < 450; t++)
		furnace_server_tick_w(W());
	fu = furnace_te_lookup_w(W(), c->furn_x, c->furn_y, c->furn_z);
	assert(fu && fu->items[2].id == ITEM_IRON && fu->items[2].count == 2
		   && fu->cook_time > 0 && fu->burn_time > 0);
	/* Armour, hotbar, main inventory. */
	S.player.inventory.items[INVENTORY_SLOT_ARMOR + 0] = I(ITEM_IRON_HELMET, 1, 11);
	S.player.inventory.items[INVENTORY_SLOT_ARMOR + 1] = I(ITEM_LEATHER_CHESTPLATE, 1, 3);
	S.player.inventory.items[INVENTORY_SLOT_ARMOR + 3] = I(ITEM_DIAMOND_BOOTS, 1, 0);
	S.player.inventory.items[INVENTORY_SLOT_HOTBAR + 0] = I(ITEM_STONE_PICKAXE, 1, 17);
	S.player.inventory.items[INVENTORY_SLOT_HOTBAR + 1] = I(BLOCK_TORCH, 32, 0);
	S.player.inventory.items[INVENTORY_SLOT_HOTBAR + 8] = I(ITEM_BREAD, 3, 0);
	inventory_set_hotbar(&S.player.inventory, 1);
	/* Two mined drops left on the ground, and a sheep. */
	add_entity_item(I(BLOCK_DIRT, 1, 0), 5.5, surface(5, 6) + 1.2, 6.5);
	add_entity_item(I(BLOCK_COBBLESTONE, 2, 0), 18.5, surface(18, 5) + 1.2, 5.5);
	add_sheep(7.5, surface(7, 20) + 1.0, 20.5);
	S.player.health = 13;
	/* Travel east into a second region, place a marker. */
	int fy = surface(JR_FAR_X + 4, 4);
	put(JR_FAR_X + 4, fy + 1, 4, BLOCK_GLOWSTONE, 0);
	record_cell(c, JR_FAR_X + 4, fy + 1, 4);
	add_entity_item(I(ITEM_STICK, 7, 0), JR_FAR_X + 6.5, surface(JR_FAR_X + 6, 6) + 1.2, 6.5);
	S.player.x = JR_FAR_X + 8.5;
	S.player.z = 8.5;
	S.player.y = surface(JR_FAR_X + 8, 8) + 1 + PLAYER_EYE_HEIGHT;
}
static void play_gen2(void) {
	struct jr_snap* c = &s_cells;
	int row = server_local_chest_row_w(W(), c->chest_x, c->chest_y, c->chest_z);
	assert(row >= 0);
	S.chest_items[row][4] = I(ITEM_DIAMOND, 1, 0);
	S.chest_items[row][5] = I(ITEM_IRON, 2, 0);
	S.chest_items[row][26] = I(0, 0, 0);
	for(int t = 0; t < 150; t++)
		furnace_server_tick_w(W());
	for(int dz = 0; dz < 3; dz++) {
		int x = JR_FAR_X + 2, z = 10 + dz;
		int y = surface(x, z);
		mine(x, y, z);
		record_cell(c, x, y, z);
	}
	S.player.inventory.items[INVENTORY_SLOT_ARMOR + 0] = I(0, 0, 0);
	S.player.inventory.items[INVENTORY_SLOT_ARMOR + 2] = I(ITEM_GOLD_LEGGINGS, 1, 4);
	S.player.inventory.items[INVENTORY_SLOT_HOTBAR + 1].count = 29;
	S.player.x -= 30.0;
	S.player.y = surface((int)floor(S.player.x), 8) + 1 + PLAYER_EYE_HEIGHT;
	S.player.health = 9;
}

/* ---- comparison ------------------------------------------------------- */
static bool player_eq(const struct jr_snap* a, const struct jr_snap* b,
					  bool print) {
	bool ok = true;
	for(int k = 0; k < INVENTORY_SIZE; k++)
		if(!item_eq(a->inv[k], b->inv[k])) {
			if(print)
				printf("    inventory slot %d: %u x%u d%u vs %u x%u d%u\n", k,
					   a->inv[k].id, a->inv[k].count, a->inv[k].durability,
					   b->inv[k].id, b->inv[k].count, b->inv[k].durability);
			ok = false;
		}
	if(fabs(a->x - b->x) > 1e-6 || fabs(a->y - b->y) > 1e-6
	   || fabs(a->z - b->z) > 1e-6 || a->health != b->health || a->dim != b->dim) {
		if(print)
			printf("    player %.6f %.6f %.6f hp %d dim %d vs %.6f %.6f %.6f hp"
				   " %d dim %d\n", a->x, a->y, a->z, a->health, a->dim, b->x,
				   b->y, b->z, b->health, b->dim);
		ok = false;
	}
	return ok;
}
static bool chunk_eq(const struct jr_chunk* a, const struct jr_chunk* b) {
	return a->blocks == b->blocks && a->te == b->te && a->items == b->items
		&& a->mobs == b->mobs;
}
/* Exact comparison of a reopened snapshot against one expected snapshot. */
static bool snap_exact(const struct jr_snap* got, const struct jr_snap* want) {
	bool ok = player_eq(got, want, true);
	for(int k = 0; k < MAX_CHEST_SLOTS; k++)
		if(!item_eq(got->chest[k], want->chest[k])) {
			printf("    chest slot %d differs\n", k);
			ok = false;
		}
	if(got->furnace.burn_time != want->furnace.burn_time
	   || got->furnace.cook_time != want->furnace.cook_time
	   || got->furnace.current_item_burn_time != want->furnace.current_item_burn_time
	   || !item_eq(got->furnace.items[0], want->furnace.items[0])
	   || !item_eq(got->furnace.items[1], want->furnace.items[1])
	   || !item_eq(got->furnace.items[2], want->furnace.items[2])) {
		printf("    furnace burn %d cook %d cur %d vs burn %d cook %d cur %d\n",
			   got->furnace.burn_time, got->furnace.cook_time,
			   got->furnace.current_item_burn_time, want->furnace.burn_time,
			   want->furnace.cook_time, want->furnace.current_item_burn_time);
		ok = false;
	}
	for(unsigned k = 0; k < want->ncells; k++)
		if(got->cells[k].type != want->cells[k].type
		   || got->cells[k].meta != want->cells[k].meta) {
			printf("    cell (%d,%d,%d): %u:%u vs %u:%u\n", want->cells[k].x,
				   want->cells[k].y, want->cells[k].z, got->cells[k].type,
				   got->cells[k].meta, want->cells[k].type, want->cells[k].meta);
			ok = false;
		}
	for(unsigned k = 0; k < want->nchunks; k++)
		if(!chunk_eq(&got->chunks[k], &want->chunks[k])) {
			printf("    chunk (%d,%d): blocks %016" PRIx64 " te %016" PRIx64
				   " items %u mobs %u vs %016" PRIx64 " %016" PRIx64 " %u %u\n",
				   want->chunks[k].cx, want->chunks[k].cz, got->chunks[k].blocks,
				   got->chunks[k].te, got->chunks[k].items, got->chunks[k].mobs,
				   want->chunks[k].blocks, want->chunks[k].te,
				   want->chunks[k].items, want->chunks[k].mobs);
			ok = false;
		}
	return ok;
}

/* ---- children --------------------------------------------------------- */
struct jr_op {
	int op;
	char rel[64];
	unsigned nth;
};
static char s_root[220];
static struct jr_op s_ops[JR_MAX_OPS];
static unsigned s_nops;
static struct jr_op s_target;
static struct thread_mutex s_lock;
static void die_now(void) {
#ifdef _WIN32
	TerminateProcess(GetCurrentProcess(), JR_KILL_RC);
#else
	kill(getpid(), SIGKILL);
#endif
	for(;;)
		thread_msleep(1000);
}
static void trace_hook(int op, const char* path, uint64_t a, uint64_t b) {
	(void)a;
	(void)b;
	size_t n = strlen(s_root);
	const char* rel = strncmp(path, s_root, n) ? path : path + n;
	thread_mutex_lock(&s_lock);
	unsigned nth = 1;
	for(unsigned k = 0; k < s_nops; k++)
		nth += s_ops[k].op == op && !strcmp(s_ops[k].rel, rel);
	if(s_nops < JR_MAX_OPS) {
		s_ops[s_nops].op = op;
		snprintf(s_ops[s_nops].rel, sizeof s_ops[s_nops].rel, "%s", rel);
		s_ops[s_nops].nth = nth;
		s_nops++;
	}
	if(s_target.op == op && s_target.nth == nth && !strcmp(s_target.rel, rel))
		die_now();
	thread_mutex_unlock(&s_lock);
}
static char* sfx(const char* root, const char* suffix) {
	static char buf[4][300];
	static int i;
	char* p = buf[i++ & 3];
	snprintf(p, 300, "%s%s", root, suffix);
	return p;
}

/* play census|kill <root> [<op> <rel> <nth>] */
static int child_play(int argc, char** argv) {
	if(argc < 4)
		return 2;
	bool census = !strcmp(argv[2], "census");
	snprintf(s_root, sizeof s_root, "%s", argv[3]);
	memset(&s_target, 0, sizeof s_target);
	if(!census) {
		if(argc < 7)
			return 2;
		s_target.op = atoi(argv[4]);
		snprintf(s_target.rel, sizeof s_target.rel, "%s", argv[5]);
		s_target.nth = (unsigned)atoi(argv[6]);
	}
	assert(thread_mutex_init(&s_lock));
	boot();
	assert(JR_MKDIR(s_root) == 0);
	{
		string_t lvl;
		string_init_set_str(lvl, s_root);
		struct level_archive la;
		assert(level_archive_create_new(&la, lvl, JR_SEED,
										(vec3) {8.5f, 80.0f, 8.5f}));
		level_archive_destroy(&la);
		string_clear(lvl);
	}
	if(!open_world(s_root))
		return 4;
	play_gen1();
	static struct jr_snap snap;
	take_snap(&snap, &s_cells);
	if(census && !write_snap(sfx(s_root, "_snap1.bin"), &snap))
		return 6;
	if(census)
		print_snap("gen 1", &snap);
	if(!quit_world())
		return 5;
	g9io_copy_tree(s_root, sfx(s_root, "_gen1"));
	/* Same process reopens (quit to title, load again). */
	if(!open_world(s_root))
		return 7;
	static struct jr_snap again;
	take_snap(&again, &s_cells);
	if(!snap_exact(&again, &snap))
		return 8;
	play_gen2();
	take_snap(&snap, &s_cells);
	if(census && !write_snap(sfx(s_root, "_snap2.bin"), &snap))
		return 6;
	if(census)
		print_snap("gen 2", &snap);
	region_archive_test_trace(trace_hook);
	bool ok = quit_world();
	region_archive_test_trace(NULL);
	if(!census)
		return 3; /* the named transition never happened */
	if(!ok)
		return 5;
	g9io_copy_tree(s_root, sfx(s_root, "_gen2"));
	FILE* f = fopen(sfx(s_root, "_ops.txt"), "w");
	if(!f)
		return 6;
	for(unsigned k = 0; k < s_nops; k++)
		fprintf(f, "%d %s %u\n", s_ops[k].op, s_ops[k].rel, s_ops[k].nth);
	return fclose(f) || s_nops >= JR_MAX_OPS ? 6 : 0;
}

/* verify <dir> <snapA> [<snapB>]: production open, compare, save, reopen,
 * compare again. With one snapshot: exact. With two: the player record is
 * wholly A or wholly B, each chunk wholly A or wholly B. Exit code: 0 ok;
 * stdout line "RESULT level=<1|2> chunks=<n1>/<n2>". */
static int which_gen(const struct jr_snap* got, const struct jr_snap* a,
					 const struct jr_snap* b, int* level, unsigned* n1,
					 unsigned* n2) {
	bool ok = true;
	*level = player_eq(got, a, false) ? 1 : player_eq(got, b, false) ? 2 : 0;
	if(!*level) {
		printf("    player record matches neither generation:\n");
		player_eq(got, b, true);
		ok = false;
	}
	*n1 = *n2 = 0;
	for(unsigned k = 0; k < a->nchunks; k++) {
		bool is1 = chunk_eq(&got->chunks[k], &a->chunks[k]);
		bool is2 = chunk_eq(&got->chunks[k], &b->chunks[k]);
		*n1 += is1 && !is2;
		*n2 += is2 && !is1;
		if(!is1 && !is2) {
			printf("    chunk (%d,%d) matches neither generation (items %u mobs"
				   " %u; gen1 %u/%u gen2 %u/%u)\n", a->chunks[k].cx,
				   a->chunks[k].cz, got->chunks[k].items, got->chunks[k].mobs,
				   a->chunks[k].items, a->chunks[k].mobs, b->chunks[k].items,
				   b->chunks[k].mobs);
			ok = false;
		}
	}
	return ok;
}
static int child_verify(int argc, char** argv) {
	if(argc < 4)
		return 2;
	static struct jr_snap a, b, got, got2;
	bool two = argc >= 5;
	if(!read_snap(argv[3], &a) || (two && !read_snap(argv[4], &b)))
		return 2;
	boot();
	s_cells = a; /* positions only; take_snap rereads every value */
	if(!open_world(argv[2])) {
		printf("    production open FAILED\n");
		return 4;
	}
	take_snap(&got, &a);
	int level = 0;
	unsigned n1 = 0, n2 = 0;
	bool ok = two ? which_gen(&got, &a, &b, &level, &n1, &n2) : snap_exact(&got, &a);
	if(!quit_world()) {
		printf("    subsequent save FAILED\n");
		return 5;
	}
	if(!open_world(argv[2])) {
		printf("    reopen after the subsequent save FAILED\n");
		return 4;
	}
	take_snap(&got2, &a);
	if(!snap_exact(&got2, &got)) {
		printf("    the subsequent save did not read back exactly\n");
		ok = false;
	}
	quit_world();
	printf("RESULT level=%d chunks=%u/%u\n", level, n1, n2);
	return ok ? 0 : 1;
}

/* ---- parent ----------------------------------------------------------- */
static int run_child(const char* args, char* out, size_t out_n) {
	char cmd[900];
#ifdef _WIN32
	snprintf(cmd, sizeof cmd, "\"\"%s\" %s\"", s_self, args);
#else
	snprintf(cmd, sizeof cmd, "\"%s\" %s", s_self, args);
#endif
	if(out)
		out[0] = 0;
	FILE* p = popen(cmd, "r");
	if(!p)
		return -1;
	char line[512];
	while(fgets(line, sizeof line, p)) {
		fputs(line, stdout);
		if(out && !strncmp(line, "RESULT ", 7))
			snprintf(out, out_n, "%s", line);
	}
	int rc = pclose(p);
#ifndef _WIN32
	if(rc != -1 && WIFSIGNALED(rc))
		return 128 + WTERMSIG(rc);
	if(rc != -1 && WIFEXITED(rc))
		return WEXITSTATUS(rc);
#endif
	return rc;
}
static void cleanup(const char* root) {
	static const char* const s[] = {"", "_gen1", "_gen2", "_copy", "_ops.txt",
									"_snap1.bin", "_snap2.bin"};
	for(size_t k = 0; k < sizeof s / sizeof *s; k++)
		test_fixture_rm_rf(sfx(root, s[k]));
}

/* Every region record of `dir`, by (file, slot) -> FNV of the framed bytes. */
struct rec {
	char file[48];
	int slot;
	uint64_t hash;
};
#define MAX_RECS 4096
struct recset {
	unsigned n;
	bool ok;
	struct rec r[MAX_RECS];
};
static void scan_records(const char* dir, struct recset* o) {
	o->n = 0;
	o->ok = true;
	static const char* const files[] = {"r.0.0.mcr", "r.0.-1.mcr", "r.-1.0.mcr",
										"r.-1.-1.mcr", "r.1.0.mcr", "r.1.-1.mcr"};
	for(size_t f = 0; f < sizeof files / sizeof *files; f++) {
		char path[400];
		snprintf(path, sizeof path, "%s/region/%s", dir, files[f]);
		FILE* fp = fopen(path, "rb");
		if(!fp)
			continue;
		uint8_t hdr[4096];
		if(fread(hdr, 1, sizeof hdr, fp) != sizeof hdr) {
			fclose(fp);
			continue; /* an empty/short file: the reopen judges it */
		}
		for(int slot = 0; slot < 1024; slot++) {
			uint32_t off = (uint32_t)hdr[slot * 4] << 16
				| (uint32_t)hdr[slot * 4 + 1] << 8 | hdr[slot * 4 + 2];
			unsigned secs = hdr[slot * 4 + 3];
			if(!off)
				continue;
			uint8_t fr[4];
			bool good = !fseek(fp, (long)off * 4096, SEEK_SET)
				&& fread(fr, 4, 1, fp) == 1;
			uint32_t len = good ? (uint32_t)fr[0] << 24 | (uint32_t)fr[1] << 16
					| (uint32_t)fr[2] << 8 | fr[3]
								: 0;
			good = good && len >= 1 && len <= secs * 4096u - 4;
			uint64_t h = FNV0;
			if(good) {
				uint8_t* buf = malloc(len);
				assert(buf);
				good = fread(buf, len, 1, fp) == 1;
				h = fnv(fnv(FNV0, fr, 4), buf, len);
				free(buf);
			}
			if(!good) {
				/* Raw index entry that does not frame; the production reader
				 * (journal replay / salvage) gets the last word in the reopen. */
				h = 0;
			}
			assert(o->n < MAX_RECS);
			snprintf(o->r[o->n].file, sizeof o->r[o->n].file, "%s", files[f]);
			o->r[o->n].slot = slot;
			o->r[o->n].hash = h;
			o->n++;
		}
		fclose(fp);
	}
}
static const struct rec* find_rec(const struct recset* s, const char* file,
								  int slot) {
	for(unsigned k = 0; k < s->n; k++)
		if(s->r[k].slot == slot && !strcmp(s->r[k].file, file))
			return &s->r[k];
	return NULL;
}

static void case_journey(void) {
	char base[200], args[800], result[128];
	test_fixture_dir("save_reload_journey", base, sizeof base);
	assert(JR_MKDIR(base) == 0);
	char root[230];
	snprintf(root, sizeof root, "%s/census", base);
	cleanup(root);
	snprintf(args, sizeof args, "play census %s", root);
	int rc = run_child(args, NULL, 0);
	check(rc == 0, "journey: census child plays gen 1, quits, reloads, plays gen 2, quits");
	snprintf(args, sizeof args, "verify %s %s", sfx(root, "_gen1"),
			 sfx(root, "_snap1.bin"));
	rc = run_child(args, result, sizeof result);
	check(rc == 0, "journey: a fresh process reopens the gen-1 save exactly");
	snprintf(args, sizeof args, "verify %s %s", sfx(root, "_gen2"),
			 sfx(root, "_snap2.bin"));
	rc = run_child(args, result, sizeof result);
	check(rc == 0, "journey: a fresh process reopens the gen-2 save exactly");
	/* Negative control: the comparison can tell the two saves apart. */
	snprintf(args, sizeof args, "verify %s %s", sfx(root, "_gen2"),
			 sfx(root, "_snap1.bin"));
	puts("  negative control (gen-2 image vs gen-1 snapshot; differences expected):");
	rc = run_child(args, result, sizeof result);
	check(rc == 1, "journey: the gen-2 image does NOT match the gen-1 snapshot");
	if(!failures)
		puts("PASS journey: inventory+armour, position, health, chest, furnace"
			 " (mid-smelt), mined/placed cells, per-chunk blocks and entity"
			 " counts survive save -> quit -> fresh-process reload (both saves)");
	cleanup(root);
	test_fixture_dir_cleanup(base);
}

static void case_powercut(void) {
	char base[200], args[800], result[128];
	test_fixture_dir("save_reload_powercut", base, sizeof base);
	assert(JR_MKDIR(base) == 0);
	char root[230];
	snprintf(root, sizeof root, "%s/census", base);
	cleanup(root);
	snprintf(args, sizeof args, "play census %s", root);
	check(run_child(args, NULL, 0) == 0, "powercut: census child completes both saves");
	static struct jr_op ops[JR_MAX_OPS];
	unsigned nops = 0;
	FILE* f = fopen(sfx(root, "_ops.txt"), "r");
	while(f && nops < JR_MAX_OPS
		  && fscanf(f, "%d %63s %u", &ops[nops].op, ops[nops].rel,
					&ops[nops].nth) == 3)
		nops++;
	if(f)
		fclose(f);
	static struct recset g1, g2, k;
	scan_records(sfx(root, "_gen1"), &g1);
	scan_records(sfx(root, "_gen2"), &g2);
	unsigned changed = 0;
	for(unsigned i = 0; i < g2.n; i++) {
		const struct rec* o = find_rec(&g1, g2.r[i].file, g2.r[i].slot);
		changed += !o || o->hash != g2.r[i].hash;
	}
	printf("  powercut: %u gen-2 transitions; %u region records in gen 1, %u in"
		   " gen 2, %u changed by the gen-2 save\n", nops, g1.n, g2.n, changed);
	check(nops > 0 && changed > 0, "powercut: the gen-2 save rewrites region records");
	char snap1[300], snap2[300];
	snprintf(snap1, sizeof snap1, "%s/snap1.bin", base);
	snprintf(snap2, sizeof snap2, "%s/snap2.bin", base);
	g9io_copy_file(sfx(root, "_snap1.bin"), snap1);
	g9io_copy_file(sfx(root, "_snap2.bin"), snap2);
	unsigned kills = 0, mixed_files = 0, mixed_regions = 0;
	for(unsigned i = 0; i < nops; i++) {
		char kroot[240], label[200];
		snprintf(kroot, sizeof kroot, "%s/k%03u", base, i);
		snprintf(label, sizeof label, "kill %03u after op %d %s #%u", i,
				 ops[i].op, ops[i].rel, ops[i].nth);
		cleanup(kroot);
		snprintf(args, sizeof args, "play kill %s %d %s %u", kroot, ops[i].op,
				 ops[i].rel, ops[i].nth);
		int rc = run_child(args, NULL, 0);
		if(rc != JR_KILL_RC)
			printf("  [%s] child exit %d, want %d\n", label, rc, JR_KILL_RC);
		check(rc == JR_KILL_RC, "powercut: the child dies at the named transition");
		kills += rc == JR_KILL_RC;
		/* Byte level: each record is gen 1's or gen 2's, never a third. */
		scan_records(kroot, &k);
		unsigned bad = 0, from1 = 0, from2 = 0;
		for(unsigned r = 0; r < k.n; r++) {
			const struct rec* a = find_rec(&g1, k.r[r].file, k.r[r].slot);
			const struct rec* b = find_rec(&g2, k.r[r].file, k.r[r].slot);
			bool is1 = a && a->hash == k.r[r].hash;
			bool is2 = b && b->hash == k.r[r].hash;
			from1 += is1 && !is2;
			from2 += is2 && !is1;
			if(!is1 && !is2) {
				bad++;
				printf("  [%s] %s slot %d: record is neither gen 1 nor gen 2"
					   " (hash %016" PRIx64 ")\n", label, k.r[r].file,
					   k.r[r].slot, k.r[r].hash);
			}
		}
		for(unsigned r = 0; r < g1.n; r++)
			if(!find_rec(&k, g1.r[r].file, g1.r[r].slot)) {
				bad++;
				printf("  [%s] %s slot %d: committed gen-1 record lost\n", label,
					   g1.r[r].file, g1.r[r].slot);
			}
		check(!bad, "powercut: every region record is wholly gen 1 or wholly gen 2");
		/* The raw journal/salvage is the production reader's to finish: a
		 * fresh process opens, compares per chunk, saves, reopens. */
		snprintf(args, sizeof args, "verify %s %s %s", kroot, snap1, snap2);
		int vr = run_child(args, result, sizeof result);
		int level = 0;
		unsigned n1 = 0, n2 = 0;
		sscanf(result, "RESULT level=%d chunks=%u/%u", &level, &n1, &n2);
		check(vr == 0, "powercut: a fresh process opens the killed world, the"
					   " player is wholly one generation, each chunk wholly one"
					   " generation, and a subsequent save reads back");
		mixed_files += (level == 2 && n1 > 0) || (level == 1 && n2 > 0);
		mixed_regions += from1 > 0 && from2 > 0;
		printf("  %-48s exit %d: records gen1-only %u gen2-only %u bad %u;"
			   " reopen level gen%d chunks %u gen1 / %u gen2, verify %d\n", label,
			   rc, from1, from2, bad, level, n1, n2, vr);
		cleanup(kroot);
	}
	printf("  powercut: %u/%u kills at named transitions; %u reopened with the"
		   " documented cross-file mix, %u with gen-1 and gen-2 records side by"
		   " side across chunks\n", kills, nops, mixed_files, mixed_regions);
	if(!failures)
		puts("PASS powercut: every traced transition of the journey save-and-quit"
			 " leaves a world that opens, a whole player record, and per-chunk"
			 " old-or-new state; the next save succeeds");
	cleanup(root);
	test_fixture_dir_cleanup(base);
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	s_self = argv[0];
	const char* which = argc > 1 ? argv[1] : "journey";
	if(!strcmp(which, "play"))
		return child_play(argc, argv);
	if(!strcmp(which, "verify"))
		return child_verify(argc, argv);
	if(!strcmp(which, "journey"))
		case_journey();
	else if(!strcmp(which, "powercut"))
		case_powercut();
	else
		return 2;
	if(failures) {
		printf("FAIL %s: %u check(s) failed\n", which, failures);
		return 1;
	}
	return 0;
}
