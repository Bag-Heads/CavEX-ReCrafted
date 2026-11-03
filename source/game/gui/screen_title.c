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

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../../config.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/input_inventory.h"
#include "../../platform/time.h"
#include "../../network/level_archive.h"
#include "../game_state.h"
#include "frontend_presentation.h"
#include "import_convert_notice.h"
#include "loading_notice.h"
#include "loading_tips.h"
#include "play_session.h"
#include "screen.h"

/* THE APPROVED TITLE (design-wiring/title-green; visual authority
 * docs/ui_design/front-end-refined/studio.js `screens.main` + studio.css
 * .studio-wordmark / .studio-splash / .studio-title-button / .studio-footer;
 * contract docs/ui_design/approved/native-play-flow.md):
 *
 *   "A quiet title: one wordmark, one splash, four choices. The scenery
 *    moves; the menu stays still."
 *
 * Composed on a 640x480 CANVAS scaled by height/480 and centred horizontally
 * (the pack screen's own mapping), in PHYSICAL pixels:
 *   wordmark  approved CAVEX + RE-CRAFTED EDITION art, 320x148 at 1:1,
 *             top 20, face centre at width/2 (October 7 title approval);
 *             absent art keeps "CaveWii", top 37, shadow offset (3,4)
 *   splash    14 px, -9 degrees, beside the tablet at anchor (551,157),
 *             fitted to 108 px; absent art keeps the previous placement
 *   rows      FIVE full-width buttons (160, 166 + 48*i, 320, 44), 20 px label:
 *             Play game / Tutorial world / Texture Packs / Options / Quit game
 *             (ui-build-1026 T03, Astra's approved title,
 *             tmp/jobs/design-round-1026/astra/title.json: 4 px gaps)
 *   footer    y 432, h 36: Move / Select / Quit from the DETECTED bindings
 *
 * What is Beta and what is not [740c583 GuiMainMenu]:
 *   KEPT: the splash DATA SOURCE (title/splashes.txt of the active pack,
 *     reservoir-sampled on a private LCG, NO splash without a list, the four
 *     fixed-date overrides, re-rolled per entry), keyTyped {} (D-pad + pointer
 *     is the carve-out), Options -> GuiOptions, Quit -> shutdown, Mods and
 *     Texture Packs -> GuiTexturePacks, and the click-before-action sound.
 *   APPROVED PRESENTATION (maintainer, September 7 package): the five-button
 *     y0 = height/4 + 48 column, the mclogo strips, the yellow 20-degree
 *     pulsing splash and the version / copyright lines are replaced by the
 *     composition above. The maintainer requires the splash pulse (2026-09-22 correction).
 *   PLAY: Singleplayer + Multiplayer collapse into ONE "Play game" row that
 *     opens the retained Play session (play_session.h: Load / Create / Join
 *     tabs behind screen_select_world). Beta's session==null Multiplayer
 *     disable therefore moves to the Join tab; every title row is enabled.
 *
 * DEBUG-ONLY STAMP: the CW_BUILD_ID that used to sit top-left on every title
 * frame (so proofs self-identify) is drawn only while gstate.debug_overlay is
 * on, as an 8 px line at canvas y 470. The subtitle is part of the approved
 * title art; there is no copyright line. The project line lives in
 * source / LICENSE.
 *
 * Input is IR-pointer + D-pad dual-drive per the Wiimote floor rule; HOME
 * quits, mirroring the Quit game row (Wii-natural). */

#ifndef CW_BUILD_ID
#define CW_BUILD_ID "unstamped"
#endif

enum {
	TB_PLAY = 0,
	TB_TUTORIAL,
	TB_PACKS,
	TB_OPTIONS,
	TB_QUIT,
	TB_COUNT,
};

static const char* const title_labels[TB_COUNT] = {
	"Play game",
	"Tutorial",
	"Texture Packs",
	"Options",
	"Quit game",
};

/* The approved composition's canvas numbers (studio.js:48-51, studio.css). */
#define TT_WORDMARK_Y 37
#define TT_WORDMARK_FONT 76
#define TT_WORDMARK_SHADOW_X 3
#define TT_WORDMARK_SHADOW_Y 4
#define TT_LOGO_Y 20
#define TT_LOGO_FACE_CENTRE_X 160
/* M1-beta-menu.png: yellow face bbox (507,148)-(595,169), with the
 * reference tile.js drawing at anchor (551,157). Keep that anchor for all
 * splashes, fitting long strings to the free space beside the tablet.
 * At 14 px, 108 px leaves room for rotation, shadow and the last glyph quad. */
#define TT_LOGO_SPLASH_X 551
#define TT_LOGO_SPLASH_Y 143
#define TT_LOGO_SPLASH_MAX_W 108
#define TT_SPLASH_RIGHT 48
#define TT_SPLASH_Y 122
#define TT_SPLASH_FONT 14
#define TT_SPLASH_ANGLE -9.0F
#define TT_SPLASH_MAX_W 280
#define TT_ROW_X 160
#define TT_ROW_Y0 166
#define TT_ROW_PITCH 48
#define TT_ROW_W 320
#define TT_ROW_H 44
#define TT_ROW_FONT 20
#define TT_FOOTER_Y 432
#define TT_FOOTER_H 36
#define TT_FOOTER_GAP 24
#define TT_STAMP_Y 470
#define TT_STAMP_FONT 8

