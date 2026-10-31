/* ui-build-1026 T05a: the Create World on-screen keyboard (design-round-1026c
 * opus-B name-osk / seed-osk), driven through the REAL screen_create_world.c
 * and widget_osk_frontend.c on the PLATFORM_WII input rig, all three
 * controller families (pointer cases on the Wii Remote only).
 *
 * Proven through real presses / pointer frames and the screen's own draft:
 *   1. Geometry: every Create key is large (name grid >= 54x44, keypad
 *      180x44), inside the modal panel, under the field/hint, no overlaps,
 *      at 640x480, 854x480 and 1280x960. The DEFAULT profile (Rename World,
 *      Multiplayer) gains no key and keeps its numbers page.
 *   2. Name: the digit row, letters and Shift type by d-pad; Delete deletes;
 *      Cancel (the Back role, and the pointer Cancel key) restores the old
 *      name; Done keeps the new one.
 *   3. Seed keypad: digits and '-' by d-pad; Random puts a visible non-zero
 *      number in the DRAFT; Cancel restores the old seed; Done + Create
 *      submits exactly the shown number.
 *   4. Text seed: "abc" opens letters, typed text is submitted as Beta's
 *      String.hashCode [740c583 GuiCreateWorld.actionPerformed l.75-86].
 *   5. Pointer: aiming at a key and pressing A types it (Wii Remote). */

#include "support/frontend_screen_rig.h"

struct tex_gfx texture_menu_background;

#include "game/gui/cworld_form_model.h"
#include "game/gui/play_session.h"
#include "game/gui/widget_osk.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern struct cworld_form screen_cworld_test_snapshot(bool* details_is_open);
extern const char* screen_cworld_test_focus_name(void);
extern unsigned screen_cworld_test_last_submit(bool* seed_set, int64_t* seed);
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

#define CW_RUN_DIR "tmp_create_world_osk_run"
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
	int close, click, up, down, left, right;
};
static struct cw_roles cw_roles_for(enum fe_family fam) {
	struct cw_roles r = {
		cw_code_for(fam, "inventory"), cw_code_for(fam, "gui_click"),
		cw_code_for(fam, "gui_up"),	   cw_code_for(fam, "gui_down"),
		cw_code_for(fam, "gui_left"),  cw_code_for(fam, "gui_right"),
	};
	assert(r.close > 0 && r.click >= 0 && r.up >= 0 && r.down > 0
		   && r.left > 0 && r.right > 0 && "a menu role is unbound");
	return r;
}

/* ---- driving ------------------------------------------------------------ */
/* One press = a held frame then a released frame: the editor detects edges
 * against its own previous frame, so two taps of one button need the gap. */
static void cw_tap(int code) {
	fe_hold(code);
	screen_create_world.update(&screen_create_world, CW_DT);
	fe_idle();
	screen_create_world.update(&screen_create_world, CW_DT);
}
static void cw_step(void) {
	fe_idle();
	screen_create_world.update(&screen_create_world, CW_DT);
}
static void cw_taps(int code, int n) {
	for(int k = 0; k < n; k++)
		cw_tap(code);
}
static struct cworld_form cw_form(void) {
	return screen_cworld_test_snapshot(NULL);
}
static void cw_open_with(const char* name, const char* seed) {
	struct cworld_form f;
	cworld_form_reset(&f);
	snprintf(f.name, sizeof(f.name), "%s", name);
	snprintf(f.seed, sizeof(f.seed), "%s", seed);
	play_session_store_draft(&f);
	screen_set(&screen_create_world);
	fe_idle();
	fe_screen_last = NULL;
	fe_screen_sets = 0;
	assert(!strcmp(cw_form().name, name) && !strcmp(cw_form().seed, seed));
}
/* A on the focused text row opens the modal; one idle frame lets the editor
 * see a released pad before the first real press. */
static void cw_edit_row(const struct cw_roles* r, const char* row) {
	for(int k = 0; k < 6 && strcmp(screen_cworld_test_focus_name(), row); k++)
		cw_tap(r->down);
	assert(!strcmp(screen_cworld_test_focus_name(), row));
	cw_tap(r->click);
	assert(cw_form().editing && "A did not open the editor");
	cw_step();
}
static const char* cw_draft(int row) {
	static struct cworld_form f;
	f = cw_form();
	return row == CWORLD_ROW_SEED ? f.seed : f.name;
}
static void cw_expect_draft(int row, const char* want, const char* why) {
	if(strcmp(cw_draft(row), want)) {
		fprintf(stderr, "[%s] draft \"%s\", want \"%s\": %s\n",
				fe_family_name[fe_family], cw_draft(row), want, why);
		assert(!"draft mismatch");
	}
}
static void cw_submit(const struct cw_roles* r, bool* set, int64_t* seed) {
	assert(!cw_form().editing);
	for(int k = 0; k < 8 && strcmp(screen_cworld_test_focus_name(), "create"); k++)
		cw_tap(r->down);
	unsigned before = screen_cworld_test_last_submit(NULL, NULL);
	cw_tap(r->click);
	assert(screen_cworld_test_last_submit(set, seed) == before + 1);
}

