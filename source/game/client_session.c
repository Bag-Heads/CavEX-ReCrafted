/* Player-owned window updates. GPL-3.0-or-later. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "client_session.h"
#include "game_state.h"
#include "respawn_clearance.h"
#include "../network/client_interface.h"
#include "../network/server_interface.h"
#include "../network/server_local_state.h"
#include "../network/vehicle_smoke_probe.h"
#include "../item/items.h"
#include "../item/inventory_sort.h"
#include "../platform/menu_sound.h"
#include "../platform/input_owner.h"
#include "../world.h"
#include "achievements/game_event.h"
#include "achievements/achievements.h"

bool client_session_convert_start(struct client_session* session, const char* path) {
	if(!session || session->convert_pending || session->local_player
	   || session->local_bound)
		return false;
	uint64_t ticket = svin_convert_request(path);
	if(!ticket)
		return false;
	session->convert = (struct svin_convert_result){
		.ticket = ticket, .outcome = SVIN_CONVERT_PENDING};
	session->convert_pending = true;
	return true;
}

bool client_session_convert_cancel(struct client_session* session) {
	return session && session->convert_pending
		&& svin_convert_cancel(session->convert.ticket);
}

bool client_session_convert_poll(struct client_session* session) {
	struct svin_convert_result result;
	if(!session || !session->convert_pending
	   || !svin_convert_read(session->convert.ticket, &result))
		return false;
	(void)svin_convert_release(result.ticket);
	session->convert = result;
	session->convert_pending = false;
	session->convert_serial++;
	return true;
}

void client_session_achievements_remote(struct client_session* session,
	uint32_t generation, bool admitted) {
	if(!session) return;
	if(session->achievement_remote_generation
	   && (!admitted || generation != session->achievement_remote_generation)) {
		game_event_emit(&(struct game_event){.kind = GE_SESSION_END,
			.authority = GE_AUTH_CLIENT,
			.who = {.slot = 0, .generation = session->achievement_epoch}});
		session->achievement_epoch = 0;
		session->achievement_remote_generation = 0;
	}
	if(admitted && generation && !session->achievement_remote_generation) {
		session->achievement_epoch = game_event_next_epoch();
		session->achievement_remote_generation = generation;
		game_event_emit(&(struct game_event){.kind = GE_SESSION_BEGIN,
			.sub = 1, .authority = GE_AUTH_CLIENT,
			.who = {.slot = 0, .generation = session->achievement_epoch}});
	}
}

void client_session_achievements_sync(struct client_session* session, uint8_t slot) {
	if(!session) return;
	session->achievement_epoch = game_event_owner_epoch(slot);
}
bool client_session_achievements_warning(struct client_session* session, uint8_t slot, bool ready) {
	if(!session || (ready && !achievements_slot_save_failed(slot))) return false;
	if(session->hud_message[0] && !time_reached(time_get(), session->hud_message_until)) return false;
	snprintf(session->hud_message, sizeof(session->hud_message), "%s", ready
		? "Achievements save failed. Save/Leave to retry."
		: "Achievements unavailable: profile/init error.");
	session->hud_message_until = time_add_ms(time_get(), 10000);
	return true;
}

/* Two simultaneous local players; each plan is 15,264 B on Broadway. Only
 * main-thread callers lease these buffers. No plan enters a stack frame. */
static struct {
	struct client_session* owner;
	struct inventory_sort_plan plan;
} sort_buffers[CLIENT_LOCAL_PLAYERS];
static uint32_t sort_admission_ticket;
static uint32_t recipe_craft_ticket;

static bool sort_imported_pins(void* user, struct client_session* session,
	uint8_t window, bool* mask, size_t count) {
	(void)user;
	struct inventory* inv = windowc_get_latest(session->windows[window]);
	for(size_t k = 0; k < count; k++)
		mask[k] = item_data_pinned(&inv->items[k]);
	return true;
}

static bool sort_viewer(struct client_session* session, uint8_t window) {
	return session && session->local_player && !session->local_reply_failed
		&& session->windows[window]
		&& (session->visible_window == window
			|| (window == WINDOWC_INVENTORY && session->visible_window < 0));
}

static bool sort_stack_equal(struct item_data a, struct item_data b) {
	return a.id == b.id && (!a.id || (a.count == b.count && a.durability == b.durability));
}

/* Remote automation alone has a deadline. Reuse rejection retirement and its
 * cursor-snapshot fence, without fabricating a Packet106 rejection response. */
static bool sort_expire(struct window_container* wc, ptime_t now) {
	struct window_sort_session* sort = &wc->sort;
	if(sort->status != WINDOW_SORT_RUNNING || !sort->remote
	   || sort->acknowledged == sort->submitted
	   || !time_reached(now, sort->deadlines[sort->acknowledged % WINDOW_SORT_PIPELINE]))
		return false;
	windowc_action_apply_result(wc, sort->actions[sort->acknowledged % WINDOW_SORT_PIPELINE], false);
	wc->correction_pending = true;
	return true;
}

bool client_session_sort_start(struct client_session* session, uint8_t window,
	const struct inventory_sort_plan* plan, bool chest, bool remote, ptime_t now,
	container_send_fn send, void* user) {
	(void)now; /* Local admission waits just like an ordinary click. */
	if(!sort_viewer(session, window) || !plan || !send) return false;
	struct window_container* wc = session->windows[window];
	/* Admission scope is certified against the planner's range, so a caller
	 * cannot label a chest plan as a main-inventory plan to bypass viewers. */
	if(!((wc->type == WINDOW_TYPE_CHEST && (wc->slot_count == 63 || wc->slot_count == 90))
	   || (wc->type == WINDOW_TYPE_INVENTORY && wc->slot_count == 45 && !chest))) return false;
	size_t main = wc->slot_count - 36;
	if(plan->begin != (chest ? 0 : main) || plan->end != (chest ? main : main + 27)) return false;
	if(wc->sort.status == WINDOW_SORT_RUNNING || wc->sort.status == WINDOW_SORT_ADMISSION
	   || wc->generation != plan->generation || !plan->count
	   || plan->count > INVENTORY_SORT_MAX_CLICKS
	   || wc->slot_count > INVENTORY_SORT_WINDOW_SLOTS
	   || windowc_actions_pending(wc) || wc->quick_pending
	   || wc->correction_pending || wc->transaction_reply_pending
	   || inventory_get_picked_item(windowc_get_latest(wc), NULL)) return false;
	wc->sort = (struct window_sort_session){.plan = plan, .chest = chest, .remote = remote,
		.admission_ticket = ++sort_admission_ticket,
		.status = remote ? WINDOW_SORT_RUNNING : WINDOW_SORT_ADMISSION,
		.generation = wc->generation};
	if(!remote && !send(user, &(struct server_rpc){.type = SRPC_WINDOW_SORT_ADMIT,
		.payload.window_sort = {.window = window, .ticket = wc->sort.admission_ticket, .chest = chest}})) {
		windowc_sort_interrupt(wc);
		return false;
	}
	return true;
}

bool client_session_sort_request(struct client_session* session, uint8_t window,
	unsigned operation, bool remote, ptime_t now,
	container_send_fn send, void* user) {
	if(!sort_viewer(session, window)) return false;
	struct window_container* wc = session->windows[window];
	if(wc->sort.status == WINDOW_SORT_RUNNING || wc->sort.status == WINDOW_SORT_ADMISSION
	   || wc->slot_count > INVENTORY_SORT_WINDOW_SLOTS || windowc_actions_pending(wc)
	   || wc->quick_pending || wc->correction_pending || wc->transaction_reply_pending
	   || inventory_get_picked_item(windowc_get_latest(wc), NULL)) return false;
	/* A session cannot overwrite its own still-live plan in another window. */
	for(size_t k = 0; k < 256; k++)
		if(session->windows[k] && (session->windows[k]->sort.status == WINDOW_SORT_RUNNING
		   || session->windows[k]->sort.status == WINDOW_SORT_ADMISSION)) return false;
	size_t buffer = CLIENT_LOCAL_PLAYERS;
	for(size_t k = 0; k < CLIENT_LOCAL_PLAYERS; k++)
		if(sort_buffers[k].owner == session) { buffer = k; break; }
	if(buffer == CLIENT_LOCAL_PLAYERS)
		for(size_t k = 0; k < CLIENT_LOCAL_PLAYERS; k++)
			if(!sort_buffers[k].owner) { buffer = k; break; }
	if(buffer == CLIENT_LOCAL_PLAYERS) return false;
	sort_buffers[buffer].owner = session;
	bool pinned[INVENTORY_SORT_WINDOW_SLOTS] = {0};
	uint8_t limits[INVENTORY_SORT_WINDOW_SLOTS] = {0};
	sort_pinned_mask_fn pinned_mask = session->sort_pinned_mask
		? session->sort_pinned_mask : sort_imported_pins;
	if(!pinned_mask(session->sort_pinned_user,
	   session, window, pinned, wc->slot_count)) return false;
	struct inventory* inv = windowc_get_latest(wc);
	for(size_t k = 0; k < wc->slot_count; k++) {
		struct item* def = item_get(&inv->items[k]);
		if(def) limits[k] = def->max_stack;
	}
	struct inventory_sort_input input = {.slots = inv->items, .capacity = wc->slot_count,
		.cursor = inv->picked_item, .window = wc->type, .operation = operation,
		.pinned = pinned, .max_stack = limits, .snapshot_generation = wc->generation,
		.current_generation = wc->generation};
	struct inventory_sort_plan* plan = &sort_buffers[buffer].plan;
	enum inventory_sort_result result = inventory_sort_build(&input, plan);
	if(result == INVENTORY_SORT_UNCHANGED) return true;
	return result == INVENTORY_SORT_READY && client_session_sort_start(session, window,
		plan, operation <= INVENTORY_SORT_CHEST_STACK, remote, now, send, user);
}

