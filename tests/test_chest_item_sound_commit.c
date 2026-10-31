/* Requirement 3 of the chest sound corrections: the approved item cue is
 * COMMIT-timed, not prediction-timed.
 *
 * docs/ui_design/approved/specs/sound-events.md, Inventory/furnace/dispenser
 * row: "Item cue once after a successful pickup, placement, split-one or quick
 * move; no second confirmation for the same transfer. Empty slots, blocked
 * moves, failed safe-close attempts and capacity failures are silent."
 * Runtime rule: "Actual transfer/save success hooks must come from
 * authoritative completed operations."
 *
 * Every cue here is produced by the REAL container_menu step against a REAL
 * window_container; only audio_menu_event is linker-wrapped, so the prediction,
 * the Packet102 submit and the authoritative reply route
 * (client_session_window_reply -> windowc_action_apply_result) are the shipping
 * ones. Before this lane the confirmation cue fired straight off the local
 * prediction inside the send branch, so every assertion below that separates
 * "submitted" from "committed" fails on the base tree. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "game/gui/container_menu.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "platform/menu_sound.h"

static enum menu_sound_event sounds[64];
static size_t sound_count;
void __wrap_audio_menu_event(enum menu_sound_event e, ptime_t now) {
	(void)now;
	assert(sound_count < sizeof(sounds) / sizeof(*sounds));
	sounds[sound_count++] = e;
}

static struct server_rpc sent[32];
static size_t sent_count;
static bool send_fails;
static bool send_click(void* user, struct server_rpc* call) {
	(void)user;
	if(send_fails) return false;
	assert(sent_count < sizeof(sent) / sizeof(*sent));
	sent[sent_count++] = *call;
	return true;
}

static struct window_container wc;
static struct window_container craft;
static struct window_container player;
static struct client_session session;
static struct entity body;
static struct container_menu menu;
/* Two chest storage slots and one player-main slot: enough for a pick, a
 * split-one placement and a quick move without any layout dependency. */
static const struct container_slot slots[] = {{0, 0, 0}, {36, 0, 1}, {72, 0, 27}};

static void step(struct container_input input) {
	container_menu_step(&menu, slots, 3, 0, 0, &wc, WINDOWC_CHEST,
						&input, .016F, send_click, NULL);
}
static void idle(void) { step((struct container_input){0}); }

/* The authoritative half: exactly the CRPC the client's reply pump delivers. */
static void transaction_on(uint8_t window, uint16_t action, bool accepted) {
	struct client_rpc call = {.type = CRPC_WINDOW_TRANSACTION,
		.payload.window_transaction = {.window = window,
			.action_id = action, .accepted = accepted}};
	assert(client_session_window_reply(&session, &call));
}
static void transaction(uint16_t action, bool accepted) {
	transaction_on(WINDOWC_CHEST, action, accepted);
}

static uint16_t action_at(size_t index) {
	assert(index < sent_count);
	const struct server_rpc* call = &sent[index];
	assert(call->type == SRPC_WINDOW_CLICK);
	return call->payload.window_click.action_id;
}
static uint16_t last_action(void) {
	assert(sent_count);
	return action_at(sent_count - 1);
}

static void reset_window(void) {
	windowc_discard_pending_actions(&wc);
	inventory_clear(windowc_get_latest(&wc));
	menu = (struct container_menu){0};
	sound_count = sent_count = 0;
	send_fails = false;
}

/* A committed transfer: one item cue, at the reply, never before. */
static void test_pick_commits(void) {
	reset_window();
	windowc_slot_change(&wc, 1, (struct item_data){.id = 1, .count = 4});
	menu.selected = 1;
	step((struct container_input){.pick = true});
	assert(sent_count == 1);
	assert(windowc_get_latest(&wc)->picked_item.count == 4); /* predicted */
	assert(!sound_count); /* submitted is not committed */
	idle();
	idle();
	assert(!sound_count); /* still only predicted, however long we wait */
	transaction(last_action(), true);
	idle();
	assert(sound_count == 1 && sounds[0] == MENU_SOUND_ITEM);
	idle();
	idle();
	assert(sound_count == 1); /* "no second confirmation for the same transfer" */
	puts("PASS item cue: silent on submit, one cue on the authoritative accept");
}

