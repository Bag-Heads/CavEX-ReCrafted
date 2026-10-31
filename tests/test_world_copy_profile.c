/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* ui-build-1026/T16a: the world_copy_convert route (menu_autopilot_script.c,
 * menu_ap_copy_*), driven against a small model of the three screens it
 * walks: Title, Select World (with the disposable fixture row among user
 * rows) and screen_convert_backup's state/focus as its production observers
 * would report them.
 *
 * What is pinned:
 *   - the OK route reaches Done and Back to worlds, and every evidence line
 *     the collector requires (WORLD_COPY_MARKERS) is formatted and sent, END
 *     last;
 *   - every activate lands on the named target: Play only with the FIXTURE
 *     row selected, Back up & convert only (never Convert without backup,
 *     never Play world), Back / Cancel on the result;
 *   - a user world is never pressed: no fixture row, or an untagged fixture,
 *     ends the route before any press on the list;
 *   - unregistered observers fail by name with ZERO presses;
 *   - low space / write error / interruption / recovery each end where the
 *     collector expects, with their markers;
 *   - the formatted lines fit the 320-byte buffer at extreme values.
 * Pure: links only menu_autopilot_script.c (same as
 * test_menu_autopilot_script). */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../source/game/menu_autopilot_script.h"

#define FIXTURE "cw_wcc_fixture"

/* MV_WRITE_FAST: hw1028a's real-Wii shape -- the rig's write fault hits the
 * .incomplete marker, and the whole cleanup of that near-empty tree runs
 * inside the failing world_copy_step, so COPYING goes straight to ERROR
 * and "Cleaning up..." is never observable; cleanup_done still is. */
enum model_variant { MV_OK, MV_LOW, MV_WRITE, MV_INTERRUPT, MV_WRITE_FAST };

struct model {
	enum menu_ap_screen screen;
	int title_focus;
	/* Select World */
	int rows;
	int fixture_row, backup_row;
	bool fixture_tagged;
	bool partial;
	int selected;
	int sworld_focus;
	int settle_frames;
	/* convert screen */
	enum model_variant mv;
	int cb_state, cb_fail, cb_err, cb_outcome;
	bool published;
	bool cleanup_done; /* screen_convert_backup_cleanup_done() */
	int cb_sel; /* index into the current button list */
	int phase;	/* frames in the current cb_state */
	int percent;
	bool frozen; /* interrupted */
	/* audit */
	int clicks_on_user_row; /* Play activated with a non-fixture row */
	int no_backup_clicks, play_world_clicks, loaded_world;
	int interrupts;
	int presses;
};

static int cb_buttons(const struct model* m, int out[3]) {
	switch(m->cb_state) {
		case MENU_AP_CB_ASK:
			out[0] = MENU_AP_CB_ACT_BACKUP;
			out[1] = MENU_AP_CB_ACT_NO_BACKUP;
			out[2] = MENU_AP_CB_ACT_CANCEL;
			return 3;
		case MENU_AP_CB_ERROR:
			out[0] = MENU_AP_CB_ACT_NO_BACKUP;
			out[1] = MENU_AP_CB_ACT_CANCEL;
			return 2;
		case MENU_AP_CB_DONE:
		case MENU_AP_CB_SAVE_PENDING:
			out[0] = MENU_AP_CB_ACT_PLAY;
			out[1] = MENU_AP_CB_ACT_BACK;
			return 2;
		case MENU_AP_CB_CONVERT_ERROR: out[0] = MENU_AP_CB_ACT_BACK; return 1;
		default: return 0;
	}
}

static void cb_enter(struct model* m, int st) {
	int b[3];
	m->cb_state = st;
	m->phase = 0;
	int n = cb_buttons(m, b);
	/* the approved default focus: Play world on Done, else the last */
	m->cb_sel = (st == MENU_AP_CB_DONE || st == MENU_AP_CB_SAVE_PENDING) ? 0
		: n - 1;
}

