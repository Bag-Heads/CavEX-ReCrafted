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

/* Contract for the retained Play session (source/game/gui/play_session.h):
 * ONE frontend session shared by the Load / Create / Join tabs. Pure state --
 * no gfx, no input, no config, no filesystem. Drafts are built with the REAL
 * cworld_form_model functions, never with a hand-rolled copy of the form. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../source/config.h"
#include "../source/game/gui/cworld_form_model.h"
#include "../source/game/gui/play_session.h"

static void fill(char* dst, size_t len, char c) {
	memset(dst, c, len);
	dst[len] = '\0';
}

/* Fresh entry is the ONLY reset point, and it clears everything. */
static void test_fresh_initial_state(void) {
	play_session_enter_fresh();

	assert(play_session_tab() == PLAY_TAB_LOAD);
	assert(play_session_world_dir() == NULL);
	assert(play_session_server_name() != NULL);
	assert(play_session_server_name()[0] == '\0');
	assert(play_session_server_address() != NULL);
	assert(play_session_server_address()[0] == '\0');
	assert(play_session_server_index() == SIZE_MAX);

	struct cworld_form out;
	cworld_form_reset(&out);
	assert(!play_session_restore_draft(&out));
}

/* A second fresh entry wipes state a first session accumulated, and repeated
 * fresh entries stay safe (the owned world identity is freed once). */
static void test_second_fresh_clears(void) {
	play_session_enter_fresh();
	assert(play_session_set_tab(PLAY_TAB_JOIN));
	assert(play_session_remember_world("saves/Amplified Hills"));
	assert(play_session_remember_server("Home", "192.0.2.72:25565", 3));

	struct cworld_form f;
	cworld_form_reset(&f);
	play_session_store_draft(&f);

	play_session_enter_fresh();
	assert(play_session_tab() == PLAY_TAB_LOAD);
	assert(play_session_world_dir() == NULL);
	assert(play_session_server_name()[0] == '\0');
	assert(play_session_server_address()[0] == '\0');
	assert(play_session_server_index() == SIZE_MAX);
	assert(!play_session_restore_draft(&f));

	play_session_enter_fresh();
	play_session_enter_fresh();
	assert(play_session_tab() == PLAY_TAB_LOAD);
	assert(play_session_world_dir() == NULL);
}

/* The four pieces of state are independent: touching one never disturbs the
 * others -- that is the whole point of tab switching keeping its context. */
static void test_slots_are_independent(void) {
	play_session_enter_fresh();
	assert(play_session_remember_world("saves/World1"));
	assert(play_session_remember_server("LAN", "10.0.0.5:25565", 1));

	struct cworld_form f;
	cworld_form_reset(&f);
	snprintf(f.name, sizeof(f.name), "%s", "Draft World");
	play_session_store_draft(&f);

	assert(play_session_set_tab(PLAY_TAB_CREATE));
	assert(play_session_tab() == PLAY_TAB_CREATE);
	assert(strcmp(play_session_world_dir(), "saves/World1") == 0);
	assert(strcmp(play_session_server_name(), "LAN") == 0);

	assert(play_session_remember_world("saves/World2"));
	assert(strcmp(play_session_server_address(), "10.0.0.5:25565") == 0);
	assert(play_session_server_index() == 1);
	assert(play_session_tab() == PLAY_TAB_CREATE);

	assert(play_session_remember_server("Other", "10.0.0.6:25565", 2));
	assert(strcmp(play_session_world_dir(), "saves/World2") == 0);

	struct cworld_form out;
	cworld_form_reset(&out);
	assert(play_session_restore_draft(&out));
	assert(strcmp(out.name, "Draft World") == 0);
	assert(strcmp(play_session_world_dir(), "saves/World2") == 0);
	assert(play_session_server_index() == 2);
}

