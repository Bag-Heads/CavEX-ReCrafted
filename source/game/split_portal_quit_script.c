/* Checklist 6b: split_recovery P0-P2 supply a disposable world, three real
 * placed blocks and two populated inventories. This file owns the race. */
#include "split_portal_quit_script.h"
#include "split_portal_quit_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../block/blocks_data.h"
#include "../item/inventory.h"
#include "../item/window_container.h"
#include "client_session.h"
#include "hw_run.h"
#include "split_recovery_script.h"

struct spq_block { int x, y, z; uint32_t id, meta; };
static struct {
	bool armed, started, done, portal_armed, autoload_armed, switch_seen;
	bool post_switch_position_set;
	unsigned step, block_count;
	uint32_t seq, start_ms, step_ms, first_request_count;
	uint32_t confirm_tick, confirm_ms, switch_tick, switch_ms;
	bool confirm_seen, switch_marker_seen, deadline_elapsed;
	uint32_t host_hash, guest_hash;
	float host_x, host_y, host_z;
	float post_switch_x, post_switch_y, post_switch_z;
	struct spq_block blocks[SPLIT_RECOVERY_BLOCKS];
	struct spq_core race;
	struct split_portal_quit_hooks hooks;
} s;

static bool field_int(const char* line, const char* key, int* out) {
	char needle[40];
	snprintf(needle, sizeof needle, "\"%s\":", key);
	const char* p = strstr(line, needle);
	if(!p) return false;
	p += strlen(needle);
	char* end = NULL;
	long value = strtol(p, &end, 10);
	if(end == p || value < -32768 || value > 32767) return false;
	*out = (int)value;
	return true;
}
void split_portal_quit_capture_recovery_marker(const char* line) {
	if(!s.armed || !line || !strstr(line, "\"marker\":\"block_placed\""))
		return;
	int i, x, y, z, id, meta;
	if(!field_int(line, "i", &i) || !field_int(line, "x", &x)
	   || !field_int(line, "y", &y) || !field_int(line, "z", &z)
	   || !field_int(line, "id", &id) || !field_int(line, "meta", &meta)
	   || i < 0 || i >= SPLIT_RECOVERY_BLOCKS || id <= 0) return;
	s.blocks[i] = (struct spq_block){x, y, z, (uint32_t)id, (uint32_t)meta};
	if(s.block_count <= (unsigned)i) s.block_count = (unsigned)i + 1;
}
static uint32_t inventory_hash(struct client_session* session,
	unsigned* used_out) {
	struct inventory* inv = session && session->windows[WINDOWC_INVENTORY]
		? windowc_get_latest(session->windows[WINDOWC_INVENTORY]) : NULL;
	uint32_t hash = 2166136261u;
	unsigned used = 0;
	for(size_t slot = 0; slot < INVENTORY_SIZE; slot++) {
		struct item_data item = {0};
		if(inv) (void)inventory_get_slot(inv, slot, &item);
		if(!item.id || !item.count) item = (struct item_data){0};
		else used++;
		uint8_t bytes[5] = {(uint8_t)item.id, (uint8_t)(item.id >> 8),
			item.count, (uint8_t)item.durability,
			(uint8_t)(item.durability >> 8)};
		for(size_t k = 0; k < sizeof bytes; k++) {
			hash ^= bytes[k]; hash *= 16777619u;
		}
	}
	if(used_out) *used_out = used;
	return hash;
}
static void neutral(struct input_player_frame* frame) {
	memset(frame, 0, sizeof *frame);
	frame->connected = true;
	frame->category = INPUT_CAT_NUNCHUK;
	for(int b = 0; b < IB_COUNT; b++) frame->glyphs[b] = -1;
}
static bool mark(const struct split_portal_quit_ctx* c, const char* name,
	bool ok, const char* extra) {
	char line[512];
	if(!spq_format_marker(line, sizeof line, s.seq + 1, name,
		c->up_ms, c->tick, s.race.target, ok, extra) || !s.hooks.emit
	   || !s.hooks.emit(line)) return false;
	s.seq++;
	return true;
}
static void fail(const struct split_portal_quit_ctx* c, const char* reason) {
	if(s.done) return;
	char extra[160];
	/* All reasons are fixed code strings, never untrusted HUD text. */
	snprintf(extra, sizeof extra, "\"reason\":\"%s\"", reason);
	(void)mark(c, "fail", false, extra);
	(void)mark(c, "run_summary", true, "\"winning_leg\":-1");
	s.done = true;
	if(s.hooks.done) s.hooks.done(reason);
}
static bool advance(const struct split_portal_quit_ctx* c, const char* name,
	const char* extra) {
	if(!mark(c, name, true, extra)) {
		fail(c, "marker_emit_blocked"); return false;
	}
	s.step++; s.step_ms = c->up_ms;
	return true;
}
void split_portal_quit_arm(const struct hw_run_config* cfg,
	const struct split_portal_quit_hooks* hooks) {
	memset(&s, 0, sizeof s);
	if(!cfg || !hooks || !cfg->enabled
	   || cfg->profile != HW_RUN_PROFILE_SPLIT_PORTAL_QUIT_4M) return;
	s.armed = true; s.hooks = *hooks;
	spq_core_init(&s.race, cfg->portal_remaining_ticks);
}
bool split_portal_quit_armed(void) { return s.armed; }

