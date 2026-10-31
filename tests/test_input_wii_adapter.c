/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define TEST_CHDIR(p) _chdir(p)
#define TEST_GETCWD(b, n) _getcwd(b, n)
#define TEST_MKDIR(p) _mkdir(p)
#define TEST_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TEST_CHDIR(p) chdir(p)
#define TEST_GETCWD(b, n) getcwd(b, n)
#define TEST_MKDIR(p) mkdir(p, 0777)
#define TEST_RMDIR(p) rmdir(p)
#endif
#include <ogc/pad.h>
#include <ogc/lwp_heap.h>
#include <wiiuse/wpad.h>

#include "game/chat_login_script.h"
#include "game/game_state.h"
#include "graphics/gui_util.h"
#include "graphics/texture_atlas.h"
#include "item/items.h"
#include "platform/gfx.h"
#include "platform/input_tap_gesture.h"
#include "platform/input_glyph.h"
#include "platform/input_inventory.h"
#include "game/gui/container_menu.h"
#include "game/gui/screen.h"
#include "game/gui/screen_stats_save.h"
#include "platform/menu_sound.h"
#include "platform/controller_events.h"
#include "platform/audio.h"
#include "network/server_interface.h"

/* The real PLATFORM_WII input.c calls these driver symbols directly. Tests
 * enqueue reports, never set the adapter's fresh/neutral/armed booleans.
 * WPAD_Data retains its last value on an empty queue. ReadPending delivers
 * each callback before coalescing the first transition of each button.
 * Driver contract: devkitPro/libogc wiiuse/wpad.c, WPAD_ReadPending:
 * https://github.com/devkitPro/libogc/blob/master/wiiuse/wpad.c
 */
static struct {
	bool attached;
	u32 type;
	WPADData retained, queue[8];
	size_t count;
} wpad[4];
static struct {
	bool attached;
	/* Probe model (pad_probe_model only): a pad plugged in after boot is not
	 * reported until the driver re-probes its channel via PAD_Reset. */
	bool enabled;
	/* libogc PAD_Read PAD_ERR_TRANSFER/NOT_READY model (pad.c: no new SI
	 * poll response since the last read, or channel busy): the channel is
	 * left out of the ScanPads mask and its keys are NOT updated, so
	 * PAD_ButtonsHeld/sticks keep the last state and PAD_ButtonsDown repeats
	 * the last scan's edge. Cleared by the next scan. */
	bool transfer;
	u16 held, previous, down;
	s8 x, y, cx, cy;
} pad[4];
static bool pad_probe_model;
static unsigned pad_resets;
static u32 pad_reset_union, pad_reset_last;
static unsigned dequeued, scans;
static unsigned configured[4];
static ptime_t now;

s32 WPAD_Init(void) { return 0; }
s32 WPAD_Shutdown(void) { return 0; }
s32 WPAD_Probe(s32 channel, u32* type) {
	assert(channel >= 0 && channel < 4);
	if(!wpad[channel].attached) return WPAD_ERR_NO_CONTROLLER;
	*type = wpad[channel].type;
	return WPAD_ERR_NONE;
}
s32 WPAD_SetDataFormat(s32 channel, s32 format) {
	assert(channel >= 0 && channel < 4 && format == WPAD_FMT_BTNS_ACC_IR);
	configured[channel]++;
	return WPAD_ERR_NONE;
}
s32 WPAD_SetVRes(s32 channel, u32 width, u32 height) {
	assert(channel >= 0 && channel < 4 && width == 640 && height == 480);
	return WPAD_ERR_NONE;
}
WPADData* WPAD_Data(int channel) { return &wpad[channel].retained; }
u32 WPAD_ButtonsHeld(int channel) { return WPAD_Data(channel)->btns_h; }
void WPAD_IR(int channel, struct ir_t* ir) { *ir = WPAD_Data(channel)->ir; }
void WPAD_Expansion(int channel, struct expansion_t* exp) {
	if(wpad[channel].attached) *exp = WPAD_Data(channel)->exp;
}
s32 WPAD_ReadPending(s32 channel, WPADDataCallback callback) {
	assert(channel == WPAD_CHAN_ALL && callback);
	dequeued = 0;
	for(int c = 0; c < 4; c++) {
		u32 before = wpad[c].retained.btns_h, coalesced = before;
		u32 seen = 0;
		for(size_t i = 0; i < wpad[c].count; i++) {
			WPADData report = wpad[c].queue[i];
			wpad[c].retained = report;
			callback(c, &wpad[c].retained);
			/* Per-bit form of the driver's first-transition coalescing. */
			for(unsigned bit = 0; bit < 32; bit++) {
				u32 mask = UINT32_C(1) << bit;
				if(!(seen & mask) && ((report.btns_h ^ before) & mask)) {
					coalesced ^= mask;
					seen |= mask;
				}
			}
			dequeued++;
		}
		wpad[c].retained.btns_h = coalesced;
		wpad[c].retained.btns_d = coalesced & ~before;
		wpad[c].retained.btns_u = before & ~coalesced;
		wpad[c].count = 0;
	}
	return dequeued;
}
u32 PAD_Init(void) { return 0; }
/* libogc/pad.c contract: PAD_Reset takes PAD_CHAN0_BIT >> channel bits and
 * (re)probes those channels; PAD_ScanPads returns connected as 1 << channel. */
u32 PAD_Reset(u32 mask) {
	pad_resets++;
	pad_reset_union |= mask;
	pad_reset_last = mask;
	for(unsigned c = 0; c < 4; c++) {
		if(mask & (PAD_CHAN0_BIT >> c)) pad[c].enabled = pad[c].attached;
	}
	return 1;
}
u32 PAD_ScanPads(void) {
	u32 mask = 0;
	scans++;
	for(unsigned c = 0; c < 4; c++) {
		if(!pad[c].attached) pad[c].enabled = false;
		if(pad[c].attached && pad[c].transfer) {
			pad[c].transfer = false;
			continue; /* libogc `default:` case: keys untouched */
		}
		if(pad[c].attached && (!pad_probe_model || pad[c].enabled))
			mask |= 1u << c;
		pad[c].down = pad[c].held & ~pad[c].previous;
		pad[c].previous = pad[c].held;
	}
	return mask;
}
u16 PAD_ButtonsHeld(int c) { return pad[c].held; }
u16 PAD_ButtonsDown(int c) { return pad[c].down; }
s8 PAD_StickX(int c) { return pad[c].x; }
s8 PAD_StickY(int c) { return pad[c].y; }
s8 PAD_SubStickX(int c) { return pad[c].cx; }
s8 PAD_SubStickY(int c) { return pad[c].cy; }

/* Deterministic clock in milliseconds; no wall-clock sleeps. It is also the
 * injected millisecond source the chat opener's tap window is measured on. */
ptime_t time_get(void) { return now; }
ptime_t time_add_ms(ptime_t t, unsigned ms) { return t + ms; }
int32_t time_diff_ms(ptime_t f, ptime_t s) { return (int32_t)(s - f); }
bool time_reached(ptime_t t, ptime_t deadline) { return t >= deadline; }

/* The real cue dispatcher is not linked into this focused TU: record what the
 * Wii input path actually emits, so requirement "edge-triggered from WPAD/PAD
 * state" can be asserted on the production input.c itself. */
static unsigned cues, battery_cues, battery_rearms;
static enum menu_sound_event cue_last;
static struct input_device cue_device;
void audio_menu_device_event(enum menu_sound_event event,
							 struct input_device device, ptime_t when) {
	(void)when;
	cues++;
	if(event == MENU_SOUND_BATTERY_ATTENTION)
		battery_cues++;
	cue_last = event;
	cue_device = device;
}
void audio_menu_battery_rearm(struct input_device device) {
	(void)device;
	battery_rearms++;
}

/* Link-only platform diagnostics and unrelated engine paths. */
heap_cntrl __wkspace_heap;
u32 __lwp_heap_getinfo(heap_cntrl* heap, heap_iblock* info) {
	(void)heap; (void)info; assert(false); return 1;
}
u32 __lwp_wkspace_heapsize(void) { assert(false); return 0; }
struct game_state gstate;
const char* path_from_sd_root(const char* path, char* buf, size_t length) {
	(void)path; (void)buf; (void)length; assert(false); return NULL;
}
int gfx_width(void) { return 640; }
int gfx_height(void) { return 480; }
bool autopilot_force_button(enum input_button b, bool* out) {
	(void)b; (void)out; return false;
}
bool menu_autopilot_active(void) {
	return false; /* no walker: the unowned player-1 frame keeps its join prompt */
}

/* SEAM S1 (mp_chat_login_3m, source/game/chat_login_script.h): input.c calls
 * these three predicates at its two shared seam sites. The driver itself is
 * not linked here -- this target deliberately keeps a minimal object set -- so
 * these are link-only stubs returning the DISARMED answer, which is exactly
 * the behaviour this test is meant to measure: today's input.c, untouched. */
bool chat_login_pad_for(unsigned player, int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION],
						const size_t* lengths, struct chat_login_pad* out) {
	(void)player; (void)map; (void)lengths; (void)out;
	return false;
}
bool chat_login_pad_key(void* user, int key) {
	(void)user; (void)key;
	return false;
}
bool chat_login_pad_native_key(int key, bool* value) {
	(void)key; (void)value;
	return false;
}
bool chat_login_pad_inventory(struct input_device device,
							  struct input_identity* identity, bool* connected,
							  bool* fresh, bool* neutral) {
	(void)device; (void)identity; (void)connected; (void)fresh; (void)neutral;
	return false;
}

bool menu_autopilot_force_button(enum input_button b, bool* out) {
	(void)b; (void)out; return false;
}
bool menu_autopilot_force_pointer(float* x, float* y, float* angle, bool* valid) {
	(void)x; (void)y; (void)angle; (void)valid; return false;
}
static bool script_owns_pointer;
static bool test_pointer_script(float* x, float* y, float* angle, bool* valid) {
	(void)x; (void)y; (void)angle;
	if(!script_owns_pointer) return false;
	*valid = false;
	return true;
}
struct tex_gfx texture_controller_pictures, texture_approved_controls, texture_gui_crafting;
struct tex_gfx texture_controls, texture_font, texture_terrain, texture_gui_widgets, texture_gui_chest, texture_pointer, texture_menu_background;
static bool texture_enabled = true;
void gfx_blending(enum gfx_blend mode) { (void)mode; } /* hint strip / card A set MODE_BLEND */
void gfx_texture(bool enabled) { texture_enabled = enabled; }
uint8_t tex_atlas_lookup(enum tex_atlas_entry e) { (void)e; assert(false); return 0; }
void tex_gfx_lookup(struct tex_gfx* t, int x, int y, uint8_t* color) {
	(void)t; (void)x; (void)y;
	memset(color, 255, 4); /* Fixed font widths for headless text capture. */
}
struct item* item_get(struct item_data* item) { (void)item; assert(false); return NULL; }
void gfx_depth_range(float a, float b) { (void)a; (void)b; assert(false); }
void gfx_write_buffers(bool a, bool b, bool c) {
	(void)a; (void)b; (void)c; assert(false);
}
static struct tex_gfx* bound;
static int cells[8], cell_count;
static unsigned solid_quads;
static int button_rows[8], button_quads;
static char drawn[1024];
static size_t drawn_count;
void gfx_bind_texture(struct tex_gfx* t) { bound = t; }
void gfx_draw_quads(size_t count, const int16_t* v, const uint8_t* color,
	const uint16_t* uv) {
	(void)color;
	assert(count == 4);
	if(!texture_enabled) { solid_quads++; return; }
	if(bound == &texture_gui_widgets) {
		assert(button_quads < 8); button_rows[button_quads++] = uv[1];
	} else if(bound == &texture_controls) {
		assert(cell_count < 8);
		cells[cell_count++] = uv[0] / 32 + uv[1] / 32 * 8;
	} else {
		if(bound != &texture_font) { assert(bound == &texture_gui_crafting || bound == &texture_controller_pictures || bound == &texture_gui_chest || bound == &texture_gui_widgets || bound == &texture_pointer || !bound); return; }
		if(v[2] == -1) { /* Face only; ignore the text shadow. */
			assert(drawn_count + 1 < sizeof(drawn));
			drawn[drawn_count++] = uv[0] / 16 + uv[1] / 16 * 16;
			drawn[drawn_count] = '\0';
		}
	}
}
void gfx_draw_quads_flt(size_t count, const float* v, const uint8_t* color,
	const float* uv) {
	(void)count; (void)v; (void)color; (void)uv;
	assert(false); /* Controller settings update tests never render. */
}

static void clear_draw(void) {
	button_quads = solid_quads = 0;
	cell_count = drawn_count = 0;
	drawn[0] = '\0';
}
static void reset(void) {
	memset(wpad, 0, sizeof(wpad));
	memset(pad, 0, sizeof(pad));
	pad_probe_model = false;
	pad_resets = pad_reset_union = pad_reset_last = 0;
	/* The premise every gameplay case here shares: a world is loaded and not
	 * being replaced. input.c clears the sneak toggle while that is not true
	 * (Astra ruling 3 D3's world/session clear), so the fixture has to say so
	 * rather than leave a zeroed gstate implying "no world". ACCEPTED by the
	 * lead (brief 07 return): the sneak clear needs a loaded world as its
	 * premise, so stating it here is the fixture being honest, not a
	 * workaround. */
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	now = scans = 0;
	cues = battery_cues = battery_rearms = 0;
	input_init();
	input_poll();
}
static WPADData report(u32 type, u32 buttons) {
	return (WPADData) {.err = WPAD_ERR_NONE,
		.data_present = WPAD_DATA_BUTTONS | WPAD_DATA_EXPANSION,
		.exp = {.type = type}, .btns_h = buttons};
}
static void enqueue(unsigned c, WPADData data) {
	assert(c < 4 && wpad[c].count < 8);
	wpad[c].queue[wpad[c].count++] = data;
}
static void tick(unsigned ms) {
	now += ms;
	unsigned before = scans;
	input_poll();
	assert(scans == before + 1);
}
static const struct input_device_status* status(unsigned c) {
	return &input_devices()->devices[c];
}
static unsigned notices(void) {
	unsigned count = 0;
	for(size_t i = 0; i < INPUT_NOTICE_COUNT; i++)
		count += input_devices()->notices[i].active;
	return count;
}
static void connect_wpad(unsigned c, u32 type) {
	wpad[c].attached = true;
	wpad[c].type = type;
	enqueue(c, report(type, 0));
	tick(1);
	assert(status(c)->armed);
}
static void claim_wpad(unsigned c, u32 type) {
	connect_wpad(c, type);
	enqueue(c, report(type, type == WPAD_EXP_CLASSIC ?
		WPAD_CLASSIC_BUTTON_A : WPAD_BUTTON_A));
	tick(1);
	assert(input_player_frame(0) && input_player_frame(0)->connected);
	enqueue(c, report(type, 0));
	tick(1);
}

/* `offer` is the join glyph's expected A cell before the claim: the DETECTED
 * device's own (0 Remote+Nunchuk, 10 Classic, 32 GameCube), or -1 when no
 * eligible device is published yet -- then the prompt is its text alone
 * (GLYPH/1, Astra pack-0926 #1: the pack-font "[A]" stand-in is gone). */
static void title_prompts_offer(bool joined, int click, int home, int offer) {
	struct input_player_frame frame;
	input_prompt_frame(0, &frame); /* REAL adapter, never a fabricated frame. */
	assert(frame.join_prompt == !joined);
	if(!joined)
		for(int b = 0; b < IB_COUNT; b++) assert(frame.glyphs[b] == -1);
	clear_draw();
	/* screen_title.c's actual footer call sequence. */
	int x = 32;
	int up = gutil_control_icon_owned(&frame, 480, x, IB_GUI_UP, "Change selection");
	x += up;
	int confirm = gutil_control_icon_owned(&frame, 480, x, IB_GUI_CLICK, "Confirm");
	x += confirm;
	int quit = gutil_control_icon_owned(&frame, 480, x, IB_HOME, "Quit");
	assert(confirm > 0);
	if(joined) {
		assert(up > 0 && quit > 0);
		assert(!strcmp(drawn, "Change selectionConfirmQuit"));
		assert(cell_count == 3 && cells[0] == 24 && cells[1] == click && cells[2] == home);
	} else {
		assert(up == 0 && quit == 0 && cell_count == (offer >= 0));
		assert(offer < 0 || cells[0] == offer);
		assert(!strcmp(drawn, "Press A to join"));
		clear_draw();
		gutil_menu_hint_owned(&frame, 8, 8, IB_GUI_UP, "Change selection");
		gutil_menu_hint_owned(&frame, 8, 8, IB_GUI_CLICK, "Confirm");
		gutil_menu_hint_owned(&frame, 8, 8, IB_HOME, "Quit");
		assert(!strcmp(drawn, "Press A to join") && cell_count == (offer >= 0));
		assert(offer < 0 || cells[0] == offer);
	}
}
static void title_prompts(bool joined, int click, int home) {
	title_prompts_offer(joined, click, home, -1);
}
static void test_join_prompts(void) {
	for(unsigned c = 0; c < 4; c++) {
		for(unsigned classic = 0; classic < 2; classic++) {
			reset();
			title_prompts(false, 0, 0);
			u32 type = classic ? WPAD_EXP_CLASSIC : WPAD_EXP_NUNCHUK;
			connect_wpad(c, type);
			title_prompts(false, 0, 0); /* Presence never implies ownership. */
			tick(INPUT_IDENTITY_STABLE_MS + 50); /* now DETECTED: its own A */
			title_prompts_offer(false, 0, 0, classic ? 10 : 0);
			enqueue(c, report(type, classic ? WPAD_CLASSIC_BUTTON_A : WPAD_BUTTON_A));
			tick(1);
			title_prompts(true, classic ? 10 : 0, 4); /* Same claim frame. */
		}
		reset();
		pad[c].attached = true;
		tick(1);
		title_prompts(false, 0, 0);
		tick(INPUT_IDENTITY_STABLE_MS + 50);
		title_prompts_offer(false, 0, 0, 32);
		pad[c].held = PAD_BUTTON_A;
		tick(1);
		title_prompts(true, 32, 39);
	}
	puts("PASS title: real Wii adapter + shipped config; text-only join "
		 "before detection, the detected device's own A cell before the "
		 "claim, owner glyphs on claim (all families/channels)");
}

static void test_loss_and_retained(void) {
	reset();
	claim_wpad(2, WPAD_EXP_NUNCHUK);
	wpad[2].attached = false;
	tick(1);
	assert(!status(2)->armed && !input_player_frame(0)->connected);
	wpad[2].attached = true; /* WPAD_Data still holds a valid neutral report. */
	tick(300);
	assert(dequeued == 0 && !status(2)->armed);
	assert(!status(2)->identity_reported && !input_player_frame(0)->connected);
	assert(notices() == 0);
	enqueue(2, report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_A));
	tick(1);
	assert(!status(2)->armed && !input_pressed(IB_GUI_CLICK));
	enqueue(2, report(WPAD_EXP_NUNCHUK, 0));
	tick(1);
	assert(status(2)->armed && input_player_frame(0)->connected);
	assert(!input_pressed(IB_GUI_CLICK) && !input_released(IB_GUI_CLICK));
	enqueue(2, report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_A));
	tick(1);
	assert(input_pressed(IB_GUI_CLICK));
	puts("PASS adapter: empty queue retains data but cannot rearm after loss; "
		 "fresh neutral then fresh press recovers");
}

static void test_invalid_and_publication(void) {
	reset();
	claim_wpad(1, WPAD_EXP_CLASSIC);
	WPADData invalid = report(WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_A);
	invalid.err = WPAD_ERR_NOT_READY;
	enqueue(1, invalid);
	tick(1);
	assert(dequeued == 1 && !status(1)->armed && !status(1)->identity_reported);
	assert(!input_player_frame(0)->connected && !input_pressed(IB_GUI_CLICK));
	tick(300);
	assert(dequeued == 0 && notices() == 0 && !status(1)->armed);
	/* A report missing expansion data is invalid even when err == NONE. */
	invalid.err = WPAD_ERR_NONE;
	invalid.data_present = WPAD_DATA_BUTTONS;
	enqueue(1, invalid);
	tick(1);
	assert(!status(1)->armed && !status(1)->identity_reported && notices() == 0);
	enqueue(1, report(WPAD_EXP_CLASSIC, 0));
	tick(1);
	assert(status(1)->armed && status(1)->identity_reported && notices() == 1);
	assert(status(1)->published.category == INPUT_CAT_CLASSIC_CONTROLLER);
	tick(300);
	assert(dequeued == 0 && notices() == 1);
	puts("PASS adapter: invalid and partial reports suppress input/publication; "
		 "retained frames do not invent report validity");
}

static void test_identity_reversal(void) {
	reset();
	claim_wpad(3, WPAD_EXP_NUNCHUK);
	tick(250);
	assert(notices() == 1);
	wpad[3].type = WPAD_EXP_CLASSIC;
	enqueue(3, report(WPAD_EXP_CLASSIC, 0));
	tick(1);
	assert(notices() == 1);
	wpad[3].type = WPAD_EXP_NUNCHUK;
	enqueue(3, report(WPAD_EXP_NUNCHUK, 0));
	tick(249);
	tick(251);
	assert(notices() == 1 && status(3)->published.category == INPUT_CAT_NUNCHUK);
	/* Both identity changes inside one dequeue must also interrupt/rearm. */
	enqueue(3, report(WPAD_EXP_CLASSIC, 0));
	enqueue(3, report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_A));
	tick(1);
	assert(dequeued == 2 && !status(3)->armed && notices() == 1);
	assert(!input_pressed(IB_GUI_CLICK));
	puts("PASS adapter: identity reversal below 250 ms emits no notice; "
		 "intra-frame reversal suppresses held input");
}

static void test_coalesced_edges(void) {
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	enqueue(0, report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_A));
	enqueue(0, report(WPAD_EXP_NUNCHUK, 0));
	enqueue(0, report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_B));
	tick(1);
	assert(dequeued == 3);
	assert(input_pressed(IB_GUI_CLICK) && input_pressed(IB_GUI_CLICK_ALT));
	assert(input_pressed(IB_GUI_CLICK) && !input_released(IB_GUI_CLICK));
	tick(1); /* Empty queue preserves coalesced levels, never repeats edges. */
	assert(dequeued == 0 && input_held(IB_GUI_CLICK) && !input_pressed(IB_GUI_CLICK));
	enqueue(0, report(WPAD_EXP_NUNCHUK, 0));
	tick(1);
	assert(input_released(IB_GUI_CLICK) && input_released(IB_GUI_CLICK_ALT));
	tick(1);
	assert(!input_released(IB_GUI_CLICK) && !input_released(IB_GUI_CLICK_ALT));
	puts("PASS adapter: coalesced A tap + B press survive one poll, "
		 "queries are idempotent, release occurs once");
}

static void assert_container_actions(bool close, bool split, bool quick) {
	const struct input_player_frame* frame = input_player_frame(0);
	struct container_input primary, owned;
	assert(frame && frame->connected);
	container_menu_read_input(&primary, 0, 0, 640, 480);
	container_menu_read_player(&owned, frame, 0, 0, 640, 480);
	assert(primary.close == close && primary.split == split && primary.quick_move == quick);
	assert(owned.close == close && owned.split == split && owned.quick_move == quick);
}

static void test_original_container_bindings(void) {
	const struct {
		u32 type, split, inventory, quick;
		int split_glyph, inventory_glyph, quick_glyph;
	} wpad_cases[] = {
		{WPAD_EXP_NUNCHUK, WPAD_BUTTON_B, WPAD_BUTTON_1,
			WPAD_NUNCHUK_BUTTON_Z, 1, 2, 8},
		{WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_B, WPAD_CLASSIC_BUTTON_X,
			WPAD_CLASSIC_BUTTON_Y, 11, 12, 13},
	};
	for(size_t k = 0; k < sizeof(wpad_cases) / sizeof(wpad_cases[0]); k++) {
		reset(); claim_wpad(0, wpad_cases[k].type);
		enqueue(0, report(wpad_cases[k].type, 0)); tick(1);
		const struct input_player_frame* frame = input_player_frame(0);
		assert(frame->glyphs[IB_GUI_SPLIT] == wpad_cases[k].split_glyph);
		assert(frame->glyphs[IB_INVENTORY] == wpad_cases[k].inventory_glyph);
		assert(frame->glyphs[IB_GUI_QUICK_MOVE] == wpad_cases[k].quick_glyph);
		enqueue(0, report(wpad_cases[k].type, wpad_cases[k].split)); tick(1);
		assert(input_pressed(IB_GUI_SPLIT) && input_pressed(IB_GUI_CLICK_ALT));
		assert_container_actions(false, true, false);
		enqueue(0, report(wpad_cases[k].type, 0)); tick(1);
		enqueue(0, report(wpad_cases[k].type, wpad_cases[k].quick)); tick(1);
		assert_container_actions(false, false, true);
		enqueue(0, report(wpad_cases[k].type, 0)); tick(1);
		enqueue(0, report(wpad_cases[k].type,
			wpad_cases[k].inventory | wpad_cases[k].quick)); tick(1);
		assert_container_actions(true, false, true);
	}

	reset();
	pad[0].attached = true; tick(1);
	pad[0].held = PAD_BUTTON_A; tick(1);
	pad[0].held = 0; tick(1);
	const struct input_player_frame* frame = input_player_frame(0);
	assert(frame && frame->glyphs[IB_GUI_SPLIT] == 33);
	assert(frame->glyphs[IB_INVENTORY] == 35
		&& frame->glyphs[IB_GUI_QUICK_MOVE] == 34);
	pad[0].held = PAD_BUTTON_B; tick(1);
	assert(input_pressed(IB_GUI_SPLIT) && input_pressed(IB_GUI_CLICK_ALT));
	assert_container_actions(false, true, false);
	pad[0].held = 0; tick(1);
	pad[0].held = PAD_BUTTON_X; tick(1);
	assert_container_actions(false, false, true);
	pad[0].held = 0; tick(1);
	pad[0].held = PAD_BUTTON_Y | PAD_BUTTON_X; tick(1);
	assert_container_actions(true, false, true);
	puts("PASS original container controls: A click, B split, 1/X/Y close and "
		 "Z/Y/X Quick Move stay independent on all three Wii controllers");
}

/* Only the inventory storage boundary is fixed; controller reports, detection,
 * prompt-frame selection and the production Sort renderer all run unchanged. */
static struct inventory sort_inventory;
struct inventory* windowc_get_latest(struct window_container* wc) { return &sort_inventory; }
bool inventory_get_picked_item(struct inventory* inv, struct item_data* item) { return false; }
void screen_stats_save_open(unsigned owner, unsigned mask,
 void (*done)(void*, enum stats_exit_result), void* user) {
 assert(owner == 0 && (mask == 1u || mask == 3u)); done(user, STATS_EXIT_SAVED);
}
void achievements_request_rescue(uint8_t slot) { (void)slot; }
bool achievements_rescue_unsaved(uint8_t slot) { assert(slot < 2); return true; }
/* Mutation paths are outside this device/render test and fail if reached. */
struct screen screen_ingame, screen_load_world, screen_title, screen_options, screen_chat, screen_texturepacks, screen_achievements, screen_inventory;
/* IMP-QUIT-UNSAVABLE: screen_pause.c opens the confirm dialog only on an
 * unsavable-chunk refusal, which no case here produces. */
struct screen screen_confirm;
void screen_confirm_setup(const char* msg1, const char* msg2,
	const char* yes_label, const char* no_label,
	enum screen_confirm_answer safe_answer, void (*cb)(bool yes)) {
	(void)msg1; (void)msg2; (void)yes_label; (void)no_label;
	(void)safe_answer; (void)cb;
	assert(0 && "no unsavable-chunk refusal in the adapter suite");
}
static unsigned pause_rpcs, unload_rpcs, back_cues;
static bool reject_rpc;
bool lworld_admit_gameplay(void) { return true; }
/* This adapter fixture covers recovery; the real container rebase is exercised
 * with installed window mirrors by test_recipe_ui. */
bool local_menu_host_open(struct client_session* s,unsigned p,uint8_t w,int width,int height){(void)s;(void)p;(void)w;(void)width;(void)height;return false;}
/* screen_set retires the logical inventory open; this device-only fixture
 * does not link local_menu_host.c. The real close is covered by ACH-1b. */
