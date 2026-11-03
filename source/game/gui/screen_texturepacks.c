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

// Pack selector (reached from the title or the world-select menu). Lists the
// bundled pack plus every folder under "texturepacks/", under two categories:
// TEXTURES writes paths.texturepack and HOT-RELOADS all textures (tex_init(),
// which is re-entrant); SOUNDS writes paths.audiopack and rescans the music and
// effect registries. The two choices are INDEPENDENT -- neither apply touches
// the other's key, whatever files the pack happens to ship -- and a pack is
// applied only by the explicit Apply button, never by pressing its row.
// No world is loaded for a texture change, so nothing needs re-meshing; a real
// Beta 1.7.3 pack dropped into texturepacks/ thus "fully refreshes" the
// textures like a Minecraft resource pack, and missing files fall back to the
// bundled assets. Sounds are title-screen only (see the in-game note below).

#include "../../graphics/gui_util.h"
#include "../../graphics/gfx_settings.h"
#include "../../platform/audio.h" // S0: GUI click (playSoundFX parity path)
#include "../../platform/audio_music.h" // H1: music/ pack live-swap
#include "../../platform/gfx.h"
#include "../../platform/hw_telemetry.h" // PACK_RELOAD wall-clock on the wire
#include "../../platform/input.h"
#include "../../platform/input_glyph.h" // input_prompt_frame: the DETECTED footer bindings
#include "../../platform/input_owner.h" // struct input_player_frame
#include "../../platform/time.h"
#include "../../platform/texture.h"
#include "../../platform/watchdog.h" // the reload is a declared multi-second main-thread stall
#include "../../texturepacks.h"
#include "../../util.h" // path_from_sd_root: launcher-independent pack compare/scan
#include "../game_state.h"
#include "frontend_presentation.h" // gate 2: the approved front-end's materials
#include "loading_notice.h"
#include "screen.h"
#include "backdrop_bake.h" /* BG-BAKE-1028/3: job hooks + bake states only */
#include "screen_beta_import.h" /* ingame-import-1030: Import Beta 1.7.3 */
#include "widget_slotlist.h"
#include "loading_indicator.h" /* pack-lag-1030: live reload frame */

#include <m-lib/m-string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TEXPACKS_DIR "texturepacks"
/* Apply runs one rendered frame after the click so the loading strip is on
 * screen before the synchronous reload blocks the main thread (2026-09-11:
 * Earth & Ash took 3.2 s and 4.7 s on the console -- hang_report seq 1/2 in
 * SCRN_UPD -- so the reload is also declared to the watchdog). */
#define TEXPACKS_APPLY_DEFER_FRAMES 2
#define TEXPACKS_RELOAD_GRACE_MS 60000 /* rig 2026-09-11: the bundled-pack reload took 23 s on the card (hang_report seq 1, PACK_RELOAD, gap 23235 ms) -- the grace must exceed the legitimate reload, not the ideal one; the incremental loader is the real fix */
#define TEXPACKS_FAILED_FRAMES 240 /* ~4 s of "Couldn't load texture pack." */
static int apply_pending_frames;
static size_t apply_pending_index;
static int apply_failed_frames;

// The config may hold either the relative form this screen writes
// ("texturepacks/<name>") or the absolute field-mitigation form
// ("sd:/texturepacks/<name>") -- both name the same pack. Compare via
// path_from_sd_root so the active-row highlight matches either spelling.
// (Unconditional on purpose: both sides get the same normalization, so
// equality is unaffected on PC.)
/* ONE comparison, shared with the transactions (texturepacks.c): the buffered
 * version here had the same 251-byte truncation REVIEW-LIBRARY-1 found in the
 * library copy, so the active-row highlight and the NO_CHANGE decision could
 * both point at the wrong pack. */
static bool texpacks_value_eq(const char* a, const char* b) {
	return texturepacks_value_eq(a, b);
}
#define TEXPACKS_DEFAULT "assets" // the bundled fCavEX pack (== TEX_DEFAULT_PACK)
#define TEXPACKS_DEFAULT_NAME "Earth & Ash (default)"
/* A dropped-in copy of the default art (texturepacks/earth-and-ash): hidden
 * when it is the bundled default byte for byte, else listed under this name
 * (maintainer ruling 2026-10-30). */
#define TEXPACKS_EA_FOLDER "earth-and-ash"
#define TEXPACKS_EA_PACK_NAME "Earth & Ash (pack)"
#define MAX_PACKS 32

struct pack_option {
	string_t name;	// shown in the list
	string_t value; // written to paths.texturepack / paths.audiopack
	bool has_audio; // eligible for the Sounds category (files present)
	bool has_music; // claims a music/ directory (for the honest row label)
};

static struct pack_option packs[MAX_PACKS];
static size_t pack_count = 0;

/* THE TWO INDEPENDENT CHOICES (Astra gate 1 items 2-3, 7). One installed-pack
 * list, two categories over it: Textures lists every pack, Sounds lists only
 * the packs that actually ship loadable audio. `rows` is the VISIBLE list for
 * the current category -- indices into packs[] -- so the slotlist, the
 * highlight and the apply all address the same row. */
enum texpacks_category {
	TEXPACKS_CAT_TEXTURES = 0,
	TEXPACKS_CAT_SOUNDS,
	TEXPACKS_CAT_COUNT,
};

static enum texpacks_category category;
static size_t rows[MAX_PACKS + 1]; /* + the Import card */
static size_t row_count;
/* The PENDING choice per category, remembered across a category toggle (gate 1
 * item 9) and discarded by Done/Home/Close (item 7). It is an index into
 * packs[], or MAX_PACKS for "nothing chosen yet, the current one stands". */
static size_t pending[TEXPACKS_CAT_COUNT];
/* What the last Apply concluded, for the caption. */
static enum texturepacks_result last_result;
static bool last_result_valid;
static enum texpacks_category last_result_category;

/* Defined with the apply transaction below; the reset above seeds them. */
static void texpacks_audio_selection(char* out, size_t size);

static size_t gui_selection;
static struct slotlist pack_slots;
static bool pointer_available;
static float pointer_x, pointer_y, pointer_angle;
static int pointer_button = -1;
/* A5 (Astra ruling 2): this screen was the one lane-A menu that never
 * arbitrated at all -- it partitioned on `pointer_available`, so with a live
 * Wii Remote the directional focus it does have was invisible and the pointer
 * decided everything. It now uses the SAME movement-based helper every other
 * menu does, plus the one activation rule. */
static struct gutil_menu_focus focus;
static bool texpacks_pointer_owns;
/* drift-1030: frames the resting pointer has stayed inside the threshold. */
#define TEXPACKS_DRIFT_FRAMES 15
static int texpacks_drift_frames;
/* -1 = the LIST owns the ring (the only thing UP/DOWN walks, unchanged);
 * otherwise the strip button the POINTER has put the ring on. Nothing
 * directional can set this: it exists so a pointer resting on Done has a
 * highlight to move onto, which is what the activation rule's focus-only step
 * requires to be able to reach a second press. */
static int texpacks_focus_button = -1;
/* WHO CHOSE the highlight -- see screen_title.c's note (lead ruling R-A,
 * closing review A6-1). Sticky: it moves only when the player does, never on
 * an IR validity transition. */
static enum gutil_menu_origin texpacks_origin;
static int texpacks_focus_target(void); /* defined with the strip geometry */
/* The approved composition's canvas mapping (see THE APPROVED COMPOSITION
 * below for what it means). Declared here because screen_texpacks_reset needs
 * it to size the list viewport; the helpers are defined with the rest of the
 * geometry. */
struct texpacks_canvas {
	int ox;	 /* left edge of the centred 640-wide canvas */
	float u; /* pixels per canvas unit */
};
static struct texpacks_canvas texpacks_canvas_of(int width, int height);
static int tp_d(const struct texpacks_canvas* c, int v);

/* THE APPROVED COMPOSITION's canvas numbers (see the block comment further
 * down for what each one means and where it comes from). They live here, above
 * the reset, because screen_texpacks_reset builds the list viewport out of
 * TP_LIST_TOP / TP_LIST_BOTTOM / TP_ROW_PITCH -- values unchanged. */
#define TP_HEAD_X 32
#define TP_HEAD_Y 22
#define TP_HEAD_FONT 28
#define TP_NOTE_Y 58
#define TP_NOTE_FONT 16
#define TP_CAT_W 140
#define TP_CAT_H 32
#define TP_CAT_Y 18
#define TP_PANEL_X 32
#define TP_PANEL_Y 86
#define TP_PANEL_W 576
#define TP_PANEL_H 276
#define TP_LIST_TOP 94
#define TP_LIST_BOTTOM 312
#define TP_ROW_X 48
#define TP_ROW_W 544
#define TP_ROW_PITCH 70
#define TP_ROW_ACTION 60
#define TP_ROW_ICON 40
#define TP_ROW_PAD 10
/* "<folder> (Beta jar)", with a trailing ".jar"/".zip" dropped from the
 * folder name (an archive tool's "extract to b1.7.3.jar/" folder). */
static void texpacks_raw_jar_name(string_t out, const char* folder) {
	size_t n = strlen(folder);
	if(n > 4 && folder[n - 4] == '.'
	   && ((strcmp(folder + n - 3, "jar") == 0)
		   || strcmp(folder + n - 3, "JAR") == 0
		   || strcmp(folder + n - 3, "zip") == 0
		   || strcmp(folder + n - 3, "ZIP") == 0))
		n -= 4;
	string_init_printf(out, "%.*s (Beta jar)", (int)n, folder);
}

/* The approved card's icon-to-name gap is 18, NOT the 10 px card padding the
 * first wiring reused for both. Named so the two can never be confused again. */
#define TP_ROW_ICON_GAP 18
#define TP_NAME_FONT 20
#define TP_BADGE_FONT 14
#define TP_DESC_X 56
#define TP_DESC_Y 322
#define TP_DESC_W 528
#define TP_DESC_FONT 16
#define TP_APPLY_X 168
#define TP_APPLY_Y 378
#define TP_APPLY_W 304
#define TP_APPLY_H 44
#define TP_FOOTER_Y 432
#define TP_FOOTER_H 36
#define TP_FOOTER_GAP 24
#define TP_SCROLL_W 6

/* ===== the per-pack PREVIEW cache =======================================
 *
 * Each card shows ITS OWN pack's picture (tex_preview_load reads that pack's
 * installed root -- never the active one, never the live atlas). Two rules
 * make that affordable on a Wii:
 *
 *   BOUNDED -- at most four textures exist at once: the viewport shows three
 *     whole 60 px cards in its 218 px band and at most one more partly
 *     scrolled in, so a list of any length costs four 40x40 RGB5A3 images.
 *   NEVER IN RENDER -- the cache is reconciled from reset and from update
 *     (texpacks_previews_sync), and render only LOOKS UP. A row whose slot is
 *     absent or empty draws the neutral tile; it does not trigger a load.
 *
 * A slot with a real `pack` and loaded == false is a REMEMBERED MISS: that
 * pack has no usable preview, and we must not go back to the SD card for it
 * on the next frame. The key is the packs[] inventory index, so the Textures
 * and Sounds tabs share one image per pack, and a category rebuild that
 * renumbers the visible rows cannot mis-attribute one. */
#define TEXPACKS_PREVIEW_SLOTS 4
struct texpacks_preview {
	size_t pack;  /* packs[] index, or MAX_PACKS for a free slot */
	bool loaded;  /* a real image is in `tex` (vs. a remembered miss) */
	struct tex_gfx tex;
};
/* MAX_PACKS, not the zero a bare static would get: pack 0 is a REAL pack (the
 * bundled default), so an untouched slot must not claim to hold it. */
static struct texpacks_preview previews[TEXPACKS_PREVIEW_SLOTS] = {
	{.pack = MAX_PACKS}, {.pack = MAX_PACKS},
	{.pack = MAX_PACKS}, {.pack = MAX_PACKS},
};
static void texpacks_previews_clear(void);
static void texpacks_previews_sync(int width, int height);
/* Did THIS update leave the screen? (texpacks_done can run from several places
 * inside the update, all of which return immediately afterwards.) */
static bool screen_texpacks_closed;

/* Where Back returns to: the opener declares it (title or world-select).
 * Defaults to world-select, the pre-title-screen behaviour, so the
 * GUI_RIGHT path from world-select needs no call. */
static struct screen* screen_texpacks_return = &screen_select_world;

void screen_texturepacks_set_return(struct screen* s) {
	screen_texpacks_return = s ? s : &screen_select_world;
}

static bool ingame_mode;
void screen_texturepacks_set_ingame(bool on) {
	ingame_mode = on;
}

/* ===== "Import Beta 1.7.3" (ingame-import-1030, coder C) ==================
 * The button is offered only at the title side (never in-game: the importer
 * decodes sounds, which the maintainer ruling allows at import only, never
 * during play) and only when beta_import_available() found a jar in
 * sd:/beta/. That check touches the SD card, so it runs once per screen
 * entry (reset), never per frame. Without a jar the header's note line says
 * where to put one.
 *
 * The importer's selector (screen_texturepacks_import_select, called once on
 * the main thread after the swap) only QUEUES the pack; the next entry to
 * this screen selects that row and applies it through the SAME deferred
 * Apply transaction a player's own Apply uses (loading strip, watchdog
 * grace, backdrop job). Re-importing the pack that is already current is a
 * NO_CHANGE for the transaction, so that one case forces the texture reload:
 * the files on the card changed under the same name. */
static bool s_import_available;
static char s_import_queued[256];
static bool s_import_force_reload;
/* After the imported textures applied: the pack index whose SOUNDS apply
 * next (the Pack Helper's own rule -- select audio too when the pack carries
 * it, core.py install(audio_select) / _plan_select), MAX_PACKS for none. */
static size_t s_import_audio_planned, s_import_audio_next;
static bool s_import_audio_running, s_import_both_applied;

/* The Import card is the FIRST row of the list (out of game, jar present).
 * It lives in rows[] as this sentinel, which is never a packs[] index, so
 * every packs[] consumer's "idx < pack_count" guard already skips it. */
#define TEXPACKS_IMPORT_ROW ((size_t)MAX_PACKS + 1)
static bool texpacks_import_shown(void) {
	return s_import_available && !ingame_mode;
}
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
int screen_texpacks_test_import_override = -1; /* -1 real, 0 no, 1 yes */
#endif

static bool texpacks_import_probe(void) {
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	if(screen_texpacks_test_import_override >= 0)
		return screen_texpacks_test_import_override != 0;
#endif
	return beta_import_available();
}

bool screen_texturepacks_import_select(const char* pack_value) {
	if(!pack_value || !pack_value[0]
	   || strlen(pack_value) >= sizeof(s_import_queued))
		return false;
	snprintf(s_import_queued, sizeof(s_import_queued), "%s", pack_value);
	return true;
}

/* Two files with identical bytes (streamed, 4 KiB at a time; a missing file
 * is "not identical"). */
static bool texpacks_same_file(const char* a, const char* b) {
	FILE* fa = fopen(a, "rb");
	FILE* fb = fopen(b, "rb");
	bool same = fa && fb;
	while(same) {
		unsigned char ba[4096], bb[4096];
		size_t na = fread(ba, 1, sizeof(ba), fa);
		size_t nb = fread(bb, 1, sizeof(bb), fb);
		if(na != nb || memcmp(ba, bb, na) != 0)
			same = false;
		else if(na == 0)
			break;
	}
	if(fa)
		fclose(fa);
	if(fb)
		fclose(fb);
	return same;
}

