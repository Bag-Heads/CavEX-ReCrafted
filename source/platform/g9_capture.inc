/* Included by thread.c after its IRQ lock/timebase and before lease code.
 * Priority setters can switch with IRQs masked, so lease events must be
 * appended BEFORE setters; never acquire g9_lock recursively here. */
struct g9_capture_window {
	uint32_t rows[G9_CAPTURE_ROWS][G9_CAPTURE_WORDS];
	uint32_t n, tick, rt_max, drain_max, pop_max, checkpoint_us, checkpoint, incomplete;
};
static struct {
	struct g9_capture_window* windows;
	uint32_t enabled, active, owner, tick, drain, generation, regions;
	uint32_t rt_us, wt_us, components, stopped, finish, flushed, armed, restore_seen;
	uint32_t candidates, rejected, saturated, output_slot, output_row;
	int scratch, rank[4][4];
	uint64_t deadline, random_start, last_pop;
} s_capture;
#ifdef PLATFORM_WII
static KTickTask s_capture_timer;
#endif
static uint64_t (*s_capture_test_clock)(void);
void g9_capture_test_clock(uint64_t (*read_tb)(void)) {
	assert(!s_capture.active);
	s_capture_test_clock = read_tb;
}
static uint64_t capture_tb(void) {
	return s_capture_test_clock ? s_capture_test_clock() : g9_tb();
}
static uint32_t capture_us(uint64_t ticks) {
	uint64_t us = ticks * 1000000u / G9_TB_HZ;
	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}
