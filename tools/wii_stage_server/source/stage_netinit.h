#ifndef CAVEWII_STAGE_NETINIT_H
#define CAVEWII_STAGE_NETINIT_H

/* Bounded network bring-up policy for the Wii stage server (main.c) and its
 * PC unit test (tests/test_stage_netinit.c). Pure, header-only (static
 * inline), zero I/O -- libogc is injected through cw_stage_netinit_ops so
 * the PC test drives the SAME state machine main.c compiles, against a fake
 * libogc that can reproduce the deadlock this file exists to escape.
 *
 * ------------------------------------------------------------------------
 * WHY THIS EXISTS -- "the stage server never reaches listen()"
 * ------------------------------------------------------------------------
 * Field symptom (2026-09-02, c0877b2/506c4ad): after some wiiload pushes the
 * stage server never opens :39019 at all. The PC's CWHELLO1 gets no TCP
 * connect for MINUTES; re-pushing the same .dol fixes it. configure_network()
 * printed "network attempt 1/30..." and never printed attempt 2, i.e. the
 * very first if_config() call never returned.
 *
 * Root cause is in libogc itself (v3.1.0 libogc/network_wii.c), and it is a
 * genuine unbounded wait, not a slow DHCP:
 *
 *   s32 net_init(void) {                             // line 570
 *       struct init_default_cb data;
 *       if (net_ip_top_fd >= 0) return 0;
 *       LWP_InitQueue(&data.queue);
 *       net_init_async((netcallback)net_init_callback, &data);  // rc IGNORED
 *       LWP_ThreadSleep(data.queue);                 // <-- NO TIMEOUT
 *       LWP_CloseQueue(data.queue);
 *       return data.result;                          // UNINITIALISED if never woken
 *   }
 *
 * `if_config()` -> `if_configex()` -> `net_init()`, so a caller of the
 * blocking API has no way to bound it. Two paths leave that sleep unwoken
 * forever:
 *
 *   (1) net_init_async() has SIX early returns that never invoke the
 *       callback -- `_init_busy` (-EBUSY), NetCreateHeap failure,
 *       iosCreateHeap failure, malloc failure, SYS_CreateAlarm failure and
 *       memalign failure (network_wii.c 511-556). net_init() ignores that
 *       return value and sleeps on a queue nobody will ever broadcast.
 *   (2) The async chain's state 1 retries an IPC_ENOENT open of
 *       /dev/net/ncd/manage by re-arming a 100 ms alarm and going back to
 *       state 0 -- with NO retry counter (network_wii.c 293-306), unlike
 *       states 7 and 10, which both have one. An IOS network stack that is
 *       not up yet (a cold power cycle; a predecessor process that still
 *       holds the manage fd, which is exactly the state a wiiload push
 *       leaves behind) keeps answering IPC_ENOENT, so the chain spins on
 *       that alarm forever, `_init_busy` stays true, and net_init() never
 *       returns.
 *
 * Escape hatch: net_deinit() sets `_init_abort` and spins until the chain
 * clears `_init_busy` (network_wii.c 588-600), and net_init_chain() tests
 * `_init_abort` at its very top on EVERY re-entry -- including the 100 ms
 * alarm re-entry of case (2). So an aborted chain unwinds within roughly one
 * alarm period. That is what makes a deadline possible at all, and it is why
 * abort() is called before EVERY retry below: without it, a leftover
 * `_init_busy` turns every subsequent attempt into case (1).
 *
 * ------------------------------------------------------------------------
 * THE CONTRACT THIS HEADER ENFORCES
 * ------------------------------------------------------------------------
 *   1. The blocking net_init()/if_config() entry point is never the FIRST
 *      thing called. Bring-up is driven through net_init_async() + a poll,
 *      so the wait has a wall-clock deadline the caller owns.
 *   2. Every attempt that does not finish -- deadline expired, begin()
 *      refused, or the chain reported an error -- is followed by abort()
 *      before the next attempt, so no attempt inherits a wedged chain.
 *   3. The whole routine is bounded by
 *      max_attempts * (attempt_timeout_ms + retry_delay_ms), with no path
 *      that can wait forever. Failure is REPORTED (and, in main.c, written
 *      to the boot trace) rather than presenting as a silent hang with no
 *      artifact -- the thing that made this fault undiagnosable in the
 *      field for a whole session.
 */

#ifndef CW_STAGE_NETINIT_PENDING
/* poll() return meaning "the bring-up chain is still in flight". Chosen
 * positive so it can never collide with a libogc result, which is 0 on
 * success and negative errno otherwise. */
#define CW_STAGE_NETINIT_PENDING 1
#endif

