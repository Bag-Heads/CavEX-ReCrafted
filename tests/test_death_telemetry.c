/* Real health producer -> non-anchor heartbeat, lead-1010/13 Part A. */
#include "support/split_residency_fixture.h"
#include "platform/hw_telemetry.h"
#include <stdio.h>
/* Existing production hazard path exposed by the pooled engine test seam;
 * only contact sensing is supplied, health/arbitration/commit are real. */
extern bool server_local_test_apply_actor_fire_hazards(struct server_local*,
	struct server_player*, bool, bool, bool);

static uint32_t writer_entered, writer_release, reader_retries;
static bool snapshot_guard;
static bool release_on_retry;
static void preempt_publisher(bool guard, bool writing) {
	if(guard != snapshot_guard) return;
	if(writing) {
		__atomic_store_n(&writer_entered, 1, __ATOMIC_RELEASE);
		while(!__atomic_load_n(&writer_release, __ATOMIC_ACQUIRE)) thread_msleep(1);
	} else {
		reader_retries++;
		if(release_on_retry) __atomic_store_n(&writer_release, 1, __ATOMIC_RELEASE);
	}
}
static void* publish(void* arg) {
	struct server_local* s = arg;
	if(snapshot_guard) server_local_perf_guard_before_damage(s);
	else assert(server_local_set_player_health(s, 0));
	return NULL;
}
static void preempted_snapshots(unsigned first, bool stalled) {
	for(unsigned guard = first; guard < 2; guard++) {
		struct server_local* s = split_fixture();
		server_local_perf_guard_configure(s, true, true, split_path);
		writer_entered = writer_release = reader_retries = 0;
		snapshot_guard = guard;
		release_on_retry = !stalled;
		server_local_test_telemetry_hook(preempt_publisher);
		struct thread writer;
		assert(thread_create(&writer, publish, s, 63));
		while(!__atomic_load_n(&writer_entered, __ATOMIC_ACQUIRE)) thread_msleep(1);
		uint32_t words[SERVER_LOCAL_PERF_GUARD_WORDS];
		bool valid = guard ? server_local_perf_guard_snapshot(words)
			: server_local_death_snapshot(words);
		/* Release even on RED so the assertion cannot strand the publisher. */
		__atomic_store_n(&writer_release, 1, __ATOMIC_RELEASE);
		thread_join(&writer); server_local_test_telemetry_hook(NULL);
		if(stalled) {
			assert(!valid && reader_retries == 3);
			for(unsigned i = 0; i < (guard ? SERVER_LOCAL_PERF_GUARD_WORDS
				: SERVER_LOCAL_DEATH_FIELDS); i++) assert(words[i] == 0);
		} else {
			assert(valid && reader_retries > 0);
			if(!guard) assert(words[SLD_HOST_DEATHS] == 1); /* No stale zero. */
		}
		server_local_shutdown(s); split_destroy(s);
	}
	puts(stalled ? "PASS stalled death/guard publishers: three sleeps then explicit unavailable, no stale fallback"
		: "PASS preempted death/guard publishers: blocking retry obtains fresh complete records");
}
static void unavailable_anchor(void) {
	struct hw_telemetry_heartbeat hb = {0};
	hb.anchor.enabled = true;
	assert(!server_local_death_heartbeat(false, &hb));
	assert(hb.death_record[SLD_SESSION] == 0);
	assert(hb.anchor.deaths != UINT32_MAX);
	char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	assert(hw_telemetry_format_heartbeat(line, sizeof(line), "unavailable",
		"split_anchor_6m", &hb) < sizeof(line));
	assert(strstr(line, "\"anchor_deaths\":null"));
	assert(strstr(line, "\"anchor_death_cause\":null"));
	assert(!strstr(line, "\"anchor_deaths\":4294967295"));
	puts("PASS unavailable split anchor uses null reporting fields; d1 remains unavailable");
}
static void completion(void) {
	struct server_local* s = split_fixture();
	struct hw_telemetry_heartbeat hb = {0};
	assert(!server_local_completion_heartbeat(s, &hb));
	assert(server_local_death_heartbeat(true, &hb));
	uint32_t session = hb.death_record[SLD_SESSION];
	assert(server_local_set_player_health(s, 0));
	clin_drain_discard();
	/* Real thread stop/join, not a replacement shutdown implementation. */
	assert(server_local_start(s));
	assert(!server_local_completion_heartbeat(s, &hb));
	server_local_shutdown(s);
	assert(!server_local_death_heartbeat(true, &hb));
	assert(server_local_completion_heartbeat(s, &hb));
	assert(hb.death_record[SLD_SESSION] == session);
	assert(hb.death_record[SLD_HOST_DEATHS] == 1 && hb.player_health == 0);
	static char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	char guard[1152];
	size_t n = hw_telemetry_format_perf_guard(guard, sizeof guard, hb.perf_guard_words, hb.tick);
	assert(n && n < sizeof guard);
	printf("%.*s,\"run_id\":\"completion-test\"}\n", (int)n-1, guard);
	assert(hb.perf_guard_present);
	assert(hw_telemetry_format_heartbeat(line, sizeof line, "completion-test",
		"perf_stress_5m", &hb) < sizeof line);
	puts(line);
	/* Reinitialization at the same address cannot borrow the old terminal. */
	split_destroy(s);
	s = split_fixture();
	assert(!server_local_completion_heartbeat(s, &hb));
	s->perf_stress = true; s->player.finished_loading = true;
	string_set_str(s->level_name, split_path);
	server_local_perf_guard_configure(s, true, true, split_path);
	s->perf_guard_armed = true;
	assert(server_local_set_player_health(s, 152)); clin_drain_discard();
	server_local_perf_guard_before_damage(s); clin_drain_discard();
	assert(s->perf_guard_words[SG_EFFECTIVE] && s->perf_guard_words[SG_REFILLS] == 1);
	assert(server_local_start(s)); server_local_shutdown(s);
	assert(server_local_completion_heartbeat(s, &hb));
	assert(hb.death_record[SLD_SESSION] != session && hb.death_record[SLD_HOST_DEATHS] == 0);
	assert(hb.player_health == MAX_PLAYER_HEALTH);
	assert(hb.perf_guard_words[SG_SESSION] == hb.death_record[SLD_SESSION]);
	assert(hb.perf_guard_words[SG_REQUESTED] && hb.perf_guard_words[SG_REFILLS] == 1);
	assert(hb.perf_guard_words[SG_MIN_HEALTH] == 152);
	n = hw_telemetry_format_perf_guard(guard, sizeof guard, hb.perf_guard_words, hb.tick);
	assert(n && n < sizeof guard);
	printf("%.*s,\"run_id\":\"completion-test\"}\n", (int)n-1, guard);
	assert(hb.perf_guard_present);
	assert(hw_telemetry_format_heartbeat(line, sizeof line, "completion-test",
		"perf_stress_5m", &hb) < sizeof line);
	puts(line);
	split_destroy(s);
	puts("PASS real orderly stop/join retains terminal death/health/guard; live reads retired; dead completion not healed; new session isolated");
}
int main(int argc, char** argv) {
	assert(svin_init() && clin_init());
	blocks_init();
	if(argc > 1 && !strcmp(argv[1], "--completion-red")) { completion(); return 0; }
	if(argc > 1 && !strcmp(argv[1], "--snapshot-red")) { preempted_snapshots(0, false); return 0; }
	if(argc > 1 && !strcmp(argv[1], "--guard-snapshot-red")) { preempted_snapshots(1, false); return 0; }
	if(argc > 1 && !strcmp(argv[1], "--anchor-red")) { unavailable_anchor(); return 0; }
	preempted_snapshots(0, false); preempted_snapshots(0, true); unavailable_anchor();
	uint32_t record[SERVER_LOCAL_DEATH_FIELDS];
	assert(!server_local_death_snapshot(record));
	struct server_local* s = split_fixture();
	struct hw_telemetry_heartbeat hb = {0};
	assert(server_local_death_heartbeat(true, &hb));
	uint32_t session = hb.death_record[SLD_SESSION];
	assert(session && !hb.anchor.enabled && hb.anchor.deaths == 0);
	/* A remote/disconnected/no-body heartbeat may not use even a retained
	 * local session. This is main.c's actual mapping, not a test double. */
	assert(!server_local_death_heartbeat(false, &hb));
	assert(hb.death_record[SLD_SESSION] == 0);
	assert(hb.death_record[SLD_HOST_DEATHS] == UINT32_MAX);
	/* Real actor-aware health producer. Guest death does not change host
	 * health/history; guest retirement does not erase committed deaths. */
	struct server_player guest = {.health = 8, .fire = 20,
		.has_pos = true, .finished_loading = true};
	s->guest_actor = &guest;
	assert(!server_local_test_apply_actor_fire_hazards(s, &guest, false, false, false));
	clin_drain_discard();
	assert(guest.health == 0 && s->player.health == MAX_PLAYER_HEALTH);
	assert(!server_local_heal_actor(s, &guest, 160));
	s->guest_actor = NULL;
	assert(server_local_death_heartbeat(true, &hb));
	assert(hb.death_record[SLD_HOST_DEATHS] == 0);
	assert(hb.death_record[SLD_GUEST_DEATHS] == 1);
	assert(hb.death_record[SLD_GUEST_CAUSE] == PLAYER_DAMAGE_FIRE);
	/* An attributed lethal lava hit through the real hazard/health path. */
	s->player.health = 8;
	s->player.finished_loading = true;
	s->player.hurt_resist_time = 0;
	assert(!server_local_test_apply_player_hazards(s, false, true, false, false));
	clin_drain_discard();
	assert(s->player_deaths == 1 && s->player_death_cause == PLAYER_DAMAGE_LAVA);
	for(unsigned i = 0; i < 4; i++) {
		assert(!server_local_set_player_health(s, 0));
		assert(!server_local_set_player_health(s, MAX_PLAYER_HEALTH));
		(void)server_local_test_apply_player_hazards(s, false, true, false, false);
	}
	assert(server_local_death_heartbeat(true, &hb));
	hb.player_health = s->player.health;
	assert(hb.death_record[SLD_SESSION] == session);
	assert(hb.death_record[SLD_HOST_DEATHS] == 1);
	assert(hb.death_record[SLD_HOST_CAUSE] == PLAYER_DAMAGE_LAVA);
	assert(hb.death_record[SLD_GUEST_DEATHS] == 1);
	assert(hb.anchor.deaths == 1 && !strcmp(hb.anchor.death_cause, "lava"));
	static char line[HW_TELEMETRY_HEARTBEAT_LINE_SIZE];
	size_t n = hw_telemetry_format_heartbeat(line, sizeof(line), "death-test",
		"perf_stress_5m", &hb);
	assert(n < sizeof(line));
	char expected[96];
	snprintf(expected, sizeof(expected), "\"d1\":[%u,1,3,1,4]", session);
	assert(strstr(line, expected));
	puts(line); /* Exact production-format row for the actual Python parser. */
	server_local_shutdown(s);
	assert(!server_local_death_snapshot(record));
	split_destroy(s);
	assert(!server_local_death_heartbeat(true, &hb)); /* freed allocation */
	s = split_fixture();
	assert(server_local_death_heartbeat(true, &hb));
	assert(hb.death_record[SLD_SESSION] != session);
	assert(hb.death_record[SLD_HOST_DEATHS] == 0);
	assert(hb.death_record[SLD_GUEST_DEATHS] == 0);
	server_local_shutdown(s);
	split_destroy(s);
	/* Widest valid d1 is exactly the legacy saturated count + unknown-cause
	 * width. Thus the existing full-payload/ring size proof remains a bound. */
	hb = (struct hw_telemetry_heartbeat){0};
	hb.anchor.deaths = UINT32_MAX;
	hb.anchor.death_cause = "unknown";
	size_t legacy = hw_telemetry_format_heartbeat(line, sizeof(line), "death-test",
		"perf_stress_5m", &hb);
	hb.death_present = true;
	hb.death_record[0] = hb.death_record[1] = hb.death_record[3] = UINT32_MAX;
	hb.death_record[2] = hb.death_record[4] = PLAYER_DAMAGE_SUFFOCATE;
	n = hw_telemetry_format_heartbeat(line, sizeof(line), "death-test", "perf_stress_5m", &hb);
	assert(n == legacy && n < sizeof(line));
	printf("PASS death snapshot: host lava=1 guest fire=1, repeated dead writes=0; session reset and freed/remote unavailable; widest delta=%d; heartbeat struct=%zu\n",
		(int)n - (int)legacy, sizeof(hb));
	return 0;
}