/* Is the pack folder `dir` the bundled default? The files that carry the
 * art of every surface (terrain, items, gui, mob skin, font, animation) must
 * be byte-identical to assets/'s; a few hundred KB, read once per screen
 * entry and only for the folder named earth-and-ash. */
static bool texpacks_is_bundled_copy(const char* dir) {
	static const char* const files[] = {
		"terrain.png", "gui/items.png", "gui/gui.png", "anim.png",
		"font/default.png", "mob/char.png", "pointer.png",
	};
	for(size_t k = 0; k < sizeof(files) / sizeof(files[0]); k++) {
		char a[320], b[320];
		snprintf(a, sizeof(a), "%s/%s", dir, files[k]);
		snprintf(b, sizeof(b), "%s/%s", TEXPACKS_DEFAULT, files[k]);
		if(!texpacks_same_file(a, b))
			return false;
	}
	return true;
}

static void packs_free(void) {
	for(size_t i = 0; i < pack_count; i++) {
		string_clear(packs[i].name);
		string_clear(packs[i].value);
	}
	pack_count = 0;
	row_count = 0;
}

/* The pack value each category currently HAS -- the config's own selection,
 * with the legacy fallback for audio (an absent/wrong-typed paths.audiopack
 * means the texture pack is what is playing). Copied by the caller when a
 * transaction is about to rewrite the tree. */
static const char* texpacks_current_value(enum texpacks_category cat) {
	const char* texture = config_read_string(&gstate.config_user,
											 "paths.texturepack", TEXPACKS_DEFAULT);
	if(cat == TEXPACKS_CAT_TEXTURES)
		return texture;
	return config_read_string(&gstate.config_user, "paths.audiopack", texture);
}

/* Rebuild the visible list for `category`, keeping the ring on the row the
 * player last had there: the pending choice if they made one, otherwise the
 * pack the category is currently using. Sounds lists only packs that ship
 * loadable audio -- a texture-only pack cannot be an audio choice, so offering
 * it would be an apply that could only fail. */
static bool texpacks_import_shown(void);
static void texpacks_build_rows(void) {
	row_count = 0;
	if(texpacks_import_shown())
		rows[row_count++] = TEXPACKS_IMPORT_ROW;
	for(size_t i = 0; i < pack_count; i++) {
		if(category == TEXPACKS_CAT_SOUNDS && !packs[i].has_audio
		   && i != 0) /* entry 0 is the bundled default: always selectable */
			continue;
		rows[row_count++] = i;
	}
	const char* active = texpacks_current_value(category);
	size_t want = pending[category] < pack_count ? pending[category]
												 : (size_t)MAX_PACKS;
	gui_selection = 0;
	for(size_t r = 0; r < row_count; r++) {
		if(rows[r] >= pack_count)
			continue; /* the Import card is never "the current pack" */
		if(want < pack_count ? rows[r] == want
							 : texpacks_value_eq(
								 string_get_cstr(packs[rows[r]].value), active)) {
			gui_selection = r;
			break;
		}
	}
	slotlist_set_count(&pack_slots, row_count);
	/* REVEAL it. Switching to a short Sounds list clamps the scroll, so coming
	 * back to a remembered row that lives further down used to leave the
	 * highlight off-screen (screen review 2). Revealing the selection is what
	 * the list already does for every directional move; the page roles are
	 * untouched and still scroll independently of the selection. */
	slotlist_select(&pack_slots, gui_selection, true);
}

static void screen_texpacks_reset(struct screen* s, int width, int height) {
	/* In-game: keep drawing the paused world behind the list (the pause
	 * screen's own contract) so an applied pack previews immediately. */
	s->render_world = ingame_mode;
	s->render3D = ingame_mode ? screen_ingame.render3D : NULL;
	input_pointer_enable(true);

	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;

	/* BEFORE packs_free(): every cached preview is keyed by a packs[] index
	 * that is about to be renumbered by the rescan. */
	texpacks_previews_clear();
	packs_free();

	// Entry 0 is always the bundled default pack (fCavEX), paths=assets.
	/* Derived from GAME_DISPLAY_NAME, not spelled out: this is the SECOND place the
	 * product name reaches the player (the F3 header is the first), and it was
	 * still reading "fCavEX" after that rename. Deriving it means a future
	 * rename cannot leave one screen behind. */
	/* Maintainer ruling 2026-10-30: the bundled default is named for what it
	 * is -- the Earth & Ash art -- not for the game. */
	string_init_set_str(packs[0].name, TEXPACKS_DEFAULT_NAME);
	string_init_set_str(packs[0].value, TEXPACKS_DEFAULT);
	/* The bundled pack ships sound/ but no music/ -- a truthful sound-only
	 * choice, not a pack that promises music and delivers none. */
	packs[0].has_audio = texturepacks_has_audio(TEXPACKS_DEFAULT);
	packs[0].has_music = texturepacks_claims_music(TEXPACKS_DEFAULT);
	pack_count = 1;

	// Then every subdirectory of texturepacks/ (e.g. a dropped-in Beta pack),
	// each addressed as paths=texturepacks/<name>.
	string_t names[MAX_PACKS];
	// Scan the SD-root texturepacks/ dir on Wii regardless of launch CWD
	// (HBC-direct CWD is sd:/apps/CavEX/, where a relative scan finds
	// nothing); PC keeps the CWD-relative scan.
	const char* packs_dir = TEXPACKS_DIR;
#ifdef PLATFORM_WII
	char packs_dir_buf[64];
	packs_dir = path_from_sd_root(TEXPACKS_DIR, packs_dir_buf,
								  sizeof(packs_dir_buf));
#endif
	size_t n = texturepacks_list(packs_dir, names, MAX_PACKS - pack_count);
	for(size_t i = 0; i < n && pack_count < MAX_PACKS; i++) {
		bool ea_copy = !strcmp(string_get_cstr(names[i]), TEXPACKS_EA_FOLDER);
		if(ea_copy) {
			char dir[256];
			snprintf(dir, sizeof(dir), "%s/%s", packs_dir,
					 string_get_cstr(names[i]));
			if(texpacks_is_bundled_copy(dir)) {
				/* The default again under another name: one entry only. */
				string_clear(names[i]);
				continue;
			}
		}
		string_init_printf(packs[pack_count].value, "%s/%s", TEXPACKS_DIR,
						   string_get_cstr(names[i]));
		/* A raw b1.7.3.jar unzip ("b1.7.3.jar" or "minecraft" folder) is
		 * named for what it is; the value (the real folder) is unchanged. */
		/* A pack that names itself (.cavewii-pack.json "name", written by the
		 * Pack Helper and the in-game importer) is listed by that name --
		 * "Beta-1.7.3" reads "Beta 1.7.3"; the value stays the folder. */
		char shown[64];
		if(texturepacks_display_name(string_get_cstr(packs[pack_count].value),
									 shown, sizeof(shown)))
			string_init_set_str(packs[pack_count].name, shown);
		else if(ea_copy)
			string_init_set_str(packs[pack_count].name, TEXPACKS_EA_PACK_NAME);
		else if(texturepacks_is_raw_jar(
					string_get_cstr(packs[pack_count].value)))
			texpacks_raw_jar_name(packs[pack_count].name,
								  string_get_cstr(names[i]));
		else
			string_init_set(packs[pack_count].name, names[i]);
		packs[pack_count].has_audio
			= texturepacks_has_audio(string_get_cstr(packs[pack_count].value));
		packs[pack_count].has_music
			= texturepacks_claims_music(string_get_cstr(packs[pack_count].value));
		pack_count++;
		string_clear(names[i]);
	}

	/* Entry state (gate 1 item 7): no pending choice, no armed apply, no stale
	 * error. A screen re-entered while an apply was armed used to fire it on
	 * whatever came next. In-game the category is forced back to Textures --
	 * Sounds is disabled there. */
	pending[TEXPACKS_CAT_TEXTURES] = MAX_PACKS;
	pending[TEXPACKS_CAT_SOUNDS] = MAX_PACKS;
	category = TEXPACKS_CAT_TEXTURES;
	apply_pending_frames = 0;
	apply_failed_frames = 0;
	last_result_valid = false;

	pointer_available = false;
	pointer_button = -1;
	focus = (struct gutil_menu_focus) {0};
	texpacks_drift_frames = 0;
	texpacks_pointer_owns = false;
	texpacks_focus_button = -1;
	texpacks_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	/* GATE 2: the approved composition's viewport and pitch, in canvas pixels
	 * (studio.js:77-85) -- 94..312 with a 70 px row pitch, replacing Beta's
	 * GuiTexturePackSlot 32 / h-51 / 36 for THIS screen only. The three other
	 * list screens keep the Beta geometry untouched. */
	{
		struct texpacks_canvas c = texpacks_canvas_of(width, height);
		slotlist_reset(&pack_slots, width, tp_d(&c, TP_LIST_TOP),
					   tp_d(&c, TP_LIST_BOTTOM), tp_d(&c, TP_ROW_PITCH), 0, 0);
	}
	s_import_available = !ingame_mode && texpacks_import_probe();
	s_import_audio_planned = s_import_audio_next = MAX_PACKS;
	s_import_audio_running = s_import_both_applied = false;
	texpacks_build_rows();
	if(s_import_queued[0]) {
		/* Back from a finished import: select the imported pack and apply it
		 * through the ordinary deferred Apply (the loading strip shows on the
		 * next frame). The queue is consumed here whatever happens, so it can
		 * never fire twice. If the rescan does not list it, nothing applies
		 * and the import screen already told the player to pick it. */
		for(size_t i = 0; i < pack_count; i++) {
			if(!texpacks_value_eq(string_get_cstr(packs[i].value),
								  s_import_queued))
				continue;
			pending[TEXPACKS_CAT_TEXTURES] = i;
			/* Then its SOUNDS, when it carries audio -- what the Pack Helper
			 * does for an imported pack (install(audio_select=True) applies
			 * the audio key only when the pack has audio). The audio half
			 * is the ordinary Sounds Apply, chained after the textures. */
			if(packs[i].has_audio)
				s_import_audio_planned = i;
			texpacks_build_rows();
			s_import_force_reload = true;
			apply_pending_frames = TEXPACKS_APPLY_DEFER_FRAMES;
			apply_pending_index = i;
			break;
		}
		s_import_queued[0] = '\0';
	}
	/* The first page's images, loaded HERE -- on the screen transition the
	 * player already reads as a load -- so the first rendered frame has them
	 * and no frame ever touches the card. */
	texpacks_previews_sync(width, height);
}

/* ===== THE APPROVED COMPOSITION (gate 2) ================================
 *
 * front-end-refined/studio.js:77-85 + proofs/{beta,isabella,earth}--packs--
 * default.png compose this screen in a 640x480 canvas, in PIXELS (not Beta's
 * logical GUI units): a heading at 32,22; a paper panel at 32,86 576x276 with
 * the list viewport 94..312 inside it and the description line at 56,322; full
 * -width pack cards at x 48, width 544, on a 70 px pitch with 60 px of action
 * art and a 10 px gap; the primary Apply at 168,378 304x44; and the approved
 * glyph footer row at y 432, height 36.
 *
 * WIDER-THAN-4:3 surfaces keep the 640-wide canvas and CENTRE it; everything
 * scales with HEIGHT (u = height/480) through ONE rounding helper, so a
 * 640x480 screen reproduces the pinned numbers exactly (u == 1, ox == 0) and
 * nothing rounds twice.
 *
 * THE THREE CONTROLS keep their enum values and their declaration order --
 * every reachability/activation contract in tests/test_frontend_no_pointer.c
 * is written against them -- but they are no longer one bottom strip:
 *   CATEGORY  a compact toggle in the header's right half (Textures / Sounds),
 *             the DISCLOSED adaptation for the sound category the preview does
 *             not have (GATE2-VISUAL.md line 3);
 *   APPLY     the composition's primary button;
 *   DONE      the footer's "Back" action, drawn as an approved glyph prompt
 *             and clickable there. Its Close/HOME semantics are unchanged.
 */
enum texpacks_button {
	TEXPACKS_BUTTON_CATEGORY,
	TEXPACKS_BUTTON_APPLY,
	TEXPACKS_BUTTON_DONE,
	TEXPACKS_BUTTON_COUNT,
};

/* (The canvas numbers themselves are declared above screen_texpacks_reset --
 * it sizes the list viewport from TP_LIST_TOP/TP_LIST_BOTTOM/TP_ROW_PITCH, so
 * they have to exist before it, not after. Values unchanged.) */

/* The one canvas->pixel mapping, so render and hit test cannot round apart.
 * (The struct is declared above, where screen_texpacks_reset needs it.) */
static struct texpacks_canvas texpacks_canvas_of(int width, int height) {
	struct texpacks_canvas c;
	c.u = height > 0 ? (float)height / 480.0F : 1.0F;
	int canvas = (int)(640.0F * c.u + 0.5F);
	c.ox = (width - canvas) / 2;
	if(c.ox < 0)
		c.ox = 0;
	return c;
}

static int tp_d(const struct texpacks_canvas* c, int v) {
	return (int)((float)v * c->u + 0.5F);
}

static int tp_x(const struct texpacks_canvas* c, int v) {
	return c->ox + tp_d(c, v);
}

/* The category SELECTOR's label is the category it is showing; pressing it
 * toggles (unchanged semantics -- only its place in the composition moved). */
static const char* texpacks_button_label(int button) {
	switch(button) {
		case TEXPACKS_BUTTON_CATEGORY:
			return category == TEXPACKS_CAT_SOUNDS ? "Sounds" : "Textures";
		case TEXPACKS_BUTTON_APPLY: return "Apply Pack";
		default: return "Back";
	}
}

/* The approved footer: three prompts (Move / Select / Back) built from the
 * ORIGINAL gutil helpers and the DETECTED bindings -- no forked glyph art, no
 * invented binding. Measured, then centred on the canvas, so the Back prompt's
 * clickable rect is exactly the prompt the player can see. */
#define TP_FOOTER_HINTS 3
static const enum input_button texpacks_footer_button[TP_FOOTER_HINTS] = {
	IB_GUI_UP, IB_GUI_CLICK, IB_INVENTORY,
};
static const char* const texpacks_footer_label[TP_FOOTER_HINTS] = {
	"Move", "Select", "Back",
};

/* The centred hint row (controller UI redesign 10-31, A1 helper). Back is
 * the DONE target: its hit rect is the row's own Back cell (drawn == hit). */
static struct gutil_hint_row texpacks_footer_layout(int width, int height) {
	struct input_player_frame prompts;
	input_prompt_frame(0, &prompts);
	struct gutil_hint_item items[TP_FOOTER_HINTS];
	for(int k = 0; k < TP_FOOTER_HINTS; k++)
		items[k] = (struct gutil_hint_item) {
			{texpacks_footer_button[k]}, 1, texpacks_footer_label[k], k,
			k == TP_FOOTER_HINTS - 1};
	struct gutil_hint_row row;
	(void)gutil_hint_row_layout(&prompts, 0, 0, width, height, false,
								GUTIL_HINT_ALIGN_CENTRE, items,
								TP_FOOTER_HINTS, &row);
	return row;
}

