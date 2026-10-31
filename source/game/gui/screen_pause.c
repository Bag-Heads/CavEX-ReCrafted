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

#include "../../platform/controller_events.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../network/client_interface.h" // clin_update_drain / clin_force_world_reset / clin_send_unload_world (remote Leave)
#include "../../network/server_interface.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/input_inventory.h"
#include "../../platform/input_owner.h"
#include "../../platform/menu_sound.h"
#include "../../platform/time.h"
#include "../game_state.h"
#include "menu_header.h"
#include "screen.h"
#include "screen_achievements.h"
#include "screen_stats_save.h"
#include "../achievements/achievements.h"

#include <stdio.h>
#include <string.h>

/* APPROVED PAUSE COMPOSITION -- docs/ui_design/approved/menus.html
 * (menu selector -> Pause) and specs/menus.md:
 *
 *   "Pause uses the approved small set of meaningful actions. Show the
 *    assigned controller's detected name; no My controller chooser. In
 *    split-screen, local pause does not pretend the world is stopped. Host
 *    save/quit and guest leave preserve ownership."
 *
 * The prototype's own body, for the record:
 *   button('close','Back to game')
 *   + (layout==='single' ? button('settings','Options...') : '')
 *   + button(p===1?'quit':'leave', p===1?'Save and quit':'Leave game')
 *   + '<div class="device-status">' + devices[d].name + '</div>'
 *   + (layout==='split' ? '<div class="pause-status">World continues</div>' : '')
 *
 * DELIBERATE DEVIATION FROM BETA 1.7.3, carve-out #2 (Wii-controller
 * playability), maintainer-approved 2026-09-07 with the rest of this package.
 * Beta's GuiIngameMenu is five buttons -- Save and quit to title / Back to
 * game / Options... / Achievements / Statistics [740c583
 * GuiIngameMenu.initGui]. The 2026-09-07 design dropped the two then-dead
 * achievement/stat rows and added the detected controller name. The approved
 * 2026-10-05 design restores Achievements as the second row now that it has
 * an owner-query-backed screen. Statistics remains absent. SIMULATION IS
 * UNTOUCHED: single-player actions still send the Beta RPC pair
 * (SRPC_TOGGLE_PAUSE, and SRPC_TOGGLE_PAUSE + SRPC_UNLOAD_WORLD for
 * id 1 -> changeWorld1(null)), in the same order, through
 * screen_pause_process_edges below -- which is also the lane-3 recovery
 * gate's quit path and is NOT touched by this layout work.
 *
 * SOUND -- docs/ui_design/approved/specs/sound-events.md, "Pause / ordinary
 * submenu": "Existing category cue when a menu actually opens or changes;
 * existing scroll on changed user selection; Back on successful
 * return/resume." Beta's quarter-volume random.click on every button press
 * [740c583 GuiScreen.mouseClicked] is REPLACED by that map: it is a menu
 * transition cue, not a world sound, so it belongs to the approved menu
 * dispatcher (audio_menu_event) and its shared 60/90 ms guards. */

enum pause_action {
	PA_BACK,	/* "Back to game"                 -- Beta id 4 */
	PA_ACHIEVEMENTS, /* approved 2026-10-05, Beta id 5 */
	PA_OPTIONS, /* "Options..."   full layout only -- Beta id 0 */
	PA_TEXPACKS, /* "Texture packs..." full layout only -- project extension (Astra ui9, 2026-09-11) */
	PA_EXIT,	/* host "Save and quit" / guest "Leave game" -- Beta id 1 */
	PA_COUNT
};

/* Beta ids retained so the RPC/behaviour mapping stays greppable against the
 * decompile even though the row order is the approved one:
 * back(4) / achievements(5) / options(0) / quit(1)
 * [740c583 GuiIngameMenu.actionPerformed]. */
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static const int pause_ids[PA_COUNT] = {4, 5, 0, -1, 1};
#endif

