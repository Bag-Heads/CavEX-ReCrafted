/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * ONE POINTER CLICK CHOOSES A LIST ROW (ui-build-1026 T01; evidence
 * tmp/jobs/design-hub-1024/investigate/REPORT.md items 1-2).
 *
 * Before T01 a Wii Remote pointer press on an unselected row of the Load world
 * list or the Texture packs list was a FOCUS_ONLY: the first click only moved
 * the highlight, a second click chose the row. This suite drives the REAL
 * screen_select_world / screen_texturepacks update() handlers through the REAL
 * PLATFORM_WII input.c with real IR pointer reports (the shared
 * test_frontend_no_pointer rig, included whole so every fixture, geometry
 * constant and observation hook is the one that suite already pins) and
 * asserts:
 *   1. Load: ONE click on a row selects it AND focuses Play, and launches
 *      nothing; a second click on that row still launches nothing (a click
 *      must never instantly activate Play); Play then needs its own press
 *      with the sprite on it.
 *   2. Packs: ONE click on a row selects it AND records it as the pending
 *      pick (Apply becomes live), and applies nothing; a second click
 *      applies nothing either.
 * The rig's own main() is renamed away; this file has its own sandbox
 * directory so it can run beside test_frontend_no_pointer under ctest -j.
 */
#define main frontend_no_pointer_suite_main
#include "test_frontend_no_pointer.c"
#undef main

#define OC_RUN_DIR "tmp_menu_one_click_rows_run"

static void case_load_one_click(bool remapped) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	const size_t rows = 3;
	fe_saves(rows);
	fe_attach(FE_REMOTE);
	fe_open(&screen_select_world);
	for(int f = 0; f < 8; f++)
		fe_step();
	assert(screen_sworld_test_row_count() == rows);
	size_t before = screen_sworld_test_list_selection();
	assert(before != 2 && "the fixture already selects row 2");

	/* Rest the sprite on row 2 (two frames, so hover is settled), ONE tap. */
	fe_pointer(fe_load_cx(FE_LW_ROW(2)), fe_load_cy(FE_LW_ROW(2)));
	fe_step();
	fe_step();
	fe_tap(r.click);
	if(screen_sworld_test_list_selection() != 2
	   || screen_sworld_test_focus() != FE_LW_PLAY)
		fprintf(stderr, "[load %s] after ONE click: selection %zu focus %d "
				"(want 2 / Play %d)\n", remapped ? "remapped" : "shipped",
				screen_sworld_test_list_selection(), screen_sworld_test_focus(),
				FE_LW_PLAY);
	assert(screen_sworld_test_list_selection() == 2
		   && screen_sworld_test_focus() == FE_LW_PLAY
		   && "ONE pointer click on a Load row did not select it and focus "
			  "Play");
	assert(fe_screen_sets == 0 && "the row click launched / left the screen");

	/* The fresh-press guard on Play is untouched: clicking the row AGAIN
	 * (sprite still on it, ring on Play) re-chooses the row, never Play. */
	fe_tap(r.click);
	assert(fe_screen_sets == 0
		   && "a second click on the row activated Play instead");
	assert(screen_sworld_test_list_selection() == 2
		   && screen_sworld_test_focus() == FE_LW_PLAY);

	/* A different row: again one click. */
	fe_pointer(fe_load_cx(FE_LW_ROW(0)), fe_load_cy(FE_LW_ROW(0)));
	fe_step();
	fe_step();
	fe_tap(r.click);
	assert(screen_sworld_test_list_selection() == 0
		   && screen_sworld_test_focus() == FE_LW_PLAY && fe_screen_sets == 0
		   && "ONE click on a second row did not choose it");

	/* Play needs the sprite ON it and its own press. */
	fe_pointer(fe_load_cx(FE_LW_PLAY), fe_load_cy(FE_LW_PLAY));
	fe_step();
	fe_step();
	fe_tap(r.click);
	assert(fe_screen_last == &screen_load_world
		   && "the pointer's Play press did not launch the chosen world");
	fe_pointer_off();
	printf("PASS load (%s bindings): one click selects the row and focuses "
		   "Play; the row click never launches; Play launches on its own "
		   "press\n", remapped ? "remapped" : "shipped");
}