/* ---- 1. geometry --------------------------------------------------------- */
static void geo_check(int profile, int layout, int width, int height) {
	static char buf[CWORLD_SEED_CAP];
	buf[0] = 0;
	struct osk_frontend o;
	osk_frontend_reset(&o, buf, sizeof(buf), 0, 0, false);
	osk_frontend_set_profile(&o, profile);
	o.layout = layout;
	float s = (float)height / 480.0F;
	int ox = (width - (int)(640 * s + .5F)) / 2;
	struct gutil_button_bounds b[OSK_FRONT_TARGET_COUNT];
	bool has[OSK_FRONT_TARGET_COUNT];
	int keys = 0;
	for(int t = 0; t < OSK_FRONT_TARGET_COUNT; t++) {
		has[t] = osk_frontend_key_bounds(&o, width, height, t, &b[t]);
		assert(has[t] == osk_frontend_enabled(&o, t, true)
			   && "a Create key is drawn but dead, or live but undrawn");
		if(!has[t])
			continue;
		keys++;
		/* inside the panel (20..620 x 16..464), below the seed hint (150) */
		assert(b[t].x >= ox + (int)(20 * s) && b[t].x + b[t].width <= ox + (int)(620 * s + 1)
			   && b[t].y >= (int)(150 * s) && b[t].y + b[t].height <= (int)(464 * s + 1));
		assert(b[t].height >= (int)(44 * s));
		bool ch = (t >= OSK_FRONT_CHAR_FIRST && t < OSK_FRONT_CASE)
			|| t >= OSK_FRONT_DIGIT_FIRST;
		if(ch)
			assert(b[t].width >= (int)((profile == OSK_FRONT_PROFILE_CREATE_SEED
										 && layout == 1 ? 180 : 54) * s));
		for(int u = 0; u < t; u++)
			if(has[u])
				assert((b[u].x + b[u].width <= b[t].x || b[t].x + b[t].width <= b[u].x
						|| b[u].y + b[u].height <= b[t].y
						|| b[t].y + b[t].height <= b[u].y)
					   && "two Create keys overlap");
	}
	assert(has[OSK_FRONT_CANCEL] && has[OSK_FRONT_DONE] && has[OSK_FRONT_BACK]);
	if(profile == OSK_FRONT_PROFILE_CREATE_SEED && layout == 1) {
		assert(keys == 11 + 5 && has[OSK_FRONT_RANDOM] && has[OSK_FRONT_LETTERS]);
		const char* want = "123456789-0";
		for(int i = 0; i < 11; i++)
			assert(osk_frontend_char(&o, i) == want[i]);
		assert(!osk_frontend_char(&o, 11) && "keypad must not offer '+'");
	} else {
		assert(!has[OSK_FRONT_RANDOM]);
		for(int d = 0; d < 10; d++)
			assert(has[OSK_FRONT_DIGIT_FIRST + d]
				   && osk_frontend_char(&o, 30 + d) == "1234567890"[d]);
	}
}
static void case_geometry(void) {
	static const int sizes[][2] = {{640, 480}, {854, 480}, {1280, 960}};
	for(int k = 0; k < 3; k++) {
		geo_check(OSK_FRONT_PROFILE_CREATE_NAME, 0, sizes[k][0], sizes[k][1]);
		geo_check(OSK_FRONT_PROFILE_CREATE_NAME, 2, sizes[k][0], sizes[k][1]);
		geo_check(OSK_FRONT_PROFILE_CREATE_SEED, 1, sizes[k][0], sizes[k][1]);
		geo_check(OSK_FRONT_PROFILE_CREATE_SEED, 0, sizes[k][0], sizes[k][1]);
	}
	/* DEFAULT (Rename World, Multiplayer): no Create-only key, numbers page
	 * keeps "+", the caret keys still exist. */
	char buf[8] = "";
	struct osk_frontend o;
	osk_frontend_reset(&o, buf, sizeof(buf), 0, 1, false);
	assert(o.profile == OSK_FRONT_PROFILE_DEFAULT);
	struct gutil_button_bounds b;
	for(int t = OSK_FRONT_RANDOM; t < OSK_FRONT_TARGET_COUNT; t++)
		assert(!osk_frontend_key_bounds(&o, 640, 480, t, &b)
			   && !osk_frontend_enabled(&o, t, true));
	assert(osk_frontend_char(&o, 11) == '+' && !osk_frontend_char(&o, 30));
	assert(osk_frontend_key_bounds(&o, 640, 480, OSK_FRONT_LEFT, &b));
	assert(osk_frontend_key_bounds(&o, 640, 480, OSK_FRONT_PREV, &b));
	printf("  geometry ok\n");
}

