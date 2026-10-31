/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

/* Create-world CONSOLE form state machine
 * (docs/ui_design/create_world_console_2026-09-01.md sections 3-4, section 7 item 5).
 *
 * There is no headless screen harness in tests/ -- every existing screen test
 * (test_osk_env, test_slotlist_env, test_sign_set_env, test_mp_setup_screen)
 * drives a PURE model factored out of the screen rather than the screen's
 * gfx/input hooks, so the focus/selector/modal logic was factored into
 * cworld_form_model.c the same way and is driven directly here.
 *
 * Every expectation is hand-derived from the design doc and the decompile,
 * never from running the unit (the poisoned-golden rule, CODESTYLE section 4.2):
 *   - focus ring order + wrap        design section 4 ("Name -> Game Mode -> Seed
 *                                    -> Create -> Cancel", wraps)
 *   - disabled-Create skipping       design section 4 rule 4 + [740c583
 *                                    GuiCreateWorld.keyTyped] enable rule
 *   - Survival always the default    maintainer decision 1, 2026-09-01
 *   - modal open/commit/revert       design section 4 rule 1-2, decision 3
 *   - numeric-first seed page        design section 3.2, decision 4
 *   - seed vectors for OSK-typed text  [740c583 GuiCreateWorld.actionPerformed] */

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../source/game/gui/cworld_form_model.h"
#include "../source/game/gui/screen.h"
#include "../source/game/gui/widget_osk.h"

/* ---- F1: initGui defaults ---------------------------------------------- */

static void test_defaults(void) {
	struct cworld_form f;
	cworld_form_reset(&f);

	/* [740c583 GuiCreateWorld.initGui]: "New World", empty seed, name
	 * FOCUSED; Create enabled because the default name is non-empty. */
	assert(strcmp(f.name, "New World") == 0);
	assert(f.seed[0] == 0);
	assert(f.focus == CWORLD_ROW_NAME);
	assert(cworld_form_create_enabled(&f));
	/* Maintainer decision 1 (2026-09-01): Survival is ALWAYS the default --
	 * there is no config that can preselect Creative. */
	assert(f.mode == CWORLD_MODE_SURVIVAL);
	assert(strcmp(cworld_form_mode_label(f.mode), "Survival") == 0);
	assert(!f.editing);

	fprintf(stderr, "F1 defaults OK\n");
}

/* ---- F2: focus ring order + wrap --------------------------------------- */

static void test_focus_ring(void) {
	struct cworld_form f;
	cworld_form_reset(&f);

	static const int down_order[CWORLD_ROW_COUNT] = {
		CWORLD_ROW_MODE, CWORLD_ROW_SEED, CWORLD_ROW_CREATE,
		CWORLD_ROW_CANCEL, CWORLD_ROW_NAME /* wrap */
	};
	for(int k = 0; k < CWORLD_ROW_COUNT; k++) {
		cworld_form_move_focus(&f, 1);
		assert(f.focus == down_order[k]);
	}

	/* Up is the exact reverse, and wraps off Name to Cancel. */
	static const int up_order[CWORLD_ROW_COUNT] = {
		CWORLD_ROW_CANCEL /* wrap */, CWORLD_ROW_CREATE, CWORLD_ROW_SEED,
		CWORLD_ROW_MODE, CWORLD_ROW_NAME
	};
	for(int k = 0; k < CWORLD_ROW_COUNT; k++) {
		cworld_form_move_focus(&f, -1);
		assert(f.focus == up_order[k]);
	}

	/* Only Name and Seed open an editor. */
	assert(cworld_form_row_is_text(CWORLD_ROW_NAME));
	assert(cworld_form_row_is_text(CWORLD_ROW_SEED));
	assert(!cworld_form_row_is_text(CWORLD_ROW_MODE));
	assert(!cworld_form_row_is_text(CWORLD_ROW_CREATE));
	assert(!cworld_form_row_is_text(CWORLD_ROW_CANCEL));

	fprintf(stderr, "F2 focus ring OK\n");
}

/* ---- F3: disabled Create is skipped ------------------------------------ */

