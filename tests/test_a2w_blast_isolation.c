/* A2-W: one blast may hit only bodies committed to its own world.
 * Keep both actors at identical coordinates to make an accidental global
 * player sweep observable. Each case blasts BOTH worlds: a filter that simply
 * suppresses all player damage cannot pass. */
#include "support/owner_swap_harness.h"

#define TEST_NAME "test_a2w_blast_isolation"
#include "platform/audio.h" /* F4-RNG: SOUND_RANDOM_HURT */
#include "worldgen/java_random.h"

/* F4-RNG (brief 18, RNG rulings 3/8): every live world owns its own Java
 * World.rand and every body its own Entity.rand. Interleaved blasts in the
 * two worlds must advance ONLY the exploding world's stream -- by exactly the
 * 1352 rays + 2 doEffects sound floats for an all-air blast -- and the hit
 * body's fresh hurt pitch must be two left-to-right floats of ITS stream.
 * JVM anchor (F4RngWitness.java, OpenJDK 21.0.7, output sha256 2736a03f...):
 * new Random(0x5eed0b0dL).nextFloat() #0/#1 = 0x3e87b2a0 / 0x3ec24558. */
static float g_hurt_pitch[8];
static size_t g_hurt_n;
static void hurt_observer(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_SOUND && call->payload.sound.sound_id == SOUND_RANDOM_HURT
	   && g_hurt_n < 8)
		g_hurt_pitch[g_hurt_n++] = call->payload.sound.pitch;
}
static int jsteps(struct java_random from, const struct java_random* to, int max) {
	for(int k = 0; k <= max; k++) {
		if(from.seed == to->seed)
			return k;
		(void)jrand_next_float(&from);
	}
	return -1;
}

struct actor_state {
	short health;
	double x, y, z;
	float vx, vy, vz;
};

static struct actor_state actor_snapshot(const struct server_player* p) {
	return (struct actor_state) {
		.health = p->health, .x = p->x, .y = p->y, .z = p->z,
		.vx = p->vel_x, .vy = p->vel_y, .vz = p->vel_z,
	};
}

static bool actor_unchanged(const struct server_player* p,
	const struct actor_state* before) {
	return p->health == before->health && p->x == before->x
		&& p->y == before->y && p->z == before->z
		&& p->vel_x == before->vx && p->vel_y == before->vy
		&& p->vel_z == before->vz;
}

static void reset_actor(struct server_player* p) {
	p->x = 8.5;
	p->y = 60.62;
	p->z = 8.5;
	p->health = MAX_PLAYER_HEALTH;
	p->hurt_resist_time = 0;
	p->vel_x = p->vel_y = p->vel_z = 0.0f;
	p->has_pos = true;
}