/* Split-one placement out of the held stack -- the same commit timing. */
static void test_split_commits(void) {
	reset_window();
	windowc_slot_change(&wc, 1, (struct item_data){.id = 1, .count = 4});
	menu.selected = 1;
	step((struct container_input){.pick = true});
	transaction(last_action(), true);
	idle();
	assert(sound_count == 1 && sounds[0] == MENU_SOUND_ITEM);
	menu.selected = 0;
	step((struct container_input){.split = true});
	assert(sent_count == 2 && sent[1].payload.window_click.right_click);
	assert(windowc_get_latest(&wc)->items[0].count == 1); /* one placed */
	assert(sound_count == 1); /* the placement is not confirmed yet */
	transaction(last_action(), true);
	idle();
	assert(sound_count == 2 && sounds[1] == MENU_SOUND_ITEM);
	puts("PASS item cue: split-one placement confirmed only by its own accept");
}

/* Quick move: the client predicts the whole transfer, the cue still waits. */
static void test_quick_move_commits(void) {
	reset_window();
	windowc_slot_change(&wc, 0, (struct item_data){.id = 1, .count = 4});
	menu.selected = 0;
	step((struct container_input){.quick_move = true});
	assert(sent_count == 1 && sent[0].payload.window_click.quick_move);
	assert(!windowc_get_latest(&wc)->items[0].id); /* predicted away */
	assert(wc.quick_pending && !sound_count);
	idle();
	assert(!sound_count);
	transaction(last_action(), true);
	assert(!wc.quick_pending);
	idle();
	assert(sound_count == 1 && sounds[0] == MENU_SOUND_ITEM);
	puts("PASS item cue: quick move confirmed at the reply, not at the prediction");
}

/* A rejected transfer is a failed action: silent, forever. */
static void test_rejection_silent(void) {
	reset_window();
	windowc_slot_change(&wc, 1, (struct item_data){.id = 1, .count = 4});
	menu.selected = 1;
	step((struct container_input){.pick = true});
	assert(sent_count == 1 && !sound_count);
	uint32_t serial = wc.rejection_serial;
	transaction(last_action(), false);
	assert(wc.rejection_serial == serial + 1 && wc.correction_pending);
	idle();
	idle();
	assert(!sound_count);
	puts("PASS item cue: a rejected transfer is silent, on the reply and after");
}

/* Nothing moved and nothing sent: both stay silent. */
static void test_no_op_and_send_failure_silent(void) {
	reset_window();
	menu.selected = 1; /* empty slot, empty hand */
	step((struct container_input){.pick = true});
	assert(!sound_count);
	/* An action that predicts nothing must not even be armed: an accepted
	 * reply for it can never be mistaken for a completed transfer. */
	if(sent_count) transaction(last_action(), true);
	idle();
	assert(!sound_count);

	reset_window();
	windowc_slot_change(&wc, 1, (struct item_data){.id = 1, .count = 4});
	menu.selected = 1;
	send_fails = true;
	step((struct container_input){.pick = true});
	assert(!sent_count && menu.send_failed && !sound_count);
	send_fails = false;
	idle();
	assert(!sound_count);
	puts("PASS item cue: empty-slot clicks and failed sends stay silent");
}

/* A window handoff retires the prediction without an accept: still silent. */
static void test_discard_silent(void) {
	reset_window();
	windowc_slot_change(&wc, 1, (struct item_data){.id = 1, .count = 4});
	menu.selected = 1;
	step((struct container_input){.pick = true});
	assert(sent_count == 1 && !sound_count);
	windowc_discard_pending_actions(&wc);
	idle();
	idle();
	assert(!sound_count);
	puts("PASS item cue: a discarded prediction never confirms a transfer");
}

/* Fix round finding 2: nothing gates ordinary click pipelining, so two rapid
 * clicks can both be in flight. Each owes its OWN cue at its OWN accept; the
 * single pre-fix arm was overwritten by the second click and the first accept
 * played nothing. (The dispatcher's 90 ms item guard may still collapse the
 * pair for the ear -- audio_menu_event is wrapped here, so this counts the
 * EVENTS the menu declares, which is exactly what that guard is entitled to
 * filter afterwards.) */
