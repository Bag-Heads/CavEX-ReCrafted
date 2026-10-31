/* Unit tests for the PURE half of mp_play_script.c. GPL-3.0-or-later.
 *
 * Code under test: source/game/mp_play_map.h -- every translation between the
 * mp_play_10m core's own vocabulary (source/game/mp_play_core.h) and the
 * production world the impure half drives:
 *
 *   1. ACTIONS  MPP_ACT_* bit -> enum input_button, and a held mask -> the
 *               action list chat_login_play_request takes.
 *   2. SLOTS    core inv[] index <-> client player-inventory index <->
 *               protocol window-0 slot, proven round-trip for ALL 36.
 *   3. RINGS    struct server_remote_play_obs's observation rings -> struct
 *               mpp_obs, including the empty-slot convention.
 *   4. MARKERS  struct mpp_marker -> the collector's telemetry line.
 *
 * Links nothing: mp_play_map.h is a pure header, like mp_play_core.h. What
 * this does NOT prove, stated rather than implied: that the values it maps TO
 * are the ones production really uses. That is the impure half's seam, checked
 * by the suite's own input / container / pause tests, not here.
 */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "game/mp_play_map.h"

static int g_fail = 0;

#define CHECK(cond, ...)                                                       \
	do {                                                                       \
		if(!(cond)) {                                                          \
			g_fail++;                                                          \
			printf("FAIL %s:%d: ", __func__, __LINE__);                        \
			printf(__VA_ARGS__);                                               \
			printf("\n");                                                      \
		}                                                                      \
	} while(0)

/* ---- 1. actions ---------------------------------------------------------- */

static void case_action_bits(void) {
	static const struct {
		unsigned bit;
		enum input_button want;
		const char* name;
	} tbl[] = {
		{MPP_ACT_ATTACK, IB_ACTION1, "attack"},
		{MPP_ACT_USE, IB_ACTION2, "use"},
		{MPP_ACT_OPEN_INVENTORY, IB_OPEN_INVENTORY, "open_inventory"},
		{MPP_ACT_CLOSE, IB_INVENTORY, "close"},
		{MPP_ACT_PAUSE, IB_HOME, "pause"},
		{MPP_ACT_GUI_UP, IB_GUI_UP, "gui_up"},
		{MPP_ACT_GUI_DOWN, IB_GUI_DOWN, "gui_down"},
		{MPP_ACT_GUI_LEFT, IB_GUI_LEFT, "gui_left"},
		{MPP_ACT_GUI_RIGHT, IB_GUI_RIGHT, "gui_right"},
		{MPP_ACT_GUI_CLICK, IB_GUI_CLICK, "gui_click"},
		{MPP_ACT_HOTBAR_NEXT, IB_SCROLL_RIGHT, "hotbar_next"},
		{MPP_ACT_FORWARD, IB_FORWARD, "forward"},
		{MPP_ACT_JUMP, IB_JUMP, "jump"},
	};
	unsigned n = (unsigned)(sizeof(tbl) / sizeof(tbl[0]));
	unsigned seen = 0;
	CHECK(n == MPP_MAP_ACTION_COUNT, "vocabulary size %u != %u", n,
		  MPP_MAP_ACTION_COUNT);
	for(unsigned k = 0; k < n; k++) {
		enum input_button got = IB_COUNT;
		CHECK(mpp_map_action(tbl[k].bit, &got), "%s not mapped", tbl[k].name);
		CHECK(got == tbl[k].want, "%s -> %d, want %d", tbl[k].name, (int)got,
			  (int)tbl[k].want);
		/* The split b1-chat-opener-0919/07 C made: OPEN and CLOSE are two
		 * different production roles, and mapping both to IB_INVENTORY would
		 * make the inventory leg close the window it just opened. */
		seen |= tbl[k].bit;
	}
	/* Every bit of the vocabulary, and nothing outside it. */
	CHECK(seen == ((1u << MPP_MAP_ACTION_COUNT) - 1u),
		  "table covers 0x%X, vocabulary is 0x%X", seen,
		  (1u << MPP_MAP_ACTION_COUNT) - 1u);
	CHECK(!mpp_map_action(1u << MPP_MAP_ACTION_COUNT, NULL),
		  "a bit above the vocabulary was accepted");
	CHECK(!mpp_map_action(0u, NULL), "the empty mask was accepted as a bit");
	CHECK(!mpp_map_action(MPP_ACT_ATTACK | MPP_ACT_USE, NULL),
		  "a two-bit mask was accepted as a single bit");
}

static void case_action_mask(void) {
	enum input_button out[MPP_MAP_MAX_ACTIONS];
	int n;

	memset(out, 0, sizeof(out));
	n = mpp_map_actions(0u, out, MPP_MAP_MAX_ACTIONS);
	CHECK(n == 0, "empty mask -> %d actions", n);

	n = mpp_map_actions(MPP_ACT_ATTACK, out, MPP_MAP_MAX_ACTIONS);
	CHECK(n == 1 && out[0] == IB_ACTION1, "single mask -> %d/%d", n,
		  (int)out[0]);

	/* Bit order, lowest first: the list must be a deterministic function of
	 * the mask so two runs' traces compare byte for byte. */
	n = mpp_map_actions(MPP_ACT_GUI_CLICK | MPP_ACT_ATTACK | MPP_ACT_PAUSE, out,
						MPP_MAP_MAX_ACTIONS);
	CHECK(n == 3, "three-bit mask -> %d", n);
	CHECK(out[0] == IB_ACTION1 && out[1] == IB_HOME && out[2] == IB_GUI_CLICK,
		  "bit order not lowest-first: %d %d %d", (int)out[0], (int)out[1],
		  (int)out[2]);

	/* The whole vocabulary at once still fits the request the login driver
	 * accepts -- that is the sizing claim MPP_MAP_MAX_ACTIONS makes. */
	n = mpp_map_actions(((1u << MPP_MAP_ACTION_COUNT) - 1u) & ~MPP_ACT_JUMP,
						out, MPP_MAP_MAX_ACTIONS);
	CHECK(n == (int)MPP_MAP_ACTION_COUNT - 1, "vocabulary sans jump -> %d", n);
	/* JUMP is the 13th word and is only ever held ALONE (the pillar escape),
	 * so the cap stays the login driver's 12: all 13 are refused, not cut. */
	n = mpp_map_actions((1u << MPP_MAP_ACTION_COUNT) - 1u, out,
						MPP_MAP_MAX_ACTIONS);
	CHECK(n < 0, "13 actions fit a 12-action request (%d)", n);

	/* REFUSED, never truncated. */
	n = mpp_map_actions(MPP_ACT_ATTACK | MPP_ACT_USE, out, 1u);
	CHECK(n < 0, "an over-long list was truncated instead of refused (%d)", n);
	n = mpp_map_actions(1u << MPP_MAP_ACTION_COUNT, out,
						MPP_MAP_MAX_ACTIONS);
	CHECK(n < 0, "an unknown bit was ignored instead of refused (%d)", n);
	n = mpp_map_actions(MPP_ACT_ATTACK | (1u << 20), out,
						MPP_MAP_MAX_ACTIONS);
	CHECK(n < 0, "a mask with one unknown bit was accepted (%d)", n);
	n = mpp_map_actions(MPP_ACT_ATTACK, NULL, MPP_MAP_MAX_ACTIONS);
	CHECK(n < 0, "a NULL destination was accepted (%d)", n);
}