static void model_init(struct model* m, enum model_variant mv) {
	memset(m, 0, sizeof(*m));
	m->screen = MENU_AP_SCREEN_TITLE;
	m->title_focus = MENU_AP_TITLE_NONE;
	m->rows = 3; /* "New World", the fixture, "Survival 2" */
	m->fixture_row = 1;
	m->backup_row = -1;
	m->fixture_tagged = true;
	m->selected = 0;
	m->sworld_focus = MENU_AP_SWORLD_LIST;
	m->mv = mv;
	m->cb_state = MENU_AP_CB_NONE;
}

static struct menu_ap_copy_obs model_obs(const struct model* m) {
	struct menu_ap_copy_obs o = {
		.observed = true,
		.title_focus = m->title_focus,
		.sworld_focus = m->sworld_focus,
		.sworld_settled = m->settle_frames > 3,
		.sworld_rows = m->rows,
		.sworld_selected = m->selected,
		.fixture_row = m->fixture_row,
		.fixture_needs_convert = m->fixture_row >= 0 && m->fixture_tagged,
		.backup_row = m->backup_row,
		.partial_visible = m->partial,
		.cb_state = m->cb_state,
		.cb_action = MENU_AP_CB_ACT_NONE,
		.cb_fail = m->cb_fail,
		.cb_copy_error = m->cb_err,
		.cb_percent = m->cb_state == MENU_AP_CB_COPYING ? m->percent : -1,
		.cb_free_bytes = m->mv == MV_LOW ? 4096u : 7948206080ull,
		.cb_cluster_bytes = 32768u,
		.cb_required_bytes = 393216u,
		.cb_convert_outcome = m->cb_outcome,
		.cb_backup_published = m->published,
		.cb_cleanup_done = m->cleanup_done,
	};
	int b[3];
	int n = cb_buttons(m, b);
	if(n && m->cb_sel >= 0 && m->cb_sel < n)
		o.cb_action = b[m->cb_sel];
	return o;
}

static void model_press(struct model* m, enum input_button b) {
	m->presses++;
	switch(m->screen) {
		case MENU_AP_SCREEN_TITLE:
			if(b == IB_GUI_DOWN)
				m->title_focus = m->title_focus < MENU_AP_TITLE_QUIT
					? m->title_focus + 1 : m->title_focus;
			else if(b == IB_GUI_UP)
				m->title_focus = m->title_focus > MENU_AP_TITLE_PLAY
					? m->title_focus - 1 : MENU_AP_TITLE_PLAY;
			else if(b == IB_GUI_CLICK && m->title_focus == MENU_AP_TITLE_PLAY) {
				m->screen = MENU_AP_SCREEN_SELECT_WORLD;
				m->settle_frames = 0;
			}
			break;
		case MENU_AP_SCREEN_SELECT_WORLD:
			if(m->sworld_focus == MENU_AP_SWORLD_LIST) {
				if(b == IB_GUI_DOWN && m->selected + 1 < m->rows)
					m->selected++;
				else if(b == IB_GUI_UP && m->selected > 0)
					m->selected--;
				else if(b == IB_GUI_RIGHT)
					m->sworld_focus = MENU_AP_SWORLD_PLAY;
			} else if(m->sworld_focus == MENU_AP_SWORLD_PLAY) {
				if(b == IB_GUI_LEFT)
					m->sworld_focus = MENU_AP_SWORLD_LIST;
				else if(b == IB_GUI_CLICK) {
					if(m->selected != m->fixture_row) {
						m->clicks_on_user_row++;
						m->loaded_world++;
						m->screen = MENU_AP_SCREEN_INGAME;
					} else if(m->fixture_tagged) {
						m->screen = MENU_AP_SCREEN_CONVERT_BACKUP;
						cb_enter(m, MENU_AP_CB_ASK);
					} else {
						m->loaded_world++;
						m->screen = MENU_AP_SCREEN_INGAME;
					}
				}
			}
			break;
		case MENU_AP_SCREEN_CONVERT_BACKUP: {
			int bt[3];
			int n = cb_buttons(m, bt);
			if(!n)
				break; /* no answers while copying / converting */
			if(b == IB_GUI_UP)
				m->cb_sel = (m->cb_sel + n - 1) % n;
			else if(b == IB_GUI_DOWN)
				m->cb_sel = (m->cb_sel + 1) % n;
			else if(b == IB_GUI_CLICK) {
				int act = bt[m->cb_sel];
				if(act == MENU_AP_CB_ACT_BACKUP) {
					cb_enter(m, MENU_AP_CB_VOLUME);
				} else if(act == MENU_AP_CB_ACT_NO_BACKUP) {
					m->no_backup_clicks++;
					cb_enter(m, MENU_AP_CB_CONVERTING);
				} else if(act == MENU_AP_CB_ACT_PLAY) {
					m->play_world_clicks++;
					m->loaded_world++;
					m->screen = MENU_AP_SCREEN_INGAME;
				} else {
					m->screen = MENU_AP_SCREEN_SELECT_WORLD;
					m->settle_frames = 0;
					m->sworld_focus = MENU_AP_SWORLD_LIST;
				}
			}
			break;
		}
		default: break;
	}
}

