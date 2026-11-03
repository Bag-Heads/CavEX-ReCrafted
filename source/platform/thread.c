/*
	Copyright (c) 2023 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../tps_observer.h"
tps_callback thread_tps_observe;

#include <assert.h>

#include "thread.h"
#include "time.h"
#include "g9_probe.h"
#include "g9_capture.h"
#include "g9_worst_tick.h"
#include <stdlib.h>
#ifdef PLATFORM_WII
#include "mem2.h"
#endif
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h> /* struct timespec for thread_cond_timedwait (both arms) */
#ifdef PLATFORM_WII
#include <ogc/lwp_watchdog.h>
#include <tuxedo/thread.h>

/* The installed logc256kr LWP shim encodes lwp_t as ~(u32)KThread*. Guard
 * every private offset used by this temporary diagnostic at compile time. */
_Static_assert(sizeof(void*) == 4 && sizeof(PPCContext) == 0x2c0,
			   "saved-context probe requires the linked PPC32 Tuxedo ABI");
_Static_assert(offsetof(KThread, ctx) == 0
			   && offsetof(PPCContext, pc) == 0
			   && offsetof(PPCContext, lr) == 0x0c
			   && offsetof(PPCContext, gpr[1]) == 0x1c
			   && offsetof(KThread, state) == 0x2c4
			   && offsetof(KThread, suspend) == 0x2c6
			   && offsetof(KThread, prio) == 0x2c8
			   && offsetof(KThread, wait.queue) == 0x2dc
			   && offsetof(KThread, wait.token) == 0x2e0,
			   "saved-context probe KThread offsets differ from linked SDK");
_Static_assert(KTHR_STATE_RUNNING == 2 && KTHR_STATE_WAITING == 3
			   && KTHR_STATE_WAITING_MUTEX == 4,
			   "saved-context probe wait-state values differ from linked SDK");
#endif

/* Only 32-bit atomic fields: the Wii has no 64-bit libatomic runtime. The
 * main, server, and mesher roles each have one thread and one transition slot,
 * so an acquiring waiter cannot erase a preempted holder. */
struct stripe_record {
	uint32_t seq, id, state;
};
static struct {
	struct thread_mutex* mutex;
	const char* name;
	struct stripe_record role[4];
} s_stripes[8];
static uint32_t s_role_id[4], s_role_phase[4];
/* The main thread's in-progress wait: one seqlock record (single writer, the
 * main thread), so a reader never pairs one wait's stripe with another wait's
 * start time. */
static uint32_t s_wait_seq, s_wait_stripe_plus_one, s_wait_start_us;
static uint32_t s_completed_seq;
static struct thread_stripe_breadcrumb s_completed;
#ifdef PLATFORM_PC
static void (*s_stripe_test_hook)(unsigned event, void*);
static void* s_stripe_test_user;
void thread_stripe_test_hook_set(void (*hook)(unsigned event, void*), void* user) {
	s_stripe_test_hook = hook;
	s_stripe_test_user = user;
}
static void stripe_test_event(unsigned event) {
	if(s_stripe_test_hook) s_stripe_test_hook(event, s_stripe_test_user);
}
#else
#define stripe_test_event(event) ((void)0)
#endif

static uint32_t stripe_now_us(void) {
#ifdef PLATFORM_WII
	return (uint32_t)ticks_to_microsecs(gettime());
#else
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)((uint64_t)t.tv_sec * 1000000u + (uint32_t)t.tv_nsec / 1000u);
#endif
}

static uint32_t stripe_thread_id(void) {
#ifdef PLATFORM_WII
	uint32_t id = (uint32_t)LWP_GetSelf();
#else
	/* pthread_t is opaque; this stable diagnostic token needs no OS query. */
	uintptr_t native = (uintptr_t)pthread_self();
	uint32_t id = (uint32_t)(native ^ (native >> 32));
#endif
	return id ? id : UINT32_MAX;
}

/* R7: one bounded multi-producer ring. IRQ exclusion serializes thread and
 * timer producers on Broadway; a mutex supplies the same contract to PC
 * injection tests. The consumer formats copied records outside this lock. */
static struct {
	uint32_t rows[G9_PROBE_CAPACITY][G9_PROBE_WORDS];
	uint32_t head, tail, seq, lost, span, active, owner, enabled, sampler;
	uint32_t samples, late, missed, rpc_id, rpc_active, rpc_owner;
	uint32_t role_id[6], mesh_state[8], mesh_start[8], submissions, stales;
	uint32_t completed, retired;
	uint64_t deadline;
} s_g9;
static struct thread_channel* s_g9_rpc_pool;
static struct {
	uint32_t enabled, frame, phase, frame_active;
	uint64_t inbox[16];
	struct { const void* world; uint64_t seeds[8192], active; } light[2];
} s_g9_main;
#ifdef PLATFORM_WII
typedef PPCIrqState g9_lock_t;
static KTickTask s_g9_timer;
_Static_assert(offsetof(KThread, baseprio) == 0x2ca,
	"G9 sampler base priority ABI differs from linked SDK");
static g9_lock_t g9_lock(void) { return PPCIrqLockByMsr(); }
static void g9_unlock(g9_lock_t s) { PPCIrqUnlockByMsr(s); }
static uint64_t g9_tb(void) { return PPCGetTickCount(); }
#define G9_TB_HZ 60750000u
#define G9_PERIOD 60750u
_Static_assert(PPC_TIMER_CLOCK == G9_TB_HZ, "G9 timebase clock differs from SDK");
#else
typedef unsigned g9_lock_t;
static pthread_mutex_t s_g9_lock = PTHREAD_MUTEX_INITIALIZER;
static g9_lock_t g9_lock(void) { pthread_mutex_lock(&s_g9_lock); return 0; }
static void g9_unlock(g9_lock_t s) { (void)s; pthread_mutex_unlock(&s_g9_lock); }
static uint64_t g9_tb(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000u + (uint32_t)t.tv_nsec / 1000u;
}
#define G9_TB_HZ 1000000u
#define G9_PERIOD 1000u
#endif
static void thread_msleep_native(size_t ms);
#ifdef PLATFORM_WII
static void g9_timer_sample(KTickTask* timer);
#endif
#include "g9_capture.inc"
/* REL-10: the lease's dependency and the tick boost's owner/begin/end entry
 * points are compiled here under private names; the public names (thread.h)
 * are defined below the MAIN pacing state, where they ALSO publish the
 * whole-tick pacing owner/active/wait state. That state must not depend on
 * whether the lease or the boost is enabled (both return early when OFF, the
 * shipped setting). The included bodies are unchanged. */
#define thread_g9_lease_dependency g9_lease_dependency_impl
#define thread_g9_lease_dependency_done g9_lease_dependency_done_impl
#define thread_g9_tick_boost_local g9_tick_boost_local_impl
#define thread_g9_tick_boost_begin g9_tick_boost_begin_impl
#define thread_g9_tick_boost_end g9_tick_boost_end_impl
static void g9_lease_dependency_impl(void);
static void g9_lease_dependency_done_impl(void);
static void g9_tick_boost_local_impl(bool present, uint8_t base_priority);
static void g9_tick_boost_begin_impl(void);
static void g9_tick_boost_end_impl(void);
#include "g9_priority_lease.inc"
#undef thread_g9_lease_dependency
#undef thread_g9_lease_dependency_done
#undef thread_g9_tick_boost_local
#undef thread_g9_tick_boost_begin
#undef thread_g9_tick_boost_end
#include "g9_worst_tick.inc"

bool g9_probe_enabled(void) {
	return __atomic_load_n(&s_g9.enabled, __ATOMIC_RELAXED) != 0
		&& !wt_cpu_mode(); /* aggregate mode produces no raw g9_cpu batches */
}
bool g9_probe_main_enabled(void) {
	return g9_probe_enabled() && __atomic_load_n(&s_g9_main.enabled, __ATOMIC_RELAXED);
}
void g9_probe_main_configure(bool enabled) {
	/* Startup only, before any producers are launched. */
	memset(&s_g9_main, 0, sizeof s_g9_main);
	__atomic_store_n(&s_g9_main.enabled, enabled, __ATOMIC_RELAXED);
}
static uint32_t* g9_row(uint32_t kind, uint64_t tb) {
	const uint32_t seq = ++s_g9.seq;
	if(s_g9.head - s_g9.tail >= G9_PROBE_CAPACITY) {
		s_g9.lost++;
		return NULL;
	}
	uint32_t* r = s_g9.rows[s_g9.head++ % G9_PROBE_CAPACITY];
	memset(r, 0, G9_PROBE_WORDS * sizeof *r);
	r[0] = seq; r[1] = s_g9.lost; r[2] = kind; r[3] = s_g9.active;
	r[4] = (uint32_t)(tb >> 32); r[5] = (uint32_t)tb;
	return r + 6;
}
static void g9_mesh_snapshot(uint32_t* d) {
	const uint32_t now = stripe_now_us();
	uint32_t oldest = 0;
	for(unsigned i = 0; i < 8; i++) {
		const unsigned state = s_g9.mesh_state[i];
		if(!state) continue;
		if(state <= 3) d[state - 1]++;
		else if(state == 4) d[6]++;
		const uint32_t age = now - s_g9.mesh_start[i];
		if(age > oldest) oldest = age;
	}
	d[3] = oldest; d[4] = s_g9.submissions; d[5] = s_g9.stales;
}
static uint32_t g9_age(uint64_t start) {
	if(!start) return UINT32_MAX;
	uint64_t us = (g9_tb() - start) * 1000000u / G9_TB_HZ;
	return us >= UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}
void g9_probe_main_mark(unsigned region, bool begin, uint32_t work,
	uint32_t backlog, uint32_t oldest_us) {
	g9_capture_main(region, begin, work);
	if(!g9_probe_main_enabled()) return;
	g9_lock_t lock = g9_lock();
	uint32_t* d = g9_row(7, g9_tb());
	if(d) {
		d[0] = s_g9_main.frame; d[1] = region; d[2] = begin ? 1 : 2;
		d[3] = work; d[4] = backlog; d[5] = oldest_us;
		d[6] = s_g9_main.phase;
#ifdef PLATFORM_WII
		KThread* t = KThreadGetSelf();
		d[7] = 127u - t->prio; d[8] = 127u - t->baseprio;
#else
		d[7] = d[8] = UINT32_MAX;
#endif
		g9_mesh_snapshot(d + 9);
	}
	g9_unlock(lock);
}
void g9_probe_main_frame(bool begin) {
	if(!g9_probe_main_enabled()) return;
	if(begin) { ++s_g9_main.frame; s_g9_main.frame_active = 1; }
	else g9_probe_main_mark(1, false, 0, 0, 0);
	g9_probe_main_mark(0, begin, 0, 0, 0);
	if(begin) g9_probe_main_mark(1, true, 0, 0, 0);
	else s_g9_main.frame_active = 0;
}
void g9_probe_main_phase(uint32_t phase) {
	if(!g9_probe_main_enabled() || phase == s_g9_main.phase) return;
	if(s_g9_main.frame_active) g9_probe_main_mark(1, false, 0, 0, 0);
	s_g9_main.phase = phase;
	if(s_g9_main.frame_active) g9_probe_main_mark(1, true, 0, 0, 0);
}
static unsigned g9_light_slot(const void* world) {
	for(unsigned i = 0; i < 2; i++) if(s_g9_main.light[i].world == world) return i;
	for(unsigned i = 0; i < 2; i++) if(!s_g9_main.light[i].world) {
		s_g9_main.light[i].world = world; return i;
	}
	return 2;
}
void g9_probe_light_push(const void* world, unsigned depth) {
	if(!g9_probe_main_enabled()) return;
	unsigned i = g9_light_slot(world);
	if(i < 2 && depth && depth <= 8192) s_g9_main.light[i].seeds[depth-1] = g9_tb();
	else { g9_lock_t lock = g9_lock(); ++s_g9.lost; g9_unlock(lock); }
}
void g9_probe_light_pop(const void* world, unsigned depth) {
	if(!g9_probe_main_enabled()) return;
	unsigned i = g9_light_slot(world);
	if(i < 2) s_g9_main.light[i].active = depth && depth <= 8192
		? s_g9_main.light[i].seeds[depth-1] : 0;
}
uint32_t g9_probe_light_age(const void* world, unsigned depth, bool flood) {
	if(!g9_probe_main_enabled()) return 0;
	unsigned i = g9_light_slot(world);
	if(i == 2 || depth > 8192) return UINT32_MAX;
	uint64_t start = depth ? s_g9_main.light[i].seeds[0] : 0;
	uint64_t active = flood ? s_g9_main.light[i].active : 0;
	if(active && (!start || active < start)) start = active;
	return depth || flood ? g9_age(start) : 0;
}
void g9_probe_light_clear(const void* world) {
	if(!g9_probe_main_enabled()) return;
	for(unsigned i = 0; i < 2; i++) if(s_g9_main.light[i].world == world)
		memset(&s_g9_main.light[i], 0, sizeof s_g9_main.light[i]);
}
void g9_probe_inbox(unsigned slot, bool queued) {
	if(!g9_probe_main_enabled() || slot >= 16) return;
	g9_lock_t lock = g9_lock();
	s_g9_main.inbox[slot] = queued ? g9_tb() : 0;
	g9_unlock(lock);
}
void g9_probe_inbox_stats(uint32_t* count, uint32_t* age) {
	*count = *age = 0;
	if(!g9_probe_main_enabled()) return;
	g9_lock_t lock = g9_lock();
	uint64_t oldest = 0;
	for(unsigned i = 0; i < 16; i++) if(s_g9_main.inbox[i]) {
		++*count;
		if(!oldest || s_g9_main.inbox[i] < oldest) oldest = s_g9_main.inbox[i];
	}
	if(oldest) *age = g9_age(oldest);
	g9_unlock(lock);
}
void g9_probe_configure(bool enabled, bool sampler) {
	g9_lock_t lock = g9_lock();
	assert(!s_g9.active);
	/* Startup-only configuration; preserves queued records if toggled later. */
	s_g9.sampler = sampler;
	__atomic_store_n(&s_g9.enabled, enabled, __ATOMIC_RELAXED);
	g9_unlock(lock);
	if(enabled) g9_probe_role(1);
}
void g9_probe_role(uint32_t role) {
	if(!g9_probe_enabled() || role >= 6) return;
	g9_lock_t lock = g9_lock();
	const uint32_t id = stripe_thread_id();
	for(unsigned i = 1; i < 6; i++)
		if(i != role && s_g9.role_id[i] == id) s_g9.role_id[i] = 0;
	s_g9.role_id[role] = id;
	uint32_t* d = g9_row(5, g9_tb());
	if(d) { d[0] = id; d[1] = role; }
	g9_unlock(lock);
}
/* Called with the recorder lock held. Delayed timer dispatch counts skipped
 * deadlines and emits ONE observation, never duplicate catch-up samples. */
