/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * THE TITLE'S TUTORIAL WORLD ROW (ui-build-1026 T03; approved design
 * tmp/jobs/design-round-1026/astra/astra-title*.png + title.json).
 *
 * Drives the REAL screen_title update() through the REAL PLATFORM_WII input.c
 * (the shared test_frontend_no_pointer rig, included whole with its main
 * renamed; own sandbox directory so it can run beside it under ctest -j):
 *   1. five rows in the approved order, D-pad DOWN walks Play game ->
 *      Tutorial world -> Texture packs -> Options -> Quit game, and the
 *      walker seam reports every row by its visual ordinal (Tutorial =
 *      SCREEN_TITLE_FOCUS_TUTORIAL), and the walker's OWN decision
 *      (menu_ap_focus_walk_button) driving this real screen reaches Play,
 *      Tutorial and Texture packs from every row within its press limit;
 *   2. with saves/CavEX Tutorial/level.dat present, ONE press (pad, and
 *      separately one pointer click) issues exactly one SRPC_LOAD_WORLD and
 *      shows the loading screen -- the Load tab's own load route -- for the
 *      world path <paths.worlds>/CavEX Tutorial, and the Play session
 *      remembers the tutorial directory (resume, not copy); a Beta-format
 *      world there is NOT loaded: the Load tab opens (its convert notice
 *      asks first);
 *   3. with it absent, the press sends NOTHING and opens the honest dialog;
 *      its Back returns to the title, its "Play game" opens the Load tab;
 *   4. the folder name the title looks for is the one the release packager
 *      (tools/make_release.sh) and the builder (build_tutorial.py) write.
 */
/* Two rig doubles are WRAPPED, not replaced: the rig's definitions are
 * renamed while it is included, and the real names below record what the
 * title passed and then defer to them (default behaviour unchanged). */
#define loading_notice_gate_load_world fe_rig_gate_load_world
#define level_archive_was_created_by_cavewii fe_rig_created_by_cavewii
#define level_archive_get_save_version fe_rig_save_version
#define level_archive_world_origin fe_rig_world_origin
#define main frontend_no_pointer_suite_main
#include "test_frontend_no_pointer.c"
#undef main
#undef loading_notice_gate_load_world
#undef level_archive_was_created_by_cavewii
#undef level_archive_get_save_version
#undef level_archive_world_origin

#include "game/menu_autopilot_script.h"

/* The world path of the last load request (the rig gate builds the
 * SRPC_LOAD_WORLD payload from exactly this argument). */
static char tt_load_path[512];
bool loading_notice_gate_load_world(const char* world_path,
									struct screen* back) {
	snprintf(tt_load_path, sizeof(tt_load_path), "%s",
			 world_path ? world_path : "(null)");
	return fe_rig_gate_load_world(world_path, back);
}

/* true: every level.dat reads as a Minecraft Beta world (no CaveWii tag,
 * save version 0) -- import_convert_notice_needed() answers true. */
static bool tt_beta_world;
bool level_archive_was_created_by_cavewii(const struct level_archive* la) {
	return tt_beta_world ? false : fe_rig_created_by_cavewii(la);
}
int32_t level_archive_get_save_version(const struct level_archive* la) {
	return tt_beta_world ? 0 : fe_rig_save_version(la);
}
/* dropin-1029: a REAL Beta world is drop-in (no notice); the notice -- and
 * so this case -- now means a pre-2026-06-11 CavEX dev save. */
enum level_world_origin level_archive_world_origin(const struct level_archive* la) {
	return tt_beta_world ? LEVEL_ORIGIN_CAVEWII_LEGACY : fe_rig_world_origin(la);
}

/* screen_confirm.c's CAVEWII_CLIENT_INTERFACE_TEST seam (as
 * test_import_convert_notice declares it). */
bool screen_confirm_test_answer(bool yes);
int screen_confirm_test_lines(const char* text[3], int y[3], const char** yes_label,
							  const char** no_label, int* safe_answer);

#define TT_RUN_DIR "tmp_title_tutorial_run"
#define TT_WORLD FE_SAVES_DIR "/" SCREEN_TITLE_TUTORIAL_DIR
#define TT_LEGACY_WORLD FE_SAVES_DIR "/" SCREEN_TITLE_TUTORIAL_LEGACY_DIR

static void tt_named_world(const char* path, bool present) {
	char level[512];
	snprintf(level, sizeof(level), "%s/level.dat", path);
	remove(level);
	fe_rmdir(path);
	if(!present)
		return;
	assert(fe_mkdir(FE_SAVES_DIR) == 0 || errno == EEXIST);
	assert(fe_mkdir(path) == 0 || errno == EEXIST);
	FILE* f = fopen(level, "wb");
	assert(f && "could not write the fixture level.dat");
	fputs("fixture", f); /* content is the rig's level_archive double's */
	fclose(f);
}

