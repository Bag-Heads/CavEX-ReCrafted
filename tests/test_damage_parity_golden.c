/* Beta 1.7.3: EntityLiving.java:113-134, 338-346, 392-403.
 * These assertions use the production damage and hazard paths. */
#include "support/split_residency_fixture.h"
#include <math.h>
#include <stdio.h>

static int failures;
static void exact_float(const char* label, float got, float want) {
	if(memcmp(&got, &want, sizeof(got)) != 0) {
		fprintf(stderr, "RED %s: got %.9g want %.9g (bit mismatch)\n", label,
			got, want);
		failures++;
	}
}
static short first_health_rpc;
static unsigned health_rpc_count;
static unsigned rpc_index, first_health_index, second_health_index;
static unsigned sound_index, sound_count;
static unsigned velocity_count, velocity_index;
static struct client_rpc last_velocity;
static void observe_rpc(const struct client_rpc* call, void* user) {
	(void)user;
	rpc_index++;
	if(call->type == CRPC_PLAYER_SET_HEALTH) {
		if(health_rpc_count++ == 0) {
			first_health_rpc = call->payload.player_set_health.health;
			first_health_index = rpc_index;
		} else if(health_rpc_count == 2)
			second_health_index = rpc_index;
	} else if(call->type == CRPC_SOUND) {
		sound_count++;
		if(sound_count == 1)
			sound_index = rpc_index;
	} else if(call->type == CRPC_LOCAL_PLAYER_VELOCITY) {
		velocity_count++;
		last_velocity = *call;
		velocity_index = rpc_index;
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
static void check(const char* label, double got, double want, double tol) {
	if(fabs(got - want) > tol) {
		fprintf(stderr, "RED %s: got %.9f want %.9f\n", label, got, want);
		failures++;
	}
}

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();
	struct server_local* s = split_fixture();
	struct entity attacker = {0};
	attacker.type = ENTITY_MONSTER;
	attacker.data.monster.id = MONSTER_ZOMBIE;
	attacker.pos[0] = (float)s->player.x + 3.0f;
	attacker.pos[2] = (float)s->player.z + 4.0f;
	/* G8-BLAST-VELOCITY v3: SERVER motion seeded separately from the client
	 * fall sample (motion_y = (double)0.3F, vel_y = -0.5F); X/Z are Beta
	 * doubles. */
	s->player.vel_x = 0.2f;
	s->player.motion_y = (double)0.3F;
	s->player.vel_y = -0.5f;
	s->player.vel_z = -0.1f;
	const double y0 = s->player.y;
	clin_test_set_rpc_observer(observe_rpc, NULL);
	server_local_hurt_player(s, &attacker);
	const unsigned hit_sends = velocity_count;
	/* MathHelper.sqrt_double returns float; Java then widens both floats
	 * before the division/multiply in knockBack. */
	const float len = (float)sqrt(3.0 * 3.0 + 4.0 * 4.0);
	const float impulse = 0.4F;
	const double kx = (double)0.2f / 2.0 - 3.0 / (double)len * (double)impulse;
	const double kz = (double)-0.1f / 2.0 - 4.0 / (double)len * (double)impulse;
	const double ky = fmin((double)0.3F / 2.0 + (double)0.4F, (double)0.4F);
	check("knock x", s->player.vel_x, kx, 0.0);
	check("knock y (server motionY capped at (double)0.4F)", s->player.motion_y,
		  ky, 0.0);
	check("knock y cap is exactly (double)0.4F", ky, (double)0.4F, 0.0);
	check("knock z", s->player.vel_z, kz, 0.0);
	check("client fall sample untouched", (double)s->player.vel_y, -0.5, 0.0);
	check("no instant Y displacement", s->player.y, y0, 0.0);
	check("the hit sends nothing itself", (double)hit_sends, 0.0, 0.0);
	const double x0 = s->player.x, z0 = s->player.z;
	const bool ground = s->player.on_ground;
	server_local_tick_actor_knockback(s); /* one accepted sample */
	server_local_flush_actor_velocity(s);
	server_local_flush_actor_velocity(s);
	clin_test_set_rpc_observer(NULL, NULL);
	/* G8: the server keeps the client-sent position (NetServerHandler.java:
	 * 172/257) and evolves only its own motion: Beta's air/ground branch
	 * (EntityLiving.java:459-516) -- upward Y is collision-free here. */
	check("knock tick x (server does not move the body)", s->player.x, x0, 0.0);
	check("knock tick z (server does not move the body)", s->player.z, z0, 0.0);
	const double k = (double)(ground ? 0.6F * 0.91F : 0.91F);
	const double sx = kx * k, sz = kz * k, sy = (ky - 0.08) * (double)0.98F;
	check("knock step x", s->player.vel_x, sx, 0.0);
	check("knock step y (gravity + drag)", s->player.motion_y, sy, 0.0);
	check("knock step z", s->player.vel_z, sz, 0.0);
	check("one tracker send", (double)velocity_count, 1.0, 0.0);
	if(velocity_count == 1) {
		const float* v = last_velocity.payload.local_player_velocity.vel;
		check("send x = Q(x)", (double)v[0], (double)q28(sx), 0.0);
		check("send y = Q(y)", (double)v[1], (double)q28(sy), 0.0);
		check("send z = Q(z)", (double)v[2], (double)q28(sz), 0.0);
		check("send absolute", (double)last_velocity.payload.local_player_velocity.add, 0.0, 0.0);
	}
	clin_drain_discard();
	velocity_count = 0;
	health_rpc_count = 0;
	sound_count = 0;
	rpc_index = 0;

	/* An opaque eye sample and air=-19 make both hits due in one tick.
	 * Beta onEntityUpdate: suffocation at 113 precedes drowning at 122.
	 * The first hit owns the fresh hurt window; the stronger drowning hit
	 * then applies excess damage without a second fresh sound. */
	split_populate(s, 0, 0);
	s->player.hurt_resist_time = 0;
	s->player.last_damage = 0;
	s->player.oxygen = -19;
	s->player.x = 0.95;
	s->player.z = 0.95;
	struct block_data stone = {.type = BLOCK_STONE};
	/* The fixture is an Overworld-only scene. */
	assert(server_world_set_block(&s->worlds[0], 1,
		(int)floor(s->player.y), 1, stone));
	struct block_data placed = {0};
	assert(server_world_get_block(&s->worlds[0], 1,
		(int)floor(s->player.y), 1, &placed));
	assert(placed.type == BLOCK_STONE);
	const short before = s->player.health;
	clin_test_set_rpc_observer(observe_rpc, NULL);
	(void)server_local_test_apply_player_hazards(s, true, false, false, false);
	if(health_rpc_count < 1 || first_health_rpc != before - HEALTH_PER_HEART / 2) {
		fprintf(stderr, "RED suffocation precedes drowning: first health %d, count %u\n",
			first_health_rpc, health_rpc_count);
		failures++;
	}
	if(health_rpc_count != 2 || sound_count != 1
	   || !(first_health_index < sound_index
		&& sound_index < second_health_index)) {
		fprintf(stderr, "RED hazard sound order: health=%u sound=%u indices=%u/%u/%u\n",
			health_rpc_count, sound_count, first_health_index, sound_index,
			second_health_index);
		failures++;
	}
	/* The fresh suffocation hit raised beenAttacked, the excess-only
	 * drowning hit kept it: ONE tracker velocity after both, last. */
	check("no velocity before the tracker phase", (double)velocity_count, 0.0, 0.0);
	server_local_flush_actor_velocity(s);
	check("both hazards share one pending flag -> one velocity",
		  (double)velocity_count, 1.0, 0.0);
	check("the velocity follows both health replies",
		  (double)(velocity_index > second_health_index), 1.0, 0.0);
	clin_test_set_rpc_observer(NULL, NULL);
	check("drowning air reset", s->player.oxygen, 0, 0);
	clin_drain_discard();
	split_destroy(s);
	if(failures)
		return 1;
	puts("PASS test_damage_parity_golden");
	return 0;
}
