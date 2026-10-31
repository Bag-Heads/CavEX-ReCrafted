/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../item/inventory_click.h"
#include "../../item/items.h"
#include "../../network/server_interface.h"
#include "../../platform/input.h"
#include "../../platform/menu_sound.h"
#include "../../platform/texture.h"
#include "../../platform/input_owner.h"
#include "../../platform/input_glyph.h"
#include "../../platform/gfx.h"
#include "../game_state.h"
#include "container_menu.h"
#include "screen.h"

/* Commit-timed transfer cues (docs/ui_design/approved/specs/sound-events.md:
 * "Item cue once after a successful pickup, placement, split-one or quick
 * move; no second confirmation for the same transfer", "Empty slots, blocked
 * moves ... are silent", "Keep the approved crafting cues and recipes
 * unchanged", and the runtime rule "Actual transfer/save success hooks must
 * come from authoritative completed operations").
 *
 * windowc_action_apply_result promotes an ACCEPTED action's own snapshot to
 * the head of the revision list and frees its predecessors, so the head node's
 * action id IS the last authoritatively committed action -- and, because the
 * predecessors are freed with it, a HIGH-WATER MARK: everything issued at or
 * before that id has committed. The non-commit outcomes are distinguishable
 * without a second source of truth: a rejection bumps wc->rejection_serial
 * (and leaves the head where it was), while a handoff/correction discard, a
 * server-authoritative slot write and a window re-create all bump
 * wc->generation (window_container.c windowc_create/_slot_change/
 * _discard_pending_actions). An arm therefore records BOTH, and only an
 * unchanged serial AND an unchanged generation AND a head id that has reached
 * the armed action is a commit.
 *
 * Arms live in a small FIFO ring rather than a single slot: ordinary clicks
 * are not gated on the previous reply (only quick moves are, via
 * wc->quick_pending), so two rapid clicks used to overwrite one arm and lose
 * the first accept's cue entirely. Ring entries are armed in issue order and
 * the server replies in that same order, so resolution is a plain FIFO drain.
 */
static void container_menu_cue_pop(struct container_menu* menu) {
	menu->item_cue_head
		= (uint8_t)((menu->item_cue_head + 1) % CONTAINER_MENU_CUE_PENDING);
	menu->item_cue_count--;
}

static void container_menu_cue_arm(struct container_menu* menu,
								   const struct window_container* wc,
								   uint16_t action, enum menu_sound_event cue) {
	if(menu->item_cue_count >= CONTAINER_MENU_CUE_PENDING) {
		/* Deeper pipelining than a player can produce inside the dispatcher's
		 * own 90 ms item guard. Retire the oldest arm and COUNT it -- the cue
		 * is dropped on the record, never silently. */
		if(menu->item_cue_dropped != UINT16_MAX) menu->item_cue_dropped++;
		container_menu_cue_pop(menu);
	}
	size_t at = (menu->item_cue_head + menu->item_cue_count)
		% CONTAINER_MENU_CUE_PENDING;
	menu->item_cue[at] = (struct container_menu_cue) {
		.serial = wc->rejection_serial, .generation = wc->generation,
		.action = action, .cue = (uint8_t)cue,
	};
	menu->item_cue_count++;
}

static void container_menu_item_cue(struct container_menu* menu,
									struct window_container* wc) {
	if(!menu->item_cue_count)
		return;
	if(!wc) {
		menu->item_cue_count = 0;
		return;
	}
	/* critique-1028 r2: the ACCEPTANCE watermark, not the list head -- the
	 * reply deadline (windowc_commit_pending) advances the head without any
	 * server confirmation, and a cue must follow only a real accept. */
	uint16_t head = wc->accepted_action;
	while(menu->item_cue_count) {
		const struct container_menu_cue* entry = &menu->item_cue[menu->item_cue_head];
		if(entry->serial != wc->rejection_serial
		   || entry->generation != wc->generation) {
			/* Rejected transfer, or the window was reset/corrected between the
			 * click and its reply: silent, and the arm is RETIRED here so a
			 * later unrelated accept -- including one whose ids restart from
			 * zero after a windowc_create -- can never fire it. */
			container_menu_cue_pop(menu);
			continue;
		}
		/* Unsigned 16-bit distance: "the head has not reached this action yet"
		 * is the far half of the id space. */
		if((uint16_t)(head - entry->action) >= 0x8000u)
			return; /* still predicted only -- no cue on a submitted transfer */
		enum menu_sound_event cue = (enum menu_sound_event)entry->cue;
		container_menu_cue_pop(menu);
		audio_menu_event(cue, time_get());
	}
}

size_t container_menu_neighbor(const struct container_slot* slots, size_t count,
							   size_t selected, int direction) {
	if(!slots || selected >= count)
		return 0;
	size_t result = selected;
	int best = INT_MAX;
	for(size_t k = 0; k < count; k++) {
		int dx = slots[k].x - slots[selected].x;
		int dy = slots[k].y - slots[selected].y;
		bool horizontal = direction == -2 || direction == 2;
		bool eligible = direction == -2 ? dx < 0 : direction == 2 ? dx > 0
			: direction == -1 ? dy < 0 : direction == 1 && dy > 0;
		int distance = horizontal ? dx * dx + dy * dy * 8
			: dx * dx * 8 + dy * dy;
		if(eligible && distance < best) {
			best = distance;
			result = k;
		}
	}
	return result;
}

/* READ-ONLY. See container_menu.h for the contract and every -1 case. Touches
 * nothing: no field of *menu, no static, no audio cue. */
int container_menu_focus_player_slot(const struct container_menu* menu,
									 const struct container_slot* slots,
									 size_t count) {
	size_t slot;
	if(!menu || !slots || menu->selected >= count)
		return -1;
	/* A pointer that OWNS focus has already overwritten `selected` with
	 * whatever it is hovering (container_menu_focus_step), so the answer would
	 * not be the directional one. */
	if(menu->focus.pointer_owner)
		return -1;
	/* Nothing is navigable in these three states, so a focus read would be a
	 * claim about a menu that is not listening. */
	if(menu->suspended || menu->sort_open || menu->close_requested)
		return -1;
	slot = slots[menu->selected].slot;
	if(slot < (size_t)INVENTORY_SLOT_MAIN || slot >= (size_t)INVENTORY_SIZE)
		return -1; /* output / crafting / armor, or past the window */
	return (int)slot;
}

static void container_menu_read_page(struct container_input* input, unsigned player,
	const struct input_player_frame* frame) {
	if(input->close || input->suspended) { input_menu_page_cancel(player); return; }
	struct input_menu_page page;
	input_menu_page_read(player, frame, &page);
	input->page_pressed = page.pressed;
	input->page_held = page.held;
	input->page_released = page.released;
	input->page_held_ms = page.held_ms;
	input->page_swap = page.page_swap;
	input->page_hold = page.hold;
}

