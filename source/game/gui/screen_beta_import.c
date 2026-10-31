/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

/* "Import Beta 1.7.3" progress / result screen (ingame-import-1030, coder C).
 *
 * NEVER BLOCKS. Every frame the update asks the worker for a snapshot
 * (beta_import_poll, non-blocking) and the render draws it, so the main loop
 * keeps presenting frames and sending heartbeats for the whole import; the
 * worker alone touches the SD card.
 *
 * Composition: the Texture Packs screen's approved canvas (640x480 centred,
 * scaled by height/480) -- heading at 32,22, the paper panel at 32,86
 * 576x276, the primary button at 168,378 304x44 and the glyph footer at
 * y 432 -- so the import reads as a page of that screen. The bar is the
 * loading screen's Beta GuiProgress bar (screen_load_world.c: 0x808080 track,
 * 0x80FF80 fill, 2 px surround), 400x12. A phase with no known total draws
 * no fill and no number: no invented progress.
 *
 * Controls (Wiimote+Nunchuk, Classic and GameCube identical roles):
 *   activate (IB_GUI_CLICK) on the button  Cancel while running, OK after;
 *   Close (IB_INVENTORY) or HOME           the same action;
 *   pointer on the button or the footer    the same action, one-press rule
 *                                          of gutil_menu_activate_decide. */

#include "screen_beta_import.h"

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/input_owner.h"
#include "frontend_presentation.h"

#include <stdio.h>
#include <string.h>

#define BIS_PACK_NAME "Beta 1.7.3"
#define BIS_MAX_LINES 4

/* Canvas numbers (pixels on the 640x480 canvas). The heading, panel, button
 * and footer are screen_texturepacks.c's TP_* values. */
#define BIS_HEAD_X 32
#define BIS_HEAD_Y 22
#define BIS_HEAD_FONT 28
#define BIS_PANEL_X 32
#define BIS_PANEL_Y 86
#define BIS_PANEL_W 576
#define BIS_PANEL_H 276
#define BIS_TITLE_Y 108
#define BIS_TITLE_FONT 20
#define BIS_BAR_X 120
#define BIS_BAR_Y 150
#define BIS_BAR_W 400
#define BIS_BAR_H 12
#define BIS_TEXT_X 56
#define BIS_TEXT_W 528
#define BIS_LINES_Y 184
#define BIS_LINES_Y_RESULT 150
#define BIS_LINE_FONT 16
#define BIS_LINE_PITCH 24
#define BIS_BUTTON_X 168
#define BIS_BUTTON_Y 378
#define BIS_BUTTON_W 304
#define BIS_BUTTON_H 44
#define BIS_FOOTER_Y 432
#define BIS_FOOTER_H 36

/* ONE target id: the button. The footer prompt is the same action. */
#define BIS_TARGET_BUTTON 0

static enum screen_beta_import_view s_view;
static bi_progress s_progress;
static bool s_selector_called, s_selector_ok;
static bool s_pointer_available;
static float s_pointer_x, s_pointer_y, s_pointer_angle;
static struct gutil_menu_focus s_focus;
static enum gutil_menu_origin s_origin;
/* The lines render draws, rebuilt by the update (never in render). */
static char s_title[64];
static char s_lines[BIS_MAX_LINES][128];
static int s_line_count;
static int s_bar_permille; /* -1: no known fraction */

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static bool s_override_on;
static bi_progress s_override;
void screen_beta_import_test_override(const bi_progress* p) {
	s_override_on = p != NULL;
	if(p)
		s_override = *p;
}
#endif

static void bis_poll(bi_progress* out) {
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_override_on) {
		*out = s_override;
		return;
	}
#endif
	beta_import_poll(out);
}

/* ---- geometry ----------------------------------------------------------- */

struct bis_canvas {
	int ox;
	float u;
};

static struct bis_canvas bis_canvas_of(int width, int height) {
	struct bis_canvas c;
	c.u = height > 0 ? (float)height / 480.0F : 1.0F;
	int canvas = (int)(640.0F * c.u + 0.5F);
	c.ox = (width - canvas) / 2;
	if(c.ox < 0)
		c.ox = 0;
	return c;
}

static int bis_d(const struct bis_canvas* c, int v) {
	return (int)((float)v * c->u + 0.5F);
}

static int bis_x(const struct bis_canvas* c, int v) {
	return c->ox + bis_d(c, v);
}

