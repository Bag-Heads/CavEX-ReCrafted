/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef PLATFORM_SERVICE_OBSERVER_H
#define PLATFORM_SERVICE_OBSERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "time.h"

/* Defined weak forwarding seams keep focused backend-only harnesses linkable
 * on PE/MinGW too (undefined weak data/function references break its linker).
 * playable app directly links this TU and configures it before producers start.
 * OFF performs no clock read or argument evaluation. No 64-bit atomics on Wii. */
extern uint32_t service_observer_on;
#define SERVICE_OBSERVER(call) do { \
	if(__atomic_load_n(&service_observer_on, __ATOMIC_RELAXED)) { call; } \
} while(0)
#define SO_BINS 8u
#define SO_CONTEXT_WORDS 20u
#define SO_LANES 4u
#define SO_PHASES 23u
#define SO_METRICS 39u
#define SO_FINAL_TIMEOUT_US 1000000u
enum so_state { SO_STARTUP, SO_LOADING, SO_GAMEPLAY, SO_MENU, SO_SHUTDOWN, SO_STATES };
enum so_metric { SO_FRAME_GAP, SO_POLL_GAP, SO_POLL_US, SO_CONSUME_AGE,
	SO_REUSE_AGE, SO_AUDIO_GAP, SO_AUDIO_RESERVE, SO_PRESENT_AGE,
	SO_TICK_US, SO_RT_US, SO_WT_US, SO_BOOST_US, SO_PHASE_BASE,
	SO_WAIT_BASE = 35, SO_CAP_US = 38 };
enum so_audio_event { SO_AUDIO_SERVICE, SO_AUDIO_PRIME, SO_AUDIO_UNDERRUN,
	SO_AUDIO_STOP, SO_AUDIO_REPRIME, SO_AUDIO_DECODE_UNDERRUN,
	SO_AUDIO_RESET, SO_AUDIO_RATE_CHANGE, SO_AUDIO_TRACK_STOP, SO_AUDIO_SHUTDOWN,
	SO_AUDIO_EVENTS };
enum so_context_kind { SO_STATE_EDGE = 1, SO_TICK_EDGE, SO_BOOST_EDGE,
	SO_CAP_EDGE, SO_CHECKPOINT_EDGE, SO_AUDIO_EDGE, SO_INPUT_EDGE, SO_DISPLAY_EDGE };

void service_observer_configure(bool on, const char* build, uint32_t config_id);
bool service_observer_enabled(void);
uint64_t service_observer_now(void);
uint64_t service_observer_at(ptime_t t);
void service_observer_frame(enum so_state state);
void service_observer_state(enum so_state state);
void service_observer_poll(bool begin);
/* Legacy API name: v2 stamps driver poll drain/PAD scan, NOT report arrival.
 * These ages cannot prove receipt-to-consumer or physical input latency. */
void service_observer_receipt(unsigned device, bool connected, bool fresh);
void service_observer_consume(unsigned owner, unsigned device, unsigned action);
void service_observer_input(unsigned owner, unsigned action, const void* owners);
void service_observer_input_frame(const void* frame, unsigned action, const void* owners);
void service_observer_device(unsigned owner, unsigned action, const void* device);
void service_observer_audio(enum so_audio_event event, bool active, uint32_t reserve);
void service_observer_decode_underruns(uint32_t cumulative);
void service_observer_tick(bool begin);
void service_observer_components(uint32_t rt, uint32_t wt);
void service_observer_phase(unsigned phase, uint64_t at);
void service_observer_wait(unsigned kind, uint64_t begin, uint64_t end);
void service_observer_boost(unsigned event, uint32_t actual_us);
uint32_t service_observer_config_hash(const void* root);
void service_observer_checkpoint(uint32_t generation, uint32_t token,
	uint32_t logical_tick, uint32_t state, uint32_t previous);
void service_observer_submit(uintptr_t framebuffer);
void service_observer_submit_safe(uintptr_t framebuffer, uintptr_t scanned);
void service_observer_display(uintptr_t framebuffer);
void service_observer_proxy(void);
/* Quiescent test adapter only. Production MAIN publishes request(), and the
 * existing sender calls pump(): one bounded packet per call, <=1 aggregate
 * batch/s, with retained context drained between batches before rings fill.
 * Clean refusal retains the serialized packet and every consumption cursor;
 * only confirmed delivery advances them. Final timeout is 1 s from freeze,
 * checked once per sender call; an in-flight IOS call is not cancellable.
 * Final=true freezes at profile_done admission (or ordinary teardown when
 * there is no profile). Profile rows do not cover post-completion teardown.
 * Disables future hooks. The tick interval ends at the last completed tick;
 * a trailing partial tick is excluded, while real gaps remain losses. */
