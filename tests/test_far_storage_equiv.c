/*
	Stage-2 LOD S2 (docs/stage2_lod_plan_2026-09-03.md; world.h's
	enum world_far_storage; chunk.h's struct far_column_skin) -- the
	faithfulness proof this slice's CLAUDE.md T2 rules require: with
	"lod.far_storage":"skin", every emitted display-list byte for a far-ring
	column must be IDENTICAL to the default "voxel" storage's output. Drives
	the REAL client chunk_mesher worker end-to-end (test_far_geomip_flagoff_
	equiv.c's harness shape), not a hand-mirrored copy -- chunk_get_block's
	synthesized read is production code, exercised exactly as world_update_lod
	+ chunk_check_built would dispatch it.

	CASE A: a matrix of synthetic seeds x column patterns (varied per-column
	stepped heights -- triggers the silhouette skirt pass -- mixed with flat
	stretches; surface ids spanning stone/grass/dirt/sand/a tree-LOG hull/
	water/snow/ice; and one all-air column) built twice per seed, once per
	storage mode, at well-separated chunk-origin regions of the SAME world so
	neither instance's neighbour-dirtying can touch the other. Every spanned
	Y-slice's has_displist[]/mesh[].index/mesh[].data and reachable[] must
	match byte-for-byte.

	CASE B: a "seam" case -- a real NEAR (full-detail, lod=0) chunk built
	immediately beside the far column in both instances (mirrored terrain),
	so the far chunk's padded gather crosses a genuine near/far boundary
	(chunk_lookup_block reading into a full chunk's real block buffer, not
	the far/far or far/missing-neighbour cases CASE A already covers).

	CASE C: the is_far_lod / lighting-self-consistency assertion -- a
	skin-storage far section still sets world_section.is_far_lod (the client
	relight seam guard, world.c) and the open-air cell just above the surface
	still reads the dimension's open-sky value (the M1 self-consistency fix:
	old==new at the seam stops the relight flood cold), exactly as the voxel
	path does.

	CASE D: far-evict / far->full promotion / walk-back-out churn -- skin
	storage survives world_unload_section, world_build_full_section (the
	far->full promotion) and a re-demotion back to a skin-storage far column
	at the SAME coordinate, several times, without a crash or a stale
	residency flag. world_release_section_chunks (the single choke point
	every one of those paths already goes through) frees each chunk's
	far_skin/blocks generically in chunk_destroy -- this proves that holds
	under real churn, not just by code inspection.

	CASE E: a directional PC timing comparison of the SAME gather work
	(chunk_mesher_send, the ONLY place storage mode changes cost) between
	voxel and skin storage, per the maintainer's "far ring must never cost
	frame time" rule. This is a PC signal only -- CLAUDE.md's standing rule
	is that only real Wii / bench_mesher / s_dbg_* counters certify a perf
	claim; see this test's printed numbers and the commit message for the
	measured delta and what remains hardware-unconfirmed.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "chunk.h"
#include "chunk_mesher.h"
#include "network/client_interface.h"
#include "platform/displaylist.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "world.h"

#define HANG_TIMEOUT_MS 5000

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

static struct world w;

static void set_block(w_coord_t x, w_coord_t y, w_coord_t z, int type) {
	world_set_block(&w, x, y, z,
					(struct block_data) {.type = (uint8_t)type,
										 .metadata = 0,
										 .sky_light = 15,
										 .torch_light = 0},
					false /* no light flood: sky_light set explicitly */);
}

static bool wait_for_mesh(struct chunk* c) {
	ptime_t t0 = time_get();
	while(c->mesh_in_flight) {
		chunk_mesher_receive();
		if(c->mesh_in_flight) {
			if(time_diff_ms(t0, time_get()) > HANG_TIMEOUT_MS) {
				fprintf(stderr, "test_far_storage_equiv: HANG -- the mesher "
								"did not return within %d ms\n",
						HANG_TIMEOUT_MS);
				return false;
			}
			thread_msleep(1);
		}
	}
	return true;
}