static struct gutil_button_bounds texpacks_button_rect(int width, int height,
											int button) {
	struct texpacks_canvas c = texpacks_canvas_of(width, height);
	switch(button) {
		case TEXPACKS_BUTTON_CATEGORY:
			/* Right-aligned on the panel's own right edge, so it uses the full
			 * header width the heading leaves free and cannot reach the title. */
			return (struct gutil_button_bounds) {
				tp_x(&c, TP_PANEL_X + TP_PANEL_W - TP_CAT_W), tp_d(&c, TP_CAT_Y),
				tp_d(&c, TP_CAT_W), tp_d(&c, TP_CAT_H)};
		case TEXPACKS_BUTTON_APPLY:
			return (struct gutil_button_bounds) {
				tp_x(&c, TP_APPLY_X), tp_d(&c, TP_APPLY_Y), tp_d(&c, TP_APPLY_W),
				tp_d(&c, TP_APPLY_H)};
		default: {
			/* The footer's Back prompt -- its drawn extent, so pointing at the
			 * glyph the player sees is what activates it. */
			struct gutil_hint_row f = texpacks_footer_layout(width, height);
			for(int k = 0; k < f.count; k++)
				if(f.cells[k].item == TP_FOOTER_HINTS - 1)
					return f.cells[k].rect;
			return (struct gutil_button_bounds) {0, 0, 0, 0};
		}
	}
}

/* ===== THE PACK CARD's own geometry =====================================
 *
 * The approved card is 48..592 wide; widget_slotlist's SHARED hit band is
 * Beta's GuiSlot width/2 +/- 110 units (100..540 at scale 2), which leaves 52
 * px of dead art down each edge of the card -- the pack tile and the "Current"
 * badge are both drawn in it. This screen therefore draws and hit-tests its
 * rows through the SAME local rect function, and the shared widget is left
 * untouched for the three screens that still use its geometry.
 *
 * The slotlist itself is kept for what it is good at and what the landed
 * contracts test: the viewport, the count, the selection, the reveal, the page
 * roles and pointer drag-scroll. Only its 220-unit RENDERER and its 220-unit
 * hit band are bypassed, and only here.
 *
 * SHORT LISTS: slotlist_clamp deliberately leaves a NEGATIVE amount_scrolled
 * (Beta's centred-content offset) when the content is shorter than the
 * viewport. The approved composition starts its cards at the top of the panel,
 * so the negative offset is ignored -- in BOTH the render and the hit test,
 * through this one helper, so they cannot disagree. */
static int texpacks_scroll(void) {
	int scroll = slotlist_amount_scrolled(&pack_slots);
	return scroll > 0 ? scroll : 0;
}

static struct fep_rect texpacks_row_rect(int width, int height, size_t row) {
	struct texpacks_canvas c = texpacks_canvas_of(width, height);
	/* slotlist's own row placement: viewport top + its 4-unit inset, then the
	 * pitch. Read from the list so the two cannot drift. */
	int y = pack_slots.top + 4 * GFX_GUI_SCALE - texpacks_scroll()
		+ (int)row * pack_slots.row_height;
	return fep_rect(tp_x(&c, TP_ROW_X), y, tp_d(&c, TP_ROW_W),
					tp_d(&c, TP_ROW_ACTION));
}

/* The row under (x, y), or -1. The 10 px GAP between cards is deliberately
 * NOT activatable (it is not drawn as a card), and a press outside the
 * viewport is outside the list however the rows happen to be scrolled. */
static int texpacks_row_at(int width, int height, float x, float y) {
	if(y < (float)pack_slots.top || y > (float)pack_slots.bottom)
		return -1;
	for(size_t row = 0; row < row_count; row++) {
		struct fep_rect r = texpacks_row_rect(width, height, row);
		if(r.y + r.h <= pack_slots.top || r.y >= pack_slots.bottom)
			continue;
		if(fep_rect_hit(r, x, y))
			return (int)row;
	}
	return -1;
}

static int texpacks_pointer_button(int width, int height) {
	if(!pointer_available)
		return -1;
	for(int button = 0; button < TEXPACKS_BUTTON_COUNT; button++) {
		struct gutil_button_bounds bounds
			= texpacks_button_rect(width, height, button);
		if(gutil_button_hit(&bounds, pointer_x, pointer_y))
			return button;
	}
	return -1;
}

/* ONE id space (Astra ruling 2): the two strip buttons keep their enum values,
 * a pack ROW is TEXPACKS_BUTTON_COUNT + its index. */
#define TEXPACKS_TARGET_ROW(i) (TEXPACKS_BUTTON_COUNT + (int)(i))

static bool texpacks_target_is_row(int target) {
	return target >= TEXPACKS_BUTTON_COUNT;
}

/* The pack index a visible row addresses, or MAX_PACKS if there is none. */
static size_t texpacks_row_pack(size_t row) {
	return row < row_count ? rows[row] : (size_t)MAX_PACKS;
}

/* The slot holding (or remembering the miss for) `pack`, or NULL. */
static struct texpacks_preview* texpacks_preview_of(size_t pack) {
	if(pack >= MAX_PACKS)
		return NULL;
	for(size_t k = 0; k < TEXPACKS_PREVIEW_SLOTS; k++)
		if(previews[k].pack == pack)
			return &previews[k];
	return NULL;
}

/* Drop every image. The GPU may still be reading one of them -- this screen's
 * update runs immediately before gfx_flip_buffers, exactly like tex_init's own
 * re-entrant reload -- so the free is bracketed by the SAME fence tex_init
 * uses, and only when there is really something bound to fence against. */
static void texpacks_previews_clear(void) {
	bool live = false;
	for(size_t k = 0; k < TEXPACKS_PREVIEW_SLOTS; k++)
		live = live || previews[k].loaded;
	if(live)
		tex_init_pre();
	for(size_t k = 0; k < TEXPACKS_PREVIEW_SLOTS; k++) {
		tex_gfx_clear(&previews[k].tex);
		previews[k].pack = MAX_PACKS;
		previews[k].loaded = false;
	}
	if(live)
		tex_init_post();
}

/* Reconcile the cache with what the viewport currently shows. Called from the
 * reset and once per update -- NEVER from render2D. A frame where the visible
 * pack identities are unchanged does no I/O, no allocation and no GX work: it
 * walks at most row_count rects and four slots and finds every wanted pack
 * already present (a remembered miss counts as present). */
static void texpacks_previews_sync(int width, int height) {
	size_t want[TEXPACKS_PREVIEW_SLOTS];
	size_t want_count = 0;
	for(size_t row = 0; row < row_count && want_count < TEXPACKS_PREVIEW_SLOTS;
		row++) {
		struct fep_rect r = texpacks_row_rect(width, height, row);
		if(r.y + r.h <= pack_slots.top || r.y >= pack_slots.bottom)
			continue; /* the same visibility test the render clips with */
		size_t idx = texpacks_row_pack(row);
		if(idx < pack_count)
			want[want_count++] = idx;
	}

	/* EVICT first, so a slot is free for whatever scrolled in. */
	bool fenced = false;
	for(size_t k = 0; k < TEXPACKS_PREVIEW_SLOTS; k++) {
		if(previews[k].pack >= MAX_PACKS)
			continue;
		bool keep = false;
		for(size_t w = 0; w < want_count; w++)
			keep = keep || want[w] == previews[k].pack;
		if(keep)
			continue;
		if(previews[k].loaded && !fenced) {
			tex_init_pre(); /* the frame in flight may still read this image */
			fenced = true;
		}
		tex_gfx_clear(&previews[k].tex);
		previews[k].pack = MAX_PACKS;
		previews[k].loaded = false;
	}

	/* Then load what is newly visible, into slots that are provably empty (so
	 * no load can free a texture the GPU is using -- the evictions above are
	 * the only frees, and they are behind the fence). */
	for(size_t w = 0; w < want_count; w++) {
		if(texpacks_preview_of(want[w]))
			continue; /* cached image, or a cached MISS: no repeat I/O */
		for(size_t k = 0; k < TEXPACKS_PREVIEW_SLOTS; k++) {
			if(previews[k].pack < MAX_PACKS)
				continue;
			/* THIS row's own installed root, resolved the way the directory
			 * scan above resolves it (HBC-direct CWD is not the SD root). The
			 * active pack's config key is never consulted. */
			const char* root = string_get_cstr(packs[want[w]].value);
#ifdef PLATFORM_WII
			/* path_from_sd_root's semantic, without its fixed buffer: a folder
			 * name long enough to overflow a stack char[] would be TRUNCATED
			 * there, and a truncated root either misses or -- worse -- names a
			 * DIFFERENT installed pack, so the card would show the wrong art.
			 * An already-anchored path ("dev:/..." or "/...") is launcher-
			 * independent already and is passed through untouched; a relative
			 * one gets the literal "sd:/" prefix in an owned string, cleared
			 * the moment the loader returns (it copies nothing). */
			string_t anchored;
			bool anchored_owned = !strchr(root, ':') && root[0] != '/';
			if(anchored_owned) {
				string_init_printf(anchored, "sd:/%s", root);
				root = string_get_cstr(anchored);
			}
#endif
			previews[k].pack = want[w];
			previews[k].loaded = tex_preview_load(&previews[k].tex, root);
#ifdef PLATFORM_WII
			if(anchored_owned)
				string_clear(anchored);
#endif
			break;
		}
	}
	if(fenced)
		tex_init_post();
}

/* ---- BG-BAKE-1028/3: the pack's generated menu backdrop -----------------
 * A non-default pack without its own gui/menu-background.png shows its own
 * dirt (gutil_bg) until one bake has made and published its backdrop.
 * The job:
 *   - is QUEUED by a texture Apply (title or in-game) and once per boot for
 *     the already-selected pack, only when the cache says MISSING/STALE/
 *     CORRUPT (author art and a verified cache never bake);
 *   - STARTS only when main reports the title with no world alive (an
 *     in-game Apply therefore waits for the title);
 *   - follows the configured pack: a pack change drops/cancels the old job;
 *   - on failure keeps a visible reason and stops: no automatic retry. The
 *     player retries explicitly with Apply Pack on the same pack (the
 *     button is enabled for exactly that). A bake interrupted only because
 *     the player left the title (world-alive refusal) resumes at the next
 *     title, which is the deferral, not a retry. */
enum texpacks_bd {
	TEXPACKS_BD_IDLE,
	TEXPACKS_BD_QUEUED,
	TEXPACKS_BD_BAKING,
	TEXPACKS_BD_FAILED,
};
static const struct backdrop_job_hooks* s_bd_hooks;
static enum texpacks_bd s_bd;
static char s_bd_pack[256];  /* paths.texturepack the job is for */
static char s_bd_reason[96]; /* visible failure reason */
static bool s_bd_boot_checked;
static bool s_bd_reload_pending;
static bool s_apply_wait;
static bool s_apply_running, s_apply_failure_audio;

void screen_texturepacks_set_backdrop_hooks(const struct backdrop_job_hooks* h);
void screen_texturepacks_set_backdrop_hooks(const struct backdrop_job_hooks* h) {
	s_bd_hooks = h;
}

static const char* texpacks_bd_current_pack(void) {
	return config_read_string(&gstate.config_user, "paths.texturepack",
							  TEXPACKS_DEFAULT);
}

static bool texpacks_bd_needed(void) {
	/* texture.c enum tex_backdrop_state: MISSING 3, STALE 4, CORRUPT 5. */
	return s_bd_hooks && s_bd_hooks->state() >= 3;
}

static void texpacks_bd_drop_bake(void) {
	if(s_bd == TEXPACKS_BD_BAKING && s_bd_hooks)
		s_bd_hooks->bake_release();
}

/* Queue a bake for the CURRENT pack if it needs one; otherwise go idle. */
static void texpacks_bd_queue(void) {
	if(!s_bd_hooks)
		return;
	texpacks_bd_drop_bake();
	s_bd_reload_pending = false;
	s_bd_reason[0] = '\0';
	snprintf(s_bd_pack, sizeof(s_bd_pack), "%s", texpacks_bd_current_pack());
	s_bd = texpacks_bd_needed() ? TEXPACKS_BD_QUEUED : TEXPACKS_BD_IDLE;
}

static void texpacks_bd_fail(const char* reason) {
	s_bd = TEXPACKS_BD_FAILED;
	snprintf(s_bd_reason, sizeof(s_bd_reason), "%s", reason);
}

enum backdrop_job_readiness screen_texturepacks_backdrop_ready_state(void) {
	if(s_bd == TEXPACKS_BD_FAILED) return BACKDROP_JOB_FAILED;
	if(!s_bd_hooks || !s_bd_boot_checked || s_bd != TEXPACKS_BD_IDLE || s_bd_reload_pending
	   || !s_bd_hooks->uploaded || !s_bd_hooks->retired)
		return BACKDROP_JOB_LOADING;
	if(!s_bd_hooks->uploaded()) return BACKDROP_JOB_FAILED;
	return s_bd_hooks->retired() ? BACKDROP_JOB_READY : BACKDROP_JOB_LOADING;
}

const char* screen_texturepacks_backdrop_failure_reason(void) {
	if(s_bd_reason[0]) return s_bd_reason;
	if(screen_texturepacks_backdrop_ready_state() == BACKDROP_JOB_FAILED)
		return "The background couldn't be uploaded.";
	return "";
}

void screen_texturepacks_backdrop_retry(void) {
	texpacks_bd_queue();
	/* A valid cache/author image may have failed only its upload. Retry that
	 * too, after sounds, rather than declaring an idle job completed. */
	s_bd_reload_pending = s_bd_hooks && s_bd == TEXPACKS_BD_IDLE;
}
void screen_texturepacks_backdrop_cancel(void) {
	texpacks_bd_drop_bake();
	s_bd_reload_pending = false;
	texpacks_bd_fail("Making the background was cancelled.");
	s_apply_wait = false;
}

static const char* texpacks_bd_bake_reason(enum backdrop_bake_error e) {
	switch(e) {
		case BACKDROP_BAKE_ERR_ALLOC: return "Not enough memory to make the background.";
		case BACKDROP_BAKE_ERR_NOT_READY: return "The background scene didn't finish loading.";
		case BACKDROP_BAKE_ERR_CAPTURE: return "The background picture couldn't be read.";
		case BACKDROP_BAKE_ERR_CANCELLED: return "Making the background was cancelled.";
		default: return "The background couldn't be made.";
	}
}

/* Once per frame from main. `at_title_idle`: the title screen is current and
 * no world is alive -- the only state a bake may start or run in. */
