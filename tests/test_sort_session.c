/* Slice 4: real prediction, FIFO, local authority and Beta Packet102 serializer.
 * Vanilla click semantics are independently pinned by test_beta_window_click_golden.
 * No assertion here treats remote sorting as atomic. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "block/blocks.h"
#include "item/items.h"
#include "item/inventory_sort.h"
#include "item/recipe.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "network/client_interface.h"
#include "network/inventory_chest_shared.h"
#include "network/inventory_logic.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_remote_internal.h"

static struct server_local server;
static struct server_remote remote;
static struct client_session session;
static struct entity body;
static struct inventory storage;
static struct window_container wc;
static struct inventory_sort_plan plan;
static struct server_rpc queue[INVENTORY_SORT_MAX_CLICKS + 16];
static size_t head, tail;
static uint8_t window;
static bool wire, fail_send;
static bool hold_corrections;
static struct client_rpc corrections[2];
static size_t correction_count;
static ptime_t now;

static struct item_data stack(unsigned id, unsigned count) {
	return (struct item_data){.id = id, .count = count};
}
static bool equal(struct item_data a, struct item_data b) {
	return a.id == b.id && (!a.id || (a.count == b.count && a.durability == b.durability));
}
static unsigned be16(const uint8_t* p) { return p[0] * 256u + p[1]; }

static bool send_click(void* unused, struct server_rpc* call) {
	(void)unused;
	if(fail_send) return false;
	assert(tail < sizeof(queue) / sizeof(*queue));
	if(call->type == SRPC_WINDOW_CLICK && wc.sort.status == WINDOW_SORT_RUNNING) {
		const struct inventory_sort_click* step = &wc.sort.plan->clicks[wc.sort.submitted];
		/* The transport callback sees the whole predicted click, never a slot
		 * mutation without its matching cursor mutation. */
		assert(equal(windowc_get_latest(&wc)->items[step->slot], step->slot_after));
		assert(equal(windowc_get_latest(&wc)->picked_item, step->cursor_after));
	}
	queue[tail] = *call;
	if(wire && call->type == SRPC_WINDOW_CLICK) {
		struct server_remote_impl* impl = remote.impl;
		assert(!impl->frame_count);
		remote_process_srpc(call, &remote);
		assert(impl->frame_count == 1);
		struct mp_frame frame = impl->frames[impl->frame_head].frame;
		impl->frame_head = (impl->frame_head + 1) % REMOTE_QUEUE_CAPACITY;
		impl->frame_count--;
		const uint8_t* p = frame.data;
		assert(p[0] == 0x66 && p[1] == window && !p[7]);
		struct item_data expected = {0};
		if(be16(p + 8) != 65535) expected = (struct item_data){
			.id = be16(p + 8), .count = p[10], .durability = be16(p + 11)};
		assert(frame.length == (expected.id ? 13 : 10));
		assert(equal(expected, call->payload.window_click.expected));
		queue[tail] = (struct server_rpc){.type = SRPC_WINDOW_CLICK,
			.payload.window_click = {.window = p[1], .slot = be16(p + 2),
				.right_click = p[4], .action_id = be16(p + 5), .expected = expected}};
	}
	tail++;
	return true;
}
static void reply(const struct client_rpc* call, void* unused) {
	(void)unused;
	if(hold_corrections && (call->type == CRPC_WINDOW_ITEMS || call->type == CRPC_INVENTORY_SLOT)) {
		assert(correction_count < 2); corrections[correction_count++] = *call; return;
	}
	assert(client_session_window_reply(&session, call));
}
static struct inventory* authority(void) { return server.player.active_inventory; }
static void process_one(void) {
	assert(head < tail);
	server_local_process_window_for(&server, &server.player, &queue[head++]);
}
static void tick(void) {
	client_session_sort_tick(&session, window, now, send_click, NULL);
	assert(wc.sort.submitted - wc.sort.acknowledged <= WINDOW_SORT_PIPELINE
		|| wc.sort.status == WINDOW_SORT_INTERRUPTED);
	now = time_add_ms(now, 1);
}
static void discard_rpc(struct server_rpc* call, void* unused) { (void)call; (void)unused; }
static void setup(size_t capacity, enum inventory_sort_operation op, bool remote_mode) {
	memset(&server, 0, sizeof(server));
	memset(&session, 0, sizeof(session));
	wire = remote_mode; fail_send = hold_corrections = false;
	correction_count = 0; head = tail = 0; now = time_get();
	window = capacity == 45 ? 0 : capacity == 63 ? WINDOWC_CHEST : WINDOWC_IRON_CHEST;
	assert(windowc_create(&wc, capacity == 45 ? WINDOW_TYPE_INVENTORY : WINDOW_TYPE_CHEST, capacity));
	struct inventory* inv = capacity == 45 ? &server.player.inventory : &storage;
	assert(inventory_create(inv, NULL, NULL, NULL, capacity, 0,0,0));
	inv->logic = capacity == 45 ? &inventory_logic_player : capacity == 63
		? &inventory_logic_chest : &inventory_logic_double_chest;
	inv->owner = &server.player; inv->user = &server; inv->reply = reply;
	inv->has_partner = capacity == 90; inv->x2 = 1;
	server.player.active_inventory = inv;
	server.player.health = 160; server.player.has_pos = server.player.finished_loading = true;
	if(capacity != 45) server.chest_views = inv;
	session.local_player = &body; session.visible_window = window; session.windows[window] = &wc;
	bool chest = op <= INVENTORY_SORT_CHEST_STACK;
	size_t begin = chest ? 0 : capacity - 36, end = chest ? capacity - 36 : capacity - 9;
	for(size_t k = 0; k < capacity; k++) {
		/* Full scopes, repeated identities, and a full player inventory. */
		inv->items[k] = stack(3, 64);
		if(k >= begin && k < end) inv->items[k] = stack(k % 2 ? 1 : 3,
			(op == INVENTORY_SORT_CHEST_STACK || op == INVENTORY_SORT_MAIN_STACK) ? 16 : 64);
		windowc_slot_change(&wc, k, inv->items[k]);
	}
	uint8_t limits[90]; bool pinned[90] = {0}; memset(limits, 64, sizeof(limits));
	struct inventory_sort_input input = {.slots = inv->items, .capacity = capacity,
		.window = wc.type, .operation = op, .max_stack = limits, .pinned = pinned,
		.snapshot_generation = wc.generation, .current_generation = wc.generation};
	assert(inventory_sort_build(&input, &plan) == INVENTORY_SORT_READY);
}
static void cleanup(void) {
	struct inventory* inv = authority();
	inv->logic = NULL; inventory_destroy(inv, NULL);
	windowc_destroy(&wc); session.windows[window] = NULL;
	client_session_clear_windows(&session);
	svin_process_messages(discard_rpc, NULL, false);
}
static void start(bool chest) {
	assert(client_session_sort_start(&session, window, &plan, chest, wire, now, send_click, NULL));
	if(!wire) { assert(wc.sort.status == WINDOW_SORT_ADMISSION); process_one(); }
	assert(wc.sort.status == WINDOW_SORT_RUNNING);
}
static void complete(void) {
	unsigned loops = 0;
	while(wc.sort.status == WINDOW_SORT_RUNNING) {
		tick();
		if(head < tail) process_one();
		assert(++loops < 2000);
	}
	assert(wc.sort.status == WINDOW_SORT_COMPLETE);
	assert(wc.sort.submitted == plan.count && wc.sort.acknowledged == plan.count);
	assert(!windowc_actions_pending(&wc));
	assert(!authority()->picked_item.id && !windowc_get_latest(&wc)->picked_item.id);
	assert(!memcmp(authority()->items, plan.target, wc.slot_count * sizeof(struct item_data)));
	assert(!memcmp(windowc_get_latest(&wc)->items, plan.target, wc.slot_count * sizeof(struct item_data)));
}
static void full_plans(void) {
	for(int mode = 0; mode < 2; mode++)
		for(size_t cap = 45; cap <= 90; cap = cap == 45 ? 63 : cap == 63 ? 90 : 91)
			for(int op = cap == 45 ? 2 : 0; op < 4; op++) {
				setup(cap, op, mode); start(op < 2); tick();
				assert(wc.sort.submitted == WINDOW_SORT_PIPELINE);
				complete(); cleanup();
			}
	puts("PASS local/remote single, double, full player inventory: all operations, byte-identical target, depth 4");
}