/* One frame of the screen's own work, AFTER input. */
static void model_frame(struct model* m) {
	m->settle_frames++;
	if(m->screen != MENU_AP_SCREEN_CONVERT_BACKUP || m->frozen)
		return;
	m->phase++;
	switch(m->cb_state) {
		case MENU_AP_CB_VOLUME:
			cb_enter(m, MENU_AP_CB_COPYING);
			m->percent = -1; /* surveying */
			break;
		case MENU_AP_CB_COPYING:
			if(m->mv == MV_LOW) { /* start refuses: LOW_SPACE */
				m->cb_fail = MENU_AP_CB_FAIL_COPY;
				m->cb_err = MENU_AP_WC_LOW_SPACE;
				cb_enter(m, MENU_AP_CB_ERROR);
				break;
			}
			m->percent = m->phase < 2 ? 0 : (m->phase - 1) * 30;
			if(m->mv == MV_WRITE_FAST && m->phase >= 2) {
				m->cb_fail = MENU_AP_CB_FAIL_COPY;
				m->cb_err = MENU_AP_WC_IO_ERROR;
				m->cleanup_done = true;
				cb_enter(m, MENU_AP_CB_ERROR);
				break;
			}
			if(m->mv == MV_WRITE && m->percent >= 60) {
				cb_enter(m, MENU_AP_CB_CLEANING);
				break;
			}
			if(m->percent >= 100) {
				m->percent = 100;
				m->published = true;
				cb_enter(m, MENU_AP_CB_CONVERTING);
			}
			break;
		case MENU_AP_CB_CLEANING:
			if(m->phase >= 3) {
				m->cb_fail = MENU_AP_CB_FAIL_COPY;
				m->cb_err = MENU_AP_WC_IO_ERROR;
				m->cleanup_done = true;
				cb_enter(m, MENU_AP_CB_ERROR);
			}
			break;
		case MENU_AP_CB_CONVERTING:
			if(m->phase >= 5) {
				m->cb_outcome = MENU_AP_CONVERT_SUCCEEDED;
				m->fixture_tagged = false;
				m->backup_row = m->rows++; /* the published backup row */
				cb_enter(m, MENU_AP_CB_DONE);
			}
			break;
		default: break;
	}
}

/* What the driver's readdir of the per-run worlds root would return. */
static void model_card(struct menu_ap_copy_state* s, const struct model* m) {
	const char* names[4] = {FIXTURE};
	int n = 1;
	if(m->published)
		names[n++] = FIXTURE " (backup)";
	if(m->frozen) { /* interrupted: marked final folder and journal */
		names[n++] = FIXTURE " (backup)";
		names[n++] = FIXTURE " (backup)/.incomplete";
		names[n++] = ".world-copy-1.owner";
	}
	menu_ap_copy_set_card(s, true, names, n);
}

struct run {
	struct menu_ap_copy_state s;
	char lines[MENU_AP_WCC_COUNT][MENU_AP_COPY_LINE_MAX];
	int order[MENU_AP_WCC_COUNT];
	int sent;
	int ticks;
};

