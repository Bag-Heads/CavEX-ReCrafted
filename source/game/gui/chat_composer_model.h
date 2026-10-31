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

#ifndef CHAT_COMPOSER_MODEL_H
#define CHAT_COMPOSER_MODEL_H

#include <stdbool.h>
#include <stddef.h>

/* Pure model behind the multiplayer chat/command composer (screen_chat.c)
 * and the received-chat history. No GL, no input, no allocation -- the unit
 * test target (tests/test_chat_composer.c) compiles exactly this file.
 * Design record: docs/mp_chat_auth_research_2026-09-11.md ("Chat opener:
 * DECIDED"). */

/* Beta's composer refuses the 101st character [740c583 GuiChat.keyTyped:
 * `message.length() < 100`]; the wire cap is 119 code units [740c583
 * Packet3Chat.readPacketData readString(119)]. The composer never reaches
 * the wire cap, so mp_encode_chat's reject-over-119 rule is unreachable
 * from this screen. */
#define CHAT_COMPOSER_CAP 100
/* One display line: Packet3's 119 code units + NUL (game_state.h ring). */
#define CHAT_LINE_BYTES 120
/* GuiIngame.addChatMessage splits any line wider than 320 logical px
 * [740c583 GuiIngame.addChatMessage]. */
#define CHAT_WRAP_WIDTH 320
/* Beta keeps at most 50 lines [740c583 GuiIngame.addChatMessage]. */
#define CHAT_HISTORY_LINES 50

/* Enter on the composer [740c583 GuiChat.keyTyped key 28]: `message.trim()`
 * (Java String.trim strips every char <= ' ' from BOTH ends), and only a
 * non-empty result is sent. Returns the number of bytes written to `out`
 * (0 = nothing to send; `out` is then an empty string). `out_size` must be
 * at least CHAT_COMPOSER_CAP + 1. */
size_t chat_composer_submit(const char* draft, char* out, size_t out_size);

/* Opener gate (decision item 3): the shortcut fires on the PHYSICAL D-pad Up
 * press edge only when neither Left nor Right of the same D-pad is held at
 * that instant -- a diagonal "rock" while scrolling the hotbar sets both
 * bits. Nothing is ever synthesized on a Left/Right release: the caller only
 * evaluates this on the Up rising edge, so a refused press needs a fresh Up
 * press. Applies to D-pad Up bindings only (the caller decides). */
bool chat_open_gate(bool up_edge, bool left_held, bool right_held);

/* Width of a display string in logical px. Production passes
 * gutil_font_width at the 8-px font scale (it already skips \247 colour
 * pairs like FontRenderer.getStringWidth); tests pass a fixed-width fake. */
typedef int (*chat_width_fn)(const char* text);

/* Byte-exact port of [740c583 GuiIngame.addChatMessage]'s split loop:
 *   while width(s) > max: i = 1; while i < len && width(s[0..i+1]) <= max: i++;
 *                         emit s[0..i]; s = s[i..];
 *   emit s;
 * Pieces are emitted OLDEST first (Beta adds the head piece to the list
 * before recursing on the remainder, so the head sits above the tail on
 * screen). Beta splits on character positions and never copies a colour code
 * onto the continuation piece -- neither does this. Display strings are
 * single-byte (mp_utf16_to_display transcodes for the byte-indexed font), so
 * a byte is a character. Returns the number of pieces written (each
 * NUL-terminated within CHAT_LINE_BYTES); stops at `max_pieces`. */
size_t chat_wrap(const char* text, int max_width, chat_width_fn width_fn,
				 char out[][CHAT_LINE_BYTES], size_t max_pieces);

/* ---- composer geometry (docs/ui_design/chat-keyboard/SPEC.md) ------------
 *
 * Astra's binding design, section "Coordinate system and safe area" and its
 * rect table: every number below is QUOTED from that table, none is derived
 * by feel. Rects are (x, y, w, h) in native GUI pixels, half-open at right and
 * bottom, for the full 640x480 composition; `ox` re-centres that same
 * composition in a wider raster (0 at 640, 81 at the Wii's 802x480 16:9 GUI,
 * source/platform/wii/gfx.c:413-420). Widths and heights never change with ox.
 *
 * THE DEFECT THIS REPLACES: the panel used to be (44,236,552,236) -- bottom
 * edge y=472, with the Space/Backspace row spanning y440..468 of 480, i.e.
 * INSIDE the ~5% SDTV overscan band. The panel now ends at y=432, 48 px clear
 * (Astra ruling Q1: "Keep the 552-pixel width; grow upward to (44,120,552,312),
 * with 40-pixel key height and bottom at 432").
 *
 * Only the FULL layout is specified here. SPEC: "Compact 240-high path remains
 * unreachable for remote chat and keeps its existing behavior; do not force
 * this 312-high panel into it." */

struct chat_rect {
	int x, y, w, h;
};

/* "Assume up to 5% cropping on each edge, so the project title-safe rectangle
 * is (32,24,576,432)". The 16:9 frame-safe rect is (41,24,720,432) and the
 * centred 640-wide composition's stricter rect is (113,24,576,432) -- which is
 * exactly this rect plus ox, so one check covers both aspects. */