void container_menu_read_input(struct container_input* input,
	int x, int y, int width, int height) {
	const struct input_player_frame* owner = input_player_frame(0);
	if(owner) {
		container_menu_read_player_owned(input, 0, owner, x, y, width, height);
		return;
	}
	*input = (struct container_input) {
		.up = input_held(IB_GUI_UP) || input_pressed(IB_GUI_UP),
		.down = input_held(IB_GUI_DOWN) || input_pressed(IB_GUI_DOWN),
		.left = input_held(IB_GUI_LEFT) || input_pressed(IB_GUI_LEFT),
		.right = input_held(IB_GUI_RIGHT) || input_pressed(IB_GUI_RIGHT),
		.close = input_pressed(IB_INVENTORY),
		.pick = input_pressed(IB_GUI_CLICK),
		.split = input_pressed(IB_GUI_SPLIT) || input_pressed(IB_GUI_CLICK_ALT),
		.drop = input_pressed(IB_GUI_DROP),
		.page = input_pressed(IB_GUI_PAGE),
		.previous = input_pressed(IB_GUI_PREVIOUS),
		.quick_move = input_pressed(IB_GUI_QUICK_MOVE),
	};
	struct input_player_frame page_frame = {.connected = true,
		.category = INPUT_CAT_NONE};
	page_frame.down[IB_GUI_PAGE_SWAP] = input_held(IB_GUI_PAGE_SWAP);
	page_frame.pressed[IB_GUI_PAGE_SWAP] = input_pressed(IB_GUI_PAGE_SWAP);
	page_frame.released[IB_GUI_PAGE_SWAP] = input_released(IB_GUI_PAGE_SWAP);
	container_menu_read_page(input, 0, &page_frame);
	input->pointer_valid = input_pointer(&input->pointer_x,
		&input->pointer_y, &input->pointer_angle);
	input->pointer_x -= x;
	input->pointer_y -= y;
	input->pointer_valid = input->pointer_valid && input->pointer_x >= 0
		&& input->pointer_x < width && input->pointer_y >= 0
		&& input->pointer_y < height;
}

void container_menu_read_player(struct container_input* input,
	const struct input_player_frame* frame, int x, int y, int width, int height) {
	unsigned player = frame && frame == input_player_frame(1) ? 1 : 0;
	container_menu_read_player_owned(input, player, frame, x, y, width, height);
}

void container_menu_read_player_owned(struct container_input* input, unsigned player,
	const struct input_player_frame* frame, int x, int y, int width, int height) {
	*input = (struct container_input){0};
	if(!frame || !frame->connected) {
		input->suspended = true;
		input_menu_page_cancel(player);
		return;
	}
	*input = (struct container_input) {
		.up = frame->down[IB_GUI_UP], .down = frame->down[IB_GUI_DOWN],
		.left = frame->down[IB_GUI_LEFT], .right = frame->down[IB_GUI_RIGHT],
		.close = frame->pressed[IB_INVENTORY],
		.pick = frame->pressed[IB_GUI_CLICK],
		.split = frame->pressed[IB_GUI_SPLIT] || frame->pressed[IB_GUI_CLICK_ALT],
		.drop = frame->pressed[IB_GUI_DROP],
		.page = frame->pressed[IB_GUI_PAGE],
		.previous = frame->pressed[IB_GUI_PREVIOUS],
		.quick_move = frame->pressed[IB_GUI_QUICK_MOVE],
	};
	container_menu_read_page(input, player, frame);
	input->pointer_valid = input_owner_pointer(frame, x, y, width, height,
		&input->pointer_x, &input->pointer_y, &input->pointer_angle);
}

bool container_menu_send(void* user, struct server_rpc* rpc) {
	(void)user;
	return svin_rpc_send(rpc);
}

void container_menu_update(struct container_menu* menu,
	const struct container_slot* slots, size_t count, int x, int y,
	uint8_t window, float dt) {
	struct container_input input;
	container_menu_read_input(&input, 0, 0, gfx_width(), gfx_height());
	struct window_container* wc = gstate.players[0].windows[window];
	menu->sort_owner = &gstate.players[0];
	container_menu_sort_layout(menu, gfx_width(), gfx_height(), wc);
	/* The bottom hint row is a pointer target too (WIRING-PLAN Q2). */
	if(wc && !menu->sort_open) {
		struct input_player_frame frame;
		input_prompt_frame(0, &frame);
		enum container_row_kind kind = container_row_kind_for(wc);
		container_row_click(kind, inventory_get_picked_item(windowc_get_latest(wc), NULL),
			kind == CONTAINER_ROW_STORAGE, &frame, 0, 0, gfx_width(), gfx_height(), &input);
	}
	/* Q7: only a PAGED chest draws header arrows, so only it hit-tests them.
	 * The unpaged full-screen chest used to test the same (invisible) rects
	 * and a click near the title jumped the focus section. */
	bool paged = menu->page_count > 1;
	if(wc && wc->type == WINDOW_TYPE_CHEST && paged && !menu->sort_open
	   && input.pick && input.pointer_valid
	   && (menu->focus.pointer_owner || !menu->focus.initialized)
	   && !input.up && !input.down && !input.left && !input.right) {
		struct gutil_button_bounds r[4];
		container_chest_header_rects(menu->sort_x, menu->sort_y, 348,
			container_chest_title(menu, wc), r);
		input.previous |= gutil_button_hit(&r[0], input.pointer_x, input.pointer_y);
		input.page |= gutil_button_hit(&r[2], input.pointer_x, input.pointer_y);
	}
	if(wc && !menu->sort_open && (input.page || input.previous)) windowc_sort_interrupt(wc);
	if(paged && wc && !menu->sort_open && !input.suspended && !menu->close_requested
	   && !input.close && input.page != input.previous) {
		/* Section L/R swaps the shown half (the split chest's page rule:
		 * "Switching views never also clicks, drops or transfers"). The focus
		 * keeps its cell; the screen refills the slot ids for the new half. */
		menu->sort_focus = false;
		menu->page = (menu->page + (input.previous ? menu->page_count - 1 : 1))
			% menu->page_count;
		audio_menu_event(MENU_SOUND_SECTION, time_get());
		menu->focus = (struct gutil_menu_focus) {.initialized = true};
		menu->repeat = (struct gutil_menu_repeat) {0};
		menu->pointer_on_slot = false;
		return;
	}
	if(!paged && wc && wc->slot_count >= 36 && !input.suspended && !menu->close_requested
	   && container_menu_section_step(menu, slots, count, wc->slot_count - 36, &input))
		return;
	if(container_menu_step(menu, slots, count, x, y, gstate.players[0].windows[window],
		window, &input, dt, container_menu_send, NULL))
		screen_set(&screen_ingame);
}

static unsigned slot_section(size_t slot, size_t main_base) {
	return slot < main_base ? 0 : slot < main_base + 27 ? 1 : 2;
}

