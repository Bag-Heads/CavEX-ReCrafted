/*
	B7 regression: a blast that breaks MORE than 512 unique blocks must break
	ALL of them -- nothing may be silently dropped past a fixed cap.

	History (bug hunt item B7/P1): server_world_explode_now deduplicated the
	destroyed cells with an O(N^2) scan over a fixed `broken[512]` array and
	silently discarded every unique cell past entry 512 (`if (!seen && bc <
	512)`). Any blast exceeding the cap left blocks floating inside the
	crater -- no AIR write, no drop, no lighting -- with no diagnostic. The
	in-tree Nether bed detonation (power 5.0) already comes within ~2% of the
	cap in solid terrain; the fix (O(1) coordinate hash set + blast-sized
	transient staging) must hold for ANY power.

	Fixture: a solid 21x19x21 dirt monolith on the superflat debug stage, one
	synchronous power-16 blast at its centre, then a before/after scan of the
	blast window. The assertion `broken > 512` FAILS on the pre-fix code by
	construction (bc could never exceed 512) and passes once the cap is gone.
	RNG is seeded to a constant so the break set is deterministic run-to-run.
*/

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h> // _getpid
#define MKDIR(p) _mkdir(p)
#define GETPID() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h> // getpid
#define MKDIR(p) mkdir(p, 0777)
#define GETPID() getpid()
#endif

#include <m-lib/m-string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/mem2.h"
#include "platform/thread.h"

/* PID-suffixed, not a fixed literal: two live processes must never share one
 * on-disk world (standing test convention since 46d68f3). */
static char g_tmp_world[64];
#define TMP_WORLD g_tmp_world

static const char* tmp_path(const char* suffix) {
	static char buf[96];
	int n = snprintf(buf, sizeof(buf), "%s%s", g_tmp_world, suffix);
	assert(n > 0 && (size_t)n < sizeof(buf));
	return buf;
}

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

/* Blast geometry. The monolith fully contains the power-16 blast sphere
 * (reach = power = 16 blocks in every direction). Power 16 is deliberate:
 * with the fixed EXPLOSION_MAX_RAYS=300 budget the break density falls off
 * with ray sparsity (~300/(4 pi d^2) cells per shell), so a blast must be
 * large before its unique broken set exceeds 512 -- empirically power 10
 * breaks ~420 cells in this fixture, power 16 breaks ~900. */
#define BLAST_POWER   16.0f
#define CENTER_X      40
#define CENTER_Z      40
#define CENTER_Y      13.5f
#define FILL_MIN_X    (CENTER_X - 16)
#define FILL_MAX_X    (CENTER_X + 16)
#define FILL_MIN_Z    (CENTER_Z - 16)
#define FILL_MAX_Z    (CENTER_Z + 16)
#define FILL_MIN_Y    4  /* superflat ground occupies y 0..3 */
#define FILL_MAX_Y    30

/* Scan window: the monolith plus a 2-block margin so crater cells broken in
 * the superflat ground underneath also count. */
#define SCAN_MIN_X    (FILL_MIN_X - 2)
#define SCAN_MAX_X    (FILL_MAX_X + 2)
#define SCAN_MIN_Z    (FILL_MIN_Z - 2)
#define SCAN_MAX_Z    (FILL_MAX_Z + 2)
#define SCAN_MIN_Y    0
#define SCAN_MAX_Y    (FILL_MAX_Y + 2)

#define SCAN_NX       (SCAN_MAX_X - SCAN_MIN_X + 1)
#define SCAN_NY       (SCAN_MAX_Y - SCAN_MIN_Y + 1)
#define SCAN_NZ       (SCAN_MAX_Z - SCAN_MIN_Z + 1)

static uint8_t g_pre[SCAN_NX * SCAN_NY * SCAN_NZ];

static size_t scan_idx(int x, int y, int z) {
	return ((size_t)(x - SCAN_MIN_X) * SCAN_NY + (size_t)(y - SCAN_MIN_Y))
		   * SCAN_NZ + (size_t)(z - SCAN_MIN_Z);
}

static void free_entities(struct server_local* s) {
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_reset(s->entities);
}