static void run_route(struct run* r, struct model* m,
					  enum menu_ap_copy_variant v, bool observed) {
	memset(r, 0, sizeof(*r));
	menu_ap_copy_reset(&r->s, v, FIXTURE);
	for(r->ticks = 0; r->ticks < 60 * 900; r->ticks++) {
		struct menu_ap_copy_obs o = model_obs(m);
		o.observed = observed;
		struct menu_ap_copy_output out;
		menu_ap_copy_tick(&r->s, m->screen, o, 1.0F / 60.0F, &out);
		if(out.press)
			model_press(m, out.button);
		if(out.interrupt) {
			m->interrupts++;
			m->frozen = true;
		}
		model_frame(m);
		if(menu_ap_copy_card_wanted(&r->s))
			model_card(&r->s, m);
		static char line[MENU_AP_COPY_LINE_MAX];
		int k;
		while((k = menu_ap_copy_next_marker(&r->s, line, sizeof(line))) >= 0) {
			assert(r->sent < MENU_AP_WCC_COUNT);
			snprintf(r->lines[k], sizeof(r->lines[k]), "%s", line);
			r->order[r->sent++] = k;
			menu_ap_copy_marker_sent(&r->s, k);
		}
		if(r->s.finished && (r->s.markers_ready & ~r->s.markers_sent) == 0u)
			break;
	}
	assert(r->s.finished);
	/* END is always sent, and always last. */
	assert(r->sent > 0 && r->order[r->sent - 1] == MENU_AP_WCC_END);
}

static unsigned sent_mask(const struct run* r) {
	unsigned m = 0;
	for(int k = 0; k < r->sent; k++)
		m |= 1u << r->order[k];
	return m;
}

static void assert_safe(const struct model* m) {
	assert(m->clicks_on_user_row == 0);
	assert(m->no_backup_clicks == 0);
	assert(m->play_world_clicks == 0);
	assert(m->loaded_world == 0);
}

static void case_ok(void) {
	struct model m;
	struct run r;
	model_init(&m, MV_OK);
	run_route(&r, &m, MENU_AP_COPY_OK, true);
	assert(r.s.ok && r.s.step == MENU_AP_COPY_STEP_DONE && r.s.failed_step == 0);
	assert_safe(&m);
	assert(m.interrupts == 0);
	assert(sent_mask(&r) == menu_ap_copy_required_markers(MENU_AP_COPY_OK));
	for(int k = 0; k < r.sent; k++)
		assert(r.order[k] <= r.order[r.sent - 1]);
	assert(strcmp(r.lines[MENU_AP_WCC_BEGIN],
				  "{\"event\":\"wcc_begin\",\"variant\":\"ok\","
				  "\"fixture\":\"cw_wcc_fixture\"}")
		   == 0);
	assert(strstr(r.lines[MENU_AP_WCC_LIST_PRE],
				  "\"fixture_row\":1,\"needs_convert\":true,"
				  "\"backup_row\":-1,\"partial_visible\":false"));
	assert(strstr(r.lines[MENU_AP_WCC_ASK], "\"safe_default\":true"));
	assert(strcmp(r.lines[MENU_AP_WCC_VOLUME],
				  "{\"event\":\"wcc_volume\",\"free_bytes\":7948206080,"
				  "\"cluster_bytes\":32768,\"required_bytes\":393216}")
		   == 0);
	/* 100% is never observed: the screen publishes and starts the convert
	 * on the step that finishes the copy (cb_copy_tick). */
	assert(strstr(r.lines[MENU_AP_WCC_COPY],
				  "\"saw_copying\":true,\"saw_cleaning\":false,"
				  "\"cleanup_done\":false,"
				  "\"pct_max\":90,\"monotonic\":true,\"interrupted\":false"));
	assert(strstr(r.lines[MENU_AP_WCC_RESULT],
				  "\"state\":6,\"fail\":0,\"copy_error\":0,\"outcome\":1,"
				  "\"backup_published\":true"));
	assert(strstr(r.lines[MENU_AP_WCC_LIST_POST],
				  "\"rows\":4,\"fixture_row\":1,\"needs_convert\":false,"
				  "\"backup_row\":3,\"partial_visible\":false"));
	assert(strcmp(r.lines[MENU_AP_WCC_CARD],
				  "{\"event\":\"wcc_card\",\"root_ok\":true,"
				  "\"truncated\":false,\"names\":[\"cw_wcc_fixture\","
				  "\"cw_wcc_fixture (backup)\"]}")
		   == 0);
	assert(strstr(r.lines[MENU_AP_WCC_END], "\"ok\":true"));
	puts("ok: OK route reaches Done and Back to worlds; 9/9 markers");
}

