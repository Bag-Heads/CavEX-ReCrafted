/* mp_chat_login_3m phase driver. GPL-3.0-or-later.
 *
 * Contract: docs/chat_login_profile.md. The cases below drive
 * source/game/chat_login_script.c through the PRODUCTION handlers it exists to
 * exercise -- input_poll -> input_binding_levels -> input_owner_latch (SEAM S1,
 * source/platform/input.c) and screen_chat_update -> osk_chat_update_owned --
 * with no reimplementation of either. The route, the repeat cadence, the
 * neutral re-arm and the draft are production's own answers, read back through
 * screen_chat_observe.
 *
 * WHAT THIS TEST DOES NOT PROVE, stated rather than implied:
 *   - main.c's seam. `rig_frame` below replicates the three thin call sites
 *     main.c uses (the screen_ingame opener, player 0's screen update, the
 *     per-frame chat_login_frame before input_poll). If those hunks drift this
 *     test still passes; the real proof of the seam is the hardware profile
 *     and the PC app run the brief requires.
 *   - the server. chat_lines_sent and the fixture's reply are scripted here;
 *     what a real mc173/Beta server does with the submission is what
 *     mp_chat_login_3m measures on a Wii.
 *   - the Wii chord gate. input_chat_open_edge's D-pad Up+Left/Right refusal
 *     is compiled for PLATFORM_WII only, so on this PC build the chord leg
 *     records refused:false (CHAT_LOGIN_CHORD_GATED). The gated answer is
 *     proved on hardware, not here.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* the trace golden is found by walking up from the CWD */

#include "config.h"
#include "lodepng/lodepng.h"
#include "entity/entity.h"
#include "game/chat_login_script.h"
#include "game/chat_ring.h"
#include "game/game_state.h"
#include "game/gui/chat_composer.h"
#include "game/gui/chat_composer_model.h"
#include "game/gui/screen.h"
#include "game/gui/widget_osk.h"
#include "game/hw_run.h"
#include "item/inventory.h"
#include "item/window_container.h"
#include "platform/input.h"
#include "platform/input_owner.h"
#include "platform/input_glyph.h"
#include "game/gui/menu_header.h"
#include "platform/menu_sound.h"
#include "platform/time.h"
#include "platform/texture.h"

void __wrap_audio_menu_event(enum menu_sound_event event, ptime_t now) {
	(void)event;
	(void)now;
}
/* The composer's layout is resolved against the real raster; a headless test
 * has no window, and a 0x0 raster would silently take the COMPACT path
 * (osk_sign_update_owned) instead of the chat keyboard this profile drives. */
int __wrap_gfx_width(void) { return 640; }
int __wrap_gfx_height(void) { return 480; }

#define RIG_MAX_LINES 512
#define RIG_LINE 1024
#define RIG_FRAME_MS 50u   /* the 20 fps floor every driver tolerance assumes */
#define RIG_DT 0.05F
#define RIG_MAX_FRAMES 20000u

/* The binding table the run resolves its codes through. Deliberately a REAL
 * config file parsed by the real config layer: the whole point of SEAM S1 is
 * that the driver names ACTIONS and the config decides which physical code
 * carries them. `chat` shares gui_up's code exactly as config_wii.json shares
 * D-pad Up between input.chat and input.gui_up, so the "the opening press must
 * be released before the first Up can move focus" ordering rule is exercised
 * here and not only on a console. */
#define RIG_CONFIG_FILE "test_chat_login_bindings.json"
static const char* const rig_config_json =
	"{\"input\":{"
	"\"chat\":[265],\"gui_up\":[265],\"gui_down\":[264],"
	"\"gui_left\":[263],\"gui_right\":[262],"
	"\"gui_click\":[88],\"gui_click_alt\":[90],"
	"\"gui_page\":[70],\"gui_previous\":[71],"
	"\"gui_quick_move\":[84],\"inventory\":[69],\"open_menu\":[257]"
	"}}";

/* D-1: the SAME bindings plus the F3 chord config_wii.json gives the Wii
 * Remote -- hold gui_previous (Minus), press gui_page (Plus) -- in this build's
 * own code space: 71 * 1000 + 70, the HOLD*1000+PRESS encoding input.c
 * decodes (source/platform/input.c, input_combo_decode). Its own file, used by
 * ONE case, so every other case still runs against a config with no chord in
 * it and is byte-identical to before. */
#define RIG_CONFIG_CHORD_FILE "test_chat_login_chord_bindings.json"
static const char* const rig_config_chord_json =
	"{\"input\":{"
	"\"chat\":[265],\"gui_up\":[265],\"gui_down\":[264],"
	"\"gui_left\":[263],\"gui_right\":[262],"
	"\"gui_click\":[88],\"gui_click_alt\":[90],"
	"\"gui_page\":[70],\"gui_previous\":[71],"
	"\"gui_quick_move\":[84],\"inventory\":[69],\"open_menu\":[257],"
	"\"debug_overlay\":[71070]"
	"}}";

/* B4a: the same twelve chat roles PLUS the gameplay vocabulary a PLAY-mode
 * caller reaches for, on codes no chat role carries -- so a code leaking from
 * one set into the other is visible rather than aliased. `item_action_left`
 * also carries a CHORD encoding (hold gui_previous 71, press gui_page 70,
 * input.c's HOLD*1000+PRESS form) beside its physical code: the seam must
 * assert the physical 65 and never the composite, exactly as it already
 * refuses to assert a chord for a chat role. Its own file, used by the play
 * cases only, so every other case still runs against the untouched table. */
#define RIG_CONFIG_PLAY_FILE "test_chat_login_play_bindings.json"
static const char* const rig_config_play_json =
	"{\"input\":{"
	"\"chat\":[265],\"gui_up\":[265],\"gui_down\":[264],"
	"\"gui_left\":[263],\"gui_right\":[262],"
	"\"gui_click\":[88],\"gui_click_alt\":[90],"
	"\"gui_page\":[70],\"gui_previous\":[71],"
	"\"gui_quick_move\":[84],\"inventory\":[69],\"open_menu\":[257],"
	"\"item_action_left\":[65,71070],\"item_action_right\":[66],"
	"\"player_jump\":[32],\"open_inventory\":[67],\"scroll_right\":[68]"
	"}}";

static void rig_config_use(const char* json, const char* file) {
	FILE* f = fopen(file, "wb");
	assert(f && "the rig needs a writable CWD for its binding fixture");
	fputs(json, f);
	fclose(f);
	config_destroy(&gstate.config_user);
	assert(config_create(&gstate.config_user, file)
		   && "the binding fixture must parse");
}

/* ---- the shared-module TRACE RECORDER (Astra ruling 1, answer B3) --------
 *
 * B4a extracts this driver's login machine into a SHARED sub-machine a third
 * consumer (the mp_play_10m driver) calls. Astra's answer B3 binds that
 * extraction: "Preserve contract-2 markers, ordering, timing semantics and
 * input behavior ... add deterministic before/after trace equivalence for the
 * shared-module extraction. Do not claim live timestamps are byte-identical."
 *
 * So this recorder replays BOTH profiles that already ride the seam --
 * mp_chat_login_3m and mp_chat_chord_1m -- on a FIXED 16 ms frame clock and
 * writes, in order:
 *   `M <line>`  one per marker the driver emitted, normalised (below);
 *   `F <n> <connected> <code>...`  one per rendered frame, the virtual pad's
 *                                  request as chat_login_pad_for resolves it,
 *                                  or `F <n> -` on a frame where production
 *                                  does not reach the seam at all;
 *   `P`/`D`     the profile header and the profile_done verdict.
 * A frame's markers precede its own `F` row, because chat_login_frame emits
 * them and posts the request in the same call -- the row is the request that
 * call left behind.
 *
 * WHAT IS COMPARED: everything above byte for byte, EXCEPT the values of the
 * live-timing keys in rec_norm_keys[] and the `t`/`rem` columns of a trace
 * row, which are replaced by `*`. Those are the only readings Astra's "do not
 * claim live timestamps are byte-identical" covers; the marker set, the
 * marker ORDER, every seq, every non-timing field, the per-frame request
 * codes and the frame each marker landed on are all held byte-exact, which is
 * what "ordering, timing semantics and input behavior preserved" means here.
 * (Under the rig's fixed clock the timing values happen to be deterministic
 * too -- they are normalised on purpose anyway, so the golden can never be
 * read as a claim about a real console's clock.)
 *
 * Regenerate with CW_UPDATE_CHAT_LOGIN_TRACE=1 in the environment, exactly as
 * test_sort_presentation regenerates its draw-list goldens. */
#define REC_FRAME_MS 16u
#define REC_FRAME_DT 0.016F
#define REC_GOLDEN_DIR "tests/golden/chat_login"
#define REC_GOLDEN_FILE "shared_login_trace.txt"

static const char* const rec_norm_keys[] = {
	"\"ms\":",			   /* the marker's own driver clock          */
	"\"hold_ms\":",		   /* p1.hold_refused: the OBSERVED hold     */
	"\"first_repeat_ms\":", /* p2.hold_result                        */
	"\"mean_repeat_ms\":",	/* p2.hold_result                        */
	"\"release_lag_ms\":",	/* p2.hold_result                        */
	"\"max_frame_ms\":",	/* every leg's cadence base              */
	"\"max_frame_ms_run\":", /* run.ok's run-wide cadence base       */
	"\"remaining_ms\":",	/* p2.page_hold's pre-reload countdown   */
	"\"frames_blocked\":",	/* p3.rearmed                            */
	"\"wait_ms\":",			/* p6.ack                                */
	"\"first_t\":",			/* a trace batch's first row time        */
};

static struct {
	bool on;
	char* buf;
	size_t len, cap;
	unsigned frame;
} rec;

static void rec_put(const char* text) {
	size_t n = strlen(text);
	if(rec.len + n + 1 > rec.cap) {
		size_t cap = rec.cap ? rec.cap : (size_t)1 << 16;
		while(cap < rec.len + n + 1)
			cap *= 2;
		char* grown = realloc(rec.buf, cap);
		assert(grown && "the trace recorder ran out of memory");
		rec.buf = grown;
		rec.cap = cap;
	}
	memcpy(rec.buf + rec.len, text, n);
	rec.len += n;
	rec.buf[rec.len] = 0;
}

/* Copy `in` to `out`, replacing every live-timing VALUE with '*'. */
static void rec_normalise(const char* in, char* out, size_t cap) {
	size_t w = 0, r = 0;
	while(in[r] && w + 1 < cap) {
		/* A trace batch carries its rows inside one string:
		 * "t:req[^v]:cp:cd:rep:rem:r:c;..." -- columns 0 (t) and 5 (rem) are
		 * clock readings, the rest is the request/consumed/focus evidence the
		 * equivalence is actually about. */
		if(!strncmp(in + r, "\"frames\":\"", 10)) {
			for(unsigned k = 0; k < 10 && w + 1 < cap; k++)
				out[w++] = in[r++];
			unsigned column = 0;
			while(in[r] && in[r] != '"' && w + 1 < cap) {
				if(in[r] == ';') {
					column = 0;
					out[w++] = in[r++];
				} else if(in[r] == ':') {
					column++;
					out[w++] = in[r++];
				} else if(column == 0 || column == 5) {
					out[w++] = '*';
					while(in[r] && in[r] != ':' && in[r] != ';' && in[r] != '"')
						r++;
				} else
					out[w++] = in[r++];
			}
			continue;
		}
		bool normalised = false;
		for(unsigned k = 0;
			k < sizeof(rec_norm_keys) / sizeof(rec_norm_keys[0]); k++) {
			size_t n = strlen(rec_norm_keys[k]);
			if(strncmp(in + r, rec_norm_keys[k], n))
				continue;
			for(size_t j = 0; j < n && w + 1 < cap; j++)
				out[w++] = in[r++];
			if(w + 1 < cap)
				out[w++] = '*';
			while(in[r] == '-' || (in[r] >= '0' && in[r] <= '9'))
				r++;
			normalised = true;
			break;
		}
		if(!normalised)
			out[w++] = in[r++];
	}
	out[w] = 0;
}

static struct {
	uint32_t now_ms;
	uint32_t frames;
	struct entity body;

	/* The frame clock this run steps on. RIG_FRAME_MS/RIG_DT for every case
	 * that was here before; the trace recorder (case_trace_equivalence) drives
	 * its own fixed 16 ms clock so the golden it writes is a pure function of
	 * the driver, never of the host. Both are set by rig_reset, so no case can
	 * inherit another's clock. */
	uint32_t frame_ms;
	float frame_dt;

	/* knobs */
	bool opener_disabled;
	/* Review finding 2: a one-time clock HITCH injected between P1's hold
	 * request and the first poll that can observe it -- the load burst a real
	 * console takes. The leg must time its hold from the press it OBSERVED,
	 * never from the frame it asked for one. */
	unsigned hold_hitch_ms;
	bool hitch_done, hitch_wait;
	/* Frames to hold player 0 on screen_load_world before gameplay is
	 * admitted -- the real-Wii dwell (up to 30 s) that used to be charged to
	 * P1's budget because P0 completed on world_loaded alone. */
	unsigned hold_loading_frames;
	bool timeout_armed;
	enum chat_login_phase timeout_phase;
	bool nudge_focus;	 /* wrong-focus injection, once, at the route start */
	bool ack_before_send; /* stale-ack case: the line is already in the ring */
	bool ack_after_send;  /* the fixture reply the happy path waits for */
	bool ack_pushed;
	bool refuse_emit;
	unsigned refuse_emit_tries;
	bool refuse_run_ok; /* the sender never accepts run.ok (section 11 M3) */
	/* D-1: model a console whose config really binds the F3 chord, so the PC
	 * suite can reach the state the 09-19 hardware run reached (see
	 * case_chord_device below). The rig's own half is main.c's F3 hunk. */
	bool chord_device;
	unsigned overlay_toggles;
	/* C-1: starve the E-2 gate of its binding SCAN by stopping the polls the
	 * moment the pad is bound. cl_chord_scan rides input_poll, so this models
	 * the one remaining way the gate can have no reading at all -- a driver /
	 * ownership fault rather than a staging one -- and must produce its own
	 * distinct refusal instead of the "stage the developer config" message. */
	bool starve_scan;
	/* B4a 2f: main.c's own per-frame recovery answer, supplied directly.
	 * PRODUCTION raises the modal for real in P3 (the pad "goes away" there and
	 * screen_controller_recovery fires, which is what case_happy's P3 leg
	 * already exercises), but a PARKED driver holds the pad connected and
	 * exposes no lever that could make the gate fire on demand. What the parked
	 * rule reads is ctx->recovery_modal -- main.c's answer for this frame -- so
	 * that is exactly what this knob supplies, and it is OR-ed with the real
	 * gate rather than replacing it. */
	bool force_recovery_modal;

