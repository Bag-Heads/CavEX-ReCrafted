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

#include "../../graphics/gui_util.h"
#include "../../graphics/gfx_settings.h"
#include "../../entity/combat.h"
#include "../../network/level_archive.h"
#include "../../network/world_copy.h"
#include "import_convert_notice.h"
#include "../../network/server_interface.h"
#include "../../network/server_local.h" /* R3: server_local_create_spawns */
#include "../../platform/audio.h" // S0: GUI click (playSoundFX parity path)
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/texture.h"
#include "../../stack.h"
#include "../split_recovery_script.h"
#include "../split_offer_piston_script.h"
#include "../vehicle_smoke_script.h" /* host_vehicle_smoke_8m rig name + decls only */
#include "../../util.h"
#include "../../worldgen/worldgen.h"
#include "../game_state.h"
#include "../../item/item_types.h" /* the row's mode icon: a real pack item */
#include "../../platform/input_glyph.h" /* the footer's detected prompts */
#include "frontend_presentation.h"
#include "play_session.h"
#include "screen.h"
#include "loading_notice.h"
#include "spawn_search_wait.h" /* R3: the create wait's idle */ /* asyncsnd-0926: the world-entry gate */
#include "loading_tips.h"
#include "widget_slotlist.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <m-lib/m-string.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h> // _mkdir (Windows only; Linux/Wii PC builds use POSIX mkdir via sys/stat.h)
#include <windows.h>
#else
#include <unistd.h>
#endif

static struct stack* worlds = NULL;

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
static void* (*s_sworld_owner_alloc)(size_t) = malloc;
void screen_sworld_test_set_owner_alloc_hook(void* (*alloc_fn)(size_t)) {
	s_sworld_owner_alloc = alloc_fn ? alloc_fn : malloc;
}
#else
#define s_sworld_owner_alloc malloc
#endif

static size_t gui_selection;
/* -1 = the list owns directional focus; otherwise a slot in
 * sworld_focus_order below. Pointer-free reachability for the action strip
 * (Astra Lane A item 1): with no pointer the only action this screen could
 * reach was Play Selected World. */
/* THE APPROVED LOAD TAB's ids, canvas and state -- declared here because
 * screen_sworld_reset (below) seeds them; the design notes sit with the
 * geometry block further down. */
enum sworld_button {
	SWORLD_BUTTON_TAB_LOAD = 0,
	SWORLD_BUTTON_TAB_CREATE,
	SWORLD_BUTTON_TAB_JOIN,
	SWORLD_BUTTON_PREV,
	SWORLD_BUTTON_NEXT,
	SWORLD_BUTTON_PLAY, /* also the Retry / Create-a-world slot */
	SWORLD_BUTTON_OPTIONS,
	SWORLD_BUTTON_COUNT,
};
enum sworld_modal_row {
	SWORLD_MODAL_RENAME = 0,
	SWORLD_MODAL_DELETE,
	SWORLD_MODAL_BACK,
	SWORLD_MODAL_COUNT,
};
#define SW_ROW_Y0 110
#define SW_ROW_PITCH 48
#define SW_ROWS_VISIBLE 4
/* SCREEN-LOCAL PAGING (load-visual, LOAD-NATIVE-REVIEW item 1): the approved
 * list is four aligned rows per page starting at canvas y 110. widget_slotlist
 * deliberately negative-centres a short list (Beta's GuiSlot) and is NOT
 * changed; it stays the selection model only. page_first is the index of the
 * first row on the shown page (a multiple of SW_ROWS_VISIBLE), and every
 * draw / hit / navigation position is computed from the LOCAL visible index
 * row - page_first (never from an unbounded size_t cast to a pixel). */
static size_t page_first;
static void sworld_reveal_selection(void); /* defined with the paging helpers */
static int focus_button = -1;
static bool list_error;
static bool modal_open;
static int modal_row = SWORLD_MODAL_BACK;
static char modal_path[512];
static char modal_name[160];
static struct gutil_menu_activate_state sworld_predict;
/* The visible status line (a refused management path / remember), drawn in
 * the page band and exported through screen_sworld_test_hint; cleared on
 * every reset. */
static char sworld_status[128];
struct sworld_canvas {
	int ox;
	float u;
};
static struct sworld_canvas sworld_canvas_of(int width, int height) {
	struct sworld_canvas c;
	c.u = height > 0 ? (float)height / 480.0F : 1.0F;
	int canvas = (int)(640.0F * c.u + 0.5F);
	c.ox = (width - canvas) / 2;
	if(c.ox < 0)
		c.ox = 0;
	return c;
}
static int sw_d(const struct sworld_canvas* c, int v) {
	return (int)((float)v * c->u + 0.5F);
}
static int sw_x(const struct sworld_canvas* c, int v) {
	return c->ox + sw_d(c, v);
}
static struct slotlist world_slots;
/* A3 H-1: the strip used to be hard-partitioned on `pointer_available` -- with
 * a pointer live it drew nothing, could not be activated, and still ate the
 * D-pad presses that drove it. It arbitrates now, through the same
 * movement-based helper the title/confirm/picker use: whoever owns focus this
 * frame owns BOTH the highlight and the activation. */
static struct gutil_menu_focus focus;
static bool pointer_owns_focus;
static bool pointer_available;
static float pointer_x, pointer_y, pointer_angle;
static int pointer_button = -1;
/* WHO CHOSE the highlight -- see screen_title.c's note (lead ruling R-A,
 * closing review A6-1). Sticky: it moves only when the player does, never on
 * an IR validity transition. */
static enum gutil_menu_origin sworld_origin;
static int sworld_focus_target(void); /* defined next to the strip geometry */

static size_t screen_sworld_count(void) {
	return worlds ? stack_size(worlds) : 0;
}

/* INCREMENTAL WORLD-LIST RESOLVE (2026-07-30, handoff P3(a)).

   screen_sworld_reset used to open, read and GZIP-INFLATE every world's level.dat inside
   screen_set(), which screen.c:25-30 runs SYNCHRONOUSLY on the main thread. Save-and-quit
   calls screen_set(&screen_select_world) directly (screen_pause.c), so on the maintainer's
   118-world card the main loop made NO PROGRESS for over 3 s and the watchdog fired a HANG
   report reading main=SCRN_UPD -- the phase that brackets current_screen->update()
   (main.c:976-979). The SAVE was never the problem: it runs on the server thread, which is
   why the same report reads srv=RPC.

   Now reset() only ENUMERATES (opendir/readdir/stat -- cheap) and pushes every row PENDING;
   screen_sworld_update resolves K rows per frame. Two properties of that split are
   load-bearing, so do not "improve" them:

   (1) The readdir sweep stays WHOLE inside reset(), so stack_size(worlds) is FINAL when
       reset() returns. That keeps the autoload-by-directory scan matching against a
       complete list (a partial list would latch the WRONG world), and keeps the
       selection-bounds arithmetic exactly as reachable as it was before.
   (2) No stack_push happens after reset() returns, so pointers into worlds->data are
       stable for the whole resolve. That is what makes the write-back below safe --
       and a write-back is REQUIRED: stack_at() hands out a memcpy COPY, and string_t is
       an SSO union, so storing a resolved name into a copy either lands in the copy and
       is lost (row never resolves -> re-parsed every frame forever) or aliases the heap
       pointer the stack entry still holds -> dangling read in render2D.

   DEVIATION, not parity: Beta enumerates eagerly and unbudgeted (GuiSelectWorld.initGui ->
   loadSaves -> SaveConverterMcRegion.getSaveList). K is a WATCHDOG-SAFETY number, not a
   Beta one -- see docs/deviations.md. */
enum world_row_state {
	WORLD_ROW_PENDING = 0, // enumerated; level.dat not read yet
	WORLD_ROW_OK,		   // level.dat parsed
	WORLD_ROW_UNREADABLE   // level.dat absent, corrupt, or unsupported
};

struct world_option {
	string_t name; // resolved LevelName; the DIRECTORY name until then
	string_t directory;
	string_t path;
	int64_t last_access; // 0 until resolved
	int64_t byte_size;	 // 0 until resolved
	uint8_t state;		 // enum world_row_state
	// LEVEL_GAME_MODE, resolved with the rest of the row's level.dat
	// metadata (false until then, and for an UNREADABLE row). Drives the
	// LCE-style "(Creative)" suffix below.
	bool creative;
	// convert-notice-1024: import_convert_notice_needed() of this row's
	// level.dat, cached by the same resolve open as `creative` so Play on a
	// resolved row needs no second open (false until resolved).
	bool needs_convert;
};

static size_t resolve_cursor = 0;		 // next row to resolve
static size_t archive_opens = 0;		 // WORK-PER-CALL instrument (tests assert on this)
// convert-notice-1024: level.dat opens made by a Play (PENDING rows only).
static size_t play_archive_opens = 0;
static uint32_t resolve_step_max_ms = 0; // worst single resolve step this boot
// Hard ceiling on the per-frame budget, whatever the config says. A frame that
// resolves more than this is back to being an unbounded SD sweep on the main thread.
#define SWORLD_RESOLVE_PER_FRAME_MAX 64

/* HIDDEN TUTORIAL WORLD (maintainer ruling 2026-10-31): the shipped tutorial
 * save (saves/SCREEN_TITLE_TUTORIAL_DIR, the SAME folder constant the title's
 * Tutorial row opens) is never a Load-tab row. Matched on the DIRECTORY name,
 * never on its LevelName ("Tutorial Island"), so a player's own world that
 * happens to share the display name still lists. Because it is never pushed,
 * every per-row action (Play, delete, copy, rename, options) and the row count
 * / empty-list text cannot reach it. Autoload by that exact directory name
 * (hardware-profile harness) still opens it: see sworld_hidden_autoload_path. */
static bool sworld_tutorial_hidden = false; /* seen (and skipped) by the last reset */

static bool sworld_dir_is_hidden(const char* name) {
	return strcmp(name, SCREEN_TITLE_TUTORIAL_DIR) == 0;
}

// Portable "is <parent>/<name> a directory?" test. Uses stat() rather than
// dirent.d_type: d_type is not populated by every libc (notably the Windows
// dirent shim used by the GLFW/PC build, where referencing DT_DIR broke the
// link) and is DT_UNKNOWN on some filesystems. Mirrors texturepacks_list().
static bool world_dir_is_directory(const char* parent, const char* name) {
	string_t p;
	string_init_printf(p, "%s/%s", parent, name);
	struct stat st;
	bool is_dir = stat(string_get_cstr(p), &st) == 0 && S_ISDIR(st.st_mode);
	string_clear(p);
	return is_dir;
}

static bool sworld_ensure_directory_tree(const char* path);

static void screen_sworld_reset(struct screen* s, int width, int height) {
	/* Returning from the sound gate's Back also drops its pending reason. */
	loading_reason_end(0);
	input_pointer_enable(true);

	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;

	if(worlds) {
		while(!stack_empty(worlds)) {
			struct world_option opt;
			stack_pop(worlds, &opt);
			string_clear(opt.name);
			string_clear(opt.directory);
			string_clear(opt.path);
		}

		stack_destroy(worlds);
		free(worlds);
		worlds = NULL;
	}

	/* Reset visible/menu state even if the tiny list-owner allocation fails. The
	 * Wii build removes stack_create's assert, so passing NULL through here used
	 * to turn heap pressure into immediate field writes through address zero. */
	resolve_cursor = 0;
	archive_opens = 0;
	sworld_tutorial_hidden = false;
	gui_selection = 0;
	focus_button = -1;
	modal_open = false;
	modal_row = SWORLD_MODAL_BACK;
	list_error = false;
	page_first = 0;
	sworld_status[0] = '\0';
	sworld_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	focus = (struct gutil_menu_focus) {0};
	pointer_owns_focus = false;
	pointer_available = false;
	pointer_button = -1;
	sworld_predict = (struct gutil_menu_activate_state) {
		.pressed = true,
		.hovered = GUTIL_MENU_TARGET_NONE,
		.selection_origin = sworld_origin,
		.selected = GUTIL_MENU_TARGET_NONE,
	};
	/* The approved list band: rows of 48 px pitch from canvas y 110, four
	 * visible inside the 214 px panel (the slotlist keeps the scroll /
	 * selection model; this screen draws and hit-tests its own row rects).
	 * Reset empty first so the OOM return is usable. */
	{
		struct sworld_canvas c = sworld_canvas_of(width, height);
		int top = sw_d(&c, SW_ROW_Y0) - 4 * GFX_GUI_SCALE;
		slotlist_reset(&world_slots, width, top,
					   top + 4 * GFX_GUI_SCALE + sw_d(&c, SW_ROW_PITCH) * SW_ROWS_VISIBLE,
					   sw_d(&c, SW_ROW_PITCH), 0, SLOT_LIST_NO_SELECTION);
	}

	worlds = s_sworld_owner_alloc(sizeof(struct stack));
	if(!worlds) {
		/* OOM is an ERROR the player can Retry -- never an empty list. */
		fprintf(stderr,
				"[world-list] list owner allocation failed; showing Retry\n");
		list_error = true;
		focus_button = SWORLD_BUTTON_TAB_LOAD;
		return;
	}
	stack_create(worlds, 8, sizeof(struct world_option));

	const char* saves_path
		= config_read_string(&gstate.config_user, "paths.worlds", "saves");

	DIR* d = opendir(saves_path);
	if(!d && errno == ENOENT) {
		/* A saves folder that does not exist yet (fresh card, or paths.worlds
		 * naming a folder nothing has created) is Beta's "no worlds" state:
		 * GuiSelectWorld -> SaveConverterMcRegion makes the directory before
		 * listing it. Create it here too, so Retry is not a permanent loop
		 * over the same ENOENT (ci-1101/27). Only ENOENT: anything else
		 * (a file in the way, EACCES) is a real error below. */
		if(sworld_ensure_directory_tree(saves_path))
			d = opendir(saves_path);
		else
			fprintf(stderr, "[world-list] cannot create missing \"%s\": %s\n",
					saves_path, strerror(errno));
	}
	if(!d) {
		/* An unreadable saves directory is an ERROR with Retry, disclosed --
		 * not "no worlds". */
		fprintf(stderr, "[world-list] cannot open \"%s\": %s -- showing Retry\n",
				saves_path, strerror(errno));
		list_error = true;
	}

	if(d) {
		struct world_copy_journals* journals;
		bool journals_ok = world_copy_journals_read(saves_path, &journals);
		if(!journals_ok) {
			fprintf(stderr, "[world-list] cannot read copy journals; showing Retry\n");
			list_error = true;
		}
		struct dirent* dir;
		while(journals_ok && (dir = readdir(d))) {
			if(*dir->d_name != '.'
			   && world_dir_is_directory(saves_path, dir->d_name)) {
				string_t marker;
				string_init_printf(marker, "%s/%s/.incomplete", saves_path, dir->d_name);
				struct stat marker_stat;
				bool incomplete = stat(string_get_cstr(marker), &marker_stat) == 0;
				string_clear(marker);
				if(incomplete) continue;
				string_t destination;
				string_init_printf(destination, "%s/%s", saves_path, dir->d_name);
				bool journaled = world_copy_journals_contains(journals, string_get_cstr(destination));
				string_clear(destination);
				if(journaled) continue;
				if(sworld_dir_is_hidden(dir->d_name)) {
					sworld_tutorial_hidden = true;
					continue;
				}
				// ENUMERATE ONLY. The level.dat open + gzip inflate + NBT walk moved to
				// screen_sworld_resolve_step; see the note on struct world_option.
				struct world_option opt;
				string_init_printf(opt.path, "%s/%s", saves_path, dir->d_name);
				string_init_set_str(opt.directory, dir->d_name);
				// Beta's own fallback when a save has no readable LevelName is the
				// DIRECTORY name (SaveConverterMcRegion.getSaveList), so seeding the title
				// with it doubles as the unresolved-row label -- no "Missing name" flash.
				string_init_set_str(opt.name, dir->d_name);
				opt.last_access = 0;
				opt.byte_size = 0;
				opt.state = WORLD_ROW_PENDING;
				// `opt` is an uninitialised stack local with per-field
				// assignment -- a new field MUST be seeded here or the row
				// carries stack garbage until (and unless) the resolve step
				// reaches it. An UNREADABLE row never resolves at all.
				opt.creative = false;
				opt.needs_convert = false;

				// stack_push DROPS the push when a realloc-grow fails (it returns void and
				// counts the drop in stack_oom_drops). Detect that and free the three
				// strings we just initialised -- otherwise every dropped push leaks them.
				size_t before = screen_sworld_count();
				stack_push(worlds, &opt);
				if(screen_sworld_count() == before) {
					string_clear(opt.name);
					string_clear(opt.directory);
					string_clear(opt.path);
					/* A dropped row would present a PARTIAL list as the whole
					 * truth. It is an ERROR with Retry, like a failed opendir. */
					fprintf(stderr,
							"[world-list] row push dropped (allocation) -- showing "
							"Retry\n");
					list_error = true;
				}
			}
		}

		world_copy_journals_free(journals);
		closedir(d);
	}

	slotlist_set_count(&world_slots, screen_sworld_count());
	/* IDENTITY RESTORE, after the enumeration is COMPLETE and before the
	 * slotlist selection: the session's remembered world directory re-seats
	 * the selection by DIRECTORY, never by index (fallback row 0; a deleted
	 * remembered world lands there too). Then the default focus: the selected
	 * row when there are rows, the Load tab header when there are none. */
	const char* remembered = play_session_world_dir();
	if(remembered && remembered[0]) {
		for(size_t i = 0; i < screen_sworld_count(); i++) {
			const struct world_option* o
				= (const struct world_option*)worlds->data + i;
			if(strcmp(string_get_cstr(o->directory), remembered) == 0) {
				gui_selection = i;
				break;
			}
		}
	}
	if(screen_sworld_count() > 0) {
		slotlist_select(&world_slots, gui_selection, true);
		sworld_reveal_selection(); /* the page the restored row is on */
		focus_button = -1;
	} else {
		page_first = 0;
		focus_button = SWORLD_BUTTON_TAB_LOAD;
	}
}