bool container_menu_section_step(struct container_menu* menu,
	const struct container_slot* slots, size_t count, size_t main_base,
	const struct container_input* input) {
	if(menu->sort_open || input->close || (!input->page && !input->previous) || !count) return false;
	bool was_sort = menu->sort_focus;
	menu->sort_focus = false;
	if(menu->selected >= count) menu->selected = 0;
	size_t before = menu->selected;
	unsigned current = slot_section(slots[menu->selected].slot, main_base);
	menu->section_selection[current] = menu->selected;
	if(input->page != input->previous) {
		unsigned target = (current + (input->previous ? 2 : 1)) % 3;
		size_t remembered = menu->section_selection[target];
		if(remembered < count && slot_section(slots[remembered].slot, main_base) == target)
			menu->selected = remembered;
		else for(size_t k = 0; k < count; k++) {
			if(slot_section(slots[k].slot, main_base) == target) {
				menu->selected = k; break;
			}
		}
	}
	if(before != menu->selected || was_sort) audio_menu_event(MENU_SOUND_SECTION, time_get());
	menu->focus = (struct gutil_menu_focus){.initialized = true};
	menu->repeat = (struct gutil_menu_repeat){0};
	menu->pointer_on_slot = false;
	return true;
}

static bool container_menu_window_blocked(const struct window_container* wc) {
	return windowc_actions_pending(wc) || wc->quick_pending
		|| wc->correction_pending || wc->transaction_reply_pending;
}

/* Close the window. Local authority (singleplayer/split): the plain request,
 * exactly as before -- callers only reach it once nothing is outstanding.
 *
 * Remote (a real Beta server): Beta's close, one Packet101 on the press, never
 * waiting for outstanding transactions. [EntityClientPlayerMP.java:132-135
 * closeScreen(): "this.sendQueue.addToSendQueue(new Packet101CloseWindow(..."
 * then "this.inventory.setItemStack((ItemStack)null);"]. A Packet106 ack still
 * owed for a rejection goes FIRST, the order Beta's client produces
 * (NetClientHandler acks a rejection the moment it arrives): the server
 * re-enables clicks only on that ack (NetServerHandler.java:523-526) and its
 * canCraft gate outlives the close (Container.java:198-209), so dropping it
 * would wedge every later click (NetServerHandler.java:499 ignores them). If
 * the ack cannot be queued the close is not sent either (honest failure).
 * After the close the window is left as Beta's client leaves it: its clicks
 * were applied locally with no rollback on silence (PlayerControllerMP.java:
 * 171-175), so outstanding predictions are COMMITTED (an accepted click gets
 * no slot/cursor update, NetServerHandler.java:502-506, so discarding would
 * strand a moved stack), and the cursor is cleared because the server's close
 * drops it without any cursor packet (server Container.java:185-190). A late
 * rejection still resyncs through Packet104 + Packet103(-1,-1). */
static bool container_menu_send_close(struct container_menu* menu,
	struct window_container* wc, uint8_t window, container_send_fn send, void* user,
	bool remote) {
	menu->close_requested = false;
	menu->close_force = false;
	if(remote && wc->transaction_reply_pending) {
		if(!send(user, &(struct server_rpc){.type = SRPC_WINDOW_TRANSACTION,
			.payload.window_transaction = {.window = window,
				.action_id = wc->transaction_reply_action, .accepted = true}})) {
			menu->send_failed = true;
			return false;
		}
		wc->transaction_reply_pending = false;
	}
	bool sent = send(user, &(struct server_rpc){.type = SRPC_WINDOW_CLOSE,
		.payload.window_close.window = window});
	menu->send_failed = !sent;
	if(sent && remote) {
		windowc_commit_pending(wc);
		inventory_clear_picked_item(windowc_get_latest(wc));
	}
	if(sent) audio_menu_event(MENU_SOUND_BACK, time_get());
	return sent;
}

/* Return only into the player's ordinary 36 slots, using ordinary acknowledged
 * clicks. Preflight all room before the first click; never swap with
 * unrelated items. Local authority keeps the acknowledged close unchanged: no
 * close packet while a cursor item or prediction remains (review M1). Against
 * a real Beta server a window close (not return_only) never waits: if the
 * window was blocked when Close was pressed, or a rejection lands during the
 * return, it closes at once the Beta way (container_menu_send_close). */
static bool container_menu_close_step(struct container_menu* menu,
	struct window_container* wc, uint8_t window, container_send_fn send, void* user) {
	bool remote = gstate.remote_session;
	bool rejected = wc->rejection_serial != menu->close_rejection_serial;
	if(remote && !menu->return_only && (wc->type == WINDOW_TYPE_INVENTORY
	   || menu->close_force || rejected))
		return container_menu_send_close(menu, wc, window, send, user, true);
	if(rejected) {
		menu->close_requested = false; menu->send_failed = true; return false;
	}
	/* Remote: only this close's OWN return click can be in flight here (a
	 * window blocked at the press took close_force above), and the predicted
	 * cursor must not be read before its ACK. */
	if(container_menu_window_blocked(wc)) return false;
	struct inventory* inv = windowc_get_latest(wc);
	struct item_data held;
	/* T12 fix r1: the PLAYER inventory closes exactly as Beta, from either
	 * page: one close-window request, never held back by a cursor stack or
	 * full storage. The window owner then drops the cursor stack
	 * (Container.java:154-160 onCraftGuiClosed) and every 2x2 craftMatrix
	 * stack (ContainerPlayer.java:48-57) -- server_local's
	 * inventory_player.c inv_on_close; a real Beta server does the same on
	 * Packet101. return_only (a sub-view returning to its parent, not a
	 * window close) keeps the acknowledged return below. */
	bool beta_close = wc->type == WINDOW_TYPE_INVENTORY && !menu->return_only;
	if(beta_close || !inventory_get_picked_item(inv, &held)) {
		if(menu->return_only) {
			menu->close_requested = false;
			return true;
		}
		return container_menu_send_close(menu, wc, window, send, user, remote);
	}
	struct item* def = item_get(&held);
	unsigned room = 0;
	size_t target = wc->slot_count;
	if(def && wc->slot_count >= 36) {
		for(int pass = 0; pass < 2; pass++) {
			for(size_t k = wc->slot_count - 36; k < wc->slot_count; k++) {
				struct item_data it = inv->items[k];
				unsigned available = 0;
				if(!pass && it.id == held.id && it.durability == held.durability
				   && it.count < def->max_stack) available = def->max_stack - it.count;
				if(pass && !it.id) available = def->max_stack;
				if(available && target == wc->slot_count) target = k;
				room += available;
			}
		}
	}
	if(room < held.count || target == wc->slot_count || menu->return_steps >= 36) {
		menu->close_requested = false; menu->return_blocked = true; return false;
	}
	uint16_t action;
	struct item_data expected = inv->items[target];
	if(!windowc_new_action(wc, &action, false, target)) {
		menu->close_requested = false; menu->send_failed = true; return false;
	}
	menu->return_steps++;
	if(!send(user, &(struct server_rpc){.type = SRPC_WINDOW_CLICK,
		.payload.window_click = {.window = window, .slot = target,
			.action_id = action, .expected = expected}})) {
		windowc_cancel_unsent(wc, action);
		menu->close_requested = false; menu->send_failed = true;
	} else windowc_get_latest(wc)->revision.sent = true;
	return false;
}

