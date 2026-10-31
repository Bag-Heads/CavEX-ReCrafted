#include <stdio.h>
#include <string.h>
#include "screen_stats_save.h"
#include "screen.h"
#include "../achievements/achievements.h"
#include "../../platform/input_owner.h"
#include "../../platform/input_glyph.h"
#include "../../platform/gfx.h"
#include "../../platform/texture.h"
#include "../../platform/menu_sound.h"
#include "../../platform/time.h"
#include "../../platform/watchdog.h"
#include "../../platform/hw_telemetry.h"

static struct {
	struct stats_exit_dialog dialog;
	void (*done)(void*, enum stats_exit_result);
	void* user;
} dialogs[2];

static void request(struct stats_exit_dialog* d) {
	d->failed = false;
	d->needed = 0;
	for(unsigned i = 0; i < 2; i++) {
		if(!(d->mask & (1u << i)) || achievements_save_durable(i)) continue;
		d->needed |= 1u << i;
		d->target[i] = achievements_save_serial(i) + (achievements_save_busy(i) ? 2 : 1);
		achievements_request_save(i);
	}
}
void stats_exit_open(struct stats_exit_dialog* d, unsigned mask) {
	*d = (struct stats_exit_dialog){.active = true, .mask = mask & 3u};
	request(d);
}
static struct gutil_button_bounds button(int w, int h, unsigned i) {
	/* Compact failure choices end at 196, where the bottom hint strip starts. */
	return (struct gutil_button_bounds){(w - 288) / 2,
		h / 2 - (h < 480 ? 52 : 22) + (int)i * (h < 480 ? 44 : 48), 288, 40};
}
static bool hint_row(const struct stats_exit_dialog* d,
	const struct input_player_frame* f, int w, int h, struct gutil_hint_row* row) {
	const struct gutil_hint_item items[] = {
		{{IB_INVENTORY}, 1, "Back", 1, true},
		{{IB_GUI_CLICK}, 1, "Retry", 0, false}};
	return gutil_hint_row_layout(f, 0, 0, w, h, h < 480,
		GUTIL_HINT_ALIGN_CENTRE, items, d->failed ? 2 : 1, row);
}
static enum stats_exit_result finish(struct stats_exit_dialog* d, enum stats_exit_result result) {
	if(d->failed && (result == STATS_EXIT_BACK || result == STATS_EXIT_WITHOUT_SAVE))
		for(unsigned i = 0; i < 2; i++) if(d->mask & (1u << i)) achievements_acknowledge_save_error(i);
	d->active = false;
	return result;
}
enum stats_exit_result stats_exit_update(struct stats_exit_dialog* d,
	const struct input_player_frame* f, int x, int y, int w, int h) {
	if(!d || !d->active) return STATS_EXIT_NONE;
	if(!d->failed) {
		bool complete = true;
		for(unsigned i = 0; i < 2; i++) {
			if(!(d->needed & (1u << i))) continue;
			if(achievements_slot_save_failed(i)) d->failed = true;
			if(achievements_save_serial(i) < d->target[i] && !achievements_save_durable(i)) complete = false;
		}
		if(complete) return finish(d, STATS_EXIT_SAVED);
	}
	if(!f || !f->connected) { d->armed = false; return STATS_EXIT_NONE; }
	if(!d->category_seen || d->category != f->category) {
		d->armed = false;
		d->category = f->category;
		d->category_seen = true;
	}
	/* Entry/handover cannot carry a held A into a destructive choice. */
	if(!d->armed) {
		bool neutral = true;
		for(int b = 0; b < IB_COUNT; b++) neutral &= !f->down[b] && !f->pressed[b];
		d->armed = neutral;
		return STATS_EXIT_NONE;
	}
	if(f->pressed[IB_INVENTORY] || f->pressed[IB_HOME]) {
		audio_menu_event(MENU_SOUND_BACK, time_get());
		return finish(d, STATS_EXIT_BACK);
	}
	unsigned previous = d->selected;
	bool navigating = f->pressed[IB_GUI_UP] || f->pressed[IB_GUI_DOWN];
	d->pointer_available = input_owner_pointer(f, x, y, w, h, &d->px, &d->py, &d->angle);
	bool pointer_owner = gutil_menu_pointer_focus(&d->focus, d->pointer_available, d->px, d->py, navigating);
	int hovered = GUTIL_MENU_TARGET_NONE;
	for(unsigned i = 0; i < (d->failed ? 3u : 1u); i++) {
		struct gutil_button_bounds r = button(w, h, i);
		if(d->pointer_available && gutil_button_hit(&r, d->px, d->py)) hovered = i;
	}
	struct gutil_hint_row row;
	if(d->pointer_available && hint_row(d, f, w, h, &row)) {
		int item = gutil_hint_row_hit(&row, d->px, d->py);
		if(item >= 0) hovered = item;
	}
	if(pointer_owner && hovered >= 0) { d->selected = hovered; d->origin = GUTIL_MENU_ORIGIN_POINTER; }
	if(navigating && d->failed) {
		d->selected = (d->selected + (f->pressed[IB_GUI_DOWN] ? 1 : 2)) % 3;
		d->origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	}
	if(d->selected != previous) audio_menu_event(MENU_SOUND_FOCUS, time_get());
	struct gutil_menu_decision decision = gutil_menu_activate_decide((struct gutil_menu_activate_state){
		.pressed = f->pressed[IB_GUI_CLICK], .navigated = navigating,
		.pointer_valid = d->pointer_available, .selection_origin = d->selected == 0
			? GUTIL_MENU_ORIGIN_DIRECTIONAL : d->origin,
		.hovered = hovered, .hovered_enabled = hovered >= 0,
		.selected = d->selected, .selected_enabled = true,
		.selected_destructive = d->selected == 2, .destructive_dialog = true,
	});
	if(decision.action == GUTIL_MENU_FOCUS_ONLY) {
		d->selected = decision.selected; d->origin = decision.selection_origin;
	}
	if(decision.action != GUTIL_MENU_ACTIVATE) return STATS_EXIT_NONE;
	audio_menu_event(decision.target == 0 ? MENU_SOUND_BACK : MENU_SOUND_SELECT, time_get());
	if(decision.target == 0) return finish(d, STATS_EXIT_BACK);
	if(decision.target == 1) { request(d); d->selected = 0; d->armed = false; return STATS_EXIT_NONE; }
	return finish(d, STATS_EXIT_WITHOUT_SAVE);
}
void stats_exit_render(const struct stats_exit_dialog* d, const struct input_player_frame* f, int w, int h) {
	if(!d || !d->active) return;
	gfx_texture(false);
	gutil_texquad_col(0, 0, 0, 0, 0, 0, w, h, 16, 16, 16, 245);
	gfx_texture(true);
	const char* title = d->failed ? "Achievements could not save." : "Saving achievements...";
	gutil_text((w - gutil_font_width(title, 20)) / 2, h / 2 - (h < 480 ? 112 : 104), title, 20, true);
	const char* reason = "Progress remains in memory.";
	if(d->failed && (d->mask == 1u || (d->mask == 3u && achievements_slot_save_failed(0) && !achievements_slot_save_failed(1))))
		reason = "Player 1 progress remains in memory.";
	else if(d->failed && (d->mask == 2u || (d->mask == 3u && achievements_slot_save_failed(1) && !achievements_slot_save_failed(0))))
		reason = "Player 2 progress remains in memory.";
	for(unsigned i = 0; i < 2; i++) {
		struct ach_status st;
		if(!(d->mask & (1u << i)) || !achievements_debug_status(i, &st)) continue;
		if(st.last_save_error == STATS_SAVE_REFUSED_NEWER) reason = "File needs a newer build; it is untouched.";
		else if(st.last_save_error == STATS_SAVE_REFUSED_FOREIGN) reason = "File has unsupported progress; it is untouched.";
	}
	gutil_text((w - gutil_font_width(reason, 16)) / 2, h / 2 - (h < 480 ? 90 : 78), reason, 16, true);
	if(d->failed) {
		const char* loss = "Quit may lose unsaved progress.";
		gutil_text((w - gutil_font_width(loss, 16)) / 2, h / 2 - (h < 480 ? 72 : 52), loss, 16, true);
	}
	const char* labels[] = {"Back", "Retry", "Quit without saving"};
	for(unsigned i = 0; i < (d->failed ? 3u : 1u); i++) {
		struct gutil_button_bounds r = button(w, h, i);
		gutil_button_draw(r.x, r.y, r.width / 2, labels[i], true, d->selected == i);
	}
	if(f) {
		/* The same cells drive pointer hits and drawing, including split. */
		struct gutil_hint_row row;
		if(hint_row(d, f, w, h, &row))
			gutil_hint_row_draw(&row);
	}
	if(d->pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(d->px, d->py, glm_rad(d->angle), 0, 0, 256, 256, 48, 48);
	}
}
void screen_stats_save_open(unsigned owner, unsigned mask, void (*done)(void*, enum stats_exit_result), void* user) {
	if(owner >= 2 || dialogs[owner].dialog.active) return;
	bool durable = true;
	for(unsigned i = 0; i < 2; i++)
		if(mask & (1u << i)) durable &= achievements_save_durable(i);
	if(durable) { if(done) done(user, STATS_EXIT_SAVED); return; }
	dialogs[owner].done = done; dialogs[owner].user = user;
	stats_exit_open(&dialogs[owner].dialog, mask);
	input_pointer_enable(true);
}
bool screen_stats_save_active(unsigned owner) { return owner < 2 && dialogs[owner].dialog.active; }
void screen_stats_save_error(unsigned owner, unsigned mask, void (*done)(void*, enum stats_exit_result), void* user) {
	if(owner >= 2 || dialogs[owner].dialog.active) return;
	dialogs[owner].done = done; dialogs[owner].user = user;
	dialogs[owner].dialog = (struct stats_exit_dialog){.active = true, .failed = true, .mask = mask & 3u};
	input_pointer_enable(true);
}
bool screen_stats_save_update(unsigned owner, const struct input_player_frame* f, int x, int y, int w, int h) {
	if(!screen_stats_save_active(owner)) return false;
	enum stats_exit_result result = stats_exit_update(&dialogs[owner].dialog, f, x, y, w, h);
	if(result != STATS_EXIT_NONE && dialogs[owner].done) {
		/* Every held/pressed parent action is fenced after the modal answer. */
		if(f) for(int b = 0; b < IB_COUNT; b++) if(f->down[b] || f->pressed[b]) input_player_block(owner, b);
		dialogs[owner].done(dialogs[owner].user, result);
	}
	return true;
}
void screen_stats_save_render(unsigned owner, const struct input_player_frame* f, int w, int h) {
	if(owner < 2) stats_exit_render(&dialogs[owner].dialog, f, w, h);
}
static void quit_failure_log(const char* cause, unsigned retry) {
 char line[192];
 snprintf(line, sizeof(line), "achievements_quit failure=%s retry=%u p1=%u p2=%u\n",
  cause, retry, achievements_slot_save_failed(0), achievements_slot_save_failed(1));
 fprintf(stderr, "[achievements] %s", line);
 (void)hw_telemetry_emit_observer(line);
}
static bool quit_without_save(void) {
 for(unsigned i = 0; i < 2; i++) {
  bool saved = achievements_rescue_unsaved(i);
  if(!saved) fprintf(stderr, "[achievements] Player %u .cwstats.unsaved attempt failed\n", i + 1);
 }
 achievements_shutdown_without_save();
 return true;
}
bool screen_stats_save_quit(bool unattended) {
	/* Process Quit supersedes a pending Leave. Retire its callback as Back,
	 * so canceling this quit cannot later dispatch a stale world/guest exit. */
	for(unsigned i = 0; i < 2; i++) {
		if(!dialogs[i].dialog.active) continue;
		dialogs[i].dialog.active = false;
		if(dialogs[i].done) dialogs[i].done(dialogs[i].user, STATS_EXIT_BACK);
	}
	/* Providers have joined, but their final facts may still be queued.
	 * Resolve those before deciding whether the profiles are durable. */
	achievements_tick();
	input_pointer_enable(true);
	struct stats_exit_dialog d;
	stats_exit_open(&d, 3u);
	struct { bool armed, connected; enum input_category category; } owners[2] = {{0}};
	unsigned selected_owner = 0, retries = 0, polls = 0;
	for(;;) {
		watchdog_heartbeat();
		achievements_tick();
		input_poll();
		polls++;
		const struct input_player_frame* frames[2] = {input_player_frame(0), input_player_frame(1)};
		for(unsigned p = 0; p < 2; p++) {
		 const struct input_player_frame* f = frames[p];
		 if(!f || !f->connected) { owners[p].armed = owners[p].connected = false; continue; }
		 if(!owners[p].connected || owners[p].category != f->category) owners[p].armed = false;
		 owners[p].connected = true; owners[p].category = f->category;
		 bool neutral = true;
		 for(int b = 0; b < IB_COUNT; b++) neutral &= !f->down[b] && !f->pressed[b];
		 if(neutral) owners[p].armed = true;
		}
		if(!owners[selected_owner].connected) selected_owner = owners[0].connected ? 0 : 1;
		/* Either player's deliberate action can take focus, including Remote
		 * A/1/HOME and D-pad. Each controller must first have been neutral. */
		for(unsigned p = 0; p < 2; p++) {
		 if(!owners[p].connected || !owners[p].armed) continue;
		 bool action = false;
		 for(int b = 0; b < IB_COUNT; b++) action |= frames[p]->pressed[b];
		 if(action) {
		  /* A focus-taking press is consumed; release before choosing. */
		  if(selected_owner != p) owners[p].armed = false;
		  selected_owner = p;
		  break;
		 }
		}
		const struct input_player_frame* f = frames[selected_owner];
		d.armed = owners[selected_owner].armed;
		d.category_seen = f && f->connected;
		if(f) d.category = f->category;
		enum stats_exit_result result = stats_exit_update(&d, unattended ? NULL : f,
			0, 0, gfx_width(), gfx_height());
		owners[selected_owner].armed = d.armed;
		if(result == STATS_EXIT_BACK) return false;
		if(result == STATS_EXIT_WITHOUT_SAVE) return quit_without_save();
		if(result == STATS_EXIT_SAVED) {
			if(achievements_shutdown()) return true;
			/* A clean profile does not prove that the lifecycle route quiesced.
			 * Never reopen a zero-needed "Saved" dialog in a tight loop. */
			d = (struct stats_exit_dialog){.active = true, .failed = true, .mask = 3u};
		}
		if(unattended && (d.failed || polls >= STATS_QUIT_MAX_POLLS)) {
			quit_failure_log(d.failed ? "save_or_shutdown" : "poll_budget", retries);
			if(retries >= 2 || polls >= STATS_QUIT_MAX_POLLS) return quit_without_save();
			retries++;
			stats_exit_open(&d, 3u);
		}
		gfx_clear_buffers(24, 24, 24);
		gfx_mode_gui();
		stats_exit_render(&d, f, gfx_width(), gfx_height());
		gfx_finish(true); /* includes the PC window event pump */
	}
}
