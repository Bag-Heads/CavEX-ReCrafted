/*
	Group E integration goldens for the 2026-06-19 mob-AI parity audit, over a real
	server_local + superflat world:

	1. PLAYER I-FRAME EXCESS (Beta EntityLiving.attackEntityFrom). During the i-frame
	   window a hit <= the window's strongest is ABSORBED, but a STRONGER hit deals only
	   the EXCESS (incoming - last) without re-opening the window. The old model absorbed
	   ALL hits in the window regardless of magnitude.
	2. 3D BLAST KNOCKBACK (Beta Explosion.doExplosionA). The shove is the away-from-center
	   3D direction scaled by (1 - dist/radius), so it is distance-scaled AND has a vertical
	   component -- not the old fixed-0.4 horizontal-only melee recoil.

	Decompile: 740c583:1.7.3-LTS EntityLiving.attackEntityFrom, Explosion.doExplosionA.
*/

#include <assert.h>
#include <math.h>
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
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h" // inventory_logic_player (death-path item drop)
#include "network/server_interface.h" // MAX_PLAYER_HEALTH
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "item/inventory.h"           // inventory_create, INVENTORY_SIZE
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_player_combat_env_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
static struct server_local s;
static volatile int g_drain_run = 1;
static short g_health_seen_by_explosion_neighbour = -1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type) {
	struct block_data expected = {.type = (uint8_t)type, .metadata = 0};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type && live.metadata == expected.metadata);
}

static void reset_player(double x, double y, double z) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = x; s.player.y = y; s.player.z = z;
	s.player.health = MAX_PLAYER_HEALTH; // 160 (full)
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;
	s.player.fall_distance = 0.0F;
	s.player.vel_x = 0.0f; s.player.vel_z = 0.0f;
	s.player.spawn_x = (int)x; s.player.spawn_y = (int)y; s.player.spawn_z = (int)z;
	s.zombie_demo = false;
	// Mirror server_local_create's init: the player's open inventory defaults to its OWN
	// inventory (no container). The death/respawn path (set_player_health, new_health<=0)
	// dereferences active_inventory to close any open container; this memset(0)-zeroed
	// harness leaves it NULL, so the LETHAL-blast test below would crash without this.
	s.player.active_inventory = &s.player.inventory;
}

static void observe_explosion_neighbour(
	struct server_local* server, struct block_info* info) {
	(void)info;
	if(g_health_seen_by_explosion_neighbour < 0)
		g_health_seen_by_explosion_neighbour = server->player.health;
}

/* Explosion.doExplosionA damages/knocks entities before doExplosionB removes
 * the first block or invokes any neighbour callback. This ordering is gameplay:
 * a callback must observe the already-damaged player.
 * [decompile 740c583 Explosion.doExplosionA/doExplosionB] */
static void test_blast_damage_precedes_blocks(void) {
	reset_player(14.0, 70.5, 10.5);
	set(10, 70, 10, BLOCK_STONE);
	set(11, 70, 10, BLOCK_BEDROCK);
	void (*old_neighbour)(struct server_local*, struct block_info*)
		= block_bedrock.onNeighbourBlockChange;
	block_bedrock.onNeighbourBlockChange = observe_explosion_neighbour;
	g_health_seen_by_explosion_neighbour = -1;
	server_world_begin_update(&s.world);
	server_world_explode(&s.worlds[0], (vec3) {10.5f, 70.5f, 10.5f}, 3.0f);
	block_bedrock.onNeighbourBlockChange = old_neighbour;
	assert(g_health_seen_by_explosion_neighbour >= 0);
	assert(g_health_seen_by_explosion_neighbour < MAX_PLAYER_HEALTH);
	set(10, 70, 10, BLOCK_AIR);
	set(11, 70, 10, BLOCK_AIR);
	puts("  blast order: doExplosionA damage precedes doExplosionB callbacks OK");
}

// 1. The i-frame excess rule.
static void test_iframe_excess(void) {
	reset_player(8.0, 70.0, 8.0);

	// Fresh hit (window closed): full 10 damage, window opens, last_damage = 10.
	server_local_damage_player(&s, 10, 0.0f, 0.0f);
	assert(s.player.health == MAX_PLAYER_HEALTH - 10);
	assert(s.player.hurt_resist_time > 0);

	// Equal hit in-window: <= last_damage -> ABSORBED (no change). This is the common
	// repeated-zombie case, identical to the old absorb-all behavior.
	server_local_damage_player(&s, 10, 0.0f, 0.0f);
	assert(s.player.health == MAX_PLAYER_HEALTH - 10);

	// Stronger hit in-window: deals only the EXCESS (30 - 10 = 20), bumps last_damage.
	server_local_damage_player(&s, 30, 0.0f, 0.0f);
	assert(s.player.health == MAX_PLAYER_HEALTH - 10 - 20);

	// Equal-to-new-strongest in-window: absorbed (30 <= 30).
	server_local_damage_player(&s, 30, 0.0f, 0.0f);
	assert(s.player.health == MAX_PLAYER_HEALTH - 30);

	// Even stronger: excess again (50 - 30 = 20).
	server_local_damage_player(&s, 50, 0.0f, 0.0f);
	assert(s.player.health == MAX_PLAYER_HEALTH - 50);
	printf("  i-frame excess: 10,10,30,30,50 -> 110 health (absorb <=, apply excess) OK\n");
}