void client_session_sort_tick(struct client_session* session, uint8_t window,
	ptime_t now, container_send_fn send, void* user) {
	if(!session || !session->windows[window]) return;
	struct window_container* wc = session->windows[window];
	struct window_sort_session* sort = &wc->sort;
	if(sort->status != WINDOW_SORT_RUNNING && sort->status != WINDOW_SORT_ADMISSION) return;
	if(!send || !sort_viewer(session, window) || wc->generation != sort->generation
	   || wc->correction_pending || wc->transaction_reply_pending) {
		windowc_sort_interrupt(wc); return;
	}
	if(sort_expire(wc, now) || sort->status == WINDOW_SORT_ADMISSION) return;
	const struct inventory_sort_plan* plan = sort->plan;
	if(sort->acknowledged == plan->count) {
		struct inventory* inv = windowc_get_latest(wc);
		bool complete = !windowc_actions_pending(wc) && !inventory_get_picked_item(inv, NULL);
		for(size_t k = 0; k < wc->slot_count; k++)
			complete &= sort_stack_equal(inv->items[k], plan->target[k]);
		sort->status = complete ? WINDOW_SORT_COMPLETE : WINDOW_SORT_INTERRUPTED;
		/* The approved confirmation cue for a completed sort
		 * (docs/ui_design/approved/specs/crafting-chests.md:27 "Sound only on
		 * a real selection/menu change or completed successful craft/sort.
		 * Failed actions ... are silent"; sound-events.md's runtime rule
		 * "Actual transfer/save success hooks must come from authoritative
		 * completed operations"). This IS that authoritative point: every
		 * click of the plan has been ACCEPTED by the server
		 * (acknowledged == plan->count, counted only in
		 * windowc_action_apply_result's accept branch) AND the reconciled
		 * window equals the plan's target with nothing pending and no cursor
		 * stack. Never on submit and never on the local prediction; an
		 * interrupted, rejected, expired or unchanged sort never reaches
		 * here, so it stays silent. The RUNNING/ADMISSION guard at the top of
		 * this function makes the transition -- and therefore the cue --
		 * happen exactly once per sort. In split screen this runs on the
		 * ACTING session's own window, so a second viewer, whose sort was
		 * refused admission, emits nothing; the dispatcher's shared
		 * confirmation guard then bounds two players' cues per frame. */
		if(complete) audio_menu_event(MENU_SOUND_SELECT, now);
		return; /* Never declared from the ACK callback itself. */
	}
	while(sort->status == WINDOW_SORT_RUNNING && sort->submitted < plan->count
	   && sort->submitted - sort->acknowledged < WINDOW_SORT_PIPELINE) {
		const struct inventory_sort_click* click = &plan->clicks[sort->submitted];
		if(click->slot >= wc->slot_count
		   || !sort_stack_equal(windowc_get_latest(wc)->items[click->slot], click->expected)) {
			windowc_sort_interrupt(wc); return;
		}
		uint16_t action;
		/* Exactly the human-click prediction + Packet102 submit path. The
		 * planner's precomputed expected stack is the wire comparison value. */
		if(!windowc_new_action(wc, &action, click->right, click->slot)) {
			windowc_sort_interrupt(wc); return;
		}
		struct inventory* predicted = windowc_get_latest(wc);
		if(!sort_stack_equal(predicted->items[click->slot], click->slot_after)
		   || !sort_stack_equal(predicted->picked_item, click->cursor_after)
		   || !send(user, &(struct server_rpc){.type = SRPC_WINDOW_CLICK,
			.payload.window_click = {.window = window, .slot = click->slot,
				.action_id = action, .right_click = click->right,
				.expected = click->expected, .sort_chest = sort->chest}})) {
			windowc_cancel_unsent(wc, action);
			windowc_sort_interrupt(wc); return;
		}
		predicted->revision.sent = true;
		size_t index = sort->submitted++ % WINDOW_SORT_PIPELINE;
		sort->actions[index] = action;
		sort->deadlines[index] = time_add_ms(now, WINDOW_SORT_TIMEOUT_MS);
	}
}

void client_session_clear_windows(struct client_session* session) {
	if(!session) return;
	for(size_t k = 0; k < CLIENT_LOCAL_PLAYERS; k++)
		if(sort_buffers[k].owner == session) sort_buffers[k].owner = NULL;
	session->map_counter = 0;
	session->local_bound = false;
	session->local_ticket = 0;
	session->local_phase = CLIENT_GUEST_IDLE;
	session->local_pending = false;
	session->local_leave_deadline_armed = false;
	session->local_leave_retry_handed_off = false;
	session->local_leave_receipt = (struct client_local_leave_token){0};
	session->local_leave_receipt_outcome = CLIENT_LOCAL_LEAVE_STALE;
	session->local_leave_receipt_active = false;
	session->local_leave_receipt_manual_retry = false;
	/* Gameplay admission dies with the session's window ownership: the body,
	 * its residency anchor and the two-view gate are all downstream of this
	 * flag, and none of them may outlive the generation that earned them.
	 * The BODY itself is the caller's (client_interface's) to retire -- this
	 * function never touches the shared entity store. */
	session->local_gameplay_ready = false;
	session->local_fire_latched = false;
	session->local_fire = false;
	/* admit-retry-0925: retained first-body replies die with the generation's
	 * window ownership (leave, unbind, world reset) -- never replayed into a
	 * later body and never handed to the host. The retirement reason lives in
	 * hud_message and deliberately survives until the next bind. */
	session->local_admit = (struct client_guest_admit_recovery){0};
	session->hud_queue_head = session->hud_queue_count = 0;
	session->hud_queue_dropped = 0;
	session->hud_message_protected = false;
	session->local_respawn_loading = false;
	session->local_respawn_correction_installed = false;
	session->local_respawn_ack_send_pending = false;
	session->local_respawn_awaiting_commit = false;
	session->local_respawn_epoch = session->local_respawn_completed_epoch = 0;
	session->local_respawn_render_camera_frozen = false;
	session->respawn_failure=RESPAWN_FAILURE_NONE;
	session->respawn_failure_epoch=0;
	session->respawn_request_observation_pending=false;
	session->local_death_leave_manual=false;
	for(size_t k = 0; k < 256; k++) {
		if(session->windows[k]) {
			windowc_destroy(session->windows[k]);
			free(session->windows[k]);
			session->windows[k] = NULL;
		}
	}
	session->container = (struct container_compact){0};
	session->recipes = (struct recipe_browser){0};
	session->manual_grid = (struct manual_grid){0};
	session->menu_host = (struct local_menu_host){0};
	/* card 4 (2026-09-16, Astra #9): a host Save and quit that is ALREADY pending
	 * survives a dimension-switch reset -- the queued unload still answers with
	 * WORLD_RESET(0) or RETURN_INGAME and client_interface settles this pause on
	 * either reply. Wiping it here left the host on the loading screen with no
	 * world and nothing to press (tests/test_local_pause.c). Every other pause
	 * (guest, or a host pause without a pending exit) is cleared as before. */
	if(!(session->pause.open && session->pause.host && session->pause.exit_pending))
		session->pause = (struct local_pause){0};
	session->death = (struct death_ui){0};
	session->sign = (struct sign_editor){0};
	session->visible_window = session->deferred_reopen_window = -1;
	/* The window a pending craft named is gone, so its reply can never be
	 * installed; retire it here rather than leaving the guide blocked. */
	session->craft_pending = false;
	session->craft_reason = RECIPE_CRAFT_INVALID_REQUEST;
}

bool client_session_unbind_local(struct client_session* session, uint64_t generation) {
	if(!session || !session->local_bound || !generation
	   || generation != session->local_generation) return false;
	client_session_clear_windows(session);
	return true;
}

bool client_session_bind_local(struct client_session* session, uint64_t generation) {
	if(!session || session->local_player || !generation || generation <= session->local_generation)
		return false;
	struct window_container* inventory = malloc(sizeof(*inventory));
	if(!inventory) return false;
	if(!windowc_create(inventory, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE)) {
		free(inventory);
		return false;
	}
	client_session_clear_windows(session);
	/* A new provider generation has no body yet. Retire the departed body's
	 * sleep, notices, correction/send latches and camera/ride presentation only
	 * after the replacement inventory allocation has succeeded. */
	*session = (struct client_session) {
		.local_generation = generation, .local_bound = true,
		.visible_window = -1, .deferred_reopen_window = -1,
	};
	/* Wii time_get has a large epoch; a zero latch can narrow to a negative
	 * 32-bit send age and silence this generation's position stream. */
	session->player_sync.last_pos_update = time_get();
	windowc_get_latest(inventory)->client_map_counter = &session->map_counter;
	session->windows[WINDOWC_INVENTORY] = inventory;
	return true;
}

/* HUD notice presentation (Astra reviews A item 4, 3 items 3-4). hud_message
 * is the one slot the HUD draws; hud_queue holds notices waiting behind it.
 * Only a first-body replay (admit_replay) creates protected turns, so with no
 * protected notice showing and nothing queued an ordinary notice still shows
 * at once, exactly as before. */
static void hud_show(struct client_session* session, const char* text,
	ptime_t now, bool protect) {
	memcpy(session->hud_message, text, sizeof(session->hud_message));
	session->hud_message[sizeof(session->hud_message) - 1] = '\0';
	session->hud_message_until = time_add_ms(now, CLIENT_HUD_NOTICE_MS);
	session->hud_message_protected = protect;
}

static bool hud_turn_held(const struct client_session* session, ptime_t now) {
	return session->hud_queue_count
		|| (session->hud_message_protected && session->hud_message[0]
			&& !time_reached(now, session->hud_message_until));
}

