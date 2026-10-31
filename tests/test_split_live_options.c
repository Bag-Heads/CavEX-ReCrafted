/* Copyright (c) 2026 CaveWii_v2 contributors
 * The Options row applies the experimental split switch LIVE (Astra option A,
 * S-A2): the client half of the transition S-A1 built on the server thread.
 *
 * What this pins is the SEQUENCE the ruling demands -- "acknowledged state
 * before input arming". screen_options_split_poll() returns the value
 * input_split_join_arm() must follow, so every "armed / not armed" assertion
 * below is that return value, read at the exact seam main.c reads it
 * (source/main.c, right after clin_update()). A pending toggle returns the
 * PREVIOUS value: nothing is armed and no config key moves until an ack with
 * the matching request_seq says ok.
 *
 * The requests are read out of the REAL transport (svin_rpc_send ->
 * svin_process_messages), so the epoch stamp, the request_seq numbering and
 * "two toggles produce two requests" are measured rather than asserted about
 * intent. The acks are hand-built struct clin_split_ack values because that is
 * exactly what clin_split_last_ack() hands main.c -- S-A1's
 * test_split_live_toggle already proves the server produces them, and
 * re-deriving them here would test that code twice instead of this state
 * machine once.
 *
 * R-A8 (S-A4) extended this with the cases the first pass left out: a late ack
 * after the timeout (g), a refused svin_rpc_send (h), the svin SPLIT_SET
 * coalescer/reserve actually executing against a held drain (i), the Options
 * REOPEN that decides which value the row shows (j), and the client-side
 * guest-maintenance predicates a live OFF drives (k).
 *
 * S-A5 adds (l): the explicit config flush at the world's teardown, which is
 * the other half of A8-M6 -- the deferral that keeps config_save out of the
 * gameplay frame must not also lose the value when the player leaves without
 * reopening Options.
 *
 * The reconcile those R-A8 findings produced also changed what cases (a)-(d)
 * mean: an ack that names the CURRENT generation is now adopted from ANY row
 * state, because the server is
 * the authority for the live state and its ack carries `s->split_enabled`
 * itself. The per-case comments below say where that matters.
 *
 * Two production symbols are wrapped:
 *   clin_input_epoch -- the stable world-generation stamp. Standing up a real
 *     handoff needs a whole server fixture (test_split_live_toggle's job); the
 *     row only has to STAMP whatever the client last ACKed, so the value is
 *     injected and then read back out of the request.
 *   config_save -- keeps the commit off the repo's own config.json while still
 *     counting that it happened. config_set_int is NOT wrapped, so the key is
 *     checked through the real in-memory config.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "entity/entity.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "network/client_interface.h"
#include "network/server_interface.h"

/* ---- wrapped production symbols ----------------------------------------- */
static uint32_t epoch;
uint32_t __wrap_clin_input_epoch(void) {
	return epoch;
}
static unsigned saves;
bool __real_config_save(struct config* c, const char* filename);
bool __wrap_config_save(struct config* c, const char* filename) {
	(void)c;
	(void)filename;
	saves++;
	return true;
}

/* ---- the real transport, drained the way the server thread drains it ----- */
#define SEEN_MAX 8
static struct server_rpc seen[SEEN_MAX];
static unsigned seen_n;
static unsigned seen_total;
static unsigned local_leave_seen;
static void grab(struct server_rpc* call, void* unused) {
	(void)unused;
	if(call->type != SRPC_SPLIT_SET)
		return;
	assert(seen_n < SEEN_MAX);
	seen[seen_n++] = *call;
	seen_total++;
}
static void grab_owned(struct server_rpc_source source, struct server_rpc* call,
					   void* unused) {
	(void)source;
	(void)unused;
	if(call->type == SRPC_LOCAL_GUEST
	   && call->payload.local_guest.action == LOCAL_GUEST_LEAVE)
		local_leave_seen++;
}
static void drain(void) {
	svin_rpc_flush_pending();
	svin_process_messages(grab, NULL, false);
}

static const char* row(void) {
	return screen_options_test_split_row();
}
static const char* status(void) {
	return screen_options_test_split_status();
}
static int stored_key(void) {
	return config_read_int(&gstate.config_user, "experimental.local_splitscreen",
						   0)
		? 1
		: 0;
}
/* Each case starts from a known switch state and a known config key. */
static void reset_case(bool applied, int key) {
	seen_n = 0;
	assert(config_set_int(&gstate.config_user, "experimental.local_splitscreen",
						  key));
	screen_options_split_boot(applied);
	/* S-A3: a freshly booted world has toggled nothing, so the heartbeat view
	 * is the documented default -- the acknowledged boot state, no request in
	 * flight, no round trip, no outcome. Asserted on EVERY case, so one case's
	 * measurement can never leak into the next world's heartbeat. */
	struct split_live_status s = screen_options_split_live_status();
	assert(s.live == applied && !s.pending && s.toggle_ms == 0
		   && s.last_ok == -1);
}

/* ---- the REAL ack slot, driven the way the inbox drain drives it ---------
 * R-A9 test gap 3: every case above and below builds `struct clin_split_ack`
 * by hand, so the one property that produced A9-H1/H2/M3 -- whether the slot
 * clin_split_last_ack reads is an EVENT or a LEVEL -- was invisible to the
 * whole suite. These two are the production seam instead of a model of it:
 * server_ack() stores an ack through clin_process's real CRPC_SPLIT_STATE
 * case, and poll_frame() is main.c's per-frame line verbatim (take whatever
 * the client consumed, hand it over once, NULL otherwise). A regression that
 * makes the slot sticky again turns cases (n) and (o) red. */
static void server_ack(bool ok, bool enabled, uint32_t generation,
					   uint32_t request_seq, const char* reason) {
	struct client_rpc call = {.type = CRPC_SPLIT_STATE};
	call.payload.split_state.ok = ok;
	call.payload.split_state.enabled = enabled;
	call.payload.split_state.generation = generation;
	call.payload.split_state.request_seq = request_seq;
	snprintf(call.payload.split_state.reason,
			 sizeof(call.payload.split_state.reason), "%s",
			 reason ? reason : "");
	clin_process(&call);
}
static bool poll_frame(float dt) {
	struct clin_split_ack ack;
	return screen_options_split_poll(
		clin_split_last_ack(&ack) ? &ack : NULL, dt);
}

/* (a) in-world toggle ON: request carries the current epoch and a fresh seq,
 * the row goes pending, NOTHING is armed and the key does not move; the
 * matching ok ack arms it and commits. */
static void a_toggle_on_then_ok(void) {
	reset_case(false, 0);
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 1);
	assert(seen[0].payload.split_set.enabled);
	assert(seen[0].payload.split_set.world_generation == epoch);
	uint32_t seq = seen[0].payload.split_set.request_seq;
	assert(seq != 0);
	assert(!strcmp(row(), "Local split-screen: Applying..."));
	/* Acknowledged state BEFORE arming: the poll still reports OFF, and the
	 * config transaction would still write the acknowledged value. */
	assert(!screen_options_split_poll(NULL, 0.0F));
	assert(stored_key() == 0 && screen_options_test_split_config_value() == 0);
	/* S-A3 heartbeat view: a request is in flight, nothing has settled, and
	 * the LIVE field still reports the acknowledged (old) state -- the same
	 * "acknowledged before armed" rule the poll's return value follows. */
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(!s.live && s.pending && s.toggle_ms == 0 && s.last_ok == -1);
	}
	unsigned before = saves;

	struct clin_split_ack ack = {.valid = true, .ok = true, .enabled = true,
		.generation = epoch, .request_seq = seq};
	assert(screen_options_split_poll(&ack, 0.0F));
	assert(!strcmp(row(), "Local split-screen: ON"));
	assert(status()[0] == '\0');
	/* A8-M6: the ack moves the key IN MEMORY and marks the Options transaction
	 * dirty. It must NOT reach for config_save from main.c's per-frame ack seam
	 * -- on the Wii that is a synchronous libfat SD write inside a gameplay
	 * frame. The existing Options commit (options_commit, the one that writes
	 * the audio keys) is what persists it. */
	assert(stored_key() == 1 && saves == before);
	assert(screen_options_test_split_key_dirty());
	/* S-A3: the ack settled it -- live, nothing pending, an ok outcome, and a
	 * round trip for the heartbeat. The clock is time_get(), so the VALUE is
	 * whatever this machine measured; what is pinned is that it is non-zero
	 * (0 is the "no toggle answered yet" sentinel, so the 1 ms floor must
	 * hold even when the ack is consumed in the request's own millisecond)
	 * and inside the row's own 5,000 ms ack timeout. */
	uint32_t toggle_ms;
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && !s.pending && s.last_ok == 1);
		assert(s.toggle_ms > 0 && s.toggle_ms <= 5000);
		toggle_ms = s.toggle_ms;
	}
	/* Idempotent for real (review gap 2): before the reconcile this assertion
	 * only proved the poll early-returned on a SETTLED row without looking at
	 * the ack. Now the ack IS read from a SETTLED row -- it names the current
	 * generation -- so re-feeding it exercises the adopt path and must still
	 * land on exactly the same state, with no second key move. */
	assert(screen_options_split_poll(&ack, 0.0F));
	assert(!strcmp(row(), "Local split-screen: ON"));
	assert(stored_key() == 1 && saves == before);
	/* R-A9 test gap 2: the ONE field a second look at the same ack used to
	 * move, and the one this case dropped the assertion for. split_commit_key
	 * inferred "already saved" from "already in the in-memory object", so the
	 * second poll cleared the deferred write it had just armed and the
	 * acknowledged toggle never reached config.json (A9-H1). The key is owed
	 * until a config_save says otherwise, however many times the value is
	 * re-adopted. */
	assert(screen_options_test_split_key_dirty());
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && !s.pending && s.last_ok == 1
			   && s.toggle_ms == toggle_ms);
	}
	printf("PASS (a) in-world toggle ON: SRPC_SPLIT_SET{enabled=1, "
		   "generation=%u, request_seq=%u}, row \"%s\" and NOTHING armed while "
		   "pending; ok ack arms it, row \"%s\", key committed to 1; heartbeat "
		   "live=1 pending=0 toggle_ms=%u last_ok=1\n",
		   epoch, seq, "Local split-screen: Applying...", row(), toggle_ms);
}