static int selected = TB_PLAY;
/* Astra menu review item 13: hover used to assign `selected` on EVERY update
 * while the pointer was merely VALID, so a Wii Remote resting anywhere near a
 * button overwrote directional focus the instant after a D-pad step. This is
 * the movement-based arbitration screen_options.c already uses -- the pointer
 * owns focus only after it actually MOVES, and any directional press takes it
 * back. */
static struct gutil_menu_focus focus;
static bool pointer_available;
/* Whether the POINTER owned focus on the last update. */
static bool pointer_owns_focus_now;
/* WHO CHOSE the highlight (lead ruling R-A, closing review A6-1). It is what
 * rule (2) asks instead of "does the pointer own focus" -- ownership is
 * recomputed from IR validity every frame, so one invalid frame used to turn
 * a press over empty space into an activation of whatever the POINTER had
 * armed. This is sticky: it moves only when the player does. */
static enum gutil_menu_origin title_origin;
/* The inputs the NEXT activate press will be decided against, captured at the
 * END of update -- i.e. the state the frame the renderer draws carries into
 * the next one, with `selected` already being what is on screen. The
 * cross-lane observer re-runs the SAME decision on it (ruling 2 item 7), so
 * it cannot answer with different logic than the press. */
static struct gutil_menu_activate_state title_predict;
static float pointer_x, pointer_y, pointer_angle;
static char splash_text[128];
static uint32_t splash_rng;

static bool title_button_enabled(int idx) {
	/* Every approved row is enabled. Beta's session==null Multiplayer disable
	 * moved with the Multiplayer path into the Play session's Join tab. */
	return idx >= 0 && idx < TB_COUNT;
}

/* ---- the canvas: render and hit test share ONE mapping -------------------- */

/* The pack screen's mapping, kept local and small (no production refactor):
 * the 640x480 composition scaled by height/480 and centred horizontally. */
struct title_canvas {
	int ox;  /* left edge of the centred 640-wide canvas */
	float u; /* pixels per canvas unit */
};

static struct title_canvas title_canvas_of(int width, int height) {
	struct title_canvas c;
	c.u = height > 0 ? (float)height / 480.0F : 1.0F;
	int canvas = (int)(640.0F * c.u + 0.5F);
	c.ox = (width - canvas) / 2;
	if(c.ox < 0)
		c.ox = 0;
	return c;
}

static int tt_d(const struct title_canvas* c, int v) {
	return (int)((float)v * c->u + 0.5F);
}

static int tt_x(const struct title_canvas* c, int v) {
	return c->ox + tt_d(c, v);
}

/* The ONE row rectangle -- what render2D paints and what the pointer is
 * tested against, so an edge pixel cannot be drawn as a row and refused as a
 * press. NOT gutil_button_rect: that is Beta's fixed 20-unit (40 px) button,
 * and the approved row is 44 px. */
static struct fep_rect title_row_rect(int width, int height, int idx) {
	struct title_canvas c = title_canvas_of(width, height);
	return fep_rect(tt_x(&c, TT_ROW_X), tt_d(&c, TT_ROW_Y0 + TT_ROW_PITCH * idx),
					tt_d(&c, TT_ROW_W), tt_d(&c, TT_ROW_H));
}

static uint32_t splash_next(void) {
	splash_rng = splash_rng * 1664525u + 1013904223u;
	return splash_rng;
}

