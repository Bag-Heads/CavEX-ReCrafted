/* Wii-only background network bring-up (S5.2). libogc's if_config blocks
 * up to the full DHCP settle (~25s measured on this rig, hw_telemetry
 * lore), so it may NEVER run on the main/render thread. This module runs
 * it on one lazily-created background LWP thread with WiiMC's proven
 * retry shape and publishes a state the connect screen polls per frame.
 *
 * Design source: docs/mp_s5_wii_transport_design_2026-08-26.md section 2
 * (re-implemented shape, no GPL code); the connect screen gates
 * mp_session_request_connect on READY so main.c's 15s provider-swap
 * deadline (MP_SWAP_DEADLINE_MS) never races DHCP -- section 2c option 1. */
#ifndef NET_BRINGUP_H
#define NET_BRINGUP_H

#include <stdbool.h>
#include <stddef.h>

enum net_bringup_state {
	NET_BRINGUP_NOT_STARTED,
	NET_BRINGUP_IN_PROGRESS,
	NET_BRINGUP_READY,
	NET_BRINGUP_FAILED,
};

/* Idempotent: starts the background ladder if it is not running and not
 * already READY. After a FAILED verdict, calling again restarts the
 * ladder (the connect screen's Retry path). */
void net_bringup_request(void);

/* Non-blocking. On FAILED, copies the last error into `error` when
 * provided. Also reaps the finished worker thread (prompt join) so its
 * 64 KB stack is reclaimed once a terminal state has been observed. */
enum net_bringup_state net_bringup_status(char* error, size_t error_size);

/* Advisory: the worker checks between attempts, not mid-if_config -- a
 * cancel can take up to one attempt's duration (~20s) to be honored.
 * Callers never wait on it (the screen just navigates away). */
void net_bringup_cancel(void);

/* C1 (candidate, hardware-unconfirmed, 2026-09-02): cancel + bounded-wait +
 * join the background worker, for process shutdown. net_bringup_cancel()
 * alone is not enough to shut down cleanly: the worker only samples the
 * cancel flag BETWEEN attempts, so it can still be blocked inside libogc's
 * if_config for up to ~25s (one DHCP settle) after cancel is requested --
 * far more than a shutdown path should ever block the loader-return. This
 * polls for the worker's own completion instead of joining blindly, and
 * gives up after a few seconds: past that bound the worker is abandoned
 * (thread_dispose(t, false) -- the same documented, bounded, on-purpose
 * leak shape as an abandoned resolver connect elsewhere in this module)
 * rather than risk hanging shutdown on a stuck if_config call. Returns true
 * if the worker was joined (or had never started / was already reaped),
 * false if it had to be abandoned still running. Idempotent and safe to
 * call when net_bringup_request() was never invoked. */
bool net_bringup_shutdown(void);

#endif