/* (b) the ack refuses: not armed, the switch stays OFF, and the server's
 * reason is shown -- truncated with "..." when it is longer than the line. */
static void b_ack_refused(void) {
	reset_case(false, 0);
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 1);
	unsigned before = saves;
	struct clin_split_ack ack = {.valid = true, .ok = false, .enabled = false,
		.generation = epoch,
		.request_seq = seen[0].payload.split_set.request_seq};
	/* The real S-A1 allocation-failure text (server_local's ack reason), which
	 * is longer than the helper line: the truncation is the point. */
	snprintf(ack.reason, sizeof(ack.reason), "%s",
			 "split scheduler allocation failed");
	assert(!screen_options_split_poll(&ack, 0.0F));
	assert(!strcmp(row(), "Local split-screen: OFF (failed)"));
	assert(stored_key() == 0 && saves == before); /* nothing changed, no save */
	assert(!strncmp(status(), "Could not apply: split scheduler allocation",
					43));
	/* S-A3: a refusal is a SETTLED outcome (last_ok 0), not an absence, and
	 * the server still answered -- so the round trip is a real measurement. */
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(!s.live && !s.pending && s.last_ok == 0);
		assert(s.toggle_ms > 0 && s.toggle_ms <= 5000);
	}
	size_t len = strlen(status());
	assert(len == 48 && !strcmp(status() + len - 3, "..."));
	printf("PASS (b) refused ack: not armed, key stays 0, row \"%s\", reason "
		   "shown as \"%s\" (%u chars, truncated with \"...\")\n",
		   row(), status(), (unsigned)len);
}

/* (c) ON then OFF before any ack: two requests N, N+1. N's ack no longer
 * decides the ROW (N+1 does, it is the request the row is waiting on) but it is
 * no longer thrown away either: it is a true statement about the server made in
 * this generation, so the ACKNOWLEDGED state adopts it while the row stays
 * "Applying..." on its own newer seq. That is the A8-H1/H2 fix seen from the
 * pending side -- the client's armed state follows the server at every instant
 * the server has told it something, and the server really was ON there. */
static void c_superseded(void) {
	reset_case(false, 0);
	assert(screen_options_test_split_set(1));
	assert(screen_options_test_split_set(0));
	drain();
	assert(seen_n == 2);
	uint32_t n = seen[0].payload.split_set.request_seq;
	assert(seen[1].payload.split_set.request_seq == n + 1);
	assert(seen[0].payload.split_set.enabled
		   && !seen[1].payload.split_set.enabled);

	struct clin_split_ack old = {.valid = true, .ok = true, .enabled = true,
		.generation = epoch, .request_seq = n};
	/* Armed, because the SERVER said ON: the row's own decision is untouched. */
	assert(screen_options_split_poll(&old, 0.0F));
	assert(!strcmp(row(), "Local split-screen: Applying..."));
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && s.pending && s.last_ok == 1);
	}
	assert(stored_key() == 1 && screen_options_test_split_config_value() == 1);

	struct clin_split_ack fresh = {.valid = true, .ok = true, .enabled = false,
		.generation = epoch, .request_seq = n + 1};
	assert(!screen_options_split_poll(&fresh, 0.0F));
	assert(!strcmp(row(), "Local split-screen: OFF"));
	assert(stored_key() == 0);
	printf("PASS (c) superseded: toggle ON then OFF sent request_seq %u and "
		   "%u; the ok ack for %u reconciles the acknowledged state to the "
		   "server (armed, key 1) without retiring the row, and %u decides -- "
		   "row \"%s\", key back to %d\n",
		   n, n + 1, n, n + 1, row(), stored_key());
}

/* (d) the acks the client must NOT act on, now that a current-generation ack is
 * adopted from any row state: one from a DIFFERENT world generation (a decision
 * about a world this client is no longer looking at) and a never-acked session
 * (valid=false). Both leave everything alone; the matching one still decides.
 * Deliberately order-independent -- the old `assert(seq > 1)` coupled this case
 * to how many requests the cases above happened to send. */
static void d_ignored_acks(void) {
	reset_case(false, 0);
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 1);
	uint32_t seq = seen[0].payload.split_set.request_seq;
	assert(seq != 0);

	/* Another generation's ok ack, claiming ON: ignored outright. */
	struct clin_split_ack other = {.valid = true, .ok = true, .enabled = true,
		.generation = epoch + 1, .request_seq = seq};
	assert(!screen_options_split_poll(&other, 0.0F));
	assert(!strcmp(row(), "Local split-screen: Applying..."));
	assert(stored_key() == 0);
	/* A never-acked session (valid=false) is ignored too, reason and all. */
	struct clin_split_ack none = {.valid = false, .ok = true, .enabled = true,
		.generation = epoch, .request_seq = seq};
	assert(!screen_options_split_poll(&none, 0.0F));
	assert(!strcmp(row(), "Local split-screen: Applying..."));
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(!s.live && s.pending && s.last_ok == -1);
	}

	struct clin_split_ack good = {.valid = true, .ok = true, .enabled = true,
		.generation = epoch, .request_seq = seq};
	assert(screen_options_split_poll(&good, 0.0F));
	assert(!strcmp(row(), "Local split-screen: ON"));
	printf("PASS (d) ignored acks: request_seq %u pending, an ok ack stamped "
		   "generation %u and an invalid ack are both ignored (still pending, "
		   "not armed, key 0), the ack for generation %u arms it\n",
		   seq, epoch + 1, epoch);
}

/* (e) no ack inside the timeout: the PREVIOUS state is kept (no assumption of
 * success) and the row says so. */
static void e_timeout(void) {
	reset_case(true, 1); /* previous state: ON */
	assert(screen_options_test_split_set(0));
	drain();
	assert(seen_n == 1 && !seen[0].payload.split_set.enabled);
	assert(!strcmp(row(), "Local split-screen: Applying..."));
	unsigned before = saves;
	/* 4.5 s (exact in binary): still pending, still ON. */
	for(int k = 0; k < 9; k++)
		assert(screen_options_split_poll(NULL, 0.5F));
	assert(!strcmp(row(), "Local split-screen: Applying..."));
	/* S-A3: still in flight at 4.5 s -- pending, and nothing settled yet. */
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && s.pending && s.toggle_ms == 0 && s.last_ok == -1);
	}
	/* 5.0 s: the request is declared lost. */
	assert(screen_options_split_poll(NULL, 0.5F));
	assert(!strcmp(row(), "Local split-screen: ON (no answer)"));
	/* S-A3: a lost request is not a success (last_ok 0) and produced no round
	 * trip at all, so toggle_ms stays 0 -- reporting the 5,000 ms timeout
	 * would invent a latency the server never produced. */
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && !s.pending && s.toggle_ms == 0 && s.last_ok == 0);
	}
	assert(!strcmp(status(), "No answer from the server."));
	assert(stored_key() == 1 && saves == before); /* nothing assumed, no save */
	printf("PASS (e) timeout: 4.5 s pending keeps \"Applying...\", at 5.0 s the "
		   "previous state is kept -- row \"%s\", status \"%s\", key still 1, "
		   "no save\n",
		   row(), status());
}

/* (f) title screen / no world: no request at all, no pending state, and the
 * row's value is what the Options transaction commits -- today's behaviour. */
static void f_title_screen(void) {
	gstate.world_loaded = false;
	reset_case(false, 0);
	unsigned before = saves;
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 0);
	assert(!strcmp(row(), "Local split-screen: ON"));
	assert(status()[0] == '\0');
	assert(screen_options_test_split_config_value() == 1);
	assert(saves == before); /* committed with the audio keys on leave */
	/* Nothing is armed without a server: the boot path applies it next launch. */
	assert(!screen_options_split_poll(NULL, 10.0F));
	printf("PASS (f) title screen: toggle sends NO request, no pending state, "
		   "row \"%s\", the Options transaction would write %d, and nothing is "
		   "armed\n",
		   row(), screen_options_test_split_config_value());
	gstate.world_loaded = true;
}

/* (g) A8-H1: the timeout fires, and THEN the ok ack the server had already
 * produced arrives. Before the fix the poll early-returned on the NO_ANSWER row
 * and dropped it -- server ON with its residency block and accept class armed,
 * client OFF, for the rest of the session, under a row that read "No answer
 * from the server." Now the client converges on the server. */
static void g_late_ack_after_timeout(void) {
	reset_case(false, 0);
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 1 && seen[0].payload.split_set.enabled);
	uint32_t seq = seen[0].payload.split_set.request_seq;
	/* One long frame is enough (gstate.stats.dt is the raw, uncapped delta): a
	 * chunk-save burst or an SD write trips this without the server being slow
	 * at all. */
	assert(!screen_options_split_poll(NULL, 6.0F));
	assert(!strcmp(row(), "Local split-screen: OFF (no answer)"));
	assert(!strcmp(status(), "No answer from the server."));

	unsigned before = saves;
	struct clin_split_ack late = {.valid = true, .ok = true, .enabled = true,
		.generation = epoch, .request_seq = seq};
	assert(screen_options_split_poll(&late, 0.0F)); /* ARMED */
	assert(!strcmp(row(), "Local split-screen: ON"));
	assert(status()[0] == '\0'); /* the false "no answer" caption is gone */
	assert(stored_key() == 1 && screen_options_test_split_key_dirty());
	assert(saves == before); /* still no mid-frame save (A8-M6) */
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && !s.pending && s.last_ok == 1);
	}
	printf("PASS (g) late ack after timeout: request_seq %u times out to "
		   "\"no answer\", then the server's ok ack for it converges the client "
		   "onto the server -- armed, key %d, row \"%s\", status cleared\n",
		   seq, stored_key(), row());
}

