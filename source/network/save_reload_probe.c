/* Copyright (c) 2026 CaveWii_v2 contributors
 * save_reload_3m server-thread probe -- see save_reload_probe.h. */
#include "save_reload_probe.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "client_interface.h"
#include "server_furnace.h"
#include "server_local.h"
#include "server_local_state.h"
#include "server_world.h"
#include "tile_entities_store.h"
#include "../block/blocks_data.h"
#include "../block/furnace_logic.h"
#include "../item/inventory.h"
#include "../item/item_types.h"

#define SR_CHEST_SLOTS 27 /* a single Beta chest */

static bool s_armed;

/* Server-thread-only fixture state (never read by main). */
static struct {
	int defer;
	bool applied;
	int ax, ay, az; /* the anchor: the player's block at apply time */
} F;

/* One writer (server thread), one reader (main). Seqlock: odd = writing. */
static uint32_t s_seq;
static struct save_reload_probe_view s_pub;

uint64_t save_reload_fnv(uint64_t h, const void* p, unsigned n) {
	const uint8_t* b = p;
	for(unsigned k = 0; k < n; k++)
		h = (h ^ b[k]) * UINT64_C(0x100000001B3);
	return h;
}

void save_reload_probe_arm(bool on) {
	memset(&F, 0, sizeof F);
	__atomic_store_n(&s_seq, 0, __ATOMIC_RELAXED);
	memset(&s_pub, 0, sizeof s_pub);
	__atomic_store_n(&s_armed, on, __ATOMIC_RELEASE);
}

bool save_reload_probe_armed(void) {
	return __atomic_load_n(&s_armed, __ATOMIC_RELAXED);
}

static void pub_begin(void) {
	__atomic_add_fetch(&s_seq, 1, __ATOMIC_ACQ_REL);
	__atomic_thread_fence(__ATOMIC_RELEASE);
}
static void pub_end(void) {
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_add_fetch(&s_seq, 1, __ATOMIC_RELEASE);
}

bool save_reload_probe_read(struct save_reload_probe_view* out) {
	for(int tries = 0; tries < 8; tries++) {
		uint32_t a = __atomic_load_n(&s_seq, __ATOMIC_ACQUIRE);
		if(a & 1u)
			continue;
		memcpy(out, &s_pub, sizeof *out);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if(__atomic_load_n(&s_seq, __ATOMIC_RELAXED) == a)
			return true;
	}
	return false; /* bounded: the caller simply retries next frame */
}

/* ---- the fixed fixture -------------------------------------------------- */
struct sr_cell {
	int dx, dy, dz;
	uint8_t type;
};
/* Placed: stone + cobblestone pillar. Broken: two ground cells. Chest and
 * furnace (the furnace may legitimately be lit, see sr_cell_ok). */
static const struct sr_cell CELLS[] = {
	{2, 0, 0, BLOCK_STONE},		  {2, 1, 0, BLOCK_COBBLESTONE},
	{-2, -1, 0, BLOCK_AIR},		  {-2, -1, 1, BLOCK_AIR},
	{0, 0, 2, BLOCK_CHEST},		  {0, 0, -2, BLOCK_FURNACE},
};
#define NCELLS (sizeof CELLS / sizeof *CELLS)

static struct item_data I(uint16_t id, unsigned count, uint16_t dur) {
	return (struct item_data) {
		.id = id, .count = (uint8_t)count, .durability = dur};
}
static const struct {
	int slot;
	uint16_t id;
	uint8_t count;
	uint16_t dur;
} CHEST[] = {{0, BLOCK_DIRT, 5, 0},
			 {4, ITEM_DIAMOND, 3, 0},
			 {13, ITEM_IRON_PICKAXE, 1, 37},
			 {26, BLOCK_SAPLING, 2, 1}};
static const struct {
	int slot;
	uint16_t id;
	uint8_t count;
	uint16_t dur;
} INV[] = {{INVENTORY_SLOT_HOTBAR + 0, BLOCK_COBBLESTONE, 17, 0},
		   {INVENTORY_SLOT_HOTBAR + 4, ITEM_IRON_PICKAXE, 1, 12},
		   {INVENTORY_SLOT_MAIN + 0, ITEM_DIAMOND, 7, 0},
		   {INVENTORY_SLOT_MAIN + 13, ITEM_COAL, 9, 0},
		   {INVENTORY_SLOT_MAIN + 26, BLOCK_SAPLING, 3, 2}};