// Slow Wii server ticks can drain several 50 ms PLAYER_POS samples in one
// update. Fall detection must consume each velocity edge as the RPC is handled;
// deferring it until after the entire batch loses the airborne->landing edge.
static void test_batched_position_samples_preserve_landing(void) {
	reset_player(8.0, 74.0, 8.0);
	s.player.vel_y = 0.0f;
	s.player.old_vel_y = 0.0f;
	s.player.fall_distance = 0.0F;
	const struct server_rpc samples[] = {
		{.type = SRPC_PLAYER_POS,
		 .payload.player_pos = {.x = 8.0, .y = 73.7, .z = 8.0, .vel_y = -0.20f}},
		{.type = SRPC_PLAYER_POS,
		 .payload.player_pos = {.x = 8.0, .y = 70.7, .z = 8.0, .vel_y = -0.50f}},
		{.type = SRPC_PLAYER_POS,
		 .payload.player_pos = {
			 .x = 8.0, .y = 70.0, .z = 8.0, .vel_y = 0.0f,
			 .on_ground = true}},
		// A later grounded sample used to overwrite the only landing edge before
		// the once-per-server-tick fall sweep could inspect it.
		{.type = SRPC_PLAYER_POS,
		 .payload.player_pos = {
			 .x = 8.0, .y = 70.0, .z = 8.0, .vel_y = 0.0f,
			 .on_ground = true}},
	};
	for(size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
		struct server_rpc sample = samples[i];
		server_local_test_process_rpc(&s, &sample);
	}
	/* G8-BLAST-VELOCITY v4 (ASTRA-V3-REVIEW P1 damage ordering): every
	 * sample is still ACCOUNTED as it is handled, so the landing edge is
	 * kept -- on the third sample's event -- but the hit is that packet's
	 * handleFalling, applied when the tracker phase replays the events. */
	assert(s.player.health == MAX_PLAYER_HEALTH && s.player.motion_q_n == 4
		&& s.player.motion_q[2].fall_damage == HEALTH_PER_HEART / 2
		&& s.player.motion_q[3].fall_damage == 0);
	server_local_test_replay_actor_motion(&s);
	assert(s.player.health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2);
	assert(s.player_damage_cause == PLAYER_DAMAGE_FALL);
	puts("  fall: batched descent preserves >3 airborne blocks and deals 1 point OK");
}

/* Ground truth is the resolved collision flag carried by PLAYER_POS, not a
 * velocity threshold. Lava drag or an upward impulse can cross -0.079 while
 * the body remains airborne; that must preserve and extend fallDistance. */
static void test_airborne_velocity_crossing_is_not_landing(void) {
	reset_player(8.0, 71.0, 8.0);
	s.player.on_ground = false;
	s.player.vel_y = -0.5F;
	s.player.old_vel_y = -0.5F;
	s.player.fall_distance = 5.0F;
	struct server_rpc airborne = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = 8.0,
			.y = 70.9,
			.z = 8.0,
			.vel_y = -0.02F,
			.on_ground = false,
			.correction_seq = s.player.correction_seq,
		},
	};
	server_local_test_process_rpc(&s, &airborne);
	assert(s.player.health == MAX_PLAYER_HEALTH);
	assert(fabsf(s.player.fall_distance - 5.1F) < 1.0e-5F);
	puts("  fall: airborne velocity crossing preserves/extends fallDistance OK");
}

/* Conversely, a resolved grounded sample bills the existing scalar even if a
 * nonstandard client impulse leaves velocity below the old heuristic cutoff. */
static void test_grounded_flag_bills_without_velocity_crossing(void) {
	reset_player(8.0, 70.7, 8.0);
	s.player.on_ground = false;
	s.player.vel_y = -0.5F;
	s.player.old_vel_y = -0.5F;
	s.player.fall_distance = 5.0F;
	struct server_rpc landing = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = 8.0,
			.y = 70.62,
			.z = 8.0,
			.vel_y = -0.2F,
			.on_ground = true,
			.correction_seq = s.player.correction_seq,
		},
	};
	server_local_test_process_rpc(&s, &landing);
	/* G8-BLAST-VELOCITY v4: the drain accounts the landing; its hit is the
	 * packet's handleFalling, applied by the tracker-phase replay. */
	server_local_test_replay_actor_motion(&s);
	assert(s.player.health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART);
	assert(s.player.fall_distance == 0.0F);
	puts("  fall: grounded flag bills existing fallDistance independent of velocity OK");
}

/* Beta updateFallState(resolvedY,true) calls fall(existing fallDistance) and
 * clears it; the collision-clipped landing component is not accumulated. This
 * is the real gravity recurrence immediately before a 4.01-block fractional
 * landing: 3.34627015 airborne blocks plus a final 0.66372985 collision move.
 * Exact damage is ceil(3.34627015-3)=1 point, not 2 from the full 4.01. */
