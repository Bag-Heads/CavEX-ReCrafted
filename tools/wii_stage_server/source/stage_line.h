#ifndef CAVEWII_STAGE_LINE_H
#define CAVEWII_STAGE_LINE_H

/* The stage server's request-line reader, as pure policy with the socket
 * injected (same shape as stage_send.h / stage_saves.h: the decision lives
 * in a header a PC test can execute, main.c supplies the real recv).
 *
 * push-0921/C32, inventory row main.c:289-305. The old reader filled its
 * buffer and returned the TRUNCATED PREFIX as a perfectly ordinary request
 * line, leaving the rest of the line in the socket -- where it was read as
 * the NEXT request. One overlong line was therefore parsed as two, the
 * second of them entirely attacker-chosen, and the first one was a header
 * whose parse said nothing about the bytes that had been dropped. An
 * oversized line is now REFUSED with its own reason and drained to the
 * newline, so the connection stays in sync for exactly one refusal reply.
 */

#include <stddef.h>

enum {
	/* Peer closed before any byte of a line arrived: an idle disconnect. */
	CW_STAGE_LINE_CLOSED = -2,
	/* Peer closed mid-line: what arrived is not a request. */
	CW_STAGE_LINE_TRUNCATED = -1,
	/* Longer than the caller's buffer: refused, and drained to the '\n'. */
	CW_STAGE_LINE_TOO_LONG = -3
};

/* Bound on the drain: a peer that never sends a newline cannot hold the
 * single-client server open by trickling bytes. Past this the connection is
 * unrecoverably desynchronised and the caller closes it. */
#define CW_STAGE_LINE_DRAIN_MAX 65536

/* recv1: 1 = one byte written to *c; 0 or negative = closed / error. */
static inline int cw_stage_recv_line(int (*recv1)(void*, char*), void* user,
									 char* dst, size_t dst_size) {
	size_t n = 0;
	size_t drained = 0;
	if(!recv1 || !dst || dst_size == 0)
		return CW_STAGE_LINE_TRUNCATED;
	dst[0] = '\0';
	while(n + 1 < dst_size) {
		char c = 0;
		int r = recv1(user, &c);
		if(r <= 0)
			return n == 0 ? CW_STAGE_LINE_CLOSED : CW_STAGE_LINE_TRUNCATED;
		if(c == '\n') {
			dst[n] = '\0';
			return (int)n;
		}
		if(c != '\r')
			dst[n++] = c;
	}
	dst[n] = '\0';
	for(;;) {
		char c = 0;
		int r = recv1(user, &c);
		if(r <= 0 || c == '\n')
			return CW_STAGE_LINE_TOO_LONG;
		if(++drained >= CW_STAGE_LINE_DRAIN_MAX)
			return CW_STAGE_LINE_TOO_LONG;
	}
}

#endif