void local_menu_host_inventory_closed(struct client_session* s) {
 if(s)s->menu_host.inventory_open=false;
}
void screen_options_set_return(struct screen* s) { (void)s; assert(false); }
bool screen_options_from_pause(void) { return false; }
void screen_texturepacks_set_return(struct screen* s) { (void)s; assert(false); }
void screen_texturepacks_set_ingame(bool on) { (void)on; assert(false); }
void audio_play_ui(uint16_t sound, float volume, float pitch) {
	(void)sound; (void)volume; (void)pitch;
}
bool svin_rpc_send(struct server_rpc* rpc) {
	if(reject_rpc) return false;
	if(rpc->type == SRPC_TOGGLE_PAUSE) pause_rpcs++;
	else if(rpc->type == SRPC_UNLOAD_WORLD) unload_rpcs++;
	else assert(false);
	return true;
}
void audio_menu_event(enum menu_sound_event e, ptime_t t) {
	(void)t;
	if(e == MENU_SOUND_BACK) back_cues++;
}
/* screen_pause.c's remote "Leave game" sequence (2026-09-12 D2b). This target
 * links screen_pause.c without client_interface.c, so the three calls need
 * doubles; the pause cases here are all single-player
 * (gstate.remote_session stays false), so reaching one is the regression.
 * tests/test_pause_approved_actions.c owns the remote assertion. */
void clin_update_drain(void) { assert(false); }
void clin_force_world_reset(void) { assert(false); }
bool clin_send_unload_world(void) { assert(false); return false; }
void windowc_sort_interrupt(struct window_container* wc) { assert(false); }
bool windowc_actions_pending(const struct window_container* wc) { assert(false); return false; }
bool windowc_new_action(struct window_container* wc, uint16_t* id, bool right, size_t slot) { assert(false); return false; }
bool windowc_new_quick_action(struct window_container* wc, uint16_t* id, size_t slot) { assert(false); return false; }
bool windowc_new_drop_action(struct window_container* wc, uint16_t* id, bool right) { assert(false); return false; }
void windowc_cancel_unsent(struct window_container* wc, uint16_t id) { assert(false); }
/* critique-1028: container_menu.c's remote Beta close (never reached here). */
void windowc_commit_pending(struct window_container* wc) { assert(false); }
void inventory_clear_picked_item(struct inventory* inv) { assert(false); }
bool inventory_get_slot(struct inventory* inv, size_t slot, struct item_data* item) { assert(false); return false; }
bool client_session_sort_request(struct client_session* s, uint8_t w, unsigned op, bool remote, ptime_t t, container_send_fn send, void* user) { assert(false); return false; }
void client_session_sort_tick(struct client_session* s, uint8_t w, ptime_t t, container_send_fn send, void* user) { assert(false); }

/* Sort parked 2026-10-25 (maintainer: a CavEX addition, not Beta). No device
 * may see a Sort control: it is never supported, never drawn (open or closed,
 * focused or not), on any controller owner. */
static void render_no_sort(struct container_menu* menu, struct window_container* wc) {
	struct input_player_frame frame;
	input_prompt_frame(0, &frame);
	assert(frame.connected);
	assert(!container_menu_sort_supported(wc));
	container_menu_sort_layout(menu, 640, 480, wc);
	for(int open = 0; open < 2; open++) {
		menu->sort_open = open;
		clear_draw();
		container_menu_sort_render(menu, wc, 0, 0, 640, 480, &frame);
		assert(cell_count == 0 && drawn[0] == 0);
	}
}
static void test_sort_parked_every_device(void) {
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	struct container_menu menu = {.sort_focus=true, .selected=7};
	struct window_container chest = {.type=WINDOW_TYPE_CHEST, .slot_count=63};
	struct window_container large = {.type=WINDOW_TYPE_CHEST, .slot_count=90};
	struct window_container inv = {.type=WINDOW_TYPE_INVENTORY, .slot_count=45};
	tick(1);
	render_no_sort(&menu, &chest); render_no_sort(&menu, &large); render_no_sort(&menu, &inv);
	wpad[0].type = WPAD_EXP_CLASSIC;
	enqueue(0, report(WPAD_EXP_CLASSIC, 0)); tick(1);
	render_no_sort(&menu, &chest); render_no_sort(&menu, &large); render_no_sort(&menu, &inv);
	pad[1].attached = true; tick(1);
	assert(input_player_bind(0, (struct input_device){INPUT_DEVICE_GAMECUBE, 1}));
	tick(1);
	render_no_sort(&menu, &chest); render_no_sort(&menu, &large); render_no_sort(&menu, &inv);
	puts("PASS Sort parked: no Sort control drawn for chest/large chest/inventory on Nunchuk, Classic or GameCube");
}

/* Requirement: the approved controller events are edge-triggered from real
 * WPAD/PAD state inside the production poll -- no separate scan, no extra
 * driver call, no per-frame allocation. Everything below drives the SHIPPING
 * input.c through the driver doubles and reads what it published. */
static unsigned events_live(enum controller_event event) {
	unsigned count = 0;
	for(size_t i = 0; i < INPUT_NOTICE_COUNT; i++)
		if(input_devices()->notices[i].active
		   && input_devices()->notices[i].event == event
		   && !time_reached(now, input_devices()->notices[i].expires))
			count++;
	return count;
}
/* Hold one WPAD report for a whole debounce window, a frame at a time. */
static void feed(unsigned channel, WPADData data, unsigned window_ms) {
	for(unsigned elapsed = 0; elapsed <= window_ms + 200; elapsed += 100) {
		enqueue(channel, data);
		tick(100);
	}
}

static void test_wii_battery_and_pointer_feed(void) {
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	const struct input_device_status* st = status(0);
	/* wiiuse has delivered no STATUS report yet (battery_level == 0), so the
	 * input layer refuses to treat it as a reading at all. */
	assert(!st->battery_known && !st->battery_low);

	/* A healthy pack is adopted silently. wiiuse's full reading is 0xC0. */
	WPADData healthy = report(WPAD_EXP_NUNCHUK, 0);
	healthy.battery_level = 0xC0;
	feed(0, healthy, 300);
	assert(st->battery_known && !st->battery_low && battery_cues == 0);

	/* A low pack, held past the stable window, is exactly one episode with
	 * exactly one attention cue, named with the real device. */
	WPADData low = report(WPAD_EXP_NUNCHUK, 0);
	low.battery_level = 20; /* ~10% of 0xC0 */
	feed(0, low, INPUT_BATTERY_STABLE_MS);
	assert(st->battery_low && battery_cues == 1);
	assert(cue_last == MENU_SOUND_BATTERY_ATTENTION);
	assert(cue_device.kind == INPUT_DEVICE_WPAD && cue_device.channel == 0);
	assert(events_live(CONTROLLER_EVENT_LOW_BATTERY) == 1);
	clear_draw();
	gutil_connection_notices(0, false, 0, 0, 640, 480);
	/* card A (10-31): edge + body + 2 light + 2 dark bevel quads, then the
	 * battery's 6 = 12 (the old flat card had 5) */
	assert(strstr(drawn, "Battery low") && solid_quads == 12);
	/* No periodic chirp while the episode stands. */
	feed(0, low, 6000);
	assert(battery_cues == 1);

	/* A wired GameCube pad never produces one, however long it is polled. */
	reset();
	pad[1].attached = true;
	for(unsigned f = 0; f < 80; f++)
		tick(100);
	assert(!status(5)->battery_known && !status(5)->battery_low);
	assert(battery_cues == 0 && battery_rearms == 0);

	/* Pointer: the doubles never set WPAD_DATA_IR, so an owner that has never
	 * aimed at the sensor bar raises nothing -- no card, no nagging. */
	reset();
	claim_wpad(2, WPAD_EXP_NUNCHUK);
	feed(2, report(WPAD_EXP_NUNCHUK, 0), 4 * INPUT_POINTER_STABLE_MS);
	assert(status(2)->pointer_known && !status(2)->pointer_valid);
	assert(events_live(CONTROLLER_EVENT_POINTER_LOST) == 0);

	WPADData aimed = report(WPAD_EXP_NUNCHUK, 0);
	aimed.data_present |= WPAD_DATA_IR;
	aimed.ir.valid = 1;
	aimed.ir.x = 320;
	aimed.ir.y = 240;
	feed(2, aimed, INPUT_POINTER_STABLE_MS);
	assert(status(2)->pointer_valid);
	unsigned cues_at_aim = cues;
	feed(2, report(WPAD_EXP_NUNCHUK, 0), INPUT_POINTER_LOST_STABLE_MS);
	assert(!status(2)->pointer_valid);
	assert(events_live(CONTROLLER_EVENT_POINTER_LOST) == 1);
	/* "Visual notice only; no audio": the pointer card is silent. */
	assert(cues == cues_at_aim);
	/* Regaining it retires the card instead of raising a second one. */
	feed(2, aimed, INPUT_POINTER_STABLE_MS);
	assert(status(2)->pointer_valid);
	assert(events_live(CONTROLLER_EVENT_POINTER_LOST) == 0);
	assert(cues == cues_at_aim);
	float px, py, pa;
	assert(input_pointer(&px, &py, &pa));
	input_pointer_set_script(test_pointer_script);
	script_owns_pointer = true;
	assert(!input_pointer(&px, &py, &pa));
	script_owns_pointer = false;
	assert(input_pointer(&px, &py, &pa));
	input_pointer_set_script(NULL);
	assert(input_pointer(&px, &py, &pa));
	puts("PASS adapter: battery and pointer events are edge-triggered from the "
		 "real WPAD reports, wired pads report neither, pointer loss is silent");
}

/* Driver -> production poll -> production screen gate -> production drawing.
 * No keyboard fallback and no fabricated neutral/armed/frame flags. */
static void retained_render(struct screen* s, int w, int h) { (void)s; (void)w; (void)h; }
static void recovery_input(unsigned family, u32 type, u32 buttons) {
	if(family == 2) pad[0].held = buttons ? PAD_BUTTON_A : 0;
	else enqueue(0, report(type, buttons));
	tick(1);
}
static void test_recovery_dialog(void) {
	for(unsigned classic = 0; classic < 3; classic++) {
		reset();
		u32 type = classic ? WPAD_EXP_CLASSIC : WPAD_EXP_NUNCHUK;
		u32 confirm = classic ? WPAD_CLASSIC_BUTTON_A : WPAD_BUTTON_A;
		if(classic == 2) {
			pad[0].attached = true; tick(1);
			pad[0].held = PAD_BUTTON_A; tick(1);
			pad[0].held = 0; tick(300);
		} else {
			claim_wpad(0, type);
			feed(0, report(type, 0), 300);
		}
		struct entity player = {0};
		struct screen menu = {.render2D = retained_render};
		screen_set(&menu);
		gstate.players[0].local_player = &player;
		gstate.players[1].local_player = NULL;
		gstate.paused = false;
		pause_rpcs = unload_rpcs = back_cues = 0;
		assert(!screen_controller_recovery());
		if(classic == 2) pad[0].attached = false; else wpad[0].attached = false; tick(1); tick(300);
		if(classic == 2) tick(300); /* GC unplug: input.c dropout hold, then the inventory's stable window */
		assert(screen_controller_recovery() && gstate.paused && pause_rpcs == 1);
		assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_LOST);
		tick(4000); /* Transient cards have expired: modal must remain. */
		assert(screen_controller_recovery());
		clear_draw(); menu.render2D(&menu, 640, 480);
		assert(strstr(drawn, "Reconnect your controller."));
		assert(!strstr(drawn, "Resume") && !strstr(drawn, "Save and quit"));
		assert(button_quads == 0);
		assert(gstate.players[0].current_screen == &menu);
		/* Reconnect held. The real adapter must keep it unarmed. */
		if(classic == 2) pad[0].attached = true; else wpad[0].attached = true;
		if(classic == 2) { pad[0].held = PAD_BUTTON_A; tick(1); tick(300); }
		else feed(0, report(type, confirm), 300);
		assert(!status(classic == 2 ? 4 : 0)->armed);
		assert(screen_controller_recovery());
		assert(gstate.paused && pause_rpcs == 1 && back_cues == 0);
		assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_NEUTRAL);
		clear_draw(); menu.render2D(&menu, 640, 480);
		assert(strstr(drawn, "Release buttons and centre the stick."));
		recovery_input(classic, type, 0);
		assert(screen_controller_recovery());
		assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_READY);
		assert(gstate.paused && !back_cues);
		clear_draw(); menu.render2D(&menu, 640, 480);
		/* A4 card (10-31, approved opus disc_card): Resume is a flat grey
		 * button with a white focus ring -- no gui.png widget quads; the
		 * card body + bevel + ring + button are solid quads. */
		assert(button_quads == 0 && solid_quads >= 8);
		assert(strstr(drawn,"Resume") && !strstr(drawn,"Save and quit"));
		/* A second bound controller still cannot operate P1's modal. */
		connect_wpad(1, WPAD_EXP_CLASSIC);
		assert(input_player_bind(1, (struct input_device){INPUT_DEVICE_WPAD, 1}));
		enqueue(1, report(WPAD_EXP_CLASSIC, 0)); tick(1);
		enqueue(1, report(WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_A)); tick(1);
		assert(input_player_frame(1)->pressed[IB_GUI_CLICK]);
		assert(screen_controller_recovery() && gstate.paused && !back_cues);
		/* A rejected resume leaves the visible modal and pause intact. */
		reject_rpc = true;
		recovery_input(classic, type, confirm);
		assert(screen_controller_recovery() && gstate.paused && !back_cues);
		reject_rpc = false;
		recovery_input(classic, type, 0); assert(screen_controller_recovery());
		recovery_input(classic, type, confirm);
		assert(screen_controller_recovery()); /* Resume consumes this frame. */
		assert(!gstate.paused && pause_rpcs == 2 && back_cues == 1);
		assert(gstate.players[0].current_screen == &menu);
		assert(!input_player_frame(0)->pressed[IB_GUI_CLICK]);
		clear_draw(); menu.render2D(&menu, 640, 480);
		assert(!strstr(drawn, "Resume"));
		/* Already-paused menu: deliberate Resume dismisses the recovery only. */
		if(classic == 2) pad[0].attached = true; else wpad[0].attached = true;
		recovery_input(classic, type, 0); tick(300);
		gstate.players[0].local_player = &player;
		gstate.paused = true;
		screen_set(&menu);
		if(classic == 2) pad[0].attached = false; else wpad[0].attached = false;
		tick(1); tick(300);
		if(classic == 2) tick(300); /* GC unplug: dropout hold + stable window */
		assert(screen_controller_recovery());
		unsigned paused_before = pause_rpcs, back_before = back_cues;
		if(classic == 2) pad[0].attached = true; else wpad[0].attached = true;
		recovery_input(classic, type, 0); tick(300);
		assert(screen_controller_recovery());
		recovery_input(classic, type, confirm); assert(screen_controller_recovery());
		assert(gstate.paused && pause_rpcs == paused_before && back_cues == back_before + 1);
		assert(screen_controller_recovery_state()->phase == CONTROLLER_RECOVERY_NONE);
		assert(gstate.players[0].current_screen == &menu);
		assert(unload_rpcs == 0);
		gstate.players[0].local_player = NULL;
		assert(!screen_controller_recovery());
	}
	puts("PASS recovery integration: Wii held/release/press, persistent rendered dialog, retained menu, rejected RPC, one Back cue and no recovery quit action");
}

static void test_unbind_rearm(void) {
 input_player_unbind(0);input_player_unbind(1);
 input_split_join_arm(true);
 connect_wpad(0,WPAD_EXP_NUNCHUK);
 connect_wpad(1,WPAD_EXP_CLASSIC);
 assert(input_player_bind(0,(struct input_device){INPUT_DEVICE_WPAD,0}));
 feed(1,report(WPAD_EXP_CLASSIC,0),300);
 feed(1,report(WPAD_EXP_CLASSIC,WPAD_CLASSIC_BUTTON_A),1);
 assert(input_split_join_claimed() && input_player_frame(1));
 input_split_join_settle(true);
 input_player_unbind(1);
 feed(1,report(WPAD_EXP_CLASSIC,WPAD_CLASSIC_BUTTON_A),300);
 assert(!input_player_frame(1) && !input_split_join_claimed());
 feed(1,report(WPAD_EXP_CLASSIC,0),1);
 feed(1,report(WPAD_EXP_CLASSIC,WPAD_CLASSIC_BUTTON_A),1);
 assert(input_player_frame(1) && input_split_join_claimed());
 input_split_join_arm(false);input_player_unbind(0);input_player_unbind(1);
 puts("PASS guest leave releases the claim; held A cannot rejoin; release then fresh A can");
}
static void test_reclaim_channel(void) {
 input_player_unbind(0);input_player_unbind(1);input_split_join_arm(true);
 connect_wpad(0,WPAD_EXP_NUNCHUK);connect_wpad(1,WPAD_EXP_CLASSIC);
 assert(input_player_bind(0,(struct input_device){INPUT_DEVICE_WPAD,0}));
 assert(input_player_bind(1,(struct input_device){INPUT_DEVICE_WPAD,1}));
 wpad[1].attached=false;tick(1);tick(300);
 unsigned before=configured[2];
 connect_wpad(2,WPAD_EXP_CLASSIC);
 assert(configured[2]>before); /* Fresh reporting must precede a spare's claim. */
 struct input_device d;
 assert(input_player_device(1,&d)&&d.channel==1); /* connection alone never claims */
 feed(2,report(WPAD_EXP_CLASSIC,WPAD_CLASSIC_BUTTON_A),1);
 assert(input_player_device(1,&d)&&d.channel==2);
 assert(input_player_device(0,&d)&&d.channel==0);
 assert(!input_player_frame(1)->pressed[IB_GUI_CLICK]&&!status(2)->armed);
 feed(2,report(WPAD_EXP_CLASSIC,0),1);assert(status(2)->armed);
 /* Both missing: a spare device cannot silently choose a player. */
 wpad[0].attached=false;wpad[2].attached=false;tick(1);tick(300);
 connect_wpad(3,WPAD_EXP_NUNCHUK);feed(3,report(WPAD_EXP_NUNCHUK,WPAD_BUTTON_A),1);
 assert(input_player_device(0,&d)&&d.channel==0);
 assert(input_player_device(1,&d)&&d.channel==2);
 input_split_join_arm(false);input_player_unbind(0);input_player_unbind(1);
 puts("PASS changed-channel recovery: deliberate spare-controller claim, no player theft or ambiguous reassignment");
}
static void test_classic_resting_bias(void) {
 reset();
 connect_wpad(0,WPAD_EXP_CLASSIC);connect_wpad(1,WPAD_EXP_CLASSIC);
 assert(input_player_bind(0,(struct input_device){INPUT_DEVICE_WPAD,0}));
 assert(input_player_bind(1,(struct input_device){INPUT_DEVICE_WPAD,1}));
 for(unsigned p=0;p<2;p++) {
  WPADData sample=report(WPAD_EXP_CLASSIC,0);
  sample.exp.classic.rjs.ang=180;sample.exp.classic.rjs.mag=.125F;
  feed(p,sample,1);
  float dx=99,dy=99;input_player_look(p,1,&dx,&dy);
  assert(dx==0 && dy==0); /* Measured physical idle report. */
  sample.exp.classic.rjs.mag=.5F;feed(p,sample,1);
  input_player_look(p,1,&dx,&dy);
  assert(dy < -.49F && dy > -.51F); /* Deliberate look still proportional. */
  sample.exp.classic.rjs.mag=.125F;feed(p,sample,1);
 }
 input_player_unbind(0);input_player_unbind(1);
 puts("PASS Classic measured -0.125 idle bias is neutral for both owners; deliberate look retained");
}
/* ---- B1 chat opener (maintainer console report 2026-09-19) ---------------
 * "Dpad up control overlaps with existing movement controls, we need to find
 * something not being used for chat purposes." Astra gate 2 D1/D2 plus the
 * maintainer's 13:5x amendment: the opener is BUTTON 2 on the Wii Remote
 * (code 7, TAP to open -- a fresh press armed and released inside
 * INPUT_CHAT_TAP_MS, because a HELD 2 is the pointer-look recenter and must
 * keep being exactly that), PLUS on the Classic (code 212, release-to-open,
 * cancelled by any Minus overlap so the debug chord keeps working) and X on
 * the GameCube pad (code 406, press-triggered). An explicitly configured
 * legacy D-pad Up keeps today's press edge INCLUDING the Left/Right rock
 * refusal. Everything below drives the real PLATFORM_WII input.c and the
 * shipped config_wii.json through the production gate screen_ingame.c calls,
 * once per poll, exactly as the game does. */
#define CHAT_LEGACY_CONFIG "test_chat_opener_legacy.json"
static bool chat_tick(unsigned ms) {
	tick(ms);
	return input_chat_open_edge();
}
static void chat_neutral(unsigned channel, u32 type, unsigned frames) {
	for(unsigned k = 0; k < frames; k++) {
		enqueue(channel, report(type, 0));
		assert(!chat_tick(1));
	}
}
/* Wii Remote: a TAP of 2 opens; anything longer than the tap window is a
 * pointer-look recenter and opens nothing. */
static void test_chat_opener_remote(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	reset();
	claim_wpad(0, T);
	chat_neutral(0, T, 2);

	/* 1. the press alone never opens; the RELEASE of a short tap does, once. */
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(1) && "2 opened chat on its press edge");
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(1) && "a held 2 opened chat");
	enqueue(0, report(T, 0));
	assert(chat_tick(1) && "the 2 tap did not open chat on release");
	chat_neutral(0, T, 2); /* and never a second time */

	/* 2. a hold past the tap window opens nothing, on release or ever. */
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(1));
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(INPUT_CHAT_TAP_MS + 100));
	enqueue(0, report(T, 0));
	assert(!chat_tick(1) && "a long hold of 2 opened chat");
	/* ... and the next fresh tap still works (the release re-armed it). */
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(1));
	enqueue(0, report(T, 0));
	assert(chat_tick(1) && "the gesture never re-armed after a long hold");
	chat_neutral(0, T, 2);

	/* 2b. THE BOUNDARY through the real poll and the injected millisecond
	 * clock (Astra ruling 2 D1): 499 opens, 500 opens -- the rule is
	 * <= INPUT_CHAT_TAP_MS -- and 501 does not. */
	{
		const unsigned held[] = {499u, 500u, 501u};
		const bool opens[] = {true, true, false};
		for(unsigned k = 0; k < 3; k++) {
			enqueue(0, report(T, WPAD_BUTTON_2));
			assert(!chat_tick(1));
			enqueue(0, report(T, 0));
			assert(chat_tick(held[k]) == opens[k]
				   && "the 499/500/501 ms tap boundary moved");
			chat_neutral(0, T, 2);
		}
	}

	/* 3. D-pad Up is released: it is no longer the opener. */
	enqueue(0, report(T, WPAD_BUTTON_UP));
	assert(!chat_tick(1) && "D-pad Up still opens chat by default");
	enqueue(0, report(T, WPAD_BUTTON_UP | WPAD_BUTTON_LEFT));
	assert(!chat_tick(1));
	chat_neutral(0, T, 2);

	/* 4. the Remote's Plus/Minus belong to the HOTBAR, never to chat -- and
	 * under the RELEASE config they no longer form any chord either (E-2,
	 * Astra ergonomics 2), so pressing both reaches nothing but the hotbar. */
	enqueue(0, report(T, WPAD_BUTTON_PLUS));
	assert(!chat_tick(1) && "Plus opened chat on the Remote");
	enqueue(0, report(T, 0));
	assert(!chat_tick(1) && "Plus opened chat on the Remote");
	enqueue(0, report(T, WPAD_BUTTON_MINUS));
	assert(!chat_tick(1));
	enqueue(0, report(T, WPAD_BUTTON_MINUS | WPAD_BUTTON_PLUS));
	assert(!chat_tick(1) && "Minus+Plus opened chat");
	assert(!input_pressed(IB_DEBUG_OVERLAY)
		   && "the release config still toggles F3 from the Remote");
	enqueue(0, report(T, 0));
	assert(!chat_tick(1));
	chat_neutral(0, T, 1);

	/* 5. a screen change (the gate is simply not called that frame) cancels an
	 * armed tap: the release that follows cannot open the composer. */
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(1));
	enqueue(0, report(T, WPAD_BUTTON_2));
	tick(1); /* another screen owned this frame */
	enqueue(0, report(T, 0));
	assert(!chat_tick(1) && "a gesture survived a screen change");
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(1));
	enqueue(0, report(T, 0));
	assert(chat_tick(1));
	chat_neutral(0, T, 1);

	/* 6. the owner drops mid-tap: reconnecting with 2 still held must not
	 * fire, and only a fresh tap after the pad re-arms does. */
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(1));
	wpad[0].attached = false;
	assert(!chat_tick(1));
	assert(!chat_tick(300));
	wpad[0].attached = true;
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(1));
	enqueue(0, report(T, 0));
	assert(!chat_tick(1) && "a gesture survived a disconnect");
	chat_neutral(0, T, 1);
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(1));
	enqueue(0, report(T, 0));
	assert(chat_tick(1));

	puts("PASS chat opener (Wii Remote): a TAP of 2 opens on release, a press "
		 "alone and a recentring hold do not, Plus/Minus and the debug chord "
		 "never open chat, D-pad Up is released");
}

/* 7. the opener may not disturb what a HELD 2 already means: the pointer look
 * recenters while it is down, before AND after the tap window.
 *
 * SCHEME NOTE (2026-09-19, maintainer's CavEX ruling + Astra ruling 3 D4):
 * pointer look is no longer the shipped default -- the CavEX layout moves the
 * camera to the Nunchuk stick -- so this case runs under an EXPLICIT
 * pointer-look preset config instead of config_wii.json. That is the whole
 * point of D4: the alternate preset is a complete binding-plus-mode example,
 * and the tap-vs-recentre overlap only exists inside it. */
#define CHAT_POINTER_LOOK_CONFIG "test_chat_opener_pointer_look.json"
static void test_chat_opener_pointer_look_preset(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	config_destroy(&gstate.config_user);
	FILE* preset = fopen(CHAT_POINTER_LOOK_CONFIG, "w");
	assert(preset);
	/* The preset in miniature: stick MOVES, D-pad cycles the hotbar, the
	 * camera is the IR pointer and a HELD 2 recentres it. */
	fputs("{\"input\":{\"chat\":[7,212,406],"
		  "\"player_forward\":[900,910,930],\"player_backward\":[901,911,931],"
		  "\"player_left\":[902,912,932],\"player_right\":[903,913,933],"
		  "\"scroll_left\":[2,210,402],\"scroll_right\":[3,211,403],"
		  "\"debug_overlay\":[212213,9008,409410]},"
		  "\"controls\":{\"wiimote_ir_look\":1}}", preset);
	fclose(preset);
	assert(config_create(&gstate.config_user, CHAT_POINTER_LOOK_CONFIG));
	reset();
	claim_wpad(0, T);
	chat_neutral(0, T, 2);
	WPADData aimed = report(T, 0);
	aimed.data_present |= WPAD_DATA_IR;
	aimed.ir.valid = 1;
	aimed.ir.x = 320;
	aimed.ir.y = 240;
	for(unsigned k = 0; k < 4; k++) {
		enqueue(0, aimed);
		assert(!chat_tick(16));
	}
	float dx = 99, dy = 99;
	WPADData off = aimed;
	off.ir.x = 600; /* the pointer is pushed well right of centre */
	for(unsigned k = 0; k < 4; k++) {
		enqueue(0, off);
		assert(!chat_tick(16));
		input_player_look(0, .016F, &dx, &dy);
	}
	assert(dx > 0.0F && "the pointer look stopped following the IR reading");
	WPADData recenter = off;
	recenter.btns_h = WPAD_BUTTON_2;
	enqueue(0, recenter);
	assert(!chat_tick(16));
	input_player_look(0, .016F, &dx, &dy);
	assert(dx == 0.0F && dy == 0.0F && "held 2 stopped recentring the look");
	for(unsigned k = 0; k < 4; k++) { /* held past the tap window */
		enqueue(0, recenter);
		assert(!chat_tick(200));
		input_player_look(0, .016F, &dx, &dy);
		assert(dx == 0.0F && dy == 0.0F && "held 2 stopped recentring the look");
	}
	enqueue(0, off);
	assert(!chat_tick(16) && "the recentring hold opened chat");
	config_destroy(&gstate.config_user);
	remove(CHAT_POINTER_LOOK_CONFIG);
	assert(config_create(&gstate.config_user, "config_wii.json"));
	puts("PASS chat opener (pointer-look preset): a held 2 still recentres the "
		 "IR look before and after the tap window, and never opens chat");
}

