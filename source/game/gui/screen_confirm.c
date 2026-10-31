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
#include <stdio.h>
#include <string.h>

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/input_owner.h"
#include "../game_state.h"
#include "screen.h"

/* Beta GuiYesNo [740c583]: dirt background, message1 centered at y=70
 * (0xFFFFFF), message2 at y=90, two 150-wide GuiSmallButtons at
 * (w/2-155, h/6+96) and (w/2-155+160, h/6+96). actionPerformed hands
 * `id == 0` (the yes button) to the parent. Our shape: the OPENER stores
 * a callback; the callback owns the follow-up screen_set (exactly like
 * Beta's deleteWorld -> displayGuiScreen). Input is pointer + d-pad
 * dual-drive per the Wiimote floor rule; IB_INVENTORY/IB_HOME = the no
 * button (a confirm screen never destroys on back-out).
 *
 * DESTRUCTIVE CALLERS (checked 2026-09-21, every screen_confirm_setup in the
 * tree): screen_select_world.c's sworld_delete_selected -- "Delete" / "Cancel",
 * cb = sworld_delete_confirmed, YES erases a world directory; and
 * screen_multiplayer_setup.c's mps_delete_selected -- "Remove" / "Cancel",
 * cb = mps_delete_confirmed, YES removes a saved server AND persists the list.
 * Both put the destruction on YES, and both now SAY SO at the call site by
 * passing SCREEN_CONFIRM_ANSWER_NO as the safe answer. Third caller
 * (IMP-QUIT-UNSAVABLE, 2026-10-21): screen_pause.c's
 * screen_pause_exit_unsavable -- "Quit anyway" / "Try again", YES gives up
 * the named unsavable chunks' unsaved changes, NO (safe) retries the save.
 * The convert-notice-1024 three-line variant (screen_confirm_setup3) was
 * removed by ui-build-1026/T09: the Beta-world question is its own screen now
 * (screen_convert_backup.c), and every dialog here is the Beta two-line
 * GuiYesNo again (tests/test_import_convert_notice.c pins the layout). */

#define CONFIRM_BTN_W 150

static char confirm_msg1[128];
static char confirm_msg2[128];
static char confirm_yes[32];
static char confirm_no[32];
static void (*confirm_cb)(bool yes);

/* Which answer changes nothing. Astra ruling 2 item 5 asked for it "by
 * meaning, not an unexplained index"; review A6-5 pointed out that a constant
 * inside this screen is not the caller saying so, and a future dialog whose
 * safe answer is YES would silently get the wrong default. It is now the
 * OPENER's word: screen_confirm_setup's safe_answer parameter, declared in
 * screen.h next to enum screen_confirm_answer. */
static enum screen_confirm_answer confirm_safe_answer
	= SCREEN_CONFIRM_ANSWER_NO;
static int selected; /* a screen_confirm_answer */
/* WHO CHOSE the highlight -- see screen_title.c's note (lead ruling R-A).
 * On THIS screen it is what keeps a pointer-armed Delete from being answered
 * by a press over empty space after an IR blip (review A6-1). */
static enum gutil_menu_origin confirm_origin;
/* Astra menu review item 13: hover used to assign `selected` on EVERY update
 * while the pointer was merely VALID, so a resting Wii Remote could pull the
 * selection back onto the DESTRUCTIVE button right after the player stepped
 * off it. Same movement-based arbitration screen_options.c uses.
 *
 * A3 C-1 then found the other half, and round 2 answered it by re-arming the
 * arbitration on every invalid->valid transition. Astra ruling 2 REMOVED that
 * re-arm: a reacquired pointer is deliberately a non-owner (the helper's rule,
 * pinned by tests/test_options_env.c), and what makes the destructive answer
 * safe is now the ACTIVATION rule instead -- the press is decided against what
 * the sprite rests on, so a sprite on Cancel can never answer Delete, with no
 * dependence on IR validity timing at all. */
