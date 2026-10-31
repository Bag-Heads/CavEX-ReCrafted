/*
	FAITHFULNESS env test (Beta 1.7.3, decompile 740c583): ItemFishingRod +
	EntityFish (fishing-slice). Every expected value below is hand-derived
	from the decompile cited in entity_fish.h/entity_fish.c/server_local.c's
	fishing dispatch, NEVER by running this engine's own implementation and
	reading back its answer (CODESTYLE.md S4.2, the poisoned-golden rule).

	Covers:
	  (1) fish_catch_cost's durability-cost table -- all 8 hooked/catchable/
	      in_ground combinations, proving the inGround OVERRIDE wins even
	      when hooked_entity or catchable would ALSO apply.
	  (2) fish_bite_chance (500 base / 300 rain) + fish_bite_duration's
	      [10,39] range (nextInt(30)+10, exhaustive over the valid roll
	      domain 0..29).
	  (3) fish_pull_toward_angler's toward-angler physics, hand-derived
	      (dx=3,dy=4,dz=0 -> dist=5 -> a clean sqrt).
	  (4) fish_splash_pitch's two-draw order sensitivity (swap the two
	      inputs -> a DIFFERENT, non-commutative result).
	  (5) The rain-gate's Y+1 query cell, proven geometrically: a column
	      whose heightmap sits exactly between Y and Y+1 gives DIFFERENT
	      canBlockBeRainedOn answers at Y vs Y+1 -- the exact trap a
	      same-cell mistranscription would fall into silently.
	  (6) The REAL dispatch (server_local_fishing_rod_action, driven via
	      server_local_test_process_rpc's SRPC_USE_ITEM / SRPC_BLOCK_PLACE,
	      never a raw struct poke): cast costs 0 durability and spawns a
	      tracked bobber; reeling nothing costs 0 durability; reeling a
	      hooked mob costs 3 durability AND applies the exact hand-derived
	      pull velocity to that mob; reeling a CAUGHT FISH costs 1 durability
	      AND spawns a real, tracked ITEM_FISH entity at the bobber's
	      position with the hand-derived pull velocity (finding 7); inGround
	      overrides a hooked reel to cost 2 (not 3); the SRPC_BLOCK_PLACE
	      fall-through casts too, like the egg/snowball precedent.
	  (7) entity_server_tick's angler-range liveness gate, driven through a
	      REAL tick (server_local_test_tick_entities, not injected state):
	      a hook just inside Beta's raw-posY 1024 distSq boundary survives,
	      one clearly outside despawns (finding 5/11).

	Post-review (2026-08-13, two adversarial reviewers): findings 1-11 fixed
	here and in entity_fish.{c,h}, server_local.{c,h}, block/aabb.c; finding
	12 documented as an explicit tracked deferral. See
	FISHING_SLICE_NOTES.md's post-review section for the full list, citation,
	and fix per finding.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "support/mob_motion_fixture.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_fish.h"
#include "entity/monsters_object.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_weather.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "worldgen/worldgen.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_fishing_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 100 // stone floor plane

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
	assert(server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)}));
}
static int get_type_at(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.type : -1;
}

static struct entity* fish_entity(void) {
	if(s.player.fish_entity_id == 0)
		return NULL;
	struct entity** fp = dict_entity_get(s.entities, s.player.fish_entity_id);
	return (fp && *fp) ? *fp : NULL;
}

/* Finding 7 (adversarial review): scan the REAL entity dict for a spawned
 * dropped-item entity, the same "drive the real player path, then inspect
 * the real dict" convention count_items/find_type establish in
 * test_animal_interact_env.c. */
static struct entity* find_item_entity(uint16_t item_id) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->data.item.item.id == item_id)
			return e;
		dict_entity_next(it);
	}
	return NULL;
}

static size_t count_item_entities(uint16_t item_id) {
	size_t count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->data.item.item.id == item_id)
			count++;
		dict_entity_next(it);
	}
	return count;
}