static void test_pipelined_clicks_each_get_a_cue(void) {
	reset_window();
	windowc_slot_change(&wc, 1, (struct item_data){.id = 1, .count = 4});
	menu.selected = 1;
	step((struct container_input){.pick = true});	/* take the stack */
	step((struct container_input){.pick = true});	/* put it straight back */
	assert(sent_count == 2 && action_at(0) != action_at(1));
	assert(!sound_count); /* two submitted, none committed */
	transaction(action_at(0), true);
	idle();
	assert(sound_count == 1 && sounds[0] == MENU_SOUND_ITEM);
	transaction(action_at(1), true);
	idle();
	assert(sound_count == 2 && sounds[1] == MENU_SOUND_ITEM);
	assert(!menu.item_cue_dropped);
	puts("PASS item cue: two pipelined transfers each keep their own cue");

	/* One of the pair rejected: the committed one still speaks, the rejected
	 * one stays silent, on the reply and after. */
	reset_window();
	windowc_slot_change(&wc, 1, (struct item_data){.id = 1, .count = 4});
	menu.selected = 1;
	step((struct container_input){.pick = true});
	step((struct container_input){.pick = true});
	assert(sent_count == 2);
	transaction(action_at(0), true);
	idle();
	assert(sound_count == 1 && sounds[0] == MENU_SOUND_ITEM);
	transaction(action_at(1), false);
	idle();
	idle();
	assert(sound_count == 1);
	puts("PASS item cue: a rejected member of a pipelined pair adds no cue");
}

/* Fix round finding 3: the session recreates the window container on
 * CRPC_OPEN_WINDOW/REOPEN (client_session_local_reply), which restarts
 * rejection_serial AND next_action_id at zero, while the SCREEN's struct
 * container_menu lives on. A stale arm therefore met an id/serial pair it
 * matched exactly and fired a spurious cue for an unrelated action. The arm
 * now carries the window generation, which windowc_create bumps. */
static void test_window_reset_retires_the_arm(void) {
	reset_window();
	/* A freshly opened window has never been rejected, so its serial is zero
	 * -- the same value windowc_create hands the REPLACEMENT container. That
	 * equality is the whole point: the serial guard cannot see this reset, so
	 * only the generation can. (Earlier tests in this file left the counter
	 * non-zero; without this the arm would be retired for the wrong reason and
	 * the case below would prove nothing.) */
	wc.rejection_serial = 0;
	windowc_slot_change(&wc, 1, (struct item_data){.id = 1, .count = 4});
	menu.selected = 1;
	step((struct container_input){.pick = true});
	uint16_t armed = last_action();
	assert(sent_count == 1 && !sound_count);

	/* Exactly what the session does to the visible window on a (re)open. */
	uint32_t generation = wc.generation;
	windowc_destroy(&wc);
	assert(windowc_create(&wc, WINDOW_TYPE_CHEST, 63));
	assert(wc.generation != generation && !wc.rejection_serial);

	/* In the field the restart to zero is what makes the ids collide; here the
	 * counter is wound to the armed id directly so the collision is exact and
	 * not a function of how many actions earlier tests happened to spend. */
	wc.next_action_id = armed;
	uint16_t reused;
	assert(windowc_new_action(&wc, &reused, false, 1));
	assert(reused == armed);
	transaction(reused, true);
	idle();
	idle();
	assert(!sound_count);
	puts("PASS item cue: a window reset retires the arm, matching ids and all");
}

/* Fix round finding 1: the base tree played the approved confirmation cue for
 * EVERY committed transfer, the crafting RESULT slot included; making the
 * whole path ITEM silently dropped the craft confirmation. sound-events.md,
 * inventory/recipe page row: "Keep the approved crafting cues and recipes
 * unchanged"; crafting-chests.md: "Sound only on a real selection/menu change
 * or completed successful craft/sort." The result slot -- and only it -- keeps
 * SELECT, and it is commit-timed like everything else. */
