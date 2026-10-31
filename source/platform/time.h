/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

#ifndef TIME_H
#define TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef PLATFORM_WII
typedef uint64_t ptime_t;
#endif

#ifdef PLATFORM_PC
#include <time.h>
typedef struct timespec ptime_t;
#endif

void time_reset(void);
ptime_t time_get(void);
ptime_t time_add_ms(ptime_t t, unsigned int ms);
int32_t time_diff_ms(ptime_t f, ptime_t s);
float time_diff_s(ptime_t f, ptime_t s);
uint64_t time_diff_us(ptime_t f, ptime_t s);
// True once `now` has reached or passed `deadline`. THE deadline primitive:
// on Wii ptime_t is unsigned timebase ticks, so `time_diff_ms(now, deadline)`
// after expiry computes (deadline - now) wrapped to a huge positive u64 --
// a toast checked that way NEVER expired (2026-07-30 hardware find). This
// reinterprets the wrapped difference as signed instead of subtracting.
bool time_reached(ptime_t now, ptime_t deadline);

#endif