static void title_load_splash(void) {
	/* Cosmetic-only randomness on a private LCG -- the simulation RNG
	 * streams are never touched from the client. Re-rolled per screen entry,
	 * like Beta's per-construction roll. */
	splash_rng ^= (uint32_t)CLOCK_SEED_TIME() ^ 0x9E3779B9u;
	/* ci-1101/18: no list, no splash. Beta's "missingno" is only ever seen
	 * when its jar's list is unreadable; CavEX ships NO title/splashes.txt
	 * (Beta's is Mojang content), so the default install showed that
	 * fixture-looking word on every boot. title_draw_splash skips an empty
	 * string; the fixed-date greetings below still land, as in initGui. */
	splash_text[0] = 0;

	const char* pack = config_read_path(&gstate.config_user,
										"paths.texturepack", "assets");
	const char* candidates[2] = {pack, "assets"};
	size_t count = strcmp(pack, "assets") == 0 ? 1 : 2;
	for(size_t k = 0; k < count; k++) {
		char path[256];
		snprintf(path, sizeof(path), "%s/title/splashes.txt", candidates[k]);
		FILE* f = fopen(path, "r");
		if(!f)
			continue;
		/* Reservoir-sample one trimmed non-empty line so the whole file
		 * never needs to sit in memory (SD cards carry big splash lists). */
		char line[sizeof(splash_text)];
		size_t seen = 0;
		while(fgets(line, sizeof(line), f)) {
			size_t len = strlen(line);
			while(len > 0
				  && (line[len - 1] == '\n' || line[len - 1] == '\r'
					  || line[len - 1] == ' ' || line[len - 1] == '\t'))
				line[--len] = 0;
			size_t start = 0;
			while(line[start] == ' ' || line[start] == '\t')
				start++;
			if(len <= start)
				continue;
			seen++;
			if(splash_next() % seen == 0)
				memmove(splash_text, line + start, len - start + 1);
		}
		fclose(f);
		if(seen > 0)
			break;
	}

	/* The four fixed-date overrides land AFTER the roll, like initGui. */
	time_t now = time(NULL);
	struct tm* lt = localtime(&now);
	if(lt) {
		int month = lt->tm_mon + 1, day = lt->tm_mday;
		if(month == 11 && day == 9)
			strcpy(splash_text, "Happy birthday, ez!");
		else if(month == 6 && day == 1)
			strcpy(splash_text, "Happy birthday, Notch!");
		else if(month == 12 && day == 24)
			strcpy(splash_text, "Merry X-mas!");
		else if(month == 1 && day == 1)
			strcpy(splash_text, "Happy new year!");
	}
}

static void screen_title_reset(struct screen* s, int width, int height) {
	(void)s;
	(void)width;
	(void)height;
	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;
	input_pointer_enable(true);
	selected = TB_PLAY;
	title_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	/* Same reason as screen_multiplayer_setup.c's: the observer's prediction
	 * must describe THIS visit, not the previous one. */
	title_predict = (struct gutil_menu_activate_state) {
		.pressed = true,
		.hovered = GUTIL_MENU_TARGET_NONE,
		.selection_origin = title_origin,
		.selected = selected,
		.selected_enabled = title_button_enabled(selected),
	};
	focus = (struct gutil_menu_focus) {0};
	pointer_owns_focus_now = false;
	title_load_splash();
}

/* ---- Tutorial world (ui-build-1026 T03) ----------------------------------
 *
 * The row opens the SHIPPED world (tools/tutorial_world/build_tutorial.py,
 * staged by tools/make_release.sh as saves/CavEX Tutorial/) through the
 * SAME load the Load tab's Play world issues: client creative armed from
 * level.dat, loading_reason_begin(LOAD_SAVE), then the asyncsnd world-entry
 * gate, which sends SRPC_LOAD_WORLD and shows the loading screen. No new
 * tutorial screen or curriculum: the world's own signs and chests teach.
 *
 * RESUME, NOT A FRESH COPY (decided here, T03): the row resumes whatever is
 * in that folder, i.e. the player's tutorial progress. A Wii U-style fresh
 * copy needs a safe directory copy on the SD card, and none exists yet --
 * world_copy.c is ui-build-1026 T07/T08, still unwritten -- so a copy here
 * would be a second, untested copier on the save path. Resume is also what
 * the release notes already promise (docs/release-install.md 3a: progress
 * lives in that folder; a newer release zip replaces it). Revisit once
 * T07/T08's copier is proven.
 *
 * A folder without level.dat is "not on this card": an honest dialog says so
 * and offers Play game (the ordinary world list) or Back -- never a dead row.
 * A Beta-format world that would need the convert notice is not loaded from
 * here; it opens the Load tab with the tutorial selected, so the notice the
 * Load tab owns still asks first (the shipped world is CaveWii save version
 * 5 and never takes this branch). */
static void title_tutorial_missing_answer(bool yes) {
	if(yes) {
		play_session_enter_fresh();
		screen_set(&screen_select_world);
	} else {
		screen_set(&screen_title);
	}
}

static void title_tutorial_explain(const char* line1, const char* line2) {
	screen_confirm_setup(line1, line2, "Play game", "Back",
						 SCREEN_CONFIRM_ANSWER_NO,
						 title_tutorial_missing_answer);
	screen_set(&screen_confirm);
}

static const char* title_tutorial_path(char* out, size_t cap) {
	const char* saves
		= config_read_string(&gstate.config_user, "paths.worlds", "saves");
	const char* directory = SCREEN_TITLE_TUTORIAL_DIR;
	int n = snprintf(out, cap, "%s/%s", saves, directory);
	if(n <= 0 || (size_t)n >= cap)
		return NULL;
	/* 0.1.0 cards retain the old folder. Prefer the new release's world;
	 * an existing but unreadable new world must not resume the old one. */
	struct stat st;
	if(stat(out, &st) != 0 && errno == ENOENT) {
		directory = SCREEN_TITLE_TUTORIAL_LEGACY_DIR;
		n = snprintf(out, cap, "%s/%s", saves, directory);
		if(n <= 0 || (size_t)n >= cap)
			return NULL;
	}
	return directory;
}

