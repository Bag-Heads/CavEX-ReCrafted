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
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../../config.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../network/world_copy.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../util.h"
#include "../game_state.h"
#include "cworld_form_model.h"
#include "frontend_presentation.h"
#include "play_session.h"
#include "screen.h"
#include "widget_osk.h"

/* Beta GuiCreateWorld, cited from [740c583 GuiCreateWorld]:
 *   initGui: name field (w/2-100, y=60, 200x20, max 32, default
 *     "New World" [selectWorld.newWorld], FOCUSED), seed field (y=116,
 *     empty, maxStringLength 0 = unbounded), Create (h/4+96+12) + Cancel
 *     (h/4+120+12).
 *   keyTyped tail: Create.enabled = name.length() > 0, then
 *     calcSaveDirName() -- BOTH recomputed after every keystroke; '\r'
 *     presses Create; '\t' toggles field focus [GuiTextField -> parent
 *     .selectNextField].
 *   calcSaveDirName: trim(); each char of the 15-entry FORBIDDEN array
 *     (misleadingly NAMED allowedCharactersArray
 *     [740c583 ChatAllowedCharacters]) replaced with '_'; empty ->
 *     "World"; then generateUnusedFolderName appends "-" while a world
 *     with that folder exists.
 *   actionPerformed(0): createClicked latch; seed = Random().nextLong();
 *     if seed text non-empty: Long.parseLong -- on success use the value
 *     UNLESS 0 (0 keeps random); on NumberFormatException ->
 *     (long)text.hashCode(). Then startWorld(folder, RAW typed name, seed)
 *     -- the display name is NOT trimmed, only the folder is.
 *   actionPerformed(1): back to the parent screen.
 *
 * ADAPTED here -- CONSOLE EDITION, carve-out #2, see
 * docs/ui_design/create_world_console_2026-09-01.md (the approved design; it
 * supersedes the layout half of osk_create_world_2026-08-15.md):
 *   - Beta's Tab-toggles-two-fields + mouse-click focus becomes four screen
 *     rows in Beta's visual order (Name -> Seed | New seed -> Game Mode ->
 *     Create world | Details | Cancel, wrapping) driven by the D-pad -- a Wii
 *     has no Tab key and no mouse (ui-build-1026 T05; see "Screen" below).
 *     The IR pointer focuses the same controls, so pointer and d-pad always
 *     agree on ONE focus.
 *   - text ENTRY is a MODAL OSK opened with A on a text row (design section 4
 *     rule 1 / maintainer decision 3): the form owns the whole 240-unit
 *     height and reads like the Legacy Console Edition, instead of an
 *     always-visible keyboard eating half the screen. B reverts the edit and
 *     closes; the grid's OK key commits.
 *   - the seed editor opens on the OSK's NUMERIC page (design section 3.2 /
 *     decision 4).
 *   - layout y-offsets are the design's section 5 sketch, not Beta's 60/116.
 *   - Ctrl-V paste is DROPPED (no platform clipboard).
 *   - NET-NEW (creative exception, docs/creative_mode.md): the Game Mode
 *     selector row, persisted per world as LEVEL_GAME_MODE. Beta 1.7.3 has
 *     no game mode at all. Survival is ALWAYS the default (maintainer
 *     decision 1) and the mode is fixed at creation (decision 5).
 * Field palette, blink cadence, edit semantics, the Create-enable rule, the
 * folder + seed resolution and the create latch are all the decompile's, and
 * stay byte-exact. Micro-deviation kept from the 08-15 pass: the seed field
 * is capped at 63 chars (Beta unbounded).
 *
 * The focus/selector/modal state machine itself lives in cworld_form_model.c
 * so it is testable with no gfx and no input (tests/test_cworld_form.c). */

/* Layout (logical units; x-column = 200 wide, centered like Beta).
 * Design section 5. */
#define CWORLD_TITLE_Y 8
#define CWORLD_NAME_LABEL_Y 26
#define CWORLD_NAME_FIELD_Y 36
#define CWORLD_FOLDER_LABEL_Y 60
#define CWORLD_MODE_LABEL_Y 74
#define CWORLD_MODE_FIELD_Y 84
#define CWORLD_SEED_LABEL_Y 108
#define CWORLD_SEED_FIELD_Y 118
#define CWORLD_SEED_INFO_Y 142
#define CWORLD_CREATE_BTN_Y 176
#define CWORLD_CANCEL_BTN_Y 200
#define CWORLD_FIELD_H 20
#define CWORLD_COLUMN_W 200

/* ---- Pure Beta-semantics helpers (test targets) ------------------------ */

/* Java String.trim(): strips every char <= 0x20 from BOTH ends
 * [740c583 GuiCreateWorld.calcSaveDirName -> String.trim]. Exported
 * (screen.h): the rename dialog needs the same trim for its enable rule
 * and its SAVED name [740c583 GuiRenameWorld.actionPerformed]. */
void screen_cworld_java_trim(const char* in, char* out, size_t out_cap) {
	const char* s = in;
	const char* e = in + strlen(in);
	while(s < e && (unsigned char)*s <= ' ')
		s++;
	while(e > s && (unsigned char)e[-1] <= ' ')
		e--;
	size_t n = (size_t)(e - s);
	if(n >= out_cap)
		n = out_cap - 1;
	memcpy(out, s, n);
	out[n] = 0;
}

/* The 15-entry forbidden-filename array [740c583 ChatAllowedCharacters
 * .allowedCharactersArray]. '\0' is in Beta's array too but cannot occur
 * inside a C string -- structurally unreachable there as well (the field
 * filter admits printables only). */
static const char cworld_forbidden[] = {'/',  '\n', '\r', '\t', '\f', '`',
										'?',  '*',  '\\', '<',  '>',  '|',
										'"',  ':'};

static void cworld_derive_folder(const char* name, char* out, size_t out_cap) {
	assert(name && out && out_cap > 6);
	screen_cworld_java_trim(name, out, out_cap);

	for(char* p = out; *p; p++) {
		for(size_t k = 0; k < sizeof(cworld_forbidden); k++) {
			if(*p == cworld_forbidden[k]) {
				*p = '_';
				break;
			}
		}
	}

	/* MathHelper.stringNullOrLengthZero -> "World"
	 * [740c583 GuiCreateWorld.calcSaveDirName]. */
	if(!out[0])
		memcpy(out, "World", 6);
}

/* Java Long.parseLong(s, 10), strictly: optional '-' sign, >= 1 digit,
 * digits only, full string, int64 range -- Java ACCUMULATES NEGATIVE so
 * that Long.MIN_VALUE parses (the JDK algorithm, mirrored shape). Any
 * deviation (whitespace, partial parse, overflow, a '+' sign -- Java 6's
 * parseLong rejects '+'; the leniency is Java 7+, AFTER Beta) is a
 * NumberFormatException in Java -> returns false here and the caller takes
 * the hashCode path. */
static bool cworld_java_parse_long(const char* s, int64_t* out) {
	if(!*s)
		return false;
	bool neg = false;
	if(*s == '-') {
		neg = true;
		s++;
		if(!*s)
			return false;
	}
	int64_t limit = neg ? INT64_MIN : -INT64_MAX;
	int64_t multmin = limit / 10;
	int64_t result = 0;
	for(; *s; s++) {
		if(*s < '0' || *s > '9')
			return false;
		int digit = *s - '0';
		if(result < multmin)
			return false;
		result *= 10;
		if(result < limit + digit)
			return false;
		result -= digit;
	}
	*out = neg ? result : -result;
	return true;
}

