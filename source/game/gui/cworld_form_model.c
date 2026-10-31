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

#include <assert.h>
#include <string.h>

#include "cworld_form_model.h"
#include "widget_osk.h"

void cworld_form_reset(struct cworld_form* f) {
	assert(f);
	memset(f, 0, sizeof(*f));
	/* [740c583 GuiCreateWorld.initGui]: StatCollector.translateToLocal
	 * "selectWorld.newWorld" == "New World", seed empty, name field focused. */
	memcpy(f->name, "New World", sizeof("New World"));
	f->seed[0] = 0;
	f->revert[0] = 0;
	f->focus = CWORLD_ROW_NAME;
	f->mode = CWORLD_MODE_SURVIVAL;
	f->editing = false;
	f->edit_row = CWORLD_ROW_NAME;
}

bool cworld_form_create_enabled(const struct cworld_form* f) {
	assert(f);
	return f->name[0] != 0;
}

bool cworld_form_row_is_text(int row) {
	return row == CWORLD_ROW_NAME || row == CWORLD_ROW_SEED;
}

static bool cworld_row_reachable(const struct cworld_form* f, int row) {
	if(row == CWORLD_ROW_CREATE)
		return cworld_form_create_enabled(f);
	return row >= 0 && row < CWORLD_ROW_COUNT;
}

void cworld_form_move_focus(struct cworld_form* f, int dir) {
	assert(f && (dir == 1 || dir == -1));
	if(f->editing)
		return; /* the OSK owns the d-pad while the modal is up */

	int row = f->focus;
	for(int k = 0; k < CWORLD_ROW_COUNT; k++) {
		row = (row + dir + CWORLD_ROW_COUNT) % CWORLD_ROW_COUNT;
		if(cworld_row_reachable(f, row)) {
			f->focus = row;
			return;
		}
	}
	/* Unreachable: Cancel is always reachable, so the sweep always lands. */
}

void cworld_form_set_focus(struct cworld_form* f, int row) {
	assert(f);
	if(f->editing)
		return;
	if(row < 0 || row >= CWORLD_ROW_COUNT || !cworld_row_reachable(f, row))
		return;
	f->focus = row;
}

bool cworld_form_cycle_mode(struct cworld_form* f, int dir) {
	assert(f && (dir == 1 || dir == -1));
	if(f->editing || f->focus != CWORLD_ROW_MODE)
		return false;
	f->mode = (f->mode + dir + CWORLD_MODE_COUNT) % CWORLD_MODE_COUNT;
	return true;
}

const char* cworld_form_mode_label(int mode) {
	return mode == CWORLD_MODE_CREATIVE ? "Creative" : "Survival";
}

bool cworld_form_open_editor(struct cworld_form* f) {
	assert(f);
	if(f->editing || !cworld_form_row_is_text(f->focus))
		return false;
	f->edit_row = f->focus;
	f->editing = true;
	const char* src = (f->edit_row == CWORLD_ROW_NAME) ? f->name : f->seed;
	/* Both buffers fit in `revert` (CWORLD_SEED_CAP is the larger). */
	assert(strlen(src) < sizeof(f->revert));
	memcpy(f->revert, src, strlen(src) + 1);
	return true;
}

void cworld_form_commit_editor(struct cworld_form* f) {
	assert(f);
	if(!f->editing)
		return;
	f->editing = false;
	f->focus = f->edit_row;
	/* Emptying the name disables Create; if focus were parked there it would
	 * be stranded on a disabled control. It cannot be here (focus returns to
	 * the edited text row), but the ring's own skip is the invariant. */
}

void cworld_form_revert_editor(struct cworld_form* f) {
	assert(f);
	if(!f->editing)
		return;
	char* dst = (f->edit_row == CWORLD_ROW_NAME) ? f->name : f->seed;
	size_t cap = (f->edit_row == CWORLD_ROW_NAME) ? sizeof(f->name)
												  : sizeof(f->seed);
	size_t n = strlen(f->revert);
	assert(n < cap);
	memcpy(dst, f->revert, n + 1);
	(void)cap;
	f->editing = false;
	f->focus = f->edit_row;
}

char* cworld_form_edit_buffer(struct cworld_form* f) {
	assert(f);
	if(!f->editing)
		return NULL;
	return (f->edit_row == CWORLD_ROW_NAME) ? f->name : f->seed;
}

size_t cworld_form_edit_cap(const struct cworld_form* f) {
	assert(f);
	return (f->edit_row == CWORLD_ROW_NAME) ? sizeof(f->name)
											: sizeof(f->seed);
}

int cworld_form_edit_max(const struct cworld_form* f) {
	assert(f);
	/* [740c583 GuiCreateWorld.initGui]: name 32, seed left uncapped. */
	return (f->edit_row == CWORLD_ROW_NAME) ? CWORLD_NAME_MAX : 0;
}

int cworld_form_edit_page(const struct cworld_form* f) {
	assert(f);
	return (f->edit_row == CWORLD_ROW_SEED) ? OSK_PAGE_NUM : OSK_PAGE_LOWER;
}
