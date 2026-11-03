/* Diagnostic guard, not Beta gameplay. Beta client EntityLiving.java:440-445
 * and server EntityLiving.java:423: ceil(fallDistance-3); health=20, scaled x8. */
#include "support/split_residency_fixture.h"
#include "platform/hw_telemetry.h"
#include "platform/service_observer.h"
#include "entity/monsters_object.h"
#include "entity/armor.h"
#include "item/items.h"
#include "entity/entity_primed_tnt.h"
#include <stdio.h>
#include <inttypes.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
static uint64_t observer_cpu_cycles(void) {
	ULONG64 cycles; assert(QueryThreadCycleTime(GetCurrentThread(), &cycles)); return cycles;
}
static uint64_t observer_cpu_us(void) {
	FILETIME created, exited, kernel, user;
	assert(GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user));
	return ((((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime)
		+ (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime)) / 10;
}
#else
#include <time.h>
static uint64_t observer_cpu_cycles(void) { return 0; } /* Unavailable, not estimated. */
static uint64_t observer_cpu_us(void) {
	struct timespec t; assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0);
	return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
#endif

static uint64_t digest = UINT64_C(14695981039346656037);
static unsigned explosions;
static FILE* trace;
static FILE* trace_map;
static uint64_t trace_offset;
static void trace_begin(const char* suffix) {
	const char* prefix = getenv("CW_G9_OFF_TRACE");
	if(!prefix || !prefix[0]) return;
	char path[512];
	int n = snprintf(path, sizeof(path), "%s.%s.bin", prefix, suffix);
	assert(n > 0 && (size_t)n < sizeof(path));
	trace = fopen(path, "wb"); assert(trace);
	n = snprintf(path, sizeof(path), "%s.%s.map", prefix, suffix);
	assert(n > 0 && (size_t)n < sizeof(path));
	trace_map = fopen(path, "w"); assert(trace_map); trace_offset = 0;
}
static void trace_end(void) {
	if(trace) { assert(fclose(trace) == 0); trace = NULL; }
	if(trace_map) { assert(fclose(trace_map) == 0); trace_map = NULL; }
}
static void fold_at(const void* p, size_t n, const char* field, unsigned line) {
	if(trace_map) { fprintf(trace_map, "%" PRIu64 " %zu %u %s\n", trace_offset, n, line, field); trace_offset += n; }
	if(trace) assert(fwrite(p, 1, n, trace) == n);
	const unsigned char* b = p;
	while(n--) { digest ^= *b++; digest *= UINT64_C(1099511628211); }
}
#define fold(p, n) fold_at((p), (n), #p, __LINE__)
static void observe(const struct client_rpc* r, void* unused) {
	(void)unused;
	fold(&r->type, sizeof(r->type));
	if(r->type == CRPC_PLAYER_SET_HEALTH)
		fold(&r->payload.player_set_health.health, sizeof(short));
	if(r->type == CRPC_SOUND) {
		fold(&r->payload.sound.sound_id, sizeof(r->payload.sound.sound_id));
		fold(r->payload.sound.pos, sizeof(vec3));
		fold(&r->payload.sound.vol, sizeof(float)); fold(&r->payload.sound.pitch, sizeof(float));
	}
	if(r->type == CRPC_LOCAL_PLAYER_VELOCITY)
		fold(&r->payload.local_player_velocity.vel, sizeof(vec3));
	if(r->type == CRPC_ENTITY_EXPLODE) {
		explosions++;
		fold(r->payload.entity_explode.pos, sizeof(vec3));
		fold(&r->payload.entity_explode.power, sizeof(float));
	}
	clin_drain_discard();
}
static struct server_local* fixture(void) {
	struct server_local* s = split_fixture();
	s->player.finished_loading = true;
	s->player.active_inventory = &s->player.inventory;
	s->perf_stress = true;
	s->rand_src.seed = 97531;
	/* F4-RNG: match body hurt/death pitch and blast/drop inputs across runs. */
	server_local_test_seed_body_rand(s, &s->player, 13579);
	jrand_set_seed(&s->worlds[0].rand, 4242);
	*server_local_sound_rng() = (struct random_gen){.seed = 13579};
	split_populate(s, 0, 0);
	clin_drain_discard();
	return s;
}
static void destroy(struct server_local* s) {
	server_local_shutdown(s);
	split_destroy(s);
}
static void arm(struct server_local* s) {
#ifdef SERVER_LOCAL_PERF_GUARD_VERSION
	server_local_perf_guard_configure(s, true, true, split_path);
	s->perf_guard_armed = true; /* The admitted stress fixture, without spawning mobs. */
	string_set_str(s->level_name, split_path);
	server_local_perf_guard_before_damage(s);
#else
	(void)s; /* RED: base has no survival fixture. */
#endif
}
static void off_equivalence(void) {
	trace_begin("damage");
	struct server_local* s = fixture();
	clin_test_set_rpc_observer(observe, NULL);
	for(unsigned i = 0; i < 64; i++) {
		s->player.hurt_resist_time = 0;
		assert(server_local_set_player_health(s, MAX_PLAYER_HEALTH));
		bool accepted = false;
		assert(server_local_damage_player_result(s, 8 + (i % 12)*8, 1, 0, &accepted));
		assert(accepted);
		fold(&s->player.health, sizeof(s->player.health));
		fold(&s->player.hurt_resist_time, sizeof(s->player.hurt_resist_time));
		fold(&s->player.last_damage, sizeof(s->player.last_damage));
		fold(&s->player.damage_remainder, sizeof(s->player.damage_remainder));
		fold(&s->rand_src.seed, sizeof(s->rand_src.seed));
		fold(&server_local_sound_rng()->seed, sizeof(server_local_sound_rng()->seed));
	}
	clin_test_set_rpc_observer(NULL, NULL);
	printf("OFF surviving stress damage/RPC/RNG digest=%016" PRIx64 "\n", digest);
	fflush(stdout);
	if(trace) { assert(fflush(trace) == 0); assert(fflush(trace_map) == 0); }
	/* FAST re-pin (maintainer 2026-10-11): pre-V6 pin 0xa71f85d82b717da8
	 * (P=c5cc9d5ee); first divergence = F3 owned sound/F4 body pitch draws. */
	assert(digest == UINT64_C(0xdd045370c48d789b));
	destroy(s);
	trace_end();
}
static int key_compare(const void* a, const void* b) {
	int64_t x = *(const int64_t*)a, y = *(const int64_t*)b;
	return x < y ? -1 : x > y;
}
static void stress_off_ticks(void) {
	trace_begin("stress");
	struct server_local* s = fixture();
	digest = UINT64_C(14695981039346656037); explosions = 0;
	clin_test_set_rpc_observer(observe, NULL);
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_expire_near_budget(true);
	server_local_test_set_near_send_minimum(4);
	server_local_test_seed_math_random(1357);
	s->far_gen_budget_ms = INT_MAX;
	s->far_view_distance = MAX_VIEW_DISTANCE;
	s->container_drop_rng.seed = 1111; s->dispenser_break_rng.seed = 2222;
	s->entity_rng.seed = 3333;
	for(int x = -3; x <= 3; x++) for(int z = -3; z <= 3; z++) split_populate(s, x, z);
	s->player.x = s->player.z = 8.5; s->player.y = 4 + PLAYER_EYE_HEIGHT;
	server_local_refresh_player_membership(s);
	s->world_time = 18001;
	int cap = server_local_test_mob_spawn_cap();
	for(int i = 0; i < cap; i++) {
		float a = i * (2.0F * (float)M_PI / cap);
		int id = i % 3 == 0 ? MONSTER_ZOMBIE : i % 3 == 1 ? MONSTER_SPIDER : MONSTER_SKELETON;
		assert(server_local_spawn_monster((vec3){8.5F + 3*cosf(a), 4, 8.5F + 3*sinf(a)}, id, s));
	}
	/* Matched SURVIVING fixture on both trees: full health at each boundary.
	 * The diagnostic guard is OFF. Actual stress ring, bursts, world ticks,
		 * RPC order, ALL resident terrain/light arrays and gameplay RNG participate. */
	const char* mode = getenv("CW_SO_MODE");
	if(mode) service_observer_configure(!strcmp(mode, "on"), "pc-surviving-stress", 1);
	uint64_t wall_us = 0, cpu_us = 0, cpu_cycles = 0;
	for(unsigned step = 0; step < 32; step++) {
		uint64_t cycle_start = mode ? observer_cpu_cycles() : 0;
		uint64_t cpu_start = mode ? observer_cpu_us() : 0;
		uint64_t wall_start = mode ? service_observer_now() : 0;
		/* The fixture calls update_once, so forward its real tick boundary.
		 * No fake controller/audio service is included in this cost claim. */
		if(mode) { SERVICE_OBSERVER(service_observer_frame(SO_GAMEPLAY)); SERVICE_OBSERVER(service_observer_tick(true)); }
		assert(server_local_set_player_health(s, MAX_PLAYER_HEALTH));
		server_local_test_update_once(s);
		if(mode) {
			SERVICE_OBSERVER(service_observer_tick(false));
			wall_us += service_observer_now() - wall_start;
			cpu_us += observer_cpu_us() - cpu_start;
			cpu_cycles += observer_cpu_cycles() - cycle_start;
		}
		assert(s->player.health > 0 && s->player_deaths == 0);
		fold(&s->player.health, sizeof(s->player.health));
		fold(&s->player.hurt_resist_time, sizeof(s->player.hurt_resist_time));
		fold(&s->player.last_damage, sizeof(s->player.last_damage));
		fold(&s->world_time, sizeof(s->world_time));
		fold(&s->worlds[0].current_tick, sizeof(s->worlds[0].current_tick));
		fold(&s->rand_src.seed, sizeof(s->rand_src.seed));
		fold(&s->entity_rng.seed, sizeof(s->entity_rng.seed));
		dict_entity_it_t it; dict_entity_it(it, s->entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			fold(&e->id, sizeof(e->id)); fold(&e->type, sizeof(e->type));
			/* Arrow factories do not initialize health: it is not arrow state.
			 * Hash every living mob health, never indeterminate unused storage. */
			if(e->type == ENTITY_MONSTER) fold(&e->health, sizeof(e->health));
			fold(e->pos, sizeof(vec3));
			fold(e->vel, sizeof(vec3)); fold(&e->gameplay_rng.seed, sizeof(uint64_t));
			fold(&e->gameplay_rng.have_next_gaussian, sizeof(bool));
			if(e->gameplay_rng.have_next_gaussian)
				fold(&e->gameplay_rng.next_gaussian_cached, sizeof(double));
			dict_entity_next(it);
		}
		size_t count = dict_server_chunks_size(s->worlds[0].chunks), index = 0;
		int64_t* keys = malloc((count ? count : 1) * sizeof(*keys)); assert(keys);
		dict_server_chunks_it_t ci; dict_server_chunks_it(ci, s->worlds[0].chunks);
		while(!dict_server_chunks_end_p(ci)) {
			assert(index < count); keys[index++] = dict_server_chunks_ref(ci)->key;
			dict_server_chunks_next(ci);
		}
		assert(index == count); qsort(keys, count, sizeof(*keys), key_compare);
		fold(&count, sizeof(count));
		for(size_t k = 0; k < count; k++) {
			struct server_chunk* c = dict_server_chunks_get(s->worlds[0].chunks, keys[k]); assert(c);
			fold(&keys[k], sizeof(*keys));
			const uint8_t* arrays[] = {c->ids, c->metadata, c->lighting_sky, c->lighting_torch, c->heightmap};
			const size_t lengths[] = {SERVER_CHUNK_IDS_BYTES, SERVER_CHUNK_NIBBLE_BYTES,
				SERVER_CHUNK_NIBBLE_BYTES, SERVER_CHUNK_NIBBLE_BYTES, SERVER_CHUNK_HEIGHTMAP_BYTES};
			for(unsigned a = 0; a < 5; a++) {
				bool present = arrays[a] != NULL; fold(&present, sizeof(present));
				if(present) fold(arrays[a], lengths[a]);
			}
			fold(&c->populated, sizeof(bool)); fold(&c->sent, sizeof(bool));
		}
		free(keys);
	}
	assert(explosions >= 3 && s->worlds[0].current_tick == 32);
	if(trace) { assert(fflush(trace) == 0); assert(fflush(trace_map) == 0); }
	/* FAST re-pin (maintainer 2026-10-11): pre-V6 pin 0xa306aade20818bf9
	 * (P=c5cc9d5ee); first divergence tick2/entity7 entity_monster.c swim roll (F4). */
	/* G9X-6 re-record (REGRESS-1015, maintainer re-record ruling 2026-10-14): old pin 0xe1ce5576ddadb9a6. With the G9X-6 post-drain refresh disabled the old pin reproduces exactly; with it the fold stream differs ONLY by 11 inserted CRPC_CHUNK republishes of already-sent columns, each carrying a stored Sky/Block light change (Beta publishes every light store, CFR World.java:512-517). tmp/jobs/loop/REGRESS-1015/RERECORD.md. */
	/* ci-1101/47 FAST re-pin: c16ebd6bd corrects Beta's skeleton arrow lift
	 * (EntitySkeleton.attackEntity:40, +1.4F; launch DY2.43->2.83). Reverting
	 * ONLY that launch height at HEAD dee9d3edf reproduces old04c925c187fcfb46.
	 * First difference: tick1/arrow11 position, fold byte285; post-flight Y
	 * 6.290963173->6.547310829. HEAD and combined-tree fold streams are exact.
	 * C:/tmp/cavex-jobs/ci-1101/47/PROOF.md and stress-first.txt. */
	assert(digest == UINT64_C(0xba1ba693ff721f0e));
	printf("OFF actual32 stress ticks/ring%d/bursts%u/ALL-column arrays/entities/RPC/RNG digest=%016" PRIx64 "\n", cap, explosions, digest);
	if(mode) printf("PC matched surviving actual32 stress: observer=%s active_wall_us=%" PRIu64 " thread_cpu_us=%" PRIu64 " thread_cpu_cycles=%" PRIu64 " (unsubtracted; fold/IO outside timing; CPU-us quantized)\n", mode, wall_us, cpu_us, cpu_cycles);
	clin_test_set_rpc_observer(NULL, NULL);
	destroy(s);
	trace_end();
}
static void sustained_stress_ticks(void) {
	struct server_local* s = fixture(); arm(s);
	server_local_test_seed_math_random(1357);
	s->entity_rng.seed = 3333;
	s->far_gen_budget_ms = INT_MAX; s->far_view_distance = MAX_VIEW_DISTANCE;
	for(int x = -3; x <= 3; x++) for(int z = -3; z <= 3; z++) split_populate(s, x, z);
	s->player.x = s->player.z = 8.5; s->player.y = 4 + PLAYER_EYE_HEIGHT;
	server_local_refresh_player_membership(s); s->world_time = 18001;
	int cap = server_local_test_mob_spawn_cap();
	for(int i = 0; i < cap; i++) {
		float a = i * (2.0F * (float)M_PI / cap);
		int id = i % 3 == 0 ? MONSTER_ZOMBIE : i % 3 == 1 ? MONSTER_SPIDER : MONSTER_SKELETON;
		assert(server_local_spawn_monster((vec3){8.5F + 3*cosf(a), 4, 8.5F + 3*sinf(a)}, id, s));
	}
	explosions = 0; clin_test_set_rpc_observer(observe, NULL);
	for(unsigned step = 0; step < 32; step++) {
		server_local_test_update_once(s); /* No test-side healing between ticks. */
		assert(s->player.health > 0 && s->player_deaths == 0);
	}
	uint32_t deaths[5]; assert(server_local_death_snapshot(deaths));
	assert(explosions >= 3 && s->perf_guard_words[SG_EVENTS] > 0
		&& s->perf_guard_words[SG_REFILLS] > 0 && s->perf_guard_words[SG_MIN_HEALTH] > 0
		&& deaths[SLD_HOST_DEATHS] == 0 && deaths[SLD_GUEST_DEATHS] == 0);
	clin_test_set_rpc_observer(NULL, NULL); destroy(s);
	puts("PASS guarded actual32 stress ticks/ring10/bursts3, actual refills/hits/minimum, no test healing, zero committed deaths");
}
static void landing(bool protected, unsigned blocks) {
	struct server_local* s = fixture();
	if(protected) arm(s);
	s->player.fall_distance = (float)blocks;
	s->player.on_ground = false;
	s->player.hurt_resist_time = 0;
	bool survived = server_local_player_vertical_sample_for(s, &s->player,
		s->player.y, 0, true);
	clin_drain_discard();
	if(protected) {
		fprintf(stderr, "protected %u-block landing: survived=%d health=%d\n",
			blocks, survived, s->player.health);
		assert(survived && s->player.health == 1);
		assert(s->perf_guard_words[SG_PREVENTED] == 1);
		assert(s->perf_guard_words[SG_MIN_HEALTH] == 1);
		const uint32_t* e = s->perf_guard_words + 10;
		assert(e[3] == PLAYER_DAMAGE_FALL && e[4] == 160);
		assert(e[5] == (blocks-3)*8 && e[6] == e[5] && e[7] == 159);
	} else assert(!survived && s->player.health <= 0 && s->player_deaths == 1);
	destroy(s);
}
static unsigned sounds, velocities, health_rows;
static float last_velocity_y; /* V6: knockback travels in the tracker packet */
static short health_values[16];
static void probe(const struct client_rpc* r, void* unused) {
	(void)unused;
	if(r->type == CRPC_SOUND) sounds++;
	if(r->type == CRPC_LOCAL_PLAYER_VELOCITY) {
		velocities++;
		last_velocity_y = r->payload.local_player_velocity.vel[1];
	}
	if(r->type == CRPC_PLAYER_SET_HEALTH) {
		if(health_rows < 16) health_values[health_rows] = r->payload.player_set_health.health;
		health_rows++;
	}
	clin_drain_discard();
}
static void hits(void) {
	struct server_local* s = fixture(); arm(s);
	clin_test_set_rpc_observer(probe, NULL);
	sounds = velocities = health_rows = 0;
	s->player.health = 1;
	s->player.hurt_resist_time = 0;
	bool accepted = false;
	assert(server_local_damage_player_result(s, 16, 1, 0, &accepted) && accepted);
	/* V6 F2a: the hit's velocity is published by the tracker phase
	 * (EntityTrackerEntry velocityChanged), not inside the damage call. */
	server_local_flush_actor_velocity(s);
	assert(s->player.health == 1 && sounds == 1 && velocities == 1);
	assert(s->perf_guard_words[SG_EVENTS] == 1 && s->perf_guard_words[SG_PREVENTED] == 1);
	uint32_t sound_seed = server_local_sound_rng()->seed;
	assert(server_local_damage_player_result(s, 16, 1, 0, &accepted) && !accepted);
	assert(s->perf_guard_words[SG_EVENTS] == 1 && server_local_sound_rng()->seed == sound_seed);
	assert(server_local_damage_player_result(s, 40, 1, 0, &accepted) && accepted);
	server_local_flush_actor_velocity(s); /* tracker phase */
	assert(sounds == 1 && velocities == 1 && s->player.last_damage == 40);
	assert(s->perf_guard_words[SG_EVENTS] == 2);
	assert(s->perf_guard_words[10+9+6] == 24 && s->perf_guard_words[10+9+7] == 0);
	/* The melee caller's local new_health must also retain knockback. */
	struct entity spider = {.type = ENTITY_MONSTER, .pos = {2, 4, 0}};
	spider.data.monster.id = MONSTER_SPIDER;
	struct entity zombie = spider; zombie.data.monster.id = MONSTER_ZOMBIE;
	s->player.hurt_resist_time = 0;
	s->player.vel_x = s->player.vel_y = s->player.vel_z = 0;
	server_local_hurt_player(s, &spider);
	/* V6 F2a: the knockback motion is client-integrated -- the tracker
	 * phase publishes it in the velocity packet, so check the packet. */
	last_velocity_y = 0;
	server_local_flush_actor_velocity(s); /* tracker phase */
	assert(s->player.health == 1 && last_velocity_y > 0 && velocities == 2 && sounds == 2);
	uint32_t count = s->perf_guard_words[SG_EVENTS];
	server_local_hurt_player(s, &spider);
	assert(s->perf_guard_words[SG_EVENTS] == count);
	server_local_hurt_player(s, &zombie);
	server_local_flush_actor_velocity(s); /* tracker phase */
	assert(s->perf_guard_words[SG_EVENTS] == count+1 && sounds == 2 && velocities == 2);
	/* Accepted armored hit at health=1: real wear and carry, zero debit. */
	s->player.hurt_resist_time = 0; s->player.damage_remainder = 0;
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_ARMOR,
		(struct item_data){.id = ITEM_DIAMOND_HELMET, .count = 1});
	assert(server_local_damage_player_result(s, 8, 1, 0, &accepted) && accepted);
	assert(s->player.health == 1 && s->player.damage_remainder == 1);
	assert(s->player.inventory.items[INVENTORY_SLOT_ARMOR].durability == 1);
	const uint32_t* e = s->perf_guard_words + 10 + ((s->perf_guard_words[SG_EVENTS]-1)%4)*9;
	assert(e[5] == 8 && e[6] == 7 && e[7] == 0);
	inventory_clear_slot(&s->player.inventory, INVENTORY_SLOT_ARMOR);
	/* Cumulative damage between refills, then minimum sampled BEFORE healing. */
	assert(server_local_set_player_health(s, 40)); s->player.hurt_resist_time = 0;
	assert(server_local_damage_player_result(s, 24, 0, 0, &accepted) && accepted);
	assert(s->player.health == 16);
	assert(server_local_damage_player_result(s, 48, 0, 0, &accepted) && accepted);
	assert(s->player.health == 1);
	server_local_perf_guard_before_damage(s);
	assert(s->player.health == 160 && s->perf_guard_words[SG_REFILLS] == 1
		&& s->perf_guard_words[SG_MIN_HEALTH] == 1);
	/* Sustained accepted lethal transactions; never a committed death. */
	for(unsigned i = 0; i < 120; i++) {
		s->player.hurt_resist_time = 0;
		assert(server_local_damage_player_result(s, 168, 0, 0, &accepted) && accepted);
		assert(s->player.health == 1);
		if(i % 3 == 2) server_local_perf_guard_before_damage(s);
	}
	uint32_t deaths[5]; assert(server_local_death_snapshot(deaths));
	assert(deaths[SLD_HOST_DEATHS] == 0 && deaths[SLD_GUEST_DEATHS] == 0);
	assert(s->perf_guard_words[SG_REFILLS] == 41);
	assert(s->perf_guard_words[SG_DROPPED] == s->perf_guard_words[SG_EVENTS]-4);
	struct hw_telemetry_heartbeat hb = {.player_health = s->player.health};
	assert(server_local_death_heartbeat(true, &hb));
	assert(server_local_perf_guard_heartbeat(true, &hb));
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE], context[640];
	assert(hw_telemetry_format_perf_guard(context, sizeof(context), hb.perf_guard_words, hb.tick) < sizeof(context));
	assert(hw_telemetry_format_heartbeat(line, sizeof(line), "guard-test", "perf_stress_5m", &hb) < sizeof(line));
	assert(hw_telemetry_init(&(struct hw_telemetry_config){.enabled = true,
		.run_id = "guard-test", .profile = "perf_stress_5m", .host = "127.0.0.1", .port = 1}));
	assert(hw_telemetry_emit_observer(context)); /* Actual run-ID envelope. */
	puts(line); /* Actual producer -> actual formatter -> Python. */
	hw_telemetry_shutdown();
	clin_test_set_rpc_observer(NULL, NULL);
	destroy(s);
	puts("PASS health=1 flat/melee accepted+knockback, fresh/excess/absorbed, armor wear/carry, cumulative, sustained120, pre-refill minimum, bounded context");
}
static void isolation(void) {
	for(unsigned mode = 0; mode < 11; mode++) {
		struct server_local* s = fixture(); arm(s);
		s->player.health = 8;
		switch(mode) {
		case 0: s->perf_guard_requested = false; break;
		case 1: s->perf_stress = false; break;
		case 2: s->perf_guard_local_arm = false; break; /* remote/manual */
		case 3: s->player.finished_loading = false; break;
		case 4: s->paused = true; break;
		case 5: s->input_ack_pending = 1; break;
		case 6: s->player.rz.recovering = true; break;
		case 7: s->load_failure_terminal = true; break;
		case 8: s->world_switch_phase = SERVER_WORLD_SWITCH_PORTAL; break;
		case 9: string_set_str(s->level_name, "ordinary-save"); break;
		case 10: s->player.health = 0; break;
		}
		server_local_perf_guard_before_damage(s);
		assert(s->player.health == (mode == 10 ? 0 : 8));
		assert(s->perf_guard_words[SG_REFILLS] == 0);
		const uint32_t barrier[] = {0, SGB_SCOPE, SGB_SCOPE, SGB_LOADING,
			SGB_PAUSE, SGB_ACK, SGB_RESIDENCY, SGB_TERMINAL, SGB_WORLD,
			SGB_SCOPE, SGB_DEAD};
		if(mode) {
			uint32_t words[SERVER_LOCAL_PERF_GUARD_WORDS];
			assert(server_local_perf_guard_snapshot(words));
			assert(words[SG_VERSION] == 2 && !words[SG_EFFECTIVE]
				&& (words[SG_BARRIERS] & barrier[mode]));
		}
		bool accepted = false;
		(void)server_local_damage_player_result(s, 16, 0, 0, &accepted);
		clin_drain_discard();
		assert(s->perf_guard_words[SG_EVENTS] == 0);
		if(mode == 0 || mode == 1 || mode == 2 || mode == 9)
			assert(s->player.health <= 0 && s->player_deaths == 1);
		destroy(s);
	}
	struct server_local* s = fixture(); arm(s);
	struct server_player guest = {.health = 8, .has_pos = true, .finished_loading = true};
	s->guest_actor = &guest;
	bool accepted = false;
	assert(!server_local_damage_actor_result(s, &guest, 16, 1, 0, &accepted) && accepted);
	clin_drain_discard(); assert(guest.health == 0 && s->player.health == 160);
	assert(s->perf_guard_words[SG_EVENTS] == 0);
	s->guest_actor = NULL;
	assert(server_local_set_player_health(s, 0)); /* unsupported direct lifecycle write */
	clin_drain_discard(); server_local_perf_guard_before_damage(s);
	assert(s->player.health == 0 && s->player_deaths == 1 && s->perf_guard_words[SG_MIN_HEALTH] == 0);
	uint32_t old = s->perf_guard_words[SG_SESSION]; destroy(s);
	uint32_t out[SERVER_LOCAL_PERF_GUARD_WORDS]; assert(!server_local_perf_guard_snapshot(out));
	s = fixture(); assert(s->perf_guard_words[SG_SESSION] != old);
	assert(!s->perf_guard_requested && !s->perf_guard_armed && !s->perf_guard_words[SG_EVENTS]);
	destroy(s);
	puts("PASS OFF/non-stress/remote/guest/dead/loading/pause/ACK/residency/terminal/transfer/root isolation, lifecycle kill, session retirement/reset");
}
static void prepass_rpc(void) {
	struct server_local* s = fixture(); arm(s);
	clin_test_set_rpc_observer(probe, NULL);
	for(int x = -1; x <= 2; x++) for(int z = -1; z <= 2; z++) split_populate(s, x, z);
	s->far_view_distance = MAX_VIEW_DISTANCE;
	s->player.x = s->player.z = 8.5; s->player.y = 100;
	s->player.health = 8; s->player.hurt_resist_time = 0;
	s->player.on_ground = false; s->player.fall_distance = 24;
	s->world_time = 6001;
	server_local_refresh_player_membership(s);
	struct entity* tnt = server_local_spawn_primed_tnt(s, (vec3){15.5,100,8.5}, 0);
	assert(tnt);
	glm_vec3_zero(tnt->vel);
	server_world_begin_update(&s->worlds[0]);
	while(server_world_try_begin_explosion(&s->worlds[0])) { }
	assert(!tnt->tick_server(tnt, s) && entity_primed_tnt_has_pending_explosion(tnt));
	struct server_rpc pos = {.type = SRPC_PLAYER_POS, .payload.player_pos = {
		.x = 8.5, .y = 100, .z = 8.5, .on_ground = true,
		.correction_seq = s->player.correction_seq}};
	assert(svin_rpc_send(&pos));
	health_rows = sounds = velocities = 0;
	server_local_test_update_once(s);
	assert(s->player.health > 0 && s->player_deaths == 0);
	assert(s->player.on_ground && s->player.fall_distance == 0);
	assert(s->perf_guard_words[SG_REFILLS] == 2);
	assert(s->perf_guard_words[SG_EVENTS] == 2);
	const uint32_t* blast = s->perf_guard_words + 10;
	const uint32_t* fall = blast + 9;
	assert(blast[4] == 160 && blast[5] > 8 && blast[5] < 160);
	assert(fall[3] == PLAYER_DAMAGE_FALL && fall[4] == 160 && fall[5] == 168);
	assert(fall[6] == 168 - blast[5]); /* real excess-window arbitration */
	assert(health_rows == 4 && health_values[0] == 160
		&& health_values[1] == 160-(short)blast[7]
		&& health_values[2] == 160 && health_values[3] == s->player.health);
	assert(s->perf_guard_words[SG_MIN_HEALTH] <= 8); /* before first refill */
	clin_test_set_rpc_observer(NULL, NULL);
	destroy(s);
	puts("PASS actual cap-retained explosion prepass -> refill -> actual queued RPC 24-block landing; health/RPC/cause/window order");
}
/* Existing pooled runtime seam; no production difficulty algorithm change. */
void server_local_test_set_difficulty(int);
static void boundaries(void) {
	struct server_local* s = fixture(); arm(s);
	bool accepted = false;
	s->player.hurt_resist_time = 0;
	assert(server_local_damage_player_result(s, 40000, 0, 0, &accepted) && accepted);
	assert(s->player.health == 1 && s->perf_guard_words[10+5] == 40000
		&& s->perf_guard_words[10+6] == 40000 && s->perf_guard_words[10+7] == 159);
	destroy(s);
	/* Real melee difficulty precedes the guard; Peaceful remains absorbed. */
	for(int difficulty = 0; difficulty < 4; difficulty++) {
		s = fixture(); arm(s); s->player.health = 1;
		server_local_test_set_difficulty(difficulty);
		struct entity zombie = {.type = ENTITY_MONSTER, .pos = {2,4,0}};
		zombie.data.monster.id = MONSTER_ZOMBIE;
		server_local_hurt_player(s, &zombie); clin_drain_discard();
		assert(s->player.health == 1);
		if(difficulty == 0) assert(s->perf_guard_words[SG_EVENTS] == 0);
		else {
			assert(s->perf_guard_words[SG_EVENTS] == 1);
			assert(s->perf_guard_words[10+5] == 40);
			assert(s->perf_guard_words[10+6] == (difficulty == 1 ? 16u : difficulty == 2 ? 40u : 56u));
			assert(s->perf_guard_words[10+7] == 0);
		}
		destroy(s);
	}
	server_local_test_set_difficulty(2);
	s = fixture(); arm(s); s->player.health = 8; s->player.y = -70;
	assert(!server_local_test_apply_player_hazards(s, 0, 0, 0, false));
	clin_drain_discard(); server_local_perf_guard_before_damage(s);
	assert(s->player.health == -24 && s->player_deaths == 1
		&& s->player_death_cause == PLAYER_DAMAGE_VOID && s->perf_guard_words[SG_MIN_HEALTH] == 0
		&& s->perf_guard_words[SG_EVENTS] == 0);
	destroy(s);
	s = fixture(); arm(s); s->perf_guard_words[SG_EVENTS] = UINT32_MAX;
	s->perf_guard_words[SG_DROPPED] = UINT32_MAX-4;
	assert(server_local_damage_player_result(s, 168, 0, 0, &accepted) && accepted);
	assert(s->player.health == 1 && s->perf_guard_words[SG_OVERFLOW] == 1
		&& s->perf_guard_words[SG_EVENTS] == UINT32_MAX && !s->perf_guard_armed);
	server_local_perf_guard_before_damage(s);
	assert(s->player.health == 1 && s->perf_guard_words[SG_EFFECTIVE] == 0);
	destroy(s);
	puts("PASS wide debit before narrowing, actual P/E/N/H arbitration, real void kill, counter exhaustion fail-closed");
}
static void width(void) {
	struct hw_telemetry_heartbeat hb = {0};
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE], context[640];
	hb.anchor.deaths = UINT32_MAX; hb.anchor.death_cause = "unknown";
	size_t base = hw_telemetry_format_heartbeat(line, sizeof(line), "width", "perf_stress_5m", &hb);
	hb.perf_guard_present = true;
	for(unsigned i = 0; i < SERVER_LOCAL_PERF_GUARD_WORDS; i++) hb.perf_guard_words[i] = UINT32_MAX;
	hb.death_present = true; hb.death_record[0] = UINT32_MAX;
	size_t n = hw_telemetry_format_heartbeat(line, sizeof(line), "width", "perf_stress_5m", &hb);
	assert(base-n == 11 && strstr(line, "\"g1\":1") && n < sizeof(line));
	/* Includes the optional widest 142-byte lease: no reserve is consumed. */
	assert(13795u + 142u - 11u < HW_TELEMETRY_HEARTBEAT_LINE_SIZE);
	hb.death_record[1] = hb.death_record[3] = UINT32_MAX;
	hb.death_record[2] = hb.death_record[4] = 8;
	n = hw_telemetry_format_heartbeat(line, sizeof(line), "width", "perf_stress_5m", &hb);
	assert(n == base && !strstr(line, "\"g1\""));
	assert(hw_telemetry_format_perf_guard(context, sizeof(context), hb.perf_guard_words, UINT32_MAX) < sizeof(context));
	puts("PASS guarded zero-death width saves11B; death/unavailable adds0B; full reserve preserved including lease; context<640B; unchanged ring/UDP cap");
}
static void void_disarms(void) {
	struct server_local* s = fixture(); arm(s);
	s->player.health = 8; s->player.y = -70;
	assert(!server_local_test_apply_player_hazards(s, 0, 0, 0, false));
	clin_drain_discard();
	assert(!s->perf_guard_armed && !s->perf_guard_local_arm);
	/* Respawn's direct fresh-body write must not restore the retired arm. */
	s->player.health = 8; s->player.y = 4 + PLAYER_EYE_HEIGHT;
	server_local_perf_guard_before_damage(s);
	assert(s->player.health == 8 && !s->perf_guard_words[SG_EFFECTIVE]);
	bool accepted = false;
	assert(!server_local_damage_player_result(s, 168, 0, 0, &accepted) && accepted);
	clin_drain_discard();
	assert(s->player_deaths == 2);
	destroy(s);
	puts("PASS void death permanently disarms session; fresh body has no refill/debit guard");
}
int main(int argc, char** argv) {
	assert(svin_init() && clin_init()); blocks_init(); items_init();
	if(argc > 1 && !strcmp(argv[1], "--void-red")) { void_disarms(); return 0; }
	/* Attribution-only slice; the stress golden assertion remains active. */
	if(argc > 1 && !strcmp(argv[1], "--stress-trace")) {
		stress_off_ticks(); return 0;
	}
	off_equivalence(); fflush(stdout);
	stress_off_ticks(); fflush(stdout);
	if(argc > 1 && !strcmp(argv[1], "--off")) return 0;
	sustained_stress_ticks();
	landing(false, 24);
	landing(true, 23);
	landing(true, 24);
	hits();
	isolation();
	prepass_rpc();
	boundaries();
	void_disarms();
	width();
	puts("PASS real-handler lethal control and guarded 23/24-block landings");
	return 0;
}