/* Classic Controller: PLUS opens on its release, any Minus overlap cancels. */
/* Review finding 3: the opener gate FAILS CLOSED. An IB_CHAT edge that no
 * physical key of the OWNING family explains used to pass through, so a
 * latched edge with player 0 unowned -- or owned by another family -- opened
 * the composer. The only code-less sources that may still open it are the two
 * that force the ACTION itself (the headless autopilot and the agent bridge);
 * this fixture forces neither, so every case below must refuse.
 *
 * Finding 6 rides along: Remote 2 carries BOTH IB_CHAT and IB_GUI_DROP, and
 * GameCube X BOTH IB_CHAT and IB_GUI_QUICK_MOVE. That is safe only because
 * the readers are screen-disjoint, so the frame is asserted to show both
 * actions while the GUI consumer (container_menu_read_player, the real one)
 * reports nothing for either press. */
static void test_chat_opener_fails_closed(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	reset();
	claim_wpad(0, T);
	chat_neutral(0, T, 2);

	/* The double binding is real: one button, two actions, one reader. */
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(1));
	const struct input_player_frame* frame = input_player_frame(0);
	assert(frame->down[IB_CHAT] && frame->down[IB_GUI_DROP]
		   && "Remote 2 is bound to both chat and gui_drop");
	struct container_input gui;
	container_menu_read_player(&gui, frame, 0, 0, 640, 480);
	assert(!gui.quick_move && !gui.close
		   && "the opener press reached a container action");
	enqueue(0, report(T, 0));
	assert(chat_tick(1)); /* ... and the tap still opens chat */
	chat_neutral(0, T, 2);

	/* (a) the edge is latched, then NOBODY owns player 0. */
	enqueue(0, report(T, WPAD_BUTTON_2));
	tick(1); /* poll only: the press edge is latched into the merged mask */
	input_player_unbind(0);
	assert(input_pressed(IB_CHAT) && "the latched edge is the premise");
	assert(!input_chat_open_edge()
		   && "an unowned player 0 opened the composer");

	/* (b) the owner is DISCONNECTED when the gate runs. */
	reset();
	claim_wpad(0, T);
	chat_neutral(0, T, 2);
	enqueue(0, report(T, WPAD_BUTTON_2));
	tick(1);
	wpad[0].attached = false;
	tick(1);
	assert(!input_player_frame(0)->connected);
	assert(!input_chat_open_edge()
		   && "a disconnected owner opened the composer");

	/* (c) the owner belongs to ANOTHER family than the code that is down:
	 * a GameCube owner cannot be opened by a Wii Remote's button. */
	reset();
	claim_wpad(0, T);
	chat_neutral(0, T, 2);
	enqueue(0, report(T, WPAD_BUTTON_2));
	tick(1);
	pad[1].attached = true;
	tick(1);
	assert(input_player_bind(0, (struct input_device) {INPUT_DEVICE_GAMECUBE, 1}));
	tick(1);
	assert(!input_chat_open_edge()
		   && "another family's button opened the composer");
	puts("PASS chat opener: an edge no physical key of the owning family "
		 "explains is REFUSED (unowned, disconnected, wrong family); the "
		 "double-bound 2 reaches no container action");
}

/* The Classic's SHIPPED opener is D-pad Up (Astra ruling 4 D1) -- see
 * test_chat_opener_classic_dpad_up. The Plus RELEASE gesture below is kept for
 * anyone who binds Plus explicitly, so this case now drives its own config
 * instead of the shipped one. It has to carry the overlay chord too: nothing
 * falls back for `debug_overlay`, and half of what this case proves is that
 * a cancelled opener still fires that chord. */
#define CHAT_CLASSIC_PLUS_CONFIG "test_chat_opener_classic_plus.json"
static void test_chat_opener_classic(void) {
	const u32 T = WPAD_EXP_CLASSIC;
	const u32 plus = WPAD_CLASSIC_BUTTON_PLUS, minus = WPAD_CLASSIC_BUTTON_MINUS;
	config_destroy(&gstate.config_user);
	FILE* explicit_plus = fopen(CHAT_CLASSIC_PLUS_CONFIG, "w");
	assert(explicit_plus);
	fputs("{\"input\":{\"chat\":[212],"
		  "\"debug_overlay\":[212213,9008,409410]},"
		  "\"controls\":{\"wiimote_ir_look\":0}}", explicit_plus);
	fclose(explicit_plus);
	assert(config_create(&gstate.config_user, CHAT_CLASSIC_PLUS_CONFIG));
	reset();
	claim_wpad(0, T);
	chat_neutral(0, T, 2);

	enqueue(0, report(T, plus));
	assert(!chat_tick(1) && "Classic Plus opened chat on its press edge");
	enqueue(0, report(T, plus));
	assert(!chat_tick(600) && "a held Classic Plus opened chat");
	enqueue(0, report(T, 0));
	assert(chat_tick(1) && "the Classic Plus release did not open chat");
	chat_neutral(0, T, 2);

	enqueue(0, report(T, WPAD_CLASSIC_BUTTON_UP));
	assert(!chat_tick(1)
		   && "D-pad Up opened chat under an EXPLICIT Plus config");
	chat_neutral(0, T, 2);

	/* chord, Plus first: the overlay fires, chat stays shut on the overlap and
	 * on either release order, and both buttons neutral re-arm it. */
	enqueue(0, report(T, plus));
	assert(!chat_tick(1));
	enqueue(0, report(T, plus | minus));
	assert(!chat_tick(1) && "the chord opened chat");
	assert(input_pressed(IB_DEBUG_OVERLAY) && "the Classic chord stopped firing");
	enqueue(0, report(T, plus)); /* Minus lifts first, Plus still held */
	assert(!chat_tick(1) && "a cancelled gesture re-armed while Plus was held");
	enqueue(0, report(T, 0));
	assert(!chat_tick(1) && "the cancelled gesture opened chat on release");
	enqueue(0, report(T, plus));
	assert(!chat_tick(1));
	enqueue(0, report(T, 0));
	assert(chat_tick(1) && "the gesture never re-armed after a chord");
	chat_neutral(0, T, 2);

	/* chord, Minus first (staggered press, Plus released first). */
	enqueue(0, report(T, minus));
	assert(!chat_tick(1));
	enqueue(0, report(T, minus | plus));
	assert(!chat_tick(1) && "Plus into a held Minus opened chat");
	assert(input_pressed(IB_DEBUG_OVERLAY));
	enqueue(0, report(T, minus)); /* Plus lifts first */
	assert(!chat_tick(1) && "the Plus release inside a chord opened chat");
	enqueue(0, report(T, 0));
	assert(!chat_tick(1));
	chat_neutral(0, T, 1);

	/* screen change and disconnect both cancel an armed gesture. */
	enqueue(0, report(T, plus));
	assert(!chat_tick(1));
	enqueue(0, report(T, plus));
	tick(1); /* another screen owned this frame */
	enqueue(0, report(T, 0));
	assert(!chat_tick(1) && "a gesture survived a screen change");
	chat_neutral(0, T, 1);
	enqueue(0, report(T, plus));
	assert(!chat_tick(1));
	wpad[0].attached = false;
	assert(!chat_tick(1));
	assert(!chat_tick(300));
	wpad[0].attached = true;
	enqueue(0, report(T, plus));
	assert(!chat_tick(1));
	enqueue(0, report(T, 0));
	assert(!chat_tick(1) && "a gesture survived a disconnect");
	chat_neutral(0, T, 1);
	enqueue(0, report(T, plus));
	assert(!chat_tick(1));
	enqueue(0, report(T, 0));
	assert(chat_tick(1));
	config_destroy(&gstate.config_user);
	remove(CHAT_CLASSIC_PLUS_CONFIG);
	assert(config_create(&gstate.config_user, "config_wii.json"));
	puts("PASS chat opener (Classic, EXPLICIT Plus config): Plus opens on "
		 "RELEASE, any Minus overlap cancels it and still fires the overlay "
		 "chord, neutral re-arms");
}

/* Astra ruling 4 D1. The Classic opener MOVED to D-pad Up (200) because this
 * branch's own Plus opener was a regression: on that pad sneak is Minus HELD
 * and the F3 chord is Plus+Minus, so chat could not be opened while sneaking
 * -- the chord ate the press. The Classic D-pad is free in gameplay
 * (movement is the left stick, the hotbar is full L/R), so Up costs nothing
 * and reaches everything. */
static void test_chat_opener_classic_dpad_up(void) {
	const u32 T = WPAD_EXP_CLASSIC;
	const u32 up = WPAD_CLASSIC_BUTTON_UP;
	const u32 left = WPAD_CLASSIC_BUTTON_LEFT;
	const u32 right = WPAD_CLASSIC_BUTTON_RIGHT;
	const u32 minus = WPAD_CLASSIC_BUTTON_MINUS;
	const u32 plus = WPAD_CLASSIC_BUTTON_PLUS;
	reset();
	claim_wpad(0, T);
	chat_neutral(0, T, 2);

	/* A plain PRESS edge -- no gesture, no window -- once per press. */
	enqueue(0, report(T, up));
	assert(chat_tick(1) && "Classic D-pad Up did not open chat");
	enqueue(0, report(T, up));
	assert(!chat_tick(1) && "a held Up opened chat twice");
	enqueue(0, report(T, 0));
	assert(!chat_tick(1) && "the Up release opened chat again");
	chat_neutral(0, T, 1);

	/* THE WHOLE POINT: chat opens WHILE SNEAKING and the sneak survives it.
	 *
	 * E-1 (Astra ergonomics 1): sneak on this pad is now Y (207) as a TOGGLE,
	 * not Minus held -- so the thumb is back on the camera stick by the time Up
	 * is pressed, and what has to survive is the LATCHED state rather than a
	 * hold. Up is still the opener for the reason Astra ruling 4 D1 gave, and
	 * the maintainer signed that row off on 2026-09-19. */
	/* The toggle only advances on a poll the GAMEPLAY screen claimed (input.c
	 * input_sneak_apply), so the claim has to start before the press -- exactly
	 * as screen_ingame does it every frame. */
	for(unsigned k = 0; k < 2; k++) {
		enqueue(0, report(T, 0));
		assert(!chat_tick(1));
		input_sneak_gameplay_tick(0, false, false);
	}
	enqueue(0, report(T, WPAD_CLASSIC_BUTTON_Y));
	assert(!chat_tick(1) && "Y opened chat");
	input_sneak_gameplay_tick(0, false, false);
	enqueue(0, report(T, 0));
	assert(!chat_tick(1));
	input_sneak_gameplay_tick(0, false, false);
	assert(input_held(IB_SNEAK) && "a tap of Classic Y did not engage sneak");
	enqueue(0, report(T, up));
	assert(chat_tick(1) && "Up did not open chat while sneaking");
	input_sneak_gameplay_tick(0, false, false);
	assert(input_held(IB_SNEAK) && "opening chat dropped the sneak toggle");
	assert(!input_pressed(IB_DEBUG_OVERLAY) && !input_held(IB_DEBUG_OVERLAY)
		   && "sneak + Up toggled the debug overlay");
	enqueue(0, report(T, 0));
	assert(!chat_tick(1) && "the Up release opened chat again");
	input_sneak_gameplay_tick(0, false, false);
	assert(input_held(IB_SNEAK) && "the sneak toggle ended with the Up release");
	/* ... and Minus, which used to be this pad's sneak, does nothing in
	 * gameplay now. */
	enqueue(0, report(T, minus));
	assert(!chat_tick(1) && "Minus opened chat");
	input_sneak_gameplay_tick(0, false, false);
	assert(input_held(IB_SNEAK) && "Minus disturbed the sneak toggle");
	enqueue(0, report(T, 0));
	assert(!chat_tick(1));
	input_sneak_gameplay_tick(0, false, false);
	enqueue(0, report(T, WPAD_CLASSIC_BUTTON_Y)); /* tap it back off */
	assert(!chat_tick(1));
	input_sneak_gameplay_tick(0, false, false);
	assert(!input_held(IB_SNEAK) && "the second Y tap did not disengage sneak");
	enqueue(0, report(T, 0));
	assert(!chat_tick(1));
	chat_neutral(0, T, 1);

	/* The legacy ROCK GUARD still applies to this D-pad: Up with Left or
	 * Right held on the SAME cluster is refused, and the refusal is not
	 * synthesized back when the Left/Right lifts -- it needs a fresh Up. */
	for(unsigned k = 0; k < 2; k++) {
		u32 rock = k ? right : left;
		enqueue(0, report(T, rock));
		assert(!chat_tick(1));
		enqueue(0, report(T, rock | up));
		assert(!chat_tick(1) && "the Classic D-pad rock refusal is gone");
		enqueue(0, report(T, up));
		assert(!chat_tick(1)
			   && "a refused press was synthesized on the rock release");
		enqueue(0, report(T, 0));
		assert(!chat_tick(1));
		chat_neutral(0, T, 1);
	}
	/* ... and a fresh Up after the refusal still works. */
	enqueue(0, report(T, up));
	assert(chat_tick(1) && "Up stopped opening chat after a refused rock");
	enqueue(0, report(T, 0));
	assert(!chat_tick(1));
	chat_neutral(0, T, 1);

	/* Plus is no longer an opener at all under the shipped config -- neither
	 * on its press nor on its release. */
	enqueue(0, report(T, plus));
	assert(!chat_tick(1) && "Plus opened chat under the shipped config");
	enqueue(0, report(T, 0));
	assert(!chat_tick(1)
		   && "Plus opened chat on release under the shipped config");
	chat_neutral(0, T, 1);
	/* ... and under the RELEASE config Plus+Minus is no longer a chord at all
	 * (E-2): it opens nothing and toggles nothing. */
	enqueue(0, report(T, plus));
	assert(!chat_tick(1));
	enqueue(0, report(T, plus | minus));
	assert(!chat_tick(1) && "Plus+Minus opened chat");
	assert(!input_pressed(IB_DEBUG_OVERLAY)
		   && "the release config still toggles F3 from the Classic pad");
	enqueue(0, report(T, 0));
	assert(!chat_tick(1));

	/* The shipped file IS the binding (the sheet's machine block reads it). */
	{
		int codes[INPUT_BINDINGS_PER_ACTION];
		size_t length = INPUT_BINDINGS_PER_ACTION;
		assert(config_read_int_array(&gstate.config_user, "input.chat", codes,
									 &length));
		assert(length == 3 && codes[0] == 7 && codes[1] == 200
			   && codes[2] == 400
			   && "the shipped chat binding is not 2 / D-pad Up / D-pad Up");
	}
	puts("PASS chat opener (Classic, shipped): D-pad Up opens on its PRESS, "
		 "works while the Y sneak toggle is engaged without toggling F3, still "
		 "refuses the Left/Right rock, and Plus opens nothing");
}

static void test_chat_opener_gamecube_and_legacy(void) {
	/* E-3 (Astra ergonomics 4): the GameCube opener is D-PAD UP (400), because
	 * X became the hotbar NEXT. Press-triggered, once per press, with the same
	 * legacy Left/Right rock guard the Classic D-pad Up opener uses. */
	reset();
	pad[0].attached = true;
	tick(1);
	pad[0].held = PAD_BUTTON_A;
	tick(1);
	pad[0].held = 0;
	assert(!chat_tick(1));
	pad[0].held = PAD_BUTTON_UP;
	assert(chat_tick(1) && "GameCube D-pad Up did not open chat on its press");
	assert(!chat_tick(1) && "a held Up opened chat twice");
	pad[0].held = 0;
	assert(!chat_tick(1) && "the Up release opened chat again");
	pad[0].held = PAD_BUTTON_UP;
	assert(chat_tick(1));
	pad[0].held = 0;
	assert(!chat_tick(1));
	/* The ROCK GUARD: Up while Left or Right steers the same cluster is
	 * refused, and needs a fresh press afterwards. */
	for(unsigned k = 0; k < 2; k++) {
		u16 rock = k ? PAD_BUTTON_RIGHT : PAD_BUTTON_LEFT;
		pad[0].held = rock;
		assert(!chat_tick(1));
		pad[0].held = (u16)(rock | PAD_BUTTON_UP);
		assert(!chat_tick(1) && "the GameCube D-pad rock refusal is gone");
		pad[0].held = PAD_BUTTON_UP;
		assert(!chat_tick(1)
			   && "a refused press was synthesized on the rock release");
		pad[0].held = 0;
		assert(!chat_tick(1));
	}
	pad[0].held = PAD_BUTTON_UP;
	assert(chat_tick(1) && "Up stopped opening chat after a refused rock");
	pad[0].held = 0;
	assert(!chat_tick(1));
	/* ... and X is the HOTBAR now: it opens nothing. */
	pad[0].held = PAD_BUTTON_X;
	assert(!chat_tick(1) && "GameCube X still opens chat");
	pad[0].held = 0;
	assert(!chat_tick(1) && "GameCube X opened chat on its release");

	/* A config that explicitly asks for the legacy D-pad Up keeps today's
	 * behaviour -- press edge, refused while Left or Right rocks the same
	 * cluster. Code never rewrites a user's file. */
	config_destroy(&gstate.config_user);
	FILE* legacy = fopen(CHAT_LEGACY_CONFIG, "w");
	assert(legacy);
	fputs("{\"input\":{\"chat\":[0,200,400]}}", legacy);
	fclose(legacy);
	assert(config_create(&gstate.config_user, CHAT_LEGACY_CONFIG));
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	chat_neutral(0, WPAD_EXP_NUNCHUK, 2);
	enqueue(0, report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_UP));
	assert(chat_tick(1) && "an explicit legacy Up binding stopped opening chat");
	chat_neutral(0, WPAD_EXP_NUNCHUK, 2);
	enqueue(0, report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_UP | WPAD_BUTTON_LEFT));
	assert(!chat_tick(1) && "the D-pad rock refusal is gone");
	enqueue(0, report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_UP));
	assert(!chat_tick(1) && "a refused press was synthesized on the rock release");
	chat_neutral(0, WPAD_EXP_NUNCHUK, 2);
	enqueue(0, report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_2));
	assert(!chat_tick(1) && "2 opened chat under an explicit Up config");
	enqueue(0, report(WPAD_EXP_NUNCHUK, 0));
	assert(!chat_tick(1) && "2 opened chat under an explicit Up config");
	config_destroy(&gstate.config_user);
	remove(CHAT_LEGACY_CONFIG);
	assert(config_create(&gstate.config_user, "config_wii.json"));
	puts("PASS chat opener (GameCube + legacy): D-pad Up opens on press and only "
		 "once per press, refuses the Left/Right rock, and X (now the hotbar) "
		 "opens nothing; an explicit [0,200,400] Remote config is unchanged");
}

static void test_reconnect_reporting(void) {
 reset();
 connect_wpad(1,WPAD_EXP_CLASSIC);
 assert(input_player_bind(1,(struct input_device){INPUT_DEVICE_WPAD,1}));
 unsigned before=configured[1];
 wpad[1].attached=false;tick(1);tick(300);
 assert(!input_player_frame(1)->connected);
 connect_wpad(1,WPAD_EXP_CLASSIC);
 assert(configured[1]>before);
 assert(input_player_frame(1)->connected && status(1)->armed);
 input_player_unbind(1);
 puts("PASS same-channel reconnect reconfigures reports and rearms from fresh neutral input");
}
/* Maintainer bug (41e5752, real Wii): a GameCube pad plugged in AFTER boot
 * was never detected. The probe model makes the double behave like a driver
 * that only finds a hot-plugged pad once its channel is PAD_Reset; the
 * production poll must issue that reset for absent channels only, at a
 * bounded rate, and every GC consumer (inventory, P1 takeover, reconnect)
 * must then see the pad through libogc's 1 << channel scan mask. */
static void test_gc_hotplug_after_boot(void) {
	/* Plug after boot -> P1 takeover from a bound Remote+Nunchuk. */
	reset();
	pad_probe_model = true;
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	for(unsigned k = 0; k < 120; k++) tick(16); /* ~2 s booted, no pad */
	assert(!status(5)->observed.connected);
	pad[1].attached = true; /* plugged in mid-session, buttons neutral */
	tick(16);
	assert(!status(5)->observed.connected); /* not probed yet */
	unsigned frames = 0;
	while(!status(5)->observed.connected) { tick(16); assert(++frames <= 70); } /* <= ~1.1 s */
	assert(pad_reset_last & (PAD_CHAN0_BIT >> 1));
	tick(INPUT_IDENTITY_STABLE_MS + 50);
	assert(status(5)->armed);
	assert(input_player_frame(0)->category == INPUT_CAT_NUNCHUK);
	pad[1].held = PAD_BUTTON_A; tick(16);
	/* Took over; the confirming A is the takeover's, never gameplay: the
	 * owner stays disarmed until the pad is seen neutral again. */
	assert(input_player_frame(0)->category == INPUT_CAT_GAMECUBE);
	pad[1].held = 0; tick(16); tick(INPUT_IDENTITY_STABLE_MS + 50);
	assert(input_player_frame(0)->connected
		   && input_player_frame(0)->category == INPUT_CAT_GAMECUBE);
	puts("PASS GC pad plugged after boot is probed, detected and takes over P1");

	/* Unplug then replug the GC pad that now owns P1. */
	pad[1].attached = false; tick(16); tick(300);
	assert(!status(5)->observed.connected && !input_player_frame(0)->connected);
	pad[1].attached = true;
	frames = 0;
	while(!status(5)->observed.connected) { tick(16); assert(++frames <= 70); } /* <= ~1.1 s */
	tick(INPUT_IDENTITY_STABLE_MS + 50);
	assert(input_player_frame(0)->connected
		   && input_player_frame(0)->category == INPUT_CAT_GAMECUBE);
	puts("PASS GC pad unplug + replug is re-probed and reconnects to its player");

	/* Rate limit + live pads never reset: 10 s with port 1 live, 0/2/3 empty. */
	unsigned before = pad_resets;
	pad_reset_union = 0;
	for(unsigned k = 0; k < 625; k++) tick(16);
	assert(pad_resets - before >= 10 && pad_resets - before <= 21);
	assert(!(pad_reset_union & (PAD_CHAN0_BIT >> 1)));
	assert(pad_reset_union == ((PAD_CHAN0_BIT >> 0) | (PAD_CHAN0_BIT >> 2)
							   | (PAD_CHAN0_BIT >> 3)));
	/* Every frame polled, still at most one reset per 500 ms window. */
	before = pad_resets;
	for(unsigned k = 0; k < 2000; k++) tick(1);
	assert(pad_resets - before <= 5);
	puts("PASS GC re-probe is rate limited (<= 2/s) and never resets a live channel");
}
/* ui1031/p1-takeover (maintainer, real Wii, build 8b32466, IN A WORLD): P1 on
 * a GameCube pad, the pad unplugged, a Wii Remote + Nunchuk picked up and A
 * pressed. In game the split join is armed, so that A started split-screen
 * with the Remote as P2 while the unplugged pad kept P1. A seat whose device
 * is gone is TAKEN by the new pad; a split join needs P1's device present. */
static const char* st_card_target(const char* text, const char* action) {
	static struct controller_card card;
	memset(&card, 0, sizeof(card));
	snprintf(card.text, sizeof(card.text), "%s", text);
	snprintf(card.action_confirm, sizeof(card.action_confirm), "%s", action);
	card.event = CONTROLLER_EVENT_DETECTED;
	controller_card_name_join_target(&card, input_devices());
	return card.action_confirm;
}
static void gc_claim_p1(unsigned c) {
	pad[c].attached = true;
	tick(16);
	tick(INPUT_IDENTITY_STABLE_MS + 50);
	assert(status(4 + c)->armed && status(4 + c)->published.connected);
	pad[c].held = PAD_BUTTON_A; tick(16);
	struct input_device d;
	assert(input_player_device(0, &d) && d.kind == INPUT_DEVICE_GAMECUBE
		   && d.channel == c);
	pad[c].held = 0; tick(16);
}
static void test_vacant_seat_takeover(void) {
	struct input_device d;
	/* (a) in game (split armed), P1 = GC, GC unplugged, Remote+Nunchuk A. */
	reset();
	input_split_join_arm(true);
	gc_claim_p1(0);
	connect_wpad(1, WPAD_EXP_NUNCHUK);
	tick(INPUT_IDENTITY_STABLE_MS + 50);
	assert(!strcmp(st_card_target("New controller", "Join P2"), "Join P2"));
	pad[0].attached = false;
	tick(16); tick(16);
	tick(250); /* the ~200 ms presence hold */
	tick(INPUT_IDENTITY_STABLE_MS + 50); /* and the identity settle */
	assert(!status(4)->published.connected);
	assert(input_devices()->vacant_seat == 1);
	assert(!strcmp(st_card_target("New controller", "Join P2"), "Play as P1"));
	enqueue(1, report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_A));
	tick(16);
	assert(input_player_device(0, &d) && d.kind == INPUT_DEVICE_WPAD
		   && d.channel == 1);
	assert(!input_player_frame(1) && !input_split_join_claimed());
	assert(!input_player_frame(0)->pressed[IB_GUI_CLICK]); /* never gameplay */
	enqueue(1, report(WPAD_EXP_NUNCHUK, 0));
	for(unsigned k = 0; k < 30; k++) tick(16);
	assert(!input_player_frame(1) && !input_split_join_claimed());
	assert(input_devices()->vacant_seat == 0 && input_player_frame(0)->connected);
	assert(!status(4)->published_assigned); /* the GC binding is released */
	/* The GC pad back in is a NEW device: P1 is present, so its A joins P2. */
	pad[0].attached = true;
	tick(16); tick(INPUT_IDENTITY_STABLE_MS + 50);
	assert(status(4)->armed);
	pad[0].held = PAD_BUTTON_A; tick(16);
	assert(input_split_join_claimed() && input_player_device(1, &d)
		   && d.kind == INPUT_DEVICE_GAMECUBE && d.channel == 0);
	assert(input_player_device(0, &d) && d.kind == INPUT_DEVICE_WPAD);
	input_split_join_settle(true);
	pad[0].held = 0; tick(16);
	puts("PASS ui1031 (a): in game, P1's GC pad gone -> Remote+Nunchuk A takes "
		 "P1 (no split, card \"Play as P1\"); the replugged GC pad is new and "
		 "joins P2");

	/* (b) P1's GC pad still connected: the Remote's A is a P2 join. */
	input_split_join_arm(false);
	reset();
	input_split_join_arm(true);
	gc_claim_p1(0);
	connect_wpad(1, WPAD_EXP_NUNCHUK);
	tick(INPUT_IDENTITY_STABLE_MS + 50);
	assert(input_devices()->vacant_seat == 0);
	assert(!strcmp(st_card_target("New controller", "Join P2"), "Join P2"));
	enqueue(1, report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_A));
	tick(16);
	assert(input_split_join_claimed() && input_player_device(1, &d)
		   && d.kind == INPUT_DEVICE_WPAD && d.channel == 1);
	assert(input_player_device(0, &d) && d.kind == INPUT_DEVICE_GAMECUBE);
	input_split_join_settle(true);
	enqueue(1, report(WPAD_EXP_NUNCHUK, 0)); tick(16);
	puts("PASS ui1031 (b): P1's GC pad connected -> the Remote's A joins P2 "
		 "(card \"Join P2\")");

	/* (c) in split, P2's device gone: a new pad's A takes P2, never "P3". */
	tick(INPUT_IDENTITY_STABLE_MS + 50);
	wpad[1].attached = false;
	tick(16); tick(INPUT_IDENTITY_STABLE_MS + 50);
	assert(!status(1)->published.connected);
	assert(input_devices()->vacant_seat == 2);
	assert(!strcmp(st_card_target("No free player slot", ""), "Play as P2"));
	connect_wpad(2, WPAD_EXP_CLASSIC);
	tick(INPUT_IDENTITY_STABLE_MS + 50);
	enqueue(2, report(WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_A));
	tick(16);
	assert(input_player_device(1, &d) && d.kind == INPUT_DEVICE_WPAD
		   && d.channel == 2);
	assert(input_player_device(0, &d) && d.kind == INPUT_DEVICE_GAMECUBE
		   && d.channel == 0);
	assert(!input_player_frame(1)->pressed[IB_GUI_CLICK]);
	enqueue(2, report(WPAD_EXP_CLASSIC, 0));
	for(unsigned k = 0; k < 30; k++) tick(16);
	assert(input_devices()->vacant_seat == 0);
	input_split_join_arm(false);
	input_player_unbind(0); input_player_unbind(1);
	puts("PASS ui1031 (c): in split, P2's device gone -> a new pad's A takes "
		 "P2 (card \"Play as P2\")");
}
/* ---- B1 slice 1: the CavEX default scheme -------------------------------
 * Maintainer, 2026-09-19, in person: "I agree, cavex layout" -- D-pad MOVES,
 * the Nunchuk stick is the CAMERA, B jumps, Z breaks, C places, A opens the
 * inventory, 1 sneaks, 2 chats, Home pauses, and tapping Minus / Plus cycles
 * the hotbar while HOLD MINUS + PRESS PLUS toggles F3. Ratified by Astra
 * ruling 3 D1/D2 (tmp/jobs/b1-chat-opener-0919/astra-ruling-3.raw).
 *
 * Minus and Plus therefore carry BOTH the hotbar and the debug chord, which
 * is what everything below is about:
 *   - a hotbar step fires on the RELEASE of a clean tap (<= INPUT_HOTBAR_TAP_MS)
 *     -- never on the press, never twice, never late after a rejected hold,
 *     and there is no hold-to-repeat on the Remote;
 *   - ANY overlap of the two cancels BOTH pending steps, in either press order
 *     and either release order, and both halves must be seen neutral again
 *     before either can re-arm;
 *   - F3 is specifically HOLD MINUS, THEN PRESS PLUS: the other order -- and
 *     both halves arriving on one poll -- toggle nothing, so toggling the
 *     overlay never moves the hotbar;
 *   - Classic (full L/R) and GameCube (D-pad L/R) keep today's plain press
 *     edge, and their chords keep today's order-free semantics.
 *
 * The rule is DERIVED, never written as "9 and 8": it applies exactly to the
 * pair a family binds to scroll_left/scroll_right when those two codes also
 * form a configured chord with each other. */
