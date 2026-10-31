/*
	Golden tests for Beta 1.7.3 map item, part 2a (Batch F, lane 2)
	[740c583: ItemMap.java, ItemMapBase.java, MapData.java, MapColor.java,
	 Material.java, MapStorage.java, CraftingManager.java:73].

	Part 1 (map DATA layer -- struct map_data, the pure scan-math helpers,
	gzip-NBT persistence) is covered by tests/test_mapdata_golden.c and is
	NOT re-tested here. This file covers what part 1 explicitly left as the
	"ItemMap" seam: the crafting recipe, onCreated/onUpdate dispatch, the
	colour-scan core (histogram/majority-vote/water-shading/brightness), the
	block->MapColor table, and idcounts.dat persistence.

	SCOPING NOTE: onCreated/onUpdate are exercised by calling `item_map`'s
	function pointers DIRECTLY (`item_map.onCreated(&s, &s.player, &stack)` etc.)
	rather than driving the full SRPC_WINDOW_CLICK crafting-grid dispatch --
	this is a deliberate, narrower slice than test_sign_placement.c's
	end-to-end SRPC test: it pins ItemMap's OWN behaviour (which this lane
	owns) without re-testing the generic crafting-output-pickup plumbing
	(inventory_crafting.c/inventory_player.c, shared with every other
	craftable item, exercised by inventory_action's own tests). The
	crafting SHAPE/OUTPUT itself is pinned separately and directly via
	recipe_match, matching test_sign_recipe.c/test_piston_recipe.c's own
	established convention for a *_recipe check.

	Every expected numeric value below is hand-derived directly from the
	cited decompile formula in comments at its point of use (CODESTYLE 4.2:
	never read back from CaveWii's own output) -- the colour-scan test's
	arithmetic is walked step-by-step in its own comment block.

	Two colour-scan sections, deliberately: section 4 runs at scale=0
	(blocks_per_pixel=1, one sample per pixel) for hand-tractability and to
	pin the stripe gate's full 16-tick sweep; section 4b runs at scale=3
	(blocks_per_pixel=8, the ONLY scale either onCreated or getMapData's
	fallback ever actually seeds -- map_data_set_defaults) with a genuine
	64-sample-per-pixel histogram, a real majority-vote decision between two
	competing block types, a nonuniform-depth water pixel, and a negative
	map-center coordinate exercising the `xCenter/blocksPerPixel` truncating
	division part 1's own golden does not cover (review round 1, Finding 2).
*/

#include <assert.h>
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
#include "chunk.h"
#include "item/inventory.h"
#include "item/items.h"
#include "item/items_object.h"
#include "item/items/item_map.h"
#include "item/recipe.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/map_data.h"
#include "network/map_data_archive.h"
#include "network/map_idcounts.h"
#include "network/server_local.h"
#include "network/server_interface.h"
#include "network/server_world.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "util.h"
#include "worldgen/jmath.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_map_item_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
static struct server_local s;

static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

// ============================================================================
// 1. Crafting recipe shape/output [740c583: CraftingManager.java:73]
// ============================================================================
static void test_recipe_shape(void) {
	struct item_data slots[9];
	bool empty[9];
	for(int i = 0; i < 9; i++) {
		slots[i] = (struct item_data) {.id = ITEM_PAPER};
		empty[i] = false;
	}
	slots[4] = (struct item_data) {.id = ITEM_COMPASS}; // center

	struct item_data out = {0};
	assert(recipe_match(recipes_crafting, slots, empty, &out));
	assert(out.id == ITEM_MAP && out.count == 1);

	// Negative control: compass moved off-center must NOT match.
	slots[4] = (struct item_data) {.id = ITEM_PAPER};
	slots[0] = (struct item_data) {.id = ITEM_COMPASS};
	out = (struct item_data) {0};
	bool m = recipe_match(recipes_crafting, slots, empty, &out);
	assert(!m || out.id != ITEM_MAP);

	printf("  recipe: 8 paper ring + compass center -> 1x ITEM_MAP OK\n");
}

// ============================================================================
// 2. onCreated [740c583: ItemMap.onCreated] + idcounts persistence
// ============================================================================
static void test_on_created(void) {
	// Non-integer player position to pin floor_double (not truncation).
	s.player.x = 10.7;
	s.player.y = 64.0;
	s.player.z = -3.2; // floor(-3.2) == -4, NOT -3 (truncation would give -3)
	s.player.dimension = WORLD_DIM_OVERWORLD;

	struct item_data stack = {.id = ITEM_MAP, .durability = 0, .count = 1};
	assert(item_map.onCreated(&s, &s.player, &stack)); // durable alloc must succeed

	// [740c583] `var2.getUniqueDataId("map")` -- FIRST call for a never-used
	// key returns 0 in Beta; maintainer reserves zero here, so first is 1.
	assert(stack.durability == 1);

	struct map_data* md = item_map_cache_resident(&s, 1);
	assert(md != NULL);
	// [740c583] xCenter=floor(posX), zCenter=floor(posZ), scale=3,
	// dimension=(byte)worldType.
	assert(md->x_center == 10);
	assert(md->z_center == -4);
	assert(md->scale == 3);
	assert(md->dimension == WORLD_DIM_OVERWORLD);
	// A fresh 16384-byte colors buffer is all-zero (map_data_set_defaults'
	// own memset, part 1).
	for(int i = 0; i < MAP_DATA_COLORS_SIZE; i++)
		assert(md->colors[i] == 0);

	// Persistence: onCreated's markDirty() means the flush boundary (not
	// onCreated itself) is what reaches disk -- exercise that boundary
	// directly and confirm the file round-trips.
	item_map_cache_flush_all(&s);
	struct map_data loaded;
	assert(map_data_archive_load(s.level_name, 1, &loaded));
	assert(loaded.x_center == 10 && loaded.z_center == -4
		   && loaded.scale == 3 && loaded.dimension == WORLD_DIM_OVERWORLD);

	// idcounts increments AND persists: a second onCreated call gets id 2,
	// and a THIRD direct call confirms the counter file itself advanced
	// (not merely an in-process counter) -- map_idcounts_get_unique_id
	// re-reads idcounts.dat from disk on every call.
	struct item_data stack2 = {.id = ITEM_MAP, .durability = 0, .count = 1};
	assert(item_map.onCreated(&s, &s.player, &stack2)); // durable alloc must succeed
	assert(stack2.durability == 2);

	int32_t third;
	assert(map_idcounts_get_unique_id(s.level_name, "map", &third));
	assert(third == 3);

	printf("  onCreated: id=1 damage/center(10,-4)/scale=3/dim=OW seeded, "
		   "flush->disk round-trip OK, idcounts 1->2->3 OK\n");
}