	/* observation */
	uint32_t lines_sent;
	char lines[RIG_MAX_LINES][RIG_LINE];
	unsigned line_count;
	char done[128];
	bool done_sent;
	bool player1_ever_owned;
} rig;

/* ---- hooks -------------------------------------------------------------- */

static bool rig_emit(void* user, const char* line) {
	(void)user;
	if(rig.refuse_emit_tries) {
		rig.refuse_emit_tries--;
		return false;
	}
	if(rig.refuse_run_ok && strstr(line, "\"phase\":\"run\"")
	   && strstr(line, "\"marker\":\"ok\""))
		return false;
	assert(rig.line_count < RIG_MAX_LINES && "marker flood");
	assert(strlen(line) < RIG_LINE);
	snprintf(rig.lines[rig.line_count++], RIG_LINE, "%s", line);
	if(rec.on) {
		/* Recorded only once the sender TOOK it: a refused line is retried
		 * verbatim with the same seq, and the golden must carry one row per
		 * marker, not one per attempt. */
		static char normalised[RIG_LINE * 2];
		rec_normalise(line, normalised, sizeof(normalised));
		rec_put("M ");
		rec_put(normalised);
		rec_put("\n");
	}
	return true;
}

static void rig_profile_done(void* user, const char* status) {
	(void)user;
	assert(!rig.done_sent && "profile_done must be sent exactly once");
	snprintf(rig.done, sizeof(rig.done), "%s", status ? status : "");
	rig.done_sent = true;
}

/* The remote provider's own counter, modelled by the composer's single
 * enqueue point (screen_chat.c's CAVEWII_TESTING seam counts the sends that
 * really reached svin_rpc_send). */
static bool rig_chat_lines_sent(void* user, uint32_t* out) {
	(void)user;
	if(!out)
		return false;
	*out = rig.lines_sent;
	return true;
}

static const struct chat_login_hooks rig_hooks = {
	.emit = rig_emit,
	.profile_done = rig_profile_done,
	.chat_lines_sent = rig_chat_lines_sent,
};

/* ---- marker helpers ----------------------------------------------------- */

static bool line_has(const char* line, const char* needle) {
	return strstr(line, needle) != NULL;
}

static int rig_find(const char* phase, const char* marker, unsigned from) {
	char p[32], m[48];
	snprintf(p, sizeof(p), "\"phase\":\"%s\"", phase);
	snprintf(m, sizeof(m), "\"marker\":\"%s\"", marker);
	for(unsigned k = from; k < rig.line_count; k++)
		if(line_has(rig.lines[k], p) && line_has(rig.lines[k], m))
			return (int)k;
	return -1;
}

static unsigned rig_count(const char* phase, const char* marker) {
	unsigned n = 0;
	int at = -1;
	while((at = rig_find(phase, marker, (unsigned)(at + 1))) >= 0)
		n++;
	return n;
}

static bool rig_marker_ok(int index) {
	return index >= 0 && line_has(rig.lines[index], "\"ok\":true");
}

/* Every emitted line must carry a gapless 1..N `seq`, in order. */
static void rig_assert_gapless(void) {
	for(unsigned k = 0; k < rig.line_count; k++) {
		char want[32];
		snprintf(want, sizeof(want), "\"seq\":%u,", k + 1u);
		assert(line_has(rig.lines[k], want) && "seq must be gapless and 1-based");
		assert(line_has(rig.lines[k], "\"event\":\"chat_login\""));
	}
}

/* ---- the rig ------------------------------------------------------------ */

static uint32_t rig_phase_budget(enum chat_login_phase phase) {
	switch(phase) {
		case CHAT_LOGIN_P0: return CHAT_LOGIN_P0_MS;
		case CHAT_LOGIN_P1: return CHAT_LOGIN_P1_MS;
		case CHAT_LOGIN_P2: return CHAT_LOGIN_P2_MS;
		case CHAT_LOGIN_P3: return CHAT_LOGIN_P3_MS;
		case CHAT_LOGIN_P4: return CHAT_LOGIN_P4_MS;
		case CHAT_LOGIN_P5: return CHAT_LOGIN_P5_MS;
		case CHAT_LOGIN_P6: return CHAT_LOGIN_P6_MS;
		default: return CHAT_LOGIN_TOTAL_MS;
	}
}

/* The chord leg now claims "and the hotbar did not move", so the rig needs a
 * REAL inventory window for the driver to read a slot index out of -- the same
 * windowc_get_latest / inventory_get_hotbar pair screen_ingame.c uses. Without
 * one the driver reports -1 ("not observable"), which is a legitimate reading
 * but proves nothing, so the rig gives it a window. */
static struct window_container rig_inventory;
static bool rig_inventory_live;

static void rig_reset(void) {
	memset(&rig, 0, sizeof(rig));
	rig.frame_ms = RIG_FRAME_MS;
	rig.frame_dt = RIG_DT;
	if(rig_inventory_live)
		windowc_destroy(&rig_inventory);
	rig_inventory_live = windowc_create(&rig_inventory, WINDOW_TYPE_INVENTORY,
										INVENTORY_SIZE);
	assert(rig_inventory_live);
	chat_login_reset();
	input_init();
	chat_controller_mode_set(false);
	screen_chat_test_enqueue_reset();
	screen_chat_test_refuse_sends(0);
	memset(&gstate.players[0].chat, 0, sizeof(gstate.players[0].chat));
	chat_ring_reset(&gstate.chat_ring);
	gstate.remote_session = true;
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	gstate.world_load_failed = false;
	gstate.remote_terrain_hold = false;
	gstate.paused = false;
	gstate.players[0].local_player = &rig.body;
	gstate.players[0].windows[WINDOWC_INVENTORY] = &rig_inventory;
	inventory_set_hotbar(windowc_get_latest(&rig_inventory), 4);
	gstate.players[0].current_screen = &screen_ingame;
	rig.body.pos[0] = 8.5F;
	rig.body.pos[1] = 65.0F;
	rig.body.pos[2] = -3.25F;
}

/* The binding table the seam resolves ACTIONS through, rebuilt here from the
 * live config exactly as input.c's input_read_bindings does for every action
 * this rig configures. The recorder has to hand chat_login_pad_for the SAME
 * map input_players_poll hands it, or the codes it reads back would not be
 * production's; a drift between this table and the JSON above shows up as a
 * golden mismatch on the first frame the pad binds. Actions the rig does not
 * configure keep length 0 -- on PC input_read_bindings has no fallback for any
 * of them (its defaults cover gui_split/gui_drop/gui_page/gui_previous/
 * gui_quick_move/chat, and the rig names every one of those it uses). */
static const struct {
	enum input_button button;
	const char* key;
} rig_binding_keys[] = {
	{IB_CHAT, "input.chat"},
	{IB_GUI_UP, "input.gui_up"},
	{IB_GUI_DOWN, "input.gui_down"},
	{IB_GUI_LEFT, "input.gui_left"},
	{IB_GUI_RIGHT, "input.gui_right"},
	{IB_GUI_CLICK, "input.gui_click"},
	{IB_GUI_CLICK_ALT, "input.gui_click_alt"},
	{IB_GUI_PAGE, "input.gui_page"},
	{IB_GUI_PREVIOUS, "input.gui_previous"},
	{IB_GUI_QUICK_MOVE, "input.gui_quick_move"},
	{IB_INVENTORY, "input.inventory"},
	{IB_HOME, "input.open_menu"},
	{IB_DEBUG_OVERLAY, "input.debug_overlay"},
	/* the PLAY vocabulary (B4a 2c) -- bound only by rig_config_play_json */
	{IB_ACTION1, "input.item_action_left"},
	{IB_ACTION2, "input.item_action_right"},
	{IB_JUMP, "input.player_jump"},
	{IB_OPEN_INVENTORY, "input.open_inventory"},
	{IB_SCROLL_RIGHT, "input.scroll_right"},
};

static void rig_binding_map(int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION],
							size_t lengths[IB_COUNT]) {
	for(int b = 0; b < IB_COUNT; b++) {
		lengths[b] = 0;
		for(size_t j = 0; j < INPUT_BINDINGS_PER_ACTION; j++)
			map[b][j] = -1;
	}
	for(unsigned k = 0; k < sizeof(rig_binding_keys) / sizeof(rig_binding_keys[0]);
		k++) {
		size_t n = INPUT_BINDINGS_PER_ACTION;
		if(config_read_int_array(&gstate.config_user, rig_binding_keys[k].key,
								 map[rig_binding_keys[k].button], &n))
			lengths[rig_binding_keys[k].button] = n;
	}
}

/* One trace row: the request chat_login_frame just posted, as the seam's own
 * output codes. Asked for ONLY while player 0 is owned, because that is the
 * only state in which input_players_poll reaches chat_login_pad_for -- an
 * earlier call would set the driver's binding-scan latch a frame before
 * production does, and the recorder must not move the machine it records. */
static void rec_frame_row(void) {
	char part[64];
	struct input_device device;
	snprintf(part, sizeof(part), "F %u", rec.frame++);
	rec_put(part);
	if(input_player_device(0, &device)) {
		int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION];
		size_t lengths[IB_COUNT];
		struct chat_login_pad pad;
		rig_binding_map(map, lengths);
		if(chat_login_pad_for(0, map, lengths, &pad)) {
			snprintf(part, sizeof(part), " %d", pad.connected ? 1 : 0);
			rec_put(part);
			for(unsigned k = 0; k < pad.count; k++) {
				snprintf(part, sizeof(part), " %d", pad.codes[k]);
				rec_put(part);
			}
		} else
			rec_put(" x");
	} else
		rec_put(" -");
	rec_put("\n");
}

/* main.c's thin call sites, in main.c's order: the screen_ingame opener, then
 * player 0's screen update, then the driver, then the poll that turns its
 * request into the next frame's owner frame. */
static void rig_frame(void) {
	/* main.c evaluates the PRODUCTION controller-recovery modal first and skips
	 * player 0's screen update while it owns the frame. P3's disconnect really
	 * raises it, so the rig has to honour the same gate or the leg would be
	 * tested against a main loop that does not exist. */
	if(rig.hold_loading_frames) {
		rig.hold_loading_frames--;
		gstate.players[0].current_screen = &screen_load_world;
		if(!rig.hold_loading_frames)
			gstate.players[0].current_screen = &screen_ingame;
	}
	bool recovery_modal = screen_controller_recovery() || rig.force_recovery_modal;
	if(recovery_modal) {
		/* the composer is suspended; the driver still ticks */
	} else if(gstate.players[0].current_screen == &screen_chat) {
		if(rig.nudge_focus && rig_find("p4", "origin", 0) >= 0) {
			/* Wrong-focus injection: nudge the cursor one cell out from under
			 * production BEFORE the update, so the next route step lands off
			 * target and p4.key has to say so. */
			rig.nudge_focus = false;
			gstate.players[0].chat.osk.col
				= (gstate.players[0].chat.osk.col + 1) % 10;
		}
		screen_chat.update(&screen_chat, rig.frame_dt);
	} else if(!rig.opener_disabled && gstate.remote_session
			  && input_chat_open_edge()) {
		screen_set(&screen_chat);
	}
	if(rig.hold_hitch_ms && !rig.hitch_done
	   && rig_find("p1", "pad_bound", 0) >= 0) {
		/* P1 asks for the hold on the frame AFTER pad_bound, so the stall has
		 * to land one frame later still: between that request and the first
		 * poll that could deliver the press. That is exactly the load burst
		 * finding 2 describes. */
		if(rig.hitch_wait) {
			rig.hitch_done = true;
			rig.now_ms += rig.hold_hitch_ms;
		} else
			rig.hitch_wait = true;
	}
	rig.lines_sent = screen_chat_test_enqueue_count();
	if(rig.ack_after_send && !rig.ack_pushed
	   && chat_login_phase_now() == CHAT_LOGIN_P6) {
		rig.ack_pushed = true;
		chat_ring_push(&gstate.chat_ring, CHAT_LOGIN_ACK_LINE);
	}
	chat_login_frame(&(struct chat_login_ctx) {
		.remote_session = gstate.remote_session,
		.world_loaded = gstate.world_loaded,
		.world_switch_pending = gstate.world_switch_pending,
		.local_player = gstate.players[0].local_player != NULL,
		.screen_is_ingame = gstate.players[0].current_screen == &screen_ingame,
		.screen_is_chat = gstate.players[0].current_screen == &screen_chat,
		.recovery_modal = recovery_modal,
		.screen_id = gstate.players[0].current_screen == &screen_chat
			? CHAT_LOGIN_SCREEN_CHAT
			: gstate.players[0].current_screen == &screen_ingame
				? CHAT_LOGIN_SCREEN_INGAME
				: CHAT_LOGIN_SCREEN_LOAD_WORLD,
		.x = rig.body.pos[0],
		.y = rig.body.pos[1],
		.z = rig.body.pos[2],
		.epoch = 7u,
		.now_ms = rig.now_ms,
		.ring = &gstate.chat_ring});
	if(rec.on)
		rec_frame_row();
	/* main.c's F3 hunk (source/main.c:4539-4542), which this rig is standing in
	 * for. Only the modelled-device case needs it -- nothing else in this file
	 * binds a chord, so the edge is never there to apply -- and it is driven by
	 * the SAME answer the driver used this frame, so the two can never disagree
	 * about whether a chord fired. */
	if(rig.chord_device && chat_login_test_chord_edge()) {
		if(gstate.force_debug_overlay)
			gstate.debug_overlay = true;
		else
			gstate.debug_overlay = !gstate.debug_overlay;
		rig.overlay_toggles++;
	}
	/* The poll is where cl_chord_scan reads the live binding table (it is
	 * called from input_players_poll, which only reaches player 0 once player
	 * 0 is OWNED -- i.e. never before P1's bind, which is C-1). Stopping it
	 * from the bind frame onward is therefore the only honest way to model
	 * "the gate never got a reading". */
	if(!(rig.starve_scan && chat_login_phase_now() == CHAT_LOGIN_P1
		 && chat_login_step_now() >= 2))
		input_poll();
	if(input_player_frame(1))
		rig.player1_ever_owned = true;
	rig.now_ms += rig.frame_ms;
	if(rig.timeout_armed && chat_login_phase_now() == rig.timeout_phase)
		rig.now_ms += rig_phase_budget(rig.timeout_phase);
	rig.frames++;
}

static void rig_run(void) {
	while(!rig.done_sent && rig.frames < RIG_MAX_FRAMES)
		rig_frame();
	assert(rig.done_sent && "the driver must always report a verdict");
}

#define RIG_USERNAME "cwa1b2c3"
static void rig_arm(enum hw_run_profile profile) {
	struct hw_run_config cfg;
	hw_run_config_init(&cfg);
	hw_run_apply_profile(&cfg, profile);
	snprintf(cfg.mp_username, sizeof(cfg.mp_username), "%s", RIG_USERNAME);
	cfg.enabled = true;
	chat_login_arm(&cfg, &rig_hooks);
}