#define CAVEX_EMPTY_CONFIG "test_cavex_empty_input.json"
#define CAVEX_LEGACY_CONFIG "test_cavex_legacy_input.json"
static bool hb_left, hb_right;
/* The GUEST's answers (player 1), for the split-screen cases. */
static bool hb_left1, hb_right1;
/* Both gates, once per poll, exactly as screen_ingame.c calls them. */
static void hb_tick(unsigned ms) {
	tick(ms);
	/* screen_ingame passes NULL for the host, which is how main.c calls it. */
	hb_left = input_hotbar_step_edge(0, NULL, IB_SCROLL_LEFT);
	hb_right = input_hotbar_step_edge(0, NULL, IB_SCROLL_RIGHT);
}
/* Two owners, four gates, one poll -- the split-screen frame: screen_ingame's
 * host pass and its owner-1 pass both run inside the SAME input_poll. */
static void hb_tick2(unsigned ms) {
	tick(ms);
	hb_left = input_hotbar_step_edge(0, NULL, IB_SCROLL_LEFT);
	hb_right = input_hotbar_step_edge(0, NULL, IB_SCROLL_RIGHT);
	/* ... and the guest's OWN latched owner frame, as main.c hands it to
	 * screen_ingame_update_owned. */
	hb_left1 = input_hotbar_step_edge(1, input_player_frame(1), IB_SCROLL_LEFT);
	hb_right1 = input_hotbar_step_edge(1, input_player_frame(1), IB_SCROLL_RIGHT);
}
static void hb_report(unsigned channel, u32 type, u32 buttons, unsigned ms) {
	enqueue(channel, report(type, buttons));
	hb_tick(ms);
}
static void hb_neutral(unsigned channel, u32 type, unsigned frames) {
	for(unsigned k = 0; k < frames; k++) {
		hb_report(channel, type, 0, 1);
		assert(!hb_left && !hb_right);
	}
}
/* The split-screen pair: one report per channel, one poll, four answers. */
static void hb_report2(u32 type, u32 host, u32 guest, unsigned ms) {
	enqueue(0, report(type, host));
	enqueue(1, report(type, guest));
	hb_tick2(ms);
}
static void hb_neutral2(u32 type, unsigned frames) {
	for(unsigned k = 0; k < frames; k++) {
		hb_report2(type, 0, 0, 1);
		assert(!hb_left && !hb_right && !hb_left1 && !hb_right1);
	}
}

/* The shipped file IS the scheme (Astra ruling 3 D1): a drift fails by name. */
static void test_shipped_cavex_scheme(void) {
	static const struct {
		const char* key;
		size_t count;
		int codes[3];
	} rows[] = {
		{"input.player_forward", 3, {0, 910, 930}},
		{"input.player_backward", 3, {1, 911, 931}},
		{"input.player_left", 3, {2, 912, 932}},
		{"input.player_right", 3, {3, 913, 933}},
		{"input.player_jump", 3, {5, 204, 404}},
		/* Astra ergonomics 1 (maintainer-accepted): Classic sneak moved off
		 * Minus onto Y, because the right thumb owns the camera stick. */
		{"input.player_sneak", 3, {6, 207, 408}},
		{"input.item_action_left", 3, {100, 209, 410}},
		{"input.item_action_right", 3, {101, 208, 409}},
		/* THE ROLE SPLIT (brief 07 C, lead ruling). One button role used to
		 * mean both "open the inventory from gameplay" and "Close / Back /
		 * Cancel in a screen", which is why the CavEX ruling's A collided:
		 * the approved 2026-09-10 matrix keeps Close on Remote 1 while A is
		 * Select, so a single A press in a chest would have picked a slot AND
		 * closed the window. input.inventory therefore STAYS the Close role
		 * on every screen (66 call sites untouched) and the gameplay open
		 * gets its own role. Classic X / GameCube Y carry both, exactly as
		 * they do today. */
		{"input.inventory", 3, {6, 206, 407}},
		{"input.open_inventory", 3, {4, 206, 407}},
		/* E-3 (Astra ergonomics 4): the GameCube hotbar moved off the D-pad
		 * onto B / X, so the left thumb can keep retreating while the right
		 * thumb selects. */
		{"input.scroll_left", 3, {9, 210, 405}},
		{"input.scroll_right", 3, {8, 211, 406}},
		/* Astra ruling 4 D1: the Classic opener is D-pad Up (200), not Plus
		 * (212) -- Plus could not be reached while Minus-sneak was held. */
		/* ... and the opener X -> D-pad Up, the same press edge and rock
		 * guard the Classic D-pad Up opener uses. */
		{"input.chat", 3, {7, 200, 400}},
		{"input.open_menu", 3, {10, 214, 411}},
		/* NO gameplay drop on ANY Wii controller (Astra ergonomics 4 + 6):
		 * the held stack is dropped from the inventory screen (gui_drop). */
		{"input.player_drop", 0, {0, 0, 0}},
		/* E-2 (Astra ergonomics 2): the F3 chords left the release config.
		 * An EXPLICIT EMPTY ARRAY, so no fallback can restore them. */
		{"input.debug_overlay", 0, {0, 0, 0}},
	};
	for(size_t k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
		int codes[INPUT_BINDINGS_PER_ACTION];
		size_t length = INPUT_BINDINGS_PER_ACTION;
		assert(config_read_int_array(&gstate.config_user, rows[k].key, codes,
									 &length)
			   && "the shipped config lost a CavEX binding");
		assert(length == rows[k].count && "a shipped binding changed length");
		for(size_t j = 0; j < length; j++)
			assert(codes[j] == rows[k].codes[j]
				   && "a shipped CavEX binding code moved");
	}
	/* The CavEX layout is stick-CAMERA + D-pad MOVE, so the pointer-look mode
	 * it replaced must ship OFF (an on flag with these bindings is the hybrid
	 * that broke the card on 09-16). */
	assert(config_read_int(&gstate.config_user, "controls.wiimote_ir_look", 1)
		   == 0 && "the shipped config re-enabled pointer look");
	puts("PASS shipped scheme: config_wii.json carries the maintainer's CavEX "
		 "Wii Remote layout, no gameplay Drop, pointer look off");
}

/* ---- E-2: the F3 chord left the RELEASE configuration --------------------
 *
 * Astra ergonomics 2, accepted by the maintainer: developer tools must not
 * compete with play, so config_wii.json now carries an EXPLICIT EMPTY
 * input.debug_overlay and the chords live in tools/presets/
 * config_wii_developer.json. Two consequences the cases below pin:
 *
 *  1. with no chord pairing Minus and Plus, input_hotbar_chord_pair finds no
 *     double-duty pair and the Remote hotbar takes the PLAIN PRESS EDGE path
 *     (input.c input_hotbar_step_edge) -- no 500 ms window, no release delay,
 *     one step per press;
 *  2. under the DEVELOPER preset every rule ratified on db01e56a is back,
 *     byte for byte: the release tap, the overlap cancel, the neutral re-arm
 *     and the hold-Minus-then-press-Plus order rule.
 *
 * The four cases that exercise (2) are therefore re-pointed at the developer
 * preset rather than deleted -- the rules did not change, the configuration
 * that turns them on did. */
#define CAVEX_DEV_PRESET "tools/presets/config_wii_developer.json"
static void use_developer_preset(void) {
	config_destroy(&gstate.config_user);
	assert(config_create(&gstate.config_user, CAVEX_DEV_PRESET)
		   && "the developer preset must exist and parse");
}
static void use_shipped_config(void) {
	config_destroy(&gstate.config_user);
	assert(config_create(&gstate.config_user, "config_wii.json"));
}

static void test_hotbar_remote_tap(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	const u32 M = WPAD_BUTTON_MINUS, P = WPAD_BUTTON_PLUS;
	use_developer_preset(); /* E-2: these rules are the preset's */
	reset();
	claim_wpad(0, T);
	hb_neutral(0, T, 2);

	/* 1. the press alone never steps; the RELEASE of a clean tap does, once. */
	hb_report(0, T, M, 1);
	assert(!hb_left && !hb_right && "Minus stepped the hotbar on its press edge");
	hb_report(0, T, M, 1);
	assert(!hb_left && !hb_right && "a held Minus stepped the hotbar");
	hb_report(0, T, 0, 1);
	assert(hb_left && !hb_right && "the Minus tap did not step on its release");
	hb_report(0, T, 0, 1);
	assert(!hb_left && !hb_right && "the Minus tap stepped twice");
	hb_report(0, T, P, 1);
	assert(!hb_left && !hb_right && "Plus stepped the hotbar on its press edge");
	hb_report(0, T, 0, 1);
	assert(hb_right && !hb_left && "the Plus tap did not step on its release");
	hb_neutral(0, T, 1);

	/* 2. the tap-duration boundary Astra ruling 3 D2 asks to be pinned:
	 * 499 and 500 step, 501 does not -- the rule is <=.
	 *
	 * Astra ruling 4 D2 added the HITCH TOLERANCE, so the wall clock is only
	 * the deciding term once the game has actually SAMPLED the button often
	 * enough (> INPUT_HOTBAR_HITCH_POLLS down polls). Each case therefore
	 * holds through three polls before releasing -- which is what a 60 Hz
	 * console does in half a millisecond of real time -- and the boundary is
	 * still measured from the ARMING poll. The one/two-poll cases the
	 * tolerance is FOR are pinned in test_hotbar_hitch_tolerance below. */
	assert(INPUT_HOTBAR_TAP_MS == 500u
		   && "the ruled hotbar tap window is 500 ms");
	assert(INPUT_HOTBAR_HITCH_POLLS == 2u
		   && "the ruled hotbar hitch tolerance is 2 observed down polls");
	{
		const unsigned held[] = {499u, 500u, 501u};
		const bool steps[] = {true, true, false};
		for(unsigned k = 0; k < 3; k++) {
			hb_report(0, T, M, 1); /* t0: arms, down poll 1 */
			assert(!hb_left);
			hb_report(0, T, M, 1); /* t0+1: down poll 2 */
			hb_report(0, T, M, 1); /* t0+2: down poll 3 -- tolerance spent */
			assert(!hb_left && !hb_right && "a held Minus stepped while down");
			hb_report(0, T, 0, held[k] - 2u); /* release exactly held[k] later */
			assert(hb_left == steps[k]
				   && "the 499/500/501 hotbar tap boundary moved");
			hb_neutral(0, T, 1);
		}
	}

	/* 3. a REJECTED long hold must never produce a delayed slot change -- not
	 * on its release, not on any later frame -- and there is no repeat. */
	hb_report(0, T, M, 1);
	for(unsigned k = 0; k < 4; k++) {
		hb_report(0, T, M, 200);
		assert(!hb_left && !hb_right && "a held Minus repeated");
	}
	hb_report(0, T, 0, 1);
	assert(!hb_left && !hb_right && "a long Minus hold stepped on release");
	for(unsigned k = 0; k < 4; k++) {
		hb_report(0, T, 0, 100);
		assert(!hb_left && !hb_right && "a rejected hold stepped late");
	}
	hb_report(0, T, M, 1);
	assert(!hb_left);
	hb_report(0, T, 0, 1);
	assert(hb_left && "the gesture never re-armed after a rejected hold");
	hb_neutral(0, T, 1);

	/* 4. ANY overlap cancels BOTH, in every press and release order. */
	{
		const u32 first[] = {M, P, M, P};
		const u32 second[] = {P, M, P, M};
		const u32 lift[] = {P, M, M, P};
		for(unsigned k = 0; k < 4; k++) {
			hb_report(0, T, first[k], 1);
			assert(!hb_left && !hb_right);
			hb_report(0, T, first[k] | second[k], 1);
			assert(!hb_left && !hb_right && "the overlap itself stepped");
			hb_report(0, T, (first[k] | second[k]) & ~lift[k], 1);
			assert(!hb_left && !hb_right
				   && "the first release inside an overlap stepped");
			hb_report(0, T, 0, 1);
			assert(!hb_left && !hb_right
				   && "the second release inside an overlap stepped");
			hb_neutral(0, T, 1);
		}
	}

	/* 5. after an overlap BOTH halves must be seen neutral before either
	 * re-arms -- a finger left on one of them cannot revive the other. */
	hb_report(0, T, M, 1);
	hb_report(0, T, M | P, 1);
	hb_report(0, T, M, 1);	   /* Plus lifts, Minus stays down */
	hb_report(0, T, M | P, 1); /* pressed again, still no re-arm */
	assert(!hb_left && !hb_right);
	hb_report(0, T, P, 1);	   /* Minus lifts, Plus stays down */
	hb_report(0, T, 0, 1);
	assert(!hb_left && !hb_right && "a half re-armed without a neutral frame");
	hb_report(0, T, P, 1);
	assert(!hb_right);
	hb_report(0, T, 0, 1);
	assert(hb_right && "the pair never re-armed after both went neutral");
	hb_neutral(0, T, 1);

	/* 6. a screen change (the gates are simply not called that frame) cancels
	 * an armed tap, exactly as it cancels the chat opener. */
	hb_report(0, T, M, 1);
	enqueue(0, report(T, M));
	tick(1); /* another screen owned this frame */
	hb_report(0, T, 0, 1);
	assert(!hb_left && "a hotbar gesture survived a screen change");
	hb_neutral(0, T, 1);
	hb_report(0, T, M, 1);
	hb_report(0, T, 0, 1);
	assert(hb_left);
	hb_neutral(0, T, 1);
	use_shipped_config();
	puts("PASS hotbar (Wii Remote): a clean Minus/Plus TAP steps on release "
		 "inside 500 ms, a long hold never steps (not even late), any overlap "
		 "cancels both and neutral re-arms");
}

/* ---- Astra ruling 4 D2: the HITCH TOLERANCE -----------------------------
 * A chunk-load stall can swallow every poll between a press and its release,
 * so a real tap arrives as two samples hundreds of milliseconds apart and the
 * wall clock alone would throw it away (review 09 finding 3). A release is
 * therefore ALSO a tap when the button was seen down on at most
 * INPUT_HOTBAR_HITCH_POLLS polls of THAT player. The ruling's own words about
 * the cost are what this case pins: across a stall either a tap or a real hold
 * may produce ONE release step, and what must never step is the hold the game
 * actually SAW -- more than two down polls AND past the window. */
/* E-2 (1): the RELEASE configuration. No chord pairs Minus with Plus, so the
 * Remote hotbar is an ordinary press-edge control again -- which is the whole
 * ergonomic point Astra made: "an overlong press silently does nothing" is a
 * poor rule for a frequent action, and removing the overlap is better than
 * arguing about whether the cutoff should be 400, 500 or 700 ms. */
static void test_hotbar_remote_press_edge(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	const u32 M = WPAD_BUTTON_MINUS, P = WPAD_BUTTON_PLUS;
	use_shipped_config();
	reset();
	claim_wpad(0, T);
	hb_neutral(0, T, 2);

	/* The PRESS steps, immediately, once. */
	hb_report(0, T, M, 1);
	assert(hb_left && !hb_right
		   && "the release config did not step the hotbar on the press edge");
	hb_report(0, T, M, 1);
	assert(!hb_left && !hb_right && "a held Minus repeated");
	hb_report(0, T, 0, 1);
	assert(!hb_left && !hb_right && "the release stepped as well as the press");
	hb_report(0, T, P, 1);
	assert(hb_right && !hb_left && "Plus did not step on its press edge");
	hb_report(0, T, 0, 1);
	assert(!hb_left && !hb_right);

	/* NO 500 ms rule: a hold far past the old tap window still stepped once,
	 * on its press, and never again. */
	hb_report(0, T, M, 1);
	assert(hb_left && "a long press lost its step");
	for(unsigned k = 0; k < 4; k++) {
		hb_report(0, T, M, 400);
		assert(!hb_left && !hb_right && "a held Minus repeated after the window");
	}
	hb_report(0, T, 0, 1);
	assert(!hb_left && !hb_right && "the release of a long hold stepped");
	hb_neutral(0, T, 1);

	/* Both halves down together is no longer a chord: each one is its own
	 * ordinary press, and NOTHING reaches the debug overlay. */
	hb_report(0, T, M, 1);
	assert(hb_left);
	hb_report(0, T, M | P, 1);
	assert(hb_right && !hb_left && "Plus lost its press while Minus was held");
	assert(!input_pressed(IB_DEBUG_OVERLAY) && !input_held(IB_DEBUG_OVERLAY)
		   && "the release config still toggles F3 from the controller");
	hb_report(0, T, 0, 1);
	assert(!hb_left && !hb_right);

	/* The shipped file really is what says so: an EXPLICIT EMPTY ARRAY, which
	 * the parser reports as PRESENT-and-empty (so no fallback runs) rather
	 * than as a missing key. */
	{
		int codes[INPUT_BINDINGS_PER_ACTION];
		size_t length = INPUT_BINDINGS_PER_ACTION;
		assert(config_read_int_array(&gstate.config_user, "input.debug_overlay",
									 codes, &length)
			   && "input.debug_overlay must be PRESENT in the release config");
		assert(length == 0
			   && "the release config still binds an F3 chord");
	}
	/* ... and an OMITTED key is unbound too: IB_DEBUG_OVERLAY has no entry in
	 * input_read_bindings' fallback chain, so there is no chord for a missing
	 * key to restore. Both ways lead to "no F3 from a controller". */
	{
		config_destroy(&gstate.config_user);
		FILE* f = fopen(CAVEX_EMPTY_CONFIG, "w");
		assert(f);
		fputs("{\"input\":{}}", f);
		fclose(f);
		assert(config_create(&gstate.config_user, CAVEX_EMPTY_CONFIG));
		int codes[INPUT_BINDINGS_PER_ACTION];
		size_t length = INPUT_BINDINGS_PER_ACTION;
		assert(!config_read_int_array(&gstate.config_user,
									  "input.debug_overlay", codes, &length)
			   && "an omitted debug_overlay key is not absent");
		reset();
		claim_wpad(0, T);
		hb_neutral(0, T, 2);
		hb_report(0, T, M, 1);
		hb_report(0, T, M | P, 1);
		assert(!input_pressed(IB_DEBUG_OVERLAY) && !input_held(IB_DEBUG_OVERLAY)
			   && "an omitted debug_overlay key restored a fallback chord");
		hb_report(0, T, 0, 1);
		remove(CAVEX_EMPTY_CONFIG); /* the swap below owns the destroy */
	}

	/* The DEVELOPER preset is the same file with that ONE key put back, and
	 * with it the tap rule returns -- proven right here, back to back. */
	use_developer_preset();
	reset();
	claim_wpad(0, T);
	hb_neutral(0, T, 2);
	hb_report(0, T, M, 1);
	assert(!hb_left && !hb_right
		   && "the developer preset stepped on the press edge");
	hb_report(0, T, 0, 1);
	assert(hb_left && "the developer preset lost the release tap");
	hb_neutral(0, T, 1);
	use_shipped_config();
	puts("PASS hotbar (release config): Minus/Plus step on the PRESS edge, one "
		 "step per press, no window and no release delay; nothing on the "
		 "controller reaches F3 with the key empty OR omitted; the developer "
		 "preset brings the tap rule back");
}

static void test_hotbar_hitch_tolerance(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	const u32 M = WPAD_BUTTON_MINUS, P = WPAD_BUTTON_PLUS;
	use_developer_preset(); /* E-2: these rules are the preset's */
	reset();
	claim_wpad(0, T);
	hb_neutral(0, T, 2);

	/* 1. ONE down poll, three seconds of stall: a tap, and it steps. */
	hb_report(0, T, M, 1);
	assert(!hb_left && !hb_right);
	hb_report(0, T, 0, 3000);
	assert(hb_left && !hb_right
		   && "a one-poll tap was lost across a 3000 ms hitch");
	hb_neutral(0, T, 1);

	/* 2. TWO down polls, 800 ms apart: still inside the tolerance. */
	hb_report(0, T, P, 1);
	assert(!hb_right);
	hb_report(0, T, P, 800);
	assert(!hb_right && "the second down poll stepped on its own");
	hb_report(0, T, 0, 800);
	assert(hb_right && !hb_left
		   && "a two-poll tap was lost across an 800 ms hitch");
	hb_neutral(0, T, 1);

	/* 3. THREE down polls past the window: an OBSERVED hold. It never steps,
	 * and it never steps late either. */
	hb_report(0, T, M, 1);
	hb_report(0, T, M, 800);
	assert(!hb_left);
	hb_report(0, T, M, 800);
	assert(!hb_left && "the third down poll of a long hold stepped");
	hb_report(0, T, 0, 1);
	assert(!hb_left && "an observed long hold stepped on its release");
	for(unsigned k = 0; k < 4; k++) {
		hb_report(0, T, 0, 500);
		assert(!hb_left && !hb_right && "an observed long hold stepped late");
	}
	/* ... and the gesture still re-arms afterwards. */
	hb_report(0, T, M, 1);
	hb_report(0, T, 0, 1);
	assert(hb_left && "the gesture never re-armed after an observed hold");
	hb_neutral(0, T, 1);

	/* 4. three down polls INSIDE the window is an ordinary 60 Hz tap: the
	 * tolerance never turns a fast tap into a hold. */
	hb_report(0, T, M, 1);
	hb_report(0, T, M, 16);
	hb_report(0, T, M, 16);
	hb_report(0, T, M, 16);
	assert(!hb_left);
	hb_report(0, T, 0, 16);
	assert(hb_left && "a 64 ms five-poll tap stopped stepping");
	hb_neutral(0, T, 1);

	/* 5. CHORD CANCELLATION still wins during a gap: the overlap cancels both
	 * halves however long the stall was, and neutral is the only way back. */
	hb_report(0, T, M, 1);
	hb_report(0, T, M | P, 3000);
	assert(!hb_left && !hb_right && "the overlap stepped across a hitch");
	hb_report(0, T, M, 3000);
	assert(!hb_left && !hb_right
		   && "the first release inside a hitched overlap stepped");
	hb_report(0, T, 0, 3000);
	assert(!hb_left && !hb_right
		   && "a hitched overlap stepped on its last release");
	hb_neutral(0, T, 1);
	hb_report(0, T, M, 1);
	hb_report(0, T, 0, 1);
	assert(hb_left && "the pair never re-armed after a hitched overlap");
	hb_neutral(0, T, 1);

	/* 6. the CHAT tap is deliberately NOT changed (Astra: "chat remains
	 * unchanged") -- tapping 2 again is a free retry, so it keeps the pure
	 * wall-clock rule and a tap across an 800 ms gap opens nothing. */
	chat_neutral(0, T, 2);
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(1));
	enqueue(0, report(T, 0));
	assert(!chat_tick(800)
		   && "the chat opener grew the hotbar's hitch tolerance");
	chat_neutral(0, T, 2);
	enqueue(0, report(T, WPAD_BUTTON_2));
	assert(!chat_tick(1));
	enqueue(0, report(T, 0));
	assert(chat_tick(10) && "a normal chat tap stopped opening");
	use_shipped_config();
	puts("PASS hotbar hitch tolerance: a tap seen on <= 2 polls steps however "
		 "long the stall, an OBSERVED long hold never steps, a chord overlap "
		 "still cancels across a hitch, and the chat tap is unchanged");
}

/* ---- review 09 finding 1: the SPLIT-SCREEN GUEST ------------------------
 * The gesture used to read player 0's owner frame and buttons, and
 * screen_ingame gated the call on slot 0 -- so the guest kept the plain press
 * edge Astra D2 exists to remove: its Minus stepped a slot before Plus was
 * ever pressed, and a long hold stepped too. Both players now run their own
 * machine on their own device. */
static void test_hotbar_guest(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	const u32 M = WPAD_BUTTON_MINUS, P = WPAD_BUTTON_PLUS;
	use_developer_preset(); /* E-2: these rules are the preset's */
	reset();
	claim_wpad(0, T);
	connect_wpad(1, T);
	assert(input_player_bind(1, (struct input_device) {INPUT_DEVICE_WPAD, 1}));
	hb_report2(T, 0, 0, 1);
	hb_neutral2(T, 2);

	/* 1. the GUEST gets the ruled gesture on its own Remote: no press step, a
	 * release step for a clean tap, and player 0 sees nothing. */
	hb_report2(T, 0, M, 1);
	assert(!hb_left1 && !hb_right1
		   && "the guest's Minus stepped on its press edge");
	assert(!hb_left && !hb_right && "the guest's Minus reached the host");
	hb_report2(T, 0, 0, 1);
	assert(hb_left1 && !hb_right1
		   && "the guest's Minus tap did not step on its release");
	assert(!hb_left && !hb_right && "the guest's tap stepped the host's hotbar");
	hb_neutral2(T, 1);

	/* 2. a guest LONG HOLD never steps -- not on release, not late. */
	hb_report2(T, 0, P, 1);
	for(unsigned k = 0; k < 4; k++) {
		hb_report2(T, 0, P, 200);
		assert(!hb_right1 && "a held guest Plus repeated");
	}
	hb_report2(T, 0, 0, 1);
	assert(!hb_right1 && "a long guest Plus hold stepped on release");
	hb_report2(T, 0, 0, 200);
	assert(!hb_right1 && "a rejected guest hold stepped late");
	hb_neutral2(T, 1);

	/* 3. a guest CHORD OVERLAP cancels both of the guest's pending steps, and
	 * both halves must be neutral again before either re-arms. */
	hb_report2(T, 0, M, 1);
	hb_report2(T, 0, M | P, 1);
	assert(!hb_left1 && !hb_right1 && "the guest's overlap stepped");
	hb_report2(T, 0, P, 1);
	assert(!hb_left1 && !hb_right1
		   && "the first release inside the guest's overlap stepped");
	hb_report2(T, 0, 0, 1);
	assert(!hb_left1 && !hb_right1
		   && "the guest's cancelled pair stepped on the last release");
	hb_report2(T, 0, M, 1);
	hb_report2(T, 0, 0, 1);
	assert(hb_left1 && "the guest's pair never re-armed from neutral");
	hb_neutral2(T, 1);

	/* 4. ISOLATION both ways, inside one poll: the host taps Minus while the
	 * guest taps Plus and each sees only its own. */
	hb_report2(T, M, P, 1);
	assert(!hb_left && !hb_right && !hb_left1 && !hb_right1);
	hb_report2(T, 0, 0, 1);
	assert(hb_left && !hb_right && "the host's own tap stopped stepping");
	assert(hb_right1 && !hb_left1 && "the guest's own tap stopped stepping");
	hb_neutral2(T, 1);

	/* 5. ... and the host's HOLD does not spend the guest's tap: the two
	 * machines keep separate state through the same polls. */
	hb_report2(T, M, M, 1);
	hb_report2(T, M, 0, 1);
	assert(hb_left1 && "the guest's tap was eaten by the host's hold");
	assert(!hb_left && "the host's still-held Minus stepped");
	for(unsigned k = 0; k < 4; k++)
		hb_report2(T, M, 0, 200);
	hb_report2(T, 0, 0, 1);
	assert(!hb_left && "the host's long hold stepped on release");
	input_player_unbind(1);
	use_shipped_config();
	puts("PASS hotbar (split guest): the second Remote runs its OWN release-tap "
		 "gesture -- no press step, no hold step, its own overlap cancel -- and "
		 "neither player's buttons reach the other's hotbar");
}

