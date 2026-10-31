/* S1-b -- owner-routed gameplay actions for the local split-screen guest.
 *
 * The REAL in-game action path runs (screen_ingame.c) against the REAL
 * client->server mailbox (server_interface.c's svin queue); only the physical
 * device boundary is doubled -- player 0 through the CAVEWII_AGENT bridge,
 * which drives the genuine merged-mask input_pressed/_held/_released, and
 * player 1 through a scripted struct input_player_frame, which is exactly what
 * input_owner_latch hands the production call site in main.c.
 *
 * What it proves:
 *  (a) OWNER-0 GOLDEN. A scripted host sequence emits a byte-identical RPC
 *      stream -- type, payload fields and order, plus each message's transport
 *      source -- before and after the owner parametrization. The hash below was
 *      captured on the base SHA (75c3a33c, pre-S1-b screen_ingame.c) by
 *      compiling THIS file with -DS1B_BASE_GOLDEN, which drops every section
 *      that references a symbol S1-b adds. Re-capture it the same way.
 *  (b) owner 1 emits only {slot 1, players[1].local_generation}-sourced RPCs
 *      and mutates only players[1] state.
 *  (c) a controller disconnect with the dig held stops BLOCK_DIG at once and
 *      re-arms only after a neutral frame.
 *  (d) a generation change drops the pending action.
 *  (e) a stale-generation CRPC_PLAYER_SET_HEALTH never touches players[1] (nor
 *      players[0]); a current-generation one updates only players[1].
 *  (f) an owner 1 that is not admitted emits nothing at all.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "entity/entity.h"
#include "entity/entity_monster.h" /* the shared left-click attack handler */
#include "game/agent_input.h"
#include "game/attack_assist.h" /* critique-1028: the real per-pane pick */
#include "game/client_session.h"
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "game/gui/screen_death.h"
#include "item/inventory.h"
#include "item/item_types.h"
#include "item/items.h"
#include "item/window_container.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "network/server_interface.h"
#include "particle.h"
#include "platform/input.h"
#include "platform/input_owner.h"
#include "platform/thread.h"
#include "world.h"

#define CAVEWII_F1_DISPATCH_ONLY
#include "../source/main.c"
#undef CAVEWII_F1_DISPATCH_ONLY

/* The primary body's reserved entity key (server_local_state.h's
 * SERVER_LOCAL_PLAYER_ENTITY_ID); spelled here so the client-side test does
 * not pull in a server header. */
#define TEST_PRIMARY_ENTITY_ID 0
#define TEST_GUEST_ENTITY_ID 4242
#define TEST_TARGET_ENTITY_ID 777

#define HIT_X 4
#define HIT_Y 103
#define HIT_Z 4

/* ---- transport capture -------------------------------------------------- */

struct cap_entry {
	struct server_rpc_source src;
	struct server_rpc call;
};
static struct cap_entry cap[256];
static size_t cap_count;

/* Keep production time_diff_ms and clin_update. Replace only the clock sample
 * while exercising the Wii timebase value observed in the frozen run. */
static bool clock_override;
static ptime_t clock_now;
ptime_t __real_time_get(void);
ptime_t __wrap_time_get(void) {
	return clock_override ? clock_now : __real_time_get();
}

static void clock_set_ms(uint64_t ms) {
	clock_now.tv_sec = (time_t)(ms / 1000u);
	clock_now.tv_nsec = (long)((ms % 1000u) * 1000000u);
}

static void cap_cb(struct server_rpc_source src, struct server_rpc* call,
				   void* user) {
	(void)user;
	assert(cap_count < sizeof(cap) / sizeof(cap[0]));
	cap[cap_count].src = src;
	cap[cap_count].call = *call;
	cap_count++;
}

static void drain(void) {
	svin_rpc_flush_pending();
	svin_process_owned_messages(cap_cb, NULL, false);
}

static void cap_clear(void) {
	drain();
	cap_count = 0;
}

/* ---- golden encoding ----------------------------------------------------
 * An explicit field-by-field encoding, never a memcpy of the payload union:
 * a compound literal leaves the union's trailing bytes unspecified, so a raw
 * byte hash would be a compiler/ABI artifact rather than the message. */

static uint64_t gh; /* FNV-1a 64 */

static void gh_bytes(const void* p, size_t n) {
	const unsigned char* b = p;
	for(size_t i = 0; i < n; i++) {
		gh ^= b[i];
		gh *= 1099511628211ULL;
	}
}
static void gh_u64(uint64_t v) {
	unsigned char b[8];
	for(int i = 0; i < 8; i++)
		b[i] = (unsigned char)(v >> (8 * i));
	gh_bytes(b, sizeof b);
}
static void gh_i64(int64_t v) {
	gh_u64((uint64_t)v);
}
static void gh_item(const struct item_data* it) {
	gh_u64(it->id);
	gh_u64(it->durability);
	gh_u64(it->count);
}

static void golden_feed(const struct cap_entry* e) {
	gh_u64(e->src.slot);
	gh_u64(e->src.generation);
	gh_u64((uint64_t)e->call.type);
	switch(e->call.type) {
		case SRPC_BLOCK_PLACE:
			gh_i64(e->call.payload.block_place.x);
			gh_i64(e->call.payload.block_place.y);
			gh_i64(e->call.payload.block_place.z);
			gh_u64((uint64_t)e->call.payload.block_place.side);
			gh_item(&e->call.payload.block_place.held_item);
			gh_u64(e->call.payload.block_place.has_held_item);
			break;
		case SRPC_BLOCK_DIG:
			gh_i64(e->call.payload.block_dig.x);
			gh_i64(e->call.payload.block_dig.y);
			gh_i64(e->call.payload.block_dig.z);
			gh_u64((uint64_t)e->call.payload.block_dig.side);
			gh_u64(e->call.payload.block_dig.finished);
			break;
		case SRPC_HOTBAR_SLOT:
			gh_u64(e->call.payload.hotbar_slot.slot);
			break;
		case SRPC_DROP_ITEM:
			gh_u64(e->call.payload.drop_item.all);
			break;
		case SRPC_USE_ITEM:
			gh_u64((uint64_t)(e->call.payload.use_item.charge * 1000.0F));
			gh_item(&e->call.payload.use_item.held_item);
			gh_u64(e->call.payload.use_item.has_held_item);
			break;
		case SRPC_ENTITY_ATTACK:
			gh_u64(e->call.payload.entity_attack.entity_id);
			break;
		case SRPC_ENTITY_USE:
			gh_u64(e->call.payload.entity_use.entity_id);
			gh_u64(e->call.payload.entity_use.desired_occupied);
			break;
		default:
			/* Any unexpected verb still changes the hash through its type. */
			break;
	}
}