static void hud_enqueue(struct client_session* session, const char* text) {
	if(session->hud_queue_count >= CLIENT_HUD_QUEUE_MAX) {
		/* Loss-explicit: the arriving notice is dropped and counted. */
		if(session->hud_queue_dropped < UINT16_MAX)
			session->hud_queue_dropped++;
		return;
	}
	unsigned slot = (session->hud_queue_head + session->hud_queue_count++)
		% CLIENT_HUD_QUEUE_MAX;
	memcpy(session->hud_queue[slot], text, sizeof(session->hud_queue[slot]));
	session->hud_queue[slot][sizeof(session->hud_queue[slot]) - 1] = '\0';
}

void client_session_hud_tick(struct client_session* session, ptime_t now) {
	if(!session || (session->hud_message[0]
		   && !time_reached(now, session->hud_message_until))) return;
	if(session->hud_queue_count) {
		hud_show(session, session->hud_queue[session->hud_queue_head], now, true);
		session->hud_queue_head = (session->hud_queue_head + 1) % CLIENT_HUD_QUEUE_MAX;
		session->hud_queue_count--;
	} else if(session->hud_queue_dropped) {
		char more[48];
		snprintf(more, sizeof(more), "(+%u more)",
			(unsigned)session->hud_queue_dropped);
		session->hud_queue_dropped = 0;
		hud_show(session, more, now, true);
	} else {
		session->hud_message_protected = false;
	}
}

bool client_session_player_reply(struct client_session* session,
	const struct client_rpc* call, ptime_t now) {
	if(!session || !call) return false;
	switch(call->type) {
		case CRPC_PLAYER_SET_HEALTH:
			if(!session->local_player) return false;
			session->local_player->health = call->payload.player_set_health.health;
			return true;
		case CRPC_HUD_MESSAGE:
			/* The wire field is bounded even if its producer omitted the NUL
			 * (both helpers terminate). Behind a queued backlog it waits its
			 * turn; otherwise it shows at once, as it always has. */
			if(hud_turn_held(session, now))
				hud_enqueue(session, call->payload.hud_message);
			else
				hud_show(session, call->payload.hud_message, now, false);
			return true;
		case CRPC_SLEEP_STATE:
			session->sleep_timer = call->payload.sleep_state.timer;
			session->sleeping = call->payload.sleep_state.sleeping;
			return true;
		default: return false;
	}
}

int32_t client_session_sleep_timer_step(bool sleeping, int32_t timer) {
	/* EntityPlayer.java:60-78: while sleeping, the sleep timer counts up
	 * and is capped at 100; otherwise, while the timer is above 0, it
	 * still counts up, and on reaching 110 or more it resets
	 * to 0 -- isPlayerSleeping() is the bare flag (line 682-684). */
	if(sleeping) {
		timer++;
		return timer > 100 ? 100 : timer;
	}
	if(timer > 0) {
		timer++;
		return timer >= 110 ? 0 : timer;
	}
	return timer;
}

/* ---- admit-retry-0925: first-body admission recovery ----------------------
 * Ruling: tmp/jobs/split-dimensions-plan-0924/out/ASTRA-RETIRE-RETRY-0925.md.
 * Bounds and retained-state rules are documented with the constants in
 * client_session.h. */
static void admit_notice(struct client_session* session, const char* reason) {
	/* Controller-readable. The guest has no viewport without a body, so the
	 * reason lives in its own slot the HOST HUD draws on a dedicated line
	 * (client_session_local_admit_failure_reason); it is also mirrored into
	 * this session's notice for a guest viewport that later draws. */
	session->hud_queue_count = 0;
	session->hud_queue_dropped = 0;
	snprintf(session->hud_message, sizeof(session->hud_message), "%s", reason);
	session->hud_message_until = time_add_ms(time_get(),
		CLIENT_GUEST_ADMIT_NOTICE_MS);
	session->hud_message_protected = true;
	snprintf(session->local_admit_notice, sizeof(session->local_admit_notice),
		"%s", reason);
	session->local_admit_notice_pending = true;
}

bool client_session_local_admit_failure_present(struct client_session* guest,
	struct client_session* host) {
	(void)host; /* the reason has its own slot; see ..._failure_reason */
	if(!guest || !guest->local_admit_notice_pending) return false;
	guest->local_admit_notice_pending = false;
	return true;
}

const char* client_session_local_admit_failure_reason(
	const struct client_session* guest) {
	return guest && guest->local_admit_notice[0]
		? guest->local_admit_notice : NULL;
}

void client_session_local_admit_failure_clear(struct client_session* guest) {
	if(!guest) return;
	guest->local_admit_notice[0] = '\0';
	guest->local_admit_notice_pending = false;
}

/* Exhaustion, timeout and overflow: drop the retained replies, then retire the
 * guest through the authoritative Leave so the server releases its body. */
static void admit_retire(struct client_session* session, const char* reason) {
	session->local_admit = (struct client_guest_admit_recovery){.retiring = true};
	session->local_gameplay_ready = false;
	admit_notice(session, reason);
	(void)client_session_request_local_leave(session);
}

void client_session_local_admit_refused(struct client_session* session,
	enum client_guest_admit_refusal refusal) {
	if(!session || !session->local_bound
	   || session->local_phase != CLIENT_GUEST_ATTACHED
	   || session->local_player || session->local_gameplay_ready) return;
	if(refusal == CLIENT_GUEST_ADMIT_INVALID) {
		/* An id the server cannot have meant for us: no retry can fix it.
		 * Retire at once -- through the authoritative Leave, so the server
		 * releases the guest it allocated and the session unbinds (Astra
		 * review A, item 1): a later Join must be able to succeed. */
		admit_retire(session, "Player 2 could not join: bad body id");
		return;
	}
	if(!session->local_admit.active) {
		session->local_admit = (struct client_guest_admit_recovery){
			.active = true,
			.deadline = time_add_ms(time_get(), CLIENT_GUEST_ADMIT_TIMEOUT_MS)};
	}
	if(++session->local_admit.refusals > CLIENT_GUEST_ADMIT_RETRY_MAX)
		admit_retire(session, "Player 2 could not join: out of memory");
}

/* Body-bound reply for a body whose first install was refused. True means
 * retained (or the overflow retired the guest); false means not in recovery. */
static bool admit_retain(struct client_session* session,
	const struct client_rpc* call) {
	struct client_guest_admit_recovery* r = &session->local_admit;
	if(!r->active || session->local_player) return false;
	switch(call->type) {
		case CRPC_PLAYER_SET_HEALTH:
			r->health_retained = true;
			r->health = call->payload.player_set_health.health;
			return true;
		case CRPC_SLEEP_STATE:
			r->sleep_retained = true;
			r->sleep_timer = call->payload.sleep_state.timer;
			r->sleeping = call->payload.sleep_state.sleeping;
			return true;
		case CRPC_HUD_MESSAGE:
			if(r->hud_count >= CLIENT_GUEST_ADMIT_HUD_RETAIN_MAX) {
				admit_retire(session, "Could not join: too many pending messages");
				return true;
			}
			memcpy(r->hud[r->hud_count], call->payload.hud_message,
				sizeof(r->hud[0]));
			r->hud[r->hud_count][sizeof(r->hud[0]) - 1] = '\0';
			r->hud_count++;
			return true;
		default: return false;
	}
}

/* Replay into the freshly installed body, before gameplay admission opens. */
static void admit_replay(struct client_session* session) {
	struct client_guest_admit_recovery r = session->local_admit;
	session->local_admit = (struct client_guest_admit_recovery){0};
	if(!r.active) return;
	ptime_t now = time_get();
	if(r.health_retained)
		(void)client_session_player_reply(session, &(struct client_rpc){
			.type = CRPC_PLAYER_SET_HEALTH,
			.payload.player_set_health.health = r.health}, now);
	if(r.sleep_retained)
		(void)client_session_player_reply(session, &(struct client_rpc){
			.type = CRPC_SLEEP_STATE, .payload.sleep_state = {
				.timer = r.sleep_timer, .sleeping = r.sleeping}}, now);
	/* Each retained notice keeps its own toast turn, in arrival order: the
	 * first shows now, the rest wait in the presentation queue. */
	for(uint8_t k = 0; k < r.hud_count; k++) {
		if(k == 0 && !hud_turn_held(session, now))
			hud_show(session, r.hud[k], now, true);
		else hud_enqueue(session, r.hud[k]);
	}
}

