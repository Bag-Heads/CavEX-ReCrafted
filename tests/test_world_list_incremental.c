/*
	Golden test for the INCREMENTAL world-list resolve (handoff 2026-07-29 P3(a) half 1).

	SYMPTOM on hardware: "save and quit" froze and painted a crash banner, then the game
	carried on. The recovered report reads main=SCRN_UPD srv=RPC.

	THE MAINTAINER'S PREMISE WAS WRONG -- the save is not what blocked. It runs on the
	server thread, which is exactly what srv=RPC says. What blocked MAIN is the very next
	statement: screen_pause.c calls screen_set(&screen_select_world), and screen.c:25-30
	invokes reset() SYNCHRONOUSLY, which used to open + GZIP-INFLATE + NBT-parse EVERY
	world's level.dat inline. The card has ~118 of them, while the server was fsync-ing the
	same card. That is the main=SCRN_UPD span (main.c:976-979).

	⚠ WHAT THE REPORT DOES *NOT* PROVE. heartbeat_gap_max_ms is a per-BOOT running MAX, not
	the duration of this stall, so "4051 ms was spent in screen_sworld_reset" is NOT a
	supported claim. What is supported: the main loop made no progress for over the 3000 ms
	threshold, and g_wd_phase_main read SCRN_UPD when the dump was assembled.

	⚠ THIS IS A DEVIATION, NOT PARITY. Beta enumerates the world list EAGERLY and
	unbudgeted on the main thread (GuiSelectWorld.initGui -> loadSaves ->
	SaveConverterMcRegion.getSaveList -> GZIP + full NBT per directory, no cache). K is a
	watchdog-safety number, not a Beta one. See docs/deviations.md.

	EVERY ASSERTION HERE IS ON WORK PER CALL -- counts of level_archive_create, counts of
	rows -- never on elapsed time. CLAUDE.md forbids wall-clock claims on the wrong
	instrument, and a PC SSD hides the SD cost this split exists to bound.
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
#define RMDIR(p) _rmdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define RMDIR(p) rmdir(p)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "config.h"
#include "network/level_archive.h"
#include "parson/parson.h"

static char s_fixture_SAVES[128]; /* was #define SAVES='test_world_list_tmp' (a literal) */
#define SAVES s_fixture_SAVES
static char s_fixture_CREATE_ROOT[128]; /* was #define CREATE_ROOT='test_world_create_tmp' (a literal) */
#define CREATE_ROOT s_fixture_CREATE_ROOT
static char s_fixture_MODE_ROOT[128]; /* was #define MODE_ROOT='test_world_mode_tmp' (a literal) */
#define MODE_ROOT s_fixture_MODE_ROOT
#define N_WORLDS 150
// Must match enum world_row_state in screen_select_world.c.
#define ROW_PENDING 0
#define ROW_OK 1
#define ROW_UNREADABLE 2

// A reference EAGER sweep: what the old screen_sworld_reset produced, so the incremental
// result can be proved equivalent rather than merely plausible.
struct ref_row {
	char directory[64];
	char name[64];
	int64_t last_access;
	int64_t byte_size;
};
static struct ref_row ref_rows[N_WORLDS + 8];
static size_t ref_count;

static void* reject_world_list_owner_alloc(size_t size) {
	(void)size;
	return NULL;
}

static void build_fixture(void) {
	MKDIR(SAVES);
	for(int i = 0; i < N_WORLDS; i++) {
		char dir[128];
		snprintf(dir, sizeof(dir), test_fixture_join(SAVES, "/New World %d"), i + 1);
		MKDIR(dir);
		char region[160];
		snprintf(region, sizeof(region), "%s/region", dir);
		MKDIR(region);

		string_t p;
		string_init_set_str(p, dir);
		struct level_archive la;
		// level_archive_create_new writes a fresh level.dat from the embedded template --
		// no assets needed, so the fixture is self-contained.
		vec3 spawn = {8.0F, 70.0F, 8.0F};
		assert(level_archive_create_new(&la, p, (int64_t)(i + 1), spawn));
		level_archive_destroy(&la);
		string_clear(p);
	}
	// HOSTILE ROW: a save directory with NO level.dat at all. Today's eager sweep dropped
	// it silently; the incremental resolve must classify it UNREADABLE instead of hanging,
	// crashing, or double-freeing on the next reset().
	MKDIR(test_fixture_join(SAVES, "/Broken World"));
}