static void test_tab_round_trip_and_invalid_tab(void) {
	play_session_enter_fresh();
	assert(play_session_set_tab(PLAY_TAB_JOIN));
	assert(play_session_tab() == PLAY_TAB_JOIN);
	assert(play_session_set_tab(PLAY_TAB_LOAD));
	assert(play_session_tab() == PLAY_TAB_LOAD);
	assert(play_session_set_tab(PLAY_TAB_CREATE));

	/* Out-of-range values (a miswired screen constant) must not corrupt the
	 * selection. */
	assert(!play_session_set_tab((enum play_tab)3));
	assert(play_session_tab() == PLAY_TAB_CREATE);
	assert(!play_session_set_tab((enum play_tab)-1));
	assert(play_session_tab() == PLAY_TAB_CREATE);
	assert(!play_session_set_tab((enum play_tab)99));
	assert(play_session_tab() == PLAY_TAB_CREATE);
}

/* The native world directory is an unbounded string_t: a long path is kept in
 * full, never truncated to a fixed field. */
static void test_world_identity_is_unbounded(void) {
	char longdir[512];
	fill(longdir, 400, 'w');
	memcpy(longdir, "saves/", 6);

	play_session_enter_fresh();
	assert(play_session_remember_world(longdir));
	assert(play_session_world_dir() != NULL);
	assert(strlen(play_session_world_dir()) == 400);
	assert(strcmp(play_session_world_dir(), longdir) == 0);
}

/* Re-remembering the identity THROUGH the getter must not read freed memory:
 * the copy has to be made before the old buffer is released. */
static void test_world_identity_aliasing_is_safe(void) {
	play_session_enter_fresh();
	assert(play_session_remember_world("saves/Aliased World Name"));

	for(int i = 0; i < 4; i++) {
		const char* current = play_session_world_dir();
		assert(current != NULL);
		assert(play_session_remember_world(current));
		assert(strcmp(play_session_world_dir(), "saves/Aliased World Name")
			   == 0);
	}
}

static void test_world_identity_invalid_and_clear(void) {
	play_session_enter_fresh();
	assert(play_session_remember_world("saves/Keep Me"));

	/* NULL is refused and the prior identity survives. */
	assert(!play_session_remember_world(NULL));
	assert(play_session_world_dir() != NULL);
	assert(strcmp(play_session_world_dir(), "saves/Keep Me") == 0);

	/* Empty clears (a deleted world leaves no selection). */
	assert(play_session_remember_world(""));
	assert(play_session_world_dir() == NULL);

	/* Clearing twice, and clearing when already clear, stay safe. */
	assert(play_session_remember_world(""));
	assert(play_session_world_dir() == NULL);
	assert(!play_session_remember_world(NULL));
	assert(play_session_world_dir() == NULL);
}

/* Caps are the native config ones, and validation happens BEFORE any field is
 * written -- a rejected call leaves name, address AND index untouched. */