/* ---- 1. happy path ------------------------------------------------------ */

static void case_happy(void) {
	rig_reset();
	rig.ack_after_send = true;
	rig_arm(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M);
	assert(chat_login_armed());
	rig_run();

	assert(!strcmp(rig.done, "ok") && rig.done);
	assert(chat_login_phase_now() == CHAT_LOGIN_OK);
	rig_assert_gapless();

	/* The contract's marker set, in the contract's phase order. */
	static const char* const phases[] = {"p0", "p1", "p1", "p1", "p2", "p2",
										 "p2", "p3", "p3", "p4", "p4", "p5",
										 "p5", "p5", "p6", "run"};
	static const char* const markers[] = {
		"session",	  "pad_bound",		 "hold_refused",  "chat_open",
		"hold_result", "tap_result",	 "page_hold",	  "frozen",
		"rearmed",	  "origin",			 "draft",		  "sent_requested",
		"composer_cleared", "lines_sent", "ack",		  "ok"};
	unsigned at = 0;
	for(unsigned k = 0; k < sizeof(phases) / sizeof(phases[0]); k++) {
		int found = rig_find(phases[k], markers[k], at);
		assert(found >= 0 && "a contract marker is missing");
		assert(rig_marker_ok(found) && "a contract marker reported ok:false");
		at = (unsigned)found;
	}
	/* run.ok is ALWAYS the last line. */
	assert(rig_find("run", "ok", 0) == (int)rig.line_count - 1);
	/* No failure anywhere. */
	for(unsigned k = 0; k < rig.line_count; k++)
		assert(!line_has(rig.lines[k], "\"ok\":false"));

	/* One p4.key per REQUESTED press: 37 directional + 1 GUI_PAGE + 15 clicks
	 * (contract section 9 ruling 1), and at least one per-frame trace batch. */
	assert(rig_count("p4", "key") == 53);
	assert(rig_count("p2", "trace") >= 1);
	assert(rig_count("p3", "trace") >= 1);
	/* Section 12: every batch names its leg, because `t` restarts at each
	 * leg's first press and the collector segments the rows by this field. */
	{
		bool seen_hold = false, seen_tap = false, seen_page = false,
			 seen_reconnect = false;
		int at2 = -1;
		while((at2 = rig_find("p2", "trace", (unsigned)(at2 + 1))) >= 0) {
			assert(line_has(rig.lines[at2], "\"leg\":\""));
			seen_hold |= line_has(rig.lines[at2], "\"leg\":\"hold\"");
			seen_tap |= line_has(rig.lines[at2], "\"leg\":\"tap\"");
			seen_page |= line_has(rig.lines[at2], "\"leg\":\"page\"");
		}
		at2 = -1;
		while((at2 = rig_find("p3", "trace", (unsigned)(at2 + 1))) >= 0)
			seen_reconnect |= line_has(rig.lines[at2], "\"leg\":\"reconnect\"");
		assert(seen_hold && seen_tap && seen_page && seen_reconnect
			   && "all four legs must name themselves");
	}

	/* The draft is the contract command, and it was submitted exactly once. */
	int draft = rig_find("p4", "draft", 0);
	assert(line_has(rig.lines[draft], "\"text\":\"/login hunter22\""));
	assert(line_has(rig.lines[draft], "\"len\":15"));
	assert(screen_chat_test_enqueue_count() == 1);
	int sent = rig_find("p5", "lines_sent", 0);
	assert(line_has(rig.lines[sent], "\"before\":0,\"after\":1"));

	/* The run nonce and the platform are echoed, and the ring baseline travels
	 * with the request (contract section 9 rulings 3/7, section 10 ruling 2). */
	assert(line_has(rig.lines[rig_find("p0", "session", 0)],
					"\"username\":\"" RIG_USERNAME "\""));
	assert(line_has(rig.lines[rig_find("p0", "session", 0)],
					"\"platform\":\"pc\""));
	/* The inventory seam's own observation: no recovery modal at chat_open. */
	assert(line_has(rig.lines[rig_find("p1", "chat_open", 0)],
					"\"recovery_modal\":false"));
	assert(line_has(rig.lines[rig_find("p5", "sent_requested", 0)],
					"\"ring_head\":0,\"ring_count\":0"));
	/* P1 really opened the composer through the owner frame, and P3 really
	 * froze a disconnected pad. */
	assert(line_has(rig.lines[rig_find("p1", "chat_open", 0)], "\"draft_len\":0"));
	assert(line_has(rig.lines[rig_find("p3", "frozen", 0)],
					"\"moves_while_down\":0"));
	/* Section 11: the chord leg separates "the press arrived" from "the screen
	 * changed" (M4); pad_bound reports the OBSERVED binding (L1); the re-arm
	 * verdict comes from a consumed-neutral trace row (L3); every trace leg
	 * carries its own cadence base and run.ok the run-wide one (H1). */
	assert(line_has(rig.lines[rig_find("p1", "hold_refused", 0)],
					"\"chat_pressed\":true"));
	assert(line_has(rig.lines[rig_find("p1", "hold_refused", 0)],
					"\"screen_changed\":"));
	assert(line_has(rig.lines[rig_find("p1", "pad_bound", 0)], "\"kind\":")
		   && line_has(rig.lines[rig_find("p1", "pad_bound", 0)],
					   "\"channel\":0"));
	assert(line_has(rig.lines[rig_find("p3", "rearmed", 0)],
					"\"neutral_before_step\":true"));
	assert(line_has(rig.lines[rig_find("p2", "hold_result", 0)], "\"max_frame_ms\":")
		   && line_has(rig.lines[rig_find("p2", "tap_result", 0)], "\"max_frame_ms\":")
		   && line_has(rig.lines[rig_find("p2", "page_hold", 0)], "\"max_frame_ms\":")
		   && line_has(rig.lines[rig_find("p3", "rearmed", 0)], "\"max_frame_ms\":"));
	assert(line_has(rig.lines[rig.line_count - 1], "\"max_frame_ms_run\":"));
	/* H2: the trace publishes PRODUCTION's pre-reload countdown, so a stepping
	 * frame really does report <= 0 and no row ever steps while it is > 0. */
	{
		int trace = rig_find("p2", "trace", 0);
		assert(trace >= 0);
		const char* frames = strstr(rig.lines[trace], "\"frames\":\"");
		assert(frames);
		unsigned rows = 0, stepping = 0;
		for(const char* p = frames + 10; *p && *p != '"';) {
			unsigned t, req, cp, cd;
			int rep, rem, r, c;
			char edge[4] = {0};
			/* "t:req[^v]:cp:cd:rep:rem:r:c" -- req carries the edge marker. */
			if(sscanf(p, "%u:%x%1[^v:]:%x:%x:%d:%d:%d:%d", &t, &req, edge, &cp,
					  &cd, &rep, &rem, &r, &c)
				   != 9
			   && sscanf(p, "%u:%x:%x:%x:%d:%d:%d:%d", &t, &req, &cp, &cd, &rep,
						 &rem, &r, &c)
				   != 8)
				break;
			rows++;
			if(rep) {
				stepping++;
				assert(rem <= 0 && "a stepping frame saw an expired countdown");
			}
			const char* next = strchr(p, ';');
			if(!next)
				break;
			p = next + 1;
		}
		assert(rows >= 8 && stepping >= 1
			   && "the trace must carry real per-frame rows");
	}
	/* Owner isolation: the driver never seeds a second player. */
	assert(!rig.player1_ever_owned);
	/* The virtual pad is withdrawn on the OK path. */
	assert(input_player_frame(0) == NULL);
	assert(!chat_controller_mode_requested());
	printf("  happy path: %u markers, %u frames\n", rig.line_count, rig.frames);
}

/* ---- 2. one timeout per phase ------------------------------------------- */

/* `want_diag` is "phase timeout" for the phases that simply never see their
 * observation. For the two TRACE phases it is "frame stall in trace leg"
 * instead, and that is the point: a harness clock jump inside a timed leg IS a
 * frame stall, and section 11 H1 rules such a leg INCONCLUSIVE rather than
 * letting it be scored. So these two cases cover both the phase budget and the
 * per-leg stall bound. */
static void case_timeout(enum chat_login_phase phase, const char* want_phase,
						 const char* want_marker, const char* want_diag) {
	rig_reset();
	rig.ack_after_send = true;
	rig.timeout_armed = true;
	rig.timeout_phase = phase;
	if(phase == CHAT_LOGIN_P0)
		gstate.world_loaded = false; /* the session observation never arrives */
	rig_arm(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M);
	rig_run();

	char want[96], diag[96];
	snprintf(want, sizeof(want), "%s:%s ", want_phase, want_diag);
	snprintf(diag, sizeof(diag), "\"diag\":\"%s ", want_diag);
	if(strncmp(rig.done, want, strlen(want))) {
		fprintf(stderr, "want prefix '%s' got '%s'\n", want, rig.done);
		assert(0 && "profile_done must name the phase AND carry the state");
	}
	int at = rig_find(want_phase, want_marker, 0);
	assert(at >= 0 && "the failure publishes on the marker the phase awaited");
	assert(!rig_marker_ok(at));
	assert(line_has(rig.lines[at], diag));
	/* Every timeout explains itself: screen, step, and the three masks (the
	 * first real-Wii timeout said only "phase timeout" and told us nothing). */
	assert(line_has(rig.lines[at], "scr=") && line_has(rig.lines[at], "st=")
		   && line_has(rig.lines[at], "req=") && line_has(rig.lines[at], "cp=")
		   && line_has(rig.lines[at], "cd=") && line_has(rig.lines[at], "own=")
		   && line_has(rig.lines[at], "chat="));
	rig_assert_gapless();
	/* Every exit path withdraws the pad. */
	assert(input_player_frame(0) == NULL);
	printf("  timeout %s.%s: %s\n", want_phase, want_marker, want_diag);
}

/* Section 11 M3: profile_done EXACTLY ONCE, ALWAYS. A run that reached OK but
 * whose run.ok the sender keeps refusing must still report -- and must still
 * withdraw the pad -- rather than idling out its window in silence. */
static void case_run_ok_blocked(void) {
	rig_reset();
	rig.ack_after_send = true;
	rig.refuse_run_ok = true;
	rig_arm(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M);
	rig_run(); /* rig_profile_done asserts it is called at most once */

	assert(!strcmp(rig.done, "run:marker emit blocked"));
	assert(rig_find("p6", "ack", 0) >= 0 && "the ack DID go out");
	assert(rig_find("run", "ok", 0) < 0 && "run.ok never reached the sender");
	assert(input_player_frame(0) == NULL && "every exit path unbinds");
	assert(!chat_controller_mode_requested());
	rig_assert_gapless();
	printf("  run.ok blocked: one verdict, pad withdrawn\n");
}

/* The real-Wii regression (run 1e42bf5_09.18): the client sits on
 * screen_load_world until the remote safe core is installed AND meshed, while
 * world_loaded has been true for seconds. P0 must NOT complete during that
 * dwell -- if it does, the wait is charged to P1's 15 s budget and the chord
 * leg, which needs screen_ingame to exist at all, times out. */
static void case_loading_screen_dwell(void) {
	rig_reset();
	rig.ack_after_send = true;
	rig.hold_loading_frames = 120; /* 6 s at the rig's 20 fps floor */
	rig_arm(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M);
	gstate.players[0].current_screen = &screen_load_world;
	while(!rig.done_sent && rig.frames < RIG_MAX_FRAMES
		  && rig_find("p0", "session", 0) < 0)
		rig_frame();
	int at = rig_find("p0", "session", 0);
	assert(at >= 0 && "p0.session must still arrive once gameplay is admitted");
	assert(rig.frames >= 120
		   && "p0 must NOT complete while the loading screen is up");
	assert(line_has(rig.lines[at], "\"screen\":1")
		   && "p0.session reports the screen it was admitted on");
	rig_run();
	assert(!strcmp(rig.done, "ok") && "the run still completes after the dwell");
	printf("  loading dwell: p0 waits %u frames, run still ok\n", rig.frames);
}

/* ---- 3. wrong focus ----------------------------------------------------- */

static void case_wrong_focus(void) {
	rig_reset();
	rig.nudge_focus = true;
	rig_arm(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M);
	rig_run();

	assert(!strcmp(rig.done, "p4:a route step landed off target"));
	int at = rig_find("p4", "key", 0);
	assert(at >= 0);
	/* The FAILING p4.key is the last one, and it carries both want and got. */
	int last = -1, k = -1;
	while((k = rig_find("p4", "key", (unsigned)(k + 1))) >= 0)
		last = k;
	assert(last >= 0 && !rig_marker_ok(last));
	assert(line_has(rig.lines[last], "\"want_row\":")
		   && line_has(rig.lines[last], "\"want_col\":")
		   && line_has(rig.lines[last], "\"row\":")
		   && line_has(rig.lines[last], "\"col\":"));
	rig_assert_gapless();
	printf("  wrong focus: p4.key names want and got\n");
}

/* ---- 4. refused send ---------------------------------------------------- */

static void case_refused_send(void) {
	rig_reset();
	screen_chat_test_refuse_sends(4);
	rig_arm(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M);
	rig_run();

	assert(!strcmp(rig.done,
				   "p5:the submission was refused: no chat line was queued"));
	int at = rig_find("p5", "lines_sent", 0);
	assert(at >= 0 && !rig_marker_ok(at));
	assert(line_has(rig.lines[at], "\"before\":0,\"after\":0"));
	/* SPEC "Error/refused enqueue": the draft and the screen are RETAINED and
	 * Retry is armed; nothing reached the transport. */
	assert(screen_chat_test_enqueue_count() == 0);
	assert(gstate.players[0].chat.retry_armed);
	assert(!strcmp(gstate.players[0].chat.buf, CHAT_LOGIN_COMMAND));
	assert(gstate.players[0].current_screen == &screen_chat);
	rig_assert_gapless();
	printf("  refused send: draft retained, retry armed, no line queued\n");
}

/* ---- 5. stale ack ------------------------------------------------------- */

static void case_stale_ack(void) {
	rig_reset();
	rig.ack_before_send = true;
	rig.timeout_armed = true;
	rig.timeout_phase = CHAT_LOGIN_P6;
	chat_ring_push(&gstate.chat_ring, CHAT_LOGIN_ACK_LINE);
	chat_ring_push(&gstate.chat_ring, "someone: hello");
	rig_arm(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M);
	rig_run();

	assert(!strncmp(rig.done, "p6:phase timeout ", 17));
	int at = rig_find("p6", "ack", 0);
	assert(at >= 0 && !rig_marker_ok(at));
	rig_assert_gapless();
	printf("  stale ack: a line already in the ring is not the reply\n");
}

/* ---- 6/7. inertness ----------------------------------------------------- */