static void case_fixture_last_row_from_play_focus(void) {
	/* the ring starts on Play with a user row selected: the route goes back
	 * to the list, walks to the fixture, and only then activates */
	struct model m;
	struct run r;
	model_init(&m, MV_OK);
	m.rows = 6;
	m.fixture_row = 5;
	m.sworld_focus = MENU_AP_SWORLD_PLAY;
	run_route(&r, &m, MENU_AP_COPY_OK, true);
	assert(r.s.ok);
	assert_safe(&m);
	puts("ok: fixture found by directory from Play focus, user rows untouched");
}

static void case_variant(enum menu_ap_copy_variant v, enum model_variant mv,
						 int err, bool cleaning, bool cleaned) {
	struct model m;
	struct run r;
	model_init(&m, mv);
	run_route(&r, &m, v, true);
	assert(r.s.ok);
	assert_safe(&m);
	assert(sent_mask(&r) == menu_ap_copy_required_markers(v));
	char want[160];
	snprintf(want, sizeof(want),
			 "\"state\":3,\"fail\":3,\"copy_error\":%d,\"outcome\":0,"
			 "\"backup_published\":false",
			 err);
	assert(strstr(r.lines[MENU_AP_WCC_RESULT], want));
	assert(strstr(r.lines[MENU_AP_WCC_COPY], cleaning ? "\"saw_cleaning\":true"
													  : "\"saw_cleaning\":false"));
	assert(strstr(r.lines[MENU_AP_WCC_COPY], cleaned ? "\"cleanup_done\":true"
													 : "\"cleanup_done\":false"));
	/* Cancel back to the list: fixture still tagged, no backup row */
	assert(strstr(r.lines[MENU_AP_WCC_LIST_POST],
				  "\"rows\":3,\"fixture_row\":1,\"needs_convert\":true,"
				  "\"backup_row\":-1,\"partial_visible\":false"));
	printf("ok: %s ends on the backup error, Cancel, fixture unchanged\n",
		   menu_ap_copy_variant_name(v));
}

static void case_interrupt(void) {
	struct model m;
	struct run r;
	model_init(&m, MV_OK);
	run_route(&r, &m, MENU_AP_COPY_INTERRUPT, true);
	assert(r.s.ok && m.interrupts == 1 && m.frozen);
	assert(m.cb_state == MENU_AP_CB_COPYING && m.percent > 0 && m.percent < 100);
	assert(!m.published);
	assert_safe(&m);
	assert(sent_mask(&r) == menu_ap_copy_required_markers(MENU_AP_COPY_INTERRUPT));
	assert(strstr(r.lines[MENU_AP_WCC_COPY], "\"interrupted\":true"));
	assert(!(sent_mask(&r) & (1u << MENU_AP_WCC_RESULT)));
	assert(strstr(r.lines[MENU_AP_WCC_CARD], "\"cw_wcc_fixture (backup)/.incomplete\""));
	assert(strstr(r.lines[MENU_AP_WCC_CARD], "\".world-copy-1.owner\""));
	assert(!strstr(r.lines[MENU_AP_WCC_CARD], ".tmp"));
	puts("ok: interrupt asks the seam once, mid-copy, and ends");
}

static void case_recover(void) {
	/* The boot after an interruption: the pre-list is latched BEFORE any
	 * press, then the OK route runs. A visible partial row is reported, not
	 * hidden (the collector fails it). */
	struct model m;
	struct run r;
	model_init(&m, MV_OK);
	m.partial = true;
	run_route(&r, &m, MENU_AP_COPY_RECOVER, true);
	assert(r.s.ok);
	assert(strstr(r.lines[MENU_AP_WCC_LIST_PRE], "\"partial_visible\":true"));
	assert(strstr(r.lines[MENU_AP_WCC_BEGIN], "\"variant\":\"recover\""));
	puts("ok: recover reports the pre-press list truthfully");
}