// ============================================================================
// 3. getMapData fallback [740c583: ItemMap.getMapData] -- spawn-centered,
//    NOT player-centered (the deliberate difference from onCreated).
// ============================================================================
static void test_get_map_data_fallback(void) {
	s.player.spawn_x = 100;
	s.player.spawn_y = 70;
	s.player.spawn_z = -200;
	s.player.dimension = WORLD_DIM_OVERWORLD;
	// Player position deliberately DIFFERENT from spawn, so a pass/fail on
	// "which center got used" is unambiguous.
	s.player.x = 10.7;
	s.player.z = -3.2;

	// durability=999 was never allocated by onCreated/idcounts -- its
	// backing file cannot exist, forcing getMapData's fallback branch.
	struct item_data stack = {.id = ITEM_MAP, .durability = 999, .count = 1};
	// is_held=false: getMapData's lookup/fallback runs regardless (Beta
	// gates only the updateMapData SCAN on isCurrentItem, not the lookup
	// itself) -- see item_map.h's onUpdate doc comment.
	item_map.onUpdate(&s, &stack, 0, false);

	// [740c583] fallback allocates the NEXT idcounts id (this world's
	// next ID is 4 after test_on_created's three draws: 1,2,3).
	assert(stack.durability == 4);
	struct map_data* md = item_map_cache_resident(&s, 4);
	assert(md != NULL);
	assert(md->x_center == s.player.spawn_x);
	assert(md->z_center == s.player.spawn_z);
	assert(md->scale == 3);
	assert(md->dimension == WORLD_DIM_OVERWORLD);
	// NOT player-centered -- the point of this test.
	assert(md->x_center != 10 && md->z_center != -4);

	printf("  getMapData fallback: absent id -> fresh id=4, SPAWN-centered "
		   "(100,-200), not player-centered OK\n");
}

// ============================================================================
// 4. Colour-scan core [740c583: ItemMap.updateMapData] -- synthetic world
//    column, hand-computed expected colors[] byte, AND the stripe gate's
//    full 16-tick sweep (both pinned by the SAME setup: only step%16==0
//    reaches pixel 64, so a correct byte at colors[64] after exactly 16
//    calls proves the gate cycled through every residue).
// ============================================================================
//
// Setup: scale=0 (blocks_per_pixel=1, so 1 pixel == 1 block column --
// chosen for hand-tractability, NOT a claim about real gameplay defaults,
// which stay scale=3 per onCreated/getMapData above). x_center=0,
// z_center=0. Player position is chosen so pixel_center = (64, 0) exactly:
//   map_data_scan_pixel_center(entity_x=0, x_center=0, bpp=1)
//     = floor(0-0)/1 + 64 = 64
//   map_data_scan_pixel_center(entity_z=-64, z_center=0, bpp=1)
//     = floor(-64-0)/1 + 64 = 0
// Target pixel (px=64, pz=0): dx=0, dz=0 -- dead center, radius=128 (has_
// no_sky=false), so dx^2+dz^2=0 is inside BOTH the (radius-2)^2 edge-band
// test (0 > 126^2 is false -> not in the edge band) and the radius^2 circle
// test (0 < 128^2) -- the write gate passes unconditionally, no dither
// parity dependence.
//
// world_x for px=64: (x_center/bpp + px - 64)*bpp = (0 + 64 - 64)*1 = 0.
// The pz loop's bounds check (`pz >= -1`) means pz=-1 is the FIRST row
// that actually executes for this px column (everything below is skipped
// by `continue` before touching prev_avg_height) -- so only TWO world_z
// columns matter: pz=-1 -> world_z = (0 + (-1) - 64)*1 = -65 (the priming
// row, never WRITTEN but seeds prev_avg_height), and pz=0 (target) ->
// world_z = (0 + 0 - 64)*1 = -64.
//
// Both (x=0,z=-65) and (x=0,z=-64) get the IDENTICAL synthetic column:
// bedrock y=0, stone y=1..2, dirt y=3, grass y=4, nothing above.
//   server_world_get_height returns "top opaque + 1" = 4+1 = 5.
//   y = height + 1 = 6 [740c583: `getHeightValue(...) + 1`, the SECOND +1
//       item_map_update_data's own comment explains].
//   Descent: read(y-1=5) = air(0) -> found=false -> y-- (=5); re-read
//     (y-1=4) = grass (still found=false from THIS iteration; loop
//     continues). Next iteration: read(y-1=4) = grass, non-air,
//     non-air-colored -> found=true; grass is not liquid -> done.
//   Final: y=5, block_id=BLOCK_GRASS. avg_height = 5/(1*1) = 5.0 for BOTH
//   pz=-1 and pz=0 (identical terrain).
//
// pz=-1 (priming, not written): prev_avg_height starts 0.0 for the first
//   executed row of this column; after this row, prev_avg_height := 5.0
//   (Beta's `var15 = var31;` runs unconditionally, before the write gate).
// pz=0 (target): avg_height=5.0, prev_avg_height=5.0 (from pz=-1) ->
//   shade = (5.0-5.0)*4/(1+4) + ((64+0)&1 - 0.5)*0.4
//         = 0 + (0 - 0.5)*0.4          [64&1 == 0, even]
//         = -0.2
//   -0.2 is neither > 0.6 nor < -0.6 -> shade_band = 1 (default).
//   histogram = {BLOCK_GRASS: 1} (bpp=1, so exactly one sample) ->
//     best_id = BLOCK_GRASS. color_index = item_map_color_index_for_block
//     (BLOCK_GRASS) = MC_GRASS = 1 (not MC_WATER, no water override).
//   colors[px + pz*128] = colors[64 + 0] = colors[64]
//     = color_index*4 + shade_band = 1*4 + 1 = 5.
static void place_grass_column(w_coord_t x, w_coord_t z) {
	// server_world_set_block's return is "did the id/metadata actually
	// CHANGE" (Beta's setBlockAndMetadataWithNotify=false-on-identical
	// contract) -- NOT "did the write succeed": a `false` here can
	// legitimately mean "already exactly this value" (the superflat
	// generator's own 4-layer stack is bedrock/dirt/dirt/grass at y=0..3,
	// so y=0's bedrock write below is expected to report no change). Don't
	// assert the return; verify the FINAL state directly afterward
	// instead.
	server_world_set_block(&s.worlds[0], x, 0, z,
						   (struct block_data) {.type = BLOCK_BEDROCK});
	server_world_set_block(&s.worlds[0], x, 1, z,
						   (struct block_data) {.type = BLOCK_STONE});
	server_world_set_block(&s.worlds[0], x, 2, z,
						   (struct block_data) {.type = BLOCK_STONE});
	server_world_set_block(&s.worlds[0], x, 3, z,
						   (struct block_data) {.type = BLOCK_DIRT});
	server_world_set_block(&s.worlds[0], x, 4, z,
						   (struct block_data) {.type = BLOCK_GRASS});
	// Defensive: superflat's own stack tops out at y=3 (grass), so y>=4
	// was already air before the write above -- clear a little further up
	// too in case that ever changes.
	for(w_coord_t y = 5; y < 8; y++)
		server_world_set_block(&s.worlds[0], x, y, z,
							   (struct block_data) {.type = BLOCK_AIR});

	struct block_data blk;
	assert(server_world_get_block(&s.worlds[0], x, 0, z, &blk)
		   && blk.type == BLOCK_BEDROCK);
	assert(server_world_get_block(&s.worlds[0], x, 1, z, &blk)
		   && blk.type == BLOCK_STONE);
	assert(server_world_get_block(&s.worlds[0], x, 2, z, &blk)
		   && blk.type == BLOCK_STONE);
	assert(server_world_get_block(&s.worlds[0], x, 3, z, &blk)
		   && blk.type == BLOCK_DIRT);
	assert(server_world_get_block(&s.worlds[0], x, 4, z, &blk)
		   && blk.type == BLOCK_GRASS);
	// Sanity check on the premise item_map_update_data's own hand-trace
	// comment relies on: top opaque (grass@y=4) + 1 == 5.
	uint8_t height = 0;
	assert(server_world_get_height(&s.worlds[0], x, z, &height) && height == 5);
}

