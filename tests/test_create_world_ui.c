/* ui-build-1026 T05: the APPROVED Create screen (design-round-1026c/opus/
 * opus-B-create + NOTES.md B), driven through the REAL screen_create_world.c
 * on the PLATFORM_WII input rig, all three controller families.
 *
 * What is proven here, each through real presses / pointer frames and the
 * screen's own observers (never a second opinion on its geometry):
 *   1. Beta field order [GuiCreateWorld.drawScreen l.120-124]: DOWN walks
 *      Name -> Seed -> Game mode -> Create world, and wraps.
 *   2. "New seed" is a visible random number; Create submits exactly it.
 *   3. Seeds as Beta reads them [actionPerformed l.75-86], via the real Create
 *      press: empty = random, numeric = parseLong, "0" = random, text and an
 *      out-of-range number = String.hashCode.
 *   4. Details: RIGHT from Create world and LEFT from Cancel reach it; the
 *      card shows all four lines whole (wrapped, none cut) and Back closes it.
 *   5. Cancel and the Back role return to the Load tab with the draft kept;
 *      "+" creates from anywhere; a blank name disables Create (Beta l.107).
 *   6. Remote pointer: the approved rects (make_spec.py B-create) focus their
 *      controls. */

#include "support/frontend_screen_rig.h"

struct tex_gfx texture_menu_background;

#include "game/gui/cworld_form_model.h"
#include "game/gui/play_session.h"
#include "network/world_copy.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern struct cworld_form screen_cworld_test_snapshot(bool* details_is_open);
extern const char* screen_cworld_test_focus_name(void);
extern unsigned screen_cworld_test_last_submit(bool* seed_set, int64_t* seed);
extern int screen_cworld_test_details_lines(int width, int height,
											char lines[][128], int max_lines,
											bool* fits);
#ifdef _WIN32
#include <direct.h>
#define cw_mkdir(p) _mkdir(p)
#define cw_rmdir(p) _rmdir(p)
#define cw_chdir(p) _chdir(p)
#else
#include <unistd.h>
#define cw_mkdir(p) mkdir(p, 0777)
#define cw_rmdir(p) rmdir(p)
#define cw_chdir(p) chdir(p)
#endif

#define CW_RUN_DIR "tmp_create_world_ui_run"
#define CW_DT (1.0F / 60.0F)

/* ---- bindings: the shipped config is the source of truth ---------------- */
static int cw_code_for(enum fe_family fam, const char* key) {
	char dotted[64];
	snprintf(dotted, sizeof(dotted), "input.%s", key);
	int codes[16];
	size_t length = sizeof(codes) / sizeof(codes[0]);
	if(!config_read_int_array(&gstate.config_user, dotted, codes, &length))
		return -1;
	for(size_t k = 0; k < length; k++) {
		int c = codes[k];
		if(fam == FE_REMOTE && ((c >= 0 && c <= 10) || c == 100 || c == 101))
			return c;
		if(fam == FE_CLASSIC && c >= 200 && c <= 214)
			return c;
		if(fam == FE_GAMECUBE && c >= 400 && c <= 411)
			return c;
	}
	return -1;
}

struct cw_roles {
	int close, click, up, down, left, right, page;
};
static struct cw_roles cw_roles_for(enum fe_family fam) {
	struct cw_roles r = {
		cw_code_for(fam, "inventory"), cw_code_for(fam, "gui_click"),
		cw_code_for(fam, "gui_up"),	   cw_code_for(fam, "gui_down"),
		cw_code_for(fam, "gui_left"),  cw_code_for(fam, "gui_right"),
		cw_code_for(fam, "gui_page"),
	};
	assert(r.close > 0 && r.click >= 0 && r.up >= 0 && r.down > 0
		   && r.left > 0 && r.right > 0 && r.page > 0
		   && "a menu role is unbound for this family");
	return r;
}

