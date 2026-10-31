/* Copyright (c) 2026 CaveWii_v2 contributors
 * damage-client-0926: a knocked-back local body RISES to Beta's height.
 *
 * Beta's whole path (decompile ../mcb173-retromcp, client + server trees):
 *   1. EntityLiving.attackEntityFrom (EntityLiving.java:333-346): on a fresh
 *      hit, setBeenAttacked() and knockBack(attacker, dmg, dx, dz).
 *   2. EntityLiving.knockBack (:392-403): motion /= 2, motionX/Z -= d/len*0.4F,
 *      motionY += 0.4F, then motionY capped at 0.4F. No position change.
 *   3. EntityTrackerEntry.updatePlayerList (server, :124-127): beenAttacked ->
 *      Packet28EntityVelocity to the tracked players AND the tracked entity
 *      itself.
 *   4. NetClientHandler.handleEntityVelocity (:165-170) -> Entity.setVelocity
 *      (:1000-1004): plain assignment on the client player, whose next
 *      moveEntityWithHeading moves it by motionY, then motionY -= 0.08D;
 *      motionY *= 0.98F (EntityLiving.java:528-535).
 * So a body resting on the ground (motionY = -0.08 * 0.98 after its last tick)
 * is thrown up by motionY = -0.0784/2 + 0.4 = 0.3608 and peaks ~0.965 blocks
 * above its rest height. The old engine popped the position by +0.4 once
 * (peak 0.4); the server-only half of the fix wrote motionY but nothing
 * delivered it, so the body did not rise at all.
 *
 * Section S: the server hit emits the Beta motion to the HIT body's own
 *   client, host untagged (owner 0). Section C (host) and G (guest): the
 *   client installs it and the ordinary local-player physics integrates it to
 *   Beta's peak. Non-aborting checks so a RED run lists every failing clause.
 */
#include "support/split_residency_fixture.h"
#include "entity/entity.h"
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

/* entity_local_player.c's EYE_HEIGHT: pos[1] is the eye. */
#define EYE 1.62F
#define FLOOR_Y 79 /* stone at y=79, feet rest on 80 */

static unsigned velocity_rpcs;
static struct client_rpc last_velocity;
static void observe(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_LOCAL_PLAYER_VELOCITY) {
		velocity_rpcs++;
		last_velocity = *call;
	}
}

/* Beta's motionY integration from m0, in Java's double arithmetic
 * (EntityLiving.moveEntityWithHeading:528-535), with no ceiling in the way:
 * the rise until motionY turns non-positive. */
static double beta_peak_rise(double m0) {
	double rise = 0.0, m = m0;
	while(m > 0.0) {
		rise += m;
		m -= 0.08;
		m *= (double)0.98F;
	}
	return rise;
}

/* G8-BLAST-VELOCITY v3 (ASTRA-V2-REVIEW (1)): Packet28 round trip
 * Q(v) = trunc(clamp(v, -3.9, 3.9) * 8000) / 8000, applied only at
 * transmission. */
static float q28(double v) {
	if(v < -3.9)
		v = -3.9;
	if(v > 3.9)
		v = 3.9;
	return (float)(int32_t)(v * 8000.0) / 8000.0f;
}

static void floor_under(int cx, int cz) {
	for(w_coord_t x = cx - 3; x <= cx + 3; x++)
		for(w_coord_t z = cz - 3; z <= cz + 3; z++) {
			world_set_block(&gstate.world, x, FLOOR_Y, z,
				(struct block_data) {.type = BLOCK_STONE}, false);
			for(w_coord_t y = FLOOR_Y + 1; y <= FLOOR_Y + 6; y++)
				world_set_block(&gstate.world, x, y, z,
					(struct block_data) {.type = BLOCK_AIR}, false);
		}
}

/* Rest the body on the floor; returns the resting feet height. */
static float settle(struct entity* p, float x, float z) {
	p->pos[0] = x;
	p->pos[1] = (float)(FLOOR_Y + 1) + EYE + 0.5F;
	p->pos[2] = z;
	glm_vec3_copy(p->pos, p->pos_old);
	glm_vec3_zero(p->vel);
	p->on_ground = false;
	for(unsigned t = 0; t < 40; t++)
		(void)p->tick_client(p);
	return p->pos[1] - EYE;
}