static void test_colour_scan_and_stripe_gate(void) {
	// Chunks (0,-5) and (0,-4) cover x=0,z=-65 and x=0,z=-64 respectively
	// (CHUNK_SIZE=16: floor(-65/16)=-5, floor(-64/16)=-4).
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, -5, &sc) && sc);
	assert(server_world_load_chunk(&s.worlds[0], 0, -4, &sc) && sc);
	place_grass_column(0, -65);
	place_grass_column(0, -64);

	struct map_data md;
	map_data_set_defaults(&md, 0, 0, WORLD_DIM_OVERWORLD);
	md.scale = 0; // override the default 3 -- see this section's header
				 // comment for why scale=0 is chosen for THIS test.

	// Stripe gate: `(px & 15) == (step & 15)`, step incremented BEFORE the
	// gate check. Pixel 64 (64 & 15 == 0) is selected only when
	// step & 15 == 0, i.e. step == 16 -- the 16th call. Fewer than 16
	// calls must leave colors[64] untouched (still the fresh 0); exactly
	// 16 calls must produce the hand-computed byte 5. This is the
	// integration proof that the gate cycles through every residue over a
	// full 16-tick sweep, not merely that map_data_scan_stripe_selected
	// (already unit-pinned in test_mapdata_golden.c) returns the right
	// bool in isolation.
	for(int tick = 1; tick <= 15; tick++) {
		item_map_update_data(&s, &md, /*entity_x=*/0.0, /*entity_z=*/-64.0,
							 WORLD_DIM_OVERWORLD);
		assert(md.colors[64] == 0); // not yet this tick's turn
	}
	bool changed = item_map_update_data(&s, &md, 0.0, -64.0,
										WORLD_DIM_OVERWORLD);
	assert(changed);
	assert(md.step == 16);
	assert(md.colors[64] == 5); // MC_GRASS(1)*4 + shade_band(1)

	// A 17th call re-scans the SAME (now-unchanged) terrain -- no pixel
	// differs from what is already stored, so `any_changed` must be false
	// this time (the "did anything change" contract, not merely "did the
	// gate select a column").
	bool changed_again = item_map_update_data(&s, &md, 0.0, -64.0,
											  WORLD_DIM_OVERWORLD);
	assert(!changed_again);
	assert(md.colors[64] == 5); // unchanged

	printf("  colour scan: grass column -> colors[64]=5 exactly on the "
		   "16th stripe-gated call, unchanged (no-op) on the 17th OK\n");
}

// ============================================================================
// 4b. Colour-scan core at REAL Beta 1.7.3 defaults: scale=3 (the ONLY scale
//     either onCreated or getMapData's fallback ever seeds -- part 1's
//     map_data_set_defaults; scale=0 above was a deliberate hand-tractability
//     simplification, never a claim about real gameplay). blocks_per_pixel=8
//     means EVERY pixel is a genuine 64-sample histogram: this section
//     exercises the majority-vote decision among competing block types (not
//     the trivial single-sample case scale=0 gave) and a nonuniform-depth
//     water pixel, per review round 1's Finding 2.
// ============================================================================
//
// Shared setup: x_center = -3 (negative, NOT a multiple of 8 -- exercises
// [740c583] `var21 = (var7/var6 + var12 - var4/2) * var6`'s `var7/var6`
// term, Java int/int TRUNCATING division: -3/8 = 0, never tested by part 1
// -- test_mapdata_golden.c's own pixel_center cases pin the DIFFERENT
// `floor(entityPos-center)/bpp` division (map_data_scan_pixel_center), not
// this standalone `xCenter/blocksPerPixel` term, so this is new coverage,
// not a duplicate). z_center = 0. Player position chosen so pixel_center =
// (64, 0) exactly, same convenience as the scale=0 section above:
//   map_data_scan_pixel_center(entity_x=0, x_center=-3, bpp=8)
//     = floor(0-(-3))/8 + 64 = floor(3)/8 + 64 = 0 + 64 = 64
//   map_data_scan_pixel_center(entity_z=-512, z_center=0, bpp=8)
//     = floor(-512-0)/8 + 64 = -64 + 64 = 0
// radius = map_data_scan_radius(8, false) = 128/8 = 16.
//
// Two target pixels, both dz=0 (dead center on Z, no edge-band/dither
// complication -- same reasoning as the scale=0 section):
//   px=64 (dx=0):  majority-vote test (grass beats sand).
//   px=72 (dx=8):  water-depth test (nonuniform depth).
// Both pass the write gate unconditionally: dx^2+dz^2 (0 or 64) is well
// inside radius^2=256 and outside the (radius-2)^2=196 edge band either way.
//
// world_x per pixel: (x_center/8 + px - 64)*8, x_center/8 = -3/8 = 0
// (truncating) for BOTH pixels:
//   px=64: (0 + 64-64)*8 = 0    -> samples x=0..7
//   px=72: (0 + 72-64)*8 = 64   -> samples x=64..71
// world_z: same for both pixels (z depends only on pz, not px):
//   pz=-1 (priming, world_z=(0 + -1-64)*8 = -520) -> samples z=-520..-513
//   pz=0  (target,  world_z=(0 +  0-64)*8 = -512) -> samples z=-512..-505
//
// Both pixels' priming rows (pz=-1) are UNIFORM grass at height 5 (same
// per-column trace as the scale=0 section: get_height=5, y=6 initial,
// descent finds grass at y=4 after one decrement, final y=5), so BOTH
// pixels' avg_height_priming = 5.0 exactly -> prev_avg_height=5.0 entering
// their own target rows (pz=0). Chunks needed: (cx=0,cz=-33)+(cx=0,cz=-32)
// for px=64's X band, (cx=4,cz=-33)+(cx=4,cz=-32) for px=72's X band
// (CHUNK_SIZE=16: floor(-520/16)=floor(-513/16)=-33, floor(-512/16)=-32).

static void place_uniform_column(w_coord_t x, w_coord_t z, uint16_t top) {
	server_world_set_block(&s.worlds[0], x, 0, z,
						   (struct block_data) {.type = BLOCK_BEDROCK});
	server_world_set_block(&s.worlds[0], x, 1, z,
						   (struct block_data) {.type = BLOCK_STONE});
	server_world_set_block(&s.worlds[0], x, 2, z,
						   (struct block_data) {.type = BLOCK_STONE});
	server_world_set_block(&s.worlds[0], x, 3, z,
						   (struct block_data) {.type = BLOCK_DIRT});
	server_world_set_block(&s.worlds[0], x, 4, z, (struct block_data) {.type = top});
	for(w_coord_t y = 5; y < 8; y++)
		server_world_set_block(&s.worlds[0], x, y, z,
							   (struct block_data) {.type = BLOCK_AIR});
}

// bedrock@0, stone@1, `depth` water blocks starting at y=2 (top surface
// always at y = 2+depth-1, i.e. y=1+depth), air above.
static void place_water_column(w_coord_t x, w_coord_t z, int depth) {
	server_world_set_block(&s.worlds[0], x, 0, z,
						   (struct block_data) {.type = BLOCK_BEDROCK});
	server_world_set_block(&s.worlds[0], x, 1, z,
						   (struct block_data) {.type = BLOCK_STONE});
	for(int i = 0; i < depth; i++)
		server_world_set_block(
			&s.worlds[0], x, 2 + i, z,
			(struct block_data) {.type = BLOCK_WATER_STILL});
	for(w_coord_t y = 2 + depth; y < 2 + depth + 3; y++)
		server_world_set_block(&s.worlds[0], x, y, z,
							   (struct block_data) {.type = BLOCK_AIR});
}