/* Force the far-ring skin (lod=1) tier directly on an already-resident chunk,
 * the same "snapshot the fields chunk_mesher_send reads" approach
 * test_far_geomip_flagoff_equiv.c uses. world_build_far_section itself never
 * sets c->lod (that is world_update_lod's job, driven by camera distance,
 * which this test never runs) -- every far chunk must be forced here before
 * meshing, exactly as production would once the camera crosses the near/far
 * boundary. */
static bool force_skin_mesh_and_wait(struct chunk* c) {
	c->lod = 1;
	c->foliage_lod = 0;
	c->lod_dist_blocks = 0; // geomip is never enabled in this test
	c->dirty_generation++;
	c->mesh_in_flight = false;
	if(!chunk_mesher_send(c)) {
		fprintf(stderr, "test_far_storage_equiv: chunk_mesher_send refused\n");
		return false;
	}
	c->rebuild_displist = false;
	c->first_meshed = true;
	c->mesh_in_flight = true;
	return wait_for_mesh(c);
}

// A varied, deterministic per-column pattern: stepped heights (forces the
// silhouette skirt pass -- flat terrain emits none, per
// test_far_geomip_flagoff_equiv.c's CASE A comment), a tree-LOG hull column,
// a water column, a snow column, an ice column, and one all-air column --
// the matrix the plan's requirement (1) asks for. `seed` perturbs the height
// staircase + which columns get which special id, so several seeds exercise
// different step positions without needing real worldgen.
static void build_pattern(unsigned seed, w_coord_t base_y, uint8_t* skin_y,
						  uint8_t* top_id, uint8_t* grass_i, uint8_t* grass_j) {
	for(int lx = 0; lx < CHUNK_SIZE; lx++) {
		for(int lz = 0; lz < CHUNK_SIZE; lz++) {
			int i = lx * CHUNK_SIZE + lz;
			unsigned h = (unsigned)((lx * 3 + lz * 7 + seed * 5) % 9); // 0..8 step
			int y = (int)base_y + (int)h;

			grass_i[i] = (uint8_t)((lx * 13 + seed * 3) & 0xFF);
			grass_j[i] = (uint8_t)((lz * 17 + seed * 11) & 0xFF);

			int slot = (lx + lz + (int)seed) % 16;
			if(slot == 0) {
				skin_y[i] = 0xFF; // all-air column (FAR_NO_SURFACE)
				top_id[i] = 0;
			} else if(slot == 1) {
				skin_y[i] = (uint8_t)y;
				top_id[i] = BLOCK_LOG; // tree-trunk hull (chunk_lod_policy.h)
			} else if(slot == 2) {
				skin_y[i] = (uint8_t)y;
				top_id[i] = BLOCK_WATER_STILL;
			} else if(slot == 3) {
				skin_y[i] = (uint8_t)y;
				top_id[i] = BLOCK_SNOW_BLOCK;
			} else if(slot == 4) {
				skin_y[i] = (uint8_t)y;
				top_id[i] = BLOCK_ICE;
			} else if((lx ^ lz) & 1) {
				skin_y[i] = (uint8_t)y;
				top_id[i] = BLOCK_GRASS;
			} else if(slot < 10) {
				skin_y[i] = (uint8_t)y;
				top_id[i] = BLOCK_SAND;
			} else {
				skin_y[i] = (uint8_t)y;
				top_id[i] = BLOCK_STONE;
			}
		}
	}
}

// Every resident chunk in the built column (index by cy, NULL where the
// column has no Y-slice there -- world_build_far_section only allocates
// cy_lo..cy_hi).
static void collect_column(w_coord_t cx, w_coord_t cz,
						   struct chunk* out[COLUMN_HEIGHT]) {
	for(int cy = 0; cy < COLUMN_HEIGHT; cy++)
		out[cy] = world_find_chunk(&w, cx * CHUNK_SIZE, cy * CHUNK_SIZE,
								   cz * CHUNK_SIZE);
}

static bool mesh_column(struct chunk* col[COLUMN_HEIGHT]) {
	for(int cy = 0; cy < COLUMN_HEIGHT; cy++)
		if(col[cy] && !force_skin_mesh_and_wait(col[cy]))
			return false;
	return true;
}