static void admission(void) {
	setup(90, INVENTORY_SORT_CHEST_GROUP, false);
	struct inventory peer = {.logic = &inventory_logic_chest, .x = 1};
	storage.shared_next = &peer;
	assert(!inventory_chest_sort_admitted(&storage));
	assert(client_session_sort_start(&session, window, &plan, true, false, now, send_click, NULL));
	process_one(); assert(wc.sort.status == WINDOW_SORT_INTERRUPTED && !wc.sort.submitted);
	storage.shared_next = NULL;
	assert(inventory_chest_sort_admitted(&storage));
	assert(!client_session_sort_start(&session, window, &plan, false, false, now, send_click, NULL));
	session.local_player = NULL;
	assert(!client_session_sort_start(&session, window, &plan, true, true, now, send_click, NULL));
	session.local_player = &body;
	windowc_get_latest(&wc)->picked_item = stack(1,1);
	assert(!client_session_sort_start(&session, window, &plan, true, true, now, send_click, NULL));
	windowc_get_latest(&wc)->picked_item = (struct item_data){0};
	plan.generation--;
	assert(!client_session_sort_start(&session, window, &plan, true, true, now, send_click, NULL));
	plan.generation++;
	uint16_t id;
	assert(windowc_new_action(&wc, &id, false, 0));
	assert(!client_session_sort_start(&session, window, &plan, true, true, now, send_click, NULL));
	windowc_cancel_unsent(&wc,id);
	start(true);
	storage.shared_next = &peer; /* Viewer opens after the grant. */
	tick(); process_one();
	assert(wc.sort.status == WINDOW_SORT_INTERRUPTED && authority()->transaction_pending);
	storage.shared_next = NULL;
	cleanup();
	puts("PASS owned viewer, overlapping double half, grant race, cursor, pending and generation admission");
}

