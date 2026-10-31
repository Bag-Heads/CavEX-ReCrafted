/*
	Tests the dungeon chest LOOT path added to source/worldgen/populate.c --
	worldgen_dungeon now materialises Beta 1.7.3 getDungeonLootItem rolls through
	the wgen_world.set_chest hook instead of discarding them. Also covers FH-17's
	spawner materialisation through the sibling wgen_world.set_spawner hook (the
	block's metadata is now always 0; the pickMobSpawner roll instead selects the
	EntityId a MobSpawner tile entity is built from).

	Proves:
	  - every loot item a chest receives is a real Beta dungeon-loot id with a
	    faithful count (stackables 1..4, singletons exactly 1, cocoa dye damage 3),
	  - a chest that generates actually receives loot (the rolls reach storage),
	  - the dungeon's spawner materialises exactly once, at its centre, with one
	    of Beta's 3 pickMobSpawner EntityId strings,
	  - set_chest/set_spawner are RNG- AND buffer-neutral: running the identical
	    dungeon with the hooks vs with both hooks == NULL leaves the PRNG
	    end-state and the block buffer byte-for-byte identical (loot/spawner data
	    land in the separate stores, never in the world), so neither can shift
	    the decorate stream the later ore passes depend on.

	The chamber is the same shape test_dungeon uses (solid stone + one -X wall
	opening, the width peeked from an identical RNG), scanned over seeds until one
	yields a chest-bearing dungeon.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "item/items.h"
#include "worldgen/java_random.h"
#include "worldgen/populate.h"

#define AIR 0
#define STONE 1
#define MOB_SPAWNER 52

#define FW_W 48
struct flatworld {
	uint8_t ids[FW_W * FW_W * 128];
	uint8_t meta[FW_W * FW_W * 128];
};

static int fw_index(int x, int y, int z, int* idx) {
	if(y < 0 || y >= 128 || x < 0 || x >= FW_W || z < 0 || z >= FW_W)
		return 0;
	*idx = (x * FW_W + z) * 128 + y;
	return 1;
}
static uint8_t fw_get(void* ctx, int x, int y, int z) {
	struct flatworld* f = ctx;
	int idx;
	return fw_index(x, y, z, &idx) ? f->ids[idx] : AIR;
}
static void fw_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	struct flatworld* f = ctx;
	int idx;
	if(fw_index(x, y, z, &idx)) {
		f->ids[idx] = id;
		f->meta[idx] = meta;
	}
}
static void put(struct flatworld* f, int x, int y, int z, uint8_t id) {
	int idx;
	if(fw_index(x, y, z, &idx))
		f->ids[idx] = id;
}

// Capture every chest the dungeon stocks.
#define MAXCAP 4
struct cap_chest {
	int x, y, z;
	struct item_data items[DUNGEON_CHEST_SLOTS];
};
static struct cap_chest g_cap[MAXCAP];
static int g_cap_n;
static void cap_set_chest(void* ctx, int x, int y, int z,
						  const struct item_data* items) {
	(void)ctx;
	assert(g_cap_n < MAXCAP);
	g_cap[g_cap_n].x = x;
	g_cap[g_cap_n].y = y;
	g_cap[g_cap_n].z = z;
	memcpy(g_cap[g_cap_n].items, items, sizeof(g_cap[g_cap_n].items));
	g_cap_n++;
}

// FH-17: capture the dungeon's spawner materialisation (w->set_spawner).
static int g_spawner_cap_n;
static int g_spawner_cap_x, g_spawner_cap_y, g_spawner_cap_z;
static char g_spawner_cap_mob_id[32];
static void cap_set_spawner(void* ctx, int x, int y, int z,
							const char* mob_id) {
	(void)ctx;
	g_spawner_cap_n++;
	g_spawner_cap_x = x;
	g_spawner_cap_y = y;
	g_spawner_cap_z = z;
	strncpy(g_spawner_cap_mob_id, mob_id, sizeof(g_spawner_cap_mob_id) - 1);
}

static int is_singleton(uint16_t id) {
	return id == ITEM_SADDLE || id == ITEM_BREAD || id == ITEM_BUCKET
		|| id == ITEM_APPLE_GOLDEN || id == ITEM_DYE;
}
static int valid_loot_id(uint16_t id) {
	switch(id) {
		case ITEM_SADDLE:
		case ITEM_IRON:
		case ITEM_BREAD:
		case ITEM_WHEAT:
		case ITEM_GUNPOWDER:
		case ITEM_STRING:
		case ITEM_BUCKET:
		case ITEM_APPLE_GOLDEN:
		case ITEM_REDSTONE:
		case ITEM_DYE: return 1;
	}
	return 0;
}

// Shape a solid-stone chamber with one -X wall opening; the dungeon's width is
// peeked from an identical RNG so the opening lands on its perimeter.
static void shape(struct flatworld* f, int px, int py, int pz, int64_t seed) {
	struct java_random rp;
	jrand_set_seed(&rp, seed);
	int wx = jrand_next_int_bound(&rp, 2) + 2;
	memset(f->ids, STONE, sizeof(f->ids));
	memset(f->meta, 0, sizeof(f->meta));
	put(f, px - wx - 1, py, pz, AIR);
	put(f, px - wx - 1, py + 1, pz, AIR);
}

int main(void) {
	const int PX = 24, PY = 64, PZ = 24;
	struct flatworld* f = malloc(sizeof *f);
	assert(f);

	// Find a seed whose dungeon generates AND stocks at least one chest.
	int64_t use_seed = -1;
	for(int64_t seed = 1; seed <= 256 && use_seed < 0; seed++) {
		shape(f, PX, PY, PZ, seed);
		g_cap_n = 0;
		struct wgen_world w = {f, fw_get, fw_set, NULL, NULL, cap_set_chest};
		struct java_random r;
		jrand_set_seed(&r, seed);
		if(worldgen_dungeon(&w, &r, PX, PY, PZ) && g_cap_n >= 1)
			use_seed = seed;
	}
	assert(use_seed >= 0); // a chest-bearing dungeon exists in the search range

	// Canonical run of the chosen seed WITH the capture hooks, plus a parallel
	// run with NO hooks, to prove set_chest/set_spawner perturb neither the
	// PRNG nor the blocks.
	struct flatworld* fa = malloc(sizeof *fa);
	struct flatworld* fb = malloc(sizeof *fb);
	assert(fa && fb);
	shape(fa, PX, PY, PZ, use_seed);
	memcpy(fb->ids, fa->ids, sizeof(fa->ids));
	memcpy(fb->meta, fa->meta, sizeof(fa->meta));

	struct wgen_world wa = {fa,		fw_get, fw_set, NULL, NULL,
							cap_set_chest, NULL,	cap_set_spawner};
	struct wgen_world wb = {fb, fw_get, fw_set, NULL, NULL, NULL};
	struct java_random ra, rb;
	jrand_set_seed(&ra, use_seed);
	jrand_set_seed(&rb, use_seed);
	g_cap_n = 0;
	g_spawner_cap_n = 0;
	bool oa = worldgen_dungeon(&wa, &ra, PX, PY, PZ);
	bool ob = worldgen_dungeon(&wb, &rb, PX, PY, PZ);
	assert(oa && ob);
	assert(ra.seed == rb.seed); // the hook did not consume/shift any randomness
	assert(memcmp(fa->ids, fb->ids, sizeof(fa->ids)) == 0);	 // nor any block id
	assert(memcmp(fa->meta, fb->meta, sizeof(fa->meta)) == 0); // nor metadata

	// Validate the captured loot.
	int total_items = 0;
	for(int c = 0; c < g_cap_n; c++) {
		for(int s = 0; s < DUNGEON_CHEST_SLOTS; s++) {
			struct item_data it = g_cap[c].items[s];
			if(!it.id)
				continue; // empty slot (incl. the unrepresentable music disc)
			total_items++;
			assert(valid_loot_id(it.id));
			assert(it.count >= 1 && it.count <= 4);
			if(is_singleton(it.id))
				assert(it.count == 1);
			if(it.id == ITEM_DYE)
				assert(it.durability == 3); // cocoa beans
		}
	}
	assert(total_items >= 1); // the chest really received loot

	// FH-17: the dungeon's mob spawner block is faithful (metadata 0, Beta's
	// own value for BlockMobSpawner) -- Beta's pickMobSpawner index instead
	// selects the EntityId string materialised as a MobSpawner tile entity
	// through w->set_spawner.
	int spi;
	assert(fw_index(PX, PY, PZ, &spi));
	assert(fa->ids[spi] == MOB_SPAWNER);
	assert(fa->meta[spi] == 0);				// Beta-faithful, not the legacy pick index
	assert(fa->meta[spi] == fb->meta[spi]); // deterministic across runs (and set_spawner-neutral)

	assert(g_spawner_cap_n == 1); // exactly one spawner materialised
	assert(g_spawner_cap_x == PX && g_spawner_cap_y == PY
		   && g_spawner_cap_z == PZ);
	assert(strcmp(g_spawner_cap_mob_id, "Skeleton") == 0
		   || strcmp(g_spawner_cap_mob_id, "Zombie") == 0
		   || strcmp(g_spawner_cap_mob_id, "Spider") == 0);

	printf("test_dungeon_loot: seed %lld -> %d chest(s), %d loot item(s), spawner "
		   "mob \"%s\" @(%d,%d,%d); ids valid + counts faithful; "
		   "set_chest/set_spawner are RNG/buffer-neutral\n",
		   (long long)use_seed, g_cap_n, total_items, g_spawner_cap_mob_id,
		   g_spawner_cap_x, g_spawner_cap_y, g_spawner_cap_z);

	free(f);
	free(fa);
	free(fb);
	return 0;
}