static uint64_t golden_of_capture(void) {
	gh = 1469598103934665603ULL;
	for(size_t i = 0; i < cap_count; i++)
		golden_feed(&cap[i]);
	return gh;
}

/* ---- world / session fixture -------------------------------------------- */

static struct entity attack_target;

static void aim_at_block(struct client_session* p) {
	p->camera_hit.hit = true;
	p->camera_hit.entity_hit = false;
	p->camera_hit.entity_id = 0;
	p->camera_hit.x = HIT_X;
	p->camera_hit.y = HIT_Y;
	p->camera_hit.z = HIT_Z;
	p->camera_hit.side = SIDE_TOP;
}

static void aim_at_air(struct client_session* p) {
	p->camera_hit.hit = false;
	p->camera_hit.entity_hit = false;
	p->camera_hit.entity_id = 0;
}

static void aim_at_entity(struct client_session* p) {
	p->camera_hit.hit = false;
	p->camera_hit.entity_hit = true;
	p->camera_hit.entity_id = TEST_TARGET_ENTITY_ID;
}

static void set_hotbar_item(struct client_session* p, uint16_t id,
							uint8_t count) {
	inventory_set_slot(windowc_get_latest(p->windows[WINDOWC_INVENTORY]),
					   inventory_get_hotbar(
						   windowc_get_latest(p->windows[WINDOWC_INVENTORY]))
						   + INVENTORY_SLOT_HOTBAR,
					   (struct item_data) {.id = id, .count = count});
}

/* ---- owner 0: the byte-exact golden ------------------------------------- */

static void host_frame(const char* script) {
	if(script)
		agent_input_feed_line(script);
	agent_input_poll();
	screen_ingame.update(&screen_ingame, 0.05F);
	drain();
}

static uint64_t owner0_golden(void) {
	struct client_session* p = &gstate.players[0];
	cap_clear();

	/* place */
	aim_at_block(p);
	set_hotbar_item(p, BLOCK_STONE, 64);
	host_frame("PLACE=1");

	/* dig: start, finish (creative makes the delay a deterministic 1 ms),
	 * release */
	gstate.config_creative = true;
	host_frame("MINE=1");
	thread_msleep(10);
	host_frame(NULL);
	host_frame("MINE=0");
	gstate.config_creative = false;

	/* hotbar both ways */
	host_frame("SLOT_RIGHT=1");
	host_frame("SLOT_LEFT=1");

	/* drop */
	host_frame("DROP=1");

	/* open-air use with a bow */
	aim_at_air(p);
	set_hotbar_item(p, ITEM_BOW, 1);
	host_frame("USE=1");

	/* attack an entity */
	aim_at_entity(p);
	host_frame("MINE=0");
	host_frame("MINE=1");
	host_frame("MINE=0");

	aim_at_block(p);
	set_hotbar_item(p, BLOCK_STONE, 64);
	return golden_of_capture();
}

/* The stream captured on the base SHA (75c3a33c) with -DS1B_BASE_GOLDEN. */
#define OWNER0_GOLDEN_HASH 0x1f50681de46486deULL

static void test_owner0_golden(void) {
	uint64_t h = owner0_golden();
	printf("owner-0 RPC-stream golden: %zu messages, hash 0x%016llx\n",
		   cap_count, (unsigned long long)h);
	for(size_t i = 0; i < cap_count; i++)
		printf("  [%zu] slot=%u gen=%llu type=%d\n", i, (unsigned)cap[i].src.slot,
			   (unsigned long long)cap[i].src.generation, (int)cap[i].call.type);
	assert(cap_count > 0);
	for(size_t i = 0; i < cap_count; i++) {
		assert(cap[i].src.slot == 0);
		assert(cap[i].src.generation == 0);
	}
#ifndef S1B_BASE_GOLDEN
	assert(h == OWNER0_GOLDEN_HASH
		   && "owner-0 RPC stream changed: the parametrization is not a pure "
			  "parameter move");
#endif
	puts("PASS owner-0 golden: the host RPC stream is unchanged, slot 0 / "
		 "generation 0 throughout");
}

#ifndef S1B_BASE_GOLDEN
/* critique-1028 eat: Beta Minecraft.clickMouse :834-838 calls sendUseItem for
 * ANY held stack when the right-click hit no block (and no place consumed it);
 * PlayerControllerMP.sendUseItem :148-153 sends Packet15Place(-1,-1,-1,255).
 * The host's USE into open air therefore emits exactly one SRPC_USE_ITEM for
 * food, a throwable and even a block item; an empty hand emits nothing; a
 * block target still places (SRPC_BLOCK_PLACE, no USE_ITEM locally: the
 * local server falls through to the item use itself), and an entity target
 * interacts (SRPC_ENTITY_USE) followed by Beta's trailing USE_ITEM
 * [Minecraft.java:799-806, 834-838]. */
static size_t count_type(enum server_rpc_type t) {
	size_t n = 0;
	for(size_t i = 0; i < cap_count; i++)
		n += cap[i].call.type == t;
	return n;
}
static void use_once(struct client_session* p) {
	(void)p;
	host_frame("USE=1");
	host_frame("USE=0");
}
static void test_no_target_use_item(void) {
	struct client_session* p = &gstate.players[0];
	static const uint16_t ids[] = {ITEM_BREAD, ITEM_SNOW_BALL, ITEM_BOW,
								   BLOCK_STONE};
	for(size_t k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
		cap_clear();
		aim_at_air(p);
		set_hotbar_item(p, ids[k], 1);
		use_once(p);
		assert(cap_count == 1 && cap[0].call.type == SRPC_USE_ITEM);
		assert(cap[0].call.payload.use_item.has_held_item
			   && cap[0].call.payload.use_item.held_item.id == ids[k]);
	}
	cap_clear();
	aim_at_air(p);
	set_hotbar_item(p, 0, 0);
	use_once(p);
	assert(count_type(SRPC_USE_ITEM) == 0 && "empty hand sends nothing");

	cap_clear();
	aim_at_block(p);
	set_hotbar_item(p, ITEM_BREAD, 1);
	use_once(p);
	assert(count_type(SRPC_BLOCK_PLACE) == 1 && count_type(SRPC_USE_ITEM) == 0);

	/* At an entity Beta's clickMouse(1) keeps var2 true after
	 * interactWithEntity, so the trailing sendUseItem still runs with the
	 * held bread [Minecraft.java:799-806, 834-838]: ENTITY_USE, then
	 * USE_ITEM (144a76094; only the empty bucket is excluded, carve-out #2). */
	cap_clear();
	aim_at_entity(p);
	use_once(p);
	assert(count_type(SRPC_ENTITY_USE) == 1 && count_type(SRPC_USE_ITEM) == 1);
	{
		size_t ie = cap_count, iu = cap_count;
		for(size_t i = 0; i < cap_count; i++) {
			if(cap[i].call.type == SRPC_ENTITY_USE && ie == cap_count)
				ie = i;
			if(cap[i].call.type == SRPC_USE_ITEM && iu == cap_count)
				iu = i;
		}
		assert(ie < iu && "ENTITY_USE precedes the trailing USE_ITEM");
		assert(cap[iu].call.payload.use_item.held_item.id == ITEM_BREAD);
	}

	aim_at_block(p);
	set_hotbar_item(p, BLOCK_STONE, 64);
	cap_clear();
	puts("PASS no-target use: bread/snowball/bow/stone into open air -> one "
		 "SRPC_USE_ITEM each; empty hand -> none; block target -> BLOCK_PLACE; "
		 "entity target -> ENTITY_USE then the trailing USE_ITEM");
}
#endif