bool client_session_local_reply(struct client_session* session,
	uint64_t generation, const struct client_rpc* call) {
	if(!session || !call || !session->local_bound || session->local_reply_failed
	   || generation != session->local_generation) return false;
	if(call->type == CRPC_LOCAL_PLAYER_FIRE) {
		/* lava-aabb-0924/B3+B4: presentation-only, sent on change plus forced
		 * resend edges (server_local_publish_split_fire); a body that is not
		 * installed yet (or was just released) cannot take it -- never a reply
		 * failure that would retire the guest. C2b: the value is latched on the
		 * session FIRST, whether or not a body exists, because the server
		 * records this send as delivered the moment the route accepts it and
		 * will not repeat it for a body the retried READY installs later
		 * (clin_guest_body_install applies the latch). */
		if(call->payload.local_player_fire.owner == 1) {
			session->local_fire_latched = true;
			session->local_fire = call->payload.local_player_fire.on_fire;
			(void)clin_apply_player_fire(session, call);
		}
		return true;
	}
	if(call->type == CRPC_LOCAL_PLAYER_VELOCITY) {
		/* damage-client-0926: the guest body's knockback motionY. Motion for
		 * a body that is not installed has nothing to move (Beta's
		 * handleEntityVelocity is a no-op for an unknown entity id,
		 * NetClientHandler.java:165-170) -- dropped, never a reply failure. */
		if(call->payload.local_player_velocity.owner == 1)
			(void)clin_apply_player_velocity(session, call);
		return true;
	}
	if(call->type == CRPC_PLAYER_SET_HEALTH || call->type == CRPC_HUD_MESSAGE
	   || call->type == CRPC_SLEEP_STATE) {
		/* Window attachment alone cannot install gameplay into a missing body.
		 * No fallback to the primary body, even for a current-generation reply.
		 * admit-retry-0925: during a first-install recovery the reply is
		 * retained for replay instead of poisoning the generation. */
		if(!session->local_player) return admit_retain(session, call);
		return client_session_player_reply(session, call, time_get());
	}
	if(call->type == CRPC_OPEN_WINDOW || call->type == CRPC_REOPEN_WINDOW) {
		bool reopen = call->type == CRPC_REOPEN_WINDOW;
		unsigned id = reopen ? call->payload.window_reopen.window : call->payload.window_open.window;
		enum window_type type = reopen ? call->payload.window_reopen.type : call->payload.window_open.type;
		size_t count = reopen ? call->payload.window_reopen.slot_count : call->payload.window_open.slot_count;
		local_menu_host_reset(session); session->sign = (struct sign_editor){0};
		session->visible_window = session->deferred_reopen_window = -1;
		if(count == 0 || count > CRPC_WINDOW_ITEMS_MAX_SLOTS) return false;
		struct window_container* window = malloc(sizeof(*window));
		if(!window) return false;
		if(!windowc_create(window, type, count)) { free(window); return false; }
		if(session->windows[id]) {
			windowc_destroy(session->windows[id]);
			free(session->windows[id]);
		}
		windowc_get_latest(window)->client_map_counter = &session->map_counter;
		session->windows[id] = window;
		session->visible_window = id;
		return true;
	}
	if(call->type == CRPC_WINDOW_CLOSE_REMOTE) {
  uint8_t id=call->payload.window_close_remote.window;
  if(id!=WINDOWC_INVENTORY) {
   if(session->deferred_reopen_window==id)session->deferred_reopen_window=-1;
   if(session->visible_window==id||(local_menu_host_active(session)&&session->menu_host.window==id)) {
    local_menu_host_reset(session); session->sign = (struct sign_editor){0};session->visible_window=-1;
   }
   if(session->windows[id]){windowc_destroy(session->windows[id]);free(session->windows[id]);session->windows[id]=NULL;}
  }
  return true;
 }
 if(call->type == CRPC_RETURN_INGAME) {
  local_menu_host_reset(session); session->sign = (struct sign_editor){0};
		session->visible_window = session->deferred_reopen_window = -1;
		return true;
	}
	return client_session_window_reply(session, call);
}

static void clin_mirror_player_section(struct client_session* session, uint8_t window, size_t slot,
									   struct item_data item) {
	if(window == WINDOWC_INVENTORY || !session->windows[window]
	   || !session->windows[WINDOWC_INVENTORY])
		return;
	size_t main_start, hotbar_start;
	switch(session->windows[window]->type) {
		case WINDOW_TYPE_CHEST:
			/* Large chests use the SAME wire type, with 54 storage slots.
			 * Quick-move publishes individual slot updates while still open;
			 * using the 27-slot offsets mirrors storage into the player's bag. */
			if(session->windows[window]->slot_count < 36) return;
			main_start = session->windows[window]->slot_count - 36;
			hotbar_start = main_start + 27;
			break;
		case WINDOW_TYPE_IRON_CHEST:
			main_start = IRON_CHEST_SLOT_MAIN;
			hotbar_start = IRON_CHEST_SLOT_HOTBAR;
			break;
		case WINDOW_TYPE_FURNACE:
			main_start = FURNACE_SLOT_MAIN;
			hotbar_start = FURNACE_SLOT_HOTBAR;
			break;
		case WINDOW_TYPE_DISPENSER:
			main_start = DISPENSER_SLOT_MAIN;
			hotbar_start = DISPENSER_SLOT_HOTBAR;
			break;
		case WINDOW_TYPE_WORKBENCH:
			main_start = CRAFTING_SLOT_MAIN;
			hotbar_start = CRAFTING_SLOT_HOTBAR;
			break;
		default:
			return; // sign/unknown: no player section
	}
	size_t base_slot;
	if(slot >= hotbar_start && slot < hotbar_start + INVENTORY_SIZE_HOTBAR)
		base_slot = INVENTORY_SLOT_HOTBAR + (slot - hotbar_start);
	else if(slot >= main_start && slot < main_start + 27)
		base_slot = INVENTORY_SLOT_MAIN + (slot - main_start);
	else
		return; // container-own section
	windowc_slot_change(session->windows[WINDOWC_INVENTORY], base_slot, item);
}

void client_session_flush_window_transactions(struct client_session* session) {
	if(!session) return;
	struct server_rpc_source source = {0};
	if(session->local_bound)
		source = (struct server_rpc_source){.slot = 1, .generation = session->local_generation};
	for(size_t window = 0; window < 256; window++) {
		struct window_container* wc = session->windows[window];
		if(!wc || !wc->transaction_reply_pending) continue;
		struct server_rpc response = {.type = SRPC_WINDOW_TRANSACTION,
			.payload.window_transaction = {.window = window,
				.action_id = wc->transaction_reply_action, .accepted = true}};
		if(svin_rpc_send_from(source, &response)) wc->transaction_reply_pending = false;
	}
	/* Remote only: a real Beta server may silently ignore a click
	 * (NetServerHandler.java:499) and then never answers it, which used to
	 * hold the window (and its Close) forever. Retire it after
	 * WINDOWC_REPLY_TIMEOUT_MS without progress; acks above go first. The
	 * local provider answers every click in FIFO order, so it is untouched. */
	if(!gstate.remote_session) return;
	ptime_t now = time_get();
	for(size_t window = 0; window < 256; window++)
		if(session->windows[window])
			(void)windowc_expire_unanswered(session->windows[window], now);
}

bool client_session_window_reply(struct client_session* session,
	const struct client_rpc* call) {
	if(!session || !call) return false;
	switch(call->type) {
		case CRPC_RECIPE_CRAFT:
			/* A delayed reply from a retired request/window/player is inert. Slot
			 * updates precede this reply in the actor's FIFO, so a matching reply
			 * is the authoritative completion edge lane 4 may present. */
			if(session->craft_pending
			   && call->payload.recipe_craft.ticket == session->craft_pending_ticket
			   && call->payload.recipe_craft.player == session->craft_pending_player
			   && call->payload.recipe_craft.window == session->craft_pending_window) {
				session->craft_pending = false;
				session->craft_reason = call->payload.recipe_craft.reason;
				session->craft_reply_serial++;
			}
			break;
		case CRPC_WINDOW_SORT_ADMIT: {
			struct window_container* wc = session->windows[call->payload.window_sort.window];
			if(wc && wc->sort.status == WINDOW_SORT_ADMISSION
			   && wc->sort.admission_ticket == call->payload.window_sort.ticket
			   && wc->generation == wc->sort.generation) {
				wc->sort.status = call->payload.window_sort.accepted
					? WINDOW_SORT_RUNNING : WINDOW_SORT_INTERRUPTED;
			}
			break;
		}
		case CRPC_INVENTORY_SLOT: {
			/* Packet103SetSlot inbound: the sibling of CRPC_WINDOW_ITEMS'
			 * fix just below, closed here for the exact same reason. `slot`
			 * is only bounded by mp_codec's own u8-cast (0..255, see
			 * server_remote.c's MP_EVENT_SET_SLOT) -- never against THIS
			 * window's real capacity -- so a remote server naming a small
			 * window (a 27-slot chest, say) with a large slot index would
			 * walk windowc_slot_change -> inventory_set_slot past
			 * inv->items[], whose own bound check is an assert() that
			 * compiles out under the shipping .dol's NDEBUG (the same bug
			 * class test_window_click_slot_bound.c guards for
			 * SRPC_WINDOW_CLICK, and CRPC_WINDOW_ITEMS was already fixed
			 * for). A well-behaved server never sends an out-of-range
			 * single-slot update; drop it rather than corrupt memory.
			 *
			 * SPECIAL_SLOT_PICKED_ITEM (255, inventory.h) is EXEMPT from
			 * this clamp: it is windowc_slot_change's own sentinel for the
			 * held/cursor stack, routed to inventory_set_picked_item's
			 * dedicated struct field, never an inv->items[] index -- it is
			 * legitimately >= every real window's slot_count by
			 * construction (test_portal_travel.c's reopened-cursor
			 * regression exercises exactly this: a bare `< slot_count`
			 * clamp silently drops every real cursor-restore CRPC). */
			uint8_t window = call->payload.inventory_slot.window;
			uint8_t slot = call->payload.inventory_slot.slot;
			if(session->windows[window]
			   && (slot == SPECIAL_SLOT_PICKED_ITEM
				   || (size_t)slot < session->windows[window]->slot_count)) {
				windowc_slot_change(session->windows[window], slot,
									call->payload.inventory_slot.item);
				if(slot == SPECIAL_SLOT_PICKED_ITEM)
					session->windows[window]->correction_pending = false;
				if(slot != SPECIAL_SLOT_PICKED_ITEM)
					clin_mirror_player_section(session,
						window, slot, call->payload.inventory_slot.item);
			}
			break;
		}
		case CRPC_WINDOW_ITEMS: {
			/* Packet104WindowItems: apply every slot in this one clin_process
			 * call, which is already atomic relative to every other CRPC.
			 * `count` is only bounded to CRPC_WINDOW_ITEMS_MAX_SLOTS at the
			 * mp_codec/server_remote boundary, never against THIS window's
			 * real capacity -- a remote server naming a smaller window
			 * (furnace/workbench) with a larger count would otherwise walk
			 * windowc_slot_change -> inventory_set_slot past inv->items[],
			 * whose own bound check is an assert() that compiles out under
			 * the shipping .dol's NDEBUG (the exact bug class
			 * test_window_click_slot_bound guards for SRPC_WINDOW_CLICK).
			 * Clamp to the window's own slot_count here -- a runtime check,
			 * not an assert -- mirroring the inventory_slot_valid guard
			 * server_local.c uses for SRPC_WINDOW_CLICK. Slots beyond
			 * capacity are safely dropped, never installed; a well-behaved
			 * server never sends them anyway. */
			uint8_t window = call->payload.window_items.window;
			if(session->windows[window]) {
				uint16_t count = call->payload.window_items.count;
				if(count > CRPC_WINDOW_ITEMS_MAX_SLOTS)
					count = CRPC_WINDOW_ITEMS_MAX_SLOTS;
				size_t capacity = session->windows[window]->slot_count;
				if((size_t)count > capacity)
					count = (uint16_t)capacity;
				for(uint16_t i = 0; i < count; i++) {
					windowc_slot_change(session->windows[window], i,
										call->payload.window_items.items[i]);
					clin_mirror_player_section(session,
						window, i, call->payload.window_items.items[i]);
				}
			}
			break;
		}
		case CRPC_PLAYER_HOTBAR_SLOT:
			if(session->windows[WINDOWC_INVENTORY]
			   && call->payload.player_hotbar_slot.slot < INVENTORY_SIZE_HOTBAR)
				windowc_hotbar_change(
					session->windows[WINDOWC_INVENTORY],
					call->payload.player_hotbar_slot.slot);
			break;
		case CRPC_WINDOW_TRANSACTION: {
			uint8_t window = call->payload.window_transaction.window;
			struct window_container* sorting = session->windows[window];
			if(sorting) sort_expire(sorting, time_get());
			if(session->windows[window])
				windowc_action_apply_result(
					session->windows[window],
					call->payload.window_transaction.action_id,
					call->payload.window_transaction.accepted);
			if(call->payload.window_transaction.accepted && session->windows[window]
			   && window != WINDOWC_INVENTORY) {
				struct inventory* inv = windowc_get_latest(session->windows[window]);
				for(size_t slot = 0; slot < inv->capacity; slot++)
					clin_mirror_player_section(session, window, slot, inv->items[slot]);
			}
			if(!call->payload.window_transaction.accepted && session->windows[window]
			   && session->windows[window]->type != WINDOW_TYPE_SIGN) {
				/* [740c583 NetClientHandler.handleTransaction]. Retry on a full
				 * local transport so the server's gate can never strand the UI. */
				struct window_container* wc = session->windows[window];
				wc->correction_pending = true;
				wc->transaction_reply_pending = true;
				wc->transaction_reply_action = call->payload.window_transaction.action_id;
				client_session_flush_window_transactions(session);
			}
			break;
		}
		case CRPC_WINDOW_PROGRESS: {
			uint8_t window = call->payload.window_progress.window;
			if(session->windows[window]) {
				session->windows[window]->furnace_cook_time
					= call->payload.window_progress.cook_time;
				session->windows[window]->furnace_burn_time
					= call->payload.window_progress.burn_time;
				session->windows[window]->furnace_current_item_burn
					= call->payload.window_progress.current_item_burn_time;
			}
			break;
		}
		default: return false;
	}
	return true;
}