static void case_unobserved(void) {
	struct model m;
	struct run r;
	model_init(&m, MV_OK);
	run_route(&r, &m, MENU_AP_COPY_OK, false);
	assert(!r.s.ok && r.s.step == MENU_AP_COPY_STEP_UNOBSERVED
		   && r.s.failed_step == MENU_AP_COPY_STEP_UNOBSERVED);
	assert(m.presses == 0);
	assert(sent_mask(&r)
		   == ((1u << MENU_AP_WCC_BEGIN) | (1u << MENU_AP_WCC_CARD)
			   | (1u << MENU_AP_WCC_END)));
	assert(strstr(r.lines[MENU_AP_WCC_END], "\"ok\":false"));
	puts("ok: unregistered observers fail by name with zero presses");
}

static void case_no_fixture(bool absent) {
	struct model m;
	struct run r;
	model_init(&m, MV_OK);
	if(absent)
		m.fixture_row = -1;
	else
		m.fixture_tagged = false;
	run_route(&r, &m, MENU_AP_COPY_OK, true);
	assert(!r.s.ok && r.s.failed_step == MENU_AP_COPY_STEP_LIST_PRE);
	assert(m.screen == MENU_AP_SCREEN_SELECT_WORLD);
	assert(m.selected == 0 && m.sworld_focus == MENU_AP_SWORLD_LIST);
	assert_safe(&m);
	/* only the title presses happened: nothing on the list */
	assert(m.presses == 2);
	printf("ok: %s fixture ends before any list press\n",
		   absent ? "absent" : "untagged");
}

static void case_names(void) {
	struct menu_ap_copy_state s;
	menu_ap_copy_reset(&s, MENU_AP_COPY_OK, "bad\"name");
	assert(s.finished && !s.ok && strcmp(s.fixture, "invalid") == 0);
	menu_ap_copy_reset(&s, MENU_AP_COPY_OK, "../saves");
	assert(s.finished && !s.ok);
	menu_ap_copy_reset(&s, (enum menu_ap_copy_variant)99, FIXTURE);
	assert(s.finished && !s.ok);
	for(int v = 0; v < MENU_AP_COPY_VARIANT_COUNT; v++)
		assert(menu_ap_copy_variant_parse(
				   menu_ap_copy_variant_name((enum menu_ap_copy_variant)v))
			   == v);
	assert(menu_ap_copy_variant_parse("nope") == -1);
	assert(menu_ap_copy_variant_parse(NULL) == -1);
	/* worst-case widths fit the 320-byte line the driver uses */
	menu_ap_copy_reset(&s, MENU_AP_COPY_WRITE_ERROR,
					   "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz0123456789a");
	assert(!s.finished);
	s.free_bytes = s.required_bytes = ~0ull;
	s.cluster_bytes = ~0u;
	s.pre_rows = s.pre_fixture_row = s.pre_backup_row = -2147483647 - 1;
	s.result_state = s.result_fail = s.result_copy_error = s.result_outcome
		= -2147483647 - 1;
	s.pct_max = s.step = s.failed_step = -2147483647 - 1;
	char line[320];
	for(int k = 0; k < MENU_AP_WCC_COUNT; k++)
		assert(menu_ap_copy_format_marker(&s, k, line, sizeof(line)) > 0);
	assert(menu_ap_copy_format_marker(&s, MENU_AP_WCC_COUNT, line, sizeof(line))
		   == -1);
	assert(menu_ap_copy_format_marker(&s, MENU_AP_WCC_BEGIN, line, 8) == -1);
	/* END waits for the card listing */
	{
		struct menu_ap_copy_state c;
		menu_ap_copy_reset(&c, MENU_AP_COPY_OK, FIXTURE);
		struct menu_ap_copy_obs o = {0};
		struct menu_ap_copy_output out;
		menu_ap_copy_tick(&c, MENU_AP_SCREEN_TITLE, o, 0.1F, &out);
		assert(c.finished && menu_ap_copy_card_wanted(&c));
		static char l2[MENU_AP_COPY_LINE_MAX];
		assert(menu_ap_copy_next_marker(&c, l2, sizeof(l2)) == MENU_AP_WCC_BEGIN);
		menu_ap_copy_marker_sent(&c, MENU_AP_WCC_BEGIN);
		assert(menu_ap_copy_next_marker(&c, l2, sizeof(l2)) == -1);
		/* escaping, an over-long name and too many names */
		static char longname[MENU_AP_COPY_CARD_NAME_MAX + 4];
		memset(longname, 'x', sizeof(longname) - 1);
		const char* names[MENU_AP_COPY_CARD_NAMES + 3]
			= {"a\"b\\c", "\x01\xe9", longname};
		for(int k = 3; k < MENU_AP_COPY_CARD_NAMES + 3; k++)
			names[k] = "w";
		menu_ap_copy_set_card(&c, true, names, MENU_AP_COPY_CARD_NAMES + 3);
		assert(!menu_ap_copy_card_wanted(&c) && c.card_truncated);
		assert(c.card_count == MENU_AP_COPY_CARD_NAMES);
		assert(menu_ap_copy_next_marker(&c, l2, sizeof(l2)) == MENU_AP_WCC_CARD);
		assert(strstr(l2, "\"truncated\":true,\"names\":[\"a\\\"b\\\\c\","
						  "\"\\u0001\\u00e9\""));
		assert(strlen(l2) < 2303);
		menu_ap_copy_marker_sent(&c, MENU_AP_WCC_CARD);
		assert(menu_ap_copy_next_marker(&c, l2, sizeof(l2)) == MENU_AP_WCC_END);
		/* worst case: 16 names of 95 bytes that all escape to \u00XX */
		static char wide[MENU_AP_COPY_CARD_NAME_MAX];
		memset(wide, 0x01, sizeof(wide) - 1);
		const char* many[MENU_AP_COPY_CARD_NAMES];
		for(int k = 0; k < MENU_AP_COPY_CARD_NAMES; k++)
			many[k] = wide;
		struct menu_ap_copy_state w;
		menu_ap_copy_reset(&w, MENU_AP_COPY_OK, FIXTURE);
		menu_ap_copy_set_card(&w, true, many, MENU_AP_COPY_CARD_NAMES);
		/* does not fit: sent empty + truncated, never malformed or held */
		assert(w.card_truncated && w.card_count == 0);
		assert(menu_ap_copy_format_marker(&w, MENU_AP_WCC_CARD, l2, sizeof(l2))
			   > 0);
	}
	puts("ok: names validated, markers fit, END waits for the card");
}