static int sworld_mkdir(const char* path) {
#ifdef _WIN32
	return _mkdir(path); // Windows (mingw) only
#else
	return mkdir(path, 0777); // Wii (devkitPPC) + Linux PC: POSIX mkdir
#endif
}

static int sworld_rmdir(const char* path) {
#ifdef _WIN32
	return _rmdir(path);
#else
	return rmdir(path);
#endif
}

static bool sworld_is_separator(char c) { return c == '/' || c == '\\'; }

static bool sworld_ensure_directory(const char* path) {
	/* Device/UNC roots can be valid directories even when mkdir(root) reports an
	 * implementation-specific error other than EEXIST. Recognize the existing
	 * directory first; the unique world leaf still uses raw mkdir as its atomic
	 * reservation below. */
	struct stat st;
	if(stat(path, &st) == 0)
		return S_ISDIR(st.st_mode);
	if(sworld_mkdir(path) == 0)
		return true;
	if(errno != EEXIST)
		return false;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static size_t sworld_directory_root_length(const char* path, size_t length) {
	/* Windows drives and Wii device names are both an arbitrary prefix followed
	 * by ":/". Require the colon to precede every separator so a colon inside a
	 * later path component cannot turn that component into a root. */
	const char* colon = strchr(path, ':');
	if(colon && colon > path && (size_t)(colon - path) + 1 < length
	   && sworld_is_separator(colon[1])) {
		bool prefix = true;
		for(const char* p = path; p < colon; p++)
			if(sworld_is_separator(*p)) {
				prefix = false;
				break;
			}
		if(prefix)
			return (size_t)(colon - path) + 2;
	}

	if(length == 0 || !sworld_is_separator(path[0]))
		return 0;
	if(length < 2 || !sworld_is_separator(path[1]))
		return 1; /* POSIX root */

	/* UNC root: protect //server/share/ from both trimming and mkdir attempts. */
	size_t root = 2;
	while(root < length && !sworld_is_separator(path[root]))
		root++;
	if(root < length)
		root++;
	while(root < length && !sworld_is_separator(path[root]))
		root++;
	if(root < length)
		root++;
	return root;
}

static size_t sworld_trim_directory_path(char* path, size_t length) {
	size_t root = sworld_directory_root_length(path, length);
	while(length > root && sworld_is_separator(path[length - 1]))
		path[--length] = '\0';
	return length;
}

/* mkdir -p with support for POSIX roots, Windows drive/UNC roots, and the
 * Wii's device prefixes (for example sd:/apps/CaveWii/saves). */
static bool sworld_ensure_directory_tree(const char* path) {
	if(!path || !path[0])
		return false;

	size_t length = strlen(path);
	char* work = malloc(length + 1);
	if(!work)
		return false;
	memcpy(work, path, length + 1);

	length = sworld_trim_directory_path(work, length);
	size_t start = sworld_directory_root_length(work, length);

	bool ok = true;
	for(size_t i = start; i < length; i++) {
		if(!sworld_is_separator(work[i]) || i == 0
		   || sworld_is_separator(work[i - 1]))
			continue;
		char separator = work[i];
		work[i] = '\0';
		if(!sworld_ensure_directory(work))
			ok = false;
		work[i] = separator;
		if(!ok)
			break;
	}
	if(ok)
		ok = sworld_ensure_directory(work);

	free(work);
	return ok;
}

/* mkdir itself is the reservation: unlike stat-then-mkdir, two callers cannot
 * both choose the same name. Existing files count as occupied names too. */
static bool sworld_reserve_unique_directory_from(const char* saves_path,
										  uint32_t first, string_t out) {
	if(!saves_path || !saves_path[0] || !out)
		return false;
	size_t base_length = strlen(saves_path);
	char* base = malloc(base_length + 1);
	if(!base)
		return false;
	memcpy(base, saves_path, base_length + 1);
	(void)sworld_trim_directory_path(base, base_length);
	if(!sworld_ensure_directory_tree(base)) {
		free(base);
		return false;
	}
	if(first == 0)
		first = 1;
	base_length = strlen(base);
	const char* join = sworld_is_separator(base[base_length - 1]) ? "" : "/";

	for(uint32_t n = first;; n++) {
		if(n == 1)
			string_printf(out, "%s%sNew World", base, join);
		else
			string_printf(out, "%s%sNew World %lu", base, join,
						  (unsigned long)n);
		if(sworld_mkdir(string_get_cstr(out)) == 0) {
			free(base);
			return true;
		}
		if(errno != EEXIST || n == UINT32_MAX) {
			free(base);
			return false;
		}
	}
}

static bool sworld_persist_new_archive(string_t directory, int64_t seed,
									   vec3 world_spawn, vec3 player_spawn,
									   bool start_nether,
									   const char* display_name,
									   bool creative) {
	struct level_archive la;
	if(!level_archive_create_new(&la, directory, seed, world_spawn))
		return false;

	vec2 rot0 = {0.0F, 0.0F};
	vec3 vel0 = {0.0F, 0.0F, 0.0F};
	/* LevelName [740c583 GuiCreateWorld.actionPerformed -> startWorld's
	 * world name]: the RAW typed display name. The template always carries
	 * the tag, so the strict writer applies. Before the dialog existed
	 * nothing wrote it and the world list fell back to the directory name. */
	bool ok = display_name && display_name[0]
		&& level_archive_write(&la, LEVEL_NAME, (void*)display_name);
	if(ok)
		ok = level_archive_write_player(
			&la, player_spawn, rot0, vel0,
			start_nether ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD);
	/* Per-world game mode (LEVEL_GAME_MODE). Written ONLY for a creative
	 * world: absent == survival by definition, so a survival create leaves
	 * level.dat byte-identical to what it was before this feature existed
	 * (docs/ui_design/create_world_console_2026-09-01.md section 3.1). */
	if(ok && creative)
		ok = level_archive_write_game_mode(&la, true);
	if(ok)
		ok = level_archive_save(&la);
	/* save() was explicit so its result could gate the load request. Do not let
	 * destroy() silently retry and turn a reported failure into ambiguous state. */
	level_archive_discard(&la);
	return ok;
}

static void sworld_remove_incomplete_directory(string_t directory) {
	static const char* files[] = {"level.dat", "level.dat_new", "level.dat_old"};
	string_t path;
	for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		string_init_printf(path, "%s/%s", string_get_cstr(directory), files[i]);
		(void)remove(string_get_cstr(path));
		string_clear(path);
	}
	string_init_printf(path, "%s/region", string_get_cstr(directory));
	(void)sworld_rmdir(string_get_cstr(path));
	string_clear(path);
	(void)sworld_rmdir(string_get_cstr(directory));
}

/* Beta ISaveFormat.deleteWorldDirectory [740c583 GuiSelectWorld
 * .deleteWorld]: delete the world tree recursively. Classify each path before
 * opening it, including the selected world root. */
