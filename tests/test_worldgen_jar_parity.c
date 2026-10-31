/* GPL-3.0-or-later. Generated-world BLOCK parity against the original Beta
 * 1.7.3 JAR: block IDs and metadata after load + population, seed 4242, both
 * dimensions, chunks -9..13 x -9..13 (529 per dimension).
 *
 * Why: test_oracle_worldgen (mc173) checks raw terrain only, never population,
 * so a wrong tree count (two-argument Perlin axis, NoiseGeneratorPerlin.java:
 * 91-92) went unseen for months (Astra review of 356da47fe, 2026-10-12).
 * The reference is the same authenticated JNI archive the lighting no-downgrade
 * gate reads (tests/oracle/lighting/g9_option_b/fixtures/no-downgrade): 3,174
 * full 82,176-byte chunk snapshots (IDs 0, meta 32768, Sky 49152, Block 65536,
 * height 81920), in the order load, pre-drain, completion per dimension. Light
 * is graded by test_light_no_downgrade; this test grades only what the world
 * IS, which must stay byte-exact (CLAUDE.md worldgen invariants). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "item/items.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "platform/thread.h"
#include "worldgen/worldgen.h"
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p,0777)
#endif

#define SNAPSHOT 82176
#define IDS 32768

static struct server_local session;
static gzFile reference;
static unsigned long long id_diff[2][3], meta_diff[2][3];

static void* consume(void* unused) {
	(void)unused;
	for(;;) { clin_drain_discard(); thread_msleep(1); }
	return NULL;
}

static void compare(int dim, int phase, int x, int z) {
	struct server_chunk* sc = dict_server_chunks_get(session.worlds[0].chunks,
		S_CHUNK_ID(x, z));
	assert(sc);
	static unsigned char raw[SNAPSHOT];
	assert(gzread(reference, raw, SNAPSHOT) == SNAPSHOT);
	unsigned ids = 0, meta = 0;
	int first = -1;
	for(unsigned i = 0; i < IDS; i++) {
		unsigned a = (sc->metadata[i / 2] >> ((i & 1) * 4)) & 15;
		unsigned b = (raw[IDS + i / 2] >> ((i & 1) * 4)) & 15;
		bool id_bad = sc->ids[i] != raw[i];
		ids += id_bad;
		meta += a != b;
		if(first < 0 && (id_bad || a != b)) first = (int)i;
	}
	id_diff[dim][phase] += ids;
	meta_diff[dim][phase] += meta;
	if(first >= 0) /* Beta index = x << 11 | z << 7 | y */
		printf("DIFF dim=%d phase=%d chunk=(%d,%d) ids=%u meta=%u first=(%d,%d,%d) "
			   "cw=%u:%u beta=%u:%u\n", dim ? -1 : 0, phase, x, z, ids, meta,
			   first >> 11, first & 127, (first >> 7) & 15, sc->ids[first],
			   (sc->metadata[first / 2] >> ((first & 1) * 4)) & 15, raw[first],
			   (raw[IDS + first / 2] >> ((first & 1) * 4)) & 15);
}

int main(int argc, char** argv) {
	assert(argc == 2);
	setvbuf(stdout, NULL, _IONBF, 0);
	reference = gzopen(argv[1], "rb");
	assert(reference);
	assert(clin_init() && svin_init());
	blocks_init();
	items_init();
	struct thread drain;
	assert(thread_create(&drain, consume, NULL, 64));
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	for(int dim = 0; dim < 2; dim++) {
		char dir[256];
		test_fixture_dir(dim ? "worldgen_jar_parity_nether" : "worldgen_jar_parity_overworld",
						 dir, sizeof dir);
		assert(MKDIR(dir) == 0);
		memset(&session, 0, sizeof session);
		dict_entity_init(session.entities);
		string_t level;
		string_init_set_str(level, dir);
		struct server_world* w = &session.worlds[0];
		server_world_create(w, level, dim ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD, 4242, false);
		string_clear(level);
		w->session = w->local = &session;
		jrand_set_seed(&w->rand, 4242);
		/* Same load order as the capture (beta_jni.py): x-major, one snapshot
		 * per load, then the whole domain before and after one drain. */
		for(int x = -9; x <= 13; x++)
			for(int z = -9; z <= 13; z++) {
				struct server_chunk* sc = NULL;
				assert(server_world_load_chunk(w, x, z, &sc) && sc);
				compare(dim, 0, x, z);
			}
		for(int phase = 1; phase <= 2; phase++) {
			if(phase == 2)
				assert(server_world_light_drain(w));
			assert(dict_server_chunks_size(w->chunks) == 529);
			for(int x = -9; x <= 13; x++)
				for(int z = -9; z <= 13; z++)
					compare(dim, phase, x, z);
		}
		server_world_discard(w);
		dict_entity_clear(session.entities);
		test_fixture_dir_cleanup(dir);
	}
	unsigned char extra;
	assert(gzread(reference, &extra, 1) == 0 && gzeof(reference));
	assert(gzclose(reference) == Z_OK);
	unsigned long long total = 0;
	for(int d = 0; d < 2; d++)
		for(int p = 0; p < 3; p++) {
			printf("TOTAL dim=%d phase=%s ids=%llu meta=%llu\n", d ? -1 : 0,
				   (const char*[]) {"load", "pre-drain", "completion"}[p],
				   id_diff[d][p], meta_diff[d][p]);
			total += id_diff[d][p] + meta_diff[d][p];
		}
	if(total) {
		printf("FAIL: generated blocks differ from the original Beta 1.7.3 JAR\n");
		return 1;
	}
	printf("PASS: all 3,174 snapshots byte-identical in block IDs and metadata\n");
	return 0;
}
