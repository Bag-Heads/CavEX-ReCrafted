/* PC-only socket backend. Included by source/platform/net_socket.c. */

#include "../net_socket.h"

#include <stdio.h>
#include <string.h>
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
#include <stdatomic.h>
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET native_socket_t;
#define NATIVE_INVALID INVALID_SOCKET
#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int native_socket_t;
#define NATIVE_INVALID (-1)
#endif

/* Bounded TCP connect phase (2026-08-20 provider-swap redesign, hostile-
 * review finding 2): the old blocking connect() had NO timeout -- a
 * blackholed host held the calling thread ~21s on Windows and up to ~130s
 * on Linux, doubled with A+AAAA records. Non-blocking connect + select()
 * caps each address attempt. getaddrinfo above this stays blocking (DNS
 * has no portable cancel), which is exactly why server_remote runs this
 * whole function on a DISPOSABLE connector thread it can abandon instead
 * of join (see remote_connector_thread). */
#define NET_CONNECT_TIMEOUT_MS 10000

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static size_t s_test_send_limit;

void net_socket_test_set_send_limit(size_t bytes) {
	s_test_send_limit = bytes;
}

/* Written by whichever thread calls net_socket_receive (server_remote's read
 * thread), sampled by the test's own thread -- C11 atomics, relaxed, for the
 * same reason server_remote_internal.h's rx_bytes uses them: a plain counter
 * read across threads is UB, not merely a torn read. Relaxed is sufficient;
 * nothing synchronizes against these. */
static atomic_size_t s_test_recv_calls;
static atomic_size_t s_test_recv_bytes;

void net_socket_test_reset_recv_stats(void) {
	atomic_store_explicit(&s_test_recv_calls, 0, memory_order_relaxed);
	atomic_store_explicit(&s_test_recv_bytes, 0, memory_order_relaxed);
}

size_t net_socket_test_recv_calls(void) {
	return atomic_load_explicit(&s_test_recv_calls, memory_order_relaxed);
}

size_t net_socket_test_recv_bytes(void) {
	return atomic_load_explicit(&s_test_recv_bytes, memory_order_relaxed);
}
#endif

static void set_error(char* error, size_t error_size, const char* message) {
	if(error && error_size)
		snprintf(error, error_size, "%s", message ? message : "socket error");
}

bool net_socket_platform_init(char* error, size_t error_size) {
#ifdef _WIN32
	WSADATA data;
	if(WSAStartup(MAKEWORD(2, 2), &data) != 0) {
		set_error(error, error_size, "WSAStartup failed");
		return false;
	}
#else
	(void)error;
	(void)error_size;
#endif
	return true;
}

void net_socket_platform_cleanup(void) {
#ifdef _WIN32
	WSACleanup();
#endif
}

/* One address attempt: non-blocking connect, select-bounded, then blocking
 * mode restored (the read/write threads use blocking IO under the 30s Beta
 * rcv/snd timeouts set by the caller). Returns false on ANY failure --
 * refusal, timeout, or a mode-switch error -- and the caller closes the
 * candidate socket. */
