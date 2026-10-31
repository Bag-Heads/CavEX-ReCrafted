/* Copyright (c) 2026 CaveWii_v2 contributors
 * WINDUP-SPEC.md: exercise the real authoritative tick and local RPC path.
 * No duplicate melee state machine or wall-clock animation in this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif
#include <m-lib/m-string.h>
#include "support/test_fixture_dir.h"
#include "support/mob_motion_fixture.h"
#include "block/blocks.h"
#include "config.h"
#include "entity/combat.h"
#include "entity/entity_monster.h"
#include "entity/mob_windup_cue.h"
#include "game/game_state.h"
#include "item/inventory.h"
#include "network/inventory_logic.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"

static struct server_local s;
static char fixture[128];
static struct mob_windup_cue received;
static uint32_t watched;
static unsigned moves;
static unsigned normal_moves, cue_only_moves;
static bool checking_retirement;
static struct mob_windup_cue spawned;
static FILE* trace_output;

/* IEEE bits avoid printf %a's platform-dependent trailing hex digits. */
static unsigned long long trace_bits(double value) {
	uint64_t bits;
	memcpy(&bits, &value, sizeof bits);
	return (unsigned long long)bits;
}

static void observe(const struct client_rpc* rpc, void* arg) {
	(void)arg;
	if(rpc->type == CRPC_ENTITY_MOVE
	   && rpc->payload.entity_move.entity_id == watched) {
		if(rpc->payload.entity_move.windup_cue_only) cue_only_moves++;
		else normal_moves++;
		if(checking_retirement)
			assert(rpc->payload.entity_move.windup_cue_only);
		received = rpc->payload.entity_move.windup_cue;
		moves++;
	}
	if(rpc->type == CRPC_SPAWN_MONSTER
	   && rpc->payload.spawn_monster.entity_id == watched)
		spawned = rpc->payload.spawn_monster.windup_cue;
}

static void pin(struct entity* e, float distance) {
	e->teleport(e, (vec3) {24.5F - distance, 4.0F, 24.5F});
	e->vel[0] = e->vel[1] = e->vel[2] = 0;
	e->on_ground = true;
	test_mob_sync(e);
	s.player.has_pos = s.player.finished_loading = true;
	s.player.x = s.player.z = 24.5;
	s.player.y = 4.0 + PLAYER_EYE_HEIGHT;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.hurt_resist_time = 0;
}

static void tick(struct entity* e) {
	assert(!e->tick_server(e, &s));
	clin_drain_discard();
	assert(received.version == MOB_WINDUP_CUE_VERSION);
	assert(received.phase == e->data.monster.windup_cue.phase);
}

static void lifecycle(int id) {
	struct entity* e = server_local_spawn_monster(
		(vec3) {23.5F, 4.0F, 24.5F}, id, &s);
	assert(e);
	clin_drain_discard();
	watched = e->id;
	moves = 0;
	e->data.monster.anger_level = id == MONSTER_PIG_ZOMBIE ? 400 : 0;
	if(id == MONSTER_WOLF)
		wolf_flag_set(&e->data.monster.wolf_flags, WOLF_FLAG_ANGRY, true);
	assert(e->data.monster.windup_cue.phase == 0);
	for(int t = 1; t <= MOB_ATTACK_WINDUP_TICKS; t++) {
		pin(e, 1.0F);
		tick(e);
		assert(e->data.monster.attack_windup == MOB_ATTACK_WINDUP_TICKS - t);
		assert(received.phase == (t == 1 ? 1 : t < MOB_ATTACK_WINDUP_TICKS ? 2 : 0));
		assert(s.player.health == MAX_PLAYER_HEALTH);
		if(t == 2) {
			assert(server_local_try_publish_entity_w(&s, &s.worlds[0], e));
			clin_drain_discard();
			assert(spawned.version == 1 && spawned.phase == 2);
		}
	}
	pin(e, 1.0F);
	tick(e);
	assert(s.player.health < MAX_PLAYER_HEALTH);
	assert(received.phase == 0);
	for(int t = 0; t < 45; t++) {
		pin(e, 1.0F);
		tick(e);
		assert(received.phase == 0); /* sustained swings get no cue */
	}
	pin(e, 10.0F);
	tick(e);
	assert(e->data.monster.attack_windup == MOB_ATTACK_WINDUP_TICKS);
	assert(received.phase == 0); /* positive re-arm alone is inert */
	e->data.monster.attack_time = 0;
	pin(e, 1.0F);
	tick(e);
	assert(received.phase == 1);
	pin(e, 10.0F);
	tick(e);
	assert(received.phase == 0); /* cancel */
	/* Unavailable target, without death, retires the presentation too. */
	s.player.has_pos = s.player.finished_loading = false;
	tick(e);
	assert(received.phase == 0);
	e->data.monster.attack_time = 0;
	pin(e, 1.0F);
	tick(e);
	assert(received.phase == 1);
	s.player.health = 0;
	tick(e);
	assert(received.phase == 0); /* dead target */
	e->data.monster.attack_time = 0;
	pin(e, 1.0F);
	tick(e);
	assert(received.phase == 1);
	/* A lethal damage event after publication must clear the client cue now,
	 * even before the victim's next authoritative tick. */
	checking_retirement = true;
	entity_monster_apply_damage(e, 1000, false);
	assert(received.phase == 0);
	checking_retirement = false;
	tick(e);
	assert(received.phase == 0); /* own death, including early-return MOVE */
	assert(moves > 0);
	printf("  species %d: countdown, first hit, sustained, re-arm, cancel, target death, own death PASS\n", id);
	free(e);
	dict_entity_erase(s.entities, watched);
}

