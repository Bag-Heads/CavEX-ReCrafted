/*
	Copyright (c) 2022 ByteBit/xtreme8000

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

#ifndef GUI_UTIL_H
#define GUI_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../item/item_types.h"
#include "../platform/input.h"
#include "../platform/texture.h"
#include "menu_focus.h"

struct input_player_frame;
int gutil_prompt_glyph(int x, int y, int size, int glyph);
int gutil_prompt_glyph_gray(int x, int y, int size, int glyph, uint8_t gray);
/* The width gutil_prompt_glyph(_gray) DRAWS for `glyph` at `size` (0 when
 * undrawable): size, 2*size for the wide PC cap (cells 45+46, approved atlas
 * only), a chord size + size/2 + size. Every layout measures with this. */
int gutil_prompt_glyph_width(int glyph, int size);
/* PC caps (10-31): where the key name lands on a cap `cap_w` wide drawn at
 * `size` (relative to the cap's top-left) and its font scale (size/2, or
 * size/4 when wider than the face). False for no label. */
bool gutil_pc_cap_label_rect(const char* label, int cap_w, int size, int* x,
							 int* y, int* w, int* h, int* scale);
/* Classic shoulder keycaps (maintainer 10-31): approved-controls.png cells
 * 22 L, 23 R, 14 ZL, 15 ZR are blank caps; gutil_prompt_glyph(_gray) letters
 * them with the ACTIVE pack font through gutil_text_rgb: size/2 text,
 * (20,20,20) times the glyph's gray, no shadow, the font's own one-unit
 * letter gap (2 px at 32), centred horizontally, vertical centre at 14.1/32
 * of the glyph. Not drawn when the cell comes from the fallback controls.png.
 * _label: the letters of `cell`, or NULL. _rect: where the 7-row letters'
 * ink lands, relative to the glyph's top-left (false when not a keycap). */
const char* gutil_keycap_label(int cell);
bool gutil_keycap_label_rect(int cell, int size, int* x, int* y, int* w,
							 int* h);
/* picture is an enum controller_picture from the detected device. */
int gutil_controller_picture_width(int picture, int height);
void gutil_controller_picture(int x, int y, int width, int height, int picture);
void gutil_menu_hint_owned(const struct input_player_frame* input, int x, int y,
						   enum input_button b, const char* text);
struct controller_card;
struct input_connection_notice;
struct input_inventory;
/* The approved A cell (0 Remote/Nunchuk, 10 Classic, 32 GameCube) of the
 * device the title's "Press A to join" offers: the inventory's join offer, or
 * on handover the first other connected, unassigned, eligible device (Wii
 * Remote channels, then GameCube ports). -1 when no such device is detected:
 * the prompt then draws its text alone, with no glyph and no icon spacing. */
int gutil_join_offer_cell(const struct input_inventory* inventory);
/* Presentation-only: draw join prompts and notice cards from `inventory`
 * instead of input_devices() (NULL restores it). Used by the PC capture
 * override on the title screen and by headless tests; never by gameplay. */
void gutil_join_inventory_override(const struct input_inventory* inventory);
int gutil_connection_notice_width(unsigned player,bool split,bool header);
/* Where an UNMOUNTED controller notice goes (notice-spot-1031). While a
 * screen's content renders, every gutil_text* call (headings, tab and button
 * labels, captions) and every declared keep-out (art such as the title logo)
 * is recorded. gutil_notice_free_spot always answers the fixed top-left anchor
 * (GUTIL_NOTICE_ANCHOR_X, _Y) -- the spot gameplay uses -- nudged DOWN, same
 * left edge, to just below any heading it would touch (4 px margin) while the
 * card stays in the top quarter. It never moves to another corner. Returns
 * false (and the bare anchor) only when no in-band spot clears the content. */
#define GUTIL_NOTICE_ANCHOR_X 24
#define GUTIL_NOTICE_ANCHOR_Y 16
/* Card A (maintainer pick 10-31): the floating notice card is 32 tall, Beta
 * grey 198 with a bevel and a 2 px dark edge OUTSIDE its bounds, text grey 40
 * at 16 px; picture, glyph and battery are 24 px, all on one centre line. */
#define GUTIL_NOTICE_CARD_H 32
#define GUTIL_NOTICE_TEXT 16
#define GUTIL_NOTICE_INK 40
void gutil_text_log_begin(void);
void gutil_text_log_end(void);
void gutil_keepout(int x, int y, int w, int h);
/* Recorded text/keep-out rects a w x h card at x,y would touch (4 px margin). */
int gutil_notice_spot_hits(int x, int y, int w, int h);
bool gutil_notice_free_spot(int width, int height, int card_w, int card_h,
	int* x, int* y);