static void g9_sample_locked(uint64_t tb, const uint32_t observation[10]) {
	if(wt_cpu_mode()) { wt_cpu_sample(tb, observation); return; }
	if(!s_g9.active || !s_g9.sampler || tb < s_g9.deadline) return;
	const uint64_t lateness = tb - s_g9.deadline;
	const uint32_t skipped = (uint32_t)(lateness / G9_PERIOD);
	const uint32_t late_us = (uint32_t)(lateness * 1000000u / G9_TB_HZ);
	s_g9.deadline += ((uint64_t)skipped + 1) * G9_PERIOD;
	s_g9.samples++;
	s_g9.missed += skipped;
	if(late_us > 50) s_g9.late++;
	uint32_t* d = g9_row(2, tb);
	if(d) {
		memcpy(d, observation, 10 * sizeof *d);
		d[10] = late_us; d[11] = skipped;
	}
}
void g9_probe_sample(uint64_t tb, const uint32_t observation[10]) {
	if(!g9_probe_enabled() && !wt_cpu_mode()) return;
	g9_lock_t lock = g9_lock();
	g9_sample_locked(tb, observation);
	g9_unlock(lock);
}
#ifdef PLATFORM_WII
static void g9_timer_sample(KTickTask* timer) {
	(void)timer;
	/* interrupt_asm.S saves ctx.pc before KIrqDispatch -> KTickTaskRun.
	 * SPRG2 still identifies the interrupted thread until IRQ return, even
	 * if an earlier tick callback requested a deferred context switch. */
	KThread* current = KThreadGetSelf();
	KThread* server = (KThread*)(uintptr_t)~(wt_cpu_mode() ? s_wt.owner : s_g9.owner);
	uint32_t role = 0;
	const uint32_t id = ~(uint32_t)(uintptr_t)current;
	for(unsigned i = 1; i < 6; i++) if(s_g9.role_id[i] == id) role = i;
	const uint32_t state = server->state;
	uint32_t o[10] = {id, role, 127u - current->prio,
		127u - current->baseprio, state, server->suspend,
		state >= KTHR_STATE_WAITING ? (uint32_t)(uintptr_t)server->wait.queue : 0,
		state >= KTHR_STATE_WAITING ? (uint32_t)server->wait.token : 0,
		__atomic_load_n(&s_role_phase[THREAD_STRIPE_MAIN], __ATOMIC_RELAXED),
		current->ctx.pc};
	const uint64_t now = g9_tb();
	g9_sample_locked(now, o);
	if(s_g9.active || s_wt.cpu_active)
		KTickTaskStart(&s_g9_timer, g9_timer_sample,
			s_g9.deadline > now ? s_g9.deadline - now : G9_PERIOD, 0);
}
#endif
void g9_probe_begin(uint32_t world, uint32_t stage) {
	if(stage >= 2) g9_capture_stage(true, world, stage);
	wt_cpu_stage(stage, true);
	if(!g9_probe_enabled()) return;
	g9_lock_t lock = g9_lock();
	assert(!s_g9.active);
	s_g9.active = ++s_g9.span;
	s_g9.owner = stripe_thread_id();
	if(s_g9.owner != s_g9.role_id[1] && s_g9.role_id[2] != s_g9.owner) {
		for(unsigned i = 1; i < 6; i++)
			if(i != 2 && s_g9.role_id[i] == s_g9.owner) s_g9.role_id[i] = 0;
		s_g9.role_id[2] = s_g9.owner;
		uint32_t* role = g9_row(5, g9_tb());
		if(role) { role[0] = s_g9.owner; role[1] = 2; }
	}
	s_g9.samples = s_g9.late = s_g9.missed = 0;
	const uint64_t tb = g9_tb();
	s_g9.deadline = tb + G9_PERIOD;
	uint32_t* d = g9_row(1, tb);
	if(d) {
		d[0] = s_g9.owner; d[1] = s_g9.sampler; d[2] = world; d[3] = stage;
		g9_mesh_snapshot(d + 4);
		d[11] = s_g9.completed; d[12] = s_g9.retired;
	}
#ifdef PLATFORM_WII
	if(s_g9.sampler) KTickTaskStart(&s_g9_timer, g9_timer_sample, G9_PERIOD, 0);
#endif
	g9_unlock(lock);
}
void g9_probe_end(uint32_t pops, uint32_t elapsed_us) {
	wt_cpu_stage(0, false);
	if(!g9_probe_enabled()) return;
	g9_lock_t lock = g9_lock();
	assert(s_g9.active);
#ifdef PLATFORM_WII
	if(s_g9.sampler) KTickTaskStop(&s_g9_timer);
#endif
	const uint64_t now = g9_tb();
	/* IRQ-disabled tail deadlines are evidence too, even if no callback ran. */
	if(s_g9.sampler && now >= s_g9.deadline)
		s_g9.missed += (uint32_t)((now - s_g9.deadline) / G9_PERIOD) + 1;
	uint32_t* d = g9_row(3, now);
	if(d) {
		d[0] = pops; d[1] = elapsed_us; d[2] = s_g9.samples;
		d[3] = s_g9.late; d[4] = s_g9.missed;
		g9_mesh_snapshot(d + 5);
		d[12] = s_g9.completed; d[13] = s_g9.retired;
	}
	s_g9.active = 0;
	g9_unlock(lock);
}
void g9_probe_mesh(unsigned slot, unsigned state, bool stale) {
	if(!g9_probe_enabled() || slot >= 8) return;
	g9_lock_t lock = g9_lock();
	if(state == 4) s_g9.mesh_start[slot] = stripe_now_us();
	if(state == 1) s_g9.submissions++;
	if(state == 3 && !stale) s_g9.completed++;
	if(state == 0 && s_g9.mesh_state[slot]) s_g9.retired++;
	if(stale) s_g9.stales++;
	s_g9.mesh_state[slot] = state;
	g9_unlock(lock);
}
void g9_probe_rpc_pool(struct thread_channel* pool) { s_g9_rpc_pool = pool; }
void g9_probe_rpc(unsigned transition, const void* token) {
	if(!g9_probe_enabled()) return;
	uint32_t active = __atomic_load_n(&s_g9.rpc_active, __ATOMIC_RELAXED);
	/* Returning/reacquiring a token in an unexhausted pool is not an RPC
	 * wait. Avoid taking the observer mutex for that normal PC traffic. */
	if((transition == 1 && active) || (transition != 1 && !active)) return;
	g9_lock_t lock = g9_lock();
	if(transition == 1 && s_g9.rpc_active) {
		g9_unlock(lock);
		return; /* repeated 1ms polling preserves the first exhaustion */
	}
	if(transition == 1) {
		__atomic_store_n(&s_g9.rpc_active, ++s_g9.rpc_id, __ATOMIC_RELAXED);
		s_g9.rpc_owner = stripe_thread_id();
	}
	if(s_g9.rpc_active) {
		uint32_t* d = g9_row(4, g9_tb());
		if(d) {
			d[0] = s_g9.rpc_active; d[1] = transition;
			d[2] = stripe_thread_id(); d[3] = (uint32_t)(uintptr_t)token;
			d[6] = __atomic_load_n(&s_role_phase[THREAD_STRIPE_MAIN], __ATOMIC_RELAXED);
#ifdef PLATFORM_WII
			KThread* waiter = (KThread*)(uintptr_t)~s_g9.rpc_owner;
			KThread* current = KThreadGetSelf();
			d[4] = waiter->state; d[5] = waiter->suspend;
			d[7] = 127u - current->prio; d[8] = 127u - current->baseprio;
#else
			d[4] = d[5] = d[7] = d[8] = UINT32_MAX;
#endif
		}
	}
	if(transition >= 3) __atomic_store_n(&s_g9.rpc_active, 0, __ATOMIC_RELAXED);
	g9_unlock(lock);
}
bool g9_probe_pump(bool (*emit)(const char*)) {
	if(g9_capture_pump(emit)) return true;
	if(!g9_probe_enabled() || !emit) return false;
	_Static_assert(8u * G9_PROBE_WORDS * 11u + 128u < 2304u,
		"G9 batch must fit the observer wire limit including delimiters");
	uint32_t rows[8][G9_PROBE_WORDS];
	g9_lock_t lock = g9_lock();
	/* Keep formatting/network work out of a live drain. */
	if(s_g9.active) { g9_unlock(lock); return false; }
	uint32_t count = s_g9.head - s_g9.tail;
	const uint32_t lost = s_g9.lost, produced = s_g9.seq;
	if(count > 8) count = 8;
	for(unsigned i = 0; i < count; i++)
		memcpy(rows[i], s_g9.rows[(s_g9.tail + i) % G9_PROBE_CAPACITY], sizeof rows[i]);
	g9_unlock(lock);
	if(!count) return false;
	char line[2304];
	int n = snprintf(line, sizeof line,
		"{\"event\":\"g9_cpu\",\"v\":1,\"tb_hz\":%u,\"lost\":%u,\"produced\":%u,\"rows\":[",
		G9_TB_HZ, lost, produced);
	if(n < 0 || (size_t)n >= sizeof line) return false;
	size_t off = (size_t)n;
	for(unsigned i = 0; i < count; i++) {
		for(unsigned k = 0; k < G9_PROBE_WORDS; k++) {
			n = snprintf(line + off, sizeof line - off, k ? ",%u" : (i ? ",[%u" : "[%u"), rows[i][k]);
			if(n < 0 || (size_t)n >= sizeof line - off) return false;
			off += (size_t)n;
		}
		line[off++] = ']';
	}
	memcpy(line + off, "]}", 3);
	if(!emit(line)) return false;
	lock = g9_lock();
	s_g9.tail += count;
	g9_unlock(lock);
	return true;
}

/* REL-10 whole-tick MAIN pacing: the registered local gameplay-server tick
 * owner (thread_g9_tick_boost_local), whether it is inside a gameplay tick
 * (thread_g9_tick_boost_begin..end = update + epilogue) and its current
 * wait/dependency depth. Fixed-size 32-bit words; only the owner thread
 * writes active/waits, MAIN only reads. Independent of the lease, the boost
 * and every diagnostic toggle. */