/* Java String.hashCode(): h = 31*h + c over the chars, int32 WRAPAROUND,
 * then sign-extended to long by the (long) cast [740c583 GuiCreateWorld
 * .actionPerformed]. Computed in uint32 (signed overflow is UB in C); the
 * final cast through int32_t is the two's-complement reinterpretation both
 * our targets define. */
static int64_t cworld_java_hash(const char* s) {
	uint32_t h = 0;
	for(; *s; s++)
		h = h * 31u + (uint32_t)(unsigned char)*s;
	return (int64_t)(int32_t)h;
}

/* Beta's seed chain. Returns true with *out set when the text DETERMINES
 * the seed; false = use a random one (empty text, or a parsed numeric 0 --
 * the `!= 0L` gate). A hashCode result of 0 IS used (only the parseLong
 * path is gated) [740c583 GuiCreateWorld.actionPerformed]. */
static bool cworld_resolve_seed(const char* text, int64_t* out) {
	assert(text && out);
	if(!text[0])
		return false;
	int64_t v;
	if(cworld_java_parse_long(text, &v)) {
		if(v == 0)
			return false;
		*out = v;
		return true;
	}
	*out = cworld_java_hash(text);
	return true;
}

/* Java Long.toString for the "New seed" button: the decimal text that
 * cworld_java_parse_long reads back to the same value. */
static void cworld_format_long(int64_t v, char* out, size_t cap) {
	snprintf(out, cap, "%lld", (long long)v);
}

/* ---- Screen ------------------------------------------------------------
 *
 * THE APPROVED CREATE SCREEN (tmp/jobs/design-round-1026c/opus/opus-B-create
 * .png + NOTES.md section B, ui-build-1026 T05). Beta's own form in Beta's own
 * order [GuiCreateWorld.drawScreen l.120-124 + initGui l.26-31 in
 * mcb173-retromcp]: the title, "World Name" label (y 47), the name field
 * (y 60), "Will be saved in: <folder>" (y 85), "Seed for the World Generator"
 * (y 104), the seed field (y 116), "Leave blank for a random seed" (y 140),
 * then Create + Cancel. Labels are Beta's en_US.lang strings in the frontend's
 * sentence case, as the approved mock draws them.
 *
 * CavEX additions, each approved:
 *   - "New seed" beside the seed field (mock): fills a VISIBLE random number,
 *     so the player can note the seed before creating. Create then reads it
 *     through the unchanged Beta chain (parseLong), so the world gets exactly
 *     the number shown. A blank seed still means a random seed at create time.
 *   - Game mode (creative exception, docs/creative_mode.md), Survival default.
 *   - DETAILS (separately approved 2026-10-25, design-hub-1024/investigate
 *     item 1: "Left/Right reach Details and the Details card fits all 4
 *     lines"). The approved create mock has NO Details control (its Cancel
 *     replaced the old Details button). Reconciliation, documented rather than
 *     invented: Details sits IN the mock's own action row, between Create
 *     world and Cancel, so RIGHT from Create world reaches it and LEFT from
 *     Cancel reaches it. The mock's two buttons keep their order, height and
 *     y; only their widths shrink to make room (Create 268 -> 212, Cancel
 *     160 -> 108). The card is wider than before and WRAPS its lines, so all
 *     four are shown whole.
 *   - "+" = Create world from anywhere on the form, "1" = Back (mock footer).
 *
 * The approved mock has no Load/Create/Join strip, so this screen no longer
 * draws one: Cancel / Back returns to the Load tab (Beta's Cancel returns to
 * the parent screen [GuiCreateWorld.actionPerformed id 1]), whose own strip
 * still reaches Join. The play_session draft is unchanged: leaving stores the
 * form, re-entering restores it, and only a successful create or a fresh title
 * entry drops it.
 *
 * Every control has ONE rect function (cworld_target_rect) used for BOTH the
 * draw and the hit test. cworld_form_model.c still owns the text, the mode,
 * the editor and Beta's enable rule; this screen owns the visual order and the
 * sub-focus the model has no row for:
 *     Name                        -> CWORLD_ROW_NAME
 *     Seed field / New seed       -> CWORLD_ROW_SEED   (+ seed_sub)
 *     Survival / Creative         -> CWORLD_ROW_MODE   (+ which is lit)
 *     Create world                -> CWORLD_ROW_CREATE
 *     Details / Cancel            -> CWORLD_ROW_CANCEL (+ action_sub)
 * The model's own Up/Down ring (Name, Mode, Seed, ...) is NOT used here: the
 * approved visual order puts the seed above the mode, so Up/Down walk the
 * SCREEN rows below and set the model row with cworld_form_set_focus (which
 * still refuses a disabled Create and is a no-op while editing). */

/* The canvas: 640x480, scaled by height/480 and centred, exactly as the title
 * and the Load tab map theirs. */
struct cworld_canvas {
	int ox, scale_num, scale_den;
};
static struct cworld_canvas cworld_canvas_of(int width, int height) {
	struct cworld_canvas c;
	c.scale_num = height;
	c.scale_den = 480;
	c.ox = (width - 640 * height / 480) / 2;
	return c;
}
static int cw_d(const struct cworld_canvas* c, int v) {
	return v * c->scale_num / c->scale_den;
}
static int cw_x(const struct cworld_canvas* c, int v) {
	return c->ox + cw_d(c, v);
}

/* Header: the Load tab's own title/subtitle numbers. */
#define CW_HEAD_X 88
#define CW_HEAD_Y 14
#define CW_HEAD_FONT 26
#define CW_SUB_RIGHT 552
#define CW_SUB_Y 18
#define CW_SUB_W 226
#define CW_SUB_FONT 15

/* The approved B-create geometry (make_spec.py "B-create"), canvas units. */
#define CW_PANEL_X 88
#define CW_PANEL_Y 50
#define CW_PANEL_W 464
#define CW_PANEL_H 300
#define CW_LABEL_X 104
#define CW_LABEL_FONT 16
#define CW_HINT_FONT 13
#define CW_NAME_LABEL_Y 62
#define CW_NAME_X 104
#define CW_NAME_Y 82
#define CW_NAME_W 432
#define CW_NAME_H 44
#define CW_FOLDER_Y 132
#define CW_SEED_LABEL_Y 158
#define CW_SEED_X 104
#define CW_SEED_Y 178
#define CW_SEED_W 300
#define CW_SEED_H 44
#define CW_NEWSEED_X 412
#define CW_NEWSEED_Y 178
#define CW_NEWSEED_W 124
#define CW_NEWSEED_H 44
#define CW_SEED_INFO_Y 228
#define CW_MODE_LABEL_Y 256
#define CW_MODE_Y 276
#define CW_MODE_W 212
#define CW_MODE_H 44
#define CW_MODE_SURVIVAL_X 104
#define CW_MODE_CREATIVE_X 324
#define CW_MODE_ICON 24
#define CW_ACTION_Y 366
#define CW_ACTION_H 48
#define CW_CREATE_X 100
#define CW_CREATE_W 212
#define CW_DETAILS_X 320
#define CW_DETAILS_W 104
#define CW_CANCEL_X 432
#define CW_CANCEL_W 108
#define CW_FOOTER_Y 432
#define CW_FOOTER_H 36
#define CW_FIELD_FONT 20
#define CW_BUTTON_FONT 18
#define CW_FAIL_Y 334

