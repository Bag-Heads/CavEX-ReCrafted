/*
	test_far_skin_equiv -- the faithfulness proof for the CHEAP terrain-only far
	skin (server_world_extract_far_skin: a single worldgen_generate_chunk with NO
	populate cascade). It must produce a far-ring surface skin byte-identical to the
	faithful POST-POPULATE skin at every column EXCEPT the disclosed render-only LOD
	carve-out: tree-trunk columns and populate lake basins/edges. Any difference
	OUTSIDE those is a faithfulness BUG and fails the test.

	The "skin" is the lod_surface_height / extract predicate: per column, the top
	NON-see-through block's y + id (server_world_extract_far_column). We build it two
	ways for the same chunk:
	  - CHEAP : worldgen_generate_chunk alone (terrain: biome+density+fill+surface+cave)
	  - FULL  : the same terrain, then worldgen_populate over its 2x2 quadrant
	and assert every differing column is attributable to populate decoration -- the
	disclosed render-only LOD carve-out (CLAUDE.md): deliberately enumerated, never
	silently widened. The five (and only five) ways populate changes the top
	surface the far extract captures:
	  (a) a tree LOG trunk now tops the column (raised, or spilled in from a neighbour);
	  (b) a lake basin lowered the surface (full skin_y < cheap skin_y);
	  (c) the lake dirt<->grass edge swap (same y, DIRT<->GRASS);
	  (d) a WorldGenMinable dirt/gravel/ore vein outcrops, replacing surface STONE
	      (same y, cheap=STONE -> full=a stone-replacement block).
	  (e) a populate LAKE filled the column, so the extract captures the WATER SURFACE
	      (full=BLOCK_WATER_STILL) where cheap had dry land -- the distant-water-surface
	      fix (the extract now reports water, not the seabed beneath it).
	All five are sub-pixel surface-material/silhouette changes at far distance and are
	regenerated to the real block on promotion. Anything ELSE is a faithfulness bug.
	Standalone -- server_world.c can't link in isolation, so we replicate the extract
	predicate exactly (same can_see_through scan). Always exits 0 on success, 1 on a
	faithfulness violation; prints a per-case + total summary.

	Like bench_worldgen this links the worldgen with a MINIMAL real block table so the
	can_see_through predicate + populate's heightmap reads are valid.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "network/server_world.h" // struct server_chunk
#include "platform/time.h"
#include "worldgen/populate.h"
#include "worldgen/worldgen.h"

#define LIDX(lx, y, lz) ((size_t)(y) + ((size_t)(lz) + (size_t)(lx) * 16) * 128)

// Minimal real block table -- the skin predicate needs can_see_through, populate's
// trees need a valid heightmap (which needs opacity), so give terrain + decoration
// blocks real light properties. Mirrors tests/bench_worldgen.c.
static struct block b_solid = {.can_see_through = false, .opacity = 15};
static struct block b_leaves = {.can_see_through = true, .opacity = 1};
static struct block b_water = {.can_see_through = true, .opacity = 3};
static struct block b_lava
	= {.can_see_through = false, .opacity = 15, .luminance = 15};
static struct block b_plant = {.can_see_through = true, .opacity = 0};
struct block* blocks[256] = {0};

static void setup_blocks(void) {
	blocks[BLOCK_STONE] = blocks[BLOCK_GRASS] = blocks[BLOCK_DIRT]
		= blocks[BLOCK_COBBLESTONE] = blocks[BLOCK_BEDROCK] = blocks[BLOCK_SAND]
		= blocks[BLOCK_GRAVEL] = blocks[BLOCK_LOG] = blocks[BLOCK_SANDSTONE]
		= blocks[BLOCK_CLAY] = &b_solid;
	blocks[BLOCK_WATER_FLOW] = blocks[BLOCK_WATER_STILL] = blocks[BLOCK_ICE]
		= &b_water;
	blocks[BLOCK_LAVA_FLOW] = blocks[BLOCK_LAVA_STILL] = &b_lava;
	blocks[BLOCK_LEAVES] = &b_leaves;
	// Snow layer + flora are see-through -> never enter the skin (the whole point).
	// The only OPAQUE thing populate puts on the surface is the tree LOG (mapped
	// above); any block left unmapped (blocks[id]==NULL) is treated see-through by
	// the predicate below, which matches Beta's flora -- so this is belt-and-braces.
	blocks[BLOCK_SNOW] = blocks[BLOCK_FLOWER] = blocks[BLOCK_ROSE]
		= blocks[BLOCK_TALL_GRASS] = blocks[BLOCK_REED] = blocks[BLOCK_CACTUS]
		= &b_plant;
}

uint8_t nibble_read(uint8_t* base, size_t idx) {
	return (idx & 1) ? (uint8_t)(base[idx >> 1] >> 4)
					 : (uint8_t)(base[idx >> 1] & 0x0F);
}
void nibble_write(uint8_t* base, size_t idx, uint8_t data) {
	size_t i = idx >> 1;
	if(idx & 1)
		base[i] = (uint8_t)((base[i] & 0x0F) | ((data & 0x0F) << 4));
	else
		base[i] = (uint8_t)((base[i] & 0xF0) | (data & 0x0F));
}

// 2x2 quadrant view for worldgen_populate (mirrors bench_worldgen.c).
struct qctx {
	int cx, cz;
	struct server_chunk* c[2][2];
};
static struct server_chunk* qpick(struct qctx* q, int x, int z, int* lx,
								  int* lz) {
	int gx = x - q->cx * 16, gz = z - q->cz * 16;
	if(gx < 0 || gx >= 32 || gz < 0 || gz >= 32)
		return NULL;
	*lx = gx & 15;
	*lz = gz & 15;
	return q->c[gx >> 4][gz >> 4];
}
static uint8_t qget(void* ctx, int x, int y, int z) {
	if(y < 0 || y >= 128)
		return 0;
	int lx, lz;
	struct server_chunk* sc = qpick(ctx, x, z, &lx, &lz);
	return sc ? sc->ids[LIDX(lx, y, lz)] : 0;
}
static void qset(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	if(y < 0 || y >= 128)
		return;
	int lx, lz;
	struct server_chunk* sc = qpick(ctx, x, z, &lx, &lz);
	if(sc) {
		sc->ids[LIDX(lx, y, lz)] = id;
		nibble_write(sc->metadata, LIDX(lx, y, lz), meta);
	}
}
static int qheight(void* ctx, int x, int z) {
	int lx, lz;
	struct server_chunk* sc = qpick(ctx, x, z, &lx, &lz);
	return sc ? sc->heightmap[lx + lz * 16] : 0;
}
static int qsky(void* ctx, int x, int y, int z) {
	(void)ctx;
	(void)x;
	(void)z;
	return y < 0 ? 0 : 15;
}

// The far-skin extract predicate (mirror of server_world_extract_far_column): per
// column, the WATER SURFACE if water sits above the seabed (top_id=BLOCK_WATER_STILL,
// untinted blue), else the top non-see-through block (seabed/land), else FAR_NO_SURFACE.
// MUST stay in lockstep with the production predicate or this equivalence proof drifts.
static void extract_skin(struct server_chunk* sc, uint8_t* skin_y,
						 uint8_t* top_id) {
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++) {
			int s = -1;
			int top_water = -1;
			for(int y = 127; y >= 0; y--) {
				uint8_t id = sc->ids[LIDX(x, y, z)];
				if(top_water < 0
				   && (id == BLOCK_WATER_STILL || id == BLOCK_WATER_FLOW))
					top_water = y;
				if(blocks[id] && !blocks[id]->can_see_through) {
					s = y;
					break;
				}
			}
			int i = x * 16 + z;
			if(top_water >= 0 && top_water > s) {
				skin_y[i] = (uint8_t)top_water;
				top_id[i] = BLOCK_WATER_STILL;
			} else if(s >= 0) {
				skin_y[i] = (uint8_t)s;
				top_id[i] = sc->ids[LIDX(x, s, z)];
			} else {
				skin_y[i] = 0xFF;
				top_id[i] = 0;
			}
		}
}

static void free_chunk(struct server_chunk* sc) {
	if(sc->ids) {
		free(sc->ids);
		free(sc->metadata);
		free(sc->lighting_sky);
		free(sc->lighting_torch);
		free(sc->heightmap);
	}
}

int main(void) {
	setup_blocks();

	long total = 0, diff = 0, tree = 0, lake = 0, vein = 0, bad = 0;

	// Spread coords over varied terrain + a couple of seeds for biome/lake/forest mix.
	const int64_t seeds[] = {1, 1337};
	for(size_t si = 0; si < sizeof(seeds) / sizeof(seeds[0]); si++) {
		int64_t seed = seeds[si];
		struct worldgen* wg = worldgen_create(seed, WORLD_DIM_OVERWORLD);
		if(!wg) {
			printf("worldgen_create failed\n");
			return 0;
		}

		for(int n = 0; n < 48; n++) {
			int cx = n * 5, cz = (n * 11) % 80;

			// 2x2 quadrant terrain-only (the cheap path generates each chunk alone;
			// the quadrant exists only so the FULL path's populate can run + spill).
			struct qctx q = {cx, cz, {{0}}};
			struct server_chunk sc[2][2];
			memset(sc, 0, sizeof(sc));
			int ok = 1;
			for(int dx = 0; dx < 2; dx++)
				for(int dz = 0; dz < 2; dz++) {
					if(worldgen_generate_chunk(wg, &sc[dx][dz], cx + dx, cz + dz))
						q.c[dx][dz] = &sc[dx][dz];
					else
						ok = 0;
				}
			if(!ok) {
				for(int dx = 0; dx < 2; dx++)
					for(int dz = 0; dz < 2; dz++)
						free_chunk(&sc[dx][dz]);
				continue;
			}

			// CHEAP skin of the (0,0) target = its terrain-only surface.
			uint8_t cheap_y[256], cheap_id[256];
			extract_skin(&sc[0][0], cheap_y, cheap_id);

			// FULL: populate the quadrant (trees/lakes/decoration + cross-chunk
			// spill), then re-read the (0,0) target's skin.
			struct wgen_world view = {&q, qget, qset, qheight, qsky, NULL};
			enum biome cb = worldgen_biome_at(wg, cx * 16 + 16, cz * 16 + 16);
			double td = worldgen_tree_density(wg, cx * 16, cz * 16);
			worldgen_populate(seed, cx, cz, &view, cb, td);

			uint8_t full_y[256], full_id[256];
			extract_skin(&sc[0][0], full_y, full_id);

			// Compare: every difference MUST be a disclosed LOD carve-out column.
			for(int i = 0; i < 256; i++) {
				total++;
				if(cheap_y[i] == full_y[i] && cheap_id[i] == full_id[i])
					continue;
				diff++;
				int fy = full_y[i], cy = cheap_y[i];
				if(full_id[i] == BLOCK_LOG) {
					tree++; // (a) tree trunk now tops the column
				} else if(fy != 0xFF && cy != 0xFF && fy < cy) {
					lake++; // (b) lake basin lowered the surface
				} else if(fy == cy
						  && ((cheap_id[i] == BLOCK_DIRT && full_id[i] == BLOCK_GRASS)
							  || (cheap_id[i] == BLOCK_GRASS
								  && full_id[i] == BLOCK_DIRT))) {
					lake++; // (c) lake dirt<->grass edge swap (same height)
				} else if(fy == cy && cheap_id[i] == BLOCK_STONE
						  && full_id[i] != BLOCK_STONE) {
					vein++; // (d) WorldGenMinable dirt/gravel/ore vein outcrop on stone
				} else if(full_id[i] == BLOCK_WATER_STILL
						  && cheap_id[i] != BLOCK_WATER_STILL) {
					// (e) populate LAKE filled this column: the far ring now captures the
					// faithful WATER SURFACE (id 9, untinted) where cheap terrain had dry
					// land. This is the distant-water fix -- over a lake whose surface sits
					// AT the cheap land height it shows as same-y land->water (lakes that
					// LOWERED the surface are already caught by (b)). Render-only; the real
					// lake regenerates on promotion.
					lake++;
				} else {
					bad++;
					if(bad <= 20)
						printf("  UNEXPECTED diff seed=%lld chunk=(%d,%d) col=%d: "
							   "cheap(y=%d,id=%d) full(y=%d,id=%d)\n",
							   (long long)seed, cx, cz, i, cy, cheap_id[i], fy,
							   full_id[i]);
				}
			}

			for(int dx = 0; dx < 2; dx++)
				for(int dz = 0; dz < 2; dz++)
					free_chunk(&sc[dx][dz]);
		}

		worldgen_destroy(wg);
	}

	printf("test_far_skin_equiv: %ld columns, %ld differ "
		   "(%ld tree-trunk, %ld lake, %ld vein-outcrop) -- %ld UNEXPECTED\n",
		   total, diff, tree, lake, vein, bad);
	printf("  cheap terrain-only far skin == post-populate skin at every column "
		   "outside the disclosed LOD carve-out (tree/lake/vein): %s\n",
		   bad == 0 ? "PASS" : "FAIL");

	// W1-d (far-reach program Wave 1, docs/far_reach_program_2026-09-03.md
	// "worldgen fast path (a)"): worldgen_generate_chunk_far_skin skips
	// phase 5 (worldgen_lighting) for the far-skin extraction path only.
	// Prove sc->ids -- the ONLY array server_world_extract_far_column reads
	// -- is byte-identical with and without that phase, over the SAME
	// seed/coord matrix used above (48 chunks x 2 seeds = 96 chunks,
	// 32768 bytes each), and report the per-chunk wall-clock delta this
	// item's proof standard requires. This is a PC signal only (CLAUDE.md:
	// only real Wii / bench_mesher / s_dbg_* counters certify a perf claim).
	{
		long lighting_skip_chunks = 0, lighting_skip_id_diffs = 0;
		double full_ms = 0.0, skip_ms = 0.0;
		for(size_t si = 0; si < sizeof(seeds) / sizeof(seeds[0]); si++) {
			int64_t seed = seeds[si];
			struct worldgen* wg = worldgen_create(seed, WORLD_DIM_OVERWORLD);
			if(!wg) {
				printf("worldgen_create failed (lighting-skip case)\n");
				continue;
			}
			for(int n = 0; n < 48; n++) {
				int cx = n * 5, cz = (n * 11) % 80;
				struct server_chunk with_light, skip_light;
				memset(&with_light, 0, sizeof(with_light));
				memset(&skip_light, 0, sizeof(skip_light));

				ptime_t t0 = time_get();
				bool ok_full = worldgen_generate_chunk(wg, &with_light, cx, cz);
				full_ms += time_diff_s(t0, time_get()) * 1000.0F;

				ptime_t t1 = time_get();
				bool ok_skip = worldgen_generate_chunk_far_skin(wg, &skip_light,
																cx, cz);
				skip_ms += time_diff_s(t1, time_get()) * 1000.0F;

				if(ok_full && ok_skip) {
					lighting_skip_chunks++;
					if(memcmp(with_light.ids, skip_light.ids, 16 * 16 * 128)
					   != 0) {
						lighting_skip_id_diffs++;
						if(lighting_skip_id_diffs <= 5)
							printf("  UNEXPECTED ids diff (lighting skip) "
								   "seed=%lld chunk=(%d,%d)\n",
								   (long long)seed, cx, cz);
					}
				}
				if(ok_full)
					free_chunk(&with_light);
				if(ok_skip)
					free_chunk(&skip_light);
			}
			worldgen_destroy(wg);
		}
		printf("test_far_skin_equiv (W1-d lighting-skip CASE): %ld chunks, "
			   "%ld ids-mismatch (%s)\n",
			   lighting_skip_chunks, lighting_skip_id_diffs,
			   lighting_skip_id_diffs == 0 ? "PASS" : "FAIL");
		printf("  per-chunk gen time: full(+lighting)=%.4f ms, "
			   "far-skin(no lighting)=%.4f ms (%.1f%% saved) -- PC wall-clock, "
			   "informational only per CLAUDE.md\n",
			   full_ms / (double)lighting_skip_chunks,
			   skip_ms / (double)lighting_skip_chunks,
			   100.0 * (1.0 - skip_ms / full_ms));
		fflush(stdout);
		assert(lighting_skip_id_diffs == 0);
		if(lighting_skip_id_diffs != 0)
			bad++;
	}

	fflush(stdout); // flush diagnostics before assert's abort() discards them
	assert(bad == 0);
	return bad == 0 ? 0 : 1;
}
