/*
	Beta 1.7.3 BlockFire spread goldens (decompile 740c583), over a real superflat server_world.
	Exercises block_fire.c's faithful updateTick by driving its onWorldTick directly:
	  - PLACEMENT/DEATH (deterministic): floating fire (no support) is removed; old fire (age>3) on a
	    solid cube with no fuel dies; young fire (age<=3) on a solid cube survives.
	  - CATCH (probabilistic, bounded): a flammable neighbour (planks, abilityToCatchFire>0) eventually
	    catches; a NON-flammable neighbour (stone, ability 0) never does; TNT primes (Beta detonation).
	  - SPREAD: fire beside a wood wall ignites NEW fire in the surrounding air (the 3x3x6 encourage box).
	rand_gen is distribution-faithful (not byte-exact to Beta nextInt), so the probabilistic cases assert
	"eventually within N ticks", not an exact RNG sequence.
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
#include "entity/entity_primed_tnt.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_weather.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_fire_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 70

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
static void set(int x, int y, int z, int type, int meta) {
	struct block_data expected = {
		.type = (uint8_t)type,
		.metadata = (uint8_t)(meta & 0x0F),
	};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type && live.metadata == expected.metadata);
}
static int gid(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.type : -1;
}
static struct entity* find_primed_tnt(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_PRIMED_TNT)
			return e;
		dict_entity_next(it);
	}
	return NULL;
}
extern void block_fire_test_try_catch(struct server_local* server, int x,
									 int y, int z, int chance, int age);

static void clear_entities(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_reset(s.entities);
}

static void test_punch_extinguishes_adjacent_fire_on_start(void) {
	const int x = 10, y = Y, z = 28;
	set(x, y, z, BLOCK_STONE, 0);
	set(x, y + 1, z, BLOCK_FIRE, 0);
	s.player.has_pos = true;
	s.player.finished_loading = true;
	/* Deviation-#18: zero-init health reads as DEAD; be explicitly alive. */
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.active_inventory = &s.player.inventory;
	struct server_rpc dig = {
		.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {
			.x = x, .y = y, .z = z, .side = SIDE_TOP, .finished = false,
		},
	};
	server_local_test_process_rpc(&s, &dig);
	assert(gid(x, y, z) == BLOCK_STONE);
	assert(gid(x, y + 1, z) == BLOCK_AIR);
	puts("  fire: dig-start on support extinguishes adjacent fire before mining OK");
}
// Drive BlockFire.onWorldTick on the fire at (x,y,z), if it is still fire. neighbours are read from
// the world inside the handler (not info->neighbours), so NULL is fine.
static void fire_tick(int x, int y, int z) {
	struct block_data fd;
	if(!server_world_get_block(&s.world, x, y, z, &fd) || fd.type != BLOCK_FIRE)
		return;
	struct block_info info = {.block = &fd, .neighbours = NULL, .x = x, .y = y, .z = z};
	blocks[BLOCK_FIRE]->onWorldTick(&s, &info);
}
// count fire blocks in a box (inclusive)
static int count_fire(int x0, int y0, int z0, int x1, int y1, int z1) {
	int n = 0;
	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++)
				if(gid(x, y, z) == BLOCK_FIRE)
					n++;
	return n;
}