static bool title_tutorial_present(const char* path) {
	char probe[600];
	int n = snprintf(probe, sizeof(probe), "%s/level.dat", path);
	if(n <= 0 || (size_t)n >= sizeof(probe))
		return false;
	FILE* f = fopen(probe, "rb");
	if(!f)
		return false;
	fclose(f);
	return true;
}

static void title_start_tutorial(void) {
	char path[512] = {0};
	const char* directory = title_tutorial_path(path, sizeof(path));
	if(!directory || !title_tutorial_present(path)) {
		fprintf(stderr, "[title] tutorial world not found: %s/level.dat\n",
				path);
		title_tutorial_explain("Tutorial world not found - reinstall CavEX",
							   "Expected saves/" SCREEN_TITLE_TUTORIAL_DIR);
		return;
	}
	/* A fresh Play session that REMEMBERS the tutorial, so a Back or a failed
	 * load returns to the Load tab with it selected. */
	play_session_enter_fresh();
	(void)play_session_remember_world(directory);

	/* The Load tab's creative arming (sworld_arm_client_creative): always
	 * assigned, so a creative world cannot leak its mode into this one. */
	gstate.config_creative = false;
	bool needs_convert = false;
	string_t probe;
	string_init_set_str(probe, path);
	struct level_archive la;
	if(level_archive_create(&la, probe)) {
		gstate.config_creative = level_archive_is_creative(&la);
		needs_convert = import_convert_notice_needed(&la);
		level_archive_destroy(&la);
	}
	string_clear(probe);
	if(needs_convert) {
		screen_set(&screen_select_world);
		return;
	}

	loading_reason_begin(0, LOAD_SAVE);
	if(!loading_notice_gate_load_world(path, &screen_select_world)) {
		loading_reason_end(0);
		fprintf(stderr, "[title] tutorial world load request refused: %s\n",
				path);
		title_tutorial_explain("Couldn't start the tutorial world.",
							   "Try it from Play game.");
	}
}

static void title_activate(int idx) {
	if(!title_button_enabled(idx))
		return;
	/* Beta plays "random.click" 1.0F/1.0F for an enabled button BEFORE the
	 * action runs [740c583: GuiScreen.mouseClicked:57-59]. */
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	switch(idx) {
		case TB_PLAY:
			/* FRESH ENTRY (GATE3-PLAY-PLAN.md item 5): the title is the ONE
			 * reset point of the retained Play session -- world identity,
			 * remembered server and draft go, the Load tab is selected -- and
			 * then the Load tab (Select World) opens. Tab switches inside the
			 * session resume; only this row starts over. */
			play_session_enter_fresh();
			screen_set(&screen_select_world);
			break;
		case TB_TUTORIAL: title_start_tutorial(); break;
		case TB_PACKS: /* id 3 -> GuiTexturePacks */
			screen_texturepacks_set_return(&screen_title);
			screen_set(&screen_texturepacks);
			break;
		case TB_OPTIONS: /* id 0 -> GuiOptions */
			screen_options_set_return(&screen_title);
			screen_set(&screen_options);
			break;
		case TB_QUIT: /* id 4 -> mc.shutdown() */
			gstate.quit = true;
			break;
		default: break;
	}
}

/* The row the SPRITE rests on, enabled or not (all are, today) -- a disabled
 * one would still be a hover, because ruling 2 makes it CONSUME the press
 * rather than fall through to whatever is selected. GUTIL_MENU_TARGET_NONE
 * when the pointer is absent or resting on no row at all. Same rect as the
 * render. */
static int title_pointer_button(void) {
	if(!pointer_available)
		return GUTIL_MENU_TARGET_NONE;
	for(int k = 0; k < TB_COUNT; k++) {
		struct fep_rect r = title_row_rect(gfx_width(), gfx_height(), k);
		if(fep_rect_hit(r, pointer_x, pointer_y))
			return k;
	}
	return GUTIL_MENU_TARGET_NONE;
}

static float menu_motion_time;