// The byte-identity proof: residency, has_displist[], mesh[].index and the
// emitted bytes, plus reachable[], must match at every spanned Y-slice.
static bool columns_equal(const char* label, struct chunk* a[COLUMN_HEIGHT],
						  struct chunk* b[COLUMN_HEIGHT]) {
	bool ok = true;
	for(int cy = 0; cy < COLUMN_HEIGHT; cy++) {
		struct chunk *ca = a[cy], *cb = b[cy];
		if((ca == NULL) != (cb == NULL)) {
			printf("  %s: cy=%d residency mismatch (voxel=%p skin=%p)\n", label,
				   cy, (void*)ca, (void*)cb);
			ok = false;
			continue;
		}
		if(!ca)
			continue;
		for(int k = 0; k < CHUNK_MESH_LISTS; k++) {
			if(ca->has_displist[k] != cb->has_displist[k]) {
				printf("  %s: cy=%d list=%d has_displist mismatch\n", label, cy, k);
				ok = false;
				continue;
			}
			if(!ca->has_displist[k])
				continue;
			if(ca->mesh[k].index != cb->mesh[k].index) {
				printf("  %s: cy=%d list=%d index mismatch %zu vs %zu\n", label,
					   cy, k, ca->mesh[k].index, cb->mesh[k].index);
				ok = false;
				continue;
			}
			if(memcmp(ca->mesh[k].data, cb->mesh[k].data, ca->mesh[k].index)
			   != 0) {
				printf("  %s: cy=%d list=%d BYTE MISMATCH over %zu bytes\n", label,
					   cy, k, ca->mesh[k].index);
				ok = false;
			}
		}
		for(int s = 0; s < 6; s++)
			if(ca->reachable[s] != cb->reachable[s]) {
				printf("  %s: cy=%d reachable[%d] mismatch\n", label, cy, s);
				ok = false;
			}
	}
	return ok;
}

static bool build_far(enum world_far_storage mode, w_coord_t cx, w_coord_t cz,
					  const uint8_t* skin_y, const uint8_t* top_id,
					  const uint8_t* grass_i, const uint8_t* grass_j,
					  struct chunk* out[COLUMN_HEIGHT]) {
	world_set_far_storage(mode);
	if(!world_build_far_section(&w, cx * CHUNK_SIZE, cz * CHUNK_SIZE, skin_y,
								top_id, grass_i, grass_j))
		return false;
	collect_column(cx, cz, out);
	return mesh_column(out);
}

/* R2: query the public metadata, not only the emitted mesh. Replacing a
 * section at negative coordinates also exercises cache invalidation. */
static void test_height_queries(void) {
	uint8_t y[256], ids[256], tint[256] = {0}, expected[256];
	const int x0 = -48, z0 = -80;
	for(int round = 0; round < 3; round++) {
		for(int x = 0; x < CHUNK_SIZE; x++)
			for(int z = 0; z < CHUNK_SIZE; z++) {
				int i = x * CHUNK_SIZE + z;
				y[i] = (x + 3 * z + round) % 11 == 0
					? 255 : 40 + (3 * x + 7 * z + round) % 45;
				ids[i] = i % 3 == 0 ? BLOCK_WATER_STILL
					: i % 3 == 1 ? BLOCK_LAVA_STILL : BLOCK_STONE;
				expected[x + z * CHUNK_SIZE] = y[i] == 255 ? 0 : y[i] + 1;
			}
		for(int mode = 0; mode < 2; mode++) {
			world_set_far_storage(mode == 0 ? WORLD_FAR_STORAGE_VOXEL
										  : WORLD_FAR_STORAGE_SKIN);
			assert(world_build_far_section(&w, x0, z0, y, ids, tint, tint));
			for(int x = 0; x < CHUNK_SIZE; x++)
				for(int z = 0; z < CHUNK_SIZE; z++)
					assert(world_get_height(&w, x0 + x, z0 + z)
						   == expected[x + z * CHUNK_SIZE]);
		}
	}
	world_unload_section(&w, -3, -5);
	puts("R2 PASS: all 256 asymmetric height queries, empty/fluid columns, "
		 "negative coordinates and repeated replacement");
}