static uint16_t rod_durability(void) {
	struct item_data held;
	assert(inventory_get_hotbar_item(&s.player.inventory, &held)
		   && held.id == ITEM_FISHING_ROD);
	return held.durability;
}

static void give_fresh_rod(void) {
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
					   (struct item_data) {
						   .id = ITEM_FISHING_ROD, .durability = 0, .count = 1});
}

/* ================= (1) durability-cost table (pure) ===================== */
static void test_catch_cost_table(void) {
	/* cost 3 when an entity is hooked;
	 * else 1 when ticksCatchable > 0;
	 * then 2 when inGround, which           // OVERRIDES, unconditionally
	 * [740c583: EntityFish.catchFish] */
	assert(fish_catch_cost(false, false, false) == 0); // nothing
	assert(fish_catch_cost(false, true, false) == 1);  // landed a fish
	assert(fish_catch_cost(true, false, false) == 3);  // hooked an entity
	assert(fish_catch_cost(true, true, false) == 3);   // hooked wins over catchable
	assert(fish_catch_cost(false, false, true) == 2);  // inGround overrides "nothing"
	assert(fish_catch_cost(false, true, true) == 2);   // inGround overrides "catchable"
	assert(fish_catch_cost(true, false, true) == 2);   // inGround overrides "hooked"
	assert(fish_catch_cost(true, true, true) == 2);    // inGround overrides both
	puts("  fish_catch_cost: all 8 hooked/catchable/in_ground combinations "
		 "distinct, inGround override verified in every combination OK");
}

/* ============= (2) bite chance + duration range (pure) =================== */
static void test_bite_chance_and_duration(void) {
	assert(fish_bite_chance(false) == 500);
	assert(fish_bite_chance(true) == 300);

	/* ticksCatchable = rand.nextInt(30) + 10; nextInt(30) in [0,29]. */
	for(int roll = 0; roll < 30; roll++) {
		int d = fish_bite_duration(roll);
		assert(d >= 10 && d <= 39);
	}
	assert(fish_bite_duration(0) == 10);
	assert(fish_bite_duration(29) == 39);
	puts("  fish_bite_chance: 500 base / 300 rain; fish_bite_duration: "
		 "nextInt(30)+10 stays in [10,39] over the full roll domain, "
		 "endpoints 10 and 39 exact OK");
}

/* ================ (3) pull-toward-angler physics (pure) ================== */
static void test_pull_physics(void) {
	/* dx=3, dy=4, dz=0 -> dist = sqrt(9+16+0) = 5 (clean).
	 * out.x = 3*0.1 = 0.3
	 * out.y = 4*0.1 + sqrt(5)*0.08 = 0.4 + 0.08*sqrt(5)
	 * out.z = 0*0.1 = 0.0
	 * [740c583: EntityFish.catchFish, both non-empty branches] */
	vec3 pull;
	fish_pull_toward_angler(3.0, 4.0, 0.0, pull);
	double expected_y = 0.4 + 0.08 * sqrt(5.0);
	assert(fabsf(pull[0] - 0.3f) < 1e-5f);
	assert(fabsf(pull[1] - (float)expected_y) < 1e-5f);
	assert(fabsf(pull[2] - 0.0f) < 1e-5f);

	/* A second, asymmetric case proves X/Y/Z are not accidentally swapped:
	 * dx=0, dy=0, dz=6 -> dist=6 -> out=(0, sqrt(6)*0.08, 0.6). */
	fish_pull_toward_angler(0.0, 0.0, 6.0, pull);
	double expected_y2 = 0.08 * sqrt(6.0);
	assert(fabsf(pull[0] - 0.0f) < 1e-5f);
	assert(fabsf(pull[1] - (float)expected_y2) < 1e-5f);
	assert(fabsf(pull[2] - 0.6f) < 1e-5f);

	puts("  fish_pull_toward_angler: motion += delta*0.1, Y additionally "
		 "+= sqrt(distance)*0.08 (distance, not distance^2) -- hand-derived "
		 "against two independent axis combinations OK");
}

