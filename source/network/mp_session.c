/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "mp_session.h"

#ifdef CAVEWII_MP_CLIENT

#include <stdio.h>
#include <string.h>

static struct server_remote g_remote;
static bool g_initialized;

static struct {
	bool pending;
	char host[MP_ADDRESS_HOST_CAPACITY];
	uint16_t port;
	char username[MP_SESSION_USERNAME_CAPACITY];
} g_pending;

void mp_session_init(void) {
	if(g_initialized)
		return;
	g_initialized = true; // handle only: local play allocates no remote state
}

bool mp_session_prepare_remote(void) {
	mp_session_init();
	return g_remote.impl || server_remote_init(&g_remote);
}

void mp_session_release_remote(void) {
	struct server_remote_status status;
	server_remote_get_status(&g_remote, &status);
	mp_session_note_request_dropped(status.reason[0] ? status.reason
		: g_remote.impl ? "Disconnected" : "Could not initialize remote provider");
	server_remote_destroy(&g_remote);
}

void mp_session_destroy(void) {
	if(!g_initialized)
		return;
	server_remote_destroy(&g_remote);
	g_initialized = false;
}

struct server_remote* mp_session_remote(void) {
	return g_initialized ? &g_remote : NULL;
}

bool mp_session_request_connect(const char* host, uint16_t port,
								const char* username_utf8) {
	if(!host || !*host || !username_utf8 || !*username_utf8)
		return false;
	if(strlen(host) + 1 > sizeof(g_pending.host)
	   || strlen(username_utf8) + 1 > sizeof(g_pending.username))
		return false;
	snprintf(g_pending.host, sizeof(g_pending.host), "%s", host);
	snprintf(g_pending.username, sizeof(g_pending.username), "%s",
			username_utf8);
	g_pending.port = port;
	g_pending.pending = true;
	return true;
}

/* Re-review finding 3: when the seam consumes-and-drops a request it cannot
 * serve, the connect screen's generation baseline never moves and it used
 * to spin "Connecting..." forever. The seam leaves a one-shot notice here;
 * the screen polls it while waiting at its baseline. Main-thread only,
 * like g_pending. */
static struct {
	bool pending;
	char reason[64];
} g_drop_notice;

void mp_session_note_request_dropped(const char* why) {
	snprintf(g_drop_notice.reason, sizeof(g_drop_notice.reason), "%s",
			 why && *why ? why : "Connect request dropped");
	g_drop_notice.pending = true;
}

bool mp_session_take_drop_notice(char* reason_out, size_t reason_cap) {
	if(!g_drop_notice.pending)
		return false;
	if(reason_out)
		snprintf(reason_out, reason_cap, "%s", g_drop_notice.reason);
	g_drop_notice.pending = false;
	return true;
}

void mp_session_cancel_pending(void) {
	/* Finding 3 (2026-08-20 redesign): before this existed, a queued
	 * request outlived the connect screen -- Cancel left it armed and the
	 * main-loop seam could fire it many frames later against a live
	 * singleplayer world. Cancel/Back now clears it explicitly. */
	g_pending.pending = false;
}

bool mp_session_take_pending_connect(char* host_out, size_t host_cap,
									 uint16_t* port_out, char* user_out,
									 size_t user_cap) {
	if(!g_pending.pending || !host_out || !port_out || !user_out)
		return false;
	snprintf(host_out, host_cap, "%s", g_pending.host);
	snprintf(user_out, user_cap, "%s", g_pending.username);
	*port_out = g_pending.port;
	g_pending.pending = false;
	return true;
}

/* The counter read, through the existing production snapshot. No new state,
 * no second transport, no cached copy that could drift from what F3 and the
 * telemetry heartbeat already publish. */
bool mp_session_chat_lines_sent(uint32_t* out) {
	if(!out || !g_initialized || !g_remote.impl)
		return false;
	struct server_remote_status status;
	server_remote_get_status(&g_remote, &status);
	*out = status.chat_lines_sent;
	return true;
}

#else /* !CAVEWII_MP_CLIENT */

/* This whole translation unit compiles to nothing without the MP client, but
 * the counter accessor is declared unconditionally so its one caller (main.c's
 * chat_login hook) needs no #ifdef of its own. No provider, no counter. */
bool mp_session_chat_lines_sent(uint32_t* out) {
	(void)out;
	return false;
}

#endif /* CAVEWII_MP_CLIENT */