/* The mandatory disarmed-identity proof for input_native_key_down: with the
 * driver disarmed (and with it armed for another profile) every key code must
 * answer exactly what the hardware read answers. */
#define RIG_KEY_SWEEP 1100
/* input.c's platform-native level read. It has no header declaration (its only
 * production callers live inside input.c), but it IS seam site 2, so the proof
 * that it is inert while disarmed has to call the real symbol. */
bool input_native_key_down(int key);
static void sweep_native(bool* out) {
	for(int k = 0; k < RIG_KEY_SWEEP; k++)
		out[k] = input_native_key_down(k);
}

/* One owner-frame snapshot per local player PLUS the whole published device
 * inventory, for the byte-identical proof. The inventory is in here because
 * SEAM S1's third site feeds it (contract section 10 ruling 1): "inventory
 * published state byte-identical with and without the driver present but
 * disarmed". */
struct rig_frames {
	struct input_player_frame f[INPUT_LOCAL_PLAYERS];
	struct input_identity published[INPUT_DEVICE_COUNT];
	bool armed[INPUT_DEVICE_COUNT];
};

static void sweep_frames(struct rig_frames* out, unsigned polls) {
	for(unsigned n = 0; n < polls; n++) {
		input_poll();
		for(unsigned p = 0; p < INPUT_LOCAL_PLAYERS; p++) {
			const struct input_player_frame* f = input_player_frame(p);
			out[n].f[p] = f ? *f : (struct input_player_frame) {0};
		}
		const struct input_inventory* inv = input_devices();
		for(unsigned d = 0; d < INPUT_DEVICE_COUNT; d++) {
			out[n].published[d] = inv->devices[d].published;
			out[n].armed[d] = inv->devices[d].armed;
		}
	}
}

static void case_disabled_profile(void) {
	static bool base[RIG_KEY_SWEEP], armed_other[RIG_KEY_SWEEP],
		after_reset[RIG_KEY_SWEEP];
	static struct rig_frames base_frames[8], other_frames[8], chat_frames[8];

	rig_reset();
	/* Baseline: nothing armed at all. */
	assert(input_player_bind(0, (struct input_device) {INPUT_DEVICE_KEYBOARD, 0}));
	sweep_native(base);
	sweep_frames(base_frames, 8);
	input_player_unbind(0);

	/* Armed for a profile that is NOT mp_chat_login_3m. */
	rig_reset();
	rig_arm(HW_RUN_PROFILE_FAR_IDLE_2M);
	assert(!chat_login_armed() && "only mp_chat_login_3m arms the driver");
	assert(input_player_bind(0, (struct input_device) {INPUT_DEVICE_KEYBOARD, 0}));
	sweep_native(armed_other);
	sweep_frames(other_frames, 8);
	input_player_unbind(0);
	assert(!memcmp(base, armed_other, sizeof(base))
		   && "input_native_key_down must be byte-identical while disarmed");
	assert(!memcmp(base_frames, other_frames, sizeof(base_frames))
		   && "owner frames must be byte-identical while disarmed");

	/* Armed for mp_chat_login_3m but not yet BOUND: arming alone is inert. */
	rig_reset();
	rig_arm(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M);
	assert(chat_login_armed());
	assert(input_player_bind(0, (struct input_device) {INPUT_DEVICE_KEYBOARD, 0}));
	sweep_native(after_reset);
	sweep_frames(chat_frames, 8);
	assert(!memcmp(base, after_reset, sizeof(base))
		   && "an armed-but-unbound driver must answer no native key");
	assert(!memcmp(base_frames, chat_frames, sizeof(base_frames))
		   && "an armed-but-unbound driver must not seed an owner frame");
	input_player_unbind(0);

	/* The seam itself refuses every player but 0, and refuses everything
	 * while disarmed. */
	int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION];
	size_t lengths[IB_COUNT];
	memset(map, 0, sizeof(map));
	for(int b = 0; b < IB_COUNT; b++)
		lengths[b] = 0;
	struct chat_login_pad pad;
	assert(!chat_login_pad_for(1, map, lengths, &pad));
	/* Site 3: the inventory substitution answers only the pad's OWN device,
	 * and nothing at all while disarmed. */
	struct input_identity id = {0};
	bool con = false, fresh = false, neu = false;
	assert(!chat_login_pad_inventory(
		(struct input_device) {INPUT_DEVICE_WPAD, 1}, &id, &con, &fresh, &neu));
	chat_login_reset();
	assert(!chat_login_pad_for(0, map, lengths, &pad));
	assert(!chat_login_pad_inventory((struct input_device) {INPUT_DEVICE_WPAD, 3},
									 &id, &con, &fresh, &neu));
	assert(!chat_login_pad_inventory(
		(struct input_device) {INPUT_DEVICE_KEYBOARD, 0}, &id, &con, &fresh,
		&neu));
	assert(!id.connected && !con && !fresh && !neu
		   && "a refused inventory query must not write its outputs");
	bool value = true;
	assert(!chat_login_pad_native_key(265, &value));

	/* Disabled profile: not one marker, not one bind. */
	rig_reset();
	rig_arm(HW_RUN_PROFILE_MP_CONNECT_SMOKE);
	for(unsigned k = 0; k < 400; k++)
		rig_frame();
	assert(rig.line_count == 0 && !rig.done_sent);
	assert(input_player_frame(0) == NULL);
	assert(gstate.players[0].current_screen == &screen_ingame);
	printf("  inertness: native keys, owner frames and markers unchanged\n");
}

/* ---- 8. owner isolation ------------------------------------------------- */

static void case_owner_isolation(void) {
	rig_reset();
	rig.ack_after_send = true;
	rig_arm(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M);
	/* Run to the middle of the typing phase. */
	while(!rig.done_sent && rig.frames < RIG_MAX_FRAMES
		  && chat_login_phase_now() != CHAT_LOGIN_P4)
		rig_frame();
	assert(chat_login_phase_now() == CHAT_LOGIN_P4);

	/* The seam can never seed player 1, whatever the binding table says. */
	int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION];
	size_t lengths[IB_COUNT];
	memset(map, 0, sizeof(map));
	for(int b = 0; b < IB_COUNT; b++)
		lengths[b] = 1;
	struct chat_login_pad pad;
	assert(!chat_login_pad_for(1, map, lengths, &pad));
	assert(chat_login_pad_for(0, map, lengths, &pad));
	assert(input_player_frame(1) == NULL);
	/* Site 3 is live for the pad's OWN device by the time the run is typing --
	 * it is enabled at P1 step 0, BEFORE the bind, so the inventory has carried
	 * the pad through its whole stability window before anything owns it (the
	 * real-hardware "recovery modal" failure). It still answers nothing else. */
	{
		struct input_identity id = {0};
		bool con = false, fresh = false, neu = false;
		assert(chat_login_pad_inventory(
			(struct input_device) {CHAT_LOGIN_PAD_DEVICE_KIND,
								   CHAT_LOGIN_PAD_CHANNEL},
			&id, &con, &fresh, &neu));
		assert(id.connected && id.category == INPUT_CAT_NUNCHUK && con && fresh
			   && neu);
		assert(!chat_login_pad_inventory(
			(struct input_device) {INPUT_DEVICE_GAMECUBE, 0}, &id, &con, &fresh,
			&neu));
	}

	/* A player-1 frame, however loud, never reaches the composer: the widget
	 * reads player 0's own frame only. Push every chat button on a manufactured
	 * frame and prove the draft and the focus do not move. */
	struct chat_composer* c = &gstate.players[0].chat;
	struct input_player_frame intruder = {.connected = true};
	for(int b = 0; b < IB_COUNT; b++)
		intruder.down[b] = intruder.pressed[b] = true;
	char before[CHAT_COMPOSER_CAP + 1];
	snprintf(before, sizeof(before), "%s", c->buf);
	int row = c->osk.row, col = c->osk.col, page = c->osk.page;
	struct osk_chat_result out;
	struct osk_state probe = c->osk;
	struct osk_chat_view view = c->kb;
	char probe_buf[CHAT_COMPOSER_CAP + 1];
	snprintf(probe_buf, sizeof(probe_buf), "%s", c->buf);
	probe.buf = probe_buf;
	/* A DISCONNECTED owner freezes edits and re-blocks every chat button
	 * (widget_osk.c osk_chat_input_tick). */
	struct input_player_frame lost = {.connected = false};
	osk_chat_input_tick(&view, &lost);
	osk_chat_update_owned(&probe, &view, RIG_DT, false, 0, 0, true, &lost,
						  false, &out);
	assert(!out.moved && !out.activated);
	assert(!strcmp(probe_buf, before));
	assert(probe.row == row && probe.col == col && probe.page == page);
	/* ... and on reconnect every button is blocked again until neutral. */
	struct input_player_frame back = {.connected = true};
	for(int b = 0; b < IB_COUNT; b++)
		back.down[b] = back.pressed[b] = true;
	osk_chat_input_tick(&view, &back);
	assert(view.rearm_mask == (1u << OSK_CHAT_ARM_COUNT) - 1u
		   && "a reconnect must re-block every chat button");
	osk_chat_update_owned(&probe, &view, RIG_DT, false, 0, 0, true, &back,
						  false, &out);
	assert(!out.moved && !out.activated && !strcmp(probe_buf, before));
	(void)intruder;

	/* The real run is unaffected by the probe and still completes. */
	rig_run();
	assert(!strcmp(rig.done, "ok"));
	printf("  owner isolation: player 1 and a lost pad cannot edit\n");
}

/* ---- main --------------------------------------------------------------- */

/* ---- mp_chat_chord_1m: the SEPARATE chord leg --------------------------
 * Astra ruling 2 D2. The device half of the contract
 * tests/test_chat_chord_evidence.py grades offline: this drives the REAL
 * driver through the REAL production handlers and reads the markers it
 * emitted. On PC no chord is bound (the rig config gives gui_page/gui_previous
 * one key each), so the leg legitimately records fired:false -- exactly the
 * platform split CHAT_CHORD_ON_DEVICE declares -- while the halves that are
 * asserted on BOTH platforms still are: a chord never opens the composer, the
 * opener still works after the neutral re-arm, and the run leaves the debug
 * overlay OFF. The console half is what a real mp_chat_chord_1m run buys. */
static void case_chord(void) {
	rig_reset();
	gstate.debug_overlay = true; /* a dirty console: P0 must force it off */
	gstate.force_debug_overlay = true;
	rig_arm(HW_RUN_PROFILE_MP_CHAT_CHORD_1M);
	assert(chat_login_armed());
	rig_run();

	assert(!strcmp(rig.done, "ok"));
	assert(chat_login_phase_now() == CHAT_LOGIN_OK);
	/* A different EVENT name from the login profile, on every line: that is
	 * what keeps a chord claim out of a login run and vice versa. */
	for(unsigned k = 0; k < rig.line_count; k++) {
		char want[32];
		snprintf(want, sizeof(want), "\"seq\":%u,", k + 1u);
		assert(line_has(rig.lines[k], want) && "seq must be gapless and 1-based");
		assert(line_has(rig.lines[k], "\"event\":\"chat_chord\""));
		assert(!line_has(rig.lines[k], "\"ok\":false"));
	}
	/* Contract 104: p1.chord_bound is the E-2 gate's own marker and it lands
	 * BEFORE p1.pad_bound -- the gate runs on the first frame after the bind,
	 * and the rest of P1 waits for it. */
	static const char* const phases[] = {"p0", "p1", "p1", "p2", "p2", "p3",
										 "p3", "p4", "run"};
	static const char* const markers[] = {"session", "chord_bound",
										  "pad_bound", "chord", "chord",
										  "opened", "closed",
										  "chord_restore", "ok"};
	unsigned at = 0;
	for(unsigned k = 0; k < sizeof(phases) / sizeof(phases[0]); k++) {
		int found = rig_find(phases[k], markers[k], at);
		assert(found >= 0 && "a chord-contract marker is missing");
		assert(rig_marker_ok(found));
		at = (unsigned)found + 1u;
	}
	assert(rig_count("p2", "chord") == 2);
	assert(rig_find("run", "ok", 0) == (int)rig.line_count - 1);
	/* P0 really forced the overlay off (and cleared the FORCE mode, which
	 * would otherwise suppress the very toggle the leg measures), and the run
	 * left it off. */
	assert(line_has(rig.lines[rig_find("p0", "session", 0)],
					"\"overlay\":false"));
	/* Review finding 4: the chord contract series is disjoint from the login
	 * one, so a PASS.json number identifies the profile that wrote it.
	 * 101 -> 102 (brief 07 A): 101 never produced a hardware run, but the
	 * MEANING of p2.chord order 0 changed -- it used to require a fire and now
	 * requires the absence of one -- so the number is spent rather than
	 * reused. 102 -> 103 (brief 12 D-1): 102 could never pass on hardware at
	 * all -- one legal toggle leaves the overlay ON -- so it is spent too, and
	 * the contract gained the p4.chord_restore leg. 103 -> 104 (hostile review
	 * 17, M-1): p2.chord gained a `fires` COUNT beside its bool, so the marker
	 * set changed; 103 never produced a hardware run either, and a number whose
	 * fields moved is spent rather than reused. */
	assert(line_has(rig.lines[rig_find("p0", "session", 0)],
					"\"contract\":104"));
	assert(!line_has(rig.lines[rig_find("p0", "session", 0)],
					 "\"contract\":102"));
	assert(!line_has(rig.lines[rig_find("p0", "session", 0)],
					 "\"contract\":103"));
	assert(!gstate.force_debug_overlay && !gstate.debug_overlay);
	/* No chord is bound here, so the ruled order fired NOTHING and the overlay
	 * was never on: p3.closed says so in both fields, and the restore leg
	 * honestly reports zero fires rather than inventing one. */
	assert(line_has(rig.lines[rig_find("p3", "closed", 0)],
					"\"overlay\":false,\"ruled_fired\":false"));
	assert(line_has(rig.lines[rig_find("p4", "chord_restore", 0)],
					"\"fired\":false,\"fires\":0"));
	assert(line_has(rig.lines[rig_find("p4", "chord_restore", 0)],
					"\"overlay\":false"));
	/* BOTH press orders, each once, and neither opened the composer.
	 *
	 * Brief 07 A -- the ORDER now decides the expected outcome, because F3 is
	 * specifically "hold Minus, THEN press Plus" (Astra ruling 3 D2):
	 *   order 0 = Plus half first  -> the overlay must NOT fire;
	 *   order 1 = Minus half first -> the overlay MUST fire (on the device;
	 *             the PC rig binds no chord, so it legitimately records
	 *             false there and the console half is what buys the claim).
	 * In BOTH orders chat must stay shut AND the hotbar must not move: Minus
	 * and Plus are the hotbar taps in the shipped scheme, so an overlap that
	 * leaked a slot step would be the regression. */
	int chord_a = rig_find("p2", "chord", 0);
	int chord_b = rig_find("p2", "chord", (unsigned)chord_a + 1u);
	assert(chord_a >= 0 && chord_b > chord_a);
	assert(line_has(rig.lines[chord_a], "\"order\":0"));
	assert(line_has(rig.lines[chord_b], "\"order\":1"));
	assert(line_has(rig.lines[chord_a], "\"chat_opened\":false"));
	assert(line_has(rig.lines[chord_b], "\"chat_opened\":false"));
	assert(line_has(rig.lines[chord_a], "\"fired\":false")
		   && "the WRONG press order reported a debug-overlay toggle");
	/* M-1 (hostile review 17): P2 COUNTS its fires the way P4 already does.
	 * A bool accepts three toggles -- which end the overlay ON again -- and
	 * the run would then be rejected four legs later at p3.closed with "the
	 * ruled chord fired but the debug overlay is off": a COUNTING defect
	 * wearing a product defect's diagnosis, the same wrong-leg failure that
	 * made contract 102 unpassable. Nothing is bound on PC, so both orders
	 * must report ZERO fires rather than merely "not fired". */
	assert(line_has(rig.lines[chord_a], "\"fired\":false,\"fires\":0"));
	assert(line_has(rig.lines[chord_b], "\"fired\":false,\"fires\":0"));
	/* The rig really handed the driver a hotbar to watch (slot 4, set by
	 * rig_reset), and neither chord moved it. A -1 here would mean the claim
	 * was never observable. */
	assert(line_has(rig.lines[chord_a], "\"slot_before\":4,\"slot_after\":4")
		   && "the wrong-order chord moved the hotbar");
	assert(line_has(rig.lines[chord_b], "\"slot_before\":4,\"slot_after\":4")
		   && "the ruled chord moved the hotbar");
	/* ... and the opener still opened the composer afterwards. */
	assert(line_has(rig.lines[rig_find("p3", "opened", 0)], "\"opened\":true"));
	/* The login profile is untouched by any of it: its own marker set still
	 * rides the chat_login event. */
	rig_reset();
	rig.ack_after_send = true;
	rig_arm(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M);
	rig_run();
	assert(!strcmp(rig.done, "ok"));
	assert(line_has(rig.lines[0], "\"event\":\"chat_login\""));
	puts("  chord: both press orders, no chat opened, the opener still opens "
		 "after the re-arm, the overlay left OFF -- on its own event name");
}

