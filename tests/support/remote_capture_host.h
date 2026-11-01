/* Native host boundaries for the Wii remote-capture test harnesses.
 * Wii's net_fcntl nonblocking bit is 4; translate it at the host boundary. */
#ifndef TEST_REMOTE_CAPTURE_HOST_H
#define TEST_REMOTE_CAPTURE_HOST_H

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
typedef SOCKET capture_socket_t;
typedef int capture_socklen_t;
typedef unsigned int socklen_t;
#define CAPTURE_INVALID_SOCKET INVALID_SOCKET
static inline void capture_network_init(void) {
    WSADATA data;
    assert(!WSAStartup(MAKEWORD(2, 2), &data));
}
static inline void capture_network_cleanup(void) { assert(!WSACleanup()); }
static inline uint64_t capture_clock_ms(void) { return GetTickCount64(); }
static inline void capture_sleep_ms(unsigned ms) { Sleep(ms); }
static inline int capture_socket_close(capture_socket_t fd) { return closesocket(fd); }
static inline int capture_nonblock(capture_socket_t fd, bool enabled) {
    u_long on = enabled;
    return ioctlsocket(fd, FIONBIO, &on);
}
static inline int capture_socket_error(void) {
    int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK ? -EAGAIN : error == WSAEINTR ? -EINTR : -EIO;
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
typedef int capture_socket_t;
typedef socklen_t capture_socklen_t;
#define CAPTURE_INVALID_SOCKET (-1)
static inline void capture_network_init(void) { assert(signal(SIGPIPE, SIG_IGN) != SIG_ERR); }
static inline void capture_network_cleanup(void) {}
static inline uint64_t capture_clock_ms(void) {
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static inline void capture_sleep_ms(unsigned ms) {
    struct timespec delay = {ms / 1000, (long)(ms % 1000) * 1000000};
    while(nanosleep(&delay, &delay) && errno == EINTR) {}
}
static inline int capture_socket_close(capture_socket_t fd) { return close(fd); }
static inline int capture_nonblock(capture_socket_t fd, bool enabled) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
}
static inline int capture_socket_error(void) {
    return errno == EWOULDBLOCK || errno == EAGAIN ? -EAGAIN : errno == EINTR ? -EINTR : -EIO;
}
#endif
#endif