void container_menu_focus_step(struct container_menu* menu,
	const struct container_slot* slots, size_t count, int x, int y,
	const struct container_input* input, float dt) {
	menu->pointer_available = input->pointer_valid;
	menu->pointer_x = input->pointer_x;
	menu->pointer_y = input->pointer_y;
	menu->pointer_angle = input->pointer_angle;
	bool up = input->up;
	bool down = input->down;
	bool left = input->left;
	bool right = input->right;
	bool pointer_owner = gutil_menu_pointer_focus(&menu->focus,
		menu->pointer_available, menu->pointer_x, menu->pointer_y,
		up || down || left || right);
	int direction = (up || down) ? down - up : 2 * (right - left);
	int step = gutil_menu_repeat_step(&menu->repeat, direction, dt);
	int hit = -1;
	if(menu->pointer_available) {
		for(size_t k = 0; k < count; k++) {
			if(menu->pointer_x >= x + slots[k].x
			   && menu->pointer_x < x + slots[k].x + container_slot_extent(menu, &slots[k])
			   && menu->pointer_y >= y + slots[k].y
			   && menu->pointer_y < y + slots[k].y + container_slot_extent(menu, &slots[k])) {
				hit = (int)k;
				break;
			}
		}
	}
	menu->pointer_hit = hit;
	menu->pointer_on_slot = pointer_owner && hit >= 0;
	if(pointer_owner)
		menu->selection_origin = GUTIL_MENU_ORIGIN_POINTER;
	if(menu->pointer_on_slot) {
		menu->selected = (size_t)hit;
	}
	if(up || down || left || right)
		menu->selection_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	if(step)
		menu->selected = container_menu_neighbor(slots, count, menu->selected, step);

}