/* ============== (4) splash-pitch two-draw order (pure) =================== */
static void test_splash_pitch_order(void) {
	/* 1.0F + (nextFloat() - nextFloat()) * 0.4F -- first draw ADDS, second
	 * SUBTRACTS, so swapping the two inputs must NOT give the same answer
	 * (a sibling-argument evaluation-order bug would silently pass an
	 * order-insensitive test). [740c583: EntityFish.onUpdate splash] */
	float forward = fish_splash_pitch(0.5f, 0.25f);
	float reversed = fish_splash_pitch(0.25f, 0.5f);
	assert(fabsf(forward - 1.1f) < 1e-5f);  // 1.0 + (0.5-0.25)*0.4
	assert(fabsf(reversed - 0.9f) < 1e-5f); // 1.0 + (0.25-0.5)*0.4
	assert(fabsf(forward - reversed) > 0.1f && "draw order must be observable");
	puts("  fish_splash_pitch: 1.0F+(first-second)*0.4F, swapping the two "
		 "draws changes the result (1.1 vs 0.9) -- order is observable OK");
}

/* ============ (5) rain-gate Y+1 query cell (geometric, deterministic) ==== */
static void test_rain_gate_y_plus_one(void) {
	/* Independently confirm the biome at this column allows rain (the
	 * test_weather_snow_ice_env.c precedent) rather than assuming it --
	 * biome depends only on X/Z, not Y. */
	enum biome b = worldgen_biome_at(s.world.generator, 40, 40);
	bool enable_snow = (b == BIOME_TAIGA || b == BIOME_ICE_DESERT
						|| b == BIOME_TUNDRA);
	bool enable_rain = (b != BIOME_DESERT && b != BIOME_ICE_DESERT);
	assert(!enable_snow && enable_rain
		   && "pick a plain rain-enabled biome column for this probe");

	s.weather.raining = true;
	s.weather.rain_strength = 1.0f;
	s.weather.prev_rain_strength = 1.0f;
	assert(server_weather_is_raining(&s));

	/* Solid ground occupies row 50 ONLY; row 51 and above are open. The
	 * column heightmap therefore sits exactly at 51 -- see_sky(50) is
	 * FALSE, see_sky(51) is TRUE. A hook whose floored Y is 50 (a fish
	 * floating just above this floor) must be queried at 50+1=51 (rain
	 * applies), never at 50 (a same-cell mistranscription would silently
	 * read "no rain" here). */
	/* Rows 51+ are already air by default (superflat ground sits far below
	 * Y=50); no need to (redundantly, and un-asserted-ly falsely) clear
	 * them -- server_world_set_block returns false for a no-op write,
	 * matching Beta's real setBlockAndMetadataWithNotify contract. */
	set(40, 50, 40, BLOCK_STONE, 0);
	assert(!world_can_block_see_sky(&s, 40, 50, 40));
	assert(world_can_block_see_sky(&s, 40, 51, 40));

	assert(world_can_block_be_rained_on(&s, 40, 51, 40)
		   && "the Y+1 cell (what entity_fish.c actually queries) must see rain");
	assert(!world_can_block_be_rained_on(&s, 40, 50, 40)
		   && "the hook's OWN cell must NOT see rain here -- proves +1 is load-bearing");

	puts("  world_can_block_be_rained_on: querying floor(hookY)+1 differs "
		 "from querying floor(hookY) at this geometry, matching "
		 "entity_fish.c's exact fx,fy+1,fz call shape OK");
}

