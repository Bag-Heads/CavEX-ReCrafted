/* Copyright (c) 2026 CaveWii_v2 contributors
 * drown-fx-0927 (DAMAGE-AUDIT-0925 items 2 + 4): Beta's drowning bubbles.
 *
 * Beta EntityLiving.onEntityUpdate (mcb173 src_original, lines 122-134):
 *   if alive && in water && !canBreatheUnderwater:
 *     --air; if(air == -20) { air = 0;
 *       for(i = 0; i < 8; ++i) spawnParticle("bubble",
 *           posX + (r-r), posY + (r-r), posZ + (r-r), motionX, motionY, motionZ);
 *       attackEntityFrom(null, 2); }
 * EntityBubbleFX: white, texture 32, scale *= r*0.6+0.2, motion = m*0.2 +
 * U(-0.02, 0.02), maxAge (int)(8/(r*0.8+0.2)); onUpdate motionY += 0.002,
 * move, *= 0.85, dead outside water or once maxAge-- <= 0.
 *
 * CLIENT (lane B, on this branch):
 *   C1 one CRPC_DROWN_BUBBLES spawns exactly 8 bubble particles: particle
 *      atlas bubble tile, white, no gravity, bubble motion, each within
 *      (-1, 1) of the actor position per axis, velocity m*0.2 +- 0.02,
 *      lifetime maxAge+1 in [9, 41].
 *   C2 a bubble outside water dies on its first update (Beta setEntityDead).
 * SERVER (lane A -- GREEN only with out/DROWN-FX-handoff.patch):
 *   S1 host: the tick air reaches the threshold emits ONE CRPC_DROWN_BUBBLES
 *      at the eye with the body's motion, BEFORE the drowning hit's health
 *      RPC; the air resets to 0.
 *   S2 the guest in split does the same for its own body.
 *   S3 no bubbles on a tick where air does not run out.
 * The suffocation-before-drowning order (item 3) is already pinned by
 * test_damage_parity_golden (host) on this base.
 *
 * RED by construction until CLIENT_DROWN_BUBBLES exists; the server legs are
 * RED until the handoff patch lands. */
#include "support/split_residency_fixture.h"

#include <math.h>
#include <stdio.h>

#include "game/client_world_context.h"
#include "game/game_state.h"
#include "graphics/texture_atlas.h"
#include "item/items.h"
#include "particle.h"
#include "world.h"

#ifndef DROWN_BUBBLE_COUNT
int main(void) {
	puts("RED: DROWN_BUBBLE_COUNT absent -- no client drowning-bubble burst "
		 "(DAMAGE-AUDIT-0925 item 2)");
	return 1;
}
#else

void server_local_test_apply_guest_hazards(struct server_local*);

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

static void client_legs(void) {
	particle_init();
	vec3 eye = {100.5F, 70.62F, -40.5F};
	vec3 motion = {0.1F, -0.05F, 0.2F};
	particle_set_camera(eye);
	particle_clear_all();
	clin_process(&(struct client_rpc) {.type = CRPC_DROWN_BUBBLES,
		.payload.drown_bubbles = {.pos = {eye[0], eye[1], eye[2]},
								  .vel = {motion[0], motion[1], motion[2]}}});
	check(particle_test_count() == DROWN_BUBBLE_COUNT && DROWN_BUBBLE_COUNT == 8,
		  "C1 one drowning event spawns exactly 8 bubbles");
	bool shape = particle_test_count() == 8;
	uint8_t tex = tex_atlas_lookup_particle(TEXAT_PARTICLE_BUBBLE);
	for(size_t i = 0; shape && i < 8; i++) {
		struct particle p;
		shape = particle_test_get(i, &p);
		for(int k = 0; shape && k < 3; k++)
			shape = fabsf(p.pos[k] - eye[k]) < 1.0F
				&& fabsf(p.vel[k] - motion[k] * 0.2F) <= 0.02F + 1e-6F;
		shape = shape && p.bubble && !p.gravity && p.tex == tex
			&& p.atlas == TEXTURE_ATLAS_PARTICLES && p.r == 255 && p.g == 255
			&& p.b == 255 && p.lifetime >= 9 && p.lifetime <= 41;
	}
	check(shape, "C1 each bubble: Beta spread, motion, tile, colour, lifetime");
	particle_update(); /* the client world here is empty: no water anywhere */
	check(particle_test_count() == 0,
		  "C2 a bubble outside water dies on its first update");
}

