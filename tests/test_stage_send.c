/* Wii stage-server send path: the CWFETCH1 body corruption of 2026-09-02.
 *
 * main.c's send loop is Wii-only (net_send/IOS), so the POLICY half of it
 * lives in tools/wii_stage_server/source/stage_send.h -- pure, header-only,
 * transport injected -- and this test drives the very same
 * cw_stage_send_all() the .dol compiles, against a fake IOS socket that
 * reproduces the libogc/IOS pathology the field captures pinned:
 *
 *   a net_send that cannot take the whole request QUEUES WHAT FITS and
 *   still returns -EAGAIN, discarding the partial count.
 *
 * Part 1 is a REGRESSION WITNESS: the legacy "retry from the same pointer
 * on -EAGAIN" loop is reproduced verbatim here and asserted to produce the
 * exact field signature (a concatenation of prefixes of the payload, first
 * duplication boundary at 4096 payload bytes). If someone reintroduces that
 * loop, part 2's invariant fails and this file explains why.
 *
 * Part 2 pins the fix: under EVERY transport schedule -- generous, stingy,
 * hostile, reported-partial, zero-progress -- the bytes cw_stage_send_all()
 * puts on the wire are always a PREFIX of the payload. Never a duplicate,
 * never a reordering, never a skip. A transfer either completes exactly or
 * stops short and fails loudly.
 *
 * Field evidence these numbers come from (tmp/wii_runs/<run>/stage_mailbox/
 * previous_cw_boot_trace.txt, three independent real-Wii fetches):
 *   bytes=8567  reply header 124 -> first duplication at body offset 3972
 *   bytes=8404  reply header 124 -> first duplication at body offset 3972
 *   bytes=20619 reply header 125 -> first duplication at body offset 3971
 * i.e. 4096 absolute in all three -- the IOS socket send buffer, not TCP
 * and not file content. CW_STAGE_SEND_IOS_SNDBUF records that measurement.
 */

#include "stage_send.h"

#include <assert.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Fake IOS socket                                                     */
/* ------------------------------------------------------------------ */

/* Big enough for the legacy witness to over-emit well past one payload
 * (bounded by its own retry budget below) without ever masking a bug
 * behind a truncated log. */
#define WIRE_CAP (192u * 1024u)

struct fake_ios {
	unsigned capacity;	/* socket send buffer size, bytes */
	unsigned queued;	/* bytes currently sitting in it */
	unsigned drain;		/* bytes the peer consumes per drain opportunity */
	int report_partial; /* 1: a short accept returns the count (a benign
						 * transport); 0: it returns -EAGAIN instead, which
						 * is the measured libogc/IOS behaviour */
	unsigned char wire[WIRE_CAP];
	size_t wire_len;
	unsigned sends;
};

#define FAKE_EAGAIN (-11)

/* cw_stage_send_all() takes ONE user pointer, so the fake socket and the
 * caller's patience budget travel together. Instances are static: the wire
 * log is far too large for a stack frame. */
struct send_user {
	struct fake_ios f;
	int pace_budget;
};

static void fake_reset(struct send_user* u, unsigned capacity,
					   unsigned drain, int report_partial, int pace_budget) {
	memset(u, 0, sizeof(*u));
	u->f.capacity = capacity;
	u->f.drain = drain;
	u->f.report_partial = report_partial;
	u->pace_budget = pace_budget;
}

static void fake_drain(struct fake_ios* f) {
	f->queued = f->queued > f->drain ? f->queued - f->drain : 0u;
}

/* The transmit hook. Draining happens only at the caller's explicit drain
 * opportunities (pace, or the legacy loop's retry sleep), never implicitly
 * inside a send -- so the FIRST body send sees a buffer still holding the
 * reply header, which is what puts the field's first duplication boundary
 * at 4096 absolute rather than at 4096 body bytes. */
static int user_send(void* user, const unsigned char* data, int len) {
	struct fake_ios* f = &((struct send_user*)user)->f;
	unsigned room, take;
	assert(len > 0);
	f->sends++;
	room = f->capacity - f->queued;
	take = (unsigned)len < room ? (unsigned)len : room;
	assert(f->wire_len + take <= WIRE_CAP);
	memcpy(f->wire + f->wire_len, data, take);
	f->wire_len += take;
	f->queued += take;
	if(take == (unsigned)len)
		return len;
	/* THE BUG SURFACE: bytes were queued, but the call reports an error and
	 * throws the count away. */
	return f->report_partial ? (int)take : FAKE_EAGAIN;
}

/* pace() = the peer gets a chance to read. Returns 0 once patience runs
 * out, standing in for main.c's idle deadline. */