/* ===================== (6) real dispatch, via RPC ========================= */
static void test_cast_costs_nothing_and_tracks_entity(void) {
	give_fresh_rod();
	s.player.x = 5.0;
	s.player.y = 70.0 + PLAYER_EYE_HEIGHT;
	s.player.z = 5.0;
	s.player.rx = 0.0f;
	s.player.ry = 0.0f;
	assert(s.player.fish_entity_id == 0);

	struct server_rpc use = {.type = SRPC_USE_ITEM,
							 .payload.use_item = {.charge = 1.0f}};
	server_local_test_process_rpc(&s, &use);

	assert(s.player.fish_entity_id != 0 && "cast must install a tracked bobber");
	struct entity* fish = fish_entity();
	assert(fish && fish->type == ENTITY_FISH);
	assert(!fish->data.fish.bobber_hooked);
	assert(!fish->data.fish.in_ground);
	assert(fish->data.fish.ticks_catchable == 0);
	assert(rod_durability() == 0 && "casting itself costs no durability");

	puts("  cast (SRPC_USE_ITEM, open air): spawns a tracked ENTITY_FISH "
		 "bobber at zero durability cost OK");
}

static void test_reel_nothing_costs_zero(void) {
	/* Reuse the cast bobber from the previous scenario: idle, not hooked,
	 * not stuck, no bite -- catchFish() must return 0 and cost nothing. */
	struct entity* fish = fish_entity();
	assert(fish && !fish->data.fish.bobber_hooked
		   && fish->data.fish.ticks_catchable == 0
		   && !fish->data.fish.in_ground);
	uint16_t before = rod_durability();

	struct server_rpc reel = {.type = SRPC_USE_ITEM,
							  .payload.use_item = {.charge = 1.0f}};
	server_local_test_process_rpc(&s, &reel);

	assert(rod_durability() == before && "reeling in nothing costs 0 durability");
	assert(s.player.fish_entity_id == 0 && "the hook is always removed on reel");
	assert(!fish_entity() && "the bobber entity itself must be gone");
	puts("  reel (nothing biting): 0 durability cost, hook removed and "
		 "fish_entity_id cleared OK");
}

static void test_reel_hooked_mob_costs_three_and_pulls(void) {
	give_fresh_rod();
	s.player.x = 13.0;
	/* eye = 64 DIRECTLY -- this engine's player.y IS the eye position (no
	 * +/-PLAYER_EYE_HEIGHT conversion), matching catchFish()'s own RAW
	 * `angler.posY - this.posY` [740c583] (see the corrected citation at
	 * server_local_fishing_rod_reel). A prior version of this test added
	 * PLAYER_EYE_HEIGHT here on the false "posY is the feet" premise
	 * findings 3/5/8 name -- see FISHING_SLICE_NOTES.md's post-review
	 * section. */
	s.player.y = 64.0;
	s.player.z = 10.0;
	s.player.rx = 0.0f;
	s.player.ry = 0.0f;

	struct server_rpc cast = {.type = SRPC_USE_ITEM,
							  .payload.use_item = {.charge = 1.0f}};
	server_local_test_process_rpc(&s, &cast);
	struct entity* fish = fish_entity();
	assert(fish);

	/* Inject the "already hooked a mob" state directly -- the hit-detection
	 * path (entity_server_tick's substep sweep) is separate machinery this
	 * test does not exercise; this proves the REEL dispatch's handling of
	 * that state, matching CODESTYLE's "drive the real player path" for
	 * the ACTION under test (reeling), not a reimplementation of the sweep. */
	struct entity* mob
		= server_local_spawn_monster_at((vec3) {10.0f, 60.0f, 10.0f},
										MONSTER_PIG, &s);
	assert(mob);
	mob->vel[0] = 0.0f;
	mob->vel[1] = 0.0f;
	mob->vel[2] = 0.0f;
	test_mob_sync(mob); /* motion units S2: the fixture pins the doubles */
	fish->pos[0] = 10.0f;
	fish->pos[1] = 60.0f;
	fish->pos[2] = 10.0f;
	fish->data.fish.bobber_hooked = true;
	fish->data.fish.bobber_id = mob->id;
	fish->data.fish.ticks_catchable = 0; // hooked wins over catchable regardless
	fish->data.fish.in_ground = false;

	uint16_t before = rod_durability();
	server_local_test_process_rpc(&s, &cast); // SRPC_USE_ITEM again -> reel

	assert(rod_durability() == (uint16_t)(before + 3)
		   && "a hooked-entity reel costs exactly 3 durability");
	assert(s.player.fish_entity_id == 0);

	/* dx = player.x - fish.x = 13-10 = 3; dy = player.y(eye, RAW, no
	 * PLAYER_EYE_HEIGHT term -- see the setup comment above and
	 * server_local_fishing_rod_reel's corrected citation) - fish.y =
	 * 64-60 = 4; dz = 10-10 = 0 -- the SAME (3,4,0)/dist=5 case
	 * test_pull_physics already hand-derived, independently, from
	 * fish_pull_toward_angler's own decompile citation:
	 * pull = (0.3, 0.4+sqrt(5)*0.08, 0.0). */
	double expected_y = 0.4 + 0.08 * sqrt(5.0);
	assert(fabsf(mob->vel[0] - 0.3f) < 1e-4f);
	assert(fabsf(mob->vel[1] - (float)expected_y) < 1e-4f);
	assert(fabsf(mob->vel[2] - 0.0f) < 1e-4f);

	puts("  reel (hooked mob): costs exactly 3 durability AND applies the "
		 "hand-derived (0.3, 0.4+sqrt(5)*0.08, 0.0) pull velocity to the "
		 "REAL mob entity OK");
}