static int selected;
static int pointer_target = -1;
static bool pointer_available;
static float pointer_x, pointer_y, pointer_angle;
static struct gutil_menu_focus pointer_focus;
static enum gutil_menu_origin selection_origin;
static bool suppress_open_cue;
static bool restore_achievements;
/* Save and quit is a request, not a transition (2026-09-11 console finding:
 * the unload's inventory close or world checkpoint can fail, and the server
 * then answers RETURN_INGAME; jumping to the title on enqueue flashed the
 * title for a second and dropped the player back in-game with no message).
 * exit_pending holds this screen with its buttons frozen until
 * client_interface settles it: WORLD_RESET(local_entity=0) -> title,
 * RETURN_INGAME -> exit_failed with the server's "Save failed: ..." toast as
 * the status line and the world paused again. */
static bool exit_pending;
static bool exit_failed;
/* IMP-QUIT-UNSAVABLE: the chunks the server could not save (from the refusing
 * RETURN_INGAME), and whether the NEXT unload carries the player's
 * "Quit anyway" for exactly those chunks. Consumed by pause_quit_saved. */
static struct {
	bool quit_anyway;
	int32_t x, z;
	uint32_t count;
} unsaved_answer;

/* menus.md: "Host save/quit and guest leave preserve ownership." The only
 * ownership distinction this client can make is who owns the world: a local
 * world is ours to save and quit; a remote session is someone else's and we
 * can only leave it. */
static bool pause_is_host(void) {
	return !gstate.remote_session;
}

static const char* pause_exit_label(bool host) {
	return host ? "Save and quit" : "Leave game";
}

static const char* pause_label(enum pause_action action, bool host) {
	switch(action) {
		case PA_BACK: return "Back to game";
		case PA_ACHIEVEMENTS: return "Achievements";
		case PA_OPTIONS: return "Options...";
		case PA_TEXPACKS: return "Texture packs...";
		default: return pause_exit_label(host);
	}
}

/* The approved 2026-10-05 compact composition is a two-by-two grid. The
 * separate owner-local split pause remains in local_pause.c. */
static bool pause_action_visible(enum pause_action action, bool full_layout) {
	return action != PA_TEXPACKS || full_layout;
}

static size_t pause_visible_count(bool full_layout) {
	size_t n = 0;
	for(int k = 0; k < PA_COUNT; k++)
		if(pause_action_visible((enum pause_action)k, full_layout))
			n++;
	return n;
}

/* Visible row index -> action. */
static enum pause_action pause_action_at(size_t row, bool full_layout) {
	size_t n = 0;
	for(int k = 0; k < PA_COUNT; k++) {
		if(!pause_action_visible((enum pause_action)k, full_layout))
			continue;
		if(n == row)
			return (enum pause_action)k;
		n++;
	}
	return PA_BACK;
}

static enum input_category pause_owner_category(void) {
	struct input_player_frame owner;
	input_prompt_frame(0, &owner);
	return owner.category;
}

/* The whole composition, rebuilt from the live viewport every frame. Nothing
 * here allocates: the struct is the caller's frame (Wii rule). */
static void pause_compose(struct menu_composition* c, int width, int height) {
	menu_composition_begin(c, MENU_SCREEN_PAUSE, 0, 0, width, height);
	bool full = c->layout == MENU_SCREEN_FULL;
 if(full) {
  menu_composition_window(c,(width-360)/2,44,360,364);
  menu_composition_header(c,(width-360)/2+12,56,336,40);
 } else {
  int pw=width>=854?720:576;
  menu_composition_window(c,(width-pw)/2,12,pw,216);
  menu_composition_header(c,(width-pw)/2+16,12,pw-32,24);
 }
	size_t rows = pause_visible_count(full);
	for(size_t k = 0; k < rows; k++) {
		struct menu_rect r;
		if(full) r=menu_pause_row(c,(int)k);
		else {
			int pw=width>=854?720:576, bw=(pw-44)/2;
			r=(struct menu_rect){(width-pw)/2+16+(int)(k%2)*(bw+12),68+(int)(k/2)*52,bw,44};
		}
		menu_composition_action(c, r.x, r.y, r.w, r.h);
	}
	menu_composition_header_notice(c,gutil_connection_notice_width(0,gstate.players[1].local_player!=NULL,true),gutil_font_width("Game menu",16));
 menu_composition_seal(c);
}