static bool container_menu_step_inner(struct container_menu* menu,
	const struct container_slot* slots, size_t count, int x, int y,
	struct window_container* wc, uint8_t window,
	const struct container_input* input, float dt,
	container_send_fn send, void* user) {
	if(!wc || !count)
		return false;
	bool sorting = wc->sort.status == WINDOW_SORT_RUNNING || wc->sort.status == WINDOW_SORT_ADMISSION;
	bool pointer_moved = input->pointer_valid && (!menu->pointer_available
		|| input->pointer_x != menu->pointer_x || input->pointer_y != menu->pointer_y);
	bool activity = input->suspended || input->close || input->pick || input->split
		|| input->drop || input->quick_move || input->page || input->previous
		|| input->up || input->down || input->left || input->right || pointer_moved;
	if(sorting && activity) windowc_sort_interrupt(wc);
	if(!sorting && activity && wc->sort.status == WINDOW_SORT_INTERRUPTED
	   && !windowc_actions_pending(wc)) wc->sort.status = WINDOW_SORT_IDLE;
	if(menu->sort_owner && !activity)
		client_session_sort_tick(menu->sort_owner, window, time_get(), send, user);
	if(menu->selected >= count)
		menu->selected = 0;
	menu->suspended = input->suspended;
	if(input->suspended) {
		menu->repeat = (struct gutil_menu_repeat){0};
		menu->close_requested = false;
		menu->pointer_available = menu->pointer_on_slot = false;
		menu->pointer_hit = GUTIL_MENU_TARGET_NONE;
		return false;
	}
	if(input->close && menu->sort_open) {
		menu->sort_open = false;
		menu->sort_focus = true;
		menu->repeat = (struct gutil_menu_repeat){0};
		audio_menu_event(MENU_SOUND_BACK, time_get());
		return false;
	}
	if(input->close && !menu->close_requested) {
		menu->send_failed = menu->return_blocked = menu->server_wait = false;
		menu->close_requested = true;
		menu->close_force = container_menu_window_blocked(wc);
		menu->return_steps = 0;
		menu->close_rejection_serial = wc->rejection_serial;
	}
	if(menu->close_requested)
		return container_menu_close_step(menu, wc, window, send, user);
	bool sort_supported = !menu->sort_disabled && container_menu_sort_supported(wc);
	unsigned first = wc->type == WINDOW_TYPE_INVENTORY ? 2 : 0;
	bool pointer_owner = sort_supported && gutil_menu_pointer_focus(&menu->focus,
		input->pointer_valid, input->pointer_x, input->pointer_y,
		input->up || input->down || input->left || input->right);
	int panel_width = menu->sort_compact ? 576 : 348;
	struct gutil_button_bounds entry;
	container_menu_sort_entry_bounds(menu, wc, 0, 0, &entry);
	int entry_width=entry.width, entry_x=entry.x, entry_y=entry.y;
	bool entry_hit = pointer_owner && input->pointer_x >= entry_x
		&& input->pointer_x < entry_x + entry_width
		&& input->pointer_y >= entry_y && input->pointer_y < entry_y + 24;
	if(menu->sort_open && sort_supported) {
		int hit = -1;
		if(pointer_owner && input->pointer_x >= menu->sort_x + 12
		   && input->pointer_x < menu->sort_x + panel_width - 12
		   && input->pointer_y >= menu->sort_y + 32
		   && input->pointer_y < menu->sort_y + 32 + (4 - first) * 28)
			hit = first + (int)(input->pointer_y - menu->sort_y - 32) / 28;
		if(hit >= 0) menu->sort_operation = hit;
		int step = gutil_menu_repeat_step(&menu->repeat, input->down - input->up, dt);
		if(step > 0 && menu->sort_operation < 3) menu->sort_operation++;
		if(step < 0 && menu->sort_operation > first) menu->sort_operation--;
		menu->pointer_available = input->pointer_valid;
		menu->pointer_x = input->pointer_x; menu->pointer_y = input->pointer_y;
		menu->pointer_angle = input->pointer_angle;
		/* Pointer chooses a row; Apply (or controller A) invokes the existing
		 * request. Frozen chest.html:153 does not apply on a choice click. */
		int footer_y = menu->sort_compact ? menu->sort_y + 191
			: menu->sort_y + (wc->slot_count == 90 ? 430 : 326);
		int aw = 36 + gutil_font_width("Apply", 16), bw = 36 + gutil_font_width("Back", 16);
		int viewport_x = menu->sort_x - (menu->sort_compact ? 32 : 146);
		int gap = ((menu->sort_compact ? 592 : 616) - aw - bw - 8) / 3;
		int ax = viewport_x + (menu->sort_compact ? 24 : 12) + gap;
		int bx = ax + aw + 8 + gap;
		bool footer_hit = input->pointer_y >= footer_y && input->pointer_y < footer_y + 32;
		bool apply = !pointer_owner || (footer_hit && input->pointer_x >= ax && input->pointer_x < ax + aw);
		bool back = pointer_owner && footer_hit && input->pointer_x >= bx && input->pointer_x < bx + bw;
		if(input->pick && back) {
			menu->sort_open = false;
			/* The panel's own Back control is a successful return, not a page
			 * change: spec's "Back on successful return/resume". Before this
			 * it fell through to the panel-change cue and aliased category. */
			menu->sort_back = true;
		} else if(input->pick && apply && !sorting && menu->sort_owner) {
			menu->send_failed = !client_session_sort_request(menu->sort_owner, window,
				menu->sort_operation, gstate.remote_session,
				time_get(), send, user);
			if(!menu->send_failed) { menu->sort_open = false; menu->sort_focus = false; }
		}
		return false;
	}
	if(entry_hit) menu->sort_focus = true;
	else if(pointer_owner && pointer_moved) menu->sort_focus = false;
	if(menu->sort_focus && sort_supported) {
		menu->pointer_available = input->pointer_valid;
		menu->pointer_x = input->pointer_x; menu->pointer_y = input->pointer_y;
		menu->pointer_angle = input->pointer_angle;
		if(input->down) {
			menu->sort_focus = false;
			menu->repeat = (struct gutil_menu_repeat){0};
		} else {
			if(input->pick && (!pointer_owner || entry_hit)) {
				menu->sort_open = true;
				menu->sort_operation = slots[menu->selected].slot < wc->slot_count - 36 ? first : 2;
				menu->repeat = (struct gutil_menu_repeat){0};
			}
			return false;
		}
	} else {
		size_t before = menu->selected;
		container_menu_focus_step(menu, slots, count, x, y, input, dt);
		if(sort_supported && input->up && before == menu->selected
		   && container_menu_neighbor(slots, count, before, -1) == before) {
			menu->sort_focus = true;
			menu->repeat = (struct gutil_menu_repeat){0};
			return false;
		}
	}
	/* Sent sort clicks drain before a user's next mutation. A held cursor is
	 * kept visible; Back uses the existing acknowledged safe-return path. */
	if((sorting || wc->sort.status == WINDOW_SORT_INTERRUPTED)
	   && windowc_actions_pending(wc)) return false;
	/* Await the authoritative transfer before another item action. Back and
	 * navigation remain responsive; each physical press sends at most once. */
	if(wc->quick_pending) return false;

	/* The same local prediction/transaction is used by both providers. */
	if(input->drop) {
		menu->send_failed = menu->server_wait = false;
		if(inventory_get_picked_item(windowc_get_latest(wc), NULL)) {
			uint16_t action;
			if(!windowc_new_drop_action(wc, &action, false)) {
				menu->send_failed = true;
				menu->server_wait = wc->correction_pending || wc->transaction_reply_pending;
			} else if(!send(user, &(struct server_rpc) {
				.type = SRPC_WINDOW_DROP,
				.payload.window_drop = {.window = window, .right = false,
					.action_id = action},
			})) {
				windowc_cancel_unsent(wc, action);
				menu->send_failed = true;
			} else windowc_get_latest(wc)->revision.sent = true;
		}
		return false;
	}
	bool pick = input->pick;
	bool split = input->split;
	bool quick = input->quick_move;
	if(!pick && !split && !quick)
		return false;
	int hovered=menu->pointer_available?menu->pointer_hit:GUTIL_MENU_TARGET_NONE;
	struct gutil_menu_decision decision=gutil_menu_activate_decide(
		(struct gutil_menu_activate_state){
			.pressed=true,
			.navigated=input->up||input->down||input->left||input->right,
			.pointer_valid=menu->pointer_available,
			.selection_origin=menu->selection_origin,
			.hovered=hovered,
			.hovered_enabled=hovered>=0&&hovered<(int)count
				&&inventory_slot_valid(windowc_get_latest(wc),slots[hovered].slot),
			.selected=(int)menu->selected,
			.selected_enabled=menu->selected<count
				&&inventory_slot_valid(windowc_get_latest(wc),slots[menu->selected].slot),
		});
	menu->selected=(size_t)decision.selected;
	menu->selection_origin=decision.selection_origin;
	if(decision.action!=GUTIL_MENU_ACTIVATE)
		return false;
	menu->send_failed = menu->server_wait = false;
	menu->return_blocked = false;
	size_t slot = slots[menu->selected].slot;
	if(!inventory_slot_valid(windowc_get_latest(wc), slot))
		return false;
	uint16_t action;
	struct item_data held_before = windowc_get_latest(wc)->picked_item;
	struct item_data expected = windowc_get_latest(wc)->items[slot];
	struct item_data slot_before = expected;
	if(quick && (!expected.id
		|| inventory_get_picked_item(windowc_get_latest(wc), NULL)))
		return false;
	if(!(quick ? windowc_new_quick_action(wc, &action, slot)
		: windowc_new_action(wc, &action, !pick, slot))) {
		menu->send_failed = true;
		/* Refused locally while the server still owes the correction that
		 * follows a rejection (NetServerHandler.java:508-516), not a send. */
		menu->server_wait = wc->correction_pending || wc->transaction_reply_pending;
		return false;
	}
	expected = windowc_get_latest(wc)->revision.expected;
	if(!send(user, &(struct server_rpc) {
		.type = SRPC_WINDOW_CLICK,
		.payload.window_click.window = window,
		.payload.window_click.action_id = action,
		.payload.window_click.right_click = !quick && !pick,
		.payload.window_click.quick_move = quick,
		.payload.window_click.expected = expected,
		.payload.window_click.slot = slot,
	})) {
		windowc_cancel_unsent(wc, action);
		menu->send_failed = true;
	} else {
		struct inventory* latest = windowc_get_latest(wc);
		latest->revision.sent = true;
		/* A prediction that moves nothing arms nothing (an empty-slot or
		 * blocked click stays silent). A prediction that DOES move something
		 * only ARMS this action's approved cue here -- container_menu_item_cue
		 * plays it when that action's authoritative accept lands, so a
		 * submitted-but-uncommitted or rejected transfer is never confirmed.
		 * (Before this the successful-action confirmation cue was played
		 * straight off the local prediction, for every slot alike.) */
		if(slot_before.id != latest->items[slot].id
		   || slot_before.count != latest->items[slot].count
		   || slot_before.durability != latest->items[slot].durability
		   || held_before.id != latest->picked_item.id
		   || held_before.count != latest->picked_item.count
		   || held_before.durability != latest->picked_item.durability)
			container_menu_cue_arm(menu, wc, action,
				/* The crafting RESULT slot is the one transfer that COMPLETES
				 * a craft (inventory_click_pre's SlotCrafting branch consumes
				 * the grid off exactly this predicate), so it keeps the
				 * approved confirmation cue the base tree played -- "Keep the
				 * approved crafting cues and recipes unchanged"
				 * (sound-events.md), "completed successful craft/sort"
				 * (crafting-chests.md). Every other transfer owes ITEM. */
				inventory_click_is_craft_output(wc->type, slot)
					? MENU_SOUND_SELECT : MENU_SOUND_ITEM);
	}
	return false;
}

/* Compare semantic focus once, including Sort entry/exit and pointer changes.
 * Sort COMPLETION is not declared here: it is owned by the authoritative sort
 * executor (client_session_sort_tick), which plays the confirmation cue on the
 * committing reply; rejection and interruption stay silent. */
