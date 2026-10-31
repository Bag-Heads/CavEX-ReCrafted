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

/* Retained state for the ONE Play frontend session -- see play_session.h for
 * the contract. Pure state: no gfx, no input, no config read, no filesystem,
 * no simulation. The screens stay authoritative for enumeration, the ping
 * sweep and focus; this module only remembers identity across a screen_set. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../config.h"
#include "play_session.h"

static struct {
	enum play_tab tab;
	char* world_dir; /* owned; NULL when no world is remembered */
	char server_name[CONFIG_MP_SERVER_NAME_CAP];
	char server_address[CONFIG_MP_SERVER_ADDRESS_CAP];
	size_t server_index;
	struct cworld_form draft;
	bool has_draft;
} session = {
	.tab = PLAY_TAB_LOAD,
	.world_dir = NULL,
	.server_index = SIZE_MAX,
	.has_draft = false,
};

void play_session_enter_fresh(void) {
	free(session.world_dir);
	session.world_dir = NULL;
	session.tab = PLAY_TAB_LOAD;
	session.server_name[0] = '\0';
	session.server_address[0] = '\0';
	session.server_index = SIZE_MAX;
	memset(&session.draft, 0, sizeof(session.draft));
	session.has_draft = false;
}

enum play_tab play_session_tab(void) {
	return session.tab;
}

bool play_session_set_tab(enum play_tab tab) {
	int t = (int)tab;

	if(t < (int)PLAY_TAB_LOAD || t > (int)PLAY_TAB_JOIN)
		return false;

	session.tab = tab;
	return true;
}

bool play_session_remember_world(const char* directory) {
	char* copy;
	size_t len;

	if(!directory)
		return false;

	if(!directory[0]) {
		free(session.world_dir);
		session.world_dir = NULL;
		return true;
	}

	/* Copy BEFORE the old buffer is released: `directory` may alias it. */
	len = strlen(directory);
	copy = malloc(len + 1);

	if(!copy)
		return false;

	memcpy(copy, directory, len + 1);
	free(session.world_dir);
	session.world_dir = copy;
	return true;
}

const char* play_session_world_dir(void) {
	return session.world_dir;
}

bool play_session_remember_server(const char* name, const char* address,
								  size_t index) {
	char tmp_name[CONFIG_MP_SERVER_NAME_CAP];
	char tmp_address[CONFIG_MP_SERVER_ADDRESS_CAP];

	/* Validate EVERYTHING before touching a single field. */
	if(!name || !address)
		return false;

	if(strlen(name) >= sizeof(tmp_name)
	   || strlen(address) >= sizeof(tmp_address))
		return false;

	if(index != SIZE_MAX && index >= CONFIG_MP_SERVER_LIST_MAX)
		return false;

	/* Through the temporaries, because the arguments may alias the getters. */
	memcpy(tmp_name, name, strlen(name) + 1);
	memcpy(tmp_address, address, strlen(address) + 1);

	if(!tmp_name[0] && !tmp_address[0]) {
		session.server_name[0] = '\0';
		session.server_address[0] = '\0';
		session.server_index = SIZE_MAX;
		return true;
	}

	memcpy(session.server_name, tmp_name, strlen(tmp_name) + 1);
	memcpy(session.server_address, tmp_address, strlen(tmp_address) + 1);
	session.server_index = index;
	return true;
}

const char* play_session_server_name(void) {
	return session.server_name;
}

const char* play_session_server_address(void) {
	return session.server_address;
}

size_t play_session_server_index(void) {
	return session.server_index;
}

void play_session_store_draft(const struct cworld_form* form) {
	struct cworld_form copy;

	if(!form)
		return;

	copy = *form;

	/* Defensive revert on the STORED COPY only -- the caller's live form must
	 * not be mutated behind the screen's back. */
	if(copy.editing)
		cworld_form_revert_editor(&copy);

	session.draft = copy;
	session.has_draft = true;
}

bool play_session_restore_draft(struct cworld_form* out) {
	if(!out || !session.has_draft)
		return false;

	*out = session.draft;
	return true;
}

void play_session_clear_draft(void) {
	memset(&session.draft, 0, sizeof(session.draft));
	session.has_draft = false;
}