/* Exercise a phase edge while the real ordinary MOVE gate is stable.
 * A stale advertised phase is injected to isolate cancellation publication. */
static void phase_only_gate(void) {
	for(int x = 22; x <= 26; x++)
		for(int z = 22; z <= 26; z++) {
			assert(server_world_set_block(&s.worlds[0], x, 8, z,
				(struct block_data) {.type = BLOCK_STONE}));
			clin_drain_discard();
		}
	struct entity* e = server_local_spawn_monster(
		(vec3) {24.5F, 4, 24.5F}, MONSTER_WOLF, &s);
	assert(e);
	clin_drain_discard(); watched = e->id;
	wolf_flag_set(&e->data.monster.wolf_flags, WOLF_FLAG_TAMED, true);
	wolf_flag_set(&e->data.monster.wolf_flags, WOLF_FLAG_SITTING, true);
	e->data.monster.wolf_target_id = UINT32_MAX;
	e->on_ground = true; test_mob_sync(e);
	s.player.has_pos = s.player.finished_loading = false;
	unsigned previous, settling = 0;
	do {
		previous = normal_moves;
		tick(e);
		assert(++settling < 50);
	} while(normal_moves != previous);
	unsigned timer = e->data.monster.ticks_since_move_send;
	assert(timer < MOB_MOVE_RESEND_INTERVAL_TICKS);
	unsigned cues = cue_only_moves;
	e->data.monster.windup_cue.phase = 2;
	e->data.monster.last_sent_move.windup_cue.phase = 2;
	tick(e);
	assert(normal_moves == previous);
	assert(cue_only_moves == cues + 1 && received.phase == 0);
	assert(e->data.monster.ticks_since_move_send == timer + 1);
	uint32_t id = e->id; free(e); dict_entity_erase(s.entities, id);
	puts("  phase-only cancellation: ordinary MOVE bytes/schedule and resend timer preserved PASS");
}

static void pose_contract(void) {
	struct config c = {.root = json_value_init_object()};
	assert(!config_mob_attack_windup_cue(&c));
	JSON_Object* o = json_value_get_object(c.root);
	assert(json_object_dotset_boolean(o, "ui.mob_attack_windup_cue", 1) == JSONSuccess);
	assert(config_mob_attack_windup_cue(&c));
	assert(json_object_dotset_number(o, "ui.mob_attack_windup_cue", 1) == JSONSuccess);
	assert(!config_mob_attack_windup_cue(&c)); /* strict boolean */
	config_destroy(&c);
	for(int id = 0; id < 256; id++) {
		struct mob_windup_cue cue = {MOB_WINDUP_CUE_VERSION, 2};
		assert(mob_windup_cue_degrees(cue, id, false) == 0);
		float full = id == MONSTER_ZOMBIE || id == MONSTER_PIG_ZOMBIE ? 35.0F
			: id == MONSTER_SPIDER ? 25.0F : id == MONSTER_WOLF ? 20.0F : 0;
		assert(mob_windup_cue_degrees(cue, id, true) == full);
		cue.phase = 1;
		assert(mob_windup_cue_degrees(cue, id, true) == full * 0.5F);
		cue.version = 0;
		assert(mob_windup_cue_degrees(cue, id, true) == 0);
		cue.version = 255;
		assert(mob_windup_cue_degrees(cue, id, true) == 0);
		cue.version = MOB_WINDUP_CUE_VERSION;
		cue.phase = 255;
		assert(mob_windup_cue_degrees(cue, id, true) == 0);
	}
}