bool client_session_recipe_craft_request(struct client_session* session,
	uint8_t player, uint8_t window, uint16_t recipe_index,
	container_send_fn send, void* user) {
	if(!session || !send || player >= CLIENT_LOCAL_PLAYERS || session->craft_pending
	   || session->local_reply_failed || !session->windows[window]
	   || (session->visible_window != window
		   && !(window == WINDOWC_INVENTORY && session->visible_window < 0)))
		return false;
	struct window_container* wc = session->windows[window];
	if((wc->type != WINDOW_TYPE_WORKBENCH && wc->type != WINDOW_TYPE_INVENTORY)
	   || windowc_actions_pending(wc) || wc->quick_pending || wc->correction_pending
	   || wc->transaction_reply_pending
	   || inventory_get_picked_item(windowc_get_latest(wc), NULL)) return false;
	size_t grid = wc->type == WINDOW_TYPE_WORKBENCH ? 9 : 4;
	for(size_t k = 0; k < grid; k++)
		if(inventory_get_slot(windowc_get_latest(wc), 1 + k, NULL)) return false;

	if(++recipe_craft_ticket == 0) ++recipe_craft_ticket;
	session->craft_pending = true;
	session->craft_pending_ticket = recipe_craft_ticket;
	session->craft_pending_player = player;
	session->craft_pending_window = window;
	/* Liveness bookkeeping: the window identity and the deadline this request is
	 * allowed to outlive (see client_session_recipe_craft_tick). Taken here
	 * rather than passed in, so the guide's call site is unchanged. */
	session->craft_pending_generation = wc->generation;
	session->craft_deadline = time_add_ms(time_get(), CLIENT_RECIPE_CRAFT_TIMEOUT_MS);
	struct server_rpc request = {.type = SRPC_RECIPE_CRAFT,
		.payload.recipe_craft = {.ticket = recipe_craft_ticket,
			.recipe_index = recipe_index, .player = player, .window = window}};
	if(send(user, &request)) return true;
	session->craft_pending = false;
	return false;
}

void client_session_recipe_craft_tick(struct client_session* session, ptime_t now) {
	if(!session || !session->craft_pending) return;
	struct window_container* wc = session->windows[session->craft_pending_window];
	/* Same "is this still the window I asked about" predicate the request
	 * itself used, plus the container IDENTITY: a close/reopen hands back the
	 * same routing index with a fresh generation, and the reply for the old one
	 * can never arrive. client_session_clear_windows already covers the
	 * provider-generation change (bind/unbind route through it). */
	bool unanswerable = !wc || wc->generation != session->craft_pending_generation
		|| (session->visible_window != (int)session->craft_pending_window
			&& !(session->craft_pending_window == WINDOWC_INVENTORY
				 && session->visible_window < 0));
	if(!unanswerable && !time_reached(now, session->craft_deadline)) return;
	session->craft_pending = false;
	session->craft_reason = RECIPE_CRAFT_TIMED_OUT;
	session->craft_reply_serial++;
}


static uint64_t client_guest_ticket;

bool client_session_request_local_join(struct client_session* session) {
	/* Astra review A, item 3: an admission retirement whose Leave the server
	 * refused (checkpoint failure) stays FAILED and bound, the server keeping
	 * ownership. The join card's Retry reaches here: re-send that same
	 * authoritative Leave; once LEFT settles, a new Join is possible. */
	if(session && session->local_bound && session->local_admit.retiring
	   && session->local_phase == CLIENT_GUEST_FAILED)
		return client_session_request_local_leave(session);
	if(!session || session->local_bound || session->local_player
	   || (session->local_phase != CLIENT_GUEST_IDLE
		   && session->local_phase != CLIENT_GUEST_FAILED)
	   || client_guest_ticket == UINT64_MAX) return false;
	session->local_leave_receipt = (struct client_local_leave_token){0};
	session->local_leave_receipt_outcome = CLIENT_LOCAL_LEAVE_STALE;
	session->local_leave_receipt_active = false;
	session->local_leave_receipt_manual_retry = false;
	session->local_ticket = ++client_guest_ticket;
	session->local_join_failed = false; /* R2: a re-Join starts clean */
	session->local_phase = CLIENT_GUEST_REQUESTED;
	session->local_pending_action = LOCAL_GUEST_JOIN;
	session->local_pending = true;
	return true;
}

/* Ask the server to admit this session's guest BODY. Only from an ATTACHED
 * (window-owning) session, and only when the caller's experimental switch is
 * on -- exposing this is main.c's decision, not this file's. Retryable: the
 * server denies while terrain is still streaming and the caller re-asks. */
bool client_session_request_local_gameplay(struct client_session* session) {
	if(!session || !session->local_bound || !session->local_ticket
	   || session->local_phase != CLIENT_GUEST_ATTACHED
	   || session->local_gameplay_ready || session->local_respawn_loading
	   || session->local_reply_failed || session->local_join_failed
	   || session->local_pending)
		return false;
	session->local_pending_action = LOCAL_GUEST_GAMEPLAY;
	session->local_pending = true;
	return true;
}

bool client_session_request_local_leave(struct client_session* session) {
	if(!session || !session->local_ticket || session->local_phase == CLIENT_GUEST_IDLE)
		return false;
	uint64_t generation = session->local_bound ? session->local_generation : 0;
	bool observed = session->local_leave_receipt_active
		&& session->local_leave_receipt.ticket == session->local_ticket
		&& session->local_leave_receipt.generation == generation;
	if(observed && session->local_phase == CLIENT_GUEST_LEAVING
	   && session->local_leave_receipt_outcome == CLIENT_LOCAL_LEAVE_PENDING)
		return true;
	session->local_pending_action = session->local_bound ? LOCAL_GUEST_LEAVE : LOCAL_GUEST_CANCEL;
	session->local_phase = CLIENT_GUEST_LEAVING;
	/* An ACK that has not entered the reliable FIFO must yield to retirement. */
	session->local_respawn_ack_send_pending = false;
	session->local_pending = true;
	/* Match the menu autopilot's 20 s WAIT_QUIT budget: a host checkpoint on
	 * the SD card can legitimately take seconds, so a short UI timeout would
	 * race a healthy guest checkpoint. */
	session->local_leave_deadline = time_add_ms(time_get(),
		LOCAL_GUEST_LEAVE_ACK_DEADLINE_MS);
	session->local_leave_deadline_armed = true;
	session->local_leave_retry_handed_off = false;
	if(observed)
		session->local_leave_receipt_outcome=CLIENT_LOCAL_LEAVE_PENDING;
	return true;
}