/* "Pointer out of view - Use stick" is gameplay advice: menus have the
 * D-pad, so it is not shown while a menu is the current screen (#02). */
void gutil_notices_menu_context(bool menu);
void gutil_connection_notices(unsigned player,bool split,int x,int y,int width,int height);
/* Approved 16px recipe/manual strip; coordinates are the exact card bounds. */
void gutil_connection_notices_mini(unsigned player,bool split,int x,int y,int width,int height);
void gutil_connection_notices_in_header(unsigned player,bool split,int x,int y,int width,int height);
int gutil_control_icon_owned(const struct input_player_frame* input, int height,
	int x, enum input_button b, const char* str);
int gutil_control_icon(int x, enum input_button b, const char* str);
// Native menu prompts: explicit position, 16px text at GUI scale 2.
void gutil_menu_hint(int x, int y, enum input_button b, const char* text);
/* critique-1028: player-explicit forms (split panes use their own frame). */
void gutil_menu_hint_player(unsigned player, int x, int y, enum input_button b,
	const char* text);
int gutil_control_icon_player(unsigned player, int x, enum input_button b,
	const char* str);
/* THE CONTEXTUAL NAVIGATION HINT (Astra ruling 6, push-0921 A19): one line of
 * menu text centred in the free 24 px band directly ABOVE a list screen's
 * action strip -- "RIGHT: actions" while the ring is on a list row, "UP: list"
 * while it is on the strip. TEXT ONLY and deliberately so: the approved button
 * glyph is 32 px and does not fit the band, and ruling 6 refused both moving
 * the strip and inventing a half-size glyph. `strip_top` is the y of the
 * strip's FIRST button row in pixels, i.e. the screen's own rect function's
 * answer -- never a literal -- so the hint follows the layout it sits under.
 * A NULL or empty `text` draws nothing, which is how a screen says "hidden"
 * (pointer-owned focus, or a ring position with nothing to point at).
 *
 * _rect returns exactly what _draw would cover, so a headless test can assert
 * the band is clear of both neighbours without calling render2D. Both read
 * the same geometry, so they cannot drift. */
struct gutil_button_bounds gutil_menu_nav_hint_rect(int width, int strip_top,
													const char* text);
void gutil_menu_nav_hint(int width, int strip_top, const char* text);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* THE DRAW OBSERVER (push-0921 A23, review A20-2). The hint's whole
 * deliverable is a visible line of text, and until now the only evidence it
 * was on screen was four PC captures: the suite called the screens' pure
 * hint/rect functions itself, IN PARALLEL with render2D, so deleting the
 * gutil_menu_nav_hint(...) call out of either render2D left every test GREEN.
 *
 * This records what THIS function actually drew -- nothing else can reach it,
 * and it is written after the gutil_text call, so a hint that returns early
 * (no text, empty rect) records nothing. A test renders the real screen and
 * reads it; a deleted draw call then shows up as zero draws.
 *
 * Test builds only (CAVEWII_CLIENT_INTERFACE_TEST), like the screens' own
 * hooks: the shipped .dol carries none of it. */
void gutil_menu_nav_hint_observe_reset(void);
/* How many times gutil_menu_nav_hint DREW since the reset, and what the last
 * one drew (text NUL-terminated into `cap`, rect into `r` when non-NULL). */
unsigned gutil_menu_nav_hint_observed(char* text, size_t cap,
									  struct gutil_button_bounds* r);
#endif
/* The band's height: 12 units, 24 px at GUI scale 2 -- the measured gap
 * between the list viewport's bottom and the strip's first row on
 * screen_select_world / screen_multiplayer_setup. */
#define GUTIL_MENU_NAV_HINT_BAND (12 * GFX_GUI_SCALE)
/* Glyph-to-label gap of an ordinary menu hint (chest footers, Sort, options,
 * video settings, sign controls): 4 px in the approved designs. */
#define GUTIL_MENU_HINT_GAP (2 * GFX_GUI_SCALE)
/* IR jitter threshold of gutil_menu_pointer_focus (Astra ruling 5 D2): a
 * stationary sprite retakes menu focus from directional selection only once
 * it has moved at least this far (4 GUI units) from where it last rested. */
