#ifndef SERVER_WORK_PROFILE_H
#define SERVER_WORK_PROFILE_H
#include <stdbool.h>
#include <stdint.h>
/* Hardware profiler: exclusive server-update WALL time, including preemption
 * and blocking within each phase. Unsigned microsecond totals wrap modulo
 * 2^32; compare deltas at intervals shorter than 71 minutes. Individual
 * counters are atomic; a heartbeat may straddle one phase transition. */
enum server_work_phase {
	SERVER_WORK_PREP, SERVER_WORK_SIM, SERVER_WORK_SAVE, SERVER_WORK_EVICT,
	SERVER_WORK_SCAN, SERVER_WORK_GEN, SERVER_WORK_SLOT, SERVER_WORK_COPY,
	SERVER_WORK_SEND, SERVER_WORK_FAR, SERVER_WORK_TAIL, SERVER_WORK_COUNT
};
void server_local_work_profile_enable(bool enabled); /* before server start */
void server_local_work_stats(uint32_t out[SERVER_WORK_COUNT]);
#endif