/* ---- D-1: the same leg on a device where the chord REALLY fires ----------
 *
 * The real-Wii run mp_chat_chord_1m_db01e56_0919_173900 FAILED with every
 * PRODUCT assertion holding: order 0 (Plus first) fired nothing, order 1 (hold
 * Minus, press Plus) fired once and turned the overlay ON, chat stayed shut and
 * the hotbar never moved -- and then the driver's own last check demanded the
 * overlay be OFF. It had been written when BOTH orders toggled (two toggles =
 * as found); with ONE legal toggle the console is left with F3 on.
 *
 * The case above cannot see that, because a PC build binds no chord and
 * therefore never fires: `fired` is false in both orders whatever the driver
 * does. So this case MODELS the console -- a config that really binds the F3
 * chord, the order gate supplied by production's own pure machine through the
 * chat_login_test_chord_model seam, and main.c's F3 hunk applied by rig_frame
 * -- and then asserts the thing the hardware run proved wrong: the profile
 * leaves the console's debug overlay exactly as it found it.
 *
 * WHAT IS MODELLED, stated rather than implied: only the WIRING a PC build
 * lacks. The chord decode, the owner-frame press edge, the phase sequence, the
 * markers, the hotbar readings and every assertion are production's own, and
 * the order gate is the same input_chord_order_step the Wii input.c calls. */
static void case_chord_device(void) {
	rig_config_use(rig_config_chord_json, RIG_CONFIG_CHORD_FILE);
	rig_reset();
	rig.chord_device = true;
	chat_login_test_chord_model(CHAT_LOGIN_CHORD_MODEL_BOUND);
	gstate.debug_overlay = true; /* a dirty console: P0 must still force it off */
	gstate.force_debug_overlay = true;
	rig_arm(HW_RUN_PROFILE_MP_CHAT_CHORD_1M);
	rig_run();

	assert(!strcmp(rig.done, "ok") && "the modelled device never completed");
	assert(chat_login_phase_now() == CHAT_LOGIN_OK);
	for(unsigned k = 0; k < rig.line_count; k++)
		assert(!line_has(rig.lines[k], "\"ok\":false"));
	/* This device DOES bind the chord, and the driver says so -- on the gate's
	 * own marker, after the bind (contract 104). Here that answer is the
	 * MODEL's short-circuit, not the scan's; case_chord_scan_carries_the_gate
	 * below is the leg that buys the scan. */
	assert(line_has(rig.lines[rig_find("p1", "chord_bound", 0)],
					"\"bound\":true"));
	assert(line_has(rig.lines[rig_find("p0", "session", 0)],
					"\"overlay\":false"));

	int chord_a = rig_find("p2", "chord", 0);
	int chord_b = rig_find("p2", "chord", (unsigned)chord_a + 1u);
	assert(chord_a >= 0 && chord_b > chord_a);
	/* The ORDER RULE, on a device that can actually answer it: the wrong press
	 * order toggled nothing, the ruled one toggled once. */
	assert(line_has(rig.lines[chord_a], "\"order\":0,\"fired\":false"));
	assert(line_has(rig.lines[chord_b], "\"order\":1,\"fired\":true"));
	/* M-1: and it is a COUNT, on the leg that can actually fire. Exactly one
	 * toggle for the ruled order, none for the wrong one -- a second fire here
	 * would leave the console's overlay as it was found and be mis-diagnosed
	 * at p3.closed instead of failing at p2 with its own name. */
	assert(line_has(rig.lines[chord_a], "\"fired\":false,\"fires\":0"));
	assert(line_has(rig.lines[chord_b], "\"fired\":true,\"fires\":1")
		   && "the ruled chord must fire EXACTLY once");
	assert(line_has(rig.lines[chord_a], "\"chat_opened\":false"));
	assert(line_has(rig.lines[chord_b], "\"chat_opened\":false"));
	assert(line_has(rig.lines[chord_a], "\"slot_before\":4,\"slot_after\":4"));
	assert(line_has(rig.lines[chord_b], "\"slot_before\":4,\"slot_after\":4"));
	/* ... so the console really is sitting there with F3 ON when the opener
	 * leg ends. THIS is the state contract 102 could never pass. */
	assert(line_has(rig.lines[rig_find("p2", "chord", (unsigned)chord_b)],
					"\"overlay\":true"));
	assert(line_has(rig.lines[rig_find("p3", "opened", 0)],
					"\"opened\":true,\"overlay\":true"));
	assert(line_has(rig.lines[rig_find("p3", "closed", 0)],
					"\"overlay\":true,\"ruled_fired\":true"));

	/* ... and the restore leg puts it back, with one toggle and nothing else:
	 * no composer, no hotbar step. */
	int restore = rig_find("p4", "chord_restore", 0);
	assert(restore >= 0 && "the run never restored the debug overlay");
	assert(line_has(rig.lines[restore], "\"fired\":true,\"fires\":1")
		   && "the restoring chord must fire EXACTLY once");
	assert(line_has(rig.lines[restore], "\"chat_opened\":false"));
	assert(line_has(rig.lines[restore], "\"slot_before\":4,\"slot_after\":4"));
	assert(line_has(rig.lines[restore], "\"overlay\":false"));
	assert(rig_find("run", "ok", 0) == (int)rig.line_count - 1);
	/* The console is as the run found it -- which is the whole claim -- and it
	 * took exactly the two legal toggles, no more. */
	assert(!gstate.debug_overlay && !gstate.force_debug_overlay);
	assert(rig.overlay_toggles == 2
		   && "exactly two legal toggles: the ruled chord and its restore");

	chat_login_test_chord_model(CHAT_LOGIN_CHORD_MODEL_OFF);
	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_CHORD_FILE);
	puts("  chord device: the ruled order toggled F3 ON once and the restore "
		 "chord put it back OFF -- 2 toggles, no chat, no hotbar step");
}

/* ---- review finding 2: the over-long-hold leg times the OBSERVED press ---
 * The gesture arms on the POLL that saw the press (input_tap_gesture.h), so a
 * driver that times its 800 ms from the frame it REQUESTED one is racing a
 * load burst: a 400 ms hitch used to leave ~450 ms of observed hold, inside
 * the 500 ms tap window -- a correct build would then open chat and be
 * reported FAIL. The leg now starts its clock at the first frame the owner
 * frame really showed IB_CHAT down, and publishes that duration as
 * p1.hold_refused.hold_ms so the collector can check it instead of trusting
 * the driver.
 *
 * Driven on PC with the opener gate disabled, which is how a PC run models
 * the Wii answer ("the composer did not open"): the run therefore ends FAIL on
 * the platform expectation (CHAT_LOGIN_CHORD_GATED is false on PC), and what
 * this case reads is the leg's own published evidence, which is emitted
 * before that verdict. */
/* chat_login_script.c's CHAT_LOGIN_OPENER_HOLD_MS, which is private to that
 * TU. Kept in step by name here and by the assertion below: if the driver
 * shortens its hold under the tap window, this case fails. */
#define RIG_OPENER_HOLD_MS 800u

static uint32_t rig_field_u32(const char* line, const char* key) {
	const char* at = strstr(line, key);
	if(!at)
		return 0xFFFFFFFFu; /* absent: never mistaken for a small hold */
	return (uint32_t)strtoul(at + strlen(key), NULL, 10);
}

static void case_hold_hitch(void) {
	rig_reset();
	rig.opener_disabled = true;
	rig.hold_hitch_ms = 400;
	rig_arm(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M);
	rig_run();

	int at = rig_find("p1", "hold_refused", 0);
	assert(at >= 0 && "the refusal leg never published its observation");
	assert(rig.hitch_done && "the hitch was never injected");
	/* The composer really stayed shut ... */
	assert(line_has(rig.lines[at], "\"screen_changed\":false"));
	assert(line_has(rig.lines[at], "\"chat_pressed\":true"));
	/* ... and the hold the leg MEASURED is the observed one, past the window,
	 * not the ~450 ms that timing from the step start would have left. */
	uint32_t held = rig_field_u32(rig.lines[at], "\"hold_ms\":");
	assert(held != 0xFFFFFFFFu && "p1.hold_refused must publish hold_ms");
	assert(held >= RIG_OPENER_HOLD_MS
		   && "the leg released the opener before its observed hold was over");
	printf("  hold hitch: a %u ms stall still left %u ms of OBSERVED hold "
		   "(>= %u), composer shut 1/1\n",
		   rig.hold_hitch_ms, held, (unsigned)RIG_OPENER_HOLD_MS);
}

/* ---- E-2: a console staged with the RELEASE config ----------------------
 *
 * Astra ergonomics 2 took the F3 chords out of config_wii.json, so "a Wii run
 * binds the chord" stopped being a property of the PLATFORM and became a
 * property of the CONFIG that was staged. A card carrying the release config
 * has nothing for this profile to measure, and the old code would have pressed
 * two buttons that form nothing through four legs and failed at p3 with "the
 * ruled order never reached IB_DEBUG_OVERLAY" -- which reads like a product
 * defect and is not one. It is refused BY NAME, naming the configuration to
 * stage.
 *
 * C-1 (hostile review 17, lead ruling): the refusal moved OFF p0. It reads the
 * live binding table, and cl_chord_scan can only see that table from inside an
 * input poll that reaches player 0 -- which cannot happen before P1 binds the
 * virtual pad, because input_players_poll skips unowned players. So the gate
 * now publishes p1.chord_bound on the first frame AFTER the bind has produced
 * a reading, still before any typing and long before P2 presses anything. This
 * case therefore asserts the refusal at its new stage, and that the run stops
 * there. */
static void case_chord_unbound_config(void) {
	rig_reset();
	chat_login_test_chord_model(CHAT_LOGIN_CHORD_MODEL_UNBOUND);
	rig_arm(HW_RUN_PROFILE_MP_CHAT_CHORD_1M);
	rig_run();
	chat_login_test_chord_model(CHAT_LOGIN_CHORD_MODEL_OFF);

	/* P0 passed -- it has nothing to say about the binding any more -- and the
	 * refusal is its own marker, after the bind, with the diag that says what
	 * to do about it. */
	int at = rig_find("p1", "chord_bound", 0);
	assert(at >= 0
		   && "the refusal must publish on p1.chord_bound, not silently");
	assert(line_has(rig.lines[at], "\"ok\":false"));
	assert(line_has(rig.lines[at], "no F3 chord is bound")
		   && "the refusal must say WHY it refused");
	assert(line_has(rig.lines[at], "config_wii_developer.json")
		   && "the refusal must name the configuration to stage");
	assert(rig_marker_ok(rig_find("p0", "session", 0))
		   && "p0 must no longer judge a binding it cannot have read");
	assert(!line_has(rig.lines[rig_find("p0", "session", 0)], "chord_bound")
		   && "p0.session must not claim a chord_bound it cannot know");
	/* It really did get as far as the bind, and no further. */
	assert(rig_find("p1", "pad_bound", 0) < 0
		   && "the gate must run BEFORE the rest of P1, not after it");
	assert(rig_find("p2", "chord", 0) < 0
		   && "the run pressed the chord anyway");
	assert(rig_find("p3", "closed", 0) < 0);
	assert(rig_find("p4", "chord_restore", 0) < 0);
	assert(strcmp(rig.done, "ok") != 0 && "a chordless run reported ok");

	/* ... and the SAME rig with a config that DOES bind the chord runs to
	 * completion, so the refusal is about the binding and nothing else. */
	rig_config_use(rig_config_chord_json, RIG_CONFIG_CHORD_FILE);
	rig_reset();
	rig.chord_device = true;
	chat_login_test_chord_model(CHAT_LOGIN_CHORD_MODEL_BOUND);
	rig_arm(HW_RUN_PROFILE_MP_CHAT_CHORD_1M);
	rig_run();
	chat_login_test_chord_model(CHAT_LOGIN_CHORD_MODEL_OFF);
	assert(!strcmp(rig.done, "ok") && "a bound chord no longer completes");
	assert(rig_find("p4", "chord_restore", 0) >= 0);
	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_CHORD_FILE);
	puts("  chord config: a card staged with the RELEASE config is refused at "
		 "p1.chord_bound by name; one that binds the chord still completes");
}

