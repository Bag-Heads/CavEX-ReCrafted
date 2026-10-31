/* Copyright (c) 2026 CaveWii_v2 contributors.
 * Host tests drive the production lease and MAIN service dispatch. The clock
 * seam injects elapsed operation costs; it does not certify Wii scheduling. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "config.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "platform/input.h"
#include "platform/audio.h"
#include "platform/audio_music.h"
#include "platform/audio_deferred.h"
#include "platform/displaylist.h"
#include "chunk_mesher.h"
#include "block/blocks.h"
#include "game/game_state.h"
#include "network/client_interface.h"
#include "network/server_interface.h"

static uint32_t clock_us, stage, worker_mode;
static uint32_t test_generation, test_turn;
static const char* audio_fixture;
static uint32_t read_clock(void) { return __atomic_load_n(&clock_us, __ATOMIC_ACQUIRE); }
static void set_clock(uint32_t us) { __atomic_store_n(&clock_us, us, __ATOMIC_RELEASE); }
static bool sender(uint8_t priority) { return priority == 65 || priority == 66; }
static void wait_stage(unsigned target) {
	ptime_t start = time_get();
	while(__atomic_load_n(&stage, __ATOMIC_ACQUIRE) != target) {
		assert(time_diff_us(start, time_get()) < 2000000);
		usleep(1000); /* Host rendezvous, outside the measured lease sleep API. */
	}
}
static void* request_worker(void* unused) {
	(void)unused;
	thread_g9_lease_local(true, 63);
	thread_g9_lease_begin();
	__atomic_store_n(&stage, 1, __ATOMIC_RELEASE);
	wait_stage(2);
	thread_g9_lease_checkpoint();
	struct g9_priority_lease state;
	thread_g9_lease_state(&state, NULL);
	if(worker_mode && state.stats.services) {
		/* Keep the owner live until MAIN has actually entered its blocking
		 * return window. This is a host rendezvous, not Wii scheduling proof. */
		ptime_t start = time_get();
		do {
			thread_g9_lease_state(&state, NULL);
			assert(time_diff_us(start, time_get()) < 2000000);
			if(!state.stats.main_sleep_requested_us) usleep(1000);
		} while(!state.stats.main_sleep_requested_us);
		if(worker_mode == 4) {
			set_clock(state.start+8001);
			do {
				thread_g9_lease_state(&state, NULL);
				assert(time_diff_us(start, time_get()) < 2000000);
				if(!state.failed) usleep(1000);
			} while(!state.failed);
		}
		if(worker_mode == 2) thread_g9_lease_dependency();
		if(worker_mode == 3) thread_g9_lease_cancel();
		thread_g9_lease_end();
		thread_g9_lease_local(false, 63);
		__atomic_store_n(&stage, 3, __ATOMIC_RELEASE);
		return NULL;
	}
	__atomic_store_n(&stage, 3, __ATOMIC_RELEASE);
	wait_stage(4);
	thread_g9_lease_end();
	thread_g9_lease_local(false, 63);
	return NULL;
}
static void* idle_owner(void* unused) {
	(void)unused;
	thread_g9_lease_local(true, 63);
	__atomic_store_n(&stage, 1, __ATOMIC_RELEASE);
	wait_stage(2);
	thread_g9_lease_local(false, 63);
	return NULL;
}
static int baseline_red(void) {
	thread_g9_lease_test_clock(read_clock);
	thread_g9_lease_configure(true, 8000, 500);
	thread_g9_handoff_configure(false);
	thread_g9_lease_sender(sender);
	thread_g9_lease_audio(UINT32_MAX, false);
	set_clock(1);
	struct thread worker;
	assert(thread_create(&worker, request_worker, NULL, 63));
	wait_stage(1);
	/* A request arrives immediately after the old sole end-of-draw point. */
	set_clock(8001);
	__atomic_store_n(&stage, 2, __ATOMIC_RELEASE);
	struct g9_priority_lease s;
	do { thread_g9_lease_state(&s, NULL); } while(!s.waiting);
	set_clock(12002); /* next whole frame reached that point after 4 ms */
	struct g9_lease_token token = thread_g9_lease_service_begin();
	bool timely = token.generation != 0;
	wait_stage(3);
	__atomic_store_n(&stage, 4, __ATOMIC_RELEASE);
	thread_join(&worker);
	thread_g9_lease_state(&s, NULL);
	assert(s.failed && s.stats.stalls == 1 && !s.boosted);
	assert(s.stats.acquired == s.stats.restored);
	thread_g9_lease_test_clock(NULL);
	printf("BASELINE request-after-sole-point: timely=%u stalls=%u (required timely=1)\n",
		(unsigned)timely, s.stats.stalls);
	return timely ? 0 : 1;
}
static struct thread worker;
static uint32_t rpc_cost, mesh_cost;
static enum g9_handoff_result real_service(unsigned operation, uint32_t budget) {
	enum g9_handoff_result result = clin_g9_handoff_service(operation, budget);
	set_clock(read_clock() + (operation == G9_HANDOFF_RPC ? rpc_cost : mesh_cost));
	return result;
}
static void begin_case(unsigned mode, uint32_t start) {
	stage = 0; worker_mode = mode;
	thread_g9_lease_configure(true, 8000, 500);
	thread_g9_handoff_configure(true);
	assert(thread_g9_handoff_test_sequence(test_generation, test_turn));
	thread_g9_lease_sender(sender);
	thread_g9_lease_audio(UINT32_MAX, false);
	set_clock(start);
	assert(thread_create(&worker, request_worker, NULL, 63));
	wait_stage(1);
	set_clock(start+8000u);
	__atomic_store_n(&stage, 2, __ATOMIC_RELEASE);
	ptime_t wall = time_get();
	struct g9_priority_lease s;
	do {
		thread_g9_lease_state(&s, NULL);
		assert(time_diff_us(wall, time_get()) < 2000000);
	} while(!s.waiting && !s.failed);
}
static struct g9_priority_lease finish_case(void) {
	/* Rejected turns must release the server at baseline without extra sleeps. */
	struct g9_priority_lease s;
	thread_g9_lease_state(&s, NULL);
	if(s.waiting && !s.failed) set_clock(s.requested+4001);
	wait_stage(3);
	__atomic_store_n(&stage, 4, __ATOMIC_RELEASE);
	thread_join(&worker);
	thread_g9_lease_state(&s, NULL);
	assert(!s.active && !s.boosted && !thread_g9_lease_defer());
	assert(s.stats.acquired == s.stats.restored);
	return s;
}
static void real_input(unsigned cost) {
	struct g9_lease_token token = thread_g9_handoff_work_begin(G9_HANDOFF_INPUT);
	input_poll(); /* Production edge retirement, no fabricated input credit. */
	set_clock(read_clock()+cost);
	thread_g9_handoff_work_end(token, G9_HANDOFF_INPUT, true);
}
static struct audio_service_limits host_audio_limits(void);
static void real_audio(unsigned cost) {
	struct g9_lease_token token = thread_g9_handoff_work_begin(G9_HANDOFF_AUDIO);
	struct audio_service_limits limits = host_audio_limits();
	struct audio_service_budget b = {.start=time_get(),
		.cap_us=thread_g9_handoff_audio_remaining(), .limits=&limits};
	enum audio_service_result r = audio_update_service((vec3){0,0,0}, 0, &b);
	if(r == AUDIO_SERVICE_DONE) r = audio_music_update_service(&b);
	struct audio_service_witness w;
	uint32_t reserve = 0;
	bool valid = r == AUDIO_SERVICE_DONE && audio_music_service_witness(&w)
		&& audio_service_wait_reserve(&w, &limits, 4000, &reserve);
	set_clock(read_clock()+cost);
	thread_g9_lease_audio(reserve, !valid);
	thread_g9_handoff_work_end(token, G9_HANDOFF_AUDIO, valid);
}
/* R18 audio continuation host gate: real decoder, ring, mutex and service.
 * The PCM sink is existing named device-output instrumentation; E below is
 * a HOST admission control, never a measured native allowance. */