int main(void) {
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	int n = snprintf(g_tmp_world, sizeof(g_tmp_world),
					 "test_explosion_large_tnt_tmp_%d", (int)GETPID());
	assert(n > 0 && (size_t)n < sizeof(g_tmp_world));
	MKDIR(TMP_WORLD);
	MKDIR(tmp_path("/region"));
	remove(tmp_path("/region/r.0.0.mcr"));
	remove(tmp_path("/level.dat"));
	remove(tmp_path("/level.dat_new"));
	remove(tmp_path("/level.dat_old"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);
	struct server_local s;
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 128; /* shipping Wii policy */
	/* Deterministic world RNG: the per-visit break rolls and ray directions
	 * must be identical run-to-run so the broken-cell count is pinned. */
	s.rand_src.seed = UINT32_C(0xB1A57B1D);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 200.5;
	s.player.y = 4.5;
	s.player.z = 200.5; /* ~275 blocks out: dist > 2*power -> zero damage */

	/* The blast sphere spans chunks 1..3 on both axes. */
	for(int cx = 1; cx <= 3; cx++)
		for(int cz = 1; cz <= 3; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	/* Solid dirt monolith covering the whole blast sphere. */
	for(int x = FILL_MIN_X; x <= FILL_MAX_X; x++)
		for(int z = FILL_MIN_Z; z <= FILL_MAX_Z; z++)
			for(int y = FILL_MIN_Y; y <= FILL_MAX_Y; y++)
				assert(server_world_set_block(
					&s.worlds[0], x, y, z,
					(struct block_data) {.type = BLOCK_DIRT,
										 .metadata = 0,
										 .sky_light = 0,
										 .torch_light = 0}));

	/* Before-state of the scan window. */
	struct block_data pre;
	for(int x = SCAN_MIN_X; x <= SCAN_MAX_X; x++)
		for(int y = SCAN_MIN_Y; y <= SCAN_MAX_Y; y++)
			for(int z = SCAN_MIN_Z; z <= SCAN_MAX_Z; z++) {
				assert(server_world_get_block(&s.world, x, y, z, &pre));
				g_pre[scan_idx(x, y, z)] = pre.type;
			}

	server_world_explode(&s.worlds[0], (vec3) {CENTER_X + 0.5f, CENTER_Y,
									  CENTER_Z + 0.5f},
						 BLAST_POWER);

	/* After-state: count solid -> AIR transitions. Pre-fix this can NEVER
	 * exceed 512 (bc was hard-capped); the monolith + power 16 yield well
	 * over 512 unique broken cells (908 under the fixed seed). */
	size_t broken_total = 0;
	struct block_data post;
	for(int x = SCAN_MIN_X; x <= SCAN_MAX_X; x++)
		for(int y = SCAN_MIN_Y; y <= SCAN_MAX_Y; y++)
			for(int z = SCAN_MIN_Z; z <= SCAN_MAX_Z; z++) {
				assert(server_world_get_block(&s.world, x, y, z, &post));
				if(g_pre[scan_idx(x, y, z)] != BLOCK_AIR
				   && post.type == BLOCK_AIR)
					broken_total++;
			}
	printf("  blast power %.0f: %zu unique blocks broken (cap was 512)\n",
		   (double)BLAST_POWER, broken_total);
	fflush(stdout);
	assert(broken_total > 512);

	/* The crater core must be fully carved: every cell within 1.5 blocks of
	 * the centre sits at rem/power ~= 1 for multiple ray visits, so a
	 * surviving block there would mean collection itself is dropping cells.
	 * (Deterministic under the fixed seed.) */
	for(int x = CENTER_X - 1; x <= CENTER_X + 1; x++)
		for(int y = 12; y <= 15; y++)
			for(int z = CENTER_Z - 1; z <= CENTER_Z + 1; z++) {
				float dx = (float)x + 0.5f - (CENTER_X + 0.5f);
				float dy = (float)y + 0.5f - CENTER_Y;
				float dz = (float)z + 0.5f - (CENTER_Z + 0.5f);
				if(sqrtf(dx * dx + dy * dy + dz * dz) > 1.5f)
					continue;
				assert(server_world_get_block(&s.world, x, y, z, &post));
				assert(post.type == BLOCK_AIR);
			}

	/* Pass 3 ran over the full set: dirt's 30% drop chance over >512 broken
	 * cells must have produced item entities. */
	assert(dict_entity_size(s.entities) > 0);
	printf("  pass 3 drops: %zu item entities spawned\n",
		   dict_entity_size(s.entities));

	free_entities(&s);
	inventory_destroy(&s.player.inventory, &s.player);
	server_world_destroy(&s.world);
	mem2_free(s.entity_tick_keys);
	s.entity_tick_keys = NULL;
	dict_entity_clear(s.entities);
	string_clear(level);
	g_drain_run = 0;
	thread_join(&drain);
	printf("test_explosion_large_tnt: OK (>512 unique blocks broken, core "
		   "carved, drops spawned)\n");
	return 0;
}
