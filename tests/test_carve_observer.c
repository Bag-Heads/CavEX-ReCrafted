/* Copyright (c) 2026 CaveWii_v2 contributors */
#include "carve_observer.h"
#include "platform/mem2.h"
#include "platform/displaylist.h"
#include "platform/thread.h"
#include "world.h"
#include "platform/hw_tel_ring.h"
#include "network/server_local_state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void* traffic(void* unused) {
	(void)unused;
	for(unsigned i = 0; i < 12000; i++) {
		void* p = mem2_tag(mem2_alloc(73), 73, CARVE_POPULATE);
		assert(p); memset(p, 42, 73);
		p = mem2_realloc(p, 73, 139); assert(p);
		for(unsigned j = 0; j < 73; j++) assert(((unsigned char*)p)[j] == 42);
		mem2_free(p);
	}
	return NULL;
}
static struct cw_tel_ring sender;
static bool queued(const char* line) { return cw_tel_publish(&sender, line); }
static unsigned packets;
static bool burst_packet(const char* line) {
	assert(strstr(line, "\"event\":\"carve_retire\""));
	assert(strstr(line, "\"count\":2"));
	assert(strstr(line, "\"io_us\":123"));
	packets++;
	return true;
}
static bool packet(const char* line) {
	assert(strlen(line) < 2048);
	assert(line[0] == '{' && line[strlen(line)-1] == '}');
	packets++;
	return true;
}
static uint64_t mesh(bool enabled) {
	if(enabled) assert(carve_observer_start());
	uint64_t hash = UINT64_C(14695981039346656037);
	for(unsigned tint = 0; tint < 2; tint++) {
		struct displaylist l;
		displaylist_init_presized(&l, 64, tint ? 12 : 9, tint, 0);
		displaylist_set_tinted(&l, tint);
		for(unsigned i = 0; i < 1700; i++)
			displaylist_vertex(&l, i, -i, i*2, i%256, i%256, (i+8)%256);
		uint32_t lists, slack, pad;
		displaylist_storage_stats(&lists, &slack, &pad);
		assert(lists == 0 && slack == 0 && pad == 0); // unfinished excluded
		size_t written = l.index;
		displaylist_finalize(&l, 1700);
		displaylist_storage_stats(&lists, &slack, &pad);
		assert(lists == 1 && slack == l.length - written && pad == 0);
		{ /* colcap-1006 R3 item 3: capacity bytes follow the same pair */
			uint32_t bl, bb;
			displaylist_storage_bytes(&bl, &bb);
			assert(bl == 1 && bb == l.length);
		}
		for(size_t j = 0; j < written; j++) { hash ^= ((unsigned char*)l.data)[j]; hash *= UINT64_C(1099511628211); }
		if(enabled) {
			struct carve_totals t; carve_observer_snapshot(&t);
			assert(!t.errors && t.count[CARVE_LIST] == 1);
			assert(t.bytes[CARVE_LIST] == l.length && t.dl_used == written);
			assert(t.dl_slack == l.length - written);
			assert(t.gx_used == ((35 + 1700*(tint ? 12 : 9) + 31) & ~31u));
			assert(t.grows > 0 && t.gx_capacity >= t.gx_used);
		}
		if(enabled) {
			struct chunk c = {0}; c.has_displist[0] = true; c.mesh[0] = l;
			world_carve_depart(&c);
			struct carve_totals t; carve_observer_snapshot(&t);
			assert(t.departed == l.length && t.departed_peak >= l.length);
		}
		displaylist_destroy(&l);
		displaylist_storage_stats(&lists, &slack, &pad);
		assert(lists == 0 && slack == 0 && pad == 0);
		{
			uint32_t bl, bb;
			displaylist_storage_bytes(&bl, &bb);
			assert(bl == 0 && bb == 0);
		}
		if(enabled) { struct carve_totals t; carve_observer_snapshot(&t); assert(t.departed == 0); }
	}
	if(enabled) { struct carve_totals t; carve_observer_snapshot(&t); assert(t.bytes[CARVE_LIST] == 0); assert(carve_observer_stop()); }
	return hash;
}

/* Final GX capacities include required NOP rounding, without growth slack
 * or the former additional recording line, independent of initial hints. */
static void gx_boundaries(void) {
	static const struct { unsigned tint, vertices, prior, capacity; } cases[] = {
		{0, 63, 0, 608}, {0, 64, 0, 640},
		{1, 63, 0, 800}, {1, 64, 0, 832},
		{0, 64, 1000, 640}, {1, 64, 1000, 832}
	};
	displaylist_set_compact(true);
	assert(carve_observer_start());
	for(unsigned j = 0; j < sizeof(cases)/sizeof(cases[0]); j++) {
		struct displaylist l;
		displaylist_init_presized(&l, 64, cases[j].tint ? 12 : 9,
			cases[j].tint, cases[j].prior);
		displaylist_set_tinted(&l, cases[j].tint);
		for(unsigned i = 0; i < cases[j].vertices; i++)
			displaylist_vertex(&l, 0, 0, 0, 0, 0, 0);
		displaylist_finalize(&l, cases[j].vertices);
		struct carve_totals t; carve_observer_snapshot(&t);
		assert(!t.errors && t.gx_capacity == cases[j].capacity);
		assert(t.gx_slack == 0 && t.dl_slack == 0);
		displaylist_destroy(&l);
	}
	assert(carve_observer_stop());
	displaylist_set_compact(false);
}

