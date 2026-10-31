#ifndef G9_WORST_TICK_H
#define G9_WORST_TICK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* G9 R20 Q3 (ASTRA-G9-R20-CKPT): ONE bounded attribution probe.
 * debug.g9_worst_tick_int = 1 (strict 0|1, default 0). Per heartbeat window
 * the server thread retains ONE fixed-size record -- the tick with the
 * largest wt_us -- and MAIN emits it as ONE "g9_worst_tick" observer line at
 * the heartbeat, outside every timed span. Diagnostic only: no simulation
 * value, no ordering, no scheduling changes. OFF = one relaxed load per hook.
 *
 * Phase totals are elapsed (wall) time. With debug.g9_cpu_sampler_int=1,
 * cpu_sample adds 1-kHz occupancy estimates, NOT exact CPU/switch accounting;
 * IRQ execution remains unresolved. Nested rows must never be summed with
 * tick/wt. Without BOTH knobs, the existing record/observer stays unchanged.
 *
 * DISJOINT: ph_pre[] + ph_post[] (every watchdog srv phase, split at the tick
 * boost cap instant) partition [tick begin, tick end] exactly; gap_us is the
 * clock rounding left over (tick_us - sum). un_us = time under WDS_IDLE (no
 * named phase) = the unassigned residual.
 * NESTED (each is a subset of some phase above, never additive with it):
 * the light drains, the checkpoint trial, IO/barrier waits, the RPC
 * token-pool (blocked send) wait. */
#define G9WT_PHASES 23u /* == WDS_MAX (static-asserted in thread.c) */
#define G9WT_LINE_CAP 1600u /* includes the bounded disk-mutex attribution */

enum g9wt_flag {
	G9WT_F_COMPONENTS = 1u << 0, /* rt/wt were computed this tick (not loading) */
	G9WT_F_CKPT = 1u << 1,       /* a settled-trial checkpoint completed */
	G9WT_F_CKPT_BATCH = 1u << 2, /* ... while the save batch was OPEN */
	G9WT_F_AUTOSAVE = 1u << 3,   /* the autosave interval fired (level.dat) */
	G9WT_F_SWEEP = 1u << 4,      /* an autosave sweep step ran */
	G9WT_F_CAPPED = 1u << 5,     /* the tick boost cap fired inside the tick */
	G9WT_F_FOREIGN = 1u << 6,    /* a note arrived off the owner thread (dropped) */
};
/* DISK measures mutex acquisition elapsed time; IO measures completion and
 * backpressure waits. Both are nested wall spans, not exact CPU accounting. */
enum g9wt_wait { G9WT_W_IO = 0, G9WT_W_SEND = 1, G9WT_W_DISK = 2, G9WT_W_N };
enum g9wt_cpu_row {
	G9WT_CPU_TICK, G9WT_CPU_WT, G9WT_CPU_NORMAL, G9WT_CPU_TRIAL,
	G9WT_CPU_SNAPSHOT, G9WT_CPU_SAVE, G9WT_CPU_RESTORE, G9WT_CPU_ROWS
};
#define G9WT_CPU_WORDS 6u
/* [elapsed_us,server,runnable_offcpu,blocked,late,missed_deadlines].
 * Server wins when interrupted==owner; otherwise RUNNING && !suspend is
 * runnable, every other state is blocked. Late = dispatch >50us late.
 * Deadlines still overdue at a stage edge count as missed in the OLD rows:
 * no invented catch-up samples and no attribution across an uncertain edge. */

struct g9_worst_tick_rec {
	uint32_t seq;          /* window number (1, 2, ...) */
	uint32_t ticks;        /* ticks folded into this window */
	uint32_t tick;         /* the probe's tick serial of the retained tick */
	uint32_t world_time;   /* low 32 bits of s->world_time at the components point */
	uint32_t dim;          /* s->worlds[0].dimension */
	uint32_t flags;        /* enum g9wt_flag */
	uint32_t tick_us;      /* begin -> end of the timed tick (update + epilogue) */
	uint32_t rt_us, wt_us; /* the tick's own srv_rt_us / srv_wt_us */
	uint32_t wt_end_us;    /* offset of the components point from tick begin */
	uint32_t cap_us;       /* offset of the cap instant from tick begin; UINT32_MAX = none */
	uint32_t ph_pre[G9WT_PHASES], ph_post[G9WT_PHASES];
	uint32_t un_us, gap_us;
	uint32_t drain_n, drain_pops, drain_us;  /* nested: normal drains */
	uint32_t trial_n, trial_pops, trial_us;  /* nested: checkpoint trial drain */
	uint32_t ckpt_n, ckpt_us;                /* nested: whole checkpoint */
	uint32_t wait_n[G9WT_W_N], wait_us[G9WT_W_N]; /* nested */
	uint32_t rpc_pend_max;           /* the tick's peak pending-output ring occupancy */
	uint32_t win_rt_max, win_wt_max; /* window maxima over EVERY tick */
	uint32_t win_rpc_pend_max;       /* ... and of ring occupancy */
	uint32_t notes_dropped;          /* off-thread / inactive notes this window */
	uint32_t cpu_sampled; /* field present only when both knobs enabled */
	uint32_t cpu_sample[G9WT_CPU_ROWS][G9WT_CPU_WORDS];
};

/* MAIN, boot, before the server thread starts. */
void g9_worst_tick_configure(bool enabled);
bool g9_worst_tick_enabled(void);
/* Server thread (the timed tick bracket, server_local_thread). */
void g9_worst_tick_begin(void);
void g9_worst_tick_end(void);
/* The two wt brackets, excluding the random and intervening entity passes. */
void g9_worst_tick_wt(bool begin);
/* Temporarily exclude a random pass, preserving its enclosing wt bracket. */
void g9_worst_tick_random(bool begin);
/* Server thread, inside the tick. */
void g9_worst_tick_phase(uint32_t phase); /* from watchdog_phase_srv */
void g9_worst_tick_components(uint32_t rt_us, uint32_t wt_us,
	uint32_t world_time, uint32_t dim);
void g9_worst_tick_flag(uint32_t flag);
void g9_worst_tick_drain(bool trial, uint32_t pops, uint32_t us);
void g9_worst_tick_ckpt(uint32_t us, bool batch_open);
/* Server thread, clin_rpc_send: ring occupancy (entries; a block-update
 * pack is one entry) after a deferral (R20 Q3 follow-up; G9 R21). */
void g9_worst_tick_pending(uint32_t occupancy);
/* A wait span: start = g9_worst_tick_wait_begin() (0 = not armed), then
 * g9_worst_tick_wait_end(kind, start). wait_us() adds an already-timed span. */
uint32_t g9_worst_tick_wait_begin(void);
void g9_worst_tick_wait_end(enum g9wt_wait kind, uint32_t start);
void g9_worst_tick_wait_us(enum g9wt_wait kind, uint32_t us);
/* MAIN, once per heartbeat: true (and the record) when a completed window is
 * ready; false when the server has not closed one since the last take. */
bool g9_worst_tick_take(struct g9_worst_tick_rec* out);
/* The one observer line (returns snprintf's length; 0 on failure). */
size_t g9_worst_tick_format(const struct g9_worst_tick_rec* r,
	char* dst, size_t cap);
/* Host tests only: replace the microsecond clock (NULL = native); fire the
 * tick boost's hard cap now (PC has no cap timer; not built on the Wii). */
void g9_worst_tick_test_clock(uint32_t (*now_us)(void));
void g9_worst_tick_test_cap(void);
#endif