/* ---- 2. name by pad ------------------------------------------------------ */
static void case_name_pad(const struct cw_roles* r) {
	cw_open_with("Cave", "");
	cw_edit_row(r, "name");
	/* opens on 'q'; UP = the digit row's '1' */
	cw_tap(r->up);
	cw_tap(r->click);
	cw_expect_draft(CWORLD_ROW_NAME, "Cave1", "UP from q, A");
	cw_tap(r->right);
	cw_tap(r->click);
	cw_tap(r->down); /* '2' -> 'w' */
	cw_tap(r->click);
	cw_expect_draft(CWORLD_ROW_NAME, "Cave12w", "digit row then letters");
	/* Shift: from 'w' DOWN x2 = 'x', LEFT = 'z', DOWN = Shift */
	cw_taps(r->down, 2);
	cw_tap(r->click); /* 'x' */
	cw_tap(r->left);  /* 'z' */
	cw_tap(r->down);  /* Shift sits under z */
	cw_tap(r->click);
	cw_tap(r->up);    /* back to 'z' */
	cw_tap(r->click);
	cw_expect_draft(CWORLD_ROW_NAME, "Cave12wxZ", "Shift then z");
	/* Delete key: bottom row RIGHT x3 from Shift = Delete (Shift, !#?, Space) */
	cw_tap(r->down);
	cw_taps(r->right, 3);
	cw_tap(r->click);
	cw_expect_draft(CWORLD_ROW_NAME, "Cave12wx", "Delete key");
	/* the Back role cancels: the old name comes back */
	cw_tap(r->close);
	assert(!cw_form().editing);
	cw_expect_draft(CWORLD_ROW_NAME, "Cave", "Cancel must keep the old name");
	assert(fe_screen_sets == 0);
	/* Done keeps the new name: type '1', then RIGHT x4 from Delete = Done */
	cw_edit_row(r, "name");
	cw_tap(r->up);
	cw_tap(r->click);
	cw_taps(r->down, 4);
	cw_taps(r->right, 8);
	cw_tap(r->click);
	assert(!cw_form().editing && "Done did not close the editor");
	cw_expect_draft(CWORLD_ROW_NAME, "Cave1", "Done keeps the typed name");
}

/* ---- 3. seed keypad ------------------------------------------------------ */
static void case_seed_keypad(const struct cw_roles* r) {
	cw_open_with("Seeds", "42");
	cw_edit_row(r, "seed");
	cw_expect_draft(CWORLD_ROW_SEED, "42", "the draft starts from the field");
	cw_tap(r->click); /* '1' */
	cw_tap(r->down);
	cw_tap(r->click); /* '4' */
	cw_taps(r->down, 2);
	cw_tap(r->click); /* '-' */
	cw_tap(r->right);
	cw_tap(r->click); /* '0' */
	cw_expect_draft(CWORLD_ROW_SEED, "4214-0", "keypad digits and minus");
	/* Random: bottom-left cell. The draft becomes a visible number. */
	cw_tap(r->left);
	cw_tap(r->down);
	cw_tap(r->click);
	char shown[CWORLD_SEED_CAP];
	snprintf(shown, sizeof(shown), "%s", cw_draft(CWORLD_ROW_SEED));
	char* end = NULL;
	long long v = strtoll(shown, &end, 10);
	assert(cw_form().editing && end && !*end && v != 0
		   && "Random must put a plain non-zero number in the draft");
	/* Cancel restores the OLD seed, not the random one. */
	cw_tap(r->close);
	cw_expect_draft(CWORLD_ROW_SEED, "42", "Cancel must keep the old seed");

	/* Random + Done + Create submits exactly the shown number. */
	cw_edit_row(r, "seed");
	cw_taps(r->down, 4);
	cw_tap(r->click);
	snprintf(shown, sizeof(shown), "%s", cw_draft(CWORLD_ROW_SEED));
	v = strtoll(shown, NULL, 10);
	cw_tap(r->click); /* a second Random gives a new number */
	assert(strcmp(cw_draft(CWORLD_ROW_SEED), shown));
	snprintf(shown, sizeof(shown), "%s", cw_draft(CWORLD_ROW_SEED));
	v = strtoll(shown, NULL, 10);
	cw_taps(r->right, 2); /* Random -> Delete -> Done */
	cw_tap(r->click);
	assert(!cw_form().editing);
	cw_expect_draft(CWORLD_ROW_SEED, shown, "Done keeps the Random seed");
	bool set = false;
	int64_t seed = 0;
	cw_submit(r, &set, &seed);
	assert(set && seed == (int64_t)v && "Create did not use the shown seed");
}