static void test_colour_scan_scale3(void) {
	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, -33, &sc) && sc);
	assert(server_world_load_chunk(&s.worlds[0], 0, -32, &sc) && sc);
	assert(server_world_load_chunk(&s.worlds[0], 4, -33, &sc) && sc);
	assert(server_world_load_chunk(&s.worlds[0], 4, -32, &sc) && sc);

	// ---- px=64 priming row: uniform grass, x=0..7, z=-520..-513 ----------
	for(w_coord_t x = 0; x < 8; x++)
		for(w_coord_t z = -520; z < -512; z++)
			place_uniform_column(x, z, BLOCK_GRASS);

	// ---- px=64 target row: 48 grass (sz=0..5) + 16 sand (sz=6,7) ---------
	// [740c583] histogram: {BLOCK_GRASS: 48, BLOCK_SAND: 16} -> best_id =
	// BLOCK_GRASS (48 > 16, the winner-take-all `if (var26[var35] > var33)`
	// loop). avg_height = 5.0 for every one of the 64 samples (grass and
	// sand columns share the identical height=4 top -> get_height=5, y
	// final after descent = 5), matching the priming row's 5.0 exactly, so
	// the brightness term is 0.
	for(w_coord_t x = 0; x < 8; x++) {
		for(w_coord_t sz = 0; sz < 8; sz++) {
			w_coord_t z = -512 + sz;
			place_uniform_column(x, z, sz < 6 ? BLOCK_GRASS : BLOCK_SAND);
		}
	}

	// ---- px=72 priming row: uniform grass, x=64..71, z=-520..-513 --------
	for(w_coord_t x = 64; x < 72; x++)
		for(w_coord_t z = -520; z < -512; z++)
			place_uniform_column(x, z, BLOCK_GRASS);

	// ---- px=72 target row: water, NONUNIFORM depth, x=64..71 -------------
	// Surface height held CONSTANT (both depths reach y=... differently, see
	// below) is deliberately NOT constant here -- half the footprint is
	// shallow (1 water block, surface y=2) and half is deep (6 water
	// blocks, surface y=7), so this pixel exercises BOTH the water-depth
	// AVERAGE and a nonzero brightness term at once (see the hand-derived
	// shade arithmetic below the loop).
	//   shallow (sz=0..3, 32 samples): depth=1 water block (y=2 only).
	//     get_height = top-opaque(water@2)+1 = 3. y_initial = 3+1 = 4.
	//     descent: read(y-1=3)=air->found=false->y--(=3),read(y-1=2)=water
	//       (found still false this iter); next iter: read(y-1=2)=water,
	//       found=true, IS liquid -> water walk. Final y=3 (avg contrib).
	//     water walk: wy=y-1=2; pass1: under=read(2)=water,wy--(=1),depth=1,
	//       continue; pass2: under=read(1)=STONE,wy--(=0),depth=2,
	//       wy<=0-> break. water_depth_sample = 2.
	//   deep (sz=4..7, 32 samples): depth=6 water blocks (y=2..7).
	//     get_height = top-opaque(water@7)+1 = 8. y_initial = 8+1 = 9.
	//     descent (same shape as the shallow trace, one decrement then
	//     found): final y = 8.
	//     water walk: wy=y-1=7; reads water@7,6,5,4,3,2 (6 passes,
	//       depth=1..6), 7th pass reads under=STONE at wy=1 (wy-- to 0,
	//       depth=7), wy<=0 -> break. water_depth_sample = 7.
	for(w_coord_t x = 64; x < 72; x++) {
		for(w_coord_t sz = 0; sz < 8; sz++) {
			w_coord_t z = -512 + sz;
			place_water_column(x, z, sz < 4 ? 1 : 6);
		}
	}

	struct map_data md;
	map_data_set_defaults(&md, -3, 0, WORLD_DIM_OVERWORLD);
	md.scale = 3; // the REAL Beta default (map_data_set_defaults already
				 // sets this; restated here for this section's own
				 // documentation clarity, matching the scale=0 section's
				 // style of overriding+annotating the field it cares about).

	// Full 16-tick stripe sweep (same discipline as the scale=0 section):
	// only after tick 16 (step&15==0) is column 64 guaranteed visited, and
	// only after the FULL sweep have BOTH px=64 (64&15=0) and px=72
	// (72&15=8, visited on tick 8) been touched.
	for(int tick = 0; tick < 16; tick++)
		item_map_update_data(&s, &md, 0.0, -512.0, WORLD_DIM_OVERWORLD);

	// ---- px=64 (majority vote): color_index=MC_GRASS(1), shade -----------
	// shade = (avg_height-prev_avg_height)*4/(bpp+4) + ((px+pz)&1-0.5)*0.4
	//       = (5.0-5.0)*4/12 + ((64+0)&1 - 0.5)*0.4
	//       = 0 + (0-0.5)*0.4 = -0.2            [64&1==0, even]
	// -0.2 is neither >0.6 nor <-0.6 -> shade_band=1 (default).
	// colors[64] = color_index*4+shade_band = 1*4+1 = 5.
	assert(md.colors[64] == 5);

	// ---- px=72 (water depth): color_index=MC_WATER(12) -------------------
	// avg_height = (32*3 + 32*8)/64 = (96+256)/64 = 352/64 = 5.5 exactly.
	// water_depth (accumulator) = 32*2 + 32*7 = 64+224 = 288;
	//   288/64 = 4 (integer division, matches map_data.c's own documented
	//   "Java int/int truncating division" convention -- 288/64=4 r32).
	// brightness term = (5.5-5.0)*4/12 = 0.5*4/12 = 0.16666...
	// dither term = ((72+0)&1 - 0.5)*0.4 = (0-0.5)*0.4 = -0.2   [72&1==0]
	// shade (pre-water-override) = 0.16666... - 0.2 = -0.03333...
	//   (neither >0.6 nor <-0.6 -> shade_band would be 1, but the winning
	//   material IS water, so [740c583] OVERRIDES shade/shade_band next:)
	// shade (water override) = water_depth*0.1 + ((px+pz)&1)*0.2
	//                         = 4*0.1 + (72&1)*0.2 = 0.4 + 0 = 0.4
	// 0.4 < 0.5 -> shade_band=2 (the water branch's OWN `if(<0.5){2}`);
	// 0.4 is not >0.9, so shade_band stays 2.
	// colors[72] = color_index*4+shade_band = 12*4+2 = 50.
	assert(md.colors[72] == 50);

	printf("  colour scan (scale=3, 64 samples/pixel): px=64 majority-vote "
		   "grass(48)/sand(16) -> colors[64]=5; px=72 nonuniform water "
		   "depth(2,7 avg->4) -> colors[72]=50 OK\n");
}

// ============================================================================
// 5. Dimension-mismatch no-op guard [740c583: `if (worldProvider.worldType
//    == mapData.dimension)`]
// ============================================================================
static void test_dimension_mismatch_noop(void) {
	struct map_data md;
	map_data_set_defaults(&md, 0, 0, WORLD_DIM_NETHER); // md.dimension = -1
	uint32_t step_before = (uint32_t)md.step;

	bool changed = item_map_update_data(&s, &md, 0.0, -64.0,
										WORLD_DIM_OVERWORLD); // mismatch
	assert(!changed);
	// step is INSIDE the dimension-guarded block in Beta [740c583:
	// `if (dim==...) { ...; ++var3.step; ... }`] -- a mismatch must not
	// even increment it.
	assert((uint32_t)md.step == step_before);
	for(int i = 0; i < MAP_DATA_COLORS_SIZE; i++)
		assert(md.colors[i] == 0); // untouched

	printf("  dimension mismatch: NETHER map data vs OVERWORLD update -> "
		   "true no-op (colors AND step both untouched) OK\n");
}

