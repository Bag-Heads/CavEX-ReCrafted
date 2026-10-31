/*
	CaveWii_v2 CAVEWII_AGENT bridge -- ACT-half (synthetic input driver, see header).

	PC dev rig ONLY (PLATFORM_PC) and ARMED by env (CAVEWII_AGENT=1): a normal build/run
	spawns no thread, reads no stdin, and every hook returns "not owned" so native input
	is byte-identical. Decoupled from state_export_emit on purpose (its own armed check)
	so it links into a standalone unit test with no gstate/engine glue.
*/

#ifdef PLATFORM_PC

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_input.h"
#include "gui/chat_composer_model.h"

#define AGENT_DEG2RAD (3.14159265358979323846f / 180.0f)

// ---- shared state (written by the stdin reader thread, read by the frame latch) ----
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_level[IB_COUNT];	// desired hold level for HOLD buttons
static int s_pulse[IB_COUNT];	// pending one-shot presses for PULSE buttons
static float s_yaw_deg, s_pitch_deg;
static bool s_yaw_set, s_pitch_set; // an axis is authored only once commanded

// ---- frame latch (main-thread only: advanced in agent_input_poll, read in the hooks) ----
// Plain bools, lock-free, matching the autopilot precedent (gstate.autopilot.* is read
// cross-thread by the player movement tick without a lock). prev/frame give clean edges.
static bool s_frame[IB_COUNT];
static bool s_prev[IB_COUNT];

static bool s_thread_started = false;
static pthread_t s_thread;

// -1 = unchecked, 0 = off, 1 = armed. Cached so the env is read once. Mirrors
// state_export_agent_armed()'s gate (same env var) without coupling to it.
static int s_armed = -1;

bool agent_input_armed(void) {
	if(s_armed < 0) {
		const char* v = getenv("CAVEWII_AGENT");
		s_armed = (v && v[0] == '1' && v[1] == '\0') ? 1 : 0;
	}
	return s_armed == 1;
}

// PULSE buttons emit a single rising edge per command; everything else is a held level.
// PLACE/USE (IB_ACTION2), hotbar scroll and the inventory toggle are press-once actions
// (the engine gates them on input_pressed), so a hold would never re-fire them.
static bool button_is_pulse(enum input_button b) {
	return b == IB_ACTION2 || b == IB_INVENTORY || b == IB_OPEN_INVENTORY
		|| b == IB_SCROLL_LEFT
		|| b == IB_SCROLL_RIGHT || b == IB_DROP
		/* 2026-09-17 menu driving: one-shot presses. The four GUI directions
		 * are deliberately LEVEL buttons (GUI_UP=1 ... GUI_UP=0) so a held
		 * direction can exercise key-repeat. */
		|| b == IB_CHAT || b == IB_HOME || b == IB_GUI_CLICK
		|| b == IB_GUI_CLICK_ALT || b == IB_GUI_QUICK_MOVE || b == IB_GUI_PAGE
		|| b == IB_GUI_PREVIOUS;
}

/* Menu buttons are owned ONLY after a command has named them once ("claimed"):
 * an armed rig that never sends a menu command keeps native menu input exactly
 * as before (the 2026-09-11 PC key-driver rigs press real keys for menus while
 * the bridge drives gameplay). Written under s_lock, read lock-free like s_frame. */
static bool s_claimed[IB_COUNT];

bool agent_input_menu_claimed(void) {
	if(!agent_input_armed())
		return false;
	/* Read lock-free like s_frame: a stale false only delays the controller
	 * mode by a frame, and the composer samples it once when it opens. */
	for(int b = 0; b < IB_COUNT; b++)
		if(s_claimed[b])
			return true;
	return false;
}

static bool button_is_menu(enum input_button b) {
	return b == IB_CHAT || b == IB_HOME || b == IB_GUI_UP || b == IB_GUI_DOWN
		|| b == IB_GUI_LEFT || b == IB_GUI_RIGHT || b == IB_GUI_CLICK
		|| b == IB_GUI_CLICK_ALT || b == IB_GUI_QUICK_MOVE || b == IB_GUI_PAGE
		|| b == IB_GUI_PREVIOUS;
}