static void interruption_indices(size_t capacity, unsigned op) {
	setup(capacity, op, true);
	size_t length = plan.count;
	cleanup();
	for(size_t stop = 0; stop <= length; stop++)
	for(size_t flying = 0; flying <= 3 && flying <= stop; flying++) {
		setup(capacity, op, true);
		struct item_data slots[90];
		memcpy(slots, authority()->items, capacity * sizeof(*slots));
		start(op < 2);
		/* Bound only the producer for this fixture, then restore the full
		 * plan BEFORE interrupting. No tick sees completion at the bound. */
		plan.count = stop;
		while(wc.sort.submitted < stop) {
			tick();
			if(wc.sort.submitted < stop) process_one();
		}
		while(tail - head > flying) process_one();
		plan.count = length;
		assert(wc.sort.status == WINDOW_SORT_RUNNING);
		assert(wc.sort.submitted == stop && tail - head == flying);
		assert(wc.sort.submitted - wc.sort.acknowledged == flying);
		windowc_sort_interrupt(&wc);
		size_t submitted = wc.sort.submitted;
		tick(); assert(wc.sort.submitted == submitted);
		while(head < tail) process_one();
		assert(wc.sort.status == WINDOW_SORT_INTERRUPTED);
		assert(!windowc_actions_pending(&wc));
		struct inventory expected = {0};
		/* Independent prefix replay from the original full fixture. */
		expected.items = slots; expected.capacity = capacity;
		for(size_t k = 0; k < submitted; k++)
			inventory_click_predict(&expected, wc.type, plan.clicks[k].slot, false, false);
		assert(!memcmp(slots, authority()->items, capacity * sizeof(*slots)));
		assert(equal(expected.picked_item, authority()->picked_item));
		assert(equal(expected.picked_item, windowc_get_latest(&wc)->picked_item));
		cleanup();
	}
}

static void generation_and_interruption(void) {
	const size_t capacities[] = {45, 63, 90};
	for(size_t layout = 0; layout < 3; layout++)
		for(unsigned op = layout ? 0 : 2; op < 4; op++)
			interruption_indices(capacities[layout], op);
	/* Foreign changes to processed, future and out-of-scope slots invalidate
	 * even when they cause no Packet106 rejection. ACK prefix stays applied. */
	for(unsigned kind = 0; kind < 3; kind++) {
		setup(90, INVENTORY_SORT_CHEST_GROUP, true); start(true); tick();
		while(head < tail) process_one();
		size_t submitted = wc.sort.submitted;
		size_t slot = kind == 0 ? plan.clicks[0].slot : kind == 1 ? plan.clicks[submitted].slot : 89;
		authority()->items[slot] = stack(4,17);
		struct client_rpc change = {.type = CRPC_INVENTORY_SLOT,
			.payload.inventory_slot = {.window = window, .slot = slot, .item = stack(4,17)}};
		reply(&change, NULL); tick();
		assert(wc.sort.status == WINDOW_SORT_INTERRUPTED && wc.sort.submitted == submitted);
		assert(!memcmp(authority()->items, windowc_get_latest(&wc)->items, 90 * sizeof(struct item_data)));
		cleanup();
	}
	setup(90, INVENTORY_SORT_CHEST_GROUP, true); start(true); tick();
	size_t sent = wc.sort.submitted;
	authority()->items[89] = stack(4,17);
	reply(&(struct client_rpc){.type = CRPC_INVENTORY_SLOT,
		.payload.inventory_slot = {.window = window, .slot = 89, .item = stack(4,17)}}, NULL);
	tick(); assert(wc.sort.submitted == sent && wc.sort.status == WINDOW_SORT_INTERRUPTED);
	while(head < tail) process_one();
	assert(!memcmp(authority()->items, windowc_get_latest(&wc)->items, 90 * sizeof(struct item_data)));
	assert(equal(authority()->picked_item, windowc_get_latest(&wc)->picked_item));
	cleanup();
	puts("PASS every exact submitted prefix 0..count with 0..3 in flight: sent prefix authoritative, cursor retained; foreign processed/future/out-of-scope changes abort");
}