static void screen_title_update(struct screen* s, float dt) {
	if(dt > 0.0F && true) menu_motion_time = fmodf(menu_motion_time + dt, 128.0F);
	(void)s;
	(void)dt;

	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);
	bool navigating = input_pressed(IB_GUI_UP) || input_pressed(IB_GUI_DOWN);
	bool pointer_owner = gutil_menu_pointer_focus(&focus, pointer_available,
												  pointer_x, pointer_y,
												  navigating);
	pointer_owns_focus_now = pointer_owner;
	int hovered = title_pointer_button();
	bool hovered_enabled = hovered != GUTIL_MENU_TARGET_NONE
		&& title_button_enabled(hovered);
	/* The highlight FOLLOWS the sprite while the pointer owns focus -- the
	 * unchanged Options arbitration. A disabled button is not followed onto;
	 * it is simply never a selection. */
	if(pointer_owner && hovered_enabled) {
		selected = hovered;
		title_origin = GUTIL_MENU_ORIGIN_POINTER;
	}
	/* D-pad steps through the ENABLED rows in visual order (a carve-out
	 * addition -- Beta's menu is mouse-only; keyTyped is an empty override).
	 * Clamped at both ends: DOWN past Quit game moves nothing. */
	if(navigating) {
		int dir = input_pressed(IB_GUI_DOWN) ? 1 : -1;
		int k = selected;
		do
			k += dir;
		while(k >= 0 && k < TB_COUNT && !title_button_enabled(k));
		if(k >= 0 && k < TB_COUNT)
			selected = k;
		title_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	}

	/* The ONE activation path on this screen (ruling 2 item 1: a caller must
	 * not fall through to another one). `pressed` is input_pressed, which is
	 * the RISING EDGE of the merged action mask, latched once per poll
	 * (source/platform/input.c input_pressed) -- so one hold can produce at
	 * most one press, and the fresh-press requirement after a FOCUS_ONLY (or
	 * on modal entry, where the opener's button is still down) needs no extra
	 * latch: the button has to be released before another edge exists. */
	bool activate_pressed = input_pressed(IB_GUI_CLICK);
	struct gutil_menu_decision decision
		= gutil_menu_activate_decide((struct gutil_menu_activate_state) {
			.pressed = activate_pressed,
			.navigated = navigating,
			.pointer_valid = pointer_available,
			.selection_origin = title_origin,
			.hovered = hovered,
			.hovered_enabled = hovered_enabled,
			.selected = selected,
			.selected_enabled = title_button_enabled(selected),
			/* R-D: this screen's targets open screens, they do not destroy
			 * anything by themselves. Select World's Delete and Multiplayer's
			 * Delete open a CONFIRMATION -- the destructive answer lives
			 * there, and that dialog declares it. */
			.selected_destructive = false,
		});
	if(decision.action == GUTIL_MENU_FOCUS_ONLY)
		selected = decision.selected;
	title_origin = decision.selection_origin;
	title_predict = (struct gutil_menu_activate_state) {
		.pressed = true, /* the observer asks "IF a press arrived" */
		.navigated = false,
		.pointer_valid = pointer_available,
		.selection_origin = title_origin,
		.hovered = hovered,
		.hovered_enabled = hovered_enabled,
		.selected = selected,
		.selected_enabled = title_button_enabled(selected),
	};
	if(decision.action == GUTIL_MENU_ACTIVATE) {
		title_activate(decision.target);
		return;
	}
	/* R-C: a press the rule CONSUMED reaches nothing else this frame. Under a
	 * remap that puts activate and Close on one physical button, the press
	 * below would otherwise be both a focus move and a quit (review A6-4). */
	if(activate_pressed)
		return;

	if(input_pressed(IB_HOME))
		gstate.quit = true; /* Wii-natural mirror of the Quit game row */
}

/* CROSS-LANE OBSERVER (push-0921 A5, Astra ruling 2 item 7; consumer: lane
 * B's mp_play_10m walker through main.c's mpp_hook_menu_focus). The contract
 * lives on the prototype in screen.h. READ-ONLY: it presses nothing, consumes
 * nothing and advances no state -- it asks the SAME gutil_menu_activate_decide
 * the press asks, on the inputs the last update left behind, with `pressed`
 * set because the question IS "if a press arrived". It is TRUE when the
 * pointer owns focus and rests on Play game (the press really opens it) and
 * FALSE when the sprite rests elsewhere (the mismatch rule consumes that
 * press), when the activate button is still held, or when another screen is
 * live.
 *
 * ROW ORDER + REACHABILITY (VISUAL-COORDINATION.md section 6 item 1): the
 * rows are Play game / Tutorial world / Texture packs / Options / Quit game,
 * top to bottom, and
 * the title OPENS with the highlight on Play game -- so from the opening focus
 * the Play row is reached by ZERO GUI_DOWN steps; a core that navigates by
 * GUI_DOWN + re-ask finds this true before its first step, and UP from any
 * lower row walks back onto it. */
int screen_title_focused_row(void) {
	if(gstate.players[0].current_screen != &screen_title)
		return SCREEN_TITLE_FOCUS_NONE;
	switch(selected) {
		case TB_PLAY: return SCREEN_TITLE_FOCUS_PLAY;
		case TB_TUTORIAL: return SCREEN_TITLE_FOCUS_TUTORIAL;
		case TB_PACKS: return SCREEN_TITLE_FOCUS_PACKS;
		case TB_OPTIONS: return SCREEN_TITLE_FOCUS_OPTIONS;
		case TB_QUIT: return SCREEN_TITLE_FOCUS_QUIT;
		default: return SCREEN_TITLE_FOCUS_NONE;
	}
}

bool screen_title_focus_is_play(void) {
	/* Another screen is live: its own update, not this one, owns the press. */
	if(gstate.players[0].current_screen != &screen_title)
		return false;
	/* Not yet re-armed: the activate button is still DOWN from a press whose
	 * edge has already been consumed, so no fresh edge can arrive. */
	if(input_held(IB_GUI_CLICK) && !input_pressed(IB_GUI_CLICK))
		return false;
	struct gutil_menu_decision decision
		= gutil_menu_activate_decide(title_predict);
	return decision.action == GUTIL_MENU_ACTIVATE
		&& decision.target == TB_PLAY && title_button_enabled(TB_PLAY);
}

