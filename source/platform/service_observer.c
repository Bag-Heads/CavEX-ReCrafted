/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#define SERVICE_OBSERVER_IMPLEMENTATION
#include "service_observer.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "input_owner.h"
#include "../parson/parson.h"
#ifdef PLATFORM_WII
#include <ogc/lwp.h>
#else
#include <pthread.h>
#endif

/* One writer per metric and ring: MAIN (0), SERVER (1), cap IRQ (2), VI (3).
 * Metric payloads use 32-bit atomic words under single-writer seqlocks: a
 * reader tries ONCE per pump call, defers contention without advancing its
 * cursor, and never stalls a writer. Cumulative samples remain in storage.
 * Rings never overwrite unread context. Every routine observation still goes
 * into cumulative bins, including when bounded context storage overflows.
 * No retries, locks, allocations, formatting or IO in a producer hook. */
#define LOAD(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define MW (16u + SO_BINS)
struct so_metric_storage { uint32_t serial, w[MW]; };
struct so_ring { uint32_t head, tail, overflow, capacity; uint32_t (*row)[SO_CONTEXT_WORDS]; };
struct so_sample { uint64_t receipt; uint32_t sequence, connected, poll; };
struct so_present { uint32_t sequence, device, action; uint64_t receipt; };
struct so_fb { uintptr_t key; uint32_t frame; struct so_present input[2][9]; };
static struct {
	struct so_metric_storage metric[SO_METRICS];
	struct so_ring ring[SO_LANES];
	/* SERVER32 (formerly40): power-of-two wrap mapping and room for every
	 * owner/device's presentation under the unchanged 16 KiB ceiling. */
	uint32_t rows[72][SO_CONTEXT_WORDS]; /* MAIN16 / SERVER32 / cap16 / VI8 */
	uint32_t batch_metrics[3][MW], batch_context[2][SO_CONTEXT_WORDS];
	struct so_sample sample[9]; /* WPAD0..3, PAD0..3, PC receipt proxy */
	struct so_present consumed[2][9];
	uint32_t consumed_frame[2][9];
	struct so_fb fb[3]; /* Wii's three XFBs; published before fb_to_vi release */
	uint32_t shown[2][9]; /* VI single writer: first presentation only */
	uint32_t state_count[SO_STATES], state_us[SO_STATES];
	uint32_t input_count[9][6], audio_count[SO_AUDIO_EVENTS], audio_active_lo, audio_active_hi;
	uint32_t input_unassigned;
	uint32_t latest[10], state, frame, tick, active_tick, phase, rt, wt;
	uint32_t owner_lo, owner_hi, lost[SO_LANES], dropped, counter_overflow;
	uint32_t row_sequence, tick_boost_id, audio_active, audio_reprime;
	uint64_t origin, frame_at, poll_at, poll_end, audio_at, tick_at, phase_at, boost_at;
	uint64_t last_flush, state_at;
	char build[64], line[2304];
	uint32_t config_id;
	uint32_t request, served, final_request, finished;
	uint32_t pumping, pump_lane, pump_cursor, pump_final, pump_request, pump_footer;
	uint32_t pump_limit[SO_LANES];
	uint32_t pending, pending_lane, pending_tail, pending_cursor;
	uint32_t pending_done, pending_footer, pending_batch, transport_failed;
	uint32_t teardown_unobserved;
	uint64_t final_at;
	uint64_t last_request;
	uint32_t decode_underruns, component_worst, display_frame;
	uint32_t audio_inactive, audio_estimated_exhaustion;
	uint32_t latest_serial;
	uintptr_t main_owner;
	uint32_t unsupported_writer;
	uint32_t tick_boundary_serial, tick_boundary[3], final_tick_boundary[3];
	struct { const JSON_Value* value; size_t next; bool entered; } hash_stack[16];
} s;
uint32_t service_observer_on;
/* The pre-existing sender may already be idle at first configuration. Keep
 * its publication flag outside the reset payload so it never reads a field
 * concurrently cleared by memset; publish initialized storage with release. */
static uint32_t observer_configured;
static uint64_t (*test_clock)(void);
static uintptr_t native_owner(void);
_Static_assert(sizeof(s) + sizeof(test_clock) + sizeof(service_observer_on) + sizeof(observer_configured) <= 16384,
	"service observer exceeds its entire 16 KiB preallocated storage budget");
_Static_assert(__atomic_always_lock_free(4, 0), "observer needs lock-free 32-bit atomics");

size_t service_observer_storage_bytes(void) {
	return sizeof(s) + sizeof(test_clock) + sizeof(service_observer_on) + sizeof(observer_configured);
}
void service_observer_test_clock(uint64_t (*clock_us)(void)) { test_clock = clock_us; }
/* Quiescent harness adapter for a preempted publication. Never called by a
 * producer; unlike a skipped sample, a cumulative metric remains recoverable. */
