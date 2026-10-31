/*
	Golden test: Beta 1.7.3 lightning (World.updateBlocksAndPlayCaveSounds's
	lightning branch, EntityLightningBolt, Entity/EntityCreeper/EntityPig
	.onStruckByLightning) [740c583].

	Decompile citations:
		gate:  rand.nextInt(100000)==0 && isRaining() && isThundering()
		strike position: LCG -> findTopSolidBlock -> canBlockBeRainedOn filter
		EntityLightningBolt ctor: places fire at the strike cell + up to 4
		  random neighbours (canPlaceBlockAt gated).
		EntityLightningBolt.onUpdate (lightningState>=0): every entity within
		  AABB(pos-3, pos-3, pos-3 .. pos+3, pos+6+3, pos+3) gets
		  onStruckByLightning.
		Entity.onStruckByLightning (default, player/zombie/creeper/etc.):
		  dealFireDamage(5) [-> attackEntityFrom(null,5), skipped if
		  isImmuneToFire]; ++fire; if(fire==0) fire=300.
		EntityCreeper.onStruckByLightning: super call (damage+fire) THEN sets
		  "powered" permanently -- doubles createExplosion's power.
		EntityPig.onStruckByLightning: NO super call (no damage/fire to the
		  pig) -- spawns an EntityPigZombie at the pig's position/orientation
		  and calls setEntityDead() on the pig.
	Damage scale: Beta's raw 5 (half-hearts) * (COMBAT_HEALTH_PER_HEART/2==8)
	== 40 engine units, identical formula/scale to ZOMBIE_ATTACK_DAMAGE
	(hand-asserted below, independent of combat.h's own constant name).

	F2 (adversarial review): a bolt's FULL multi-tick lifecycle is now
	simulated (world_lightning_bolt_spawn = the constructor only;
	server_weather_tick_lightning_bolts = onUpdate, called once per active
	bolt per server tick). test_bolt_lifecycle_pinned below drives a MIRROR
	of the exact lightningState/boltLivingTime state machine on a COPY of the
	seeded RNG, in lockstep with the real production tick, and asserts
	bit-exact field equality plus identical RNG state (memcmp) after every
	tick -- not just "eventually restrikes", but the EXACT restrike count and
	timing Beta's own draws produce for this seed.
*/

#include <assert.h>
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
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_weather.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_lightning_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 70 /* platform top */

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
	struct block_data expected
		= {.type = (uint8_t)type, .metadata = (uint8_t)(meta & 0x0F)};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type && live.metadata == expected.metadata);
}

static struct block_data get(int x, int y, int z) {
	struct block_data b = {0};
	assert(server_world_get_block(&s.world, x, y, z, &b));
	return b;
}

static void reset_player(int px, int py, int pz) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.active_inventory = &s.player.inventory;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.damage_remainder = 0;
	s.player.fire = 0;
	s.player.x = px;
	s.player.y = py;
	s.player.z = pz;
	for(size_t slot = INVENTORY_SLOT_ARMOR;
		slot < INVENTORY_SLOT_ARMOR + INVENTORY_SIZE_ARMOR; slot++)
		inventory_clear_slot(&s.player.inventory, slot);
}

static void despawn_all_monsters(void) {
	bool again = true;
	while(again) {
		again = false;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			if(e && e->type == ENTITY_MONSTER) {
				uint32_t id = e->id;
				free(e);
				dict_entity_erase(s.entities, id);
				again = true;
				break;
			}
			dict_entity_next(it);
		}
	}
}

/* Test isolation: a bolt that was only ticked once (strike_one_tick) is
 * still ACTIVE (lightningState 1, >=0) -- left in the list, it would keep
 * restriking and running entity-AABB passes during a LATER subtest's own
 * ticks (server_weather_effects_tick calls
 * server_weather_tick_lightning_bolts every time), corrupting that
 * subtest's own fire/damage observations. Clear between subtests. */
static void clear_active_bolts(void) {
	s.weather.active_bolts_count = 0;
}

static struct entity* find_monster(int monster_id) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MONSTER && e->data.monster.id == monster_id)
			return e;
		dict_entity_next(it);
	}
	return NULL;
}