// Eagerly parse ONE world directory exactly as the old screen_sworld_reset did, so each
// incremental row can be compared against its own directory rather than against an assumed
// ordering.
static bool eager_parse(const char* path, const char* dir, struct ref_row* out) {
	string_t p;
	string_init_set_str(p, path);
	struct level_archive la;
	bool ok = level_archive_create(&la, p);
	if(ok) {
		snprintf(out->directory, sizeof(out->directory), "%s", dir);
		if(!level_archive_read(&la, LEVEL_NAME, out->name, sizeof(out->name))
		   || !out->name[0])
			snprintf(out->name, sizeof(out->name), "%s", dir);
		if(!level_archive_read(&la, LEVEL_DISK_SIZE, &out->byte_size, 0))
			out->byte_size = 0;
		if(!level_archive_read(&la, LEVEL_LAST_PLAYED, &out->last_access, 0))
			out->last_access = 0;
		out->last_access /= 1000;
		level_archive_destroy(&la);
	}
	string_clear(p);
	return ok;
}

static char order_ref[N_WORLDS + 8][64];
static size_t order_ref_count;

static void point_config_at_fixture(void) {
	if(!gstate.config_user.root)
		gstate.config_user.root = json_value_init_object();
	config_set_string(&gstate.config_user, "paths.worlds", SAVES);
}

// --- (1) reset() must do NO archive work --------------------------------------------
// Reads 150 today: the whole point of the defect.
static void check_reset_does_no_archive_work(void) {
	screen_sworld_test_reset(640, 480);
	assert(screen_sworld_test_archive_opens() == 0
		   && "screen_sworld_reset still opens level.dat files -- that is the >3s "
			  "main-thread stall this split exists to remove");
	assert(screen_sworld_test_row_count() == N_WORLDS + 1
		   && "every save directory must be ENUMERATED in reset(), including the "
			  "unreadable one -- the list size has to be FINAL when reset() returns or "
			  "the autoload-by-directory scan can latch the wrong world");
	assert(screen_sworld_test_pending() == N_WORLDS + 1);
	printf("  [1] reset() enumerates %d rows with 0 archive opens\n", N_WORLDS + 1);
}

// --- (2) the budget is honoured exactly, and no row is opened twice -------------------
static void check_budget_is_exact(void) {
	const size_t budgets[] = {1, 2, 7, 64};
	for(size_t b = 0; b < sizeof(budgets) / sizeof(budgets[0]); b++) {
		const size_t k = budgets[b];
		screen_sworld_test_reset(640, 480);
		const size_t total = screen_sworld_test_row_count();
		size_t steps = 0, sum = 0;
		for(;;) {
			size_t did = screen_sworld_test_resolve_step(k);
			if(did == 0)
				break;
			assert(did <= k && "a resolve step exceeded its budget");
			if(sum + did < total)
				assert(did == k && "a mid-list step resolved fewer rows than the budget");
			sum += did;
			steps++;
			assert(steps <= total + 2 && "resolve is not terminating");
		}
		assert(sum == total
			   && "every row must be resolved exactly once -- no row opened twice, none "
				  "skipped");
		assert(screen_sworld_test_archive_opens() == total);
		assert(screen_sworld_test_pending() == 0);
	}
	printf("  [2] budget honoured exactly at K=1/2/7/64; each row opened exactly once\n");
}

