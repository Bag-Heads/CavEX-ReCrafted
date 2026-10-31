/* R15 STEP 1 HOST ONLY. No engine edits. The native lease .inc is included
 * verbatim behind named SDK doubles; its actual priority setter dispatches
 * two real host threads. The pooled drain/random/checkpoint/client algorithms
 * call that instance through link wrappers. OFF replay uses the normal pool.
 * SDK timer/IRQ/queue doubles are not hardware timing or SDK certification. */
#include <assert.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <direct.h>
#include <cpuid.h>
#include <x86intrin.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "item/items.h"
#include "entity/combat.h"
#include "network/inventory_logic.h"
#include "column_view.h"
#include "lighting.h"
#include "world.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/g9_drain_workload.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "platform/g9_capture.h"
#include "tps_observer.h"
#include "sha256.h"
#include "config.h"

enum { MAIN = 1, SERVER = 2 };
enum seam { OFF, TERMINAL, EXPIRED_ACK, DEPENDENCY, CONTESTED, WAKE, CAP };
static enum seam seam;
static bool control, native, measuring, dispatch_armed;
/* Optional R25 admission gate. Keep R24's diagnostic/control unchanged by
 * default; the opt-in gate requires B's actual knobs to close the SAME gap. */
static bool admission;
/* Optional --tick-boost gate (G9 tick-wide boost, docs/g9_tick_boost.md).
 * WAKE = lease OFF, MAIN becomes ready mid-tick (VI/timer wake): strict
 * priority runs MAIN iff the server is below it. CAP = lease OFF, the hard
 * per-tick cap timer fires mid-tick. The lease seams compose with the boost.
 * Default and --r25-admission runs keep the boost OFF: their results are
 * unchanged. */
static bool tick_boost_mode, tb_on, seam_fired;
static unsigned lwp_calls;
static bool main_ready, server_runnable;
static __thread unsigned role = MAIN;
static uint32_t model_us = 1;
static double cycles_per_us;
static uint64_t wall_us(void) { return time_diff_us((ptime_t){0}, time_get()); }
static uint64_t cpu_us(void) {
	FILETIME create, exit, kernel, user;
	assert(GetThreadTimes(GetCurrentThread(), &create, &exit, &kernel, &user));
	ULARGE_INTEGER k = {.LowPart=kernel.dwLowDateTime, .HighPart=kernel.dwHighDateTime};
	ULARGE_INTEGER u = {.LowPart=user.dwLowDateTime, .HighPart=user.dwHighDateTime};
	return (k.QuadPart + u.QuadPart) / 10;
}
static uint64_t cpu_cycles(void) {
	ULONG64 value; assert(QueryThreadCycleTime(GetCurrentThread(),&value)); return value;
}
static void calibrate_cycles(void) {
	unsigned a,b,c,d; assert(__get_cpuid(0x80000007,&a,&b,&c,&d) && (d&(1u<<8)));
	double lo=1e30, hi=0, sum=0;
	for(unsigned i=0;i<3;i++) {
		uint64_t start=wall_us(), ticks=__rdtsc(), end;
		do { end=wall_us(); } while(end-start<100000);
		double rate=(double)(__rdtsc()-ticks)/(double)(end-start);
		if(rate<lo) lo=rate;
		if(rate>hi) hi=rate;
		sum+=rate;
	}
	cycles_per_us=sum/3; assert(hi/lo<1.001);
	printf("CPU_CLOCK raw=QueryThreadCycleTime cycles_per_us=%.9f spread_ppm=%.3f CPU_us_estimate=TSC_QPC_scaled\n",cycles_per_us,(hi/lo-1)*1e6);
}
struct account { uint64_t elapsed, cpu, cycles, sync, dispatch, wait_start; unsigned depth; };
static __thread struct account* account;
static struct account srv_account, main_account, main_gap_account;
static void contested_enter(void);
static void wait_observe(enum tps_action action, unsigned part) {
	if(!account || (part != TPS_MUTEX_WAIT && part != TPS_CHANNEL_WAIT && part != TPS_MEM2_WAIT)) return;
	if(action == TPS_ENTER && account->depth++ == 0) account->wait_start = wall_us();
	if(action == TPS_ENTER && role==SERVER && seam==CONTESTED && part==TPS_MUTEX_WAIT) contested_enter();
	if(action == TPS_LEAVE && --account->depth == 0) account->sync += wall_us()-account->wait_start;
}

/* Adapter lock is never a world lock. A condition wait means MODEL RUNNABLE
 * but not selected, and is accounted separately from real lock/channel wait. */
static pthread_mutex_t dispatch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dispatch_wake = PTHREAD_COND_INITIALIZER;
static unsigned selected = MAIN, request, completed, lock_attempt, setup_stage;
static uint64_t transition_at, final_pop_at, restore_return_at, tick_complete_at;
static uint64_t paired_final_pop_at;
static uint64_t main_work_before_restore, main_work_before_tick;
static struct thread_mutex contested;
static uint64_t contested_wait_us;
static void select_role(unsigned next);
static void contested_enter(void) {
	if(lock_attempt!=1) return;
	pthread_mutex_lock(&dispatch_lock); lock_attempt=2; select_role(MAIN); pthread_mutex_unlock(&dispatch_lock);
}
static void select_role(unsigned next) {
	selected = next;
	pthread_cond_broadcast(&dispatch_wake);
}
static void await_role(unsigned wanted) {
	while(selected != wanted) pthread_cond_wait(&dispatch_wake, &dispatch_lock);
}
static void dispatch_drop(void) {
	uint64_t start = wall_us();
	pthread_mutex_lock(&dispatch_lock);
	transition_at = start;
	request++;
	select_role(MAIN);
	await_role(SERVER);
	pthread_mutex_unlock(&dispatch_lock);
	if(account) account->dispatch += wall_us()-start;
}

/* Minimal SDK boundary doubles. Priority encoding mirrors the source checks;
 * the ready dispatch uses LWP's documented higher-number-higher convention. */