static void sworld_delete_tree(const char* path, int depth) {
	/* The Wii path keeps its original filesystem behavior. */
#if defined(_WIN32) && !defined(PLATFORM_WII)
	DWORD attributes = GetFileAttributesA(path);
	if(attributes == INVALID_FILE_ATTRIBUTES) {
		if(GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
			fprintf(stderr, "[world] cannot classify for delete: %s\n", path);
		return;
	}
	if(attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		BOOL removed = (attributes & FILE_ATTRIBUTE_DIRECTORY)
			? RemoveDirectoryA(path) : DeleteFileA(path);
		if(!removed)
			fprintf(stderr, "[world] cannot remove link: %s\n", path);
		return;
	}
	if(!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
		if(!DeleteFileA(path))
			fprintf(stderr, "[world] cannot remove file: %s\n", path);
		return;
	}
#elif !defined(PLATFORM_WII)
	struct stat st;
	if(lstat(path, &st) != 0) {
		if(errno != ENOENT)
			fprintf(stderr, "[world] cannot classify for delete: %s\n", path);
		return;
	}
	if(S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
		if(unlink(path) != 0)
			fprintf(stderr, "[world] cannot remove entry: %s\n", path);
		return;
	}
#endif
	if(depth > 4)
		return;
	DIR* d = opendir(path);
	if(d) {
		struct dirent* e;
		while((e = readdir(d))) {
			if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			string_t p;
			string_init_printf(p, "%s/%s", path, e->d_name);
			#if defined(PLATFORM_WII)
			if(world_dir_is_directory(path, e->d_name))
				sworld_delete_tree(string_get_cstr(p), depth + 1);
			else
				(void)remove(string_get_cstr(p));
			#else
			sworld_delete_tree(string_get_cstr(p), depth + 1);
			#endif
			string_clear(p);
		}
		closedir(d);
	}
	#if defined(PLATFORM_WII)
	(void)sworld_rmdir(path);
	#else
	if(sworld_rmdir(path) != 0)
		fprintf(stderr, "[world] cannot remove directory: %s\n", path);
	#endif
}

/* The confirm callback (screen_confirm contract: the callback owns the
 * follow-up screen_set, like Beta's deleteWorld -> displayGuiScreen).
 * Re-entering select-world re-enumerates [740c583 loadSaves-after-delete]. */
static char sworld_delete_path[512];
static void sworld_delete_confirmed(bool yes) {
	if(yes && sworld_delete_path[0])
		sworld_delete_tree(sworld_delete_path, 0);
	sworld_delete_path[0] = 0;
	screen_set(&screen_select_world);
}

/* Beta's dialog folder reservation [740c583 GuiCreateWorld
 * .generateUnusedFolderName]: append "-" while the name is taken --
 * realized with the same mkdir-as-reservation guarantee as the numbered
 * scheme above (Beta's names, CaveWii's concurrency). */
static bool sworld_reserve_beta_directory(const char* saves_path,
										  const char* folder_base,
										  string_t out) {
	if(!saves_path || !saves_path[0] || !folder_base || !folder_base[0]
	   || !out)
		return false;
	size_t base_length = strlen(saves_path);
	char* base = malloc(base_length + 1);
	if(!base)
		return false;
	memcpy(base, saves_path, base_length + 1);
	(void)sworld_trim_directory_path(base, base_length);
	if(!sworld_ensure_directory_tree(base)) {
		free(base);
		return false;
	}
	base_length = strlen(base);
	const char* join = sworld_is_separator(base[base_length - 1]) ? "" : "/";

	char candidate[192];
	size_t cand_len = strlen(folder_base);
	if(cand_len + 1 > sizeof(candidate)) {
		free(base);
		return false;
	}
	memcpy(candidate, folder_base, cand_len + 1);
	struct world_copy_journals* journals;
	if(!world_copy_journals_read(base, &journals)) {
		free(base);
		return false;
	}

	for(int guard = 0; guard < 64; guard++) {
		string_printf(out, "%s%s%s", base, join, candidate);
		bool journaled = world_copy_journals_contains(journals, string_get_cstr(out));
		if(!journaled && sworld_mkdir(string_get_cstr(out)) == 0) {
			world_copy_journals_free(journals);
			free(base);
			return true;
		}
		if((!journaled && errno != EEXIST) || cand_len + 2 > sizeof(candidate)) {
			world_copy_journals_free(journals);
			free(base);
			return false;
		}
		candidate[cand_len++] = '-';
		candidate[cand_len] = 0;
	}
	world_copy_journals_free(journals);
	free(base);
	return false;
}

// Harness fixed-seed override (hw_run --cw-seed, 2026-09-04). See screen.h's
// screen_sworld_set_seed_override doc comment.
static bool hw_seed_override_set = false;
static int64_t hw_seed_override_value = 0;

void screen_sworld_set_seed_override(bool set, int64_t seed) {
	hw_seed_override_set = set;
	hw_seed_override_value = seed;
}

/* The shared create tail: region dir, seed resolution, spawn search,
 * archive persist, SRPC_LOAD_WORLD. `dir` is an already-RESERVED world
 * directory (cleaned up on failure). Seed precedence: the hw_run --cw-seed
 * harness override (screen_sworld_set_seed_override) outranks everything --
 * a harness run that explicitly asked for a seed should always get it --
 * then the debug.world_seed capture-rig pin (the rig is the only consumer
 * of the pin and never types one), then a dialog-typed seed, then the
 * per-create random mix. */
/* REL-03b: worldgen.c defines the real worldgen_last_spawn_search_ok. The
 * front-end harnesses (tests/support/frontend_screen_rig.h) link this file
 * with a stubbed worldgen_find_initial_spawns and no worldgen.c; for them
 * this weak fallback reports success, matching the stub (same weak-fallback
 * pattern as service_observer.h). Any build with worldgen.c takes the strong
 * definition. */
__attribute__((weak)) bool worldgen_last_spawn_search_ok(void) { return true; }
/* R3: server_local_create_spawns (server_local.c) and spawn_search_wait_idle
 * (spawn_search_wait.c) are real in the game and the full test pools; the
 * front-end screen rigs stub both in tests/support/frontend_screen_rig.h,
 * next to their worldgen_find_initial_spawns stub. */

static bool sworld_finish_create(string_t dir, const char* display_name,
								 bool seed_set, int64_t typed_seed,
								 bool creative) {
	string_t region;
	string_init_printf(region, "%s/region", string_get_cstr(dir));
	bool region_ok = sworld_ensure_directory(string_get_cstr(region));
	string_clear(region);
	if(!region_ok) {
		fprintf(stderr, "[world] could not create region directory in %s\n",
				string_get_cstr(dir));
		sworld_remove_incomplete_directory(dir);
		return false;
	}

	// Vary the seed per create: mix the clock with a session salt so worlds
	// differ even if the RTC is unset on hardware.
	static uint64_t salt = 0;
	salt++;
	int64_t seed = (int64_t)((uint64_t)CLOCK_SEED_TIME() * 6364136223846793005ULL
							 + salt * 1442695040888963407ULL);

	if(seed_set)
		seed = typed_seed;

	// debug.world_seed: pin a FIXED worldgen seed for deterministic test worlds
	// (e.g. an ocean-shore seed found by tools/find_ocean_seed for far-water captures,
	// since the far ring regenerates from worldgen and a placed demo can't fake it).
	// 0/unset keeps the seed chosen above. The spawn search below then runs
	// on the pinned seed, so the same seed reproduces the same world every boot.
	int seed_override = config_read_int(&gstate.config_user, "debug.world_seed", 0);
	if(seed_override != 0)
		seed = (int64_t)seed_override;

	// hw_run --cw-seed (harness fixed-seed lane, 2026-09-04): highest
	// precedence -- see screen_sworld_set_seed_override's doc comment (screen.h)
	// and this function's own doc comment above. Unset (the default) is
	// byte-identical to before this override existed.
	if(hw_seed_override_set)
		seed = hw_seed_override_value;

	// Debug ("debug.start_nether"): create the world already in the Nether so a
	// ChunkProviderHell render can be captured without a portal (M5 is the real
	// dimension-switch). Off = normal overworld create.
	bool start_nether
		= strcmp(config_read_string(&gstate.config_user, "debug.start_nether",
								   "off"),
				 "on")
		== 0;

	bool superflat
		= strcmp(config_read_string(&gstate.config_user, "debug.superflat", "off"),
				 "on")
		== 0;
	vec3 world_spawn;
	vec3 player_spawn;
	/* R3: the search runs on the server thread against the live chunk
	 * provider (server_local_create_spawns): Beta's getInitialSpawnLocation
	 * loads AND populates the chunks it probes, drawing World.rand on the
	 * way, so the raw-terrain probe walked a different path for some seeds
	 * (sweep seed 3141592653589793238). Blocks like the old probe did. */
	const bool spawn_ok = server_local_create_spawns(
		string_get_cstr(dir), seed, superflat, start_nether, world_spawn,
		player_spawn, spawn_search_wait_idle, NULL);
	/* REL-03b (maintainer ruling 2026-10-17): a failed search left only the
	 * placeholder spawn. Beta never saves a world without its sand spawn
	 * (World.java:215-227), so stop here: nothing is persisted, the reserved
	 * directory goes, and the caller's create path reports the failure (the
	 * create-world dialog stays up with its failure line; the list screen
	 * shows this status line). */
	if(!spawn_ok) {
		fprintf(stderr, "[world] spawn search failed for seed %lld -- world "
						"%s not created\n",
				(long long)seed, string_get_cstr(dir));
		snprintf(sworld_status, sizeof(sworld_status),
				 "Couldn't find a spawn point - world not created");
		sworld_remove_incomplete_directory(dir);
		return false;
	}

	// Generator spawns are feet positions; CaveWii's network Player.Pos is the
	// eye. SpawnXYZ remains the independently probed Overworld block position.
	player_spawn[1] += PLAYER_EYE_HEIGHT;
	if(sworld_persist_new_archive(dir, seed, world_spawn, player_spawn,
								  start_nether, display_name, creative)) {
		/* The dialog's Game Mode selection IS the new world's mode; no need
		 * to read back the tag we just wrote. */
		gstate.config_creative = creative;
		/* asyncsnd-0926 world-entry gate (loading_notice.h): the load request
		 * is sent now, or parked behind the sound gate while the opt-in async
		 * registry is still loading; either way the next screen is set. */
		loading_reason_begin(0, NEW_WORLD);
		if(loading_notice_gate_load_world(string_get_cstr(dir),
										  &screen_select_world))
			return true;
		/* The world persisted fine; only the load request failed. Leave it
		 * on disk -- it appears in the list. */
		loading_reason_end(0);
		return false;
	}

	fprintf(stderr, "[world] failed to persist new world %s\n",
			string_get_cstr(dir));
	sworld_remove_incomplete_directory(dir);
	return false;
}

// Create a brand-new world: a unique directory under the saves path, a fresh
// level.dat (random seed + an on-land spawn), then load it. The Beta terrain
// generates on demand once the world is loaded. This is the INSTANT path --
// the "+new" autoload sentinel and headless capture rigs depend on it staying
// zero-input; the interactive path is the create-world dialog below.
static void screen_sworld_create_new(void) {
	const char* saves_path
		= config_read_string(&gstate.config_user, "paths.worlds", "saves");

	string_t dir;
	string_init(dir);
	if(!sworld_reserve_unique_directory_from(saves_path, 1, dir)) {
		fprintf(stderr, "[world] could not reserve a new directory under %s\n",
				saves_path);
		string_clear(dir);
		return;
	}

	/* The zero-input "+new" path is always SURVIVAL: the game mode is chosen
	 * in the create-world dialog, and a sentinel/capture-rig world has no
	 * dialog to choose in. */
	(void)sworld_finish_create(dir, "New World", false, 0, false);
	string_clear(dir);
}

/* split_recovery_6m's rig world (docs/split_recovery_profile.md, lead ruling
 * F1 2026-09-17). The profile's P4 phase reloads the SAME world by directory
 * name after a save-and-quit, so the name has to be KNOWN -- and
 * sworld_reserve_beta_directory above deliberately appends "-" when the name
 * is taken, which would make run 2 of the profile create "cw_recovery_rig-"
 * and then autoload run 1's stale world. This entry reserves the EXACT name
 * instead, replacing an earlier copy.
 *
 * It is the only code path in the game that deletes a world the player did not
 * select, so it is gated twice and cannot be aimed anywhere else:
 * split_recovery_rig_create_allowed() is false unless the split_recovery_6m
 * phase driver is ARMED (an hw_run profile, never a config key) AND
 * `directory` is that driver's single fixed constant. Everything after the
 * gate is the existing machinery -- sworld_delete_tree and sworld_finish_create
 * verbatim -- so the ordinary create and delete paths are untouched. */
bool screen_sworld_create_exact(const char* display_name, const char* directory,
								bool seed_set, int64_t seed) {
	if(!display_name || !split_recovery_rig_create_allowed(directory))
		return false;
	const char* saves_path
		= config_read_string(&gstate.config_user, "paths.worlds", "saves");
	if(!saves_path || !saves_path[0])
		return false;
	size_t base_length = strlen(saves_path);
	char* base = malloc(base_length + 1);
	if(!base)
		return false;
	memcpy(base, saves_path, base_length + 1);
	(void)sworld_trim_directory_path(base, base_length);
	if(!sworld_ensure_directory_tree(base)) {
		free(base);
		return false;
	}
	base_length = strlen(base);
	const char* join = sworld_is_separator(base[base_length - 1]) ? "" : "/";
	string_t dir;
	string_init_printf(dir, "%s%s%s", base, join, directory);
	free(base);
	/* Replace, not merge: a half-deleted earlier copy would reload as itself. */
	sworld_delete_tree(string_get_cstr(dir), 0);
	if(sworld_mkdir(string_get_cstr(dir)) != 0) {
		fprintf(stderr, "[world] rig world %s could not be reserved\n",
				string_get_cstr(dir));
		string_clear(dir);
		return false;
	}
	fprintf(stderr, "[world] split_recovery rig world reserved at %s\n",
			string_get_cstr(dir));
	bool ok = sworld_finish_create(dir, display_name, seed_set, seed, false);
	string_clear(dir);
	return ok;
}

/* O19: lives here (not in vehicle_smoke_script.c) because the frontend test
 * pools link this file without the driver; declared in
 * vehicle_smoke_script.h. Pure libc (stat/rename), so tests drive it
 * directly (tests/test_vehicle_rig_slot.c). */
enum vehicle_rig_slot vehicle_smoke_rig_prepare_slot(const char* rig_path,
	char* archived, size_t archived_cap) {
	if(archived && archived_cap)
		archived[0] = '\0';
	if(!rig_path || !rig_path[0])
		return VEHICLE_RIG_SLOT_REFUSED;
	struct stat st;
	if(stat(rig_path, &st) != 0)
		return VEHICLE_RIG_SLOT_FREE;
	/* Only an earlier run's rig DIRECTORY is moved; any other object holding
	 * the name is someone else's and is refused untouched. */
	if(!S_ISDIR(st.st_mode))
		return VEHICLE_RIG_SLOT_REFUSED;
	/* Archive, never delete (CONTRACT-v1 / Astra ruling 4 forbids the rig
	 * create from destroying a world). rename() of a directory within the same
	 * saves root is a single directory-entry move on libfat and on the host
	 * file systems; the first free _consumed_NNN keeps every earlier run. */
	char target[512];
	for(unsigned k = 0; k < VEHICLE_RIG_ARCHIVE_MAX; k++) {
		int n = snprintf(target, sizeof(target), "%s_consumed_%03u", rig_path, k);
		if(n < 0 || (size_t)n >= sizeof(target))
			return VEHICLE_RIG_SLOT_REFUSED;
		struct stat taken;
		if(stat(target, &taken) == 0)
			continue;
		if(rename(rig_path, target) != 0 || stat(rig_path, &taken) == 0)
			return VEHICLE_RIG_SLOT_REFUSED;
		if(archived && archived_cap)
			snprintf(archived, archived_cap, "%s", target);
		return VEHICLE_RIG_SLOT_ARCHIVED;
	}
	return VEHICLE_RIG_SLOT_REFUSED;
}

/* host_vehicle_smoke_8m's disposable rig world (CONTRACT-v1 / Astra ruling
 * 4). Stricter than the recovery entry above on purpose: it NEVER deletes.
 * The gate is vehicle_smoke_rig_create_allowed() -- false unless the vehicle
 * smoke driver is ARMED (an hw_run profile, never a config key) AND
 * `directory` is its single fixed constant -- and a directory that already
 * exists (an earlier, consumed run) is REFUSED, so archiving/resetting it is
 * the lead's explicit act outside the run. Everything after the gate is the
 * existing create machinery (sworld_mkdir + sworld_finish_create). */
/* Set ONLY by vehicle_smoke_arm (the armed host_vehicle_smoke_8m driver) and
 * cleared by its reset; a plain flag so this file references nothing in the
 * driver (the frontend test pools link it standalone). */
static bool s_vehicle_rig_authorized;
static bool s_offer_piston_rig_authorized;
void screen_sworld_split_offer_piston_authorize(bool on) {
	s_offer_piston_rig_authorized = on;
}

void screen_sworld_vehicle_rig_authorize(bool on) {
	s_vehicle_rig_authorized = on;
}

bool screen_sworld_create_vehicle_rig(const char* display_name,
									  const char* directory, bool seed_set,
									  int64_t seed) {
	if(!display_name || !directory
	   || !((s_vehicle_rig_authorized && strcmp(directory, VEHICLE_SMOKE_RIG_WORLD) == 0)
		   || (s_offer_piston_rig_authorized && strcmp(directory, SOP_RIG_WORLD) == 0)))
		return false;
	const char* saves_path
		= config_read_string(&gstate.config_user, "paths.worlds", "saves");
	if(!saves_path || !saves_path[0])
		return false;
	size_t base_length = strlen(saves_path);
	char* base = malloc(base_length + 1);
	if(!base)
		return false;
	memcpy(base, saves_path, base_length + 1);
	(void)sworld_trim_directory_path(base, base_length);
	if(!sworld_ensure_directory_tree(base)) {
		free(base);
		return false;
	}
	base_length = strlen(base);
	const char* join = sworld_is_separator(base[base_length - 1]) ? "" : "/";
	string_t dir;
	string_init_printf(dir, "%s%s%s", base, join, directory);
	free(base);
	/* O19: a consumed rig from an earlier run is moved aside (never deleted)
	 * by vehicle_smoke_rig_prepare_slot; anything it cannot safely move is
	 * still refused. */
	char archived[512];
	enum vehicle_rig_slot slot = vehicle_smoke_rig_prepare_slot(
		string_get_cstr(dir), archived, sizeof(archived));
	if(slot == VEHICLE_RIG_SLOT_REFUSED) {
		fprintf(stderr, "[world] vehicle rig world %s already exists and "
						"could not be archived -- refusing\n",
				string_get_cstr(dir));
		string_clear(dir);
		return false;
	}
	if(slot == VEHICLE_RIG_SLOT_ARCHIVED)
		fprintf(stderr, "[world] consumed vehicle rig world archived to %s\n",
				archived);
	if(sworld_mkdir(string_get_cstr(dir)) != 0) {
		fprintf(stderr, "[world] vehicle rig world %s could not be reserved\n",
				string_get_cstr(dir));
		string_clear(dir);
		return false;
	}
	fprintf(stderr, "[world] vehicle rig world reserved at %s\n",
			string_get_cstr(dir));
	bool ok = sworld_finish_create(dir, display_name, seed_set, seed, false);
	string_clear(dir);
	return ok;
}

bool screen_sworld_create_split_offer_piston_rig(const char* display,
	const char* directory, bool seed_set, int64_t seed) {
	if(!s_offer_piston_rig_authorized || !directory
	   || strcmp(directory, SOP_RIG_WORLD)) return false;
	return screen_sworld_create_vehicle_rig(display, directory, seed_set, seed);
}

/* The create-world dialog's entry [740c583 GuiCreateWorld.actionPerformed
 * id 0]: `folder_base` is the ALREADY-derived folder name (trimmed,
 * forbidden chars replaced, "World" fallback); the "-" dedupe happens here
 * inside the reservation. Returns true when the load request was sent (the
 * dialog hands off to screen_load_world). */
bool screen_sworld_create_from_dialog(const char* display_name,
									  const char* folder_base, bool seed_set,
									  int64_t seed, bool creative) {
	const char* saves_path
		= config_read_string(&gstate.config_user, "paths.worlds", "saves");

	string_t dir;
	string_init(dir);
	if(!sworld_reserve_beta_directory(saves_path, folder_base, dir)) {
		fprintf(stderr, "[world] could not reserve '%s' under %s\n",
				folder_base, saves_path);
		string_clear(dir);
		return false;
	}

	bool ok = sworld_finish_create(dir, display_name, seed_set, seed, creative);
	string_clear(dir);
	return ok;
}

/* Resolve up to `budget` PENDING rows. Returns the number of level_archive_create calls
   made -- the WORK-PER-CALL unit the tests assert on. Never assert on elapsed time: a PC
   SSD hides the SD cost this exists to bound. */
static size_t screen_sworld_resolve_step(size_t budget) {
	if(!worlds)
		return 0;
	size_t done = 0;
	ptime_t t0 = time_get();
	while(done < budget && resolve_cursor < screen_sworld_count()) {
		// POINTER into the backing array, NOT stack_at(). Safe here and only here: no
		// stack_push happens after reset() returns, so `data` cannot be reallocated under
		// us. stack_at() would hand back a COPY, and writing a resolved name into a copy of
		// an SSO string_t either loses it or aliases the heap pointer -- see the note on
		// struct world_option.
		struct world_option* e
			= (struct world_option*)worlds->data + resolve_cursor;
		resolve_cursor++;
		done++;
		archive_opens++;

		struct level_archive la;
		if(!level_archive_create(&la, e->path)) {
			// Do NOT level_archive_destroy() a failed create: it already cleared
			// file_name and left data NULL, and destroy asserts on data -- which the
			// suite's forced -UNDEBUG would fire.
			e->state = WORLD_ROW_UNREADABLE;
			continue;
		}
		char name[64];
		// string_set_str, NOT string_init_set_str: the string is already initialised from
		// the directory name, and re-initialising would drop its heap pointer unfreed.
		if(level_archive_read(&la, LEVEL_NAME, name, sizeof(name)) && name[0])
			string_set_str(e->name, name);
		/* Free with the level.dat this row already opens -- no extra archive
		 * open, so the WORK-PER-CALL budget above is unchanged. */
		e->creative = level_archive_is_creative(&la);
		e->needs_convert = import_convert_notice_needed(&la);
		if(!level_archive_read(&la, LEVEL_DISK_SIZE, &e->byte_size, 0))
			e->byte_size = 0;
		if(!level_archive_read(&la, LEVEL_LAST_PLAYED, &e->last_access, 0))
			e->last_access = 0;
		e->last_access /= 1000;
		level_archive_destroy(&la);
		e->state = WORLD_ROW_OK;
	}
	if(done) {
		int32_t ms = time_diff_ms(t0, time_get());
		if(ms > 0 && (uint32_t)ms > resolve_step_max_ms)
			resolve_step_max_ms = (uint32_t)ms;
	}
	return done;
}

// Rows still waiting on their level.dat, and the worst single resolve step this boot.
// Surfaced in the hang report so a card that has merely gone SLOW (rather than wedged) is
// visible: after this split only a single open over ~1.5s can still trip the detector, so
// everything between "fast" and "wedged" would otherwise be a silent crawl.
size_t world_list_pending_rows(void) {
	if(!worlds)
		return 0;
	size_t n = screen_sworld_count();
	return resolve_cursor < n ? n - resolve_cursor : 0;
}
uint32_t world_list_resolve_max_ms(void) { return resolve_step_max_ms; }

// Selection arithmetic, factored out so the empty-list boundary is table-testable.
// `count - 1` on a size_t is SIZE_MAX when the list is empty, which made DOWN always
// increment; the list being fully built inside reset() kept that latent, and this keeps it
// latent now that rows appear over several frames.
static size_t sworld_next_selection(size_t cur, size_t count, bool up, bool down) {
	if(count == 0)
		return 0;
	if(up && cur > 0)
		cur--;
	if(down && cur + 1 < count)
		cur++;
	if(cur >= count)
		cur = count - 1;
	return cur;
}

/* THE per-frame resolve the main loop performs. Extracted so a test can drive the SAME
   budget path the game does: a test that only called screen_sworld_resolve_step() directly
   would still pass if update() resolved the WHOLE list in one frame -- which is precisely
   the defect. K is NOT a Beta number (Beta enumerates eagerly); it is a WATCHDOG-SAFETY
   number, since one frame's opens must stay under the detector's 3000ms no-progress
   threshold. At K=2 that tolerates ~1500ms per open before a single frame could fire. */
static size_t screen_sworld_resolve_budgeted(void) {
	int k = config_read_int(&gstate.config_user, "gui.world_list_resolve_per_frame", 2);
	if(k < 1)
		k = 1;
	if(k > SWORLD_RESOLVE_PER_FRAME_MAX)
		k = SWORLD_RESOLVE_PER_FRAME_MAX;
	return screen_sworld_resolve_step((size_t)k);
}

/* THE APPROVED LOAD TAB (design-wiring/load-green; LOAD-GREEN-BRIEF.md,
 * docs/ui_design/approved/native-play-flow.md). Select World IS the Load tab
 * of the retained Play session (play_session.h). Composed on a 640x480 canvas
 * scaled by height/480 and centred horizontally, like the title and the pack
 * screen, in PHYSICAL pixels:
 *   title "Play game"       88,14  26 px
 *   tabs Load/Create/Join   88+156*i, 50  152x46
 *   paper panel             88,94  464x214 ; four 46 px rows at 100,110+48*i 440 wide
 *   Prev / Next             100,310 / 208,310  44x44
 *   Play                    100,366 276x44 ; World options 392,366 148x44
 *   empty list: "Create a world" 100,242 440x44 in the panel (the Play slot)
 *   enumeration error: "Retry" 208,222 224x44 in the panel (the Play slot)
 *   footer y 432 h 36: Move / Select / Back from the detected bindings
 * Beta's five-button strip [740c583 GuiSelectWorld.initButtons] is replaced by
 * the approved composition; Rename and Delete live in the local "World
 * options" modal (Rename / Delete / Back, opening on Back), Create is the
 * Create tab, Cancel is the footer Back (Close / HOME). The TRANSACTIONS are
 * unchanged: sworld_play_selected, the rename screen, the delete confirm.
 *
 * ONE id space for everything the screen can activate (Astra ruling 2), the
 * SAME ids for the pointer hit test and the D-pad ring: enum sworld_button
 * (declared near the top of this file, with the modal rows, the state and
 * the canvas mapping, because the reset seeds them). The World options
 * modal's rows are Rename / Delete / Back, visual order; it opens on Back.
 *
 * Canvas numbers (ratified by root, 2026-09-23). */
#define SW_TITLE_X 88
#define SW_TITLE_Y 14
#define SW_TITLE_FONT 26
#define SW_TAB_X0 88
#define SW_TAB_PITCH 156
#define SW_TAB_Y 50
#define SW_TAB_W 152
#define SW_TAB_H 46
#define SW_PANEL_X 88
#define SW_PANEL_Y 94
#define SW_PANEL_W 464
#define SW_PANEL_H 214
#define SW_ROW_X 100
#define SW_ROW_W 440
#define SW_ROW_H 46
#define SW_PREV_X 100
#define SW_NEXT_X 208
#define SW_PAGE_Y 310
#define SW_PAGE_S 44
#define SW_PLAY_X 100
#define SW_ACTION_Y 366
#define SW_PLAY_W 276
#define SW_OPTIONS_X 392
#define SW_OPTIONS_W 148
#define SW_ACTION_H 44
#define SW_EMPTY_X 100
#define SW_EMPTY_Y 242
#define SW_EMPTY_W 440
#define SW_RETRY_X 208
#define SW_RETRY_Y 222
#define SW_RETRY_W 224
#define SW_FOOTER_Y 432
#define SW_FOOTER_H 36
#define SW_FOOTER_GAP 24
#define SW_MODAL_W 280
#define SW_MODAL_ROW_H 44
#define SW_MODAL_PAD 12

/* The Load state that is not the list (declared at the top): focus_button =
 * one of the ids above or -1 ("on the selected row"); list_error = the
 * enumeration FAILED (missing / unreadable saves directory, or the list
 * owner allocation); modal_open / modal_row = the local World options modal,
 * whose modal_path / modal_name capture the row it acts on even if the list
 * re-sorts under it (same 512-byte bound as sworld_delete_path);
 * sworld_predict = the inputs the NEXT activate press is decided against,
 * captured at the end of update like the title's title_predict. */

static bool sworld_list_empty(void) { return screen_sworld_count() == 0; }

/* The ONE rect per control -- what render2D paints and what the pointer is
 * tested against. The Play slot is Retry while the enumeration failed and
 * "Create a world" while the list is empty. */
static struct fep_rect sworld_button_rect_fep(int width, int height, int button) {
	struct sworld_canvas c = sworld_canvas_of(width, height);
	switch(button) {
		case SWORLD_BUTTON_TAB_LOAD:
		case SWORLD_BUTTON_TAB_CREATE:
		case SWORLD_BUTTON_TAB_JOIN:
			return fep_rect(sw_x(&c, SW_TAB_X0 + SW_TAB_PITCH * button),
							sw_d(&c, SW_TAB_Y), sw_d(&c, SW_TAB_W),
							sw_d(&c, SW_TAB_H));
		case SWORLD_BUTTON_PREV:
			return fep_rect(sw_x(&c, SW_PREV_X), sw_d(&c, SW_PAGE_Y),
							sw_d(&c, SW_PAGE_S), sw_d(&c, SW_PAGE_S));
		case SWORLD_BUTTON_NEXT:
			return fep_rect(sw_x(&c, SW_NEXT_X), sw_d(&c, SW_PAGE_Y),
							sw_d(&c, SW_PAGE_S), sw_d(&c, SW_PAGE_S));
		case SWORLD_BUTTON_PLAY:
			if(list_error)
				return fep_rect(sw_x(&c, SW_RETRY_X), sw_d(&c, SW_RETRY_Y),
								sw_d(&c, SW_RETRY_W), sw_d(&c, SW_ACTION_H));
			if(sworld_list_empty())
				return fep_rect(sw_x(&c, SW_EMPTY_X), sw_d(&c, SW_EMPTY_Y),
								sw_d(&c, SW_EMPTY_W), sw_d(&c, SW_ACTION_H));
			return fep_rect(sw_x(&c, SW_PLAY_X), sw_d(&c, SW_ACTION_Y),
							sw_d(&c, SW_PLAY_W), sw_d(&c, SW_ACTION_H));
		default:
			return fep_rect(sw_x(&c, SW_OPTIONS_X), sw_d(&c, SW_ACTION_Y),
							sw_d(&c, SW_OPTIONS_W), sw_d(&c, SW_ACTION_H));
	}
}

/* Row `row`'s drawn rect: the panel's row band, minus the slotlist's scroll
 * (widget_slotlist stays the scroll / selection MODEL; its 220-unit row
 * drawing and hit band are bypassed on this screen only, like the pack
 * screen). */
static struct fep_rect sworld_row_rect(int width, int height, size_t row) {
	struct sworld_canvas c = sworld_canvas_of(width, height);
	/* Off the shown page: an EMPTY rect, which fep_rect_hit refuses and the
	 * render skips -- the row is neither drawn nor pressable. */
	if(row < page_first || row - page_first >= SW_ROWS_VISIBLE)
		return fep_rect(0, 0, 0, 0);
	int local = (int)(row - page_first); /* 0..3 by construction */
	return fep_rect(sw_x(&c, SW_ROW_X), sw_d(&c, SW_ROW_Y0 + SW_ROW_PITCH * local),
					sw_d(&c, SW_ROW_W), sw_d(&c, SW_ROW_H));
}

/* Paging helpers: pages of SW_ROWS_VISIBLE, count ceil(n / 4); Prev / Next
 * exist only above one page. */
static bool sworld_paging(void) {
	return screen_sworld_count() > SW_ROWS_VISIBLE;
}
static size_t sworld_page_count(void) {
	size_t n = screen_sworld_count();
	return n == 0 ? 1 : (n + SW_ROWS_VISIBLE - 1) / SW_ROWS_VISIBLE;
}
/* Show the page the selected row is on (the selection-reveal rule). */
static void sworld_reveal_selection(void) {
	if(gui_selection < screen_sworld_count())
		page_first = gui_selection / SW_ROWS_VISIBLE * SW_ROWS_VISIBLE;
	else
		page_first = 0;
}
/* Step a full page; the selection follows onto the new page's first row when
 * it would otherwise be off-screen, so the focused row is always revealed. */
static void sworld_page_step(int dir);

static struct fep_rect sworld_panel_rect(int width, int height) {
	struct sworld_canvas c = sworld_canvas_of(width, height);
	return fep_rect(sw_x(&c, SW_PANEL_X), sw_d(&c, SW_PANEL_Y),
					sw_d(&c, SW_PANEL_W), sw_d(&c, SW_PANEL_H));
}

static const struct world_option* sworld_selected_row(void) {
	if(!worlds || gui_selection >= screen_sworld_count())
		return NULL;
	return (const struct world_option*)worlds->data + gui_selection;
}

/* Remember the selected world's FULL directory in the session; a refused
 * remember (allocation failure) keeps the prior identity, disclosed. */
/* A VISIBLE status line (drawn in the panel band beside Prev / Next, and
 * exported through screen_sworld_test_hint): the player-facing disclosure of
 * a refused management path or a refused remember. Cleared on every reset. */
static bool sworld_remember_selected(void) {
	const struct world_option* e = sworld_selected_row();
	if(!e)
		return false;
	if(!play_session_remember_world(string_get_cstr(e->directory))) {
		fprintf(stderr, "[world-list] could not remember \"%s\" (allocation)\n",
				string_get_cstr(e->directory));
		snprintf(sworld_status, sizeof(sworld_status),
				 "Couldn't remember the selection (out of memory)");
		return false;
	}
	return true;
}

static bool sworld_button_enabled(int button) {
	const struct world_option* e = sworld_selected_row();
	switch(button) {
		case SWORLD_BUTTON_TAB_LOAD:
		case SWORLD_BUTTON_TAB_CREATE:
		case SWORLD_BUTTON_TAB_JOIN: return true;
		case SWORLD_BUTTON_PREV:
			return !list_error && sworld_paging() && page_first > 0;
		case SWORLD_BUTTON_NEXT:
			return !list_error && sworld_paging()
				&& page_first + SW_ROWS_VISIBLE < screen_sworld_count();
		case SWORLD_BUTTON_PLAY:
			if(list_error || sworld_list_empty())
				return true; /* Retry / Create a world */
			/* INTERACTIVE Play needs a RESOLVED, readable row: a pending row
			 * is not offered until its level.dat has been read (the autoload
			 * path above keeps its own pending-is-loadable rule, unchanged),
			 * and an unreadable one is never offered. */
			return e && e->state == WORLD_ROW_OK;
		case SWORLD_BUTTON_OPTIONS:
			/* ANY enumerated world can be managed (root correction,
			 * load-fix1): a corrupt save must remain deletable through the
			 * confirmation, so the modal opens for unreadable and pending rows
			 * too. Rename alone waits for a readable row (the modal gates it). */
			return e != NULL;
		default: return false;
	}
}

/* Arm the CLIENT half of creative for the world about to be loaded
 * (gstate.config_creative: instant dig + the item picker). The SERVER reads
 * the same LEVEL_GAME_MODE tag itself when it opens level.dat -- there is
 * deliberately NO CRPC for the mode: a local world's mode is IMMUTABLE (fixed
 * at creation, design section 8 Q5), the client and the local server share one
 * process, and the client already opens every world's level.dat for this
 * list. If the mode ever becomes changeable at runtime this MUST become a
 * CRPC instead of a second reader.
 *
 * A resolved row already knows; only a PENDING row (still loadable -- the
 * server needs just the path) pays one archive open, on an explicit load
 * action. Always assigns, so a creative world can never leak its mode into
 * the next survival world loaded in the same session. */
static void sworld_arm_client_creative(const struct world_option* e,
									   bool* needs_convert) {
	gstate.config_creative = false;
	/* convert-notice-1024: `needs_convert` (NULL on the autoload path, which
	 * never shows the notice) is the Beta-import predicate. A resolved row
	 * has it cached from its resolve open (no I/O here, as before); only a
	 * PENDING row pays its one open, shared with the creative probe. */
	if(needs_convert)
		*needs_convert = false;
	if(e->state == WORLD_ROW_OK) {
		gstate.config_creative = e->creative;
		if(needs_convert)
			*needs_convert = e->needs_convert;
		return;
	}
	play_archive_opens++;
	/* An owned copy: level_archive_create takes a mutable string_t and the
	 * row is const here. */
	string_t probe;
	string_init_set(probe, e->path);
	struct level_archive la;
	if(level_archive_create(&la, probe)) {
		gstate.config_creative = level_archive_is_creative(&la);
		if(needs_convert)
			*needs_convert = import_convert_notice_needed(&la);
		level_archive_destroy(&la);
	}
	/* An unreadable archive stays survival and shows no notice; the load
	 * fails on its own. */
	string_clear(probe);
}

/* The world load a Play issues, unchanged: begin the loading reason, then the
 * asyncsnd-0926 world-entry gate (loading_notice.h): sent now, or parked
 * behind the sound gate until the async registry is complete. Returns false
 * when nothing was sent or parked. */
static bool sworld_begin_load(const char* path) {
	loading_reason_begin(0, LOAD_SAVE);
	if(!loading_notice_gate_load_world(path, &screen_select_world)) {
		loading_reason_end(0);
		return false;
	}
	return true;
}

static void sworld_play_selected(void) {
	if(gui_selection >= screen_sworld_count())
		return;
	const struct world_option* e
		= (const struct world_option*)worlds->data + gui_selection;
	/* A PENDING row is loadable because the server needs only its path.
	 * Preserve the incremental-list refusal for an UNREADABLE row. */
	if(e->state == WORLD_ROW_UNREADABLE)
		return;
	bool needs_convert = false;
	sworld_arm_client_creative(e, &needs_convert);
	if(needs_convert) {
		/* ui-build-1026/T09: a Minecraft Beta world first asks "Back up this
		 * world?" (Cancel focused). Nothing touches the disk until Back up is
		 * chosen; the screen then runs this same unchanged load. */
		const char* path = string_get_cstr(e->path);
		if(!screen_convert_backup_setup(path, string_get_cstr(e->name),
										sworld_begin_load)) {
			fprintf(stderr, "[world-list] world path too long to load: %s\n",
					path);
			snprintf(sworld_status, sizeof(sworld_status),
					 "This world's folder path is too long");
			return;
		}
		screen_set(&screen_convert_backup);
		return;
	}
	(void)sworld_begin_load(string_get_cstr(e->path));
}

/* Delete the world the MODAL captured (its identity is stable even if the
 * list re-sorts under it). The 512-byte path bound is checked at capture. */
static void sworld_delete_selected(void) {
	snprintf(sworld_delete_path, sizeof(sworld_delete_path), "%s", modal_path);
	/* [740c583 GuiSelectWorld.actionPerformed id 2]: deleteQuestion,
	 * "'<name>' " + deleteWarning, deleteButton, gui.cancel. */
	char warn[256];
	snprintf(warn, sizeof(warn), "'%s' will be lost forever! (A long time!)",
			 modal_name);
	/* YES erases the directory, so Cancel is the answer that changes
	 * nothing. */
	screen_confirm_setup("Are you sure you want to delete this world?", warn,
					 "Delete", "Cancel", SCREEN_CONFIRM_ANSWER_NO,
					 sworld_delete_confirmed);
	screen_set(&screen_confirm);
}

/* Open the World options modal on the SELECTED row: capture its identity
 * (path + name) now, so a re-sort under the modal cannot re-aim Rename or
 * Delete, and REFUSE an overlong path before any dispatch could truncate it
 * into the 512-byte management buffers (fail closed, disclosed). */
/* The modal's own canonical-activation state (the same arbitration every
 * screen uses, on the modal's three rows): who chose the highlight, the
 * pointer-movement tracker, and whether Rename is offered for the captured
 * row (readable rows only; Delete + Back are always offered). */
static enum gutil_menu_origin modal_origin;
static struct gutil_menu_focus modal_pfocus;
static bool modal_can_rename;

static void sworld_open_options(void) {
	const struct world_option* e = sworld_selected_row();
	if(!e)
		return;
	const char* path = string_get_cstr(e->path);
	if(strlen(path) >= sizeof(modal_path)) {
		/* Refused BEFORE any dispatch could truncate it into the 512-byte
		 * management buffers; disclosed on screen, not only on stderr. */
		fprintf(stderr,
				"[world-list] path too long to manage (%zu >= %zu): \"%s\"\n",
				strlen(path), sizeof(modal_path), path);
		snprintf(sworld_status, sizeof(sworld_status),
				 "This world's folder path is too long to manage");
		return;
	}
	snprintf(modal_path, sizeof(modal_path), "%s", path);
	snprintf(modal_name, sizeof(modal_name), "%s", string_get_cstr(e->name));
	modal_can_rename = e->state == WORLD_ROW_OK;
	modal_open = true;
	modal_row = SWORLD_MODAL_BACK; /* the safe initial answer */
	modal_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	modal_pfocus = (struct gutil_menu_focus) {0};
}

static bool sworld_modal_row_enabled(int row) {
	if(row == SWORLD_MODAL_RENAME)
		return modal_can_rename;
	return row == SWORLD_MODAL_DELETE || row == SWORLD_MODAL_BACK;
}

/* The modal card and its rows: ONE rect function for draw and hit. */
/* The approved World options modal (console-revision.js, proofs/
 * beta--worlds--options.png): panel 80,120 480x252; heading 104,138 w432
 * 24 px; the captured world name 104,178 w432 20 px; its mode / date centred
 * at y 210; Rename 104,252 208x44; Delete world 328,252 208x44; Back 328,308
 * 208x44. ONE rect per row for hit and draw. */
#define SW_MODAL_X 80
#define SW_MODAL_Y 120
#define SW_MODAL_PANEL_W 480
#define SW_MODAL_PANEL_H 252
#define SW_MODAL_HEAD_X 104
#define SW_MODAL_HEAD_Y 138
#define SW_MODAL_TEXT_W 432
#define SW_MODAL_HEAD_FONT 24
#define SW_MODAL_NAME_Y 178
#define SW_MODAL_NAME_FONT 20
#define SW_MODAL_META_Y 210
#define SW_MODAL_META_FONT 13
#define SW_MODAL_BTN_W 208
#define SW_MODAL_BTN_H 44
static struct fep_rect sworld_modal_card_rect(int width, int height) {
	struct sworld_canvas c = sworld_canvas_of(width, height);
	return fep_rect(sw_x(&c, SW_MODAL_X), sw_d(&c, SW_MODAL_Y),
					sw_d(&c, SW_MODAL_PANEL_W), sw_d(&c, SW_MODAL_PANEL_H));
}
static struct fep_rect sworld_modal_row_rect(int width, int height, int row) {
	struct sworld_canvas c = sworld_canvas_of(width, height);
	int x = row == SWORLD_MODAL_RENAME ? 104 : 328;
	int y = row == SWORLD_MODAL_BACK ? 308 : 252;
	return fep_rect(sw_x(&c, x), sw_d(&c, y), sw_d(&c, SW_MODAL_BTN_W),
					sw_d(&c, SW_MODAL_BTN_H));
}

/* Switch to a sibling tab: the selection is remembered FIRST (the hand-off
 * rule), the session's tab moves, then the existing screen opens. The Create
 * and Join screens receive their own slices; nothing is stubbed here. */
static void sworld_switch_tab(int button) {
	sworld_remember_selected();
	if(button == SWORLD_BUTTON_TAB_CREATE) {
		play_session_set_tab(PLAY_TAB_CREATE);
		screen_set(&screen_create_world);
	} else if(button == SWORLD_BUTTON_TAB_JOIN) {
		play_session_set_tab(PLAY_TAB_JOIN);
		if(screen_multiplayer_setup)
			screen_set(screen_multiplayer_setup);
	}
}

static void screen_sworld_reset(struct screen* s, int width, int height);

static void sworld_activate_button(int button) {
	if(button < 0 || button >= SWORLD_BUTTON_COUNT
	   || !sworld_button_enabled(button))
		return;
	/* The ACTIVE Load tab is not an action. */
	if(button == SWORLD_BUTTON_TAB_LOAD)
		return;
	/* [740c583 GuiScreen.mouseClicked]: enabled-button click sound before
	 * actionPerformed. */
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	switch(button) {
		case SWORLD_BUTTON_TAB_CREATE:
		case SWORLD_BUTTON_TAB_JOIN: sworld_switch_tab(button); break;
		case SWORLD_BUTTON_PREV: sworld_page_step(-1); break;
		case SWORLD_BUTTON_NEXT: sworld_page_step(1); break;
		case SWORLD_BUTTON_PLAY:
			if(list_error) {
				/* Retry: the REAL enumeration again, in place, and the ring
				 * stays on this slot so a second Retry is one more press. */
				screen_sworld_reset(&screen_select_world, gfx_width(),
									gfx_height());
				focus_button = SWORLD_BUTTON_PLAY;
			} else if(sworld_list_empty()) {
				sworld_switch_tab(SWORLD_BUTTON_TAB_CREATE); /* Create a world */
			} else {
				sworld_remember_selected();
				sworld_play_selected();
			}
			break;
		case SWORLD_BUTTON_OPTIONS: sworld_open_options(); break;
		default: break;
	}
}

static int sworld_pointer_button(int width, int height) {
	if(!pointer_available)
		return -1;
	for(int button = 0; button < SWORLD_BUTTON_COUNT; button++) {
		/* Prev / Next / options are absent while the panel shows Retry or
		 * "Create a world"; only the drawn controls are hit-tested. */
		if(!sworld_button_enabled(button) && button != SWORLD_BUTTON_OPTIONS
		   && button != SWORLD_BUTTON_PLAY)
			continue;
		if((list_error || sworld_list_empty())
		   && (button == SWORLD_BUTTON_PREV || button == SWORLD_BUTTON_NEXT
			   || button == SWORLD_BUTTON_OPTIONS))
			continue;
		struct fep_rect r = sworld_button_rect_fep(width, height, button);
		if(fep_rect_hit(r, pointer_x, pointer_y))
			return button;
	}
	return -1;
}

/* ONE id space for everything this screen can activate (Astra ruling 2): the
 * seven controls keep their enum values, a list ROW is SWORLD_BUTTON_COUNT +
 * its index. */
#define SWORLD_TARGET_ROW(i) (SWORLD_BUTTON_COUNT + (int)(i))

static bool sworld_target_is_row(int target) {
	return target >= SWORLD_BUTTON_COUNT;
}

/* The target the DIRECTIONAL ring rests on: a control while focus_button is
 * set, otherwise the selected ROW -- NONE on an empty list. The RENDER reads
 * this same function (A3 NEW-7). */
static int sworld_focus_target(void) {
	if(focus_button >= 0)
		return focus_button;
	return gui_selection < screen_sworld_count()
		? SWORLD_TARGET_ROW(gui_selection)
		: GUTIL_MENU_TARGET_NONE;
}

static bool sworld_target_enabled(int target) {
	if(target == GUTIL_MENU_TARGET_NONE)
		return false;
	if(sworld_target_is_row(target))
		return (size_t)(target - SWORLD_BUTTON_COUNT) < screen_sworld_count();
	return sworld_button_enabled(target);
}

/* The row under the sprite, through THIS screen's row rects and only inside
 * the panel's list band. */
/* The LIST BAND the rows are drawn (scissored) into: canvas y 110..300 --
 * the rows' own band, NOT the paper panel (94..308). A row scrolled partly
 * out of it is clipped in the render, so it must be clipped in the hit test
 * by the SAME rect, or a press at y 100 after one Next would select a row the
 * player cannot see. */
static struct fep_rect sworld_list_band(int width, int height) {
	struct sworld_canvas c = sworld_canvas_of(width, height);
	int top = sw_d(&c, SW_ROW_Y0);
	int bottom = sw_d(&c, SW_PANEL_Y + SW_PANEL_H - 8);
	return fep_rect(sw_x(&c, SW_PANEL_X), top, sw_d(&c, SW_PANEL_W), bottom - top);
}

static int sworld_row_at(int width, int height, float x, float y) {
	struct fep_rect band = sworld_list_band(width, height);
	if(!fep_rect_hit(band, x, y))
		return -1;
	for(size_t row = 0; row < screen_sworld_count(); row++)
		if(fep_rect_hit(sworld_row_rect(width, height, row), x, y))
			return (int)row;
	return -1;
}

/* The target the SPRITE rests on: a control, else the row under it. */
static int sworld_pointer_target(void) {
	if(!pointer_available)
		return GUTIL_MENU_TARGET_NONE;
	if(pointer_button >= 0)
		return pointer_button;
	if(list_error || sworld_list_empty())
		return GUTIL_MENU_TARGET_NONE;
	int row = sworld_row_at(gfx_width(), gfx_height(), pointer_x, pointer_y);
	return row < 0 ? GUTIL_MENU_TARGET_NONE : SWORLD_TARGET_ROW(row);
}

/* A ROW press SELECTS it and hands focus to the explicit Play button -- it
 * never launches (Load contract). Beta's row-click-to-open [740c583
 * GuiWorldSlot.elementClicked] becomes the two-step select + Play. */
static void sworld_activate_target(int target) {
	if(sworld_target_is_row(target)) {
		gui_selection = (size_t)(target - SWORLD_BUTTON_COUNT);
		slotlist_select(&world_slots, gui_selection, true);
		sworld_remember_selected();
		focus_button = SWORLD_BUTTON_PLAY;
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		return;
	}
	sworld_activate_button(target);
}

/* Where a FOCUS_ONLY press puts the ring. */
static void sworld_focus_target_set(int target) {
	if(sworld_target_is_row(target)) {
		focus_button = -1;
		gui_selection = (size_t)(target - SWORLD_BUTTON_COUNT);
		slotlist_select(&world_slots, gui_selection, true);
		sworld_reveal_selection();
		sworld_remember_selected();
		return;
	}
	if(target >= 0 && target < SWORLD_BUTTON_COUNT)
		focus_button = target;
}

/* Directional navigation across the composition (one step, already knowing
 * exactly one of up/down/left/right is pressed):
 *   header tabs: LEFT/RIGHT wrap through Load / Create / Join; DOWN -> the
 *     selected row, or the Play slot (Create a world / Retry) with no rows.
 *   rows: UP from the first -> the Load tab; UP/DOWN otherwise move the
 *     selection; DOWN from the last -> Prev.
 *   Prev / Next: LEFT/RIGHT swap; UP -> the selected row; DOWN -> Play.
 *   Play / World options: LEFT/RIGHT swap (a disabled options is skipped);
 *     UP -> Prev (or the selected row / header when there is no page row). */
/* The page control the ring lands on from the rows / the action row: the
 * first ENABLED one (Next before Prev, the direction a reader expects), or
 * -1 when the list is one page (no page controls exist). */
static int sworld_page_control(void) {
	if(!sworld_paging())
		return -1;
	if(sworld_button_enabled(SWORLD_BUTTON_NEXT))
		return SWORLD_BUTTON_NEXT;
	if(sworld_button_enabled(SWORLD_BUTTON_PREV))
		return SWORLD_BUTTON_PREV;
	return -1;
}

static void sworld_page_step(int dir) {
	if(!sworld_paging())
		return;
	size_t pages = sworld_page_count();
	size_t page = page_first / SW_ROWS_VISIBLE;
	if(dir < 0 && page == 0)
		return;
	if(dir > 0 && page + 1 >= pages)
		return;
	page = dir < 0 ? page - 1 : page + 1;
	page_first = page * SW_ROWS_VISIBLE;
	if(gui_selection < page_first || gui_selection - page_first >= SW_ROWS_VISIBLE) {
		gui_selection = page_first;
		slotlist_select(&world_slots, gui_selection, true);
		sworld_remember_selected();
	}
	/* A boundary arrow that just became disabled hands the ring to the other
	 * one (or Play), so focus never rests on a dead control. */
	if(focus_button >= 0 && !sworld_button_enabled(focus_button)) {
		int pc = sworld_page_control();
		focus_button = pc >= 0 ? pc : SWORLD_BUTTON_PLAY;
	}
}

static void sworld_navigate(bool up, bool down, bool left, bool right) {
	bool rows = !list_error && !sworld_list_empty();
	if(focus_button < 0) {
		if(!rows) {
			focus_button = SWORLD_BUTTON_TAB_LOAD;
			return;
		}
		/* The action entry acts on the selected world, even in a long list. */
		if(right) {
			focus_button = sworld_button_enabled(SWORLD_BUTTON_PLAY)
				? SWORLD_BUTTON_PLAY : SWORLD_BUTTON_OPTIONS;
			sworld_reveal_selection();
			return;
		}
		if(up && gui_selection == 0) {
			focus_button = SWORLD_BUTTON_TAB_LOAD;
			return;
		}
		size_t next = sworld_next_selection(gui_selection, screen_sworld_count(),
											up, down);
		if(next != gui_selection) {
			gui_selection = next;
			slotlist_select(&world_slots, gui_selection, true);
			sworld_reveal_selection();
			sworld_remember_selected();
		} else if(down) {
			/* DOWN off the last row: an enabled page control when paging,
			 * else Play. */
			int pc = sworld_page_control();
			focus_button = pc >= 0 ? pc : SWORLD_BUTTON_PLAY;
		}
		return;
	}
	switch(focus_button) {
		case SWORLD_BUTTON_TAB_LOAD:
		case SWORLD_BUTTON_TAB_CREATE:
		case SWORLD_BUTTON_TAB_JOIN:
			if(left)
				focus_button = focus_button == SWORLD_BUTTON_TAB_LOAD
					? SWORLD_BUTTON_TAB_JOIN
					: focus_button - 1;
			else if(right)
				focus_button = focus_button == SWORLD_BUTTON_TAB_JOIN
					? SWORLD_BUTTON_TAB_LOAD
					: focus_button + 1;
			else if(down) {
				if(rows) {
					focus_button = -1;
					slotlist_select(&world_slots, gui_selection, true);
				} else
					focus_button = SWORLD_BUTTON_PLAY;
			}
			break;
		case SWORLD_BUTTON_PREV:
		case SWORLD_BUTTON_NEXT:
			if(right && sworld_button_enabled(SWORLD_BUTTON_NEXT))
				focus_button = SWORLD_BUTTON_NEXT;
			else if(left && sworld_button_enabled(SWORLD_BUTTON_PREV))
				focus_button = SWORLD_BUTTON_PREV;
			else if(up) {
				focus_button = -1;
				sworld_reveal_selection();
				slotlist_select(&world_slots, gui_selection, true);
			} else if(down)
				focus_button = SWORLD_BUTTON_PLAY;
			break;
		case SWORLD_BUTTON_PLAY:
		case SWORLD_BUTTON_OPTIONS:
			if(right && rows && sworld_button_enabled(SWORLD_BUTTON_OPTIONS))
				focus_button = SWORLD_BUTTON_OPTIONS;
			else if(left)
				focus_button = SWORLD_BUTTON_PLAY;
			else if(down)
				focus_button = SWORLD_BUTTON_TAB_LOAD;
			else if(up) {
				if(!rows)
					focus_button = SWORLD_BUTTON_TAB_LOAD;
				else {
					/* UP from the action row: a page control when paging,
					 * else the selected row. */
					int pc = sworld_page_control();
					if(pc >= 0)
						focus_button = pc;
					else {
						focus_button = -1;
						sworld_reveal_selection();
					}
				}
			}
			break;
		default: break;
	}
}

/* THE MODAL owns the whole frame while it is up: UP/DOWN move its ring, A
 * acts, Close / HOME close it, the pointer is ignored, and nothing reaches
 * the list, the tabs or the actions. */
static void sworld_modal_activate(int row) {
	if(!sworld_modal_row_enabled(row))
		return;
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	modal_open = false;
	switch(row) {
		case SWORLD_MODAL_RENAME:
			screen_rename_world_setup(modal_path, modal_name);
			screen_set(&screen_rename_world);
			break;
		case SWORLD_MODAL_DELETE: sworld_delete_selected(); break;
		default: break; /* Back: nothing changes */
	}
}

/* THE MODAL runs the CANONICAL activation rule on its three rows -- the same
 * gutil_menu_pointer_focus / gutil_menu_activate_decide every screen uses --
 * so navigation + activate on one frame is consumed (UP + A from Back moves
 * to Delete and opens NOTHING), a held button supplies no second press, a
 * pointer over another row is a focus-only first, and a press over empty
 * space (the list BEHIND the card) is refused. Every path returns here: no
 * input reaches the list, the tabs or the actions while the card is up. */
static void sworld_modal_update(void) {
	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);
	bool gui_up = input_pressed(IB_GUI_UP), gui_down = input_pressed(IB_GUI_DOWN);
	bool navigating = gui_up || gui_down;
	bool pointer_owner = gutil_menu_pointer_focus(&modal_pfocus, pointer_available,
												  pointer_x, pointer_y, navigating);
	int hovered = GUTIL_MENU_TARGET_NONE;
	if(pointer_available)
		for(int k = 0; k < SWORLD_MODAL_COUNT; k++)
			if(fep_rect_hit(sworld_modal_row_rect(gfx_width(), gfx_height(), k),
							pointer_x, pointer_y))
				hovered = k;
	bool hovered_enabled = hovered != GUTIL_MENU_TARGET_NONE
		&& sworld_modal_row_enabled(hovered);
	if(pointer_owner && hovered_enabled) {
		modal_row = hovered;
		modal_origin = GUTIL_MENU_ORIGIN_POINTER;
	}
	if(navigating) {
		/* Step to the next ENABLED row, clamped; a disabled Rename is skipped. */
		int dir = gui_down ? 1 : -1;
		int k = modal_row;
		do
			k += dir;
		while(k >= 0 && k < SWORLD_MODAL_COUNT && !sworld_modal_row_enabled(k));
		if(k >= 0 && k < SWORLD_MODAL_COUNT)
			modal_row = k;
		modal_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	}
	bool click_pressed = input_pressed(IB_GUI_CLICK);
	struct gutil_menu_decision decision
		= gutil_menu_activate_decide((struct gutil_menu_activate_state) {
			.pressed = click_pressed,
			.navigated = navigating,
			.pointer_valid = pointer_available,
			.selection_origin = modal_origin,
			.hovered = hovered,
			.hovered_enabled = hovered_enabled,
			.selected = modal_row,
			.selected_enabled = sworld_modal_row_enabled(modal_row),
			/* Delete opens a CONFIRMATION; the destructive answer lives there. */
			.selected_destructive = false,
		});
	if(decision.action == GUTIL_MENU_FOCUS_ONLY)
		modal_row = decision.target;
	modal_origin = decision.selection_origin;
	if(decision.action == GUTIL_MENU_ACTIVATE) {
		sworld_modal_activate(decision.target);
		return;
	}
	if(click_pressed)
		return; /* consumed: a click behind the card activates nothing */
	if(input_pressed(IB_INVENTORY) || input_pressed(IB_HOME))
		modal_open = false; /* Close = Back */
}