static void tt_tutorial_world(bool present) {
	tt_named_world(TT_WORLD, present);
}

/* 1. order + walker seam. */
static void case_rows_and_walker(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer_off();
	fe_step();
	assert(screen_title_test_highlight() == FE_TB_PLAY);
	assert(screen_title_focused_row() == SCREEN_TITLE_FOCUS_PLAY);
	fe_tap(r.down);
	assert(screen_title_test_highlight() == FE_TB_TUTORIAL
		   && "DOWN from Play game did not land on Tutorial world");
	assert(screen_title_focused_row() == SCREEN_TITLE_FOCUS_TUTORIAL
		   && "the Tutorial row did not report its own walker ordinal");
	assert(!fe_title_play() && "the Play seam claimed the Tutorial row");
	fe_tap(r.down);
	assert(screen_title_test_highlight() == FE_TB_PACKS
		   && screen_title_focused_row() == SCREEN_TITLE_FOCUS_PACKS);
	fe_tap(r.down);
	assert(screen_title_focused_row() == SCREEN_TITLE_FOCUS_OPTIONS);
	fe_tap(r.down);
	assert(screen_title_focused_row() == SCREEN_TITLE_FOCUS_QUIT);
	fe_tap(r.down); /* clamped */
	assert(screen_title_test_highlight() == FE_TB_QUIT);
	for(int k = 0; k < 4; k++)
		fe_tap(r.up);
	assert(screen_title_test_highlight() == FE_TB_PLAY && fe_title_play());
	assert(fe_screen_sets == 0 && "walking the title opened something");
	printf("PASS title rows: Play / Tutorial world / Texture Packs / Options "
		   "/ Quit; walker seam reports each row's own ordinal\n");
}

/* 1b. The walker's own decision on the REAL title: from every row, a walk to
 * Play / Tutorial / Texture packs lands within MENU_AP_FOCUS_WALK_MAX presses
 * and opens nothing on the way (Sol review: the old NONE ordinal bounced
 * Packs -> Tutorial -> Packs until the limit). */
static void case_walker_on_real_title(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	const int targets[] = {SCREEN_TITLE_FOCUS_PLAY,
						   SCREEN_TITLE_FOCUS_TUTORIAL,
						   SCREEN_TITLE_FOCUS_PACKS};
	for(int start = SCREEN_TITLE_FOCUS_PLAY; start <= SCREEN_TITLE_FOCUS_QUIT;
		start++) {
		for(size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); t++) {
			fe_attach(FE_REMOTE);
			fe_open(&screen_title);
			fe_pointer_off();
			fe_step();
			for(int k = 0; k < start; k++)
				fe_tap(r.down);
			assert(screen_title_focused_row() == start);
			fe_screen_sets = 0;
			int presses = 0, button;
			while((button = menu_ap_focus_walk_button(
					   screen_title_focused_row(), targets[t]))
				  != -1) {
				if(presses >= MENU_AP_FOCUS_WALK_MAX)
					fprintf(stderr, "walk %d -> %d stuck at %d\n", start,
							targets[t], screen_title_focused_row());
				assert(presses < MENU_AP_FOCUS_WALK_MAX
					   && "a title walk ran out of presses");
				fe_tap(button == IB_GUI_UP ? r.up : r.down);
				presses++;
			}
			assert(screen_title_focused_row() == targets[t]
				   && fe_screen_sets == 0);
			int distance = start > targets[t] ? start - targets[t]
											  : targets[t] - start;
			assert(presses == distance && "a title walk took a detour");
		}
	}
	printf("PASS walker on the real title: every row of 5 reaches Play / "
		   "Tutorial / Packs in exactly its distance (limit %d)\n",
		   MENU_AP_FOCUS_WALK_MAX);
}

