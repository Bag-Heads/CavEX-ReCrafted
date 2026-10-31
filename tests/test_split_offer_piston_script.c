/* Copyright (c) 2026 CaveWii_v2 contributors
 * Real Join/offer, fixture command, moving TE, correction and admission.
 * The driver uses ordinary client physics + immutable movement credits.
 * No test writes a guest pose, relation or ACK. Real hardware stays owed. */
#include "support/owner_swap_harness.h"
#include "game/split_offer_piston_script.h"
#include "game/hw_run.h"
#include "game/difficulty.h"
#include "game/autopilot.h"
#include "network/vehicle_smoke_probe.h"
#include "worldgen/beta_math.h"

static unsigned emitted, done_calls, creates, refusals;
static unsigned server_refusals;
static bool retry_server, server_retry_started;
static char refused_line[SOP_LINE_CAP];
static char done_status[64], last_line[SOP_LINE_CAP];
static FILE* tape;
static bool emit(void* user, const char* line) {
	(void)user;
	if(refusals) { refusals--; return false; }
	if(retry_server && strstr(line,"\"event\":\"split_offer_piston.extended\"")) {
		if(!server_retry_started) {
			server_retry_started = true; server_refusals = 5;
			snprintf(refused_line,sizeof(refused_line),"%s",line);
		}
		check(!strcmp(line,refused_line),"refused server record retries identical bytes");
		if(server_refusals) { server_refusals--; return false; }
	}
	emitted++;
	snprintf(last_line, sizeof(last_line), "%s", line);
	char seq[48]; snprintf(seq, sizeof(seq), "\"seq\":%u,", emitted);
	check(strstr(line, seq) != NULL, "contiguous serialized seq");
	if(tape) {
		size_t n = strlen(line);
		fprintf(tape, "%.*s,\"run_id\":\"pc-offer\"}\n", (int)n-1, line);
		fflush(tape);
	}
	return true;
}
static void done(void* user, const char* status) {
	(void)user; done_calls++;
	snprintf(done_status, sizeof(done_status), "%s", status);
	if(tape) fprintf(tape, "{\"event\":\"profile_done\",\"run_id\":\"pc-offer\",\"status\":\"%s\"}\n", status);
}
static bool create(void* user, const char* dir) {
	(void)user; creates++;
	/* The already-created real disposable harness world stands in for the
 * frontend create/load. Frontend and main seams require the device run. */
	return !strcmp(dir, SOP_RIG_WORLD);
}
static struct hw_run_config config(void) {
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	char* args[] = {"test", "--cw-tel-host", "h", "--cw-profile", SOP_PROFILE};
	char err[128];
	assert(hw_run_parse_args(&cfg, 5, args, err, sizeof(err)));
	return cfg;
}
static const struct vehicle_smoke_hooks hooks = {.emit = emit,
	.profile_done = done, .create_world = create};

static struct server_local* ready_fixture(void) {
	struct server_local* s = split_fixture();
	s->paused = true; s->split_view_radius = 4;
	string_set_str(s->level_name, split_path);
	stream_to_marker(s); acknowledge(s);
	s->thread_started = true; enable_split(s);
	for(int x = -3; x <= 3; x++)
		for(int z = -3; z <= 3; z++) {
			struct server_chunk* c = dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(x,z));
			if(!c) c = split_populate(s,x,z);
			c->sent = true;
		}
	server_local_request_difficulty(DIFFICULTY_PEACEFUL);
	s->paused = false;
	return s;
}
/* Same producer main calls after each executed local physics tick. The
 * camera samples the body's real physics result; no pose is fabricated. */