static void response(struct server_rpc* call, void* unused) {
	(void)unused;
	if(call->type == SRPC_WINDOW_TRANSACTION)
		server_local_process_window_for(&server, &server.player, call);
}
static void rejection_timeout(void) {
	setup(63, INVENTORY_SORT_CHEST_GROUP, true); start(true); tick();
	process_one();
	/* Rejection AFTER mutation: foreign viewer changed the next clicked slot. */
	uint8_t slot = queue[head].payload.window_click.slot;
	authority()->items[slot] = stack(4,2);
	uint16_t rejected = queue[head].payload.window_click.action_id;
	hold_corrections = true;
	process_one();
	assert(wc.sort.status == WINDOW_SORT_INTERRUPTED && authority()->transaction_pending);
	uint16_t blocked;
	assert(wc.correction_pending && !windowc_new_action(&wc, &blocked, false, 0));
	assert(correction_count == 2);
	hold_corrections = false;
	for(size_t k = 0; k < correction_count; k++) reply(&corrections[k], NULL);
	/* Correction arrived; the server still fences pipelined clicks until echo. */
	struct item_data corrected[90]; memcpy(corrected, authority()->items, wc.slot_count * sizeof(*corrected));
	while(head < tail) process_one();
	assert(!memcmp(corrected, authority()->items, wc.slot_count * sizeof(*corrected)));
	struct server_rpc wrong = {.type = SRPC_WINDOW_TRANSACTION,
		.payload.window_transaction = {.window = window, .action_id = rejected + 1, .accepted = true}};
	server_local_process_window_for(&server, &server.player, &wrong);
	assert(authority()->transaction_pending);
	svin_process_messages(response, NULL, false);
	assert(!authority()->transaction_pending);
	cleanup();
	for(unsigned ack_first = 0; ack_first < 2; ack_first++) {
		setup(63, INVENTORY_SORT_CHEST_GROUP, true); start(true); tick();
		uint16_t retired = queue[head].payload.window_click.action_id;
		if(ack_first) {
			wc.sort.deadlines[0] = time_get();
			process_one(); /* Expiry in the ACK callback, before a render tick. */
		} else {
			now = time_add_ms(now, WINDOW_SORT_TIMEOUT_MS); tick();
		}
		assert(wc.sort.status == WINDOW_SORT_INTERRUPTED && wc.sort.submitted == 4);
		assert(wc.correction_pending && !windowc_actions_pending(&wc));
		assert(!wc.transaction_reply_pending); /* No invented server rejection. */
		struct container_compact closing;
		assert(container_compact_open(&closing, wc.type, wc.slot_count, 0, 640, 240));
		struct container_input back = {.close = true};
		size_t sent = tail;
		assert(!container_menu_step(&closing.menu, closing.slots, closing.count, 0,0,
			&wc, window, &back, .016f, send_click, NULL));
		assert(closing.menu.close_requested && tail == sent);
		enum window_sort_status stopped = wc.sort.status;
		assert(stopped != WINDOW_SORT_RUNNING && stopped != WINDOW_SORT_ADMISSION);
		uint16_t blocked;
		assert(!windowc_new_action(&wc, &blocked, false, 0));
		struct item_data base[63];
		memcpy(base, windowc_get_latest(&wc)->items, sizeof(base));
		struct item_data cursor = windowc_get_latest(&wc)->picked_item;
		while(head < tail) process_one();
		reply(&(struct client_rpc){.type = CRPC_WINDOW_TRANSACTION,
			.payload.window_transaction = {.window = window, .action_id = retired, .accepted = true}}, NULL);
		assert(wc.sort.acknowledged == 0 && !windowc_actions_pending(&wc));
		assert(wc.correction_pending && equal(cursor, windowc_get_latest(&wc)->picked_item));
		assert(!memcmp(base, windowc_get_latest(&wc)->items, sizeof(base)));
		struct client_rpc snapshot = {.type = CRPC_WINDOW_ITEMS,
			.payload.window_items = {.window = window, .count = 63}};
		memcpy(snapshot.payload.window_items.items, authority()->items, sizeof(base));
		reply(&snapshot, NULL);
		assert(wc.correction_pending); /* Slots alone cannot reconcile the cursor. */
		reply(&(struct client_rpc){.type = CRPC_INVENTORY_SLOT,
			.payload.inventory_slot = {.window = window, .slot = SPECIAL_SLOT_PICKED_ITEM,
				.item = authority()->picked_item}}, NULL);
		assert(!wc.correction_pending && equal(authority()->picked_item, windowc_get_latest(&wc)->picked_item));
		assert(!memcmp(authority()->items, windowc_get_latest(&wc)->items, sizeof(base)));
		tick(); assert(wc.sort.status == stopped && wc.sort.submitted == 4);
		cleanup();
	}
	setup(63, INVENTORY_SORT_CHEST_GROUP, false);
	assert(client_session_sort_start(&session, window, &plan, true, false, now, send_click, NULL));
	now = time_add_ms(now, 8000); tick();
	assert(wc.sort.status == WINDOW_SORT_ADMISSION);
	process_one(); tick();
	/* Force expired timestamps too: the ACK callback uses time_get(). */
	for(size_t k = 0; k < WINDOW_SORT_PIPELINE; k++)
		wc.sort.deadlines[k] = time_get();
	now = time_add_ms(now, 8000); tick();
	assert(wc.sort.status == WINDOW_SORT_RUNNING && wc.sort.submitted == 4);
	process_one(); assert(wc.sort.status == WINDOW_SORT_RUNNING);
	complete(); cleanup();
	setup(63, INVENTORY_SORT_CHEST_GROUP, false);
	assert(client_session_sort_start(&session, window, &plan, true, false, now, send_click, NULL));
	struct server_rpc stale = queue[head++];
	windowc_sort_interrupt(&wc);
	assert(client_session_sort_start(&session, window, &plan, true, false, now, send_click, NULL));
	server_local_process_window_for(&server, &server.player, &stale);
	assert(wc.sort.status == WINDOW_SORT_ADMISSION);
	process_one(); complete(); cleanup();
	puts("PASS rejection recovery; remote tick/ACK expiry retires predictions and fences stray ACKs; delayed local admission and 8s ACK complete");
}