void screen_texturepacks_backdrop_service(bool at_title_idle);
void screen_texturepacks_backdrop_service(bool at_title_idle) {
	if(!s_bd_hooks)
		return;
	/* The background is the next phase, never concurrent with sound loading. */
	if(s_bd == TEXPACKS_BD_BAKING && !at_title_idle) {
		texpacks_bd_drop_bake();
		s_bd = TEXPACKS_BD_QUEUED;
		return; /* Never publish a completed optional scene after world admission. */
	}
	if(s_bd_hooks->sounds_state
	   && s_bd_hooks->sounds_state() != AUDIO_REGISTRY_READY
	   && s_bd_hooks->sounds_state() != AUDIO_REGISTRY_DISABLED) return;
	if(!s_bd_boot_checked && at_title_idle) {
		/* An already-selected pack without a valid backdrop (first boot after
		 * install, a cache lost to a crash, a pack edited on the PC). */
		s_bd_boot_checked = true;
		if(s_bd == TEXPACKS_BD_IDLE)
			texpacks_bd_queue();
	}
	if(s_bd != TEXPACKS_BD_IDLE
	   && strcmp(s_bd_pack, texpacks_bd_current_pack()) != 0) {
		/* The pack changed under the job (a later Apply already queued
		 * through texpacks_bd_queue; this covers any other config change). */
		texpacks_bd_queue();
	}
	if(s_bd_reload_pending && at_title_idle) {
		s_bd_reload_pending = false;
		s_bd_hooks->reload();
		if(s_bd_hooks->uploaded && !s_bd_hooks->uploaded())
			texpacks_bd_fail("The background couldn't be uploaded.");
	}
	/* A scene deferred during boot/Apply becomes eligible at the title. */
	if(s_bd == TEXPACKS_BD_IDLE && at_title_idle && texpacks_bd_needed())
		texpacks_bd_queue();
	if(s_bd == TEXPACKS_BD_QUEUED && at_title_idle) {
		/* A replaced/cancelled bake can still own mesher-visible scratch. */
		if(s_bd_hooks->retired && !s_bd_hooks->retired()) return;
		if(!texpacks_bd_needed()) {
			s_bd = TEXPACKS_BD_IDLE;
		} else if(s_bd_hooks->bake_start()) {
			s_bd = TEXPACKS_BD_BAKING;
		} else {
			texpacks_bd_fail("The background couldn't be started.");
		}
		return;
	}
	if(s_bd != TEXPACKS_BD_BAKING)
		return;
	enum backdrop_bake_state st = s_bd_hooks->bake_poll();
	if(st == BACKDROP_BAKE_DONE) {
		uint8_t* px = s_bd_hooks->bake_take();
		s_bd_hooks->bake_release();
		if(!px) {
			texpacks_bd_fail("The background picture was lost.");
			return;
		}
		/* Encode + SD write: a declared multi-second main-thread stall. */
		watchdog_phase_main(WDM_PACK_RELOAD);
		watchdog_expect_stall_ms(TEXPACKS_RELOAD_GRACE_MS);
		int r = s_bd_hooks->publish(px);
		s_bd_hooks->pixels_free(px);
		watchdog_phase_main(WDM_SCREEN_UPDATE);
		if(r != 0) {
			texpacks_bd_fail(s_bd_hooks->publish_reason(r));
			return;
		}
		s_bd_hooks->reload();
		if(s_bd_hooks->uploaded && !s_bd_hooks->uploaded()) {
			texpacks_bd_fail("The background couldn't be uploaded.");
			return;
		}
		/* The same title job handles the next missing scene after retirement. */
		s_bd = texpacks_bd_needed() ? TEXPACKS_BD_QUEUED : TEXPACKS_BD_IDLE;
		s_bd_reason[0] = '\0';
	} else if(st == BACKDROP_BAKE_FAILED || st == BACKDROP_BAKE_CANCELLED) {
		enum backdrop_bake_error e = s_bd_hooks->bake_error();
		s_bd_hooks->bake_release();
		if(e == BACKDROP_BAKE_ERR_WORLD_ALIVE)
			s_bd = TEXPACKS_BD_QUEUED; /* left the title: resume there */
		else
			texpacks_bd_fail(texpacks_bd_bake_reason(e));
	}
}

/* The active texture pack's backdrop failed: Apply Pack on that same pack is
 * the explicit retry. */