/* Finding 7 (adversarial review): no test previously drove the REEL
 * dispatch through catchFish()'s ticksCatchable>0 branch -- "landed a
 * fish" -- end to end. Only the PURE fish_catch_cost(false,true,false)==1
 * sub-computation was checked (test_catch_cost_table); the real
 * EntityItem(fishRaw) spawn, its pulled velocity, and the publish path
 * were never exercised against the actual entity dict. This proves
 * durability cost 1, hook removal, AND the spawned item's identity/count/
 * position/velocity -- the single most player-visible fishing outcome. */
static void test_reel_catchable_fish_costs_one_and_spawns_item(void) {
	give_fresh_rod();
	s.player.x = 10.0;
	s.player.y = 64.0; // eye, direct -- see the mob-reel test's citation
	s.player.z = 16.0;
	s.player.rx = 0.0f;
	s.player.ry = 0.0f;

	struct server_rpc cast = {.type = SRPC_USE_ITEM,
							  .payload.use_item = {.charge = 1.0f}};
	server_local_test_process_rpc(&s, &cast);
	struct entity* fish = fish_entity();
	assert(fish);

	/* Inject the "already biting" state directly -- the bite-roll RNG path
	 * (entity_server_tick's submersion probe) is separate machinery this
	 * test does not exercise; this proves the REEL dispatch's handling of
	 * that state, the SAME "drive the real player path for the ACTION
	 * under test" convention the hooked-mob test above already
	 * establishes. Any ticksCatchable>0 takes the identical branch
	 * (fish_catch_cost only tests >0 vs ==0); 15 is an arbitrary
	 * mid-[10,39] pick, not load-bearing. */
	fish->pos[0] = 10.0f;
	fish->pos[1] = 64.0f;
	fish->pos[2] = 10.0f;
	fish->data.fish.bobber_hooked = false;
	fish->data.fish.bobber_id = 0;
	fish->data.fish.ticks_catchable = 15;
	fish->data.fish.in_ground = false;

	assert(count_item_entities(ITEM_FISH) == 0
		   && "no stray fish item before reeling");
	uint16_t before = rod_durability();
	server_local_test_process_rpc(&s, &cast); // SRPC_USE_ITEM again -> reel

	assert(rod_durability() == (uint16_t)(before + 1)
		   && "landing a fish costs exactly 1 durability");
	assert(s.player.fish_entity_id == 0);
	assert(!fish_entity() && "the bobber entity itself must be gone");

	/* dx = player.x - fish.x = 10-10 = 0; dy = player.y(eye, RAW) -
	 * fish.y = 64-64 = 0; dz = player.z - fish.z = 16-10 = 6 -- the SAME
	 * (0,0,6)/dist=6 axis-swapped case test_pull_physics already
	 * hand-derived, independently, from fish_pull_toward_angler's own
	 * decompile citation: pull = (0.0, sqrt(6)*0.08, 0.6). */
	double expected_y = 0.08 * sqrt(6.0);

	assert(count_item_entities(ITEM_FISH) == 1
		   && "exactly one fish item spawned, no duplicates");
	struct entity* fish_item = find_item_entity(ITEM_FISH);
	assert(fish_item
		   && "catchFish()'s EntityItem(fishRaw) must be a real, tracked "
			  "entity");
	assert(fish_item->data.item.item.count == 1);
	assert(fish_item->data.item.item.durability == 0);
	/* `new EntityItem(worldObj, this.posX, this.posY, this.posZ, ...)` --
	 * spawns AT THE BOBBER'S position, not the angler's [740c583:
	 * catchFish]. */
	assert(fabsf(fish_item->pos[0] - 10.0f) < 1e-5f);
	assert(fabsf(fish_item->pos[1] - 64.0f) < 1e-5f);
	assert(fabsf(fish_item->pos[2] - 10.0f) < 1e-5f);
	assert(fabsf(fish_item->vel[0] - 0.0f) < 1e-4f);
	assert(fabsf(fish_item->vel[1] - (float)expected_y) < 1e-4f);
	assert(fabsf(fish_item->vel[2] - 0.6f) < 1e-4f);

	puts("  reel (landed a fish): costs exactly 1 durability, hook removed, "
		 "AND spawns a REAL tracked ITEM_FISH entity at the bobber's "
		 "position with the hand-derived (0.0, sqrt(6)*0.08, 0.6) pull "
		 "velocity OK");
}