/* The Details card: as wide as the approved OSK panel, lines WRAPPED. */
#define CW_DET_X 56
#define CW_DET_Y 50
#define CW_DET_W 528
#define CW_DET_H 368
#define CW_DET_TITLE_X 76
#define CW_DET_TITLE_Y 66
#define CW_DET_TITLE_FONT 24
#define CW_DET_BODY_X 76
#define CW_DET_BODY_Y 108
#define CW_DET_BODY_W 488
#define CW_DET_BODY_FONT 16
#define CW_DET_BODY_LINE 22
#define CW_DET_ITEM_GAP 8
#define CW_DET_BACK_X 216
#define CW_DET_BACK_Y 354
#define CW_DET_BACK_W 208
#define CW_DET_BACK_H 48
#define CW_DET_LINES_MAX 12
#define CW_DET_LINE_CAP 128

enum cworld_target {
	CW_T_NAME = 0,
	CW_T_SEED,
	CW_T_NEW_SEED,
	CW_T_MODE_SURVIVAL,
	CW_T_MODE_CREATIVE,
	CW_T_CREATE,
	CW_T_DETAILS,
	CW_T_CANCEL,
	CW_T_DETAILS_BACK,
	CW_T_COUNT
};

static const char* const cworld_target_names[CW_T_COUNT] = {
	"name",		"seed",	   "new_seed", "survival",	   "creative",
	"create",	"details", "cancel",   "details_back",
};

static struct cworld_form form;
static char folder_preview[96];
static struct osk_frontend osk;
static bool create_latch;
static bool pointer_available;
static float pointer_x, pointer_y, pointer_angle;

/* Screen-only sub-focus the model has no row for. */
static bool seed_sub_new;  /* seed row: New seed (true) or the field */
static int action_sub;	   /* CANCEL row: CW_T_DETAILS or CW_T_CANCEL */
static int mode_focus;	   /* mode row: which button is lit */
static bool details_open;  /* the Details card is up */
static enum gutil_menu_origin focus_origin;
static bool create_failed; /* the last create attempt failed */
static struct gutil_menu_focus pointer_focus;
static uint64_t new_seed_salt;

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* What the screen last handed to the create path (test observer only). */
static unsigned test_submits;
static bool test_submit_seed_set;
static int64_t test_submit_seed;
#endif

/* Preview-only dedupe probe [GuiCreateWorld.generateUnusedFolderName l.52-58
 * -- Beta's preview shows the ALREADY-deduped name]: append "-" while the
 * directory exists. Display only; the create path re-runs the loop with mkdir
 * as the reservation (TOCTOU-safe), producing the same name. */
static void cworld_probe_unused(const char* saves_path, char* folder,
								size_t cap) {
	struct world_copy_journals* journals;
	if(!world_copy_journals_read(saves_path, &journals))
		return; /* The final reservation refuses unreadable journals. */
	for(int guard = 0; guard < 64; guard++) {
		char full[512];
		snprintf(full, sizeof(full), "%s/%s", saves_path, folder);
		struct stat st;
		if(!world_copy_journals_contains(journals, full) && stat(full, &st) != 0)
			break;
		size_t len = strlen(folder);
		if(len + 2 > cap)
			break;
		folder[len] = '-';
		folder[len + 1] = 0;
	}
	world_copy_journals_free(journals);
}

static void cworld_refresh_preview(void) {
	cworld_derive_folder(form.name, folder_preview, sizeof(folder_preview));
	const char* saves_path
		= config_read_string(&gstate.config_user, "paths.worlds", "saves");
	cworld_probe_unused(saves_path, folder_preview, sizeof(folder_preview));
}

/* ---- geometry: ONE rect per control, draw and hit ---------------------- */

static struct fep_rect cworld_target_rect(int width, int height, int target) {
	struct cworld_canvas c = cworld_canvas_of(width, height);
#define CW_R(X, Y, W, H) fep_rect(cw_x(&c, X), cw_d(&c, Y), cw_d(&c, W), cw_d(&c, H))
	switch(target) {
		case CW_T_NAME: return CW_R(CW_NAME_X, CW_NAME_Y, CW_NAME_W, CW_NAME_H);
		case CW_T_SEED: return CW_R(CW_SEED_X, CW_SEED_Y, CW_SEED_W, CW_SEED_H);
		case CW_T_NEW_SEED:
			return CW_R(CW_NEWSEED_X, CW_NEWSEED_Y, CW_NEWSEED_W, CW_NEWSEED_H);
		case CW_T_MODE_SURVIVAL:
			return CW_R(CW_MODE_SURVIVAL_X, CW_MODE_Y, CW_MODE_W, CW_MODE_H);
		case CW_T_MODE_CREATIVE:
			return CW_R(CW_MODE_CREATIVE_X, CW_MODE_Y, CW_MODE_W, CW_MODE_H);
		case CW_T_CREATE:
			return CW_R(CW_CREATE_X, CW_ACTION_Y, CW_CREATE_W, CW_ACTION_H);
		case CW_T_DETAILS:
			return CW_R(CW_DETAILS_X, CW_ACTION_Y, CW_DETAILS_W, CW_ACTION_H);
		case CW_T_CANCEL:
			return CW_R(CW_CANCEL_X, CW_ACTION_Y, CW_CANCEL_W, CW_ACTION_H);
		default:
			return CW_R(CW_DET_BACK_X, CW_DET_BACK_Y, CW_DET_BACK_W,
						CW_DET_BACK_H);
	}
#undef CW_R
}

/* Is this target reachable RIGHT NOW? Details owns the frame while it is up;
 * Create is Beta's `name.length() > 0` rule [GuiCreateWorld.keyTyped l.107]. */
static bool cworld_target_enabled(int target) {
	if(details_open)
		return target == CW_T_DETAILS_BACK;
	if(target == CW_T_DETAILS_BACK)
		return false;
	if(target == CW_T_CREATE)
		return cworld_form_create_enabled(&form);
	return target >= 0 && target < CW_T_COUNT;
}

/* The target the highlight is on, DERIVED from the model row plus the
 * screen-only sub-focus -- never stored twice. */
static int cworld_focus_target(void) {
	if(details_open)
		return CW_T_DETAILS_BACK;
	switch(form.focus) {
		case CWORLD_ROW_NAME: return CW_T_NAME;
		case CWORLD_ROW_SEED: return seed_sub_new ? CW_T_NEW_SEED : CW_T_SEED;
		case CWORLD_ROW_MODE: return mode_focus;
		case CWORLD_ROW_CREATE: return CW_T_CREATE;
		default: return action_sub;
	}
}

/* Put the highlight ON a target. The model's set_focus is the only thing that
 * moves a content row (it refuses a disabled Create and is a no-op while
 * editing); the sub-focus only follows when the row move was accepted. */
static void cworld_focus_set_target(int target) {
	switch(target) {
		case CW_T_NAME: cworld_form_set_focus(&form, CWORLD_ROW_NAME); return;
		case CW_T_SEED:
		case CW_T_NEW_SEED:
			cworld_form_set_focus(&form, CWORLD_ROW_SEED);
			if(form.focus == CWORLD_ROW_SEED)
				seed_sub_new = target == CW_T_NEW_SEED;
			return;
		case CW_T_MODE_SURVIVAL:
		case CW_T_MODE_CREATIVE:
			cworld_form_set_focus(&form, CWORLD_ROW_MODE);
			if(form.focus == CWORLD_ROW_MODE)
				mode_focus = target;
			return;
		case CW_T_CREATE: cworld_form_set_focus(&form, CWORLD_ROW_CREATE); return;
		case CW_T_DETAILS:
		case CW_T_CANCEL:
			cworld_form_set_focus(&form, CWORLD_ROW_CANCEL);
			if(form.focus == CWORLD_ROW_CANCEL)
				action_sub = target;
			return;
		default: return;
	}
}

/* The four SCREEN rows, top to bottom, in the approved visual order. */
static int cworld_row_of(int target) {
	switch(target) {
		case CW_T_NAME: return 0;
		case CW_T_SEED:
		case CW_T_NEW_SEED: return 1;
		case CW_T_MODE_SURVIVAL:
		case CW_T_MODE_CREATIVE: return 2;
		default: return 3;
	}
}

