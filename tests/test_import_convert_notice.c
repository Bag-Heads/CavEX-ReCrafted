/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.

	convert-notice-1024 (maintainer Option B, tmp/jobs/convert-notice-1023/
	PLAN.md): the one-time "this world will be converted" notice on world
	select for a Minecraft Beta world.

	  N1  the predicate (import_convert_notice_needed) is true for a Beta
	      level.dat (no CaveWiiGenerator, no CaveWiiSaveVersion) and false for
	      a fresh CavEX world and for a Beta world already stamped version 5;
	  N2  the shared confirm dialog is the Beta two-line layout (y=70/90
	      units, no third line) for every remaining caller (delete world,
	      quit-unsavable); the three-line notice variant is gone (T09);
	  N3  ui-build-1026/T09: Play on a Beta row opens screen_convert_backup
	      with the approved text ("Back up this world?", the world's name,
	      'Saves a copy as "<name> (backup)"'), three buttons with Cancel
	      highlighted, and nothing on disk; Cancel returns to world select
	      with the same row selected, level.dat byte-identical and NO
	      "(backup)" folder; Convert without backup issues exactly one
	      SRPC_LOAD_WORLD for that world;
	  N4  Play on a CavEX row and on a stamped row loads directly (no ask);
	  N5  the autoload path never opens the ask, even for a Beta world;
	  N6  Back up & convert: real world_copy steps (bounded, one per frame),
	      progress -1 while surveying then 0..100 non-decreasing, NO load
	      before the backup is published, then exactly one load; the
	      published "<dir> (backup)" holds level.dat + region/ and the
	      source level.dat is byte-identical;
	  N7  a failed backup (SD full; free-space query failure) shows "Couldn't
	      back up world" with Cancel highlighted, never loads, leaves no
	      "(backup)" or ".world-copy-*" entry behind; only an explicit
	      Convert without backup loads afterwards;
	  N8  review round 1: a copy that fails mid-way cleans up over several
	      frames ("Cleaning up...", one bounded step per frame, no answers),
	      then destroy finds no temp tree left; error + Cancel, no load.
	  N9  ui-build-1026/T10: after Convert without backup (and after a
	      published backup, N6) the screen runs the REAL convert-only
	      mailbox (svin_convert_* via client_session_convert_*): no load and
	      no answers while CONVERTING, an indeterminate stage (never a
	      percent) when the chain reports no fraction, then Done with Play
	      world focused / Back to worlds; Done names the backup only when one
	      was published AND still exists; Play issues exactly ONE load of the
	      ORIGINAL path; Back returns to the list with the same row, no load;
	  N10 every other receipt: SAVE_PENDING is its own screen ("Finishes
	      when you play", never "World converted"), Play still loads once;
	      CANCELLED with may_have_changed=true says "The world may already be
	      converted" (never "not changed"/"unchanged"); FAILED with and
	      without may_have_changed; OPEN_FAILED, JOURNAL_REFUSED,
	      WORLD_ACTIVE, PROVIDER_STOPPED and a refused start each show an
	      honest error with Back to worlds only, and never load.

	The server thread is not running here: the test plays it through the
	real mailbox (svin_convert_take / svin_convert_finish), exactly the two
	calls server_local_convert_service makes. The migration chain itself is
	proven by test_convert_only (T09a/T09b).

	Drives the REAL screen_select_world.c / screen_convert_backup.c /
	screen_confirm.c through their CAVEWII_CLIENT_INTERFACE_TEST hooks. No
	GPU: nothing here renders.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
static void set_autoload_env(const char* value) {
	static char buf[128];
	snprintf(buf, sizeof(buf), "CAVEX_AUTOLOAD=%s", value ? value : "");
	_putenv(buf);
}
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
static void set_autoload_env(const char* value) {
	if(value)
		setenv("CAVEX_AUTOLOAD", value, 1);
	else
		unsetenv("CAVEX_AUTOLOAD");
}
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "cNBT/nbt.h"
#include "config.h"
#include "game/game_state.h"
#include "game/gui/import_convert_notice.h"
#include "game/gui/screen.h"
#include "graphics/gfx_settings.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/world_copy.h"
#include "parson/parson.h"

/* Test hooks (CAVEWII_CLIENT_INTERFACE_TEST). */
void screen_sworld_test_reset(int width, int height);
void screen_sworld_test_reset_autoload_latch(void);
size_t screen_sworld_test_resolve_step(size_t budget);
size_t screen_sworld_test_row_count(void);
int screen_sworld_test_row(size_t idx, const char** name, const char** directory,
						   const char** path, int64_t* last_access,
						   int64_t* byte_size);
size_t screen_sworld_test_gui_selection(void);
bool screen_sworld_test_play_row(size_t idx);
void screen_convert_backup_test_set_volume(int mode, uint64_t free_bytes,
										   uint32_t cluster_bytes);
int screen_convert_backup_test_state(void);
int screen_convert_backup_test_text(const char** title, char lines[3][192],
									const char* labels[3], int* button_count,
									int* selected);
bool screen_convert_backup_test_press(int k);
void screen_convert_backup_test_tick(void);
int screen_convert_backup_test_percent(void);
void screen_convert_backup_test_set_fault(int fault, size_t successes);
unsigned screen_convert_backup_test_steps_last_tick(void);
bool screen_convert_backup_test_temp_left_at_destroy(void);
int screen_convert_backup_test_convert_stage(char* label, size_t cap);
const char* screen_convert_backup_test_footer_back(void);
enum {
	CB_ASK = 0,
	CB_VOLUME = 1,
	CB_COPYING = 2,
	CB_ERROR = 3,
	CB_CLEANING = 4,
	CB_CONVERTING = 5,
	CB_DONE = 6,
	CB_SAVE_PENDING = 7,
	CB_CONVERT_ERROR = 8
};
size_t screen_sworld_test_archive_opens(void);
size_t screen_sworld_test_play_archive_opens(void);
int screen_confirm_test_highlight(void);
int screen_confirm_test_lines(const char* text[3], int y[3],
							  const char** yes_label, const char** no_label,
							  int* safe_answer);
bool screen_confirm_test_answer(bool yes);

static char s_root[128];

#define DIR_BETA "Beta World"
#define DIR_CAVEX "CavEX World"
#define DIR_STAMPED "Stamped Beta"

struct strip {
	bool generator, version;
};
static bool keep_tag(const nbt_node* node, void* aux) {
	const struct strip* st = aux;
	if(!node->name)
		return true;
	if(st->generator && !strcmp(node->name, "CaveWiiGenerator"))
		return false;
	if(st->version && !strcmp(node->name, "CaveWiiSaveVersion"))
		return false;
	return true;
}

/* Same construction as test_import_migrate_fast_equiv.c's write_level: a
 * real level.dat from the embedded template, with the CaveWii provenance
 * tags stripped to make it look like a level.dat Beta itself wrote. */
static void make_world(const char* dir_name, bool cavewii_created,
					   int version) {
	char dir[256], region[300], path[300];
	snprintf(dir, sizeof dir, "%s/%s", s_root, dir_name);
	assert(MKDIR(dir) == 0);
	snprintf(region, sizeof region, "%s/region", dir);
	assert(MKDIR(region) == 0);
	string_t world;
	string_init_set_str(world, dir);
	struct level_archive level;
	assert(level_archive_create_new(&level, world, 173,
									(vec3) {0.5F, 65.62F, 0.5F}));
	nbt_node* v = nbt_find_by_path(level.data, LEVEL_CAVEWII_SAVE_VERSION.name);
	assert(v && v->type == TAG_INT);
	v->payload.tag_int = version;
	struct strip st = {.generator = !cavewii_created,
					   .version = version == CAVEWII_SAVE_VERSION_LEGACY
						   && !cavewii_created};
	level.data = nbt_filter_inplace(level.data, keep_tag, &st);
	assert(level.data);
	snprintf(path, sizeof path, "%s/level.dat", dir);
	FILE* f = fopen(path, "wb");
	assert(f);
	assert(nbt_dump_file(level.data, f, STRAT_GZIP) == NBT_OK);
	assert(fclose(f) == 0);
	level_archive_discard(&level);
	string_clear(world);
}

static bool predicate_for(const char* dir_name) {
	char dir[256];
	snprintf(dir, sizeof dir, "%s/%s", s_root, dir_name);
	string_t world;
	string_init_set_str(world, dir);
	struct level_archive la;
	assert(level_archive_create(&la, world));
	bool needed = import_convert_notice_needed(&la);
	level_archive_destroy(&la);
	string_clear(world);
	return needed;
}

/* Whole-file bytes, for the "Back writes nothing" check. */
static size_t read_file(const char* dir_name, unsigned char* buf, size_t cap) {
	char path[300];
	snprintf(path, sizeof path, "%s/%s/level.dat", s_root, dir_name);
	FILE* f = fopen(path, "rb");
	assert(f);
	size_t n = fread(buf, 1, cap, f);
	assert(n > 0 && n < cap);
	fclose(f);
	return n;
}

static size_t row_of(const char* dir_name) {
	for(size_t i = 0; i < screen_sworld_test_row_count(); i++) {
		const char* d = NULL;
		screen_sworld_test_row(i, NULL, &d, NULL, NULL, NULL);
		if(d && !strcmp(d, dir_name))
			return i;
	}
	assert(!"row not found");
	return 0;
}

/* Drain the server inbox (no server thread here) counting world loads. */
struct load_count {
	unsigned loads;
	char last[512];
};
static void count_rpc(struct server_rpc* call, void* user) {
	struct load_count* c = user;
	if(call->type == SRPC_LOAD_WORLD) {
		c->loads++;
		snprintf(c->last, sizeof c->last, "%s",
				 string_get_cstr(call->payload.load_world.name));
		string_clear(call->payload.load_world.name);
	}
}
static struct load_count drain_loads(void) {
	struct load_count c = {0};
	svin_process_messages(count_rpc, &c, false);
	return c;
}

static void fresh_list(void) {
	screen_sworld_test_reset(640, 480);
	while(screen_sworld_test_resolve_step(64))
		;
	assert(screen_sworld_test_row_count() == 3);
}

static void park_on_select_world(void) {
	screen_set(&screen_select_world);
	fresh_list();
}

static void dummy_cb(bool yes) { (void)yes; }

/* ---- N1 ---------------------------------------------------------------- */
static void check_predicate(void) {
	assert(predicate_for(DIR_BETA));
	assert(!predicate_for(DIR_CAVEX));
	assert(!predicate_for(DIR_STAMPED));
	puts("PASS N1 predicate: Beta level.dat -> notice; CavEX world and "
		 "version-5 Beta world -> none");
}

/* ---- N2 ---------------------------------------------------------------- */
static void check_two_line_unchanged(void) {
	const int sc = GFX_GUI_SCALE;
	const char* t[3];
	int y[3];
	const char *yes, *no;
	int safe;

	/* The delete-world dialog exactly as sworld_delete_selected arms it. */
	screen_confirm_setup("Are you sure you want to delete this world?",
						 "'x' will be lost forever! (A long time!)", "Delete",
						 "Cancel", SCREEN_CONFIRM_ANSWER_NO, dummy_cb);
	int n = screen_confirm_test_lines(t, y, &yes, &no, &safe);
	assert(n == 2 && t[2] == NULL);
	assert(y[0] == 70 * sc && y[1] == 90 * sc);
	assert(!strcmp(t[0], "Are you sure you want to delete this world?"));
	assert(!strcmp(t[1], "'x' will be lost forever! (A long time!)"));
	assert(!strcmp(yes, "Delete") && !strcmp(no, "Cancel"));
	assert(safe == SCREEN_CONFIRM_ANSWER_NO);

	/* The quit-unsavable shape: also exactly two Beta lines. */
	screen_confirm_setup("Couldn't save part of the world", "Chunk 3, -2",
						 "Quit anyway", "Try again", SCREEN_CONFIRM_ANSWER_NO,
						 dummy_cb);
	n = screen_confirm_test_lines(t, y, &yes, &no, &safe);
	assert(n == 2 && t[2] == NULL);
	assert(y[0] == 70 * sc && y[1] == 90 * sc);
	assert(!strcmp(t[0], "Couldn't save part of the world"));
	assert(!strcmp(t[1], "Chunk 3, -2"));
	assert(!strcmp(yes, "Quit anyway") && !strcmp(no, "Try again"));
	puts("PASS N2 confirm dialogs: 2 Beta lines at y=70/90 units, no third "
		 "line (three-line notice removed)");
}

/* ---- helpers for the backup screen --------------------------------------- */
static bool entry_exists(const char* name) {
	char path[300];
	struct stat st;
	snprintf(path, sizeof path, "%s/%s", s_root, name);
	return stat(path, &st) == 0;
}

/* Anything in the saves folder besides the three fixture worlds. */
static unsigned stray_entries(void) {
	unsigned n = 0;
	DIR* d = opendir(s_root);
	assert(d);
	struct dirent* e;
	while((e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")
		   || !strcmp(e->d_name, DIR_BETA) || !strcmp(e->d_name, DIR_CAVEX)
		   || !strcmp(e->d_name, DIR_STAMPED))
			continue;
		printf("  stray saves entry: %s\n", e->d_name);
		n++;
	}
	closedir(d);
	return n;
}

static void expect_text(const char* title, const char* line0,
						const char* label0, int buttons, int selected) {
	const char* t;
	char lines[3][192];
	const char* labels[3];
	int count, sel;
	int n = screen_convert_backup_test_text(&t, lines, labels, &count, &sel);
	assert(!strcmp(t, title));
	assert(n >= 1 && !strcmp(lines[0], line0));
	assert(count == buttons && sel == selected);
	if(buttons)
		assert(!strcmp(labels[0], label0));
}

/* ---- T10: the convert-only step -------------------------------------------
 * Play the server thread's half of the REAL mailbox exactly as
 * server_local_convert_service does: take the queued ticket (asserting it
 * names the ORIGINAL world, never the backup), optionally let the player's
 * cancel land while the chain "runs", then finish with `outcome`. */
static void serve_convert(enum svin_convert_outcome outcome,
						  bool may_have_changed, bool cancel_while_running) {
	uint64_t ticket = 0;
	const char* path = NULL;
	bool cancelled = true;
	assert(svin_convert_take(&ticket, &path, &cancelled));
	assert(ticket && ticket == gstate.players[0].convert.ticket);
	assert(!cancelled && strstr(path, DIR_BETA) && !strstr(path, "backup")
		   && "convert-only runs on the ORIGINAL world path");
	if(cancel_while_running)
		assert(svin_convert_cancel(ticket));
	assert(svin_convert_finish(ticket, outcome, may_have_changed));
}

/* Ticks while CONVERTING with no receipt: nothing loads, nothing answers,
 * and the stage is honest (no fraction is running here: indeterminate). */
static void converting_frames(unsigned frames) {
	for(unsigned k = 0; k < frames; k++) {
		assert(screen_convert_backup_test_state() == CB_CONVERTING);
		assert(gstate.players[0].current_screen == &screen_convert_backup);
		char label[64];
		assert(screen_convert_backup_test_convert_stage(label, sizeof label)
				   == -1
			   && "no denominator -> indeterminate, never a percent");
		assert(!strchr(label, '%'));
		assert(!screen_convert_backup_test_press(0)
			   && "no answers while converting");
		screen_convert_backup_test_tick();
		assert(drain_loads().loads == 0 && "nothing loads while converting");
	}
}

struct result_text {
	const char* title;
	char lines[3][192];
	const char* labels[3];
	int n, count, sel;
};
static struct result_text result_text(void) {
	struct result_text r;
	r.n = screen_convert_backup_test_text(&r.title, r.lines, r.labels, &r.count,
										  &r.sel);
	return r;
}
static bool text_has(const struct result_text* r, const char* needle) {
	if(strstr(r->title, needle))
		return true;
	for(int k = 0; k < r->n; k++)
		if(strstr(r->lines[k], needle))
			return true;
	return false;
}

/* Settle one receipt and land on its result screen. */
static void settle(enum svin_convert_outcome outcome, bool changed,
				   bool cancel_while_running, int want_state) {
	converting_frames(3);
	serve_convert(outcome, changed, cancel_while_running);
	screen_convert_backup_test_tick();
	assert(screen_convert_backup_test_state() == want_state);
	assert(!gstate.players[0].convert_pending);
	/* A settled receipt is consumed once: more frames change nothing. */
	for(int k = 0; k < 3; k++) {
		screen_convert_backup_test_tick();
		assert(screen_convert_backup_test_state() == want_state);
	}
	assert(drain_loads().loads == 0 && "a result never plays on its own");
}

/* Back to worlds: the list, the same row, nothing loaded. */
static void back_to_list(int k) {
	assert(screen_convert_backup_test_press(k));
	assert(gstate.players[0].current_screen == &screen_select_world);
	assert(screen_sworld_test_gui_selection() == row_of(DIR_BETA)
		   && "Back keeps the selected world");
	assert(drain_loads().loads == 0);
}

/* Play world: exactly ONE load, of the original path. */
static void play_once(void) {
	assert(screen_convert_backup_test_press(0));
	assert(gstate.players[0].current_screen == &screen_load_world);
	struct load_count c = drain_loads();
	assert(c.loads == 1 && strstr(c.last, DIR_BETA) && !strstr(c.last, "backup")
		   && "Play loads the ORIGINAL world exactly once");
	assert(drain_loads().loads == 0);
}

/* Open the ask on the Beta row from a fresh list. */
static void open_ask(void) {
	park_on_select_world();
	(void)drain_loads();
	assert(screen_sworld_test_play_row(row_of(DIR_BETA)));
	assert(gstate.players[0].current_screen == &screen_convert_backup);
	assert(screen_convert_backup_test_state() == CB_ASK);
}

/* ---- N3 ---------------------------------------------------------------- */
static void check_beta_row_ask(void) {
	static unsigned char before[1 << 16], after[1 << 16];
	park_on_select_world();
	(void)drain_loads();
	size_t beta = row_of(DIR_BETA);
	size_t n_before = read_file(DIR_BETA, before, sizeof before);

	/* Review blocker (Sol): Play on a RESOLVED row must not reopen level.dat
	 * -- the predicate is cached by the resolve open. */
	size_t resolve_opens = screen_sworld_test_archive_opens();
	size_t play_opens = screen_sworld_test_play_archive_opens();
	assert(screen_sworld_test_play_row(beta));
	assert(screen_sworld_test_archive_opens() == resolve_opens
		   && screen_sworld_test_play_archive_opens() == play_opens
		   && "Play on an OK row does zero additional level.dat opens");
	assert(gstate.players[0].current_screen == &screen_convert_backup);

	const char* title;
	char lines[3][192];
	const char* labels[3];
	int count, sel;
	int n = screen_convert_backup_test_text(&title, lines, labels, &count, &sel);
	/* The fixture's LevelName is the template's; the screen names it. */
	const char* name = NULL;
	screen_sworld_test_row(beta, &name, NULL, NULL, NULL, NULL);
	char want[3][192];
	snprintf(want[0], 192, "\"%s\" needs conversion for CavEX.", name);
	snprintf(want[1], 192, "A backup keeps the original for PC.");
	snprintf(want[2], 192, "Saves a copy as \"%s (backup)\".", name);
	assert(!strcmp(title, "Back up this world?"));
	assert(n == 3);
	for(int k = 0; k < 3; k++)
		assert(!strcmp(lines[k], want[k]));
	assert(count == 3 && !strcmp(labels[0], "Back up & convert")
		   && !strcmp(labels[1], "Convert without backup")
		   && !strcmp(labels[2], "Cancel"));
	assert(sel == 2 && "Cancel is highlighted on open");
	assert(drain_loads().loads == 0 && "nothing loads before an answer");
	assert(stray_entries() == 0 && "asking reserves nothing on disk");

	/* Cancel: world select, same row, nothing written, nothing loaded. */
	assert(screen_convert_backup_test_press(2));
	assert(gstate.players[0].current_screen == &screen_select_world);
	assert(screen_sworld_test_gui_selection() == row_of(DIR_BETA)
		   && "Cancel keeps the selected world");
	assert(drain_loads().loads == 0);
	assert(stray_entries() == 0 && "Cancel leaves no (backup) folder");
	size_t n_after = read_file(DIR_BETA, after, sizeof after);
	assert(n_after == n_before && !memcmp(before, after, n_before)
		   && "Cancel must leave level.dat byte-identical");
	assert(predicate_for(DIR_BETA) && "Cancel asks again next time");

	/* Convert without backup: convert-only first (T10), no load yet. */
	open_ask();
	assert(screen_convert_backup_test_press(1));
	assert(gstate.players[0].current_screen == &screen_convert_backup);
	assert(screen_convert_backup_test_state() == CB_CONVERTING);
	assert(drain_loads().loads == 0);
	assert(stray_entries() == 0);
	settle(SVIN_CONVERT_SUCCEEDED, true, false, CB_DONE);
	play_once();
	puts("PASS N3 Beta row: backup ask (approved text, Cancel highlighted, "
		 "nothing on disk); Cancel -> list, same row, level.dat unchanged, no "
		 "backup folder; Convert without backup -> convert, Done, one load");
}

/* ---- N4 ---------------------------------------------------------------- */
static void check_direct_rows(void) {
	const char* dirs[2] = {DIR_CAVEX, DIR_STAMPED};
	for(int k = 0; k < 2; k++) {
		park_on_select_world();
		(void)drain_loads();
		size_t play_opens = screen_sworld_test_play_archive_opens();
		assert(screen_sworld_test_play_row(row_of(dirs[k])));
		assert(screen_sworld_test_play_archive_opens() == play_opens);
		assert(gstate.players[0].current_screen == &screen_load_world);
		struct load_count c = drain_loads();
		assert(c.loads == 1 && strstr(c.last, dirs[k]));
	}
	/* A PENDING (unresolved) Beta row: one open, shared by creative and the
	 * predicate, and the ask still shows. */
	screen_set(&screen_select_world);
	screen_sworld_test_reset(640, 480);
	size_t before_pending = screen_sworld_test_play_archive_opens();
	assert(screen_sworld_test_play_row(row_of(DIR_BETA)));
	assert(screen_sworld_test_play_archive_opens() == before_pending + 1);
	assert(gstate.players[0].current_screen == &screen_convert_backup);
	assert(screen_convert_backup_test_press(2));
	puts("PASS N4 CavEX world and stamped Beta world load directly (no "
		 "ask)");
}

/* ---- N5 ---------------------------------------------------------------- */
static void check_autoload_never_notices(void) {
	park_on_select_world();
	(void)drain_loads();
	set_autoload_env(DIR_BETA);
	screen_sworld_test_reset_autoload_latch();
	screen_select_world.update(&screen_select_world, 0.0F);
	set_autoload_env(NULL);
	assert(gstate.players[0].current_screen == &screen_load_world
		   && "autoload goes straight to loading, never to the ask");
	struct load_count c = drain_loads();
	assert(c.loads == 1 && strstr(c.last, DIR_BETA));
	puts("PASS N5 autoload of a Beta world never opens the backup ask");
}

/* ---- N6 ---------------------------------------------------------------- */
static void check_backup_then_load(void) {
	static unsigned char before[1 << 16], after[1 << 16];
	size_t n_before = read_file(DIR_BETA, before, sizeof before);
	/* A 300000-byte region file: several WORLD_COPY_BYTES_PER_STEP frames,
	 * so real intermediate progress is observable. The REAL free-space query
	 * of this volume is used (mode 0). */
	char region[300];
	snprintf(region, sizeof region, "%s/%s/region/r.0.0.mcr", s_root, DIR_BETA);
	FILE* rf = fopen(region, "wb");
	assert(rf);
	for(int k = 0; k < 300000; k++)
		fputc(k * 7 & 0xFF, rf);
	assert(fclose(rf) == 0);
	screen_convert_backup_test_set_volume(0, 0, 0);
	open_ask();
	assert(screen_convert_backup_test_press(0));
	assert(gstate.players[0].current_screen == &screen_convert_backup);
	assert(screen_convert_backup_test_state() == CB_VOLUME);
	{
		const char* t;
		char lines[3][192];
		const char* labels[3];
		int count, sel;
		assert(screen_convert_backup_test_text(&t, lines, labels, &count, &sel)
			   == 2);
		assert(!strcmp(t, "Backing up world") && count == 0);
		assert(!strncmp(lines[0], "Saving a copy as \"", 18));
		assert(!strcmp(lines[1], "Don't turn off the Wii or remove the SD card."));
	}
	int last = -1, frames = 0;
	bool saw_survey = false, saw_copy = false;
	while(screen_convert_backup_test_state() == CB_VOLUME
	      || screen_convert_backup_test_state() == CB_COPYING) {
		assert(gstate.players[0].current_screen == &screen_convert_backup);
		assert(drain_loads().loads == 0
			   && "no load before the backup is published");
		int pct = screen_convert_backup_test_percent();
		assert(pct >= -1 && pct <= 100);
		if(screen_convert_backup_test_state() == CB_COPYING) {
			if(pct < 0) {
				saw_survey = true;
			} else {
				saw_copy = true;
				assert(pct >= last && "progress never goes backwards");
				last = pct;
			}
		}
		assert(screen_convert_backup_test_state() != CB_ERROR);
		screen_convert_backup_test_tick();
		assert(++frames < 10000 && "copy finishes in bounded frames");
	}
	assert(saw_survey && saw_copy && frames > 5);
	assert(last > 0 && "copy progress was observed between 0 and done");
	/* T10: the published backup leads to convert-only, not to a load. */
	assert(screen_convert_backup_test_state() == CB_CONVERTING);
	assert(drain_loads().loads == 0);
	{
		struct result_text r = result_text();
		assert(!strcmp(r.title, "Converting world") && r.count == 0);
		assert(!strcmp(r.lines[0], "Backup saved as \"" DIR_BETA " (backup)\"."));
		assert(!strcmp(r.lines[1], "Don't turn off the Wii or remove the SD card."));
	}
	settle(SVIN_CONVERT_SUCCEEDED, true, false, CB_DONE);
	{
		const char* name = NULL;
		screen_sworld_test_row(row_of(DIR_BETA), &name, NULL, NULL, NULL, NULL);
		char want0[192];
		snprintf(want0, sizeof want0, "\"%s\" is ready to play in CavEX.", name);
		struct result_text r = result_text();
		assert(!strcmp(r.title, "World converted") && r.n == 3);
		assert(!strcmp(r.lines[0], want0));
		assert(!strcmp(r.lines[1],
					   "Your original is kept as \"" DIR_BETA " (backup)\"."));
		assert(!strcmp(r.lines[2], "Copy that folder to play it on PC."));
		assert(r.count == 2 && !strcmp(r.labels[0], "Play world")
			   && !strcmp(r.labels[1], "Back to worlds") && r.sel == 0
			   && "Done: Play world focused, Back to worlds");
		assert(!strcmp(screen_convert_backup_test_footer_back(), "Back"));
	}
	play_once();
	assert(entry_exists(DIR_BETA " (backup)/level.dat"));
	assert(entry_exists(DIR_BETA " (backup)/region/r.0.0.mcr"));
	size_t n_after = read_file(DIR_BETA, after, sizeof after);
	assert(n_after == n_before && !memcmp(before, after, n_before)
		   && "the backup never writes the source world");
	char backup[300];
	snprintf(backup, sizeof backup, "%s/%s (backup)", s_root, DIR_BETA);
	test_fixture_rm_rf(backup);
	assert(remove(region) == 0);
	assert(stray_entries() == 0 && "no temp or journal left after publish");
	printf("PASS N6 Back up & convert (real volume query): %d frames of bounded "
		   "world_copy steps, survey then progress up to %d%%, no load until "
		   "published, then convert-only, Done naming the backup, Play -> one "
		   "load; backup has level.dat + region file\n",
		   frames, last);
}

/* ---- N7 ---------------------------------------------------------------- */
static void run_to_error(void) {
	assert(screen_convert_backup_test_press(0));
	int frames = 0;
	while(screen_convert_backup_test_state() != CB_ERROR) {
		assert(gstate.players[0].current_screen == &screen_convert_backup
			   && "a failing backup never leaves the screen on its own");
		screen_convert_backup_test_tick();
		assert(++frames < 10000);
	}
	assert(drain_loads().loads == 0 && "a failed backup never loads");
	assert(stray_entries() == 0
		   && "a failed backup leaves no (backup) or temp entry");
}

static void check_backup_failures(void) {
	static unsigned char before[1 << 16], after[1 << 16];
	size_t n_before = read_file(DIR_BETA, before, sizeof before);

	/* SD card full: world_copy_start refuses (LOW_SPACE). */
	screen_convert_backup_test_set_volume(1, 0, 4096);
	open_ask();
	run_to_error();
	expect_text("Couldn't back up world", "The SD card is full.",
				"Convert without backup", 2, 1);
	{
		const char* t;
		char lines[3][192];
		const char* labels[3];
		int count, sel;
		assert(screen_convert_backup_test_text(&t, lines, labels, &count, &sel)
			   == 3);
		assert(strstr(lines[1], "was not changed."));
		assert(!strcmp(lines[2], "Free some space, or convert without a backup."));
		assert(!strcmp(labels[1], "Cancel"));
	}
	/* Cancel: back to the list, nothing loaded. */
	assert(screen_convert_backup_test_press(1));
	assert(gstate.players[0].current_screen == &screen_select_world);
	assert(drain_loads().loads == 0);

	/* Free-space query failure: nothing is surveyed or reserved. */
	screen_convert_backup_test_set_volume(-1, 0, 0);
	open_ask();
	run_to_error();
	expect_text("Couldn't back up world",
				"The SD card's free space couldn't be read.",
				"Convert without backup", 2, 1);
	/* Only the player's explicit Convert without backup converts, and only
	 * Play world loads. Done names no backup: none was made. */
	assert(screen_convert_backup_test_press(0));
	assert(screen_convert_backup_test_state() == CB_CONVERTING);
	settle(SVIN_CONVERT_SUCCEEDED, true, false, CB_DONE);
	{
		struct result_text r = result_text();
		assert(r.n == 2 && !strcmp(r.lines[1],
								   "No backup of the original was made."));
		assert(!text_has(&r, "(backup)") && !text_has(&r, "kept as"));
	}
	play_once();

	size_t n_after = read_file(DIR_BETA, after, sizeof after);
	assert(n_after == n_before && !memcmp(before, after, n_before));
	screen_convert_backup_test_set_volume(0, 0, 0);
	puts("PASS N7 failed backup (SD full, free-space unreadable): error "
		 "screen with Cancel highlighted, no load, nothing left on disk; only "
		 "an explicit Convert without backup converts, then Play loads");
}

/* ---- N8 ---------------------------------------------------------------- */
/* Review round 1 (Sol): a copy that fails mid-way cleans its temp tree over
 * SEVERAL frames, one bounded world_copy_step per frame, showing "Cleaning
 * up..."; only then is the object destroyed, with no temp tree left for
 * destroy to walk. */
static void check_cleanup_is_stepped(void) {
	static unsigned char before[1 << 16], after[1 << 16];
	size_t n_before = read_file(DIR_BETA, before, sizeof before);
	char file[320];
	for(int k = 0; k < 200; k++) {
		snprintf(file, sizeof file, "%s/%s/region/f%03d.bin", s_root, DIR_BETA, k);
		FILE* f = fopen(file, "wb");
		assert(f);
		fputs("cleanup fixture", f);
		assert(fclose(f) == 0);
	}
	screen_convert_backup_test_set_volume(1, UINT64_C(1) << 40, 4096);
	screen_convert_backup_test_set_fault(WORLD_COPY_FAULT_WRITE, 150);
	open_ask();
	assert(screen_convert_backup_test_press(0));
	int frames = 0, cleaning_frames = 0;
	while(screen_convert_backup_test_state() != CB_ERROR) {
		int state = screen_convert_backup_test_state();
		if(state == CB_CLEANING) {
			cleaning_frames++;
			assert(!screen_convert_backup_test_press(0)
				   && "no answer (and no Back) while cleaning up");
			const char* t;
			char lines[3][192];
			const char* labels[3];
			int count, sel;
			screen_convert_backup_test_text(&t, lines, labels, &count, &sel);
			assert(!strcmp(t, "Cleaning up...") && count == 0);
		}
		screen_convert_backup_test_tick();
		if(state != CB_VOLUME)
			assert(screen_convert_backup_test_steps_last_tick() == 1
				   && "exactly one bounded world_copy_step per frame");
		assert(drain_loads().loads == 0);
		assert(++frames < 10000);
	}
	printf("  cleanup took %d frames (of %d)\n", cleaning_frames, frames);
	assert(cleaning_frames >= 3 && "cleanup spans several frames");
	assert(!screen_convert_backup_test_temp_left_at_destroy()
		   && "destroy found no temp tree left: it does no copy work");
	expect_text("Couldn't back up world",
				"The backup couldn't be written to the SD card.",
				"Convert without backup", 2, 1);
	assert(stray_entries() == 0 && "cleanup removed temp and reservation");
	assert(screen_convert_backup_cleanup_done() && "stepped cleanup proven too");
	assert(screen_convert_backup_test_press(1)); /* Cancel */
	assert(gstate.players[0].current_screen == &screen_select_world);
	assert(drain_loads().loads == 0);
	size_t n_after = read_file(DIR_BETA, after, sizeof after);
	assert(n_after == n_before && !memcmp(before, after, n_before));
	for(int k = 0; k < 200; k++) {
		snprintf(file, sizeof file, "%s/%s/region/f%03d.bin", s_root, DIR_BETA, k);
		assert(remove(file) == 0);
	}
	screen_convert_backup_test_set_fault(WORLD_COPY_FAULT_NONE, 0);
	screen_convert_backup_test_set_volume(0, 0, 0);
	printf("PASS N8 failed copy cleans up over %d frames, one bounded step "
		   "each, then destroy with nothing left; error + Cancel, no load\n",
		   cleaning_frames);
}

/* ---- N8b (hw1028b) -------------------------------------------------------
 * The T16a rig's exact write fault (screen_convert_backup_rig_arm_fault(2):
 * WORLD_COPY_FAULT_WRITE after ONE success). Write 1 is the journal record,
 * write 2 the destination's .incomplete marker, so the copy fails with a
 * near-empty tree and world_copy runs the whole cleanup inside that same
 * bounded step: COPYING -> ERROR with no CLEANING frame (hw1028a's real-Wii
 * tape: saw_cleaning=false, fail=3, card clean). The cleanup still provably
 * ran: cleanup_done, no stray entry, the journal gone. */
static void check_fast_cleanup_is_proven(void) {
	static unsigned char before[1 << 16], after[1 << 16];
	size_t n_before = read_file(DIR_BETA, before, sizeof before);
	screen_convert_backup_test_set_volume(1, UINT64_C(1) << 40, 4096);
	screen_convert_backup_test_set_fault(WORLD_COPY_FAULT_WRITE, 1);
	open_ask();
	assert(!screen_convert_backup_cleanup_done());
	assert(screen_convert_backup_test_press(0));
	int frames = 0, cleaning_frames = 0, copying_frames = 0;
	while(screen_convert_backup_test_state() != CB_ERROR) {
		int state = screen_convert_backup_test_state();
		if(state == CB_CLEANING)
			cleaning_frames++;
		if(state == CB_COPYING)
			copying_frames++;
		screen_convert_backup_test_tick();
		assert(++frames < 10000);
	}
	printf("  rig fault: %d copying frames, %d cleaning frames\n",
		   copying_frames, cleaning_frames);
	assert(copying_frames >= 1);
	assert(cleaning_frames == 0
		   && "cleanup of the marker-only tree fits inside the failing step");
	assert(screen_convert_backup_fail() == 3 /* CB_FAIL_COPY, not CLEANUP */);
	assert(screen_convert_backup_copy_error() == WORLD_COPY_IO_ERROR);
	assert(screen_convert_backup_cleanup_done()
		   && "cleanup ran to its end although it was never shown");
	assert(stray_entries() == 0 && "no (backup), marker or journal left");
	expect_text("Couldn't back up world",
				"The backup couldn't be written to the SD card.",
				"Convert without backup", 2, 1);
	assert(screen_convert_backup_test_press(1)); /* Cancel */
	assert(drain_loads().loads == 0);
	size_t n_after = read_file(DIR_BETA, after, sizeof after);
	assert(n_after == n_before && !memcmp(before, after, n_before));
	screen_convert_backup_test_set_fault(WORLD_COPY_FAULT_NONE, 0);
	screen_convert_backup_test_set_volume(0, 0, 0);
	puts("PASS N8b rig write fault (2nd write = marker): ERROR with no "
		 "Cleaning-up frame, cleanup_done proven, card clean");
}

/* N8 round 2: recovery cleanup fails with its parent DIR still open. Repeated
 * screen attempts must release that handle without retrying the bad tree. */
static void check_failed_cleanup_releases(void) {
	char source_path[300], retained_path[WORLD_COPY_PATH_MAX], journal[300];
	char destination[WORLD_COPY_PATH_MAX], marker[WORLD_COPY_PATH_MAX];
	snprintf(source_path, sizeof source_path, "%s/%s", s_root, DIR_BETA);
	struct world_copy* copy = world_copy_survey_begin(source_path, 4096);
	const struct world_copy_result* r = world_copy_get_result(copy);
	for(unsigned steps = 0; r->state == WORLD_COPY_SURVEYING; steps++) {
		assert(steps < 10000);
		world_copy_step(copy, 1);
	}
	assert(r->state == WORLD_COPY_READY);
	assert(!r->temporary[0]);
	strcpy(destination, r->destination);
	snprintf(marker, sizeof marker, "%s/.incomplete", destination);
	assert(world_copy_start(copy, UINT64_MAX) == WORLD_COPY_COPYING);
	struct stat marked;
	for(unsigned steps = 0; stat(marker, &marked) != 0; steps++) {
		assert(steps < 10000 && r->state == WORLD_COPY_COPYING);
		world_copy_step(copy, 1);
	}
	assert(S_ISREG(marked.st_mode));
	world_copy_release(copy);
	assert(world_copy_test_open_handles() == 0);
	/* Keep the journal/marker pair valid. An over-deep payload makes bounded
	 * recovery fail with its parent DIR open, without touching the source. */
	strcpy(retained_path, destination);
	for(unsigned depth = 0; depth <= WORLD_COPY_DEPTH_MAX; depth++) {
		strcat(retained_path, "/d");
		assert(MKDIR(retained_path) == 0);
	}
	strcat(retained_path, "/evidence");
	FILE* f = fopen(retained_path, "wb"); assert(f);
	assert(fputs("retained recovery evidence", f) >= 0 && fclose(f) == 0);
	screen_convert_backup_test_set_volume(1, UINT64_C(1) << 40, 4096);
	for(unsigned attempt = 0; attempt < 8; attempt++) {
		open_ask();
		assert(screen_convert_backup_test_press(0));
		unsigned frames = 0;
		while(screen_convert_backup_test_state() != CB_ERROR) {
			screen_convert_backup_test_tick();
			assert(++frames < 10000);
		}
		assert(world_copy_test_open_handles() == 0);
		expect_text("Couldn't back up world",
					"The unfinished backup couldn't be removed.", "Cancel", 1, 0);
		struct stat retained;
		assert(stat(retained_path, &retained) == 0 && retained.st_size == 26);
		assert(stat(marker, &marked) == 0 && S_ISREG(marked.st_mode));
		assert(drain_loads().loads == 0);
		assert(screen_convert_backup_test_press(0)); /* Cleanup error: Cancel only. */
		assert(gstate.players[0].current_screen == &screen_select_world);
	}
	/* The shared fixture remover is itself depth-limited to 16. */
	assert(remove(retained_path) == 0);
	*strrchr(retained_path, '/') = 0;
	test_fixture_rm_rf(retained_path);
	test_fixture_rm_rf(destination);
	snprintf(journal, sizeof journal, "%s/.world-copy-1.owner", s_root);
	assert(remove(journal) == 0);
	assert(stray_entries() == 0);
	screen_convert_backup_test_set_volume(0, 0, 0);
	puts("PASS N8 repeated cleanup failures release all handles, no load");
}

/* ---- N9 ------------------------------------------------------------------ */
static void check_done_back_and_vanished_backup(void) {
	/* Done -> Back to worlds: the list, the same row, nothing loaded; the
	 * Home/1 footer reads Back. */
	open_ask();
	assert(screen_convert_backup_test_press(1));
	settle(SVIN_CONVERT_SUCCEEDED, true, false, CB_DONE);
	assert(!strcmp(screen_convert_backup_test_footer_back(), "Back"));
	back_to_list(1);

	/* A backup that vanished before the result is never named. */
	screen_convert_backup_test_set_volume(1, UINT64_C(1) << 40, 4096);
	open_ask();
	assert(screen_convert_backup_test_press(0));
	unsigned frames = 0;
	while(screen_convert_backup_test_state() != CB_CONVERTING) {
		assert(screen_convert_backup_test_state() != CB_ERROR);
		screen_convert_backup_test_tick();
		assert(++frames < 10000);
	}
	char backup[300];
	snprintf(backup, sizeof backup, "%s/%s (backup)", s_root, DIR_BETA);
	assert(entry_exists(DIR_BETA " (backup)/level.dat"));
	test_fixture_rm_rf(backup);
	settle(SVIN_CONVERT_SUCCEEDED, true, false, CB_DONE);
	{
		struct result_text r = result_text();
		assert(!strcmp(r.title, "World converted"));
		assert(r.n == 2 && !strcmp(r.lines[1],
								   "No backup of the original was made."));
		assert(!text_has(&r, "kept as"));
	}
	back_to_list(1);
	assert(stray_entries() == 0);
	screen_convert_backup_test_set_volume(0, 0, 0);
	puts("PASS N9 Done -> Back to worlds keeps the row, loads nothing; a "
		 "backup gone before the result is never named");
}

/* ---- N10 ----------------------------------------------------------------- */
static void expect_error(const char* title, const char* line0,
						 const char* line1) {
	struct result_text r = result_text();
	assert(!strcmp(r.title, title));
	assert(r.n == 3 && !strcmp(r.lines[0], line0));
	assert(strstr(r.lines[1], line1));
	assert(!strcmp(r.lines[2], "No backup of the original was made."));
	assert(r.count == 1 && !strcmp(r.labels[0], "Back to worlds")
		   && r.sel == 0 && "an error offers Back only");
	assert(!text_has(&r, "World converted") && !text_has(&r, "ready to play"));
	assert(!strcmp(screen_convert_backup_test_footer_back(), "Back"));
}

static void check_other_receipts(void) {
	/* SAVE_PENDING: its own screen, never Done; Play still loads once. */
	open_ask();
	assert(screen_convert_backup_test_press(1));
	settle(SVIN_CONVERT_SAVE_PENDING, true, false, CB_SAVE_PENDING);
	{
		struct result_text r = result_text();
		assert(!strcmp(r.title, "Finishes when you play"));
		assert(!text_has(&r, "World converted") && !text_has(&r, "ready to play"));
		assert(!strcmp(r.lines[1],
					   "The last step is saved when you play the world."));
		assert(r.count == 2 && !strcmp(r.labels[0], "Play world")
			   && !strcmp(r.labels[1], "Back to worlds"));
	}
	play_once();

	/* CANCELLED after the chain was entered (cancel lands while running):
	 * "may already be converted", never "not changed"/"unchanged". */
	open_ask();
	assert(screen_convert_backup_test_press(1));
	settle(SVIN_CONVERT_SUCCEEDED, true, true, CB_CONVERT_ERROR);
	assert(gstate.players[0].convert.outcome == SVIN_CONVERT_CANCELLED
		   && gstate.players[0].convert.may_have_changed);
	expect_error("Conversion stopped", "The conversion was stopped.",
				 "The world may already be converted.");
	{
		struct result_text r = result_text();
		assert(!text_has(&r, "not changed") && !text_has(&r, "unchanged"));
	}
	back_to_list(0);

	/* CANCELLED before the chain was entered: truthfully unchanged. */
	open_ask();
	assert(screen_convert_backup_test_press(1));
	settle(SVIN_CONVERT_CANCELLED, false, false, CB_CONVERT_ERROR);
	expect_error("Conversion stopped", "The conversion was stopped.",
				 "was not changed.");
	back_to_list(0);

	/* FAILED, with and without may_have_changed. */
	open_ask();
	assert(screen_convert_backup_test_press(1));
	settle(SVIN_CONVERT_FAILED, true, false, CB_CONVERT_ERROR);
	expect_error("Couldn't convert world", "The world couldn't be converted.",
				 "may be partly converted.");
	{
		struct result_text r = result_text();
		assert(!text_has(&r, "not changed"));
	}
	back_to_list(0);

	static const struct {
		enum svin_convert_outcome outcome;
		const char* line0;
	} refusals[] = {
		{SVIN_CONVERT_FAILED, "The world couldn't be converted."},
		{SVIN_CONVERT_OPEN_FAILED, "The world's level.dat couldn't be read."},
		{SVIN_CONVERT_JOURNAL_REFUSED,
		 "An unfinished save must be repaired first."},
		{SVIN_CONVERT_WORLD_ACTIVE, "Another world is still open."},
		{SVIN_CONVERT_PROVIDER_STOPPED,
		 "The converter stopped before it started."},
	};
	for(size_t k = 0; k < sizeof refusals / sizeof refusals[0]; k++) {
		open_ask();
		assert(screen_convert_backup_test_press(1));
		settle(refusals[k].outcome, false, false, CB_CONVERT_ERROR);
		expect_error("Couldn't convert world", refusals[k].line0,
					 "was not changed.");
		back_to_list(0);
	}

	/* A refused start (no local provider): honest error, Back only. */
	svin_set_local_motion(false);
	open_ask();
	assert(screen_convert_backup_test_press(1));
	assert(screen_convert_backup_test_state() == CB_CONVERT_ERROR);
	assert(!gstate.players[0].convert_pending);
	expect_error("Couldn't convert world", "The converter couldn't be started.",
				 "was not changed.");
	back_to_list(0);
	svin_set_local_motion(true);

	assert(stray_entries() == 0);
	puts("PASS N10 SAVE_PENDING is its own screen (Play loads once); "
		 "CANCELLED+may_have_changed says 'may already be converted'; "
		 "FAILED/OPEN_FAILED/JOURNAL_REFUSED/WORLD_ACTIVE/PROVIDER_STOPPED and "
		 "a refused start show an honest error, Back only, no load");
}

int main(void) {
	test_fixture_dir("test_import_convert_notice_tmp", s_root, sizeof s_root);
	test_fixture_rm_rf(s_root);
	assert(MKDIR(s_root) == 0);
	make_world(DIR_BETA, false, CAVEWII_SAVE_VERSION_LEGACY);
	make_world(DIR_CAVEX, true, CAVEWII_SAVE_VERSION_CURRENT);
	make_world(DIR_STAMPED, false, CAVEWII_SAVE_VERSION_CURRENT);

	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	config_set_string(&gstate.config_user, "paths.worlds", s_root);
	set_autoload_env(NULL);
	assert(svin_init());
	/* The local provider's start arms the convert-only mailbox (T10); the
	 * server thread's half is played by serve_convert(). */
	svin_set_local_motion(true);

	check_predicate();
	check_two_line_unchanged();
	check_beta_row_ask();
	check_direct_rows();
	check_autoload_never_notices();
	check_backup_then_load();
	check_backup_failures();
	check_cleanup_is_stepped();
	check_fast_cleanup_is_proven();
	check_failed_cleanup_releases();
	check_done_back_and_vanished_backup();
	check_other_receipts();

	test_fixture_dir_cleanup(s_root);
	puts("PASS test_import_convert_notice");
	return 0;
}