static void test_debug_chord_order(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	const u32 M = WPAD_BUTTON_MINUS, P = WPAD_BUTTON_PLUS;
	use_developer_preset(); /* E-2: these rules are the preset's */
	reset();
	claim_wpad(0, T);
	hb_neutral(0, T, 2);

	/* 1. the ruled gesture: Minus ALREADY down, then Plus. Once per chord. */
	hb_report(0, T, M, 1);
	assert(!input_pressed(IB_DEBUG_OVERLAY));
	hb_report(0, T, M | P, 1);
	assert(input_pressed(IB_DEBUG_OVERLAY)
		   && "hold Minus + press Plus stopped toggling F3");
	assert(!hb_left && !hb_right && "the F3 chord stepped the hotbar");
	hb_report(0, T, M | P, 1);
	assert(!input_pressed(IB_DEBUG_OVERLAY) && "a held chord fired twice");

	/* 2. both halves must be released before it can fire again. */
	hb_report(0, T, M, 1);
	hb_report(0, T, M | P, 1);
	assert(!input_pressed(IB_DEBUG_OVERLAY)
		   && "the chord re-fired without a neutral frame");
	hb_report(0, T, 0, 1);
	hb_neutral(0, T, 1);
	hb_report(0, T, M, 1);
	hb_report(0, T, M | P, 1);
	assert(input_pressed(IB_DEBUG_OVERLAY)
		   && "the chord never re-armed from neutral");
	hb_report(0, T, 0, 1);
	hb_neutral(0, T, 1);

	/* 3. the WRONG order toggles nothing, and moves no slot either. */
	hb_report(0, T, P, 1);
	hb_report(0, T, P | M, 1);
	assert(!input_pressed(IB_DEBUG_OVERLAY) && !input_held(IB_DEBUG_OVERLAY)
		   && "Plus-then-Minus toggled F3");
	hb_report(0, T, M, 1);
	assert(!input_pressed(IB_DEBUG_OVERLAY));
	hb_report(0, T, 0, 1);
	assert(!hb_left && !hb_right && "the refused chord stepped the hotbar");
	hb_neutral(0, T, 1);

	/* 4. both halves arriving on ONE poll is not "already down" either. */
	hb_report(0, T, M | P, 1);
	assert(!input_pressed(IB_DEBUG_OVERLAY)
		   && "a same-poll Minus+Plus toggled F3");
	hb_report(0, T, 0, 1);
	hb_neutral(0, T, 1);

	/* 5. the whole point (Astra ruling 3 D2): toggling F3 ten times moves the
	 * hotbar zero slots. */
	{
		unsigned toggles = 0, steps = 0;
		for(unsigned k = 0; k < 10; k++) {
			hb_report(0, T, M, 1);
			steps += (hb_left ? 1u : 0u) + (hb_right ? 1u : 0u);
			hb_report(0, T, M | P, 3);
			toggles += input_pressed(IB_DEBUG_OVERLAY) ? 1u : 0u;
			steps += (hb_left ? 1u : 0u) + (hb_right ? 1u : 0u);
			hb_report(0, T, M, 2);
			steps += (hb_left ? 1u : 0u) + (hb_right ? 1u : 0u);
			hb_report(0, T, 0, 2);
			steps += (hb_left ? 1u : 0u) + (hb_right ? 1u : 0u);
			hb_neutral(0, T, 2);
		}
		assert(toggles == 10 && "ten ruled chords did not toggle F3 ten times");
		assert(steps == 0 && "toggling F3 ten times moved the hotbar");
	}
	use_shipped_config();
	puts("PASS F3 chord (Wii Remote): only HOLD MINUS then PRESS PLUS toggles "
		 "the overlay, once per chord, and ten toggles move zero hotbar slots");
}

static void test_hotbar_other_families(void) {
	/* Classic full L/R are NOT the pair that carries the chord (that is Plus +
	 * Minus, 212213), so they keep today's plain press edge -- and the Classic
	 * chord keeps today's order-free semantics in BOTH press orders. */
	const u32 T = WPAD_EXP_CLASSIC;
	const u32 L = WPAD_CLASSIC_BUTTON_FULL_L, R = WPAD_CLASSIC_BUTTON_FULL_R;
	const u32 CP = WPAD_CLASSIC_BUTTON_PLUS, CM = WPAD_CLASSIC_BUTTON_MINUS;
	reset();
	claim_wpad(0, T);
	hb_neutral(0, T, 2);
	hb_report(0, T, L, 1);
	assert(hb_left && !hb_right && "Classic full L stopped stepping on press");
	hb_report(0, T, L, 300);
	assert(!hb_left && "a held Classic full L repeated");
	hb_report(0, T, 0, 1);
	assert(!hb_left && "the Classic full L release stepped again");
	hb_report(0, T, R, 1);
	assert(hb_right && !hb_left && "Classic full R stopped stepping on press");
	hb_report(0, T, 0, 1);
	assert(!hb_right);
	hb_neutral(0, T, 1);
	/* Under the RELEASE config Plus+Minus is not a chord at all any more
	 * (E-2), in either press order. */
	hb_report(0, T, CP, 1);
	hb_report(0, T, CP | CM, 1);
	assert(!input_pressed(IB_DEBUG_OVERLAY)
		   && "the release config still toggles F3 from the Classic pad");
	hb_report(0, T, 0, 1);
	hb_neutral(0, T, 2);

	/* GameCube D-pad L/R: the same plain press edge, its own chord untouched. */
	reset();
	pad[0].attached = true;
	tick(1);
	pad[0].held = PAD_BUTTON_A;
	tick(1);
	pad[0].held = 0;
	hb_tick(1);
	/* E-3: the GameCube hotbar is B / X now, not the D-pad -- still the plain
	 * press edge with no repeat, and the D-pad steps NOTHING. */
	pad[0].held = PAD_BUTTON_B;
	hb_tick(1);
	assert(hb_left && !hb_right && "GameCube B stopped stepping the hotbar");
	pad[0].held = PAD_BUTTON_B;
	hb_tick(300);
	assert(!hb_left && "a held GameCube B repeated");
	pad[0].held = 0;
	hb_tick(1);
	assert(!hb_left && "the GameCube B release stepped again");
	pad[0].held = PAD_BUTTON_X;
	hb_tick(1);
	assert(hb_right && !hb_left && "GameCube X stopped stepping the hotbar");
	pad[0].held = 0;
	hb_tick(1);
	pad[0].held = PAD_BUTTON_LEFT;
	hb_tick(1);
	assert(!hb_left && !hb_right && "the GameCube D-pad still steps the hotbar");
	pad[0].held = PAD_BUTTON_RIGHT;
	hb_tick(1);
	assert(!hb_left && !hb_right && "the GameCube D-pad still steps the hotbar");
	pad[0].held = 0;
	hb_tick(1);
	pad[0].held = PAD_TRIGGER_L;
	hb_tick(1);
	pad[0].held = PAD_TRIGGER_L | PAD_TRIGGER_R;
	hb_tick(1);
	assert(!input_pressed(IB_DEBUG_OVERLAY)
		   && "the release config still toggles F3 from L+R");
	pad[0].held = 0;
	hb_tick(1);

	/* ... and under the DEVELOPER preset both chords are back, order-FREE in
	 * either press order, exactly as they were on db01e56a. Only the Remote's
	 * pair is order-sensitive, because only there do the chord halves double as
	 * the hotbar. */
	use_developer_preset();
	reset();
	claim_wpad(0, T);
	hb_neutral(0, T, 2);
	for(unsigned k = 0; k < 2; k++) {
		u32 first = k ? CM : CP, second = k ? CP : CM;
		hb_report(0, T, first, 1);
		hb_report(0, T, first | second, 1);
		assert(input_pressed(IB_DEBUG_OVERLAY)
			   && "the Classic chord stopped firing under the developer preset");
		hb_report(0, T, 0, 1);
		hb_neutral(0, T, 2);
	}
	reset();
	pad[0].attached = true;
	tick(1);
	pad[0].held = PAD_BUTTON_A;
	tick(1);
	pad[0].held = 0;
	hb_tick(1);
	for(unsigned k = 0; k < 2; k++) {
		u16 first = k ? PAD_TRIGGER_R : PAD_TRIGGER_L;
		u16 second = k ? PAD_TRIGGER_L : PAD_TRIGGER_R;
		pad[0].held = first;
		hb_tick(1);
		pad[0].held = (u16)(first | second);
		hb_tick(1);
		assert(input_pressed(IB_DEBUG_OVERLAY)
			   && "the GameCube chord stopped firing under the developer preset");
		pad[0].held = 0;
		hb_tick(1);
		hb_tick(1);
	}
	use_shipped_config();
	puts("PASS hotbar (Classic + GameCube): full L/R and D-pad L/R keep the "
		 "plain press edge with no repeat; the release config forms no chord at "
		 "all, and the developer preset restores both, order-free");
}

static void test_binding_fallbacks(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	/* An EMPTY "input" block: every key input.c still carries a built-in
	 * default for answers with the shipped CavEX Remote code, and the keys it
	 * has no default for stay UNBOUND -- code never invents a user's config. */
	config_destroy(&gstate.config_user);
	FILE* f = fopen(CAVEX_EMPTY_CONFIG, "w");
	assert(f);
	fputs("{\"input\":{}}", f);
	fclose(f);
	assert(config_create(&gstate.config_user, CAVEX_EMPTY_CONFIG));
	reset();
	claim_wpad(0, T);
	hb_neutral(0, T, 2);
	{
		/* code 7 = 2 -> chat + gui_drop, 5 = B -> gui_split/gui_click_alt,
		 * 100 = Nunchuk Z -> gui_quick_move, 8/9 = Plus/Minus -> the menu
		 * paging pair. All exactly the shipped Remote codes. */
		const struct {
			u32 button;
			enum input_button action;
		} defaults[] = {{WPAD_BUTTON_2, IB_CHAT},
						{WPAD_BUTTON_2, IB_GUI_DROP},
						{WPAD_BUTTON_B, IB_GUI_SPLIT},
						{WPAD_NUNCHUK_BUTTON_Z, IB_GUI_QUICK_MOVE},
						{WPAD_BUTTON_PLUS, IB_GUI_PAGE},
						{WPAD_BUTTON_MINUS, IB_GUI_PREVIOUS}};
		for(size_t k = 0; k < sizeof(defaults) / sizeof(defaults[0]); k++) {
			hb_report(0, T, defaults[k].button, 1);
			assert(input_held(defaults[k].action)
				   && "a built-in Remote fallback left the shipped scheme");
			hb_neutral(0, T, 2);
		}
		/* No fallback exists for the gameplay scheme keys: the shipped config
		 * is their only source, and an empty block leaves them silent. */
		const struct {
			u32 button;
			enum input_button action;
		} unbound[] = {{WPAD_BUTTON_UP, IB_FORWARD}, {WPAD_BUTTON_B, IB_JUMP},
					   {WPAD_BUTTON_1, IB_INVENTORY}, {WPAD_BUTTON_1, IB_SNEAK},
					   {WPAD_BUTTON_MINUS, IB_SCROLL_LEFT},
					   {WPAD_BUTTON_PLUS, IB_SCROLL_RIGHT},
					   {WPAD_NUNCHUK_BUTTON_C, IB_ACTION2}};
		for(size_t k = 0; k < sizeof(unbound) / sizeof(unbound[0]); k++) {
			hb_report(0, T, unbound[k].button, 1);
			assert(!input_held(unbound[k].action)
				   && "input.c invented a binding the config omitted");
			hb_neutral(0, T, 2);
		}
	}

	/* An EXPLICIT legacy config stays authoritative: the old inventory button
	 * still opens the inventory, and the old D-pad hotbar keeps its PRESS edge
	 * -- D-pad Left/Right form no chord with each other, so the release-tap
	 * rule (which is derived, not hard-coded to Minus/Plus) does not apply. */
	config_destroy(&gstate.config_user);
	f = fopen(CAVEX_LEGACY_CONFIG, "w");
	assert(f);
	fputs("{\"input\":{\"inventory\":[6],\"scroll_left\":[2],"
		  "\"scroll_right\":[3],\"debug_overlay\":[9008]}}", f);
	fclose(f);
	assert(config_create(&gstate.config_user, CAVEX_LEGACY_CONFIG));
	reset();
	claim_wpad(0, T);
	hb_neutral(0, T, 2);
	hb_report(0, T, WPAD_BUTTON_1, 1);
	assert(input_pressed(IB_INVENTORY)
		   && "an explicit inventory binding was overwritten");
	hb_neutral(0, T, 2);
	hb_report(0, T, WPAD_BUTTON_LEFT, 1);
	assert(hb_left && "an explicit D-pad hotbar lost its press edge");
	hb_report(0, T, 0, 1);
	assert(!hb_left && "an explicit D-pad hotbar stepped on release too");
	hb_report(0, T, WPAD_BUTTON_RIGHT, 1);
	assert(hb_right);
	hb_report(0, T, 0, 1);
	assert(!hb_right);
	/* Minus/Plus are now the chord ONLY, so a tap of either moves nothing. */
	hb_neutral(0, T, 1);
	hb_report(0, T, WPAD_BUTTON_MINUS, 1);
	hb_report(0, T, 0, 1);
	assert(!hb_left && !hb_right && "Minus stepped a hotbar it is not bound to");

	config_destroy(&gstate.config_user);
	remove(CAVEX_EMPTY_CONFIG);
	remove(CAVEX_LEGACY_CONFIG);
	assert(config_create(&gstate.config_user, "config_wii.json"));
	puts("PASS bindings: the built-in Remote fallbacks match the shipped CavEX "
		 "scheme, omitted keys stay unbound, and an explicit config wins");
}

/* ---- B1 slice 2: the Wii Remote SNEAK TOGGLE ----------------------------
 * Astra ruling 3 D3, through the PRODUCTION input.c and the shipped config.
 * The pure machine is tests/test_sneak_toggle.c; everything here is the part
 * only input.c can answer: which families get the toggle at all, where its
 * level is published, and the clears the machine cannot see for itself
 * (world/session leave, disconnect, owner or family change, scheme-mode
 * change). Sneak is read by entity_local_player.c, camera.c and
 * client_interface.c as the ORDINARY IB_SNEAK level, so the toggle is
 * substituted into that level rather than bolted onto five consumers: what
 * they see is exactly what a held button would have given them. */
#define CAVEX_HOLD_CONFIG "test_cavex_sneak_hold.json"
/* input_poll, then the in-game screen's own tick -- main.c's order. Passing
 * gameplay=false is a frame some OTHER screen owned. */
static void sn_tick(unsigned ms, bool gameplay, bool dead, bool resting) {
	tick(ms);
	if(gameplay)
		input_sneak_gameplay_tick(0, dead, resting);
}
static void sn_report(u32 type, u32 buttons, bool gameplay) {
	enqueue(0, report(type, buttons));
	sn_tick(1, gameplay, false, false);
}
static bool sneaking(void) {
	const struct input_player_frame* f = input_player_frame(0);
	/* BOTH readers must agree: the merged mask that entity_local_player.c and
	 * camera.c use, and the owner frame the split path uses. */
	assert(input_held(IB_SNEAK) == (f && f->down[IB_SNEAK]));
	return input_held(IB_SNEAK);
}

static void test_sneak_toggle_remote(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	const u32 ONE = WPAD_BUTTON_1; /* code 6 -- sneak in the CavEX scheme */
	reset();
	claim_wpad(0, T);
	sn_report(T, 0, true);
	sn_report(T, 0, true);
	assert(!sneaking());

	/* A tap ENGAGES and stays engaged with the button up -- that is the whole
	 * point: the thumb goes back to the D-pad. */
	sn_report(T, ONE, true);
	assert(sneaking() && "a tap of 1 did not engage sneak");
	sn_report(T, ONE, true);
	assert(sneaking() && "a held 1 re-toggled sneak");
	sn_report(T, 0, true);
	assert(sneaking() && "the release of 1 cleared sneak");
	for(unsigned k = 0; k < 5; k++) {
		sn_report(T, 0, true);
		assert(sneaking() && "sneak decayed on its own");
	}
	/* ... and a second tap DISENGAGES. */
	sn_report(T, ONE, true);
	assert(!sneaking() && "the second tap did not disengage sneak");
	sn_report(T, 0, true);
	assert(!sneaking());

	/* PRESERVED through a screen, its input SUPPRESSED there, and the press
	 * that CLOSES the menu (Remote 1 is also the menu Close) cannot toggle. */
	sn_report(T, ONE, true);
	assert(sneaking());
	sn_report(T, 0, true);
	for(unsigned k = 0; k < 3; k++) {
		sn_report(T, 0, false);
		assert(sneaking() && "opening a screen removed ledge protection");
	}
	sn_report(T, ONE, false); /* the Close press, inside the menu */
	assert(sneaking() && "a press inside a screen reached the sneak toggle");
	/* ... and that very press IS the real menu Close (brief 07 C): the role
	 * fires, gameplay does not see it. */
	assert(input_pressed(IB_INVENTORY)
		   && "the menu Close role stopped firing inside a screen");
	sn_report(T, ONE, true); /* gameplay resumes with 1 still down */
	assert(sneaking() && "the press that closed a menu toggled sneak");
	sn_report(T, 0, true); /* neutral re-arms */
	sn_report(T, ONE, true);
	assert(!sneaking() && "the toggle never re-armed after a screen");
	sn_report(T, 0, true);

	/* DEATH clears it, through the gameplay tick's own dead level. */
	sn_report(T, ONE, true);
	assert(sneaking());
	sn_report(T, 0, true);
	tick(1);
	input_sneak_gameplay_tick(0, true, false);
	tick(1);
	assert(!sneaking() && "death did not clear the sneak toggle");

	/* A BED / VEHICLE entry clears it the same way. */
	input_sneak_gameplay_tick(0, false, false);
	sn_report(T, 0, true);
	sn_report(T, ONE, true);
	assert(sneaking());
	sn_report(T, 0, true);
	tick(1);
	input_sneak_gameplay_tick(0, false, true);
	tick(1);
	assert(!sneaking() && "a bed/vehicle entry did not clear the sneak toggle");
	input_sneak_gameplay_tick(0, false, false);
	puts("PASS sneak toggle (Wii Remote): a tap engages and stays engaged, a "
		 "second tap releases it, a screen preserves it while suppressing its "
		 "input, and death / vehicle entry clear it");
}

/* ---- review 09 finding 2: the RIDING LEVEL ------------------------------
 * input_sneak_vehicle_mark is published by the 20 Hz client tick, but the
 * sneak scope used to CONSUME and zero it on every 60 Hz frame poll -- so the
 * level pulsed true/false/false and the pure machine took a fresh ENTRY edge
 * on every tick-frame, re-clearing the toggle forever. A rider could never
 * sneak (Beta's 0.3 rider speed scale was unreachable). The level is now
 * sticky, published both ways by the tick, and the edge fires once. */
#define SN_POLLS_PER_TICK 3u /* 60 Hz frames per 20 Hz client tick */
static void test_sneak_toggle_vehicle_level(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	const u32 ONE = WPAD_BUTTON_1;
	reset();
	claim_wpad(0, T);
	input_sneak_vehicle_mark(0, false); /* the tick's walking path */
	sn_report(T, 0, true);
	sn_report(T, 0, true);
	assert(!sneaking());

	/* On foot the toggle behaves as always. */
	sn_report(T, ONE, true);
	assert(sneaking());
	sn_report(T, 0, true);

	/* BOARDING clears it exactly once, on the level's rising edge. */
	input_sneak_vehicle_mark(0, true);
	sn_report(T, 0, true);
	assert(!sneaking() && "boarding did not clear the sneak toggle");

	/* ABOARD: three frame polls per client tick, the level re-published every
	 * tick. A tap of 1 must engage AND SURVIVE every poll in between. */
	sn_report(T, ONE, true);
	assert(sneaking() && "a rider could not engage the sneak toggle");
	sn_report(T, 0, true);
	for(unsigned t = 0; t < 8; t++) {
		input_sneak_vehicle_mark(0, true); /* the next client tick */
		for(unsigned p = 0; p < SN_POLLS_PER_TICK; p++) {
			sn_report(T, 0, true);
			assert(sneaking()
				   && "the ride re-fired the entry clear between ticks");
		}
	}
	/* ... and a second tap still disengages it while riding. */
	sn_report(T, ONE, true);
	assert(!sneaking() && "a rider could not release the sneak toggle");
	sn_report(T, 0, true);

	/* DISMOUNT: the tick's walking path publishes false. Astra D3 clears on
	 * ENTRY, so leaving is not itself a clear and the toggle survives. */
	sn_report(T, ONE, true);
	assert(sneaking());
	sn_report(T, 0, true);
	for(unsigned t = 0; t < 4; t++) {
		input_sneak_vehicle_mark(0, false);
		for(unsigned p = 0; p < SN_POLLS_PER_TICK; p++) {
			sn_report(T, 0, true);
			assert(sneaking() && "dismounting cleared a toggle it must not");
		}
	}

	/* RE-BOARDING clears once again -- the entry edge re-armed only because
	 * the tick reported not-riding in between. */
	input_sneak_vehicle_mark(0, true);
	sn_report(T, 0, true);
	assert(!sneaking() && "re-boarding did not clear the sneak toggle again");
	for(unsigned p = 0; p < SN_POLLS_PER_TICK; p++)
		sn_report(T, 0, true);
	sn_report(T, ONE, true);
	assert(sneaking() && "the re-boarded rider could not sneak");
	input_sneak_vehicle_mark(0, false);
	sn_report(T, 0, true);
	puts("PASS sneak toggle (vehicles): boarding clears it ONCE, a rider can "
		 "then toggle sneak and keep it across every poll between ticks, "
		 "dismounting keeps it, and a fresh boarding clears it again");
}

static void test_sneak_toggle_clears_from_input_layer(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	const u32 ONE = WPAD_BUTTON_1;

	/* A controller DISCONNECT clears it: a pad that comes back must not
	 * resume a crouch, and the returning button is blocked until neutral. */
	reset();
	claim_wpad(0, T);
	/* TWO neutral gameplay frames: the first poll after a claim was not
	 * preceded by a claimed one, so it is not yet a gameplay frame, and the
	 * machine arms on the first gameplay poll that sees the button up. */
	sn_report(T, 0, true);
	sn_report(T, 0, true);
	sn_report(T, ONE, true);
	assert(sneaking());
	sn_report(T, 0, true);
	wpad[0].attached = false;
	sn_tick(1, true, false, false);
	sn_tick(300, true, false, false);
	assert(!sneaking() && "a disconnect left the sneak toggle engaged");
	wpad[0].attached = true;
	sn_report(T, ONE, true);
	assert(!sneaking() && "a reconnect with 1 held re-engaged sneak");
	sn_report(T, 0, true);
	sn_report(T, ONE, true);
	assert(sneaking() && "the toggle never re-armed after a reconnect");

	/* A WORLD / SESSION leave or replacement clears it -- input.c watches the
	 * world flags, because no in-game frame runs to report it. */
	sn_report(T, 0, true);
	assert(sneaking());
	gstate.world_loaded = false;
	sn_tick(1, true, false, false);
	assert(!sneaking() && "leaving the world left the sneak toggle engaged");
	gstate.world_loaded = true;
	sn_report(T, 0, true);
	sn_report(T, ONE, true);
	assert(sneaking());
	gstate.world_switch_pending = true;
	sn_tick(1, true, false, false);
	assert(!sneaking() && "a world replacement left the sneak toggle engaged");
	gstate.world_switch_pending = false;

	/* A FAMILY change on the same channel clears it (and the Classic family
	 * gets no toggle at all -- see the next case). */
	reset();
	claim_wpad(0, T);
	/* TWO neutral gameplay frames: the first poll after a claim was not
	 * preceded by a claimed one, so it is not yet a gameplay frame, and the
	 * machine arms on the first gameplay poll that sees the button up. */
	sn_report(T, 0, true);
	sn_report(T, 0, true);
	sn_report(T, ONE, true);
	assert(sneaking());
	sn_report(T, 0, true);
	wpad[0].type = WPAD_EXP_CLASSIC;
	sn_report(WPAD_EXP_CLASSIC, 0, true);
	assert(!sneaking() && "a family change left the sneak toggle engaged");

	/* A SCHEME-MODE change goes through input_init (main.c re-reads
	 * controls.* there), which owns a fresh input layer: nothing survives. */
	reset();
	claim_wpad(0, T);
	/* TWO neutral gameplay frames: the first poll after a claim was not
	 * preceded by a claimed one, so it is not yet a gameplay frame, and the
	 * machine arms on the first gameplay poll that sees the button up. */
	sn_report(T, 0, true);
	sn_report(T, 0, true);
	sn_report(T, ONE, true);
	assert(sneaking());
	input_init();
	sn_tick(1, true, false, false);
	assert(!sneaking() && "a scheme-mode change left the sneak toggle engaged");
	puts("PASS sneak toggle: a disconnect, a world leave or replacement, a "
		 "family change and a scheme-mode change each clear it");
}

/* ---- E-1: the sneak TOGGLE on the CLASSIC and the GAMECUBE pad ----------
 *
 * Astra ergonomics study 2026-09-19, recommendations 1 and 3, accepted by the
 * maintainer ("All but 5, and we workshop 5 later"). The toggle used to be a
 * Remote-only carve-out on the reading that "a shoulder button is free there
 * while moving"; the study showed that reading to be wrong for the hands that
 * actually hold those pads:
 *
 *   CLASSIC   -- the right thumb owns the CAMERA STICK, so a held Y means
 *                letting go of the camera exactly at a ledge. Sneak moves to
 *                Y (207) and becomes a toggle.
 *   GAMECUBE  -- the right index finger owns R (break), so a held Z needs a
 *                special grip to mine while sneaking. Z (408) stays put and
 *                becomes a toggle.
 *
 * Each family has its own key, absent = toggle, explicit 0 = Beta's hold. The
 * Remote's own behaviour is untouched -- the cases above this one are the
 * proof, and they were not edited. */
static void sn_gc(unsigned channel, u16 held, bool gameplay) {
	pad[channel].held = held;
	sn_tick(1, gameplay, false, false);
}
static void sn_gc_claim(unsigned channel) {
	pad[channel].attached = true;
	tick(1);
	pad[channel].held = PAD_BUTTON_A;
	tick(1);
	sn_gc(channel, 0, true);
	sn_gc(channel, 0, true);
}

/* Maintainer bug (7fd0d1b, real Wii): "input lag or dropped signal from the
 * GameCube controller". libogc's PAD_ScanPads leaves a PRESENT pad out of its
 * mask on any scan whose PAD_Read answered PAD_ERR_TRANSFER (no new SI poll
 * since the last read -- routine when a frame lands inside one poll period,
 * and around a re-probe's SI traffic). input.c used to read that as an
 * unplug: the inventory disarmed the pad and P1 stayed dead until every
 * control was released. */
