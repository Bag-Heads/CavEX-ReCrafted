/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * THE MENU ACTIVATION RULE AS A TRUTH TABLE (push-0921/A5, Astra ruling 2).
 *
 * gutil_menu_activate_decide is the ONE decision every lane-A menu asks on an
 * activate press. It is pure -- no statics, no clock, no input, no draw -- so
 * it is testable as a table, which is the whole point of hoisting the rule out
 * of seven screens.
 *
 * Every expected value below is written BY HAND from the ruling's own wording
 * (quoted next to each block), never by running the implementation -- the
 * poisoned-golden rule, CODESTYLE.md section 4.2.
 *
 *   (1) pointer valid AND hovering a target T
 *         T disabled    -> NOTHING, the press CONSUMED (no fallback)
 *         T == selected -> ACTIVATE(T)
 *         T != selected -> FOCUS_ONLY(T), the press consumed
 *       except DIRECTIONAL origin after a stationary sprite: ACTIVATE the
 *       enabled selected target (Astra ruling 5 D2)
 *   (2) pointer valid, hovering nothing -- by the SELECTION'S ORIGIN (lead
 *       ruling R-A, which replaced "does the pointer own focus", a bit that
 *       flips with IR validity and was the whole of review A6-1), narrowed by
 *       R-E because a pointer user clicking the BACKGROUND of a sub-editor was
 *       firing its DEFAULT Cancel and discarding the typing (review A9-2)
 *         origin DIRECTIONAL        -> ACTIVATE(selected) iff real AND enabled
 *         origin POINTER or DEFAULT -> NOTHING
 *   (3) pointer invalid -> ACTIVATE(selected) iff it is real AND enabled;
 *       DEFAULT still activates here, so a Classic, a GameCube or a Remote
 *       pointed away can press A on a freshly opened screen.
 *   R-D: a DESTRUCTIVE selection additionally needs the player to have MEANT
 *        that target -- the sprite resting ON it, or a direction press having
 *        put the selection there. "Aim at Delete, lower the Remote, press A"
 *        is therefore NOTHING (review A9-1, the last descendant of C-1).
 *   R-B: a frame that ALSO carries a navigation edge decides NOTHING, the
 *        press consumed -- this replaces A5's displayed-snapshot rule.
 *
 * Part 1 is the hand-written table (every meaningful combination, named).
 * Part 2 sweeps ALL 6912 input combinations and asserts the rulings'
 * invariants on each one -- deliberately NOT a second implementation of the
 * rule, but the properties the rule exists to guarantee.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "graphics/gui_util.h"
#include "graphics/gfx_settings.h"
#include "graphics/texture_atlas.h"
#include "item/items.h"
#include "platform/gfx.h"
#include "platform/input_glyph.h"
#include "platform/input_inventory.h"

/* ---- link-only doubles: gui_util.c draws, this test does not ------------ */

struct tex_gfx texture_controller_pictures, texture_approved_controls,
	texture_controls, texture_font, texture_terrain, texture_gui_widgets;
static struct input_inventory fe_inventory;
const struct input_inventory* input_devices(void) {
	return &fe_inventory;
}
void input_prompt_frame(unsigned player, struct input_player_frame* frame) {
	(void)player;
	(void)frame;
	assert(false);
}
int gfx_width(void) {
	return 640;
}
int gfx_height(void) {
	return 480;
}
void gfx_blending(enum gfx_blend mode) { (void)mode; } /* hint strip / card A set MODE_BLEND */
void gfx_texture(bool enabled) {
	(void)enabled;
	assert(false);
}
void gfx_bind_texture(struct tex_gfx* texture) {
	(void)texture;
	assert(false);
}
uint8_t tex_atlas_lookup(enum tex_atlas_entry entry) {
	(void)entry;
	assert(false);
	return 0;
}
void tex_gfx_lookup(struct tex_gfx* tex, int x, int y, uint8_t* color) {
	(void)tex;
	(void)x;
	(void)y;
	memset(color, 255, 4);
}
struct item* item_get(struct item_data* item) {
	(void)item;
	assert(false);
	return NULL;
}
void gfx_depth_range(float near_plane, float far_plane) {
	(void)near_plane;
	(void)far_plane;
	assert(false);
}
void gfx_write_buffers(bool color, bool depth, bool test) {
	(void)color;
	(void)depth;
	(void)test;
	assert(false);
}
void gfx_draw_quads(size_t count, const int16_t* vertices, const uint8_t* color,
					const uint16_t* uv) {
	(void)count;
	(void)vertices;
	(void)color;
	(void)uv;
	assert(false);
}
void gfx_draw_quads_flt(size_t count, const float* vertices,
						const uint8_t* color, const float* uv) {
	(void)count;
	(void)vertices;
	(void)color;
	(void)uv;
	assert(false);
}

