/*
	test_far_skin_nether -- proves the Nether far-ring roof exclusion (M6 render-distance
	fix #3, server_world_extract_far_column). The Nether has a solid opaque ROOF (Beta
	func_4058_b replaces randomized top cells with bedrock but leaves netherrack below).
	A naive top-down far-skin scan captures that roof, and world_build_far_section then
	raises a solid bedrock plane across the whole far annulus -- OCCLUDING the far ring
	instead of extending the view (the reported "render distance too short": the ceiling
	skin hid everything past the near chunks).

	The fix: in the Nether, skip the contiguous opaque ceiling shell from the top, then
	skin the netherrack terrain UNDER it (what the player actually sees from below). This
	test mirrors the production per-column predicate EXACTLY (same roof-skip + top non-see-
	through scan) and proves, on a synthetic Nether-shaped chunk:
	  1. the NAIVE (overworld) scan skins the bedrock roof (the occluding-plane bug), and
	  2. the NETHER-aware scan skips the mixed roof and skins the netherrack top instead,
	so the two genuinely differ and the fix removes the occluder. Keep in lockstep with
	server_world_extract_far_column (server_world.c) -- same discipline as
	test_far_skin_equiv / test_far_water_surface.

	Standalone: server_world.c can't link in isolation, so the predicate is replicated.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"

#define WH		 128
#define LIDX(lx, y, lz) ((size_t)(y) + ((size_t)(lz) + (size_t)(lx) * 16) * WH)
#define FAR_NO_SURFACE 0xFF

// Minimal block table: bedrock + netherrack are opaque (enter the skin); nothing else
// is needed (the Nether far skin has no water/flora on the ceiling path).
static struct block b_solid = {.can_see_through = false, .opacity = 15};
struct block* blocks[256] = {0};
static void setup_blocks(void) {
	blocks[BLOCK_BEDROCK] = blocks[BLOCK_NETHERRACK] = &b_solid;
}

// Mirror of server_world_extract_far_column's per-column scan. `nether` selects the
// opaque-ceiling skip. Returns the skin height (or FAR_NO_SURFACE) + the top block id.
static void extract_column(const uint8_t* ids, int x, int z, bool nether,
						   uint8_t* skin_y, uint8_t* top_id) {
	int skin = -1;
	int y_start = WH - 1;
	if(nether)
		while(y_start >= 0) {
			uint8_t id = ids[LIDX(x, y_start, z)];
			if(!blocks[id] || blocks[id]->can_see_through)
				break;
			y_start--;
		}
	for(int y = y_start; y >= 0; y--) {
		uint8_t id = ids[LIDX(x, y, z)];
		if(blocks[id] && !blocks[id]->can_see_through) {
			skin = y;
			break;
		}
	}
	if(skin >= 0) {
		*skin_y = (uint8_t)skin;
		*top_id = ids[LIDX(x, skin, z)];
	} else {
		*skin_y = FAR_NO_SURFACE;
		*top_id = 0;
	}
}

int main(void) {
	setup_blocks();

	uint8_t* ids = calloc((size_t)16 * 16 * WH, 1); // all air
	assert(ids);

	// Build a synthetic Nether-shaped chunk:
	//  - floor bedrock y0..4, netherrack terrain y5..NR_TOP (varies per column),
	//  - a big AIR cavern above the terrain (what the player flies/looks through),
	//  - an 8-block netherrack ceiling shell with randomized bedrock cells near y127.
	const int FLOOR_TOP = 4;	 // bedrock y0..4
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++) {
			// deterministic per-column terrain height 40..71 and mixed opaque roof
			int nr_top = 40 + ((x * 7 + z * 13) & 31);
			int roof_thick = 1 + ((x * 5 + z * 3) % 5);
			for(int y = 0; y <= FLOOR_TOP; y++)
				ids[LIDX(x, y, z)] = BLOCK_BEDROCK; // floor
			for(int y = FLOOR_TOP + 1; y <= nr_top; y++)
				ids[LIDX(x, y, z)] = BLOCK_NETHERRACK; // terrain
			for(int y = 120; y < WH; y++)
				ids[LIDX(x, y, z)] = BLOCK_NETHERRACK; // opaque ceiling body
			for(int y = WH - roof_thick; y < WH; y++)
				ids[LIDX(x, y, z)] = BLOCK_BEDROCK; // randomized bedrock cap
		}

	long roof_naive = 0, netherrack_nether = 0, differ = 0;
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++) {
			uint8_t ny, nid, hy, hid; // naive (overworld) vs hell (nether)
			extract_column(ids, x, z, false, &ny, &nid);
			extract_column(ids, x, z, true, &hy, &hid);

			// 1. The naive scan ALWAYS captures the bedrock roof (the occluder bug):
			//    top block is bedrock, high up in the ceiling band.
			assert(nid == BLOCK_BEDROCK);
			assert(ny >= WH - 5); // within the 1..5-thick roof band
			roof_naive++;

			// 2. The nether-aware scan NEVER returns bedrock roof: it skins netherrack.
			assert(hid == BLOCK_NETHERRACK);
			netherrack_nether++;

			// The skip peels the mixed roof, stops at the air cavern, then the scan
			// lands on the netherrack terrain top (40..71), far below the roof.
			assert(hy <= 71 && hy >= 40);
			if(ny != hy || nid != hid)
				differ++;
		}

	printf("test_far_skin_nether: 256 columns -- naive skins bedrock ROOF (%ld), "
		   "nether-aware skins NETHERRACK (%ld), all differ (%ld)\n",
		   roof_naive, netherrack_nether, differ);
	printf("  Nether far ring skins the terrain under the ceiling, not the occluding "
		   "bedrock roof: PASS\n");
	assert(differ == 256); // the fix changes EVERY column (roof removed everywhere)

	// Edge case: an ALL-OPAQUE mixed column. The Nether skip
	// loop must decrement y_start to -1 WITHOUT an OOB read (short-circuit &&), the scan
	// then finds no non-see-through block below, and the column reports FAR_NO_SURFACE.
	// Cannot occur in real Nether worldgen (worldgen_hell always leaves a netherrack/air
	// middle band) but the production predicate handles it, so lock it in. Done last so
	// it doesn't disturb the 256-column grid above.
	{
		const int ax = 15, az = 15;
		for(int y = 0; y < WH; y++)
			ids[LIDX(ax, y, az)] = (y & 1) ? BLOCK_BEDROCK : BLOCK_NETHERRACK;
		uint8_t ay, aid;
		extract_column(ids, ax, az, true, &ay, &aid);
		assert(ay == FAR_NO_SURFACE && aid == 0); // no skin under an all-bedrock column
		printf("  all-opaque column -> FAR_NO_SURFACE (no OOB, no occluder): PASS\n");
	}
	free(ids);
	return 0;
}
