/* split-dimensions-plan-0924, A1d review limit L1 (out/A1d-REVIEW-opus-sub.md):
 * Beta sleep spawning tries EVERY player, not only the host. RED today.
 *
 * The spec (client Jad decompile, ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   World.tick:1742-1752          all players fully asleep -> performSleepSpawning(world, its playerEntities)
 *                                 (:1745) when spawnHostileMobs && difficultySetting >= 1; ONLY when it
 *                                 returns false: skip to dawn + wakeUpAllPlayers.
 *   SpawnerAnimals.java:169-246   performSleepSpawning(World, List): an iterator over
 *                                 the WHOLE player list (:172-185); for EACH player, up to 20
 *                                 candidates (:189) around THAT player's floor(posX/posY/posZ) (:190-192),
 *                                 the path is to THAT player (:222), the placement uses THAT player's bed
 *                                 (:226), and only THAT player is woken (:235, wakeUpPlayer(true,false,
 *                                 false)); the spawned flag (:237) cancels the skip but the loop CONTINUES to
 *                                 the next player -- a spawn beside player 1 does not stop player 2's
 *                                 candidates.
 *   World.java:901                playerEntities.add on join -> iteration order is join order: the host
 *                                 (who created the world) first, the guest second.
 *
 * CaveWii today: server_local_sleep_spawn (server_local.c) draws 20 candidates around s->player only.
 * With two Overworld sleepers the guest's candidates are never drawn: the world RNG stream diverges from
 * Beta and a spawn Beta would place beside the guest (waking the guest, cancelling the skip) never happens.
 *
 * Fixture, case 1 (all_sleepers): superflat void world. The HOST's bed stands on two stone blocks at
 * (0,Y,0) with nothing else in its +-31 candidate square, so no host candidate can ever path to within
 * 1.5 of the host (the test_sleep_spawn_golden void case) -- every host attempt draws and fails. The
 * GUEST's bed is at (GX,Y,0) in a stone field x in [FIELD_X0, 63], z in [-40, 40], out of the host's
 * reach, where Beta's guest candidates DO spawn for many seeds. Seeds whose host transcription spawns
 * are skipped (none should), so the reference never has to model an entity left by the host leg.
 *
 * Case 2 (both_legs; Opus G6SEP-L1 review H1): the host ALSO sleeps in an open stone field (its bed at
 * (HX,Y,0), field x in [-63, HFIELD_X1]), so BOTH legs can spawn. Beta walks the whole list: a host
 * spawn ends only the host's 20 candidates (:240 its loop flag), the guest's leg still draws (:174-183) and can
 * spawn too; the result is the OR over both legs (:239 the spawned flag, returned at :180). The reference runs both
 * legs unconditionally and the RNG must agree after the SECOND leg; each sleeper wakes iff its own leg
 * spawned; the skip to dawn happens iff neither did.
 *
 * RED checks exit 1 through the check() ledger; setup asserts may abort. */
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
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/pathfinding.h"
#include "entity/monsters_object.h"
#include "game/difficulty.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_guest.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/spawner.h"
#include "platform/thread.h"
#include "worldgen/java_random.h"

void server_local_test_set_difficulty(int d);

#define Y 70
#define GX 50
#define FIELD_X0 36
#define HX (-50)
#define HFIELD_X1 (-36)
#define SEEDS 120u
static char fixture[128];
static struct server_local s;
static struct server_player guest;
static volatile int draining = 1;
static unsigned failures;

static void* drain_entry(void* arg) {
	(void)arg;
	while(draining) { clin_drain_discard(); thread_msleep(1); }
	clin_drain_discard();
	return NULL;
}
static void set(int x, int y, int z, int type, int meta) {
	if(server_world_set_block(&s.worlds[0], x, y, z,
		(struct block_data) {.type = type, .metadata = meta})) return;
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	assert(b.type == type);
}
static void check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if(!ok) failures++;
}
static void clear_entities(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(e) free(e);
	}
	dict_entity_reset(s.entities);
}
static int count_monsters(void) {
	int n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MONSTER) n++;
	}
	return n;
}

