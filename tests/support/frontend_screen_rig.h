/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * Host rig for tests/test_frontend_no_pointer.c: the libogc driver doubles the
 * REAL PLATFORM_WII source/platform/input.c calls, plus the link-only engine
 * doubles the seven front-end screens reference but this focused target never
 * reaches.
 *
 * This is NOT a second input mock. It is the SAME driver-double shape
 * tests/test_input_wii_adapter.c established against the same fake libogc
 * headers in tests/support/wii_input/: reports are enqueued, WPAD_ReadPending
 * coalesces them exactly as devkitPro's wiiuse/wpad.c does, and everything
 * above that -- the binding tables, the per-family code filter, the owner
 * latch, the merged mask input_pressed() reads -- is production code. No
 * IB_* level is ever written by hand.
 *
 * Driver contract: devkitPro/libogc wiiuse/wpad.c, WPAD_ReadPending.
 */

#ifndef TEST_FRONTEND_SCREEN_RIG_H
#define TEST_FRONTEND_SCREEN_RIG_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ogc/lwp_heap.h>
#include <ogc/pad.h>
#include <wiiuse/wpad.h>

#include "config.h"
#include "game/game_state.h"
#include "game/gui/creative_picker_list.h"
#include "game/gui/loading_notice.h"
#include "game/gui/screen.h"
#include "game/split_recovery_script.h"
#include "graphics/gui_util.h"
#include "graphics/texture_atlas.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_remote.h"
#include "platform/audio.h"
#include "platform/audio_music.h"
#include "platform/gfx.h"
#include "platform/hw_telemetry.h"
#include "platform/input.h"
#include "platform/menu_sound.h"
#include "platform/net_socket.h"
#include "platform/texture.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "platform/watchdog.h"
#include "platform/wii/net_bringup.h"
#include "worldgen/worldgen.h"

/* ---- the libogc driver doubles ----------------------------------------- */

static struct {
	bool attached;
	u32 type;
	WPADData retained, queue[8];
	size_t count;
} fe_wpad[4];
static struct {
	bool attached;
	u16 held, previous, down;
	s8 x, y, cx, cy;
} fe_pad[4];
static unsigned fe_scans;
static ptime_t fe_now;

