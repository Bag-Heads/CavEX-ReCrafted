#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "platform/thread.h"
#include "platform/g9_capture.h"
#include "platform/audio_stream_ring.h"
#include "platform/time.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "item/items.h"
#include "network/server_world.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "game/game_state.h"
#include "lighting.h"
#include "column_view.h"
#include "world.h"
#include "platform/mem2.h"
#include "support/test_fixture_dir.h"
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

static struct g9_priority_lease fresh(void) {
	return (struct g9_priority_lease){.enabled=true, .max_us=8000,
		.service_us=500, .wait_us=4000, .stats.reserve_min_us=UINT32_MAX};
}
static void model_tests(void) {
	struct config c = {.root=json_value_init_object()};
	struct config_g9_lease cfg = config_read_g9_lease(&c);
	assert(!cfg.enabled && cfg.max_us == 8000 && cfg.service_us == 500);
	config_set_int(&c, "experimental.g9_priority_lease_int", 1);
	assert(config_read_g9_lease(&c).enabled);
	json_object_dotset_number(json_object(c.root), "experimental.g9_priority_lease_int", 1.5);
	config_set_int(&c, "experimental.g9_priority_lease_us_int", 8001);
	assert(!config_read_g9_lease(&c).enabled && config_read_g9_lease(&c).max_us == 8000);
	config_destroy(&c);
	struct g9_priority_lease s = fresh();
	assert(!g9_lease_begin(&s, false, true, 0, UINT32_MAX, false));
	assert(!g9_lease_begin(&s, true, false, 0, UINT32_MAX, false));
	s.enabled=false;
	assert(!g9_lease_begin(&s, true, true, 0, UINT32_MAX, false));
	s=fresh();
	assert(!g9_lease_begin(&s, true, true, 0, 14000, false));
	assert(!g9_lease_boost(&s, 1) && s.stats.audio_reject == 1);
	g9_lease_end(&s, 2);
	assert(!g9_lease_begin(&s, true, true, 3, UINT32_MAX, true));
	g9_lease_end(&s, 4);
	assert(g9_lease_begin(&s, true, true, 5, 14001, false));
	assert(g9_lease_boost(&s, 5));
	g9_lease_request(&s, 8005);
	struct g9_lease_token token={s.generation, s.turn};
	assert(!g9_lease_ack(&s, (struct g9_lease_token){token.generation, token.turn+1},
		8006, 100, 20000, false, true));
	assert(g9_lease_ack(&s, token, 8105, 100, 20000, false, true));
	assert(g9_lease_boost(&s, 8105));
	g9_lease_request(&s, 16105);
	token=(struct g9_lease_token){s.generation, s.turn};
	assert(!g9_lease_ack(&s, token, 16106, 501, 20000, false, true));
	assert(s.failed && !s.boosted && s.stats.late_ack == 1);
	g9_lease_end(&s, 16107);
	assert(g9_lease_begin(&s, true, true, 16108, 20000, false));
	assert(!g9_lease_ack(&s, token, 16109, 1, 20000, false, true));
	assert(g9_lease_boost(&s, 16109));
	g9_lease_request(&s, 24108);
	token=(struct g9_lease_token){s.generation, s.turn};
	assert(!g9_lease_ack(&s, token, 28109, 100, 20000, false, true));
	assert(s.failed && !s.boosted);
	g9_lease_end(&s, 28110);
	assert(g9_lease_begin(&s, true, true, UINT32_MAX-3000u, 20000, false));
	assert(g9_lease_boost(&s, UINT32_MAX-3000u));
	g9_lease_request(&s, 4999); /* clock wrap, exactly 8 ms */
	g9_lease_expire_ack(&s, 9000);
	assert(s.failed && s.stats.stalls == 1 && !s.boosted);
	g9_lease_end(&s, 9001);
	assert(s.stats.acquired == s.stats.restored);
	assert(g9_lease_begin(&s, true, true, 10000, 20000, false));
	assert(g9_lease_boost(&s, 10000));
	g9_lease_request(&s, 18000);
	token=(struct g9_lease_token){s.generation, s.turn};
	assert(!g9_lease_ack(&s, token, 18100, 100, 20000, false, false));
	assert(s.failed && !s.boosted && s.stats.faults == 1);
	g9_lease_end(&s, 18101);
	struct audio_stream_ring ring;
	audio_stream_ring_init(&ring, 9, 4096);
	assert(audio_stream_ring_reserve_us(&ring, 0) == 0);
	ring.running=true; ring.rate=48000; ring.written_samples=8192;
	assert(audio_stream_ring_reserve_us(&ring, 0) == (8192u-624u)*1000000ull/48000u);
	assert(audio_stream_ring_reserve_us(&ring, 100) == (8192u-4800u-624u)*1000000ull/48000u);
	assert(audio_stream_ring_reserve_us(&ring, 171) == 0);
	puts("PASS: OFF/remote/no-work, generation/turn/clock wrap, late ACK, consumer stall, audio reserve");
}
static uint32_t sender_priority=65, sender_refuse;
static bool sender(uint8_t priority) {
	if(priority == 66 && __atomic_load_n(&sender_refuse, __ATOMIC_RELAXED)) return false;
	__atomic_store_n(&sender_priority, priority, __ATOMIC_RELAXED);
	return true;
}
static void restored(void) {
	struct g9_priority_lease s;
	thread_g9_lease_state(&s, NULL);
	assert(!s.active && !s.boosted && !s.waiting);
	assert(s.stats.acquired == s.stats.restored);
	assert(s.stats.priority_sets == 2*s.stats.acquired);
	assert(__atomic_load_n(&sender_priority, __ATOMIC_RELAXED) == 65);
}
static uint32_t stage;
/* Host-only rendezvous preserves the state under inspection. PC priority is
 * a no-op and the lease clock is frozen; this is not native scheduling proof. */
