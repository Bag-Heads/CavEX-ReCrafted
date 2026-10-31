/* Wii/libogc IOS socket backend (S5). Included by
 * source/platform/net_socket.c -- never compiled standalone, mirroring the
 * gfx/audio/watchdog dispatcher convention.
 *
 * Design constraints, each pinned to a measured/documented source:
 *  - IOS's non-block flag is 0x04, NOT newlib's O_NONBLOCK (0x4000) nor
 *    network.h's octal fallback -- the emit_boot 2026-07-01 class.
 *    hw_telemetry.c's CW_IOS_O_NONBLOCK is the proven in-tree precedent.
 *  - libogc's net_sendto loops in 32 KB chunks and DISCARDS the sent count
 *    when a later chunk errors (network_wii.c: `ret = sent; break;`), so a
 *    multi-chunk send on a non-blocking socket can silently lose bytes
 *    (hw_telemetry.c documents the same trap for its NDJSON lines).
 *    net_recvfrom has the mirror-image loop. Every send/recv here is capped
 *    to ONE chunk so those loops are single-iteration by construction.
 *  - SO_RCVTIMEO/SO_SNDTIMEO support on IOS is unverified (S5 design doc
 *    #8.3), so the socket stays NON-blocking for its whole life and every
 *    receive/send call enforces the shared 30s Beta silent-peer bound
 *    [740c583 NetworkManager.NetworkManager] with its own deadline loop.
 *  - Connect completion is detected by re-calling net_connect until it
 *    reports -EISCONN (accepted-idiom on IOS) instead of net_select +
 *    SO_ERROR: net_getsockopt on IOS is another unverified surface, and the
 *    retry loop uses only primitives this project has already proven on
 *    hardware. net_select is still used for read/write waits, with an
 *    errno-degrading usleep fallback so a hardware net_select gap cannot
 *    hang or spin a worker (S5 doc #8.2/#8.3 stay open until measured). */

#include "../cw_canary.h" // ROUND 10 cycle 2: CW_SD_TRACE_OFF() kill switch
						  // for the connect_begin/connect_ret/recv_ret/
						  // first_send SD tracers below (PLATFORM_WII-gated
						  // no-op include on PC).
#include "../net_socket.h"
#include "../time.h"
#include "net_bringup.h"

#include <errno.h>
#include <fcntl.h> /* F_GETFL/F_SETFL for net_fcntl (hw_telemetry precedent) */
#include <gccore.h>
#include <network.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
int usleep(useconds_t __useconds);

#define CW_IOS_O_NONBLOCK 0x04

/* Same values as pc/net_socket.c (kept as named constants in both backends
 * on purpose -- the bound is Beta semantics, not a platform choice). */
#define NET_CONNECT_TIMEOUT_MS 10000
#define NET_IO_TIMEOUT_MS 30000

/* One IOS transfer <= half of libogc's 64 KB net heap (maxblocksize); our
 * MP frames are <= 1 KB and the read scratch is 256 B, so this cap is
 * defensive, not a throughput limit. */
#define NET_IOS_MAX_CHUNK 8192u

/* net_shutdown passes `how` straight to IOS; libogc ships no SHUT_*
 * constants. BSD numbering. Half-close honoring is S5 doc #8 [verify on
 * hardware]; the transport's teardown always follows with net_close, so a
 * refused half-close degrades to the documented full-close shape. */
#define CW_SHUT_WR 1
#define CW_SHUT_RDWR 2

/* select()-wait granularity: short enough that socket close/teardown from
 * another thread is noticed promptly, long enough not to busy-spin. */
#define NET_WAIT_SLICE_MS 250

static void wii_set_error(char* error, size_t error_size,
						  const char* message) {
	if(error && error_size)
		snprintf(error, error_size, "%s", message ? message : "socket error");
}

static bool wii_set_nonblock(s32 sock) {
	s32 flags = net_fcntl(sock, F_GETFL, 0);
	if(flags < 0)
		flags = 0;
	s32 r = net_fcntl(sock, F_SETFL, (u32)flags | CW_IOS_O_NONBLOCK);
	return r >= 0;
}

