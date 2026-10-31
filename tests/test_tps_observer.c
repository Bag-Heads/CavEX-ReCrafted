/* Copyright (c) 2026 CaveWii_v2 contributors
 * Accounting/protocol test, NOT a deterministic server replay or CPU result.
 */
#include "tps_observer.h"
#include "game/hw_run.h"
#include "network/server_local_state.h"
#include "block/blocks.h"
#include "platform/hw_tel_ring.h"
#include "support/test_fixture_dir.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t now, step = 1000;
static uint64_t fake_cpu(void) { return now += step; }
static unsigned emitted, ends;
static uint64_t emit_now;
static size_t emitted_bytes;
static unsigned summary_records, seal_records;
static char last_seal[224];
static bool capture_seals;
static uint64_t fake_emit_clock(void) { return emit_now; }
static unsigned published_ticks;
static struct tps_census census_sum;
static void interrupted_publisher(void) {
	struct tps_cpu_evidence e;
	tps_observer_snapshot(&e);
	assert(e.enabled && e.ticks == published_ticks);
}
static void close_during_publication(void) { tps_observer_frame(false); }
static unsigned row_publications;
static void close_second_row(void) {
	if(++row_publications == 2) tps_observer_close(false);
}
static bool sink(const char* line) {
	assert(strlen(line) <= 1754);
	if(capture_seals) puts(line);
	if(strstr(line, "\"event\":\"tps_summary\"")) {
		emitted++;
		summary_records += (unsigned)strtoul(strstr(line, "\"count\":") + 8, NULL, 10);
		emitted_bytes += strlen(line) + 1 + 256;
		assert(strstr(line, "\"phase_ns\":[") && strstr(line, "\"nested_ns\":["));
		assert(strstr(line, "\"census_version\":3"));
		const char* p = strstr(line, "\"census\":[");
		assert(p);
		p += strlen("\"census\":[");
		for(unsigned i = 0; i < TPS_C_COUNT; i++) {
			char* end;
			census_sum.value[i] += strtoull(p, &end, 10);
			assert(end != p && *end == (i + 1 == TPS_C_COUNT ? ']' : ','));
			p = end + 1;
		}
	} else {
		assert(strstr(line, "\"event\":\"tps_end\""));
		ends++;
		seal_records = (unsigned)strtoul(strstr(line, "\"records\":") + 10, NULL, 10);
		snprintf(last_seal, sizeof(last_seal), "%s", line);
	}
	return true;
}

/* Use the production three-slot observer admission, including a slot held
 * by the sender until pop. Publication never drains it on the caller's behalf. */