#define CHAT_SAFE_X 32
#define CHAT_SAFE_Y 24
#define CHAT_SAFE_W 576
#define CHAT_SAFE_H 432

/* SPEC "Art, typography and states": key characters 20, every informational
 * label 16. No 12/14-px label may appear in the chat surface. */
#define CHAT_FONT_KEY 20
#define CHAT_FONT_LABEL 16
#define CHAT_GLYPH 24
/* "resolved A glyph, 4-pixel gap, 16-pixel label" -- the gap inside every
 * glyph+label group (Type, Send, Cancel, Backspace). */
#define CHAT_HINT_GAP 4

/* SPEC "World remains live": open history is at most three lines, newest at
 * y=98, 18-px pitch (= 9 logical px at GFX_GUI_SCALE 2), clipped to
 * (44,60,552,54) in both text and strip. */
#define CHAT_HISTORY_OPEN_LINES 3
#define CHAT_HISTORY_PITCH 18

/* SPEC "Draft text ... Counter right edge=576 ... reserve 88 pixels at
 * x488..576"; the composer cap is CHAT_COMPOSER_CAP, always shown as n/100. */
#define CHAT_COUNTER_RESERVE 88

/* SPEC "Art, typography and states" state table + motion rules, in ms. */
#define CHAT_PRESS_MS 80	 /* pressed label offset, then released */
#define CHAT_CAP_FLASH_MS 120 /* counter flashes yellow on a refused byte */
#define CHAT_RETRY_MS 1200	 /* refused enqueue: counter shows "Retry" */

struct chat_geometry {
	struct chat_rect safe;	   /* 32,24,576,432 (+ox) */
	struct chat_rect panel;	   /* 44,120,552,312 */
	struct chat_rect header;   /* 56,132,528,24 */
	struct chat_rect input;	   /* 56,164,528,32  (the draft inset) */
	struct chat_rect keyboard; /* 56,204,528,216 (tabs..utility envelope) */
	struct chat_rect tabs;	   /* 56,204,528,32 */
	struct chat_rect grid;	   /* 56,244,528,128 */
	struct chat_rect utility;  /* 56,380,528,40 */
	struct chat_rect notice;   /* 44,24,552,28 */
	struct chat_rect history;  /* 44,60,552,54  (clip, text and strips) */
	struct chat_rect draft;	   /* 64,172,416,16 (clip) */
	struct chat_rect counter;  /* 488,172,88,16 (reserved allocation) */
	int title_x, title_y;	   /* 56,136 at font 20 */
	int type_hint_x, type_hint_y; /* 152,132: A glyph + "Type" */
	int hint_y;				   /* 132: Send/Cancel target tops */
	int hint_right;			   /* 584: Cancel's right edge */
	int hint_gap;			   /* 16: between the Send and Cancel groups */
	int history_newest_y;	   /* 98, older lines climb by CHAT_HISTORY_PITCH */
};

/* Fill `g` with the SPEC rect table, offset horizontally by `ox`. */
void chat_geometry(struct chat_geometry* g, int ox);

/* ---- controller-mode request (inverted dependency) -----------------------
 *
 * The composer must know whether something is driving the menus through the
 * CAVEWII_AGENT bridge, so it can stay OUT of PC keyboard text mode and let the
 * controller aliases run (Astra 2026-09-17, the approved agent-armed mode).
 *
 * It CANNOT ask agent_input.c directly. screen_chat.c compiles into the
 * cavex_prod_core OBJECT pool, which is built with only -DPLATFORM_PC, while
 * CAVEWII_AGENT_BRIDGE is defined on the input-agent variant and the app target
 * (CMakeLists.txt:476-499) and agent_input.c is app-only (CMakeLists.txt:447).
 * A direct call was therefore compiled out of the shipped binary -- the mode
 * silently never existed, which a real-app capture run caught.
 *
 * So the dependency is INVERTED: the core owns this plain flag, and the
 * app-only bridge writes it (agent_input.c sets it when a menu button is first
 * claimed and clears it on RESET). With the bridge unarmed nothing ever writes
 * it, so ordinary PC text entry and every non-app target are untouched. */
/* Clip one DISPLAY line to `max_width` for the open composer's 552-px history
 * band (SPEC: "Long open lines clip at the right, not rewrap the stored ring").
 * Bytes are dropped from the end until the line fits, then a half-cut Beta
 * colour pair is repaired: 0xA7 + selector is ONE zero-width unit, so a
 * byte-wise cut can leave a dangling 0xA7 that would swallow the next glyph.
 * The stored ring line is never modified. Returns the length written. */
size_t chat_clip_display(const char* text, char* out, size_t out_size,
						 int max_width, chat_width_fn width_fn);

void chat_controller_mode_set(bool requested);
bool chat_controller_mode_requested(void);

/* Is `r` fully inside `outer`? (Used by the geometry tests and by the
 * title-safe assertions; half-open at right/bottom.) */
bool chat_rect_inside(const struct chat_rect* outer, const struct chat_rect* r);
bool chat_rect_overlap(const struct chat_rect* a, const struct chat_rect* b);

#endif