static void screen_pause_reset(struct screen* s, int width, int height) {
	(void)width;
	(void)height;
	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;
	input_pointer_enable(true);
	selected = restore_achievements ? 1 : 0;
	restore_achievements = false;
	exit_pending = false;
	exit_failed = false;
	unsaved_answer.quit_anyway = false;
	pointer_target = -1;
	pointer_focus = (struct gutil_menu_focus){0};
	selection_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	/* "Existing category cue when a menu actually opens or changes." One cue,
	 * on the frame the pause panel actually appears -- not per frame, and not
	 * on the return from Options (which is its own Back). */
	if(!suppress_open_cue)
		audio_menu_event(MENU_SOUND_SECTION, time_get());
	suppress_open_cue = false;
	s->render3D = screen_ingame.render3D;
}

/* Beta's id-4 "Back to game": local resume toggles; remote resume leaves the
 * world running. HOME/Start, the Close role and the "Back to game" row share
 * this path, so the three cannot drift apart. */
static bool pause_resume(void) {
	if(gstate.remote_session) {
		audio_menu_event(MENU_SOUND_BACK, time_get());
		screen_set(&screen_ingame);
		return true;
	}
	struct server_rpc resume = {
		.type = SRPC_TOGGLE_PAUSE,
	};
	if(!svin_rpc_send(&resume))
		return false; // remain consistently paused on both sides
	gstate.paused = false;
	/* "Back on successful return/resume" -- after the toggle is accepted,
	 * never on the press. */
	audio_menu_event(MENU_SOUND_BACK, time_get());
	screen_set(&screen_ingame);
	return true;
}

bool screen_pause_resume_game(void) { return pause_resume(); }
void screen_pause_restore_achievements(void) {
	restore_achievements=true;
	suppress_open_cue=true;
	screen_set(&screen_pause);
}
bool screen_pause_achievements_focused(void) { return selected==1; }

/* Beta's id-1 "Save and quit" (host) / the project's "Leave game" (remote):
 * the local resume toggle precedes unload; remote Leave tears down its client
 * without a pause toggle. In PRODUCTION it is reached from exactly one place
 * -- the named row (pause_activate, PA_EXIT) --
 * never from a button edge, since the maintainer's 2026-09-19 decision below.
 * screen_pause_recovery_quit() below is a second door onto it that nothing
 * production calls today; see its own comment. */
static void pause_quit_saved(void) {
	if(!gstate.remote_session) {
		struct server_rpc resume = {
			.type = SRPC_TOGGLE_PAUSE,
		};
		if(!svin_rpc_send(&resume))
			return; // remain consistently paused on both sides
		gstate.paused = false;
	}

	/* REMOTE "Leave game" runs the client's OWN teardown; the server's
	 * never comes back (2026-09-12 D2 diagnosis).
	 *
	 * SRPC_UNLOAD_WORLD on a remote session lands in
	 * server_remote_protocol.c:223-225, which answers it by calling
	 * server_remote_disconnect(remote, "Quitting") -> graceful_stop
	 * (server_remote_transport.c:1601) -> mp_status.graceful
	 * (server_remote.c:455). main.c:2757-2760's own
	 * clin_force_world_reset() is gated on `!mp_status.graceful &&
	 * mp_status.logged_in`, i.e. exactly the UNgraceful (kick / link
	 * loss) case -- so a deliberate Leave produces NO CRPC_WORLD_RESET,
	 * nothing ever calls screen_pause_exit_settled(), and exit_pending
	 * below would hold this screen frozen on "Saving world..." forever.
	 * For the same reason main.c cannot double-fire against the reset we
	 * do here: by the time it evaluates that branch, graceful_stop is
	 * already latched.
	 *
	 * So take the proven remote-escape sequence verbatim --
	 * screen_load_world.c:397-405 (screen_lworld_remote_escape; static,
	 * hence replicated rather than called): drain the inbox, force the
	 * client-side WORLD_RESET, then send the unload through the chip-B
	 * reliable sender (a saturated svin_rpc_send latches a per-frame
	 * retry instead of being dropped, leaving server_remote stuck in
	 * REMOTE_PLAY). That one send REPLACES the bare svin_rpc_send below,
	 * so the server still sees exactly one SRPC_UNLOAD_WORLD.
	 *
	 * ORDERING: exit_pending is armed BEFORE clin_force_world_reset()
	 * because that call is synchronous -- clin_force_world_reset()
	 * dispatches CRPC_WORLD_RESET straight into clin_process() on this
	 * thread (client_interface.c:1382-1388), whose local_entity==0 arm
	 * (client_interface.c:2085-2091) is what calls
	 * screen_pause_exit_settled(true), and that function early-returns
	 * unless exit_pending is already set. Arming first keeps the title
	 * transition and its Back cue on the ONE settle path both the
	 * single-player and the remote case share; arming afterwards would
	 * re-create the very freeze this fixes. No screen_ingame fallback on
	 * a refused send either: the world is already torn down, and the
	 * latched retry is what finishes the handshake. */
	if(gstate.remote_session) {
		exit_pending = true;
		exit_failed = false;
		clin_update_drain();
		clin_force_world_reset();
		clin_send_unload_world();
		return;
	}

	struct server_rpc unload = {
		.type = SRPC_UNLOAD_WORLD,
		.payload.unload_world = {
			.accept_unsaved = unsaved_answer.quit_anyway,
			.chunk_x = unsaved_answer.x,
			.chunk_z = unsaved_answer.z,
			.count = unsaved_answer.count,
		},
	};
	unsaved_answer.quit_anyway = false; /* one request only */
	if(svin_rpc_send(&unload)) {
		/* id 1 -> changeWorld1(null), then GuiMainMenu
		 * [740c583 GuiIngameMenu.actionPerformed] -- but only once the
		 * server has actually unloaded: screen_pause_exit_settled. */
		exit_pending = true;
		exit_failed = false;
	} else {
		// Resume rather than hiding a still-live world behind world-select.
		screen_set(&screen_ingame);
	}
}