/* THE CONTEXTUAL NAVIGATION HINT (Astra ruling 6, push-0921 A19). One pure
 * function, read by BOTH the render and the test hook, so what is drawn and
 * what is asserted cannot drift (the same rule sworld_focus_target follows).
 *
 * NULL = draw nothing, in exactly two cases:
 *   * the POINTER owns the selection -- ruling 6 hides the hint then, because
 *     a player driving the sprite is not looking for the D-pad route;
 *   * the ring is on the STRIP of an EMPTY list. "UP: list" would promise a
 *     destination with no rows in it: UP does move the ring out of the strip,
 *     but it lands on nothing the player can select or act on, and a hint that
 *     names an empty place is a false affordance. (The other half stays: on an
 *     empty list with the ring OFF the strip, "RIGHT: actions" is still true
 *     and still useful -- Create and Cancel are there.)
 *
 * "There is no ring at all" was listed here as a third case until A23 (review
 * A20-10) and never was one: every focus_slot < 0 -- the ring on a row, and
 * the no-ring state alike -- returns "RIGHT: actions", which is the ruled
 * behaviour and what case_nav_hint pins. The comment was the thing that was
 * wrong, in a file where the comments are the design record.
 *
 * The Multiplayer twin has one more NULL case (its sticky save-failure line
 * owns the same band, review A20-3). This screen draws nothing else between
 * the list viewport and the strip, so it needs no such rule -- see
 * screen_sworld_render2D. */