#define GUTIL_MENU_POINTER_JITTER (4 * GFX_GUI_SCALE)
/* The approved inventory/machine footer gap: 3 px, 6 px for Close. */
int gutil_footer_hint_gap(enum input_button b);
/* gutil_menu_hint_owned with an explicit glyph-to-label gap. */
void gutil_menu_hint_gap(const struct input_player_frame* input, int x, int y,
						 enum input_button b, const char* text, int gap);
/* Drawn width of a menu hint (glyph or 80 px chord + gap + label), for the
 * default 4 px gap and for an explicit one. Footers space and centre on it. */
int gutil_menu_hint_width_owned(const struct input_player_frame* input,
								enum input_button b, const char* text);
int gutil_menu_hint_width_gap(const struct input_player_frame* input,
							  enum input_button b, const char* text, int gap);

/* ---- the menu ACTIVATION rule (Astra ruling 2, push-0921 gate 2) --------
 *
 * ONE pure decision for "the player pressed activate; what happens?", shared
 * by every menu that drives a pointer and a directional ring through
 * gutil_menu_pointer_focus. That helper decides who OWNS focus and is
 * deliberately unchanged; this one decides what a PRESS does, and it is the
 * only thing allowed to answer that question -- a caller must not fall
 * through to another activation path after asking (ruling 2 item 1).
 *
 * The rule, with Astra ruling 5 D2's last-input-wins extension:
 *   (1) pointer valid AND hovering a target T
 *         T disabled            -> NOTHING; the press is CONSUMED (never a
 *                                  fallback to some other selected control)
 *         T == selected         -> ACTIVATE(T)
 *         T != selected         -> FOCUS_ONLY(T): the highlight jumps under
 *                                  the sprite, the press is consumed, and the
 *                                  NEXT (fresh) press activates
 *       Astra ruling 5 D2: after directional selection, a stationary sprite
 *       on a DIFFERENT enabled target yields to that ring selection. Moving
 *       past gutil_menu_pointer_focus's 4-GUI-unit threshold reassigns the
 *       selection with POINTER origin and restores the hover rule above.
 *       EXCEPT in a destructive confirmation (`destructive_dialog`, Astra
 *       D2-destructive ruling B): there the mismatch is always
 *       FOCUS_ONLY(T), regardless of origin, so a first A can never fire
 *       either answer while the sprite and the ring disagree.
 *   (2) pointer valid, hovering nothing -- decided by the SELECTION'S
 *       ORIGIN, i.e. WHO CHOSE what is highlighted (lead ruling R-A, which
 *       replaced "does the pointer own focus": ownership is recomputed from
 *       IR validity every frame, and that alone turned review A6-1's
 *       destructive confirm from NOTHING into ACTIVATE), narrowed by R-E
 *         origin DIRECTIONAL        -> ACTIVATE(selected) if it is a real,
 *                                      ENABLED target, else NOTHING
 *         origin POINTER or DEFAULT -> NOTHING. POINTER because what is
 *                                      highlighted is what the pointer itself
 *                                      armed; DEFAULT because a pointer user
 *                                      clicking the BACKGROUND of a
 *                                      sub-editor was firing its default
 *                                      Cancel and discarding the typing
 *                                      (review A9-2)
 *   (3) pointer invalid         -> ACTIVATE(selected) if it is a real,
 *                                   ENABLED target, else NOTHING. A DEFAULT
 *                                   selection does activate here: a Classic,
 *                                   a GameCube, or a Remote pointed away must
 *                                   be able to press A on a freshly opened
 *                                   screen.
 *   R-D, across (2) and (3): a DESTRUCTIVE selection additionally requires
 *        the player to have MEANT that target -- the sprite resting ON it
 *        (pointer valid AND hovered == selected) or a direction press having
 *        put the selection there. So "aim at Delete, lower the Remote (IR
 *        goes invalid), press A" is NOTHING, while a D-pad user who walked to
 *        Delete still deletes with one press (review A9-1, the last live
 *        descendant of C-1). Rule (1) needs no extra guard: a hover on the
 *        destructive target already satisfies it.
 *   R-B: a frame that ALSO carries a navigation edge decides NOTHING and the
 *        press is consumed. It outranks (1), (2) and (3), and it replaces the
 *        "decide against the last DISPLAYED selection" rule, which still
 *        fired the target the player was moving away from -- on a confirm,
 *        the destructive answer (review A6-6).
 *
 * Validity transitions alone never steal focus. A mismatched valid hover
 * consumes the press when the pointer/default owns selection; after a
 * direction press, the highlighted ring target fires while the sprite is
 * stationary. Pointer movement past the focus threshold takes ownership.
 *
 * Three obligations live in the CALLER, because they are about frames and
 * about state this function deliberately does not keep:
 *   - `navigated` must be true on any frame carrying a direction edge this
 *     screen acts on, so R-B can consume the press.
 *   - `selection_origin` must be MAINTAINED: POINTER wherever the screen
 *     assigns the selection from a hover, DIRECTIONAL wherever a direction
 *     press moves it, DEFAULT once on screen/modal entry. It must NEVER
 *     change on a pointer validity transition -- that is the whole point.
 *   - `pressed` must be a FRESH press edge: after a FOCUS_ONLY (and on modal
 *     entry) the activate button has to be released before it can press
 *     again, so one hold can never supply the second press.
 *
 * And one obligation about the REST of the frame (lead ruling R-C): a press
 * this helper CONSUMED -- any FOCUS_ONLY, and any NOTHING returned for a
 * press -- must not reach another handler in that same update. A remap that
 * puts activate and Close on one physical button otherwise turns one press
 * into a focus move AND a Close (review A6-4).
 */

