/* R2 production quit-loop, owner isolation, cooperative writes and CPU cost. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "support/test_fixture_dir.h"
#include "game/achievements/achievements.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "game/hw_run.h"
#include "network/client_interface.h"
#include "game/gui/screen_stats_save.h"
#include "platform/thread.h"
#include "platform/input_owner.h"
#include "platform/menu_sound.h"
#include "platform/gfx.h"
#include "platform/texture.h"
#include "graphics/gui_util.h"

static char root[128], stats[256];
static struct stats_profile profile, loaded;
static struct input_player_frame frame;
static uint64_t io_thread;
static unsigned saves[2], steps, polls, presents, heartbeats, script_stage;
static int fail_left, choice;
static const char* fail_profile;
static bool quit_driver;
static bool unattended_driver, focus_driver;
static bool stall_writer;
static bool clean_quiesce_driver, primary_connected = true;
static unsigned responding_owner, focus_stage;
static struct input_owner quit_owners[2];
static unsigned text_draws, telemetry_failures;
static unsigned hint_rows;
static struct gutil_hint_row last_hints;
static int last_button_bottom;
static int draw_width = 640, draw_height = 480;
static uint32_t fixed_clock(void);

static uint64_t thread_id(void) {
#ifdef _WIN32
	return GetCurrentThreadId();
#else
	return (uintptr_t)pthread_self();
#endif
}
static bool refuse(const struct stats_profile* p) {
	if(!fail_left || !fail_profile || strcmp(fail_profile, p->profile_id)) return false;
	if(fail_left > 0) fail_left--;
	return true;
}
enum stats_save_status __real_stats_profile_save(const char*, const struct stats_profile*);
enum stats_save_status __wrap_stats_profile_save(const char* dir, const struct stats_profile* p) {
	assert(thread_id() == io_thread);
	saves[strstr(p->profile_id, "two") != NULL]++;
	return refuse(p) ? STATS_SAVE_ERR_WRITE : __real_stats_profile_save(dir, p);
}
void __real_stats_profile_load(const char*, const char*, struct stats_profile*, enum stats_load_status*);
void __wrap_stats_profile_load(const char* dir, const char* id, struct stats_profile* p, enum stats_load_status* st) {
	assert(thread_id() == io_thread);
	__real_stats_profile_load(dir, id, p, st);
}
bool __real_stats_save_step(struct stats_save_job*);
bool __wrap_stats_save_step(struct stats_save_job* job) {
	assert(thread_id() == io_thread);
	steps++;
	if(stall_writer) return false; /* a job that never completes */
	if(!job->done && job->phase == 0) {
		saves[strstr(job->profile->profile_id, "two") != NULL]++;
		if(refuse(job->profile)) {
			job->done = true; job->result = STATS_SAVE_ERR_WRITE;
		}
	}
	bool done = __real_stats_save_step(job);
	assert(job->scanned <= CW_STATS_SAVE_STEP_RECORDS);
	return done;
}

/* The real production quit loop calls these platform functions. The driver
 * supplies released owner frames followed by deliberate navigation/clicks;
 * assertions cap its event pump, so a regression fails instead of hanging. */
