/* Copyright (c) 2026 CaveWii_v2 contributors
 * save_reload_3m (SPEC R12): the phase driver's decision core.
 *
 * Drives srs_tick through the whole route with synthetic observations and
 * pins: the marker order the collector requires
 * (tools/save_reload_evidence.py MARKERS), one-frame presses with neutral
 * gaps, the create and reload hooks fire exactly once, the world must load
 * exactly twice, and every wait fails with its own named reason. Also pins
 * the device's hex formatting helper (FNV) against a known vector. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/save_reload_script.h"
#include "network/save_reload_probe.h"

static const char* const MARKERS[] = {
	"create_world", "world_loaded_1", "fixture_applied", "pause_open",
	"exit_focused", "save_quit_clicked", "saved_at_title", "reload_armed",
	"world_loaded_2", "dwell_done"};
#define NMARK (sizeof MARKERS / sizeof *MARKERS)

struct sim {
	struct srs_state st;
	struct srs_obs o;
	unsigned nmark, creates, reloads, homes, downs, clicks;
	const char* marks[32];
	int focus_row; /* pause rows: 0 Back .. 3 Save and quit */
	bool prev_press;
	unsigned adjacent_presses;
};

static void step(struct sim* s, uint32_t dt) {
	struct srs_out out;
	s->o.ms += dt;
	srs_tick(&s->st, &s->o, &out);
	if(out.marker && s->nmark < 32)
		s->marks[s->nmark++] = out.marker;
	s->creates += out.create_world;
	s->reloads += out.arm_reload;
	bool pressed = out.press != SRS_BTN_NONE;
	if(pressed && s->prev_press)
		s->adjacent_presses++;
	s->prev_press = pressed;
	/* The world reacts to the press on the next frame. */
	switch(out.press) {
		case SRS_BTN_HOME:
			s->homes++;
			if(s->o.ingame) {
				s->o.ingame = false;
				s->o.pause = true;
				s->focus_row = 0;
			}
			break;
		case SRS_BTN_DOWN:
			s->downs++;
			s->focus_row = (s->focus_row + 1) % 4;
			break;
		case SRS_BTN_CLICK:
			s->clicks++;
			if(s->o.pause && s->focus_row == 3)
				s->o.exit_pending = true;
			break;
		default: break;
	}
	s->o.focus_exit = s->o.pause && s->focus_row == 3;
}

static void run_until(struct sim* s, enum srs_phase p, unsigned max_frames) {
	for(unsigned k = 0; k < max_frames && s->st.phase != p
		&& s->st.phase != SRS_FAIL; k++)
		step(s, 16);
}

static void happy(void) {
	struct sim s;
	memset(&s, 0, sizeof s);
	srs_init(&s.st);
	/* Boot: cards up, nothing happens. */
	for(int k = 0; k < 30; k++)
		step(&s, 16);
	assert(s.st.phase == SRS_TITLE && s.creates == 0);
	s.o.title_ready = true;
	step(&s, 16);
	assert(s.creates == 1 && s.st.phase == SRS_LOAD1);
	s.o.title_ready = false;
	for(int k = 0; k < 50; k++)
		step(&s, 16);
	s.o.world_loaded = true;
	s.o.ingame = true;
	s.o.probe_loads = 1;
	step(&s, 16);
	assert(s.st.phase == SRS_FIXTURE);
	for(int k = 0; k < 100; k++)
		step(&s, 16);
	assert(s.st.phase == SRS_FIXTURE && s.homes == 0);
	s.o.probe_applied = 1;
	step(&s, 16);
	/* The settle window holds the pause back. */
	run_until(&s, SRS_PAUSE, (SRS_SETTLE_AFTER_FIXTURE_MS - 1000) / 16);
	assert(s.st.phase == SRS_FIXTURE);
	run_until(&s, SRS_SAVING, 4000);
	assert(s.st.phase == SRS_SAVING);
	assert(s.homes == 1 && s.downs == 3 && s.clicks == 1);
	assert(s.adjacent_presses == 0);
	/* The server saves, the world goes, the title comes back. */
	s.o.pause = false;
	s.o.exit_pending = false;
	s.o.world_loaded = false;
	for(int k = 0; k < 20; k++)
		step(&s, 16);
	assert(s.st.phase == SRS_SAVING); /* no save published yet */
	s.o.probe_saves = 1;
	s.o.title_ready = true;
	step(&s, 16);
	assert(s.st.phase == SRS_RELOAD);
	step(&s, 16);
	assert(s.reloads == 1);
	s.o.title_ready = false;
	for(int k = 0; k < 50; k++)
		step(&s, 16);
	assert(s.reloads == 1 && s.st.phase == SRS_RELOAD);
	s.o.world_loaded = true;
	s.o.ingame = true;
	s.o.probe_loads = 2;
	step(&s, 16);
	assert(s.st.phase == SRS_RELOAD); /* the AFTER snapshot is not in yet */
	s.o.probe_after_loads = 1;
	step(&s, 16);
	assert(s.st.phase == SRS_DWELL);
	uint32_t dwell_start = s.o.ms;
	run_until(&s, SRS_DONE, (SRS_POST_RELOAD_DWELL_MS - 1000) / 16);
	assert(s.st.phase == SRS_DWELL); /* not before 30 s */
	run_until(&s, SRS_DONE, 200);
	assert(s.st.phase == SRS_DONE
		   && s.o.ms - dwell_start >= SRS_POST_RELOAD_DWELL_MS);
	assert(s.st.loaded_edges == 2 && s.creates == 1 && s.reloads == 1);
	assert(s.nmark == NMARK);
	for(unsigned k = 0; k < NMARK; k++)
		assert(strcmp(s.marks[k], MARKERS[k]) == 0);
	assert(s.o.ms < SRS_TOTAL_CAP_MS);
	printf("  happy: %u markers in order, 1 HOME / %u DOWN / %u CLICK, "
		   "2 loads, done at %u ms\n", s.nmark, s.downs, s.clicks,
		   (unsigned)s.o.ms);
}