/* ---- 4. text seed -------------------------------------------------------- */
static void case_text_seed(const struct cw_roles* r) {
	cw_open_with("Words", "");
	cw_edit_row(r, "seed");
	cw_taps(r->down, 3);
	cw_taps(r->right, 2);
	cw_tap(r->click); /* abc -> letters; focus stays on the page key (123) */
	cw_taps(r->up, 2); /* -> 'c' -> 'd' */
	cw_taps(r->left, 2);
	cw_tap(r->click); /* a */
	cw_tap(r->down);
	cw_taps(r->right, 4);
	cw_tap(r->click); /* b */
	cw_taps(r->left, 2);
	cw_tap(r->click); /* c */
	cw_expect_draft(CWORLD_ROW_SEED, "abc", "letters from the seed keypad");
	/* Done: bottom row from 'c' is the 123 key; Space, Delete, Done */
	cw_tap(r->down);
	cw_taps(r->right, 3);
	cw_tap(r->click);
	assert(!cw_form().editing);
	bool set = false;
	int64_t seed = 0;
	cw_submit(r, &set, &seed);
	assert(set && seed == 96354 && "\"abc\" must seed as String.hashCode");
}

/* ---- 5. pointer ---------------------------------------------------------- */
static void cw_point_at(int profile, int layout, int target) {
	char buf[4] = "";
	struct osk_frontend o;
	osk_frontend_reset(&o, buf, sizeof(buf), 0, 0, false);
	osk_frontend_set_profile(&o, profile);
	o.layout = layout;
	struct gutil_button_bounds b;
	assert(osk_frontend_key_bounds(&o, 640, 480, target, &b));
	fe_ir_x = (float)b.x + b.width / 2.0F;
	fe_ir_y = (float)b.y + b.height / 2.0F;
	cw_step();
}
static void case_pointer(const struct cw_roles* r) {
	if(fe_family != FE_REMOTE)
		return;
	cw_open_with("Ptr", "5");
	cw_edit_row(r, "name");
	fe_ir_armed = true;
	fe_ir_x = 2.0F;
	fe_ir_y = 2.0F;
	cw_step();
	cw_point_at(OSK_FRONT_PROFILE_CREATE_NAME, 0, OSK_FRONT_DIGIT_FIRST + 6);
	cw_tap(r->click);
	cw_point_at(OSK_FRONT_PROFILE_CREATE_NAME, 0, OSK_FRONT_CHAR_FIRST + 10);
	cw_tap(r->click);
	cw_expect_draft(CWORLD_ROW_NAME, "Ptr7a", "pointer + A types the aimed key");
	cw_point_at(OSK_FRONT_PROFILE_CREATE_NAME, 0, OSK_FRONT_CANCEL);
	for(int k = 0; k < 2 && cw_form().editing; k++)
		cw_tap(r->click);
	assert(!cw_form().editing && "pointer Cancel did not close");
	cw_expect_draft(CWORLD_ROW_NAME, "Ptr", "pointer Cancel keeps the old name");
	/* keypad Random by pointer */
	fe_ir_armed = false;
	cw_step();
	cw_edit_row(r, "seed");
	fe_ir_armed = true;
	fe_ir_x = 2.0F;
	fe_ir_y = 2.0F;
	cw_step();
	cw_point_at(OSK_FRONT_PROFILE_CREATE_SEED, 1, OSK_FRONT_RANDOM);
	cw_tap(r->click);
	char* end = NULL;
	long long v = strtoll(cw_draft(CWORLD_ROW_SEED), &end, 10);
	assert(end && !*end && v != 0 && strcmp(cw_draft(CWORLD_ROW_SEED), "5"));
	fe_render(&screen_create_world);
	fe_ir_armed = false;
	cw_tap(r->close);
	cw_expect_draft(CWORLD_ROW_SEED, "5", "Cancel after pointer Random");
}

static void run_family(enum fe_family family) {
	fe_attach(family);
	struct cw_roles r = cw_roles_for(family);
	case_name_pad(&r);
	case_seed_keypad(&r);
	case_text_seed(&r);
	case_pointer(&r);
	printf("PASS create world OSK (%s)\n", fe_family_name[family]);
}

int main(int argc, char** argv) {
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

	case_geometry();
	for(int family = 0; family < FE_FAMILY_COUNT; family++)
		run_family((enum fe_family)family);

	config_destroy(&gstate.config_user);
	remove("config_wii.json");
	remove("config.json");
	assert(cw_chdir("..") == 0);
	cw_rmdir(CW_RUN_DIR);
	printf("PASS test_create_world_osk\n");
	return 0;
}