static void test_fractional_landing_excludes_final_component(void) {
	const double landing_eye_y = 70.62;
	const double final_component = 0.66372985;
	reset_player(8.0, landing_eye_y + final_component, 8.0);
	s.player.on_ground = false;
	s.player.vel_y = -0.7170746F;
	s.player.old_vel_y = -0.7170746F;
	s.player.fall_distance = 3.34627015F;
	struct server_rpc landing = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = 8.0,
			.y = landing_eye_y,
			.z = 8.0,
			.vel_y = -0.0784F,
			.on_ground = true,
			.correction_seq = s.player.correction_seq,
		},
	};
	server_local_test_process_rpc(&s, &landing);
	/* G8-BLAST-VELOCITY v4: the drain accounts the landing; its hit is the
	 * packet's handleFalling, applied by the tracker-phase replay. */
	server_local_test_replay_actor_motion(&s);
	assert(s.player.health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2);
	assert(s.player.fall_distance == 0.0F);
	puts("  fall: fractional landing bills airborne distance before final collision move OK");
}

/* Production transport regression: fill the real 16-slot inbox with grounded
 * samples, lose every airborne sample under its non-blocking policy, then drain
 * and deliver the grounded endpoint. The endpoint displacement must still bill
 * the fall; treating PLAYER_POS as wholly idempotent used to make it free. */
static void test_dropped_airborne_samples_preserve_fall(void) {
	reset_player(8.0, 74.0, 8.0);
	s.player.vel_y = 0.0f;
	s.player.old_vel_y = 0.0f;
	s.player.fall_distance = 0.0F;
	for(int i = 0; i < 16; i++) {
		struct server_rpc grounded = {
			.type = SRPC_PLAYER_POS,
			.payload.player_pos
				= {.x = 8.0, .y = 74.0, .z = 8.0, .vel_y = 0.0f,
				   .on_ground = true},
		};
		assert(svin_rpc_send(&grounded));
	}
	uint32_t drops_before = svin_dropped_count();
	for(int i = 1; i <= 3; i++) {
		struct server_rpc airborne = {
			.type = SRPC_PLAYER_POS,
			.payload.player_pos = {.x = 8.0,
								   .y = 74.0 - i,
								   .z = 8.0,
								   .vel_y = -0.2f * i},
		};
		assert(!svin_rpc_send(&airborne));
	}
	assert(svin_dropped_count() == drops_before + 3);
	server_local_test_process_messages(&s);
	struct server_rpc landing = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos
			= {.x = 8.0, .y = 70.0, .z = 8.0, .vel_y = 0.0f,
			   .on_ground = true},
	};
	assert(svin_rpc_send(&landing));
	server_local_test_process_messages(&s);
	/* G8-BLAST-VELOCITY v4: the drain accounts the landing; its hit is the
	 * packet's handleFalling, applied by the tracker-phase replay. */
	server_local_test_replay_actor_motion(&s);
	assert(s.player.health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2);
	puts("  fall transport: all airborne samples dropped; grounded endpoints still deal 1 point OK");
}

/* Entity.handleWaterMovement probes the contracted player body AABB, not only
 * the eye point. Shallow water around the feet therefore clears fallDistance
 * before a landing even when the eye cell is air. */
static void test_body_water_cancels_fall(void) {
	reset_player(8.5, 70.2, 8.5);
	s.player.vel_y = -0.5F;
	s.player.old_vel_y = -0.5F;
	s.player.fall_distance = 5.0F;
	const int water_y = (int)floor(s.player.y - PLAYER_EYE_HEIGHT);
	set(8, water_y, 8, BLOCK_WATER_STILL);
	struct server_rpc landing = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = s.player.x,
			.y = s.player.y,
			.z = s.player.z,
			.vel_y = 0.0F,
			.on_ground = true,
			.correction_seq = s.player.correction_seq,
		},
	};
	server_local_test_process_rpc(&s, &landing);
	assert(s.player.health == MAX_PLAYER_HEALTH);
	assert(s.player.fall_distance == 0.0F);
	set(8, water_y, 8, BLOCK_AIR);
	puts("  fall: shallow body water clears fallDistance with eye in air OK");
}

/* Water handling is independent of resolved Y. A rising/stationary wet sample
 * can be followed by a dry grounded sample in the same drained RPC batch; the
 * first sample must clear fallDistance before the second can bill it. */
static void test_nondescending_water_sample_cancels_batched_fall(void) {
	reset_player(8.5, 70.7, 8.5);
	s.player.on_ground = false;
	s.player.vel_y = 0.1F;
	s.player.old_vel_y = 0.1F;
	s.player.fall_distance = 5.0F;
	set(8, 69, 8, BLOCK_WATER_STILL);
	struct server_rpc wet_rise = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = 8.5,
			.y = 70.8,
			.z = 8.5,
			.vel_y = 0.1F,
			.on_ground = false,
			.correction_seq = s.player.correction_seq,
		},
	};
	server_local_test_process_rpc(&s, &wet_rise);
	struct server_rpc dry_landing = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = 9.5,
			.y = 70.7,
			.z = 8.5,
			.vel_y = 0.0F,
			.on_ground = true,
			.correction_seq = s.player.correction_seq,
		},
	};
	server_local_test_process_rpc(&s, &dry_landing);
	assert(s.player.health == MAX_PLAYER_HEALTH);
	assert(s.player.fall_distance == 0.0F);
	set(8, 69, 8, BLOCK_AIR);
	puts("  fall: non-descending wet sample clears scalar before batched dry landing OK");
}