/* ---- the SpawnerAnimals.performSleepSpawning transcription, per player (as in test_sleep_spawn_golden,
 * generalized from the host to any sleeper position) ------------------------------------------------ */
static struct block_data ref_block(int x, int y, int z) {
	struct block_data b = {.type = BLOCK_AIR};
	if(y >= 0 && y < 128 && !server_world_get_block(&s.worlds[0], x, y, z, &b))
		b = (struct block_data) {.type = BLOCK_AIR};
	return b;
}
static bool ref_normal(int x, int y, int z) { return block_is_normal_cube(ref_block(x, y, z).type); }
static bool ref_liquid(int x, int y, int z) {
	int id = ref_block(x, y, z).type;
	return id == BLOCK_WATER_FLOW || id == BLOCK_WATER_STILL || id == BLOCK_LAVA_FLOW || id == BLOCK_LAVA_STILL;
}
static bool ref_can_spawn(int x, int y, int z) { // SpawnerAnimals.java:153-155
	return ref_normal(x, y - 1, z) && !ref_normal(x, y, z) && !ref_liquid(x, y, z) && !ref_normal(x, y + 1, z);
}
/* RNG owners (R-B02, 2026-10-09; the same accounting as test_sleep_spawn_golden's reference). World.rand
 * (s.rand_src): the offsets, the mob pick, setLocationAndAngles' yaw (:191-196, :221) and creatureSpecificInit's
 * jockey nextInt(100) (:158). The CANDIDATE's own Random (newInstance :216; Entity.java:50) draws
 * EntityMob.getCanSpawnHere's light rolls (EntityMob.java:73, :84) -- engine lifetime seed
 * entity_lifetime_rng_seed(world seed, dim, id), Java48 mob-RNG slice J/N (0db9e79ea) -- and the spawned mob
 * keeps that stream. Zero-motion join (Entity.java:20-22, slice I): no velocity draws. Ids come from ONE
 * session counter shared by both legs (Entity.nextEntityID), one per constructed candidate. */
struct ref_spawn {
	bool spawned;
	uint32_t id;
	struct java_random mob_rng;
};
static uint32_t ref_gen_id(uint32_t* next) { // server_local_gen_id on a cleared dictionary
	do { ++*next; } while(*next == 0 || *next == UINT32_MAX);
	return *next;
}
/* One player's leg of performSleepSpawning (:187-243) on the running streams. Returns whether this
 * player got a spawn (the leg stops at its first spawn, :189 `!var7`). */