/* ---- driving ------------------------------------------------------------ */
static void cw_open(void) {
	screen_set(&screen_create_world);
	fe_idle();
	fe_screen_last = NULL;
	fe_screen_sets = 0;
}
static void cw_tap(int code) {
	fe_hold(code);
	screen_create_world.update(&screen_create_world, CW_DT);
	fe_idle();
}
static void cw_step(void) {
	fe_idle();
	screen_create_world.update(&screen_create_world, CW_DT);
}
static const char* cw_focus(void) { return screen_cworld_test_focus_name(); }
static void cw_expect(const char* want, const char* why) {
	if(strcmp(cw_focus(), want) != 0) {
		fprintf(stderr, "[%s] focus %s, want %s: %s\n", fe_family_name[fe_family],
				cw_focus(), want, why);
		assert(!"focus mismatch");
	}
}

/* Enter with a retained draft (the screen's real RESUME path). */
static void cw_open_with(const char* name, const char* seed) {
	struct cworld_form f;
	cworld_form_reset(&f);
	snprintf(f.name, sizeof(f.name), "%s", name);
	snprintf(f.seed, sizeof(f.seed), "%s", seed);
	play_session_store_draft(&f);
	cw_open();
	struct cworld_form got = screen_cworld_test_snapshot(NULL);
	assert(!strcmp(got.name, name) && !strcmp(got.seed, seed));
}

/* Walk to Create world with DOWN only and press A; returns what it submitted. */
static void cw_create_submits(const struct cw_roles* r, bool* set,
							  int64_t* seed) {
	for(int k = 0; k < 8 && strcmp(cw_focus(), "create"); k++)
		cw_tap(r->down);
	cw_expect("create", "DOWN never reached Create world");
	unsigned before = screen_cworld_test_last_submit(NULL, NULL);
	unsigned attempts = fe_world_create_attempts;
	cw_tap(r->click);
	assert(screen_cworld_test_last_submit(set, seed) == before + 1
		   && "A on Create world did not submit");
	/* ...and it reached the real create path (the rig's archive refuses). */
	assert(fe_world_create_attempts == attempts + 1);
}

/* ---- cases -------------------------------------------------------------- */

static void case_journal_names_taken(const struct cw_roles* r) {
	/* Use the copier itself to produce a valid durable ownership journal. */
	assert(cw_mkdir("saves") == 0 || errno == EEXIST);
	assert(cw_mkdir("saves/Journal") == 0);
	struct world_copy* copy = world_copy_survey_begin("saves/Journal", 4096);
	assert(copy);
	unsigned steps = 0;
	while(world_copy_step(copy, 1) == WORLD_COPY_SURVEYING)
		assert(++steps < 1000);
	assert(world_copy_get_result(copy)->state == WORLD_COPY_READY);
	assert(!strcmp(world_copy_get_result(copy)->destination, "saves/Journal (backup)"));
	assert(world_copy_start(copy, UINT64_MAX) == WORLD_COPY_COPYING);
	struct stat marker;
	while(stat("saves/Journal (backup)/.incomplete", &marker) != 0) {
		assert(world_copy_step(copy, 1) == WORLD_COPY_COPYING);
		assert(++steps < 1000);
	}
	world_copy_release(copy); /* Simulate interruption, retaining journal/marker. */
	struct world_copy_journals* journals;
	assert(world_copy_journals_read("saves", &journals));
	assert(world_copy_journals_contains(journals, "saves/Journal (backup)"));
	world_copy_journals_free(journals);

	for(int absent = 0; absent < 2; absent++) {
		if(absent) {
			assert(remove("saves/Journal (backup)/.incomplete") == 0);
			assert(cw_rmdir("saves/Journal (backup)") == 0);
		}
		cw_open_with("Journal (backup)", "42");
		char lines[12][128];
		bool fits;
		assert(screen_cworld_test_details_lines(640, 480, lines, 12, &fits) > 0);
		assert(!strcmp(lines[0], "0|Will be saved in: Journal (backup)-"));
		char reserved[256];
		assert(screen_sworld_test_reserve_beta("saves", "Journal (backup)",
			reserved, sizeof(reserved)));
		assert(!strcmp(reserved, "saves/Journal (backup)-"));
		assert(cw_rmdir(reserved) == 0);
		bool set;
		int64_t seed;
		cw_create_submits(r, &set, &seed);
		struct stat st;
		assert(stat("saves/.world-copy-1.owner", &st) == 0);
		assert((stat("saves/Journal (backup)", &st) != 0) == absent);
	}
	assert(remove("saves/.world-copy-1.owner") == 0);
	char reserved[256];
	assert(screen_sworld_test_reserve_beta("saves", "Journal (backup)",
		reserved, sizeof(reserved)));
	assert(!strcmp(reserved, "saves/Journal (backup)"));
	assert(cw_rmdir(reserved) == 0);
	assert(cw_rmdir("saves/Journal") == 0);
}