static bool texpacks_bd_retry_offered(size_t choice) {
	return s_bd == TEXPACKS_BD_FAILED
		&& (category == TEXPACKS_CAT_TEXTURES || s_apply_failure_audio)
		&& choice < pack_count
		&& texpacks_value_eq(string_get_cstr(packs[choice].value),
							 texpacks_current_value(category));
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
int screen_texturepacks_test_backdrop_job(void);
int screen_texturepacks_test_backdrop_job(void) { return (int)s_bd; }
const char* screen_texturepacks_test_backdrop_reason(void);
const char* screen_texturepacks_test_backdrop_reason(void) { return s_bd_reason; }
void screen_texturepacks_test_backdrop_reset(void);
void screen_texturepacks_test_backdrop_reset(void) {
	s_bd = TEXPACKS_BD_IDLE;
	s_bd_reason[0] = s_bd_pack[0] = '\0';
	s_bd_boot_checked = false;
	s_bd_reload_pending = false;
	s_apply_wait = false;
	s_apply_running = s_apply_failure_audio = false;
}
#endif

/* Would Apply do anything? Only if a pending choice exists AND it is not
 * already what this category uses -- the same normalisation the transaction
 * itself applies, so a disabled Apply and a NO_CHANGE result cannot disagree.
 * Frozen while a deferred apply is in flight (gate 1 item 7). */
static bool texpacks_apply_enabled(void) {
	if(apply_pending_frames > 0 || s_apply_wait)
		return false;
	size_t choice = pending[category];
	if(choice >= pack_count)
		return false;
	if(texpacks_bd_retry_offered(choice))
		return true; /* BG-BAKE-1028/3: explicit backdrop retry */
	return !texpacks_value_eq(string_get_cstr(packs[choice].value),
							  texpacks_current_value(category));
}

/* In-game the SOUNDS category is unavailable: the live sound and music sets
 * are not swapped under a running world (Astra ui9 / gate 1 item 6), and a
 * selection recorded as "current" that only takes effect next launch would be
 * a lie. The selector is therefore a DISABLED target in-game, which under the
 * activation rule consumes its own press instead of falling through to the
 * selected pack. */
static bool texpacks_target_enabled(int target) {
	if(target == GUTIL_MENU_TARGET_NONE)
		return false;
	if(texpacks_target_is_row(target))
		return (size_t)(target - TEXPACKS_BUTTON_COUNT) < row_count;
	switch(target) {
		case TEXPACKS_BUTTON_CATEGORY: return !ingame_mode;
		case TEXPACKS_BUTTON_APPLY: return texpacks_apply_enabled();
		case TEXPACKS_BUTTON_DONE: return true;
		default: return false;
	}
}

/* The first / next ENABLED strip button, skipping disabled ones and clamping
 * at both ends -- screen_select_world.c's sworld_focus_first/_step for this
 * screen's two-button strip (review A16-10). "Open texture pack folder" is
 * permanently disabled, so the first enabled button is Done and LEFT clamps
 * there rather than dropping the ring back into the pack list. The strip is
 * walked in its declaration order, which IS its visual order here (Open on
 * the left, Done on the right -- texpacks_button_rect). Returns -1 only if
 * NOTHING is enabled, which cannot happen (Done is unconditional) but is the
 * "stay in the list" answer if it ever did. */
static int texpacks_focus_first(void) {
	for(int button = 0; button < TEXPACKS_BUTTON_COUNT; button++)
		if(texpacks_target_enabled(button))
			return button;
	return -1;
}

static int texpacks_focus_step(int button, int dir) {
	for(int k = button + dir; k >= 0 && k < TEXPACKS_BUTTON_COUNT; k += dir)
		if(texpacks_target_enabled(k))
			return k;
	return button; /* clamp at both ends; the strip does not wrap */
}

/* The target the ring rests on -- a strip button if the pointer has put it
 * there, otherwise the selected row (NONE on an empty list). The RENDER reads
 * this same function, so the highlight and the decision cannot drift
 * (A3 NEW-7). */
static int texpacks_focus_target(void) {
	if(texpacks_focus_button >= 0)
		return texpacks_focus_button;
	return gui_selection < row_count ? TEXPACKS_TARGET_ROW(gui_selection)
									 : GUTIL_MENU_TARGET_NONE;
}

/* The target the SPRITE rests on. */
static int texpacks_pointer_target(void) {
	if(!pointer_available)
		return GUTIL_MENU_TARGET_NONE;
	if(pointer_button >= 0)
		return pointer_button;
	/* GATE 2: the LOCAL card hit test, not widget_slotlist's shared 220-unit
	 * band -- the approved card is 544 px wide and the pack tile and "Current"
	 * badge live in the 52 px the shared band would drop on each side. */
	int row = texpacks_row_at(gfx_width(), gfx_height(), pointer_x, pointer_y);
	return row < 0 ? GUTIL_MENU_TARGET_NONE : TEXPACKS_TARGET_ROW(row);
}

static void texpacks_done(void) {
	/* [740c583 GuiScreen.mouseClicked / GuiTexturePacks.actionPerformed id 6]:
	 * click before returning to the parent. */
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
	/* Nothing of this screen outlives it: the four preview images go back to
	 * the heap here, fenced, rather than sitting in MEM2 for the whole game. */
	texpacks_previews_clear();
	screen_texpacks_closed = true;
	ingame_mode = false;
	screen_set(screen_texpacks_return);
}

/* The EFFECTIVE audio root, resolved exactly the way audio_registry_load()
 * and audio_music_init() resolve it: the independent paths.audiopack choice,
 * falling back to paths.texturepack for legacy configs. Copied out, because
 * texturepacks_apply() rewrites the JSON tree the returned pointer lives in. */
static void texpacks_audio_selection(char* out, size_t size) {
	const char* legacy = config_read_string(&gstate.config_user,
										"paths.texturepack", TEXPACKS_DEFAULT);
	snprintf(out, size, "%s",
			 config_read_string(&gstate.config_user, "paths.audiopack", legacy));
}

/* Production applies the real texture loader and the real config file. Under
 * CAVEWII_TESTING both are re-pointable so tests/test_pack_helper_audio.c can
 * drive THIS function -- the real texpacks_value_eq normalisation, the real
 * !ingame_mode gate, the real watchdog brackets -- instead of hand-mirroring
 * it (2026-09-17 DELTA review, test gap (a)). Dormant by default and compiled
 * out of every shipping binary, the displaylist_test_fail_* precedent
 * (source/platform/displaylist.h:180-184). */
#ifdef CAVEWII_TESTING
static texturepack_reload_fn s_test_reload = tex_init;
static const char* s_test_config_path = "config.json";
#define TEXPACKS_RELOAD_FN s_test_reload
#define TEXPACKS_CONFIG_PATH s_test_config_path
#else
#define TEXPACKS_RELOAD_FN tex_init
#define TEXPACKS_CONFIG_PATH "config.json"
#endif

/* ===== observer telemetry (real-Wii audio-switch evidence) ================
 * Astra gate 3 check (3): PACK_RELOAD's wall-clock with an audio-carrying pack
 * must be shown to stay inside TEXPACKS_RELOAD_GRACE_MS, on the console, not
 * inferred from a heartbeat gap. One record per apply carries the whole
 * transaction: what was asked for, whether it stood, where the audio selection
 * was before / after the transaction / after the music safety net, whether the
 * arena was re-decoded at all, and the total the watchdog grace is measured
 * against. Same gate/sink/escaper shape as audio_registry.c and audio_music.c;
 * they are three copies because none of their headers is part of this change.
 * Inert unless telemetry is armed -- the gate is tested before any formatting,
 * and the apply's own clock reads are two time_get() calls. */
#ifdef CAVEWII_TESTING
static bool (*s_observer_sink)(const char* line);
void screen_texturepacks_test_set_observer(bool (*sink)(const char* line));
void screen_texturepacks_test_set_observer(bool (*sink)(const char* line)) {
	s_observer_sink = sink;
}
#endif

static bool texpacks_tel_armed(void) {
#ifdef CAVEWII_TESTING
	if(s_observer_sink)
		return true;
#endif
	return hw_telemetry_active() && !hw_telemetry_quiesced();
}

static bool texpacks_tel_emit(const char* line) {
#ifdef CAVEWII_TESTING
	if(s_observer_sink)
		return s_observer_sink(line);
#endif
	return hw_telemetry_emit_observer(line);
}

static size_t texpacks_tel_json_str(char* dst, size_t size, size_t off,
									const char* src) {
	if(off + 1 >= size)
		return size;
	dst[off++] = '"';
	for(const char* p = src; p && *p; p++) {
		char esc[7];
		size_t n;
		if(*p == '"' || *p == '\\') {
			esc[0] = '\\';
			esc[1] = *p;
			n = 2;
		} else if((unsigned char)*p < 0x20) {
			n = (size_t)snprintf(esc, sizeof(esc), "\\u%04x",
								 (unsigned)(unsigned char)*p);
		} else {
			esc[0] = *p;
			n = 1;
		}
		if(off + n + 1 >= size)
			return size;
		memcpy(dst + off, esc, n);
		off += n;
	}
	dst[off++] = '"';
	return off;
}

/* One key=<string> member; keeps the four root fields below readable. */
static size_t texpacks_tel_kv(char* dst, size_t size, size_t off,
							  const char* key, const char* value) {
	off += (size_t)snprintf(dst + off, size > off ? size - off : 0, ",\"%s\":",
							key);
	if(off >= size)
		return size;
	return texpacks_tel_json_str(dst, size, off, value);
}

/* Lane C "Loading sounds..." (Astra ruling 1, c3-loading-sounds-0919), the
 * pack-switch half. The strip the player saw while the TEXTURES reloaded said
 * "Loading texture pack..."; the sound re-decode that follows blocks this
 * thread for another ~31 s on the console and said nothing. Called ONLY on the
 * branch that really re-decodes (after the final audio-root selection).
 *
 * It redraws a VALID FULL FRAME of this screen with the textures as they are
 * NOW (the reload just replaced them; the previous frame in the draw buffer
 * is not assumed to survive) and REALLY presents it -- the main loop's own
 * finish + flip pair, since a bare finish only copies the EFB out. The buffer
 * the flip hands back is an older image, and the main loop's next act is to
 * flip it to the TV: paint the same frame into it too, so the strip cannot
 * flicker back to a three-frames-old picture when the reload returns. */
static bool s_sound_phase;
static void screen_texpacks_render2D(struct screen* s, int width, int height);

static void texpacks_paint_sound_phase_frame(bool capture) {
	gfx_clear_buffers(128, 128, 128); /* the main loop's menu clear */
	gfx_mode_gui();
	screen_texpacks_render2D(&screen_texturepacks, gfx_width(), gfx_height());
	if(capture)
		loading_notice_capture("reload"); /* debug proof capture; off by default */
	gfx_finish(true);
}

static void texpacks_present_sound_phase(void) {
	if(!gfx_can_present())
		return; /* headless unit test: nothing to draw on (gfx.h) */
	float gpu_wait = 0.0F, vsync_wait = 0.0F;
	s_sound_phase = true;
	texpacks_paint_sound_phase_frame(true);
	gfx_flip_buffers(&gpu_wait, &vsync_wait);
	texpacks_paint_sound_phase_frame(false);
	/* gfx_flip_buffers left its own GFX phase tag behind; the ~31 s that
	 * follow are the DECLARED pack reload, and a wedge in them must be
	 * reported as one (hostile review 03 #1). The grace is untouched. */
	watchdog_phase_main(WDM_PACK_RELOAD);
	s_sound_phase = false; /* the NEXT loop frame draws the screen normally */
}

/* The audio root this apply STARTED from -- the callback's only state, set
 * once per apply and never carried across one. */
static char s_apply_audio_before[256];
static bool s_audio_music_reloaded, s_audio_sound_reloaded;
static int s_audio_tracks;

/* The AUDIO half of the transaction, handed to texturepacks_apply_audio the
 * same way tex_init is handed to the texture half: the transaction has already
 * written the tentative paths.audiopack, and both registries resolve their root
 * from the config exactly as they do at boot.
 *
 * Music FIRST, because it is the cheap one (a file-existence scan, no decode)
 * and the only one that can lose everything -- audio_music's fallback root is
 * the bundled "assets", which ships no music at all. Returning false REJECTS
 * the root: the transaction restores the exact previous JSON, persists nothing,
 * and re-runs this callback so the previous root is live again. That replaces
 * the old "report success, then quietly put the selection back" fallback.
 *
 * Thread-safety unchanged: audio_music_registry_reload() calls audio_music_stop()
 * first (clears the worker's pending path, bumps s_ring.gen under s_ring.lock),
 * and audio_registry_reload() stops every voice under the mixer lock before it
 * touches one arena byte. Main thread, no new threads. */
static bool texpacks_audio_reload(void) {
	char root[256];
	texpacks_audio_selection(root, sizeof(root));
	/* WHICH CALL IS THIS? The transaction runs the callback twice on a failure:
	 * once for the pick, then once more after restoring the exact previous
	 * JSON. The restore re-run is the only one whose root equals the root this
	 * apply started from, and the two have opposite jobs:
	 *   - the PICK is validated and may be refused;
	 *   - the RESTORE must put the previous runtime back, whatever that pack
	 *     looks like. It is already what the player had; refusing it here
	 *     would leave the NEW effects live behind a failed apply, which is the
	 *     bug REVIEW-SCREEN-1 (HIGH) found. */
	bool restoring = texpacks_value_eq(root, s_apply_audio_before);

	audio_music_registry_reload();
	s_audio_music_reloaded = true;
	/* Everything the player can actually hear from this root: the Beta list
	 * plus the creative playlist, which is registered separately and was being
	 * counted as zero (REVIEW-SCREEN-1, MEDIUM) -- a creative-only pack is a
	 * legitimate music pack. Records are deliberately NOT counted: they are
	 * jukebox items, not background music, and a records-only pack ships no
	 * music/ directory so it is never asked this question. */
	s_audio_tracks
		= audio_music_track_count() + audio_music_creative_track_count();
	/* A pack that CLAIMS music (it ships a music/ directory) and resolves no
	 * playable track is rejected. A pack that claims none -- the bundled
	 * default, a sound-only pack -- is a legitimate choice that simply has no
	 * music, and the screen says so rather than refusing it. */
	if(!restoring && s_audio_tracks == 0 && texturepacks_claims_music(root)) {
		fprintf(stderr,
				"[music] \"%s\" claims music but resolves no playable track; "
				"keeping the previous sound pack\n",
				root);
		return false;
	}
	/* The expensive half (voice drain + full arena re-decode, ~25 s from SD on
	 * Broadway) runs only when the root really moved -- and on the restore
	 * re-run only if this apply had already decoded the new root, so the
	 * player's old effects come back. Nothing is remembered between applies:
	 * the comparison is against the root THIS apply started from, which cannot
	 * go stale behind another caller's reload.
	 * The file-eligibility test that made this pack selectable proves files
	 * are PRESENT, not that every sample decodes; a sample that fails to
	 * decode falls back per key inside the registry, as before. */
	if(!restoring || s_audio_sound_reloaded) {
		texpacks_present_sound_phase();
		audio_registry_reload();
		s_audio_sound_reloaded = true;
	}
	return true;
}

static const char* texpacks_result_name(enum texturepacks_result r) {
	switch(r) {
		case TEXTUREPACKS_OK: return "ok";
		case TEXTUREPACKS_NO_CHANGE: return "no_change";
		case TEXTUREPACKS_INVALID: return "invalid";
		case TEXTUREPACKS_RELOAD_FAILED: return "reload_failed";
		case TEXTUREPACKS_SAVE_FAILED: return "save_failed";
	}
	return "unknown";
}

/* pack-lag-1030: a LIVE frame while the textures (and, at the title, the
 * sounds) reload. tex_init / audio_registry_reload block the main thread for
 * 0.8-2.6 s per switch on the console (35.6 s with the old opaque bake); the
 * TV showed one frozen black strip. main's loader service
 * (boot_gate_apply_service, every 66 ms) calls this while a reload is live.
 * The textures are being REPLACED in place, so nothing textured -- not even
 * the font -- may be sampled: the frame is the black strip band plus the
 * approved three-block activity indicator (loading_indicator.h, untextured),
 * stepped by the apply's own clock. Activity, not a fake percentage. */
static ptime_t s_reload_t0;
static bool s_reload_live;
static unsigned s_reload_frames;

void screen_texturepacks_reload_frame(void);
void screen_texturepacks_reload_frame(void) {
	if(!s_reload_live)
		return;
	s_reload_frames++;
	if(!gfx_can_present())
		return; /* headless unit test */
	int w = gfx_width(), h = gfx_height();
	float elapsed = (float)time_diff_ms(s_reload_t0, time_get()) / 1000.0F;
	gfx_clear_buffers(0, 0, 0);
	gfx_mode_gui();
	gfx_texture(false);
	int band_h = 20 * GFX_GUI_SCALE + 24;
	int band_y = h / 2 - band_h / 2;
	gutil_texquad_col(0, band_y, 0, 0, 0, 0, w, band_h, 0, 0, 0, 200);
	struct loading_indicator ind;
	loading_indicator_layout(w, h, elapsed, &ind);
	int y = band_y + band_h + 12 * GFX_GUI_SCALE;
	for(int k = 0; k < LOADING_INDICATOR_SQUARES; k++) {
		const struct loading_indicator_square* q = &ind.squares[k];
		int b = LOADING_INDICATOR_BORDER;
		unsigned rgb = k == ind.lit ? LOADING_INDICATOR_LIT_RGB
									: LOADING_INDICATOR_UNLIT_RGB;
		gutil_texquad_col(q->x - b, y - b, 0, 0, 0, 0, q->size + 2 * b,
						  q->size + 2 * b, (LOADING_INDICATOR_BORDER_RGB >> 16) & 0xFF,
						  (LOADING_INDICATOR_BORDER_RGB >> 8) & 0xFF,
						  LOADING_INDICATOR_BORDER_RGB & 0xFF, 255);
		gutil_texquad_col(q->x, y, 0, 0, 0, 0, q->size, q->size,
						  (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
	}
	gfx_texture(true);
	float gpu_wait = 0.0F, vsync_wait = 0.0F;
	gfx_finish(true);
	gfx_flip_buffers(&gpu_wait, &vsync_wait);
	watchdog_phase_main(WDM_PACK_RELOAD);
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
unsigned screen_texpacks_test_reload_frames(void);
unsigned screen_texpacks_test_reload_frames(void) { return s_reload_frames; }
#endif

static void texpacks_apply_now(size_t idx) {
	if(idx >= pack_count)
		return;
	if(texpacks_bd_retry_offered(idx)) {
		/* Explicit retry of this pack's failed backdrop: nothing is reloaded;
		 * the bake runs at the title. */
		if(s_apply_failure_audio && s_bd_hooks->sounds_retry
		   && !s_bd_hooks->sounds_retry()) return;
		s_apply_failure_audio = false;
		screen_texturepacks_backdrop_retry();
		s_apply_wait = !ingame_mode && s_bd_hooks && s_bd_hooks->sounds_state;
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		return;
	}
	s_apply_running = true;
	s_apply_failure_audio = false;
	const char* value = string_get_cstr(packs[idx].value);
	char audio_before[256];
	texpacks_audio_selection(audio_before, sizeof(audio_before));
	bool tel = texpacks_tel_armed();
	ptime_t apply_started = time_get();
	/* audio_after stays == audio_before unless the transaction moved the
	 * selection; audio_final unless the music safety net put it back. Both are
	 * seeded here so the record is complete on every path, including the
	 * in-game one that deliberately reloads nothing. Seeded unconditionally
	 * rather than behind `tel`: these two <=255-byte copies and one clock read
	 * are the whole cost a disarmed apply pays, on an operation that then
	 * spends seconds reloading every texture, and guarding them would trade
	 * that for a maybe-uninitialised read. */
	char audio_after[256], audio_final[256];
	snprintf(audio_after, sizeof(audio_after), "%s", audio_before);
	snprintf(audio_final, sizeof(audio_final), "%s", audio_before);
	s_audio_music_reloaded = false;
	s_audio_sound_reloaded = false;
	snprintf(s_apply_audio_before, sizeof(s_apply_audio_before), "%s",
			 audio_before);
	/* Declared long operation: every texture -- plus, when the new pack
	 * carries its own audio, the sound and music registries -- reloads
	 * synchronously here. The watchdog keeps its 3 s HANG
	 * rule for everything else; a freeze inside the reload still reports
	 * once the grace is spent. */
	watchdog_phase_main(WDM_PACK_RELOAD);
	watchdog_expect_stall_ms(TEXPACKS_RELOAD_GRACE_MS);
	s_reload_t0 = apply_started;
	s_reload_live = true; /* the loaders' service may now present frames */
	/* THE TWO INDEPENDENT CHOICES. A texture apply changes paths.texturepack
	 * (pinning a legacy absent/wrong-typed audio key to the root the player is
	 * already hearing, never overwriting an explicit one); an audio apply
	 * changes paths.audiopack and never reads or writes the texture choice.
	 * Neither can move the other's selection, whatever the pack directory
	 * happens to ship (gate 1 item 2). */
	enum texturepacks_result result;
	if(category == TEXPACKS_CAT_SOUNDS && ingame_mode) {
		/* The UI disables the Sounds category in-game; this is the second
		 * lock, on the transaction itself, so no caller can swap the live
		 * sound sets under a running world or persist a next-boot choice the
		 * player would see labelled as current (gate 1 item 6). Nothing is
		 * read, written or reloaded. */
		result = TEXTUREPACKS_INVALID;
	} else if(category == TEXPACKS_CAT_SOUNDS) {
		result = texturepacks_apply_audio(&gstate.config_user, value,
										  TEXPACKS_CONFIG_PATH,
										  texpacks_audio_reload);
	} else {
		result = texturepacks_apply_textures(&gstate.config_user, value,
											 TEXPACKS_CONFIG_PATH,
											 TEXPACKS_RELOAD_FN);
	}
	/* A re-import of the pack that is already current: same name, new files on
	 * the card. The transaction saw no change of choice, so reload here (the
	 * config already names it; nothing to write). */
	if(s_import_force_reload && result == TEXTUREPACKS_NO_CHANGE
	   && category == TEXPACKS_CAT_TEXTURES)
		result = TEXPACKS_RELOAD_FN() ? TEXTUREPACKS_OK
									  : TEXTUREPACKS_RELOAD_FAILED;
	if(s_import_force_reload && result == TEXTUREPACKS_NO_CHANGE
	   && category == TEXPACKS_CAT_SOUNDS) {
		/* The same reloads texpacks_audio_reload runs for a moved root:
		 * music scan first, then the drained, presented sound re-decode. */
		audio_music_registry_reload();
		s_audio_music_reloaded = true;
		texpacks_present_sound_phase();
		audio_registry_reload();
		s_audio_sound_reloaded = true;
		result = TEXTUREPACKS_OK;
	}
	s_import_force_reload = false;
	bool applied = result == TEXTUREPACKS_OK || result == TEXTUREPACKS_NO_CHANGE;
	if(category == TEXPACKS_CAT_TEXTURES) {
		/* The import's textures stood: its sounds are next. A failed texture
		 * half applies no sounds either. */
		s_import_audio_next = applied ? s_import_audio_planned : MAX_PACKS;
		s_import_audio_planned = MAX_PACKS;
	}
	/* BG-BAKE-1028/3: a texture pack without its own backdrop gets one baked
	 * at the title (in-game: deferred until the world is gone). */
	if(result == TEXTUREPACKS_OK && category == TEXPACKS_CAT_TEXTURES)
		texpacks_bd_queue();
	/* Once the task-3 adapter is registered, Apply stays opaque through the
	 * existing audio generation, any missing bake, upload AND retirement.
	 * In-game texture Apply retains the signed deferred-until-title policy. */
	if(applied && !ingame_mode && s_bd_hooks && s_bd_hooks->sounds_state)
		s_apply_wait = true;
	s_apply_running = false;
	last_result = result;
	last_result_valid = true;
	last_result_category = category;
	if(s_import_audio_running) {
		s_import_audio_running = false;
		if(applied) {
			/* Both halves stood: back to the Textures list, one sentence. */
			s_import_both_applied = true;
			category = TEXPACKS_CAT_TEXTURES;
			last_result_category = TEXPACKS_CAT_TEXTURES;
			texpacks_build_rows();
		}
	}
	texpacks_audio_selection(audio_after, sizeof(audio_after));
	snprintf(audio_final, sizeof(audio_final), "%s", audio_after);
	s_reload_live = false;
	watchdog_phase_main(WDM_SCREEN_UPDATE);
	/* The reload replaced every global texture object under us (tex_init, with
	 * its own fence). The previews are separate objects and survived it, but a
	 * pack apply is exactly the moment a stale row image would be noticed, so
	 * they are dropped here and re-read by the next sync -- four 40x40 reads
	 * after an operation that just spent seconds on the card. */
	texpacks_previews_clear();
	if(tel) {
		char line[1280];
		size_t off = (size_t)snprintf(line, sizeof(line),
									  "{\"event\":\"pack_apply\",\"pack\":");
		off = texpacks_tel_json_str(line, sizeof(line), off, value);
		off = texpacks_tel_kv(line, sizeof(line), off, "audio_before",
							  audio_before);
		off = texpacks_tel_kv(line, sizeof(line), off, "audio_after",
							  audio_after);
		off = texpacks_tel_kv(line, sizeof(line), off, "audio_final",
							  audio_final);
		off = texpacks_tel_kv(line, sizeof(line), off, "category",
							  category == TEXPACKS_CAT_SOUNDS ? "sounds"
															  : "textures");
		off = texpacks_tel_kv(line, sizeof(line), off, "result",
							  texpacks_result_name(result));
		if(off < sizeof(line)) {
			off += (size_t)snprintf(
				line + off, sizeof(line) - off,
				",\"applied\":%s,\"ingame\":%s,\"sound_reloaded\":%s,"
				"\"music_reloaded\":%s,\"tracks\":%d,\"total_ms\":%d,"
				"\"grace_ms\":%d,\"async_pending\":%s}",
				applied ? "true" : "false", ingame_mode ? "true" : "false",
				s_audio_sound_reloaded ? "true" : "false",
				s_audio_music_reloaded ? "true" : "false",
				audio_music_track_count(),
				(int)time_diff_ms(apply_started, time_get()),
				TEXPACKS_RELOAD_GRACE_MS,
				/* asyncsnd-0926: true when the sound half of this apply is
				 * still completing on the loader thread (opt-in on); world
				 * entry is gated until it finishes. */
				audio_registry_async_pending() ? "true" : "false");
			if(off < sizeof(line))
				(void)texpacks_tel_emit(line);
		}
	}
	if(!applied)
		apply_failed_frames = TEXPACKS_FAILED_FRAMES;
	// Click AFTER the reload on this one button (review F2): the drain
	// would truncate a click played first — and the confirmation should
	// come from the pack the player just applied anyway. Beta's
	// click-before-action rule [740c583: GuiScreen.mouseClicked:57-59]
	// stays intact on every non-reloading button (back, below).
	audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
}

#ifdef CAVEWII_TESTING
/* Test entry points (tests/test_pack_helper_audio.c). These do not
 * re-implement the seam -- they aim the two production-external dependencies
 * (the GL texture loader and the SD-root config path) at the test's own, then
 * run the REAL texpacks_apply_now on a one-entry pack list. Declared here
 * rather than in screen.h because no production caller may ever see them; the
 * test declares the matching externs itself (the audio_music_test_* shape). */
void screen_texturepacks_test_arm(const char* config_path,
								  texturepack_reload_fn reload);
bool screen_texturepacks_test_apply(const char* pack_value, bool ingame);
bool screen_texturepacks_test_apply_audio(const char* pack_value, bool ingame);
enum texturepacks_result screen_texturepacks_test_last_result(void);

void screen_texturepacks_test_arm(const char* config_path,
								  texturepack_reload_fn reload) {
	s_test_config_path = config_path ? config_path : "config.json";
	s_test_reload = reload ? reload : tex_init;
}

/* One entry point per CATEGORY, both running the REAL texpacks_apply_now on a
 * one-entry pack list. Returns what the screen itself concluded:
 * apply_failed_frames is the one piece of state texpacks_apply_now sets from
 * `applied` (so NO_CHANGE reports true -- the player has what they asked for
 * -- and a rejected root reports false). */
static bool screen_texpacks_test_apply_as(const char* pack_value, bool ingame,
										  enum texpacks_category as) {
	packs_free();
	string_init_set_str(packs[0].name, pack_value);
	string_init_set_str(packs[0].value, pack_value);
	packs[0].has_audio = texturepacks_has_audio(pack_value);
	packs[0].has_music = texturepacks_claims_music(pack_value);
	pack_count = 1;
	rows[0] = 0;
	row_count = 1;

	bool was_ingame = ingame_mode;
	enum texpacks_category was_category = category;
	ingame_mode = ingame;
	category = as;
	apply_failed_frames = 0;
	texpacks_apply_now(0);
	ingame_mode = was_ingame;
	category = was_category;

	bool ok = apply_failed_frames == 0;
	packs_free();
	return ok;
}

bool screen_texturepacks_test_apply(const char* pack_value, bool ingame) {
	return screen_texpacks_test_apply_as(pack_value, ingame,
										 TEXPACKS_CAT_TEXTURES);
}

bool screen_texturepacks_test_apply_audio(const char* pack_value, bool ingame) {
	return screen_texpacks_test_apply_as(pack_value, ingame,
										 TEXPACKS_CAT_SOUNDS);
}

enum texturepacks_result screen_texturepacks_test_last_result(void) {
	return last_result;
}
#endif

static void screen_texpacks_update_input(struct screen* s, float dt);

/* ONE place the preview cache is reconciled per frame, after whatever the
 * input did to the selection, the scroll or the category -- and never from
 * render2D. texpacks_done() has already cleared the cache and left the screen
 * on its way out, so the reconcile is skipped on that path rather than
 * re-reading four images for a screen nobody will see again. */
static void screen_texpacks_update(struct screen* s, float dt) {
	screen_texpacks_closed = false;
	screen_texpacks_update_input(s, dt);
	if(!screen_texpacks_closed)
		texpacks_previews_sync(gfx_width(), gfx_height());
}

static void screen_texpacks_update_input(struct screen* s, float dt) {
	(void)s;
	(void)dt;

	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);
	pointer_button = texpacks_pointer_button(gfx_width(), gfx_height());
	/* LEFT/RIGHT joined UP/DOWN here with the A16-10 strip entry below: they
	 * are real navigation on this screen now, so they must take focus
	 * ownership from a stationary pointer and stamp the origin exactly like
	 * the other two list screens. */
	bool gui_up = input_pressed(IB_GUI_UP);
	bool gui_down = input_pressed(IB_GUI_DOWN);
	bool gui_left = input_pressed(IB_GUI_LEFT);
	bool gui_right = input_pressed(IB_GUI_RIGHT);
	bool navigating = gui_up || gui_down || gui_left || gui_right;
	if(s_apply_wait) {
		if(input_pressed(IB_HOME) || input_pressed(IB_INVENTORY)) {
			screen_texturepacks_backdrop_cancel();
			return;
		}
		/* pack-lag-1030: Apply waits for the SOUNDS only. The menu backdrop
		 * bake (measured on the console: ~36 s of black "Applying pack..."
		 * on 2026-10-30, footage round2 15:59-16:35, until the player
		 * cancelled it) is DEFERRED: it stays queued and main's per-frame
		 * service runs it in bounded steps once the player is back at the
		 * title, where the job already resumes after a world-alive refusal.
		 * Until then the title shows the dirt fallback (disclosed, as before
		 * any bake). Nothing visual changes once the bake lands. */
		int sounds = s_bd_hooks->sounds_state();
		bool sounds_ready = sounds == AUDIO_REGISTRY_READY
			|| sounds == AUDIO_REGISTRY_DISABLED;
		if(sounds == AUDIO_REGISTRY_FAILED) {
			s_apply_failure_audio = true;
			texpacks_bd_fail(s_bd_hooks->sounds_reason
				? s_bd_hooks->sounds_reason() : "Required sounds couldn't be loaded.");
			s_apply_wait = false;
		} else if(sounds_ready) {
			s_apply_wait = false;
		}
		return; /* consume loading-frame input; never activate the new menu */
	}
	/* drift-1030 (footage round2 15:05-15:20): a hand-held Remote resting on
	 * the TV is never still. The shared helper re-takes focus once the
	 * sprite is 4 GUI units from where it RESTED, so a slow drift summed up
	 * over seconds and took focus back from the D-pad -- then A chose the
	 * row under the cursor instead of Apply. Here the rest point FOLLOWS a
	 * pointer that stays inside the threshold for TEXPACKS_DRIFT_FRAMES
	 * (~1/4 s): only a move of >= 4 units within that window -- a real
	 * point, at >= 32 px/s -- retakes focus. Validity transitions and
	 * direction presses behave exactly as before. */
	if(pointer_available && focus.pointer_valid && !focus.pointer_owner
	   && !navigating) {
		float dx = pointer_x - focus.x, dy = pointer_y - focus.y;
		float t = GUTIL_MENU_POINTER_JITTER;
		if(dx * dx + dy * dy < t * t) {
			if(++texpacks_drift_frames >= TEXPACKS_DRIFT_FRAMES) {
				focus.x = pointer_x;
				focus.y = pointer_y;
				texpacks_drift_frames = 0;
			}
		} else {
			texpacks_drift_frames = 0;
		}
	} else {
		texpacks_drift_frames = 0;
	}
	texpacks_pointer_owns = gutil_menu_pointer_focus(
		&focus, pointer_available, pointer_x, pointer_y, navigating);

	if(apply_pending_frames == 0 && s_import_audio_next < pack_count) {
		/* The import's audio half: the ordinary Sounds Apply of the same
		 * pack, behind the same strip, once the texture half (and its
		 * backdrop/sound wait above) has finished. Never in-game: the
		 * import itself is title-only. */
		size_t i = s_import_audio_next;
		s_import_audio_next = MAX_PACKS;
		if(!ingame_mode) {
			category = TEXPACKS_CAT_SOUNDS;
			pending[TEXPACKS_CAT_SOUNDS] = i;
			texpacks_previews_clear();
			texpacks_build_rows();
			s_import_force_reload = true;
			s_import_audio_running = true;
			apply_pending_frames = TEXPACKS_APPLY_DEFER_FRAMES;
			apply_pending_index = i;
			return;
		}
	}
	if(apply_pending_frames > 0) {
		/* Frozen behind the loading strip; the reload runs on the frame after
		 * the strip has been presented once. */
		if(--apply_pending_frames > 0)
			return;
		texpacks_apply_now(apply_pending_index);
		/* A successful apply disables Apply; return to the selected row. */
		if(!texpacks_apply_enabled())
			texpacks_focus_button = -1;
		return;
	}
	if(apply_failed_frames > 0)
		apply_failed_frames--;

	/* UP/DOWN always belong to the LIST (unchanged), so they take the ring
	 * back from any strip button -- whether the pointer or the A16-10 RIGHT
	 * entry below put it there. */
	if(gui_up || gui_down)
		texpacks_focus_button = -1;
	if(navigating)
		texpacks_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	size_t next = gui_selection;
	if(gui_up && next > 0)
		next--;
	if(gui_down && next + 1 < row_count)
		next++;
	if(next != gui_selection) {
		gui_selection = next;
		slotlist_select(&pack_slots, gui_selection, true);
	}
	/* THE STRIP ENTRY (review A16-10), the same one-line rule the two list
	 * screens got in A15: RIGHT from a pack row puts the ring on the strip's
	 * first ENABLED button without moving the selection, and reveals the
	 * selected row (A16-7's reason applies here too -- the page roles move the
	 * viewport and keep the selection). Inside the strip LEFT/RIGHT walk the
	 * enabled buttons and CLAMP, so LEFT at the leftmost one does not fall
	 * back into the list; UP/DOWN above are the way back, exactly as before.
	 *
	 * The practical effect today is small -- "Open texture pack folder" is a
	 * named parity gap and permanently disabled, so the strip's only enabled
	 * button is Done, which the Close role also reaches -- but the suite's
	 * per-screen banner claims every enabled action is reachable by direction
	 * + activate on all three list screens, and before this it was true of
	 * only two of them (case_reach_texture_packs now backs it). */
	if(texpacks_focus_button < 0) {
		if(gui_right) {
			texpacks_focus_button = texpacks_focus_first();
			if(gui_selection < row_count)
				slotlist_select(&pack_slots, gui_selection, true);
		}
	} else if(gui_left) {
		texpacks_focus_button = texpacks_focus_step(texpacks_focus_button, -1);
	} else if(gui_right) {
		texpacks_focus_button = texpacks_focus_step(texpacks_focus_button, 1);
	}
	/* While the pointer OWNS focus the ring follows it across the strip (the
	 * unchanged Options arbitration), but never onto a list ROW: Beta's
	 * GuiSlot selects on the CLICK, not on hover. */
	{
		int over = texpacks_pointer_target();
		if(texpacks_pointer_owns && over != GUTIL_MENU_TARGET_NONE
		   && texpacks_target_enabled(over)) {
			/* Hover keeps row selection unchanged, but takes activation ownership. */
			texpacks_origin = GUTIL_MENU_ORIGIN_POINTER;
			if(!texpacks_target_is_row(over))
				texpacks_focus_button = over;
		}
	}

	/* The CANONICAL menu page roles. The hotbar roles this used to read are
	 * bound to the SAME physical buttons as the page roles on the Wii Remote
	 * (Minus/Plus) and the Classic (full L/R), so one press paged twice; a
	 * role read is not per-family, so the alias goes entirely. The motion is
	 * unchanged -- Beta's GuiSlot scroll step [740c583
	 * GuiSlot.actionPerformed]. See screen_select_world.c's matching note. */
	bool apply_selected = false;
	bool click_pressed = input_pressed(IB_GUI_CLICK);
	slotlist_pointer_scroll(
		&pack_slots, pointer_y,
		pointer_available && pointer_button < 0
			&& (click_pressed || input_held(IB_GUI_CLICK)));

	/* The ONE activation path (ruling 2 item 1). A press on a row only ever
	 * SELECTS here -- Beta's 250 ms double click [740c583
	 * GuiTexturePackSlot.elementClicked] has no equivalent; applying is the
	 * explicit Apply button. */
	int hovered = texpacks_pointer_target();
	struct gutil_menu_decision decision
		= gutil_menu_activate_decide((struct gutil_menu_activate_state) {
			.pressed = click_pressed,
			.navigated = navigating,
			.pointer_valid = pointer_available,
			.selection_origin = texpacks_origin,
			.hovered = hovered,
			.hovered_enabled = texpacks_target_enabled(hovered),
			.selected = texpacks_focus_target(),
			.selected_enabled
			= texpacks_target_enabled(texpacks_focus_target()),
			/* R-D: this screen's targets open screens, they do not destroy
			 * anything by themselves. Select World's Delete and Multiplayer's
			 * Delete open a CONFIRMATION -- the destructive answer lives
			 * there, and that dialog declares it. */
			.selected_destructive = false,
			/* T01: a row press only SELECTS the pack (never applies), so ONE
			 * click on a row chooses it. */
			.hovered_selects_only = texpacks_target_is_row(hovered),
		});
	if(decision.action == GUTIL_MENU_FOCUS_ONLY) {
		if(texpacks_target_is_row(decision.target)) {
			texpacks_focus_button = -1;
			gui_selection = (size_t)(decision.target - TEXPACKS_BUTTON_COUNT);
			slotlist_select(&pack_slots, gui_selection, true);
		} else {
			texpacks_focus_button = decision.target;
		}
	}
	texpacks_origin = decision.selection_origin;
	if(decision.action == GUTIL_MENU_ACTIVATE) {
		if(decision.target == TEXPACKS_BUTTON_DONE) {
			/* Done DISCARDS an unapplied selection: nothing here writes
			 * pending[] out, and the next entry resets it (gate 1 item 7). */
			texpacks_done();
			return;
		}
		if(decision.target == TEXPACKS_BUTTON_CATEGORY) {
			/* A SELECTOR, not an action: it switches which choice the list is
			 * editing and applies nothing. Each category keeps its own pending
			 * row (item 9), so switching back finds the player's choice where
			 * they left it. */
			category = category == TEXPACKS_CAT_SOUNDS ? TEXPACKS_CAT_TEXTURES
													   : TEXPACKS_CAT_SOUNDS;
			last_result_valid = false;
			apply_failed_frames = 0;
			/* The other category shows a different set of rows; drop the
			 * images here, under the fence, and let the sync at the end of
			 * this update load the new page's. */
			texpacks_previews_clear();
			texpacks_build_rows();
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
			return;
		}
		if(texpacks_target_is_row(decision.target)
		   && texpacks_row_pack((size_t)(decision.target - TEXPACKS_BUTTON_COUNT))
			   == TEXPACKS_IMPORT_ROW) {
			/* The Import card: opens the progress screen, which starts the
			 * worker and comes back here when the player leaves it. The
			 * previews go back to the heap first (fenced), as Back does. */
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
			texpacks_previews_clear();
			screen_texpacks_closed = true;
			screen_beta_import_open();
			return;
		}
		if(decision.target == TEXPACKS_BUTTON_APPLY) {
			apply_selected = texpacks_apply_enabled();
		} else if(texpacks_target_is_row(decision.target)) {
			/* A row press SELECTS and nothing else (gate 1 item 7). Beta's
			 * double-click-to-apply is replaced by the explicit Apply button:
			 * on a controller the second press of a double click is how a
			 * player scrolls, and a 3-to-25 s reload is not something to
			 * trigger by accident. */
			size_t row = (size_t)(decision.target - TEXPACKS_BUTTON_COUNT);
			gui_selection = row;
			slotlist_select(&pack_slots, gui_selection, true);
			pending[category] = texpacks_row_pack(row);
			texpacks_focus_button = texpacks_apply_enabled()
				? TEXPACKS_BUTTON_APPLY : -1;
			last_result_valid = false;
			apply_failed_frames = 0;
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
			return;
		}
	}
	/* R-C: a consumed press reaches nothing else this frame -- not the page
	 * roles below, not Close. */
	if(click_pressed && decision.action != GUTIL_MENU_ACTIVATE)
		return;

	/* The CANONICAL menu page roles; see the note above. */
	if(input_pressed(IB_GUI_PREVIOUS))
		slotlist_scroll(&pack_slots, -1);
	if(input_pressed(IB_GUI_PAGE))
		slotlist_scroll(&pack_slots, 1);

	// The PENDING choice, applied as a transaction (texpacks_apply_now,
	// deferred behind the loading strip). The previous pack remains both live
	// and persisted if validation, the reload, or the config write fails.
	if(apply_selected && pending[category] < pack_count) {
		apply_pending_frames = TEXPACKS_APPLY_DEFER_FRAMES;
		apply_pending_index = pending[category];
		apply_failed_frames = 0;
		return;
	}

	/* HOME remains the Wii-native alias for Beta's Done id 6; the Close role
	 * joins it (Astra Lane A item 1 -- this screen read no Close at all).
	 * IB_HOME's own behaviour is untouched. GUI_LEFT is freed with the
	 * interim hint-row binding. */
	if(input_pressed(IB_HOME) || input_pressed(IB_INVENTORY))
		texpacks_done();
}

/* ===== the approved PACK CARD ==========================================
 *
 * proofs/earth--packs--default.png, left to right: a 40 px pack tile, the pack
 * NAME at 20 px, and -- right-aligned -- the "Current" badge at 14 px, whose
 * width the name reserves so the two can never collide. The card's surface is
 * the pack's own widget material, FLAT -- the atlas art carries its own edge,
 * so the card draws no bevel of its own.
 *
 * FOCUS and SELECTION are two different marks, and neither invents a colour:
 * FOCUSED takes the focus row and the bright inset ring; SELECTED (pending),
 * when the ring is elsewhere, takes the approved narrow 3 px left accent
 * (#d4d1b4); CURRENT has no outline at all and is identified purely by its
 * badge. Both badges use the one muted metadata ink (#d5d6ca) -- the WORD is
 * the distinction, not a green/amber palette this design never had.
 *
 * THE PACK TILE IS THAT PACK'S OWN PICTURE, from the bounded preview cache
 * above -- never the LIVE terrain atlas, which is the CURRENT pack's art and
 * would be a lie on every row that is not the current one. A pack that ships
 * neither pack.png nor terrain.png, or ships a corrupt one, keeps the neutral
 * "stack of pages" tile: the missing thumbnail is disclosed, never faked, and
 * never substituted from another pack. This LOOKS UP only -- the cache is
 * filled by the reset and the update (texpacks_previews_sync). */
static void texpacks_draw_icon(struct fep_rect r, size_t idx) {
	fep_paper(r);
	int e = GFX_GUI_SCALE;
	const struct texpacks_preview* p = texpacks_preview_of(idx);
	if(p && p->loaded && p->tex.data) {
		/* The FULL approved 40x40 tile -- no bevel and no inset. The pack art
		 * carries its own edge (Beta and Earth both do; Isabella's cards are
		 * deliberately flat), so a drawn frame was an invention that also stole
		 * 2 px of picture on each side.
		 * The source rect is 0,0,256,256 -- the WHOLE preview -- and NOT
		 * TEX_PREVIEW_DIM: gutil_texquad's tx/ty/sx/sy are units of a fixed
		 * 1/256 grid (gfx_draw_quads divides texcoords by 256), not texels of
		 * this texture. Passing 40 asked for the top-left 40/256 of the image,
		 * which is why the tile showed one corner. 256 == the full 0..1 UV
		 * range whatever the preview's real pixel size is. */
		gfx_bind_texture((struct tex_gfx*)&p->tex);
		gutil_texquad(r.x, r.y, 0, 0, 256, 256, r.w, r.h);
		return;
	}
	/* A neutral "stack of pages" mark: two inset bars, pack-independent. */
	gfx_texture(false);
	fep_solid(fep_rect(r.x + 3 * e, r.y + 3 * e, r.w - 6 * e, 2 * e), 0x55, 0x55,
			  0x55, 0xC0);
	fep_solid(fep_rect(r.x + 3 * e, r.y + r.h / 2, r.w - 6 * e, 2 * e), 0x55,
			  0x55, 0x55, 0xC0);
	gfx_texture(true);
}

/* The Import card: the pack card's own material, focus ring and type, with a
 * "+" tile, the action as its name and where the jar goes under it. */
static void texpacks_draw_import_row(int width, int height, size_t row) {
	struct texpacks_canvas c = texpacks_canvas_of(width, height);
	struct fep_rect r = texpacks_row_rect(width, height, row);
	bool focused = texpacks_focus_target() == TEXPACKS_TARGET_ROW(row);
	fep_widget_row(r, focused);
	if(focused)
		fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
	int pad = tp_d(&c, TP_ROW_PAD);
	int icon = tp_d(&c, TP_ROW_ICON);
	struct fep_rect t = fep_rect(r.x + pad, r.y + (r.h - icon) / 2, icon, icon);
	fep_paper(t);
	gfx_texture(false);
	int bar = tp_d(&c, 4), arm = tp_d(&c, 24);
	fep_solid(fep_rect(t.x + (t.w - arm) / 2, t.y + (t.h - bar) / 2, arm, bar),
			  0x30, 0x30, 0x30, 0xFF);
	fep_solid(fep_rect(t.x + (t.w - bar) / 2, t.y + (t.h - arm) / 2, bar, arm),
			  0x30, 0x30, 0x30, 0xFF);
	gfx_texture(true);
	int name_font = tp_d(&c, TP_NAME_FONT);
	int sub_font = tp_d(&c, TP_BADGE_FONT);
	int name_x = r.x + pad + icon + tp_d(&c, TP_ROW_ICON_GAP);
	int room = r.x + r.w - pad - name_x;
	char name[64], sub[64];
	fep_text_fit(name, sizeof(name), "Import Beta 1.7.3", name_font, room);
	fep_text_fit(sub, sizeof(sub), "From your b1.7.3.jar in sd:/beta/",
				 sub_font, room);
	gutil_text(name_x, r.y + pad, name, name_font, true);
	gutil_text_rgb(name_x, r.y + r.h - pad - sub_font, sub, sub_font, true,
				   0xC8, 0xC8, 0xC8);
}

static void texpacks_draw_row(int width, int height, size_t row,
							  const char* active) {
	size_t idx = texpacks_row_pack(row);
	if(idx == TEXPACKS_IMPORT_ROW) {
		texpacks_draw_import_row(width, height, row);
		return;
	}
	if(idx >= pack_count)
		return;
	struct texpacks_canvas c = texpacks_canvas_of(width, height);
	struct fep_rect r = texpacks_row_rect(width, height, row);
	bool is_active = texpacks_value_eq(string_get_cstr(packs[idx].value), active);
	bool is_pending = pending[category] == idx && !is_active;
	bool focused = texpacks_focus_target() == TEXPACKS_TARGET_ROW(row);

	/* A pack ROW, not a button: the approved card base (#383A37) under the
	 * pack's own -- for Isabella, fully transparent -- button material. */
	fep_widget_row(r, focused);
	/* KEYBOARD/pointer FOCUS keeps the bright inset ring -- that is the only
	 * full outline on a card. SELECTION is the approved narrow 3 px LEFT
	 * ACCENT (#d4d1b4, studio.css .studio-pack[aria-pressed="true"]:not(
	 * [data-focused="true"])), drawn only when the ring is elsewhere so the two
	 * never stack. CURRENT has no outline style of its own at all: it is
	 * identified by its badge, exactly as the approved card does. */
	if(focused)
		fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
	else if(is_pending)
		fep_fill(fep_rect(r.x, r.y, tp_d(&c, 3), r.h), 0xD4, 0xD1, 0xB4, 0xFF);

	int pad = tp_d(&c, TP_ROW_PAD);
	int icon = tp_d(&c, TP_ROW_ICON);
	texpacks_draw_icon(fep_rect(r.x + pad, r.y + (r.h - icon) / 2, icon, icon),
					   idx);

	/* The badge FIRST, because the name's room is what the badge leaves. */
	int badge_font = tp_d(&c, TP_BADGE_FONT);
	const char* badge = is_active ? "Current" : is_pending ? "Selected" : NULL;
	int badge_w = badge ? gutil_font_width(badge, badge_font) : 0;
	/* BOTH badges carry the approved MUTED METADATA ink (#d5d6ca); neither gets
	 * a colour of its own. The words -- "Current" vs "Selected" -- are what
	 * separate the live pack from the pending one. */
	if(badge) {
		gutil_text_rgb(r.x + r.w - pad - badge_w,
					   r.y + (r.h - badge_font) / 2, badge, badge_font, true,
					   0xD5, 0xD6, 0xCA);
	}

	int name_font = tp_d(&c, TP_NAME_FONT);
	int name_x = r.x + pad + icon + tp_d(&c, TP_ROW_ICON_GAP);
	int room = r.x + r.w - pad - badge_w - (badge_w ? pad : 0) - name_x;
	char name[64];
	fep_text_fit(name, sizeof(name), string_get_cstr(packs[idx].name),
				 name_font, room);

	/* In SOUNDS the card also says truthfully what the pack actually carries
	 * (gate 1 item 4), so the name rides the card's upper half with the fact
	 * under it; in TEXTURES there is only the name, and it is centred. */
	if(category == TEXPACKS_CAT_SOUNDS) {
		int sub_font = tp_d(&c, TP_BADGE_FONT);
		char sub[64];
		fep_text_fit(sub, sizeof(sub),
					 packs[idx].has_music ? "sounds + music" : "sounds only",
					 sub_font, room);
		gutil_text(name_x, r.y + pad, name, name_font, true);
		gutil_text_rgb(name_x, r.y + r.h - pad - sub_font, sub, sub_font, true,
					   0xC8, 0xC8, 0xC8);
	} else {
		gutil_text(name_x, r.y + (r.h - name_font) / 2, name, name_font, true);
	}
}

/* A page indication for a list that overflows -- a track and a proportional
 * thumb down the panel's right margin. It INVENTS NO BINDING: the page roles
 * that move it (IB_GUI_PREVIOUS / IB_GUI_PAGE) and UP/DOWN are the ones this
 * screen already reads; this only makes their effect visible. */
static void texpacks_draw_scrollbar(int width, int height) {
	struct texpacks_canvas c = texpacks_canvas_of(width, height);
	int track_top = pack_slots.top;
	int track_h = pack_slots.bottom - pack_slots.top;
	int content = (int)row_count * pack_slots.row_height;
	if(track_h <= 0 || content <= track_h)
		return;
	int bar_w = tp_d(&c, TP_SCROLL_W);
	int bar_x = tp_x(&c, TP_ROW_X + TP_ROW_W) + tp_d(&c, TP_ROW_PAD) / 2;
	int thumb_h = track_h * track_h / content;
	if(thumb_h < bar_w * 2)
		thumb_h = bar_w * 2;
	int span = content - track_h;
	int scroll = texpacks_scroll();
	if(scroll > span)
		scroll = span;
	int thumb_y = track_top
		+ (span > 0 ? (track_h - thumb_h) * scroll / span : 0);
	fep_fill(fep_rect(bar_x, track_top, bar_w, track_h), 0, 0, 0, 0x60);
	fep_fill(fep_rect(bar_x, thumb_y, bar_w, thumb_h), 0xE0, 0xE0, 0xE0, 0xE0);
}

/* The status line: ONE truthful sentence for the state the screen is really
 * in, fitted to the panel's own 528 px column (never centred off the panel,
 * never overflowing it). */
static const char* texpacks_status_text(void) {
	bool sounds = category == TEXPACKS_CAT_SOUNDS;
	if(apply_pending_frames > 0 || s_sound_phase || s_apply_wait)
		return sounds ? "Applying sound pack..." : "Applying texture pack...";
	if(apply_failed_frames > 0)
		return last_result_valid && last_result_category == TEXPACKS_CAT_SOUNDS
			? "Couldn't load sound pack."
			: "Couldn't load texture pack.";
	if(ingame_mode && sounds)
		return "Sounds can't change while a world is loaded.";
	if(gui_selection < row_count && rows[gui_selection] == TEXPACKS_IMPORT_ROW
	   && texpacks_focus_button < 0)
		return "Makes a Beta 1.7.3 pack from your own b1.7.3.jar.";
	if(s_import_both_applied && last_result_valid
	   && last_result_category == category)
		return "Beta 1.7.3 textures and sounds applied.";
	if(s_bd == TEXPACKS_BD_FAILED && (!sounds || s_apply_failure_audio)) {
		/* The visible reason + the explicit retry it offers. */
		static char line[160];
		snprintf(line, sizeof(line), "%s Apply Pack retries.", s_bd_reason);
		return line;
	}
	if(!sounds && !texpacks_apply_enabled()
	   && (s_bd == TEXPACKS_BD_QUEUED || s_bd == TEXPACKS_BD_BAKING))
		return ingame_mode ? "This pack's background is made at the title."
						   : "Making this pack's background at the title...";
	if(texpacks_apply_enabled())
		return sounds ? "Press Apply Pack to use these sounds."
					  : "Press Apply Pack to use these textures.";
	if(last_result_valid && last_result_category == category
	   && (last_result == TEXTUREPACKS_OK
		   || last_result == TEXTUREPACKS_NO_CHANGE))
		return sounds ? "Sound pack applied." : "Texture pack applied.";
	return sounds ? "Changes the sounds and music."
				  : "Changes the look of blocks and items.";
}

/* Up to two lines at `font` within `width`: break at the last space that
 * fits; the second line is fitted (ellipsis only if even two lines can't hold
 * it). */
static void texpacks_wrap2(const char* text, int font, int width,
						   char out[2][160]) {
	out[0][0] = out[1][0] = '\0';
	if(gutil_font_width(text, font) <= width) {
		snprintf(out[0], sizeof(out[0]), "%s", text);
		return;
	}
	size_t n = strlen(text), cut = 0;
	for(size_t i = 1; i < n && i < sizeof(out[0]); i++) {
		if(text[i] != ' ')
			continue;
		char head[160];
		memcpy(head, text, i);
		head[i] = '\0';
		if(gutil_font_width(head, font) > width)
			break;
		cut = i;
	}
	if(!cut) {
		fep_text_fit(out[0], sizeof(out[0]), text, font, width);
		return;
	}
	memcpy(out[0], text, cut);
	out[0][cut] = '\0';
	fep_text_fit(out[1], sizeof(out[1]), text + cut + 1, font, width);
}

/* toast-1030: the controller-notice mount, 32,57 576x28 on the canvas --
 * a 28 px card clear of the heading and its shadow (22 + 28 + 3 = 53, plus a
 * 4 px gap) and above the panel (86). */
static struct gutil_button_bounds texpacks_notice_rect(int width, int height) {
	struct texpacks_canvas c = texpacks_canvas_of(width, height);
	return (struct gutil_button_bounds) {tp_x(&c, TP_HEAD_X), tp_d(&c, 57),
										 tp_d(&c, TP_PANEL_W), tp_d(&c, 28)};
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void screen_texpacks_test_notice_rect(int width, int height, int* x, int* y,
									  int* w, int* h);
void screen_texpacks_test_notice_rect(int width, int height, int* x, int* y,
									  int* w, int* h) {
	struct gutil_button_bounds n = texpacks_notice_rect(width, height);
	*x = n.x;
	*y = n.y;
	*w = n.width;
	*h = n.height;
}
#endif

static const char* texpacks_heading(void) {
	return category == TEXPACKS_CAT_SOUNDS ? "Sound Packs" : "Texture Packs";
}

static void screen_texpacks_render2D(struct screen* s, int width, int height) {
	(void)s;
	if(apply_pending_frames > 0 || s_sound_phase || s_apply_wait || s_apply_running) {
		/* No new-pack scenery is revealed until the transaction gate opens. */
		fep_fill(fep_rect(0, 0, width, height), 0, 0, 0, 255);
		loading_notice_draw_strip(width, height,
			s_sound_phase ? LOADING_NOTICE_SOUNDS : apply_pending_frames > 0
				? LOADING_NOTICE_TEXTURES : "Applying pack...");
		return;
	}
	struct texpacks_canvas c = texpacks_canvas_of(width, height);
	if(ingame_mode) {
		/* IN-GAME the backdrop is the ACTUAL WORLD (render_world is on): the
		 * pause screen's own contract, so an applied pack previews immediately.
		 * Only the dark overlay Options uses is drawn over it. */
		fep_fill(fep_rect(0, 0, width, height), 0, 0, 0, 180);
	} else if(!fep_backdrop(width, height, 90)) {
		/* No scenic asset in this pack or the bundled one -- the ordinary
		 * tiled-dirt menu background, disclosed rather than faked. */
		gutil_bg();
	}

	/* The heading, at the composition's own 32,22 in the 28 px face. */
	const char* title = texpacks_heading();
	gutil_text(tp_x(&c, TP_HEAD_X), tp_d(&c, TP_HEAD_Y), title,
			   tp_d(&c, TP_HEAD_FONT), true);
	/* toast-1030 (round2 bug 02/14): controller notices used the shared
	 * whole-screen corner and covered the heading. This screen mounts them in
	 * the note row between the heading (ends y 50) and the panel (y 86); while
	 * one is up, the row's own line steps aside for it. */
	bool notice_up = gutil_connection_notice_width(
						 0, gstate.players[1].local_player != NULL, false) > 0;
	{
		struct gutil_button_bounds n = texpacks_notice_rect(width, height);
		screen_notice_compose(n.x, n.y, n.width, n.height, false);
	}
	if(!notice_up && !ingame_mode && !s_import_available) {
		/* ingame-import-1030: no jar on the card -> one line saying where to
		 * put it, in the header band the in-game note uses. */
		char note[96];
		fep_text_fit(note, sizeof(note),
					 "To import Beta 1.7.3, put your b1.7.3.jar in sd:/beta/",
					 tp_d(&c, TP_NOTE_FONT), tp_d(&c, TP_PANEL_W));
		gutil_text_rgb(tp_x(&c, TP_HEAD_X), tp_d(&c, TP_NOTE_Y), note,
					   tp_d(&c, TP_NOTE_FONT), true, 0xD0, 0xD0, 0xD0);
	}
	if(!notice_up && ingame_mode) {
		/* The in-game explanation the ruling asks for (gate 1 item 6): WHY the
		 * Sounds selector is disabled here, and what to do about it. It sits in
		 * the header band between the heading and the panel, fitted to the
		 * panel's width so it cannot run off the composition. */
		char note[96];
		fep_text_fit(note, sizeof(note),
					 "Textures only here - return to the title to change sounds.",
					 tp_d(&c, TP_NOTE_FONT), tp_d(&c, TP_PANEL_W));
		gutil_text_rgb(tp_x(&c, TP_HEAD_X), tp_d(&c, TP_NOTE_Y), note,
					   tp_d(&c, TP_NOTE_FONT), true, 0xD0, 0xD0, 0xD0);
	}

	/* The paper panel, then the list viewport recessed into it. */
	struct fep_rect panel = fep_rect(tp_x(&c, TP_PANEL_X), tp_d(&c, TP_PANEL_Y),
									 tp_d(&c, TP_PANEL_W), tp_d(&c, TP_PANEL_H));
	fep_paper(panel);
	fep_bevel(panel);

	/* The cards, clipped to the viewport -- the local renderer (widget_slotlist
	 * is still the scroll/selection model; only its 220-unit row drawing is
	 * bypassed, and only on this screen). */
	const char* active = texpacks_current_value(category);
	gfx_scissor(true, (uint32_t)(c.ox > 0 ? c.ox : 0),
				(uint32_t)pack_slots.top, (uint32_t)tp_d(&c, 640),
				(uint32_t)(pack_slots.bottom - pack_slots.top));
	for(size_t row = 0; row < row_count; row++) {
		struct fep_rect r = texpacks_row_rect(width, height, row);
		if(r.y + r.h <= pack_slots.top || r.y >= pack_slots.bottom)
			continue;
		texpacks_draw_row(width, height, row, active);
	}
	gfx_scissor(false, 0, 0, 0, 0);
	texpacks_draw_scrollbar(width, height);

	/* The status line, inside the panel at 56,322. */
	const char* status = texpacks_status_text();
	char lines[2][160];
	texpacks_wrap2(status, tp_d(&c, TP_DESC_FONT), tp_d(&c, TP_DESC_W), lines);
	bool bad = apply_failed_frames > 0;
	/* Shadow OFF: this line is DARK ink on the paper panel, and a dark drop
	 * shadow under dark text reads as a heavy blur rather than as lift.
	 * critique-1030 #7: a long reason ("Making the background was
	 * cancelled. Apply Pack retries.") was cut off at the panel edge; it now
	 * takes a second line (322 + 18, inside the panel's 362 bottom). */
	for(int k = 0; k < 2; k++)
		if(lines[k][0])
			gutil_text_rgb(tp_x(&c, TP_DESC_X),
						   tp_d(&c, TP_DESC_Y + k * (TP_DESC_FONT + 2)),
						   lines[k], tp_d(&c, TP_DESC_FONT), false,
						   bad ? 0xC0 : 0x30, 0x30, 0x30);

	/* The category toggle and the primary Apply, both drawn from the pack's own
	 * widget material. The ONE pure predicate the activation decision reads
	 * (A3 NEW-7) still decides enabled/focused, so the highlight and the press
	 * cannot drift apart. */
	for(int button = TEXPACKS_BUTTON_CATEGORY; button <= TEXPACKS_BUTTON_APPLY;
		button++) {
		struct gutil_button_bounds b
			= texpacks_button_rect(width, height, button);
		struct fep_rect r = fep_rect(b.x, b.y, b.width, b.height);
		bool enabled = texpacks_target_enabled(button);
		bool focused = texpacks_focus_target() == button;
		fep_widget(r, enabled && focused);
		fep_bevel(r);
		if(focused)
			fep_outline(r, GFX_GUI_SCALE, 0xFF, 0xFF, 0xA0, 0xFF);
		if(!enabled)
			fep_fill(r, 0, 0, 0, 0x70);
		const char* label = texpacks_button_label(button);
		int font = tp_d(&c, button == TEXPACKS_BUTTON_APPLY ? TP_NAME_FONT
															: TP_DESC_FONT);
		char text[32];
		fep_text_fit(text, sizeof(text), label, font, r.w - 2 * GFX_GUI_SCALE);
		int tw = gutil_font_width(text, font);
		uint8_t ink = enabled ? (focused ? 0xFF : 0xE0) : 0xA0;
		gutil_text_rgb(r.x + (r.w - tw) / 2, r.y + (r.h - font) / 2, text, font,
					   true, ink, ink, enabled && focused ? 0xE0 : ink);
	}

	/* The approved glyph footer: Move / Select / Back, drawn by the ORIGINAL
	 * gutil prompt helper from the DETECTED bindings. Back is the same target
	 * the Close/HOME roles reach -- one action, two ways in. */
	struct gutil_hint_row f = texpacks_footer_layout(width, height);
	if(texpacks_focus_target() == TEXPACKS_BUTTON_DONE) {
		struct gutil_button_bounds b
			= texpacks_button_rect(width, height, TEXPACKS_BUTTON_DONE);
		fep_outline(fep_rect(b.x, b.y, b.width, b.height), GFX_GUI_SCALE, 0xFF,
					0xFF, 0xA0, 0xFF);
	}
	gutil_hint_row_draw(&f);

	/* pointer-1030 (maintainer, real Wii): this screen never drew the IR
	 * cursor, so a live, drifting pointer was INVISIBLE while it still owned
	 * hover and activation -- after a D-pad move the hidden sprite drifted
	 * past the 4-unit threshold, took focus back, and A re-chose the row (or
	 * Apply) under it: "buttons won't make a new selection". Every sibling
	 * menu draws it; so does this one now. */
	if(pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(pointer_x, pointer_y, glm_rad(pointer_angle), 0,
							 0, 256, 256, 48 * GFX_GUI_SCALE,
							 48 * GFX_GUI_SCALE);
	}

}

/* ui-build-1026 T15: menus_dwell walker observers (screen.h). Reads only. */
int screen_texpacks_row_count(void) { return (int)row_count; }
int screen_texpacks_selected_row(void) {
	return gui_selection < row_count ? (int)gui_selection : -1;
}
int screen_texpacks_focused_row(void) {
	int t = texpacks_focus_target();
	return texpacks_target_is_row(t) ? t - TEXPACKS_BUTTON_COUNT : -1;
}
bool screen_texpacks_apply_focused(void) {
	return texpacks_focus_target() == TEXPACKS_BUTTON_APPLY;
}
bool screen_texpacks_apply_enabled(void) { return texpacks_apply_enabled(); }
bool screen_texpacks_pending_selected(void) {
	return gui_selection < row_count
		&& pending[category] == texpacks_row_pack(gui_selection);
}
bool screen_texpacks_busy(void) {
	return apply_pending_frames > 0 || s_sound_phase || s_apply_wait || s_apply_running;
}
bool screen_texpacks_row_center(int row, int width, int height, float* cx,
								float* cy) {
	if(row < 0 || (size_t)row >= row_count || !cx || !cy)
		return false;
	if(rows[row] == TEXPACKS_IMPORT_ROW)
		return false; /* menus_dwell must never start an import */
	struct fep_rect r = texpacks_row_rect(width, height, (size_t)row);
	if(r.y < pack_slots.top || r.y + r.h > pack_slots.bottom)
		return false;
	*cx = (float)r.x + (float)r.w / 2.0F;
	*cy = (float)r.y + (float)r.h / 2.0F;
	return true;
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* Read-only observation points for tests/test_frontend_no_pointer.c, plus the
 * one fixture it cannot build from the filesystem: screen_texpacks_reset
 * scans path_from_sd_root(TEXPACKS_DIR), i.e. "sd:/texturepacks", which no
 * host has -- so a headless run always sees the one bundled default row and a
 * multi-page pack list is otherwise unreachable. The seed appends exactly the
 * rows the directory scan above would have appended (same name/value shape,
 * same MAX_PACKS ceiling) and re-arms the REAL slotlist through
 * slotlist_set_count; everything the test then drives is production code. */
int screen_texpacks_test_scroll(void) {
	return slotlist_amount_scrolled(&pack_slots);
}

size_t screen_texpacks_test_selection(void) { return gui_selection; }

size_t screen_texpacks_test_pack_count(void) { return pack_count; }

/* A19 (Astra ruling 6): the two edges the hint's band would have to fit
 * between on THIS screen. Exposed -- read-only, from the same rect functions
 * the render uses -- because the measurement is the whole finding: the gap
 * here is 6 px, not the 24 px the other two list screens have, so the ruled
 * text-only hint does not fit and is NOT drawn here. A test pins the number so
 * it cannot rot while the question is with Astra. */
int screen_texpacks_test_list_bottom(void) { return pack_slots.bottom; }

int screen_texpacks_test_strip_top(void) {
	return texpacks_button_rect(gfx_width(), gfx_height(),
								TEXPACKS_BUTTON_DONE)
		.y;
}

void screen_texpacks_test_seed(size_t extra) {
	for(size_t k = 0; k < extra && pack_count < MAX_PACKS; k++) {
		char name[32];
		snprintf(name, sizeof(name), "pack%02zu", k);
		string_init_set_str(packs[pack_count].name, name);
		string_init_printf(packs[pack_count].value, "%s/%s", TEXPACKS_DIR,
						   name);
		/* Seeded rows stand in for the directory scan, which on a host finds
		 * nothing under "sd:/texturepacks". They are texture-only: no host
		 * directory backs them, so nothing could load audio from one. The
		 * audio-eligible fixture is seeded by the case that needs it. */
		packs[pack_count].has_audio = false;
		packs[pack_count].has_music = false;
		pack_count++;
	}
	texpacks_build_rows();
}

/* The same seed, for packs that are eligible in the SOUNDS category. There is
 * no host directory behind them either, so an apply of one fails at the
 * directory check -- which is exactly what the navigation cases assert about
 * (selection and traversal), never about a successful load. */
void screen_texpacks_test_seed_audio(size_t extra, bool with_music);
void screen_texpacks_test_seed_audio(size_t extra, bool with_music) {
	for(size_t k = 0; k < extra && pack_count < MAX_PACKS; k++) {
		char name[32];
		snprintf(name, sizeof(name), "snd%02zu", k);
		string_init_set_str(packs[pack_count].name, name);
		string_init_printf(packs[pack_count].value, "%s/%s", TEXPACKS_DIR,
						   name);
		packs[pack_count].has_audio = true;
		packs[pack_count].has_music = with_music;
		pack_count++;
	}
	texpacks_build_rows();
}

int screen_texpacks_test_category(void);
int screen_texpacks_test_category(void) { return (int)category; }

size_t screen_texpacks_test_row_count(void);
size_t screen_texpacks_test_row_count(void) { return row_count; }

/* The pack index Apply would commit, or MAX_PACKS for "nothing pending". */
size_t screen_texpacks_test_pending(void);
size_t screen_texpacks_test_pending(void) { return pending[category]; }

bool screen_texpacks_test_apply_enabled(void);
bool screen_texpacks_test_apply_enabled(void) { return texpacks_apply_enabled(); }

int screen_texpacks_test_highlight(void);
int screen_texpacks_test_highlight(void) { return texpacks_focus_target(); }

/* ingame-import-1030: a control's rect exactly as render and hit test use it
 * (false = not drawn: a zero-size rect), and whether a pack apply is armed. */
bool screen_texpacks_test_button_rect(int button, int width, int height,
									  int* x, int* y, int* w, int* h);
bool screen_texpacks_test_button_rect(int button, int width, int height,
									  int* x, int* y, int* w, int* h) {
	struct gutil_button_bounds b = texpacks_button_rect(width, height, button);
	*x = b.x;
	*y = b.y;
	*w = b.width;
	*h = b.height;
	return b.width > 0 && b.height > 0;
}
bool screen_texpacks_test_import_shown(void);
bool screen_texpacks_test_import_shown(void) { return texpacks_import_shown(); }
/* The name the list shows for packs[i] (NULL past the end). */
const char* screen_texpacks_test_pack_name(size_t i);
const char* screen_texpacks_test_pack_name(size_t i) {
	return i < pack_count ? string_get_cstr(packs[i].name) : NULL;
}
/* The visible row that is the Import card, or -1. */
int screen_texpacks_test_import_row(void);
int screen_texpacks_test_import_row(void) {
	for(size_t r = 0; r < row_count; r++)
		if(rows[r] == TEXPACKS_IMPORT_ROW)
			return (int)r;
	return -1;
}

/* The preview cache exactly as the render reads it: how many slots exist, and
 * which pack each one answers for (MAX_PACKS = free) and whether it holds a
 * real image or a remembered miss. */
size_t screen_texpacks_test_preview_slots(void);
size_t screen_texpacks_test_preview_slots(void) {
	return TEXPACKS_PREVIEW_SLOTS;
}

size_t screen_texpacks_test_preview_pack(size_t slot);
size_t screen_texpacks_test_preview_pack(size_t slot) {
	return slot < TEXPACKS_PREVIEW_SLOTS ? previews[slot].pack
										 : (size_t)MAX_PACKS;
}

bool screen_texpacks_test_preview_loaded(size_t slot);
bool screen_texpacks_test_preview_loaded(size_t slot) {
	return slot < TEXPACKS_PREVIEW_SLOTS && previews[slot].loaded;
}
#endif

struct screen screen_texturepacks = {
	.reset = screen_texpacks_reset,
	.update = screen_texpacks_update,
	.render2D = screen_texpacks_render2D,
	.render3D = NULL,
	.render_world = false,
};