/* Up/Down: the entry target of a screen row. The mode row lands on the CHOSEN
 * mode; the action row on Create world, or Details while Create is disabled. */
static int cworld_row_entry(int row) {
	switch(row) {
		case 0: return CW_T_NAME;
		case 1: return CW_T_SEED;
		case 2:
			return form.mode == CWORLD_MODE_CREATIVE ? CW_T_MODE_CREATIVE
													 : CW_T_MODE_SURVIVAL;
		default:
			return cworld_form_create_enabled(&form) ? CW_T_CREATE
													 : CW_T_DETAILS;
	}
}

static int cworld_pointer_target(int width, int height) {
	if(!pointer_available)
		return GUTIL_MENU_TARGET_NONE;
	for(int k = 0; k < CW_T_COUNT; k++) {
		if(details_open ? k != CW_T_DETAILS_BACK : k == CW_T_DETAILS_BACK)
			continue;
		if(fep_rect_hit(cworld_target_rect(width, height, k), pointer_x,
						pointer_y))
			return k;
	}
	return GUTIL_MENU_TARGET_NONE;
}

/* ---- the Details card's text, wrapped (draw and test share it) ---------- */

/* Greedy word wrap of `src` into `room` px at `font`; a word wider than the
 * room is broken by characters. Returns the number of lines written. */
static int cworld_wrap(const char* src, int font, int room,
					   char lines[][CW_DET_LINE_CAP], int max_lines) {
	int n = 0;
	const char* p = src;
	while(*p && n < max_lines) {
		while(*p == ' ')
			p++;
		if(!*p)
			break;
		char line[CW_DET_LINE_CAP];
		size_t len = 0, last_break = 0;
		const char* q = p;
		while(*q && len + 1 < sizeof(line)) {
			line[len] = *q;
			line[len + 1] = 0;
			if(gutil_font_width(line, font) > room) {
				line[len] = 0;
				break;
			}
			len++;
			q++;
			if(*q == ' ' || !*q)
				last_break = len;
		}
		if(*q && last_break > 0 && *q != ' ') {
			len = last_break; /* back off to the last whole word */
			q = p + last_break;
		}
		if(len == 0) { /* not even one glyph fits: emit it alone */
			len = 1;
			q = p + 1;
		}
		line[len] = 0;
		memcpy(lines[n], line, len + 1);
		n++;
		p = q;
	}
	return n;
}

/* The card's four items, from the SAME state Create will submit. */
static void cworld_details_items(char items[4][CW_DET_LINE_CAP]) {
	snprintf(items[0], CW_DET_LINE_CAP, "Will be saved in: %s", folder_preview);
	snprintf(items[1], CW_DET_LINE_CAP,
			 "Game mode: %s. It is fixed when the world is created.",
			 cworld_form_mode_label(form.mode));
	int64_t v;
	if(!form.seed[0]) {
		snprintf(items[2], CW_DET_LINE_CAP,
				 "Seed: blank, so Create picks a random seed.");
	} else if(!cworld_resolve_seed(form.seed, &v)) {
		snprintf(items[2], CW_DET_LINE_CAP,
				 "Seed: 0 counts as blank, so Create picks a random seed.");
	} else {
		char num[24];
		cworld_format_long(v, num, sizeof(num));
		if(strcmp(num, form.seed) == 0)
			snprintf(items[2], CW_DET_LINE_CAP, "Seed: %s, used as typed.", num);
		else
			snprintf(items[2], CW_DET_LINE_CAP,
					 "Seed: the text \"%.24s%s\" becomes the number %s.", form.seed,
					 strlen(form.seed) > 24 ? "..." : "", num);
	}
	snprintf(items[3], CW_DET_LINE_CAP,
			 "Create world starts a new local world on this console.");
}

/* Lays the card out at this resolution. Returns the line count; *fits says
 * every line is inside the body width AND above the Back button. */
static int cworld_details_layout(int width, int height,
								 char lines[CW_DET_LINES_MAX][CW_DET_LINE_CAP],
								 int item_of[CW_DET_LINES_MAX], bool* fits) {
	struct cworld_canvas c = cworld_canvas_of(width, height);
	int font = cw_d(&c, CW_DET_BODY_FONT);
	int room = cw_d(&c, CW_DET_BODY_W);
	char items[4][CW_DET_LINE_CAP];
	cworld_details_items(items);
	int n = 0;
	bool ok = true;
	for(int k = 0; k < 4; k++) {
		int got = cworld_wrap(items[k], font, room, lines + n,
							  CW_DET_LINES_MAX - n);
		if(got == 0)
			ok = false;
		for(int j = 0; j < got; j++) {
			item_of[n + j] = k;
			if(gutil_font_width(lines[n + j], font) > room)
				ok = false;
		}
		n += got;
	}
	/* Every item's text must be shown WHOLE: the wrapped lines rejoin to it. */
	for(int k = 0; k < 4 && ok; k++) {
		char joined[CW_DET_LINE_CAP * 2] = "";
		for(int j = 0; j < n; j++) {
			if(item_of[j] != k)
				continue;
			if(joined[0] && lines[j][0] != ' ')
				strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
			strncat(joined, lines[j], sizeof(joined) - strlen(joined) - 1);
		}
		char want[CW_DET_LINE_CAP];
		snprintf(want, sizeof(want), "%s", items[k]);
		/* compare ignoring the spaces the wrap consumed at breaks */
		const char *a = joined, *b = want;
		while(*a || *b) {
			if(*a == ' ' && *b != ' ') { a++; continue; }
			if(*b == ' ' && *a != ' ') { b++; continue; }
			if(*a != *b) { ok = false; break; }
			a++;
			b++;
		}
	}
	int bottom = cw_d(&c, CW_DET_BODY_Y) + n * cw_d(&c, CW_DET_BODY_LINE)
		+ 3 * cw_d(&c, CW_DET_ITEM_GAP);
	if(bottom > cw_d(&c, CW_DET_BACK_Y))
		ok = false;
	if(fits)
		*fits = ok;
	return n;
}

/* ---- editor plumbing (semantics unchanged) ----------------------------- */

/* Bind the OSK to the field the modal just opened, on that field's page. */
static void cworld_bind_editor(void) {
	assert(form.editing);
	osk_frontend_reset(&osk, cworld_form_edit_buffer(&form),
		cworld_form_edit_cap(&form), cworld_form_edit_max(&form),
		form.edit_row == CWORLD_ROW_SEED ? 1 : 0, false);
	/* T05a: big name keys / seed keypad (opus-B name-osk, seed-osk). */
	osk_frontend_set_profile(&osk, form.edit_row == CWORLD_ROW_SEED
		? OSK_FRONT_PROFILE_CREATE_SEED : OSK_FRONT_PROFILE_CREATE_NAME);
	/* The PC physical keyboard owns the screen only while the modal is up;
	 * on the form the same keys are the menu action aliases (input.h). */
	input_text_enable(true);
}

static void cworld_close_editor(bool commit) {
	if(commit)
		cworld_form_commit_editor(&form);
	else
		cworld_form_revert_editor(&form);
	input_text_enable(false);
	cworld_refresh_preview();
}

/* "New seed": a VISIBLE random long, typed into the seed field so Create's
 * unchanged Beta chain reads it back exactly [GuiCreateWorld.actionPerformed
 * l.75-86]. 0 is skipped because Beta treats a parsed 0 as blank. The source
 * is the same clock + session-salt mix the create path uses for a blank seed
 * (screen_select_world.c sworld_finish_create), finished with splitmix64 so
 * two presses in the same second still differ. */