#ifndef S1B_BASE_GOLDEN

/* ---- owner 1 ------------------------------------------------------------- */

#define GUEST_GEN 11u

static struct input_player_frame guest_frame;
static struct input_device guest_device
	= {.kind = INPUT_DEVICE_KEYBOARD, .channel = 0};

static void guest_release_all(void) {
	for(int b = 0; b < IB_COUNT; b++) {
		guest_frame.down[b] = false;
		guest_frame.pressed[b] = false;
		guest_frame.released[b] = false;
	}
}

/* Latch one scripted button set the way input_owner_latch does. */
static void guest_set(enum input_button b, bool down) {
	bool was = guest_frame.down[b];
	guest_frame.down[b] = down;
	guest_frame.pressed[b] = down && !was;
	guest_frame.released[b] = !down && was;
}

static bool guest_frame_run(bool admitted) {
	bool armed = screen_ingame_guest_action_frame(
		&gstate.players[1], guest_frame.connected ? &guest_frame : NULL,
		&guest_device, admitted);
	if(armed)
		screen_ingame_update_owned(&gstate.players[1], 1, &guest_frame, 0.05F);
	drain();
	/* Consumed edges, exactly like the next hardware poll would produce. */
	for(int b = 0; b < IB_COUNT; b++)
		guest_frame.pressed[b] = guest_frame.released[b] = false;
	return armed;
}

/* A clean owner-1 starting point: the latch forgotten and the session's own
 * dig cooldown/animation cleared (a finished dig leaves a 250 ms re-dig
 * cooldown, which would otherwise make the next sub-test's timing a race),
 * then one neutral frame to re-arm. */
static void guest_fresh_start(void) {
	screen_ingame_guest_action_reset(&gstate.players[1]);
	guest_release_all();
	guest_frame.connected = true;
	(void)guest_frame_run(true);
}

static void guest_neutral_rearm(void) {
	guest_release_all();
	guest_frame.connected = true;
	(void)guest_frame_run(true);
}

/* The owner-indexed public API the action path is built on: the same three
 * readers, resolved either from an explicitly supplied frame (what the action
 * path is called with) or from input_player_frame(player). */
static void test_owned_edge_api(void) {
	input_player_unbind(1);
	for(int b = 0; b < IB_COUNT; b++) {
		assert(!input_pressed_owned(1, (enum input_button)b));
		assert(!input_held_owned(1, (enum input_button)b));
		assert(!input_released_owned(1, (enum input_button)b));
	}
	/* A bound owner that has not been polled is still all-up -- a bind must
	 * never synthesize an edge (input_owner_bind blocks every action). */
	assert(input_player_bind(1, (struct input_device) {INPUT_DEVICE_KEYBOARD, 0}));
	assert(input_player_frame(1));
	for(int b = 0; b < IB_COUNT; b++)
		assert(!input_pressed_owned(1, (enum input_button)b)
			   && !input_held_owned(1, (enum input_button)b)
			   && !input_released_owned(1, (enum input_button)b));
	input_player_unbind(1);
	assert(!input_player_frame(1));

	struct input_player_frame f = {.connected = true};
	f.down[IB_ACTION1] = f.pressed[IB_ACTION1] = true;
	f.released[IB_ACTION2] = true;
	assert(input_frame_pressed(&f, IB_ACTION1) && input_frame_held(&f, IB_ACTION1)
		   && !input_frame_released(&f, IB_ACTION1));
	assert(input_frame_released(&f, IB_ACTION2) && !input_frame_held(&f, IB_ACTION2)
		   && !input_frame_pressed(&f, IB_ACTION2));
	assert(!input_frame_pressed(NULL, IB_ACTION1)
		   && !input_frame_held(NULL, IB_ACTION1)
		   && !input_frame_released(NULL, IB_ACTION1));
	assert(!input_frame_pressed(&f, (enum input_button)IB_COUNT)
		   && !input_frame_pressed(&f, (enum input_button)-1));
	puts("PASS owner-indexed input edges: an unowned player reads every action "
		 "up, a bind synthesizes no edge, frame reads carry pressed/held/released "
		 "with bounds and NULL guarded");
}

