/* Real local guest archive + token-pool routing regressions. GPL-3.0-or-later. */
#ifndef CAVEWII_SERVER_LOCAL_TEST
#define CAVEWII_SERVER_LOCAL_TEST 1
#endif
#include <assert.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "support/test_fixture_dir.h"
#include "network/server_transfer.h"
#include "network/server_local_guest.h"
#include "network/server_local_state.h"
#include "network/server_local.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "platform/thread.h"
#include "platform/audio.h"
#include "game/game_state.h"
#include "item/items.h"
#include "item/recipe.h"
#include "block/blocks.h"
#include "block/redstone_piston.h" // RS_PISTON_BASE_ID
#include "entity/entity_fish.h"   // FISH_STUCK_DESPAWN_TICKS (A2 despawn)
#include "entity/entity_boat.h"   // entity_boat (R-S1a3 F5 zero-health cleanup)
#include "entity/entity_monster.h"
#include "entity/entity_minecart.h"
#include "entity/monsters_object.h"
#include "game/gui/screen_death.h"
#include "item/inventory_click.h" // inventory_click_post (A3 map craft)
#include "item/items/item_map.h" // item_map_cache_resident (A3 map craft)
#include "network/map_data.h"     // struct map_data (A3 map craft)
#include "network/client_admission.h"
#include "network/client_world_payload.h"
#include "game/client_world_context.h"
#include "column_observer.h"
#include "settle_observer.h"
#include "entity/combat.h"    // PLAYER_EYE_HEIGHT (guest-body obstruction cases)
static struct random_gen o16_world_before;
/* F4-RNG: body Entity.rand streams are java.util.Random (RNG ruling 8). */
static struct java_random o16_host_before, o16_guest_before;
/* draws from `from` (rand_gen_flt steps) until it equals `to`; -1 if > 64 */
static int o16_draws(struct random_gen from, const struct random_gen* to) {
	for(int k = 0; k <= 64; k++) {
		if(from.seed == to->seed)
			return k;
		(void)rand_gen_flt(&from);
	}
	return -1;
}
static int o16_jdraws(struct java_random from, const struct java_random* to) {
	for(int k = 0; k <= 64; k++) {
		if(from.seed == to->seed)
			return k;
		(void)jrand_next_float(&from);
	}
	return -1;
}
/* O16 (R06 P1): the guest's G6 death scatter draws ONLY from the guest's own
 * Random (2 per made drop); the host's own Random is untouched (independence).
 * G7: the ONE thrown crafting-grid close drop this retry makes is the
 * player's dropPlayerItem [EntityPlayer.java:290-315], so its 2 throw draws
 * now come from the GUEST's Random too, and the world stream does not move at
 * all (before G7 it advanced by exactly those 2 draws -- the residual owner
 * deviation this check used to pin). */
static void o16_guest_rng_check(const struct server_local* server,
								const struct server_player* guest,
								size_t death_drops) {
	int wd = o16_draws(o16_world_before, &server->rand_src);
	int gd = o16_jdraws(o16_guest_before, &guest->entity_rand.r);
	int hs = memcmp(&o16_host_before, &server->player.entity_rand.r,
					sizeof o16_host_before) == 0;
	fprintf(stderr, "O16 guest death: drops=%zu world_draws=%d guest_draws=%d "
			"host_unchanged=%d\n", death_drops, wd, gd, hs);
	/* O21: the close drop is Beta's dropPlayerItem -- 4 player-rand draws
	 * (EntityPlayer.java:305-315), not the engine's old 2-draw throw. */
	assert(gd == (int)(2 * death_drops + 4)
		   && "guest: 2 draws per death drop + 4 for the crafting-close "
			  "dropPlayerItem, all from the GUEST's own Random");
	assert(hs && "a guest death must not advance the host's own Random");
	assert(wd == 0 && "world stream: no player-owned drop draws from it");
}


/* A1b: fixture rows and hand-built windows name their world explicitly (the
 * slot-0 fallbacks for unstamped rows / unbound windows are gone). */
#define A1B_STAMP(s, kind, k, w) \
	((s)->te_owners.kind##_slot[k] = (w)->world_index, \
	 (s)->te_owners.kind##_gen[k] = (w)->generation)
#define A1B_BIND(player, w) ((player)->window_world = server_world_handle_of(w))
void server_local_test_damage_observer_reset(void);
bool server_local_test_damage_observer_get(const struct server_player** actor,
	enum server_player_damage_cause* cause);
bool server_local_test_apply_actor_fire_hazards(struct server_local*,
	struct server_player*, bool, bool, bool);
void server_local_test_apply_guest_hazards(struct server_local*);
#ifdef _WIN32
#include <direct.h>
#define MAKE_DIR(p) _mkdir(p)
#else
#define MAKE_DIR(p) mkdir(p, 0700)
#endif

#include "support/player_position_reference.h"
#include "../source/entity/combat.h" /* MOB_TARGET_RETENTION_TICKS,

                                     ARROW_OWNER_GRACE_TICKS,
                                     arrow_should_hit_actor (S1-a2) */

static void discard_request(struct server_rpc_source,struct server_rpc*,void*);

static void test_owned_body_updates(void) {
	/* Exercise every correction bit combination against the frozen old body
	 * algorithm. Nonzero interpolation, velocity, camera and ride state ensure
	 * a partial correction cannot quietly become a destructive full teleport. */
	unsigned cases = 0;
	struct game_state world_before;
	memcpy(&world_before, &gstate, sizeof(gstate));
	for(unsigned flags = 0; flags < 256; flags++)
	for(unsigned pose = 0; pose < 16; pose++) {
		struct entity actual = {0}, expected;
		actual.type = ENTITY_LOCAL_PLAYER;
		actual.teleport = entity_default_teleport;
		actual.health = 113;
		actual.on_ground = (pose & 1) != 0;
		for(int k = 0; k < 3; k++) {
			actual.pos[k] = (float)(pose * 3 + k) - 29.25F;
			actual.pos_old[k] = actual.pos[k] - 0.375F;
			actual.network_pos[k] = actual.pos[k] + 0.25F;
			actual.vel[k] = (float)(k - 1) * 0.1875F;
		}
		memcpy(&expected, &actual, sizeof(actual));
		struct client_session owner = {0}, reference;
		owner.local_player = &actual;
		owner.camera.rx = 1.125F;
		owner.camera.ry = 0.375F;
		owner.ride.camera_correction[0] = 2.5F;
		owner.oxygen = 19;
		owner.player_sync.state_known = true;
		owner.player_sync.state_correction_seq = 37;
		owner.player_sync.last_pos_update = time_add_ms((ptime_t){0}, 1234);
		memcpy(&reference, &owner, sizeof(owner));
		reference.local_player = &expected;
		struct client_rpc call = {.type = CRPC_PLAYER_POS,
			.payload.player_pos = {
				.position = {(float)pose - 8.5F, 64.25F, -(float)pose},
				.rotation = {(float)pose * 31.25F, (float)pose * 20.0F - 160.0F},
				.correction_seq = pose + 1, .correction_flags = flags}};
		struct client_rpc untouched;
		memcpy(&untouched, &call, sizeof(call));
		reference_player_position(&reference, &call);
		assert(clin_apply_player_position(&owner, &call));
		assert(!memcmp(&actual, &expected, sizeof(actual)));
		reference.local_player = &actual; /* compare the owned state, not fixture addresses */
		assert(!memcmp(&owner, &reference, sizeof(owner)));
		assert(!memcmp(&call, &untouched, sizeof(call)));
		cases++;
	}
	assert(!memcmp(&gstate, &world_before, sizeof(gstate)));
	struct client_session missing = {0}, before;
	memcpy(&before, &missing, sizeof(before));
	assert(!clin_apply_player_position(&missing, &(struct client_rpc){.type=CRPC_PLAYER_POS}));
	assert(!memcmp(&missing, &before, sizeof(before)));
	assert(!clin_apply_player_position(NULL, NULL));
	printf("PASS owned body corrections: %u byte-identical cases; no world/other-owner effects\n", cases);
}

static void test_guest_leave_deadline(void) {
	assert(svin_init());
	struct server_rpc pressure = {.type = SRPC_USE_ITEM};
	for(int k = 0; k < 80; k++) assert(svin_rpc_send(&pressure));
	struct client_session guest = {.local_ticket=41,.local_generation=73,
		.local_phase=CLIENT_GUEST_ATTACHED,.local_bound=true};
	assert(local_pause_open(&guest.pause,false));
	guest.pause.selected=3;
	assert(client_session_request_local_leave(&guest));
	client_session_flush_local(&guest);
	assert(guest.local_pending); /* the full action queue refused the send */
	guest.menu_host.recovery.phase=CONTROLLER_RECOVERY_LOST;
	client_session_local_leave_tick(&guest,guest.local_leave_deadline);
	assert(guest.local_phase==CLIENT_GUEST_LEAVING&&guest.local_bound
		&&guest.local_ticket==41&&guest.local_generation==73&&!guest.local_pending);
	assert(!guest.pause.exit_pending&&guest.pause.exit_failed);
	/* Wrong-generation success remains stale even after the deadline. */
	struct client_rpc left={.type=CRPC_LOCAL_GUEST,.payload.local_guest={
		.ticket=41,.generation=74,.result=LOCAL_GUEST_LEFT}};
	client_session_local_status(&guest,&left);
	assert(guest.local_phase==CLIENT_GUEST_LEAVING&&guest.local_bound);
	/* Retry is the real Pause activation path and re-arms the same request. */
	struct input_player_frame click={.connected=true,.category=INPUT_CAT_GAMECUBE};
	click.pressed[IB_GUI_CLICK]=true;
	(void)local_pause_update(&guest.pause,&guest,&click,0,0,640,240);
	assert(guest.pause.exit_pending&&!guest.pause.exit_failed&&guest.local_pending);
	/* Astra #4: expiry hands the wait to Retry; if the guest then DISMISSES the
	 * pause (Home/Back) before settlement, the next tick must take the
	 * transport retry back so recovery continues without a UI, and the
	 * following window must keep retrying too. */
	client_session_local_leave_tick(&guest,guest.local_leave_deadline);
	assert(guest.pause.exit_failed&&!guest.local_pending
		&&!guest.local_leave_deadline_armed&&guest.local_leave_retry_handed_off);
	struct input_player_frame home={.connected=true,.category=INPUT_CAT_GAMECUBE};
	home.pressed[IB_HOME]=true;
	assert(local_pause_update(&guest.pause,&guest,&home,0,0,640,240)==LOCAL_PAUSE_BACK);
	assert(!guest.pause.open);
	ptime_t dismissed=guest.local_leave_deadline;
	client_session_local_leave_tick(&guest,dismissed);
	assert(guest.local_phase==CLIENT_GUEST_LEAVING&&guest.local_bound
		&&guest.local_ticket==41&&guest.local_generation==73
		&&guest.local_pending&&guest.local_leave_deadline_armed
		&&!guest.local_leave_retry_handed_off
		&&!time_reached(dismissed,guest.local_leave_deadline));
	client_session_local_leave_tick(&guest,guest.local_leave_deadline);
	assert(guest.local_pending&&guest.local_leave_deadline_armed);
	/* Without a guest Pause/Retry control, expiry must keep transport retries
	 * alive and start a fresh bounded acknowledgement window. */
	struct client_session no_pause = {.local_ticket=51,.local_generation=83,
		.local_phase=CLIENT_GUEST_ATTACHED,.local_bound=true};
	assert(client_session_request_local_leave(&no_pause));
	ptime_t expired=no_pause.local_leave_deadline;
	client_session_local_leave_tick(&no_pause,expired);
	assert(no_pause.local_phase==CLIENT_GUEST_LEAVING&&no_pause.local_bound
		&&no_pause.local_pending&&no_pause.local_leave_deadline_armed);
	assert(!time_reached(expired,no_pause.local_leave_deadline));
	/* A late, correct ACK still owns teardown and releases the claim once. */
	left.payload.local_guest.generation=73;
	client_session_local_status(&guest,&left);
	assert(guest.local_phase==CLIENT_GUEST_IDLE&&!guest.local_bound);
	guest.local_ticket=99;guest.local_generation=100;
	guest.local_phase=CLIENT_GUEST_ATTACHED;guest.local_bound=true;
	client_session_local_status(&guest,&left); /* duplicate must not unbind anew */
	assert(guest.local_ticket==99&&guest.local_generation==100
		&&guest.local_phase==CLIENT_GUEST_ATTACHED&&guest.local_bound);
	while(svin_pending_count()) {
		svin_process_owned_messages(discard_request,NULL,false);
		svin_rpc_flush_pending();
	}
	svin_process_owned_messages(discard_request,NULL,false);
	puts("PASS guest leave deadline: send refusal expires to Retry; claim retained; late ACK reconciles once");
}

static void test_guest_correction_flags(void) {
	struct entity body = {0};
	body.type = ENTITY_LOCAL_PLAYER;
	body.teleport = entity_default_teleport;
	body.pos[0] = 1.0F; body.pos[1] = 65.0F; body.pos[2] = 2.0F;
	body.vel[0] = 0.25F; body.vel[1] = -0.5F; body.vel[2] = 0.75F;
	body.on_ground = true;
	struct client_session guest = {0};
	guest.local_ticket = 41;
	guest.local_generation = 73;
	guest.local_bound = true;
	guest.local_phase = CLIENT_GUEST_ATTACHED;
	guest.local_gameplay_ready = true;
	guest.local_player = &body;
	struct client_rpc correction = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 41, .generation = 73,
			.result = LOCAL_GUEST_CORRECTION, .correction_seq = 9,
			.correction_flags = PLAYER_CORRECTION_POSITION_ONLY
				| PLAYER_CORRECTION_ZERO_VEL_X
				| PLAYER_CORRECTION_SET_ON_GROUND,
			.x = 8.5, .y = 66.25, .z = -3.5}};
	client_session_local_status(&guest, &correction);
	assert(body.pos[0] == 8.5F && body.pos[1] == 66.25F
		   && body.pos[2] == -3.5F);
	assert(body.vel[0] == 0.0F && body.vel[1] == -0.5F
		   && body.vel[2] == 0.75F && !body.on_ground);
	assert(guest.player_sync.correction_seq == 9);
	struct entity applied = body;
	correction.payload.local_guest.generation = 72;
	correction.payload.local_guest.x = 99.0;
	correction.payload.local_guest.correction_seq = 10;
	client_session_local_status(&guest, &correction);
	assert(!memcmp(&body, &applied, sizeof body)
		   && guest.player_sync.correction_seq == 9);
	puts("PASS guest correction flags: selective velocity/on-ground application and stale-generation rejection");
}

static void test_owned_gameplay_reply_routes(void) {
	struct entity host = {.type=ENTITY_LOCAL_PLAYER, .health=160};
	struct entity body = {.type=ENTITY_LOCAL_PLAYER, .health=80};
	struct client_session* guest = &gstate.players[1];
	gstate.players[0].local_player = &host;
	assert(client_session_bind_local(guest, 18));
	guest->local_player = &body; /* fixture only: production body admission remains gated */
	struct client_session host_before;
	memcpy(&host_before, &gstate.players[0], sizeof(host_before));
	struct client_rpc health = {.type=CRPC_PLAYER_SET_HEALTH,
		.payload.player_set_health.health=24};
	struct client_rpc sleep = {.type=CRPC_SLEEP_STATE,
		.payload.sleep_state={.sleeping=true,.timer=47}};
	struct client_rpc toast = {.type=CRPC_HUD_MESSAGE};
	memset(toast.payload.hud_message, 'x', sizeof(toast.payload.hud_message));
	assert(clin_rpc_send_local(&health,1,17)); /* stale health is never applied */
	clin_update_drain();
	assert(body.health == 80 && !guest->local_reply_failed);
	assert(clin_rpc_send_local(&health,1,18));
	assert(clin_rpc_send_local(&sleep,1,18));
	assert(clin_rpc_send_local(&toast,1,18));
	clin_update_drain();
	assert(body.health == 24 && host.health == 160);
	assert(guest->sleeping && guest->sleep_timer == 47);
	assert(strlen(guest->hud_message) == 47);
	assert(!memcmp(&host_before, &gstate.players[0], sizeof(host_before)));
	assert(!guest->local_reply_failed);
	/* Position/world packets still require terrain admission. Entity-id
	 * payloads cannot smuggle a write to another body through this route. */
	assert(!clin_rpc_send_local(&(struct client_rpc){.type=CRPC_PLAYER_POS},1,18));
	assert(!clin_rpc_send_local(&(struct client_rpc){.type=CRPC_ENTITY_VELOCITY},1,18));
	assert(!clin_rpc_send_local(&(struct client_rpc){.type=CRPC_WORLD_RESET},1,18));
	guest->local_player = NULL;
	assert(clin_rpc_send_local(&health,1,18));
	assert(clin_rpc_send_local(&sleep,1,18));
	clin_update_drain();
	assert(guest->local_reply_failed && body.health == 24 && host.health == 160);
	assert(client_session_unbind_local(guest,18));
	assert(client_session_bind_local(guest,19));
	assert(!guest->sleeping && guest->sleep_timer == 0 && !guest->hud_message[0]);
	assert(!guest->player_sync.authoritative_pos_seen && !guest->player_sync.state_known);
	guest->local_player = &body;
	assert(clin_rpc_send_local(&health,1,18));
	clin_update_drain();
	assert(body.health == 24 && !guest->local_reply_failed);
	/* Primary tokens still target the host after the guest's queue traffic. */
	health.payload.player_set_health.health = 112;
	assert(clin_try_rpc_send(&health));
	clin_update_drain();
	assert(host.health == 112 && body.health == 24);
	ptime_t now = time_add_ms((ptime_t){0}, 800);
	assert(client_session_player_reply(guest, &toast, now));
	assert(time_diff_ms(now, guest->hud_message_until) == 3000);
	guest->local_player = NULL;
	assert(client_session_unbind_local(guest,19));
	gstate.players[0].local_player = NULL;
	puts("PASS owned gameplay replies: health/sleep/toast isolation, stale generations, missing body and token reuse");
}

static void* fail_alloc(size_t size) { (void)size; return NULL; }

static void test_reply_routes(void) {
	assert(clin_init());
	struct client_session* guest = &gstate.players[1];
	assert(client_session_bind_local(guest, 10));
	struct window_container* previous = guest->windows[0];
	guest->sleeping = true;
	guest->sleep_timer = 47;
	guest->player_sync.correction_seq = 99;
	guest->player_sync.state_known = true;
	memcpy(guest->hud_message, "Retain on allocation failure", 29);
	struct client_session retained;
	memcpy(&retained, guest, sizeof(retained));
	windowc_set_alloc_hook(fail_alloc);
	assert(!client_session_bind_local(guest, 11));
	windowc_set_alloc_hook(NULL);
	assert(guest->windows[0] == previous && guest->local_generation == 10);
	assert(!memcmp(guest, &retained, sizeof(retained)));
	struct client_rpc reply = {.type=CRPC_INVENTORY_SLOT,
		.payload.inventory_slot={.window=0,.slot=INVENTORY_SLOT_MAIN,
			.item={.id=BLOCK_STONE,.count=3}}};
	for(int k=0;k<16;k++) assert(clin_rpc_send_local(&reply,1,10));
	assert(!clin_try_rpc_send(&reply));
	/* Replacement precedes inbox drain: every queued old reply must vanish. */
	assert(client_session_bind_local(guest,11));
	assert(!guest->sleeping && !guest->hud_message[0]);
	assert(!guest->player_sync.state_known && guest->player_sync.correction_seq == 0);
	clin_update_drain();
	assert(!windowc_get_latest(guest->windows[0])->items[INVENTORY_SLOT_MAIN].id);
	for(int k=0;k<16;k++) assert(clin_rpc_send_local(&reply,1,11));
	clin_update_drain();
	assert(windowc_get_latest(guest->windows[0])->items[INVENTORY_SLOT_MAIN].count==3);
	assert(!gstate.players[0].windows[0]);
	assert(!clin_rpc_send_local(&(struct client_rpc){.type=CRPC_WORLD_RESET},1,11));
	assert(!clin_rpc_send_local(&reply,0,11));
	assert(!clin_rpc_send_local(&reply,1,0));
	assert(!client_session_bind_local(guest,10));
	assert(!client_session_unbind_local(guest,10));
	assert(guest->local_bound && guest->windows[0]);
	assert(clin_rpc_send_local(&reply,1,11));
	assert(client_session_unbind_local(guest,11));
	clin_update_drain();
	assert(!guest->windows[0] && !guest->local_bound);
	assert(!client_session_bind_local(guest,11));


 /* Failed OPEN and REOPEN retain the old mirror for safe teardown, but
  * retire presentation and stop later updates for that generation. */
 for(unsigned trial=0;trial<6;trial++) {
  uint64_t generation=12+trial;
  assert(client_session_bind_local(guest,generation));
  struct client_rpc open={.type=CRPC_OPEN_WINDOW,
   .payload.window_open={.window=WINDOWC_CHEST,.type=WINDOW_TYPE_CHEST,.slot_count=CHEST_SIZE}};
  assert(clin_rpc_send_local(&open,1,generation-1));
  clin_update_drain();assert(!guest->windows[WINDOWC_CHEST]);
  assert(clin_rpc_send_local(&open,1,generation));clin_update_drain();
  previous=guest->windows[WINDOWC_CHEST];assert(previous&&guest->visible_window==WINDOWC_CHEST);
  guest->menu_host=(struct local_menu_host){.mode=LOCAL_MENU_CONTAINER,
   .window=WINDOWC_CHEST,.window_identity=previous,.split_owned=true};
  guest->deferred_reopen_window=WINDOWC_CHEST;
  struct client_rpc failed=open;
  size_t count=trial/2==0?CHEST_SIZE:trial/2==1?0:CRPC_WINDOW_ITEMS_MAX_SLOTS+1;
  if(trial%2)failed=(struct client_rpc){.type=CRPC_REOPEN_WINDOW,
   .payload.window_reopen={.window=WINDOWC_CHEST,.type=WINDOW_TYPE_CHEST,.slot_count=count}};
  else failed.payload.window_open.slot_count=count;
  assert(clin_rpc_send_local(&failed,1,generation));assert(clin_rpc_send_local(&reply,1,generation));
  if(trial<2)windowc_set_alloc_hook(fail_alloc);
  clin_update_drain();windowc_set_alloc_hook(NULL);
  assert(guest->local_reply_failed&&guest->windows[WINDOWC_CHEST]==previous);
  assert(!local_menu_host_active(guest));
  assert(guest->visible_window==-1&&guest->deferred_reopen_window==-1);
  assert(!windowc_get_latest(guest->windows[0])->items[INVENTORY_SLOT_MAIN].id);
  assert(client_session_unbind_local(guest,generation));
 }

	/* A recycled token from the guest pool must become primary again. */
	assert(client_session_bind_local(&gstate.players[0],1));
	for(int k=0;k<32;k++) {
		assert(clin_try_rpc_send(&reply));
		clin_update_drain();
	}
	assert(windowc_get_latest(gstate.players[0].windows[0])->items[INVENTORY_SLOT_MAIN].count==3);
	client_session_clear_windows(&gstate.players[0]);
	puts("PASS guest replies: saturation, stale generations, detach, OOM and primary-token reuse");
}

static void ignore_reply(const struct client_rpc* call, void* user) {
	(void)call; (void)user;
}

/* Simulated process loss: discard live allocations WITHOUT checkpointing.
 * Production must use the acknowledged leave path. */
static void crash_guest(struct server_local* server) {
	struct server_player* guest = server->guest_actor;
	guest->inventory.reply = ignore_reply;
	if(guest->active_inventory != &guest->inventory) {
		guest->active_inventory->reply = ignore_reply;
		inventory_destroy_owned(guest->active_inventory, guest);
	}
	inventory_destroy(&guest->inventory, guest);
	free(guest);
	server->guest_actor = NULL;
	server->guest_owned = false;
	server->guest_generation = 0;
	client_session_clear_windows(&gstate.players[1]);
	level_archive_discard(&server->level);
	assert(level_archive_create(&server->level, server->level_name));
}

static struct server_rpc_source test_live_autosave(struct server_local* server,
	struct server_rpc_source source) {
	struct server_player* guest = server->guest_actor;
	/* A workbench has nine owned inputs; neither its preview output nor
	 * shared storage may become extra loot on reload. */
	struct inventory* bench = malloc(sizeof(*bench));
	assert(bench && inventory_create(bench,&inventory_logic_crafting,server,
		guest,CRAFTING_SIZE,4,70,4));
	bench->reply = ignore_reply;
	guest->active_inventory = bench;
	guest->fire=123;guest->oxygen=19;guest->on_ground=true;
	guest->inventory.items[INVENTORY_SLOT_CRAFTING] = (struct item_data){.id=BLOCK_LOG,.count=2};
	guest->inventory.picked_item = (struct item_data){.id=ITEM_DIAMOND,.count=3};
	bench->picked_item = (struct item_data){.id=BLOCK_DIRT,.count=4};
	bench->items[CRAFTING_SLOT_INPUT+8] = (struct item_data){.id=BLOCK_LOG,.count=5};
	bench->items[CRAFTING_SLOT_OUTPUT] = (struct item_data){.id=ITEM_DIAMOND,.count=64};
	bench->items[CRAFTING_SLOT_MAIN] = (struct item_data){.id=ITEM_DIAMOND,.count=11};
	assert(server_local_guest_checkpoint(server));
	assert(guest->active_inventory==bench && bench->picked_item.count==4);
	assert(guest->inventory.items[INVENTORY_SLOT_MAIN].count==13); /* stale base intact */
	assert(level_archive_save(&server->level));
	crash_guest(server);
	assert(server_local_guest_create(server,&source));
	guest=server->guest_actor; bench=guest->active_inventory;
	assert(bench!=&guest->inventory && bench->logic==&inventory_logic_crafting);
	assert(guest->inventory.items[INVENTORY_SLOT_MAIN].count==11);
	assert(guest->fire==123 && guest->oxygen==19 && guest->on_ground);
	assert(guest->inventory.items[INVENTORY_SLOT_CRAFTING].count==2);
	assert(guest->inventory.items[INVENTORY_SLOT_OUTPUT].id==BLOCK_PLANKS);
	assert(guest->inventory.items[INVENTORY_SLOT_OUTPUT].count==4);
	assert(guest->inventory.picked_item.count==3 && bench->picked_item.count==4);
	assert(bench->items[CRAFTING_SLOT_INPUT+8].count==5);
	assert(bench->items[CRAFTING_SLOT_OUTPUT].id==BLOCK_PLANKS);
	assert(bench->items[CRAFTING_SLOT_OUTPUT].count==4);
	assert(client_session_bind_local(&gstate.players[1],source.generation));
	assert(server_local_guest_publish(server,source));clin_update_drain();
	assert(gstate.players[1].visible_window==WINDOWC_CRAFTING);
	assert(windowc_get_latest(gstate.players[1].windows[WINDOWC_CRAFTING])->picked_item.count==4);
	assert(!server_local_guest_settled(server));
	/* Fixture settlement avoids synthesizing a user Drop; test the persisted
	 * private state above, then test a fresh chest snapshot independently. */
	guest->inventory.reply=ignore_reply;bench->reply=ignore_reply;
	inventory_destroy_owned(bench,guest);guest->active_inventory=&guest->inventory;
	memset(guest->inventory.items+INVENTORY_SLOT_OUTPUT,0,
		(INVENTORY_SIZE_CRAFTING+1)*sizeof(struct item_data));
	guest->inventory.picked_item=(struct item_data){0};
	struct server_chunk* chunk=NULL;
	assert(server_world_load_chunk(&server->worlds[0],0,0,&chunk) && chunk);
	assert(server_world_set_block(&server->worlds[0],4,70,4,(struct block_data){.type=BLOCK_CHEST}));
	server->chest_pos[0]=(struct complex_block_pos){4,70,4}; A1B_STAMP(server, chest, 0, &server->worlds[0]); A1B_BIND(guest, &server->worlds[0]);
	server->chest_items[0][0]=(struct item_data){.id=ITEM_DIAMOND,.count=20};
	struct inventory* chest=malloc(sizeof(*chest));
	assert(chest && inventory_create(chest,&inventory_logic_chest,server,
		guest,CHEST_SIZE,4,70,4));
	chest->reply=ignore_reply;guest->active_inventory=chest;
	chest->items[CHEST_SLOT_MAIN]=(struct item_data){.id=BLOCK_DIRT,.count=19};
	chest->picked_item=(struct item_data){.id=ITEM_DIAMOND,.count=1};
	assert(server_local_guest_checkpoint(server));assert(level_archive_save(&server->level));
	crash_guest(server);
	/* Another player changed shared storage after this guest snapshot. */
	server->chest_items[0][0].count=7;
	assert(server_local_guest_create(server,&source));
	guest=server->guest_actor;chest=guest->active_inventory;
	assert(chest->items[CHEST_SLOT_STORAGE].count==7);
	assert(chest->items[CHEST_SLOT_MAIN].count==19 && chest->picked_item.count==1);
	assert(guest->inventory.items[INVENTORY_SLOT_MAIN].count==19);
	guest->inventory.reply=ignore_reply;chest->reply=ignore_reply;
	chest->picked_item=(struct item_data){0};
	inventory_destroy_owned(chest,guest);guest->active_inventory=&guest->inventory;
	assert(client_session_bind_local(&gstate.players[1],source.generation));
	puts("PASS guest live autosave: open chest/workbench, both cursors, all crafting inputs, derived outputs, current shared storage");
	return source;
}

static unsigned s_guard_return, s_guard_reset, s_guard_hud_named, s_guard_hud_other;
static void count_guard_reply(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_RETURN_INGAME) s_guard_return++;
	else if(call->type == CRPC_WORLD_RESET) s_guard_reset++;
	else if(call->type == CRPC_HUD_MESSAGE) {
		if(!strcmp(call->payload.hud_message, "Save failed: guest not settled.")) s_guard_hud_named++;
		else s_guard_hud_other++;
	}
}
static void pump_handshake(struct server_local* server) {
	client_session_flush_local(&gstate.players[1]);
	server_local_test_process_messages(server);
	clin_update_drain();
}

static void discard_request(struct server_rpc_source source,
	struct server_rpc* call, void* user) {
	(void)source; (void)call; (void)user;
}

struct sign_reply_capture { struct client_rpc calls[2]; size_t count; };
static void capture_sign_reply(const struct client_rpc* call,void* user) {
 struct sign_reply_capture* c=user;assert(call&&c&&c->count<2);c->calls[c->count++]=*call;
}
static void test_guest_sign_set_routing(struct server_local* server,struct server_rpc_source source) {
 struct server_player* guest=server->guest_actor;assert(guest&&server->guest_confirmed);
 bool had_pos=guest->has_pos,finished=guest->finished_loading;
 guest->has_pos=guest->finished_loading=true;
 const int x=6,y=70,z=4;
 assert(server_world_set_block(&server->worlds[0],x,y,z,(struct block_data){.type=BLOCK_SIGN,.metadata=2}));
 server->sign_pos[0]=(struct complex_block_pos){x,y,z};memset(server->sign_texts[0],' ',SIGN_SIZE); A1B_STAMP(server, sign, 0, &server->worlds[0]); A1B_BIND(&server->player, &server->worlds[0]); A1B_BIND(guest, &server->worlds[0]);
 struct inventory host_sign,guest_sign;
 assert(inventory_init(&host_sign,&inventory_logic_sign,server,&server->player,SIGN_SIZE,x,y,z));
 assert(inventory_init(&guest_sign,&inventory_logic_sign,server,guest,SIGN_SIZE,x,y,z));
 struct sign_reply_capture host={0},reply={0};
 host_sign.reply=capture_sign_reply;host_sign.reply_user=&host;
 guest_sign.reply=capture_sign_reply;guest_sign.reply_user=&reply;
 server->player.active_inventory=&host_sign;guest->active_inventory=&guest_sign;
 struct server_rpc edit={.type=SRPC_SIGN_SET,.payload.sign_set={.window=WINDOWC_SIGN,.slot=2,.ch='G',.action_id=71}};
 assert(svin_rpc_send_from(source,&edit));server_local_test_process_messages(server);
 assert(guest_sign.items[2].count=='G'&&host_sign.items[2].count==' ');
 assert(!host.count&&reply.count==2&&reply.calls[0].type==CRPC_WINDOW_TRANSACTION);
 assert(reply.calls[0].payload.window_transaction.accepted&&reply.calls[0].payload.window_transaction.action_id==71);
 assert(reply.calls[1].type==CRPC_INVENTORY_SLOT&&reply.calls[1].payload.inventory_slot.slot==2);
 host.count=reply.count=0;struct server_rpc_source stale=source;
 stale.generation=source.generation==1?2:source.generation-1;
 edit.payload.sign_set.ch='S';edit.payload.sign_set.action_id=72;
 assert(svin_rpc_send_from(stale,&edit));server_local_test_process_messages(server);
 assert(guest_sign.items[2].count=='G'&&host_sign.items[2].count==' '&&!host.count&&!reply.count);
 guest_sign.capacity=2;edit.payload.sign_set.ch='C';edit.payload.sign_set.action_id=73;
 assert(svin_rpc_send_from(source,&edit));server_local_test_process_messages(server);
 assert(guest_sign.items[2].count=='G'&&host_sign.items[2].count==' '&&!host.count&&reply.count==1);
 assert(reply.calls[0].type==CRPC_WINDOW_TRANSACTION&&!reply.calls[0].payload.window_transaction.accepted);
 assert(reply.calls[0].payload.window_transaction.action_id==73&&!guest_sign.transaction_pending);
 guest_sign.capacity=SIGN_SIZE;
 server->player.active_inventory=&server->player.inventory;guest->active_inventory=&guest->inventory;
 inventory_destroy(&host_sign,&server->player);inventory_destroy(&guest_sign,guest);
 server->sign_pos[0].y=-1;guest->has_pos=had_pos;guest->finished_loading=finished;
 puts("PASS guest sign set: owner routing, stale generation and capacity rejection");
}
/* ---- S1-a1: the seven per-actor gameplay verbs ----------------------------
 * Drives slot 0 and slot 1 through the REAL mailbox (svin_rpc_send_from +
 * server_local_test_process_messages) against an ADMITTED guest body, and
 * reads the SERVER's own state back -- no client involved. Per verb:
 *   (a) a host action changes host state only;
 *   (b) a guest action changes the guest actor plus the expected shared-world
 *       effect, and leaves the host's inventory and hotbar alone;
 *   (c) a structurally valid but STALE generation is queued and then dropped
 *       at the dispatcher, mutating nothing;
 *   (d) an unconfirmed ticket, and a guest with no admitted body, are dropped;
 *   (e) entity reach/LOS is measured from the ACTING actor's eye -- the guest
 *       is refused a target the host is standing next to, and vice versa.
 * The verbs (or sub-paths) that stay FAIL-CLOSED for a guest are asserted as
 * such here too, so the gate-2 boundary is pinned rather than assumed. */
/* Reply-envelope probe (S1-a2 item 4): records the owner generation a reply
 * was published under. Installed on the ACTING actor inventory in place of the
 * real guest_reply, so the envelope is observable without a client thread. */
static uint64_t s_tag_seen_generation;
static bool s_tag_seen_untagged;
static unsigned s_tag_seen_count;
static unsigned s_tag_seen_health_count;
static enum client_rpc_type s_tag_last_type;
static short s_tag_last_health;
/* damage-client-0926: knockback motion replies on the guest envelope. */
static unsigned s_tag_seen_velocity_count;
static struct client_rpc s_tag_last_velocity;
static unsigned s_cactus_zero_health;
static void tag_probe_reply(const struct client_rpc* call, void* user) {
	struct server_local* server = user;
	s_tag_seen_count++;
	if(call) {
		if(call->type == CRPC_PLAYER_SET_HEALTH) {
			s_tag_last_type = call->type;
			s_tag_seen_health_count++;
			s_tag_last_health = call->payload.player_set_health.health;
		}
		if(call->type == CRPC_LOCAL_PLAYER_VELOCITY) {
			s_tag_seen_velocity_count++;
			s_tag_last_velocity = *call;
		}
	}
	if(server && server->guest_generation)
		s_tag_seen_generation = server->guest_generation;
	else
		s_tag_seen_untagged = true;
}

static void count_cactus_zero_health(const struct client_rpc* call, void* user) {
	(void)user;
	if(call && call->type == CRPC_PLAYER_SET_HEALTH
	   && call->payload.player_set_health.health == 0)
		s_cactus_zero_health++;
}

/* ===== S1-a2 (1): SRPC_PLAYER_STATE is per actor =========================
 * Astra gate-1 item 2: the state verb must not couple P1 and P2. Drives the
 * REAL mailbox in both directions and checks the other actor is untouched,
 * plus that a guest state change raises nothing the host reads. */
static void test_guest_player_state(struct server_local* server,
									struct server_rpc_source live) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	assert(guest);
	host->has_pos = host->finished_loading = true;
	guest->has_pos = guest->finished_loading = true;
	host->sneaking = false; host->on_ground = false;
	guest->sneaking = false; guest->on_ground = false;
	assert(!server->reject_player_pos_this_update);
	assert(!host->reject_pos_this_update && !guest->reject_pos_this_update);

	struct server_rpc st = {.type = SRPC_PLAYER_STATE,
		.payload.player_state = {.on_ground = true, .sneaking = true,
			.correction_seq = guest->correction_seq}};
	assert(svin_rpc_send_from(live, &st));
	server_local_test_process_messages(server);
	assert(guest->sneaking && guest->on_ground);
	assert(!host->sneaking && !host->on_ground); /* the host is untouched */
	/* ...and nothing the host reads was raised by the guest state change. */
	assert(!server->reject_player_pos_this_update);
	assert(!host->reject_pos_this_update);

	st.payload.player_state.correction_seq = host->correction_seq;
	st.payload.player_state.sneaking = false;
	st.payload.player_state.on_ground = true;
	assert(svin_rpc_send(&st));
	server_local_test_process_messages(server);
	assert(host->on_ground && !host->sneaking);
	assert(guest->sneaking && guest->on_ground); /* the guest is untouched */

	/* The HOST latch must not swallow the GUEST sample (the coupling Astra
	 * item 2 rejected): arm the host body latch, then send a guest state. */
	host->reject_pos_this_update = true;
	guest->sneaking = false;
	st.payload.player_state.correction_seq = guest->correction_seq;
	st.payload.player_state.sneaking = true;
	assert(svin_rpc_send_from(live, &st));
	server_local_test_process_messages(server);
	assert(guest->sneaking); /* accepted despite the HOST latch */
	/* ...while the host body legitimately drops its own sample. */
	st.payload.player_state.correction_seq = host->correction_seq;
	st.payload.player_state.sneaking = true;
	assert(svin_rpc_send(&st));
	server_local_test_process_messages(server);
	assert(!host->sneaking);
	host->reject_pos_this_update = false;
	/* Symmetrically, the guest latch does not block the host. */
	guest->reject_pos_this_update = true;
	assert(svin_rpc_send(&st));
	server_local_test_process_messages(server);
	assert(host->sneaking);
	guest->reject_pos_this_update = false;
	host->sneaking = guest->sneaking = false;

	/* ---- R-S1a2 F2: a world-authored position correction barriers BOTH ----
	 * bodies. This is the exact call the piston push correction makes
	 * (source/block/redstone_piston_server.c, one line beside its server-wide
	 * flag write): it re-authors a player position mid-update, so every body's
	 * already-queued movement input -- authored against the pre-push state --
	 * must be dropped. Before that call the piston wrote ONLY the server-wide
	 * flag, which no longer gates either handler, so the barrier survived
	 * purely on an undocumented coupling to the correction_seq bump on the
	 * next line. */
	assert(!host->reject_pos_this_update && !guest->reject_pos_this_update);
	/* The server-wide flag ALONE must not gate the handlers any more (that is
	 * the documented split: it means 'a body was replaced, end this logical
	 * update', not 'drop this body's input'); pin it, so a future 'just read
	 * both flags' patch cannot silently re-couple P1 and P2. */
	server->reject_player_pos_this_update = true;
	st.payload.player_state.correction_seq = guest->correction_seq;
	st.payload.player_state.sneaking = true;
	st.payload.player_state.on_ground = true;
	assert(svin_rpc_send_from(live, &st));
	server_local_test_process_messages(server);
	assert(guest->sneaking);
	server->reject_player_pos_this_update = false;
	guest->sneaking = false;

	server_local_raise_actor_input_barriers(server);
	assert(host->reject_pos_this_update && guest->reject_pos_this_update);
	/* The GUEST's next state update is dropped... */
	st.payload.player_state.correction_seq = guest->correction_seq;
	st.payload.player_state.sneaking = true;
	assert(svin_rpc_send_from(live, &st));
	server_local_test_process_messages(server);
	assert(!guest->sneaking);
	/* ...and so is the HOST's. */
	st.payload.player_state.correction_seq = host->correction_seq;
	assert(svin_rpc_send(&st));
	server_local_test_process_messages(server);
	assert(!host->sneaking);
	/* A position sample is gated by the same latch, for both bodies. */
	double hx0 = host->x, gx0 = guest->x;
	struct server_rpc pos = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = hx0 + 3.0, .y = host->y, .z = host->z,
			.rx = host->rx, .ry = host->ry,
			.correction_seq = host->correction_seq}};
	assert(svin_rpc_send(&pos));
	server_local_test_process_messages(server);
	assert(host->x == hx0);
	pos.payload.player_pos.x = gx0 + 3.0;
	pos.payload.player_pos.y = guest->y;
	pos.payload.player_pos.z = guest->z;
	pos.payload.player_pos.rx = guest->rx;
	pos.payload.player_pos.ry = guest->ry;
	pos.payload.player_pos.correction_seq = guest->correction_seq;
	assert(svin_rpc_send_from(live, &pos));
	server_local_test_process_messages(server);
	assert(guest->x == gx0);
	/* The latches retire at the update boundary, exactly as before. */
	host->reject_pos_this_update = guest->reject_pos_this_update = false;
	host->sneaking = guest->sneaking = false;
	printf("PASS S1-a2 (1): SRPC_PLAYER_STATE applies per actor; neither"
		   " latch crosses; a world position correction barriers both\n");
}

/* S1-a3: the damage/heal pipeline and the zero-health state. Defined at the
 * END of this file so the diff that adds it stays out of everyone else way;
 * declared here because test_guest_gameplay calls it beside its siblings. */
static void test_valid_guest_bed_respawn(struct server_local* server,
	const struct server_player* guest) {
	enum { HX=13, HY=70, HZ=13 };
	struct block_data saved[3][3][3];
	for(int dx=-1; dx<=1; dx++) for(int dy=-1; dy<=1; dy++)
		for(int dz=-1; dz<=1; dz++)
			assert(server_world_get_block(&server->worlds[0], HX+dx, HY+dy, HZ+dz,
				&saved[dx+1][dy+1][dz+1]));
	for(int dx=-1; dx<=1; dx++) for(int dz=-1; dz<=1; dz++) {
		(void)server_world_set_block(&server->worlds[0], HX+dx, HY-1, HZ+dz,
			(struct block_data){.type=BLOCK_STONE});
		(void)server_world_set_block(&server->worlds[0], HX+dx, HY, HZ+dz,
			(struct block_data){.type=BLOCK_AIR});
		(void)server_world_set_block(&server->worlds[0], HX+dx, HY+1, HZ+dz,
			(struct block_data){.type=BLOCK_AIR});
		clin_drain_discard();
	}
	/* Beta bed direction is meta&3 (PAR-BED-META). Metadata 0 points
	 * south; Beta checks the head-centered 3x3 in x-major/z-minor order. */
	(void)server_world_set_block(&server->worlds[0], HX, HY, HZ,
		(struct block_data){.type=BLOCK_BED, .metadata=0});
	clin_drain_discard();
	struct server_player bed_guest = *guest;
	bed_guest.has_bed_spawn = true;
	bed_guest.bed_spawn_x = HX; bed_guest.bed_spawn_y = HY;
	bed_guest.bed_spawn_z = HZ;
	double x=0,y=0,z=0; bool keep=false;
	assert(server_local_prepare_guest_respawn(server, &bed_guest, &x,&y,&z,&keep));
	assert(keep && x == HX-0.5 && y == HY+PLAYER_EYE_HEIGHT+0.1
		&& z == HZ-0.5);
	for(int dx=-1; dx<=1; dx++) for(int dy=-1; dy<=1; dy++)
		for(int dz=-1; dz<=1; dz++)
			{
				(void)server_world_set_block(&server->worlds[0], HX+dx, HY+dy, HZ+dz,
					saved[dx+1][dy+1][dz+1]);
				clin_drain_discard();
			}
	puts("PASS S3 guest valid bed: real metadata and Beta x-major first safe cell retained");
}

struct respawn_reply_probe {
	uint64_t generation, epoch;
	int sequence, correction_order, ready_order;
	uint8_t correction_flags;
	uint64_t correction_generation, correction_epoch;
	uint8_t correction_owner;
};
struct host_respawn_reply_probe {
	unsigned retry0, retry_epoch, terminal0, loading;
	int sequence, correction_order, ready_order;
};
static void capture_host_respawn_reply(const struct client_rpc* call, void* user) {
	if(call->type != CRPC_HOST_RESPAWN) return;
	struct host_respawn_reply_probe* p = user;
	uint64_t epoch = call->payload.host_respawn.respawn_epoch;
	switch(call->payload.host_respawn.result) {
		case HOST_RESPAWN_RETRY:
			if(epoch) p->retry_epoch++; else p->retry0++;
			break;
		case HOST_RESPAWN_TERMINAL:
			if(!epoch) p->terminal0++;
			break;
		case HOST_RESPAWN_LOADING: p->loading++; break;
		case HOST_RESPAWN_CORRECTION:
			p->correction_order = ++p->sequence;
			break;
		case HOST_RESPAWN_GAMEPLAY_READY:
			p->ready_order = ++p->sequence;
			break;
		default: break;
	}
}
static void capture_respawn_reply(const struct client_rpc* call, void* user) {
	struct respawn_reply_probe* p = user;
	if(call->type != CRPC_LOCAL_GUEST
	   || call->payload.local_guest.respawn_epoch != p->epoch) return;
	int order = ++p->sequence;
	if(call->payload.local_guest.result == LOCAL_GUEST_CORRECTION) {
		p->correction_order = order;
		p->correction_flags = call->payload.local_guest.correction_flags;
		p->correction_owner = call->payload.local_guest.owner;
		p->correction_generation = call->payload.local_guest.generation;
		p->correction_epoch = call->payload.local_guest.respawn_epoch;
	} else if(call->payload.local_guest.result == LOCAL_GUEST_GAMEPLAY_READY)
		p->ready_order = order;
}
static void count_bed_warning(const struct client_rpc* call, void* user) {
	if(call->type == CRPC_HUD_MESSAGE
	   && strcmp(call->payload.hud_message,
		"Your home bed was missing or obstructed") == 0)
		(*(unsigned*)user)++;
}
static void count_guest_respawn_retry(const struct client_rpc* call, void* user) {
	if(call->type == CRPC_LOCAL_GUEST
	   && call->payload.local_guest.result == LOCAL_GUEST_RESPAWN_RETRY)
		(*(unsigned*)user)++;
}

static void test_guest_damage_pipeline(struct server_local* server,
									   struct server_rpc_source live);
static void complete_real_guest_hazard_respawn(struct server_local* server,
	struct server_rpc_source live);
/* R-S1a3 F1/F2: the two PER-TICK steps of that pipeline -- the hurt-resistance
 * decay and the knockback shove -- which S1-a3 left host-only. Same placement
 * rule as its sibling above. */
static void test_guest_per_tick_damage_steps(struct server_local* server,
											 struct server_rpc_source live);

struct hazard_update_job {
	bool apply_client;
	int running;
};

static void* hazard_update_entry(void* arg) {
	struct hazard_update_job* job = arg;
	while(__atomic_load_n(&job->running, __ATOMIC_ACQUIRE)) {
		if(job->apply_client)
			clin_update_drain();
		else
			clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}

/* Keep archive/server work on the main thread that created the region handle,
 * while a temporary client consumer supplies production's other half of the
 * bounded inbox. The main thread does not touch client state until the worker
 * is stopped and joined. */
static void run_hazard_update(struct server_local* server, bool apply_client) {
	struct hazard_update_job job = {
		.apply_client = apply_client,
		.running = 1,
	};
	struct thread consumer;
	assert(thread_create(&consumer, hazard_update_entry, &job, 64));
	server_local_test_update_once(server);
	__atomic_store_n(&job.running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	if(apply_client)
		clin_update_drain();
	else
		clin_drain_discard();
}

static void test_guest_lethal_lava_respawn(struct server_local* server,
	struct server_rpc_source live);
static void test_guest_lethal_dry_burn_respawn(struct server_local* server,
	struct server_rpc_source live);
static void test_guest_lethal_lava_does_not_skip_host_hazard(
	struct server_local* server, struct server_rpc_source live);

/* ===== S1-a2 (2+3+4): aggro identity, projectile attribution, tagging ====
 * Every assertion runs the REAL mailbox and then SUBSEQUENT ENTITY/AI TICKS,
 * and checks the effect on the OTHER player, as Astra gate-2 D2 requires. */
static void test_guest_aggro_identity(struct server_local* server,
									  struct server_rpc_source live) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	assert(guest);
	test_valid_guest_bed_respawn(server, guest);
	/* Freeze what we perturb -- the caller's residency/leave assertions run
	 * against the ORIGINAL poses (the sibling test_guest_action_verbs restores
	 * for the same reason). */
	double hx = host->x, hy = host->y, hz = host->z;
	double gx = guest->x, gy = guest->y, gz = guest->z;
	float hvx = host->vel_x, hvy = host->vel_y, hvz = host->vel_z;
	float gvx = guest->vel_x, gvy = guest->vel_y, gvz = guest->vel_z;
	size_t hslot0 = INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&host->inventory);
	size_t gslot0 = INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&guest->inventory);
	struct item_data hheld0 = host->inventory.items[hslot0];
	struct item_data gheld0 = guest->inventory.items[gslot0];
	struct item_data hmain0 = host->inventory.items[INVENTORY_SLOT_MAIN];
	struct item_data gmain0 = guest->inventory.items[INVENTORY_SLOT_MAIN];
	assert(host->has_pos && host->finished_loading);
	assert(guest->has_pos && guest->finished_loading);
	host->x = 3.5; host->y = 72.0; host->z = 3.5;
	guest->x = 12.5; guest->y = 72.0; guest->z = 12.5;

	/* ---- (2) a guest punch latches the mob onto the GUEST --------------- */
	struct entity* zombie = server_local_spawn_monster_at(
		(vec3) {(float)guest->x + 1.0F, (float)(guest->y - 1.0),
				(float)guest->z}, MONSTER_ZOMBIE, server);
	assert(zombie && zombie->type == ENTITY_MONSTER);
	assert(zombie->data.monster.aggro_time == 0);
	assert(actor_ident_is_host(zombie->data.monster.target_actor));
	zombie->data.monster.hurt_resist_time = 0;
	zombie->data.monster.last_damage = 0;
	int32_t zhealth = zombie->health;
	short host_health_before = host->health;
	struct server_rpc hit = {.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack = {.entity_id = zombie->id}};
	assert(svin_rpc_send_from(live, &hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(zombie->health < zhealth);
	assert(zombie->data.monster.aggro_time == MOB_TARGET_RETENTION_TICKS);
	assert(zombie->data.monster.target_actor.slot == ACTOR_SLOT_GUEST);
	assert(zombie->data.monster.target_actor.generation == live.generation);

	/* ...and it SURVIVES subsequent AI ticks (the identity is re-resolved
	 * every tick, so a broken resolver would silently reset it here), while
	 * the host takes no damage from a mob that is chasing the guest. */
	for(int k = 0; k < 4; k++) {
		(void)server_local_test_tick_entities(server);
		clin_drain_discard();
		assert(zombie->data.monster.target_actor.slot == ACTOR_SLOT_GUEST);
		assert(zombie->data.monster.target_actor.generation
			   == live.generation);
		assert(zombie->data.monster.aggro_time > 0);
	}
	assert(host->health == host_health_before);

	/* ---- (2b) a GENERATION CHANGE invalidates it on the next AI tick ---- */
	uint64_t saved_generation = server->guest_generation;
	server->guest_generation = saved_generation + 1;
	assert(!server_local_actor_resolve(server,
		(struct actor_ident) {.slot = ACTOR_SLOT_GUEST,
			.generation = saved_generation}));
	(void)server_local_test_tick_entities(server);
	clin_drain_discard();
	/* The stale identity is rejected, then this still-admitted, nearby body
	 * may be acquired afresh under its current generation in the same tick. */
	assert(zombie->data.monster.target_actor.slot == ACTOR_SLOT_GUEST);
	assert(zombie->data.monster.target_actor.generation
		   == server->guest_generation);
	server->guest_generation = saved_generation;

	/* ---- (2c) a LEAVE (no admitted body) invalidates it too ------------- */
	/* Re-latch first. Clear the Beta i-frame window explicitly: a second hit
	 * inside it is ABSORBED and (faithfully) sets no target, which would make
	 * this case pass vacuously. */
	zombie->data.monster.hurt_resist_time = 0;
	zombie->data.monster.last_damage = 0;
	assert(zombie->health > 0 && zombie->data.monster.death_time == 0);
	/* The chase moved it; put it back inside the guest's own 6.0-block
	 * func_6006_a reach so the re-latch is not refused on distance. */
	zombie->teleport(zombie, (vec3) {(float)guest->x + 1.0F,
		(float)(guest->y - 1.0), (float)guest->z});
	assert(svin_rpc_send_from(live, &hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(zombie->data.monster.target_actor.slot == ACTOR_SLOT_GUEST);
	bool saved_gameplay = server->guest_gameplay;
	server->guest_gameplay = false;
	(void)server_local_test_tick_entities(server);
	clin_drain_discard();
	assert(actor_ident_is_host(zombie->data.monster.target_actor));
	assert(zombie->data.monster.target_actor.generation == 0);
	server->guest_gameplay = saved_gameplay;
	zombie->delay_destroy = 0;

	/* ---- (2d) the LATCH EXPIRING retires the identity, guest still here --
	 * R-S1a2 F6: the two cases above both land on the resolve-FAIL reset
	 * (entity_monster.c re-resolves the identity every AI tick and clears both
	 * the latch and the identity when the body is gone), so neither exercised
	 * the give-up retirement -- deleting it left them green. Beta clears
	 * entityToAttack when the mob gives up [740c583 EntityCreature.
	 * updatePlayerActionState], and the next findPlayerToAttack picks a player
	 * afresh; without the retirement a mob that had latched onto a STILL
	 * PRESENT guest would keep measuring its chase and acquisition distances
	 * to the guest forever and could never re-acquire the host. So: re-latch
	 * onto the live guest, then walk the guest out of the leash so the mob
	 * holds no target and bleeds the Beta retention down to zero. */
	zombie = server_local_spawn_monster_at(
		(vec3) {(float)guest->x + 1.0F, (float)(guest->y - 1.0),
				(float)guest->z}, MONSTER_ZOMBIE, server);
	assert(zombie && zombie->type == ENTITY_MONSTER);
	zombie->data.monster.hurt_resist_time = 0;
	zombie->data.monster.last_damage = 0;
	hit.payload.entity_attack.entity_id = zombie->id;
	assert(svin_rpc_send_from(live, &hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(zombie->data.monster.target_actor.slot == ACTOR_SLOT_GUEST);
	assert(zombie->data.monster.aggro_time == MOB_TARGET_RETENTION_TICKS);
	{
		/* Out of the leash AND out of follow range, so no tick re-tops the
		 * latch and none holds the target; the guest stays fully admitted, so
		 * the identity still RESOLVES on every one of these ticks -- the only
		 * thing that can clear it is the expiry retirement. */
		double gx_far = guest->x, gz_far = guest->z;
		guest->x = gx_far + 96.0; guest->z = gz_far + 96.0;
		/* Fast-forward the countdown to its LAST unit. The countdown itself is
		 * Beta retention arithmetic that combat.h and the mob-AI tests already
		 * pin (and the AI decrements on its own update cadence, not once per
		 * entity tick, so walking it all the way down here would just be a slow
		 * race against the zombie's own daylight death). What is under test is
		 * what happens AT zero: the identity retires to the host even though
		 * the guest is still admitted and still resolving. */
		zombie->data.monster.aggro_time = 1;
		unsigned ticks = 0;
		while(zombie->data.monster.aggro_time > 0 && ticks <= 30u) {
			/* Until the very tick it expires, the identity is still the guest. */
			assert(zombie->data.monster.target_actor.slot == ACTOR_SLOT_GUEST);
			assert(server_local_actor_resolve(
					   server, zombie->data.monster.target_actor) == guest);
			(void)server_local_test_tick_entities(server);
			clin_drain_discard();
			ticks++;
		}
		assert(zombie->data.monster.aggro_time == 0 && ticks > 0 && ticks <= 30u);
		assert(zombie->health > 0); /* it gave up, it did not die */
		/* Retired to the host, with the guest still admitted and resolving. */
		assert(server->guest_gameplay && server->guest_actor == guest);
		assert(actor_ident_is_host(zombie->data.monster.target_actor));
		assert(zombie->data.monster.target_actor.generation == 0);
		guest->x = gx_far; guest->z = gz_far;
	}
	zombie->delay_destroy = 0;
	(void)server_local_test_tick_entities(server);
	clin_drain_discard();
	/* ---- (3) projectile attribution ------------------------------------- */
	size_t hslot = INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&host->inventory);
	size_t gslot = INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&guest->inventory);
	host->inventory.items[hslot] = (struct item_data) {.id = ITEM_BOW, .count = 1};
	guest->inventory.items[gslot] = (struct item_data) {.id = ITEM_BOW, .count = 1};
	/* One arrow each, in a non-held slot (find_arrow_slot scans the whole
	 * inventory), so the shot is not blocked by the Beta ammo gate. */
	host->inventory.items[INVENTORY_SLOT_MAIN]
		= (struct item_data) {.id = ITEM_ARROW, .count = 4};
	guest->inventory.items[INVENTORY_SLOT_MAIN]
		= (struct item_data) {.id = ITEM_ARROW, .count = 4};
	struct server_rpc use = {.type = SRPC_USE_ITEM,
		.payload.use_item = {.charge = 1.0f}};
	assert(svin_rpc_send_from(live, &use));
	server_local_test_process_messages(server);
	assert(server->pending_arrow_count == 1);
	assert(server->pending_arrows[0].shooter.slot == ACTOR_SLOT_GUEST);
	assert(server->pending_arrows[0].shooter.generation == live.generation);
	/* The identity survives the fire-time -> entity handoff. */
	(void)server_local_test_tick_entities(server);
	clin_drain_discard();
	assert(server->pending_arrow_count == 0);
	{
		struct entity* guest_arrow = NULL;
		dict_entity_it_t it;
		dict_entity_it(it, server->entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			if(e && e->type == ENTITY_ARROW)
				guest_arrow = e;
			dict_entity_next(it);
		}
		assert(guest_arrow);
		assert(guest_arrow->data.arrow.shooter_actor.slot == ACTOR_SLOT_GUEST);
		assert(guest_arrow->data.arrow.shooter_actor.generation
			   == live.generation);
		/* A guest arrow may hit the HOST from tick 0 (Beta: players damage
		 * players); the HOST-fired arrow keeps its own five-tick grace. */
		assert(arrow_should_hit_actor(
			actor_ident_is_host(guest_arrow->data.arrow.shooter_actor), 0));
		guest_arrow->delay_destroy = 0;
	}
	/* The host shot is byte-identically attributed to the host. */
	assert(svin_rpc_send(&use));
	server_local_test_process_messages(server);
	assert(server->pending_arrow_count == 1);
	assert(actor_ident_is_host(server->pending_arrows[0].shooter));
	assert(!arrow_should_hit_actor(true, 0));  /* own grace still closed */
	assert(arrow_should_hit_actor(true, ARROW_OWNER_GRACE_TICKS));
	server->pending_arrow_count = 0;

	/* ---- (4) reply tagging on a guest action ---------------------------- */
	/* BLOCK_PLACE (the brief's example) is still fail-closed -- its hook
	 * signatures belong to S1-a1b -- so the tagged reply is proved on
	 * BLOCK_DIG's TOOL WEAR instead, which is the same CRPC_INVENTORY_SLOT
	 * through the same server_local_actor_slot_reply envelope. */
	guest->inventory.items[gslot]
		= (struct item_data) {.id = ITEM_IRON_PICKAXE, .count = 1};
	host->inventory.items[hslot]
		= (struct item_data) {.id = ITEM_IRON_PICKAXE, .count = 1};
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	void (*guest_saved)(const struct client_rpc*, void*) = guest->inventory.reply;
	void* guest_user_saved = guest->inventory.reply_user;
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;
	struct server_rpc dig = {.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.x = 13, .y = 70, .z = 13, .finished = true}};
	server_world_set_block(&server->worlds[0], 13, 70, 13,
		(struct block_data) {.type = BLOCK_STONE, .metadata = 0});
	assert(svin_rpc_send_from(live, &dig));
	server_local_test_process_messages(server);
	clin_drain_discard();
	guest->inventory.reply = guest_saved;
	guest->inventory.reply_user = guest_user_saved;
	assert(guest->inventory.items[gslot].durability == 1); /* it DID wear */
	assert(s_tag_seen_generation == live.generation); /* tagged for the guest */
	assert(!s_tag_seen_untagged);
	/* The HOST reply stays untagged: nothing in production installs a reply
	 * hook on the host inventory, so server_local_actor_slot_reply takes its
	 * clin_rpc_send fallback -- exactly the route it replaced. */
	assert(!host->inventory.reply);
	/* A stale-generation reply never reaches players[1]: the queue ACCEPTS the
	 * send and client_interface.c drops it at DRAIN against
	 * rpc_guest_generation (:3742-3752) -- the behaviour
	 * test_owned_gameplay_reply_routes above already pins end to end with a real
	 * drain. What is asserted here is the structural half the envelope owes
	 * this verb path: a MIS-ADDRESSED reply is refused outright. */
	struct client_rpc probe = {.type = CRPC_INVENTORY_SLOT,
		.payload.inventory_slot = {.window = WINDOWC_INVENTORY, .slot = 0}};
	assert(!clin_rpc_send_local(&probe, 0, live.generation)); /* slot 0 tagged */
	assert(!clin_rpc_send_local(&probe, 1, 0));               /* guest, no gen */
	assert(clin_rpc_send_local(&probe, 1, live.generation + 7)); /* accepted... */
	clin_drain_discard();                                     /* ...then dropped */
	host->x = hx; host->y = hy; host->z = hz;
	guest->x = gx; guest->y = gy; guest->z = gz;
	host->vel_x = hvx; host->vel_y = hvy; host->vel_z = hvz;
	guest->vel_x = gvx; guest->vel_y = gvy; guest->vel_z = gvz;
	host->inventory.items[hslot0] = hheld0;
	guest->inventory.items[gslot0] = gheld0;
	host->inventory.items[INVENTORY_SLOT_MAIN] = hmain0;
	guest->inventory.items[INVENTORY_SLOT_MAIN] = gmain0;
	printf("PASS S1-a2 (2/3/4): mob target + projectile shooter resolve to the"
		   " guest by {slot, generation}; invalidate on generation change and"
		   " leave; guest action replies carry the guest generation\n");
}

static void test_guest_action_verbs(struct server_local* server,
									struct server_rpc_source live) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	assert(guest && server->guest_gameplay && server->guest_confirmed);
	struct server_rpc_source stale = live;
	stale.generation = live.generation + 1;

	/* Freeze what we perturb; the caller's residency/leave assertions follow. */
	double hx = host->x, hy = host->y, hz = host->z;
	double gx = guest->x, gy = guest->y, gz = guest->z;
	float hrx = host->rx, hry = host->ry, grx = guest->rx, gry = guest->ry;
	assert(host->active_inventory == &host->inventory);
	assert(guest->active_inventory == &guest->inventory);
	host->has_pos = host->finished_loading = true;
	guest->has_pos = guest->finished_loading = true;
	/* Same loaded column, nine blocks apart: further than the 6.0-block
	 * func_6006_a entity reach, so an entity beside one actor is provably out
	 * of the other's. */
	host->x = 3.5; host->y = 72.0; host->z = 3.5; host->rx = 0; host->ry = 0;
	guest->x = 12.5; guest->y = 72.0; guest->z = 12.5; guest->rx = 0;
	guest->ry = 0;

	/* ---- SRPC_HOTBAR_SLOT ------------------------------------------------ */
	/* Pick indices that are provably a CHANGE for each actor, so neither
	 * assertion can pass on a pre-existing value (the archived guest already
	 * carries hotbar_slot 7). */
	size_t host_before = inventory_get_hotbar(&host->inventory);
	size_t guest_before = inventory_get_hotbar(&guest->inventory);
	size_t host_want = (host_before + 1) % INVENTORY_SIZE_HOTBAR;
	size_t guest_want = (guest_before + 2) % INVENTORY_SIZE_HOTBAR;
	if(guest_want == host_want)
		guest_want = (guest_want + 1) % INVENTORY_SIZE_HOTBAR;
	size_t never_want = (guest_want + 3) % INVENTORY_SIZE_HOTBAR;
	if(never_want == host_want)
		never_want = (never_want + 1) % INVENTORY_SIZE_HOTBAR;
	assert(host_want != host_before && guest_want != guest_before
		   && guest_want != host_want && never_want != guest_want
		   && never_want != host_want);
	struct server_rpc hot = {.type = SRPC_HOTBAR_SLOT,
		.payload.hotbar_slot = {.slot = host_want}};
	assert(svin_rpc_send(&hot));
	server_local_test_process_messages(server);
	assert(inventory_get_hotbar(&host->inventory) == host_want);
	assert(inventory_get_hotbar(&guest->inventory) == guest_before);
	hot.payload.hotbar_slot.slot = guest_want;
	assert(svin_rpc_send_from(live, &hot));
	server_local_test_process_messages(server);
	assert(inventory_get_hotbar(&guest->inventory) == guest_want);
	assert(inventory_get_hotbar(&host->inventory) == host_want);
	/* (c) stale generation: ACCEPTED at the queue boundary, dropped at the
	 * dispatcher -- neither actor moves. */
	hot.payload.hotbar_slot.slot = never_want;
	assert(svin_rpc_send_from(stale, &hot));
	server_local_test_process_messages(server);
	assert(inventory_get_hotbar(&guest->inventory) == guest_want
		   && inventory_get_hotbar(&host->inventory) == host_want);
	/* (d) an outstanding, unconfirmed ticket is the dispatcher's own gate. */
	uint64_t ticket_saved = server->guest_ticket;
	bool confirmed_saved = server->guest_confirmed;
	server->guest_ticket = ticket_saved ? ticket_saved : 4242;
	server->guest_confirmed = false;
	hot.payload.hotbar_slot.slot = never_want;
	assert(svin_rpc_send_from(live, &hot));
	server_local_test_process_messages(server);
	assert(inventory_get_hotbar(&guest->inventory) == guest_want);
	server->guest_ticket = ticket_saved;
	server->guest_confirmed = confirmed_saved;
	/* (d) ...and a guest with NO admitted body owns nothing to act with. */
	server->guest_gameplay = false;
	assert(svin_rpc_send_from(live, &hot));
	server_local_test_process_messages(server);
	assert(inventory_get_hotbar(&guest->inventory) == guest_want);
	server->guest_gameplay = true;
	/* (d) ...nor may a DEAD or SLEEPING actor act -- the same barriers the host
	 * path applies, re-derived from the guest. */
	guest->health = 0;
	assert(svin_rpc_send_from(live, &hot));
	server_local_test_process_messages(server);
	assert(inventory_get_hotbar(&guest->inventory) == guest_want);
	guest->health = 160;
	guest->sleeping = true;
	assert(svin_rpc_send_from(live, &hot));
	server_local_test_process_messages(server);
	assert(inventory_get_hotbar(&guest->inventory) == guest_want);
	guest->sleeping = false;
	/* The host's own hotbar never moved through any of that. */
	assert(inventory_get_hotbar(&host->inventory) == host_want);

	/* ---- SRPC_DROP_ITEM -------------------------------------------------- */
	size_t hslot = INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&host->inventory);
	size_t gslot = INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&guest->inventory);
	host->inventory.items[hslot]
		= (struct item_data) {.id = ITEM_DIAMOND, .count = 4};
	guest->inventory.items[gslot]
		= (struct item_data) {.id = BLOCK_DIRT, .count = 5};
	size_t entities_before = dict_entity_size(server->entities);
	struct server_rpc drop = {.type = SRPC_DROP_ITEM,
		.payload.drop_item = {.all = false}};
	assert(svin_rpc_send_from(live, &drop));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(guest->inventory.items[gslot].count == 4);          /* guest spent one */
	assert(host->inventory.items[hslot].count == 4);           /* host untouched */
	assert(dict_entity_size(server->entities) == entities_before + 1);
	assert(svin_rpc_send(&drop));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(host->inventory.items[hslot].count == 3);
	assert(guest->inventory.items[gslot].count == 4);
	assert(dict_entity_size(server->entities) == entities_before + 2);
	/* (c) stale generation drops: no third item, no inventory change. */
	assert(svin_rpc_send_from(stale, &drop));
	server_local_test_process_messages(server);
	assert(dict_entity_size(server->entities) == entities_before + 2);
	assert(guest->inventory.items[gslot].count == 4
		   && host->inventory.items[hslot].count == 3);

	/* ---- SRPC_USE_ITEM: boat admitted, projectiles + rod fail-closed ----- */
	host->inventory.items[hslot]
		= (struct item_data) {.id = ITEM_EGG, .count = 6};
	guest->inventory.items[gslot]
		= (struct item_data) {.id = ITEM_EGG, .count = 6};
	struct server_rpc use = {.type = SRPC_USE_ITEM,
		.payload.use_item = {.charge = 1.0F}};
	assert(svin_rpc_send_from(live, &use));
	server_local_test_process_messages(server);
	clin_drain_discard();
	/* S1-a2 OPEN: the throw is attributed to the acting actor, so the GUEST
	 * spends the guest egg and the host stack is untouched. */
	assert(guest->inventory.items[gslot].count == 5);
	assert(host->inventory.items[hslot].count == 6);
	assert(svin_rpc_send(&use));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(host->inventory.items[hslot].count == 5);   /* the host still throws */
	assert(guest->inventory.items[gslot].count == 5);
	/* S1-a2 OPEN: the rod cast records the ANGLER identity on the hook, so the
	 * GUEST owns a bobber and the host owns none. */
	guest->inventory.items[gslot]
		= (struct item_data) {.id = ITEM_FISHING_ROD, .count = 1};
	assert(svin_rpc_send_from(live, &use));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(guest->fish_entity_id != 0);
	assert(host->fish_entity_id == 0);
	{
		struct entity** hook
			= dict_entity_get(server->entities, guest->fish_entity_id);
		assert(hook && *hook && (*hook)->type == ENTITY_FISH);
		assert((*hook)->data.fish.angler_actor.slot == ACTOR_SLOT_GUEST);
		assert((*hook)->data.fish.angler_actor.generation == live.generation);
		/* Precondition for both ticks below: Beta only runs an entity's
		 * onUpdate when every chunk in its +/-32 block box exists
		 * (World.updateEntityWithOptionalForce(e, true) -> checkChunksExist,
		 * mirrored at server_local.c's entity pass). The guest stands near
		 * this fixture's resident edge, so make the hook's box resident and
		 * PROVE it -- otherwise the hook is frozen, never re-resolves its
		 * angler, and both checks would test nothing. */
		{
			w_coord_t hx_b = (w_coord_t)floor((double)(*hook)->pos[0]);
			w_coord_t hz_b = (w_coord_t)floor((double)(*hook)->pos[2]);
			for(w_coord_t cx = (w_coord_t)floor((hx_b - 32) / 16.0);
				cx <= (w_coord_t)floor((hx_b + 32) / 16.0); cx++)
				for(w_coord_t cz = (w_coord_t)floor((hz_b - 32) / 16.0);
					cz <= (w_coord_t)floor((hz_b + 32) / 16.0); cz++)
					if(!server_world_is_chunk_loaded(&server->worlds[0], cx, cz)) {
						struct server_chunk* box_chunk = NULL;
						assert(server_world_load_chunk(&server->worlds[0], cx, cz,
													   &box_chunk) && box_chunk);
					}
			assert(server_world_area_loaded_radius(&server->worlds[0], hx_b, 0,
												   hz_b, 32));
		}
		/* The hook survives an AI tick while its angler resolves... */
		(void)server_local_test_tick_entities(server);
		clin_drain_discard();
		assert(dict_entity_get(server->entities, guest->fish_entity_id));
		/* ...and is REMOVED once the angler generation moves on, without
		 * touching the host. */
		uint64_t saved = server->guest_generation;
		server->guest_generation = saved + 3;
		(void)server_local_test_tick_entities(server);
		clin_drain_discard();
		server->guest_generation = saved;
		struct entity** gone
			= dict_entity_get(server->entities, guest->fish_entity_id);
		assert(!gone || !*gone);
		assert(host->fish_entity_id == 0);
		guest->fish_entity_id = 0;
	}
	/* Leave the world exactly as this section found it: the caller's Leave
	 * sequence (and server_local_guest_settled behind it) must not inherit a
	 * live bobber or a thrown egg this case created. */
	{
		dict_entity_it_t it;
		dict_entity_it(it, server->entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			dict_entity_next(it);
			if(e && (e->type == ENTITY_FISH || e->type == ENTITY_EGG
					 || e->type == ENTITY_SNOWBALL || e->type == ENTITY_ARROW))
				e->delay_destroy = 0;
		}
		(void)server_local_test_tick_entities(server);
		clin_drain_discard();
	}

	/* ---- SRPC_BLOCK_DIG: admitted, with PER-ACTOR tool wear -------------- */
	assert(server_world_set_block(&server->worlds[0], 8, 66, 8,
								  (struct block_data) {.type = BLOCK_STONE}));
	assert(server_world_set_block(&server->worlds[0], 9, 66, 9,
								  (struct block_data) {.type = BLOCK_STONE}));
	assert(server_world_set_block(&server->worlds[0], 10, 66, 10,
								  (struct block_data) {.type = BLOCK_STONE}));
	host->inventory.items[hslot] = (struct item_data) {
		.id = ITEM_WOOD_PICKAXE, .durability = 0, .count = 1};
	guest->inventory.items[gslot] = (struct item_data) {
		.id = ITEM_WOOD_PICKAXE, .durability = 0, .count = 1};
	struct server_rpc dig = {.type = SRPC_BLOCK_DIG,
		.payload.block_dig
		= {.finished = true, .x = 8, .y = 66, .z = 8, .side = SIDE_TOP}};
	assert(svin_rpc_send_from(live, &dig));
	server_local_test_process_messages(server);
	clin_drain_discard();
	struct block_data dug;
	assert(server_world_get_block(&server->worlds[0], 8, 66, 8, &dug)
		   && dug.type == BLOCK_AIR);                    /* shared-world effect */
	assert(guest->inventory.items[gslot].durability == 1);   /* GUEST's tool wore */
	assert(host->inventory.items[hslot].durability == 0);    /* host's did not */
	dig.payload.block_dig.x = 9;
	dig.payload.block_dig.z = 9;
	assert(svin_rpc_send(&dig));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(server_world_get_block(&server->worlds[0], 9, 66, 9, &dug)
		   && dug.type == BLOCK_AIR);
	assert(host->inventory.items[hslot].durability == 1);
	assert(guest->inventory.items[gslot].durability == 1);
	/* (c) stale generation: the third block survives untouched. */
	dig.payload.block_dig.x = 10;
	dig.payload.block_dig.z = 10;
	assert(svin_rpc_send_from(stale, &dig));
	server_local_test_process_messages(server);
	assert(server_world_get_block(&server->worlds[0], 10, 66, 10, &dug)
		   && dug.type == BLOCK_STONE);
	assert(guest->inventory.items[gslot].durability == 1
		   && host->inventory.items[hslot].durability == 1);

	/* ---- SRPC_BLOCK_PLACE: ADMITTED for the guest (S1-a1b) ---------------
	 * Every place / activate / item-use hook now takes the acting actor, so
	 * the verb is no longer fail-closed as a whole. Astra gate-2 D1 required
	 * BOTH-owner coverage of placement obstruction, facing, activation,
	 * inventory and consumption -- each asserting the OTHER owner is
	 * untouched -- plus proof that the paths a parameter move does NOT
	 * authorize stayed refused. That is what follows. */
	struct server_rpc place = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place
		= {.x = 10, .y = 66, .z = 10, .side = SIDE_TOP}};
	/* Window opens need a reply hook on BOTH actors for the whole section: a
	 * non-host actor is refused a window outright without one
	 * (server_local_window_actor_valid), and a 27-slot chest emits OPEN +
	 * CHEST_SIZE messages in ONE handler call, which blocks the 16-token
	 * client inbox with no client thread to drain it. Same idiom as the
	 * minecart-chest case below (lines 239/295/320). Restored at the end. */
	void (*bp_host_reply)(const struct client_rpc*, void*)
		= host->inventory.reply;
	void* bp_host_reply_user = host->inventory.reply_user;
	void (*bp_guest_reply)(const struct client_rpc*, void*)
		= guest->inventory.reply;
	void* bp_guest_reply_user = guest->inventory.reply_user;
	host->inventory.reply = ignore_reply;
	guest->inventory.reply = ignore_reply;

	/* (b) the GUEST places on the shared world and pays from the GUEST's own
	 * inventory; the host stack is untouched. */
	host->inventory.items[hslot]
		= (struct item_data) {.id = BLOCK_DIRT, .count = 8};
	guest->inventory.items[gslot]
		= (struct item_data) {.id = BLOCK_DIRT, .count = 8};
	assert(svin_rpc_send_from(live, &place));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(server_world_get_block(&server->worlds[0], 10, 67, 10, &dug)
		   && dug.type == BLOCK_DIRT);               /* shared-world effect */
	assert(guest->inventory.items[gslot].count == 7); /* GUEST paid */
	assert(host->inventory.items[hslot].count == 8);  /* host did not */
	/* (a) the host identical request pays from the HOST instead. */
	place.payload.block_place.x = 11;
	place.payload.block_place.z = 11;
	assert(server_world_set_block(&server->worlds[0], 11, 66, 11,
								  (struct block_data) {.type = BLOCK_STONE}));
	assert(svin_rpc_send(&place));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(server_world_get_block(&server->worlds[0], 11, 67, 11, &dug)
		   && dug.type == BLOCK_DIRT);
	assert(host->inventory.items[hslot].count == 7);
	assert(guest->inventory.items[gslot].count == 7);
	/* (c) a stale generation still reaches nothing. */
	place.payload.block_place.x = 10;
	place.payload.block_place.z = 12;
	assert(server_world_set_block(&server->worlds[0], 10, 66, 12,
								  (struct block_data) {.type = BLOCK_STONE}));
	assert(svin_rpc_send_from(stale, &place));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(server_world_get_block(&server->worlds[0], 10, 67, 12, &dug)
		   && dug.type == BLOCK_AIR);
	assert(guest->inventory.items[gslot].count == 7
		   && host->inventory.items[hslot].count == 7);
	assert(server_world_set_block(&server->worlds[0], 10, 67, 10,
								  (struct block_data) {.type = BLOCK_AIR}));
	assert(server_world_set_block(&server->worlds[0], 11, 67, 11,
								  (struct block_data) {.type = BLOCK_AIR}));

	/* OBSTRUCTION sweeps EVERY present player body, not just the acting one
	 * (block_place_obstructed_for; S1 fix round 2, Astra call #8). Beta:
	 * canBlockBePlacedAt -> checkIfAABBIsClear ->
	 * getEntitiesWithinAABBExcludingEntity((Entity)null, box) excludes NOBODY
	 * and EntityPlayer carries preventEntitySpawning, so both players refuse.
	 * Driven through the REAL SRPC_BLOCK_PLACE dispatch, in both directions.
	 * A block lands at clicked + (0,1,0), so the cell a body must occupy to
	 * refuse it is the one directly above the clicked block. */
	{
		double g_saved_x = guest->x, g_saved_y = guest->y, g_saved_z = guest->z;
		double h_saved_x = host->x, h_saved_y = host->y, h_saved_z = host->z;
		assert(server_world_set_block(&server->worlds[0], 5, 66, 5,
									  (struct block_data) {.type = BLOCK_STONE}));
		place.payload.block_place.x = 5;
		place.payload.block_place.y = 66;
		place.payload.block_place.z = 5;
		/* (1) The HOST standing in the target DOES block the guest -- and Beta
		 * refuses before any consumption, so nothing is spent either. */
		host->x = 5.5; host->z = 5.5; host->y = 67.0 + PLAYER_EYE_HEIGHT;
		assert(svin_rpc_send_from(live, &place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server_world_get_block(&server->worlds[0], 5, 67, 5, &dug)
			   && dug.type == BLOCK_AIR); /* NOT placed through the host body */
		assert(guest->inventory.items[gslot].count == 7);
		assert(host->inventory.items[hslot].count == 7);
		/* (2) The GUEST standing there blocks the guest's own placement. */
		host->x = h_saved_x; host->y = h_saved_y; host->z = h_saved_z;
		guest->x = 5.5; guest->z = 5.5; guest->y = 67.0 + PLAYER_EYE_HEIGHT;
		assert(svin_rpc_send_from(live, &place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server_world_get_block(&server->worlds[0], 5, 67, 5, &dug)
			   && dug.type == BLOCK_AIR);
		assert(guest->inventory.items[gslot].count == 7);
		/* (3) ...and the mirror image: that guest body ALSO blocks the HOST. */
		assert(svin_rpc_send(&place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server_world_get_block(&server->worlds[0], 5, 67, 5, &dug)
			   && dug.type == BLOCK_AIR);
		assert(host->inventory.items[hslot].count == 7);
		/* (4) Non-vacuity: with BOTH bodies clear the same request lands. */
		guest->x = g_saved_x; guest->y = g_saved_y; guest->z = g_saved_z;
		assert(svin_rpc_send_from(live, &place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server_world_get_block(&server->worlds[0], 5, 67, 5, &dug)
			   && dug.type == BLOCK_DIRT);
		assert(guest->inventory.items[gslot].count == 6);
		assert(host->inventory.items[hslot].count == 7);
		assert(server_world_set_block(&server->worlds[0], 5, 67, 5,
									  (struct block_data) {.type = BLOCK_AIR}));
	}

	/* FACING: every yaw-derived placer reads the ACTING actor rx. Give the two
	 * actors opposite yaws and prove each placement carries its own placer
	 * quadrant, not the other one. furnace/dispenser use
	 * furnace_facing_meta(rx); chest derives its own cardinal from rx;
	 * stairs/pumpkin use the actor-to-cell delta; piston uses
	 * determineOrientation(actor eye, rx). */
	{
		host->rx = 0.0F;
		guest->rx = 180.0F;
		/* Both cells stay inside the one resident column, and the two rows
		 * are four apart so an adjacent-chest pair can never form. */
		static const struct { uint16_t id; int x; } facing_cases[] = {
			{BLOCK_FURNACE, 6}, {BLOCK_DISPENSER, 7}, {BLOCK_CHEST, 8},
			{BLOCK_WOODEN_STAIRS, 9}, {BLOCK_PUMPKIN, 13},
		};
		const int gz_row = 4, hz_row = 8;
		for(unsigned k = 0; k < sizeof(facing_cases) / sizeof(facing_cases[0]);
			k++) {
			int cell_x = facing_cases[k].x;
			for(unsigned who = 0; who < 2; who++) {
				int cx = cell_x, cz = who ? hz_row : gz_row;
				assert(server_world_set_block(&server->worlds[0], cx, 66, cz,
					(struct block_data) {.type = BLOCK_STONE}));
				struct server_player* who_actor = who ? host : guest;
				size_t slot = who ? hslot : gslot;
				who_actor->inventory.items[slot] = (struct item_data) {
					.id = facing_cases[k].id, .count = 2};
				place.payload.block_place.x = cx;
				place.payload.block_place.y = 66;
				place.payload.block_place.z = cz;
				if(who)
					assert(svin_rpc_send(&place));
				else
					assert(svin_rpc_send_from(live, &place));
				server_local_test_process_messages(server);
				clin_drain_discard();
				assert(server_world_get_block(&server->worlds[0], cx, 67, cz, &dug)
					   && dug.type == facing_cases[k].id);
			}
			/* THE POINT: two placements identical but for the acting actor
			 * carry DIFFERENT metadata, so the facing provably came from the
			 * actor and not from a shared host read. */
			struct block_data gblk, hblk;
			assert(server_world_get_block(&server->worlds[0], cell_x, 67, gz_row,
										  &gblk));
			assert(server_world_get_block(&server->worlds[0], cell_x, 67, hz_row,
										  &hblk));
			assert(gblk.metadata != hblk.metadata);
			/* Retire both through the real BLOCK_DIG verb, so the chest /
			 * furnace / dispenser tile-entity rows these placements
			 * registered are released by their own onBreak instead of being
			 * orphaned by a raw set_block. */
			struct server_rpc scrub = {.type = SRPC_BLOCK_DIG,
				.payload.block_dig = {.finished = true, .x = cell_x, .y = 67,
									  .side = SIDE_TOP}};
			for(unsigned row = 0; row < 2; row++) {
				scrub.payload.block_dig.z = row ? hz_row : gz_row;
				assert(svin_rpc_send(&scrub));
				server_local_test_process_messages(server);
				clin_drain_discard();
				assert(server_world_get_block(&server->worlds[0], cell_x, 67,
											  scrub.payload.block_dig.z, &dug)
					   && dug.type == BLOCK_AIR);
			}
		}
		host->rx = 0.0F;
		guest->rx = 0.0F;
	}

	/* ACTIVATION opens on the ACTING actor window: a guest right-click on a
	 * chest or a workbench must never touch the host active_inventory. Both
	 * windows are closed back through the real SRPC_WINDOW_CLOSE mailbox. */
	{
		/* Both containers are PLACED through the real verb, not written with
		 * server_world_set_block: a chest window is backed by the chest_pos
		 * row its placement registers, so a raw block write would leave
		 * inventory_create with nothing to back and the open would fail
		 * closed for BOTH actors -- proving nothing about ownership. */
		static const struct { uint16_t id; int z; uint8_t window; } act_cases[] = {
			{BLOCK_CHEST, 6, WINDOWC_CHEST},
			{BLOCK_WORKBENCH, 9, WINDOWC_CRAFTING},
		};
		for(unsigned k = 0; k < 2; k++) {
			assert(server_world_set_block(&server->worlds[0], 6, 66, act_cases[k].z,
				(struct block_data) {.type = BLOCK_STONE}));
			host->inventory.items[hslot]
				= (struct item_data) {.id = act_cases[k].id, .count = 2};
			guest->inventory.items[gslot] = (struct item_data) {0};
			place.payload.block_place.x = 6;
			place.payload.block_place.y = 66;
			place.payload.block_place.z = act_cases[k].z;
			assert(svin_rpc_send(&place));
			server_local_test_process_messages(server);
			clin_drain_discard();
			assert(server_world_get_block(&server->worlds[0], 6, 67,
										  act_cases[k].z, &dug)
				   && dug.type == act_cases[k].id);
			host->inventory.items[hslot] = (struct item_data) {0};
			struct server_rpc shut = {.type = SRPC_WINDOW_CLOSE,
				.payload.window_close = {.window = act_cases[k].window}};
			place.payload.block_place.y = 67;
			assert(svin_rpc_send_from(live, &place));
			server_local_test_process_messages(server);
			clin_drain_discard();
			assert(guest->active_inventory != &guest->inventory); /* GUEST */
			assert(host->active_inventory == &host->inventory);   /* not P1 */
			assert(svin_rpc_send_from(live, &shut));
			server_local_test_process_messages(server);
			clin_drain_discard();
			assert(guest->active_inventory == &guest->inventory);
			/* ...and the host own click opens the HOST window only. */
			assert(svin_rpc_send(&place));
			server_local_test_process_messages(server);
			clin_drain_discard();
			assert(host->active_inventory != &host->inventory);
			assert(guest->active_inventory == &guest->inventory);
			assert(svin_rpc_send(&shut));
			server_local_test_process_messages(server);
			clin_drain_discard();
			assert(host->active_inventory == &host->inventory);
			/* Retire the container through its own onBreak. */
			struct server_rpc scrub = {.type = SRPC_BLOCK_DIG,
				.payload.block_dig = {.finished = true, .x = 6, .y = 67,
									  .z = act_cases[k].z, .side = SIDE_TOP}};
			assert(svin_rpc_send(&scrub));
			server_local_test_process_messages(server);
			clin_drain_discard();
			assert(server_world_get_block(&server->worlds[0], 6, 67,
										  act_cases[k].z, &dug)
				   && dug.type == BLOCK_AIR);
		}
	}

	/* DOOR use must not move the HOST. block_door playability nudge is the one
	 * activation that WRITES a body position; it now writes the acting actor.
	 * Stand the host inside the door cell and have the GUEST toggle it. */
	{
		assert(server_world_set_block(&server->worlds[0], 4, 66, 3,
									  (struct block_data) {.type = BLOCK_STONE}));
		guest->inventory.items[gslot]
			= (struct item_data) {.id = ITEM_DOOR_WOOD, .count = 2};
		place.payload.block_place.x = 4;
		place.payload.block_place.y = 66;
		place.payload.block_place.z = 3;
		assert(svin_rpc_send_from(live, &place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server_world_get_block(&server->worlds[0], 4, 67, 3, &dug)
			   && dug.type == BLOCK_DOOR_WOOD);
		assert(guest->inventory.items[gslot].count == 1);
		double h_saved_x = host->x, h_saved_y = host->y, h_saved_z = host->z;
		host->x = 4.5; host->z = 3.5; host->y = 67.0 + PLAYER_EYE_HEIGHT;
		guest->inventory.items[gslot] = (struct item_data) {0};
		assert(server_world_get_block(&server->worlds[0], 4, 67, 3, &dug));
		uint8_t meta_before = dug.metadata;
		place.payload.block_place.y = 67;
		assert(svin_rpc_send_from(live, &place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server_world_get_block(&server->worlds[0], 4, 67, 3, &dug)
			   && dug.metadata != meta_before);  /* the door really toggled */
		assert(host->x == 4.5 && host->z == 3.5); /* and the HOST did not move */
		host->x = h_saved_x; host->y = h_saved_y; host->z = h_saved_z;
		/* Retire it through BLOCK_DIG: breaking the bottom half is what drops
		 * the top one (a raw set_block on the halves leaves the pair
		 * half-torn and its neighbour-change cascade unrun). */
		struct server_rpc scrub = {.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {.finished = true, .x = 4, .y = 67, .z = 3,
								  .side = SIDE_TOP}};
		assert(svin_rpc_send(&scrub));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server_world_get_block(&server->worlds[0], 4, 67, 3, &dug)
			   && dug.type == BLOCK_AIR);
		assert(server_world_get_block(&server->worlds[0], 4, 68, 3, &dug)
			   && dug.type == BLOCK_AIR);
	}

	/* CONSUMPTION debits the acting owner only for the non-block item placers
	 * too: the sign item (which also opens the ACTOR own OSK) and the bed
	 * ITEM (two cells, both swept against the acting body). */
	{
		assert(server_world_set_block(&server->worlds[0], 7, 66, 3,
									  (struct block_data) {.type = BLOCK_STONE}));
		host->inventory.items[hslot]
			= (struct item_data) {.id = ITEM_SIGN, .count = 3};
		guest->inventory.items[gslot]
			= (struct item_data) {.id = ITEM_SIGN, .count = 3};
		place.payload.block_place.x = 7;
		place.payload.block_place.y = 66;
		place.payload.block_place.z = 3;
		assert(svin_rpc_send_from(live, &place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server_world_get_block(&server->worlds[0], 7, 67, 3, &dug)
			   && dug.type == BLOCK_SIGN_POST);
		assert(guest->inventory.items[gslot].count == 2); /* GUEST paid */
		assert(host->inventory.items[hslot].count == 3);  /* host did not */
		/* Beta displayGUIEditSign tail opened the OSK -- on the GUEST. */
		assert(guest->active_inventory != &guest->inventory);
		assert(host->active_inventory == &host->inventory);
		struct server_rpc sign_shut = {.type = SRPC_WINDOW_CLOSE,
			.payload.window_close = {.window = WINDOWC_SIGN}};
		assert(svin_rpc_send_from(live, &sign_shut));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(guest->active_inventory == &guest->inventory);
		assert(server_world_set_block(&server->worlds[0], 7, 67, 3,
									  (struct block_data) {.type = BLOCK_AIR}));
		server->sign_pos[0].y = -1;

		/* Bed ITEM: placeable for the guest (head direction from the guest
		 * yaw). Bed USE stays fail-closed, asserted next. */
		assert(server_world_set_block(&server->worlds[0], 7, 66, 0,
									  (struct block_data) {.type = BLOCK_STONE}));
		assert(server_world_set_block(&server->worlds[0], 7, 66, 1,
									  (struct block_data) {.type = BLOCK_STONE}));
		guest->inventory.items[gslot]
			= (struct item_data) {.id = ITEM_BED, .count = 2};
		host->inventory.items[hslot]
			= (struct item_data) {.id = ITEM_BED, .count = 2};
		place.payload.block_place.x = 7;
		place.payload.block_place.z = 0;
		place.payload.block_place.y = 66;
		assert(svin_rpc_send_from(live, &place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server_world_get_block(&server->worlds[0], 7, 67, 0, &dug)
			   && dug.type == BLOCK_BED);
		assert(guest->inventory.items[gslot].count == 1);
		assert(host->inventory.items[hslot].count == 2);

		/* A1d (split-dimensions-plan-0924 row #3) SUPERSEDES the old
		 * gate-2 D1 fail-closed here: guest sleep is LIVE (owner-swapped
		 * proof: tests/test_owner_swap_bed_sleep_wake_spawn.c). A1d review C2:
		 * this leg no longer leans on a stale skylight cache. World.isDaytime
		 * reads the CACHED skylightSubtracted (w2), so each half recomputes it
		 * explicitly after writing world_time:
		 *   day   (6000)  -> NOT_POSSIBLE_NOW: neither body sleeps, bed stays;
		 *   night (18000) -> the GUEST sleeps in the bed it clicked; the host's
		 *                    sleep flag, timer and position are untouched. */
		double g_saved_x = guest->x, g_saved_y = guest->y, g_saved_z = guest->z;
		guest->x = 7.5; guest->z = 0.5; guest->y = 67.0 + PLAYER_EYE_HEIGHT;
		guest->inventory.items[gslot] = (struct item_data) {0};
		host->inventory.items[hslot] = (struct item_data) {0};
		place.payload.block_place.y = 67;
		{
			uint64_t time_saved = server->world_time;
			double hx0 = host->x, hy0 = host->y, hz0 = host->z;
			int32_t htimer0 = host->sleep_timer;
			server->world_time = 6000;
			server_local_skylight_recompute(server);
			assert(server_local_is_daytime(server));
			assert(svin_rpc_send_from(live, &place));
			server_local_test_process_messages(server);
			clin_drain_discard();
			assert(!guest->sleeping);   /* daytime: NOT_POSSIBLE_NOW */
			assert(!host->sleeping && host->sleep_timer == htimer0);
			assert(server_world_get_block(&server->worlds[0], 7, 67, 0, &dug)
				   && dug.type == BLOCK_BED);

			server->world_time = 18000;
			server_local_skylight_recompute(server);
			assert(!server_local_is_daytime(server));
			assert(svin_rpc_send_from(live, &place));
			server_local_test_process_messages(server);
			clin_drain_discard();
			assert(guest->sleeping && guest->sleep_timer == 0); /* guest sleeps */
			assert(!host->sleeping && host->sleep_timer == htimer0
				   && host->x == hx0 && host->y == hy0 && host->z == hz0);
			assert(server_world_get_block(&server->worlds[0], 7, 67, 0, &dug)
				   && dug.type == BLOCK_BED);
			/* Back to the leg's baseline for the checks below: awake guest,
			 * original clock and skylight (setup-state reset, not behaviour). */
			guest->sleeping = false;
			guest->sleep_timer = 0;
			/* The sleep snap authored a guest correction, which raises the
			 * guest's per-actor input barrier until the next server update
			 * clears it; this harness runs no update, so clear it the way
			 * that update would (the snap's new token stays in force). */
			guest->reject_pos_this_update = false;
			server->world_time = time_saved;
			server_local_skylight_recompute(server);
		}
		/* S1-a3 OPENED (Astra call #8 Q2): food heals the ACTING actor and
		 * only that actor. This case used to assert the fail-closed drop the
		 * S1-a1b guard produced; it is rewritten to the new behaviour rather
		 * than deleted, so the isolation it was really testing -- a bite by
		 * one body never moves the other body health -- is still asserted,
		 * now in BOTH directions. Beta has no hunger: ItemFood calls
		 * player.heal() straight away [740c583]. */
		{
			short h_before = host->health, g_before = guest->health;
			short half = (short)(MAX_PLAYER_HEALTH / 2);
			host->health = half;
			guest->health = half;
			guest->inventory.items[gslot]
				= (struct item_data) {.id = ITEM_BREAD, .count = 2};
			assert(server_world_set_block(&server->worlds[0], 9, 66, 3,
				(struct block_data) {.type = BLOCK_STONE}));
			place.payload.block_place.x = 9;
			place.payload.block_place.y = 66;
			place.payload.block_place.z = 3;
			assert(svin_rpc_send_from(live, &place));
			server_local_test_process_messages(server);
			clin_drain_discard();
			assert(guest->health > half);   /* the GUEST ate and healed */
			assert(host->health == half);   /* P1 hearts untouched */
			assert(guest->inventory.items[gslot].count == 1); /* consumed one */
			short g_after_bite = guest->health;
			host->inventory.items[hslot]
				= (struct item_data) {.id = ITEM_BREAD, .count = 2};
			assert(svin_rpc_send(&place));
			server_local_test_process_messages(server);
			clin_drain_discard();
			assert(host->health > half);
			assert(guest->health == g_after_bite); /* and back the other way */
			assert(host->inventory.items[hslot].count == 1);
			/* Beta refuses the bite at full health and consumes nothing --
			 * per actor, so a full guest cannot spend a loaf. */
			guest->health = MAX_PLAYER_HEALTH;
			guest->inventory.items[gslot]
				= (struct item_data) {.id = ITEM_BREAD, .count = 2};
			assert(svin_rpc_send_from(live, &place));
			server_local_test_process_messages(server);
			clin_drain_discard();
			assert(guest->health == MAX_PLAYER_HEALTH);
			assert(guest->inventory.items[gslot].count == 2);
			host->health = h_before;
			guest->health = g_before;
		}
		guest->x = g_saved_x; guest->y = g_saved_y; guest->z = g_saved_z;
		/* Retire the bed through BLOCK_DIG: breaking either half drops the
		 * other, and a raw set_block on a cell that is ALREADY air returns
		 * false (Beta Chunk.setBlockIDWithMetadata is a no-op for an
		 * identical id+metadata), so the cascade must do the work. */
		struct server_rpc scrub = {.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {.finished = true, .x = 7, .y = 67, .z = 0,
								  .side = SIDE_TOP}};
		assert(svin_rpc_send(&scrub));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server_world_get_block(&server->worlds[0], 7, 67, 0, &dug)
			   && dug.type == BLOCK_AIR);
		assert(server_world_get_block(&server->worlds[0], 7, 67, 1, &dug)
			   && dug.type == BLOCK_AIR);
	}
	host->inventory.items[hslot] = (struct item_data) {0};
	guest->inventory.items[gslot] = (struct item_data) {0};
	host->inventory.reply = bp_host_reply;
	host->inventory.reply_user = bp_host_reply_user;
	guest->inventory.reply = bp_guest_reply;
	guest->inventory.reply_user = bp_guest_reply_user;

	/* ---- SRPC_ENTITY_ATTACK: guest-eye reach, monster branch fail-closed - */
	struct entity* near_guest = server_local_spawn_boat(
		(vec3) {(float)guest->x + 1.0F, 66.0F, (float)guest->z}, server);
	assert(near_guest && near_guest->type == ENTITY_BOAT);
	struct entity* near_host = server_local_spawn_boat(
		(vec3) {(float)host->x + 1.0F, 66.0F, (float)host->z}, server);
	assert(near_host);
	/* Both boats sit well below the actors' eye line but inside 6.0 blocks of
	 * their own actor and outside the other's. */
	near_guest->pos[1] = (float)(guest->y - 1.0);
	near_host->pos[1] = (float)(host->y - 1.0);
	struct server_rpc hit = {.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack = {.entity_id = near_guest->id}};
	int16_t guest_boat_dmg = near_guest->data.boat.damage_taken;
	int16_t host_boat_dmg = near_host->data.boat.damage_taken;
	/* (e) the HOST cannot reach the boat beside the GUEST... */
	assert(svin_rpc_send(&hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(near_guest->data.boat.damage_taken == guest_boat_dmg);
	/* ...but the guest can, measured from its OWN eye. */
	assert(svin_rpc_send_from(live, &hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(near_guest->data.boat.damage_taken > guest_boat_dmg);
	/* ...and symmetrically the guest cannot reach the boat beside the host. */
	hit.payload.entity_attack.entity_id = near_host->id;
	assert(svin_rpc_send_from(live, &hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(near_host->data.boat.damage_taken == host_boat_dmg);
	assert(svin_rpc_send(&hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(near_host->data.boat.damage_taken > host_boat_dmg);
	/* S1-a2: the MONSTER branch is OPEN for the guest now that the retaliation
	 * latch carries the attacker IDENTITY. A PASSIVE animal takes the damage
	 * and (faithfully) never latches a target at all, so this case pins the
	 * damage half and the "no target for a passive" half; the hostile identity
	 * case is test_guest_aggro_identity below. */
	struct entity* pig = server_local_spawn_monster_at(
		(vec3) {(float)guest->x + 1.0F, (float)(guest->y - 1.0), (float)guest->z},
		MONSTER_PIG, server);
	assert(pig && pig->type == ENTITY_MONSTER);
	int32_t pig_health = pig->health;
	hit.payload.entity_attack.entity_id = pig->id;
	assert(svin_rpc_send_from(live, &hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(pig->health < pig_health);
	assert(pig->data.monster.aggro_time == 0); /* Beta: animals never retaliate */
	assert(actor_ident_is_host(pig->data.monster.target_actor));
	/* ...and the host, nine blocks away, cannot reach the same animal. */
	pig_health = pig->health;
	assert(svin_rpc_send(&hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(pig->health == pig_health);
	/* (c) a stale generation cannot reach any of it either. */
	hit.payload.entity_attack.entity_id = near_guest->id;
	guest_boat_dmg = near_guest->data.boat.damage_taken;
	assert(svin_rpc_send_from(stale, &hit));
	server_local_test_process_messages(server);
	assert(near_guest->data.boat.damage_taken == guest_boat_dmg);

	/* ---- SRPC_ENTITY_USE: cow milking and boat riding admitted ----------- */
	struct entity* cow = server_local_spawn_monster_at(
		(vec3) {(float)guest->x + 1.0F, (float)(guest->y - 1.0), (float)guest->z},
		MONSTER_COW, server);
	assert(cow && cow->type == ENTITY_MONSTER);
	host->inventory.items[hslot]
		= (struct item_data) {.id = ITEM_BUCKET, .count = 1};
	guest->inventory.items[gslot]
		= (struct item_data) {.id = ITEM_BUCKET, .count = 1};
	struct server_rpc interact = {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = cow->id}};
	assert(svin_rpc_send_from(live, &interact));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(guest->inventory.items[gslot].id == ITEM_MILK_BUCKET);
	assert(host->inventory.items[hslot].id == ITEM_BUCKET);  /* host untouched */
	/* (e) the host cannot reach that same cow at all. */
	assert(svin_rpc_send(&interact));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(host->inventory.items[hslot].id == ITEM_BUCKET);
	/* R2: production guest admission preserves the host's independent latch. */
	interact.payload.entity_use.entity_id = near_guest->id;
	interact.payload.entity_use.desired_occupied = true;
	assert(svin_rpc_send_from(live, &interact));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(near_guest->data.boat.occupied && !host->riding_boat
		   && guest->riding_boat
		   && near_guest->data.boat.occupant_id == server->guest_entity_id);
	interact.payload.entity_use.desired_occupied = false;
	assert(svin_rpc_send_from(live, &interact));
	server_local_test_process_messages(server); clin_drain_discard();
	assert(!near_guest->data.boat.occupied && !guest->riding_boat);

	/* ---- SRPC_ENTITY_USE, both admitted MINECART branches (R-S1a1 1+2) --- */
	/* (1) minecart_type 1 (CHEST cart) -- the one admitted verb that OPENS A
	 * WINDOW, and the regression finding 1 caught: the branch used to call the
	 * actor-free wrapper server_local_open_entity_inventory(), which hard-codes
	 * `&s->player`, so a GUEST cart click seized the HOST's active_inventory
	 * and pushed CRPC_OPEN_WINDOW + all 27 slots down the host's untagged
	 * route. The host-screen assert below is the one that trips on the
	 * unfixed code; the guest-window assert proves the open is not merely
	 * suppressed. */
	struct entity* chest_cart = server_local_spawn_minecart_type(
		(vec3) {(float)guest->x + 1.0F, (float)(guest->y - 1.0),
				(float)guest->z}, server, 1);
	assert(chest_cart && chest_cart->type == ENTITY_MINECART
		   && chest_cart->data.minecart.minecart_type == 1);
	assert(host->active_inventory == &host->inventory
		   && guest->active_inventory == &guest->inventory);
	/* Opening a 27-slot window emits CRPC_OPEN_WINDOW + CHEST_SIZE slot
	 * messages in ONE handler call, and clin_rpc_send_tagged BLOCKS on the
	 * 16-token client inbox -- with no client thread to drain it mid-handler,
	 * EITHER route (the host's untagged clin_rpc_send or the guest's
	 * guest_reply) deadlocks the process before any assert can run. So both
	 * actors get the sibling cases' ignore_reply for the duration (lines
	 * 239/295/320 use the same idiom), which makes the ACTOR-OWNERSHIP
	 * question -- whose active_inventory the open lands on -- observable as a
	 * plain assert instead of a hang. Restored below. */
	void (*host_reply_saved)(const struct client_rpc*, void*)
		= host->inventory.reply;
	void* host_reply_user_saved = host->inventory.reply_user;
	void (*guest_reply_saved)(const struct client_rpc*, void*)
		= guest->inventory.reply;
	void* guest_reply_user_saved = guest->inventory.reply_user;
	host->inventory.reply = ignore_reply;
	guest->inventory.reply = ignore_reply;
	interact.payload.entity_use.desired_occupied = false;
	interact.payload.entity_use.entity_id = chest_cart->id;
	assert(svin_rpc_send_from(live, &interact));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(host->active_inventory == &host->inventory);   /* P1's screen alone */
	assert(guest->active_inventory != &guest->inventory); /* ...the guest's did */
	assert(guest->active_inventory->logic == &inventory_logic_minecart_chest
		   && guest->active_inventory->capacity == CHEST_SIZE
		   && guest->active_inventory->entity_id == chest_cart->id
		   && guest->active_inventory->owner == guest);
	/* Closing goes back through the real mailbox too (SRPC_WINDOW_CLOSE is
	 * already per-actor), so the window is retired the way a player retires
	 * it -- not by reaching into the actor struct. */
	struct server_rpc cart_close = {.type = SRPC_WINDOW_CLOSE,
		.payload.window_close = {.window = WINDOWC_MINECART_CHEST}};
	assert(svin_rpc_send_from(live, &cart_close));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(guest->active_inventory == &guest->inventory
		   && host->active_inventory == &host->inventory);
	/* (e) ...and the host, nine blocks away, cannot open that cart at all. */
	assert(svin_rpc_send(&interact));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(host->active_inventory == &host->inventory
		   && guest->active_inventory == &guest->inventory);
	host->inventory.reply = host_reply_saved;
	host->inventory.reply_user = host_reply_user_saved;
	guest->inventory.reply = guest_reply_saved;
	guest->inventory.reply_user = guest_reply_user_saved;

	/* (2) minecart_type 2 (FURNACE cart): the coal must leave the ACTING
	 * actor's hotbar and the push vector must aim away from the ACTING actor.
	 * This branch already read `player` at db3db335 -- it had no defect, only
	 * no coverage (finding 2's second half), so this case pins it rather than
	 * repairing it. */
	struct entity* fuel_cart = server_local_spawn_minecart_type(
		(vec3) {(float)guest->x + 1.0F, (float)(guest->y - 1.0),
				(float)guest->z}, server, 2);
	assert(fuel_cart && fuel_cart->type == ENTITY_MINECART
		   && fuel_cart->data.minecart.minecart_type == 2);
	host->inventory.items[hslot]
		= (struct item_data) {.id = ITEM_COAL, .count = 3};
	guest->inventory.items[gslot]
		= (struct item_data) {.id = ITEM_COAL, .count = 3};
	int32_t fuel_before = fuel_cart->data.minecart.fuel;
	interact.payload.entity_use.entity_id = fuel_cart->id;
	assert(svin_rpc_send_from(live, &interact));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(guest->inventory.items[gslot].id == ITEM_COAL
		   && guest->inventory.items[gslot].count == 2);  /* GUEST paid the coal */
	assert(host->inventory.items[hslot].id == ITEM_COAL
		   && host->inventory.items[hslot].count == 3);   /* host's stack intact */
	assert(fuel_cart->data.minecart.fuel == fuel_before + 1200);
	assert(fabs((double)fuel_cart->data.minecart.push_x
				- (fuel_cart->pos[0] - guest->x)) < 1e-6);
	assert(fabs((double)fuel_cart->data.minecart.push_z
				- (fuel_cart->pos[2] - guest->z)) < 1e-6);
	/* The same aim is provably NOT the host's: nine blocks apart, so a
	 * host-derived push would differ by far more than the tolerance. */
	assert(fabs((double)fuel_cart->data.minecart.push_x
				- (fuel_cart->pos[0] - host->x)) > 1.0);
	/* (e) the host cannot reach it: no second coal, no second refuel, no
	 * re-aim. */
	assert(svin_rpc_send(&interact));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(host->inventory.items[hslot].count == 3
		   && guest->inventory.items[gslot].count == 2
		   && fuel_cart->data.minecart.fuel == fuel_before + 1200);
	assert(fabs((double)fuel_cart->data.minecart.push_x
				- (fuel_cart->pos[0] - guest->x)) < 1e-6);
	/* (c) a stale generation reaches neither cart. */
	interact.payload.entity_use.entity_id = chest_cart->id;
	assert(svin_rpc_send_from(stale, &interact));
	server_local_test_process_messages(server);
	assert(guest->active_inventory == &guest->inventory
		   && host->active_inventory == &host->inventory);
	interact.payload.entity_use.entity_id = fuel_cart->id;
	assert(svin_rpc_send_from(stale, &interact));
	server_local_test_process_messages(server);
	assert(guest->inventory.items[gslot].count == 2
		   && fuel_cart->data.minecart.fuel == fuel_before + 1200);

	/* ---- R-S1a1b A1/A2: the BLOCK_PLACE trailing use-item branches -------
	 * Beta Minecraft.clickMouse still runs sendUseItem when NEITHER the block
	 * activation NOR the held item consumed the click, so an egg/snowball or a
	 * fishing rod aimed AT an unhandled block still throws/casts. Those two
	 * branches were unreachable for the guest before S1-a1b (the whole-verb
	 * guard dropped the click) and are reachable now, so they must go through
	 * the SAME actor-aware helpers the air-use verb uses -- no second
	 * implementation, no separate gate. */
	{
		host->x = 3.5; host->y = 72.0; host->z = 3.5;
		guest->x = 12.5; guest->y = 72.0; guest->z = 12.5;
		assert(server_world_set_block(&server->worlds[0], 12, 66, 11,
			(struct block_data) {.type = BLOCK_STONE}));
		/* server_world_set_block is a no-op (false) for an identical id, so
		 * the clicked face is CHECKED clear rather than written clear. */
		(void)server_world_set_block(&server->worlds[0], 12, 67, 11,
			(struct block_data) {.type = BLOCK_AIR});
		assert(server_world_get_block(&server->worlds[0], 12, 67, 11, &dug)
			   && dug.type == BLOCK_AIR);
		place.payload.block_place.x = 12;
		place.payload.block_place.y = 66;
		place.payload.block_place.z = 11;
		place.payload.block_place.side = SIDE_TOP;
		/* Stone has no onRightClick* and an egg/rod has no onUseOnBlock or
		 * onItemPlace, so `used` and `placed` are both false here. */

		/* (A1) the GUEST throws, and the projectile carries the GUEST. */
		host->inventory.items[hslot]
			= (struct item_data) {.id = ITEM_EGG, .count = 4};
		guest->inventory.items[gslot]
			= (struct item_data) {.id = ITEM_EGG, .count = 4};
		assert(svin_rpc_send_from(live, &place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(guest->inventory.items[gslot].count == 3); /* the GUEST paid */
		assert(host->inventory.items[hslot].count == 4);  /* the host did not */
		/* ...and nothing was placed at the clicked face. */
		assert(server_world_get_block(&server->worlds[0], 12, 67, 11, &dug)
			   && dug.type == BLOCK_AIR);
		struct entity* thrown = NULL;
		unsigned thrown_count = 0;
		{
			dict_entity_it_t it;
			dict_entity_it(it, server->entities);
			while(!dict_entity_end_p(it)) {
				struct entity* e = dict_entity_ref(it)->value;
				dict_entity_next(it);
				if(e && (e->type == ENTITY_EGG || e->type == ENTITY_SNOWBALL)) {
					thrown = e;
					thrown_count++;
				}
			}
		}
		assert(thrown_count == 1 && thrown);
		assert(thrown->data.egg.thrower_actor.slot == ACTOR_SLOT_GUEST);
		assert(thrown->data.egg.thrower_actor.generation == live.generation);
		/* (B3) the exclusion the egg tick applies to the swept HOST body: the
		 * host did not throw this, so it is a valid target from tick 0 (Beta
		 * EntityThrowable excludes the THROWER only, while ticksInAir < 5). */
		assert(!projectile_shooter_is_this_body(
			thrown->data.egg.owner_id,
			actor_ident_is_host(thrown->data.egg.thrower_actor)));
		assert(arrow_should_hit_actor(projectile_shooter_is_this_body(
					   thrown->data.egg.owner_id,
					   actor_ident_is_host(thrown->data.egg.thrower_actor)),
			   0));
		thrown->delay_destroy = 0;
		(void)server_local_test_tick_entities(server);
		clin_drain_discard();

		/* (A1, host leg) the host's identical click still throws the HOST's
		 * own egg, and its five-tick self-grace is unchanged. */
		assert(svin_rpc_send(&place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(host->inventory.items[hslot].count == 3);
		assert(guest->inventory.items[gslot].count == 3);
		thrown = NULL;
		thrown_count = 0;
		{
			dict_entity_it_t it;
			dict_entity_it(it, server->entities);
			while(!dict_entity_end_p(it)) {
				struct entity* e = dict_entity_ref(it)->value;
				dict_entity_next(it);
				if(e && (e->type == ENTITY_EGG || e->type == ENTITY_SNOWBALL)) {
					thrown = e;
					thrown_count++;
				}
			}
		}
		assert(thrown_count == 1 && thrown);
		assert(actor_ident_is_host(thrown->data.egg.thrower_actor));
		assert(!arrow_should_hit_actor(projectile_shooter_is_this_body(
						thrown->data.egg.owner_id,
						actor_ident_is_host(thrown->data.egg.thrower_actor)),
				0));
		assert(arrow_should_hit_actor(projectile_shooter_is_this_body(
					   thrown->data.egg.owner_id,
					   actor_ident_is_host(thrown->data.egg.thrower_actor)),
			   ARROW_OWNER_GRACE_TICKS));
		thrown->delay_destroy = 0;
		(void)server_local_test_tick_entities(server);
		clin_drain_discard();

		/* (A2) the same trailing branch casts the ROD. The bobber id lands on
		 * the GUEST body, the hook records the GUEST as its angler, and the
		 * stuck-in-ground despawn clears the ANGLER's id -- not the host's.
		 * That last clear is the world-save regression: while a guest bobber id
		 * is set, server_local_guest_checkpoint refuses, and it is the first
		 * line of server_local_write_level_state. */
		host->inventory.items[hslot]
			= (struct item_data) {.id = ITEM_FISHING_ROD, .count = 1};
		guest->inventory.items[gslot]
			= (struct item_data) {.id = ITEM_FISHING_ROD, .count = 1};
		assert(host->fish_entity_id == 0 && guest->fish_entity_id == 0);
		assert(svin_rpc_send_from(live, &place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(guest->fish_entity_id != 0);
		assert(host->fish_entity_id == 0);
		uint32_t guest_hook_id = guest->fish_entity_id;
		struct entity** hookp = dict_entity_get(server->entities, guest_hook_id);
		assert(hookp && *hookp && (*hookp)->type == ENTITY_FISH);
		assert((*hookp)->data.fish.angler_actor.slot == ACTOR_SLOT_GUEST);
		assert((*hookp)->data.fish.angler_actor.generation == live.generation);
		/* (B7) a HOST bobber id must SURVIVE the guest hook's own clear -- the
		 * cross-actor half the S1-a2 test asserted vacuously (0 before and 0
		 * after). Park a nonzero id on the host and drive the guest hook to its
		 * 1200-tick stuck despawn. */
		const uint32_t host_bobber_sentinel = 4242u;
		host->fish_entity_id = host_bobber_sentinel;
		(*hookp)->data.fish.in_ground = true;
		(*hookp)->data.fish.ticks_in_ground = FISH_STUCK_DESPAWN_TICKS - 1;
		(void)server_local_test_tick_entities(server);
		clin_drain_discard();
		{
			struct entity** gone
				= dict_entity_get(server->entities, guest_hook_id);
			assert(!gone || !*gone);
		}
		assert(guest->fish_entity_id == 0);                   /* ANGLER cleared */
		assert(host->fish_entity_id == host_bobber_sentinel); /* host untouched */
		host->fish_entity_id = 0;
		host->inventory.items[hslot] = (struct item_data) {0};
		guest->inventory.items[gslot] = (struct item_data) {0};
		/* ...so the world save is not wedged: both gates pass. */
		assert(server_local_guest_checkpoint(server));
		assert(server_local_test_save_player_state(server));
	}
	/* ---- R-S1a1b A3: a GUEST-crafted map centres on the GUEST ------------
	 * Beta ItemMap.onCreated stamps `var5.xCenter = floor(player.posX)` from
	 * the CRAFTER [740c583]. The map recipe is 8 paper + a compass, i.e. 3x3,
	 * so it was out of reach of the guest's own 2x2 grid until S1-a1b admitted
	 * the guest workbench window -- and the hook still read s->player, so a
	 * guest-crafted map came out centred on the HOST. onCreated now takes the
	 * acting actor (the craft dispatch passes the inventory owning body). */
	{
		void (*a3_host_reply)(const struct client_rpc*, void*)
			= host->inventory.reply;
		void* a3_host_reply_user = host->inventory.reply_user;
		void (*a3_guest_reply)(const struct client_rpc*, void*)
			= guest->inventory.reply;
		void* a3_guest_reply_user = guest->inventory.reply_user;
		host->inventory.reply = ignore_reply;
		guest->inventory.reply = ignore_reply;
		host->x = 3.5; host->y = 72.0; host->z = 3.5;
		guest->x = 12.5; guest->y = 72.0; guest->z = 12.5;
		/* The HOST places the bench (a window needs the chest/bench block to
		 * exist through the real verb), the GUEST opens it. */
		assert(server_world_set_block(&server->worlds[0], 8, 66, 12,
			(struct block_data) {.type = BLOCK_STONE}));
		(void)server_world_set_block(&server->worlds[0], 8, 67, 12,
			(struct block_data) {.type = BLOCK_AIR});
		assert(server_world_get_block(&server->worlds[0], 8, 67, 12, &dug)
			   && dug.type == BLOCK_AIR);
		host->inventory.items[hslot]
			= (struct item_data) {.id = BLOCK_WORKBENCH, .count = 1};
		place.payload.block_place.x = 8;
		place.payload.block_place.y = 66;
		place.payload.block_place.z = 12;
		place.payload.block_place.side = SIDE_TOP;
		assert(svin_rpc_send(&place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server_world_get_block(&server->worlds[0], 8, 67, 12, &dug)
			   && dug.type == BLOCK_WORKBENCH);
		host->inventory.items[hslot] = (struct item_data) {0};
		place.payload.block_place.y = 67;
		assert(svin_rpc_send_from(live, &place));
		server_local_test_process_messages(server);
		clin_drain_discard();
		struct inventory* bench = guest->active_inventory;
		assert(bench != &guest->inventory && bench->owner == guest);
		assert(host->active_inventory == &host->inventory);
		for(size_t k = 1; k <= 9; k++)
			bench->items[k] = (struct item_data) {
				.id = (uint16_t)(k == 5 ? ITEM_COMPASS : ITEM_PAPER), .count = 1};
		inventory_click_post(bench, WINDOW_TYPE_WORKBENCH, 0, NULL);
		assert(bench->items[0].id == ITEM_MAP);
		struct server_rpc craft = {.type = SRPC_WINDOW_CLICK,
			.payload.window_click = {.window = WINDOWC_CRAFTING, .slot = 0,
				.action_id = 7, .expected = bench->items[0]}};
		assert(svin_rpc_send_from(live, &craft));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(bench->picked_item.id == ITEM_MAP
			   && bench->picked_item.count == 1);
		struct map_data* crafted
			= item_map_cache_resident(server, bench->picked_item.durability);
		assert(crafted);
		/* floor(12.5) == 12, and the host stands at 3.5: a host stamp would
		 * read 3/3 here. */
		assert(crafted->x_center == 12 && crafted->z_center == 12);
		assert(crafted->dimension == (int8_t)guest->dimension);
		/* Retire the window and the bench. The craft consumed all nine input
		 * cells, so only the cursor would spill on close. */
		bench->picked_item = (struct item_data) {0};
		struct server_rpc bench_shut = {.type = SRPC_WINDOW_CLOSE,
			.payload.window_close = {.window = WINDOWC_CRAFTING}};
		assert(svin_rpc_send_from(live, &bench_shut));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(guest->active_inventory == &guest->inventory);
		struct server_rpc bench_scrub = {.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {.finished = true, .x = 8, .y = 67, .z = 12,
								  .side = SIDE_TOP}};
		assert(svin_rpc_send(&bench_scrub));
		server_local_test_process_messages(server);
		clin_drain_discard();
		host->inventory.reply = a3_host_reply;
		host->inventory.reply_user = a3_host_reply_user;
		guest->inventory.reply = a3_guest_reply;
		guest->inventory.reply_user = a3_guest_reply_user;
	}
	/* ---- R-S1a2 B4: force-dismounting a DEPARTED rider keeps the host ----
	 * entity_boat.c's `if(!rider) server_local_set_boat_occupancy(s, e, false)`
	 * exists exactly for the rider whose body is gone, and it routes through
	 * the host wrapper -- so with `rider` keeping the caller's argument, that
	 * release cleared the HOST's riding_boat (the Beta footstep-odometer gate)
	 * and zeroed the host's steering input, for a boat the GUEST had been
	 * riding. The departed-lifetime fixture is stamped through the occupancy
	 * writer; the DISMOUNT is the real production call. */
	{
		struct entity* boat = server_local_spawn_boat(
			(vec3) {(float)guest->x, (float)(guest->y - 1.0), (float)guest->z},
			server);
		assert(boat && boat->type == ENTITY_BOAT);
		clin_drain_discard();
		/* The guest is the recorded rider... */
		server_local_set_boat_occupancy_for(server, boat, guest, true);
		clin_drain_discard();
		assert(boat->data.boat.occupied && guest->riding_boat);
		assert(boat->data.boat.rider_actor.slot == ACTOR_SLOT_GUEST);
		assert(boat->data.boat.rider_actor.generation == live.generation);
		assert(!host->riding_boat);
		/* ...and the HOST is riding a boat of its own, with live steering. */
		host->riding_boat = true;
		host->rider_vel_x = 0.25f;
		host->rider_vel_z = -0.5f;
		/* The guest session moves on (leave / rejoin under a new generation),
		 * so the recorded rider no longer resolves, and the boat is released
		 * through the very wrapper entity_boat.c uses. */
		uint64_t saved_gen = server->guest_generation;
		server->guest_generation = saved_gen + 5;
		assert(!server_local_actor_resolve(server, boat->data.boat.rider_actor));
		server_local_set_boat_occupancy(server, boat, false);
		clin_drain_discard();
		server->guest_generation = saved_gen;
		assert(!boat->data.boat.occupied && boat->data.boat.occupant_id == 0);
		assert(actor_ident_is_host(boat->data.boat.rider_actor));
		/* The HOST kept its own riding state and steering input. */
		assert(host->riding_boat);
		assert(host->rider_vel_x == 0.25f && host->rider_vel_z == -0.5f);
		host->riding_boat = false;
		host->rider_vel_x = host->rider_vel_z = 0.0f;
		guest->riding_boat = false;
		boat->delay_destroy = 0;
		(void)server_local_test_tick_entities(server);
		clin_drain_discard();
	}
	/* Retire the fixtures this case introduced; the caller's Leave asserts an
	 * empty shared entity store. */
	uint32_t ids[6] = {near_guest->id, near_host->id, pig->id, cow->id,
					   chest_cart->id, fuel_cart->id};
	for(unsigned k = 0; k < 6; k++) {
		struct entity** p = dict_entity_get(server->entities, ids[k]);
		if(p && *p) {
			free(*p);
			dict_entity_erase(server->entities, ids[k]);
		}
	}
	dict_entity_it_t sweep;
	dict_entity_it(sweep, server->entities);
	while(!dict_entity_end_p(sweep)) {
		struct entity* e = dict_entity_ref(sweep)->value;
		uint32_t id = dict_entity_ref(sweep)->key;
		dict_entity_next(sweep);
		if(e && e->type == ENTITY_ITEM) {
			free(e);
			dict_entity_erase(server->entities, id);
		}
	}
	host->inventory.items[hslot] = (struct item_data) {0};
	guest->inventory.items[gslot] = (struct item_data) {0};
	host->x = hx; host->y = hy; host->z = hz; host->rx = hrx; host->ry = hry;
	guest->x = gx; guest->y = gy; guest->z = gz; guest->rx = grx;
	guest->ry = gry;
	clin_drain_discard();
	puts("PASS guest gameplay verbs: per-actor hotbar/drop/dig (incl. tool "
		 "wear), chest-cart window opens on the GUEST (host screen untouched) "
		 "and furnace-cart coal/push come from the GUEST, guest-eye entity "
		 "reach, host isolation, stale-generation and "
		 "pre-confirm/unadmitted/dead/sleeping drops, per-actor BLOCK_PLACE "
		 "(obstruction body, facing metadata, chest/workbench window, door "
		 "nudge, sign/bed consumption), and the gate-2 fail-closed "
		 "bed-sleep/food-heal/projectile/rod/monster paths");
}

/* Both local players are server_player bodies, not entries in s->entities.
 * A valid tagged attack must reach the peer's authoritative health; distance
 * and a solid eye-line wall must still refuse it.  The old generic entity
 * lookup rejects host ID 0 and cannot find the guest body either. */
static void test_local_peer_pvp(struct server_local* server,
	struct server_rpc_source live) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	assert(guest && server->guest_gameplay && server->guest_confirmed);
	assert(server->guest_entity_id && server->guest_entity_id != UINT32_MAX);
	assert(server_local_world_of(server, host)
		== server_local_world_of(server, guest));
	struct server_world* world = server_local_world_of(server, host);
	assert(world);
	double hx = host->x, hy = host->y, hz = host->z;
	double gx = guest->x, gy = guest->y, gz = guest->z;
	short hh = host->health, gh = guest->health;
	short hres = host->hurt_resist_time, gres = guest->hurt_resist_time;
	short hlast = host->last_damage, glast = guest->last_damage;
	bool creative = server->creative_mode;
	struct block_data wall_low, wall_high;
	assert(server_world_get_block(world, 8, 72, 8, &wall_low));
	assert(server_world_get_block(world, 8, 73, 8, &wall_high));
	struct item_data guest_armor[INVENTORY_SIZE_ARMOR];
	memcpy(guest_armor, guest->inventory.items + INVENTORY_SLOT_ARMOR,
		sizeof(guest_armor));
	memset(guest->inventory.items + INVENTORY_SLOT_ARMOR, 0,
		sizeof(guest_armor));
	(void)server_world_set_block(world, 8, 72, 8,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(world, 8, 73, 8,
		(struct block_data) {.type = BLOCK_AIR});
	struct block_data open_low, open_high;
	assert(server_world_get_block(world, 8, 72, 8, &open_low)
		&& open_low.type == BLOCK_AIR);
	assert(server_world_get_block(world, 8, 73, 8, &open_high)
		&& open_high.type == BLOCK_AIR);
	host->x = 7.5; host->y = 72.0; host->z = 8.5;
	guest->x = 9.5; guest->y = 72.0; guest->z = 8.5;
	host->has_pos = host->finished_loading = true;
	guest->has_pos = guest->finished_loading = true;
	host->health = guest->health = MAX_PLAYER_HEALTH;
	host->hurt_resist_time = guest->hurt_resist_time = 0;
	host->last_damage = guest->last_damage = 0;
	server->creative_mode = false;
	clin_drain_discard();

	/* Hit guest first: damaging the host authors a host correction/ACK
	 * barrier, which correctly refuses the host's old-body queued input. */
	struct server_rpc hit = {.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack = {.entity_id = server->guest_entity_id}};
	assert(svin_rpc_send(&hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	bool host_hurt_guest = guest->health < MAX_PLAYER_HEALTH;

	hit.payload.entity_attack.entity_id = SERVER_LOCAL_PLAYER_ENTITY_ID;
	assert(svin_rpc_send_from(live, &hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	bool guest_hurt_host = host->health < MAX_PLAYER_HEALTH;
	printf("local peer PvP authority: guest_hurt_host=%d host_hurt_guest=%d\n",
		(int)guest_hurt_host, (int)host_hurt_guest);
	fflush(stdout);
	assert(guest_hurt_host && host_hurt_guest);

	/* The same accepted sources must fail when the bodies are far apart. */
	host->health = guest->health = MAX_PLAYER_HEALTH;
	host->hurt_resist_time = guest->hurt_resist_time = 0;
	guest->x = 15.5; guest->z = 15.5;
	hit.payload.entity_attack.entity_id = server->guest_entity_id;
	assert(svin_rpc_send(&hit));
	hit.payload.entity_attack.entity_id = SERVER_LOCAL_PLAYER_ENTITY_ID;
	assert(svin_rpc_send_from(live, &hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(host->health == MAX_PLAYER_HEALTH
		&& guest->health == MAX_PLAYER_HEALTH);

	/* Re-approach across a two-block eye-line wall. */
	guest->x = 9.5; guest->z = 8.5;
	assert(server_world_set_block(world, 8, 72, 8,
		(struct block_data) {.type = BLOCK_STONE}));
	assert(server_world_set_block(world, 8, 73, 8,
		(struct block_data) {.type = BLOCK_STONE}));
	assert(svin_rpc_send_from(live, &hit));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(host->health == MAX_PLAYER_HEALTH);

	assert(server_world_set_block(world, 8, 72, 8, wall_low));
	assert(server_world_set_block(world, 8, 73, 8, wall_high));
	memcpy(guest->inventory.items + INVENTORY_SLOT_ARMOR, guest_armor,
		sizeof(guest_armor));
	host->x = hx; host->y = hy; host->z = hz;
	guest->x = gx; guest->y = gy; guest->z = gz;
	host->health = hh; guest->health = gh;
	host->hurt_resist_time = hres; guest->hurt_resist_time = gres;
	host->last_damage = hlast; guest->last_damage = glast;
	server->creative_mode = creative;
	puts("PASS local peer PvP: both directions damage, far and wall rejected");
}

/* R2, maintainer 2026-10-08: admitted P2 can ride boats and saddled pigs.
 * Beta EntityBoat.interact / EntityPig.interact retain owner-only use and
 * Entity.mountEntity releases the previous ride before mounting the next. */
static unsigned s_mount_reply_count;

static void count_mount_reply(const struct client_rpc* call, void* user) {
	(void)call;
	(void)user;
	s_mount_reply_count++;
}

static void test_guest_mount_intent_admitted(struct server_local* server,
											 struct server_rpc_source live) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	assert(guest && server->guest_gameplay && server->guest_confirmed);
	double hx = host->x, hy = host->y, hz = host->z;
	double gx = guest->x, gy = guest->y, gz = guest->z;
	bool host_boat_before = host->riding_boat;
	bool host_cart_before = host->riding_minecart;
	host->x = guest->x = 8.5;
	host->y = guest->y = 72.0;
	host->z = guest->z = 8.5;
	host->has_pos = host->finished_loading = true;
	guest->has_pos = guest->finished_loading = true;

	struct entity* boat = server_local_spawn_boat(
		(vec3) {9.5F, 71.0F, 8.5F}, server);
	struct entity* pig = server_local_spawn_monster_at(
		(vec3) {9.5F, 71.0F, 8.5F}, MONSTER_PIG, server);
	assert(boat && boat->type == ENTITY_BOAT);
	assert(pig && pig->type == ENTITY_MONSTER
		   && pig->data.monster.id == MONSTER_PIG);
	pig->data.monster.saddled = true;
	clin_drain_discard();

	struct server_rpc use = {.type = SRPC_ENTITY_USE,
		.payload.entity_use = {.entity_id = boat->id,
			.desired_occupied = true}};
	s_mount_reply_count = 0;
	clin_test_set_rpc_observer(count_mount_reply, NULL);
	assert(svin_rpc_send_from(live, &use));
	server_local_test_process_messages(server);
	clin_test_set_rpc_observer(NULL, NULL);
	assert(boat->data.boat.occupied && boat->data.boat.occupant_id == server->guest_entity_id
		&& actor_ident_equal(boat->data.boat.rider_actor, server_local_actor_ident(server, guest)));
	assert(guest->riding_boat && !guest->riding_minecart);
	assert(host->riding_boat == host_boat_before
		   && host->riding_minecart == host_cart_before);
	assert(s_mount_reply_count == 1);
	clin_drain_discard();

	use.payload.entity_use.entity_id = pig->id;
	s_mount_reply_count = 0;
	clin_test_set_rpc_observer(count_mount_reply, NULL);
	assert(svin_rpc_send_from(live, &use));
	server_local_test_process_messages(server);
	clin_test_set_rpc_observer(NULL, NULL);
	assert(pig->data.monster.pig_ridden
		   && pig->data.monster.pig_rider_id == server->guest_entity_id
		   && actor_ident_equal(pig->data.monster.pig_rider_actor,
			server_local_actor_ident(server, guest)));
	assert(!boat->data.boat.occupied && !boat->data.boat.occupant_id);
	assert(!guest->riding_boat && !guest->riding_minecart);
	assert(host->riding_boat == host_boat_before
		   && host->riding_minecart == host_cart_before);
	assert(s_mount_reply_count == 1); /* previous boat's release correction */
	clin_drain_discard();

	use.payload.entity_use.entity_id = boat->id;
	s_mount_reply_count = 0;
	clin_test_set_rpc_observer(count_mount_reply, NULL);
	assert(svin_rpc_send(&use));
	server_local_test_process_messages(server);
	clin_test_set_rpc_observer(NULL, NULL);
	assert(boat->data.boat.occupied && host->riding_boat);
	assert(boat->data.boat.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID);
	assert(actor_ident_is_host(boat->data.boat.rider_actor));
	assert(!guest->riding_boat && !guest->riding_minecart);
	assert(s_mount_reply_count == 1);
	use.payload.entity_use.entity_id = pig->id;
	assert(svin_rpc_send_from(live, &use));
	server_local_test_process_messages(server);
	assert(!pig->data.monster.pig_ridden && !pig->data.monster.pig_rider_id);
	assert(host->riding_boat && boat->data.boat.occupied && !guest->riding_boat);
	puts("PASS R2 mount intent: guest boat admission, boat-to-pig release and pig "
		 "toggle-off preserve the host's independent boat");

	server_local_set_boat_occupancy_for(server, boat, host, false);
	clin_drain_discard();
	uint32_t ids[2] = {boat->id, pig->id};
	for(unsigned i = 0; i < 2; i++) {
		struct entity** ptr = dict_entity_get(server->entities, ids[i]);
		assert(ptr && *ptr);
		free(*ptr);
		dict_entity_erase(server->entities, ids[i]);
	}
	host->x = hx; host->y = hy; host->z = hz;
	guest->x = gx; guest->y = gy; guest->z = gz;
}

/* ---- Experimental local split-screen: gameplay admission ------------------
 * ATTACHED certifies WINDOW ownership. This exercises the SEPARATE body gate:
 * refused outright with the experimental switch off, refused for a wrong
 * source or unloaded terrain, granted once, idempotent, movable independently
 * of the host, and fully retired by Leave -- including the second residency
 * anchor and the client's two-view gate. */
static void test_guest_gameplay(struct server_local* server) {
	struct client_session* client = &gstate.players[1];
	assert(client_session_request_local_join(client));
	pump_handshake(server); pump_handshake(server); pump_handshake(server);
	assert(client->local_phase == CLIENT_GUEST_ATTACHED && server->guest_confirmed);
	assert(!client->local_player && !client->local_gameplay_ready);

	struct server_rpc_source gsrc
		= {.slot = 1, .generation = server->guest_generation};
	/* Switch OFF: no admission, no second anchor, no two-view gate -- and the
	 * guest's movement class does not even cross the QUEUE boundary, so an
	 * attached guest spends no inbox token and no dispatch on it. */
	assert(!server->split_enabled);
	struct server_rpc probe = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = server->player.x, .y = server->player.y,
			.z = server->player.z}};
	assert(!svin_rpc_send_from(gsrc, &probe));
	assert(!svin_pending_count());
	/* S1-a1: the seven gameplay verbs share that accept class, so with the
	 * switch OFF they are refused at the very same boundary. */
	struct server_rpc act_probe = {.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.finished = true, .x = 0, .y = 66, .z = 0}};
	assert(!svin_rpc_send_from(gsrc, &act_probe));
	act_probe.type = SRPC_HOTBAR_SLOT;
	act_probe.payload.hotbar_slot.slot = 1;
	assert(!svin_rpc_send_from(gsrc, &act_probe));
	assert(!svin_pending_count());
	/* The window class the guest legitimately owns is untouched by the gate. */
	struct server_rpc close_probe = {.type = SRPC_WINDOW_CLOSE,
		.payload.window_close = {.window = WINDOWC_INVENTORY}};
	assert(svin_rpc_send_from(gsrc, &close_probe));
	server_local_test_process_messages(server);
	assert(!server_local_guest_gameplay(server, gsrc));
	assert(!server->guest_gameplay && !server_local_two_anchor(server));
	assert(server_local_set_split_enabled(server, true) && server->residency);
	/* Switch ON: the same message is now admitted at the boundary. */
	assert(svin_rpc_send_from(gsrc, &probe));
	server_local_test_process_messages(server);
	/* ...and turning it back off closes the boundary again. */
	assert(server_local_set_split_enabled(server, false));
	assert(!svin_rpc_send_from(gsrc, &probe));
	assert(server_local_set_split_enabled(server, true) && server->residency);
	/* Wrong owner: the host source and a stale generation are both refused. */
	assert(!server_local_guest_gameplay(server, (struct server_rpc_source) {0}));
	assert(!server_local_guest_gameplay(server,
		(struct server_rpc_source) {.slot = 1, .generation = gsrc.generation + 1}));
	assert(!server->guest_gameplay);
	/* Terrain gate: an unloaded column is denied and the ATTACHED session
	 * survives the denial untouched. */
	struct server_player* body = server->guest_actor;
	body->x = server->player.x; body->y = server->player.y;
	body->z = server->player.z; body->rx = 0; body->ry = 0;
	double home_x = body->x, home_z = body->z;
	body->x = 100000.0;
	assert(!server_local_guest_gameplay(server, gsrc));
	assert(!server->guest_gameplay && server->guest_actor == body
		   && client->local_phase == CLIENT_GUEST_ATTACHED);
	body->x = home_x;
	/* A column the SERVER holds is still not enough: readiness is the host
	 * spawn path's published-column predicate, not two block reads. */
	if(!server_world_is_chunk_loaded(&server->worlds[0], 0, 0)) {
		struct server_chunk* loaded = NULL;
		assert(server_world_load_chunk(&server->worlds[0], 0, 0, &loaded));
	}
	struct server_chunk* home_chunk
		= dict_server_chunks_get(server->worlds[0].chunks, S_CHUNK_ID(0, 0));
	assert(home_chunk);
	struct block_data readable;
	assert(server_world_get_block(&server->worlds[0], (w_coord_t)home_x,
								  (w_coord_t)(body->y - 1.62),
								  (w_coord_t)home_z, &readable));
	assert(!home_chunk->sent);
	assert(!server_local_guest_gameplay(server, gsrc));
	assert(!server->guest_gameplay);
	home_chunk->sent = true;
	/* A negative health value is corrupt, unlike the persisted zero-health
	 * guest exercised by the saved-dead rejoin case below. */
	short zero_probe_health = body->health;
	body->health = -1;
	assert(!server_local_guest_gameplay(server, gsrc));
	assert(!server->guest_gameplay && !body->finished_loading
		   && client->local_phase == CLIENT_GUEST_ATTACHED);
	body->health = zero_probe_health;
	/* Granted -- and idempotent. */
	assert(server_local_guest_gameplay(server, gsrc));
	assert(server->guest_gameplay && body->finished_loading);
	assert(server->guest_entity_id
		   && server->guest_entity_id != SERVER_LOCAL_PLAYER_ENTITY_ID);
	assert(server_local_two_anchor(server));
	assert(server_local_guest_gameplay(server, gsrc));
	/* The switch cannot be pulled out from under an admitted body. */
	assert(!server_local_set_split_enabled(server, false));

	/* Independent movement: the guest route moves the GUEST actor and leaves
	 * the host's position, rotation and footstep odometer untouched. */
	double host_x = server->player.x, host_z = server->player.z;
	float host_rx = server->player.rx;
	float host_walk = server->player.distance_walked;
	/* One tick's worth of movement (Beta sprint is ~0.28 blocks/tick, so this
	 * is already generous); a teleport-scale sample is exercised below. */
	struct server_rpc move = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = home_x + 1.0, .y = body->y,
			.z = home_z + 0.5, .rx = 17.0F, .ry = -4.0F, .on_ground = true,
			.correction_seq = 0}};
	assert(svin_rpc_send_from(gsrc, &move));
	server_local_test_process_messages(server);
	assert(body->x == home_x + 1.0 && body->z == home_z + 0.5);
	assert(body->rx == 17.0F && body->ry == -4.0F && body->on_ground);
	assert(server->player.x == host_x && server->player.z == host_z
		   && server->player.rx == host_rx
		   && server->player.distance_walked == host_walk);
	/* A stale correction sequence is ignored rather than teleporting. */
	struct server_rpc stale = move;
	stale.payload.player_pos.x = home_x + 1.5;
	stale.payload.player_pos.correction_seq = body->correction_seq + 1;
	float rejected_fall = body->fall_distance;
	short rejected_health = body->health;
	assert(svin_rpc_send_from(gsrc, &stale));
	server_local_test_process_messages(server);
	assert(body->x == home_x + 1.0 && body->fall_distance == rejected_fall
		&& body->health == rejected_health);
	/* A non-finite sample is rejected outright. */
	struct server_rpc bad = move;
	bad.payload.player_pos.y = (double)INFINITY;
	assert(svin_rpc_send_from(gsrc, &bad));
	server_local_test_process_messages(server);
	assert(body->x == home_x + 1.0 && body->fall_distance == rejected_fall
		&& body->health == rejected_health);
	body->x = home_x; body->z = home_z;

	/* fall-0915 RED: two ordinary guest position packets traverse the real
	 * owner-1 FIFO. Beta accumulates the accepted 4-block airborne displacement
	 * and the following grounded packet bills ceil(4-3) == one heart (8 HP). */
	{
		struct item_data armor_before[INVENTORY_SIZE_ARMOR];
		memcpy(armor_before, body->inventory.items + INVENTORY_SLOT_ARMOR,
			sizeof armor_before);
		int remainder_before = body->damage_remainder;
		short health_before = body->health;
		memset(body->inventory.items + INVENTORY_SLOT_ARMOR, 0,
			sizeof armor_before);
		body->damage_remainder = 0;
		const w_coord_t fx = (w_coord_t)floor(body->x);
		const w_coord_t fz = (w_coord_t)floor(body->z);
		for(w_coord_t fy = 65; fy <= 73; fy++)
			(void)server_world_set_block(&server->worlds[0], fx, fy, fz,
				(struct block_data) {.type = BLOCK_AIR});
		body->y = 73.0; body->on_ground = true;
		body->fall_distance = 0.0F; body->health = MAX_PLAYER_HEALTH;
		body->hurt_resist_time = 0;
		struct server_rpc airborne = {.type = SRPC_PLAYER_POS,
			.payload.player_pos = {.x = body->x, .y = 69.0, .z = body->z,
				.rx = body->rx, .ry = body->ry, .on_ground = false,
				.correction_seq = body->correction_seq}};
		struct server_rpc landed = airborne;
		landed.payload.player_pos.on_ground = true;
		assert(svin_rpc_send_from(gsrc, &airborne));
		assert(svin_rpc_send_from(gsrc, &landed));
		server_local_test_process_messages(server);
		/* G8-BLAST-VELOCITY v4 (ASTRA-V3-REVIEW P1 damage ordering): the
		 * drain only ACCOUNTS the landing (fallDistance cleared); its
		 * handleFalling hit is the packet's replayed event. */
		assert(body->health == MAX_PLAYER_HEALTH && body->motion_q_n >= 1
			&& body->motion_q[body->motion_q_n - 1].fall_damage == 8);
		server_local_test_replay_actor_motion(server);
		assert(body->health == MAX_PLAYER_HEALTH - 8);
		assert(body->fall_distance == 0.0F && body->on_ground);

		/* If transport drops the airborne packet, two grounded endpoints four
		 * blocks apart reconstruct the same scalar and the same 8 HP landing. */
		body->health = MAX_PLAYER_HEALTH; body->hurt_resist_time = 0;
		body->last_damage = 0; body->damage_remainder = 0;
		body->y = 73.0; body->on_ground = true; body->fall_distance = 0.0F;
		struct server_rpc merged = landed;
		merged.payload.player_pos.y = 69.0;
		assert(svin_rpc_send_from(gsrc, &merged));
		server_local_test_process_messages(server);
		/* G8-BLAST-VELOCITY v4 (ASTRA-V3-REVIEW P1 damage ordering): the
		 * drain only ACCOUNTS the landing (fallDistance cleared); its
		 * handleFalling hit is the packet's replayed event. */
		assert(body->health == MAX_PLAYER_HEALTH && body->motion_q_n >= 1
			&& body->motion_q[body->motion_q_n - 1].fall_damage == 8);
		server_local_test_replay_actor_motion(server);
		assert(body->health == MAX_PLAYER_HEALTH - 8
			&& body->fall_distance == 0.0F && body->on_ground);

		/* Existing scalar is billed before the clipped grounded component. */
		body->health = MAX_PLAYER_HEALTH; body->hurt_resist_time = 0;
		body->last_damage = 0; body->damage_remainder = 0;
		body->fall_distance = 3.34627015F; body->on_ground = false;
		body->y = 69.0;
		struct server_rpc clipped = landed;
		clipped.payload.player_pos.y = 68.33627015;
		assert(svin_rpc_send_from(gsrc, &clipped));
		server_local_test_process_messages(server);
		/* G8-BLAST-VELOCITY v4 (ASTRA-V3-REVIEW P1 damage ordering): the
		 * drain only ACCOUNTS the landing (fallDistance cleared); its
		 * handleFalling hit is the packet's replayed event. */
		assert(body->health == MAX_PLAYER_HEALTH && body->motion_q_n >= 1
			&& body->motion_q[body->motion_q_n - 1].fall_damage == 8);
		server_local_test_replay_actor_motion(server);
		assert(body->health == MAX_PLAYER_HEALTH - 8
			&& body->fall_distance == 0.0F);

		/* Two accepted airborne samples accumulate 1.25+1.5=2.75 and land safe. */
		body->health = MAX_PLAYER_HEALTH; body->hurt_resist_time = 0;
		body->last_damage = 0; body->damage_remainder = 0;
		body->fall_distance = 0.0F; body->on_ground = false; body->y = 73.0;
		struct server_rpc part = airborne;
		part.payload.player_pos.y = 71.75;
		assert(svin_rpc_send_from(gsrc, &part));
		server_local_test_process_messages(server);
		part.payload.player_pos.y = 70.25;
		assert(svin_rpc_send_from(gsrc, &part));
		server_local_test_process_messages(server);
		assert(fabsf(body->fall_distance - 2.75F) < 1.0e-6F);
		part.payload.player_pos.on_ground = true;
		assert(svin_rpc_send_from(gsrc, &part));
		server_local_test_process_messages(server);
		assert(body->health == MAX_PLAYER_HEALTH && body->fall_distance == 0.0F);

		/* Same inputs through the actor-parametric sampler must produce bit-identical
		 * host/guest vertical mirrors and scalar. Keep lethal replacement out of this
		 * comparison: owner-specific death/respawn semantics are proved below. */
		{
			struct server_player* host = &server->player;
			double hxyz[3] = {host->x, host->y, host->z};
			bool hground = host->on_ground;
			short hhealth = host->health;
			float hfall = host->fall_distance, holdvy = host->old_vel_y;
			float hvy = host->vel_y;
			struct item_data host_armor[INVENTORY_SIZE_ARMOR];
			memcpy(host_armor, host->inventory.items + INVENTORY_SLOT_ARMOR,
				sizeof host_armor);
			memset(host->inventory.items + INVENTORY_SLOT_ARMOR, 0,
				sizeof host_armor);
			host->x = body->x; host->z = body->z;
			host->health = body->health = MAX_PLAYER_HEALTH;
			host->hurt_resist_time = body->hurt_resist_time = 0;
			host->last_damage = body->last_damage = 0;
			host->damage_remainder = body->damage_remainder = 0;

			/* Accumulating airborne samples, including vertical mirror state. */
			host->y = body->y = 71.75; host->on_ground = body->on_ground = false;
			host->fall_distance = body->fall_distance = 0.0F;
			host->vel_y = body->vel_y = 0.125F;
			assert(server_local_player_vertical_sample_for(server, host, 73.0,
				-0.25F, false));
			assert(server_local_player_vertical_sample_for(server, body, 73.0,
				-0.25F, false));
			assert(host->fall_distance == body->fall_distance
				&& host->old_vel_y == body->old_vel_y && host->vel_y == body->vel_y);
			host->y = body->y = 70.25;
			assert(server_local_player_vertical_sample_for(server, host, 71.75,
				-0.5F, false));
			assert(server_local_player_vertical_sample_for(server, body, 71.75,
				-0.5F, false));
			assert(host->fall_distance == 2.75F
				&& host->fall_distance == body->fall_distance
				&& host->old_vel_y == body->old_vel_y && host->vel_y == body->vel_y);

			/* The exact safe threshold is 3.0: no owner loses health. */
			host->fall_distance = body->fall_distance = 3.0F;
			host->on_ground = body->on_ground = false;
			assert(server_local_player_vertical_sample_for(server, host, host->y,
				0.0F, true));
			assert(server_local_player_vertical_sample_for(server, body, body->y,
				0.0F, true));
			assert(host->health == MAX_PLAYER_HEALTH && body->health == MAX_PLAYER_HEALTH
				&& host->fall_distance == 0.0F && body->fall_distance == 0.0F);

			/* The same nonlethal scalar 4 landing bills exactly 8 HP on each owner. */
			host->fall_distance = body->fall_distance = 4.0F;
			host->on_ground = body->on_ground = false;
			struct entity feedback_body = {.type = ENTITY_LOCAL_PLAYER,
				.health = MAX_PLAYER_HEALTH};
			struct entity* feedback_before = gstate.players[1].local_player;
			gstate.players[1].local_player = &feedback_body;
			assert(server_local_player_vertical_sample_for(server, host, host->y,
				0.0F, true));
			assert(server_local_player_vertical_sample_for(server, body, body->y,
				0.0F, true));
			clin_update_drain();
			assert(host->health == MAX_PLAYER_HEALTH - 8
				&& body->health == MAX_PLAYER_HEALTH - 8
				&& feedback_body.health == body->health);
			gstate.players[1].local_player = feedback_before;

			/* Identical water and ladder probes reset only the fall scalar. */
			struct block_data reset_feet;
			assert(server_world_get_block(&server->worlds[0], fx, 67, fz, &reset_feet));
			host->y = body->y = 69.0; host->on_ground = body->on_ground = false;
			(void)server_world_set_block(&server->worlds[0], fx, 67, fz,
				(struct block_data) {.type = BLOCK_WATER_STILL});
			host->fall_distance = body->fall_distance = 5.0F;
			assert(server_local_player_vertical_sample_for(server, host, 69.0, 0.0F, false));
			assert(server_local_player_vertical_sample_for(server, body, 69.0, 0.0F, false));
			assert(host->fall_distance == 0.0F && body->fall_distance == 0.0F);
			(void)server_world_set_block(&server->worlds[0], fx, 67, fz,
				(struct block_data) {.type = BLOCK_LADDER});
			host->fall_distance = body->fall_distance = 5.0F;
			assert(server_local_player_vertical_sample_for(server, host, 69.0, 0.0F, false));
			assert(server_local_player_vertical_sample_for(server, body, 69.0, 0.0F, false));
			assert(host->fall_distance == 0.0F && body->fall_distance == 0.0F);
			(void)server_world_set_block(&server->worlds[0], fx, 67, fz, reset_feet);

			host->x = hxyz[0]; host->y = hxyz[1]; host->z = hxyz[2];
			host->on_ground = hground; host->health = hhealth;
			host->fall_distance = hfall; host->old_vel_y = holdvy; host->vel_y = hvy;
			memcpy(host->inventory.items + INVENTORY_SLOT_ARMOR, host_armor,
				sizeof host_armor);
			body->health = MAX_PLAYER_HEALTH; body->hurt_resist_time = 0;
			body->last_damage = 0; body->damage_remainder = 0;
		}

		/* No position RPC: the actual server tick independently clears the healthy
		 * guest in water and on a ladder, even while owner 0 is dead/LOADING. */
		body->y = 69.0; body->on_ground = false; body->fall_distance = 5.0F;
		struct block_data wet_before, ladder_before;
		assert(server_world_get_block(&server->worlds[0], fx, 68, fz, &wet_before));
		assert(server_world_get_block(&server->worlds[0], fx, 67, fz, &ladder_before));
		(void)server_world_set_block(&server->worlds[0], fx, 68, fz,
			(struct block_data) {.type = BLOCK_WATER_STILL});
		clin_drain_discard();
		short host_health_before = server->player.health;
		float host_fall_before = server->player.fall_distance;
		enum host_respawn_phase host_phase_before = server->host_respawn_phase;
		bool host_ready_before = server->host_respawn_ready;
		short guest_health_before = body->health;
		body->fire = 0;
		body->oxygen = 17;
		int guest_fire_before = body->fire;
		server->player.health = 0;
		server->host_respawn_phase = HOST_RESPAWN_PHASE_LOADING;
		server->host_respawn_ready = false;
		server_local_test_update_once(server);
		clin_drain_discard();
		/* G6b F4: in water with fire 0, moveEntity's idle reset rests the
		 * guest at -fireResistance (Entity.java:489-490, EntityPlayer:50). */
		assert(guest_fire_before == 0);
		assert(body->fall_distance == 0.0F && body->health == guest_health_before
			&& body->fire == -PLAYER_FIRE_RESISTANCE
			&& body->oxygen == MAX_OXYGEN);
		assert(server->player.health == 0
			&& server->player.fall_distance == host_fall_before);
		(void)server_world_set_block(&server->worlds[0], fx, 68, fz, wet_before);
		(void)server_world_set_block(&server->worlds[0], fx, 67, fz,
			(struct block_data) {.type = BLOCK_LADDER});
		clin_drain_discard();
		body->fall_distance = 5.0F;
		server_local_test_update_once(server);
		clin_drain_discard();
		assert(body->fall_distance == 0.0F && body->health == guest_health_before);
		(void)server_world_set_block(&server->worlds[0], fx, 67, fz, ladder_before);
		server->player.health = host_health_before;
		server->host_respawn_phase = host_phase_before;
		server->host_respawn_ready = host_ready_before;
		memcpy(body->inventory.items + INVENTORY_SLOT_ARMOR, armor_before,
			sizeof armor_before);
		body->damage_remainder = remainder_before;
		body->health = health_before;
		body->hurt_resist_time = 0;
		clin_drain_discard();
	}

	/* End to end through the REAL client path: the request rides the guest
	 * source, the reply installs a body in the shared entity store and only
	 * then sets local_gameplay_ready -- the two-view loop's only gate. */
	server->guest_gameplay = false;
	server->guest_entity_id = 0;
	body->finished_loading = false;
	short pre_admit_health = body->health;
	body->health = MAX_PLAYER_HEALTH / 2;
	assert(client_session_request_local_gameplay(client));
	pump_handshake(server);
	assert(server->guest_gameplay && client->local_gameplay_ready);
	assert(client->local_player
		   && client->local_player->id == server->guest_entity_id
		   && client->local_player->health == MAX_PLAYER_HEALTH / 2);
	body->health = pre_admit_health;
	client->local_player->health = pre_admit_health;
	assert(client->local_player->tick_client == NULL); /* never the primary tick */
	assert(gstate.players[0].local_player != client->local_player);
	assert(client->camera.x == (float)body->x
		   && client->camera.z == (float)body->z);
	assert(!client_session_request_local_gameplay(client)); /* already granted */
	/* A full slot refuses a second guest outright. */
	struct server_rpc_source spare;
	assert(!server_local_guest_create(server, &spare));

	/* Per-sample move clamp, against the LIVE client half. An in-bound sample
	 * is applied and leaves the movement token alone; a teleport-scale one is
	 * refused, the body stays put, the token ADVANCES, and the snap-back reply
	 * puts the guest camera and echoed token back in agreement -- which is
	 * what makes body_sync correction real rather than inert. */
	struct server_rpc_source live
		= {.slot = 1, .generation = server->guest_generation};
	uint32_t seq_before = body->correction_seq;
	assert(client->player_sync.correction_seq == seq_before);
	double kept_x = body->x, kept_z = body->z;
	struct server_rpc step = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = kept_x + 0.25, .y = body->y,
			.z = kept_z + 0.25, .rx = body->rx, .ry = body->ry,
			.on_ground = true, .correction_seq = seq_before}};
	assert(svin_rpc_send_from(live, &step));
	server_local_test_process_messages(server);
	clin_update_drain();
	assert(body->x == kept_x + 0.25 && body->z == kept_z + 0.25);
	assert(body->correction_seq == seq_before);
	kept_x = body->x; kept_z = body->z;
	struct server_rpc jump = step;
	jump.payload.player_pos.x = kept_x + 3.0;
	jump.payload.player_pos.z = kept_z + 2.0;
	assert(svin_rpc_send_from(live, &jump));
	server_local_test_process_messages(server);
	assert(body->x == kept_x && body->z == kept_z);
	assert(body->correction_seq == seq_before + 1);
	clin_update_drain();
	assert(client->player_sync.correction_seq == body->correction_seq);
	assert(client->camera.x == (float)kept_x
		   && client->camera.z == (float)kept_z);
	assert(client->local_gameplay_ready && client->local_player);
	/* The stale token the guest was still echoing is now refused. */
	assert(svin_rpc_send_from(live, &jump));
	server_local_test_process_messages(server);
	assert(body->x == kept_x && body->correction_seq == seq_before + 1);
	/* ...and the corrected token is accepted again. */
	struct server_rpc resumed = step;
	resumed.payload.player_pos.x = kept_x + 0.5;
	resumed.payload.player_pos.z = kept_z;
	resumed.payload.player_pos.correction_seq = body->correction_seq;
	assert(svin_rpc_send_from(live, &resumed));
	server_local_test_process_messages(server);
	assert(body->x == kept_x + 0.5);

	test_guest_player_state(server, live);
	test_guest_mount_intent_admitted(server, live);
	test_guest_action_verbs(server, live);
	test_local_peer_pvp(server, live);
	test_guest_aggro_identity(server, live);
	/* S1-a3 -- defined at the END of this file (the damage/heal pipeline and
	 * the zero-health fail-closed state). */
	test_guest_damage_pipeline(server, live);
	test_guest_per_tick_damage_steps(server, live);

	/* S2 golden: an admitted same-Overworld guest makes host respawn a local
	 * readiness handoff.  The retained guest/world must not enter the legacy
	 * global death switch, arm its input ACK/eviction barriers, or revive the
	 * host before that local handoff is ready.  Drive the ordinary source-0
	 * mailbox route so this cannot pass on a fabricated admission flag. */
	{
		struct server_player* retained_guest = server->guest_actor;
		struct entity* retained_client_body = client->local_player;
		uint64_t retained_generation = server->guest_generation;
		assert(server->worlds[0].dimension == WORLD_DIM_OVERWORLD
			&& server->split_enabled && server->guest_gameplay
			&& retained_guest == body && retained_guest->finished_loading
			&& retained_generation == live.generation
			&& client->local_phase == CLIENT_GUEST_ATTACHED
			&& client->local_gameplay_ready && retained_client_body);
		assert(server->world_switch_phase == SERVER_WORLD_SWITCH_IDLE
			&& server->input_ack_pending == 0 && !server->death_world_evict
			&& !server->paused);
		server->player.hurt_resist_time = 0;
		bool host_hit_accepted = false;
		assert(!server_local_damage_actor_result(server, &server->player,
			MAX_PLAYER_HEALTH, 0.0f, 0.0f, &host_hit_accepted));
		assert(host_hit_accepted && server->player.health == 0
			&& server->player.has_death_pos);
		assert(server->world_switch_phase == SERVER_WORLD_SWITCH_IDLE
			&& server->input_ack_pending == 0 && !server->death_world_evict
			&& !server->paused);
		struct server_rpc host_respawn = {.type = SRPC_RESPAWN};
		struct host_respawn_reply_probe host_replies = {0};
		clin_test_set_rpc_observer(capture_host_respawn_reply, &host_replies);
		uint64_t host_counter_before = server->host_respawn_epoch_counter;
		server_local_test_fail_next_host_respawn_plan();
		assert(svin_rpc_send(&host_respawn));
		server_local_test_process_messages(server);
		clin_update_drain();
		assert(host_replies.retry0 == 1
			&& server->host_respawn_phase == HOST_RESPAWN_PHASE_IDLE
			&& server->host_respawn_epoch_counter == host_counter_before
			&& server->player.health == 0 && server->player.has_death_pos
			&& server->world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
		server->host_respawn_epoch_counter = UINT64_MAX;
		assert(svin_rpc_send(&host_respawn));
		server_local_test_process_messages(server);
		clin_update_drain();
		assert(host_replies.terminal0 == 1
			&& server->host_respawn_phase == HOST_RESPAWN_PHASE_IDLE
			&& server->player.health == 0
			&& server->world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
		server->host_respawn_epoch_counter = host_counter_before;
		/* The player-inventory close drops owned 2x2 crafting inputs. Inject its
		 * first allocation failure: PREPARED and the death latch must survive,
		 * and the next epoch-0 press continues the same plan exactly once. */
		server->player.inventory.items[INVENTORY_SLOT_CRAFTING]
			= (struct item_data) {.id = ITEM_COAL, .count = 1};
		server_local_test_fail_next_entity_alloc();
		assert(svin_rpc_send(&host_respawn));
		server_local_test_process_messages(server);
		clin_update_drain();
		assert(host_replies.retry_epoch == 1
			&& server->host_respawn_phase == HOST_RESPAWN_PHASE_PREPARED
			&& server->player.has_death_pos
			&& server->player.inventory.items[INVENTORY_SLOT_CRAFTING].id
				== ITEM_COAL);
		uint64_t prepared_host_epoch = server->host_respawn_epoch;
		assert(svin_rpc_send(&host_respawn));
		server_local_test_process_messages(server);
		clin_update_drain();
		clin_test_set_rpc_observer(NULL, NULL);
		assert(server->host_respawn_epoch == prepared_host_epoch
			&& host_replies.loading == 1);
		assert(server->world_switch_phase == SERVER_WORLD_SWITCH_IDLE
			&& "split host respawn must not begin the global death switch");
		assert(server->input_ack_pending == 0 && !server->death_world_evict
			&& !server->paused);
		assert(server->player.health == 0
			&& "the host must remain dead until local readiness");
		assert(server->guest_actor == retained_guest
			&& server->guest_generation == retained_generation
			&& server->guest_gameplay && retained_guest->finished_loading
			&& client->local_phase == CLIENT_GUEST_ATTACHED
			&& client->local_gameplay_ready
			&& client->local_player == retained_client_body);
		assert(server->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING
			&& server->host_respawn_epoch != 0
			&& server->host_respawn_anchor && !server->host_respawn_ready);
		uint64_t host_epoch = server->host_respawn_epoch;
		/* A wrong nonzero value is an ACK attempt, never a loading resend and
		 * never a route into the legacy global-switch transaction. */
		host_respawn.payload.respawn.respawn_epoch = host_epoch + 1;
		assert(svin_rpc_send(&host_respawn));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server->player.health == 0
			&& server->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING
			&& !server->host_respawn_ready
			&& server->world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
		/* An exact ACK latches before the planned center is published, but cannot
		 * revive the host or emit completion early. */
		w_coord_t target_cx = WCOORD_CHUNK_OFFSET(
			(w_coord_t)floor(server->host_respawn_x));
		w_coord_t target_cz = WCOORD_CHUNK_OFFSET(
			(w_coord_t)floor(server->host_respawn_z));
		struct server_chunk* target = dict_server_chunks_get(
			server->worlds[0].chunks, S_CHUNK_ID(target_cx, target_cz));
		assert(target);
		target->sent = false;
		host_respawn.payload.respawn.respawn_epoch = host_epoch;
		uint32_t corpse_seq = server->player.correction_seq;
		assert(svin_rpc_send(&host_respawn));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING
			&& server->host_respawn_ready && server->player.health == 0
			&& server->player.correction_seq == corpse_seq
			&& server->world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
		/* Publishing the center makes the duplicate exact ACK complete through
		 * the real source-0 path. The guest and world identity survive. */
		target->sent = true;
		clin_test_set_rpc_observer(capture_host_respawn_reply, &host_replies);
		assert(svin_rpc_send(&host_respawn));
		/* These old-body samples are FIFO-behind the ACK. The commit advances
		 * correction_seq before they drain, so clearing the active barrier cannot
		 * expose either one to the fresh body. */
		struct server_rpc stale_pos = {.type = SRPC_PLAYER_POS,
			.payload.player_pos = {.x = server->host_respawn_x + 0.25,
				.y = server->host_respawn_y, .z = server->host_respawn_z,
				.correction_seq = corpse_seq}};
		struct server_rpc stale_state = {.type = SRPC_PLAYER_STATE,
			.payload.player_state = {.on_ground = true, .sneaking = true,
				.correction_seq = corpse_seq}};
		assert(svin_rpc_send(&stale_pos));
		assert(svin_rpc_send(&stale_state));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server->player.health == MAX_PLAYER_HEALTH
			&& server->host_respawn_phase == HOST_RESPAWN_PHASE_IDLE
			&& !server->host_respawn_anchor && !server->host_respawn_ready
			&& server->host_respawn_epoch_counter == host_epoch
			&& server->player.correction_seq != corpse_seq
			&& server->player.x == server->host_respawn_x
			&& !server->player.on_ground && !server->player.sneaking
			&& server->world_switch_phase == SERVER_WORLD_SWITCH_IDLE
			&& server->input_ack_pending == 0 && !server->death_world_evict);
		assert(host_replies.correction_order > 0
			&& host_replies.ready_order > host_replies.correction_order);
		clin_test_set_rpc_observer(NULL, NULL);
		assert(server->guest_actor == retained_guest
			&& server->guest_generation == retained_generation
			&& server->guest_gameplay && retained_guest->finished_loading);
		server->player.reject_pos_this_update = false;
	}

	/* Anchor-1 residency debt must not outlive the body that owed it. Accrue
	 * some against columns no one has published, then Leave. */
	server->residency_anchors[0] = (struct residency_anchor) {0, 0};
	server->residency_anchors[1] = (struct residency_anchor) {64, 64};
	server_local_residency_refresh(server);
	assert(server->anchor_near_owed[1] > 0);
	unsigned owed_slots = 0;
	for(unsigned b = 0; b < RESIDENCY_BUCKETS; b++)
		for(unsigned i = 0; i < 2; i++)
			if(server->residency->debt[b][i].used
			   && (server->residency->debt[b][i].near_owners & 2u))
				owed_slots++;
	assert(owed_slots > 0);

/* Leave retires EVERYTHING: body, entity, anchor and gate. */
	assert(client_session_request_local_leave(client));
	pump_handshake(server);
	assert(!server->guest_actor && !server->guest_gameplay
		   && !server->guest_entity_id && !server_local_two_anchor(server));
	assert(!client->local_gameplay_ready && !client->local_player);
	assert(client->local_phase == CLIENT_GUEST_IDLE);
	assert(dict_entity_size(gstate.entities) == 0);
	/* ...including the departed body residency debt: the refresh pass stops
	 * running the instant two-anchor mode ends, so anything left here would be
	 * inherited by the next admitted guest. */
	assert(server->anchor_near_owed[1] == 0 && server->anchor_oldest[1] == 0);
	for(unsigned b = 0; b < RESIDENCY_BUCKETS; b++)
		for(unsigned i = 0; i < 2; i++)
			assert(!(server->residency->debt[b][i].used
					 && ((server->residency->debt[b][i].near_owners
						  | server->residency->debt[b][i].far_owners)
						 & 2u)));

	/* Re-join after a leave is a fresh generation with no stale admission --
	 * and no inherited anchor-1 debt. */
	assert(client_session_request_local_join(client));
	pump_handshake(server); pump_handshake(server); pump_handshake(server);
	assert(client->local_phase == CLIENT_GUEST_ATTACHED
		   && !client->local_gameplay_ready && !server->guest_gameplay);
	assert(server->anchor_near_owed[1] == 0);
	assert(client_session_request_local_leave(client));
	pump_handshake(server);
	assert(!server->guest_actor && client->local_phase == CLIENT_GUEST_IDLE);

	/* A2-T (Astra a2t-0927): the Overworld-only Join rule is REPLACED by the
	 * saved-dimension admission -- with the host in the Nether, this
	 * Overworld-saved guest joins a LIVE Overworld admitted beside it. */
	/* G9: settled fixture setup. Relabelling an Overworld-lit world as the
	 * Nether is a fixture hack; light still pending from the earlier
	 * Overworld guest edits must settle as Overworld first (a no-sky drain
	 * over stored skylight 15 would relight the whole loaded area -- a
	 * runaway the lossless drain now stops instead of silently capping). */
	assert(server_world_light_drain(&server->worlds[0]));
	server->worlds[0].dimension = WORLD_DIM_NETHER;
	assert(client_session_request_local_join(client));
	/* +2 pumps: the Join into another world round-trips the client capacity */
	pump_handshake(server); pump_handshake(server); pump_handshake(server);
	pump_handshake(server); pump_handshake(server);
	assert(client->local_phase == CLIENT_GUEST_ATTACHED && server->guest_actor);
	struct server_world* beside = server_local_world_of(server, server->guest_actor);
	assert(beside && beside != &server->worlds[0]
		   && beside->dimension == WORLD_DIM_OVERWORLD && server_local_world_live(beside));
	assert(client_session_request_local_leave(client));
	pump_handshake(server);
	assert(!server->guest_actor && client->local_phase == CLIENT_GUEST_IDLE);
	assert(server_local_world_retire(server, beside));
	server->worlds[0].dimension = WORLD_DIM_OVERWORLD;
	/* ...and the very same request succeeds once the host is back overworld. */
	assert(client_session_request_local_join(client));
	pump_handshake(server); pump_handshake(server); pump_handshake(server);
	assert(client->local_phase == CLIENT_GUEST_ATTACHED && server->guest_actor);
	assert(client_session_request_local_leave(client));
	pump_handshake(server);
	assert(!server->guest_actor && client->local_phase == CLIENT_GUEST_IDLE);

	assert(server_local_set_split_enabled(server, false) && !server->residency);
	/* Disarming the switch closes the transport accept class with it. */
	assert(!svin_rpc_send_from(live, &step));
	puts("PASS guest gameplay admission: queue-boundary accept gate, "
		 "saved-dimension Join, published-column readiness, clamped+corrected "
		 "movement, anchor-1 debt retired on release");
}

/* ===== A0 (split-dimensions-plan-0924): portal travel with the partner in ====
 * One world context serves both players until dimensions land. The host's
 * portal dwell used to reach server_local_portal_travel, whose discard refused
 * (guest attached) without saying so, and whose destination load then asserted
 * over the still-live world. The interim rule refuses the travel BEFORE any
 * checkpoint/discard: both players stay in the source world, the dwell resets
 * (re-armed only after leaving the portal) and ONE HUD notice names why. */
#define A0_PORTAL_NOTICE "Portal: Player 2 must leave first"
struct a0_portal_capture { unsigned notice, other_hud, reset, begin_switch, load_failed; };
static void a0_capture_portal_rpc(const struct client_rpc* call, void* user) {
	struct a0_portal_capture* c = user;
	if(call->type == CRPC_HUD_MESSAGE) {
		if(!strcmp(call->payload.hud_message, A0_PORTAL_NOTICE)) c->notice++;
		else c->other_hud++;
	} else if(call->type == CRPC_WORLD_RESET) c->reset++;
	else if(call->type == CRPC_BEGIN_WORLD_SWITCH) c->begin_switch++;
	else if(call->type == CRPC_LOAD_FAILED) c->load_failed++;
}
static void a0_assert_both_live(struct server_local* server,
	struct server_player* guest, const struct a0_portal_capture* cap) {
	assert(server->player.has_pos && server->level.data);
	assert(server->player.dimension == WORLD_DIM_OVERWORLD
		&& server->worlds[0].dimension == WORLD_DIM_OVERWORLD);
	assert(server->guest_actor == guest && server->guest_owned && server->guest_confirmed);
	assert(guest->dimension == WORLD_DIM_OVERWORLD);
	assert(gstate.players[1].local_bound);
	assert(!server->portal_job && !server->portal_origin.valid);
	assert(server->world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
	assert(!server->player.portal_travel_requested);
	assert(cap->reset == 0 && cap->begin_switch == 0 && cap->load_failed == 0);
	assert(server_local_world_of(server, &server->player) == &server->worlds[0]);
	assert(server_local_world_of(server, guest) == &server->worlds[0]);
}
static void a0_tick(struct server_local* server) {
	server_local_test_update_once(server);
	clin_drain_discard();
}
static void test_guest_blocks_host_portal(struct server_local* server) {
	struct server_player* guest = server->guest_actor;
	assert(guest && server->guest_owned && server->guest_confirmed);
	struct server_player* host = &server->player;
	assert(host->has_pos && host->finished_loading && host->health > 0);
	assert(host->dimension == WORLD_DIM_OVERWORLD);
	const w_coord_t fx = (w_coord_t)floor(host->x);
	const w_coord_t fy = (w_coord_t)floor(host->y - PLAYER_EYE_HEIGHT + 0.001);
	const w_coord_t fz = (w_coord_t)floor(host->z);
	struct block_data before;
	assert(server_world_get_block(&server->worlds[0], fx, fy, fz, &before));
	const struct block_data portal = {.type = BLOCK_PORTAL};
	struct a0_portal_capture cap = {0};
	clin_drain_discard();
	clin_test_set_rpc_observer(a0_capture_portal_rpc, &cap);

	/* 1. The REAL dwell: portal contact scan -> 0.0125F ramp -> latch -> the
	 *    tick-top trigger -> server_local_portal_travel. Beta's setInPortal only
	 *    arms the ramp once the post-load cooldown has drained off-portal
	 *    (contact during cooldown pins it at 10), so walk in only after that. */
	for(int i = 0; i < 40 && host->time_until_portal > 0; i++) a0_tick(server);
	assert(host->time_until_portal == 0 && host->time_in_portal == 0.0F);
	(void)server_world_set_block(&server->worlds[0], fx, fy, fz, portal);
	struct block_data probe;
	assert(server_world_get_block(&server->worlds[0], fx, fy, fz, &probe)
		&& probe.type == BLOCK_PORTAL);
	int ticks = 0;
	while(cap.notice == 0 && ticks < 200) { a0_tick(server); ticks++; }
	assert(cap.notice == 1);
	assert(ticks >= 82); /* the full Beta dwell ran; nothing short-circuited it */
	a0_assert_both_live(server, guest, &cap);
	assert(host->time_in_portal == 0.0F && !host->in_portal);
	/* 2. Standing in the portal does not re-trigger: the cooldown stays pinned
	 *    by contact, so the ramp never re-arms and no second notice is sent. */
	for(int i = 0; i < 150; i++) a0_tick(server);
	assert(cap.notice == 1);
	assert(host->time_in_portal == 0.0F && host->time_until_portal > 0);
	a0_assert_both_live(server, guest, &cap);
	/* 3. Leaving re-arms it: after the cooldown drains off-portal, a fresh
	 *    entry dwells again and is refused again with exactly one notice. */
	(void)server_world_set_block(&server->worlds[0], fx, fy, fz, before);
	for(int i = 0; i < 15; i++) a0_tick(server);
	assert(host->time_until_portal == 0 && cap.notice == 1);
	(void)server_world_set_block(&server->worlds[0], fx, fy, fz, portal);
	ticks = 0;
	while(cap.notice == 1 && ticks < 200) { a0_tick(server); ticks++; }
	assert(cap.notice == 2 && ticks >= 82);
	a0_assert_both_live(server, guest, &cap);
	(void)server_world_set_block(&server->worlds[0], fx, fy, fz, before);
	clin_drain_discard();

	/* 4. Armed load failure is never reached (refusal precedes checkpoint/discard/load): arm the archive-open failure, then drive the
	 *    real travel handler directly. The refusal lands before any checkpoint,
	 *    discard or load, so the source world and BOTH players survive. */
	server_local_test_fail_world_loads(1);
	host->time_in_portal = 1.0F;
	host->time_until_portal = 10;
	server_local_portal_travel(server);
	assert(cap.notice == 3);
	a0_assert_both_live(server, guest, &cap);
	assert(host->time_in_portal == 0.0F);
	server_local_test_fail_world_loads(0);
	for(int i = 0; i < 20; i++) a0_tick(server);
	a0_assert_both_live(server, guest, &cap);
	assert(cap.other_hud == 0);
	clin_test_set_rpc_observer(NULL, NULL);
	clin_drain_discard();
	puts("PASS A0 guest portal: dwell refused before teardown, one notice, no re-trigger in the portal, re-arm after leaving, armed load failure never reached, both players kept");
}

static void test_guest_handshake(struct server_local* server) {
	assert(svin_init());
	struct client_session* client=&gstate.players[1];
	assert(client_session_request_local_join(client));
	uint64_t ticket=client->local_ticket;
	pump_handshake(server);
	assert(client->local_phase==CLIENT_GUEST_INSTALLING && server->guest_actor);
	assert(!server->guest_confirmed && !server->guest_actor->finished_loading);
	nbt_node* saved_before=nbt_find_by_path(server->level.data,".Data.CaveWiiGuest");
	assert(server_local_guest_checkpoint(server));
	assert(nbt_find_by_path(server->level.data,".Data.CaveWiiGuest")==saved_before);
	struct server_rpc_source source={.slot=1,.generation=server->guest_generation};
	struct server_rpc click={.type=SRPC_WINDOW_CLICK,
		.payload.window_click={.window=0,.slot=INVENTORY_SLOT_MAIN,.action_id=1,
			.expected=server->guest_actor->inventory.items[INVENTORY_SLOT_MAIN]}};
	assert(svin_rpc_send_from(source,&click));
	server_local_test_process_messages(server);
	assert(!server->guest_actor->inventory.picked_item.id); /* pre-confirm rejected */
	pump_handshake(server);
	assert(client->local_phase==CLIENT_GUEST_CONFIRMING && !server->guest_confirmed);
	pump_handshake(server);
	assert(client->local_phase==CLIENT_GUEST_ATTACHED && server->guest_confirmed);
	assert(!client->local_player && !server->guest_actor->finished_loading);
 test_guest_sign_set_routing(server,source);
	assert(!client_session_request_local_join(client));
	/* Stale control replies must not cancel the current owner. */
	client_session_local_status(client,&(struct client_rpc){.type=CRPC_LOCAL_GUEST,
		.payload.local_guest={.ticket=ticket+1,.generation=source.generation,.result=LOCAL_GUEST_LEFT}});
	assert(client->local_phase==CLIENT_GUEST_ATTACHED);
	server->guest_actor->inventory.picked_item=(struct item_data){.id=ITEM_DIAMOND,.count=4};
	server->guest_actor->inventory.items[INVENTORY_SLOT_CRAFTING]=(struct item_data){.id=BLOCK_LOG,.count=2};
	string_t path;string_init_set(path,server->level.file_name);
	string_cat_str(server->level.file_name,"/missing");
	assert(client_session_request_local_leave(client));pump_handshake(server);
	assert(client->local_phase==CLIENT_GUEST_FAILED && client->local_bound
		&& !client->local_pending);
	assert(server->guest_actor && server->guest_actor->inventory.picked_item.count==4);
	string_set(server->level.file_name,path);string_clear(path);
	assert(client_session_request_local_leave(client));
	pump_handshake(server);
	assert(client->local_phase==CLIENT_GUEST_IDLE && !client->local_bound && !server->guest_actor);
	/* Leave with a full/private cursor saves it without a Drop. */
	assert(client_session_request_local_join(client));
	pump_handshake(server);pump_handshake(server);pump_handshake(server);
	assert(client->local_phase==CLIENT_GUEST_ATTACHED);
	assert(server->guest_actor->inventory.picked_item.count==4);
	assert(server->guest_actor->inventory.items[INVENTORY_SLOT_CRAFTING].count==2);
	assert(windowc_get_latest(client->windows[0])->picked_item.count==4);
	assert(client_session_request_local_leave(client));pump_handshake(server);
	assert(!server->guest_actor);
	/* Cancel before receiving the offer. The offered actor is not yet owned
	 * by gameplay and can be retired without changing the archived inventory. */
	assert(client_session_request_local_join(client));client_session_flush_local(client);
	server_local_test_process_messages(server);
	assert(client_session_request_local_leave(client));
	clin_update_drain();pump_handshake(server);
	assert(client->local_phase==CLIENT_GUEST_IDLE && !server->guest_actor);
	/* Client allocation failure cancels the offered generation; no empty
	 * player is silently substituted and the next join may retry. */
	assert(client_session_request_local_join(client));client_session_flush_local(client);
	server_local_test_process_messages(server);
	windowc_set_alloc_hook(fail_alloc);clin_update_drain();windowc_set_alloc_hook(NULL);
	assert(client->local_phase==CLIENT_GUEST_LEAVING && !client->local_bound);
	pump_handshake(server);assert(!server->guest_actor);
	/* Retry survives exhaustion of the real client-to-server token/pending
	 * queues. Discard irrelevant pressure traffic, then deliver the Join. */
	struct server_rpc pressure={.type=SRPC_USE_ITEM};
	for(int k=0;k<80;k++) assert(svin_rpc_send(&pressure));
	assert(client_session_request_local_join(client));client_session_flush_local(client);
	assert(client->local_pending);
	while(svin_pending_count()) {
		svin_process_owned_messages(discard_request,NULL,false);
		svin_rpc_flush_pending();
	}
	svin_process_owned_messages(discard_request,NULL,false);
	pump_handshake(server);pump_handshake(server);pump_handshake(server);
	assert(client->local_phase==CLIENT_GUEST_ATTACHED);
	assert(client_session_request_local_leave(client));pump_handshake(server);
	assert(client->local_phase==CLIENT_GUEST_IDLE && !server->guest_actor);
	test_guest_gameplay(server);
	/* Ordinary world quit shares one checkpoint for BOTH players. A failure
	 * must leave both live; retry may release the guest before its world. */
	assert(client_session_request_local_join(client));
	pump_handshake(server);pump_handshake(server);pump_handshake(server);
	assert(server->guest_actor && server->guest_confirmed);
	/* card 3 (2026-09-16): the release guard answers a USER Save and quit while a
	 * guest body exists that the server does not own -- the HUD reason plus
	 * RETURN_INGAME so the host's pending exit settles; nothing is released and
	 * ownership is untouched. An internal release (no client reset) stays silent. */
	{
		struct server_player* guard_guest = server->guest_actor;
		bool owned_before = server->guest_owned;
		assert(owned_before && guard_guest);
		server->guest_owned = false;
		s_guard_return = s_guard_reset = s_guard_hud_named = s_guard_hud_other = 0;
		clin_test_set_rpc_observer(count_guard_reply, NULL);
		server_local_unload_world_impl(server, true);  /* user save: persist + client reset */
		assert(s_guard_return == 1 && s_guard_hud_named == 1 && s_guard_reset == 0);
		assert(server->guest_actor == guard_guest && !server->guest_owned
			   && server->player.has_pos && server->level.data && client->local_bound);
		server_local_unload_world_impl(server, false); /* internal: no client reset -> silent */
		assert(s_guard_return == 1 && s_guard_hud_named == 1 && s_guard_hud_other == 0 && s_guard_reset == 0);
		assert(server->guest_actor == guard_guest && server->player.has_pos && server->level.data);
		clin_test_set_rpc_observer(NULL, NULL);
		server->guest_owned = owned_before;
		clin_drain_discard();
		puts("PASS guest guard: a user Save and quit with an unowned guest body is named and returned; an internal release stays silent; nothing released");
	}
	test_guest_blocks_host_portal(server);
	string_init_set(path,server->level.file_name);
	string_cat_str(server->level.file_name,"/missing");
	server_local_unload_world_impl(server,true);
	clin_drain_discard();
	assert(server->guest_actor && server->player.has_pos && server->level.data);
	assert(client->local_bound);
	string_set(server->level.file_name,path);string_clear(path);
	server_local_unload_world_impl(server,true);
	clin_drain_discard(); /* backend fixture does not drive global renderer teardown */
	client_session_clear_windows(client);
	assert(!server->guest_actor && !server->player.has_pos && !server->level.data);
	puts("PASS guest world quit: failed checkpoint retains both players; successful checkpoint releases guest before world");
	puts("PASS guest handshake: install-before-confirm, stale replies, pre-admission input, cancellation, OOM, save retry and queue pressure");
}

static void test_guest_lifetime(void) {
	char path[256];
	test_fixture_dir("test_local_guest",path,sizeof(path));
	assert(MAKE_DIR(path)==0);
	static struct server_local server;
	memset(&server.guest_join_cap, 0xa5, sizeof server.guest_join_cap);
	memset(&server.guest_respawn_cap, 0xa5, sizeof server.guest_respawn_cap);
	memset(&server.guest_respawn_target, 0xa5, sizeof server.guest_respawn_target);
	server.guest_respawn_phase = GUEST_RESPAWN_LOADING;
	server.guest_respawn_epoch_counter = UINT64_MAX;
	server.guest_respawn_epoch = UINT64_MAX;
	server.guest_respawn_generation = UINT64_MAX;
	server.guest_respawn_anchor = server.guest_respawn_ready = true;
	server_local_init(&server,false,false,false,false,false,false,false);
	assert(server.guest_join_cap.counter == 0
		&& server.guest_join_cap.pending == false);
	assert(server.guest_respawn_cap.counter == 0
		&& !server.guest_respawn_cap.pending
		&& !server.guest_respawn_cap.cleanup_cancel
		&& server.guest_respawn_cap.cleanup_ticket == 0
		&& server.guest_respawn_cap.timed_out_id == 0);
	assert(server.guest_respawn_phase == GUEST_RESPAWN_IDLE
		&& server.guest_respawn_epoch_counter == 0
		&& server.guest_respawn_epoch == 0
		&& server.guest_respawn_generation == 0
		&& server.guest_respawn_target.generation == 0
		&& !server.guest_respawn_anchor && !server.guest_respawn_ready);
	string_set_str(server.level_name,path);
	assert(level_archive_create_new(&server.level,server.level_name,12345,(vec3){4,72,4}));
	assert(level_archive_save(&server.level));
	/* This save/reopen fixture deliberately owns the Overworld slot. */
	server_world_create(&server.worlds[0],server.level_name,WORLD_DIM_OVERWORLD,12345,true);
	server.worlds[0].local = &server; /* production load binds archive/session owner */
	server.worlds[0].session = &server; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&server))
		abort();
	assert(server.player.inventory.items);
	server.player.active_inventory=&server.player.inventory;
	server.player.has_pos=server.player.finished_loading=true;
	server.player.x=4;server.player.y=72;server.player.z=4;
	server.player.health=160;
	server.player.dimension=WORLD_DIM_OVERWORLD;
	server.player.inventory.items[INVENTORY_SLOT_MAIN]=(struct item_data){.id=BLOCK_DIRT,.count=9};
	/* A successful provider-only unbind retires only the active tuple. */
	struct server_player probe = {0};
	assert(inventory_create(&probe.inventory, &inventory_logic_player,
		&server, &probe, INVENTORY_SIZE, 0, 0, 0));
	probe.inventory.reply = ignore_reply; probe.inventory.reply_user = &server;
	probe.active_inventory = &probe.inventory;
	server.guest_respawn_epoch_counter = 41;
	struct server_rpc_source probe_source;
	assert(server_local_bind_guest(&server, &probe, &probe_source));
	assert(server_local_unbind_guest(&server, &probe_source));
	assert(server.guest_respawn_epoch_counter == 41);
	struct server_rpc_source probe_source2;
	assert(server_local_bind_guest(&server, &probe, &probe_source2));
	assert(probe_source2.generation > probe_source.generation);
	assert(server_local_unbind_guest(&server, &probe_source2));
	inventory_destroy(&probe.inventory, &probe);
	struct server_rpc_source source;
	assert(server_local_guest_create(&server,&source));
	assert(source.slot==1 && source.generation && server.guest_owned);
	assert(!server_local_unbind_guest(&server,&source));
	struct server_player* guest=server.guest_actor;
	assert(guest && guest!=&server.player && !guest->finished_loading);
	assert(guest->health==160 && guest->time_until_portal==20);
	assert(!guest->inventory.items[INVENTORY_SLOT_MAIN].id);
	assert(!server_local_guest_create(&server,&source));
	assert(client_session_bind_local(&gstate.players[1],source.generation));
	guest->inventory.items[INVENTORY_SLOT_MAIN]=(struct item_data){.id=ITEM_DIAMOND,.count=13};
	guest->inventory.items[INVENTORY_SLOT_ARMOR]=(struct item_data){.id=310,.durability=273,.count=1};
	guest->inventory.hotbar_slot=7;
	guest->has_bed_spawn=true;guest->bed_spawn_x=12;guest->bed_spawn_y=70;guest->bed_spawn_z=-9;
	guest->x=19.5;guest->z=-12.25;guest->rx=35;guest->fall_distance=2.5F;guest->health=17;
	assert(server_local_guest_publish(&server,source));
	clin_update_drain();
	assert(windowc_get_latest(gstate.players[1].windows[0])->items[INVENTORY_SLOT_MAIN].count==13);
	assert(server.player.inventory.items[INVENTORY_SLOT_MAIN].count==9);
	guest->inventory.picked_item=(struct item_data){.id=BLOCK_STONE,.count=4};
	assert(server_local_guest_checkpoint(&server));
	assert(server.guest_actor==guest && guest->inventory.picked_item.count==4);
	guest->inventory.picked_item=(struct item_data){0};
	guest->inventory.items[INVENTORY_SLOT_CRAFTING]=(struct item_data){.id=BLOCK_STONE,.count=1};
	assert(server_local_guest_checkpoint(&server));
	guest->inventory.items[INVENTORY_SLOT_CRAFTING]=(struct item_data){0};
	assert(server_local_guest_checkpoint(&server));
	/* Save failure must retain the actor and generation; retry remains legal. */
	server.guest_respawn_phase = GUEST_RESPAWN_LOADING;
	server.guest_respawn_generation = source.generation;
	server.guest_respawn_epoch = 7;
	server.guest_respawn_anchor = server.guest_respawn_ready = true;
	string_t old_path;string_init_set(old_path,server.level.file_name);
	string_set_str(server.level.file_name,test_fixture_join(path,"/absent/level.dat"));
	assert(!server_local_guest_leave(&server,source));
	assert(server.guest_actor==guest && server.guest_generation==source.generation);
	assert(server.guest_respawn_phase == GUEST_RESPAWN_LOADING
		&& server.guest_respawn_epoch == 7 && server.guest_respawn_anchor
		&& server.guest_respawn_ready);
	string_set(server.level.file_name,old_path);string_clear(old_path);
	/* Reply survives actor destruction in the inbox, carrying only its copied
	 * generation and item bytes. A replacement must never consume it. */
	struct client_rpc late = {.type=CRPC_INVENTORY_SLOT,
		.payload.inventory_slot={.window=0,.slot=INVENTORY_SLOT_MAIN,
			.item={.id=BLOCK_STONE,.count=1}}};
	assert(clin_rpc_send_local(&late,1,source.generation));
	assert(server_local_guest_leave(&server,source));
	assert(!server.guest_actor && !server.guest_owned);
	assert(server.guest_respawn_phase == GUEST_RESPAWN_IDLE
		&& !server.guest_respawn_epoch && !server.guest_respawn_anchor
		&& !server.guest_respawn_ready);
	assert(!server_local_guest_leave(&server,source));
	assert(client_session_unbind_local(&gstate.players[1],source.generation));

	/* Reopen disk rather than reusing the in-memory checkpoint. */
	level_archive_discard(&server.level);
	assert(level_archive_create(&server.level,server.level_name));
	struct server_rpc_source next;
	assert(server_local_guest_create(&server,&next));
	assert(next.generation>source.generation);
	assert(client_session_bind_local(&gstate.players[1],next.generation));
	assert(!client_session_unbind_local(&gstate.players[1],source.generation));
	clin_update_drain();
	assert(!windowc_get_latest(gstate.players[1].windows[0])->items[INVENTORY_SLOT_MAIN].id);
	assert(server_local_guest_publish(&server,next));
	clin_update_drain();
	assert(windowc_get_latest(gstate.players[1].windows[0])->items[INVENTORY_SLOT_MAIN].count==13);
	struct inventory host_reload;
	assert(inventory_create(&host_reload,&inventory_logic_player,&server,
		&server.player,INVENTORY_SIZE,0,0,0));
	assert(level_archive_read_inventory(&server.level,&host_reload));
	assert(host_reload.items[INVENTORY_SLOT_MAIN].id==BLOCK_DIRT
		&& host_reload.items[INVENTORY_SLOT_MAIN].count==9);
	inventory_destroy(&host_reload,&server.player);
	guest=server.guest_actor;
	assert(guest->inventory.items[INVENTORY_SLOT_MAIN].count==13);
	assert(guest->inventory.items[INVENTORY_SLOT_ARMOR].durability==273);
	assert(guest->inventory.hotbar_slot==7 && guest->health==17);
	assert(guest->x==19.5 && guest->z==-12.25 && guest->rx==35 && guest->fall_distance==2.5F);
	/* A delayed owner-1 fall sample from the retired generation cannot add to
	 * the persisted scalar or damage the replacement body. */
	float rejoin_fall = guest->fall_distance;
	short rejoin_health = guest->health;
	struct server_rpc old_fall = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = guest->x, .y = guest->y - 4.0, .z = guest->z,
			.on_ground = false, .correction_seq = guest->correction_seq}};
	assert(!svin_rpc_send_from(source, &old_fall));
	assert(guest->fall_distance == rejoin_fall && guest->health == rejoin_health);
	assert(guest->has_bed_spawn && guest->bed_spawn_z==-9);
	assert(!server_local_guest_publish(&server,source));
	assert(!server_local_guest_leave(&server,source));
	next=test_live_autosave(&server,next);
	assert(server_local_guest_leave(&server,next));
	assert(client_session_unbind_local(&gstate.players[1],next.generation));
	/* Beta NBT only knows tag types 0..10. Private metadata must remain
	 * skippable by that reader; a torn byte array fails without a reset. */
	nbt_node* window=nbt_find_by_path(server.level.data,".Data.CaveWiiGuest.CaveWiiWindow");
	assert(window && window->type==TAG_BYTE_ARRAY && window->payload.tag_byte_array.length==32);
	nbt_node* private=nbt_find_by_path(server.level.data,".Data.CaveWiiGuest.CaveWiiPrivate");
	assert(private && private->type==TAG_BYTE_ARRAY && private->payload.tag_byte_array.length==75);
	private->payload.tag_byte_array.length=74;
	assert(!server_local_guest_create(&server,&next) && !server.guest_actor);
	private->payload.tag_byte_array.length=75;
	nbt_node* schema=nbt_find_by_path(server.level.data,".Data.CaveWiiGuest.CaveWiiGuestSchema");
	assert(schema && schema->type==TAG_INT && schema->payload.tag_int==1);
	schema->payload.tag_int=2;
	assert(!server_local_guest_create(&server,&next) && !server.guest_actor);
	schema->payload.tag_int=1;
	/* Missing required private fields must not downgrade version 1 to legacy. */
	char* private_name=private->name;char* window_name=window->name;
	private->name="HiddenPrivate";window->name="HiddenWindow";
	assert(!server_local_guest_create(&server,&next) && !server.guest_actor);
	private->name=private_name;window->name=window_name;
	/* Corrupt guest data is a load failure, never a fresh empty inventory.
	 * S1-a3: the probe moved from 0 to -1. ZERO is no longer corruption --
	 * it is the defined fail-closed state a guest that reached zero health
	 * checkpoints (Astra call #8 Q2, "the checkpoint stores health 0 and the
	 * new body inherits it"), and the dedicated case at the end of this file
	 * proves that inheritance. A NEGATIVE health is still impossible for the
	 * writer to produce and is still rejected. */
	nbt_node* health=nbt_find_by_path(server.level.data,".Data.CaveWiiGuest.Health");
	assert(health);health->payload.tag_short=-1;
	assert(!server_local_guest_create(&server,&next));
	assert(!server.guest_actor);
	health->payload.tag_short=17;
	/* R-S1a3 F3 (half 1): ZERO is NOT corruption -- it is the record a guest
	 * that reached zero health checkpoints, and the rejoined body INHERITS it
	 * rather than being refused or silently revived. Mutate
	 * server_local_guest_create's `p->health >= 0` back to `> 0` and this
	 * create FAILS; the S1-a3 suite had no case that could tell the two
	 * apart (its own probe moved to -1, which is rejected either way). */
	health->payload.tag_short=0;
	assert(server_local_guest_create(&server,&next));
	assert(server.guest_actor && server.guest_actor->health==0);
	/* ...and a zero-health body owns no gameplay attachment: settled() -- the
	 * riding/fishing/sleep/death predicate -- refuses it on the health leg. */
	assert(!server_local_guest_settled(&server));
	/* Drop the body WITHOUT checkpointing (a leave would persist the zero);
	 * crash_guest reopens the archive from disk, restoring Health 17 there. */
	crash_guest(&server);
	assert(!server.guest_actor && !server.guest_owned);
	test_guest_handshake(&server);
	inventory_destroy(&server.player.inventory,&server.player);
	assert(!server.level.data && !server.player.has_pos);
	dict_entity_clear(server.entities);
	string_clear(server.level_name);
	test_fixture_dir_cleanup(path);
	puts("PASS guest lifecycle: isolated inventory, disk reload, safe leave and failed-save retention");
}

static void test_saved_dead_guest_rejoin(void) {
	char path[256];
	test_fixture_dir("test_saved_dead_guest", path, sizeof path);
	assert(MAKE_DIR(path) == 0);
	static struct server_local server;
	server_local_init(&server, false, false, false, false, false, false, false);
	string_set_str(server.level_name, path);
	assert(level_archive_create_new(&server.level, server.level_name, 12345,
		(vec3){4, 72, 4}));
	assert(level_archive_save(&server.level));
	/* Saved-dead admission begins in explicit Overworld slot zero. */
	server_world_create(&server.worlds[0], server.level_name,
		WORLD_DIM_OVERWORLD, 12345, true);
	server.worlds[0].local = &server;
	server.worlds[0].session = &server;
	server.player.active_inventory = &server.player.inventory;
	server.player.has_pos = server.player.finished_loading = true;
	server.player.x = 4; server.player.y = 72; server.player.z = 4;
	server.player.health = MAX_PLAYER_HEALTH;
	server.player.dimension = WORLD_DIM_OVERWORLD;
	struct server_rpc_source first;
	assert(server_local_guest_create(&server, &first));
	struct server_player* body = server.guest_actor;
	assert(body && body->health == MAX_PLAYER_HEALTH);
	body->health = 0;
	body->inventory.items[INVENTORY_SLOT_MAIN]
		= (struct item_data){.id = ITEM_DIAMOND, .count = 3};
	assert(server_local_guest_checkpoint(&server));
	assert(level_archive_save(&server.level));
	crash_guest(&server);
	assert(nbt_find_by_path(server.level.data,
		".Data.CaveWiiGuest.Health")->payload.tag_short == 0);
	assert(svin_init());
	assert(server_local_set_split_enabled(&server, true));
	struct client_session* client = &gstate.players[1];
	/* A malformed READY for the current owner must retire without installing
	 * a dead or overfull body. Each refusal gets a fresh public generation. */
	const short bad_health[2] = {-1, MAX_PLAYER_HEALTH + 1};
	for(unsigned n = 0; n < 2; n++) {
		assert(client_session_request_local_join(client));
		pump_handshake(&server); pump_handshake(&server); pump_handshake(&server);
		assert(client->local_phase == CLIENT_GUEST_ATTACHED
			&& !client->local_player);
		struct client_rpc bad = {.type = CRPC_LOCAL_GUEST,
			.payload.local_guest = {.ticket = client->local_ticket,
				.generation = client->local_generation,
				.result = LOCAL_GUEST_GAMEPLAY_READY,
				.entity_id = 99, .health = bad_health[n]}};
		assert(clin_try_rpc_send(&bad));
		clin_update_drain();
		assert(!client->local_player && !client->local_gameplay_ready);
		pump_handshake(&server);
		assert(!server.guest_actor && !client->local_bound);
	}
	assert(client_session_request_local_join(client));
	pump_handshake(&server); pump_handshake(&server); pump_handshake(&server);
	assert(client->local_phase == CLIENT_GUEST_ATTACHED
		&& server.guest_actor && server.guest_actor->health == 0);
	body = server.guest_actor;
	assert(body->inventory.items[INVENTORY_SLOT_MAIN].count == 3);
	int cx = WCOORD_CHUNK_OFFSET((int)floor(body->x));
	int cz = WCOORD_CHUNK_OFFSET((int)floor(body->z));
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&server.worlds[0], cx, cz, &chunk) && chunk);
	chunk->sent = true; /* exact saved-position column, as a published stream */
	assert(client_session_request_local_gameplay(client));
	pump_handshake(&server);
	assert(server.guest_gameplay && body->finished_loading
		&& client->local_gameplay_ready && client->local_player
		&& client->local_player->health == 0
		&& !client_session_local_gameplay_active(client));
	struct input_player_frame frame = {.connected = true};
	assert(screen_death_owner_update(client, 1, &frame, 0.0f,
		0, 0, 320, 240) && client->death.active);
	int hold_before = gstate.config_death_hold_respawn_ms;
	gstate.config_death_hold_respawn_ms = 0;
	assert(screen_death_owner_update(client, 1, &frame, 0.5f,
		0, 0, 320, 240));
	frame.down[IB_GUI_CLICK] = frame.pressed[IB_GUI_CLICK] = true;
	assert(screen_death_owner_update(client, 1, &frame, 0.02f,
		0, 0, 320, 240));
	gstate.config_death_hold_respawn_ms = hold_before;
	assert(client->death.commit_pending == DEATH_COMMIT_RESPAWN);
	server_local_test_process_messages(&server);
	clin_update_drain();
	struct server_world* target = server_local_guest_respawn_target(&server);
	assert(target);
	int tx = WCOORD_CHUNK_OFFSET((int)floor(server.guest_respawn_x));
	int tz = WCOORD_CHUNK_OFFSET((int)floor(server.guest_respawn_z));
	for(int tick = 0; tick < 100 && !server_world_is_chunk_sent(target, tx, tz);
		tick++) {
		server_local_test_update_once(&server);
		clin_update_drain();
	}
	assert(server_world_is_chunk_sent(target, tx, tz));
	complete_real_guest_hazard_respawn(&server, (struct server_rpc_source){
		.slot = 1, .generation = server.guest_generation});
	assert(body->health == MAX_PLAYER_HEALTH && body->finished_loading
		&& client->local_player->health == MAX_PLAYER_HEALTH
		&& client->local_gameplay_ready);
	assert(client_session_request_local_leave(client));
	pump_handshake(&server);
	assert(!server.guest_actor && !client->local_bound);
	server_local_unload_world_impl(&server, true);
	clin_drain_discard();
	client_session_clear_windows(client);
	inventory_destroy(&server.player.inventory, &server.player);
	dict_entity_clear(server.entities);
	string_clear(server.level_name);
	test_fixture_dir_cleanup(path);
	puts("PASS saved-dead guest: durable zero health, public rejoin, owned death pane, explicit respawn");
}

/* ===== S1 fix round 2 (Astra call #8) ====================================
 * block_place_obstructed_for sweeps EVERY PRESENT player body, not just the
 * acting one. Beta: ItemBlock.onItemUse [740c583 ItemBlock.java:45] ->
 * World.canBlockBePlacedAt [World.java:2096] -> World.checkIfAABBIsClear
 * [World.java:1357] -> getEntitiesWithinAABBExcludingEntity((Entity)null,
 * box) [World.java:1358]; the excluded entity is null so NOBODY is exempt,
 * and EntityPlayer inherits preventEntitySpawning = true from EntityLiving.
 *
 * test_guest_lifetime already drives both directions through the real
 * SRPC_BLOCK_PLACE dispatch. This is the direct unit matrix on the gate
 * itself, so a regression names the sweep instead of the RPC path: both
 * directions, the both-clear control, the departed guest, and the host-only
 * world whose byte-identity the goldens pin. */
static struct server_local s_obstruct;
static void test_two_body_placement_obstruction(void) {
	dict_entity_init(s_obstruct.entities);
	struct server_player* host = &s_obstruct.player;
	static struct server_player guest_body;
	struct block_data blk = {.type = BLOCK_STONE};
	const int BX = 40, BY = 70, BZ = -17;
	struct block_info where = {.x = BX, .y = BY, .z = BZ, .block = &blk};
	/* entity_local_player_block_collide takes the EYE position and rebuilds
	 * the 0.6 x 1.8 box beneath it, so "feet in cell BY" is BY + eye height
	 * -- the same convention the dispatch cases above use. */
	const double in_x = BX + 0.5, in_y = BY + PLAYER_EYE_HEIGHT,
				 in_z = BZ + 0.5;
	const double off_x = BX + 8.5, off_y = BY + PLAYER_EYE_HEIGHT,
				 off_z = BZ + 8.5;
#define OBSTRUCT_PUT(p, X, Y, Z) \
	do { (p)->x = (X); (p)->y = (Y); (p)->z = (Z); } while(0)
#define OBSTRUCT_AWAY(p) OBSTRUCT_PUT(p, off_x, off_y, off_z)
#define OBSTRUCT_IN(p) OBSTRUCT_PUT(p, in_x, in_y, in_z)

	/* --- host-only world: exactly the pre-split single-body sweep --------- */
	s_obstruct.guest_actor = NULL;
	OBSTRUCT_AWAY(host);
	assert(!block_place_obstructed_for(&s_obstruct, host, &where));
	OBSTRUCT_IN(host);
	assert(block_place_obstructed_for(&s_obstruct, host, &where));

	/* --- both bodies present -------------------------------------------- */
	s_obstruct.guest_actor = &guest_body;
	s_obstruct.guest_generation = 9; /* presence, not resolution: no gameplay
		admission is set here and the sweep must still see the body */
	assert(!s_obstruct.guest_gameplay);
	/* (a) neither body in the cell -> clear, for EITHER placer. */
	OBSTRUCT_AWAY(host);
	OBSTRUCT_AWAY(&guest_body);
	assert(!block_place_obstructed_for(&s_obstruct, host, &where));
	assert(!block_place_obstructed_for(&s_obstruct, &guest_body, &where));
	/* (b) the HOST standing in the cell blocks the GUEST's placement (new)
	 *     and still blocks its own (the single-body case, preserved). */
	OBSTRUCT_IN(host);
	assert(block_place_obstructed_for(&s_obstruct, &guest_body, &where));
	assert(block_place_obstructed_for(&s_obstruct, host, &where));
	/* (c) the GUEST standing in the cell blocks the HOST's placement (new)
	 *     and still blocks its own. */
	OBSTRUCT_AWAY(host);
	OBSTRUCT_IN(&guest_body);
	assert(block_place_obstructed_for(&s_obstruct, host, &where));
	assert(block_place_obstructed_for(&s_obstruct, &guest_body, &where));

	/* --- a guest that has LEFT owns no body and blocks nothing ------------
	 * guest_release / server_local_unbind_guest clear s->guest_actor; the
	 * stale struct still holds the old coordinates, so this is the exact
	 * lifetime case that would resurrect a departed player as a wall. */
	s_obstruct.guest_actor = NULL;
	s_obstruct.guest_generation = 0;
	assert(block_place_obstructed_for(&s_obstruct, &guest_body, &where)
		   && "the placer is always tested, bound or not");
	assert(!block_place_obstructed_for(&s_obstruct, host, &where));
	/* ...and the host alone still obstructs, so the line above is not
	 * vacuously clear. */
	OBSTRUCT_IN(host);
	assert(block_place_obstructed_for(&s_obstruct, host, &where));
	OBSTRUCT_AWAY(host);
	OBSTRUCT_AWAY(&guest_body);
	assert(!block_place_obstructed_for(&s_obstruct, host, &where));

	/* The presence list itself: host always, guest only while bound. */
	struct server_player* present[SERVER_LOCAL_MAX_PRESENT_ACTORS];
	assert(server_local_present_actors(&s_obstruct, present,
									   SERVER_LOCAL_MAX_PRESENT_ACTORS) == 1
		   && present[0] == host);
	s_obstruct.guest_actor = &guest_body;
	assert(server_local_present_actors(&s_obstruct, present,
									   SERVER_LOCAL_MAX_PRESENT_ACTORS) == 2
		   && present[0] == host && present[1] == &guest_body);
	assert(server_local_present_actors(&s_obstruct, present, 1) == 1
		   && present[0] == host);
	assert(server_local_present_actors(NULL, present,
									   SERVER_LOCAL_MAX_PRESENT_ACTORS) == 0);
	s_obstruct.guest_actor = NULL;
	dict_entity_clear(s_obstruct.entities);
#undef OBSTRUCT_IN
#undef OBSTRUCT_AWAY
#undef OBSTRUCT_PUT
	puts("PASS two-body placement obstruction: every present body refuses the "
		 "placement (both directions), a departed guest does not, host-only is "
		 "the single-body sweep");
}

/* ===== S1-a3: the actor-aware damage pipeline, and zero health ===========
 * Astra call #8 Q2. Every case drives the REAL entry points (the mailbox, the
 * real entity/AI ticks, the real arrow/egg entity ticks) and asserts what
 * happened to the OTHER player as well, which is the isolation S1 exists to
 * prove. The host-path byte-identity half of the claim is a separate file
 * (tests/test_host_damage_golden.c); this one is the guest half. */

/* Materialize a projectile directly. server_local_spawn_arrow only QUEUES into
 * pending_arrows (flushed by the static tick loop), so the tests that want a
 * live entity build one the way tests/test_arrow_env.c does. */
static uint32_t s_a3_next_id = 9000;
static struct entity* a3_spawn_arrow(struct server_local* server, vec3 pos,
									 vec3 vel, uint32_t owner_id,
									 bool from_player,
									 struct actor_ident shooter) {
	uint32_t id = s_a3_next_id++;
	struct entity** slot = dict_entity_safe_get(server->entities, id);
	*slot = malloc(sizeof(struct entity));
	struct entity* e = *slot;
	assert(e);
	entity_arrow(id, e, true, &server->worlds[0], owner_id, ITEM_ARROW,
				 ARROW_DAMAGE, from_player);
	e->data.arrow.shooter_actor = shooter;
	e->teleport(e, pos);
	glm_vec3_copy(vel, e->vel);
	assert(e->tick_server);
	return e;
}

static void a3_equip_armor(struct server_player* p) {
	const uint16_t ids[INVENTORY_SIZE_ARMOR]
		= {ITEM_DIAMOND_HELMET, ITEM_DIAMOND_CHESTPLATE,
		   ITEM_DIAMOND_LEGGINGS, ITEM_DIAMOND_BOOTS};
	for(size_t k = 0; k < INVENTORY_SIZE_ARMOR; k++)
		p->inventory.items[INVENTORY_SLOT_ARMOR + k]
			= (struct item_data) {.id = ids[k], .count = 1, .durability = 0};
}

static void a3_strip_armor(struct server_player* p) {
	for(size_t k = 0; k < INVENTORY_SIZE_ARMOR; k++)
		p->inventory.items[INVENTORY_SLOT_ARMOR + k] = (struct item_data) {0};
}

/* Every entity this case materializes, so none is left in the world for the
 * caller own leave/residency assertions to trip over (a live entity in a
 * column the leave wants to release fails its owner preflight). */
static uint32_t s_a3_spawned[16];
static unsigned s_a3_spawned_n;
static void a3_track(struct entity* e) {
	assert(e && s_a3_spawned_n < 16);
	s_a3_spawned[s_a3_spawned_n++] = e->id;
}
static void a3_remove_entity(struct server_local* server, uint32_t id) {
	struct entity** slot = dict_entity_get(server->entities, id);
	if(!slot || !*slot)
		return;
	clin_rpc_send(&(struct client_rpc) {.type = CRPC_ENTITY_DESTROY,
		.payload.entity_destroy.entity_id = id});
	free(*slot);
	dict_entity_erase(server->entities, id);
}
static void a3_despawn_all(struct server_local* server) {
	for(unsigned k = 0; k < s_a3_spawned_n; k++)
		a3_remove_entity(server, s_a3_spawned[k]);
	s_a3_spawned_n = 0;
	clin_drain_discard();
}
static void a3_despawn_items_near(struct server_local* server,
								   double x, double z) {
	dict_entity_it_t it;
	dict_entity_it(it, server->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		uint32_t id = dict_entity_ref(it)->key;
		dict_entity_next(it);
		if(e && e->type == ENTITY_ITEM
		   && fabsf(e->pos[0] - (float)x) < 8.0f
		   && fabsf(e->pos[2] - (float)z) < 8.0f)
			a3_remove_entity(server, id);
	}
	clin_drain_discard();
}

/* R-S1a3 F1: close an OPEN hurt-resistance window the way the server does --
 * one decay per server tick (server_local_tick_actor_hurt_resistance, the
 * per-tick step server_local_update drives) -- instead of poking the field.
 * Every manual `->hurt_resist_time = 0` between the sub-cases below was
 * replaced by this call, which is why the guest's window never closing is now
 * a FAILURE here instead of being papered over: the assert fires if the decay
 * stops reaching this body. Bounded by the window length + 1 so a decay that
 * runs but never reaches zero cannot spin. */
static void a3_expire_hurt_window(struct server_local* server,
								  struct server_player* p) {
	for(int k = 0; k <= PLAYER_HURT_RESIST_TICKS && p->hurt_resist_time > 0; k++)
		server_local_tick_actor_hurt_resistance(server);
	assert(p->hurt_resist_time == 0
		   && "the per-tick decay must CLOSE this body's window");
}

static void test_guest_damage_pipeline(struct server_local* server,
									   struct server_rpc_source live) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	assert(guest);
	/* Restore everything this case perturbs -- the caller keeps asserting
	 * residency and leave against the ORIGINAL poses. */
	double hx = host->x, hy = host->y, hz = host->z;
	double gx = guest->x, gy = guest->y, gz = guest->z;
	short h_health0 = host->health, g_health0 = guest->health;
	size_t hslot = INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&host->inventory);
	size_t gslot = INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&guest->inventory);
	struct item_data hheld0 = host->inventory.items[hslot];
	struct item_data gheld0 = guest->inventory.items[gslot];
	host->has_pos = host->finished_loading = true;
	guest->has_pos = guest->finished_loading = true;
	host->x = 3.5; host->y = 72.0; host->z = 3.5;
	guest->x = 12.5; guest->y = 72.0; guest->z = 12.5;
	host->health = guest->health = MAX_PLAYER_HEALTH;
	host->hurt_resist_time = guest->hurt_resist_time = 0;
	host->last_damage = guest->last_damage = 0;
	host->damage_remainder = guest->damage_remainder = 0;
	host->vel_x = host->vel_z = guest->vel_x = guest->vel_z = 0.0f;
	a3_strip_armor(host);

	/* ---- (1) a SKELETON ARROW hits the GUEST ---------------------------
	 * Armor mitigates and wears on the GUEST, the knockback lands on the
	 * GUEST body, the health reply carries the guest owner+generation, and
	 * the host is untouched end to end. */
	a3_equip_armor(guest);
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	void (*guest_saved)(const struct client_rpc*, void*) = guest->inventory.reply;
	void* guest_user_saved = guest->inventory.reply_user;
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;
	{
		/* 777 = a mob owner id (NOT PROJECTILE_OWNER_LOCAL_PLAYER), so no
		 * owner grace applies to either body -- Beta excludes the shooting
		 * ENTITY only [740c583 EntityArrow.onUpdate]. */
		vec3 apos = {(float)guest->x, (float)guest->y - 0.85f,
					 (float)guest->z - 1.5f};
		vec3 avel = {0.0f, 0.0f, 2.0f};
		/* G8-BLAST-VELOCITY v4 (ASTRA-V3-REVIEW arrow): the knockBack is
		 * measured from the OWNER's current position (EntityArrow.java:147,
		 * EntityLiving.java:321-332); an owner id naming no entity (the old
		 * 777) is Beta's null owner -- no knockBack at all. So the shooter is
		 * a real, never-ticking skeleton standing on the flight line 3
		 * blocks behind the guest: still a nonzero mob owner (no grace). */
		struct entity* shooter = server_local_spawn_monster_at_w(
			&server->worlds[0], (vec3) {(float)guest->x,
				(float)(guest->y - PLAYER_EYE_HEIGHT), (float)guest->z - 3.0f},
			MONSTER_SKELETON);
		assert(shooter);
		shooter->tick_server = NULL;
		struct entity* arrow = a3_spawn_arrow(server, apos, avel, shooter->id,
											  false, ACTOR_IDENT_HOST);
		a3_track(arrow);
		double gy_before = guest->y;
		s_tag_seen_velocity_count = 0;
		arrow->tick_server(arrow, server);
		clin_drain_discard();
		const uint32_t shooter_id = shooter->id;
		dict_entity_erase(server->entities, shooter_id); /* read at the hit only */
		free(shooter);
		assert(guest->health < MAX_PLAYER_HEALTH); /* the GUEST took it */
		assert(host->health == MAX_PLAYER_HEALTH); /* P1 untouched */
		assert(host->hurt_resist_time == 0);       /* no host i-frame churn */
		/* Armor: the GUEST wore, the host has none to wear. */
		assert(guest->inventory.items[INVENTORY_SLOT_ARMOR].durability > 0);
		/* ARMOR_DAMAGE_DIVISOR mitigation actually applied (a full diamond
		 * set cannot let the raw 32 through). */
		assert(guest->health > MAX_PLAYER_HEALTH - ARROW_DAMAGE);
		/* Knockback on the GUEST body, not the host. Beta's knockBack
		 * (EntityLiving.java:392-403) writes motion, never position: the
		 * body rises on its own client once the motion is delivered there
		 * (EntityTrackerEntry.java:124-127 -> NetClientHandler.java:165-170).
		 * damage-client-0926: so the server-side body keeps its y, carries
		 * the upward motionY, and that motion rides the GUEST envelope as
		 * the owner-1 CRPC_LOCAL_PLAYER_VELOCITY
		 * (test_knockback_client_motion proves the client rise). */
		assert(guest->vel_z != 0.0f && guest->y == gy_before);
		/* G8-BLAST-VELOCITY (ASTRA-V-REVIEW (b)/(c)): knockBack writes the
		 * SERVER motionY and raises beenAttacked; the hit itself sends
		 * nothing -- the tracker phase delivers ONE owner-1 Packet28. */
		assert(guest->motion_y > 0.0 && guest->motion_y <= (double)0.4F);
		assert(guest->been_attacked && s_tag_seen_velocity_count == 0);
		server_local_flush_actor_velocity(server);
		assert(s_tag_seen_velocity_count == 1 && !guest->been_attacked);
		assert(s_tag_last_velocity.payload.local_player_velocity.owner == 1);
		assert(s_tag_last_velocity.payload.local_player_velocity.vel[1]
			   == (float)(int32_t)(guest->motion_y * 8000.0) / 8000.0f);
		assert(host->vel_x == 0.0f && host->vel_z == 0.0f);
		/* The health reply + the armor slot echo both rode the guest
		 * envelope; nothing went out untagged from this hit. */
		assert(s_tag_seen_generation == live.generation);
		assert(!s_tag_seen_untagged);
	}
	guest->inventory.reply = guest_saved;
	guest->inventory.reply_user = guest_user_saved;

	/* ---- (2) ZOMBIE MELEE on the guest, with the hurt-resistance window -- */
	a3_strip_armor(guest);
	guest->health = MAX_PLAYER_HEALTH;
	a3_expire_hurt_window(server, guest); /* the arrow above opened one */
	guest->last_damage = 0;
	guest->damage_remainder = 0;
	guest->vel_x = guest->vel_z = 0.0f;
	{
		struct entity* zombie = server_local_spawn_monster_at(
			(vec3) {(float)guest->x + 0.6F, (float)(guest->y - 1.0),
					(float)guest->z}, MONSTER_ZOMBIE, server);
		assert(zombie);
		a3_track(zombie);
		/* Name the guest as the retained target, the way a guest punch does
		 * (test_guest_aggro_identity proves that latch through the mailbox);
		 * here the point is the SWING that S1-a2 left fail-closed. */
		zombie->data.monster.target_actor
			= (struct actor_ident) {.slot = ACTOR_SLOT_GUEST,
									.generation = live.generation};
		zombie->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;
		zombie->data.monster.attack_time = 0;
		zombie->data.monster.attack_windup = 0;
		short g_before = guest->health;
		short h_before = host->health;
		bool landed = false;
		for(int k = 0; k < 12 && !landed; k++) {
			(void)server_local_test_tick_entities(server);
			clin_drain_discard();
			landed = guest->health < g_before;
		}
		assert(landed && "a mob whose target is the guest now SWINGS at it");
		assert(host->health == h_before); /* and never at the host */
		assert(guest->hurt_resist_time > 0); /* per-body i-frame window open */
		assert(host->hurt_resist_time == 0); /* which is NOT the host window */
		/* An equal hit inside that window is absorbed on the guest -- the
		 * same Beta arbitration, per body. (The mob stays alive and is
		 * retired with every other spawn at the end of this case.) */
		short g_mid = guest->health;
		server_local_hurt_player_for(server, guest, zombie);
		assert(guest->health == g_mid);
		/* R-S1a3 F1: ...and once the window EXPIRES, the identical hit LANDS
		 * again. This is the assertion S1-a3 never made: nothing decayed the
		 * guest's window, so after its first bite the guest was permanently
		 * immune to every equal-or-weaker hit and could not be driven to zero
		 * health at all. */
		a3_expire_hurt_window(server, guest);
		server_local_hurt_player_for(server, guest, zombie);
		clin_drain_discard();
		assert(guest->health < g_mid
			   && "an equal hit AFTER the window expires must land");
		assert(guest->hurt_resist_time == PLAYER_HURT_RESIST_TICKS);
		/* ...while the HOST's own window is untouched and a hit on the host
		 * still lands. */
		server_local_hurt_player_for(server, host, zombie);
		assert(host->health < h_before);
		host->health = MAX_PLAYER_HEALTH;
		a3_expire_hurt_window(server, host); /* the HOST decay, still running */
		host->last_damage = 0;
		host->damage_remainder = 0;
		host->vel_x = host->vel_z = 0.0f;
	}

	/* ---- (3) the guest EATS: heals, and caps at the Beta maximum --------- */
	{
		guest->health = (short)(MAX_PLAYER_HEALTH / 2);
		short before = guest->health;
		assert(server_local_heal_actor(server, guest,
									   guest->health + HEALTH_PER_HEART));
		assert(guest->health == before + HEALTH_PER_HEART);
		assert(host->health == MAX_PLAYER_HEALTH);
		/* Beta heal() cannot exceed the maximum; the writer clamps. */
		assert(server_local_heal_actor(server, guest,
									   (short)(MAX_PLAYER_HEALTH * 2)));
		assert(guest->health == MAX_PLAYER_HEALTH);
	}

	/* ---- (4) a GUEST SNOWBALL hits the HOST once the grace expires -------
	 * Beta: players damage players, and the thrower grace belongs to the
	 * THROWER only. A snowball deals 0 but must be ACCEPTED (knockback +
	 * impact) on the other body. */
	{
		a3_expire_hurt_window(server, host);
		host->last_damage = 0;
		host->vel_x = host->vel_z = 0.0f;
		vec3 apos = {(float)host->x, (float)host->y - 0.85f,
					 (float)host->z - 1.5f};
		vec3 avel = {0.0f, 0.0f, 2.0f};
		/* owner id 0 = a local player body; the GUEST identity is the
		 * thrower, so the grace excludes the guest and NOT the host. */
		struct entity* shot = a3_spawn_arrow(server, apos, avel,
			PROJECTILE_OWNER_LOCAL_PLAYER, true,
			(struct actor_ident) {.slot = ACTOR_SLOT_GUEST,
								  .generation = live.generation});
		a3_track(shot);
		/* Age past the shared grace window so no body is excluded by age
		 * either -- the identity is what must decide, and the host is not the
		 * thrower. */
		shot->data.arrow.age = ARROW_OWNER_GRACE_TICKS + 1;
		short h_before = host->health, g_before = guest->health;
		shot->tick_server(shot, server);
		clin_drain_discard();
		assert(host->health < h_before); /* a guest arrow CAN hit the host */
		assert(guest->health == g_before); /* the thrower is untouched */
		assert(host->vel_z != 0.0f);       /* knockback on the HOST body */
		host->health = MAX_PLAYER_HEALTH;
		a3_expire_hurt_window(server, host);
		host->last_damage = 0;
		host->damage_remainder = 0;
		host->vel_x = host->vel_z = 0.0f;
	}

	/* ---- (5) ZERO HEALTH is a defined, fail-closed state ---------------- */
	{
		/* Arm the two world attachments the cleanup must retire, by identity.
		 * (These inconsistent simultaneous mounts are fixture injection;
		 * production admission itself is covered above.) Here,
		 * what is under test is that a zero-health guest does not LEAVE one
		 * behind, and that the HOST's own are not touched. */
		uint32_t bobber_id = s_a3_next_id++;
		struct entity** bslot = dict_entity_safe_get(server->entities, bobber_id);
		*bslot = malloc(sizeof(struct entity));
		assert(*bslot);
		entity_fish(bobber_id, *bslot, true, &server->worlds[0]);
		(*bslot)->data.fish.angler_actor
			= (struct actor_ident) {.slot = ACTOR_SLOT_GUEST,
									.generation = live.generation};
		a3_track(*bslot);
		guest->fish_entity_id = bobber_id;

		/* R-S1a3 F5: a REAL boat carrying the guest, and a second one carrying
		 * the host. S1-a3 only set `guest->riding_boat = true`, which the
		 * cleanup's unconditional belt-and-braces write clears whether or not
		 * the boat loop runs -- delete the whole ENTITY_BOAT branch of that
		 * loop and the old assertion still passed. Occupancy is armed through
		 * the single writer (server_local_set_boat_occupancy_for), so the
		 * recorded rider_actor is the real identity the cleanup matches on. */
		uint32_t guest_boat_id = s_a3_next_id++;
		struct entity** gb = dict_entity_safe_get(server->entities, guest_boat_id);
		*gb = malloc(sizeof(struct entity));
		assert(*gb);
		entity_boat(guest_boat_id, *gb, true, &server->worlds[0]);
		a3_track(*gb);
		struct entity* guest_boat = *gb;
		/* The mounted pose clamp measures from the vehicle. A constructor at
		 * (0,0,0) cannot carry this fixture's rider at (12.5,72,12.5). */
		guest_boat->teleport(guest_boat, (vec3){(float)guest->x,
			(float)(guest->y - PLAYER_EYE_HEIGHT), (float)guest->z});
		server_local_set_boat_occupancy_for(server, guest_boat, guest, true);
		assert(guest_boat->data.boat.occupied && guest->riding_boat);
		assert(!actor_ident_is_host(guest_boat->data.boat.rider_actor));

		uint32_t host_boat_id = s_a3_next_id++;
		struct entity** hb = dict_entity_safe_get(server->entities, host_boat_id);
		*hb = malloc(sizeof(struct entity));
		assert(*hb);
		entity_boat(host_boat_id, *hb, true, &server->worlds[0]);
		a3_track(*hb);
		struct entity* host_boat = *hb;
		host_boat->teleport(host_boat, (vec3){(float)host->x,
			(float)(host->y - PLAYER_EYE_HEIGHT), (float)host->z});
		server_local_set_boat_occupancy_for(server, host_boat, host, true);
		assert(host_boat->data.boat.occupied && host->riding_boat);
		assert(actor_ident_is_host(host_boat->data.boat.rider_actor));

		struct entity* pig = server_local_spawn_monster_at(
			(vec3) {(float)guest->x + 2.0F, (float)(guest->y - 1.0),
					(float)guest->z}, MONSTER_PIG, server);
		assert(pig);
		a3_track(pig);
		pig->data.monster.pig_ridden = true;
		pig->data.monster.pig_rider_id = server->guest_entity_id;
		pig->data.monster.pig_rider_actor
			= (struct actor_ident) {.slot = ACTOR_SLOT_GUEST,
									.generation = live.generation};

		/* A mob holding the guest as its target -- it must RETIRE the role
		 * rather than keep a permanent harmless target (Astra #8 Q2). */
		struct entity* stalker = server_local_spawn_monster_at(
			(vec3) {(float)guest->x + 1.0F, (float)(guest->y - 1.0),
					(float)guest->z + 1.0F}, MONSTER_ZOMBIE, server);
		assert(stalker);
		a3_track(stalker);
		stalker->data.monster.target_actor
			= (struct actor_ident) {.slot = ACTOR_SLOT_GUEST,
									.generation = live.generation};
		stalker->data.monster.aggro_time = MOB_TARGET_RETENTION_TICKS;

		uint32_t host_deaths_before
			= __atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE);
		bool host_evict_before = server->death_world_evict;
		bool host_death_pos_before = host->has_death_pos;
		short h_before = host->health;

		/* Take the guest to zero through the real owner-1 position mailbox.
		 * An unarmored scalar of 23 bills ceil(23-3)*8 == 160 raw HP. */
		a3_expire_hurt_window(server, guest);
		a3_strip_armor(guest);
		guest->health = MAX_PLAYER_HEALTH;
		guest->fall_distance = 23.0F;
		guest->on_ground = false;
		short host_fall_health = host->health;
		bool host_fall_death_pos = host->has_death_pos;
		uint32_t host_fall_deaths
			= __atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE);
		struct server_rpc fall_land = {.type = SRPC_PLAYER_POS,
			.payload.player_pos = {.x = guest->x, .y = guest->y, .z = guest->z,
				.rx = guest->rx, .ry = guest->ry, .on_ground = true,
				.correction_seq = guest->correction_seq}};
		assert(svin_rpc_send_from(live, &fall_land));
		server_local_test_process_messages(server);
		/* G8-BLAST-VELOCITY v4: the drain accounts the landing; its lethal
		 * hit is the packet's replayed handleFalling (tracker phase). */
		printf("    v4 guest lethal landing: health %d q %d last fall %d\n",
			   (int)guest->health, (int)guest->motion_q_n,
			   guest->motion_q_n
				   ? (int)guest->motion_q[guest->motion_q_n - 1].fall_damage : -1);
		assert(guest->health == MAX_PLAYER_HEALTH && guest->motion_q_n >= 1
			&& guest->motion_q[guest->motion_q_n - 1].fall_damage == 160);
		server_local_test_replay_actor_motion(server);
		clin_update_drain();
		/* (a) health CLAMPS at zero, never negative. */
		assert(guest->health == 0);
		assert(gstate.players[1].local_player
			&& gstate.players[1].local_player->health == 0);
		assert(host->health == host_fall_health
			&& host->has_death_pos == host_fall_death_pos
			&& __atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE)
				== host_fall_deaths);
		/* (b) NO host death journal, NO world eviction, NO shared reset. */
		assert(__atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE)
			   == host_deaths_before);
		assert(server->death_world_evict == host_evict_before);
		assert(host->has_death_pos == host_death_pos_before);
		assert(server->world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
		assert(host->health == h_before); /* the host is entirely unaffected */
		/* (c) riding and fishing retired, by identity. */
		assert(guest->fish_entity_id == 0);
		assert(!dict_entity_get(server->entities, bobber_id));
		assert(!guest->riding_boat && !guest->riding_minecart);
		/* R-S1a3 F5: the BOAT the guest was actually seated in is retired --
		 * by identity, through the single occupancy writer -- and the HOST's
		 * own boat is untouched. Delete the ENTITY_BOAT branch of the cleanup
		 * loop and the first two of these fail. */
		assert(!guest_boat->data.boat.occupied);
		assert(guest_boat->data.boat.occupant_id == 0
			   && actor_ident_is_host(guest_boat->data.boat.rider_actor));
		assert(host_boat->data.boat.occupied && host->riding_boat
			   && actor_ident_is_host(host_boat->data.boat.rider_actor));
		assert(host_boat->data.boat.occupant_id
			   == SERVER_LOCAL_PLAYER_ENTITY_ID);
		/* Dismount the host and retire both hulls now: the rest of this case
		 * ticks entities, and a live boat would drive real boat physics (and
		 * a land-crash drop) through them. */
		server_local_set_boat_occupancy_for(server, host_boat, host, false);
		assert(!host->riding_boat);
		for(unsigned bi = 0; bi < 2; bi++) {
			uint32_t bid = bi ? host_boat_id : guest_boat_id;
			struct entity** bp = dict_entity_get(server->entities, bid);
			assert(bp && *bp);
			clin_rpc_send(&(struct client_rpc) {.type = CRPC_ENTITY_DESTROY,
				.payload.entity_destroy.entity_id = bid});
			free(*bp);
			dict_entity_erase(server->entities, bid);
		}
		clin_drain_discard();
		assert(!pig->data.monster.pig_ridden
			   && pig->data.monster.pig_rider_id == 0
			   && actor_ident_is_host(pig->data.monster.pig_rider_actor));
		/* (d) HEALING is rejected -- Beta EntityLiving.heal gates on
		 * health > 0, and that is the writer first statement. */
		assert(!server_local_heal_actor(server, guest, MAX_PLAYER_HEALTH));
		assert(guest->health == 0);
		/* (e) every gameplay VERB from the dead body is dropped. */
		{
			size_t hotbar_before = inventory_get_hotbar(&guest->inventory);
			struct server_rpc hot = {.type = SRPC_HOTBAR_SLOT,
				.payload.hotbar_slot = {.slot = (uint8_t)((hotbar_before + 1)
					% INVENTORY_SIZE_HOTBAR)}};
			assert(svin_rpc_send_from(live, &hot));
			server_local_test_process_messages(server);
			clin_drain_discard();
			assert(inventory_get_hotbar(&guest->inventory) == hotbar_before);
			assert(server_world_set_block(&server->worlds[0], 13, 70, 13,
				(struct block_data) {.type = BLOCK_STONE}));
			struct server_rpc dig = {.type = SRPC_BLOCK_DIG,
				.payload.block_dig
				= {.x = 13, .y = 70, .z = 13, .finished = true}};
			assert(svin_rpc_send_from(live, &dig));
			server_local_test_process_messages(server);
			clin_drain_discard();
			struct block_data still;
			assert(server_world_get_block(&server->worlds[0], 13, 70, 13, &still)
				   && still.type == BLOCK_STONE);
		}
		/* (f) hostile targeting RETIRES on the next AI tick: the mob drops the
		 * zero-health guest and is free to acquire the host again. */
		(void)server_local_test_tick_entities(server);
		clin_drain_discard();
		assert(actor_ident_is_host(stalker->data.monster.target_actor));
		/* ...and it must not be SWINGING at the corpse either. */
		short g_zero = guest->health;
		for(int k = 0; k < 6; k++) {
			(void)server_local_test_tick_entities(server);
			clin_drain_discard();
		}
		assert(guest->health == g_zero && guest->health == 0);
		/* (g) a further projectile deals nothing: the zero-health body is
		 * still a swept candidate (a corpse is not isDead in Beta), but the
		 * HIT is refused by the same health<=0 guard the host corpse gets --
		 * no damage, no armor wear, no i-frame churn. */
		{
			/* R-S1a3 F4: prove BOTH halves, at the hit site.
			 * (1) The corpse is STILL a swept candidate -- Beta/mc173 filter
			 *     projectile collision by entity KIND, never by liveness
			 *     (mc173 entity/tick.rs:388-399) -- so it has not silently
			 *     dropped out of the sweep. */
			assert(server_local_damage_candidate(server, 1) == guest);
			/* (2) The HIT itself is refused there: `accepted` is the Beta
			 *     attackEntityFrom return, and the corpse's health<=0 guard
			 *     reports false. This is the assertion S1-a3 never made -- the
			 *     old case only checked that nothing happened to the BODY,
			 *     which is equally true of a body that was never swept. */
			bool corpse_accepted = true;
			short resist_before = guest->hurt_resist_time;
			assert(server_local_damage_actor_result(server, guest,
				ARROW_DAMAGE, 0.0f, 1.0f, &corpse_accepted));
			assert(!corpse_accepted && "the corpse REFUSES the hit");
			assert(guest->health == 0);
			assert(guest->hurt_resist_time == resist_before);

			/* ...and the same thing through the REAL arrow entity: the shot
			 * must actually REACH the corpse cell and bounce off it. The
			 * bounce is the discriminator -- arrow_bounce_from_rejected_hit
			 * reverses the tick's travel by exactly -0.1 and resets ticksInAir
			 * to 0 [740c583 EntityArrow.onUpdate], neither of which a
			 * fly-through can produce (a miss only takes the 0.99 air drag and
			 * leaves age at 1). */
			vec3 apos = {(float)guest->x, (float)guest->y - 0.85f,
						 (float)guest->z - 1.5f};
			vec3 avel = {0.0f, 0.0f, 2.0f};
			struct entity* arrow = a3_spawn_arrow(server, apos, avel, 777u,
												  false, ACTOR_IDENT_HOST);
			a3_track(arrow);
			assert(arrow->vel[2] > 0.0f && arrow->data.arrow.age == 0);
			assert(!arrow->tick_server(arrow, server)); /* not consumed */
			clin_drain_discard();
			assert(arrow->vel[2] < 0.0f
				   && "the arrow bounced BACK off the corpse (rejected hit)");
			assert(arrow->data.arrow.age == 0
				   && "...and its ticksInAir was reset by that same bounce");
			assert(!arrow->data.arrow.in_ground); /* not a block stick */
			assert(guest->health == 0);
			assert(guest->hurt_resist_time == resist_before);
		}
		/* (h) CHECKPOINT + SAVE succeed at zero, and zero is what persists. */
		assert(server_local_guest_checkpoint(server));
		assert(server_local_test_save_player_state(server));
		{
			nbt_node* saved = nbt_find_by_path(server->level.data,
				".Data.CaveWiiGuest.Health");
			assert(saved && saved->type == TAG_SHORT
				   && saved->payload.tag_short == 0);
		}
		/* (i) REJOIN does not resurrect: the record is inherited verbatim.
		 * Proven on the persisted archive directly (a full leave/join round
		 * trip is the caller's own lifecycle case), so this asserts the one
		 * thing S1-a3 owns -- the value that comes back is 0, not 20 hearts. */
		{
			nbt_node* saved = nbt_find_by_path(server->level.data,
				".Data.CaveWiiGuest.Health");
			assert(saved->payload.tag_short == 0);
		}
		/* The lethal health transition owns the drop anchor. A later accepted
		 * corpse sample may move presentation, but not the eventual drops. */
		assert(guest->has_death_pos);
		double lethal_x=guest->death_x, lethal_y=guest->death_y, lethal_z=guest->death_z;
		struct server_rpc corpse_move = {.type=SRPC_PLAYER_POS,
			.payload.player_pos={.x=guest->x+.25,.y=guest->y,.z=guest->z+.25,
				.rx=guest->rx,.ry=guest->ry,.correction_seq=guest->correction_seq}};
		server_local_guest_position(server, &corpse_move);
		assert(guest->x != lethal_x && guest->z != lethal_z
			&& guest->death_x == lethal_x && guest->death_y == lethal_y
			&& guest->death_z == lethal_z);

		/* S3 golden: drive the existing owned envelope from the admitted dead
		 * body. Acceptance may only latch the server's respawn/loading plan: an
		 * in-view position is not a client mesh/collision readiness ACK, so this
		 * request must not revive the guest prematurely. */
		struct item_data host_items_before[INVENTORY_SIZE];
		memcpy(host_items_before, host->inventory.items,
			sizeof host_items_before);
		double host_pos_before[3] = {host->x, host->y, host->z};
		short host_health_before = host->health;
		uint32_t host_seq_before = host->correction_seq;
		struct item_data host_cursor_before = host->inventory.picked_item;
		struct inventory* host_window_before = host->active_inventory;
		bool host_death_evict_before = server->death_world_evict;
		bool host_journal_before = server->death_drop_active;
		uint32_t input_ack_before = server->input_ack_pending;
		uint32_t respawn_host_deaths_before = __atomic_load_n(&server->player_deaths,
			__ATOMIC_ACQUIRE);
		uint32_t host_death_cause_before = __atomic_load_n(
			&server->player_death_cause, __ATOMIC_ACQUIRE);
		bool paused_before = server->paused;
		struct item_data guest5_before = guest->inventory.items[5];
		struct item_data guest6_before = guest->inventory.items[6];
		struct item_data guest7_before = guest->inventory.items[7];
		struct item_data guest_crafting_before[INVENTORY_SIZE_CRAFTING];
		memcpy(guest_crafting_before,
			guest->inventory.items + INVENTORY_SLOT_CRAFTING,
			sizeof guest_crafting_before);
		struct item_data guest_picked_before = guest->inventory.picked_item;
		struct entity* client_body_before = gstate.players[1].local_player;
		assert(client_body_before && gstate.players[1].local_gameplay_ready);
		/* A transient host loading barrier must answer a valid dead owner's
		 * real death-screen press with RETRY. A silent drop leaves the UI's
		 * commit_pending latch armed forever, even after the barrier clears. */
		int hold_before = gstate.config_death_hold_respawn_ms;
		gstate.config_death_hold_respawn_ms = 0;
		screen_death_owner_enter(&gstate.players[1]);
		struct input_player_frame death_frame = {.connected = true};
		assert(screen_death_owner_update(&gstate.players[1], 1,
			&death_frame, 0.5F, 0, 240, 640, 240));
		death_frame.down[IB_GUI_CLICK] = true;
		death_frame.pressed[IB_GUI_CLICK] = true;
		assert(screen_death_owner_update(&gstate.players[1], 1,
			&death_frame, 0.02F, 0, 240, 640, 240));
		gstate.config_death_hold_respawn_ms = hold_before;
		assert(gstate.players[1].death.commit_pending == DEATH_COMMIT_RESPAWN);
		unsigned transient_retries = 0;
		clin_test_set_rpc_observer(count_guest_respawn_retry,
			&transient_retries);
		server->player.finished_loading = false;
		server_local_test_process_messages(server);
		server->player.finished_loading = true;
		clin_update_drain();
		clin_test_set_rpc_observer(NULL, NULL);
		death_frame.down[IB_GUI_CLICK] = false;
		death_frame.pressed[IB_GUI_CLICK] = false;
		(void)screen_death_owner_update(&gstate.players[1], 1,
			&death_frame, 0.02F, 0, 240, 640, 240);
		printf("transient respawn: replies=%u failure=%d pending=%d phase=%d health=%d\n",
			transient_retries, (int)gstate.players[1].respawn_failure,
			(int)gstate.players[1].death.commit_pending,
			(int)server->guest_respawn_phase, (int)guest->health);
		fflush(stdout);
		assert(transient_retries == 1
			&& gstate.players[1].respawn_failure == RESPAWN_FAILURE_RETRY
			&& gstate.players[1].death.commit_pending == DEATH_COMMIT_NONE
			&& server->guest_respawn_phase == GUEST_RESPAWN_IDLE
			&& guest->health == 0);
		screen_death_owner_reset(&gstate.players[1]);
		guest->has_bed_spawn = true;
		guest->bed_spawn_x = 12; guest->bed_spawn_y = 70; guest->bed_spawn_z = -9;
		char host_hud_before[sizeof gstate.players[0].hud_message];
		memcpy(host_hud_before, gstate.players[0].hud_message, sizeof host_hud_before);
		unsigned bed_warnings = 0;
		clin_test_set_rpc_observer(count_bed_warning, &bed_warnings);
		memset(guest->inventory.items + INVENTORY_SLOT_CRAFTING, 0,
			sizeof guest_crafting_before);
		guest->inventory.items[INVENTORY_SLOT_CRAFTING]
			= (struct item_data) {.id = ITEM_COAL, .count = 4};
		guest->inventory.picked_item = (struct item_data) {0};
		guest->inventory.items[5] = (struct item_data) {.id = ITEM_COAL, .count = 1};
		guest->inventory.items[6] = (struct item_data) {.id = ITEM_COAL, .count = 2};
		guest->inventory.items[7] = (struct item_data) {.id = ITEM_COAL, .count = 3};
		server_local_test_fail_next_entity_alloc();
		enum server_world_switch_phase switch_before
			= server->world_switch_phase;
		struct server_rpc respawn = {.type = SRPC_RESPAWN,
			.payload.respawn = {.owner = 1, .generation = live.generation}};
		uint64_t counter_before = server->guest_respawn_epoch_counter;
		server->guest_respawn_epoch_counter = UINT64_MAX;
		assert(svin_rpc_send_from(live, &respawn));
		server_local_test_process_messages(server);
		assert(server->guest_respawn_phase == GUEST_RESPAWN_IDLE && guest->health == 0);
		server->guest_respawn_epoch_counter = counter_before;
		assert(svin_rpc_send_from(live, &respawn)
			   && "an admitted dead guest must enqueue its owned respawn");
		server_local_test_process_messages(server);
		clin_update_drain();
		assert(guest->health == 0
			   && "guest must stay dead until an explicit readiness ACK");
		assert(host->health == host_health_before
			   && host->x == host_pos_before[0] && host->y == host_pos_before[1]
			   && host->z == host_pos_before[2]
			   && host->correction_seq == host_seq_before);
		assert(memcmp(host->inventory.items, host_items_before,
			sizeof host_items_before) == 0);
		assert(memcmp(&host->inventory.picked_item, &host_cursor_before,
			sizeof host_cursor_before) == 0 && host->active_inventory == host_window_before
			&& server->death_world_evict == host_death_evict_before
			&& server->death_drop_active == host_journal_before
			&& server->input_ack_pending == input_ack_before
			&& __atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE)
				== respawn_host_deaths_before
			&& __atomic_load_n(&server->player_death_cause, __ATOMIC_ACQUIRE)
				== host_death_cause_before);
		assert(server->paused == paused_before
			   && server->world_switch_phase == switch_before
			   && server->guest_actor == guest
			   && server->guest_generation == live.generation);
		assert(server->guest_respawn_phase == GUEST_RESPAWN_PREPARED
			&& "the crafting-input drop must deterministically fail close");
		{
			/* The injected allocation can be consumed by the real inventory-close
			 * callback. That failure keeps the corpse/plan retryable. */
			assert(guest->health == 0);
			assert(guest->inventory.items[INVENTORY_SLOT_CRAFTING].id == ITEM_COAL
				   && "a failed close must retain its crafting input");
			uint64_t prepared_epoch = server->guest_respawn_epoch;
			struct item_data prepared_crafting
				= guest->inventory.items[INVENTORY_SLOT_CRAFTING];
			struct item_data prepared_cursor=guest->inventory.picked_item;
			struct random_gen prepared_rng=server->rand_src;
			struct random_gen prepared_drop_rng=server->container_drop_rng;
			double prepared_plan[3]={server->guest_respawn_x,
				server->guest_respawn_y,server->guest_respawn_z};
			bool prepared_keep=server->guest_respawn_keep_bed;
			unsigned prepared_replies=bed_warnings;
			respawn.payload.respawn.respawn_epoch = prepared_epoch + 1;
			assert(svin_rpc_send_from(live, &respawn));
			server_local_test_process_messages(server);
			assert(server->guest_respawn_phase == GUEST_RESPAWN_PREPARED
				&& server->guest_respawn_epoch == prepared_epoch
				&& memcmp(&guest->inventory.items[INVENTORY_SLOT_CRAFTING],
					&prepared_crafting, sizeof prepared_crafting) == 0
				&& memcmp(&guest->inventory.picked_item,&prepared_cursor,
					sizeof prepared_cursor)==0
				&& memcmp(&server->rand_src,&prepared_rng,sizeof prepared_rng)==0
				&& memcmp(&server->container_drop_rng,&prepared_drop_rng,
					sizeof prepared_drop_rng)==0
				&& server->guest_respawn_x==prepared_plan[0]
				&& server->guest_respawn_y==prepared_plan[1]
				&& server->guest_respawn_z==prepared_plan[2]
				&& server->guest_respawn_keep_bed==prepared_keep
				&& bed_warnings==prepared_replies);
			respawn.payload.respawn.respawn_epoch = 0;
			/* The retry closes that input successfully; fail the following first
			 * ordinary slot allocation to prove partial-OOM retention. */
			server_local_test_fail_entity_alloc_after(2);
			/* O16 (R06 P1): every stream before the death that drops */
			o16_world_before = server->rand_src;
			o16_host_before = *server_local_player_rand(server, &server->player);
			o16_guest_before = *server_local_player_rand(server, guest);
			assert(svin_rpc_send_from(live, &respawn));
			server_local_test_process_messages(server);
			clin_update_drain();
		}
		assert(server->guest_respawn_phase == GUEST_RESPAWN_LOADING);
		clin_test_set_rpc_observer(NULL, NULL);
		assert(bed_warnings == 1
			&& strcmp(gstate.players[1].hud_message,
				"Your home bed was missing or obstructed") == 0
			&& memcmp(gstate.players[0].hud_message, host_hud_before,
				sizeof host_hud_before) == 0);
		assert(server->guest_respawn_death_x == lethal_x
			&& server->guest_respawn_death_y == lethal_y
			&& server->guest_respawn_death_z == lethal_z);
		assert(gstate.players[1].local_respawn_loading
			&& !gstate.players[1].local_gameplay_ready
			&& gstate.players[1].local_player == client_body_before
			&& gstate.players[1].local_respawn_epoch == server->guest_respawn_epoch);
		/* G6: InventoryPlayer.dropAllItems drops mainInventory (hotbar, then
		 * slots 9..35) BEFORE armorInventory, so the FIRST death allocation
		 * (the one forced to fail) is now the guest's main slot 9 stack, not
		 * armor slot 5: slot 9 is retained, all three armor stacks drop. */
		assert(guest->inventory.items[9].id != 0
			   && !guest->inventory.items[5].id && !guest->inventory.items[6].id
			   && !guest->inventory.items[7].id
			   && "allocate-before-clear must retain only the failed slot drop");
		size_t death_drops = 0, death_drop_items = 0;
		dict_entity_it_t drop_it;
		dict_entity_it(drop_it, server->entities);
		while(!dict_entity_end_p(drop_it)) {
			struct entity* e = dict_entity_ref(drop_it)->value;
			if(e && e->type == ENTITY_ITEM
			   && e->data.item.pickup_delay == 40
			   && e->added_to_chunk
			   && fabsf(e->pos[0] - (float)server->guest_respawn_death_x) < 0.01f
			   && fabsf(e->pos[2] - (float)server->guest_respawn_death_z) < 0.01f) {
				death_drops++;
				death_drop_items += e->data.item.item.count;
			}
			dict_entity_next(drop_it);
		}
		assert(death_drops >= 2 && "successful drops need a loaded chunk owner");
		o16_guest_rng_check(server, guest, death_drops);
		uint64_t epoch = server->guest_respawn_epoch;
		double expected_respawn[3] = {server->guest_respawn_x,
			server->guest_respawn_y, server->guest_respawn_z};
		respawn.payload.respawn.respawn_epoch = epoch;
		assert(svin_rpc_send_from(live, &respawn));
		server_local_test_process_messages(server);
		clin_drain_discard();
		size_t repeated_drops = 0;
		dict_entity_it(drop_it, server->entities);
		while(!dict_entity_end_p(drop_it)) {
			struct entity* e = dict_entity_ref(drop_it)->value;
			if(e && e->type == ENTITY_ITEM && e->data.item.pickup_delay == 40
			   && e->added_to_chunk
			   && fabsf(e->pos[0] - (float)server->guest_respawn_death_x) < 0.01f
			   && fabsf(e->pos[2] - (float)server->guest_respawn_death_z) < 0.01f)
				repeated_drops++;
			dict_entity_next(drop_it);
		}
		assert(repeated_drops == death_drops && "LOADING retry must not drop twice");
		/* Retire this case's combat fixtures, then exercise the real ordinary
		 * save-before-erase owner path. The successful death drops must come
		 * back from the region archive before the destination can commit. */
		a3_despawn_all(server);
		w_coord_t dcx = WCOORD_CHUNK_OFFSET(
			(w_coord_t)floor(server->guest_respawn_death_x));
		w_coord_t dcz = WCOORD_CHUNK_OFFSET(
			(w_coord_t)floor(server->guest_respawn_death_z));
		server_world_light_drain(&server->worlds[0]); /* G9: unload waits for the drain */
		assert(server_world_save_chunk(&server->worlds[0], true, dcx, dcz));
		assert(!server_world_is_chunk_loaded(&server->worlds[0], dcx, dcz));
		struct server_chunk* restored = NULL;
		assert(server_world_load_chunk(&server->worlds[0], dcx, dcz, &restored));
		assert(restored);
		size_t restored_drops = 0, restored_items = 0;
		dict_entity_it(drop_it, server->entities);
		while(!dict_entity_end_p(drop_it)) {
			struct entity* e = dict_entity_ref(drop_it)->value;
			if(e && e->type == ENTITY_ITEM
			   && fabsf(e->pos[0] - (float)server->guest_respawn_death_x) < 0.01f
			   && fabsf(e->pos[2] - (float)server->guest_respawn_death_z) < 0.01f) {
				restored_drops++;
				restored_items += e->data.item.item.count;
			}
			dict_entity_next(drop_it);
		}
		/* Beta EntityItem.writeEntityToNBT/readEntityFromNBT (RetroMCP
		 * b1.7.3) persist Item and Age, but not delayBeforeCanPickup. The
		 * old fixture skipped entity retirement, so checking delay==40 after
		 * reload passed only because the live drops never left memory. */
		assert(restored_drops == death_drops
			&& restored_items == death_drop_items
			   && "death drops must survive real eviction/reload");
		/* Persistence is proved; these drops are no longer part of the fixture. */
		a3_despawn_items_near(server, lethal_x, lethal_z);
		w_coord_t rcx = WCOORD_CHUNK_OFFSET(
			(w_coord_t)floor(server->guest_respawn_x));
		w_coord_t rcz = WCOORD_CHUNK_OFFSET(
			(w_coord_t)floor(server->guest_respawn_z));
		struct server_chunk* destination = dict_server_chunks_get(
			server->worlds[0].chunks, S_CHUNK_ID(rcx, rcz));
		assert(destination);
		destination->sent = false;
		struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
			.payload.local_guest = {.ticket = server->guest_ticket,
				.action = LOCAL_GUEST_RESPAWN_READY, .owner = 1,
				.generation = live.generation, .respawn_epoch = epoch}};
		struct server_rpc wrong_ready = ready;
		wrong_ready.payload.local_guest.ticket++;
		assert(svin_rpc_send_from(live, &wrong_ready));
		server_local_test_process_messages(server);
		assert(!server->guest_respawn_ready && guest->health == 0);
		wrong_ready = ready; wrong_ready.payload.local_guest.owner = 0;
		assert(svin_rpc_send_from(live, &wrong_ready));
		server_local_test_process_messages(server);
		assert(!server->guest_respawn_ready && guest->health == 0);
		wrong_ready = ready; wrong_ready.payload.local_guest.generation++;
		assert(svin_rpc_send_from(live, &wrong_ready));
		server_local_test_process_messages(server);
		assert(!server->guest_respawn_ready && guest->health == 0);
		struct server_rpc_source wrong_source = live; wrong_source.generation--;
		assert(svin_rpc_send_from(wrong_source, &ready));
		server_local_test_process_messages(server);
		assert(!server->guest_respawn_ready && guest->health == 0);
		wrong_ready = ready; wrong_ready.payload.local_guest.respawn_epoch++;
		assert(svin_rpc_send_from(live, &wrong_ready));
		server_local_test_process_messages(server);
		assert(!server->guest_respawn_ready && guest->health == 0);
		gstate.players[1].local_respawn_ack_send_pending = true;
		client_session_flush_local(&gstate.players[1]);
		assert(!gstate.players[1].local_respawn_ack_send_pending
			&& gstate.players[1].local_respawn_awaiting_commit);
		/* A reliably queued ACK is not enqueued again on later frames. */
		client_session_flush_local(&gstate.players[1]);
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server->guest_respawn_ready && guest->health == 0
			   && "an early exact ACK must latch without premature revival");
		destination->sent = true;
		/* Every shared lifecycle barrier that can arise after the early ACK
		 * retains the plan and dead body until this same world is stable. */
		server->world_switch_phase = SERVER_WORLD_SWITCH_PORTAL;
		server_local_guest_respawn_poll(server);
		assert(guest->health == 0 && server->guest_respawn_ready);
		server->world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
		server->death_world_evict = true;
		server_local_guest_respawn_poll(server);
		assert(guest->health == 0 && server->guest_respawn_ready);
		server->death_world_evict = false;
		server->input_ack_pending = 1;
		server_local_guest_respawn_poll(server);
		assert(guest->health == 0 && server->guest_respawn_ready);
		server->input_ack_pending = 0;
		server->player.finished_loading = false;
		server_local_guest_respawn_poll(server);
		assert(guest->health == 0 && server->guest_respawn_ready);
		server->player.finished_loading = true;
		server->player.has_pos = false;
		server_local_guest_respawn_poll(server);
		assert(guest->health == 0 && server->guest_respawn_ready);
		server->player.has_pos = true;
		struct respawn_reply_probe reply_probe = {
			.generation=live.generation, .epoch=epoch};
		client_body_before->on_ground = true;
		clin_test_set_rpc_observer(capture_respawn_reply, &reply_probe);
		server_local_guest_respawn_poll(server);
		clin_test_set_rpc_observer(NULL, NULL);
		clin_update_drain();
		assert(guest->health == MAX_PLAYER_HEALTH && guest->finished_loading);
		assert(guest->fall_distance == 0.0F && guest->old_vel_y == 0.0F
			&& guest->vel_y == 0.0F);
		assert(guest->x == expected_respawn[0]
			   && guest->y == expected_respawn[1]
			   && guest->z == expected_respawn[2]);
		assert(server->guest_respawn_phase == GUEST_RESPAWN_IDLE
			   && !server->guest_respawn_ready && !server->guest_respawn_anchor);
		assert(reply_probe.correction_order > 0
			&& reply_probe.ready_order > reply_probe.correction_order
			&& reply_probe.correction_owner == 1
			&& reply_probe.correction_generation == live.generation
			&& reply_probe.correction_epoch == epoch
			&& (reply_probe.correction_flags & (PLAYER_CORRECTION_POSITION_ONLY
				| PLAYER_CORRECTION_ZERO_VEL_X | PLAYER_CORRECTION_ZERO_VEL_Y
				| PLAYER_CORRECTION_ZERO_VEL_Z | PLAYER_CORRECTION_SET_ON_GROUND))
				== (PLAYER_CORRECTION_POSITION_ONLY | PLAYER_CORRECTION_ZERO_VEL_X
					| PLAYER_CORRECTION_ZERO_VEL_Y | PLAYER_CORRECTION_ZERO_VEL_Z
					| PLAYER_CORRECTION_SET_ON_GROUND)
			&& !(reply_probe.correction_flags & PLAYER_CORRECTION_ON_GROUND));
		assert(gstate.players[1].local_player == client_body_before
			&& gstate.players[1].local_gameplay_ready
			&& !gstate.players[1].local_respawn_loading
			&& gstate.players[1].local_respawn_completed_epoch == epoch
			&& client_body_before->pos[0] == (float)expected_respawn[0]
			&& client_body_before->pos[1] == (float)expected_respawn[1]
			&& client_body_before->pos[2] == (float)expected_respawn[2]
			&& !client_body_before->on_ground);
		/* The complete commit, including publish and feedback, cannot touch host
		 * body, cursor/window, lifecycle or death telemetry. */
		assert(host->health == host_health_before
			&& host->x == host_pos_before[0] && host->y == host_pos_before[1]
			&& host->z == host_pos_before[2] && host->correction_seq == host_seq_before
			&& memcmp(host->inventory.items, host_items_before,
				sizeof host_items_before) == 0
			&& memcmp(&host->inventory.picked_item, &host_cursor_before,
				sizeof host_cursor_before) == 0 && host->active_inventory == host_window_before
			&& server->paused == paused_before && server->world_switch_phase == switch_before
			&& server->death_world_evict == host_death_evict_before
			&& server->death_drop_active == host_journal_before
			&& server->input_ack_pending == input_ack_before
			&& __atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE)
				== respawn_host_deaths_before
			&& __atomic_load_n(&server->player_death_cause, __ATOMIC_ACQUIRE)
				== host_death_cause_before);

		/* Full valid-bed transaction: keep the real bed in place through plan,
		 * hidden anchor, ACK and commit rather than stopping at the planner. */
		enum { BED_X=13, BED_Y=70, BED_Z=13 };
		struct block_data bed_saved[3][3][3];
		for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++)
			for(int dz=-1;dz<=1;dz++)
				assert(server_world_get_block(&server->worlds[0], BED_X+dx,
					BED_Y+dy, BED_Z+dz, &bed_saved[dx+1][dy+1][dz+1]));
		for(int dx=-1;dx<=1;dx++) for(int dz=-1;dz<=1;dz++) {
			(void)server_world_set_block(&server->worlds[0],BED_X+dx,BED_Y-1,BED_Z+dz,
				(struct block_data){.type=BLOCK_STONE});
			(void)server_world_set_block(&server->worlds[0],BED_X+dx,BED_Y,BED_Z+dz,
				(struct block_data){.type=BLOCK_AIR});
			(void)server_world_set_block(&server->worlds[0],BED_X+dx,BED_Y+1,BED_Z+dz,
				(struct block_data){.type=BLOCK_AIR});
			clin_drain_discard();
		}
		/* PAR-BED-META: a lone HEAD (0x8) -- the respawn cell -- which Beta's
		 * onNeighborBlockChange pops silently; a lone meta-0 half would drop
		 * a bed item (BlockBed.java:118-122) and leak it into the next case. */
		(void)server_world_set_block(&server->worlds[0],BED_X,BED_Y,BED_Z,
			(struct block_data){.type=BLOCK_BED,.metadata=8});
		clin_drain_discard();
		guest->has_bed_spawn=true; guest->bed_spawn_x=BED_X;
		guest->bed_spawn_y=BED_Y; guest->bed_spawn_z=BED_Z;
		a3_expire_hurt_window(server, guest);
		guest->health=HEALTH_PER_HEART/2;
		assert(!server_local_damage_actor_result(server,guest,
			8*HEALTH_PER_HEART,0,0,NULL) && guest->health==0);
		struct server_rpc bed_respawn={.type=SRPC_RESPAWN,
			.payload.respawn={.owner=1,.generation=live.generation}};
		assert(svin_rpc_send_from(live,&bed_respawn));
		server_local_test_process_messages(server); clin_update_drain();
		assert(server->guest_respawn_phase==GUEST_RESPAWN_LOADING
			&& server->guest_respawn_anchor && server->guest_respawn_keep_bed
			&& server->guest_respawn_x==BED_X-.5
			&& server->guest_respawn_y==BED_Y+PLAYER_EYE_HEIGHT+.1
			&& server->guest_respawn_z==BED_Z-.5);
		uint64_t bed_epoch=server->guest_respawn_epoch;
		struct server_rpc bed_ready={.type=SRPC_LOCAL_GUEST,
			.payload.local_guest={.ticket=server->guest_ticket,
				.action=LOCAL_GUEST_RESPAWN_READY,.owner=1,
				.generation=live.generation,.respawn_epoch=bed_epoch}};
		assert(svin_rpc_send_from(live,&bed_ready));
		server_local_test_process_messages(server);
		w_coord_t bcx=WCOORD_CHUNK_OFFSET((w_coord_t)floor(server->guest_respawn_x));
		w_coord_t bcz=WCOORD_CHUNK_OFFSET((w_coord_t)floor(server->guest_respawn_z));
		struct server_chunk* bed_destination=dict_server_chunks_get(
			server->worlds[0].chunks,S_CHUNK_ID(bcx,bcz));
		assert(bed_destination); bed_destination->sent=true;
		server_local_guest_respawn_poll(server); clin_update_drain();
		assert(guest->health==MAX_PLAYER_HEALTH && guest->finished_loading
			&& guest->has_bed_spawn && guest->x==BED_X-.5
			&& guest->y==BED_Y+PLAYER_EYE_HEIGHT+.1 && guest->z==BED_Z-.5
			&& server->guest_respawn_phase==GUEST_RESPAWN_IDLE
			&& !server->guest_respawn_anchor);
		for(int dx=-1;dx<=1;dx++) for(int dy=-1;dy<=1;dy++)
			for(int dz=-1;dz<=1;dz++) {
				(void)server_world_set_block(&server->worlds[0],BED_X+dx,BED_Y+dy,BED_Z+dz,
					bed_saved[dx+1][dy+1][dz+1]);
				clin_drain_discard();
			}
		assert(svin_rpc_send_from(live, &ready));
		respawn.payload.respawn.respawn_epoch = 0;
		assert(svin_rpc_send_from(live, &respawn));
		struct server_rpc_source stale_live = live;
		stale_live.generation--;
		respawn.payload.respawn.generation = stale_live.generation;
		assert(svin_rpc_send_from(stale_live, &respawn));
		server_local_test_process_messages(server);
		clin_drain_discard();
		assert(server->guest_respawn_phase == GUEST_RESPAWN_IDLE
			   && guest->health == MAX_PLAYER_HEALTH
			   && "duplicate ACK plus alive/stale requests are inert");
		a3_despawn_items_near(server, expected_respawn[0], expected_respawn[2]);
		/* The production update retires this per-actor barrier at its normal
		 * boundary; this helper-driven test continues into a later logical tick. */
		guest->reject_pos_this_update = false;
		guest->inventory.items[5] = guest5_before;
		guest->inventory.items[6] = guest6_before;
		guest->inventory.items[7] = guest7_before;
		memcpy(guest->inventory.items + INVENTORY_SLOT_CRAFTING,
			guest_crafting_before, sizeof guest_crafting_before);
		guest->inventory.picked_item = guest_picked_before;
	}
	/* Leave the world exactly as empty as it was found. */
	a3_despawn_all(server);
	dict_entity_it_t remaining;
	dict_entity_it(remaining, server->entities);
	while(!dict_entity_end_p(remaining)) {
		struct entity* e = dict_entity_ref(remaining)->value;
		assert((!e || e->type != ENTITY_ITEM)
			   && "damage fixture left an item for a later hazard tick");
		dict_entity_next(remaining);
	}

	/* Restore: the caller keeps driving this body. */
	guest->health = g_health0 > 0 ? g_health0 : MAX_PLAYER_HEALTH;
	host->health = h_health0 > 0 ? h_health0 : MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = host->hurt_resist_time = 0;
	guest->last_damage = host->last_damage = 0;
	guest->damage_remainder = host->damage_remainder = 0;
	guest->vel_x = guest->vel_z = host->vel_x = host->vel_z = 0.0f;
	a3_strip_armor(guest);
	a3_strip_armor(host);
	host->x = hx; host->y = hy; host->z = hz;
	guest->x = gx; guest->y = gy; guest->z = gz;
	host->inventory.items[hslot] = hheld0;
	guest->inventory.items[gslot] = gheld0;
	printf("PASS S1-a3: the guest takes mob/arrow damage with its own"
		   " resistance window, armor and knockback; eats and heals; its"
		   " health replies carry the guest generation; zero health is a"
		   " defined fail-closed state (verbs dropped, heal rejected,"
		   " targeting retired, riding/fishing cleaned, no host journal,"
		   " checkpoint+save succeed, no rejoin resurrection)\n");
}

static void complete_real_guest_hazard_respawn(struct server_local* server,
	struct server_rpc_source live) {
	struct client_session* client = &gstate.players[1];
	struct server_player* guest = server->guest_actor;
	struct server_rpc respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = live.generation}};
	assert(guest && guest->health == 0);
	if(server->guest_respawn_phase == GUEST_RESPAWN_IDLE) {
		assert(svin_rpc_send_from(live, &respawn));
		server_local_test_process_messages(server);
		clin_update_drain();
	}
	assert(server->guest_respawn_phase == GUEST_RESPAWN_LOADING
		&& guest->health == 0 && server->guest_respawn_anchor);
	assert(client->local_respawn_loading
		&& client->local_respawn_epoch == server->guest_respawn_epoch);

	int radius_before = world_server_view_radius();
	world_set_server_view_radius(1);
	struct world ready_world;
	world_create(&ready_world);
	w_coord_t ready_cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(
		client->local_respawn_x));
	w_coord_t ready_cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(
		client->local_respawn_z));
	for(int x = ready_cx - 1; x <= ready_cx + 1; x++)
		for(int z = ready_cz - 1; z <= ready_cz + 1; z++)
			memset(dict_wsection_safe_get(ready_world.sections,
				SECTION_TO_ID(x, z)), 0, sizeof(struct world_section));
	struct world_section* ready_section = dict_wsection_get(
		ready_world.sections, SECTION_TO_ID(ready_cx, ready_cz));
	assert(ready_section);
	w_coord_t floor_y = (w_coord_t)floor(
		client->local_respawn_y - PLAYER_EYE_HEIGHT) - 1;
	w_coord_t floor_base = floor_y & ~(CHUNK_SIZE - 1);
	w_coord_t body_base = (floor_y + 1) & ~(CHUNK_SIZE - 1);
	struct chunk* floor_chunk = chunk_alloc();
	assert(floor_chunk);
	chunk_init(floor_chunk, &ready_world, ready_cx, floor_base, ready_cz);
	ready_section->column[floor_base / CHUNK_SIZE] = floor_chunk;
	floor_chunk->rebuild_displist = floor_chunk->mesh_in_flight = false;
	if(body_base != floor_base) {
		struct chunk* body_chunk = chunk_alloc();
		assert(body_chunk);
		chunk_init(body_chunk, &ready_world, ready_cx, body_base, ready_cz);
		ready_section->column[body_base / CHUNK_SIZE] = body_chunk;
		body_chunk->rebuild_displist = body_chunk->mesh_in_flight = false;
	}
	chunk_set_block_raw(floor_chunk, W2C_COORD((w_coord_t)floor(
		client->local_respawn_x)), W2C_COORD(floor_y),
		W2C_COORD((w_coord_t)floor(client->local_respawn_z)),
		(struct block_data) {.type = BLOCK_STONE});
	assert(client_session_local_respawn_readiness(client,
		&ready_world, true, false, false));
	for(int flush = 0; flush < 3 && !client->local_respawn_awaiting_commit;
		flush++) {
		client_session_flush_local(client);
		server_local_test_process_messages(server);
	}
	clin_update_drain();
	if(server->guest_respawn_phase != GUEST_RESPAWN_IDLE) {
		printf("saved-dead respawn diagnostic: phase=%d hp=%d loaded=%d anchor=%d ready=%d ack=%d client_loading=%d client_ready=%d epoch=%llu target=%d,%d\n",
			(int)server->guest_respawn_phase, (int)guest->health,
			(int)guest->finished_loading, (int)server->guest_respawn_anchor,
			(int)server->guest_respawn_ready,
			(int)client->local_respawn_ack_send_pending,
			(int)client->local_respawn_loading,
			(int)client->local_gameplay_ready,
			(unsigned long long)client->local_respawn_epoch,
			WCOORD_CHUNK_OFFSET((int)floor(server->guest_respawn_x)),
			WCOORD_CHUNK_OFFSET((int)floor(server->guest_respawn_z)));
		fflush(stdout);
	}
	assert(server->guest_respawn_phase == GUEST_RESPAWN_IDLE
		&& guest->health == MAX_PLAYER_HEALTH && guest->finished_loading
		&& guest->fire == -PLAYER_FIRE_RESISTANCE
		&& !guest->has_death_pos && !server->guest_respawn_anchor);
	world_destroy(&ready_world);
	world_set_server_view_radius(radius_before);
}

static void clear_guest_hazard_fixture(struct server_local* server) {
	for(int x = 11; x <= 14; x++)
		for(int y = 69; y <= 71; y++)
			for(int z = 11; z <= 13; z++)
				(void)server_world_set_block(&server->worlds[0], x, y, z,
					(struct block_data) {.type = BLOCK_AIR});
}

static void test_guest_lethal_lava_respawn(struct server_local* server,
	struct server_rpc_source live) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	assert(guest && server->guest_gameplay && guest->finished_loading);
	struct item_data items_before[INVENTORY_SIZE];
	memcpy(items_before, guest->inventory.items, sizeof items_before);
	struct item_data picked_before = guest->inventory.picked_item;
	struct inventory* window_before = guest->active_inventory;
	memset(guest->inventory.items, 0, sizeof items_before);
	guest->inventory.picked_item = (struct item_data) {0};
	guest->active_inventory = &guest->inventory;
	guest->x = 12.85; guest->y = 72.0; guest->z = 12.5;
	guest->health = HEALTH_PER_HEART / 2;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	guest->fire = -PLAYER_FIRE_RESISTANCE;
	clear_guest_hazard_fixture(server);
	assert(server_world_set_block(&server->worlds[0], 12, 69, 12,
		(struct block_data) {.type = BLOCK_SAND}));
	assert(server_world_set_block(&server->worlds[0], 12, 70, 12,
		(struct block_data) {.type = BLOCK_CACTUS}));
	assert(server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_LAVA_STILL}));

	short host_health = host->health;
	bool host_has_death_pos = host->has_death_pos;
	uint32_t host_deaths = __atomic_load_n(&server->player_deaths,
		__ATOMIC_ACQUIRE);
	uint32_t host_cause = __atomic_load_n(&server->player_death_cause,
		__ATOMIC_ACQUIRE);
	enum host_respawn_phase host_phase = server->host_respawn_phase;
	bool host_anchor = server->host_respawn_anchor;
	bool host_ready = server->host_respawn_ready;
	enum server_world_switch_phase switch_phase = server->world_switch_phase;
	bool death_evict = server->death_world_evict;
	bool death_drop = server->death_drop_active;
	uint32_t input_ack = server->input_ack_pending;
	bool paused = server->paused;
	double death_x = guest->x, death_y = guest->y, death_z = guest->z;
	void (*reply_before)(const struct client_rpc*, void*) = guest->inventory.reply;
	void* reply_user_before = guest->inventory.reply_user;
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = 0;
	s_tag_seen_health_count = 0;
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;
	server_local_test_damage_observer_reset();
	assert(server->input_ack_pending == 0);
	run_hazard_update(server, false);
	const struct server_player* observed_actor = NULL;
	enum server_player_damage_cause observed_cause = PLAYER_DAMAGE_UNKNOWN;
	assert(server_local_test_damage_observer_get(
		&observed_actor, &observed_cause));
	assert(guest->health == 0 && guest->fire == -PLAYER_FIRE_RESISTANCE
		&& observed_actor == guest && observed_cause == PLAYER_DAMAGE_LAVA);
	assert(guest->has_death_pos && guest->death_x == death_x
		&& guest->death_y == death_y && guest->death_z == death_z);
	/* One clause per line so a failure names the clause (M2b flake hunt). */
	assert(s_tag_seen_generation == live.generation);
	assert(!s_tag_seen_untagged);
	assert(s_tag_seen_count == 1);
	assert(s_tag_seen_health_count == 1);
	assert(s_tag_last_type == CRPC_PLAYER_SET_HEALTH);
	assert(s_tag_last_health == 0);
	assert(host->health == host_health && host->has_death_pos == host_has_death_pos
		&& __atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE) == host_deaths
		&& __atomic_load_n(&server->player_death_cause, __ATOMIC_ACQUIRE) == host_cause);
	assert(server->host_respawn_phase == host_phase
		&& server->host_respawn_anchor == host_anchor
		&& server->host_respawn_ready == host_ready
		&& server->world_switch_phase == switch_phase
		&& server->death_world_evict == death_evict
		&& server->death_drop_active == death_drop
		&& server->input_ack_pending == input_ack && server->paused == paused);
	guest->inventory.reply = reply_before;
	guest->inventory.reply_user = reply_user_before;
	complete_real_guest_hazard_respawn(server, live);
	short respawn_health = guest->health;
	run_hazard_update(server, false);
	assert(guest->health == respawn_health
		&& guest->fire == -PLAYER_FIRE_RESISTANCE
		&& "old-position lava must not hit the respawned body");
	assert(host->health == host_health
		&& __atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE) == host_deaths
		&& __atomic_load_n(&server->player_death_cause, __ATOMIC_ACQUIRE) == host_cause
		&& host->has_death_pos == host_has_death_pos
		&& server->host_respawn_phase == host_phase
		&& server->host_respawn_anchor == host_anchor
		&& server->host_respawn_ready == host_ready
		&& server->world_switch_phase == switch_phase
		&& server->death_world_evict == death_evict
		&& server->death_drop_active == death_drop
		&& server->input_ack_pending == input_ack && server->paused == paused);
	clear_guest_hazard_fixture(server);
	memcpy(guest->inventory.items, items_before, sizeof items_before);
	guest->inventory.picked_item = picked_before;
	guest->active_inventory = window_before;
	guest->x = 12.5; guest->y = 72.0; guest->z = 12.5;
}

static void test_guest_lethal_dry_burn_respawn(struct server_local* server,
	struct server_rpc_source live) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	assert(guest && server->guest_gameplay && guest->finished_loading);
	struct item_data items_before[INVENTORY_SIZE];
	memcpy(items_before, guest->inventory.items, sizeof items_before);
	struct item_data picked_before = guest->inventory.picked_item;
	struct inventory* window_before = guest->active_inventory;
	memset(guest->inventory.items, 0, sizeof items_before);
	guest->inventory.picked_item = (struct item_data) {0};
	guest->active_inventory = &guest->inventory;
	guest->x = 12.5; guest->y = 72.0; guest->z = 12.5;
	guest->health = HEALTH_PER_HEART / 2;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	guest->fire = 20;
	clear_guest_hazard_fixture(server);
	assert(server_world_set_block(&server->worlds[0], 12, 69, 12,
		(struct block_data) {.type = BLOCK_SAND}));
	assert(server_world_set_block(&server->worlds[0], 12, 70, 12,
		(struct block_data) {.type = BLOCK_CACTUS}));

	short host_health = host->health;
	bool host_has_death_pos = host->has_death_pos;
	uint32_t host_deaths = __atomic_load_n(&server->player_deaths,
		__ATOMIC_ACQUIRE);
	uint32_t host_cause = __atomic_load_n(&server->player_death_cause,
		__ATOMIC_ACQUIRE);
	enum host_respawn_phase host_phase = server->host_respawn_phase;
	bool host_anchor = server->host_respawn_anchor;
	bool host_ready = server->host_respawn_ready;
	enum server_world_switch_phase switch_phase = server->world_switch_phase;
	bool death_evict = server->death_world_evict;
	bool death_drop = server->death_drop_active;
	uint32_t input_ack = server->input_ack_pending;
	bool paused = server->paused;
	double death_x = guest->x, death_y = guest->y, death_z = guest->z;
	void (*reply_before)(const struct client_rpc*, void*) = guest->inventory.reply;
	void* reply_user_before = guest->inventory.reply_user;
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = 0;
	s_tag_seen_health_count = 0;
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;
	server_local_test_damage_observer_reset();
	run_hazard_update(server, false);
	const struct server_player* observed_actor = NULL;
	enum server_player_damage_cause observed_cause = PLAYER_DAMAGE_UNKNOWN;
	assert(server_local_test_damage_observer_get(
		&observed_actor, &observed_cause));
	assert(guest->health == 0 && guest->fire == 20
		&& observed_actor == guest && observed_cause == PLAYER_DAMAGE_FIRE);
	assert(guest->has_death_pos && guest->death_x == death_x
		&& guest->death_y == death_y && guest->death_z == death_z);
	/* One clause per line so a failure names the clause (M2b flake hunt). */
	assert(s_tag_seen_generation == live.generation);
	assert(!s_tag_seen_untagged);
	assert(s_tag_seen_count == 1);
	assert(s_tag_seen_health_count == 1);
	assert(s_tag_last_type == CRPC_PLAYER_SET_HEALTH);
	assert(s_tag_last_health == 0);
	assert(host->health == host_health && host->has_death_pos == host_has_death_pos
		&& __atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE) == host_deaths
		&& __atomic_load_n(&server->player_death_cause, __ATOMIC_ACQUIRE) == host_cause);
	assert(server->host_respawn_phase == host_phase
		&& server->host_respawn_anchor == host_anchor
		&& server->host_respawn_ready == host_ready
		&& server->world_switch_phase == switch_phase
		&& server->death_world_evict == death_evict
		&& server->death_drop_active == death_drop
		&& server->input_ack_pending == input_ack && server->paused == paused);
	guest->inventory.reply = reply_before;
	guest->inventory.reply_user = reply_user_before;
	complete_real_guest_hazard_respawn(server, live);
	short respawn_health = guest->health;
	run_hazard_update(server, false);
	assert(guest->health == respawn_health
		&& guest->fire == -PLAYER_FIRE_RESISTANCE
		&& "a respawned body must not retain the lethal burn timer");
	assert(host->health == host_health
		&& __atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE) == host_deaths
		&& __atomic_load_n(&server->player_death_cause, __ATOMIC_ACQUIRE) == host_cause
		&& host->has_death_pos == host_has_death_pos
		&& server->host_respawn_phase == host_phase
		&& server->host_respawn_anchor == host_anchor
		&& server->host_respawn_ready == host_ready
		&& server->world_switch_phase == switch_phase
		&& server->death_world_evict == death_evict
		&& server->death_drop_active == death_drop
		&& server->input_ack_pending == input_ack && server->paused == paused);
	clear_guest_hazard_fixture(server);
	memcpy(guest->inventory.items, items_before, sizeof items_before);
	guest->inventory.picked_item = picked_before;
	guest->active_inventory = window_before;
	guest->x = 12.5; guest->y = 72.0; guest->z = 12.5;
}

static void test_guest_lethal_lava_does_not_skip_host_hazard(
	struct server_local* server, struct server_rpc_source live) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	assert(guest && server->guest_gameplay && guest->finished_loading);
	struct item_data items_before[INVENTORY_SIZE];
	memcpy(items_before, guest->inventory.items, sizeof items_before);
	struct item_data picked_before = guest->inventory.picked_item;
	struct inventory* window_before = guest->active_inventory;
	memset(guest->inventory.items, 0, sizeof items_before);
	guest->inventory.picked_item = (struct item_data) {0};
	guest->active_inventory = &guest->inventory;
	guest->x = 12.85; guest->y = 72.0; guest->z = 12.5;
	guest->health = HEALTH_PER_HEART / 2;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	guest->fire = -PLAYER_FIRE_RESISTANCE;
	host->x = 3.85; host->y = 72.0; host->z = 3.5;
	host->health = MAX_PLAYER_HEALTH;
	host->hurt_resist_time = host->last_damage = 0;
	host->damage_remainder = 0;
	host->fire = -PLAYER_FIRE_RESISTANCE;
	clear_guest_hazard_fixture(server);
	assert(server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_LAVA_STILL}));
	assert(server_world_set_block(&server->worlds[0], 4, 70, 3,
		(struct block_data) {.type = BLOCK_FIRE}));
	enum host_respawn_phase host_phase = server->host_respawn_phase;
	bool host_anchor = server->host_respawn_anchor;
	bool host_ready = server->host_respawn_ready;
	bool host_has_death_pos = host->has_death_pos;
	uint32_t host_deaths = __atomic_load_n(&server->player_deaths,
		__ATOMIC_ACQUIRE);
	uint32_t host_cause = __atomic_load_n(&server->player_death_cause,
		__ATOMIC_ACQUIRE);
	void (*reply_before)(const struct client_rpc*, void*) = guest->inventory.reply;
	void* reply_user_before = guest->inventory.reply_user;
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = 0;
	s_tag_seen_health_count = 0;
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;
	server_local_test_damage_observer_reset();
	run_hazard_update(server, false);
	const struct server_player* observed_actor = NULL;
	enum server_player_damage_cause observed_cause = PLAYER_DAMAGE_UNKNOWN;
	assert(server_local_test_damage_observer_get(
		&observed_actor, &observed_cause));
	assert(guest->health == 0 && guest->fire == -PLAYER_FIRE_RESISTANCE
		&& guest->has_death_pos);
	/* One clause per line so a failure names the clause (M2b flake hunt). */
	assert(s_tag_seen_generation == live.generation);
	assert(!s_tag_seen_untagged);
	assert(s_tag_seen_count == 1);
	assert(s_tag_seen_health_count == 1);
	assert(s_tag_last_type == CRPC_PLAYER_SET_HEALTH);
	assert(s_tag_last_health == 0);
	assert(host->health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2
		&& host->fire == -PLAYER_FIRE_RESISTANCE + 1
		&& observed_actor == host && observed_cause == PLAYER_DAMAGE_FIRE);
	assert(host->has_death_pos == host_has_death_pos
		&& __atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE) == host_deaths
		&& __atomic_load_n(&server->player_death_cause, __ATOMIC_ACQUIRE) == host_cause
		&& server->host_respawn_phase == host_phase
		&& server->host_respawn_anchor == host_anchor
		&& server->host_respawn_ready == host_ready);
	guest->inventory.reply = reply_before;
	guest->inventory.reply_user = reply_user_before;
	(void)server_world_set_block(&server->worlds[0], 4, 70, 3,
		(struct block_data) {.type = BLOCK_AIR});
	complete_real_guest_hazard_respawn(server, live);
	clear_guest_hazard_fixture(server);
	memcpy(guest->inventory.items, items_before, sizeof items_before);
	guest->inventory.picked_item = picked_before;
	guest->active_inventory = window_before;
	guest->x = 12.5; guest->y = 72.0; guest->z = 12.5;
	host->x = 3.5; host->y = 72.0; host->z = 3.5;
	host->health = MAX_PLAYER_HEALTH;
	host->hurt_resist_time = host->last_damage = 0;
	host->damage_remainder = 0;
	host->fire = -PLAYER_FIRE_RESISTANCE;
}

static void test_guest_drowning(struct server_local* server,
	struct server_rpc_source live) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	assert(guest && server->guest_gameplay && guest->finished_loading);
	double hx = host->x, hy = host->y, hz = host->z;
	double gx = guest->x, gy = guest->y, gz = guest->z;
	short host_health = host->health, guest_health = guest->health;
	short host_oxygen = host->oxygen, guest_oxygen = guest->oxygen;
	bool host_loaded = host->finished_loading;
	uint32_t input_ack_before = server->input_ack_pending;
	int guest_fire = guest->fire;
	void (*reply_before)(const struct client_rpc*, void*) = guest->inventory.reply;
	void* reply_user_before = guest->inventory.reply_user;

	host->x = 3.5; host->y = 72.0; host->z = 3.5;
	host->health = MAX_PLAYER_HEALTH;
	host->oxygen = MAX_OXYGEN;
	host->finished_loading = true;
	guest->x = 12.5; guest->y = 72.0; guest->z = 12.5;
	guest->health = MAX_PLAYER_HEALTH;
	guest->oxygen = MAX_OXYGEN;
	guest->fire = -PLAYER_FIRE_RESISTANCE;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	a3_strip_armor(host);
	a3_strip_armor(guest);
	clear_guest_hazard_fixture(server);
	(void)server_world_set_block(&server->worlds[0], 12, 72, 12,
		(struct block_data) {.type = BLOCK_WATER_STILL});
	struct block_data water_probe;
	assert(server_world_get_block(&server->worlds[0], 12, 72, 12, &water_probe)
		&& water_probe.type == BLOCK_WATER_STILL);
	clin_drain_discard();
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = 0;
	s_tag_seen_health_count = 0;
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;
	server_local_test_damage_observer_reset();

	/* Fallback EntityLiving.java: air 300, decrement once per submerged
	 * living tick, hit/reset at exactly -20. 319 ticks therefore stop at -19. */
	for(int tick = 0; tick < MAX_OXYGEN - OXYGEN_THRESHOLD - 1; tick++)
		run_hazard_update(server, false);
	assert(guest->oxygen == OXYGEN_THRESHOLD + 1
		&& guest->health == MAX_PLAYER_HEALTH
		&& s_tag_seen_health_count == 0);
	assert(host->oxygen == MAX_OXYGEN && host->health == MAX_PLAYER_HEALTH);
	run_hazard_update(server, false);
	const struct server_player* observed_actor = NULL;
	enum server_player_damage_cause observed_cause = PLAYER_DAMAGE_UNKNOWN;
	assert(guest->oxygen == 0
		&& guest->health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART);
	assert(server_local_test_damage_observer_get(
		&observed_actor, &observed_cause));
	assert(observed_actor == guest && observed_cause == PLAYER_DAMAGE_DROWN);
	assert(s_tag_seen_generation == live.generation && !s_tag_seen_untagged
		&& s_tag_seen_health_count == 1
		&& s_tag_last_health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART);
	for(int tick = 0; tick < -OXYGEN_THRESHOLD - 1; tick++)
		run_hazard_update(server, false);
	assert(guest->oxygen == OXYGEN_THRESHOLD + 1
		&& guest->health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART);
	run_hazard_update(server, false);
	assert(guest->oxygen == 0
		&& guest->health == MAX_PLAYER_HEALTH - 2 * HEALTH_PER_HEART
		&& s_tag_seen_health_count == 2);

	/* Surfacing resets immediately; flowing water starts a fresh full breath. */
	(void)server_world_set_block(&server->worlds[0], 12, 72, 12,
		(struct block_data) {.type = BLOCK_AIR});
	run_hazard_update(server, false);
	assert(guest->oxygen == MAX_OXYGEN);
	(void)server_world_set_block(&server->worlds[0], 12, 72, 12,
		(struct block_data) {.type = BLOCK_WATER_FLOW});
	assert(server_world_get_block(&server->worlds[0], 12, 72, 12, &water_probe)
		&& water_probe.type == BLOCK_WATER_FLOW);
	run_hazard_update(server, false);
	assert(guest->oxygen == MAX_OXYGEN - 1);
	(void)server_world_set_block(&server->worlds[0], 12, 72, 12,
		(struct block_data) {.type = BLOCK_AIR});
	run_hazard_update(server, false);
	assert(guest->oxygen == MAX_OXYGEN);

	/* A dead host does not freeze a playable guest; actual global loading does. */
	(void)server_world_set_block(&server->worlds[0], 12, 72, 12,
		(struct block_data) {.type = BLOCK_WATER_STILL});
	assert(server_world_get_block(&server->worlds[0], 12, 72, 12, &water_probe)
		&& water_probe.type == BLOCK_WATER_STILL);
	guest->oxygen = 10;
	host->health = 0;
	run_hazard_update(server, false);
	assert(guest->oxygen == 9 && guest->health > 0);
	host->health = MAX_PLAYER_HEALTH;
	host->finished_loading = false;
	run_hazard_update(server, false);
	assert(guest->oxygen == 9);
	host->finished_loading = true;
	/* The synthetic loading transition arms a host input handoff. Restore the
	 * pre-test barrier state before exercising live guest input/hazards. */
	server->input_ack_pending = input_ack_before;
	assert(server->input_ack_pending == 0);

	/* Every guest lifecycle gate is inert. */
	guest->oxygen = 77;
	unsigned replies_before = s_tag_seen_health_count;
	server->guest_gameplay = false;
	run_hazard_update(server, false);
	assert(guest->oxygen == 77 && s_tag_seen_health_count == replies_before);
	server->guest_gameplay = true;
	server->guest_actor = NULL;
	run_hazard_update(server, false);
	assert(guest->oxygen == 77 && s_tag_seen_health_count == replies_before);
	server->guest_actor = guest;
	guest->finished_loading = false;
	run_hazard_update(server, false);
	assert(guest->oxygen == 77 && s_tag_seen_health_count == replies_before);
	guest->finished_loading = true;
	guest->health = 0;
	run_hazard_update(server, false);
	assert(guest->oxygen == 77 && s_tag_seen_health_count == replies_before);
	guest->health = MAX_PLAYER_HEALTH;
	guest->y = WORLD_HEIGHT + 0.25;
	server_local_test_apply_guest_hazards(server);
	assert(guest->oxygen == MAX_OXYGEN
		&& "a failed/out-of-range eye read is dry");

	/* Rejected position samples cannot move a dry authoritative eye into water. */
	guest->x = 11.5; guest->y = 72.0; guest->z = 12.5;
	guest->oxygen = MAX_OXYGEN;
	s_tag_seen_health_count = 0;
	struct server_rpc wet_pos = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = 12.5, .y = 72.0, .z = 12.5,
			.rx = guest->rx, .ry = guest->ry,
			.correction_seq = guest->correction_seq - 1}};
	assert(svin_rpc_send_from(live, &wet_pos));
	server_local_test_process_messages(server);
	run_hazard_update(server, false);
	assert(guest->x == 11.5 && guest->oxygen == MAX_OXYGEN
		&& s_tag_seen_health_count == 0);
	wet_pos.payload.player_pos.correction_seq = guest->correction_seq;
	wet_pos.payload.player_pos.x = NAN;
	assert(svin_rpc_send_from(live, &wet_pos));
	server_local_test_process_messages(server);
	run_hazard_update(server, false);
	assert(guest->x == 11.5 && guest->oxygen == MAX_OXYGEN);
	wet_pos.payload.player_pos.x = 12.5;
	guest->reject_pos_this_update = true;
	assert(svin_rpc_send_from(live, &wet_pos));
	server_local_test_process_messages(server);
	guest->reject_pos_this_update = false;
	run_hazard_update(server, false);
	assert(guest->x == 11.5 && guest->oxygen == MAX_OXYGEN);
	struct server_rpc_source retired = live;
	retired.generation--;
	assert(svin_rpc_send_from(retired, &wet_pos));
	server_local_test_process_messages(server);
	run_hazard_update(server, false);
	assert(guest->x == 11.5 && guest->oxygen == MAX_OXYGEN);
	assert(svin_rpc_send_from(live, &wet_pos));
	server_local_test_process_messages(server);
	assert(guest->x == 12.5);
	run_hazard_update(server, false);
	assert(guest->oxygen == MAX_OXYGEN - 1);

	/* Join drowning to production's real owner/generation client route. */
	struct client_session* guest_client = &gstate.players[1];
	struct entity* guest_client_body = guest_client->local_player;
	struct entity* host_client_before = gstate.players[0].local_player;
	struct entity route_host_client = {
		.type = ENTITY_LOCAL_PLAYER,
		.health = host->health,
	};
	if(!host_client_before)
		gstate.players[0].local_player = &route_host_client;
	struct entity* host_client_body = gstate.players[0].local_player;
	assert(reply_before);
	assert(reply_user_before == server);
	assert(guest_client_body);
	assert(host_client_body);
	assert(guest_client->local_gameplay_ready);
	assert(guest_client->local_generation == live.generation);
	assert(server->guest_generation == live.generation);
	assert(server->input_ack_pending == 0);
	assert(server_world_get_block(&server->worlds[0], 12, 72, 12, &water_probe)
		&& water_probe.type == BLOCK_WATER_STILL);
	int guest_client_health_before = guest_client_body->health;
	int host_client_health_before = host_client_body->health;
	short host_route_health = host->health;
	uint64_t client_generation = guest_client->local_generation;
	guest->inventory.reply = reply_before;
	guest->inventory.reply_user = reply_user_before;
	guest_client_body->health = guest->health = MAX_PLAYER_HEALTH;
	guest->oxygen = OXYGEN_THRESHOLD + 1;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	run_hazard_update(server, true);
	assert(guest->health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART
		&& guest_client_body->health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART
		&& host->health == host_route_health
		&& host_client_body->health == host_client_health_before);

	/* Retire only the client envelope: server identity remains admitted/live. */
	assert(client_generation != UINT64_MAX);
	guest_client->local_generation = client_generation + 1;
	short stale_server_before = guest->health;
	int stale_client_before = guest_client_body->health;
	guest->oxygen = OXYGEN_THRESHOLD + 1;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	run_hazard_update(server, true);
	assert(server->guest_generation == live.generation
		&& guest->health == stale_server_before - HEALTH_PER_HEART
		&& guest_client_body->health == stale_client_before
		&& host->health == host_route_health
		&& host_client_body->health == host_client_health_before);

	/* Rematching the client generation delivers the next real drowning hit. */
	guest_client->local_generation = client_generation;
	short rematch_server_before = guest->health;
	guest->oxygen = OXYGEN_THRESHOLD + 1;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	run_hazard_update(server, true);
	assert(guest->health == rematch_server_before - HEALTH_PER_HEART
		&& guest_client_body->health == guest->health
		&& host->health == host_route_health
		&& host_client_body->health == host_client_health_before);
	guest_client_body->health = guest_client_health_before;
	gstate.players[0].local_player = host_client_before;
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;

	/* Lethal drowning uses the ordinary guest death/respawn transaction. */
	guest->y = 72.5;
	(void)server_world_set_block(&server->worlds[0], 12, 72, 12,
		(struct block_data) {.type = BLOCK_WATER_STILL});
	assert(server_world_get_block(&server->worlds[0], 12, 72, 12, &water_probe)
		&& water_probe.type == BLOCK_WATER_STILL);
	assert(server->input_ack_pending == 0);
	guest->health = HEALTH_PER_HEART;
	guest->oxygen = OXYGEN_THRESHOLD + 1;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = 0;
	s_tag_seen_health_count = 0;
	uint32_t host_deaths = __atomic_load_n(
		&server->player_deaths, __ATOMIC_ACQUIRE);
	bool host_death_pos = host->has_death_pos;
	double death_x = guest->x, death_y = guest->y, death_z = guest->z;
	server_local_test_damage_observer_reset();
	run_hazard_update(server, false);
	assert(guest->health == 0 && guest->oxygen == 0
		&& s_tag_seen_count == 1 && s_tag_seen_health_count == 1
		&& s_tag_last_type == CRPC_PLAYER_SET_HEALTH && s_tag_last_health == 0
		&& s_tag_seen_generation == live.generation && !s_tag_seen_untagged);
	assert(guest->has_death_pos && guest->death_x == death_x
		&& guest->death_y == death_y && guest->death_z == death_z);
	assert(server_local_test_damage_observer_get(
		&observed_actor, &observed_cause));
	assert(observed_actor == guest && observed_cause == PLAYER_DAMAGE_DROWN);
	assert(host->health == MAX_PLAYER_HEALTH
		&& host->has_death_pos == host_death_pos
		&& __atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE) == host_deaths);
	complete_real_guest_hazard_respawn(server, live);
	assert(guest->health == MAX_PLAYER_HEALTH && guest->oxygen == MAX_OXYGEN
		&& guest->finished_loading && !guest->has_death_pos);
	short respawn_health = guest->health;
	run_hazard_update(server, false);
	assert(guest->health == respawn_health && guest->oxygen == MAX_OXYGEN
		&& "old-position water must not hit the respawned body");

	(void)server_world_set_block(&server->worlds[0], 12, 72, 12,
		(struct block_data) {.type = BLOCK_AIR});
	guest->inventory.reply = reply_before;
	guest->inventory.reply_user = reply_user_before;
	host->x = hx; host->y = hy; host->z = hz;
	guest->x = gx; guest->y = gy; guest->z = gz;
	host->health = host_health; guest->health = guest_health;
	host->oxygen = host_oxygen; guest->oxygen = guest_oxygen;
	host->finished_loading = host_loaded;
	server->input_ack_pending = input_ack_before;
	guest->fire = guest_fire;
	puts("PASS guest drowning: cadence 320/20, surface reset, real reply"
		 " routing/stale rejection, death/respawn oxygen reset");
}

static void test_guest_void(struct server_local* server,
	struct server_rpc_source live) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	struct client_session* client = &gstate.players[1];
	struct entity* client_body = client->local_player;
	assert(guest && client_body && guest->finished_loading
		&& server->guest_gameplay && server->input_ack_pending == 0);
	double gx = guest->x, gy = guest->y, gz = guest->z;
	double hy = host->y;
	short guest_health = guest->health, host_health = host->health;
	bool host_loaded = host->finished_loading;
	bool guest_has_pos = guest->has_pos;
	enum server_world_switch_phase switch_phase = server->world_switch_phase;
	uint32_t input_ack = server->input_ack_pending;
	bool paused = server->paused;
	bool creative = server->creative_mode;
	uint64_t client_generation = client->local_generation;
	int client_health = client_body->health;
	void (*reply)(const struct client_rpc*, void*) = guest->inventory.reply;
	void* reply_user = guest->inventory.reply_user;

	a3_strip_armor(guest);
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	guest->y = VOID_KILL_Y;
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH);

	/* A loaded guest without an authoritative pose has no hazard-eligible body. */
	guest->y = VOID_KILL_Y - 1.0;
	guest->has_pos = false;
	guest->hurt_resist_time = 3;
	guest->last_damage = 5;
	guest->damage_remainder = 7;
	a3_equip_armor(guest);
	s_tag_seen_count = s_tag_seen_health_count = 0;
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH && guest->hurt_resist_time == 2
		&& guest->last_damage == 5 && guest->damage_remainder == 7
		&& guest->inventory.items[INVENTORY_SLOT_ARMOR].durability == 0
		&& !guest->has_death_pos && s_tag_seen_health_count == 0);
	guest->inventory.reply = reply;
	guest->inventory.reply_user = reply_user;
	guest->has_pos = true;
	a3_strip_armor(guest);
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;

	guest->y = VOID_KILL_Y - 1.0;
	server_local_test_damage_observer_reset();
	run_hazard_update(server, false);
	const struct server_player* actor = NULL;
	enum server_player_damage_cause cause = PLAYER_DAMAGE_UNKNOWN;
	assert(guest->health == MAX_PLAYER_HEALTH - 2 * HEALTH_PER_HEART
		&& server_local_test_damage_observer_get(&actor, &cause)
		&& actor == guest && cause == PLAYER_DAMAGE_VOID);
	short resisted = guest->health;
	run_hazard_update(server, false);
	assert(guest->health == resisted);
	a3_expire_hurt_window(server, guest);
	run_hazard_update(server, false);
	assert(guest->health == resisted - 2 * HEALTH_PER_HEART);

	/* The same stationary authoritative pose keeps attempting damage. */
	a3_expire_hurt_window(server, guest);
	short stationary = guest->health;
	run_hazard_update(server, false);
	assert(guest->health == stationary - 2 * HEALTH_PER_HEART);

	/* Rejected/nonfinite input cannot move a safe body into the void. */
	guest->y = VOID_KILL_Y;
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	struct server_rpc bad = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = guest->x, .y = NAN, .z = guest->z,
			.rx = guest->rx, .ry = guest->ry,
			.correction_seq = guest->correction_seq}};
	assert(svin_rpc_send_from(live, &bad));
	server_local_test_process_messages(server);
	run_hazard_update(server, false);
	assert(guest->y == VOID_KILL_Y && guest->health == MAX_PLAYER_HEALTH);
	bad.payload.player_pos.y = VOID_KILL_Y - 1.0;
	bad.payload.player_pos.correction_seq = guest->correction_seq - 1;
	assert(svin_rpc_send_from(live, &bad));
	server_local_test_process_messages(server);
	run_hazard_update(server, false);
	assert(guest->y == VOID_KILL_Y && guest->health == MAX_PLAYER_HEALTH);
	bad.payload.player_pos.correction_seq = guest->correction_seq;
	guest->reject_pos_this_update = true;
	assert(svin_rpc_send_from(live, &bad));
	server_local_test_process_messages(server);
	guest->reject_pos_this_update = false;
	run_hazard_update(server, false);
	assert(guest->y == VOID_KILL_Y && guest->health == MAX_PLAYER_HEALTH);

	/* Guest-local and global lifecycle barriers freeze void damage. */
	guest->y = VOID_KILL_Y - 1.0;
	server->guest_gameplay = false;
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH);
	server->guest_gameplay = true;
	server->guest_actor = NULL;
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH);
	server->guest_actor = guest;
	guest->finished_loading = false;
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH);
	guest->finished_loading = true;
	guest->health = 0;
	run_hazard_update(server, false);
	assert(guest->health == 0);
	guest->health = MAX_PLAYER_HEALTH;
	server->paused = true;
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH);
	server->paused = paused;
	host->finished_loading = false;
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH);
	host->finished_loading = true;
	server->input_ack_pending = input_ack;
	assert(server->input_ack_pending == 0);
	host->health = 0;
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH - 2 * HEALTH_PER_HEART);
	host->health = host_health;

	/* A real input handoff suspends simulation until the exact ACK arrives. */
	a3_expire_hurt_window(server, guest);
	guest->health = host->health = MAX_PLAYER_HEALTH;
	host->y = VOID_KILL_Y - 1.0;
	host->hurt_resist_time = host->last_damage = 0;
	host->damage_remainder = 0;
	bool guest_death_pos = guest->has_death_pos;
	bool host_death_pos = host->has_death_pos;
	s_tag_seen_count = s_tag_seen_health_count = 0;
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;
	server->input_ack_pending = 0x13579u;
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH && host->health == MAX_PLAYER_HEALTH
		&& guest->has_death_pos == guest_death_pos
		&& host->has_death_pos == host_death_pos
		&& s_tag_seen_health_count == 0);
	server->input_ack_pending = input_ack;

	/* A non-idle switch is paired with the production loading suspension. */
	server->world_switch_phase = SERVER_WORLD_SWITCH_PORTAL;
	host->finished_loading = false;
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH && host->health == MAX_PLAYER_HEALTH
		&& guest->has_death_pos == guest_death_pos
		&& host->has_death_pos == host_death_pos
		&& s_tag_seen_health_count == 0);
	server->world_switch_phase = switch_phase;
	host->finished_loading = host_loaded;
	server->input_ack_pending = input_ack;
	host->y = hy;
	guest->inventory.reply = reply;
	guest->inventory.reply_user = reply_user;

	/* Full armor participates in the ordinary living damage path. */
	a3_expire_hurt_window(server, guest);
	a3_equip_armor(guest);
	guest->health = MAX_PLAYER_HEALTH;
	guest->damage_remainder = 0;
	run_hazard_update(server, false);
	assert(guest->health < MAX_PLAYER_HEALTH
		&& guest->health > MAX_PLAYER_HEALTH - 2 * HEALTH_PER_HEART);
	a3_strip_armor(guest);

	/* Host void behavior goes through this same shipping update. */
	guest->y = 72.0;
	host->y = VOID_KILL_Y;
	host->health = MAX_PLAYER_HEALTH;
	host->hurt_resist_time = host->last_damage = 0;
	host->damage_remainder = 0;
	a3_strip_armor(host);
	server_local_test_damage_observer_reset();
	run_hazard_update(server, false);
	assert(host->health == MAX_PLAYER_HEALTH);
	host->y = VOID_KILL_Y - 1.0;
	run_hazard_update(server, false);
	assert(host->health == MAX_PLAYER_HEALTH - 2 * HEALTH_PER_HEART
		&& server_local_test_damage_observer_get(&actor, &cause)
		&& actor == host && cause == PLAYER_DAMAGE_VOID);
	short host_resisted = host->health;
	run_hazard_update(server, false);
	assert(host->health == host_resisted);
	a3_expire_hurt_window(server, host);
	a3_equip_armor(host);
	host->health = MAX_PLAYER_HEALTH;
	host->damage_remainder = 0;
	run_hazard_update(server, false);
	int host_armor_wear = 0;
	for(size_t k = 0; k < INVENTORY_SIZE_ARMOR; k++)
		host_armor_wear += host->inventory.items[INVENTORY_SLOT_ARMOR + k].durability;
	assert(host->health == MAX_PLAYER_HEALTH - 6
		&& host_armor_wear == 16 && host->damage_remainder == 10);
	a3_strip_armor(host);
	a3_expire_hurt_window(server, host);
	server->creative_mode = true;
	host->health = MAX_PLAYER_HEALTH;
	host->damage_remainder = 0;
	server_local_test_damage_observer_reset();
	run_hazard_update(server, false);
	assert(host->health == MAX_PLAYER_HEALTH - 2 * HEALTH_PER_HEART
		&& server_local_test_damage_observer_get(&actor, &cause)
		&& actor == host && cause == PLAYER_DAMAGE_VOID);
	server->creative_mode = creative;
	host->y = hy;
	host->health = host_health;
	host->hurt_resist_time = host->last_damage = 0;
	host->damage_remainder = 0;
	guest->y = VOID_KILL_Y - 1.0;

	/* Real owner route: matching, stale client generation, rematching. */
	struct entity* host_body_before = gstate.players[0].local_player;
	struct entity route_host = {.type = ENTITY_LOCAL_PLAYER,
		.health = host->health};
	if(!host_body_before)
		gstate.players[0].local_player = &route_host;
	struct entity* host_body = gstate.players[0].local_player;
	int host_client_health = host_body->health;
	guest->health = client_body->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	run_hazard_update(server, true);
	assert(guest->health == MAX_PLAYER_HEALTH - 2 * HEALTH_PER_HEART
		&& client_body->health == guest->health
		&& host->health == host_health && host_body->health == host_client_health);
	client->local_generation = client_generation + 1;
	a3_expire_hurt_window(server, guest);
	short stale_server = guest->health;
	int stale_client = client_body->health;
	run_hazard_update(server, true);
	assert(guest->health == stale_server - 2 * HEALTH_PER_HEART
		&& client_body->health == stale_client);
	client->local_generation = client_generation;
	a3_expire_hurt_window(server, guest);
	run_hazard_update(server, true);
	assert(client_body->health == guest->health);
	gstate.players[0].local_player = host_body_before;

	/* Creative retains only the project's void exception. */
	server->creative_mode = true;
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH - 2 * HEALTH_PER_HEART);
	a3_expire_hurt_window(server, guest);
	short creative_health = guest->health;
	assert(server_local_damage_actor_result(
		server, guest, HEALTH_PER_HEART, 0.0f, 0.0f, NULL));
	assert(guest->health == creative_health);
	server->creative_mode = creative;

	/* Lethal void uses the existing guest death and real respawn transaction. */
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;
	guest->health = 2 * HEALTH_PER_HEART;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = s_tag_seen_health_count = 0;
	host->y = VOID_KILL_Y - 1.0;
	host->health = MAX_PLAYER_HEALTH;
	host->hurt_resist_time = host->last_damage = 0;
	host->damage_remainder = 0;
	server_local_test_damage_observer_reset();
	run_hazard_update(server, false);
	assert(guest->health == 0 && guest->has_death_pos
		&& s_tag_seen_count == 1 && s_tag_seen_health_count == 1
		&& s_tag_last_health == 0 && s_tag_seen_generation == live.generation
		&& !s_tag_seen_untagged
		&& server_local_test_damage_observer_get(&actor, &cause)
		&& actor == host && cause == PLAYER_DAMAGE_VOID
		&& host->health == MAX_PLAYER_HEALTH - 2 * HEALTH_PER_HEART);
	uint64_t respawn_counter = server->guest_respawn_epoch_counter;
	complete_real_guest_hazard_respawn(server, live);
	assert(server->guest_respawn_epoch_counter == respawn_counter + 1
		&& server->guest_respawn_epoch == 0
		&& client->local_respawn_completed_epoch == client->local_respawn_epoch
		&& client->local_respawn_epoch == respawn_counter + 1
		&& guest->x == client->local_respawn_x
		&& guest->y == client->local_respawn_y
		&& guest->z == client->local_respawn_z
		&& guest->y >= VOID_KILL_Y && guest->health == MAX_PLAYER_HEALTH
		&& guest->hurt_resist_time == 0 && guest->last_damage == 0
		&& guest->damage_remainder == 0 && !guest->has_death_pos);
	s_tag_seen_count = s_tag_seen_health_count = 0;
	host->y = hy;
	host->health = host_health;
	short fresh_health = guest->health;
	run_hazard_update(server, false);
	assert(guest->health == fresh_health && !guest->has_death_pos
		&& s_tag_seen_health_count == 0);
	guest->inventory.reply = reply;
	guest->inventory.reply_user = reply_user;

	guest->x = gx; guest->y = gy; guest->z = gz;
	guest->health = guest_health;
	host->health = host_health;
	host->y = hy;
	host->finished_loading = host_loaded;
	server->input_ack_pending = input_ack;
	server->creative_mode = creative;
	server->paused = paused;
	client->local_generation = client_generation;
	client_body->health = client_health;
	guest->has_pos = guest_has_pos;
	a3_strip_armor(guest);
	puts("PASS guest void: strict boundary, living damage, armor/resistance,"
		 " barriers, real route, creative exception, death/respawn");
}

/* ===== recovery-0916/01: a blast must reach the admitted guest ===========
 * server_world_explode_now damaged and impulsed `s->player` ALONE, and
 * entity_fireball.c's collision sweep tested `s->player` ALONE, so a ghast
 * fireball that landed on the split-screen guest left it standing unhurt in
 * the middle of a blast that hurt the host. This case is the RED for both.
 *
 * SCOPE (Astra ruling #1 for this slice): match the HOST path at 63ec8e32 --
 * the same explosion_half_hearts damage with exposure fixed at 1, and the
 * same impulse (horizontal velocities assigned, vertical position
 * incremented) -- for BOTH bodies. The Beta gaps that host path
 * still carries (sampled exposure, the 1,352-ray resistance crater, the
 * eye-point distance, non-player entities) are recorded as parity debt in
 * docs/local_splitscreen.md and are NOT closed here.
 *
 * Every case drives a PRODUCTION entry point: server_world_explode_flaming
 * (the ghast fireball's own payload, and the same server_world_explode_now
 * body that TNT, creepers and beds reach) and, for the collision leg, a real
 * ENTITY_FIREBALL joined to the world and ticked by the production
 * entity tick.
 *
 * SYMMETRY IS MEASURED, NOT ASSUMED: the reference leg measures the host
 * ALONE at a geometry, and case (a) then asserts the guest's damage, its
 * assigned horizontal velocities and its vertical increment at the SAME
 * relative geometry are bit-identical to it (Z is zero at this geometry).
 * That proves the guest is handled like the host; the host's own numbers are
 * frozen separately by test_host_damage_golden and test_split_off_equiv. */
static void blast_run(struct server_local* server, vec3 center, float power) {
	struct hazard_update_job job = {.apply_client = false, .running = 1};
	struct thread consumer;
	assert(thread_create(&consumer, hazard_update_entry, &job, 64));
	server_world_explode_flaming(&server->worlds[0], center, power);
	__atomic_store_n(&job.running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_drain_discard();
}

static void blast_tick_until_gone(struct server_local* server, uint32_t id,
								  int max_ticks) {
	struct hazard_update_job job = {.apply_client = false, .running = 1};
	struct thread consumer;
	assert(thread_create(&consumer, hazard_update_entry, &job, 64));
	for(int t = 0; t < max_ticks; t++) {
		struct entity** slot = dict_entity_get(server->entities, id);
		if(!slot || !*slot)
			break;
		server_world_begin_update(&server->worlds[0]);
		assert(server_local_test_tick_entities(server));
	}
	__atomic_store_n(&job.running, 0, __ATOMIC_RELEASE);
	thread_join(&consumer);
	clin_drain_discard();
}

/* Put both bodies at a known pose with a fresh damage window. Every case below
 * starts from here so a leftover i-frame cannot silently absorb a blast and
 * read as "the guest is still not a candidate". */
static void blast_reset_bodies(struct server_local* server, double host_x,
							   double guest_x, short guest_health) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	host->has_pos = guest->has_pos = true;
	host->x = host_x; host->y = 72.0; host->z = 12.5;
	guest->x = guest_x; guest->y = 72.0; guest->z = 12.5;
	host->health = MAX_PLAYER_HEALTH;
	guest->health = guest_health;
	host->hurt_resist_time = guest->hurt_resist_time = 0;
	host->last_damage = guest->last_damage = 0;
	host->damage_remainder = guest->damage_remainder = 0;
	host->vel_x = host->vel_z = 0.0f;
	guest->vel_x = guest->vel_z = 0.0f;
	/* G8-BLAST-VELOCITY: the blast's vertical share is motionY now
	 * (Explosion.doExplosionA:102), so the reference starts from rest. */
	host->vel_y = guest->vel_y = 0.0f;
	/* ASTRA-V-REVIEW: the server motionY carries the impulse; isolate the
	 * pending tracker flag between independent cases too. */
	host->motion_y = guest->motion_y = 0.0;
	host->been_attacked = guest->been_attacked = false;
}

static void test_guest_blasts(struct server_local* server,
							  struct server_rpc_source live) {
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	assert(guest && server->guest_gameplay && guest->finished_loading
		   && host->finished_loading && server->input_ack_pending == 0);

	double hx = host->x, hy = host->y, hz = host->z;
	double gx = guest->x, gy = guest->y, gz = guest->z;
	short host_health0 = host->health, guest_health0 = guest->health;
	float hvx = host->vel_x, hvz = host->vel_z;
	float gvx = guest->vel_x, gvz = guest->vel_z;
	bool guest_has_pos0 = guest->has_pos;
	bool guest_gameplay0 = server->guest_gameplay;
	bool creative0 = server->creative_mode;
	void (*reply0)(const struct client_rpc*, void*) = guest->inventory.reply;
	void* reply_user0 = guest->inventory.reply_user;
	/* An explosion draws up to 300 rays off the WORLD rng and the sibling
	 * cases after this one assert against that stream, so it is restored
	 * verbatim rather than left advanced by however many rays ran. */
	struct random_gen rand_before = server->rand_src;

	const float power = 1.0f; /* Beta EntityFireball newExplosion(...,1.0F,true) */
	clear_guest_hazard_fixture(server);
	a3_strip_armor(host);
	a3_strip_armor(guest);
	server->creative_mode = false;
	clin_drain_discard();

	/* ---- reference: the HOST alone, at the geometry cases (a)/(c) reuse.
	 * This is 63ec8e32's shipped host behaviour, MEASURED. */
	blast_reset_bodies(server, 12.5, 3.5, MAX_PLAYER_HEALTH);
	guest->z = 3.5;
	vec3 ref_center = {(float)host->x - 1.0f, (float)host->y - 1.0f,
					   (float)host->z};
	blast_run(server, ref_center, power);
	short ref_damage = (short)(MAX_PLAYER_HEALTH - host->health);
	double ref_vel_x = host->vel_x, ref_vel_z = host->vel_z; /* v3: double server motion */
	/* G8-BLAST-VELOCITY (ASTRA-RB06-RESIDUALS section 2): the vertical
	 * impulse is motionY, never a position shove. */
	double ref_dy = host->motion_y;
	assert(ref_damage > 0 && ref_vel_x > 0.0f && ref_dy > 0.0
		   && host->y == 72.0 && host->vel_y == 0.0f
		   && "the host path must damage AND push the host (motion, not position)");
	assert(guest->health == MAX_PLAYER_HEALTH && guest->vel_x == 0.0f
		   && guest->vel_z == 0.0f && guest->y == 72.0
		   && guest->vel_y == 0.0f && guest->motion_y == 0.0
		   && "a body twelve blocks away is outside a power-1 blast");

	/* ---- (a) SWAPPED GEOMETRY: guest inside the blast, host outside.
	 * RED before the fix: the guest takes nothing at all. */
	blast_reset_bodies(server, 3.5, 12.5, MAX_PLAYER_HEALTH);
	host->z = 3.5;
	server_local_test_damage_observer_reset();
	vec3 a_center = {(float)guest->x - 1.0f, (float)guest->y - 1.0f,
					 (float)guest->z};
	blast_run(server, a_center, power);
	assert(MAX_PLAYER_HEALTH - guest->health == ref_damage
		   && "the guest must take the SAME blast damage the host took");
	assert(guest->vel_x == ref_vel_x && guest->vel_z == ref_vel_z
		   && guest->motion_y == ref_dy && guest->vel_y == 0.0f && guest->y == 72.0
		   && "the guest must take the SAME blast impulse (vel_x/vel_y/vel_z)");
	const struct server_player* blast_actor = NULL;
	enum server_player_damage_cause blast_cause = PLAYER_DAMAGE_UNKNOWN;
	assert(server_local_test_damage_observer_get(&blast_actor, &blast_cause)
		   && blast_actor == guest
		   && "the blast damage must be billed to the guest body");
	assert(host->health == MAX_PLAYER_HEALTH && host->vel_x == 0.0f
		   && host->vel_z == 0.0f && host->y == 72.0
		   && host->vel_y == 0.0f && host->motion_y == 0.0
		   && "a host outside the radius stays untouched");

	/* ---- (a2) the same swap through the REAL fireball collision sweep.
	 * The shot is dropped onto the guest with the host far away; before the
	 * fix the sweep only knows s->player, so it flies straight THROUGH the
	 * guest and detonates wherever it eventually stops -- never on the guest. */
	blast_reset_bodies(server, 3.5, 12.5, MAX_PLAYER_HEALTH);
	host->z = 3.5;
	for(int y = 73; y <= 77; y++)
		(void)server_world_set_block(&server->worlds[0], 12, y, 12,
			(struct block_data) {.type = BLOCK_AIR});
	clin_drain_discard();
	uint32_t fb_id = server_local_gen_id(server);
	struct entity** fb_slot = dict_entity_safe_get(server->entities, fb_id);
	assert(fb_slot);
	*fb_slot = malloc(sizeof(**fb_slot));
	assert(*fb_slot);
	float fb_dir[3] = {0.0f, -1.0f, 0.0f};
	entity_fireball(fb_id, *fb_slot, true, &server->worlds[0], 0, fb_dir);
	(*fb_slot)->teleport(*fb_slot, (vec3) {12.5f, 76.0f, 12.5f});
	(*fb_slot)->vel[0] = 0.0f;
	(*fb_slot)->vel[1] = -1.0f;
	(*fb_slot)->vel[2] = 0.0f;
	assert(server_local_test_entity_join(server, fb_id));
	a3_track(*fb_slot);
	blast_tick_until_gone(server, fb_id, 12);
	struct entity** fb_after = dict_entity_get(server->entities, fb_id);
	assert((!fb_after || !*fb_after)
		   && "the fireball must die on the body it struck");
	assert(guest->health < MAX_PLAYER_HEALTH
		   && "a ghast fireball aimed at the guest must hurt the guest");
	assert(host->health == MAX_PLAYER_HEALTH
		   && "the distant host is untouched by the guest's fireball");
	a3_despawn_all(server);

	/* ---- (b) BOTH inside ONE blast: both damaged, both moved, and moved
	 * AWAY from the shared centre in opposite directions. */
	blast_reset_bodies(server, 12.5, 13.5, MAX_PLAYER_HEALTH);
	vec3 b_center = {13.0f, 71.5f, 12.5f};
	blast_run(server, b_center, power);
	short both_host = (short)(MAX_PLAYER_HEALTH - host->health);
	short both_guest = (short)(MAX_PLAYER_HEALTH - guest->health);
	assert(both_host > 0 && both_guest == both_host
		   && "two equidistant bodies in one blast take the same damage");
	assert(host->vel_x < 0.0f && guest->vel_x > 0.0f
		   && host->vel_x == -guest->vel_x
		   && "the impulse shoves each body away from the shared centre");
	assert(host->motion_y > 0.0 && guest->motion_y == host->motion_y
		   && host->vel_y == 0.0f && guest->vel_y == 0.0f
		   && host->y == 72.0 && guest->y == 72.0
		   && "both bodies take the same vertical component (motionY, G8-BLAST-VELOCITY)");

	/* ---- (c) NEGATIVE CONTROL -- passes BEFORE and after the fix. A guest
	 * outside the radius, and a bound-but-not-admitted guest standing inside
	 * it, are both untouched. This is the over-application guard. */
	blast_reset_bodies(server, 3.5, 12.5, MAX_PLAYER_HEALTH);
	host->z = 3.5;
	vec3 c_center = {12.5f, 72.0f, 15.5f}; /* 3.0 away; radius is 2.0 */
	blast_run(server, c_center, power);
	assert(guest->health == MAX_PLAYER_HEALTH && guest->vel_x == 0.0f
		   && guest->vel_z == 0.0f && guest->y == 72.0
		   && guest->vel_y == 0.0f && guest->motion_y == 0.0
		   && "a guest strictly outside the radius takes nothing");
	assert(host->health == MAX_PLAYER_HEALTH && host->y == 72.0
		   && host->vel_y == 0.0f && host->motion_y == 0.0
		   && "the distant host takes nothing either");

	blast_reset_bodies(server, 3.5, 12.5, MAX_PLAYER_HEALTH);
	host->z = 3.5;
	server->guest_gameplay = false;
	vec3 c_inside = {(float)guest->x - 1.0f, (float)guest->y - 1.0f,
					 (float)guest->z};
	blast_run(server, c_inside, power);
	assert(guest->health == MAX_PLAYER_HEALTH && guest->vel_x == 0.0f
		   && guest->vel_z == 0.0f && guest->y == 72.0
		   && guest->vel_y == 0.0f && guest->motion_y == 0.0
		   && "a guest not admitted to gameplay is not a blast candidate");
	server->guest_gameplay = true;

	/* ---- (d) LETHAL on the guest, in a blast the host survives: the guest
	 * dies through the ordinary death + real respawn transaction, the host's
	 * own number is EXACTLY the number it took at this geometry in (b), and
	 * nothing crashes on either side. */
	blast_reset_bodies(server, 12.5, 13.5, 2 * HEALTH_PER_HEART);
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = s_tag_seen_health_count = 0;
	server_local_test_damage_observer_reset();
	blast_run(server, b_center, power);
	assert(guest->health == 0 && guest->has_death_pos
		   && "a blast bigger than the guest's health must kill it");
	assert(s_tag_seen_health_count >= 1 && s_tag_last_health == 0
		   && s_tag_seen_generation == live.generation && !s_tag_seen_untagged
		   && "the death must reach the GUEST client, tagged for its slot");
	assert(MAX_PLAYER_HEALTH - host->health == both_host
		   && "a lethal guest must not disturb the host's own blast damage");
	assert(host->health > 0 && host->motion_y > 0.0 && host->vel_y == 0.0f && host->y == 72.0 /* G8-BLAST-VELOCITY: server motionY */
		   && "the surviving host still takes its impulse");
	uint64_t respawn_counter = server->guest_respawn_epoch_counter;
	complete_real_guest_hazard_respawn(server, live);
	assert(server->guest_respawn_epoch_counter == respawn_counter + 1
		   && guest->health == MAX_PLAYER_HEALTH && !guest->has_death_pos
		   && guest->hurt_resist_time == 0
		   && "the guest respawns through the existing handshake");
	s_tag_seen_count = s_tag_seen_health_count = 0;
	guest->inventory.reply = reply0;
	guest->inventory.reply_user = reply_user0;

	host->x = hx; host->y = hy; host->z = hz;
	guest->x = gx; guest->y = gy; guest->z = gz;
	host->health = host_health0;
	guest->health = guest_health0;
	host->vel_x = hvx; host->vel_z = hvz;
	guest->vel_x = gvx; guest->vel_z = gvz;
	host->hurt_resist_time = guest->hurt_resist_time = 0;
	host->last_damage = guest->last_damage = 0;
	host->damage_remainder = guest->damage_remainder = 0;
	guest->has_pos = guest_has_pos0;
	server->guest_gameplay = guest_gameplay0;
	server->creative_mode = creative0;
	server->rand_src = rand_before;
	a3_strip_armor(host);
	a3_strip_armor(guest);
	clear_guest_hazard_fixture(server);
	clin_drain_discard();
	puts("PASS guest blasts: swapped geometry, real fireball collision,"
		 " both-in-blast, outside-radius control, lethal guest + respawn");
}

/* ===== R-S1a3 F1/F2: the PER-TICK steps of the damage pipeline ===========
 * S1-a3 parameterized every EVENT step by actor but left both PER-TICK steps
 * on s->player alone, and its own suite could not see it: the hurt-resistance
 * case poked the field to zero between sub-cases (so nothing ever asserted the
 * window CLOSES), and the knockback case asserted the FIELD WRITE
 * (`vel_z != 0 && y > before`) rather than the effect on the body.
 *
 * This case drives the real per-tick entry points
 * (server_local_tick_actor_hurt_resistance / server_local_tick_actor_knockback,
 * the two calls server_local_update makes) and asserts the EFFECT: the window
 * expires and the next equal hit lands, the shove MOVES the guest body away
 * from its attacker, the correction reaches the guest client, and the stale
 * sample the client authored before it knew about the hit is refused. The host
 * leg of each is asserted alongside as the regression control. */
static void test_guest_per_tick_damage_steps(struct server_local* server,
											 struct server_rpc_source live) {
	struct client_session* client = &gstate.players[1];
	struct server_player* host = &server->player;
	struct server_player* guest = server->guest_actor;
	assert(guest && server->guest_gameplay);
	/* Restore everything this case perturbs: the caller keeps asserting
	 * residency and leave against the ORIGINAL poses. */
	double hx = host->x, hy = host->y, hz = host->z;
	double gx = guest->x, gy = guest->y, gz = guest->z;
	short h_health0 = host->health, g_health0 = guest->health;
	host->has_pos = host->finished_loading = true;
	guest->has_pos = guest->finished_loading = true;
	host->x = 3.5; host->y = 72.0; host->z = 3.5;
	guest->x = 12.5; guest->y = 72.0; guest->z = 12.5;
	host->health = guest->health = MAX_PLAYER_HEALTH;
	host->hurt_resist_time = guest->hurt_resist_time = 0;
	host->last_damage = guest->last_damage = 0;
	host->damage_remainder = guest->damage_remainder = 0;
	host->vel_x = host->vel_z = guest->vel_x = guest->vel_z = 0.0f;
	a3_strip_armor(host);
	a3_strip_armor(guest);
	test_guest_lethal_lava_respawn(server, live);
	test_guest_lethal_dry_burn_respawn(server, live);
	test_guest_lethal_lava_does_not_skip_host_hazard(server, live);
	test_guest_drowning(server, live);
	test_guest_void(server, live);
	test_guest_blasts(server, live);
	/* Ordinary stationary guest lava contact is sampled from the body edge. */
	guest->x = 12.85;
	guest->fire = -PLAYER_FIRE_RESISTANCE;
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = 0;
	s_tag_seen_health_count = 0;
	void (*fire_reply)(const struct client_rpc*, void*) = guest->inventory.reply;
	void* fire_reply_user = guest->inventory.reply_user;
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;
	short fire_host_health = host->health;
	int fire_host_timer = host->fire;
	uint32_t fire_host_deaths = __atomic_load_n(
		&server->player_deaths, __ATOMIC_ACQUIRE);
	server_local_test_damage_observer_reset();
	(void)server_world_set_block(&server->worlds[0], 12, 70, 12,
		(struct block_data) {.type = BLOCK_STONE});
	(void)server_world_set_block(&server->worlds[0], 14, 70, 12,
		(struct block_data) {.type = BLOCK_STONE});
	(void)server_world_set_block(&server->worlds[0], 13, 69, 12,
		(struct block_data) {.type = BLOCK_STONE});
	(void)server_world_set_block(&server->worlds[0], 13, 71, 12,
		(struct block_data) {.type = BLOCK_STONE});
	(void)server_world_set_block(&server->worlds[0], 13, 70, 11,
		(struct block_data) {.type = BLOCK_STONE});
	(void)server_world_set_block(&server->worlds[0], 13, 70, 13,
		(struct block_data) {.type = BLOCK_STONE});
	assert(server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_LAVA_STILL}));
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2)
		   && "ordinary admitted guest lava contact must bill four points");
	/* setOnFireFromLava's 600 (Entity.java:236), then the burning box scan
	 * reaches the same lava cell and advances it (Entity.java:481-487). */
	assert(guest->fire == 601);
	const struct server_player* observed_actor = NULL;
	enum server_player_damage_cause observed_cause = PLAYER_DAMAGE_UNKNOWN;
	assert(server_local_test_damage_observer_get(
		&observed_actor, &observed_cause));
	assert(observed_actor == guest && observed_cause == PLAYER_DAMAGE_LAVA);
	/* G8-BLAST-VELOCITY (ASTRA-V-REVIEW (b)): the fresh lava hit raises
	 * beenAttacked, so the update's tracker phase adds ONE owner-1 velocity
	 * message after the health reply (EntityTrackerEntry:125-127). */
	assert(s_tag_seen_generation == live.generation && !s_tag_seen_untagged
		&& s_tag_seen_count == 2 && s_tag_seen_health_count == 1
		&& s_tag_last_type == CRPC_PLAYER_SET_HEALTH
		&& s_tag_last_health == MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2));
	assert(host->health == fire_host_health && host->fire == fire_host_timer
		&& __atomic_load_n(&server->player_deaths, __ATOMIC_ACQUIRE)
			== fire_host_deaths);
	(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 12, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 14, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 69, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 71, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 70, 11,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 70, 13,
		(struct block_data) {.type = BLOCK_AIR});

	/* Dry burn attempts use the pre-decrement 600, 580 and 560 counters
	 * (Entity.java:208-213). The contact tick left 601 (box ++ after lava's
	 * 600), so they fall on updates 2, 22 and 42; update 2's 1 is absorbed by
	 * the lava window still > 10 (EntityLiving.java:317). */
	short lava_health = guest->health;
	for(int k = 1; k <= 42; k++) {
		run_hazard_update(server, false);
		assert(guest->fire == 601 - k);
		if(k <= 21)
			assert(guest->health == lava_health);
		else if(k <= 41)
			assert(guest->health == lava_health - HEALTH_PER_HEART / 2);
		else
			assert(guest->health == lava_health - HEALTH_PER_HEART);
		unsigned expected_replies = 1u + (k >= 22) + (k >= 42);
		short expected_reply_health = k >= 42
			? lava_health - HEALTH_PER_HEART
			: k >= 22 ? lava_health - HEALTH_PER_HEART / 2 : lava_health;
		assert(s_tag_seen_generation == live.generation && !s_tag_seen_untagged
			&& s_tag_seen_health_count == expected_replies
			&& s_tag_last_type == CRPC_PLAYER_SET_HEALTH
			&& s_tag_last_health == expected_reply_health);
	}
	assert(host->health == fire_host_health && host->fire == fire_host_timer);

	/* Fire body-edge contact bills while the -20..-1 resistance ramp advances;
	 * the twentieth dry contact crosses to the 300-tick ignition timer. */
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->fire = -PLAYER_FIRE_RESISTANCE;
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = 0;
	s_tag_seen_health_count = 0;
	server_local_test_damage_observer_reset();
	assert(server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_FIRE}));
	for(int k = 1; k <= PLAYER_FIRE_RESISTANCE; k++) {
		run_hazard_update(server, false);
		assert(guest->fire == (k < PLAYER_FIRE_RESISTANCE
			? -PLAYER_FIRE_RESISTANCE + k : 300));
		short expected_health = k <= PLAYER_HURT_RESIST_TICKS
			? MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2
			: MAX_PLAYER_HEALTH - HEALTH_PER_HEART;
		assert(guest->health == expected_health);
		assert(s_tag_seen_generation == live.generation && !s_tag_seen_untagged
			&& s_tag_seen_health_count
				== (unsigned)(k <= PLAYER_HURT_RESIST_TICKS ? 1 : 2)
			&& s_tag_last_type == CRPC_PLAYER_SET_HEALTH
			&& s_tag_last_health == expected_health);
	}
	assert(guest->health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART);
	assert(server_local_test_damage_observer_get(
		&observed_actor, &observed_cause));
	assert(observed_actor == guest && observed_cause == PLAYER_DAMAGE_FIRE);

	/* Wet fire still bills contact but does not ignite. Beta Entity.java:168-220
	 * clears in water before lava assigns 600; moveEntity:493-496 then quenches
	 * the still-wet player to -fireResistance after contact. */
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->fire = 40;
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = 0;
	s_tag_seen_health_count = 0;
	assert(server_world_set_block(&server->worlds[0], 12, 70, 12,
		(struct block_data) {.type = BLOCK_WATER_STILL}));
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2
		&& guest->fire == 0);
	assert(s_tag_seen_generation == live.generation && !s_tag_seen_untagged
		&& s_tag_seen_health_count == 1
		&& s_tag_last_type == CRPC_PLAYER_SET_HEALTH
		&& s_tag_last_health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2);
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->z = 12.85;
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = 0;
	s_tag_seen_health_count = 0;
	(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	assert(server_world_set_block(&server->worlds[0], 13, 70, 13,
		(struct block_data) {.type = BLOCK_LAVA_STILL}));
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2)
		&& guest->fire == -PLAYER_FIRE_RESISTANCE);
	assert(s_tag_seen_generation == live.generation && !s_tag_seen_untagged
		&& s_tag_seen_health_count == 1
		&& s_tag_last_type == CRPC_PLAYER_SET_HEALTH
		&& s_tag_last_health == MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2));
	(void)server_world_set_block(&server->worlds[0], 12, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 70, 13,
		(struct block_data) {.type = BLOCK_AIR});
	guest->z = 12.5;

	/* The exact admission gate makes every non-playing lifecycle state inert. */
	guest->x = 12.85;
	guest->health = MAX_PLAYER_HEALTH;
	guest->fire = -PLAYER_FIRE_RESISTANCE;
	assert(server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_FIRE}));
	server->guest_gameplay = false;
	server_local_test_apply_guest_hazards(server);
	assert(guest->health == MAX_PLAYER_HEALTH
		&& guest->fire == -PLAYER_FIRE_RESISTANCE);
	server->guest_gameplay = true;
	server->guest_actor = NULL;
	server_local_test_apply_guest_hazards(server);
	assert(guest->health == MAX_PLAYER_HEALTH
		&& guest->fire == -PLAYER_FIRE_RESISTANCE);
	server->guest_actor = guest;
	guest->finished_loading = false;
	server_local_test_apply_guest_hazards(server);
	assert(guest->health == MAX_PLAYER_HEALTH
		&& guest->fire == -PLAYER_FIRE_RESISTANCE);
	guest->finished_loading = true;
	guest->health = 0;
	server_local_test_apply_guest_hazards(server);
	assert(guest->health == 0 && guest->fire == -PLAYER_FIRE_RESISTANCE);
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;

	/* Guest hazards remain live while the host is dead and in LOADING. */
	short dead_host_health = host->health;
	enum host_respawn_phase fire_host_phase = server->host_respawn_phase;
	bool fire_host_ready = server->host_respawn_ready;
	host->health = 0;
	server->host_respawn_phase = HOST_RESPAWN_PHASE_LOADING;
	server->host_respawn_ready = false;
	server_local_test_apply_guest_hazards(server);
	clin_drain_discard();
	assert(guest->health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2
		&& guest->fire == -PLAYER_FIRE_RESISTANCE + 1);
	assert(host->health == 0
		&& server->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING
		&& !server->host_respawn_ready);
	host->health = dead_host_health;
	server->host_respawn_phase = fire_host_phase;
	server->host_respawn_ready = fire_host_ready;

	/* R1: the host wrapper dead guard prevents drown/void tail processing. */
	short dead_oxygen = host->oxygen;
	int dead_fire = host->fire;
	double dead_y = host->y;
	host->health = 0;
	host->oxygen = OXYGEN_THRESHOLD;
	host->fire = 20;
	host->y = VOID_KILL_Y - 1.0;
	assert(server_local_test_apply_player_hazards(
		server, true, false, false, false));
	assert(host->health == 0 && host->oxygen == OXYGEN_THRESHOLD
		&& host->fire == 20);
	host->health = dead_host_health;
	host->oxygen = dead_oxygen;
	host->fire = dead_fire;
	host->y = dead_y;

	/* Lava precedes cactus; the shared window absorbs the weaker second hit. */
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->fire = -PLAYER_FIRE_RESISTANCE;
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = 0;
	s_tag_seen_health_count = 0;
	server_local_test_damage_observer_reset();
	assert(server_world_set_block(&server->worlds[0], 12, 70, 12,
		(struct block_data) {.type = BLOCK_CACTUS}));
	assert(server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_LAVA_STILL}));
	server_local_test_apply_guest_hazards(server);
	clin_drain_discard();
	/* 600 from lava, +1 from the burning box on the same cell
	 * (Entity.java:236, 481-487). */
	assert(guest->health == MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2)
		&& guest->fire == 601);
	assert(server_local_test_damage_observer_get(
		&observed_actor, &observed_cause));
	assert(observed_actor == guest && observed_cause == PLAYER_DAMAGE_LAVA);
	assert(s_tag_seen_generation == live.generation && !s_tag_seen_untagged
		&& s_tag_seen_health_count == 1
		&& s_tag_last_type == CRPC_PLAYER_SET_HEALTH
		&& s_tag_last_health == MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2));
	(void)server_world_set_block(&server->worlds[0], 12, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 70, 13,
		(struct block_data) {.type = BLOCK_AIR});
	guest->z = 12.5;

	/* R3: a prior cactus point opens the shared window, then a larger lava hit
	 * in that same window applies only its 24-point excess. */
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->fire = -PLAYER_FIRE_RESISTANCE;
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = 0;
	s_tag_seen_health_count = 0;
	assert(server_world_set_block(&server->worlds[0], 12, 69, 12,
		(struct block_data) {.type = BLOCK_SAND}));
	assert(server_world_set_block(&server->worlds[0], 12, 70, 12,
		(struct block_data) {.type = BLOCK_CACTUS}));
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2
		&& guest->hurt_resist_time > 0);
	assert(s_tag_seen_generation == live.generation && !s_tag_seen_untagged
		&& s_tag_seen_health_count == 1
		&& s_tag_last_type == CRPC_PLAYER_SET_HEALTH
		&& s_tag_last_health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2);
	(void)server_world_set_block(&server->worlds[0], 12, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	assert(server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_LAVA_STILL}));
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2)
		&& guest->last_damage == 4 * (HEALTH_PER_HEART / 2)
		&& guest->fire == 601); /* lava 600 + burning box (Entity.java:484) */
	assert(s_tag_seen_generation == live.generation && !s_tag_seen_untagged
		&& s_tag_seen_health_count == 2
		&& s_tag_last_type == CRPC_PLAYER_SET_HEALTH
		&& s_tag_last_health == MAX_PLAYER_HEALTH - 4 * (HEALTH_PER_HEART / 2));
	(void)server_world_set_block(&server->worlds[0], 12, 69, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	guest->inventory.reply = fire_reply;
	guest->inventory.reply_user = fire_reply_user;
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->fire = -PLAYER_FIRE_RESISTANCE;
	guest->x = 12.5;

	/* Ordinary stationary guest cactus contact is sampled by the real server
	 * update, even without a fresh PLAYER_POS packet.  The center remains in
	 * x=12 while the Beta 0.6-wide body reaches the cactus in x=13. */
	guest->x = 12.698;
	guest->z = 12.5;
	(void)server_world_set_block(&server->worlds[0], 12, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 14, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 70, 11,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 70, 13,
		(struct block_data) {.type = BLOCK_AIR});
	assert(server_world_set_block(&server->worlds[0], 13, 68, 12,
		(struct block_data) {.type = BLOCK_STONE}));
	assert(server_world_set_block(&server->worlds[0], 13, 69, 12,
		(struct block_data) {.type = BLOCK_SAND}));
	(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_CACTUS});
	s_tag_seen_generation = 0;
	s_tag_seen_untagged = false;
	s_tag_seen_count = 0;
	s_tag_seen_health_count = 0;
	void (*cactus_reply)(const struct client_rpc*, void*) = guest->inventory.reply;
	void* cactus_user = guest->inventory.reply_user;
	guest->inventory.reply = tag_probe_reply;
	guest->inventory.reply_user = server;
	clin_drain_discard();
	/* A rejected producer/sample cannot manufacture cactus contact. */
	double cactus_clear_x = guest->x;
	short cactus_clear_health = guest->health;
	struct server_rpc cactus_pos = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = 12.7625, .y = guest->y, .z = guest->z,
			.correction_seq = guest->correction_seq - 1}};
	assert(svin_rpc_send_from(live, &cactus_pos));
	server_local_test_process_messages(server);
	run_hazard_update(server, false);
	assert(guest->x == cactus_clear_x && guest->health == cactus_clear_health
		   && s_tag_seen_count == 0);
	guest->reject_pos_this_update = true;
	cactus_pos.payload.player_pos.correction_seq = guest->correction_seq;
	assert(svin_rpc_send_from(live, &cactus_pos));
	server_local_test_process_messages(server);
	guest->reject_pos_this_update = false;
	run_hazard_update(server, false);
	assert(guest->x == cactus_clear_x && guest->health == cactus_clear_health
		   && s_tag_seen_count == 0);
	struct server_rpc_source retired = live;
	retired.generation--;
	assert(svin_rpc_send_from(retired, &cactus_pos));
	server_local_test_process_messages(server);
	run_hazard_update(server, false);
	assert(guest->x == cactus_clear_x && guest->health == cactus_clear_health
		   && guest->fire == -PLAYER_FIRE_RESISTANCE && s_tag_seen_count == 0);
	assert(svin_rpc_send_from(live, &cactus_pos));
	server_local_test_process_messages(server);
	assert(guest->x == 12.7625);
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2
		   && "ordinary admitted guest cactus contact must bill one point");
	assert(host->health == MAX_PLAYER_HEALTH && host->hurt_resist_time == 0);
	assert(s_tag_seen_generation == live.generation && !s_tag_seen_untagged);
	assert(s_tag_seen_health_count == 1
		&& s_tag_last_type == CRPC_PLAYER_SET_HEALTH
		&& s_tag_last_health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2);
	short cactus_once = guest->health;
	(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_CACTUS});
	run_hazard_update(server, false);
	assert(guest->health == cactus_once
		   && "the immediate equal cactus callback is absorbed");
	for(int k = 2; k <= PLAYER_HURT_RESIST_TICKS; k++) {
		(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
			(struct block_data) {.type = BLOCK_CACTUS});
		run_hazard_update(server, false);
		if(k < PLAYER_HURT_RESIST_TICKS)
			assert(guest->health == cactus_once);
	}
	assert(guest->health == cactus_once - HEALTH_PER_HEART / 2
		   && "the next cactus point lands after ten resistance decrements");

	guest->x = 12.698;
	guest->hurt_resist_time = guest->last_damage = 0;
	short epsilon_health = guest->health;
	(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_CACTUS});
	run_hazard_update(server, false);
	assert(guest->health == epsilon_health
		   && "a body whose contracted max stays before x=13 is clear");

	/* Guest cactus remains owner-local while the host is a corpse. */
	guest->x = 12.7625;
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	host->health = 0;
	enum host_respawn_phase cactus_host_phase = server->host_respawn_phase;
	bool cactus_host_ready = server->host_respawn_ready;
	server->host_respawn_phase = HOST_RESPAWN_PHASE_LOADING;
	server->host_respawn_ready = false;
	(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_CACTUS});
	run_hazard_update(server, false);
	assert(guest->health == MAX_PLAYER_HEALTH - HEALTH_PER_HEART / 2);
	assert(host->health == 0
		   && server->host_respawn_phase == HOST_RESPAWN_PHASE_LOADING
		   && !server->host_respawn_ready);
	host->health = MAX_PLAYER_HEALTH;
	server->host_respawn_phase = cactus_host_phase;
	server->host_respawn_ready = cactus_host_ready;
	guest->inventory.reply = cactus_reply;
	guest->inventory.reply_user = cactus_user;
	/* Kill this same admitted body through the ordinary cactus tail, then
	 * carry that corpse through the shipping respawn request/readiness/commit
	 * path.  The RPC carries health rather than cause; cactus is isolated as
	 * the sole damaging input here and server_local_apply_cactus_contact is the
	 * inspected PLAYER_DAMAGE_CACTUS call site. */
	struct item_data cactus_items_before[INVENTORY_SIZE];
	memcpy(cactus_items_before, guest->inventory.items,
		sizeof cactus_items_before);
	struct item_data cactus_picked_before = guest->inventory.picked_item;
	struct inventory* cactus_window_before = guest->active_inventory;
	memset(guest->inventory.items, 0, sizeof cactus_items_before);
	guest->inventory.picked_item = (struct item_data) {0};
	guest->active_inventory = &guest->inventory;
	guest->x = 12.7625;
	guest->health = HEALTH_PER_HEART / 2;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->damage_remainder = 0;
	s_cactus_zero_health = 0;
	clin_test_set_rpc_observer(count_cactus_zero_health, NULL);
	(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_CACTUS});
	double lethal_x = guest->x, lethal_y = guest->y, lethal_z = guest->z;
	short lethal_host_health = host->health;
	run_hazard_update(server, true);
	clin_test_set_rpc_observer(NULL, NULL);
	assert(guest->health == 0 && s_cactus_zero_health == 1);
	assert(guest->has_death_pos && guest->death_x == lethal_x
		&& guest->death_y == lethal_y && guest->death_z == lethal_z);
	assert(host->health == lethal_host_health
		&& server->host_respawn_phase == cactus_host_phase);
	(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});

	struct server_rpc cactus_respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = live.generation}};
	assert(svin_rpc_send_from(live, &cactus_respawn));
	server_local_test_process_messages(server);
	clin_update_drain();
	assert(server->guest_respawn_phase == GUEST_RESPAWN_LOADING
		&& guest->health == 0 && server->guest_respawn_anchor);
	assert(client->local_respawn_loading
		&& client->local_respawn_epoch == server->guest_respawn_epoch);

	/* A small real client world supplies the exact target ring, clear body and
	 * solid floor required by the production readiness helper. */
	int cactus_radius_before = world_server_view_radius();
	world_set_server_view_radius(1);
	struct world cactus_ready_world;
	world_create(&cactus_ready_world);
	w_coord_t ready_cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(
		client->local_respawn_x));
	w_coord_t ready_cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(
		client->local_respawn_z));
	for(int x = ready_cx - 1; x <= ready_cx + 1; x++)
		for(int z = ready_cz - 1; z <= ready_cz + 1; z++)
			memset(dict_wsection_safe_get(cactus_ready_world.sections,
				SECTION_TO_ID(x, z)), 0, sizeof(struct world_section));
	struct world_section* ready_section = dict_wsection_get(
		cactus_ready_world.sections, SECTION_TO_ID(ready_cx, ready_cz));
	assert(ready_section);
	w_coord_t ready_floor_y = (w_coord_t)floor(
		client->local_respawn_y - PLAYER_EYE_HEIGHT) - 1;
	w_coord_t ready_floor_base = ready_floor_y & ~(CHUNK_SIZE - 1);
	w_coord_t ready_body_base = (ready_floor_y + 1) & ~(CHUNK_SIZE - 1);
	struct chunk* ready_floor = chunk_alloc();
	assert(ready_floor);
	chunk_init(ready_floor, &cactus_ready_world, ready_cx,
		ready_floor_base, ready_cz);
	ready_section->column[ready_floor_base / CHUNK_SIZE] = ready_floor;
	ready_floor->rebuild_displist = ready_floor->mesh_in_flight = false;
	struct chunk* ready_body = ready_floor;
	if(ready_body_base != ready_floor_base) {
		ready_body = chunk_alloc();
		assert(ready_body);
		chunk_init(ready_body, &cactus_ready_world, ready_cx,
			ready_body_base, ready_cz);
		ready_section->column[ready_body_base / CHUNK_SIZE] = ready_body;
		ready_body->rebuild_displist = ready_body->mesh_in_flight = false;
	}
	chunk_set_block_raw(ready_floor, W2C_COORD((w_coord_t)floor(
		client->local_respawn_x)), W2C_COORD(ready_floor_y),
		W2C_COORD((w_coord_t)floor(client->local_respawn_z)),
		(struct block_data) {.type = BLOCK_STONE});
	assert(client_session_local_respawn_readiness(client,
		&cactus_ready_world, true, false, false));
	/* The normal frame flush may first retire an already queued owner action;
	 * the following frame sends the reliable readiness ACK. */
	for(int flush = 0; flush < 3 && !client->local_respawn_awaiting_commit;
		flush++) {
		client_session_flush_local(client);
		server_local_test_process_messages(server);
	}
	clin_update_drain();
	/* The established fixture's destination is already sent, so the shipping
	 * READY handler commits synchronously during dispatch. */
	assert(server->guest_respawn_phase == GUEST_RESPAWN_IDLE
		&& guest->health == MAX_PLAYER_HEALTH && guest->finished_loading
		&& !guest->has_death_pos && !server->guest_respawn_anchor);
	short cactus_respawn_health = guest->health;
	run_hazard_update(server, false);
	assert(guest->health == cactus_respawn_health
		&& "removed cactus must not stale-hit the respawned body");
	world_destroy(&cactus_ready_world);
	world_set_server_view_radius(cactus_radius_before);
	memcpy(guest->inventory.items, cactus_items_before,
		sizeof cactus_items_before);
	guest->inventory.picked_item = cactus_picked_before;
	guest->active_inventory = cactus_window_before;
	(void)server_world_set_block(&server->worlds[0], 13, 70, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 69, 12,
		(struct block_data) {.type = BLOCK_AIR});
	(void)server_world_set_block(&server->worlds[0], 13, 68, 12,
		(struct block_data) {.type = BLOCK_AIR});
	guest->health = MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = guest->last_damage = 0;
	guest->x = 12.5;

	struct entity* zombie = server_local_spawn_monster_at(
		(vec3) {(float)guest->x + 0.6F, (float)(guest->y - 1.0),
				(float)guest->z}, MONSTER_ZOMBIE, server);
	assert(zombie);
	a3_track(zombie);
	clin_drain_discard();

	/* ---- (1) the GUEST hurt-resistance window actually CLOSES -----------
	 * Beta decrements hurtResistantTime once per living tick, per entity
	 * [740c583 EntityLiving.onEntityUpdate; oracle: mc173
	 * entity/tick_state.rs:179 decrements it for EVERY living entity]. Before
	 * this fix the ONLY decrement in the tree was `s->player.hurt_resist_
	 * time--`, so the guest window opened on its first hit and never shut:
	 * every later equal-or-weaker hit was absorbed forever. */
	short g_full = guest->health;
	server_local_hurt_player_for(server, guest, zombie);
	clin_drain_discard();
	short after_first = guest->health;
	assert(after_first < g_full);
	assert(guest->hurt_resist_time == PLAYER_HURT_RESIST_TICKS);
	assert(host->hurt_resist_time == 0);
	/* Arm the host window too, as the per-tick control: one decay per tick on
	 * BOTH bodies, host first, and an equal hit inside the window is absorbed
	 * per Beta the whole way down. */
	host->hurt_resist_time = PLAYER_HURT_RESIST_TICKS;
	for(int k = 1; k < PLAYER_HURT_RESIST_TICKS; k++) {
		server_local_tick_actor_hurt_resistance(server);
		assert(guest->hurt_resist_time == PLAYER_HURT_RESIST_TICKS - k);
		assert(host->hurt_resist_time == PLAYER_HURT_RESIST_TICKS - k);
		short before = guest->health;
		server_local_hurt_player_for(server, guest, zombie);
		clin_drain_discard();
		assert(guest->health == before
			   && "an equal hit INSIDE the window is absorbed");
		assert(guest->hurt_resist_time == PLAYER_HURT_RESIST_TICKS - k
			   && "...and an absorbed hit does not re-open it");
	}
	/* The last tick shuts it, on both bodies. */
	server_local_tick_actor_hurt_resistance(server);
	assert(guest->hurt_resist_time == 0 && host->hurt_resist_time == 0);
	/* ...and now the SAME hit lands: two hits more than the window apart both
	 * damage the guest. */
	server_local_hurt_player_for(server, guest, zombie);
	clin_drain_discard();
	assert(guest->health < after_first
		   && "the second hit, past the window, must land on the guest");
	assert(guest->hurt_resist_time == PLAYER_HURT_RESIST_TICKS);
	/* The decay saturates at zero rather than running negative. */
	for(int k = 0; k < PLAYER_HURT_RESIST_TICKS + 4; k++)
		server_local_tick_actor_hurt_resistance(server);
	assert(guest->hurt_resist_time == 0 && host->hurt_resist_time == 0);

	/* ---- (2) the KNOCKBACK shove reaches the guest BODY -----------------
	 * Beta EntityLiving.knockBack only sets MOTION; the body moves because
	 * its own integrator consumes it [740c583 EntityLiving.knockBack, called
	 * from attackEntityFrom; oracle: mc173 entity/common.rs:146
	 * update_knock_back]. Cave players are client-simulated, so the
	 * server-side decaying shove IS that integrator -- and S1-a3 ran it for
	 * the host only, which meant the guest knockback moved it zero blocks and
	 * left vel_x/vel_z permanently non-zero. */
	guest->health = MAX_PLAYER_HEALTH;
	guest->last_damage = 0;
	guest->damage_remainder = 0;
	guest->vel_x = guest->vel_z = 0.0f;
	guest->x = 12.5; guest->y = 72.0; guest->z = 12.5;
	zombie->pos[0] = (float)guest->x + 0.6F;
	zombie->pos[1] = (float)(guest->y - 1.0);
	zombie->pos[2] = (float)guest->z;
	double gx0 = guest->x, gy0 = guest->y, gz0 = guest->z;
	uint32_t seq_before = guest->correction_seq;
	server_local_hurt_player_for(server, guest, zombie);
	clin_drain_discard();
	assert(guest->health < MAX_PLAYER_HEALTH);
	/* The HIT writes motion and nothing else (knockBack, EntityLiving.java:
	 * 392-403): the body has not moved yet. damage-client-0926: the vertical
	 * half is Beta's motionY, which the guest's own client integrates
	 * (CRPC_LOCAL_PLAYER_VELOCITY; test_knockback_client_motion). */
	assert(guest->vel_x < 0.0f && "knocked AWAY from the attacker (+X of it)");
	/* G8-BLAST-VELOCITY: knockBack lifts the SERVER motionY. */
	assert(guest->motion_y > 0.0 && guest->motion_y <= (double)0.4F
		   && guest->vel_y == 0.0f);
	assert(guest->y == gy0);
	assert(guest->x == gx0 && guest->z == gz0);
	assert(host->vel_x == 0.0f && host->vel_z == 0.0f);
	double shove_y = guest->y;
	const double kvx = guest->vel_x, kvz = guest->vel_z;
	/* G8: the hit delivered X/Y/Z to the GUEST client on the guest route
	 * (Packet28 -> setVelocity, EntityTrackerEntry.java:195-198,
	 * NetClientHandler.java:165-170); its own physics integrates it. */
	server_local_tick_actor_knockback(server);
	assert(guest->x == gx0 && guest->z == gz0 && guest->y == shove_y
		   && "the server does not move the body (NetServerHandler.java:172/257)");
	assert(guest->correction_seq == seq_before
		   && "no correction token: the client stays authoritative");
	assert(guest->vel_x == kvx * (double)0.91F
		   && guest->vel_z == kvz * (double)0.91F
		   && "the server copy decays by Beta friction (feeds the next motion/2)");
	assert(host->x == 3.5 && host->y == 72.0 && host->z == 3.5);
	clin_update_drain();
	assert(client->player_sync.correction_seq == seq_before);

	/* The sample the client authors during the push is ACCEPTED (it echoes
	 * the unchanged token): Beta's handleFlying keeps the client position. */
	float shove_fall_before = guest->fall_distance;
	short shove_health_before = guest->health;
	struct server_rpc stale = {.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = gx0 - 0.2, .y = gy0, .z = gz0,
			.correction_seq = seq_before}};
	assert(svin_rpc_send_from(live, &stale));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(guest->x == gx0 - 0.2 && guest->z == gz0
		   && guest->fall_distance == shove_fall_before
		   && guest->health == shove_health_before
		   && "the client-integrated push position is accepted");
	struct server_rpc acked = stale;
	acked.payload.player_pos.x = gx0;
	acked.payload.player_pos.correction_seq = guest->correction_seq;
	assert(svin_rpc_send_from(live, &acked));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(guest->x == gx0 && guest->y == gy0 && guest->z == gz0);

	/* The OTHER half of the same rule: the per-actor input barrier
	 * (R-S1a2 F2 raised it on both bodies; this path had no reader until now)
	 * drops a guest sample whatever token it carries. */
	server_local_raise_actor_input_barriers(server);
	assert(guest->reject_pos_this_update && host->reject_pos_this_update);
	struct server_rpc barriered = acked;
	barriered.payload.player_pos.x = gx0 + 0.25;
	barriered.payload.player_pos.correction_seq = guest->correction_seq;
	assert(svin_rpc_send_from(live, &barriered));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(guest->x == gx0 && "the input barrier drops the queued sample");
	guest->reject_pos_this_update = false;
	host->reject_pos_this_update = false;
	assert(svin_rpc_send_from(live, &barriered));
	server_local_test_process_messages(server);
	clin_drain_discard();
	assert(guest->x == gx0 + 0.25
		   && "...and the identical sample lands once the barrier retires");

	/* The server copy DECAYS geometrically (Beta friction, airborne x0.91F
	 * here). G8-BLAST-VELOCITY v3 (ASTRA-V2-REVIEW (c)): Beta has NO tail
	 * cutoff -- after 400 steps the double motion is ~0.91^400 of the
	 * knockBack (far below Packet28's 1/8000), never forced to 0. */
	const double kvx0 = guest->vel_x;
	for(int k = 0; k < 400; k++) {
		server_local_tick_actor_knockback(server);
		clin_drain_discard();
	}
	assert(fabs(guest->vel_x) < 1e-15 && fabs(guest->vel_z) < 1e-15
		   && (kvx0 == 0.0 || guest->vel_x != 0.0));

	/* ---- (3) the HOST leg of the same integrator ------------------------- */
	host->vel_x = 0.25f;
	host->vel_z = 0.0f;
	host->on_ground = false; /* G5: airborne -> Beta's 0.91F */
	/* G8-BLAST-VELOCITY: the server motion steps every update now; the
	 * fixture's 400 decay steps above let this hovering body's motionY fall
	 * to terminal speed, which would drop its clip box into the terrain.
	 * Start the host leg from rest. */
	host->motion_y = 0.0;
	double hx0 = host->x;
	uint32_t hseq0 = host->correction_seq;
	server_local_tick_actor_knockback(server);
	clin_drain_discard();
	assert(host->x == hx0);                 /* G8: not moved by the server */
	assert(host->vel_x == (double)0.25f * (double)0.91F); /* ...its copy decays (G5, Beta double motion) */
	assert(host->correction_seq == hseq0);  /* ...and nothing is re-authored */
	/* v3: geometric decay, no non-Beta tail cutoff (see the guest leg). */
	for(int k = 0; k < 400; k++) {
		server_local_tick_actor_knockback(server);
		clin_drain_discard();
	}
	assert(fabs(host->vel_x) < 1e-15 && fabs(host->vel_z) < 1e-15
		   && host->vel_x != 0.0);
	host->vel_x = host->vel_z = 0.0; /* leave the fixture at rest */
	host->motion_y = guest->motion_y = 0.0;

	/* Leave the world exactly as empty as it was found. */
	a3_despawn_all(server);

	guest->health = g_health0 > 0 ? g_health0 : MAX_PLAYER_HEALTH;
	host->health = h_health0 > 0 ? h_health0 : MAX_PLAYER_HEALTH;
	guest->hurt_resist_time = host->hurt_resist_time = 0;
	guest->last_damage = host->last_damage = 0;
	guest->damage_remainder = host->damage_remainder = 0;
	guest->vel_x = guest->vel_z = host->vel_x = host->vel_z = 0.0f;
	host->x = hx; host->y = hy; host->z = hz;
	guest->x = gx; guest->y = gy; guest->z = gz;
	clin_update_drain();
	puts("PASS R-S1a3 F1/F2: the guest hurt-resistance window decays one tick"
		 " at a time and closes (two hits past it both land, an in-window hit"
		 " absorbs); guest knockback is integrated into the guest BODY,"
		 " re-authored on the guest client, and the pre-hit sample is refused"
		 " until acknowledged; the host leg of both is unchanged");
}


/* Received Join tickets must survive real client-token refusal. Delayed reply
 * capture still uses the actual client admission handler; it holds <=2 tickets.
 * The full release-queue rows are explicit storage-boundary fixtures, not a
 * claim that eight historical cleanup records are reachable in gameplay. */
static struct { uint32_t id, ticket; uint8_t result; } join_cap_reply[4];
static unsigned join_cap_reply_n, join_cap_releases;
static uint32_t join_cap_consumed_ticket[4];
static bool join_cap_drain_status, join_cap_drop_reply;
static bool join_cap_capture(uint32_t id, enum clin_capacity_result result,
	clin_capacity_ticket ticket) {
	if(id && join_cap_reply_n < 4) {
		join_cap_reply[join_cap_reply_n].id = id;
		join_cap_reply[join_cap_reply_n].ticket = ticket;
		join_cap_reply[join_cap_reply_n++].result = result;
	}
	if(join_cap_drop_reply) { join_cap_drop_reply = false; return false; }
	return true;
}
static void join_cap_producer(const struct client_rpc* call, void* user) {
	(void)user;
	/* The existing blocking status observer runs before it acquires a token,
	 * after the RELEASE attempt. Free the harmless fixture tokens there. */
	if(join_cap_drain_status && (call->type == CRPC_LOCAL_GUEST || call->type == CRPC_HUD_MESSAGE)) {
		join_cap_drain_status = false;
		clin_drain_discard();
	}
}
static void join_cap_consumer(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type != CRPC_CAPACITY_REQUEST
	   || call->payload.capacity_request.op != CLIN_CAP_OP_RELEASE) return;
	assert(call->world_generation == 0 && call->world_index == 0
		&& call->payload.capacity_request.request_id == 0);
	assert(join_cap_releases < 4);
	for(unsigned i = 0; i < join_cap_releases; i++)
		assert(join_cap_consumed_ticket[i] != call->payload.capacity_request.ticket);
	join_cap_consumed_ticket[join_cap_releases++] = call->payload.capacity_request.ticket;
}
static void join_cap_pressure(void) {
	unsigned sent = 0;
	struct client_rpc harmless = {.type = CRPC_PLAYER_SET_HEALTH};
	while(clin_try_rpc_send(&harmless)) sent++;
	assert(sent > 0 && !clin_rpc_headroom(1));
}
static void join_cap_start(struct server_local* s, uint64_t ticket) {
	struct server_rpc join = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = ticket, .action = LOCAL_GUEST_JOIN}};
	assert(svin_rpc_send(&join));
	server_local_test_process_messages(s);
	assert(s->guest_join_cap.pending);
	clin_update_drain(); /* actual RESERVE -> client admission -> capture */
	assert(join_cap_reply_n && join_cap_reply[join_cap_reply_n-1].result == CLIN_CAP_OK);
}
static void join_cap_deliver(struct server_local* s, unsigned n) {
	struct server_rpc reply = {.type = SRPC_CAPACITY_REPLY,
		.payload.capacity_reply = {.request_id = join_cap_reply[n].id,
			.result = join_cap_reply[n].result, .ticket = join_cap_reply[n].ticket}};
	assert(svin_rpc_send(&reply));
	server_local_test_process_messages(s);
}
static bool join_cap_owns(struct server_local* s, uint32_t ticket) {
	for(unsigned i = 0; i < s->transfer_cap_release_n; i++)
		if(s->transfer_cap_release_q[i] == ticket) return true;
	return s->guest_join_cap.cleanup_ticket[0] == ticket
		|| s->guest_join_cap.cleanup_ticket[1] == ticket;
}
static void join_cap_timeout(struct server_local* s) {
	for(unsigned i = 0; i <= SERVER_TRANSFER_CLIENT_WAIT_TICKS; i++)
		server_local_guest_join_capacity_tick(s);
	assert(!s->guest_join_cap.pending);
	clin_drain_discard();
}
static void test_join_capacity_release(unsigned scenario) {
	char path[256];
	test_fixture_dir("test_join_capacity_release", path, sizeof path);
	assert(MAKE_DIR(path) == 0);
	static struct server_local s;
	server_local_init(&s, false, false, false, false, false, false, false);
	string_set_str(s.level_name, path);
	assert(level_archive_create_new(&s.level, s.level_name, 12345, (vec3){4,72,4}));
	server_world_create(&s.worlds[0], s.level_name, WORLD_DIM_OVERWORLD, 12345, true);
	s.worlds[0].local = s.worlds[0].session = &s;
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = s.player.finished_loading = true;
	s.player.x = 4; s.player.y = 72; s.player.z = 4;
	s.player.health = MAX_PLAYER_HEALTH; s.player.dimension = WORLD_DIM_OVERWORLD;
	struct server_rpc_source guest;
	assert(server_local_guest_create(&s, &guest));
	assert(server_local_guest_checkpoint(&s));
	assert(server_local_guest_leave(&s, guest));
	nbt_find_by_path(s.level.data, ".Data.CaveWiiGuest.Dimension")->payload.tag_int = -1;
	assert(level_archive_save(&s.level));
	assert(svin_init());
	assert(server_local_set_split_enabled(&s, true));
	assert(clin_world_payloads_set_enabled(true));
	clin_capacity_reset();
	join_cap_reply_n = join_cap_releases = 0;
	clin_capacity_set_reply_sender(join_cap_capture);
	clin_test_set_rpc_observer(join_cap_producer, NULL);
	clin_test_set_consume_observer(join_cap_consumer, NULL);
	join_cap_drop_reply = scenario == 4;
	join_cap_start(&s, 401);
	uint32_t retired_generation = s.worlds[1].generation;
	if(scenario != 1) join_cap_timeout(&s);
	if(scenario == 4) {
		assert(!clin_capacity_release(join_cap_reply[0].ticket)); /* reply refusal auto-freed */
		assert(s.guest_join_cap.timed_out_id && !s.guest_join_cap.cleanup_ticket[0]
			&& !s.guest_join_cap.cleanup_ticket[1]);
		join_cap_start(&s, 402); /* unknown timeout never blocks a new Join */
	}
	if(scenario == 2 || scenario == 3) {
		/* Keep both requests on one live server generation: A's timeout may
		 * retire it, so re-admit before B, and ask the real client for a share
		 * of A's held generation by restoring this storage-boundary handle. */
		struct server_world* w = server_local_world_admit(&s, WORLD_DIM_NETHER);
		assert(w); w->generation = retired_generation;
		join_cap_start(&s, 402);
		for(unsigned i = 0; i < SERVER_TRANSFER_CAP_RELEASE_QUEUE; i++)
			s.transfer_cap_release_q[i] = 0x10000000u + i;
		s.transfer_cap_release_n = SERVER_TRANSFER_CAP_RELEASE_QUEUE;
	}
	join_cap_pressure();
	if(scenario == 1 || scenario == 4) {
		nbt_find_by_path(s.level.data, ".Data.CaveWiiGuest.Health")->payload.tag_short = -1;
		join_cap_drain_status = true;
	}
	join_cap_deliver(&s, scenario == 4 ? 1 : 0);
	assert(scenario != 1 || (!s.guest_join_cap.pending
		&& s.world_slot_state[1] == SERVER_WORLD_SLOT_EMPTY));
	uint64_t held_mem1 = 0, held_mem2 = 0; unsigned held_contexts = 0;
	clin_capacity_held(&held_mem1, &held_mem2, &held_contexts);
	assert(held_contexts == 1 && held_mem1 && held_mem2);
	assert(!client_ctx_registry_current(clin_world_contexts(), 1));
	printf("Join scenario %u ticket %u after refusal: held_contexts=%u MEM1=%llu MEM2=%llu pending=%u slot_state=%u\n",
		scenario, join_cap_reply[scenario == 4 ? 1 : 0].ticket, held_contexts,
		(unsigned long long)held_mem1, (unsigned long long)held_mem2,
		(unsigned)s.guest_join_cap.pending, (unsigned)s.world_slot_state[1]);
	fflush(stdout);
	assert(join_cap_owns(&s, join_cap_reply[scenario == 4 ? 1 : 0].ticket));
	if(scenario == 2 || scenario == 3) {
		join_cap_deliver(&s, 0); /* duplicate latest-timeout OK consumes no owner */
		if(scenario == 3) {
			join_cap_drain_status = true;
			join_cap_timeout(&s); /* A's known debt survives B's reset */
			assert(join_cap_owns(&s, join_cap_reply[0].ticket));
			join_cap_pressure();
		} else {
			nbt_find_by_path(s.level.data, ".Data.CaveWiiGuest.Health")->payload.tag_short = -1;
			join_cap_drain_status = true;
		}
		join_cap_deliver(&s, 1);
		assert(join_cap_owns(&s, join_cap_reply[0].ticket)
			&& join_cap_owns(&s, join_cap_reply[1].ticket));
		assert(s.guest_join_cap.cleanup_ticket[0] != s.guest_join_cap.cleanup_ticket[1]);
		clin_drain_discard();
		/* New requests are refused only for known cleanup, before resetting it. */
		uint32_t counter = s.guest_join_cap.counter;
		struct server_rpc join = {.type = SRPC_LOCAL_GUEST,
			.payload.local_guest = {.ticket = 403, .action = LOCAL_GUEST_JOIN}};
		assert(svin_rpc_send(&join)); server_local_test_process_messages(&s);
		assert(!s.guest_join_cap.pending && s.guest_join_cap.counter == counter);
		assert(join_cap_owns(&s, join_cap_reply[0].ticket)
			&& join_cap_owns(&s, join_cap_reply[1].ticket));
		/* Remove only synthetic boundary records; both real admission tickets
		 * are still live and owned in the Join cleanup slots. */
		s.transfer_cap_release_n = 0;
	}
	clin_drain_discard();
	nbt_find_by_path(s.level.data, ".Data.CaveWiiGuest.Health")->payload.tag_short = MAX_PLAYER_HEALTH;
	s.input_ack_pending = false;
	server_local_guest_join_capacity_tick(&s);
	(void)server_transfer_step(&s);
	clin_update_drain();
	assert(join_cap_releases == ((scenario == 2 || scenario == 3) ? 2u : 1u));
	for(unsigned i = scenario == 4 ? 1 : 0; i < join_cap_reply_n; i++) {
		bool consumed = false;
		for(unsigned k = 0; k < join_cap_releases; k++)
			consumed |= join_cap_consumed_ticket[k] == join_cap_reply[i].ticket;
		assert(consumed);
		assert(!clin_capacity_release(join_cap_reply[i].ticket));
	} /* consumer freed it */
	assert(!s.guest_join_cap.cleanup_ticket[0] && !s.guest_join_cap.cleanup_ticket[1]);
	assert(!s.guest_actor && s.player.health == MAX_PLAYER_HEALTH);
	server_local_guest_join_capacity_tick(&s); (void)server_transfer_step(&s);
	clin_update_drain(); assert(join_cap_releases == ((scenario == 2 || scenario == 3) ? 2u : 1u));
	/* Once actual cleanup settles, the ordinary Join succeeds and a COMMIT
	 * echo must not release its committed slot owner. */
	join_cap_reply_n = 0;
	join_cap_start(&s, 404);
	join_cap_deliver(&s, 0);
	uint32_t committed_ticket = join_cap_reply[0].ticket;
	assert(s.guest_owned && s.transfer_slot_ticket[1] == committed_ticket);
	clin_update_drain();
	assert(join_cap_reply_n == 2); join_cap_deliver(&s, 1);
	assert(s.guest_owned && s.transfer_slot_ticket[1] == committed_ticket);
	assert(server_local_guest_leave(&s, (struct server_rpc_source){
		.slot = 1, .generation = s.guest_generation}));
	clin_drain_discard();
	server_transfer_on_world_vacated(&s, 1); clin_update_drain();
	assert(!clin_capacity_release(committed_ticket));
	clin_test_set_rpc_observer(NULL, NULL); clin_test_set_consume_observer(NULL, NULL);
	clin_capacity_set_reply_sender(NULL);
	server_local_worlds_discard_secondary(&s); server_world_discard(&s.worlds[0]);
	clin_drain_discard(); assert(clin_world_payloads_set_enabled(false));
	inventory_destroy(&s.player.inventory, &s.player);
	dict_entity_clear(s.entities); level_archive_destroy(&s.level);
	string_clear(s.level_name); test_fixture_dir_cleanup(path);
	printf("PASS Join capacity release scenario %u: real refused transport, durable debt, untagged consumed RELEASE, no duplicate\n", scenario);
}

static void test_join_capacity_barrier(unsigned scenario) {
	char path[256];
	test_fixture_dir("test_join_capacity_barrier", path, sizeof path);
	assert(MAKE_DIR(path) == 0);
	static struct server_local s;
	server_local_init(&s, false, false, false, false, false, false, false);
	string_set_str(s.level_name, path);
	assert(level_archive_create_new(&s.level, s.level_name, 12345, (vec3){4,72,4}));
	server_world_create(&s.worlds[0], s.level_name, WORLD_DIM_OVERWORLD, 12345, true);
	s.worlds[0].local = s.worlds[0].session = &s;
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = s.player.finished_loading = true;
	s.player.x = 4; s.player.y = 72; s.player.z = 4;
	s.player.health = MAX_PLAYER_HEALTH; s.player.dimension = WORLD_DIM_OVERWORLD;
	struct server_rpc_source guest;
	assert(server_local_guest_create(&s, &guest));
	assert(server_local_guest_checkpoint(&s));
	assert(server_local_guest_leave(&s, guest));
	nbt_find_by_path(s.level.data, ".Data.CaveWiiGuest.Dimension")->payload.tag_int = -1;
	assert(level_archive_save(&s.level));
	assert(svin_init());
	assert(server_local_set_split_enabled(&s, true));
	assert(clin_world_payloads_set_enabled(true));
	clin_capacity_reset();

	/* Establish the real public marker/ACK epoch. Discarding its presentation
	 * is fixture setup; the recovery request below names this actual epoch. */
	assert(server_local_host_world_marker(&s));
	uint32_t epoch = s.input_ack_pending;
	assert(epoch && epoch == s.input_epoch_counter);
	clin_drain_discard();
	struct server_rpc ready = {.type = SRPC_INPUT_READY,
		.payload.input_ready = {.epoch = epoch}};
	assert(svin_rpc_send(&ready)); server_local_test_process_messages(&s);
	assert(!s.input_ack_pending && s.player.finished_loading);
	if(scenario == 2) {
		world_create(&gstate.world);
		clin_process(&(struct client_rpc){.type = CRPC_WORLD_RESET,
			.payload.world_reset = {.world_index = 0,
				.world_generation = s.worlds[0].generation}});
	}
	clin_capacity_set_reply_sender(NULL); /* actual default svin sender */
	struct server_rpc join = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 401, .action = LOCAL_GUEST_JOIN}};
	assert(svin_rpc_send(&join)); server_local_test_process_messages(&s);
	assert(s.guest_join_cap.pending);
	uint32_t request = s.guest_join_cap.request_id;
	/* A live storage failure may publish recovery before the client reaches
	 * this Join's RESERVE in the outbound FIFO. Preserve that production
	 * order: recover first, then the real default sender's capacity reply. */
	struct server_rpc recover = {.type = SRPC_STREAM_RECOVER,
		.payload.stream_recover = {.epoch = scenario == 1 ? epoch + 1 : epoch}};
	assert(svin_rpc_send(&recover));
	if(scenario == 2) server_local_test_process_messages(&s);
	clin_update_drain(); /* actual RESERVE -> actual admission -> actual svin */
	uint64_t mem1 = 0, mem2 = 0; unsigned contexts = 0;
	clin_capacity_held(&mem1, &mem2, &contexts);
	assert(contexts == 1 && mem1 && mem2);
	if(scenario == 2) {
		clin_test_set_rpc_observer(join_cap_producer, NULL);
		join_cap_pressure(); join_cap_drain_status = true;
	}
	server_local_test_process_messages(&s);
	if(scenario == 1) {
		assert(s.player.finished_loading && !s.stream_recovery_epoch
			&& !s.guest_join_cap.pending && s.guest_owned);
		uint32_t committed = s.transfer_slot_ticket[1];
		assert(committed);
		clin_update_drain(); /* actual COMMIT and its default-sender echo */
		server_local_test_process_messages(&s);
		assert(s.guest_owned && s.transfer_slot_ticket[1] == committed);
		assert(!clin_capacity_commit(committed)); /* already committed */
		assert(server_local_guest_leave(&s, (struct server_rpc_source){
			.slot = 1, .generation = s.guest_generation}));
		clin_update_drain();
		server_transfer_on_world_vacated(&s, 1); clin_update_drain();
		assert(!clin_capacity_release(committed));
		puts("PASS invalid stream recovery: ordinary Join and COMMIT echo retain slot owner");
		goto cleanup;
	}
	assert(!s.player.finished_loading && s.stream_recovery_epoch == epoch);
	printf("after real FIFO recovery: pending=%u request=%u\n", (unsigned)s.guest_join_cap.pending, request);
	assert(!s.guest_actor);
	/* Production timeout and client world-retirement consumer cannot rescue
	 * a world the Join never published a target context for. */
	clin_update_drain();
	for(unsigned i = 0; i <= SERVER_TRANSFER_CLIENT_WAIT_TICKS; i++)
		server_local_guest_join_capacity_tick(&s);
	(void)server_transfer_step(&s);
	clin_update_drain();
	clin_capacity_held(&mem1, &mem2, &contexts);
	assert(!s.guest_join_cap.pending && s.world_slot_state[1] == SERVER_WORLD_SLOT_EMPTY);
	assert(!client_ctx_registry_current(clin_world_contexts(), 1));
	printf("real FIFO recovery epoch=%u Join request=%u: held=%u MEM1=%llu MEM2=%llu slot_state=%u pending=%u\n",
		epoch, request, contexts, (unsigned long long)mem1,
		(unsigned long long)mem2, (unsigned)s.world_slot_state[1],
		(unsigned)s.guest_join_cap.pending); fflush(stdout);
	assert(contexts == 0 && "default-sender Join ticket must be released at the recovery barrier");
	assert(!s.player.finished_loading && s.stream_recovery_epoch == epoch
		&& !s.guest_actor && s.player.health == MAX_PLAYER_HEALTH);
	assert(!s.transfer_cap_release_n && !s.guest_join_cap.cleanup_ticket[0]
		&& !s.guest_join_cap.cleanup_ticket[1]);
	server_local_guest_join_capacity_tick(&s); (void)server_transfer_step(&s);
	clin_update_drain(); clin_capacity_held(&mem1, &mem2, &contexts);
	assert(contexts == 0); /* repeated cleanup cannot recreate debt */
	if(scenario == 2) {
		/* Real restream update publishes the collision center and arms its
		 * new epoch; the actual readiness fallback supplies the exact ACK. */
		clin_test_set_rpc_observer(NULL, NULL);
		s.paused = true;
		for(unsigned i = 0; i < 1000 && !s.input_ack_pending; i++)
			run_hazard_update(&s, true);
		assert(s.player.finished_loading && s.input_ack_pending);
		screen_load_world.update(&screen_load_world, 30.0f);
		clin_update_input_readiness(); server_local_test_process_messages(&s);
		assert(!s.input_ack_pending && !s.stream_recovery_epoch);
		join.payload.local_guest.ticket = 402;
		assert(svin_rpc_send(&join)); server_local_test_process_messages(&s);
		assert(s.guest_join_cap.pending); clin_update_drain();
		server_local_test_process_messages(&s);
		assert(s.guest_owned && s.transfer_slot_ticket[1]);
		uint32_t committed = s.transfer_slot_ticket[1];
		clin_update_drain(); server_local_test_process_messages(&s);
		assert(s.guest_owned && s.transfer_slot_ticket[1] == committed);
		assert(server_local_guest_leave(&s, (struct server_rpc_source){
			.slot = 1, .generation = s.guest_generation}));
		clin_update_drain(); server_transfer_on_world_vacated(&s, 1);
		clin_update_drain(); assert(!clin_capacity_release(committed));
		puts("PASS real recovery restream/readiness ACK permits saved-world Join retry and COMMIT echo");
	}
	printf("PASS real FIFO Join recovery scenario %u: rejected body and consumed ticket release\n", scenario);
cleanup:
	clin_test_set_rpc_observer(NULL, NULL); clin_capacity_set_reply_sender(NULL);
	server_local_worlds_discard_secondary(&s); server_world_discard(&s.worlds[0]);
	clin_drain_discard();
	if(scenario == 2) {
		clin_force_world_reset();
		client_ctx_registry_retire(clin_world_contexts(), 0);
		(void)client_ctx_registry_poll(clin_world_contexts());
	}
	assert(clin_world_payloads_set_enabled(false));
	if(scenario == 2) world_destroy(&gstate.world);
	inventory_destroy(&s.player.inventory, &s.player);
	dict_entity_clear(s.entities); level_archive_destroy(&s.level);
	string_clear(s.level_name); test_fixture_dir_cleanup(path);
}

/* G8-VEHICLE-SEAMS-01: provider-level SERIALIZABLE snapshot contract.
 * This fixture covers the detached serialized record, including existing
 * cart state. Production ingress is separate: test_guest_mount_intent_admitted. */
static unsigned vehicle_record_failures;
static void vehicle_record_check(bool ok, const char* label) {
	printf("%s vehicle record: %s\n", ok ? "ok  " : "FAIL", label);
	if(!ok) vehicle_record_failures++;
}
static int test_mounted_vehicle_record(unsigned kind, bool separate) {
	assert(kind < 3);
	char path[256];
	test_fixture_dir("test_mounted_vehicle_record", path, sizeof path);
	assert(MAKE_DIR(path) == 0);
	static struct server_local s;
	server_local_init(&s, false, false, false, false, false, false, false);
	string_set_str(s.level_name, path);
	assert(level_archive_create_new(&s.level, s.level_name, 12345, (vec3){4,72,4}));
	server_world_create(&s.worlds[0], s.level_name, WORLD_DIM_OVERWORLD, 12345, true);
	s.worlds[0].local = s.worlds[0].session = &s;
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = s.player.finished_loading = true;
	s.player.x = 4; s.player.y = 72; s.player.z = 4;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.dimension = WORLD_DIM_OVERWORLD;
	struct server_rpc_source source;
	assert(server_local_guest_create(&s, &source));
	struct server_player* g = s.guest_actor;
	struct server_world* w = &s.worlds[0];
	if(separate) {
		w = server_local_world_admit(&s, WORLD_DIM_NETHER); assert(w);
		/* Provider fixture injection: explicit valid committed owner/world,
		 * not a claimed completed transfer or client gameplay handshake. */
		g->world_slot = w->world_index; g->dimension = w->dimension;
	}
	g->x = 6.5; g->y = 72.25; g->z = 4.125;
	g->health = 112; g->inventory.hotbar_slot = 7;
	g->inventory.items[INVENTORY_SLOT_MAIN] = (struct item_data){.id = ITEM_DIAMOND, .count = 13};
	/* A minimal admitted runtime relation behind closed guest use ingress.
	 * These existing state seams are injection; the serializer, disk reopen,
	 * owner resolution and lifecycle release below are production operations. */
	s.guest_gameplay = true; s.guest_entity_id = server_local_gen_id(&s);
	uint32_t mount_id = server_local_gen_id(&s), host_id = server_local_gen_id(&s);
	struct entity* mount = calloc(1, sizeof(*mount));
	struct entity* host = calloc(1, sizeof(*host)); assert(mount && host);
	if(kind == 0) entity_boat(mount_id, mount, true, w);
	else if(kind == 1) entity_monster(mount_id, mount, true, w, MONSTER_PIG);
	else entity_minecart(mount_id, mount, true, w);
	entity_boat(host_id, host, true, &s.worlds[0]);
	mount->teleport(mount, (vec3){7.5F,70.0F,4.125F});
	host->teleport(host, (vec3){4,70,4});
	*dict_entity_safe_get(s.entities, mount_id) = mount;
	*dict_entity_safe_get(s.entities, host_id) = host;
	server_local_set_boat_occupancy_for(&s, host, &s.player, true);
	if(kind == 0) server_local_set_boat_occupancy_for(&s, mount, g, true);
	else if(kind == 1) {
		mount->data.monster.saddled = true;
		mount->data.monster.pig_ridden = true;
		mount->data.monster.pig_rider_id = s.guest_entity_id;
		mount->data.monster.pig_rider_actor = server_local_actor_ident(&s, g);
	} else {
		mount->data.minecart.occupied = true;
		mount->data.minecart.occupant_id = s.guest_entity_id;
		mount->data.minecart.rider_actor = server_local_actor_ident(&s, g); /* S1 seam */
		mount->data.minecart.ride_epoch = 91;
		g->riding_minecart = true;
	}
	clin_drain_discard();
	vehicle_record_failures = 0;
	double x = g->x, y = g->y, z = g->z;
	enum world_dim dimension = g->dimension;
	/* Refusing invalid snapshot pose must retain live relation AND prior
	 * record. In this isolated provider case the prior record is absent. */
	g->x = NAN;
	vehicle_record_check(!server_local_guest_checkpoint(&s), "nonfinite authoritative pose is refused");
	vehicle_record_check(!nbt_find_by_path(s.level.data, ".Data.CaveWiiGuest"),
		"refused snapshot does not install a partial player record");
	g->x = x;
	bool saved = server_local_guest_checkpoint(&s);
	vehicle_record_check(saved, "mounted finite owner-world pose can be captured");
	vehicle_record_check(s.guest_actor == g && g->x == x && g->y == y && g->z == z
		&& s.guest_generation == source.generation && s.player.riding_boat
		&& host->data.boat.occupied, "ordinary snapshot retains live mount and host");
	vehicle_record_check(kind == 0 ? mount->data.boat.occupied
		: kind == 1 ? mount->data.monster.pig_ridden : mount->data.minecart.occupied,
		"snapshot is detached data, not a live dismount");
	if(saved) {
		nbt_node* record = nbt_find_by_path(s.level.data, ".Data.CaveWiiGuest");
		assert(record);
		vehicle_record_check(!nbt_find_by_path(record, ".Riding")
			&& !nbt_find_by_path(record, ".ridingEntity")
			&& !nbt_find_by_path(record, ".rider_actor")
			&& !nbt_find_by_path(record, ".CaveWiiRiderGeneration"),
			"Beta player record persists no runtime mount/actor relation");
		vehicle_record_check(level_archive_save(&s.level), "snapshot writes real level.dat");
		/* Existing committed-release entry: immediately clears relation BEFORE
		 * invalidating lifetime. This also models reload after successful save. */
		server_local_guest_release_checkpointed(&s);
		vehicle_record_check(kind == 0 ? !mount->data.boat.occupied
			: kind == 1 ? !mount->data.monster.pig_ridden : !mount->data.minecart.occupied,
			"committed release retires reciprocal relation before lifetime invalidation");
		level_archive_discard(&s.level);
		assert(level_archive_create(&s.level, s.level_name));
		struct server_rpc_source next;
		bool restored = server_local_guest_create(&s, &next);
		vehicle_record_check(restored, "disk reload restores a readable detached player");
		if(restored) {
			struct server_player* p = s.guest_actor;
			vehicle_record_check(next.generation > source.generation
				&& !p->riding_boat && !p->riding_minecart
				&& p->rider_vel_x == 0 && p->rider_vel_z == 0,
				"new generation has no inherited attachment or steering");
			vehicle_record_check(p->x == x && p->y == y && p->z == z
				&& p->dimension == dimension && p->health == 112 && p->inventory.hotbar_slot == 7
				&& p->inventory.items[INVENTORY_SLOT_MAIN].id == ITEM_DIAMOND
				&& p->inventory.items[INVENTORY_SLOT_MAIN].count == 13,
				"owner-world pose/dimension/health/hotbar/inventory survive actual reopen");
			vehicle_record_check(!server_local_actor_resolve(&s,
				(struct actor_ident){.slot = ACTOR_SLOT_GUEST, .generation = source.generation}),
				"departed full actor lifetime cannot resolve on rejoin");
		}
	}
	if(s.guest_actor) server_local_guest_release_checkpointed(&s);
	vehicle_record_check(s.player.riding_boat && host->data.boat.occupied,
		"guest snapshot/reload operations leave host mounted");
	server_local_set_boat_occupancy_for(&s, host, &s.player, false);
	clin_drain_discard();
	/* Explicit teardown of injected props; no successful lifecycle is inferred
	 * from this cleanup (behavioral failures have already been recorded). */
	free(mount); free(host); dict_entity_reset(s.entities);
	server_local_worlds_discard_secondary(&s); server_world_discard(&s.worlds[0]);
	inventory_destroy(&s.player.inventory, &s.player);
	dict_entity_clear(s.entities); level_archive_destroy(&s.level);
	string_clear(s.level_name); test_fixture_dir_cleanup(path);
	printf("test_local_guest vehicle-record[%u,%u]: %s (%u failures)\n", kind,
		(unsigned)separate, vehicle_record_failures ? "FAIL" : "PASS", vehicle_record_failures);
	return vehicle_record_failures ? 1 : 0;
}

/* P01: real token pool and both production publication callers. Callback
 * hooks below only observe (or probe-and-return free tokens), never produce. */
static struct client_rpc publication_calls[16];
static unsigned publication_n, publication_free, publication_columns, publication_settles;
static void publication_observe(const struct client_rpc* call, void* user) {
	(void)user;
	assert(publication_n < 16 && publication_free);
	publication_free--;
	/* Called after acquiring this send's token. A competing/reentrant producer
	 * would consume the remaining admission and fail this exact accounting. */
	assert(clin_rpc_headroom(publication_free));
	assert(!clin_rpc_headroom(publication_free + 1));
	publication_calls[publication_n++] = *call;
}
static void publication_column_observe(const struct client_rpc* call) {
	(void)call;
	publication_columns++;
	assert(clin_rpc_headroom(publication_free));
	assert(!clin_rpc_headroom(publication_free + 1));
}
static void publication_settle_observe(const struct settle_input* in) {
	(void)in;
	publication_settles++;
}
static void publication_watch(unsigned free_tokens) {
	publication_n = publication_columns = publication_settles = 0;
	publication_free = free_tokens;
	clin_test_set_rpc_observer(publication_observe, NULL);
	clin_column_observe_publish = publication_column_observe;
	client_settle_observe = publication_settle_observe;
}
static void publication_unwatch(void) {
	clin_test_set_rpc_observer(NULL, NULL);
	clin_column_observe_publish = NULL;
	client_settle_observe = NULL;
}
static unsigned publication_pressure(struct server_local* s, unsigned leave) {
	publication_unwatch();
	clin_drain_discard();
	struct client_rpc filler = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest = {.owner = 1, .generation = s->guest_generation,
			.ticket = s->guest_ticket + 1, .result = LOCAL_GUEST_ATTACHED}};
	unsigned capacity = 0;
	while(clin_try_rpc_send_local(&filler, 1, s->guest_generation)) capacity++;
	assert(capacity == 16 && leave <= capacity);
	clin_drain_discard();
	for(unsigned i = leave; i < capacity; i++)
		assert(clin_try_rpc_send_local(&filler, 1, s->guest_generation));
	publication_watch(leave);
	return capacity;
}
static void publication_snapshot_order(unsigned count) {
	assert(publication_n >= count);
	assert(publication_calls[0].type == CRPC_WINDOW_ITEMS
		&& publication_calls[0].payload.window_items.window == WINDOWC_INVENTORY);
	assert(publication_calls[1].type == CRPC_PLAYER_HOTBAR_SLOT);
	assert(publication_calls[2].type == CRPC_INVENTORY_SLOT);
	if(count == 6) {
		assert(publication_calls[3].type == CRPC_OPEN_WINDOW);
		assert(publication_calls[4].type == CRPC_WINDOW_ITEMS);
		assert(publication_calls[5].type == CRPC_INVENTORY_SLOT);
	}
	assert(publication_columns == publication_n && !publication_settles);
}
static void publication_accept(struct server_local* s, struct server_rpc_source source) {
	struct server_rpc accept = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = s->guest_ticket, .action = LOCAL_GUEST_ACCEPT}};
	assert(svin_rpc_send_from(source, &accept));
	server_local_test_process_messages(s);
}
static void publication_prepare_respawn(struct server_local* s,
	struct server_rpc_source source) {
	struct server_player* p = s->guest_actor;
	p->health = 0;
	p->has_pos = p->finished_loading = true;
	p->has_death_pos = true;
	p->death_x = p->x; p->death_y = p->y; p->death_z = p->z;
	struct server_rpc respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = source.generation}};
	assert(svin_rpc_send_from(source, &respawn));
	server_local_test_process_messages(s);
	assert(s->guest_respawn_phase == GUEST_RESPAWN_LOADING);
	clin_drain_discard();
	w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_x));
	w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(s->guest_respawn_z));
	struct server_chunk* destination = dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(cx, cz));
	if(!destination) assert(server_world_load_chunk(&s->worlds[0], cx, cz, &destination));
	assert(destination);
	/* Fixture injection: same target-column sent-bit seam as the existing
	 * respawn tests. All commit and publication handlers remain production. */
	destination->sent = true;
}
static void publication_ready(struct server_local* s, struct server_rpc_source source) {
	struct server_rpc ready = {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.owner = 1, .ticket = s->guest_ticket,
			.generation = source.generation, .action = LOCAL_GUEST_RESPAWN_READY,
			.respawn_epoch = s->guest_respawn_epoch}};
	assert(svin_rpc_send_from(source, &ready));
	server_local_test_process_messages(s);
}
static void test_guest_publication_retry(void) {
	assert(clin_init() && svin_init());
	char path[256];
	test_fixture_dir("test_guest_publication", path, sizeof(path));
	assert(MAKE_DIR(path) == 0);
	static struct server_local s;
	memset(&s, 0, sizeof(s));
	server_local_init(&s, false, false, false, false, false, false, false);
	string_set_str(s.level_name, path);
	assert(level_archive_create_new(&s.level, s.level_name, 12345, (vec3){4,72,4}));
	server_world_create(&s.worlds[0], s.level_name, WORLD_DIM_OVERWORLD, 12345, true);
	s.worlds[0].local = s.worlds[0].session = &s;
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = s.player.finished_loading = true;
	s.player.x = 4; s.player.y = 72; s.player.z = 4;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.dimension = WORLD_DIM_OVERWORLD;
	assert(server_local_set_split_enabled(&s, true));
	clin_drain_discard();
	struct server_rpc_source source;
	assert(server_local_guest_create(&s, &source));
	s.guest_ticket = 901;
	struct server_player* p = s.guest_actor;
	p->inventory.items[INVENTORY_SLOT_MAIN] = (struct item_data){.id=ITEM_DIAMOND,.count=13};
	p->inventory.hotbar_slot = 7;
	p->inventory.picked_item = (struct item_data){.id=BLOCK_STONE,.count=4};
	assert(client_session_bind_local(&gstate.players[1], source.generation));
	gstate.players[1].local_ticket = s.guest_ticket;
	gstate.players[1].local_phase = CLIENT_GUEST_INSTALLING;
	static struct server_player host_before;
	static struct client_session client_host_before;
	memcpy(&host_before, &s.player, sizeof(host_before));
	memcpy(&client_host_before, &gstate.players[0], sizeof(client_host_before));
	struct inventory active = {0};
	assert(inventory_init(&active, &inventory_logic_crafting, &s, p, CRAFTING_SIZE, 0, 0, 0));
	active.items[CRAFTING_SLOT_INPUT] = (struct item_data){.id=BLOCK_DIRT,.count=2};
	active.picked_item = (struct item_data){.id=ITEM_DIAMOND,.count=3};
	for(unsigned views = 0; views < 2; views++) {
		p->active_inventory = views ? &active : &p->inventory;
		unsigned count = views ? 6 : 3;
		for(unsigned free_tokens = 0; free_tokens < count; free_tokens += count - 1) {
			publication_pressure(&s, free_tokens);
			assert(!server_local_guest_publish(&s, source));
			assert(!publication_n && !publication_columns && !publication_settles);
			assert(clin_rpc_headroom(free_tokens) && !clin_rpc_headroom(free_tokens + 1));
		}
		/* Invalid later view/cursor must not leak the earlier base messages. */
		clin_drain_discard(); publication_watch(16);
		if(views) active.capacity++;
		else p->inventory.picked_item.id = UINT16_MAX;
		assert(!server_local_guest_publish(&s, source) && !publication_n);
		if(views) active.capacity--;
		else p->inventory.picked_item.id = BLOCK_STONE;
		assert(server_local_guest_publish(&s, source));
		assert(publication_n == count); publication_snapshot_order(count);
		clin_update_drain();
		assert(windowc_get_latest(gstate.players[1].windows[0])->items[INVENTORY_SLOT_MAIN].count == 13);
		assert(!memcmp(&s.player, &host_before, sizeof(host_before)));
		assert(!memcmp(&gstate.players[0], &client_host_before, sizeof(client_host_before)));
	}
	publication_unwatch();
	p->active_inventory = &p->inventory;
	/* The synthetic snapshot view owns only its array, no backing/window
	 * lifecycle. Teardown must not invoke the real crafting close/drop path. */
	active.logic = NULL;
	inventory_destroy(&active, p);
	/* ACCEPT goes through the actual owned mailbox; the successful enqueue
	 * cleared client_pending, so no client re-ask drives the retry. */
	for(unsigned free_tokens = 0; free_tokens < 4; free_tokens += 3) {
		s.guest_snapshot_sent = false;
		publication_pressure(&s, free_tokens);
		publication_accept(&s, source);
		assert(s.guest_accept_publish.pending && !s.guest_snapshot_sent && !publication_n);
		server_local_guest_join_capacity_tick(&s);
		assert(s.guest_accept_publish.pending && !publication_n);
		clin_drain_discard(); publication_watch(16);
		/* Production retries intentionally follow pause/loading early returns;
		 * the mailbox owns the first attempt, then retries wait for an active
		 * update without repeating committed effects (Opus P02 ruling). */
		server_local_guest_join_capacity_tick(&s);
		assert(!s.guest_accept_publish.pending && s.guest_snapshot_sent && publication_n == 4);
		publication_snapshot_order(3);
		assert(publication_calls[3].payload.local_guest.result == LOCAL_GUEST_INSTALLED);
		clin_update_drain();
	}
	/* Invalid ACCEPT is terminal, even if the REJECTED status itself must wait. */
	s.guest_snapshot_sent = false;
	p->inventory.hotbar_slot = INVENTORY_SIZE_HOTBAR;
	publication_pressure(&s, 0); publication_accept(&s, source);
	assert(s.guest_accept_publish.pending && s.guest_accept_publish.result == LOCAL_GUEST_REJECTED);
	clin_drain_discard(); publication_watch(16);
	server_local_guest_join_capacity_tick(&s);
	assert(publication_n == 1 && publication_calls[0].payload.local_guest.result == LOCAL_GUEST_REJECTED);
	assert(!s.guest_snapshot_sent && !s.guest_accept_publish.pending);
	clin_drain_discard(); p->inventory.hotbar_slot = 7;
	/* Stale lifetime/world and transfer supersession cancel queued ACCEPT. */
	for(unsigned stale = 0; stale < 3; stale++) {
		publication_pressure(&s, 0); publication_accept(&s, source);
		assert(s.guest_accept_publish.pending);
		uint32_t generation = s.worlds[0].generation;
		if(stale == 0) s.guest_generation++;
		if(stale == 1) s.worlds[0].generation++;
		if(stale == 2) s.transfer[1].open = true;
		clin_drain_discard(); publication_watch(16);
		server_local_guest_join_capacity_tick(&s);
		assert(!s.guest_accept_publish.pending && !publication_n);
		s.guest_generation = source.generation; s.worlds[0].generation = generation;
		s.transfer[1].open = false;
	}
	/* Fresh-body effects commit once, before a temporarily refused completion. */
	s.guest_confirmed = s.guest_gameplay = true;
	p->inventory.picked_item = (struct item_data){0};
	for(unsigned scenario = 0; scenario < 7; scenario++) {
		publication_unwatch();
		publication_prepare_respawn(&s, source);
		uint64_t epoch = s.guest_respawn_epoch;
		uint32_t correction = p->correction_seq;
		publication_pressure(&s, scenario == 1 ? 5 : 0);
		publication_ready(&s, source);
		assert(p->health == MAX_PLAYER_HEALTH && !p->finished_loading);
		assert(s.guest_respawn_publish.pending && s.guest_respawn_phase == GUEST_RESPAWN_LOADING);
		assert(!publication_n && p->correction_seq == correction);
		p->ticks_existed = 19; /* would be reset by a repeated commit */
		server_local_guest_respawn_poll(&s);
		assert(p->ticks_existed == 19 && !publication_n);
		uint32_t generation = s.worlds[0].generation;
		if(scenario == 2) s.guest_generation++;
		if(scenario == 3) s.worlds[0].generation++;
		if(scenario == 4) s.guest_respawn_epoch++;
		if(scenario == 5) {
			s.transfer[1].open = s.transfer[1].hold_sent = true;
			s.transfer[1].actor_slot = ACTOR_SLOT_GUEST;
			s.transfer[1].actor_generation = source.generation;
		}
		clin_drain_discard(); publication_watch(16);
		if(scenario == 6) {
			publication_unwatch();
			assert(server_local_guest_leave(&s, source));
			assert(!s.guest_respawn_publish.pending && !s.guest_accept_publish.pending);
			assert(server_local_guest_create(&s, &source));
			s.guest_ticket = 902;
			publication_watch(16); server_local_guest_respawn_poll(&s);
			assert(!publication_n && !s.guest_respawn_publish.pending);
			break;
		}
		server_local_guest_respawn_poll(&s);
		assert(!s.guest_respawn_publish.pending && p->ticks_existed == 19);
		if(scenario < 2) {
			assert(publication_n == 6 && publication_columns == 6 && !publication_settles);
			assert(publication_calls[0].type == CRPC_PLAYER_SET_HEALTH);
			assert(publication_calls[1].type == CRPC_WINDOW_ITEMS);
			assert(publication_calls[2].type == CRPC_PLAYER_HOTBAR_SLOT);
			assert(publication_calls[3].type == CRPC_INVENTORY_SLOT);
			assert(publication_calls[4].payload.local_guest.result == LOCAL_GUEST_CORRECTION);
			assert(publication_calls[5].payload.local_guest.result == LOCAL_GUEST_GAMEPLAY_READY);
			assert(publication_calls[4].payload.local_guest.respawn_epoch == epoch
				&& publication_calls[5].payload.local_guest.respawn_epoch == epoch);
			assert(p->correction_seq == correction + 1 && s.guest_respawn_phase == GUEST_RESPAWN_IDLE);
			assert(p->finished_loading);
		} else {
			assert(!publication_n && p->correction_seq == correction);
			/* Retire only the injected obsolete fixture; no success is claimed. */
			s.guest_respawn_phase = GUEST_RESPAWN_IDLE;
			s.guest_respawn_epoch = s.guest_respawn_generation = 0;
			s.guest_respawn_anchor = s.guest_respawn_ready = false;
		}
		clin_drain_discard();
		s.guest_generation = source.generation; s.worlds[0].generation = generation;
		s.transfer[1].open = s.transfer[1].hold_sent = false;
		assert(!memcmp(&s.player, &host_before, sizeof(host_before)));
		assert(!memcmp(&gstate.players[0], &client_host_before, sizeof(client_host_before)));
	}
	publication_unwatch();
	/* Leave/replacement also clears a pending ACCEPT before the next update. */
	publication_pressure(&s, 0); publication_accept(&s, source);
	assert(s.guest_accept_publish.pending);
	clin_drain_discard(); publication_unwatch();
	server_local_guest_control(&s, (struct server_rpc_source){0},
		&(struct server_rpc){.type=SRPC_LOCAL_GUEST,.payload.local_guest={
			.ticket=s.guest_ticket,.action=LOCAL_GUEST_CANCEL}});
	assert(!s.guest_actor && !s.guest_accept_publish.pending);
	clin_drain_discard();
	assert(client_session_unbind_local(&gstate.players[1], gstate.players[1].local_generation));
	server_world_discard(&s.worlds[0]);
	inventory_destroy(&s.player.inventory, &s.player);
	dict_entity_clear(s.entities); level_archive_destroy(&s.level);
	string_clear(s.level_name); test_fixture_dir_cleanup(path);
	puts("PASS P01 publication: full/near-full atomic refusal; ACCEPT/respawn retries and ordering; stale lifetime/world/epoch/transfer/leave cancellation; exclusive producer callbacks; host isolation");
}

/* R2 body witnesses: F4R2Witness.java on the retained JVM; EntityLiving's
 * two left-to-right pitch floats, with Entity.rand's Gaussian pair primed.
 * Body lifetimes follow ServerConfigurationManager.recreatePlayerEntity;
 * dimension transfer keeps the same EntityPlayerMP instance. */
#define R2_BODY_G0 UINT64_C(0xbfd96ffffcee2251)
#define R2_BODY_G_STATE UINT64_C(0xc4e3177c3064)
#define R2_BODY_CACHE UINT64_C(0xbfe3e6ebc870a01c)
#define R2_BODY_PITCH_0 0x3f5c2847u
#define R2_BODY_STATE_0 UINT64_C(0xd9688581a7be)
#define R2_BODY_PITCH_1 0x3f808cc9u
#define R2_BODY_STATE_1 UINT64_C(0x3da6e6291fa8)
#define R2_BODY_PITCH_2 0x3f7d6c3fu
#define R2_BODY_STATE_2 UINT64_C(0xb560954872a2)
#define R2_BODY_CLOSE_STATE UINT64_C(0x447b41f687c6)
#define R2_MATH_NULL_STATE UINT64_C(0x2f40f875eb6f)
#define R2_MATH_COINCIDENT_LOOPS 24
#define R2_MATH_COINCIDENT_STATE UINT64_C(0xef57ed342b2f)
static uint64_t r2_dbits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static uint32_t r2_fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void r2_prime(struct server_local* s, struct server_player* p) {
	server_local_test_seed_body_rand(s, p, INT64_C(0x5eed0b0d));
	assert(r2_dbits(jrand_next_gaussian(&p->entity_rand.r)) == R2_BODY_G0);
	assert(p->entity_rand.r.seed == R2_BODY_G_STATE && p->entity_rand.r.have_next_gaussian);
	assert(r2_dbits(p->entity_rand.r.next_gaussian_cached) == R2_BODY_CACHE);
}
static void r2_body_equal(const struct server_body_rand* a, const struct server_body_rand* b) {
	assert(a->seeded == b->seeded && a->r.seed == b->r.seed
		&& a->r.have_next_gaussian == b->r.have_next_gaussian
		&& r2_dbits(a->r.next_gaussian_cached) == r2_dbits(b->r.next_gaussian_cached));
}
static void r2_world_equal(const struct java_random* a, const struct java_random* b) {
	assert(a->seed == b->seed && a->have_next_gaussian == b->have_next_gaussian
		&& r2_dbits(a->next_gaussian_cached) == r2_dbits(b->next_gaussian_cached));
}
static uint32_t r2_heard_pitch;
static unsigned r2_hurt_n;
static void r2_sound(const struct client_rpc* c, void* unused) {
	(void)unused;
	if(c->type == CRPC_SOUND && c->payload.sound.sound_id == SOUND_RANDOM_HURT) {
		r2_hurt_n++;
		r2_heard_pitch = r2_fbits(c->payload.sound.pitch);
	}
}
static void r2_send(struct server_local* s, struct server_rpc_source src, struct server_rpc c) {
	assert(svin_rpc_send_from(src, &c));
	server_local_test_process_messages(s);
	clin_drain_discard();
}
static void r2_control(struct server_local* s, struct server_rpc_source src, enum local_guest_action a, uint64_t ticket) {
	r2_send(s, src, (struct server_rpc){.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = ticket, .action = a,
			.owner = 1, .generation = src.generation}});
}
static struct server_chunk* r2_column(struct server_world* w, double x, double z) {
	w_coord_t cx = WCOORD_CHUNK_OFFSET((w_coord_t)floor(x));
	w_coord_t cz = WCOORD_CHUNK_OFFSET((w_coord_t)floor(z));
	struct server_chunk* c = dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz));
	if(!c)
		assert(server_world_load_chunk(w, cx, cz, &c) && c);
	return c;
}
static void r2_fresh(struct server_local* s, struct server_player* p, const struct server_body_rand* old) {
	printf("  F4.R2 fresh owner=%d seeded=%d state=%012llx old=%012llx phase=%d/%d\n",
		p == &s->player ? 0 : 1, p->entity_rand.seeded,
		(unsigned long long)p->entity_rand.r.seed, (unsigned long long)old->r.seed,
		s->host_respawn_phase, s->guest_respawn_phase);
	assert(!p->entity_rand.seeded); /* observe native commit BEFORE a test seed */
	struct java_random* r = server_local_player_rand(s, p);
	assert(p->entity_rand.seeded && r->seed != old->r.seed && !r->have_next_gaussian
		&& r2_dbits(r->next_gaussian_cached) == 0);
	/* Getter is idempotent; it must not silently reseed the same body. */
	struct server_body_rand now = p->entity_rand;
	(void)server_local_player_rand(s, p);
	r2_body_equal(&p->entity_rand, &now);
}
static void r2_transfer(struct server_local* s, struct server_player* p,
	enum world_dim dim, bool fail) {
	struct server_body_rand before = p->entity_rand;
	uint32_t id = 0;
	if(fail)
		server_transfer_test_fault(SERVER_TRANSFER_FAULT_TELEPORTER);
	assert(server_transfer_request(s, p, dim, &id) == SERVER_TRANSFER_OK);
	unsigned steps = 0;
	enum server_transfer_state state;
	for(; steps < 4000; steps++) {
		state = server_transfer_state_of(s, id);
		if(state == SERVER_TRANSFER_COMMITTED || state == SERVER_TRANSFER_ROLLED_BACK)
			break;
		for(unsigned k = 0; k < SERVER_LOCAL_WORLD_SLOTS; k++)
			if(server_local_world_live(&s->worlds[k]))
				server_world_begin_update(&s->worlds[k]);
		(void)server_transfer_step(s);
		clin_drain_discard();
		/* Client capacity/terrain are external fixture inputs. Deliver them
		 * through the real native mailbox; no transfer state is pre-set. */
		struct server_transfer_txn* t = &s->transfer[1];
		if(t->state == SERVER_TRANSFER_CAPACITY && t->request_sent)
			r2_send(s, (struct server_rpc_source){0}, (struct server_rpc){
				.type = SRPC_CAPACITY_REPLY, .payload.capacity_reply = {
					.request_id = id, .result = CLIN_CAP_OK, .ticket = 22022}});
		if(t->state == SERVER_TRANSFER_LANDING && t->landing_armed)
			r2_send(s, (struct server_rpc_source){0}, (struct server_rpc){
				.type = SRPC_LANDING_READY, .payload.landing_ready = {
					.token = id, .slot = t->dst_slot,
					.generation = s->worlds[t->dst_slot].generation}});
		r2_body_equal(&p->entity_rand, &before);
	}
	printf("  F4.R2 native guest transfer dim=%d fail=%d steps=%u state=%d\n", dim, fail, steps, state);
	assert(steps < 4000 && state == (fail ? SERVER_TRANSFER_ROLLED_BACK : SERVER_TRANSFER_COMMITTED));
	if(!fail && s->a2w_guest_presentation) {
		/* A later logical update releases the per-update pose barrier. The
		 * native echoed correction, not a test write to the presentation flag,
		 * acknowledges the installed destination before another transfer. */
		p->reject_pos_this_update = false;
		r2_send(s, (struct server_rpc_source){.slot=1, .generation=s->guest_generation},
			(struct server_rpc){.type=SRPC_PLAYER_POS, .payload.player_pos={
				.x=p->x, .y=p->y, .z=p->z, .rx=p->rx, .ry=p->ry,
				.correction_seq=p->correction_seq}});
		assert(!s->a2w_guest_presentation);
	}
	r2_body_equal(&p->entity_rand, &before);
}

static void test_r2_native_rng(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(clin_init() && svin_init());
	char path[256]; test_fixture_dir("test_r2_native_rng", path, sizeof path);
	assert(MAKE_DIR(path) == 0);
	static struct server_local s;
	memset(&s, 0, sizeof s);
	server_local_init(&s, false, false, false, false, false, false, false);
	string_set_str(s.level_name, path);
	assert(level_archive_create_new(&s.level, s.level_name, 12345, (vec3){4,72,4}));
	assert(level_archive_save(&s.level));
	server_world_create(&s.worlds[0], s.level_name, WORLD_DIM_OVERWORLD, 12345, true);
	s.worlds[0].session = s.worlds[0].local = &s;
	s.player.has_pos = s.player.finished_loading = true;
	s.player.x = 4; s.player.y = 72; s.player.z = 4;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.active_inventory = &s.player.inventory;
	assert(server_local_set_split_enabled(&s, true)); clin_drain_discard();
	r2_prime(&s, &s.player);
	const struct server_body_rand host = s.player.entity_rand;
	struct java_random world = s.worlds[0].rand;
	const struct random_gen session = s.rand_src;
	const struct server_rpc_source primary = {0};
	r2_control(&s, primary, LOCAL_GUEST_JOIN, 2201);
	assert(s.guest_actor && !s.guest_confirmed && !s.guest_actor->entity_rand.seeded);
	struct server_player* p = s.guest_actor;
	struct server_rpc_source src = {.slot = 1, .generation = s.guest_generation};
	r2_prime(&s, p);
	const struct server_body_rand guest = p->entity_rand;
	/* Actual ACCEPT publication is refused; same request and autonomous retry
	 * retain every draw/cache. No body is recreated by a transport retry. */
	publication_pressure(&s, 0);
	publication_accept(&s, src);
	assert(s.guest_accept_publish.pending && !s.guest_snapshot_sent);
	r2_body_equal(&p->entity_rand, &guest);
	server_local_guest_join_capacity_tick(&s);
	r2_body_equal(&p->entity_rand, &guest);
	clin_drain_discard(); publication_unwatch();
	server_local_guest_join_capacity_tick(&s); clin_drain_discard();
	assert(s.guest_snapshot_sent && !s.guest_accept_publish.pending && s.guest_actor == p);
	r2_control(&s, src, LOCAL_GUEST_CONFIRM, 2201);
	struct server_chunk* home = r2_column(&s.worlds[0], p->x, p->z);
	home->sent = false;
	for(int i=0;i<3;i++) r2_control(&s, src, LOCAL_GUEST_GAMEPLAY, 2201);
	assert(!s.guest_gameplay && !p->finished_loading);
	r2_body_equal(&p->entity_rand, &guest);
	home->sent = true;
	r2_control(&s, src, LOCAL_GUEST_GAMEPLAY, 2201);
	r2_control(&s, src, LOCAL_GUEST_GAMEPLAY, 2201);
	assert(s.guest_gameplay && p->finished_loading);
	r2_body_equal(&p->entity_rand, &guest); r2_body_equal(&s.player.entity_rand, &host);
	r2_world_equal(&s.worlds[0].rand, &world);
	assert(s.rand_src.seed == session.seed);
	/* Native host pause/resume preserves both live bodies. */
	r2_send(&s, primary, (struct server_rpc){.type=SRPC_TOGGLE_PAUSE});
	assert(s.paused);
	r2_body_equal(&p->entity_rand, &guest); r2_body_equal(&s.player.entity_rand, &host);
	r2_send(&s, primary, (struct server_rpc){.type=SRPC_TOGGLE_PAUSE});
	assert(!s.paused);
	puts("PASS F4.R2 native admission: refused ACCEPT, deferred/duplicate GAMEPLAY, pause/resume preserve full streams/cache");
	/* Exact guest fresh/absorbed/excess/fresh branch ownership, including
	 * null and coincident direction: Math.random remains a distinct stream. */
	server_local_test_seed_math_random(INT64_C(0x22f4c000));
	clin_test_set_rpc_observer(r2_sound, NULL);
	r2_hurt_n = 0;
	bool branch_accepted = false;
	assert(server_local_damage_actor_result(&s, p, 8, 0, 0, &branch_accepted) && branch_accepted);
	assert(r2_hurt_n == 1 && r2_heard_pitch == R2_BODY_PITCH_0 && p->entity_rand.r.seed == R2_BODY_STATE_0);
	struct server_body_rand first_hit = p->entity_rand;
	struct server_local_math_random_state math_first, math_later;
	server_local_math_random_capture(&math_first);
	assert(math_first.rng.seed == R2_MATH_NULL_STATE);
	assert(server_local_damage_actor_result(&s, p, 8, 0, 0, &branch_accepted) && !branch_accepted);
	assert(server_local_damage_actor_result(&s, p, 12, 0, 0, &branch_accepted) && branch_accepted);
	r2_body_equal(&p->entity_rand, &first_hit);
	server_local_math_random_capture(&math_later);
	r2_world_equal(&math_first.rng, &math_later.rng);
	assert(r2_hurt_n == 1);
	p->hurt_resist_time = 0;
	assert(server_local_damage_actor_from_attacker(&s, p, 4, true, p->x, p->z, false, &branch_accepted) && branch_accepted);
	assert(r2_hurt_n == 2 && r2_heard_pitch == R2_BODY_PITCH_1 && p->entity_rand.r.seed == R2_BODY_STATE_1
		&& p->entity_rand.r.have_next_gaussian && r2_dbits(p->entity_rand.r.next_gaussian_cached) == R2_BODY_CACHE);
	server_local_math_random_capture(&math_later);
	assert(math_later.rng.seed == R2_MATH_COINCIDENT_STATE);
	clin_drain_discard(); clin_test_set_rpc_observer(NULL, NULL);
	p->health = MAX_PLAYER_HEALTH; p->hurt_resist_time = p->last_damage = 0;
	p->vel_x = p->vel_z = 0; p->motion_y = 0; p->been_attacked = false;
	r2_prime(&s, p); /* next fixture starts at the documented primed boundary */
	puts("PASS F4.R2 guest fresh/absorbed/excess/null/coincident: exact body pitch/state/cache and independent JVM Math.random continuation");
	r2_transfer(&s, p, WORLD_DIM_NETHER, true);
	r2_transfer(&s, p, WORLD_DIM_NETHER, false);
	r2_transfer(&s, p, WORLD_DIM_OVERWORLD, false);
	assert(p == s.guest_actor && p->dimension == WORLD_DIM_OVERWORLD);
	r2_body_equal(&p->entity_rand, &guest);
	r2_body_equal(&s.player.entity_rand, &host);
	world = s.worlds[0].rand; /* portal callbacks may consume World.rand */
	r2_column(&s.worlds[0], p->x, p->z)->sent = true;
	puts("PASS F4.R2 native guest transfer: rollback and two committed legs preserve the same body stream/cache");

	/* A refused health commit restores the body's complete primed stream.
	 * This production damage entry is shared by arrows, blasts and hazards. */
	clin_test_set_rpc_observer(r2_sound, NULL);
	r2_hurt_n = 0;
	bool accepted = false;
	s.creative_mode = true;
	assert(server_local_damage_actor_result(&s, p, 8, 0, 0, &accepted) && !accepted);
	r2_body_equal(&p->entity_rand, &guest); assert(!r2_hurt_n);
	s.creative_mode = false;
	p->health = 8;
	assert(!server_local_damage_actor_result(&s, p, 8, 0, 0, &accepted) && accepted);
	assert(p->health == 0 && p->entity_rand.r.seed == R2_BODY_STATE_0
		&& p->entity_rand.r.have_next_gaussian
		&& r2_dbits(p->entity_rand.r.next_gaussian_cached) == R2_BODY_CACHE);
	assert(r2_hurt_n == 1 && r2_heard_pitch == R2_BODY_PITCH_0);
	clin_drain_discard(); clin_test_set_rpc_observer(NULL, NULL);
	struct server_body_rand dead = p->entity_rand;
	r2_body_equal(&s.player.entity_rand, &host);
	r2_world_equal(&s.worlds[0].rand, &world);
	/* PRE-COMMIT native respawn failure must restore every close-drop draw.
	 * Fail the second allocation after the first item's four body draws;
	 * the successful retry commits exactly eight floats for both inputs. */
	p->inventory.items[INVENTORY_SLOT_CRAFTING] = (struct item_data){.id=ITEM_COAL,.count=1};
	p->inventory.items[INVENTORY_SLOT_CRAFTING + 1] = (struct item_data){.id=ITEM_COAL,.count=1};
	struct server_local_math_random_state close_math;
	server_local_math_random_capture(&close_math);
	uint32_t close_id = s.next_entity_id;
	struct server_rpc respawn = {.type=SRPC_RESPAWN,
		.payload.respawn={.owner=1,.generation=src.generation}};
	server_local_test_fail_entity_alloc_after(2);
	r2_send(&s, src, respawn);
	assert(s.guest_respawn_phase == GUEST_RESPAWN_PREPARED && p->health == 0);
	r2_body_equal(&p->entity_rand, &dead);
	r2_body_equal(&s.player.entity_rand, &host);
	r2_world_equal(&s.worlds[0].rand, &world);
	server_local_math_random_capture(&math_later);
	r2_world_equal(&math_later.rng, &close_math.rng);
	assert(math_later.initialized == close_math.initialized);
	assert(s.next_entity_id == close_id);
	assert(p->inventory.items[INVENTORY_SLOT_CRAFTING].id == ITEM_COAL);
	assert(p->inventory.items[INVENTORY_SLOT_CRAFTING + 1].id == ITEM_COAL);
	uint64_t epoch = s.guest_respawn_epoch;
	r2_send(&s, src, respawn);
	assert(s.guest_respawn_phase == GUEST_RESPAWN_LOADING && s.guest_respawn_epoch == epoch);
	assert(p->entity_rand.r.seed == R2_BODY_CLOSE_STATE && p->entity_rand.r.have_next_gaussian
		&& r2_dbits(p->entity_rand.r.next_gaussian_cached) == R2_BODY_CACHE);
	dead = p->entity_rand;
	/* Loading resend and an early ACK do not reset or consume the dying body. */
	struct server_chunk* destination = r2_column(&s.worlds[0], s.guest_respawn_x, s.guest_respawn_z);
	destination->sent = false;
	r2_send(&s, src, respawn);
	struct server_rpc ready = {.type=SRPC_LOCAL_GUEST,
		.payload.local_guest={.ticket=2201,.owner=1,.generation=src.generation,
			.action=LOCAL_GUEST_RESPAWN_READY,.respawn_epoch=epoch}};
	r2_send(&s, src, ready);
	assert(s.guest_respawn_ready && p->health == 0);
	r2_body_equal(&p->entity_rand, &dead);
	destination->sent = true;
	/* POST-COMMIT publication refusal has the opposite rule: never roll back
	 * or reinitialize the fresh body while retrying the delivery. */
	publication_pressure(&s, 0);
	assert(svin_rpc_send_from(src, &ready)); server_local_test_process_messages(&s);
	assert(s.guest_respawn_publish.pending && p->health == MAX_PLAYER_HEALTH && !p->finished_loading);
	r2_fresh(&s, p, &dead);
	r2_prime(&s, p); /* fixed fixture stream AFTER native replacement proved */
	struct server_body_rand fresh = p->entity_rand;
	server_local_guest_respawn_poll(&s); r2_body_equal(&p->entity_rand, &fresh);
	clin_drain_discard(); publication_unwatch();
	r2_send(&s, src, ready);
	server_local_guest_respawn_poll(&s); clin_drain_discard();
	assert(!s.guest_respawn_publish.pending && p->finished_loading);
	r2_send(&s, src, ready); r2_send(&s, src, respawn);
	r2_body_equal(&p->entity_rand, &fresh);
	puts("PASS F4.R2 native guest lethal/respawn: exact pitch/state/cache; rollback after four draws; eight close draws once; early ACK; fresh commit; delivery retries inert");

	/* Native host respawn uses its same-world owned path while guest lives. */
	s.player.health = 8; s.player.hurt_resist_time = 0;
	assert(!server_local_damage_actor_result(&s, &s.player, 8, 0, 0, &accepted) && accepted);
	assert(s.player.entity_rand.r.seed == R2_BODY_STATE_0);
	struct server_body_rand host_dead = s.player.entity_rand;
	struct server_rpc hr = {.type=SRPC_RESPAWN};
	server_local_test_fail_next_host_respawn_plan(); r2_send(&s, primary, hr);
	assert(s.host_respawn_phase == HOST_RESPAWN_PHASE_IDLE && s.player.health == 0);
	r2_body_equal(&s.player.entity_rand, &host_dead);
	r2_send(&s, primary, hr);
	assert(s.host_respawn_phase == HOST_RESPAWN_PHASE_LOADING);
	struct server_chunk* hc = r2_column(&s.worlds[0], s.host_respawn_x, s.host_respawn_z);
	hc->sent = false; hr.payload.respawn.respawn_epoch = s.host_respawn_epoch;
	r2_send(&s, primary, hr); assert(s.player.health == 0);
	r2_body_equal(&s.player.entity_rand, &host_dead);
	hc->sent = true; r2_send(&s, primary, hr);
	assert(s.host_respawn_phase == HOST_RESPAWN_PHASE_IDLE && s.player.health == MAX_PLAYER_HEALTH);
	r2_fresh(&s, &s.player, &host_dead);
	struct server_body_rand host_fresh = s.player.entity_rand;
	r2_send(&s, primary, hr); r2_body_equal(&s.player.entity_rand, &host_fresh);
	r2_body_equal(&p->entity_rand, &fresh);
	puts("PASS F4.R2 native host respawn: plan failure/early ACK preserve corpse; exact ACK replaces body/cache; duplicates inert");

	/* Native Leave failure keeps the old lifetime, successful Leave and Join
	 * create a new body even when entity identity storage is reused. */
	uint32_t reused_id = s.guest_entity_id;
	assert(reused_id && !dict_entity_get(s.entities, reused_id));
	string_t oldpath; string_init_set(oldpath, s.level.file_name);
	string_cat_str(s.level.file_name, "/missing");
	r2_control(&s, src, LOCAL_GUEST_LEAVE, 2201);
	assert(s.guest_actor == p); r2_body_equal(&p->entity_rand, &fresh);
	string_set(s.level.file_name, oldpath); string_clear(oldpath);
	r2_control(&s, src, LOCAL_GUEST_LEAVE, 2201);
	assert(!s.guest_actor);
	r2_control(&s, primary, LOCAL_GUEST_JOIN, 2202);
	assert(s.guest_actor && s.guest_generation != src.generation && !s.guest_actor->entity_rand.seeded);
	r2_fresh(&s, s.guest_actor, &fresh);
	struct server_body_rand readmitted = s.guest_actor->entity_rand;
	r2_control(&s, primary, LOCAL_GUEST_JOIN, 2202);
	r2_body_equal(&s.guest_actor->entity_rand, &readmitted);
	struct server_rpc_source replacement = {.slot=1, .generation=s.guest_generation};
	publication_accept(&s, replacement); clin_drain_discard();
	r2_control(&s, replacement, LOCAL_GUEST_CONFIRM, 2202);
	p = s.guest_actor;
	r2_column(server_local_world_of(&s, p), p->x, p->z)->sent = true;
	/* Counter initialization is a fixture input, not a body reset. The real
	 * GAMEPLAY admission must reuse the retired numeric ID with a new generation. */
	s.next_entity_id = reused_id - 1;
	r2_control(&s, replacement, LOCAL_GUEST_GAMEPLAY, 2202);
	assert(s.guest_gameplay && s.guest_entity_id == reused_id);
	r2_body_equal(&p->entity_rand, &readmitted);
	r2_control(&s, replacement, LOCAL_GUEST_LEAVE, 2202);
	assert(!s.guest_actor);
	dict_entity_it_t remaining;
	dict_entity_it(remaining, s.entities);
	while(!dict_entity_end_p(remaining)) {
		free(dict_entity_ref(remaining)->value);
		dict_entity_next(remaining);
	}
	dict_entity_reset(s.entities);
	if(server_local_world_live(&s.worlds[1]))
		assert(server_local_world_retire(&s, &s.worlds[1]));
	server_world_discard(&s.worlds[0]);
	inventory_destroy(&s.player.inventory, &s.player);
	dict_entity_clear(s.entities); level_archive_destroy(&s.level);
	string_clear(s.level_name); test_fixture_dir_cleanup(path);
	puts("PASS F4.R2 native leave/re-admission: failed save preserves lifetime; new Join resets it; duplicate Join preserves it; native GAMEPLAY reuses retired ID with new generation");
}

int main(int argc, char** argv) {
	blocks_init();items_init();recipe_init();
	/* The split-screen guest BODY lives in the shared client entity store. */
	dict_entity_init(gstate.entities);
	if(getenv("CW_F4RNG_R2_NATIVE_ONLY")) {
		test_r2_native_rng(); return 0;
	}
	if(argc > 1 && !strcmp(argv[1], "publication-retry")) {
		test_guest_publication_retry(); test_r2_native_rng(); return 0;
	}
	if(argc > 1 && !strcmp(argv[1], "vehicle-record")) {
		assert(clin_init() && svin_init());
		return test_mounted_vehicle_record(argc > 2 ? (unsigned)atoi(argv[2]) : 0,
			argc > 3 && atoi(argv[3]) != 0);
	}
	if(argc > 1 && !strcmp(argv[1], "capacity-barrier")) {
		assert(clin_init());
		unsigned scenario = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
		test_join_capacity_barrier(scenario); return 0;
	}
	if(argc > 1 && !strcmp(argv[1], "capacity-release")) {
		assert(clin_init());
		unsigned scenario = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
		test_join_capacity_release(scenario); return 0;
	}
	test_owned_body_updates();
	test_guest_leave_deadline();
	test_guest_correction_flags();
	test_reply_routes();
	test_owned_gameplay_reply_routes();
	/* Reply test used artificial generations; a new client-session allocation
	 * is the explicit lifetime boundary before real provider tokens. */
	memset(&gstate.players[1],0,sizeof(gstate.players[1]));
	test_guest_publication_retry();
	test_r2_native_rng();
	test_guest_lifetime();
	test_saved_dead_guest_rejoin();
	test_two_body_placement_obstruction();
	for(unsigned scenario = 0; scenario < 3; scenario++)
		test_join_capacity_barrier(scenario);
	for(unsigned scenario = 0; scenario < 5; scenario++)
		test_join_capacity_release(scenario);
	for(unsigned kind = 0; kind < 3; kind++)
		if(test_mounted_vehicle_record(kind, false)) return 1;
	puts("test_local_guest: PASS");
	return 0;
}