typedef uint32_t lwp_t;
typedef struct { unsigned prio, baseprio; } KThread;
typedef struct KTickTask { void (*callback)(struct KTickTask*); uint64_t ticks; } KTickTask;
typedef struct { unsigned unused; } KThrQueue;
#define KTHR_MIN_PRIO 128u
#define G9_TB_HZ 60750000u
typedef unsigned g9_lock_t;
static KThread native_thread[3];
static uint8_t ready_priority[3] = {0,64,63};
static KThread* KThreadGetSelf(void) { return &native_thread[role]; }
static uint32_t stripe_thread_id(void) { return role; }
static uint32_t stripe_now_us(void) { return model_us; }
static g9_lock_t g9_lock(void) { return 0; } /* strict token serializes .inc */
static void g9_unlock(g9_lock_t lock) { (void)lock; }
static void KTickTaskStop(KTickTask* t) { t->callback = NULL; }
static void KTickTaskStart(KTickTask* t, void (*callback)(KTickTask*), uint64_t ticks, unsigned repeat) {
	assert(!repeat); t->callback = callback; t->ticks = ticks;
}
static void KThrQueueUnblockAllByMask(KThrQueue* q, uint32_t mask) { (void)q; (void)mask; }
static int KThrQueueBlock(KThrQueue* q, uint32_t token) {
	(void)q; (void)token; assert(!"B CPU-return path excluded from this diagnosis"); return 0;
}
static void thread_msleep_native(size_t ms) {
	(void)ms;
	/* Under a tick boost the lease ACK wait is still a VOLUNTARY block: the
	 * boosted server sleeps and MAIN runs. Modelled, not hidden. */
	assert(tick_boost_mode && tb_on && dispatch_armed && seam == EXPIRED_ACK);
	dispatch_armed = false;
	dispatch_drop();
}
static void capture_lease(uint32_t e, uint32_t r, uint32_t g, uint32_t t,
	uint32_t d, bool a, bool c, uint32_t f, uint32_t p) {
	(void)e; (void)r; (void)g; (void)t; (void)d; (void)a; (void)c; (void)f; (void)p;
}
static void LWP_SetThreadPriority(lwp_t id, uint8_t priority) {
	lwp_calls++;
	assert(id == SERVER);
	ready_priority[id] = priority;
	native_thread[id].prio = native_thread[id].baseprio = KTHR_MIN_PRIO-priority;
	/* Exactly inside production lease_priority, before cleanup/checkpoint
	 * returns, including the native path while IRQs are logically masked. */
	if(dispatch_armed && role == SERVER && priority < ready_priority[MAIN]) {
		assert(main_ready && server_runnable && selected==SERVER);
		dispatch_armed = false;
		dispatch_drop();
	}
}

/* Keep the real host instance for OFF replay and avoid duplicate symbols. */
#define thread_g9_lease_test_clock native_lease_test_clock
#define thread_g9_handoff_test_timer native_handoff_test_timer
#define thread_g9_lease_configure native_lease_configure
#define thread_g9_lease_sender native_lease_sender
#define thread_g9_lease_enabled native_lease_enabled
#define thread_g9_lease_local_present native_lease_local_present
#define thread_g9_lease_active_owner native_lease_active_owner
#define thread_g9_lease_active native_lease_active
#define thread_g9_lease_pops native_lease_pops
#define thread_g9_lease_local native_lease_local
#define thread_g9_lease_audio native_lease_audio
#define thread_g9_lease_begin native_lease_begin
#define thread_g9_lease_end native_lease_end
#define thread_g9_lease_cancel native_lease_cancel
#define thread_g9_lease_dependency native_lease_dependency
#define thread_g9_lease_dependency_done native_lease_dependency_done
#define thread_g9_lease_checkpoint native_lease_checkpoint
#define thread_g9_lease_defer native_lease_defer
#define thread_g9_lease_service_begin native_lease_service_begin
#define thread_g9_lease_service_end native_lease_service_end
#define thread_g9_lease_snapshot native_lease_snapshot
#define thread_g9_lease_state native_lease_state
#define thread_g9_handoff_configure native_handoff_configure
#define thread_g9_handoff_test_priority native_handoff_test_priority
#define thread_g9_handoff_enabled native_handoff_enabled
#define thread_g9_handoff_test_sequence native_handoff_test_sequence
#define thread_g9_handoff_work_begin native_handoff_work_begin
#define thread_g9_handoff_work_end native_handoff_work_end
#define thread_g9_handoff_dispatch native_handoff_dispatch
#define thread_g9_handoff_audio_remaining native_handoff_audio_remaining
#define thread_g9_handoff_audio_abort native_handoff_audio_abort
#define thread_g9_handoff_audio_guard native_handoff_audio_guard
#define thread_g9_tick_boost_configure native_tick_boost_configure
#define thread_g9_tick_boost_enabled native_tick_boost_enabled
#define thread_g9_tick_boost_local_present native_tick_boost_local_present
#define thread_g9_tick_boost_local native_tick_boost_local
#define thread_g9_tick_boost_begin native_tick_boost_begin
#define thread_g9_tick_boost_end native_tick_boost_end
#define thread_g9_tick_boost_snapshot native_tick_boost_snapshot
void native_lease_end(void);
void native_lease_checkpoint(void);
/* Compile the unchanged native lease branch only in this test file. Engine
 * OBJECT pools and their compile profiles remain untouched. */
#define PLATFORM_WII
#include "../source/platform/g9_priority_lease.inc"
#undef PLATFORM_WII
#undef thread_g9_lease_test_clock
#undef thread_g9_handoff_test_timer
#undef thread_g9_handoff_audio_remaining
#undef thread_g9_handoff_audio_abort
#undef thread_g9_handoff_audio_guard
#undef thread_g9_lease_configure
#undef thread_g9_lease_sender
#undef thread_g9_lease_enabled
#undef thread_g9_lease_local_present
#undef thread_g9_lease_active_owner
#undef thread_g9_lease_active
#undef thread_g9_lease_pops
#undef thread_g9_lease_local
#undef thread_g9_lease_audio
#undef thread_g9_lease_begin
#undef thread_g9_lease_end
#undef thread_g9_lease_cancel
#undef thread_g9_lease_dependency
#undef thread_g9_lease_dependency_done
#undef thread_g9_lease_checkpoint
#undef thread_g9_lease_defer
#undef thread_g9_lease_service_begin
#undef thread_g9_lease_service_end
#undef thread_g9_lease_snapshot
#undef thread_g9_lease_state
#undef thread_g9_handoff_configure
#undef thread_g9_handoff_test_priority
#undef thread_g9_handoff_enabled
#undef thread_g9_handoff_test_sequence
#undef thread_g9_handoff_work_begin
#undef thread_g9_handoff_work_end
#undef thread_g9_handoff_dispatch
#undef thread_g9_tick_boost_configure
#undef thread_g9_tick_boost_enabled
#undef thread_g9_tick_boost_local
#undef thread_g9_tick_boost_begin
#undef thread_g9_tick_boost_end
#undef thread_g9_tick_boost_snapshot