/* ---- part 1: the hand-written table ------------------------------------- */

/* Two arbitrary, distinct target ids. The helper only ever compares ids for
 * equality, so their VALUES carry no meaning -- which is itself a property
 * part 2 re-checks with a different pair. */
#define T_A 10
#define T_B 20
#define NONE GUTIL_MENU_TARGET_NONE
#define O_DEF GUTIL_MENU_ORIGIN_DEFAULT
#define O_DIR GUTIL_MENU_ORIGIN_DIRECTIONAL
#define O_PTR GUTIL_MENU_ORIGIN_POINTER

struct row {
	const char* name;
	/* inputs, in the struct's own order */
	bool pressed, navigated, valid;
	enum gutil_menu_origin origin;
	int hovered;
	bool hovered_enabled;
	int selected;
	bool selected_enabled, selected_destructive;
	/* expected outputs, written from the rulings */
	enum gutil_menu_activation action;
	int target;
	int new_selected;
	enum gutil_menu_origin new_origin;
	/* trailing input, zero (false) on every row that omits it: the whole
	 * dialog is a destructive confirmation (Astra D2-destructive ruling B) */
	bool destructive_dialog;
	/* trailing input, zero on every row that omits it: the HOVERED target is
	 * a select-only list row (ui-build-1026 T01) */
	bool hovered_selects_only;
};

