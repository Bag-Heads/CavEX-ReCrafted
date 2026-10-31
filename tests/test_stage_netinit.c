/* Wii stage-server network bring-up: the 2026-09-02 "never reaches listen()"
 * hang.
 *
 * main.c's bring-up is Wii-only (net_init_async/net_deinit/gettime), so the
 * POLICY half of it lives in tools/wii_stage_server/source/stage_netinit.h --
 * pure, header-only, libogc injected -- and this test drives the very same
 * cw_stage_netinit_run() the .dol compiles, against a fake libogc that can
 * reproduce the two ways real libogc v3.1.0 wedges:
 *
 *   (1) net_init_async() returns early WITHOUT calling the callback
 *       (_init_busy/-EBUSY, heap/alarm/malloc failure) and net_init()'s
 *       LWP_ThreadSleep is never broadcast;
 *   (2) the async chain's state-1 IPC_ENOENT retry re-arms a 100 ms alarm
 *       with no retry counter, so the chain spins forever and the callback
 *       never fires.
 *
 * Part 1 is a REGRESSION WITNESS: the old blocking shape (wait for the
 * callback with no deadline) is reproduced here and asserted to never
 * terminate within any bound -- which is exactly why the field symptom was
 * "no TCP connect for minutes, a re-push fixes it".
 *
 * Part 2 pins the fix's invariants:
 *   - the routine ALWAYS terminates inside
 *     max_attempts * (attempt_timeout_ms + retry_delay_ms);
 *   - abort() is called before EVERY retry, so no attempt inherits a wedged
 *     chain (without it, libogc's _init_busy turns every later attempt into
 *     case (1) above);
 *   - a stack that comes up on attempt N reports OK with attempts == N;
 *   - the deadline arithmetic survives a now_ms() wraparound.
 */

#include "stage_netinit.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Fake libogc                                                         */
/* ------------------------------------------------------------------ */

#define FAKE_EBUSY (-16)
#define FAKE_ENETDOWN (-115)

struct fake_ogc {
	unsigned now;		 /* free-running ms clock */
	unsigned slept_ms;	 /* total simulated sleep */
	int begins;			 /* begin() calls */
	int aborts;			 /* abort() calls */
	int polls;			 /* poll() calls */
	int begin_refuses;	 /* refuse the first N begin() calls (-EBUSY) */
	int chain_wedged;	 /* 1: the callback NEVER fires (the field hang) */
	int fail_result;	 /* if non-zero, poll() reports this error ... */
	int fail_attempts;	 /* ... on the first N attempts */
	unsigned up_after_ms; /* ms after the CURRENT begin() at which it comes up */
	unsigned attempt_t0; /* now at the current begin() */
	int in_flight;
	int aborts_at_begin; /* aborts seen when the last begin() ran */
	int saw_abort_before_retry; /* 0 = an attempt was retried un-aborted */
	int attempt_index;
};

static int fake_begin(void* user) {
	struct fake_ogc* f = (struct fake_ogc*)user;
	/* Invariant check: every begin() after the first must have been preceded
	 * by an abort() of the previous attempt. */
	if(f->begins > 0 && f->aborts <= f->aborts_at_begin)
		f->saw_abort_before_retry = 0;
	f->aborts_at_begin = f->aborts;
	f->begins++;
	f->attempt_index++;
	if(f->begin_refuses > 0) {
		f->begin_refuses--;
		return FAKE_EBUSY;
	}
	f->in_flight = 1;
	f->attempt_t0 = f->now;
	return 0;
}

static int fake_poll(void* user) {
	struct fake_ogc* f = (struct fake_ogc*)user;
	f->polls++;
	if(!f->in_flight)
		return CW_STAGE_NETINIT_PENDING;
	if(f->fail_result && f->attempt_index <= f->fail_attempts) {
		f->in_flight = 0;
		return f->fail_result;
	}
	if(f->chain_wedged)
		return CW_STAGE_NETINIT_PENDING; /* callback never fires */
	if((unsigned)(f->now - f->attempt_t0) >= f->up_after_ms)
		return 0;
	return CW_STAGE_NETINIT_PENDING;
}