static void case_beta_field_order(const struct cw_roles* r) {
	play_session_clear_draft();
	cw_open();
	cw_expect("name", "Beta initGui focuses the name field");
	static const char* const order[] = {"seed", "survival", "create", "name"};
	for(size_t k = 0; k < 4; k++) {
		cw_tap(r->down);
		cw_expect(order[k], "DOWN order is not Name, Seed, Game mode, Create");
	}
	static const char* const back[] = {"create", "survival", "seed", "name"};
	for(size_t k = 0; k < 4; k++) {
		cw_tap(r->up);
		cw_expect(back[k], "UP order is not the reverse");
	}
	/* Seed row: RIGHT = New seed, LEFT = the field. Mode row: LEFT/RIGHT pick
	 * the mode and the highlight follows it. */
	cw_tap(r->down);
	cw_tap(r->right);
	cw_expect("new_seed", "RIGHT on the seed field");
	cw_tap(r->left);
	cw_expect("seed", "LEFT on New seed");
	cw_tap(r->down);
	cw_tap(r->right);
	cw_expect("creative", "RIGHT on the mode row");
	assert(screen_cworld_test_snapshot(NULL).mode == CWORLD_MODE_CREATIVE);
	cw_tap(r->left);
	cw_expect("survival", "LEFT on the mode row");
	assert(screen_cworld_test_snapshot(NULL).mode == CWORLD_MODE_SURVIVAL);
	assert(fe_screen_sets == 0);
}

static void case_new_seed_visible_and_submitted(const struct cw_roles* r) {
	play_session_clear_draft();
	cw_open();
	cw_tap(r->down);
	cw_tap(r->right);
	cw_expect("new_seed", "route to New seed");
	cw_tap(r->click);
	struct cworld_form f = screen_cworld_test_snapshot(NULL);
	assert(f.seed[0] && "New seed left the field blank");
	assert(!f.editing && "New seed opened the editor");
	char* end = NULL;
	long long shown = strtoll(f.seed, &end, 10);
	assert(end && *end == '\0' && shown != 0
		   && "New seed is not a plain non-zero number");
	char first[CWORLD_SEED_CAP];
	snprintf(first, sizeof(first), "%s", f.seed);
	cw_tap(r->click);
	assert(strcmp(screen_cworld_test_snapshot(NULL).seed, first) != 0
		   && "a second New seed press repeated the number");
	shown = strtoll(screen_cworld_test_snapshot(NULL).seed, NULL, 10);
	bool set;
	int64_t seed;
	cw_create_submits(r, &set, &seed);
	assert(set && seed == (int64_t)shown
		   && "Create did not use the number New seed showed");
}

static void case_seed_semantics(const struct cw_roles* r) {
	static const struct {
		const char* text;
		bool set;
		int64_t seed;
	} cases[] = {
		{"", false, 0},		 /* empty = Random().nextLong() */
		{"0", false, 0},	 /* parseLong 0 keeps random (`!= 0L`) */
		{"-123", true, -123}, /* numeric = parseLong */
		{"9223372036854775807", true, INT64_MAX},
		{"abc", true, 96354},	   /* "abc".hashCode() */
		{"Glacier", true, 1772835215}, /* "Glacier".hashCode() */
		{"9223372036854775808", true,
		 -1773151197}, /* overflow -> hashCode, negative, sign-extended */
		{"+5", true, 1386},				 /* Java 6 rejects '+' -> hashCode */
	};
	for(size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
		cw_open_with("New World", cases[k].text);
		bool set = !cases[k].set;
		int64_t seed = 0;
		cw_create_submits(r, &set, &seed);
		if(set != cases[k].set || (set && seed != cases[k].seed)) {
			fprintf(stderr, "seed \"%s\": set=%d seed=%lld\n", cases[k].text,
					set, (long long)seed);
			assert(!"Create submitted a seed Beta would not");
		}
	}
}