/* Entity.handleLavaMovement uses World.isMaterialInBB over
 * boundingBox.expand(-0.10000000149011612, -0.4000000059604645,
 * -0.10000000149011612). A player center at x=8.15 is in the air column x=8,
 * while that contracted body still overlaps x=7. Eye/feet point samples miss
 * this faithful side contact entirely. */
static void test_body_side_lava_contact(void) {
	reset_player(8.15, 70.7, 8.5);
	s.player.on_ground = true;
	s.player.oxygen = MAX_OXYGEN;
	s.player.fire = -PLAYER_FIRE_RESISTANCE;
	s.player.vel_y = 0.0F;
	s.player.old_vel_y = 0.0F;
	const int feet_y = (int)floor(s.player.y - PLAYER_EYE_HEIGHT);
	const int lava_y = (int)floor(
		s.player.y - (double)PLAYER_EYE_HEIGHT
		+ 0.4000000059604645);
	assert(lava_y == feet_y);
	assert((int)floor(s.player.x) == 8);
	set(8, feet_y, 8, BLOCK_AIR);
	set(8, (int)floor(s.player.y), 8, BLOCK_AIR);
	set(7, lava_y, 8, BLOCK_LAVA_STILL);

	server_local_test_update_once(&s);

	assert(s.player.health
		   == MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2));
	assert(s.player.fire == 601);
	set(7, lava_y, 8, BLOCK_AIR);
	puts("  lava: 4-point hit precedes absorbed burning-box 1; fire advances to 601 OK");
}

/* Beta Entity.onEntityUpdate clears fire in water, then independently applies
 * lava's 4-point hit and fire=600 (mcb173-retromcp Entity.java:168-220,
 * 234-237). EntityLiving.onUpdate then reaches moveEntity through
 * onLivingUpdate (EntityLiving.java:203-205,642), whose wet tail replaces that
 * positive fire with -fireResistance (Entity.java:480-495). Rain-quench commit
 * f7409540 ports that same post-contact quench order. */
static void test_body_water_and_lava_both_apply(void) {
	reset_player(8.0, 70.7, 8.0);
	s.player.on_ground = true;
	s.player.oxygen = MAX_OXYGEN;
	s.player.fire = 123;
	s.player.fall_distance = 5.0F;
	s.player.vel_y = 0.0F;
	s.player.old_vel_y = 0.0F;
	const int contact_y = (int)floor(
		s.player.y - (double)PLAYER_EYE_HEIGHT
		+ 0.4000000059604645);
	/* Corner-adjacent cells avoid the lava/water hardening callback while both
	 * still overlap the 0.6-wide player body. */
	set(7, contact_y, 7, BLOCK_WATER_STILL);
	set(8, contact_y, 8, BLOCK_LAVA_STILL);

	server_local_test_update_once(&s);

	assert(s.player.health
		   == MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2));
	/* G6b F3: the lava hit arms the window at Entity.onEntityUpdate:218 and
	 * EntityLiving.java:151 decrements it once later in the SAME tick. */
	assert(s.player.last_damage == 4 * (HEALTH_PER_HEART / 2)
		   && s.player.hurt_resist_time == PLAYER_HURT_RESIST_TICKS - 1);
	assert(s.player.fire == -PLAYER_FIRE_RESISTANCE);
	assert(s.player.fall_distance == 0.0F);
	set(7, contact_y, 7, BLOCK_AIR);
	set(8, contact_y, 8, BLOCK_AIR);
	puts("  fluids: simultaneous body water quenches, lava damages and ignites, the wet check extinguishes in the same tick OK");
}

static void test_invalid_position_rpc_rejected(void) {
	reset_player(8.0, 74.0, 8.0);
	s.player.rx = 0.25f;
	s.player.ry = 0.5f;
	const struct server_rpc invalid[] = {
		{.type = SRPC_PLAYER_POS,
		 .payload.player_pos = {.x = NAN, .y = 70.0, .z = 8.0}},
		{.type = SRPC_PLAYER_POS,
		 .payload.player_pos = {.x = 8.0, .y = INFINITY, .z = 8.0}},
		{.type = SRPC_PLAYER_POS,
		 .payload.player_pos = {.x = 30000001.0, .y = 70.0, .z = 8.0}},
		{.type = SRPC_PLAYER_POS,
		 .payload.player_pos = {.x = 8.0, .y = 70.0, .z = 8.0, .rx = NAN}},
		{.type = SRPC_PLAYER_POS,
		 .payload.player_pos
			= {.x = 8.0, .y = 70.0, .z = 8.0, .vel_y = INFINITY}},
	};
	for(size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
		struct server_rpc call = invalid[i];
		server_local_test_process_rpc(&s, &call);
		assert(s.player.x == 8.0 && s.player.y == 74.0 && s.player.z == 8.0);
		assert(s.player.rx == 0.25f && s.player.ry == 0.5f);
	}
	puts("  position RPC: NaN/Inf/out-of-range payloads rejected before mutation OK");
}