/* (h) A8-H2: svin_rpc_send refuses the toggle. The request never left, so
 * nothing about it may be recorded -- no pending seq (the old code left the
 * PREVIOUS request's seq there under a FAILED row, which is how its ack got
 * dropped) and no burned request_seq. The refusal is reported as a transport
 * failure, and the next real request works. */
static void h_send_failure(void) {
	reset_case(false, 0);
	/* Hold the drain: 16 ordinary messages fill the inbox (RPC_INBOX_SIZE), so
	 * every later send falls through to the client-side pending ring. */
	for(unsigned k = 0; k < 16; k++) {
		struct server_rpc fill = {.type = SRPC_HOTBAR_SLOT,
			.payload.hotbar_slot = {.slot = 0}};
		assert(svin_rpc_send(&fill));
	}
	assert(svin_pending_count() == 0);
	/* One toggle takes the SPLIT_SET reserve slot in the pending ring ... */
	assert(screen_options_test_split_set(1));
	assert(svin_pending_count() == 1);
	uint32_t sent_gen = epoch;
	/* ... and a toggle stamped for a DIFFERENT generation cannot coalesce into
	 * it (a different decision about a different world), so the reserve is
	 * already spent and the send is refused. */
	epoch = sent_gen + 1;
	assert(screen_options_test_split_set(0));
	assert(!strcmp(row(), "Local split-screen: OFF (failed)"));
	assert(!strcmp(status(), "Could not send: transport busy"));
	/* Captured now: the requests below settle the row and clear the line. */
	char failed_status[64];
	snprintf(failed_status, sizeof(failed_status), "%s", status());
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(!s.live && !s.pending && s.last_ok == 0);
	}
	/* No phantom request: still exactly one queued, and nothing for the new
	 * generation ever reaches the transport. */
	assert(svin_pending_count() == 1);
	while(svin_pending_count() > 0 || seen_n == 0)
		drain();
	drain();
	assert(seen_n == 1);
	assert(seen[0].payload.split_set.world_generation == sent_gen);
	uint32_t queued_seq = seen[0].payload.split_set.request_seq;

	/* No phantom SEQ either: the refused attempt burned no number, so the next
	 * real request is exactly queued_seq + 1 rather than +2. */
	seen_n = 0;
	assert(screen_options_test_split_set(0));
	drain();
	assert(seen_n == 1);
	assert(seen[0].payload.split_set.request_seq == queued_seq + 1);
	assert(seen[0].payload.split_set.world_generation == epoch);
	assert(!strcmp(row(), "Local split-screen: Applying..."));
	struct clin_split_ack ack = {.valid = true, .ok = true, .enabled = false,
		.generation = epoch,
		.request_seq = seen[0].payload.split_set.request_seq};
	assert(!screen_options_split_poll(&ack, 0.0F));
	assert(!strcmp(row(), "Local split-screen: OFF"));
	printf("PASS (h) send failure: refused toggle shows \"%s\" with last_ok=0, "
		   "records NO pending seq and burns NO request_seq (queued %u, next "
		   "real request %u), and the next request is answered normally\n",
		   failed_status, queued_seq, queued_seq + 1);
	epoch = sent_gen;
}

/* (i) review gap 5: the svin SPLIT_SET coalescer and its reserve, EXECUTED.
 * Every send in the original pair of tests was followed immediately by a drain,
 * so svin_try_send_now always succeeded and neither the coalescing block nor the
 * svin_pending_split_set_count accounting ever ran -- the documented
 * justification for the reserve ("a full ordinary-action backlog can never
 * swallow a menu toggle", "50 impatient toggles stay bounded") was unproven. */
static void i_coalescer_and_reserve(void) {
	reset_case(false, 0);
	seen_n = 0;
	/* Hold the drain again. */
	for(unsigned k = 0; k < 16; k++) {
		struct server_rpc fill = {.type = SRPC_HOTBAR_SLOT,
			.payload.hotbar_slot = {.slot = 0}};
		assert(svin_rpc_send(&fill));
	}
	/* Saturate the ORDINARY class: 64 queued actions is its whole capacity, and
	 * the 65th is rejected. This is the backlog the reserve exists to survive. */
	for(unsigned k = 0; k < 64; k++) {
		struct server_rpc fill = {.type = SRPC_HOTBAR_SLOT,
			.payload.hotbar_slot = {.slot = 0}};
		assert(svin_rpc_send(&fill));
	}
	assert(svin_pending_count() == 64);
	struct server_rpc overflow = {.type = SRPC_HOTBAR_SLOT,
		.payload.hotbar_slot = {.slot = 0}};
	assert(!svin_rpc_send(&overflow));

	/* Three toggles behind that full backlog: the first takes the reserve slot,
	 * the next two COALESCE into it (same generation, newest wins) instead of
	 * consuming the reserve twice or being dropped. */
	assert(screen_options_test_split_set(1));
	assert(svin_pending_count() == 65);
	assert(screen_options_test_split_set(0));
	assert(screen_options_test_split_set(1));
	assert(svin_pending_count() == 65); /* still ONE SPLIT_SET queued */
	assert(!strcmp(row(), "Local split-screen: Applying..."));

	while(svin_pending_count() > 0)
		drain();
	drain();
	/* Exactly one request reached the server, and it is the NEWEST desired
	 * state with the newest seq -- not the first toggle, and not three. */
	assert(seen_n == 1);
	assert(seen[0].payload.split_set.enabled);
	uint32_t newest = seen[0].payload.split_set.request_seq;
	struct clin_split_ack ack = {.valid = true, .ok = true, .enabled = true,
		.generation = epoch, .request_seq = newest};
	assert(screen_options_split_poll(&ack, 0.0F));
	assert(!strcmp(row(), "Local split-screen: ON"));
	printf("PASS (i) coalescer/reserve: a SPLIT_SET queues behind a FULL 64 "
		   "ordinary backlog (which itself rejects a 65th action), three "
		   "toggles occupy ONE reserve slot, and the single request that "
		   "arrives is the newest (enabled=1, request_seq=%u)\n", newest);
	/* Leave the switch off for the cases that follow. */
	assert(screen_options_test_split_set(0));
	seen_n = 0;
	drain();
	assert(seen_n == 1);
	struct clin_split_ack off = {.valid = true, .ok = true, .enabled = false,
		.generation = epoch,
		.request_seq = seen[0].payload.split_set.request_seq};
	assert(!screen_options_split_poll(&off, 0.0F));
}

/* (j) A8-M4: which value the row shows when Options is REOPENED -- the one
 * thing the original title-screen case never exercised, and the one place the
 * regression lived. Each screen must tell the truth about its own scope. */
static void j_reopen(void) {
	/* Title screen: the FILE is the truth, because the file is what the next
	 * world load applies. Before the fix screen_options_reset preferred
	 * split_applied whenever main.c had seeded it -- which it does once, before
	 * the main loop -- so the file was never read again and a title-screen
	 * toggle was silently reverted in the row. */
	gstate.world_loaded = false;
	reset_case(false, 0);
	assert(screen_options_test_split_set(1));
	assert(!strcmp(row(), "Local split-screen: ON"));
	assert(screen_options_test_split_config_value() == 1);
	/* Leaving Options is what writes the key (options_commit's transaction). */
	assert(config_set_int(&gstate.config_user, "experimental.local_splitscreen",
						  1));
	screen_options_test_reopen();
	assert(!strcmp(row(), "Local split-screen: ON"));
	assert(screen_options_test_split_config_value() == 1);
	/* ...and the reverse direction round-trips too. */
	assert(screen_options_test_split_set(0));
	assert(config_set_int(&gstate.config_user, "experimental.local_splitscreen",
						  0));
	screen_options_test_reopen();
	assert(!strcmp(row(), "Local split-screen: OFF"));

	/* In-world: the ACKNOWLEDGED value is the truth, because that is what input
	 * arming follows -- even when the file still disagrees. */
	gstate.world_loaded = true;
	reset_case(true, 0);
	assert(stored_key() == 0);
	screen_options_test_reopen();
	assert(!strcmp(row(), "Local split-screen: ON"));
	printf("PASS (j) Options reopen: at the title the row round-trips the FILE "
		   "(ON then OFF), in-world it shows the ACKNOWLEDGED switch (\"%s\") "
		   "even with the file at %d\n", row(), stored_key());
}

/* (k) A8-H3: the client-side guest-maintenance predicates a live OFF drives.
 * "OFF never ejects a guest" held on the server only -- the client's whole
 * guest block was gated on the switch, so a live OFF stopped maintaining a
 * guest that was mid-join (card never retired, controller still claimed) and
 * froze the guest ANCHOR at a stale position. */
static void k_guest_maintenance(void) {
	reset_case(false, 0); /* acknowledged OFF */
	/* Nothing to maintain: the single-player frame is untouched. */
	assert(!screen_options_split_guest_maintain(false, false));
	/* ...but a session or a body keeps the block (and the anchor) alive. */
	assert(screen_options_split_guest_maintain(true, false));
	assert(screen_options_split_guest_maintain(false, true));
	/* An unfinished join under OFF is refused and its claim released. */
	assert(screen_options_split_guest_refuse(true, false));
	/* An ADMITTED body is never refused -- the server keeps it, so we do too. */
	assert(!screen_options_split_guest_refuse(true, true));
	/* Nothing joining: nothing to refuse. */
	assert(!screen_options_split_guest_refuse(false, false));
	assert(!screen_options_split_guest_refuse(false, true));

	reset_case(true, 1); /* acknowledged ON */
	assert(screen_options_split_guest_maintain(false, false));
	/* ON: a join that has not been admitted yet keeps trying (the server denies
	 * while the body's terrain streams), it is NOT refused. */
	assert(!screen_options_split_guest_refuse(true, false));
	assert(!screen_options_split_guest_refuse(true, true));
	reset_case(false, 0);
	puts("PASS (k) guest maintenance under a live OFF: the block runs while a "
		 "guest session OR body exists (never with neither), an unfinished join "
		 "is refused and released, an admitted body is left alone, and with the "
		 "switch ON a not-yet-admitted join keeps trying");
}