static void test_full_install_equiv(void) {
	static uint8_t ids[32768], md[16384], sky[16384], torch[16384];
	uint8_t height[256], gi[256], gj[256];
	struct chunk* reference = chunk_alloc();
	assert(reference);
	chunk_init(reference, &w, 0, 0, 0);
	chunk_ref(reference);
	uint32_t rng = 12345;
	float total_ms = 0;
	for(int pattern = 0; pattern < 20; pattern++) {
		for(size_t i = 0; i < sizeof(ids); i++) {
			rng = rng * 1664525u + 1013904223u;
			ids[i] = pattern == 0 ? 0 : pattern == 1 ? 255 : rng >> 24;
		}
		for(size_t i = 0; i < sizeof(md); i++) {
			rng = rng * 1664525u + 1013904223u;
			md[i] = rng >> 24; sky[i] = rng >> 16; torch[i] = rng >> 8;
		}
		for(int i = 0; i < 256; i++) {
			height[i] = i % 129; gi[i] = i; gj[i] = 255-i;
		}
		ptime_t start = time_get();
		assert(world_build_full_section(&w, -1600, -3200, ids, md, sky,
			torch, height, gi, gj));
		total_ms += time_diff_s(start, time_get()) * 1000.0f;
		struct world_section* section = dict_wsection_get(w.sections, SECTION_TO_ID(-100, -200));
		assert(section && !section->is_far_lod);
		assert(memcmp(section->heightmap, height, 256) == 0);
		assert(memcmp(section->grass_i, gi, 256) == 0);
		assert(memcmp(section->grass_j, gj, 256) == 0);
		for(int cy = 0; cy < COLUMN_HEIGHT; cy++) {
			memset(reference->blocks, 0xA5, CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*5/2);
			for(int x = 0; x < 16; x++)
				for(int z = 0; z < 16; z++)
					for(int y = 0; y < 16; y++) {
						size_t k = (x*16+z)*128 + cy*16+y;
						unsigned shift = (k%2)*4;
						chunk_set_block_raw(reference,x,y,z,(struct block_data){
							.type=ids[k], .metadata=(md[k/2]>>shift)&15,
							.sky_light=(sky[k/2]>>shift)&15,
							.torch_light=(torch[k/2]>>shift)&15});
					}
			assert(memcmp(reference->blocks, section->column[cy]->blocks,
				CHUNK_SIZE*CHUNK_SIZE*CHUNK_SIZE*5/2) == 0);
		}
	}
	chunk_unref(reference);
	world_unload_section(&w, -100, -200);
	printf("Full install equivalence PASS: 20 columns, every storage byte, metadata/sky/torch, replacement; %.3f ms/column PC\n", total_ms/20);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); // flush every diagnostic before a failing assert's abort()

	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);

	blocks_init();
	chunk_mesher_init();
	world_create(&w);
	test_height_queries();
	test_full_install_equiv();

	const w_coord_t BASE_Y = 60;
	bool all_ok = true;

	// --- CASE A: seed x pattern matrix ------------------------------------
	const unsigned seeds[] = {0, 1, 5, 9, 13};
	for(size_t si = 0; si < sizeof(seeds) / sizeof(seeds[0]); si++) {
		uint8_t skin_y[CHUNK_SIZE * CHUNK_SIZE], top_id[CHUNK_SIZE * CHUNK_SIZE];
		uint8_t grass_i[CHUNK_SIZE * CHUNK_SIZE], grass_j[CHUNK_SIZE * CHUNK_SIZE];
		build_pattern(seeds[si], BASE_Y, skin_y, top_id, grass_i, grass_j);

		// Well-separated chunk-origin regions: 20 chunks apart, far outside
		// each other's 4-neighbour dirty radius and each build's own padded
		// gather halo, so the two instances cannot influence one another.
		w_coord_t cx_voxel = (w_coord_t)(si * 20), cz_voxel = 0;
		w_coord_t cx_skin = (w_coord_t)(si * 20 + 5), cz_skin = 0;

		struct chunk* voxel_col[COLUMN_HEIGHT];
		struct chunk* skin_col[COLUMN_HEIGHT];
		bool built = build_far(WORLD_FAR_STORAGE_VOXEL, cx_voxel, cz_voxel, skin_y,
							   top_id, grass_i, grass_j, voxel_col)
			&& build_far(WORLD_FAR_STORAGE_SKIN, cx_skin, cz_skin, skin_y, top_id,
						grass_i, grass_j, skin_col);
		assert(built);

		char label[64];
		snprintf(label, sizeof(label), "CASE A seed=%u", seeds[si]);
		if(!columns_equal(label, voxel_col, skin_col))
			all_ok = false;
	}
	printf("CASE A (seed x pattern matrix, %zu seeds): %s\n",
		   sizeof(seeds) / sizeof(seeds[0]), all_ok ? "PASS" : "FAIL");

	// --- CASE B: seam next to a real near (full-detail) chunk -------------
	{
		uint8_t skin_y[CHUNK_SIZE * CHUNK_SIZE], top_id[CHUNK_SIZE * CHUNK_SIZE];
		uint8_t grass_i[CHUNK_SIZE * CHUNK_SIZE], grass_j[CHUNK_SIZE * CHUNK_SIZE];
		build_pattern(3, BASE_Y, skin_y, top_id, grass_i, grass_j);

		w_coord_t cx_voxel = 100, cz_voxel = 0;
		w_coord_t cx_skin = 105, cz_skin = 0;

		// A flat full-detail near chunk immediately EAST of each far column
		// (mirrored terrain at both regions) so the padded gather's +x halo
		// crosses a genuine near/far seam rather than hitting a missing
		// neighbour (already covered by CASE A's isolated columns).
		for(w_coord_t region = 0; region < 2; region++) {
			w_coord_t cx0 = region == 0 ? cx_voxel : cx_skin;
			for(w_coord_t x = (cx0 + 1) * CHUNK_SIZE; x < (cx0 + 2) * CHUNK_SIZE;
				x++)
				for(w_coord_t zz = 0; zz < CHUNK_SIZE; zz++)
					set_block(x, BASE_Y + 4, zz, BLOCK_STONE);
		}

		struct chunk* voxel_col[COLUMN_HEIGHT];
		struct chunk* skin_col[COLUMN_HEIGHT];
		bool built = build_far(WORLD_FAR_STORAGE_VOXEL, cx_voxel, cz_voxel, skin_y,
							   top_id, grass_i, grass_j, voxel_col)
			&& build_far(WORLD_FAR_STORAGE_SKIN, cx_skin, cz_skin, skin_y, top_id,
						grass_i, grass_j, skin_col);
		assert(built);

		bool ok = columns_equal("CASE B seam", voxel_col, skin_col);
		printf("CASE B (seam beside a real near chunk): %s\n", ok ? "PASS" : "FAIL");
		all_ok = all_ok && ok;
	}

	// --- CASE C: is_far_lod + open-sky self-consistency assertion ---------
	{
		uint8_t skin_y[CHUNK_SIZE * CHUNK_SIZE], top_id[CHUNK_SIZE * CHUNK_SIZE];
		uint8_t grass_i[CHUNK_SIZE * CHUNK_SIZE], grass_j[CHUNK_SIZE * CHUNK_SIZE];
		build_pattern(7, BASE_Y, skin_y, top_id, grass_i, grass_j);

		w_coord_t cx = 200, cz = 0;
		world_set_far_storage(WORLD_FAR_STORAGE_SKIN);
		assert(world_build_far_section(&w, cx * CHUNK_SIZE, cz * CHUNK_SIZE,
									   skin_y, top_id, grass_i, grass_j));

		struct world_section* s
			= dict_wsection_get(w.sections, SECTION_TO_ID(cx, cz));
		assert(s && s->is_far_lod
			   && "skin storage must still set the client relight seam guard");

		// A solid column's cell just above the surface must read the
		// dimension's open-sky value (15, Overworld) -- the exact
		// old==new self-consistency the M1 far-ring crash fix relies on
		// (world.c's world_build_far_section doc comment) -- reproduced here
		// via chunk_get_block instead of a stored buffer.
		int checked = 0;
		for(int lx = 0; lx < CHUNK_SIZE; lx++)
			for(int lz = 0; lz < CHUNK_SIZE; lz++) {
				int i = lx * CHUNK_SIZE + lz;
				if(skin_y[i] >= WORLD_HEIGHT)
					continue;
				w_coord_t world_y = skin_y[i] + 1;
				struct chunk* c
					= world_find_chunk(&w, cx * CHUNK_SIZE, world_y, cz * CHUNK_SIZE);
				assert(c);
				struct block_data above
					= chunk_get_block(c, (c_coord_t)lx, W2C_COORD(world_y),
									 (c_coord_t)lz);
				assert(above.type == BLOCK_AIR);
				assert(above.sky_light == 15);
				checked++;
			}
		printf("CASE C (is_far_lod + open-sky self-consistency, %d columns "
			   "checked): PASS\n",
			   checked);

		world_unload_section(&w, cx, cz); // world_unload_section takes CHUNK coords, not world coords
	}

	// --- CASE D: far-evict / far<->full promotion churn --------------------
	{
		uint8_t skin_y[CHUNK_SIZE * CHUNK_SIZE], top_id[CHUNK_SIZE * CHUNK_SIZE];
		uint8_t grass_i[CHUNK_SIZE * CHUNK_SIZE], grass_j[CHUNK_SIZE * CHUNK_SIZE];
		build_pattern(11, BASE_Y, skin_y, top_id, grass_i, grass_j);

		w_coord_t cx = 300, cz = 0;
		int64_t id = SECTION_TO_ID(cx, cz);

		size_t ids_n = (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
		uint8_t* ids = calloc(1, ids_n);
		uint8_t* metadata = calloc(1, ids_n / 2);
		uint8_t* lighting_sky = calloc(1, ids_n / 2);
		uint8_t* lighting_torch = calloc(1, ids_n / 2);
		uint8_t heightmap[CHUNK_SIZE * CHUNK_SIZE];
		memset(heightmap, (int)(BASE_Y + 1), sizeof(heightmap));
		for(int lx = 0; lx < CHUNK_SIZE; lx++)
			for(int lz = 0; lz < CHUNK_SIZE; lz++)
				for(int y = 0; y <= BASE_Y; y++)
					ids[(size_t)y + ((size_t)lz + (size_t)lx * CHUNK_SIZE)
											   * WORLD_HEIGHT]
						= BLOCK_STONE;
		assert(ids && metadata && lighting_sky && lighting_torch);

		bool churn_ok = true;
		for(int iter = 0; iter < 3; iter++) {
			world_set_far_storage(WORLD_FAR_STORAGE_SKIN);
			if(!world_build_far_section(&w, cx * CHUNK_SIZE, cz * CHUNK_SIZE,
										skin_y, top_id, grass_i, grass_j)) {
				churn_ok = false;
				break;
			}
			struct world_section* s = dict_wsection_get(w.sections, id);
			if(!s || !s->is_far_lod) {
				churn_ok = false;
				break;
			}
			for(int lx = 0; lx < CHUNK_SIZE; lx++)
				for(int lz = 0; lz < CHUNK_SIZE; lz++) {
					uint8_t y = skin_y[lx * CHUNK_SIZE + lz];
					assert(world_get_height(&w, cx * CHUNK_SIZE + lx,
						cz * CHUNK_SIZE + lz) == (y == 255 ? 0 : y + 1));
				}
			/* Promotion must also retire urgency on all old skin owners. */
			for(int cy = 0; cy < COLUMN_HEIGHT; cy++)
				if(s->column[cy])
					chunk_mark_urgent(s->column[cy]);
			uint32_t urgent_before = w.mesh_urgent_count;
			unsigned old_urgent = 0;
			for(int cy = 0; cy < COLUMN_HEIGHT; cy++)
				if(s->column[cy])
					old_urgent++;
			for(int cy = 0; cy < COLUMN_HEIGHT; cy++)
				if(s->column[cy] && (s->column[cy]->blocks != NULL
									 || s->column[cy]->far_skin == NULL)) {
					churn_ok = false;
					break;
				}

			// far->full promotion: replaces every Y-slice with real voxel
			// chunks (world_build_full_section always allocates via plain
			// chunk_alloc -- see world.c) and, via world_release_section_
			// chunks, frees the old skin-storage shells generically.
			if(!world_build_full_section(&w, cx * CHUNK_SIZE, cz * CHUNK_SIZE,
										 ids, metadata, lighting_sky,
										 lighting_torch, heightmap, grass_i,
										 grass_j)) {
				churn_ok = false;
				break;
			}
			s = dict_wsection_get(w.sections, id);
			if(!s || s->is_far_lod) {
				churn_ok = false;
				break;
			}
			assert(w.mesh_urgent_count == urgent_before - old_urgent);
			for(int lx = 0; lx < CHUNK_SIZE; lx++)
				for(int lz = 0; lz < CHUNK_SIZE; lz++)
					assert(world_get_height(&w, cx * CHUNK_SIZE + lx,
						cz * CHUNK_SIZE + lz) == heightmap[lx + lz * CHUNK_SIZE]);
			for(int cy = 0; cy < COLUMN_HEIGHT; cy++)
				if(!s->column[cy] || s->column[cy]->blocks == NULL
				   || s->column[cy]->far_skin != NULL) {
					churn_ok = false;
					break;
				}

			// Walk back out: evict entirely, proving world_unload_section's
			// world_release_section_chunks path also tears down a FULL
			// section's real chunks cleanly before the next iteration
			// re-demotes to skin storage at the same coordinate.
			world_unload_section(&w, cx, cz); // world_unload_section takes CHUNK coords, not world coords
			if(dict_wsection_get(w.sections, id) != NULL) {
				churn_ok = false;
				break;
			}
		}
		printf("CASE D (far-evict / far<->full promotion churn, 3 rounds): %s\n",
			   churn_ok ? "PASS" : "FAIL");
		all_ok = all_ok && churn_ok;

		free(ids);
		free(metadata);
		free(lighting_sky);
		free(lighting_torch);
	}

	// --- CASE E: directional PC gather-cost signal (informational) --------
	{
		uint8_t skin_y[CHUNK_SIZE * CHUNK_SIZE], top_id[CHUNK_SIZE * CHUNK_SIZE];
		uint8_t grass_i[CHUNK_SIZE * CHUNK_SIZE], grass_j[CHUNK_SIZE * CHUNK_SIZE];
		build_pattern(2, BASE_Y, skin_y, top_id, grass_i, grass_j);

		enum { ITERS = 200 };
		w_coord_t cx0 = 400, cz0 = 0;

		ptime_t t0 = time_get();
		for(int it = 0; it < ITERS; it++) {
			w_coord_t cx = cx0 + (w_coord_t)(it % 20);
			w_coord_t cz = cz0 + (w_coord_t)(it / 20);
			world_set_far_storage(WORLD_FAR_STORAGE_VOXEL);
			struct chunk* col[COLUMN_HEIGHT];
			assert(build_far(WORLD_FAR_STORAGE_VOXEL, cx, cz, skin_y, top_id,
							 grass_i, grass_j, col));
		}
		double voxel_ms = time_diff_ms(t0, time_get());

		w_coord_t cx1 = 500, cz1 = 0;
		ptime_t t1 = time_get();
		for(int it = 0; it < ITERS; it++) {
			w_coord_t cx = cx1 + (w_coord_t)(it % 20);
			w_coord_t cz = cz1 + (w_coord_t)(it / 20);
			struct chunk* col[COLUMN_HEIGHT];
			assert(build_far(WORLD_FAR_STORAGE_SKIN, cx, cz, skin_y, top_id,
							 grass_i, grass_j, col));
		}
		double skin_ms = time_diff_ms(t1, time_get());

		printf("CASE E (informational, PC wall-clock only -- NOT a hardware "
			   "cert): %d far-column build+mesh cycles, voxel=%.2f ms "
			   "(%.4f ms/col), skin=%.4f ms (%.4f ms/col)\n",
			   (int)ITERS, voxel_ms, voxel_ms / ITERS, skin_ms, skin_ms / ITERS);
	}

	g_drain_run = 0;
	thread_join(&drain);
	world_destroy(&w);

	printf("test_far_storage_equiv: %s\n", all_ok ? "PASS" : "FAIL");
	assert(all_ok);
	return all_ok ? 0 : 1;
}