static bool ref_player_leg(struct random_gen* g, uint32_t* next_id, struct ref_spawn* out, double px, double py,
						   double pz) {
	struct ref_spawn scratch;
	if(!out) out = &scratch;
	memset(out, 0, sizeof *out);
	static const int night[3] = {MONSTER_SPIDER, MONSTER_ZOMBIE, MONSTER_SKELETON}; // SpawnerAnimals.java:10
	for(int v8 = 0; v8 < 20; v8++) {
		int a = rand_gen_int(g, 32), b = rand_gen_int(g, 32);
		int v9 = (int)floor(px) + a - b;
		a = rand_gen_int(g, 32); b = rand_gen_int(g, 32);
		int v10 = (int)floor(pz) + a - b;
		a = rand_gen_int(g, 16); b = rand_gen_int(g, 16);
		int v11 = (int)floor(py) + a - b;
		if(v11 < 1) v11 = 1; else if(v11 > 128) v11 = 128;
		int v12 = rand_gen_int(g, 3);
		int v13;
		for(v13 = v11; v13 > 2 && !ref_normal(v9, v13 - 1, v10); --v13) {}
		while(!ref_can_spawn(v9, v13, v10) && v13 < v11 + 16 && v13 < 128) ++v13;
		if(!(v13 < v11 + 16 && v13 < 128)) continue;
		int mob = night[v12];
		uint32_t id = ref_gen_id(next_id); // newInstance (:216): the next entity id + the entity's own Random
		struct java_random own;
		entity_lifetime_rng_seed(&own, s.worlds[0].seed, s.worlds[0].dimension, id);
		(void)rand_gen_flt(g); // setLocationAndAngles yaw on World.rand (:221)
		struct block_data here = ref_block(v9, v13, v10);
		if((int)here.sky_light > jrand_next_int_bound(&own, 32)) continue;       // EntityMob.java:73
		if(server_local_hostile_spawn_light(&s, v9, v13, v10) > jrand_next_int_bound(&own, 8)) continue; // :84
		if(!server_local_test_mob_spawn_bbox_clear(&s, mob, v9, v13, v10)) continue;
		const struct monster_def* d = monster_def_get(mob);
		struct path_entity path;
		if(!monster_path_to_entity(&s, (float)v9 + 0.5F, (float)v13, (float)v10 + 0.5F, d->box_w, d->box_h,
			   (float)px, (float)(py - 0.2), (float)pz, 32.0F, &path) || path.length <= 1)
			continue;
		const struct path_waypoint* e = &path.points[path.length - 1];
		if(!(fabs(e->x - px) < 1.5 && fabs(e->z - pz) < 1.5 && fabs(e->y - py) < 1.5)) continue;
		out->spawned = true;
		out->id = id;
		// creatureSpecificInit (:158): the jockey's world nextInt(100); a hit builds a skeleton (next id).
		if(mob == MONSTER_SPIDER && rand_gen_int(g, 100) == 0) (void)ref_gen_id(next_id);
		// playLivingSound (:238) on the spawned mob's own Random: two pitch floats
		// (LIVING-SOUND-RNG; EntityLiving.java:96-100).
		(void)jrand_next_float(&own);
		(void)jrand_next_float(&own);
		out->mob_rng = own;
		return true;
	}
	return false;
}

/* The spawned monster with this id exists and continues the candidate's own Random. */
static bool monster_owns(const struct ref_spawn* r) {
	if(!r->spawned) return true;
	dict_entity_it_t it;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MONSTER && e->id == r->id) return e->gameplay_rng.seed == r->mob_rng.seed;
	}
	return false;
}

/* Both bodies one tick short of fully asleep in their own beds, at midnight. */
static void prepare_at(uint32_t seed, int host_bed_x) {
	clear_entities();
	s.world_time = 18000;
	s.player.has_bed_spawn = false;
	guest.has_bed_spawn = false;
	s.player.hurt_resist_time = guest.hurt_resist_time = 0;
	server_local_begin_sleep(&s, host_bed_x, Y, 0);
	server_local_begin_sleep_for(&s, &guest, GX, Y, 0);
	assert(s.player.sleeping && guest.sleeping);
	s.player.sleep_timer = 99;
	guest.sleep_timer = 99;
	guest.reject_pos_this_update = false;
	s.rand_src = (struct random_gen) {.seed = seed};
}
static void prepare(uint32_t seed) { prepare_at(seed, 0); }

/* Case 2: both legs can spawn. Beta draws the host's 20 candidates, then -- whatever the host's
 * result -- the guest's; the outcome is the OR (SpawnerAnimals.java:174-183, :239-240, :180). */