static bool local_leave_token_equal(const struct client_local_leave_token* a,
	const struct client_local_leave_token* b) {
	return a && b && a->ticket == b->ticket
		&& a->generation == b->generation;
}

bool client_session_local_leave_begin(struct client_session* session,
	struct client_local_leave_token* token) {
	if(!session || !token || !session->local_ticket
	   || session->local_phase == CLIENT_GUEST_IDLE) return false;
	struct client_local_leave_token next = {
		.ticket = session->local_ticket,
		.generation = session->local_bound ? session->local_generation : 0,
	};
	if(session->local_leave_receipt_active
	   && !local_leave_token_equal(&session->local_leave_receipt, &next)) return false;
	if(session->local_leave_receipt_active) {
		session->local_leave_receipt_manual_retry = true;
		session->local_leave_retry_handed_off = false;
		*token = next;
		return true;
	}
	bool already_leaving = session->local_phase == CLIENT_GUEST_LEAVING;
	if(!already_leaving && !client_session_request_local_leave(session))
		return false;
	session->local_leave_receipt = next;
	session->local_leave_receipt_outcome = already_leaving
		&& !session->local_leave_deadline_armed && !session->local_pending
		? CLIENT_LOCAL_LEAVE_TIMED_OUT : CLIENT_LOCAL_LEAVE_PENDING;
	session->local_leave_receipt_active = true;
	session->local_leave_receipt_manual_retry = true;
	session->local_leave_retry_handed_off = false;
	*token = next;
	return true;
}

bool client_session_local_leave_retry(struct client_session* session,
	const struct client_local_leave_token* token) {
	if(!session || !token || !session->local_leave_receipt_active
	   || !session->local_leave_receipt_manual_retry
	   || !local_leave_token_equal(&session->local_leave_receipt, token)
	   || (session->local_leave_receipt_outcome != CLIENT_LOCAL_LEAVE_TIMED_OUT
		   && session->local_leave_receipt_outcome != CLIENT_LOCAL_LEAVE_FAILED)
	   || (session->local_phase != CLIENT_GUEST_LEAVING
		   && session->local_phase != CLIENT_GUEST_FAILED)
	   || !client_session_request_local_leave(session)) return false;
	session->local_leave_receipt_outcome = CLIENT_LOCAL_LEAVE_PENDING;
	session->local_leave_receipt_manual_retry = true;
	return true;
}

enum client_local_leave_outcome client_session_local_leave_observe(
	const struct client_session* session,
	const struct client_local_leave_token* token) {
	if(!session || !token || !session->local_leave_receipt_active
	   || !local_leave_token_equal(&session->local_leave_receipt, token))
		return CLIENT_LOCAL_LEAVE_STALE;
	if(session->local_ticket && session->local_ticket != token->ticket)
		return CLIENT_LOCAL_LEAVE_STALE;
	if(session->local_bound && session->local_generation != token->generation)
		return CLIENT_LOCAL_LEAVE_STALE;
	return session->local_leave_receipt_outcome;
}

bool client_session_local_leave_release(struct client_session* session,
	const struct client_local_leave_token* token) {
	if(!session || !token || !session->local_leave_receipt_active
	   || !session->local_leave_receipt_manual_retry
	   || !local_leave_token_equal(&session->local_leave_receipt, token))
		return false;
	session->local_leave_receipt_manual_retry = false;
	if(session->local_leave_receipt_outcome == CLIENT_LOCAL_LEAVE_TIMED_OUT
	   && session->local_phase == CLIENT_GUEST_LEAVING) {
		session->local_leave_deadline_armed = false;
		session->local_pending = false;
		session->local_leave_retry_handed_off = true;
		if(session->pause.open && !session->pause.host) {
			session->pause.exit_pending = false;
			session->pause.exit_failed = true;
			session->pause.selected = 3;
		}
	}
	return true;
}

void client_session_local_leave_tick(struct client_session* session,ptime_t now) {
	if(!session || session->local_phase != CLIENT_GUEST_LEAVING) return;
	bool retry_ui = session->pause.open && !session->pause.host;
	if(!session->local_leave_deadline_armed) {
		if(session->local_leave_receipt_manual_retry) return;
		/* An expiry handed the wait to an explicit Retry owner, or that owner
		 * released it here during epoch retirement. If no Retry UI remains,
		 * take the transport back and open one fresh acknowledgement window.
		 * A server-declared LEAVE_FAILED never sets this hand-off flag. */
		if(!retry_ui && session->local_leave_retry_handed_off) {
			session->local_leave_retry_handed_off=false;
			session->local_pending=true;
			session->local_leave_deadline=time_add_ms(now,
				LOCAL_GUEST_LEAVE_ACK_DEADLINE_MS);
			session->local_leave_deadline_armed=true;
			if(session->local_leave_receipt_active)
				session->local_leave_receipt_outcome
					=CLIENT_LOCAL_LEAVE_PENDING;
		}
		return;
	}
	if(!time_reached(now,session->local_leave_deadline)) return;
	/* Keep LEAVING, ticket, generation and claim intact: a late matching LEFT
	 * remains authoritative and still performs the one real teardown. */
	if(session->local_leave_receipt_manual_retry) {
		session->local_leave_deadline_armed=false;
		session->local_pending=false;
		if(session->local_leave_receipt_active)
			session->local_leave_receipt_outcome
				=CLIENT_LOCAL_LEAVE_TIMED_OUT;
		return;
	}
	if(retry_ui) {
		session->local_leave_deadline_armed=false;
		session->local_leave_retry_handed_off=true;
		session->local_pending=false;
		if(session->local_leave_receipt_active)
			session->local_leave_receipt_outcome
				=CLIENT_LOCAL_LEAVE_TIMED_OUT;
		session->pause.exit_pending=false;
		session->pause.exit_failed=true;
		session->pause.selected=3;
	} else {
		/* There is no guest Retry control to take over. Keep the transport retry
		 * alive and bound the next attempt by the same acknowledgement window. */
		session->local_pending=true;
		session->local_leave_deadline=time_add_ms(now,
			LOCAL_GUEST_LEAVE_ACK_DEADLINE_MS);
		session->local_leave_deadline_armed=true;
		if(session->local_leave_receipt_active)
			session->local_leave_receipt_outcome
				=CLIENT_LOCAL_LEAVE_PENDING;
	}
}

void client_session_flush_local(struct client_session* session) {
	if(!session) return;
	client_session_local_leave_tick(session,time_get());
	client_session_hud_tick(session, time_get());
	if(session->local_admit.active
	   && session->local_phase == CLIENT_GUEST_ATTACHED
	   && time_reached(time_get(), session->local_admit.deadline))
		admit_retire(session, "Player 2 could not join: timed out");
	if(session->local_phase == CLIENT_GUEST_ATTACHED && session->local_reply_failed)
		session->local_phase = CLIENT_GUEST_FAILED;
	if(!session->local_ticket) return;
	if(!session->local_pending) {
		if(!session->local_respawn_ack_send_pending
		   || session->local_phase != CLIENT_GUEST_ATTACHED) return;
		struct server_rpc_source source = {.slot=1,
			.generation=session->local_generation};
		struct server_rpc ready = {.type=SRPC_LOCAL_GUEST,
			.payload.local_guest={.ticket=session->local_ticket,
				.action=LOCAL_GUEST_RESPAWN_READY, .owner=1,
				.generation=session->local_generation,
				.respawn_epoch=session->local_respawn_epoch}};
		if(svin_rpc_send_from(source, &ready)) {
			session->local_respawn_ack_send_pending = false;
			session->local_respawn_awaiting_commit = true;
		}
		return;
	}
	enum local_guest_action action = (enum local_guest_action)session->local_pending_action;
	struct server_rpc_source source = {0};
	if(action != LOCAL_GUEST_JOIN && action != LOCAL_GUEST_CANCEL)
		source = (struct server_rpc_source){.slot=1,.generation=session->local_generation};
	struct server_rpc call = {.type=SRPC_LOCAL_GUEST,
		.payload.local_guest={.ticket=session->local_ticket,.action=action}};
	/* Retry on a later frame, never block the thread draining server replies.
	 * O11 / S03 #2: an armed vehicle smoke records the request's receipt at
	 * THIS accepted send, with an order reserved before the server can see
	 * it; a refused send records nothing (inert when unarmed). */
	const uint32_t vs_order = vsp_dispatch_reserve();
	if(svin_rpc_send_from(source,&call)) {
		session->local_pending=false;
		vsp_dispatch_sent(vsp_ui_of_guest_action((int)action), vs_order);
	}
}

static bool client_respawn_get_block(void* ctx,w_coord_t x,w_coord_t y,
	w_coord_t z,struct block_data* out) {
	struct world* world=ctx;
	if(!world||!out||!world_find_chunk(world,x,y,z))return false;
	*out=world_get_block(world,x,y,z);return true;
}
static bool client_respawn_get_moving(void* ctx,w_coord_t x,w_coord_t y,
	w_coord_t z,struct piston_moving_state* out) {
	(void)ctx;return client_piston_moving_query(x,y,z,out);
}

static bool respawn_target_ready(struct world* world,double x,double y,double z,
	bool world_loaded,bool world_failed,bool terrain_hold) {
	if(!world||!world_loaded||world_failed||terrain_hold)return false;
	int missing = 0, mesh_pending = 0;
	int radius = world_server_view_radius();
	if(radius <= 0) radius = MAX_VIEW_DISTANCE;
	world_near_ring_status(world,
		WCOORD_CHUNK_OFFSET((w_coord_t)floor(x)),
		WCOORD_CHUNK_OFFSET((w_coord_t)floor(z)),
		radius, &missing, &mesh_pending);
	struct AABB body;
	if(missing||mesh_pending||!respawn_clearance_body(x,y,z,&body))return false;
	return respawn_clearance_blocks_clear(world,client_respawn_get_block,
		client_respawn_get_moving,&body);
}