#define WRAP_VOID(name, args, call) \
	void __real_thread_g9_##name args; \
	void __wrap_thread_g9_##name args { if(native) native_##name call; else __real_thread_g9_##name call; }
#define WRAP_BOOL(name) \
	bool __real_thread_g9_##name(void); \
	bool __wrap_thread_g9_##name(void) { return native ? native_##name() : __real_thread_g9_##name(); }
WRAP_VOID(lease_begin, (void), ())
WRAP_VOID(lease_end, (void), ())
WRAP_VOID(lease_checkpoint, (void), ())
WRAP_VOID(lease_pops, (uint32_t n), (n))
WRAP_BOOL(lease_active_owner)
WRAP_BOOL(lease_active)
WRAP_BOOL(lease_defer)
WRAP_BOOL(handoff_enabled)
bool __real_thread_g9_handoff_dispatch(unsigned caps, g9_handoff_service service);
bool __wrap_thread_g9_handoff_dispatch(unsigned caps, g9_handoff_service service) {
	return native ? native_handoff_dispatch(caps, service) : __real_thread_g9_handoff_dispatch(caps, service);
}

static struct server_local host;
static struct world client;
static struct server_world* active_world;
static char fixture_dir[220];
static unsigned workload;
static int work_y;
static cw_sha256_ctx server_trace, client_trace, publication_trace;
static uint64_t server_pops, server_writes, client_work;
static char server_hex[65], client_hex[65], publication_hex[65];
static void hash_words(cw_sha256_ctx* h, const int32_t* v, size_t n) { cw_sha256_update(h, v, n*sizeof *v); }
static void light_write(w_coord_t x, w_coord_t y, w_coord_t z, uint8_t light) {
	if(!measuring) return;
	int32_t row[] = {2,x,y,z,light}; hash_words(&server_trace,row,5); server_writes++;
}
static void publication(const struct client_rpc* rpc, void* unused) {
	(void)unused;
	if(measuring && rpc->type == CRPC_SET_BLOCK) {
		int32_t row[] = {rpc->type,rpc->payload.set_block.x,rpc->payload.set_block.y,
			rpc->payload.set_block.z,rpc->payload.set_block.block.type,
			rpc->payload.set_block.block.metadata,rpc->payload.set_block.block.sky_light,
			rpc->payload.set_block.block.torch_light};
		hash_words(&publication_trace,row,8);
	}
	/* Fixed synchronous test consumer; no unmeasured third thread or pool
	 * starvation. This does not certify real MAIN packet/GX/input behavior. */
	clin_drain_discard();
}
void __real_g9_capture_pop(void);
void __wrap_g9_capture_pop(void) {
	if(measuring && role == SERVER) {
		/* Kernel has just decremented index; popped storage is still present. */
		struct pop { w_coord_t x,y,z; bool forced; };
		struct pop p;
		memcpy(&p,(struct pop*)active_world->light_frontier.data+active_world->light_frontier.index,sizeof p);
		int32_t row[] = {1,p.x,p.y,p.z,p.forced}; hash_words(&server_trace,row,5);
		server_pops++; final_pop_at = wall_us();
	}
	__real_g9_capture_pop();
}
void __real_g9_capture_drain_final(uint32_t pops, uint32_t result);
void __wrap_g9_capture_drain_final(uint32_t pops, uint32_t result) {
	__real_g9_capture_drain_final(pops,result);
	if(!measuring || role != SERVER || !native || !dispatch_armed
	   || (tick_boost_mode && seam_fired)) return;
	if(tick_boost_mode) seam_fired = true; /* one seam event per replay */
	if(seam == WAKE && ready_priority[SERVER] < ready_priority[MAIN]) {
		/* MAIN became ready: strict priority preempts a lower server. */
		dispatch_armed = false;
		dispatch_drop();
	}
	if(seam == CAP) {
		assert(s_tb.boosted && s_tb_timer.callback);
		model_us = s_tb.start + s_tb.cap_us; /* the real one-shot cap timer */
		s_tb_timer.callback(&s_tb_timer);
		assert(!s_tb.boosted && s_tb.stats.capped == 1);
	}
	if(seam == EXPIRED_ACK) { model_us = 8001; native_lease_checkpoint(); }
	if(seam == DEPENDENCY || seam == CONTESTED) native_lease_dependency();
	if(seam == CONTESTED) {
		/* Prove the resource is unavailable before classifying this interval
		 * as blocked; the unlocked pair instead remains model-runnable. */
		assert(pthread_mutex_trylock(&contested.native) != 0);
		server_runnable=false;
		uint64_t start = wall_us();
		lock_attempt=1;
		thread_mutex_lock(&contested); /* actual engine mutex + wait observer */
		contested_wait_us=wall_us()-start;
		pthread_mutex_lock(&dispatch_lock); await_role(SERVER); pthread_mutex_unlock(&dispatch_lock);
		server_runnable=true;
		thread_mutex_unlock(&contested);
	}
}
void __real_g9_capture_restore_return(void);
void __wrap_g9_capture_restore_return(void) {
	if(measuring && role == SERVER && !__atomic_load_n(&restore_return_at,__ATOMIC_ACQUIRE)) {
		paired_final_pop_at=final_pop_at;
		__atomic_store_n(&restore_return_at,wall_us(),__ATOMIC_RELEASE);
	}
	__real_g9_capture_restore_return();
}

/* Observe real client relaxation reads and writes, in order, without changing
 * the kernel. Hashes cover coordinates/values, not merely pop totals. */