/* Target ids are opaque: the helper only ever compares them for equality.
 * Every screen numbers its own controls (and list rows) in one id space. */
#define GUTIL_MENU_TARGET_NONE (-1)

enum gutil_menu_activation {
	GUTIL_MENU_NOTHING = 0, /* the press is consumed; nothing activates */
	GUTIL_MENU_FOCUS_ONLY,  /* the press only moves the highlight */
	GUTIL_MENU_ACTIVATE,    /* activate `target` */
};

/* WHO CHOSE the current selection. Sticky: it changes only when the player
 * acts, never when IR validity flips. */
enum gutil_menu_origin {
	GUTIL_MENU_ORIGIN_DEFAULT = 0, /* the screen/modal picked it on entry */
	GUTIL_MENU_ORIGIN_DIRECTIONAL, /* a direction press moved it */
	GUTIL_MENU_ORIGIN_POINTER,     /* a hover assignment or a FOCUS_ONLY */
};

struct gutil_menu_activate_state {
	bool pressed;       /* a FRESH activate edge on this frame */
	bool navigated;     /* a direction edge on this frame (R-B) */
	bool pointer_valid; /* input_pointer() said yes this frame */
	enum gutil_menu_origin selection_origin;
	int hovered; /* target under the sprite, or ..._NONE */
	bool hovered_enabled;
	int selected; /* the HIGHLIGHTED target, or ..._NONE */
	bool selected_enabled;
	/* This selection DESTROYS something if activated -- the caller says so,
	 * because only it knows (R-D). A control that merely OPENS a confirmation
	 * is not destructive: the confirmation is where the destructive answer
	 * lives, and that dialog declares it. */
	bool selected_destructive;
	/* This WHOLE DIALOG is a destructive confirmation -- one whose answers
	 * include ending a session, deleting or overwriting saved data, or
	 * discarding work (split Turn off, Delete world, Remove server, Confirm
	 * save and quit, Discard sign edits). A per-dialog property the caller
	 * declares; it qualifies Astra ruling 5 D2 (Astra D2-destructive ruling,
	 * 2026-09-24, option B): with a valid pointer hovering an enabled
	 * button OTHER than the selected one, the press is ALWAYS FOCUS_ONLY
	 * onto the hovered button, whatever the selection's origin -- it
	 * executes neither answer. The next FRESH press, focus now reconciled
	 * (hovered == selected), activates under rule (1). Without a mismatch
	 * nothing changes: rules (1)-(3), R-D and the dialog's safe default
	 * apply as before. False on every other screen, which keeps D2. */
	bool destructive_dialog;
	/* The HOVERED target is a list row whose activation only SELECTS it --
	 * it never launches, applies, opens or destroys anything (the Load
	 * world list, the Texture packs list). The caller says so, because only
	 * it knows. When the sprite rests on such a row while the selection is
	 * elsewhere, rule (1)'s FOCUS_ONLY is replaced by ACTIVATE(hovered): one
	 * click chooses the row (ui-build-1026 T01, design-hub-1024 REPORT 1-2).
	 * This is safe because "activating" the row is itself only a selection;
	 * the explicit action (Play, Apply) still needs its own fresh press with
	 * the sprite ON it. Everything else is unchanged: the press must be
	 * fresh, R-B still consumes it, a disabled row still consumes it, D2
	 * (a DIRECTIONAL selection with a stationary sprite) still wins, and a
	 * destructive_dialog ignores this flag (ruling B's FOCUS_ONLY stays).
	 * False on every other target, which keeps rule (1) exactly. */
	bool hovered_selects_only;
};