static void excluded_species(void) {
	const int ids[] = {MONSTER_SKELETON, MONSTER_CREEPER, MONSTER_SLIME_MEDIUM,
		MONSTER_PIG, MONSTER_PIG_ZOMBIE, MONSTER_WOLF, MONSTER_GIANT};
	for(size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
		struct entity* e = server_local_spawn_monster(
			(vec3) {23.5F, 4, 24.5F}, ids[i], &s);
		assert(e);
		clin_drain_discard();
		watched = e->id;
		pin(e, 1.0F);
		tick(e);
		assert(received.phase == 0);
		uint32_t id = e->id;
		free(e);
		dict_entity_erase(s.entities, id);
	}
	puts("  ranged/fuse/contact/passive/neutral pig zombie/idle wolf: no cue PASS");
}

/* This tape is also run with the original cd687b6a8 entity_monster.c object
 * substituted at link time. It omits the new cue fields, and
 * records the actual movement, combat and BOTH simulation RNG streams. */
/* Preserve the existing MOVE payload and its tick of publication as well
 * as simulation values. Cue-only envelopes deliberately have no base inputs. */
static int trace_species, trace_tick;
static void observe_trace(const struct client_rpc* rpc, void* arg) {
	(void)arg;
	if(rpc->type != CRPC_ENTITY_MOVE || rpc->payload.entity_move.entity_id != watched
	   || rpc->payload.entity_move.windup_cue_only)
		return;
	fprintf(trace_output, "MOVE %d %d", trace_species, trace_tick);
	for(int axis = 0; axis < 3; axis++)
		fprintf(trace_output, " %016llx", trace_bits(rpc->payload.entity_move.pos[axis]));
#define TRACE_INT(field) fprintf(trace_output, " %d", (int)rpc->payload.entity_move.field)
#define TRACE_FLOAT(field) fprintf(trace_output, " %016llx", trace_bits(rpc->payload.entity_move.field))
	TRACE_INT(approach_ticks);
	TRACE_INT(on_fire); TRACE_INT(swelling); TRACE_INT(hurt);
	TRACE_INT(sheared); TRACE_INT(saddled); TRACE_INT(fleece_color);
	TRACE_INT(fuse); TRACE_INT(death_time); TRACE_INT(ghast_attacking);
	TRACE_INT(wolf_flags); TRACE_INT(wolf_health);
	TRACE_INT(wolf_shake_armed); TRACE_INT(wolf_shake_active);
	TRACE_FLOAT(wolf_shake_time); TRACE_FLOAT(wolf_prev_shake_time);
	TRACE_FLOAT(wolf_head_rotation); TRACE_FLOAT(wolf_head_rotation_old);
	TRACE_INT(charged); TRACE_INT(pig_ridden); TRACE_INT(pig_rider_id);
	TRACE_INT(arrow_visual.valid);
	fputc('\n', trace_output);
#undef TRACE_INT
#undef TRACE_FLOAT
}

static void simulation_trace(int id) {
	struct entity* e = server_local_spawn_monster(
		(vec3) {23.5F, 4.0F, 24.5F}, id, &s);
	assert(e);
	clin_drain_discard();
	watched = e->id;
	trace_species = id;
	clin_test_set_rpc_observer(observe_trace, NULL);
	e->data.monster.anger_level = id == MONSTER_PIG_ZOMBIE ? 400 : 0;
	if(id == MONSTER_WOLF)
		wolf_flag_set(&e->data.monster.wolf_flags, WOLF_FLAG_ANGRY, true);
	for(int t = 0; t < 120; t++) {
		trace_tick = t;
		pin(e, t >= 15 && t < 26 ? 10.0F : 1.0F);
		if(t >= 86 && t < 96)
			s.player.health = 0;
		if(t == 110)
			entity_monster_apply_damage(e, 1000, false);
		assert(!e->tick_server(e, &s));
		clin_drain_discard();
		fprintf(trace_output, "%d %d %d %d %d %d %d %d %llu %llu",
			id, t, e->health, s.player.health, e->data.monster.attack_time,
			e->data.monster.attack_windup, e->data.monster.aggro_time,
			e->ai_state, (unsigned long long)e->gameplay_rng.seed,
			(unsigned long long)s.rand_src.seed);
		for(int axis = 0; axis < 3; axis++)
			fprintf(trace_output, " %016llx %016llx %016llx %016llx",
				trace_bits(e->pos[axis]), trace_bits(e->vel[axis]),
				trace_bits(e->data.monster.motion_d.pos[axis]),
				trace_bits(e->data.monster.motion_d.motion[axis]));
		fprintf(trace_output, " %016llx %016llx %u %d %d\n",
			trace_bits(s.player.vel_x), trace_bits(s.player.vel_z),
			e->data.monster.wolf_target_id,
			e->gameplay_rng.have_next_gaussian, s.rand_src.has_next_gaussian);
		fprintf(trace_output, "CACHE %u %u %016llx %016llx\n", e->data.monster.last_sent_move_valid,
			e->data.monster.ticks_since_move_send,
			trace_bits(e->gameplay_rng.have_next_gaussian ? e->gameplay_rng.next_gaussian_cached : 0),
			trace_bits(s.rand_src.has_next_gaussian ? s.rand_src.next_gaussian : 0));
	}
	clin_test_set_rpc_observer(NULL, NULL);
	uint32_t id_to_free = e->id;
	free(e);
	dict_entity_erase(s.entities, id_to_free);
}