static void test_owner1_stream(void) {
	struct client_session* g = &gstate.players[1];
	struct client_session host_before = gstate.players[0];
	cap_clear();

	guest_neutral_rearm();
	assert(cap_count == 0);

	aim_at_block(g);
	set_hotbar_item(g, BLOCK_STONE, 64);
	guest_set(IB_ACTION2, true);
	assert(guest_frame_run(true));
	guest_set(IB_ACTION2, false);
	(void)guest_frame_run(true);

	gstate.config_creative = true;
	guest_set(IB_ACTION1, true);
	(void)guest_frame_run(true);
	thread_msleep(10);
	(void)guest_frame_run(true);
	guest_set(IB_ACTION1, false);
	(void)guest_frame_run(true);
	gstate.config_creative = false;

	guest_set(IB_SCROLL_RIGHT, true);
	(void)guest_frame_run(true);
	guest_set(IB_SCROLL_RIGHT, false);
	(void)guest_frame_run(true);
	/* Back to slot 0 -- the held stack lives there, and the drop below is
	 * gated on a non-empty hand, exactly like the host script. */
	guest_set(IB_SCROLL_LEFT, true);
	(void)guest_frame_run(true);
	guest_set(IB_SCROLL_LEFT, false);
	(void)guest_frame_run(true);

	guest_set(IB_DROP, true);
	(void)guest_frame_run(true);
	guest_set(IB_DROP, false);
	(void)guest_frame_run(true);

	aim_at_air(g);
	set_hotbar_item(g, ITEM_BOW, 1);
	guest_set(IB_ACTION2, true);
	(void)guest_frame_run(true);
	guest_set(IB_ACTION2, false);
	(void)guest_frame_run(true);

	aim_at_entity(g);
	guest_set(IB_ACTION1, true);
	(void)guest_frame_run(true);
	guest_set(IB_ACTION1, false);
	(void)guest_frame_run(true);

	drain();
	printf("owner-1 stream: %zu messages\n", cap_count);
	bool seen_place = false, seen_dig = false, seen_dig_done = false,
		 seen_hotbar = false, seen_drop = false, seen_use = false,
		 seen_attack = false;
	for(size_t i = 0; i < cap_count; i++) {
		printf("  [%zu] slot=%u gen=%llu type=%d\n", i,
			   (unsigned)cap[i].src.slot,
			   (unsigned long long)cap[i].src.generation, (int)cap[i].call.type);
		assert(cap[i].src.slot == 1);
		assert(cap[i].src.generation == GUEST_GEN);
		switch(cap[i].call.type) {
			case SRPC_BLOCK_PLACE: seen_place = true; break;
			case SRPC_BLOCK_DIG:
				if(cap[i].call.payload.block_dig.finished)
					seen_dig_done = true;
				else
					seen_dig = true;
				break;
			case SRPC_HOTBAR_SLOT: seen_hotbar = true; break;
			case SRPC_DROP_ITEM: seen_drop = true; break;
			case SRPC_USE_ITEM: seen_use = true; break;
			case SRPC_ENTITY_ATTACK:
				seen_attack = true;
				assert(cap[i].call.payload.entity_attack.entity_id
					   == TEST_TARGET_ENTITY_ID);
				break;
			default: assert(0 && "guest emitted an unexpected verb"); break;
		}
	}
	assert(seen_place && seen_dig && seen_dig_done && seen_hotbar && seen_drop
		   && seen_use && seen_attack);

	/* The host session is untouched: no digging, no animation, no hotbar
	 * move, no camera retarget. (current_screen is compared too -- a guest
	 * press must never open the host's inventory or pause.) */
	assert(!memcmp(&host_before, &gstate.players[0], sizeof host_before));
	puts("PASS owner-1 stream: every verb sourced {slot 1, guest generation}, "
		 "host session byte-identical");
}

/* The reciprocal local-player target also has no legacy onLeftClick callback.
 * The host must still emit a source-0 attack against the actual guest body. */
static void test_host_attacks_local_guest(void) {
	struct client_session* host = &gstate.players[0];
	struct entity* guest = gstate.players[1].local_player;
	assert(guest && guest->id == TEST_GUEST_ENTITY_ID);
	assert(guest->onLeftClick == NULL);
	cap_clear();
	aim_at_entity(host);
	host->camera_hit.entity_id = guest->id;
	host_frame("MINE=0");
	host_frame("MINE=1");
	host_frame("MINE=0");
	bool emitted = false;
	for(size_t i = 0; i < cap_count; i++)
		emitted |= cap[i].src.slot == 0 && cap[i].src.generation == 0
			&& cap[i].call.type == SRPC_ENTITY_ATTACK
			&& cap[i].call.payload.entity_attack.entity_id == guest->id;
	assert(emitted);
	cap_clear();
	puts("PASS host attacks actual local guest: null callback still emits source-0 RPC");
}

/* critique-1028 split PvP: the camera_hit both panes act on comes from the
 * REAL per-pane pick (attack_pick_world, the call main.c makes), not a
 * hand-set entity_hit. Before the fix raycast_entity skipped every
 * ENTITY_LOCAL_PLAYER (and id 0), so neither body could ever be selected and
 * the peer branch above never ran on hardware. P1 -> P2 and P2 -> P1 both
 * emit SRPC_ENTITY_ATTACK at the other body from the acting slot; the online
 * path (peer NULL) keeps the legacy miss. */
static struct camera pvp_camera_at(const struct entity* body, float rx) {
	struct camera c;
	memset(&c, 0, sizeof c);
	c.x = body->pos[0];
	c.y = body->pos[1];
	c.z = body->pos[2];
	c.rx = rx;
	c.ry = GLM_PI_2f; /* level */
	return c;
}

static void test_split_pvp_real_pick(void) {
	struct client_session* host = &gstate.players[0];
	struct client_session* gs = &gstate.players[1];
	struct entity* hb = host->local_player;
	struct entity* gb = gs->local_player;
	assert(hb && gb && hb->id == TEST_PRIMARY_ENTITY_ID
		   && gb->id == TEST_GUEST_ENTITY_ID);
	vec3 hpos, gpos;
	glm_vec3_copy(hb->pos, hpos);
	glm_vec3_copy(gb->pos, gpos);
	int hhealth = hb->health;
	/* Two eye positions 3 blocks apart along z at y=110 (clear of the test
	 * stone at y=103); P1 looks +z (rx 0), P2 looks -z (rx pi). */
	hb->pos[0] = gb->pos[0] = 4.5F;
	hb->pos[1] = gb->pos[1] = 110.5F; /* mid-cell: a level float ray never grazes a cell face */
	hb->pos[2] = 1.5F;
	gb->pos[2] = 4.5F;
	hb->health = MAX_PLAYER_HEALTH;
	struct camera hc = pvp_camera_at(hb, 0.0F);
	struct camera gc = pvp_camera_at(gb, GLM_PIf);

	/* Online / no peer: the legacy pick still misses the other body. */
	attack_pick_world((dict_entity_t*)world_view_entities(), hb, NULL, &hc, &gstate.world,
					  ATTACK_ASSIST_OFF, false, &host->camera_hit);
	assert(!host->camera_hit.entity_hit);

	/* P1 -> P2. */
	attack_pick_world((dict_entity_t*)world_view_entities(), hb, gb, &hc, &gstate.world,
					  ATTACK_ASSIST_OFF, false, &host->camera_hit);
	assert(host->camera_hit.entity_hit
		   && host->camera_hit.entity_id == TEST_GUEST_ENTITY_ID);
	cap_clear();
	host_frame("MINE=0");
	host_frame("MINE=1");
	host_frame("MINE=0");
	bool p1_hit_p2 = false;
	for(size_t i = 0; i < cap_count; i++) {
		assert(cap[i].call.type != SRPC_BLOCK_DIG);
		p1_hit_p2 |= cap[i].src.slot == 0
			&& cap[i].call.type == SRPC_ENTITY_ATTACK
			&& cap[i].call.payload.entity_attack.entity_id
				== TEST_GUEST_ENTITY_ID;
	}
	assert(p1_hit_p2);

	/* A block between the bodies keeps the block targeted (Beta
	 * getMouseOver clamps entity reach to the block hit). */
	world_set_block(&gstate.world, 4, 110, 3,
					(struct block_data) {.type = BLOCK_STONE}, false);
	attack_pick_world((dict_entity_t*)world_view_entities(), hb, gb, &hc,
					  &gstate.world, ATTACK_ASSIST_OFF, false,
					  &host->camera_hit);
	printf("wall pick: stone=%d hit=%d ent=%d id=%u at %d,%d,%d\n",
		   (int)world_get_block(&gstate.world, 4, 110, 3).type,
		   (int)host->camera_hit.hit, (int)host->camera_hit.entity_hit,
		   (unsigned)host->camera_hit.entity_id, host->camera_hit.x,
		   host->camera_hit.y, host->camera_hit.z);
	assert(host->camera_hit.hit && !host->camera_hit.entity_hit
		   && host->camera_hit.z == 3);
	world_set_block(&gstate.world, 4, 110, 3,
					(struct block_data) {.type = BLOCK_AIR}, false);

	/* P2 -> P1 (the host body's id is 0, which raycast_entity also skipped). */
	attack_pick_world((dict_entity_t*)world_view_entities(), gb, hb, &gc, &gstate.world,
					  ATTACK_ASSIST_OFF, false, &gs->camera_hit);
	assert(gs->camera_hit.entity_hit
		   && gs->camera_hit.entity_id == TEST_PRIMARY_ENTITY_ID);
	cap_clear();
	guest_fresh_start();
	guest_set(IB_ACTION1, true);
	(void)guest_frame_run(true);
	guest_set(IB_ACTION1, false);
	(void)guest_frame_run(true);
	bool p2_hit_p1 = false;
	for(size_t i = 0; i < cap_count; i++) {
		assert(cap[i].src.slot == 1 && cap[i].src.generation == GUEST_GEN);
		p2_hit_p1 |= cap[i].call.type == SRPC_ENTITY_ATTACK
			&& cap[i].call.payload.entity_attack.entity_id
				== TEST_PRIMARY_ENTITY_ID;
	}
	assert(p2_hit_p1);
	printf("split PvP real pick: p1_hit_p2=%d p2_hit_p1=%d\n",
		   (int)p1_hit_p2, (int)p2_hit_p1);

	cap_clear();
	aim_at_air(host);
	aim_at_air(gs);
	glm_vec3_copy(hpos, hb->pos);
	glm_vec3_copy(gpos, gb->pos);
	hb->health = hhealth;
	guest_fresh_start();
	puts("PASS split PvP: the real per-pane pick selects the peer body both "
		 "ways; NULL peer keeps the online miss");
}

