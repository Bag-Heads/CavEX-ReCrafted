/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * ci-1101/18: the title's splash when NO title/splashes.txt exists -- the
 * default install (none ships; Beta's list is Mojang content). Beta shows
 * "missingno" only because its jar always carries the list; a default CavEX
 * install showed that fixture-looking word on every boot. Rule: no list, no
 * splash (the date greetings still land, as in initGui). With a list the
 * roll is one of its lines.
 *
 * Same rig + TU set as test_title_tutorial (the real screen_title through the
 * real PLATFORM_WII input.c, in its own scratch run directory).
 */
#define main frontend_no_pointer_suite_main
#include "test_frontend_no_pointer.c"
#undef main

#define TS_RUN_DIR "tmp_title_splash_run"

const char* screen_title_test_splash(void);

/* The four initGui fixed-date greetings are legitimate without a list. */
static bool ts_is_date_greeting(const char* s) {
	return strcmp(s, "Happy birthday, ez!") == 0
		|| strcmp(s, "Happy birthday, Notch!") == 0
		|| strcmp(s, "Merry X-mas!") == 0
		|| strcmp(s, "Happy new year!") == 0;
}

static void ts_open_title(void) {
	fe_attach(FE_REMOTE);
	fe_open(&screen_title);
	fe_pointer_off();
	fe_step();
}

int main(void) {
	remove(TS_RUN_DIR "/assets/title/splashes.txt");
	fe_rmdir(TS_RUN_DIR "/assets/title");
	fe_rmdir(TS_RUN_DIR "/assets");
	remove(TS_RUN_DIR "/config_wii.json");
	fe_rmdir(TS_RUN_DIR "/" FE_SAVES_DIR);
	fe_rmdir(TS_RUN_DIR);
	assert((fe_mkdir(TS_RUN_DIR) == 0 || errno == EEXIST)
		   && "could not create the scratch run directory");
	fe_copy_file("config_wii.json", TS_RUN_DIR "/config_wii.json");
	assert(fe_chdir(TS_RUN_DIR) == 0 && "could not enter the scratch run dir");
	fe_config_open(false);
	fe_saves(0);
	/* config_wii.json's pack is "assets": neither candidate exists here. */
	assert(!fopen("assets/title/splashes.txt", "r"));

	for(int k = 0; k < 3; k++) {
		ts_open_title();
		const char* s = screen_title_test_splash();
		assert(strcmp(s, "missingno") != 0
			   && "no splashes.txt must draw NO splash, not 'missingno'");
		assert((s[0] == 0 || ts_is_date_greeting(s))
			   && "without a list only a date greeting may show");
	}
	puts("PASS no splashes.txt: empty splash (no 'missingno')");

	/* With a one-line list the roll is that line (unless a date greeting). */
	assert(fe_mkdir("assets") == 0 || errno == EEXIST);
	assert(fe_mkdir("assets/title") == 0 || errno == EEXIST);
	FILE* f = fopen("assets/title/splashes.txt", "w");
	assert(f);
	fputs("\n  Only line here!  \r\n\n", f);
	fclose(f);
	ts_open_title();
	const char* s = screen_title_test_splash();
	assert((strcmp(s, "Only line here!") == 0 || ts_is_date_greeting(s))
		   && "a present list is still rolled (trimmed)");
	puts("PASS splashes.txt present: its line is rolled");

	remove("assets/title/splashes.txt");
	fe_rmdir("assets/title");
	fe_rmdir("assets");
	fe_saves(0);
	remove("config_wii.json");
	remove("config.json");
	config_destroy(&gstate.config_user);
	assert(fe_chdir("..") == 0);
	fe_rmdir(TS_RUN_DIR);
	puts("PASS title splash absent");
	return 0;
}