bool client_session_local_respawn_readiness(struct client_session* session,
	struct world* world, bool world_loaded, bool world_failed, bool terrain_hold) {
	if(!session || !session->local_respawn_loading
	   || session->local_phase != CLIENT_GUEST_ATTACHED
	   || session->local_respawn_correction_installed
	   || session->local_respawn_ack_send_pending
	   || session->local_respawn_awaiting_commit
	   ||!respawn_target_ready(world,session->local_respawn_x,
		 session->local_respawn_y,session->local_respawn_z,world_loaded,
		 world_failed,terrain_hold)) return false;
	session->local_respawn_ack_send_pending = true;
	return true;
}

bool client_session_host_respawn_readiness(struct client_session* s,
	struct world* world,bool loaded,bool failed,bool hold) {
	if(!s||!s->host_respawn_loading||s->host_respawn_correction_installed
	   ||s->host_respawn_ack_send_pending||s->host_respawn_awaiting_commit
	   ||!respawn_target_ready(world,s->host_respawn_x,s->host_respawn_y,
		 s->host_respawn_z,loaded,failed,hold))return false;
	s->host_respawn_ack_send_pending=true;return true;
}

void client_session_flush_host_respawn(struct client_session* s) {
	if(!s||!s->host_respawn_ack_send_pending||!s->host_respawn_epoch)return;
	struct server_rpc ack={.type=SRPC_RESPAWN,.payload.respawn={.owner=0,
		.generation=0,.respawn_epoch=s->host_respawn_epoch}};
	if(svin_rpc_send(&ack)){s->host_respawn_ack_send_pending=false;
		s->host_respawn_awaiting_commit=true;}
}

bool client_session_host_respawn_hidden_camera(const struct client_session* s,
	struct camera* camera) {
	if(!s||!camera||!s->host_respawn_loading)return false;
	*camera=s->camera;camera->x=(float)s->host_respawn_x;
	camera->y=(float)s->host_respawn_y;camera->z=(float)s->host_respawn_z;
	return true;
}

bool client_session_local_respawn_hidden_camera(const struct client_session* session,
	struct camera* camera) {
	if(!session || !camera || !session->local_respawn_loading
	   || session->local_gameplay_ready || session->local_phase != CLIENT_GUEST_ATTACHED)
		return false;
	*camera = session->camera;
	camera->x = (float)session->local_respawn_x;
	camera->y = (float)session->local_respawn_y;
	camera->z = (float)session->local_respawn_z;
	return true;
}

struct camera* client_session_local_render_camera(struct client_session* session) {
	if(!session)return NULL;
	if(session->host_respawn_render_camera_frozen)
		return &session->host_respawn_render_camera;
	return session->local_respawn_render_camera_frozen
		? &session->local_respawn_render_camera : &session->camera;
}

bool client_session_host_gameplay_active(const struct client_session* s) {
	return s&&s->local_player&&s->local_player->health>0
		&&!s->host_respawn_loading&&!s->death.active;
}
bool client_session_local_gameplay_active(const struct client_session* s) {
	return s&&s->local_bound&&s->local_generation
		&&s->local_phase==CLIENT_GUEST_ATTACHED&&s->local_player
		&&s->local_player->health>0&&s->local_gameplay_ready
		&&!s->local_respawn_loading&&!s->local_reply_failed
		&&!s->local_death_leave_manual&&!s->death.active;
}

void client_session_host_respawn_world_reset(struct client_session* s) {
	if(!s)return;
	if(s->host_respawn_epoch>s->host_respawn_completed_epoch)
		s->host_respawn_completed_epoch=s->host_respawn_epoch;
	s->host_respawn_loading=s->host_respawn_correction_installed=false;
	s->host_respawn_ack_send_pending=s->host_respawn_awaiting_commit=false;
	s->host_respawn_epoch=0;s->host_respawn_render_camera_frozen=false;
	s->respawn_request_observation_pending=false;
	s->respawn_failure=RESPAWN_FAILURE_NONE;s->respawn_failure_epoch=0;
}
void client_session_host_respawn_transport_reset(struct client_session* s) {
	if(!s)return;
	client_session_host_respawn_world_reset(s);
	s->host_respawn_completed_epoch=0;
}

bool client_session_try_local_leave(struct client_session* s) {
	if(!s||!s->local_ticket||!s->local_generation)return false;
	bool observed=s->local_leave_receipt_active
		&&s->local_leave_receipt.ticket==s->local_ticket
		&&s->local_leave_receipt.generation==s->local_generation;
	bool observed_retry=observed
		&&((s->local_phase==CLIENT_GUEST_FAILED
			&&s->local_leave_receipt_outcome==CLIENT_LOCAL_LEAVE_FAILED)
		   ||(s->local_phase==CLIENT_GUEST_LEAVING
			&&s->local_leave_receipt_outcome==CLIENT_LOCAL_LEAVE_TIMED_OUT));
	if((s->local_phase!=CLIENT_GUEST_ATTACHED
	      &&s->local_phase!=CLIENT_GUEST_FAILED)&&!observed_retry)return false;
	struct server_rpc_source source={.slot=1,.generation=s->local_generation};
	struct server_rpc call={.type=SRPC_LOCAL_GUEST,.payload.local_guest={
		.ticket=s->local_ticket,.action=LOCAL_GUEST_LEAVE}};
	if(!svin_rpc_send_from(source,&call))return false;
	s->local_phase=CLIENT_GUEST_LEAVING;s->local_pending=false;
	s->local_death_leave_manual=true;s->local_respawn_ack_send_pending=false;
	if(observed) {
		s->local_leave_receipt_outcome=CLIENT_LOCAL_LEAVE_PENDING;
		s->local_leave_deadline=time_add_ms(time_get(),
			LOCAL_GUEST_LEAVE_ACK_DEADLINE_MS);
		s->local_leave_deadline_armed=true;
		s->local_leave_retry_handed_off=false;
	}
	return true;
}