/* ---- C-1 / H-1: the E-2 gate is carried by the SCAN, not by the model ----
 *
 * Hostile review 17, H-1: every "the chord IS bound" case armed
 * CHAT_LOGIN_CHORD_MODEL_BOUND, and that mode SHORT-CIRCUITS
 * cl_chord_on_device(), so s.chord_bound -- the live scan of the binding table
 * -- was never read on any PASSING path and cl_chord_scan never had to answer
 * true for a test to pass. The assertion that looked like proof of the E-2
 * mechanism proved only that the model was armed.
 *
 * That hole is why C-1 shipped, and C-1 is not subtle once the scan is really
 * on the hook: chat_login_pad_for is reached only from input_players_poll,
 * which skips any player whose owner device is NONE, and nobody owns player 0
 * until P1 binds the virtual pad. The gate therefore CANNOT be answered in P0
 * -- P0 needed the scan, the scan needed the bind, the bind needed P0 to
 * finish -- which is why it now runs after the bind.
 *
 * This case is the missing leg and it is built so the model cannot satisfy it:
 * UNBOUND answers nothing about the binding (it only declares "a console" and
 * supplies the order gate), while the config really carries the chord. The
 * only thing that can put "bound":true on p1.chord_bound and carry the run
 * through to p4.chord_restore is cl_chord_scan's own true. */
static void case_chord_scan_carries_the_gate(void) {
	rig_config_use(rig_config_chord_json, RIG_CONFIG_CHORD_FILE);
	rig_reset();
	rig.chord_device = true;
	chat_login_test_chord_model(CHAT_LOGIN_CHORD_MODEL_UNBOUND);
	rig_arm(HW_RUN_PROFILE_MP_CHAT_CHORD_1M);
	rig_run();
	chat_login_test_chord_model(CHAT_LOGIN_CHORD_MODEL_OFF);

	assert(!strcmp(rig.done, "ok")
		   && "a card whose config really binds the chord was refused anyway");
	for(unsigned k = 0; k < rig.line_count; k++)
		assert(!line_has(rig.lines[k], "\"ok\":false"));
	/* THE SCAN. The model answers nothing here, so only cl_chord_scan's own
	 * reading of debug_overlay:[71070] against gui_previous/gui_page can make
	 * this true. */
	int gate = rig_find("p1", "chord_bound", 0);
	assert(gate >= 0 && line_has(rig.lines[gate], "\"bound\":true")
		   && "the live binding scan never reached p1.chord_bound");
	/* ... and the gate really is a GATE: it precedes the rest of P1. */
	assert(gate < rig_find("p1", "pad_bound", 0));
	/* ... and what it carried is the whole profile, not just its own marker. */
	int restore = rig_find("p4", "chord_restore", 0);
	assert(restore >= 0 && "the scan-gated run never reached the restore leg");
	assert(line_has(rig.lines[restore], "\"fired\":true,\"fires\":1"));
	assert(line_has(rig.lines[restore], "\"overlay\":false"));
	assert(rig.overlay_toggles == 2);

	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_CHORD_FILE);
	puts("  chord scan: the LIVE binding table -- not the test model -- "
		 "carried the E-2 gate all the way to the restore leg");
}

/* ---- C-1: the gate with no reading AT ALL -------------------------------
 *
 * The bounded wait exists so that "the table says no chord" and "the table was
 * never read" cannot both end as a phase timeout. They also must not end as
 * the SAME refusal: one is a staging error the operator fixes by staging the
 * developer config, the other is a driver/ownership fault where no input poll
 * ever reached player 0, and sending the operator to the config for the second
 * is the loop C-1 already sent them round once. */
static void case_chord_scan_never_arrives(void) {
	rig_config_use(rig_config_chord_json, RIG_CONFIG_CHORD_FILE);
	rig_reset();
	rig.chord_device = true;
	rig.starve_scan = true; /* the polls stop the moment the pad is bound */
	chat_login_test_chord_model(CHAT_LOGIN_CHORD_MODEL_UNBOUND);
	rig_arm(HW_RUN_PROFILE_MP_CHAT_CHORD_1M);
	rig_run();
	chat_login_test_chord_model(CHAT_LOGIN_CHORD_MODEL_OFF);

	assert(strcmp(rig.done, "ok") != 0 && "an unscanned run reported ok");
	int at = rig_find("p1", "chord_bound", 0);
	assert(at >= 0 && "the unscanned gate must still publish a verdict");
	assert(line_has(rig.lines[at], "\"ok\":false"));
	assert(line_has(rig.lines[at], "never scanned")
		   && "the no-reading refusal must say the table was never read");
	/* ... and it must NOT send the operator to the config, which is a
	 * different fault with a different fix. */
	assert(!line_has(rig.lines[at], "config_wii_developer.json")
		   && "a driver fault was reported as a staging error");
	assert(!line_has(rig.lines[at], "phase timeout")
		   && "the wait must end in a verdict, not a timeout");
	assert(rig_find("p2", "chord", 0) < 0);
	assert(rig_find("p4", "chord_restore", 0) < 0);

	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_CHORD_FILE);
	puts("  chord scan starved: no reading ever arrived, and the refusal says "
		 "so by its own name instead of blaming the staged config");
}

/* ---- B4a: PLAY MODE ------------------------------------------------------
 *
 * mp_play_10m calls this login machine twice in one run and needs the virtual
 * pad for everything in between (docs/mp_play_profile.md section 3; the core
 * is source/game/mp_play_core.h). These cases drive the real driver through
 * the real production handlers exactly as every case above does; what is new
 * is only what happens AFTER the login succeeds.
 *
 * The companion proof is case_trace_equivalence: none of this may be visible
 * to mp_chat_login_3m or mp_chat_chord_1m, and its golden says so byte for
 * byte. */

static void rig_arm_play(void) {
	chat_login_arm_play(&rig_hooks, RIG_USERNAME);
}

static void rig_run_parked(void) {
	while(!rig.done_sent && rig.frames < RIG_MAX_FRAMES
		  && chat_login_phase_now() != CHAT_LOGIN_PARKED)
		rig_frame();
	assert(!rig.done_sent && "a play-mode login reported instead of parking");
	assert(chat_login_phase_now() == CHAT_LOGIN_PARKED
		   && "the play-mode login never parked");
}

/* What the virtual pad is asserting right now, read back through the seam's
 * own resolver with the same live table input_players_poll hands it -- so the
 * assertion is about production's resolution, not about a code list this test
 * wrote down. Returns the count, or 0xFFFFFFFF when the seam refused (nothing
 * owns player 0, or the driver is not bound). */
static unsigned rig_pad_codes(int* out, unsigned cap, bool* connected) {
	int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION];
	size_t lengths[IB_COUNT];
	struct chat_login_pad pad;
	rig_binding_map(map, lengths);
	if(!chat_login_pad_for(0, map, lengths, &pad))
		return 0xFFFFFFFFu;
	if(connected)
		*connected = pad.connected;
	/* `out` may be NULL when the caller only wants the seam re-resolved (the
	 * vocabulary sweep below). */
	assert(!out || pad.count <= cap);
	for(unsigned k = 0; out && k < pad.count; k++)
		out[k] = pad.codes[k];
	return pad.count;
}

/* The contract-2 marker set of ONE login, in contract order. */
static void rig_assert_login_markers(unsigned from, bool with_bind) {
	static const char* const phases[] = {"p0", "p1", "p1", "p1", "p2", "p2",
										 "p2", "p3", "p3", "p4", "p4", "p5",
										 "p5", "p5", "p6", "run"};
	static const char* const markers[] = {
		"session",			"pad_bound", "hold_refused", "chat_open",
		"hold_result",		"tap_result", "page_hold",	 "frozen",
		"rearmed",			"origin",	 "draft",		 "sent_requested",
		"composer_cleared", "lines_sent", "ack",		 "ok"};
	unsigned at = from;
	for(unsigned k = 0; k < sizeof(phases) / sizeof(phases[0]); k++) {
		/* A RESTARTED login re-enters past the bind work, so it emits neither
		 * p0.session nor p1.pad_bound (chat_login_script.h says so by name). */
		if(!with_bind && k <= 1) {
			assert(rig_find(phases[k], markers[k], from) < 0
				   && "a restarted login re-emitted a bind-time marker");
			continue;
		}
		int found = rig_find(phases[k], markers[k], at);
		assert(found >= 0 && "a contract marker is missing from this login");
		assert(rig_marker_ok(found) && "a contract marker reported ok:false");
		at = (unsigned)found;
	}
}

/* 2b: the login ends on run.ok exactly as it does today, and then PARKS --
 * pad still bound, profile_done not called -- and the parked stretch is
 * charged to no login's budget. 2e: play_end withdraws the pad in silence. */
static void case_play_park(void) {
	rig_config_use(rig_config_play_json, RIG_CONFIG_PLAY_FILE);
	rig_reset();
	rig.ack_after_send = true;
	rig_arm_play();
	assert(chat_login_armed());
	rig_run_parked();

	rig_assert_login_markers(0, true);
	for(unsigned k = 0; k < rig.line_count; k++)
		assert(!line_has(rig.lines[k], "\"ok\":false"));
	assert(rig_find("run", "ok", 0) == (int)rig.line_count - 1
		   && "run.ok is still the last line of the login");
	assert(!rig.done_sent && "play mode must not report the login as the run");
	assert(input_player_frame(0) != NULL && "the parked pad must stay bound");
	assert(chat_controller_mode_requested()
		   && "the parked pad must stay in controller mode");
	rig_assert_gapless();
	/* Parked and idle: no request has been made, so the pad asserts nothing. */
	{
		int codes[CHAT_LOGIN_PAD_CODES];
		bool connected = false;
		unsigned n = rig_pad_codes(codes, CHAT_LOGIN_PAD_CODES, &connected);
		assert(n == 0 && connected && "a parked pad must be present and quiet");
	}

	/* The total cap is per LOGIN, not per process: sit here well past
	 * CHAT_LOGIN_TOTAL_MS and nothing may time out, report or emit. */
	unsigned lines = rig.line_count;
	uint32_t until = rig.now_ms + CHAT_LOGIN_TOTAL_MS + 5000u;
	while(rig.now_ms < until && rig.frames < RIG_MAX_FRAMES)
		rig_frame();
	assert(rig.now_ms >= until && "the parked dwell never ran");
	assert(chat_login_phase_now() == CHAT_LOGIN_PARKED
		   && "parked time was charged to the login budget");
	assert(!rig.done_sent && rig.line_count == lines
		   && "a parked driver must be silent");
	assert(input_player_frame(0) != NULL);

	/* 2e: the play driver owns completion, so ending is a withdrawal, not a
	 * verdict. */
	chat_login_play_end();
	assert(!chat_login_armed());
	assert(input_player_frame(0) == NULL && "play_end must unbind");
	assert(!chat_controller_mode_requested());
	assert(!rig.done_sent && "play_end must never call profile_done");
	rig_frame();
	assert(rig.line_count == lines && !rig.done_sent
		   && "a disarmed driver must emit nothing");

	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_PLAY_FILE);
	printf("  play park: run.ok then PARKED, pad held, %u ms of parked time "
		   "charged to nothing, play_end silent\n",
		   CHAT_LOGIN_TOTAL_MS + 5000u);
}

/* 2c: while parked the pad asserts the codes bound to the REQUESTED actions
 * and nothing else -- resolved through the live table, chords never asserted
 * as composites -- and the request is refused outright when not parked. */
static void case_play_request(void) {
	static const enum input_button attack_jump[] = {IB_ACTION1, IB_JUMP};
	static const enum input_button too_many[CHAT_LOGIN_PLAY_ACTIONS + 1] = {
		IB_ACTION1};
	int codes[CHAT_LOGIN_PAD_CODES];
	unsigned n;

	rig_config_use(rig_config_play_json, RIG_CONFIG_PLAY_FILE);
	rig_reset();
	rig.ack_after_send = true;
	rig_arm_play();
	/* NOT parked yet: the machine is still typing, and a caller that pressed
	 * a gameplay button now would type it into the composer. */
	while(!rig.done_sent && rig.frames < RIG_MAX_FRAMES
		  && chat_login_phase_now() != CHAT_LOGIN_P4)
		rig_frame();
	assert(chat_login_phase_now() == CHAT_LOGIN_P4);
	assert(!chat_login_play_request(attack_jump, 2)
		   && "a request was accepted mid-login");
	rig_run_parked();
	assert(rig_count("p4", "key") == 53
		   && "the refused request disturbed the typing route");

	/* The request, resolved: item_action_left is [65, 71070] and jump is
	 * [32], so the pad asserts 65 and 32 -- never the 71070 composite, whose
	 * halves reach the owner frame through this same reader. */
	assert(chat_login_play_request(attack_jump, 2));
	n = rig_pad_codes(codes, CHAT_LOGIN_PAD_CODES, NULL);
	assert(n == 2 && codes[0] == 65 && codes[1] == 32);
	/* It PERSISTS: nothing but another call changes it. */
	rig_frame();
	rig_frame();
	n = rig_pad_codes(codes, CHAT_LOGIN_PAD_CODES, NULL);
	assert(n == 2 && codes[0] == 65 && codes[1] == 32);
	/* ... and is REPLACED, not merged. */
	{
		static const enum input_button use_only[] = {IB_ACTION2};
		assert(chat_login_play_request(use_only, 1));
		n = rig_pad_codes(codes, CHAT_LOGIN_PAD_CODES, NULL);
		assert(n == 1 && codes[0] == 66);
	}
	/* count == 0 releases everything. */
	assert(chat_login_play_request(NULL, 0));
	n = rig_pad_codes(codes, CHAT_LOGIN_PAD_CODES, NULL);
	assert(n == 0);
	/* An over-long list is refused, not truncated, and leaves the pad alone. */
	assert(chat_login_play_request(attack_jump, 2));
	assert(!chat_login_play_request(too_many, CHAT_LOGIN_PLAY_ACTIONS + 1));
	n = rig_pad_codes(codes, CHAT_LOGIN_PAD_CODES, NULL);
	assert(n == 2 && codes[0] == 65 && codes[1] == 32
		   && "a refused request changed the pad");

	chat_login_play_end();
	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_PLAY_FILE);
	puts("  play request: the bound codes of the requested actions only, the "
		 "chord encoding never asserted, refused off the parked state");
}

/* 2c, the other half: the vocabulary chat_login_pad_native_key answers is
 * EXACTLY today's twelve chat roles for the two existing profiles -- even
 * with a config that binds the play actions -- and reaches past them only for
 * a parked play-mode driver's current request. */