void service_observer_flush(bool final, bool (*emit)(const char*));
void service_observer_request(bool final);
bool service_observer_pump(bool (*emit)(const char*));
bool service_observer_hold_transport(void);
/* Sender-only: false emit means clean refusal/defer. A transport that has
 * attempted bytes must abort on failure rather than retry ambiguous TCP. */
void service_observer_transport_abort(void);
bool service_observer_transport_failed(void);
size_t service_observer_storage_bytes(void);
void service_observer_test_clock(uint64_t (*clock_us)(void));
void service_observer_test_metric_busy(unsigned metric, bool busy);
unsigned service_observer_bin(uint32_t us);
#ifndef SERVICE_OBSERVER_IMPLEMENTATION
/* Production's strong definitions override these no-ops. A focused harness
 * that omits this diagnostic TU has defined OFF storage, never weak NULLs. */
__attribute__((weak)) uint32_t service_observer_on = 0;
#define SO_FALLBACK __attribute__((weak))
SO_FALLBACK uint64_t service_observer_now(void) { return 0; }
SO_FALLBACK uint64_t service_observer_at(ptime_t t) { (void)t; return 0; }
SO_FALLBACK void service_observer_frame(enum so_state state) { (void)state; }
SO_FALLBACK void service_observer_state(enum so_state state) { (void)state; }
SO_FALLBACK void service_observer_poll(bool begin) { (void)begin; }
SO_FALLBACK void service_observer_receipt(unsigned d, bool c, bool f) { (void)d; (void)c; (void)f; }
SO_FALLBACK void service_observer_consume(unsigned o, unsigned d, unsigned a) { (void)o; (void)d; (void)a; }
SO_FALLBACK void service_observer_input(unsigned o, unsigned a, const void* p) { (void)o; (void)a; (void)p; }
SO_FALLBACK void service_observer_input_frame(const void* f, unsigned a, const void* p) { (void)f; (void)a; (void)p; }
SO_FALLBACK void service_observer_device(unsigned o, unsigned a, const void* d) { (void)o; (void)a; (void)d; }
SO_FALLBACK void service_observer_audio(enum so_audio_event e, bool a, uint32_t r) { (void)e; (void)a; (void)r; }
SO_FALLBACK void service_observer_decode_underruns(uint32_t n) { (void)n; }
SO_FALLBACK void service_observer_tick(bool begin) { (void)begin; }
SO_FALLBACK void service_observer_components(uint32_t r, uint32_t w) { (void)r; (void)w; }
SO_FALLBACK void service_observer_phase(unsigned p, uint64_t t) { (void)p; (void)t; }
SO_FALLBACK void service_observer_wait(unsigned k, uint64_t b, uint64_t e) { (void)k; (void)b; (void)e; }
SO_FALLBACK void service_observer_boost(unsigned e, uint32_t t) { (void)e; (void)t; }
SO_FALLBACK void service_observer_checkpoint(uint32_t g, uint32_t t, uint32_t l, uint32_t s, uint32_t p) { (void)g; (void)t; (void)l; (void)s; (void)p; }
SO_FALLBACK void service_observer_submit(uintptr_t f) { (void)f; }
SO_FALLBACK void service_observer_submit_safe(uintptr_t f, uintptr_t v) { (void)f; (void)v; }
SO_FALLBACK void service_observer_display(uintptr_t f) { (void)f; }
SO_FALLBACK void service_observer_proxy(void) { }
SO_FALLBACK void service_observer_request(bool final) { (void)final; }
SO_FALLBACK bool service_observer_pump(bool (*emit)(const char*)) { (void)emit; return false; }
SO_FALLBACK bool service_observer_hold_transport(void) { return false; }
SO_FALLBACK void service_observer_transport_abort(void) { }
SO_FALLBACK bool service_observer_transport_failed(void) { return false; }
#undef SO_FALLBACK
#endif
#endif
