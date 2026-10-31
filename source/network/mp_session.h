/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Interim title-screen path (plan docs/mp_online_client_plan_2026-08-15.md
 * section 3.3): the GUI<->main-loop seam for provider selection.
 *
 * server_remote's own object (struct server_remote) is intentionally NOT
 * owned by any GUI screen -- screens never reach into main()'s locals, and
 * main() owns the ONLY struct server_local (see main.c). This module is the
 * single shared point both sides can reach:
 *   - the connecting screen (screen_multiplayer_connect.c) calls
 *     mp_session_request_connect() to ask for a connection, then polls
 *     mp_session_remote() + server_remote_get_status() for progress/errors;
 *   - main()'s loop drains the request once per frame via
 *     mp_session_take_pending_connect(), and is the ONLY place that performs
 *     the actual provider swap: server_local_shutdown() THEN
 *     server_remote_start() on mp_session_remote() -- the ordering plan 3.1
 *     requires ("provider selection happens before either server starts,
 *     never both") since server_local's thread otherwise runs continuously
 *     from boot, draining the very same svin mailbox server_remote_pump()
 *     must drain.
 *
 * PC-only, exactly like server_remote.c/.h: this whole translation unit
 * compiles to nothing under a non-PLATFORM_PC build (the Wii Makefile globs
 * every source/network C file, so this one IS compiled there too). Every
 * call site in main.c and screen_multiplayer_connect.c is itself wrapped in
 * #ifdef PLATFORM_PC, so no Wii reference to these symbols exists. */
#ifndef MP_SESSION_H
#define MP_SESSION_H

#include <stdbool.h>
#include <stdint.h>

#include "mp_codec.h"   /* MP_ADDRESS_HOST_CAPACITY */
#include "server_remote.h"

/* Matches struct server_remote_impl's own username[65] (mp_session.c);
 * server_remote_start would reject anything this can't hold anyway. */
#define MP_SESSION_USERNAME_CAPACITY 65

/* Idempotent, allocation-free handle initialization at boot. */
void mp_session_init(void);
/* Main-thread connect/teardown seam: allocate once, release after workers stop. */
bool mp_session_prepare_remote(void);
void mp_session_release_remote(void);
/* Idempotent: safe pre-init and safe to call twice. */
void mp_session_destroy(void);

/* The single remote-provider instance. Never NULL after mp_session_init();
 * callers use the ordinary server_remote_* API on it directly (status,
 * pump, cancel) rather than duplicating wrappers here. */
struct server_remote* mp_session_remote(void);

/* GUI-facing: record a connect ask for main() to service next frame. host
 * must already be a resolved address (see mp_address_parse) and username
 * must already pass mp_login_username_valid -- this function does not
 * re-validate either; it only guards against an overlong host. Only one
 * request may be outstanding; a second call before main() drains the first
 * replaces it (the connecting screen only ever calls this once per visit,
 * from reset()). */
bool mp_session_request_connect(const char* host, uint16_t port,
								const char* username_utf8);

/* GUI-facing: clear any queued-but-undrained request (the connect screen's
 * Cancel/Back). Without this a stale request could fire frames later and
 * tear down a live singleplayer world (2026-08-20 redesign, finding 3). */
void mp_session_cancel_pending(void);

/* main()-only: record that a consumed request was DROPPED (provider busy)
 * so the connect screen can report instead of spinning at its generation
 * baseline forever (re-review finding 3). One-shot. */
void mp_session_note_request_dropped(const char* why);

/* GUI-facing: consume the drop notice. Returns false when none pending. */
bool mp_session_take_drop_notice(char* reason_out, size_t reason_cap);

/* main()-only: drain a pending request. Returns false (leaving the outputs
 * untouched) when nothing is pending. */
bool mp_session_take_pending_connect(char* host_out, size_t host_cap,
									 uint16_t* port_out, char* user_out,
									 size_t user_cap);

/* Read-only: the Packet3 frames the remote provider has queued from
 * SRPC_CHAT_SEND, through the EXISTING production snapshot
 * (server_remote_get_status().chat_lines_sent -- the same call main.c already
 * makes once a frame). Wrapped here so a caller that only needs the counter
 * never has to hold a struct server_remote handle of its own; the scripted
 * mp_chat_login_3m profile reads it to turn "exactly one submission" into a
 * number instead of a claim.
 *
 * NOT const-qualified on purpose: the underlying getter takes the impl lock,
 * so this is a read that synchronizes, not a pure accessor. Flagged, not
 * papered over. Returns false when no remote provider exists (single-player,
 * pre-init, or a build without CAVEWII_MP_CLIENT), leaving `out` untouched. */
bool mp_session_chat_lines_sent(uint32_t* out);

#endif