static bool pins(void* unused, struct client_session* owner, uint8_t id,
	bool* mask, size_t capacity) {
	(void)unused;
	assert(owner == &session && id == window && capacity == 63);
	mask[0] = true;
	return true;
}
static void hooks_and_adapters(void) {
	setup(63, INVENTORY_SORT_CHEST_GROUP, true);
	struct container_compact navigation;
	assert(container_compact_open(&navigation, wc.type, wc.slot_count, 0, 640, 240));
	navigation.menu.sort_owner = &session;
	struct container_input nav = {.pointer_valid = true, .pointer_x = 590, .pointer_y = 24};
	container_compact_step(&navigation, &wc, window, &nav, .016f, send_click, NULL);
	/* Sort UI parked 2026-10-25 (maintainer): the former header entry is not a
	 * target, and A there, D-pad UP and a plain A never open a panel or start a
	 * sort. The executor below stays exercised through client_session_sort_*. */
	assert(!container_menu_sort_supported(&wc));
	assert(!navigation.menu.sort_focus && !navigation.menu.sort_open);
	nav.pick = true;
	container_compact_step(&navigation, &wc, window, &nav, .016f, send_click, NULL);
	assert(!navigation.menu.sort_focus && !navigation.menu.sort_open);
	nav = (struct container_input){.up=true};
	container_compact_step(&navigation, &wc, window, &nav, .016f, send_click, NULL);
	assert(!navigation.menu.sort_focus && !navigation.menu.sort_open);
	gstate.remote_session = true;
	nav = (struct container_input){.pick=true};
	container_compact_step(&navigation, &wc, window, &nav, .016f, send_click, NULL);
	assert(!navigation.menu.sort_open && wc.sort.status == WINDOW_SORT_IDLE);
	gstate.remote_session = false;
	cleanup();
	for(unsigned imported = 0; imported < 2; imported++) {
		setup(63, INVENTORY_SORT_CHEST_GROUP, true);
		if(imported) {
			authority()->items[0].nbt_record = UINT8_MAX;
			windowc_slot_change(&wc, 0, authority()->items[0]);
			assert(!session.sort_pinned_mask);
		} else session.sort_pinned_mask = pins;
		struct item_data pinned = authority()->items[0];
		assert(client_session_sort_request(&session, window, INVENTORY_SORT_CHEST_GROUP,
			true, now, send_click, NULL));
		unsigned loops = 0;
		while(wc.sort.status == WINDOW_SORT_RUNNING) {
			tick(); if(head < tail) process_one(); assert(++loops < 2000);
		}
		assert(wc.sort.status == WINDOW_SORT_COMPLETE && equal(authority()->items[0], pinned));
		assert(authority()->items[0].nbt_record == pinned.nbt_record);
		assert(windowc_get_latest(&wc)->items[0].nbt_record == pinned.nbt_record);
		cleanup();
	}
	for(unsigned adapter = 0; adapter < 2; adapter++)
		for(unsigned kind = 0; kind < 8; kind++) {
			setup(63, INVENTORY_SORT_CHEST_GROUP, true); start(true); tick();
			struct container_compact compact;
			assert(container_compact_open(&compact, wc.type, wc.slot_count, 0, 640, 240));
			compact.menu.sort_owner = &session;
			struct container_input input = {0};
			if(kind == 0) input.pick = true;
			if(kind == 1) input.close = true;
			if(kind == 2) input.right = true;
			if(kind == 3) input.page = true;
			if(kind == 4) input.drop = true;
			if(kind == 5) input.split = true;
			if(kind == 6) input.quick_move = true;
			if(kind == 7) input.suspended = true;
			size_t sent = tail;
			if(adapter) container_compact_step(&compact, &wc, window, &input, .016f, send_click, NULL);
			else container_menu_step(&compact.menu, compact.slots, compact.count, 0,0,
				&wc, window, &input, .016f, send_click, NULL);
			assert(wc.sort.status == WINDOW_SORT_INTERRUPTED && tail == sent);
			while(head < tail) process_one();
			assert(equal(authority()->picked_item, windowc_get_latest(&wc)->picked_item));
			if(kind == 1) {
				assert(authority()->picked_item.id); /* four clicks end mid-cycle */
				input = (struct container_input){0};
				container_menu_step(&compact.menu, compact.slots, compact.count, 0,0,
					&wc, window, &input, .016f, send_click, NULL);
				assert(compact.menu.return_blocked && tail == sent);
				assert(equal(authority()->picked_item, windowc_get_latest(&wc)->picked_item));
				/* Once room is authoritative, Back returns the held stack
				 * through an ACKed click before it emits the close packet. */
				authority()->items[62] = (struct item_data){0};
				reply(&(struct client_rpc){.type = CRPC_INVENTORY_SLOT,
					.payload.inventory_slot = {.window = window, .slot = 62}}, NULL);
				input.close = true; /* Full inventory cancelled the first Back request. */
				assert(!container_menu_step(&compact.menu, compact.slots, compact.count, 0,0,
					&wc, window, &input, .016f, send_click, NULL));
				assert(tail == sent + 1 && queue[head].type == SRPC_WINDOW_CLICK);
				assert(windowc_actions_pending(&wc));
				process_one();
				assert(!windowc_actions_pending(&wc) && !authority()->picked_item.id);
				assert(container_menu_step(&compact.menu, compact.slots, compact.count, 0,0,
					&wc, window, &input, .016f, send_click, NULL));
				assert(tail == sent + 2 && queue[head].type == SRPC_WINDOW_CLOSE);
			}
			cleanup();
		}
	setup(63, INVENTORY_SORT_CHEST_GROUP, true); start(true);
	fail_send = true; tick();
	assert(wc.sort.status == WINDOW_SORT_INTERRUPTED && !windowc_actions_pending(&wc)
		&& !windowc_get_latest(&wc)->picked_item.id);
	cleanup();
	puts("PASS caller pinned hook, compact/native click/Back/navigation/page/drop/split/quick/controller-suspension interruption, unsent rollback");
}