static uint32_t s_tick_owner, s_tick_active, s_tick_waits;
/* REL-10-overdue (hw1030f FAIL, docs/g9_tick_boost.md "Overdue escalation"):
 * the owner stamps each gameplay tick's begin (time_get microseconds, 32-bit
 * so MAIN's read can never tear) and bumps a serial; MAIN uses the age to
 * decide whether the tick is overdue and the serial to reset its per-tick
 * escalation budget. Owner writes, MAIN reads. */
static uint32_t s_tick_begin_us, s_tick_serial;
static uint32_t pacing_us_of(ptime_t t) {
#ifdef PLATFORM_WII
	return (uint32_t)ticks_to_microsecs(t); /* ptime_t = timebase ticks */
#else
	return (uint32_t)((uint64_t)t.tv_sec * 1000000u + (uint32_t)t.tv_nsec / 1000u);
#endif
}
static uint32_t pacing_now_us(void) { return pacing_us_of(time_get()); }
static void g9_thread_joined(uint32_t id) {
	/* A joined thread can never be a donation target again (stale owner). */
	uint32_t owner = id;
	if(__atomic_compare_exchange_n(&s_tick_owner, &owner, 0, false,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
		__atomic_store_n(&s_tick_active, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&s_tick_waits, 0, __ATOMIC_RELEASE);
	}
	if(!g9_probe_enabled()) return;
	g9_lock_t lock = g9_lock();
	/* An unsuccessful nonblocking send can leave an exhaustion outstanding
	 * when the server exits. Never retain its freed KThread for a later main
	 * token-return snapshot. Cancellation after join has no live waiter state. */
	if(s_g9.rpc_active && s_g9.rpc_owner == id) {
		uint32_t* d = g9_row(4, g9_tb());
		if(d) {
			d[0] = s_g9.rpc_active; d[1] = 4; d[2] = stripe_thread_id();
			d[4] = d[5] = d[7] = d[8] = UINT32_MAX;
		}
		__atomic_store_n(&s_g9.rpc_active, 0, __ATOMIC_RELAXED);
	}
	for(unsigned i = 1; i < 6; i++) if(s_g9.role_id[i] == id) s_g9.role_id[i] = 0;
	g9_unlock(lock);
}

/* Single drain owner; no PPC TLS or shared 64-bit atomics. */
static uint32_t s_drain_owner;
static struct thread_drain_waits s_drain_waits;
static bool drain_owner(void) {
	uint32_t id = __atomic_load_n(&s_drain_owner, __ATOMIC_ACQUIRE);
	return id && id == stripe_thread_id();
}
/* ci-1101/37: keep the LONGEST >= 10 ms mutex wait of this window. The lock
 * path is the only caller, and only the single drain owner ever writes it.
 * holder is the id sampled from the mutex's own owner field BEFORE we blocked
 * (it is 0 once we hold the lock, so it MUST be read on the wait side, never
 * after); 0 means free or unknown. No lock, no allocation, no syscall -- the
 * cost is one compare when the wait is short, and this bounded body plus one
 * small copy on the rare long wait that lands. */
static unsigned stripe_index(struct thread_mutex* mutex);
static uint32_t stripe_role(uint32_t id);
static void drain_wait_record(uint32_t wait_us, struct thread_mutex* m,
							  uint32_t holder) {
	if(wait_us < THREAD_DRAIN_WAIT_ATTRIB_US || wait_us <= s_drain_waits.wait_us)
		return;
	s_drain_waits.wait_us = wait_us;
	s_drain_waits.wait_mutex = (uint32_t)(uintptr_t)m;
	s_drain_waits.wait_holder_id = holder;
	s_drain_waits.wait_holder_tag = holder ? stripe_role(holder) : 0;
	unsigned idx = stripe_index(m);
	s_drain_waits.wait_stripe = idx < 8 ? idx : 0;
	const char* name = idx < 8 && s_stripes[idx].name ? s_stripes[idx].name : "";
	size_t n = strlen(name);
	if(n >= sizeof s_drain_waits.wait_mutex_name)
		n = sizeof s_drain_waits.wait_mutex_name - 1;
	memcpy(s_drain_waits.wait_mutex_name, name, n);
	s_drain_waits.wait_mutex_name[n] = '\0';
}
bool thread_drain_wait_active(void) { return drain_owner(); }
static struct {
	bool enabled;
	uint32_t owner, slice_us, sleep_ms, frame_sleep_ms, remaining_ms;
	ptime_t start;
	bool cap_hit;
	uint32_t stats[THREAD_PACING_WORDS];
	/* REL-10-overdue: an owner tick older than overdue_us is escalated --
	 * the frame cap is bypassed and the slice is halved -- for at most
	 * overdue_budget_ms of escalated sleep per tick (serial-keyed). */
	uint32_t overdue_us, overdue_budget_ms, overdue_left_ms, overdue_serial;
	uint32_t ostats[THREAD_PACING_OVERDUE_WORDS];
} s_main_pacing;
static void pacing_add(unsigned i, uint64_t n) {
	uint32_t* v = &s_main_pacing.stats[i];
	if(n > UINT32_MAX - *v) { *v = UINT32_MAX; s_main_pacing.stats[15] = 1; }
	else *v += (uint32_t)n;
}

/* ---- REL-10 whole-tick eligibility state (see s_tick_owner above) -------- */
static uint32_t s_main_holds; /* MAIN-only: thread_mutex/stripe locks held */
static bool tick_owner_self(void) {
	const uint32_t owner = __atomic_load_n(&s_tick_owner, __ATOMIC_ACQUIRE);
	return owner && owner == stripe_thread_id();
}
/* The owner is the only writer of s_tick_waits; a guarded decrement keeps a
 * wait that began before registration (or across a teardown) from wrapping. */
static void tick_wait_enter(void) {
	if(!tick_owner_self()) return;
	__atomic_store_n(&s_tick_waits,
		__atomic_load_n(&s_tick_waits, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELEASE);
}
static void tick_wait_leave(void) {
	if(!tick_owner_self()) return;
	const uint32_t v = __atomic_load_n(&s_tick_waits, __ATOMIC_RELAXED);
	if(v) __atomic_store_n(&s_tick_waits, v - 1u, __ATOMIC_RELEASE);
}
/* MAIN's own lock holds: MAIN never sleeps holding a lock the server needs. */
static void main_hold(bool acquired) {
	if(stripe_thread_id() != s_main_pacing.owner) return;
	if(acquired) s_main_holds++;
	else if(s_main_holds) s_main_holds--;
}
#ifdef PLATFORM_WII
static bool thread_cached_ram_range(uintptr_t addr, size_t bytes);
/* Scheduler view, no diagnostic toggle: Tuxedo's KTHR_STATE_RUNNING is
 * "ready or running" (MAIN is the one running), so anything else -- a sleep,
 * an IOS/SD wait, a mutex or queue wait, a suspension -- is not runnable and
 * a MAIN sleep would donate to nobody. Same handle convention and validation
 * as thread_saved_context_snapshot_raw; scalar reads only, IRQs masked. */
static bool tick_owner_runnable(uint32_t id) {
	if((KThread*)(uintptr_t)~(uint32_t)LWP_GetSelf() != KThreadGetSelf()) return false;
	KThread* const kt = (KThread*)(uintptr_t)~id;
	if(((uintptr_t)kt & (sizeof(uint32_t) - 1u)) != 0
	   || !thread_cached_ram_range((uintptr_t)kt, sizeof(*kt)))
		return false;
	PPCIrqState irq = PPCIrqLockByMsr();
	const bool runnable = kt != KThreadGetSelf()
		&& kt->state == KTHR_STATE_RUNNING && kt->suspend == 0;
	PPCIrqUnlockByMsr(irq);
	return runnable;
}
#else
/* PC pthreads expose no scheduler state; the owner's waits are tracked at
 * every thread.c wait primitive (tick_wait_enter/leave) instead. */
static bool tick_owner_runnable(uint32_t id) { (void)id; return true; }
#endif
/* Public lease dependency entry points: unchanged lease behaviour plus the
 * owner's wait depth (explicit dependencies: MAIN-owned recovery, COW
 * allocation gaps, channel/condition waits, sleeps). */
void thread_g9_lease_dependency(void) {
	tick_wait_enter();
	g9_lease_dependency_impl();
}
void thread_g9_lease_dependency_done(void) {
	tick_wait_leave();
	g9_lease_dependency_done_impl();
}
/* Tick owner registration / gameplay-tick boundaries. Published BEFORE the
 * boost body on entry and after clearing on exit, whatever the boost state. */
void thread_g9_tick_boost_local(bool present, uint8_t base_priority) {
	const uint32_t self = stripe_thread_id();
	if(present) {
		/* One owner: a second registrant (or MAIN itself) never steals it.
		 * active/waits are already 0 (cleared at every unregister/join). */
		uint32_t expected = 0;
		if(self != s_main_pacing.owner)
			(void)__atomic_compare_exchange_n(&s_tick_owner, &expected, self, false,
				__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	} else if(tick_owner_self()) {
		__atomic_store_n(&s_tick_owner, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&s_tick_active, 0, __ATOMIC_RELEASE);
		__atomic_store_n(&s_tick_waits, 0, __ATOMIC_RELEASE);
	}
	g9_tick_boost_local_impl(present, base_priority);
}
void thread_g9_tick_boost_begin(void) {
	if(tick_owner_self()) {
		/* Stamp and serial BEFORE active: a MAIN that sees active=1 sees the
		 * begin of THIS tick, never the previous one's. */
		__atomic_store_n(&s_tick_begin_us, pacing_now_us(), __ATOMIC_RELEASE);
		__atomic_store_n(&s_tick_serial,
			__atomic_load_n(&s_tick_serial, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELEASE);
		__atomic_store_n(&s_tick_active, 1, __ATOMIC_RELEASE);
	}
	g9_tick_boost_begin_impl();
}
void thread_g9_tick_boost_end(void) {
	g9_tick_boost_end_impl();
	if(tick_owner_self()) __atomic_store_n(&s_tick_active, 0, __ATOMIC_RELEASE);
}
/* MAIN-only. The thread a checkpoint may donate to right now, or 0. A
 * foreign normal-light drain keeps the pre-REL-10 eligibility unless its
 * owner is the registered tick owner; the tick owner is eligible inside a
 * gameplay tick (or its drain) only while runnable and outside every
 * wait/dependency. MAIN holding a lock, or a lease waiting on MAIN service,
 * refuses everything. *self_drain reports MAIN owning the drain itself. */
static uint32_t pacing_donee(uint32_t self, bool* self_drain) {
	const uint32_t drain = __atomic_load_n(&s_drain_owner, __ATOMIC_ACQUIRE);
	*self_drain = drain && drain == self;
	if(*self_drain || s_main_holds) return 0;
	if(s_lease.enabled) {
		g9_lock_t lock = g9_lock();
		const bool needs_main = s_lease.active && s_lease.waiting;
		g9_unlock(lock);
		if(needs_main) return 0;
	}
	const uint32_t owner = __atomic_load_n(&s_tick_owner, __ATOMIC_ACQUIRE);
	if(drain && drain != owner) return drain;
	if(!owner || owner == self) return 0;
	if(drain != owner && !__atomic_load_n(&s_tick_active, __ATOMIC_ACQUIRE)) return 0;
	if(__atomic_load_n(&s_tick_waits, __ATOMIC_ACQUIRE)) return 0;
	if(!tick_owner_runnable(owner)) return 0;
	/* The owner may have unregistered meanwhile: never donate to a stale id. */
	if(__atomic_load_n(&s_tick_owner, __ATOMIC_ACQUIRE) != owner) return 0;
	return owner;
}
bool thread_main_pacing_snapshot(uint32_t out[THREAD_PACING_WORDS]) {
	assert(s_main_pacing.owner == stripe_thread_id());
	if(!s_main_pacing.enabled && !g9_probe_enabled()) return false;
	memcpy(out, s_main_pacing.stats, sizeof s_main_pacing.stats);
	return true;
}
void thread_main_pacing_configure(bool enabled, int slice_us,
	int sleep_ms, int frame_sleep_ms) {
	/* Startup config is published before threads start. Afterwards only main
	 * mutates this state; foreign checkpoints return before reading timers. */
	s_main_pacing.enabled = enabled;
	s_main_pacing.owner = stripe_thread_id();
	s_main_pacing.slice_us
		= slice_us < 500 ? 500 : slice_us > 10000 ? 10000 : slice_us;
	s_main_pacing.sleep_ms = sleep_ms < 1 ? 1 : sleep_ms > 2 ? 2 : sleep_ms;
	s_main_pacing.frame_sleep_ms
		= frame_sleep_ms < 1 ? 1 : frame_sleep_ms > 16 ? 16 : frame_sleep_ms;
	memset(s_main_pacing.stats, 0, sizeof s_main_pacing.stats);
	s_main_pacing.stats[0] = enabled;
	s_main_pacing.stats[1] = s_main_pacing.slice_us;
	s_main_pacing.stats[2] = s_main_pacing.sleep_ms;
	s_main_pacing.stats[3] = s_main_pacing.frame_sleep_ms;
	/* Escalation defaults (docs/g9_tick_boost.md "Overdue escalation");
	 * thread_main_pacing_overdue_configure may override them afterwards. */
	thread_main_pacing_overdue_configure(THREAD_PACING_OVERDUE_MS_DEFAULT,
		THREAD_PACING_OVERDUE_BUDGET_MS_DEFAULT);
	thread_main_pacing_frame();
}
void thread_main_pacing_overdue_configure(int overdue_ms, int budget_ms) {
	/* MAIN-only, like configure. overdue_ms 0 = escalation OFF (REL-10
	 * behaviour unchanged); otherwise clamped to 4..50 ms, budget 1..200 ms. */
	s_main_pacing.overdue_us = overdue_ms <= 0 ? 0
		: (uint32_t)(overdue_ms < 4 ? 4 : overdue_ms > 50 ? 50 : overdue_ms) * 1000u;
	s_main_pacing.overdue_budget_ms
		= (uint32_t)(budget_ms < 1 ? 1 : budget_ms > 200 ? 200 : budget_ms);
	s_main_pacing.overdue_left_ms = s_main_pacing.overdue_budget_ms;
	s_main_pacing.overdue_serial = __atomic_load_n(&s_tick_serial, __ATOMIC_ACQUIRE);
	memset(s_main_pacing.ostats, 0, sizeof s_main_pacing.ostats);
	s_main_pacing.ostats[0] = s_main_pacing.overdue_us;
	s_main_pacing.ostats[1] = s_main_pacing.overdue_budget_ms;
}
bool thread_main_pacing_overdue_snapshot(uint32_t out[THREAD_PACING_OVERDUE_WORDS]) {
	assert(s_main_pacing.owner == stripe_thread_id());
	if(!s_main_pacing.enabled) return false;
	memcpy(out, s_main_pacing.ostats, sizeof s_main_pacing.ostats);
	return true;
}
/* MAIN-only. True when `donee` is the registered tick owner, inside a
 * gameplay tick older than overdue_us, and this tick's escalation budget is
 * not spent. A new tick serial resets the budget. Never true for a foreign
 * drain donee (that path keeps the pre-REL-10 rules) or with escalation OFF. */
static bool pacing_overdue(uint32_t donee, uint32_t now_us) {
	if(!s_main_pacing.overdue_us) return false;
	if(donee != __atomic_load_n(&s_tick_owner, __ATOMIC_ACQUIRE)) return false;
	if(!__atomic_load_n(&s_tick_active, __ATOMIC_ACQUIRE)) return false;
	const uint32_t serial = __atomic_load_n(&s_tick_serial, __ATOMIC_ACQUIRE);
	if(serial != s_main_pacing.overdue_serial) {
		s_main_pacing.overdue_serial = serial;
		s_main_pacing.overdue_left_ms = s_main_pacing.overdue_budget_ms;
	}
	const uint32_t begin = __atomic_load_n(&s_tick_begin_us, __ATOMIC_ACQUIRE);
	if(now_us - begin < s_main_pacing.overdue_us) return false;
	if(s_main_pacing.overdue_left_ms < s_main_pacing.sleep_ms) {
		s_main_pacing.ostats[5]++; /* budget exhausted: normal rules resume */
		return false;
	}
	return true;
}
bool thread_main_pacing_enabled(void) { return s_main_pacing.enabled; }
void thread_main_pacing_frame(void) {
	if(!s_main_pacing.enabled) return;
	assert(s_main_pacing.owner == stripe_thread_id());
	s_main_pacing.cap_hit = false;
	s_main_pacing.remaining_ms = s_main_pacing.frame_sleep_ms;
	s_main_pacing.start = time_get();
}
bool thread_main_pacing_checkpoint(void) {
	return thread_main_pacing_checkpoint_at(THREAD_PACING_OTHER);
}
bool thread_main_pacing_checkpoint_at(enum thread_pacing_phase phase) {
	if(!s_main_pacing.enabled && !g9_probe_enabled()) return false;
	const uint32_t self = stripe_thread_id();
	if(self != s_main_pacing.owner) return false;
	assert(phase < THREAD_PACING_PHASE_COUNT);
	pacing_add(4, 1);
	bool self_drain = false;
	const uint32_t drain = pacing_donee(self, &self_drain);
	if(!drain) {
		pacing_add(self_drain ? 7 : 6, 1);
		if(s_main_pacing.enabled) s_main_pacing.start = time_get();
		return false;
	}
	pacing_add(5, 1);
	if(!s_main_pacing.enabled) return false;
	/* REL-10-overdue: an overdue owner tick bypasses the frame cap and runs a
	 * half slice, bounded per tick by overdue_budget_ms (pacing_overdue). */
	const ptime_t now = time_get(); /* the one clock read of the common path */
	const uint64_t elapsed_us = time_diff_us(s_main_pacing.start, now);
	if(elapsed_us < s_main_pacing.slice_us / 2u) {
		pacing_add(8, 1); return false; /* short of even the escalated slice */
	}
	const bool overdue = pacing_overdue(drain, pacing_us_of(now));
	const bool capped = s_main_pacing.remaining_ms < s_main_pacing.sleep_ms;
	if(capped && !overdue) {
		pacing_add(9, 1);
		if(!s_main_pacing.cap_hit) { pacing_add(10, 1); s_main_pacing.cap_hit = true; }
		return false;
	}
	if(elapsed_us < (overdue ? s_main_pacing.slice_us / 2u : s_main_pacing.slice_us)) {
		pacing_add(8, 1); return false;
	}
	/* Recheck immediately before sleeping: the donee may have blocked,
	 * finished its tick or unregistered since the first look. Counted as a
	 * no-donee observation (word 6) and the slice restarts, as above. */
	if(pacing_donee(self, &self_drain) != drain) {
		pacing_add(6, 1);
		s_main_pacing.start = time_get();
		return false;
	}
	if(overdue) {
		/* [2] ticks escalated: the first escalated sleep of a tick serial. */
		if(s_main_pacing.ostats[3] == 0
		   || s_main_pacing.ostats[6] != s_main_pacing.overdue_serial)
			s_main_pacing.ostats[2]++;
		s_main_pacing.ostats[6] = s_main_pacing.overdue_serial;
		s_main_pacing.ostats[3]++;            /* [3] escalated sleeps */
		if(capped) s_main_pacing.ostats[4]++; /* [4] sleeps the frame cap refused */
		s_main_pacing.overdue_left_ms -= s_main_pacing.sleep_ms;
	}
	if(!capped) s_main_pacing.remaining_ms -= s_main_pacing.sleep_ms;
	if(s_main_pacing.remaining_ms < s_main_pacing.sleep_ms && !s_main_pacing.cap_hit) {
		pacing_add(10, 1); s_main_pacing.cap_hit = true;
	}
	const uint64_t before_tb = g9_probe_enabled() ? g9_tb() : 0;
	const ptime_t before = time_get();
	thread_msleep(s_main_pacing.sleep_ms);
	s_main_pacing.start = time_get();
	const uint64_t wake = time_diff_us(before, s_main_pacing.start);
	pacing_add(11, 1); pacing_add(12, s_main_pacing.sleep_ms); pacing_add(13, wake);
	pacing_add(16 + phase, 1);
	if(wake > s_main_pacing.stats[14]) {
		s_main_pacing.stats[14] = wake > UINT32_MAX ? UINT32_MAX : (uint32_t)wake;
		if(wake > UINT32_MAX) s_main_pacing.stats[15] = 1;
	}
	if(g9_probe_enabled()) {
		g9_lock_t lock = g9_lock();
		uint32_t* d = g9_row(6, before_tb);
		if(d) {
			d[0] = phase; d[1] = drain; d[2] = s_main_pacing.sleep_ms;
			d[3] = wake > UINT32_MAX ? UINT32_MAX : (uint32_t)wake;
			d[4] = s_main_pacing.remaining_ms;
		}
		g9_unlock(lock);
	}
	return true;
}
void thread_drain_wait_begin(void) {
	s_drain_waits = (struct thread_drain_waits){0};
	__atomic_store_n(&s_drain_owner, stripe_thread_id(), __ATOMIC_RELEASE);
}
void thread_drain_wait_end(struct thread_drain_waits* out) {
	assert(drain_owner());
	__atomic_store_n(&s_drain_owner, 0, __ATOMIC_RELEASE);
	*out = s_drain_waits;
}
void thread_drain_recovery_sleep(void) {
	thread_g9_lease_dependency();
	bool armed = drain_owner();
	ptime_t start = time_get();
	thread_msleep(1);
	if(armed) s_drain_waits.recovery_us += time_diff_us(start, time_get());
	thread_g9_lease_dependency_done();
}

void thread_stripe_register(struct thread_mutex* mutex, unsigned stripe) {
	assert(stripe < 8 && mutex);
	if(stripe < 8) s_stripes[stripe].mutex = mutex;
}

static unsigned stripe_index(struct thread_mutex* mutex);

/* ci-1101/37: borrowed operator-facing name for a registered stripe's mutex,
 * read only on the (rare) long-drain-wait path. Call after register; a mutex
 * registered more than once keeps the last name. */
void thread_stripe_set_name(struct thread_mutex* mutex, const char* name) {
	unsigned i = stripe_index(mutex);
	if(i < 8) s_stripes[i].name = name;
}

/* A thread holds exactly ONE role. Thread ids are recycled after a thread
 * exits (glibc reuses a joined pthread_t at once; libogc reuses a freed LWP
 * handle), so a role slot can still carry a DEAD thread's id when a new thread
 * with that same id claims a different role -- e.g. the per-world server
 * thread exits and a restarted mesher gets its handle. stripe_role() scans
 * MAIN, SERVER, MESHER in order, so without eviction that mesher's locks were
 * filed (and reported in the hang breadcrumb) under the stale SERVER role.
 * Claiming a role therefore evicts this id from every other slot. The CAS only
 * clears a slot that still holds OUR id, so it can never erase a live
 * thread's claim; the fast path (role already ours) stays one relaxed load. */
void thread_stripe_phase(enum thread_stripe_tag tag, uint32_t phase) {
	if(tag == THREAD_STRIPE_MAIN) g9_probe_main_phase(phase);
	if(tag <= THREAD_STRIPE_OTHER || tag > THREAD_STRIPE_MESHER) return;
	const uint32_t id = stripe_thread_id();
	if(__atomic_load_n(&s_role_id[tag], __ATOMIC_RELAXED) != id) {
		for(uint32_t other = THREAD_STRIPE_MAIN; other <= THREAD_STRIPE_MESHER; other++) {
			uint32_t expected = id;
			if(other != (uint32_t)tag)
				__atomic_compare_exchange_n(&s_role_id[other], &expected, 0, false,
					__ATOMIC_RELAXED, __ATOMIC_RELAXED);
		}
		__atomic_store_n(&s_role_id[tag], id, __ATOMIC_RELAXED);
	}
	__atomic_store_n(&s_role_phase[tag], phase, __ATOMIC_RELAXED);
}

static unsigned stripe_index(struct thread_mutex* mutex) {
	for(unsigned i = 0; i < 8; i++)
		if(s_stripes[i].mutex == mutex) return i;
	return 8;
}

static uint32_t stripe_role(uint32_t id) {
	for(uint32_t tag = THREAD_STRIPE_MAIN; tag <= THREAD_STRIPE_MESHER; tag++)
		if(__atomic_load_n(&s_role_id[tag], __ATOMIC_RELAXED) == id) return tag;
	return THREAD_STRIPE_OTHER;
}

/* SEQLOCK DISCIPLINE (all three records here: role records, the completed
 * record, the active-wait record). C11 seqlock per Boehm, "Can seqlocks get
 * along with programming language memory models?" (MSPC 2012):
 *   writer: seq++ (odd); RELEASE FENCE; payload stores; seq++ (release)
 *   reader: a = seq (acquire); payload loads; ACQUIRE FENCE; b = seq
 * The writer fence keeps the odd increment ahead of every payload store; the
 * reader fence keeps every payload load ahead of the final sequence load. An
 * acquire or acq_rel RMW/load does neither: acquire orders LATER accesses
 * after itself, release orders EARLIER ones before itself. On Broadway GCC
 * lowers both fences to lwsync (verified in the devkitPPC objdump; see the
 * commit that added them). */
static void stripe_record_set(struct stripe_record* r, uint32_t id, uint32_t state) {
	__atomic_fetch_add(&r->seq, 1, __ATOMIC_ACQ_REL);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	stripe_test_event(THREAD_STRIPE_TEST_ODD);
	__atomic_store_n(&r->id, id, __ATOMIC_RELAXED);
	__atomic_store_n(&r->state, state, __ATOMIC_RELAXED);
	__atomic_fetch_add(&r->seq, 1, __ATOMIC_RELEASE);
}

/* Three attempts only: the watchdog must never wait for a preempted writer. */
static bool stripe_record_get(struct stripe_record* r, uint32_t* id, uint32_t* state) {
	for(unsigned attempt = 0; attempt < 3; attempt++) {
		uint32_t a = __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE);
		if(a & 1) continue;
		stripe_test_event(THREAD_STRIPE_TEST_READ_BEGIN);
		uint32_t value_id = __atomic_load_n(&r->id, __ATOMIC_RELAXED);
		uint32_t value_state = __atomic_load_n(&r->state, __ATOMIC_RELAXED);
		stripe_test_event(THREAD_STRIPE_TEST_READ_VALIDATE);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		uint32_t b = __atomic_load_n(&r->seq, __ATOMIC_RELAXED);
		if(a == b) { *id = value_id; *state = value_state; return true; }
	}
	return false;
}

static void stripe_holder_get(unsigned stripe, struct thread_stripe_breadcrumb* out) {
	uint32_t best = THREAD_STRIPE_IDLE;
	for(unsigned tag = THREAD_STRIPE_OTHER; tag <= THREAD_STRIPE_MESHER; tag++) {
		uint32_t id, state;
		if(!stripe_record_get(&s_stripes[stripe].role[tag], &id, &state)) {
			out->uncertain = 1;
			continue;
		}
		if(state > THREAD_STRIPE_RELEASING) { out->uncertain = 1; continue; }
		/* A held/releasing owner takes precedence over an acquiring waiter. */
		uint32_t rank = state == THREAD_STRIPE_HELD ? 3
			: state == THREAD_STRIPE_RELEASING ? 2
			: state == THREAD_STRIPE_ACQUIRING ? 1 : 0;
		if(rank > best) {
			best = rank;
			out->holder_id = id;
			out->holder_tag = tag;
			out->holder_state = state;
		}
	}
	if(best) {
		if(out->holder_state != THREAD_STRIPE_HELD || out->holder_tag == THREAD_STRIPE_OTHER)
			out->uncertain = 1;
		out->holder_phase = out->holder_tag <= THREAD_STRIPE_MESHER
			? __atomic_load_n(&s_role_phase[out->holder_tag], __ATOMIC_RELAXED) : 0;
	}
}
#ifdef PLATFORM_PC
void thread_stripe_test_snapshot(unsigned stripe, struct thread_stripe_breadcrumb* out) {
	assert(stripe < 8 && out);
	*out = (struct thread_stripe_breadcrumb){.stripe = stripe};
	stripe_holder_get(stripe, out);
}
#endif

static void stripe_completed_set(const struct thread_stripe_breadcrumb* value) {
	__atomic_fetch_add(&s_completed_seq, 1, __ATOMIC_ACQ_REL);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&s_completed.stripe, value->stripe, __ATOMIC_RELAXED);
	__atomic_store_n(&s_completed.holder_id, value->holder_id, __ATOMIC_RELAXED);
	__atomic_store_n(&s_completed.holder_tag, value->holder_tag, __ATOMIC_RELAXED);
	__atomic_store_n(&s_completed.holder_phase, value->holder_phase, __ATOMIC_RELAXED);
	__atomic_store_n(&s_completed.holder_state, value->holder_state, __ATOMIC_RELAXED);
	__atomic_store_n(&s_completed.uncertain, value->uncertain, __ATOMIC_RELAXED);
	__atomic_store_n(&s_completed.wait_us, value->wait_us, __ATOMIC_RELAXED);
	__atomic_fetch_add(&s_completed_seq, 1, __ATOMIC_RELEASE);
}

static bool stripe_completed_get(struct thread_stripe_breadcrumb* out) {
	for(unsigned attempt = 0; attempt < 3; attempt++) {
		uint32_t a = __atomic_load_n(&s_completed_seq, __ATOMIC_ACQUIRE);
		if(a & 1) continue;
		stripe_test_event(THREAD_STRIPE_TEST_READ_BEGIN);
		out->stripe = __atomic_load_n(&s_completed.stripe, __ATOMIC_RELAXED);
		out->holder_id = __atomic_load_n(&s_completed.holder_id, __ATOMIC_RELAXED);
		out->holder_tag = __atomic_load_n(&s_completed.holder_tag, __ATOMIC_RELAXED);
		out->holder_phase = __atomic_load_n(&s_completed.holder_phase, __ATOMIC_RELAXED);
		out->holder_state = __atomic_load_n(&s_completed.holder_state, __ATOMIC_RELAXED);
		out->uncertain = __atomic_load_n(&s_completed.uncertain, __ATOMIC_RELAXED);
		out->wait_us = __atomic_load_n(&s_completed.wait_us, __ATOMIC_RELAXED);
		stripe_test_event(THREAD_STRIPE_TEST_READ_VALIDATE);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		uint32_t b = __atomic_load_n(&s_completed_seq, __ATOMIC_RELAXED);
		if(a == b) { out->active = 0; return true; }
	}
	out->uncertain = 1;
	out->active = 0;
	return false;
}

/* Main thread only (the single writer). stripe_plus_one == 0 clears. */
static void stripe_wait_set(uint32_t stripe_plus_one, uint32_t start_us) {
	__atomic_fetch_add(&s_wait_seq, 1, __ATOMIC_ACQ_REL);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&s_wait_start_us, start_us, __ATOMIC_RELAXED);
	__atomic_store_n(&s_wait_stripe_plus_one, stripe_plus_one, __ATOMIC_RELAXED);
	__atomic_fetch_add(&s_wait_seq, 1, __ATOMIC_RELEASE);
}

/* Bounded like the other readers. On success *seq is the even sequence the
 * pair was read under, for the post-scan re-validation. */
static bool stripe_wait_get(uint32_t* seq, uint32_t* slot, uint32_t* start) {
	for(unsigned attempt = 0; attempt < 3; attempt++) {
		uint32_t a = __atomic_load_n(&s_wait_seq, __ATOMIC_ACQUIRE);
		if(a & 1) continue;
		uint32_t value_slot = __atomic_load_n(&s_wait_stripe_plus_one, __ATOMIC_RELAXED);
		stripe_test_event(THREAD_STRIPE_TEST_ACTIVE_SLOT);
		uint32_t value_start = __atomic_load_n(&s_wait_start_us, __ATOMIC_RELAXED);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		uint32_t b = __atomic_load_n(&s_wait_seq, __ATOMIC_RELAXED);
		if(a == b) { *seq = a; *slot = value_slot; *start = value_start; return true; }
	}
	return false;
}

bool thread_stripe_breadcrumb_get(struct thread_stripe_breadcrumb* out) {
	if(!out) return false;
	*out = (struct thread_stripe_breadcrumb){0};
	uint32_t wait_seq = 0, slot = 0, start = 0;
	const bool wait_valid = stripe_wait_get(&wait_seq, &slot, &start);
	if(wait_valid && slot && slot <= 8) {
		uint32_t wait = stripe_now_us() - start;
		if(wait >= THREAD_STRIPE_WAIT_THRESHOLD_US) {
			out->stripe = slot - 1;
			stripe_holder_get(out->stripe, out);
			out->wait_us = wait;
			out->active = 1;
			stripe_test_event(THREAD_STRIPE_TEST_ACTIVE_HOLDER);
			/* The holder scan and the wait it explains must describe the
			 * same wait: if the main thread finished (or republished) it
			 * meanwhile, the stripe/holder/timestamp triple is not one
			 * moment -- report it, but uncertain. */
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			if(__atomic_load_n(&s_wait_seq, __ATOMIC_RELAXED) != wait_seq)
				out->uncertain = 1;
			return true;
		}
	}
	const bool completed_valid = stripe_completed_get(out);
	if(!wait_valid) {
		/* The active record never read stably: say so, whatever the
		 * completed record holds. */
		out->uncertain = 1;
		return true;
	}
	if(!completed_valid) return true; /* Explicitly uncertain. */
	return out->wait_us >= THREAD_STRIPE_WAIT_THRESHOLD_US;
}

static bool stripe_mutex_trylock(struct thread_mutex* mutex) {
#ifdef PLATFORM_WII
	bool locked = LWP_MutexTryLock(mutex->native) == 0;
#else
	bool locked = pthread_mutex_trylock(&mutex->native) == 0;
#endif
	if(locked) {
		/* ci-1101/37: an uncontended acquire is still a real hold; record the
		 * owner here so a later drain wait on this mutex can name it. */
		mutex->owner = stripe_thread_id();
		lease_lock_held();
		main_hold(true);
	}
	return locked;
}
static void thread_mutex_unlock_native(struct thread_mutex* mutex);

void thread_stripe_lock(struct thread_mutex* mutex) {
	assert(mutex && mutex->initialized);
	unsigned i = stripe_index(mutex);
	if(i >= 8) { thread_mutex_lock(mutex); return; }
	uint32_t id = stripe_thread_id();
	uint32_t tag = stripe_role(id);
	struct stripe_record* record = &s_stripes[i].role[tag];
	stripe_record_set(record, id, THREAD_STRIPE_ACQUIRING);
	if(!stripe_mutex_trylock(mutex)) {
		uint32_t start = 0;
		struct thread_stripe_breadcrumb prior = {.stripe = i};
		if(tag == THREAD_STRIPE_MAIN) {
			stripe_holder_get(i, &prior);
			start = stripe_now_us();
			stripe_wait_set(i + 1, start);
		}
		thread_mutex_lock(mutex);
		if(tag == THREAD_STRIPE_MAIN) {
			uint32_t waited = stripe_now_us() - start;
			stripe_wait_set(0, 0);
			if(waited >= THREAD_STRIPE_WAIT_THRESHOLD_US) {
				prior.wait_us = waited;
				stripe_completed_set(&prior);
			}
		}
	}
	stripe_test_event(THREAD_STRIPE_TEST_ACQUIRED);
	stripe_record_set(record, id, THREAD_STRIPE_HELD);
}

void thread_stripe_unlock(struct thread_mutex* mutex) {
	unsigned i = stripe_index(mutex);
	if(i < 8) {
		uint32_t id = stripe_thread_id();
		struct stripe_record* record = &s_stripes[i].role[stripe_role(id)];
		stripe_record_set(record, id, THREAD_STRIPE_RELEASING);
		stripe_test_event(THREAD_STRIPE_TEST_RELEASING);
		thread_mutex_unlock_native(mutex);
		stripe_test_event(THREAD_STRIPE_TEST_UNLOCKED);
		stripe_record_set(record, 0, THREAD_STRIPE_IDLE);
		main_hold(false);
		g9_lease_dependency_done_impl(); /* lease only: ACK sleep only after IDLE. */
		return;
	}
	thread_mutex_unlock(mutex);
}

// Times a block=true tchannel_send found its channel FULL. Every blocking send
// site is proven never-full by token conservation (see thread.h), so any
// non-zero value means a future change broke a channel's token accounting.
// Not an assert: the Wii build ships NDEBUG, and the gfx fb-ring precedent
// showed a counted-never-asserted diagnostic is what actually surfaces in a
// hang report.
//
// CR-08 (2026-08-20): plain (not volatile) uint32_t, incremented with
// __atomic_fetch_add and read with __atomic_load_n. tchannel_send can run on
// ANY of the app's threads (main, server, mesher worker, the remote-play
// connector) so "increments race-tolerantly" was never actually true for
// `x++` -- a plain read-modify-write can lose an increment when two threads
// hit it at once, undercounting a real regression. thread_create is called
// from fewer sites (mostly the main thread at startup) but is not guaranteed
// single-threaded either, so the same treatment applies. Both getters are
// also read from the Wii watchdog thread's hang-report dump (wii/watchdog.c),
// including from the panic/fault path -- __atomic_load_n on an aligned
// uint32_t lowers to a single ordinary load on PPC32 (no OS lock, nothing
// that can itself wedge), so this is still safe to read from there.
static uint32_t tchannel_full_events = 0;
static uint32_t thread_create_failures = 0;

uint32_t tchannel_full_count(void) {
	return __atomic_load_n(&tchannel_full_events, __ATOMIC_RELAXED);
}

uint32_t thread_create_failure_count(void) {
	return __atomic_load_n(&thread_create_failures, __ATOMIC_RELAXED);
}

#ifdef PLATFORM_WII
#include <malloc.h>
#include <ogc/video.h>
// devkitPPC's newlib hides usleep behind _DEFAULT_SOURCE / _BSD_SOURCE
// when the Wii Makefile builds with -std=c99 -pedantic. Forward-declare
// it explicitly so we don't have to thread a feature-test macro into
// every translation unit that pulls in this header chain.
#include <sys/types.h>
int usleep(useconds_t __useconds);
#endif
#ifdef PLATFORM_PC

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

size_t thread_stack_lowwater_bytes(const struct thread* t) {
	(void)t;
	return 0;
}

bool thread_saved_context_snapshot(const struct thread* t,
							   struct thread_saved_context* out) {
	(void)t;
	if(out) *out = (struct thread_saved_context) {0};
	return false;
}

bool thread_main_context_register(void) {
	return false;
}

bool thread_main_context_snapshot(struct thread_saved_context* out) {
	if(out) *out = (struct thread_saved_context) {0};
	return false;
}

bool thread_create(struct thread* t, void* (*entry)(void* arg), void* arg,
				   uint8_t priority) {
	assert(t && entry);
	if(!t || !entry)
		return false;
	t->created = false;
	int result = pthread_create(&t->native, NULL, entry, arg);
	if(result != 0) {
		__atomic_fetch_add(&thread_create_failures, 1, __ATOMIC_RELAXED);
		fprintf(stderr, "[thread] pthread_create failed: %d\n", result);
		return false;
	}
	t->created = true;
	return true;
}

void thread_join(struct thread* t) {
	assert(t);
	if(t && t->created) {
		pthread_join(t->native, NULL);
		uintptr_t native = (uintptr_t)t->native;
		uint32_t id = (uint32_t)(native ^ (native >> 32));
		g9_thread_joined(id ? id : UINT32_MAX);
		t->created = false;
	}
}

void thread_set_priority(struct thread* t, uint8_t priority) {
	// See the declaration comment in thread.h: no portable, dependency-free
	// priority knob is worth reaching for on PC's preemptive OS scheduler.
	(void)t;
	(void)priority;
}

static void thread_msleep_native(size_t ms) {
	usleep(ms * 1000);
}

void thread_yield(void) {
	// PC has real cores + a preemptive OS scheduler, so the server thread never
	// freezes render the way the single-core Wii does. The cascade-pacing yields are
	// a harmless no-op here (kept as a call so the shared server code compiles).
}

bool tchannel_init(struct thread_channel* c, size_t count) {
	assert(c && count > 0);
	if(!c || count == 0)
		return false;

	c->initialized = false;
	c->count = 0;
	c->length = count;
	c->data = malloc(c->length * sizeof(void*));
	if(!c->data)
		return false;
	if(pthread_mutex_init(&c->lock, NULL) != 0) {
		free(c->data);
		c->data = NULL;
		return false;
	}
	if(pthread_cond_init(&c->signal, NULL) != 0) {
		pthread_mutex_destroy(&c->lock);
		free(c->data);
		c->data = NULL;
		return false;
	}
	c->initialized = true;
	return true;
}

void tchannel_close(struct thread_channel* c) {
	assert(c);
	if(!c || !c->initialized)
		return;
	free(c->data);
	c->data = NULL;
	pthread_cond_destroy(&c->signal);
	pthread_mutex_destroy(&c->lock);
	c->initialized = false;
}

static bool tchannel_receive_impl(struct thread_channel* c, void** msg, bool block) {
	assert(c && msg);
	if(!c || !c->initialized || !msg)
		return false;

	bool waiting = false;
	if(s_lease.enabled && pthread_mutex_trylock(&c->lock) == 0) lease_lock_held();
	else { g9_lease_dependency_impl(); pthread_mutex_lock(&c->lock); } /* hold, not a tick wait */

	if(block) {
		while(!c->count) {
			if(!waiting) { thread_g9_lease_dependency(); waiting = true; }
			if(c == s_g9_rpc_pool) g9_probe_rpc(1, NULL);
			tps_note(thread_tps_observe, TPS_ENTER, TPS_CHANNEL_WAIT);
			pthread_cond_wait(&c->signal, &c->lock);
			tps_note(thread_tps_observe, TPS_LEAVE, TPS_CHANNEL_WAIT);
		}
	} else {
		if(!c->count) {
			if(c == s_g9_rpc_pool) g9_probe_rpc(1, NULL);
			pthread_mutex_unlock(&c->lock);
			g9_lease_dependency_done_impl();
			return false;
		}
	}

	*msg = c->data[0];
	c->count--;
	if(c == s_g9_rpc_pool) g9_probe_rpc(3, *msg);

	memmove(c->data, c->data + 1, c->count * sizeof(void*));

	pthread_cond_signal(&c->signal);
	pthread_mutex_unlock(&c->lock);
	if(waiting) thread_g9_lease_dependency_done();
	g9_lease_dependency_done_impl(); /* pairs with the lock acquisition */
	return true;
}

static bool tchannel_send_impl(struct thread_channel* c, void* msg, bool block) {
	assert(c && msg);
	if(!c || !c->initialized || !msg)
		return false;

	bool waiting = false;
	if(s_lease.enabled && pthread_mutex_trylock(&c->lock) == 0) lease_lock_held();
	else { g9_lease_dependency_impl(); pthread_mutex_lock(&c->lock); } /* hold, not a tick wait */

	if(block) {
		// Full on a blocking send = token conservation broken somewhere (see
		// thread.h). PC semantics unchanged -- we still wait -- but the event
		// is counted so the test suite / debug overlay can catch the
		// regression that the wait would otherwise hide.
		if(c->count >= c->length)
			__atomic_fetch_add(&tchannel_full_events, 1, __ATOMIC_RELAXED);
		while(c->count >= c->length) {
			if(!waiting) { thread_g9_lease_dependency(); waiting = true; }
			tps_note(thread_tps_observe, TPS_ENTER, TPS_CHANNEL_WAIT);
			pthread_cond_wait(&c->signal, &c->lock);
			tps_note(thread_tps_observe, TPS_LEAVE, TPS_CHANNEL_WAIT);
		}
	} else {
		if(c->count >= c->length) {
			pthread_mutex_unlock(&c->lock);
			g9_lease_dependency_done_impl();
			return false;
		}
	}

	c->data[c->count++] = msg;
	if(c == s_g9_rpc_pool) g9_probe_rpc(2, msg);

	pthread_cond_signal(&c->signal);
	pthread_mutex_unlock(&c->lock);
	if(waiting) thread_g9_lease_dependency_done();
	g9_lease_dependency_done_impl(); /* pairs with the lock acquisition */
	return true;
}

bool thread_spawn_disposable(struct thread* t, void* (*entry)(void* arg),
							 void* arg, uint8_t priority) {
	assert(t && entry);
	if(!t || !entry)
		return false;
	(void)priority;
	t->created = false;
	pthread_t native;
	if(pthread_create(&native, NULL, entry, arg) != 0) {
		__atomic_fetch_add(&thread_create_failures, 1, __ATOMIC_RELAXED);
		fprintf(stderr, "[thread] disposable pthread_create failed\n");
		return false;
	}
	// Detached: the OS reaps it whether it completes or gets abandoned.
	pthread_detach(native);
	return true;
}

void thread_dispose(struct thread* t, bool completed) {
	// Detached on PC -- nothing to reclaim either way.
	(void)t;
	(void)completed;
}

bool thread_mutex_init(struct thread_mutex* m) {
	assert(m);
	if(!m)
		return false;
	m->initialized = pthread_mutex_init(&m->native, NULL) == 0;
	m->owner = 0;
	return m->initialized;
}

void thread_mutex_destroy(struct thread_mutex* m) {
	assert(m);
	if(m && m->initialized) {
		pthread_mutex_destroy(&m->native);
		m->initialized = false;
		m->owner = 0;
	}
}

void thread_mutex_lock(struct thread_mutex* m) {
	assert(m && m->initialized);
	if(s_lease.enabled && m && m->initialized && stripe_mutex_trylock(m)) return;
	thread_g9_lease_dependency();
	if(m && m->initialized) {
		tps_note(thread_tps_observe, TPS_ENTER, TPS_MUTEX_WAIT);
		bool armed = drain_owner();
		ptime_t start;
		if(armed) start = time_get();
		/* ci-1101/37: sample the holder BEFORE blocking; once we hold the
		 * lock the owner field is ours, so a post-wait read would name US. */
		uint32_t holder = m->owner;
		pthread_mutex_lock(&m->native);
		m->owner = stripe_thread_id();
		if(armed) {
			uint32_t waited = (uint32_t)time_diff_us(start, time_get());
			s_drain_waits.lock_us += waited;
			drain_wait_record(waited, m, holder);
		}
		tps_note(thread_tps_observe, TPS_LEAVE, TPS_MUTEX_WAIT);
	}
	/* The contended wait is over; the lease keeps its depth for the hold
	 * (released by thread_mutex_unlock), the tick wait depth does not. */
	tick_wait_leave();
	main_hold(true);
}

static void thread_mutex_unlock_native(struct thread_mutex* m) {
	assert(m && m->initialized);
	if(m && m->initialized) {
		m->owner = 0; /* release clears the owner; the next waiter names us until here */
		pthread_mutex_unlock(&m->native);
	}
}

bool thread_cond_init(struct thread_cond* c) {
	assert(c);
	if(!c)
		return false;
	c->initialized = pthread_cond_init(&c->native, NULL) == 0;
	return c->initialized;
}

void thread_cond_destroy(struct thread_cond* c) {
	assert(c);
	if(c && c->initialized) {
		pthread_cond_destroy(&c->native);
		c->initialized = false;
	}
}

void thread_cond_wait(struct thread_cond* c, struct thread_mutex* m) {
	assert(c && c->initialized && m && m->initialized);
	if(c && c->initialized && m && m->initialized) {
		thread_g9_lease_dependency();
		pthread_cond_wait(&c->native, &m->native);
		thread_g9_lease_dependency_done();
	}
}

bool thread_cond_timedwait(struct thread_cond* c, struct thread_mutex* m,
						   uint32_t timeout_ms) {
	assert(c && c->initialized && m && m->initialized);
	if(!c || !c->initialized || !m || !m->initialized)
		return false;
	// pthread's deadline is ABSOLUTE, on CLOCK_REALTIME for a default-attr
	// condvar (see the thread.h declaration for why the two platforms differ).
	struct timespec deadline;
	if(clock_gettime(CLOCK_REALTIME, &deadline) != 0)
		return false;
	deadline.tv_sec += (time_t)(timeout_ms / 1000u);
	deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
	if(deadline.tv_nsec >= 1000000000L) {
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec += 1;
	}
	thread_g9_lease_dependency();
	bool signalled = pthread_cond_timedwait(&c->native, &m->native, &deadline) == 0;
	thread_g9_lease_dependency_done();
	return signalled;
}

void thread_cond_signal(struct thread_cond* c) {
	assert(c && c->initialized);
	if(c && c->initialized)
		pthread_cond_signal(&c->native);
}

void thread_cond_broadcast(struct thread_cond* c) {
	assert(c && c->initialized);
	if(c && c->initialized)
		pthread_cond_broadcast(&c->native);
}

#endif

#ifdef PLATFORM_WII

// libogc convention (ogc/lwp.h):
//   LWP_PRIO_IDLE     = 0   (lowest)
//   LWP_PRIO_HIGHEST  = 127 (highest)
// i.e. *higher number = higher scheduler priority*. The default main
// thread runs at ~LWP_PRIO_NORMAL (64). The previous mesher (priority 4)
// and server (priority 8) values both sit just above idle, which makes
// them prone to being starved whenever the main render thread isn't
// strictly blocked. We pass priorities through from the caller; see
// the call sites in chunk_mesher.c and server_local.c for the chosen
// values.
//
// Stack: libogc's default LWP stack is 8 KB, which is too tight for
// the server world tick (lighting flood + scheduled-tick processing)
// and the chunk mesher (recursive BFS + per-job arena prep). We
// pre-allocate a 64 KB MEM2 stack per thread via memalign so the
// scheduler doesn't have to hand them out from internal pools.
#define THREAD_STACK_SIZE (64 * 1024)
#define THREAD_STACK_ALIGN 32
#define THREAD_STACK_PAINT UINT32_C(0x5AFE5AFE)

/* LWP thread records are newlib malloc objects and our owned stacks are
 * memalign objects in cached Broadway RAM. Reject a corrupted handle/stack
 * before dereferencing it; uncached aliases and MMIO are not diagnostic
 * targets. These are the Wii MEM1 and MEM2 cached virtual ranges. */
static bool thread_cached_ram_range(uintptr_t addr, size_t bytes) {
	static const struct { uintptr_t lo, hi; } ranges[] = {
		{UINT32_C(0x80000000), UINT32_C(0x81800000)},
		{UINT32_C(0x90000000), UINT32_C(0x94000000)},
	};
	for(size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
		if(addr >= ranges[i].lo && addr < ranges[i].hi
		   && bytes <= ranges[i].hi - addr)
			return true;
	return false;
}

size_t thread_stack_lowwater_bytes(const struct thread* t) {
	if(!t || !t->created || !t->stack) return 0;
	const uint32_t* words = (const uint32_t*)t->stack;
	for(size_t i = 0; i < THREAD_STACK_SIZE / sizeof(uint32_t); i++)
		if(__atomic_load_n(&words[i], __ATOMIC_RELAXED)
		   != THREAD_STACK_PAINT)
			return i * sizeof(uint32_t);
	return THREAD_STACK_SIZE;
}

bool thread_saved_context_snapshot_raw(lwp_t native, const void* stack,
									  size_t stack_bytes,
									  struct thread_saved_context* out) {
	if(!out) return false;
	*out = (struct thread_saved_context) {0};
	if(!stack || native == LWP_THREAD_NULL || native == 0
	   || stack_bytes < 2u * sizeof(uint32_t))
		return false;

	const uintptr_t low = (uintptr_t)stack;
	if((low & 7u) != 0 || (stack_bytes & 7u) != 0
	   || !thread_cached_ram_range(low, stack_bytes))
		return false;
	const uintptr_t high = low + stack_bytes;
	/* Check the complement-handle convention against our own live LWP
	 * before dereferencing the target's derived pointer. */
	if((KThread*)(uintptr_t)~(uint32_t)LWP_GetSelf() != KThreadGetSelf())
		return false;
	KThread* const kt = (KThread*)(uintptr_t)~(uint32_t)native;
	if(((uintptr_t)kt & (sizeof(uint32_t) - 1u)) != 0
	   || !thread_cached_ram_range((uintptr_t)kt, sizeof(*kt)))
		return false;

	/* The caller executes on Broadway, so this other thread has a saved
	 * context. Mask IRQs only for these scalar reads and <=24 validated PPC
	 * stack-link steps; never format, allocate or acquire a lock here. */
	PPCIrqState irq = PPCIrqLockByMsr();
	if(kt == KThreadGetSelf() || kt->state < KTHR_STATE_RUNNING
	   || kt->state > KTHR_STATE_WAITING_MUTEX) {
		PPCIrqUnlockByMsr(irq);
		return false;
	}
	out->pc = kt->ctx.pc;
	out->lr = kt->ctx.lr;
	out->sp = kt->ctx.gpr[1];
	out->state = kt->state;
	out->suspend = kt->suspend;
	out->priority = kt->prio;
	if(kt->state == KTHR_STATE_WAITING
	   || kt->state == KTHR_STATE_WAITING_MUTEX) {
		out->wait_queue = (uint32_t)(uintptr_t)kt->wait.queue;
		out->wait_token = (uint32_t)kt->wait.token;
	}
	out->valid = 1;

	/* PPC EABI stores the caller's LR at parent-SP+4. Read the back-link
	 * at current SP only after checking the owned stack range; each parent
	 * must be aligned and strictly higher in this downward-growing stack.
	 * An invalid link ends the walk without touching the proposed address. */
	uintptr_t frame = (uintptr_t)out->sp;
	for(unsigned i = 0; i < THREAD_SAVED_CONTEXT_CALLERS; i++) {
		if(frame < low || frame > high - sizeof(uint32_t)
		   || (frame & 7u) != 0)
			break;
		uintptr_t parent = (uintptr_t)*(const uint32_t*)frame;
		if(parent <= frame || parent < low || parent > high - 2u * sizeof(uint32_t)
		   || (parent & 7u) != 0)
			break;
		uint32_t ret = *(const uint32_t*)(parent + sizeof(uint32_t));
		if(!ret || (ret & 3u) != 0) break;
		out->callers[out->caller_count++] = ret;
		frame = parent;
	}
	PPCIrqUnlockByMsr(irq);
	return true;
}

bool thread_saved_context_snapshot(const struct thread* t,
							   struct thread_saved_context* out) {
	if(!t || !t->created) {
		if(out) *out = (struct thread_saved_context) {0};
		return false;
	}
	return thread_saved_context_snapshot_raw(t->native, t->stack,
		THREAD_STACK_SIZE, out);
}

/* R6 diagnostic, pinned to linked logc256kr tuxedo/default_stacks.c: the
 * startup loads __ppc_main_sp, which points past static s_mainStack[0x20000].
 * There is no __stack_addr in this SDK. Verify s_mainStack's ELF size and
 * __ppc_main_sp's stored pointer on each diagnostic release; an SDK stack
 * override invalidates this contract. Linker BSS bounds and current SP are
 * checked here before accepting the main thread's process-lifetime handle. */
extern void* __ppc_main_sp;
extern char __bss_start[], __bss_end[];
#define THREAD_MAIN_STACK_SIZE UINT32_C(0x20000)
static lwp_t s_main_context_native;
static const void* s_main_context_stack;
static bool s_main_context_registered;

bool thread_main_context_register(void) {
	if(__atomic_load_n(&s_main_context_registered, __ATOMIC_ACQUIRE))
		return LWP_GetSelf() == s_main_context_native;
	const uintptr_t high = (uintptr_t)__ppc_main_sp;
	if(high < THREAD_MAIN_STACK_SIZE || (high & 7u) != 0)
		return false;
	const uintptr_t low = high - THREAD_MAIN_STACK_SIZE;
	uintptr_t sp;
	__asm__ volatile("mr %0,1" : "=r"(sp));
	if(!thread_cached_ram_range(low, THREAD_MAIN_STACK_SIZE)
	   || low < (uintptr_t)__bss_start || high > (uintptr_t)__bss_end
	   || (sp & 7u) != 0 || sp < low || sp > high - 2u * sizeof(uint32_t))
		return false;
	lwp_t native = LWP_GetSelf();
	if(!native || native == LWP_THREAD_NULL
	   || (KThread*)(uintptr_t)~(uint32_t)native != KThreadGetSelf())
		return false;
	s_main_context_native = native;
	s_main_context_stack = (const void*)low;
	__atomic_store_n(&s_main_context_registered, true, __ATOMIC_RELEASE);
	return true;
}

bool thread_main_context_snapshot(struct thread_saved_context* out) {
	if(!__atomic_load_n(&s_main_context_registered, __ATOMIC_ACQUIRE)) {
		if(out) *out = (struct thread_saved_context) {0};
		return false;
	}
	return thread_saved_context_snapshot_raw(s_main_context_native,
		s_main_context_stack, THREAD_MAIN_STACK_SIZE, out);
}

bool thread_create(struct thread* t, void* (*entry)(void* arg), void* arg,
				   uint8_t priority) {
	assert(t && entry);
	if(!t || !entry)
		return false;
	t->created = false;
	t->native = LWP_THREAD_NULL;
	t->stack = memalign(THREAD_STACK_ALIGN, THREAD_STACK_SIZE);
	if(!t->stack) {
		__atomic_fetch_add(&thread_create_failures, 1, __ATOMIC_RELAXED);
		fprintf(stderr, "[thread] 64 KiB stack allocation failed\n");
		return false;
	}
	/* Temporary stall probe: paint before publication to LWP, then sample the
	 * downward-growing stack from the main thread at heartbeat cadence. */
	for(size_t i = 0; i < THREAD_STACK_SIZE / sizeof(uint32_t); i++)
		((uint32_t*)t->stack)[i] = THREAD_STACK_PAINT;
	s32 result = LWP_CreateThread(&t->native, entry, arg, t->stack,
							  THREAD_STACK_SIZE, priority);
	if(result != 0) {
		fprintf(stderr, "[thread] LWP_CreateThread failed: %ld\n", (long)result);
		free(t->stack);
		t->stack = NULL;
		t->native = LWP_THREAD_NULL;
		__atomic_fetch_add(&thread_create_failures, 1, __ATOMIC_RELAXED);
		return false;
	}
	t->created = true;
	return true;
}

void thread_join(struct thread* t) {
	assert(t);

	if(!t || !t->created)
		return;
	void* unused;
	LWP_JoinThread(t->native, &unused);
	g9_thread_joined((uint32_t)t->native);
	if(t->stack) {
		free(t->stack);
		t->stack = NULL;
	}
	t->native = LWP_THREAD_NULL;
	t->created = false;
}

void thread_set_priority(struct thread* t, uint8_t priority) {
	assert(t);
	if(!t || t->native == LWP_THREAD_NULL)
		return;
	// void return, cannot fail -- see the thread.h declaration comment for
	// why libogc's higher-number-is-higher-priority convention lets a live
	// thread (including this one calling on itself) be re-prioritized
	// in-place with no create/join dance.
	LWP_SetThreadPriority(t->native, priority);
}

static void thread_msleep_native(size_t ms) {
	// Previously: a loop of VIDEO_WaitVSync(). That coupled every
	// sleeping thread to the video subsystem's frame rate (~60 Hz) and
	// could starve the render thread by piling up vsync waiters.
	// usleep is a normal nanosleep-equivalent via newlib + libogc's
	// time subsystem and lets the scheduler pick up another runnable
	// thread immediately.
	usleep(ms * 1000);
}

void thread_yield(void) {
	// Move this thread to the back of its priority's ready queue so an EQUAL-priority peer gets a
	// turn, then resume. The server thread (63) is below main/render (64), which preempts it
	// anyway; this is pacing among equal-priority threads, not render protection.
	LWP_YieldThread();
}

bool tchannel_init(struct thread_channel* c, size_t count) {
	assert(c && count > 0);
	if(!c || count == 0)
		return false;
	c->initialized = false;
	c->native = 0;
	s32 result = MQ_Init(&c->native, count);
	if(result != 0) {
		fprintf(stderr, "[thread] MQ_Init(%u) failed: %ld\n", (unsigned)count,
				(long)result);
		return false;
	}
	c->initialized = true;
	return true;
}

void tchannel_close(struct thread_channel* c) {
	assert(c);
	if(c && c->initialized) {
		MQ_Close(c->native);
		c->initialized = false;
		c->native = 0;
	}
}

static bool tchannel_receive_impl(struct thread_channel* c, void** msg, bool block) {
	if(!c || !c->initialized || !msg)
		return false;
	if(c == s_g9_rpc_pool && g9_probe_enabled() && !block) {
		g9_lock_t lock = g9_lock();
		bool got = MQ_Receive(c->native, msg, MQ_MSG_NOBLOCK);
		if(got) g9_probe_rpc(3, *msg);
		else g9_probe_rpc(1, NULL);
		g9_unlock(lock);
		return got;
	}
	/* The tick owner's blocking receive is a wait MAIN must not donate into
	 * (PC: the condition wait in tchannel_receive_impl). */
	if(block && (thread_g9_lease_active_owner() || tick_owner_self())) {
		if(MQ_Receive(c->native, msg, MQ_MSG_NOBLOCK)) return true;
		thread_g9_lease_dependency();
		bool got = MQ_Receive(c->native, msg, MQ_MSG_BLOCK);
		thread_g9_lease_dependency_done();
		return got;
	}
	return MQ_Receive(c->native, msg, block ? MQ_MSG_BLOCK : MQ_MSG_NOBLOCK);
}

static bool g9_mq_send(struct thread_channel* c, void* msg, uint32_t flags) {
	if(c != s_g9_rpc_pool || !g9_probe_enabled())
		return MQ_Send(c->native, msg, flags);
	/* The installed SDK uses TrySend for both flags. Keep publication and
	 * its timestamp in one IRQ critical section, before a woken waiter can
	 * reacquire. No sleeping/blocking operation is added here. */
	g9_lock_t lock = g9_lock();
	bool sent = MQ_Send(c->native, msg, flags);
	if(sent) g9_probe_rpc(2, msg);
	g9_unlock(lock);
	return sent;
}

static bool tchannel_send_impl(struct thread_channel* c, void* msg, bool block) {
	if(!c || !c->initialized || !msg)
		return false;
	if(!block)
		return g9_mq_send(c, msg, MQ_MSG_NOBLOCK);

	// libogc v3.0.4 (tuxedo) DELETED blocking MQ_Send: lwpcompat/lwpc_mq.c
	// forces every send through KMailboxTrySend (an `if (true)` ORs in
	// MQ_MSG_NOBLOCK; the blocking KMailboxSend is commented out), so on a
	// full queue the message is silently DROPPED -- the verified root cause
	// of the framebuffer-flip hang (see platform/wii/gfx.c). The ~14 blocking
	// send sites in the tree were written against the old "blocking send
	// always succeeds" contract, so emulate it here: retry until the consumer
	// drains a slot. KMailboxTrySend fails ONLY when genuinely full (one
	// IRQ-locked compare in tuxedo/mailbox.c), and every blocking site is
	// proven never-full by token conservation (thread.h), so this loop is
	// expected to never iterate; if it ever does, the counter makes the
	// conservation regression loud instead of losing the message. usleep --
	// not LWP_YieldThread -- so a LOWER-priority consumer can run too.
	if(g9_mq_send(c, msg, MQ_MSG_BLOCK))
		return true;

	// CR-08 also binds this Wii arm: tchannel_send can run on any of the
	// app's threads, so a plain `x++` here can lose an increment the same
	// way the PC arm's comment at line ~183 describes. Match it exactly.
	__atomic_fetch_add(&tchannel_full_events, 1, __ATOMIC_RELAXED);
	thread_g9_lease_dependency();
	do {
		usleep(1000);
	} while(!g9_mq_send(c, msg, MQ_MSG_BLOCK));
	thread_g9_lease_dependency_done();
	return true;
}

bool thread_spawn_disposable(struct thread* t, void* (*entry)(void* arg),
							 void* arg, uint8_t priority) {
	// Same creation shape as thread_create; the difference is the teardown
	// contract (thread_dispose below) -- the caller may abandon the handle.
	return thread_create(t, entry, arg, priority);
}

void thread_dispose(struct thread* t, bool completed) {
	assert(t);
	if(!t || !t->created)
		return;
	if(completed) {
		// The thread has finished its work; LWP_JoinThread returns promptly
		// and the 64 KB stack is reclaimed.
		thread_join(t);
		return;
	}
	// Abandoned in flight (a connect stuck in IOS DNS/handshake): joining
	// would recreate the very hang the disposable pattern exists to avoid.
	// Leak the LWP bookkeeping + stack ON PURPOSE -- documented in thread.h,
	// bounded by user connect attempts, mirrors PC's stuck detached thread.
	t->created = false;
	t->stack = NULL;
	t->native = LWP_THREAD_NULL;
}

bool thread_mutex_init(struct thread_mutex* m) {
	assert(m);
	if(!m)
		return false;
	m->native = LWP_MUTEX_NULL;
	m->initialized = LWP_MutexInit(&m->native, false) == 0;
	m->owner = 0;
	return m->initialized;
}

void thread_mutex_destroy(struct thread_mutex* m) {
	assert(m);
	if(m && m->initialized) {
		LWP_MutexDestroy(m->native);
		m->native = LWP_MUTEX_NULL;
		m->initialized = false;
		m->owner = 0;
	}
}

void thread_mutex_lock(struct thread_mutex* m) {
	assert(m && m->initialized);
	if(s_lease.enabled && m && m->initialized && stripe_mutex_trylock(m)) return;
	thread_g9_lease_dependency();
	if(m && m->initialized) {
		bool armed = drain_owner();
		ptime_t start;
		if(armed) start = time_get();
		/* ci-1101/37: sample the holder BEFORE blocking; see the PC arm. */
		uint32_t holder = m->owner;
		LWP_MutexLock(m->native);
		m->owner = stripe_thread_id();
		if(armed) {
			uint32_t waited = (uint32_t)time_diff_us(start, time_get());
			s_drain_waits.lock_us += waited;
			drain_wait_record(waited, m, holder);
		}
	}
	/* As the PC arm: the wait is over, the lease depth stays for the hold. */
	tick_wait_leave();
	main_hold(true);
}

static void thread_mutex_unlock_native(struct thread_mutex* m) {
	assert(m && m->initialized);
	if(m && m->initialized) {
		m->owner = 0; /* release clears the owner; see the PC arm */
		LWP_MutexUnlock(m->native);
	}
}

bool thread_cond_init(struct thread_cond* c) {
	assert(c);
	if(!c)
		return false;
	c->native = LWP_COND_NULL;
	c->initialized = LWP_CondInit(&c->native) == 0;
	return c->initialized;
}

void thread_cond_destroy(struct thread_cond* c) {
	assert(c);
	if(c && c->initialized) {
		LWP_CondDestroy(c->native);
		c->native = LWP_COND_NULL;
		c->initialized = false;
	}
}

// S5.3 hunt PROBE (temporary, round 2): coordinator hypothesis -- this
// portable cond wrapper's Wii arm (LWP_CondWait/Signal/Broadcast) gets its
// FIRST genuinely-contended cross-thread fire in the exact death window
// (write thread parks waiting for the LOGIN frame; the read thread's
// handshake-reply decode signals it via process_event's queue_frame). Round
// 1 already proved the main loop alive for several frames post-swap with
// mp_status.state reaching REMOTE_HANDSHAKE_SENT and never advancing -- this
// traces whether the wait/signal pair around exactly that transition
// actually completes. One-shot, per-call-kind budget, tagged with
// LWP_GetSelf() so wait/signal/broadcast can be attributed to a thread.
// Separate SD file so it interleaves cleanly with cw_boot_trace/cw_mp_trace
// without competing for their budgets.
static void cw_cond_trace(const char* label, u32 who, s32 rc) {
	// ROUND 10 (Sol/codex static review, coordinator-escalated): NO-OP'd.
	// thread_cond_wait/signal/broadcast are GENERIC primitives -- every
	// thread in the process calls them for EVERY condvar, not just the MP
	// write_signal one this comment block was written about. That makes
	// this the SAME multi-writer-collision class round 8 fixed for the
	// boot/mp/wt trace files (main signals holding remote->lock on every
	// outbound queue_frame, the write thread waits/wakes concurrently,
	// exactly at the login-queue moment), just in a file the round-8/9 SD
	// audits never reached because it lives in thread.c, not the transport/
	// net/main layers. Decisive test (round 10 cycle 1): no-op this ALONE,
	// change nothing else, and see if mp_connect_smoke's wall moves.
	(void)label;
	(void)who;
	(void)rc;
}

void thread_cond_wait(struct thread_cond* c, struct thread_mutex* m) {
	assert(c && c->initialized && m && m->initialized);
	if(c && c->initialized && m && m->initialized) {
		static unsigned wait_budget = 5;
		bool trace_this = wait_budget > 0;
		if(trace_this) {
			wait_budget--;
			cw_cond_trace("wait_before", LWP_GetSelf(), 0);
		}
		thread_g9_lease_dependency();
		s32 rc = LWP_CondWait(c->native, m->native);
		thread_g9_lease_dependency_done();
		if(trace_this)
			cw_cond_trace("wait_after", LWP_GetSelf(), rc);
	}
}

bool thread_cond_timedwait(struct thread_cond* c, struct thread_mutex* m,
						   uint32_t timeout_ms) {
	assert(c && c->initialized && m && m->initialized);
	if(!c || !c->initialized || !m || !m->initialized)
		return false;
	// libogc's timespec here is RELATIVE, not an absolute deadline --
	// ogc/cond.h:92-100 declares the third parameter `reltime` and documents
	// it as "the relative time for the timeout", and LWP_CondTimedWait feeds
	// it straight to the watchdog tick conversion. No clock read is needed
	// (and none is wanted: gettime() on the sender's park path would be one
	// more timebase read per idle iteration).
	struct timespec reltime;
	reltime.tv_sec = (time_t)(timeout_ms / 1000u);
	reltime.tv_nsec = (long)(timeout_ms % 1000u) * 1000000L;
	// 0 = signalled; any non-zero (libogc returns a timeout/EINVAL code) is
	// reported as "not a signal". The caller re-checks its predicate anyway.
	thread_g9_lease_dependency();
	bool signalled = LWP_CondTimedWait(c->native, m->native, &reltime) == 0;
	thread_g9_lease_dependency_done();
	return signalled;
}

void thread_cond_signal(struct thread_cond* c) {
	assert(c && c->initialized);
	if(c && c->initialized) {
		static unsigned signal_budget = 5;
		bool trace_this = signal_budget > 0;
		if(trace_this) {
			signal_budget--;
			cw_cond_trace("signal_before", LWP_GetSelf(), 0);
		}
		s32 rc = LWP_CondSignal(c->native);
		if(trace_this)
			cw_cond_trace("signal_after", LWP_GetSelf(), rc);
	}
}

void thread_cond_broadcast(struct thread_cond* c) {
	assert(c && c->initialized);
	if(c && c->initialized) {
		static unsigned broadcast_budget = 3;
		bool trace_this = broadcast_budget > 0;
		if(trace_this) {
			broadcast_budget--;
			cw_cond_trace("broadcast_before", LWP_GetSelf(), 0);
		}
		s32 rc = LWP_CondBroadcast(c->native);
		if(trace_this)
			cw_cond_trace("broadcast_after", LWP_GetSelf(), rc);
	}
}

#endif

void thread_mutex_unlock(struct thread_mutex* m) {
	thread_mutex_unlock_native(m);
	main_hold(false);
	g9_lease_dependency_done_impl(); /* lease hold only; see thread_mutex_lock */
}

bool thread_mutex_try_guard(struct thread_mutex* m, struct thread_mutex_guard* guard) {
	guard->held = false;
	if(!m || !m->initialized) return false;
#ifdef PLATFORM_WII
	guard->irq = PPCIrqLockByMsr();
	if(KThreadGetSelf()->waiters.next || LWP_MutexTryLock(m->native) != 0) {
		PPCIrqUnlockByMsr(guard->irq);
		return false;
	}
#else
	if(pthread_mutex_trylock(&m->native) != 0) return false;
#endif
	guard->held = true;
	m->owner = stripe_thread_id(); /* ci-1101/37: guard acquire is a hold too */
	main_hold(true);
	return true;
}

void thread_mutex_release_guard(struct thread_mutex* m, struct thread_mutex_guard* guard) {
	assert(guard->held);
	thread_mutex_unlock_native(m);
	guard->held = false;
	main_hold(false);
#ifdef PLATFORM_WII
	PPCIrqUnlockByMsr(guard->irq);
#endif
}

void thread_msleep(size_t ms) {
	if(s_lease.enabled) {
		g9_lock_t lock = g9_lock();
		if(lease_owner()) s_lease.stats.sleep_requested_us += (uint32_t)(ms * 1000u);
		if(stripe_thread_id() == s_lease_main)
			s_lease.stats.main_sleep_requested_us += (uint32_t)(ms * 1000u);
		g9_unlock(lock);
	}
	bool owner = thread_g9_lease_active_owner();
	/* A sleeping tick owner is not runnable: MAIN must not donate to it. */
	bool tick = !owner && tick_owner_self();
	if(owner) thread_g9_lease_dependency();
	else if(tick) tick_wait_enter();
	thread_msleep_native(ms);
	if(owner) thread_g9_lease_dependency_done();
	else if(tick) tick_wait_leave();
}

/* Native arms drop only on contention or an actual MQ/condition wait. */
bool tchannel_receive(struct thread_channel* c, void** msg, bool block) {
	return tchannel_receive_impl(c, msg, block);
}
bool tchannel_send(struct thread_channel* c, void* msg, bool block) {
	return tchannel_send_impl(c, msg, block);
}
