/* PC transport seam for the staged online client. libogc is S5. */
#ifndef NET_SOCKET_H
#define NET_SOCKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef intptr_t net_socket_t;
#define NET_SOCKET_INVALID ((net_socket_t)-1)

bool net_socket_platform_init(char* error, size_t error_size);
void net_socket_platform_cleanup(void);
net_socket_t net_socket_connect(const char* host, uint16_t port, char* error,
								size_t error_size);
/* Positive byte count, zero peer-close, negative failure. Wii retains the
 * negative IOS errno (including -ETIMEDOUT); PC currently returns -1. */
int net_socket_receive(net_socket_t socket, void* data, size_t length);
int net_socket_send(net_socket_t socket, const void* data, size_t length);
void net_socket_shutdown_write(net_socket_t socket);
void net_socket_shutdown(net_socket_t socket);
void net_socket_close(net_socket_t socket);

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* Deterministic writer-loop fault seam; zero restores normal sends. */
void net_socket_test_set_send_limit(size_t bytes);
/* Drain-rate observability for the 2026-09-02 "world never reported loaded"
 * regression guard: how many net_socket_receive() calls (== how many IOS
 * network round trips on the Wii backend) the read thread needed, and how
 * many payload bytes they returned. A client that drains a burst in tiny
 * slices is what lets mc173's unbuffered non-blocking writes silently
 * discard chunk columns it has already marked delivered -- see
 * server_remote_internal.h's read_receive_buffer comment. Counters are
 * process-wide and reset together. */
void net_socket_test_reset_recv_stats(void);
size_t net_socket_test_recv_calls(void);
size_t net_socket_test_recv_bytes(void);
#endif

#endif
