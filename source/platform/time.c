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

#include "time.h"

#ifdef PLATFORM_PC

#include <time.h>

// CLOCK_MONOTONIC_RAW is a Linux extension (a monotonic clock not slewed by
// NTP). It is absent on some libcs (e.g. MSYS2/ucrt64 used by the Windows PC
// build). CLOCK_MONOTONIC is POSIX-standard and equivalent for a frame/tick
// timer — the NTP slewing is irrelevant over per-frame intervals — so fall
// back to it where the raw variant is unavailable.
#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

void time_reset() { }

ptime_t time_get() {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC_RAW, &t);
	return t;
}

ptime_t time_add_ms(ptime_t t, unsigned int ms) {
	uint64_t ns = t.tv_nsec + ms * 1000000LL;
	t.tv_sec += ns / 1000000000LL;
	t.tv_nsec = ns % 1000000000LL;
	return t;
}

int32_t time_diff_ms(ptime_t f, ptime_t s) {
	// Combine before dividing. Splitting the terms rounds a negative nsec
	// remainder toward zero across a second boundary (e.g. 49.5 ms -> 50),
	// which can make the work-inclusive server pacing skip its final millisecond.
	int64_t ns = ((int64_t)s.tv_sec - (int64_t)f.tv_sec) * INT64_C(1000000000)
		+ (int64_t)s.tv_nsec - (int64_t)f.tv_nsec;
	return (int32_t)(ns / INT64_C(1000000));
}

float time_diff_s(ptime_t f, ptime_t s) {
	return (float)(s.tv_sec - f.tv_sec)
		+ (float)(s.tv_nsec - f.tv_nsec) / 1000000000.0F;
}

uint64_t time_diff_us(ptime_t f, ptime_t s) {
	return ((int64_t)(s.tv_sec - f.tv_sec) * INT64_C(1000000000)
		+ s.tv_nsec - f.tv_nsec) / 1000;
}

bool time_reached(ptime_t now, ptime_t deadline) {
	return now.tv_sec > deadline.tv_sec
		|| (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec);
}

#endif

#ifdef PLATFORM_WII

#include <ogc/lwp_watchdog.h>

void time_reset() {
	settime(0);
}

ptime_t time_get() {
	return gettime();
}

ptime_t time_add_ms(ptime_t t, unsigned int ms) {
	return t + TB_TIMER_CLOCK * ms;
}

int32_t time_diff_ms(ptime_t f, ptime_t s) {
	return (s - f) / TB_TIMER_CLOCK;
}

float time_diff_s(ptime_t f, ptime_t s) {
	return (float)(s - f) / (float)(1000UL * TB_TIMER_CLOCK);
}

uint64_t time_diff_us(ptime_t f, ptime_t s) {
	return ticks_to_microsecs(s - f);
}

bool time_reached(ptime_t now, ptime_t deadline) {
	// Unsigned tick counters: (now - deadline) wraps negative-as-huge before
	// the deadline; reinterpreting the wrapped difference as signed is the
	// overflow-safe "has it passed" test (deltas are always << 2^63 ticks).
	return (int64_t)(now - deadline) >= 0;
}

#endif
