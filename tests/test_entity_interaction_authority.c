/*
	GB-06 (docs/audit_2026-08-14_game_breakage.md): the server-side
	entity-interaction authority gate. Ground truth: decompile 740c583,
	NetServerHandler.func_6006_a (NetServerHandler.java:477-488):

	    if (var3 != null && this.playerEntity.canEntityBeSeen(var3)
	        && this.playerEntity.getDistanceSqToEntity(var3) < 36.0D)

	ONE gate for BOTH Packet7UseEntity legs -- attack (isLeftClick==1) AND
	use (==0). A forged/stale/duplicated RPC must mutate NOTHING when the
	gate rejects: no health, no weapon wear, no wolf state, no pig-zombie
	pack anger, no RNG draws, no knockback, no mount/occupancy change.

	Endpoint conventions under test (the engine's canonical documented
	choices, NOT fresh inventions -- see the Y-BASIS DEVIATION note in
	entity_monster.c and the validator's own header comment):
	  - distance: strict < 36.0, FEET-to-feet Y basis;
	  - LOS: player network eye pos (raw) -> target pos + per-kind Beta
	    getEyeHeight() (monsters height*0.85F via mob_eye_height();
	    minecart/boat/fireball 0.0F per Entity.getEyeHeight()).

	Coverage:
	  S1  positive control: visible in-range zombie -- exact damage, weapon
	      wear, knockback, retaliation latch all land.
	  S2  boundary: flat-ground 6.0 blocks == distSq 36.0 exactly -> reject
	      (strict <); 5.8 -> accept.
	  S3  wall: in-range zombie behind stone -> reject, ZERO mutation sweep
	      (health, hurt window, aggro, durability, RNG seed, velocity).
	  S4  order proof: gate runs BEFORE the death-flop guard -- a rejected
	      attack on a dying mob must NOT reset entity_age (Beta: the packet
	      gate precedes attackEntityFrom, whose first statement is the age
	      reset); an accepted one must.
	  S5  pig zombie: rejected attack angers NOBODY in the pack and draws
	      no RNG; accepted attack angers the whole pack.
	  S6  wolf: rejected attack leaves health/anger untouched, no wear.
	  S7  saddle: rejected attack consumes no saddle (the scarce-consumable
	      case the old inner gate protected -- now the branch gate).
	  S8  minecart attack: through-wall rejected (NEW -- the cart branch
	      previously had reach only), open accepted (+10 damage).
	  S9  boat attack: through-wall still rejected (regression pin -- the
	      boat branch already had LOS).
	  S10 USE / milking: through-wall rejected (bucket intact), open
	      accepted (milk bucket).
	  S11 ride-intent mount: through-wall rejected (occupancy false), open
	      accepted.
	  S12 stale/absent id: no crash, no RNG draw.
	  S13 fireball: through-wall rejected (velocity untouched), open
	      deflected along the player look (runs LAST -- the spawn is
	      queued and needs one entity tick to flush).
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
#include "support/mob_motion_fixture.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/thread.h"
#include "worldgen/java_random.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_entity_authority_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-4f;
}

extern struct inventory_logic inventory_logic_player;

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

static void set_block(int x, int y, int z, int type) {
	bool ok = server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type, .metadata = 0});
	if(!ok) // stdout is fully buffered under a pipe; name the site on stderr
		fprintf(stderr, "set_block FAILED at (%d,%d,%d) type=%d\n", x, y, z,
				type);
	assert(ok && "server_world_set_block refused the edit");
}

/* A 1-wide, 3-high stone wall column: covers feet..eye cells of both a
   standing player (eye cell ground+2) and any biped/quadruped target. */
static void wall_column(int x, int gy, int z) {
	set_block(x, gy + 1, z, BLOCK_STONE);
	set_block(x, gy + 2, z, BLOCK_STONE);
	set_block(x, gy + 3, z, BLOCK_STONE);
}

static void attack_entity(uint32_t id) {
	struct server_rpc rpc = {
		.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = id,
	};
	server_local_test_process_rpc(&s, &rpc);
}