static void test_gc_transient_transfer_keeps_input(void) {
	reset();
	pad_probe_model = true;
	for(unsigned k = 0; k < 40; k++) tick(16); /* boot, no pad: re-probes run */
	pad[0].attached = true;
	unsigned frames = 0;
	while(!status(4)->observed.connected) { tick(16); assert(++frames <= 70); }
	tick(INPUT_IDENTITY_STABLE_MS + 50);
	pad[0].held = PAD_BUTTON_A; tick(16);
	pad[0].held = 0; tick(16); tick(16);
	const struct input_player_frame* f = input_player_frame(0);
	assert(f && f->connected && f->category == INPUT_CAT_GAMECUBE);
	assert(status(4)->armed);

	/* Hold B (GUI split) and run 10 s of play: every 3rd scan is a transient
	 * TRANSFER while channels 1-3 stay empty and keep being re-probed. */
	pad[0].held = PAD_BUTTON_B; tick(16);
	assert(input_held(IB_GUI_SPLIT));
	unsigned resets_before = pad_resets;
	pad_reset_union = 0;
	for(unsigned k = 0; k < 625; k++) {
		if(k % 3 == 1) pad[0].transfer = true;
		tick(16);
		assert(status(4)->observed.connected && "transient error read as unplug");
		assert(status(4)->armed && "transient error disarmed the live pad");
		f = input_player_frame(0);
		assert(f && f->connected && f->category == INPUT_CAT_GAMECUBE);
		assert(input_held(IB_GUI_SPLIT) && "held input dropped on a transient error");
	}
	assert(pad_resets > resets_before && "absent channels were never re-probed");
	assert(!(pad_reset_union & PAD_CHAN0_BIT) && "a live pad was PAD_Reset");
	puts("PASS GC transient PAD_ERR_TRANSFER keeps the live pad connected, armed "
		 "and its held input while absent channels are re-probed");

	/* A stale ButtonsDown on a TRANSFER scan is not a second press. */
	pad[0].held = 0; tick(16);
	pad[0].held = PAD_BUTTON_B; tick(16);
	assert(input_pressed(IB_GUI_SPLIT));
	pad[0].transfer = true; tick(16);
	assert(!input_pressed(IB_GUI_SPLIT) && "TRANSFER scan repeated the press");
	assert(input_held(IB_GUI_SPLIT));
	puts("PASS GC TRANSFER scan never repeats the last press edge");

	/* A real unplug is still reported promptly; replug is re-probed. */
	pad[0].held = 0; tick(16);
	pad[0].attached = false;
	frames = 0;
	while(status(4)->observed.connected) { tick(16); assert(++frames <= 20); } /* <= ~320 ms */
	pad[0].attached = true;
	frames = 0;
	while(!status(4)->observed.connected) { tick(16); assert(++frames <= 70); } /* <= ~1.1 s */
	puts("PASS GC real unplug still drops within ~0.3 s and replug is detected within ~1 s");
}

static void test_sneak_toggle_classic_gamecube(void) {
	const u32 C = WPAD_EXP_CLASSIC;
	const u32 Y = WPAD_CLASSIC_BUTTON_Y;
	reset();
	claim_wpad(0, C);
	sn_report(C, 0, true);
	sn_report(C, 0, true);
	assert(!sneaking());

	/* CLASSIC: a tap of Y engages and STAYS engaged with the thumb back on the
	 * camera stick; a second tap releases it. */
	sn_report(C, Y, true);
	assert(sneaking() && "a tap of Classic Y did not engage sneak");
	sn_report(C, Y, true);
	assert(sneaking() && "a held Classic Y re-toggled sneak");
	sn_report(C, 0, true);
	assert(sneaking() && "releasing Classic Y cleared sneak");
	for(unsigned k = 0; k < 4; k++) {
		sn_report(C, 0, true);
		assert(sneaking() && "Classic sneak decayed on its own");
	}
	sn_report(C, Y, true);
	assert(!sneaking() && "the second tap of Classic Y did not disengage");
	sn_report(C, 0, true);

	/* PRESERVED through a screen, SUPPRESSED there, and RELEASE-PROTECTED on
	 * the way back: the Y press that quick-moved a stack must not toggle sneak
	 * when gameplay resumes. */
	sn_report(C, Y, true);
	assert(sneaking());
	sn_report(C, 0, true);
	for(unsigned k = 0; k < 3; k++) {
		sn_report(C, 0, false);
		assert(sneaking() && "opening a screen removed Classic ledge protection");
	}
	sn_report(C, Y, false); /* the quick-move press, inside the menu */
	assert(sneaking() && "a press inside a screen reached the Classic toggle");
	sn_report(C, Y, true); /* gameplay resumes with Y still down */
	assert(sneaking() && "the press that acted in a menu toggled Classic sneak");
	sn_report(C, 0, true);
	sn_report(C, Y, true);
	assert(!sneaking() && "the Classic toggle never re-armed after a screen");
	sn_report(C, 0, true);

	/* CLASSIC MINUS (213) is now unbound in gameplay: it is the menu stack
	 * drop and nothing else. */
	sn_report(C, WPAD_CLASSIC_BUTTON_MINUS, true);
	assert(!sneaking() && "Classic Minus still sneaks");
	sn_report(C, 0, true);

	/* Every clear the Remote already had, on this pad: DEATH ... */
	sn_report(C, Y, true);
	assert(sneaking());
	sn_report(C, 0, true);
	tick(1);
	input_sneak_gameplay_tick(0, true, false);
	tick(1);
	assert(!sneaking() && "death did not clear the Classic toggle");
	input_sneak_gameplay_tick(0, false, false);
	/* ... a VEHICLE / BED entry ... */
	sn_report(C, 0, true);
	sn_report(C, Y, true);
	assert(sneaking());
	sn_report(C, 0, true);
	tick(1);
	input_sneak_gameplay_tick(0, false, true);
	tick(1);
	assert(!sneaking() && "a vehicle entry did not clear the Classic toggle");
	input_sneak_gameplay_tick(0, false, false);
	/* ... a WORLD leave ... */
	sn_report(C, 0, true);
	sn_report(C, Y, true);
	assert(sneaking());
	sn_report(C, 0, true);
	gstate.world_loaded = false;
	sn_report(C, 0, true);
	assert(!sneaking() && "a world leave did not clear the Classic toggle");
	gstate.world_loaded = true;
	/* ... and a DISCONNECT. */
	sn_report(C, 0, true);
	sn_report(C, Y, true);
	assert(sneaking());
	sn_report(C, 0, true);
	wpad[0].attached = false;
	sn_tick(1, true, false, false);
	assert(!sneaking() && "a disconnect did not clear the Classic toggle");

	/* GAMECUBE: the same machine on Z (408). */
	reset();
	sn_gc_claim(0);
	assert(!sneaking());
	sn_gc(0, PAD_TRIGGER_Z, true);
	assert(sneaking() && "a tap of GameCube Z did not engage sneak");
	sn_gc(0, 0, true);
	assert(sneaking() && "releasing GameCube Z cleared sneak");
	sn_gc(0, PAD_TRIGGER_R, true); /* mining while sneaking: the whole point */
	assert(sneaking() && "R broke the GameCube sneak toggle");
	sn_gc(0, 0, true);
	assert(sneaking());
	sn_gc(0, PAD_TRIGGER_Z, true);
	assert(!sneaking() && "the second tap of GameCube Z did not disengage");
	sn_gc(0, 0, true);
	/* preserved through a screen, suppressed there, release-protected after */
	sn_gc(0, PAD_TRIGGER_Z, true);
	assert(sneaking());
	sn_gc(0, 0, true);
	for(unsigned k = 0; k < 3; k++) {
		sn_gc(0, 0, false);
		assert(sneaking() && "opening a screen removed GameCube ledge protection");
	}
	sn_gc(0, PAD_TRIGGER_Z, false); /* the menu stack-drop press */
	assert(sneaking() && "a press inside a screen reached the GameCube toggle");
	sn_gc(0, PAD_TRIGGER_Z, true);
	assert(sneaking() && "the press that dropped a stack toggled GameCube sneak");
	sn_gc(0, 0, true);
	sn_gc(0, PAD_TRIGGER_Z, true);
	assert(!sneaking() && "the GameCube toggle never re-armed after a screen");
	sn_gc(0, 0, true);
	/* death / vehicle / disconnect clear it here too */
	sn_gc(0, PAD_TRIGGER_Z, true);
	assert(sneaking());
	sn_gc(0, 0, true);
	tick(1);
	input_sneak_gameplay_tick(0, true, false);
	tick(1);
	assert(!sneaking() && "death did not clear the GameCube toggle");
	input_sneak_gameplay_tick(0, false, false);
	sn_gc(0, 0, true);
	sn_gc(0, PAD_TRIGGER_Z, true);
	assert(sneaking());
	sn_gc(0, 0, true);
	pad[0].attached = false;
	/* A real unplug is reported after input.c's short dropout hold (a lone
	 * missed scan is a transient PAD_ERR_TRANSFER, not an unplug). */
	sn_tick(1, true, false, false);
	sn_tick(250, true, false, false);
	assert(!sneaking() && "a disconnect did not clear the GameCube toggle");

	/* FAMILY CHANGE: the same physical channel swapping pads clears it, so a
	 * toggle can never survive into a family that did not set it. */
	reset();
	claim_wpad(0, C);
	sn_report(C, 0, true);
	sn_report(C, 0, true);
	sn_report(C, Y, true);
	assert(sneaking() && "the family-change case never engaged sneak");
	sn_report(C, 0, true);
	sn_report(WPAD_EXP_NUNCHUK, 0, true);
	assert(!sneaking() && "a family change did not clear the sneak toggle");
	puts("PASS sneak toggle (Classic Y + GameCube Z): a tap engages and stays, "
		 "a second releases it, mining with R keeps it, screens preserve and "
		 "suppress it, the acting press cannot toggle on the way out, and "
		 "death / vehicle / world leave / disconnect / family change clear it");
}

/* The SPLIT GUEST on each of the two new families: its own machine, its own
 * clears, and never the host's. */
static void test_sneak_toggle_other_families_guest(void) {
	const u32 C = WPAD_EXP_CLASSIC;
	const u32 Y = WPAD_CLASSIC_BUTTON_Y;
	reset();
	claim_wpad(0, C);
	connect_wpad(1, C);
	assert(input_player_bind(1, (struct input_device) {INPUT_DEVICE_WPAD, 1}));
	for(unsigned k = 0; k < 3; k++) {
		enqueue(0, report(C, 0));
		enqueue(1, report(C, 0));
		tick(1);
		input_sneak_gameplay_tick(0, false, false);
		input_sneak_gameplay_tick(1, false, false);
	}
	/* only the GUEST taps Y */
	enqueue(0, report(C, 0));
	enqueue(1, report(C, Y));
	tick(1);
	input_sneak_gameplay_tick(0, false, false);
	input_sneak_gameplay_tick(1, false, false);
	assert(!input_held(IB_SNEAK) && "the Classic guest toggled the host's sneak");
	assert(input_player_frame(1)->down[IB_SNEAK]
		   && "the Classic guest's own sneak never engaged");
	enqueue(0, report(C, 0));
	enqueue(1, report(C, 0));
	tick(1);
	input_sneak_gameplay_tick(0, false, false);
	input_sneak_gameplay_tick(1, false, false);
	assert(input_player_frame(1)->down[IB_SNEAK]
		   && "the Classic guest's toggle did not latch");
	assert(!input_held(IB_SNEAK));
	/* the guest's DISCONNECT clears the guest's toggle and nothing else */
	enqueue(0, report(C, Y));
	tick(1);
	input_sneak_gameplay_tick(0, false, false);
	input_sneak_gameplay_tick(1, false, false);
	assert(input_held(IB_SNEAK) && "the host's own tap stopped engaging");
	wpad[1].attached = false;
	enqueue(0, report(C, 0));
	tick(1);
	input_sneak_gameplay_tick(0, false, false);
	input_sneak_gameplay_tick(1, false, false);
	assert(!input_player_frame(1)->down[IB_SNEAK]
		   && "the guest's disconnect did not clear its toggle");
	assert(input_held(IB_SNEAK)
		   && "the guest's disconnect cleared the HOST's toggle");
	input_player_unbind(1);

	/* A GAMECUBE guest beside a Classic host: two families, two machines. */
	reset();
	claim_wpad(0, C);
	pad[1].attached = true;
	tick(1);
	assert(input_player_bind(1, (struct input_device) {INPUT_DEVICE_GAMECUBE, 1}));
	for(unsigned k = 0; k < 3; k++) {
		enqueue(0, report(C, 0));
		pad[1].held = 0;
		tick(1);
		input_sneak_gameplay_tick(0, false, false);
		input_sneak_gameplay_tick(1, false, false);
	}
	enqueue(0, report(C, 0));
	pad[1].held = PAD_TRIGGER_Z;
	tick(1);
	input_sneak_gameplay_tick(0, false, false);
	input_sneak_gameplay_tick(1, false, false);
	assert(input_player_frame(1)->down[IB_SNEAK]
		   && "the GameCube guest's own sneak never engaged");
	assert(!input_held(IB_SNEAK)
		   && "the GameCube guest toggled the Classic host's sneak");
	enqueue(0, report(C, 0));
	pad[1].held = 0;
	tick(1);
	input_sneak_gameplay_tick(0, false, false);
	input_sneak_gameplay_tick(1, false, false);
	assert(input_player_frame(1)->down[IB_SNEAK]
		   && "the GameCube guest's toggle did not latch");
	input_player_unbind(1);
	puts("PASS sneak toggle (Classic + GameCube split guest): each owner runs "
		 "its own toggle and its own clears, across two different pad families");
}

/* The DOUBLE DUTY the two new toggles create, at the level every consumer
 * reads it: Classic Y is also gui_quick_move -- which is container Quick Move
 * (container_menu.c:172), the sign editor's Done (screen_sign.c:342) and the
 * chat keyboard's Send (widget_osk.c:738, OSK_CHAT_ARM_SEND) -- and GameCube Z
 * is also the menu stack Drop (container_menu.c:169). All four consumers read
 * a PRESSED EDGE of that role off the owner frame, so one frame-level proof
 * covers all of them: a button held across a screen boundary has no edge left,
 * and a release is not a press. (The chat keyboard carries a SECOND guard of
 * its own -- osk_chat_input_reset blocks all twelve chat buttons on the opening
 * frame -- pinned by tests/test_chat_keyboard.c's test_opener_press_never_sends;
 * nothing here weakens it.) */
static void test_sneak_double_duty_handoffs(void) {
	const u32 C = WPAD_EXP_CLASSIC;
	const u32 Y = WPAD_CLASSIC_BUTTON_Y;
	struct container_input in;
	reset();
	claim_wpad(0, C);
	sn_report(C, 0, true);
	sn_report(C, 0, true);

	/* ENTRY: the Y tap that engaged sneak is still held when the window opens.
	 * No quick move / Done / Send until it is released and pressed again. */
	sn_report(C, Y, true);
	assert(sneaking() && "a tap of Classic Y did not engage sneak");
	for(unsigned k = 0; k < 3; k++) {
		enqueue(0, report(C, Y));
		sn_tick(1, false, false, false); /* the window owns the frame */
		container_menu_read_input(&in, 0, 0, 640, 480);
		assert(!in.quick_move
			   && "the Y that engaged sneak also quick-moved a stack");
		assert(sneaking() && "the window removed ledge protection");
	}
	enqueue(0, report(C, 0));
	sn_tick(1, false, false, false);
	container_menu_read_input(&in, 0, 0, 640, 480);
	assert(!in.quick_move && "the RELEASE of Y quick-moved a stack");
	enqueue(0, report(C, Y));
	sn_tick(1, false, false, false);
	container_menu_read_input(&in, 0, 0, 640, 480);
	assert(in.quick_move && "a fresh Y stopped quick-moving inside the window");

	/* EXIT: that quick-move press is still held when gameplay resumes -- it
	 * must not toggle sneak, and a fresh press must. */
	for(unsigned k = 0; k < 3; k++) {
		sn_report(C, Y, true);
		assert(sneaking() && "the Y that quick-moved toggled sneak off");
	}
	sn_report(C, 0, true);
	sn_report(C, Y, true);
	assert(!sneaking() && "Classic Y never re-armed after a window");
	sn_report(C, 0, true);

	/* GAMECUBE Z: sneak in the world, the stack DROP in a menu, both ways. */
	reset();
	sn_gc_claim(0);
	sn_gc(0, PAD_TRIGGER_Z, true);
	assert(sneaking() && "a tap of GameCube Z did not engage sneak");
	for(unsigned k = 0; k < 3; k++) {
		pad[0].held = PAD_TRIGGER_Z;
		sn_tick(1, false, false, false);
		container_menu_read_input(&in, 0, 0, 640, 480);
		assert(!in.drop && "the Z that engaged sneak also dropped a stack");
		assert(sneaking() && "the window removed GameCube ledge protection");
	}
	pad[0].held = 0;
	sn_tick(1, false, false, false);
	container_menu_read_input(&in, 0, 0, 640, 480);
	assert(!in.drop && "the RELEASE of Z dropped a stack");
	pad[0].held = PAD_TRIGGER_Z;
	sn_tick(1, false, false, false);
	container_menu_read_input(&in, 0, 0, 640, 480);
	assert(in.drop && "a fresh Z stopped dropping inside the window");
	for(unsigned k = 0; k < 3; k++) {
		sn_gc(0, PAD_TRIGGER_Z, true);
		assert(sneaking() && "the Z that dropped a stack toggled sneak off");
	}
	sn_gc(0, 0, true);
	sn_gc(0, PAD_TRIGGER_Z, true);
	assert(!sneaking() && "GameCube Z never re-armed after a window");
	sn_gc(0, 0, true);
	puts("PASS sneak double duty: Classic Y (quick move / sign Done / chat "
		 "Send) and GameCube Z (menu Drop) each act on ONE side of a screen "
		 "boundary -- a held button never leaks, a fresh press always works");
}

static void test_sneak_hold_families_and_optout(void) {
	/* EXPLICIT OPT-OUT, now PER FAMILY: a 0 restores Beta's hold on exactly
	 * the pad it names and leaves the other two alone. */
	config_destroy(&gstate.config_user);
	FILE* f = fopen(CAVEX_HOLD_CONFIG, "w");
	assert(f);
	fputs("{\"input\":{\"player_sneak\":[6,207,408]},"
		  "\"controls\":{\"wiimote_ir_look\":0,\"wiimote_sneak_toggle\":0,"
		  "\"classic_sneak_toggle\":0,\"gamecube_sneak_toggle\":0}}", f);
	fclose(f);
	assert(config_create(&gstate.config_user, CAVEX_HOLD_CONFIG));
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	sn_report(WPAD_EXP_NUNCHUK, 0, true);
	sn_report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_1, true);
	assert(sneaking() && "the hold opt-out stopped sneaking entirely");
	sn_report(WPAD_EXP_NUNCHUK, 0, true);
	assert(!sneaking() && "an explicit 0 did not restore the faithful hold");
	reset();
	claim_wpad(0, WPAD_EXP_CLASSIC);
	sn_report(WPAD_EXP_CLASSIC, 0, true);
	sn_report(WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_Y, true);
	assert(sneaking() && "the Classic hold opt-out stopped sneaking entirely");
	sn_report(WPAD_EXP_CLASSIC, 0, true);
	assert(!sneaking()
		   && "an explicit classic_sneak_toggle 0 did not restore the hold");
	reset();
	sn_gc_claim(0);
	sn_gc(0, PAD_TRIGGER_Z, true);
	assert(sneaking() && "the GameCube hold opt-out stopped sneaking entirely");
	sn_gc(0, 0, true);
	assert(!sneaking()
		   && "an explicit gamecube_sneak_toggle 0 did not restore the hold");

	/* ... and ONE family opting out leaves the others on the toggle. */
	config_destroy(&gstate.config_user);
	f = fopen(CAVEX_HOLD_CONFIG, "w");
	assert(f);
	fputs("{\"input\":{\"player_sneak\":[6,207,408]},"
		  "\"controls\":{\"wiimote_ir_look\":0,\"classic_sneak_toggle\":0}}", f);
	fclose(f);
	assert(config_create(&gstate.config_user, CAVEX_HOLD_CONFIG));
	reset();
	claim_wpad(0, WPAD_EXP_CLASSIC);
	sn_report(WPAD_EXP_CLASSIC, 0, true);
	sn_report(WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_Y, true);
	sn_report(WPAD_EXP_CLASSIC, 0, true);
	assert(!sneaking() && "the Classic opt-out stopped working on its own");
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	sn_report(WPAD_EXP_NUNCHUK, 0, true);
	sn_report(WPAD_EXP_NUNCHUK, 0, true);
	sn_report(WPAD_EXP_NUNCHUK, WPAD_BUTTON_1, true);
	sn_report(WPAD_EXP_NUNCHUK, 0, true);
	assert(sneaking() && "the Classic opt-out also turned off the Remote's");
	reset();
	sn_gc_claim(0);
	sn_gc(0, 0, true);
	sn_gc(0, PAD_TRIGGER_Z, true);
	sn_gc(0, 0, true);
	assert(sneaking() && "the Classic opt-out also turned off the GameCube's");
	config_destroy(&gstate.config_user);
	remove(CAVEX_HOLD_CONFIG);
	assert(config_create(&gstate.config_user, "config_wii.json"));
	/* ... and the shipped config states all three keys, explicitly ON. */
	static const char* const keys[] = {"controls.wiimote_sneak_toggle",
									   "controls.classic_sneak_toggle",
									   "controls.gamecube_sneak_toggle"};
	for(size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
		assert(config_read_int(&gstate.config_user, keys[k], 0) == 1
			   && "the shipped config lost a sneak-toggle key");
	puts("PASS sneak toggle opt-out: an explicit 0 restores Beta's HOLD on the "
		 "family it names, per family, and the shipped config states all three "
		 "keys as 1");
}

static void test_sneak_toggle_guest_isolation(void) {
	/* Two owners, two machines: the guest's Remote must not toggle the host's
	 * sneak, and the host's tick must not advance the guest's. */
	const u32 T = WPAD_EXP_NUNCHUK;
	const u32 ONE = WPAD_BUTTON_1;
	reset();
	claim_wpad(0, T);
	connect_wpad(1, T);
	assert(input_player_bind(1, (struct input_device) {INPUT_DEVICE_WPAD, 1}));
	enqueue(0, report(T, 0));
	enqueue(1, report(T, 0));
	tick(1);
	input_sneak_gameplay_tick(0, false, false);
	input_sneak_gameplay_tick(1, false, false);
	for(unsigned k = 0; k < 2; k++) {
		enqueue(0, report(T, 0));
		enqueue(1, report(T, 0));
		tick(1);
		input_sneak_gameplay_tick(0, false, false);
		input_sneak_gameplay_tick(1, false, false);
	}
	/* Only the GUEST presses. */
	enqueue(0, report(T, 0));
	enqueue(1, report(T, ONE));
	tick(1);
	input_sneak_gameplay_tick(0, false, false);
	input_sneak_gameplay_tick(1, false, false);
	assert(!input_held(IB_SNEAK) && "the guest toggled the host's sneak");
	assert(input_player_frame(1)->down[IB_SNEAK]
		   && "the guest's own sneak never engaged");
	/* ... and it stays engaged for the guest once released. */
	enqueue(0, report(T, 0));
	enqueue(1, report(T, 0));
	tick(1);
	input_sneak_gameplay_tick(0, false, false);
	input_sneak_gameplay_tick(1, false, false);
	assert(input_player_frame(1)->down[IB_SNEAK]
		   && "the guest's toggle did not latch");
	assert(!input_held(IB_SNEAK));
	input_player_unbind(1);
	puts("PASS sneak toggle: each local player owns its own toggle; a guest "
		 "press never reaches the host's sneak level");
}

/* ---- B1 slice 3: the OPEN-INVENTORY vs CLOSE role split -----------------
 * Brief 07 C + the lead's ruling. IB_INVENTORY used to mean BOTH "open the
 * inventory from gameplay" and "Close / Back / Cancel in a screen" -- 24
 * behavioural reads across 13 files mean the second one. The CavEX ruling puts
 * the gameplay open on A, but the approved 2026-09-10 matrix keeps Close on
 * Remote 1 with A as Select, so one role cannot carry both: a single A press
 * in a chest would pick a slot AND close the window.
 *
 * So the GAMEPLAY OPEN moved out instead: IB_OPEN_INVENTORY /
 * `input.open_inventory`, read by exactly the four sites that mean "open"
 * (screen_ingame, the creative picker in main.c, the split guest's menu host,
 * and the menus_dwell walker's INVENTORY_OPEN step). Close keeps its 66 call
 * sites and its codes, so every screen behaves exactly as before. A config
 * that omits the new key falls back to its OWN `input.inventory` codes, which
 * is what makes every existing config -- and the live card -- behave exactly
 * as today. */
#define CAVEX_LEGACY_INV_CONFIG "test_cavex_legacy_inventory.json"
static void test_open_inventory_role(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	reset();
	claim_wpad(0, T);
	enqueue(0, report(T, 0));
	tick(1);
	enqueue(0, report(T, 0));
	tick(1);

	/* A opens the inventory and does NOT close anything; 1 closes and does
	 * NOT open. That separation is the whole deliverable. */
	enqueue(0, report(T, WPAD_BUTTON_A));
	tick(1);
	assert(input_pressed(IB_OPEN_INVENTORY) && "A stopped opening the inventory");
	assert(!input_pressed(IB_INVENTORY) && "A also fired the menu Close role");
	assert(input_pressed(IB_GUI_CLICK) && "A stopped being Select in menus");
	enqueue(0, report(T, 0));
	tick(1);
	enqueue(0, report(T, WPAD_BUTTON_1));
	tick(1);
	assert(input_pressed(IB_INVENTORY) && "1 stopped closing menus");
	assert(!input_pressed(IB_OPEN_INVENTORY) && "1 also opened the inventory");
	enqueue(0, report(T, 0));
	tick(1);

	/* NEUTRAL REARM across the boundary: the A press that opens the window
	 * must not also pick a slot inside it. The container's own reader is the
	 * proof -- it reads a rising edge of IB_GUI_CLICK, and the opening press
	 * has no edge left by the time the window is up. */
	enqueue(0, report(T, WPAD_BUTTON_A));
	tick(1);
	assert(input_pressed(IB_OPEN_INVENTORY));
	for(unsigned k = 0; k < 3; k++) {
		enqueue(0, report(T, WPAD_BUTTON_A)); /* still held in the new window */
		tick(1);
		struct container_input in;
		container_menu_read_input(&in, 0, 0, 640, 480);
		assert(!in.pick && "the A press that opened the window also picked");
		assert(!in.close && "the A press that opened the window also closed it");
	}
	enqueue(0, report(T, 0)); /* release re-arms ... */
	tick(1);
	enqueue(0, report(T, WPAD_BUTTON_A));
	tick(1);
	{
		struct container_input in;
		container_menu_read_input(&in, 0, 0, 640, 480);
		assert(in.pick && "a fresh A press stopped picking inside the window");
	}
	enqueue(0, report(T, 0));
	tick(1);

	/* Classic X and GameCube Y carry BOTH roles, exactly as they do today --
	 * the split changed nothing on those two families. */
	reset();
	claim_wpad(0, WPAD_EXP_CLASSIC);
	enqueue(0, report(WPAD_EXP_CLASSIC, 0));
	tick(1);
	enqueue(0, report(WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_X));
	tick(1);
	assert(input_pressed(IB_INVENTORY) && input_pressed(IB_OPEN_INVENTORY)
		   && "Classic X lost one of its two roles");
	reset();
	pad[0].attached = true;
	tick(1);
	pad[0].held = PAD_BUTTON_A;
	tick(1);
	pad[0].held = 0;
	tick(1);
	pad[0].held = PAD_BUTTON_Y;
	tick(1);
	assert(input_pressed(IB_INVENTORY) && input_pressed(IB_OPEN_INVENTORY)
		   && "GameCube Y lost one of its two roles");
	pad[0].held = 0;
	tick(1);

	/* THE FALLBACK, which is what keeps every existing config -- and the live
	 * card -- behaving exactly as today: a config that never heard of
	 * open_inventory answers it with its OWN inventory codes, so 1 still
	 * opens AND closes. */
	config_destroy(&gstate.config_user);
	FILE* f = fopen(CAVEX_LEGACY_INV_CONFIG, "w");
	assert(f);
	fputs("{\"input\":{\"inventory\":[6,206,407],\"gui_click\":[4,204,404]}}", f);
	fclose(f);
	assert(config_create(&gstate.config_user, CAVEX_LEGACY_INV_CONFIG));
	reset();
	claim_wpad(0, T);
	enqueue(0, report(T, 0));
	tick(1);
	enqueue(0, report(T, WPAD_BUTTON_1));
	tick(1);
	assert(input_pressed(IB_INVENTORY)
		   && "a legacy config stopped closing on 1");
	assert(input_pressed(IB_OPEN_INVENTORY)
		   && "a legacy config stopped OPENING on 1 -- the fallback is gone");
	enqueue(0, report(T, 0));
	tick(1);
	enqueue(0, report(T, WPAD_BUTTON_A));
	tick(1);
	assert(!input_pressed(IB_OPEN_INVENTORY)
		   && "a legacy config invented an A open the user never asked for");
	config_destroy(&gstate.config_user);
	remove(CAVEX_LEGACY_INV_CONFIG);
	assert(config_create(&gstate.config_user, "config_wii.json"));
	puts("PASS role split: A opens the inventory, 1 closes, neither does the "
		 "other; the opening press cannot pick a slot; Classic X / GameCube Y "
		 "keep both roles; a config without the key falls back to its own");
}