static void pause_stats_saved(void* user, enum stats_exit_result result) {
 (void)user;
 if(result == STATS_EXIT_BACK) return;
 if(result == STATS_EXIT_WITHOUT_SAVE)
  for(unsigned i = 0; i < 2; i++) achievements_request_rescue(i);
 pause_quit_saved();
}
static void pause_quit(void) {
 screen_stats_save_open(0, gstate.remote_session ? 1u : 3u, pause_stats_saved, NULL);
}

/* `close` is the CLOSE role (IB_INVENTORY -- Remote 1 / Classic X / GameCube
 * Y). Since the maintainer's decision of 2026-09-19 on Astra menu review MUST
 * item 8 it means exactly what HOME/Start means here: BACK / RESUME.
 *
 * Until then this screen was the ONE place in the whole UI where Close did not
 * mean Back -- it ran Beta's id-1 pair (SRPC_TOGGLE_PAUSE + SRPC_UNLOAD_WORLD)
 * and dropped the player at the title with no confirmation, an exception the
 * older contract preserved on purpose (M/docs/ui_design/control-bindings.md:35)
 * while revision 1 of the controls sheet advertised Close as plain Back. The
 * maintainer resolved it toward consistency: "Close = Back / Resume,
 * consistently; quitting only through the named menu row." Save and quit keeps
 * its row (pause_activate) and does not go through a button edge any more.
 *
 * The recovery gate is unchanged in intent and now covers BOTH edges: "Resume
 * belongs exclusively to the modal while recovery is active." Close is a
 * resume, so it is withheld WITH home rather than beside it.
 *
 * WHAT THE OTHER HALF OF THAT RULING IS NOT (hostile review 17, L-1). The rule
 * also says save/quit stays available during recovery, and this file does not
 * implement that -- it only declines to break it. screen_pause_recovery_quit()
 * below is the entry point a recovery modal WOULD call, and nothing production
 * calls it: `git grep screen_pause_recovery_quit -- source` finds the
 * declaration (screen.h), the definition and these comments, and the only
 * callers are tests/test_pause_approved_actions.c. Nor could the OLD
 * Close-quits-during-recovery path have served it: main.c:4178 skips
 * screen_pause_update() entirely while recovery_modal is up, so that edge was
 * equally unreachable. Net behaviour is therefore unchanged by the 09-19 edit,
 * but "quit is still available during recovery" is an availability this code
 * does not provide, and this comment does not claim it does. Wiring the modal
 * to this function (or deleting it) is a separate decision. */