// --- (2b) THE PER-FRAME PATH IS BOUNDED ------------------------------------------------
// (2) only proves screen_sworld_resolve_step honours a budget it is HANDED. That passes
// even if the per-frame caller hands it the whole list -- which is the defect itself. This
// drives the same function update() calls, so an unbounded frame goes red. (The first
// version of this test missed exactly that and passed with the fix reverted.)
static void check_per_frame_resolve_is_bounded(void) {
	screen_sworld_test_reset(640, 480);
	const size_t total = screen_sworld_test_row_count();
	assert(total > 64 && "fixture too small to distinguish bounded from eager");

	size_t first = screen_sworld_test_resolve_frame();
	assert(first > 0 && "a frame must make progress");
	assert(first <= 64
		   && "ONE FRAME resolved more than the hard per-frame ceiling -- that is an "
			  "unbounded SD sweep on the main thread again, the >3s stall this fixes");
	assert(screen_sworld_test_pending() > 0
		   && "the whole list resolved in a single frame");

	// ...and it still terminates: repeated frames drain the list exactly once.
	size_t frames = 1, sum = first;
	while(screen_sworld_test_resolve_frame() > 0) {
		frames++;
		assert(frames <= total + 2 && "per-frame resolve is not terminating");
	}
	(void)sum;
	assert(screen_sworld_test_pending() == 0);
	assert(screen_sworld_test_archive_opens() == total
		   && "the per-frame path must open each row exactly once");
	assert(frames >= total / 64
		   && "fewer frames than the ceiling allows means the budget was ignored");
	printf("  [2b] the per-frame resolve is bounded (%zu rows over %zu frames)\n", total,
		   frames);
}

// --- (3) EQUIVALENCE with the old eager sweep, at every budget ------------------------
// The corrected assertion (the handoff's "the final list is identical" is unsatisfiable:
// today an unparseable directory is never pushed, and incremental resolve must push before
// it knows). What must hold: every parseable row matches byte-for-byte and keeps its
// relative order; unparseable directories are additionally present, marked UNREADABLE.
static void check_equivalence_with_eager_sweep(void) {
	const size_t budgets[] = {1, 2, 7, 64};
	for(size_t b = 0; b < sizeof(budgets) / sizeof(budgets[0]); b++) {
		screen_sworld_test_reset(640, 480);
		while(screen_sworld_test_resolve_step(budgets[b]) > 0)
			;
		size_t ok_seen = 0, unreadable_seen = 0;
		for(size_t i = 0; i < screen_sworld_test_row_count(); i++) {
			const char *name, *dir, *path;
			int64_t last, size;
			int st = screen_sworld_test_row(i, &name, &dir, &path, &last, &size);
			assert(st >= 0);
			assert(st != ROW_PENDING && "a row was left unresolved");

			// Compare each row against an EAGER parse of its OWN directory rather than
			// against a pre-built ordered list: readdir order is a filesystem property
			// ("New World 10" precedes "New World 2"), so baking an order into the
			// reference would test the filesystem, not the split.
			struct ref_row r;
			bool parses = eager_parse(path, dir, &r);

			if(st == ROW_UNREADABLE) {
				unreadable_seen++;
				assert(!parses
					   && "a row marked UNREADABLE parses fine on an eager open -- the "
						  "incremental resolve lost data the old sweep kept");
				assert(strcmp(name, dir) == 0
					   && "an unreadable row must fall back to its directory name");
				continue;
			}
			ok_seen++;
			assert(parses
				   && "a row resolved OK that an eager open cannot parse");
			assert(strcmp(name, r.name) == 0 && "resolved LevelName differs from eager");
			assert(last == r.last_access && "resolved last_access differs from eager");
			assert(size == r.byte_size && "resolved byte_size differs from eager");
		}
		assert(ok_seen == ref_count && "a parseable world went missing");
		assert(unreadable_seen == 1 && "the level.dat-less directory should be UNREADABLE");

		// Order must be STABLE across budgets: the same rows in the same positions
		// regardless of K, so the visible list never depends on frame timing.
		if(b == 0) {
			for(size_t i = 0; i < screen_sworld_test_row_count(); i++) {
				const char* d;
				screen_sworld_test_row(i, NULL, &d, NULL, NULL, NULL);
				snprintf(order_ref[i], sizeof(order_ref[0]), "%s", d);
			}
			order_ref_count = screen_sworld_test_row_count();
		} else {
			assert(screen_sworld_test_row_count() == order_ref_count);
			for(size_t i = 0; i < order_ref_count; i++) {
				const char* d;
				screen_sworld_test_row(i, NULL, &d, NULL, NULL, NULL);
				assert(strcmp(d, order_ref[i]) == 0
					   && "row order changed with the resolve budget -- the list must not "
						  "depend on frame timing");
			}
		}
	}
	printf("  [3] resolved rows are byte-identical to the eager sweep at every budget\n");
}

