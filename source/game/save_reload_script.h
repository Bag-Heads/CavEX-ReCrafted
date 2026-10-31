/* Copyright (c) 2026 CaveWii_v2 contributors
 * save_reload_3m (SPEC R12 real-Wii proof): the main-thread phase driver.
 *
 * Pure decision core (no engine calls): main.c fills a srs_obs each frame,
 * applies the srs_out (one-frame button press through menu_autopilot's
 * input seam, the world create / named-reload hooks, the marker emit) and
 * builds the verdict line from the server probe (network/save_reload_probe.h).
 *
 * Route: title -> create a fresh seeded world through the dialog's create
 * entry -> loaded -> server applies the fixture -> HOME opens the pause ->
 * DOWN to "Save and quit" -> CLICK -> the server saves and unloads -> title ->
 * the named autoload reopens the SAME folder through screen_select_world ->
 * loaded -> verdict. Every wait has its own budget; the total cap (170 s)
 * fires inside the 180 s profile window. */
#ifndef GAME_SAVE_RELOAD_SCRIPT_H
#define GAME_SAVE_RELOAD_SCRIPT_H

#include <stdbool.h>
#include <stdint.h>

#define SRS_TOTAL_CAP_MS 170000u
#define SRS_SETTLE_AFTER_FIXTURE_MS 8000u
/* Post-reload stability: the reopened world must stay loaded and in game
 * this long before the verdict (also gives the collector its >= 30 beats). */
#define SRS_POST_RELOAD_DWELL_MS 30000u

enum srs_phase {
	SRS_TITLE = 0,
	SRS_LOAD1,
	SRS_FIXTURE,
	SRS_PAUSE,
	SRS_FOCUS,
	SRS_CLICK,
	SRS_SAVING,
	SRS_RELOAD,
	SRS_DWELL,
	SRS_DONE,
	SRS_FAIL,
};

enum srs_button {
	SRS_BTN_NONE = 0,
	SRS_BTN_HOME,
	SRS_BTN_DOWN,
	SRS_BTN_CLICK,
};

struct srs_obs {
	uint32_t ms;	  /* since arm */
	bool title_ready; /* title screen current and the boot gate passed */
	bool world_loaded;
	bool ingame;	 /* screen_ingame current */
	bool pause;		 /* screen_pause current */
	bool focus_exit; /* the pause's focused row is Save and quit */
	bool exit_pending;
	bool stats_modal;
	uint32_t probe_applied, probe_loads, probe_saves, probe_after_loads;
};

struct srs_out {
	enum srs_button press; /* held for exactly this frame */
	bool create_world;	   /* once */
	bool arm_reload;	   /* once */
	const char* marker;	   /* phase marker to emit this frame, or NULL */
};

struct srs_state {
	enum srs_phase phase;
	uint32_t phase_ms, last_press_ms, fixture_ms;
	unsigned presses, loaded_edges;
	bool prev_loaded, created, reload_armed;
	const char* reason; /* failure reason (static string) */
};

void srs_init(struct srs_state* st);
void srs_tick(struct srs_state* st, const struct srs_obs* o,
			  struct srs_out* out);
const char* srs_phase_name(enum srs_phase p);

/* ---- device wiring (save_reload_script.c, main thread) ---------------- */
struct save_reload_hooks {
	bool (*emit)(const char* line);
	bool (*create_world)(void);
	bool (*arm_reload)(const char* folder);
};
void save_reload_arm(const struct save_reload_hooks* hooks,
					 const char* run_id);
bool save_reload_armed(void);
/* Once per frame, before the screens read input. */
void save_reload_frame(const struct srs_obs* obs, const char* world_folder);
/* menu_autopilot_force_button's first question: the driver's press. */
bool save_reload_force_button(int button, bool* out);
/* NULL until the run is over; then "ok" or the failure reason. */
const char* save_reload_status(void);

#endif