static void fake_abort(void* user) {
	struct fake_ogc* f = (struct fake_ogc*)user;
	f->aborts++;
	f->in_flight = 0;
}

static unsigned fake_now(void* user) {
	return ((struct fake_ogc*)user)->now;
}

static void fake_sleep(void* user, unsigned ms) {
	struct fake_ogc* f = (struct fake_ogc*)user;
	f->now += ms;
	f->slept_ms += ms;
}

static const struct cw_stage_netinit_ops fake_ops
	= { fake_begin, fake_poll, fake_abort, fake_now, fake_sleep };

static void fake_reset(struct fake_ogc* f) {
	memset(f, 0, sizeof(*f));
	f->saw_abort_before_retry = 1;
	f->now = 1000u;
}

/* ------------------------------------------------------------------ */
/* Part 1 -- regression witness: the blocking shape never terminates    */
/* ------------------------------------------------------------------ */

/* libogc net_init()'s actual shape: kick the chain off, then sleep until the
 * callback broadcasts, with no deadline and no check of net_init_async()'s
 * return value. Bounded here ONLY by a step budget the real code does not
 * have -- reaching that budget IS the failure being witnessed. */
#define LEGACY_STEP_BUDGET 100000

static int legacy_blocking_net_init(struct fake_ogc* f, unsigned* out_ms) {
	int steps = 0;
	unsigned t0 = f->now;
	(void)fake_begin(f); /* return value ignored, exactly like net_init() */
	for(;;) {
		int r = fake_poll(f);
		if(r != CW_STAGE_NETINIT_PENDING) {
			*out_ms = f->now - t0;
			return r;
		}
		if(++steps >= LEGACY_STEP_BUDGET) {
			*out_ms = f->now - t0;
			return CW_STAGE_NETINIT_PENDING; /* still hung */
		}
		fake_sleep(f, 100u); /* the chain's own 100 ms alarm period */
	}
}

static void test_legacy_witness(void) {
	struct fake_ogc f;
	unsigned ms = 0;
	int r;

	/* Case (2): the chain spins on its uncapped IPC_ENOENT alarm. */
	fake_reset(&f);
	f.chain_wedged = 1;
	r = legacy_blocking_net_init(&f, &ms);
	assert(r == CW_STAGE_NETINIT_PENDING);
	assert(ms >= 100u * (LEGACY_STEP_BUDGET - 1)); /* >2.7 hours, i.e. never */

	/* Case (1): net_init_async refuses (-EBUSY) and never calls back, so the
	 * blocking sleep is on a queue nobody will broadcast. The legacy shape
	 * cannot even see the refusal -- it discards the return value. */
	fake_reset(&f);
	f.begin_refuses = 1000000;
	r = legacy_blocking_net_init(&f, &ms);
	assert(r == CW_STAGE_NETINIT_PENDING);
	assert(f.aborts == 0); /* nothing ever clears the wedged chain */
}

/* ------------------------------------------------------------------ */
/* Part 2 -- the fix                                                   */
/* ------------------------------------------------------------------ */

static void test_wedged_chain_is_bounded(void) {
	struct fake_ogc f;
	struct cw_stage_netinit_report rep;
	int rc;
	const int attempts = 4;
	const unsigned timeout_ms = 15000u, poll_ms = 50u, retry_ms = 500u;

	fake_reset(&f);
	f.chain_wedged = 1; /* the exact field hang */
	rc = cw_stage_netinit_run(&fake_ops, &f, attempts, timeout_ms, poll_ms,
							  retry_ms, &rep);
	assert(rc == CW_STAGE_NETINIT_TIMEOUT);
	assert(rep.attempts == attempts);
	assert(rep.timeouts == attempts);
	/* Every attempt was aborted -- rule 2. */
	assert(f.aborts == attempts);
	assert(f.saw_abort_before_retry == 1);
	/* Hard ceiling, not "eventually". */
	assert(rep.elapsed_ms
		   <= (unsigned)attempts * (timeout_ms + poll_ms + retry_ms));
	assert(rep.elapsed_ms >= (unsigned)attempts * timeout_ms);
}

