#ifndef CW_PC_HEAP_STATS_H
#define CW_PC_HEAP_STATS_H

#include <stdbool.h>
#include <stddef.h>

/* Diagnostic census, not a frame-time instrument. The Windows process heap
 * reports BUSY allocations; Wii mallinfo.ordblks instead counts FREE pieces. */
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>

struct pc_heap_stats {
	size_t blocks, bytes, working_set;
	bool valid;
};

static struct pc_heap_stats pc_heap_stats(void) {
	struct pc_heap_stats stats = {0};
	HANDLE heap = GetProcessHeap();
	PROCESS_HEAP_ENTRY entry = {0};
	if(HeapLock(heap)) {
		while(HeapWalk(heap, &entry)) {
			if(entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
				stats.blocks++;
				stats.bytes += entry.cbData;
			}
		}
		stats.valid = GetLastError() == ERROR_NO_MORE_ITEMS;
		HeapUnlock(heap);
	}
	PROCESS_MEMORY_COUNTERS memory = {0};
	if(K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
		stats.working_set = memory.WorkingSetSize;
	return stats;
}
#endif
#endif