bool container_menu_step(struct container_menu* menu,
	const struct container_slot* slots, size_t count, int x, int y,
	struct window_container* wc, uint8_t window,
	const struct container_input* input, float dt,
	container_send_fn send, void* user) {
	/* Sort parked (2026-10-25): no stale state may open or focus it, and
	 * clearing it is not a player-visible focus change (no cue). */
	if(menu->sort_disabled || !container_menu_sort_supported(wc))
		menu->sort_open = menu->sort_focus = false;
	size_t selected = menu->selected;
	bool sort_focus = menu->sort_focus;
	bool sort_open = menu->sort_open;
	unsigned operation = menu->sort_operation;
	/* Resolve an outstanding transfer BEFORE this frame's own input, so the
	 * commit cue is attributed to the frame its accept arrived in even when
	 * the player is idle, suspended or closing the window. */
	container_menu_item_cue(menu, wc);
	bool closed = container_menu_step_inner(menu, slots, count, x, y, wc,
		window, input, dt, send, user);
	bool sort_back = menu->sort_back;
	menu->sort_back = false;
	if(wc && count && !input->suspended && !input->close && !closed
	   && (sort_open != menu->sort_open || sort_focus != menu->sort_focus
		|| (menu->sort_focus ? operation != menu->sort_operation : selected != menu->selected)))
		audio_menu_event(sort_back ? MENU_SOUND_BACK
			: sort_open != menu->sort_open ? MENU_SOUND_SECTION : MENU_SOUND_FOCUS,
			time_get());
	return closed;
}

/* ---- The container hint row (container_menu.h) ------------------------- */
enum container_row_kind container_row_kind_for(const struct window_container* wc) {
	if(!wc) return CONTAINER_ROW_MACHINE;
	switch(wc->type) {
		case WINDOW_TYPE_INVENTORY: return CONTAINER_ROW_INVENTORY;
		case WINDOW_TYPE_FURNACE:
		case WINDOW_TYPE_DISPENSER: return CONTAINER_ROW_MACHINE;
		default: return CONTAINER_ROW_STORAGE; /* chests, workbench grid */
	}
}

static int container_row_items(enum container_row_kind kind, bool held, bool paged,
	struct gutil_hint_item items[CONTAINER_ROW_ITEMS],
	enum container_row_action actions[CONTAINER_ROW_ITEMS]) {
	int n = 0;
#define ROW_ITEM(role, text, prio, kept, act) do { \
		items[n] = (struct gutil_hint_item) {.roles = {(role)}, .role_count = 1, \
			.label = (text), .priority = (prio), .keep = (kept)}; \
		actions[n++] = (act); } while(0)
	if(kind == CONTAINER_ROW_INVENTORY)
		ROW_ITEM(IB_GUI_PAGE_SWAP, "Crafting", 2, false, CONTAINER_ROW_PAGE_SWAP);
	ROW_ITEM(IB_GUI_CLICK, held ? "Place" : "Pick", 6, false, CONTAINER_ROW_PICK);
	ROW_ITEM(IB_GUI_SPLIT, held ? "One" : "Split", 5, false, CONTAINER_ROW_SPLIT);
	if(kind == CONTAINER_ROW_MANUAL) {
		/* Astra ruling 6 Q1: the third cell SWAPS (Quick Move <-> Drop). */
		if(held) ROW_ITEM(IB_GUI_DROP, "Drop", 4, false, CONTAINER_ROW_DROP);
		else ROW_ITEM(IB_GUI_QUICK_MOVE, "Quick Move", 4, false, CONTAINER_ROW_QUICK);
		ROW_ITEM(IB_INVENTORY, "Recipes", 0, true, CONTAINER_ROW_CLOSE);
		return n;
	}
	if(kind != CONTAINER_ROW_INVENTORY && !held)
		ROW_ITEM(IB_GUI_QUICK_MOVE, "Quick Move", 3, false, CONTAINER_ROW_QUICK);
	if(kind == CONTAINER_ROW_STORAGE && paged) {
		items[n] = (struct gutil_hint_item) {.roles = {IB_GUI_PREVIOUS, IB_GUI_PAGE},
			.role_count = 2, .label = "Section", .priority = 1};
		actions[n++] = CONTAINER_ROW_SECTION;
	}
	/* Drop only while the cursor HOLDS a stack (Astra menu review 2026-09-19
	 * MUST 4: the Remote has no gameplay drop, so the menu must say where it
	 * is). Bound to IB_GUI_DROP, never an atlas cell, so a remap moves it. */
	if(held) ROW_ITEM(IB_GUI_DROP, "Drop", 4, false, CONTAINER_ROW_DROP);
	ROW_ITEM(IB_INVENTORY, "Close", 0, true, CONTAINER_ROW_CLOSE);
#undef ROW_ITEM
	return n;
}

int container_row_layout(enum container_row_kind kind, bool held, bool paged,
	const struct input_player_frame* frame, int x, int y, int width, int height,
	struct gutil_hint_row* row, enum container_row_action actions[CONTAINER_ROW_ITEMS]) {
	struct gutil_hint_item items[CONTAINER_ROW_ITEMS];
	int n = container_row_items(kind, held, paged, items, actions);
	if(!frame || !gutil_hint_row_layout(frame, x, y, width, height, height < 480,
		GUTIL_HINT_ALIGN_LEFT, items, n, row)) {
		memset(row, 0, sizeof(*row));
		return 0;
	}
	return row->count;
}

void container_row_draw(enum container_row_kind kind, bool held, bool paged,
	const struct input_player_frame* frame, int x, int y, int width, int height) {
	struct gutil_hint_row row;
	enum container_row_action actions[CONTAINER_ROW_ITEMS];
	if(container_row_layout(kind, held, paged, frame, x, y, width, height, &row, actions))
		gutil_hint_row_draw(&row);
}

int container_row_click(enum container_row_kind kind, bool held, bool paged,
	const struct input_player_frame* frame, int x, int y, int width, int height,
	struct container_input* in) {
	if(!in->pick || !in->pointer_valid || in->suspended
	   || in->up || in->down || in->left || in->right)
		return -1;
	struct gutil_hint_row row;
	enum container_row_action actions[CONTAINER_ROW_ITEMS];
	if(!container_row_layout(kind, held, paged, frame, x, y, width, height, &row, actions))
		return -1;
	int hit = gutil_hint_row_hit(&row, in->pointer_x, in->pointer_y);
	if(hit < 0 || hit >= CONTAINER_ROW_ITEMS) return -1;
	enum container_row_action a = actions[hit];
	/* The click sets EXACTLY the action of the cell it hit and clears the
	 * others, so the pointer never issues an action the row does not show
	 * (the rule the Manual Grid row always had). It acts on the FOCUSED slot:
	 * the pointer is consumed so no slot under it is clicked. */
	in->pick = a == CONTAINER_ROW_PICK;
	in->split = a == CONTAINER_ROW_SPLIT;
	in->quick_move = a == CONTAINER_ROW_QUICK;
	in->drop = a == CONTAINER_ROW_DROP;
	in->close = a == CONTAINER_ROW_CLOSE;
	in->page_swap = a == CONTAINER_ROW_PAGE_SWAP;
	in->page = a == CONTAINER_ROW_SECTION;
	in->previous = false;
	in->pointer_valid = false;
	return (int)a;
}

