/* Real Create lifecycle, through title/Load/Create, actual PLATFORM_WII
 * owner input and shipped/remapped bindings on all three controller families.
 * The frontend keyboard mirror is only a physical-button driver. Text, mode,
 * persistence, transitions and create attempts are observed from the real
 * screen and retained Play session. Initial missing retention/tab behavior
 * was demonstrated in RED commit a590b290. */

#include "support/frontend_screen_rig.h"

/* Optional drawing-only backdrop, absent in this headless input rig. */
struct tex_gfx texture_menu_background;

#include "game/gui/cworld_form_model.h"
#include "game/gui/play_session.h"
#include "game/gui/widget_osk.h"
#include "graphics/gfx_settings.h"
#include "platform/input_glyph.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern struct cworld_form screen_cworld_test_snapshot(bool* details_is_open);
#ifdef _WIN32
#include <direct.h>
#define fe_mkdir(p) _mkdir(p)
#define fe_rmdir(p) _rmdir(p)
#define fe_chdir(p) _chdir(p)
#else
#include <unistd.h>
#define fe_mkdir(p) mkdir(p, 0777)
#define fe_rmdir(p) rmdir(p)
#define fe_chdir(p) chdir(p)
#endif

/* ---- binding lookup: the config is the source of truth ------------------
 * Copied (not re-derived) from tests/test_frontend_no_pointer.c's fe_code_for /
 * fe_roles_for. Only these two helpers and the small driving set below are
 * taken over; the 5000-line suite itself stays where it is. The ranges are
 * input_owner_accepts' own (input_owner.h). */
