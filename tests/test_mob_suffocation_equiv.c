/*
	next-gaps-0924 W1B-b (Opus review item 5): T1 equivalence proof for the
	cell de-duplication inside entity_monster_inside_opaque_block_at
	(source/entity/entity_monster.c).

	Beta 1.7.3 Entity.isEntityInsideOpaqueBlock (Entity.java:851-863) reads
	eight corner cells of a (0.9*width) x 0.1 x (0.9*width) box around the eye
	and returns on the first World.isBlockNormalCube hit. The production body
	skips a corner whose cell was already read in the same call. Since the
	verdict is a pure function of the cell and the first-true order is kept,
	the result must be byte-identical to the naive transcription below for
	every position -- this sweep proves it over a checkerboard world (every
	cell boundary matters), random positions, widths 0.3..4.0 (chicken to
	ghast) and eye heights 0.2..3.4, and also checks the read count never
	exceeds the naive loop's (server_world get_block instrument).
*/

#include <assert.h>
#include <math.h>
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
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture[128];
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

/* Naive transcription of Entity.isEntityInsideOpaqueBlock: eight reads, no
 * de-duplication -- the reference the production body must match. */
static bool naive_inside_opaque(float x, float y, float z, float width, float eye) {
	for(int var1 = 0; var1 < 8; ++var1) {
		float var2 = ((float)((var1 >> 0) % 2) - 0.5F) * width * 0.9F;
		float var3 = ((float)((var1 >> 1) % 2) - 0.5F) * 0.1F;
		float var4 = ((float)((var1 >> 2) % 2) - 0.5F) * width * 0.9F;
		w_coord_t var5 = (w_coord_t)floor((double)x + (double)var2);
		w_coord_t var6 = (w_coord_t)floor((double)y + (double)eye + (double)var3);
		w_coord_t var7 = (w_coord_t)floor((double)z + (double)var4);
		struct block_data cell;
		if(server_world_get_block(&s.world, var5, var6, var7, &cell)
		   && block_is_normal_cube(cell.type))
			return true;
	}
	return false;
}

/* Small deterministic LCG for the sweep (not the game RNG: nothing here is
 * simulation, and the test must not disturb rand_src). */
static uint32_t g_lcg = 0x2545F491u;
static float frand(void) {
	g_lcg = g_lcg * 1664525u + 1013904223u;
	return (float)(g_lcg >> 8) / 16777216.0f;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_mob_suffocation_equiv_tmp", s_fixture, sizeof s_fixture);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();

	MKDIR(s_fixture);
	MKDIR(test_fixture_join(s_fixture, "/region"));
	remove(test_fixture_join(s_fixture, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, s_fixture);
	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;

	struct server_chunk* sc = NULL;
	for(w_coord_t cx = -1; cx <= 2; cx++)
		for(w_coord_t cz = -1; cz <= 2; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	/* Checkerboard: stone where (x+y+z) is even, air otherwise, over
	 * x 0..31, y 60..79, z 0..31 -- every cell boundary flips the verdict. Set
	 * silently (no neighbour notify storms for 10k blocks). */
	for(int x = 0; x < 32; x++)
		for(int y = 60; y < 80; y++)
			for(int z = 0; z < 32; z++) {
				uint8_t want = (uint8_t)(((x + y + z) & 1) ? BLOCK_AIR : BLOCK_STONE);
				struct block_data cur;
				assert(server_world_get_block(&s.world, x, y, z, &cur));
				if(cur.type == want)
					continue; /* raw write of an identical block is a no-op (NULL) */
				assert(server_world_set_block_silent(&s.worlds[0], x, y, z,
					(struct block_data) {.type = want, .metadata = 0}));
			}

	int us, sn, smax, gn, gmax;
	long reads_prod = 0, reads_naive = 0;
	int mismatches = 0, trues = 0;
	const int N = 20000;
	for(int i = 0; i < N; i++) {
		float x = 2.0f + frand() * 27.0f;
		float y = 61.0f + frand() * 15.0f;
		float z = 2.0f + frand() * 27.0f;
		/* half the samples at mob-sized widths (0.3..1.0: chicken..pig), half
		 * across the full 0.3..4.0 (up to the ghast) -- wide boxes almost
		 * always touch a stone cell on a checkerboard, so this keeps both
		 * verdicts well represented */
		float width = (i & 1) ? 0.3f + frand() * 3.7f : 0.3f + frand() * 0.7f;
		float eye = 0.2f + frand() * 3.2f;
		/* a share of samples sit exactly on cell boundaries */
		if((i & 7) == 0) x = floorf(x) + 0.5f * (float)((i >> 3) & 1);
		if((i & 7) == 1) z = floorf(z);
		if((i & 7) == 2) y = floorf(y) + 0.05f;

		server_world_perf_tick_reset();
		bool prod = entity_monster_inside_opaque_block_at(&s, x, y, z, width, eye);
		server_world_perf_tick_reset();
		server_world_edit_stats(&us, &sn, &smax, &gn, &gmax);
		int rp = gn;

		server_world_perf_tick_reset();
		bool ref = naive_inside_opaque(x, y, z, width, eye);
		server_world_perf_tick_reset();
		server_world_edit_stats(&us, &sn, &smax, &gn, &gmax);
		int rn = gn;

		reads_prod += rp;
		reads_naive += rn;
		trues += ref ? 1 : 0;
		if(prod != ref) {
			printf("MISMATCH at (%.6g, %.6g, %.6g) width %.4g eye %.4g: prod %d ref %d\n", x, y,
				   z, width, eye, (int)prod, (int)ref);
			mismatches++;
		}
		if(rp > rn) {
			printf("READ COUNT at (%.6g, %.6g, %.6g) width %.4g eye %.4g: prod %d > naive %d\n",
				   x, y, z, width, eye, rp, rn);
			mismatches++;
		}
	}
	printf("sweep: %d samples (%d inside), reads prod %ld vs naive %ld (%.2f vs %.2f per call)\n",
		   N, trues, reads_prod, reads_naive, (double)reads_prod / N, (double)reads_naive / N);
	assert(mismatches == 0 && "dedupe must be byte-identical to the 8-read loop and never read more");
	/* Both verdicts must be exercised in bulk (a degenerate world would make
	 * the sweep vacuous). On a 3-D checkerboard a box straddling a cell
	 * boundary in x or z almost always touches stone, so "outside" is the
	 * minority (~9%): require >= 500 of each rather than a 10..90% band. */
	assert(trues >= 500 && (N - trues) >= 500 && "the checkerboard must exercise both verdicts");

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(s_fixture);
	puts("PASS test_mob_suffocation_equiv");
	return 0;
}