static int count_monsters(int monster_id) {
	int n = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MONSTER && e->data.monster.id == monster_id)
			n++;
		dict_entity_next(it);
	}
	return n;
}

static bool any_fire_within(int cx, int cy, int cz, int r) {
	for(int x = cx - r; x <= cx + r; x++)
		for(int y = cy - r; y <= cy + r; y++)
			for(int z = cz - r; z <= cz + r; z++)
				if(get(x, y, z).type == BLOCK_FIRE)
					return true;
	return false;
}

/* --- (1) damage scale: independent hand computation, not the implementation's
 * own constant -- Beta raw 5 half-hearts * 8 (COMBAT_HEALTH_PER_HEART/2) ---- */
static void test_damage_scale_hand_computed(void) {
	assert(LIGHTNING_STRIKE_DAMAGE == 5 * 8 && "5 Beta half-hearts * 8 == 40");
	assert(LIGHTNING_STRIKE_DAMAGE == ZOMBIE_ATTACK_DAMAGE
		   && "same raw value (5) and the same player-scale conversion as "
			  "the zombie melee hit");
	puts("  LIGHTNING_STRIKE_DAMAGE == 40 (5 half-hearts * 8), independently "
		 "hand-computed OK");
}

/* Spawn a bolt (the constructor) then run its onUpdate exactly ONE tick --
 * lightningState goes 2->1 (>=0), so this ONE call reproduces the same
 * "immediate" struck-entity pass + sound the old single-shot design gave,
 * for tests that only care about one activation, not the full lifecycle. */
static void strike_one_tick(w_coord_t x, w_coord_t y, w_coord_t z) {
	world_lightning_bolt_spawn(&s, x, y, z);
	server_weather_tick_lightning_bolts(&s);
}

/* --- (2) world_lightning_bolt_spawn + one onUpdate tick: fire placement +
 * every struck-entity effect on the bolt's first active tick. ------------- */
static void test_strike_effects(void) {
	/* Open platform: solid stone floor, air above, nothing overhead. */
	for(int dx = -6; dx <= 6; dx++)
		for(int dz = -6; dz <= 6; dz++) {
			set(dx, Y - 1, dz, BLOCK_STONE, 0);
			for(int dy = 0; dy < 12; dy++)
				set(dx, Y + dy, dz, BLOCK_AIR, 0);
		}

	reset_player(0, Y, 0); /* well within the struck AABB */

	struct entity* zombie
		= server_local_spawn_monster_at((vec3) {2.0f, (float)Y, 0.0f},
										MONSTER_ZOMBIE, &s);
	struct entity* creeper
		= server_local_spawn_monster_at((vec3) {-2.0f, (float)Y, 0.0f},
										MONSTER_CREEPER, &s);
	struct entity* pig = server_local_spawn_monster_at(
		(vec3) {0.0f, (float)Y, 2.0f}, MONSTER_PIG, &s);
	assert(zombie && creeper && pig);
	short zombie_health_before = zombie->health;
	short creeper_health_before = creeper->health;
	assert(!creeper->data.monster.charged);
	uint32_t pig_id = pig->id;

	strike_one_tick(0, Y, 0);

	/* Player: full LIGHTNING_STRIKE_DAMAGE (no armor), fire armed. */
	assert(s.player.health == MAX_PLAYER_HEALTH - LIGHTNING_STRIKE_DAMAGE
		   && "player takes the un-mitigated (no armor) lightning strike");
	assert(s.player.fire > 0 && "Entity.onStruckByLightning: ++fire");

	/* Zombie: same damage/fire path as the player (generic Entity path),
	 * mob-scale (no x8 conversion crosses into a mob's OWN health). */
	assert(zombie->health == zombie_health_before - LIGHTNING_STRIKE_DAMAGE);
	assert(zombie->data.monster.fire_time > 0);

	/* Creeper: damage/fire (super call) THEN charged permanently. */
	assert(creeper->health == creeper_health_before - LIGHTNING_STRIKE_DAMAGE);
	assert(creeper->data.monster.fire_time > 0);
	assert(creeper->data.monster.charged
		   && "EntityCreeper.onStruckByLightning must set \"powered\"");

	/* Pig: NO super call -- converted to a pigman, original pig entity gone,
	 * no damage/fire ever applied to the (destroyed) pig. */
	assert(find_monster(MONSTER_PIG) == NULL
		   && "the struck pig must no longer exist as a pig");
	struct entity* pigman = find_monster(MONSTER_PIG_ZOMBIE);
	assert(pigman && "EntityPig.onStruckByLightning must spawn a zombie pigman");
	assert(pigman->id != pig_id);
	float dx = pigman->pos[0] - 0.0f, dz = pigman->pos[2] - 2.0f;
	assert(dx * dx + dz * dz < 4.0f
		   && "the pigman must appear at (approximately) the pig's position");
	assert(count_monsters(MONSTER_PIG) == 0 && count_monsters(MONSTER_PIG_ZOMBIE) == 1
		   && "exactly one pig->pigman conversion, no leftover/duplicate entities");

	/* Fire placed at or near the strike cell (floor is solid stone -> the
	 * strike cell itself, an open-air cube on a normal cube, satisfies
	 * Block.fire.canPlaceBlockAt). */
	assert(any_fire_within(0, Y, 0, 1)
		   && "EntityLightningBolt ctor must place fire at/near the strike cell");

	puts("  bolt spawn + first onUpdate tick: player+zombie damage/fire, "
		 "creeper charged, pig->pigman conversion (no damage to the pig), "
		 "fire placement OK");

	despawn_all_monsters();
	clear_active_bolts(); /* this bolt is still lightningState=1 (active) */
}