static void case_packs_one_click(bool remapped) {
	struct fe_roles r = fe_roles_for(FE_REMOTE);
	char active_before[128];
	snprintf(active_before, sizeof(active_before), "%s",
			 config_read_string(&gstate.config_user, "paths.texturepack", ""));

	fe_attach(FE_REMOTE);
	screen_texturepacks_set_return(&screen_title);
	screen_texturepacks_set_ingame(false);
	fe_open(&screen_texturepacks);
	screen_texpacks_test_seed(FE_LONG_LIST);
	assert(screen_texpacks_test_selection() == 0);
	assert(screen_texpacks_test_scroll() == 0);
	size_t pending_before = screen_texpacks_test_pending();
	assert(pending_before != 1 && "the fixture already picks row 1");

	fe_pointer(320.0F, (float)(FE_TP_ROW_TOP_PX(1) + FE_TP_ROW_ACTION_PX / 2));
	fe_step();
	fe_step();
	fe_tap(r.click);
	if(screen_texpacks_test_pending() != 1)
		fprintf(stderr, "[packs %s] after ONE click: selection %zu pending "
				"%zu apply %d\n", remapped ? "remapped" : "shipped",
				screen_texpacks_test_selection(),
				screen_texpacks_test_pending(),
				(int)screen_texpacks_test_apply_enabled());
	assert(fe_screen_last == NULL && "a pack row click left the screen");
	assert(screen_texpacks_test_selection() == 1
		   && screen_texpacks_test_pending() == 1
		   && screen_texpacks_test_apply_enabled()
		   && "ONE pointer click on a pack row did not choose it (pending "
			  "pick + live Apply)");
	assert(!strcmp(config_read_string(&gstate.config_user, "paths.texturepack",
									  ""),
				   active_before)
		   && "a pack row click applied the pack");

	/* A second click on the same row applies nothing either. */
	fe_tap(r.click);
	assert(fe_screen_last == NULL && screen_texpacks_test_pending() == 1);
	fe_pointer_off();
	fe_step();
	fe_tap(r.down);
	assert(screen_texpacks_test_selection() == 2
		   && "a pack row click armed the deferred apply (the screen froze)");
	assert(!strcmp(config_read_string(&gstate.config_user, "paths.texturepack",
									  ""),
				   active_before)
		   && "a pack row click applied the pack");
	printf("PASS packs (%s bindings): one click selects the pack and makes it "
		   "the pending pick; row clicks never apply\n",
		   remapped ? "remapped" : "shipped");
}

int main(void) {
	remove(OC_RUN_DIR "/config.json");
	remove(OC_RUN_DIR "/config_wii.json");
	remove(OC_RUN_DIR "/" FE_REMAP_CONFIG);
	for(size_t k = 0; k < 32; k++) {
		char path[192];
		snprintf(path, sizeof(path), OC_RUN_DIR "/" FE_SAVES_DIR "/world%02zu",
				 k);
		fe_rmdir(path);
	}
	fe_rmdir(OC_RUN_DIR "/" FE_SAVES_DIR);
	fe_rmdir(OC_RUN_DIR);
	assert((fe_mkdir(OC_RUN_DIR) == 0 || errno == EEXIST)
		   && "could not create the scratch run directory");
	fe_copy_file("config_wii.json", OC_RUN_DIR "/config_wii.json");
	assert(fe_chdir(OC_RUN_DIR) == 0 && "could not enter the scratch run dir");

	for(int remapped = 0; remapped < 2; remapped++) {
		fe_config_open(remapped != 0);
		case_load_one_click(remapped != 0);
		case_packs_one_click(remapped != 0);
	}

	fe_saves(0);
	remove(FE_REMAP_CONFIG);
	remove("config_wii.json");
	remove("config.json");
	config_destroy(&gstate.config_user);
	assert(fe_chdir("..") == 0);
	fe_rmdir(OC_RUN_DIR);
	puts("PASS menu one-click rows");
	return 0;
}