static const char* sworld_nav_hint(void) {
	/* The approved composition has no hint band; the status line inside the
	 * panel carries the ONE truthful state that must be disclosed -- a failed
	 * enumeration, which names Retry. Exported unchanged through
	 * screen_sworld_test_hint so a case can pin it. */
	if(list_error)
		return "Couldn't read the saves folder - press Retry";
	if(sworld_status[0])
		return sworld_status;
	return NULL;
}

/* The y of the action row, from the same rect function the render uses. */
static int sworld_strip_top(int width, int height) {
	return sworld_button_rect_fep(width, height, SWORLD_BUTTON_PLAY).y;
}

/* See screen.h for the contract. A pure mapping of sworld_focus_target()'s id
 * space onto the published names -- the SAME function the render and the
 * activation decision read, so this cannot drift from what is highlighted.
 * Production-visible on purpose: lane B's walker links the real screen. */
int screen_sworld_focus_target(void) {
	if(modal_open) {
		switch(modal_row) {
			case SWORLD_MODAL_RENAME: return SCREEN_SWORLD_FOCUS_MODAL_RENAME;
			case SWORLD_MODAL_DELETE: return SCREEN_SWORLD_FOCUS_MODAL_DELETE;
			default: return SCREEN_SWORLD_FOCUS_MODAL_BACK;
		}
	}
	int target = sworld_focus_target();
	if(target == GUTIL_MENU_TARGET_NONE)
		return SCREEN_SWORLD_FOCUS_NONE;
	if(sworld_target_is_row(target))
		return SCREEN_SWORLD_FOCUS_LIST;
	switch(target) {
		case SWORLD_BUTTON_TAB_LOAD: return SCREEN_SWORLD_FOCUS_TAB_LOAD;
		case SWORLD_BUTTON_TAB_CREATE: return SCREEN_SWORLD_FOCUS_TAB_CREATE;
		case SWORLD_BUTTON_TAB_JOIN: return SCREEN_SWORLD_FOCUS_TAB_JOIN;
		case SWORLD_BUTTON_PREV: return SCREEN_SWORLD_FOCUS_PREV;
		case SWORLD_BUTTON_NEXT: return SCREEN_SWORLD_FOCUS_NEXT;
		case SWORLD_BUTTON_PLAY:
			return list_error ? SCREEN_SWORLD_FOCUS_RETRY
				: sworld_list_empty() ? SCREEN_SWORLD_FOCUS_CREATE
									  : SCREEN_SWORLD_FOCUS_PLAY;
		case SWORLD_BUTTON_OPTIONS: return SCREEN_SWORLD_FOCUS_OPTIONS;
		default: return SCREEN_SWORLD_FOCUS_NONE;
	}
}