static void external_marker(const char* name, uint32_t tick, uint32_t up_ms) {
	if(!s.armed || s.done || !s.hooks.emit) return;
	char line[256];
	if(spq_format_marker(line, sizeof line, s.seq + 1, name, up_ms,
		tick, s.race.target, true, NULL) && s.hooks.emit(line)) s.seq++;
}
void split_portal_quit_confirm_sent(uint32_t tick, uint32_t up_ms) {
	if(!s.armed || s.confirm_seen) return;
	s.confirm_seen = true; s.confirm_tick = tick; s.confirm_ms = up_ms;
	external_marker("confirm_sent", tick, up_ms);
}
void split_portal_quit_switch_begin(uint32_t tick, uint32_t up_ms) {
	if(!s.armed || s.switch_marker_seen) return;
	s.switch_marker_seen = true; s.switch_tick = tick; s.switch_ms = up_ms;
	external_marker("switch_begin", tick, up_ms);
}
void split_portal_quit_p3_enter(uint32_t* last_seen, uint32_t begin_seq) {
	if(last_seen) *last_seen = begin_seq;
}
void split_portal_quit_observe_switch_begin(uint32_t* last_seen,
	uint32_t begin_seq, uint32_t tick, uint32_t up_ms) {
	if(!last_seen) return;
	if(begin_seq != *last_seen)
		split_portal_quit_switch_begin(tick, up_ms);
	*last_seen = begin_seq;
}
bool split_portal_quit_post_switch_position(float* x, float* y, float* z) {
	if(!s.post_switch_position_set) return false;
	if(x) *x = s.post_switch_x;
	if(y) *y = s.post_switch_y;
	if(z) *z = s.post_switch_z;
	return true;
}