// ============================================================================
// 6. Block -> MapColor spot checks [740c583, full chain cited in
//    item_map.c's table header] -- a handful of the >=15 ids cited there,
//    re-checked here as executable assertions (not merely comments).
// ============================================================================
static void test_block_color_spot_checks(void) {
	assert(item_map_color_index_for_block(BLOCK_STONE) == 11);
	assert(item_map_color_index_for_block(BLOCK_GRASS) == 1);
	assert(item_map_color_index_for_block(BLOCK_WATER_STILL) == 12);
	assert(item_map_color_index_for_block(BLOCK_LAVA_STILL) == 4); // shares TNT's colour
	assert(item_map_color_index_for_block(BLOCK_SAND) == 2);
	assert(item_map_color_index_for_block(BLOCK_GRAVEL) == 2); // NOT dirt
	assert(item_map_color_index_for_block(BLOCK_LOG) == 13);
	assert(item_map_color_index_for_block(BLOCK_LEAVES) == 7);
	assert(item_map_color_index_for_block(BLOCK_GLASS) == 0); // airColor
	assert(item_map_color_index_for_block(BLOCK_GOLD_CAST) == 6);
	assert(item_map_color_index_for_block(BLOCK_TNT) == 4);
	assert(item_map_color_index_for_block(BLOCK_OBSIDIAN) == 11); // plain stone
	assert(item_map_color_index_for_block(BLOCK_REDSTONE_WIRE) == 0);
	assert(item_map_color_index_for_block(BLOCK_SNOW) == 8);
	assert(item_map_color_index_for_block(BLOCK_SNOW_BLOCK) == 8);
	assert(item_map_color_index_for_block(BLOCK_ICE) == 5);
	assert(item_map_color_index_for_block(BLOCK_SOULSAND) == 2); // sandColor

	// See-through (colorIndex 0 == MapColor.airColor) spot checks -- these
	// are the ids updateMapData's descent loop treats as transparent and
	// keeps scanning DOWN through (item_map_update_data's descent block).
	// [740c583] chain for each, re-verified per review Finding 3 (the
	// descent's condition is on materialMapColor == airColor, NOT id==0 --
	// colorIndex==0 is the correct C stand-in because MapColor.airColor is
	// the UNIQUE MapColor instance at index 0 (MapColor.java's own static
	// initializer: `airColor = new MapColor(0, 0)` is the only `(0, ...)`
	// literal), so "colorIndex==0" and "is the airColor object" are the
	// same predicate -- not merely a coincidental stand-in):
	assert(item_map_color_index_for_block(BLOCK_GLASS) == 0); // airColor
	assert(item_map_color_index_for_block(BLOCK_REDSTONE_WIRE) == 0);
	// BlockTorch(50,80) -> super(id,tex,Material.circuits) [beta.sh
	// BlockTorch:7] -> circuits=MaterialLogic(airColor) [Material.java:124]
	assert(item_map_color_index_for_block(BLOCK_TORCH) == 0);
	// BlockRail(66,128,false) -> super(id,tex,Material.circuits) [beta.sh
	// BlockRail:18] -- same Material.circuits chain as torch/wire above.
	assert(item_map_color_index_for_block(BLOCK_RAIL) == 0);
	assert(item_map_color_index_for_block(BLOCK_POWERED_RAIL) == 0);

	// Unregistered-id default: Beta 1.7.3 tops out at id 96 (BLOCK_TRAP_
	// DOOR); id 150 has no Beta 1.7.3 block, and this port's own NET-NEW
	// BLOCK_IRON_CHEST=200 relocation (FH-07) is likewise not a Beta id --
	// both must fall back to the C array's implicit 0 (== MC_AIR ==
	// airColor), never an uninitialized/garbage read.
	assert(item_map_color_index_for_block(150) == 0);
	assert(item_map_color_index_for_block(BLOCK_IRON_CHEST) == 0);
	assert(item_map_color_index_for_block(BLOCK_AIR) == 0); // id 0 itself

	// Encoded RGB pairs (2b render, sanity-checked here) -- cloth and iron
	// share an RGB despite distinct indices.
	assert(item_map_color_rgb_for_index(3) == 10987431);  // clothColor
	assert(item_map_color_rgb_for_index(6) == 10987431);  // ironColor
	assert(item_map_color_rgb_for_index(11) == 7368816);  // stoneColor

	printf("  block->MapColor spot checks: 20 ids (incl. 5 see-through + "
		   "3 unregistered-default) + 3 RGB pairs OK\n");
}

// ============================================================================
// 7. Write-failure durability contract [map_idcounts.h/.c] -- REGRESSION for
//    the id-reuse-on-persistence-failure defect: the old
//    map_idcounts_get_unique_id discarded map_idcounts_write_entries's
//    return value and handed back the in-memory `new_value` regardless, so a
//    transient write failure (full disk, permissions, a torn temp-rename)
//    left idcounts.dat stale on disk while the caller believed an id was
//    reserved -- the NEXT call (for a DIFFERENT, unrelated map) would
//    re-derive that SAME id from the still-stale table and collide with it,
//    both maps sharing one cache/archive record and overwriting each other.
//    The fix: propagate failure (bool return + out-param) all the way to
//    item_map_on_created (item_map.c:~671/682). This exercises BOTH layers:
//    the raw map_idcounts_get_unique_id primitive, and item_map.onCreated's
//    own caller-side handling of a failure.
// ============================================================================
static char s_fixture_TMP_WORLD_FAILURE[128]; /* was #define TMP_WORLD_FAILURE='test_map_item_failure_tmp' (a literal) */
#define TMP_WORLD_FAILURE s_fixture_TMP_WORLD_FAILURE
static void test_idcounts_write_failure_no_reuse(void) {
	MKDIR(TMP_WORLD_FAILURE);
	MKDIR(test_fixture_join(TMP_WORLD_FAILURE, "/data"));
	// Repeatable clean-slate fixture (main()'s own established precedent
	// below): a prior local run must not leave this world's counter
	// advanced past what this test assumes.
	remove(test_fixture_join(TMP_WORLD_FAILURE, "/data/idcounts.dat"));
	remove(test_fixture_join(TMP_WORLD_FAILURE, "/data/idcounts.dat_new"));

	string_t level;
	string_init_set_str(level, TMP_WORLD_FAILURE);

	// A normal, healthy first allocation durably reserves id 1.
	int32_t id0;
	assert(map_idcounts_get_unique_id(level, "map", &id0));
	assert(id0 == 1);

	// Arm the one-shot write-failure injection (map_idcounts.h's test-only
	// seam, mirrors map_data_archive.c/region_archive.c's identical
	// `_test_fail_next` idiom), then attempt a SECOND allocation for the
	// SAME key. It must fail closed: false, and *out_id left untouched --
	// NOT the in-memory `new_value` handed out anyway (the bug).
	map_idcounts_test_fail_next(MAP_IDCOUNTS_TEST_FAIL_WRITE);
	int32_t id_failed = -12345; // sentinel: must survive untouched on failure
	assert(!map_idcounts_get_unique_id(level, "map", &id_failed));
	assert(id_failed == -12345
		   && "a failed allocation must not write *out_id at all");

	// The on-disk table must be EXACTLY as it was before the failed call --
	// proven two ways: (a) a fresh, healthy call recomputes the SAME next id
	// (1) the failed call would have handed out (no id silently SKIPPED),
	// and (b) the call after THAT gets a genuinely new id (2), never
	// repeating 0 or 1 (no id silently REUSED/shared between two callers --
	// the exact defect this fix closes).
	int32_t id1;
	assert(map_idcounts_get_unique_id(level, "map", &id1));
	assert(id1 == 2 && "a failed write must not skip an id");
	int32_t id2;
	assert(map_idcounts_get_unique_id(level, "map", &id2));
	assert(id2 == 3 && "a failed write must not let a later id repeat");

	string_clear(level);

	// Layer 2: item_map_on_created (item_map.c) itself, not just the raw
	// primitive -- confirm the item_map.c:671/682 caller-side fix. A `static`
	// local (not a stack local) for the same THREAD_STACK_SIZE-budget reason
	// item_map.c's own onCreated comment documents (struct server_local is
	// large); this test's main thread has no such constraint, but matching
	// the production shape costs nothing and avoids setting a bad precedent.
	static struct server_local mini_s;
	memset(&mini_s, 0, sizeof(mini_s));
	MKDIR(test_fixture_join(TMP_WORLD_FAILURE, "_item"));
	MKDIR(test_fixture_join(TMP_WORLD_FAILURE, "_item/data"));
	remove(test_fixture_join(TMP_WORLD_FAILURE, "_item/data/idcounts.dat"));
	remove(test_fixture_join(TMP_WORLD_FAILURE, "_item/data/idcounts.dat_new"));
	string_init_set_str(mini_s.level_name, test_fixture_join(TMP_WORLD_FAILURE, "_item"));
	mini_s.player.x = 0.0;
	mini_s.player.z = 0.0;
	mini_s.player.dimension = WORLD_DIM_OVERWORLD;

	map_idcounts_test_fail_next(MAP_IDCOUNTS_TEST_FAIL_WRITE);
	struct item_data busted_stack
		= {.id = ITEM_MAP, .durability = 777, .count = 1};
	assert(!item_map.onCreated(&mini_s, &mini_s.player, &busted_stack));
	assert(busted_stack.durability == 777
		   && "a failed onCreated must not stamp an unreserved id onto the "
			  "stack -- exactly what the old code's discarded-return-value "
			  "bug did");
	// A follow-up healthy onCreated call on a FRESH stack still gets id 1
	// (the failed attempt never durably reserved anything, so nothing was
	// spent) -- the self-healing retry the module's header comment
	// documents.
	struct item_data ok_stack = {.id = ITEM_MAP, .durability = 0, .count = 1};
	assert(item_map.onCreated(&mini_s, &mini_s.player, &ok_stack));
	assert(ok_stack.durability == 1);

	string_clear(mini_s.level_name);

	printf("  idcounts write-failure: raw primitive 1->[FAIL,no id]->2->3 "
		   "(no skip, no reuse); item_map.onCreated propagates the failure "
		   "(stack left untouched) and self-heals on retry (id 1) OK\n");
}