/* (l) R-A8 A8-M6, the other half (S-A5): the ack defers the file write, so the
 * value must be flushed when the WORLD goes away -- otherwise a player who
 * toggles in-world and leaves without reopening Options loses it. main.c calls
 * screen_options_split_flush() on the falling edge of gstate.world_loaded; this
 * drives the same sequence: toggle in-world -> ok ack -> unload -> the config
 * holds the new value, written exactly once. */
static void l_unload_flush(void) {
	reset_case(false, 0);
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 1);
	unsigned before = saves;

	struct clin_split_ack ack = {.valid = true, .ok = true, .enabled = true,
		.generation = epoch,
		.request_seq = seen[0].payload.split_set.request_seq};
	assert(screen_options_split_poll(&ack, 0.0F));
	/* The A8-M6 state the flush exists for: the key has moved IN MEMORY, the
	 * transaction is owed, and NO file write has happened. */
	assert(stored_key() == 1 && saves == before);
	assert(screen_options_test_split_key_dirty());

	/* R-A9 test gap 1: this case used to poll the ack ONCE and flush, which is
	 * not what main.c did -- the ack slot was never consumed, so the seam
	 * re-fed the same ack on every frame between the toggle and the unload.
	 * One extra poll reproduces that, and it must leave the owed write owed:
	 * the deferral is cancelled only by a config_save that actually happened.
	 * (The fix that makes the slot an event is proven on the real plumbing in
	 * cases (n)/(o)/(p); this pins the row's half -- it must be robust even if
	 * a future caller hands the same ack over twice.) */
	assert(screen_options_split_poll(&ack, 0.0F));
	assert(stored_key() == 1 && saves == before);
	assert(screen_options_test_split_key_dirty());

	/* The world goes away (the player left without reopening Options). */
	assert(screen_options_split_flush());
	assert(saves == before + 1);
	assert(stored_key() == 1);
	assert(!screen_options_test_split_key_dirty());
	int flushed = stored_key();
	/* Nothing owed -> no second write. This is what keeps the ordinary unload
	 * path (and a dimension switch, which clears the same flag) free. */
	assert(screen_options_split_flush());
	assert(saves == before + 1);

	/* ...and an unload with nothing ever toggled touches no file at all. */
	reset_case(false, 0);
	assert(!screen_options_test_split_key_dirty());
	assert(screen_options_split_flush());
	assert(saves == before + 1);
	printf("PASS (l) config flush on world unload: the ok ack leaves the key "
		   "dirty with 0 saves, the unload flush writes it exactly once "
		   "(stored=%d), and a clean key flushes without touching the file\n",
		   flushed);
	reset_case(false, 0);
}

/* (m) R-A9 A9-H1, stated as the contract main.c now satisfies: ONE ack, ONE
 * call. Frame 1 adopts the grant and arms the deferred write; every later frame
 * has no new ack, so the row is left completely alone and the write stays owed;
 * the teardown flush writes it exactly once. This is the two-frame sequence the
 * S-A9 brief names, driven through the real getter so "frame 2 has no new ack"
 * is a measured consequence of consuming the slot rather than an assumption. */
static void m_two_frame_poll(void) {
	reset_case(false, 0);
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 1);
	uint32_t seq = seen[0].payload.split_set.request_seq;
	unsigned before = saves;

	/* Frame 1: the server answers and the seam takes the ack. */
	server_ack(true, true, epoch, seq, NULL);
	assert(poll_frame(0.0F));
	assert(!strcmp(row(), "Local split-screen: ON"));
	assert(stored_key() == 1 && screen_options_test_split_key_dirty());
	assert(saves == before);

	/* Frames 2..60: no new ack. Nothing moves, and the write stays OWED --
	 * before the fix frame 2 re-adopted the same ack and cleared exactly this
	 * flag, so the value the server had granted was silently dropped. */
	for(int k = 0; k < 59; k++)
		assert(poll_frame(1.0F / 60.0F));
	assert(!strcmp(row(), "Local split-screen: ON"));
	assert(status()[0] == '\0');
	assert(stored_key() == 1 && screen_options_test_split_key_dirty());
	assert(saves == before);
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && !s.pending && s.last_ok == 1);
	}

	/* The world goes away: one owed toggle, exactly one write. */
	assert(screen_options_split_flush());
	assert(saves == before + 1 && stored_key() == 1);
	assert(!screen_options_test_split_key_dirty());
	assert(screen_options_split_flush());
	assert(saves == before + 1);
	printf("PASS (m) two-frame poll: the ok ack commits and leaves the write "
		   "owed on frame 1, 59 further frames with NO new ack change nothing "
		   "and keep it owed (key=%d, dirty=1, saves=%u), and the unload flush "
		   "writes it exactly once\n",
		   stored_key(), before);
	reset_case(false, 0);
}

/* (n) R-A9 A9-H2(a): the 5 s NO_ANSWER path must be REACHABLE after a
 * successful toggle. It was not -- the ack slot stayed valid for the rest of
 * the process, so main.c re-fed that ok ack every frame, the adopt branch
 * returned before the age accumulation, and a genuinely lost request read
 * "Applying..." forever with no way for the player to retire it. Driven
 * through poll_frame(), i.e. the production seam, so the timeout is proven
 * against the real ack plumbing and not against a NULL the test chose. */
static void n_timeout_after_an_answered_toggle(void) {
	reset_case(false, 0);
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 1);
	server_ack(true, true, epoch, seen[0].payload.split_set.request_seq, NULL);
	assert(poll_frame(0.0F));
	assert(!strcmp(row(), "Local split-screen: ON"));

	/* The next toggle is LOST (a dropped mailbox entry / a wedged tick). */
	seen_n = 0;
	assert(screen_options_test_split_set(0));
	drain();
	assert(seen_n == 1 && !seen[0].payload.split_set.enabled);
	for(int k = 0; k < 9; k++)
		assert(poll_frame(0.5F)); /* 4.5 s: still ON, still pending */
	assert(!strcmp(row(), "Local split-screen: Applying..."));
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && s.pending);
	}
	assert(poll_frame(0.5F)); /* exactly 5.0 s */
	assert(!strcmp(row(), "Local split-screen: ON (no answer)"));
	assert(!strcmp(status(), "No answer from the server."));
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && !s.pending && s.last_ok == 0);
	}
	/* ...and it STAYS said: 60 further frames with no new ack leave the
	 * caption the player is reading exactly where it is. */
	for(int k = 0; k < 60; k++)
		assert(poll_frame(1.0F / 60.0F));
	assert(!strcmp(row(), "Local split-screen: ON (no answer)"));
	assert(!strcmp(status(), "No answer from the server."));
	printf("PASS (n) timeout reachable after an answered toggle: the grant's "
		   "ack is consumed once, the next request is lost, at exactly 5.0 s "
		   "the row reads \"%s\" with \"%s\", and it survives 60 more frames\n",
		   row(), status());
	reset_case(false, 0);
}

/* (o) R-A9 A9-H2(c): a "Could not send" caption must survive until the row's
 * next state change. With the sticky slot it survived one frame: the next poll
 * re-adopted the old ok ack, took the "not waiting for one at all" arm and
 * forced the row back to SETTLED with an empty status -- which also made
 * screen_options_reset's deliberate "a FAILED / NO_ANSWER status is NOT
 * cleared here" unreachable in any session that had seen one grant. */
static void o_send_failure_caption_survives(void) {
	reset_case(false, 0);
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 1);
	server_ack(true, true, epoch, seen[0].payload.split_set.request_seq, NULL);
	assert(poll_frame(0.0F));
	assert(!strcmp(row(), "Local split-screen: ON"));

	/* Refuse a send, exactly the way (h) does: a full inbox, the SPLIT_SET
	 * reserve spent by one queued toggle, and a second toggle stamped for a
	 * different generation that cannot coalesce into it. */
	for(unsigned k = 0; k < 16; k++) {
		struct server_rpc fill = {.type = SRPC_HOTBAR_SLOT,
			.payload.hotbar_slot = {.slot = 0}};
		assert(svin_rpc_send(&fill));
	}
	uint32_t sent_gen = epoch;
	seen_n = 0;
	assert(screen_options_test_split_set(0));
	assert(svin_pending_count() == 1);
	epoch = sent_gen + 1;
	assert(screen_options_test_split_set(1));
	assert(!strcmp(row(), "Local split-screen: ON (failed)"));
	assert(!strcmp(status(), "Could not send: transport busy"));

	/* 60 frames, no new ack: the diagnosis the player has not read yet stays
	 * on the helper line, and the row keeps saying it failed. */
	for(int k = 0; k < 60; k++)
		assert(poll_frame(1.0F / 60.0F));
	assert(!strcmp(row(), "Local split-screen: ON (failed)"));
	assert(!strcmp(status(), "Could not send: transport busy"));
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && !s.pending && s.last_ok == 0);
	}
	printf("PASS (o) send-failure caption survives: after a granted toggle a "
		   "refused send shows \"%s\" under row \"%s\", and 60 frames later "
		   "both are still exactly that\n",
		   status(), row());
	/* Clean the held transport back up (the queued toggle for the old
	 * generation is still in the ring) and restore the epoch. */
	epoch = sent_gen;
	while(svin_pending_count() > 0 || seen_n == 0)
		drain();
	drain();
	seen_n = 0;
	reset_case(false, 0);
}

/* (p) R-A9 test gap 3, stated directly: the ack slot is an EVENT source. One
 * stored ack is handed over exactly once; the slot is a single last-wins struct
 * (an unread ack is LOST when the next one lands, which is what A9-M6 is
 * about); and it is the TAKE that feeds the row, not a level the row samples. */