static void boosted_host_barrier(uint32_t wanted) {
	ptime_t start=time_get();
	while(__atomic_load_n(&stage,__ATOMIC_ACQUIRE) != wanted) {
		assert(time_diff_us(start,time_get()) < 2000000);
		thread_yield();
	}
}
static struct thread_channel blocked_queue;
static uint32_t fake_us, clock_step;
static uint32_t lease_clock(void) {
	uint32_t step=__atomic_load_n(&clock_step,__ATOMIC_RELAXED);
	return __atomic_add_fetch(&fake_us,step,__ATOMIC_RELAXED);
}
static void stage_wait(uint32_t wanted) {
	ptime_t start=time_get();
	while(__atomic_load_n(&stage, __ATOMIC_ACQUIRE) != wanted) {
		assert(time_diff_us(start,time_get()) < 2000000);
		thread_msleep(1);
	}
}
static void* runtime_worker(void* arg) {
	(void)arg;
	thread_g9_lease_local(true, 63);
	struct thread_mutex a,b;
	assert(thread_mutex_init(&a) && thread_mutex_init(&b));
	thread_stripe_register(&a, 0);
	__atomic_store_n(&sender_refuse,1,__ATOMIC_RELAXED);
	thread_g9_lease_begin();
	struct g9_priority_lease s;
	thread_g9_lease_state(&s,NULL);
	assert(s.failed && !s.boosted && s.stats.faults == 1);
	thread_g9_lease_end(); restored();
	__atomic_store_n(&sender_refuse,0,__ATOMIC_RELAXED);
	thread_g9_lease_begin();
	uint32_t depth;
	thread_g9_lease_state(&s, &depth);
	assert(s.active && s.boosted && !depth);
	uint32_t acquired = s.stats.acquired, setters = s.stats.priority_sets;
	thread_stripe_lock(&a);
	thread_g9_lease_state(&s, &depth);
	assert(s.boosted && depth == 1);
	thread_mutex_lock(&b);
	thread_mutex_unlock(&b);
	thread_g9_lease_state(&s, &depth);
	assert(s.boosted && depth == 1);
	thread_stripe_unlock(&a);
	thread_g9_lease_state(&s, &depth);
	assert(!depth && s.boosted && s.stats.acquired == acquired && s.stats.priority_sets == setters);
	for(unsigned k=0;k<274;k++) thread_g9_lease_checkpoint();
	assert(tchannel_send(&blocked_queue,&blocked_queue,true));
	void* fast_token=NULL;
	assert(tchannel_receive(&blocked_queue,&fast_token,true) && fast_token==&blocked_queue);
	thread_g9_lease_state(&s,&depth);
	assert(!depth && s.boosted && s.stats.acquired==acquired && s.stats.priority_sets==setters);
	thread_drain_recovery_sleep();
	thread_g9_lease_end(); restored();
	thread_g9_lease_begin();
	__atomic_fetch_add(&fake_us,9000,__ATOMIC_RELAXED);
	__atomic_store_n(&stage, 1, __ATOMIC_RELEASE);
	thread_g9_lease_checkpoint(); /* server blocks until MAIN ACKs this turn */
	thread_g9_lease_state(&s, &depth);
	printf("runtime ACK: failed=%u boosted=%u services=%u stalls=%u late=%u depth=%u\n",
		s.failed,s.boosted,s.stats.services,s.stats.stalls,s.stats.late_ack,depth);
	assert(!s.failed && s.boosted && s.stats.services == 1);
	thread_g9_lease_end(); restored();
	__atomic_store_n(&stage, 2, __ATOMIC_RELEASE);
	stage_wait(3);
	thread_g9_lease_begin();
	__atomic_fetch_add(&fake_us,9000,__ATOMIC_RELAXED);
	__atomic_store_n(&clock_step,1000,__ATOMIC_RELAXED);
	thread_g9_lease_checkpoint(); /* no consumer: safe baseline after deadline */
	__atomic_store_n(&clock_step,0,__ATOMIC_RELAXED);
	thread_g9_lease_state(&s, &depth);
	assert(s.failed && !s.boosted && s.stats.stalls >= 1);
	thread_g9_lease_end(); restored();
	thread_g9_lease_begin();
	__atomic_store_n(&stage,6,__ATOMIC_RELEASE);
	void* envelope=NULL;
	assert(tchannel_receive(&blocked_queue,&envelope,true) && envelope == &blocked_queue);
	thread_g9_lease_end(); restored();
	__atomic_store_n(&stage,7,__ATOMIC_RELEASE);
	stage_wait(8);
	thread_g9_lease_begin();
	thread_g9_lease_state(&s,NULL);
	assert(s.failed && !s.boosted && s.stats.audio_reject);
	uint32_t sleeps = s.stats.sleep_requested_us;
	for(unsigned k=0;k<274;k++) thread_g9_lease_checkpoint();
	thread_g9_lease_state(&s,NULL);
	assert(s.stats.sleep_requested_us == sleeps); /* 70k pops: zero added seam sleep. */
	puts("PASS failed/audio-pending drain: 274 seams, zero requested sleep us added (previously 548000 us)");
	thread_g9_lease_end(); restored();
	__atomic_store_n(&stage,9,__ATOMIC_RELEASE);
	stage_wait(10);
	thread_g9_lease_begin();
	__atomic_store_n(&stage,11,__ATOMIC_RELEASE);
	boosted_host_barrier(12);
	thread_g9_lease_checkpoint(); /* Still-boosted seams must retain the audio gate. */
	thread_g9_lease_state(&s,NULL);
	assert(s.failed && !s.boosted);
	thread_g9_lease_end(); restored();
	__atomic_store_n(&stage,13,__ATOMIC_RELEASE);
	stage_wait(14);
	thread_g9_lease_begin();
	__atomic_store_n(&stage, 4, __ATOMIC_RELEASE);
	stage_wait(5); /* foreign cancellation restores this LIVE server */
	restored();
	thread_g9_lease_begin(); restored(); /* cancelled session cannot rearm */
	thread_mutex_destroy(&a); thread_mutex_destroy(&b);
	thread_g9_lease_local(false, 63);
	return NULL;
}
static void runtime_tests(void) {
	thread_g9_lease_test_clock(lease_clock);
	thread_g9_lease_configure(true, 8000, 500);
	thread_g9_lease_sender(sender);
	thread_g9_lease_audio(UINT32_MAX, false);
	struct g9_lease_stats idle;
	thread_g9_lease_audio(1000,true);
	thread_g9_lease_snapshot(&idle);
	uint32_t idle_sleeps = idle.main_sleep_requested_us;
	for(unsigned k=0;k<274;k++) thread_g9_lease_service_end((struct g9_lease_token){0},0,true);
	thread_g9_lease_snapshot(&idle);
	assert(idle.main_sleep_requested_us == idle_sleeps);
	puts("PASS idle MAIN/audio-pending: 274 services, zero requested sleep us added");
	thread_g9_lease_audio(UINT32_MAX,false);
	gstate.remote_session=true;
	assert(!thread_g9_lease_local_present());
	thread_g9_lease_begin(); restored();
	assert(!thread_g9_lease_defer());
	gstate.remote_session=false;
	struct thread worker;
	assert(tchannel_init(&blocked_queue,1));
	assert(thread_create(&worker, runtime_worker, NULL, 63));
	stage_wait(1);
	struct g9_lease_token token={0};
	ptime_t start=time_get();
	while(!token.generation) {
		token=thread_g9_lease_service_begin();
		assert(time_diff_us(start,time_get()) < 1000000);
		if(!token.generation) thread_msleep(1);
	}
	assert(thread_g9_lease_service_end(token, 100, true));
	stage_wait(2);
	assert(!thread_g9_lease_service_end(token, 100, true)); /* late duplicate */
	__atomic_store_n(&stage, 3, __ATOMIC_RELEASE);
	stage_wait(6);
	struct g9_priority_lease snapshot;
	uint32_t depth=0;
	start=time_get();
	while(!depth) {
		thread_g9_lease_state(&snapshot,&depth);
		assert(time_diff_us(start,time_get()) < 1000000);
		if(!depth) thread_msleep(1);
	}
	assert(!snapshot.boosted);
	assert(snapshot.parked && !thread_g9_lease_defer());
	assert(tchannel_send(&blocked_queue,&blocked_queue,true));
	stage_wait(7);
	thread_g9_lease_audio(1000,true);
	__atomic_store_n(&stage,8,__ATOMIC_RELEASE);
	stage_wait(9);
	thread_g9_lease_audio(UINT32_MAX,false);
	__atomic_store_n(&stage,10,__ATOMIC_RELEASE);
	stage_wait(11);
	assert(thread_g9_lease_defer());
	thread_g9_lease_audio(1000,true);
	__atomic_store_n(&stage,12,__ATOMIC_RELEASE);
	stage_wait(13);
	thread_g9_lease_audio(UINT32_MAX,false);
	__atomic_store_n(&stage,14,__ATOMIC_RELEASE);
	stage_wait(4);
	thread_g9_lease_cancel();
	__atomic_store_n(&stage, 5, __ATOMIC_RELEASE);
	thread_join(&worker);
	tchannel_close(&blocked_queue);
	restored(); assert(!thread_g9_lease_local_present());
	thread_g9_lease_state(&snapshot,NULL);
	assert(snapshot.cancelled && snapshot.stats.cancelled==1);
	thread_g9_lease_test_clock(NULL);
	thread_g9_lease_configure(true,8000,500);
	thread_g9_lease_cancel(); /* Idle cancellation must be visible too. */
	thread_g9_lease_cancel();
	thread_g9_lease_state(&snapshot,NULL);
	assert(snapshot.cancelled && snapshot.stats.cancelled==1 && !snapshot.active);
	puts("PASS: runtime local-only, nested stripe/ordinary locks, COW recovery sleep, exact ACK, consumer stall, foreign cancel/rearm rejection");
	thread_g9_lease_configure(true,8000,500);
	thread_g9_lease_sender(sender); thread_g9_lease_audio(UINT32_MAX,false);
}
static struct thread_mutex contested;
static void* lock_wait_worker(void* arg) {
	(void)arg;
	thread_g9_lease_local(true,63);
	thread_g9_lease_begin();
	__atomic_store_n(&stage,20,__ATOMIC_RELEASE);
	thread_stripe_lock(&contested); /* Failed fast path: MAIN owns it. */
	thread_stripe_unlock(&contested);
	thread_g9_lease_end(); restored();
	thread_g9_lease_local(false,63);
	return NULL;
}
static void lock_owner_test(void) {
	assert(thread_mutex_init(&contested));
	thread_stripe_register(&contested,1);
	thread_stripe_lock(&contested);
	struct thread worker;
	assert(thread_create(&worker,lock_wait_worker,NULL,63));
	stage_wait(20);
	ptime_t start=time_get();
	struct g9_priority_lease s; uint32_t depth;
	do {
		thread_g9_lease_state(&s,&depth);
		assert(time_diff_us(start,time_get()) < 2000000);
		if(!depth) thread_msleep(1);
	} while(!depth);
	assert(s.active && !s.boosted);
	assert(s.parked && !thread_g9_lease_defer());
	thread_stripe_unlock(&contested);
	thread_join(&worker); restored();
	thread_mutex_destroy(&contested);
	puts("PASS: contested MAIN-owned mutex restores before blocking without priority inheritance");
}
static struct server_local fixture;
static struct thread_cond owner_condition;
static void* condition_worker(void* unused) {
	(void)unused;
	thread_g9_lease_local(true,63); thread_g9_lease_begin();
	thread_mutex_lock(&contested);
	__atomic_store_n(&stage,90,__ATOMIC_RELEASE);
	thread_cond_wait(&owner_condition,&contested);
	thread_mutex_unlock(&contested);
	thread_g9_lease_end(); restored(); thread_g9_lease_local(false,63);
	return NULL;
}
static void condition_wait_test(void) {
	assert(thread_mutex_init(&contested) && thread_cond_init(&owner_condition));
	struct thread worker;
	assert(thread_create(&worker,condition_worker,NULL,63));
	stage_wait(90);
	thread_mutex_lock(&contested); /* Acquired only after the real cond wait releases it. */
	struct g9_priority_lease s; uint32_t depth;
	thread_g9_lease_state(&s,&depth);
	assert(s.active && !s.boosted && s.parked && depth==2 && !thread_g9_lease_defer());
	thread_cond_signal(&owner_condition);
	thread_mutex_unlock(&contested);
	thread_join(&worker); restored();
	thread_mutex_destroy(&contested); thread_cond_destroy(&owner_condition);
	puts("PASS uncontended ordinary lock keeps boost; actual condition wait parks and resumes MAIN");
}
static char fixture_dir[220];
static unsigned cow_allocations;
static uint32_t fail_cow;
void* __real_mem2_alloc(size_t bytes);
void* __wrap_mem2_alloc(size_t bytes) {
	if(bytes == COLUMN_VIEW_PRIVATE_BYTES && __atomic_load_n(&fail_cow,__ATOMIC_ACQUIRE)) return NULL;
	return __real_mem2_alloc(bytes);
}
static void cow_allocation(enum carve_action action, uintptr_t a, uintptr_t b,
	size_t bytes, size_t flags) {
	(void)a; (void)b; (void)flags;
	if(action != CARVE_ALLOC || bytes != COLUMN_VIEW_PRIVATE_BYTES) return;
	struct g9_priority_lease s;
	uint32_t depth;
	thread_g9_lease_state(&s,&depth);
	assert(s.active && !s.boosted && depth);
	cow_allocations++;
}
static void fault_hook(struct server_world* w) {
	(void)w; restored();
}
static void* drain_worker(void* arg) {
	(void)arg;
	thread_g9_lease_local(true, 63);
	memset(&fixture,0,sizeof fixture);
	dict_entity_init(fixture.entities);
	test_fixture_dir("test_g9_priority_lease", fixture_dir, sizeof fixture_dir);
	MKDIR(fixture_dir);
	char region[256]; snprintf(region,sizeof region,"%s/region",fixture_dir); MKDIR(region);
	string_t path; string_init_set_str(path,fixture_dir);
	server_world_create(&fixture.worlds[0],path,WORLD_DIM_OVERWORLD,4242,true);
	string_clear(path);
	struct server_world* w=&fixture.worlds[0]; w->session=&fixture; w->local=&fixture;
	for(int x=-1;x<=1;x++) for(int z=-1;z<=1;z++) {
		struct server_chunk* chunk=NULL;
		assert(server_world_load_chunk(w,x,z,&chunk) && chunk);
	}
	assert(server_world_light_drain(w)); restored(); /* empty work */
	struct server_chunk* shared=dict_server_chunks_get(w->chunks,S_CHUNK_ID(0,0));
	assert(shared && shared->backing);
	struct column_view* view=column_view_create(shared->backing,shared->ids,
		shared->metadata,shared->lighting_sky,shared->lighting_torch);
	assert(view);
	column_view_test_page_limit(0);
	mem2_carve_observe=cow_allocation;
	thread_g9_lease_begin();
	column_backing_set_cell(shared->backing,COLUMN_TORCH,90,1);
	column_backing_set_cell(shared->backing,COLUMN_TORCH,90,0);
	thread_g9_lease_end(); restored();
	mem2_carve_observe=NULL;
	assert(cow_allocations == 1 && column_view_get(view,90).torch_light == 0);
	column_view_test_page_limit(COLUMN_VIEW_POOL_PAGES);
	column_view_unref(view);
	puts("PASS: COW raw MEM2 allocation stays at base priority with the stripe unlocked");
	struct server_chunk* second=dict_server_chunks_get(w->chunks,S_CHUNK_ID(1,0));
	assert(second && second->backing);
	struct column_view* views[2]={
		column_view_create(shared->backing,shared->ids,shared->metadata,shared->lighting_sky,shared->lighting_torch),
		column_view_create(second->backing,second->ids,second->metadata,second->lighting_sky,second->lighting_torch)};
	assert(views[0] && views[1]);
	column_view_test_page_limit(0);
	__atomic_store_n(&fail_cow,1,__ATOMIC_RELEASE);
	thread_g9_lease_begin();
	__atomic_store_n(&stage,70,__ATOMIC_RELEASE);
	boosted_host_barrier(71); /* MAIN observes defer before the real COW wait. */
	column_backing_set_cell(shared->backing,COLUMN_TORCH,90,1); /* consumes emergency */
	column_backing_set_cell(second->backing,COLUMN_TORCH,90,1); /* real recovery */
	thread_g9_lease_end(); restored();
	__atomic_store_n(&stage,72,__ATOMIC_RELEASE);
	stage_wait(73);
	__atomic_store_n(&fail_cow,0,__ATOMIC_RELEASE);
	column_view_unref(views[0]); column_view_unref(views[1]);
	column_view_recovery_reset();
	column_view_test_page_limit(COLUMN_VIEW_POOL_PAGES);
	column_backing_set_cell(shared->backing,COLUMN_TORCH,90,0);
	column_backing_set_cell(second->backing,COLUMN_TORCH,90,0);
	puts("PASS: MAIN defers at boost, resumes during real COW exhaustion, accepts recovery; server write completes");
	uint8_t height; assert(server_world_get_height(w,8,8,&height));
	server_world_test_light_fault_hook=fault_hook;
	for(unsigned fault=0;fault<5;fault++) {
		struct column_view* drain_view = fault==0 ? column_view_create(shared->backing,
			shared->ids,shared->metadata,shared->lighting_sky,shared->lighting_torch) : NULL;
		assert(fault!=0 || drain_view);
		unsigned index=(8u*CHUNK_SIZE+8u)*WORLD_HEIGHT+height;
		struct block_data presented = drain_view ? column_view_get(drain_view,index) : (struct block_data){0};
		server_world_test_light_clear_fault(w);
		struct block_data block={.type= fault&1 ? BLOCK_GLOWSTONE : BLOCK_TORCH};
		assert(server_world_set_block(w,8,height,8,block));
		stack_destroy(&w->light_frontier); lighting_drain_queue_init(&w->light_frontier);
		if(fault==0) assert(lighting_drain_reserve(&w->light_frontier,65536));
		struct g9_lease_stats before,after;
		thread_g9_lease_snapshot(&before);
		if(fault==1) lighting_test_drain_fail_reserve_after(0);
		if(fault==2) lighting_test_drain_fail_reserve_after(1);
		if(fault==3) server_world_test_light_drain_ceiling(1);
		bool done=server_world_light_drain(w);
		thread_g9_lease_snapshot(&after);
		if(fault==0) {
			uint32_t pops=after.lease_pops-before.lease_pops;
			assert(pops > 256 && after.acquired-before.acquired == 1);
			assert(after.priority_sets-before.priority_sets == 2);
			assert(after.sender_sets-before.sender_sets == 2);
			struct block_data retained=column_view_get(drain_view,index);
			assert(retained.sky_light==presented.sky_light && retained.torch_light==presented.torch_light);
			assert(nibble_read(shared->lighting_torch,index)>retained.torch_light);
			printf("PASS backed drain: %u pops, %u acquisitions, setters owner=%u sender=%u total=%u; one interval across shared writes\n",
				pops,after.acquired-before.acquired,after.priority_sets-before.priority_sets,
				after.sender_sets-before.sender_sets,
				after.priority_sets-before.priority_sets+after.sender_sets-before.sender_sets);
			column_view_unref(drain_view);
		}
		assert(done == (fault==0 || fault==4));
		lighting_test_drain_fail_reserve_after(-1);
		server_world_test_light_drain_ceiling(0);
		restored();
		if(!done) {
			assert(w->light_seed_n && w->light_fault);
			server_world_test_light_clear_fault(w);
			assert(server_world_light_drain(w)); restored();
		}
	}
	/* Real settled-trial save rolls the live trial field back, preserving debt. */
	assert(server_world_set_block(w,9,height,8,(struct block_data){.type=BLOCK_TORCH}));
	size_t debt=w->light_seed_n;
	assert(server_world_checkpoint_chunks(w));
	assert(w->light_seed_n == debt); restored();
	assert(server_world_light_drain(w)); restored();
	server_world_test_light_fault_hook=NULL;
	server_world_discard(w); restored();
	dict_entity_clear(fixture.entities);
	test_fixture_dir_cleanup(fixture_dir);
	thread_g9_lease_local(false,63);
	return NULL;
}
static struct thread_channel client_gate;
static void* client_owner_worker(void* unused) {
	(void)unused;
	thread_g9_lease_local(true,63);
	__atomic_store_n(&stage,80,__ATOMIC_RELEASE);
	void* token;
	assert(tchannel_receive(&client_gate,&token,true));
	thread_g9_lease_begin();
	__atomic_store_n(&stage,81,__ATOMIC_RELEASE);
	assert(tchannel_receive(&client_gate,&token,true)); /* Active dependency park. */
	thread_g9_lease_end();
	thread_g9_lease_local(false,63);
	return NULL;
}
static void client_light_equivalence(void) {
	enum { CELLS=32768, FRAMES=128 };
	uint8_t* tape=malloc((size_t)CELLS*FRAMES);
	uint8_t* image=calloc(81920,1);
	assert(tape && image);
	uint8_t height[256]={0},tint[256]={0};
	uint32_t works[FRAMES],depths[FRAMES];
	unsigned frames=0;
	struct thread worker;
	assert(tchannel_init(&client_gate,1));
	for(unsigned mode=0;mode<3;mode++) {
		if(mode==1) {
			thread_g9_lease_configure(true,8000,500);
			thread_g9_lease_sender(sender); thread_g9_lease_audio(UINT32_MAX,false);
			assert(thread_create(&worker,client_owner_worker,NULL,63));
			stage_wait(80);
			assert(!thread_g9_lease_active());
		} else if(mode==0) thread_g9_lease_configure(false,8000,500);
		else {
			assert(tchannel_send(&client_gate,&client_gate,true));
			stage_wait(81);
			struct g9_priority_lease s;
			ptime_t start=time_get();
			do {
				thread_g9_lease_state(&s,NULL);
				assert(time_diff_us(start,time_get())<2000000);
				if(!s.parked) thread_msleep(1);
			} while(!s.parked);
			assert(s.active && !s.boosted && !thread_g9_lease_defer());
		}
		struct world w; world_create(&w); w.dimension=WORLD_DIM_OVERWORLD;
		world_build_full_section(&w,0,0,image,image+32768,image+49152,image+65536,height,tint,tint);
		world_set_block(&w,8,64,8,(struct block_data){.type=BLOCK_TORCH},true);
		unsigned f=0;
		do {
			assert(f<FRAMES);
			world_update_lighting(&w);
			if(mode==0) {
				works[f]=(uint32_t)world_lighting_driver_last_work();
				depths[f]=(uint32_t)stack_size(&w.lighting_flood_queue);
			} else {
				assert(works[f]==world_lighting_driver_last_work());
				assert(depths[f]==stack_size(&w.lighting_flood_queue));
			}
			unsigned i=0;
			for(int x=0;x<16;x++) for(int z=0;z<16;z++) for(int y=0;y<128;y++) {
				struct block_data b=world_get_block(&w,x,y,z);
				uint8_t light=(uint8_t)(b.sky_light|(b.torch_light<<4));
				if(mode==0) tape[(size_t)f*CELLS+i]=light;
				else assert(tape[(size_t)f*CELLS+i]==light);
				i++;
			}
			f++;
		} while(!stack_empty(&w.lighting_updates)||!lighting_flood_queue_empty(&w.lighting_flood_queue));
		if(mode==0) frames=f; else assert(frames==f);
		world_destroy(&w);
	}
	assert(tchannel_send(&client_gate,&client_gate,true));
	thread_join(&worker); restored(); tchannel_close(&client_gate);
	free(tape); free(image);
	printf("PASS client-light OFF / ON idle-owner / ON active-parked: %u frames, identical Sky+Torch bytes every frame and frontier depths\n",frames);
}
static unsigned capture_leases[10], capture_drops[8], capture_ends[8];
static bool capture_lease_emit(const char* line) {
	const char* p = strstr(line, "\"rows\":[");
	if(!p) return true; /* seal */
	p += 8;
	while(*p != ']') {
		assert(*p++ == '[');
		uint32_t r[G9_CAPTURE_WORDS];
		for(unsigned i = 0; i < G9_CAPTURE_WORDS; i++) {
			char* end; r[i] = (uint32_t)strtoul(p, &end, 10); assert(end != p); p = end;
			if(i+1 < G9_CAPTURE_WORDS) assert(*p++ == ',');
		}
		assert(*p++ == ']'); if(*p == ',') p++;
		if(r[0] == 10) {
			assert(r[5] < 10); capture_leases[r[5]]++;
			if(r[5] == 3) { assert(r[6] < 8); capture_drops[r[6]]++; }
			if(r[5] == 8) { assert(r[6] < 8); capture_ends[r[6]]++; }
		}
	}
	return true;
}
static void capture_runtime_test(void) {
	assert(g9_capture_configure(true)); g9_capture_arm();
	g9_capture_tick_begin(); g9_capture_random(true, 0, 1, 1);
	runtime_tests(); /* same native lease paths and assertions with observer ON */
	g9_capture_random(false, 0, 1, 1); g9_capture_tick_end(); g9_capture_finish();
	while(g9_capture_pump(capture_lease_emit)) {}
	assert(!g9_capture_pending());
	assert(capture_leases[2] && capture_leases[2] == capture_leases[3]);
	assert(capture_leases[5] >= 276 && capture_leases[6] && capture_leases[9]);
	assert(capture_drops[1] && capture_drops[2] && capture_drops[3] && capture_ends[6]);
	assert(g9_capture_configure(false));
	puts("PASS capture ON: every production boost/drop, end/dependency/cap/cancel reasons, ACK/stall/sender-fault; same lease assertions");
}