static bool file_exists(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	fclose(f);
	return true;
}

// ============================================================================
// 8. Atomic-replace durability [map_idcounts.c] -- REGRESSION for the
//    delete-then-rename data-loss window: map_idcounts_write_entries's
//    promoting step was `remove(path); rename(tmp, path)`. A crash/power-cut
//    (or, deterministically here, an injected rename() failure) landing
//    between those two calls used to leave idcounts.dat GONE entirely, not
//    merely one allocation lost -- the next load re-read an EMPTY table and
//    re-issued id 0, overwriting whatever map_0.dat already existed on disk.
//    Mirrors test_mapdata_golden.c's identical "_new" rename-failure-survivor
//    test for map_data_archive.c (the precedent this fix ports into
//    map_idcounts.c). Fixed: the write side never deletes a just-written,
//    fsynced "_new" survivor on a rename failure, and the load side
//    (map_idcounts_load_entries) recovers from "_new" whenever the primary
//    is genuinely absent, self-healing by promoting it onto the primary
//    name.
// ============================================================================
static char s_fixture_TMP_WORLD_RENAME[128]; /* was #define TMP_WORLD_RENAME='test_map_item_rename_tmp' (a literal) */
#define TMP_WORLD_RENAME s_fixture_TMP_WORLD_RENAME
static void test_idcounts_rename_failure_survives(void) {
	MKDIR(TMP_WORLD_RENAME);
	MKDIR(test_fixture_join(TMP_WORLD_RENAME, "/data"));
	remove(test_fixture_join(TMP_WORLD_RENAME, "/data/idcounts.dat"));
	remove(test_fixture_join(TMP_WORLD_RENAME, "/data/idcounts.dat_new"));

	string_t level;
	string_init_set_str(level, TMP_WORLD_RENAME);

	// "save#1": a normal healthy allocation durably commits id 1 to the
	// primary name.
	int32_t id0;
	assert(map_idcounts_get_unique_id(level, "map", &id0));
	assert(id0 == 1);
	assert(file_exists(test_fixture_join(TMP_WORLD_RENAME, "/data/idcounts.dat")));

	// "save#2": write+fsync succeed, remove(path) runs (the primary is now
	// GONE), then the injected fault makes the promoting rename() report
	// failure -- reproducing the exact crash window the old code left
	// uncovered, deterministically instead of via an OS-specific race.
	map_idcounts_test_fail_next(MAP_IDCOUNTS_TEST_FAIL_RENAME);
	int32_t id_failed = -777; // sentinel: must survive untouched on failure
	assert(!map_idcounts_get_unique_id(level, "map", &id_failed));
	assert(id_failed == -777);

	// Under the PRE-fix code, this is the data-loss moment: the primary is
	// gone AND load had no fallback to the survivor, so the very next call
	// would recompute id 0 from an EMPTY table -- silently colliding with the
	// already-existing map_0.dat from save#1. The fix: the "_new" survivor
	// from save#2's completed write (which already recorded "map" -> 1, the
	// allocation that never got reported back to any caller) must still be on
	// disk right now, and the next call must recover FROM it, continuing the
	// count from 1 (next allocation 2) rather than restarting from 0.
	assert(file_exists(test_fixture_join(TMP_WORLD_RENAME, "/data/idcounts.dat_new")));
	int32_t id1;
	assert(map_idcounts_get_unique_id(level, "map", &id1));
	assert(id1 == 3
		   && "a rename-failure survivor must be recovered and continued "
			  "from, not silently dropped back to id 0 (map_0.dat collision)");

	// Recovery also self-heals: the "_new" survivor must have been promoted
	// onto the primary name (then immediately superseded by id1's own
	// durable write) -- not left recoverable-but-parked forever (matches
	// map_data_archive_load's identical promotion behaviour).
	assert(file_exists(test_fixture_join(TMP_WORLD_RENAME, "/data/idcounts.dat")));
	assert(!file_exists(test_fixture_join(TMP_WORLD_RENAME, "/data/idcounts.dat_new")));

	// One more healthy call proves the table keeps advancing normally from
	// the recovered state (no skip, no repeat).
	int32_t id2;
	assert(map_idcounts_get_unique_id(level, "map", &id2));
	assert(id2 == 4);

	string_clear(level);
	printf("  idcounts rename-failure: '_new' survivor recovered and "
		   "continued from on the next load (id 3, not a data-loss reset to "
		   "id 0) and self-heal promoted onto the primary name OK\n");
}