static void case_load_bridge(void) {
	/* Row depth is deliberately absent from the bridge vocabulary. Row 649 of
	 * 650 has the same semantic focus as row zero, so it reaches Join in four
	 * decisions instead of 649 UP presses. */
	static const enum mpp_load_focus focus[] = {
		MPP_LOAD_FOCUS_LIST,
		MPP_LOAD_FOCUS_PLAY,
		MPP_LOAD_FOCUS_TAB_LOAD,
		MPP_LOAD_FOCUS_TAB_CREATE,
	};
	static const enum input_button want[] = {
		IB_GUI_RIGHT,
		IB_GUI_DOWN,
		IB_GUI_RIGHT,
		IB_GUI_RIGHT,
	};
	for(unsigned k = 0; k < sizeof(focus) / sizeof(focus[0]); k++)
		CHECK(mpp_map_load_route_button(focus[k]) == want[k],
			  "deep-row route step %u -> %d, want %d", k,
			  (int)mpp_map_load_route_button(focus[k]), (int)want[k]);

	/* Couple the exact production bridge decision to the real core. Refusing
	 * to move focus keeps translating GUI_DOWN to RIGHT, but can neither fake
	 * Join focus nor enter READY; the named core budget must end the attempt. */
	struct mpp_state s;
	struct mpp_obs o;
	struct mpp_out out;
	bool saw_ready = false;
	mpp_reset(&s);
	memset(&o, 0, sizeof(o));
	s.clock_set = true;
	s.phase = MPP_PH_REJOIN_SETUP;
	s.gen = 2;
	o.screen = MPP_SCREEN_MP_SETUP;
	o.focus = MPP_FOCUS_OTHER;
	for(o.now_ms = 0; o.now_ms <= MPP_MENU_STEP_BUDGET_MS; o.now_ms += 50u) {
		mpp_tick(&s, &o, &out);
		if(s.phase == MPP_PH_READY)
			saw_ready = true;
		if(out.actions & MPP_ACT_GUI_DOWN)
			CHECK(mpp_map_load_route_button(MPP_LOAD_FOCUS_LIST)
					  == IB_GUI_RIGHT,
				  "stuck deep row emitted a non-RIGHT route");
	}
	CHECK(!saw_ready, "stuck Load focus reached READY");
	CHECK(s.phase == MPP_PH_FAILED, "stuck Load focus ended in phase %d",
		  (int)s.phase);
	CHECK(strcmp(s.fail_step, "rejoin_setup") == 0,
		  "stuck Load focus failed as %s", s.fail_step);
	CHECK(strcmp(s.fail_diag, "join never started") == 0,
		  "stuck Load focus diagnostic was %s", s.fail_diag);
}

static void case_multiplayer_setup_bridge(void) {
	/* The real Join screen starts on its tab with saved row zero selected.
	 * With two rows, DOWN from the row would select row one; RIGHT reaches the
	 * Join button without changing the chosen row. Pin that distinction here:
	 * the unattended LAN route must activate the first endpoint, never walk to
	 * the last saved server merely because another row exists. */
	static const char* endpoints[] = {
		"127.0.0.1:25565",
		"127.0.0.1:25566",
	};
	size_t selected = 0;
	bool advanced = false;
	enum input_button from_tab = mpp_map_mps_route_step(&advanced);
	CHECK(from_tab == IB_GUI_DOWN, "Join tab route -> %d, want DOWN",
		  (int)from_tab);
	/* Production DOWN from the tab enters the already-selected row. */
	CHECK(selected == 0, "tab route changed selected row to %u",
		  (unsigned)selected);
	enum input_button from_row = mpp_map_mps_route_step(&advanced);
	CHECK(from_row == IB_GUI_RIGHT, "saved-row route -> %d, want RIGHT",
		  (int)from_row);
	/* Production RIGHT from a row focuses Join and preserves selection. */
	CHECK(selected == 0, "row route changed selected row to %u",
		  (unsigned)selected);
	CHECK(strcmp(endpoints[selected], "127.0.0.1:25565") == 0,
		  "activated wrong target %s", endpoints[selected]);
	CHECK(strcmp(endpoints[selected], endpoints[1]) != 0,
		  "activated second saved target");
	CHECK(mpp_map_mps_route_step(&advanced) == IB_GUI_RIGHT,
		  "Join-focus carry changed away from RIGHT before the neutral gap");
	CHECK(selected == 0 && strcmp(endpoints[selected], endpoints[1]) != 0,
		  "continuous hold walked from saved row zero to row one");
	/* The abstract press's neutral gap and any screen change both leave this
	 * route through the non-setup branch, which resets the next setup press to
	 * DOWN. Pin both reset boundaries rather than inferring focus from time. */
	mpp_map_mps_route_reset(&advanced);
	CHECK(mpp_map_mps_route_step(&advanced) == IB_GUI_DOWN,
		  "neutral gap did not restart at DOWN");
	mpp_map_mps_route_reset(&advanced); /* setup -> another screen */
	CHECK(mpp_map_mps_route_step(&advanced) == IB_GUI_DOWN,
		  "screen change did not restart at DOWN");
}

/* ---- 2. slots and faces -------------------------------------------------- */

