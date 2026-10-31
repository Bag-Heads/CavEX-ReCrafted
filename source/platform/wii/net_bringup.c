/* Wii-only background network bring-up (S5.2). Included by
 * source/platform/net_bringup.c -- never compiled standalone (the
 * gfx/audio/net_socket dispatcher convention). See net_bringup.h for the
 * contract and the design-doc citation. */

#include "net_bringup.h"

#include "../thread.h"

#include <gccore.h>
#include <network.h>
#include <ogc/if_config.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
int usleep(useconds_t __useconds);

/* WiiMC's proven ladder shape (re-implemented): 5 outer attempts, 1s
 * backoff, net_deinit + net_wc24cleanup between failed attempts. Each
 * attempt is one synchronous if_config on THIS background thread -- the
 * project's proven primitive (hw_telemetry.c:683) -- whose own internal
 * DHCP retry count bounds it at roughly the ~25s measured settle.
 * Worst-case ladder ~= 5 x (attempt + 1s) before FAILED. */
#define NET_BRINGUP_ATTEMPTS 5
#define NET_BRINGUP_BACKOFF_MS 1000
#define NET_BRINGUP_DHCP_MAX_RETRIES 5

static struct thread_mutex s_lock;
static bool s_lock_ready;
static struct thread s_worker;
static bool s_worker_live;   /* thread created and not yet joined */
static bool s_worker_done;   /* thread reached its exit (join is prompt) */
static bool s_cancel;
static enum net_bringup_state s_state = NET_BRINGUP_NOT_STARTED;
static char s_error[96];
static char s_local_ip[16];

static void* net_bringup_thread(void* arg) {
	(void)arg;
	char err[96] = "network bring-up failed";
	bool ready = false;
	for(int attempt = 1; attempt <= NET_BRINGUP_ATTEMPTS; ++attempt) {
		thread_mutex_lock(&s_lock);
		bool cancelled = s_cancel;
		thread_mutex_unlock(&s_lock);
		if(cancelled) {
			snprintf(err, sizeof(err), "cancelled");
			break;
		}
		char ip[16] = {0};
		char netmask[16] = {0};
		char gateway[16] = {0};
		s32 result = if_config(ip, netmask, gateway, true,
							   NET_BRINGUP_DHCP_MAX_RETRIES);
		if(result >= 0 && ip[0]) {
			thread_mutex_lock(&s_lock);
			snprintf(s_local_ip, sizeof(s_local_ip), "%s", ip);
			thread_mutex_unlock(&s_lock);
			ready = true;
			break;
		}
		snprintf(err, sizeof(err), "if_config failed (%ld), attempt %d/%d",
				 (long)result, attempt, NET_BRINGUP_ATTEMPTS);
		fprintf(stderr, "[net_bringup] %s\n", err);
		if(attempt < NET_BRINGUP_ATTEMPTS) {
			/* WiiMC shape: tear the half-initialized stack down before the
			 * next try; libogc net_init tolerates a later restart (the -15
			 * "already started" case is success, network_wii.c:402). */
			net_deinit();
			net_wc24cleanup();
			usleep(NET_BRINGUP_BACKOFF_MS * 1000);
		}
	}
	thread_mutex_lock(&s_lock);
	if(ready) {
		s_state = NET_BRINGUP_READY;
	} else {
		snprintf(s_error, sizeof(s_error), "%s", err);
		s_state = NET_BRINGUP_FAILED;
	}
	s_worker_done = true;
	thread_mutex_unlock(&s_lock);
	return NULL;
}

/* Reap a finished worker under the lock (join is prompt once
 * s_worker_done). Never called for a mid-ladder worker. */
static void reap_locked(void) {
	if(s_worker_live && s_worker_done) {
		thread_join(&s_worker);
		s_worker_live = false;
		s_worker_done = false;
	}
}