static void p_ack_slot_is_an_event(void) {
	reset_case(false, 0);
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 1);
	uint32_t seq = seen[0].payload.split_set.request_seq;

	struct clin_split_ack got;
	assert(!clin_split_last_ack(&got)); /* nothing stored yet */
	server_ack(true, true, epoch, seq, NULL);
	assert(clin_split_last_ack(&got));
	assert(got.valid && got.ok && got.enabled && got.generation == epoch
		   && got.request_seq == seq);
	assert(!clin_split_last_ack(&got)); /* CONSUMED: one statement, once */
	/* Taking it did not retire the row -- the row is retired by being GIVEN
	 * the ack, which is why the seam must not drop one. */
	assert(!strcmp(row(), "Local split-screen: Applying..."));
	assert(screen_options_split_poll(&got, 0.0F));
	assert(!strcmp(row(), "Local split-screen: ON"));

	/* Two acks in ONE drain: last wins, and there is exactly one to take. */
	server_ack(true, false, epoch, seq + 1, NULL);
	server_ack(false, true, epoch, seq + 2, "world reloading");
	assert(clin_split_last_ack(&got));
	assert(!got.ok && got.enabled && got.request_seq == seq + 2);
	assert(!clin_split_last_ack(&got));
	printf("PASS (p) ack slot is an event: an unread slot reads empty, a "
		   "stored ack is taken exactly once (seq %u), the row is retired by "
		   "being given it, and two acks in one drain leave only the last\n",
		   seq);
	reset_case(false, 0);
}

/* (q) R-A9 A9-M5 / A8-M4: which value a REOPEN shows, in BOTH directions, with
 * the file and the acknowledged switch deliberately disagreeing -- the crossed
 * cases (j) did not cover. Each screen tells the truth about its own scope:
 * the title screen shows the FILE (what the next world load applies), in-world
 * shows the ACKNOWLEDGED switch (what input arming follows). The disagreement
 * itself is the documented one-session divergence: a title-screen edit reaches
 * the file and the next launch's boot path, not this process's live switch. */
static void q_reopen_both_directions(void) {
	gstate.world_loaded = false;
	reset_case(true, 0); /* acknowledged ON, file OFF */
	screen_options_test_reopen();
	assert(!strcmp(row(), "Local split-screen: OFF"));
	reset_case(false, 1); /* acknowledged OFF, file ON */
	screen_options_test_reopen();
	assert(!strcmp(row(), "Local split-screen: ON"));

	gstate.world_loaded = true;
	reset_case(true, 0);
	screen_options_test_reopen();
	assert(!strcmp(row(), "Local split-screen: ON"));
	reset_case(false, 1);
	screen_options_test_reopen();
	assert(!strcmp(row(), "Local split-screen: OFF"));
	puts("PASS (q) reopen in both directions: with the file and the "
		 "acknowledged switch disagreeing, the title screen shows the FILE "
		 "(OFF then ON) and in-world shows the ACKNOWLEDGED switch (ON then "
		 "OFF)");
	reset_case(false, 0);
}

/* (r) R-A9 A9-M6 and A9-L7, the two halves of what a REFUSAL may do. The
 * server stamps .enabled on every ack -- refusals included -- with
 * s->split_enabled read after the transition it answers, so a refusal REPORTS
 * the switch even though it changed nothing. Adopting that only inside the ok
 * branch lost a grant that shared a drain with a later refusal (M6). The
 * opposite error is just as real: a refusal that answers nothing this row is
 * tracking must not rewrite the row, or the server's "invalid request" answer
 * to some other caller flips a healthy row to FAILED (L7). */
static void r_refusal_reports_but_does_not_rewrite(void) {
	/* --- M6: the switch is ON per the refusal; the client must converge. --- */
	reset_case(false, 0); /* acknowledged OFF, SETTLED */
	unsigned before = saves;
	struct clin_split_ack lost_grant = {.valid = true, .ok = false,
		.enabled = true, .generation = epoch, .request_seq = 4242};
	snprintf(lost_grant.reason, sizeof(lost_grant.reason), "%s",
			 "world reloading");
	assert(screen_options_split_poll(&lost_grant, 0.0F)); /* ARMED */
	assert(!strcmp(row(), "Local split-screen: ON"));
	assert(status()[0] == '\0'); /* no failure the player caused */
	assert(stored_key() == 1 && screen_options_test_split_key_dirty());
	assert(saves == before);
	{
		/* No request of OURS settled, so the outcome field is untouched. */
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && !s.pending && s.last_ok == -1);
	}

	/* --- L7: the same shape with NOTHING to report must change nothing. --- */
	reset_case(true, 1); /* acknowledged ON, SETTLED, file ON */
	struct clin_split_ack forged = {.valid = true, .ok = false,
		.enabled = true, .generation = epoch, .request_seq = 0};
	snprintf(forged.reason, sizeof(forged.reason), "%s", "invalid request");
	assert(screen_options_split_poll(&forged, 0.0F));
	assert(!strcmp(row(), "Local split-screen: ON"));
	assert(status()[0] == '\0');
	assert(stored_key() == 1 && !screen_options_test_split_key_dirty());
	assert(saves == before);
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && !s.pending && s.last_ok == -1);
	}

	/* --- ...and a refusal the row IS waiting on still retires it. --- */
	reset_case(true, 1);
	seen_n = 0;
	assert(screen_options_test_split_set(0));
	drain();
	assert(seen_n == 1);
	struct clin_split_ack refused = {.valid = true, .ok = false,
		.enabled = true, .generation = epoch,
		.request_seq = seen[0].payload.split_set.request_seq};
	snprintf(refused.reason, sizeof(refused.reason), "%s", "world reloading");
	assert(screen_options_split_poll(&refused, 0.0F));
	assert(!strcmp(row(), "Local split-screen: ON (failed)"));
	assert(!strcmp(status(), "Could not apply: world reloading"));
	{
		struct split_live_status s = screen_options_split_live_status();
		assert(s.live && !s.pending && s.last_ok == 0);
	}
	assert(saves == before);
	/* --- ...and a refusal for a request the row already GAVE UP on replaces
	 * "No answer from the server." with the reason it never got. That is the
	 * one row state where a non-matching refusal may speak: NO_ANSWER already
	 * shows the acknowledged value, so there is nothing to corrupt, and
	 * stranding the server's answer is what A8-H1 was about. --- */
	reset_case(true, 1);
	seen_n = 0;
	assert(screen_options_test_split_set(0));
	drain();
	assert(seen_n == 1);
	uint32_t lost_seq = seen[0].payload.split_set.request_seq;
	assert(screen_options_split_poll(NULL, 6.0F));
	assert(!strcmp(row(), "Local split-screen: ON (no answer)"));
	struct clin_split_ack late = {.valid = true, .ok = false, .enabled = true,
		.generation = epoch, .request_seq = lost_seq + 3};
	snprintf(late.reason, sizeof(late.reason), "%s", "world reloading");
	assert(screen_options_split_poll(&late, 0.0F));
	assert(!strcmp(row(), "Local split-screen: ON (failed)"));
	assert(!strcmp(status(), "Could not apply: world reloading"));
	assert(stored_key() == 1 && saves == before);

	puts("PASS (r) refusal reports the switch without rewriting the row: a "
		 "refusal carrying enabled=1 arms an OFF client (the lost-grant heal), "
		 "a request_seq=0 \"invalid request\" changes not one field, the "
		 "refusal the row is waiting on retires it with the reason, and one "
		 "arriving after the 5 s timeout replaces \"no answer\" with it");
	reset_case(false, 0);
}

/* (s) R-A9 A9-M5, the owed half: the live switch is re-seeded from the FILE on
 * every WORLD GENERATION, not once at boot. Before this, main.c read
 * experimental.local_splitscreen once before the main loop, so a title-screen
 * OFF was ignored by every world the process loaded afterwards: the row snapped
 * back to the boot-applied value on reopen, input stayed armed, and a second
 * controller could still join. The seed asks the SERVER for the file's value
 * (server_local_set_split_enabled refuses once the server thread runs, so the
 * live SRPC_SPLIT_SET route is the only one that does not race the owning
 * thread) and arming still follows the ack, never the file.
 *
 * This is the brief's sequence: title -> file ON -> load world -> row ON and
 * armed; live OFF -> unload (flush) -> load again -> the file's OFF wins, with
 * no stale live state crossing the generation. */