/* ---- M-3: the Wii Remote's DOUBLE-DUTY buttons across a screen boundary --
 *
 * Astra menu review 2026-09-19, section C item 3 ("certify the held-button
 * handoffs with the new map"). The maintainer's CavEX layout gives four Remote
 * buttons two jobs each -- one in gameplay, one in menus -- and every one of
 * them is a button a player really does hold while a window opens or closes:
 *
 *   A     (4) gameplay OPEN the inventory   / menu SELECT
 *   1     (6) gameplay SNEAK toggle         / menu CLOSE
 *   2     (7) gameplay CHAT tap             / menu DROP
 *   Minus (9) gameplay HOTBAR previous      / menu PREVIOUS
 *   Plus  (8) gameplay HOTBAR next          / menu PAGE
 *
 * The rule being certified is one sentence: a button HELD across the boundary
 * does the job of exactly ONE side, and the other side needs a release and a
 * fresh press. Both DIRECTIONS (menu entry and menu exit) and BOTH OWNERS
 * (player 0 and the split guest).
 *
 * Nothing here is new production behaviour -- it is the existing machinery
 * being held to the claim: menu roles are PRESSED edges off the owner frame
 * (container_menu.c container_menu_read_player), so a press spent in gameplay
 * has no edge left inside the window; and the two RELEASE gestures (the chat
 * tap and the hotbar tap) are cleared by the poll gap their scope watches for
 * (input_tap_gesture.h input_tap_gesture_tick), because a screen that owns the
 * frame does not call the gameplay gates at all. This case is what stops either
 * of those two mechanisms from being removed by accident.
 *
 * NOT COVERED, stated rather than implied: the CHAT opener is player-0 only by
 * design (screen_ingame gates it on slot 0 and input_chat_open_edge reads
 * player 0), so the guest's half of the 2 pair is "its Drop works and it never
 * opens the host's composer", which is asserted rather than assumed. */
static struct mh_owner {
	bool chat, left, right, sneak, open_inv;
	struct container_input menu;
} mh[2];

/* ONE poll, each owner's frame consumed by exactly ONE screen -- main.c never
 * runs both for the same player. `menu` means a container owns that player's
 * frame: its roles are read, and the gameplay gates are NOT called, which is
 * precisely the poll gap the tap gestures treat as a cancel. */
static void mh_read(unsigned p, bool menu) {
	const struct input_player_frame* f = input_player_frame(p);
	mh[p] = (struct mh_owner) {0};
	if(menu) {
		container_menu_read_player(&mh[p].menu, f, 0, 0, 640, 480);
		return;
	}
	if(p == 0) {
		mh[0].chat = input_chat_open_edge();
		mh[0].open_inv = input_pressed(IB_OPEN_INVENTORY);
	} else
		mh[1].open_inv = f && f->pressed[IB_OPEN_INVENTORY];
	/* screen_ingame passes NULL for the host and the guest's own latched frame
	 * for owner 1 -- main.c's own two call shapes. */
	mh[p].left = input_hotbar_step_edge(p, p ? f : NULL, IB_SCROLL_LEFT);
	mh[p].right = input_hotbar_step_edge(p, p ? f : NULL, IB_SCROLL_RIGHT);
	input_sneak_gameplay_tick(p, false, false);
	mh[p].sneak = p == 0 ? input_held(IB_SNEAK) : (f && f->down[IB_SNEAK]);
}

static void mh_poll(u32 type, u32 host, u32 guest, bool host_menu,
					bool guest_menu, unsigned ms) {
	enqueue(0, report(type, host));
	enqueue(1, report(type, guest));
	tick(ms);
	mh_read(0, host_menu);
	mh_read(1, guest_menu);
}

/* The sneak LEVEL survives a screen (Astra ruling 3 D3) and is read off the
 * owner frame in the menu, where the gameplay gates never ran. */
static bool mh_sneaking(unsigned p) {
	const struct input_player_frame* f = input_player_frame(p);
	return f && f->down[IB_SNEAK];
}

static void test_remote_menu_handoffs(void) {
	const u32 T = WPAD_EXP_NUNCHUK;
	const u32 A = WPAD_BUTTON_A, ONE = WPAD_BUTTON_1, TWO = WPAD_BUTTON_2;
	const u32 M = WPAD_BUTTON_MINUS, P = WPAD_BUTTON_PLUS;
	reset();
	claim_wpad(0, T);
	connect_wpad(1, T);
	assert(input_player_bind(1, (struct input_device) {INPUT_DEVICE_WPAD, 1}));
	for(unsigned k = 0; k < 3; k++)
		mh_poll(T, 0, 0, false, false, 1);

	/* ---- A: gameplay OPEN -> menu SELECT, and back ---------------------- */
	/* ENTRY, both owners: the A that opened the window is still down inside it
	 * and must not pick a slot; a release and a fresh press must. */
	mh_poll(T, A, A, false, false, 1);
	assert(mh[0].open_inv && mh[1].open_inv && "A stopped opening the inventory");
	for(unsigned k = 0; k < 3; k++) {
		mh_poll(T, A, A, true, true, 1); /* the window is up, A still held */
		assert(!mh[0].menu.pick && !mh[1].menu.pick
			   && "the A press that opened the window also picked a slot");
	}
	mh_poll(T, 0, 0, true, true, 1);
	mh_poll(T, A, A, true, true, 1);
	assert(mh[0].menu.pick && mh[1].menu.pick
		   && "a fresh A press stopped picking inside the window");

	/* EXIT, both owners: A is still held as the window closes -- it must not
	 * re-open the inventory on the way out. */
	for(unsigned k = 0; k < 3; k++) {
		mh_poll(T, A, A, false, false, 1); /* gameplay again, A still down */
		assert(!mh[0].open_inv && !mh[1].open_inv
			   && "a held A re-opened the inventory on the way out of a menu");
	}
	mh_poll(T, 0, 0, false, false, 1);
	mh_poll(T, A, A, false, false, 1);
	assert(mh[0].open_inv && mh[1].open_inv
		   && "A never re-armed after a menu closed");
	mh_poll(T, 0, 0, false, false, 1);

	/* ---- 1: gameplay SNEAK toggle -> menu CLOSE, and back --------------- */
	/* ENTRY: the tap that engaged sneak is still down when the window opens;
	 * it must not Close the window it did not open. */
	mh_poll(T, ONE, ONE, false, false, 1);
	assert(mh_sneaking(0) && mh_sneaking(1) && "a tap of 1 did not engage sneak");
	for(unsigned k = 0; k < 3; k++) {
		mh_poll(T, ONE, ONE, true, true, 1);
		assert(!mh[0].menu.close && !mh[1].menu.close
			   && "the 1 that toggled sneak also closed the window");
		assert(mh_sneaking(0) && mh_sneaking(1)
			   && "opening a screen removed ledge protection");
	}
	/* ... and a fresh 1 inside the window IS the Close, for both owners,
	 * without reaching the gameplay toggle. */
	mh_poll(T, 0, 0, true, true, 1);
	mh_poll(T, ONE, ONE, true, true, 1);
	assert(mh[0].menu.close && mh[1].menu.close
		   && "a fresh 1 stopped closing the window");
	assert(mh_sneaking(0) && mh_sneaking(1)
		   && "the Close press reached the sneak toggle inside a screen");

	/* EXIT: that same Close press is still down when gameplay resumes -- it
	 * must not toggle sneak off, and a fresh press must. */
	for(unsigned k = 0; k < 3; k++) {
		mh_poll(T, ONE, ONE, false, false, 1);
		assert(mh_sneaking(0) && mh_sneaking(1)
			   && "the press that closed a menu toggled sneak");
	}
	mh_poll(T, 0, 0, false, false, 1);
	mh_poll(T, ONE, ONE, false, false, 1);
	assert(!mh_sneaking(0) && !mh_sneaking(1)
		   && "the sneak toggle never re-armed after a menu");
	mh_poll(T, 0, 0, false, false, 1);

	/* ---- 2: gameplay CHAT tap -> menu DROP, and back -------------------- */
	/* ENTRY: a tap armed in gameplay, the window opens before the release. The
	 * release must open NOTHING (the gesture's poll gap cancelled it) and must
	 * not drop either -- a release is not a press edge. */
	mh_poll(T, TWO, TWO, false, false, 1);
	assert(!mh[0].chat && "2 opened chat on its press edge");
	mh_poll(T, TWO, TWO, true, true, 1);
	assert(!mh[0].menu.drop && !mh[1].menu.drop
		   && "the 2 press spent in gameplay dropped a stack in the window");
	mh_poll(T, 0, 0, true, true, 1);
	assert(!mh[0].menu.drop && !mh[1].menu.drop
		   && "the release of a chat tap dropped a stack");
	/* ... a FRESH 2 inside the window is the Drop, for both owners. */
	mh_poll(T, TWO, TWO, true, true, 1);
	assert(mh[0].menu.drop && mh[1].menu.drop
		   && "a fresh 2 stopped dropping inside the window");

	/* EXIT: that Drop press is still held as the window closes. Its release in
	 * gameplay must NOT open the composer -- the tap never armed there. */
	mh_poll(T, TWO, TWO, false, false, 1);
	assert(!mh[0].chat);
	mh_poll(T, 0, 0, false, false, 1);
	assert(!mh[0].chat
		   && "the 2 that dropped a stack opened chat on the way out");
	for(unsigned k = 0; k < 3; k++) {
		mh_poll(T, 0, 0, false, false, 1);
		assert(!mh[0].chat && "a cancelled chat tap opened late");
	}
	/* ... and a fresh tap in gameplay still opens, exactly once. */
	mh_poll(T, TWO, 0, false, false, 1);
	assert(!mh[0].chat);
	mh_poll(T, 0, 0, false, false, 1);
	assert(mh[0].chat && "the chat tap never re-armed after a menu");
	mh_poll(T, 0, 0, false, false, 1);
	assert(!mh[0].chat && "the chat tap opened twice");
	/* The GUEST's half of this pair: its 2 drops in its OWN window and never
	 * reaches the host's composer -- the opener is player-0 only by design. */
	mh_poll(T, 0, TWO, false, true, 1);
	assert(mh[1].menu.drop && "the guest's fresh 2 stopped dropping");
	assert(!mh[0].chat && "the guest's 2 opened the host's composer");
	mh_poll(T, 0, 0, false, false, 1);
	assert(!mh[0].chat && "the guest's 2 release opened the host's composer");

	/* ---- Minus / Plus: gameplay HOTBAR -> menu PREVIOUS / PAGE ----------
	 * Under the RELEASE config these are PRESS-edge hotbar steps (E-2: no
	 * chord pairs them any more), so the handoff claim is the plain one: an
	 * edge belongs to whichever screen owned the frame it happened on. */
	mh_poll(T, M, P, false, false, 1);
	assert(mh[0].left && !mh[0].right && "Minus stopped stepping the hotbar");
	assert(mh[1].right && !mh[1].left
		   && "the guest's Plus stopped stepping the hotbar");
	/* ENTRY: those same buttons are still held when the window opens -- no
	 * page, no previous, because the edge was spent in gameplay. */
	for(unsigned k = 0; k < 3; k++) {
		mh_poll(T, M, P, true, true, 1);
		assert(!mh[0].menu.previous && !mh[1].menu.page
			   && "the Minus/Plus press spent in gameplay paged the window");
	}
	mh_poll(T, 0, 0, true, true, 1);
	assert(!mh[0].menu.previous && !mh[1].menu.page
		   && "the RELEASE of a hotbar press paged the window");
	/* ... a FRESH press inside the window pages, for both owners. */
	mh_poll(T, M, P, true, true, 1);
	assert(mh[0].menu.previous && "a fresh Minus stopped paging back");
	assert(mh[1].menu.page && "a fresh guest Plus stopped paging forward");

	/* EXIT: those paging presses are still down as the window closes. They
	 * must not step a slot on the way out; a fresh press must. */
	for(unsigned k = 0; k < 3; k++) {
		mh_poll(T, M, P, false, false, 1);
		assert(!mh[0].left && !mh[0].right && !mh[1].left && !mh[1].right
			   && "a menu paging press stepped the hotbar on the way out");
	}
	mh_poll(T, 0, 0, false, false, 1);
	assert(!mh[0].left && !mh[0].right && !mh[1].left && !mh[1].right
		   && "the release of a paging press stepped the hotbar");
	mh_poll(T, M, P, false, false, 1);
	assert(mh[0].left && !mh[0].right
		   && "the host's hotbar never re-armed after a menu");
	assert(mh[1].right && !mh[1].left
		   && "the guest's hotbar never re-armed after a menu");
	mh_poll(T, 0, 0, false, false, 1);

	input_player_unbind(1);
	puts("PASS Remote handoffs (host + split guest): A / 1 / 2 / Minus / Plus "
		 "each do ONE side of their double duty across a menu boundary -- no "
		 "pick, close, drop, page, open, sneak toggle, hotbar step or chat open "
		 "leaks in either direction, and a release re-arms every one of them");
}

/* ---- E-3b: the GAMECUBE pad's new double duty across a screen boundary ---
 *
 * Astra ergonomics 4 moved the hotbar onto B and X, which are ALSO menu
 * buttons -- B is take-half / backspace (gui_split + gui_click_alt) and X is
 * Quick Move -- and moved the chat opener onto D-pad Up, which is also menu
 * Up. The same one-sentence rule as the Remote's handoffs is certified here,
 * in both directions, for BOTH owners. */
static void gc_poll(u16 host, u16 guest, bool host_menu, bool guest_menu) {
	pad[0].held = host;
	pad[1].held = guest;
	tick(1);
	mh_read(0, host_menu);
	mh_read(1, guest_menu);
}

static void test_gamecube_menu_handoffs(void) {
	reset();
	pad[0].attached = true;
	pad[1].attached = true;
	tick(1);
	pad[0].held = PAD_BUTTON_A;
	tick(1);
	pad[0].held = 0;
	tick(1);
	assert(input_player_bind(1, (struct input_device) {INPUT_DEVICE_GAMECUBE, 1}));
	for(unsigned k = 0; k < 3; k++)
		gc_poll(0, 0, false, false);

	/* B / X: gameplay HOTBAR -> menu take-half / Quick Move, and back. */
	gc_poll(PAD_BUTTON_B, PAD_BUTTON_X, false, false);
	assert(mh[0].left && "GameCube B stopped stepping the hotbar");
	assert(mh[1].right && "the GameCube guest's X stopped stepping");
	for(unsigned k = 0; k < 3; k++) {
		gc_poll(PAD_BUTTON_B, PAD_BUTTON_X, true, true);
		assert(!mh[0].menu.split
			   && "the B that stepped the hotbar also took half a stack");
		assert(!mh[1].menu.quick_move
			   && "the X that stepped the hotbar also quick-moved");
	}
	gc_poll(0, 0, true, true);
	assert(!mh[0].menu.split && !mh[1].menu.quick_move
		   && "a hotbar press acted in the window on its release");
	gc_poll(PAD_BUTTON_B, PAD_BUTTON_X, true, true);
	assert(mh[0].menu.split && "a fresh B stopped taking half a stack");
	assert(mh[1].menu.quick_move && "a fresh guest X stopped quick-moving");
	/* EXIT: those menu presses are still held as the window closes. */
	for(unsigned k = 0; k < 3; k++) {
		gc_poll(PAD_BUTTON_B, PAD_BUTTON_X, false, false);
		assert(!mh[0].left && !mh[0].right && !mh[1].left && !mh[1].right
			   && "a menu B / X press stepped the hotbar on the way out");
	}
	gc_poll(0, 0, false, false);
	gc_poll(PAD_BUTTON_B, PAD_BUTTON_X, false, false);
	assert(mh[0].left && "the host's hotbar never re-armed after a menu");
	assert(mh[1].right && "the guest's hotbar never re-armed after a menu");
	gc_poll(0, 0, false, false);

	/* D-pad Up: the chat opener in gameplay, menu Up inside a screen.
	 *
	 * WHERE THE KEYBOARD HALF IS PROVED, stated rather than faked: container
	 * directional roles are LEVELS on purpose (container_menu.c:164 --
	 * `frame->down[IB_GUI_UP]`, which is what gives a held D-pad its repeat),
	 * so "a held Up moves the selection" is CORRECT for a container and this
	 * case must not assert otherwise. What must not happen is the CHAT
	 * KEYBOARD taking the opening press, and that is the composer's own
	 * neutral re-arm (osk_chat_input_reset blocks all twelve chat buttons on
	 * the opening frame), pinned by tests/test_chat_keyboard.c's
	 * test_opener_press_never_sends. This case proves the half it can see:
	 * the opener fires ONCE, a screen owning the frame opens nothing, and a
	 * press held across the close cannot re-open. */
	pad[0].held = PAD_BUTTON_UP;
	assert(chat_tick(1) && "GameCube D-pad Up stopped opening chat");
	for(unsigned k = 0; k < 3; k++) {
		pad[0].held = PAD_BUTTON_UP;
		tick(1); /* the composer owns the frame: the gate is not called */
		mh_read(0, true);
	}
	pad[0].held = 0;
	tick(1);
	mh_read(0, true);
	pad[0].held = PAD_BUTTON_UP;
	tick(1);
	mh_read(0, true);
	assert(mh[0].menu.up && "a fresh Up stopped moving the selection");
	/* ... and that press, held as the screen closes, opens nothing. */
	for(unsigned k = 0; k < 3; k++) {
		pad[0].held = PAD_BUTTON_UP;
		assert(!chat_tick(1)
			   && "a menu Up press opened chat on the way out");
	}
	pad[0].held = 0;
	assert(!chat_tick(1));
	pad[0].held = PAD_BUTTON_UP;
	assert(chat_tick(1) && "the opener never re-armed after a screen");
	pad[0].held = 0;
	assert(!chat_tick(1));
	input_player_unbind(1);
	puts("PASS GameCube handoffs (host + split guest): B / X do the hotbar OR "
		 "the menu action, never both across a boundary, and D-pad Up opens "
		 "chat without moving the keyboard cursor it lands on");
}

/* ---- P-1: the CLOSE press that now RESUMES must not act again in gameplay --
 *
 * Maintainer decision 2026-09-19 on Astra menu review MUST item 8: the Close
 * role RESUMES the global Pause (screen_pause.c) and the split guest's local
 * Pause (local_pause.c) instead of quitting. That hands control straight back
 * to gameplay while the button is still physically down -- and on every Wii
 * family that button already has a gameplay job:
 *
 *   Wii Remote  1 (6)   menu Close  +  gameplay SNEAK toggle
 *   Classic     X (206) menu Close  +  gameplay OPEN the inventory
 *   GameCube    Y (407) menu Close  +  gameplay OPEN the inventory
 *
 * The protection is the press EDGE itself: input_owner_latch and the merged
 * mask both compute `down && !previous`, and input_poll keeps tracking across
 * a screen change, so a held button has no edge left when gameplay returns.
 * The Remote's 1 already has its EXIT leg in test_remote_menu_handoffs above
 * (asserted there against a container close); this case adds the two families
 * that had NO exit leg at all for their Close button, and repeats the Remote's
 * with the sneak gate driven exactly as screen_ingame drives it. */
static void test_close_role_return_handoffs(void) {
	/* ---- Wii Remote 1: Close, then gameplay, sneak must stay off. -------- */
	const u32 T = WPAD_EXP_NUNCHUK;
	reset();
	claim_wpad(0, T);
	sn_report(T, 0, true);
	sn_report(T, 0, true);
	assert(!sneaking());
	/* the pause is up: the gameplay gate is NOT called on its frames, which is
	 * exactly the poll gap the machine treats as "a screen owns this" */
	for(unsigned k = 0; k < 3; k++) {
		enqueue(0, report(T, 0));
		sn_tick(1, false, false, false);
	}
	enqueue(0, report(T, WPAD_BUTTON_1)); /* the Close press, inside the pause */
	sn_tick(1, false, false, false);
	assert(input_pressed(IB_INVENTORY) && "Remote 1 stopped being the Close role");
	for(unsigned k = 0; k < 3; k++) {
		sn_report(T, WPAD_BUTTON_1, true); /* gameplay again, 1 still down */
		assert(!sneaking()
			   && "the Remote 1 press that resumed the pause toggled sneak");
	}
	sn_report(T, 0, true);
	assert(!sneaking() && "the release of the resuming press toggled sneak");
	sn_report(T, WPAD_BUTTON_1, true);
	assert(sneaking() && "the sneak toggle never re-armed after a pause");
	sn_report(T, WPAD_BUTTON_1, true);
	sn_report(T, 0, true);
	sn_report(T, WPAD_BUTTON_1, true);
	assert(!sneaking());
	sn_report(T, 0, true);

	/* ---- Classic X: Close, then gameplay, the inventory must stay shut. -- */
	reset();
	claim_wpad(0, WPAD_EXP_CLASSIC);
	for(unsigned k = 0; k < 2; k++) {
		enqueue(0, report(WPAD_EXP_CLASSIC, 0));
		tick(1);
	}
	enqueue(0, report(WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_X));
	tick(1);
	assert(input_pressed(IB_INVENTORY) && "Classic X stopped being the Close role");
	for(unsigned k = 0; k < 3; k++) {
		enqueue(0, report(WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_X));
		tick(1);
		assert(!input_pressed(IB_OPEN_INVENTORY)
			   && "the Classic X that resumed the pause re-opened the inventory");
	}
	enqueue(0, report(WPAD_EXP_CLASSIC, 0));
	tick(1);
	assert(!input_pressed(IB_OPEN_INVENTORY)
		   && "the release of the resuming Classic X opened the inventory");
	enqueue(0, report(WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_X));
	tick(1);
	assert(input_pressed(IB_OPEN_INVENTORY)
		   && "Classic X never re-armed the inventory after a pause");
	enqueue(0, report(WPAD_EXP_CLASSIC, 0));
	tick(1);

	/* ---- GameCube Y: the same pair. ------------------------------------- */
	reset();
	pad[0].attached = true;
	tick(1);
	pad[0].held = PAD_BUTTON_A;
	tick(1);
	pad[0].held = 0;
	tick(1);
	pad[0].held = PAD_BUTTON_Y;
	tick(1);
	assert(input_pressed(IB_INVENTORY) && "GameCube Y stopped being the Close role");
	for(unsigned k = 0; k < 3; k++) {
		pad[0].held = PAD_BUTTON_Y;
		tick(1);
		assert(!input_pressed(IB_OPEN_INVENTORY)
			   && "the GameCube Y that resumed the pause re-opened the inventory");
	}
	pad[0].held = 0;
	tick(1);
	assert(!input_pressed(IB_OPEN_INVENTORY)
		   && "the release of the resuming GameCube Y opened the inventory");
	pad[0].held = PAD_BUTTON_Y;
	tick(1);
	assert(input_pressed(IB_OPEN_INVENTORY)
		   && "GameCube Y never re-armed the inventory after a pause");
	pad[0].held = 0;
	tick(1);
	puts("PASS Close-role return: the Remote 1 / Classic X / GameCube Y press "
		 "that RESUMES the pause cannot toggle sneak or open the inventory on "
		 "the way back, and a release re-arms every one of them");
}

/* ---- b1-chat-opener-0919/14b H-1: the READ-ONLY opener accessor ----------
 * The HUD hint may not duplicate the fallback literals or re-read the config
 * in a screen (lead ruling 1), so input.c answers "which code opens chat for
 * the family that owns THIS player, and does it open through the gesture
 * rule". Everything below drives the production accessor against the real
 * bindings; nothing here presses a button, because the accessor is a pure
 * read and must not depend on (or disturb) any gesture in flight. */
#define CHAT_HINT_REMAP_CONFIG "test_chat_hint_remap.json"
#define CHAT_HINT_PLUS_CONFIG "test_chat_hint_classic_plus.json"
static void chat_hint_config(const char* path, const char* chat_array) {
	config_destroy(&gstate.config_user);
	FILE* f = fopen(path, "w");
	assert(f);
	fprintf(f, "{\"input\":{\"chat\":%s}}", chat_array);
	fclose(f);
	assert(config_create(&gstate.config_user, path));
}
/* Claim a GameCube pad for player 0 the way claim_wpad does for a Remote. */
static void claim_pad(unsigned c) {
	pad[c].attached = true;
	tick(1);
	pad[c].held = PAD_BUTTON_A;
	tick(1);
	pad[c].held = 0;
	tick(1);
	assert(input_player_frame(0) && input_player_frame(0)->connected);
}
static void assert_opener(unsigned player, int want_code, bool want_tap) {
	int code = -12345;
	bool is_tap = !want_tap;
	assert(input_player_chat_opener(player, &code, &is_tap)
		   && "the owning family resolved no chat opener");
	assert(code == want_code && "the accessor resolved the wrong opener code");
	assert(is_tap == want_tap && "the accessor mis-reported the gesture rule");
}
static void test_chat_opener_accessor(void) {
	int code = -12345;
	bool is_tap = true;

	/* 1. NO OWNER: player 0 is unbound, so there is no family to resolve. */
	use_shipped_config();
	reset();
	assert(!input_player_chat_opener(0, &code, &is_tap)
		   && "an unowned player resolved a chat opener");

	/* 2. the three SHIPPED defaults, each read through its owning family.
	 *    Remote 2 (7) goes through input_chat_opener_rule -> Tap; the two
	 *    D-pad Ups (200 / 400) are plain press edges. */
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	assert_opener(0, 7, true);
	reset();
	claim_wpad(0, WPAD_EXP_CLASSIC);
	assert_opener(0, 200, false);
	reset();
	claim_pad(0);
	assert_opener(0, 400, false);

	/* 3. a REMAP to a gesture-less button: the accessor follows the config
	 *    (Remote 1, Classic Y, GameCube B) and reports no tap. */
	chat_hint_config(CHAT_HINT_REMAP_CONFIG, "[6,207,405]");
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	assert_opener(0, 6, false);
	reset();
	claim_wpad(0, WPAD_EXP_CLASSIC);
	assert_opener(0, 207, false);
	reset();
	claim_pad(0);
	assert_opener(0, 405, false);

	/* 4. a config that binds the CLASSIC opener to Plus (212): that code is
	 *    the release/Minus-cancel gesture, so it reports a tap too -- while
	 *    the Remote's own opener in the same config stays a plain press. */
	chat_hint_config(CHAT_HINT_PLUS_CONFIG, "[0,212,400]");
	reset();
	claim_wpad(0, WPAD_EXP_CLASSIC);
	assert_opener(0, 212, true);
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	assert_opener(0, 0, false);

	/* 5. the SPLIT GUEST resolves ITS OWN family, never player 0's. */
	use_shipped_config();
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	connect_wpad(1, WPAD_EXP_CLASSIC);
	assert(input_player_bind(1, (struct input_device) {INPUT_DEVICE_WPAD, 1}));
	tick(1);
	assert_opener(0, 7, true);
	assert_opener(1, 200, false);
	input_player_unbind(1);
	tick(1);
	assert(!input_player_chat_opener(1, &code, &is_tap)
		   && "an unbound guest still resolved an opener");

	remove(CHAT_HINT_REMAP_CONFIG);
	remove(CHAT_HINT_PLUS_CONFIG);
	use_shipped_config();
	puts("PASS chat opener accessor: the owning family's configured code "
		 "(shipped 7 tap / 200 / 400, a remap, an explicit Classic 212 tap), "
		 "the guest's own family, and false with no owner");
}