static struct cw_tel_ring tps_ring;
static unsigned ring_attempts, received_seq, received_last;
static bool received_ordered;
static unsigned attempted_seq;
static unsigned release_on_refusal;
static char attempted_summary[2048];
static void receive_one(void);
static bool ring_sink(const char* line) {
	ring_attempts++;
	if(strstr(line, "\"event\":\"tps_summary\"")) {
		unsigned seq = (unsigned)strtoul(strstr(line, "\"seq\":") + 6, NULL, 10);
		if(seq == attempted_seq) assert(!strcmp(line, attempted_summary));
		attempted_seq = seq;
		snprintf(attempted_summary, sizeof(attempted_summary), "%s", line);
	}
	bool admitted = cw_tel_publish_observer(&tps_ring, line);
	/* Optional deterministic sender progress between final-flush attempts. */
	if(!admitted && release_on_refusal) {
		release_on_refusal--;
		receive_one();
	}
	return admitted;
}
static void receive_one(void) {
	const char* line = cw_tel_peek(&tps_ring);
	assert(line);
	if(strstr(line, "\"event\":\"tps_summary\"")) {
		assert(!ends); /* The seal never overtakes an admitted summary. */
		unsigned seq = (unsigned)strtoul(strstr(line, "\"seq\":") + 6, NULL, 10);
		unsigned first = (unsigned)strtoul(strstr(line, "\"first\":") + 8, NULL, 10);
		unsigned last = (unsigned)strtoul(strstr(line, "\"last\":") + 7, NULL, 10);
		unsigned count = (unsigned)strtoul(strstr(line, "\"count\":") + 8, NULL, 10);
		if(seq != received_seq + 1 || first != received_last + 1)
			received_ordered = false;
		received_seq = seq;
		assert(last - first + 1 == count);
		received_last = last;
		assert(sink(line));
	} else if(strstr(line, "\"event\":\"tps_end\"")) assert(sink(line));
	cw_tel_pop(&tps_ring);
}
static void receive_all(void) {
	while(cw_tel_peek(&tps_ring)) receive_one();
}
static void ring_start(const char* prefix) {
	memset(&tps_ring, 0, sizeof(tps_ring));
	memset(&census_sum, 0, sizeof(census_sum));
	emit_now = 0;
	ring_attempts = received_seq = received_last = attempted_seq = 0;
	release_on_refusal = 0;
	received_ordered = true;
	emitted = ends = summary_records = seal_records = 0;
	assert(tps_observer_start(prefix));
	tps_observer_output(ring_sink);
	tps_observer_frame(true);
	for(unsigned i = 0; i < CW_TEL_SLOTS - 1; i++)
		assert(cw_tel_publish_observer(&tps_ring, "{\"event\":\"other_observer\"}"));
	assert(cw_tel_peek(&tps_ring)); /* Sender owns the first slot, but stalls. */
}
static void measured_ticks(struct server_local* s, unsigned count) {
	unsigned before = ring_attempts;
	for(unsigned i = 0; i < count; i++) {
		server_tps_boundary(s, true);
		server_tps_boundary(s, false);
	}
	assert(ring_attempts == before); /* Tick producer performs no publication. */
}
static void test_ring_backpressure(struct server_local* s, const char* prefix) {
	/* RED on e226823bd: seq 1 is discarded at the first refusal, and
	 * received records=1 disagrees with seal records=6. */
	ring_start(prefix);
	measured_ticks(s, 2);
	emit_now = UINT64_C(1000000000);
	tps_observer_frame(true);
	assert(tps_ring.drops && !emitted);
	measured_ticks(s, 3);
	emit_now += UINT64_C(1000000000);
	tps_observer_frame(true);
	receive_one();
	tps_observer_frame(true);
	receive_all();
	tps_observer_frame(true);
	measured_ticks(s, 1);
	tps_observer_frame(false);
	receive_all();
	printf("ring recovery: summaries=%u records=%u seal=%u\n", emitted, summary_records, seal_records);
	fflush(stdout);
	assert(ends == 1 && summary_records == 6 && seal_records == summary_records);
	assert(received_ordered && received_seq == 3 && census_sum.value[TPS_C_MUTATIONS] == 6);
	assert(strstr(last_seal, "\"stream_valid\":true"));
	assert(tps_observer_stop());
	puts("PASS observer ring saturation: immutable retries, ordered summaries, matching valid seal");

	/* Close with both a retained summary and a newer aggregate. The frame
	 * path yields to later publication opportunities; it cannot reopen. */
	ring_start(prefix);
	measured_ticks(s, 2);
	emit_now = UINT64_C(1000000000);
	tps_observer_frame(true);
	measured_ticks(s, 3);
	emit_now += UINT64_C(1000000000);
	tps_observer_frame(false);
	assert(!ends);
	receive_one();
	emit_now += UINT64_C(100000000);
	tps_observer_frame(true); /* Closing forces measuring=false. */
	measured_ticks(s, 1); /* Outside the frozen measurement prefix. */
	receive_all();
	tps_observer_frame(false);
	receive_all();
	assert(ends == 1 && summary_records == 5 && seal_records == 5);
	assert(received_ordered);
	assert(strstr(last_seal, "\"stream_valid\":true"));
	assert(strtoull(strstr(last_seal, "\"measured_ns\":") + 14, NULL, 10)
		== UINT64_C(2000000000));
	assert(tps_observer_stop());
	puts("PASS deferred close: all pending summaries precede seal; measurement span stays frozen");

	/* A permanently blocked ring exhausts the final opportunity bound.
	 * Once transport resumes the seal must admit the actual loss honestly. */
	ring_start(prefix);
	measured_ticks(s, 2);
	emit_now = UINT64_C(1000000000);
	tps_observer_frame(true);
	measured_ticks(s, 3);
	tps_observer_frame(false);
	for(unsigned i = 0; i < 256; i++) {
		unsigned before = ring_attempts;
		tps_observer_frame(false);
		assert(ring_attempts - before <= 3); /* Fixed work; no waiting loop. */
	}
	receive_all();
	tps_observer_frame(false);
	receive_all();
	assert(ends == 1 && seal_records == 5 && !summary_records);
	assert(strstr(last_seal, "\"stream_valid\":false"));
	assert(strstr(last_seal, "observer_error"));
	assert(!tps_observer_stop());
	puts("PASS bounded final flush: true summary loss reports invalid seal, never fabricated completeness");

	/* Refuse only the seal: accepting the final summary fills the last slot.
	 * Retry the seal without duplicating that summary or extending the span. */
	ring_start(prefix);
	measured_ticks(s, 2);
	receive_one();
	emit_now = UINT64_C(1000000000);
	tps_observer_frame(false);
	receive_all();
	assert(emitted == 1 && !ends);
	emit_now += UINT64_C(100000000);
	tps_observer_frame(false);
	receive_all();
	assert(ends == 1 && summary_records == 2 && seal_records == 2);
	assert(received_ordered && received_seq == 1);
	assert(strstr(last_seal, "\"stream_valid\":true"));
	assert(strtoull(strstr(last_seal, "\"measured_ns\":") + 14, NULL, 10)
		== UINT64_C(1000000000));
	tps_observer_close(false);
	receive_all();
	assert(ends == 1 && tps_observer_stop());
	puts("PASS refused seal retries without duplicate summaries or extending the measurement");

	/* The time bound works even when very few publication opportunities occur. */
	ring_start(prefix);
	measured_ticks(s, 1);
	tps_observer_frame(false);
	emit_now = UINT64_C(2000000000);
	tps_observer_frame(false);
	receive_all();
	tps_observer_frame(false);
	receive_all();
	assert(ends == 1 && seal_records == 1 && !summary_records);
	assert(strstr(last_seal, "\"stream_valid\":false"));
	assert(!tps_observer_stop());
	puts("PASS final flush expires at the time bound, independent of frame cadence");

	/* Sender progress between explicit final attempts can recover the run.
	 * Every admission still tries once; the producer never waits for progress. */
	ring_start(prefix);
	measured_ticks(s, 2);
	release_on_refusal = 2;
	tps_observer_close(true);
	receive_all();
	assert(ends == 1 && received_ordered && received_seq == 1);
	assert(summary_records == 2 && seal_records == 2);
	assert(strstr(last_seal, "\"stream_valid\":true"));
	assert(tps_observer_stop());
	puts("PASS explicit final flush recovers when sender frees slots between attempts");

	/* Lifecycle callers have no later frame guarantee. Close does only a
	 * small fixed amount of work, loses refused summaries explicitly, and
	 * keeps an invalid seal eligible if transport later becomes available. */
	ring_start(prefix);
	measured_ticks(s, 1);
	unsigned before = ring_attempts;
	tps_observer_close(false);
	assert(ring_attempts - before <= 15);
	struct tps_cpu_evidence evidence;
	tps_observer_snapshot(&evidence);
	assert(!evidence.valid);
	receive_all();
	tps_observer_frame(false);
	receive_all();
	assert(ends == 1 && seal_records == 1 && !summary_records);
	assert(strstr(last_seal, "\"stream_valid\":false"));
	assert(!tps_observer_stop());
	puts("PASS explicit close uses bounded nonwaiting attempts and marks true loss invalid");

	/* A long mid-window stall retains just one immutable line and accumulates
	 * all newer ticks; it must not create an unbounded per-second backlog. */
	ring_start(prefix);
	measured_ticks(s, 2);
	emit_now = UINT64_C(1000000000);
	tps_observer_frame(true);
	for(unsigned second = 0; second < 16; second++) {
		measured_ticks(s, 20);
		emit_now += UINT64_C(1000000000);
		tps_observer_frame(true);
	}
	assert(attempted_seq == 1 && tps_observer_static_bytes() <= 65536);
	receive_all();
	tps_observer_frame(false);
	receive_all();
	assert(ends == 1 && summary_records == 322 && seal_records == 322);
	assert(received_ordered && received_seq == 2);
	assert(census_sum.value[TPS_C_MUTATIONS] == 322);
	assert(strstr(last_seal, "\"stream_valid\":true"));
	assert(tps_observer_stop());
	puts("PASS bounded mid-window retention: 322 ticks survive 16 saturated windows exactly");

	/* Transport that closes permanently cannot carry even an invalid seal.
	 * Teardown must report failure, rather than claim successful evidence. */
	ring_start(prefix);
	measured_ticks(s, 1);
	cw_tel_close(&tps_ring);
	tps_observer_close(true);
	assert(!tps_observer_stop());
	receive_all();
	assert(!ends && !summary_records);
	puts("PASS permanently closed transport: missing seal fails observer teardown closed");
}