/* TRANSITION ALIAS: the legacy Multiplayer query, kept DEFINED so main.c's
 * mpp_hook_menu_focus links unchanged until Fable's joint seam commit (section
 * 6 item 4: Load-tab phase + retire the alias) lands together with this title.
 * The standalone Multiplayer row no longer exists; the multiplayer path is now
 * Play game -> Join tab, so "focus rests on the row whose press opens the
 * multiplayer path" IS the Play row. One line, no second implementation. */
bool screen_title_focus_is_multiplayer(void) { return screen_title_focus_is_play(); }

/* ---- the approved presentation ------------------------------------------ */

/* The approved footer: Move / Select / Quit from the ORIGINAL gutil prompt
 * helper and the DETECTED bindings, measured then centred on the canvas (the
 * pack screen's accepted footer drawing, kept local). No glyph art is drawn
 * here; the helper draws the real prompt glyphs. */
#define TT_FOOTER_HINTS 3
static const enum input_button title_footer_button[TT_FOOTER_HINTS] = {
	IB_GUI_UP, IB_GUI_CLICK, IB_HOME,
};
static const char* const title_footer_label[TT_FOOTER_HINTS] = {
	"Move", "Select", "Quit",
};

#ifdef PLATFORM_PC
/* Capture-only (PC, "debug.join_proof"; absent = off). The Wii-only join
 * prompt cannot occur on a PC, which always owns a keyboard, so a capture
 * presents it through a presentation inventory read by the same gui_util draw
 * path as the console. Input, ownership and the real inventory are untouched.
 *   "wiimote" | "classic" | "gamecube": FIRST-PLAYER state -- nobody holds a
 *     controller; ONE detected, unassigned device of that family with its live
 *     "New controller" notice; player 1's footer is the join prompt.
 *   "p2-wiimote" | "p2-classic" | "p2-gamecube": P2-OFFER state -- player 1
 *     holds a Wii Remote + Nunchuk (Remote 1) and its footer shows that
 *     controller's prompts; a SECOND device of the named family (Remote 2, or
 *     GameCube port 1) is detected and offered "Join P2".
 *   "none": no device at all. */
static struct input_inventory title_join_proof;

static void title_join_proof_device(struct input_device device,
									enum input_category family, bool p1) {
	struct input_device_status* status = (struct input_device_status*)
		input_inventory_device(&title_join_proof, device);
	if(!status)
		return;
	status->initialized = true;
	status->published = status->observed
		= (struct input_identity) {true, family};
	status->published_usable = status->ever_usable = true;
	status->published_assigned = p1;
	status->published_player = 0;
	status->last_event = p1 ? CONTROLLER_EVENT_CONNECTED :
		CONTROLLER_EVENT_DETECTED;
}

static bool title_join_proof_apply(struct input_player_frame* prompts) {
	const char* proof = config_read_string(&gstate.config_user,
										   "debug.join_proof", "off");
	bool p2 = !strncmp(proof, "p2-", 3);
	const char* fam = p2 ? proof + 3 : proof;
	bool wiimote = !strcmp(fam, "wiimote"), classic = !strcmp(fam, "classic");
	bool gamecube = !strcmp(fam, "gamecube");
	bool none = !p2 && !strcmp(fam, "none");
	if(!wiimote && !classic && !gamecube && !none)
		return false;
	memset(&title_join_proof, 0, sizeof(title_join_proof));
	if(p2)
		title_join_proof_device((struct input_device) {INPUT_DEVICE_WPAD, 0},
								INPUT_CAT_NUNCHUK, true);
	if(!none) {
		struct input_device device = {
			gamecube ? INPUT_DEVICE_GAMECUBE : INPUT_DEVICE_WPAD,
			p2 && !gamecube ? 1 : 0};
		enum input_category family = gamecube ? INPUT_CAT_GAMECUBE :
			classic ? INPUT_CAT_CLASSIC_CONTROLLER : INPUT_CAT_NUNCHUK;
		title_join_proof_device(device, family, false);
		title_join_proof.join.state = INPUT_JOIN_OFFERED;
		title_join_proof.join.device = device;
		title_join_proof.notices[0] = (struct input_connection_notice) {
			.expires = time_add_ms(time_get(), INPUT_NOTICE_MS),
			.kind = (uint8_t)device.kind, .channel = (uint8_t)device.channel,
			.player = 0, .event = CONTROLLER_EVENT_DETECTED,
			.category = (uint8_t)family, .previous_category = INPUT_CAT_NONE,
			.active = 1, .connected = 1};
	}
	gutil_join_inventory_override(&title_join_proof);
	*prompts = (struct input_player_frame) {.category = INPUT_CAT_NONE,
											.join_prompt = !p2};
	for(int b = 0; b < IB_COUNT; b++)
		prompts->glyphs[b] = -1;
	if(p2) {
		/* Player 1's Remote + Nunchuk footer, from the shipped Wii bindings
		 * (config_wii.json: gui_up 0, gui_click 4, open_menu 10). */
		prompts->connected = true;
		prompts->category = INPUT_CAT_NUNCHUK;
		prompts->glyphs[IB_GUI_UP] = input_glyph_key(INPUT_CAT_NUNCHUK, 0);
		prompts->glyphs[IB_GUI_CLICK] = input_glyph_key(INPUT_CAT_NUNCHUK, 4);
		prompts->glyphs[IB_HOME] = input_glyph_key(INPUT_CAT_NUNCHUK, 10);
	}
	return true;
}
#endif