static void cworld_new_seed_into(char* out, size_t cap) {
	int64_t v = 0;
	while(v == 0) {
		new_seed_salt++;
		uint64_t z = (uint64_t)CLOCK_SEED_TIME() * 6364136223846793005ULL
			+ new_seed_salt * 0x9E3779B97F4A7C15ULL;
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		z ^= z >> 31;
		v = (int64_t)z;
	}
	cworld_format_long(v, out, cap);
}
static void cworld_new_seed(void) {
	cworld_new_seed_into(form.seed, sizeof(form.seed));
}

/* ---- the session seam -------------------------------------------------- */

static void cworld_sync_subfocus(void) {
	seed_sub_new = false;
	action_sub = CW_T_CANCEL;
	mode_focus = form.mode == CWORLD_MODE_CREATIVE ? CW_T_MODE_CREATIVE
												   : CW_T_MODE_SURVIVAL;
}

static void screen_cworld_reset(struct screen* s, int width, int height) {
	(void)s;
	(void)width;
	(void)height;
	input_pointer_enable(true);
	/* Text input is armed only while the OSK modal is open. */
	input_text_enable(false);

	/* RESUME, then FRESH. A retained draft is restored verbatim; with no
	 * draft this is Beta's initGui: default name, empty seed, NAME focused
	 * [GuiCreateWorld.initGui l.28-31]. */
	if(!play_session_restore_draft(&form))
		cworld_form_reset(&form);
	form.editing = false;

	cworld_sync_subfocus();
	details_open = false;
	create_latch = false;
	create_failed = false;
	pointer_available = false;
	focus_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	pointer_focus = (struct gutil_menu_focus) {0};
	cworld_refresh_preview();
}

/* LEAVE to the Load tab: commit the form to the session first. Nothing is
 * discarded -- the draft outlives the screen. */
static void cworld_leave_to_load(void) {
	input_text_enable(false);
	play_session_store_draft(&form);
	play_session_set_tab(PLAY_TAB_LOAD);
	screen_set(&screen_select_world);
}

static void cworld_try_create(void) {
	/* Beta's enable rule guards the button, plus the createClicked latch
	 * [GuiCreateWorld.actionPerformed l.65-74, keyTyped l.107]. */
	if(create_latch || !cworld_form_create_enabled(&form))
		return;
	create_latch = true; /* createClicked */

	char folder_base[96];
	cworld_derive_folder(form.name, folder_base, sizeof(folder_base));

	int64_t seed = 0;
	bool seed_set = cworld_resolve_seed(form.seed, &seed);
	bool creative = form.mode == CWORLD_MODE_CREATIVE;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	test_submits++;
	test_submit_seed_set = seed_set;
	test_submit_seed = seed;
#endif

	/* The display name is the RAW typed text (Beta does not trim it,
	 * l.89 startWorld(folderName, textboxWorldName.getText(), seed)). */
	if(screen_sworld_create_from_dialog(form.name, folder_base, seed_set, seed,
										creative)) {
		input_text_enable(false);
		play_session_clear_draft();
		create_failed = false;
	} else {
		fprintf(stderr, "[cworld] create failed for '%s'\n", folder_base);
		create_latch = false; /* stay on the form, retryable */
		create_failed = true; /* and SAY so on the panel */
	}
}

/* A on the focused target. */
static void cworld_activate(int target) {
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	switch(target) {
		case CW_T_NAME:
		case CW_T_SEED:
			if(cworld_form_open_editor(&form))
				cworld_bind_editor();
			return;
		case CW_T_NEW_SEED: cworld_new_seed(); return;
		case CW_T_MODE_SURVIVAL:
		case CW_T_MODE_CREATIVE:
			/* Pressing a mode BUTTON chooses that mode outright; the model
			 * stays the only mutator. */
			if((target == CW_T_MODE_CREATIVE)
			   != (form.mode == CWORLD_MODE_CREATIVE))
				(void)cworld_form_cycle_mode(&form, 1);
			return;
		case CW_T_CREATE: cworld_try_create(); return;
		case CW_T_DETAILS:
			details_open = true;
			cworld_refresh_preview();
			return;
		case CW_T_CANCEL: cworld_leave_to_load(); return;
		case CW_T_DETAILS_BACK: details_open = false; return;
		default: return;
	}
}

/* ---- update ------------------------------------------------------------ */

/* The OSK modal frame: the widget owns the d-pad, A, the backspace
 * accelerator and (on PC) the physical keyboard. */
static void cworld_update_modal(int width) {
	struct input_player_frame scratch;
	enum osk_event ev = osk_frontend_update_owned(&osk, width, gfx_height(),
		osk_frontend_input_frame(&scratch), true, pointer_available, pointer_x, pointer_y,
		true, true);

	switch(ev) {
		case OSK_EV_CHANGED:
			/* keyTyped tail: live folder recompute [l.108]. Only the NAME
			 * feeds the folder; do not stat() the SD per seed keystroke. */
			if(form.edit_row == CWORLD_ROW_NAME)
				cworld_refresh_preview();
			return;
		case OSK_EV_DONE: /* field Done: keep the text, close; never create */
			cworld_close_editor(true);
			return;
		case OSK_EV_NEXT: /* physical Tab: commit + selectNextField [l.130] */
			cworld_form_commit_editor(&form);
			cworld_form_set_focus(&form, form.focus == CWORLD_ROW_NAME
				? CWORLD_ROW_SEED : CWORLD_ROW_NAME);
			seed_sub_new = false;
			if(cworld_form_open_editor(&form)) {
				osk_frontend_bind(&osk, cworld_form_edit_buffer(&form),
					cworld_form_edit_cap(&form), cworld_form_edit_max(&form));
				osk_frontend_set_profile(&osk, form.edit_row == CWORLD_ROW_SEED
					? OSK_FRONT_PROFILE_CREATE_SEED : OSK_FRONT_PROFILE_CREATE_NAME);
			}
			cworld_refresh_preview();
			return;
		case OSK_EV_CANCEL:
			cworld_close_editor(false);
			return;
		case OSK_EV_RANDOM:
			/* Keypad Random (T05a): the same visible seed as "New seed", but
			 * into the DRAFT, so Cancel still restores the old seed. */
			cworld_new_seed_into(cworld_form_edit_buffer(&form),
				cworld_form_edit_cap(&form));
			osk_frontend_bind(&osk, cworld_form_edit_buffer(&form),
				cworld_form_edit_cap(&form), cworld_form_edit_max(&form));
			return;
		default: break;
	}
}

/* Directional navigation on the form. Returns true when a direction edge was
 * consumed (even if the focus could not move). */
static bool cworld_navigate(void) {
	bool up = input_pressed(IB_GUI_UP), down = input_pressed(IB_GUI_DOWN);
	bool left = input_pressed(IB_GUI_LEFT), right = input_pressed(IB_GUI_RIGHT);
	if(!(up || down || left || right))
		return false;
	if(details_open)
		return true; /* the card's one control already owns the focus */

	int at = cworld_focus_target();
	if(up != down) {
		int row = (cworld_row_of(at) + (down ? 1 : 3)) % 4;
		cworld_focus_set_target(cworld_row_entry(row));
		return true;
	}
	if(left == right)
		return true;
	int dir = right ? 1 : -1;
	switch(cworld_row_of(at)) {
		case 1:
			cworld_focus_set_target(right ? CW_T_NEW_SEED : CW_T_SEED);
			break;
		case 2:
			/* The model's one-press cycle; the highlight follows the mode. */
			if(cworld_form_cycle_mode(&form, dir))
				mode_focus = form.mode == CWORLD_MODE_CREATIVE
					? CW_T_MODE_CREATIVE : CW_T_MODE_SURVIVAL;
			break;
		case 3: {
			/* Create world | Details | Cancel, no wrap; a disabled Create is
			 * refused by the model, so LEFT from Details stays put. */
			int next = at + dir;
			if(next >= CW_T_CREATE && next <= CW_T_CANCEL)
				cworld_focus_set_target(next);
			break;
		}
		default: break;
	}
	return true;
}