static void test_disabled_create_skip(void) {
	struct cworld_form f;
	cworld_form_reset(&f);

	/* Beta's rule is on the RAW name, not the trimmed one
	 * [740c583 GuiCreateWorld.keyTyped: `name.getText().length() > 0`]:
	 * a name of one space still enables Create (it derives folder "World"). */
	f.name[0] = ' ';
	f.name[1] = 0;
	assert(cworld_form_create_enabled(&f));

	f.name[0] = 0; /* empty -> disabled */
	assert(!cworld_form_create_enabled(&f));

	/* Down from Seed jumps straight to Cancel. */
	f.focus = CWORLD_ROW_SEED;
	cworld_form_move_focus(&f, 1);
	assert(f.focus == CWORLD_ROW_CANCEL);
	/* Up from Cancel jumps straight back to Seed. */
	cworld_form_move_focus(&f, -1);
	assert(f.focus == CWORLD_ROW_SEED);
	/* Wrapping down off Cancel reaches Name, never the disabled Create. */
	f.focus = CWORLD_ROW_CANCEL;
	cworld_form_move_focus(&f, 1);
	assert(f.focus == CWORLD_ROW_NAME);

	/* The pointer cannot land on it either. */
	cworld_form_set_focus(&f, CWORLD_ROW_CREATE);
	assert(f.focus == CWORLD_ROW_NAME);
	cworld_form_set_focus(&f, CWORLD_ROW_CANCEL);
	assert(f.focus == CWORLD_ROW_CANCEL);

	/* Re-enable: the row is reachable again with no other state change. */
	memcpy(f.name, "W", 2);
	cworld_form_set_focus(&f, CWORLD_ROW_CREATE);
	assert(f.focus == CWORLD_ROW_CREATE);

	fprintf(stderr, "F3 disabled-Create skipping OK\n");
}

/* ---- F4: game-mode selector -------------------------------------------- */

static void test_mode_selector(void) {
	struct cworld_form f;
	cworld_form_reset(&f);

	/* Left/Right only act on the Game Mode row. */
	f.focus = CWORLD_ROW_NAME;
	assert(!cworld_form_cycle_mode(&f, 1));
	assert(f.mode == CWORLD_MODE_SURVIVAL);

	f.focus = CWORLD_ROW_MODE;
	assert(cworld_form_cycle_mode(&f, 1));
	assert(f.mode == CWORLD_MODE_CREATIVE);
	assert(strcmp(cworld_form_mode_label(f.mode), "Creative") == 0);

	/* Two values, so it wraps in both directions. */
	assert(cworld_form_cycle_mode(&f, 1));
	assert(f.mode == CWORLD_MODE_SURVIVAL);
	assert(cworld_form_cycle_mode(&f, -1));
	assert(f.mode == CWORLD_MODE_CREATIVE);
	assert(cworld_form_cycle_mode(&f, -1));
	assert(f.mode == CWORLD_MODE_SURVIVAL);

	/* The modal owns Left/Right while it is open. */
	f.focus = CWORLD_ROW_SEED;
	assert(cworld_form_open_editor(&f));
	f.focus = CWORLD_ROW_MODE; /* even if focus were forced there */
	assert(!cworld_form_cycle_mode(&f, 1));
	assert(f.mode == CWORLD_MODE_SURVIVAL);

	fprintf(stderr, "F4 mode selector OK\n");
}

/* ---- F5: modal OSK open / commit / revert ------------------------------ */

static void test_modal(void) {
	struct cworld_form f;
	cworld_form_reset(&f);

	/* A non-text row never opens an editor. */
	f.focus = CWORLD_ROW_MODE;
	assert(!cworld_form_open_editor(&f));
	assert(!f.editing);
	f.focus = CWORLD_ROW_CREATE;
	assert(!cworld_form_open_editor(&f));
	f.focus = CWORLD_ROW_CANCEL;
	assert(!cworld_form_open_editor(&f));

	/* NAME: bound to the name buffer, Beta's max 32, letters page. */
	f.focus = CWORLD_ROW_NAME;
	assert(cworld_form_open_editor(&f));
	assert(f.editing && f.edit_row == CWORLD_ROW_NAME);
	assert(cworld_form_edit_buffer(&f) == f.name);
	assert(cworld_form_edit_cap(&f) == sizeof(f.name));
	assert(cworld_form_edit_max(&f) == CWORLD_NAME_MAX);
	assert(cworld_form_edit_page(&f) == OSK_PAGE_LOWER);
	/* Reopening is refused (one modal). */
	assert(!cworld_form_open_editor(&f));
	/* The d-pad belongs to the OSK while the modal is up. */
	assert(f.focus == CWORLD_ROW_NAME);
	cworld_form_move_focus(&f, 1);
	assert(f.focus == CWORLD_ROW_NAME);

	/* REVERT restores the pre-edit text exactly (design section 4: B while the
	 * modal is open closes it and throws the edit away). */
	memcpy(f.name, "Glacier", sizeof("Glacier"));
	cworld_form_revert_editor(&f);
	assert(!f.editing);
	assert(strcmp(f.name, "New World") == 0);
	assert(f.focus == CWORLD_ROW_NAME);
	assert(cworld_form_edit_buffer(&f) == NULL);

	/* COMMIT keeps it. */
	assert(cworld_form_open_editor(&f));
	memcpy(f.name, "Glacier", sizeof("Glacier"));
	cworld_form_commit_editor(&f);
	assert(!f.editing);
	assert(strcmp(f.name, "Glacier") == 0);

	/* Reverting an EMPTIED name restores Create-enabled state too -- the
	 * enable rule is derived from the buffer, so it can never desync. */
	assert(cworld_form_open_editor(&f));
	f.name[0] = 0;
	assert(!cworld_form_create_enabled(&f));
	cworld_form_revert_editor(&f);
	assert(cworld_form_create_enabled(&f));
	assert(strcmp(f.name, "Glacier") == 0);

	/* SEED: bound to the seed buffer, UNBOUNDED (Beta maxStringLength 0),
	 * and opens on the NUMERIC page (design section 3.2 / decision 4). */
	f.focus = CWORLD_ROW_SEED;
	assert(cworld_form_open_editor(&f));
	assert(cworld_form_edit_buffer(&f) == f.seed);
	assert(cworld_form_edit_cap(&f) == sizeof(f.seed));
	assert(cworld_form_edit_max(&f) == 0);
	assert(cworld_form_edit_page(&f) == OSK_PAGE_NUM);
	memcpy(f.seed, "12345", sizeof("12345"));
	cworld_form_commit_editor(&f);
	assert(strcmp(f.seed, "12345") == 0);
	assert(f.focus == CWORLD_ROW_SEED); /* focus returns to the edited row */
	/* Editing the seed never touches the name. */
	assert(strcmp(f.name, "Glacier") == 0);

	fprintf(stderr, "F5 modal open/commit/revert OK\n");
}