static void spill_transfers(void) {
	assert(carve_observer_start());
	carve_callback note = mem2_carve_observe;
	note(CARVE_BEGIN, 0, 0, 0, 0);
	note(CARVE_ALLOC, 4096, 0, 100, CARVE_MEM2 | CARVE_SPILLED);
	note(CARVE_TAG, 4096, 0, 100, CARVE_LIST);
	/* Failed growth keeps the old live allocation, but records the spill. */
	note(CARVE_ALLOC, 0, 0, 200, CARVE_MEM2 | CARVE_SPILLED);
	/* A successful allocate/copy/free transfers the new spill to the old
	 * tenant. The untagged NULL call remains other in this synthetic trace. */
	note(CARVE_ALLOC, 8192, 0, 200, CARVE_MEM2 | CARVE_SPILLED);
	note(CARVE_TRANSFER, 4096, 8192, 200, 100);
	note(CARVE_FREE, 4096, 0, 0, 0);
	note(CARVE_END, 0, 0, 0, 0);
	struct carve_totals t; carve_observer_snapshot(&t);
	assert(!t.errors && t.bytes[CARVE_LIST] == 200);
	assert(t.spills[CARVE_LIST] == 2 && t.spill_bytes[CARVE_LIST] == 300);
	assert(t.spills[CARVE_OTHER] == 1 && t.spill_bytes[CARVE_OTHER] == 200);
	assert(t.grows == 1 && t.copy_bytes == 100);
	note(CARVE_BEGIN, 0, 0, 0, 0);
	note(CARVE_RESIZE, 8192, 0, 400, CARVE_MEM2);
	note(CARVE_END, 0, 0, 0, 0);
	carve_observer_snapshot(&t);
	assert(t.bytes[CARVE_LIST] == 200 && t.count[CARVE_LIST] == 1);
	/* Tail release retains identity/tenant, is not a grow or a copy. */
	note(CARVE_BEGIN, 0, 0, 0, 0);
	note(CARVE_RESIZE, 8192, 8192, 128, CARVE_MEM2);
	note(CARVE_FINAL, 8192, 0, 128, 96);
	note(CARVE_END, 0, 0, 0, 0);
	carve_observer_snapshot(&t);
	assert(t.bytes[CARVE_LIST] == 128 && t.count[CARVE_LIST] == 1);
	assert(t.grows == 1 && t.moved == 1 && t.copy_bytes == 100);
	assert(t.dl_slack == 0 && t.gx_slack == 0);
	note(CARVE_BEGIN, 0, 0, 0, 0);
	note(CARVE_FREE, 8192, 0, 0, 0);
	note(CARVE_END, 0, 0, 0, 0);
	assert(carve_observer_stop());
}

static void ledger_growth_keeps_retirement_capture(void) {
	assert(carve_observer_start());
	void (*capture)(struct server_local*, unsigned, uint32_t) = server_carve_retirement;
	assert(capture);
	struct server_local s = {0};
	s.near_send_seq = 17;
	s.startup_retired = 2;
	capture(&s, 2, 123);
	carve_callback note = mem2_carve_observe;
	for(uintptr_t i = 0; i < 2049; i++)
		note(CARVE_ALLOC, (uintptr_t)0x10000 + i * 16, 0, 8, CARVE_MEM2);
	assert(server_carve_retirement == capture);
	unsigned before = packets;
	assert(carve_observer_emit_next(burst_packet)); /* empty point-0 turn */
	assert(carve_observer_emit_next(burst_packet));
	assert(packets == before + 1);
	for(uintptr_t i = 0; i < 2049; i++)
		note(CARVE_FREE, (uintptr_t)0x10000 + i * 16, 0, 0, 0);
	assert(carve_observer_stop());
}

/* 2026-09-07 hardware evidence, the SECOND finding on the same run family
 * (build e83f6f8d, split_anchor_6m, --cw-carve-census): the collector
 * received exactly 188 carve_tenant records in 63 s -- 2.98/s -- and ZERO
 * carve_census, while the drop counter climbed ~1/s from t=0. The census set
 * is CARVE_TENANTS (11) carve_tenant records PLUS one carve_census, and the
 * old code published all twelve back-to-back inside the heartbeat frame,
 * behind that frame's heartbeat and settle record, into a FOUR-slot
 * admission FIFO drained one blocking net_send at a time. Two got in, the
 * third was refused, and the whole-set loop returned at the first refusal --
 * so nine tenants and the census never even reached the queue. Staging the
 * set and draining ONE record per frame delivers all twelve with room to
 * spare. */