void service_observer_test_metric_busy(unsigned m, bool busy) {
	if(m < SO_METRICS && (bool)(LOAD(&s.metric[m].serial) & 1u) != busy)
		__atomic_store_n(&s.metric[m].serial, LOAD(&s.metric[m].serial)+1, __ATOMIC_RELEASE);
}
static uint32_t hash_bytes(uint32_t h, const void* ptr, size_t n) {
	const unsigned char* b = ptr;
	while(n--) h = (h ^ *b++) * 16777619u;
	return h;
}
uint32_t service_observer_config_hash(const void* root) {
	/* Initialization only; no serialization allocation or recursive stack.
	 * Excess depth returns UNAVAILABLE (0), never a truncated checksum. The
	 * configured observer reports missing identity and cannot claim coverage. */
	if(!root) return 0;
	unsigned depth = 1; uint32_t h = 2166136261u;
	memset(s.hash_stack, 0, sizeof s.hash_stack); s.hash_stack[0].value = root;
	while(depth) {
		unsigned k = depth - 1; const JSON_Value* v = s.hash_stack[k].value;
		int type = json_value_get_type(v);
		if(!s.hash_stack[k].entered) {
			s.hash_stack[k].entered = true; h = hash_bytes(h, &type, sizeof type);
			if(type == JSONString) {
				const char* b = json_value_get_string(v); h = hash_bytes(h, b, json_value_get_string_len(v)+1);
			} else if(type == JSONNumber) {
				double d = json_value_get_number(v); h = hash_bytes(h, &d, sizeof d);
			} else if(type == JSONBoolean) {
				int b = json_value_get_boolean(v); h = hash_bytes(h, &b, sizeof b);
			}
		}
		const JSON_Value* child = NULL; size_t next = s.hash_stack[k].next++;
		if(type == JSONObject) {
			const JSON_Object* o = json_value_get_object(v);
			if(next < json_object_get_count(o)) {
				const char* name = json_object_get_name(o, next); h = hash_bytes(h, name, strlen(name)+1);
				child = json_object_get_value_at(o, next);
			}
		} else if(type == JSONArray) {
			const JSON_Array* a = json_value_get_array(v);
			if(next < json_array_get_count(a)) child = json_array_get_value(a, next);
		}
		if(!child) { depth--; continue; }
		if(depth == 16) return 0;
		s.hash_stack[depth].value = child; s.hash_stack[depth].next = 0;
		s.hash_stack[depth++].entered = false;
	}
	return h;
}
uint64_t service_observer_at(ptime_t t) { return time_diff_us((ptime_t){0}, t); }
uint64_t service_observer_now(void) { return test_clock ? test_clock() : service_observer_at(time_get()); }
bool service_observer_enabled(void) { return LOAD(&service_observer_on) != 0; }
void service_observer_configure(bool on, const char* build, uint32_t config_id) {
	/* Called only before producers start; OFF need not even read the clock. */
	STORE(&service_observer_on, 0);
	__atomic_store_n(&observer_configured, 0u, __ATOMIC_RELEASE);
	memset(&s, 0, sizeof s);
	if(!on) return;
	s.origin = s.state_at = service_observer_now();
	s.main_owner = native_owner();
	s.state_count[SO_STARTUP] = 1; /* real initial state entry, before the first loop frame */
	s.config_id = config_id;
	if(!config_id) s.lost[0] = 1;
	snprintf(s.build, sizeof s.build, "%s", build ? build : "unknown");
	for(unsigned m = 0; m < SO_METRICS; m++) s.metric[m].w[4] = UINT32_MAX;
	const unsigned capacities[4] = {16, 32, 16, 8}; unsigned offset = 0;
	for(unsigned k = 0; k < 4; k++) {
		s.ring[k].capacity = capacities[k]; s.ring[k].row = s.rows + offset;
		offset += capacities[k];
	}
	__atomic_store_n(&observer_configured, 1u, __ATOMIC_RELEASE);
	__atomic_store_n(&service_observer_on, 1u, __ATOMIC_RELEASE);
}
unsigned service_observer_bin(uint32_t us) {
	/* [0,1ms), [1,5), [5,10), [10,20), [20,50), [50,100), [100,250), >=250ms */
	return us < 1000 ? 0 : us < 5000 ? 1 : us < 10000 ? 2 : us < 20000 ? 3
		: us < 50000 ? 4 : us < 100000 ? 5 : us < 250000 ? 6 : 7;
}
static void add(uint32_t* p, uint32_t n) {
	/* Single-writer slots: load/store, no reservation/CAS retry in an IRQ. */
	uint32_t old = LOAD(p); STORE(p, old + n);
	if(n > UINT32_MAX - old) {
		STORE(&s.counter_overflow, 1u);
	}
}
static uint32_t span(uint64_t from, uint64_t to) {
	if(to < from || to - from > UINT32_MAX) {
		STORE(&s.counter_overflow, 1u);
		return UINT32_MAX;
	}
	return (uint32_t)(to - from);
}
static uintptr_t native_owner(void) {
#ifdef PLATFORM_WII
	return (uintptr_t)LWP_GetSelf();
#else
	return (uintptr_t)pthread_self();
#endif
}
static bool server_owner(void) {
	uintptr_t id = native_owner();
	return LOAD(&s.owner_lo) == (uint32_t)id
		&& LOAD(&s.owner_hi) == (uint32_t)((uint64_t)id >> 32);
}
static unsigned metric_lane(unsigned m) { return m == SO_CAP_US ? 2 : m == SO_PRESENT_AGE ? 3 : m >= SO_TICK_US ? 1 : 0; }
static void latest_begin(void) {
	__atomic_store_n(&s.latest_serial, LOAD(&s.latest_serial)+1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE); /* odd serial before payload stores */
}
static void latest_end(void) { __atomic_store_n(&s.latest_serial, LOAD(&s.latest_serial)+1, __ATOMIC_RELEASE); }
static void context(unsigned lane, unsigned kind, uint64_t at, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
	struct so_ring* r = &s.ring[lane];
	uint32_t head = LOAD(&r->head), tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
	if(head - tail == r->capacity) { add(&r->overflow, 1); return; }
	uint32_t* w = r->row[head % r->capacity];
	uint32_t serial = __atomic_load_n(&s.latest_serial, __ATOMIC_ACQUIRE);
	w[0] = kind; w[1] = (uint32_t)at; w[2] = (uint32_t)(at >> 32);
	w[3] = LOAD(&s.state); w[4] = LOAD(&s.frame); w[5] = LOAD(&s.tick);
	w[6] = a; w[7] = b; w[8] = c;
	for(unsigned k = 0; k < 10; k++) w[9+k] = LOAD(&s.latest[k]);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if((serial & 1) || serial != LOAD(&s.latest_serial) || w[4] != w[10]) {
		w[0] |= 0x80000000u; add(&s.lost[lane], 1);
	}
	w[19] = d;
	__atomic_store_n(&r->head, head + 1, __ATOMIC_RELEASE);
}
static void measure(unsigned m, uint32_t us, uint64_t at, uint32_t sample,
	uint32_t device, uint32_t owner, uint32_t action) {
	if(m >= SO_METRICS) { STORE(&s.counter_overflow, 1u); return; }
	struct so_metric_storage* t = &s.metric[m];
	uint32_t seq = LOAD(&t->serial);
	__atomic_store_n(&t->serial, seq + 1, __ATOMIC_SEQ_CST);
	__atomic_thread_fence(__ATOMIC_RELEASE); /* odd serial before payload stores */
	uint32_t count = LOAD(&t->w[1]);
	STORE(&t->w[0], m); add(&t->w[1], 1);
	uint64_t sum = ((uint64_t)LOAD(&t->w[3]) << 32) | LOAD(&t->w[2]);
	sum += us; STORE(&t->w[2], (uint32_t)sum); STORE(&t->w[3], (uint32_t)(sum >> 32));
	uint32_t minimum = LOAD(&t->w[4]), maximum = LOAD(&t->w[5]);
	if(us < minimum) STORE(&t->w[4], us);
	if(!count || us > maximum) STORE(&t->w[5], us);
	if(!count || (m == SO_AUDIO_RESERVE ? us < minimum : us > maximum)) {
		STORE(&t->w[6], (uint32_t)at); STORE(&t->w[7], (uint32_t)(at >> 32));
		STORE(&t->w[8], m == SO_PRESENT_AGE ? s.display_frame : LOAD(&s.frame)); STORE(&t->w[9], m == SO_CAP_US ? LOAD(&s.tick_boost_id) : LOAD(&s.tick));
		STORE(&t->w[10], sample); STORE(&t->w[11], device); STORE(&t->w[12], owner);
		STORE(&t->w[13], action); STORE(&t->w[14], LOAD(&s.state));
		STORE(&t->w[15], m == SO_AUDIO_RESERVE || m == SO_RT_US || m == SO_WT_US ? 0 : (uint32_t)(at - us));
	}
	add(&t->w[16 + service_observer_bin(us)], 1);
	__atomic_store_n(&t->serial, seq + 2, __ATOMIC_RELEASE);
}
void service_observer_frame(enum so_state state) {
	if(!service_observer_enabled()) return;
	uint64_t now = service_observer_now();
	if(state >= SO_STATES) { add(&s.lost[0], 1); return; }
	bool previous = LOAD(&s.frame) != 0;
	unsigned previous_state = LOAD(&s.state);
	latest_begin(); add(&s.frame, 1); STORE(&s.state, state);
	STORE(&s.latest[0], (uint32_t)now); STORE(&s.latest[1], LOAD(&s.frame)); latest_end();
	if(previous) measure(SO_FRAME_GAP, span(s.frame_at, now), now, 0, 0, 0, 0);
	add(&s.state_us[previous_state], span(s.state_at, now));
	s.state_at = now;
	if((unsigned)state != previous_state) context(0, SO_STATE_EDGE, now, state, 0, 0, 0);
	add(&s.state_count[state], 1); s.frame_at = now;
}
void service_observer_state(enum so_state state) {
	if(!service_observer_enabled()) return;
	if(state >= SO_STATES) { add(&s.lost[0], 1); return; }
	uint64_t now = service_observer_now();
	add(&s.state_us[LOAD(&s.state)], span(s.state_at, now));
	s.state_at = now;
	if((unsigned)state != LOAD(&s.state)) add(&s.state_count[state], 1);
	latest_begin(); STORE(&s.state, state); latest_end();
	context(0, SO_STATE_EDGE, now, state, 0, 0, 0);
}
void service_observer_poll(bool begin) {
	if(!service_observer_enabled()) return;
	uint64_t now = service_observer_now();
	if(begin) {
		if(s.poll_at) measure(SO_POLL_GAP, span(s.poll_at, now), now, 0, 0, 0, 0);
		s.poll_at = now;
	} else {
		if(!s.poll_at) { add(&s.lost[0], 1); return; }
		measure(SO_POLL_US, span(s.poll_at, now), now, 0, 0, 0, 0);
		s.poll_end = now; latest_begin(); STORE(&s.latest[2], (uint32_t)now); latest_end();
	}
}
void service_observer_receipt(unsigned device, bool connected, bool fresh) {
	if(!service_observer_enabled()) return;
	if(device >= 9) { add(&s.lost[0], 1); return; }
	struct so_sample* p = &s.sample[device];
	p->connected = connected;
	if(!connected) { add(&s.input_count[device][3], 1); p->receipt = 0; return; }
	if(!fresh) { add(&s.input_count[device][2], 1); return; }
	p->receipt = service_observer_now(); add(&p->sequence, 1);
	p->poll = LOAD(&s.frame); add(&s.input_count[device][0], 1);
}
void service_observer_consume(unsigned owner, unsigned device, unsigned action) {
	if(!service_observer_enabled()) return;
	if(owner >= 2 || device >= 9) { add(&s.lost[0], 1); return; }
	struct so_sample* p = &s.sample[device];
	struct so_present* consumed = &s.consumed[owner][device];
	uint32_t frame = LOAD(&s.frame);
	/* Queries never call here. Multiple actual action consumers in one
	 * frame share one owner/device sample and OR their action bits in O(1).
	 * Reuse means another FRAME uses the old sample, not another query. */
	if(s.consumed_frame[owner][device] == frame && consumed->sequence == p->sequence
	   && frame) { consumed->action |= action; return; }
	if(!p->connected || !p->receipt) { add(&s.input_count[device][4], 1); return; }
	uint64_t now = service_observer_now();
	bool reuse = consumed->sequence == p->sequence;
	measure(reuse ? SO_REUSE_AGE : SO_CONSUME_AGE, span(p->receipt, now), now, p->sequence, device, owner, action);
	add(&s.input_count[device][reuse ? 1 : 5], 1);
	*consumed = (struct so_present){p->sequence, device, action, p->receipt};
	s.consumed_frame[owner][device] = frame;
	latest_begin(); STORE(&s.latest[3], p->sequence); STORE(&s.latest[4], (uint32_t)p->receipt);
	STORE(&s.latest[5], (uint32_t)now); STORE(&s.latest[6], device | (owner << 8) | (action << 16)); latest_end();
}
void service_observer_input(unsigned owner, unsigned action, const void* owners) {
	if(!service_observer_enabled()) return;
	if(owner >= 2 || !owners) { add(&s.lost[0], 1); return; }
	service_observer_device(owner, action, &((const struct input_owner*)owners)[owner].device);
}
void service_observer_device(unsigned owner, unsigned action, const void* device_ptr) {
	if(!service_observer_enabled()) return;
	if(owner >= 2 || !device_ptr) { add(&s.lost[0], 1); return; }
	struct input_device d = *(const struct input_device*)device_ptr;
	if(d.kind == INPUT_DEVICE_NONE) { add(&s.input_unassigned, 1); return; }
	unsigned device = d.kind == INPUT_DEVICE_WPAD ? d.channel
		: d.kind == INPUT_DEVICE_GAMECUBE ? 4+d.channel : d.kind == INPUT_DEVICE_KEYBOARD ? 8 : 9;
	service_observer_consume(owner, device, action);
}
void service_observer_input_frame(const void* frame, unsigned action, const void* owners) {
	if(!service_observer_enabled() || !frame || !owners) return;
	const struct input_owner* p = owners;
	for(unsigned owner = 0; owner < 2; owner++) if(frame == &p[owner].frame) {
		service_observer_input(owner, action, owners); return;
	}
}
void service_observer_audio(enum so_audio_event event, bool active, uint32_t reserve) {
	if(!service_observer_enabled()) return;
	if(event >= SO_AUDIO_EVENTS) { add(&s.lost[0], 1); return; }
	if(event == SO_AUDIO_UNDERRUN) s.audio_reprime = 1;
	if(event == SO_AUDIO_PRIME && s.audio_reprime) { event = SO_AUDIO_REPRIME; s.audio_reprime = 0; }
	uint64_t now = service_observer_now();
	if(s.audio_at && s.audio_active) {
		uint32_t duration = span(s.audio_at, now);
		measure(SO_AUDIO_GAP, duration, now, 0, 0, 0, 0);
		if(duration > LOAD(&s.latest[8])) {
			add(&s.audio_estimated_exhaustion, 1);
			context(0, SO_AUDIO_EDGE, now, event, LOAD(&s.latest[8]), duration, 2);
		}
		uint64_t total = ((uint64_t)LOAD(&s.audio_active_hi) << 32) | LOAD(&s.audio_active_lo);
		total += duration; STORE(&s.audio_active_lo, (uint32_t)total); STORE(&s.audio_active_hi, (uint32_t)(total >> 32));
	}
	s.audio_at = now; s.audio_active = active;
	if(!active) add(&s.audio_inactive, 1);
	if(active) measure(SO_AUDIO_RESERVE, reserve, now, 0, 0, 0, event);
	add(&s.audio_count[event], 1);
	latest_begin(); STORE(&s.latest[7], (uint32_t)now); STORE(&s.latest[8], reserve); STORE(&s.latest[9], active); latest_end();
	if(event != SO_AUDIO_SERVICE) context(0, SO_AUDIO_EDGE, now, event, reserve, active, 0);
}
void service_observer_decode_underruns(uint32_t cumulative) {
	if(!service_observer_enabled()) return;
	if(cumulative < s.decode_underruns) { s.decode_underruns = cumulative; return; }
	uint32_t n = cumulative - s.decode_underruns; s.decode_underruns = cumulative;
	add(&s.audio_count[SO_AUDIO_DECODE_UNDERRUN], n);
	if(n) context(0, SO_AUDIO_EDGE, service_observer_now(), SO_AUDIO_DECODE_UNDERRUN, n, 0, 1);
}
void service_observer_tick(bool begin) {
	if(!service_observer_enabled()) return;
	uint64_t now = service_observer_now();
	if(begin) {
		uintptr_t id = native_owner(); STORE(&s.owner_lo, (uint32_t)id); STORE(&s.owner_hi, (uint32_t)((uint64_t)id >> 32));
		if(s.active_tick) add(&s.lost[1], 1);
		add(&s.tick, 1); s.tick_at = now; s.active_tick = 1;
		s.phase_at = now; s.rt = s.wt = s.component_worst = 0;
	} else if(server_owner() && s.active_tick) {
		/* Publish the completed interval as one snapshot. A final request may
		 * cut off an idle/partial tick, but never certify a torn completion. */
		__atomic_store_n(&s.tick_boundary_serial, LOAD(&s.tick_boundary_serial)+1, __ATOMIC_SEQ_CST);
		__atomic_thread_fence(__ATOMIC_RELEASE);
		/* MAIN may have frozen after this hook's initial enabled check.
		 * Publish odd before rechecking: either MAIN sees contention (loss),
		 * or this writer sees OFF and leaves the previous boundary intact. */
		if(!service_observer_enabled()) {
			__atomic_store_n(&s.tick_boundary_serial, LOAD(&s.tick_boundary_serial)+1, __ATOMIC_RELEASE);
			return;
		}
		service_observer_phase(s.phase, now);
		uint32_t elapsed = span(s.tick_at, now);
		bool worst = !LOAD(&s.metric[SO_TICK_US].w[1]) || elapsed > LOAD(&s.metric[SO_TICK_US].w[5]);
		measure(SO_TICK_US, elapsed, now, 0, 0, 0, 0);
		if(worst || s.component_worst) context(1, SO_TICK_EDGE, now, (uint32_t)s.tick_at, s.rt, s.wt, 0);
		s.active_tick = 0;
		STORE(&s.tick_boundary[0], (uint32_t)now);
		STORE(&s.tick_boundary[1], (uint32_t)(now >> 32));
		STORE(&s.tick_boundary[2], LOAD(&s.metric[SO_TICK_US].w[1]));
		__atomic_store_n(&s.tick_boundary_serial, LOAD(&s.tick_boundary_serial)+1, __ATOMIC_RELEASE);
	} else add(&s.lost[1], 1);
}
void service_observer_components(uint32_t rt, uint32_t wt) {
	if(!service_observer_enabled() || !server_owner() || !s.active_tick) return;
	uint64_t now = service_observer_now(); s.rt = rt; s.wt = wt;
	s.component_worst = !LOAD(&s.metric[SO_RT_US].w[1]) || rt > LOAD(&s.metric[SO_RT_US].w[5]) || wt > LOAD(&s.metric[SO_WT_US].w[5]);
	measure(SO_RT_US, rt, now, 0, 0, 0, 0); measure(SO_WT_US, wt, now, 0, 0, 0, 0);
}
void service_observer_phase(unsigned phase, uint64_t at) {
	if(!service_observer_enabled()) return;
	if(!server_owner()) {
		if(phase < SO_PHASES && native_owner() == s.main_owner && LOAD(&s.state) == SO_SHUTDOWN) {
			add(&s.teardown_unobserved, 1); return;
		}
		if(native_owner() == s.main_owner) add(&s.lost[0], 1);
		else STORE(&s.unsupported_writer, 1); /* multiwriter sticky missing flag, no RMW/retry */
		return;
	}
	if(phase >= SO_PHASES) { add(&s.lost[1], 1); return; }
	if(s.active_tick) measure(SO_PHASE_BASE + s.phase, span(s.phase_at, at), at, 0, 0, 0, s.phase);
	s.phase = phase; s.phase_at = at;
}
void service_observer_wait(unsigned kind, uint64_t begin, uint64_t end) {
	if(!service_observer_enabled()) return;
	if(!server_owner()) {
		if(kind < 3 && native_owner() == s.main_owner && LOAD(&s.state) == SO_SHUTDOWN) {
			add(&s.teardown_unobserved, 1); return;
		}
		if(native_owner() == s.main_owner) add(&s.lost[0], 1);
		else STORE(&s.unsupported_writer, 1);
		return;
	}
	if(kind >= 3) { add(&s.lost[1], 1); return; }
	measure(SO_WAIT_BASE + kind, span(begin, end), end, 0, 0, 0, kind);
}
void service_observer_boost(unsigned event, uint32_t actual_us) {
	if(!service_observer_enabled()) return;
	uint64_t now = service_observer_now();
	uint64_t at = now - (uint32_t)((uint32_t)now - actual_us);
#ifndef PLATFORM_WII
	/* Host cap has no IRQ/timer. Its MONOTONIC clock may differ from the
	 * engine RAW clock on Linux; report the common-clock forwarding instant.
	 * Fake-clock tests still exercise the exact Wii timestamp expansion. */
	if(!test_clock) at = now;
#endif
	/* begin is SERVER; capped-end is exclusively IRQ (or test injection).
 * normal-end is SERVER. Never share a ring between these writers. */
	if(event == 0) { s.boost_at = at; STORE(&s.tick_boost_id, LOAD(&s.tick) + (s.active_tick ? 0 : 1)); }
	else if(event == 1 && server_owner()) measure(SO_BOOST_US, span(s.boost_at, at), at, 0, 0, 0, event);
	else if(event == 2) measure(SO_CAP_US, span(s.boost_at, at), at, 0, 0, 0, event);
	context(event == 2 ? 2 : 1, event == 2 ? SO_CAP_EDGE : SO_BOOST_EDGE,
		at, event, LOAD(&s.tick_boost_id), (uint32_t)s.boost_at, (uint32_t)(at-s.boost_at));
}
void service_observer_checkpoint(uint32_t generation, uint32_t token, uint32_t logical_tick, uint32_t state, uint32_t previous) {
	if(!service_observer_enabled() || !server_owner()) return;
	if(state == previous) return;
	context(1, SO_CHECKPOINT_EDGE, service_observer_now(), generation, token, logical_tick, state);
}
void service_observer_submit(uintptr_t framebuffer) {
	if(!service_observer_enabled()) return;
	for(unsigned k = 0; k < 3; k++) if(!s.fb[k].key || s.fb[k].key == framebuffer) {
		s.fb[k].key = framebuffer; s.fb[k].frame = LOAD(&s.frame);
		/* MAIN owns an XFB removed from fb_from_vi, never the scanned XFB.
		 * copy_buffers sees it only AFTER this copy and the release fence,
		 * via fb_to_vi's volatile publication; post-retrace acquires here.
		 * Broadway is one core; MAIN cannot run inside the VI callback.
		 * A returned XFB is
		 * no longer scanned. That ownership prevents torn 64-bit receipts;
		 * no 64-bit atomic or IRQ-side retry is needed. See wii/gfx.c. */
		memcpy(s.fb[k].input, s.consumed, sizeof s.consumed);
		__atomic_thread_fence(__ATOMIC_RELEASE); return;
	}
	add(&s.lost[0], 1);
}
void service_observer_display(uintptr_t framebuffer) {
	if(!service_observer_enabled()) return;
	if(!framebuffer && !LOAD(&s.frame)) return; /* VI before first XFB publication. */
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	uint64_t now = service_observer_now();
	for(unsigned k = 0; k < 3; k++) if(s.fb[k].key == framebuffer) {
		s.display_frame = s.fb[k].frame;
		for(unsigned owner = 0; owner < 2; owner++) for(unsigned device = 0; device < 9; device++) {
			const struct so_present* p = &s.fb[k].input[owner][device];
			if(!p->sequence) continue;
			if(p->device >= 9) { add(&s.lost[3], 1); continue; }
			if(s.shown[owner][p->device] == p->sequence) continue;
			s.shown[owner][p->device] = p->sequence;
			uint32_t age = span(p->receipt, now);
			bool worst = !LOAD(&s.metric[SO_PRESENT_AGE].w[1]) || age > LOAD(&s.metric[SO_PRESENT_AGE].w[5]);
			measure(SO_PRESENT_AGE, age, now, p->sequence, p->device, owner, p->action);
			if(worst) context(3, SO_DISPLAY_EDGE, now, s.fb[k].frame, p->sequence, p->device | (owner << 8), 0);
		}
		return;
	}
	add(&s.lost[3], 1);
}
void service_observer_proxy(void) { service_observer_submit(1); service_observer_display(1); }
void service_observer_submit_safe(uintptr_t framebuffer, uintptr_t scanned) {
	if(!service_observer_enabled()) return;
	/* Do not overwrite a snapshot that VI may be reading. The observer is
	 * diagnostic: a broken token invariant loses evidence, never aborts play. */
	if(framebuffer == scanned) { add(&s.lost[0], 1); return; }
	service_observer_submit(framebuffer);
}