/* Finding 5 (adversarial review): entity_server_tick's angler-range
 * liveness gate subtracted PLAYER_EYE_HEIGHT from player.y before
 * comparing to the hook's position, on the same false "posY is the feet"
 * premise findings 3/8 name -- but Entity.getDistanceSqToEntity [740c583]
 * uses RAW posY-posY, no yOffset term at all. Finding 11 (adversarial
 * review, NOTE): entity_server_tick itself was never driven through a REAL
 * tick by any test in this slice -- every other test injects bobber state
 * directly and exercises only the REEL dispatch, not the tick that
 * actually calls this gate. This test drives the REAL
 * server_local_test_tick_entities path and picks a boundary geometry that
 * DISCRIMINATES the fix: under the old (buggy) feet-based computation this
 * exact case would have been wrongly killed. */
static void test_liveness_range_gate_uses_raw_posy(void) {
	give_fresh_rod();
	s.player.x = 0.0;
	s.player.y = 60.0; // eye, direct
	s.player.z = 0.0;
	s.player.rx = 0.0f;
	s.player.ry = 0.0f;

	struct server_rpc cast = {.type = SRPC_USE_ITEM,
							  .payload.use_item = {.charge = 1.0f}};
	server_local_test_process_rpc(&s, &cast);
	struct entity* fish = fish_entity();
	assert(fish);

	/* --- Case A: 31.5 blocks above the angler's EYE, straight up --------
	 * Correct (eye-based, this fix): dy = 60 - 91.5 = -31.5,
	 * distSq = 992.25 <= 1024 -> SURVIVES [740c583:
	 * getDistanceSqToEntity(angler) > 1024.0 kills the hook].
	 * Buggy (feet-based, PLAYER_EYE_HEIGHT=1.62 subtracted from player.y
	 * FIRST, the code this replaces): dy = (60-1.62) - 91.5 = -33.12,
	 * distSq = 1096.9... > 1024 -> would have been WRONGLY DESPAWNED.
	 * This geometry is the discriminator between the two implementations. */
	fish->pos[0] = 0.0f;
	fish->pos[1] = 91.5f;
	fish->pos[2] = 0.0f;
	fish->vel[0] = fish->vel[1] = fish->vel[2] = 0.0f;
	fish->data.fish.bobber_hooked = false;
	fish->data.fish.in_ground = false;
	uint32_t id_before = s.player.fish_entity_id;

	assert(server_local_test_tick_entities(&s));

	assert(s.player.fish_entity_id == id_before
		   && "31.5 eye-relative blocks is within Beta's raw-posY 1024 "
			  "distSq -- must survive a REAL tick");
	assert(fish_entity() != NULL && "the bobber entity itself must remain");

	/* --- Case B: the SAME hook moved clearly out of range (sanity) ------
	 * Proves the fix did not turn the gate into a permanent no-op: a hook
	 * genuinely out of range (140 blocks either way) must still despawn on
	 * the very next real tick. Beta's entity activation check uses fixed Y
	 * bounds 0..128, so the hook's own high Y must not freeze this update. */
	fish->pos[1] = 200.0f;

	assert(server_local_test_tick_entities(&s));

	assert(s.player.fish_entity_id == 0
		   && "genuinely out of range must still despawn -- the gate is "
			  "not now a no-op");
	assert(!fish_entity());

	puts("  entity_server_tick liveness/range gate (REAL tick via "
		 "server_local_test_tick_entities, not injected state): a hook "
		 "31.5 eye-relative blocks away survives (discriminates the "
		 "eye-vs-feet fix) and one 140 blocks away still despawns OK");
}

