/*
	Copyright (c) 2023 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <stdio.h>

#include "window_container.h"
#include "inventory_click.h"

static void* (*s_windowc_alloc)(size_t) = malloc;
static uint32_t s_window_generation;

void windowc_sort_interrupt(struct window_container* wc) {
	if(wc && (wc->sort.status == WINDOW_SORT_RUNNING
	   || wc->sort.status == WINDOW_SORT_ADMISSION))
		wc->sort.status = WINDOW_SORT_INTERRUPTED;
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void windowc_set_alloc_hook(void* (*alloc_fn)(size_t)) {
	s_windowc_alloc = alloc_fn ? alloc_fn : malloc;
}
#endif

bool windowc_create(struct window_container* wc, enum window_type type,
					size_t slot_count) {
	assert(wc);

	ilist_inventory_init(wc->invs);
	wc->generation = ++s_window_generation;
	wc->sort = (struct window_sort_session){0};

	wc->slot_count = slot_count;
	wc->type = type;
	wc->map_counter = 0;
	wc->next_action_id = 0;
	wc->quick_pending = false;
	wc->quick_action_id = 0;
	wc->correction_pending = wc->transaction_reply_pending = false;
	wc->transaction_reply_action = 0;
	wc->furnace_cook_time = 0;
	wc->furnace_burn_time = 0;
	wc->furnace_current_item_burn = 0;
	wc->rejection_serial = 0;
	wc->accept_serial = 0;
	wc->reply_deadline_armed = false;

	uint16_t unused;
	// no action performed because new inventory is empty
	if(!windowc_new_action(wc, &unused, false, 0)) return false;
	wc->accepted_action = unused; /* the seed revision counts as committed */
	windowc_get_latest(wc)->client_map_counter = &wc->map_counter;
	return true;
}

void windowc_destroy(struct window_container* wc) {
	assert(wc);

	ilist_inventory_it_t it;
	ilist_inventory_it(it, wc->invs);

	while(!ilist_inventory_end_p(it)) {
		struct inventory* inv = ilist_inventory_ref(it);
		ilist_inventory_remove(wc->invs, it);
		inventory_destroy(inv, NULL);
		free(inv);
	}

	ilist_inventory_clear(wc->invs);
}

struct inventory* windowc_get_latest(struct window_container* wc) {
	assert(wc);
	assert(!ilist_inventory_empty_p(wc->invs));

	ilist_inventory_it_t it;
	ilist_inventory_it_last(it, wc->invs);
	return ilist_inventory_ref(it);
}

bool windowc_new_action(struct window_container* wc, uint16_t* action_id,
						bool action_type, size_t action_slot) {
	assert(wc && action_id);
	if(wc->correction_pending || wc->transaction_reply_pending
	   || action_slot >= wc->slot_count) return false;

	struct inventory* next = s_windowc_alloc(sizeof(struct inventory));

	if(!next)
		return false;

	if(!inventory_create(next, NULL, NULL, NULL, wc->slot_count, 0, 0, 0)) {
		free(next); // create failed -> the struct isn't owned by anything; plain free, not inventory_destroy
		return false;
	}

	if(!ilist_inventory_empty_p(wc->invs))
		inventory_copy(next, windowc_get_latest(wc));

	next->revision.sent = false;
	next->revision.created_map_id = 0;
	next->revision.action_id = wc->next_action_id;
	next->revision.action_type = action_type;
	next->revision.action_slot = action_slot;
	next->revision.sign_set = false;
	next->revision.no_action = ilist_inventory_empty_p(wc->invs);
	next->revision.sign_ch = 0;
	next->revision.quick_move = false;
	next->revision.drop = false;

	next->revision.expected = next->revision.no_action ? (struct item_data){0}
		: inventory_click_predict(next, wc->type, action_slot, action_type, false);

	*action_id = wc->next_action_id;
	wc->next_action_id++;

	ilist_inventory_push_back(wc->invs, next);
	return true;
}

bool windowc_actions_pending(const struct window_container* wc) {
	ilist_inventory_it_t it;
	ilist_inventory_it(it, wc->invs);
	if(ilist_inventory_end_p(it)) return false;
	ilist_inventory_next(it);
	return !ilist_inventory_end_p(it);
}

