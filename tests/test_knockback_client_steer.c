/* Copyright (c) 2026 CaveWii_v2 contributors
 * G8 (vehicle-smoke-1004): knockback is delivered as CLIENT velocity on local
 * sessions -- the player keeps steering through the push and the server no
 * longer snaps the body's position.
 *
 * Beta's path (CFR server decompile + Jad/retromcp client):
 *   - EntityLiving.knockBack [EntityLiving.java:392-403] writes motion only;
 *     EntityTrackerEntry [:195-198] sends Packet28EntityVelocity (x8000,
 *     clamped +-3.9, Packet28EntityVelocity.java:29-35) to the tracked
 *     entity ITSELF; the client's NetClientHandler.handleEntityVelocity
 *     [:165-170] -> Entity.setVelocity [:1000-1004] assigns all three.
 *   - The client's EntityPlayerSP then runs moveEntityWithHeading
 *     [EntityLiving.java:440-487]: moveFlying adds the stick input, moveEntity
 *     moves by the motion, then motion *= friction (0.91F in air,
 *     slipperiness * 0.91F on the ground -- 0.54600006F on stone).
 *   - The server never moves the player for it: NetServerHandler.handleFlying
 *     puts the body where the CLIENT says (setPositionAndRotation,
 *     NetServerHandler.java:172/257); EntityPlayerMP's own motion only decays.
 *   - Server pushes without beenAttacked (Entity.addVelocity) ride the same
 *     message as an ADD (lead ruling 2026-10-05).
 *
 * Section S: hit -> one velocity RPC carrying X/Y/Z; N integrator ticks send
 *   NO position correction and leave the server body where it was; the
 *   server copy decays by Beta's friction. An addVelocity push sends an
 *   additive RPC.
 * Section C: the host client installs X/Z (and adds an additive push) and,
 *   while FORWARD is held, both travels Beta's horizontal knockback distance
 *   AND advances along its own facing (it steers through the push).
 */
#include "support/split_residency_fixture.h"
#include "entity/entity.h"
#include "entity/entity_local_player.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "item/items.h"
#include "particle.h"
#include "world.h"
#include <math.h>
#include <stdio.h>

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

#define EYE 1.62F
#define FLOOR_Y 79

static unsigned velocity_rpcs, pos_rpcs;
static struct client_rpc last_velocity;
static void observe(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_LOCAL_PLAYER_VELOCITY) {
		velocity_rpcs++;
		last_velocity = *call;
	} else if(call->type == CRPC_PLAYER_POS) {
		pos_rpcs++;
	}
}

/* Q(v) = trunc(clamp(v, -3.9, 3.9) * 8000) / 8000 (Packet28 round trip). */
static float q28(double v) {
	if(v < -3.9)
		v = -3.9;
	if(v > 3.9)
		v = 3.9;
	return (float)(int32_t)(v * 8000.0) / 8000.0f;
}

static void floor_under(int cx, int cz) {
	for(w_coord_t x = cx - 8; x <= cx + 8; x++)
		for(w_coord_t z = cz - 8; z <= cz + 8; z++) {
			world_set_block(&gstate.world, x, FLOOR_Y, z,
				(struct block_data) {.type = BLOCK_STONE}, false);
			for(w_coord_t y = FLOOR_Y + 1; y <= FLOOR_Y + 6; y++)
				world_set_block(&gstate.world, x, y, z,
					(struct block_data) {.type = BLOCK_AIR}, false);
		}
}