static void case_both_legs(void) {
	unsigned rng_agree = 0, outcome_agree = 0, host_spawns = 0, guest_spawns = 0, both = 0;
	for(uint32_t seed = 1; seed <= SEEDS; seed++) {
		prepare_at(seed * 7919u, HX);
		struct random_gen ref = {.seed = seed * 7919u};
		uint32_t ref_id = s.next_entity_id;
		struct ref_spawn hs, gs;
		double hx = s.player.x, hy = s.player.y, hz = s.player.z;
		double gx = guest.x, gy = guest.y, gz = guest.z;
		bool host_spawn = ref_player_leg(&ref, &ref_id, &hs, hx, hy, hz);
		bool guest_spawn = ref_player_leg(&ref, &ref_id, &gs, gx, gy, gz); /* ALWAYS drawn (:174-183) */
		host_spawns += host_spawn;
		guest_spawns += guest_spawn;
		both += host_spawn && guest_spawn;
		server_local_sleep_tick(&s);
		rng_agree += s.rand_src.seed == ref.seed && s.next_entity_id == ref_id && monster_owns(&hs)
			&& monster_owns(&gs);
		bool any = host_spawn || guest_spawn;
		bool ok = count_monsters() == (int)host_spawn + (int)guest_spawn
			&& (any
					/* a spawn somewhere: each sleeper wakes iff ITS leg spawned (:237
					 * wakes var5 only, no spawn point), the other keeps sleeping, and
					 * the night is NOT skipped */
					? s.player.sleeping == !host_spawn && guest.sleeping == !guest_spawn
						&& (!host_spawn || (s.player.sleep_timer == 0 && !s.player.has_bed_spawn))
						&& (!guest_spawn || (guest.sleep_timer == 0 && !guest.has_bed_spawn))
						&& s.world_time == 18000
					/* neither leg spawned: skip to dawn, both woken with their own bed
					 * spawn (World.tick:1748-1751, wakeUpAllPlayers) */
					: s.world_time == 24001 && !s.player.sleeping && !guest.sleeping
						&& s.player.has_bed_spawn && guest.has_bed_spawn);
		outcome_agree += ok;
		if(!ok)
			printf("  mismatch seed %u: ref host %d guest %d | monsters %d host_sleeping %d (timer %d bed %d) "
				   "guest_sleeping %d (timer %d bed %d) time %llu\n", seed, host_spawn, guest_spawn,
				   count_monsters(), s.player.sleeping, s.player.sleep_timer, s.player.has_bed_spawn,
				   guest.sleeping, guest.sleep_timer, guest.has_bed_spawn, (unsigned long long)s.world_time);
		s.player.sleeping = guest.sleeping = false;
		s.player.sleep_timer = guest.sleep_timer = 0;
	}
	printf("both-legs: %u seeds; host leg spawns %u, guest leg spawns %u, both %u; RNG agrees on %u, "
		   "outcome agrees on %u\n", SEEDS, host_spawns, guest_spawns, both, rng_agree, outcome_agree);
	check(host_spawns > 0 && guest_spawns > 0 && both > 0,
		"fixture: both legs spawn for some seeds, and BOTH spawn for some (the divergent case is exercised)");
	check(rng_agree == SEEDS,
		"RNG order: after a HOST spawn the guest's 20 candidates are still drawn (performSleepSpawning walks "
		"the whole list, SpawnerAnimals.java:174-183; var7 ends only the host's loop, :240)  [H1]");
	check(outcome_agree == SEEDS,
		"outcome: each sleeper wakes iff its own leg spawned; the skip to dawn only when neither did "
		"(var2 accumulated, :239, returned at :180; World.tick:1748-1751)  [H1]");
}

