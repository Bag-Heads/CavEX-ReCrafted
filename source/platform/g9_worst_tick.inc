/* G9 R20 Q3 worst-tick attribution probe (g9_worst_tick.h). Included by
 * thread.c AFTER g9_priority_lease.inc: it reads the tick boost's lock-free
 * cap instant (s_tb.cap_seq / cap_at) and shares stripe_thread_id and the
 * lease's microsecond clock. No allocation, no lock, no formatting on the
 * server thread; every hook is one relaxed load while the knob is off.
 *
 * Window handoff (no lock): MAIN owns `req`, the server owns `served`.
 * At a tick end with served != req the server copies its window into `pub`,
 * resets the window and stores served = req (release). MAIN reads `pub` only
 * while served == req (acquire), then increments req (release) -- so the two
 * never touch `pub` at the same time and every tick lands in exactly one
 * window. A window closes at the first tick end after MAIN's previous take. */
static struct {
	uint32_t enabled, owner, active, tick, seg_start, seg_phase, cap_seq0;
	uint32_t t0, cap_at, cap_split, have_best, req, served, seq, foreign;
	uint32_t last_phase; /* the server's current srv phase (thread.c has no watchdog link) */
	uint32_t cpu_active, cpu_mask, cpu_edge_us, cpu_wt, cpu_random;
	struct g9_worst_tick_rec cur, best, pub;
	uint32_t win_ticks, win_rt_max, win_wt_max, win_dropped, win_rpc_pend;
} s_wt;
static uint32_t (*s_wt_test_clock)(void);

static uint32_t wt_now(void) {
	return s_wt_test_clock ? s_wt_test_clock() : lease_now();
}
void g9_worst_tick_test_clock(uint32_t (*now_us)(void)) {
	s_wt_test_clock = now_us;
}
void g9_worst_tick_configure(bool enabled) {
	assert(!s_wt.active);
	memset(&s_wt, 0, sizeof s_wt);
	s_wt.req = 1;
	__atomic_store_n(&s_wt.enabled, enabled ? 1u : 0u, __ATOMIC_RELEASE);
}
bool g9_worst_tick_enabled(void) {
	return __atomic_load_n(&s_wt.enabled, __ATOMIC_RELAXED) != 0;
}
static bool wt_cpu_mode(void) {
	return g9_worst_tick_enabled()
		&& __atomic_load_n(&s_g9.sampler, __ATOMIC_RELAXED) != 0;
}
static uint64_t wt_cpu_tb(void) {
	return s_wt_test_clock ? (uint64_t)wt_now() * G9_TB_HZ / 1000000u : g9_tb();
}
/* All cpu state/counters share the EXISTING recorder lock (IRQ exclusion on
 * Wii). No allocation or observer work in the callback or timed brackets. */