bool g9_capture_enabled(void) {
	return __atomic_load_n(&s_capture.enabled, __ATOMIC_RELAXED) != 0;
}
static const unsigned s_capture_rank_count[4] = {3, 3, 1, 1};
static bool capture_referenced(int slot) {
	for(unsigned k = 0; k < 4; k++) for(unsigned i = 0; i < s_capture_rank_count[k]; i++)
		if(s_capture.rank[k][i] == slot) return true;
	return false;
}
static uint32_t* capture_row(uint32_t kind, uint64_t tb) {
	if(!s_capture.active) return NULL;
	struct g9_capture_window* w = &s_capture.windows[s_capture.scratch];
	/* Reserve a terminal record BEFORE accepting any ordinary row. */
	if(w->n == G9_CAPTURE_ROWS - 1) {
		uint32_t* r = w->rows[w->n++];
		memset(r, 0, G9_CAPTURE_WORDS * sizeof *r);
		r[0] = 13; r[1] = (uint32_t)(tb >> 32); r[2] = (uint32_t)tb;
		r[3] = s_capture.drain; r[4] = s_capture.generation;
		w->incomplete = kind; r[5] = kind;
		s_capture.saturated++;
		s_capture.stopped = 1;
		return NULL;
	}
	if(w->incomplete) return NULL;
	uint32_t* r = w->rows[w->n++];
	memset(r, 0, G9_CAPTURE_WORDS * sizeof *r);
	r[0] = kind; r[1] = (uint32_t)(tb >> 32); r[2] = (uint32_t)tb;
	r[3] = s_capture.drain; r[4] = s_capture.generation;
	return r + 5;
}
static void capture_lease(uint32_t event, uint32_t reason,
	uint32_t generation, uint32_t turn, uint32_t duration,
	uint32_t accepted, uint32_t complete, uint32_t flags, uint32_t pops) {
	if(!g9_capture_enabled() || !s_capture.active) return;
	if(event == 1) s_capture.generation = generation;
	uint32_t* d = capture_row(10, capture_tb());
	if(d) {
		d[-1] = generation; /* ACK may name a stale generation */
		d[0] = event; d[1] = reason; d[2] = turn; d[3] = duration;
		d[4] = accepted; d[5] = complete; d[6] = flags; d[7] = pops;
	}
}
bool g9_capture_configure(bool enabled) {
	/* Startup only, before sender/server/timer producers. Allocate ONLY ON. */
	assert(!s_capture.active);
#ifdef PLATFORM_WII
	if(s_capture.windows) mem2_free(s_capture.windows);
#else
	free(s_capture.windows);
#endif
	memset(&s_capture, 0, sizeof s_capture);
	for(unsigned k = 0; k < 4; k++) for(unsigned i = 0; i < s_capture_rank_count[k]; i++)
		s_capture.rank[k][i] = -1;
	if(!enabled) return true;
#ifdef PLATFORM_WII
	s_capture.windows = mem2_alloc_strict(sizeof *s_capture.windows * G9_CAPTURE_SLOTS);
#else
	s_capture.windows = malloc(sizeof *s_capture.windows * G9_CAPTURE_SLOTS);
#endif
	if(!s_capture.windows) return false;
	/* Touch reserved pages before timing any candidate. */
	memset(s_capture.windows, 0, sizeof *s_capture.windows * G9_CAPTURE_SLOTS);
	__atomic_store_n(&s_capture.enabled, 1, __ATOMIC_RELAXED);
	return true;
}
static uint32_t* capture_sample_locked(uint64_t tb, const uint32_t o[10]) {
	if(!s_capture.active || s_capture.stopped || tb < s_capture.deadline) return NULL;
	uint64_t late = tb - s_capture.deadline;
	uint32_t missed = (uint32_t)(late / G9_PERIOD);
	s_capture.deadline += ((uint64_t)missed + 1) * G9_PERIOD;
	uint32_t* d = capture_row(2, tb);
	if(d) {
		memcpy(d, o, 10 * sizeof *d);
		d[10] = capture_us(late); d[11] = missed;
		d[12] = s_capture.regions;
	}
	return d;
}
void g9_capture_sample(uint64_t tb, const uint32_t o[10]) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock(); (void)capture_sample_locked(tb, o); g9_unlock(lock);
}
#ifdef PLATFORM_WII
static void capture_timer_sample(KTickTask* timer) {
	(void)timer;
	uint64_t irq_begin = capture_tb();
	KThread* current = KThreadGetSelf();
	KThread* server = (KThread*)(uintptr_t)~s_capture.owner;
	uint32_t id = ~(uint32_t)(uintptr_t)current;
	uint32_t role = id == s_capture.owner ? 2 : id == s_role_id[THREAD_STRIPE_MAIN] ? 1 : 0;
	uint32_t state = server->state;
	uint32_t o[10] = {id, role, 127u-current->prio, 127u-current->baseprio,
		state, server->suspend,
		state >= KTHR_STATE_WAITING ? (uint32_t)(uintptr_t)server->wait.queue : 0,
		state >= KTHR_STATE_WAITING ? (uint32_t)server->wait.token : 0,
		__atomic_load_n(&s_role_phase[THREAD_STRIPE_MAIN], __ATOMIC_RELAXED), current->ctx.pc};
	uint64_t now = capture_tb();
	uint32_t* sample = capture_sample_locked(now, o);
	if(s_capture.active && !s_capture.stopped)
		KTickTaskStart(&s_capture_timer, capture_timer_sample,
			s_capture.deadline > now ? s_capture.deadline-now : G9_PERIOD, 0);
	if(sample) sample[13] = capture_us(capture_tb()-irq_begin);
}
#endif
void g9_capture_tick_begin(void) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock();
	assert(!s_capture.active);
	if(!s_capture.armed || s_capture.finish || s_capture.stopped) { s_capture.rejected++; g9_unlock(lock); return; }
	int slot = 0;
	while(slot < (int)G9_CAPTURE_SLOTS && capture_referenced(slot)) slot++;
	assert(slot < (int)G9_CAPTURE_SLOTS); /* eight ranks + reserved candidate */
	s_capture.scratch = slot;
	struct g9_capture_window* w = &s_capture.windows[slot];
	/* Payload is cleared one row at a time, never a megabyte hot memcpy. */
	w->n = w->rt_max = w->drain_max = w->pop_max = w->checkpoint_us = w->checkpoint = w->incomplete = 0;
	w->tick = ++s_capture.tick;
	s_capture.candidates++;
	s_capture.active = 1; s_capture.owner = stripe_thread_id();
	s_capture.drain = s_capture.generation = 0;
	s_capture.rt_us = s_capture.wt_us = s_capture.components = 0;
	uint64_t now = capture_tb();
	s_capture.deadline = now + G9_PERIOD;
	uint32_t* d = capture_row(1, now);
	d[0] = s_capture.owner; d[1] = s_capture.regions;
	d[2] = __atomic_load_n(&s_role_phase[THREAD_STRIPE_MAIN], __ATOMIC_RELAXED);