/* Bounded wait between non-blocking retries. NO net_select on purpose
 * (S5.3 hunt, take-4 probe verdict): a blocking SO_POLL ioctl parks
 * inside IOS's single network-IPC pipe, and with the read thread issuing
 * back-to-back 250ms selects, the main thread's telemetry sends and the
 * write thread's handshake send starved -- heartbeats died at tick 0 and
 * mc173 logged accept-then-silence while the connect itself (proven by
 * the same probe run: -EINPROGRESS then -EISCONN, 2 attempts) was fine.
 * Short-sleep polling is the idiom hw_telemetry.c has always used on
 * this hardware; 20ms keeps worst-case added latency far below any Beta
 * timing bound. Returns false once the deadline has passed. */
#define NET_POLL_SLEEP_MS 20
static bool wii_wait_io(s32 sock, bool for_write, ptime_t deadline) {
	(void)sock;
	(void)for_write;
	if(time_reached(time_get(), deadline))
		return false;
	usleep(NET_POLL_SLEEP_MS * 1000);
	return true;
}

bool net_socket_platform_init(char* error, size_t error_size) {
	/* On Wii there is NOTHING to initialize here: IOS net bring-up is a
	 * process-lifetime singleton owned by net_bringup (S5.2, background
	 * WiiMC-shape ladder), not a per-process socket-library init like PC's
	 * WSAStartup. This used to REPORT a not-yet-up network as an init
	 * FAILURE -- wrong layer: server_remote_init runs ONCE at boot
	 * (mp_session_init, main.c), BEFORE the connect screen's MPC_BRINGUP
	 * phase has had any chance to run the ladder, so a cold-network
	 * HBC-DIRECT boot permanently killed the MP session ("Multiplayer
	 * session unavailable") while chainloaded / armed-telemetry boots
	 * (whose boot-time if_config had already leased an IP) sailed through
	 * -- the 2026-08-30 field failure. Readiness is gated where it
	 * belongs: the connect screen holds the connect request until
	 * net_bringup reports READY, and net_socket_connect fails with a real
	 * error if the stack is genuinely down at connect time. */
	(void)error;
	(void)error_size;
	return true;
}