static __thread bool (*original_get)(void*,w_coord_t,w_coord_t,w_coord_t,struct block_data*,uint8_t*);
static __thread bool (*original_light)(void*,w_coord_t,w_coord_t,w_coord_t,uint8_t*,uint8_t*);
static __thread void (*original_set)(void*,w_coord_t,w_coord_t,w_coord_t,uint8_t);
static bool client_get(void* u,w_coord_t x,w_coord_t y,w_coord_t z,struct block_data* b,uint8_t* height) {
	bool ok=original_get(u,x,y,z,b,height);
	int32_t row[]={1,x,y,z,ok,b!=NULL,height!=NULL,ok&&b?b->type:0,
		ok&&b?b->metadata:0,ok&&b?b->sky_light:0,ok&&b?b->torch_light:0,ok&&height?*height:0};
	hash_words(&client_trace,row,12); return ok;
}
static bool client_light(void* u,w_coord_t x,w_coord_t y,w_coord_t z,uint8_t* sky,uint8_t* torch) {
	bool ok=original_light(u,x,y,z,sky,torch);
	int32_t row[]={2,x,y,z,ok,ok?*sky:0,ok?*torch:0}; hash_words(&client_trace,row,7); return ok;
}
static void client_set(void* u,w_coord_t x,w_coord_t y,w_coord_t z,uint8_t light) {
	int32_t row[]={3,x,y,z,light}; hash_words(&client_trace,row,5); original_set(u,x,y,z,light);
}
size_t __real_lighting_flood_resume_light(struct stack*,bool,size_t,
	bool (*)(void*,w_coord_t,w_coord_t,w_coord_t,struct block_data*,uint8_t*),
	bool (*)(void*,w_coord_t,w_coord_t,w_coord_t,uint8_t*,uint8_t*),
	void (*)(void*,w_coord_t,w_coord_t,w_coord_t,uint8_t),void*);
size_t __wrap_lighting_flood_resume_light(struct stack* q,bool sky,size_t budget,
	bool (*get)(void*,w_coord_t,w_coord_t,w_coord_t,struct block_data*,uint8_t*),
	bool (*light)(void*,w_coord_t,w_coord_t,w_coord_t,uint8_t*,uint8_t*),
	void (*set)(void*,w_coord_t,w_coord_t,w_coord_t,uint8_t),void* u) {
	if(!measuring || role != MAIN) return __real_lighting_flood_resume_light(q,sky,budget,get,light,set,u);
	original_get=get; original_light=light; original_set=set;
	return __real_lighting_flood_resume_light(q,sky,budget,client_get,client_light,client_set,u);
}

static void run_client(bool finish) {
	account=&main_account;
	uint64_t start=wall_us(), cpu=cpu_us(), cycles=cpu_cycles();
	unsigned frames=0;
	do {
		assert(++frames < 512);
		world_update_lighting(&client);
		uint64_t did=world_lighting_driver_last_work(); client_work+=did;
		if(!__atomic_load_n(&restore_return_at,__ATOMIC_ACQUIRE)) main_work_before_restore+=did;
		if(!__atomic_load_n(&tick_complete_at,__ATOMIC_ACQUIRE)) main_work_before_tick+=did;
	} while(finish && (!stack_empty(&client.lighting_updates) || !lighting_flood_queue_empty(&client.lighting_flood_queue)));
	main_account.elapsed+=wall_us()-start; main_account.cpu+=cpu_us()-cpu;
	main_account.cycles+=cpu_cycles()-cycles;
	account=NULL;
}
static bool sender(uint8_t priority) { assert(priority == 65 || priority == 66); return true; }
static void open_world(void);
static void close_world(void);
static void* server_entry(void* unused) {
	(void)unused; role=SERVER;
	pthread_mutex_lock(&dispatch_lock); await_role(SERVER); pthread_mutex_unlock(&dispatch_lock);
	open_world(); active_world=&host.worlds[0];
	pthread_mutex_lock(&dispatch_lock); setup_stage=1; select_role(MAIN); await_role(SERVER); pthread_mutex_unlock(&dispatch_lock);
	g9_drain_edits(active_world,work_y,workload==0?20:40);
	if(workload!=0) for(int k=0;k<16;k++) {
		bool on=((k+2)&1)!=0;
		g9_drain_set(active_world,17+2*(k%4),work_y,17+2*(k/4),on?BLOCK_TORCH:BLOCK_AIR,on?5:0);
	}
	pthread_mutex_lock(&dispatch_lock); setup_stage=2; select_role(MAIN); await_role(SERVER); pthread_mutex_unlock(&dispatch_lock);
	if(native) { native_lease_local(true,63); native_tick_boost_local(true,63); }
	account=&srv_account;
	uint64_t start=wall_us(), cpu=cpu_us(), cycles=cpu_cycles();
	active_world=&host.worlds[0];
	/* Production placement (server_local_thread): begin before ALL measured
	 * tick work (here including the workload-2 checkpoint trial drain), end
	 * after update + epilogue. Dormant unless the knob is ON. */
	if(native) native_tick_boost_begin();
	if(workload==2) assert(server_world_checkpoint_chunks(active_world));
	server_local_test_update_once(&host); /* real full update, including random pass */
	assert(!server_local_light_stopped(&host));
	if(seam==DEPENDENCY || seam==CONTESTED) native_lease_dependency_done();
	__atomic_store_n(&tick_complete_at,wall_us(),__ATOMIC_RELEASE);
	if(native) native_tick_boost_end(); /* the tick real end */
	srv_account.elapsed=wall_us()-start; srv_account.cpu=cpu_us()-cpu;
	srv_account.cycles=cpu_cycles()-cycles;
	account=NULL;
	if(native) { native_lease_end(); native_tick_boost_local(false,63); native_lease_local(false,63); }
	server_runnable=false;
	pthread_mutex_lock(&dispatch_lock); completed=1; select_role(MAIN); await_role(SERVER); pthread_mutex_unlock(&dispatch_lock);
	close_world();
	return NULL;
}

/* Saved input is a private disposable fixture. Restore ALL saved region bytes
 * before each replay so the real checkpoint cannot contaminate its control. */