void audio_music_test_set_pcm_sink(void (*sink)(const int16_t*,uint32_t,uint32_t));
void audio_music_test_set_backend_ready(int ready);
struct thread_mutex* audio_music_test_service_mutex(void);
static int16_t audio_tape[2][600000];
static size_t audio_length[2];
static unsigned audio_replay;
static void audio_sink(const int16_t* pcm, uint32_t frames, uint32_t rate) {
    assert(rate && audio_length[audio_replay]+frames <= 600000);
    memcpy(audio_tape[audio_replay]+audio_length[audio_replay],pcm,frames*sizeof(int16_t));
    audio_length[audio_replay]+=frames;
}
static struct audio_service_limits host_audio_limits(void) {
    struct audio_service_limits l={.bookkeeping_us=4, .wake_refill_us=300,
        .backend_us=12000, .drift_us=1000, .max_age_us=50000, .margins_verified=true};
    for(unsigned i=0;i<AUDIO_UNIT_COUNT;i++) l.e_us[i]=300;
    return l;
}
static void audio_witness_cases(void) {
    struct audio_service_limits l=host_audio_limits();
    struct audio_service_witness w;
    uint32_t reserve;
    /* Produce current inactive evidence through actual ordinary/native seam,
     * rather than treating inactive == UINT32_MAX as a permanent exemption. */
    audio_update((vec3){0,0,0},0);
    assert(audio_music_service_witness(&w) && w.inactive);
    assert(audio_service_wait_reserve(&w,&l,4000,&reserve) && reserve < UINT32_MAX);
    l.margins_verified=false;
    assert(!audio_service_wait_reserve(&w,&l,0,&reserve)); l.margins_verified=true;
    struct audio_service_witness stale=w;
    stale.sampled_at=time_add_ms(stale.sampled_at,0);
    thread_msleep(2); l.max_age_us=1000;
    assert(!audio_service_wait_reserve(&stale,&l,0,&reserve)); l.max_age_us=50000;
    w.valid=false; assert(!audio_service_wait_reserve(&w,&l,0,&reserve));
    assert(audio_music_service_witness(&w));
    uint64_t margin=(uint64_t)l.wake_refill_us+l.backend_us+l.drift_us;
    l.max_age_us=(uint32_t)margin+4000; /* exact boundary refuses */
    assert(!audio_service_wait_reserve(&w,&l,4000,&reserve)); l.max_age_us=50000;
    audio_stop_all(); /* generation invalidates earlier inactive evidence */
    assert(!audio_service_wait_reserve(&w,&l,0,&reserve));
    puts("PASS: current inactive generation, absent/stale/exact-boundary/exhausted witness and unknown margins deny wait");
}
static void audio_continuation_cases(void) {
    gstate.config_user=(struct config){.root=json_value_init_object()};
    audio_init(false,0); /* real decode/ring, explicitly no physical PC device */
    audio_music_test_set_backend_ready(1);
    audio_music_init(100);
    audio_music_test_set_pcm_sink(audio_sink);
    struct audio_service_limits l=host_audio_limits();
    struct audio_service_metrics m={0};
    uint32_t probe_starts[2],probe_rejects[2];
    for(audio_replay=0;audio_replay<2;audio_replay++) {
        uint32_t starts_before=audio_music_probe_starts(),rejects_before=audio_music_probe_rejects();
	    audio_music_play_record(audio_fixture);
        assert(audio_music_is_playing());
        if(audio_replay) {
            struct thread_mutex* lock=audio_music_test_service_mutex();
            struct thread_mutex_guard guard;
            /* A real held mutex refuses its second nonblocking guard on PC. */
            thread_mutex_lock(lock);
            assert(!thread_mutex_try_guard(lock,&guard));
            struct audio_service_budget b={.start=time_get(),.cap_us=500,.limits=&l,.metrics=&m};
            assert(audio_music_update_service(&b)==AUDIO_SERVICE_UNSAFE);
            struct audio_service_witness w;
            assert(!audio_music_service_witness(&w));
            thread_mutex_unlock(lock);
        }
        for(unsigned frames=0; audio_music_is_playing(); frames++) {
            assert(frames < 100000);
            if(!audio_replay) audio_music_update();
            else {
                struct audio_service_budget b={.start=time_get(),.cap_us=500,.limits=&l,.metrics=&m};
                enum audio_service_result r=audio_music_update_service(&b);
                /* Both insufficient budget and unsupported wake/close escape
                 * through real ordinary service; never fabricated credit. */
                if(r!=AUDIO_SERVICE_DONE) audio_music_update();
            }
            thread_msleep(1);
        }
        probe_starts[audio_replay]=audio_music_probe_starts()-starts_before;
        probe_rejects[audio_replay]=audio_music_probe_rejects()-rejects_before;
    }
    assert(probe_starts[0]==probe_starts[1] && probe_rejects[0]==probe_rejects[1]);
    assert(audio_length[0] && audio_length[0]==audio_length[1]);
    assert(!memcmp(audio_tape[0],audio_tape[1],audio_length[0]*sizeof(int16_t)));
    audio_music_test_set_pcm_sink(NULL);
    audio_music_stop();
    audio_music_play_record(audio_fixture);
    struct audio_service_witness w;
    assert(!audio_music_service_witness(&w)); /* opening/pending forbids credit */
    audio_music_stop(); audio_music_shutdown();
    audio_shutdown();
    audio_music_test_set_backend_ready(-1);
    config_destroy(&gstate.config_user);
    audio_witness_cases();
    /* A real pending deferred job must escape ordinary, never hide its
     * file/decode/voice-start work in the admitted snapshot. */
    static int16_t scratch[16] __attribute__((aligned(32)));
    assert(audio_deferred_attach(scratch,sizeof scratch));
    struct audio_deferred_source src={.path="never-opened-service.wav",.frames=16,.rate=48000};
    assert(audio_deferred_request(&src,SOUND_AMBIENT_CAVE,0,(float[3]){0,0,0},1,1));
    struct audio_service_budget pending={.start=time_get(),.cap_us=500,.limits=&l,.metrics=&m};
    assert(audio_update_service((vec3){0,0,0},0,&pending)==AUDIO_SERVICE_UNSAFE);
    assert(!audio_music_service_witness(&w));
    audio_deferred_cancel(); audio_deferred_shutdown();
    audio_menu_event(MENU_SOUND_FOCUS,time_get());
    pending.start=time_get();
    assert(audio_update_service((vec3){0,0,0},0,&pending)==AUDIO_SERVICE_UNSAFE);
    assert(!audio_music_service_witness(&w));
    audio_update((vec3){0,0,0},0); /* same OFF cue arbitration/dispatch once */
    assert(audio_music_service_witness(&w));
    printf("PASS: real decoder/worker/ring PCM %zu frames and probe decisions byte-identical OFF/continuation, EOF/cancel/restart/contention, deferred/cue rejection and ordinary wake escape\n",audio_length[0]);
}
static void config_cases(void) {
	struct config c = {.root = json_value_init_object()};
	assert(!config_read_g9_lease(&c).main_handoff);
	for(unsigned lease=0; lease<2; lease++) for(unsigned handoff=0; handoff<2; handoff++) {
		config_set_int(&c, "experimental.g9_priority_lease_int", lease);
		config_set_int(&c, "experimental.g9_main_handoff_int", handoff);
		struct config_g9_lease cfg = config_read_g9_lease(&c);
		assert(cfg.enabled == lease && cfg.main_handoff == handoff);
		thread_g9_lease_configure(cfg.enabled, cfg.max_us, cfg.service_us);
		thread_g9_handoff_configure(cfg.main_handoff);
		assert(!thread_g9_handoff_enabled()); /* no local owner, including remote */
		gstate.remote_session = true;
		assert(!thread_g9_handoff_dispatch(G9_HANDOFF_ALL, real_service));
		gstate.remote_session = false;
		stage = 0;
		struct thread owner;
		assert(thread_create(&owner, idle_owner, NULL, 63));
		wait_stage(1);
		assert(thread_g9_handoff_enabled() == (lease && handoff));
		__atomic_store_n(&stage, 2, __ATOMIC_RELEASE);
		thread_join(&owner);
	}
	const char* invalid[] = {"1.5", "2", "-1", "true", "\"1\"", "null"};
	for(unsigned k=0; k<sizeof(invalid)/sizeof(invalid[0]); k++) {
		json_object_dotset_value(json_object(c.root), "experimental.g9_main_handoff_int",
			json_parse_string(invalid[k]));
		assert(!config_read_g9_lease(&c).main_handoff);
	}
	config_destroy(&c);
	thread_g9_lease_configure(true, 8000, 2000);
	thread_g9_handoff_configure(true);
	struct g9_priority_lease s;
	thread_g9_lease_state(&s, NULL); assert(s.service_us == 500);
	puts("PASS: strict default-OFF config, all four lease/handoff combinations, remote/no owner inert");
}
static void early_cases(void) {
	for(unsigned mode=1; mode<=3; mode++) {
		begin_case(mode, mode == 3 ? UINT32_MAX-3000u : 1);
		rpc_cost=30; mesh_cost=40;
		/* Real early RPC seam; no GPU retirement/input actions are permitted. */
		assert(!thread_g9_handoff_dispatch(G9_HANDOFF_RPC, real_service));
		if(mode == 1)
			clin_rpc_send(&(struct client_rpc){.type=CRPC_TIME_SET, .payload.time_set=777});
		real_audio(50); real_input(60);
		/* The real post-flip/complete draw seam admits the fixed mesh quantum. */
		if(mode == 1) {
			assert(!thread_g9_handoff_dispatch(G9_HANDOFF_RPC|G9_HANDOFF_MESH, real_service));
			assert(gstate.world_time == 777);
		}
		assert(thread_g9_handoff_dispatch(G9_HANDOFF_RPC|G9_HANDOFF_MESH, real_service));
		struct g9_priority_lease s = finish_case();
		printf("mode=%u services=%u failed=%u stalls=%u late=%u faults=%u\n",
			mode, s.stats.services, s.failed, s.stats.stalls, s.stats.late_ack, s.stats.faults);
		assert(s.stats.services == 1 && !s.failed && !s.stats.stalls && !s.stats.late_ack);
		printf("early mode=%u service=%u return_window=%u\n", mode,
			s.stats.service_max_us, s.stats.main_sleep_requested_us);
		assert(s.stats.service_max_us == (mode == 1 ? 280 : 210)
			&& s.stats.main_sleep_requested_us == 8000);
	}
	puts("PASS: request-after-old-point, real RPC/mesh/input/audio, <=500us; end/dependency/cancel wake and clock wrap");
}
static void rejection_cases(void) {
	for(unsigned oversized=0; oversized<4; oversized++) {
		begin_case(0, 1);
		rpc_cost = oversized==0 ? 501 : 10;
		mesh_cost = oversized==1 ? 501 : 10;
		(void)thread_g9_handoff_dispatch(G9_HANDOFF_RPC, real_service);
		real_input(oversized==2 ? 501 : 10);
		real_audio(oversized==3 ? 501 : 10);
		assert(!thread_g9_handoff_dispatch(G9_HANDOFF_RPC|G9_HANDOFF_MESH, real_service));
		struct g9_priority_lease s = finish_case();
		assert(s.failed && s.stats.late_ack == 1 && !s.stats.services);
		assert(s.stats.service_max_us > 500 && !s.stats.main_sleep_requested_us);
	}
	begin_case(0, 1); rpc_cost = mesh_cost = 10;
	(void)thread_g9_handoff_dispatch(G9_HANDOFF_RPC, real_service);
	set_clock(read_clock()+501); /* Safe seams do not conceal frame/dispatch time. */
	real_input(10);
	struct g9_priority_lease s = finish_case();
	assert(s.failed && s.stats.late_ack == 1 && s.stats.service_max_us > 500);
	begin_case(0, 1);
	rpc_cost = mesh_cost = 10;
	real_input(10); real_audio(10);
	(void)thread_g9_handoff_dispatch(G9_HANDOFF_RPC, real_service);
	/* A draw still in flight grants no mesh capability. No partial ACK. */
	assert(!thread_g9_handoff_dispatch(G9_HANDOFF_RPC, real_service));
	s = finish_case();
	assert(!s.stats.services && s.stats.stalls == 1);
	begin_case(0, 1);
	set_clock(12002);
	assert(!thread_g9_handoff_dispatch(G9_HANDOFF_ALL, real_service));
	s = finish_case(); assert(s.failed && s.stats.stalls == 1);
	begin_case(0, 1);
	struct g9_lease_token token = thread_g9_lease_service_begin();
	assert(!thread_g9_lease_service_end((struct g9_lease_token){token.generation-1, token.turn}, 1, true));
	assert(!thread_g9_lease_service_end((struct g9_lease_token){token.generation, token.turn+1}, 1, true));
	thread_g9_lease_cancel();
	assert(!thread_g9_lease_service_end(token, 1, true));
	s = finish_case(); assert(!s.stats.services);
	begin_case(0, 1);
	real_audio(10); real_input(10);
	thread_g9_lease_audio(1000, true); /* pending refill rejects a complete service */
	assert(!thread_g9_handoff_dispatch(G9_HANDOFF_RPC|G9_HANDOFF_MESH, real_service));
	s = finish_case(); assert(s.failed && s.stats.audio_reject && !s.stats.services);
	thread_g9_handoff_test_priority(false);
	begin_case(0, 1);
	assert(!thread_g9_handoff_dispatch(G9_HANDOFF_ALL, real_service));
	s = finish_case(); assert(s.failed && s.stats.faults == 1 && !s.stats.acquired);
	thread_g9_handoff_test_priority(true);
	test_generation = UINT32_MAX;
	begin_case(0, 1);
	s = finish_case(); assert(s.failed && !s.stats.acquired && !s.stats.services);
	test_generation = 0; test_turn = UINT32_MAX;
	begin_case(0, 1);
	s = finish_case(); assert(s.failed && s.stats.faults == 1 && !s.stats.services);
	test_turn = 0;
	puts("PASS: >500us input/RPC/mesh/audio, missing retirement, >4ms expiry, stale/cancelled tokens, pending refill reject baseline");
}
static uint16_t gpu_completed;
static uint16_t gpu_read(void) { return gpu_completed; }
static void gpu_seal(uint16_t token) { assert(token == 1); }
static bool gpu_main(void) { return true; }
static void gpu_retirement_case(void) {
	displaylist_gpu_configure(&(struct displaylist_gpu_hooks){
		.read_completed=gpu_read, .seal=gpu_seal, .is_main=gpu_main}, 0);
	void* bytes = malloc(64); assert(bytes); memset(bytes, 0xA5, 64);
	/* Inject a submitted descriptor, then use the REAL backend's retirement
	 * queue and fence. Real GL/GX submission is covered by the L205 suite. */
	struct displaylist list = {.data=bytes, .length=64, .submitted=true, .last_submit=1};
	assert(displaylist_gpu_seal() == 1);
	displaylist_destroy(&list);
	assert(displaylist_gpu_pending() == 1 && *(uint8_t*)bytes == 0xA5);
	begin_case(0, 1); rpc_cost=mesh_cost=10;
	assert(!thread_g9_handoff_dispatch(G9_HANDOFF_MESH, real_service));
	struct g9_priority_lease s = finish_case();
	assert(s.failed && s.stats.faults == 1 && !s.stats.services);
	assert(displaylist_gpu_pending() == 1 && *(uint8_t*)bytes == 0xA5);
	gpu_completed=1; displaylist_gpu_reap();
	assert(!displaylist_gpu_pending());
	displaylist_gpu_configure(NULL, 0);
	puts("PASS: real retirement queue/fence rejects mesh service while submitted storage remains in flight");
}
static void cpu_return_expiry_case(void) {
	begin_case(4, 1); rpc_cost=mesh_cost=10;
	real_input(10); real_audio(10);
	assert(thread_g9_handoff_dispatch(G9_HANDOFF_RPC|G9_HANDOFF_MESH, real_service));
	struct g9_priority_lease s = finish_case();
	assert(s.failed && s.stats.faults == 1 && s.stats.services == 1 && s.stats.overshoot_us == 1);
	assert(!thread_g9_lease_service_end((struct g9_lease_token){s.generation, s.turn}, 1, true));
	puts("PASS: production CPU-return deadline fails baseline, reports overshoot, rejects duplicate ACK");
}
static void audio_no_credit_cases(void) {
    for(unsigned mode=0; mode<3; mode++) {
        begin_case(0,1);
        struct g9_lease_token token=thread_g9_handoff_work_begin(G9_HANDOFF_AUDIO);
        assert(token.generation);
        struct audio_service_budget b;
        audio_service_get_budget(&b,thread_g9_handoff_audio_remaining());
        if(mode==1) b.cap_us=0;
        if(mode==2) b.calibration=true;
        enum audio_service_result r=audio_update_service((vec3){0,0,0},0,&b);
        if(r==AUDIO_SERVICE_DONE) r=audio_music_update_service(&b);
        assert(mode==0 ? r==AUDIO_SERVICE_UNSAFE : mode==1 ? r==AUDIO_SERVICE_MORE : r==AUDIO_SERVICE_DONE);
        bool complete=r==AUDIO_SERVICE_DONE && b.limits->margins_verified && !b.calibration;
        assert(!complete); /* even structurally DONE calibration has no credit */
        thread_g9_handoff_work_end(token,G9_HANDOFF_AUDIO,complete);
        thread_g9_handoff_audio_abort();
        assert(!thread_g9_handoff_dispatch(G9_HANDOFF_ALL,real_service));
        struct g9_priority_lease s=finish_case();
        assert(!s.stats.services && !s.stats.main_sleep_requested_us);
    }
    puts("PASS: replacement UNSAFE/MORE and calibration DONE through real MAIN dispatch never grant completion or wait credit");
}
static unsigned audio_guard_calls, audio_guard_mode;
/* Named wait-entry evidence invalidation double, not native latency. */
static bool audio_guard_double(uint32_t wait, uint32_t remaining) {
    assert(wait && remaining<=500);
    audio_guard_calls++;
    return audio_guard_mode && audio_guard_calls==1;
}
static void audio_wait_entry_cases(void) {
    for(audio_guard_mode=0; audio_guard_mode<2; audio_guard_mode++) {
        begin_case(0,1); rpc_cost=mesh_cost=10;
        real_audio(10); real_input(10);
        audio_guard_calls=0;
        thread_g9_handoff_audio_guard(audio_guard_double);
        bool ack=thread_g9_handoff_dispatch(G9_HANDOFF_RPC|G9_HANDOFF_MESH,real_service);
        assert(ack==(audio_guard_mode==1));
        struct g9_priority_lease s=finish_case();
        assert(s.failed && !s.stats.main_sleep_requested_us);
        assert(audio_guard_mode ? audio_guard_calls==2 && s.stats.services==1 && s.stats.faults==1
            : audio_guard_calls==1 && !s.stats.services && s.stats.audio_reject);
    }
    puts("PASS: fresh audio validator before ACK and again at actual wait entry; generation/reserve invalidation prevents sleep");
}
static struct thread_mutex held_stripe;
static void* stripe_owner(void* unused) {
	(void)unused;
	thread_g9_lease_local(true, 63); thread_g9_lease_begin();
	thread_stripe_lock(&held_stripe);
	__atomic_store_n(&stage, 1, __ATOMIC_RELEASE);
	wait_stage(2);
	thread_stripe_unlock(&held_stripe);
	thread_g9_lease_end(); thread_g9_lease_local(false, 63);
	__atomic_store_n(&stage, 3, __ATOMIC_RELEASE);
	return NULL;
}
static void held_stripe_timer_case(void) {
	assert(thread_mutex_init(&held_stripe)); thread_stripe_register(&held_stripe, 0);
	stage=0; set_clock(1);
	thread_g9_lease_configure(true, 8000, 500); thread_g9_handoff_configure(true);
	thread_g9_lease_sender(sender); thread_g9_lease_audio(UINT32_MAX, false);
	assert(thread_create(&worker, stripe_owner, NULL, 63)); wait_stage(1);
	set_clock(8001);
	assert(thread_g9_handoff_test_timer()); /* Same production native-timer body. */
	struct g9_priority_lease s; uint32_t depth;
	thread_g9_lease_state(&s, &depth);
	assert(s.waiting && !s.boosted && depth == 1 && s.stats.timer_restores == 1);
	assert(!thread_g9_lease_defer()); /* pending consumer dependency removes deferral */
	assert(!thread_g9_handoff_dispatch(G9_HANDOFF_ALL, real_service));
	set_clock(12002); (void)thread_g9_handoff_dispatch(0, NULL);
	__atomic_store_n(&stage, 2, __ATOMIC_RELEASE); wait_stage(3); thread_join(&worker);
	thread_g9_lease_state(&s, NULL);
	assert(s.failed && !s.stats.services && s.stats.acquired == s.stats.restored);
	thread_mutex_destroy(&held_stripe);
	puts("PASS: real held stripe + injected production timer expiry prevents deferral/ACK/reboost");
}
static uint32_t consumed;
static void consume(const struct client_rpc* call, void* unused) {
	(void)unused;
	if(call->type == CRPC_TIME_SET) consumed++;
}
static void fifo_cases(void) {
	clin_test_set_consume_observer(consume, NULL);
	clin_rpc_send(&(struct client_rpc){.type=CRPC_TIME_SET, .payload.time_set=17});
	clin_rpc_send(&(struct client_rpc){.type=CRPC_SET_BLOCK});
	clin_rpc_send(&(struct client_rpc){.type=CRPC_TIME_SET, .payload.time_set=19});
	begin_case(0, 1); rpc_cost=mesh_cost=10;
	assert(!thread_g9_handoff_dispatch(G9_HANDOFF_RPC, real_service));
	assert(consumed == 1);
	assert(!thread_g9_handoff_dispatch(G9_HANDOFF_RPC, real_service));
	struct g9_priority_lease s=finish_case();
	assert(s.failed && s.stats.faults == 1 && !s.stats.services && consumed == 1);
	clin_update_drain(); /* Real baseline resumes held head then next message. */
	assert(consumed == 2 && gstate.world_time == 19);
	clin_test_set_consume_observer(NULL, NULL);
	puts("PASS: bounded scalar handler; unproven terrain head retained; baseline FIFO resumes losslessly");
}
int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if(argc > 1 && !strcmp(argv[1], "--baseline-red")) return baseline_red();
	audio_fixture = argc > 1 ? argv[1] : "../tests/fixtures/music_readahead/stream_stereo_12s.ogg";
	config_cases();
	blocks_init(); assert(clin_init() && svin_init() && chunk_mesher_init());
	thread_g9_lease_test_clock(read_clock);
	early_cases(); rejection_cases(); fifo_cases(); gpu_retirement_case();
	thread_g9_handoff_configure(false);
	audio_continuation_cases();
	cpu_return_expiry_case(); held_stripe_timer_case(); audio_no_credit_cases(); audio_wait_entry_cases();
	thread_g9_handoff_configure(false);
	thread_g9_lease_test_clock(NULL);
	chunk_mesher_shutdown();
	puts("test_g9_main_handoff: PASS (native/Wii timing and worker/audio windows unconfirmed)");
	return 0;
}