int main(void) {
	test_fixture_dir("test_fire_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 99, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
	test_punch_extinguishes_adjacent_fire_on_start();

	// ===================== PLACEMENT / DEATH (deterministic) =====================
	{
		// A1: floating fire (air below, no flammable neighbour) -> canPlaceBlockAt false -> removed.
		const int x = 4, z = 4;
		set(x, Y, z, BLOCK_FIRE, 0); // nothing below, all air around
		fire_tick(x, Y, z);
		assert(gid(x, Y, z) == BLOCK_AIR);

		// A2: OLD fire (age 15) on a solid cube, no flammable neighbour -> dies (age > 3, no fuel).
		set(x, Y - 1, z, BLOCK_STONE, 0);
		set(x, Y, z, BLOCK_FIRE, 15);
		fire_tick(x, Y, z);
		assert(gid(x, Y, z) == BLOCK_AIR);

		// A3: YOUNG fire (age 0) on a solid cube, no flammable neighbour -> survives the tick.
		set(x, Y, z, BLOCK_FIRE, 0);
		fire_tick(x, Y, z);
		assert(gid(x, Y, z) == BLOCK_FIRE);
		printf("  fire: floating removed; old/no-fuel dies; young-on-solid survives OK\n");
	}

	// ===================== CATCH: flammable yes, non-flammable never =====================
	{
		const int x = 12, z = 4;
		set(x, Y - 1, z, BLOCK_STONE, 0);	   // solid support so the fire persists
		set(x, Y, z, BLOCK_FIRE, 0);
		set(x + 1, Y, z, BLOCK_PLANKS, 0);	   // flammable neighbour (ability 20)
		set(x - 1, Y, z, BLOCK_STONE, 0);	   // NON-flammable neighbour (ability 0): never catches
		set(x - 1, Y - 1, z, BLOCK_STONE, 0);
		bool planks_caught = false;
		for(int k = 0; k < 800 && !planks_caught; k++) {
			fire_tick(x, Y, z);
			if(gid(x + 1, Y, z) != BLOCK_PLANKS)
				planks_caught = true; // burned into fire or consumed to air
			assert(gid(x - 1, Y, z) == BLOCK_STONE); // stone NEVER catches (fire_catch[stone]==0)
			if(gid(x, Y, z) != BLOCK_FIRE)
				set(x, Y, z, BLOCK_FIRE, 0); // re-light the source if it aged out, to keep driving
		}
		assert(planks_caught);
		printf("  fire: flammable planks catch within budget; adjacent stone never catches OK\n");
	}

	// ===================== TNT ignition (Beta detonation -> real primed entity) =====================
	{
		/* Directly pin BlockFire.tryToCatchBlockOnFire's TNT-only ordering.
		 * chance=1 guarantees the catch, while searched deterministic seeds cover
		 * both replacement branches. The oracle advances the same production RNG
		 * calls and the post-state must match exactly. */
		bool covered[2] = {false, false};
		for(uint32_t seed = 1; seed < 10000 && !(covered[0] && covered[1]); seed++) {
			struct random_gen oracle = {.seed = seed};
			assert(rand_gen_int(&oracle, 1) == 0);
			bool leave_fire = rand_gen_int(&oracle, 10) < 5;
			int expected_meta = 0;
			if(leave_fire)
				expected_meta = rand_gen_int(&oracle, 5) / 4;
			if(covered[leave_fire ? 1 : 0])
				continue;

			const int tx = 16 + (leave_fire ? 1 : 0), tz = 12;
			set(tx, Y - 1, tz, BLOCK_STONE, 0);
			set(tx, Y, tz, BLOCK_TNT, 0);
			s.rand_src.seed = seed;
			block_fire_test_try_catch(&s, tx, Y, tz, 1, 0);
			assert(s.rand_src.seed == oracle.seed);
			struct block_data result;
			assert(server_world_get_block(&s.world, tx, Y, tz, &result));
			assert(result.type == (leave_fire ? BLOCK_FIRE : BLOCK_AIR));
			if(leave_fire)
				assert(result.metadata == expected_meta);
			struct entity* exact = find_primed_tnt();
			assert(exact && exact->data.primed_tnt.fuse == TNT_FUSE_DEFAULT);
			covered[leave_fire ? 1 : 0] = true;
			clear_entities();
			set(tx, Y, tz, BLOCK_AIR, 0);
		}
		assert(covered[0] && covered[1]);
		printf("  fire: TNT catch preserves both FIRE/AIR rolls and RNG order OK\n");

		const int x = 18, z = 4;
		set(x, Y - 1, z, BLOCK_STONE, 0);
		set(x, Y, z, BLOCK_FIRE, 0);
		set(x + 1, Y, z, BLOCK_TNT, 0); // dormant TNT (metadata 0)
		bool primed = false;
		for(int k = 0; k < 400 && !primed; k++) {
			fire_tick(x, Y, z);
			if(gid(x + 1, Y, z) != BLOCK_TNT && find_primed_tnt())
				primed = true;
			if(gid(x, Y, z) != BLOCK_FIRE)
				set(x, Y, z, BLOCK_FIRE, 0);
		}
		assert(primed);
		struct entity* tnt = find_primed_tnt();
		assert(tnt && tnt->data.primed_tnt.fuse == TNT_FUSE_DEFAULT);
		assert(fabsf(tnt->pos[0] - (x + 1.5f)) < 1.0e-5f);
		assert(gid(x + 1, Y, z) == BLOCK_AIR
			   || gid(x + 1, Y, z) == BLOCK_FIRE);
		printf("  fire: caught TNT became an 80-tick primed entity after its FIRE/AIR roll OK\n");
	}

	// ===================== SPREAD: ignite NEW fire next to sustained burning fuel =====================
	{
		// A real fire sits on abundant fuel; here we keep the source lit AND its immediate planks fuel
		// present each tick (otherwise the source consumes its lone neighbour, loses fuel, and stops --
		// which would make the probabilistic spread RNG-flaky). With a fueled source, the 3x3x6 encourage
		// box reliably ignites a NEW fire in an air pocket touching the wood within a small budget.
		const int x = 24, z = 4;
		set(x, Y - 1, z, BLOCK_STONE, 0);
		bool spread = false;
		for(int k = 0; k < 1000 && !spread; k++) {
			set(x, Y, z, BLOCK_FIRE, 0);	   // fresh, lit source
			set(x + 1, Y, z, BLOCK_PLANKS, 0); // sustained fuel beside it (so it reaches the spread branch)
			fire_tick(x, Y, z);
			// ANY fire in the box other than the source cell = a genuine fresh ignition.
			for(int bx = x - 1; bx <= x + 2 && !spread; bx++)
				for(int by = Y - 1; by <= Y + 5 && !spread; by++)
					for(int bz = z - 1; bz <= z + 1 && !spread; bz++)
						if(!(bx == x && by == Y && bz == z) && gid(bx, by, bz) == BLOCK_FIRE)
							spread = true;
		}
		assert(spread);
		printf("  fire: spreads NEW fire next to sustained burning fuel OK\n");
	}

	// ============ F1 (adversarial review): rain gates NEW ignitions too ============
	{
		// Beta's ignition loop (the 3x3x6 encourage box) carries its OWN 5-clause
		// canBlockBeRainedOn gate, INSIDE the loop -- not just the top-level
		// extinguish branch. [740c583: BlockFire.updateTick, the
		// `nextInt(var11)<=var13 && (...5 canBlockBeRainedOn clauses...)` compound
		// condition guarding the ignition setBlockAndMetadataWithNotify]
		const int ix = 6, iz = 20;  // EXPOSED: open sky above the whole box
		const int sx = 14, sz = 20; // SHELTERED: roofed above the whole box

		set(ix, Y - 1, iz, BLOCK_STONE, 0);
		set(sx, Y - 1, sz, BLOCK_STONE, 0);
		// Roof the SHELTERED box's full footprint one cell above its top (the
		// ignition loop's by range is y-1..y+4): every candidate cell there
		// fails canBlockSeeTheSky, so canBlockBeRainedOn is false regardless
		// of the raining flag.
		for(int bx = sx - 1; bx <= sx + 1; bx++)
			for(int bz = sz - 1; bz <= sz + 1; bz++)
				set(bx, Y + 5, bz, BLOCK_STONE, 0);

		s.weather.raining = true;
		s.weather.rain_strength = 1.0f;
		s.weather.prev_rain_strength = 1.0f;
		s.weather.thundering = false;
		s.weather.thunder_strength = 0.0f;

		// Fixture validity (not a tautology against the implementation under
		// test -- world_can_block_be_rained_on is the SAME primitive
		// BlockFire's rain-extinguish branch already used and tested,
		// exercised here purely to confirm the geometry/biome give the
		// EXPOSED/SHELTERED split this golden depends on).
		assert(world_can_block_be_rained_on(&s, ix, Y, iz)
			   && "fixture: the exposed ignition-box cell must actually be "
				  "rained on for this golden to test the right thing");
		assert(!world_can_block_be_rained_on(&s, sx, Y, sz)
			   && "fixture: the roofed cell must NOT be rained on (sky "
				  "blocked)");

		bool ignited_exposed = false;
		for(int k = 0; k < 3000 && !ignited_exposed; k++) {
			set(ix, Y, iz, BLOCK_FIRE, 0);
			set(ix + 1, Y, iz, BLOCK_PLANKS, 0);
			fire_tick(ix, Y, iz);
			for(int bx = ix - 1; bx <= ix + 1 && !ignited_exposed; bx++)
				for(int by = Y - 1; by <= Y + 4 && !ignited_exposed; by++)
					for(int bz = iz - 1; bz <= iz + 1 && !ignited_exposed; bz++)
						if(!(bx == ix && by == Y && bz == iz)
						   && gid(bx, by, bz) == BLOCK_FIRE)
							ignited_exposed = true;
		}
		assert(!ignited_exposed
			   && "raining + exposed candidate cell must NEVER ignite new "
				  "fire (3000 trials)");
		printf("  fire: raining + exposed candidate -- 0 ignitions in 3000 "
			   "trials OK\n");

		bool ignited_sheltered = false;
		for(int k = 0; k < 3000 && !ignited_sheltered; k++) {
			set(sx, Y, sz, BLOCK_FIRE, 0);
			set(sx + 1, Y, sz, BLOCK_PLANKS, 0);
			fire_tick(sx, Y, sz);
			for(int bx = sx - 1; bx <= sx + 1 && !ignited_sheltered; bx++)
				for(int by = Y - 1; by <= Y + 4 && !ignited_sheltered; by++)
					for(int bz = sz - 1; bz <= sz + 1 && !ignited_sheltered;
						bz++)
						if(!(bx == sx && by == Y && bz == sz)
						   && gid(bx, by, bz) == BLOCK_FIRE)
							ignited_sheltered = true;
		}
		assert(ignited_sheltered
			   && "raining + SHELTERED candidate cell can still ignite "
				  "(the extinguish gate is per-cell, not a blanket rain "
				  "freeze on the whole block)");
		printf("  fire: raining + sheltered candidate -- ignites within "
			   "budget OK\n");

		// Not raining: the SAME exposed cell/spot must now ignite normally
		// (rules out "always blocked at this spot" as a false pass above).
		s.weather.raining = false;
		s.weather.rain_strength = 0.0f;
		s.weather.prev_rain_strength = 0.0f;
		assert(!world_can_block_be_rained_on(&s, ix, Y, iz));

		bool ignited_not_raining = false;
		for(int k = 0; k < 3000 && !ignited_not_raining; k++) {
			set(ix, Y, iz, BLOCK_FIRE, 0);
			set(ix + 1, Y, iz, BLOCK_PLANKS, 0);
			fire_tick(ix, Y, iz);
			for(int bx = ix - 1; bx <= ix + 1 && !ignited_not_raining; bx++)
				for(int by = Y - 1; by <= Y + 4 && !ignited_not_raining; by++)
					for(int bz = iz - 1; bz <= iz + 1 && !ignited_not_raining;
						bz++)
						if(!(bx == ix && by == Y && bz == iz)
						   && gid(bx, by, bz) == BLOCK_FIRE)
							ignited_not_raining = true;
		}
		assert(ignited_not_raining
			   && "not raining -> the same exposed spot ignites normally "
				  "(ignition is unaffected when it isn't raining)");
		printf("  fire: not raining -- the same exposed spot ignites "
			   "normally OK\n");
	}

	// ===================== ETERNAL FIRE on NETHERRACK (deterministic) =====================
	{
		// Beta BlockFire.updateTick var6: fire directly above netherrack never dies -- no fuel needed,
		// no age-15 burn-out -- it only ever spreads. Control: the SAME setup over stone dies. var6 is a
		// plain id compare (below == netherrack), NOT a dimension check, so this reproduces in the
		// overworld harness. [decompile 740c583 BlockFire.updateTick]
		// Both spots must sit in a LOADED chunk: the harness loads only chunks (0..1,0..1) = world
		// x/z in [0,31]. Keep clear of the earlier cases (all at z=4).
		const int ex = 30, ez = 10, cx = 26, cz = 10;

		// Control over STONE, no flammable neighbour, age 15 -> dies on the first tick (age > 3, no fuel).
		set(cx, Y - 1, cz, BLOCK_STONE, 0);
		set(cx, Y, cz, BLOCK_FIRE, 15);
		fire_tick(cx, Y, cz);
		assert(gid(cx, Y, cz) == BLOCK_AIR);

		// Eternal over NETHERRACK, no flammable neighbour, age 15 -> survives indefinitely.
		set(ex, Y - 1, ez, BLOCK_NETHERRACK, 0);
		set(ex, Y, ez, BLOCK_FIRE, 15);
		for(int k = 0; k < 200; k++) {
			fire_tick(ex, Y, ez);
			assert(gid(ex, Y, ez) == BLOCK_FIRE); // never clears while netherrack is below
		}
		printf("  fire: eternal over netherrack (never dies); dies over stone OK\n");
	}

	// ============ STILL LAVA random fire ignition (Beta BlockStationary.updateTick) ============
	{
		// DRAW-ORDER golden via a MIRRORED generator: before each trial, copy the seeded rand
		// state and independently replay Beta's exact walk -- one nextInt(3) for the step count
		// (drawn even when 0), then per step nextInt(3)-1 for x, y+1, nextInt(3)-1 for z,
		// positions cumulative; an AIR landing cell with a flammable-material neighbour takes
		// fire and returns, a solid cell aborts. After driving the real handler we assert BOTH
		// the world diff matches the mirror's prediction AND the generator state is IDENTICAL
		// (memcmp) -- i.e. the exact same number of draws in the exact same order.
		// [decompile 740c583 BlockStationary.updateTick + Material.getBurning]
		const int lx = 6, lz = 24;
		set(lx, Y - 1, lz, BLOCK_STONE, 0);
		set(lx, Y, lz, BLOCK_LAVA_STILL, 0);

		struct block_data lava_bd = {.type = BLOCK_LAVA_STILL, .metadata = 0};
		struct block_info lava_info
			= {.block = &lava_bd, .neighbours = NULL, .x = lx, .y = Y, .z = lz};

		int fires_seen = 0;
		for(int phase = 0; phase < 2; phase++) {
			if(phase == 1) // phase 1: planks above the lava -> the center landing ignites
				set(lx, Y + 2, lz, BLOCK_PLANKS, 0);

			for(int trial = 0; trial < 200; trial++) {
				// Mirror the walk on a COPY of the generator, against the same world.
				struct random_gen mirror = s.rand_src;
				int wx = lx, wy = Y, wz = lz, fx = 0, fy = 0, fz = 0;
				bool expect_fire = false;
				int steps = rand_gen_int(&mirror, 3);
				for(int i = 0; i < steps; i++) {
					wx += rand_gen_int(&mirror, 3) - 1;
					wy += 1;
					wz += rand_gen_int(&mirror, 3) - 1;
					int id = gid(wx, wy, wz);
					if(id == BLOCK_AIR) {
						if(gid(wx - 1, wy, wz) == BLOCK_PLANKS
						   || gid(wx + 1, wy, wz) == BLOCK_PLANKS
						   || gid(wx, wy, wz - 1) == BLOCK_PLANKS
						   || gid(wx, wy, wz + 1) == BLOCK_PLANKS
						   || gid(wx, wy - 1, wz) == BLOCK_PLANKS
						   || gid(wx, wy + 1, wz) == BLOCK_PLANKS) {
							expect_fire = true;
							fx = wx, fy = wy, fz = wz;
							break;
						}
					} else if(id != BLOCK_FIRE) {
						break; // stone/planks abort (solid); fire cells can't occur here
					}
				}

				blocks[BLOCK_LAVA_STILL]->onRandomTick(&s, &lava_info);

				// Exact same draws consumed, in the same order.
				assert(memcmp(&mirror, &s.rand_src, sizeof(mirror)) == 0);
				if(expect_fire) {
					assert(gid(fx, fy, fz) == BLOCK_FIRE);
					set(fx, fy, fz, BLOCK_AIR, 0); // reset for the next trial
					fires_seen++;
				} else if(phase == 0) {
					// bare stone surroundings: NO fire may ever appear
					assert(count_fire(lx - 2, Y, lz - 2, lx + 2, Y + 4, lz + 2) == 0);
				}
			}
		}
		assert(fires_seen > 0); // the planks phase must have ignited at least once
		printf("  lava: still-lava ignition walk matches the mirrored Beta draw order "
			   "(%d fires in 200 planks trials; none on bare stone) OK\n",
			   fires_seen);
	}

	g_drain_run = 0;
	thread_join(&drain);
	struct entity* tnt = find_primed_tnt();
	if(tnt)
		free(tnt);
	dict_entity_clear(s.entities);
	server_world_destroy(&s.world);
	string_clear(level);
	printf("test_fire: OK (placement/death, flammable catch, stone never, TNT prime, air spread, "
		   "eternal netherrack, still-lava ignition)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