struct saved_file { char path[540]; void* bytes; size_t n; };
static struct saved_file saved[32];
static unsigned saved_n;
static void save_input_files(void) {
	char dir[260]; snprintf(dir,sizeof dir,"%s/region",fixture_dir);
	DIR* d=opendir(dir); assert(d); struct dirent* e;
	while((e=readdir(d))) {
		if(e->d_name[0]=='.') continue;
		assert(saved_n<32); struct saved_file* f=&saved[saved_n++];
		snprintf(f->path,sizeof f->path,"%s/%s",dir,e->d_name);
		FILE* in=fopen(f->path,"rb"); assert(in); assert(!fseek(in,0,SEEK_END));
		long n=ftell(in); assert(n>0); f->n=(size_t)n; rewind(in);
		f->bytes=malloc(f->n); assert(f->bytes); assert(fread(f->bytes,1,f->n,in)==f->n); assert(!fclose(in));
		cw_sha256_ctx h; char hex[65]; cw_sha256_init(&h); cw_sha256_update(&h,f->bytes,f->n); cw_sha256_final_hex(&h,hex);
		printf("SAVED_INPUT name=%s bytes=%zu sha256=%s\n",e->d_name,f->n,hex);
		const char* evidence=getenv("CW_G9_DIAG_EVIDENCE");
		if(evidence) {
			_mkdir(evidence); char path[1024]; int used=snprintf(path,sizeof path,"%s/%s",evidence,e->d_name); assert(used>0 && (size_t)used<sizeof path);
			FILE* out=fopen(path,"wb"); assert(out); assert(fwrite(f->bytes,1,f->n,out)==f->n); assert(!fclose(out));
		}
	}
	closedir(d); assert(saved_n);
}
static void restore_input_files(void) {
	for(unsigned i=0;i<saved_n;i++) {
		FILE* out=fopen(saved[i].path,"wb"); assert(out);
		assert(fwrite(saved[i].bytes,1,saved[i].n,out)==saved[i].n); assert(!fclose(out));
	}
}
static void open_world(void) {
	memset(&host,0,sizeof host);
	dict_entity_init(host.entities); if(!server_local_te_tables_init(&host)) abort(); /* IMP-TE-CAPS */
	clin_server_publication_stop(false);
	string_t path; string_init_set_str(path,fixture_dir);
	server_world_create(&host.worlds[0],path,WORLD_DIM_OVERWORLD,4242,true); string_clear(path);
	struct server_world* w=&host.worlds[0]; w->session=&host; w->local=&host;
	host.scheduled_tick_budget=128; host.rand_src.seed=0x9e11; host.world_time=6000;
	host.container_drop_rng.seed=17; host.dispenser_break_rng.seed=31; host.entity_rng.seed=47;
	server_local_skylight_recompute(&host);
	/* Full resident/sent PC near ring: no time-budgeted generation can make
	 * the full update perform different work across dispatch choices. */
	for(int x=-MAX_VIEW_DISTANCE;x<=MAX_VIEW_DISTANCE;x++) for(int z=-MAX_VIEW_DISTANCE;z<=MAX_VIEW_DISTANCE;z++) {
		struct server_chunk* c=NULL; assert(server_world_load_chunk(w,x,z,&c) && c); c->sent=true; c->populated=true;
	}
	assert(inventory_create(&host.player.inventory,&inventory_logic_player,&host,&host.player,INVENTORY_SIZE,0,0,0));
	host.player.active_inventory=&host.player.inventory;
	host.player.has_pos=host.player.finished_loading=true;
	host.player.x=8.5; host.player.y=65+PLAYER_EYE_HEIGHT; host.player.z=8.5;
	host.player.health=MAX_PLAYER_HEALTH; host.render_demo_mob=-1;
	host.far_view_distance=MAX_VIEW_DISTANCE;
}
static void close_world(void) {
	inventory_destroy(&host.player.inventory,&host.player);
	server_world_discard(&host.worlds[0]); dict_entity_clear(host.entities);
}
static void prepare_client(void) {
	world_create(&client); client.dimension=WORLD_DIM_OVERWORLD;
	for(int x=-3;x<=3;x++) for(int z=-3;z<=3;z++) {
		struct server_chunk* c=dict_server_chunks_get(host.worlds[0].chunks,S_CHUNK_ID(x,z)); assert(c);
		uint8_t tint[256]={0};
		world_build_full_section(&client,x,z,c->ids,c->metadata,c->lighting_sky,c->lighting_torch,c->heightmap,tint,tint);
	}
	assert(!client.settled_light);
	/* Same immutable saved section planes, then the same changed roof/torch
	 * input as the server. Client work is finished, never padded with a burn. */
	int t=workload==0?20:40;
	if(t==40) for(int dx=4;dx<12;dx++) for(int dz=4;dz<12;dz++)
		world_set_block(&client,-16+dx,work_y+4,16+dz,(struct block_data){.type=BLOCK_STONE},true);
	for(int k=0;k<16;k++) {
		bool on=((k+t/20)&1)!=0;
		world_set_block(&client,-15+2*(k%4),work_y,17+2*(k/4),
			(struct block_data){.type=on?BLOCK_TORCH:BLOCK_AIR,.metadata=on?5:0},true);
	}
	if(workload!=0) for(int k=0;k<16;k++) {
		bool on=((k+2)&1)!=0;
		world_set_block(&client,17+2*(k%4),work_y,17+2*(k/4),
			(struct block_data){.type=on?BLOCK_TORCH:BLOCK_AIR,.metadata=on?5:0},true);
	}
}