static void use_entity(uint32_t id) {
	struct server_rpc rpc = {
		.type = SRPC_ENTITY_USE,
		.payload.entity_use.entity_id = id,
	};
	server_local_test_process_rpc(&s, &rpc);
}

static void ride_intent(uint32_t id, uint32_t seq, bool desired) {
	struct server_rpc rpc = {
		.type = SRPC_MINECART_RIDE_INTENT,
		.payload.minecart_ride_intent = {
			.entity_id = id,
			.input_epoch = s.input_epoch_counter,
			.request_seq = seq,
			.desired_occupied = desired,
		},
	};
	server_local_test_process_rpc(&s, &rpc);
}

static struct item_data* hotbar_slot(size_t k) {
	return &s.player.inventory.items[INVENTORY_SLOT_HOTBAR + k];
}

static struct entity* find_type(enum entity_type type) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == type)
			return e;
		dict_entity_next(it);
	}
	return NULL;
}

/* Place the player's FEET at (x, gy+1, z): s.player.y is the network EYE. */
static void player_at(double x, double gy_feet, double z) {
	s.player.x = x;
	s.player.y = gy_feet + PLAYER_EYE_HEIGHT;
	s.player.z = z;
	s.player.rx = 0.0f;
	s.player.ry = 0.0f;
}