static WPADData look_report(u32 type) {
	WPADData data = report(type, 0);
	if(type == WPAD_EXP_NUNCHUK) {
		data.exp.nunchuk.js.ang = 45.0F;
		data.exp.nunchuk.js.mag = 0.8F;
	} else {
		assert(type == WPAD_EXP_CLASSIC);
		data.exp.classic.rjs.ang = 45.0F;
		data.exp.classic.rjs.mag = 0.8F;
	}
	return data;
}

static void assert_near(float actual, float expected) {
	assert(fabsf(actual - expected) < 0.000001F);
}

static void test_primary_look_preferences_wii(void) {
	/* Drive the production Wii adapter for each shipped camera stick. Compare
	 * against that family's own default delta so libogc's polar/cartesian
	 * conversion remains implementation detail while the preference ratio is
	 * pinned exactly where gameplay reads it. */
	for(unsigned family = 0; family < 3; family++) {
		use_shipped_config();
		reset();
		assert(input_primary_look_sensitivity() == 3);
		assert(!input_primary_look_inverted());
		if(family < 2) {
			u32 type = family == 0 ? WPAD_EXP_NUNCHUK : WPAD_EXP_CLASSIC;
			claim_wpad(0, type);
			enqueue(0, look_report(type));
			tick(1);
		} else {
			claim_pad(0);
			pad[0].cx = 64;
			pad[0].cy = 64;
			tick(1);
		}

		float native_x, native_y, x, y;
		input_player_look(0, 0.5F, &native_x, &native_y);
		assert(native_x > 0.0F && native_y > 0.0F);

		assert(input_primary_look_set(1, false));
		assert(input_primary_look_sensitivity() == 1);
		assert(!input_primary_look_inverted());
		input_player_look(0, 0.5F, &x, &y);
		assert_near(x, native_x * 0.5F);
		assert_near(y, native_y * 0.5F);

		assert(input_primary_look_set(5, true));
		assert(input_primary_look_sensitivity() == 5);
		assert(input_primary_look_inverted());
		input_player_look(0, 0.5F, &x, &y);
		assert_near(x, native_x * 1.5F);
		assert_near(y, native_y * -1.5F);
	}

	/* Phase 1 is Player 1 only. Changing P1's preference around the same
	 * latched Classic guest report must leave P2's camera byte-identical. */
	use_shipped_config();
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	connect_wpad(1, WPAD_EXP_CLASSIC);
	assert(input_player_bind(
		1, (struct input_device) {INPUT_DEVICE_WPAD, 1}));
	tick(1);
	enqueue(1, look_report(WPAD_EXP_CLASSIC));
	tick(1);
	float guest_x, guest_y, guest_after_x, guest_after_y;
	assert(input_primary_look_set(5, true));
	input_player_look(1, 0.5F, &guest_x, &guest_y);
	assert(guest_x > 0.0F && guest_y > 0.0F);
	assert(input_primary_look_set(1, false));
	input_player_look(1, 0.5F, &guest_after_x, &guest_after_y);
	assert(!memcmp(&guest_x, &guest_after_x, sizeof(guest_x)));
	assert(!memcmp(&guest_y, &guest_after_y, sizeof(guest_y)));
	input_player_unbind(1);

	/* The same preference state must not touch the menu's IR cursor. Reload
	 * restores the shipped missing-key defaults without rewriting config. */
	use_shipped_config();
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	WPADData aimed = look_report(WPAD_EXP_NUNCHUK);
	aimed.data_present |= WPAD_DATA_IR;
	aimed.ir.valid = 1;
	aimed.ir.x = 137.0F;
	aimed.ir.y = 281.0F;
	aimed.ir.angle = 17.0F;
	enqueue(0, aimed);
	tick(1);
	float pointer_x, pointer_y, pointer_angle;
	assert(input_pointer(&pointer_x, &pointer_y, &pointer_angle));
	assert(input_primary_look_set(5, true));
	float pointer_after_x, pointer_after_y, pointer_after_angle;
	assert(input_pointer(
		&pointer_after_x, &pointer_after_y, &pointer_after_angle));
	assert(!memcmp(&pointer_x, &pointer_after_x, sizeof(pointer_x)));
	assert(!memcmp(&pointer_y, &pointer_after_y, sizeof(pointer_y)));
	assert(!memcmp(&pointer_angle, &pointer_after_angle, sizeof(pointer_angle)));
	input_primary_look_reload();
	assert(input_primary_look_sensitivity() == 3);
	assert(!input_primary_look_inverted());
	puts("PASS P1 look preferences: Wii Nunchuk/Classic/GameCube camera deltas "
		 "honor default, sensitivity 1/5 and inversion; P2 look and menu IR "
		 "pointer stay unchanged");
}

enum settings_family {
	SETTINGS_NUNCHUK,
	SETTINGS_CLASSIC,
	SETTINGS_GAMECUBE,
};

static void settings_input(enum settings_family family, u32 wpad_buttons,
						   u16 pad_buttons) {
	if(family == SETTINGS_GAMECUBE)
		pad[0].held = pad_buttons;
	else
		enqueue(0, report(family == SETTINGS_CLASSIC ? WPAD_EXP_CLASSIC
												 : WPAD_EXP_NUNCHUK,
						 wpad_buttons));
	tick(1);
	assert(gstate.players[0].current_screen == &screen_controller_settings);
	gstate.players[0].current_screen->update(
		gstate.players[0].current_screen, 0.016F);
}

static void settings_press(enum settings_family family, u32 nunchuk,
						   u32 classic, u16 gamecube) {
	settings_input(family,
		family == SETTINGS_CLASSIC ? classic : nunchuk, gamecube);
	settings_input(family, 0, 0);
}

static void settings_claim(enum settings_family family) {
	if(family == SETTINGS_GAMECUBE)
		claim_pad(0);
	else
		claim_wpad(0, family == SETTINGS_CLASSIC ? WPAD_EXP_CLASSIC
											 : WPAD_EXP_NUNCHUK);
}

static void test_controller_settings_screen(void) {
	/* controller_commit writes the literal config.json. Isolate that production
	 * path from the repository and give it a config owned only by this test. */
	char original_cwd[1024];
	assert(TEST_GETCWD(original_cwd, sizeof(original_cwd)));
	const char* run_dir = "tmp_input_wii_controller_settings";
	assert(TEST_MKDIR(run_dir) == 0 || errno == EEXIST);
	assert(TEST_CHDIR(run_dir) == 0);
	remove("config.json");
	FILE* shipped = fopen("../config_wii.json", "rb");
	FILE* seed = fopen("input.json", "wb");
	assert(shipped && seed);
	for(int ch = fgetc(shipped); ch != EOF; ch = fgetc(shipped))
		assert(fputc(ch, seed) != EOF);
	assert(!ferror(shipped));
	assert(fclose(shipped) == 0);
	assert(fclose(seed) == 0);
	config_destroy(&gstate.config_user);
	assert(config_create(&gstate.config_user, "input.json"));

	for(enum settings_family family = SETTINGS_NUNCHUK;
		family <= SETTINGS_GAMECUBE; family++) {
		assert(config_set_int(
			&gstate.config_user, "controls.look_sensitivity", 3));
		assert(config_set_int(&gstate.config_user, "controls.invert_look", 0));
		assert(input_primary_look_set(3, false));
		reset();
		settings_claim(family);
		screen_set(&screen_controller_settings);
		assert(screen_controller_settings_test_focus() == 0);
		for(int target = 0; target < 4; target++) {
			struct gutil_button_bounds bounds;
			assert(screen_controller_settings_test_target_rect(target, &bounds));
			assert(bounds.width > 0 && bounds.height > 0);
		}
		assert(screen_controller_settings_test_sensitivity() == 3);
		assert(!screen_controller_settings_test_inverted());
		assert(!screen_controller_settings_test_save_failed());
		float px, py, pa;
		assert(!input_pointer(&px, &py, &pa));

		/* A toggles Invert, then Down enters the sensitivity row. */
		settings_press(family, WPAD_BUTTON_A, WPAD_CLASSIC_BUTTON_A,
			PAD_BUTTON_A);
		assert(screen_controller_settings_test_inverted());
		settings_press(family, WPAD_BUTTON_DOWN, WPAD_CLASSIC_BUTTON_DOWN,
			PAD_BUTTON_DOWN);
		assert(screen_controller_settings_test_focus() == 1);

		/* Right owns Plus and clamps at 5; activating the disabled endpoint is
		 * consumed without changing the staged value. */
		for(int i = 0; i < 2; i++)
			settings_press(family, WPAD_BUTTON_RIGHT,
				WPAD_CLASSIC_BUTTON_RIGHT, PAD_BUTTON_RIGHT);
		assert(screen_controller_settings_test_focus() == 2);
		assert(screen_controller_settings_test_sensitivity() == 5);
		assert(!screen_controller_settings_test_target_enabled(2));
		settings_press(family, WPAD_BUTTON_A, WPAD_CLASSIC_BUTTON_A,
			PAD_BUTTON_A);
		assert(screen_controller_settings_test_sensitivity() == 5);

		/* Left owns Minus and clamps at 1 under the same disabled-button rule. */
		for(int i = 0; i < 4; i++)
			settings_press(family, WPAD_BUTTON_LEFT,
				WPAD_CLASSIC_BUTTON_LEFT, PAD_BUTTON_LEFT);
		assert(screen_controller_settings_test_focus() == 1);
		assert(screen_controller_settings_test_sensitivity() == 1);
		assert(!screen_controller_settings_test_target_enabled(1));
		settings_press(family, WPAD_BUTTON_A, WPAD_CLASSIC_BUTTON_A,
			PAD_BUTTON_A);
		assert(screen_controller_settings_test_sensitivity() == 1);

		/* Each family's real Back binding commits the draft and applies it to
		 * Player 1 only after the save succeeds. */
		settings_input(family,
			family == SETTINGS_CLASSIC ? WPAD_CLASSIC_BUTTON_X
										 : WPAD_BUTTON_1,
			PAD_BUTTON_Y);
		assert(gstate.players[0].current_screen == &screen_options);
		assert(input_primary_look_sensitivity() == 1);
		assert(input_primary_look_inverted());
		assert(!screen_controller_settings_test_save_failed());
		FILE* saved = fopen("config.json", "rb");
		assert(saved);
		fclose(saved);
	}

	/* A bound guest cannot navigate or activate this Player 1 screen. */
	assert(config_set_int(&gstate.config_user,
		"controls.look_sensitivity", 3));
	assert(config_set_int(&gstate.config_user, "controls.invert_look", 0));
	assert(input_primary_look_set(3, false));
	reset();
	claim_wpad(0, WPAD_EXP_NUNCHUK);
	connect_wpad(1, WPAD_EXP_CLASSIC);
	assert(input_player_bind(
		1, (struct input_device) {INPUT_DEVICE_WPAD, 1}));
	tick(1);
	screen_set(&screen_controller_settings);
	enqueue(1, report(WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_A));
	tick(1);
	gstate.players[0].current_screen->update(
		gstate.players[0].current_screen, 0.016F);
	assert(!screen_controller_settings_test_inverted());
	enqueue(1, report(WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_DOWN));
	tick(1);
	gstate.players[0].current_screen->update(
		gstate.players[0].current_screen, 0.016F);
	assert(screen_controller_settings_test_focus() == 0);
	input_player_unbind(1);

	config_destroy(&gstate.config_user);
	remove("config.json");
	remove("input.json");
	assert(TEST_CHDIR(original_cwd) == 0);
	assert(TEST_RMDIR(run_dir) == 0);
	assert(config_create(&gstate.config_user, "config_wii.json"));
	puts("PASS Controller settings screen: real Nunchuk/Classic/GameCube "
		 "navigation toggles inversion, clamps disabled 1/5 endpoints, Back "
		 "saves/applies in an isolated config, and P2 cannot act");
}

/* BUGS.md #07 (round 2, 41e5752): the inventory flickered between Inventory and
 * Crafting every 0.25-0.5 s. A page swap must fire ONCE per tap/flick, for
 * every pad, through the production adapter (reports -> owner frame -> T11
 * page detector). One poll is one 16 ms frame. */
static unsigned page_swap_count;
static void page_poll(void) {
	tick(16);
	struct container_input in;
	container_menu_read_input(&in, 0, 0, 640, 480);
	page_swap_count += in.page_swap;
}
static void page_wpad(u32 type, u32 buttons, bool ir) {
	WPADData r = report(type, buttons);
	if(ir) {
		r.data_present |= WPAD_DATA_IR;
		r.ir.valid = 1;
		r.ir.x = 320;
		r.ir.y = 240;
	}
	enqueue(0, r);
	page_poll();
}
static void page_cstick(s8 cx, s8 cy) {
	pad[0].cx = cx;
	pad[0].cy = cy;
	page_poll();
}
static void test_page_swap_once_per_press(void) {
	const struct { u32 type, swap; } wpads[] = {
		{WPAD_EXP_NUNCHUK, WPAD_NUNCHUK_BUTTON_C},
		{WPAD_EXP_CLASSIC, WPAD_CLASSIC_BUTTON_ZL},
	};
	for(size_t k = 0; k < 2; k++) {
		u32 t = wpads[k].type, c = wpads[k].swap;
		reset(); claim_wpad(0, t);
		for(int i = 0; i < 4; i++) page_wpad(t, 0, true);
		page_swap_count = 0;
		/* One tap: exactly one swap. */
		page_wpad(t, c, true); page_wpad(t, c, true); page_wpad(t, 0, true);
		for(int i = 0; i < 40; i++) page_wpad(t, 0, true);
		assert(page_swap_count == 1);
		/* Held 3 s while the pointer leaves and re-enters view: no swap. */
		for(int i = 0; i < 190; i++) page_wpad(t, c, (i / 10) % 2 == 0);
		page_wpad(t, 0, false);
		for(int i = 0; i < 40; i++) page_wpad(t, 0, i % 7 != 0);
		assert(page_swap_count == 1);
		/* Held with an empty driver queue (retained level): no swap. */
		page_wpad(t, c, true);
		for(int i = 0; i < 60; i++) page_poll();
		page_wpad(t, 0, true);
		for(int i = 0; i < 20; i++) page_poll();
		assert(page_swap_count == 1);
	}
	/* GameCube: claim, then flick the C-stick. */
	reset();
	pad[0].attached = true; tick(1);
	pad[0].held = PAD_BUTTON_A; tick(1);
	pad[0].held = 0; tick(1);
	assert(input_player_frame(0) && input_player_frame(0)->connected
		&& input_player_frame(0)->category == INPUT_CAT_GAMECUBE);
	for(int i = 0; i < 4; i++) page_cstick(0, 0);
	page_swap_count = 0;
	/* A clean flick right and back: one swap. */
	page_cstick(110, 0); page_cstick(110, 0); page_cstick(0, 0);
	for(int i = 0; i < 30; i++) page_cstick(0, 0);
	assert(page_swap_count == 1);
	/* A flick whose spring return overshoots past centre to the other side
	 * (a stiff C-stick snapping back): still ONE swap, not two. */
	page_cstick(110, 0); page_cstick(110, 0);
	page_cstick(5, 0); page_cstick(-45, 0); page_cstick(-20, 0); page_cstick(8, 0);
	for(int i = 0; i < 30; i++) page_cstick(0, 0);
	if(page_swap_count != 2) printf("overshoot: %u swaps\n", page_swap_count);
	assert(page_swap_count == 2);
	page_cstick(-110, 0); page_cstick(-110, 0);
	page_cstick(-6, 0); page_cstick(45, 3); page_cstick(30, -2); page_cstick(-5, 0);
	for(int i = 0; i < 30; i++) page_cstick(0, 0);
	assert(page_swap_count == 3);
	/* A worn C-stick resting on the 0.2 edge (drift jitter 24..28 of 128)
	 * must never swap by itself. */
	for(int i = 0; i < 190; i++) page_cstick(i % 2 ? 24 : 28, i % 3 - 1);
	for(int i = 0; i < 190; i++) page_cstick(i % 2 ? -24 : -29, 0);
	for(int i = 0; i < 30; i++) page_cstick(0, 0);
	if(page_swap_count != 3) printf("drift: %u swaps\n", page_swap_count);
	assert(page_swap_count == 3);
	/* Held over 3 s: a hold, never a swap. */
	for(int i = 0; i < 190; i++) page_cstick(100, 0);
	for(int i = 0; i < 30; i++) page_cstick(0, 0);
	assert(page_swap_count == 3);
	puts("PASS page swap: Nunchuk C / Classic ZL tap and GameCube C-stick "
		 "flick swap once; hold, pointer out of view, overshoot and drift never swap");
}

/* ---- notice-spot-1031 (B): 20k-step seeded plug/unplug stress ------------
 * One long random but SEEDED sequence through the real input.c + inventory +
 * notice path on the PC doubles above: GameCube plug/unplug on channels 0-3
 * (re-probe model on), Remote / Nunchuk / Classic connect, swap and drop,
 * PAD_ERR_TRANSFER noise on live pads (input_pad_presence, 542389b2f), A
 * presses (P1 takeover), and split join armed/unarmed with random admission.
 * After EVERY step: no assert; a connected P1/P2 frame is backed by a device
 * observed connected within the debounce window; no device drives two
 * players; a drawn notice names its own player and sits at the one
 * top-left anchor (gameplay 24,16 in every pane); and quiet input converges. */
struct gutil_test_notice_draw { int x, y, w, h; char text[48]; };
extern struct gutil_test_notice_draw gutil_test_last_notice;
static uint32_t st_rng;
static unsigned st_rand(unsigned n) {
	st_rng = st_rng * 1664525u + 1013904223u;
	return (st_rng >> 8) % n;
}
static ptime_t st_lost_at[INPUT_DEVICE_COUNT];
static bool st_was_connected[INPUT_DEVICE_COUNT];
static unsigned st_index(struct input_device d) {
	return d.kind == INPUT_DEVICE_GAMECUBE ? 4u + d.channel : d.channel;
}
static void st_check_notice(unsigned player, bool split, unsigned step) {
	gutil_test_last_notice.w = 0;
	clear_draw();
	gutil_connection_notices(player, split, 16, 8, 640 - 32, split ? 232 : 472);
	if(!gutil_test_last_notice.w) return;
	if(gutil_test_last_notice.x != GUTIL_NOTICE_ANCHOR_X
	   || gutil_test_last_notice.y != GUTIL_NOTICE_ANCHOR_Y) {
		fprintf(stderr, "step %u: notice \"%s\" at %d,%d\n", step,
				gutil_test_last_notice.text, gutil_test_last_notice.x,
				gutil_test_last_notice.y);
		assert(false && "notice off the top-left anchor");
	}
	const char* p = strstr(gutil_test_last_notice.text, "Player ");
	if(p && p[7] >= '1' && p[7] <= '4') {
		/* The card shown in this player's pane is a card of that player. */
		unsigned named = (unsigned)(p[7] - '0');
		bool own = false;
		ptime_t t = time_get();
		for(size_t k = 0; k < INPUT_NOTICE_COUNT; k++) {
			const struct input_connection_notice* n = &input_devices()->notices[k];
			if(input_notice_visible(n, player, t) && n->player + 1u == named)
				own = true;
		}
		if(!own)
			fprintf(stderr, "step %u: pane %u shows \"%s\"\n", step, player,
					gutil_test_last_notice.text);
		assert(own && "notice names the wrong player");
	}
}
static void st_check(unsigned step) {
	ptime_t t = time_get();
	for(unsigned d = 0; d < INPUT_DEVICE_COUNT; d++) {
		bool c = status(d)->observed.connected;
		if(st_was_connected[d] && !c) st_lost_at[d] = t;
		st_was_connected[d] = c;
	}
	struct input_device dev[2];
	bool has[2];
	for(unsigned p = 0; p < 2; p++) {
		has[p] = input_player_device(p, &dev[p]);
		const struct input_player_frame* f = input_player_frame(p);
		if(has[p] && f && f->connected) {
			unsigned i = st_index(dev[p]);
			assert(i < INPUT_DEVICE_COUNT);
			bool ok = status(i)->observed.connected
				|| time_diff_ms(st_lost_at[i], t) <= INPUT_IDENTITY_STABLE_MS + 100;
			if(!ok)
				fprintf(stderr, "step %u: P%u connected on lost device %u\n",
						step, p + 1, i);
			assert(ok && "player bound to a gone device still reads connected");
		}
	}
	if(has[0] && has[1])
		assert(!input_device_equal(dev[0], dev[1]) && "one device, two players");
	st_check_notice(0, false, step);
	st_check_notice(0, true, step);
	st_check_notice(1, true, step);
}
struct st_snapshot {
	bool has[2], connected[2];
	struct input_device dev[2];
	bool armed[INPUT_DEVICE_COUNT];
	bool pub_connected[INPUT_DEVICE_COUNT];
	int pub_category[INPUT_DEVICE_COUNT];
};
static void st_snap(struct st_snapshot* s) {
	memset(s, 0, sizeof(*s));
	for(unsigned p = 0; p < 2; p++) {
		s->has[p] = input_player_device(p, &s->dev[p]);
		s->connected[p] = input_player_frame(p) && input_player_frame(p)->connected;
	}
	for(unsigned d = 0; d < INPUT_DEVICE_COUNT; d++) {
		s->armed[d] = status(d)->armed;
		s->pub_connected[d] = status(d)->published.connected;
		s->pub_category[d] = (int)status(d)->published.category;
	}
}
static void st_quiet(unsigned step) {
	for(unsigned c = 0; c < 4; c++) {
		pad[c].held = 0;
		pad[c].transfer = false;
		if(wpad[c].attached) enqueue(c, report(wpad[c].type, 0));
	}
	if(input_split_join_claimed()) input_split_join_settle(false);
	for(unsigned k = 0; k < 400; k++) { tick(16); st_check(step); } /* 6.4 s */
	struct st_snapshot a, b;
	st_snap(&a);
	for(unsigned k = 0; k < 200; k++) { tick(16); st_check(step); }
	st_snap(&b);
	if(memcmp(&a, &b, sizeof(a)))
		fprintf(stderr, "step %u: state still moving after quiet input\n", step);
	assert(!memcmp(&a, &b, sizeof(a)) && "quiet input converges");
	for(unsigned c = 0; c < 4; c++) {
		if(wpad[c].attached != status(c)->published.connected
		   || pad[c].attached != status(4 + c)->published.connected)
			fprintf(stderr, "step %u: ch %u wpad %d/%d gc %d/%d\n", step, c,
					wpad[c].attached, status(c)->published.connected,
					pad[c].attached, status(4 + c)->published.connected);
		assert(status(c)->published.connected == wpad[c].attached);
		assert(status(4 + c)->published.connected == pad[c].attached);
	}
	for(unsigned p = 0; p < 2; p++)
		if(a.has[p] && a.connected[p])
			assert(status(st_index(a.dev[p]))->published.connected);
}
static void test_plug_unplug_stress(void) {
	static const u32 types[3] = {WPAD_EXP_NONE, WPAD_EXP_NUNCHUK, WPAD_EXP_CLASSIC};
	const unsigned steps = 20000;
	reset();
	pad_probe_model = true;
	memset(st_lost_at, 0, sizeof(st_lost_at));
	memset(st_was_connected, 0, sizeof(st_was_connected));
	st_rng = 0xC0FFEEu; /* fixed seed: the same sequence every run */
	unsigned plugs = 0, unplugs = 0, transfers = 0, presses = 0, joins = 0;
	for(unsigned step = 0; step < steps; step++) {
		unsigned c = st_rand(4);
		switch(st_rand(12)) {
		case 0: if(!pad[c].attached) plugs++; pad[c].attached = true; break;
		case 1: if(pad[c].attached) unplugs++;
			pad[c].attached = false; pad[c].held = 0; break;
		case 2: case 3:
			if(pad[c].attached) { pad[c].transfer = true; transfers++; }
			break;
		case 4: pad[c].held = pad[c].held ? 0 : PAD_BUTTON_A;
			if(pad[c].held) presses++;
			break;
		case 5:
			if(!wpad[c].attached) plugs++;
			wpad[c].attached = true;
			wpad[c].type = types[st_rand(3)];
			enqueue(c, report(wpad[c].type, 0));
			break;
		case 6: if(wpad[c].attached) unplugs++; wpad[c].attached = false; break;
		case 7:
			if(wpad[c].attached) {
				u32 a = wpad[c].type == WPAD_EXP_CLASSIC ? WPAD_CLASSIC_BUTTON_A
														  : WPAD_BUTTON_A;
				bool down = st_rand(2);
				enqueue(c, report(wpad[c].type, down ? a : 0));
				if(down) presses++;
			}
			break;
		case 8:
			if(wpad[c].attached) { /* expansion swap / pull */
				wpad[c].type = types[st_rand(3)];
				enqueue(c, report(wpad[c].type, 0));
			}
			break;
		case 9: input_split_join_arm(st_rand(2) != 0); break;
		default: break;
		}
		unsigned ms = st_rand(5) ? 16 : 1 + st_rand(400);
		tick(ms);
		if(input_split_join_claimed()) { joins++; input_split_join_settle(st_rand(2) != 0); }
		st_check(step);
		if(step % 2500 == 2499) st_quiet(step);
	}
	input_split_join_arm(false);
	st_quiet(steps);
	printf("PASS notice-spot-1031: %u seeded plug/unplug steps (plugs %u, unplugs %u, "
		   "transfer noise %u, A presses %u, split joins %u) keep one device per "
		   "player, own-player notices at the top-left anchor, and converge\n",
		   steps, plugs, unplugs, transfers, presses, joins);
}

int main(void) {
	assert(config_create(&gstate.config_user, "config_wii.json"));
	texture_font.width = texture_font.height = 256;
	gutil_reset_font(&texture_font);
	test_join_prompts();
	test_loss_and_retained();
	test_invalid_and_publication();
	test_identity_reversal();
	test_coalesced_edges();
	test_original_container_bindings();
	test_sort_parked_every_device();
	test_wii_battery_and_pointer_feed();
	test_recovery_dialog();
	test_unbind_rearm();
	test_reclaim_channel();
	test_classic_resting_bias();
	test_reconnect_reporting();
	test_gc_hotplug_after_boot();
	test_vacant_seat_takeover();
	test_chat_opener_remote();
	test_chat_opener_pointer_look_preset();
	test_chat_opener_fails_closed();
	test_chat_opener_classic();
	test_chat_opener_classic_dpad_up();
	test_chat_opener_gamecube_and_legacy();
	test_shipped_cavex_scheme();
	test_hotbar_remote_tap();
	test_hotbar_remote_press_edge();
	test_hotbar_hitch_tolerance();
	test_hotbar_guest();
	test_debug_chord_order();
	test_hotbar_other_families();
	test_binding_fallbacks();
	test_sneak_toggle_remote();
	test_sneak_toggle_vehicle_level();
	test_sneak_toggle_clears_from_input_layer();
	test_sneak_toggle_classic_gamecube();
	test_gc_transient_transfer_keeps_input();
	test_sneak_toggle_other_families_guest();
	test_sneak_double_duty_handoffs();
	test_sneak_hold_families_and_optout();
	test_sneak_toggle_guest_isolation();
	test_open_inventory_role();
	test_remote_menu_handoffs();
	test_gamecube_menu_handoffs();
	test_close_role_return_handoffs();
	test_chat_opener_accessor();
	test_primary_look_preferences_wii();
	test_controller_settings_screen();
	test_page_swap_once_per_press();
	test_plug_unplug_stress();
	config_destroy(&gstate.config_user);
	return 0;
}