static void title_draw_footer(const struct title_canvas* c) {
	struct input_player_frame prompts;
	input_prompt_frame(0, &prompts);
#ifdef PLATFORM_PC
	(void)title_join_proof_apply(&prompts);
#endif
	/* The centred hint row (controller UI redesign 10-31, A1 helper), in the
	 * title canvas's frame. */
	struct gutil_hint_item items[TT_FOOTER_HINTS];
	for(int k = 0; k < TT_FOOTER_HINTS; k++)
		items[k] = (struct gutil_hint_item) {
			{title_footer_button[k]}, 1, title_footer_label[k],
			TT_FOOTER_HINTS - k, false};
	struct gutil_hint_row row;
	if(gutil_hint_row_layout(&prompts, tt_x(c, 0), 0, tt_d(c, 640),
							 tt_d(c, 480), false, GUTIL_HINT_ALIGN_CENTRE,
							 items, TT_FOOTER_HINTS, &row))
		gutil_hint_row_draw(&row);
}

static void title_draw_wordmark(const struct title_canvas* c, int width) {
	if(texture_title_logo.data) {
		/* gutil_texquad uses fixed 1/256 UV units. This 320x148 image needs
		 * normalized UVs, as the existing backdrop does. No rescaling. */
		/* Canvas units like every other title element: 1:1 at 480 lines
		 * (the Wii), integer-scaled with the canvas on a taller PC window. */
		(void)width;
		int x = tt_x(c, 320 - TT_LOGO_FACE_CENTRE_X);
		int y = tt_d(c, TT_LOGO_Y);
		int w = tt_d(c, (int)texture_title_logo.width);
		int h = tt_d(c, (int)texture_title_logo.height);
		gutil_keepout(x, y, w, h); /* toast-1030: the art heading */
		gfx_bind_texture(&texture_title_logo);
		gfx_draw_quads_flt(
			4,
			(float[]) {x, y, -2, x + w, y, -2, x + w, y + h, -2,
					   x, y + h, -2},
			(uint8_t[]) {255, 255, 255, 255, 255, 255, 255, 255,
						 255, 255, 255, 255, 255, 255, 255, 255},
			(float[]) {0, 0, 1, 0, 1, 1, 0, 1});
		return;
	}

	/* The wordmark: the pack's own font at 76 px, centred, top 37, with the
	 * approved EXPLICIT (3,4) shadow. gutil_text's built-in shadow scales with
	 * the glyph (scale/8 -> 9 px at 76), which is not the approved offset, so
	 * the dark pass is drawn by hand first and the white face over it, both
	 * with shadow OFF. */
	{
		int font = tt_d(c, TT_WORDMARK_FONT);
		const char* mark = "CavEX";
		int x = tt_x(c, 0) + (tt_d(c, 640) - gutil_font_width(mark, font)) / 2;
		int y = tt_d(c, TT_WORDMARK_Y);
		gutil_text_rgb(x + tt_d(c, TT_WORDMARK_SHADOW_X),
					   y + tt_d(c, TT_WORDMARK_SHADOW_Y), mark, font, false,
					   0x3F, 0x3F, 0x3F);
		gutil_text_rgb(x, y, mark, font, false, 0xFF, 0xFF, 0xFF);
	}
}

static void title_draw_splash(const struct title_canvas* c) {
	/* The splash: the loaded / date-overridden text (never a fixture), fitted
	 * beside the logo, or at the previous position when the art is absent,
	 * rotated -9 degrees. gutil_text_rot centres the string on its anchor with
	 * its top one glyph above it, so the anchor is (right - w/2, top + font).
	 * A bounded one-second pulse preserves the fitted maximum size. */
	{
		bool logo = texture_title_logo.data != NULL;
		int font = tt_d(c, TT_SPLASH_FONT);
		/* Like Beta GuiMainMenu, a long splash is scaled down to fit, never
		 * cut with an ellipsis. */
		if(splash_text[0]) {
			int max_w = tt_d(c, logo ? TT_LOGO_SPLASH_MAX_W : TT_SPLASH_MAX_W);
			int w = gutil_font_width(splash_text, font);
			float fit = w > max_w ? (float)max_w / (float)w : 1.0F;
			int right = tt_x(c, 640 - TT_SPLASH_RIGHT);
			int x = logo ? tt_x(c, TT_LOGO_SPLASH_X)
						 : right - (int)(w * fit) / 2;
			int y = tt_d(c, logo ? TT_LOGO_SPLASH_Y : TT_SPLASH_Y) + font;
			gutil_text_rot_zoom(x, y, splash_text, font, TT_SPLASH_ANGLE,
				fit * (1.0F - 0.05F
					   * fabsf(sinf(menu_motion_time * 3.141592654F))),
				true, 0xFF, 0xFF, 0xA0);
		}
	}
}