// --- (4) re-entry mid-resolve is clean ------------------------------------------------
// Reachable in the real UI: the texture-pack screen returns here. A half-resolved list must
// be torn down and rebuilt without leaking or double-freeing the three strings per row.
// Run under ASan (tools/local_ci.sh) -- that is where this one earns its keep.
static void check_reentry_mid_resolve(void) {
	screen_sworld_test_reset(640, 480);
	assert(screen_sworld_test_resolve_step(3) == 3);
	screen_sworld_test_reset(640, 480); // re-enter with 3 rows resolved
	assert(screen_sworld_test_archive_opens() == 0
		   && "re-entering the screen must restart the resolve, not continue it");
	assert(screen_sworld_test_pending() == screen_sworld_test_row_count());
	while(screen_sworld_test_resolve_step(8) > 0)
		;
	assert(screen_sworld_test_pending() == 0);
	printf("  [4] re-entry mid-resolve rebuilds cleanly (ASan-relevant)\n");
}

// --- (4b) world-list owner allocation failure is fail-closed -------------------------
// The shipping Wii build compiles out stack_create's assert. Writing the stack fields
// through a NULL outer allocation was therefore a DSI crash under heap pressure.
static void check_owner_allocation_failure(void) {
	screen_sworld_test_set_owner_alloc_hook(reject_world_list_owner_alloc);
	screen_sworld_test_reset(640, 480);
	screen_sworld_test_set_owner_alloc_hook(NULL);
	assert(screen_sworld_test_row_count() == 0);
	assert(screen_sworld_test_archive_opens() == 0);
	/* The empty fallback is temporary: re-entering after memory pressure clears
	 * must enumerate normally rather than leaving the menu permanently wedged. */
	screen_sworld_test_reset(640, 480);
	assert(screen_sworld_test_row_count() == N_WORLDS + 1);
	printf("  [4b] owner OOM yields an empty list and recovers on re-entry\n");
}

// --- (5) the selection-bounds underflow ------------------------------------------------
// `gui_selection < stack_size(worlds) - 1` is `< SIZE_MAX` on an empty list, so DOWN always
// incremented. It stayed latent only because the list was fully built before any input
// could arrive; rows now appear over several frames, so it is pinned here.
static void check_selection_bounds(void) {
	assert(screen_sworld_test_next_selection(0, 0, false, true) == 0
		   && "DOWN on an EMPTY list must not move -- count-1 underflows to SIZE_MAX");
	assert(screen_sworld_test_next_selection(0, 0, true, false) == 0);
	assert(screen_sworld_test_next_selection(0, 1, false, true) == 0);
	assert(screen_sworld_test_next_selection(3, 5, false, true) == 4);
	assert(screen_sworld_test_next_selection(4, 5, false, true) == 4);
	assert(screen_sworld_test_next_selection(4, 5, true, false) == 3);
	assert(screen_sworld_test_next_selection(0, 5, true, false) == 0);
	// A stale selection from a longer previous list must be clamped, not left dangling.
	assert(screen_sworld_test_next_selection(99, 5, false, false) == 4);
	printf("  [5] selection bounds hold on an empty list and clamp a stale index\n");
}

static void remove_test_world(const char* directory) {
	char path[320];
	// Precision-bounded (%.300s): `directory` is a plain const char*, so once
	// its callers can hand it a runtime fixture buffer (cleanup_create_fixture
	// below) GCC can no longer prove boundedness from the declared array size
	// alone across the inlined call and defaults to a large worst case
	// -Wformat-truncation warning; bounding the format string lets it prove
	// no truncation instead, matching tests/support/test_fixture_dir.h's own
	// %.511s convention.
	snprintf(path, sizeof(path), "%.300s/level.dat", directory);
	remove(path);
	snprintf(path, sizeof(path), "%.300s/level.dat_new", directory);
	remove(path);
	snprintf(path, sizeof(path), "%.300s/level.dat_old", directory);
	remove(path);
	snprintf(path, sizeof(path), "%.300s/region", directory);
	RMDIR(path);
	RMDIR(directory);
}