void __wrap_input_poll(void) {
	assert(quit_driver && ++polls < (unattended_driver ? 600 : 2000));
	memset(frame.down, 0, sizeof(frame.down));
	memset(frame.pressed, 0, sizeof(frame.pressed));
	if(!achievements_slot_save_failed(0) && !clean_quiesce_driver) script_stage = 0;
	else if(!unattended_driver) {
	unsigned phase = script_stage++;
	if(focus_driver) {
		responding_owner = phase >= 5 && phase <= 7 ? 1 : 0;
		if(phase == 1 || phase == 3) frame.down[IB_GUI_DOWN] = true;
		if(phase == 5 || phase == 6) frame.down[IB_GUI_CLICK] = true;
		if(phase == 8 || phase == 10) frame.down[IB_HOME] = true;
	} else if(responding_owner == 1 && primary_connected && focus_stage < 3) {
		if(focus_stage++ == 1) frame.down[IB_GUI_UP] = true; /* take prompt focus only */
		script_stage = 0;
	} else {
	if(choice == STATS_EXIT_BACK && phase % 2) frame.down[IB_INVENTORY] = true;
	else if(choice == STATS_EXIT_SAVED) {
		if(phase == 1) frame.down[IB_GUI_DOWN] = true;
		if(phase == 2) frame.down[IB_GUI_CLICK] = true;
	} else {
		if(phase == 1 || phase == 3) frame.down[IB_GUI_DOWN] = true;
		if(phase == 5) frame.down[IB_GUI_CLICK] = true;
	}
	}
	}
	/* Production device ownership and neutral/edge latching; only raw
	 * hardware samples are scripted. P1 may remain connected but idle. */
	for(unsigned p=0;p<2;p++) {
	 struct input_player_frame sample=frame;
	 if(p != responding_owner) { memset(sample.down,0,sizeof(sample.down)); memset(sample.pressed,0,sizeof(sample.pressed)); }
	 if(!p && !primary_connected) sample.connected=false;
	 input_owner_latch(&quit_owners[p],&sample);
	}
}
const struct input_player_frame* __wrap_input_player_frame(unsigned slot) {
 return quit_driver ? &quit_owners[slot].frame : slot ? NULL : &frame;
}
bool __real_achievements_shutdown(void);
bool __wrap_achievements_shutdown(void) {
 if(quit_driver && clean_quiesce_driver) return false;
 return __real_achievements_shutdown();
}
bool __wrap_hw_telemetry_emit_observer(const char* line) {
 assert(strstr(line, "achievements") && strstr(line, "fail")); telemetry_failures++; return true;
}
int __wrap_gfx_width(void) { return 640; }
int __wrap_gfx_height(void) { return 480; }
void __wrap_gfx_clear_buffers(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void __wrap_gfx_mode_gui(void) {}
void __wrap_gfx_finish(bool vsync) { assert(vsync); presents++; }
void __wrap_watchdog_heartbeat(void) { heartbeats++; }
void __wrap_audio_menu_event(enum menu_sound_event e, ptime_t t) { (void)e; (void)t; }
void __wrap_gfx_texture(bool on) { (void)on; }
void __wrap_gfx_bind_texture(struct tex_gfx* t) { (void)t; }
void __wrap_gutil_texquad_col(int x, int y, int u, int v, int tw, int th, int w, int h,
	uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	(void)x; (void)y; (void)u; (void)v; (void)tw; (void)th; (void)w; (void)h; (void)r; (void)g; (void)b; (void)a;
}
void __wrap_gutil_texquad_rt_any(int x, int y, float angle, int u, int v, int tw, int th, float w, float h) {
	(void)x; (void)y; (void)angle; (void)u; (void)v; (void)tw; (void)th; (void)w; (void)h;
}
int __wrap_gutil_font_width(const char* s, int size) { return strlen(s) * size / 2; }
void __wrap_gutil_text(int x, int y, const char* s, int size, bool shadow) {
	text_draws++;
	assert(size >= 16 && x >= 0 && y >= 0 && y + size <= draw_height);
	assert(x + (int)strlen(s) * size / 2 <= draw_width); (void)shadow;
}
void __wrap_gutil_button_draw(int x, int y, int w, const char* s, bool enabled, bool hovered) {
	assert(x >= 0 && y >= 0 && x + w * 2 <= draw_width && y + 40 <= draw_height);
	last_button_bottom = y + 40;
	(void)s; (void)hovered; assert(enabled);
}
int __wrap_gutil_prompt_glyph(int x, int y, int size, int glyph) { (void)x; (void)y; (void)glyph; return size; }
/* 10-31: the failure dialog's centred hint row (A1); its cells stay on screen.
 * (gui_util's own strip quads are intra-object calls the texquad wrap misses.) */
void __wrap_gutil_hint_row_draw(const struct gutil_hint_row* row) {
	assert(row && row->count > 0 && row->strip.y >= last_button_bottom);
	last_hints = *row;
	hint_rows++;
	for(int k = 0; row && k < row->count; k++) {
		const struct gutil_button_bounds* r = &row->cells[k].rect;
		assert(r->x >= 0 && r->y >= 0 && r->x + r->width <= draw_width
			   && r->y + r->height <= draw_height);
	}
}

/* G9: main's auto-exit gate also waits for a pending capture to quiesce. */
#include "platform/g9_capture.h"
#include "platform/hw_telemetry.h"
#include "achievements_main_slices.h"

static void boundary(unsigned kind, unsigned slot, uint64_t epoch) {
	game_event_emit(&(struct game_event){.kind = kind, .authority = GE_AUTH_CLIENT,
		.who = {.slot = slot, .generation = epoch}});
}
static void event(unsigned kind, unsigned slot, uint64_t epoch) {
	game_event_emit(&(struct game_event){.kind = kind, .authority = GE_AUTH_CLIENT,
		.who = {.slot = slot, .generation = epoch}});
}
static struct ach_status status(unsigned slot) {
	struct ach_status st; assert(achievements_debug_status(slot, &st)); return st;
}
static unsigned jumps(unsigned slot) {
	return stats_counter_total(&achievements_debug_profile(slot)->general[stat_general_index(2010)]);
}
static void setup(const char* one, const char* two, bool cooperative) {
	fail_left = 0; fail_profile = NULL;
	achievements_set_cooperative_saves(false);
	assert(achievements_shutdown());
	assert(achievements_init(root));
	assert(achievements_set_profiles(one, two));
	achievements_set_cooperative_saves(cooperative);
	boundary(GE_SESSION_BEGIN, 0, 100); boundary(GE_SESSION_BEGIN, 1, 200);
	achievements_tick();
	memset(saves, 0, sizeof(saves)); steps = 0;
	memset(quit_owners,0,sizeof(quit_owners));
	assert(input_owner_bind(quit_owners,2,0,(struct input_device){INPUT_DEVICE_WPAD,0}));
	assert(input_owner_bind(quit_owners,2,1,(struct input_device){INPUT_DEVICE_WPAD,1}));
}
static void test_retry_and_isolation(void) {
	setup("retryone", "retrytwo", false);
	event(GE_JUMPED, 1, 200);
	fail_profile = "retrytwo"; fail_left = -1;
	boundary(GE_SESSION_END, 1, 200); achievements_tick();
	assert(achievements_slot_save_failed(1) && !status(1).bound);
	boundary(GE_SESSION_BEGIN, 1, 201); achievements_tick();
	assert(status(1).bound && jumps(1) == 1); /* retained profile, no reload/loss */
	unsigned attempts = saves[1];
	for(unsigned retry = 0; retry < 3; retry++) {
		for(unsigned n = 0; n < 5000; n++) boundary(GE_SESSION_CHECKPOINT, 1, 201);
		event(GE_JUMPED, 0, 100); event(GE_INVENTORY_OPENED, 0, 100);
		achievements_tick();
		assert(saves[1] == ++attempts); /* each retry, including second, fires once */
		assert(jumps(0) == retry + 1 && !status(0).progress_incomplete);
		assert(achievements_debug_profile(0)->ach[0].local == retry + 1);
		boundary(GE_SESSION_CHECKPOINT, 0, 100); achievements_tick();
		assert(achievements_save_durable(0) && achievements_slot_save_failed(1));
	}
	for(unsigned n = 0; n < 2000; n++) {
		event(GE_INVENTORY_OPENED, 0, 100); event(GE_JUMPED, 0, 100);
	}
	achievements_tick();
	assert(jumps(0) == 2003 && achievements_debug_profile(0)->ach[0].local == 2003);
	assert(!status(0).progress_incomplete && achievements_slot_save_failed(1));
	boundary(GE_SESSION_CHECKPOINT, 0, 100); achievements_tick();
	assert(achievements_save_durable(0));
	fail_left = 0;
	boundary(GE_SESSION_CHECKPOINT, 1, 201); achievements_tick();
	assert(saves[1] == ++attempts && achievements_save_durable(1));
	assert(!achievements_lifecycle_pending() && jumps(1) == 1);
	puts("PASS persistent P2 error: three deduplicated retries fire; P1 unlocks/counters/saves independent");
}
static void test_empty_coalescer(void) {
	setup("emptyone", "emptytwo", true);
	unsigned baseline = achievements_debug_coalescer_rebuilds();
	for(unsigned n = 0; n < 500; n++) achievements_tick();
	assert(achievements_debug_coalescer_rebuilds() == baseline);
	puts("PASS empty frame drain performs zero coalescer table rebuilds");
}
static void test_overflow_play(void) {
	setup("overflowone", "overflowtwo", true);
	/* Saturate BOTH queues with boundaries, then drop a BEGIN. Its publication
 	 * must remain recoverable without quit, even if its first save fails. */
	boundary(GE_SESSION_END, 0, 100);
	for(unsigned i = 0; i < 1800; i++) {
		boundary(GE_SESSION_BEGIN, 0, 1000 + i);
		boundary(GE_SESSION_END, 0, 1000 + i);
	}
	boundary(GE_SESSION_BEGIN, 0, 9000);
	event(GE_JUMPED, 0, 9000);
	achievements_tick();
	event(GE_INVENTORY_OPENED, 1, 200); achievements_tick();
	assert(game_event_owner_epoch(0) == 9000 && status(0).bound && status(0).generation == 9000);
	assert(status(0).progress_incomplete && !status(1).progress_incomplete);
	assert(jumps(0) == 1 && achievements_debug_profile(1)->ach[0].local == 1);
	event(GE_JUMPED, 0, 9000); achievements_tick(); assert(jumps(0) == 2);
	assert(achievements_flush_all());
	enum stats_load_status st; stats_profile_load(stats, "overflowone", &loaded, &st);
	assert(st == STATS_LOAD_OK && loaded.progress_incomplete && stats_counter_total(&loaded.general[stat_general_index(2010)]) == 2);
	puts("PASS lost BEGIN recovered during play; current owner tracks; incomplete flag durable and P2 clean");
}
static void test_snapshot_mutation(void) {
	setup("snapshotone", "snapshottwo", true);
	event(GE_JUMPED, 0, 100); boundary(GE_SESSION_CHECKPOINT, 0, 100);
	achievements_tick(); assert(achievements_save_busy(0));
	event(GE_JUMPED, 0, 100);
	for(unsigned i = 0; i < 100 && achievements_save_busy(0); i++) achievements_tick();
	assert(!achievements_save_busy(0) && status(0).dirty && jumps(0) == 2);
	enum stats_load_status st; stats_profile_load(stats, "snapshotone", &loaded, &st);
	assert(st == STATS_LOAD_OK && stats_counter_total(&loaded.general[stat_general_index(2010)]) == 1);
	boundary(GE_SESSION_CHECKPOINT, 0, 100);
	for(unsigned i = 0; i < 100 && !achievements_save_durable(0); i++) achievements_tick();
	assert(achievements_save_durable(0));
	stats_profile_load(stats, "snapshotone", &loaded, &st);
	assert(stats_counter_total(&loaded.general[stat_general_index(2010)]) == 2);
	puts("PASS cooperative snapshot immutable; post-snapshot events stay dirty and next checkpoint commits them");
}
static void test_cancel_writer(void) {
	stats_set_clock(fixed_clock);
	for(unsigned phase = 1; phase <= 6; phase++) {
		stats_profile_reset(&profile, "cancelone");
		profile.general[stat_general_index(2010)].local = 1;
		assert(stats_profile_save(stats, &profile) == STATS_SAVE_OK);
		profile.general[stat_general_index(2010)].local = 2;
		struct stats_save_job job; stats_save_begin(&job, stats, &profile);
		for(unsigned n = 0; job.phase < phase; n++) { assert(n < 100); stats_save_step(&job); }
		stats_save_cancel(&job);
		enum stats_load_status st; stats_profile_load(stats, "cancelone", &loaded, &st);
		assert(st == STATS_LOAD_OK);
		assert(stats_counter_total(&loaded.general[stat_general_index(2010)]) == (phase < 6 ? 1u : 2u));
	}
	stats_set_clock(NULL);
	puts("PASS cancel at every writer phase leaves old or fully committed new progress loadable");
}
static void test_hud(void) {
	static struct client_session p;
	memset(&p, 0, sizeof(p));
	snprintf(p.hud_message, sizeof(p.hud_message), "World save failed: region");
	p.hud_message_until = time_add_ms(time_get(), 10000);
	assert(!client_session_achievements_warning(&p, 0, false));
	assert(!strcmp(p.hud_message, "World save failed: region"));
	p.hud_message[0] = 0;
	assert(client_session_achievements_warning(&p, 0, false));
	assert(strstr(p.hud_message, "unavailable") && !strstr(p.hud_message, "save") && !strstr(p.hud_message, "Retry"));
	puts("PASS init warning honest; existing world-save HUD retained");
}
static void leave_canceled(void* result, enum stats_exit_result answer) {
 assert(answer == STATS_EXIT_BACK); (*(unsigned*)result)++;
}
static void test_quit_choices(bool hw_enabled) {
	const enum input_category categories[] = {INPUT_CAT_NUNCHUK, INPUT_CAT_CLASSIC_CONTROLLER, INPUT_CAT_GAMECUBE};
	for(unsigned who=0;who<3;who++) for(unsigned c = 0; c < 3; c++) for(unsigned answer = 0; answer < 3; answer++) {
		responding_owner=who ? 1 : 0; primary_connected=who!=2;
		char one[17], two[17]; snprintf(one, sizeof(one), "quitone%u%u%u%u", hw_enabled,who,c, answer); snprintf(two, sizeof(two), "quittwo%u%u%u%u",hw_enabled,who, c, answer);
		setup(one, two, true); event(GE_JUMPED, 0, 100); achievements_tick();
		if(c==2) {
		 memset(quit_owners,0,sizeof(quit_owners));
		 assert(input_owner_bind(quit_owners,2,0,(struct input_device){INPUT_DEVICE_GAMECUBE,0}));
		 assert(input_owner_bind(quit_owners,2,1,(struct input_device){INPUT_DEVICE_GAMECUBE,1}));
		}
		boundary(GE_SESSION_END, 0, 100); boundary(GE_SESSION_END, 1, 200);
		frame = (struct input_player_frame){.connected = true, .category = categories[c]};
		fail_profile = one; fail_left = answer == 1 ? 2 : -1;
		choice = answer == 0 ? STATS_EXIT_BACK : answer == 1 ? STATS_EXIT_SAVED : STATS_EXIT_WITHOUT_SAVE;
		unsigned canceled = 0;
		screen_stats_save_error(0, 1u, leave_canceled, &canceled);
		polls = presents = heartbeats = script_stage = focus_stage = 0; quit_driver = true;
		bool exited = main_stats_quit(hw_enabled, false); quit_driver = false;
		assert(exited == (answer != 0)); assert(presents && heartbeats && polls);
		assert(canceled == 1 && !screen_stats_save_active(0));
		if(answer == 0) { assert(achievements_save_error_acknowledged(0)); assert(status(0).dirty && jumps(0) == 1); fail_left = 0; assert(achievements_flush_all()); }
		else if(answer == 1) {
			assert(saves[0] == 3); /* first refusal, failed Retry, second Retry succeeds */
			enum stats_load_status st; stats_profile_load(stats, one, &loaded, &st);
			assert(st == STATS_LOAD_OK && stats_counter_total(&loaded.general[stat_general_index(2010)]) == 1);
		} else {
			char survivor[CW_STATS_PATH_MAX]; assert(stats_profile_path(survivor, sizeof(survivor), stats, one, ".unsaved"));
			FILE* f = fopen(survivor, "rb"); assert(f); fclose(f);
		}
		printf("PASS production quit loop hw-enabled=%u auto-exit=0 owner=%u P1-connected=%u category=%d answer=%d pumps=%u renders=%u\n", hw_enabled, responding_owner,primary_connected,categories[c], choice, polls, presents);
	}
	fail_left = 0; responding_owner=0; primary_connected=true;
}
static void test_focus_transfer(void) {
 setup("focusone", "focustwo", true);
 event(GE_JUMPED,0,100); achievements_tick();
 boundary(GE_SESSION_END,0,100); boundary(GE_SESSION_END,1,200);
 fail_profile="focusone"; fail_left=-1;
 frame=(struct input_player_frame){.connected=true,.category=INPUT_CAT_NUNCHUK};
 memset(quit_owners,0,sizeof(quit_owners));
 polls=presents=script_stage=0; responding_owner=0;
 quit_driver=focus_driver=true;
 /* P1 selects Quit; P2's A takes focus only, even while held. P1's first
  * HOME takes focus back only; release and a second HOME choose Back. */
 assert(!RUN_QUIT(false));
 assert(script_stage==11);
 quit_driver=focus_driver=false; responding_owner=0;
 assert(status(0).dirty && jumps(0)==1);
 fail_left=0; assert(achievements_flush_all());
 puts("PASS focus transfers consume P2 A and P1 HOME; held A cannot quit; second HOME returns Back");
}
static void test_clean_shutdown_failure(void) {
 setup("cleanone","cleantwo",false);
 boundary(GE_SESSION_END,0,100); boundary(GE_SESSION_END,1,200); achievements_tick();
 assert(achievements_save_durable(0) && achievements_save_durable(1));
 frame=(struct input_player_frame){.connected=true,.category=INPUT_CAT_NUNCHUK};
 choice=STATS_EXIT_BACK; script_stage=polls=presents=0;
 quit_driver=clean_quiesce_driver=true;
 assert(!RUN_QUIT(false)); quit_driver=clean_quiesce_driver=false;
 assert(polls>=2 && presents);
 puts("PASS clean profiles with refused shutdown still draw failure and pump Back input");
}
static void test_dialog_layout(void) {
 struct stats_exit_dialog d = {.active = true, .failed = true, .mask = 3u};
 const int widths[] = {640, 802}, heights[] = {240, 480};
 for(unsigned w = 0; w < 2; w++) for(unsigned h = 0; h < 2; h++) {
  draw_width = widths[w]; draw_height = heights[h];
  unsigned before = hint_rows;
  stats_exit_render(&d, &frame, draw_width, draw_height);
  assert(hint_rows == before + 1);
  assert(abs(2 * last_hints.strip.x + last_hints.strip.width - draw_width) <= 1);
  for(int item = 0; item < 2; item++) {
   const struct gutil_hint_cell* cell = NULL;
   for(int k = 0; k < last_hints.count; k++)
    if(last_hints.cells[k].item == item) cell = &last_hints.cells[k];
   assert(cell);
   struct stats_exit_dialog clicked = {.active = true, .failed = true,
    .armed = true, .category_seen = true, .category = frame.category};
   struct input_player_frame pointer = frame;
   memset(pointer.pressed, 0, sizeof pointer.pressed);
   memset(pointer.down, 0, sizeof pointer.down);
   pointer.connected = pointer.pointer_valid = true;
   pointer.pointer_x = cell->rect.x + 1;
   pointer.pointer_y = 240 + cell->rect.y + 1;
   pointer.pressed[IB_GUI_CLICK] = pointer.down[IB_GUI_CLICK] = true;
   enum stats_exit_result result = stats_exit_update(&clicked, &pointer,
    0, 240, draw_width, draw_height);
   if(item == 0) assert(result == STATS_EXIT_BACK && !clicked.active);
   else assert(result == STATS_EXIT_NONE && clicked.active && !clicked.failed);
  }
 }
 draw_width = 640; draw_height = 480;
 puts("PASS failure text, three buttons and bottom hints fit full/split at 640/802; footer Back/Retry pointer hits use pane offsets");
}
static void test_background_rescue(void) {
 setup("rescueone", "rescuetwo", true);
 event(GE_JUMPED, 1, 200); boundary(GE_SESSION_END, 1, 200);
 fail_profile = "rescuetwo"; fail_left = -1; achievements_tick();
 assert(achievements_slot_save_failed(1));
 fail_left = 0;
 unsigned before = steps;
 achievements_request_rescue(1);
 assert(steps == before); /* Start/snapshot performs no filesystem stage. */
 assert(achievements_slot_save_failed(1) && achievements_save_error_acknowledged(1)); /* Survivor doesn't hide failure. */
 for(unsigned n = 0; n < 70; n++) { event(GE_JUMPED, 0, 100); achievements_tick(); }
 assert(jumps(0) == 70 && status(1).dirty && achievements_slot_save_failed(1));
 char survivor[256]; assert(stats_profile_path(survivor, sizeof(survivor), stats, "rescuetwo", ".unsaved"));
 FILE* f = fopen(survivor, "rb"); assert(f); fclose(f);
 achievements_request_save(1);
 assert(!achievements_save_error_acknowledged(1));
 for(unsigned n = 0; n < 100 && !achievements_save_durable(1); n++) achievements_tick();
 assert(achievements_save_durable(1) && !achievements_slot_save_failed(1));
 assert(achievements_flush_all());
 puts("PASS Leave survivor spread over main ticks; P1 plays; canonical error retained until successful Retry");
}
static void test_departed_main(bool options_only) {
 setup("mainone", "maintwo", false);
 event(GE_JUMPED, 1, 200); fail_profile="maintwo"; fail_left=-1;
 boundary(GE_SESSION_END, 1, 200); achievements_tick();
 assert(achievements_slot_save_failed(1) && !status(1).bound);
 struct entity body={0}; body.data.local_player.capture_input=true;
 gstate.players[0]=(struct client_session){.current_screen=&screen_ingame,.local_player=&body};
 gstate.players[1]=(struct client_session){0};
 gstate.world_loaded=true; gstate.paused=false;
 frame=(struct input_player_frame){.connected=true,.category=INPUT_CAT_NUNCHUK};
 if(!options_only) {
  for(unsigned n=0;n<8;n++) { main_stats_poll(); main_stats_update(); }
  assert(!screen_stats_save_active(0));
  assert(body.data.local_player.capture_input);
  assert(strstr(gstate.players[0].hud_message,"achievements"));
  event(GE_JUMPED,0,100); achievements_tick(); assert(jumps(0)==1);
  gstate.world_loaded=false; gstate.players[0].current_screen=&screen_load_world;
  main_stats_poll(); main_stats_update(); assert(!screen_stats_save_active(0));
  gstate.world_loaded=true;
 }
 gstate.paused=true; gstate.players[0].current_screen=&screen_options;
 main_stats_poll(); assert(screen_stats_save_active(0));
 main_stats_update(); assert(!body.data.local_player.capture_input);
 text_draws=0; main_stats_draw(false); assert(text_draws>=3);
 frame.pressed[IB_INVENTORY]=true; main_stats_update();
 assert(!screen_stats_save_active(0) && achievements_save_error_acknowledged(1));
 frame.pressed[IB_INVENTORY]=false;
 main_stats_poll(); assert(!screen_stats_save_active(0));
 fail_left=0; assert(achievements_flush_all());
 main_stats_poll();
 /* The other supported presentation boundaries use the real viewport draw
  * call sliced from main, including the split owner's pause page. */
 for(unsigned n=0;n<3;n++) {
  boundary(GE_SESSION_BEGIN,1,201+n); achievements_tick(); event(GE_JUMPED,1,201+n);
  fail_left=-1; boundary(GE_SESSION_END,1,201+n); achievements_tick();
  gstate.paused=false; gstate.world_loaded=n!=2;
  gstate.players[0].pause.open=n==1;
  gstate.players[0].current_screen=n==0 ? &screen_achievements : n==1 ? &screen_ingame : &screen_title;
  main_stats_poll(); assert(screen_stats_save_active(0));
  main_stats_update(); text_draws=0; main_stats_viewport_draw(0,480); assert(text_draws>=3);
  frame.pressed[IB_INVENTORY]=true; main_stats_update(); frame.pressed[IB_INVENTORY]=false;
  assert(!screen_stats_save_active(0)); main_stats_poll();
  fail_left=0; assert(achievements_flush_all()); main_stats_poll();
 }
 gstate.players[0].pause.open=false;
 puts("PASS production main: departed P2 HUD only during play; P1 input survives; Options prompt drawn and Back acknowledged");
 puts("PASS production main: achievements board, owner pause and title boundaries draw the pending prompt");
}
static void test_hw_auto_gate(void) {
 assert(main_auto_exit_gate(false));
 assert(main_auto_exit_gate(true));
 puts("PASS production main hw_run auto-exit remains armed after canceled quit");
}
static void test_hw_failure(bool stall) {
 const char* one=stall ? "hwstallone" : "hwone";
 setup(one,stall ? "hwstalltwo" : "hwtwo",true);
 event(GE_JUMPED,0,100); event(GE_JUMPED,1,200); achievements_tick();
 boundary(GE_SESSION_END,0,100); boundary(GE_SESSION_END,1,200);
 fail_profile=one; fail_left=stall ? 0 : -1; stall_writer=stall;
 frame=(struct input_player_frame){0};
 polls=presents=heartbeats=telemetry_failures=0;
 quit_driver=unattended_driver=true;
 assert(RUN_QUIT(true)); quit_driver=unattended_driver=stall_writer=false;
 assert(telemetry_failures && polls<=STATS_QUIT_MAX_POLLS);
 if(stall) assert(polls==STATS_QUIT_MAX_POLLS);
 char survivor[256]; assert(stats_profile_path(survivor,sizeof(survivor),stats,one,".unsaved"));
 FILE* f=fopen(survivor,"rb"); assert(f); fclose(f);
 /* Reload the exact survivor with the production reader. */
 char restored[256];
 /* profile identity is checked by the reader, so copy to a separate stats root. */
 char recovery[256]; snprintf(recovery,sizeof(recovery),"%s/hwrecovery%u",root,stall);
#ifdef _WIN32
 assert(mkdir(recovery)==0);
#else
 assert(mkdir(recovery,0755)==0);
#endif
 assert(stats_profile_path(restored,sizeof(restored),recovery,one,""));
 FILE* in=fopen(survivor,"rb"); f=fopen(restored,"wb"); assert(in&&f);
 for(int ch; (ch=fgetc(in))!=EOF;) assert(fputc(ch,f)!=EOF);
 assert(!fclose(in)&&!fclose(f));
 enum stats_load_status st; stats_profile_load(recovery,one,&loaded,&st);
 assert(st==STATS_LOAD_OK && stats_counter_total(&loaded.general[stat_general_index(2010)])==1);
 printf("PASS production unattended quit: failure telemetry; bounded polls=%u; .unsaved reload retains jump\n",polls);
 fail_left=0;
}
static void test_refused_file_quit(bool foreign) {
	const char* one = foreign ? "foreignone" : "newerone";
	setup(one, foreign ? "foreigntwo" : "newertwo", true);
	boundary(GE_SESSION_END, 0, 100); boundary(GE_SESSION_END, 1, 200); achievements_tick();
	char canonical[256], survivor[256], recovery[256], restored[256];
	assert(stats_profile_path(canonical, sizeof(canonical), stats, one, ""));
	char original[8192];
	size_t length = snprintf(original, sizeof(original), "cwstats %u\nprofile %s\n", foreign ? 1u : 2u, one);
	if(foreign) {
		length += snprintf(original + length, sizeof(original) - length, "saved 1\n");
		for(unsigned i = 0; i <= CW_STATS_FOREIGN_MAX; i++)
			length += snprintf(original + length, sizeof(original) - length, "s %u 1 0\n", 0x1010000u + 32768u + i);
		uLong crc = crc32(0, (const Bytef*)original, length);
		length += snprintf(original + length, sizeof(original) - length, "end %u %08lx\n", CW_STATS_FOREIGN_MAX + 1, (unsigned long)crc);
	} else length += snprintf(original + length, sizeof(original) - length, "future progress must survive\n");
	assert(length < sizeof(original));
	FILE* f = fopen(canonical, "wb"); assert(f);
	assert(fwrite(original, 1, length, f) == length); assert(!fclose(f));
	boundary(GE_SESSION_BEGIN, 0, 101); achievements_tick();
	assert(achievements_debug_profile(0)->write_refused == (foreign ? STATS_WRITE_REFUSED_FOREIGN : STATS_WRITE_REFUSED_NEWER));
	event(GE_JUMPED, 0, 101); boundary(GE_SESSION_END, 0, 101);
	frame = (struct input_player_frame){.connected = true, .category = INPUT_CAT_NUNCHUK};
	choice = STATS_EXIT_WITHOUT_SAVE; polls = presents = script_stage = 0; quit_driver = true;
	assert(RUN_QUIT(false)); quit_driver = false;
	char bytes[sizeof(original)]; f = fopen(canonical, "rb"); assert(f);
	assert(fread(bytes, 1, sizeof(bytes), f) == length); assert(!fclose(f));
	assert(!memcmp(bytes, original, length));
	assert(stats_profile_path(survivor, sizeof(survivor), stats, one, ".unsaved"));
	snprintf(recovery, sizeof(recovery), "%s/recovered-%u", root, foreign);
#ifdef _WIN32
	assert(mkdir(recovery) == 0);
#else
	assert(mkdir(recovery, 0755) == 0);
#endif
	assert(stats_profile_path(restored, sizeof(restored), recovery, one, ""));
	FILE* in = fopen(survivor, "rb"); f = fopen(restored, "wb"); assert(in && f);
	for(int c; (c = fgetc(in)) != EOF;) assert(fputc(c, f) != EOF);
	assert(!fclose(in) && !fclose(f));
	enum stats_load_status st; stats_profile_load(recovery, one, &loaded, &st);
	assert(st == STATS_LOAD_OK && stats_counter_total(&loaded.general[stat_general_index(2010)]) == 1);
	printf("PASS refused-%s quit exits; original bytes untouched; independent survivor reloads known progress\n", foreign ? "foreign" : "newer");
}

static uint32_t fixed_clock(void) { return 1234567890; }
#ifdef _WIN32
static uint64_t cycles(void) { ULONG64 n; assert(QueryThreadCycleTime(GetCurrentThread(), &n)); return n; }
static int compare_cycles(const void* a, const void* b) {
	uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b; return (x > y) - (x < y);
}
#endif
static void fill_profile(bool dense) {
	stats_profile_reset(&profile, dense ? "costdense" : "costsparse");
	profile.general[stat_general_index(2010)].local = 1;
	profile.general[stat_general_index(1100)].local = 20000;
	profile.mine[1].local = 80;
	if(!dense) return;
	for(unsigned i = 0; i < CW_STAT_COUNT; i++) profile.general[i] = (struct stats_pair){UINT32_MAX, UINT32_MAX};
	for(unsigned i = 0; i < CW_ACH_COUNT; i++) { profile.ach[i] = (struct stats_pair){1, 1}; profile.ach_first[i] = STATS_FIRST_LOCAL; }
	for(unsigned i = 0; i < CW_STAT_BLOCK_COUNT; i++) profile.mine[i] = (struct stats_pair){UINT32_MAX, UINT32_MAX};
	for(unsigned i = 0; i < ITEMS_MAX; i++) profile.craft[i] = profile.use[i] = profile.brk[i] = (struct stats_pair){UINT32_MAX, UINT32_MAX};
}
static void measure(void) {
	stats_set_clock(fixed_clock);
	for(unsigned dense = 0; dense < 2; dense++) {
		fill_profile(dense);
#ifdef _WIN32
		uint64_t full[17], totals[17], peak[17]; unsigned max_scanned = 0, calls = 0;
		for(unsigned n = 0; n < 17; n++) {
			uint64_t begin = cycles(); assert(__real_stats_profile_save(stats, &profile) == STATS_SAVE_OK); full[n] = cycles() - begin;
			struct stats_save_job job; begin = cycles(); loaded = profile; stats_save_begin(&job, stats, &loaded); totals[n] = cycles() - begin; peak[n] = 0; calls = 0;
			while(!job.done) {
				begin = cycles(); __real_stats_save_step(&job); uint64_t elapsed = cycles() - begin;
				totals[n] += elapsed; if(elapsed > peak[n]) peak[n] = elapsed;
				if(job.scanned > max_scanned) max_scanned = job.scanned;
				calls++;
			}
			assert(job.result == STATS_SAVE_OK && max_scanned <= CW_STATS_SAVE_STEP_RECORDS);
		}
		qsort(full, 17, sizeof(*full), compare_cycles); qsort(totals, 17, sizeof(*totals), compare_cycles); qsort(peak, 17, sizeof(*peak), compare_cycles);
		printf("MEASURE %s profile=%zu B full-save-thread-cycles median=%llu max=%llu cooperative-total-median=%llu largest-step-median=%llu largest-step-max=%llu steps=%u scan-bound=%u records\n",
			dense ? "dense" : "sparse", sizeof(profile), (unsigned long long)full[8], (unsigned long long)full[16],
			(unsigned long long)totals[8], (unsigned long long)peak[8], (unsigned long long)peak[16], calls, max_scanned);
#endif
		enum stats_load_status st; stats_profile_load(stats, profile.profile_id, &loaded, &st);
		assert(st == STATS_LOAD_OK && !memcmp(&profile, &loaded, sizeof(profile)));
	}
	stats_set_clock(NULL);
}
int main(int argc, char** argv) {
	io_thread = thread_id();
	test_fixture_dir("test_achievements_r2", root, sizeof(root));
	
#ifdef _WIN32
	assert(mkdir(root) == 0);
#else
    assert(mkdir(root, 0755) == 0);
#endif
	snprintf(stats, sizeof(stats), "%s/stats", root);
	assert(achievements_init(root));
	if(argc > 1 && !strcmp(argv[1], "measure")) measure();
	else if(argc > 1 && !strcmp(argv[1], "n1")) test_departed_main(false);
	else if(argc > 1 && !strcmp(argv[1], "n1_options")) test_departed_main(true);
	else if(argc > 1 && !strcmp(argv[1], "hw_human")) test_quit_choices(true);
	else if(argc > 1 && !strcmp(argv[1], "focus")) test_focus_transfer();
	else if(argc > 1 && !strcmp(argv[1], "hw_gate")) test_hw_auto_gate();
	else if(argc > 1 && !strcmp(argv[1], "hw_failure")) test_hw_failure(false);
	else if(argc > 1 && !strcmp(argv[1], "hw_budget")) test_hw_failure(true);
	else {
		test_retry_and_isolation(); test_empty_coalescer(); test_overflow_play();
		test_snapshot_mutation(); test_cancel_writer(); test_hud(); test_dialog_layout(); test_quit_choices(false); test_quit_choices(true); test_focus_transfer(); test_background_rescue(); test_refused_file_quit(false); test_refused_file_quit(true);
		test_departed_main(false); test_hw_auto_gate(); test_hw_failure(false); test_hw_failure(true); test_clean_shutdown_failure();
	}
	fail_left = 0; achievements_set_cooperative_saves(false); assert(achievements_shutdown());
	test_fixture_dir_cleanup(root); puts("PASS test_achievements_r2"); return 0;
}