static bool bis_running(void) {
	return s_view == SCREEN_BETA_IMPORT_RUNNING
		|| s_view == SCREEN_BETA_IMPORT_CANCELLING;
}

static const char* bis_button_label(void) {
	return bis_running() ? "Cancel" : "OK";
}

/* The footer: ONE glyph prompt -- the Close role while running ("Cancel"),
 * activate afterwards ("OK") -- built from the detected bindings by the same
 * gutil helper every approved footer uses. */
static enum input_button bis_footer_button(void) {
	return bis_running() ? IB_INVENTORY : IB_GUI_CLICK;
}

/* The centred hint row (controller UI redesign 10-31, A1 helper); its one
 * cell is the footer target, so the pointer hits exactly what is drawn. */
static bool bis_footer_row(int width, int height, struct gutil_hint_row* row) {
	struct input_player_frame prompts;
	input_prompt_frame(0, &prompts);
	const struct gutil_hint_item item = {
		{bis_footer_button()}, 1, bis_button_label(), 0, true};
	return gutil_hint_row_layout(&prompts, 0, 0, width, height, false,
								 GUTIL_HINT_ALIGN_CENTRE, &item, 1, row);
}

static struct screen_beta_import_rect bis_footer_rect(int width, int height) {
	struct gutil_hint_row row;
	if(!bis_footer_row(width, height, &row) || row.count < 1)
		return (struct screen_beta_import_rect) {0, 0, 0, 0};
	struct gutil_button_bounds r = row.cells[0].rect;
	return (struct screen_beta_import_rect) {r.x, r.y, r.width, r.height};
}

void screen_beta_import_layout(int width, int height,
							   struct screen_beta_import_layout* out) {
	struct bis_canvas c = bis_canvas_of(width, height);
	out->heading = (struct screen_beta_import_rect) {
		bis_x(&c, BIS_HEAD_X), bis_d(&c, BIS_HEAD_Y), bis_d(&c, BIS_PANEL_W),
		bis_d(&c, BIS_HEAD_FONT)};
	out->bar = (struct screen_beta_import_rect) {
		bis_x(&c, BIS_BAR_X), bis_d(&c, BIS_BAR_Y), bis_d(&c, BIS_BAR_W),
		bis_d(&c, BIS_BAR_H)};
	out->status = (struct screen_beta_import_rect) {
		bis_x(&c, BIS_TEXT_X),
		bis_d(&c, bis_running() ? BIS_LINES_Y : BIS_LINES_Y_RESULT),
		bis_d(&c, BIS_TEXT_W), bis_d(&c, BIS_LINE_FONT)};
	out->button = (struct screen_beta_import_rect) {
		bis_x(&c, BIS_BUTTON_X), bis_d(&c, BIS_BUTTON_Y),
		bis_d(&c, BIS_BUTTON_W), bis_d(&c, BIS_BUTTON_H)};
	out->footer = bis_footer_rect(width, height);
}

static bool bis_hit(struct screen_beta_import_rect r, float x, float y) {
	return x >= (float)r.x && x < (float)(r.x + r.w) && y >= (float)r.y
		&& y < (float)(r.y + r.h);
}

/* ---- the words ---------------------------------------------------------- */

/* Word-wrap `text` into the remaining line slots at the status font, so a long
 * plain sentence ("No sounds found: put your .minecraft resources folder in
 * sd:/beta/resources to add them.") is shown whole instead of cut off. */
static void bis_add_wrapped(const char* text, int width_px, int font) {
	const char* p = text;
	while(*p && s_line_count < BIS_MAX_LINES) {
		while(*p == ' ')
			p++;
		if(!*p)
			break;
		char* line = s_lines[s_line_count];
		size_t cap = sizeof(s_lines[0]);
		size_t len = 0, fit_len = 0;
		const char* fit_end = NULL;
		const char* q = p;
		line[0] = '\0';
		while(*q) {
			const char* word_end = q;
			while(*word_end && *word_end != ' ')
				word_end++;
			size_t add = (size_t)(word_end - q) + (len ? 1 : 0);
			if(len + add + 1 > cap)
				break;
			char trial[128];
			memcpy(trial, line, len);
			if(len)
				trial[len] = ' ';
			memcpy(trial + len + (len ? 1 : 0), q, (size_t)(word_end - q));
			trial[len + add] = '\0';
			if(gutil_font_width(trial, font) > width_px && len)
				break;
			memcpy(line, trial, len + add + 1);
			len += add;
			fit_len = len;
			fit_end = word_end;
			q = word_end;
			while(*q == ' ')
				q++;
		}
		if(!fit_end) {
			/* One word wider than the line: fit it with an ellipsis. */
			fep_text_fit(line, cap, p, font, width_px);
			while(*p && *p != ' ')
				p++;
		} else {
			line[fit_len] = '\0';
			p = fit_end;
		}
		s_line_count++;
	}
}