static void s_world_load_reseed(void) {
	/* --- title: the file is edited, nothing is asked of any server. --- */
	gstate.world_loaded = false;
	reset_case(false, 0); /* acknowledged OFF, file OFF */
	epoch = 50;
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 0); /* no world -> no request */
	/* Leaving Options writes the key (options_commit's transaction). */
	assert(config_set_int(&gstate.config_user, "experimental.local_splitscreen",
						  1));
	/* Not available yet: the seed must not record a generation it never
	 * seeded, so it still runs when the world finishes loading. */
	assert(!screen_options_split_world_seed(false));
	drain();
	assert(seen_n == 0);

	/* --- the world loads: the seed asks the server for the FILE's value. --- */
	gstate.world_loaded = true;
	assert(!screen_options_split_world_seed(false)); /* asks; NOT armed yet */
	assert(!strcmp(row(), "Local split-screen: Applying..."));
	drain();
	assert(seen_n == 1);
	assert(seen[0].payload.split_set.enabled
		   && seen[0].payload.split_set.world_generation == epoch);
	uint32_t seed_seq = seen[0].payload.split_set.request_seq;
	/* One attempt per generation: the same generation asks nothing more. */
	seen_n = 0;
	assert(!screen_options_split_world_seed(false));
	drain();
	assert(seen_n == 0);
	/* The grant arms it -- the acknowledged value, at the seam main.c reads. */
	server_ack(true, true, epoch, seed_seq, "");
	assert(poll_frame(0.0F));
	assert(!strcmp(row(), "Local split-screen: ON"));
	assert(stored_key() == 1);

	/* --- live OFF in-world, then the world goes away: the flush writes it. --- */
	seen_n = 0;
	assert(screen_options_test_split_set(0));
	drain();
	assert(seen_n == 1);
	server_ack(true, false, epoch, seen[0].payload.split_set.request_seq, "");
	assert(!poll_frame(0.0F));
	assert(stored_key() == 0 && screen_options_test_split_key_dirty());
	unsigned before = saves;
	gstate.world_loaded = false;
	assert(screen_options_split_flush());
	assert(saves == before + 1 && !screen_options_test_split_key_dirty());

	/* --- the next world load: the FILE (OFF) wins and asks for nothing. --- */
	gstate.world_loaded = true;
	epoch = 51; /* a new generation */
	seen_n = 0;
	assert(!screen_options_split_world_seed(false));
	drain();
	assert(seen_n == 0); /* file and server already agree */
	assert(!strcmp(row(), "Local split-screen: OFF"));
	assert(status()[0] == '\0');
	{
		/* Per-WORLD heartbeat evidence: the previous world's round trip and
		 * outcome do not cross the generation. */
		struct split_live_status st = screen_options_split_live_status();
		assert(!st.live && !st.pending && st.toggle_ms == 0 && st.last_ok == -1);
	}
	assert(saves == before + 1); /* the seed itself writes no file */

	/* --- A9-M5's breaking sequence, the direction that was broken: the file
	 * says OFF while the boot path applied ON. The OFF must reach this world
	 * instead of being ignored for the rest of the process. --- */
	reset_case(true, 0); /* acknowledged ON, file OFF */
	epoch = 52;
	seen_n = 0;
	assert(screen_options_split_world_seed(false)); /* still ON until acked */
	drain();
	assert(seen_n == 1 && !seen[0].payload.split_set.enabled);
	server_ack(true, false, epoch, seen[0].payload.split_set.request_seq, "");
	assert(!poll_frame(0.0F)); /* the player's OFF is now live */
	assert(!strcmp(row(), "Local split-screen: OFF"));

	/* --- ...but an armed split harness profile's forced ON is never asked
	 * away by a file that says 0 (main.c passes hw_run.split_guest). --- */
	reset_case(true, 0);
	epoch = 53;
	seen_n = 0;
	assert(screen_options_split_world_seed(true));
	drain();
	assert(seen_n == 0);
	assert(!strcmp(row(), "Local split-screen: ON"));

	/* --- no stale live state crosses a generation: a toggle still in flight
	 * when the world went away can never be answered (acks are
	 * generation-stamped), so the seed cancels it instead of letting it strand
	 * the row until the 5 s timeout and then fail against the NEW world. --- */
	reset_case(false, 0);
	epoch = 54;
	seen_n = 0;
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 1);
	uint32_t stranded = seen[0].payload.split_set.request_seq;
	assert(!strcmp(row(), "Local split-screen: Applying..."));
	epoch = 55; /* the world was replaced under the pending toggle */
	seen_n = 0;
	assert(!screen_options_split_world_seed(false));
	assert(!strcmp(row(), "Local split-screen: OFF")); /* SETTLED, clean */
	assert(status()[0] == '\0');
	drain();
	assert(seen_n == 0); /* file OFF, acknowledged OFF: nothing to ask */
	{
		struct split_live_status st = screen_options_split_live_status();
		assert(!st.pending);
	}
	/* The old request's ack arrives late: a different generation's ack is
	 * ignored outright, and the retired seq can never match anything. */
	server_ack(true, true, 54, stranded, "");
	assert(!poll_frame(0.0F));
	assert(!strcmp(row(), "Local split-screen: OFF"));

	printf("PASS (s) world-load re-seed: a title-screen edit reaches the NEXT "
		   "world load (one SRPC_SPLIT_SET per generation, armed only by the "
		   "ack), the flushed file value wins on the load after it, a forced "
		   "harness ON is never asked away, and a toggle left pending by the "
		   "old world is cancelled rather than crossing the generation\n");
	epoch = 41;
	reset_case(false, 0);
}

/* (t) S-A13: the APPROVED WORDING, per state, as strings.
 * Astra's call #5 provisional ruling settled what this row and its helper
 * block may say (tmp/jobs/mp-split-0912/astra-call5-provisional-ruling.md):
 * "Applying...", "Could not apply: <reason>" and "No answer from the server."
 * as-is; the ON/OFF word ALWAYS the last ACKNOWLEDGED state in both variants
 * (so "ON (failed)" and "OFF (no answer)" must be reachable, not just the
 * "OFF (failed)" / "ON (no answer)" examples cases (b) and (e) pin);
 * "Experimental - applies immediately." ONLY where application is live, with
 * the title screen describing what it actually does; the join-availability
 * text for an enabled switch; and "New joins disabled. Player 2 can keep
 * playing." for OFF with player 2 present.
 *
 * The strings are asserted through the production selector
 * (screen_options_test_split_helper -> split_helper_lines), the same function
 * screen_options_render2D draws from, so a reworded helper cannot pass here
 * and ship something else. Its prototype is declared here rather than in
 * screen.h because S-A13 owns screen_options.c and this test only. */
const char* screen_options_test_split_helper(int line);
static const char* help1(void) {
	return screen_options_test_split_helper(0);
}
static const char* help2(void) {
	return screen_options_test_split_helper(1);
}
static const char* join_line = "When active, second controller presses A to join.";
static void t_astra_wording(void) {
	static struct entity guest;
	unsigned before = saves;

	/* -- title screen: no server, so the helper must NOT claim an immediate
	 * apply. The value is committed with the other Options keys and the next
	 * world load asks for it (case (s) proves that path). */
	gstate.world_loaded = false;
	reset_case(false, 0);
	assert(!strcmp(row(), "Local split-screen: OFF"));
	assert(!strcmp(help1(), "Applies when the next world loads."));
	assert(!strcmp(help2(), join_line));
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 0); /* nothing asked, as case (f) already pins */
	assert(!strcmp(row(), "Local split-screen: ON"));
	/* The title-screen edit is the row's PENDING value, not an acknowledged
	 * one, so the wording still describes the next world load -- and the
	 * enabled-availability helper must not appear here. */
	assert(!strcmp(help1(), "Applies when the next world loads."));
	assert(!strcmp(help2(), join_line));
	gstate.world_loaded = true;

	/* -- in-world, settled OFF: the live marker, and the join gesture. */
	reset_case(false, 0);
	assert(!strcmp(row(), "Local split-screen: OFF"));
	assert(!strcmp(help1(), "Experimental - applies immediately."));
	assert(!strcmp(help2(), join_line));

	/* -- in-world, request in flight: the status owns line 1. */
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 1);
	uint32_t seq = seen[0].payload.split_set.request_seq;
	assert(!strcmp(row(), "Local split-screen: Applying..."));
	assert(!strcmp(status(), "Applying..."));
	assert(!strcmp(help1(), "Applying..."));
	assert(!strcmp(help2(), join_line));

	/* -- granted: settled ON with no guest yet. This is "enabled
	 * availability", and the approved sentence pair is what the block says.
	 * It is 71 characters against a 48-character line, so it is shown across
	 * the block's two existing lines rather than truncated -- the whole point
	 * being the second half, which a player needs BEFORE enabling. */
	server_ack(true, true, epoch, seq, "");
	assert(poll_frame(0.0F));
	assert(!strcmp(row(), "Local split-screen: ON"));
	assert(status()[0] == '\0');
	assert(!strcmp(help1(), "A second player can join."));
	assert(!strcmp(help2(), "View distance is reduced during split-screen."));
	assert(strlen(help1()) + 1 + strlen(help2()) == 71);

	/* -- ON with player 2 already playing: nobody else can join, so the
	 * availability sentence stands down and the block is the live marker plus
	 * the unchanged join gesture. */
	gstate.players[1].local_player = &guest;
	assert(!strcmp(help1(), "Experimental - applies immediately."));
	assert(!strcmp(help2(), join_line));

	/* -- OFF with player 2 present: new joins are closed and the guest keeps
	 * playing (the server never ejects an admitted body). No confirmation. */
	reset_case(false, 0);
	assert(!strcmp(row(), "Local split-screen: OFF"));
	assert(!strcmp(help1(), "Experimental - applies immediately."));
	assert(!strcmp(help2(), "New joins disabled. Player 2 can keep playing."));
	gstate.players[1].local_player = NULL;

	/* -- the acked-word variants (b)/(e) do not reach. A refusal while the
	 * acknowledged switch is ON reads "ON (failed)"... */
	reset_case(true, 1);
	assert(screen_options_test_split_set(0));
	drain();
	assert(seen_n == 1);
	server_ack(false, true, epoch, seen[0].payload.split_set.request_seq,
			   "guest still joining");
	assert(poll_frame(0.0F)); /* still ON: the transition did not happen */
	assert(!strcmp(row(), "Local split-screen: ON (failed)"));
	assert(!strcmp(status(), "Could not apply: guest still joining"));
	assert(!strcmp(help1(), "Could not apply: guest still joining"));

	/* ...and a request nobody answers while the acknowledged switch is OFF
	 * reads "OFF (no answer)". */
	reset_case(false, 0);
	assert(screen_options_test_split_set(1));
	drain();
	assert(seen_n == 1);
	assert(!poll_frame(6.0F)); /* past SPLIT_ACK_TIMEOUT_S */
	assert(!strcmp(row(), "Local split-screen: OFF (no answer)"));
	assert(!strcmp(status(), "No answer from the server."));
	assert(!strcmp(help1(), "No answer from the server."));
	assert(!strcmp(help2(), join_line));

	/* Not one of the wording states above reached for config_save: the
	 * strings are a render decision, and the key still travels on the Options
	 * transaction (A8-M6). */
	assert(saves == before);
	printf("PASS (t) S-A13 approved wording: title \"Applies when the next "
		   "world loads.\" vs live \"Experimental - applies immediately.\"; "
		   "enabled availability \"%s %s\"; OFF with player 2 \"New joins "
		   "disabled. Player 2 can keep playing.\"; acked word in both "
		   "variants (\"ON (failed)\", \"OFF (no answer)\"); no truncation, "
		   "no config_save\n",
		   "A second player can join.",
		   "View distance is reduced during split-screen.");
	reset_case(false, 0);
}