/* Re-pin integ1011/2d: controlled c9e25a0fa replay + c5927d951/8f59aac0a
 * corpse scheduling + 90708c579 double X/Z knockback reconstructs every line.
 * Old c9e25a0fa tape SHA256: 8b48c6f3c9197544033ebcbde0d7424e194a131a8334868c74de559f998c9c3f.
 * Old tape retained at tmp/jobs/integ-1011/evidence/lineage-2d/
 * pr02-retained-old-pin.txt (also c9e25a0fa:tests/support/... in git).
 * Executable proof: tests/oracle/integ1011_lineage/pr02.py; zero residuals. */
static void check_simulation_baseline(void) {
	char path[1024];
	assert(strlen(__FILE__) < sizeof path);
	memcpy(path, __FILE__, strlen(__FILE__) + 1);
	char* separator = strrchr(path, '/');
	char* windows_separator = strrchr(path, '\\');
	if(windows_separator && (!separator || windows_separator > separator))
		separator = windows_separator;
	assert(separator);
	const char* suffix = "/support/mob_windup_sim_baseline.txt";
	assert((size_t)(separator - path) + strlen(suffix) < sizeof path);
	memcpy(separator, suffix, strlen(suffix) + 1);
	FILE* base = fopen(path, "r");
	assert(base);
	trace_output = tmpfile();
	assert(trace_output);
	simulation_trace(MONSTER_ZOMBIE);
	simulation_trace(MONSTER_PIG_ZOMBIE);
	simulation_trace(MONSTER_SPIDER);
	simulation_trace(MONSTER_WOLF);
	rewind(trace_output);
	/* Attribution aid (write-only, changes nothing checked): CW_WINDUP_DUMP=path
	 * copies the actual tape out so a moved byte can be attributed by field. */
	if(getenv("CW_WINDUP_DUMP")) {
		FILE* dump = fopen(getenv("CW_WINDUP_DUMP"), "wb");
		assert(dump);
		for(int c; (c = fgetc(trace_output)) != EOF;)
			fputc(c, dump);
		fclose(dump);
		rewind(trace_output);
	}
	for(;;) {
		int expected = fgetc(base), actual = fgetc(trace_output);
		assert(expected == actual);
		if(expected == EOF)
			break;
	}
	assert(!ferror(base) && !ferror(trace_output));
	assert(fclose(base) == 0 && fclose(trace_output) == 0);
	puts("  pre-W01 sim tape: 480 actual ticks, combat/motion/entity+world RNG byte-exact PASS");
}