static void cleanup_create_fixture(void) {
	// Was a `static const char*[]` array of test_fixture_join() call results:
	// invalid now that CREATE_ROOT is a runtime buffer (a static initializer
	// must be a compile-time constant), and even without `static` the join()
	// ring buffer only holds 4 slots at a time -- the 5th join() call below
	// ("New World 2") would silently overwrite one of these four before the
	// loop got to read it back. Persistent per-entry buffers avoid both
	// problems.
	char worlds[4][256];
	snprintf(worlds[0], sizeof(worlds[0]), "%s/nested/saves/New World", CREATE_ROOT);
	snprintf(worlds[1], sizeof(worlds[1]), "%s/nested/saves/New World 3", CREATE_ROOT);
	snprintf(worlds[2], sizeof(worlds[2]), "%s/nested/saves/New World 999", CREATE_ROOT);
	snprintf(worlds[3], sizeof(worlds[3]), "%s/nested/saves/New World 1000", CREATE_ROOT);
	for(size_t i = 0; i < sizeof(worlds) / sizeof(worlds[0]); i++)
		remove_test_world(worlds[i]);
	remove(test_fixture_join(CREATE_ROOT, "/nested/saves/New World 2"));
	RMDIR(test_fixture_join(CREATE_ROOT, "/nested/saves"));
	RMDIR(test_fixture_join(CREATE_ROOT, "/nested"));
	RMDIR(CREATE_ROOT);
}

// --- (6) new-world creation is exclusive and persistence-gated -----------------------
static void check_new_world_creation_safety(void) {
	cleanup_create_fixture();
	char path[256];
	/* Trimming must stop at the actual root, not the one-character Windows
	 * drive special case. In particular, "sd:/" is a complete Wii device root;
	 * turning it into "sd:" makes fresh-world creation fail on hardware. */
	assert(screen_sworld_test_normalize_directory_path(
		"sd:/", path, sizeof(path)));
	assert(strcmp(path, "sd:/") == 0);
	assert(screen_sworld_test_normalize_directory_path(
		"usb:/apps///", path, sizeof(path)));
	assert(strcmp(path, "usb:/apps") == 0);
	assert(screen_sworld_test_normalize_directory_path(
		"//", path, sizeof(path)));
	assert(strcmp(path, "//") == 0);
	assert(screen_sworld_test_normalize_directory_path(
		"relative/saves///", path, sizeof(path)));
	assert(strcmp(path, "relative/saves") == 0);

	assert(screen_sworld_test_reserve_directory(
		test_fixture_join(CREATE_ROOT, "/nested/saves"), 1, path, sizeof(path)));
	assert(strcmp(path, test_fixture_join(CREATE_ROOT, "/nested/saves/New World")) == 0
		   && "the saves path must be created recursively on a fresh install");

	/* Existing files are occupied names just like directories. The mkdir call
	 * itself reserves the winner, eliminating the old stat-then-mkdir race. */
	FILE* occupied = fopen(test_fixture_join(CREATE_ROOT, "/nested/saves/New World 2"), "wb");
	assert(occupied);
	fclose(occupied);
	assert(screen_sworld_test_reserve_directory(
		test_fixture_join(CREATE_ROOT, "/nested/saves///"), 1, path, sizeof(path)));
	assert(strcmp(path, test_fixture_join(CREATE_ROOT, "/nested/saves/New World 3")) == 0);

	/* Regression for the old n<1000 cap, which selected and overwrote World 999. */
	assert(MKDIR(test_fixture_join(CREATE_ROOT, "/nested/saves/New World 999")) == 0);
	assert(screen_sworld_test_reserve_directory(
		test_fixture_join(CREATE_ROOT, "/nested/saves"), 999, path, sizeof(path)));
	assert(strcmp(path, test_fixture_join(CREATE_ROOT, "/nested/saves/New World 1000")) == 0);

	vec3 world_spawn = {8.0F, 70.0F, 8.0F};
	vec3 player_spawn = {8.0F, 71.62F, 8.0F};
	assert(screen_sworld_test_persist_new(
		test_fixture_join(CREATE_ROOT, "/nested/saves/New World"), 1234, world_spawn, player_spawn,
		true));
	string_t world_path;
	string_init_set_str(world_path, test_fixture_join(CREATE_ROOT, "/nested/saves/New World"));
	struct level_archive la;
	assert(level_archive_create(&la, world_path));
	vec3 saved_pos, saved_vel;
	vec2 saved_rot;
	enum world_dim saved_dim;
	assert(level_archive_read_player(
		&la, saved_pos, saved_rot, saved_vel, &saved_dim));
	assert(saved_dim == WORLD_DIM_NETHER);
	assert(saved_pos[0] == player_spawn[0] && saved_pos[1] == player_spawn[1]
		   && saved_pos[2] == player_spawn[2]);
	level_archive_destroy(&la);
	string_clear(world_path);

	/* create_new succeeds in memory even when the directory is absent; the
	 * explicit save result must still make the operation fail. */
	assert(!screen_sworld_test_persist_new(
		test_fixture_join(CREATE_ROOT, "/missing/world"), 5678, world_spawn, player_spawn, false));
	struct stat st;
	assert(stat(test_fixture_join(CREATE_ROOT, "/missing/world/level.dat"), &st) != 0);

	cleanup_create_fixture();
	printf("  [6] new-world directories are exclusive; load is gated on a durable save\n");
}