static bool item_is(struct item_data a, uint16_t id, uint8_t count,
					uint16_t dur) {
	return a.id == id && a.count == count && a.durability == dur;
}
static uint64_t item_hash(uint64_t h, struct item_data it) {
	uint16_t v[3] = {0, 0, 0};
	if(it.count && it.id) {
		v[0] = it.id;
		v[1] = it.durability;
		v[2] = it.count;
	}
	return save_reload_fnv(h, v, sizeof v);
}

static void sr_set(struct server_local* s, int x, int y, int z, uint8_t t,
				   uint8_t m) {
	server_world_set_block(&s->worlds[0], x, y, z,
						   (struct block_data) {.type = t, .metadata = m,
												.sky_light = 15,
												.torch_light = 0});
}

static void sr_apply(struct server_local* s) {
	struct server_world* w = &s->worlds[0];
	F.ax = (int)floor(s->player.x);
	F.ay = (int)floor(s->player.y);
	F.az = (int)floor(s->player.z);
	for(unsigned k = 0; k < NCELLS; k++)
		sr_set(s, F.ax + CELLS[k].dx, F.ay + CELLS[k].dy, F.az + CELLS[k].dz,
			   CELLS[k].type, CELLS[k].type == BLOCK_FURNACE ? 2 : 0);
	/* Chest: the backing row registered exactly as block_chest.c's
	 * onItemPlace does (a bare chest block cannot hold items). */
	struct te_store st = server_world_tile_store(w);
	bool existed = false;
	int row = te_store_claim(&st, TE_KIND_CHEST, F.ax, F.ay, F.az + 2,
							 &existed);
	if(row >= 0) {
		for(int k = 0; k < MAX_CHEST_SLOTS; k++)
			s->chest_items[row][k] = I(0, 0, 0);
		for(unsigned k = 0; k < sizeof CHEST / sizeof *CHEST; k++)
			s->chest_items[row][CHEST[k].slot]
				= I(CHEST[k].id, CHEST[k].count, CHEST[k].dur);
	}
	/* Furnace: ore + fuel; the normal furnace tick lights and smelts it. */
	(void)furnace_te_register_w(w, F.ax, F.ay, F.az - 2);
	struct furnace_te* fu = furnace_te_lookup_w(w, F.ax, F.ay, F.az - 2);
	if(fu) {
		fu->items[0] = I(BLOCK_IRON_ORE, 4, 0);
		fu->items[1] = I(ITEM_COAL, 2, 0);
	}
	/* Inventory: fixed stacks, mirrored to the client window exactly as the
	 * load pass (server_local_finish_load_inventory) re-sends slots. */
	for(unsigned k = 0; k < sizeof INV / sizeof *INV; k++) {
		s->player.inventory.items[INV[k].slot]
			= I(INV[k].id, INV[k].count, INV[k].dur);
		clin_rpc_send(&(struct client_rpc) {
			.type = CRPC_INVENTORY_SLOT,
			.payload.inventory_slot.window = WINDOWC_INVENTORY,
			.payload.inventory_slot.slot = INV[k].slot,
			.payload.inventory_slot.item = s->player.inventory.items[INV[k].slot],
		});
	}
	F.applied = true;
	printf("[save_reload] fixture applied at (%d,%d,%d) chest_row=%d furnace=%d\n",
		   F.ax, F.ay, F.az, row, fu != NULL);
}