static void wt_cpu_missed(uint32_t n) {
	for(unsigned i = 0; i < G9WT_CPU_ROWS; i++)
		if(s_wt.cpu_mask & (1u << i)) s_wt.cur.cpu_sample[i][5] += n;
}
static void wt_cpu_edge(uint32_t mask, uint32_t now) {
	uint32_t us = now - s_wt.cpu_edge_us;
	for(unsigned i = 0; i < G9WT_CPU_ROWS; i++)
		if(s_wt.cpu_mask & (1u << i)) s_wt.cur.cpu_sample[i][0] += us;
	const uint64_t tb = wt_cpu_tb();
	if(tb >= s_g9.deadline) {
		uint32_t n = (uint32_t)((tb - s_g9.deadline) / G9_PERIOD) + 1u;
		wt_cpu_missed(n);
		s_g9.deadline += (uint64_t)n * G9_PERIOD;
	}
	s_wt.cpu_edge_us = now;
	s_wt.cpu_mask = mask;
}
static void wt_cpu_sample(uint64_t tb, const uint32_t o[10]) {
	if(!s_wt.cpu_active || tb < s_g9.deadline) return;
	const uint64_t late = tb - s_g9.deadline;
	const uint32_t skipped = (uint32_t)(late / G9_PERIOD);
	const bool is_late = late * 1000000u / G9_TB_HZ > 50u;
	s_g9.deadline += ((uint64_t)skipped + 1u) * G9_PERIOD;
	/* SDK state RUNNING=2 is guarded by thread.c's Wii ABI assertion. */
	unsigned kind = o[0] == s_wt.owner ? 1u : o[4] == 2u && !o[5] ? 2u : 3u;
	for(unsigned i = 0; i < G9WT_CPU_ROWS; i++) if(s_wt.cpu_mask & (1u << i)) {
		s_wt.cur.cpu_sample[i][kind]++;
		if(is_late) s_wt.cur.cpu_sample[i][4]++;
	}
	wt_cpu_missed(skipped);
}
/* The owner-thread, inside-a-tick test every note shares. */
static bool wt_live(void) {
	if(!g9_worst_tick_enabled()) return false;
	uint32_t owner = __atomic_load_n(&s_wt.owner, __ATOMIC_RELAXED);
	if(!owner) return false; /* before the first timed tick */
	if(owner != stripe_thread_id()) {
		__atomic_fetch_add(&s_wt.foreign, 1u, __ATOMIC_RELAXED);
		return false;
	}
	return s_wt.active != 0;
}
void g9_worst_tick_wt(bool begin) {
	if(!wt_cpu_mode() || !wt_live()) return;
	g9_lock_t lock = g9_lock();
	s_wt.cpu_wt = begin;
	uint32_t mask = s_wt.cpu_mask & ~(1u << G9WT_CPU_WT);
	if(begin && !s_wt.cpu_random) mask |= 1u << G9WT_CPU_WT;
	wt_cpu_edge(mask, wt_now());
	g9_unlock(lock);
}
void g9_worst_tick_random(bool begin) {
	if(!wt_cpu_mode() || !wt_live()) return;
	g9_lock_t lock = g9_lock();
	s_wt.cpu_random = begin;
	uint32_t mask = s_wt.cpu_mask & ~(1u << G9WT_CPU_WT);
	if(!begin && s_wt.cpu_wt) mask |= 1u << G9WT_CPU_WT;
	wt_cpu_edge(mask, wt_now());
	g9_unlock(lock);
}
static void wt_cpu_stage(uint32_t stage, bool begin) {
	if(!wt_cpu_mode() || !wt_live()) return;
	if(begin && stage > 4) return;
	g9_lock_t lock = g9_lock();
	uint32_t mask = s_wt.cpu_mask & ((1u << G9WT_CPU_TICK) | (1u << G9WT_CPU_WT));
	if(begin) mask |= 1u << (G9WT_CPU_NORMAL + stage);
	wt_cpu_edge(mask, wt_now());
	g9_unlock(lock);
}
/* Close the open segment at `now`, splitting it at the cap instant once. */
static void wt_account(uint32_t now) {
	uint32_t ph = s_wt.seg_phase < G9WT_PHASES ? s_wt.seg_phase : 0u;
	uint32_t seg = now - s_wt.seg_start;
	if(!s_wt.cap_split
	   && __atomic_load_n(&s_tb.cap_seq, __ATOMIC_ACQUIRE) != s_wt.cap_seq0) {
		uint32_t at = __atomic_load_n(&s_tb.cap_at, __ATOMIC_RELAXED);
		/* clamp into this segment (the cap fired since the previous note) */
		uint32_t pre = at - s_wt.seg_start;
		if((int32_t)pre < 0) pre = 0;
		if(pre > seg) pre = seg;
		s_wt.cap_split = 1;
		s_wt.cap_at = s_wt.seg_start + pre;
		s_wt.cur.cap_us = s_wt.cap_at - s_wt.t0;
		s_wt.cur.flags |= G9WT_F_CAPPED;
		s_wt.cur.ph_pre[ph] += pre;
		s_wt.cur.ph_post[ph] += seg - pre;
	} else if(s_wt.cap_split) {
		s_wt.cur.ph_post[ph] += seg;
	} else {
		s_wt.cur.ph_pre[ph] += seg;
	}
	s_wt.seg_start = now;
}
void g9_worst_tick_begin(void) {
	if(!g9_worst_tick_enabled()) return;
	if(!s_wt.owner) __atomic_store_n(&s_wt.owner, stripe_thread_id(), __ATOMIC_RELAXED);
	if(s_wt.owner != stripe_thread_id()) {
		__atomic_fetch_add(&s_wt.foreign, 1u, __ATOMIC_RELAXED);
		return;
	}
	memset(&s_wt.cur, 0, sizeof s_wt.cur);
	s_wt.cur.tick = ++s_wt.tick;
	s_wt.cur.cap_us = UINT32_MAX;
	s_wt.cap_seq0 = __atomic_load_n(&s_tb.cap_seq, __ATOMIC_ACQUIRE);
	s_wt.cap_split = 0;
	s_wt.t0 = s_wt.seg_start = wt_now();
	s_wt.seg_phase = s_wt.last_phase;
	s_wt.active = 1;
	if(wt_cpu_mode()) {
		g9_lock_t lock = g9_lock();
		s_wt.cur.cpu_sampled = s_wt.cpu_active = 1;
		s_wt.cpu_wt = s_wt.cpu_random = 0;
		s_wt.cpu_mask = 1u << G9WT_CPU_TICK;
		s_wt.cpu_edge_us = s_wt.t0;
		s_g9.deadline = wt_cpu_tb() + G9_PERIOD;
#ifdef PLATFORM_WII
		KTickTaskStart(&s_g9_timer, g9_timer_sample, G9_PERIOD, 0);
#endif
		g9_unlock(lock);
	}
}
void g9_worst_tick_phase(uint32_t phase) {
	if(!g9_worst_tick_enabled()) return;
	uint32_t owner = __atomic_load_n(&s_wt.owner, __ATOMIC_RELAXED);
	if(!owner || owner == stripe_thread_id()) s_wt.last_phase = phase;
	if(!wt_live()) return;
	wt_account(wt_now());
	s_wt.seg_phase = phase;
}
void g9_worst_tick_components(uint32_t rt_us, uint32_t wt_us,
	uint32_t world_time, uint32_t dim) {
	SERVICE_OBSERVER(service_observer_components(rt_us, wt_us));
	if(!wt_live()) return;
	s_wt.cur.rt_us = rt_us;
	s_wt.cur.wt_us = wt_us;
	s_wt.cur.world_time = world_time;
	s_wt.cur.dim = dim;
	s_wt.cur.wt_end_us = wt_now() - s_wt.t0;
	s_wt.cur.flags |= G9WT_F_COMPONENTS;
}
void g9_worst_tick_flag(uint32_t flag) {
	if(wt_live()) s_wt.cur.flags |= flag;
}
void g9_worst_tick_drain(bool trial, uint32_t pops, uint32_t us) {
	if(!wt_live()) return;
	if(trial) {
		s_wt.cur.trial_n++; s_wt.cur.trial_pops += pops; s_wt.cur.trial_us += us;
	} else {
		s_wt.cur.drain_n++; s_wt.cur.drain_pops += pops; s_wt.cur.drain_us += us;
	}
}
void g9_worst_tick_ckpt(uint32_t us, bool batch_open) {
	if(!wt_live()) return;
	s_wt.cur.ckpt_n++;
	s_wt.cur.ckpt_us += us;
	s_wt.cur.flags |= G9WT_F_CKPT | (batch_open ? G9WT_F_CKPT_BATCH : 0u);
}
void g9_worst_tick_pending(uint32_t occupancy) {
	if(wt_live() && occupancy > s_wt.cur.rpc_pend_max)
		s_wt.cur.rpc_pend_max = occupancy;
}
uint32_t g9_worst_tick_wait_begin(void) {
	if(!wt_live()) return 0;
	uint32_t now = wt_now();
	return now ? now : 1u;
}
void g9_worst_tick_wait_us(enum g9wt_wait kind, uint32_t us) {
	if((unsigned)kind >= G9WT_W_N || !wt_live()) return;
	s_wt.cur.wait_n[kind]++;
	s_wt.cur.wait_us[kind] += us;
}
void g9_worst_tick_wait_end(enum g9wt_wait kind, uint32_t start) {
	if(!start) return;
	g9_worst_tick_wait_us(kind, wt_now() - start);
}
void g9_worst_tick_end(void) {
	if(!g9_worst_tick_enabled() || !s_wt.active
	   || s_wt.owner != stripe_thread_id()) return;
	uint32_t now;
	if(s_wt.cpu_active) {
		g9_lock_t lock = g9_lock();
#ifdef PLATFORM_WII
		KTickTaskStop(&s_g9_timer);
#endif
		now = wt_now();
		wt_cpu_edge(0, now); /* includes overdue IRQ-disabled tail deadlines */
		s_wt.cpu_active = 0;
		g9_unlock(lock);
	} else now = wt_now();
	wt_account(now);
	s_wt.active = 0;
	struct g9_worst_tick_rec* c = &s_wt.cur;
	c->tick_us = now - s_wt.t0;
	uint32_t sum = 0;
	for(unsigned k = 0; k < G9WT_PHASES; k++) sum += c->ph_pre[k] + c->ph_post[k];
	c->gap_us = c->tick_us - sum; /* 0 by construction (one clock, contiguous) */
	c->un_us = c->ph_pre[0] + c->ph_post[0]; /* WDS_IDLE: no named phase */
	s_wt.win_ticks++;
	if(c->rt_us > s_wt.win_rt_max) s_wt.win_rt_max = c->rt_us;
	if(c->wt_us > s_wt.win_wt_max) s_wt.win_wt_max = c->wt_us;
	if(c->rpc_pend_max > s_wt.win_rpc_pend) s_wt.win_rpc_pend = c->rpc_pend_max;
	if(!s_wt.have_best || c->wt_us > s_wt.best.wt_us) {
		s_wt.best = *c;
		s_wt.have_best = 1;
	}
	uint32_t req = __atomic_load_n(&s_wt.req, __ATOMIC_ACQUIRE);
	if(__atomic_load_n(&s_wt.served, __ATOMIC_RELAXED) != req) {
		s_wt.pub = s_wt.best;
		s_wt.pub.seq = ++s_wt.seq;
		s_wt.pub.ticks = s_wt.win_ticks;
		s_wt.pub.win_rt_max = s_wt.win_rt_max;
		s_wt.pub.win_wt_max = s_wt.win_wt_max;
		s_wt.pub.win_rpc_pend_max = s_wt.win_rpc_pend;
		uint32_t foreign = __atomic_exchange_n(&s_wt.foreign, 0u, __ATOMIC_RELAXED);
		s_wt.pub.notes_dropped = foreign;
		if(foreign) s_wt.pub.flags |= G9WT_F_FOREIGN;
		s_wt.have_best = 0;
		s_wt.win_ticks = s_wt.win_rt_max = s_wt.win_wt_max = 0;
		s_wt.win_rpc_pend = 0;
		__atomic_store_n(&s_wt.served, req, __ATOMIC_RELEASE);
	}
}
bool g9_worst_tick_take(struct g9_worst_tick_rec* out) {
	if(!g9_worst_tick_enabled() || !out) return false;
	uint32_t req = __atomic_load_n(&s_wt.req, __ATOMIC_RELAXED);
	if(__atomic_load_n(&s_wt.served, __ATOMIC_ACQUIRE) != req) return false;
	*out = s_wt.pub;
	__atomic_store_n(&s_wt.req, req + 1u, __ATOMIC_RELEASE);
	return true;
}
#ifndef PLATFORM_WII
/* Host tests: fire the tick boost's hard cap now (PC has no cap timer). */
void g9_worst_tick_test_cap(void) {
	g9_lock_t lock = g9_lock();
	tb_drop(lease_now(), true);
	g9_unlock(lock);
}
#endif
static size_t wt_arr(char* dst, size_t cap, size_t off, const char* key,
	const uint32_t* v, unsigned n) {
	if(off >= cap) return off;
	int w = snprintf(dst + off, cap - off, ",\"%s\":[", key);
	if(w < 0) return cap;
	off += (size_t)w;
	for(unsigned k = 0; k < n && off < cap; k++) {
		w = snprintf(dst + off, cap - off, k ? ",%u" : "%u", (unsigned)v[k]);
		if(w < 0) return cap;
		off += (size_t)w;
	}
	if(off < cap) {
		w = snprintf(dst + off, cap - off, "]");
		off += w < 0 ? cap : (size_t)w;
	}
	return off;
}
size_t g9_worst_tick_format(const struct g9_worst_tick_rec* r, char* dst, size_t cap) {
	if(!r || !dst || !cap) return 0;
	int w = snprintf(dst, cap,
		"{\"event\":\"g9_worst_tick\",\"v\":1,\"seq\":%u,\"ticks\":%u,"
		"\"tick\":%u,\"wtime\":%u,\"dim\":%u,\"flags\":%u,\"tick_us\":%u,"
		"\"rt_us\":%u,\"wt_us\":%u,\"wt_end_us\":%u,\"cap_us\":%d",
		r->seq, r->ticks, r->tick, r->world_time, r->dim, r->flags, r->tick_us,
		r->rt_us, r->wt_us, r->wt_end_us,
		r->cap_us == UINT32_MAX ? -1 : (int)r->cap_us);
	if(w < 0 || (size_t)w >= cap) return 0;
	size_t off = (size_t)w;
	off = wt_arr(dst, cap, off, "pre", r->ph_pre, G9WT_PHASES);
	off = wt_arr(dst, cap, off, "post", r->ph_post, G9WT_PHASES);
	if(off >= cap) return 0;
	w = snprintf(dst + off, cap - off,
		",\"un_us\":%u,\"gap_us\":%u,\"drain\":[%u,%u,%u],\"trial\":[%u,%u,%u],"
		"\"ckpt\":[%u,%u],\"io_wait\":[%u,%u],\"send_wait\":[%u,%u],"
		"\"disk_wait\":[%u,%u],"
		"\"rpc_pend\":[%u,%u],"
		"\"win_rt_max\":%u,\"win_wt_max\":%u,\"dropped\":%u,"
		"\"disjoint\":\"pre+post+gap=tick_us\","
		"\"nested\":\"drain,trial,ckpt,io_wait,send_wait,disk_wait\",\"cpu\":\"unresolved\"",
		r->un_us, r->gap_us, r->drain_n, r->drain_pops, r->drain_us,
		r->trial_n, r->trial_pops, r->trial_us, r->ckpt_n, r->ckpt_us,
		r->wait_n[G9WT_W_IO], r->wait_us[G9WT_W_IO],
		r->wait_n[G9WT_W_SEND], r->wait_us[G9WT_W_SEND],
		r->wait_n[G9WT_W_DISK], r->wait_us[G9WT_W_DISK],
		r->rpc_pend_max, r->win_rpc_pend_max,
		r->win_rt_max, r->win_wt_max, r->notes_dropped);
	if(w < 0 || (size_t)w >= cap - off) return 0;
	off += (size_t)w;
	if(r->cpu_sampled) {
		w = snprintf(dst + off, cap - off, ",\"cpu_sample\":[");
		if(w < 0 || (size_t)w >= cap - off) return 0;
		off += (size_t)w;
		for(unsigned i = 0; i < G9WT_CPU_ROWS; i++) {
			const uint32_t* v = r->cpu_sample[i];
			w = snprintf(dst + off, cap - off, "%s[%u,%u,%u,%u,%u,%u]",
				i ? "," : "", v[0], v[1], v[2], v[3], v[4], v[5]);
			if(w < 0 || (size_t)w >= cap - off) return 0;
			off += (size_t)w;
		}
		if(off + 1 >= cap) return 0;
		dst[off++] = ']';
	}
	if(off + 1 >= cap) return 0;
	dst[off++] = '}'; dst[off] = '\0';
	return off;
}