static float menu_motion_time;

static void screen_cworld_update(struct screen* s, float dt) {
	(void)s;
	if(dt > 0.0F && !form.editing)
		menu_motion_time = fmodf(menu_motion_time + dt, 128.0F);
	int width = gfx_width();
	int height = gfx_height();

	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);

	/* THE EDITOR IS FIRST AND TOTAL: no form control exists while it is up. */
	if(form.editing) {
		cworld_update_modal(width);
		return;
	}

	bool navigated = cworld_navigate();
	if(navigated)
		focus_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;

	/* THE ONE activation rule (Astra ruling 2 / ruling 5 D2), shared with
	 * every other menu: the sprite takes the highlight only when it MOVES onto
	 * a control, a same-frame navigate+activate is ONE action, and a consumed
	 * press cannot also do something else. */
	int hovered = cworld_pointer_target(width, height);
	bool pointer_owner = gutil_menu_pointer_focus(&pointer_focus,
		pointer_available, pointer_x, pointer_y, navigated);
	if(pointer_owner && hovered != GUTIL_MENU_TARGET_NONE
	   && cworld_target_enabled(hovered)) {
		cworld_focus_set_target(hovered);
		focus_origin = GUTIL_MENU_ORIGIN_POINTER;
	}
	int selected = cworld_focus_target();
	struct gutil_menu_activate_state in = {
		.pressed = input_pressed(IB_GUI_CLICK),
		.navigated = navigated,
		.pointer_valid = pointer_available,
		.selection_origin = focus_origin,
		.hovered = hovered,
		.hovered_enabled
		= hovered != GUTIL_MENU_TARGET_NONE && cworld_target_enabled(hovered),
		.selected = selected,
		.selected_enabled = cworld_target_enabled(selected),
		.selected_destructive = false,
	};
	struct gutil_menu_decision decision = gutil_menu_activate_decide(in);
	if(decision.selected != selected
	   && decision.selected != GUTIL_MENU_TARGET_NONE)
		cworld_focus_set_target(decision.selected);
	focus_origin = decision.selection_origin;

	if(decision.action == GUTIL_MENU_ACTIVATE
	   && decision.target != GUTIL_MENU_TARGET_NONE) {
		cworld_activate(decision.target);
		return; /* activation may have opened the editor or left the screen */
	}
	if(in.pressed)
		return; /* the rule consumed the press; nothing else may read it */

	/* Back. Inside Details it closes the card and CONSUMES the frame; on the
	 * form it stores the draft and returns to the Load tab. */
	if(input_pressed(IB_INVENTORY) || input_pressed(IB_HOME)) {
		if(details_open) {
			details_open = false;
			return;
		}
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		cworld_leave_to_load();
		return;
	}

	/* "+" = Create world from anywhere on the form (approved footer). Same
	 * guard as the button: Beta's enable rule and the create latch. */
	if(!details_open && !navigated && input_pressed(IB_GUI_PAGE)
	   && cworld_form_create_enabled(&form)) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		cworld_try_create();
	}
}

/* ---- render ------------------------------------------------------------ */

#define CW_INK 0x30, 0x30, 0x30
#define CW_GREY 0x60, 0x60, 0x60

/* A control box: the pack's own widget material, the approved thin dark
 * border, and the bright ring only on the focused one. */
static void cworld_draw_control(struct fep_rect r, bool focused, bool enabled) {
	fep_widget(r, focused && enabled);
	int border = GFX_GUI_SCALE / 2;
	fep_outline(r, border > 0 ? border : 1, 0x00, 0x00, 0x00, 0x60);
	if(focused)
		fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
	if(!enabled)
		fep_fill(r, 0, 0, 0, 0x70);
}

static void cworld_draw_button(struct fep_rect r, const char* label, int font,
							   bool focused, bool enabled) {
	cworld_draw_control(r, focused, enabled);
	char fitted[48];
	(void)fep_text_fit(fitted, sizeof(fitted), label, font,
					   r.w - 2 * GFX_GUI_SCALE * 3);
	int tw = gutil_font_width(fitted, font);
	gutil_text_rgb(r.x + (r.w - tw) / 2, r.y + (r.h - font) / 2, fitted, font,
				   true, enabled ? 0xF4 : 0xA0, enabled ? 0xF4 : 0xA0,
				   enabled ? 0xED : 0xA0);
}

static void cworld_draw_field(struct fep_rect r, bool focused) {
	fep_fill(r, 0x23, 0x23, 0x21, 255);
	fep_outline(r, GFX_GUI_SCALE, 0x70, 0x70, 0x68, 255);
	if(focused)
		fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 255);
}

/* One line of text inside a box, left-padded and fitted. */
static void cworld_draw_boxed_text(struct fep_rect r, const char* text,
								   int font, bool dim) {
	char fitted[96];
	int pad = GFX_GUI_SCALE * 4;
	(void)fep_text_fit(fitted, sizeof(fitted), text, font, r.w - 2 * pad);
	gutil_text_rgb(r.x + pad, r.y + (r.h - font) / 2, fitted, font, false,
				   dim ? 0xA0 : 0xF4, dim ? 0xA0 : 0xF4, dim ? 0xA0 : 0xED);
}

static void cworld_label(const struct cworld_canvas* c, int y, const char* s,
						 int size) {
	char fitted[128];
	(void)fep_text_fit(fitted, sizeof(fitted), s, cw_d(c, size),
					   cw_d(c, CW_PANEL_W - 32));
	gutil_text_rgb(cw_x(c, CW_LABEL_X), cw_d(c, y), fitted, cw_d(c, size),
				   false, CW_GREY);
}

/* The footer: Move, A Type/Select, + Create, 1 Back (approved mock). */
#define CW_FOOTER_N 4
static void cworld_footer(int width, int height) {
	static const enum input_button buttons[CW_FOOTER_N]
		= {IB_GUI_UP, IB_GUI_CLICK, IB_GUI_PAGE, IB_INVENTORY};
	int focus = cworld_focus_target();
	const char* labels[CW_FOOTER_N]
		= {"Move", (focus == CW_T_NAME || focus == CW_T_SEED) ? "Type" : "Select",
		   "Create", "Back"};
	struct input_player_frame prompts;
	input_prompt_frame(0, &prompts);
	/* The centred hint row (controller UI redesign 10-31, A1 helper). */
	struct gutil_hint_item items[CW_FOOTER_N];
	for(int k = 0; k < CW_FOOTER_N; k++)
		items[k] = (struct gutil_hint_item) {
			{buttons[k]}, 1, labels[k], k == 2 ? 3 : 2 - k / 2,
			k == CW_FOOTER_N - 1};
	struct gutil_hint_row row;
	if(gutil_hint_row_layout(&prompts, 0, 0, width, height, false,
							 GUTIL_HINT_ALIGN_CENTRE, items, CW_FOOTER_N, &row))
		gutil_hint_row_draw(&row);
}