static void walk(unsigned owner, const struct input_player_frame* f, float yaw) {
	struct client_session* c = &gstate.players[owner];
	struct entity* e = c->local_player;
	if(!e || (owner && !c->local_gameplay_ready)) return;
	e->data.local_player.capture_input = true;
	e->orient[0] = yaw; e->orient[1] = 1.5707964F;
	if(!clin_local_tick_begin(owner)) return;
	(void)entity_local_player_walk_owned(e, c, f);
	camera_attach_owned(&c->camera, e, 1.0F, 0.05F,
		&(struct camera_player_view) {.world = e->world, .ride = &c->ride,
			.sleeping = c->sleeping, .yaw = yaw, .pitch = 1.5707964F});
	if(clin_local_tick_physics_ran(owner)) clin_local_tick_end(owner, &c->camera);
	else clin_local_tick_cancel(owner);
}
static void case_run(bool refuse) {
	struct server_local* s = ready_fixture();
	struct hw_run_config cfg = config();
	if(refuse) refusals = 5;
	retry_server = refuse;
	tape = fopen("split-offer-piston-pc.ndjson", "w"); assert(tape);
	split_offer_piston_arm(&cfg, &hooks);
	ptime_t start = time_get();
	for(unsigned i = 0; i < 7000 && !split_offer_piston_finished(); i++) {
		uint32_t ms = (uint32_t)time_diff_ms(start, time_get());
		struct vehicle_smoke_actions a;
		split_offer_piston_frame(&(struct vehicle_smoke_ctx) {
			.host = &gstate.players[0], .guest = &gstate.players[1],
			.world_loaded = true, .now_ms = ms}, &a);
		struct input_player_frame host = {.connected = true};
		for(unsigned b = 0; b < IB_COUNT; b++) host.down[b] = a.host_buttons[b];
		walk(0, &host, a.host_yaw);
		walk(1, &a.guest_frame, a.guest_yaw);
		client_session_flush_local(&gstate.players[1]);
		if(gstate.players[1].local_phase == CLIENT_GUEST_ATTACHED
		   && !gstate.players[1].local_gameplay_ready)
			(void)client_session_request_local_gameplay(&gstate.players[1]);
		tick(s); client_acks_marker(s);
		thread_msleep(50); /* Real 20 Hz physics, including the 30 s play leg. */
	}
	fclose(tape); tape = NULL;
	printf("driver verdict: %s; host %.6f guest %.6f corrections %u\n",
		done_status, s->player.x, s->guest_actor ? s->guest_actor->x : 0, guest_corrections);
	check(done_calls == 1 && !strcmp(done_status, "ok"), "real driver completes once");
	check(creates == 1 && emitted == 17, "one disposable create and complete observation tape");
	check(s->guest_gameplay && s->guest_actor && s->guest_actor->health == MAX_PLAYER_HEALTH,
		"real guest admitted alive after offered push");
	check(!sop_probe_armed() && !sop_probe_hold_offer(), "completion revokes fixture");
	split_offer_piston_reset(); teardown(s);
}
static void case_profile(void) {
	struct hw_run_config cfg = config();
	check(cfg.profile == HW_RUN_PROFILE_SPLIT_REGROW_8M+1,
		"profile ordinal appended");
	check(cfg.enabled && cfg.split_guest && cfg.superflat && cfg.force_overworld
		&& cfg.driver_owns_completion && !cfg.autoload_new && !cfg.vehicle_smoke
		&& !cfg.split_guest_walk && cfg.seed == 12345, "profile shape");
	split_offer_piston_arm(&cfg,&hooks);
	check(split_offer_piston_armed() && sop_probe_hold_offer(), "profile arms disclosed hold");
	bool buttons[IB_COUNT] = {0};
	buttons[IB_RIGHT] = buttons[IB_SNEAK] = true;
	autopilot_script_look(true, 0, 1.5707964F);
	autopilot_script_buttons(true, buttons);
	check(input_held(IB_RIGHT) && input_held(IB_SNEAK),
		"fixture creep uses real host button levels");
	autopilot_script_buttons(false, NULL);
	autopilot_script_look(false, 0, 0);
	hw_run_apply_profile(&cfg, HW_RUN_PROFILE_HEARTBEAT_SELFTEST);
	split_offer_piston_arm(&cfg,&hooks);
	struct vehicle_smoke_actions a; memset(&a,0xff,sizeof(a));
	struct vehicle_smoke_ctx ctx = {.now_ms = 900000};
	split_offer_piston_frame(&ctx,&a);
	struct vehicle_smoke_actions zero = {0};
	check(!memcmp(&a,&zero,sizeof(a)) && !sop_probe_armed() && emitted == 0,
		"other profile completely inert");
}
static void case_fifo(void) {
	struct server_local* s = ready_fixture();
	sop_probe_arm();
	for(unsigned i = 0; i < SOP_FIFO_CAP+1; i++) sop_probe_snapshot(s,SOP_ERROR,false,"test");
	check(sop_probe_pending() == SOP_FIFO_CAP && sop_probe_overflows() == 1,
		"bounded FIFO refuses overflow without overwrite");
	struct sop_record r;
	for(unsigned i = 0; i < SOP_FIFO_CAP; i++) check(sop_probe_pop(&r), "FIFO drains admitted record");
	check(!sop_probe_pop(&r), "FIFO empty");
	sop_probe_reset(); teardown(s);
}
static uint32_t hash_bytes(uint32_t h, const void* p, size_t n) {
	const unsigned char* b = p;
	for(size_t i = 0; i < n; i++) h = (h ^ b[i])*16777619u;
	return h;
}
static uint32_t dormant_digest(bool armed) {
	struct server_local* s = admitted_fixture();
	jrand_set_seed(&s->worlds[0].rand,4242);
	server_local_test_seed_body_rand(s,&s->player,13579);
	server_local_test_seed_body_rand(s,s->guest_actor,24680);
	*server_local_sound_rng() = (struct random_gen){.seed=13579};
	if(armed) sop_probe_arm(); else sop_probe_reset();
	for(unsigned i = 0; i < 8; i++) { s->paused = false; tick(s); }
	double poses[] = {s->player.x,s->player.y,s->player.z,s->guest_actor->x,s->guest_actor->y,s->guest_actor->z};
	uint32_t h = hash_bytes(2166136261u,poses,sizeof(poses));
	h = hash_bytes(h,&s->rand_src,sizeof(s->rand_src));
	h = hash_bytes(h,&s->worlds[0].rand,sizeof(s->worlds[0].rand));
	h = hash_bytes(h,&s->player.entity_rand,sizeof(s->player.entity_rand));
	h = hash_bytes(h,&s->guest_actor->entity_rand,sizeof(s->guest_actor->entity_rand));
	h = hash_bytes(h,server_local_sound_rng(),sizeof(*server_local_sound_rng()));
	unsigned rpc_counts[] = {host_pos_msgs, guest_corrections, sound_msgs, hud_msgs};
	h = hash_bytes(h,rpc_counts,sizeof(rpc_counts));
	h = hash_bytes(h,s->player.inventory.items,sizeof(s->player.inventory.items));
	h = hash_bytes(h,s->guest_actor->inventory.items,sizeof(s->guest_actor->inventory.items));
	h = hash_bytes(h,&s->world_time,sizeof(s->world_time));
	check(sop_probe_pending() == 0 && sop_probe_overflows() == 0 && emitted == 0,
		"no fixture command: no record or RNG draw");
	printf("DORMANT_DIGEST %08x\n", h);
	sop_probe_reset(); teardown(s); return h;
}
static void case_dormant(const char* self) {
	uint32_t hashes[2] = {0};
	for(unsigned i = 0; i < 2; i++) {
		char cmd[1024];
#ifdef _WIN32
		snprintf(cmd,sizeof(cmd),"\"\"%s\" dormant-run %s\"",self,i?"on":"off");
		FILE* child = _popen(cmd,"r");
#else
		snprintf(cmd,sizeof(cmd),"\"%s\" dormant-run %s",self,i?"on":"off");
		FILE* child = popen(cmd,"r");
#endif
		assert(child); char line[512];
		while(fgets(line,sizeof(line),child)) {
			fputs(line,stdout); (void)sscanf(line,"DORMANT_DIGEST %x",&hashes[i]);
		}
#ifdef _WIN32
		check(_pclose(child) == 0,"dormant child passed");
#else
		check(pclose(child) == 0,"dormant child passed");
#endif
	}
	check(hashes[0] && hashes[0] == hashes[1],"observe-only and disarmed simulation byte digest identical");
}
int main(int argc,char** argv) {
	const char* which = argc > 1 ? argv[1] : "profile";
	harness_init();
	clin_test_set_consume_observer(consumed,NULL);
	if(!strcmp(which,"profile")) case_profile();
	else if(!strcmp(which,"run")) case_run(false);
	else if(!strcmp(which,"retry")) case_run(true);
	else if(!strcmp(which,"fifo")) case_fifo();
	else if(!strcmp(which,"dormant")) case_dormant(argv[0]);
	else if(!strcmp(which,"dormant-run")) (void)dormant_digest(argc>2 && !strcmp(argv[2],"on"));
	else check(false,"unknown case");
	return harness_result("test_split_offer_piston_script",which);
}