void container_menu_label(const struct container_menu* menu,
	struct window_container* wc, size_t slot, char* label, size_t capacity) {
	struct inventory* inv = windowc_get_latest(wc);
	struct item_data item;
	bool held = inventory_get_picked_item(inv, &item);
	label[0] = '\0';
	if(menu->suspended) { snprintf(label, capacity, "Reconnect controller"); return; }
	if(wc->sort.status == WINDOW_SORT_RUNNING || wc->sort.status == WINDOW_SORT_ADMISSION) {
		snprintf(label, capacity, "Sorting..."); return;
	}
	if(wc->sort.status == WINDOW_SORT_INTERRUPTED && !held) {
		snprintf(label, capacity, "Sort interrupted"); return;
	}
	if(menu->close_requested) { snprintf(label, capacity, "Returning held item..."); return; }
	if(menu->return_blocked) { snprintf(label, capacity, "Inventory full. Place item or Drop."); return; }
	if(wc->quick_pending) {
		snprintf(label, capacity, "Moving...");
		return;
	}
	/* A refusal while the server still owes its correction is a wait, not a
	 * failed send; once the correction lands the refusal is moot. */
	bool waiting = menu->send_failed && menu->server_wait;
	if(waiting && (wc->correction_pending || wc->transaction_reply_pending)) {
		snprintf(label, capacity, "Waiting for server..."); return;
	}
	bool failed = menu->send_failed && !waiting;
	if(!held && !inventory_get_slot(inv, slot, &item) && !failed)
		return;
	if(failed)
		snprintf(label, capacity, "Action not sent. Try again.");
	else {
		struct item* def = item_get(&item);
		if(def && def->has_damage && def->max_damage && item.durability)
			snprintf(label, capacity, "%s%s - Wear %u / %u", held ? "Holding: " : "",
				def->name, (unsigned)item.durability, (unsigned)def->max_damage);
		else
			snprintf(label, capacity, "%s%s", held ? "Holding: " : "", def ? def->name : "Unknown");
	}
}

void container_menu_item_name(const struct container_menu* menu, uint8_t window,
	size_t slot, int width) {
	char label[128];
	container_menu_label(menu, gstate.players[0].windows[window], slot, label, sizeof(label));
	int scale = 8 * GFX_GUI_SCALE;
	while(strlen(label) && gutil_font_width(label, scale) > width - 272)
		label[strlen(label) - 1] = '\0';
	gutil_text((width - gutil_font_width(label, scale)) / 2,
		16 * GFX_GUI_SCALE, label, scale, true);
}

/* Parked 2026-10-25 (maintainer): Sort is a CavEX addition, not Beta 1.7.3, so
 * no container screen shows or focuses it in release. This predicate is the one
 * gate every Sort path goes through (entry render, open panel, pointer hit,
 * D-pad UP from the top row, Apply), so with it false the header is Beta's.
 * The executor (client_session_sort_*) and planner (inventory_sort.c) stay in
 * the tree; revive as a documented addition by setting this to 1. */
#ifndef CONTAINER_MENU_SORT_UI
#define CONTAINER_MENU_SORT_UI 0
#endif
bool container_menu_sort_supported(const struct window_container* wc) {
#if CONTAINER_MENU_SORT_UI
	return wc && ((wc->type == WINDOW_TYPE_CHEST && (wc->slot_count == 63 || wc->slot_count == 90))
		|| (wc->type == WINDOW_TYPE_INVENTORY && wc->slot_count == 45));
#else
	(void)wc;
	return false;
#endif
}


/* Frozen chest.html CSS: lines 10-15, 19, 23, 25; header/menu:146/148/152.
 * Coordinates are physical pixels in a 640x480 (compact 640x240) viewport.
 * Chest bodies share this frozen panel origin; crafting keeps its own layout. */
void container_menu_sort_layout(struct container_menu* menu, int width, int height,
	const struct window_container* wc) {
	menu->sort_compact = height < 480;
	menu->sort_x = (width - (menu->sort_compact ? 576 : 348)) / 2;
	/* Q7: the 90-slot chest is paged into the 63-slot panel, same origin. */
	(void)wc;
	menu->sort_y = menu->sort_compact ? (height - 240) / 2 + 8 : (height - 480) / 2 + 24;
}

void container_menu_sort_entry_bounds(const struct container_menu* menu,
 const struct window_container* wc, int ox, int oy, struct gutil_button_bounds* out) {
 int width=menu->sort_compact?576:348;
 int entry_width=gutil_font_width("Sort",16)+12;
 *out=(struct gutil_button_bounds){ox+menu->sort_x+width-12-entry_width,oy+menu->sort_y+8,entry_width,24};
 if(wc->type==WINDOW_TYPE_INVENTORY&&!menu->sort_compact){out->x+=12;out->y+=20;}
}

/* Frozen chest.html:10-24,146,152. Chest presentation only. */
void container_chest_panel(int x, int y, int w, int h) {
	/* Plain pack paper with its container frame, never slot/widget strips. */
	gfx_bind_texture(&texture_gui_chest);
	gutil_texquad(x, y, 8, 4, 16, 8, w, h);
	gutil_texquad(x, y, 0, 0, 3, 3, 6, 6);
	gutil_texquad(x + 6, y, 3, 0, 170, 3, w - 12, 6);
	gutil_texquad(x + w - 6, y, 173, 0, 3, 3, 6, 6);
	gutil_texquad(x, y + 6, 0, 3, 3, 216, 6, h - 12);
	gutil_texquad(x + w - 6, y + 6, 173, 3, 3, 216, 6, h - 12);
	gutil_texquad(x, y + h - 6, 0, 219, 3, 3, 6, 6);
	gutil_texquad(x + 6, y + h - 6, 3, 219, 170, 3, w - 12, 6);
	gutil_texquad(x + w - 6, y + h - 6, 173, 219, 3, 3, 6, 6);
}

void container_chest_header_rects(int x, int y, int width, const char* title,
	struct gutil_button_bounds* out) {
	out[0] = (struct gutil_button_bounds){x + 12, y + 8, 24, 24};
	out[1] = (struct gutil_button_bounds){x + 40, y + 8,
		gutil_font_width(title, 16), 24};
	out[2] = (struct gutil_button_bounds){out[1].x + out[1].width + 4, y + 8, 24, 24};
	int sw = gutil_font_width("Sort", 16) + 12;
	out[3] = (struct gutil_button_bounds){x + width - 12 - sw, y + 8, sw, 24};
}

void container_menu_arrow(const struct gutil_button_bounds* r, const char* arrow) {
	gutil_text_rgb(r->x + (r->width - gutil_font_width(arrow, 16)) / 2,
		r->y + (r->height - 16) / 2, arrow, 16, false, 64, 64, 64);
}