static void tt_expect_load_directory(const char* how, const char* directory) {
	if(fe_rpcs != 1 || fe_rpc_last != SRPC_LOAD_WORLD
	   || fe_screen_last != &screen_load_world)
		fprintf(stderr, "[%s] rpcs %u last %d screen %p (want 1 / %d / %p)\n",
				how, fe_rpcs, fe_rpc_last, (void*)fe_screen_last,
				(int)SRPC_LOAD_WORLD, (void*)&screen_load_world);
	assert(fe_rpcs == 1 && fe_rpc_last == SRPC_LOAD_WORLD
		   && "the Tutorial row did not issue exactly one world load");
	assert(fe_screen_last == &screen_load_world
		   && "the Tutorial row did not show the loading screen");
	const char* worlds
		= config_read_string(&gstate.config_user, "paths.worlds", "saves");
	char want[512];
	snprintf(want, sizeof(want), "%s/%s", worlds, directory);
	if(strcmp(tt_load_path, want) != 0)
		fprintf(stderr, "[%s] load path \"%s\" (want \"%s\")\n", how,
				tt_load_path, want);
	assert(strcmp(tt_load_path, want) == 0
		   && "the load request named another world path");
	const char* remembered = play_session_world_dir();
	assert(remembered && strcmp(remembered, directory) == 0
		   && "the Play session does not remember the tutorial world");
	assert(!gstate.config_creative);
}

static void tt_expect_load(const char* how) {
	tt_expect_load_directory(how, SCREEN_TITLE_TUTORIAL_DIR);
}

/* 2. present: one pad press, then one pointer click, each loads it. */
static void case_tutorial_present(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	tt_tutorial_world(true);

	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer_off();
	fe_step();
	fe_tap(r.down);
	fe_rpcs = 0;
	fe_rpc_last = -1;
	fe_screen_last = NULL;
	fe_screen_sets = 0;
	tt_load_path[0] = 0;
	fe_tap(r.click);
	tt_expect_load("pad");

	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer(fe_title_cx(FE_TB_TUTORIAL), fe_title_cy(FE_TB_TUTORIAL));
	fe_step();
	fe_step();
	assert(screen_title_test_highlight() == FE_TB_TUTORIAL);
	fe_rpcs = 0;
	fe_rpc_last = -1;
	fe_screen_last = NULL;
	fe_screen_sets = 0;
	tt_load_path[0] = 0;
	fe_tap(r.click);
	tt_expect_load("pointer");
	fe_pointer_off();
	printf("PASS tutorial present: one press (pad or pointer) loads saves/%s "
		   "through SRPC_LOAD_WORLD + the loading screen; session remembers "
		   "it (resume)\n", SCREEN_TITLE_TUTORIAL_DIR);
}

/* 0.1.0 upgrade: legacy-only resumes; both always open the new folder.
 * An existing new directory without level.dat never falls back to legacy. */
static void case_tutorial_upgrade(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	tt_named_world(TT_LEGACY_WORLD, true);
	for(int state = 0; state < 3; state++) {
		tt_tutorial_world(state == 1);
		if(state == 2)
			assert(fe_mkdir(TT_WORLD) == 0);
		fe_attach(FE_REMOTE);
		fe_open(&screen_title);
		fe_pointer_off();
		fe_step();
		fe_tap(r.down);
		fe_rpcs = 0;
		fe_rpc_last = -1;
		fe_screen_last = NULL;
		fe_screen_sets = 0;
		tt_load_path[0] = 0;
		fe_tap(r.click);
		if(state == 2) {
			assert(fe_rpcs == 0 && tt_load_path[0] == 0);
			assert(fe_screen_last == &screen_confirm && fe_screen_sets == 1);
		} else {
			tt_expect_load_directory(state == 0 ? "legacy only" : "both",
				state == 0 ? SCREEN_TITLE_TUTORIAL_LEGACY_DIR
						   : SCREEN_TITLE_TUTORIAL_DIR);
		}
		FILE* f = fopen(TT_LEGACY_WORLD "/level.dat", "rb");
		char contents[16] = {0};
		assert(f);
		assert(fread(contents, 1, sizeof(contents), f) == 7);
		fclose(f);
		assert(strcmp(contents, "fixture") == 0);
	}
	tt_named_world(TT_LEGACY_WORLD, false);
	tt_tutorial_world(false);
	puts("PASS tutorial upgrade: legacy fallback, new preferred, legacy untouched; "
		 "existing new folder never falls back");
}

/* 2b. A Beta-format world in the tutorial folder: never loaded from the
 * title; the Load tab opens with it remembered, so ITS convert notice asks
 * first. */
static void case_tutorial_beta_format(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	tt_tutorial_world(true);
	tt_beta_world = true;
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer_off();
	fe_step();
	fe_tap(r.down);
	fe_rpcs = 0;
	fe_screen_last = NULL;
	fe_screen_sets = 0;
	tt_load_path[0] = 0;
	fe_tap(r.click);
	tt_beta_world = false;
	assert(fe_rpcs == 0 && tt_load_path[0] == 0
		   && "a Beta-format tutorial world was loaded past the convert notice");
	assert(fe_screen_last == &screen_select_world && fe_screen_sets == 1
		   && "a Beta-format tutorial world did not route to the Load tab");
	const char* remembered = play_session_world_dir();
	assert(remembered && strcmp(remembered, SCREEN_TITLE_TUTORIAL_DIR) == 0
		   && "the Load tab will not have the tutorial selected");
	tt_tutorial_world(false);
	printf("PASS tutorial Beta-format: no load; Load tab opens with it "
		   "selected (convert notice owns the ask)\n");
}

