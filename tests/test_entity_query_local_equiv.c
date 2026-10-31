/*
	test_entity_query_local_equiv -- next-gaps-0924/W2 MOVE P4b (Opus MED-2,
	Astra item 13). EQUIVALENCE: the local section-list query
	(server_local_entity_query_local, Beta Chunk.entities[] walked chunk X asc,
	chunk Z asc, section asc, insertion order -- World.java:2014-2020,
	Chunk.java:495-503) yields EXACTLY the candidate sequence of the exhaustive
	ordered snapshot (server_local_prepare_entity_query_snapshot_bounded: full
	dict walk + sort by chunk_x, chunk_z, chunk_y, list_order, player, id),
	for every span, before and after entities move across sections, are erased
	and re-created (ids never reused here; the world-reset path clears the
	index), with the host player as a member. Then the per-query cost of both
	at 100 mobs + 300 items (PC wall clock; the Wii number is owed to
	perf_stress_5m).
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#include "item/items.h"
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

static uint32_t s_rng = 0x2545F491u;
static uint32_t rnd(void) {
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}
static float rndf(float lo, float hi) {
	return lo + (hi - lo) * (float)(rnd() % 10000u) / 10000.0f;
}

#define MAX_REFS 512
struct snap {
	size_t n;
	struct entity* e[MAX_REFS];
	bool player[MAX_REFS];
};

static void take(struct snap* out) {
	size_t n = 0;
	const struct server_entity_query_ref* refs
		= server_local_entity_query_snapshot(&s, &n);
	assert(n <= MAX_REFS);
	out->n = n;
	for(size_t i = 0; i < n; i++) {
		out->e[i] = refs[i].entity;
		out->player[i] = refs[i].player;
	}
}

static int s_compared, s_mismatch;

static void compare_span(int32_t cx0, int32_t cz0, int32_t cx1, int32_t cz1,
						 int32_t cy0, int32_t cy1) {
	struct snap a, b;
	assert(server_local_prepare_entity_query_snapshot_bounded(&s, cx0, cz0, cx1,
															   cz1, cy0, cy1));
	take(&a);
	assert(server_local_entity_query_local(&s, cx0, cz0, cx1, cz1, cy0, cy1));
	take(&b);
	s_compared++;
	bool same = a.n == b.n;
	for(size_t i = 0; same && i < a.n; i++)
		same = a.e[i] == b.e[i] && a.player[i] == b.player[i];
	if(!same) {
		s_mismatch++;
		if(s_mismatch <= 5)
			printf("MISMATCH span cx %d..%d cz %d..%d cy %d..%d: snapshot %zu "
				   "refs, local %zu refs\n",
				   cx0, cx1, cz0, cz1, cy0, cy1, a.n, b.n);
	}
}

static void compare_many(const char* label) {
	int before = s_mismatch;
	/* every 1..3-chunk span over the populated area, all section ranges */
	for(int32_t cx = -3; cx <= 2; cx++)
		for(int32_t cz = -3; cz <= 2; cz++)
			for(int w = 0; w < 3; w++) {
				compare_span(cx, cz, cx + w, cz + w, 0, 7);
				compare_span(cx, cz, cx + w, cz, 6, 6);
				compare_span(cx, cz, cx, cz + w, 5, 7);
			}
	/* whole populated fixture: loaded chunks -3..3 cover every member and moved position */
	compare_span(-3, -3, 3, 3, 0, 7);
	printf("%s %s (%d spans, %d mismatches)\n",
		   s_mismatch == before ? "PASS" : "FAIL", label, s_compared,
		   s_mismatch - before);
}