static void test_duration_seal(struct server_local* s, const char* prefix) {
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	hw_run_apply_profile(&cfg, HW_RUN_PROFILE_FAR_WALK_6M);
	cfg.tps_cpu = true;
	cfg.duration_seconds = 10;
	assert(!hw_run_tps_duration_end(&cfg, 7.999F));
	assert(hw_run_tps_duration_end(&cfg, 8.0F));
	assert(hw_run_tps_duration_end(&cfg, 10.0F));
	for(unsigned abort = 0; abort < 3; abort++) {
		struct hw_run_tps_window window = {0};
		emit_now = 0;
		summary_records = seal_records = ends = 0;
		assert(tps_observer_start(prefix));
		tps_observer_output(sink);
		capture_seals = true;
		puts("{\"event\":\"tps_window\",\"kind\":\"single_anchor\",\"settle_ms\":5000}");
		assert(!hw_run_tps_measuring(&window, &cfg, true, false, false, 0));
		assert(hw_run_tps_measuring(&window, &cfg, true, false, false, 5000));
		emit_now = UINT64_C(5000000000);
		tps_observer_frame(window.measuring);
		for(unsigned i = 0; i < 3; i++) {
			server_tps_boundary(s, true);
			server_tps_boundary(s, false);
		}
		emit_now += UINT64_C(1000000000);
		if(abort == 1) tps_observer_close(false);
		else {
			/* Both margin and a frame jumping straight to duration close. */
			float elapsed = abort == 2 ? 10.0F : 8.0F;
			emit_now = (uint64_t)elapsed * UINT64_C(1000000000);
			bool done = hw_run_tps_duration_end(&cfg, elapsed);
			tps_observer_frame(hw_run_tps_measuring(&window, &cfg,
				true, false, done, emit_now / 1000000));
		}
		assert(ends == 1 && summary_records == 3 && seal_records == summary_records);
		assert(strstr(last_seal, abort == 1 ? "\"stream_valid\":false" : "\"stream_valid\":true"));
		assert(strstr(last_seal, abort == 1 ? "aborted_or_quiesced" : "window_complete"));
		assert(strtoull(strstr(last_seal, "\"measured_ns\":") + 14, NULL, 10)
			== emit_now - UINT64_C(5000000000));
		tps_observer_close(false); /* shutdown following duration never reseals */
		assert(ends == 1);
		assert(tps_observer_stop());
		capture_seals = false;
	}
	puts("PASS single-anchor duration margin/end shutdown: seal records=3; early abort partial with reason; measured spans exact");
}

