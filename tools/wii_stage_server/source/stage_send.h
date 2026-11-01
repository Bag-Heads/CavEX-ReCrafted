#ifndef CAVEWII_STAGE_SEND_H
#define CAVEWII_STAGE_SEND_H

/* Duplication-safe "send this whole buffer" loop for the Wii stage server
 * (main.c) and its PC unit test (tests/test_stage_send.c). Pure,
 * header-only (static inline), zero I/O -- the actual transmit is injected
 * through cw_stage_send_ops so the PC test drives the SAME loop main.c
 * compiles, against a fake IOS socket.
 *
 * ------------------------------------------------------------------------
 * WHY THIS EXISTS -- the 2026-09-02 real-Wii CWFETCH1 corruption
 * ------------------------------------------------------------------------
 * Symptom: fetching sd:/cw_boot_trace.txt returned the RIGHT byte count but
 * the WRONG bytes (server declared sha256 X, client's received bytes hashed
 * to Y). Three independent hardware captures survive in tmp/wii_runs/<run>/
 * stage_mailbox/previous_cw_boot_trace.txt:
 *
 *   run                             bytes   header  1st dup boundary
 *   staged_heartbeat_selftest_0844   8567     124    body 3972 -> abs 4096
 *   staged_mp_lan_walk_10m_0920      8404     124    body 3972 -> abs 4096
 *   staged_far_idle_2m_1024         20619     125    body 3971 -> abs 4096
 *
 * Every one of those received streams is a CONCATENATION OF PREFIXES of the
 * file: e.g. for the 20619-byte capture, received =
 *   file[0:3971] ++ file[0:1585] ++ file[0:2920] ++ file[0:2636]
 *              ++ file[0:2920] ++ file[0:2636] ++ file[0:2920] ++ file[0:1031]
 * (verified byte-for-byte; the segment boundaries are where the file's own
 * 207-byte first line reappears). The client reads exactly the declared
 * `bytes` and stops, so the COUNT is right and the CONTENT is a repeated
 * head -- exactly the reported symptom.
 *
 * The three captures pin the mechanism precisely. Their fetch REPLY HEADER
 * lines differ in length (124 bytes for a 4-digit `"bytes"`, 125 for a
 * 5-digit one) and the first duplication boundary moves by exactly that one
 * byte in the opposite direction, so the boundary sits at a fixed ABSOLUTE
 * offset of 4096 bytes into the connection's payload -- i.e. at the moment
 * the IOS socket's send buffer (4 KiB) first filled. That is not a TCP
 * artifact and not a file-content artifact; it is the socket buffer.
 *
 * Root cause: since the 2026-08-31 DoS hardening (0869f6f) the accepted
 * client socket is NON-BLOCKING, and main.c's cw_client_send() looped
 *
 *     for(;;) { r = net_send(sock, buf, len, 0);
 *               if(r >= 0) return r;
 *               if(r is EAGAIN/EWOULDBLOCK/EINTR) { sleep; continue; } }
 *
 * retrying FROM THE SAME POINTER after -EAGAIN. On libogc/IOS a net_send
 * that cannot take the whole request QUEUES WHAT FITS AND STILL REPORTS AN
 * ERROR, throwing the partial count away. This project already documented
 * that exact trap twice, in the game's own Wii net code:
 *   - source/platform/hw_telemetry.c tcp_send_bytes(): "Retrying after
 *     -EAGAIN is NOT duplication-safe: libogc's net_send loops internally
 *     and CLOBBERS partial progress ... so a retry from the same offset can
 *     duplicate bytes mid-line and corrupt the NDJSON stream."
 *   - source/platform/wii/net_socket.c header: "libogc's net_sendto loops
 *     in 32 KB chunks and DISCARDS the sent count when a later chunk errors
 *     ... so a multi-chunk send on a non-blocking socket can silently lose
 *     bytes."
 * The stage server simply never got that memo. Every OTHER stage-server
 * reply is a single short JSON line that fits the socket buffer in one call
 * and returns >= 0, which is why only the fetch BODY -- the one payload
 * that spans more than one net_send -- was ever corrupted, and why uploads
 * (a recv path, sha256-verified on the card) always passed.
 *
 * ------------------------------------------------------------------------
 * THE CONTRACT THIS HEADER ENFORCES
 * ------------------------------------------------------------------------
 * A negative return from the transmit hook means "an UNKNOWN prefix of this
 * request, anywhere in [0, len), may already be on the wire". There is no
 * way to resynchronise a TCP stream after that, so:
 *
 *   1. A negative return is FATAL. It is never retried, and never will be.
 *      The caller aborts the transfer and closes the connection; the client
 *      sees a short stream and fails LOUDLY (fetch_wii_stage_file.ps1
 *      already throws "stage server closed after N / M bytes") instead of
 *      silently keeping duplicated bytes.
 *   2. Requests are capped to CW_STAGE_SEND_MAX_CHUNK, which is both well
 *      under the measured 4 KiB IOS socket send buffer and far under
 *      libogc's 32 KiB internal net_sendto chunk loop, so that loop is
 *      single-iteration by construction (the net_socket.c precedent).
 *   3. The caller PACES between chunks (cw_stage_send_ops::pace) so the
 *      socket buffer drains and rule 1 effectively never fires: with <= 1
 *      KiB outstanding against a 4 KiB buffer on a LAN, a full buffer needs
 *      a peer that has genuinely stopped reading, which is the case rule 1
 *      is there to refuse.
 *
 * Tradeoff, stated on purpose rather than hidden: this trades a rare, LOUD,
 * client-retryable abort for the previous silent corruption, and caps
 * throughput at roughly one chunk per pace tick (~1 MB/s at 1 KiB / 1 ms) --
 * a few tens of milliseconds for the KB-sized telemetry reports this server
 * actually fetches. A partial count reported POSITIVELY is still trusted and
 * simply advances the cursor, because a positive return is the one thing
 * IOS's send does report unambiguously.
 */

