/* Requirements 1 and 4 of the chest sound corrections: a SUCCESSFULLY COMMITTED
 * sort plays the approved confirmation cue exactly once, and nothing else in the
 * sort lifecycle makes a sound.
 *
 * docs/ui_design/approved/specs/crafting-chests.md:27: "Sound only on a real
 * selection/menu change or completed successful craft/sort. Failed actions,
 * unchanged boundaries, redraws and stew animation are silent."
 * docs/ui_design/approved/specs/sound-events.md runtime rule: "Actual
 * transfer/save success hooks must come from authoritative completed
 * operations."  crafting-chests.md:20: "Another player viewing the same chest
 * blocks chest sorting."
 *
 * This drives the REAL executor (client_session_sort_tick) against the REAL
 * local server (server_local_process_window_for) -- the same harness shape as
 * test_sort_session -- so "committed" means the server accepted every click of
 * the plan and the reconciled window equals the plan's target. Only
 * audio_menu_event is linker-wrapped. On the base tree completion was
 * deliberately silent, so test_completed_sort_confirms below fails there. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "item/inventory_sort.h"
#include "item/items.h"
#include "item/recipe.h"
#include "network/client_interface.h"
#include "network/inventory_chest_shared.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "platform/menu_sound.h"

static enum menu_sound_event sounds[64];
static size_t sound_count;
void __wrap_audio_menu_event(enum menu_sound_event e, ptime_t now) {
	(void)now;
	assert(sound_count < sizeof(sounds) / sizeof(*sounds));
	sounds[sound_count++] = e;
}

static struct server_local server;
static struct client_session session;	   /* the acting player */
static struct client_session bystander;	   /* a second local player, idle */
static struct entity body, bystander_body;
static struct inventory storage;		   /* the acting player's chest view */
static struct inventory second_view;	   /* another player's view of the SAME chest */
static struct window_container wc;
static struct window_container bystander_wc;
static struct server_rpc queue[INVENTORY_SORT_MAX_CLICKS + 16];
static size_t head, tail;
static ptime_t now;
static bool reject_clicks;

static bool send_click(void* user, struct server_rpc* call) {
	(void)user;
	assert(tail < sizeof(queue) / sizeof(*queue));
	queue[tail++] = *call;
	return true;
}
static void reply(const struct client_rpc* call, void* user) {
	(void)user;
	assert(client_session_window_reply(&session, call));
}
static void discard_rpc(struct server_rpc* call, void* user) { (void)call; (void)user; }

static void process_one(void) {
	assert(head < tail);
	if(reject_clicks && queue[head].type == SRPC_WINDOW_CLICK) {
		/* An authoritative rejection, exactly as NetClientHandler sees it. */
		struct client_rpc call = {.type = CRPC_WINDOW_TRANSACTION,
			.payload.window_transaction = {.window = WINDOWC_CHEST,
				.action_id = queue[head].payload.window_click.action_id,
				.accepted = false}};
		head++;
		assert(client_session_window_reply(&session, &call));
		return;
	}
	server_local_process_window_for(&server, &server.player, &queue[head++]);
}

/* One executor step for BOTH local players. The bystander never sorts, so it
 * must never contribute a cue: menu cues carry no player identity, so the only
 * thing that attributes one is which session's window actually committed. */
static void tick(void) {
	client_session_sort_tick(&session, WINDOWC_CHEST, now, send_click, NULL);
	client_session_sort_tick(&bystander, WINDOWC_CHEST, now, send_click, NULL);
	now = time_add_ms(now, 1);
}

static void run_until_settled(void) {
	unsigned loops = 0;
	while(wc.sort.status == WINDOW_SORT_RUNNING
		  || wc.sort.status == WINDOW_SORT_ADMISSION || head < tail) {
		tick();
		if(head < tail) process_one();
		assert(++loops < 4000);
	}
}