static void test_server_caps_are_native_and_validated(void) {
	char name_ok[CONFIG_MP_SERVER_NAME_CAP + 8];
	char name_over[CONFIG_MP_SERVER_NAME_CAP + 8];
	char addr_ok[CONFIG_MP_SERVER_ADDRESS_CAP + 8];
	char addr_over[CONFIG_MP_SERVER_ADDRESS_CAP + 8];

	fill(name_ok, CONFIG_MP_SERVER_NAME_CAP - 1, 'n');
	fill(name_over, CONFIG_MP_SERVER_NAME_CAP, 'n');
	fill(addr_ok, CONFIG_MP_SERVER_ADDRESS_CAP - 1, 'a');
	fill(addr_over, CONFIG_MP_SERVER_ADDRESS_CAP, 'a');

	play_session_enter_fresh();
	assert(play_session_remember_server("Prior", "1.2.3.4:25565", 0));

	assert(!play_session_remember_server(NULL, "1.2.3.4:25565", 0));
	assert(!play_session_remember_server("Nameless", NULL, 0));
	assert(!play_session_remember_server(name_over, "1.2.3.4:25565", 0));
	assert(!play_session_remember_server("Fits", addr_over, 0));
	assert(!play_session_remember_server("Fits", "1.2.3.4:25565",
										 CONFIG_MP_SERVER_LIST_MAX));

	assert(strcmp(play_session_server_name(), "Prior") == 0);
	assert(strcmp(play_session_server_address(), "1.2.3.4:25565") == 0);
	assert(play_session_server_index() == 0);

	/* Exactly-at-cap strings and the last valid slot are accepted in full. */
	assert(play_session_remember_server(name_ok, addr_ok,
										CONFIG_MP_SERVER_LIST_MAX - 1));
	assert(strcmp(play_session_server_name(), name_ok) == 0);
	assert(strlen(play_session_server_name())
		   == (size_t)(CONFIG_MP_SERVER_NAME_CAP - 1));
	assert(strcmp(play_session_server_address(), addr_ok) == 0);
	assert(strlen(play_session_server_address())
		   == (size_t)(CONFIG_MP_SERVER_ADDRESS_CAP - 1));
	assert(play_session_server_index() == CONFIG_MP_SERVER_LIST_MAX - 1);

	/* SIZE_MAX is the explicit "no index hint". */
	assert(play_session_remember_server("Hintless", "5.6.7.8:25565",
										SIZE_MAX));
	assert(play_session_server_index() == SIZE_MAX);

	/* Two empty strings clear the remembered server, index included. */
	assert(play_session_remember_server("", "", SIZE_MAX));
	assert(play_session_server_name()[0] == '\0');
	assert(play_session_server_address()[0] == '\0');
	assert(play_session_server_index() == SIZE_MAX);
}

/* Saved servers may share an address; name + index disambiguate them. */
static void test_server_duplicate_addresses(void) {
	play_session_enter_fresh();
	assert(play_session_remember_server("Creative", "play.example.net", 2));
	assert(strcmp(play_session_server_name(), "Creative") == 0);
	assert(play_session_server_index() == 2);

	assert(play_session_remember_server("Survival", "play.example.net", 5));
	assert(strcmp(play_session_server_name(), "Survival") == 0);
	assert(strcmp(play_session_server_address(), "play.example.net") == 0);
	assert(play_session_server_index() == 5);

	/* Same name, same address, moved row: the hint follows the row. */
	assert(play_session_remember_server("Survival", "play.example.net", 0));
	assert(play_session_server_index() == 0);
}

/* The draft is stored BY VALUE with the raw seed text as typed -- "0", a
 * negative number and free text are all distinct inputs to Beta's seed
 * parse, so none of them may be normalised here. */
static void test_draft_keeps_raw_seed_mode_and_focus(void) {
	static const char* seeds[] = {"0", "-42", "glacier", ""};

	for(size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
		play_session_enter_fresh();

		struct cworld_form f;
		cworld_form_reset(&f);
		snprintf(f.name, sizeof(f.name), "%s", "Seeded");
		snprintf(f.seed, sizeof(f.seed), "%s", seeds[i]);
		cworld_form_set_focus(&f, CWORLD_ROW_SEED);
		assert(f.focus == CWORLD_ROW_SEED);

		play_session_store_draft(&f);

		struct cworld_form out;
		cworld_form_reset(&out);
		assert(play_session_restore_draft(&out));
		assert(strcmp(out.name, "Seeded") == 0);
		assert(strcmp(out.seed, seeds[i]) == 0);
		assert(out.focus == CWORLD_ROW_SEED);
		assert(out.mode == f.mode);
	}
}