static unsigned paced_tenants, paced_census, paced_set_len, paced_worst_set;
static bool paced_capture(const char* line) {
	if(!cw_tel_publish(&sender, line)) return false;
	return true;
}
static void drain_paced_ring(void) {
	const char* line;
	while((line = cw_tel_peek(&sender))) {
		if(strstr(line, "\"event\":\"carve_tenant\"")) {
			paced_tenants++;
			paced_set_len++;
		} else if(strstr(line, "\"event\":\"carve_census\"")) {
			/* The census always closes its set: 11 tenants precede it. */
			assert(paced_set_len == CARVE_TENANTS);
			paced_census++;
			if(paced_set_len + 1 > paced_worst_set)
				paced_worst_set = paced_set_len + 1;
			paced_set_len = 0;
		}
		cw_tel_pop(&sender);
	}
}
/* 2026-09-07 sender-starvation brief, deliverable 2(c). One staged record per
 * frame is right only while the sender keeps up. On the real Wii during the
 * two-anchor reload it did not: build 0077cd09 (split_anchor_6m, seed 42, sep
 * 256, far 6, voxel) refused 571 of ~583 offers in 53 s, so every frame's
 * offer was a refusal -- pure drop-counter noise on top of a sender that was
 * already behind. The retry interval now derives from that measured drain
 * rate: double per refusal, capped at one offer per second, reset on the
 * first success. Nothing is dropped by the observer itself -- the cursor only
 * advances on an accepted record, so the set stays complete and in order. */
static unsigned saturated_offers;
static bool saturated_sink(const char* line) {
	(void)line;
	saturated_offers++;
	return false; /* the ring is full: the sender is not draining */
}
static unsigned recovered_tenants, recovered_census, recovered_first_frame;
static unsigned recovered_frame;
static bool recovered_sink(const char* line) {
	if(strstr(line, "\"event\":\"carve_tenant\"")) {
		if(!recovered_tenants && !recovered_census)
			recovered_first_frame = recovered_frame;
		recovered_tenants++;
		assert(!recovered_census); /* the census always closes its set */
	} else if(strstr(line, "\"event\":\"carve_census\"")) {
		assert(recovered_tenants == CARVE_TENANTS);
		recovered_census++;
	}
	/* Anything else is the attribution round-robin resuming once the staged
	 * set has drained -- not part of this test's contract. */
	return true;
}
static void census_offer_rate_backs_off_when_the_sender_stalls(void) {
	assert(carve_observer_start());
	struct world w;
	world_create(&w);
	saturated_offers = 0;
	recovered_tenants = recovered_census = 0;
	recovered_first_frame = UINT32_MAX;
	carve_observer_frame(&w, 0, 100, 0, true);
	assert(carve_observer_stage(&w, 100, true));

	/* 600 frames (10 s at 60 Hz) against a sender that never drains. The
	 * ladder is 1, 2, 4, 8, 16, 32, then the 60-frame cap, so the offers land
	 * on frames 0, 1, 3, 7, 15, 31, 63, 123, 183, 243, 303, 363, 423, 483,
	 * 543 -- fifteen, against 600 before this change. */
	for(unsigned frame = 0; frame < 600; frame++) {
		carve_observer_frame(&w, frame, 100, 0, true);
		(void)carve_observer_emit_next(saturated_sink);
	}
	assert(saturated_offers == 15);
	/* Steady state really is one offer per second, not merely "fewer". */
	assert(saturated_offers <= 7u + 600u / CARVE_STAGED_BACKOFF_MAX_FRAMES);

	/* The sender recovers: the SAME set drains completely and in order, at
	 * the restored per-frame cadence, having lost nothing to the stall. */
	for(unsigned frame = 600; frame < 700; frame++) {
		recovered_frame = frame;
		carve_observer_frame(&w, frame, 100, 0, true);
		assert(carve_observer_emit_next(recovered_sink));
	}
	assert(recovered_tenants == CARVE_TENANTS && recovered_census == 1);
	assert(recovered_first_frame == 603); /* the pending back-off is honoured */
	printf("PASS census back-off: %u offers in 600 stalled frames (was 600), "
		   "capped at one per %u frames; all %u records of the set then "
		   "delivered in order from frame %u\n", saturated_offers,
		   (unsigned)CARVE_STAGED_BACKOFF_MAX_FRAMES,
		   recovered_tenants + recovered_census, recovered_first_frame);
	world_destroy(&w);
	assert(carve_observer_stop());
}