int main(int argc, char** argv) {
	setvbuf(stdout,NULL,_IONBF,0);
	/* Repeat real stripe/MQ/COW recovery cases with the R22 dependency gate. */
	thread_g9_handoff_configure(argc > 1 && !strcmp(argv[1], "--handoff"));
	model_tests(); runtime_tests(); capture_runtime_test(); lock_owner_test(); condition_wait_test();
	blocks_init(); items_init(); assert(clin_init() && svin_init());
	thread_g9_lease_test_clock(lease_clock);
	thread_g9_lease_configure(true,8000,500);
	thread_g9_lease_sender(sender); thread_g9_lease_audio(UINT32_MAX,false);
	struct thread worker;
	assert(thread_create(&worker,drain_worker,NULL,63));
	stage_wait(70);
	assert(thread_g9_lease_defer());
	__atomic_store_n(&stage,71,__ATOMIC_RELEASE);
	ptime_t start=time_get();
	while(!column_view_recovery_pending()) {
		assert(time_diff_us(start,time_get()) < 2000000);
		thread_msleep(1);
	}
	struct g9_priority_lease s; uint32_t depth;
	thread_g9_lease_state(&s,&depth);
	assert(s.active && s.parked && depth && !s.boosted && !thread_g9_lease_defer());
	assert(column_view_recovery_begin()); /* Production recovery, no replacement handler. */
	stage_wait(72);
	__atomic_store_n(&stage,73,__ATOMIC_RELEASE);
	thread_join(&worker); restored();
	assert(g9_capture_configure(true)); g9_capture_arm();
	g9_capture_tick_begin();
	client_light_equivalence();
	g9_capture_tick_end();
	assert(g9_capture_configure(false));
	thread_g9_lease_test_clock(NULL);
	puts("PASS: production success/empty, seed and mid-frontier OOM, frontier ceiling, fault retry, checkpoint trial/rollback, unload");
	puts("test_g9_priority_lease: PASS");
	return 0;
}