static size_t words(size_t off, const uint32_t* w, unsigned n) {
	int len = snprintf(s.line + off, sizeof s.line - off, "[");
	if(len < 0) return sizeof s.line;
	off += (size_t)len;
	for(unsigned k = 0; k < n && off < sizeof s.line; k++) {
		len = snprintf(s.line + off, sizeof s.line - off, "%s%u", k ? "," : "", w[k]);
		if(len < 0) return sizeof s.line;
		off += (size_t)len;
	}
	if(off + 2 >= sizeof s.line) return sizeof s.line;
	s.line[off++] = ']'; s.line[off] = '\0'; return off;
}
static void output(unsigned lane, bool final, bool census, const uint32_t metrics[][MW], unsigned n,
	const uint32_t context_rows[][SO_CONTEXT_WORDS], unsigned nc) {
	uint64_t now = service_observer_now();
	uint64_t over = 0, missing = LOAD(&s.unsupported_writer);
	for(unsigned k = 0; k < SO_LANES; k++) { over += LOAD(&s.ring[k].overflow); missing += LOAD(&s.lost[k]); }
	if(over > UINT32_MAX || missing > UINT32_MAX) STORE(&s.counter_overflow, 1u);
	int len = snprintf(s.line, sizeof s.line,
		"{\"event\":\"service_observer\",\"v\":2,\"input_boundary\":\"poll_drain\",\"seq\":%u,\"lane\":%u,\"at_us\":%llu,\"final\":%u,"
		"\"build\":\"%s\",\"config_id\":%u,\"origin_us\":%llu,\"clock\":\"monotonic_us\",\"resolution_us\":1,"
#ifdef PLATFORM_WII
		"\"presentation\":\"vi_retrace\","
#else
		"\"presentation\":\"render_submit_proxy\","
#endif
		"\"reserve\":\"time_head_estimate\",\"storage\":%u,\"input_unassigned\":%u,\"teardown_unobserved\":%u,\"tick_end_us\":%llu,\"completed_ticks\":%u,\"loss\":[%u,%u,%u,%u],\"metrics\":[",
		s.row_sequence+1, lane, (unsigned long long)now, final, s.build, s.config_id,
		(unsigned long long)s.origin, (unsigned)service_observer_storage_bytes(), LOAD(&s.input_unassigned),
		LOAD(&s.teardown_unobserved),
		(unsigned long long)(((uint64_t)s.final_tick_boundary[1] << 32) | s.final_tick_boundary[0]), s.final_tick_boundary[2],
		(unsigned)(missing > UINT32_MAX ? UINT32_MAX : missing), (unsigned)(over > UINT32_MAX ? UINT32_MAX : over), s.dropped, LOAD(&s.counter_overflow));
	if(len < 0 || (size_t)len >= sizeof s.line) { service_observer_transport_abort(); return; }
	size_t off = (size_t)len;
	for(unsigned k = 0; k < n && off < sizeof s.line; k++) {
		if(k) s.line[off++] = ',';
		off = words(off, metrics[k], MW);
	}
	if(off + 14 >= sizeof s.line) { service_observer_transport_abort(); return; }
	memcpy(s.line + off, "],\"context\":[", 13); off += 13;
	for(unsigned k = 0; k < nc && off < sizeof s.line; k++) {
		if(k) s.line[off++] = ',';
		off = words(off, context_rows[k], SO_CONTEXT_WORDS);
	}
	if(off + 13 >= sizeof s.line) { service_observer_transport_abort(); return; }
	memcpy(s.line + off, "],\"states\":[", 12); off += 12;
	if(census) for(unsigned k = 0; k < SO_STATES && off < sizeof s.line; k++) {
		if(k) s.line[off++] = ',';
		uint32_t w[3] = {k, LOAD(&s.state_count[k]), LOAD(&s.state_us[k])}; off = words(off, w, 3);
	}
	if(off + 12 >= sizeof s.line) { service_observer_transport_abort(); return; }
	memcpy(s.line + off, "],\"input\":[", 11); off += 11;
	if(census) for(unsigned k = 0; k < 9 && off < sizeof s.line; k++) {
		if(k) s.line[off++] = ',';
		uint32_t w[7]; w[0] = k; for(unsigned a = 0; a < 6; a++) w[a+1] = LOAD(&s.input_count[k][a]);
		off = words(off, w, 7);
	}
	if(off + 12 >= sizeof s.line) { service_observer_transport_abort(); return; }
	memcpy(s.line + off, "],\"audio\":[", 11); off += 11;
	if(census) {
		uint32_t w[14]; for(unsigned k = 0; k < SO_AUDIO_EVENTS; k++) w[k] = LOAD(&s.audio_count[k]);
		w[10] = LOAD(&s.audio_active_lo); w[11] = LOAD(&s.audio_active_hi);
		w[12] = LOAD(&s.audio_inactive); w[13] = LOAD(&s.audio_estimated_exhaustion);
		for(unsigned k = 0; k < 14 && off < sizeof s.line; k++) {
			len = snprintf(s.line + off, sizeof s.line - off, "%s%u", k ? "," : "", w[k]);
			if(len < 0) { off = sizeof s.line; break; } off += (size_t)len;
		}
	}
	if(off + 3 >= sizeof s.line) { service_observer_transport_abort(); return; }
	memcpy(s.line + off, "]}", 3);
	s.pending = 1;
}
/* Sender-only terminal failure: never replay a body that may already be on
 * TCP. No final lane is invented, so the collector remains fail-closed. */