static void census_set_paces_one_record_per_frame(void) {
	enum { SECONDS = 4 };
	assert(carve_observer_start());
	struct world w;
	world_create(&w);
	memset(&sender, 0, sizeof(sender));
	paced_tenants = paced_census = paced_set_len = paced_worst_set = 0;
	for(unsigned second = 0; second < SECONDS; second++) {
		/* main.c's heartbeat frame: heartbeat + settle publish first, then
		 * the census set is STAGED (not published). */
		assert(cw_tel_publish(&sender, "{\"event\":\"heartbeat\"}"));
		assert(cw_tel_publish(&sender, "{\"event\":\"settle\"}"));
		assert(carve_observer_stage(&w, 100 + second, true));
		for(unsigned frame = 0; frame < 60; frame++) {
			carve_observer_frame(&w, second * 60 + frame, 100 + second, 0, true);
			assert(carve_observer_emit_next(paced_capture));
			drain_paced_ring(); /* the sender keeps up between frames */
		}
	}
	/* Every record of every set arrived, in order, with nothing refused. */
	assert(paced_tenants == SECONDS * CARVE_TENANTS);
	assert(paced_census == SECONDS);
	assert(paced_worst_set == CARVE_TENANTS + 1);
	assert(sender.drops == 0);

	/* Contrast, and the regression guard: the old whole-set burst against the
	 * same four-slot FIFO with the heartbeat and settle record already in it.
	 * It refuses, and the refusal aborts the rest of the set. */
	memset(&sender, 0, sizeof(sender));
	assert(cw_tel_publish(&sender, "{\"event\":\"heartbeat\"}"));
	assert(cw_tel_publish(&sender, "{\"event\":\"settle\"}"));
	unsigned admitted = 0;
	assert(!carve_observer_emit(&w, 999, true, paced_capture));
	while(cw_tel_peek(&sender)) { admitted++; cw_tel_pop(&sender); }
	assert(admitted == CW_TEL_SLOTS);       /* 2 pre-existing + 2 carve records */
	assert(sender.drops == 1);              /* the third was refused ... */
	printf("PASS census pacing: %u sets x %u records delivered one per frame, "
		   "0 drops; the same set published as one burst admits only %u of %u "
		   "and aborts\n", (unsigned)SECONDS, CARVE_TENANTS + 1,
		   admitted - 2u, CARVE_TENANTS + 1);
	world_destroy(&w);
	assert(carve_observer_stop());
}

/* 2026-09-07 hardware evidence (build d9ce4a79, profile split_anchor_6m):
 * 1,058 carve_attrib records in 66 s (~16/s) drove tel_fail (errno -105
 * ENOBUFS) from the first second and stalled the heartbeat -- the sender's
 * ~8 KiB/s four-slot budget was saturated by a "point" that re-armed a new
 * wire record on almost every frame of memory churn. This drives 600 frames
 * (the real 60 Hz loop cadence: one carve_observer_frame + one
 * carve_observer_emit_next per frame, exactly like source/main.c's main
 * loop) of CONTINUOUS churn -- never idle -- through world_create's real
 * loading -> ready -> post-load transition and proves the emission-cadence
 * fix: the three point transitions (frame-observed loading high-water,
 * readiness, session low-water) are never coalesced away, each point's
 * later sends are capped to about one per simulated second (independently
 * per point -- see the "not a shared clock" reasoning on carve_observer_
 * frame in carve_observer.h), and the sustained byte rate stays inside the
 * sender's budget. */