static void case_details_reach_and_fit(const struct cw_roles* r) {
	/* A long name and a long text seed: the worst case for the card. */
	cw_open_with("A Very Long World Name For Wrap", "the quick brown fox jumps");
	for(int k = 0; k < 8 && strcmp(cw_focus(), "create"); k++)
		cw_tap(r->down);
	cw_tap(r->right);
	cw_expect("details", "RIGHT from Create world must reach Details");
	cw_tap(r->right);
	cw_expect("cancel", "RIGHT from Details");
	cw_tap(r->right);
	cw_expect("cancel", "the action row does not wrap");
	cw_tap(r->left);
	cw_expect("details", "LEFT from Cancel must reach Details");
	cw_tap(r->click);
	bool open = false;
	(void)screen_cworld_test_snapshot(&open);
	assert(open && fe_screen_sets == 0 && "Details did not open in place");
	cw_expect("details_back", "the card owns the focus");
	fe_render(&screen_create_world);

	static const int sizes[][2] = {{640, 480}, {854, 480}, {1280, 960}};
	for(size_t s = 0; s < 3; s++) {
		char lines[16][128];
		bool fits = false;
		int n = screen_cworld_test_details_lines(sizes[s][0], sizes[s][1],
												 lines, 16, &fits);
		bool seen[4] = {false, false, false, false};
		for(int k = 0; k < n && k < 16; k++)
			seen[lines[k][0] - '0'] = true;
		if(!fits || !(seen[0] && seen[1] && seen[2] && seen[3])) {
			for(int k = 0; k < n && k < 16; k++)
				fprintf(stderr, "  %s\n", lines[k]);
			assert(!"the Details card does not show all four lines whole");
		}
		if(s == 0)
			for(int k = 0; k < n; k++)
				printf("  details[%d] %s\n", k, lines[k]);
	}

	/* No action reaches the form under the card; A on Back closes it. */
	unsigned before = screen_cworld_test_last_submit(NULL, NULL);
	cw_tap(r->page);
	cw_tap(r->down);
	assert(screen_cworld_test_last_submit(NULL, NULL) == before);
	cw_tap(r->click);
	(void)screen_cworld_test_snapshot(&open);
	assert(!open && fe_screen_sets == 0);
	cw_expect("details", "closing Details returns to Details");
	/* The Back role also closes it without leaving the screen. */
	cw_tap(r->click);
	cw_tap(r->close);
	(void)screen_cworld_test_snapshot(&open);
	assert(!open && fe_screen_sets == 0 && "Back inside Details left the screen");
}

static void case_cancel_back_plus(const struct cw_roles* r) {
	/* Cancel = Beta's id 1: back to the parent (Load), draft kept. */
	cw_open_with("Kept", "42");
	for(int k = 0; k < 8 && strcmp(cw_focus(), "create"); k++)
		cw_tap(r->down);
	cw_tap(r->right);
	cw_tap(r->right);
	cw_expect("cancel", "route to Cancel");
	unsigned before = screen_cworld_test_last_submit(NULL, NULL);
	cw_tap(r->click);
	assert(fe_screen_last == &screen_select_world && fe_screen_sets == 1
		   && play_session_tab() == PLAY_TAB_LOAD && "Cancel did not return to Load");
	assert(screen_cworld_test_last_submit(NULL, NULL) == before);
	struct cworld_form kept;
	assert(play_session_restore_draft(&kept) && !strcmp(kept.name, "Kept")
		   && !strcmp(kept.seed, "42"));

	/* The Back role from the name field does the same. */
	cw_open_with("Kept", "42");
	cw_tap(r->close);
	assert(fe_screen_last == &screen_select_world && fe_screen_sets == 1);

	/* "+" creates from the name field, with the typed seed. */
	cw_open_with("Plus", "77");
	cw_expect("name", "resume focus");
	bool set = false;
	int64_t seed = 0;
	cw_tap(r->page);
	assert(screen_cworld_test_last_submit(&set, &seed) == before + 1 && set
		   && seed == 77 && "+ did not create from the form");
	assert(!screen_cworld_test_snapshot(NULL).editing);

	/* Blank name: Create disabled (Beta l.107); + does nothing, DOWN skips to
	 * Details, LEFT cannot reach the disabled button. */
	cw_open_with("", "");
	unsigned b2 = screen_cworld_test_last_submit(NULL, NULL);
	cw_tap(r->page);
	for(int k = 0; k < 3; k++)
		cw_tap(r->down);
	cw_expect("details", "a disabled Create must be skipped");
	cw_tap(r->left);
	cw_expect("details", "LEFT reached a disabled Create");
	cw_tap(r->click);
	cw_tap(r->click);
	assert(screen_cworld_test_last_submit(NULL, NULL) == b2);
}