// The buttons the agent drives. Non-owned buttons (GUI nav, screenshot, pause, debug
// overlay) fall through to native input so the hooks never hijack them.
static bool button_is_owned(enum input_button b) {
	switch(b) {
		case IB_FORWARD:
		case IB_BACKWARD:
		case IB_LEFT:
		case IB_RIGHT:
		case IB_JUMP:
		case IB_SNEAK:
		case IB_ACTION1:
		case IB_ACTION2:
		case IB_INVENTORY:
		case IB_OPEN_INVENTORY:
		case IB_SCROLL_LEFT:
		case IB_SCROLL_RIGHT:
		case IB_DROP: return true;
		default: return button_is_menu(b) && s_claimed[b];
	}
}

// Map a command name to an owned button, or -1. MINE/ATTACK/DIG all mean "hold left
// click" (IB_ACTION1); PLACE/USE mean "tap right click" (IB_ACTION2).
static int name_to_button(const char* n) {
	static const struct {
		const char* name;
		enum input_button b;
	} map[] = {
		{"FORWARD", IB_FORWARD},		 {"BACK", IB_BACKWARD},
		{"BACKWARD", IB_BACKWARD},		 {"LEFT", IB_LEFT},
		{"RIGHT", IB_RIGHT},			 {"JUMP", IB_JUMP},
		{"SNEAK", IB_SNEAK},			 {"MINE", IB_ACTION1},
		{"ATTACK", IB_ACTION1},			 {"DIG", IB_ACTION1},
		{"PLACE", IB_ACTION2},			 {"USE", IB_ACTION2},
		/* brief 07 C: INVENTORY keeps meaning the CLOSE / Back role it has
		 * always driven in menus; OPEN_INVENTORY is the gameplay open that
		 * was split out of it. */
		{"INVENTORY", IB_INVENTORY},	 {"INV", IB_INVENTORY},
		{"OPEN_INVENTORY", IB_OPEN_INVENTORY},
		{"OPEN_INV", IB_OPEN_INVENTORY},
		{"SCROLL_LEFT", IB_SCROLL_LEFT}, {"SLOT_LEFT", IB_SCROLL_LEFT},
		{"SCROLL_RIGHT", IB_SCROLL_RIGHT}, {"SLOT_RIGHT", IB_SCROLL_RIGHT},
		{"DROP", IB_DROP},
		/* menus (claimed on first use, see s_claimed) */
		{"CHAT", IB_CHAT},				 {"HOME", IB_HOME},
		{"GUI_UP", IB_GUI_UP},			 {"GUI_DOWN", IB_GUI_DOWN},
		{"GUI_LEFT", IB_GUI_LEFT},		 {"GUI_RIGHT", IB_GUI_RIGHT},
		{"GUI_CLICK", IB_GUI_CLICK},	 {"GUI_BACK", IB_GUI_CLICK_ALT},
		{"GUI_CLICK_ALT", IB_GUI_CLICK_ALT}, {"GUI_SEND", IB_GUI_QUICK_MOVE},
		{"GUI_QUICK_MOVE", IB_GUI_QUICK_MOVE}, {"GUI_PAGE", IB_GUI_PAGE},
		{"GUI_PREVIOUS", IB_GUI_PREVIOUS},
	};
	for(size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if(strcmp(n, map[i].name) == 0)
			return (int)map[i].b;
	}
	return -1;
}

static void apply_command(const char* name, const char* value) {
	if(strcmp(name, "YAW") == 0) {
		float v = value ? strtof(value, NULL) : 0.0f;
		pthread_mutex_lock(&s_lock);
		s_yaw_deg = v;
		s_yaw_set = true;
		pthread_mutex_unlock(&s_lock);
		return;
	}
	if(strcmp(name, "PITCH") == 0) {
		float v = value ? strtof(value, NULL) : 0.0f;
		pthread_mutex_lock(&s_lock);
		s_pitch_deg = v;
		s_pitch_set = true;
		pthread_mutex_unlock(&s_lock);
		return;
	}
	if(strcmp(name, "RESET") == 0) {
		pthread_mutex_lock(&s_lock);
		for(int b = 0; b < IB_COUNT; b++) {
			s_level[b] = false;
			s_pulse[b] = 0;
			s_claimed[b] = false; /* menus go back to native input */
		}
		pthread_mutex_unlock(&s_lock);
		chat_controller_mode_set(false); /* ... and the composer stops asking */
		return;
	}

	int b = name_to_button(name);
	if(b < 0) {
		fprintf(stderr, "[agent_input] ignoring unknown command '%s'\n", name);
		return;
	}

	// Bare NAME (no '=') means 1; otherwise atoi the value (0 = off / no-op).
	int v = value ? atoi(value) : 1;

	pthread_mutex_lock(&s_lock);
	if(button_is_menu((enum input_button)b)) {
		s_claimed[b] = true;
		/* The chat composer lives in the core pool and cannot call into this
		 * app-only file, so the dependency is inverted: tell the core the
		 * menus are being driven (chat_composer_model.h). */
		chat_controller_mode_set(true);
	}
	if(button_is_pulse((enum input_button)b)) {
		if(v != 0)
			s_pulse[b]++; // enqueue exactly one press; a 0 is a no-op for pulses
	} else {
		s_level[b] = (v != 0);
	}
	pthread_mutex_unlock(&s_lock);
}