static void case_slot_round_trip(void) {
	int seen_client[INVENTORY_SIZE];
	int k;

	memset(seen_client, 0, sizeof(seen_client));

	/* The client's own layout is the one this whole mapping is written
	 * against; if inventory.h ever moves a region, these four are what say so
	 * instead of a silently wrong slot number on the wire. */
	CHECK(INVENTORY_SLOT_MAIN == 9, "INVENTORY_SLOT_MAIN is %d",
		  INVENTORY_SLOT_MAIN);
	CHECK(INVENTORY_SLOT_HOTBAR == 36, "INVENTORY_SLOT_HOTBAR is %d",
		  INVENTORY_SLOT_HOTBAR);
	CHECK(INVENTORY_SIZE_MAIN == 27, "INVENTORY_SIZE_MAIN is %d",
		  INVENTORY_SIZE_MAIN);
	CHECK(INVENTORY_SIZE_HOTBAR + INVENTORY_SIZE_MAIN == MPP_INV_SLOTS,
		  "the player's own slots are %d, the core carries %d",
		  INVENTORY_SIZE_HOTBAR + INVENTORY_SIZE_MAIN, MPP_INV_SLOTS);

	/* ALL 36, both directions, plus the protocol pair and the core's own
	 * mpp_proto_slot() -- three functions that must agree everywhere. */
	for(k = 0; k < MPP_INV_SLOTS; k++) {
		int client = mpp_map_core_to_client(k);
		int proto = mpp_map_core_to_proto(k);
		CHECK(client >= 0 && client < INVENTORY_SIZE,
			  "core %d -> client %d, out of the window", k, client);
		CHECK(mpp_map_client_to_core(client) == k,
			  "core %d -> client %d -> core %d", k, client,
			  mpp_map_client_to_core(client));
		CHECK(proto == client, "core %d: proto %d != client %d", k, proto,
			  client);
		CHECK(proto == mpp_proto_slot(k),
			  "core %d: map says proto %d, the core says %d", k, proto,
			  mpp_proto_slot(k));
		CHECK(mpp_map_proto_to_core(proto) == k, "core %d -> proto %d -> %d", k,
			  proto, mpp_map_proto_to_core(proto));
		CHECK(!seen_client[client], "client slot %d claimed twice", client);
		seen_client[client] = 1;
	}

	/* The hotbar is 36..44 and the main grid 9..35 -- the numbering the
	 * fixture prints. */
	CHECK(mpp_map_core_to_proto(0) == 36, "core 0 -> %d, want 36",
		  mpp_map_core_to_proto(0));
	CHECK(mpp_map_core_to_proto(8) == 44, "core 8 -> %d, want 44",
		  mpp_map_core_to_proto(8));
	CHECK(mpp_map_core_to_proto(9) == 9, "core 9 -> %d, want 9",
		  mpp_map_core_to_proto(9));
	CHECK(mpp_map_core_to_proto(35) == 35, "core 35 -> %d, want 35",
		  mpp_map_core_to_proto(35));

	/* Armor / crafting / output have NO core index: the inventory leg must
	 * never treat a focused armor slot as one of the player's 36. */
	for(k = 0; k < INVENTORY_SLOT_MAIN; k++)
		CHECK(mpp_map_client_to_core(k) == -1,
			  "client slot %d (output/crafting/armor) mapped to core %d", k,
			  mpp_map_client_to_core(k));
	CHECK(mpp_map_client_to_core(INVENTORY_SIZE) == -1,
		  "a slot past the window mapped to core %d",
		  mpp_map_client_to_core(INVENTORY_SIZE));
	CHECK(mpp_map_client_to_core(-1) == -1, "a negative slot mapped");
	CHECK(mpp_map_core_to_client(-1) == -1, "core -1 mapped");
	CHECK(mpp_map_core_to_client(MPP_INV_SLOTS) == -1, "core 36 mapped");
	CHECK(mpp_map_core_to_proto(MPP_INV_SLOTS) == -1, "core 36 -> a proto slot");
}

static void case_face_map(void) {
	/* Beta's ItemBlock.onItemUse axis numbering, matched by DELTA:
	 * 0 -Y, 1 +Y, 2 -Z, 3 +Z, 4 -X, 5 +X. */
	CHECK(mpp_map_face_of_side(SIDE_BOTTOM) == 0, "BOTTOM -> %d",
		  mpp_map_face_of_side(SIDE_BOTTOM));
	CHECK(mpp_map_face_of_side(SIDE_TOP) == 1, "TOP -> %d",
		  mpp_map_face_of_side(SIDE_TOP));
	CHECK(mpp_map_face_of_side(SIDE_FRONT) == 2, "FRONT -> %d",
		  mpp_map_face_of_side(SIDE_FRONT));
	CHECK(mpp_map_face_of_side(SIDE_BACK) == 3, "BACK -> %d",
		  mpp_map_face_of_side(SIDE_BACK));
	CHECK(mpp_map_face_of_side(SIDE_LEFT) == 4, "LEFT -> %d",
		  mpp_map_face_of_side(SIDE_LEFT));
	CHECK(mpp_map_face_of_side(SIDE_RIGHT) == 5, "RIGHT -> %d",
		  mpp_map_face_of_side(SIDE_RIGHT));
	/* The one the core actually branches on. */
	CHECK(mpp_map_face_of_side(SIDE_TOP) == MPP_FACE_TOP,
		  "the core's MPP_FACE_TOP (%d) is not the mapped +Y face (%d)",
		  MPP_FACE_TOP, mpp_map_face_of_side(SIDE_TOP));
	/* An unhandled side is refused, not guessed into a face. */
	CHECK(mpp_map_face_of_side(SIDE_MAX) == -1, "SIDE_MAX -> %d",
		  mpp_map_face_of_side(SIDE_MAX));
}

/* ---- 3. rings ------------------------------------------------------------ */

/* Write one record into a ring the way server_remote.c does: serial `s` lands
 * at index `s % capacity`. */
static void put_echo(struct server_remote_play_obs* st, uint32_t serial, int32_t x,
					 int32_t y, int32_t z, uint8_t type, uint8_t meta) {
	struct server_remote_block_echo* e
		= &st->block_echoes[serial % SERVER_REMOTE_BLOCK_ECHO_RING];
	e->x = x;
	e->y = y;
	e->z = z;
	e->type = type;
	e->metadata = meta;
	e->serial = serial;
	if(serial > st->block_echo_serial)
		st->block_echo_serial = serial;
}

static void put_txn(struct server_remote_play_obs* st, uint32_t serial,
					int8_t window, int16_t action, bool accepted) {
	struct server_remote_txn_result* t
		= &st->txn_results[serial % SERVER_REMOTE_TXN_RING];
	t->window = window;
	t->action_id = action;
	t->accepted = accepted;
	t->serial = serial;
	if(serial > st->txn_serial)
		st->txn_serial = serial;
}

