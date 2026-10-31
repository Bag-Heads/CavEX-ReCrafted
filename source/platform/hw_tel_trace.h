/* First-stop evidence. Sender-owned; never allocated or written in a fault handler. */
#ifndef CW_HW_TEL_TRACE_H
#define CW_HW_TEL_TRACE_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
enum cw_tel_reason { CW_TEL_SEND = 1, CW_TEL_QUIESCE, CW_TEL_SHUTDOWN,
	CW_TEL_INIT, CW_TEL_UDP };
struct cw_tel_trace {
	uint64_t os_time;
	uint32_t reason, frame, tick, queued, length, stalls, pending;
	int32_t error, result, syscall_errno, mem2_free_kb, last[3];
	char kind[24];
};
static inline void cw_tel_trace_capture(struct cw_tel_trace* t,
	unsigned reason, const char* line, size_t queued, unsigned stalls,
	int error, int result, const int last[3]) {
	if(t->reason)
		return; /* first stop wins, even if shutdown follows */
	t->reason = reason;
	t->error = error;
	t->result = result;
	t->queued = (uint32_t)queued;
	t->stalls = stalls;
	t->length = line ? (uint32_t)strlen(line) : 0;
	memcpy(t->last, last, sizeof(t->last));
	const char* kind = line ? strstr(line, "\"event\":\"") : NULL;
	if(kind) {
		kind += 9;
		size_t n = 0;
		while(kind[n] && kind[n] != '"' && n < sizeof(t->kind) - 1)
			n++;
		memcpy(t->kind, kind, n);
		t->kind[n] = 0;
	}
}
static inline size_t cw_tel_trace_format(char* dst, size_t cap,
	const struct cw_tel_trace* t, const char* run) {
	if(!cap)
		return 0;
	int n = snprintf(dst, cap,
		"run=%s reason=%u errno=%d net_send=%d syscall_errno=%d queued=%u partial_unknown=%u "
		"pending=%u kind=%s length=%u stalls=%u frame_counter=%u tick=%u "
		"OSGetTime=%llu mem2_free_kb=%d last3=[%d,%d,%d]\n",
		run, t->reason, t->error, t->result, t->syscall_errno, t->queued,
		(t->reason == CW_TEL_SEND && t->result <= 0) || t->result < 0,
		t->pending, t->kind, t->length, t->stalls, t->frame, t->tick,
		(unsigned long long)t->os_time, t->mem2_free_kb,
		t->last[0], t->last[1], t->last[2]);
	return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}
/* Exactly 64 bytes; leading LF resynchronizes after an ambiguous prefix.
 * Diagnostic only: no run id, no heartbeat validity or PASS semantics. */
static inline void cw_tel_alive_format(char out[64], uint32_t frame, uint32_t tick) {
	memset(out, ' ', 64);
	int n = snprintf(out, 64, "\n{\"event\":\"alive\",\"frame\":%u,\"tick\":%u}", frame, tick);
	if(n > 0 && n < 63)
		out[n] = ' ';
	out[63] = '\n';
}
#endif