static void screen_pause_process_edges(bool home, bool close) {
	if(screen_controller_recovery_state()->phase != CONTROLLER_RECOVERY_NONE)
		return;
	/* A controller report may carry HOME and Close together. Both now mean the
	 * same thing, so the frame produces at most ONE toggle -- the guarantee the
	 * old HOME-owns-the-frame early return existed to make (two toggles plus an
	 * unload left the server paused while the next world waited forever at its
	 * loading gate). */
	if(home || close)
		pause_resume();
}

void screen_pause_recovery_quit(void) {
	pause_quit();
}

static void pause_activate(enum pause_action action, bool host) {
	(void)host;
	if(action == PA_BACK) {
		screen_pause_process_edges(true, false);
		return;
	}
	if(action == PA_EXIT) {
		pause_quit();
		return;
	}
	if(action == PA_ACHIEVEMENTS) {
		suppress_open_cue=true;
		screen_achievements.render3D=screen_ingame.render3D;
		screen_set(&screen_achievements);
		return;
	}
	if(action == PA_TEXPACKS) {
		/* Astra ui9: the approved pack list over the paused world; in-game
		 * Apply is textures-only (sound and music registries stay). Same
		 * category cue and return contract as Options below. */
		audio_menu_event(MENU_SOUND_SECTION, time_get());
		suppress_open_cue = true;
		screen_texturepacks_set_return(&screen_pause);
		screen_texturepacks_set_ingame(true);
		screen_set(&screen_texturepacks);
		return;
	}
	/* id 0 -> new GuiOptions(this, gameSettings).
	 * [740c583 GuiIngameMenu.actionPerformed]. A submenu that actually opens
	 * is the category cue; coming back here must not re-fire it. */
	audio_menu_event(MENU_SOUND_SECTION, time_get());
	suppress_open_cue = true;
	screen_options_set_return(&screen_pause);
	screen_set(&screen_options);
}

static void pause_step_selection(int dir, size_t rows) {
	int next = selected + dir;
	if(next < 0 || next >= (int)rows)
		return;
	selected = next;
	/* "existing scroll on changed user selection" -- only when it changed. */
	audio_menu_event(MENU_SOUND_FOCUS, time_get());
}

bool screen_pause_exit_pending(void) {
	return exit_pending;
}

void screen_pause_exit_settled(bool unloaded) {
	if(!exit_pending)
		return;
	exit_pending = false;
	if(unloaded) {
		audio_menu_event(MENU_SOUND_BACK, time_get());
		screen_set(&screen_title);
		return;
	}
	exit_failed = true;
	if(gstate.remote_session)
		return;
	/* The resume toggle went out ahead of the unload, so the live world is
	 * running unpaused behind this screen. Pause it again so Back to game's
	 * own toggle keeps client/server pause ownership consistent; if even that
	 * enqueue is refused, show the live world rather than a stale pause. */
	struct server_rpc pause = {
		.type = SRPC_TOGGLE_PAUSE,
	};
	if(svin_rpc_send(&pause))
		gstate.paused = true;
	else
		screen_set(&screen_ingame);
}

/* IMP-QUIT-UNSAVABLE: the dialog's answer. Back on the pause screen (its
 * "Saving world..." status), re-run the same Save and quit; "Quit anyway"
 * tells the server it may leave out exactly the chunks it named. */
static void pause_unsavable_answer(bool quit_anyway) {
	suppress_open_cue = true;
	screen_set(&screen_pause);
	unsaved_answer.quit_anyway = quit_anyway;
	pause_quit_saved();
}