// --- (7) the LCE "(Creative)" world-list suffix -------------------------------------
// docs/ui_design/create_world_console_2026-09-01.md 3.1 + maintainer decision 2:
// a creative world is labelled in the list, a survival one is not. The flag
// must come from the row's OWN level.dat and cost NO extra archive open --
// it is read inside the same resolve step that already reads LevelName.
static void make_mode_world(const char* dir, const char* name, bool creative) {
	MKDIR(dir);
	string_t p;
	string_init_set_str(p, dir);
	struct level_archive la;
	vec3 spawn = {8.0F, 70.0F, 8.0F};
	assert(level_archive_create_new(&la, p, 99, spawn));
	assert(level_archive_write(&la, LEVEL_NAME, (void*)name));
	// Survival writes NOTHING: absent tag == survival, so the survival row's
	// level.dat is byte-identical to a pre-tag world's.
	if(creative)
		assert(level_archive_write_game_mode(&la, true));
	assert(level_archive_save(&la));
	level_archive_discard(&la);
	string_clear(p);
}

static void check_creative_suffix(void) {
	MKDIR(MODE_ROOT);
	make_mode_world(test_fixture_join(MODE_ROOT, "/plain"), "Plain", false);
	make_mode_world(test_fixture_join(MODE_ROOT, "/fancy"), "Fancy", true);
	config_set_string(&gstate.config_user, "paths.worlds", MODE_ROOT);

	screen_sworld_test_reset(640, 480);
	assert(screen_sworld_test_row_count() == 2);
	// PENDING rows report survival -- the flag is never guessed from a name.
	for(size_t i = 0; i < 2; i++)
		assert(!screen_sworld_test_row_creative(i));

	size_t before = screen_sworld_test_archive_opens();
	while(screen_sworld_test_pending() > 0)
		(void)screen_sworld_test_resolve_step(8);
	// EXACTLY one open per row: reading the mode is free.
	assert(screen_sworld_test_archive_opens() - before == 2);

	bool saw_plain = false, saw_fancy = false;
	for(size_t i = 0; i < 2; i++) {
		const char* name = NULL;
		int st = screen_sworld_test_row(i, &name, NULL, NULL, NULL, NULL);
		assert(st == ROW_OK);
		if(strcmp(name, "Plain") == 0) {
			saw_plain = true;
			assert(!screen_sworld_test_row_creative(i));
		} else {
			assert(strcmp(name, "Fancy") == 0);
			saw_fancy = true;
			assert(screen_sworld_test_row_creative(i));
		}
	}
	assert(saw_plain && saw_fancy);

	screen_sworld_test_delete_tree(MODE_ROOT);
	printf("  [7] the world list labels a creative world and nothing else\n");
}