// 2a. Horizontal blast: distance-scaled magnitude, NOT the fixed 0.4.
static void test_blast_horizontal(void) {
	reset_player(14.0, 70.0, 10.0); // 4 blocks +x of the blast at (10,70,10)
	vec3 center = {10.0f, 70.0f, 10.0f};
	server_world_begin_update(&s.world);
	server_world_explode(&s.worlds[0], center, 3.0f); // power 3 -> radius 6
	// pdist 4, mag = 1 - 4/6 = 0.3333; dir +x -> vel_x = 0.3333, vel_z = 0.
	assert(fabsf(s.player.vel_x - 0.33333f) < 0.01f);
	assert(fabsf(s.player.vel_z) < 1.0e-4f);
	assert(fabsf(s.player.vel_x - KNOCKBACK_H) > 0.05f); // NOT the old fixed 0.4
	assert(s.player.health < MAX_PLAYER_HEALTH);          // and it dealt (non-lethal) damage
	printf("  blast knockback: 4 blocks -> vel_x ~0.333 (distance-scaled, not 0.4) OK\n");
}

// 2b. Vertical blast: the shove has a Y component the old horizontal-only path dropped.
static void test_blast_vertical(void) {
	reset_player(10.0, 74.0, 10.0); // 4 blocks ABOVE the blast at (10,70,10)
	double y0 = s.player.y;
	s.player.motion_y = 0.0;
	vec3 center = {10.0f, 70.0f, 10.0f};
	server_world_begin_update(&s.world);
	server_world_explode(&s.worlds[0], center, 3.0f);
	// pdy 4, pdist 4, mag 0.3333 -> the Y impulse is ~0.333 (old code: 0, Y
	// dropped). G8-BLAST-VELOCITY: Explosion.doExplosionA:101-103 adds it to
	// the SERVER motionY (delivered to the client by the tracker's Packet28);
	// the server never moves the body (NetServerHandler keeps the client's
	// position). Re-derived from the v1-era "y += impulse" shove.
	assert(s.player.y == y0);                         // no position shove
	assert(s.player.motion_y > 0.2);                  // launched upward (motion)
	assert(fabs(s.player.motion_y - 0.33333) < 0.05);
	assert(fabsf(s.player.vel_x) < 1.0e-4f && fabsf(s.player.vel_z) < 1.0e-4f);
	printf("  blast knockback: directly above -> vertical launch ~0.333 (3D, was 0) OK\n");
}

// 2c. LETHAL blast: the player dies + respawns to spawn; the post-death blast shove is
//     SKIPPED (regression guard for the split damage/knockback path). server_local_damage_
//     player repositions to spawn + zeroes vel_y on death and returns false; applying the
//     3D shove afterward would bump the fresh respawn Y off-spawn and leave a residual
//     velocity pointing away from the OLD blast center.
static void test_blast_lethal(void) {
	// Stay within the loaded chunk region [0,32): player at (20,71,20), spawn DISTINCT at
	// block (8,71,8). A power-3 point-blank blast (radius 6, breaks blocks x/z in [14,26]) is far
	// over the 160-pt health budget (half_hearts*8 ~= 320), so it is lethal at full health.
	reset_player(20.0, 71.0, 20.0);                   // player HERE...
	s.player.spawn_x = 8; s.player.spawn_y = 71; s.player.spawn_z = 8; // ...spawn ELSEWHERE
	s.world.dimension = WORLD_DIM_NETHER;
	s.player.dimension = WORLD_DIM_NETHER;
	vec3 center = {20.4f, 70.4f, 20.0f};              // pdy 0.6 -> a Y shove the bug would add
	server_world_begin_update(&s.world);
	server_world_explode(&s.worlds[0], center, 3.0f);          // power 3 -> radius 6, point-blank lethal
	// Deviation-#18 split: the blast commits only the DEAD state; the corpse
	// keeps its position and the post-death shove is skipped (the dead-guard
	// absorbs everything after the lethal commit). The respawn transaction
	// then owns the fresh body.
	assert(s.player.health <= 0);
	assert(server_local_respawn_request(&s));
	// Respawned to the DISTINCT spawn (proves the death path ran), full health, and the
	// post-death shove was skipped: centered eye position + no stray knockback velocity.
	assert(s.player.health == MAX_PLAYER_HEALTH);     // respawned full
	assert(s.player.dimension == WORLD_DIM_OVERWORLD);
	assert(s.death_world_evict); // Nether world switch is deferred out of entity iteration
	assert(fabs(s.player.x - 8.5) < 1.0e-4 && fabs(s.player.z - 8.5) < 1.0e-4);
	assert(fabs(s.player.y - (71.0 + PLAYER_EYE_HEIGHT)) < 1.0e-4);
	assert(fabsf(s.player.vel_x) < 1.0e-4f);          // no residual blast knockback (~-0.5 if buggy)
	assert(fabsf(s.player.vel_z) < 1.0e-4f);
	s.player.finished_loading = true; // settle the loading epoch for later sections
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	s.world.dimension = WORLD_DIM_OVERWORLD;
	s.death_world_evict = false;
	printf("  blast knockback: LETHAL blast -> clean respawn to spawn (no post-death shove) OK\n");
}

