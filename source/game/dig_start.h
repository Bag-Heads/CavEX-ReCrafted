/* R1d (next-gaps-0924, Fable R1 review): the client's per-frame dig-start
 * decision while the dig button is held, header-only and pure so a test pins
 * it. Beta PlayerControllerMP.clickBlock sends Packet14 status 0 only when
 * `!isHittingBlock || cell differs`, and Minecraft.sendClickBlockToController
 * calls resetBlockRemoving (NO packet) when the crosshair is off every block.
 * [740c583: PlayerControllerMP.java:41-60, resetBlockRemoving:63-66] */
#ifndef DIG_START_H
#define DIG_START_H

#include <stdbool.h>
#include <stdint.h>

enum dig_start_action {
	DIG_START_KEEP,	 // same cell, still hitting: accumulate, send nothing
	DIG_START_SEND,	 // a real new cell: one dig-start packet
	DIG_START_RESET, // crosshair left every block: stop digging, send nothing
};

static inline enum dig_start_action dig_start_decide(bool hit, bool same_cell) {
	if(!hit)
		return DIG_START_RESET;
	return same_cell ? DIG_START_KEEP : DIG_START_SEND;
}

// The next process-monotonic dig-start id (block_dig.start_seq); skips 0,
// which means "unsequenced" to the server.
static inline uint32_t dig_start_next_seq(uint32_t* counter) {
	if(++*counter == 0)
		++*counter;
	return *counter;
}

#endif