static void cworld_render_form(int width, int height) {
	struct cworld_canvas c = cworld_canvas_of(width, height);
	int focus = cworld_focus_target();

	/* Header: Beta's title [selectWorld.create] + the breadcrumb. */
	gutil_text(cw_x(&c, CW_HEAD_X), cw_d(&c, CW_HEAD_Y), "Create new world",
			   cw_d(&c, CW_HEAD_FONT), true);
	{
		int font = cw_d(&c, CW_SUB_FONT);
		char fitted[64];
		(void)fep_text_fit(fitted, sizeof(fitted), "Play game", font,
						   cw_d(&c, CW_SUB_W));
		int tw = gutil_font_width(fitted, font);
		gutil_text_rgb(cw_x(&c, CW_SUB_RIGHT) - tw, cw_d(&c, CW_SUB_Y), fitted,
					   font, true, 0xD5, 0xD6, 0xCA);
	}

	struct fep_rect panel = fep_rect(cw_x(&c, CW_PANEL_X), cw_d(&c, CW_PANEL_Y),
									 cw_d(&c, CW_PANEL_W), cw_d(&c, CW_PANEL_H));
	fep_paper(panel);
	fep_bevel(panel);
	int field_font = cw_d(&c, CW_FIELD_FONT);
	int button_font = cw_d(&c, CW_BUTTON_FONT);

	/* World name [selectWorld.enterName] + field + the folder line
	 * [selectWorld.resultFolder + folderName, l.122]. */
	cworld_label(&c, CW_NAME_LABEL_Y, "World name", CW_LABEL_FONT);
	struct fep_rect name = cworld_target_rect(width, height, CW_T_NAME);
	cworld_draw_field(name, focus == CW_T_NAME);
	cworld_draw_boxed_text(name, form.name, field_font, false);
	{
		char line[128];
		snprintf(line, sizeof(line), "Will be saved in: %s", folder_preview);
		cworld_label(&c, CW_FOLDER_Y, line, CW_HINT_FONT);
	}

	/* Seed [selectWorld.enterSeed] + field + New seed + the hint
	 * [selectWorld.seedInfo, l.124]. */
	cworld_label(&c, CW_SEED_LABEL_Y, "Seed for the world generator",
				 CW_LABEL_FONT);
	struct fep_rect seed = cworld_target_rect(width, height, CW_T_SEED);
	cworld_draw_field(seed, focus == CW_T_SEED);
	cworld_draw_boxed_text(seed, form.seed[0] ? form.seed : "Random",
						   field_font, form.seed[0] == '\0');
	cworld_draw_button(cworld_target_rect(width, height, CW_T_NEW_SEED),
					   "New seed", button_font, focus == CW_T_NEW_SEED, true);
	cworld_label(&c, CW_SEED_INFO_Y, "Leave blank for a random seed",
				 CW_HINT_FONT);

	/* Game mode: two buttons with the pack's own block icons (cobblestone 4,
	 * crafting table 58, as the Load list draws them); the chosen one has the
	 * approved tan outline. */
	cworld_label(&c, CW_MODE_LABEL_Y, "Game mode", CW_LABEL_FONT);
	for(int k = CW_T_MODE_SURVIVAL; k <= CW_T_MODE_CREATIVE; k++) {
		struct fep_rect r = cworld_target_rect(width, height, k);
		bool chosen
			= (k == CW_T_MODE_CREATIVE) == (form.mode == CWORLD_MODE_CREATIVE);
		if(chosen)
			fep_widget_based(r, focus == k, 0x50, 0x50, 0x48);
		else
			fep_widget(r, focus == k);
		fep_outline(r, GFX_GUI_SCALE / 2 > 0 ? GFX_GUI_SCALE / 2 : 1, 0, 0, 0,
					0x60);
		if(chosen)
			fep_outline(r, GFX_GUI_SCALE, 0xB8, 0xA8, 0x78, 0xFF);
		if(focus == k)
			fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
		int icon = cw_d(&c, CW_MODE_ICON);
		const char* label = cworld_form_mode_label(
			k == CW_T_MODE_SURVIVAL ? CWORLD_MODE_SURVIVAL : CWORLD_MODE_CREATIVE);
		int tw = gutil_font_width(label, button_font);
		int gap = cw_d(&c, 6);
		int x0 = r.x + (r.w - icon - gap - tw) / 2;
		struct item_data item = {.id = k == CW_T_MODE_SURVIVAL ? 4 : 58,
								 .count = 1};
		gutil_draw_item_sized(&item, x0, r.y + (r.h - icon) / 2, icon, 0);
		gutil_text_rgb(x0 + icon + gap, r.y + (r.h - button_font) / 2, label,
					   button_font, true, 0xF4, 0xF4, 0xED);
	}

	/* A FAILED create is disclosed on the panel, not swallowed. */
	if(create_failed) {
		int font = cw_d(&c, 13);
		char fitted[96];
		(void)fep_text_fit(fitted, sizeof(fitted),
						   "Couldn't create that world - try another name.",
						   font, cw_d(&c, CW_PANEL_W - 32));
		gutil_text_rgb(cw_x(&c, CW_LABEL_X), cw_d(&c, CW_FAIL_Y), fitted, font,
					   false, 0xC0, 0x30, 0x30);
	}

	/* The action row: Create world | Details | Cancel. */
	bool can_create = cworld_form_create_enabled(&form);
	cworld_draw_button(cworld_target_rect(width, height, CW_T_CREATE),
					   "Create world", cw_d(&c, 20), focus == CW_T_CREATE,
					   can_create);
	cworld_draw_button(cworld_target_rect(width, height, CW_T_DETAILS),
					   "Details", button_font, focus == CW_T_DETAILS, true);
	cworld_draw_button(cworld_target_rect(width, height, CW_T_CANCEL),
					   "Cancel", cw_d(&c, 20), focus == CW_T_CANCEL, true);

	cworld_footer(width, height);
}

/* THE DETAILS CARD. A real modal over the form: it owns the frame, shows the
 * folder, mode and seed Create would actually use, and Back is its one
 * control. Every line is wrapped to the body width (never cut). */
static void cworld_render_details(int width, int height) {
	struct cworld_canvas c = cworld_canvas_of(width, height);
	fep_fill(fep_rect(0, 0, width, height), 0, 0, 0, 0xA0);
	struct fep_rect card = fep_rect(cw_x(&c, CW_DET_X), cw_d(&c, CW_DET_Y),
									cw_d(&c, CW_DET_W), cw_d(&c, CW_DET_H));
	fep_paper(card);
	fep_bevel(card);

	gutil_text_rgb(cw_x(&c, CW_DET_TITLE_X), cw_d(&c, CW_DET_TITLE_Y),
				   "World details", cw_d(&c, CW_DET_TITLE_FONT), false, CW_INK);

	char lines[CW_DET_LINES_MAX][CW_DET_LINE_CAP];
	int item_of[CW_DET_LINES_MAX];
	int n = cworld_details_layout(width, height, lines, item_of, NULL);
	int font = cw_d(&c, CW_DET_BODY_FONT);
	int y = cw_d(&c, CW_DET_BODY_Y);
	for(int k = 0; k < n; k++) {
		if(k > 0 && item_of[k] != item_of[k - 1])
			y += cw_d(&c, CW_DET_ITEM_GAP);
		gutil_text_rgb(cw_x(&c, CW_DET_BODY_X), y, lines[k], font, false,
					   CW_INK);
		y += cw_d(&c, CW_DET_BODY_LINE);
	}

	cworld_draw_button(cworld_target_rect(width, height, CW_T_DETAILS_BACK),
					   "Back", cw_d(&c, 20), true, true);
}

/* Frontend-only editor: approved 640x480 geometry; parent form retains
 * commit/revert, raw seed semantics, live name/folder preview. (T05a owns the
 * big keys and the seed keypad; unchanged here.) */