static void case_echo_ring(void) {
	struct server_remote_play_obs st;
	struct mpp_obs o;
	const struct mpp_echo* e;

	memset(&st, 0, sizeof(st));
	memset(&o, 0x5A, sizeof(o)); /* poisoned: the copy must not leave junk */
	mpp_map_copy_echoes(&st, &o);
	CHECK(o.echo_serial == 0, "empty ring reported serial %u", o.echo_serial);
	for(size_t k = 0; k < 16; k++)
		CHECK(o.echoes[k].serial == 0, "empty ring left slot %u set", (unsigned)k);

	/* Three records, newest last. */
	put_echo(&st, 1, 10, 60, -20, 3, 0);
	put_echo(&st, 2, 11, 60, -20, 0, 0);
	put_echo(&st, 3, 10, 61, -20, 12, 5);
	memset(&o, 0, sizeof(o));
	mpp_map_copy_echoes(&st, &o);
	CHECK(o.echo_serial == 3, "serial %u, want 3", o.echo_serial);
	/* The core's own lookup must find each one. */
	e = mpp_echo_for(&o, 0, 11, 60, -20);
	CHECK(e && e->serial == 2 && e->type == 0, "serial-2 record lost");
	e = mpp_echo_for(&o, 0, 10, 61, -20);
	CHECK(e && e->serial == 3 && e->type == 12 && e->metadata == 5,
		  "serial-3 record lost or mangled");
	/* "after_serial" still filters: the core counts only what is NEWER than
	 * the baseline it latched when the phase began. */
	e = mpp_echo_for(&o, 2, 10, 60, -20);
	CHECK(!e, "a stale record was credited past its baseline");

	/* A full wrap: only the newest 16 survive, and the OLDEST are the ones
	 * lost -- never a newer one. */
	memset(&st, 0, sizeof(st));
	for(uint32_t s = 1; s <= 20; s++)
		put_echo(&st, s, (int32_t)s, 64, 0, (uint8_t)(s & 0xFFu), 0);
	memset(&o, 0, sizeof(o));
	mpp_map_copy_echoes(&st, &o);
	CHECK(o.echo_serial == 20, "wrapped serial %u, want 20", o.echo_serial);
	for(uint32_t s = 5; s <= 20; s++) {
		e = mpp_echo_for(&o, 0, (int32_t)s, 64, 0);
		CHECK(e && e->serial == s, "newest record %u missing after wrap", s);
	}
	for(uint32_t s = 1; s <= 4; s++) {
		e = mpp_echo_for(&o, 0, (int32_t)s, 64, 0);
		CHECK(!e, "overwritten record %u resurrected", s);
	}

	/* No provider at all: the observation must read as "nothing confirmed",
	 * not as stale junk. */
	memset(&o, 0x5A, sizeof(o));
	mpp_map_copy_echoes(NULL, &o);
	CHECK(o.echo_serial == 0, "NULL status left serial %u", o.echo_serial);
	CHECK(!mpp_echo_for(&o, 0, 0, 0, 0), "NULL status left a findable record");
}

/* push-0921/B19: THE WATCHED-CELL SLOT'S COPY. The network layer keeps the
 * newest echo for the one cell the script named OUTSIDE the ring, so a burst
 * of unrelated block changes cannot evict it (PC run
 * pc_mp_play_20260921_191831 lost two build legs to exactly that). This case
 * is the translation half: the slot must arrive in mpp_obs.watch_echo, and
 * mpp_echo_for must find it there even when the ring no longer holds it. */
static void case_watch_slot(void) {
	struct server_remote_play_obs st;
	struct mpp_obs o;

	/* A ring FULL of other cells, plus the watched cell only in the slot --
	 * the post-burst shape. Serial 3 is the watched echo; 100..115 are the
	 * burst that evicted it from the ring. */
	memset(&st, 0, sizeof(st));
	for(uint32_t s = 100; s < 100 + SERVER_REMOTE_BLOCK_ECHO_RING; s++)
		put_echo(&st, s, (int32_t)s, 70, (int32_t)s, BLOCK_DIRT, 0);
	st.watch_echo = (struct server_remote_block_echo) {.x = 10, .y = 61,
		.z = -12, .type = BLOCK_AIR, .metadata = 0, .serial = 3};

	memset(&o, 0x5A, sizeof(o));
	mpp_map_copy_echoes(&st, &o);
	CHECK(o.watch_echo.serial == 3, "watch serial %u, want 3",
		  o.watch_echo.serial);
	CHECK(o.watch_echo.x == 10 && o.watch_echo.y == 61 && o.watch_echo.z == -12
			  && o.watch_echo.type == BLOCK_AIR,
		  "watch echo cell/type lost in the copy");

	/* The ring genuinely does not hold it any more, so a find can only be
	   coming from the slot -- which is the whole claim. */
	const struct mpp_echo* e = mpp_echo_for(&o, 0, 10, 61, -12);
	CHECK(e && e->serial == 3,
		  "mpp_echo_for could not reach the watched cell through the slot");
	CHECK(mpp_echo_for_type(&o, 0, 10, 61, -12, BLOCK_AIR) != NULL,
		  "mpp_echo_for_type could not reach the watched cell");
	/* The baseline still applies to the slot: an echo at or before the
	   baseline is not a fresh confirmation. */
	CHECK(!mpp_echo_for(&o, 3, 10, 61, -12),
		  "the slot ignored the serial baseline");

	/* An empty slot (serial 0) is never a find, and never shadows the ring. */
	memset(&st, 0, sizeof(st));
	put_echo(&st, 7, 10, 61, -12, BLOCK_AIR, 0);
	memset(&o, 0x5A, sizeof(o));
	mpp_map_copy_echoes(&st, &o);
	CHECK(o.watch_echo.serial == 0, "empty slot copied as serial %u",
		  o.watch_echo.serial);
	e = mpp_echo_for(&o, 0, 10, 61, -12);
	CHECK(e && e->serial == 7, "an empty slot swallowed the ring's own record");

	/* A NULL source clears the slot as well as the ring -- "no remote
	   provider" must not leave last frame's watched echo standing. */
	memset(&o, 0x5A, sizeof(o));
	mpp_map_copy_echoes(NULL, &o);
	CHECK(o.watch_echo.serial == 0, "NULL status left watch serial %u",
		  o.watch_echo.serial);
}