void screen_pause_exit_unsavable(int32_t chunk_x, int32_t chunk_z,
								 uint32_t count) {
	if(!exit_pending)
		return;
	screen_pause_exit_settled(false); /* re-pauses the live world */
	/* Only over a paused local world: if the re-pause was refused the settle
	 * above already showed the live game (and the toast). */
	if(gstate.remote_session || !gstate.paused)
		return;
	unsaved_answer.x = chunk_x;
	unsaved_answer.z = chunk_z;
	unsaved_answer.count = count;
	unsaved_answer.quit_anyway = false;
	char where[64];
	if(count > 1)
		snprintf(where, sizeof(where), "%u chunks, first %d, %d",
				 (unsigned)count, (int)chunk_x, (int)chunk_z);
	else
		snprintf(where, sizeof(where), "Chunk %d, %d", (int)chunk_x,
				 (int)chunk_z);
	/* The approved confirm dialog [740c583 GuiYesNo]: "Try again" is the
	 * safe answer (modal default and Back); "Quit anyway" is the one that
	 * gives up the named chunks' unsaved changes. */
	screen_confirm_setup("Couldn't save part of the world", where,
						 "Quit anyway", "Try again", SCREEN_CONFIRM_ANSWER_NO,
						 pause_unsavable_answer);
	screen_set(&screen_confirm);
}

/* READ-ONLY OBSERVATION (push-0921/B4b, Astra gate 1 Lane B item 3). Is the
 * DIRECTIONALLY focused row the one that leaves a remote session?
 *
 * That row is PA_EXIT, labelled "Leave game" while gstate.remote_session is
 * true (pause_exit_label), and activating it runs pause_quit()'s remote branch
 * -- clin_force_world_reset() + clin_send_unload_world(), after the Beta
 * resume toggle -- settling to the title through screen_pause_exit_settled().
 * In a LOCAL world the same row is "Save and quit", which leaves no remote
 * session, so this answers false there: the function's name stays true.
 *
 * The row index is derived exactly as the click path derives it -- `selected`
 * against the live composition -- so the two can never disagree.
 * pause_compose() only fills the caller's struct from the viewport, so calling
 * it here is a pure recomputation with no side effect.
 *
 * The shared menu activation rule below uses the same selected row and its
 * origin, so a stationary pointer over empty space cannot veto a directional
 * Leave selection. */
bool screen_pause_focus_is_leave(void) {
	struct menu_composition c;
	if(!gstate.remote_session)
		return false;
	pause_compose(&c, gfx_width(), gfx_height());
	if(selected < 0 || (size_t)selected >= c.action_count)
		return false;
	return pause_action_at((size_t)selected, c.layout == MENU_SCREEN_FULL)
		== PA_EXIT;
}

int screen_pause_focused_action(void) {
	if(gstate.players[0].current_screen != &screen_pause)
		return SCREEN_PAUSE_FOCUS_NONE;
	struct menu_composition c;
	pause_compose(&c, gfx_width(), gfx_height());
	if(selected < 0 || (size_t)selected >= c.action_count)
		return SCREEN_PAUSE_FOCUS_NONE;
	switch(pause_action_at((size_t)selected, c.layout == MENU_SCREEN_FULL)) {
		case PA_BACK: return SCREEN_PAUSE_FOCUS_BACK;
		case PA_ACHIEVEMENTS: return SCREEN_PAUSE_FOCUS_ACHIEVEMENTS;
		case PA_OPTIONS: return SCREEN_PAUSE_FOCUS_OPTIONS;
		case PA_TEXPACKS: return SCREEN_PAUSE_FOCUS_TEXPACKS;
		case PA_EXIT: return SCREEN_PAUSE_FOCUS_EXIT;
		default: return SCREEN_PAUSE_FOCUS_NONE;
	}
}