static int calibrate(const char* prefix) {
	struct server_local* s = calloc(1, sizeof(*s));
	struct thread_mutex mutex;
	assert(s && thread_mutex_init(&mutex));
	printf("clock_step_ns=%u\n", tps_cpu_probe_step_ns());
	for(unsigned run = 0; run < 5; run++) {
		uint64_t start = tps_cpu_now_ns();
		for(unsigned i = 0; i < 100000; i++) {
			thread_mutex_lock(&mutex); thread_mutex_unlock(&mutex);
		}
		uint64_t off = tps_cpu_now_ns() - start;
		assert(tps_observer_start(prefix));
		server_tps_boundary(s, true);
		start = tps_cpu_now_ns();
		for(unsigned i = 0; i < 100000; i++) {
			thread_mutex_lock(&mutex); thread_mutex_unlock(&mutex);
		}
		uint64_t on = tps_cpu_now_ns() - start;
		server_tps_boundary(s, false);
		assert(tps_observer_stop());
		printf("mutex_observer_pair_ns off=%.3f on=%.3f added=%.3f\n",
			(double)off / 100000, (double)on / 100000,
			((double)on - (double)off) / 100000);
	}
	thread_mutex_destroy(&mutex); free(s);
	return 0;
}

static tps_save_callback actual_save;
static void force_hash_collision(enum tps_save_action action, struct tps_save_input* in) {
	actual_save(action, in);
	if(action == TPS_SAVE_COMPARE_BEGIN) {
		in->candidate = true; /* force the equality-check path despite different bytes */
		in->known = false;
	}
}

static void copy_planes(struct server_chunk* c, uint8_t* bytes, bool restore) {
	uint8_t* planes[] = {c->ids, c->metadata, c->lighting_sky, c->lighting_torch, c->heightmap};
	const unsigned sizes[] = {32768, 16384, 16384, 16384, 256};
	for(unsigned i = 0, offset = 0; i < 5; offset += sizes[i++]) {
		if(restore) memcpy(planes[i], bytes + offset, sizes[i]);
		else memcpy(bytes + offset, planes[i], sizes[i]);
	}
}