static void test_personal_bed_respawn_resolution(void) {
	const int hx = 10, hy = 70, hz = 10;
	/* Cave metadata 8 is a +Z head. Beta's deterministic first candidate is
	 * x-1,z-1 when that cell has a normal floor and two air blocks. */
	assert(server_world_set_block_silent(
		&s.worlds[0], hx, hy, hz,
		(struct block_data) {.type = BLOCK_BED, .metadata = 8}));
	set(hx - 1, hy - 1, hz - 1, BLOCK_STONE);
	set(hx - 1, hy, hz - 1, BLOCK_AIR);
	set(hx - 1, hy + 1, hz - 1, BLOCK_AIR);
	reset_player(20.0, 71.0, 20.0);
	s.player.spawn_x = 4;
	s.player.spawn_y = 5;
	s.player.spawn_z = 4;
	s.player.has_bed_spawn = true;
	s.player.bed_spawn_x = hx;
	s.player.bed_spawn_y = hy;
	s.player.bed_spawn_z = hz;
	s.world.dimension = WORLD_DIM_OVERWORLD;
	s.player.dimension = WORLD_DIM_OVERWORLD;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	assert(server_local_set_player_health(&s, 0));
	assert(server_local_respawn_request(&s));
	assert(s.player.has_bed_spawn);
	assert(fabs(s.player.x - (hx - 1 + 0.5)) < 1.0e-6);
	assert(fabs(s.player.y - (hy + PLAYER_EYE_HEIGHT + 0.1)) < 1.0e-6);
	assert(fabs(s.player.z - (hz - 1 + 0.5)) < 1.0e-6);

	/* With Cave +Z metadata the foot-centered pass is one block toward -Z.
	 * Its z=hz-2 edge is outside the head-centered 3x3, so this sole safe cell
	 * proves the second pass really runs and uses the decoded direction. */
	for(int x = hx - 1; x <= hx + 1; x++)
		for(int z = hz - 2; z <= hz + 1; z++)
			set(x, hy - 1, z, BLOCK_AIR);
	set(hx - 1, hy - 1, hz - 2, BLOCK_STONE);
	reset_player(20.0, 71.0, 20.0);
	s.player.spawn_x = 4;
	s.player.spawn_y = 5;
	s.player.spawn_z = 4;
	s.player.has_bed_spawn = true;
	s.player.bed_spawn_x = hx;
	s.player.bed_spawn_y = hy;
	s.player.bed_spawn_z = hz;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	assert(server_local_set_player_health(&s, 0));
	assert(server_local_respawn_request(&s));
	assert(s.player.has_bed_spawn);
	assert(s.player.x == hx - 1 + 0.5 && s.player.z == hz - 2 + 0.5);

	/* Beta's nested loops are x-outer, z-inner. These two head-pass cells make
	 * that observable: x=hx-1,z=hz+1 wins over x=hx,z=hz-1. */
	for(int x = hx - 1; x <= hx + 1; x++)
		for(int z = hz - 2; z <= hz + 1; z++)
			set(x, hy - 1, z, BLOCK_AIR);
	set(hx - 1, hy - 1, hz + 1, BLOCK_STONE);
	set(hx, hy - 1, hz - 1, BLOCK_STONE);
	reset_player(20.0, 71.0, 20.0);
	s.player.spawn_x = 4;
	s.player.spawn_y = 5;
	s.player.spawn_z = 4;
	s.player.has_bed_spawn = true;
	s.player.bed_spawn_x = hx;
	s.player.bed_spawn_y = hy;
	s.player.bed_spawn_z = hz;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	assert(server_local_set_player_health(&s, 0));
	assert(server_local_respawn_request(&s));
	assert(s.player.has_bed_spawn);
	assert(s.player.x == hx - 1 + 0.5 && s.player.z == hz + 1 + 0.5);

	/* Beta +X head metadata (8 | dir 3) at the x=16 chunk boundary. Leave the earlier scan cells
	 * unsupported so resolution must cross from chunk 0 into chunk 1. */
	const int boundary_hx = 16;
	assert(server_world_set_block_silent(
		&s.worlds[0], boundary_hx, hy, hz,
		(struct block_data) {.type = BLOCK_BED, .metadata = 11}));
	set(boundary_hx + 1, hy - 1, hz - 1, BLOCK_STONE);
	reset_player(20.0, 71.0, 20.0);
	s.player.has_bed_spawn = true;
	s.player.bed_spawn_x = boundary_hx;
	s.player.bed_spawn_y = hy;
	s.player.bed_spawn_z = hz;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	assert(server_local_set_player_health(&s, 0));
	assert(server_local_respawn_request(&s));
	assert(s.player.has_bed_spawn);
	assert(s.player.x == boundary_hx + 1.5
		   && s.player.z == hz - 1 + 0.5);

	/* Removing the exact stored head invalidates the personal spawn and falls
	 * back to permanent world SpawnXYZ. */
	assert(server_world_set_block_silent(
		&s.worlds[0], hx, hy, hz, (struct block_data) {.type = BLOCK_AIR}));
	reset_player(20.0, 71.0, 20.0);
	s.player.spawn_x = 4;
	s.player.spawn_y = 5;
	s.player.spawn_z = 4;
	s.player.has_bed_spawn = true;
	s.player.bed_spawn_x = hx;
	s.player.bed_spawn_y = hy;
	s.player.bed_spawn_z = hz;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	assert(server_local_set_player_health(&s, 0));
	assert(server_local_respawn_request(&s));
	assert(!s.player.has_bed_spawn);
	assert(s.player.x == 4.5 && s.player.y == 5 + PLAYER_EYE_HEIGHT
		   && s.player.z == 4.5);

	/* An intact head with normal floors but solid feet/headroom at every
	 * candidate is genuinely blocked. It must clear the personal spawn rather
	 * than treating a non-air cell as safe. */
	assert(server_world_set_block_silent(
		&s.worlds[0], hx, hy, hz,
		(struct block_data) {.type = BLOCK_BED, .metadata = 8}));
	for(int x = hx - 1; x <= hx + 1; x++)
		for(int z = hz - 2; z <= hz + 1; z++) {
			set(x, hy - 1, z, BLOCK_STONE);
			if(x != hx || z != hz)
				set(x, hy, z, BLOCK_STONE);
			set(x, hy + 1, z, BLOCK_STONE);
		}
	reset_player(20.0, 71.0, 20.0);
	s.player.spawn_x = 4;
	s.player.spawn_y = 5;
	s.player.spawn_z = 4;
	s.player.has_bed_spawn = true;
	s.player.bed_spawn_x = hx;
	s.player.bed_spawn_y = hy;
	s.player.bed_spawn_z = hz;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	assert(server_local_set_player_health(&s, 0));
	assert(server_local_respawn_request(&s));
	assert(!s.player.has_bed_spawn);
	assert(s.player.x == 4.5 && s.player.y == 5 + PLAYER_EYE_HEIGHT
		   && s.player.z == 4.5);
	for(int x = hx - 1; x <= hx + 1; x++)
		for(int z = hz - 2; z <= hz + 1; z++) {
			set(x, hy - 1, z, BLOCK_AIR);
			set(x, hy, z, BLOCK_AIR);
			set(x, hy + 1, z, BLOCK_AIR);
		}
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	puts("  respawn: exact two-pass order; removed/obstructed bed clears + falls back");
}