static void test_craft_output_keeps_the_confirmation_cue(void) {
	menu = (struct container_menu){0};
	sound_count = sent_count = 0;
	send_fails = false;
	windowc_slot_change(&craft, CRAFTING_SLOT_OUTPUT,
						(struct item_data){.id = 1, .count = 4});
	static const struct container_slot craft_slots[]
		= {{0, 0, CRAFTING_SLOT_OUTPUT}, {36, 0, CRAFTING_SLOT_MAIN}};
	struct container_input press = {.pick = true};
	struct container_input none = {0};
	menu.selected = 0;
	container_menu_step(&menu, craft_slots, 2, 0, 0, &craft, WINDOWC_CRAFTING,
						&press, .016F, send_click, NULL);
	assert(sent_count == 1 && !sound_count); /* commit-timed like every other */
	transaction_on(WINDOWC_CRAFTING, last_action(), true);
	container_menu_step(&menu, craft_slots, 2, 0, 0, &craft, WINDOWC_CRAFTING,
						&none, .016F, send_click, NULL);
	assert(sound_count == 1 && sounds[0] == MENU_SOUND_SELECT);

	/* The same window, an ordinary slot: the item cue, never SELECT. */
	menu.selected = 1;
	container_menu_step(&menu, craft_slots, 2, 0, 0, &craft, WINDOWC_CRAFTING,
						&press, .016F, send_click, NULL);
	assert(sent_count == 2);
	transaction_on(WINDOWC_CRAFTING, last_action(), true);
	container_menu_step(&menu, craft_slots, 2, 0, 0, &craft, WINDOWC_CRAFTING,
						&none, .016F, send_click, NULL);
	assert(sound_count == 2 && sounds[1] == MENU_SOUND_ITEM);
	puts("PASS craft cue: the result slot keeps SELECT, other slots play ITEM");
}

/* critique-1028 r2: the 5 s reply deadline commits an unanswered transfer
 * (windowc_commit_pending) without any server confirmation. That must never
 * play the success cue; only a real Packet106 accept may, even a late one,
 * and a late rejection stays silent. */
static void test_reply_deadline_is_not_an_accept(void) {
	for(int late = 0; late < 2; late++) {
		reset_window();
		windowc_slot_change(&wc, 1, (struct item_data){.id = 1, .count = 4});
		menu.selected = 1;
		step((struct container_input){.pick = true});
		assert(sent_count == 1 && !sound_count);
		uint16_t action = last_action();
		ptime_t t0 = time_get();
		assert(!windowc_expire_unanswered(&wc, t0));
		assert(windowc_expire_unanswered(&wc, time_add_ms(t0, WINDOWC_REPLY_TIMEOUT_MS)));
		assert(!windowc_actions_pending(&wc)
			&& windowc_get_latest(&wc)->picked_item.count == 4); /* committed */
		idle();
		idle();
		assert(!sound_count); /* expiry is not an accept */
		uint32_t serial = wc.rejection_serial;
		transaction(action, late == 1);
		idle();
		idle();
		if(late == 1) {
			assert(wc.rejection_serial == serial);
			assert(sound_count == 1 && sounds[0] == MENU_SOUND_ITEM); /* late accept */
		} else {
			assert(wc.rejection_serial == serial + 1);
			assert(!sound_count); /* late reject: silent */
		}
	}
	puts("PASS item cue: reply-deadline commit is silent; late accept cues, late reject stays silent");
}

int main(void) {
	blocks_init();
	items_init();
	assert(windowc_create(&player, WINDOW_TYPE_INVENTORY, 45));
	assert(windowc_create(&wc, WINDOW_TYPE_CHEST, 63));
	assert(windowc_create(&craft, WINDOW_TYPE_WORKBENCH, CRAFTING_SIZE));
	gstate.players[0].windows[WINDOWC_INVENTORY] = &player;
	gstate.players[0].windows[WINDOWC_CHEST] = &wc;
	gstate.players[0].windows[WINDOWC_CRAFTING] = &craft;
	session.local_player = &body;
	session.visible_window = WINDOWC_CHEST;
	session.windows[WINDOWC_CHEST] = &wc;
	session.windows[WINDOWC_CRAFTING] = &craft;

	test_pick_commits();
	test_split_commits();
	test_quick_move_commits();
	test_rejection_silent();
	test_no_op_and_send_failure_silent();
	test_discard_silent();
	test_pipelined_clicks_each_get_a_cue();
	test_window_reset_retires_the_arm();
	test_craft_output_keeps_the_confirmation_cue();
	test_reply_deadline_is_not_an_accept();

	session.windows[WINDOWC_CHEST] = NULL;
	session.windows[WINDOWC_CRAFTING] = NULL;
	windowc_destroy(&craft);
	windowc_destroy(&wc);
	windowc_destroy(&player);
	puts("PASS chest item cue is commit-timed for pick, split-one and quick move");
	return 0;
}