struct cadence_stats {
	unsigned total, count[3];
	uint32_t last_frame[3], last_coalesced[3];
	uint64_t bytes;
};
static struct cadence_stats cadence;
static bool cadence_capture(const char* line) {
	assert(strstr(line, "\"event\":\"carve_attrib\""));
	cadence.total++;
	cadence.bytes += strlen(line) + 1; /* +1: the wire's trailing newline framing */
	const char* p = strstr(line, "\"point\":");
	assert(p);
	unsigned point = (unsigned)strtoul(p + 8, NULL, 10);
	assert(point < 3);
	p = strstr(line, "\"frame\":");
	assert(p);
	uint32_t frame = (uint32_t)strtoul(p + 8, NULL, 10);
	p = strstr(line, "\"coalesced\":");
	assert(p);
	uint32_t coalesced = (uint32_t)strtoul(p + 12, NULL, 10);
	cadence.count[point]++;
	cadence.last_frame[point] = frame;
	cadence.last_coalesced[point] = coalesced;
	return true;
}
static void attribution_cadence_capped(void) {
	enum { FRAMES = 600, READY_AT = 301 }; /* 600 @ 60 Hz == 10 simulated seconds */
	assert(carve_observer_start());
	struct world w; world_create(&w);
	void* churn[FRAMES];
	memset(&cadence, 0, sizeof(cadence));
	for(unsigned frame = 1; frame <= FRAMES; frame++) {
		/* Continuous churn: strictly growing carve usage every single frame,
		 * never idle -- the worst case for a per-frame "high-water improved"
		 * re-arm. Never freed mid-run so `used` cannot dip and hide a bug. */
		churn[frame - 1] = mem2_tag(mem2_alloc(8), 8, CARVE_COLUMN);
		assert(churn[frame - 1]);
		bool ready = frame >= READY_AT;
		carve_observer_frame(&w, frame, frame, 0, ready);
		assert(carve_observer_emit_next(cadence_capture));
	}
	for(unsigned i = 0; i < FRAMES; i++) mem2_free(churn[i]);
	/* All three point transitions occurred and were never coalesced away. */
	assert(cadence.count[0] && cadence.count[1] && cadence.count[2]);
	assert(cadence.last_frame[1] >= READY_AT); /* readiness sampled at its true boundary */
	/* Point 1 (readiness) is a genuine one-shot: no continuation exists to
	 * coalesce, so exactly one record, ever. */
	assert(cadence.count[1] == 1);
	/* Points 0 and 2 churn continuously and independently: each is capped
	 * to roughly one send per simulated second of its OWN active window
	 * (point 0 only pre-ready, ~READY_AT frames; point 2 the whole run) --
	 * generous +2 slack for round-robin/window-boundary jitter. Bounding
	 * each point separately (not just their sum) is the actual regression
	 * guard: a shared/global clock let point 2's continuous churn starve
	 * point 0 outright (observed: point 0 sent exactly once, ever, freezing
	 * its reported loading high-water at its very first, near-empty
	 * sample) -- which a sum-only bound would not have caught. */
	assert(cadence.count[0] <= (READY_AT - 1) / 60 + 2);
	assert(cadence.count[2] <= FRAMES / 60 + 2);
	/* Nowhere close to the ~16/s (~960 over 10 s) pre-fix flood. */
	assert(cadence.total < 32);
	double seconds = (double)FRAMES / 60.0;
	assert((double)cadence.bytes / seconds <= 2048.0);
	printf("PASS carve_attrib cadence: %u records (points: %u/%u/%u) over %.0f s, "
		   "%.1f B/s -- sender budget held, no point starved\n",
		   cadence.total, cadence.count[0], cadence.count[1], cadence.count[2],
		   seconds, (double)cadence.bytes / seconds);
	world_destroy(&w);
	assert(carve_observer_stop());
}

/* LOADING-PHASE CADENCE (2026-09-08). Three real-Wii split_anchor_6m runs
 * (2d8c83cb / 3efeb30e / 55190e07) lost the telemetry stream at 57 / 68 / 65
 * heartbeats, always inside the two-anchor reload: the observer offers ~12
 * records/s there (11 carve_tenant + 1 carve_census per second, plus the
 * attribution round-robin and up to 20 startup-retirement bursts/s) exactly
 * while the sender's net_send shares the IOS IPC path with the server
 * thread's libfat fsync storm. This drives 30 SIMULATED SECONDS of the
 * loading phase at the real 60 Hz main-loop cadence -- one
 * carve_observer_frame + one carve_observer_emit_next per frame, one
 * carve_observer_stage per second, exactly like source/main.c -- under
 * CONTINUOUS churn, and holds the phase to the contract: only snapshot-point
 * transitions and at most one carve_census per second reach the wire, no
 * carve_tenant record does, and every suppressed set is counted and reported
 * on the census line. Readiness must then restore the full cadence, and the
 * held point records must still go out with their real (folded) extrema --
 * suppression may never become silent loss. */