/* Make the newest prediction the authoritative base, as Beta's client holds
 * it: a click is applied to the client's own container at once and is never
 * rolled back on silence [PlayerControllerMP.java:171-175 "ItemStack var7 =
 * super.func_27174_a(...)" before the Packet102 is queued]; only a rejection
 * replaces it (NetClientHandler.java:523-536 -> Packet104 + Packet103(-1,-1)).
 * A late ACCEPT then names a retired or the base action and changes nothing --
 * correct, since an accepted click gets no slot/cursor update
 * (NetServerHandler.java:502-506) -- and a late REJECT still bumps
 * rejection_serial, owes its ack and blocks clicks until the server's
 * correction lands. correction_pending is NOT touched here. */
void windowc_commit_pending(struct window_container* wc) {
	assert(wc && !ilist_inventory_empty_p(wc->invs));
	windowc_sort_interrupt(wc);
	wc->quick_pending = false;
	while(windowc_actions_pending(wc)) {
		ilist_inventory_it_t it;
		ilist_inventory_it(it, wc->invs);
		struct inventory* base = ilist_inventory_ref(it);
		ilist_inventory_remove(wc->invs, it);
		inventory_destroy(base, NULL);
		free(base);
	}
	wc->reply_deadline_armed = false;
}

bool windowc_expire_unanswered(struct window_container* wc, ptime_t now) {
	assert(wc);
	/* Only our own unanswered clicks age out. A rejection's correction
	 * (correction_pending) is never guessed past: Beta's server sends
	 * Packet104 + Packet103(-1,-1) right behind the reject on the same stream
	 * (EntityPlayerMP.java:322-325), so clicks stay blocked until it lands;
	 * Close always works meanwhile. */
	bool waiting = windowc_actions_pending(wc) || wc->quick_pending;
	/* Nothing outstanding: disarm. An ack still owed: hold the clock (the ack
	 * must reach the server before anything is committed). */
	if(!waiting || wc->transaction_reply_pending || !wc->reply_deadline_armed) {
		wc->reply_deadline_armed = waiting;
		wc->reply_deadline = time_add_ms(now, WINDOWC_REPLY_TIMEOUT_MS);
		return false;
	}
	if(!time_reached(now, wc->reply_deadline)) return false;
	fprintf(stderr, "[window] no server reply for %u ms: committing predictions"
		" as Beta's client holds them (pending=%d quick=%d correction=%d)\n",
		(unsigned)WINDOWC_REPLY_TIMEOUT_MS, windowc_actions_pending(wc) ? 1 : 0,
		wc->quick_pending ? 1 : 0, wc->correction_pending ? 1 : 0);
	windowc_commit_pending(wc);
	return true;
}

static bool windowc_new_special_action(struct window_container* wc,
	uint16_t* action_id, size_t slot, bool right, bool quick) {
	assert(wc && action_id);
	if(wc->correction_pending || wc->transaction_reply_pending) return false;
	if(quick && (wc->quick_pending || slot >= wc->slot_count)) return false;
	struct inventory* next = s_windowc_alloc(sizeof(*next));
	if(!next) return false;
	if(!inventory_create(next, NULL, NULL, NULL, wc->slot_count, 0, 0, 0)) {
		free(next);
		return false;
	}
	inventory_copy(next, windowc_get_latest(wc));
	memset(&next->revision, 0, sizeof(next->revision));
	next->revision.action_id = wc->next_action_id++;
	next->revision.action_slot = slot;
	next->revision.action_type = right;
	next->revision.quick_move = quick;
	next->revision.drop = !quick;
	if(quick) {
		next->revision.expected = inventory_click_predict(next, wc->type, slot, right, true);
		wc->quick_pending = true;
		wc->quick_action_id = next->revision.action_id;
	} else {
		/* [740c583 Container.java:50-61]: predict cursor mutation, but the
		 * returned/expected stack for -999 is always null. */
		struct item_data dropped;
		inventory_drop_picked(next, right, &dropped);
	}
	*action_id = next->revision.action_id;
	ilist_inventory_push_back(wc->invs, next);
	return true;
}

bool windowc_new_drop_action(struct window_container* wc, uint16_t* action_id,
	bool right) {
	return windowc_new_special_action(wc, action_id, 0, right, false);
}

bool windowc_new_quick_action(struct window_container* wc, uint16_t* action_id,
	size_t slot) {
	return windowc_new_special_action(wc, action_id, slot, false, true);
}