static void test_respawn_is_fresh_entity_and_stops_old_hazards(void) {
	reset_player(20.0, 71.0, 20.0);
	s.player.spawn_x = 8;
	s.player.spawn_y = 71;
	s.player.spawn_z = 8;
	s.player.oxygen = -7;
	s.player.fire = 123;
	s.player.vel_x = 0.25f;
	s.player.vel_y = -0.75f;
	s.player.old_vel_y = -0.5f;
	s.player.vel_z = -0.125f;
	s.player.fall_distance = 25.5F;
	s.player.hurt_resist_time = 7;
	s.player.last_damage = 40;
	s.player.distance_walked = 9.0f;
	s.player.next_step_distance = 6;
	s.player.time_until_portal = 0;
	s.player.time_in_portal = 1.0f;
	s.player.in_portal = true;
	s.player.portal_travel_requested = true;
	assert(server_local_set_player_health(&s, 0));
	assert(server_local_respawn_request(&s));
	assert(s.player.health == MAX_PLAYER_HEALTH);
	assert(s.player.oxygen == MAX_OXYGEN);
	assert(s.player.fire == -PLAYER_FIRE_RESISTANCE);
	assert(s.player.vel_x == 0.0f && s.player.vel_y == 0.0f
		   && s.player.old_vel_y == 0.0f && s.player.vel_z == 0.0f);
	assert(s.player.hurt_resist_time == 0 && s.player.last_damage == 0);
	assert(s.player.fall_distance == 0.0F);
	assert(s.player.distance_walked == 0.0f && s.player.next_step_distance == 1);
	assert(s.player.time_until_portal == 20 && s.player.time_in_portal == 0.0f
		   && !s.player.in_portal && !s.player.portal_travel_requested);

	/* Lava and cactus flags were sampled at one old position. A lethal lava hit
	 * must terminate the sweep; otherwise cactus immediately damages the fresh
	 * respawn after its i-frame state has correctly reset. */
	reset_player(20.0, 71.0, 20.0);
	s.player.spawn_x = 8;
	s.player.spawn_y = 71;
	s.player.spawn_z = 8;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE; /* prior respawn's epoch */
	s.player.health = 4 * (HEALTH_PER_HEART / 2);
	assert(!server_local_test_apply_player_hazards(
		&s, false, true, false, true));
	assert(s.player.health <= 0); /* dead commit; the sweep stopped */
	assert(server_local_respawn_request(&s));
	assert(s.player.health == MAX_PLAYER_HEALTH);
	assert(s.player.fire == -PLAYER_FIRE_RESISTANCE);

	/* Drowning's oxygen=0 write precedes damage in Beta, but the replacement
	 * entity starts with maxAir and must not inherit that exhausted counter. */
	reset_player(20.0, 71.0, 20.0);
	s.player.spawn_x = 8;
	s.player.spawn_y = 71;
	s.player.spawn_z = 8;
	s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE; /* prior respawn's epoch */
	s.player.health = HEALTH_PER_HEART;
	s.player.oxygen = OXYGEN_THRESHOLD;
	uint32_t deaths_before = s.player_deaths;
	assert(!server_local_test_apply_player_hazards(
		&s, true, false, false, false));
	assert(s.player.health <= 0); /* dead commit; the sweep stopped */
	assert(s.player_deaths == deaths_before + 1
		&& s.player_death_cause == PLAYER_DAMAGE_DROWN);
	assert(server_local_test_apply_player_hazards(
		&s, true, false, false, false));
	assert(s.player_deaths == deaths_before + 1); /* corpse is not another death */
	assert(server_local_respawn_request(&s));
	assert(s.player_deaths == deaths_before + 1
		&& !strcmp(server_player_damage_cause_name(s.player_death_cause), "drown"));
	assert(s.player.health == MAX_PLAYER_HEALTH);
	assert(s.player.oxygen == MAX_OXYGEN);
	printf("  respawn: fresh entity state; lethal old-position hazard sweep stops exactly once OK\n");
}