static struct entity* make_item(uint32_t id, float x, float y, float z) {
	struct entity* e = malloc(sizeof *e);
	assert(e);
	memset(e, 0, sizeof *e);
	entity_item(id, e, true, &s.worlds[0],
				(struct item_data) {.id = BLOCK_COBBLESTONE, .count = 1});
	e->teleport(e, (vec3) {x, y, z});
	*dict_entity_safe_get(s.entities, id) = e;
	server_local_refresh_entity_membership(&s, e);
	return e;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_entity_query_local_equiv_tmp", s_fixture, sizeof s_fixture);
	assert(clin_init());
	blocks_init();
	items_init();
	MKDIR(s_fixture);
	MKDIR(test_fixture_join(s_fixture, "/region"));
	remove(test_fixture_join(s_fixture, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, s_fixture);
	memset(&s, 0, sizeof(s));
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 128;
	s.rand_src.seed = 0x51DEB002u;
	s.entity_rng.seed = 0x13579BDFu;
	dict_entity_init(s.entities);
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	struct server_chunk* sc = NULL;
	for(w_coord_t cx = -3; cx <= 3; cx++)
		for(w_coord_t cz = -3; cz <= 3; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	/* the host player is a member too */
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 3.5;
	s.player.y = 101.0 + 1.62;
	s.player.z = -7.5;
	server_local_refresh_player_membership(&s);

	/* 100 mobs + 300 items scattered over chunks -3..2, y 60..120 (several
	 * sections), spawned in an interleaved order so list orders mix */
	enum { NM = 100, NI = 300 };
	struct entity* mobs[NM];
	struct entity* items[NI];
	int im = 0, ii = 0;
	uint32_t next_item_id = 500000;
	while(im < NM || ii < NI) {
		if(im < NM && (ii >= NI || (rnd() & 3) == 0)) {
			vec3 pos = {rndf(-40.0f, 40.0f), rndf(60.0f, 120.0f), rndf(-40.0f, 40.0f)};
			mobs[im] = server_local_spawn_monster(pos, MONSTER_PIG_ZOMBIE, &s);
			assert(mobs[im]);
			im++;
		} else {
			items[ii] = make_item(next_item_id++, rndf(-40.0f, 40.0f),
								  rndf(60.0f, 120.0f), rndf(-40.0f, 40.0f));
			ii++;
		}
	}
	compare_many("1 fresh population");

	/* move everything (some across sections), re-refresh memberships in a
	 * scrambled order, move the player too */
	for(int round = 0; round < 3; round++) {
		for(int k = 0; k < NM + NI; k++) {
			struct entity* e = k < NM ? mobs[k] : items[k - NM];
			if(!e)
				continue;
			e->pos[0] = rndf(-40.0f, 40.0f);
			e->pos[1] = rndf(60.0f, 120.0f);
			e->pos[2] = rndf(-40.0f, 40.0f);
		}
		for(int pass = 0; pass < 2; pass++)
			for(int k = NM + NI - 1; k >= 0; k -= 1 + (int)(rnd() % 3)) {
				struct entity* e = k < NM ? mobs[k] : items[k - NM];
				if(e)
					server_local_refresh_entity_membership(&s, e);
			}
		for(int k = 0; k < NM + NI; k++) {
			struct entity* e = k < NM ? mobs[k] : items[k - NM];
			if(e)
				server_local_refresh_entity_membership(&s, e);
		}
		s.player.x = rndf(-40.0f, 40.0f);
		s.player.z = rndf(-40.0f, 40.0f);
		server_local_refresh_player_membership(&s);
		compare_many("2 after a scramble round");
	}

	/* erase every third item and every fifth mob (free + dict erase, no
	 * hook), re-create half of the erased items with fresh ids */
	for(int k = 0; k < NI; k += 3) {
		dict_entity_erase(s.entities, items[k]->id);
		free(items[k]);
		items[k] = NULL;
	}
	for(int k = 0; k < NM; k += 5) {
		uint32_t id = mobs[k]->id;
		dict_entity_erase(s.entities, id);
		free(mobs[k]);
		mobs[k] = NULL;
	}
	for(int k = 0; k < NI; k += 6)
		items[k] = make_item(next_item_id++, rndf(-40.0f, 40.0f),
							 rndf(60.0f, 120.0f), rndf(-40.0f, 40.0f));
	compare_many("3 after erasures and re-creation");
	/* player absent */
	s.player.has_pos = false;
	compare_many("4 player without a position");
	s.player.has_pos = true;

	/* cost: a mob-sized span (2x2 chunks, 2 sections) around a busy spot */
	const int Q = 4000;
	clock_t t0 = clock();
	for(int q = 0; q < Q; q++)
		assert(server_local_prepare_entity_query_snapshot_bounded(
			&s, -1 + (q & 1), -1, (q & 1), 0, 5, 6));
	clock_t t1 = clock();
	for(int q = 0; q < Q; q++)
		assert(server_local_entity_query_local(&s, -1 + (q & 1), -1, (q & 1), 0, 5, 6));
	clock_t t2 = clock();
	size_t live = dict_entity_size(s.entities);
	printf("MEASURE per-query cost, %zu live entities (%d mobs + %d items spawned, "
		   "erasures applied): exhaustive snapshot %.2f us, local sections %.2f us "
		   "(PC wall clock, %d queries each)\n",
		   live, NM, NI, 1e6 * (double)(t1 - t0) / CLOCKS_PER_SEC / Q,
		   1e6 * (double)(t2 - t1) / CLOCKS_PER_SEC / Q, Q);

	g_drain_run = 0;
	thread_join(&drain);
	if(s_mismatch) {
		fprintf(stderr, "test_entity_query_local_equiv: %d of %d spans differ\n",
				s_mismatch, s_compared);
		return 1;
	}
	printf("PASS test_entity_query_local_equiv (%d spans identical)\n", s_compared);
	return 0;
}