/* --- (3) fire-immune mob takes NO damage but still ticks the fire counter -- */
static void test_fire_immune_mob_no_damage(void) {
	for(int dx = -3; dx <= 3; dx++)
		for(int dz = -3; dz <= 3; dz++) {
			set(dx, Y - 1, dz, BLOCK_STONE, 0);
			for(int dy = 0; dy < 8; dy++)
				set(dx, Y + dy, dz, BLOCK_AIR, 0);
		}
	reset_player(20, Y, 20); /* well outside this strike's AABB */

	struct entity* pigman = server_local_spawn_monster_at(
		(vec3) {0.0f, (float)Y, 0.0f}, MONSTER_PIG_ZOMBIE, &s);
	assert(pigman);
	assert(monster_def_get(MONSTER_PIG_ZOMBIE)->fire_immune
		   && "fixture precondition: pig zombie is fire-immune");
	short before = pigman->health;

	strike_one_tick(0, Y, 0);

	assert(pigman->health == before
		   && "Entity.dealFireDamage is a no-op for isImmuneToFire mobs");
	assert(pigman->data.monster.fire_time > 0
		   && "the fire counter itself still ticks regardless of immunity "
			  "(Entity.onStruckByLightning's ++fire is unconditional)");

	puts("  fire-immune mob (pig zombie): zero damage, fire counter still "
		 "armed OK");
	despawn_all_monsters();
	clear_active_bolts();
}

/* --- (3b) F2 (adversarial review): the FULL multi-tick bolt lifecycle,
 * pinned to a seeded RNG and cross-checked against an INDEPENDENT mirror of
 * Beta's exact lightningState/boltLivingTime state machine
 * [740c583: EntityLightningBolt.onUpdate] driven on a COPY of the same RNG.
 * Every tick: the mirror's predicted lightning_state/bolt_living_time must
 * match the real bolt's fields EXACTLY, and the RNG state (memcmp) must be
 * IDENTICAL -- not just "eventually restrikes", the exact restrike count and
 * timing this seed produces. Also proves a restrike places fire at the
 * bolt's fixed position (the ONE-attempt restrike placement, distinct from
 * the constructor's 4-neighbour scatter). ------------------------------- */