static void case_play_vocabulary(void) {
	/* The twelve roles' codes under the play config (CHAT shares gui_up's
	 * 265, exactly as config_wii.json shares a code between two roles). */
	static const int role_codes[]
		= {263, 262, 265, 264, 88, 90, 70, 71, 84, 69, 257};
	static const enum input_button attack[] = {IB_ACTION1};

	rig_config_use(rig_config_play_json, RIG_CONFIG_PLAY_FILE);
	rig_reset();
	rig.ack_after_send = true;
	rig_arm(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M);
	while(!rig.done_sent && rig.frames < RIG_MAX_FRAMES
		  && chat_login_phase_now() != CHAT_LOGIN_P4)
		rig_frame();
	assert(chat_login_phase_now() == CHAT_LOGIN_P4);
	for(int k = 0; k < RIG_KEY_SWEEP; k++) {
		bool value = false, role = false;
		for(unsigned j = 0; j < sizeof(role_codes) / sizeof(role_codes[0]); j++)
			role |= role_codes[j] == k;
		assert(chat_login_pad_native_key(k, &value) == role
			   && "mp_chat_login_3m's native-key vocabulary changed");
	}
	rig_run();
	assert(!strcmp(rig.done, "ok"));

	/* The SAME sweep on a parked play-mode driver: the requested action's code
	 * is answered too, and only while it is requested. */
	rig_reset();
	rig.ack_after_send = true;
	rig_arm_play();
	rig_run_parked();
	assert(chat_login_play_request(attack, 1));
	(void)rig_pad_codes(NULL, 0, NULL); /* refresh the seam's own vocabulary */
	{
		bool value = false;
		assert(chat_login_pad_native_key(65, &value) && value
			   && "a requested play action is not in the pad's vocabulary");
		assert(!chat_login_pad_native_key(66, &value)
			   && "an unrequested play action leaked into the vocabulary");
		assert(chat_login_play_request(NULL, 0));
		(void)rig_pad_codes(NULL, 0, NULL);
		assert(!chat_login_pad_native_key(65, &value)
			   && "a released play action stayed in the vocabulary");
	}
	chat_login_play_end();

	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_PLAY_FILE);
	puts("  play vocabulary: unchanged for mp_chat_login_3m across the whole "
		 "code space; extended only for a parked play request");
}

/* 2d: a second login on the still-bound pad. */
static void case_play_restart(void) {
	rig_config_use(rig_config_play_json, RIG_CONFIG_PLAY_FILE);
	rig_reset();
	rig.ack_after_send = true;
	rig_arm_play();
	rig_run_parked();
	unsigned first = rig.line_count;
	uint32_t seq_after_first = chat_login_seq_now();
	assert(seq_after_first == first && "the first login's seq is 1..N");

	/* A pending request is released by the restart: a gameplay button still
	 * held would type into the composer. */
	{
		static const enum input_button attack[] = {IB_ACTION1};
		int codes[CHAT_LOGIN_PAD_CODES];
		assert(chat_login_play_request(attack, 1));
		assert(chat_login_play_restart());
		assert(chat_login_phase_now() == CHAT_LOGIN_P1);
		assert(rig_pad_codes(codes, CHAT_LOGIN_PAD_CODES, NULL) == 0
			   && "the restart kept a gameplay button asserted");
	}
	rig.ack_pushed = false; /* the fixture answers the SECOND /login too */
	rig_run_parked();

	/* The second login emits the whole contract set except the two bind-time
	 * markers, and the stream's seq never restarts. */
	rig_assert_login_markers(first, false);
	rig_assert_gapless();
	assert(chat_login_seq_now() == rig.line_count
		   && chat_login_seq_now() > seq_after_first
		   && "seq must continue across a restart, never reset");
	assert(rig_count("p0", "session") == 1 && rig_count("p1", "pad_bound") == 1
		   && "only the FIRST login binds");
	assert(rig_count("run", "ok") == 2 && rig_count("p6", "ack") == 2
		   && rig_count("p4", "draft") == 2);
	assert(rig_count("p4", "key") == 106 && "both logins typed the command");
	for(unsigned k = 0; k < rig.line_count; k++)
		assert(!line_has(rig.lines[k], "\"ok\":false"));
	assert(!rig.done_sent && input_player_frame(0) != NULL);
	/* The second submission really was a second chat line. */
	assert(screen_chat_test_enqueue_count() == 2);
	{
		int second = rig_find("p5", "lines_sent", first);
		assert(second >= 0
			   && line_has(rig.lines[second], "\"before\":1,\"after\":2"));
	}
	/* Restarting is refused from anywhere but PARKED. */
	assert(chat_login_play_restart());
	assert(chat_login_phase_now() == CHAT_LOGIN_P1);
	assert(!chat_login_play_restart() && "a restart was accepted mid-login");

	chat_login_play_end();
	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_PLAY_FILE);
	printf("  play restart: a second login on the same pad, seq %u -> %u, no "
		   "p0.session and no p1.pad_bound repeated\n",
		   (unsigned)seq_after_first, (unsigned)chat_login_seq_now());
}

/* 2b: a login that FAILS in play mode is still a run that ended -- the pad is
 * withdrawn and profile_done is called exactly once, as today. */
static void case_play_failed_login(void) {
	rig_config_use(rig_config_play_json, RIG_CONFIG_PLAY_FILE);
	rig_reset();
	rig.ack_after_send = true;
	rig.timeout_armed = true;
	rig.timeout_phase = CHAT_LOGIN_P4;
	rig_arm_play();
	rig_run(); /* rig_profile_done asserts it is called at most once */

	assert(!strncmp(rig.done, "p4:phase timeout ", 17)
		   && "a failed play-mode login must report like any other");
	assert(chat_login_phase_now() == CHAT_LOGIN_FAILED);
	assert(input_player_frame(0) == NULL && "a failed login still unbinds");
	assert(!chat_controller_mode_requested());
	rig_assert_gapless();
	/* ... and a parked-only control is refused on the wreckage. */
	assert(!chat_login_play_restart());
	assert(!chat_login_play_request(NULL, 0));

	chat_login_play_end();
	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_PLAY_FILE);
	puts("  play failed login: unbound, one profile_done, parked controls "
		 "refused");
}

/* 2f: the recovery-modal rule stays live while PARKED, and surfaces through
 * chat_login_play_fault instead of taking the pad away and reporting a verdict
 * that belongs to the play driver. */
static void case_play_fault(void) {
	const char* diag = NULL;
	rig_config_use(rig_config_play_json, RIG_CONFIG_PLAY_FILE);
	rig_reset();
	rig.ack_after_send = true;
	rig_arm_play();
	rig_run_parked();
	assert(!chat_login_play_fault(&diag) && "a clean park reported a fault");

	unsigned lines = rig.line_count;
	rig.force_recovery_modal = true;
	rig_frame();
	assert(chat_login_play_fault(&diag) && diag);
	assert(strstr(diag, "recovery modal") && "the fault must name the rule");
	/* It is a QUERY, not a verdict: no marker, no profile_done, pad still
	 * bound and still parked, so the play driver decides what the run does. */
	assert(rig.line_count == lines && !rig.done_sent);
	assert(chat_login_phase_now() == CHAT_LOGIN_PARKED);
	assert(input_player_frame(0) != NULL);
	/* First fault wins and a restart does not erase it. */
	rig.force_recovery_modal = false;
	rig_frame();
	assert(chat_login_play_fault(NULL) && "the fault latch was cleared");
	assert(chat_login_play_restart());
	assert(chat_login_play_fault(NULL));

	chat_login_play_end();
	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_PLAY_FILE);
	printf("  play fault: \"%s\" surfaced by query, pad kept, nothing "
		   "reported\n",
		   diag);
}

/* ---- REL-14b-0: action-only acquisition (silent vanilla login) ----------
 * Vanilla Beta logs an unknown `/login` and replies with nothing (CFR
 * NetServerHandler.java:471-474), so no case below ever pushes the fixture
 * ACK: readiness must come from the observed session and the observed bind. */

static unsigned rig_count_typed(unsigned from) {
	unsigned n = 0;
	for(unsigned k = from; k < rig.line_count; k++)
		n += line_has(rig.lines[k], "\"marker\":\"hold_refused\"")
			|| line_has(rig.lines[k], "\"phase\":\"p4\"")
			|| line_has(rig.lines[k], "\"phase\":\"p5\"")
			|| line_has(rig.lines[k], "\"phase\":\"p6\"");
	return n;
}

static void case_actions_silent_vanilla(void) {
	static const enum input_button attack_jump[] = {IB_ACTION1, IB_JUMP};
	int codes[CHAT_LOGIN_PAD_CODES];
	rig_config_use(rig_config_play_json, RIG_CONFIG_PLAY_FILE);
	rig_reset();
	chat_login_arm_play_actions(&rig_hooks, RIG_USERNAME);
	assert(!chat_login_play_request(attack_jump, 2)
		   && "actions accepted before readiness was observed");
	rig_run_parked();
	assert(rig_marker_ok(rig_find("p0", "session", 0))
		   && rig_marker_ok(rig_find("p1", "pad_bound", 0))
		   && rig_marker_ok(rig_find("p1", "actions_ready", 0)));
	assert(rig_find("p1", "actions_ready", 0) == (int)rig.line_count - 1);
	assert(rig_count_typed(0) == 0 && screen_chat_test_enqueue_count() == 0
		   && "action-only acquisition typed a command");
	assert(rig_find("run", "ok", 0) < 0 && !rig.done_sent);
	rig_assert_gapless();
	assert(chat_login_play_request(attack_jump, 2));
	assert(rig_pad_codes(codes, CHAT_LOGIN_PAD_CODES, NULL) == 2
		   && codes[0] == 65 && codes[1] == 32);
	chat_login_play_end();
	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_PLAY_FILE);
	puts("  actions silent vanilla: session + bind -> PARKED with no ACK and "
		 "nothing typed");
}

/* The session drops between the bind and readiness: refused, unbound, one
 * verdict, and every parked-only control refused on the wreckage. */
static void case_actions_disconnect(void) {
	rig_config_use(rig_config_play_json, RIG_CONFIG_PLAY_FILE);
	rig_reset();
	chat_login_arm_play_actions(&rig_hooks, RIG_USERNAME);
	while(rig.frames < RIG_MAX_FRAMES
		  && !(chat_login_phase_now() == CHAT_LOGIN_P1
			   && chat_login_step_now() == 2))
		rig_frame();
	gstate.remote_session = false;
	rig_run();
	assert(chat_login_phase_now() == CHAT_LOGIN_FAILED);
	assert(strstr(rig.done, "session lost") && "the refusal must say why");
	assert(!rig_marker_ok(rig_find("p1", "actions_ready", 0)));
	assert(input_player_frame(0) == NULL && !chat_controller_mode_requested());
	assert(!chat_login_play_request(NULL, 0) && !chat_login_play_restart());
	rig_assert_gapless();
	chat_login_play_end();
	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_PLAY_FILE);
	puts("  actions disconnect: lost session refused readiness, pad withdrawn");
}

/* Reconnect: the restart leaves PARKED at once (no stale grant), re-observes a
 * fresh session and the binding, and only then accepts actions again. */
static void case_actions_reconnect(void) {
	static const enum input_button attack[] = {IB_ACTION1};
	int codes[CHAT_LOGIN_PAD_CODES];
	rig_config_use(rig_config_play_json, RIG_CONFIG_PLAY_FILE);
	rig_reset();
	chat_login_arm_play_actions(&rig_hooks, RIG_USERNAME);
	rig_run_parked();
	unsigned first = rig.line_count;
	assert(chat_login_play_request(attack, 1));
	assert(chat_login_play_restart());
	assert(chat_login_phase_now() != CHAT_LOGIN_PARKED
		   && "a restart kept the previous generation's PARKED");
	assert(!chat_login_play_request(attack, 1)
		   && "actions accepted on a stale generation");
	assert(rig_pad_codes(codes, CHAT_LOGIN_PAD_CODES, NULL) == 0);
	gstate.world_loaded = false; /* the rejoined world is still installing */
	for(unsigned k = 0; k < 30; k++)
		rig_frame();
	assert(chat_login_phase_now() == CHAT_LOGIN_P0 && rig.line_count == first);
	gstate.world_loaded = true;
	rig_run_parked();
	assert(rig_count("p0", "session") == 2 && rig_count("p1", "pad_bound") == 2
		   && rig_count("p1", "actions_ready") == 2);
	assert(rig_count_typed(0) == 0 && screen_chat_test_enqueue_count() == 0);
	rig_assert_gapless();
	assert(chat_login_play_request(attack, 1));
	/* release */
	assert(chat_login_play_request(NULL, 0));
	assert(rig_pad_codes(codes, CHAT_LOGIN_PAD_CODES, NULL) == 0);
	chat_login_play_end();
	assert(input_player_frame(0) == NULL && !rig.done_sent);
	assert(!chat_login_play_request(attack, 1) && "a withdrawn pad took actions");
	rig_config_use(rig_config_json, RIG_CONFIG_FILE);
	remove(RIG_CONFIG_PLAY_FILE);
	puts("  actions reconnect: fresh generation re-acquired, stale PARKED "
		 "refused, release + end silent");
}

/* ---- B4a: the before/after trace equivalence (Astra answer B3) ----------
 *
 * See the recorder's own comment above for what is recorded and what is
 * normalised. This case is the "before" when it is committed against the
 * untouched driver and the "after" every time it runs afterwards: the PLAY
 * mode added on top of this file must leave both existing profiles emitting
 * the same markers, in the same order, on the same frames, with the same
 * per-frame pad request codes. */

/* add_test() gives this test the BUILD directory as its CWD (no
 * WORKING_DIRECTORY property, and CMakeLists.txt is another lane's file), so
 * the golden is found by walking up. Four levels covers build-verify/ under
 * the worktree root and the source root itself. */
static const char* rec_golden_path(void) {
	static char path[512];
	static const char* const up[] = {"", "../", "../../", "../../../"};
	for(unsigned k = 0; k < sizeof(up) / sizeof(up[0]); k++) {
		char dir[512];
		struct stat st;
		snprintf(dir, sizeof(dir), "%s%s", up[k], REC_GOLDEN_DIR);
		if(stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
			/* precision-bounded so GCC can prove no truncation (the
			 * test_fixture_dir.h convention) */
			snprintf(path, sizeof(path), "%.400s/%s", dir, REC_GOLDEN_FILE);
			return path;
		}
	}
	/* An out-of-tree build dir (tools/ci.sh CI_VERIFY_DIR=/tmp/...) has no
	 * tests/ above its CWD: fall back to this source file's own directory, the
	 * same __FILE__ anchor test_sort_import_roundtrip's native_file uses. */
	char dir[512];
	struct stat st;
	snprintf(dir, sizeof(dir), "%.400s", __FILE__);
	char* slash = strrchr(dir, '/');
	char* back = strrchr(dir, '\\');
	if(back && (!slash || back > slash))
		slash = back;
	if(slash) {
		/* REC_GOLDEN_DIR is tests/-relative-to-root; strip the "tests/". */
		snprintf(slash + 1, sizeof(dir) - (size_t)(slash + 1 - dir), "%s",
				 REC_GOLDEN_DIR + strlen("tests/"));
		if(stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
			snprintf(path, sizeof(path), "%.400s/%s", dir, REC_GOLDEN_FILE);
			return path;
		}
	}
	return NULL;
}