static const struct row rows[] = {
	/* -- no press at all. The caller decides what a fresh edge is; a decision
	 * asked WITHOUT one must be inert in every field. */
	{"held, not an edge, over the selected target", false, false, true, O_PTR,
	 T_A, true, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A, O_PTR},
	{"held, not an edge, over a different target", false, false, true, O_DIR,
	 T_B, true, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A, O_DIR},

	/* -- R-B: navigation + activate in the SAME frame is NOTHING, the press
	 * consumed. It outranks every other case. */
	{"navigated this frame, hovering the selected target", true, true, true,
	 O_PTR, T_A, true, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A, O_PTR},
	{"navigated this frame, hovering another target", true, true, true, O_DIR,
	 T_B, true, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A, O_DIR},
	{"navigated this frame, over empty space, directional", true, true, true,
	 O_DIR, NONE, false, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A,
	 O_DIR},
	{"navigated this frame, pointer invalid", true, true, false, O_DIR, NONE,
	 false, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A, O_DIR},

	/* -- (3) pointer invalid: ACTIVATE(selected) if it is real AND enabled.
	 * DEFAULT activates here on purpose -- a Classic, a GameCube or a Remote
	 * pointed away must be able to press A on a freshly opened screen -- and
	 * that is what R-E keeps out of rule (2) only. */
	{"invalid, selected enabled, origin pointer", true, false, false, O_PTR,
	 NONE, false, T_A, true, false, GUTIL_MENU_ACTIVATE, T_A, T_A, O_PTR},
	{"invalid, selected enabled, origin directional", true, false, false,
	 O_DIR, NONE, false, T_A, true, false, GUTIL_MENU_ACTIVATE, T_A, T_A,
	 O_DIR},
	{"invalid, selected enabled, origin default", true, false, false, O_DEF,
	 NONE, false, T_A, true, false, GUTIL_MENU_ACTIVATE, T_A, T_A, O_DEF},
	{"invalid, selected disabled", true, false, false, O_DIR, NONE, false, T_A,
	 false, false, GUTIL_MENU_NOTHING, NONE, T_A, O_DIR},
	{"invalid, no selection", true, false, false, O_DEF, NONE, false, NONE,
	 false, false, GUTIL_MENU_NOTHING, NONE, NONE, O_DEF},
	{"invalid, stale hover on another target is ignored", true, false, false,
	 O_PTR, T_B, true, T_A, true, false, GUTIL_MENU_ACTIVATE, T_A, T_A, O_PTR},

	/* -- R-D (review A9-1): a DESTRUCTIVE selection activates only when the
	 * player MEANT that target -- the sprite is resting on it, or a direction
	 * press put it there. "Aim at Delete, lower the Remote (IR goes invalid),
	 * press A" was the last live descendant of C-1. */
	{"R-D: invalid, DESTRUCTIVE selection the POINTER armed", true, false,
	 false, O_PTR, NONE, false, T_A, true, true, GUTIL_MENU_NOTHING, NONE, T_A,
	 O_PTR},
	{"R-D: invalid, DESTRUCTIVE selection from the DEFAULT", true, false,
	 false, O_DEF, NONE, false, T_A, true, true, GUTIL_MENU_NOTHING, NONE, T_A,
	 O_DEF},
	{"R-D: invalid, DESTRUCTIVE selection the D-PAD chose", true, false, false,
	 O_DIR, NONE, false, T_A, true, true, GUTIL_MENU_ACTIVATE, T_A, T_A,
	 O_DIR},
	{"R-D: valid, empty space, DESTRUCTIVE, the D-PAD chose", true, false,
	 true, O_DIR, NONE, false, T_A, true, true, GUTIL_MENU_ACTIVATE, T_A, T_A,
	 O_DIR},
	{"R-D: valid, empty space, DESTRUCTIVE, the POINTER armed", true, false,
	 true, O_PTR, NONE, false, T_A, true, true, GUTIL_MENU_NOTHING, NONE, T_A,
	 O_PTR},
	/* The sprite RESTING on the destructive target is the other way the
	 * player can mean it: rule (1) is unchanged, R-D is satisfied by the
	 * hover itself. */
	{"R-D: valid, hovering the DESTRUCTIVE selected target", true, false, true,
	 O_PTR, T_A, true, T_A, true, true, GUTIL_MENU_ACTIVATE, T_A, T_A, O_PTR},
	{"R-D: valid, hovering ANOTHER target, destructive selection", true, false,
	 true, O_PTR, T_B, true, T_A, true, true, GUTIL_MENU_FOCUS_ONLY, T_B, T_B,
	 O_PTR},

	/* -- (2) pointer valid, hovering NOTHING. R-A asks the origin; R-E then
	 * narrows it to DIRECTIONAL alone, because a pointer user clicking the
	 * BACKGROUND of a sub-editor was firing its DEFAULT Cancel and discarding
	 * the typing (review A9-2). */
	{"valid, empty space, origin POINTER", true, false, true, O_PTR, NONE,
	 false, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A, O_PTR},
	{"valid, empty space, origin POINTER, no selection", true, false, true,
	 O_PTR, NONE, false, NONE, false, false, GUTIL_MENU_NOTHING, NONE, NONE,
	 O_PTR},
	{"R-E: valid, empty space, origin DEFAULT", true, false, true, O_DEF, NONE,
	 false, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A, O_DEF},
	{"valid, empty space, origin DIRECTIONAL", true, false, true, O_DIR, NONE,
	 false, T_A, true, false, GUTIL_MENU_ACTIVATE, T_A, T_A, O_DIR},
	{"valid, empty space, origin DIRECTIONAL, selected disabled", true, false,
	 true, O_DIR, NONE, false, T_A, false, false, GUTIL_MENU_NOTHING, NONE,
	 T_A, O_DIR},
	{"valid, empty space, origin DIRECTIONAL, no selection", true, false, true,
	 O_DIR, NONE, false, NONE, false, false, GUTIL_MENU_NOTHING, NONE, NONE,
	 O_DIR},

	/* -- (1) pointer valid AND hovering a target. Unchanged by R-A, R-D and
	 * R-E: the sprite resting on a control is the whole of the case, and
	 * neither ownership nor origin is consulted -- that is what makes a
	 * validity flicker unable to change the answer. */
	{"valid, hovering the selected target, origin pointer", true, false, true,
	 O_PTR, T_A, true, T_A, true, false, GUTIL_MENU_ACTIVATE, T_A, T_A, O_PTR},
	{"valid, hovering the selected target, origin directional", true, false,
	 true, O_DIR, T_A, true, T_A, true, false, GUTIL_MENU_ACTIVATE, T_A, T_A,
	 O_DIR},
	{"valid, hovering ANOTHER target, origin directional", true, false, true,
	 O_DIR, T_B, true, T_A, true, false, GUTIL_MENU_ACTIVATE, T_A, T_A,
	 O_DIR},
	{"valid, hovering ANOTHER target, origin pointer", true, false, true,
	 O_PTR, T_B, true, T_A, true, false, GUTIL_MENU_FOCUS_ONLY, T_B, T_B,
	 O_PTR},
	{"valid, hovering ANOTHER target, origin default", true, false, true,
	 O_DEF, T_B, true, T_A, true, false, GUTIL_MENU_FOCUS_ONLY, T_B, T_B,
	 O_PTR},
	{"valid, hovering a target with NO selection", true, false, true, O_DEF,
	 T_B, true, NONE, false, false, GUTIL_MENU_FOCUS_ONLY, T_B, T_B, O_PTR},
	/* "A pointer over a disabled control must consume activation, not fall
	 * back to another selected control" (Astra ruling 2 item 2). */
	{"valid, hovering a DISABLED target, another selected", true, false, true,
	 O_DIR, T_B, false, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A,
	 O_DIR},
	{"valid, hovering a DISABLED target, origin pointer", true, false, true,
	 O_PTR, T_B, false, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A,
	 O_PTR},
	{"valid, hovering the selected target, now DISABLED", true, false, true,
	 O_DIR, T_A, false, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A,
	 O_DIR},
	{"valid, hovering a DISABLED target with no selection", true, false, true,
	 O_DEF, T_B, false, NONE, false, false, GUTIL_MENU_NOTHING, NONE, NONE,
	 O_DEF},
	/* Both flags describe the SAME id here, so they cannot honestly disagree;
	 * the hover is the live reading and wins. Pinned so the tie-break is a
	 * decision on the record rather than an accident of branch order. */
	{"valid, hovering the selected target, stale disabled selection flag",
	 true, false, true, O_DIR, T_A, true, T_A, false, false,
	 GUTIL_MENU_ACTIVATE, T_A, T_A, O_DIR},

	/* -- A6-1 and A9-1 as pairs whose only difference is the validity
	 * transition. The first pair is non-destructive, and that difference is
	 * the ONE the rulings keep. The second is the delete confirm, where R-D
	 * now makes both sides NOTHING. */
	{"A6-1: pointer-armed selection, sprite on empty space, IR valid", true,
	 false, true, O_PTR, NONE, false, T_A, true, false, GUTIL_MENU_NOTHING,
	 NONE, T_A, O_PTR},
	{"A6-1 control: the same sprite path with IR invalid (rule (3))", true,
	 false, false, O_PTR, NONE, false, T_A, true, false, GUTIL_MENU_ACTIVATE,
	 T_A, T_A, O_PTR},
	{"A9-1: aim at Delete, IR still valid, press A", true, false, true, O_PTR,
	 NONE, false, T_A, true, true, GUTIL_MENU_NOTHING, NONE, T_A, O_PTR},
	{"A9-1: aim at Delete, LOWER THE REMOTE, press A", true, false, false,
	 O_PTR, NONE, false, T_A, true, true, GUTIL_MENU_NOTHING, NONE, T_A,
	 O_PTR},

	/* -- Astra D2-destructive ruling (option B, 2026-09-24): in a DESTRUCTIVE
	 * CONFIRMATION a valid sprite on a button OTHER than the selected one
	 * makes the first A a FOCUS_ONLY onto the hovered button, whatever the
	 * origin -- D2's DIRECTIONAL override does not apply. Both mismatch
	 * directions (T_A = safe, T_B = destructive and the reverse). */
	{"B: dialog, ring on SAFE (directional), sprite on DESTRUCTIVE", true,
	 false, true, O_DIR, T_B, true, T_A, true, false, GUTIL_MENU_FOCUS_ONLY,
	 T_B, T_B, O_PTR, true},
	{"B: dialog, ring on DESTRUCTIVE (directional), sprite on SAFE", true,
	 false, true, O_DIR, T_B, true, T_A, true, true, GUTIL_MENU_FOCUS_ONLY,
	 T_B, T_B, O_PTR, true},
	{"B: dialog, DEFAULT safe ring, sprite on DESTRUCTIVE", true, false, true,
	 O_DEF, T_B, true, T_A, true, false, GUTIL_MENU_FOCUS_ONLY, T_B, T_B,
	 O_PTR, true},
	{"B: dialog, pointer-armed ring, sprite on the other button", true, false,
	 true, O_PTR, T_B, true, T_A, true, true, GUTIL_MENU_FOCUS_ONLY, T_B, T_B,
	 O_PTR, true},
	/* the NEXT fresh press, focus reconciled (hovered == selected) */
	{"B: dialog, reconciled DESTRUCTIVE, fresh press activates", true, false,
	 true, O_PTR, T_B, true, T_B, true, true, GUTIL_MENU_ACTIVATE, T_B, T_B,
	 O_PTR, true},
	/* -- ui-build-1026 T01: a SELECT-ONLY list row under the sprite (Load
	 * world list, Texture packs list). Activating such a row only selects it,
	 * so ONE click chooses it instead of a FOCUS_ONLY first. Everything that
	 * guards Play/Apply/Delete is unchanged: a fresh edge, R-B, a disabled
	 * row consuming, D2 for a directional ring, ruling B in a destructive
	 * confirmation, and the selection/origin left to the caller. */
	{"T01: row, ring on Play (DEFAULT), one click chooses the row", true,
	 false, true, O_DEF, T_B, true, T_A, true, false, GUTIL_MENU_ACTIVATE, T_B,
	 T_A, O_DEF, false, true},
	{"T01: row, ring pointer-armed elsewhere, one click chooses", true, false,
	 true, O_PTR, T_B, true, T_A, true, false, GUTIL_MENU_ACTIVATE, T_B, T_A,
	 O_PTR, false, true},
	{"T01: row, no selection at all, one click chooses", true, false, true,
	 O_DEF, T_B, true, NONE, false, false, GUTIL_MENU_ACTIVATE, T_B, NONE,
	 O_DEF, false, true},
	{"T01: row, destructive selection elsewhere, the ROW is chosen", true,
	 false, true, O_PTR, T_B, true, T_A, true, true, GUTIL_MENU_ACTIVATE, T_B,
	 T_A, O_PTR, false, true},
	{"T01: row already selected, unchanged ACTIVATE", true, false, true,
	 O_PTR, T_B, true, T_B, true, false, GUTIL_MENU_ACTIVATE, T_B, T_B, O_PTR,
	 false, true},
	{"T01: row HELD, not an edge: nothing", false, false, true, O_DEF, T_B,
	 true, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A, O_DEF, false,
	 true},
	{"T01: row + navigation same frame: nothing (R-B)", true, true, true,
	 O_PTR, T_B, true, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A, O_PTR,
	 false, true},
	{"T01: DISABLED row still consumes", true, false, true, O_PTR, T_B, false,
	 T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A, O_PTR, false, true},
	{"T01: D-pad ring + stationary sprite on a row: D2 still fires the ring",
	 true, false, true, O_DIR, T_B, true, T_A, true, false,
	 GUTIL_MENU_ACTIVATE, T_A, T_A, O_DIR, false, true},
	{"T01: destructive DIALOG ignores the row flag (ruling B)", true, false,
	 true, O_PTR, T_B, true, T_A, true, false, GUTIL_MENU_FOCUS_ONLY, T_B, T_B,
	 O_PTR, true, true},
	{"T01: IR invalid, stale hover on a row is ignored", true, false, false,
	 O_PTR, T_B, true, T_A, true, false, GUTIL_MENU_ACTIVATE, T_A, T_A, O_PTR,
	 false, true},
	/* holding A is no edge: nothing */
	{"B: dialog, HELD A after the focus move", false, false, true, O_PTR, T_B,
	 true, T_B, true, true, GUTIL_MENU_NOTHING, NONE, T_B, O_PTR, true},
	/* no mismatch: the existing rules and the safe default are untouched */
	{"B: dialog, sprite on nothing, D-pad safe answer (D1)", true, false,
	 true, O_DIR, NONE, false, T_A, true, false, GUTIL_MENU_ACTIVATE, T_A, T_A,
	 O_DIR, true},
	{"B: dialog, IR invalid, D-pad destructive answer", true, false, false,
	 O_DIR, NONE, false, T_B, true, true, GUTIL_MENU_ACTIVATE, T_B, T_B, O_DIR,
	 true},
	{"B: dialog, hovered DISABLED still consumes", true, false, true, O_DIR,
	 T_B, false, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A, O_DIR, true},
	{"B: dialog, navigate + A in one frame is still nothing", true, true,
	 true, O_DIR, T_B, true, T_A, true, false, GUTIL_MENU_NOTHING, NONE, T_A,
	 O_DIR, true},
};

