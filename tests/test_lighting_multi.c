/*
	The byte-for-byte equivalence guard that lighting.c:168 already CLAIMS exists
	("proven byte-for-byte in tests/test_lighting_multi.c") but never did. P1 batches
	the explosion's per-block lighting into ONE lighting_update_at_blocks flood, so
	this test pins the load-bearing invariant:

	    lighting_update_at_blocks(all N changed cells, ONE flood)
	      ==  N x lighting_update_at_block(cell), applied per-cell in order

	-- a BYTE-IDENTICAL sky+torch light field AND heightmap, for the EXPLOSION dual
	that test_relight does not cover: clustered cells set to AIR (light REMOVAL +
	skylight re-derivation + heightmap DROP), stacked columns, chunk-boundary cells,
	and block-light sources (torch/lava) cleared by the blast.

	Pure unit: links only source/lighting.c + source/stack.c with a local flat
	block_data grid + a minimal blocks[] table (the test_relight harness pattern).

	Arm A reproduces the REAL per-block path exactly: for each changed cell, in order,
	write id -> lighting_heightmap_update -> single-source flood (light accumulates
	across cells). Arm B is the batched path: write ALL ids -> all heightmap updates
	-> ONE multi-source flood. Both must reach the identical terminal field (the
	light equation has a unique fixed point; the heightmap is a pure function of the
	final column). A fuzz loop over 32 seeds is the generality proof.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "lighting.h"

// --- minimal block table (the flood reads can_see_through/opacity/luminance) ----
static struct block b_solid = {.can_see_through = false, .opacity = 15};
static struct block b_leaves = {.can_see_through = true, .opacity = 1};
static struct block b_lava
	= {.can_see_through = false, .opacity = 15, .luminance = 15};
static struct block b_torch
	= {.can_see_through = true, .opacity = 0, .luminance = 14};
struct block* blocks[256] = {0};
static void setup_blocks(void) {
	blocks[BLOCK_STONE] = blocks[BLOCK_DIRT] = blocks[BLOCK_GRASS]
		= blocks[BLOCK_BEDROCK] = blocks[BLOCK_LOG] = &b_solid;
	blocks[BLOCK_LEAVES] = &b_leaves;
	blocks[BLOCK_LAVA_STILL] = blocks[BLOCK_LAVA_FLOW] = &b_lava;
	blocks[BLOCK_TORCH] = &b_torch;
	// BLOCK_AIR (0) stays NULL: the flood treats !blocks[type] as see-through air.
}

void blocks_side_offset(enum side s, int* x, int* y, int* z) {
	static const int off[6][3]
		= {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
	*x = off[s][0];
	*y = off[s][1];
	*z = off[s][2];
}
uint8_t nibble_read(uint8_t* base, size_t idx) {
	return (idx & 1) ? (uint8_t)(base[idx >> 1] >> 4)
					 : (uint8_t)(base[idx >> 1] & 0x0F);
}
void nibble_write(uint8_t* base, size_t idx, uint8_t v) {
	size_t i = idx >> 1;
	if(idx & 1)
		base[i] = (uint8_t)((base[i] & 0x0F) | ((v & 0x0F) << 4));
	else
		base[i] = (uint8_t)((base[i] & 0xF0) | (v & 0x0F));
}

// --- a flat block_data grid. LW must equal CHUNK_SIZE because the real
// lighting_heightmap_update strides the heightmap by CHUNK_SIZE. -----------------
#define LW 16
#define H 128
struct cell {
	uint8_t type, sky, torch, meta;
};
static struct cell gworld[LW * LW * H];
static uint8_t gheight[LW * LW];
static bool g_track_sky_writes;
static uint8_t g_max_sky_write;
static size_t gidx(int x, int y, int z) {
	return (size_t)y + ((size_t)z + (size_t)x * LW) * H;
}
// 5-arg flood callback (blk + height; both may be NULL as existence probes).
static bool g_get(void* u, w_coord_t x, w_coord_t y, w_coord_t z,
				  struct block_data* blk, uint8_t* height) {
	(void)u;
	if(x < 0 || x >= LW || z < 0 || z >= LW || y < 0 || y >= H)
		return false;
	struct cell* c = &gworld[gidx(x, y, z)];
	if(blk) {
		blk->type = c->type;
		blk->metadata = c->meta;
		blk->sky_light = c->sky;
		blk->torch_light = c->torch;
	}
	if(height)
		*height = gheight[x + z * LW];
	return true;
}
static void g_set(void* u, w_coord_t x, w_coord_t y, w_coord_t z, uint8_t light) {
	(void)u;
	if(x < 0 || x >= LW || z < 0 || z >= LW || y < 0 || y >= H)
		return;
	struct cell* c = &gworld[gidx(x, y, z)];
	c->sky = light & 0x0F;
	c->torch = light >> 4;
	if(g_track_sky_writes && c->sky > g_max_sky_write)
		g_max_sky_write = c->sky;
}
// 4-arg heightmap callback (lighting_heightmap_update's get_block: no height arg).
static bool hm_get(void* u, c_coord_t x, w_coord_t y, c_coord_t z,
				   struct block_data* blk) {
	(void)u;
	if(x < 0 || x >= LW || z < 0 || z >= LW || y < 0 || y >= H)
		return false;
	struct cell* c = &gworld[gidx(x, y, z)];
	if(blk) {
		blk->type = c->type;
		blk->metadata = c->meta;
	}
	return true;
}

// Heightmap = 1 + (highest opaque/non-see-through-with-opacity y) per column; 0 if
// none. A pure function of the FINAL grid -- both arms start from this identical map.
static void compute_height_all(void) {
	for(int x = 0; x < LW; x++)
		for(int z = 0; z < LW; z++) {
			int h = 0;
			for(int y = H - 1; y >= 0; y--) {
				uint8_t t = gworld[gidx(x, y, z)].type;
				if(blocks[t]
				   && (!blocks[t]->can_see_through || blocks[t]->opacity > 0)) {
					h = y + 1;
					break;
				}
			}
			gheight[x + z * LW] = (uint8_t)h;
		}
}

// Bring the grid to a lit steady state the explosion can then darken: seed ONE sky
// source per column (the first cell at/above the heightmap, which the flood drives
// to 15 and propagates up/down the connected air) plus every luminous block. This is
// the same modest seed shape the real worldgen relight uses -- NOT a 32k all-cell
// flood (which is far outside anything the engine does and is unnecessary, since the
// equivalence holds for ANY identical starting W0). g_allsrc is over-sized to fit.
static struct world_modification_entry* g_allsrc = NULL;
static void relight_all(void) {
	size_t n = 0;
	for(int x = 0; x < LW; x++)
		for(int z = 0; z < LW; z++) {
			int h = gheight[x + z * LW];
			if(h < H)
				g_allsrc[n++] = (struct world_modification_entry) {
					.x = x, .y = h, .z = z};
			for(int y = 0; y < H; y++) {
				uint8_t t = gworld[gidx(x, y, z)].type;
				if(blocks[t] && blocks[t]->luminance > 0)
					g_allsrc[n++] = (struct world_modification_entry) {
						.x = x, .y = y, .z = z};
			}
		}
	lighting_update_at_blocks(g_allsrc, n, false, g_get, g_set, NULL);
}

struct edit {
	int x, y, z;
	uint8_t type, meta;
};

static struct cell W0[LW * LW * H];
static uint8_t W0h[LW * LW];
static struct cell snapA[LW * LW * H];
static uint8_t snapAh[LW * LW];

// Apply `edits` two ways from the captured W0 and assert byte-identical results.
static void run_case(const struct edit* edits, int n, const char* name) {
	// ---- Arm A: interleaved per-cell (write id -> heightmap -> single flood) ----
	memcpy(gworld, W0, sizeof(gworld));
	memcpy(gheight, W0h, sizeof(gheight));
	for(int i = 0; i < n; i++) {
		struct cell* c = &gworld[gidx(edits[i].x, edits[i].y, edits[i].z)];
		c->type = edits[i].type;
		c->meta = edits[i].meta;
		lighting_heightmap_update(gheight, edits[i].x, edits[i].y, edits[i].z,
								  edits[i].type, hm_get, NULL);
		lighting_update_at_block(
			(struct world_modification_entry) {
				.x = edits[i].x, .y = edits[i].y, .z = edits[i].z},
			false, g_get, g_set, NULL);
	}
	memcpy(snapA, gworld, sizeof(gworld));
	memcpy(snapAh, gheight, sizeof(gheight));

	// ---- Arm B: batched (write ALL ids -> all heightmaps -> ONE flood) ----
	memcpy(gworld, W0, sizeof(gworld));
	memcpy(gheight, W0h, sizeof(gheight));
	struct world_modification_entry* src
		= malloc((size_t)(n > 0 ? n : 1) * sizeof(*src));
	assert(src);
	for(int i = 0; i < n; i++) {
		struct cell* c = &gworld[gidx(edits[i].x, edits[i].y, edits[i].z)];
		c->type = edits[i].type;
		c->meta = edits[i].meta;
		src[i] = (struct world_modification_entry) {
			.x = edits[i].x, .y = edits[i].y, .z = edits[i].z};
	}
	for(int i = 0; i < n; i++)
		lighting_heightmap_update(gheight, edits[i].x, edits[i].y, edits[i].z,
								  edits[i].type, hm_get, NULL);
	lighting_update_at_blocks(src, n, false, g_get, g_set, NULL);
	free(src);

	// ---- Compare the WHOLE field + heightmap, byte for byte ----
	size_t cells = (size_t)LW * LW * H;
	for(size_t i = 0; i < cells; i++) {
		if(gworld[i].sky != snapA[i].sky || gworld[i].torch != snapA[i].torch) {
			size_t y = i % H, rest = i / H, z = rest % LW, x = rest / LW;
			fprintf(stderr,
					"%s: light mismatch at (%zu,%zu,%zu): "
					"batched sky=%u torch=%u vs perblock sky=%u torch=%u\n",
					name, x, y, z, gworld[i].sky, gworld[i].torch, snapA[i].sky,
					snapA[i].torch);
			assert(!"batched light field != per-block light field");
		}
	}
	assert(memcmp(gheight, snapAh, sizeof(gheight)) == 0);
}

// A simple deterministic PRNG so the fuzz is reproducible across platforms.
static uint32_t rng_state;
static uint32_t xr(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}
static int xr_range(int lo, int hi) { // [lo,hi)
	return lo + (int)(xr() % (uint32_t)(hi - lo));
}

int main(void) {
	setup_blocks();
	assert(CHUNK_SIZE == LW); // heightmap stride invariant
	g_allsrc = malloc((size_t)LW * LW * H * sizeof(*g_allsrc));
	assert(g_allsrc);

	// Nether edits ignore skylight entirely. A stale non-zero neighbour must not
	// be propagated into the edited cell, and this tiny repair must stay below
	// the global flood-work ceiling.
	memset(gworld, 0, sizeof(gworld));
	memset(gheight, 0, sizeof(gheight));
	gworld[gidx(9, 64, 8)].sky = 15;
	g_track_sky_writes = true;
	g_max_sky_write = 0;
	uint32_t ceiling_before = lighting_flood_ceiling_hits();
	lighting_update_at_block(
		(struct world_modification_entry) {.x = 8, .y = 64, .z = 8}, true,
		g_get, g_set, NULL);
	g_track_sky_writes = false;
	assert(g_max_sky_write == 0);
	assert(gworld[gidx(9, 64, 8)].sky == 0);
	assert(lighting_flood_ceiling_hits() == ceiling_before);
	printf("test_lighting_multi: Nether edit ignored stale neighbour skylight\n");

	// ===== Structured explosion fixture =====================================
	// Solid stone 0..70, air above; a hollow cavern at y=64..68 (interior) holding
	// a lava cell + a torch so the blast removes BLOCK light too. Surface height 71.
	memset(gworld, 0, sizeof(gworld));
	for(int x = 0; x < LW; x++)
		for(int z = 0; z < LW; z++)
			for(int y = 0; y <= 70; y++)
				gworld[gidx(x, y, z)].type = BLOCK_STONE;
	for(int x = 4; x <= 11; x++)
		for(int z = 4; z <= 11; z++)
			for(int y = 64; y <= 68; y++)
				gworld[gidx(x, y, z)].type = BLOCK_AIR; // cavern
	gworld[gidx(6, 64, 6)].type = BLOCK_LAVA_STILL;
	gworld[gidx(9, 64, 9)].type = BLOCK_TORCH;
	compute_height_all();
	relight_all();
	memcpy(W0, gworld, sizeof(gworld));
	memcpy(W0h, gheight, sizeof(gheight));

	// Sanity: the pre-explosion world is genuinely lit (so removal has something to
	// remove) and the cavern is shadowed under the roof.
	assert(gworld[gidx(6, 64, 6)].torch == 15);       // lava glows
	assert(gworld[gidx(9, 64, 9)].torch == 14);       // torch glows
	assert(gworld[gidx(0, 72, 0)].sky == 15);          // open sky above terrain
	assert(gworld[gidx(6, 66, 6)].sky < 15);           // under the cavern roof: dark

	// A blast: a chunk of the surface + roof cleared to AIR, INCLUDING:
	//  - a stacked column (>=3 cells) at (8, 69..71)  -> heightmap drop + skylight
	//  - an x=0 EDGE column (neighbour out-of-range -> g_get false)  -> boundary
	//  - cells clearing the torch's roof + the lava-adjacent roof  -> block-light reflux
	{
		struct edit e[160];
		int n = 0;
		// surface slab y=69..71 over the cavern footprint (clears the roof)
		for(int x = 5; x <= 10; x++)
			for(int z = 5; z <= 10; z++)
				for(int y = 69; y <= 71; y++)
					e[n++] = (struct edit) {x, y, z, BLOCK_AIR, 0};
		// a deep stacked column at (8,*) down into the cavern (>=3 stacked)
		for(int y = 66; y <= 71; y++)
			e[n++] = (struct edit) {8, y, 8, BLOCK_AIR, 0};
		// an EDGE column at x=0 (left face: x=-1 neighbour is out of range)
		for(int y = 69; y <= 71; y++)
			e[n++] = (struct edit) {0, y, 0, BLOCK_AIR, 0};
		// clear the torch + lava themselves (block-light source REMOVAL)
		e[n++] = (struct edit) {9, 64, 9, BLOCK_AIR, 0};
		e[n++] = (struct edit) {6, 64, 6, BLOCK_AIR, 0};
		assert(n <= 160);
		run_case(e, n, "structured-explosion");

		// Non-vacuous: confirm the change actually did the expected physics in arm A
		// (snapA holds the per-block result), so a passing memcmp isn't both-wrong.
		assert(snapAh[8 + 8 * LW] <= 66);      // column 8,8 height dropped to the cavern
		assert(snapA[gidx(8, 68, 8)].sky == 15); // skylight flooded the opened column
		assert(snapA[gidx(9, 64, 9)].torch < 14); // removed torch -> its cell darkened
	}

	// N==1 degeneracy: a single-cell change, batched(n=1) must equal one single flood.
	{
		struct edit e1[1] = {{7, 71, 7, BLOCK_AIR, 0}};
		run_case(e1, 1, "n==1-degeneracy");
	}

	printf("test_lighting_multi: structured explosion + n==1 byte-identical\n");

	// ===== Fuzz: 32 seeds, random terrain + ~150 random edits ================
	for(int seed = 1; seed <= 32; seed++) {
		rng_state = (uint32_t)seed * 2654435761u + 1u;

		// Random terrain: each column a random opaque-stack height, random scattered
		// see-through (leaves), torches, and lava pockets so light has structure.
		memset(gworld, 0, sizeof(gworld));
		for(int x = 0; x < LW; x++)
			for(int z = 0; z < LW; z++) {
				int top = xr_range(40, 90);
				for(int y = 0; y <= top; y++) {
					int r = (int)(xr() % 16);
					uint8_t t = BLOCK_STONE;
					if(r == 0)
						t = BLOCK_LEAVES;   // see-through opacity 1
					else if(r == 1)
						t = BLOCK_AIR;      // pocket
					else if(r == 2)
						t = BLOCK_LAVA_STILL;
					gworld[gidx(x, y, z)].type = t;
				}
				// occasional torch in the air just above the stack
				if((xr() % 4) == 0)
					gworld[gidx(x, top + 1 + (int)(xr() % 3), z)].type = BLOCK_TORCH;
			}
		compute_height_all();
		relight_all();
		memcpy(W0, gworld, sizeof(gworld));
		memcpy(W0h, gheight, sizeof(gheight));

		// ~150 random edits: a mix of ->AIR (blast) and ->solid/torch/lava (place).
		struct edit e[200];
		int n = xr_range(80, 180);
		for(int i = 0; i < n; i++) {
			int x = (int)(xr() % LW), z = (int)(xr() % LW), y = xr_range(1, H - 1);
			int pick = (int)(xr() % 10);
			uint8_t t = BLOCK_AIR;            // 60% clear (the explosion direction)
			if(pick == 6)
				t = BLOCK_STONE;
			else if(pick == 7)
				t = BLOCK_LEAVES;
			else if(pick == 8)
				t = BLOCK_TORCH;
			else if(pick == 9)
				t = BLOCK_LAVA_STILL;
			e[i] = (struct edit) {x, y, z, t, 0};
		}
		run_case(e, n, "fuzz");
	}
	printf("test_lighting_multi: 32 fuzz seeds byte-identical "
		   "(batched flood == per-block floods, light field + heightmap)\n");

	free(g_allsrc);
	return 0;
}