static int user_pace(void* user) {
	struct send_user* u = (struct send_user*)user;
	if(u->pace_budget-- <= 0)
		return 0;
	fake_drain(&u->f);
	return 1;
}

static const struct cw_stage_send_ops g_ops = { user_send, user_pace };

/* ------------------------------------------------------------------ */
/* Payload shaped like a real cw_boot_trace.txt                        */
/* ------------------------------------------------------------------ */

#define BODY_LEN 20619u
#define HEADER_LEN 125u /* {"status":"PASS",...,"bytes":20619,"sha256":..} */

static unsigned char g_body[BODY_LEN];
static unsigned char g_header[HEADER_LEN];
static size_t g_line0_len;

static void build_payload(void) {
	size_t i;
	/* A distinctive first line, then filler that never reproduces it, so
	 * "the first line reappeared" is an unambiguous duplication marker --
	 * exactly how the field captures were segmented. */
	static const char line0[] =
		"phase=wii_after_fat build=da9ff01_09.02 "
		"run_id=staged_mp_reconnect_soak_20260902_100102 "
		"profile=mp_reconnect_soak tel_host=pull tel_port=39017 pull=1 "
		"netmask=255.255.252.0 detail=fatInitDefault ok attempts=1\n";
	g_line0_len = strlen(line0);
	assert(g_line0_len > 0 && g_line0_len < BODY_LEN);
	memcpy(g_body, line0, g_line0_len);
	for(i = g_line0_len; i < BODY_LEN; ++i)
		g_body[i] = (unsigned char)('a' + (int)(i % 23));
	memset(g_header, 'H', HEADER_LEN);
}