void net_socket_platform_cleanup(void) {
	/* Never net_deinit mid-session (S5 design doc section 1): bring-up is a
	 * process-lifetime singleton owned by net_bringup, not per-connection. */
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static size_t s_test_send_limit;

void net_socket_test_set_send_limit(size_t bytes) {
	s_test_send_limit = bytes;
}
#endif

net_socket_t net_socket_connect(const char* host, uint16_t port, char* error,
								size_t error_size) {
	if(!host || !host[0]) {
		wii_set_error(error, error_size, "empty host");
		return NET_SOCKET_INVALID;
	}

	/* Dotted-quad first; DNS only for real names. net_gethostbyname is one
	 * unbounded IOS ioctl (S5 doc 1b) -- acceptable ONLY because this whole
	 * function runs on the disposable connector thread the caller can
	 * abandon (server_remote_transport.c handoff contract). */
	u32 addr = inet_addr(host);
	if(addr == 0 || addr == 0xFFFFFFFFu) {
		struct hostent* resolved = net_gethostbyname(host);
		if(!resolved || resolved->h_addrtype != AF_INET || !resolved->h_addr
		   || resolved->h_length != 4) {
			wii_set_error(error, error_size, "hostname lookup failed");
			return NET_SOCKET_INVALID;
		}
		memcpy(&addr, resolved->h_addr, 4);
	}

	s32 sock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if(sock < 0) {
		wii_set_error(error, error_size, "socket creation failed");
		return NET_SOCKET_INVALID;
	}
	if(!wii_set_nonblock(sock)) {
		/* A silently-blocking IOS socket recreates the boot-park class the
		 * 0x04 flag fix exists to kill -- refuse it (telemetry precedent). */
		net_close(sock);
		wii_set_error(error, error_size, "socket nonblock setup failed");
		return NET_SOCKET_INVALID;
	}

	struct sockaddr_in dest;
	memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_port = htons(port);
	dest.sin_addr.s_addr = addr;

	/* Non-blocking connect completion via net_connect retry until -EISCONN;
	 * bounded by the same 10s the PC backend uses.
	 * PROBE (S5.3 hunt, temporary): IOS SO_CONNECT's nonblocking errno
	 * contract is exactly what takes 2+3 imploded on (server accepts, this
	 * loop never converges) -- log every distinct return to SD so the
	 * hardware itself names the sequence. Connector-thread-only writes.
	 * ROUND 8 (Codex's cross-round theory, coordinator-escalated): this file
	 * was ALREADY commented "Connector-thread-only" -- the original intent
	 * was single-writer-per-file -- but it reused the SAME filename the
	 * read thread's OWN tracer (server_remote_transport.c's cw_decode_trace/
	 * cw_lock_trace, added later in round 2) also targets, an accidental
	 * collision, not a design choice. Decisive test: give every OS thread
	 * that touches SD its own exclusive file and see if the mp_connect_smoke
	 * failure disappears or moves. Connector's own file, touched by no other
	 * thread. */
	FILE* mp_trace = CW_SD_TRACE_OFF()
						 ? NULL
						 : fopen("sd:/cw_trace_connector.txt", "a");
	if(mp_trace) {
		fprintf(mp_trace, "connect_begin sock=%ld dest=%08lx:%u\n", (long)sock,
				(unsigned long)dest.sin_addr.s_addr, (unsigned)port);
		fflush(mp_trace);
	}
	ptime_t deadline = time_add_ms(time_get(), NET_CONNECT_TIMEOUT_MS);
	bool connected = false;
	s32 connect_result = -ETIMEDOUT;
	s32 last_logged = 1; /* impossible value so the first result always logs */
	int attempts = 0;
	for(;;) {
		s32 result = net_connect(sock, (struct sockaddr*)&dest, sizeof(dest));
		connect_result = result;
		attempts++;
		if(mp_trace && (result != last_logged || attempts <= 3)) {
			fprintf(mp_trace, "connect_ret n=%d r=%ld\n", attempts,
					(long)result);
			fflush(mp_trace);
			last_logged = result;
		}
		if(result == 0 || result == -EISCONN) {
			connected = true;
			break;
		}
		if(result != -EINPROGRESS && result != -EALREADY
		   && result != -EAGAIN && result != -EWOULDBLOCK && result != -EINTR)
			break; /* refused/unreachable/hard error */
		if(!wii_wait_io(sock, true, deadline)) {
			connect_result = -ETIMEDOUT;
			break; /* timed out */
		}
	}
	if(mp_trace) {
		fprintf(mp_trace, "connect_end connected=%d attempts=%d\n",
				(int)connected, attempts);
		fclose(mp_trace);
	}
	if(!connected) {
		net_close(sock);
		/* Preserve both the actual IOS error and resolved endpoint. The old
		 * combined message made refusal, invalid arguments and timeout
		 * indistinguishable during hardware diagnosis. */
		const uint8_t* ip = (const uint8_t*)&dest.sin_addr.s_addr;
		if(error && error_size)
			snprintf(error, error_size, "connect %ld to %u.%u.%u.%u:%u",
				(long)connect_result, ip[0], ip[1], ip[2], ip[3], (unsigned)port);
		return NET_SOCKET_INVALID;
	}

	/* TCP_NODELAY: Beta's movement/keep-alive packets are exactly what
	 * Nagle coalescing is worst for (S5 doc section 5). IOS support is
	 * [verify on hardware]; a refusal is loud on stderr, never silent. */
	u32 one = 1;
	s32 nodelay_r = net_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	if(nodelay_r < 0)
		fprintf(stderr, "[net] TCP_NODELAY not honored by IOS\n");

	/* Socket deliberately STAYS non-blocking: receive/send below own the
	 * 30s Beta bound with deadline loops (SO_RCVTIMEO on IOS unverified). */
	return (net_socket_t)sock;
}

int net_socket_receive(net_socket_t socket_value, void* data, size_t length) {
	s32 sock = (s32)socket_value;
	if(sock < 0 || !data || length == 0)
		return -EINVAL;
	if(length > NET_IOS_MAX_CHUNK)
		length = NET_IOS_MAX_CHUNK;
	ptime_t deadline = time_add_ms(time_get(), NET_IO_TIMEOUT_MS);
	/* PROBE (S5.3 hunt, temporary, one-shot): the read thread's first 3
	 * net_recv verdicts on the MP socket -- captures whether mc173's
	 * handshake reply reaches this client at all (do NOT over-read mc173's
	 * own "accept, then silence" log per the hunt brief: the server may
	 * simply not log receipt -- this probe is the client's own view, which
	 * is authoritative). Bounded to 3 lines total across this thread's
	 * whole lifetime so a healthy long-lived connection's routine EAGAIN
	 * polling never spams the SD. Read-thread-only writes.
	 * ROUND 8: this ALSO targeted cw_mp_trace.txt -- the SAME file
	 * server_remote_transport.c's read-thread tracers use, so at least
	 * those two writers were already same-thread-safe with each other, but
	 * BOTH shared the file with the connector thread's connect_begin/
	 * connect_ret (net_socket_connect above) and, misrouted, with
	 * net_socket_send's first_send below (a DIFFERENT thread's write --
	 * see that comment). Own file now, shared only with this same read
	 * thread's other tracers in server_remote_transport.c. */
	static int s_recv_logged;
	for(;;) {
		s32 count = net_recv(sock, data, (s32)length, 0);
		if(s_recv_logged < 3 && !CW_SD_TRACE_OFF()) {
			s_recv_logged++;
			FILE* t = fopen("sd:/cw_trace_read.txt", "a");
			if(t) {
				fprintf(t, "recv_ret n=%d sock=%ld count=%ld\n", s_recv_logged,
						(long)sock, (long)count);
				fclose(t);
			}
		}
		if(count > 0)
			return (int)count;
		if(count == 0)
			return 0; /* peer closed */
		if(count != -EAGAIN && count != -EWOULDBLOCK && count != -EINTR)
			return (int)count; /* Preserve IOS error for the connection diagnosis. */
		if(!wii_wait_io(sock, false, deadline))
			return -ETIMEDOUT; /* Distinguish silent peer from a socket reset. */
	}
}

int net_socket_send(net_socket_t socket_value, const void* data,
					size_t length) {
	s32 sock = (s32)socket_value;
	if(sock < 0 || !data || length == 0)
		return -1;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_test_send_limit && length > s_test_send_limit)
		length = s_test_send_limit;
#endif
	if(length > NET_IOS_MAX_CHUNK)
		length = NET_IOS_MAX_CHUNK;
	ptime_t deadline = time_add_ms(time_get(), NET_IO_TIMEOUT_MS);
	/* PROBE (S5.3 hunt, temporary, one-shot): the first send verdict on the
	 * MP socket discriminates a dead write path from a starved one.
	 * ROUND 8: net_socket_send is called EXCLUSIVELY by the write thread
	 * (remote_write_thread's send loop, server_remote_transport.c) -- every
	 * queued frame, including the very first handshake, goes out through
	 * that one call site regardless of which thread queued it. This log was
	 * MISROUTED to cw_mp_trace.txt, the read/connector threads' file -- a
	 * THIRD distinct thread writing there. Own file, shared only with this
	 * same write thread's cw_write_trace markers
	 * (server_remote_transport.c). */
	static bool s_first_send_logged;
	for(;;) {
		s32 count = net_send(sock, data, (s32)length, 0);
		if(!s_first_send_logged && !CW_SD_TRACE_OFF()) {
			s_first_send_logged = true;
			FILE* t = fopen("sd:/cw_trace_write.txt", "a");
			if(t) {
				fprintf(t, "first_send sock=%ld len=%u r=%ld\n", (long)sock,
						(unsigned)length, (long)count);
				fclose(t);
			}
		}
		if(count > 0)
			return (int)count; /* partial is fine -- the write loop resumes */
		if(count != 0 && count != -EAGAIN && count != -EWOULDBLOCK
		   && count != -EINTR)
			return -1;
		if(!wii_wait_io(sock, true, deadline))
			return -1;
	}
}

void net_socket_shutdown_write(net_socket_t socket_value) {
	if(socket_value == NET_SOCKET_INVALID)
		return;
	net_shutdown((s32)socket_value, CW_SHUT_WR);
}

void net_socket_shutdown(net_socket_t socket_value) {
	if(socket_value == NET_SOCKET_INVALID)
		return;
	net_shutdown((s32)socket_value, CW_SHUT_RDWR);
}

void net_socket_close(net_socket_t socket_value) {
	if(socket_value == NET_SOCKET_INVALID)
		return;
	net_close((s32)socket_value);
}