static int fe_code_for(enum fe_family fam, const char* key) {
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

struct fe_roles {
	int close, home, click, click_alt, up, down, left, right, page, previous;
};

static struct fe_roles fe_roles_for(enum fe_family fam) {
	struct fe_roles r;
	r.close = fe_code_for(fam, "inventory");
	r.home = fe_code_for(fam, "open_menu");
	r.click = fe_code_for(fam, "gui_click");
	r.click_alt = fe_code_for(fam, "gui_click_alt");
	r.up = fe_code_for(fam, "gui_up");
	r.down = fe_code_for(fam, "gui_down");
	r.left = fe_code_for(fam, "gui_left");
	r.right = fe_code_for(fam, "gui_right");
	r.page = fe_code_for(fam, "gui_page");
	r.previous = fe_code_for(fam, "gui_previous");
	assert(r.click_alt >= 0 && "gui_click_alt (the OSK backspace) is unbound");
	assert(r.close > 0 && r.home > 0 && r.click > 0 && r.up >= 0 && r.down > 0
		   && r.left > 0 && r.right > 0 && r.page > 0 && r.previous > 0
		   && "a menu role is unbound for this family");
	return r;
}

/* ---- configuration fixtures --------------------------------------------- */

#define PF_SAVES_DIR "saves"
#define PF_REMAP_CONFIG "test_play_flow_remap.json"
#define PF_RUN_DIR "tmp_frontend_play_flow_run"

/* The same REMAPPED table the A1 suite uses: it separates the page roles from
 * the hotbar roles on ALL THREE families, so a role read that is right for one
 * family's codes and wrong for another's is caught here too. */
static const char pf_remap_json[]
	= "{\"input\":{"
	  "\"inventory\":[5,205,406],"
	  "\"open_menu\":[10,214,411],"
	  "\"gui_click\":[4,204,404],"
	  "\"gui_click_alt\":[6,206,407],"
	  "\"gui_up\":[0,200,400],\"gui_down\":[1,201,401],"
	  "\"gui_left\":[2,202,402],\"gui_right\":[3,203,403],"
	  "\"gui_page\":[7,209,410],\"gui_previous\":[100,208,409],"
	  "\"scroll_left\":[9,210,405],\"scroll_right\":[8,211,406],"
	  "\"debug_overlay\":[],\"screenshot\":[]"
	  "}}";

static bool pf_config_live;

static void pf_config_open(bool remapped) {
	if(pf_config_live)
		config_destroy(&gstate.config_user);
	pf_config_live = true;
	if(remapped) {
		FILE* f = fopen(PF_REMAP_CONFIG, "w");
		assert(f);
		fputs(pf_remap_json, f);
		fclose(f);
		assert(config_create(&gstate.config_user, PF_REMAP_CONFIG));
	} else {
		assert(config_create(&gstate.config_user, "config_wii.json"));
	}
	assert(
		config_set_string(&gstate.config_user, "paths.worlds", PF_SAVES_DIR));
	assert(config_set_string(&gstate.config_user, "mp.username", ""));
}

/* `count` world directories, rebuilt from scratch. No level.dat: the rig's
 * level_archive double resolves every row to a readable survival world. */
static void pf_saves(size_t count) {
	for(size_t k = 0; k < 32; k++) {
		char path[128];
		snprintf(path, sizeof(path), PF_SAVES_DIR "/world%02zu", k);
		fe_rmdir(path);
	}
	fe_rmdir(PF_SAVES_DIR);
	if(count == 0)
		return;
	assert(fe_mkdir(PF_SAVES_DIR) == 0 || errno == EEXIST);
	for(size_t k = 0; k < count; k++) {
		char path[128];
		snprintf(path, sizeof(path), PF_SAVES_DIR "/world%02zu", k);
		assert(fe_mkdir(path) == 0 || errno == EEXIST);
	}
}

/* ---- screen driving ----------------------------------------------------- */

#define PF_DT (1.0F / 60.0F)

static struct screen* pf_current;

/* ONE action scope = one place the transition counter is cleared. The first
 * draft cleared it twice per open and never after a hand-off, so a Title ->
 * Load -> Create route arrived at Create already showing 2 and every later
 * "== 0" / "== 1" assertion failed for a HARNESS reason instead of a product
 * one. Now: pf_open and pf_follow each end the scope exactly once, and nothing
 * else touches the counter. */
static void pf_scope_reset(void) {
	fe_screen_last = NULL;
	fe_screen_sets = 0;
}

static void pf_open(struct screen* s) {
	screen_set(s);
	pf_current = s;
	/* One neutral hardware poll before the first press -- the frame boundary
	 * production puts between screen_set and the arriving screen's first
	 * update, and the thing that clears input.c's edge SWALLOW (a key the OSK
	 * consumed must be released before it can drive an IB action again). */
	fe_idle();
	pf_scope_reset();
}

/* Follow the hand-off the last update asked for, then start a fresh scope. The
 * caller asserts on fe_screen_last / fe_screen_sets BEFORE calling this. */
static void pf_follow(void) {
	assert(fe_screen_last && "pf_follow with no hand-off to follow");
	pf_current = fe_screen_last;
	fe_idle();
	pf_scope_reset();
}

static void pf_step(void) {
	fe_idle();
	pf_current->update(pf_current, PF_DT);
}

/* One TAP: the down frame runs update (that is the frame the press edge exists
 * on), then a neutral frame re-arms the latch. The screen is NOT updated on
 * the neutral frame, because the down frame may have handed off. */
static void pf_tap(int code) {
	fe_hold(code);
	pf_current->update(pf_current, PF_DT);
	fe_idle();
}

/* TWO physical buttons on the SAME poll: navigation and activation arriving
 * together (Astra ruling 2 item 2's same-frame ordering). */
static void pf_tap2(int a, int b) {
	int codes[2] = {a, b};
	fe_poll_with(codes, 2);
	pf_current->update(pf_current, PF_DT);
	fe_idle();
}

/* A button HELD across several updates: the edge exists once, and only a
 * neutral frame re-arms it. */
static void pf_hold_frames(int code, int frames) {
	for(int k = 0; k < frames; k++) {
		fe_hold(code);
		pf_current->update(pf_current, PF_DT);
	}
}

static void pf_release(void) {
	fe_idle();
	fe_idle();
}

/* ---- the IR pointer (Wii Remote only) ------------------------------------ */

static void pf_pointer(float x, float y) {
	assert(fe_family == FE_REMOTE
		   && "only the Wii Remote has an IR pointer; use the d-pad route");
	fe_ir_armed = true;
	fe_ir_x = x;
	fe_ir_y = y;
}
static void pf_pointer_off(void) { fe_ir_armed = false; }

/* The composition's own mapping, identical on the title, the Load tab and (per
 * section 15) the Create tab: scale by height/480, centre horizontally. A case
 * points at the CENTRE of an approved rect, so a one-pixel rounding difference
 * between this helper and a screen's own rounding cannot decide a case. */
static float pf_cx(float canvas_x) {
	float s = (float)gfx_height() / 480.0F;
	return ((float)gfx_width() - 640.0F * s) / 2.0F + canvas_x * s;
}
static float pf_cy(float canvas_y) {
	return canvas_y * (float)gfx_height() / 480.0F;
}
static void pf_point_at(float cx, float cy, float cw, float ch) {
	pf_pointer(pf_cx(cx + cw / 2.0F), pf_cy(cy + ch / 2.0F));
}

/* THE APPROVED CREATE SCREEN (ui-build-1026 T05: design-round-1026c/opus/
 * opus-B-create, make_spec.py "B-create"). Beta's field order -- name, the
 * folder line, seed (+ New seed), then Game mode -- and Create world | Cancel,
 * with the separately approved Details between them. No tab strip: Cancel /
 * Back return to the Load tab, whose strip still reaches Join. */
#define PF_NAME_X 104
#define PF_NAME_Y 82
#define PF_NAME_W 432
#define PF_NAME_H 44
#define PF_SEED_X 104
#define PF_SEED_Y 178
#define PF_SEED_W 300
#define PF_SEED_H 44
#define PF_NEWSEED_X 412
#define PF_NEWSEED_W 124
#define PF_MODE_SURVIVAL_X 104
#define PF_MODE_CREATIVE_X 324
#define PF_MODE_Y 276
#define PF_MODE_W 212
#define PF_MODE_H 44
#define PF_CREATE_X 100
#define PF_CREATE_Y 366
#define PF_CREATE_W 212
#define PF_CREATE_H 48
#define PF_DETAILS_X 320
#define PF_DETAILS_Y 366
#define PF_DETAILS_W 104
#define PF_DETAILS_H 48
#define PF_CANCEL_X 432
#define PF_CANCEL_Y 366
#define PF_CANCEL_W 108
#define PF_CANCEL_H 48
/* The Details card's own Back control. */
#define PF_DETAILS_BACK_X 216
#define PF_DETAILS_BACK_Y 354
#define PF_DETAILS_BACK_W 208
#define PF_DETAILS_BACK_H 48

/* The mirror plans physical d-pad input only. Assertions observe the real
 * screen's committed state, never the mirror's text. */
static struct osk_frontend pf_nav;
static char pf_nav_buf[CWORLD_SEED_CAP];
static void pf_nav_neutral(void) {
    fe_idle();
    pf_current->update(pf_current, PF_DT);
    (void)osk_frontend_update_owned(&pf_nav,640,480,input_player_frame(0),
        false,false,0,0,true,true);
}
static void pf_nav_begin(int page,const char* text,size_t cap,int max) {
    assert(cap<=sizeof(pf_nav_buf));
    snprintf(pf_nav_buf,sizeof(pf_nav_buf),"%s",text?text:"");
    osk_frontend_reset(&pf_nav,pf_nav_buf,cap,max,
        page==OSK_PAGE_NUM?1:page==OSK_PAGE_SYMBOLS?2:0,false);
    /* T05a: the Create editor's keyboard profile (mirror the real screen). */
    osk_frontend_set_profile(&pf_nav,page==OSK_PAGE_NUM
        ?OSK_FRONT_PROFILE_CREATE_SEED:OSK_FRONT_PROFILE_CREATE_NAME);
    pf_nav_neutral();
}
static enum osk_event pf_nav_tap(int code) {
    assert(!fe_ir_armed);
    fe_hold(code); pf_current->update(pf_current,PF_DT);
    enum osk_event ev=osk_frontend_update_owned(&pf_nav,640,480,
        input_player_frame(0),false,false,0,0,true,true);
    if(ev==OSK_EV_NONE||ev==OSK_EV_CHANGED) pf_nav_neutral();
    else fe_idle();
    return ev;
}
/* Breadth-first search through the public real widget input API finds a pad
 * route; no cursor setter is used on the screen or on the live mirror. */
static void pf_nav_goto(const struct fe_roles* r,int target) {
    struct osk_frontend states[OSK_FRONT_TARGET_COUNT];
    int queue[OSK_FRONT_TARGET_COUNT],prev[OSK_FRONT_TARGET_COUNT];
    int via[OSK_FRONT_TARGET_COUNT],head=0,tail=0;
    const int roles[]={IB_GUI_UP,IB_GUI_DOWN,IB_GUI_LEFT,IB_GUI_RIGHT};
    const int codes[]={r->up,r->down,r->left,r->right};
    for(int i=0;i<OSK_FRONT_TARGET_COUNT;i++) prev[i]=-1;
    int start=pf_nav.target; prev[start]=start; states[start]=pf_nav;
    queue[tail++]=start;
    while(head<tail&&prev[target]<0) {
        int at=queue[head++];
        for(int d=0;d<4;d++) {
            struct osk_frontend next=states[at];
            struct input_player_frame sample=*input_player_frame(0);
            memset(sample.down,0,sizeof(sample.down));
            memset(sample.pressed,0,sizeof(sample.pressed));
            (void)osk_frontend_update_owned(&next,640,480,&sample,false,false,0,0,true,true);
            sample.down[roles[d]]=sample.pressed[roles[d]]=true;
            (void)osk_frontend_update_owned(&next,640,480,&sample,false,false,0,0,true,true);
            int to=next.target;
            if(prev[to]<0) {prev[to]=at;via[to]=d;states[to]=next;queue[tail++]=to;}
        }
    }
    assert(prev[target]>=0&&"frontend keyboard target unreachable by controller");
    int route[OSK_FRONT_TARGET_COUNT],n=0;
    for(int at=target;at!=start;at=prev[at])route[n++]=via[at];
    while(n)pf_nav_tap(codes[route[--n]]);
    assert(pf_nav.target==target);
}
static void pf_nav_type(const struct fe_roles* r,int page,char ch) {
    (void)page;
    int target=-1;
    for(int i=0;i<30;i++) if(osk_frontend_char(&pf_nav,i)==ch)
        target=OSK_FRONT_CHAR_FIRST+i;
    assert(target>=0&&"requested character absent from current keyboard page");
    pf_nav_goto(r,target);
    assert(pf_nav_tap(r->click)==OSK_EV_CHANGED);
}
static void pf_nav_done(const struct fe_roles* r) {
    pf_nav_goto(r,OSK_FRONT_DONE);
    assert(pf_nav_tap(r->click)==OSK_EV_DONE);
}

/* CLEAR the edited field with real presses. NOT by walking the function row:
 * that probe was the second diagnosed driver bug -- the row's Sym key switches
 * PAGE, so a probe that walked past it left the cursor on the symbols page and
 * the next character lookup asked for a row that page does not have
 * ("[diag] want row 3 col 5, mirror at 1,5 page 2"). widget_osk.h documents a
 * backspace ACCELERATOR that works "from anywhere on the widget" --
 * IB_GUI_CLICK_ALT -- so the driver presses that instead: one real bound
 * button, no page side effect, no cursor walk.
 *
 * The mirror's length is the DRIVER's loop condition only. What the SCREEN
 * ended up holding is read back from its own snapshot after the tab exit. */
static void pf_nav_clear_field(const struct fe_roles* r) {
	for(int guard = 0; guard < (int)sizeof(pf_nav_buf) && pf_nav_buf[0];
		guard++) {
		enum osk_event ev = pf_nav_tap(r->click_alt);
		assert(ev == OSK_EV_CHANGED
			   && "the OSK backspace accelerator did not backspace");
	}
	assert(pf_nav_buf[0] == '\0' && "the field would not clear");
}

/* ---- routes ------------------------------------------------------------- */

/* The REAL title -> Load tab entry: the Play row, activated. */
static void pf_enter_play(const struct fe_roles* r) {
	pf_open(&screen_title);
	/* The title opens with the highlight on Play game (zero DOWN steps). */
	pf_tap(r->click);
	assert(fe_screen_last == &screen_select_world
		   && "the title's Play row did not open the Load tab");
	assert(play_session_tab() == PLAY_TAB_LOAD
		   && "a fresh Play entry did not select the Load tab");
	pf_follow();
}

/* Walk the Load tab's focus onto the Create TAB HEADER with directional
 * presses only -- the route every family has. Bounded so a route that does not
 * exist fails by name instead of spinning. */
static void pf_focus_create_tab(const struct fe_roles* r) {
	for(int guard = 0; guard < 24; guard++) {
		if(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_TAB_CREATE)
			return;
		pf_tap(guard % 2 == 0 ? r->up : r->right);
	}
	assert(screen_sworld_focus_target() == SCREEN_SWORLD_FOCUS_TAB_CREATE
		   && "no directional route from the Load tab to the Create tab header");
}

/* Open the Create tab from the Load tab (the caller is already on Load). */
static void pf_open_create_tab(const struct fe_roles* r) {
	pf_focus_create_tab(r);
	pf_tap(r->click);
	assert(fe_screen_last == &screen_create_world
		   && "activating the Create tab header did not open the Create tab");
	assert(fe_screen_sets == 1
		   && "opening the Create tab handed off more than once");
	assert(play_session_tab() == PLAY_TAB_CREATE
		   && "the Create tab did not become the session's tab");
	pf_follow();
}

/* Title -> Load -> Create, all through real presses. Leaves pf_current on the
 * Create screen with a FRESH transition scope. */
static void pf_enter_create(const struct fe_roles* r) {
	pf_enter_play(r);
	pf_open_create_tab(r);
	assert(pf_current == &screen_create_world);
}

/* THE SCREEN ROWS, by d-pad -- the route all three families have. The
 * approved screen walks DOWN in Beta's visual order: NAME -> SEED -> MODE ->
 * CREATE (or Details while Create is disabled) and wraps. The model's CANCEL
 * row is the action row's Details / Cancel pair: it is reached by RIGHT from
 * Create world (landing on Details). The row this lands on is not assumed:
 * every case that uses this ends by asserting the committed `focus`. */
static void pf_focus_row_from_reset(const struct fe_roles* r, int row) {
	struct cworld_form current = screen_cworld_test_snapshot(NULL);
	int walk_to = row == CWORLD_ROW_CANCEL ? CWORLD_ROW_CREATE : row;
	for(int k = 0; k < CWORLD_ROW_COUNT && current.focus != row
		&& current.focus != walk_to; k++) {
		pf_tap(r->down);
		current = screen_cworld_test_snapshot(NULL);
	}
	if(row == CWORLD_ROW_CANCEL && current.focus == CWORLD_ROW_CREATE) {
		pf_tap(r->right); /* Create world -> Details */
		current = screen_cworld_test_snapshot(NULL);
	}
	assert(current.focus == row);
}

/* LEAVE the Create tab with the Back/Close role and return the snapshot it
 * committed. This is the observation point for every field assertion here. */
static bool pf_leave_create_and_restore(const struct fe_roles* r,
										struct cworld_form* out) {
	pf_tap(r->close);
	assert(fe_screen_sets == 1
		   && "leaving the Create tab did not hand off exactly once");
	pf_follow();
	return play_session_restore_draft(out);
}

/* ---- cases -------------------------------------------------------------- */

/* 1. The route itself: Title -> Load -> Create is reachable with a controller,
 *    and the session follows it. */
static void case_route_title_load_create(const struct fe_roles* r) {
	pf_saves(3);
	pf_enter_create(r);
}

/* 2. The approved Create geometry, proven through the POINTER: a click at the
 *    centre of each approved rect must focus that control, and the committed
 *    snapshot says which one it was.
 *
 *    REMOTE ONLY, on purpose. The Classic is refused a pointer by
 *    input_owner_latch and the GameCube has none, so there is no honest way to
 *    assert a pixel rect for them -- their coverage is the d-pad ring, which
 *    every other case runs on all three families. */
static void case_create_geometry_pointer(const struct fe_roles* r) {
	if(fe_family != FE_REMOTE)
		return;
	static const struct {
		float x, y, w, h;
		int row;
		const char* what;
	} rects[] = {
		{PF_NAME_X, PF_NAME_Y, PF_NAME_W, PF_NAME_H, CWORLD_ROW_NAME,
		 "Name 104,82 432x44"},
		{PF_SEED_X, PF_SEED_Y, PF_SEED_W, PF_SEED_H, CWORLD_ROW_SEED,
		 "Seed 104,178 300x44"},
		{PF_NEWSEED_X, PF_SEED_Y, PF_NEWSEED_W, PF_SEED_H, CWORLD_ROW_SEED,
		 "New seed 412,178 124x44"},
		{PF_MODE_SURVIVAL_X, PF_MODE_Y, PF_MODE_W, PF_MODE_H, CWORLD_ROW_MODE,
		 "Survival 104,276 212x44"},
		{PF_MODE_CREATIVE_X, PF_MODE_Y, PF_MODE_W, PF_MODE_H, CWORLD_ROW_MODE,
		 "Creative 324,276 212x44"},
		{PF_CREATE_X, PF_CREATE_Y, PF_CREATE_W, PF_CREATE_H, CWORLD_ROW_CREATE,
		 "Create 100,366 212x48"},
		{PF_DETAILS_X, PF_DETAILS_Y, PF_DETAILS_W, PF_DETAILS_H,
		 CWORLD_ROW_CANCEL, "Details 320,366 104x48"},
		{PF_CANCEL_X, PF_CANCEL_Y, PF_CANCEL_W, PF_CANCEL_H, CWORLD_ROW_CANCEL,
		 "Cancel 432,366 108x48"},
	};
	for(size_t k = 0; k < sizeof(rects) / sizeof(rects[0]); k++) {
		pf_saves(3);
		pf_enter_create(r);
		pf_point_at(rects[k].x, rects[k].y, rects[k].w, rects[k].h);
		pf_step(); /* the frame the pointer moves onto the control */
		struct cworld_form f;
		bool got = pf_leave_create_and_restore(r, &f);
		pf_pointer_off();
		assert(got && "the Create tab committed no draft on leave");
		assert(f.focus == rects[k].row && rects[k].what);
	}
}

/* 3. THE CARD'S CLAIM: Create -> Load -> Create retains the ACTUAL typed name,
 *    the RAW seed text, the game mode and the focus row -- across a real tab
 *    switch, through play_session. All three families, d-pad only. */
static void case_create_draft_survives_tab_round_trip(const struct fe_roles* r) {
	pf_saves(3);
	pf_enter_create(r);
	pf_pointer_off();

	/* NAME: clear Beta's default and type a NON-DEFAULT one, so "the name was
	 * retained" cannot pass on a form that was simply re-reset. Lower case
	 * only -- the Shift key would add a page flip that proves nothing extra
	 * here (test_osk_env owns the page tables). */
	pf_focus_row_from_reset(r, CWORLD_ROW_NAME);
	pf_tap(r->click); /* modal opens on the name row */
	pf_nav_begin(OSK_PAGE_LOWER, "New World", CWORLD_NAME_CAP,
				 CWORLD_NAME_MAX);
	pf_nav_clear_field(r);
	static const char pf_name[] = "native";
	for(size_t k = 0; pf_name[k]; k++)
		pf_nav_type(r, OSK_PAGE_LOWER, pf_name[k]);
	pf_nav_done(r);
	assert(fe_screen_sets == 0 && "the modal's OK key left the Create tab");

	/* SEED: its editor opens on the numeric page, so "7" is one unambiguous
	 * character and the raw text is exactly "7". */
	pf_tap(r->down); /* NAME -> SEED (Beta's field order) */
	pf_tap(r->click);
	pf_nav_begin(OSK_PAGE_NUM, "", CWORLD_SEED_CAP, 0);
	pf_nav_type(r, OSK_PAGE_NUM, '7');
	pf_nav_done(r);
	assert(fe_screen_sets == 0);

	/* MODE: exactly ONE toggle off the Survival default. */
	pf_tap(r->down); /* SEED -> MODE */
	pf_tap(r->right);

	struct cworld_form first;
	assert(pf_leave_create_and_restore(r, &first)
		   && "the Create tab committed no draft on leave");
	assert(strcmp(first.name, pf_name) == 0
		   && "the typed name was not committed verbatim");
	assert(strcmp(first.seed, "7") == 0
		   && "the typed seed text was not committed verbatim");
	assert(first.mode == CWORLD_MODE_CREATIVE
		   && "one RIGHT off Survival did not land on Creative");

	/* Back into Create through the REAL tab header. Its reset() re-runs, and it
	 * must re-seat from the session instead of wiping the draft. */
	pf_open_create_tab(r);

	struct cworld_form again;
	assert(pf_leave_create_and_restore(r, &again)
		   && "the re-entered Create tab committed no draft");
	assert(strcmp(again.name, pf_name) == 0
		   && "re-entering the Create tab wiped the typed name");
	assert(strcmp(again.seed, "7") == 0
		   && "re-entering the Create tab wiped the raw seed text");
	assert(again.mode == CWORLD_MODE_CREATIVE
		   && "re-entering the Create tab wiped the game mode");
	assert(again.focus == first.focus
		   && "re-entering the Create tab wiped the focus row");
}

/* 4. The modal's DONE keeps the text and CLOSES -- it does not create a world
 *    and it does not leave the screen. (Beta's OK key commits the field; only
 *    the Create button creates.) */
static void case_modal_done_commits_without_creating(const struct fe_roles* r) {
	pf_saves(3);
	pf_enter_create(r);
	pf_pointer_off();
	unsigned creates_before = fe_world_create_attempts;

	pf_focus_row_from_reset(r, CWORLD_ROW_SEED);
	pf_tap(r->click);
	pf_nav_begin(OSK_PAGE_NUM, "", CWORLD_SEED_CAP, 0);
	pf_nav_type(r, OSK_PAGE_NUM, '4');
	pf_nav_done(r);
	assert(fe_screen_sets == 0
		   && "the modal's OK key left the Create tab");
	assert(fe_world_create_attempts == creates_before
		   && "the modal's OK key tried to create a world");

	struct cworld_form f;
	assert(pf_leave_create_and_restore(r, &f));
	assert(strcmp(f.seed, "4") == 0 && "OK did not keep the typed seed");
}

/* 5. Cancel in the modal REVERTS the field and closes it, leaving the tab
 *    alone. The close role is the project's menu-back verb. */
static void case_modal_cancel_reverts(const struct fe_roles* r) {
	pf_saves(3);
	pf_enter_create(r);
	pf_pointer_off();

	pf_focus_row_from_reset(r, CWORLD_ROW_SEED);
	pf_tap(r->click);
	pf_nav_begin(OSK_PAGE_NUM, "", CWORLD_SEED_CAP, 0);
	pf_nav_type(r, OSK_PAGE_NUM, '9');
	pf_tap(r->close); /* B while the modal is up: revert + close */
	assert(fe_screen_sets == 0
		   && "cancelling the modal left the Create tab as well");

	struct cworld_form f;
	assert(pf_leave_create_and_restore(r, &f));
	assert(f.seed[0] == '\0'
		   && "cancelling the modal kept the reverted seed text");
}

/* 6. A tab/page press while the modal is open must not escape the screen: the
 *    modal owns the frame. */
static void case_tab_press_while_modal_does_not_escape(
	const struct fe_roles* r) {
	pf_saves(3);
	pf_enter_create(r);
	pf_pointer_off();

	pf_focus_row_from_reset(r, CWORLD_ROW_NAME);
	pf_tap(r->click); /* modal open on the name row */
    assert(screen_cworld_test_snapshot(NULL).editing);
	pf_tap(r->page);
	pf_tap(r->previous);
	assert(fe_screen_sets == 0
		   && "a page/tab role escaped the Create tab while the modal was up");
	if(fe_family == FE_REMOTE) {
		/* Cancel is a real rect on this screen that leaves it; clicking it
		 * while the modal is up must not leave either. */
		pf_point_at(PF_CANCEL_X, PF_CANCEL_Y, PF_CANCEL_W, PF_CANCEL_H);
		pf_step();
		pf_tap(r->click);
		pf_pointer_off();
		assert(fe_screen_sets == 0
			   && "a Cancel click escaped the Create screen under the modal");
	}
}

/* 7. DETAILS: opens IN PLACE, takes the frame while it is up, its Back returns,
 *    and the draft survives the round trip.
 *
 *    The first draft of this case passed if BOTH clicks were no-ops -- "no
 *    transition" is not evidence that anything opened. So the middle of the
 *    round trip now PROBES: with Details up, an activate aimed at the Create
 *    button underneath must NOT reach it (fe_world_create_attempts unchanged),
 *    and after Back the tab must still be leavable and the draft intact.
 *    Remote-only, because Details and its Back are pointer rects; the
 *    equivalent d-pad route does not exist in the approved design yet (see the
 *    gap note at the bottom of this file). */
static void case_details_round_trip_keeps_draft(const struct fe_roles* r) {
	pf_saves(3);
	pf_enter_create(r);
	pf_pointer_off();

	pf_focus_row_from_reset(r, CWORLD_ROW_SEED);
	pf_tap(r->click);
	pf_nav_begin(OSK_PAGE_NUM, "", CWORLD_SEED_CAP, 0);
	pf_nav_type(r, OSK_PAGE_NUM, '3');
	pf_nav_done(r);

	unsigned creates_before = fe_world_create_attempts;
	pf_focus_row_from_reset(r, CWORLD_ROW_CANCEL);
	pf_tap(r->click);
	bool details = false;
	(void)screen_cworld_test_snapshot(&details);
	assert(details && "Details did not open");
	assert(fe_screen_sets == 0
		   && "Details left the Create tab instead of opening in place");

	/* THE PROBE: Details is up, so the Create button behind it is unreachable.
	 * On a screen where Details never opened, this press lands on Create and
	 * the counter moves -- which is exactly the both-clicks-no-op hole. */
	if(fe_family == FE_REMOTE) {
		pf_pointer(2,2);
		pf_step();
		pf_point_at(PF_DETAILS_BACK_X,PF_DETAILS_BACK_Y,
			PF_DETAILS_BACK_W,PF_DETAILS_BACK_H);
		pf_step();
		pf_point_at(PF_CREATE_X, PF_CREATE_Y, PF_CREATE_W, PF_CREATE_H);
		pf_step();
		pf_tap(r->click);
	}
	pf_tap2(r->down, r->click);
	(void)screen_cworld_test_snapshot(&details);
	assert(details && "navigate plus activate dismissed Details");
	assert(fe_world_create_attempts == creates_before
		   && "an activate reached the Create button while Details was open");

	/* The Remote still has valid IR over the covered Create button. Once a
	 * direction owns focus, Back must activate despite that inert hover. */
	pf_step();
	pf_tap(r->click);
	struct cworld_form closed = screen_cworld_test_snapshot(&details);
	assert(!details && closed.focus == CWORLD_ROW_CANCEL);
	assert(fe_screen_sets == 0
		   && "the Details Back control left the Create tab");
	pf_pointer_off();

	struct cworld_form f;
	assert(pf_leave_create_and_restore(r, &f)
		   && "the tab could not be left after the Details round trip");
	assert(strcmp(f.seed, "3") == 0
		   && "the Details round trip dropped the typed seed");
}

/* 8. A FRESH title entry resets the session -- the retained draft is gone --
 *    while a tab switch never does. */
static void case_fresh_title_entry_drops_draft(const struct fe_roles* r) {
	pf_saves(3);
	pf_enter_create(r);
	pf_pointer_off();

	pf_focus_row_from_reset(r, CWORLD_ROW_SEED);
	pf_tap(r->click);
	pf_nav_begin(OSK_PAGE_NUM, "", CWORLD_SEED_CAP, 0);
	pf_nav_type(r, OSK_PAGE_NUM, '5');
	pf_nav_done(r);

	struct cworld_form kept;
	assert(pf_leave_create_and_restore(r, &kept));
	assert(strcmp(kept.seed, "5") == 0);

	pf_enter_play(r); /* the title's Play row again = a FRESH entry */
	struct cworld_form after;
	assert(!play_session_restore_draft(&after)
		   && "a fresh title Play entry kept the previous Create draft");
}

/* 9. Same-frame navigate + activate is ONE action (Astra ruling 2), and a HELD
 *    activate does not re-fire -- on the Create tab's own rows. */
static void case_same_frame_and_held_edges(const struct fe_roles* r) {
	pf_saves(3);
	pf_enter_create(r);
	pf_pointer_off();

	/* DOWN + activate on the same poll: the press the rule consumed must not
	 * also open an editor on the row the move landed on. */
	pf_tap(r->up); /* Name -> Create world (the rows wrap) */
	pf_tap2(r->down, r->click); /* Create -> Name would open the name editor */
	assert(!screen_cworld_test_snapshot(NULL).editing);
	struct cworld_form f;
	assert(pf_leave_create_and_restore(r, &f)
		   && "the Create tab committed no draft on leave");
	assert(!f.editing
		   && "a same-frame move+activate opened the modal on the new row");

	/* HELD activate across several frames: one action, then re-armed. */
	pf_open_create_tab(r);
	unsigned creates_before = fe_world_create_attempts;
	pf_focus_row_from_reset(r, CWORLD_ROW_CREATE);
	pf_hold_frames(r->click, 4);
	assert(fe_world_create_attempts == creates_before + 1
		   && "a HELD activate on Create re-fired the create attempt");
	pf_release();
	pf_tap(r->click);
	assert(fe_world_create_attempts == creates_before + 2);
	pf_tap(r->close);
	assert(fe_screen_sets == 1
		   && "the Close role did not re-arm after the held activate");
	pf_follow();
	struct cworld_form retained;
	assert(play_session_restore_draft(&retained));
	assert(!strcmp(retained.name, f.name) && !strcmp(retained.seed, f.seed)
		   && retained.mode == f.mode);
}

static void case_create_pointer_controls(const struct fe_roles* r) {
	if(fe_family != FE_REMOTE)
		return;
	unsigned creates_before = fe_world_create_attempts;
	pf_saves(3);
	pf_enter_create(r);
	pf_point_at(PF_MODE_CREATIVE_X, PF_MODE_Y, PF_MODE_W, PF_MODE_H);
	pf_step();
	pf_tap(r->click);
	pf_tap(r->click);
	assert(screen_cworld_test_snapshot(NULL).mode == CWORLD_MODE_CREATIVE);
	pf_point_at(PF_MODE_SURVIVAL_X, PF_MODE_Y, PF_MODE_W, PF_MODE_H);
	pf_step();
	pf_tap(r->click);
	pf_tap(r->click);
	assert(screen_cworld_test_snapshot(NULL).mode == CWORLD_MODE_SURVIVAL);

	pf_point_at(PF_NAME_X, PF_NAME_Y, PF_NAME_W, PF_NAME_H);
	pf_step();
	pf_tap(r->down); /* Name -> Seed, Beta's order */
	assert(screen_cworld_test_snapshot(NULL).focus == CWORLD_ROW_SEED);
	fe_ir_x += 1.0F;
	pf_step();
	assert(screen_cworld_test_snapshot(NULL).focus == CWORLD_ROW_SEED);
	pf_pointer_off();
	pf_step();
	fe_ir_armed = true;
	pf_step();
	assert(screen_cworld_test_snapshot(NULL).focus == CWORLD_ROW_SEED);
	pf_pointer_off();

	pf_focus_row_from_reset(r, CWORLD_ROW_NAME);
	pf_tap(r->click);
	pf_nav_begin(OSK_PAGE_LOWER, "New World", CWORLD_NAME_CAP, 32);
	pf_nav_clear_field(r);
	pf_nav_done(r);
	pf_focus_row_from_reset(r, CWORLD_ROW_SEED);
	pf_point_at(PF_CREATE_X, PF_CREATE_Y, PF_CREATE_W, PF_CREATE_H);
	pf_step();
	pf_tap(r->click);
	assert(!screen_cworld_test_snapshot(NULL).editing);
	assert(fe_world_create_attempts == creates_before);
	pf_pointer_off();
}

/* The approved Create screen has NO tab strip (opus-B-create): Join is
 * reached from the Load tab. UP from Name wraps to the action row instead of
 * climbing to a header, and nothing on the form hands off to Join. */
static void case_create_join_route(const struct fe_roles* r) {
	struct screen destination = {0};
	struct screen* previous = screen_multiplayer_setup;
	screen_multiplayer_setup = &destination;
	pf_saves(3);
	pf_enter_create(r);
	pf_pointer_off();
	pf_tap(r->up);
	assert(screen_cworld_test_snapshot(NULL).focus == CWORLD_ROW_CREATE
		   && "UP from Name did not wrap to Create world");
	pf_tap(r->right);
	pf_tap(r->click); /* Details, in place */
	bool details = false;
	(void)screen_cworld_test_snapshot(&details);
	assert(details && fe_screen_sets == 0 && play_session_tab() != PLAY_TAB_JOIN
		   && "the Create screen handed off toward Join");
	pf_tap(r->close); /* closes Details only */
	assert(fe_screen_sets == 0);
	pf_tap(r->close); /* back to the Load tab, whose strip reaches Join */
	assert(fe_screen_last == &screen_select_world && fe_screen_sets == 1
		   && play_session_tab() == PLAY_TAB_LOAD);
	pf_follow();
	screen_multiplayer_setup = previous;
}

/* ---- run ---------------------------------------------------------------- */

/* Cancel (Beta's id 1) returns to the parent without creating; and Astra
 * ruling 5 D2 ("last input wins") on the approved Cancel button: a sprite that
 * REAPPEARS over Cancel without moving does not own focus, so A answers the
 * ring the D-pad put on Create; MOVING it onto Cancel hands focus over. */
static void case_create_footer_back(const struct fe_roles* r) {
    pf_saves(3); pf_enter_create(r); pf_pointer_off();
    pf_focus_row_from_reset(r,CWORLD_ROW_CANCEL);
    pf_tap(r->right); /* Details -> Cancel */
    unsigned attempts=fe_world_create_attempts;
    pf_tap(r->click);
    assert(fe_screen_last==&screen_select_world&&fe_world_create_attempts==attempts
        &&"Cancel did not return to the Load tab");
    if(fe_family!=FE_REMOTE)return;
    pf_enter_create(r); pf_pointer_off();
    pf_focus_row_from_reset(r,CWORLD_ROW_CREATE);
    float cx=pf_cx(PF_CANCEL_X+PF_CANCEL_W/2.0F), cy=pf_cy(PF_CANCEL_Y+PF_CANCEL_H/2.0F);
    pf_pointer(cx,cy);pf_step();
    attempts=fe_world_create_attempts;
    pf_tap(r->click);
    assert(fe_screen_sets==0&&fe_world_create_attempts==attempts+1
        &&"a stationary reacquired sprite on Cancel overrode the ring's Create (ruling 5 D2)");
    attempts=fe_world_create_attempts;
    pf_pointer(cx+(float)(3*GUTIL_MENU_POINTER_JITTER)/2.0F,cy);pf_step();
    assert(fe_screen_sets==0&&fe_world_create_attempts==attempts);
    pf_tap(r->click);
    assert(fe_screen_last==&screen_select_world&&fe_world_create_attempts==attempts
        &&"the Cancel button activated Create instead of returning");
    pf_pointer_off();
}

static void run_family(enum fe_family family, bool remapped) {
	pf_config_open(remapped);
	fe_attach(family);
	struct fe_roles r = fe_roles_for(family);

	case_route_title_load_create(&r);
	case_create_geometry_pointer(&r);
	case_create_draft_survives_tab_round_trip(&r);
	case_create_footer_back(&r);
	case_modal_done_commits_without_creating(&r);
	case_modal_cancel_reverts(&r);
	case_tab_press_while_modal_does_not_escape(&r);
	case_details_round_trip_keeps_draft(&r);
	case_fresh_title_entry_drops_draft(&r);
	case_same_frame_and_held_edges(&r);
	case_create_pointer_controls(&r);
	case_create_join_route(&r);

	printf("PASS Create tab lifecycle (%s, %s bindings)\n",
		   fe_family_name[family], remapped ? "remapped" : "shipped");
}

int main(int argc, char** argv) {
	/* The Create path really writes: cworld_probe_unused stat()s the saves
	 * directory and a successful Create would mkdir in it, and the screens
	 * below reach config_save(..., "config.json") with a HARD-CODED relative
	 * path. ctest runs this target from the SOURCE ROOT, so the whole run
	 * happens inside a scratch directory instead. Purge the exact set of files
	 * this target can create first: fe_rmdir is rmdir, which removes only an
	 * EMPTY directory, so an aborted run must not be silently continued.
	 *
	 * The scratch directory is created under argv[1] (ctest passes the BUILD
	 * tree), never in the source root: an untracked directory there flips
	 * tools/worktree-stamp.sh mid-run, and local_ci then reports "PARTIAL --
	 * tree changed during run" (G3, 09-26). config_wii.json is opened from
	 * the source root BEFORE leaving it. With no argument (a hand run) the
	 * scratch dir stays in the CWD, as before. */
	FILE* shipped_config = fopen("config_wii.json", "rb");
	assert(shipped_config
		   && "config_wii.json missing; is the CWD the source root?");
	if(argc > 1)
		assert(fe_chdir(argv[1]) == 0
			   && "could not enter the scratch parent (argv[1])");
	{
		static const char* const stale[] = {
			PF_RUN_DIR "/config.json",
			PF_RUN_DIR "/config_wii.json",
			PF_RUN_DIR "/" PF_REMAP_CONFIG,
		};
		for(size_t k = 0; k < sizeof(stale) / sizeof(stale[0]); k++) {
			remove(stale[k]);
			fe_rmdir(stale[k]);
		}
		for(size_t k = 0; k < 32; k++) {
			char path[192];
			snprintf(path, sizeof(path),
					 PF_RUN_DIR "/" PF_SAVES_DIR "/world%02zu", k);
			fe_rmdir(path);
		}
		fe_rmdir(PF_RUN_DIR "/" PF_SAVES_DIR);
	}
	fe_rmdir(PF_RUN_DIR);
	assert((fe_mkdir(PF_RUN_DIR) == 0 || errno == EEXIST)
		   && "could not create the scratch run directory");
	{
		FILE* in = shipped_config;
		FILE* out = fopen(PF_RUN_DIR "/config_wii.json", "wb");
		assert(out);
		char buf[4096];
		size_t n;
		while((n = fread(buf, 1, sizeof(buf), in)) > 0)
			assert(fwrite(buf, 1, n, out) == n);
		fclose(in);
		fclose(out);
	}
	assert(fe_chdir(PF_RUN_DIR) == 0 && "could not enter the scratch run dir");

	for(int remapped = 0; remapped < 2; remapped++)
		for(int family = 0; family < FE_FAMILY_COUNT; family++)
			run_family((enum fe_family)family, remapped != 0);

	/* Leave nothing behind: the exact set this target can create, then the
	 * (now empty) scratch dir itself, after stepping out of it. */
	pf_saves(0);
	remove(PF_REMAP_CONFIG);
	remove("config_wii.json");
	remove("config.json");
	if(pf_config_live)
		config_destroy(&gstate.config_user);
	pf_config_live = false;
	assert(fe_chdir("..") == 0);
	fe_rmdir(PF_RUN_DIR);

	printf("PASS test_frontend_play_flow\n");
	return 0;
}

/* This headless matrix proves screen state and input routing across all three
 * controller families and remaps. Native presentation and physical-Wii timing
 * require their separate capture/hardware evidence. */