void net_bringup_request(void) {
	if(!s_lock_ready) {
		if(!thread_mutex_init(&s_lock))
			return; /* status stays NOT_STARTED; the screen re-requests */
		s_lock_ready = true;
	}
	thread_mutex_lock(&s_lock);
	reap_locked();
	if(s_state == NET_BRINGUP_READY || s_worker_live) {
		thread_mutex_unlock(&s_lock);
		return;
	}
	/* Already up (armed telemetry ran its own boot-time if_config): READY
	 * without touching the stack. The S5 design doc section 2b flagged the
	 * double-bring-up interaction as hardware-unverified -- and the first
	 * mp_connect_smoke run (20260827_203028) showed exactly that failure
	 * shape: a second if_config over a live telemetry session stalled the
	 * handshake and starved the heartbeat stream (mc173 log: accept then
	 * silence). Never re-init a working stack. */
	if(net_gethostip() != 0) {
		s_state = NET_BRINGUP_READY;
		thread_mutex_unlock(&s_lock);
		return;
	}
	s_cancel = false;
	s_error[0] = '\0';
	/* Mesher-band priority (32): bring-up competes with menus, never with
	 * a live connection's read/write pair (S5 design doc section 3c). */
	if(thread_create(&s_worker, net_bringup_thread, NULL, 32)) {
		s_worker_live = true;
		s_worker_done = false;
		s_state = NET_BRINGUP_IN_PROGRESS;
	} else {
		snprintf(s_error, sizeof(s_error), "bring-up thread creation failed");
		s_state = NET_BRINGUP_FAILED;
	}
	thread_mutex_unlock(&s_lock);
}

enum net_bringup_state net_bringup_status(char* error, size_t error_size) {
	if(!s_lock_ready)
		return NET_BRINGUP_NOT_STARTED;
	thread_mutex_lock(&s_lock);
	reap_locked();
	enum net_bringup_state state = s_state;
	if(error && error_size)
		snprintf(error, error_size, "%s", s_error);
	thread_mutex_unlock(&s_lock);
	return state;
}

void net_bringup_cancel(void) {
	if(!s_lock_ready)
		return;
	thread_mutex_lock(&s_lock);
	s_cancel = true;
	thread_mutex_unlock(&s_lock);
}

/* C1 (candidate, hardware-unconfirmed): shutdown wait bound. Well under one
 * ladder attempt's worst case (~25s DHCP settle, see the header) -- past this
 * the worker is abandoned rather than risk stalling the loader-return on a
 * stuck if_config call. */
#define NET_BRINGUP_SHUTDOWN_WAIT_MS 3000
#define NET_BRINGUP_SHUTDOWN_POLL_MS 20

bool net_bringup_shutdown(void) {
	if(!s_lock_ready)
		return true; /* net_bringup_request() never ran: nothing to stop */

	net_bringup_cancel(); /* sampled at the next attempt boundary, not mid-call */

	/* Poll s_worker_done under the lock instead of joining blindly: the
	 * worker may still be parked inside if_config, well past the cancel
	 * flag it cannot see yet. */
	for(int waited = 0; waited < NET_BRINGUP_SHUTDOWN_WAIT_MS;
		waited += NET_BRINGUP_SHUTDOWN_POLL_MS) {
		thread_mutex_lock(&s_lock);
		bool done = !s_worker_live || s_worker_done;
		thread_mutex_unlock(&s_lock);
		if(done)
			break;
		thread_msleep(NET_BRINGUP_SHUTDOWN_POLL_MS);
	}

	thread_mutex_lock(&s_lock);
	bool joined;
	if(!s_worker_live) {
		joined = true; /* never started, or already reaped by status()/reap_locked */
	} else if(s_worker_done) {
		thread_join(&s_worker); /* prompt: the thread has already exited */
		s_worker_live = false;
		s_worker_done = false;
		joined = true;
	} else {
		/* Still bringing up the network past the bound -- abandon it rather
		 * than hang shutdown. Documented, bounded, on-purpose leak: same
		 * shape as an abandoned resolver connect elsewhere in the tree
		 * (thread.h's thread_dispose contract). */
		fprintf(stderr,
				"[net_bringup] shutdown: worker still bringing up network after "
				"%dms -- abandoning it\n",
				NET_BRINGUP_SHUTDOWN_WAIT_MS);
		thread_dispose(&s_worker, false);
		s_worker_live = false;
		joined = false;
	}
	thread_mutex_unlock(&s_lock);
	return joined;
}