bool windowc_new_sign_set_action(struct window_container* wc,
								 uint16_t* action_id, size_t action_slot,
								 uint8_t ch) {
	if(!wc || !action_id || action_slot >= wc->slot_count
	   || ch < ' ' || ch > '~')
		return false;

	struct inventory* next = s_windowc_alloc(sizeof(struct inventory));
	if(!next)
		return false;

	if(!inventory_create(next, NULL, NULL, NULL, wc->slot_count, 0, 0, 0)) {
		free(next);
		return false;
	}

	if(!ilist_inventory_empty_p(wc->invs))
		inventory_copy(next, windowc_get_latest(wc));

	next->revision.sent = false;
	next->revision.created_map_id = 0;
	next->revision.action_id = wc->next_action_id;
	next->revision.action_type = false;
	next->revision.action_slot = action_slot;
	next->revision.quick_move = false;
	next->revision.drop = false;
	next->revision.sign_set = true;
	next->revision.no_action = false;
	next->revision.sign_ch = ch;

	struct item_data item;
	if(!inventory_get_slot(next, action_slot, &item)) {
		inventory_destroy(next, NULL);
		free(next);
		return false;
	}
	item.count = ch;
	inventory_set_slot(next, action_slot, item);

	*action_id = wc->next_action_id;
	wc->next_action_id++;
	ilist_inventory_push_back(wc->invs, next);
	return true;
}

/* Only the newest, not-yet-enqueued action can be cancelled. Earlier sent
 * revisions still await their own ACKs; this is not a server rejection. */
void windowc_cancel_unsent(struct window_container* wc, uint16_t action_id) {
	assert(wc && !ilist_inventory_empty_p(wc->invs));
	if(!windowc_actions_pending(wc)) return;
	ilist_inventory_it_t it;
	ilist_inventory_it_last(it, wc->invs);
	struct inventory* inv = ilist_inventory_ref(it);
	if(inv->revision.action_id != action_id || inv->revision.sent) return;
	/* LIFO allocation rollback; sent predecessors keep their IDs. */
	wc->next_action_id = action_id;
	if(inv->revision.created_map_id && inv->client_map_counter)
		*inv->client_map_counter = inv->revision.map_counter_before;
	if(wc->quick_pending && wc->quick_action_id == action_id)
		wc->quick_pending = false;
	ilist_inventory_remove(wc->invs, it);
	inventory_destroy(inv, NULL);
	free(inv);
}

void windowc_action_apply_result(struct window_container* wc,
								 uint16_t action_id, bool accept) {
	assert(wc);
	assert(!ilist_inventory_empty_p(wc->invs));
	wc->reply_deadline_armed = false; /* a reply is progress: restart the clock */
	if(wc->sort.status == WINDOW_SORT_RUNNING) {
		struct window_sort_session* sort = &wc->sort;
		if(!accept || sort->acknowledged == sort->submitted
		   || sort->actions[sort->acknowledged % WINDOW_SORT_PIPELINE] != action_id)
			windowc_sort_interrupt(wc);
		else sort->acknowledged++;
	}
	if(wc->quick_pending && (action_id == wc->quick_action_id || !accept))
		wc->quick_pending = false;

	ilist_inventory_it_t it;
	ilist_inventory_it(it, wc->invs);

	if(accept) {
		// Beta's own client tracks NO per-action snapshot list at all
		// (Container.func_20113_a(short) [740c583] is an empty no-op) -- an
		// accepted Packet106Transaction's action id is not protocol-
		// constrained to match anything CaveWii's richer client-side-
		// prediction is currently tracking (a hostile/buggy peer, or an
		// ordinary latency race against clin_reset_input_handoff pruning
		// the queue on a position-correction/respawn handoff). The single-
		// pass version below used to remove every predecessor node
		// looking for a match and, on a total miss, walked off the end
		// having removed EVERY node -- including the always-present base
		// snapshot windowc_create seeds -- leaving wc->invs completely
		// empty for the very next windowc_get_latest/windowc_slot_change
		// call to dereference a garbage pointer off the intrusive list's
		// own sentinel. Scan for a match FIRST; an accepted ack for an
		// action id we are not tracking is simply ignored (the safest
		// choice: we cannot tell which prediction snapshot it was meant to
		// confirm, so neither committing forward nor discarding history is
		// correct) rather than destroying the whole queue.
		bool have_match = false;
		ilist_inventory_it_t scan;
		ilist_inventory_it(scan, wc->invs);
		while(!ilist_inventory_end_p(scan)) {
			if(ilist_inventory_ref(scan)->revision.action_id == action_id) {
				have_match = true;
				break;
			}
			ilist_inventory_next(scan);
		}

		if(have_match) wc->accept_serial++;
		/* Watermark: a matched accept, or one for an id this window issued after
		 * the current mark (its snapshot may already be committed by the reply
		 * deadline, which itself never moves the mark). */
		uint16_t ahead = (uint16_t)(action_id - wc->accepted_action);
		uint16_t issued = (uint16_t)(wc->next_action_id - wc->accepted_action);
		if(have_match || (ahead && ahead < issued))
			wc->accepted_action = action_id;
		while(have_match && !ilist_inventory_end_p(it)) {
			struct inventory* inv = ilist_inventory_ref(it);

			if(inv->revision.action_id == action_id)
				break;

			ilist_inventory_remove(wc->invs, it);
			inventory_destroy(inv, NULL);
			free(inv);
		}
	} else {
		wc->rejection_serial++;
		/* [740c583 NetClientHandler.handleTransaction/handleWindowItems]:
		 * rejection is followed by authoritative replacement, not replay.
		 * Retire every outstanding prediction, including when the ID is
		 * absent. Already sent requests are not resent; their later ACKs
		 * cannot promote a retired snapshot. Keep the committed base. */
		ilist_inventory_next(it);
		while(!ilist_inventory_end_p(it)) {
			struct inventory* inv = ilist_inventory_ref(it);
			ilist_inventory_remove(wc->invs, it);
			inventory_destroy(inv, NULL);
			free(inv);
		}
	}

	assert(!ilist_inventory_empty_p(wc->invs));
}