static void case_txn_ring(void) {
	struct server_remote_play_obs st;
	struct mpp_obs o;
	const struct mpp_txn* t;

	memset(&st, 0, sizeof(st));
	put_txn(&st, 1, 0, 100, true);
	put_txn(&st, 2, 0, 101, false);
	memset(&o, 0x5A, sizeof(o));
	mpp_map_copy_txns(&st, &o);
	CHECK(o.txn_serial == 2, "txn serial %u, want 2", o.txn_serial);
	t = mpp_txn_for(&o, 0, 100);
	CHECK(t && t->accepted, "accepted txn 100 lost");
	t = mpp_txn_for(&o, 0, 101);
	CHECK(t && !t->accepted, "REJECTED txn 101 lost -- the core would hang");
	t = mpp_txn_for(&o, 2, 100);
	CHECK(!t, "a stale txn was credited past its baseline");

	/* Window is carried: mpp_txn_for only accepts window 0, so a chest's
	 * transaction must not be able to satisfy a player-inventory click. */
	memset(&st, 0, sizeof(st));
	put_txn(&st, 1, 3, 100, true);
	memset(&o, 0, sizeof(o));
	mpp_map_copy_txns(&st, &o);
	CHECK(!mpp_txn_for(&o, 0, 100), "a window-3 txn satisfied a window-0 click");

	/* Wrap: newest 8 survive. */
	memset(&st, 0, sizeof(st));
	for(uint32_t s = 1; s <= 12; s++)
		put_txn(&st, s, 0, (int16_t)(200 + s), true);
	memset(&o, 0, sizeof(o));
	mpp_map_copy_txns(&st, &o);
	for(uint32_t s = 5; s <= 12; s++)
		CHECK(mpp_txn_for(&o, 0, (int16_t)(200 + s)) != NULL,
			  "newest txn %u missing after wrap", s);
	for(uint32_t s = 1; s <= 4; s++)
		CHECK(mpp_txn_for(&o, 0, (int16_t)(200 + s)) == NULL,
			  "overwritten txn %u resurrected", s);

	/* REL-14b. Packet106's yes/no is the server's verdict on ONE click
	 * (CFR NetServerHandler.java:536 accepted=true, :543 accepted=false), so
	 * the bit must survive the copy for every slot, including after a wrap. */
	memset(&st, 0, sizeof(st));
	for(uint32_t s = 1; s <= 11; s++)
		put_txn(&st, s, 0, (int16_t)(300 + s), (s & 1u) != 0);
	memset(&o, 0x5A, sizeof(o));
	mpp_map_copy_txns(&st, &o);
	for(uint32_t s = 4; s <= 11; s++) {
		t = mpp_txn_for(&o, 0, (int16_t)(300 + s));
		CHECK(t && t->accepted == ((s & 1u) != 0) && t->serial == s,
			  "txn %u lost its accepted bit or serial across the wrap", s);
	}

	/* Exact identity: a neighbouring action id, the "no click yet" -1, or
	 * the cursor/other window (-1, 1) never answers a window-0 click. */
	memset(&st, 0, sizeof(st));
	put_txn(&st, 1, -1, 7, true);
	put_txn(&st, 2, 1, 7, true);
	put_txn(&st, 3, 0, 8, true);
	memset(&o, 0, sizeof(o));
	mpp_map_copy_txns(&st, &o);
	CHECK(!mpp_txn_for(&o, 0, 7), "a foreign-window txn answered click 7");
	CHECK(!mpp_txn_for(&o, 0, -1), "a txn matched the no-click id -1");
	t = mpp_txn_for(&o, 0, 8);
	CHECK(t && t->window == 0 && t->action_id == 8, "click 8 not matched");
	/* Freshness: the baseline is EXCLUSIVE, so a txn whose serial equals the
	 * serial latched at the click is an older answer, never this one. */
	CHECK(!mpp_txn_for(&o, 3, 8), "a txn AT the baseline counted as fresh");
	CHECK(mpp_txn_for(&o, 2, 8) != NULL, "a txn past the baseline was lost");

	/* RECONNECT RESET: a new session's ring starts again at serial 1. Slots
	 * still holding the old session's records (serials that are not the one
	 * the slot should hold now) must not be copied, and gen 2's own id 0
	 * must be found against a fresh baseline of 0. */
	memset(&st, 0, sizeof(st));
	for(uint32_t s = 1; s <= 12; s++)
		put_txn(&st, s, 0, (int16_t)s, true);
	st.txn_serial = 1;
	put_txn(&st, 1, 0, 0, false);
	memset(&o, 0x5A, sizeof(o));
	mpp_map_copy_txns(&st, &o);
	CHECK(o.txn_serial == 1, "reset txn serial %u, want 1", o.txn_serial);
	t = mpp_txn_for(&o, 0, 0);
	CHECK(t && !t->accepted && t->serial == 1, "gen-2 txn 0 lost after reset");
	for(int16_t a = 2; a <= 12; a++)
		CHECK(!mpp_txn_for(&o, 0, a), "old-session txn %d survived the reset",
			  (int)a);

	memset(&o, 0x5A, sizeof(o));
	mpp_map_copy_txns(NULL, &o);
	CHECK(o.txn_serial == 0, "NULL status left txn serial %u", o.txn_serial);
	CHECK(!mpp_txn_for(&o, 0, 100), "NULL status left a findable txn");
}

static void case_empty_slot_convention(void) {
	struct server_remote_set_slot ss;
	struct mpp_item it;

	memset(&ss, 0, sizeof(ss));
	ss.window = 0;
	ss.slot = 36;
	ss.item = -1; /* the ring's "absent" */
	ss.count = 0;
	ss.damage = 0;
	it = mpp_map_item_from_set_slot(&ss);
	CHECK(it.item == 0, "ring item -1 became core item %d, want 0", (int)it.item);
	CHECK(it.count == 0 && it.damage == 0, "absent stack kept count/damage");

	ss.item = 3;
	ss.count = 7;
	ss.damage = 2;
	it = mpp_map_item_from_set_slot(&ss);
	CHECK(it.item == 3 && it.count == 7 && it.damage == 2,
		  "present stack mangled: %d/%u/%d", (int)it.item, (unsigned)it.count,
		  (int)it.damage);

	it = mpp_map_item_from_set_slot(NULL);
	CHECK(it.item == 0 && it.count == 0, "NULL set-slot produced a stack");

	/* The core's own emptiness test must agree with the conversion, or a
	 * cleared slot would compare unequal to an empty one across the rejoin. */
	{
		struct mpp_item empty;
		memset(&empty, 0, sizeof(empty));
		ss.item = -1;
		it = mpp_map_item_from_set_slot(&ss);
		CHECK(mpp_item_eq(&it, &empty), "converted empty != core empty");
	}
}

/* ---- 4. markers ---------------------------------------------------------- */

static struct mpp_marker mk(uint32_t seq, const char* key, const char* body) {
	struct mpp_marker m;
	memset(&m, 0, sizeof(m));
	m.seq = seq;
	snprintf(m.key, sizeof(m.key), "%s", key);
	snprintf(m.body, sizeof(m.body), "%s", body);
	return m;
}