// Re-pin a slime + the player to a fixed contact geometry before each tick: reset the
// slime's POSITION AND VELOCITY (else leftover gravity velocity descends it out of reach
// over successive ticks) and ground it, and undo any knockback pop on the player so the
// feet stay aligned (player EYE 71.62 -> feet 70 == slime feet 70 -> dy 0).
static void pin_contact(struct entity* sl, float sx, float sz, double px, double pz) {
	sl->pos[0] = sx; sl->pos[1] = 70.0f; sl->pos[2] = sz;
	sl->vel[0] = 0.0f; sl->vel[1] = 0.0f; sl->vel[2] = 0.0f;
	sl->on_ground = true;
	test_mob_sync(sl); /* motion units S2: the fixture pins the doubles */
	s.player.x = px; s.player.y = 71.62; s.player.z = pz;
	s.player.vel_x = 0.0f; s.player.vel_z = 0.0f;
}

// 3. Harmful slime CONTACT damage (Beta EntitySlime.onCollideWithPlayer): hits on the
//    first contact tick (NO windup), re-hits as soon as the player's i-frames clear (NO
//    20-tick attack cooldown), and only within 0.6*size.
static void test_slime_contact(void) {
	// A small stone platform under the contact site so a grounded slime stays put.
	for(int x = 8; x <= 13; x++)
		for(int z = 8; z <= 13; z++)
			set(x, 69, z, BLOCK_STONE);
	reset_player(10.0, 71.62, 10.0);
	vec3 spos = {10.8f, 70.0f, 10.0f}; // 0.8 away, inside medium reach 0.6*2 = 1.2
	struct entity* sl = server_local_spawn_monster(spos, MONSTER_SLIME_MEDIUM, &s);
	assert(sl && sl->tick_server);
	short hp0 = s.player.health;

	// First contact tick: immediate hit, NO 8-tick windup (the EntityMob path had one).
	pin_contact(sl, 10.8f, 10.0f, 10.0, 10.0);
	sl->tick_server(sl, &s);
	assert(s.player.health < hp0);
	short hp1 = s.player.health;

	// Immediately again: the player's i-frames absorb (no extra damage).
	pin_contact(sl, 10.8f, 10.0f, 10.0, 10.0);
	sl->tick_server(sl, &s);
	assert(s.player.health == hp1);

	// Once the i-frames clear there is NO slime-side attack cooldown -> it re-hits on the
	// very next contact tick (the old 20-tick EntityMob attack_time would have blocked it).
	s.player.hurt_resist_time = 0; s.player.last_damage = 0;
	pin_contact(sl, 10.8f, 10.0f, 10.0, 10.0);
	sl->tick_server(sl, &s);
	assert(s.player.health < hp1);
	printf("  slime contact: first-tick hit (no windup) + re-hit when i-frames clear (no cooldown) OK\n");

	// Outside the 1.2 reach (1.5 away): no contact damage.
	s.player.hurt_resist_time = 0; s.player.last_damage = 0;
	short hp2 = s.player.health;
	pin_contact(sl, 11.5f, 10.0f, 10.0, 10.0); // 1.5 away
	sl->tick_server(sl, &s);
	assert(s.player.health == hp2);
	printf("  slime contact: 1.5 > medium 0.6*2=1.2 reach -> no hit OK\n");
}

int main(void) {
	test_fixture_dir("test_player_combat_env_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	assert(svin_init());
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	// Mirror server_local_create: a real, capacity-INVENTORY_SIZE player inventory so the
	// lethal-blast test's death/respawn path (which iterates + drops every slot) is valid.
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;

	printf("test_player_combat_env:\n");
	test_batched_position_samples_preserve_landing();
	test_fractional_landing_excludes_final_component();
	test_airborne_velocity_crossing_is_not_landing();
	test_grounded_flag_bills_without_velocity_crossing();
	test_dropped_airborne_samples_preserve_fall();
	test_body_water_cancels_fall();
	test_nondescending_water_sample_cancels_batched_fall();
	test_body_side_lava_contact();
	test_body_water_and_lava_both_apply();
	test_invalid_position_rpc_rejected();
	test_iframe_excess();
	test_blast_damage_precedes_blocks();
	test_blast_horizontal();
	test_blast_vertical();
	test_blast_lethal();
	test_personal_bed_respawn_resolution();
	test_respawn_is_fresh_entity_and_stops_old_hazards();
	test_slime_contact();

	g_drain_run = 0;
	thread_join(&drain);
	string_clear(level);
	printf("PASS test_player_combat_env\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