/* `test_world_copy_profile <dir>` additionally writes <dir>/<variant>.ndjson:
 * the marker lines THIS formatter produced on a clean model run, which is how
 * tests/fixtures/world_copy_convert/<variant>.ndjson (the collector's fixture
 * telemetry) were made -- so the Python verdict is graded on the walker's
 * real bytes, not on a hand-typed copy. */
static void dump_tapes(const char* dir) {
	static const enum model_variant mv[MENU_AP_COPY_VARIANT_COUNT]
		= {MV_OK, MV_LOW, MV_WRITE, MV_OK, MV_OK};
	for(int v = 0; v < MENU_AP_COPY_VARIANT_COUNT; v++) {
		struct model m;
		struct run r;
		model_init(&m, mv[v]);
		run_route(&r, &m, (enum menu_ap_copy_variant)v, true);
		char path[512];
		snprintf(path, sizeof(path), "%s/%s.ndjson", dir,
				 menu_ap_copy_variant_name((enum menu_ap_copy_variant)v));
		FILE* f = fopen(path, "wb");
		assert(f);
		fprintf(f, "{\"event\":\"hello\",\"profile\":\"world_copy_convert\"}\n");
		for(int k = 0; k < r.sent; k++)
			fprintf(f, "%s\n", r.lines[r.order[k]]);
		fclose(f);
	}
}

/* Review round 1 (Sol B1): the route runs only in the isolated per-run
 * worlds root; anything else is refused by name with zero presses, no
 * listing of that root and the three refusal markers. */