static struct gutil_menu_focus focus;
static bool pointer_available;
static float pointer_x, pointer_y, pointer_angle;

void screen_confirm_setup(const char* msg1, const char* msg2,
						  const char* yes_label, const char* no_label,
						  enum screen_confirm_answer safe_answer,
						  void (*cb)(bool yes)) {
	assert(msg1 && msg2 && yes_label && no_label && cb);
	snprintf(confirm_msg1, sizeof(confirm_msg1), "%s", msg1);
	snprintf(confirm_msg2, sizeof(confirm_msg2), "%s", msg2);
	snprintf(confirm_yes, sizeof(confirm_yes), "%s", yes_label);
	snprintf(confirm_no, sizeof(confirm_no), "%s", no_label);
	confirm_cb = cb;
	assert(safe_answer == SCREEN_CONFIRM_ANSWER_YES
		   || safe_answer == SCREEN_CONFIRM_ANSWER_NO);
	confirm_safe_answer = safe_answer;
}

/* The message lines as the render draws them: text and top y, in draw order.
 * Always the Beta layout: two lines at y=70/90 units. */
static int confirm_lines(const char* text[3], int y[3]) {
	int sc = GFX_GUI_SCALE;
	text[0] = confirm_msg1;
	text[1] = confirm_msg2;
	y[0] = 70 * sc;
	y[1] = 90 * sc;
	return 2;
}

static void confirm_rect(int width, int height, int idx, int* bx, int* by) {
	int s = GFX_GUI_SCALE;
	/* [740c583 GuiYesNo.initGui]: w/2-155 and w/2-155+160, h/6+96 */
	*bx = width / 2 - 155 * s + idx * 160 * s;
	*by = height / 6 + 96 * s;
}

static void screen_confirm_reset(struct screen* s, int width, int height) {
	(void)s;
	(void)width;
	(void)height;
	assert(confirm_cb); /* setup() must precede screen_set */
	input_pointer_enable(true);
	/* MODAL ENTRY selects the non-destructive answer (ruling 2 item 5). This
	 * is the ONLY place it is applied: round 2 also re-applied it on every IR
	 * reacquisition, which silently discarded a directional choice (A3 NEW-4)
	 * and diverged from the rule tests/test_options_env.c pins. Beta has no
	 * d-pad default at all [740c583 GuiYesNo]. */
	selected = confirm_safe_answer;
	confirm_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	focus = (struct gutil_menu_focus) {0};
	pointer_available = false;
}

/* The button the SPRITE rests on, or GUTIL_MENU_TARGET_NONE. Both answers are
 * always enabled on this screen. */
static int confirm_pointer_button(void) {
	if(!pointer_available)
		return GUTIL_MENU_TARGET_NONE;
	for(int k = 0; k < 2; k++) {
		int bx, by;
		confirm_rect(gfx_width(), gfx_height(), k, &bx, &by);
		struct gutil_button_bounds bounds
			= gutil_button_rect(bx, by, CONFIRM_BTN_W);
		if(gutil_button_hit(&bounds, pointer_x, pointer_y))
			return k;
	}
	return GUTIL_MENU_TARGET_NONE;
}

