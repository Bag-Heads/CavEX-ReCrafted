/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924/A1d, P2 inventory row #5: guest WINDOW requests
 * during the sleep / death / respawn-plan / world barriers, owner-swapped.
 *
 * Beta citations (client Jad decompile,
 * ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   - EntityPlayer.isMovementBlocked, EntityPlayer.java:127-129:
 *     `return this.health <= 0 || this.isPlayerSleeping();` -- the sleep GUI
 *     and the death screen both take the input; while asleep no inventory
 *     screen can be opened or clicked.
 *   - EntityLiving.onLivingUpdate, EntityLiving.java:618-622 zeroes the input
 *     while isMovementBlocked holds.
 *   - Server: EntityPlayer.readEntityFromNBT, EntityPlayer.java:344-350
 *     (/c/tmp/mcb173-server-decompile): a player saved asleep is woken at load
 *     (`wakeUpPlayer(true, true, false)`), so "sleeping" never survives a
 *     respawn/reload -- the fresh body starts with no window barrier of its own.
 *
 * The HOST enforces the window barriers ahead of its SRPC switch
 * (server_local.c server_local_process): a world switch / death eviction /
 * outstanding input hand-off drops EVERY request; sleeping drops
 * SRPC_WINDOW_CLICK (server_rpc_blocked_while_sleeping); a respawn plan in
 * flight (server_local_host_respawn_active, any non-IDLE phase) drops it too
 * (server_rpc_blocked_while_host_respawning); the DEAD body's window clicks
 * still FLOW (deviation-#18 container-open dwell, server_rpc_blocked_while_dead
 * is deliberately narrower). The GUEST route dispatches straight into
 * server_local_process_window_for, which re-derives the sleeping/dead/world
 * legs from the acting actor but has NO respawn-plan leg -- C2inv row #5's
 * "weaker". RED on the base: a PREPARED guest respawn plan still lets the
 * corpse's window click land.
 *
 * Two ctest names, one executable (argv[1]):
 *   base  -- the legs both owners already agree on (sleep, world switch, dead
 *            dwell): passes on the base, stays a normal test.
 *   red   -- the respawn-plan leg: host reference PASSES, guest is RED.
 *            Registered WILL_FAIL until the A1d wiring (after G1). */
#include "support/owner_swap_harness.h"

#define TEST_NAME "test_owner_swap_window_state_barriers"

/* One ordinary pickup click on the player inventory's first main slot: with a
 * stack there and an empty cursor, a processed click moves the stack onto the
 * cursor (picked_item); a dropped click leaves both as they were. */
static void arm_slot(struct server_player* p) {
	p->inventory.items[INVENTORY_SLOT_MAIN]
		= (struct item_data) {.id = BLOCK_STONE, .count = 3};
	p->inventory.picked_item = (struct item_data) {0};
	p->inventory.transaction_pending = false;
}
static struct server_rpc pickup_click(struct server_player* p, uint16_t action) {
	return (struct server_rpc) {.type = SRPC_WINDOW_CLICK,
		.payload.window_click = {.window = WINDOWC_INVENTORY,
			.slot = INVENTORY_SLOT_MAIN, .action_id = action,
			.expected = p->inventory.items[INVENTORY_SLOT_MAIN]}};
}
static bool click_landed(const struct server_player* p) {
	return p->inventory.picked_item.id == BLOCK_STONE
		&& p->inventory.picked_item.count == 3
		&& p->inventory.items[INVENTORY_SLOT_MAIN].id == 0;
}
static bool click_dropped(const struct server_player* p) {
	return p->inventory.picked_item.id == 0
		&& p->inventory.items[INVENTORY_SLOT_MAIN].id == BLOCK_STONE
		&& p->inventory.items[INVENTORY_SLOT_MAIN].count == 3;
}
static uint16_t action_seq = 100;
static void host_click(struct server_local* s) {
	struct server_rpc call = pickup_click(&s->player, ++action_seq);
	host_send(s, &call);
}
static void guest_click(struct server_local* s) {
	struct server_rpc call = pickup_click(s->guest_actor, ++action_seq);
	check(guest_send(s, &call), "queue: the guest's WINDOW_CLICK is admitted");
}

/* ======================================================================== */
static void case_base(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;

	/* Baseline: awake, alive, IDLE -- the click lands for both owners, so
	 * every drop below is a barrier and not a dead route. */
	arm_slot(h); host_click(s);
	check(click_landed(h), "baseline: awake live HOST click picks the stack up");
	arm_slot(g); guest_click(s);
	check(click_landed(g), "baseline: awake live GUEST click picks the stack up");

	/* --- sleeping: server_rpc_blocked_while_sleeping(WINDOW_CLICK) -------- */
	arm_slot(h); h->sleeping = true; host_click(s);
	check(click_dropped(h),
		"host: SLEEPING drops WINDOW_CLICK (EntityPlayer.isMovementBlocked, "
		"EntityPlayer.java:128)");
	h->sleeping = false;
	arm_slot(g); g->sleeping = true; guest_click(s);
	check(click_dropped(g),
		"guest: SLEEPING drops WINDOW_CLICK the same way "
		"(server_local_process_window_for's own actor leg)");
	g->sleeping = false;
	guest_click(s);
	check(click_landed(g), "guest: the click after waking lands again");

	/* --- world switch in flight: a SERVER-WIDE barrier for both owners ---- */
	s->world_switch_phase = SERVER_WORLD_SWITCH_PORTAL;
	arm_slot(h); host_click(s);
	check(click_dropped(h), "host: a world switch in flight drops WINDOW_CLICK");
	arm_slot(g); guest_click(s);
	check(click_dropped(g), "guest: the same world switch drops its WINDOW_CLICK");
	s->world_switch_phase = SERVER_WORLD_SWITCH_IDLE;

	/* --- dead: the container-open dwell keeps window clicks FLOWING -------- */
	short hh = h->health, gh = g->health;
	arm_slot(h); h->health = 0; host_click(s);
	check(click_landed(h),
		"host: a DEAD body's window click still flows (deviation-#18 "
		"container-open dwell; server_rpc_blocked_while_dead excludes WINDOW_*)");
	h->health = hh;
	arm_slot(g); g->health = 0; guest_click(s);
	check(click_landed(g),
		"guest: the DEAD body's window click flows identically -- host parity, "
		"NOT a drop");
	g->health = gh;

	/* Both bodies are pristine again for teardown's checkpointed release. */
	arm_slot(h); h->inventory.items[INVENTORY_SLOT_MAIN] = (struct item_data) {0};
	arm_slot(g); g->inventory.items[INVENTORY_SLOT_MAIN] = (struct item_data) {0};
	teardown(s);
}

/* ======================================================================== */
static void case_red(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;

	arm_slot(h); host_click(s);
	check(click_landed(h), "baseline: awake live HOST click picks the stack up");
	arm_slot(g); guest_click(s);
	check(click_landed(g), "baseline: awake live GUEST click picks the stack up");

	/* --- host reference: a respawn plan in flight drops the window click -- */
	arm_slot(h);
	s->host_respawn_phase = HOST_RESPAWN_PHASE_PREPARED;
	host_click(s);
	check(click_dropped(h),
		"host: a PREPARED respawn plan drops WINDOW_CLICK "
		"(server_rpc_blocked_while_host_respawning)");
	s->host_respawn_phase = HOST_RESPAWN_PHASE_LOADING;
	host_click(s);
	check(click_dropped(h), "host: the LOADING phase drops it too");
	s->host_respawn_phase = HOST_RESPAWN_PHASE_IDLE;
	host_click(s);
	check(click_landed(h), "host: IDLE again -- the click lands");

	/* --- guest: reach PREPARED through the production path (the pattern of
	 * test_owner_swap_pose_dead_sleep_barrier): a REAL lethal hit, then
	 * SRPC_RESPAWN whose inventory close is made to fail once, so the plan is
	 * recorded but LOADING has not begun. ------------------------------------ */
	g->hurt_resist_time = 0;
	bool accepted = false;
	(void)server_local_damage_actor_result(s, g, MAX_PLAYER_HEALTH, 0.0f, 0.0f,
		&accepted);
	clin_update_drain();
	check(accepted && g->health == 0 && g->has_death_pos,
		"setup: guest killed through the production damage path");
	check(s->guest_respawn_phase == GUEST_RESPAWN_IDLE,
		"setup: no plan yet (IDLE + dead)");
	/* The corpse's window is the dwell case pinned in `base`: its click flows. */
	arm_slot(g); guest_click(s);
	check(click_landed(g), "guest: IDLE-phase corpse click flows (dwell parity)");

	g->inventory.picked_item = (struct item_data) {0};
	g->inventory.items[INVENTORY_SLOT_CRAFTING]
		= (struct item_data) {.id = ITEM_COAL, .count = 1};
	struct server_rpc respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = s->guest_generation,
			.respawn_epoch = 0}};
	server_local_test_fail_next_entity_alloc();
	assert(svin_rpc_send_from(guest_source(s), &respawn));
	drain_server(s);
	check(s->guest_respawn_phase == GUEST_RESPAWN_PREPARED && g->health == 0
		&& g->finished_loading,
		"setup: guest plan PREPARED (close failed once), body still dead and "
		"still finished_loading");
	arm_slot(g);
	guest_click(s);
	check(click_dropped(g),
		"guest: a PREPARED respawn plan DROPS the corpse's WINDOW_CLICK -- the "
		"old body's inventory may not change once the plan recorded its death "
		"(host: server_rpc_blocked_while_host_respawning)  [row #5: RED on base]");

	/* Retry -> LOADING: finished_loading is false, both routes drop. */
	g->inventory.picked_item = (struct item_data) {0};
	respawn.payload.respawn.respawn_epoch = 0;
	assert(svin_rpc_send_from(guest_source(s), &respawn));
	drain_server(s);
	check(s->guest_respawn_phase == GUEST_RESPAWN_LOADING && !g->finished_loading,
		"setup: retry reached LOADING");
	arm_slot(g);
	guest_click(s);
	check(click_dropped(g),
		"guest: the LOADING phase drops the click (mid-load leg, already shared)");

	/* Leave the fixture in a state the checkpointed release accepts: the
	 * plan is retired by finishing the respawn for real (exact READY ack on a
	 * published destination column, then the poll commits). */
	w_coord_t rcx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_x));
	w_coord_t rcz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_z));
	if(!server_world_is_chunk_loaded(&s->worlds[0], rcx, rcz)) {
		struct server_chunk* loaded = NULL;
		assert(server_world_load_chunk(&s->worlds[0], rcx, rcz, &loaded));
	}
	struct server_chunk* destination
		= dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(rcx, rcz));
	assert(destination);
	destination->sent = true;
	struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = s->guest_ticket,
			.action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
			.generation = s->guest_generation,
			.respawn_epoch = s->guest_respawn_epoch}};
	assert(svin_rpc_send_from(guest_source(s), &ready));
	drain_server(s);
	server_local_guest_respawn_poll(s);
	clin_update_drain();
	check(g->health == MAX_PLAYER_HEALTH && s->guest_respawn_phase == GUEST_RESPAWN_IDLE,
		"setup: guest respawn committed, plan retired");
	tick(s);
	arm_slot(g);
	guest_click(s);
	check(click_landed(g), "guest: the fresh body's click lands again");
	g->inventory.picked_item = (struct item_data) {0};
	g->inventory.items[INVENTORY_SLOT_MAIN] = (struct item_data) {0};
	h->inventory.picked_item = (struct item_data) {0};
	h->inventory.items[INVENTORY_SLOT_MAIN] = (struct item_data) {0};
	teardown(s);
}

/* ======================================================================== */
int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : NULL;
	if(!which || (strcmp(which, "base") && strcmp(which, "red"))) {
		fprintf(stderr, "usage: %s base|red  (one case per process)\n", argv[0]);
		return 2;
	}
	harness_init();
	if(!strcmp(which, "base"))
		case_base();
	else
		case_red();
	return harness_result(TEST_NAME, which);
}