// A failed dirty-victim save must leave the resident record available for a
// later eviction retry. Exercise each caller that can install a fifth map.
static char s_fixture_TMP_WORLD_EVICTION[128];
static struct server_local eviction_s;
extern unsigned item_map_test_victim_save_attempts(void);
static void test_failed_dirty_eviction(void) {
	static struct map_data record;
	static struct map_data loaded;
	for(int path = 0; path < 3; path++) {
		char world[192];
		char data_dir[208];
		snprintf(world, sizeof world, "%s/case_%d",
				 s_fixture_TMP_WORLD_EVICTION, path);
		snprintf(data_dir, sizeof data_dir, "%s/data", world);
		assert(MKDIR(world) == 0);
		assert(MKDIR(data_dir) == 0);
		memset(&eviction_s, 0, sizeof eviction_s);
		for(int i = 0; i < MAX_MAP_CACHE; i++)
			eviction_s.map_cache_id[i] = INT32_MIN;
		string_init_set_str(eviction_s.level_name, world);
		eviction_s.player.dimension = WORLD_DIM_OVERWORLD;
		eviction_s.player.spawn_x = 32;
		eviction_s.player.spawn_z = -16;

		for(int i = 0; i < MAX_MAP_CACHE; i++) {
			map_data_set_defaults(&record, i, -i, WORLD_DIM_OVERWORLD);
			record.colors[0] = (uint8_t)(40 + i);
			record.colors[MAP_DATA_COLORS_SIZE - 1] = (uint8_t)(90 + i);
			item_map_cache_install(&eviction_s, 100 + i, &record, i == 0);
		}
		const uint32_t victim_touch = eviction_s.map_cache_touch[0];
		const uint32_t touch_counter = eviction_s.map_cache_touch_counter;
		if(path == 1) {
			map_data_set_defaults(&record, 200, -200, WORLD_DIM_OVERWORLD);
			assert(map_data_archive_save(eviction_s.level_name, 200, &record));
		}

		struct item_data stack = {.id = ITEM_MAP,
			.durability = path == 0 ? 777 : path == 1 ? 200 : 999,
			.count = 1};
		const uint16_t original_id = stack.durability;
		map_data_archive_test_fail_next(MAP_DATA_ARCHIVE_TEST_FAIL_RENAME);
		if(path == 0)
			assert(!item_map.onCreated(&eviction_s, &eviction_s.player,
									   &stack));
		else
			item_map.onUpdate(&eviction_s, &stack, 0, false);
		assert(stack.durability == original_id);
		assert(eviction_s.map_cache_touch_counter == touch_counter);
		for(int i = 0; i < MAX_MAP_CACHE; i++) {
			assert(eviction_s.map_cache_id[i] == 100 + i);
			assert(eviction_s.map_cache_data[i].colors[0] == 40 + i);
			assert(eviction_s.map_cache_data[i].colors[MAP_DATA_COLORS_SIZE - 1]
				   == 90 + i);
		}
		assert(eviction_s.map_cache_dirty[0]);
		assert(eviction_s.map_cache_touch[0] == victim_touch);
		if(path == 1) {
			unsigned attempts = item_map_test_victim_save_attempts();
			for(int tick = 1; tick <= 100; tick++) {
				eviction_s.world_time = (uint64_t)tick;
				map_data_archive_test_fail_next(MAP_DATA_ARCHIVE_TEST_FAIL_RENAME);
				item_map.onUpdate(&eviction_s, &stack, 0, false);
			}
			unsigned retries = item_map_test_victim_save_attempts() - attempts;
			printf("  failed dirty eviction: %u save retries over 100 ticks\n", retries);
			assert(retries <= 1 && "dirty-victim save retries must be bounded");
			map_data_archive_test_fail_next(MAP_DATA_ARCHIVE_TEST_FAIL_NONE);
		}

		// The same request can retry once persistence works again. The
		// successful eviction must write the original victim pixels.
		if(path == 0)
			assert(item_map.onCreated(&eviction_s, &eviction_s.player,
									  &stack));
		else {
			eviction_s.world_time += 100;
			item_map.onUpdate(&eviction_s, &stack, 0, false);
		}
		assert(map_data_archive_load(eviction_s.level_name, 100, &loaded));
		assert(loaded.colors[0] == 40);
		assert(loaded.colors[MAP_DATA_COLORS_SIZE - 1] == 90);
		string_clear(eviction_s.level_name);
	}
	printf("  failed dirty eviction: create/load/fallback preserve victim and "
		   "stack, then retry persists original pixels OK\n");
}

/* R2: production inventory dispatch, frozen against bdbd7cbe4. Hash
 * logical fields and exact payload bytes (never struct padding). --sp-bytes
 * writes the same trace for an independently built historical replay. */
static uint64_t sp_hash;
static FILE* sp_trace;
static void sp_bytes(const void* ptr, size_t n) {
	const uint8_t* bytes = ptr;
	for(size_t i = 0; i < n; i++)
		sp_hash = (sp_hash ^ bytes[i]) * UINT64_C(1099511628211);
	if(sp_trace)
		assert(fwrite(ptr, 1, n, sp_trace) == n);
}
static void sp_observe(const struct client_rpc* call, void* unused) {
	(void)unused;
	if(call->type == CRPC_MAP_MARKERS) {
		uint8_t tag = 1;
		sp_bytes(&tag, 1);
		sp_bytes(&call->payload.map_markers.map_id, sizeof(int32_t));
		sp_bytes(&call->payload.map_markers.length, 1);
		sp_bytes(call->payload.map_markers.bytes, call->payload.map_markers.length);
	} else if(call->type == CRPC_MAP_DATA) {
		uint8_t tag = 2;
		sp_bytes(&tag, 1);
		sp_bytes(&call->payload.map_data.slot->map_id, sizeof(int32_t));
		sp_bytes(call->payload.map_data.slot->colors, MAP_DATA_COLORS_SIZE);
	}
}
static void test_production_sp_bytes(const char* trace_path) {
	sp_hash = UINT64_C(14695981039346656037);
	if(trace_path) {
		sp_trace = fopen(trace_path, "wb");
		assert(sp_trace);
	}
	clin_test_set_rpc_observer(sp_observe, NULL);
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.finished_loading = true;
	memset(s.player.inventory.items, 0, INVENTORY_SIZE * sizeof(struct item_data));
	for(unsigned dimension = 0; dimension < 2; dimension++) {
		s.worlds[0].dimension = s.player.dimension = dimension ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
		static struct map_data fresh;
		map_data_set_defaults(&fresh, -3, 7, s.player.dimension);
		fresh.scale = dimension ? 3 : 0;
		int32_t id = 1200 + dimension;
		assert(item_map_cache_install(&s, id, &fresh, false));
		s.player.inventory.items[INVENTORY_SLOT_HOTBAR] =
			(struct item_data){.id = ITEM_MAP, .count = 1, .durability = (uint16_t)id};
		for(unsigned tick = 0; tick < 32; tick++) {
			s.player.x = -2.5 + (tick % 3);
			s.player.z = 6.5 - (tick % 2);
			s.player.rx = tick * 13.0f;
			struct random_gen rng = s.rand_src;
			server_local_test_tick_item_updates(&s);
			assert(!memcmp(&rng, &s.rand_src, sizeof(rng)));
			clin_drain_discard();
			struct map_data* md = item_map_cache_resident(&s, id);
			sp_bytes(&md->x_center, sizeof(md->x_center));
			sp_bytes(&md->z_center, sizeof(md->z_center));
			sp_bytes(&md->dimension, sizeof(md->dimension));
			sp_bytes(&md->scale, sizeof(md->scale));
			sp_bytes(&md->step, sizeof(md->step));
			sp_bytes(md->colors, sizeof(md->colors));
			sp_bytes(s.player.inventory.items, INVENTORY_SIZE * sizeof(struct item_data));
		}
	}
	/* Missing held identity: getMapData stamps the inventory stack before
	 * this tick's marker and terrain payload (the Java stack is in-place). */
	s.worlds[0].dimension = s.player.dimension = WORLD_DIM_OVERWORLD;
	s.player.inventory.items[INVENTORY_SLOT_HOTBAR] =
		(struct item_data){.id = ITEM_MAP, .count = 1, .durability = (uint16_t)-123};
	server_local_test_tick_item_updates(&s);
	clin_drain_discard();
	sp_bytes(s.player.inventory.items, INVENTORY_SIZE * sizeof(struct item_data));
	clin_test_set_rpc_observer(NULL, NULL);
	if(sp_trace) {
		assert(fclose(sp_trace) == 0);
		sp_trace = NULL;
	}
	printf("production SP trace FNV64=%016llx\n", (unsigned long long)sp_hash);
	/* Independently built bdbd7cbe4 production trace: 1,592,120 bytes,
	 * SHA256 84d50e1ecfa1bd89de4c65995b98c869a2100045a64366fbcd7a79104a0e4285.
	 * Candidate and historical binary traces were compared byte-for-byte. */
	assert(sp_hash == UINT64_C(0x81099e600b7dc26b));
	s.worlds[0].dimension = s.player.dimension = WORLD_DIM_OVERWORLD;
}