static void test_draft_keeps_creative_mode(void) {
	play_session_enter_fresh();

	struct cworld_form f;
	cworld_form_reset(&f);
	assert(f.mode == CWORLD_MODE_SURVIVAL);
	cworld_form_set_focus(&f, CWORLD_ROW_MODE);
	assert(cworld_form_cycle_mode(&f, 1));
	assert(f.mode == CWORLD_MODE_CREATIVE);

	play_session_store_draft(&f);

	struct cworld_form out;
	cworld_form_reset(&out);
	assert(play_session_restore_draft(&out));
	assert(out.mode == CWORLD_MODE_CREATIVE);
	assert(out.focus == CWORLD_ROW_MODE);
}

/* A defensive store while the OSK modal is open reverts the editor ON THE
 * COPY only: the caller's live form is not mutated behind the screen's back. */
static void test_store_reverts_editor_on_the_copy_only(void) {
	play_session_enter_fresh();

	struct cworld_form f;
	cworld_form_reset(&f);
	snprintf(f.name, sizeof(f.name), "%s", "Committed");
	cworld_form_set_focus(&f, CWORLD_ROW_NAME);
	assert(cworld_form_open_editor(&f));
	snprintf(f.name, sizeof(f.name), "%s", "Half Typed");
	assert(f.editing);

	play_session_store_draft(&f);

	/* Caller unchanged: still editing, still holding the typed text. */
	assert(f.editing);
	assert(strcmp(f.name, "Half Typed") == 0);

	/* Stored copy: editor closed, pre-edit text restored. */
	struct cworld_form out;
	cworld_form_reset(&out);
	assert(play_session_restore_draft(&out));
	assert(!out.editing);
	assert(strcmp(out.name, "Committed") == 0);

	/* And the same form stored with the editor already closed round-trips
	 * untouched. */
	cworld_form_revert_editor(&f);
	assert(!f.editing);
	play_session_store_draft(&f);
	assert(play_session_restore_draft(&out));
	assert(strcmp(out.name, "Committed") == 0);
	assert(!out.editing);
}

static void test_draft_null_inputs_and_clear(void) {
	play_session_enter_fresh();

	struct cworld_form f;
	cworld_form_reset(&f);
	snprintf(f.name, sizeof(f.name), "%s", "Kept");

	/* A NULL store does nothing -- it must not clear an existing draft. */
	play_session_store_draft(&f);
	play_session_store_draft(NULL);

	struct cworld_form out;
	cworld_form_reset(&out);
	assert(play_session_restore_draft(&out));
	assert(strcmp(out.name, "Kept") == 0);

	/* A NULL restore target is refused, draft intact. */
	assert(!play_session_restore_draft(NULL));
	assert(play_session_restore_draft(&out));
	assert(strcmp(out.name, "Kept") == 0);

	/* clear_draft drops the draft ONLY. */
	assert(play_session_set_tab(PLAY_TAB_CREATE));
	assert(play_session_remember_world("saves/Survivor"));
	assert(play_session_remember_server("LAN", "10.0.0.5:25565", 4));

	play_session_clear_draft();
	assert(!play_session_restore_draft(&out));
	assert(play_session_tab() == PLAY_TAB_CREATE);
	assert(strcmp(play_session_world_dir(), "saves/Survivor") == 0);
	assert(strcmp(play_session_server_name(), "LAN") == 0);
	assert(play_session_server_index() == 4);

	/* Clearing an already-empty draft stays safe. */
	play_session_clear_draft();
	assert(!play_session_restore_draft(&out));
}

int main(void) {
	test_fresh_initial_state();
	test_second_fresh_clears();
	test_slots_are_independent();
	test_tab_round_trip_and_invalid_tab();
	test_world_identity_is_unbounded();
	test_world_identity_aliasing_is_safe();
	test_world_identity_invalid_and_clear();
	test_server_caps_are_native_and_validated();
	test_server_duplicate_addresses();
	test_draft_keeps_raw_seed_mode_and_focus();
	test_draft_keeps_creative_mode();
	test_store_reverts_editor_on_the_copy_only();
	test_draft_null_inputs_and_clear();

	printf("test_play_session: PASS\n");
	return 0;
}