static size_t count_occurrences(const unsigned char* hay, size_t hay_len,
								const unsigned char* needle,
								size_t needle_len) {
	size_t n = 0, i;
	if(needle_len == 0 || hay_len < needle_len)
		return 0;
	for(i = 0; i + needle_len <= hay_len; ++i) {
		if(memcmp(hay + i, needle, needle_len) == 0)
			n++;
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Part 1 -- regression witness: the legacy retry loop                 */
/* ------------------------------------------------------------------ */

/* main.c BEFORE the fix, transcribed: cw_client_send() retried net_send
 * from the SAME pointer whenever it returned -EAGAIN (sleeping in between,
 * which is the peer's chance to drain), and send_all() advanced only on a
 * non-negative return. Nothing capped the request length. */
static int legacy_send_all(struct send_user* u, const unsigned char* data,
						   size_t len, int retry_budget) {
	size_t sent = 0;
	while(sent < len) {
		int want = (int)(len - sent);
		int n;
		for(;;) {
			n = user_send(u, data + sent, want);
			if(n >= 0)
				break;
			if(retry_budget-- <= 0)
				return 0;
			fake_drain(&u->f); /* the retry sleep */
		}
		if(n <= 0)
			return 0;
		sent += (size_t)n;
	}
	return 1;
}

static struct send_user g_legacy;

static void test_legacy_loop_reproduces_field_signature(void) {
	fake_reset(&g_legacy, CW_STAGE_SEND_IOS_SNDBUF, 1460u,
			   0 /* EAGAIN, the measured IOS shape */, 0);

	/* The reply header goes out first, in one call, and fits -- which is
	 * why every OTHER stage-server reply survived this bug intact. */
	assert(legacy_send_all(&g_legacy, g_header, HEADER_LEN, 64) == 1);
	assert(g_legacy.f.wire_len == HEADER_LEN);

	/* Then the body: the one stage-server payload larger than the socket
	 * buffer, and the only one that was ever corrupted. The retry budget is
	 * what stops this loop -- with the whole 20619-byte body requested in
	 * one call it can never fully succeed, so it duplicates forever. */
	assert(legacy_send_all(&g_legacy, g_body, BODY_LEN, 40) == 0);

	/* SIGNATURE 1: the first duplication boundary is at absolute payload
	 * offset 4096 -- reply header (125) + body (3971) -- exactly as
	 * measured on hardware across three captures whose reply headers had
	 * two different lengths. */
	assert(g_legacy.f.wire_len > 4096u);
	assert(memcmp(g_legacy.f.wire, g_header, HEADER_LEN) == 0);
	assert(memcmp(g_legacy.f.wire + HEADER_LEN, g_body,
				  4096u - HEADER_LEN) == 0);
	assert(memcmp(g_legacy.f.wire + 4096u, g_body, g_line0_len) == 0);

	/* SIGNATURE 2: the body's first line appears MORE THAN ONCE on the wire
	 * though it appears exactly once in the file -- "right byte count,
	 * wrong bytes". */
	assert(count_occurrences(g_body, BODY_LEN, g_body, g_line0_len) == 1);
	assert(count_occurrences(g_legacy.f.wire + HEADER_LEN,
							 g_legacy.f.wire_len - HEADER_LEN, g_body,
							 g_line0_len) > 1);

	/* SIGNATURE 3: a client that reads exactly `bytes` bytes and stops gets
	 * a FULL count of wrong content and never sees the file's tail. */
	assert(g_legacy.f.wire_len - HEADER_LEN >= BODY_LEN);
	assert(memcmp(g_legacy.f.wire + HEADER_LEN, g_body, BODY_LEN) != 0);
}

/* ------------------------------------------------------------------ */
/* Part 2 -- the fix: the wire is ALWAYS a prefix of the payload       */
/* ------------------------------------------------------------------ */

static struct send_user g_fixed;

/* The single invariant that makes the corruption impossible: every byte on
 * the wire is the payload byte at that same offset, so the stream can be
 * SHORT but can never be duplicated, reordered or skipped. */
static int run_fixed(unsigned capacity, unsigned drain, int report_partial,
					 int pace_budget, size_t* out_sent) {
	int rc;
	fake_reset(&g_fixed, capacity, drain, report_partial, pace_budget);
	rc = cw_stage_send_all(&g_ops, &g_fixed, g_body, BODY_LEN, out_sent);
	assert(g_fixed.f.wire_len <= BODY_LEN);
	assert(memcmp(g_fixed.f.wire, g_body, g_fixed.f.wire_len) == 0);
	/* Never more acknowledged than actually transmitted: an over-count
	 * would silently punch a hole in the file. */
	assert(*out_sent <= g_fixed.f.wire_len);
	return rc;
}

static void test_fixed_loop_never_duplicates(void) {
	size_t sent;

	/* A peer that keeps up: complete, byte-exact delivery. */
	assert(run_fixed(CW_STAGE_SEND_IOS_SNDBUF, 4096u, 0, 100000, &sent) ==
		   CW_STAGE_SEND_OK);
	assert(sent == BODY_LEN);
	assert(g_fixed.f.wire_len == BODY_LEN);
	assert(memcmp(g_fixed.f.wire, g_body, BODY_LEN) == 0);

	/* A slow but progressing peer that drains exactly one chunk per pace
	 * tick: still complete and byte-exact. This is the case the inter-chunk
	 * pacing exists to keep OUT of the fatal branch. */
	assert(run_fixed(CW_STAGE_SEND_IOS_SNDBUF, CW_STAGE_SEND_MAX_CHUNK, 0,
					 100000, &sent) == CW_STAGE_SEND_OK);
	assert(sent == BODY_LEN);
	assert(memcmp(g_fixed.f.wire, g_body, BODY_LEN) == 0);

	/* A peer that stops reading entirely: the buffer fills, the transport
	 * goes ambiguous, and the transfer ABORTS instead of duplicating. The
	 * wire holds a short, honest prefix; the client sees a truncated stream
	 * and throws. */
	assert(run_fixed(CW_STAGE_SEND_IOS_SNDBUF, 0u, 0, 100000, &sent) ==
		   CW_STAGE_SEND_ERR_AMBIGUOUS);
	assert(g_fixed.f.wire_len < BODY_LEN);
	assert(g_fixed.f.wire_len <= CW_STAGE_SEND_IOS_SNDBUF);

	/* A transport that reports its partials honestly (positive short
	 * return): trusted, and still byte-exact. */
	assert(run_fixed(CW_STAGE_SEND_IOS_SNDBUF, 512u, 1, 100000, &sent) ==
		   CW_STAGE_SEND_OK);
	assert(sent == BODY_LEN);
	assert(memcmp(g_fixed.f.wire, g_body, BODY_LEN) == 0);

	/* Patience exhausted mid-transfer: a clean STALL, still a prefix. */
	assert(run_fixed(CW_STAGE_SEND_IOS_SNDBUF, CW_STAGE_SEND_MAX_CHUNK, 0, 5,
					 &sent) == CW_STAGE_SEND_ERR_STALLED);
	assert(sent == g_fixed.f.wire_len);
	assert(sent < BODY_LEN);
}

/* Sweep capacities and drain rates, including the pathological ones, and
 * assert the prefix invariant survives all of them. */
static void test_fixed_loop_prefix_invariant_sweep(void) {
	static const unsigned caps[] = { 64u,	1024u, 1460u,
									 4096u, 8192u, 65536u };
	static const unsigned drains[] = { 0u,	  1u,	 17u,	512u,
									   1024u, 1459u, 4096u, 65536u };
	size_t ci, di;
	for(ci = 0; ci < sizeof(caps) / sizeof(caps[0]); ++ci) {
		for(di = 0; di < sizeof(drains) / sizeof(drains[0]); ++di) {
			int partial;
			for(partial = 0; partial <= 1; ++partial) {
				size_t sent = 0;
				/* run_fixed asserts the prefix invariant itself. */
				int rc = run_fixed(caps[ci], drains[di], partial, 3000,
								   &sent);
				assert(rc == CW_STAGE_SEND_OK ||
					   rc == CW_STAGE_SEND_ERR_AMBIGUOUS ||
					   rc == CW_STAGE_SEND_ERR_STALLED);
				if(rc == CW_STAGE_SEND_OK) {
					assert(sent == BODY_LEN);
					assert(g_fixed.f.wire_len == BODY_LEN);
				} else {
					assert(g_fixed.f.wire_len < BODY_LEN);
				}
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* Chunking + argument contract                                        */
/* ------------------------------------------------------------------ */

static void test_chunk_cap(void) {
	size_t sent = 0;
	/* Capped at CW_STAGE_SEND_MAX_CHUNK so libogc's 32 KiB internal
	 * net_sendto chunk loop -- the one that discards partial progress when
	 * a later chunk errors -- is single-iteration by construction. */
	assert(cw_stage_send_chunk_len(0) == 0);
	assert(cw_stage_send_chunk_len(1) == 1);
	assert(cw_stage_send_chunk_len(CW_STAGE_SEND_MAX_CHUNK) ==
		   (int)CW_STAGE_SEND_MAX_CHUNK);
	assert(cw_stage_send_chunk_len(CW_STAGE_SEND_MAX_CHUNK + 1) ==
		   (int)CW_STAGE_SEND_MAX_CHUNK);
	assert(cw_stage_send_chunk_len(8u * 1024u * 1024u) ==
		   (int)CW_STAGE_SEND_MAX_CHUNK);
	/* Both calibration bounds the cap was chosen against. */
	assert(CW_STAGE_SEND_MAX_CHUNK <= CW_STAGE_SEND_IOS_SNDBUF / 2u);
	assert(CW_STAGE_SEND_MAX_CHUNK < 32u * 1024u);

	/* No request ever exceeds the cap, whatever the payload size: at least
	 * ceil(BODY_LEN / cap) calls, i.e. nothing went out in one big shot. */
	assert(run_fixed(65536u, 65536u, 0, 100000, &sent) ==
		   CW_STAGE_SEND_OK);
	assert(g_fixed.f.sends >= (BODY_LEN + CW_STAGE_SEND_MAX_CHUNK - 1u) /
								  CW_STAGE_SEND_MAX_CHUNK);
}

static void test_argument_contract(void) {
	size_t sent = 12345;
	struct cw_stage_send_ops broken = { NULL, NULL };
	fake_reset(&g_fixed, 4096u, 4096u, 0, 16);

	assert(cw_stage_send_all(NULL, &g_fixed, g_body, 4, &sent) ==
		   CW_STAGE_SEND_ERR_ARGS);
	assert(sent == 0);
	assert(cw_stage_send_all(&broken, &g_fixed, g_body, 4, &sent) ==
		   CW_STAGE_SEND_ERR_ARGS);
	assert(cw_stage_send_all(&g_ops, &g_fixed, NULL, 4, &sent) ==
		   CW_STAGE_SEND_ERR_ARGS);
	/* Zero-length is a no-op success, not an error: send_all("") is a legal
	 * call shape for an empty file body. */
	assert(cw_stage_send_all(&g_ops, &g_fixed, NULL, 0, &sent) ==
		   CW_STAGE_SEND_OK);
	assert(sent == 0);
	assert(g_fixed.f.wire_len == 0);
}

/* A transport that over-reports is refused rather than trusted: believing
 * it would advance the cursor past bytes that never left, silently punching
 * a hole in the file. */
static int overcounting_send(void* user, const unsigned char* data,
							 int len) {
	(void)data;
	((struct send_user*)user)->f.sends++;
	return len + 1;
}

static void test_overcount_refused(void) {
	struct cw_stage_send_ops ops = { overcounting_send, user_pace };
	size_t sent = 999;
	fake_reset(&g_fixed, 4096u, 4096u, 0, 16);
	assert(cw_stage_send_all(&ops, &g_fixed, g_body, BODY_LEN, &sent) ==
		   CW_STAGE_SEND_ERR_BADCOUNT);
	assert(sent == 0);
}

int main(void) {
	build_payload();
	test_legacy_loop_reproduces_field_signature();
	test_fixed_loop_never_duplicates();
	test_fixed_loop_prefix_invariant_sweep();
	test_chunk_cap();
	test_argument_contract();
	test_overcount_refused();
	return 0;
}