static void test_bolt_lifecycle_pinned(void) {
	const int bx = 4, by = Y, bz = -4; /* fresh spot, distinct from tests 2/3 */
	for(int dx = -3; dx <= 3; dx++)
		for(int dz = -3; dz <= 3; dz++) {
			set(bx + dx, Y - 1, bz + dz, BLOCK_STONE, 0);
			for(int dy = 0; dy < 8; dy++)
				set(bx + dx, Y + dy, bz + dz, BLOCK_AIR, 0);
		}
	reset_player(1000, Y, 1000); /* well outside every AABB this bolt can reach */

	struct entity* target = server_local_spawn_monster_at(
		(vec3) {(float)bx, (float)by, (float)bz}, MONSTER_ZOMBIE, &s);
	assert(target && "zombie: not fire-immune, not a wolf -- a plain damage target");
	short health_before = target->health;

	s.rand_src.seed = 0xABCD1234u;

	/* CFR EntityLightningBolt.java:35-47: nextLong, lifetime, 4 XYZ.
	 * Lifetime ID is reserved from the same counter as ordinary entities. */
	struct random_gen world_before = s.rand_src;
	struct java_random mirror = {0};
	entity_lifetime_rng_seed(&mirror, s.worlds[0].seed, s.worlds[0].dimension,
		s.next_entity_id + 1);
	(void)jrand_next_long(&mirror);
	int32_t predicted_blt = jrand_next_int_bound(&mirror, 3) + 1;
	for(int i = 0; i < 12; i++)
		(void)jrand_next_int_bound(&mirror, 3);

	world_lightning_bolt_spawn(&s, bx, by, bz);

	assert(memcmp(&world_before, &s.rand_src, sizeof world_before) == 0
		   && "bolt constructor preserves the world stream byte-for-byte");
	assert(s.weather.active_bolts_count == 1);
	assert(s.weather.active_bolts[0].lightning_state == 2);
	assert(s.weather.active_bolts[0].bolt_living_time == predicted_blt
		   && "boltLivingTime = rand.nextInt(3)+1, pinned to this seed");

	assert(s.weather.active_bolts[0].gameplay_rng.seed == mirror.seed);
	int32_t mirror_state = 2, mirror_blt = predicted_blt;
	int expected_restrikes = 0, expected_active_ticks = 0, total_ticks = 0;
	bool mirror_dead = false;

	while(!mirror_dead) {
		bool will_restrike = false;

		/* CFR :56-57,63,69: two pitch floats, wait roll, restrike long.
		 * S0 keeps actual sound pitch on its cosmetic stream; the two
		 * discarded floats still preserve the bolt's Beta draw order. */
		if(mirror_state == 2) {
			(void)jrand_next_float(&mirror);
			(void)jrand_next_float(&mirror);
		}
		mirror_state--;
		if(mirror_state < 0) {
			if(mirror_blt == 0) {
				mirror_dead = true;
			} else {
				int roll = jrand_next_int_bound(&mirror, 10);
				if(mirror_state < -roll) {
					will_restrike = true;
					mirror_blt--;
					mirror_state = 1;
					(void)jrand_next_long(&mirror);
					expected_restrikes++;
				}
			}
		}
		if(!mirror_dead && mirror_state >= 0)
			expected_active_ticks++;

		if(will_restrike)
			/* Clear the strike cell so the restrike's fire placement is
			 * OBSERVABLE (it's a no-op on an already-fire cell -- the
			 * constructor's own attempt may have already lit it). */
			set(bx, by, bz, BLOCK_AIR, 0);

		total_ticks++;
		assert(total_ticks < 200
			   && "runaway bolt lifecycle -- mirror/production logic diverged");

		server_weather_tick_lightning_bolts(&s);

		if(will_restrike)
			assert(get(bx, by, bz).type == BLOCK_FIRE
				   && "a restrike must place fire at the bolt's fixed "
					  "position (the ONE-attempt restrike placement)");

		if(!mirror_dead) {
			assert(s.weather.active_bolts[0].gameplay_rng.seed == mirror.seed);
			assert(s.weather.active_bolts_count == 1
				   && "the bolt must stay in the active list until it dies");
			assert(s.weather.active_bolts[0].lightning_state == mirror_state
				   && "lightningState must match the mirror EXACTLY, every tick");
			assert(s.weather.active_bolts[0].bolt_living_time == mirror_blt
				   && "boltLivingTime must match the mirror EXACTLY, every tick");
		} else {
			assert(s.weather.active_bolts_count == 0
				   && "a dead bolt must be removed from the active list");
		}
		assert(memcmp(&world_before, &s.rand_src, sizeof world_before) == 0
			   && "onUpdate RNG draw order/count must match exactly, every tick");
	}

	assert(expected_restrikes == predicted_blt
		   && "total restrikes over the bolt's life must equal the initial "
			  "boltLivingTime roll (each restrike consumes exactly one)");
	assert(expected_active_ticks == 2 * (1 + expected_restrikes)
		   && "AABB-pass tick count = 2 initial + 2 per restrike (Beta's "
			  "lightningState visits {1,0} >=0 exactly twice per activation)");

	short total_damage = (short)(health_before - target->health);
	assert(total_damage > 0
		   && "the target must take damage over the bolt's full lifetime");

	printf("  bolt lifecycle (seed pinned): boltLivingTime=%d, %d restrike(s), "
		   "%d active (AABB-pass) ticks over %d total ticks, target lost %d "
		   "health -- state+RNG bit-exact vs. the independent mirror every "
		   "tick OK\n",
		   predicted_blt, expected_restrikes, expected_active_ticks,
		   total_ticks, (int)total_damage);

	despawn_all_monsters();
}