s32 WPAD_Init(void) { return 0; }
s32 WPAD_Shutdown(void) { return 0; }
s32 WPAD_Probe(s32 channel, u32* type) {
	assert(channel >= 0 && channel < 4);
	if(!fe_wpad[channel].attached)
		return WPAD_ERR_NO_CONTROLLER;
	*type = fe_wpad[channel].type;
	return WPAD_ERR_NONE;
}
s32 WPAD_SetDataFormat(s32 channel, s32 format) {
	assert(channel >= 0 && channel < 4 && format == WPAD_FMT_BTNS_ACC_IR);
	return WPAD_ERR_NONE;
}
s32 WPAD_SetVRes(s32 channel, u32 width, u32 height) {
	assert(channel >= 0 && channel < 4 && width == 640 && height == 480);
	return WPAD_ERR_NONE;
}
WPADData* WPAD_Data(int channel) { return &fe_wpad[channel].retained; }
u32 WPAD_ButtonsHeld(int channel) { return WPAD_Data(channel)->btns_h; }
void WPAD_IR(int channel, struct ir_t* ir) { *ir = WPAD_Data(channel)->ir; }
void WPAD_Expansion(int channel, struct expansion_t* exp) {
	if(fe_wpad[channel].attached)
		*exp = WPAD_Data(channel)->exp;
}
s32 WPAD_ReadPending(s32 channel, WPADDataCallback callback) {
	assert(channel == WPAD_CHAN_ALL && callback);
	unsigned dequeued = 0;
	for(int c = 0; c < 4; c++) {
		u32 before = fe_wpad[c].retained.btns_h, coalesced = before;
		u32 seen = 0;
		for(size_t i = 0; i < fe_wpad[c].count; i++) {
			WPADData report = fe_wpad[c].queue[i];
			fe_wpad[c].retained = report;
			callback(c, &fe_wpad[c].retained);
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
		fe_wpad[c].retained.btns_h = coalesced;
		fe_wpad[c].retained.btns_d = coalesced & ~before;
		fe_wpad[c].retained.btns_u = before & ~coalesced;
		fe_wpad[c].count = 0;
	}
	return (s32)dequeued;
}
u32 PAD_Init(void) { return 0; }
u32 PAD_Reset(u32 mask) { (void)mask; return 1; }
u32 PAD_ScanPads(void) {
	u32 mask = 0;
	fe_scans++;
	for(unsigned c = 0; c < 4; c++) {
		if(fe_pad[c].attached)
			mask |= 1u << c; /* libogc PAD_ScanPads convention */
		fe_pad[c].down = fe_pad[c].held & ~fe_pad[c].previous;
		fe_pad[c].previous = fe_pad[c].held;
	}
	return mask;
}
u16 PAD_ButtonsHeld(int c) { return fe_pad[c].held; }
u16 PAD_ButtonsDown(int c) { return fe_pad[c].down; }
s8 PAD_StickX(int c) { return fe_pad[c].x; }
s8 PAD_StickY(int c) { return fe_pad[c].y; }
s8 PAD_SubStickX(int c) { return fe_pad[c].cx; }
s8 PAD_SubStickY(int c) { return fe_pad[c].cy; }

/* Deterministic millisecond clock; no wall-clock sleeps anywhere. */
ptime_t time_get(void) { return fe_now; }
ptime_t time_add_ms(ptime_t t, unsigned ms) { return t + ms; }
int32_t time_diff_ms(ptime_t f, ptime_t s) { return (int32_t)(s - f); }
float time_diff_s(ptime_t f, ptime_t s) { return (float)(s - f) / 1000.0F; }
bool time_reached(ptime_t t, ptime_t deadline) { return t >= deadline; }
void time_reset(void) { fe_now = 0; }

/* The framebuffer these screens lay themselves out in. */
int gfx_width(void) { return 640; }
int gfx_height(void) { return 480; }

/* ---- screen_set: the observation point --------------------------------- */

/* The REAL screen.c is deliberately not linked (it would drag the whole
 * notice/recovery overlay in for no coverage). Every "closes to its parent"
 * and "opens the confirm screen" assertion in this suite is a statement about
 * which screen the handler ASKED for, which is exactly this call. reset() is
 * run for the requested screen so a follow-up screen is driven in the state
 * production would hand it. */
static struct screen* fe_screen_last;
static unsigned fe_screen_sets;
void screen_set(struct screen* s) {
	assert(s);
	fe_screen_last = s;
	fe_screen_sets++;
	gstate.players[0].current_screen = s;
	if(s->reset)
		s->reset(s, gfx_width(), gfx_height());
}
/* toast-1030: menus publish their controller-notice mount here (screen.c's
 * overlay, not linked in this rig); the rect is what a suite may assert. */
#ifndef FE_RIG_OWN_NOTICE_COMPOSE
void screen_notice_compose(int x, int y, int w, int h, bool in_header) {
	(void)x; (void)y; (void)w; (void)h; (void)in_header;
}
#endif

/* ---- audio ------------------------------------------------------------- */

static unsigned fe_ui_sounds;
void audio_play_ui(uint16_t sound_id, float volume, float pitch) {
	(void)sound_id;
	(void)volume;
	(void)pitch;
	fe_ui_sounds++;
}
void audio_menu_device_event(enum menu_sound_event event,
							 struct input_device device, ptime_t when) {
	(void)event;
	(void)device;
	(void)when;
}
void audio_menu_battery_rearm(struct input_device device) { (void)device; }

/* ---- link-only engine doubles ------------------------------------------ */

/* Platform diagnostics input.c publishes for the Wii overlay; unreachable in
 * a headless run, so an assert is the honest body. */
heap_cntrl __wkspace_heap;
u32 __lwp_heap_getinfo(heap_cntrl* heap, heap_iblock* info) {
	(void)heap;
	(void)info;
	assert(false);
	return 1;
}
u32 __lwp_wkspace_heapsize(void) {
	assert(false);
	return 0;
}

struct game_state gstate;

bool autopilot_force_button(enum input_button b, bool* out) {
	(void)b;
	(void)out;
	return false;
}
bool menu_autopilot_active(void) { return false; }
bool menu_autopilot_force_button(enum input_button b, bool* out) {
	(void)b;
	(void)out;
	return false;
}
bool menu_autopilot_force_pointer(float* x, float* y, float* angle,
								  bool* valid) {
	(void)x;
	(void)y;
	(void)angle;
	(void)valid;
	return false;
}

/* SEAM S1 (source/game/chat_login_script.h): the scripted login pad is not
 * linked here, so these are the DISARMED answers -- which is exactly the
 * behaviour under test (today's input.c, untouched). */
struct chat_login_pad;
struct input_identity;
bool chat_login_pad_for(unsigned player,
						int map[IB_COUNT][INPUT_BINDINGS_PER_ACTION],
						const size_t* lengths, struct chat_login_pad* out) {
	(void)player;
	(void)map;
	(void)lengths;
	(void)out;
	return false;
}
bool chat_login_pad_key(void* user, int key) {
	(void)user;
	(void)key;
	return false;
}
bool chat_login_pad_native_key(int key, bool* value) {
	(void)key;
	(void)value;
	return false;
}
bool chat_login_pad_inventory(struct input_device device,
							  struct input_identity* identity, bool* connected,
							  bool* fresh, bool* neutral) {
	(void)device;
	(void)identity;
	(void)connected;
	(void)fresh;
	(void)neutral;
	return false;
}

/* ---- render-side doubles ----------------------------------------------- */

/* Most of this suite's defects are in update(), and these doubles exist to
 * satisfy the link for it. Since push-0921 A23 (review A20-2) ONE case also
 * RENDERS -- fe_render() below -- because the navigation hint's whole
 * deliverable is a visible line of text and nothing here could see whether
 * render2D drew it. The quads/text go nowhere (a headless run has no
 * framebuffer); what the test reads is gui_util.c's own draw observer, which
 * only the real gutil_menu_nav_hint can write. The two that ASSERT below stay
 * asserts on purpose: neither list screen's render2D reaches them, and a
 * future case that does needs a real double, not a silent fake frame. */
struct tex_gfx texture_menu_reception;
struct tex_gfx texture_title_logo;
struct tex_gfx texture_controller_pictures, texture_approved_controls,
	texture_gui_crafting, texture_controls, texture_font, texture_terrain,
	texture_gui_widgets, texture_gui_chest, texture_pointer, texture_items;
void gfx_blending(enum gfx_blend mode) { (void)mode; } /* hint strip / card A set MODE_BLEND */
void gfx_texture(bool enabled) { (void)enabled; }
void gfx_bind_texture(struct tex_gfx* t) { (void)t; }
void gfx_draw_quads(size_t count, const int16_t* v, const uint8_t* color,
					const uint16_t* uv) {
	(void)count;
	(void)v;
	(void)color;
	(void)uv;
}
void gfx_draw_quads_flt(size_t count, const float* v, const uint8_t* color,
						const float* uv) {
	(void)count;
	(void)v;
	(void)color;
	(void)uv;
}
/* gui_util's bounded inventory-icon draw transaction. */
static unsigned fe_icon_render_phase;
void gfx_depth_range(float a, float b) {
	assert(a == 0);
	if(fe_icon_render_phase == 1) {
		assert(b == 0.1F);
		fe_icon_render_phase = 2;
	} else {
		assert(fe_icon_render_phase == 5 && b == 1);
		fe_icon_render_phase = 0;
	}
}
void gfx_write_buffers(bool color, bool depth, bool test) {
	assert(color);
	if(fe_icon_render_phase == 2) {
		assert(depth && test);
		fe_icon_render_phase = 3;
	} else {
		assert(fe_icon_render_phase == 4 && !depth && !test);
		fe_icon_render_phase = 5;
	}
}
void gfx_scissor(bool enable, uint32_t x, uint32_t y, uint32_t width,
				 uint32_t height) {
	(void)enable;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
}
/* The dirt-tile lookup gutil_bg() makes -- reached since A23, because the
 * navigation-hint case renders the real screens. It used to assert. A headless
 * run has no pixels (every quad above is a no-op) and nothing in this suite
 * reads the returned index, so a fixed one is as honest as a copy of
 * texture_atlas.c's table would be, and cannot drift from it. */
uint8_t tex_atlas_lookup(enum tex_atlas_entry e) {
	(void)e;
	return 0;
}
void tex_gfx_lookup(struct tex_gfx* t, int x, int y, uint8_t* color) {
	(void)t;
	(void)x;
	(void)y;
	memset(color, 255, 4);
}
/* Bounded rendering fixture for Load's two inventory icons. It records the
 * real gui_util renderer dispatch; pixel/pack appearance remains native proof. */
static unsigned fe_load_icon_draws;
static void fe_load_icon_render(struct item* definition, struct item_data* item,
							   mat4 model, bool inventory, enum render_item_env env) {
	(void)definition;
	assert(item && (item->id == 4 || item->id == 58) && item->count == 1);
	assert(inventory && env == R_ITEM_ENV_INVENTORY);
	assert(model[0][0] > 0 && model[1][1] > 0);
	assert(fe_icon_render_phase == 3);
	fe_icon_render_phase = 4;
	fe_load_icon_draws++;
}
struct item* item_get(struct item_data* item) {
	static struct item icon = {.renderItem = fe_load_icon_render};
	assert(item && (item->id == 4 || item->id == 58));
	assert(fe_icon_render_phase == 0);
	fe_icon_render_phase = 1;
	return &icon;
}

/* ---- the screens this suite does not drive ------------------------------ */

/* Destinations the seven owned screens hand off to. Each one is only ever
 * compared by ADDRESS here ("Close went to the title", "Delete opened the
 * confirm screen"), so an inert vtable is the whole requirement -- linking
 * the real screen_load_world.c/screen_create_world.c would drag the world,
 * server and network stacks into a focused input test for no coverage. */
struct screen screen_ingame = {0};
struct screen screen_load_world = {0};
struct screen screen_inventory = {0};
struct screen screen_options = {0};
struct screen screen_rename_world = {0};
#ifndef TEST_FRONTEND_PLAY_FLOW
/* test_frontend_no_pointer reaches Create only by ADDRESS, so an inert vtable
 * is the whole requirement there. */
struct screen screen_create_world = {0};
#else
/* test_frontend_play_flow (VISUAL-COORDINATION section 15, superseding 14)
 * compiles the REAL screen_create_world.c, so the double above would be a
 * duplicate definition -- and driving Create is the entire point of that
 * target. It omits the pack screen instead, which the title only hands off to
 * by address. The real screen owns screen_texturepacks_set_return, which the
 * REAL title calls on its Texture packs row, so that gets a no-op double too:
 * nothing in this target ever opens the pack screen to return FROM. */
struct screen screen_texturepacks = {0};
void screen_texturepacks_set_return(struct screen* s) { (void)s; }
#endif
/* main.c registers these two at boot; nothing here runs main(). */
struct screen* screen_multiplayer_setup = NULL;
struct screen* screen_multiplayer_connect = NULL;
void screen_options_set_return(struct screen* s) { (void)s; }
/* The rename editor's own screen is not linked (it would drag the OSK widget
 * and the world-directory writer into a focused input test), but WHICH world
 * Rename handed it is exactly the "the strip acts on the selected row" claim
 * push-0921 A15 has to prove -- so the double RECORDS its arguments instead of
 * discarding them. Cleared by fe_open through fe_rename_forget(). */
static char fe_rename_path[512], fe_rename_name[160];
static unsigned fe_rename_setups;
void screen_rename_world_setup(const char* directory_path,
							   const char* current_name) {
	snprintf(fe_rename_path, sizeof(fe_rename_path), "%s",
			 directory_path ? directory_path : "");
	snprintf(fe_rename_name, sizeof(fe_rename_name), "%s",
			 current_name ? current_name : "");
	fe_rename_setups++;
}
static void fe_rename_forget(void) {
	fe_rename_path[0] = fe_rename_name[0] = '\0';
	fe_rename_setups = 0;
}
/* Java String.trim, copied verbatim from screen_create_world.c:129-141
 * [740c583 String.trim] rather than linking that whole screen for one
 * five-line helper. The MP address fields call it on every keystroke.
 * test_frontend_play_flow links the real screen, so it takes the real one. */
#ifndef TEST_FRONTEND_PLAY_FLOW
void screen_cworld_java_trim(const char* in, char* out, size_t out_cap) {
	const char* s = in;
	const char* e = in + strlen(in);
	while(s < e && (unsigned char)*s <= ' ')
		s++;
	while(e > s && (unsigned char)e[-1] <= ' ')
		e--;
	size_t n = (size_t)(e - s);
	if(n >= out_cap)
		n = out_cap - 1;
	memcpy(out, s, n);
	out[n] = 0;
}
#endif

/* ---- world-list backing ------------------------------------------------- */

/* The select-world rows resolve through level_archive. This rig builds its
 * saves directories with mkdir alone (no level.dat), so the archive layer is
 * doubled to report "present, survival, no metadata" -- the row state the
 * screen's own reachability rules key on (a WORLD_ROW_UNREADABLE row refuses
 * Play, which would hide the very defect case (c) is about). Nothing here
 * touches worldgen or NBT. */
bool level_archive_create(struct level_archive* la, string_t filename) {
	(void)filename;
	memset(la, 0, sizeof(*la));
	la->data = (void*)&fe_screen_sets; /* non-NULL: destroy asserts on it */
	return true;
}
#ifdef TEST_FRONTEND_PLAY_FLOW
/* "Did anything try to CREATE a world?" -- the one thing test_frontend_play_flow
 * needs that a false return cannot tell it apart from "nothing was attempted".
 * Counting only; the answer stays false, so every caller's failure path is the
 * one that runs, exactly as before. */
static unsigned fe_world_create_attempts;
#endif
bool level_archive_create_new(struct level_archive* la, string_t directory,
							  int64_t seed, vec3 spawn) {
	(void)la;
	(void)directory;
	(void)seed;
	(void)spawn;
#ifdef TEST_FRONTEND_PLAY_FLOW
	fe_world_create_attempts++;
#endif
	return false;
}
bool level_archive_read(struct level_archive* la, struct level_archive_tag tag,
						void* result, size_t length) {
	(void)la;
	(void)tag;
	(void)result;
	(void)length;
	return false;
}
bool level_archive_write(struct level_archive* la, struct level_archive_tag tag,
						 void* data) {
	(void)la;
	(void)tag;
	(void)data;
	return false;
}
bool level_archive_write_game_mode(struct level_archive* la, bool creative) {
	(void)la;
	(void)creative;
	return false;
}
bool level_archive_write_player(struct level_archive* la, vec3 position,
								vec2 rotation, vec3 velocity,
								enum world_dim dimension) {
	(void)la;
	(void)position;
	(void)rotation;
	(void)velocity;
	(void)dimension;
	return false;
}
bool level_archive_is_creative(const struct level_archive* la) {
	(void)la;
	return false;
}
/* convert-notice-1024: the rig's rows are CavEX-created worlds at the current
 * save version, so Play loads directly (no Beta convert notice) exactly as
 * before; the notice itself is covered by tests/test_import_convert_notice.c. */
bool level_archive_was_created_by_cavewii(const struct level_archive* la) {
	(void)la;
	return true;
}
int32_t level_archive_get_save_version(const struct level_archive* la) {
	(void)la;
	return CAVEWII_SAVE_VERSION_CURRENT;
}
/* dropin-1029: the convert notice now also asks where the world came from;
 * the rig's rows are CavEX-created (LEVEL_ORIGIN_CAVEWII). */
enum level_world_origin level_archive_world_origin(const struct level_archive* la) {
	(void)la;
	return LEVEL_ORIGIN_CAVEWII;
}
bool level_archive_save(struct level_archive* la) {
	(void)la;
	return false;
}
void level_archive_destroy(struct level_archive* la) { (void)la; }
void level_archive_discard(struct level_archive* la) { (void)la; }
void worldgen_find_initial_spawns(int64_t seed, bool superflat,
								  bool start_nether, float world_spawn[3],
								  float player_spawn[3]) {
	(void)seed;
	(void)superflat;
	(void)start_nether;
	world_spawn[0] = world_spawn[1] = world_spawn[2] = 0;
	player_spawn[0] = player_spawn[1] = player_spawn[2] = 0;
}
/* R3: the create tail's spawn search goes through server_local_create_spawns
 * (real: the server thread's live-world search). Rigs have no server: the
 * same in-thread stub search as worldgen_find_initial_spawns above. */
bool server_local_create_spawns(const char* dir, int64_t seed, bool superflat,
								bool start_nether, float world_spawn[3],
								float player_spawn[3],
								void (*idle)(void* user), void* user) {
	(void)dir;
	(void)idle;
	(void)user;
	worldgen_find_initial_spawns(seed, superflat, start_nether, world_spawn,
								 player_spawn);
	return true;
}
void spawn_search_wait_idle(void* user) { (void)user; }
bool split_recovery_rig_create_allowed(const char* directory) {
	(void)directory;
	return false;
}

/* ---- server / client interface ------------------------------------------ */

static unsigned fe_rpcs;
static int fe_rpc_last = -1;
static int fe_rpc_item = -1;
bool svin_rpc_send(struct server_rpc* call) {
	fe_rpcs++;
	fe_rpc_last = (int)call->type;
	if(call->type == SRPC_CREATIVE_GIVE)
		fe_rpc_item = (int)call->payload.creative_give.id;
	return true;
}
bool clin_send_unload_world(void) { return true; }

/* ---- MP transport ------------------------------------------------------- */

/* No case in this suite opens a socket, pings, or connects: the connect
 * screen is driven only through its ERROR page (Retry / Back), which
 * mpc_error/mpc_state reach without a transport. */
struct server_remote* mp_session_remote(void) { return NULL; }
bool mp_session_request_connect(const char* host, uint16_t port,
								const char* username) {
	(void)host;
	(void)port;
	(void)username;
	return false;
}
void mp_session_cancel_pending(void) {}
bool mp_session_take_drop_notice(char* reason_out, size_t reason_cap) {
	(void)reason_out;
	(void)reason_cap;
	return false;
}
void server_remote_get_status(struct server_remote* remote,
							  struct server_remote_status* status) {
	(void)remote;
	memset(status, 0, sizeof(*status));
}
void net_bringup_request(void) {}
void net_bringup_cancel(void) {}
enum net_bringup_state net_bringup_status(char* error, size_t error_size) {
	if(error && error_size)
		error[0] = '\0';
	return NET_BRINGUP_READY;
}
bool net_socket_platform_init(char* error, size_t error_size) {
	(void)error;
	(void)error_size;
	return false;
}
void net_socket_platform_cleanup(void) {}
net_socket_t net_socket_connect(const char* host, uint16_t port, char* error,
								size_t error_size) {
	(void)host;
	(void)port;
	(void)error;
	(void)error_size;
	return -1;
}
int net_socket_receive(net_socket_t socket, void* data, size_t length) {
	(void)socket;
	(void)data;
	(void)length;
	return -1;
}
int net_socket_send(net_socket_t socket, const void* data, size_t length) {
	(void)socket;
	(void)data;
	(void)length;
	return -1;
}
void net_socket_close(net_socket_t socket) { (void)socket; }
void* mp_backing_alloc(size_t length) { (void)length; return NULL; }
void mp_backing_free(void* body) { (void)body; }
bool mp_backing_owns(const void* body) { (void)body; return false; }
bool thread_spawn_disposable(struct thread* t, void* (*entry)(void* arg),
							 void* arg, uint8_t priority) {
	(void)t;
	(void)entry;
	(void)arg;
	(void)priority;
	return false;
}
void thread_dispose(struct thread* t, bool completed) {
	(void)t;
	(void)completed;
}
bool thread_mutex_init(struct thread_mutex* m) { (void)m; return true; }
void thread_mutex_destroy(struct thread_mutex* m) { (void)m; }
void thread_mutex_lock(struct thread_mutex* m) { (void)m; }
void thread_mutex_unlock(struct thread_mutex* m) { (void)m; }

/* ---- texture-pack apply / presentation ---------------------------------- */

bool tex_init(void) { return true; }
void audio_registry_reload(void) {}
void audio_music_registry_reload(void) {}
/* asyncsnd-0926: the opt-in async sound load is OFF in every rig -- the gate
 * is transparent (the pre-gate sequence: queue the RPC, set the load
 * screen), exactly what production does with "audio.async_load" off. */
bool audio_registry_async_pending(void) { return false; }
/* ui-build-1026/T10: screen_convert_backup.c drives convert-only through
 * client_session.c / server_local.c, which no rig links. No local provider
 * runs here: a start is refused (the screen shows its honest error), nothing
 * settles, and no import progress is reported. */
bool client_session_convert_start(struct client_session* session,
								  const char* path) {
	(void)session;
	(void)path;
	return false;
}
bool client_session_convert_poll(struct client_session* session) {
	(void)session;
	return false;
}
bool server_local_import_progress(uint32_t* percent) {
	if(percent)
		*percent = UINT32_MAX;
	return false;
}
bool loading_notice_gate_remote_connect(void) { return false; }
bool loading_notice_gate_load_world(const char* world_path,
									struct screen* back) {
	(void)back;
	struct server_rpc rpc;
	rpc.type = SRPC_LOAD_WORLD;
	string_init_set_str(rpc.payload.load_world.name, world_path);
	if(svin_rpc_send(&rpc)) {
		screen_set(&screen_load_world);
		return true;
	}
	string_clear(rpc.payload.load_world.name);
	return false;
}
int audio_music_track_count(void) { return 0; }
void loading_notice_draw_strip(int width, int height, const char* primary) {
	(void)width;
	(void)height;
	(void)primary;
}
void loading_notice_capture(const char* tag) { (void)tag; }
bool gfx_can_present(void) { return false; }
void gfx_clear_buffers(uint8_t r, uint8_t g, uint8_t b) {
	(void)r;
	(void)g;
	(void)b;
}
void gfx_finish(bool vsync) { (void)vsync; }
void gfx_flip_buffers(float* gpu_wait, float* vsync_wait) {
	if(gpu_wait)
		*gpu_wait = 0;
	if(vsync_wait)
		*vsync_wait = 0;
}
void gfx_mode_gui(void) {}
bool hw_telemetry_active(void) { return false; }
bool hw_telemetry_quiesced(void) { return true; }
bool hw_telemetry_emit_observer(const char* line) { (void)line; return false; }
void watchdog_expect_stall_ms(uint32_t ms) { (void)ms; }
void watchdog_phase_main(enum wd_phase_main p) { (void)p; }

/* ---- creative picker item source ---------------------------------------- */

/* The picker's ITEM list is not what item 13 is about -- the defect is that a
 * stationary pointer overwrites directional focus every update. A fixed
 * two-page catalogue keeps the grid, the page flip and the button row exactly
 * as reachable as the real registry makes them, without linking items. */
#define FE_PICKER_ITEMS 100
size_t creative_picker_count(void) { return FE_PICKER_ITEMS; }
bool creative_picker_entry(size_t index, struct item_data* out) {
	if(index >= FE_PICKER_ITEMS)
		return false;
	memset(out, 0, sizeof(*out));
	out->id = (uint16_t)(1 + index);
	out->count = 1;
	return true;
}

/* ---- rig helpers ------------------------------------------------------- */

enum fe_family {
	FE_REMOTE = 0, /* Wii Remote + Nunchuk */
	FE_CLASSIC,
	FE_GAMECUBE,
	FE_FAMILY_COUNT,
};

static const char* const fe_family_name[FE_FAMILY_COUNT]
	= {"Wii Remote + Nunchuk", "Classic Controller", "GameCube"};

static enum fe_family fe_family;
static bool fe_ir_armed;
static float fe_ir_x, fe_ir_y;
/* A3/H-1: when set, fe_attach brings the family up with a LIVE IR pointer
 * parked on `fe_live_x/_y` -- a spot that hits no control on any of the seven
 * screens. Every A1 case then runs a second time with a pointer present
 * instead of being duplicated, which is the whole of the reviewer's "no case
 * runs a list screen with the pointer available". Wii Remote only: the
 * Classic is refused a pointer by input_owner_latch and the GameCube has
 * none, so for those two families the flag is inert and the leg degenerates
 * to the pointer-free one (stated, not hidden). */
static bool fe_live_pointer;
#define FE_LIVE_X 5.0F
#define FE_LIVE_Y 5.0F

/* The one physical-code -> device-bit table this rig owns. It is the inverse
 * of input.c's own input_wpad_translate/input_pad_translate, written out here
 * because the test names PHYSICAL BUTTONS (Remote Minus, Classic full L, GC
 * Y) and the production translators are static. Anything that drifts between
 * the two shows up immediately as a role that stops firing. */
static u32 fe_wpad_bit(int code) {
	switch(code) {
		case 0: return WPAD_BUTTON_UP;
		case 1: return WPAD_BUTTON_DOWN;
		case 2: return WPAD_BUTTON_LEFT;
		case 3: return WPAD_BUTTON_RIGHT;
		case 4: return WPAD_BUTTON_A;
		case 5: return WPAD_BUTTON_B;
		case 6: return WPAD_BUTTON_1;
		case 7: return WPAD_BUTTON_2;
		case 8: return WPAD_BUTTON_PLUS;
		case 9: return WPAD_BUTTON_MINUS;
		case 10: return WPAD_BUTTON_HOME;
		case 100: return WPAD_NUNCHUK_BUTTON_Z;
		case 101: return WPAD_NUNCHUK_BUTTON_C;
		case 200: return WPAD_CLASSIC_BUTTON_UP;
		case 201: return WPAD_CLASSIC_BUTTON_DOWN;
		case 202: return WPAD_CLASSIC_BUTTON_LEFT;
		case 203: return WPAD_CLASSIC_BUTTON_RIGHT;
		case 204: return WPAD_CLASSIC_BUTTON_A;
		case 205: return WPAD_CLASSIC_BUTTON_B;
		case 206: return WPAD_CLASSIC_BUTTON_X;
		case 207: return WPAD_CLASSIC_BUTTON_Y;
		case 208: return WPAD_CLASSIC_BUTTON_ZL;
		case 209: return WPAD_CLASSIC_BUTTON_ZR;
		case 210: return WPAD_CLASSIC_BUTTON_FULL_L;
		case 211: return WPAD_CLASSIC_BUTTON_FULL_R;
		case 212: return WPAD_CLASSIC_BUTTON_PLUS;
		case 213: return WPAD_CLASSIC_BUTTON_MINUS;
		case 214: return WPAD_CLASSIC_BUTTON_HOME;
		default: assert(false); return 0;
	}
}
static u16 fe_pad_bit(int code) {
	switch(code) {
		case 400: return PAD_BUTTON_UP;
		case 401: return PAD_BUTTON_DOWN;
		case 402: return PAD_BUTTON_LEFT;
		case 403: return PAD_BUTTON_RIGHT;
		case 404: return PAD_BUTTON_A;
		case 405: return PAD_BUTTON_B;
		case 406: return PAD_BUTTON_X;
		case 407: return PAD_BUTTON_Y;
		case 408: return PAD_TRIGGER_Z;
		case 409: return PAD_TRIGGER_L;
		case 410: return PAD_TRIGGER_R;
		case 411: return PAD_BUTTON_START;
		default: assert(false); return 0;
	}
}

static u32 fe_expansion(void) {
	return fe_family == FE_CLASSIC ? (u32)WPAD_EXP_CLASSIC
								   : (u32)WPAD_EXP_NUNCHUK;
}

/* One hardware poll with `codes` (0-terminated by count) held down. */
static void fe_poll_with(const int* codes, size_t count) {
	if(fe_family == FE_GAMECUBE) {
		u16 held = 0;
		for(size_t k = 0; k < count; k++)
			held |= fe_pad_bit(codes[k]);
		fe_pad[0].held = held;
	} else {
		u32 type = fe_expansion();
		WPADData data = {.err = WPAD_ERR_NONE,
						 .data_present = WPAD_DATA_BUTTONS | WPAD_DATA_EXPANSION,
						 .exp = {.type = (int)type}};
		for(size_t k = 0; k < count; k++)
			data.btns_h |= fe_wpad_bit(codes[k]);
		if(fe_ir_armed && fe_family == FE_REMOTE) {
			data.data_present |= WPAD_DATA_IR;
			data.ir.valid = 1;
			data.ir.x = fe_ir_x;
			data.ir.y = fe_ir_y;
		}
		assert(fe_wpad[0].count < 8);
		fe_wpad[0].queue[fe_wpad[0].count++] = data;
	}
	fe_now += 16;
	input_poll();
}

static void fe_idle(void) { fe_poll_with(NULL, 0); }
static void fe_hold(int code) { fe_poll_with(&code, 1); }

/* ---- the DRAW OBSERVER (push-0921 A23, review A20-2) -------------------- */

/* Render ONE frame of `s` through its REAL render2D, with gui_util.c's
 * navigation-hint observer armed. Everything the screen draws goes to the
 * no-op gfx doubles above; what survives is the observer's record of what
 * gutil_menu_nav_hint actually put on the frame.
 *
 * This is the whole point: the suite's hint assertions used to call the
 * screens' pure hint/rect functions themselves, which is a SECOND OPINION on
 * what render2D would do -- deleting the draw call left them green. Anything
 * asserted through fe_render() is evidence instead. */
static void fe_render(struct screen* s) {
	assert(s && s->render2D);
	gutil_menu_nav_hint_observe_reset();
	s->render2D(s, gfx_width(), gfx_height());
}

/* What the frame just rendered drew, if anything: false when the hint was not
 * drawn at all. `text` / `r` are the string and rectangle gutil_text received.
 * A hint drawn more than once in one frame is a defect in itself, so it is
 * asserted here rather than left for each caller. */
static bool fe_hint_drawn(char* text, size_t cap,
						  struct gutil_button_bounds* r) {
	unsigned draws = gutil_menu_nav_hint_observed(text, cap, r);
	assert(draws <= 1 && "render2D drew the navigation hint twice in a frame");
	return draws == 1;
}

/* Bring `family` up and let it claim player 0 the way a real pad does: the
 * Confirm button. The claim press is consumed by the owner latch's attach
 * block, so the neutral polls after it leave every action re-armed. */
static void fe_attach(enum fe_family family) {
	memset(fe_wpad, 0, sizeof(fe_wpad));
	memset(fe_pad, 0, sizeof(fe_pad));
	fe_family = family;
	fe_ir_armed = fe_live_pointer && family == FE_REMOTE;
	fe_ir_x = FE_LIVE_X;
	fe_ir_y = FE_LIVE_Y;
	fe_now = 1000;
	fe_screen_last = NULL;
	fe_screen_sets = 0;
	fe_ui_sounds = 0;
	input_init();
	if(family == FE_GAMECUBE) {
		fe_pad[0].attached = true;
	} else {
		fe_wpad[0].attached = true;
		fe_wpad[0].type = fe_expansion();
	}
	fe_idle();
	fe_hold(family == FE_GAMECUBE ? 404 : family == FE_CLASSIC ? 204 : 4);
	fe_idle();
	fe_idle();
	assert(input_player_frame(0) && input_player_frame(0)->connected
		   && "the family never claimed player 0");
	assert(!input_pressed(IB_GUI_CLICK)
		   && "the claim press leaked a Confirm edge into the menu");
}

#endif