void client_session_local_status(struct client_session* session,
	const struct client_rpc* call) {
	if(!session || !call || call->type != CRPC_LOCAL_GUEST
	   || !session->local_ticket || call->payload.local_guest.ticket != session->local_ticket
	   || session->local_phase == CLIENT_GUEST_IDLE) return;
	uint64_t ticket = session->local_ticket;
	uint64_t generation = call->payload.local_guest.generation;
	int result = call->payload.local_guest.result;
	if(result == LOCAL_GUEST_OFFER) {
		if(session->local_phase != CLIENT_GUEST_REQUESTED) return;
		bool bound = client_session_bind_local(session,generation);
		session->local_ticket=ticket; /* bind initializes the new generation */
		session->local_phase = bound ? CLIENT_GUEST_INSTALLING : CLIENT_GUEST_LEAVING;
		session->local_pending_action = bound ? LOCAL_GUEST_ACCEPT : LOCAL_GUEST_CANCEL;
		session->local_pending=true;
		return;
	}
	bool receipt_left = result == LOCAL_GUEST_LEFT
		&& session->local_leave_receipt_active
		&& session->local_leave_receipt.ticket == ticket
		&& (session->local_phase == CLIENT_GUEST_LEAVING
			|| session->local_phase == CLIENT_GUEST_FAILED)
		&& (session->local_leave_receipt.generation == 0
			? !session->local_bound
			: session->local_bound
				&& session->local_leave_receipt.generation == generation
				&& session->local_generation == generation);
	bool ordinary_left = result == LOCAL_GUEST_LEFT
		&& session->local_phase == CLIENT_GUEST_LEAVING
		&& (!session->local_bound || generation == session->local_generation);
	if(receipt_left || ordinary_left) {
		struct client_local_leave_token receipt
			= session->local_leave_receipt;
		if(session->local_death_leave_manual)
			(void)screen_death_owner_exit_settled(session,true);
        if(session->pause.open && !session->pause.host && session->pause.exit_pending)
            audio_menu_event(MENU_SOUND_BACK,time_get());
		client_session_clear_windows(session);
		if(receipt_left) {
			session->local_leave_receipt = receipt;
			session->local_leave_receipt_outcome = CLIENT_LOCAL_LEAVE_LEFT;
			session->local_leave_receipt_active = true;
		}
		/* The claim is freed here on every leave, and only for a MATCHING
		 * successful leave ACK (the generation test above) -- never on a
		 * rejection or another guest's reply. What this comment used to claim,
		 * that the ACK is the ONLY thing that frees it, is false (R-A9 A9-L8):
		 * main.c's refuse path settles the join card with
		 * input_split_join_settle(false) while the session is still LEAVING,
		 * which unbinds one or more frames earlier. That is harmless and
		 * deliberate -- input_player_unbind is idempotent (the second call sees
		 * INPUT_DEVICE_NONE, skips the held-A de-arm and only re-zeroes the
		 * owner) and the de-arm has already run on the still-bound device -- so
		 * this call is the one that GUARANTEES the release on every ordinary
		 * leave, not the one that has sole authority over it. */
		input_player_unbind(1);
		return;
	}
	if(result == LOCAL_GUEST_REJECTED) {
		if(session->local_phase == CLIENT_GUEST_LEAVING) return; /* cancel ACK follows */
		if(session->local_bound) {
			(void)client_session_request_local_leave(session);
		} else {
			session->local_phase=CLIENT_GUEST_FAILED;
			session->local_pending=false;
		}
		return;
	}
	if(!session->local_bound || generation != session->local_generation) return;
	/* residency-1005/6 R2: the server's BOUNDED Join terrain wait ended this
	 * generation unadmitted (the saved column never published). Visible,
	 * retryable failure: the reason goes on the host HUD and settles the join
	 * card as refused (client_session_local_admit_failure_present); main.c
	 * then releases the session through the authoritative Leave, and the
	 * card's Retry is a fresh Join that restores the saved record again.
	 * Only the current generation's first failure counts; a duplicate, or one
	 * for a body that was admitted, is inert (a foreign generation never got
	 * here). */
	if(result==LOCAL_GUEST_JOIN_LOADING_FAILED) {
		if(session->local_phase==CLIENT_GUEST_ATTACHED
		   &&!session->local_gameplay_ready&&!session->local_join_failed) {
			session->local_join_failed=true;
			if(session->local_pending
			   &&session->local_pending_action==LOCAL_GUEST_GAMEPLAY)
				session->local_pending=false;
			admit_notice(session,"Player 2 could not join: no terrain");
		}
		return;
	}
	uint64_t epoch = call->payload.local_guest.respawn_epoch;
	if(epoch && call->payload.local_guest.owner != 1) return;
	/* residency-1005/5: the server's BOUNDED respawn LOADING terminated this
	 * epoch without committing (its arrival terrain never loaded). The plan
	 * is gone server-side, so the loading screen ends and the death screen
	 * offers "Retry Respawn"; nothing of the uncommitted plan survives (no
	 * correction was installed -- the server only corrects after commit). */
	if(result==LOCAL_GUEST_RESPAWN_LOADING_FAILED&&epoch
	   &&session->local_respawn_loading
	   &&epoch==session->local_respawn_epoch
	   &&!session->local_respawn_correction_installed
	   &&session->local_player&&session->local_player->health<=0) {
		session->local_respawn_loading=false;
		session->local_respawn_ack_send_pending=false;
		session->local_respawn_awaiting_commit=false;
		session->local_respawn_epoch=0;
		session->local_respawn_render_camera_frozen=false;
		/* back to the pre-LOADING death-screen state: the dead body stays
		 * installed (LOADING cleared this; without it the status handler
		 * would release the corpse's client body) */
		session->local_gameplay_ready=true;
		session->respawn_request_observation_pending=false;
		session->respawn_failure=RESPAWN_FAILURE_RETRY;
		session->respawn_failure_epoch=epoch;
		session->respawn_failure_serial++;
		return;
	}
	if(result==LOCAL_GUEST_RESPAWN_LOADING_FAILED)return; /* stale/foreign */
	if(result==LOCAL_GUEST_RESPAWN_RETRY
	   ||result==LOCAL_GUEST_RESPAWN_TERMINAL) {
		if(!session->respawn_request_observation_pending
		   ||call->payload.local_guest.owner!=1
		   ||!session->local_player
		   ||session->local_player->health>0
		   ||session->local_respawn_completed_epoch
		      >session->death.completed_floor
		   ||(epoch&&(epoch<=session->death.completed_floor
		      ||(session->local_respawn_epoch
		         &&epoch!=session->local_respawn_epoch))))return;
		session->respawn_request_observation_pending=false;
		session->respawn_failure=result==LOCAL_GUEST_RESPAWN_RETRY
			?RESPAWN_FAILURE_RETRY:RESPAWN_FAILURE_TERMINAL;
		session->respawn_failure_epoch=epoch;
		session->respawn_failure_serial++;
		return;
	}
	if(result == LOCAL_GUEST_RESPAWN_LOADING) {
		if(session->local_phase != CLIENT_GUEST_ATTACHED || !session->local_player
		   || session->local_reply_failed || !epoch
		   || epoch <= session->local_respawn_completed_epoch
		   || (session->local_respawn_loading
			&& epoch < session->local_respawn_epoch)) return;
		if(session->local_respawn_loading && epoch == session->local_respawn_epoch)
			return; /* preserve correction and reliable-send progress */
		if(!session->local_respawn_render_camera_frozen) {
			session->local_respawn_render_camera=session->camera;
			session->local_respawn_render_camera_frozen=true;
		}
		session->respawn_request_observation_pending=false;
		session->respawn_failure=RESPAWN_FAILURE_NONE;
		session->local_respawn_loading = true;
		session->local_respawn_epoch = epoch;
		session->local_respawn_x = call->payload.local_guest.x;
		session->local_respawn_y = call->payload.local_guest.y;
		session->local_respawn_z = call->payload.local_guest.z;
		session->local_respawn_world_slot
			= call->payload.local_guest.respawn_world_slot;
		session->local_respawn_world_generation
			= call->payload.local_guest.respawn_world_generation;
		session->local_respawn_dimension
			= call->payload.local_guest.respawn_dimension;
		session->local_respawn_correction_installed = false;
		session->local_respawn_ack_send_pending = false;
		session->local_respawn_awaiting_commit = false;
		session->local_gameplay_ready = false;
		return;
	}
	if(result == LOCAL_GUEST_INSTALLED && session->local_phase == CLIENT_GUEST_INSTALLING) {
		if(session->local_reply_failed) {
			(void)client_session_request_local_leave(session);
		} else {
			session->local_phase=CLIENT_GUEST_CONFIRMING;
			session->local_pending_action=LOCAL_GUEST_CONFIRM;
			session->local_pending=true;
		}
	} else if(result == LOCAL_GUEST_ATTACHED && session->local_phase == CLIENT_GUEST_CONFIRMING) {
		session->local_phase = session->local_reply_failed ? CLIENT_GUEST_FAILED : CLIENT_GUEST_ATTACHED;
	} else if(result == LOCAL_GUEST_GAMEPLAY_READY) {
		/* The ONLY production setter of local_gameplay_ready (the two-view
		 * loop's gate). Everything it certifies must already be true HERE:
		 * a matching generation on a bound, ATTACHED session whose replies
		 * have not failed, and a body the caller installed in the shared
		 * entity store before handing this reply over. A missing body is not
		 * an error -- the request is simply not granted and may be retried. */
		if(epoch) {
			if(session->local_phase == CLIENT_GUEST_ATTACHED && session->local_player
			   && session->local_respawn_loading
			   && epoch == session->local_respawn_epoch
			   && session->local_respawn_correction_installed
			   && !session->local_reply_failed) {
				session->local_respawn_completed_epoch = epoch;
				session->local_respawn_loading = false;
				session->local_respawn_ack_send_pending = false;
				session->local_respawn_awaiting_commit = false;
				session->local_gameplay_ready = true;
				session->local_respawn_render_camera_frozen = false;
				session->respawn_failure=RESPAWN_FAILURE_NONE;
			}
		} else if(session->local_phase == CLIENT_GUEST_ATTACHED && session->local_player
		   && !session->local_reply_failed) {
			/* admit-retry-0925: retained replies land before gameplay opens. */
			admit_replay(session);
			session->local_gameplay_ready = true;
		}
	} else if(result == LOCAL_GUEST_GAMEPLAY_DENIED) {
		session->local_gameplay_ready = false;
	} else if(result == LOCAL_GUEST_CORRECTION) {
		/* The server rejected a movement sample and re-authored this body's
		 * position. Install it through the SAME routine the primary player's
		 * corrections use, so the body, its interpolation history and the
		 * camera move together -- and adopt the new token, which is what makes
		 * every already-in-flight sample stale. Ignored unless this session
		 * actually holds an admitted body. */
		if(((epoch && session->local_respawn_loading
			&& epoch == session->local_respawn_epoch)
		   || (!epoch && session->local_gameplay_ready))
		   && session->local_phase == CLIENT_GUEST_ATTACHED
		   && session->local_player && !session->local_reply_failed) {
			struct client_rpc correction = {.type = CRPC_PLAYER_POS,
				.payload.player_pos = {
					.position = {(float)call->payload.local_guest.x,
						(float)call->payload.local_guest.y,
						(float)call->payload.local_guest.z},
					.rotation = {call->payload.local_guest.rx,
						call->payload.local_guest.ry},
					.correction_seq
					= call->payload.local_guest.correction_seq,
					/* Position only. Rotation is never validated or
					 * clamped server-side, so snapping the aim would be a
					 * gratuitous camera yank; the payload carries the
					 * server record of it for diagnostics, not to apply. */
					.correction_flags
						= call->payload.local_guest.correction_flags,
				}};
			if(clin_apply_player_position(session, &correction) && epoch) {
				session->local_respawn_correction_installed = true;
				if(session->local_respawn_render_camera_frozen)
					session->camera=session->local_respawn_render_camera;
			}
		}
	} else if(result == LOCAL_GUEST_LEAVE_FAILED && session->local_phase == CLIENT_GUEST_LEAVING) {
		session->local_leave_deadline_armed=false;
		session->local_leave_retry_handed_off=false;
		bool observed = session->local_leave_receipt_active
			&& session->local_leave_receipt.ticket == ticket
			&& session->local_leave_receipt.generation == generation;
		if(observed) {
			session->local_leave_receipt_outcome=CLIENT_LOCAL_LEAVE_FAILED;
			session->local_phase=CLIENT_GUEST_FAILED;
			session->local_pending=false;
			if(session->local_death_leave_manual)
				(void)screen_death_owner_exit_settled(session,false);
		} else if(session->local_death_leave_manual) {
			session->local_phase=CLIENT_GUEST_FAILED;
			session->local_pending=false;
			(void)screen_death_owner_exit_settled(session,false);
		} else if(session->local_respawn_loading) {
			/* A respawn-loading body cannot return to gameplay: retain its
			 * ownership tuple and retry the same owned Leave. */
			session->local_pending_action=LOCAL_GUEST_LEAVE;
			session->local_pending=true;
		} else {
			/* Preserve the ordinary Leave contract: surface the failure to
			 * Pause/Join UI and wait for an explicit retry. */
			session->local_phase=CLIENT_GUEST_FAILED;
			session->local_pending=false;
		}
	}
}