struct result {
	char input[65],server[65],client[65],publication[65],field[65];
	uint64_t pops,writes,work,final_to_restore,restore_to_tick,gap;
	uint64_t before_restore,before_tick;
	struct account srv,main;
	struct account gap_main;
	uint32_t rng,tick;
};
static void field_hash(char hex[65]) {
	cw_sha256_ctx h; cw_sha256_init(&h);
	for(int x=-MAX_VIEW_DISTANCE;x<=MAX_VIEW_DISTANCE;x++) for(int z=-MAX_VIEW_DISTANCE;z<=MAX_VIEW_DISTANCE;z++) {
		struct server_chunk* c=dict_server_chunks_get(host.worlds[0].chunks,S_CHUNK_ID(x,z)); assert(c);
		cw_sha256_update(&h,c->ids,32768); cw_sha256_update(&h,c->metadata,16384);
		cw_sha256_update(&h,c->lighting_sky,16384); cw_sha256_update(&h,c->lighting_torch,16384);
		cw_sha256_update(&h,c->heightmap,256);
	}
	cw_sha256_final_hex(&h,hex);
}
static void finish_hash(cw_sha256_ctx* ctx,char hex[65]) {
	cw_sha256_final_hex(ctx,hex);
}
static struct result replay(enum seam mode,bool parked) {
	seam=mode; control=parked; native=mode!=OFF; measuring=false; dispatch_armed=false; seam_fired=false;
	bool lease_on = native && mode != WAKE && mode != CAP;
	thread_g9_lease_configure(false,8000,500); thread_g9_handoff_configure(false); column_light_configure(false);
	model_us=1; role=MAIN;
	for(unsigned id=MAIN;id<=SERVER;id++) native_thread[id].prio=native_thread[id].baseprio=KTHR_MIN_PRIO-ready_priority[id];
	ready_priority[MAIN]=64; ready_priority[SERVER]=63;
	struct config cfg = {.root = json_value_init_object()}; assert(cfg.root);
	assert(config_set_int(&cfg,"experimental.g9_priority_lease_int",lease_on));
	assert(config_set_int(&cfg,"experimental.g9_main_handoff_int",admission));
	assert(config_set_int(&cfg,"experimental.g9_tick_boost_int",tb_on));
	struct config_g9_lease knobs = config_read_g9_lease(&cfg);
	assert(knobs.enabled == lease_on && knobs.main_handoff == admission);
	struct config_g9_tick_boost tbk = config_read_g9_tick_boost(&cfg);
	assert(tbk.enabled == tb_on && tbk.cap_ms == 60);
	native_lease_configure(knobs.enabled,knobs.max_us,knobs.service_us);
	native_handoff_configure(knobs.main_handoff);
	native_tick_boost_configure(tbk.enabled,tbk.cap_ms);
	config_destroy(&cfg);
	native_lease_sender(sender); native_lease_audio(UINT32_MAX,false);
	restore_input_files();
	memset(&srv_account,0,sizeof srv_account); memset(&main_account,0,sizeof main_account);
	memset(&main_gap_account,0,sizeof main_gap_account);
	request=completed=lock_attempt=setup_stage=0; selected=MAIN;
	contested_wait_us=0;
	if(mode==CONTESTED) { assert(thread_mutex_init(&contested)); thread_mutex_lock(&contested); }
	transition_at=final_pop_at=paired_final_pop_at=restore_return_at=tick_complete_at=0;
	server_pops=server_writes=client_work=main_work_before_restore=main_work_before_tick=0;
	cw_sha256_init(&server_trace); cw_sha256_init(&client_trace); cw_sha256_init(&publication_trace);
	struct thread worker; assert(thread_create(&worker,server_entry,NULL,63));
	pthread_mutex_lock(&dispatch_lock); select_role(SERVER); await_role(MAIN); assert(setup_stage==1); pthread_mutex_unlock(&dispatch_lock);
	char input_hex[65]; field_hash(input_hex);
	prepare_client();
	pthread_mutex_lock(&dispatch_lock); select_role(SERVER); await_role(MAIN); assert(setup_stage==2); pthread_mutex_unlock(&dispatch_lock);
	measuring=true; dispatch_armed=native; main_ready=server_runnable=true;
	pthread_mutex_lock(&dispatch_lock); select_role(SERVER); pthread_mutex_unlock(&dispatch_lock);
	if(mode==OFF) {
		if(!parked) run_client(false);
		pthread_mutex_lock(&dispatch_lock); while(!completed) pthread_cond_wait(&dispatch_wake,&dispatch_lock); pthread_mutex_unlock(&dispatch_lock);
		run_client(true);
	} else {
		bool pi_flow = tb_on && mode == CONTESTED;
		pthread_mutex_lock(&dispatch_lock); await_role(MAIN);
		if(pi_flow) {
			/* Boosted server blocked on a MAIN-held mutex: KMutex priority
			 * inheritance runs MAIN only to the end of its critical section;
			 * the unlock makes the 65 server runnable and it preempts. */
			assert(request==0 && lock_attempt==2 && !completed);
			pthread_mutex_unlock(&dispatch_lock);
			thread_mutex_unlock(&contested);
			pthread_mutex_lock(&dispatch_lock); select_role(SERVER); await_role(MAIN);
		}
		assert(request==1 && !completed); pthread_mutex_unlock(&dispatch_lock);
		unsigned expect_server = tb_on && mode == EXPIRED_ACK ? 65u : 63u;
		assert(ready_priority[SERVER]==expect_server && ready_priority[MAIN]==64);
		if(mode==EXPIRED_ACK) {
			assert(native_lease_defer()); model_us=12002;
			assert(!native_lease_service_begin().generation); assert(s_lease.failed && s_lease.stats.stalls==1);
		}
		assert(!native_lease_defer());
		if(admission) {
			/* Give the real production poll its strongest existing safe MAIN
			 * capability (post-flip RPC + mesh). No manufactured ACK or work
			 * witnesses. Terminal/expired/parked paths must return CPU here. */
			clin_g9_handoff_poll(true);
			printf("R25_NATIVE seam=%u active=%u failed=%u parked=%u services=%u block_us=%u caps=%u/%u/%u\n",
				mode,s_lease.active,s_lease.failed,s_lease.parked,s_lease.stats.services,
				s_lease.stats.main_sleep_requested_us,s_lease.max_us,s_lease.wait_us,s_lease.service_us);
		}
		if(mode==CONTESTED && !pi_flow) {
			pthread_mutex_lock(&dispatch_lock); select_role(SERVER); await_role(MAIN);
			assert(lock_attempt==2); pthread_mutex_unlock(&dispatch_lock);
		}
		if(!parked) run_client(false);
		main_gap_account=main_account;
		if(mode==CONTESTED && !pi_flow) thread_mutex_unlock(&contested);
		main_ready=false; /* control parks before work; RED at real frame end */
		pthread_mutex_lock(&dispatch_lock); select_role(SERVER);
		while(!completed) pthread_cond_wait(&dispatch_wake,&dispatch_lock);
		pthread_mutex_unlock(&dispatch_lock);
		run_client(true);
	}
	measuring=false;
	finish_hash(&server_trace,server_hex); finish_hash(&client_trace,client_hex); finish_hash(&publication_trace,publication_hex);
	struct result r={.pops=server_pops,.writes=server_writes,.work=client_work,.srv=srv_account,.main=main_account,
		.gap_main=main_gap_account,
		.final_to_restore=restore_return_at-paired_final_pop_at,.restore_to_tick=tick_complete_at-restore_return_at,
		.gap=transition_at?restore_return_at-transition_at:0,.rng=host.rand_src.seed,.tick=host.worlds[0].current_tick,
		.before_restore=main_work_before_restore,.before_tick=main_work_before_tick};
	memcpy(r.input,input_hex,sizeof r.input);
	memcpy(r.server,server_hex,sizeof r.server); memcpy(r.client,client_hex,sizeof r.client);
	memcpy(r.publication,publication_hex,sizeof r.publication); field_hash(r.field);
	printf("ROW workload=%u seam=%u control=%u pops=%"PRIu64" writes=%"PRIu64" client=%"PRIu64
		" server_elapsed=%"PRIu64" server_cpu=%"PRIu64" server_sync=%"PRIu64" runnable_dispatch=%"PRIu64
		" main_elapsed=%"PRIu64" main_cpu=%"PRIu64" main_sync=%"PRIu64
		" final_restore=%"PRIu64" restore_tick=%"PRIu64" transition_restore=%"PRIu64
		" main_before_restore=%"PRIu64" main_before_tick=%"PRIu64" rng=%u tick=%u\n",
		workload,mode,parked,r.pops,r.writes,r.work,r.srv.elapsed,r.srv.cpu,r.srv.sync,r.srv.dispatch,
		r.main.elapsed,r.main.cpu,r.main.sync,r.final_to_restore,r.restore_to_tick,r.gap,
		main_work_before_restore,main_work_before_tick,r.rng,r.tick);
	printf("HASH input=%s server=%s client=%s publication=%s field=%s\n",r.input,r.server,r.client,r.publication,r.field);
	printf("CPU server_cycles=%"PRIu64" main_cycles=%"PRIu64" server_us_est=%.3f main_us_est=%.3f\n",
		r.srv.cycles,r.main.cycles,(double)r.srv.cycles/cycles_per_us,(double)r.main.cycles/cycles_per_us);
	printf("MAIN_GAP elapsed=%"PRIu64" cycles=%"PRIu64" cpu_us_est=%.3f sync=%"PRIu64"\n",
		r.gap_main.elapsed,r.gap_main.cycles,(double)r.gap_main.cycles/cycles_per_us,r.gap_main.sync);
	if(mode==CONTESTED) { printf("LOCK unavailable_to_acquired=%"PRIu64" observer_wait=%"PRIu64"\n",contested_wait_us,r.srv.sync); thread_mutex_destroy(&contested); }
	world_destroy(&client);
	pthread_mutex_lock(&dispatch_lock); select_role(SERVER); pthread_mutex_unlock(&dispatch_lock);
	thread_join(&worker); clin_drain_discard();
	return r;
}