static void rec_profile(enum hw_run_profile profile, const char* name) {
	char head[160];
	rig_reset();
	rig.frame_ms = REC_FRAME_MS;
	rig.frame_dt = REC_FRAME_DT;
	rig.ack_after_send = true;
	rec.frame = 0;
	snprintf(head, sizeof(head), "P %s frame_ms=%u\n", name, REC_FRAME_MS);
	rec_put(head);
	rec.on = true;
	rig_arm(profile);
	rig_run();
	rec.on = false;
	snprintf(head, sizeof(head), "D %s %s\n", name, rig.done);
	rec_put(head);
	/* The recorded run is a HAPPY path in both profiles -- a golden of a run
	 * that failed for an unrelated reason would freeze the failure, not the
	 * behaviour. */
	assert(!strcmp(rig.done, "ok") && "the recorded profile must complete ok");
}

static void case_trace_equivalence(void) {
	rec.len = 0;
	if(rec.buf)
		rec.buf[0] = 0;
	rec_put("# chat_login shared-module trace -- tests/test_chat_login_script.c,\n"
			"# case_trace_equivalence. Astra ruling 1 answer B3. Regenerate with\n"
			"# CW_UPDATE_CHAT_LOGIN_TRACE=1. '*' = a live-timing value, deliberately\n"
			"# NOT claimed byte-identical (rec_norm_keys[] plus a trace row's t/rem).\n"
			"# M <marker line>   F <frame> <pad connected> <asserted code>...\n"
			"# F <frame> -       player 0 unowned: production never reaches the seam\n"
			"# F <frame> x       owned, but the seam refused (driver not bound)\n");
	rec_profile(HW_RUN_PROFILE_MP_CHAT_LOGIN_3M, "mp_chat_login_3m");
	rec_profile(HW_RUN_PROFILE_MP_CHAT_CHORD_1M, "mp_chat_chord_1m");

	const char* path = rec_golden_path();
	assert(path && "tests/golden/chat_login must exist at or above the CWD");
	if(getenv("CW_UPDATE_CHAT_LOGIN_TRACE")) {
		FILE* out = fopen(path, "wb");
		assert(out && "the trace golden is not writable");
		assert(fwrite(rec.buf, 1, rec.len, out) == rec.len);
		fclose(out);
		printf("  trace golden REGENERATED: %s (%u bytes)\n", path,
			   (unsigned)rec.len);
		return;
	}
	FILE* in = fopen(path, "rb");
	assert(in && "the trace golden is missing -- see case_trace_equivalence");
	char* want = malloc(rec.len + 4096);
	assert(want);
	size_t n = fread(want, 1, rec.len + 4096, in);
	assert(!ferror(in));
	fclose(in);
	if(n != rec.len || memcmp(want, rec.buf, n)) {
		size_t at = 0;
		while(at < n && at < rec.len && want[at] == rec.buf[at])
			at++;
		fprintf(stderr,
				"chat_login trace golden mismatch at byte %u "
				"(golden %u bytes, recorded %u bytes)\n",
				(unsigned)at, (unsigned)n, (unsigned)rec.len);
		size_t from = at > 160 ? at - 160 : 0;
		fprintf(stderr, "golden   ...%.240s\n", want + from);
		fprintf(stderr, "recorded ...%.240s\n", rec.buf + from);
		assert(0 && "the two existing profiles are no longer trace-equivalent");
	}
	free(want);
	printf("  trace equivalence: both profiles byte-identical to the golden "
		   "(%u bytes, %u frames recorded)\n",
		   (unsigned)rec.len, rec.frame);
}

/* Execute the actual screen layouts; the standalone keyboard test covers the
 * forwarded page/backspace activation and source wiring separately. */
extern void screen_chat_test_row(const struct input_player_frame*, int, int,
	struct gutil_hint_row*, int[5], struct gutil_button_bounds[5]);
extern void screen_chat_test_layout(int, int, struct menu_composition*,
	struct menu_rect*, struct menu_rect*, int*);
extern int screen_ingame_chat_socket_layout(const struct input_player_frame*,
	int, int, int, bool, struct gutil_button_bounds*, struct gutil_button_bounds*,
	struct gutil_button_bounds*, struct gutil_button_bounds*,
	struct gutil_button_bounds*, int[2], const char**);
extern int screen_ingame_chat_socket_prompt(const struct input_player_frame*, int);
extern bool screen_ingame_gameplay_hint_layout(const struct input_player_frame*,
	int, int, bool, const struct screen_ingame_hint*, int, struct gutil_hint_row*);

static void case_footer_geometry(void) {
	const enum input_category families[] = {INPUT_CAT_NUNCHUK,
		INPUT_CAT_CLASSIC_CONTROLLER, INPUT_CAT_GAMECUBE};
	const int codes[] = {3, 200, 400}, widths[] = {640, 802}, heights[] = {480, 240};
	const enum input_button roles[] = {IB_GUI_QUICK_MOVE, IB_INVENTORY,
		IB_GUI_PREVIOUS, IB_GUI_PAGE, IB_GUI_CLICK_ALT};
	const int hint_codes[3][5] = {{100, 6, 9, 8, 5},
		{207, 206, 210, 211, 205}, {406, 407, 408, 409, 405}};
	for(int f = 0; f < 3; f++) for(int w = 0; w < 2; w++)
	for(int h = 0; h < 2; h++) for(int page = 0; page < 3; page++) {
		struct input_player_frame owner = {.connected = true, .category = families[f]};
		for(int b = 0; b < IB_COUNT; b++) owner.glyphs[b] = input_glyph_key(families[f], codes[f]);
		for(int b = 0; b < 5; b++) owner.glyphs[roles[b]] = input_glyph_key(families[f], hint_codes[f][b]);
		gstate.players[0].chat.osk.page = page == 0 ? OSK_PAGE_LOWER
			: page == 1 ? OSK_PAGE_NUM : OSK_PAGE_SYMBOLS;
		struct gutil_hint_row row;
		struct gutil_button_bounds keys[5]; int targets[5];
		screen_chat_test_row(&owner, widths[w], heights[h], &row, targets, keys);
		assert(row.count >= 3 && row.fits);
		assert(abs(2 * row.strip.x + row.strip.width - widths[w]) <= 1);
		assert(row.strip.y == (h ? 196 : 428));
		bool send = false, cancel = false;
		for(int k = 0; k < row.count; k++) {
			const struct gutil_hint_cell* c = &row.cells[k];
			assert(gutil_hint_row_hit(&row, c->rect.x + 1, c->rect.y + 1) == c->item);
			int role = targets[k] < 2 ? targets[k] : targets[k] - 1;
			assert(c->glyph[0] == owner.glyphs[roles[role]]);
			send |= targets[k] == 0; cancel |= targets[k] == 1;
			if(targets[k] >= 3) {
				assert(keys[k].width > 0 && keys[k].y + keys[k].height <= row.strip.y);
				if(!h && targets[k] != 5) {
					struct gutil_button_bounds expected;
					assert(osk_chat_key_bounds((widths[w] - 640) / 2,
						OSK_CHAT_ROW_TABS, page + (targets[k] == 3 ? -1 : 1), &expected));
					assert(!memcmp(&expected, &keys[k], sizeof expected));
				}
			}
		}
		assert(send && cancel);
		struct menu_composition composition;
		screen_chat_test_layout(widths[w], heights[h], &composition, NULL, NULL, NULL);
		assert(composition.window.y + composition.window.h <= row.strip.y);
	}
	puts("PASS chat footer: centred clickable cells, Send/Cancel and page targets, 3 families/full/split/640/802");
}

static void case_socket_row_clearance(void) {
	struct screen_ingame_hint_ctx ctx = {.target = SCREEN_INGAME_HINT_BLOCK,
		.held = SCREEN_INGAME_HELD_BLOCK};
	struct screen_ingame_hint hints[SCREEN_INGAME_HINT_MAX];
	int n = screen_ingame_hint_row(&ctx, hints);
	assert(n == 5);
	const enum input_category families[] = {INPUT_CAT_NUNCHUK,
		INPUT_CAT_CLASSIC_CONTROLLER, INPUT_CAT_GAMECUBE, INPUT_CAT_NONE, INPUT_CAT_NONE};
	const int codes[] = {4, 204, 404, 84, 257};
	int collisions = 0;
	for(int f = 0; f < 5; f++) for(int w = 640; w <= 802; w += 162) {
		struct input_player_frame owner = {.connected = true, .category = families[f]};
		for(int b = 0; b < IB_COUNT; b++) owner.glyphs[b] = input_glyph_key(families[f], codes[f]);
		struct gutil_button_bounds frame, glyph, label;
		int kind = screen_ingame_chat_socket_layout(&owner, w, 480, codes[f], false,
			NULL, &frame, &glyph, NULL, &label, NULL, NULL);
		assert(kind);
		int prompt = screen_ingame_chat_socket_prompt(&owner, codes[f]);
		assert(prompt == owner.glyphs[IB_CHAT]);
		if(f >= 3) {
			bool wide = false;
			const char* name = input_glyph_label_for_code(prompt, &wide);
			assert(name && !strcmp(name, f == 3 ? "T" : "Enter"));
			assert(glyph.width == (wide ? 64 : 32));
			assert(glyph.width == gutil_prompt_glyph_width(prompt, glyph.height));
			assert(glyph.x + glyph.width <= frame.x + frame.width);
			if(wide) assert(label.y + label.height <= frame.y);
			else assert(frame.x + frame.width < label.x);
			assert(frame.x > (w + 182 * 2) / 2); /* hotbar right edge */
			assert(label.x + label.width <= w);
		}
		struct gutil_hint_row online, offline;
		assert(screen_ingame_gameplay_hint_layout(&owner, w, 480, true, hints, n, &online));
		assert(online.strip.x + online.strip.width <= frame.x);
		assert(screen_ingame_gameplay_hint_layout(&owner, w, 480, false, hints, n, &offline));
		bool pause = false, jump = false;
		for(int k = 0; k < online.count; k++) {
			pause |= !strcmp(online.cells[k].label, "Pause");
			jump |= !strcmp(online.cells[k].label, "Jump");
		}
		assert(pause);
		if(offline.strip.x + offline.strip.width > frame.x) {
			collisions++;
			assert(!jump);
		}
	}
	assert(collisions > 0 && "the font fixture must exercise the reported overlap");
	puts("PASS online gameplay row clears chat; PC T/Enter retain encoded labels and cap widths");
}

extern void gutil_test_set_font_widths(const uint8_t[256]);
static void case_ui_pack_fonts(void) {
	const char* fonts[] = {"assets/font/default.png", "packs/isabella-ii/font/default.png",
		"texturepacks/beta-1.7.3/font/default.png"};
	uint8_t saved[256]; memcpy(saved, gutil_font_glyph_widths(), sizeof saved);
	uint8_t atlas = 0;
	uint8_t* saved_atlas = texture_approved_controls.data;
	texture_approved_controls.data = &atlas; /* metrics only; no graphics calls */
	for(int f = 0; f < 3; f++) {
		char path[1024]; snprintf(path, sizeof path, "%s", __FILE__);
		char* base = strstr(path, "tests"); assert(base); strcpy(base, fonts[f]);
		FILE* probe = fopen(path, "rb");
		if(!probe) { assert(f == 2); puts("Beta font unavailable; optional pack case skipped"); continue; }
		fclose(probe);
		unsigned char* px = NULL; unsigned w = 0, h = 0;
		assert(!lodepng_decode32_file(&px, &w, &h, path));
		assert(w >= 128 && h >= 128 && w % 16 == 0 && h % 16 == 0);
		unsigned cw = w / 16, ch = h / 16; uint8_t widths[256];
		for(unsigned cell = 0; cell < 256; cell++) {
			unsigned ink = 0;
			for(unsigned x = 0; x < cw; x++) {
				bool present = false;
				for(unsigned y = 0; y < ch && !present; y++) {
					const unsigned char* pixel = px + 4 * ((cell / 16 * ch + y) * w + cell % 16 * cw + x);
					present = pixel[0] || pixel[1] || pixel[2];
				}
				if(!present) break;
				ink++;
			}
			widths[cell] = ink * 8 / cw;
		}
		widths[' '] = 4; free(px);
		gutil_test_set_font_widths(widths);
		printf("UI footer font: %s\n", fonts[f]);
		case_footer_geometry(); case_socket_row_clearance();
	}
	gutil_test_set_font_widths(saved);
	texture_approved_controls.data = saved_atlas;
}

int main(void) {
	FILE* f = fopen(RIG_CONFIG_FILE, "wb");
	assert(f && "the rig needs a writable CWD for its binding fixture");
	fputs(rig_config_json, f);
	fclose(f);
	assert(config_create(&gstate.config_user, RIG_CONFIG_FILE)
		   && "the binding fixture must parse");

	/* FIRST, on the pristine rig config: the golden must not depend on any
	 * state a later case leaves behind (case_chord_device and friends swap the
	 * binding fixture out and back again). */
	case_ui_pack_fonts();
	case_trace_equivalence();
	case_happy();
	case_chord();
	case_chord_device();
	case_chord_unbound_config();
	case_chord_scan_carries_the_gate();
	case_chord_scan_never_arrives();
	case_hold_hitch();
	case_timeout(CHAT_LOGIN_P0, "p0", "session", "phase timeout");
	case_timeout(CHAT_LOGIN_P1, "p1", "pad_bound", "phase timeout");
	/* P2 is entered with the trace still CLOSED -- the hold leg opens on its own
	 * first request (section 12) -- so the harness clock jump lands on a
	 * non-recording frame and the phase budget is what fires. P3 is entered
	 * with its leg already open, so there the same jump IS a frame stall: the
	 * two cases together cover both the budget and the per-leg bound (H1). */
	case_timeout(CHAT_LOGIN_P2, "p2", "hold_result", "phase timeout");
	case_timeout(CHAT_LOGIN_P3, "p3", "frozen", "frame stall in trace leg");
	case_timeout(CHAT_LOGIN_P4, "p4", "origin", "phase timeout");
	case_timeout(CHAT_LOGIN_P5, "p5", "sent_requested", "phase timeout");
	case_timeout(CHAT_LOGIN_P6, "p6", "ack", "phase timeout");
	case_run_ok_blocked();
	case_loading_screen_dwell();
	case_wrong_focus();
	case_refused_send();
	case_stale_ack();
	case_disabled_profile();
	case_owner_isolation();
	/* B4a play mode, last: each case swaps the binding fixture in and back
	 * out again, exactly as the chord-device cases above do. */
	case_play_park();
	case_play_request();
	case_play_vocabulary();
	case_play_restart();
	case_play_failed_login();
	case_play_fault();
	case_actions_silent_vanilla();
	case_actions_disconnect();
	case_actions_reconnect();

	config_destroy(&gstate.config_user);
	remove(RIG_CONFIG_FILE);
	printf("test_chat_login_script: all cases passed\n");
	return 0;
}