static const char* bis_phase_label(bi_state st) {
	switch(st) {
		case BI_SCANNING: return "Looking for your Beta files";
		case BI_TEXTURES: return "Step 1 of 3: Copying textures";
		case BI_SOUNDS: return "Step 2 of 3: Converting sounds";
		case BI_SWAPPING: return "Step 3 of 3: Finishing up";
		default: return "Starting";
	}
}

/* Rebuild the title, the lines and the bar from the latest snapshot. */
static void bis_compose(int width, int height) {
	struct bis_canvas c = bis_canvas_of(width, height);
	int text_w = bis_d(&c, BIS_TEXT_W);
	int font = bis_d(&c, BIS_LINE_FONT);
	s_line_count = 0;
	s_bar_permille = -1;
	const bi_progress* p = &s_progress;
	switch(s_view) {
		case SCREEN_BETA_IMPORT_RUNNING:
		case SCREEN_BETA_IMPORT_CANCELLING: {
			snprintf(s_title, sizeof(s_title), "%s",
					 s_view == SCREEN_BETA_IMPORT_CANCELLING
						 ? "Stopping the import..."
						 : bis_phase_label(p->state));
			bool counted = p->total > 0
				&& (p->state == BI_TEXTURES || p->state == BI_SOUNDS);
			if(counted) {
				int done = p->done < 0 ? 0 : p->done > p->total ? p->total : p->done;
				s_bar_permille = (int)((long)done * 1000 / p->total);
				char count[64];
				snprintf(count, sizeof(count), "%d of %d %s", done, p->total,
						 p->state == BI_SOUNDS ? "sounds" : "files");
				bis_add_wrapped(count, text_w, font);
			}
			if(p->msg[0])
				bis_add_wrapped(p->msg, text_w, font);
			if(s_view == SCREEN_BETA_IMPORT_CANCELLING)
				bis_add_wrapped("Your texture pack will not change.", text_w,
								font);
			break;
		}
		case SCREEN_BETA_IMPORT_DONE: {
			snprintf(s_title, sizeof(s_title), "Beta 1.7.3 imported");
			bool sounds = p->sounds_imported && p->sounds_count > 0;
			char line[112];
			if(s_selector_ok && sounds)
				snprintf(line, sizeof(line),
						 "Beta 1.7.3 textures and %d sounds are now active.",
						 p->sounds_count);
			else if(s_selector_ok)
				snprintf(line, sizeof(line),
						 "Beta 1.7.3 textures are now active.");
			else
				snprintf(line, sizeof(line),
						 "Textures%s imported. Pick Beta 1.7.3 in the list, "
						 "then Apply Pack.",
						 sounds ? " and sounds" : "");
			bis_add_wrapped(line, text_w, font);
			if(!sounds)
				bis_add_wrapped("No sounds found: put your .minecraft resources "
								"folder in sd:/beta/resources to add them.",
								text_w, font);
			break;
		}
		case SCREEN_BETA_IMPORT_FAILED:
			snprintf(s_title, sizeof(s_title), "The import didn't work");
			bis_add_wrapped(p->msg[0] ? p->msg : "Something went wrong.",
							text_w, font);
			bis_add_wrapped("Your texture pack was not changed.", text_w, font);
			break;
		case SCREEN_BETA_IMPORT_CANCELLED:
			snprintf(s_title, sizeof(s_title), "Import cancelled");
			bis_add_wrapped("Nothing was changed.", text_w, font);
			break;
	}
}

/* ---- the worker hand-off ------------------------------------------------ */

/* Wraps the pack screen's selector so this screen knows what to say. Called
 * by beta_import_poll on the main thread, once, after the swap. */
static bool bis_selector(const char* pack_value) {
	s_selector_called = true;
	s_selector_ok = screen_texturepacks_import_select(pack_value);
	return s_selector_ok;
}

static void bis_follow(void) {
	bis_poll(&s_progress);
	switch(s_progress.state) {
		case BI_DONE: s_view = SCREEN_BETA_IMPORT_DONE; break;
		case BI_FAILED: s_view = SCREEN_BETA_IMPORT_FAILED; break;
		case BI_CANCELLED: s_view = SCREEN_BETA_IMPORT_CANCELLED; break;
		default: break; /* still running (or cancelling) */
	}
}