static const char* action_name(enum gutil_menu_activation a) {
	switch(a) {
		case GUTIL_MENU_NOTHING: return "NOTHING";
		case GUTIL_MENU_FOCUS_ONLY: return "FOCUS_ONLY";
		case GUTIL_MENU_ACTIVATE: return "ACTIVATE";
		default: return "?";
	}
}

/* Astra ruling 5 D2: a direction press owns A while the IR sprite stays
 * inside its movement threshold. A subsequent pointer move that assigns a
 * row changes the origin to POINTER, so that row owns the next press. */
static void last_input_wins(void) {
	struct gutil_menu_focus focus = {.initialized = true};
	assert(!gutil_menu_pointer_focus(&focus, true, 100, 100, true));
	assert(!gutil_menu_pointer_focus(&focus, true,
		100 + GUTIL_MENU_POINTER_JITTER - GFX_GUI_SCALE, 100, false));
	struct gutil_menu_activate_state in = {
		.pressed = true, .pointer_valid = true,
		.selection_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL,
		.hovered = 202, .hovered_enabled = true,
		.selected = 101, .selected_enabled = true,
	};
	struct gutil_menu_decision d = gutil_menu_activate_decide(in);
	assert(d.action == GUTIL_MENU_ACTIVATE && d.target == 101);
	assert(gutil_menu_pointer_focus(&focus, true,
		100 + GUTIL_MENU_POINTER_JITTER + GFX_GUI_SCALE, 100, false));
	in.selection_origin = GUTIL_MENU_ORIGIN_POINTER;
	d = gutil_menu_activate_decide(in);
	assert(d.action == GUTIL_MENU_FOCUS_ONLY && d.selected == 202);
	in.selected = d.selected;
	d = gutil_menu_activate_decide(in);
	assert(d.action == GUTIL_MENU_ACTIVATE && d.target == 202);
	puts("PASS last input wins: stationary sprite yields to ring; moved sprite retakes focus");
}