struct gutil_menu_decision {
	enum gutil_menu_activation action;
	int target;   /* ACTIVATE / FOCUS_ONLY: the id. Otherwise ..._NONE. */
	int selected; /* the selection this press leaves behind -- only
				   * FOCUS_ONLY ever changes it. */
	enum gutil_menu_origin selection_origin; /* its origin afterwards; only
											  * FOCUS_ONLY changes it, to
											  * POINTER. */
};

struct gutil_menu_decision
gutil_menu_activate_decide(struct gutil_menu_activate_state in);

// Read-only accessor for the 256-entry glyph ink-width table
// gutil_reset_font() populates from the bound texture_font (glyph pixel
// width, measured off the real texture, scaled to an 8-unit space -- see
// gutil_reset_font's own comment). The SAME table gutil_font_width/
// gutil_text(_rgb) already read internally; exposed so a non-GL/GX consumer
// (render_sign_text_draw.c's world-space sign-text pass) can mirror this
// engine's GUI font metrics without linking a texture/GL context itself.
// Entries are only meaningful once gutil_reset_font has run at least once
// (texture.c's boot font load does this); pre-init entries are zero.
const uint8_t* gutil_font_glyph_widths(void);
void gutil_texquad_col(int x, int y, int tx, int ty, int sx, int sy, int width,
					   int height, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void gutil_texquad(int x, int y, int tx, int ty, int sx, int sy, int width,
				   int height);
void gutil_texquad_rt(int x, int y, int tx, int ty, int sx, int sy, int width,
					  int height);
void gutil_texquad_rt_any(int x, int y, float angle, int tx, int ty, int sx,
						  int sy, float width, float height);
void gutil_bg(void);
void gutil_reset_font(struct tex_gfx* tex);
int  gutil_font_width(const char* str, int scale);
void gutil_text(int x, int y, const char* str, int scale, bool shadow);
// gutil_text with an explicit BASE color (Beta FontRenderer's color param --
// e.g. GuiButton's 0xE0E0E0 idle / 0xFFFFE0 hover, which the 16-entry chat
// palette cannot express). Section-sign codes still override mid-string,
// exactly like Beta.
void gutil_text_rgb(int x, int y, const char* str, int scale, bool shadow,
					uint8_t r, uint8_t g, uint8_t b);
// gutil_text_rgb with an explicit vertex ALPHA (shadow inherits it, like
// Beta's drawStringWithShadow carrying the packed-color alpha through both
// passes). Caller owns the blending bracket -- alpha < 255 only reads
// correctly with gfx_blending(MODE_BLEND) + gfx_alpha_test(false) active
// (the chat-fade consumer in screen_ingame.c is the precedent).
void gutil_text_rgba(int x, int y, const char* str, int scale, bool shadow,
					 uint8_t r, uint8_t g, uint8_t b, uint8_t a);
// Rotated, horizontally-centered text for the title splash (GuiMainMenu's
// glTranslate/glRotate/glScale transform, which the 2D pass has no matrix
// stack for): the string is centered on the anchor with its top one glyph
// height above it (Beta draws at local (0,-8) inside the scaled space) and
// every glyph quad's corners are pre-rotated as one rigid unit. The shadow
// offset rotates WITH the string, exactly like Beta's in-matrix shadow.
// Section-sign codes are not processed (the splash is a plain string).
void gutil_text_rot(int anchor_x, int anchor_y, const char* str, int scale,
					float angle_deg, bool shadow, uint8_t r, uint8_t g,
					uint8_t b);
void gutil_text_rot_zoom(int anchor_x, int anchor_y, const char* str, int scale,
                        float angle_deg, float zoom, bool shadow,
                        uint8_t r, uint8_t g, uint8_t b);
void gutil_draw_item(struct item_data* item, int x, int y, int layer);
void gutil_draw_item_sized(struct item_data* item, int x, int y, int size, int layer);

// Beta GuiTextField.drawTextBox [740c583]: 0xA0A0A0 border rect, black
// fill, 0xE0E0E0 text at (x+4, y+(h-8)/2), and -- while show_cursor -- the
// trailing '_' blink glyph. (x, y) are PHYSICAL pixels; w_units logical
// (field height is Beta's fixed 20 units). Callers own the blink cadence
// (cursorCounter/6 % 2 at 20 Hz) and focus state.
void gutil_text_field(int x, int y, int w_units, const char* text,
					  bool show_cursor);

/* Shared Beta GuiButton geometry. x/y are physical pixels; width is in
 * logical GUI units, matching the decompile constructors. */
struct gutil_button_bounds {
	int x;
	int y;
	int width;
	int height;
};

struct gutil_button_bounds gutil_button_rect(int x, int y, int w_units);
void gutil_menu_focus_border(const struct gutil_button_bounds* bounds);
bool gutil_button_hit(const struct gutil_button_bounds* bounds, float x,
					  float y);
void gutil_button_draw(int x, int y, int w_units, const char* label,
					   bool enabled, bool hover);

// Pure predicate backing the button half-split guard: true iff w_units
// splits losslessly into the shared draw's two texture halves (half*2 ==
// w_units). [740c583 GuiButton.drawButton] always halves via integer
// width/2; an odd w_units drops a 1-unit seam the old per-screen private
// draws (lw=bw/2, rw=bw-lw) would not have dropped (7a10eb4's recorded
// debt). gutil_button_draw_bg asserts this at the split site; exposed so
// the invariant is unit-testable without a GL context.
bool gutil_button_even_width(int w_units);

// Pure: charged width in PHYSICAL pixels for a dwell/hold-to-confirm fill
// at fill_frac (clamped to [0,1]) of a w_units-wide button -- the
// Wii-controller hold-to-confirm carve-out #2 (Beta's GuiButton has no
// dwell state). Exposed so the fill math is unit-testable without a GL
// context; gutil_button_draw_dwell is its only production caller.
int gutil_button_dwell_fill_px(int w_units, float fill_frac);

// Composes gutil_button's bg+label draw with an optional dwell/hold-to-
// confirm charge-fill overlay in between. fill_frac <= 0 draws no overlay
// -- output is then byte-identical to gutil_button_draw. Otherwise dims
// the button's UNFILLED remainder (black, alpha 110, untextured) between
// the bg and label draws so the label stays legible on top and the
// undimmed portion reads as charge filling left-to-right. See
// screen_death.c (the Respawn / Title-menu hold buttons) for the
// reference caller.
void gutil_button_draw_dwell(int x, int y, int w_units, const char* label,
							 bool enabled, bool hover, float fill_frac);

/* ---- THE HINT ROW (controller UI redesign, maintainer picks 2026-10-31) ---
 * ONE row of button prompts for every menu and the gameplay HUD: no glyph is
 * drawn inside a panel any more. Approved look (tmp/jobs/controller-ui-1031
 * screens.py row_big + WIRING-PLAN Q1): 24 px glyphs, 16 px white menu text,
 * on a black strip at alpha 120 that is 28 px tall (2 px above and below the
 * glyph) and pads its content 8 px left and right. Inside an item a pair of
 * glyphs (L+R "Category") is 4 px apart and the label starts 5 px after the
 * last glyph; items are 14 px apart.
 *
 * Placement, inside the frame x,y,width,height the caller hands over (the
 * whole screen, or one split pane):
 *   - the strip's bottom edge is 24 px above the frame's bottom full screen
 *     (TV-safe 456 of 480) and 16 px above it in a split pane (224 of 240);
 *   - LEFT: the strip starts at the TV-safe side edge, 5% of the width rounded
 *     up (41 at 802, 32 at 640); CENTRE: the strip is centred in the frame.
 *   Either way the strip stays between the two side edges.
 *
 * Overflow, in this order, never anything else: (1) the item gap shrinks from
 * 14 to 8; (2) items are dropped, lowest `priority` first (ties: the rightmost
 * first), and the gap is retried at 14 after every drop. An item with `keep`
 * (Close / Back) is never dropped and no label is ever truncated; when only
 * kept items remain and they still do not fit, the row is laid out at gap 8,
 * centred on the frame, and `fits` is false.
 *
 * Glyphs come ONLY from input->glyphs[role] (the owner's bound buttons). A
 * role with no drawable glyph draws nothing; an item none of whose roles has
 * one is left out (a label naming no button would be a lie). A chord draws
 * at gutil_prompt_glyph's own width (modifier + "+" + button, 60 px at 24 px);
 * the older 32 px helpers above are unchanged (their chord stays 80 px).
 *
 * The layout is pure (no gfx call) and the draw reads ONLY the layout, so the
 * pointer hit rectangle of an item is exactly what is drawn for it: the item's
 * glyph-to-label span, the strip's full height. */
#define GUTIL_HINT_ROW_MAX 8
#define GUTIL_HINT_GLYPH 24
#define GUTIL_HINT_TEXT (8 * GFX_GUI_SCALE)
#define GUTIL_HINT_STRIP_H 28
#define GUTIL_HINT_STRIP_ALPHA 120
#define GUTIL_HINT_PAD 8
#define GUTIL_HINT_PAIR_GAP 4
#define GUTIL_HINT_LABEL_GAP 5
#define GUTIL_HINT_GAP 14
#define GUTIL_HINT_GAP_TIGHT 8
#define GUTIL_HINT_BOTTOM_FULL 24
#define GUTIL_HINT_BOTTOM_SPLIT 16
enum gutil_hint_align { GUTIL_HINT_ALIGN_LEFT, GUTIL_HINT_ALIGN_CENTRE };
struct gutil_hint_item {
	enum input_button roles[2];
	int role_count;    /* 1 or 2; 0 reads as 1 */
	const char* label; /* never truncated */
	int priority;      /* higher stays longer when the row overflows */
	bool keep;         /* Close / Back: never dropped */
};
struct gutil_hint_cell {
	int item; /* index into the caller's items[] */
	/* Pointer hit area == drawn extent: glyph(s) to label end, strip height. */
	struct gutil_button_bounds rect;
	int glyph_count;
	int glyph[2], glyph_x[2], glyph_w[2]; /* glyph codes from input->glyphs */
	const char* label;
	int label_x, label_w;
};
struct gutil_hint_row {
	struct gutil_button_bounds strip; /* width 0: nothing to draw */
	int glyph_y, text_y, gap;
	bool fits;
	int count;
	struct gutil_hint_cell cells[GUTIL_HINT_ROW_MAX];
};
/* Returns the TV-safe side margin used for `width`: ceil(width * 5 / 100). */
int gutil_hint_row_margin(int width);
/* Lays out at most GUTIL_HINT_ROW_MAX items (more are ignored). Returns true
 * when at least one item is laid out. */
bool gutil_hint_row_layout(const struct input_player_frame* input, int x,
						   int y, int width, int height, bool split,
						   enum gutil_hint_align align,
						   const struct gutil_hint_item* items, int count,
						   struct gutil_hint_row* out);
/* The caller's item index under (px, py), or -1. */
int gutil_hint_row_hit(const struct gutil_hint_row* row, float px, float py);
void gutil_hint_row_draw(const struct gutil_hint_row* row);

/* critique-1028 restored CavEX hint row (rules in screen_ingame.c; drawn with gutil_control_icon_owned). */
#define SCREEN_INGAME_HINT_MAX 8
enum screen_ingame_hint_target {
	SCREEN_INGAME_HINT_NONE, SCREEN_INGAME_HINT_BLOCK, SCREEN_INGAME_HINT_ENTITY
};
enum screen_ingame_held {
	SCREEN_INGAME_HELD_NONE, SCREEN_INGAME_HELD_BLOCK, SCREEN_INGAME_HELD_FOOD,
	SCREEN_INGAME_HELD_AIR_USE, SCREEN_INGAME_HELD_OTHER
};
struct screen_ingame_hint_ctx {
	enum screen_ingame_hint_target target;
	const char* entity_left; /* the entity's leftClickText, or NULL */
	const char* entity_right;
	bool block_usable; /* the targeted block has onRightClick */
	enum screen_ingame_held held;
};
struct screen_ingame_hint {
	int button; /* enum input_button */
	const char* label;
};
/* hint-audit-1030: ONE table, gameplay ACTION -> the input ROLE the gameplay
 * code polls for it. The hint row names its buttons ONLY through this table,
 * and screen_ingame / local_menu_host poll the open and pause edges through
 * it too, so a label can never name a different role than the one that acts.
 * The per-device BUTTON comes from that role's binding (input_glyph_frame
 * over the same input_map the poller reads), so docs/controls_sheet.md ->
 * config_wii.json -> glyph is one chain; test_ui_geometry pins it per family.
 * (2026-10-30 real-Wii bug: the row used IB_INVENTORY, the menu CLOSE role,
 * and drew Remote "1" for Inventory; the open is A.) */
enum screen_ingame_action {
	SCREEN_INGAME_ACT_OPEN_INVENTORY,
	SCREEN_INGAME_ACT_JUMP,
	SCREEN_INGAME_ACT_BREAK, /* Mine / Punch / an entity's left-click text */
	SCREEN_INGAME_ACT_USE, /* Place / Use / Eat / an entity's right-click */
	SCREEN_INGAME_ACT_PAUSE,
	SCREEN_INGAME_ACT_COUNT
};
extern const enum input_button
	screen_ingame_action_role[SCREEN_INGAME_ACT_COUNT];
int screen_ingame_hint_row(const struct screen_ingame_hint_ctx* c,
	struct screen_ingame_hint out[SCREEN_INGAME_HINT_MAX]);
/* A2 (10-31): the gameplay HUD per pane. A pane at most
 * SCREEN_INGAME_SPLIT_PANE_H tall is a split pane (the 240 half of 480). */
#define SCREEN_INGAME_SPLIT_PANE_H 240
#define SCREEN_INGAME_ROW_HOTBAR_GAP 1
bool screen_ingame_split_pane(int height);
/* The hint row's band in a width x height pane: y/height are the strip's,
 * x/width the TV-safe span it may use. */
struct gutil_button_bounds screen_ingame_row_band(int width, int height);
int screen_ingame_hotbar_top(int height, int scale);
/* A pane whose disconnect card is up (its own split recovery, or the
 * single-player modal in P1's view) draws no gameplay HUD under the card. */
bool screen_ingame_card_hides_hud(bool pane_recovery, bool primary_recovery);
/* Every gameplay HUD rectangle of a pane, pane-local, for menus that must
 * keep clear of them (Q3: split panels end above the hearts). */
struct screen_ingame_hud_layout {
	struct gutil_button_bounds row;    /* the hint row band */
	struct gutil_button_bounds hotbar; /* bar + selection frame overhang */
	struct gutil_button_bounds hearts; /* hearts + armour + air block */
};
void screen_ingame_hud_layout(int width, int height,
	struct screen_ingame_hud_layout* out);
void screen_ingame_hint_items(const struct screen_ingame_hint* hints, int n,
	struct gutil_hint_item* items);
/* A5 (maintainer 10-31): the player's block coordinates and the frame rate,
 * always on in game outside F3, top-left of every pane just UNDER the notice
 * card area (card 32 tall at 24,16 plus its 2 px edge), so they never move
 * whether a card is up or not: "X: 123  Y: 64  Z: -45" then "60 FPS", 16 px
 * pack-font text with the usual shadow. ui.show_coordinates and ui.show_fps
 * (JSON true/false or 1/0, default true) switch each line. Pane-local. */
#define SCREEN_INGAME_COORDS_X GUTIL_NOTICE_ANCHOR_X
#define SCREEN_INGAME_COORDS_Y (GUTIL_NOTICE_ANCHOR_Y + GUTIL_NOTICE_CARD_H + 4)
#define SCREEN_INGAME_COORDS_PITCH 18
/* Line 0 = coords (when shown), then FPS right under it; a hidden line takes
 * no space. Height covers the text and its shadow (16 + 2). Returns lines. */
int screen_ingame_coords_layout(bool coords, int coords_w, bool fps, int fps_w,
	struct gutil_button_bounds out[2]);
struct config;
bool screen_ingame_ui_flag(struct config* c, const char* key);
/* The block position the F3 overlay uses: floor of the feet position. */
void screen_ingame_block_pos(const float pos[3], int out[3]);
void screen_ingame_leave_bed_layout(int width, int height, int glyph_w,
	int label_w, struct gutil_button_bounds* cell,
	struct gutil_button_bounds* label);

/* ---- THE DISCONNECT CARD (controller UI redesign A4, maintainer pick 10-31:
 * opus_variants.py disc_card, "small centre card") ---------------------------
 * ONE pure layout for the single-player modal (screen.c) and every split
 * pane's local recovery (local_menu_host.c): a Beta-grey card centred in the
 * frame over the dimmed game. Full screen: pad 14, a 64 px controller
 * picture and 16 px text rows 20 apart beside it (title, detail, status);
 * split: pad 10, a 48 px picture. In READY the Resume button (the owner's
 * [A] glyph at 24 px + "Resume", 32 tall) sits under the body, centred. The
 * pointer hit rectangle of Resume IS the drawn button rectangle (`button`),
 * and both update paths hit-test exactly that. */
struct controller_recovery_card;
struct recovery_card_layout {
 struct gutil_button_bounds card,picture,button;
 int text_x,row_y[3],rows,glyph_x,glyph_y,glyph_w,label_x,label_y;
 bool has_button;
};
void recovery_card_layout(int x,int y,int width,int height,bool split,
 const struct controller_recovery_card* c,bool resume,int glyph,
 struct recovery_card_layout* out);
void recovery_card_draw(const struct recovery_card_layout* l,
 const struct controller_recovery_card* c,int glyph,bool focus);

#endif
