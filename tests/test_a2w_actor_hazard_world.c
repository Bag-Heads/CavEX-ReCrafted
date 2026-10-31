/* Beta Entity.isEntityInsideOpaqueBlock samples this.worldObj around the eye.
 * Two actors at identical coordinates in different committed worlds must
 * receive only their own world's suffocation hit. */
#include "support/owner_swap_harness.h"
#include "network/server_weather.h"

#define TEST_NAME "test_a2w_actor_hazard_world"
void server_local_test_apply_guest_hazards(struct server_local* s);
bool server_local_test_apply_actor_fire_hazards(struct server_local* s,
	struct server_player* player, bool in_water, bool in_lava, bool in_fire);

static void set_cell(struct server_world* w, int y, uint8_t type) {
	struct block_data actual;
	struct server_chunk* chunk = NULL;
	if(!server_world_get_block(w, 8, y, 8, &actual))
		(void)server_world_load_chunk(w, 0, 0, &chunk);
	(void)server_world_set_block(w, 8, y, 8,
		(struct block_data) {.type = type});
	check(server_world_get_block(w, 8, y, 8, &actual)
			&& actual.type == type,
		"hazard cell installed in owning world");
}

static void reset_actor(struct server_player* p) {
	p->x = 8.5;
	/* Clear the float representation edge at eye-1.62: feet cell is 59. */
	p->y = 60.64;
	p->z = 8.5;
	p->health = MAX_PLAYER_HEALTH;
	p->hurt_resist_time = 0;
	p->has_pos = true;
	p->finished_loading = true;
}

static void probe(struct server_local* s, bool host_nether,
	bool stone_in_nether) {
	struct server_player* host = &s->player;
	struct server_player* guest = s->guest_actor;
	reset_actor(host);
	reset_actor(guest);
	host->world_slot = host_nether ? 1 : 0;
	host->dimension = host_nether ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
	guest->world_slot = host_nether ? 0 : 1;
	guest->dimension = host_nether ? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER;
	set_cell(&s->worlds[0], 60, stone_in_nether ? BLOCK_AIR : BLOCK_STONE);
	set_cell(&s->worlds[1], 60, stone_in_nether ? BLOCK_STONE : BLOCK_AIR);
	check(server_local_world_of(s, host) == &s->worlds[host->world_slot]
			&& server_local_world_of(s, guest) == &s->worlds[guest->world_slot],
		"both actors resolve their committed world");
	/* The existing test hooks run production damage paths with the same
	 * eight-point eye sampler; no mock hazard predicate is used. */
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	(void)server_local_test_apply_player_hazards(s,
		false, false, false, false);
	server_local_test_apply_guest_hazards(s);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
	check((host->health < MAX_PLAYER_HEALTH) ==
			(host_nether == stone_in_nether),
		"host suffocation follows committed world");
	check((guest->health < MAX_PLAYER_HEALTH) ==
			(host_nether != stone_in_nether),
		"guest suffocation follows committed world");
}