/* --- (4) the per-tick gate: raining AND thundering both required ---------- */
static void test_gate_requires_raining_and_thundering(void) {
	for(int dx = 0; dx < 16; dx++)
		for(int dz = 0; dz < 16; dz++) {
			set(dx, Y - 1, dz, BLOCK_STONE, 0);
			set(dx, Y, dz, BLOCK_AIR, 0);
		}
	reset_player(8, Y, 8);

	const struct { bool raining, thundering; const char* label; } cases[] = {
		{false, false, "neither"},
		{true, false, "raining only"},
		{false, true, "thundering only (impossible in practice -- thunder is "
					  "weighted by rain strength, but gate it explicitly anyway)"},
	};
	for(size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		for(int x = 0; x < 16; x++)
			for(int z = 0; z < 16; z++)
				set(x, Y, z, BLOCK_AIR, 0);
		s.weather.raining = cases[c].raining;
		s.weather.rain_strength = cases[c].raining ? 1.0f : 0.0f;
		s.weather.prev_rain_strength = s.weather.rain_strength;
		s.weather.thundering = cases[c].thundering;
		s.weather.thunder_strength = cases[c].thundering ? 1.0f : 0.0f;
		s.weather.prev_thunder_strength = s.weather.thunder_strength;

		for(int i = 0; i < 3000; i++)
			server_weather_effects_tick(&s, 0, 0);

		bool any_fire = false;
		for(int x = 0; x < 16 && !any_fire; x++)
			for(int z = 0; z < 16 && !any_fire; z++)
				if(get(x, Y, z).type == BLOCK_FIRE)
					any_fire = true;
		assert(!any_fire && "no lightning strike without BOTH raining and "
							"thundering (gate)");
		(void)cases[c].label;
	}
	puts("  lightning gate: raining alone / thundering alone / neither -> "
		 "never strikes (3000 trials each) OK");
}

