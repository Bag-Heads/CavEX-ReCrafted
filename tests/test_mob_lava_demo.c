/* Side-contact debug scene through the ordinary exact-position spawn and
 * production server update. The control uses the same builder at cell centre. */
#include <assert.h>
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
#include "block/blocks.h"
#include "entity/entity_monster.h"
#include "item/inventory.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static struct server_local s;
static volatile int draining = 1;

static void* drain(void* unused) {
	(void)unused;
	while(draining) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static int block_at(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.world, x, y, z, &b));
	return b.type;
}

static struct entity* mob_by_id(uint32_t id) {
	struct entity** pp = dict_entity_get(s.entities, id);
	assert(pp && *pp);
	return *pp;
}

static void check_scene(int cx, int y0, int cz) {
	int lava_count = 0;
	for(int x = cx - 1; x <= cx + 1; x++)
		for(int z = cz - 1; z <= cz + 1; z++) {
			assert(block_at(x, y0 - 1, z) == BLOCK_STONE);
			for(int y = y0; y <= y0 + 2; y++) {
				int b = block_at(x, y, z);
				lava_count += b == BLOCK_LAVA_STILL || b == BLOCK_LAVA_FLOW;
				if(x == cx + 1 && z == cz && y == y0)
					assert(b == BLOCK_LAVA_STILL);
				else if(x == cx + 1 && z == cz && y == y0 + 1)
					assert(b == BLOCK_GLASS);
				else if(x == cx && z == cz - 1 && y <= y0 + 1)
					assert(b == BLOCK_GLASS);
				else if((x == cx - 1 && z == cz)
					|| (x == cx && z == cz + 1))
					assert(b == (y <= y0 + 1 ? BLOCK_STONE : BLOCK_AIR));
				else if(x == cx + 1 && z == cz - 1)
					assert(b == (y == y0 ? BLOCK_GLASS : BLOCK_AIR));
				else if(x == cx + 1 && z == cz + 1)
					assert(b == (y == y0 ? BLOCK_STONE : BLOCK_AIR));
				else
					assert(b == BLOCK_AIR);
			}
		}
	assert(lava_count == 1);
	assert(block_at(cx, y0, cz) == BLOCK_AIR);
	assert(block_at(cx, y0 - 1, cz) == BLOCK_STONE);
	assert(block_at(cx + 2, y0, cz) == BLOCK_STONE);
	assert(block_at(cx + 2, y0 + 1, cz) == BLOCK_AIR);
}

int main(void) {
	char fixture[128];
	test_fixture_dir("test_mob_lava_demo_tmp", fixture, sizeof fixture);
	assert(clin_init());
	assert(svin_init());
	struct thread worker;
	assert(thread_create(&worker, drain, NULL, 64));
	blocks_init();
	MKDIR(fixture);
	MKDIR(test_fixture_join(fixture, "/region"));
	string_t level;
	string_init_set_str(level, fixture);
	memset(&s, 0, sizeof s);
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	/* Fixed nonzero seed (the test_block_behavior_parity precedent), not
	 * rand_gen_seed's time(NULL): the creeper's tick-1 wander (EntityLiving
	 * updatePlayerActionState rolls on s.rand_src) is what this demo ticks
	 * through, and a wall-clock seed made ~20% of runs walk the mob off the
	 * lava face before moveEntity's burning box (fire 600, not 601). The
	 * asserted scenario -- side contact at the face -- is the intended one;
	 * this seed keeps the mob on it, so the values below stay exact.
	 * [MOBLAVA-0925: 26/120 time-seeded failures at dadbffd1] */
	s.rand_src.seed = rand_test_scenario_seed(0x9E3779B9u); /* pinned; CW_TEST_SEED_FORCE replays */
	struct server_chunk* chunk = NULL;
	/* Entity updates use Beta's position +/-32 checkChunksExist gate
	 * (World.updateEntityWithOptionalForce; server_local.c's entity tick loop
	 * since the 09-22 save-recovery checkpoint). Both scenes sit in chunk
	 * (0,0)/(0,1) at z 12 and z 24, so the mobs only tick once every chunk in
	 * cx -2..2, cz -2..3 exists. */
	for(w_coord_t lcx = -2; lcx <= 2; lcx++)
		for(w_coord_t lcz = -2; lcz <= 3; lcz++)
			assert(server_world_load_chunk(&s.world, lcx, lcz, &chunk) && chunk);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
		&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.x = 8.5;
	s.player.y = 70.0;
	s.player.z = 8.5;
	s.weather.rain_strength = s.weather.prev_rain_strength = 0.0F;

	uint32_t contact_id = server_local_test_mob_lava_demo_build(&s, 0.85F);
	assert(contact_id);
	check_scene(10, 70, 12);
	assert(block_at(9, 70, 9) == BLOCK_STONE);
	assert(mob_by_id(contact_id)->health == 20);
	assert(mob_by_id(contact_id)->data.monster.fire_time == 0);
	/* Scenario prerequisites (ASTRA-SEED-POLICY-0925): Beta spawns mobs at
	 * rest (no motionX/Y/Z in the Entity/EntityLiving constructors); the
	 * shared spawn helper's random 0.05..0.25 velocity would decide the
	 * contact, so zero it and assert the geometry before and after the tick.
	 * Lava is the cell x=11 (cx+1). */
	glm_vec3_zero(mob_by_id(contact_id)->vel);
	struct AABB box;
	entity_make_world_bbox(mob_by_id(contact_id), &box);
	assert(box.x2 > 11.0F && "contact prerequisite: body overlaps lava x=11");
	server_local_test_update_once(&s);
	entity_make_world_bbox(mob_by_id(contact_id), &box);
	assert(box.x2 > 11.0F
		   && "contact prerequisite: body still overlaps lava x=11 after the tick");
	check_scene(10, 70, 12);
	assert(mob_by_id(contact_id)->health == 16);
	/* setOnFireFromLava's 600 (Entity.java:236), then moveEntity's burning
	 * box advances it to 601 (Entity.java:481-487; G6 aa3ce112). */
	assert(mob_by_id(contact_id)->data.monster.fire_time == 601);
	puts("PASS side contact: health=16 fire=601 after one server tick");

	s.player.z = 20.5;
	uint32_t control_id = server_local_test_mob_lava_demo_build(&s, 0.5F);
	assert(control_id);
	check_scene(10, 70, 24);
	assert(block_at(9, 70, 21) == BLOCK_STONE);
	assert(mob_by_id(control_id)->health == 20);
	glm_vec3_zero(mob_by_id(control_id)->vel);
	entity_make_world_bbox(mob_by_id(control_id), &box);
	assert(box.x2 <= 11.0F && "control prerequisite: body out of lava x=11");
	server_local_test_update_once(&s);
	entity_make_world_bbox(mob_by_id(control_id), &box);
	assert(box.x2 <= 11.0F
		   && "control prerequisite: body still out of lava x=11 after the tick");
	check_scene(10, 70, 24);
	assert(mob_by_id(control_id)->health == 20);
	/* G6b F2: not burning and fire <= 0 -> -fireResistance = -1
	 * (Entity.java:489-490, :52). */
	assert(mob_by_id(control_id)->data.monster.fire_time == -1);
	puts("PASS non-contact control: health=20 fire=-1 after one server tick");

	draining = 0;
	thread_join(&worker);
	string_clear(level);
	test_fixture_dir_cleanup(fixture);
	puts("test_mob_lava_demo: PASS");
	return 0;
}