static void probe_host_tail(struct server_local* s, bool host_nether) {
	struct server_player* host = &s->player;
	struct server_player* guest = s->guest_actor;
	struct server_world* owner = &s->worlds[host_nether ? 1 : 0];
	struct server_world* foreign = &s->worlds[host_nether ? 0 : 1];
	reset_actor(host);
	reset_actor(guest);
	host->world_slot = host_nether ? 1 : 0;
	host->dimension = host_nether ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
	guest->world_slot = host_nether ? 0 : 1;
	guest->dimension = host_nether ? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER;
	set_cell(owner, 60, BLOCK_WATER_STILL);
	set_cell(foreign, 60, BLOCK_AIR);
	set_cell(owner, 59, BLOCK_AIR);
	set_cell(foreign, 59, BLOCK_AIR);
	host->oxygen = MAX_OXYGEN;
	s->paused = false;
	tick(s);
	s->paused = true;
	check(host->oxygen == MAX_OXYGEN - 1,
		"host head-water oxygen follows committed world");
	set_cell(owner, 60, BLOCK_AIR);
	set_cell(foreign, 60, BLOCK_WATER_STILL);
	host->oxygen = MAX_OXYGEN;
	s->paused = false;
	tick(s);
	s->paused = true;
	check(host->oxygen == MAX_OXYGEN,
		"foreign-world head water does not drown host");
	set_cell(foreign, 60, BLOCK_AIR);
	set_cell(owner, 59, BLOCK_LADDER);
	host->fall_distance = 5.0f;
	s->paused = false;
	tick(s);
	s->paused = true;
	check(host->fall_distance == 0.0f,
		"host ladder contact follows committed world");
	set_cell(owner, 59, BLOCK_AIR);
	set_cell(foreign, 59, BLOCK_LADDER);
	host->fall_distance = 5.0f;
	s->paused = false;
	tick(s);
	s->paused = true;
	check(host->fall_distance == 5.0f,
		"foreign-world ladder does not reset host fall distance");
	set_cell(foreign, 59, BLOCK_AIR);
}

static void probe_wet(struct server_local* s, bool host_nether) {
	struct server_player* host = &s->player;
	struct server_player* guest = s->guest_actor;
	reset_actor(host);
	reset_actor(guest);
	host->world_slot = host_nether ? 1 : 0;
	host->dimension = host_nether ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
	guest->world_slot = host_nether ? 0 : 1;
	guest->dimension = host_nether ? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER;
	host->y = guest->y = 120.0;
	s->weather.raining = true;
	s->weather.rain_strength = s->weather.prev_rain_strength = 1.0f;
	for(unsigned i = 0; i < 2; i++) {
		bool sky, biome;
		uint8_t height;
		bool wet = server_weather_exposure_diagnostic_w(s, &s->worlds[i],
			8, 120, 8, &sky, &height, &biome);
		fprintf(stderr, "RAIN fixture: world=%u height=%u sky=%d biome_rains=%d wet=%d dimension=%d\n",
			i, height, sky, biome, wet, s->worlds[i].dimension);
	}
	check(world_can_block_be_rained_on_w(s, &s->worlds[0], 8, 120, 8)
			&& !world_can_block_be_rained_on_w(s, &s->worlds[1], 8, 120, 8),
		"fixture: Overworld rains at the actor; Nether does not");
	host->fire = guest->fire = 20;
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	(void)server_local_test_apply_actor_fire_hazards(s, host,
		false, false, false);
	(void)server_local_test_apply_actor_fire_hazards(s, guest,
		false, false, false);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
	check(host->fire == (host_nether ? 19 : -PLAYER_FIRE_RESISTANCE),
		"host rain quench follows committed world");
	check(guest->fire == (host_nether ? -PLAYER_FIRE_RESISTANCE : 19),
		"guest rain quench follows committed world");
}

int main(void) {
	harness_init();
	struct server_local* s = admitted_fixture();
	struct server_world* nether = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(nether, path, WORLD_DIM_NETHER, 12345, false);
	string_clear(path);
	nether->local = s;
	nether->session = s;
	nether->world_index = 1;
	probe(s, true, true);
	probe(s, true, false);
	probe(s, false, true);
	probe(s, false, false);
	probe_host_tail(s, true);
	probe_host_tail(s, false); /* legacy slot-0 ownership remains identical */
	probe_wet(s, true);
	probe_wet(s, false);
	s->player.world_slot = 0;
	s->player.dimension = WORLD_DIM_OVERWORLD;
	s->guest_actor->world_slot = 0;
	s->guest_actor->dimension = WORLD_DIM_OVERWORLD;
	server_world_discard(nether);
	teardown(s);
	printf("%s: %u failure(s)\n", TEST_NAME, failures);
	return failures ? 1 : 0;
}