static bool connect_bounded(native_socket_t candidate,
							const struct sockaddr* addr, int addr_len) {
#ifdef _WIN32
	u_long nonblocking = 1;
	if(ioctlsocket(candidate, FIONBIO, &nonblocking) != 0)
		return false;
	if(connect(candidate, addr, addr_len) != 0) {
		if(WSAGetLastError() != WSAEWOULDBLOCK)
			return false;
#else
	/* fd >= FD_SETSIZE makes FD_SET below an out-of-bounds STACK write on
	 * glibc (re-review finding 5) -- possible in a long-lived process with
	 * many open region files. Windows fd_set is a counted array; its
	 * FD_SET drops silently past capacity, no OOB. */
	if(candidate >= FD_SETSIZE)
		return false;
	int flags = fcntl(candidate, F_GETFL, 0);
	if(flags < 0 || fcntl(candidate, F_SETFL, flags | O_NONBLOCK) < 0)
		return false;
	if(connect(candidate, addr, (socklen_t)addr_len) != 0) {
		/* POSIX: EINTR on connect means the attempt CONTINUES
		 * asynchronously -- fall through to the select wait, same as
		 * EINPROGRESS (re-review finding 5). */
		if(errno != EINPROGRESS && errno != EINTR)
			return false;
#endif
		fd_set write_set, except_set;
		FD_ZERO(&write_set);
		FD_ZERO(&except_set);
		FD_SET(candidate, &write_set);
		FD_SET(candidate, &except_set);
		struct timeval bound = {.tv_sec = NET_CONNECT_TIMEOUT_MS / 1000,
								.tv_usec = (NET_CONNECT_TIMEOUT_MS % 1000)
									* 1000};
		/* Windows ignores select's nfds; POSIX needs max fd + 1. A POSIX
		 * EINTR restarts the wait with the SAME budget (slightly extends
		 * the worst-case bound; the connector is abandonable, so this
		 * stays honest) -- re-review finding 5. */
		int selected;
		do {
			selected = select((int)candidate + 1, NULL, &write_set,
							  &except_set, &bound);
		} while(selected < 0 &&
#ifdef _WIN32
				false
#else
				errno == EINTR
#endif
		);
		if(selected <= 0)
			return false; /* timed out (or select failed) */
		int so_error = 0;
		socklen_t so_len = sizeof(so_error);
		if(getsockopt(candidate, SOL_SOCKET, SO_ERROR, (char*)&so_error,
					  &so_len)
			   != 0
		   || so_error != 0)
			return false; /* refused/unreachable, reported via except/SO_ERROR */
	}
#ifdef _WIN32
	nonblocking = 0;
	if(ioctlsocket(candidate, FIONBIO, &nonblocking) != 0)
		return false;
#else
	if(fcntl(candidate, F_SETFL, flags) < 0)
		return false;
#endif
	return true;
}

net_socket_t net_socket_connect(const char* host, uint16_t port, char* error,
								size_t error_size) {
	char service[6];
	snprintf(service, sizeof(service), "%u", (unsigned)port);
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	struct addrinfo* addresses = NULL;
	int gai = getaddrinfo(host, service, &hints, &addresses);
	if(gai != 0) {
		set_error(error, error_size, gai_strerror(gai));
		return NET_SOCKET_INVALID;
	}

	native_socket_t connected = NATIVE_INVALID;
	for(struct addrinfo* address = addresses; address; address = address->ai_next) {
		native_socket_t candidate = socket(address->ai_family, address->ai_socktype,
										 address->ai_protocol);
		if(candidate == NATIVE_INVALID)
			continue;
		if(connect_bounded(candidate, address->ai_addr,
						   (int)address->ai_addrlen)) {
			connected = candidate;
			break;
		}
#ifdef _WIN32
		closesocket(candidate);
#else
		close(candidate);
#endif
	}
	freeaddrinfo(addresses);
	if(connected == NATIVE_INVALID) {
		set_error(error, error_size, "connect failed or timed out");
		return NET_SOCKET_INVALID;
	}
	/* Beta's NetworkManager bounds a silent peer at 30 seconds.
	 * [740c583 NetworkManager.NetworkManager] */
#ifdef _WIN32
	DWORD timeout_ms = 30000;
	(void)setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO,
					 (const char*)&timeout_ms, sizeof(timeout_ms));
	(void)setsockopt(connected, SOL_SOCKET, SO_SNDTIMEO,
					 (const char*)&timeout_ms, sizeof(timeout_ms));
#else
	struct timeval timeout = {.tv_sec = 30, .tv_usec = 0};
	(void)setsockopt(connected, SOL_SOCKET, SO_RCVTIMEO, &timeout,
					 sizeof(timeout));
	(void)setsockopt(connected, SOL_SOCKET, SO_SNDTIMEO, &timeout,
					 sizeof(timeout));
#endif
	return (net_socket_t)connected;
}

int net_socket_receive(net_socket_t socket_value, void* data, size_t length) {
	native_socket_t socket = (native_socket_t)socket_value;
#ifdef _WIN32
	int count = recv(socket, (char*)data, (int)length, 0);
#else
	int count = (int)recv(socket, data, length, 0);
#endif
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	atomic_fetch_add_explicit(&s_test_recv_calls, 1, memory_order_relaxed);
	if(count > 0)
		atomic_fetch_add_explicit(&s_test_recv_bytes, (size_t)count,
								  memory_order_relaxed);
#endif
	return count;
}

int net_socket_send(net_socket_t socket_value, const void* data, size_t length) {
	native_socket_t socket = (native_socket_t)socket_value;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_test_send_limit && length > s_test_send_limit)
		length = s_test_send_limit;
#endif
#ifdef _WIN32
	return send(socket, (const char*)data, (int)length, 0);
#else
	/* A peer that already closed its end (or the loopback test fixtures that
	 * intentionally close mid-fixture) makes this send() hit a broken pipe.
	 * Default SIGPIPE disposition kills the whole process before send() can
	 * even return -- MSG_NOSIGNAL keeps the failure inside this return value
	 * (EPIPE, count < 0) so the existing "socket write failed" error path in
	 * remote_write_thread (server_remote_transport.c) handles it like any
	 * other send failure. Windows has no MSG_NOSIGNAL and never raises
	 * SIGPIPE on send(), so that branch above is unaffected. */
#ifdef MSG_NOSIGNAL
	return (int)send(socket, data, length, MSG_NOSIGNAL);
#else
	return (int)send(socket, data, length, 0);
#endif
#endif
}

void net_socket_shutdown_write(net_socket_t socket_value) {
	if(socket_value == NET_SOCKET_INVALID)
		return;
#ifdef _WIN32
	shutdown((native_socket_t)socket_value, SD_SEND);
#else
	shutdown((native_socket_t)socket_value, SHUT_WR);
#endif
}

void net_socket_shutdown(net_socket_t socket_value) {
	if(socket_value == NET_SOCKET_INVALID)
		return;
#ifdef _WIN32
	shutdown((native_socket_t)socket_value, SD_BOTH);
#else
	shutdown((native_socket_t)socket_value, SHUT_RDWR);
#endif
}

void net_socket_close(net_socket_t socket_value) {
	if(socket_value == NET_SOCKET_INVALID)
		return;
#ifdef _WIN32
	closesocket((native_socket_t)socket_value);
#else
	close((native_socket_t)socket_value);
#endif
}