void service_observer_transport_abort(void) {
	if(!service_observer_hold_transport()) return;
	add(&s.dropped, 1); STORE(&s.transport_failed, 1); s.pending = 0;
	__atomic_store_n(&service_observer_on, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&s.finished, 1u, __ATOMIC_RELEASE);
}
bool service_observer_transport_failed(void) { return LOAD(&s.transport_failed) != 0; }
static bool deliver(bool (*emit)(const char*)) {
	if(!s.pending || !emit || !emit(s.line)) return false;
	if(s.transport_failed) return false;
	add(&s.row_sequence, 1); s.pending = 0;
	__atomic_store_n(&s.ring[s.pending_lane].tail, s.pending_tail, __ATOMIC_RELEASE);
	if(s.pending_batch) {
		s.pump_cursor = s.pending_cursor; s.pump_footer = s.pending_footer;
		if(s.pending_done) {
			s.pump_lane++; s.pump_cursor = s.pump_footer = 0;
			if(s.pump_lane == SO_LANES) {
				s.pumping = 0;
				__atomic_store_n(&s.served, s.pump_request, __ATOMIC_RELEASE);
				if(s.pump_final) __atomic_store_n(&s.finished, 1u, __ATOMIC_RELEASE);
			}
		}
	}
	return true;
}
/* MAIN publishes an O(1) request. The existing telemetry sender alone
 * consumes/serializes a batch, one bounded packet per pump. This avoids a
 * producer burst overflowing the telemetry FIFO and keeps newlib off MAIN. */