static size_t fifo_seen;
static void fifo_capture(struct server_rpc* call, void* unused) {
	(void)unused;
	if(!fifo_seen) assert(call->type == SRPC_HOTBAR_SLOT);
	else {
		assert(call->type == SRPC_WINDOW_CLICK && !call->payload.window_click.quick_move);
		assert(equal(call->payload.window_click.expected, plan.clicks[fifo_seen - 1].expected));
		assert(call->payload.window_click.action_id == wc.sort.actions[fifo_seen - 1]);
	}
	fifo_seen++;
}
static void fifo_and_rejection_indices(void) {
	setup(63, INVENTORY_SORT_CHEST_GROUP, true);
	assert(svin_rpc_send(&(struct server_rpc){.type = SRPC_HOTBAR_SLOT, .payload.hotbar_slot.slot = 2}));
	assert(client_session_sort_start(&session, window, &plan, true, true, now, container_menu_send, NULL));
	client_session_sort_tick(&session, window, now, container_menu_send, NULL);
	fifo_seen = 0; svin_process_messages(fifo_capture, NULL, false);
	assert(fifo_seen == WINDOW_SORT_PIPELINE + 1);
	size_t count = plan.count;
	cleanup();
	for(size_t reject = 0; reject < count; reject++) {
		setup(63, INVENTORY_SORT_CHEST_GROUP, true); start(true);
		for(size_t k = 0; k < reject; k++) { tick(); process_one(); }
		tick();
		authority()->items[queue[head].payload.window_click.slot] = stack(4,2);
		hold_corrections = true; process_one();
		assert(wc.sort.status == WINDOW_SORT_INTERRUPTED && authority()->transaction_pending);
		uint16_t action;
		assert(!windowc_new_action(&wc, &action, false, 0));
		size_t sent = tail; tick(); assert(tail == sent);
		while(head < tail) process_one();
		hold_corrections = false;
		for(size_t k = 0; k < correction_count; k++) reply(&corrections[k], NULL);
		svin_process_messages(response, NULL, false);
		assert(!authority()->transaction_pending && !wc.correction_pending);
		assert(!memcmp(authority()->items, windowc_get_latest(&wc)->items, 63 * sizeof(struct item_data)));
		assert(equal(authority()->picked_item, windowc_get_latest(&wc)->picked_item));
		cleanup();
	}
	puts("PASS shared FIFO preserves preexisting action order; rejection at every step index fences and reconciles without rollback");
}