static void test_disconnect_neutral_rearm(void) {
	struct client_session* g = &gstate.players[1];
	cap_clear();
	guest_fresh_start();
	aim_at_block(g);
	set_hotbar_item(g, BLOCK_STONE, 64);

	guest_set(IB_ACTION1, true);
	assert(guest_frame_run(true));
	drain();
	assert(cap_count > 0 && g->digging.active);
	cap_count = 0;

	/* The controller drops mid-dig: no further BLOCK_DIG, pending state gone. */
	guest_frame.connected = false;
	assert(!guest_frame_run(true));
	drain();
	assert(cap_count == 0);
	assert(!g->digging.active);

	/* It comes back with the button still physically down: still refused. */
	guest_frame.connected = true;
	guest_frame.down[IB_ACTION1] = true;
	assert(!guest_frame_run(true));
	drain();
	assert(cap_count == 0 && !g->digging.active);

	/* One neutral frame re-arms it. */
	guest_release_all();
	guest_frame.connected = true;
	assert(guest_frame_run(true));
	drain();
	assert(cap_count == 0 && "a neutral frame must not itself act");

	guest_set(IB_ACTION1, true);
	assert(guest_frame_run(true));
	drain();
	assert(cap_count > 0 && g->digging.active);
	guest_set(IB_ACTION1, false);
	(void)guest_frame_run(true);
	cap_clear();
	puts("PASS owner-1 disconnect: held dig stops on the drop frame, no phantom "
		 "action, neutral frame required before re-arming");
}

static void test_generation_change_drops_pending(void) {
	struct client_session* g = &gstate.players[1];
	cap_clear();
	guest_fresh_start();
	aim_at_block(g);
	guest_set(IB_ACTION1, true);
	assert(guest_frame_run(true));
	drain();
	assert(g->digging.active);
	cap_count = 0;

	/* A rejoin issues a new generation on the same device. */
	g->local_generation = GUEST_GEN + 1;
	assert(!guest_frame_run(true));
	drain();
	assert(cap_count == 0 && !g->digging.active);

	/* Still refused while the button is held; a neutral frame re-arms. */
	assert(!guest_frame_run(true));
	guest_release_all();
	guest_frame.connected = true;
	assert(guest_frame_run(true));
	drain();
	assert(cap_count == 0);
	/* Restoring the generation is itself a change: one frame drops the latch,
	 * the next re-latches, and the neutral frame after that re-arms. */
	g->local_generation = GUEST_GEN;
	guest_release_all();
	guest_frame.connected = true;
	assert(!guest_frame_run(true));
	assert(guest_frame_run(true));

	/* Same generation, DIFFERENT pad claiming owner 1: also a drop. */
	guest_set(IB_ACTION1, true);
	assert(guest_frame_run(true));
	drain();
	assert(g->digging.active);
	cap_count = 0;
	guest_device.channel = 1;
	assert(!guest_frame_run(true));
	drain();
	assert(cap_count == 0 && !g->digging.active);
	guest_device.channel = 0;
	guest_release_all();
	guest_frame.connected = true;
	(void)guest_frame_run(true);
	cap_clear();
	puts("PASS owner-1 generation/ownership change: pending dig dropped, "
		 "re-arm requires a neutral frame");
}

static void test_not_admitted_is_silent(void) {
	struct client_session* g = &gstate.players[1];
	cap_clear();
	guest_fresh_start();
	aim_at_block(g);
	guest_set(IB_ACTION1, true);
	guest_set(IB_ACTION2, true);
	guest_set(IB_DROP, true);
	assert(!guest_frame_run(false));
	drain();
	assert(cap_count == 0 && !g->digging.active);

	/* S2: a retained respawn body may own a presentation rectangle while its
	 * gameplay admission is deliberately false. Even a connected device with
	 * simultaneous action edges must remain silent. */
	guest_release_all();
	guest_frame.connected = true;
	g->local_gameplay_ready = false;
	g->local_respawn_loading = true;
	g->local_respawn_epoch = 7;
	guest_set(IB_ACTION1, true);
	guest_set(IB_ACTION2, true);
	guest_set(IB_DROP, true);
	assert(!guest_frame_run(false));
	drain();
	assert(cap_count == 0 && !g->digging.active);
	g->local_respawn_loading = false;
	g->local_respawn_epoch = 0;
	g->local_gameplay_ready = true;

	/* Unbound owner (no local join) with admission otherwise granted. */
	guest_release_all();
	guest_frame.connected = true;
	g->local_bound = false;
	assert(!guest_frame_run(true));
	guest_set(IB_ACTION2, true);
	assert(!guest_frame_run(true));
	drain();
	assert(cap_count == 0);
	g->local_bound = true;
	guest_release_all();
	guest_frame.connected = true;
	(void)guest_frame_run(true);
	cap_clear();
	puts("PASS owner-1 admission gate: an unadmitted or unbound guest emits "
		 "nothing");
}