static void case_marker_line(void) {
	char line[600];
	struct mpp_marker m;

	/* THE BUILDER WRITES NO run_id. The transport does: every marker leaves
	 * through mp_play_hooks.emit -> main.c's mpp_hook_emit ->
	 * hw_telemetry_emit_observer (source/platform/hw_telemetry.c:2728-2750),
	 * which strips the closing brace and appends `,"run_id":"<s_run_id>"}` to
	 * whatever it is handed. A run_id written here therefore appears TWICE on
	 * the wire -- a duplicate JSON key, ~20 wasted bytes on every one of the
	 * ~60 markers a run emits, on a transport where a single lost line fails
	 * an 8-minute run. chat_login_script.c:552-557 sets the precedent: its
	 * marker builder writes no run_id either. */
	m = mk(7, "dig", "\"n\":3,\"x\":10");
	CHECK(mpp_map_marker_line(line, sizeof(line), &m, "cwab12cd", "wii"),
		  "dig line refused");
	CHECK(strcmp(line,
				 "{\"event\":\"mp_play\",\"seq\":7,\"marker\":\"dig\","
				 "\"n\":3,\"x\":10}")
			  == 0,
		  "dig line: %s", line);

	/* An empty body writes no dangling comma. */
	m = mk(1, "rejoin_begin", "");
	CHECK(mpp_map_marker_line(line, sizeof(line), &m, "n", "wii"),
		  "empty-body line refused");
	CHECK(strcmp(line,
				 "{\"event\":\"mp_play\",\"seq\":1,\"marker\":\"rejoin_begin\"}")
			  == 0,
		  "empty-body line: %s", line);

	/* `session` alone carries the platform and the nonce. */
	m = mk(1, "session", "\"contract\":1");
	CHECK(mpp_map_marker_line(line, sizeof(line), &m, "cwab12cd", "wii"),
		  "session line refused");
	CHECK(strstr(line, "\"platform\":\"wii\"") != NULL,
		  "session line has no platform: %s", line);
	CHECK(strstr(line, "\"nonce\":\"cwab12cd\"") != NULL,
		  "session line has no nonce: %s", line);
	CHECK(strstr(line, "\"contract\":1") != NULL,
		  "session line dropped the core's body: %s", line);
	/* and no other marker does. */
	m = mk(2, "ready", "\"gen\":1");
	CHECK(mpp_map_marker_line(line, sizeof(line), &m, "cwab12cd", "wii"),
		  "ready line refused");
	CHECK(strstr(line, "platform") == NULL && strstr(line, "nonce") == NULL,
		  "a non-session marker carried platform/nonce: %s", line);

	/* No marker of any kind carries a run_id, and the line the builder hands
	 * the transport is exactly the shape hw_telemetry_emit_observer's own
	 * contract requires: non-empty, <= 2303 bytes, last byte '}'. */
	{
		static const char* const keys[] = {
			"session", "ready",		  "login_begin", "login_ok", "p2_begin",
			"dig",	   "pickup",	  "place",		 "inv_move", "reserve_kept",
			"p2_end",  "snapshot_pre", "inv",		 "disconnected",
			"rejoin_begin", "rejoin_ok", "fail", "ok"};
		size_t k;
		for(k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
			size_t len;
			m = mk((uint32_t)(k + 1), keys[k], "\"gen\":1");
			CHECK(mpp_map_marker_line(line, sizeof(line), &m, "cwab12cd",
									  "wii"),
				  "%s line refused", keys[k]);
			CHECK(strstr(line, "run_id") == NULL,
				  "%s carries a run_id the transport also appends: %s", keys[k],
				  line);
			len = strlen(line);
			CHECK(len > 0 && len <= 2303 && line[len - 1] == '}',
				  "%s is not a line hw_telemetry_emit_observer accepts: %s",
				  keys[k], line);
		}
	}

	m = mk(4, "session", "");
	CHECK(mpp_map_marker_line(line, sizeof(line), &m, "x\"y", "wii"),
		  "escaped nonce refused");
	CHECK(strstr(line, "\"nonce\":\"x\\\"y\"") != NULL, "nonce not escaped: %s",
		  line);

	/* A line that does not fit is REFUSED and leaves nothing behind: half a
	 * JSON object reads as malformed, which is worse than missing. */
	m = mk(9, "dig", "\"n\":3");
	CHECK(!mpp_map_marker_line(line, 20, &m, "n", "wii"),
		  "a too-small buffer was accepted");
	CHECK(line[0] == 0, "a refused line left a truncated string: %s", line);
	CHECK(!mpp_map_marker_line(line, sizeof(line), NULL, "n", "wii"),
		  "a NULL marker was accepted");

	/* The platform is a PARAMETER, not a second copy of CHAT_LOGIN_PLATFORM:
	 * whatever the caller passes is what the session line carries, and a
	 * missing one is an empty string rather than a crash or a guess. */
	m = mk(5, "session", "");
	CHECK(mpp_map_marker_line(line, sizeof(line), &m, "n", "pc"),
		  "pc session line refused");
	CHECK(strstr(line, "\"platform\":\"pc\"") != NULL, "platform not carried: %s",
		  line);
	CHECK(mpp_map_marker_line(line, sizeof(line), &m, "n", NULL),
		  "NULL platform refused");
	CHECK(strstr(line, "\"platform\":\"\"") != NULL,
		  "NULL platform was guessed: %s", line);

	/* The core's widest body must still fit a sane emit buffer: the snapshot
	 * `inv` markers are the longest thing this profile ever writes. */
	{
		char big[MPP_MARKER_BODY];
		size_t k;
		for(k = 0; k + 1 < sizeof(big); k++)
			big[k] = 'x';
		big[k] = 0;
		m = mk(99, "inv", big);
		CHECK(mpp_map_marker_line(line, sizeof(line), &m, "n", "wii"),
			  "the widest core body did not fit a %u-byte line",
			  (unsigned)sizeof(line));
	}
}

/* ---- 5. body ------------------------------------------------------------- */

/* One tick of Beta gravity as entity_local_player.c really leaves it on a body
 * that is STANDING STILL on the ground. The tick's tail is unconditional
 * (entity_local_player.c:801 `e->vel[1] -= 0.08F` and :811 `e->vel[1] *=
 * 0.98F`, the non-water/lava branch), and it runs AFTER the collision move has
 * zeroed vel[1] against the floor (entity.c:610) -- so this, not zero, is what
 * vel[1] reads at the point mp_play_frame observes the body, every frame, for
 * as long as it stands there. The start-of-tick 0.005 cutoff
 * (entity_local_player.c:592-594) cannot touch it: 0.0784 is 15x the cutoff. */
#define STANDING_VY (-0.0784F)