/* The approved B-create rects (make_spec.py), aimed at their centres. */
static void case_pointer_rects(const struct cw_roles* r) {
	if(fe_family != FE_REMOTE)
		return;
	static const struct {
		int x, y, w, h;
		const char* name;
	} rects[] = {
		{104, 82, 432, 44, "name"},		  {104, 178, 300, 44, "seed"},
		{412, 178, 124, 44, "new_seed"},  {104, 276, 212, 44, "survival"},
		{324, 276, 212, 44, "creative"},  {100, 366, 212, 48, "create"},
		{320, 366, 104, 48, "details"},	  {432, 366, 108, 48, "cancel"},
	};
	play_session_clear_draft();
	cw_open();
	for(size_t k = 0; k < sizeof(rects) / sizeof(rects[0]); k++) {
		fe_ir_armed = true;
		fe_ir_x = 2.0F;
		fe_ir_y = 2.0F;
		cw_step();
		fe_ir_x = (float)rects[k].x + rects[k].w / 2.0F;
		fe_ir_y = (float)rects[k].y + rects[k].h / 2.0F;
		cw_step();
		cw_expect(rects[k].name, "pointer on an approved rect");
	}
	/* Pointer + A on Creative picks it. */
	fe_ir_x = 324 + 106;
	fe_ir_y = 276 + 22;
	cw_step();
	cw_tap(r->click);
	assert(screen_cworld_test_snapshot(NULL).mode == CWORLD_MODE_CREATIVE);
	fe_ir_armed = false;
	cw_step();
}

static void run_family(enum fe_family family) {
	fe_attach(family);
	struct cw_roles r = cw_roles_for(family);
	case_beta_field_order(&r);
	case_new_seed_visible_and_submitted(&r);
	case_seed_semantics(&r);
	case_details_reach_and_fit(&r);
	case_cancel_back_plus(&r);
	case_pointer_rects(&r);
	case_journal_names_taken(&r);
	printf("PASS create world UI (%s)\n", fe_family_name[family]);
}

int main(int argc, char** argv) {
	/* The screen stat()s the saves dir and the rig may write config.json:
	 * run inside a scratch dir under the BUILD tree (argv[1]). */
	FILE* in = fopen("config_wii.json", "rb");
	assert(in && "config_wii.json missing; is the CWD the source root?");
	if(argc > 1)
		assert(cw_chdir(argv[1]) == 0);
	remove(CW_RUN_DIR "/config_wii.json");
	remove(CW_RUN_DIR "/config.json");
	cw_rmdir(CW_RUN_DIR "/saves");
	assert(cw_mkdir(CW_RUN_DIR) == 0 || errno == EEXIST);
	FILE* out = fopen(CW_RUN_DIR "/config_wii.json", "wb");
	assert(out);
	char buf[4096];
	size_t n;
	while((n = fread(buf, 1, sizeof(buf), in)) > 0)
		assert(fwrite(buf, 1, n, out) == n);
	fclose(in);
	fclose(out);
	assert(cw_chdir(CW_RUN_DIR) == 0);
	assert(config_create(&gstate.config_user, "config_wii.json"));
	assert(config_set_string(&gstate.config_user, "paths.worlds", "saves"));
	assert(config_set_string(&gstate.config_user, "mp.username", ""));

	for(int family = 0; family < FE_FAMILY_COUNT; family++)
		run_family((enum fe_family)family);

	config_destroy(&gstate.config_user);
	remove("config_wii.json");
	remove("config.json");
	assert(cw_chdir("..") == 0);
	cw_rmdir(CW_RUN_DIR);
	printf("PASS test_create_world_ui\n");
	return 0;
}