static void receive_contract(void) {
	/* Actual client handlers, including joining in mid-countdown, completion,
	 * legacy/absent state and a remote session that never advertised this cue. */
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 4242);
	struct client_rpc spawn = {.type = CRPC_SPAWN_MONSTER,
		.payload.spawn_monster = {.entity_id = 777,
			.monster_id = MONSTER_ZOMBIE,
			.windup_cue = {MOB_WINDUP_CUE_VERSION, 2},
			.pos = {24, 4, 24}}};
	clin_process_direct(&spawn);
	struct entity** slot = dict_entity_get(gstate.entities, 777);
	assert(slot && (*slot)->data.monster.windup_cue.phase == 2);
	struct entity* e = *slot;
	struct client_rpc move = {.type = CRPC_ENTITY_MOVE,
		.payload.entity_move = {.entity_id = 777,
			.windup_cue = {MOB_WINDUP_CUE_VERSION, 1}}};
	clin_process_direct(&move);
	assert(e->data.monster.windup_cue.phase == 1);
	/* Retirement/phase-only updates may never restart lerp or change old
	 * render inputs, even when the unused payload deliberately disagrees. */
	struct entity unchanged;
	memcpy(&unchanged, e, sizeof unchanged);
	struct client_rpc cue_only = {.type = CRPC_ENTITY_MOVE,
		.payload.entity_move = {.entity_id = 777, .windup_cue_only = true,
			.windup_cue = {MOB_WINDUP_CUE_VERSION, 0}, .pos = {99, 98, 97},
			.hurt = true, .on_fire = true, .swelling = true, .fuse = 30,
			.death_time = 19, .sheared = true, .saddled = true,
			.ghast_attacking = true, .wolf_flags = 255, .wolf_health = 99,
			.pig_ridden = true, .pig_rider_id = 123}};
	clin_process_direct(&cue_only);
	assert(e->data.monster.windup_cue.phase == 0);
	unchanged.data.monster.windup_cue = e->data.monster.windup_cue;
	assert(memcmp(e, &unchanged, sizeof unchanged) == 0);
	cue_only.payload.entity_move.windup_cue.version = 255;
	clin_process_direct(&cue_only);
	assert(memcmp(e, &unchanged, sizeof unchanged) == 0);
	cue_only.payload.entity_move.windup_cue = (struct mob_windup_cue) {1, 2};
	gstate.remote_session = true;
	clin_process_direct(&cue_only);
	assert(memcmp(e, &unchanged, sizeof unchanged) == 0);
	gstate.remote_session = false;
	puts("  cue-only/unknown/remote receipt: all base entity bytes unchanged PASS");
	move.payload.entity_move.windup_cue.phase = 0;
	clin_process_direct(&move);
	assert(e->data.monster.windup_cue.phase == 0);
	move.payload.entity_move.windup_cue = (struct mob_windup_cue) {0};
	clin_process_direct(&move);
	assert(e->data.monster.windup_cue.version == 0);
	gstate.remote_session = true;
	move.payload.entity_move.windup_cue = (struct mob_windup_cue) {1, 2};
	clin_process_direct(&move);
	assert(e->data.monster.windup_cue.version == 0);
	struct client_rpc destroy = {.type = CRPC_ENTITY_DESTROY,
		.payload.entity_destroy.entity_id = 777};
	clin_process_direct(&destroy);
	spawn.payload.spawn_monster.entity_id = 778;
	clin_process_direct(&spawn);
	slot = dict_entity_get(gstate.entities, 778);
	assert(slot && (*slot)->data.monster.windup_cue.version == 0);
	destroy.payload.entity_destroy.entity_id = 778;
	clin_process_direct(&destroy);
	gstate.remote_session = false;
	puts("  actual client SPAWN/MOVE: received/absent/remote state PASS");
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	bool trace = argc == 3 && strcmp(argv[1], "--sim-trace") == 0;
	if(trace) {
		trace_output = fopen(argv[2], "w");
		assert(trace_output);
	} else
		pose_contract();
	test_fixture_dir("test_mob_windup_cue_tmp", fixture, sizeof fixture);
	assert(clin_init());
	blocks_init();
	MKDIR(fixture);
	MKDIR(test_fixture_join(fixture, "/region"));
	string_t level;
	string_init_set_str(level, fixture);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	string_clear(level);
	s.worlds[0].session = &s;
	s.world_time = 18000;
	s.rand_src.seed = 12345;
	/* F4-RNG: the simulation tape also reaches body hurt and block drops. */
	jrand_set_seed(&s.worlds[0].rand, 4242);
	server_local_test_seed_body_rand(&s, &s.player, 12345);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player,
		&s, &s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	struct server_chunk* sc;
	for(int x = 0; x <= 2; x++)
		for(int z = 0; z <= 2; z++)
			assert(server_world_load_chunk(&s.worlds[0], x, z, &sc));
	if(trace) {
		simulation_trace(MONSTER_ZOMBIE);
		simulation_trace(MONSTER_PIG_ZOMBIE);
		simulation_trace(MONSTER_SPIDER);
		simulation_trace(MONSTER_WOLF);
		assert(fclose(trace_output) == 0);
	} else {
		check_simulation_baseline();
		clin_test_set_rpc_observer(observe, NULL);
		lifecycle(MONSTER_ZOMBIE);
		lifecycle(MONSTER_PIG_ZOMBIE);
		lifecycle(MONSTER_SPIDER);
		lifecycle(MONSTER_WOLF);
		excluded_species();
		phase_only_gate();
		clin_test_set_rpc_observer(NULL, NULL);
		receive_contract();
	}
	inventory_destroy(&s.player.inventory, &s.player);
	dict_entity_clear(s.entities);
	server_world_destroy(&s.worlds[0]);
	test_fixture_dir_cleanup(fixture);
	puts("PASS test_mob_windup_cue");
	return 0;
}