/* CROSS-LANE OBSERVER (VISUAL-COORDINATION.md section 6 item 2; consumer:
 * Fable's mp_play core). "Would a fresh activate press THIS frame open the
 * JOIN tab?" -- the title helper's exact shape: false when another screen is
 * live, false while the activate button is still held with its edge spent,
 * otherwise the SAME gutil_menu_activate_decide the press asks, on the inputs
 * the last update left, with target = the Join tab header.
 *
 * ROUTE for the core (GUI_DOWN + re-ask never by index): the Load tab opens
 * with focus on the selected ROW (or on the Load tab header when there are no
 * worlds). GUI_UP from the first row reaches the header; GUI_RIGHT on the
 * header walks Load -> Create -> Join (wrapping), and this observer turns true
 * on the Join header. A press there stores the selection, sets the session
 * tab to Join and opens the multiplayer setup screen, whose own
 * screen_mps_focus_is_join then answers for the server row + Join.
 * screen_load_focus_is_join (Join-tab semantics) is the Join slice's, not
 * defined here. */
bool screen_load_focus_is_join_tab(void) {
	if(gstate.players[0].current_screen != &screen_select_world)
		return false;
	if(modal_open)
		return false;
	if(input_held(IB_GUI_CLICK) && !input_pressed(IB_GUI_CLICK))
		return false;
	struct gutil_menu_decision decision
		= gutil_menu_activate_decide(sworld_predict);
	return decision.action == GUTIL_MENU_ACTIVATE
		&& decision.target == SWORLD_BUTTON_TAB_JOIN;
}

/* See screen.h. The same count every other reader uses, so it cannot drift
 * from what the screen lists. */
int screen_sworld_row_count(void) {
	return (int)screen_sworld_count();
}

/* ui-build-1026/T16a: read-only row observers for the world_copy_convert
 * route (see screen.h). No side effects; the same rows render2D draws. */
bool screen_sworld_settled(void) {
	return resolve_cursor >= screen_sworld_count();
}

int screen_sworld_selected_row(void) {
	return gui_selection < screen_sworld_count() ? (int)gui_selection : -1;
}

int screen_sworld_row_of_directory(const char* directory) {
	for(size_t i = 0; directory && i < screen_sworld_count(); i++) {
		const struct world_option* e
			= (const struct world_option*)worlds->data + i;
		if(strcmp(string_get_cstr(e->directory), directory) == 0)
			return (int)i;
	}
	return -1;
}

bool screen_sworld_row_needs_convert(int row) {
	if(row < 0 || (size_t)row >= screen_sworld_count())
		return false;
	const struct world_option* e
		= (const struct world_option*)worlds->data + row;
	return e->state == WORLD_ROW_OK && e->needs_convert;
}

/* A world_copy reservation or partial shown as a row: UNREADABLE and named
 * "<x> (backup)" / "<x> (backup N)". Dot-named temp trees never reach the
 * list (the enumeration skips names starting with '.'). */
bool screen_sworld_partial_copy_visible(void) {
	for(size_t i = 0; i < screen_sworld_count(); i++) {
		const struct world_option* e
			= (const struct world_option*)worlds->data + i;
		if(e->state == WORLD_ROW_UNREADABLE
		   && strstr(string_get_cstr(e->directory), " (backup"))
			return true;
	}
	return false;
}

// See screen.h: the menus_dwell hardware profile's synthetic-pointer target
// for the one interaction this screen has no D-pad route to. Reuses
// sworld_button_rect verbatim so the target can never drift from the real
// hit-test geometry above.
bool screen_sworld_create_button_center(int width, int height, float* cx,
										 float* cy) {
	/* The Create TAB header -- the same rect the render and hit test use. */
	struct fep_rect bounds
		= sworld_button_rect_fep(width, height, SWORLD_BUTTON_TAB_CREATE);
	if(cx)
		*cx = (float)bounds.x + (float)bounds.w / 2.0f;
	if(cy)
		*cy = (float)bounds.y + (float)bounds.h / 2.0f;
	return true;
}

// Process-lifetime latch: the autoload decision fires at most once per boot.
// File scope (not a screen_sworld_update local) so sworld_autoload_new_tick()
// below and the named-world branch in screen_sworld_update share exactly one
// latch -- identical to the single `static int` this replaces, just visible
// to both.
static int autoload_done = 0;

// The "+new" sentinel gate, isolated from screen_sworld_update so it can be
// driven from a test without the heavier per-frame engine calls the rest of
// update() makes (input_pointer/gfx_width/audio -- see screen_sworld_test_reset's
// identical carve-out in the test-hooks block below). Boots straight into a
// freshly created world with NO existing-save gate -- that is BY DESIGN, the
// zero-input capture-rig convenience screen_sworld_create_new()'s own comment
// documents, not a bug to fix here.
//
// 2026-08-17 leak fix: this used to fire SILENTLY. Two arming doors leave the
// sentinel engaged past its intended single run -- a persistent-scope
// `export CAVEX_AUTOLOAD=+new` (see docs/betapp_oracle_2026-08-12.md's capture
// recipe) and a stale cw_run_args.txt left on an SD card after a staged
// hardware run (source/game/hw_run.c) -- and either one silently created a
// junk world on every later boot with no trace of why (the 2026-07-30
// junk-worlds 121/122/123 incident). The fprintf below is layer one: it does
// NOT change *whether* the sentinel fires (still unconditional, on purpose),
// only makes it impossible to miss when it does. Layers two and three (the SD
// cleanup in tools/wait_and_run_wii_hardware.ps1 and the ephemeral-key strip
// in config.c's config_save) stop the two doors from outliving one run.
static bool sworld_autoload_new_tick(const char* autoload_name, bool from_env) {
	if(autoload_done || !autoload_name || strcmp(autoload_name, "+new") != 0)
		return false;
	autoload_done = 1;
	fprintf(stderr,
			"[autoload] \"+new\" sentinel engaged (source: %s) -- creating a "
			"fresh world unconditionally, no existing-save gate\n",
			from_env ? "env CAVEX_AUTOLOAD" : "config autoload");
	screen_sworld_create_new();
	return true;
}

/* Read-only: has this boot already spent the process-lifetime autoload latch?
 * Changes nothing -- the split_recovery_6m driver asks BEFORE it deletes its
 * rig world, because its P4 phase arms that same hook and a card whose config
 * carries an "autoload" key would have consumed it here at boot, leaving P4 to
 * arm nothing and time out in silence. */
bool screen_sworld_autoload_consumed(void) { return autoload_done != 0; }

static float menu_motion_time;

/* Autoload of the hidden tutorial: true (and the world path in `out`) only
 * when `name` is exactly the tutorial folder AND the last reset saw that folder
 * on the card (a directory, not .incomplete, not a copy-journal target). */
static bool sworld_hidden_autoload_path(const char* name, char* out, size_t cap) {
	if(!name || !sworld_dir_is_hidden(name) || !sworld_tutorial_hidden)
		return false;
	const char* saves
		= config_read_string(&gstate.config_user, "paths.worlds", "saves");
	int n = snprintf(out, cap, "%s/%s", saves, SCREEN_TITLE_TUTORIAL_DIR);
	return n > 0 && (size_t)n < cap;
}

