/* Beta 1.7.3 Entity.java:168-220,234-238,480-496,535-537. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <m-lib/m-string.h>
#include "support/test_fixture_dir.h"
#include "support/mob_motion_fixture.h"
#include "block/blocks.h"
#include "entity/combat.h"
#include "entity/entity_monster.h"
#include "network/client_interface.h"
#include "network/entity_archive.h"
#include "network/server_local.h"
#include "network/server_interface.h"
#include "network/server_local_state.h"
#include "network/server_weather.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "worldgen/worldgen.h"

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

bool server_local_test_apply_actor_fire_hazards(struct server_local*,
	struct server_player*, bool, bool, bool);
void server_local_test_apply_guest_hazards(struct server_local*);

static struct server_local s;
static char fixture[128];
static volatile int draining = 1;
static int failures;
static void golden(bool ok, const char* case_name, int actual, int expected) {
	if(!ok) {
		fprintf(stderr,"RED %s: fire=%d expected=%d\n",case_name,actual,expected);
		failures++;
	} else printf("PASS %s: fire=%d\n",case_name,actual);
}
static void* drain(void* unused) {
	(void)unused;
	while(draining) { clin_drain_discard(); thread_msleep(1); }
	clin_drain_discard();
	return NULL;
}
static void put(int x, int y, int z, int block) {
	if(server_world_set_block(&s.worlds[0], x, y, z,
		(struct block_data){.type = (uint8_t)block})) return;
	struct block_data current;
	assert(server_world_get_block(&s.world,x,y,z,&current));
	assert(current.type == block);
}
static void resident(int x, int z) {
	struct server_chunk* c = NULL;
	assert(server_world_load_chunk(&s.world, x >> 4, z >> 4, &c) && c);
}
static void find_biome(enum biome wanted, int* x, int* z) {
	for(int a = -1024; a < 1024; a += 8)
	for(int b = -1024; b < 1024; b += 8) {
		enum biome found = worldgen_biome_at(s.world.generator, a, b);
		bool rain_biome = found != BIOME_TAIGA && found != BIOME_ICE_DESERT
			&& found != BIOME_TUNDRA && found != BIOME_DESERT;
		if((wanted == BIOME_PLAINS && rain_biome) || found == wanted) {
			*x = a; *z = b; resident(a,b); return;
		}
	}
	assert(!"fixture seed must have requested biome");
}
static void actor_at(struct server_player* p, int x, int z) {
	p->x = x + 0.5; p->y = 72.0; p->z = z + 0.5;
	p->health = MAX_PLAYER_HEALTH;
	p->hurt_resist_time = p->last_damage = p->damage_remainder = 0;
	p->has_pos = p->finished_loading = true;
	put(x, 70, z, BLOCK_AIR);
	put(x, 71, z, BLOCK_AIR);
	put(x, 73, z, BLOCK_AIR);
}
static void assert_host_weather_diagnostic(int x, int z, bool wet,
	bool rained_on) {
	struct server_local_host_weather_diagnostic diag;
	uint8_t height = 0;
	assert(server_local_host_weather_diagnostic_read(&diag));
	assert(server_world_get_height(&s.world, x, z, &height));
	assert(diag.wet == wet);
	assert(diag.rained_on == rained_on);
	assert(diag.height == height);
	assert(diag.biome_rains);
	assert(diag.fx == x && diag.fy == 70 && diag.fz == z);
	assert(diag.fire_ticks == s.player.fire);
	assert(diag.wet == server_entity_is_wet(&s, false, s.player.x,
		s.player.y - PLAYER_EYE_HEIGHT, s.player.z));
	assert(diag.rained_on == world_can_block_be_rained_on(
		&s, diag.fx, diag.fy, diag.fz));
}
static void diagnostic_request_cases(int x, int z) {
	struct server_local_host_weather_diagnostic diag;
	uint32_t seq = server_local_test_host_weather_diagnostic_sequence();
	uint32_t queries
		= server_local_test_host_weather_diagnostic_exposure_queries();
	assert(!server_local_host_weather_diagnostic_read(&diag));
	assert(server_local_test_apply_player_hazards(
		&s, false, false, false, false));
	assert(server_local_test_host_weather_diagnostic_sequence() == seq
		&& "no request must not advance the publish sequence");
	assert(server_local_test_host_weather_diagnostic_exposure_queries() == queries
		&& "no request must issue no diagnostic exposure query");
	assert(!server_local_host_weather_diagnostic_read(&diag)
		&& "no consumer request must not publish a weather snapshot");

	actor_at(&s.player, x, z);
	server_local_host_weather_diagnostic_request();
	assert(server_local_test_apply_player_hazards(
		&s, false, false, false, false));
	assert(server_local_test_host_weather_diagnostic_sequence() != seq
		&& "a fresh request must advance the publish sequence");
	assert(server_local_test_host_weather_diagnostic_exposure_queries()
		== queries + 1);
	assert_host_weather_diagnostic(x, z, true, true);

	/* D085-SEQLOCK-FIX-01: a writer preempted mid-publish leaves the sequence
	 * odd. The reader (main, higher priority, single core) must give up and
	 * report "not available" instead of waiting forever. */
	{
		uint32_t even = server_local_test_host_weather_diagnostic_sequence();
		assert(!(even & 1u));
		assert(server_local_test_host_weather_diagnostic_sequence_exchange(
			even + 1u) == even);
		fputs("rain_quench: reading with writer mid-publish\n", stderr);
		fflush(stderr);
		for(int i = 0; i < 4; i++) {
			memset(&diag, 0x5a, sizeof diag);
			assert(!server_local_host_weather_diagnostic_read(&diag)
				&& "odd sequence must not block or report a snapshot");
			assert(!diag.wet && diag.fire_ticks == 0 && !diag.rained_on
				&& diag.height == 0 && !diag.biome_rains && !diag.sky
				&& diag.fx == 0 && diag.fy == 0 && diag.fz == 0);
		}
		assert(server_local_test_host_weather_diagnostic_sequence_exchange(
			even) == even + 1u);
		assert_host_weather_diagnostic(x, z, true, true);
	}
}
static void player_cases(struct server_player* p, bool guest,
	int rain_x, int rain_z, int desert_x, int desert_z) {
	if(!guest) {
		/* C53 item 13: a leaf canopy shelters the floored feet cell. Fire
		 * contact advances the negative resistance counter only while dry;
		 * removing the canopy makes the same contact wet and leaves it at -20. */
		actor_at(p, rain_x, rain_z);
		put(rain_x, 73, rain_z, BLOCK_LEAVES);
		p->fire = -PLAYER_FIRE_RESISTANCE;
		server_local_host_weather_diagnostic_request();
		assert(server_local_test_apply_player_hazards(
			&s, false, false, true, false));
		golden(p->fire == -PLAYER_FIRE_RESISTANCE + 1,
			"host leaf canopy fire", p->fire, -PLAYER_FIRE_RESISTANCE + 1);
		assert_host_weather_diagnostic(rain_x, rain_z, false, false);

		put(rain_x, 73, rain_z, BLOCK_AIR);
		p->fire = -PLAYER_FIRE_RESISTANCE;
		server_local_host_weather_diagnostic_request();
		assert(server_local_test_apply_player_hazards(
			&s, false, false, true, false));
		golden(p->fire == -PLAYER_FIRE_RESISTANCE,
			"host open rain fire", p->fire, -PLAYER_FIRE_RESISTANCE);
		assert_host_weather_diagnostic(rain_x, rain_z, true, true);
	}

	actor_at(p, rain_x, rain_z);
	p->fire = 20;
	assert(world_can_block_be_rained_on(&s, rain_x, 70, rain_z));
	if(guest) server_local_test_apply_guest_hazards(&s);
	else assert(server_local_test_apply_actor_fire_hazards(&s,p,false,false,false));
	/* Entity.onEntityUpdate fire 20 -> 19 (one damage); moveEntity.isWet -> -20. */
	golden(p->fire == -PLAYER_FIRE_RESISTANCE,
		guest ? "guest open rain" : "host open rain",p->fire,-PLAYER_FIRE_RESISTANCE);

	actor_at(p, rain_x, rain_z); put(rain_x,73,rain_z,BLOCK_STONE);
	p->fire = 20;
	assert(!world_can_block_be_rained_on(&s,rain_x,70,rain_z));
	if(guest) server_local_test_apply_guest_hazards(&s);
	else assert(server_local_test_apply_actor_fire_hazards(&s,p,false,false,false));
	/* Entity.java:203-215, no wet quench. */
	golden(p->fire == 19,guest ? "guest roof" : "host roof",p->fire,19);
	put(rain_x,73,rain_z,BLOCK_AIR);

	actor_at(p, desert_x, desert_z); p->fire = 20;
	assert(!world_can_block_be_rained_on(&s,desert_x,70,desert_z));
	if(guest) server_local_test_apply_guest_hazards(&s);
	else assert(server_local_test_apply_actor_fire_hazards(&s,p,false,false,false));
	/* World.java:2415-2429 desert rain gate. */
	golden(p->fire == 19,guest ? "guest desert" : "host desert",p->fire,19);

	actor_at(p, rain_x, rain_z); p->fire = 0;
	short before = p->health;
	if(guest) {
		p->x = rain_x + 0.85;
		put(rain_x+1,70,rain_z,BLOCK_LAVA_STILL);
		server_local_test_apply_guest_hazards(&s);
	} else assert(server_local_test_apply_actor_fire_hazards(&s,p,false,true,false));
	/* Entity.java:234-238 sets 600 after old-fire handling, :493-496 clears it. */
	assert(p->health == before - 4*(HEALTH_PER_HEART/2));
	golden(p->fire == -PLAYER_FIRE_RESISTANCE,
		guest ? "guest rain lava" : "host rain lava",p->fire,-PLAYER_FIRE_RESISTANCE);
	put(rain_x+1,70,rain_z,BLOCK_AIR);

	actor_at(p,rain_x,rain_z); p->fire = -PLAYER_FIRE_RESISTANCE;
	before = p->health;
	assert(server_local_test_apply_actor_fire_hazards(&s,p,false,false,true));
	/* Entity.java:480-496: fire contact damages, wetness suppresses ignition. */
	assert(p->health == before - HEALTH_PER_HEART/2);
	golden(p->fire == -PLAYER_FIRE_RESISTANCE,
		guest ? "guest rain fire" : "host rain fire",p->fire,-PLAYER_FIRE_RESISTANCE);

	actor_at(p,rain_x,rain_z); p->fire = 20;
	assert(server_local_test_apply_actor_fire_hazards(&s,p,true,false,false));
	/* Entity.java:168-196 water clears early; moveEntity's box is not burning
	 * and fire <= 0, so :489-490 resets to -fireResistance (EntityPlayer:50). */
	golden(p->fire == -PLAYER_FIRE_RESISTANCE,guest ? "guest water" : "host water",
		p->fire,-PLAYER_FIRE_RESISTANCE);

	/* G6b F5: isWet (Entity.java:535-537) samples canBlockBeRainedOn at
	 * floor(posY), and a player's posY is the EYE (yOffset 1.62,
	 * EntityPlayer.java:44). Leaves in the floored FEET cell (70) put the
	 * precipitation height at 71: the feet cell is sheltered, the eye cell
	 * (actor_at puts the eye at 72.0) is rained on. Beta is wet:
	 * fire contact deals its 1 but the ignition ramp is suppressed (:483-487). */
	actor_at(p,rain_x,rain_z);
	put(rain_x,70,rain_z,BLOCK_LEAVES);
	assert(!world_can_block_be_rained_on(&s,rain_x,70,rain_z));
	assert(world_can_block_be_rained_on(&s,rain_x,72,rain_z));
	p->fire = -PLAYER_FIRE_RESISTANCE;
	assert(server_local_test_apply_actor_fire_hazards(&s,p,false,false,true));
	golden(p->fire == -PLAYER_FIRE_RESISTANCE,
		guest ? "guest eye-sampled wet" : "host eye-sampled wet",p->fire,
		-PLAYER_FIRE_RESISTANCE);
	put(rain_x,70,rain_z,BLOCK_AIR);
}
static struct entity* mob_at(int x, int z, float offset) {
	vec3 pos = {x+offset,70.05f,z+0.5f};
	struct entity* m = server_local_spawn_monster(pos, MONSTER_CREEPER, &s);
	assert(m && m->tick_server);
	m->pos[0] = x + offset;
	test_mob_sync(m); /* motion units S2: the fixture pins the doubles */
	return m;
}
static void mob_cases(int x, int z, int desert_x, int desert_z) {
	/* EntityLiving.onUpdate -> onLivingUpdate -> moveEntity: Entity.java:480-496
	 * samples isWet after the axis moves. Roof at the starting foot column,
	 * open rain at the destination: the FIRST tick must quench to -1. */
	put(x,73,z,BLOCK_STONE);
	struct entity* crossing = mob_at(x,z,0.85f);
	crossing->data.monster.fire_time = 20;
	crossing->data.monster.knockback_time = 2;
	crossing->vel[0] = 0.25f;
	test_mob_sync(crossing);
	assert(!crossing->tick_server(crossing,&s));
	assert(crossing->pos[0] >= x+1.0f);
	golden(crossing->data.monster.fire_time == -1,"mob roof-to-rain tick 1",
		crossing->data.monster.fire_time,-1);
	put(x,73,z,BLOCK_AIR);

	struct entity* m = mob_at(x,z,0.5f);
	m->data.monster.fire_time = 20;
	assert(!m->tick_server(m,&s));
	/* EntityLiving.java:105-107,203-205 inherits Entity.moveEntity wet quench. */
	golden(m->data.monster.fire_time == -1,"mob open rain",
		m->data.monster.fire_time,-1);
	assert(m->health == 19); /* Entity.java:210-214: old fire 20 deals 1. */
	/* Snapshot the actually quenched mob through the production copier and
	 * archive codec. Beta Entity.java:755,786 stores signed Fire as a short. */
	struct archived_entity record = {0};
	server_local_test_archive_entity(m,&record);
	struct chunk_entities input = {.entities = &record, .count = 1};
	void* arena = NULL;
	struct nbt_list* encoded = chunk_entities_build(&input,&arena);
	assert(encoded && arena);
	nbt_node list = {.type = TAG_LIST, .name = "Entities",
		.payload.tag_list = encoded};
	struct chunk_entities restored = {0};
	assert(chunk_entities_parse(&list,x >> 4,z >> 4,&restored));
	assert(restored.count == 1);
	golden(restored.entities[0].data.monster.fire_time == -1,
		"mob quenched archive round trip",
		restored.entities[0].data.monster.fire_time,-1);
	chunk_entities_free(&restored);
	chunk_entities_build_free(arena);

	put(x,73,z,BLOCK_STONE);
	m = mob_at(x,z,0.5f); m->data.monster.fire_time = 20;
	assert(!m->tick_server(m,&s));
	golden(m->data.monster.fire_time == 19,"mob roof",m->data.monster.fire_time,19);
	put(x,73,z,BLOCK_AIR);

	m = mob_at(desert_x,desert_z,0.5f); m->data.monster.fire_time = 20;
	assert(!m->tick_server(m,&s));
	golden(m->data.monster.fire_time == 19,"mob desert",m->data.monster.fire_time,19);

	m = mob_at(x,z,0.85f);
	m->data.monster.knockback_time = 2;
	m->vel[0] = 0.25f;
	test_mob_sync(m);
	put(x,70,z,BLOCK_LAVA_STILL);
	assert(world_can_block_be_rained_on(&s,x+1,70,z));
	assert(!m->tick_server(m,&s));
	assert(m->pos[0] >= x+1.0f);
	/* Entity.java:234-238 lava inflicts 4 and assigns 600 once the
	 * contracted body reaches the lava cell; :493-496 extinguishes after this tick's move into rain. */
	assert(m->health == 16);
	golden(m->data.monster.fire_time == -1,"mob rain lava",
		m->data.monster.fire_time,-1);
	put(x,70,z,BLOCK_AIR);

	m = mob_at(x,z,0.5f); m->data.monster.fire_time = 20;
	put(x,70,z,BLOCK_WATER_STILL);
	assert(!m->tick_server(m,&s));
	/* Entity.java:195 water zeroes fire, then :489-490 (not burning,
	 * fire <= 0) rests it at -fireResistance = -1 (Entity.java:52). */
	golden(m->data.monster.fire_time == -1,"mob water",
		m->data.monster.fire_time,-1);
	put(x,70,z,BLOCK_AIR);

	/* Entity.handleLavaMovement scans boundingBox.expand(-0.1F,-0.4F,-0.1F)
	 * via World.isMaterialInBB (Entity.java:566, World.java:1458): the dry
	 * mob's contracted body [x+0.65,x+1.05] reaches the side lava cell, so
	 * setOnFireFromLava applies 4 damage and fire = 600 (push-0921/C64). */
	s.weather.rain_strength = s.weather.prev_rain_strength = 0.0f;
	m = mob_at(x,z,0.85f);
	put(x+1,70,z,BLOCK_LAVA_STILL);
	assert(!m->tick_server(m,&s));
	golden(m->data.monster.fire_time == 601,"mob off-foot lava AABB",
		m->data.monster.fire_time,601);
	assert(m->health == 16);
	put(x+1,70,z,BLOCK_AIR);
}
int main(void) {
	test_fixture_dir("test_rain_quench_tmp",fixture,sizeof fixture);
	assert(clin_init());
	struct thread worker; assert(thread_create(&worker,drain,NULL,64));
	blocks_init();
	MKDIR(fixture); MKDIR(test_fixture_join(fixture,"/region"));
	string_t level; string_init_set_str(level,fixture);
	memset(&s,0,sizeof s); dict_entity_init(s.entities);
	server_world_create(&s.world,level,WORLD_DIM_OVERWORLD,424242,true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s; s.rand_src.seed = 0x51DEB001u;
	int rx,rz,dx,dz;
	find_biome(BIOME_PLAINS,&rx,&rz);
	find_biome(BIOME_DESERT,&dx,&dz);
	s.weather.rain_strength = s.weather.prev_rain_strength = 1.0f;
	diagnostic_request_cases(rx,rz);
	player_cases(&s.player,false,rx,rz,dx,dz);
	struct server_player guest = {0};
	s.guest_actor = &guest; s.guest_gameplay = true;
	player_cases(&guest,true,rx,rz,dx,dz);
	mob_cases(rx,rz,dx,dz);
	assert(failures == 0);
	draining = 0; thread_join(&worker);
	test_fixture_dir_cleanup(fixture);
	puts("test_rain_quench_golden: PASS");
	return 0;
}