struct loading_stats {
	unsigned census, tenant, attrib, retire, other;
	unsigned points[3];
	uint32_t max_coalesced[3];
	uint64_t bytes;
	char last_census[2048];
};
static struct loading_stats loading_seen;
static bool loading_capture(const char* line) {
	loading_seen.bytes += strlen(line) + 1; /* +1: the wire's LF framing */
	if(strstr(line, "\"event\":\"carve_census\"")) {
		loading_seen.census++;
		snprintf(loading_seen.last_census, sizeof(loading_seen.last_census),
			"%s", line);
	} else if(strstr(line, "\"event\":\"carve_tenant\"")) {
		loading_seen.tenant++;
	} else if(strstr(line, "\"event\":\"carve_attrib\"")) {
		const char* p = strstr(line, "\"point\":");
		assert(p);
		unsigned point = (unsigned)strtoul(p + 8, NULL, 10);
		assert(point < 3);
		loading_seen.points[point]++;
		const char* c = strstr(line, "\"coalesced\":");
		assert(c);
		uint32_t folded = (uint32_t)strtoul(c + 12, NULL, 10);
		if(folded > loading_seen.max_coalesced[point])
			loading_seen.max_coalesced[point] = folded;
		loading_seen.attrib++;
	} else if(strstr(line, "\"event\":\"carve_retire\"")) {
		loading_seen.retire++;
	} else {
		loading_seen.other++;
	}
	return true;
}
static unsigned loading_census_field(const char* key) {
	const char* p = strstr(loading_seen.last_census, key);
	assert(p);
	return (unsigned)strtoul(p + strlen(key), NULL, 10);
}
static void loading_phase_cadence_holds_one_record_per_second(void) {
	enum { SECONDS = 30, FPS = 60, READY_SECONDS = 2 };
	assert(carve_observer_start());
	struct world w;
	world_create(&w);
	static void* churn[SECONDS * FPS];
	memset(&loading_seen, 0, sizeof(loading_seen));
	carve_observer_loading(true);
	uint32_t frame = 0;
	for(unsigned second = 0; second < SECONDS; second++) {
		/* main.c stages one census set per heartbeat, ~1 Hz. */
		assert(carve_observer_stage(&w, 100 + second, false));
		for(unsigned f = 0; f < FPS; f++, frame++) {
			/* Continuous churn: strictly growing carve usage every frame,
			 * the worst case for a per-frame "high-water improved" re-arm. */
			churn[frame] = mem2_tag(mem2_alloc(8), 8, CARVE_COLUMN);
			assert(churn[frame]);
			carve_observer_frame(&w, frame, 100 + second, 0, false);
			assert(carve_observer_emit_next(loading_capture));
		}
	}
	/* The whole point: no per-tenant sample goes out during the reload -- the
	 * eleven records of every set were suppressed, and the count says so. */
	assert(loading_seen.tenant == 0);
	assert(loading_seen.census == SECONDS);
	assert(loading_census_field("\"loading\":") == 1);
	assert(loading_census_field("\"loading_suppressed\":") == SECONDS);
	/* Only the two point transitions that exist before readiness (loading
	 * high-water and session low-water); the readiness point cannot have one
	 * yet, and neither point repeats. */
	assert(loading_seen.attrib == 2);
	assert(loading_seen.points[0] == 1 && loading_seen.points[1] == 0
		   && loading_seen.points[2] == 1);
	assert(loading_seen.retire == 0 && loading_seen.other == 0);
	unsigned loading_total = loading_seen.census + loading_seen.tenant
		+ loading_seen.attrib + loading_seen.retire + loading_seen.other;
	assert(loading_total <= SECONDS + 2); /* <= 1 record/s + the transitions */
	double loading_seconds = (double)SECONDS;
	double loading_bytes_s = (double)loading_seen.bytes / loading_seconds;
	unsigned loading_census = loading_seen.census;

	/* Readiness restores the full cadence, and nothing that was held is lost:
	 * the readiness point's own transition goes out, the held points resume,
	 * and a full eleven-tenant set drains again. */
	memset(&loading_seen, 0, sizeof(loading_seen));
	carve_observer_loading(false);
	for(unsigned second = 0; second < READY_SECONDS; second++) {
		assert(carve_observer_stage(&w, 200 + second, true));
		for(unsigned f = 0; f < FPS; f++, frame++) {
			carve_observer_frame(&w, frame, 200 + second, 0, true);
			assert(carve_observer_emit_next(loading_capture));
		}
	}
	assert(loading_seen.tenant == CARVE_TENANTS * READY_SECONDS);
	assert(loading_seen.census == READY_SECONDS);
	assert(loading_census_field("\"loading\":") == 0);
	/* The suppressed-set count is a running total, not a per-set flag. */
	assert(loading_census_field("\"loading_suppressed\":") == SECONDS);
	assert(loading_seen.points[1] == 1); /* the readiness transition */
	/* HELD, NOT DROPPED: the session low-water point kept folding EVERY
	 * observation of the 30 s reload into its pending record, so its first
	 * send after readiness carries the real extremum and says how many
	 * observations it stands for. Suppression that had lost data would show a
	 * small coalesced count here; a throttle that had kept sending would show
	 * a count no larger than one window.
	 * The number of OBSERVATIONS is the reconciliation count, not the frame
	 * count: since the 2026-09-08 far-extract crawl fix, carve_observer_frame
	 * reconciles on the census cadence (CARVE_FRAME_WALK_FRAMES) rather than
	 * on every frame -- see the contract in carve_observer.h. So the window's
	 * observation budget here is SECONDS * FPS / CARVE_FRAME_WALK_FRAMES (one
	 * per simulated second, ~30), and the guard is that the fold spans the
	 * WHOLE held window rather than one send window: anything above half the
	 * budget can only come from observations made before readiness. */
	assert(loading_seen.points[2] >= 1);
	assert(loading_seen.max_coalesced[2]
		   > SECONDS * FPS / CARVE_FRAME_WALK_FRAMES / 2);
	for(unsigned i = 0; i < SECONDS * FPS; i++)
		mem2_free(churn[i]);
	/* The census line carries two new keys; it is formatted into a fixed
	 * 2 KiB staging buffer that silently drops the line on overflow, so keep
	 * the headroom visible rather than assumed. */
	size_t census_bytes = strlen(loading_seen.last_census);
	assert(census_bytes < 2048);
	printf("PASS loading cadence: %u simulated s of the reload under "
		   "continuous churn -> %u carve_census + 2 point transitions, 0 "
		   "carve_tenant (%u sets suppressed), %.0f B/s; readiness restores "
		   "%u tenants/s and the readiness transition (census line %zu/2048 "
		   "B)\n",
		   (unsigned)SECONDS, loading_census, (unsigned)SECONDS,
		   loading_bytes_s, (unsigned)CARVE_TENANTS, census_bytes);
	world_destroy(&w);
	assert(carve_observer_stop());
}