static void fill_chest(void) {
	for(size_t k = 0; k < wc.slot_count; k++) {
		/* Split identities across the storage range so a group sort has real
		 * work to do; the player half stays a plain full bag. */
		struct item_data item = k < 27
			? (struct item_data){.id = k % 2 ? 1 : 3, .count = k % 2 ? 16 : 32}
			: (struct item_data){.id = 3, .count = 64};
		storage.items[k] = item;
		windowc_slot_change(&wc, k, item);
	}
	wc.sort.status = WINDOW_SORT_IDLE;
}

static void setup(void) {
	memset(&server, 0, sizeof(server));
	memset(&session, 0, sizeof(session));
	memset(&bystander, 0, sizeof(bystander));
	head = tail = 0;
	now = time_get();
	reject_clicks = false;
	sound_count = 0;
	assert(windowc_create(&wc, WINDOW_TYPE_CHEST, 63));
	assert(windowc_create(&bystander_wc, WINDOW_TYPE_CHEST, 63));
	assert(inventory_create(&storage, NULL, NULL, NULL, 63, 0, 0, 0));
	assert(inventory_create(&second_view, NULL, NULL, NULL, 63, 0, 0, 0));
	storage.logic = second_view.logic = &inventory_logic_chest;
	storage.owner = &server.player;
	storage.user = second_view.user = &server;
	storage.reply = reply;
	server.player.active_inventory = &storage;
	server.player.health = 160;
	server.player.has_pos = server.player.finished_loading = true;
	server.chest_views = &storage;
	storage.shared_next = NULL; /* the acting player is the only viewer */
	session.local_player = &body;
	session.visible_window = WINDOWC_CHEST;
	session.windows[WINDOWC_CHEST] = &wc;
	bystander.local_player = &bystander_body;
	bystander.visible_window = WINDOWC_CHEST;
	bystander.windows[WINDOWC_CHEST] = &bystander_wc;
	fill_chest();
}

static void teardown(void) {
	storage.logic = second_view.logic = NULL;
	inventory_destroy(&storage, NULL);
	inventory_destroy(&second_view, NULL);
	session.windows[WINDOWC_CHEST] = NULL;
	bystander.windows[WINDOWC_CHEST] = NULL;
	windowc_destroy(&wc);
	windowc_destroy(&bystander_wc);
	client_session_clear_windows(&session);
	client_session_clear_windows(&bystander);
	svin_process_messages(discard_rpc, NULL, false);
}

/* The requirement-1 case: one cue, and only at the committing reply. */
static void test_completed_sort_confirms(void) {
	setup();
	assert(client_session_sort_request(&session, WINDOWC_CHEST,
		INVENTORY_SORT_CHEST_GROUP, false, now, send_click, NULL));
	assert(wc.sort.status == WINDOW_SORT_ADMISSION);
	assert(!sound_count); /* submitting a sort is not completing one */
	unsigned loops = 0;
	while(wc.sort.status == WINDOW_SORT_RUNNING
		  || wc.sort.status == WINDOW_SORT_ADMISSION) {
		/* Every intermediate state -- admitted, submitted, partially
		 * acknowledged -- is silent until the whole plan commits. */
		assert(!sound_count);
		tick();
		if(head < tail) process_one();
		assert(++loops < 4000);
	}
	assert(wc.sort.status == WINDOW_SORT_COMPLETE);
	assert(sound_count == 1 && sounds[0] == MENU_SOUND_SELECT);
	/* Ticking on does not re-announce it. */
	for(int k = 0; k < 8; k++) tick();
	assert(sound_count == 1);
	/* And the sort really did commit: server and client agree on the target. */
	assert(!memcmp(storage.items, wc.sort.plan->target,
				   wc.slot_count * sizeof(struct item_data)));
	assert(!memcmp(windowc_get_latest(&wc)->items, wc.sort.plan->target,
				   wc.slot_count * sizeof(struct item_data)));
	teardown();
	puts("PASS sort completion: one confirmation cue, at the committing reply");
}