static void test_health_reply_routing(void) {
	struct client_session* g = &gstate.players[1];
	struct entity* guest_body = g->local_player;
	struct entity* host_body = gstate.players[0].local_player;
	assert(guest_body && host_body);
	guest_body->health = MAX_PLAYER_HEALTH;
	host_body->health = MAX_PLAYER_HEALTH;

	struct client_rpc health = {
		.type = CRPC_PLAYER_SET_HEALTH,
		.payload.player_set_health = {.health = 7},
	};
	/* Stale generation: dropped outright -- neither HUD moves. */
	assert(!client_session_local_reply(g, GUEST_GEN - 1, &health));
	assert(guest_body->health == MAX_PLAYER_HEALTH);
	assert(host_body->health == MAX_PLAYER_HEALTH);
	assert(!g->local_reply_failed);

	/* Current generation: only the guest body moves. */
	assert(client_session_local_reply(g, GUEST_GEN, &health));
	assert(guest_body->health == 7);
	assert(host_body->health == MAX_PLAYER_HEALTH);

	/* A guest at zero health is refused gameplay (Astra call #8 (iv)); the
	 * production admission predicate reads exactly this field. */
	guest_body->health = 0;
	assert(!(g->local_bound && g->local_gameplay_ready && g->local_player
			 && g->local_player->health > 0));
	guest_body->health = MAX_PLAYER_HEALTH;
	puts("PASS guest health reply: stale generation never touches either HUD, "
		 "current generation updates players[1] only");
}

static size_t guest_pos_count(void) {
	size_t n=0;for(size_t k=0;k<cap_count;k++)
		if(cap[k].src.slot==1&&cap[k].call.type==SRPC_PLAYER_POS)n++;
	return n;
}
static void sample_guest_position(void) {
	cap_clear();
	clin_update();
	if(client_session_local_gameplay_active(&gstate.players[1])) {
		const struct input_player_frame neutral = {.connected = true};
		main_client_guest_tick_dispatch(&neutral);
	}
	drain();
	for(size_t k = 0; k < cap_count; k++)
		if(cap[k].src.slot == 1 && cap[k].call.type == SRPC_PLAYER_POS)
			assert(cap[k].src.generation == gstate.players[1].local_generation);
}
static void test_guest_position_lifecycle_barrier(void) {
	struct client_session* g=&gstate.players[1];
	assert(g->local_bound&&g->local_generation==GUEST_GEN&&g->local_player);
	g->local_phase=CLIENT_GUEST_ATTACHED;g->local_gameplay_ready=true;
	g->local_respawn_loading=false;g->local_reply_failed=false;
	g->local_death_leave_manual=false;g->death.active=false;
	g->local_player->health=MAX_PLAYER_HEALTH;
	sample_guest_position();assert(guest_pos_count()==1);

	g->local_player->health=0;sample_guest_position();assert(!guest_pos_count());
	g->local_player->health=MAX_PLAYER_HEALTH;g->local_respawn_loading=true;
	sample_guest_position();assert(!guest_pos_count());
	g->local_respawn_loading=false;g->death.active=true;
	sample_guest_position();assert(!guest_pos_count());
	g->death.active=false;g->local_phase=CLIENT_GUEST_LEAVING;
	sample_guest_position();assert(!guest_pos_count());
	g->local_phase=CLIENT_GUEST_FAILED;g->local_death_leave_manual=true;
	sample_guest_position();assert(!guest_pos_count());

	/* A fresh current ATTACHED/READY lifetime restores this exact route. */
	g->local_death_leave_manual=false;g->local_phase=CLIENT_GUEST_ATTACHED;
	g->local_gameplay_ready=true;g->local_player->health=MAX_PLAYER_HEALTH;
	sample_guest_position();assert(guest_pos_count()==1);
	cap_clear();
	puts("PASS guest position emitter: dead/loading/death/LEAVING/manual FAILED inert; next ATTACHED READY resumes owned POS");
}

static void test_guest_position_long_uptime_join(void) {
	const uint64_t wii_clock_ms = UINT64_C(844002925865);
	struct client_session* guest = &gstate.players[1];
	struct entity* body = guest->local_player;
	uint64_t generation = guest->local_generation;
	assert(body && guest->local_bound);
	assert(client_session_unbind_local(guest, generation));
	guest->local_player = NULL;
	clock_set_ms(wii_clock_ms);
	clock_override = true;
	assert(time_diff_ms((ptime_t){0}, clock_now) == -2105631447);
	const unsigned samples_ms[] = {0, 49, 50, 99, 100};
	for(unsigned join = 0; join < 2; join++) {
		assert(client_session_bind_local(guest, ++generation));
		guest->local_player = body;
		guest->local_phase = CLIENT_GUEST_ATTACHED;
		guest->local_gameplay_ready = true;
		for(size_t k = 0; k < sizeof(samples_ms) / sizeof(samples_ms[0]); k++) {
			unsigned elapsed = samples_ms[k];
			clock_set_ms(wii_clock_ms + join * 1000u + elapsed);
			cap_clear();
			clin_update();
			/* Zero-tick frames remain silent, each actual 20 Hz tick sends
			 * once, regardless of the Wii clock's signed elapsed value. */
			if(elapsed == 50 || elapsed == 100) {
				const struct input_player_frame neutral = {.connected = true};
				main_client_guest_tick_dispatch(&neutral);
			}
			drain();
			size_t observed = guest_pos_count();
			size_t expected = elapsed == 50 || elapsed == 100 ? 1u : 0u;
			if(observed != expected)
				fprintf(stderr, "guest POS join=%u elapsed=%u count=%zu age=%d\n",
					join, elapsed, observed,
					time_diff_ms(guest->player_sync.last_pos_update, clock_now));
			assert(observed == expected);
		}
		ptime_t sent_at = guest->player_sync.last_pos_update;
		clock_set_ms(wii_clock_ms + join * 1000u + 101u);
		assert(!client_session_bind_local(guest, generation + 1));
		assert(guest->player_sync.last_pos_update.tv_sec == sent_at.tv_sec
			   && guest->player_sync.last_pos_update.tv_nsec == sent_at.tv_nsec);
		if(join == 0) {
			guest->local_player = NULL;
			assert(client_session_unbind_local(guest, generation));
			clock_set_ms(wii_clock_ms + 1000u);
		}
	}
	clock_override = false;
	puts("PASS guest POS Wii clock join/rejoin: zero-tick frames silent, ticks at 50/100 ms send once; failed bind retains stamp");
}