bool save_reload_probe_capture(struct server_local* s,
							   struct save_reload_snap* o) {
	if(!save_reload_probe_armed() || !F.applied || !s || !o)
		return false;
	struct server_world* w = &s->worlds[0];
	memset(o, 0, sizeof *o);
	o->resident = true;
	o->fixture_ok = true;
	/* Inventory: every slot, empty stacks normalised. */
	uint64_t h = SAVE_RELOAD_FNV0;
	for(int k = 0; k < INVENTORY_SIZE; k++)
		h = item_hash(h, s->player.inventory.items[k]);
	o->inv = h;
	for(unsigned k = 0; k < sizeof INV / sizeof *INV; k++)
		o->fixture_ok &= item_is(s->player.inventory.items[INV[k].slot],
								 INV[k].id, INV[k].count, INV[k].dur);
	/* Blocks: relative position, type and metadata of every fixture cell. */
	h = SAVE_RELOAD_FNV0;
	for(unsigned k = 0; k < NCELLS; k++) {
		struct block_data b = {0};
		if(!server_world_get_block(w, F.ax + CELLS[k].dx, F.ay + CELLS[k].dy,
								   F.az + CELLS[k].dz, &b))
			o->resident = false;
		int32_t v[5] = {CELLS[k].dx, CELLS[k].dy, CELLS[k].dz, b.type,
						b.metadata};
		h = save_reload_fnv(h, v, sizeof v);
		bool ok = b.type == CELLS[k].type
			|| (CELLS[k].type == BLOCK_FURNACE && b.type == BLOCK_FURNACE_LIT);
		o->fixture_ok &= ok;
	}
	o->blocks = h;
	/* Chest. */
	h = SAVE_RELOAD_FNV0;
	int row = server_local_chest_row_w(w, F.ax, F.ay, F.az + 2);
	if(row < 0) {
		o->fixture_ok = false;
		h = save_reload_fnv(h, "nochest", 7);
	} else {
		for(int k = 0; k < SR_CHEST_SLOTS; k++)
			h = item_hash(h, s->chest_items[row][k]);
		for(unsigned k = 0; k < sizeof CHEST / sizeof *CHEST; k++)
			o->fixture_ok &= item_is(s->chest_items[row][CHEST[k].slot],
									 CHEST[k].id, CHEST[k].count, CHEST[k].dur);
	}
	o->chest = h;
	/* Furnace: the three timers and the three slots. */
	h = SAVE_RELOAD_FNV0;
	struct furnace_te* fu = furnace_te_lookup_w(w, F.ax, F.ay, F.az - 2);
	if(!fu) {
		o->fixture_ok = false;
		o->burn = o->cook = -1;
		h = save_reload_fnv(h, "nofurnace", 9);
	} else {
		int32_t t[3] = {fu->burn_time, fu->cook_time,
						fu->current_item_burn_time};
		h = save_reload_fnv(h, t, sizeof t);
		for(int k = 0; k < 3; k++)
			h = item_hash(h, fu->items[k]);
		o->burn = fu->burn_time;
		o->cook = fu->cook_time;
		/* Ore still in, or already smelted out: either way iron is there. */
		o->fixture_ok &= (fu->items[0].count && fu->items[0].id == BLOCK_IRON_ORE)
			|| (fu->items[2].count && fu->items[2].id == ITEM_IRON);
	}
	o->furnace = h;
	/* Player position (the archive stores doubles). */
	o->x = s->player.x;
	o->y = s->player.y;
	o->z = s->player.z;
	double p[3] = {o->x, o->y, o->z};
	o->pos = save_reload_fnv(SAVE_RELOAD_FNV0, p, sizeof p);
	return true;
}

void save_reload_probe_publish_save(const struct save_reload_snap* snap) {
	if(!save_reload_probe_armed() || !snap)
		return;
	pub_begin();
	s_pub.before = *snap;
	s_pub.saves++;
	pub_end();
}

void save_reload_probe_on_load(struct server_local* s) {
	if(!save_reload_probe_armed() || !s)
		return;
	struct save_reload_snap after;
	bool took = F.applied && save_reload_probe_capture(s, &after);
	pub_begin();
	s_pub.loads++;
	if(s_pub.loads == 1) {
		const char* path = string_get_cstr(s->level_name);
		const char* base = path;
		for(const char* c = path; c && *c; c++)
			if(*c == '/' || *c == '\\')
				base = c + 1;
		snprintf(s_pub.folder, sizeof s_pub.folder, "%s", base ? base : "");
		F.defer = SAVE_RELOAD_PROBE_FIXTURE_DELAY_TICKS;
	}
	if(took) {
		s_pub.after = after;
		s_pub.after_loads++;
	}
	pub_end();
}

void save_reload_probe_tick(struct server_local* s) {
	if(!save_reload_probe_armed() || !s || F.applied || F.defer <= 0)
		return;
	if(!s->player.finished_loading || !s->player.has_pos)
		return;
	if(--F.defer > 0)
		return;
	sr_apply(s);
	pub_begin();
	s_pub.applied = 1;
	pub_end();
}