static void table(void) {
	for(size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
		const struct row* r = &rows[k];
		struct gutil_menu_activate_state in = {
			.pressed = r->pressed,
			.navigated = r->navigated,
			.pointer_valid = r->valid,
			.selection_origin = r->origin,
			.hovered = r->hovered,
			.hovered_enabled = r->hovered_enabled,
			.selected = r->selected,
			.selected_enabled = r->selected_enabled,
			.selected_destructive = r->selected_destructive,
			.destructive_dialog = r->destructive_dialog,
			.hovered_selects_only = r->hovered_selects_only,
		};
		struct gutil_menu_decision d = gutil_menu_activate_decide(in);
		if(d.action != r->action || d.target != r->target
		   || d.selected != r->new_selected
		   || d.selection_origin != r->new_origin) {
			printf("FAIL row %u \"%s\": expected %s target %d selected %d "
				   "origin %d, got %s target %d selected %d origin %d\n",
				   (unsigned)k, r->name, action_name(r->action), r->target,
				   r->new_selected, (int)r->new_origin, action_name(d.action),
				   d.target, d.selected, (int)d.selection_origin);
			assert(false && "the activation rule's truth table");
		}
		/* Purity: the same inputs answer the same way. */
		struct gutil_menu_decision again = gutil_menu_activate_decide(in);
		assert(again.action == d.action && again.target == d.target
			   && again.selected == d.selected
			   && again.selection_origin == d.selection_origin
			   && "the decision is not a pure function of its inputs");
	}
	printf("PASS truth table: %u hand-written rows of the activation rule "
		   "(Astra ruling 2 plus the lead rulings R-A / R-B / R-D / R-E)\n",
		   (unsigned)(sizeof(rows) / sizeof(rows[0])));
}