/* Real-console discriminators: the host is a local-player entity, whose
 * client callback is NULL, and Wii RTC uptime can make zero-to-now
 * time_diff_ms negative.  Use the production guest action path and mailbox;
 * a swing alone is not an attack or a dig-start. */
static void test_guest_first_left_actions_at_wii_uptime(void) {
	struct client_session* guest = &gstate.players[1];
	struct entity* host = gstate.players[0].local_player;
	assert(host && host->id == TEST_PRIMARY_ENTITY_ID);
	assert(host->onLeftClick == NULL);
	assert(dict_entity_get(gstate.entities, host->id));
	clock_set_ms(UINT64_C(844002925865));
	clock_override = true;
	assert(time_diff_ms((ptime_t) {0}, clock_now) < 0);

	cap_clear();
	guest_fresh_start();
	aim_at_entity(guest);
	guest->camera_hit.entity_id = host->id;
	guest_set(IB_ACTION1, true);
	assert(guest_frame_run(true));
	bool attacked_host = false;
	for(size_t i = 0; i < cap_count; i++)
		attacked_host |= cap[i].src.slot == 1
			&& cap[i].call.type == SRPC_ENTITY_ATTACK
			&& cap[i].call.payload.entity_attack.entity_id == host->id;
	guest_set(IB_ACTION1, false);
	(void)guest_frame_run(true);

	cap_clear();
	guest_fresh_start(); /* cooldown is exactly zero */
	aim_at_block(guest);
	guest_set(IB_ACTION1, true);
	assert(guest_frame_run(true));
	bool started_dig = false;
	for(size_t i = 0; i < cap_count; i++)
		started_dig |= cap[i].src.slot == 1
			&& cap[i].call.type == SRPC_BLOCK_DIG
			&& !cap[i].call.payload.block_dig.finished;
	guest_set(IB_ACTION1, false);
	(void)guest_frame_run(true);
	clock_override = false;
	cap_clear();
	printf("Wii first-left actions: host_attack=%d block_dig_start=%d\n",
		(int)attacked_host, (int)started_dig);
	assert(attacked_host && started_dig);
}

/* The first-use RTC repair must not erase the ordinary 250 ms repeat gate. */
static void test_guest_dig_cooldown_at_wii_uptime(void) {
	const uint64_t base = UINT64_C(844002925865);
	struct client_session* guest = &gstate.players[1];
	clock_override = true;
	clock_set_ms(base);
	cap_clear();
	guest_fresh_start();
	aim_at_block(guest);
	gstate.config_creative = true;
	guest_set(IB_ACTION1, true);
	assert(guest_frame_run(true));
	clock_set_ms(base + 1);
	assert(guest_frame_run(true)); /* completes; cooldown = base+1 */
	assert(!guest->digging.active);
	guest_set(IB_ACTION1, false);
	(void)guest_frame_run(true);
	cap_clear();
	clock_set_ms(base + 250); /* 249 ms after completion */
	guest_set(IB_ACTION1, true);
	assert(guest_frame_run(true));
	for(size_t i = 0; i < cap_count; i++)
		assert(cap[i].call.type != SRPC_BLOCK_DIG);
	clock_set_ms(base + 251); /* exactly 250 ms after completion */
	assert(guest_frame_run(true));
	bool started = false;
	for(size_t i = 0; i < cap_count; i++)
		started |= cap[i].src.slot == 1
			&& cap[i].call.type == SRPC_BLOCK_DIG
			&& !cap[i].call.payload.block_dig.finished;
	assert(started);
	guest_set(IB_ACTION1, false);
	(void)guest_frame_run(true);
	gstate.config_creative = false;
	clock_override = false;
	cap_clear();
	puts("PASS guest dig repeat cooldown: high Wii uptime waits 249 ms, starts at 250 ms");
}

/* The guest's action update must query the world bound to view 1, not the
 * host/default view.  The same cell deliberately differs between worlds; the
 * guest-only entity ID also makes a host-dictionary lookup visibly wrong.
 * Context installation is synthetic, but the action gate, update, terrain
 * lookup and outgoing RPC path are the production implementations. */
static void test_guest_action_uses_own_world(void) {
	struct client_session* guest = &gstate.players[1];
	struct clin_world_payload extra;
	assert(client_ctx_registry_install(clin_world_contexts(), WORLD_DIM_NETHER,
		1, 200));
	assert(clin_world_payload_bind_view(1, 1));
	assert(clin_world_payload_of_slot(1, &extra));
	assert(extra.terrain != &gstate.world && extra.entities != gstate.entities);
	assert(clin_world_view_slot(1) == 1);
	world_set_block(extra.terrain, HIT_X, HIT_Y, HIT_Z,
		(struct block_data) {.type = BLOCK_STONE}, false);
	world_set_block(&gstate.world, HIT_X, HIT_Y, HIT_Z,
		(struct block_data) {.type = BLOCK_AIR}, false);
	assert(world_get_block(extra.terrain, HIT_X, HIT_Y, HIT_Z).type
		== BLOCK_STONE);
	assert(world_get_block(&gstate.world, HIT_X, HIT_Y, HIT_Z).type
		== BLOCK_AIR);

	static struct entity guest_target;
	memset(&guest_target, 0, sizeof guest_target);
	guest_target.id = TEST_TARGET_ENTITY_ID + 1;
	guest_target.onLeftClick = onLeftClick;
	struct entity* ptr = &guest_target;
	dict_entity_set_at(extra.entities, guest_target.id, ptr);
	assert(!dict_entity_get(gstate.entities, guest_target.id));
	assert(dict_entity_get(extra.entities, guest_target.id));
	assert(world_view_current() == &gstate.world);
	assert(world_view_entities() == gstate.entities);

	cap_clear();
	guest_fresh_start();
	aim_at_entity(guest);
	guest->camera_hit.entity_id = guest_target.id;
	guest_set(IB_ACTION1, true);
	assert(guest_frame_run(true));
	assert(world_view_current() == &gstate.world);
	assert(world_view_entities() == gstate.entities);
	assert(cap_count == 1 && cap[0].src.slot == 1
		&& cap[0].call.type == SRPC_ENTITY_ATTACK
		&& cap[0].call.payload.entity_attack.entity_id == guest_target.id);
	guest_set(IB_ACTION1, false);
	(void)guest_frame_run(true);

	cap_clear();
	guest_fresh_start();
	aim_at_block(guest);
	gstate.config_creative = true;
	guest_set(IB_ACTION1, true);
	assert(guest_frame_run(true));
	thread_msleep(10);
	assert(guest_frame_run(true));
	gstate.config_creative = false;
	assert(world_view_current() == &gstate.world);
	assert(world_view_entities() == gstate.entities);
	bool started = false, finished = false;
	for(size_t i = 0; i < cap_count; i++)
		if(cap[i].src.slot == 1 && cap[i].call.type == SRPC_BLOCK_DIG) {
			started |= !cap[i].call.payload.block_dig.finished;
			finished |= cap[i].call.payload.block_dig.finished;
		}
	assert(started && finished);
	guest_set(IB_ACTION1, false);
	(void)guest_frame_run(true);
	cap_clear();
	dict_entity_erase(extra.entities, guest_target.id);
	client_ctx_registry_retire(clin_world_contexts(), 1);
	puts("PASS guest action world scope: guest-only target and block, prior view restored");
}