/* These fixtures enter the actual client lifecycle handlers, with heap-owned
 * windows and a leased request plan (not the standalone wc/plan above). */
static bool lifecycle_send(void* unused, struct server_rpc* call) {
	(void)unused;
	assert(call->type == SRPC_WINDOW_CLICK);
	assert(tail < sizeof(queue) / sizeof(*queue));
	queue[tail++] = *call;
	return true;
}
static struct window_container* lifecycle_plan(struct client_session* owner) {
	struct window_container* win = malloc(sizeof(*win));
	assert(win && windowc_create(win, WINDOW_TYPE_CHEST, 63));
	owner->windows[WINDOWC_CHEST] = win;
	owner->visible_window = WINDOWC_CHEST;
	for(size_t k = 0; k < 63; k++)
		windowc_slot_change(win, k, stack(k % 2 ? 1 : 3, 64));
	assert(client_session_sort_request(owner, WINDOWC_CHEST, INVENTORY_SORT_CHEST_GROUP,
		true, time_get(), lifecycle_send, NULL));
	client_session_sort_tick(owner, WINDOWC_CHEST, time_get(), lifecycle_send, NULL);
	assert(win->sort.status == WINDOW_SORT_RUNNING && win->sort.submitted == 4);
	assert(windowc_actions_pending(win));
	return win;
}
static void lifecycle_transitions(void) {
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	gstate.remote_session = true;
	clin_force_world_reset();
	for(unsigned kind = 0; kind < 4; kind++) {
		struct client_session* owner = &gstate.players[0];
		tail = head = 0;
		struct window_container* win = lifecycle_plan(owner);
		const struct inventory_sort_plan* leased = win->sort.plan;
		uint16_t retired = queue[0].payload.window_click.action_id;
		if(kind == 0) {
			/* Actual server-initiated window close mid-plan. */
			struct client_rpc close = {.type = CRPC_WINDOW_CLOSE_REMOTE,
				.payload.window_close_remote.window = WINDOWC_CHEST};
			clin_process_direct(&close);
			assert(!owner->windows[WINDOWC_CHEST] && owner->visible_window == -1);
		} else if(kind == 1) {
			/* Production disconnect seam: retire the remote provider, drain
			 * its inbox, then reset the client's world before provider reuse. */
			server_remote_cancel(&remote);
			server_remote_destroy(&remote);
			assert(!remote.impl);
			clin_update_drain();
			clin_force_world_reset();
			assert(!owner->windows[WINDOWC_CHEST]);
			assert(server_remote_init(&remote));
			((struct server_remote_impl*)remote.impl)->state = REMOTE_PLAY;
		} else if(kind == 2) {
			/* Local death/respawn uses a fresh-body epoch after the complete
			 * authoritative inventory/cursor sync, not controller suspension. */
			struct client_rpc health = {.type = CRPC_PLAYER_SET_HEALTH,
				.payload.player_set_health.health = 0};
			clin_process_direct(&health);
			assert(owner->local_player->health == 0);
			struct client_rpc snapshot = {.type = CRPC_WINDOW_ITEMS,
				.payload.window_items = {.window = WINDOWC_CHEST, .count = 63}};
			for(size_t k = 0; k < 63; k++) snapshot.payload.window_items.items[k] = stack(4, 1);
			clin_process_direct(&snapshot);
			struct client_rpc cursor = {.type = CRPC_INVENTORY_SLOT,
				.payload.inventory_slot = {.window = WINDOWC_CHEST,
					.slot = SPECIAL_SLOT_PICKED_ITEM}};
			clin_process_direct(&cursor);
			struct client_rpc respawn = {.type = CRPC_PLAYER_POS,
				.payload.player_pos = {.position = {0, 70, 0}, .input_epoch = 1,
					.fresh_body = true, .oxygen = 300}};
			const size_t cells = (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
			uint8_t* air = calloc(cells, 1);
			assert(air && world_build_full_section(&gstate.world, 0, 0,
				air, air, air, air, air, air, air));
			free(air);
			clin_test_reset_input_epoch_guard();
			clin_process_direct(&respawn);
			assert(win->sort.status == WINDOW_SORT_INTERRUPTED);
			assert(!windowc_actions_pending(win) && windowc_revision_count(win) == 1);
			assert(!win->correction_pending && !win->transaction_reply_pending);
			assert(!windowc_get_latest(win)->picked_item.id);
			assert(!memcmp(windowc_get_latest(win)->items, snapshot.payload.window_items.items,
				63 * sizeof(struct item_data)));
			struct client_rpc late = {.type = CRPC_WINDOW_TRANSACTION,
				.payload.window_transaction = {.window = WINDOWC_CHEST,
					.action_id = retired, .accepted = true}};
			clin_process_direct(&late);
			assert(win->sort.status == WINDOW_SORT_INTERRUPTED && win->sort.acknowledged == 0);
			assert(windowc_revision_count(win) == 1 && !windowc_get_latest(win)->picked_item.id);
			assert(!memcmp(windowc_get_latest(win)->items, snapshot.payload.window_items.items,
				63 * sizeof(struct item_data)));
			windowc_destroy(win); free(win); owner->windows[WINDOWC_CHEST] = NULL;
		} else {
			/* World unload/reset consumes the same real CRPC as the server. */
			struct client_rpc reset = {.type = CRPC_WORLD_RESET,
				.payload.world_reset.dimension = WORLD_DIM_OVERWORLD};
			clin_process_direct(&reset);
			assert(!owner->windows[WINDOWC_CHEST]);
		}
		struct client_rpc stray = {.type = CRPC_WINDOW_TRANSACTION,
			.payload.window_transaction = {.window = WINDOWC_CHEST,
				.action_id = retired, .accepted = true}};
		clin_process_direct(&stray);
		assert(!owner->windows[WINDOWC_CHEST]);
		assert(!windowc_actions_pending(owner->windows[WINDOWC_INVENTORY]));
		/* Closed/interrupted plans do not hold a window lease: this session
		 * can immediately reuse its cached buffer for a new window. */
		win = lifecycle_plan(owner);
		assert(win->sort.plan == leased);
		client_session_clear_windows(owner);
		/* A teardown must release the session reservation too: two OTHER
		 * owners can lease both buffers simultaneously, without the old one. */
		struct client_session probes[CLIENT_LOCAL_PLAYERS] = {0};
		for(size_t k = 0; k < CLIENT_LOCAL_PLAYERS; k++) {
			probes[k].local_player = &body;
			lifecycle_plan(&probes[k]);
		}
		for(size_t k = 0; k < CLIENT_LOCAL_PLAYERS; k++) client_session_clear_windows(&probes[k]);
		clin_force_world_reset();
		svin_process_messages(discard_rpc, NULL, false);
	}
	client_session_clear_windows(&gstate.players[0]);
	dict_entity_it_t it;
	dict_entity_it(it, gstate.entities);
	while(!dict_entity_end_p(it)) { free(dict_entity_ref(it)->value); dict_entity_next(it); }
	dict_entity_clear(gstate.entities);
	gstate.players[0].local_player = NULL;
	world_destroy(&gstate.world);
	gstate.remote_session = false;
	puts("PASS actual remote window close, provider disconnect, death/respawn epoch and world unload: predictions retired, plan leases reusable/released");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init(); items_init(); recipe_init();
	assert(svin_init()); assert(clin_init());
	assert(server_remote_init(&remote));
	((struct server_remote_impl*)remote.impl)->state = REMOTE_PLAY;
	full_plans(); admission(); generation_and_interruption(); rejection_timeout(); hooks_and_adapters();
	fifo_and_rejection_indices(); lifecycle_transitions();
	server_remote_destroy(&remote);
	puts("PASS test_sort_session");
	return 0;
}