/* No-owner / remote / knob-OFF never boost, and the strict config parse. Runs
 * the real native tick-boost entry points with the SDK doubles above. */
static unsigned tick_boost_unit_checks(void) {
	unsigned bad = 0;
	struct config cfg = {.root = json_value_init_object()}; assert(cfg.root);
	assert(config_set_int(&cfg,"experimental.g9_tick_boost_int",2));
	assert(config_set_int(&cfg,"experimental.g9_tick_boost_cap_ms_int",500));
	struct config_g9_tick_boost k = config_read_g9_tick_boost(&cfg);
	bad += k.enabled || k.cap_ms != 60; /* non-0|1 is OFF; out-of-range cap = default */
	assert(config_set_int(&cfg,"experimental.g9_tick_boost_int",1));
	assert(config_set_int(&cfg,"experimental.g9_tick_boost_cap_ms_int",40));
	k = config_read_g9_tick_boost(&cfg);
	bad += !k.enabled || k.cap_ms != 40;
	config_destroy(&cfg);
	struct config empty = {.root = json_value_init_object()}; assert(empty.root);
	k = config_read_g9_tick_boost(&empty); bad += k.enabled || k.cap_ms != 60; /* default OFF */
	config_destroy(&empty);
	bad += thread_g9_tick_boost_enabled(); /* the real engine instance defaults OFF */
	role = MAIN; native_lease_configure(false,8000,500); native_lease_sender(sender);
	struct g9_tick_boost_stats st; bool boosted;
	/* 1. knob OFF, owner registered: nothing at all. */
	native_tick_boost_configure(false,60); role = SERVER; native_tick_boost_local(true,63);
	unsigned calls = lwp_calls; native_tick_boost_begin(); native_tick_boost_end();
	native_tick_boost_snapshot(&st,&boosted); native_tick_boost_local(false,63);
	bad += lwp_calls != calls || st.ticks || st.rejected || boosted;
	/* 2. knob ON, no local owner (remote Beta MP never registers): rejected. */
	native_tick_boost_configure(true,60); role = SERVER;
	calls = lwp_calls; native_tick_boost_begin();
	native_tick_boost_snapshot(&st,&boosted);
	bad += lwp_calls != calls || boosted || st.ticks || st.rejected != 1;
	native_tick_boost_end(); bad += lwp_calls != calls;
	/* 3. knob ON, owner registered, MAIN (wrong thread) calls begin: rejected. */
	native_tick_boost_local(true,63); role = MAIN;
	native_tick_boost_begin(); native_tick_boost_end();
	native_tick_boost_snapshot(&st,&boosted);
	bad += lwp_calls != calls || boosted || st.ticks || st.rejected != 2;
	/* 4. owner: boost to 65, restore to base 63 at end; nested begin rejected. */
	role = SERVER; model_us = 100;
	native_tick_boost_begin(); bad += !s_tb.boosted || ready_priority[SERVER] != 65;
	native_tick_boost_begin(); /* nested */
	model_us = 30100; native_tick_boost_end();
	native_tick_boost_snapshot(&st,&boosted);
	bad += boosted || ready_priority[SERVER] != 63 || st.ticks != 1 || st.rejected != 3
		|| st.last_us != 30000 || st.capped;
	/* 5. cap timer: drops to base mid-tick; tick end is then a no-op. */
	native_tick_boost_begin(); assert(s_tb_timer.callback);
	model_us += 60000; s_tb_timer.callback(&s_tb_timer);
	bad += s_tb.boosted || ready_priority[SERVER] != 63;
	calls = lwp_calls; native_tick_boost_end(); bad += lwp_calls != calls;
	native_tick_boost_snapshot(&st,&boosted); bad += st.capped != 1;
	native_tick_boost_local(false,63); native_tick_boost_configure(false,60);
	role = MAIN;
	printf("TICK_BOOST_UNIT no_owner_remote_off_wrongthread_nested_cap=%s\n", bad ? "RED" : "GREEN");
	return bad;
}