static void test_host_exit_real_dispatch(void) {
	struct client_session* h=&gstate.players[0];
	/* WORLD_RESET owns/free()s every remaining dictionary body. This fixture's
	 * guest and attack target are static test objects, so retire their aliases
	 * before exercising the real teardown. */
	dict_entity_erase(gstate.entities,TEST_GUEST_ENTITY_ID);
	dict_entity_erase(gstate.entities,TEST_TARGET_ENTITY_ID);
	gstate.players[1].local_player=NULL;
	h->death=(struct death_ui){.active=true,.host_local_respawn=true,
		.exit_pending=true,.commit_pending=DEATH_COMMIT_EXIT};
	h->current_screen=&screen_death;
	h->host_respawn_loading=true;h->host_respawn_epoch=3;
	h->host_respawn_render_camera_frozen=true;
	h->host_respawn_render_camera=h->camera;
	struct client_rpc correction={.type=CRPC_HOST_RESPAWN,
		.payload.host_respawn={.result=HOST_RESPAWN_CORRECTION,.owner=0,
			.generation=0,.respawn_epoch=3,.x=8.5,.y=72,.z=9.5,
			.correction_seq=4,.correction_flags=PLAYER_CORRECTION_POSITION_ONLY
				|PLAYER_CORRECTION_ZERO_VEL_X|PLAYER_CORRECTION_ZERO_VEL_Y
				|PLAYER_CORRECTION_ZERO_VEL_Z|PLAYER_CORRECTION_SET_ON_GROUND}};
	clin_process(&correction);
	struct client_rpc ready=correction;
	ready.payload.host_respawn.result=HOST_RESPAWN_GAMEPLAY_READY;
	clin_process(&ready);
	assert(h->host_respawn_completed_epoch==3&&h->death.active
		&&h->death.exit_pending&&h->current_screen==&screen_death);
	struct client_rpc returned={.type=CRPC_RETURN_INGAME};
	clin_process(&returned);
	assert(h->death.active&&h->death.exit_failed&&!h->death.exit_pending
		&&h->current_screen==&screen_death);
	h->death.exit_pending=true;h->death.commit_pending=DEATH_COMMIT_EXIT;
	struct client_rpc reset={.type=CRPC_WORLD_RESET,
		.payload.world_reset={.dimension=WORLD_DIM_OVERWORLD,.local_entity=0}};
	clin_process(&reset);
	assert(h->current_screen==&screen_title&&!h->death.active);
	puts("PASS host exit dispatch: READY during exit cannot dismiss card; RETURN fails in place; RESET0 alone reaches title");
}

#endif /* !S1B_BASE_GOLDEN */

/* ---- fixture ------------------------------------------------------------- */

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	static char env[] = "CAVEWII_AGENT=1";
	putenv(env);
	assert(agent_input_armed());

	assert(clin_init());
	assert(svin_init());
	svin_set_split_accept(true);

	blocks_init();
	items_init();

	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	assert(clin_world_payloads_set_enabled(true));

	struct client_rpc reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {
			.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = TEST_PRIMARY_ENTITY_ID,
		},
	};
	clin_process(&reset);
	assert(gstate.players[0].local_player);
	assert(gstate.players[0].windows[WINDOWC_INVENTORY]);
	gstate.players[0].local_player->health = MAX_PLAYER_HEALTH;
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	gstate.players[0].current_screen = &screen_ingame;

	world_set_block(&gstate.world, HIT_X, HIT_Y, HIT_Z,
					(struct block_data) {.type = BLOCK_STONE}, false);
	assert(world_get_block(&gstate.world, HIT_X, HIT_Y, HIT_Z).type
		   == BLOCK_STONE);

	/* A punchable target: the shared mob/painting/boat/minecart left-click
	 * handler is the one thing every onLeftClick does on the wire. */
	memset(&attack_target, 0, sizeof attack_target);
	attack_target.id = TEST_TARGET_ENTITY_ID;
	attack_target.onLeftClick = onLeftClick;
	struct entity* target = &attack_target;
	dict_entity_set_at(gstate.entities, TEST_TARGET_ENTITY_ID, target);

	test_owner0_golden();
#ifndef S1B_BASE_GOLDEN
	test_no_target_use_item();
#endif

#ifndef S1B_BASE_GOLDEN
	/* The guest session through the production bind, then a body of its own. */
	assert(client_session_bind_local(&gstate.players[1], GUEST_GEN));
	gstate.players[1].local_gameplay_ready = true;
	gstate.players[1].local_phase = CLIENT_GUEST_ATTACHED;
	static struct entity guest_body;
	entity_local_player(TEST_GUEST_ENTITY_ID, &guest_body, &gstate.world);
	guest_body.pos[0] = guest_body.pos[2] = 4.5F;
	guest_body.pos[1] = 105.0F;
	guest_body.health = MAX_PLAYER_HEALTH;
	struct entity* guest_ptr = &guest_body;
	dict_entity_set_at(gstate.entities, TEST_GUEST_ENTITY_ID, guest_ptr);
	gstate.players[1].local_player = guest_ptr;
	set_hotbar_item(&gstate.players[1], BLOCK_STONE, 64);

	guest_frame.category = INPUT_CAT_GAMECUBE;
	guest_frame.connected = true;

	test_owned_edge_api();
	test_owner1_stream();
	test_host_attacks_local_guest();
	test_split_pvp_real_pick();
	test_disconnect_neutral_rearm();
	test_generation_change_drops_pending();
	test_not_admitted_is_silent();
	test_health_reply_routing();
	test_guest_position_lifecycle_barrier();
	test_guest_position_long_uptime_join();
	test_guest_first_left_actions_at_wii_uptime();
	test_guest_dig_cooldown_at_wii_uptime();
	test_guest_action_uses_own_world();
	test_host_exit_real_dispatch();
#endif

	return 0;
}