static void test_production_scan_identity(struct server_local* s, const char* prefix) {
	blocks_init();
	struct server_chunk* chunks[4];
	uint8_t* before = malloc(4 * 82176);
	uint8_t* expected = malloc(4 * 82176);
	uint8_t* after = malloc(4 * 82176);
	assert(before && expected && after);
	for(unsigned i = 0; i < 4; i++) {
		assert(server_world_load_chunk(&s->world, i % 2, i / 2, &chunks[i]));
	}
	/* Loaded dictionary values can move when another member is inserted. */
	for(unsigned i = 0; i < 4; i++)
		chunks[i] = dict_server_chunks_get(s->world.chunks, S_CHUNK_ID(i % 2, i / 2));
	chunks[0]->ids[6 + (1 + 1 * 16) * 128] = BLOCK_GLOWSTONE;
	for(unsigned x = 8; x <= 10; x++)
		for(unsigned z = 8; z <= 10; z++)
			chunks[3]->ids[80 + (z + x * 16) * 128] = BLOCK_STONE;
	for(unsigned i = 0; i < 4; i++) {
		/* Exercise the last physical bit of every column through the real
		 * count/fill kernels and compare the resulting light planes. */
		chunks[i]->ids[CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT - 1] = BLOCK_GLOWSTONE;
		copy_planes(chunks[i], before + i * 82176, false);
	}
	server_world_test_light_memo_mode(1);
	assert(server_world_test_relight_columns(&s->world, 0, 0));
	server_world_test_light_memo_mode(0);
	for(unsigned i = 0; i < 4; i++) {
		copy_planes(chunks[i], expected + i * 82176, false);
		copy_planes(chunks[i], before + i * 82176, true);
	}
	memset(&census_sum, 0, sizeof(census_sum));
	assert(tps_observer_start(prefix));
	tps_observer_output(sink);
	tps_observer_frame(true);
	server_tps_boundary(s, true);
	assert(server_world_test_relight_columns(&s->world, 0, 0));
	server_tps_boundary(s, false);
	tps_observer_frame(false);
	assert(tps_observer_stop());
	for(unsigned i = 0; i < 4; i++) copy_planes(chunks[i], after + i * 82176, false);
	assert(!memcmp(expected, after, 4 * 82176));
	assert(census_sum.value[TPS_C_BLOCK_PAIRS] == 1);
	assert(census_sum.value[TPS_C_BLOCK_DUPLICATES] == 0);
	assert(census_sum.value[TPS_C_BLOCK_COUNT_CELLS] == 131072);
	assert(census_sum.value[TPS_C_BLOCK_FILL_CELLS] == 0);
	assert(census_sum.value[TPS_C_BLOCK_CONSUMED_CELLS] == 131072);
	assert(census_sum.value[TPS_C_BLOCK_CONSUMED_SOURCES] >= 4);
	assert(census_sum.value[TPS_C_SKY_PAIRS] == 1);
	assert(census_sum.value[TPS_C_SKY_DUPLICATES] == 0);
	assert(census_sum.value[TPS_C_SKY_COUNT_CELLS] == 131072);
	assert(census_sum.value[TPS_C_SKY_FILL_CELLS] == 0);
	assert(census_sum.value[TPS_C_SKY_FILL_CELLS] < 131072);
	free(before); free(expected); free(after);
	puts("PASS real block/sky count-fill duplicates and armed/unarmed 328704-byte relight identity");
}