static void screen_sworld_update(struct screen* s, float dt) {
	if(dt > 0.0F && true) menu_motion_time = fmodf(menu_motion_time + dt, 128.0F);
	// K rows per frame. K is NOT a Beta number -- Beta enumerates the whole list eagerly.
	// It is a WATCHDOG-SAFETY number: one frame's worth of opens must stay under the
	// detector's 3000ms no-progress threshold, so K=2 tolerates ~1500ms per open before a
	// single frame could fire. Placed FIRST: the autoload block below returns early.
	screen_sworld_resolve_budgeted();

	// Dev hook: boot straight into a named world (no menu click). Source is
	// CAVEX_AUTOLOAD in the environment (PC) OR an "autoload" key in the active
	// config (config_wii.json on the SD) -- the Wii has no environment, so the
	// config path is what works on real hardware / Dolphin. A value of "1" loads
	// the first world; anything else selects by directory name. Lets automated
	// runs reach in-world for screenshots without synthetic input; with neither
	// source set, normal play is completely unaffected.
	const char* autoload_env = getenv("CAVEX_AUTOLOAD");
	bool autoload_from_env = autoload_env && autoload_env[0];
	const char* autoload_name = autoload_from_env
		? autoload_env
		: config_read_string(&gstate.config_user, "autoload", "");
	if(sworld_autoload_new_tick(autoload_name, autoload_from_env))
		return;
	/* The hidden tutorial is not a row, so the by-directory scan below cannot
	 * find it; an autoload naming its exact folder loads it here, through the
	 * same creative arming + world-entry gate as a listed world. */
	if(!autoload_done && autoload_name) {
		char hidden_path[512];
		if(sworld_hidden_autoload_path(autoload_name, hidden_path,
									   sizeof(hidden_path))) {
			autoload_done = 1;
			struct world_option hopt;
			string_init_set_str(hopt.path, hidden_path);
			hopt.state = WORLD_ROW_PENDING;
			hopt.creative = false;
			hopt.needs_convert = false;
			sworld_arm_client_creative(&hopt, NULL);
			string_clear(hopt.path);
			(void)play_session_remember_world(SCREEN_TITLE_TUTORIAL_DIR);
			loading_reason_begin(0, LOAD_SAVE);
			if(!loading_notice_gate_load_world(hidden_path, &screen_select_world))
				loading_reason_end(0);
			return;
		}
	}
	if(!autoload_done && autoload_name && autoload_name[0]
	   && screen_sworld_count() > 0) {
		size_t idx = 0;
		// A value other than "1" selects the world by its directory name -- known at
		// ENUMERATION time, so this branch needs no resolve and works on frame one.
		if(strcmp(autoload_name, "1") != 0) {
			bool found = false;
			for(size_t i = 0; i < screen_sworld_count(); i++) {
				const struct world_option* o
					= (const struct world_option*)worlds->data + i;
				if(strcmp(string_get_cstr(o->directory), autoload_name) == 0) {
					idx = i;
					found = true;
					break;
				}
			}
			if(!found) {
				// Falling through to idx 0 would silently boot the WRONG world, which
				// invalidates every hardware profile run and the world_reload
				// create/verify pair. Stay on the menu instead.
				fprintf(stderr,
						"[autoload] world \"%s\" not found in %zu saves -- staying on "
						"the menu\n",
						autoload_name, screen_sworld_count());
				autoload_done = 1;
				return;
			}
		} else {
			// "1" means the first world of today's EAGER list -- and that list omitted
			// unreadable saves entirely. So skip UNREADABLE rows, and WAIT rather than
			// latch while a leading row is still PENDING.
			size_t i = 0;
			while(i < screen_sworld_count()) {
				const struct world_option* o
					= (const struct world_option*)worlds->data + i;
				if(o->state == WORLD_ROW_PENDING)
					return; // undecided -- try again next frame
				if(o->state == WORLD_ROW_OK)
					break;
				i++;
			}
			if(i >= screen_sworld_count()) {
				autoload_done = 1;
				return; // nothing loadable
			}
			idx = i;
		}
		autoload_done = 1;
		const struct world_option* opt_p
			= (const struct world_option*)worlds->data + idx;
		struct world_option opt = *opt_p;
		sworld_arm_client_creative(opt_p, NULL); /* never the convert notice */

		/* asyncsnd-0926 world-entry gate (loading_notice.h): an armed autoload
		 * parks behind the sound gate like a hand-picked world and proceeds
		 * by itself once the async registry is complete. */
		loading_reason_begin(0, LOAD_SAVE);
		if(!loading_notice_gate_load_world(string_get_cstr(opt.path),
											 &screen_select_world)) loading_reason_end(0);
		return;
	}

	// Debug ("debug.texpack_reload_test", off by default): exercise the live
	// texture-pack reload (re-entrant tex_init()) with no menu input -- after a
	// short delay, switch to the Beta pack so a screenshot catches the menu
	// before + after the swap and confirms tex_init() re-runs without crashing.
	static int texpack_test = -2;
	if(texpack_test == -2)
		texpack_test = (strcmp(config_read_string(&gstate.config_user,
												  "debug.texpack_reload_test",
												  "off"),
							   "on")
						== 0)
			? 0
			: -1;
	if(texpack_test >= 0 && ++texpack_test == 90) {
		config_set_string(&gstate.config_user, "paths.texturepack",
						  "texturepacks/beta-1.7.3");
		tex_init();
	}

	/* The World options modal owns the whole frame while it is up. */
	if(modal_open) {
		sworld_modal_update();
		return;
	}

	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);
	pointer_button = sworld_pointer_button(gfx_width(), gfx_height());

	bool gui_up = input_pressed(IB_GUI_UP);
	bool gui_down = input_pressed(IB_GUI_DOWN);
	bool gui_left = input_pressed(IB_GUI_LEFT);
	bool gui_right = input_pressed(IB_GUI_RIGHT);
	bool navigating = gui_up || gui_down || gui_left || gui_right;
	pointer_owns_focus = gutil_menu_pointer_focus(&focus, pointer_available,
												  pointer_x, pointer_y,
												  navigating);
	int hovered = sworld_pointer_target();
	/* While the pointer OWNS focus the ring follows the sprite across the
	 * controls -- the unchanged Options arbitration. It deliberately does NOT
	 * follow onto list ROWS: Beta's GuiSlot selects a row on the CLICK, not on
	 * hover [740c583 GuiSlot.drawScreen]. */
	if(pointer_owns_focus && hovered != GUTIL_MENU_TARGET_NONE
	   && sworld_target_enabled(hovered)) {
		/* Rows select on click, but pointer movement owns the next activation. */
		sworld_origin = GUTIL_MENU_ORIGIN_POINTER;
		if(!sworld_target_is_row(hovered))
			sworld_focus_target_set(hovered);
	}
	if(navigating) {
		sworld_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
		sworld_navigate(gui_up, gui_down, gui_left, gui_right);
	}

	/* The CANONICAL menu page roles, not the hotbar ones (Astra Lane A item
	 * 3; the shipped Remote / Classic tables bind both pairs to one button).
	 * The MOTION is Beta's own GuiSlot scroll step. */
	bool click_pressed = input_pressed(IB_GUI_CLICK);
	slotlist_pointer_scroll(
		&world_slots, pointer_y,
		pointer_available && pointer_button < 0
			&& (click_pressed || input_held(IB_GUI_CLICK)));

	/* The ONE activation path (ruling 2 item 1): controls AND list rows go
	 * through the same decision, so a sprite resting on a row while the
	 * highlight is on World options activates NEITHER -- unless the sprite is on
	 * a ROW: one click on any row SELECTS it and moves focus to Play (never
	 * launches; ui-build-1026 T01). Play then needs its own press. */
	struct gutil_menu_decision decision
		= gutil_menu_activate_decide((struct gutil_menu_activate_state) {
			.pressed = click_pressed,
			.navigated = navigating,
			.pointer_valid = pointer_available,
			.selection_origin = sworld_origin,
			.hovered = hovered,
			.hovered_enabled = sworld_target_enabled(hovered),
			.selected = sworld_focus_target(),
			.selected_enabled = sworld_target_enabled(sworld_focus_target()),
			/* R-D: this screen's targets open screens, they do not destroy
			 * anything by themselves; Delete opens a CONFIRMATION. */
			.selected_destructive = false,
			/* T01: a row press only selects + focuses Play (never launches),
			 * so ONE click on a row chooses it. */
			.hovered_selects_only = sworld_target_is_row(hovered),
		});
	if(decision.action == GUTIL_MENU_FOCUS_ONLY)
		sworld_focus_target_set(decision.target);
	sworld_origin = decision.selection_origin;
	sworld_predict = (struct gutil_menu_activate_state) {
		.pressed = true, /* the observer asks "IF a press arrived" */
		.navigated = false,
		.pointer_valid = pointer_available,
		.selection_origin = sworld_origin,
		.hovered = hovered,
		.hovered_enabled = sworld_target_enabled(hovered),
		.selected = sworld_focus_target(),
		.selected_enabled = sworld_target_enabled(sworld_focus_target()),
		.hovered_selects_only = sworld_target_is_row(hovered),
	};
	if(decision.action == GUTIL_MENU_ACTIVATE) {
		sworld_activate_target(decision.target);
		return;
	}
	/* R-C: a press the rule CONSUMED reaches nothing else this frame --
	 * neither the page roles below nor Close (review A6-4). */
	if(click_pressed)
		return;

	/* The CANONICAL menu page roles; see the note above. Full pages here (the
	 * screen-local paging), the same step Prev / Next take. */
	if(input_pressed(IB_GUI_PREVIOUS))
		sworld_page_step(-1);
	if(input_pressed(IB_GUI_PAGE))
		sworld_page_step(1);

	/* Footer Back: the Close role and HOME (Beta's Cancel id 0) return to the
	 * title. The selection is remembered on the way out (hand-off rule). */
	if(input_pressed(IB_HOME) || input_pressed(IB_INVENTORY)) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		sworld_remember_selected();
		screen_set(&screen_title);
	}
}

/* One world row: the pack's widget material over the dark card, the REAL
 * name (with the LCE-style "(Creative)" suffix, maintainer decision 2,
 * 2026-09-01) and the real directory / date / size -- or the truthful
 * "reading..." / "unreadable" -- as DARK ink with no heavy shadow, fitted to
 * the card. The selected row carries the coloured outline, the focused one
 * the bright ring. */
/* The mode / date line of a RESOLVED row: "Survival - 09/23/26" (real
 * state, real last-played date); pending and unreadable rows say so and
 * claim nothing else. Shared by the row card and the modal. */
static void sworld_meta_text(const struct world_option* opt, char* out,
							 size_t cap) {
	if(opt->state == WORLD_ROW_PENDING) {
		snprintf(out, cap, "Reading...");
		return;
	}
	if(opt->state == WORLD_ROW_UNREADABLE) {
		snprintf(out, cap, "Unreadable save");
		return;
	}
	char tmp_time[32];
	time_t t = (time_t)opt->last_access;
	struct tm* tmv = gmtime(&t);
	if(tmv)
		strftime(tmp_time, sizeof(tmp_time), "%D", tmv);
	else
		snprintf(tmp_time, sizeof(tmp_time), "--/--/--");
	snprintf(out, cap, "%s - %s", opt->creative ? "Creative" : "Survival",
			 tmp_time);
}

/* The approved row (proofs/beta--worlds--default.png): a 34 px ITEM ICON from
 * the active pack's own atlas -- cobblestone for Survival, the crafting table
 * for Creative, drawn only once the mode is KNOWN -- then the real name at
 * 19 px and the mode / date at 13 px, both fitted to the space the icon
 * leaves. Selected-but-unfocused rows carry the muted approved accent, the
 * focused row the bright ring. */
#define SW_ROW_ICON 34
#define SW_ROW_NAME_FONT 19
#define SW_ROW_META_FONT 13
static void sworld_draw_row_card(int width, int height, size_t idx) {
	struct sworld_canvas c = sworld_canvas_of(width, height);
	struct fep_rect r = sworld_row_rect(width, height, idx);
	if(r.w <= 0)
		return; /* off the shown page */
	struct world_option opt;
	stack_at(worlds, &opt, idx);
	bool selected = idx == gui_selection;
	bool focused = sworld_focus_target() == SWORLD_TARGET_ROW(idx);
	fep_widget_row(r, focused);
	if(focused)
		fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
	else if(selected)
		fep_outline(r, GFX_GUI_SCALE, 0xB8, 0xA8, 0x78, 0xFF);

	int pad = sw_d(&c, 6), icon = sw_d(&c, SW_ROW_ICON);
	int text_x = r.x + pad + icon + sw_d(&c, 10);
	int text_w = r.x + r.w - pad - text_x;
	if(opt.state == WORLD_ROW_OK) {
		/* Beta ids: cobblestone 4, crafting table 58 -- the pack's own art
		 * through the real item renderer, never a fixture image. */
		struct item_data icon_item = {0};
		icon_item.id = opt.creative ? 58 : 4;
		icon_item.count = 1;
		gutil_draw_item_sized(&icon_item, r.x + pad, r.y + (r.h - icon) / 2, icon,
							  0);
	}

	int name_font = sw_d(&c, SW_ROW_NAME_FONT);
	int meta_font = sw_d(&c, SW_ROW_META_FONT);
	char fitted[192];
	fep_text_fit(fitted, sizeof(fitted), string_get_cstr(opt.name), name_font,
				 text_w);
	gutil_text_rgb(text_x, r.y + sw_d(&c, 4), fitted, name_font, true, 0xF4, 0xF4,
				   0xED);
	int meta_y = r.y + r.h - sw_d(&c, 4) - meta_font;
	if(opt.state == WORLD_ROW_OK && opt.needs_convert) {
		/* ui-build-1026/T09 (approved convert-load): a white-on-#801010
		 * "Must be converted" chip replaces the mode / date line (10.5:1). */
		int pad_x = sw_d(&c, 4), pad_y = sw_d(&c, 2);
		fep_text_fit(fitted, sizeof(fitted), IMPORT_CONVERT_TAG, meta_font,
					 text_w - 2 * pad_x);
		int tw = gutil_font_width(fitted, meta_font);
		fep_fill(fep_rect(text_x, meta_y - pad_y, tw + 2 * pad_x,
						  meta_font + 2 * pad_y),
				 0x80, 0x10, 0x10, 0xFF);
		gutil_text_rgb(text_x + pad_x, meta_y, fitted, meta_font, false, 0xFF,
					   0xFF, 0xFF);
		return;
	}
	char meta[64];
	sworld_meta_text(&opt, meta, sizeof(meta));
	fep_text_fit(fitted, sizeof(fitted), meta, meta_font, text_w);
	gutil_text_rgb(text_x, meta_y, fitted, meta_font, false, 0xC8, 0xC8, 0xC0);
}

static void sworld_draw_button(int width, int height, int button,
							   const char* label, int font) {
	struct fep_rect r = sworld_button_rect_fep(width, height, button);
	bool enabled = sworld_button_enabled(button);
	bool focused = sworld_focus_target() == button;
	fep_widget(r, enabled && focused);
	if(focused)
		fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
	if(!enabled)
		fep_fill(r, 0, 0, 0, 0x70);
	char text[48];
	fep_text_fit(text, sizeof(text), label, font, r.w - 2 * GFX_GUI_SCALE);
	int tw = gutil_font_width(text, font);
	uint8_t ink = enabled ? (focused ? 0xFF : 0xF4) : 0xA0;
	gutil_text_rgb(r.x + (r.w - tw) / 2, r.y + (r.h - font) / 2, text, font,
				   true, ink, ink, enabled && focused ? 0xA0 : ink);
}

/* The approved footer: Move / Select / Back from the real prompt helper and
 * the detected bindings, measured then centred (the pack screen's accepted
 * footer, kept local). */
static void sworld_draw_footer(int width, int height) {
	static const enum input_button btn[3] = {IB_GUI_UP, IB_GUI_CLICK,
											 IB_INVENTORY};
	static const char* const lbl[3] = {"Move", "Select", "Back"};
	struct input_player_frame prompts;
	input_prompt_frame(0, &prompts);
	/* The centred hint row (controller UI redesign 10-31, A1 helper). */
	struct gutil_hint_item items[3];
	for(int k = 0; k < 3; k++)
		items[k] = (struct gutil_hint_item) {{btn[k]}, 1, lbl[k], k, k == 2};
	struct gutil_hint_row row;
	if(gutil_hint_row_layout(&prompts, 0, 0, width, height, false,
							 GUTIL_HINT_ALIGN_CENTRE, items, 3, &row))
		gutil_hint_row_draw(&row);
}