void container_chest_header_paged(int x, int y, int width, const char* title,
	bool paged, const struct input_player_frame* frame) {
	struct gutil_button_bounds r[4];
	container_chest_header_rects(x, y, width, title, r);
	(void)frame;
	gutil_text_rgb(r[1].x, r[1].y + 4, title, 16, false, 64, 64, 64);
	if(!paged) return;
	/* Maintainer 2026-10-31 (Q2): plain arrows, no button glyph, on the same
	 * pointer targets; the Section L+R prompt lives in the bottom row. */
	container_menu_arrow(&r[0], "<");
	container_menu_arrow(&r[2], ">");
}

const char* container_chest_title(const struct container_menu* menu,
	const struct window_container* wc) {
	if(!wc || wc->slot_count != 90 || !menu || menu->page_count <= 1) return "Chest";
	return menu->page ? "Chest 2" : "Chest 1";
}

void container_chest_slot(int x, int y) {
	gfx_bind_texture(&texture_gui_chest);
	gutil_texquad(x - 2, y - 2, 7, 17, 18, 18, 36, 36);
}

void container_chest_info(int x, int y, int width, int bag, const char* text) {
	bool compact = bag > 0;
	char lines[128];
	snprintf(lines, sizeof(lines), "%s", text);
	int count = 0;
	char* line = lines;
	while(*line) {
		char* end = line;
		char* space = NULL;
		while(*end) {
			char saved = end[1]; end[1] = '\0';
			bool fits = gutil_font_width(line, 16) <= width;
			end[1] = saved;
			if(!fits) break;
			if(*end == ' ') space = end;
			end++;
		}
		if(*end) {
			if(space) end = space;
			/* Every word in the native label fits these columns. */
			if(end == line) end++;
			if(*end) *end++ = '\0';
		}
		count++;
		line = end;
	}
	if(compact && bag > count * 20) y += (bag - count * 20) / 2; /* centred in the bag */
	line = lines;
	for(int k = 0; k < count; k++) {
		gutil_text_rgb(x, y + k * 20, line, 16, !compact,
			compact ? 64 : 255, compact ? 64 : 255, compact ? 64 : 255);
		line += strlen(line) + 1;
	}
}

/* Player 1's full-viewport container screens: the bottom row for `window`. */
void container_menu_row(uint8_t window) {
	struct window_container* wc = gstate.players[0].windows[window];
	if(!wc) return;
	struct input_player_frame frame;
	input_prompt_frame(0, &frame);
	enum container_row_kind kind = container_row_kind_for(wc);
	container_row_draw(kind, inventory_get_picked_item(windowc_get_latest(wc), NULL),
		kind == CONTAINER_ROW_STORAGE, &frame, 0, 0, gfx_width(), gfx_height());
}

void container_menu_sort_render(const struct container_menu* menu, struct window_container* wc,
	int ox, int oy, int width, int height, const struct input_player_frame* frame) {
	if(menu->sort_disabled || !container_menu_sort_supported(wc)) return;
	struct container_menu layout = *menu;
	container_menu_sort_layout(&layout, width, height, wc);
	int x = ox + layout.sort_x, y = oy + layout.sort_y;
	int w = layout.sort_compact ? 576 : 348;
	int h = layout.sort_compact ? 188 : wc->slot_count == 90 ? 428 : 320;
	if(!menu->sort_open) {
		struct gutil_button_bounds bounds;
		container_menu_sort_entry_bounds(&layout,wc,ox,oy,&bounds);
		gutil_text_rgb(bounds.x + 6, bounds.y + 4, "Sort", 16, false, 64, 64, 64);
		if(menu->sort_focus) gutil_menu_focus_border(&bounds);
		return;
	}
	container_chest_panel(x, y, w, h);
	gutil_text_rgb(x + 12, y + 8, "Sort", 16, false, 64, 64, 64);
	static const char* labels[] = {"Chest: group & stack", "Chest: stack only",
		"Inventory: group & stack", "Inventory: stack only"};
	unsigned first = wc->type == WINDOW_TYPE_INVENTORY ? 2 : 0;
	for(unsigned k = first; k < 4; k++) {
		struct gutil_button_bounds bounds = {x + 12, y + 32 + (k - first) * 28, w - 24, 28};
		gutil_text_rgb(bounds.x + 6, bounds.y + 6, labels[k], 16, false, 64, 64, 64);
		if(menu->sort_operation == k) gutil_menu_focus_border(&bounds);
	}
	bool held = inventory_get_picked_item(windowc_get_latest(wc), NULL);
	bool busy = wc->sort.status == WINDOW_SORT_RUNNING || wc->sort.status == WINDOW_SORT_ADMISSION;
	char hint[96];
	if(held) snprintf(hint, sizeof(hint), "Place held stack first");
	else if(busy) snprintf(hint, sizeof(hint), "Sorting...");
	else if(menu->send_failed && menu->server_wait) snprintf(hint, sizeof(hint), "Waiting for server...");
	else if(menu->send_failed) snprintf(hint, sizeof(hint), "Action not sent. Try again.");
	else if(menu->sort_operation < 2) snprintf(hint, sizeof(hint), "All %u chest slots", (unsigned)wc->slot_count - 36);
	else snprintf(hint, sizeof(hint), "Hotbar stays put");
	bool blocked = held || busy || menu->send_failed;
	gutil_text_rgb(x + 12, y + 40 + (4 - first) * 28, hint, 16, false,
		blocked ? 124 : 64, blocked ? 37 : 64, blocked ? 37 : 64);
	int fy = layout.sort_compact ? y + 191 : y + (wc->slot_count == 90 ? 430 : 326);
	int aw = 36 + gutil_font_width("Apply", 16), bw = 36 + gutil_font_width("Back", 16);
	int left = ox + (width - 640) / 2 + (layout.sort_compact ? 24 : 12);
	int gap = ((layout.sort_compact ? 592 : 616) - aw - bw - 8) / 3;
	int ax = left + gap, bx = ax + aw + 8 + gap;
	/* chest.html:147 disables only Apply (CSS:5 opacity .4), not choices.
	 * Match opacity on the glyph/text themselves, without a dark rectangle. */
	int cell = frame ? frame->glyphs[IB_GUI_CLICK] : -1;
	if((held || busy) && cell >= 0 && cell < INPUT_GLYPH_CELLS) {
		gfx_bind_texture(texture_approved_controls.data&&cell<42?&texture_approved_controls:&texture_controls);
		gutil_texquad_col(ax, fy, cell % 8 * 32, cell / 8 * 32, 32, 32,
			32, 32, 255, 255, 255, 102);
		gutil_text_rgba(ax + 36, fy + 8, "Apply", 16, true, 255, 255, 255, 102);
	} else gutil_menu_hint_owned(frame, ax, fy, IB_GUI_CLICK, "Apply");
	gutil_menu_hint_owned(frame, bx, fy, IB_INVENTORY, "Back");
}

int container_slot_extent(const struct container_menu* menu, const struct container_slot* slot) {
 return slot->extent ? slot->extent : menu->slot_extent ? menu->slot_extent : 16 * GFX_GUI_SCALE;
}