static void screen_confirm_update(struct screen* s, float dt) {
	(void)s;
	(void)dt;

	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);

	bool navigating = input_pressed(IB_GUI_LEFT) || input_pressed(IB_GUI_RIGHT);
	bool pointer_owner = gutil_menu_pointer_focus(&focus, pointer_available,
												  pointer_x, pointer_y,
												  navigating);
	int hovered = confirm_pointer_button();
	if(pointer_owner && hovered != GUTIL_MENU_TARGET_NONE) {
		selected = hovered;
		confirm_origin = GUTIL_MENU_ORIGIN_POINTER;
	}

	if(navigating) {
		selected = 1 - selected;
		confirm_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	}

	/* D1 (Astra ruling 5, closing review A16-1). R-E made the FIRST A on a
	 * fresh confirmation a no-op for the commonest Wii posture -- Remote in
	 * hand, pointed at the TV, sprite over empty space, D-pad for navigation
	 * -- because the ring opens ORIGIN_DEFAULT. The only navigation this
	 * screen has is LEFT/RIGHT and it TOGGLES, so the natural recovery from a
	 * dead button moved the highlight onto the DESTRUCTIVE answer and stamped
	 * it DIRECTIONAL; the next A deleted. Measured on a real world by the
	 * reviewer: A (nothing), RIGHT, A -> world gone.
	 *
	 * So while the SELECTION IS THE OPENER'S DECLARED SAFE ANSWER, this
	 * screen hands the helper DIRECTIONAL: the safe answer is the screen's
	 * own declared intent and is non-destructive by construction, so A
	 * dismisses the dialog and only a deliberate toggle can arm the other
	 * one. The exception is by MEANING, never by index, and it is NEVER
	 * transferred to the destructive answer -- the moment `selected` is not
	 * the safe answer the real sticky origin applies and R-D guards it, which
	 * is why this is computed per frame rather than stored. */
	enum gutil_menu_origin decide_origin = selected == (int)confirm_safe_answer
		? GUTIL_MENU_ORIGIN_DIRECTIONAL
		: confirm_origin;

	/* The ONE activation path (ruling 2 item 1); see screen_title.c for why
	 * input_pressed alone satisfies the fresh-press requirement. */
	bool activate_pressed = input_pressed(IB_GUI_CLICK);
	struct gutil_menu_decision decision
		= gutil_menu_activate_decide((struct gutil_menu_activate_state) {
			.pressed = activate_pressed,
			.navigated = navigating,
			.pointer_valid = pointer_available,
			.selection_origin = decide_origin,
			.hovered = hovered,
			.hovered_enabled = hovered != GUTIL_MENU_TARGET_NONE,
			.selected = selected,
			.selected_enabled = true,
			/* R-D: the answer that is NOT the declared safe one performs
			 * whatever the dialog is asking about. It activates only if the
			 * sprite is resting on it or a direction press chose it -- so
			 * "aim at Delete, lower the Remote, press A" does nothing
			 * (review A9-1). */
			.selected_destructive = selected != (int)confirm_safe_answer,
			/* Every opener of this screen is a destructive confirmation
			 * (the caller list above: Delete world, Remove server), so a
			 * sprite/ring mismatch only moves focus (Astra D2-destructive
			 * ruling B). */
			.destructive_dialog = true,
		});
	if(decision.action == GUTIL_MENU_FOCUS_ONLY) {
		selected = decision.selected;
		/* The helper changes the origin in exactly ONE case -- the pointer
		 * FOCUS_ONLY branch, which stamps POINTER because the sprite chose
		 * the new selection (gui_util.c:325-332); everywhere else it echoes
		 * what it was given. Writing it back only here is therefore identical
		 * to the previous unconditional write-back for every input, and it is
		 * what keeps D1's per-frame exception above out of the STICKY origin:
		 * the exception must never travel with the player to the destructive
		 * answer. */
		confirm_origin = decision.selection_origin;
	}
	if(decision.action == GUTIL_MENU_ACTIVATE) {
		/* Beta GuiScreen.mouseClicked plays the click for an enabled
		 * button before the action. */
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		void (*cb)(bool) = confirm_cb;
		confirm_cb = NULL;
		cb(decision.target == SCREEN_CONFIRM_ANSWER_YES);
		return;
	}

	/* R-C: a press the rule CONSUMED reaches nothing else this frame -- on a
	 * table where activate shares a button with Close, it would otherwise
	 * ANSWER this dialog on a frame the player meant as a navigation. */
	if(activate_pressed)
		return;

	/* Back-out = the no button (never the destructive path). */
	if(input_pressed(IB_INVENTORY) || input_pressed(IB_HOME)) {
		void (*cb)(bool) = confirm_cb;
		confirm_cb = NULL;
		cb(false);
	}
}

