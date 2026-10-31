/*
	Mob motion units S2, Q3 (Astra 2026-09-27) + Fable S1 review F2: every
	mob hit site composes EntityLiving.knockBack AT THE HIT, on the
	authoritative doubles, from the ATTACKER Beta passes to attackEntityFrom:

	  EntityLiving.attackEntityFrom:336-346 (a FRESH hit, non-null attacker):
	      dx = attacker.posX - posX; dz = attacker.posZ - posZ;
	      knockBack(attacker, dmg, dx, dz);
	  EntityLiving.knockBack:392-405:
	      len = (float)sqrt(dx*dx + dz*dz); strength = 0.4F;
	      motion /= 2.0D; motionX -= dx / (double)len * (double)strength;
	      motionY += (double)0.4F; motionZ -= dz / (double)len * (double)strength;
	      if(motionY > (double)0.4F) motionY = (double)0.4F;

	  melee   SRPC_ENTITY_ATTACK -> attackEntityFrom(player)       [server_local.c]
	  arrow   EntityArrow:163 attackEntityFrom(shootingEntity, 4)  [entity_arrow.c]
	          (the pre-S2 "v2 Wii-feel 0.18 shove along the flight" is retired)
	  egg     EntityEgg attackEntityFrom(thrower, 0)               [entity_egg.c]
	  fish    EntityFish attackEntityFrom(angler, 0)               [entity_fish.c]

	Each case drives the real production path, reads the victim's motion just
	before the hit, and compares the result BYTE-FOR-BYTE with the knockBack
	formula evaluated here; the bypass counter must stay 0 (the site writes
	through the mutation API, never the float mirror). RED before S2: the
	sites composed a float halve-then-add along a unit direction, deferred the
	vertical pop to the victim's next tick, and the arrow shoved along its
	flight instead of away from the shooter.

	Cases (argv[1]): melee, arrow, egg, fish.
*/
#include <assert.h>
#include <math.h>
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
#include "entity/entity_fish.h"
#include "entity/mob_motion.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "util.h"
#include "support/drain_quiesce.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define FLOOR_Y 100

static struct server_local s;

static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type) {
	server_world_set_block(&s.worlds[0], x, y, z,
						   (struct block_data) {.type = (uint8_t)type});
}

static bool same_d(double a, double b) {
	return memcmp(&a, &b, sizeof a) == 0;
}

/* The reference: attackEntityFrom's var4/var6 + knockBack, from the decompile. */
static void beta_knockback(double motion[3], double var4, double var6) {
	float var7 = (float)sqrt(var4 * var4 + var6 * var6);
	float var8 = 0.4F;
	motion[0] /= 2.0;
	motion[1] /= 2.0;
	motion[2] /= 2.0;
	motion[0] -= var4 / (double)var7 * (double)var8;
	motion[1] += (double)0.4F;
	motion[2] -= var6 / (double)var7 * (double)var8;
	if(motion[1] > (double)0.4F)
		motion[1] = (double)0.4F;
}

static int check_site(const char* tag, const struct entity* v,
					  const double want[3]) {
	const struct mob_motion* m = &v->data.monster.motion_d;
	for(int i = 0; i < 3; i++)
		if(!same_d(m->motion[i], want[i])) {
			fprintf(stderr, "RED %s axis %d: motion %.17g, Beta knockBack %.17g\n",
					tag, i, m->motion[i], want[i]);
			return 1;
		}
	for(int i = 0; i < 3; i++)
		if(v->vel[i] != (float)want[i]) {
			fprintf(stderr, "RED %s axis %d: float mirror %.9g != (float)%.17g\n",
					tag, i, v->vel[i], want[i]);
			return 1;
		}
	if(v->data.monster.knockback_time != MOB_KNOCKBACK_TICKS) {
		fprintf(stderr, "RED %s: steer-suppression window not armed (%d)\n", tag,
				v->data.monster.knockback_time);
		return 1;
	}
	if(mob_motion_bypass_count() != 0) {
		fprintf(stderr, "RED %s: %u bypass(es)\n", tag, mob_motion_bypass_count());
		return 1;
	}
	printf("  %s: knockBack from the attacker composed at the hit, byte-exact "
		   "(%.6f, %.6f, %.6f)\n",
		   tag, want[0], want[1], want[2]);
	return 0;
}

/* A pig at rest with a known residual motion (so the halving is visible). */
static struct entity* victim_at(double x, double z) {
	struct entity* v = server_local_spawn_monster(
		(vec3) {(float)x, (float)(FLOOR_Y + 1), (float)z}, MONSTER_PIG, &s);
	assert(v);
	mob_motion_set_velocity(v, &v->data.monster.motion_d, 0.0625, -0.0784,
							-0.03125);
	v->data.monster.hurt_resist_time = 0;
	v->data.monster.knockback_time = 0;
	return v;
}

static void host_at(double x, double z) {
	s.player.x = x;
	s.player.y = (double)(FLOOR_Y + 1) + PLAYER_EYE_HEIGHT;
	s.player.z = z;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
}

static int case_melee(void) {
	host_at(20.5, 18.0);
	struct entity* v = victim_at(20.9, 20.3);
	double want[3];
	memcpy(want, v->data.monster.motion_d.motion, sizeof want);
	beta_knockback(want, s.player.x - v->data.monster.motion_d.pos[0],
				   s.player.z - v->data.monster.motion_d.pos[2]);
	mob_motion_bypass_reset();
	struct server_rpc rpc = {.type = SRPC_ENTITY_ATTACK,
							 .payload.entity_attack.entity_id = v->id};
	server_local_test_process_rpc(&s, &rpc);
	if(v->health == 10) {
		fprintf(stderr, "melee: the attack did not land (reach/LOS fixture)\n");
		return 2;
	}
	return check_site("melee", v, want);
}