void screen_beta_import_open(void) {
	s_selector_called = s_selector_ok = false;
	memset(&s_progress, 0, sizeof(s_progress));
	s_view = SCREEN_BETA_IMPORT_RUNNING;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(s_override_on) { /* PC captures: no worker, the override is the state */
		s_selector_ok = true; /* the normal case: the pack screen queued it */
		screen_set(&screen_beta_import);
		return;
	}
#endif
	beta_import_set_selector(bis_selector);
	if(!beta_import_start()) {
		/* Already running (it cannot be, this screen is the only starter and
		 * it stays up until the worker is finished): follow it if so,
		 * otherwise say so plainly. */
		bis_poll(&s_progress);
		bool active = s_progress.state >= BI_SCANNING
			&& s_progress.state <= BI_SWAPPING;
		if(!active) {
			s_view = SCREEN_BETA_IMPORT_FAILED;
			s_progress.state = BI_FAILED;
			snprintf(s_progress.msg, sizeof(s_progress.msg),
					 "The import couldn't start.");
		}
	}
	screen_set(&screen_beta_import);
}

/* ---- the screen ---------------------------------------------------------- */

static void bis_reset(struct screen* s, int width, int height) {
	s->render_world = false;
	s->render3D = NULL;
	input_pointer_enable(true);
	s_pointer_available = false;
	s_focus = (struct gutil_menu_focus) {0};
	s_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	bis_compose(width, height);
}

static void bis_leave(void) {
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	screen_set(&screen_texturepacks);
}

static void bis_cancel(void) {
	if(s_view != SCREEN_BETA_IMPORT_RUNNING)
		return;
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(!s_override_on)
#endif
		beta_import_cancel();
	s_view = SCREEN_BETA_IMPORT_CANCELLING;
}

/* The button is enabled except while a cancel is already on its way. */
static bool bis_button_enabled(void) {
	return s_view != SCREEN_BETA_IMPORT_CANCELLING;
}

static void bis_activate(void) {
	if(bis_running())
		bis_cancel();
	else
		bis_leave();
}

static void bis_update(struct screen* s, float dt) {
	(void)s;
	(void)dt;
	if(bis_running())
		bis_follow();
	int width = gfx_width(), height = gfx_height();
	bis_compose(width, height);

	s_pointer_available
		= input_pointer(&s_pointer_x, &s_pointer_y, &s_pointer_angle);
	bool navigating = input_pressed(IB_GUI_UP) || input_pressed(IB_GUI_DOWN)
		|| input_pressed(IB_GUI_LEFT) || input_pressed(IB_GUI_RIGHT);
	(void)gutil_menu_pointer_focus(&s_focus, s_pointer_available, s_pointer_x,
								   s_pointer_y, navigating);
	if(navigating)
		s_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;

	struct screen_beta_import_layout l;
	screen_beta_import_layout(width, height, &l);
	int hovered = GUTIL_MENU_TARGET_NONE;
	if(s_pointer_available
	   && (bis_hit(l.button, s_pointer_x, s_pointer_y)
		   || bis_hit(l.footer, s_pointer_x, s_pointer_y)))
		hovered = BIS_TARGET_BUTTON;
	bool click = input_pressed(IB_GUI_CLICK);
	struct gutil_menu_decision d
		= gutil_menu_activate_decide((struct gutil_menu_activate_state) {
			.pressed = click,
			.navigated = navigating,
			.pointer_valid = s_pointer_available,
			.selection_origin = s_origin,
			.hovered = hovered,
			.hovered_enabled
			= hovered != GUTIL_MENU_TARGET_NONE && bis_button_enabled(),
			.selected = BIS_TARGET_BUTTON,
			.selected_enabled = bis_button_enabled(),
		});
	s_origin = d.selection_origin;
	if(d.action == GUTIL_MENU_ACTIVATE) {
		bis_activate();
		return;
	}
	/* R-C: a consumed press reaches nothing else this frame. */
	if(click)
		return;
	if(input_pressed(IB_INVENTORY) || input_pressed(IB_HOME))
		bis_activate();
}