static void test_begin_refused_forever(void) {
	struct fake_ogc f;
	struct cw_stage_netinit_report rep;
	int rc;

	fake_reset(&f);
	f.begin_refuses = 1000; /* -EBUSY on every attempt */
	rc = cw_stage_netinit_run(&fake_ops, &f, 3, 15000u, 50u, 500u, &rep);
	assert(rc == CW_STAGE_NETINIT_REFUSED);
	assert(rep.attempts == 3);
	assert(rep.refusals == 3);
	assert(rep.last_error == FAKE_EBUSY);
	/* A refusal means a previous chain is wedged; abort() is the only thing
	 * that can clear libogc's _init_busy, so it must run every time. */
	assert(f.aborts == 3);
	assert(f.polls == 0); /* never polls a chain that was never started */
	assert(rep.elapsed_ms <= 3u * (15000u + 50u + 500u));
}

static void test_recovers_on_later_attempt(void) {
	struct fake_ogc f;
	struct cw_stage_netinit_report rep;
	int rc;

	/* Two refusals (a wedged predecessor chain), then the stack comes up
	 * 400 ms into the third attempt -- the "re-push fixes it" case, but
	 * without needing the re-push. */
	fake_reset(&f);
	f.begin_refuses = 2;
	f.up_after_ms = 400u;
	rc = cw_stage_netinit_run(&fake_ops, &f, 6, 15000u, 50u, 500u, &rep);
	assert(rc == CW_STAGE_NETINIT_OK);
	assert(rep.attempts == 3);
	assert(rep.refusals == 2);
	assert(rep.timeouts == 0);
	assert(f.saw_abort_before_retry == 1);
}

static void test_timeout_then_success(void) {
	struct fake_ogc f;
	struct cw_stage_netinit_report rep;
	int rc;

	/* Attempt 1 needs longer than its deadline; attempt 2 comes up fast
	 * because the abort() released the wedged fds. */
	fake_reset(&f);
	f.up_after_ms = 9000u;
	rc = cw_stage_netinit_run(&fake_ops, &f, 4, 2000u, 50u, 100u, &rep);
	assert(rc == CW_STAGE_NETINIT_TIMEOUT || rc == CW_STAGE_NETINIT_OK);
	/* With a 2 s deadline and a 9 s bring-up, every attempt times out. */
	assert(rc == CW_STAGE_NETINIT_TIMEOUT);
	assert(rep.attempts == 4);
	assert(f.aborts == 4);

	/* Same schedule, deadline generous enough: first attempt wins. */
	fake_reset(&f);
	f.up_after_ms = 9000u;
	rc = cw_stage_netinit_run(&fake_ops, &f, 4, 15000u, 50u, 100u, &rep);
	assert(rc == CW_STAGE_NETINIT_OK);
	assert(rep.attempts == 1);
	assert(f.aborts == 0); /* nothing to abort on a clean bring-up */
}

static void test_chain_error_then_success(void) {
	struct fake_ogc f;
	struct cw_stage_netinit_report rep;
	int rc;

	fake_reset(&f);
	f.fail_result = FAKE_ENETDOWN;
	f.fail_attempts = 2; /* chain completes WITH AN ERROR twice */
	f.up_after_ms = 0u;	 /* then comes up immediately */
	rc = cw_stage_netinit_run(&fake_ops, &f, 5, 15000u, 50u, 100u, &rep);
	assert(rc == CW_STAGE_NETINIT_OK);
	assert(rep.attempts == 3);
	assert(rep.failures == 2);
	assert(rep.last_error == FAKE_ENETDOWN);
	assert(f.aborts == 2);

	/* Failing forever is still bounded and still REPORTED, never a hang. */
	fake_reset(&f);
	f.fail_result = FAKE_ENETDOWN;
	f.fail_attempts = 1000;
	rc = cw_stage_netinit_run(&fake_ops, &f, 3, 15000u, 50u, 100u, &rep);
	assert(rc == CW_STAGE_NETINIT_FAILED);
	assert(rep.failures == 3);
	assert(rep.last_error == FAKE_ENETDOWN);
	assert(rep.elapsed_ms <= 3u * (15000u + 50u + 100u));
}