void agent_input_feed_line(const char* line) {
	if(!line)
		return;

	// Copy: strtok mutates, and the input may be a literal. Truncate over-long lines.
	char buf[256];
	strncpy(buf, line, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	// Strip a trailing CR/LF if a caller left one in.
	char* nl = strpbrk(buf, "\r\n");
	if(nl)
		*nl = '\0';

	// Whitespace-separated NAME=VALUE tokens. Plain strtok is fine: only the single
	// reader thread (or a single-threaded test) ever tokenizes.
	for(char* tok = strtok(buf, " \t"); tok; tok = strtok(NULL, " \t")) {
		char* eq = strchr(tok, '=');
		char* val = NULL;
		if(eq) {
			*eq = '\0';
			val = eq + 1;
		}
		for(char* c = tok; *c; c++)
			*c = (char)toupper((unsigned char)*c);
		apply_command(tok, val);
	}
}

static void* reader_main(void* arg) {
	(void)arg;
	char line[256];
	while(fgets(line, sizeof(line), stdin))
		agent_input_feed_line(line);
	return NULL; // stdin closed (EOF) -> reader exits; process keeps running
}

void agent_input_init(void) {
	if(!agent_input_armed() || s_thread_started)
		return;

	if(pthread_create(&s_thread, NULL, reader_main, NULL) == 0) {
		s_thread_started = true;
		pthread_detach(s_thread); // never joined; dies with the process
		fprintf(stderr,
				"[agent_input] ACT bridge armed: driving the player from stdin "
				"(NAME=VALUE, e.g. FORWARD=1 / MINE=1 / PLACE=1 / YAW=90)\n");
	} else {
		fprintf(stderr, "[agent_input] failed to start stdin reader thread\n");
	}
}

void agent_input_poll(void) {
	pthread_mutex_lock(&s_lock);
	for(int b = 0; b < IB_COUNT; b++) {
		s_prev[b] = s_frame[b];
		if(button_is_pulse((enum input_button)b)) {
			if(s_frame[b]) {
				// Down last frame -> force up this frame so the NEXT queued pulse
				// re-edges (two PLACE commands => two distinct presses, not one hold).
				s_frame[b] = false;
			} else if(s_pulse[b] > 0) {
				s_frame[b] = true;
				s_pulse[b]--;
			} else {
				s_frame[b] = false;
			}
		} else {
			s_frame[b] = s_level[b];
		}
	}
	pthread_mutex_unlock(&s_lock);
}

bool agent_input_held(enum input_button b, bool* out) {
	if(!agent_input_armed() || !button_is_owned(b))
		return false;
	*out = s_frame[b];
	return true;
}

bool agent_input_pressed(enum input_button b, bool* out) {
	if(!agent_input_armed() || !button_is_owned(b))
		return false;
	*out = s_frame[b] && !s_prev[b]; // rising edge, stable for the whole frame
	return true;
}

bool agent_input_released(enum input_button b, bool* out) {
	if(!agent_input_armed() || !button_is_owned(b))
		return false;
	*out = !s_frame[b] && s_prev[b]; // falling edge
	return true;
}

bool agent_input_apply_look(float* rx, float* ry) {
	if(!agent_input_armed())
		return false;
	bool any = false;
	pthread_mutex_lock(&s_lock);
	if(s_yaw_set) {
		*rx = s_yaw_deg * AGENT_DEG2RAD;
		any = true;
	}
	if(s_pitch_set) {
		*ry = s_pitch_deg * AGENT_DEG2RAD;
		any = true;
	}
	pthread_mutex_unlock(&s_lock);
	return any;
}

#endif // PLATFORM_PC