/* L214: the same coordinates in two providers have opposite surfaces.
 * Beta ItemMap.func_28011_a: equal adjacent heights -> brightness 1;
 * grass MapColor index 1 -> byte 5; stone index 11 -> byte 45. */
static void test_explicit_world_opposite_terrain(void) {
	static struct server_world other;
	string_t path;
	string_init_set_str(path, TMP_WORLD);
	server_world_create(&other, path, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(path);
	struct server_chunk* c;
	assert(server_world_load_chunk(&other, 0, 0, &c));
	for(unsigned wi = 0; wi < 2; wi++) {
		struct server_world* w = wi ? &other : &s.worlds[0];
		for(int z = 7; z <= 8; z++)
			for(int y = 1; y < 128; y++)
				(void)server_world_set_block_silent(w, 8, y, z,
					(struct block_data){.type = y == 4 ? (wi ? BLOCK_STONE : BLOCK_GRASS)
						: y < 4 ? BLOCK_STONE : BLOCK_AIR});
		static struct map_data md;
		map_data_set_defaults(&md, 8, 8, WORLD_DIM_OVERWORLD);
		md.scale = 0; md.step = 15;
		assert(item_map_update_data_w(w, &md, 8, 8));
		assert(md.colors[64 + 64 * MAP_DATA_DIM] == (wi ? 45 : 5));
	}
	server_world_discard(&other);
	puts("  explicit-world terrain: same coordinates grass=5 / foreign stone=45 OK");
}

int main(int argc, char** argv) {
	test_fixture_dir("test_map_item_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	test_fixture_dir("test_map_item_failure_tmp", s_fixture_TMP_WORLD_FAILURE, sizeof s_fixture_TMP_WORLD_FAILURE);
	test_fixture_dir("test_map_item_rename_tmp", s_fixture_TMP_WORLD_RENAME, sizeof s_fixture_TMP_WORLD_RENAME);
	test_fixture_dir("test_map_item_eviction_tmp", s_fixture_TMP_WORLD_EVICTION,
					 sizeof s_fixture_TMP_WORLD_EVICTION);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	recipe_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/data"));
	// Repeatable clean-slate fixture (test_region_boundary.c's own
	// established precedent for a targeted remove() rather than a
	// recursive rmdir, which has no portable C equivalent): idcounts.dat
	// and every map_<id>.dat this run allocates (ids 0..3, see
	// test_on_created/test_get_map_data_fallback below) must start absent,
	// or a second LOCAL run in the same build dir sees test_on_created's
	// "first-ever allocation is id 0" assumption fail against a idcounts
	// counter a PRIOR run already advanced. CI/fresh-checkout runs never
	// hit this (no prior tmp dir exists), but a repeated local re-run does.
	remove(test_fixture_join(TMP_WORLD, "/data/idcounts.dat"));
	remove(test_fixture_join(TMP_WORLD, "/data/idcounts.dat_new"));
	for(int mid = 0; mid < 4; mid++) {
		/* Was char[64]: fine for the old literal TMP_WORLD, but GCC can no
		 * longer prove no truncation now that TMP_WORLD is a s_fixture_*[128]
		 * runtime buffer (base + "_" + pid) -- widen to match. */
		char path[192];
		snprintf(path, sizeof(path), "%s/data/map_%d.dat", TMP_WORLD, mid);
		remove(path);
		snprintf(path, sizeof(path), "%s/data/map_%d.dat_new", TMP_WORLD,
				mid);
		remove(path);
	}

	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	// map_cache_id's "free slot" sentinel is INT32_MIN (server_local_reset_
	// positional_te_tables' convention, matching chest_pos/furnace_pos/...
	// -- see server_local.h's MAX_MAP_CACHE doc comment); a raw memset(0)
	// would misread slot 0 as "already caching id 0", which IS a
	// realistic id this test allocates. This test does not call
	// server_local_reset_positional_te_tables (a static, non-test-exposed
	// function) so it sets this convention up by hand instead.
	for(int i = 0; i < MAX_MAP_CACHE; i++)
		s.map_cache_id[i] = INT32_MIN;
	string_init_set(s.level_name, level);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);

	struct server_chunk* sc0 = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc0) && sc0);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;

	if(argc == 3 && !strcmp(argv[1], "--sp-bytes")) {
		g_drain_run = 0; thread_join(&drain);
		test_production_sp_bytes(argv[2]);
		string_clear(level);
		test_fixture_dir_cleanup(TMP_WORLD);
		return 0;
	}
	g_drain_run = 0; thread_join(&drain);
	test_production_sp_bytes(NULL);
	for(int i = 0; i < MAX_MAP_CACHE; i++) s.map_cache_id[i] = INT32_MIN;
	memset(s.player.inventory.items, 0, INVENTORY_SIZE * sizeof(struct item_data));
	remove(test_fixture_join(TMP_WORLD, "/data/idcounts.dat"));
	remove(test_fixture_join(TMP_WORLD, "/data/idcounts.dat_new"));
	g_drain_run = 1; assert(thread_create(&drain, drain_entry, NULL, 64));
	printf("test_map_item:\n");
	test_recipe_shape();
	test_on_created();
	test_get_map_data_fallback();
	test_colour_scan_and_stripe_gate();
	test_colour_scan_scale3();
	test_dimension_mismatch_noop();
	test_block_color_spot_checks();
	test_idcounts_write_failure_no_reuse();
	test_idcounts_rename_failure_survives();
	assert(MKDIR(s_fixture_TMP_WORLD_EVICTION) == 0);
	test_failed_dirty_eviction();
	test_explicit_world_opposite_terrain();

	g_drain_run = 0;
	thread_join(&drain);
	string_clear(level);
	printf("PASS test_map_item\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	test_fixture_dir_cleanup(TMP_WORLD_FAILURE);
	test_fixture_dir_cleanup(TMP_WORLD_RENAME);
	test_fixture_dir_cleanup(s_fixture_TMP_WORLD_EVICTION);
	/* test_idcounts_write_failure_no_reuse's own sibling fixture (a separate
	 * "<TMP_WORLD_FAILURE>_item" tree, not a child of TMP_WORLD_FAILURE) --
	 * pre-existing behavior left this one uncleaned even before the fixture
	 * moved to a per-process path; now that every run's name is unique, an
	 * uncleaned sibling would accumulate across runs instead of being
	 * overwritten in place, so it gets the same cleanup here. */
	test_fixture_dir_cleanup(test_fixture_join(TMP_WORLD_FAILURE, "_item"));
	return 0;
}