/* ---- F6: the numeric seed page ----------------------------------------- */

static void test_numeric_page(void) {
	/* Every character on OSK_PAGE_NUM is a digit or '-' -- exactly Java 6
	 * Long.parseLong's alphabet [740c583 GuiCreateWorld.actionPerformed].
	 * A stray letter here would silently push a typed seed onto the
	 * hashCode path. */
	bool seen_minus = false;
	int digits = 0;
	for(int r = 0; r < OSK_ROWS; r++) {
		for(int c = 0; c < OSK_COLS; c++) {
			char ch = osk_page_char(OSK_PAGE_NUM, r, c);
			if(!ch)
				continue;
			assert((ch >= '0' && ch <= '9') || ch == '-');
			if(ch == '-')
				seen_minus = true;
			else
				digits++;
		}
	}
	assert(seen_minus);
	assert(digits == 10); /* 0-9, each exactly once */

	/* Opening on the numeric page parks the cursor on a SELECTABLE cell --
	 * the sparse page has blanks where the letter pages have letters. */
	char buf[CWORLD_SEED_CAP];
	buf[0] = 0;
	struct osk_state o;
	osk_reset_page(&o, buf, sizeof(buf), 0, OSK_PAGE_NUM);
	assert(o.page == OSK_PAGE_NUM);
	assert(osk_page_char(o.page, o.row, o.col) != 0);

	/* osk_reset is still exactly the lower-case page. */
	osk_reset(&o, buf, sizeof(buf), 0);
	assert(o.page == OSK_PAGE_LOWER);

	fprintf(stderr, "F6 numeric seed page OK\n");
}

/* ---- F7: seed vectors for an OSK-typed TEXT seed ------------------------ */

static void expect_seed(const char* text, bool want_set, int64_t want) {
	int64_t out = 0;
	bool set = screen_cworld_test_resolve_seed(text, &out);
	if(set != want_set || (set && out != want)) {
		fprintf(stderr,
				"resolve_seed('%s') = (%d, %" PRId64 "), want (%d, %" PRId64
				")\n",
				text, (int)set, out, (int)want_set, want);
		assert(0);
	}
}

static void test_osk_typed_seed(void) {
	/* A seed typed on the OSK's LETTER pages cannot parse as a long, so it
	 * takes Beta's hashCode branch. Hand-computed Java String.hashCode
	 * (h = 31*h + c, int32 wrap, sign-extended by the (long) cast):
	 *   "seed"   = ((115*31+101)*31+101)*31+100 = 3526257
	 *              115*31=3565, +101=3666; 3666*31=113646, +101=113747;
	 *              113747*31=3526157, +100=3526257
	 *   "-"      is on the NUMERIC page but is not a valid long on its own
	 *            (parseLong needs >= 1 digit after the sign) -> hashCode 45
	 *   "1-2"    does not parse ('-' mid-string) -> hashCode
	 *              49*31=1519, +45=1564; 1564*31=48484, +50=48534
	 * The numeric page's own alphabet still resolves through parseLong. */
	expect_seed("seed", true, 3526257);
	expect_seed("-", true, 45);
	expect_seed("1-2", true, 48534);

	/* The characters the numeric page CAN type, through the parseLong branch
	 * [740c583 GuiCreateWorld.actionPerformed]: 0 keeps the random seed. */
	expect_seed("12345", true, 12345);
	expect_seed("-12345", true, -12345);
	expect_seed("0", false, 0);
	expect_seed("", false, 0);

	fprintf(stderr, "F7 OSK-typed seed vectors OK\n");
}

int main(void) {
	test_defaults();
	test_focus_ring();
	test_disabled_create_skip();
	test_mode_selector();
	test_modal();
	test_numeric_page();
	test_osk_typed_seed();
	fprintf(stderr, "test_cworld_form: all OK\n");
	return 0;
}