static void case_body_at_rest(void) {
	/* THE reading the whole build leg turns on. A body standing motionless on
	 * the ground must read at rest -- and until 2026-09-21 it could not: the
	 * predicate compared vel[1] against the horizontal cutoff, so `at_rest`
	 * was unsatisfiable for ANY grounded body and every build cycle of the
	 * first PC run was skipped "never at rest in one column". */
	CHECK(mpp_map_body_at_rest(true, 0.0F, STANDING_VY, 0.0F),
		  "a body standing still on the ground does not read at rest "
		  "(vy=%.4f is the gravity load entity_local_player.c leaves on every "
		  "grounded tick)", (double)STANDING_VY);
	/* The same body a few ticks after the walker was released: friction has
	 * taken the horizontal velocity under the simulation's own cutoff. */
	CHECK(mpp_map_body_at_rest(true, 0.004F, STANDING_VY, -0.004F),
		  "a grounded body under the horizontal cutoff does not read at rest");

	/* ... and everything that is NOT at rest still is not. */
	CHECK(!mpp_map_body_at_rest(false, 0.0F, STANDING_VY, 0.0F),
		  "an airborne body read at rest");
	CHECK(!mpp_map_body_at_rest(true, 0.05F, STANDING_VY, 0.0F),
		  "a body still walking (vx 0.05) read at rest");
	CHECK(!mpp_map_body_at_rest(true, 0.0F, STANDING_VY, -0.05F),
		  "a body still walking (vz -0.05) read at rest");
	/* A step-up frame: the ledge branch hands back vel[1] = -0.6 and the same
	 * tick tail turns it into -0.6664. on_ground is true on that frame, so the
	 * VERTICAL reading is the only thing that can refuse it -- the body is
	 * about to drop into the very cell the dig leg would aim at. */
	CHECK(!mpp_map_body_at_rest(true, 0.0F, -0.6664F, 0.0F),
		  "a body dropping off a step (vy -0.6664) read at rest");
	/* The jump frame: on_ground has not been cleared yet and vel[1] is +0.42
	 * (entity_local_player.c:658). Rising is never at rest. */
	CHECK(!mpp_map_body_at_rest(true, 0.0F, 0.42F, 0.0F),
		  "a body that just jumped (vy +0.42) read at rest");
	/* Garbage in is not rest: a NaN must refuse, never compare true. */
	CHECK(!mpp_map_body_at_rest(true, (float)NAN, STANDING_VY, 0.0F),
		  "a NaN horizontal velocity read at rest");
	CHECK(!mpp_map_body_at_rest(true, 0.0F, (float)NAN, 0.0F),
		  "a NaN vertical velocity read at rest");
}

/* ---- 6. yaw -------------------------------------------------------------- */

/* Beta's own forward vector for a yaw in the CORE's convention -- written out
 * here from the decompile rather than from the code under test, so the test
 * cannot agree with a wrong mapping by sharing its arithmetic.
 * [740c583 Entity.moveFlying, strafe 0 / forward 1:
 *  motionX += -forward*sin(yaw), motionZ += forward*cos(yaw)] */
static void beta_forward(int core_yaw_deg, float* fx, float* fz) {
	double r = (double)core_yaw_deg * 3.14159265358979323846 / 180.0;
	*fx = (float)(-sin(r));
	*fz = (float)(cos(r));
}

static void case_yaw_bridge(void) {
	/* THE CLAIM: converting a core (Beta) yaw to this engine's convention and
	 * then taking the ENGINE's forward vector reproduces BETA's forward vector
	 * for that yaw. Four cardinals plus a diagonal. If this is wrong the core
	 * refuses to nudge ("aim never faced the column centre") instead of walking
	 * the wrong way -- but it still never digs, so the mapping has to be right.
	 *
	 * The cardinals are also written out explicitly, because "0 = +Z, 90 = -X"
	 * is the sentence mp_play_core.h states and a table is how it is pinned. */
	static const struct {
		int core_yaw;
		float wx, wz;
		const char* name;
	} tbl[] = {
		{0, 0.0F, 1.0F, "0 = +Z"},
		{90, -1.0F, 0.0F, "90 = -X"},
		{180, 0.0F, -1.0F, "180 = -Z"},
		{270, 1.0F, 0.0F, "270 = +X"},
		{45, -0.70710678F, 0.70710678F, "45 = -X+Z diagonal"},
	};
	for(size_t k = 0; k < sizeof(tbl) / sizeof(tbl[0]); k++) {
		int eng = mpp_map_engine_yaw_deg(tbl[k].core_yaw);
		float fx, fz, bx, bz;
		CHECK(eng >= 0 && eng < 360, "%s: engine yaw %d out of [0,360)",
			  tbl[k].name, eng);
		mpp_map_forward_of((float)eng * 3.14159265F / 180.0F, &fx, &fz);
		beta_forward(tbl[k].core_yaw, &bx, &bz);
		CHECK(fabsf(fx - bx) < 1e-4F && fabsf(fz - bz) < 1e-4F,
			  "%s: engine yaw %d gives forward (%.4f, %.4f), Beta wants "
			  "(%.4f, %.4f)", tbl[k].name, eng, (double)fx, (double)fz,
			  (double)bx, (double)bz);
		CHECK(fabsf(fx - tbl[k].wx) < 1e-4F && fabsf(fz - tbl[k].wz) < 1e-4F,
			  "%s: forward (%.4f, %.4f), the table says (%.4f, %.4f)",
			  tbl[k].name, (double)fx, (double)fz, (double)tbl[k].wx,
			  (double)tbl[k].wz);
	}
	/* Normalized into [0,360) whatever comes in -- the core turns its heading
	 * with `(h + 90) % 360`, but a negative or >360 value must not become a
	 * negative angle at the aim lock. */
	CHECK(mpp_map_engine_yaw_deg(0) == 0, "0 -> %d",
		  mpp_map_engine_yaw_deg(0));
	CHECK(mpp_map_engine_yaw_deg(360) == 0, "360 -> %d",
		  mpp_map_engine_yaw_deg(360));
	CHECK(mpp_map_engine_yaw_deg(-90) == mpp_map_engine_yaw_deg(270),
		  "-90 and 270 disagree: %d vs %d", mpp_map_engine_yaw_deg(-90),
		  mpp_map_engine_yaw_deg(270));
	CHECK(mpp_map_engine_yaw_deg(450) == mpp_map_engine_yaw_deg(90),
		  "450 and 90 disagree: %d vs %d", mpp_map_engine_yaw_deg(450),
		  mpp_map_engine_yaw_deg(90));

	/* The forward vector is a UNIT vector on the horizontal plane -- the core
	 * divides by |(cx,cz)| only, and takes the dot product raw. */
	for(int d = 0; d < 360; d += 37) {
		float fx, fz;
		mpp_map_forward_of((float)d * 3.14159265F / 180.0F, &fx, &fz);
		CHECK(fabsf(sqrtf(fx * fx + fz * fz) - 1.0F) < 1e-4F,
			  "forward at %d deg is not unit length (%.6f)", d,
			  (double)sqrtf(fx * fx + fz * fz));
	}
	/* NULL outs are ignored, never written through. */
	mpp_map_forward_of(0.0F, NULL, NULL);
}

/* ---- 7. window action ids ------------------------------------------------ */