/* (u) Approved leave-before-OFF coordinator. The row still owns transport and
 * acknowledgement truth; this proves the coordinator adds only ordering,
 * exact correlation and the non-dismissible recovery latch. */
static void u_leave_before_off_coordinator(void) {
	struct client_session guest = {0};

	reset_case(true, 1);
	assert(screen_options_test_split_off_open());
	assert(screen_options_test_split_off_focus() == 0); /* Keep on */
	assert(!screen_options_split_off_blocks_join());
	screen_options_test_split_off_choose(false, &guest);
	drain();
	assert(seen_n == 0 && !screen_options_split_off_blocks_join());

	/* A confirmation belongs to the world in which it opened. */
	assert(screen_options_test_split_off_open());
	epoch++;
	screen_options_split_off_tick(&guest, epoch, NULL);
	assert(!screen_options_split_off_blocks_join());
	assert(screen_options_test_split_off_rearm_step(true)); /* same-frame Back */
	assert(screen_options_test_split_off_rearm_step(false)); /* neutral frame */
	assert(!screen_options_test_split_off_rearm_step(false));
	epoch--;

	/* Epoch retirement returns an in-flight exact leave receipt to the normal
	 * guest lifecycle instead of silently suppressing its retry owner. */
	reset_case(true, 1);
	guest = (struct client_session) {.local_phase = CLIENT_GUEST_ATTACHED,
		.local_ticket = 76, .local_generation = 8, .local_bound = true};
	assert(screen_options_test_split_off_open());
	screen_options_test_split_off_choose(true, &guest);
	assert(screen_options_split_off_blocks_join()
		&& guest.local_leave_receipt_manual_retry);
	epoch++;
	screen_options_split_off_tick(&guest, epoch, NULL);
	assert(!screen_options_split_off_blocks_join()
		&& !guest.local_leave_receipt_manual_retry);
	assert(!client_session_local_leave_release(&guest,
		&guest.local_leave_receipt)); /* already handed back exactly once */
	client_session_clear_windows(&guest);
	epoch--;

	/* With no guest, confirmation authors exactly one OFF. An unrelated ack may
	 * reconcile row truth but cannot settle this exact transaction. */
	reset_case(true, 1);
	assert(screen_options_test_split_off_open());
	screen_options_test_split_off_choose(true, &guest);
	assert(screen_options_split_off_blocks_join());
	drain();
	assert(seen_n == 1 && !seen[0].payload.split_set.enabled);
	uint32_t seq = seen[0].payload.split_set.request_seq;
	struct clin_split_ack unrelated = {.valid = true, .ok = true, .enabled = false,
		.generation = epoch, .request_seq = seq + 100};
	(void)screen_options_split_poll(&unrelated, 0.0F);
	screen_options_split_off_tick(&guest, epoch, &unrelated);
	assert(screen_options_split_off_blocks_join());
	struct clin_split_ack exact = {.valid = true, .ok = true, .enabled = false,
		.generation = epoch, .request_seq = seq};
	(void)screen_options_split_poll(&exact, 0.0F);
	screen_options_split_off_tick(&guest, epoch, &exact);
	assert(!screen_options_split_off_blocks_join());
	assert(screen_options_test_split_off_rearm_step(true)); /* same-frame A */
	assert(screen_options_test_split_off_rearm_step(false));
	assert(!screen_options_test_split_off_rearm_step(false));

	/* A live guest must produce authoritative LEFT before OFF is even queued. */
	reset_case(true, 1);
	guest = (struct client_session) {.local_phase = CLIENT_GUEST_ATTACHED,
		.local_ticket = 77, .local_generation = 9, .local_bound = true};
	assert(screen_options_test_split_off_open());
	screen_options_test_split_off_choose(true, &guest);
	assert(screen_options_split_off_blocks_join());
	client_session_flush_local(&guest);
	local_leave_seen = 0;
	svin_process_owned_messages(grab_owned, NULL, false);
	assert(local_leave_seen == 1);
	drain();
	assert(seen_n == 0);
	struct client_rpc left = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 77, .generation = 8,
			.result = LOCAL_GUEST_LEFT}};
	client_session_local_status(&guest, &left);
	screen_options_split_off_tick(&guest, epoch, NULL);
	drain();
	assert(seen_n == 0); /* stale generation */
	left.payload.local_guest.generation = 9;
	client_session_local_status(&guest, &left);
	screen_options_split_off_tick(&guest, epoch, NULL);
	drain();
	assert(seen_n == 1 && !seen[0].payload.split_set.enabled);
	seq = seen[0].payload.split_set.request_seq;
	exact = (struct clin_split_ack) {.valid = true, .ok = true, .enabled = false,
		.generation = epoch, .request_seq = seq};
	(void)screen_options_split_poll(&exact, 0.0F);
	screen_options_split_off_tick(&guest, epoch, &exact);
	assert(!screen_options_split_off_blocks_join());
	assert(screen_options_test_split_off_rearm_step(true));
	assert(screen_options_test_split_off_rearm_step(false));
	assert(!screen_options_test_split_off_rearm_step(false));

	/* A refused initial enqueue has no coordinator sequence. Even a fabricated
	 * current-world seq-0 OFF statement cannot complete that committed modal. */
	reset_case(true, 1);
	for(unsigned k = 0; k < 16; k++) {
		struct server_rpc fill = {.type = SRPC_HOTBAR_SLOT,
			.payload.hotbar_slot = {.slot = 0}};
		assert(svin_rpc_send(&fill));
	}
	assert(screen_options_test_split_set(0)); /* spends the split reserve */
	uint32_t held_epoch = epoch;
	epoch++;
	reset_case(true, 1);
	assert(screen_options_test_split_off_open());
	screen_options_test_split_off_choose(true, &guest);
	assert(screen_options_split_off_blocks_join()
		&& screen_options_test_split_off_request_seq() == 0);
	struct clin_split_ack zero = {.valid = true, .ok = true, .enabled = false,
		.generation = epoch, .request_seq = 0};
	(void)screen_options_split_poll(&zero, 0.0F);
	screen_options_split_off_tick(&guest, epoch, &zero);
	assert(screen_options_split_off_blocks_join());
	epoch = held_epoch;
	screen_options_split_off_tick(&guest, epoch, NULL); /* retire old world */
	while(svin_pending_count() > 0 || seen_n == 0)
		drain();
	drain();
	seen_n = 0;

	/* A timed-out OFF cannot be dismissed. Retry authors a fresh seq; a late
	 * grant for the retired seq cannot unblock the replacement request. */
	reset_case(true, 1);
	guest = (struct client_session) {0};
	assert(screen_options_test_split_off_open());
	screen_options_test_split_off_choose(true, &guest);
	drain();
	assert(seen_n == 1);
	uint32_t old_seq = seen[0].payload.split_set.request_seq;
	(void)screen_options_split_poll(NULL, 6.0F);
	screen_options_split_off_tick(&guest, epoch, NULL);
	assert(screen_options_split_off_blocks_join());
	screen_options_test_split_off_choose(false, &guest);
	assert(screen_options_split_off_blocks_join());
	assert(screen_options_test_split_off_retry(&guest));
	drain();
	assert(seen_n == 2);
	uint32_t new_seq = seen[1].payload.split_set.request_seq;
	assert(new_seq != old_seq);
	struct clin_split_ack late = {.valid = true, .ok = true, .enabled = false,
		.generation = epoch, .request_seq = old_seq};
	(void)screen_options_split_poll(&late, 0.0F);
	screen_options_split_off_tick(&guest, epoch, &late);
	assert(screen_options_split_off_blocks_join());
	exact = (struct clin_split_ack) {.valid = true, .ok = true, .enabled = false,
		.generation = epoch, .request_seq = new_seq};
	(void)screen_options_split_poll(&exact, 0.0F);
	screen_options_split_off_tick(&guest, epoch, &exact);
	assert(!screen_options_split_off_blocks_join());

	printf("PASS (u) split OFF: safe-default confirmation, epoch retirement, "
		   "leave-before-OFF, exact ack correlation and mandatory retry\n");
	reset_case(false, 0);
}

static void compact_frame(struct client_session* host,
		const struct input_player_frame* frame, int height) {
	(void)local_pause_update(&host->pause, host, frame, 0,
		height == 240 ? 240 : 0, 640, height);
}

static void compact_press(struct client_session* host, int button, int height) {
	struct input_player_frame frame = {.connected = true,
		.category = INPUT_CAT_GAMECUBE};
	if(button >= 0) frame.pressed[button] = true;
	compact_frame(host, &frame, height);
}

/* (v) The admitted-player journey, through Player 1's real owner-local menu.
 * This is the access-path proof case (u) deliberately could not provide. */
