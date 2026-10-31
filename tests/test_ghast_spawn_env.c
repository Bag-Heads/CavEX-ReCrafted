/*
	Ghast spawn-channel fix (2026-09-01). CONFIRMED DEFECT (PC investigation,
	2026-09-01, live natural-spawn measurement, 15 fresh-population trials):
	1/15 trials produced any ghast at all (avg 0.067 ghast) vs 9.933
	pig-zombies pinned near the shared cap -- ghast AI/flight/fireball are
	byte-faithful (see tests/test_nether_mobs_env.c S3/S4); the ONLY problem
	was spawn RATE.

	Root cause [740c583: SpawnerAnimals.performSpawning + EnumCreatureType]:
	Beta gates each creature CATEGORY before its per-chunk loop via
	`countEntities(category) <= category.maxNumberOfCreature * eligibleChunks
	/ 256`; EnumCreatureType.monster's maxNumberOfCreature is 70 -- ONE shared
	budget across every IMob (ghast AND pig zombie both implement IMob, so
	Beta genuinely shares a category cap between them -- same SHAPE as this
	port's flat MOB_SPAWN_CAP, just Wii-shrunk from a chunk-scaled ~70 down to
	a flat 10). BiomeGenHell's list is ghast(10)/pig-zombie(10) -- fair odds
	per attempt -- but a pig-zombie PACK places up to 4
	(PACK_SPAWN_MEMBERS) per successful roll while a ghast places at most 1
	[EntityGhast.getMaxSpawnedInChunk] AND needs its own extra
	`rand.nextInt(20)==0` [EntityGhast.getCanSpawnHere] to even attempt. At
	Beta's real ~70-slot budget that imbalance barely matters; crushed to a
	flat 10 it starves the rarer roll outright, and the pass's own
	`monster_count >= MOB_SPAWN_CAP` top-of-pass early return then blocks
	EVERY later cycle too (pig zombies barely despawn -- neutral, no light
	gate, only the 1-in-800/32-block aged roll).

	Fix: GHAST_SPAWN_CAP (2) in source/network/server_local.c -- an
	independent budget for ghasts, counted (`ghast_count`) and capped
	entirely separately from the ground-hostile `monster_count`/
	MOB_SPAWN_CAP. Same "one category can't starve another" shape
	ANIMAL_SPAWN_CAP / WATER_SPAWN_CAP already use. See
	docs/deviations.md ("Nether ghast sightings starved by the Wii hostile
	cap") for the full equivalence argument this file proves:

	  S1 -- OVERWORLD is a provable no-op. `ghast_eligible_dim` is only true
	       in the Nether, so every new branch collapses back to its pre-fix
	       form there. Proven EXECUTABLY: with the ground-hostile cap already
	       full, run many capped spawn-pass calls and assert rand_src is
	       UNTOUCHED (byte-identical to the pre-fix top-of-pass early return,
	       which never read rand_src at all).
	  S2 -- NETHER ghast presence: many fresh-population trials over the
	       REAL server_local_spawn_mobs path (via the test-only
	       server_local_test_spawn_mobs hook, on real Nether terrain) must
	       show ghasts in a strong majority of trials with a sane average,
	       AND pig-zombie population staying healthy (not starved the other
	       way -- the fix must not become a new imbalance).
	  S3 -- the ghast channel itself never exceeds GHAST_SPAWN_CAP, and the
	       ground channel never exceeds MOB_SPAWN_CAP, across a long
	       single-population run (no channel bleeds into the other's slots).
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
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "world.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_ghast_spawn_tmp' (a literal) */
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

static size_t count_hostile(int mob_id) {
	size_t n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(e && e->type == ENTITY_MONSTER && e->delay_destroy != 0
		   && e->data.monster.id == mob_id)
			n++;
	}
	return n;
}

static void clear_entities(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(e)
			free(e);
	}
	dict_entity_reset(s.entities);
}

static struct entity* spawn_mob(int mob, float x, float y, float z) {
	uint32_t id = server_local_gen_id(&s);
	struct entity** pp = dict_entity_safe_get(s.entities, id);
	*pp = malloc(sizeof(**pp));
	assert(*pp);
	entity_monster(id, *pp, true, &s.world, mob);
	(*pp)->teleport(*pp, (vec3) {x, y, z});
	assert(server_local_test_entity_join(&s, id));
	return *pp;
}

// Reseed the shared LCG mid-process the same way rand_gen_seed's own reset
// does (util.h: seed + clear the cached nextGaussian), without going through
// rand_gen_seed itself so every trial is a REPRODUCIBLE fixed value, not a
// wall-clock draw -- test_nether_mobs_env.c uses the same direct-field
// convention (`s.rand_src.seed = 0x1234567u;`).
static void reseed(uint32_t seed) {
	s.rand_src.seed = seed;
	s.rand_src.has_next_gaussian = false;
	s.rand_src.next_gaussian = 0.0f;
}