// --- (8) the shipped tutorial is hidden from Play game > Load (maintainer 10-31) ----
// A saves dir holding saves/CaveWii Tutorial (LevelName "Tutorial Island") plus two
// ordinary worlds lists ONLY the two. The filter keys on the folder constant the
// title's Tutorial row opens (SCREEN_TITLE_TUTORIAL_DIR), never the display name: a
// player's own world NAMED "Tutorial Island" in another folder still lists. The
// autoload-by-directory route still resolves the tutorial's path.
static char s_fixture_TUT_ROOT[128];
static void check_tutorial_hidden(void) {
	test_fixture_dir("test_world_tutorial_tmp", s_fixture_TUT_ROOT,
					 sizeof s_fixture_TUT_ROOT);
	MKDIR(s_fixture_TUT_ROOT);
	char tut[256];
	snprintf(tut, sizeof(tut), "%s/%s", s_fixture_TUT_ROOT, SCREEN_TITLE_TUTORIAL_DIR);
	make_mode_world(tut, "Tutorial Island", false);
	make_mode_world(test_fixture_join(s_fixture_TUT_ROOT, "/World A"), "World A", false);
	make_mode_world(test_fixture_join(s_fixture_TUT_ROOT, "/World B"),
					"Tutorial Island", false); /* same DISPLAY name: must still list */
	config_set_string(&gstate.config_user, "paths.worlds", s_fixture_TUT_ROOT);

	char out[512];
	screen_sworld_test_reset(640, 480);
	assert(screen_sworld_test_row_count() == 2
		   && "the tutorial folder must not be a Play game > Load row");
	bool saw_a = false, saw_b = false;
	for(size_t i = 0; i < 2; i++) {
		const char* dir = NULL;
		(void)screen_sworld_test_row(i, NULL, &dir, NULL, NULL, NULL);
		assert(dir && strcmp(dir, SCREEN_TITLE_TUTORIAL_DIR) != 0);
		if(strcmp(dir, "World A") == 0) saw_a = true;
		if(strcmp(dir, "World B") == 0) saw_b = true;
	}
	assert(saw_a && saw_b);
	while(screen_sworld_test_pending() > 0)
		(void)screen_sworld_test_resolve_step(8);
	for(size_t i = 0; i < 2; i++) {
		const char* dir = NULL;
		(void)screen_sworld_test_row(i, NULL, &dir, NULL, NULL, NULL);
		assert(strcmp(dir, SCREEN_TITLE_TUTORIAL_DIR) != 0);
	}

	/* Autoload by the exact folder name still reaches it; any other name, or
	 * the display name, does not take the hidden route. */
	assert(screen_sworld_test_hidden_autoload_path(SCREEN_TITLE_TUTORIAL_DIR, out,
													sizeof(out)));
	assert(strcmp(out, tut) == 0);
	assert(!screen_sworld_test_hidden_autoload_path("Tutorial Island", out, sizeof(out)));
	assert(!screen_sworld_test_hidden_autoload_path("World A", out, sizeof(out)));

	/* The Tutorial button's lookup (<paths.worlds>/SCREEN_TITLE_TUTORIAL_DIR/level.dat,
	 * screen_title.c title_tutorial_path) still resolves the folder. */
	char probe[600];
	snprintf(probe, sizeof(probe), "%s/level.dat", tut);
	FILE* f = fopen(probe, "rb");
	assert(f && "the Tutorial button's path must still resolve the tutorial");
	fclose(f);

	/* Without the folder, autoload by its name has no hidden route either. */
	screen_sworld_test_delete_tree(tut);
	screen_sworld_test_reset(640, 480);
	assert(screen_sworld_test_row_count() == 2);
	assert(!screen_sworld_test_hidden_autoload_path(SCREEN_TITLE_TUTORIAL_DIR, out,
													 sizeof(out)));

	screen_sworld_test_delete_tree(s_fixture_TUT_ROOT);
	test_fixture_dir_cleanup(s_fixture_TUT_ROOT);
	printf("  [8] the tutorial folder is hidden from the Load list; autoload by its "
		   "folder still resolves it\n");
}