static void screen_sworld_render2D(struct screen* s, int width, int height) {
	(void)s;
	struct sworld_canvas c = sworld_canvas_of(width, height);
	if(!fep_backdrop_at(width, height, 90, menu_motion_time, true))
		gutil_bg();

	/* The heading at the composition's 88,14 in the 26 px face. */
	gutil_text(sw_x(&c, SW_TITLE_X), sw_d(&c, SW_TITLE_Y), "Play game",
			   sw_d(&c, SW_TITLE_FONT), true);

	/* toast-1030 (round2 bug 14): "Player 1 Connected" covered "Play game"
	 * from the shared whole-screen corner. The notice mounts in the header
	 * field right of the title (326..616, above the tabs at 50) instead; the
	 * "Saved worlds" caption that lives there steps aside while one is up. */
	bool notice_up = gutil_connection_notice_width(
						 0, gstate.players[1].local_player != NULL, false) > 0;
	{
		struct sworld_canvas nc = sworld_canvas_of(width, height);
		int nx = sw_x(&nc, 326);
		screen_notice_compose(nx, sw_d(&nc, 14), sw_x(&nc, 616) - nx,
							  sw_d(&nc, 32), false);
	}
	/* "Saved worlds", right-aligned in the 226 px header field at 326,18,
	 * 15 px. */
	if(!notice_up) {
		int f = sw_d(&c, 15);
		const char* sub = "Saved worlds";
		gutil_text_rgb(sw_x(&c, 326 + 226) - gutil_font_width(sub, f), sw_d(&c, 18),
					   sub, f, true, 0xD8, 0xD8, 0xD0);
	}

	/* The tab header. INACTIVE tabs are plain: the paper material tinted
	 * grey with a dark, unshadowed label -- no widget strip. The ACTIVE Load
	 * tab is paper CONTINUOUS with the panel (drawn before the panel, no
	 * bevel between them) with a 4 px underline inset 12. Only the ring
	 * that really has focus gets the bright inset outline; there is no
	 * "active" outline. Everything here is the pack's own crafting-panel
	 * cell and the shared neutral greys; nothing inspects the pack's name. */
	static const char* const tab_label[3] = {"Load", "Create", "Join"};
	struct fep_rect panel = sworld_panel_rect(width, height);
	for(int t = SWORLD_BUTTON_TAB_LOAD; t <= SWORLD_BUTTON_TAB_JOIN; t++) {
		struct fep_rect r = sworld_button_rect_fep(width, height, t);
		bool active = t == SWORLD_BUTTON_TAB_LOAD;
		bool focused = sworld_focus_target() == t;
		fep_paper(r);
		if(!active)
			fep_fill(r, 0x40, 0x40, 0x40, 0x70); /* the grey tint */
		int f = sw_d(&c, 20);
		int tw = gutil_font_width(tab_label[t], f);
		uint8_t ink = active ? 0x28 : 0x50;
		gutil_text_rgb(r.x + (r.w - tw) / 2, r.y + (r.h - f) / 2, tab_label[t], f,
					   false, ink, ink, ink);
		if(active) {
			int inset = sw_d(&c, 12), ul = sw_d(&c, 4);
			fep_fill(fep_rect(r.x + inset, r.y + r.h - ul, r.w - 2 * inset, ul),
					 0xB8, 0xA8, 0x78, 0xFF);
		}
		if(focused)
			fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
	}

	/* The paper panel (continuous with the active tab), the list inside. */
	fep_paper(panel);
	{
		/* A bevel on three sides only: the top edge under the active tab
		 * stays open so the tab and the panel read as ONE surface. */
		int e = GFX_GUI_SCALE;
		struct fep_rect load_tab
			= sworld_button_rect_fep(width, height, SWORLD_BUTTON_TAB_LOAD);
		gfx_texture(false);
		fep_solid(fep_rect(panel.x, panel.y, load_tab.x - panel.x, e), 0xFF, 0xFF,
				  0xFF, 0x60);
		fep_solid(fep_rect(load_tab.x + load_tab.w, panel.y,
						   panel.x + panel.w - load_tab.x - load_tab.w, e),
				  0xFF, 0xFF, 0xFF, 0x60);
		fep_solid(fep_rect(panel.x, panel.y + e, e, panel.h - 2 * e), 0xFF, 0xFF,
				  0xFF, 0x60);
		fep_solid(fep_rect(panel.x, panel.y + panel.h - e, panel.w, e), 0x00, 0x00,
				  0x00, 0x70);
		fep_solid(fep_rect(panel.x + panel.w - e, panel.y + e, e, panel.h - 2 * e),
				  0x00, 0x00, 0x00, 0x70);
		gfx_texture(true);
	}
	if(list_error) {
		char fitted[96];
		fep_text_fit(fitted, sizeof(fitted), sworld_nav_hint(), sw_d(&c, 16),
					 panel.w - sw_d(&c, 24));
		gutil_text_rgb(panel.x + (panel.w - gutil_font_width(fitted, sw_d(&c, 16))) / 2,
					   sw_d(&c, 160), fitted, sw_d(&c, 16), false, 0xC0, 0x30,
					   0x30);
		sworld_draw_button(width, height, SWORLD_BUTTON_PLAY, "Retry",
						   sw_d(&c, 20));
	} else if(sworld_list_empty()) {
		const char* none = "No saved worlds yet.";
		int f = sw_d(&c, 16);
		gutil_text_rgb(panel.x + (panel.w - gutil_font_width(none, f)) / 2,
					   sw_d(&c, 180), none, f, false, 0x30, 0x30, 0x30);
		sworld_draw_button(width, height, SWORLD_BUTTON_PLAY, "Create a world",
						   sw_d(&c, 20));
	} else {
		/* The SAME band the hit test uses (sworld_list_band): 110..300. */
		struct fep_rect band = sworld_list_band(width, height);
		int top = band.y, bottom = band.y + band.h;
		gfx_scissor(true, (uint32_t)(c.ox > 0 ? c.ox : 0), (uint32_t)top,
					(uint32_t)sw_d(&c, 640), (uint32_t)(bottom - top));
		for(size_t row = 0; row < screen_sworld_count(); row++) {
			struct fep_rect r = sworld_row_rect(width, height, row);
			if(r.y + r.h <= top || r.y >= bottom)
				continue;
			sworld_draw_row_card(width, height, row);
		}
		gfx_scissor(false, 0, 0, 0, 0);
		/* The page band: one page -> "N worlds" at 108,325 and no arrows;
		 * more -> Prev / Next 44x44 with the boundary arrow disabled and the
		 * "page/pages" count at 148,325 in a 60 px field. */
		char count[32];
		int cf = sw_d(&c, 14);
		if(sworld_paging()) {
			sworld_draw_button(width, height, SWORLD_BUTTON_PREV, "<",
							   sw_d(&c, 20));
			sworld_draw_button(width, height, SWORLD_BUTTON_NEXT, ">",
							   sw_d(&c, 20));
			snprintf(count, sizeof(count), "%zu/%zu",
					 page_first / SW_ROWS_VISIBLE + 1, sworld_page_count());
			int cw = gutil_font_width(count, cf);
			gutil_text_rgb(sw_x(&c, 148) + (sw_d(&c, 60) - cw) / 2, sw_d(&c, 325),
						   count, cf, true, 0xF4, 0xF4, 0xED);
		} else {
			size_t n = screen_sworld_count();
			snprintf(count, sizeof(count), "%zu world%s", n, n == 1 ? "" : "s");
			gutil_text_rgb(sw_x(&c, 108), sw_d(&c, 325), count, cf, true, 0xF4,
						   0xF4, 0xED);
		}
		/* PRIMARY: "Play world" on the pack's widget material. */
		sworld_draw_button(width, height, SWORLD_BUTTON_PLAY, "Play world",
						   sw_d(&c, 20));
		/* QUIETER: "World options" is a TEXT action outside the panel -- no
		 * filled widget strip; only real focus draws the ring. Same 148x44
		 * hit rect. */
		{
			struct fep_rect r
				= sworld_button_rect_fep(width, height, SWORLD_BUTTON_OPTIONS);
			bool enabled = sworld_button_enabled(SWORLD_BUTTON_OPTIONS);
			bool focused = sworld_focus_target() == SWORLD_BUTTON_OPTIONS;
			if(focused)
				fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
			int f = sw_d(&c, 16);
			char text[32];
			fep_text_fit(text, sizeof(text), "World options", f, r.w - 2 * GFX_GUI_SCALE);
			int tw = gutil_font_width(text, f);
			uint8_t ink = enabled ? (focused ? 0xFF : 0xD8) : 0x80;
			gutil_text_rgb(r.x + (r.w - tw) / 2, r.y + (r.h - f) / 2, text, f, true,
						   ink, ink, enabled && focused ? 0xA0 : ink);
		}
	}

	/* The visible status line (a refused management path / remember): to the
	 * RIGHT of the page count, fitted so it cannot collide with it, red on
	 * the paper. */
	if(!list_error && sworld_status[0]) {
		char fitted[128];
		int f = sw_d(&c, 13);
		int x = sw_x(&c, sworld_paging() ? 264 : 200);
		int room = sw_x(&c, 540) - x;
		fep_text_fit(fitted, sizeof(fitted), sworld_status, f, room);
		gutil_text_rgb(x, sw_d(&c, 325), fitted, f, false, 0xB0, 0x30, 0x30);
	}

	/* The footer is hidden under the modal (the card owns the frame). */
	if(!modal_open)
		sworld_draw_footer(width, height);

	/* The World options modal over everything: a paper card with three rows
	 * from the SAME rect functions the hit test uses; the focused one ringed,
	 * a disabled Rename dimmed. */
	if(modal_open) {
		fep_fill(fep_rect(0, 0, width, height), 0, 0, 0, 0xA0);
		struct fep_rect card = sworld_modal_card_rect(width, height);
		fep_paper(card);
		fep_bevel(card);
		/* Heading, the CAPTURED world's name and its mode / date -- dark
		 * unshadowed ink on the paper, fitted to the 432 px text field. */
		{
			int hf = sw_d(&c, SW_MODAL_HEAD_FONT), nf = sw_d(&c, SW_MODAL_NAME_FONT);
			int mf = sw_d(&c, SW_MODAL_META_FONT), tw = sw_d(&c, SW_MODAL_TEXT_W);
			char fitted[192];
			gutil_text_rgb(sw_x(&c, SW_MODAL_HEAD_X), sw_d(&c, SW_MODAL_HEAD_Y),
						   "World options", hf, false, 0x28, 0x28, 0x28);
			fep_text_fit(fitted, sizeof(fitted), modal_name, nf, tw);
			gutil_text_rgb(sw_x(&c, SW_MODAL_HEAD_X), sw_d(&c, SW_MODAL_NAME_Y),
						   fitted, nf, false, 0x28, 0x28, 0x28);
			const struct world_option* e = sworld_selected_row();
			char meta[64] = "";
			if(e && strcmp(string_get_cstr(e->path), modal_path) == 0)
				sworld_meta_text(e, meta, sizeof(meta));
			fep_text_fit(fitted, sizeof(fitted), meta, mf, tw);
			int mw = gutil_font_width(fitted, mf);
			gutil_text_rgb(sw_x(&c, 320) - mw / 2, sw_d(&c, SW_MODAL_META_Y), fitted,
						   mf, false, 0x50, 0x50, 0x50);
		}
		static const char* const rows[SWORLD_MODAL_COUNT] = {"Rename",
															 "Delete world", "Back"};
		for(int k = 0; k < SWORLD_MODAL_COUNT; k++) {
			struct fep_rect r = sworld_modal_row_rect(width, height, k);
			bool enabled = sworld_modal_row_enabled(k);
			bool focused = modal_row == k;
			fep_widget(r, enabled && focused);
			if(focused)
				fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
			if(!enabled)
				fep_fill(r, 0, 0, 0, 0x70);
			int f = sw_d(&c, 20);
			int tw = gutil_font_width(rows[k], f);
			uint8_t ink = enabled ? 0xFF : 0xA0;
			gutil_text_rgb(r.x + (r.w - tw) / 2, r.y + (r.h - f) / 2, rows[k], f,
						   true, ink, ink, enabled && focused ? 0xA0 : ink);
		}
	}

	/* The cursor stays visible over the modal: the modal's rows are pointer
	 * targets. */
	if(pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(pointer_x, pointer_y, glm_rad(pointer_angle), 0,
							 0, 256, 256, 48 * GFX_GUI_SCALE,
							 48 * GFX_GUI_SCALE);
	}
}

struct screen screen_select_world = {
	.reset = screen_sworld_reset,
	.update = screen_sworld_update,
	.render2D = screen_sworld_render2D,
	.render3D = NULL,
	.render_world = false,
};

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* Test hooks for the incremental world-list resolve (handoff P3(a)). These drive
   enumeration and resolution directly, with no GL context and no input, so the assertions
   can be on WORK PER CALL -- counts of level_archive_create -- rather than on elapsed time.
   Wall clock would be the wrong instrument here: a PC SSD hides the SD-card cost this split
   exists to bound. */
void screen_sworld_test_reset(int width, int height) {
	screen_sworld_reset(&screen_select_world, width, height);
}

// The "+new" autoload sentinel gate, isolated exactly like screen_sworld_reset
// above (no input/gfx/audio) -- 2026-08-17 leak fix regression coverage.
// Resets the process-lifetime autoload_done latch FIRST: production only ever
// loads a menu once per boot, so nothing else needs to clear it, but repeated
// test cases in one binary would otherwise see a stale "already handled" from
// an earlier case and silently no-op.
void screen_sworld_test_reset_autoload_latch(void) { autoload_done = 0; }

// Thin passthrough to the exact function screen_sworld_update calls -- not a
// reimplementation, so this cannot drift from what actually ships. Returns
// true iff the sentinel fired (screen_sworld_create_new() ran).
bool screen_sworld_test_autoload_new_tick(const char* autoload_name,
										   bool from_env) {
	return sworld_autoload_new_tick(autoload_name, from_env);
}

size_t screen_sworld_test_resolve_step(size_t budget) {
	return screen_sworld_resolve_step(budget);
}

// The REAL per-frame path, budget and all -- what update() calls.
size_t screen_sworld_test_resolve_frame(void) { return screen_sworld_resolve_budgeted(); }

size_t screen_sworld_test_archive_opens(void) { return archive_opens; }

size_t screen_sworld_test_row_count(void) { return screen_sworld_count(); }

bool screen_sworld_test_hidden_autoload_path(const char* name, char* out,
											 size_t cap) {
	return sworld_hidden_autoload_path(name, out, cap);
}

size_t screen_sworld_test_pending(void) { return world_list_pending_rows(); }

int screen_sworld_test_row(size_t idx, const char** name, const char** directory,
						   const char** path, int64_t* last_access,
						   int64_t* byte_size) {
	if(!worlds || idx >= screen_sworld_count())
		return -1;
	const struct world_option* e = (const struct world_option*)worlds->data + idx;
	if(name)
		*name = string_get_cstr(e->name);
	if(directory)
		*directory = string_get_cstr(e->directory);
	if(path)
		*path = string_get_cstr(e->path);
	if(last_access)
		*last_access = e->last_access;
	if(byte_size)
		*byte_size = e->byte_size;
	return (int)e->state;
}

bool screen_sworld_test_row_creative(size_t idx) {
	if(!worlds || idx >= screen_sworld_count())
		return false;
	return ((const struct world_option*)worlds->data + idx)->creative;
}

size_t screen_sworld_test_next_selection(size_t cur, size_t count, bool up, bool down) {
	return sworld_next_selection(cur, count, up, down);
}

/* Read-only observation points for tests/test_frontend_no_pointer.c: the
 * viewport offset a page role moves, and the row a page role must NOT lose.
 * Nothing here changes state. */
int screen_sworld_test_scroll(void) {
	/* The screen-local page offset in canvas pixels: page index times the
	 * page height (4 rows of 48). Zero on the first page. */
	return (int)(page_first / SW_ROWS_VISIBLE) * SW_ROWS_VISIBLE * SW_ROW_PITCH;
}

size_t screen_sworld_test_gui_selection(void) { return gui_selection; }

/* convert-notice-1024: the REAL Play of row `idx` (remember + play, exactly
 * the Play button's two calls). Returns false for an out-of-range row. */
bool screen_sworld_test_play_row(size_t idx) {
	if(!worlds || idx >= screen_sworld_count())
		return false;
	gui_selection = idx;
	sworld_remember_selected();
	sworld_play_selected();
	return true;
}

size_t screen_sworld_test_play_archive_opens(void) {
	return play_archive_opens;
}

/* THE NAVIGATION HINT as the render draws it (A19): the same pure decision
 * and the same rect function screen_sworld_render2D calls, plus the two edges
 * it must stay between -- the list viewport's bottom and the strip's first
 * row. Returns false when nothing is drawn. Read-only. */
bool screen_sworld_test_hint(char* text, size_t cap, int* y, int* h,
							 int* list_bottom, int* strip_top) {
	int width = gfx_width(), height = gfx_height();
	const char* hint = sworld_nav_hint();
	struct gutil_button_bounds r
		= gutil_menu_nav_hint_rect(width, sworld_strip_top(width, height),
								   hint);
	if(text && cap)
		snprintf(text, cap, "%s", hint ? hint : "");
	if(y)
		*y = r.y;
	if(h)
		*h = r.height;
	if(list_bottom)
		*list_bottom = world_slots.bottom;
	if(strip_top)
		*strip_top = sworld_strip_top(width, height);
	return hint != NULL && r.width > 0;
}

/* The row widget_slotlist.c's render draws the selection chrome on (A15).
 * gui_selection is the screen's intent; THIS is what the player sees -- so a
 * case can state "the row stays highlighted as the selected world while the
 * strip button carries the focus treatment" as an assertion instead of prose. */
size_t screen_sworld_test_list_selection(void) {
	return slotlist_selected(&world_slots);
}

/* The target the ring rests on, in the ONE id space (a control id, or
 * SWORLD_BUTTON_COUNT + row while the list owns focus). */
int screen_sworld_test_focus(void) {
	return sworld_focus_target();
}

/* The target this screen draws HIGHLIGHTED: the modal's row while the modal
 * is up (Rename 0 / Delete 1 / Back 2), else the same id sworld_focus_target
 * reports -- the very function render2D and the decision both read (A3
 * NEW-7). */
int screen_sworld_test_highlight(void) {
	if(modal_open)
		return modal_row;
	return sworld_focus_target();
}
int screen_sworld_test_row_target(size_t row) {
	return SWORLD_TARGET_ROW(row);
}

/* WHICH world a Delete armed (push-0921 A15). sworld_delete_path is what
 * sworld_delete_confirmed would erase on YES, so this is the delete's TARGET
 * stated as the thing that would actually be destroyed -- a stronger claim
 * than the confirmation's message text, and the one that matters when the
 * question is "does Delete act on the row the player selected, or on the last
 * row?". Empty while no confirmation is armed. Read-only. */
bool screen_sworld_test_delete_target(char* path, size_t cap) {
	if(path && cap)
		snprintf(path, cap, "%s", sworld_delete_path);
	return sworld_delete_path[0] != 0;
}

bool screen_sworld_test_normalize_directory_path(const char* path, char* result,
										 size_t result_size) {
	if(!path || !result)
		return false;
	size_t length = strlen(path);
	if(length == 0 || length >= result_size)
		return false;
	memcpy(result, path, length + 1);
	(void)sworld_trim_directory_path(result, length);
	return true;
}

bool screen_sworld_test_reserve_directory(const char* saves_path,
										uint32_t first, char* result,
										size_t result_size) {
	if(!result || result_size == 0)
		return false;
	result[0] = '\0';
	string_t directory;
	string_init(directory);
	bool ok = sworld_reserve_unique_directory_from(saves_path, first, directory);
	if(ok) {
		size_t required = strlen(string_get_cstr(directory)) + 1;
		if(required > result_size) {
			(void)sworld_rmdir(string_get_cstr(directory));
			ok = false;
		} else {
			memcpy(result, string_get_cstr(directory), required);
		}
	}
	string_clear(directory);
	return ok;
}

void screen_sworld_test_delete_tree(const char* path) {
	sworld_delete_tree(path, 0);
}

bool screen_sworld_test_reserve_beta(const char* saves_path,
									 const char* folder_base, char* result,
									 size_t result_size) {
	if(!result || result_size == 0)
		return false;
	result[0] = '\0';
	string_t directory;
	string_init(directory);
	bool ok = sworld_reserve_beta_directory(saves_path, folder_base, directory);
	if(ok) {
		size_t required = strlen(string_get_cstr(directory)) + 1;
		if(required > result_size) {
			(void)sworld_rmdir(string_get_cstr(directory));
			ok = false;
		} else {
			memcpy(result, string_get_cstr(directory), required);
		}
	}
	string_clear(directory);
	return ok;
}

bool screen_sworld_test_persist_new(const char* directory, int64_t seed,
									vec3 world_spawn, vec3 player_spawn,
									bool start_nether) {
	if(!directory)
		return false;
	string_t path;
	string_init_set_str(path, directory);
	bool ok = sworld_persist_new_archive(path, seed, world_spawn,
										 player_spawn, start_nether,
										 "New World", false);
	string_clear(path);
	return ok;
}
#endif