void split_portal_quit_frame(const struct split_portal_quit_ctx* c,
	struct split_portal_quit_actions* out) {
	if(!out) return;
	memset(out, 0, sizeof *out);
	if(!s.armed || !c || !c->host || !c->guest || s.done) return;
	out->guest_frame_valid = true;
	out->host_frame_valid = true;
	neutral(&out->guest_frame);
	neutral(&out->host_frame);
	out->suppress_autojoin = s.step >= 8 && s.step < 14;
	if(!s.started) {
		s.started = true; s.start_ms = c->up_ms; s.step_ms = c->up_ms;
	}
	/* A pending unload keeps listening until the 240 s run is ending: a
	 * 30 s deadline alone cannot prove silence. */
	if(((s.step != 9 && s.step != 10)
		&& c->up_ms - s.start_ms >= 125000u) || c->up_ms >= 235000u) {
		fail(c, (s.step == 9 || s.step == 10)
			&& spq_resolve_unload(s.deadline_elapsed, false, true)
				== SPQ_UNLOAD_DROPPED
			? "unload_dropped_silently" : "portal_quit_timeout");
		return;
	}
	struct local_pause* pause = &c->host->pause;
	switch(s.step) {
	case 0: {
		unsigned hu, gu;
		s.host_hash = inventory_hash(c->host, &hu);
		s.guest_hash = inventory_hash(c->guest, &gu);
		if(!c->world_loaded || c->world_switch_pending
		   || !c->host->local_player || !c->guest->local_player
		   || !c->guest->local_gameplay_ready || hu == 0 || gu == 0
		   || s.block_count != SPLIT_RECOVERY_BLOCKS) {
			fail(c, "baseline_incomplete"); return;
		}
		s.host_x = c->host->camera.x;
		s.host_y = c->host->camera.y;
		s.host_z = c->host->camera.z;
		if(fabsf(c->guest->camera.x - s.host_x) < 6.0f
		   && fabsf(c->guest->camera.z - s.host_z) < 6.0f) {
			fail(c, "guest_inside_portal_frame"); return;
		}
		s.step++; break;
	}
	case 1: out->host_frame.pressed[IB_HOME] = true; s.step++; break;
	case 2:
		if(pause->open) {
			out->host_frame.pressed[IB_GUI_DOWN] = true;
			out->host_frame.pressed[IB_GUI_RIGHT] = true; s.step++;
		}
		break;
	case 3:
		if(pause->selected == 3) {
			out->host_frame.pressed[IB_GUI_CLICK] = true; s.step++;
		} else {
			out->host_frame.pressed[IB_GUI_DOWN] = true;
			out->host_frame.pressed[IB_GUI_RIGHT] = true;
		}
		break;
	case 4:
		if(pause->page == LOCAL_PAUSE_HOST_CONFIRM) {
			out->host_frame.pressed[IB_GUI_DOWN] = true; s.step++;
		}
		break;
	case 5:
		if(pause->selected != 1) break;
		if(!s.portal_armed) {
			if(!s.hooks.arm_portal || !s.hooks.arm_portal()) {
				fail(c, "portal_setup_refused"); return;
			}
			s.portal_armed = true;
		}
		s.step++; s.step_ms = c->up_ms; break;
	case 6: {
		int x = (int)floorf(s.host_x);
		int y = (int)floorf(s.host_y - 1.62f + 0.01f);
		int z = (int)floorf(s.host_z);
		uint32_t id = 0, meta = 0;
		if(s.hooks.block_at && s.hooks.block_at(x, y, z, &id, &meta)
		   && id == BLOCK_PORTAL)
			(void)advance(c, "portal_lit", "\"guest_outside\":true");
		else if(c->up_ms - s.step_ms > 10000u) fail(c, "portal_not_lit");
		break;
	}
	case 7: {
		int remaining = spq_remaining_ticks(c->portal_progress);
		if(c->switch_started || s.switch_marker_seen) {
			fail(c, "race_not_exercised_switch_before_confirm"); return;
		}
		if(c->portal_progress <= 0.0f) break;
		struct spq_decision d = spq_core_step(&s.race,
			(struct spq_observation){.remaining = remaining,
				.tick = c->tick, .up_ms = c->up_ms});
		if(d.request_quit) {
			char extra[80];
			snprintf(extra, sizeof extra, "\"observed_remaining\":%d", remaining);
			if(advance(c, "dwell_trigger", extra))
				out->host_frame.pressed[IB_GUI_CLICK] = true;
		}
		break;
	}
	case 8:
		if(c->request_after_switch) {
			fail(c, "race_not_exercised_switch_before_confirm"); return;
		}
		if(c->unload_requests > 0) {
			s.first_request_count = c->unload_requests;
			(void)advance(c, "quit_requested", "\"production_srpc\":true");
		} else if(c->up_ms - s.step_ms > 2000u)
			fail(c, "quit_request_not_enqueued");
		break;
	case 9:
		if(c->switch_started || s.switch_marker_seen) {
			if(!s.switch_marker_seen) break;
			s.switch_seen = true;
			char extra[192];
			snprintf(extra, sizeof extra,
				"\"switch_started\":true,\"confirm_tick\":%u,\"confirm_up_ms\":%u,\"switch_tick\":%u,\"switch_up_ms\":%u",
				s.confirm_tick, s.confirm_ms, s.switch_tick, s.switch_ms);
			(void)advance(c, "race_switch_first", extra);
		} else if(c->at_title) {
			if(s.deadline_elapsed) {
				fail(c, "unload_replied_late"); return;
			}
			if(pause->exit_pending || c->world_loaded) {
				fail(c, "title_before_save_settled"); return;
			}
			(void)advance(c, "race_unload_first", "\"switch_started\":false");
		} else if(c->up_ms - s.step_ms > 30000u && !s.deadline_elapsed) {
			s.deadline_elapsed = true;
			(void)mark(c, "unload_deadline", true, NULL);
		} else if(pause->exit_failed && !pause->exit_pending) {
			fail(c, s.deadline_elapsed ? "unload_replied_late"
				: "race_not_reached");
		}
		break;
	case 10:
		if(s.switch_seen && c->world_loaded && !c->world_switch_pending
		   && c->host->local_player && !s.post_switch_position_set) {
			s.post_switch_position_set = true;
			s.post_switch_x = c->host->camera.x;
			s.post_switch_y = c->host->camera.y;
			s.post_switch_z = c->host->camera.z;
		}
		if(c->at_title) {
			if(s.deadline_elapsed) {
				fail(c, "unload_replied_late"); return;
			}
			if(pause->exit_pending || c->world_loaded) {
				fail(c, "title_before_save_settled"); return;
			}
			(void)advance(c, "save_settled", "\"world_unloaded\":true");
		} else if(s.switch_seen && pause->exit_failed && !pause->exit_pending) {
			if(s.deadline_elapsed) {
				fail(c, "unload_replied_late"); return;
			}
			if(strstr(c->host->hud_message, "Save failed") == NULL) {
				fail(c, "save_refusal_not_visible"); return;
			}
			if(advance(c, "save_refused", "\"retry_visible\":true"))
				s.step = 20;
		} else if(c->up_ms - s.step_ms > 30000u && !s.deadline_elapsed) {
			s.deadline_elapsed = true;
			(void)mark(c, "unload_deadline", true, NULL);
		}
		break;
	case 11:
		if(c->at_title && advance(c, "title_entered", "\"after_settle\":true"))
			s.step_ms = c->up_ms;
		break;
	case 12:
		if(!s.autoload_armed) {
			if(!s.hooks.arm_autoload
			   || !s.hooks.arm_autoload(SPLIT_RECOVERY_RIG_WORLD)) {
				fail(c, "reload_arm_refused"); return;
			}
			s.autoload_armed = true;
		}
		s.step++; s.step_ms = c->up_ms; break;
	case 13: {
		if(!c->world_loaded || c->world_switch_pending
		   || !c->host->local_player) {
			if(c->up_ms - s.step_ms > 45000u) fail(c, "reload_timeout");
			break;
		}
		unsigned used;
		bool inv_ok = inventory_hash(c->host, &used) == s.host_hash && used > 0;
		float x = s.switch_seen ? s.post_switch_x : s.host_x;
		float y = s.switch_seen ? s.post_switch_y : s.host_y;
		float z = s.switch_seen ? s.post_switch_z : s.host_z;
		bool pos_ok = (!s.switch_seen || s.post_switch_position_set)
			&& fabsf(c->host->camera.x - x) <= 0.5f
			&& fabsf(c->host->camera.y - y) <= 0.5f
			&& fabsf(c->host->camera.z - z) <= 0.5f;
		bool blocks_ok = true;
		bool blocks_ready = true;
		bool (*read_block)(int, int, int, uint32_t*, uint32_t*)
			= c->dimension == 0 ? s.hooks.block_at : s.hooks.saved_block_at;
		for(unsigned i = 0; blocks_ok && i < SPLIT_RECOVERY_BLOCKS; i++) {
			uint32_t id = 0, meta = 0;
			struct spq_block* b = &s.blocks[i];
			if(!read_block || !read_block(b->x, b->y, b->z,
				&id, &meta)) { blocks_ready = false; break; }
			if((id != b->id || meta != b->meta)
			   && !(b->id == BLOCK_DIRT && b->meta == 0
				&& id == BLOCK_GRASS && meta == 0)) blocks_ok = false;
		}
		if(!blocks_ready) {
			if(c->up_ms - s.step_ms > 20000u)
				fail(c, "torn_or_unreadable_region_entry");
			break;
		}
		if(!inv_ok || !pos_ok || !blocks_ok) {
			fail(c, !inv_ok ? "host_inventory_lost"
				: !pos_ok ? "host_position_lost" : "region_entry_or_block_torn");
			return;
		}
		(void)advance(c, "reload_verified",
			"\"host_inventory_match\":true,\"blocks_match\":true,"
			"\"position_match\":true,\"region_intact\":true");
		break;
	}
	case 14: {
		out->suppress_autojoin = false;
		if(c->guest->local_phase == CLIENT_GUEST_LEAVING) {
			if(c->up_ms - s.step_ms > 30000u) fail(c, "guest_stuck_leaving");
			break;
		}
		if(c->guest->local_phase != CLIENT_GUEST_ATTACHED
		   || !c->guest->local_gameplay_ready || !c->guest->local_player) {
			if(c->up_ms - s.step_ms > 30000u) fail(c, "guest_rejoin_timeout");
			break;
		}
		unsigned used;
		if(inventory_hash(c->guest, &used) != s.guest_hash || used == 0) {
			fail(c, "guest_inventory_lost"); return;
		}
		(void)advance(c, "guest_rejoined",
			"\"guest_inventory_match\":true,\"guest_phase\":\"ACTIVE\"");
		break;
	}
	case 15:
		if(!s.confirm_seen || !s.switch_marker_seen
		   || !spq_after_confirm(s.confirm_tick, s.confirm_ms,
				s.switch_tick, s.switch_ms)) {
			fail(c, "race_not_reached"); return;
		}
		char extra[80];
		snprintf(extra, sizeof extra,
			"\"checklist\":\"6b\",\"winning_leg\":%d", s.race.target);
		if(advance(c, "run_ok", extra)) {
			snprintf(extra, sizeof extra, "\"winning_leg\":%d", s.race.target);
			(void)mark(c, "run_summary", true, extra);
			s.done = true;
			if(s.hooks.done) s.hooks.done("ok");
		}
		break;
	/* The visible Save failed notice leaves the menu one click from Retry. */
	case 20:
		if(pause->open && pause->selected == 3) {
			out->host_frame.pressed[IB_GUI_CLICK] = true; s.step++;
		}
		break;
	case 21:
		if(pause->page == LOCAL_PAUSE_HOST_CONFIRM) {
			out->host_frame.pressed[IB_GUI_DOWN] = true; s.step++;
		}
		break;
	case 22:
		if(pause->selected == 1) {
			out->host_frame.pressed[IB_GUI_CLICK] = true; s.step++;
		}
		break;
	case 23:
		if(c->unload_requests > s.first_request_count) {
			if(advance(c, "retry_requested", "\"production_srpc\":true"))
				s.step = 24;
		} else if(c->up_ms - s.step_ms > 5000u)
			fail(c, "retry_not_enqueued");
		break;
	case 24:
		if(c->at_title && !pause->exit_pending && !c->world_loaded) {
			if(advance(c, "save_settled", "\"world_unloaded\":true"))
				s.step = 11;
		} else if(c->up_ms - s.step_ms > 30000u)
			fail(c, "retry_did_not_reach_title");
		break;
	default: fail(c, "invalid_phase"); break;
	}
}