// --- (9) a MISSING saves folder is created, not a permanent read error ----------------
// ci-1101/27: a fresh card (or a paths.worlds pointing at a folder nothing has made yet)
// opened the Load tab on "Couldn't read the saves folder - press Retry", and Retry only
// re-ran the same failing opendir. ENOENT is created in place; a path that exists but is
// not a directory is a REAL error and keeps Retry.
static char s_fixture_MISSING_ROOT[128];
/* Exported by screen_select_world.c for tests; declared ad hoc as in
 * test_frontend_no_pointer.c. */
bool screen_sworld_test_hint(char* text, size_t cap, int* y, int* h,
							 int* list_bottom, int* strip_top);
static void check_missing_saves_folder(void) {
	test_fixture_dir("test_world_missing_tmp", s_fixture_MISSING_ROOT,
					 sizeof s_fixture_MISSING_ROOT);
	char saves[256];
	snprintf(saves, sizeof(saves), "%s/nested/saves", s_fixture_MISSING_ROOT);
	struct stat st;
	assert(stat(saves, &st) != 0 && "fixture must start absent");

	config_set_string(&gstate.config_user, "paths.worlds", saves);
	screen_sworld_test_reset(640, 480);
	char hint[128] = "x";
	bool drawn = screen_sworld_test_hint(hint, sizeof(hint), NULL, NULL, NULL, NULL);
	assert(!drawn && hint[0] == '\0' && "a missing saves folder is not a read error");
	assert(screen_sworld_test_row_count() == 0);
	assert(stat(saves, &st) == 0 && (st.st_mode & S_IFDIR)
		   && "reset() creates the missing saves tree");

	/* Counter-case: the path exists but is a regular FILE. That is not ENOENT and
	 * nothing may be created over it -- the error with Retry stays. */
	char file_path[256];
	snprintf(file_path, sizeof(file_path), "%s/not_a_dir", s_fixture_MISSING_ROOT);
	FILE* f = fopen(file_path, "wb");
	assert(f);
	fputs("x", f);
	fclose(f);
	config_set_string(&gstate.config_user, "paths.worlds", file_path);
	screen_sworld_test_reset(640, 480);
	drawn = screen_sworld_test_hint(hint, sizeof(hint), NULL, NULL, NULL, NULL);
	assert(drawn && strstr(hint, "Retry") && "a non-directory path keeps the Retry error");
	assert(screen_sworld_test_row_count() == 0);
	assert(stat(file_path, &st) == 0 && !(st.st_mode & S_IFDIR)
		   && "the file is left untouched");

	config_set_string(&gstate.config_user, "paths.worlds", SAVES);
	screen_sworld_test_delete_tree(s_fixture_MISSING_ROOT);
	test_fixture_dir_cleanup(s_fixture_MISSING_ROOT);
	printf("  [9] a missing saves folder is created on reset; a file in its place keeps "
		   "Retry\n");
}

int main(void) {
	test_fixture_dir("test_world_list_tmp", s_fixture_SAVES, sizeof s_fixture_SAVES);
	test_fixture_dir("test_world_create_tmp", s_fixture_CREATE_ROOT, sizeof s_fixture_CREATE_ROOT);
	test_fixture_dir("test_world_mode_tmp", s_fixture_MODE_ROOT, sizeof s_fixture_MODE_ROOT);
	printf("test_world_list_incremental: P3(a) incremental world-list resolve\n");
	build_fixture();
	ref_count = N_WORLDS; // every synthetic world parses; the hostile one does not
	point_config_at_fixture();

	check_reset_does_no_archive_work();
	check_budget_is_exact();
	check_per_frame_resolve_is_bounded();
	check_equivalence_with_eager_sweep();
	check_reentry_mid_resolve();
	check_owner_allocation_failure();
	check_selection_bounds();
	check_new_world_creation_safety();
	check_creative_suffix();
	check_tutorial_hidden();
	check_missing_saves_folder();

	printf("test_world_list_incremental: PASS\n");
	test_fixture_dir_cleanup(SAVES);
	test_fixture_dir_cleanup(CREATE_ROOT);
	test_fixture_dir_cleanup(MODE_ROOT);
	return 0;
}