/* 3. absent: honest dialog, no load, both answers go somewhere real. */
static void case_tutorial_absent(void) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	tt_tutorial_world(false);
	for(int answer = 0; answer < 2; answer++) {
		fe_attach(FE_REMOTE);
		fe_open(&screen_title);
		fe_pointer_off();
		fe_step();
		fe_tap(r.down);
		fe_rpcs = 0;
		fe_screen_last = NULL;
		fe_screen_sets = 0;
		fe_tap(r.click);
		assert(fe_rpcs == 0 && "a missing tutorial world still sent a load");
		assert(fe_screen_last == &screen_confirm && fe_screen_sets == 1
			   && "a missing tutorial world did not explain itself");
		{
			const char* lines[3];
			int ys[3];
			(void)screen_confirm_test_lines(lines, ys, NULL, NULL, NULL);
			assert(lines[0]
				   && strcmp(lines[0], "Tutorial world not found - reinstall CavEX") == 0
				   && "the missing-tutorial message is not the plain reinstall text");
		}
		assert(screen_confirm_test_answer(answer != 0));
		if(answer)
			assert(fe_screen_last == &screen_select_world
				   && "the dialog's Play game did not open the Load tab");
		else
			assert(fe_screen_last == &screen_title
				   && "the dialog's Back did not return to the title");
		assert(fe_rpcs == 0);
	}
	printf("PASS tutorial absent: no load sent; dialog Back -> title, "
		   "Play game -> Load tab\n");
}

/* 4. one folder name across title, packager and builder. Read as TEXT from
 * the repo root before the sandbox moves the cwd. */
static bool tt_file_has(const char* path, const char* needle) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	static char buf[1 << 20];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	return strstr(buf, needle) != NULL;
}

static void case_folder_name_matches_release(void) {
	assert(tt_file_has("tools/make_release.sh",
					   "TUTORIAL_WORLD_DIR=\"" SCREEN_TITLE_TUTORIAL_DIR "\"")
		   && "make_release.sh stages the tutorial under another name");
	assert(tt_file_has("tools/tutorial_world/build_tutorial.py",
					   "\"saves\", \"" SCREEN_TITLE_TUTORIAL_DIR "\"")
		   && "build_tutorial.py DEFAULT_OUT names another folder");
	printf("PASS folder name: title, make_release.sh and build_tutorial.py "
		   "agree on \"%s\"\n", SCREEN_TITLE_TUTORIAL_DIR);
}

int main(void) {
	case_folder_name_matches_release();

	remove(TT_RUN_DIR "/" TT_WORLD "/level.dat");
	fe_rmdir(TT_RUN_DIR "/" TT_WORLD);
	remove(TT_RUN_DIR "/" TT_LEGACY_WORLD "/level.dat");
	fe_rmdir(TT_RUN_DIR "/" TT_LEGACY_WORLD);
	remove(TT_RUN_DIR "/config.json");
	remove(TT_RUN_DIR "/config_wii.json");
	remove(TT_RUN_DIR "/" FE_REMAP_CONFIG);
	fe_rmdir(TT_RUN_DIR "/" FE_SAVES_DIR);
	fe_rmdir(TT_RUN_DIR);
	assert((fe_mkdir(TT_RUN_DIR) == 0 || errno == EEXIST)
		   && "could not create the scratch run directory");
	fe_copy_file("config_wii.json", TT_RUN_DIR "/config_wii.json");
	assert(fe_chdir(TT_RUN_DIR) == 0 && "could not enter the scratch run dir");

	fe_config_open(false);
	fe_saves(0);
	case_rows_and_walker();
	case_walker_on_real_title();
	case_tutorial_present();
	case_tutorial_upgrade();
	case_tutorial_beta_format();
	case_tutorial_absent();

	tt_tutorial_world(false);
	fe_saves(0);
	remove(FE_REMAP_CONFIG);
	remove("config_wii.json");
	remove("config.json");
	config_destroy(&gstate.config_user);
	assert(fe_chdir("..") == 0);
	fe_rmdir(TT_RUN_DIR);
	puts("PASS title tutorial");
	return 0;
}