/* A rejected sort is a failed action: silent. */
static void test_rejected_sort_silent(void) {
	setup();
	reject_clicks = true;
	assert(client_session_sort_request(&session, WINDOWC_CHEST,
		INVENTORY_SORT_CHEST_GROUP, false, now, send_click, NULL));
	run_until_settled();
	assert(wc.sort.status == WINDOW_SORT_INTERRUPTED);
	assert(!sound_count);
	teardown();
	puts("PASS sort rejection: an interrupted sort is silent");
}

/* An interrupted (player-touched) sort is silent too. */
static void test_interrupted_sort_silent(void) {
	setup();
	assert(client_session_sort_request(&session, WINDOWC_CHEST,
		INVENTORY_SORT_CHEST_GROUP, false, now, send_click, NULL));
	tick();
	if(head < tail) process_one();
	tick();
	windowc_sort_interrupt(&wc);
	run_until_settled();
	assert(wc.sort.status != WINDOW_SORT_COMPLETE);
	assert(!sound_count);
	teardown();
	puts("PASS sort interruption: a sort the player broke off is silent");
}

/* An already-sorted chest produces no plan and therefore no cue: the spec's
 * "unchanged boundaries ... are silent". */
static void test_unchanged_sort_silent(void) {
	setup();
	assert(client_session_sort_request(&session, WINDOWC_CHEST,
		INVENTORY_SORT_CHEST_GROUP, false, now, send_click, NULL));
	run_until_settled();
	assert(wc.sort.status == WINDOW_SORT_COMPLETE && sound_count == 1);
	wc.sort.status = WINDOW_SORT_IDLE;
	/* The same operation on the now-sorted chest changes nothing. */
	assert(client_session_sort_request(&session, WINDOWC_CHEST,
		INVENTORY_SORT_CHEST_GROUP, false, now, send_click, NULL));
	assert(wc.sort.status == WINDOW_SORT_IDLE);
	run_until_settled();
	assert(sound_count == 1);
	teardown();
	puts("PASS unchanged sort: a no-op sort adds no cue");
}

/* Split screen: a second player viewing the same chest blocks the sort, and a
 * blocked sort is silent. The bystander session, which never sorts, never
 * contributes a cue either -- the cue belongs to the session that committed. */
static void test_second_viewer_blocked_is_silent(void) {
	setup();
	/* Another player's view of the SAME chest block, registered exactly as the
	 * server registers a second opener. */
	second_view.x = storage.x;
	second_view.y = storage.y;
	second_view.z = storage.z;
	storage.shared_next = &second_view;
	assert(!inventory_chest_sort_admitted(&storage));
	assert(client_session_sort_request(&session, WINDOWC_CHEST,
		INVENTORY_SORT_CHEST_GROUP, false, now, send_click, NULL));
	assert(wc.sort.status == WINDOW_SORT_ADMISSION);
	run_until_settled();
	assert(wc.sort.status == WINDOW_SORT_INTERRUPTED); /* admission refused */
	assert(!sound_count);
	assert(bystander_wc.sort.status == WINDOW_SORT_IDLE);

	/* The other viewer closes; the acting player's sort now commits, and the
	 * single cue is the acting session's. */
	storage.shared_next = NULL;
	wc.sort.status = WINDOW_SORT_IDLE;
	windowc_discard_pending_actions(&wc);
	fill_chest();
	assert(client_session_sort_request(&session, WINDOWC_CHEST,
		INVENTORY_SORT_CHEST_GROUP, false, now, send_click, NULL));
	run_until_settled();
	assert(wc.sort.status == WINDOW_SORT_COMPLETE);
	assert(sound_count == 1 && sounds[0] == MENU_SOUND_SELECT);
	assert(bystander_wc.sort.status == WINDOW_SORT_IDLE);
	teardown();
	puts("PASS split screen: the blocked viewer is silent; one cue for the actor");
}

int main(void) {
	blocks_init();
	items_init();
	recipe_init();
	test_completed_sort_confirms();
	test_rejected_sort_silent();
	test_interrupted_sort_silent();
	test_unchanged_sort_silent();
	test_second_viewer_blocked_is_silent();
	puts("PASS sort completion cue is commit-timed, once, and silent otherwise");
	return 0;
}