static void bis_render2D(struct screen* s, int width, int height) {
	(void)s;
	struct bis_canvas c = bis_canvas_of(width, height);
	if(!fep_backdrop(width, height, 90))
		gutil_bg();

	gutil_text(bis_x(&c, BIS_HEAD_X), bis_d(&c, BIS_HEAD_Y),
			   "Import " BIS_PACK_NAME, bis_d(&c, BIS_HEAD_FONT), true);

	struct fep_rect panel
		= fep_rect(bis_x(&c, BIS_PANEL_X), bis_d(&c, BIS_PANEL_Y),
				   bis_d(&c, BIS_PANEL_W), bis_d(&c, BIS_PANEL_H));
	fep_paper(panel);
	fep_bevel(panel);

	/* The panel title: dark ink on paper, shadow off (the pack screen's
	 * status-line rule). */
	int title_font = bis_d(&c, BIS_TITLE_FONT);
	char fitted[96];
	fep_text_fit(fitted, sizeof(fitted), s_title, title_font,
				 bis_d(&c, BIS_TEXT_W));
	bool failed = s_view == SCREEN_BETA_IMPORT_FAILED;
	gutil_text_rgb(panel.x + (panel.w - gutil_font_width(fitted, title_font)) / 2,
				   bis_d(&c, BIS_TITLE_Y), fitted, title_font, false,
				   failed ? 0xC0 : 0x30, 0x30, 0x30);

	struct screen_beta_import_layout l;
	screen_beta_import_layout(width, height, &l);
	if(bis_running()) {
		int e = bis_d(&c, 2);
		fep_fill(fep_rect(l.bar.x - e, l.bar.y - e, l.bar.w + 2 * e,
						  l.bar.h + 2 * e),
				 0x20, 0x20, 0x20, 255);
		fep_fill(fep_rect(l.bar.x, l.bar.y, l.bar.w, l.bar.h), 0x80, 0x80,
				 0x80, 255);
		if(s_bar_permille >= 0)
			fep_fill(fep_rect(l.bar.x, l.bar.y,
							  l.bar.w * s_bar_permille / 1000, l.bar.h),
					 0x80, 0xFF, 0x80, 255);
	}

	int font = bis_d(&c, BIS_LINE_FONT);
	for(int k = 0; k < s_line_count; k++) {
		int y = l.status.y + k * bis_d(&c, BIS_LINE_PITCH);
		int w = gutil_font_width(s_lines[k], font);
		gutil_text_rgb(panel.x + (panel.w - w) / 2, y, s_lines[k], font, false,
					   0x30, 0x30, 0x30);
	}

	/* The primary button: the pack screen's Apply material and rules. */
	struct fep_rect b = fep_rect(l.button.x, l.button.y, l.button.w, l.button.h);
	bool enabled = bis_button_enabled();
	fep_widget(b, enabled);
	fep_bevel(b);
	if(enabled)
		fep_outline(b, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
	else
		fep_fill(b, 0, 0, 0, 0x70);
	const char* label = bis_button_label();
	int bfont = bis_d(&c, 20);
	int tw = gutil_font_width(label, bfont);
	uint8_t ink = enabled ? 0xFF : 0xA0;
	gutil_text_rgb(b.x + (b.w - tw) / 2, b.y + (b.h - bfont) / 2, label, bfont,
				   true, ink, ink, enabled ? 0xE0 : ink);

	/* The glyph footer (the shared action->binding table). */
	struct gutil_hint_row row;
	if(bis_footer_row(width, height, &row))
		gutil_hint_row_draw(&row);

	if(s_pointer_available) { /* the IR cursor, as every menu draws it */
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(s_pointer_x, s_pointer_y, glm_rad(s_pointer_angle),
							 0, 0, 256, 256, 48 * GFX_GUI_SCALE,
							 48 * GFX_GUI_SCALE);
	}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void screen_beta_import_test_request_cancel(void) { bis_cancel(); }
enum screen_beta_import_view screen_beta_import_test_view(void) {
	return s_view;
}
int screen_beta_import_test_lines(const char** lines, int cap) {
	for(int k = 0; k < s_line_count && k < cap; k++)
		lines[k] = s_lines[k];
	return s_line_count;
}
const char* screen_beta_import_test_heading(void) { return s_title; }
const char* screen_beta_import_test_button_label(void) {
	return bis_button_label();
}
bool screen_beta_import_test_button_enabled(void) {
	return bis_button_enabled();
}
int screen_beta_import_test_bar_permille(void) { return s_bar_permille; }
#endif

struct screen screen_beta_import = {
	.reset = bis_reset,
	.update = bis_update,
	.render2D = bis_render2D,
	.render3D = NULL,
	.render_world = false,
};