#ifdef PLATFORM_WII
	KTickTaskStart(&s_capture_timer, capture_timer_sample, G9_PERIOD, 0);
#endif
	g9_unlock(lock);
}
void g9_capture_tick_components(uint32_t rt_us, uint32_t wt_us) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock();
	if(s_capture.active) { s_capture.rt_us = rt_us; s_capture.wt_us = wt_us; s_capture.components = 1; }
	g9_unlock(lock);
}
static uint32_t capture_score(unsigned kind, const struct g9_capture_window* w) {
	return kind == 0 ? w->rt_max : kind == 1 ? w->drain_max
		: kind == 2 ? w->pop_max : w->checkpoint_us;
}
static void capture_rank(unsigned kind, int slot) {
	struct g9_capture_window* w = &s_capture.windows[slot];
	uint32_t score = capture_score(kind, w);
	if(!score) return;
	for(unsigned i = 0; i < s_capture_rank_count[kind]; i++) {
		int old = s_capture.rank[kind][i];
		uint32_t old_score = old < 0 ? 0 : capture_score(kind, &s_capture.windows[old]);
		if(score > old_score) {
			for(unsigned j = s_capture_rank_count[kind]-1; j > i; j--) s_capture.rank[kind][j] = s_capture.rank[kind][j-1];
			s_capture.rank[kind][i] = slot; return;
		}
	}
}
void g9_capture_tick_end(void) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock();
	if(s_capture.active) {
#ifdef PLATFORM_WII
		KTickTaskStop(&s_capture_timer);
#endif
		uint64_t now = capture_tb();
		uint32_t* d = capture_row(12, now);
		if(d) {
			d[0] = s_capture.rt_us; d[1] = s_capture.wt_us; d[2] = s_capture.components;
			d[3] = now >= s_capture.deadline ? (uint32_t)((now-s_capture.deadline)/G9_PERIOD)+1 : 0;
		}
		struct g9_capture_window* w = &s_capture.windows[s_capture.scratch];
		if(!w->incomplete) {
			if(w->checkpoint) {
				uint64_t start = ((uint64_t)w->rows[0][1] << 32) | w->rows[0][2];
				w->checkpoint_us = capture_us(now-start);
			}
			for(unsigned kind = 0; kind < 4; kind++) capture_rank(kind, s_capture.scratch);
		}
		s_capture.active = 0;
	}
	g9_unlock(lock);
}
void g9_capture_random(bool begin, uint32_t world, uint32_t generation, uint32_t tick) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock();
	uint64_t now = capture_tb();
	uint32_t us = begin ? 0 : capture_us(now-s_capture.random_start);
	if(begin) s_capture.random_start = now;
	uint32_t* d = capture_row(4, now);
	if(d) { d[0] = begin ? 1 : 2; d[1] = world; d[2] = generation; d[3] = tick; d[4] = us; }
	if(s_capture.active && us > s_capture.windows[s_capture.scratch].rt_max)
		s_capture.windows[s_capture.scratch].rt_max = us;
	g9_unlock(lock);
}
void g9_capture_drain_begin(uint32_t world, uint32_t generation, uint32_t tick, uint32_t stage, uint32_t seeds) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock();
	s_capture.drain++; s_capture.generation = 0; s_capture.last_pop = 0; s_capture.restore_seen = 0;
	uint64_t now = capture_tb();
	uint32_t* d = capture_row(5, now);
	if(d) { d[0] = world; d[1] = generation; d[2] = tick; d[3] = stage; d[4] = seeds; }
	if(s_capture.active && stage) s_capture.windows[s_capture.scratch].checkpoint = 1;
	g9_unlock(lock);
}
void g9_capture_pop(void) {
	if(g9_capture_enabled())
		s_capture.last_pop = capture_tb(); /* server writer only; published by final */
}
void g9_capture_record_enter(void) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock(); (void)capture_row(8, capture_tb()); g9_unlock(lock);
}
void g9_capture_arm(void) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock(); s_capture.armed = 1; g9_unlock(lock);
}
void g9_capture_drain_final(uint32_t pops, uint32_t result) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock(); uint32_t* d = capture_row(6, capture_tb());
	if(d) { d[0] = pops; d[1] = result; d[2] = (uint32_t)(s_capture.last_pop >> 32); d[3] = (uint32_t)s_capture.last_pop; }
	if(s_capture.active && pops > s_capture.windows[s_capture.scratch].pop_max)
		s_capture.windows[s_capture.scratch].pop_max = pops;
	g9_unlock(lock);
}
void g9_capture_restore_return(void) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock();
	(void)capture_row(s_capture.restore_seen ? 15 : 7, capture_tb());
	s_capture.restore_seen = 1; g9_unlock(lock);
}
void g9_capture_record_end(uint32_t old_us) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock(); uint64_t now = capture_tb();
	uint32_t* d = capture_row(14, now); if(d) d[0] = old_us;
	if(s_capture.active && old_us > s_capture.windows[s_capture.scratch].drain_max)
		s_capture.windows[s_capture.scratch].drain_max = old_us;
	g9_unlock(lock);
}
void g9_capture_record_return(void) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock(); (void)capture_row(9, capture_tb()); g9_unlock(lock);
}
void g9_capture_stage(bool begin, uint32_t world, uint32_t stage) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock(); uint32_t* d = capture_row(11, capture_tb());
	if(d) { d[0] = begin ? 1 : 2; d[1] = world; d[2] = stage; }
	if(s_capture.active) s_capture.windows[s_capture.scratch].checkpoint = 1;
	g9_unlock(lock);
}
static void capture_main_locked(unsigned region, bool begin, uint32_t work) {
	if(region >= 16) return;
	if(begin) s_capture.regions |= 1u << region;
	else s_capture.regions &= ~(1u << region);
	if(!s_capture.active) return;
	uint32_t* d = capture_row(3, capture_tb());
	if(d) { d[0] = region; d[1] = begin ? 1 : 2; d[2] = work; d[3] = s_capture.regions; }
}
void g9_capture_main(unsigned region, bool begin, uint32_t work) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock(); capture_main_locked(region, begin, work); g9_unlock(lock);
}
void g9_capture_audio(uint32_t reserve_us, bool worker, uint32_t fx_state) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock(); uint32_t* d = capture_row(16, capture_tb());
	if(d) { d[0] = reserve_us; d[1] = worker; d[2] = fx_state; }
	g9_unlock(lock);
}
void g9_capture_finish(void) {
	if(!g9_capture_enabled()) return;
	g9_lock_t lock = g9_lock(); s_capture.finish = 1; g9_unlock(lock);
}
bool g9_capture_pending(void) {
	if(!g9_capture_enabled()) return false;
	g9_lock_t lock = g9_lock(); bool pending = s_capture.finish && !s_capture.flushed; g9_unlock(lock);
	return pending;
}
bool g9_capture_pump(bool (*emit)(const char*)) {
	_Static_assert(8u * G9_CAPTURE_WORDS * 11u + 538u < 2304u,
		"bounded capture batch must fit the observer wire limit");
	if(!g9_capture_enabled() || !emit || thread_drain_wait_active()) return false;
	uint32_t rows[8][G9_CAPTURE_WORDS], n = 0, slot, offset, tick, rt, drain, pops, checkpoint, incomplete;
	g9_lock_t lock = g9_lock();
	/* Frozen buffers; no formatting/network while a captured tick/drain is live. */
	if(!s_capture.finish || s_capture.active || s_capture.flushed) { g9_unlock(lock); return false; }
	slot = s_capture.output_slot;
	while(slot < G9_CAPTURE_SLOTS && !capture_referenced((int)slot)
		&& !(s_capture.saturated && slot == (uint32_t)s_capture.scratch)) slot++;
	if(slot == G9_CAPTURE_SLOTS) {
		uint32_t candidates = s_capture.candidates, saturated = s_capture.saturated;
		uint32_t ticks[G9_CAPTURE_SLOTS], n_ticks = 0;
		for(unsigned i = 0; i < G9_CAPTURE_SLOTS; i++)
			if(capture_referenced((int)i) || (s_capture.saturated && i == (unsigned)s_capture.scratch))
				ticks[n_ticks++] = s_capture.windows[i].tick;
		g9_unlock(lock);
		char line[512];
		int len = snprintf(line, sizeof line, "{\"event\":\"g9_capture_seal\",\"v\":1,\"candidates\":%u,\"saturated\":%u,\"capacity\":%u,\"slots\":%u,\"ticks\":[",
			candidates, saturated, G9_CAPTURE_ROWS, G9_CAPTURE_SLOTS);
		if(len < 0 || (size_t)len >= sizeof line) return false;
		size_t off = (size_t)len;
		for(unsigned i = 0; i < n_ticks; i++) {
			len = snprintf(line+off, sizeof line-off, i ? ",%u" : "%u", ticks[i]);
			if(len < 0 || (size_t)len >= sizeof line-off) return false;
			off += (size_t)len;
		}
		memcpy(line+off, "]}", 3);
		if(!emit(line)) return false;
		lock = g9_lock(); s_capture.flushed = 1; g9_unlock(lock); return true;
	}
	struct g9_capture_window* w = &s_capture.windows[slot];
	offset = slot == s_capture.output_slot ? s_capture.output_row : 0;
	n = w->n-offset; if(n > 8) n = 8;
	for(unsigned i = 0; i < n; i++) memcpy(rows[i], w->rows[offset+i], sizeof rows[i]);
	tick = w->tick; rt = w->rt_max; drain = w->drain_max; pops = w->pop_max; checkpoint = w->checkpoint_us; incomplete = w->incomplete;
	uint32_t total = w->n;
	g9_unlock(lock);
	char line[2304];
	int len = snprintf(line, sizeof line, "{\"event\":\"g9_capture\",\"v\":1,\"tb_hz\":%u,\"tick\":%u,\"offset\":%u,\"total\":%u,\"rt_max\":%u,\"drain_max\":%u,\"pop_max\":%u,\"checkpoint_tick_us\":%u,\"incomplete\":%u,\"rows\":[",
		G9_TB_HZ, tick, offset, total, rt, drain, pops, checkpoint, incomplete);
	if(len < 0 || (size_t)len >= sizeof line) return false;
	size_t off = (size_t)len;
	for(unsigned i = 0; i < n; i++) {
		for(unsigned k = 0; k < G9_CAPTURE_WORDS; k++) {
			len = snprintf(line+off, sizeof line-off, k ? ",%u" : i ? ",[%u" : "[%u", rows[i][k]);
			if(len < 0 || (size_t)len >= sizeof line-off) return false;
			off += (size_t)len;
		}
		line[off++] = ']';
	}
	memcpy(line+off, "]}", 3);
	if(!emit(line)) return false; /* Same frozen bytes on retry. */
	lock = g9_lock();
	s_capture.output_slot = offset+n == total ? slot+1 : slot;
	s_capture.output_row = offset+n == total ? 0 : offset+n;
	g9_unlock(lock);
	return true;
}