static void cworld_render_modal(int width, int height) {
	struct cworld_canvas c = cworld_canvas_of(width, height);
	if(!fep_backdrop_at(width, height, 90, menu_motion_time, !form.editing)) gutil_bg();
	struct fep_rect panel = fep_rect(cw_x(&c,20),cw_d(&c,16),
		cw_d(&c,600),cw_d(&c,448));
	fep_paper(panel); fep_bevel(panel);
	const char* title = form.edit_row == CWORLD_ROW_NAME ? "World name" : "World seed";
	gutil_text_rgb(cw_x(&c,32),cw_d(&c,28),title,cw_d(&c,27),false,53,53,53);
	char count[32];
	size_t cap = osk.edit.beta_max > 0 ? (size_t)osk.edit.beta_max : osk.edit.buf_cap-1;
	snprintf(count,sizeof(count),"%u / %u",(unsigned)strlen(osk.edit.buf),(unsigned)cap);
	int count_font=cw_d(&c,14);
	gutil_text_rgb(cw_x(&c,608)-gutil_font_width(count,count_font),cw_d(&c,37),
		count,count_font,false,53,53,53);
	struct fep_rect field=fep_rect(cw_x(&c,32),cw_d(&c,76),cw_d(&c,576),cw_d(&c,52));
	fep_fill(field,0x19,0x1A,0x17,255);
	fep_outline(field,cw_d(&c,1),85,85,85,255);
	fep_fill(fep_rect(field.x,field.y+field.h-cw_d(&c,2),field.w,cw_d(&c,2)),184,188,178,255);
	int pad=cw_d(&c,16), font=cw_d(&c,20), caret_w=cw_d(&c,2);
	int room=field.w-2*pad-caret_w;
	size_t start=osk_frontend_reveal(&osk,font,room);
	int text_x=field.x+pad, text_y=field.y+(field.h-font)/2;
	gfx_scissor(true,(uint32_t)text_x,(uint32_t)field.y,
		(uint32_t)(field.w-2*pad),(uint32_t)field.h);
	gutil_text_rgb(text_x,text_y,osk.edit.buf+start,font,false,244,244,237);
	int caret_x=text_x;
	for(size_t i=start;i<osk.caret;i++) {
		char ch[2]={osk.edit.buf[i],0}; caret_x+=gutil_font_width(ch,font);
	}
	fep_fill(fep_rect(caret_x,text_y,caret_w,font),255,255,160,255);
	gfx_scissor(false,0,0,0,0);
	if(form.edit_row==CWORLD_ROW_SEED)
		gutil_text_rgb(cw_x(&c,32),cw_d(&c,136),"Leave empty for a random seed. Letters work too.",
			cw_d(&c,14),false,53,53,53);
	osk_frontend_render(&osk,width,height,true);
	struct input_player_frame prompts;
	input_prompt_frame(0,&prompts);
	osk_frontend_hint_row(&osk,&prompts,width,height);
}

static void screen_cworld_render2D(struct screen* s, int width, int height) {
	(void)s;
	int sc = GFX_GUI_SCALE;

	/* The scenic backdrop the whole front end shares, with the tiled-dirt
	 * fallback disclosed when the active pack ships no menu background. */
	if(!fep_backdrop_at(width, height, 90, menu_motion_time, !form.editing))
		gutil_bg();

	cworld_render_form(width, height);
	if(details_open)
		cworld_render_details(width, height);
	if(form.editing)
		cworld_render_modal(width, height);

	if(pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(pointer_x, pointer_y, glm_rad(pointer_angle), 0, 0,
							 256, 256, 48 * sc, 48 * sc);
	}
}

// menus_dwell hardware profile (menu_autopilot.c): see screen.h's doc
// comment. Reads the SAME `form.mode` the render/submit code already uses.
bool screen_cworld_mode_is_survival(void) {
	return form.mode == CWORLD_MODE_SURVIVAL;
}

// menus_dwell walker observers (menus-dwell-walker-1026): see screen.h.
bool screen_cworld_editing(void) {
	return form.editing;
}

bool screen_cworld_create_ready(void) {
	return !form.editing && !details_open
		&& cworld_focus_target() == CW_T_CREATE
		&& cworld_target_enabled(CW_T_CREATE);
}

/* ui-build-1026 T15: menus_dwell walker observers (screen.h). Reads only. */
_Static_assert((int)SCREEN_CWORLD_FOCUS_NAME == (int)CW_T_NAME
				   && (int)SCREEN_CWORLD_FOCUS_SEED == (int)CW_T_SEED
				   && (int)SCREEN_CWORLD_FOCUS_NEW_SEED == (int)CW_T_NEW_SEED
				   && (int)SCREEN_CWORLD_FOCUS_MODE_SURVIVAL == (int)CW_T_MODE_SURVIVAL
				   && (int)SCREEN_CWORLD_FOCUS_MODE_CREATIVE == (int)CW_T_MODE_CREATIVE
				   && (int)SCREEN_CWORLD_FOCUS_CREATE == (int)CW_T_CREATE
				   && (int)SCREEN_CWORLD_FOCUS_DETAILS == (int)CW_T_DETAILS
				   && (int)SCREEN_CWORLD_FOCUS_CANCEL == (int)CW_T_CANCEL
				   && (int)SCREEN_CWORLD_FOCUS_DETAILS_BACK == (int)CW_T_DETAILS_BACK,
			   "screen_cworld_focus drifted from cworld_target");
int screen_cworld_focus_target(void) { return cworld_focus_target(); }
bool screen_cworld_details_open(void) { return details_open; }
int screen_cworld_osk_profile(void) { return form.editing ? osk.profile : -1; }
int screen_cworld_osk_target(void) { return form.editing ? osk.target : -1; }
unsigned screen_cworld_text_signature(void) {
	unsigned h = 2166136261u;
	for(const char* p = form.name; *p; p++)
		h = (h ^ (unsigned char)*p) * 16777619u;
	h = (h ^ 0u) * 16777619u;
	for(const char* p = form.seed; *p; p++)
		h = (h ^ (unsigned char)*p) * 16777619u;
	return h;
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void screen_cworld_test_derive_folder(const char* name, char* out,
									  size_t out_cap) {
	cworld_derive_folder(name, out, out_cap);
}

bool screen_cworld_test_resolve_seed(const char* text, int64_t* out) {
	return cworld_resolve_seed(text, out);
}

/* Read-only observers. Nothing outside a CAVEWII_CLIENT_INTERFACE_TEST build
 * can see them, and none of them sets state. */
struct cworld_form screen_cworld_test_snapshot(bool* details_is_open) {
	if(details_is_open)
		*details_is_open = details_open;
	return form;
}

const char* screen_cworld_test_focus_name(void) {
	return cworld_target_names[cworld_focus_target()];
}

unsigned screen_cworld_test_last_submit(bool* seed_set, int64_t* seed) {
	if(seed_set)
		*seed_set = test_submit_seed_set;
	if(seed)
		*seed = test_submit_seed;
	return test_submits;
}

/* The Details card exactly as render2D lays it out at this size. */
int screen_cworld_test_details_lines(int width, int height,
									 char lines[][128], int max_lines,
									 bool* fits) {
	char got[CW_DET_LINES_MAX][CW_DET_LINE_CAP];
	int item_of[CW_DET_LINES_MAX];
	int n = cworld_details_layout(width, height, got, item_of, fits);
	for(int k = 0; k < n && k < max_lines; k++)
		snprintf(lines[k], 128, "%d|%.120s", item_of[k], got[k]);
	return n;
}
#endif

struct screen screen_create_world = {
	.reset = screen_cworld_reset,
	.update = screen_cworld_update,
	.render2D = screen_cworld_render2D,
	.render3D = NULL,
};
