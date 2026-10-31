#ifndef G9_CAPTURE_H
#define G9_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

/* R21 observer: three worst random-pass ticks, three worst drain ticks,
 * the largest-pop tick and the worst checkpoint tick.
 * A tick belonging to several lists is stored once. Nine complete reservations
 * (eight retained + one candidate), 4096 x 20 words each = 2,949,120 bytes.
 * No allocation, clock, lock, timer or formatting on the default-OFF path.
 * Exhaustion seals an explicitly incomplete prefix and closes admission;
 * it never overwrites queued records or claims a complete capture. */
#define G9_CAPTURE_WORDS 20u
#define G9_CAPTURE_ROWS 4096u
#define G9_CAPTURE_SLOTS 9u
bool g9_capture_configure(bool enabled);
/* PC recorder tests only; NULL restores the native clock. Configure quiescent. */
void g9_capture_test_clock(uint64_t (*read_tb)(void));
bool g9_capture_enabled(void);
void g9_capture_tick_begin(void);
void g9_capture_tick_components(uint32_t rt_us, uint32_t wt_us);
void g9_capture_tick_end(void);
void g9_capture_random(bool begin, uint32_t world, uint32_t generation,
	uint32_t world_tick);
void g9_capture_drain_begin(uint32_t world, uint32_t generation,
	uint32_t world_tick, uint32_t stage, uint32_t seeds);
void g9_capture_pop(void);
void g9_capture_record_enter(void);
void g9_capture_drain_final(uint32_t pops, uint32_t result);
void g9_capture_restore_return(void);
void g9_capture_record_end(uint32_t old_us);
void g9_capture_record_return(void);
void g9_capture_stage(bool begin, uint32_t world, uint32_t stage);
void g9_capture_arm(void);
void g9_capture_main(unsigned region, bool begin, uint32_t work);
void g9_capture_audio(uint32_t reserve_us, bool worker, uint32_t fx_state);
void g9_capture_sample(uint64_t tb, const uint32_t observation[10]);
void g9_capture_finish(void);
bool g9_capture_pending(void);
bool g9_capture_pump(bool (*emit)(const char*));
#endif