static void case_all_sleepers(void) {
	unsigned used = 0, rng_agree = 0, guest_spawn_seeds = 0, outcome_agree = 0;
	for(uint32_t seed = 1; seed <= SEEDS; seed++) {
		prepare(seed * 6151u);
		struct random_gen ref = {.seed = seed * 6151u};
		uint32_t ref_id = s.next_entity_id;
		struct ref_spawn gs;
		double hx = s.player.x, hy = s.player.y, hz = s.player.z;
		double gx = guest.x, gy = guest.y, gz = guest.z;
		if(ref_player_leg(&ref, &ref_id, NULL, hx, hy, hz))
			continue; // the host leg spawned: out of this fixture's scope (it should never)
		bool guest_spawn = ref_player_leg(&ref, &ref_id, &gs, gx, gy, gz);
		used++;
		guest_spawn_seeds += guest_spawn;
		server_local_sleep_tick(&s);
		rng_agree += s.rand_src.seed == ref.seed && s.next_entity_id == ref_id && monster_owns(&gs);
		bool ok = guest_spawn
			/* Beta: a monster beside the GUEST, the guest woken at once (timer 0, no spawn point), the
			 * skip cancelled, the HOST still asleep (only var5 is woken, :235). */
			? (count_monsters() == 1 && !guest.sleeping && guest.sleep_timer == 0 && !guest.has_bed_spawn
			   && s.world_time == 18000 && s.player.sleeping)
			/* Beta: nothing spawned for either -> skip to dawn, both woken with their own bed spawn. */
			: (count_monsters() == 0 && s.world_time == 24001 && !s.player.sleeping && !guest.sleeping);
		outcome_agree += ok;
		s.player.sleeping = guest.sleeping = false;
		s.player.sleep_timer = guest.sleep_timer = 0;
	}
	printf("%u seeds used (host leg never spawned), %u with a Beta guest-leg spawn; RNG agrees on %u, "
		   "outcome agrees on %u\n", used, guest_spawn_seeds, rng_agree, outcome_agree);
	check(used == SEEDS, "fixture: the host leg never spawns (void around the host's bed)");
	check(guest_spawn_seeds > 0, "fixture: Beta's guest leg spawns for some seeds (open field at the guest)");
	check(rng_agree == used,
		"per-sleeper order: after the host's 20 candidates the world RNG has drawn the GUEST's candidates "
		"too (performSleepSpawning iterates every player, SpawnerAnimals.java:172-185)  "
		"[L1: RED -- only the host is tried]");
	check(outcome_agree == used,
		"outcome: a Beta spawn beside the guest wakes ONLY the guest and cancels the skip; otherwise the skip "
		"to dawn wakes both (SpawnerAnimals.java:222-238, World.tick:1745-1752)  [L1: RED]");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_sleep_spawn_all_sleepers_tmp", fixture, sizeof fixture);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	MKDIR(fixture);
	MKDIR(test_fixture_join(fixture, "/region"));
	remove(test_fixture_join(fixture, "/region/r.0.0.mcr"));
	remove(test_fixture_join(fixture, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(fixture, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(fixture, "/region/r.-1.-1.mcr"));
	string_t level;
	string_init_set_str(level, fixture);
	memset(&s, 0, sizeof s);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s, &s.player,
		INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	for(int cx = -4; cx <= 3; cx++)
		for(int cz = -4; cz <= 3; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	s.player.has_pos = s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	/* The guest: an admitted gameplay body in the same (Overworld) world. */
	memset(&guest, 0, sizeof guest);
	guest.has_pos = guest.finished_loading = true;
	guest.health = MAX_PLAYER_HEALTH;
	guest.next_step_distance = 1;
	s.guest_actor = &guest;
	s.guest_gameplay = true;
	s.guest_generation = 1;
	assert(server_local_world_of(&s, &guest) == &s.worlds[0]);
	/* Host bed: foot (0,Y,-1), head (0,Y,0), on two stone supports, void around. */
	set(0, Y - 1, -1, BLOCK_STONE, 0);
	set(0, Y - 1, 0, BLOCK_STONE, 0);
	set(0, Y, -1, BLOCK_BED, 0);
	set(0, Y, 0, BLOCK_BED, 8);
	/* Guest field + bed: foot (GX,Y,-1), head (GX,Y,0). */
	for(int x = FIELD_X0; x <= 63; x++)
		for(int z = -40; z <= 40; z++)
			set(x, Y - 1, z, BLOCK_STONE, 0);
	set(GX, Y, -1, BLOCK_BED, 0);
	set(GX, Y, 0, BLOCK_BED, 8);
	server_local_test_set_difficulty(DIFFICULTY_NORMAL);

	case_all_sleepers();

	/* Case 2: give the host an open field too (its bed at (HX,Y,0), the bed at (0,Y,0) stays unused). */
	for(int x = -63; x <= HFIELD_X1; x++)
		for(int z = -40; z <= 40; z++)
			set(x, Y - 1, z, BLOCK_STONE, 0);
	set(HX, Y, -1, BLOCK_BED, 0);
	set(HX, Y, 0, BLOCK_BED, 8);
	case_both_legs();

	s.guest_actor = NULL;
	s.guest_gameplay = false;
	clear_entities();
	draining = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	test_fixture_dir_cleanup(fixture);
	if(failures) {
		printf("test_sleep_spawn_all_sleepers: %u FAILED\n", failures);
		return 1;
	}
	printf("test_sleep_spawn_all_sleepers: PASS\n");
	return 0;
}