static void screen_pause_update(struct screen* s, float dt) {
	(void)s;
	(void)dt;
	if(exit_pending)
		return; /* frozen until the server settles the unload */
	int width = gfx_width(), height = gfx_height();
	struct menu_composition c;
	pause_compose(&c, width, height);
	size_t rows = c.action_count;
	if(rows && selected >= (int)rows)
		selected = (int)rows - 1;

	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);
	bool up = input_pressed(IB_GUI_UP);
	bool down = input_pressed(IB_GUI_DOWN);
	bool navigating = up || down;
	bool pointer_owner = gutil_menu_pointer_focus(&pointer_focus,
			pointer_available, pointer_x, pointer_y, navigating);
	pointer_target = -1;
	if(pointer_available) {
		for(size_t k = 0; k < rows; k++) {
			struct gutil_button_bounds bounds
				= {c.actions[k].x, c.actions[k].y, c.actions[k].w,
				   c.actions[k].h};
			if(gutil_button_hit(&bounds, pointer_x, pointer_y)) {
				pointer_target = (int)k;
				if(pointer_owner) {
					selection_origin = GUTIL_MENU_ORIGIN_POINTER;
					if(selected != (int)k) {
						selected = (int)k;
						audio_menu_event(MENU_SOUND_FOCUS, time_get());
					}
				}
				break;
			}
		}
	}

	if(up) {
		pause_step_selection(-1, rows);
	}
	if(down) {
		pause_step_selection(1, rows);
	}
	if(navigating)
		selection_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	bool click = input_pressed(IB_GUI_CLICK);
	struct gutil_menu_decision decision = gutil_menu_activate_decide(
		(struct gutil_menu_activate_state) {
			.pressed = click, .navigated = navigating,
			.pointer_valid = pointer_available,
			.selection_origin = selection_origin, .hovered = pointer_target,
			.hovered_enabled = pointer_target >= 0, .selected = selected,
			.selected_enabled = selected >= 0 && selected < (int)rows,
		});
	selected = decision.selected;
	selection_origin = decision.selection_origin;
	if(click && decision.action == GUTIL_MENU_ACTIVATE) {
		pause_activate(pause_action_at((size_t)decision.target,
									   c.layout == MENU_SCREEN_FULL),
						   pause_is_host());
		return;
	}

	bool home = input_pressed(IB_HOME) || input_pressed(IB_GUI_CLICK_ALT);
	bool close = input_pressed(IB_INVENTORY);
	screen_pause_process_edges(home, close);
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void screen_pause_test_composition(struct menu_composition* c, int width, int height) {
	pause_compose(c,width,height);
}
void screen_pause_test_activate_row(size_t row) {
	if(row < pause_visible_count(true))
		pause_activate(pause_action_at(row, true), pause_is_host());
}
/* The second parameter is the CLOSE role, which resumes since 2026-09-19. */
void screen_pause_test_process_edges(bool home, bool close) {
	screen_pause_process_edges(home, close);
}

size_t screen_pause_test_button_count(bool full_layout) {
	return pause_visible_count(full_layout);
}

bool screen_pause_test_button(size_t idx, bool full_layout, bool host, int* id,
							  const char** label) {
	if(idx >= pause_visible_count(full_layout))
		return false;
	enum pause_action action = pause_action_at(idx, full_layout);
	if(id)
		*id = pause_ids[action];
	if(label)
		*label = pause_label(action, host);
	return true;
}

const char* screen_pause_test_controller_name(int category) {
	return controller_family_name((enum input_category)category);
}
#endif

/* Use the same native painter in the app and renderer probes. */
static void pause_panel_draw(const struct menu_rect* r) {
 gfx_texture(false);
 gutil_texquad_col(r->x-2,r->y-2,0,0,0,0,r->w+4,r->h+4,17,17,17,255);
 gfx_texture(true);gfx_bind_texture(&texture_gui_crafting);
 for(int yy=0;yy<r->h;yy+=32)for(int xx=0;xx<r->w;xx+=32){int w=r->w-xx<32?r->w-xx:32,h=r->h-yy<32?r->h-yy:32;gutil_texquad_col(r->x+xx,r->y+yy,150,62,(w+1)/2,(h+1)/2,w,h,255,255,255,255);}
 gfx_texture(false);
 gutil_texquad_col(r->x,r->y,0,0,0,0,r->w,3,255,255,255,153);
 gutil_texquad_col(r->x,r->y,0,0,0,0,3,r->h,255,255,255,153);
 gutil_texquad_col(r->x,r->y+r->h-3,0,0,0,0,r->w,3,85,85,85,255);
 gutil_texquad_col(r->x+r->w-3,r->y,0,0,0,0,3,r->h,85,85,85,255);
 gfx_texture(true);
}
static void pause_button_draw(const struct menu_rect* r,const char* label,bool hover) {
 int half=r->w/GFX_GUI_SCALE/2, ty=hover ? 86 : 66;
 gfx_bind_texture(&texture_gui_widgets);
 gutil_texquad_col(r->x,r->y,0,ty,half,20,r->w/2,r->h,255,255,255,255);
 gutil_texquad_col(r->x+r->w/2,r->y,200-half,ty,half,20,r->w/2,r->h,255,255,255,255);
 int scale=16,tw=gutil_font_width(label,scale);
 gutil_text_rgb(r->x+(r->w-tw)/2,r->y+(r->h-scale)/2,label,scale,true,
  hover ? 255 : 224,hover ? 255 : 224,224);
}