#include <stddef.h>

/* One IOS transfer. Chosen to satisfy both bounds above:
 *   - <= 1/4 of the measured 4096-byte IOS socket send buffer, so the
 *     paced steady state never approaches a full buffer;
 *   - <<  libogc's 32 KiB net_sendto chunk size, so its internal loop
 *     (which is what discards partial progress on a later chunk error) is
 *     single-iteration for every call we make. */
#define CW_STAGE_SEND_MAX_CHUNK 1024u

/* The IOS socket send-buffer size this policy is calibrated against, as
 * MEASURED from the three hardware captures above (first duplication
 * boundary at absolute payload offset 4096 in all three, across two
 * different reply-header lengths). Not used for control flow -- recorded
 * here so the chunk cap's justification travels with the constant. */
#define CW_STAGE_SEND_IOS_SNDBUF 4096u

enum cw_stage_send_status {
	CW_STAGE_SEND_OK = 0,
	/* Transmit hook returned < 0: an unknown prefix may already be on the
	 * wire. Caller MUST abandon the connection, not retry. */
	CW_STAGE_SEND_ERR_AMBIGUOUS = 1,
	/* No forward progress within the caller's own deadline (pace() said
	 * stop). Nothing extra was queued, but the transfer cannot finish. */
	CW_STAGE_SEND_ERR_STALLED = 2,
	/* Transmit hook claimed to have sent more than it was asked for. A
	 * transport that miscounts upward would silently SKIP payload bytes, so
	 * refuse it rather than trust it. */
	CW_STAGE_SEND_ERR_BADCOUNT = 3,
	CW_STAGE_SEND_ERR_ARGS = 4
};

struct cw_stage_send_ops {
	/* Transmit at most `len` bytes from `data`. Contract:
	 *    > 0  exactly this many bytes were queued (partials are trusted)
	 *   == 0  nothing was queued, no error -- safe to pace and retry
	 *    < 0  UNKNOWN prefix may have been queued -- never retried */
	int (*send)(void* user, const unsigned char* data, int len);
	/* Called between chunks (and after a zero-progress send). Returns
	 * non-zero to keep going, 0 to give up on a deadline. May be NULL, in
	 * which case a zero-progress send is treated as a stall immediately. */
	int (*pace)(void* user);
};

/* Chunk cap as a function, so the test pins the same arithmetic main.c
 * compiles rather than a re-typed copy of it. */
static inline int cw_stage_send_chunk_len(size_t remain) {
	if(remain > (size_t)CW_STAGE_SEND_MAX_CHUNK)
		return (int)CW_STAGE_SEND_MAX_CHUNK;
	return (int)remain;
}

/* Sends exactly `len` bytes of `data`, or fails. On any non-OK return
 * `*out_sent` (when non-NULL) holds the number of bytes the transport
 * ACKNOWLEDGED queuing before the failure -- which, for
 * CW_STAGE_SEND_ERR_AMBIGUOUS, is a LOWER BOUND on what reached the wire,
 * never an exact figure. That is precisely why the caller must close the
 * connection instead of resuming from it. */
static inline int cw_stage_send_all(const struct cw_stage_send_ops* ops,
									void* user, const unsigned char* data,
									size_t len, size_t* out_sent) {
	size_t sent = 0;
	if(out_sent)
		*out_sent = 0;
	if(!ops || !ops->send || (!data && len > 0))
		return CW_STAGE_SEND_ERR_ARGS;
	while(sent < len) {
		int want = cw_stage_send_chunk_len(len - sent);
		int n = ops->send(user, data + sent, want);
		if(n < 0)
			return CW_STAGE_SEND_ERR_AMBIGUOUS;
		if(n > want)
			return CW_STAGE_SEND_ERR_BADCOUNT;
		if(n > 0) {
			sent += (size_t)n;
			if(out_sent)
				*out_sent = sent;
			if(sent >= len)
				break;
		}
		/* Pace after every chunk, not just after a zero-progress one: the
		 * point is to keep the socket buffer drained BEFORE it fills, since
		 * a full buffer is what produced the ambiguous -EAGAIN in the first
		 * place. */
		if(!ops->pace || !ops->pace(user))
			return CW_STAGE_SEND_ERR_STALLED;
	}
	return CW_STAGE_SEND_OK;
}

#endif /* CAVEWII_STAGE_SEND_H */
