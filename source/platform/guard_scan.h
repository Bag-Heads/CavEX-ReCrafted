#ifndef CAVEWII_GUARD_SCAN_H
#define CAVEWII_GUARD_SCAN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Return the first differing byte plus one, or zero. Every byte is checked;
 * memcpy permits arbitrary alignment and avoids uint32_t aliasing UB. */
static inline size_t guard_scan_a5(const void* data, size_t length) {
	const uint8_t* bytes = data;
	size_t i = 0;
	for(; length - i >= sizeof(uint32_t); i += sizeof(uint32_t)) {
		uint32_t word;
		memcpy(&word, bytes + i, sizeof(word));
		if(word != UINT32_C(0xA5A5A5A5)) {
			const uint8_t* lane = (const uint8_t*)&word;
			for(size_t j = 0; j < sizeof(word); j++)
				if(lane[j] != 0xA5) return i + j + 1;
		}
	}
	for(; i < length; i++)
		if(bytes[i] != 0xA5) return i + 1;
	return 0;
}

#ifdef PLATFORM_WII
/* Once at profile startup, before the watchdog starts. Times both scans of
 * the real uncached XFB guards; retains average microseconds, not a PC proxy. */
void gfx_guard_scan_benchmark(uint32_t out_us[2]);
#endif
#endif