static void screen_pause_render2D(struct screen* s, int width, int height) {
	(void)s;
	// darken background
	gfx_texture(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, width, height, 0, 0, 0, 180);
	gfx_texture(true);

	struct menu_composition c;
	pause_compose(&c, width, height);
	bool full = c.layout == MENU_SCREEN_FULL;
	bool host = pause_is_host();
	size_t rows = c.action_count;

 if(!full) pause_panel_draw(&c.window);
 struct menu_rect head=full ? c.header : menu_composition_header_content(&c);
 int title_scale=full ? 26 : 16;
 int title_x=full ? head.x+(head.w-gutil_font_width("Game menu",title_scale))/2 : head.x;
 if(full) gutil_text(title_x,head.y+(head.h-title_scale)/2,"Game menu",title_scale,true);
 else gutil_text_rgb(title_x,head.y+(head.h-title_scale)/2,"Game menu",title_scale,false,64,64,64);
 for(size_t k=0;k<rows;k++)
  pause_button_draw(&c.actions[k],pause_label(pause_action_at(k,full),host),selected==(int)k);
 enum input_category family=pause_owner_category();
 const char* name=controller_family_name(family);
 struct input_device device={0};
 (void)input_player_device(0,&device);
 enum controller_picture picture=controller_picture_of(device,family);
 struct menu_rect status=full?menu_pause_status(&c,(int)rows)
  :(struct menu_rect){0,176,width,24};
 int pw=full?gutil_controller_picture_width(picture,64):40;
 int sx=width/2-(full?146:105);
 gutil_controller_picture(sx+(full?120-pw:0),status.y,pw,full?64:24,picture);
 /* Status line: the split/compact "World continues" as before; while an
  * unload is in flight or has just failed, the outcome in both layouts. */
 const char* label=exit_pending?"Saving world...":exit_failed?
  (gstate.players[0].hud_message[0]?gstate.players[0].hud_message:"Save failed - try again")
  :full?NULL:"World continues";
 uint8_t lr=exit_failed?255:64,lg=exit_failed?85:64,lb=exit_failed?85:64;
 if(full) {
  gutil_text(sx+128,status.y+25,name,14,true);
  if(label) gutil_text_rgb((width-gutil_font_width(label,14))/2,status.y+26,label,14,false,lr,lg,lb);
 } else {
  /* Compact: the hint row's strip (split 196-224) takes the old second
   * line, so the state joins the controller name on the one status line. */
  gutil_text_rgb(sx+46,status.y+5,name,14,false,64,64,64);
  if(label) gutil_text_rgb(sx+46+gutil_font_width(name,14)+12,status.y+5,label,14,false,lr,lg,lb);
 }
 {
  /* The centred hint row (controller UI redesign 10-31, A1 helper). */
  struct input_player_frame f;input_prompt_frame(0,&f);
  static const struct gutil_hint_item items[]={
   {{IB_GUI_UP},1,"Move",0,false},{{IB_GUI_CLICK},1,"Select",1,false},
   {{IB_HOME},1,"Resume",2,true}};
  struct gutil_hint_row row;
  if(gutil_hint_row_layout(&f,0,0,width,height,!full,GUTIL_HINT_ALIGN_CENTRE,items,3,&row))
   gutil_hint_row_draw(&row);
 }

	/* Approved notice composition for THIS frame: into the reserved header
	 * slot in the compact layout, the floating card otherwise
	 * (menu_header.h). */
	screen_notice_compose(c.notice.x,c.notice.y,c.notice.w,c.notice.h,c.notice_in_header);

	if(pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(pointer_x, pointer_y, glm_rad(pointer_angle), 0, 0,
							 256, 256, 48 * GFX_GUI_SCALE,
							 48 * GFX_GUI_SCALE);
	}
}

struct screen screen_pause = {
	.reset = screen_pause_reset,
	.update = screen_pause_update,
	.render2D = screen_pause_render2D,
	.render3D = NULL,
	.render_world = true,
};