static void test_save_and_light_census(struct server_local* s, const char* dir,
	const char* prefix) {
	string_t world;
	string_init_set_str(world, dir);
#ifdef _WIN32
	assert(_mkdir(test_fixture_join(dir, "/region")) == 0);
#else
	assert(mkdir(test_fixture_join(dir, "/region"), 0777) == 0);
#endif
	server_world_create(&s->world, world, WORLD_DIM_OVERWORLD, 42, true);
	s->worlds[0].session = s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	struct server_chunk sc = {0};
	uint8_t* planes = calloc(1, 82176);
	assert(planes);
	sc.ids = planes;
	sc.metadata = planes + 32768;
	sc.lighting_sky = planes + 49152;
	sc.lighting_torch = planes + 65536;
	sc.heightmap = planes + 81920;
	memset(&census_sum, 0, sizeof(census_sum));
	assert(tps_observer_start(prefix));
	tps_observer_output(sink);
	tps_observer_frame(true);
	server_tps_boundary(s, true);
	for(unsigned i = 0; i < 8; i++) {
		sc.modified = i != 4; /* one legitimate clean no-op */
		if(i == 2) sc.ids[1] = 1;
		if(i == 3) {
			sc.ids[2] = 1;
			actual_save = region_tps_save;
			region_tps_save = force_hash_collision;
		}
		if(i == 5) region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
		bool ok = server_world_save_chunk_obj(&s->world, false, 0, 0, &sc);
		assert(ok == (i != 5));
		if(i == 3) region_tps_save = actual_save;
	}
	for(unsigned different = 0; different < 2; different++) {
		struct tps_scan scan = {.kind = 0};
		world_tps_scan(TPS_SCAN_BEGIN, &scan);
		assert(scan.bits);
		for(unsigned i = 0; i < 4; i++) tps_scan_count(&scan, i, i == 1 || i == 3);
		world_tps_scan(TPS_SCAN_COUNT_END, &scan);
		world_tps_scan(TPS_SCAN_FILL_BEGIN, &scan);
		for(unsigned i = 0; i < 4; i++)
			tps_scan_fill(&scan, i, i == (different ? 0 : 1) || i == 3);
		world_tps_scan(TPS_SCAN_END, &scan);
	}
	struct tps_scan sky = {.kind = 1};
	world_tps_scan(TPS_SCAN_BEGIN, &sky);
	for(unsigned i = 0; i < 10; i++) tps_scan_count(&sky, i, i == 2 || i == 4);
	world_tps_scan(TPS_SCAN_COUNT_END, &sky);
	world_tps_scan(TPS_SCAN_FILL_BEGIN, &sky);
	for(unsigned i = 0; i < 5; i++) tps_scan_fill(&sky, i, i == 2 || i == 4);
	world_tps_scan(TPS_SCAN_END, &sky);
	struct tps_light_input light = {.world = s, .columns = {planes}, .count = 1};
	world_tps_light(&light);
	world_tps_light(&light);
	world_tps_mutation(2);
	world_tps_light(&light); /* full column input includes sky, even for a vertical rebuild */
	world_tps_mutation(1);
	world_tps_light(&light);
	light.kind = 1;
	world_tps_light(&light);
	world_tps_light(&light);
	world_tps_mutation(2);
	world_tps_light(&light); /* neighbour light changes invalidate flood input */
	light.kind = 2;
	world_tps_light(&light);
	world_tps_light(&light);
	light.x++;
	world_tps_light(&light); /* different source cohort */
	server_tps_boundary(s, false);
	tps_observer_frame(false);
	assert(tps_observer_stop());
	assert(census_sum.value[TPS_C_SAVE_CALLS] == 8);
	assert(census_sum.value[TPS_C_SAVE_WRITES] == 6);
	assert(census_sum.value[TPS_C_SAVE_UNCHANGED] == 2);
	assert(census_sum.value[TPS_C_SAVE_UNKNOWN] == 2);
	assert(census_sum.value[TPS_C_SAVE_COMPRESS_NS] > 0);
	assert(census_sum.value[TPS_C_SAVE_COMMIT_NS] > 0);
	assert(census_sum.value[TPS_C_SAVE_COMPARE_NS] > 0);
	assert(census_sum.value[TPS_C_BLOCK_PAIRS] == 2);
	assert(census_sum.value[TPS_C_BLOCK_DUPLICATES] == 1);
	assert(census_sum.value[TPS_C_BLOCK_COUNT_CELLS] == 8);
	assert(census_sum.value[TPS_C_BLOCK_FILL_CELLS] == 8);
	assert(census_sum.value[TPS_C_SKY_PAIRS] == 1);
	assert(census_sum.value[TPS_C_SKY_DUPLICATES] == 1);
	assert(census_sum.value[TPS_C_SKY_COUNT_CELLS] == 10);
	assert(census_sum.value[TPS_C_SKY_FILL_CELLS] == 5);
	assert(census_sum.value[TPS_C_VERTICAL_CALLS] == 4);
	assert(census_sum.value[TPS_C_VERTICAL_UNCHANGED] == 1);
	assert(census_sum.value[TPS_C_BLOCK_CALLS] == 3);
	assert(census_sum.value[TPS_C_BLOCK_UNCHANGED] == 1);
	assert(census_sum.value[TPS_C_SKY_CALLS] == 3);
	assert(census_sum.value[TPS_C_SKY_UNCHANGED] == 1);
	test_production_scan_identity(s, prefix);
	server_world_destroy(&s->world);
	string_clear(world);
	free(planes);
	puts("PASS census: 8 save calls, 6 writes, 2 exact unchanged, 2 changed, 2 cold; "
		"forced hash collision rejected; failed commit not cached; exact scan and mutation counts");
}