static unsigned bubbles, health_rpcs, bubble_index, health_index, seq;
static vec3 bubble_pos, bubble_vel;

static void observe(const struct client_rpc* call, void* user) {
	(void)user;
	seq++;
	if(call->type == CRPC_DROWN_BUBBLES) {
		bubbles++;
		bubble_index = seq;
		glm_vec3_copy((float*)call->payload.drown_bubbles.pos, bubble_pos);
		glm_vec3_copy((float*)call->payload.drown_bubbles.vel, bubble_vel);
	} else if(call->type == CRPC_PLAYER_SET_HEALTH && !health_index) {
		health_rpcs++;
		health_index = seq;
	}
}

static void reset_obs(void) {
	bubbles = health_rpcs = bubble_index = health_index = seq = 0;
}

static void place_water(struct server_local* s, double x, double y, double z) {
	struct block_data water = {.type = BLOCK_WATER_STILL};
	assert(server_world_set_block(&s->worlds[0], (w_coord_t)floor(x),
		(w_coord_t)floor(y), (w_coord_t)floor(z), water));
}

static void server_legs(void) {
	struct server_local* s = split_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	split_populate(s, 0, 0);
	s->player.finished_loading = true;
	s->player.x = 4.5;
	s->player.z = 4.5;
	place_water(s, s->player.x, s->player.y, s->player.z);
	s->player.vel_x = 0.03F;
	s->player.vel_y = -0.02F;
	s->player.vel_z = 0.01F;
	s->player.hurt_resist_time = 0;
	clin_drain_discard();
	clin_test_set_rpc_observer(observe, NULL);

	/* S3 first: air above the threshold -> no bubbles. */
	s->player.oxygen = 50;
	reset_obs();
	(void)server_local_test_apply_player_hazards(s, true, false, false, false);
	check(bubbles == 0 && s->player.oxygen == 49,
		  "S3 no bubbles while air remains");

	/* S1: the tick air reaches -20. */
	s->player.oxygen = -19;
	reset_obs();
	(void)server_local_test_apply_player_hazards(s, true, false, false, false);
	check(bubbles == 1 && s->player.oxygen == 0,
		  "S1 host: one CRPC_DROWN_BUBBLES on the tick air runs out");
	check(bubbles == 1 && bubble_pos[0] == (float)s->player.x
			  && bubble_pos[1] == (float)s->player.y
			  && bubble_pos[2] == (float)s->player.z
			  && bubble_vel[0] == s->player.vel_x
			  && bubble_vel[1] == s->player.vel_y
			  && bubble_vel[2] == s->player.vel_z,
		  "S1 host: at the eye, with the body's motion");
	check(bubbles == 1 && health_index && bubble_index < health_index,
		  "S1 host: the bubbles precede the drowning hit (Beta 127-134)");
	clin_drain_discard();

	/* S2: the guest body in split. */
	struct server_player* g = calloc(1, sizeof(*g));
	assert(g);
	g->has_pos = true;
	g->finished_loading = true;
	g->health = MAX_PLAYER_HEALTH;
	g->x = 6.5;
	g->y = s->player.y;
	g->z = 6.5;
	g->vel_x = -0.04F;
	g->vel_y = 0.0F;
	g->vel_z = 0.05F;
	g->oxygen = -19;
	g->dimension = s->player.dimension;
	place_water(s, g->x, g->y, g->z);
	s->guest_actor = g;
	s->guest_gameplay = true;
	reset_obs();
	server_local_test_apply_guest_hazards(s);
	check(bubbles == 1 && g->oxygen == 0 && bubble_pos[0] == (float)g->x
			  && bubble_pos[2] == (float)g->z && bubble_vel[2] == g->vel_z,
		  "S2 guest: its own bubbles at its eye with its motion");
	s->guest_gameplay = false;
	s->guest_actor = NULL;
	free(g);

	clin_test_set_rpc_observer(NULL, NULL);
	clin_drain_discard();
	split_destroy(s);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	puts("test_drown_bubbles");
	blocks_init();
	items_init();
	assert(clin_init() && svin_init());
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	gstate.players[0].visible_window = -1;
	/* World-scoped RPCs reach the client through a reset (ready) world. */
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = 77}});
	client_legs();
	server_legs();
	printf("%s test_drown_bubbles (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
#endif