static void test_reel_in_ground_overrides_hooked(void) {
	give_fresh_rod();
	s.player.x = 20.0;
	s.player.y = 70.0 + PLAYER_EYE_HEIGHT;
	s.player.z = 20.0;
	s.player.rx = 0.0f;
	s.player.ry = 0.0f;

	struct server_rpc cast = {.type = SRPC_USE_ITEM,
							  .payload.use_item = {.charge = 1.0f}};
	server_local_test_process_rpc(&s, &cast);
	struct entity* fish = fish_entity();
	assert(fish);

	/* Both hooked AND in_ground set -- Beta's override still applies even
	 * though the "hooked" branch alone would cost 3. Beta's own control
	 * flow makes this combination unreachable in a single live tick (see
	 * entity.h's ticks_in_ground comment), but catchFish()'s override is a
	 * SEPARATE, unconditional statement with no such guard -- this proves
	 * the override wins on its own terms, not by construction. */
	fish->data.fish.bobber_hooked = true;
	fish->data.fish.bobber_id = 0; // would be a (deferred) self-hook if reached
	fish->data.fish.in_ground = true;

	uint16_t before = rod_durability();
	server_local_test_process_rpc(&s, &cast); // reel

	assert(rod_durability() == (uint16_t)(before + 2)
		   && "inGround overrides a hooked reel to cost 2, never 3");
	puts("  reel (hooked AND inGround): costs 2, not 3 -- the override "
		 "applies even when the hooked branch would also match OK");
}

static void test_block_place_fallthrough_casts(void) {
	give_fresh_rod();
	/* An isolated stone pillar with no right-click behavior of its own --
	 * the SAME "unhandled block" shape the egg/snowball fall-through
	 * comment describes. (30,30) stays inside the loaded chunk range. */
	set(30, Y, 30, BLOCK_STONE, 0);
	s.player.x = 30.0 + 0.5;
	s.player.y = (double)Y + 1.0 + PLAYER_EYE_HEIGHT;
	s.player.z = 30.0 + 0.5;
	s.player.rx = 0.0f;
	s.player.ry = 0.0f;
	assert(s.player.fish_entity_id == 0);

	struct server_rpc place = {
		.type = SRPC_BLOCK_PLACE,
		.payload.block_place
		= {.x = 30, .y = Y, .z = 30, .side = SIDE_TOP},
	};
	server_local_test_process_rpc(&s, &place);

	assert(s.player.fish_entity_id != 0
		   && "aiming at an unhandled block must still cast, like egg/snowball");
	assert(get_type_at(30, Y, 30) == BLOCK_STONE
		   && "the pillar itself must be untouched (no place, no dig)");
	puts("  SRPC_BLOCK_PLACE fall-through (unhandled block): casts, same "
		 "trailing-sendUseItem path egg/snowball already use OK");
}

