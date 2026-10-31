/*
	LIGHT-CLIENT-LOSSLESS (hw1023a IMPORT-RCA Finding 2): the client render relight
	must never lose light work.

	On the real Wii (Dream Journal import, far_walk_6m) one client flood reached
	500,224 pops and the paced resume ABANDONED its frontier at the 500k ceiling,
	while stack_push DROPPED every neighbour push whose grow failed on a 4.4 MB
	heap (stk_drop saturated at 65535). Nothing was re-queued, so the light ended
	up wrong. This drives the production world_update_lighting and checks:

	  1. ceiling: a flood far over 500k pops parks, its frontier becomes column
	     repairs, and the final field equals an independent full recompute
	     (oracle below), with every frame inside LIGHTING_CLIENT_POPS_PER_FRAME;
	  2. low memory: every client queue grow fails from mid-flood on (edit queue,
	     flood frontier, seeds), and the final field is STILL the full recompute,
	     with zero stack_push drops;
	  3. root cause: a relight never treats a missing neighbour column as dark.
	     Beta does not relight a cell unless the chunks within one block exist
	     (MetadataChunkBlock.java:43 doChunksNearChunkExist(x, 0, z, 1),
	     World.java:323-327). The client may still RAISE such a cell (an edit
	     packet can carry light 0) but never lowers it, so an edit on a column
	     edge next to an absent column no longer darkens a 15-deep strip.

	The oracle is a plain Gauss-Seidel fixed point of the light equation over a
	local copy of the blocks, written here independently of lighting.c: a cell
	sees the sky at y >= heightmap; otherwise, if see-through, it takes the max
	of its existing neighbours minus max(opacity, 1); luminance for torch light.
	Cells on the outer edge of the resident area touch an absent column: there
	the value may rise but never fall (the root-cause rule above). Cells outside
	the area do not exist.
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "lighting.h"
#include "stack.h"
#include "world.h"

#define R 1				  // resident columns: cx, cz in [-R, R]
#define SPAN ((2 * R + 1) * CHUNK_SIZE)
#define X0 (-R * CHUNK_SIZE)
#define FLOOR_Y 4		  // stone at y <= FLOOR_Y
#define ROOF_Y 40		  // stone roof layer
#define CELLS ((size_t)SPAN * SPAN * WORLD_HEIGHT)

static uint8_t g_id[SPAN][SPAN][WORLD_HEIGHT];
static uint8_t g_sky[SPAN][SPAN][WORLD_HEIGHT], g_torch[SPAN][SPAN][WORLD_HEIGHT];
static uint8_t g_height[SPAN][SPAN];

static bool roof_at(int x, int z) {
	// A roof over the north-west 2/3 of the area: everything under it is lit
	// only sideways from the open part, so the flood must propagate, not just
	// copy the heightmap rule.
	return x < X0 + 2 * CHUNK_SIZE + 4 && z < X0 + 2 * CHUNK_SIZE;
}

static void fixture_blocks(void) {
	for(int i = 0; i < SPAN; i++)
		for(int k = 0; k < SPAN; k++) {
			int x = X0 + i, z = X0 + k;
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				uint8_t id = BLOCK_AIR;
				if(y <= FLOOR_Y || (y == ROOF_Y && roof_at(x, z)))
					id = BLOCK_STONE;
				g_id[i][k][y] = id;
			}
			g_height[i][k] = roof_at(x, z) ? ROOF_Y + 1 : FLOOR_Y + 1;
		}
	// Torches under the roof (luminance 14) close enough to its open edge that
	// the sky flood reaches them (an unlit start is only "one flood away" from
	// its fixed point where the flood can reach); one sits on an inner column
	// seam (x = -1 | 0), so its halo crosses columns.
	g_id[33][8][10] = BLOCK_TORCH;			// x = 17, z = -8: 3 from x = 20
	g_id[CHUNK_SIZE - 1][31][12] = BLOCK_TORCH; // x = -1, z = 15: 1 from z = 16
}

static int op_of(uint8_t id) {
	return blocks[id] ? (blocks[id]->opacity > 1 ? blocks[id]->opacity : 1) : 1;
}
static bool see_through(uint8_t id) {
	return !blocks[id] || blocks[id]->can_see_through;
}
static bool edge_fixed(int i, int k) {
	return i == 0 || k == 0 || i == SPAN - 1 || k == SPAN - 1;
}

/* Fixed point from the CURRENT g_sky/g_torch (fixed cells keep their values). */
static void oracle_settle(void) {
	bool changed = true;
	unsigned sweeps = 0;
	while(changed) {
		changed = false;
		assert(++sweeps < 1000 && "oracle does not converge");
		for(int i = 0; i < SPAN; i++)
			for(int k = 0; k < SPAN; k++) {
				const bool seam = edge_fixed(i, k);
				for(int y = 0; y < WORLD_HEIGHT; y++) {
					uint8_t id = g_id[i][k][y];
					int sky = y >= g_height[i][k] ? 15 : 0;
					int torch = blocks[id] ? blocks[id]->luminance : 0;
					if(see_through(id)) {
						static const int d[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
							{0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
						int op = op_of(id);
						for(int s = 0; s < 6; s++) {
							int a = i + d[s][0], yy = y + d[s][1], b = k + d[s][2];
							if(a < 0 || b < 0 || a >= SPAN || b >= SPAN || yy < 0
							   || yy >= WORLD_HEIGHT)
								continue;
							if(g_sky[a][b][yy] - op > sky) sky = g_sky[a][b][yy] - op;
							if(g_torch[a][b][yy] - op > torch)
								torch = g_torch[a][b][yy] - op;
						}
					}
					if(seam) { // touches an absent column: raise, never lower
						if(sky < g_sky[i][k][y]) sky = g_sky[i][k][y];
						if(torch < g_torch[i][k][y]) torch = g_torch[i][k][y];
					}
					if(sky != g_sky[i][k][y] || torch != g_torch[i][k][y]) {
						g_sky[i][k][y] = (uint8_t)sky;
						g_torch[i][k][y] = (uint8_t)torch;
						changed = true;
					}
				}
			}
	}
}

static void install(struct world* w) {
	const size_t n = (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	uint8_t* ids = malloc(n);
	uint8_t* meta = calloc(n / 2, 1);
	uint8_t* sky = malloc(n / 2);
	uint8_t* torch = malloc(n / 2);
	uint8_t hm[CHUNK_SIZE * CHUNK_SIZE], gi[CHUNK_SIZE * CHUNK_SIZE] = {0},
		gj[CHUNK_SIZE * CHUNK_SIZE] = {0};
	assert(ids && meta && sky && torch);
	for(int cx = -R; cx <= R; cx++)
		for(int cz = -R; cz <= R; cz++) {
			memset(sky, 0, n / 2);
			memset(torch, 0, n / 2);
			for(int lx = 0; lx < CHUNK_SIZE; lx++)
				for(int lz = 0; lz < CHUNK_SIZE; lz++) {
					int i = (cx + R) * CHUNK_SIZE + lx, k = (cz + R) * CHUNK_SIZE + lz;
					hm[lx + lz * CHUNK_SIZE] = g_height[i][k];
					for(int y = 0; y < WORLD_HEIGHT; y++) {
						size_t a = (size_t)lx * CHUNK_SIZE * WORLD_HEIGHT
							+ (size_t)lz * WORLD_HEIGHT + y;
						ids[a] = g_id[i][k][y];
						unsigned sh = (y & 1) * 4;
						sky[a / 2] |= (uint8_t)((g_sky[i][k][y] & 15) << sh);
						torch[a / 2] |= (uint8_t)((g_torch[i][k][y] & 15) << sh);
					}
				}
			assert(world_build_full_section(w, cx * CHUNK_SIZE, cz * CHUNK_SIZE, ids,
											meta, sky, torch, hm, gi, gj));
		}
	free(torch);
	free(sky);
	free(meta);
	free(ids);
}

static size_t mismatches(struct world* w, const char* label) {
	size_t bad = 0;
	for(int i = 0; i < SPAN; i++)
		for(int k = 0; k < SPAN; k++)
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				struct block_data b = world_get_block(w, X0 + i, y, X0 + k);
				if(b.sky_light != g_sky[i][k][y] || b.torch_light != g_torch[i][k][y]) {
					if(!bad)
						printf("  %s: first mismatch at (%d,%d,%d) sky %u/%u torch %u/%u\n",
							   label, X0 + i, y, X0 + k, b.sky_light, g_sky[i][k][y],
							   b.torch_light, g_torch[i][k][y]);
					bad++;
				}
			}
	return bad;
}

/* Spill observation (fix-r2): frames that ended with a parked frontier still
 * being spilled, and the largest frontier seen entering such a frame. A
 * synchronous spill would end every park in one frame. */
static size_t g_spill_frames, g_spill_max_frontier;

/* Run frames until the edit queue, the flood and the repair are all empty. */
static size_t drain(struct world* w, size_t* max_work) {
	size_t frames = 0, total = 0;
	*max_work = 0;
	g_spill_frames = g_spill_max_frontier = 0;
	while(!stack_empty(&w->lighting_updates)
		  || !lighting_flood_queue_empty(&w->lighting_flood_queue)
		  || world_light_repair_pending(w)) {
		if(w->light_spill_active
		   && stack_size(&w->lighting_flood_queue) > g_spill_max_frontier)
			g_spill_max_frontier = stack_size(&w->lighting_flood_queue);
		world_update_lighting(w);
		if(w->light_spill_active)
			g_spill_frames++;
		size_t work = world_lighting_driver_last_work();
		assert(work <= LIGHTING_CLIENT_POPS_PER_FRAME && "frame over budget");
		if(work > *max_work)
			*max_work = work;
		total += work;
		assert(++frames < 20000u && "client relight never settled");
	}
	return total;
}

/* Unlit start (stored sky/torch 0 everywhere) whose fixed point every interior
 * cell differs from, so ONE forced edit floods the whole resident area. */
static void build_unlit(struct world* w) {
	fixture_blocks();
	memset(g_sky, 0, sizeof(g_sky));
	memset(g_torch, 0, sizeof(g_torch));
	world_create(w);
	w->dimension = WORLD_DIM_OVERWORLD;
	install(w);
	oracle_settle(); // the expected answer (edge cells stay at their stored 0)
}

static void test_ceiling_parks_and_repairs(void) {
	struct world w;
	build_unlit(&w);
	uint32_t parks0 = lighting_client_ceiling_parks(),
			 fceil0 = lighting_flood_ceiling_hits(), drops0 = stack_oom_drops(),
			 rep0 = world_light_repairs_done();
	world_set_block(&w, 0, 64, 0, (struct block_data) {.type = BLOCK_AIR}, true);
	assert(stack_size(&w.lighting_updates) == 1u);
	size_t max_work, total = drain(&w, &max_work);
	uint32_t parks = lighting_client_ceiling_parks() - parks0;
	printf("  ceiling: %zu work units, max %zu/frame, %u park(s), %u column "
		   "repairs, flood max %u pops; spill over %zu frame(s), frontier up "
		   "to %zu\n", total, max_work, parks,
		   world_light_repairs_done() - rep0, lighting_client_flood_max_pops(),
		   g_spill_frames, g_spill_max_frontier);
	assert(parks >= 1u && "fixture must exceed the 500k per-flood ceiling");
	/* fix-r2: the parked frontier is bigger than one frame's budget, so the
	 * per-frame cap asserted in drain() only holds because the spill is
	 * incremental and charged (it used to drain the whole stack in one go). */
	assert(g_spill_max_frontier > LIGHTING_CLIENT_POPS_PER_FRAME
		   && "fixture frontier must exceed one frame's budget");
	assert(g_spill_frames >= 1u && "spill did not span frames");
	assert(lighting_client_flood_max_pops() >= 500000u);
	assert(world_light_repairs_done() > rep0 && "parked work was not repaired");
	assert(lighting_flood_ceiling_hits() == fceil0 && "client park blamed on server");
	assert(stack_oom_drops() == drops0 && "a push was dropped");
	assert(mismatches(&w, "ceiling") == 0 && "parked flood != full recompute");
	world_destroy(&w);
}

static void test_low_memory_is_lossless(void) {
	struct world w;
	build_unlit(&w);
	uint32_t oom0 = lighting_client_oom_parks(), drops0 = stack_oom_drops();
	// Every client queue grow fails from now on: the edit queue (16 entries
	// preallocated) overflows after 16 edits, the 128-entry flood frontier
	// cannot grow, and a freed frontier cannot take a seed. Only the O(1)
	// column repair can make progress.
	lighting_test_client_fail_reserve_after(0);
	for(int y = 50; y < 80; y++) // 30 edits: 16 queued, 14 straight to repair
		world_set_block(&w, 3, y, 3, (struct block_data) {.type = BLOCK_AIR}, true);
	assert(stack_size(&w.lighting_updates) == 16u);
	assert(world_light_repair_pending(&w) >= 1u);
	size_t max_work, total = drain(&w, &max_work);
	lighting_test_client_fail_reserve_after(-1);
	printf("  low memory: %zu work units, max %zu/frame, %u failed grows kept "
		   "as repairs, %u drops\n", total, max_work,
		   lighting_client_oom_parks() - oom0, stack_oom_drops() - drops0);
	assert(lighting_client_oom_parks() > oom0);
	assert(stack_oom_drops() == drops0 && "a push was dropped");
	assert(mismatches(&w, "low memory") == 0 && "low-memory relight != full recompute");
	world_destroy(&w);
}

/* Root cause: a one-column world lit (consistently) from an absent eastern
 * neighbour. An edit on the shared edge must not relight against "darkness". */
static void test_missing_neighbour_is_not_dark(void) {
	struct world w;
	world_create(&w);
	w.dimension = WORLD_DIM_OVERWORLD;
	const size_t n = (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	uint8_t* ids = calloc(n, 1);
	uint8_t* meta = calloc(n / 2, 1);
	uint8_t* sky = calloc(n / 2, 1);
	uint8_t* torch = calloc(n / 2, 1);
	uint8_t hm[CHUNK_SIZE * CHUNK_SIZE], gi[CHUNK_SIZE * CHUNK_SIZE] = {0},
		gj[CHUNK_SIZE * CHUNK_SIZE] = {0};
	assert(ids && meta && sky && torch);
	for(int lx = 0; lx < CHUNK_SIZE; lx++)
		for(int lz = 0; lz < CHUNK_SIZE; lz++) {
			hm[lx + lz * CHUNK_SIZE] = ROOF_Y + 1;
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				size_t a = (size_t)lx * CHUNK_SIZE * WORLD_HEIGHT
					+ (size_t)lz * WORLD_HEIGHT + y;
				unsigned sh = (y & 1) * 4;
				int s = 0;
				if(y <= FLOOR_Y || y == ROOF_Y)
					ids[a] = BLOCK_STONE;
				else if(y > ROOF_Y)
					s = 15;
				else // under the roof: sky entering from the open column at x=16
					s = 14 - (CHUNK_SIZE - 1 - lx);
				sky[a / 2] |= (uint8_t)((s < 0 ? 0 : s) << sh);
			}
		}
	assert(world_build_full_section(&w, 0, 0, ids, meta, sky, torch, hm, gi, gj));
	free(torch);
	free(sky);
	free(meta);
	free(ids);

	uint32_t max0 = lighting_client_flood_max_pops();
	// A no-op edit on the edge cell beside the absent column (x = 16). The
	// edit packet's light (0 here) must not stick there: the cell keeps its
	// stored light like Beta's setBlock, and the relight never lowers it.
	world_set_block(&w, CHUNK_SIZE - 1, 20, 8, (struct block_data) {.type = BLOCK_AIR},
					true);
	size_t max_work, total = drain(&w, &max_work);
	size_t bad = 0;
	for(int lx = 0; lx < CHUNK_SIZE; lx++)
		for(int lz = 0; lz < CHUNK_SIZE; lz++)
			for(int y = FLOOR_Y + 1; y < ROOF_Y; y++) {
				int want = 14 - (CHUNK_SIZE - 1 - lx);
				want = want < 0 ? 0 : want;
				unsigned got = world_get_block(&w, lx, y, lz).sky_light;
				if(got != (unsigned)want && bad++ < 8)
					printf("  (%d,%d,%d) sky %u, stored %d\n", lx, y, lz, got, want);
			}
	printf("  missing neighbour: %zu cells changed by an edge edit (%zu work)\n",
		   bad, total);
	assert(bad == 0 && "edge relight treated the absent column as dark");
	// The interior still relights: a torch placed mid-column lights its cell.
	world_set_block(&w, 8, 20, 8, (struct block_data) {.type = BLOCK_TORCH}, true);
	drain(&w, &max_work);
	assert(world_get_block(&w, 8, 20, 8).torch_light == 14);
	assert(world_get_block(&w, 9, 20, 8).torch_light == 13);
	printf("  missing neighbour: edge edit changed nothing (%zu work units), "
		   "interior torch lit; flood max %u -> %u\n", total, max0,
		   lighting_client_flood_max_pops());
	world_destroy(&w);
}

/* fix-r2 finding 1: under OOM an edit goes straight to the column repair. Stone
 * sealing a torch-lit tunnel that crosses a column edge, ON the edge: the
 * sealed cell already holds light 0 after the write, so its own relaxation
 * changes nothing; the repair must still visit the column across the seal (the
 * forced seed it replaces pushes that neighbour), or the far half of the tunnel
 * keeps its stale light. Expected = the oracle's fixed point of the new blocks
 * (the tunnel is far from the area's outer edge, so the seam rule plays no
 * part in the cells that change). */
#define TUN_Y 20
#define TUN_Z 8
#define SEAL_X (-1) /* lx 15 of column cx = -1; the tunnel continues at x = 0 */
static void test_oom_seal_cross_column_tunnel(void) {
	for(int i = 0; i < SPAN; i++)
		for(int k = 0; k < SPAN; k++) {
			for(int y = 0; y < WORLD_HEIGHT; y++)
				g_id[i][k][y] = y <= ROOF_Y ? BLOCK_STONE : BLOCK_AIR;
			g_height[i][k] = ROOF_Y + 1;
		}
	for(int x = -10; x <= 10; x++)
		g_id[x - X0][TUN_Z - X0][TUN_Y] = BLOCK_AIR;
	g_id[-8 - X0][TUN_Z - X0][TUN_Y] = BLOCK_TORCH;
	memset(g_sky, 0, sizeof(g_sky));
	memset(g_torch, 0, sizeof(g_torch));
	oracle_settle(); // the lit, consistent start
	assert(g_torch[SEAL_X - X0][TUN_Z - X0][TUN_Y] == 7);
	assert(g_torch[0 - X0][TUN_Z - X0][TUN_Y] == 6);
	struct world w;
	world_create(&w);
	w.dimension = WORLD_DIM_OVERWORLD;
	install(&w);
	assert(mismatches(&w, "tunnel start") == 0);

	uint32_t oom0 = lighting_client_oom_parks(), drops0 = stack_oom_drops();
	lighting_test_client_fail_reserve_after(0);
	// 16 light-neutral edits (stone over stone, deep in the rock of column
	// (1, -1)) fill the preallocated edit queue, so the seal cannot be queued.
	for(int n = 0; n < 16; n++)
		world_set_block(&w, 20, 10, -10, (struct block_data) {.type = BLOCK_STONE},
						true);
	assert(stack_size(&w.lighting_updates) == 16u);
	world_set_block(&w, SEAL_X, TUN_Y, TUN_Z, (struct block_data) {.type = BLOCK_STONE},
					true);
	assert(stack_size(&w.lighting_updates) == 16u && "seal must go to the repair");
	assert(lighting_client_oom_parks() > oom0);
	size_t max_work, total = drain(&w, &max_work);
	lighting_test_client_fail_reserve_after(-1);

	g_id[SEAL_X - X0][TUN_Z - X0][TUN_Y] = BLOCK_STONE;
	g_torch[SEAL_X - X0][TUN_Z - X0][TUN_Y] = 0; // what the edit wrote
	oracle_settle();
	assert(g_torch[0 - X0][TUN_Z - X0][TUN_Y] == 0 && "oracle: far half is dark");
	size_t bad = mismatches(&w, "oom seal");
	printf("  oom seal: %zu work units, max %zu/frame, %zu mismatching cells, "
		   "%u drops\n", total, max_work, bad, stack_oom_drops() - drops0);
	assert(stack_oom_drops() == drops0 && "a push was dropped");
	assert(bad == 0 && "OOM-path seal left stale light across the column edge");
	world_destroy(&w);
}

/* fix-r2 finding 3: an opaque roof placed ON a residency seam (a single resident
 * column; x = 15 touches the absent column x = 16). Beta clears the newly
 * covered skylight at once (Chunk.java:251 -> func_1003_g, lines 199-200), so
 * raise-only relaxation must not keep 15 under the roof. Closed-form full
 * recompute, independent of the seam rule: every cell under the roof at
 * (15, y, 8) has an open-sky resident neighbour (14, y, 8) at 15 and the roof
 * above, so its sky is exactly 14; the roof (stone) is 0; nothing else moves. */
static void test_seam_roof_clears_skylight(void) {
	struct world w;
	world_create(&w);
	w.dimension = WORLD_DIM_OVERWORLD;
	const size_t n = (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	uint8_t* ids = calloc(n, 1);
	uint8_t* meta = calloc(n / 2, 1);
	uint8_t* sky = calloc(n / 2, 1);
	uint8_t* torch = calloc(n / 2, 1);
	uint8_t hm[CHUNK_SIZE * CHUNK_SIZE], gi[CHUNK_SIZE * CHUNK_SIZE] = {0},
		gj[CHUNK_SIZE * CHUNK_SIZE] = {0};
	assert(ids && meta && sky && torch);
	for(int lx = 0; lx < CHUNK_SIZE; lx++)
		for(int lz = 0; lz < CHUNK_SIZE; lz++) {
			hm[lx + lz * CHUNK_SIZE] = FLOOR_Y + 1;
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				size_t a = (size_t)lx * CHUNK_SIZE * WORLD_HEIGHT
					+ (size_t)lz * WORLD_HEIGHT + y;
				if(y <= FLOOR_Y)
					ids[a] = BLOCK_STONE;
				else
					sky[a / 2] |= (uint8_t)(15 << ((y & 1) * 4));
			}
		}
	assert(world_build_full_section(&w, 0, 0, ids, meta, sky, torch, hm, gi, gj));
	free(torch);
	free(sky);
	free(meta);
	free(ids);

	const int RX = CHUNK_SIZE - 1, RZ = 8;
	world_set_block(&w, RX, ROOF_Y, RZ, (struct block_data) {.type = BLOCK_STONE},
					true);
	size_t max_work, total = drain(&w, &max_work);
	size_t bad = 0;
	for(int lx = 0; lx < CHUNK_SIZE; lx++)
		for(int lz = 0; lz < CHUNK_SIZE; lz++)
			for(int y = 0; y < WORLD_HEIGHT; y++) {
				unsigned want = y <= FLOOR_Y ? 0 : 15;
				if(lx == RX && lz == RZ && y == ROOF_Y)
					want = 0;
				else if(lx == RX && lz == RZ && y > FLOOR_Y && y < ROOF_Y)
					want = 14;
				unsigned got = world_get_block(&w, lx, y, lz).sky_light;
				if(got != want && bad++ < 4)
					printf("  (%d,%d,%d) sky %u, want %u\n", lx, y, lz, got, want);
			}
	printf("  seam roof: %zu wrong skylight cells (%zu work units, max %zu/frame)\n",
		   bad, total, max_work);
	assert(bad == 0 && "stale skylight kept under a roof placed at a seam");
	world_destroy(&w);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	puts("test_lighting_client_lossless:");
	test_missing_neighbour_is_not_dark();
	test_seam_roof_clears_skylight();
	test_oom_seal_cross_column_tunnel();
	test_ceiling_parks_and_repairs();
	test_low_memory_is_lossless();
	puts("test_lighting_client_lossless: PASS (no client light work lost)");
	return 0;
}
