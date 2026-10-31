/* Split-dims B1 remote stamping (lane B, b1stamp-remote-0926): every
 * remote-origin client RPC carries the remote session's world identity after
 * a login, none is ever stamped with a dead session's identity, and a
 * reconnect mints a fresh one.
 *
 * Driven through the real codec bytes -> process_event
 * (server_remote_protocol.c) and the entity-status flush
 * (server_remote_entities.c); clin_process_direct is --wrap'ed to record the
 * stamp each dispatch carries (the remote adapters reach it through the
 * inline clin_process_direct_remote, so the wrap sees the stamped call).
 * server_remote_chunks.c's five dispatches use the same inline; their
 * streaming preconditions (resident columns) are covered by the existing
 * test_mp_* suites, which stay green. */
#define main existing_mp_suite_main
#include "test_mp_s0_env.c"
#undef main

#ifdef _WIN32
#include <windows.h>
#define wait_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define wait_ms(ms) usleep((ms) * 1000)
#endif

enum { LOCAL_ID = 42, MOB_ID = 64001 };

/* ---- clin_process_direct wrap ------------------------------------------ */
void __real_clin_process_direct(struct client_rpc* call);
static unsigned n_seen;
static uint32_t seen_gen[256];
static int seen_type[256];
/* Every CRPC_SOUND payload seen, for the Packet22 pickup-pop check. */
static unsigned n_snd;
static struct { uint16_t id; float vol, pitch; vec3 pos; } snd[64];
void __wrap_clin_process_direct(struct client_rpc* call) {
	if(call->type == CRPC_SOUND && n_snd < 64) {
		snd[n_snd].id = call->payload.sound.sound_id;
		snd[n_snd].vol = call->payload.sound.vol;
		snd[n_snd].pitch = call->payload.sound.pitch;
		glm_vec3_copy(call->payload.sound.pos, snd[n_snd].pos);
		n_snd++;
	}
	if(n_seen < 256) {
		seen_gen[n_seen] = call->world_generation;
		seen_type[n_seen] = (int)call->type;
		n_seen++;
	}
	__real_clin_process_direct(call);
}

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

static void time_packet(struct server_remote* owner, int64_t t) {
	uint8_t bytes[9] = {0x04};
	for(int i = 0; i < 8; i++)
		bytes[1 + i] = (uint8_t)((uint64_t)t >> (56 - 8 * i));
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	event.packet = decode_exact(bytes, sizeof(bytes), MP_EVENT_UPDATE_TIME);
	assert(process_event(owner, &event));
}

static void spawn_pig(struct server_remote* owner, int32_t id) {
	uint8_t bytes[25] = {0x18};
	put_i32(bytes + 1, id);
	bytes[5] = 90;
	put_i32(bytes + 10, 320);
	bytes[24] = 0x7f;
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	event.packet = decode_exact(bytes, sizeof(bytes), MP_EVENT_MOB_SPAWN);
	assert(process_event(owner, &event));
}

static void send_status(struct server_remote* owner, int32_t id, uint8_t status) {
	uint8_t bytes[6] = {0x26, 0, 0, 0, 0, status};
	put_i32(bytes + 1, id);
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	event.packet = decode_exact(bytes, sizeof(bytes), MP_EVENT_ENTITY_STATUS);
	assert(process_event(owner, &event));
}

static void pickup_spawn(struct server_remote* owner, int32_t id) {
	uint8_t bytes[25] = {0x15};
	put_i32(bytes + 1, id);
	bytes[6] = 1; /* itemID low byte */
	bytes[7] = 1; /* count */
	put_i32(bytes + 10, 64);  /* x = 2.0 blocks */
	put_i32(bytes + 14, 96);  /* y = 3.0 */
	put_i32(bytes + 18, 128); /* z = 4.0 */
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	event.packet = decode_exact(bytes, sizeof(bytes), MP_EVENT_PICKUP_SPAWN);
	assert(process_event(owner, &event));
}

static void collect(struct server_remote* owner, int32_t collected,
					int32_t collector) {
	uint8_t bytes[9] = {0x16};
	put_i32(bytes + 1, collected);
	put_i32(bytes + 5, collector);
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	event.packet = decode_exact(bytes, sizeof(bytes), MP_EVENT_COLLECT);
	assert(process_event(owner, &event));
}

/* Every dispatch recorded since `from` carries exactly `gen`. */
static bool all_stamped(unsigned from, uint32_t gen, unsigned* count) {
	*count = n_seen - from;
	for(unsigned i = from; i < n_seen; i++)
		if(seen_gen[i] != gen) {
			printf("     dispatch %u (type %d) carries %u, expected %u\n", i,
				   seen_type[i], (unsigned)seen_gen[i], (unsigned)gen);
			return false;
		}
	return *count > 0;
}