static void case_isolation(void) {
	const char* exp = "sd:/apps/CavEX/wcc_runs/wcc_ok_1";
	assert(menu_ap_copy_root_isolated(exp, exp));
	assert(menu_ap_copy_root_isolated("wcc_runs/r1", "wcc_runs/r1"));
	/* the setter failed: the player's saves root is still configured */
	assert(!menu_ap_copy_root_isolated("saves", exp));
	assert(!menu_ap_copy_root_isolated("sd:/apps/CavEX/saves", exp));
	assert(!menu_ap_copy_root_isolated(NULL, exp));
	assert(!menu_ap_copy_root_isolated(exp, NULL));
	assert(!menu_ap_copy_root_isolated("", ""));	  /* setter-failed sentinel */
	assert(!menu_ap_copy_root_isolated("saves", "saves")); /* not a run root */
	assert(!menu_ap_copy_root_isolated("x/wcc_runs/", "x/wcc_runs/"));
	assert(!menu_ap_copy_root_isolated("x/wcc_runs/../saves",
									   "x/wcc_runs/../saves"));
	assert(!menu_ap_copy_root_isolated("x/mywcc_runs/a", "x/mywcc_runs/a"));
	assert(!menu_ap_copy_root_isolated("sd:/apps/CavEX/wcc_runs/a",
									   "sd:/apps/CavEX/wcc_runs/b"));

	struct menu_ap_copy_state s;
	menu_ap_copy_refuse(&s, MENU_AP_COPY_OK, FIXTURE);
	assert(s.finished && !s.ok && s.step == MENU_AP_COPY_STEP_ISOLATION
		   && s.failed_step == MENU_AP_COPY_STEP_ISOLATION);
	assert(!menu_ap_copy_card_wanted(&s)); /* never lists the user root */
	struct model m;
	model_init(&m, MV_OK);
	for(int t = 0; t < 600; t++) {
		struct menu_ap_copy_output out;
		struct menu_ap_copy_obs o = model_obs(&m);
		assert(!menu_ap_copy_tick(&s, m.screen, o, 1.0F / 60.0F, &out));
		assert(!out.press && !out.interrupt);
	}
	static char line[MENU_AP_COPY_LINE_MAX];
	unsigned sent = 0;
	int k;
	static char card[MENU_AP_COPY_LINE_MAX], end[MENU_AP_COPY_LINE_MAX];
	while((k = menu_ap_copy_next_marker(&s, line, sizeof(line))) >= 0) {
		sent |= 1u << k;
		if(k == MENU_AP_WCC_CARD)
			snprintf(card, sizeof(card), "%s", line);
		if(k == MENU_AP_WCC_END)
			snprintf(end, sizeof(end), "%s", line);
		menu_ap_copy_marker_sent(&s, k);
	}
	assert(sent == ((1u << MENU_AP_WCC_BEGIN) | (1u << MENU_AP_WCC_CARD)
					| (1u << MENU_AP_WCC_END)));
	assert(strcmp(card, "{\"event\":\"wcc_card\",\"root_ok\":false,"
						"\"truncated\":false,\"names\":[]}")
		   == 0);
	char want[96];
	snprintf(want, sizeof(want), "\"ok\":false,\"step\":%d,\"failed_step\":%d",
			 MENU_AP_COPY_STEP_ISOLATION, MENU_AP_COPY_STEP_ISOLATION);
	assert(strstr(end, want));
	puts("ok: a non-isolated worlds root is refused: zero presses, no listing");
}

int main(int argc, char** argv) {
	if(argc > 1)
		dump_tapes(argv[1]);
	case_isolation();
	case_ok();
	case_fixture_last_row_from_play_focus();
	case_variant(MENU_AP_COPY_LOW_SPACE, MV_LOW, MENU_AP_WC_LOW_SPACE, false,
				 false);
	case_variant(MENU_AP_COPY_WRITE_ERROR, MV_WRITE, MENU_AP_WC_IO_ERROR, true,
				 true);
	/* hw1028b: cleanup inside the failing step -- never shown, still proven */
	case_variant(MENU_AP_COPY_WRITE_ERROR, MV_WRITE_FAST, MENU_AP_WC_IO_ERROR,
				 false, true);
	case_interrupt();
	case_recover();
	case_unobserved();
	case_no_fixture(true);
	case_no_fixture(false);
	case_names();
	puts("test_world_copy_profile: ALL PASS");
	return 0;
}