/* See screen.h. `selected` is exactly what screen_confirm_render2D draws
 * hovered and what the activation decision reads, so this is a naming of the
 * live highlight, never a second opinion about it. */
bool screen_confirm_focus_is_safe(void) {
	return gstate.players[0].current_screen == &screen_confirm
		&& selected == (int)confirm_safe_answer;
}

static void screen_confirm_render2D(struct screen* s, int width, int height) {
	(void)s;
	int sc = GFX_GUI_SCALE;

	gutil_bg(); /* drawDefaultBackground */

	/* messages at y=70 / y=90, centered, 0xFFFFFF [740c583 GuiYesNo
	 * .drawScreen] (confirm_lines). */
	const char* lines[3];
	int line_y[3];
	int line_count = confirm_lines(lines, line_y);
	for(int k = 0; k < line_count; k++)
		gutil_text((width - gutil_font_width(lines[k], 8 * sc)) / 2,
				   line_y[k], lines[k], 8 * sc, true);

	const char* labels[2] = {confirm_yes, confirm_no};
	for(int k = 0; k < 2; k++) {
		int bx, by;
		confirm_rect(width, height, k, &bx, &by);
		bool hover = selected == k;
		gutil_button_draw(bx, by, CONFIRM_BTN_W, labels[k], true, hover);
	}

	/* The centred hint row (controller UI redesign 10-31, A1 helper). */
	{
		struct input_player_frame hint_f;
		input_prompt_frame(0, &hint_f);
		const struct gutil_hint_item hint_items[] = {
			{{IB_GUI_LEFT}, 1, "Switch", 0, false},
			{{IB_GUI_CLICK}, 1, "Confirm", 1, false},
			{{IB_INVENTORY}, 1, "Back", 2, true}};
		struct gutil_hint_row hint_row;
		if(gutil_hint_row_layout(&hint_f, 0, 0, width, height, false,
				GUTIL_HINT_ALIGN_CENTRE, hint_items, 3, &hint_row))
			gutil_hint_row_draw(&hint_row);
	}

	if(pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(pointer_x, pointer_y, glm_rad(pointer_angle), 0,
							 0, 256, 256, 48 * sc, 48 * sc);
	}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* The answer screen_confirm_render2D draws HOVERED -- literally `selected`,
 * the same variable the decision reads (A3 NEW-7). */
int screen_confirm_test_highlight(void) {
	return selected;
}

/* The message lines exactly as screen_confirm_render2D draws them (the same
 * confirm_lines call), plus the button labels and the declared safe answer.
 * Read-only. Returns the line count. */
int screen_confirm_test_lines(const char* text[3], int y[3],
							  const char** yes_label, const char** no_label,
							  int* safe_answer) {
	const char* t[3] = {NULL, NULL, NULL};
	int ys[3] = {0, 0, 0};
	int n = confirm_lines(t, ys);
	for(int k = 0; k < 3; k++) {
		if(text)
			text[k] = t[k];
		if(y)
			y[k] = ys[k];
	}
	if(yes_label)
		*yes_label = confirm_yes;
	if(no_label)
		*no_label = confirm_no;
	if(safe_answer)
		*safe_answer = (int)confirm_safe_answer;
	return n;
}

/* Answer the live dialog exactly as an activation does (the stored callback,
 * fired once). Returns false when no answer is armed. */
bool screen_confirm_test_answer(bool yes) {
	void (*cb)(bool) = confirm_cb;
	if(!cb)
		return false;
	confirm_cb = NULL;
	cb(yes);
	return true;
}
#endif

struct screen screen_confirm = {
	.reset = screen_confirm_reset,
	.update = screen_confirm_update,
	.render2D = screen_confirm_render2D,
	.render3D = NULL,
	.render_world = false,
};