int main(int argc, char** argv) {
	if(argc == 3 && !strcmp(argv[1], "--calibrate")) return calibrate(argv[2]);
	char dir[256], prefix[300];
	test_fixture_dir("test_tps_observer", dir, sizeof(dir));
#ifdef _WIN32
	assert(_mkdir(dir) == 0);
#else
	assert(mkdir(dir, 0777) == 0);
#endif
	snprintf(prefix, sizeof(prefix), "%s/probe_", dir);
	uint32_t real_step = tps_cpu_probe_step_ns();
	printf("Native thread CPU minimum observed increment: %u ns\n", real_step);
	if(argc == 2 && !strcmp(argv[1], "--native-seals")) {
		struct server_local* s = calloc(1, sizeof(*s));
		assert(s && tps_observer_test_emit_clock(fake_emit_clock));
		test_duration_seal(s, prefix);
		free(s);
		test_fixture_dir_cleanup(dir);
		return 0;
	}
	/* A rejected native clock must never arm engine callbacks. */
	if(!real_step || real_step > 100000) {
		assert(!tps_observer_start(prefix));
		assert(!server_tps_boundary && !server_tps_observe);
	}
	assert(tps_observer_test_clock(fake_cpu));
	assert(tps_observer_start(prefix));
	assert(!tps_observer_test_clock(NULL));
	struct server_local* s = calloc(1, sizeof(*s));
	assert(s);
	step = UINT64_C(1000000);
	server_tps_boundary(s, true);                /* t=0 relative */
	tps_note(server_tps_observe, TPS_PHASE, 0);  /* t=1 */
	tps_note(world_tps_observe, TPS_ENTER, TPS_LOAD); /* t=2 */
	tps_note(world_tps_observe, TPS_ENTER, TPS_TERRAIN); /* t=3 */
	tps_note(world_tps_observe, TPS_LEAVE, TPS_TERRAIN); /* t=4 */
	tps_note(world_tps_observe, TPS_LEAVE, TPS_LOAD); /* t=5 */
	tps_note(server_tps_observe, TPS_PHASE, 1);  /* t=6 */
	tps_note(thread_tps_observe, TPS_ENTER, TPS_CHANNEL_WAIT); /* t=7 */
	tps_note(thread_tps_observe, TPS_LEAVE, TPS_CHANNEL_WAIT); /* t=8 */
	tps_note(server_tps_observe, TPS_PHASE, TPS_PHASE_COUNT); /* t=9 */
	server_tps_boundary(s, false);               /* t=10 */
	struct tps_cpu_evidence evidence;
	tps_observer_snapshot(&evidence);
	assert(evidence.enabled && evidence.valid && evidence.ticks == 1);
	assert(evidence.phase_us[0] == 5000 && evidence.phase_us[1] == 3000);
	assert(evidence.nested_us[TPS_LOAD] == 3000);
	assert(evidence.nested_us[TPS_TERRAIN] == 1000);
	assert(evidence.wait_count[0] == 1);
	/* A tick crossing either window edge is not a complete measured tick. */
	tps_observer_frame(true);
	server_tps_boundary(s, true);
	tps_observer_frame(false);
	server_tps_boundary(s, false);
	tps_observer_frame(true);
	server_tps_boundary(s, true);
	server_tps_boundary(s, false);
	assert(tps_observer_stop());
	char path[340], line[2048];
	snprintf(path, sizeof(path), "%sticks.csv", prefix);
	FILE* rows = fopen(path, "rb");
	assert(rows && fgets(line, sizeof(line), rows));
	assert(strstr(line, "terrain,initial_light,decorate"));
	assert(fgets(line, sizeof(line), rows) && !strncmp(line, "1,0,0,", 6));
	assert(fgets(line, sizeof(line), rows) && !strncmp(line, "2,0,0,", 6));
	assert(fgets(line, sizeof(line), rows) && !strncmp(line, "3,0,1,", 6));
	assert(fclose(rows) == 0);
	assert(!server_tps_boundary && !server_tps_observe && !thread_tps_observe);
	step = 1000;
	assert(tps_observer_start(prefix));
	server_tps_boundary(s, true);
	tps_note(world_tps_observe, TPS_LEAVE, TPS_LOAD); /* corrupt nesting */
	server_tps_boundary(s, false);
	tps_observer_snapshot(&evidence);
	assert(!evidence.valid);
	assert(!tps_observer_stop());
	assert(tps_observer_test_publication(interrupted_publisher));
	assert(tps_observer_start(prefix));
	server_tps_boundary(s, true);
	server_tps_boundary(s, false); /* reader during odd sequence must return */
	tps_observer_snapshot(&evidence);
	assert(evidence.ticks == 1 && evidence.valid);
	published_ticks = 1;
	server_tps_boundary(s, true);
	server_tps_boundary(s, false);
	assert(tps_observer_stop());
	assert(tps_observer_test_publication(NULL));
	assert(tps_observer_test_publication(close_during_publication));
	assert(tps_observer_start(prefix));
	tps_observer_output(sink);
	tps_observer_frame(true);
	server_tps_boundary(s, true);
	server_tps_boundary(s, false);
	tps_observer_snapshot(&evidence);
	assert(evidence.valid && !tps_observer_stop()); /* partial, not corrupt */
	assert(tps_observer_test_publication(NULL));
	emitted = ends = 0;
	assert(tps_observer_test_row_publication(close_second_row));
	assert(tps_observer_start(prefix));
	tps_observer_output(sink);
	tps_observer_frame(true);
	server_tps_boundary(s, true);
	server_tps_boundary(s, false); /* first row is part of the sealed prefix */
	server_tps_boundary(s, true);
	server_tps_boundary(s, false); /* publishes its row AFTER the partial seal */
	assert(ends == 1 && seal_records == 1 && emitted == 1);
	assert(strstr(last_seal, "\"stream_valid\":false") && strstr(last_seal, "publication_race"));
	tps_observer_frame(true); /* cannot reopen a terminal stream */
	assert(!tps_observer_stop());
	assert(ends == 1 && emitted == 1);
	assert(tps_observer_test_row_publication(NULL));
	emitted = ends = 0;
	assert(tps_observer_test_wall_mask(true));
	assert(tps_observer_start(prefix));
	step = 1000000;
	server_tps_boundary(s, true);
	tps_note(server_tps_observe, TPS_PHASE, 0);
	tps_note(thread_tps_observe, TPS_ENTER, TPS_CHANNEL_WAIT);
	tps_note(thread_tps_observe, TPS_LEAVE, TPS_CHANNEL_WAIT);
	tps_note(server_tps_observe, TPS_PHASE, TPS_PHASE_COUNT);
	server_tps_boundary(s, false);
	tps_observer_snapshot(&evidence);
	assert(evidence.valid && evidence.phase_us[0] == 2000);
	assert(evidence.nested_us[TPS_CHANNEL_WAIT] == 0);
	assert(tps_observer_stop());
	assert(tps_observer_test_wall_mask(false));
	step = 1000;
	assert(tps_observer_start(prefix));
	tps_observer_output(sink);
	tps_observer_frame(true);
	for(unsigned i = 0; i < 32; i++) {
		server_tps_boundary(s, true);
		server_tps_boundary(s, false);
	}
	tps_observer_frame(false);
	assert(emitted == 1 && ends == 1);
	assert(tps_observer_stop());
	assert(tps_observer_start(prefix));
	tps_observer_output(sink);
	tps_observer_frame(true);
	for(unsigned i = 0; i < 33; i++) {
		server_tps_boundary(s, true);
		server_tps_boundary(s, false);
	}
	tps_observer_snapshot(&evidence);
	assert(!evidence.valid); /* Never silently overwrite exact tick history. */
	assert(!tps_observer_stop());
	/* 60 frames / 20 server ticks each second: no tick emits I/O and the
	 * entire tick stream is represented by exactly one bounded summary. */
	assert(tps_observer_static_bytes() <= 65536);
	assert(tps_observer_test_emit_clock(fake_emit_clock));
	assert(tps_observer_start(prefix));
	tps_observer_output(sink);
	tps_observer_frame(true);
	for(unsigned second = 0; second < 3; second++) {
		unsigned before = emitted;
		uint64_t mutations_before = census_sum.value[TPS_C_MUTATIONS];
		emitted_bytes = 0;
		for(unsigned frame = 0; frame < 60; frame++) {
			if(frame % 3 == 0) {
				server_tps_boundary(s, true);
				server_tps_boundary(s, false);
			}
			emit_now = (uint64_t)second * 1000000000 + (uint64_t)(frame + 1) * 1000000000 / 60;
			tps_observer_frame(true);
		}
		assert(emitted == before + 1 && emitted_bytes <= 1920);
		assert(census_sum.value[TPS_C_MUTATIONS] - mutations_before == 20);
	}
	tps_observer_frame(false);
	assert(tps_observer_stop());
	assert(tps_observer_test_emit_clock(NULL));
	assert(tps_observer_test_emit_clock(fake_emit_clock));
	test_duration_seal(s, prefix);
	test_ring_backpressure(s, prefix);
	assert(tps_observer_test_emit_clock(NULL));
	printf("PASS TPS: one bounded summary/s at 20 TPS; static storage %zu bytes; no heap buffers\n", tps_observer_static_bytes());
	test_save_and_light_census(s, dir, prefix);
	assert(tps_observer_test_clock(NULL));
	free(s);
	test_fixture_dir_cleanup(dir);
	puts("PASS dormant hooks, coarse-clock rejection, exclusive phases, nested CPU, wait count, invalid nesting");
	return 0;
}