static void blast_world(struct server_local* s, struct server_world* w,
	struct server_player* local, struct server_player* foreign,
	const char* label) {
	reset_actor(local);
	reset_actor(foreign);
	struct actor_state local_before = actor_snapshot(local);
	struct actor_state foreign_before = actor_snapshot(foreign);
	check(server_local_world_of(s, local) == w
			  && server_local_world_of(s, foreign) != w,
		"fixture: only the local victim belongs to the blast world");
	vec3 center = {9.5f, 60.0f, 8.5f};
	/* F4-RNG: distinct deterministic seeds for both worlds and both bodies */
	struct server_world* other = w == &s->worlds[0] ? &s->worlds[1] : &s->worlds[0];
	jrand_set_seed(&w->rand, INT64_C(0x0A2B0001));
	jrand_set_seed(&other->rand, INT64_C(0x0A2B0002));
	const struct java_random w_before = w->rand, other_before = other->rand;
	server_local_test_seed_body_rand(s, local, INT64_C(0x5eed0b0d));
	server_local_test_seed_body_rand(s, foreign, INT64_C(0x0F0E0D0C));
	g_hurt_n = 0;
	clin_test_set_rpc_observer(hurt_observer, NULL);
	struct thread consumer;
	__atomic_store_n(&consumer_running, 1, __ATOMIC_RELEASE);
	assert(thread_create(&consumer, consumer_entry, NULL, 64));
	server_world_begin_update(w);
	bool accepted = server_world_try_explode_by(w, NULL, center, 1.0f);
	__atomic_store_n(&consumer_running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_update_drain();
	clin_test_set_rpc_observer(NULL, NULL);
	check(accepted, "production blast accepted");
	{
		struct server_world_explosion_cost cost;
		server_world_explosion_last_cost(&cost);
		int k = jsteps(w_before, &w->rand, 4000);
		printf("%s: F4-RNG blast-world draws %d (all-air: broken %u), other world %s\n",
			label, k, cost.broken,
			other->rand.seed == other_before.seed ? "untouched" : "ADVANCED");
		check(cost.broken == 0 && k == 1352 + 2,
			"F4-RNG: the exploding world's OWN World.rand drew 1352 rays + 2 sound floats");
		check(other->rand.seed == other_before.seed,
			"F4-RNG: the other world's World.rand is untouched (independent per world)");
		uint32_t b0 = 0x3e87b2a0u, b1 = 0x3ec24558u;
		float f0, f1;
		memcpy(&f0, &b0, 4);
		memcpy(&f1, &b1, 4);
		const float want = (f0 - f1) * 0.2F + 1.0F;
		uint32_t gb = 0, wb;
		if(g_hurt_n)
			memcpy(&gb, &g_hurt_pitch[0], 4);
		memcpy(&wb, &want, 4);
		printf("%s: F4-RNG hurt sounds %zu, pitch %.9g (want %.9g)\n", label, g_hurt_n,
			g_hurt_n ? (double)g_hurt_pitch[0] : 0.0, (double)want);
		check(g_hurt_n == 1 && gb == wb,
			"F4-RNG: one fresh hurt pitch from the HIT body's own Entity.rand");
	}
	check(local->health < local_before.health,
		"blast damages the actor committed to its world");
	check(local->vel_x != local_before.vx || local->vel_y != local_before.vy
			  || local->vel_z != local_before.vz || local->y != local_before.y,
		"blast applies an impulse to the actor committed to its world");
	check(actor_unchanged(foreign, &foreign_before),
		"foreign-world actor keeps exact health, pose and velocity");
	printf("%s: local %d->%d, foreign %d->%d\n", label,
		local_before.health, local->health,
		foreign_before.health, foreign->health);
}

static void case_world_pair(bool host_nether) {
	struct server_local* s = admitted_fixture();
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	struct server_world* nether = &s->worlds[1];
	string_t path;
	string_init_set_str(path, split_path);
	server_world_create(nether, path, WORLD_DIM_NETHER, 12345, false);
	string_clear(path);
	nether->local = s;
	nether->session = s;
	nether->world_index = 1;
	struct server_player* guest = s->guest_actor;
	assert(guest);
	s->player.world_slot = host_nether ? 1 : 0;
	s->player.dimension = host_nether ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
	guest->world_slot = host_nether ? 0 : 1;
	guest->dimension = host_nether ? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER;
	check(server_local_world_of(s, &s->player)
			  == &s->worlds[s->player.world_slot]
			  && server_local_world_of(s, guest) == &s->worlds[guest->world_slot],
		"both committed actors resolve to distinct live worlds");
	blast_world(s, &s->worlds[0], host_nether ? guest : &s->player,
		host_nether ? &s->player : guest, "Overworld blast");
	blast_world(s, nether, host_nether ? &s->player : guest,
		host_nether ? guest : &s->player, "Nether blast");
	s->player.world_slot = 0;
	s->player.dimension = WORLD_DIM_OVERWORLD;
	guest->world_slot = 0;
	guest->dimension = WORLD_DIM_OVERWORLD;
	server_world_discard(nether);
	teardown(s);
}

int main(int argc, char** argv) {
	harness_init();
	const char* which = argc > 1 ? argv[1] : "";
	if(!strcmp(which, "host_nether"))
		case_world_pair(true);
	else if(!strcmp(which, "guest_nether"))
		case_world_pair(false);
	else {
		printf("unknown case %s\n", which);
		return 2;
	}
	printf("%s[%s]: %u failure(s)\n", TEST_NAME, which, failures);
	return failures ? 1 : 0;
}
