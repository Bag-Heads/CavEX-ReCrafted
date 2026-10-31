/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

/* R3: the main thread's idle while the server thread runs the new-world
 * spawn search (server_local_create_spawns): pet the watchdog and present a
 * frame saying what is happening, at most every 66 ms like the boot gate's
 * loader service (main.c BOOT_GATE_PRESENT_MS). Headless just yields. Lives
 * in its own translation unit because the front-end screen rigs link
 * screen_select_world.c without the watchdog, loading_notice or gfx; they
 * see this symbol as a NULL weak reference and the create flow then sleeps
 * between polls. */

#include "spawn_search_wait.h"

#include <stdio.h>

#include "../../graphics/gui_util.h"
#include "../../platform/gfx.h"
#include "../../platform/thread.h"
#include "../../platform/time.h"
#include "../../platform/watchdog.h"
#include "loading_notice.h"

/* hw1030n: a long Beta walk (seed 22: 39-218+ chunks) sat here with no
 * telemetry heartbeat, so the collector called a busy search a 5 s hang.
 * main.c registers its normal heartbeat (and the probe count) here; the
 * idle offers it every 250 ms and main applies its usual 1 s cadence to its
 * own heartbeat clock. Unregistered = the old path. */
static void (*s_heartbeat)(void);
static bool (*s_progress)(uint32_t* probes);

void spawn_search_wait_set_hooks(void (*heartbeat)(void),
								 bool (*progress)(uint32_t* probes)) {
	s_heartbeat = heartbeat;
	s_progress = progress;
}

static void spawn_search_wait_paint(void* user) {
	(void)user;
	char line[64];
	uint32_t probes = 0;
	if(s_progress && s_progress(&probes) && probes > 0)
		snprintf(line, sizeof line, "Finding a spawn point... (%u chunks)",
				 (unsigned)probes);
	else
		snprintf(line, sizeof line, "Finding a spawn point...");
	const int font = 16;
	const int tw = gutil_font_width(line, font);
	gutil_text((gfx_width() - tw) / 2, gfx_height() / 2 - font / 2, line, font,
			   true);
}

void spawn_search_wait_idle(void* user) {
	(void)user;
	static ptime_t last, last_hb;
	static bool have_last, have_hb;
	watchdog_heartbeat();
	const ptime_t now = time_get();
	if(s_heartbeat && (!have_hb || time_diff_ms(last_hb, now) >= 250)) {
		last_hb = now;
		have_hb = true;
		s_heartbeat();
	}
	if(!have_last || time_diff_ms(last, now) >= 66) {
		last = now;
		have_last = true;
		if(loading_notice_present_next(spawn_search_wait_paint, NULL) < 0)
			thread_msleep(1); /* headless */
	} else
		thread_msleep(1);
}
