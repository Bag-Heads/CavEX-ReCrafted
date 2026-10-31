/*
	Copyright (c) 2026 CaveWii contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef PLAY_SESSION_H
#define PLAY_SESSION_H

#include <stdbool.h>
#include <stddef.h>

#include "cworld_form_model.h"

/* Retained state for the ONE Play frontend session (Load / Create / Join),
 * factored out of the three screens so it is testable with no gfx, no input,
 * no config I/O and no filesystem (test_play_session).
 *
 * Why it exists: every tab switch is a `screen_set`, and each screen's
 * `reset()` re-enumerates storage and zeroes its selection. The screens KEEP
 * doing that -- enumeration, the ping sweep, the config read and focus
 * arbitration stay authoritative there -- and then re-seat themselves from
 * this module by STABLE IDENTITY, never by index.
 *
 * Scope (Astra, GATE3-PLAY-PLAN.md): pure state only. This module never reads
 * config, never touches the filesystem, never runs simulation, and never
 * allocates on behalf of a caller beyond its own world-identity copy. It is
 * a single process-wide session, because there is exactly one frontend. */

enum play_tab {
	PLAY_TAB_LOAD = 0,
	PLAY_TAB_CREATE,
	PLAY_TAB_JOIN,
};

/* The only reset point: title "Play" activation. Frees the owned world
 * identity, clears the remembered server (index back to SIZE_MAX) and the
 * draft, and selects PLAY_TAB_LOAD. Safe to call repeatedly. */
void play_session_enter_fresh(void);

enum play_tab play_session_tab(void);
/* Refuses a value outside the enum and leaves the prior tab intact. */
bool play_session_set_tab(enum play_tab tab);

/* --- world identity (Load tab) -------------------------------------------- */

/* The native world directory is an unbounded string_t, so the identity is an
 * owned heap copy and is NEVER truncated. The copy is made BEFORE the old
 * buffer is released, so passing play_session_world_dir() straight back in is
 * safe (aliasing). NULL, or a failed allocation, retains the previous identity
 * and returns false. An empty string clears the identity and returns true. */
bool play_session_remember_world(const char* directory);
/* NULL when no world is remembered. Owned by the module; valid until the next
 * remember/fresh call. */
const char* play_session_world_dir(void);

/* --- server identity (Join tab) ------------------------------------------- */

/* Saved servers may share an address, so the identity is name + address, with
 * the list index only as a disambiguating HINT for the restore. Caps are the
 * native config ones (CONFIG_MP_SERVER_NAME_CAP / _ADDRESS_CAP, including the
 * terminator); `index` must be a valid CONFIG_MP_SERVER_LIST_MAX slot or
 * SIZE_MAX for "no hint". Every argument is validated BEFORE any field is
 * mutated: a rejected call leaves the whole remembered server unchanged and
 * returns false. NULL name or address is rejected. Two empty strings clear the
 * remembered server (index back to SIZE_MAX) and return true.
 * No new persistent server IDs, no config schema change. */
bool play_session_remember_server(const char* name, const char* address,
								  size_t index);
/* Empty strings (never NULL) when no server is remembered. */
const char* play_session_server_name(void);
const char* play_session_server_address(void);
/* SIZE_MAX when there is no index hint. */
size_t play_session_server_index(void);

/* --- create-world draft (Create tab) -------------------------------------- */

/* Stores a BY-VALUE copy of the form -- raw seed text, focus row and game mode
 * included, exactly as typed. If the caller's form has its modal editor open,
 * only the STORED COPY is defensively reverted (cworld_form_revert_editor on
 * the copy); the caller's form is left untouched, because a modal action must
 * not silently mutate the live screen. A NULL form does nothing. */
void play_session_store_draft(const struct cworld_form* form);
/* Copies the stored draft into `out` and returns true. Returns false and
 * leaves `out` untouched when no draft is stored or `out` is NULL. */
bool play_session_restore_draft(struct cworld_form* out);
/* Drops the draft ONLY; tab, world identity and server identity survive. */
void play_session_clear_draft(void);

#endif