enum cw_stage_netinit_status {
	CW_STAGE_NETINIT_OK = 0,
	/* begin() refused on every attempt (libogc: -EBUSY from a chain that is
	 * still wedged, or a heap/alarm allocation failure). */
	CW_STAGE_NETINIT_REFUSED = 1,
	/* Every attempt hit its deadline with the chain still pending -- the
	 * exact shape of the field hang this header exists to bound. */
	CW_STAGE_NETINIT_TIMEOUT = 2,
	/* The chain completed, with an error, on every attempt. */
	CW_STAGE_NETINIT_FAILED = 3,
	CW_STAGE_NETINIT_ARGS = 4
};

struct cw_stage_netinit_ops {
	/* Start (or re-start) bring-up. Return < 0 to refuse -- the attempt is
	 * abandoned, abort()ed and retried. Return >= 0 to mean "either the
	 * chain is running or the stack was already up"; poll() decides which,
	 * because libogc's net_init_async() returns 0 for BOTH and only calls
	 * the callback in one of them. */
	int (*begin)(void* user);
	/* CW_STAGE_NETINIT_PENDING while in flight, 0 once the stack is up, a
	 * negative libogc result once the chain has failed. */
	int (*poll)(void* user);
	/* Cancel an in-flight chain and release its fds (libogc: net_deinit()).
	 * Must be safe to call when nothing is in flight. Required, not
	 * optional -- rule 2 above depends on it. */
	void (*abort)(void* user);
	/* Free-running millisecond counter. Wraparound-safe: only unsigned
	 * differences are ever taken. */
	unsigned (*now_ms)(void* user);
	void (*sleep_ms)(void* user, unsigned ms);
};

struct cw_stage_netinit_report {
	int attempts;		 /* attempts actually made (1-based, 0 if none) */
	int timeouts;		 /* attempts that hit their deadline */
	int refusals;		 /* attempts begin() refused */
	int failures;		 /* attempts whose chain reported an error */
	int last_error;		 /* last negative result seen from begin()/poll() */
	unsigned elapsed_ms; /* now_ms() delta across the whole routine */
};

static inline int cw_stage_netinit_run(const struct cw_stage_netinit_ops* ops,
									   void* user, int max_attempts,
									   unsigned attempt_timeout_ms,
									   unsigned poll_interval_ms,
									   unsigned retry_delay_ms,
									   struct cw_stage_netinit_report* rep) {
	struct cw_stage_netinit_report local;
	unsigned started;
	int attempt;
	int status = CW_STAGE_NETINIT_TIMEOUT;

	if(!rep)
		rep = &local;
	rep->attempts = 0;
	rep->timeouts = 0;
	rep->refusals = 0;
	rep->failures = 0;
	rep->last_error = 0;
	rep->elapsed_ms = 0;

	if(!ops || !ops->begin || !ops->poll || !ops->abort || !ops->now_ms ||
	   !ops->sleep_ms || max_attempts <= 0)
		return CW_STAGE_NETINIT_ARGS;

	started = ops->now_ms(user);

	for(attempt = 1; attempt <= max_attempts; ++attempt) {
		unsigned attempt_started;
		int begun;
		rep->attempts = attempt;

		begun = ops->begin(user);
		if(begun < 0) {
			rep->refusals++;
			rep->last_error = begun;
			status = CW_STAGE_NETINIT_REFUSED;
			/* A refusal is usually libogc's -EBUSY, i.e. a PREVIOUS chain is
			 * still wedged. Aborting is the only thing that can clear it, so
			 * this is the one case where abort() is load-bearing rather than
			 * hygiene. */
			ops->abort(user);
			if(attempt < max_attempts && retry_delay_ms)
				ops->sleep_ms(user, retry_delay_ms);
			continue;
		}

		attempt_started = ops->now_ms(user);
		for(;;) {
			int r = ops->poll(user);
			if(r == CW_STAGE_NETINIT_PENDING) {
				/* Unsigned difference: correct across a now_ms() wrap. */
				if((unsigned)(ops->now_ms(user) - attempt_started) >=
				   attempt_timeout_ms) {
					rep->timeouts++;
					status = CW_STAGE_NETINIT_TIMEOUT;
					ops->abort(user);
					break;
				}
				if(poll_interval_ms)
					ops->sleep_ms(user, poll_interval_ms);
				continue;
			}
			if(r == 0) {
				rep->elapsed_ms = (unsigned)(ops->now_ms(user) - started);
				return CW_STAGE_NETINIT_OK;
			}
			rep->failures++;
			rep->last_error = r;
			status = CW_STAGE_NETINIT_FAILED;
			ops->abort(user);
			break;
		}

		if(attempt < max_attempts && retry_delay_ms)
			ops->sleep_ms(user, retry_delay_ms);
	}

	rep->elapsed_ms = (unsigned)(ops->now_ms(user) - started);
	return status;
}

#endif /* CAVEWII_STAGE_NETINIT_H */