void windowc_discard_pending_actions(struct window_container* wc) {
	assert(wc && !ilist_inventory_empty_p(wc->invs));
	windowc_sort_interrupt(wc);
	wc->generation = ++s_window_generation;
	wc->quick_pending = false;
	wc->correction_pending = wc->transaction_reply_pending = false;
	ilist_inventory_it_t it;
	ilist_inventory_it(it, wc->invs);
	/* The first node is the authoritative base. Slot RPCs update it before
	 * replaying later speculative snapshots, so retaining exactly this node
	 * reveals the complete server state without allocating. */
	ilist_inventory_next(it);
	while(!ilist_inventory_end_p(it)) {
		struct inventory* inv = ilist_inventory_ref(it);
		ilist_inventory_remove(wc->invs, it);
		inventory_destroy(inv, NULL);
		free(inv);
	}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
size_t windowc_revision_count(const struct window_container* wc) {
	assert(wc);
	size_t count = 0;
	ilist_inventory_it_t it;
	ilist_inventory_it(it, wc->invs);
	while(!ilist_inventory_end_p(it)) {
		count++;
		ilist_inventory_next(it);
	}
	return count;
}
#endif

void windowc_hotbar_change(struct window_container* wc, size_t slot) {
	assert(wc);
	ilist_inventory_it_t it;
	ilist_inventory_it(it, wc->invs);
	while(!ilist_inventory_end_p(it)) {
		inventory_set_hotbar(ilist_inventory_ref(it), slot);
		ilist_inventory_next(it);
	}
}

void windowc_slot_change(struct window_container* wc, size_t slot,
						 struct item_data item) {
	assert(wc);
	assert(!ilist_inventory_empty_p(wc->invs));
	windowc_sort_interrupt(wc);
	wc->generation = ++s_window_generation;

	ilist_inventory_it_t it;
	ilist_inventory_it(it, wc->invs);

	struct inventory* prev = ilist_inventory_ref(it);

	if(slot == SPECIAL_SLOT_PICKED_ITEM) {
		inventory_set_picked_item(prev, item);
	} else {
		inventory_set_slot(prev, slot, item);
	}

	ilist_inventory_next(it);

	while(!ilist_inventory_end_p(it)) {
		struct inventory* current = ilist_inventory_ref(it);
		inventory_copy(current, prev);
		if(current->revision.sign_set) {
			struct item_data item;
			bool present = inventory_get_slot(
				current, current->revision.action_slot, &item);
			(void)present; // assert-only reader; -DNDEBUG orphans it
			assert(present);
			item.count = current->revision.sign_ch;
			inventory_set_slot(current, current->revision.action_slot, item);
		} else if(current->revision.drop) {
			struct item_data dropped;
			inventory_drop_picked(current, current->revision.action_type, &dropped);
		} else if(!current->revision.no_action) {
			/* Replay may reuse an existing craft ID, but must never allocate
			 * a new one if a correction changes a former no-op into a craft. */
			uint16_t* counter = current->client_map_counter;
			current->client_map_counter = NULL;
			inventory_click_predict(current, wc->type, current->revision.action_slot,
				current->revision.action_type, current->revision.quick_move);
			current->client_map_counter = counter;
		}

		prev = current;
		ilist_inventory_next(it);
	}
}