/* --- (5) with both true, in an OPEN sky area, a strike eventually happens -- */
static void test_strike_happens_when_gated_open(void) {
	for(int dx = 0; dx < 16; dx++)
		for(int dz = 0; dz < 16; dz++) {
			set(dx, Y - 1, dz, BLOCK_STONE, 0);
			set(dx, Y, dz, BLOCK_AIR, 0);
		}
	reset_player(8, Y, 8);
	s.weather.raining = true;
	s.weather.rain_strength = 1.0f;
	s.weather.prev_rain_strength = 1.0f;
	s.weather.thundering = true;
	s.weather.thunder_strength = 1.0f;
	s.weather.prev_thunder_strength = 1.0f;
	assert(server_weather_is_raining(&s) && server_weather_is_thundering(&s));

	/* Isolate this stochastic gate from preceding bolt-lifetime draws. */
	s.rand_src = (struct random_gen){.seed = 0x51DEB001u};
	s.weather.pos_lcg = 0;
	s.weather.sound_counter = 1;
	bool struck = false;
	int i;
	const int BUDGET = 400000; /* gate is rand.nextInt(100000)==0 per chunk/tick */
	for(i = 0; i < BUDGET && !struck; i++) {
		server_weather_effects_tick(&s, 0, 0);
		for(int x = 0; x < 16 && !struck; x++)
			for(int z = 0; z < 16 && !struck; z++)
				if(get(x, Y, z).type == BLOCK_FIRE)
					struck = true;
	}
	assert(struck && "a strike must eventually happen when raining+thundering "
					 "over open sky, within the trial budget");
	printf("  lightning strike observed (fire placed) after %d effects-tick "
		   "trial(s) OK\n", i);

	/* Test isolation (F2): the bolt that just struck is very likely still
	 * ACTIVE (mid multi-tick lifecycle, possibly mid-restrike-wait) -- left
	 * alone it would keep restriking and placing fire in this SAME 16x16
	 * area during a LATER subtest's own ticks. */
	clear_active_bolts();
}

/* --- (6) canBlockBeRainedOn filter: fully roofed area never strikes ------- */
static void test_no_strike_under_roof(void) {
	for(int dx = 0; dx < 16; dx++)
		for(int dz = 0; dz < 16; dz++) {
			set(dx, Y - 1, dz, BLOCK_STONE, 0);
			set(dx, Y, dz, BLOCK_AIR, 0);
			set(dx, Y + 1, dz, BLOCK_STONE, 0); /* roof: blocks canBlockSeeTheSky */
		}
	reset_player(8, Y, 8);
	s.weather.raining = true;
	s.weather.rain_strength = 1.0f;
	s.weather.prev_rain_strength = 1.0f;
	s.weather.thundering = true;
	s.weather.thunder_strength = 1.0f;
	s.weather.prev_thunder_strength = 1.0f;

	for(int i = 0; i < 200000; i++)
		server_weather_effects_tick(&s, 0, 0);

	bool any_fire = false;
	for(int x = 0; x < 16 && !any_fire; x++)
		for(int z = 0; z < 16 && !any_fire; z++)
			if(get(x, Y, z).type == BLOCK_FIRE)
				any_fire = true;
	assert(!any_fire
		   && "canBlockBeRainedOn's canBlockSeeTheSky gate must block every "
			  "strike under a solid roof, even at raining+thundering==1.0 "
			  "over 200000 trials");
	puts("  canBlockBeRainedOn filter: a fully roofed area is never struck "
		 "(200000 trials) OK");

	/* Clear the roof for any later test that reuses this world. */
	for(int dx = 0; dx < 16; dx++)
		for(int dz = 0; dz < 16; dz++)
			set(dx, Y + 1, dz, BLOCK_AIR, 0);
}

int main(void) {
	test_fixture_dir("test_lightning_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	puts("test_lightning_env: Beta 1.7.3 lightning (EntityLightningBolt + "
		 "EntityCreeper/EntityPig.onStruckByLightning)");
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.rand_src.seed = 0x51DEB001u;
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, NULL,
							&s.player,
		INVENTORY_SIZE, 0, 0, 0));

	struct server_chunk* sc = NULL;
	/* test_strike_effects/test_fire_immune_mob_no_damage use x,z in [-6,6]
	 * (chunks -1,0 on both axes). */
	for(int cx = -1; cx <= 0; cx++)
		for(int cz = -1; cz <= 0; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	test_damage_scale_hand_computed();
	test_strike_effects();
	test_fire_immune_mob_no_damage();
	test_bolt_lifecycle_pinned();
	test_gate_requires_raining_and_thundering();
	test_strike_happens_when_gated_open();
	test_no_strike_under_roof();

	inventory_destroy(&s.player.inventory, &s.player);
	g_drain_run = 0;
	thread_join(&drain);
	puts("test_lightning_env: PASS");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