int main(void) {
	test_fixture_dir("test_entity_authority_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	assert(svin_init());
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
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 777,
						true /* superflat */);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = UINT32_C(0xC0FFEE01);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;

	/* The sub-test zones span x,z in 0..31: keep all four chunks resident so
	   the LOS predicate reads real blocks (it fails OPEN on unloaded cells). */
	for(int cx = 0; cx < 2; cx++)
		for(int cz = 0; cz < 2; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	/* Superflat surface probe (the test_pig_saddle_env.c idiom). */
	int gy = -1;
	for(int y = 127; y >= 0; y--) {
		struct block_data probe;
		if(server_world_get_block(&s.world, 8, y, 8, &probe)
		   && probe.type != BLOCK_AIR) {
			gy = y;
			break;
		}
	}
	assert(gy > 0);
	const float feet = (float)(gy + 1);

	inventory_set_hotbar(&s.player.inventory, 0);

	/* ============ S1: positive control -- visible, in range ============= */
	player_at(2.5, gy + 1, 2.5);
	struct entity* z1 = server_local_spawn_monster_at(
		(vec3) {2.5f, feet, 5.5f}, MONSTER_ZOMBIE, &s); /* 3.0 blocks */
	assert(z1);
	int z1_hp0 = z1->health;
	/* Spawn gives every mob a random initial velocity -- knockback is
	   proven by CHANGE, not by nonzero. */
	float z1_vel0[2] = {z1->vel[0], z1->vel[2]};
	*hotbar_slot(0) = (struct item_data) {0}; /* fist: damage 1 */
	attack_entity(z1->id);
	assert(z1->health == z1_hp0 - 1 && "visible in-range fist hit lands");
	assert(z1->data.monster.aggro_time > 0 && "retaliation latch armed");
	assert((z1->vel[0] != z1_vel0[0] || z1->vel[2] != z1_vel0[1])
		   && "knockback rewrote the horizontal velocity");
	puts("  S1: visible in-range attack lands (damage+latch+knockback)");

	/* ============ S2: strict < 36.0 boundary ============================ */
	player_at(8.5, gy + 1, 2.5);
	struct entity* z2 = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 8.5f}, MONSTER_ZOMBIE, &s); /* 6.0 blocks flat */
	assert(z2);
	int z2_hp0 = z2->health;
	uint32_t seed0 = s.rand_src.seed;
	attack_entity(z2->id);
	assert(z2->health == z2_hp0 && "distSq == 36.0 exactly must reject");
	assert(z2->data.monster.aggro_time == 0);
	assert(s.rand_src.seed == seed0 && "rejected attack draws no RNG");
	player_at(8.5, gy + 1, 2.7); /* 5.8 blocks */
	attack_entity(z2->id);
	assert(z2->health == z2_hp0 - 1 && "5.8 blocks is inside the gate");
	puts("  S2: 6.0-block boundary rejects (strict <), 5.8 accepts");

	/* ============ S3: through-wall zero-mutation sweep ================== */
	player_at(14.5, gy + 1, 2.5);
	struct entity* z3 = server_local_spawn_monster_at(
		(vec3) {14.5f, feet, 6.5f}, MONSTER_ZOMBIE, &s); /* 4.0 blocks */
	assert(z3);
	wall_column(14, gy, 4);
	*hotbar_slot(0)
		= (struct item_data) {.id = ITEM_IRON_SWORD, .count = 1};
	int z3_hp0 = z3->health;
	float z3_vel0[2] = {z3->vel[0], z3->vel[2]};
	uint16_t dur0 = hotbar_slot(0)->durability;
	seed0 = s.rand_src.seed;
	attack_entity(z3->id);
	assert(z3->health == z3_hp0 && "wall blocks the hit");
	assert(z3->data.monster.aggro_time == 0 && "no latch through a wall");
	assert(z3->vel[0] == z3_vel0[0] && z3->vel[2] == z3_vel0[1]
		   && "no knockback write on reject");
	assert(hotbar_slot(0)->durability == dur0 && "no weapon wear on reject");
	assert(s.rand_src.seed == seed0 && "no RNG draw on reject");
	*hotbar_slot(0) = (struct item_data) {0};
	puts("  S3: through-wall attack mutates NOTHING");

	/* ============ S4: gate ordering vs the death-flop age reset ========= */
	player_at(20.5, gy + 1, 2.5);
	struct entity* z4 = server_local_spawn_monster_at(
		(vec3) {20.5f, feet, 12.5f}, MONSTER_ZOMBIE, &s); /* 10 blocks: out */
	assert(z4);
	z4->data.monster.death_time = 5; /* mid death-flop */
	z4->data.monster.entity_age = 123;
	attack_entity(z4->id);
	assert(z4->data.monster.entity_age == 123
		   && "REJECTED attack must not touch entity_age (gate precedes "
			  "the attackEntityFrom-internal age reset)");
	player_at(20.5, gy + 1, 9.5); /* 3 blocks: in range, visible */
	attack_entity(z4->id);
	assert(z4->data.monster.entity_age == 0
		   && "accepted attack on a dying mob still resets entityAge "
			  "(EntityLiving.attackEntityFrom first statement)");
	puts("  S4: gate runs BEFORE the death-flop guard (age-reset proof)");

	/* ============ S5: pig-zombie pack anger needs an ACCEPTED hit ======= */
	player_at(26.5, gy + 1, 2.5);
	struct entity* pz1 = server_local_spawn_monster_at(
		(vec3) {26.5f, feet, 6.5f}, MONSTER_PIG_ZOMBIE, &s);
	struct entity* pz2 = server_local_spawn_monster_at(
		(vec3) {28.5f, feet, 6.5f}, MONSTER_PIG_ZOMBIE, &s);
	assert(pz1 && pz2);
	for(int wx = 24; wx <= 29; wx++)
		wall_column(wx, gy, 4);
	/* RNG OWNER [b1.7.3 EntityPigZombie.java:67-70 becomeAngryAt]:
	     angerLevel = 400 + rand.nextInt(400);
	     randomSoundDelay = rand.nextInt(40);
	   that rand is the entity's own per-instance java.util.Random
	   (Entity.java:50), NOT World.rand -- so the draws belong to EACH pig
	   zombie's own gameplay_rng (Java48 mob-RNG slice, 0db9e79ea), never the
	   session rand_src. Replay copies of each owner's stream to prove the
	   exact owner, the exact values and the anger-then-delay order. */
	seed0 = s.rand_src.seed;
	struct java_random pz1_rng0 = pz1->gameplay_rng;
	struct java_random pz2_rng0 = pz2->gameplay_rng;
	attack_entity(pz1->id);
	assert(pz1->data.monster.anger_level == 0
		   && pz2->data.monster.anger_level == 0
		   && "no pack anger through a wall");
	assert(s.rand_src.seed == seed0
		   && "rejected pig-zombie attack draws no session RNG");
	assert(pz1->gameplay_rng.seed == pz1_rng0.seed
		   && pz2->gameplay_rng.seed == pz2_rng0.seed
		   && "rejected pig-zombie attack draws no anger RNG");
	player_at(26.5, gy + 1, 5.2); /* inside, past the wall */
	const int pz1_hp0 = pz1->health;
	attack_entity(pz1->id);
	{
		struct java_random r1 = pz1_rng0, r2 = pz2_rng0;
		const int a1 = 400 + jrand_next_int_bound(&r1, 400);
		const int d1 = jrand_next_int_bound(&r1, 40);
		const int a2 = 400 + jrand_next_int_bound(&r2, 400);
		const int d2 = jrand_next_int_bound(&r2, 40);
		assert(pz1->data.monster.anger_level >= 400
			   && pz2->data.monster.anger_level >= 400
			   && "accepted hit angers the whole pack");
		assert(pz1->data.monster.anger_level == a1
			   && pz1->data.monster.anger_sound_delay == d1
			   && "struck pig zombie's anger draws come from ITS Entity.rand");
		assert(pz2->data.monster.anger_level == a2
			   && pz2->data.monster.anger_sound_delay == d2
			   && "neighbour's anger draws come from ITS Entity.rand");
		/* LIVING-SOUND-RNG: the struck pig zombie's base attackEntityFrom
		   then lands a FRESH hit (it was unhurt) and draws the hurt pitch
		   (rand.nextFloat() - rand.nextFloat()) * 0.2F + 1.0F on
		   that SAME Random, AFTER becomeAngryAt [server EntityPigZombie.java:
		   61-64 becomeAngryAt then the base attackEntityFrom; EntityLiving.java:
		   345 hurt pitch]. The neighbour is not struck: anger draws only. */
		assert(pz1->health < pz1_hp0 && pz1->health > 0
			   && "precondition: the struck pig zombie took a fresh hit");
		(void)jrand_next_float(&r1);
		(void)jrand_next_float(&r1);
		assert(pz1->gameplay_rng.seed == r1.seed
			   && pz2->gameplay_rng.seed == r2.seed
			   && "becomeAngryAt draws exactly nextInt(400), nextInt(40); the "
				  "struck one then exactly its 2 hurt-pitch floats");
	}
	puts("  S5: pig-zombie pack anger gated (reject = no anger, no RNG)");

	/* ============ S6: wolf through a wall ============================== */
	player_at(2.5, gy + 1, 20.5);
	struct entity* wolf = server_local_spawn_monster_at(
		(vec3) {2.5f, feet, 24.5f}, MONSTER_WOLF, &s);
	assert(wolf);
	wall_column(2, gy, 22);
	int wolf_hp0 = wolf->health;
	seed0 = s.rand_src.seed;
	attack_entity(wolf->id);
	assert(wolf->health == wolf_hp0 && "wall protects the wolf");
	assert(s.rand_src.seed == seed0);
	puts("  S6: wolf attack rejected through wall (no damage path entered)");

	/* ============ S7: saddle is not consumed through a wall ============= */
	player_at(8.5, gy + 1, 20.5);
	struct entity* pig = server_local_spawn_monster_at(
		(vec3) {8.5f, feet, 24.5f}, MONSTER_PIG, &s);
	assert(pig && !pig->data.monster.saddled);
	wall_column(8, gy, 22);
	*hotbar_slot(0) = (struct item_data) {.id = ITEM_SADDLE, .count = 1};
	attack_entity(pig->id);
	assert(!pig->data.monster.saddled && "no saddling through a wall");
	assert(hotbar_slot(0)->id == ITEM_SADDLE && hotbar_slot(0)->count == 1
		   && "saddle not consumed on reject");
	pig->pos[2] = 21.5f; /* pull the pig to our side of the wall */
	attack_entity(pig->id);
	assert(pig->data.monster.saddled && hotbar_slot(0)->id == 0
		   && "branch-level gate still saddles a visible in-range pig");
	*hotbar_slot(0) = (struct item_data) {0};
	puts("  S7: saddle consumption obeys the branch gate");

	/* ============ S8: minecart attack gains LOS ========================= */
	player_at(14.5, gy + 1, 20.5);
	struct entity* cart = server_local_spawn_minecart(
		(vec3) {14.5f, feet, 24.5f}, &s);
	assert(cart);
	wall_column(14, gy, 22);
	int16_t cart_dmg0 = cart->data.minecart.damage_taken;
	attack_entity(cart->id);
	assert(cart->data.minecart.damage_taken == cart_dmg0
		   && "cart cannot be beaten through a wall (NEW: LOS on carts)");
	cart->pos[2] = 21.5f;
	attack_entity(cart->id);
	assert(cart->data.minecart.damage_taken == cart_dmg0 + 10
		   && "open fist hit adds strength*10");
	puts("  S8: minecart attack now LOS-gated");

	/* ============ S9: boat attack keeps its LOS (regression pin) ======== */
	player_at(20.5, gy + 1, 20.5);
	struct entity* boat = server_local_spawn_boat(
		(vec3) {20.5f, feet, 24.5f}, &s);
	assert(boat);
	wall_column(20, gy, 22);
	int16_t boat_dmg0 = boat->data.boat.damage_taken;
	attack_entity(boat->id);
	assert(boat->data.boat.damage_taken == boat_dmg0
		   && "boat still cannot be beaten through a wall");
	boat->pos[2] = 21.5f;
	attack_entity(boat->id);
	assert(boat->data.boat.damage_taken == boat_dmg0 + 10);
	puts("  S9: boat attack LOS regression pinned");

	/* ============ S10: USE (milking) through a wall ===================== */
	player_at(26.5, gy + 1, 20.5);
	struct entity* cow = server_local_spawn_monster_at(
		(vec3) {26.5f, feet, 24.5f}, MONSTER_COW, &s);
	assert(cow);
	wall_column(26, gy, 22);
	*hotbar_slot(0) = (struct item_data) {.id = ITEM_BUCKET, .count = 1};
	use_entity(cow->id);
	assert(hotbar_slot(0)->id == ITEM_BUCKET
		   && "no milking through a wall (NEW: LOS on entity use)");
	cow->pos[2] = 21.5f;
	use_entity(cow->id);
	assert(hotbar_slot(0)->id == ITEM_MILK_BUCKET
		   && "visible in-range milking still works");
	*hotbar_slot(0) = (struct item_data) {0};
	puts("  S10: entity USE now LOS-gated (milking)");

	/* ============ S11: ride-intent mount through a wall ================= */
	s.input_epoch_counter = 1;
	player_at(2.5, gy + 1, 27.5);
	struct entity* cart2 = server_local_spawn_minecart(
		(vec3) {2.5f, feet, 30.5f}, &s); /* 3 blocks */
	assert(cart2);
	wall_column(2, gy, 29);
	ride_intent(cart2->id, 1, true);
	assert(!cart2->data.minecart.occupied
		   && "no mounting through a wall (NEW: LOS on ride-intent)");
	cart2->pos[2] = 28.5f;
	ride_intent(cart2->id, 2, true);
	assert(cart2->data.minecart.occupied
		   && "visible in-range mount still works");
	ride_intent(cart2->id, 3, false); /* dismount stays un-gated */
	assert(!cart2->data.minecart.occupied);
	puts("  S11: ride-intent mount LOS-gated; dismount untouched");

	/* ============ S12: stale/absent target id =========================== */
	seed0 = s.rand_src.seed;
	attack_entity(UINT32_C(424242));
	use_entity(UINT32_C(424242));
	assert(s.rand_src.seed == seed0 && "absent id is a pure no-op");
	puts("  S12: stale/absent target id is a no-op");

	/* ============ S13: fireball deflection (queued spawn -> LAST) ======= */
	player_at(8.5, gy + 1, 27.5);
	server_local_spawn_fireball((vec3) {8.5f, feet + 1.0f, 30.5f},
								(float[3]) {0.0f, 0.0f, -1.0f}, 0, &s);
	server_world_begin_update(&s.world);
	assert(server_local_test_tick_entities(&s));
	struct entity* fb = find_type(ENTITY_FIREBALL);
	assert(fb && "queued fireball flushed by the tick");
	fb->pos[0] = 8.5f;
	fb->pos[1] = feet + 1.0f;
	fb->pos[2] = 30.5f;
	wall_column(8, gy, 29);
	float fb_vel0[3] = {fb->vel[0], fb->vel[1], fb->vel[2]};
	attack_entity(fb->id);
	assert(fb->vel[0] == fb_vel0[0] && fb->vel[1] == fb_vel0[1]
		   && fb->vel[2] == fb_vel0[2]
		   && "no punch-deflection through a wall (NEW: LOS on fireballs)");
	set_block(8, gy + 1, 29, BLOCK_AIR);
	set_block(8, gy + 2, 29, BLOCK_AIR);
	set_block(8, gy + 3, 29, BLOCK_AIR);
	attack_entity(fb->id);
	/* player_at set rx=ry=0 -> look == {0,0,1}: deflected AWAY from us. */
	assert(fb->vel[2] > 0.9f && "open punch re-aims the fireball along look");
	puts("  S13: fireball deflection LOS-gated");

	/* ============ S14: knockback COMPOSES with existing velocity (fix) === */
	// Beta EntityLiving.knockBack HALVES the mob's EXISTING velocity, THEN
	// adds the 0.4 impulse -- it never overwrites outright. Seed a nonzero
	// residual (as if the mob were still sliding from an earlier shove) and
	// confirm a real SRPC_ENTITY_ATTACK hit, through the actual server_local.c
	// handler, composes with it rather than discarding it.
	player_at(14.5, gy + 1, 27.5);
	struct entity* z14 = server_local_spawn_monster_at(
		(vec3) {14.5f, feet, 30.5f}, MONSTER_ZOMBIE, &s); /* 3 blocks: visible */
	assert(z14);
	z14->vel[0] = 1.0f; /* an arbitrary pre-existing rightward residual */
	z14->vel[2] = 0.0f;
	test_mob_sync(z14); /* motion units S2: the fixture pins the doubles */
	*hotbar_slot(0) = (struct item_data) {0}; /* fist */
	attack_entity(z14->id);
	// Mob motion units S2: EntityLiving.knockBack on the authoritative doubles,
	// composed at the hit: var3 = player.posX - posX = 0, var5 = player.posZ -
	// posZ = 27.5 - 30.5 = -3, f = (float)sqrt(9) = 3:
	//   motionX = 1.0 / 2.0D - 0 / 3 * 0.4F = 0.5
	//   motionZ = 0.0 / 2.0D - (-3.0) / (double)3.0F * (double)0.4F
	double var5 = 27.5 - (double)z14->data.monster.motion_d.pos[2];
	float f = (float)sqrt(var5 * var5);
	double expect_x = 1.0 / 2.0;
	double expect_z = 0.0 / 2.0 - var5 / (double)f * (double)0.4F;
	assert(z14->data.monster.motion_d.motion[0] == expect_x
		   && "the halved residual (0.5) must survive -- an outright "
			  "overwrite would have zeroed this axis");
	assert(z14->data.monster.motion_d.motion[2] == expect_z);
	puts("  S14: knockback composes with a pre-existing residual velocity "
		 "(halve-then-add, not overwrite)");

	g_drain_run = 0;
	thread_join(&drain);
	string_clear(level);
	puts("test_entity_interaction_authority: ALL PASS");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