/* Tick until the body is back on the ground; returns the peak feet rise. */
static float rise_after(struct entity* p, float rest_feet) {
	float peak = 0.0F;
	for(unsigned t = 0; t < 40; t++) {
		(void)p->tick_client(p);
		float rise = (p->pos[1] - EYE) - rest_feet;
		if(rise > peak)
			peak = rise;
	}
	return peak;
}

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();

	/* ---- S: the server hit emits Beta's motion to the host client -------- */
	struct server_local* s = split_fixture();
	struct entity attacker = {0};
	attacker.type = ENTITY_MONSTER;
	attacker.data.monster.id = MONSTER_ZOMBIE;
	attacker.pos[0] = (float)s->player.x + 0.6f;
	attacker.pos[2] = (float)s->player.z;
	/* What a resting client reports: one tick of Beta gravity from 0. */
	const float rest_vel_y = (0.0F - 0.08F) * 0.98F;
	/* G8-BLAST-VELOCITY v3: the SERVER motionY is seeded independently of
	 * the client fall sample (Packet10 carries no motionY): Beta's resting
	 * EntityPlayerMP motion after one air step from 0. The client sample is
	 * set to an unrelated value to prove knockBack never reads it. */
	const double rest = -0.08 * (double)0.98F;
	s->player.vel_x = s->player.vel_z = 0.0;
	s->player.motion_y = rest;
	s->player.vel_y = 0.123F;
	s->player.on_ground = false; /* one collision-free AIR step below */
	s->player.hurt_resist_time = 0;
	const double y_before = s->player.y;
	clin_test_set_rpc_observer(observe, NULL);
	server_local_hurt_player(s, &attacker);
	const unsigned at_hit = velocity_rpcs;
	const double kdx = (double)attacker.pos[0] - s->player.x;
	const double kdz = (double)attacker.pos[2] - s->player.z;
	const float klen = (float)sqrt(kdx * kdx + kdz * kdz);
	const double km = fmin(rest / 2.0 + (double)0.4F, (double)0.4F);
	const double kx = 0.0 / 2.0 - kdx / (double)klen * (double)0.4F;
	const double kz = 0.0 / 2.0 - kdz / (double)klen * (double)0.4F;
	check(s->player.motion_y == km && s->player.vel_x == kx
			  && s->player.vel_z == kz,
		  "S: knockBack wrote Beta's SERVER motion (motionY = rest/2 + 0.4F, capped)");
	check(at_hit == 0 && s->player.been_attacked,
		  "S: the fresh hit raises beenAttacked and sends NOTHING itself");
	check(s->player.y == y_before && s->player.vel_y == 0.123F,
		  "S: pose and client fall sample untouched by the hit");
	/* One accepted (stationary) packet's server physics, then the tracker. */
	server_local_tick_actor_knockback(s);
	server_local_flush_actor_velocity(s);
	server_local_flush_actor_velocity(s);
	clin_test_set_rpc_observer(NULL, NULL);
	clin_drain_discard();
	const double sy = (km - 0.08) * (double)0.98F;
	const double sx = kx * (double)0.91F, sz = kz * (double)0.91F;
	check(velocity_rpcs == 1,
		  "S: exactly one CRPC_LOCAL_PLAYER_VELOCITY, at the tracker phase, no repeat");
	check(velocity_rpcs == 1
			  && last_velocity.payload.local_player_velocity.owner == 0
			  && !last_velocity.payload.local_player_velocity.add,
		  "S: absolute (setVelocity), on the untagged host route (owner 0)");
	check(velocity_rpcs == 1
			  && last_velocity.payload.local_player_velocity.vel[0] == q28(sx)
			  && last_velocity.payload.local_player_velocity.vel[1] == q28(sy)
			  && last_velocity.payload.local_player_velocity.vel[2] == q28(sz),
		  "S: payload = Q(one Beta air step of the knockBack motion)");
	/* Section C consumes the captured production payload. */
	const float beta_m0 = velocity_rpcs == 1
		? last_velocity.payload.local_player_velocity.vel[1] : q28(sy);
	split_destroy(s);

	/* ---- C: the host client integrates it to Beta's peak ------------------ */
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
	struct entity* host = gstate.players[0].local_player;
	assert(host && host->tick_client);
	floor_under(8, 8);
	const float host_rest = settle(host, 8.5F, 8.5F);
	check(host->on_ground && fabsf(host->vel[1] - rest_vel_y) < 1e-7F,
		  "C: the host body rests with Beta's resting motionY");
	/* The RPC exactly as the server built it (fall back to the Beta value
	 * when section S is RED, so C measures the client half on its own). */
	struct client_rpc hit = {.type = CRPC_LOCAL_PLAYER_VELOCITY,
		.payload.local_player_velocity = {.owner = 0,
			.vel = {0.0F, beta_m0, 0.0F}}};
	/* The production delivery, with its horizontal half dropped so the
	 * peak measures the vertical integration alone (the steer test owns
	 * the horizontal half). */
	clin_process(&hit);
	check(host->vel[1] == beta_m0, "C: the host client installed motionY");
	const double want = beta_peak_rise((double)beta_m0);
	const float host_peak = rise_after(host, host_rest);
	printf("  host peak rise %.6f, Beta %.6f\n", (double)host_peak, want);
	check(fabs((double)host_peak - want) < 1e-4,
		  "C: the host body peaks at Beta's knockback height");
	check(host->on_ground, "C: and lands again");
	/* The guest route is tagged; an untagged owner-1 copy must not move the
	 * host (and must not crash with no guest bound). */
	const float host_y_before = host->pos[1];
	struct client_rpc stray = hit;
	stray.payload.local_player_velocity.owner = 1;
	clin_process(&stray);
	check(host->vel[1] != beta_m0 && host->pos[1] == host_y_before,
		  "C: an untagged owner-1 velocity is not applied to the host");

	/* ---- G: the guest session integrates its own copy -------------------- */
	struct client_session* gs = &gstate.players[1];
	struct entity* guest = calloc(1, sizeof(*guest));
	assert(guest);
	entity_local_player(SERVER_LOCAL_PLAYER_ENTITY_ID + 1000u, guest,
						&gstate.world);
	floor_under(40, 40);
	const float guest_rest = settle(guest, 40.5F, 40.5F);
	gs->local_player = guest;
	gs->local_bound = true;
	gs->local_generation = 7u;
	struct client_rpc ghit = hit;
	ghit.payload.local_player_velocity.owner = 1;
	check(client_session_local_reply(gs, 7u, &ghit),
		  "G: the guest route accepts the tagged velocity");
	check(guest->vel[1] == beta_m0, "G: the guest body installed motionY");
	check(host->vel[1] != beta_m0, "G: the host body is untouched");
	const float guest_peak = rise_after(guest, guest_rest);
	printf("  guest peak rise %.6f, Beta %.6f\n", (double)guest_peak, want);
	check(fabs((double)guest_peak - want) < 1e-4,
		  "G: the guest body peaks at Beta's knockback height");
	/* A stale generation never reaches the body. */
	const float gv = guest->vel[1];
	check(!client_session_local_reply(gs, 6u, &ghit) && guest->vel[1] == gv,
		  "G: a stale-generation velocity is refused");
	gs->local_player = NULL;
	gs->local_bound = false;
	free(guest);

	if(failures) {
		printf("RED test_knockback_client_motion: %u failing clause(s)\n",
			   failures);
		return 1;
	}
	puts("PASS test_knockback_client_motion: the hit raises beenAttacked; the "
		 "tracker phase delivers Q(server motion after one Beta step) to the "
		 "hit body's own client, which integrates the delivered motionY to "
		 "Beta's peak (EntityLiving.java:392-403, EntityTrackerEntry."
		 "java:124-127, NetClientHandler.java:165-170)");
	return 0;
}
