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

#ifndef GUI_SPAWN_SEARCH_WAIT_H
#define GUI_SPAWN_SEARCH_WAIT_H

#include <stdbool.h>
#include <stdint.h>

/* Main-thread idle for server_local_create_spawns: watchdog heartbeat + a
 * presented "Finding a spawn point..." frame (<= every 66 ms). See the .c. */
void spawn_search_wait_idle(void* user);

/* hw1030n: main.c's telemetry heartbeat service (offered every 250 ms while
 * the search runs; main keeps its own 1 s cadence) and the chunks-probed
 * reader for the on-screen count. Either may be NULL (rigs, tools). */
void spawn_search_wait_set_hooks(void (*heartbeat)(void),
								 bool (*progress)(uint32_t* probes));

#endif