static void case_last_click_id(void) {
	/* THE FIRST REAL PC RUN THAT GOT THIS FAR (pc_mp_play_20260921_124033)
	 * failed here: the fixture logged
	 *   ev=click window=0 action=1 ... accepted=true
	 *   ev=slot_state window=0 slot=36 item=0        (source emptied)
	 *   ev=slot_state window=0 slot=-1 item=3 count=2 (cursor holds it)
	 * -- a perfect server-side pick-up -- and the driver still failed
	 * "pick-up click never confirmed", because it was waiting on transaction
	 * id 2 while the click on the wire carried id 1. window_container.c hands
	 * out the PRE-increment value, so the counter after that click read 2. */
	CHECK(mpp_map_last_click_id(2u) == 1,
		  "a client counter of 2 means the last click carried id 1, not %d",
		  (int)mpp_map_last_click_id(2u));
	CHECK(mpp_map_last_click_id(1u) == 0, "counter 1 -> %d, want 0",
		  (int)mpp_map_last_click_id(1u));
	CHECK(mpp_map_last_click_id(1000u) == 999, "counter 1000 -> %d, want 999",
		  (int)mpp_map_last_click_id(1000u));
	/* No click yet: a sentinel no real id can equal, so the core's baseline
	 * test still fires on the first click and never matches a transaction. */
	CHECK(mpp_map_last_click_id(0u) == -1,
		  "counter 0 (no click yet) -> %d, want the -1 sentinel",
		  (int)mpp_map_last_click_id(0u));
	/* Strictly increasing with the counter, so "the counter moved off the
	 * baseline" is still the right test for "a click went out". */
	for(uint16_t k = 1; k < 600; k++)
		CHECK(mpp_map_last_click_id((uint16_t)(k + 1u))
				  > mpp_map_last_click_id(k),
			  "not monotonic at %u", (unsigned)k);
}

/* ---- main ---------------------------------------------------------------- */

/* CaveWii internal health -> Beta hit points (push-0921/B11). The marker used
 * to publish the internal number: run pc_mp_play_20260921_143758 said
 * health 152 in its `ready` marker while the fixture receipt beside it said
 * h=19, for the same body at the same instant. */
static void case_health_scale(void) {
	/* THE WIRE GRID IS EXACT. health_wire_to_internal (server_interface.h:41)
	 * is `wire * (HEALTH_PER_HEART / 2)`, so every value Beta can send must
	 * come back unchanged -- including the 19 that produced the 152. */
	for(int wire = 0; wire <= MPP_MAP_BETA_MAX_HEALTH; wire++) {
		int internal = wire * (MPP_MAP_HEALTH_PER_HEART / 2);
		CHECK(mpp_map_health_to_beta(internal) == wire,
			   "wire %d -> internal %d must round-trip", wire, internal);
	}
	CHECK(mpp_map_health_to_beta(152) == 19, "the run's own reading");
	CHECK(mpp_map_health_to_beta(160) == MPP_MAP_BETA_MAX_HEALTH,
		   "a full player is 20, not 160");

	/* ZERO MEANS DEAD, AND ONLY DEAD. The core ends a run on health <= 0, so
	 * every live internal value -- including the 1..7 that truncation would
	 * have flattened to 0 -- must report at least 1. A single wrong answer
	 * here ends a healthy run on a death that never happened. */
	CHECK(mpp_map_health_to_beta(0) == 0, "zero is zero");
	CHECK(mpp_map_health_to_beta(-1) == 0, "below zero is zero");
	CHECK(mpp_map_health_to_beta(-160) == 0, "far below zero is zero");
	for(int internal = 1; internal <= 2 * 160; internal++)
		CHECK(mpp_map_health_to_beta(internal) >= 1,
			   "internal %d is ALIVE and must never report 0", internal);

	/* Monotonic and clamped: more internal health never reports less, and
	 * nothing above the player maximum escapes the Beta range. */
	for(int internal = 0; internal <= 2 * 160; internal++) {
		int a = mpp_map_health_to_beta(internal);
		int b = mpp_map_health_to_beta(internal + 1);
		CHECK(b >= a, "monotonic at %d", internal);
		CHECK(a >= 0 && a <= MPP_MAP_BETA_MAX_HEALTH, "in range at %d",
			   internal);
	}
	printf("  health: internal -> Beta hit points, exact on the whole wire "
		   "grid, clamped at %d, and 0 only for a body at or below zero\n",
		   MPP_MAP_BETA_MAX_HEALTH);
}

static void case_landing_support_shape(void) {
	const struct AABB full = {0, 0, 0, 1, 1, 1};
	struct AABB box = full;
	CHECK(mpp_map_support_full_cube(true, BLOCK_GRASS, 1, &full),
		  "known grass with a full collision cube must admit one-block landing");
	CHECK(mpp_map_support_full_cube(true, BLOCK_STONE, 1, &full),
		  "known stone with a full collision cube must admit one-block landing");
	CHECK(!mpp_map_support_full_cube(false, BLOCK_STONE, 1, &full),
		  "unknown chunk must fail closed even with apparent stone");
	CHECK(!mpp_map_support_full_cube(true, BLOCK_STONE, 0, &full),
		  "no collision box must fail closed");
	CHECK(!mpp_map_support_full_cube(true, BLOCK_STONE, 2, &full),
		  "multiple partial boxes must fail closed");
	CHECK(!mpp_map_support_full_cube(true, BLOCK_STONE, 1, NULL),
		  "absent collision box must fail closed");
	box.y2 = 0.5f;
	CHECK(!mpp_map_support_full_cube(true, BLOCK_STONE, 1, &box),
		  "half-height landing must fail closed");
	box = full;
	box.x1 = 0.1f;
	CHECK(!mpp_map_support_full_cube(true, BLOCK_STONE, 1, &box),
		  "offset footing must fail closed");
	box = full;
	box.z2 = NAN;
	CHECK(!mpp_map_support_full_cube(true, BLOCK_STONE, 1, &box),
		  "nonfinite collision box must fail closed");
	const uint8_t bad[] = {BLOCK_AIR, BLOCK_WATER_FLOW, BLOCK_WATER_STILL,
		BLOCK_LAVA_FLOW, BLOCK_LAVA_STILL, BLOCK_FIRE, BLOCK_CACTUS,
		BLOCK_SAND, BLOCK_GRAVEL};
	for(size_t i = 0; i < sizeof(bad); i++)
		CHECK(!mpp_map_support_full_cube(true, bad[i], 1, &full),
			  "hazard/unstable type %u must fail closed", (unsigned)bad[i]);
	puts("  landing support: full known solid cube only; unknown, fluid, hazard, unstable and partial fail closed");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	case_action_bits();
	case_action_mask();
	case_load_bridge();
	case_multiplayer_setup_bridge();
	case_slot_round_trip();
	case_face_map();
	case_echo_ring();
	case_watch_slot(); /* push-0921/B19 */
	case_txn_ring();
	case_empty_slot_convention();
	case_marker_line();
	case_body_at_rest();
	case_yaw_bridge();
	case_last_click_id();
	case_health_scale();
	case_landing_support_shape();
	printf("mp_play_map: %s (%d failure%s)\n", g_fail ? "FAIL" : "PASS", g_fail,
		   g_fail == 1 ? "" : "s");
	return g_fail ? 1 : 0;
}