/* ---- part 2: every combination, against the rulings' INVARIANTS ---------- */

/* Not a second implementation of the rule: these are the guarantees the rule
 * was written to buy, each asserted on all 6912 input combinations. */
static void sweep(void) {
	static const int ids[3] = {NONE, 101, 202}; /* a different id pair */
	static const enum gutil_menu_origin origins[3] = {O_DEF, O_DIR, O_PTR};
	unsigned seen = 0, activates = 0, focus_only = 0, nothing = 0;
	unsigned validity_differs = 0, destructive_refusals = 0;
	unsigned dialog_reconciles = 0, row_one_clicks = 0;

	for(int pressed = 0; pressed < 2; pressed++)
	for(int navigated = 0; navigated < 2; navigated++)
	for(int valid = 0; valid < 2; valid++)
	for(int o = 0; o < 3; o++)
	for(int h = 0; h < 3; h++)
	for(int he = 0; he < 2; he++)
	for(int sel = 0; sel < 3; sel++)
	for(int se = 0; se < 2; se++)
	for(int sd = 0; sd < 2; sd++)
	for(int dd = 0; dd < 2; dd++)
	for(int so = 0; so < 2; so++) {
		struct gutil_menu_activate_state in = {
			.pressed = pressed != 0,
			.navigated = navigated != 0,
			.pointer_valid = valid != 0,
			.selection_origin = origins[o],
			.hovered = ids[h],
			.hovered_enabled = he != 0,
			.selected = ids[sel],
			.selected_enabled = se != 0,
			.selected_destructive = sd != 0,
			.destructive_dialog = dd != 0,
			.hovered_selects_only = so != 0,
		};
		struct gutil_menu_decision d = gutil_menu_activate_decide(in);
		seen++;

		/* I. No press, no decision. */
		if(!in.pressed)
			assert(d.action == GUTIL_MENU_NOTHING && d.target == NONE
				   && d.selected == in.selected
				   && d.selection_origin == in.selection_origin
				   && "a frame with no fresh press decided something");

		/* II. R-B: a frame that also navigated decides NOTHING, and changes
		 * neither the selection nor its origin. */
		if(in.pressed && in.navigated)
			assert(d.action == GUTIL_MENU_NOTHING && d.target == NONE
				   && d.selected == in.selected
				   && d.selection_origin == in.selection_origin
				   && "navigation and activation in one frame activated "
					  "something");

		/* III. Only FOCUS_ONLY moves the selection, it moves it exactly onto
		 * its own target, and it is the ONLY thing that makes the origin
		 * POINTER (R-A). */
		if(d.action == GUTIL_MENU_FOCUS_ONLY)
			assert(d.selected == d.target && d.target != in.selected
				   && d.target != NONE
				   && d.selection_origin == GUTIL_MENU_ORIGIN_POINTER
				   && "FOCUS_ONLY did not put the selection, and its origin, "
					  "on its own target");
		else
			assert(d.selected == in.selected
				   && d.selection_origin == in.selection_origin
				   && "a non-FOCUS_ONLY decision moved the selection or its "
					  "origin");

		/* IV. NOTHING names no target. */
		if(d.action == GUTIL_MENU_NOTHING)
			assert(d.target == NONE && "NOTHING named a target");

		/* V. Nothing ever activates a target that is not real, or disabled. */
		if(d.action == GUTIL_MENU_ACTIVATE) {
			bool by_hover = in.pointer_valid && in.hovered != NONE
				&& !(!in.destructive_dialog && in.selection_origin == O_DIR
					 && in.hovered != in.selected && in.hovered_enabled);
			bool ok = by_hover
				? (d.target == in.hovered && in.hovered_enabled)
				: (d.target == in.selected && in.selected_enabled);
			assert(d.target != NONE && "activated a non-target");
			assert(ok && "activated a disabled or unhovered target");
		}

		/* VI. A pointer/default selection still needs a focus-only press when
		 * the sprite rests on another target -- unless that target is a
		 * select-only list row (T01), which is then the ONLY thing that may
		 * activate: never the selection the sprite is not on. */
		if(in.pointer_valid && in.hovered != NONE && in.hovered != in.selected
		   && in.selection_origin != O_DIR) {
			if(in.hovered_selects_only && !in.destructive_dialog)
				assert((d.action != GUTIL_MENU_ACTIVATE
						|| d.target == in.hovered)
					   && "a click on a select-only row activated a target "
						  "other than that row");
			else
				assert(d.action != GUTIL_MENU_ACTIVATE
					   && "a valid pointer on a different target activated "
						  "something");
		}

		/* VI-T01. THE ROW-FLAG INVARIANT. Flipping hovered_selects_only may
		 * change the outcome in EXACTLY ONE class: a fresh, non-navigating
		 * press, valid pointer on an ENABLED row that is not the selection,
		 * origin POINTER or DEFAULT, outside a destructive confirmation --
		 * FOCUS_ONLY(row) without the flag, ACTIVATE(row) with it, the
		 * selection and its origin untouched. Every other input answers the
		 * same either way (so D2, R-B, R-D, ruling B, VII and VIII hold). */
		{
			struct gutil_menu_activate_state other = in;
			other.hovered_selects_only = !in.hovered_selects_only;
			struct gutil_menu_decision od = gutil_menu_activate_decide(other);
			bool may_differ = in.pressed && !in.navigated && in.pointer_valid
				&& in.hovered != NONE && in.hovered_enabled
				&& in.hovered != in.selected && in.selection_origin != O_DIR
				&& !in.destructive_dialog;
			if(may_differ) {
				const struct gutil_menu_decision* on
					= in.hovered_selects_only ? &d : &od;
				const struct gutil_menu_decision* off
					= in.hovered_selects_only ? &od : &d;
				assert(off->action == GUTIL_MENU_FOCUS_ONLY
					   && off->target == in.hovered
					   && on->action == GUTIL_MENU_ACTIVATE
					   && on->target == in.hovered
					   && on->selected == in.selected
					   && on->selection_origin == in.selection_origin
					   && "the one sanctioned row-flag difference is not the "
						  "one that occurred");
				row_one_clicks++;
			} else {
				assert(od.action == d.action && od.target == d.target
					   && od.selected == d.selected
					   && od.selection_origin == d.selection_origin
					   && "the select-only row flag changed an outcome outside "
						  "its one class");
			}
		}

		/* VI-B. Astra D2-destructive ruling B: in a destructive confirmation
		 * a valid sprite on an enabled button other than the selected one
		 * NEVER activates -- the press is a FOCUS_ONLY onto that button,
		 * whatever the origin. */
		if(in.destructive_dialog && in.pressed && !in.navigated
		   && in.pointer_valid && in.hovered != NONE && in.hovered_enabled
		   && in.hovered != in.selected) {
			assert(d.action == GUTIL_MENU_FOCUS_ONLY && d.target == in.hovered
				   && "a destructive confirmation acted on a sprite/ring "
					  "mismatch");
			dialog_reconciles++;
		}
		/* ... and outside a mismatch the dialog flag changes nothing. */
		if(!(in.pointer_valid && in.hovered != NONE && in.hovered_enabled
			 && in.hovered != in.selected)) {
			struct gutil_menu_activate_state flip = in;
			flip.destructive_dialog = !in.destructive_dialog;
			struct gutil_menu_decision fd = gutil_menu_activate_decide(flip);
			assert(fd.action == d.action && fd.target == d.target
				   && fd.selected == d.selected
				   && fd.selection_origin == d.selection_origin
				   && "the destructive-dialog flag changed a no-mismatch press");
		}

		/* VII. A hovered disabled control consumes the press. */
		if(in.pointer_valid && in.hovered != NONE && !in.hovered_enabled)
			assert(d.action == GUTIL_MENU_NOTHING
				   && "a disabled control under the pointer fell through");

		/* VIII. R-A + R-E: while the pointer is VALID and resting on nothing,
		 * only a selection the PLAYER moved to with a direction press is
		 * activatable. */
		if(in.pointer_valid && in.hovered == NONE
		   && in.selection_origin != GUTIL_MENU_ORIGIN_DIRECTIONAL)
			assert(d.action == GUTIL_MENU_NOTHING
				   && "a click on empty space fired a selection the player "
					  "did not move to");

		/* IX. R-D: a DESTRUCTIVE target activates only when the player meant
		 * THAT target -- the sprite is resting on it, or a direction press
		 * put the selection there. */
		if(d.action == GUTIL_MENU_ACTIVATE && in.selected_destructive
		   && d.target == in.selected)
		{
			bool meant = (in.pointer_valid && in.hovered == in.selected)
				|| in.selection_origin == GUTIL_MENU_ORIGIN_DIRECTIONAL;
			assert(meant
				   && "a destructive target activated without the player "
					  "meaning it");
		}
		if(in.pressed && !in.navigated && in.hovered == NONE
		   && in.selected != NONE && in.selected_enabled
		   && in.selected_destructive
		   && in.selection_origin != GUTIL_MENU_ORIGIN_DIRECTIONAL) {
			assert(d.action == GUTIL_MENU_NOTHING
				   && "R-D let a destructive target fire with the sprite on "
					  "nothing");
			destructive_refusals++;
		}

		/* X. THE VALIDITY-FLIP INVARIANT. With the sprite on NO target,
		 * flipping pointer_valid may change the outcome in EXACTLY ONE class,
		 * written out here and nowhere else:
		 *
		 *     pressed, not navigated, hovered == NONE,
		 *     selected real AND enabled AND **NOT destructive**,
		 *     origin == POINTER or DEFAULT
		 *         valid   -> NOTHING            (R-A + R-E, rule (2))
		 *         invalid -> ACTIVATE(selected) (rule (3), kept on purpose so
		 *                                        a pad with no pointer can
		 *                                        press A on a fresh screen)
		 *
		 * In particular the DESTRUCTIVE version of that class is NOTHING on
		 * BOTH sides (R-D, review A9-1), and every other combination with
		 * hovered == NONE has an identical action, target, selection and
		 * origin across the flip. */
		if(in.hovered == NONE) {
			struct gutil_menu_activate_state other = in;
			other.pointer_valid = !in.pointer_valid;
			struct gutil_menu_decision od = gutil_menu_activate_decide(other);
			bool may_differ = in.pressed && !in.navigated
				&& in.selected != NONE && in.selected_enabled
				&& !in.selected_destructive
				&& in.selection_origin != GUTIL_MENU_ORIGIN_DIRECTIONAL;
			if(may_differ) {
				const struct gutil_menu_decision* v = in.pointer_valid ? &d : &od;
				const struct gutil_menu_decision* i = in.pointer_valid ? &od : &d;
				assert(v->action == GUTIL_MENU_NOTHING && v->target == NONE
					   && i->action == GUTIL_MENU_ACTIVATE
					   && i->target == in.selected
					   && "the one sanctioned validity difference is not the "
						  "one that occurred");
				validity_differs++;
			} else {
				assert(od.action == d.action && od.target == d.target
					   && od.selected == d.selected
					   && od.selection_origin == d.selection_origin
					   && "an IR validity transition alone changed the "
						  "OUTCOME");
			}
		}

		switch(d.action) {
			case GUTIL_MENU_ACTIVATE: activates++; break;
			case GUTIL_MENU_FOCUS_ONLY: focus_only++; break;
			default: nothing++; break;
		}
	}

	assert(seen == 6912 && "the sweep did not cover every combination");
	assert(row_one_clicks > 0 && "the T01 row-flag invariant never fired");
	assert(dialog_reconciles > 0 && "the ruling-B invariant never fired");
	assert(activates > 0 && focus_only > 0 && nothing > 0
		   && "a whole outcome class never occurred -- the sweep is degenerate");
	assert(validity_differs > 0 && destructive_refusals > 0
		   && "an invariant never fired on any input -- it is vacuous");
	printf("PASS sweep: %u combinations, %u ACTIVATE / %u FOCUS_ONLY / %u "
		   "NOTHING; R-D refused %u destructive presses, the ONE sanctioned "
		   "validity difference occurred %u times and no other outcome "
		   "depended on validity; the select-only row flag changed exactly "
		   "%u outcomes, all FOCUS_ONLY -> ACTIVATE(that row)\n",
		   seen, activates, focus_only, nothing, destructive_refusals,
		   validity_differs, row_one_clicks);
}

int main(void) {
	last_input_wins();
	table();
	sweep();
	puts("PASS menu activation rule");
	return 0;
}