int main(void) {
	test_fixture_dir("test_ghast_spawn_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();

	int mob_cap = server_local_test_mob_spawn_cap();
	int ghast_cap = server_local_test_ghast_spawn_cap();
	printf("test_ghast_spawn_env: MOB_SPAWN_CAP=%d GHAST_SPAWN_CAP=%d\n",
		  mob_cap, ghast_cap);
	fflush(stdout);
	assert(mob_cap > 0 && ghast_cap > 0
		  && "both channels must have real headroom");

	/* ============ S1: overworld equivalence -- capped pass touches NO RNG === */
	{
		MKDIR(test_fixture_join(TMP_WORLD, "_ow"));
		MKDIR(test_fixture_join(TMP_WORLD, "_ow/region"));
		string_t level;
		string_init_set_str(level, test_fixture_join(TMP_WORLD, "_ow"));
		memset(&s, 0, sizeof(s));
		server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 555,
						   true /*superflat*/);
		s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
		s.scheduled_tick_budget = 0;
		s.render_demo_mob = -1;
		dict_entity_init(s.entities);
		assert(inventory_create(&s.player.inventory, &inventory_logic_player,
								&s, &s.player, INVENTORY_SIZE, 0, 0, 0));
		s.player.active_inventory = &s.player.inventory;
		for(int cx = -1; cx <= 1; cx++)
			for(int cz = -1; cz <= 1; cz++) {
				struct server_chunk* sc = NULL;
				assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
			}
		s.player.has_pos = true;
		s.player.finished_loading = true;
		s.player.x = 8.5;
		s.player.y = 5.0;
		s.player.z = 8.5;

		// Fill the ground-hostile cap directly (bypass natural spawn -- only
		// the CAPPED state matters here, not how it was reached).
		for(int i = 0; i < mob_cap; i++)
			spawn_mob(MONSTER_ZOMBIE, 8.5f + (float)i, 5.0f, 8.5f);
		assert((int)count_hostile(MONSTER_ZOMBIE) == mob_cap);

		reseed(0xC0FFEEu);
		uint32_t seed_before = s.rand_src.seed;
		bool gaussian_before = s.rand_src.has_next_gaussian;

		// The pre-fix code's ENTIRE behaviour once monster_count>=MOB_SPAWN_CAP
		// was `return;` before touching rand_src at all. Prove the fix keeps
		// that exact behaviour for a non-Nether world across MANY calls (the
		// "~2000 capped passes" identity check): if the ghast-headroom OR
		// ever leaked into a dimension that can't pick a ghast, this loop
		// would perturb the seed and the assert below would catch it.
		for(int i = 0; i < 2000; i++)
			server_local_test_spawn_mobs(&s);

		assert(s.rand_src.seed == seed_before
			  && "overworld capped pass must NOT touch rand_src (ghast "
				 "channel is Nether-only -- see ghast_eligible_dim)");
		assert(s.rand_src.has_next_gaussian == gaussian_before);
		assert((int)count_hostile(MONSTER_ZOMBIE) == mob_cap
			  && "capped ground population must not grow either");
		printf("  S1: overworld capped pass is a provable no-op across 2000 "
			  "calls (rand_src frozen)\n");
		fflush(stdout);

		clear_entities();
		server_world_destroy(&s.world);
		string_clear(level);
	}

	/* ============ S2/S3: Nether ghast presence + no channel bleed ========== */
	{
		MKDIR(TMP_WORLD);
		MKDIR(test_fixture_join(TMP_WORLD, "/region"));
		string_t level;
		string_init_set_str(level, TMP_WORLD);
		memset(&s, 0, sizeof(s));
		// Real (non-superflat) Nether terrain: worldgen_hell's cave/cavern
		// system gives solid-floor/2-tall-air cells across most of the
		// height range, unlike a thin superflat crust -- a fair sample of
		// what the natural-spawn ring actually sees in real play.
		server_world_create(&s.world, level, WORLD_DIM_NETHER, 99881,
						   false /*real Nether worldgen*/);
		s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
		s.scheduled_tick_budget = 0;
		s.render_demo_mob = -1;
		dict_entity_init(s.entities);
		assert(inventory_create(&s.player.inventory, &inventory_logic_player,
								&s, &s.player, INVENTORY_SIZE, 0, 0, 0));
		s.player.active_inventory = &s.player.inventory;

		// Load every chunk the MOB_SPAWN_MAX_DIST=56 ring (plus the ghast
		// 4x4x4 clearance margin) can ever reach from a player parked at the
		// origin chunk's block (8.5, *, 8.5): floor((8.5-56-2)/16)=-4 ..
		// floor((8.5+56+2)/16)=4.
		for(int cx = -4; cx <= 4; cx++)
			for(int cz = -4; cz <= 4; cz++) {
				struct server_chunk* sc = NULL;
				assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
			}
		s.player.has_pos = true;
		s.player.finished_loading = true;
		s.player.x = 8.5;
		s.player.y = 64.0; // irrelevant to the ring (x/z only) or the
							// independently-drawn spawn Y
		s.player.z = 8.5;

		enum { TRIALS = 24, TICKS_PER_TRIAL = 1200 };
		int ghast_trials_hit = 0;
		long ghast_total = 0, pigzombie_total = 0;
		int ghast_over_cap = 0, pigzombie_over_cap = 0;

		for(int trial = 0; trial < TRIALS; trial++) {
			clear_entities();
			// A distinct, reproducible seed per trial -- a real "fresh
			// population" (new world/session), not a continuation.
			reseed(0x1234567u + (uint32_t)trial * 0x9E3779B1u);

			int trial_max_ghast = 0, trial_max_pz = 0;
			for(int t = 0; t < TICKS_PER_TRIAL; t++) {
				server_local_test_spawn_mobs(&s);
				int gc = (int)count_hostile(MONSTER_GHAST);
				int pc = (int)count_hostile(MONSTER_PIG_ZOMBIE);
				if(gc > trial_max_ghast)
					trial_max_ghast = gc;
				if(pc > trial_max_pz)
					trial_max_pz = pc;
				if(gc > ghast_cap)
					ghast_over_cap++;
				if(pc > mob_cap)
					pigzombie_over_cap++;
			}

			int ghast_final = (int)count_hostile(MONSTER_GHAST);
			int pz_final = (int)count_hostile(MONSTER_PIG_ZOMBIE);
			if(ghast_final > 0)
				ghast_trials_hit++;
			ghast_total += ghast_final;
			pigzombie_total += pz_final;
		}

		double ghast_avg = (double)ghast_total / TRIALS;
		double pz_avg = (double)pigzombie_total / TRIALS;
		printf("  S2: %d/%d trials had >=1 ghast, avg ghast=%.3f "
			  "avg pig-zombie=%.3f (cap %d/%d)\n",
			  ghast_trials_hit, TRIALS, ghast_avg, pz_avg, ghast_cap, mob_cap);
		fflush(stdout);

		// S3: neither channel ever bled past its own cap, over the whole run.
		assert(ghast_over_cap == 0
			  && "ghast_count must never exceed GHAST_SPAWN_CAP");
		assert(pigzombie_over_cap == 0
			  && "monster_count must never exceed MOB_SPAWN_CAP");

		// S2 (the fix): before the fix, measured 1/15 (0.067 avg). After the
		// fix, ghasts must reach a strong majority of trials with a sane
		// average -- NOT full parity with the ground channel (GHAST_SPAWN_CAP
		// is deliberately small headroom, not the ground cap), but no longer
		// a near-zero fluke.
		assert(ghast_trials_hit >= (TRIALS * 3) / 4
			  && "ghasts must now appear in a strong majority of fresh "
				 "Nether populations (pre-fix measured 1/15)");
		assert(ghast_avg >= 0.5 && ghast_avg <= (double)ghast_cap
			  && "average ghast presence must be sane (headroom, not "
				 "parity with the ground cap)");

		// The other half of the equivalence claim: pig zombies must stay
		// HEALTHY, not starved the other way by the extra Nether-only
		// attempts the ghast headroom now spends once the ground cap is
		// full. A thriving pig-zombie population (near its own cap) proves
		// the fix didn't just move the starvation onto the other mob.
		assert(pz_avg >= (double)mob_cap * 0.5
			  && "pig-zombie population must stay healthy, not starved by "
				 "the ghast channel's extra attempts");
		printf("  S2: pig-zombie population stays healthy (not starved the "
			  "other way)\n");

		clear_entities();
		server_world_destroy(&s.world);
		string_clear(level);
	}

	g_drain_run = 0;
	thread_join(&drain);
	puts("test_ghast_spawn_env: all sections OK");
	test_fixture_dir_cleanup(TMP_WORLD);
	/* the S1/S2 nether-vs-overworld split test's own sibling fixture (a separate
	 * "<TMP_WORLD>_ow" tree, not a child of TMP_WORLD) -- pre-existing behavior left
	 * this one uncleaned even before the fixture moved to a per-process path; now
	 * that every run's name is unique, an uncleaned sibling would accumulate
	 * across runs instead of being overwritten in place, so it gets the same
	 * cleanup here. */
	test_fixture_dir_cleanup(test_fixture_join(TMP_WORLD, "_ow"));
	return 0;
}