void service_observer_request(bool final) {
	if(!__atomic_load_n(&observer_configured, __ATOMIC_ACQUIRE) || LOAD(&s.final_request)) return;
	uint64_t now = service_observer_now();
	if(final) {
		s.final_at = now;
		add(&s.state_us[LOAD(&s.state)], span(s.state_at, now));
		__atomic_store_n(&service_observer_on, 0u, __ATOMIC_RELEASE);
		/* No incomplete tick sample was recorded. Close tick coverage at the
		 * prior completed boundary, independent of profile_done's phase. Other
		 * service spans remain individually measured up to MAIN's freeze. */
		uint32_t serial = __atomic_load_n(&s.tick_boundary_serial, __ATOMIC_ACQUIRE);
		for(unsigned k = 0; k < 3; k++) s.final_tick_boundary[k] = LOAD(&s.tick_boundary[k]);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if((serial & 1) || serial != LOAD(&s.tick_boundary_serial)) add(&s.lost[0], 1);
		__atomic_store_n(&s.final_request, 1u, __ATOMIC_RELEASE);
		return;
	}
	if(s.last_request && now - s.last_request < 1000000) return;
	s.last_request = now;
	if(LOAD(&s.request) != __atomic_load_n(&s.served, __ATOMIC_ACQUIRE)) {
		/* Cumulative metrics and unread contexts are retained. Coalescing
		 * another snapshot request loses no observation; real ring overflow
		 * and incoherent publications still carry their own loss counters. */
		return;
	}
	__atomic_store_n(&s.request, LOAD(&s.request)+1, __ATOMIC_RELEASE);
}
bool service_observer_hold_transport(void) {
	return __atomic_load_n(&observer_configured, __ATOMIC_ACQUIRE) && !__atomic_load_n(&s.finished, __ATOMIC_ACQUIRE);
}
bool service_observer_pump(bool (*emit)(const char*)) {
	if(!service_observer_hold_transport()) return false;
	if(__atomic_load_n(&s.final_request, __ATOMIC_ACQUIRE)
	   && service_observer_now() - s.final_at >= SO_FINAL_TIMEOUT_US) {
		service_observer_transport_abort(); return false;
	}
	if(s.pending) return deliver(emit);
	if(!s.pumping) {
		uint32_t req = __atomic_load_n(&s.request, __ATOMIC_ACQUIRE);
		bool final = __atomic_load_n(&s.final_request, __ATOMIC_ACQUIRE) != 0;
		if(!final && req == LOAD(&s.served)) {
			/* Aggregate snapshots remain <=1/s. Edges cannot wait that long:
			 * two boost edges per tick exceed SERVER32 at 20 ticks/s. Drain
			 * at most two retained contexts per sender call between batches,
			 * with no producer IO, allocation, wakeup or changed ring bound. */
			for(unsigned lane = 0; lane < SO_LANES; lane++) {
				struct so_ring* ring = &s.ring[lane];
				uint32_t tail = LOAD(&ring->tail);
				uint32_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
				unsigned n = 0;
				while(tail != head && n < 2) {
					memcpy(s.batch_context[n++], ring->row[tail % ring->capacity], sizeof s.batch_context[0]);
					tail++;
				}
				if(!n) continue;
				s.pending_lane = lane; s.pending_tail = tail; s.pending_batch = 0;
				output(lane, false, false, s.batch_metrics, 0, s.batch_context, n);
				return deliver(emit);
			}
			return false;
		}
		s.pump_request = req; s.pump_final = final;
		s.pumping = 1; s.pump_lane = s.pump_cursor = s.pump_footer = 0;
		for(unsigned lane = 0; lane < SO_LANES; lane++)
			s.pump_limit[lane] = __atomic_load_n(&s.ring[lane].head, __ATOMIC_ACQUIRE);
	}
	unsigned lane = s.pump_lane;
	struct so_ring* ring = &s.ring[lane];
	uint32_t (*metrics)[MW] = s.batch_metrics;
	uint32_t (*ctx)[SO_CONTEXT_WORDS] = s.batch_context;
	unsigned nm = 0, nc = 0;
	bool contended = false;
	bool census = lane == 0 && s.pump_footer;
	uint32_t cursor = s.pump_cursor, tail = LOAD(&ring->tail);
	if(!census) {
		while(cursor < SO_METRICS && nm < 3) {
			unsigned m = cursor;
			if(metric_lane(m) != lane) { cursor++; continue; }
			uint32_t before = __atomic_load_n(&s.metric[m].serial, __ATOMIC_ACQUIRE);
			if(before & 1) { contended = true; break; }
			if(!LOAD(&s.metric[m].w[1])) { cursor++; continue; }
			for(unsigned k = 0; k < MW; k++) metrics[nm][k] = LOAD(&s.metric[m].w[k]);
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			if(before != LOAD(&s.metric[m].serial)) { contended = true; break; }
			cursor++; nm++;
		}
		while(tail != s.pump_limit[lane] && nc < 2) {
			memcpy(ctx[nc++], ring->row[tail % ring->capacity], sizeof ctx[0]); tail++;
		}
	}
	/* Only the sender retries, on a later call after its ordinary sleep/IO
	 * yield. No hook retry and no cursor advance means no missing samples.
	 * A stalled publisher retains the completion fence; it cannot earn PASS. */
	if(contended && !nm && !nc) return false;
	bool done = cursor == SO_METRICS && tail == s.pump_limit[lane];
	s.pending_footer = s.pump_footer;
	if(lane == 0 && done && !census) { s.pending_footer = 1; done = false; }
	s.pending_lane = lane; s.pending_tail = tail; s.pending_cursor = cursor;
	s.pending_done = done; s.pending_batch = 1;
	output(lane, s.pump_final && done, census, metrics, nm, ctx, nc);
	return deliver(emit);
}
void service_observer_flush(bool final, bool (*emit)(const char*)) {
	/* Quiescent fake-clock/PC fixture adapter. Production publishes requests
	 * only; the existing sender calls pump and owns all serialization/IO. */
	service_observer_request(final);
	for(unsigned k = 0; k < 42 && service_observer_pump(emit); k++) { }
}
