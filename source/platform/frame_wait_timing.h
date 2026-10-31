/* Pure elapsed-time accounting for the Wii framebuffer/sleep diagnostics. */
#ifndef FRAME_WAIT_TIMING_H
#define FRAME_WAIT_TIMING_H
#include <stdint.h>
/* A buffer released before we started waiting was already available: its
 * age is not scheduling delay. All timestamps are monotonic microseconds. */
static inline uint32_t frame_wait_resume_delay_us(uint64_t wait_started,
    uint64_t released, uint64_t resumed) {
    if(released <= wait_started || resumed < released)
        return 0;
    uint64_t delay = resumed - released;
    return delay > UINT32_MAX ? UINT32_MAX : (uint32_t)delay;
}
#endif