static void screen_title_render2D(struct screen* s, int width, int height) {
	(void)s;
	struct title_canvas c = title_canvas_of(width, height);

	/* The scenic backdrop (the pack's optional gui/menu-background.png under
	 * the composition's wash); without it, the ordinary tiled-dirt menu
	 * background -- disclosed, not faked. */
	if(!fep_backdrop_at(width, height, 90, menu_motion_time, true))
		gutil_bg();

	title_draw_wordmark(&c, width);
	title_draw_splash(&c);

	/* The five rows: the pack's own widget material (idle / focus rows) and the
	 * bright focus ring on the highlighted one. `selected` is literally what
	 * the activation decision reads (A3 NEW-7).
	 *
	 * NO fep_bevel. The widget atlas already draws this button's edge, so the
	 * extra highlight/shadow pair was a second edge on top of the art -- the
	 * native/Beta+Earth comparison caught it. What remains is the approved
	 * pack-style button border: ONE thin dark line, 1 canvas px scaled through
	 * the title canvas, inside the rect. Geometry is untouched, so the drawn
	 * extent still equals the hit rect (title_row_rect is the single source).
	 *
	 * INK is the shared high-contrast pair: idle #f4f4ed, focused #ffffa0 (the
	 * pack-style.css default), disabled the muted grey. This is a deliberate
	 * NATIVE ADAPTATION of the prototype: the HTML studio let each pack nudge
	 * its label ink a shade, which on a TV at Wii distance reads as noise and
	 * would need a per-pack theme table this screen has no business inventing.
	 * The pack still supplies the MATERIAL -- widget atlas and font -- and the
	 * one ink pair rides on top of all of them. No pack-name detection. */
	for(int k = 0; k < TB_COUNT; k++) {
		struct fep_rect r = title_row_rect(width, height, k);
		bool enabled = title_button_enabled(k);
		bool focused = enabled && selected == k;
		fep_widget(r, focused);
		int border = tt_d(&c, 1);
		fep_outline(r, border > 0 ? border : 1, 0x00, 0x00, 0x00, 0x60);
		if(focused)
			fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
		if(!enabled)
			fep_fill(r, 0, 0, 0, 0x70);
		int font = tt_d(&c, TT_ROW_FONT);
		char text[32];
		fep_text_fit(text, sizeof(text), title_labels[k], font,
					 r.w - 2 * GFX_GUI_SCALE);
		int tw = gutil_font_width(text, font);
		uint8_t ir = enabled ? (focused ? 0xFF : 0xF4) : 0xA0;
		uint8_t ig = enabled ? (focused ? 0xFF : 0xF4) : 0xA0;
		uint8_t ib = enabled ? (focused ? 0xA0 : 0xED) : 0xA0;
		gutil_text_rgb(r.x + (r.w - tw) / 2, r.y + (r.h - font) / 2, text, font,
					   true, ir, ig, ib);
	}

	title_draw_footer(&c);

	/* DEBUG-ONLY build stamp (the native oracle's self-identification): only
	 * with the F3-style overlay on, as an 8 px line at canvas y 470. The
	 * approved UI has no copyright line. */
	if(gstate.debug_overlay) {
		char ver[96];
		snprintf(ver, sizeof(ver), "CavEX Beta 1.7.3 %s", CW_BUILD_ID);
		int font = tt_d(&c, TT_STAMP_FONT);
		gutil_text_rgb(tt_x(&c, 2), tt_d(&c, TT_STAMP_Y), ver, font, true, 0xA0,
					   0xA0, 0xA0);
	}

	if(pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(pointer_x, pointer_y, glm_rad(pointer_angle), 0,
							 0, 256, 256, 48 * GFX_GUI_SCALE,
							 48 * GFX_GUI_SCALE);
	}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* The row screen_title_render2D draws HOVERED. It is literally `selected`,
 * the same variable the activation decision reads, so "what is highlighted is
 * what fires" is structural here rather than two parallel expressions that
 * could drift (A3 NEW-7). Exposed so a headless case can assert the highlight
 * without calling render2D. */
int screen_title_test_highlight(void) {
	return selected;
}
/* The splash text the last reset rolled ("" = none drawn). */
const char* screen_title_test_splash(void) {
	return splash_text;
}
#endif

struct screen screen_title = {
	.reset = screen_title_reset,
	.update = screen_title_update,
	.render2D = screen_title_render2D,
	.render3D = NULL,
	.render_world = false,
};