int main(void) {
	assert(!carve_observer_active() && !mem2_carve_observe);
	carve_observer_frame(NULL, 0, 0, 0, false); /* no per-tenant/world work */
	assert(carve_observer_start());
	mem2_carve_observe(CARVE_BEGIN, 0, 0, 0, 0);
	mem2_carve_observe(CARVE_ALLOC, 4096, 0, 100, CARVE_MEM2 | CARVE_IN_HEAP);
	mem2_carve_observe(CARVE_TAG, 4096, 0, 100, CARVE_COLUMN);
	mem2_carve_observe(CARVE_OWNER, 4096, 0, 0, CARVE_A_DEPENDENCY);
	/* Repeated owner labeling cannot create a second tenant. */
	mem2_carve_observe(CARVE_OWNER, 4096, 0, 0, CARVE_A_PLAYER);
	mem2_carve_observe(CARVE_END, 0, 0, 0, 0);
	struct carve_totals synthetic;
	carve_observer_test_snapshot(&synthetic, 1024, 624, 512, 2);
	assert(!synthetic.errors && synthetic.used == 400 && synthetic.largest == 512);
	uint64_t sum = 0, count = 0;
	for(unsigned c = 0; c < CARVE_CLASSES; c++) {
		sum += synthetic.attribution.bytes[c][0];
		if(c != CARVE_A_GUARD) count += synthetic.attribution.count[c][0];
	}
	assert(sum == synthetic.used && count == 1);
	assert(synthetic.attribution.bytes[CARVE_A_PLAYER][0] == 100);
	assert(synthetic.attribution.bytes[CARVE_A_DEPENDENCY][0] == 0);
	assert(synthetic.attribution.bytes[CARVE_A_GUARD][0] == 256);
	assert(synthetic.attribution.bytes[CARVE_A_OVERHEAD][0] == 44);
	mem2_carve_observe(CARVE_BEGIN, 0, 0, 0, 0);
	mem2_carve_observe(CARVE_FREE, 4096, 0, 0, 0);
	mem2_carve_observe(CARVE_END, 0, 0, 0, 0);
	assert(carve_observer_stop());
	assert(mesh(false) == mesh(true));
	gx_boundaries();
	spill_transfers();
	ledger_growth_keeps_retirement_capture();
	attribution_cadence_capped();
	loading_phase_cadence_holds_one_record_per_second();
	census_set_paces_one_record_per_frame();
	census_offer_rate_backs_off_when_the_sender_stalls();
	assert(carve_observer_start());
	/* This section is about the EMISSION cadence (which frame a queued record
	 * actually reaches the wire on), so it wants one observation per frame --
	 * the pre-2026-09-08 reconciliation cadence. Since the far-extract crawl
	 * fix carve_observer_frame reconciles once per CARVE_FRAME_WALK_FRAMES by
	 * default (carve_observer.h), which would make the hand-picked frame
	 * numbers below silent no-ops instead of observations. Ask for the
	 * per-frame cadence explicitly rather than renumbering the scenario. */
	carve_observer_walk_interval(1);
	void* p = mem2_tag(mem2_alloc(123), 123, CARVE_COLUMN); assert(p);
	struct carve_totals t; carve_observer_snapshot(&t);
	assert(t.bytes[CARVE_COLUMN] == 123 && t.count[CARVE_COLUMN] == 1);
	assert(t.capacity == 0 && t.carve[CARVE_COLUMN] == 0 && !t.errors);
	const uint64_t observer_bytes_armed = t.observer_bytes;
	mem2_free(p);
	struct thread workers[3];
	for(unsigned i = 0; i < 3; i++) assert(thread_create(&workers[i], traffic, NULL, 0));
	for(unsigned i = 0; i < 3; i++) thread_join(&workers[i]);
	carve_observer_snapshot(&t);
	assert(!t.errors && t.bytes[CARVE_POPULATE] == 0 && t.count[CARVE_POPULATE] == 0);
	assert(t.allocs[CARVE_POPULATE] == 72000);
	/* WAS: observer_bytes < 1 MiB, "tombstone churn cannot grow forever".
	 * ROUND 3 (2026-09-08) makes that an IDENTITY rather than a bound: the
	 * ledger table is reserved once at arm time and never reallocated
	 * (carve_observer.c, LEDGER TABLE), and the tombstones it existed to
	 * compact are gone with it (backward-shift deletion). So the observer
	 * footprint after 72,000 churned allocations across three threads is
	 * byte-for-byte what it was before them -- a strictly stronger statement
	 * than the old ceiling, which the FIXED table (CARVE_LEDGER_SLOTS records:
	 * 2.25 MiB on a 64-bit PC, 1.375 MiB on the Wii, claimed at boot when the
	 * claim cannot fail) deliberately exceeds. The hardware defect this
	 * replaces is the 1.44 MB mid-run calloc that FAILED under load pressure on
	 * 703aef00 and collapsed tracked blocks 4,888 -> 745. */
	assert(t.observer_bytes == observer_bytes_armed);
	struct world w; world_create(&w);
	carve_observer_frame(&w, 1, 2, 3, false);
	struct carve_attrib a;
	assert(carve_observer_attrib(0, &a) && a.frame == 1 && a.phase == 3);
	carve_observer_frame(&w, 4, 5, 6, true);
	assert(carve_observer_attrib(1, &a) && a.frame == 4 && a.tick == 5);
	/* Actual frame snapshot must produce a queued attribution before ready.
	 * This is the unchanged a4d0a817 production ring, not a permissive sink. */
	for(unsigned i = 0; i < CW_TEL_SLOTS; i++) assert(queued("busy"));
	assert(!carve_observer_emit_next(queued));
	void* later = mem2_tag(mem2_alloc(321), 321, CARVE_COLUMN); assert(later);
	carve_observer_frame(&w, 7, 8, 9, false);
	while(cw_tel_peek(&sender)) cw_tel_pop(&sender);
	/* Point 0's transition (its very first actual send) was congestion-
	 * delayed by the full ring above, so by the time it goes out here it
	 * has already picked up the frame-7 observation that arrived while it
	 * waited -- folded, not a second record. A transition is never
	 * coalesced AWAY (it still reaches the wire), but it can honestly
	 * report what merged into it before its own send. */
	assert(carve_observer_emit_next(queued));
	assert(sender.head - sender.tail == 1);
	assert(strstr(cw_tel_peek(&sender), "\"event\":\"carve_attrib\""));
	assert(strstr(cw_tel_peek(&sender), "\"point\":0"));
	assert(strstr(cw_tel_peek(&sender), "\"frame\":1"));
	assert(strstr(cw_tel_peek(&sender), "\"coalesced\":1"));
	assert(strlen(cw_tel_peek(&sender)) + 1 < CW_TEL_LINE);
	cw_tel_pop(&sender);
	/* Point 0 just sent, closing the shared ~1 s emission-cadence window for
	 * every point: the frame-7 continuation it refreshed to must now wait,
	 * demonstrating the sender-saturation fix (2026-09-07) -- repeatedly
	 * re-sending a still-dirty pending record on every round-robin turn,
	 * unthrottled, is exactly the ~16 records/s the hardware evidence
	 * measured. Drain a full round-robin sweep: only genuine, still-unsent
	 * point transitions (never delayed) may appear; point 0's own
	 * continuation must not. */
	bool saw_point0 = false;
	for(unsigned i = 0; i < 6; i++) {
		assert(carve_observer_emit_next(queued));
		while(cw_tel_peek(&sender)) {
			const char* line = cw_tel_peek(&sender);
			assert(!strstr(line, "\"point\":0") || strstr(line, "\"frame\":1"));
			if(strstr(line, "\"point\":0")) saw_point0 = true;
			cw_tel_pop(&sender);
		}
	}
	assert(!saw_point0); /* the window truly held it back, not luck of the draw */
	/* Advance well past the 60-frame shared window; the folded frame-7
	 * value must still reach the wire once it reopens, un-swallowed, with
	 * its coalesced count reset now that nothing further has folded in. */
	carve_observer_frame(&w, 70, 71, 9, false);
	for(unsigned i = 0; i < 6 && !saw_point0; i++) {
		assert(carve_observer_emit_next(queued));
		while(cw_tel_peek(&sender)) {
			const char* line = cw_tel_peek(&sender);
			if(strstr(line, "\"point\":0")) {
				assert(strstr(line, "\"frame\":7"));
				assert(strstr(line, "\"coalesced\":0"));
				saw_point0 = true;
			}
			cw_tel_pop(&sender);
		}
	}
	assert(saw_point0);
	mem2_free(later);
	size_t prior_cap = w.ilist_walk_cap_max;
	assert(carve_observer_emit(&w, UINT32_MAX, true, packet));
	assert(w.ilist_walk_cap_max == prior_cap);
	assert(packets == CARVE_TENANTS + 2); /* one burst above, tenants + census here */
	world_destroy(&w);
	assert(carve_observer_stop());
	puts("PASS carve ledger: live bytes, labels, churn/reuse, concurrency, GL emitted-byte equivalence, bounded packets");
}