int main(int argc, char** argv) {
	assert(argc == 1 || (argc == 2 && (!strcmp(argv[1],"--r25-admission")
		|| !strcmp(argv[1],"--tick-boost"))));
	tick_boost_mode = argc == 2 && !strcmp(argv[1],"--tick-boost");
	admission = argc == 2 && !tick_boost_mode;
	setvbuf(stdout,NULL,_IONBF,0);
	DWORD_PTR process,system; assert(GetProcessAffinityMask(GetCurrentProcess(),&process,&system));
	DWORD_PTR one=process & (~process+1); assert(one && SetProcessAffinityMask(GetCurrentProcess(),one));
	printf("HOST ONLY one_cpu_mask=%"PRIu64" CPU=GetThreadTimes (coarse OS accounting), elapsed=monotonic\n",(uint64_t)one);
	calibrate_cycles();
	blocks_init(); items_init(); assert(clin_init() && svin_init());
	(void)lease_lock_held; /* Included production helper belongs to native locks. */
	clin_test_set_rpc_observer(publication,NULL);
	server_world_test_light_write_hook=light_write;
	thread_tps_observe=wait_observe;
	test_fixture_dir("test_g9_starvation_diag",fixture_dir,sizeof fixture_dir); _mkdir(fixture_dir);
	char region[260]; snprintf(region,sizeof region,"%s/region",fixture_dir); _mkdir(region);
	open_world(); uint8_t height; assert(server_world_get_height(&host.worlds[0],8,8,&height));
	work_y=height+12; g9_drain_floor(&host.worlds[0],work_y); assert(server_world_light_drain(&host.worlds[0]));
	for(int x=16;x<32;x++) for(int z=16;z<32;z++) g9_drain_set(&host.worlds[0],x,work_y-1,z,BLOCK_STONE,0);
	assert(server_world_light_drain(&host.worlds[0]));
	assert(server_world_checkpoint_chunks(&host.worlds[0])); close_world(); save_input_files();
	unsigned failures=0, admission_failures=0;
	if(tick_boost_mode) {
		unsigned tb_failures = tick_boost_unit_checks();
		for(workload=0;workload<3;workload++) for(enum seam mode=TERMINAL;mode<=CAP;mode++) {
			struct result off_red = {0};
			for(unsigned knob = mode==CAP ? 1 : 0; knob<=1; knob++) {
				tb_on = knob;
				struct result red=replay(mode,false), ctl=replay(mode,true);
				bool same=!strcmp(red.input,ctl.input) && !strcmp(red.server,ctl.server) && !strcmp(red.client,ctl.client)
					&& !strcmp(red.publication,ctl.publication) && !strcmp(red.field,ctl.field)
					&& red.pops==ctl.pops && red.writes==ctl.writes && red.work==ctl.work && red.rng==ctl.rng && red.tick==ctl.tick;
				if(knob && mode!=CAP) same = same && !strcmp(red.input,off_red.input) && !strcmp(red.server,off_red.server)
					&& !strcmp(red.client,off_red.client) && !strcmp(red.publication,off_red.publication)
					&& !strcmp(red.field,off_red.field) && red.pops==off_red.pops && red.writes==off_red.writes
					&& red.work==off_red.work && red.rng==off_red.rng && red.tick==off_red.tick;
				if(!knob) off_red = red;
				/* The predicate: zero ordinary MAIN work before FULL tick completion. */
				bool zero=red.before_tick==0, ctl_zero=ctl.before_tick==0;
				const char* kind = !knob ? "OFF_EXPECT_RED" : mode==CAP ? "CAP_EXPECT_MAIN_RUNS"
					: mode==EXPIRED_ACK ? "VOLUNTARY_ACK_WAIT_EXPECT_MAIN_RUNS" : "CAUSAL_EXPECT_GREEN";
				bool ok = same && ctl_zero && (knob && mode!=CAP && mode!=EXPIRED_ACK ? zero : !zero);
				printf("TICK_BOOST workload=%u seam=%u knob=%u kind=%s ordered_equivalent_on_off=%u main_before_tick=%"PRIu64"/%"PRIu64
					" server_elapsed=%"PRIu64" result=%s\n", workload, mode, knob, kind, same,
					red.before_tick, ctl.before_tick, red.srv.elapsed, ok ? "PASS" : "FAIL");
				if(!ok) tb_failures++;
			}
		}
		tb_on = false;
		thread_tps_observe=NULL; server_world_test_light_write_hook=NULL; clin_test_set_rpc_observer(NULL,NULL);
		for(unsigned i=0;i<saved_n;i++) free(saved[i].bytes);
		test_fixture_dir_cleanup(fixture_dir);
		printf("TICK_BOOST failures=%u (HOST ONLY strict-priority model)\n",tb_failures);
		return tb_failures ? 1 : 0;
	}
	for(workload=0;workload<3;workload++) for(enum seam mode=OFF;mode<=CONTESTED;mode++) {
		struct result red=replay(mode,false), ctl=replay(mode,true);
		bool same=!strcmp(red.input,ctl.input) && !strcmp(red.server,ctl.server) && !strcmp(red.client,ctl.client)
			&& !strcmp(red.publication,ctl.publication) && !strcmp(red.field,ctl.field)
			&& red.pops==ctl.pops && red.writes==ctl.writes && red.work==ctl.work && red.rng==ctl.rng && red.tick==ctl.tick;
		printf("PAIR workload=%u seam=%u ordered_equivalent=%u RED_dispatch=%"PRIu64" CONTROL_dispatch=%"PRIu64"\n",
			workload,mode,same,red.srv.dispatch,ctl.srv.dispatch);
		if(!same) failures++;
		if(mode!=OFF) {
			/* The SAME CPU-return predicate is RED under ordinary MAIN work,
			 * GREEN when MAIN parks before it. No production repair is applied. */
			bool red_return=red.before_restore==0, control_return=ctl.before_restore==0;
			printf("RETURN_ASSERT ordinary=%s parked=%s main_work=%"PRIu64"/%"PRIu64"\n",
				red_return?"GREEN":"RED",control_return?"GREEN":"RED",red.before_restore,ctl.before_restore);
			if(admission) {
				/* This assertion is deliberately RED on 98f30ed81's native
				 * path; a scoped repair must make it GREEN without replacing
				 * the ordered workload or the parked diagnostic control. */
				bool admitted = red_return && red.before_tick == 0 && control_return;
				printf("R25_ADMISSION zero_ordinary_work_before_restore_and_tick=%s\n",admitted?"GREEN":"RED");
				if(!admitted) admission_failures++;
			} else assert(!red_return && control_return);
		}
	}
	thread_tps_observe=NULL; server_world_test_light_write_hook=NULL; clin_test_set_rpc_observer(NULL,NULL);
	for(unsigned i=0;i<saved_n;i++) free(saved[i].bytes);
	test_fixture_dir_cleanup(fixture_dir);
	printf("DIAGNOSTIC equivalence_failures=%u (model evidence is HOST ONLY)\n",failures);
	if(admission) printf("R25_ADMISSION failures=%u (no native repair admitted)\n",admission_failures);
	return failures || admission_failures ? 1 : 0;
}