static void v_compact_host_route(void) {
	struct client_session* host = &gstate.players[0];
	struct client_session* guest = &gstate.players[1];
	*host = (struct client_session) {0};
	*guest = (struct client_session) {.local_phase = CLIENT_GUEST_ATTACHED,
		.local_ticket = 91, .local_generation = 12, .local_bound = true,
		.local_gameplay_ready = true};
	static struct entity host_body;
	host->local_player = &host_body;
	reset_case(true, 1);
	assert(strcmp(screen_options_split_label(),
			  "Local split-screen: ON") == 0
		&& "fresh boot compact Options must not depend on full Options setup");
	assert(local_pause_open(&host->pause, true));
	compact_press(host, IB_GUI_DOWN, 240);
	compact_press(host, IB_GUI_DOWN, 240);
	compact_press(host, IB_GUI_CLICK, 240);
	assert(host->pause.page == LOCAL_PAUSE_OPTIONS && host->pause.selected == 0);

	/* A direction and click on one frame only moves the highlight. */
	compact_press(host, IB_GUI_CLICK, 240);
	assert(screen_options_split_off_phase() == SCREEN_SPLIT_OFF_CONFIRM);
	struct input_player_frame both = {.connected = true,
		.category = INPUT_CAT_GAMECUBE};
	both.pressed[IB_GUI_DOWN] = true;
	both.pressed[IB_GUI_CLICK] = true;
	compact_frame(host, &both, 240);
	assert(screen_options_split_off_phase() == SCREEN_SPLIT_OFF_CONFIRM
		&& host->pause.selected == 1);
	/* The full-screen Options copy of this modal declares the same property
	 * to the shared helper (its press is covered by (u)'s hooks). */
	assert(screen_options_test_split_off_destructive_dialog(0));

	/* Astra D2-destructive ruling B (tmp/jobs/next-gaps-0924/
	 * astra-D2-destructive.raw): Turn off ends Player 2's session, so this is
	 * a DESTRUCTIVE confirmation. With the valid sprite resting on one button
	 * and the ring (a direction press) on the other, the first A only moves
	 * focus under the sprite and executes NEITHER answer, whatever D2's
	 * last-input ownership says; holding A confirms nothing; the next FRESH A,
	 * focus reconciled, answers the hovered button. Both mismatch directions,
	 * a stationary and a REAPPEARING sprite (IR lost, back on the same spot:
	 * a reacquired sprite does not own focus). Only one row may actually turn
	 * split OFF, so the stationary sprite-on-Turn-off row backs out with Home
	 * after proving the focus move and the hold; the last row confirms Turn
	 * off and leads into the leave flow below. */
	static const struct {
		int sprite, ring;
		bool reappear, confirm;
	} b_rows[] = {
		{0, 1, false, true}, /* sprite Keep on, ring Turn off -> Keep on */
		{0, 1, true, true},
		{1, 0, false, false}, /* sprite Turn off, ring Keep on; Home */
		{1, 0, true, true},   /* -> Turn off: LEAVE_PENDING */
	};
	const float b_x[2] = {120.0F, 430.0F}; /* Keep on / Turn off at 640x240 */
	for(size_t k = 0; k < sizeof b_rows / sizeof b_rows[0]; k++) {
		if(screen_options_split_off_phase() == SCREEN_SPLIT_OFF_IDLE) {
			compact_press(host, -1, 240);
			compact_press(host, -1, 240);
			compact_press(host, IB_GUI_CLICK, 240);
		}
		assert(screen_options_split_off_phase() == SCREEN_SPLIT_OFF_CONFIRM);
		int x = b_rows[k].sprite, y = b_rows[k].ring;
		struct input_player_frame f = {.connected = true,
			.category = INPUT_CAT_GAMECUBE, .pointer_valid = true,
			.pointer_x = b_x[x], .pointer_y = 400.0F};
		compact_frame(host, &f, 240);
		assert(host->pause.pointer_target == x);
		f.pressed[y ? IB_GUI_DOWN : IB_GUI_UP] = true;
		compact_frame(host, &f, 240);
		f.pressed[IB_GUI_DOWN] = f.pressed[IB_GUI_UP] = false;
		assert(host->pause.selected == y);
		if(b_rows[k].reappear) {
			f.pointer_valid = false;
			compact_frame(host, &f, 240);
			f.pointer_valid = true;
			compact_frame(host, &f, 240);
			assert(host->pause.selected == y && host->pause.pointer_target == x
				&& "a reacquired stationary sprite took focus by itself");
		}
		f.pressed[IB_GUI_CLICK] = true;
		compact_frame(host, &f, 240);
		f.pressed[IB_GUI_CLICK] = false;
		drain();
		assert(screen_options_split_off_phase() == SCREEN_SPLIT_OFF_CONFIRM
			&& host->pause.selected == x && seen_n == 0
			&& "a destructive confirm acted on a sprite/ring mismatch "
			   "(ruling B)");
		f.down[IB_GUI_CLICK] = true; /* HOLD: no fresh edge */
		compact_frame(host, &f, 240);
		compact_frame(host, &f, 240);
		assert(screen_options_split_off_phase() == SCREEN_SPLIT_OFF_CONFIRM
			&& "holding A confirmed a destructive answer");
		f.down[IB_GUI_CLICK] = false;
		f.released[IB_GUI_CLICK] = true;
		compact_frame(host, &f, 240);
		f.released[IB_GUI_CLICK] = false;
		if(!b_rows[k].confirm) {
			f.pressed[IB_HOME] = true;
			compact_frame(host, &f, 240);
			drain();
			assert(screen_options_split_off_phase() == SCREEN_SPLIT_OFF_IDLE
				&& guest->local_phase == CLIENT_GUEST_ATTACHED
				&& seen_n == 0);
			continue;
		}
		f.pressed[IB_GUI_CLICK] = true; /* the FRESH, reconciled press */
		compact_frame(host, &f, 240);
		drain();
		if(x == 0)
			assert(screen_options_split_off_phase() == SCREEN_SPLIT_OFF_IDLE
				&& guest->local_phase == CLIENT_GUEST_ATTACHED
				&& seen_n == 0
				&& "the reconciled press did not answer Keep on");
		else
			assert(screen_options_split_off_phase()
					== SCREEN_SPLIT_OFF_LEAVE_PENDING
				&& screen_options_split_off_blocks_join()
				&& "the reconciled press did not answer Turn off");
	}
	/* Confirmed from the same compact page. No OFF request may precede LEFT. */
	client_session_flush_local(guest);
	local_leave_seen = 0;
	svin_process_owned_messages(grab_owned, NULL, false);
	assert(local_leave_seen == 1);
	drain();
	assert(seen_n == 0);

	struct client_rpc left = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 91, .generation = 12,
			.result = LOCAL_GUEST_LEFT}};
	client_session_local_status(guest, &left);
	screen_options_split_off_tick(guest, epoch, NULL);
	/* LEFT expands the host from 240 to 480 before OFF settles. The same local
	 * page remains the owner and consumes a neutral full-height frame. */
	compact_press(host, -1, 480);
	assert(host->pause.open && host->pause.page == LOCAL_PAUSE_OPTIONS);
	drain();
	assert(seen_n == 1 && !seen[0].payload.split_set.enabled);
	uint32_t old_seq = seen[0].payload.split_set.request_seq;

	/* Timeout is non-dismissible. The compact Retry authors a fresh sequence. */
	(void)screen_options_split_poll(NULL, 6.0F);
	screen_options_split_off_tick(guest, epoch, NULL);
	assert(screen_options_split_off_phase() == SCREEN_SPLIT_OFF_REQUEST_FAILED);
	compact_press(host, IB_GUI_CLICK, 480);
	drain();
	assert(seen_n == 2);
	uint32_t seq = seen[1].payload.split_set.request_seq;
	assert(seq != 0 && seq != old_seq);
	struct clin_split_ack exact = {.valid = true, .ok = true, .enabled = false,
		.generation = epoch, .request_seq = seq};
	(void)screen_options_split_poll(&exact, 0.0F);
	screen_options_split_off_tick(guest, epoch, &exact);
	assert(screen_options_split_off_phase() == SCREEN_SPLIT_OFF_IDLE
		&& !screen_options_split_off_blocks_join());

	/* Completion-frame input and the neutral frame are quarantined. A later
	 * fresh Back returns to the host Game menu with Options focused. */
	compact_press(host, IB_GUI_CLICK, 480);
	assert(host->pause.page == LOCAL_PAUSE_OPTIONS);
	compact_press(host, -1, 480);
	assert(host->pause.page == LOCAL_PAUSE_OPTIONS);
	compact_press(host, IB_HOME, 480);
	assert(host->pause.page == LOCAL_PAUSE_MENU && host->pause.selected == 2);
	puts("PASS (v) compact host Options: admitted P2 route, safe cancel, LEFT-before-OFF, retry/exact ACK, 240->480 survival and neutral rearm");
	reset_case(false, 0);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(svin_init());
	/* An empty in-memory user config: config_create only parses a FILE, and
	 * this test must not read or write the repo's own config.json. Same two
	 * fields config_create populates. */
	gstate.config_user.root = json_value_init_object();
	assert(gstate.config_user.root);
	gstate.config_user.ephemeral_key_count = 0;
	/* The in-world predicate screen_options.c checks: a local world that is
	 * loaded, with a local player and an ACKED input epoch. */
	static struct entity host;
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	gstate.remote_session = false;
	gstate.players[0].local_player = &host;
	epoch = 41;

	a_toggle_on_then_ok();
	b_ack_refused();
	c_superseded();
	d_ignored_acks();
	e_timeout();
	f_title_screen();
	g_late_ack_after_timeout();
	h_send_failure();
	i_coalescer_and_reserve();
	j_reopen();
	k_guest_maintenance();
	/* A8-M6, stated as a measurement: every acknowledged toggle above moved
	 * the config key and not ONE of them called config_save from the
	 * per-frame ack seam. The Options transaction carries the value. Sampled
	 * HERE, before (l), because (l) is the deliberate flush. */
	unsigned ack_seam_saves = saves;
	assert(ack_seam_saves == 0);
	l_unload_flush();
	assert(saves == 1);
	m_two_frame_poll();
	assert(saves == 2);
	n_timeout_after_an_answered_toggle();
	o_send_failure_caption_survives();
	p_ack_slot_is_an_event();
	q_reopen_both_directions();
	r_refusal_reports_but_does_not_rewrite();
	s_world_load_reseed();
	t_astra_wording();
	u_leave_before_off_coordinator();
	v_compact_host_route();
	/* ...and the three deliberate teardown flushes are the ONLY writes in the
	 * whole run: one per dirty toggle, nothing from the ack seam -- including
	 * across the 240-odd extra frames (n)/(o)/(m) poll, and nothing from the
	 * per-world re-seed in (s), which reads the file and never writes it. */
	assert(saves == 3);

	gstate.players[0].local_player = NULL;
	printf("test_split_live_options: PASS (%u requests observed, %u blocking "
		   "config saves from the ack seam, %u from the unload flush)\n",
		   seen_total, ack_seam_saves, saves - ack_seam_saves);
	return 0;
}