/* CFR EntityFish.java:231-255: chance -> duration -> 2 pitch floats ->
 * 18 bubble + 12 splash floats -> 3 tug floats. Exact Java48 state and
 * world preservation, including the Gaussian cache, after EVERY branch. */
static void test_fish_owned_rng(void) {
	give_fresh_rod();
	s.weather.raining = false;
	s.player.x = 40.5;
	s.player.y = 100.5;
	s.player.z = 40.5;
	for(int x = 39; x <= 41; x++)
		for(int z = 39; z <= 41; z++)
			for(int y = 99; y <= 100; y++)
				set(x, y, z, BLOCK_WATER_STILL, 0);
	const int seeds[] = {0, 61, 61, 0}; /* no bite, dry bite, rain bite, ongoing */
	for(int c = 0; c < 4; c++) {
		struct entity e = {0};
		entity_fish(7000 + c, &e, true, &s.worlds[0]);
		struct java_random born = {0};
		entity_lifetime_rng_seed(&born, s.worlds[0].seed, s.worlds[0].dimension, e.id);
		assert(e.gameplay_rng.seed == born.seed);
		e.teleport(&e, (vec3){40.5F, 100.5F, 40.5F});
		jrand_set_seed(&e.gameplay_rng, seeds[c]);
		struct java_random expected = e.gameplay_rng;
		s.weather.raining = c == 2;
		s.weather.rain_strength = c == 2 ? 1.0F : 0.0F;
		s.weather.prev_rain_strength = s.weather.rain_strength;
		int duration = 0;
		if(c == 3) {
			e.data.fish.ticks_catchable = 2;
			duration = 1;
		} else {
			int chance = c == 2 ? 300 : 500;
			assert(world_can_block_be_rained_on(&s, 40, 101, 40) == (c == 2));
			int roll = jrand_next_int_bound(&expected, chance);
			assert((roll == 0) == (c != 0));
			if(roll == 0) {
				duration = jrand_next_int_bound(&expected, 30) + 10;
				for(int i = 0; i < 32; i++)
					(void)jrand_next_float(&expected);
			}
		}
		if(duration > 0)
			for(int i = 0; i < 3; i++)
				(void)jrand_next_float(&expected);
		struct random_gen world_before = s.rand_src;
		struct random_gen shared_before = s.entity_rng;
		assert(!e.tick_server(&e, &s));
		assert(e.data.fish.ticks_catchable == duration);
		assert(e.gameplay_rng.seed == expected.seed);
		assert(!e.gameplay_rng.have_next_gaussian);
		assert(memcmp(&world_before, &s.rand_src, sizeof world_before) == 0);
		assert(memcmp(&shared_before, &s.entity_rng, sizeof shared_before) == 0);
	}
	puts("  fish own Java48 stream: no bite/dry bite/rain bite/ongoing tug PASS");
}

int main(void) {
	test_fixture_dir("test_fishing_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	test_catch_cost_table();
	test_bite_chance_and_duration();
	test_pull_physics();
	test_splash_pitch_order();

	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	/* Covers the ordinary arena including Beta's +/-32 block entity-activation
	 * margin. */
	for(int cz = -2; cz <= 2; cz++)
		for(int cx = -2; cx <= 2; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.active_inventory = &s.player.inventory;
	s.player.health = MAX_PLAYER_HEALTH;
	inventory_set_hotbar(&s.player.inventory, 0);

	test_rain_gate_y_plus_one();
	test_cast_costs_nothing_and_tracks_entity();
	test_reel_nothing_costs_zero();
	test_reel_hooked_mob_costs_three_and_pulls();
	test_reel_catchable_fish_costs_one_and_spawns_item();
	test_liveness_range_gate_uses_raw_posy();
	test_reel_in_ground_overrides_hooked();
	test_block_place_fallthrough_casts();
	test_fish_owned_rng();

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.world);
	string_clear(level);

	printf("test_fishing_env: ALL OK\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