static uint32_t g_next_id = 800000;

static int case_arrow(void) {
	host_at(12.5, 30.5); /* the shooter: off the flight line */
	struct entity* v = victim_at(20.5, 20.5);
	double want[3];
	memcpy(want, v->data.monster.motion_d.motion, sizeof want);
	beta_knockback(want, s.player.x - v->data.monster.motion_d.pos[0],
				   s.player.z - v->data.monster.motion_d.pos[2]);
	/* A player-fired arrow 1.2 blocks -X of the pig's body, flying +X. */
	uint32_t id = g_next_id++;
	struct entity** pp = dict_entity_safe_get(s.entities, id);
	*pp = malloc(sizeof(struct entity));
	struct entity* a = *pp;
	assert(a);
	entity_arrow(id, a, true, &s.worlds[0], 0, ITEM_ARROW, ARROW_DAMAGE, true);
	a->teleport(a, (vec3) {19.3f, (float)(FLOOR_Y + 1) + 0.45f, 20.5f});
	glm_vec3_copy((vec3) {1.5f, 0.0f, 0.0f}, a->vel);
	a->data.arrow.age = 10; /* past the owner grace */
	mob_motion_bypass_reset();
	bool gone = a->tick_server(a, &s);
	(void)gone;
	if(v->health == 10) {
		fprintf(stderr, "arrow: did not hit the pig\n");
		return 2;
	}
	return check_site("arrow", v, want);
}

static int case_egg(void) {
	host_at(12.5, 30.5);
	struct entity* v = victim_at(20.5, 20.5);
	double want[3];
	memcpy(want, v->data.monster.motion_d.motion, sizeof want);
	beta_knockback(want, s.player.x - v->data.monster.motion_d.pos[0],
				   s.player.z - v->data.monster.motion_d.pos[2]);
	uint32_t id = g_next_id++;
	struct entity** pp = dict_entity_safe_get(s.entities, id);
	*pp = malloc(sizeof(struct entity));
	struct entity* egg = *pp;
	assert(egg);
	entity_egg(id, egg, true, &s.worlds[0], 0);
	egg->teleport(egg, (vec3) {19.3f, (float)(FLOOR_Y + 1) + 0.45f, 20.5f});
	glm_vec3_copy((vec3) {1.5f, 0.0f, 0.0f}, egg->vel);
	mob_motion_bypass_reset();
	(void)egg->tick_server(egg, &s);
	if(v->data.monster.knockback_time == 0) {
		fprintf(stderr, "egg: did not hit the pig\n");
		return 2;
	}
	return check_site("egg", v, want);
}

static int case_fish(void) {
	host_at(12.5, 30.5); /* the angler */
	struct entity* v = victim_at(20.5, 20.5);
	double want[3];
	memcpy(want, v->data.monster.motion_d.motion, sizeof want);
	beta_knockback(want, s.player.x - v->data.monster.motion_d.pos[0],
				   s.player.z - v->data.monster.motion_d.pos[2]);
	uint32_t id = g_next_id++;
	struct entity** pp = dict_entity_safe_get(s.entities, id);
	*pp = malloc(sizeof(struct entity));
	struct entity* f = *pp;
	assert(f);
	entity_fish(id, f, true, &s.worlds[0]);
	f->teleport(f, (vec3) {19.3f, (float)(FLOOR_Y + 1) + 0.45f, 20.5f});
	glm_vec3_copy((vec3) {1.5f, 0.0f, 0.0f}, f->vel);
	s.player.fish_entity_id = id;
	/* the angler must hold a rod or the hook removes itself */
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
					   (struct item_data) {.id = ITEM_FISHING_ROD,
										   .durability = 0, .count = 1});
	mob_motion_bypass_reset();
	(void)f->tick_server(f, &s);
	if(v->data.monster.knockback_time == 0) {
		fprintf(stderr, "fish: did not hook the pig\n");
		return 2;
	}
	return check_site("fish", v, want);
}

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "";
	char tag[64];
	snprintf(tag, sizeof tag, "test_mob_kb_sites_%s_tmp", which);
	test_fixture_dir(tag, s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	/* Overworld-only fixture: worlds[0] named explicitly (world alias gate). */
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0x4B0CBAC4u;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	for(int cz = -1; cz <= 3; cz++)
		for(int cx = -1; cx <= 3; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	for(int x = 0; x < 40; x++)
		for(int z = 0; z < 40; z++) {
			set(x, FLOOR_Y, z, BLOCK_STONE);
			for(int y = FLOOR_Y + 1; y < FLOOR_Y + 8; y++)
				set(x, y, z, BLOCK_AIR);
		}

	int rc;
	if(!strcmp(which, "melee"))
		rc = case_melee();
	else if(!strcmp(which, "arrow"))
		rc = case_arrow();
	else if(!strcmp(which, "egg"))
		rc = case_egg();
	else if(!strcmp(which, "fish"))
		rc = case_fish();
	else {
		fprintf(stderr, "unknown case '%s'\n", which);
		rc = 2;
	}
	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	if(rc == 0)
		printf("test_mob_knockback_sites %s: PASS\n", which);
	return rc;
}