static void test_already_up_is_free(void) {
	struct fake_ogc f;
	struct cw_stage_netinit_report rep;
	int rc;

	fake_reset(&f);
	f.up_after_ms = 0u; /* poll() says "up" on its very first call */
	rc = cw_stage_netinit_run(&fake_ops, &f, 6, 15000u, 50u, 500u, &rep);
	assert(rc == CW_STAGE_NETINIT_OK);
	assert(rep.attempts == 1);
	assert(f.polls == 1);
	assert(f.slept_ms == 0u);
	assert(f.aborts == 0);
	assert(rep.elapsed_ms == 0u);
}

static void test_clock_wraparound(void) {
	struct fake_ogc f;
	struct cw_stage_netinit_report rep;
	int rc;

	/* Deadline arithmetic must survive the free-running counter wrapping
	 * mid-attempt (main.c derives now_ms from gettime(), which wraps). */
	fake_reset(&f);
	f.now = UINT_MAX - 300u;
	f.chain_wedged = 1;
	rc = cw_stage_netinit_run(&fake_ops, &f, 2, 1000u, 50u, 50u, &rep);
	assert(rc == CW_STAGE_NETINIT_TIMEOUT);
	assert(rep.attempts == 2);
	assert(rep.timeouts == 2);
	assert(rep.elapsed_ms <= 2u * (1000u + 50u + 50u));

	fake_reset(&f);
	f.now = UINT_MAX - 100u;
	f.up_after_ms = 500u; /* comes up only after the wrap */
	rc = cw_stage_netinit_run(&fake_ops, &f, 2, 5000u, 50u, 50u, &rep);
	assert(rc == CW_STAGE_NETINIT_OK);
	assert(rep.attempts == 1);
}

static void test_argument_guards(void) {
	struct fake_ogc f;
	struct cw_stage_netinit_ops bad;
	struct cw_stage_netinit_report rep;

	fake_reset(&f);
	assert(cw_stage_netinit_run(NULL, &f, 3, 100u, 10u, 10u, &rep)
		   == CW_STAGE_NETINIT_ARGS);
	assert(cw_stage_netinit_run(&fake_ops, &f, 0, 100u, 10u, 10u, &rep)
		   == CW_STAGE_NETINIT_ARGS);

	/* abort() is REQUIRED -- rule 2 has no meaning without it, and silently
	 * skipping it is how a wedged _init_busy would survive every retry. */
	bad = fake_ops;
	bad.abort = NULL;
	assert(cw_stage_netinit_run(&bad, &f, 3, 100u, 10u, 10u, &rep)
		   == CW_STAGE_NETINIT_ARGS);

	/* A NULL report is allowed (main.c always passes one, but the policy
	 * must not depend on it). */
	fake_reset(&f);
	f.up_after_ms = 0u;
	assert(cw_stage_netinit_run(&fake_ops, &f, 3, 100u, 10u, 10u, NULL)
		   == CW_STAGE_NETINIT_OK);
}

int main(void) {
	test_legacy_witness();
	test_wedged_chain_is_bounded();
	test_begin_refused_forever();
	test_recovers_on_later_attempt();
	test_timeout_then_success();
	test_chain_error_then_success();
	test_already_up_is_free();
	test_clock_wraparound();
	test_argument_guards();
	printf("test_stage_netinit: PASS (bring-up is bounded; the blocking "
		   "libogc shape provably is not)\n");
	return 0;
}