static const char* fail_case(int which) {
	struct sim s;
	memset(&s, 0, sizeof s);
	srs_init(&s.st);
	if(which == 0) { /* never a title */
		run_until(&s, SRS_LOAD1, 10000);
		return s.st.reason;
	}
	s.o.title_ready = true;
	step(&s, 16);
	s.o.title_ready = false;
	if(which == 1) { /* never loads */
		run_until(&s, SRS_FIXTURE, 10000);
		return s.st.reason;
	}
	s.o.world_loaded = s.o.ingame = true;
	step(&s, 16);
	if(which == 2) { /* fixture never applied */
		run_until(&s, SRS_PAUSE, 10000);
		return s.st.reason;
	}
	s.o.probe_applied = 1;
	if(which == 3) { /* HOME does nothing: the pause never opens */
		for(unsigned k = 0; k < 10000 && s.st.phase != SRS_FAIL; k++) {
			step(&s, 16);
			s.o.pause = false;
			s.o.ingame = true;
		}
		return s.st.reason;
	}
	if(which == 4) { /* the exit row is never focused */
		for(unsigned k = 0; k < 10000 && s.st.phase != SRS_FAIL; k++) {
			step(&s, 16);
			s.o.focus_exit = false;
		}
		return s.st.reason;
	}
	run_until(&s, SRS_SAVING, 10000);
	if(which == 5) { /* the save is never published */
		s.o.pause = false;
		s.o.world_loaded = false;
		s.o.title_ready = true;
		run_until(&s, SRS_RELOAD, 10000);
		return s.st.reason;
	}
	s.o.pause = false;
	s.o.world_loaded = false;
	s.o.title_ready = true;
	s.o.probe_saves = 1;
	run_until(&s, SRS_RELOAD, 10);
	/* which == 6: the reload never completes */
	run_until(&s, SRS_DONE, 20000);
	return s.st.reason;
}

static void failures(void) {
	static const char* const want[] = {
		"title_never_reached", "world_never_loaded", "fixture_never_applied",
		"pause_never_opened", "exit_row_not_focused", "save_never_published",
		"reload_never_completed"};
	for(int k = 0; k < 7; k++) {
		const char* got = fail_case(k);
		if(!got || strcmp(got, want[k]) != 0) {
			printf("FAIL case %d: got %s want %s\n", k, got ? got : "(none)",
				   want[k]);
			assert(0);
		}
	}
	/* A world that loads a second time before the save is refused. */
	struct sim s;
	memset(&s, 0, sizeof s);
	srs_init(&s.st);
	s.o.title_ready = true;
	step(&s, 16);
	s.o.title_ready = false;
	s.o.world_loaded = true;
	step(&s, 16); /* loaded but not ingame yet */
	s.o.world_loaded = false;
	step(&s, 16);
	s.o.world_loaded = true;
	step(&s, 16);
	assert(s.st.phase == SRS_FAIL
		   && strcmp(s.st.reason, "unexpected_second_load") == 0);
	/* The total cap fires inside the 180 s profile window. */
	memset(&s, 0, sizeof s);
	srs_init(&s.st);
	s.o.title_ready = true;
	step(&s, 16);
	s.o.ms = SRS_TOTAL_CAP_MS;
	step(&s, 0);
	assert(s.st.phase == SRS_FAIL && strcmp(s.st.reason, "total_cap") == 0);
	assert(SRS_TOTAL_CAP_MS < 180000u);
	/* Losing the reopened world inside the dwell fails it. */
	{
		struct sim d;
		memset(&d, 0, sizeof d);
		srs_init(&d.st);
		d.st.phase = SRS_DWELL;
		d.st.loaded_edges = 2;
		d.st.prev_loaded = true;
		d.o.world_loaded = d.o.ingame = true;
		step(&d, 16);
		d.o.world_loaded = false;
		step(&d, 16);
		assert(d.st.phase == SRS_FAIL
			   && strcmp(d.st.reason, "world_lost_after_reload") == 0);
	}
	printf("  failures: 10 named reasons\n");
}

int main(void) {
	/* FNV-1a-64 known vector ("a"), the hash every snapshot field uses. */
	assert(save_reload_fnv(SAVE_RELOAD_FNV0, "a", 1)
		   == UINT64_C(0xaf63dc4c8601ec8c));
	happy();
	failures();
	/* Disarmed: the input seam answers false for every button. */
	bool v = false;
	for(int b = 0; b < 64; b++)
		assert(!save_reload_force_button(b, &v));
	assert(!save_reload_armed() && save_reload_status() == NULL);
	puts("test_save_reload_script: PASS");
	return 0;
}
