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

#ifndef CWORLD_FORM_MODEL_H
#define CWORLD_FORM_MODEL_H

#include <stdbool.h>
#include <stddef.h>

/* Create-world FORM state machine -- the console-edition navigation model of
 * docs/ui_design/create_world_console_2026-09-01.md sections 3-4, factored out of
 * screen_create_world.c so it is testable with no gfx, no input and no
 * filesystem (test_cworld_form). The screen owns pixels and buttons; this
 * owns WHAT IS FOCUSED, WHAT IS SELECTED and WHETHER THE OSK MODAL IS OPEN.
 *
 * What is Beta and what is the carve-out:
 *   BETA [740c583 GuiCreateWorld]: the name field defaults to "New World"
 *     with maxStringLength 32 and starts focused; the seed field starts
 *     empty and unbounded; Create.enabled is `name.length() > 0`, recomputed
 *     after every keystroke.
 *   CARVE-OUT #2 (Wii controller playability, docs/wii_controller_playability.md):
 *     Beta's Tab-toggles-two-fields + mouse-click focus becomes a five-row
 *     FOCUS RING driven by D-pad Up/Down, because a Wii has neither Tab nor a
 *     mouse; text entry is a MODAL OSK instead of a keyboard.
 *   NET-NEW (creative exception, docs/creative_mode.md): the Game Mode row.
 *     Beta 1.7.3 has no game mode at all. */

enum cworld_row {
	CWORLD_ROW_NAME = 0,
	CWORLD_ROW_MODE,
	CWORLD_ROW_SEED,
	CWORLD_ROW_CREATE,
	CWORLD_ROW_CANCEL,
	CWORLD_ROW_COUNT
};

enum cworld_game_mode {
	CWORLD_MODE_SURVIVAL = 0, /* ALWAYS the default (maintainer, 2026-09-01) */
	CWORLD_MODE_CREATIVE,
	CWORLD_MODE_COUNT
};

/* [740c583 GuiCreateWorld.initGui] setMaxStringLength(32). */
#define CWORLD_NAME_MAX 32
#define CWORLD_NAME_CAP (CWORLD_NAME_MAX + 4)
/* Beta leaves the seed field unbounded; a 63-char cap is the documented
 * micro-deviation (no dynamic buffer on a 24MB MEM1 budget). Every string
 * that parses as a long is 20 chars, and longer text only ever reaches
 * String.hashCode. */
#define CWORLD_SEED_CAP 64

struct cworld_form {
	char name[CWORLD_NAME_CAP];
	char seed[CWORLD_SEED_CAP];
	/* Pre-edit snapshot for the modal's B/revert. Sized to the LARGER of the
	 * two buffers so one array serves both fields. */
	char revert[CWORLD_SEED_CAP];
	int focus; /* enum cworld_row */
	int mode;  /* enum cworld_game_mode */
	bool editing;
	int edit_row; /* the text row the open modal is editing */
};

/* [740c583 GuiCreateWorld.initGui]: "New World", empty seed, NAME focused.
 * Survival, modal closed. */
void cworld_form_reset(struct cworld_form* f);

/* Beta's enable rule, derived rather than cached so it can never desync:
 * `Create.enabled = name.length() > 0` [740c583 GuiCreateWorld.keyTyped]. */
bool cworld_form_create_enabled(const struct cworld_form* f);

bool cworld_form_row_is_text(int row);

/* Focus ring, `dir` = -1 up / +1 down. Wraps, and SKIPS a disabled Create
 * (design section 4 rule 4). No-op while the modal is open -- the OSK owns the
 * d-pad then. */
void cworld_form_move_focus(struct cworld_form* f, int dir);

/* Point-to-focus (IR pointer / stick cursor): pointer and d-pad always agree
 * on one focus. Refuses a disabled Create, and is a no-op while editing. */
void cworld_form_set_focus(struct cworld_form* f, int row);

/* Game Mode selector, `dir` = -1 left / +1 right; wraps. Only acts when the
 * Game Mode row is focused and the modal is closed. Returns true on change. */
bool cworld_form_cycle_mode(struct cworld_form* f, int dir);

const char* cworld_form_mode_label(int mode);

/* --- modal OSK (design section 4 rule 1, maintainer decision 3) ------------ */

/* Open the editor on the focused text row: snapshots the buffer for revert.
 * Returns false (and changes nothing) on a non-text row or when already
 * open. */
bool cworld_form_open_editor(struct cworld_form* f);
/* Keep the typed text and close. */
void cworld_form_commit_editor(struct cworld_form* f);
/* Restore the pre-edit text and close (B while the modal is open). */
void cworld_form_revert_editor(struct cworld_form* f);

/* The buffer the OSK binds to while the modal is open (NULL when closed). */
char* cworld_form_edit_buffer(struct cworld_form* f);
size_t cworld_form_edit_cap(const struct cworld_form* f);
/* Beta setMaxStringLength for the edited field: 32 for the name, 0
 * (unbounded, to the buffer) for the seed. */
int cworld_form_edit_max(const struct cworld_form* f);
/* OSK_PAGE_* the editor opens on: the numeric seed page for the seed row
 * (design section 3.2), lower-case letters for the name. Returned as a plain int so
 * this header stays free of widget_osk.h. */
int cworld_form_edit_page(const struct cworld_form* f);

#endif