static bool saw_type(unsigned from, int type) {
	for(unsigned i = from; i < n_seen; i++)
		if(seen_type[i] == type)
			return true;
	return false;
}

int main(void) {
	assert(svin_init() && clin_init());
	blocks_init();
	items_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	struct server_remote owner = {0};
	assert(server_remote_init(&owner));
	gstate.remote_session = true;
	unsigned count;

	/* Before any remote install: unstamped, as before B1. */
	unsigned mark = n_seen;
	time_packet(&owner, 100);
	check(all_stamped(mark, 0, &count), "pre-login remote RPCs stay unstamped");

	/* Login 1. */
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, LOCAL_ID);
	((struct server_remote_impl*)owner.impl)->entity_id = LOCAL_ID;
	uint32_t g1 = clin_remote_world_generation();
	check(g1 != 0, "login mints a nonzero remote identity");
	mark = n_seen;
	time_packet(&owner, 200);                 /* server_remote_protocol.c */
	spawn_pig(&owner, MOB_ID);                /* server_remote_protocol.c */
	send_status(&owner, MOB_ID, 2);           /* server_remote_entities.c SOUND */
	wait_ms(650);
	flush_entity_status((struct server_remote_impl*)owner.impl); /* ENTITY_MOVE */
	check(all_stamped(mark, g1, &count),
		  "every remote-origin RPC after login carries the session identity");
	check(saw_type(mark, CRPC_TIME_SET) && saw_type(mark, CRPC_SOUND)
			  && saw_type(mark, CRPC_ENTITY_MOVE),
		  "protocol + entity-adapter dispatches were exercised");
	printf("     %u stamped dispatches under identity %u\n", count,
		   (unsigned)g1);

	/* Packet22Collect pickup pop: Beta NetClientHandler.handleCollect plays
	 * "random.pop" 0.2F, pitch ((r-r)*0.7F+1.0F)*2.0F at the COLLECTED entity
	 * whenever it is known (any collector, any entity kind); an unknown
	 * collected id is a total no-op (no sound). */
	{
		*audio_cosmetic_rng() = (struct random_gen) {.seed = 1357};
		struct random_gen expect = *audio_cosmetic_rng();
		float e1 = rand_gen_flt(&expect), e2 = rand_gen_flt(&expect);
		float want1 = ((e1 - e2) * 0.7F + 1.0F) * 2.0F;
		e1 = rand_gen_flt(&expect);
		e2 = rand_gen_flt(&expect);
		float want2 = ((e1 - e2) * 0.7F + 1.0F) * 2.0F;
		unsigned s0 = n_snd;
		pickup_spawn(&owner, 9001);
		collect(&owner, 9001, 77); /* another player collects */
		check(n_snd == s0 + 1 && snd[s0].id == SOUND_RANDOM_POP
				  && snd[s0].vol == 0.2f && snd[s0].pitch >= 1.3f
				  && snd[s0].pitch <= 2.7f && snd[s0].pos[0] == 2.0f
				  && snd[s0].pos[1] == 3.0f && snd[s0].pos[2] == 4.0f,
			  "remote Packet22Collect plays random.pop 0.2 at the item");
		check(snd[s0].pitch == want1, "pop pitch is Beta's formula from the cosmetic RNG");
		unsigned s1 = n_snd;
		pickup_spawn(&owner, 9003);
		collect(&owner, 9003, 77);
		check(n_snd == s1 + 1 && snd[s1].pitch == want2 && want2 != want1
				  && snd[s1].pitch >= 1.3f && snd[s1].pitch <= 2.7f,
			  "a second collect rolls a different pitch");
		s0 = n_snd;
		collect(&owner, 9002, 77); /* never spawned */
		check(n_snd == s0, "collect of an unknown entity makes no sound");
	}

	/* Disconnect: the dead identity is retired, never reused on traffic. */
	clin_force_world_reset();
	mark = n_seen;
	time_packet(&owner, 300);
	check(all_stamped(mark, 0, &count),
		  "after disconnect no RPC carries the dead session's identity");

	/* Login 2: a fresh identity. */
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, LOCAL_ID);
	uint32_t g2 = clin_remote_world_generation();
	check(g2 != 0 && g2 != g1, "reconnect mints a fresh identity");
	mark = n_seen;
	time_packet(&owner, 400);
	send_status(&owner, MOB_ID, 2);
	check(all_stamped(mark, g2, &count),
		  "the reconnected session's RPCs carry the new identity only");

	server_remote_destroy(&owner);
	printf("%s test_b1_remote_stamp (%u failure%s)\n", failures ? "FAIL" : "PASS",
		   failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