static void settle(struct entity* p, float x, float z) {
	p->pos[0] = x;
	p->pos[1] = (float)(FLOOR_Y + 1) + EYE + 0.5F;
	p->pos[2] = z;
	glm_vec3_copy(p->pos, p->pos_old);
	glm_vec3_zero(p->vel);
	p->orient[0] = 0.0F; /* yaw 0: moveFlying's forward is +Z only */
	p->orient[1] = 0.0F;
	p->on_ground = false;
	for(unsigned t = 0; t < 40; t++)
		(void)p->tick_client(p);
}

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();

	/* ---- S: the server delivers and stops moving the body ---------------- */
	struct server_local* s = split_fixture();
	struct entity attacker = {0};
	attacker.type = ENTITY_MONSTER;
	attacker.data.monster.id = MONSTER_ZOMBIE;
	attacker.pos[0] = (float)s->player.x + 0.6f;
	attacker.pos[2] = (float)s->player.z;
	/* G8-BLAST-VELOCITY v3: server motion (double, separate from the client
	 * fall sample) from rest; the fixture column under the body is empty
	 * and the event uses air friction (onGround false). */
	s->player.vel_x = s->player.vel_z = 0.0;
	s->player.motion_y = -0.08 * (double)0.98F;
	s->player.vel_y = (0.0F - 0.08F) * 0.98F;
	s->player.hurt_resist_time = 0;
	s->player.on_ground = false; /* air friction 0.91F: no block lookup */
	clin_test_set_rpc_observer(observe, NULL);
	server_local_hurt_player(s, &attacker);
	const uint32_t seq0 = s->player.correction_seq;
	check(velocity_rpcs == 0 && s->player.been_attacked,
		  "S: the hit sends NOTHING itself; beenAttacked is raised");
	const double kdx = (double)attacker.pos[0] - s->player.x;
	const double kdz = (double)attacker.pos[2] - s->player.z;
	const float klen = (float)sqrt(kdx * kdx + kdz * kdz);
	double wx = 0.0 / 2.0 - kdx / (double)klen * (double)0.4F;
	double wz = 0.0 / 2.0 - kdz / (double)klen * (double)0.4F;
	double wy = fmin(-0.08 * (double)0.98F / 2.0 + (double)0.4F, (double)0.4F);
	check(s->player.vel_x == wx && s->player.vel_z == wz
			  && s->player.motion_y == wy,
		  "S: knockBack's server motion, from independent state");
	const double x0 = s->player.x, z0 = s->player.z;
	pos_rpcs = 0;
	for(unsigned t = 0; t < 30; t++) {
		server_local_tick_actor_knockback(s); /* one accepted sample */
		server_local_flush_actor_velocity(s);  /* the tracker phase */
		clin_drain_discard(); /* a mutant's corrections must not block */
		/* Independent recurrence (EntityLiving.moveEntityWithHeading air
		 * branch + Entity.moveEntity): the fixture column under the body is
		 * empty, so every move is collision-free -- pure gravity/drag on Y,
		 * air friction on X/Z. */
		wy = (wy - 0.08) * (double)0.98F;
		wx *= (double)0.91F;
		wz *= (double)0.91F;
		if(t == 0)
			check(velocity_rpcs == 1
					  && !last_velocity.payload.local_player_velocity.add
					  && last_velocity.payload.local_player_velocity.owner == 0
					  && last_velocity.payload.local_player_velocity.vel[0] == q28(wx)
					  && last_velocity.payload.local_player_velocity.vel[1] == q28(wy)
					  && last_velocity.payload.local_player_velocity.vel[2] == q28(wz),
				  "S: ONE absolute host-route delivery of Q(post-step X/Y/Z)");
	}
	printf("  server body dx=%.6f dz=%.6f corrections=%u vel_x=%.9g want=%.9g\n",
		   s->player.x - x0, s->player.z - z0, pos_rpcs,
		   s->player.vel_x, wx);
	check(velocity_rpcs == 1,
		  "S: no further send without a new fresh hit");
	check(pos_rpcs == 0 && s->player.correction_seq == seq0,
		  "S: no CRPC_PLAYER_POS snap, no correction token change");
	check(s->player.x == x0 && s->player.z == z0,
		  "S: the server does not move the body (client samples rule)");
	check(s->player.vel_x == wx && s->player.vel_z == wz
			  && s->player.motion_y == wy,
		  "S: the server motion follows Beta's recurrence exactly (double, no cutoff)");
	/* Characterization of the SEPARATE, non-Beta G8 additive push (cart
	 * shove / fishing pull; follow-up card): not a tracker send. */
	velocity_rpcs = 0;
	server_local_push_actor_velocity(s, &s->player, 0.0125F, -0.005F);
	check(velocity_rpcs == 1 && last_velocity.payload.local_player_velocity.add
			  && last_velocity.payload.local_player_velocity.vel[0] == 0.0125F
			  && last_velocity.payload.local_player_velocity.vel[1] == 0.0F
			  && last_velocity.payload.local_player_velocity.vel[2] == -0.005F
			  && last_velocity.payload.local_player_velocity.owner == 0,
		  "S: an addVelocity push is delivered as an ADD on the host route");
	clin_test_set_rpc_observer(NULL, NULL);
	clin_drain_discard();
	split_destroy(s);

	/* ---- C: the client steers through the push ---------------------------- */
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	struct client_rpc reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD,
								.local_entity = SERVER_LOCAL_PLAYER_ENTITY_ID},
	};
	clin_process(&reset);
	struct client_session* hs = &gstate.players[0];
	struct entity* host = hs->local_player;
	assert(host && host->tick_client);
	floor_under(8, 8);
	settle(host, 8.5F, 8.5F);
	const float m0x = -0.4F, m0y = 0.3608F;
	struct client_rpc hit = {.type = CRPC_LOCAL_PLAYER_VELOCITY,
		.payload.local_player_velocity = {.owner = 0,
			.vel = {m0x, m0y, 0.0F}}};
	clin_process(&hit);
	check(host->vel[0] == m0x && host->vel[1] == m0y && host->vel[2] == 0.0F,
		  "C: the local host client installs X, Y and Z (setVelocity)");
	struct client_rpc push = {.type = CRPC_LOCAL_PLAYER_VELOCITY,
		.payload.local_player_velocity = {.owner = 0,
			.vel = {0.01F, 0.0F, 0.0F}, .add = true}};
	clin_process(&push);
	check(host->vel[0] == m0x + 0.01F && host->vel[1] == m0y,
		  "C: an additive push extends the motion (addVelocity)");

	struct input_player_frame frame = {.connected = true};
	frame.down[IB_FORWARD] = true;
	host->data.local_player.capture_input = true;
	const float hx0 = host->pos[0], hz0 = host->pos[2];
	/* Beta's horizontal X: x += m (moveEntity), then m *= friction chosen by
	 * onGround BEFORE the move (EntityLiving.java:450-457, 480, 486-487).
	 * Yaw 0 puts the stick's moveFlying entirely on +Z, so X is the push. */
	double mx = (double)(m0x + 0.01F), want_dx = 0.0;
	for(unsigned t = 0; t < 40; t++) {
		const bool ground = host->on_ground;
		(void)entity_local_player_walk_owned(host, hs, &frame);
		want_dx += mx;
		mx = (double)(float)((float)mx * (ground ? 0.54600006F : 0.91F));
	}
	const double dx = (double)(host->pos[0] - hx0);
	const double dz = (double)(host->pos[2] - hz0);
	printf("  client dx=%.6f (Beta %.6f) dz=%.6f\n", dx, want_dx, dz);
	check(fabs(dx - want_dx) < 1e-3,
		  "C: the body travels Beta's horizontal knockback distance");
	check(dz > 0.5,
		  "C: and keeps advancing along its own facing (steers through it)");

	if(failures) {
		printf("RED test_knockback_client_steer: %u failing clause(s)\n",
			   failures);
		return 1;
	}
	puts("PASS test_knockback_client_steer: knockback and pushes are client "
		 "velocity; the server neither moves nor snaps the body "
		 "(EntityTrackerEntry.java:195-198, NetClientHandler.java:165-170, "
		 "NetServerHandler.java:172/257)");
	return 0;
}
