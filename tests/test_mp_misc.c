/* P3 MP-MISC (critique-1028 AUDIT-PACKETS rows 0x47 / 0xC8 / 0x82): the
 * ONLINE client's handling of
 *   0x47 Packet71Weather  -> EntityLightningBolt: thunder + explode once
 *        [740c583 NetClientHandler.handleWeather,
 *         EntityLightningBolt.onUpdate]
 *   0xC8 Packet200Statistic -> addStat via the achievements seam
 *        (GE_REMOTE_STAT, authority PACKET200)
 *        [740c583 NetClientHandler.handleStatistic,
 *         EntityClientPlayerMP.func_27027_b]
 *   0x82 Packet130UpdateSign: our own sign placement opens the editor on the
 *        server's Packet53 for the target cell; edits + close send ONE
 *        Packet130 at that cell [740c583 ItemSign.onItemUse,
 *        GuiEditSign.onGuiClosed, NetServerHandler.handleUpdateSign]
 * plus malformed / oversized / truncated input on each of the three ids
 * through the real decoder (no event, no desync of the following packet).
 * Fixture bytes -> decoder -> process_event / remote_process_srpc; the
 * clin_process_direct and game_event_emit seams are --wrap'ed spies. */
#define main existing_mp_suite_main
#include "test_mp_s0_env.c"
#undef main

#include "game/achievements/game_event.h"

enum { LOCAL_ID = 42 };

/* ---- spies ------------------------------------------------------------- */
void __real_clin_process_direct(struct client_rpc* call);
static struct client_rpc seen[64];
static unsigned n_seen;
void __wrap_clin_process_direct(struct client_rpc* call) {
	bool spied = call->type == CRPC_SOUND || call->type == CRPC_OPEN_WINDOW
		|| call->type == CRPC_WINDOW_TRANSACTION
		|| call->type == CRPC_SIGN_TEXT;
	if(spied) {
		if(n_seen < 64)
			seen[n_seen] = *call;
		n_seen++;
	}
	/* No window exists client-side in this harness: OPEN/TRANSACTION are
	 * recorded only (their client effect is screen_sign's own suite). */
	if(call->type != CRPC_OPEN_WINDOW && call->type != CRPC_WINDOW_TRANSACTION)
		__real_clin_process_direct(call);
}

void __real_game_event_emit(const struct game_event* ev);
static struct game_event events[16];
static unsigned n_events;
void __wrap_game_event_emit(const struct game_event* ev) {
	if(n_events < 16)
		events[n_events] = *ev;
	n_events++;
	__real_game_event_emit(ev);
}

static void spy_reset(void) {
	n_seen = 0;
	n_events = 0;
}

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}
static bool approx(float a, float b) {
	return fabsf(a - b) < 1e-4f;
}

static void feed(struct server_remote* owner, const uint8_t* bytes, size_t n,
				 enum mp_event_type type) {
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET};
	event.packet = decode_exact(bytes, n, type);
	assert(process_event(owner, &event));
}

/* Feed a whole byte stream; returns the number of events and the last
 * result. Used for the malformed / oversized / back-to-back cases. */
static unsigned stream(const uint8_t* bytes, size_t n, struct mp_event* out,
					   unsigned cap, enum mp_decode_result* last) {
	struct mp_decoder decoder;
	mp_decoder_init(&decoder);
	unsigned count = 0;
	enum mp_decode_result r = MP_DECODE_MORE;
	for(size_t i = 0; i < n; i++) {
		struct mp_event ev;
		r = mp_decoder_feed(&decoder, bytes[i], &ev);
		if(r == MP_DECODE_EVENT && count < cap)
			out[count++] = ev;
		if(r == MP_DECODE_ERROR)
			break;
	}
	*last = r;
	return count;
}

static void set_play(struct server_remote_impl* impl) {
	thread_mutex_lock(&impl->lock);
	impl->state = REMOTE_PLAY;
	thread_mutex_lock(&impl->frame_lock);
	impl->frame_count = 0;
	thread_mutex_unlock(&impl->frame_lock);
	thread_mutex_unlock(&impl->lock);
}

/* Copy queued frame `id` (first match) out; false if none. */
static bool find_frame(struct server_remote_impl* impl, uint8_t id,
					   struct mp_frame* out, unsigned* count) {
	bool found = false;
	unsigned n = 0;
	thread_mutex_lock(&impl->frame_lock);
	for(size_t i = 0; i < impl->frame_count; i++) {
		const struct mp_frame* f
			= &impl->frames[(impl->frame_head + i) % REMOTE_QUEUE_CAPACITY].frame;
		if(f->length && f->data[0] == id) {
			if(!found && out)
				*out = *f;
			found = true;
			n++;
		}
	}
	thread_mutex_unlock(&impl->frame_lock);
	if(count)
		*count = n;
	return found;
}

/* ---- 0x47 ---------------------------------------------------------------- */
static void weather_bytes(uint8_t b[18], int32_t eid, int8_t type, int32_t x,
						  int32_t y, int32_t z) {
	b[0] = 0x47;
	put_i32(b + 1, eid);
	b[5] = (uint8_t)type;
	put_i32(b + 6, x);
	put_i32(b + 10, y);
	put_i32(b + 14, z);
}

static void test_lightning(struct server_remote* owner) {
	struct server_remote_impl* impl = owner->impl;
	uint8_t b[18];
	/* (10.0, 64.0, -5.5) in Beta's 1/32 fixed point. */
	weather_bytes(b, 77, 1, 320, 2048, -176);
	spy_reset();
	uint32_t bolts = impl->weather_lightning_bolts;
	feed(owner, b, sizeof b, MP_EVENT_WEATHER);
	check(n_seen == 2 && seen[0].type == CRPC_SOUND && seen[1].type == CRPC_SOUND,
		"0x47 type 1: exactly two sounds (no restrike replay)");
	check(seen[0].payload.sound.sound_id == SOUND_AMBIENT_WEATHER_THUNDER
		&& approx(seen[0].payload.sound.vol, 10000.0f)
		&& seen[0].payload.sound.pitch >= 0.8f
		&& seen[0].payload.sound.pitch <= 1.0f,
		"0x47: ambient.weather.thunder 10000.0, pitch 0.8 + r*0.2");
	check(seen[1].payload.sound.sound_id == SOUND_RANDOM_EXPLODE
		&& approx(seen[1].payload.sound.vol, 2.0f)
		&& seen[1].payload.sound.pitch >= 0.5f
		&& seen[1].payload.sound.pitch <= 0.7f,
		"0x47: random.explode 2.0, pitch 0.5 + r*0.2");
	check(approx(seen[0].payload.sound.pos[0], 10.0f)
		&& approx(seen[0].payload.sound.pos[1], 64.0f)
		&& approx(seen[0].payload.sound.pos[2], -5.5f)
		&& approx(seen[1].payload.sound.pos[2], -5.5f),
		"0x47: both at the bolt (fixed-point / 32.0)");
	check(impl->weather_lightning_bolts == bolts + 1, "0x47: bolt counter +1");

	/* Beta's handler spawns nothing for any other type byte. */
	uint32_t noop = impl->weather_lightning_noop;
	weather_bytes(b, 78, 0, 0, 0, 0);
	spy_reset();
	feed(owner, b, sizeof b, MP_EVENT_WEATHER);
	weather_bytes(b, 79, -1, 0, 0, 0);
	feed(owner, b, sizeof b, MP_EVENT_WEATHER);
	check(n_seen == 0 && impl->weather_lightning_noop == noop + 2,
		"0x47 type 0 / -1: silent, counted");

	/* Extreme coordinates: no crash, finite sound position. */
	weather_bytes(b, 80, 1, INT32_MAX, INT32_MIN, INT32_MAX);
	spy_reset();
	feed(owner, b, sizeof b, MP_EVENT_WEATHER);
	check(n_seen == 2 && isfinite(seen[0].payload.sound.pos[0])
		&& isfinite(seen[0].payload.sound.pos[1]),
		"0x47 at INT32 extremes: still two finite-position sounds");

	/* Truncated (17 of 18 bytes): no event; back-to-back with a 0xC8 the
	 * framing stays aligned (both decode). */
	struct mp_event evs[4];
	enum mp_decode_result last;
	weather_bytes(b, 81, 1, 32, 32, 32);
	check(stream(b, 17, evs, 4, &last) == 0 && last == MP_DECODE_MORE,
		"0x47 truncated: no event, decoder waits");
	uint8_t pair[18 + 6];
	memcpy(pair, b, 18);
	pair[18] = 0xC8;
	put_i32(pair + 19, 5242881);
	pair[23] = 1;
	unsigned n = stream(pair, sizeof pair, evs, 4, &last);
	check(n == 2 && evs[0].type == MP_EVENT_WEATHER
		&& evs[0].payload.weather.x == 32
		&& evs[1].type == MP_EVENT_STATISTIC
		&& evs[1].payload.statistic.stat_id == 5242881
		&& evs[1].payload.statistic.amount == 1,
		"0x47 then 0xC8 in one stream: both decode, no desync");
}

/* ---- 0xC8 ---------------------------------------------------------------- */
static void test_statistic(struct server_remote* owner) {
	struct server_remote_impl* impl = owner->impl;
	/* achievement.mineWood: Achievement id 1 -> stat 5242880 + 1. */
	uint8_t b[6] = {0xC8};
	put_i32(b + 1, 5242881);
	b[5] = 1;
	spy_reset();
	uint32_t fwd = impl->statistic_forwarded;
	feed(owner, b, sizeof b, MP_EVENT_STATISTIC);
	check(n_events == 1 && events[0].kind == GE_REMOTE_STAT
		&& events[0].authority == GE_AUTH_PACKET200
		&& events[0].u.remote.stat_id == 5242881
		&& events[0].u.remote.amount == 1 && events[0].who.slot == 0,
		"0xC8 achievement id: one GE_REMOTE_STAT (PACKET200, id, amount)");
	check(impl->statistic_forwarded == fwd + 1, "0xC8: forwarded counter +1");

	/* A server-side counter (stat.walkOneCm 2000 is independent -> the
	 * consumer ignores it; any server stat id still forwards verbatim, the
	 * consumer owns the class rule). Amount 100 = Beta's chunk cap. */
	put_i32(b + 1, 2000);
	b[5] = 100;
	spy_reset();
	feed(owner, b, sizeof b, MP_EVENT_STATISTIC);
	check(n_events == 1 && events[0].u.remote.stat_id == 2000
		&& events[0].u.remote.amount == 100,
		"0xC8 counter id: forwarded verbatim (amount 100)");

	/* Hostile bytes: a negative amount (signed byte) or negative id never
	 * reaches the consumer; counted. */
	uint32_t noop = impl->statistic_noop;
	b[5] = 0xFF;
	spy_reset();
	feed(owner, b, sizeof b, MP_EVENT_STATISTIC);
	put_i32(b + 1, -5);
	b[5] = 1;
	feed(owner, b, sizeof b, MP_EVENT_STATISTIC);
	check(n_events == 0 && impl->statistic_noop == noop + 2,
		"0xC8 amount -1 / id -5: not emitted, counted");

	struct mp_event evs[2];
	enum mp_decode_result last;
	check(stream(b, 5, evs, 2, &last) == 0 && last == MP_DECODE_MORE,
		"0xC8 truncated: no event");
}

/* ---- 0x82 ---------------------------------------------------------------- */
static void place(struct server_remote* owner, int32_t x, int32_t y, int32_t z,
				  enum side side, int16_t item) {
	struct server_rpc call = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = side,
			.has_held_item = item != 0,
			.held_item = {.id = (uint16_t)item, .count = 1}}};
	remote_process_srpc(&call, owner);
}

static void block_change(struct server_remote* owner, int32_t x, int8_t y,
						 int32_t z, uint8_t type, uint8_t meta) {
	uint8_t b[12] = {0x35};
	put_i32(b + 1, x);
	b[5] = (uint8_t)y;
	put_i32(b + 6, z);
	b[10] = type;
	b[11] = meta;
	feed(owner, b, sizeof b, MP_EVENT_BLOCK_CHANGE);
}

static void sign_set(struct server_remote* owner, uint8_t window, size_t slot,
					 char ch, uint16_t action) {
	struct server_rpc call = {.type = SRPC_SIGN_SET,
		.payload.sign_set = {.window = window, .slot = slot, .ch = ch,
			.action_id = action}}; /* x/y/z 0, exactly as the online GUI */
	remote_process_srpc(&call, owner);
}

static void close_window(struct server_remote* owner, uint8_t window) {
	struct server_rpc call = {.type = SRPC_WINDOW_CLOSE,
		.payload.window_close.window = window};
	remote_process_srpc(&call, owner);
}

static unsigned count_seen(enum client_rpc_type type) {
	unsigned n = 0;
	for(unsigned i = 0; i < n_seen && i < 64; i++)
		n += seen[i].type == type;
	return n;
}

static void test_sign_online(struct server_remote* owner) {
	struct server_remote_impl* impl = owner->impl;
	set_play(impl);

	/* Sign on the top face of (4,63,4): Packet15 goes out, nothing opens
	 * until the server answers. */
	spy_reset();
	place(owner, 4, 63, 4, SIDE_TOP, ITEM_SIGN);
	check(find_frame(impl, 0x0F, NULL, NULL) && n_seen == 0,
		"sign place: Packet15 queued, no editor before the server answers");
	/* handlePlace's Packet53 pair: the clicked cell first (stone), then the
	 * target cell (sign post). Only the target opens the editor. */
	block_change(owner, 4, 63, 4, BLOCK_STONE, 0);
	check(count_seen(CRPC_OPEN_WINDOW) == 0, "Packet53 for the clicked cell: no editor");
	uint32_t opened = impl->sign_editor_opened;
	block_change(owner, 4, 64, 4, BLOCK_SIGN_POST, 8);
	check(count_seen(CRPC_OPEN_WINDOW) == 1
		&& seen[0].payload.window_open.window == REMOTE_SIGN_EDIT_WINDOW
		&& seen[0].payload.window_open.type == WINDOW_TYPE_SIGN
		&& seen[0].payload.window_open.slot_count == 64
		&& impl->sign_editor_opened == opened + 1,
		"Packet53 sign at the target: the 64-cell sign editor opens");
	/* A repeat Packet53 (e.g. a later metadata echo) does not reopen. */
	block_change(owner, 4, 64, 4, BLOCK_SIGN_POST, 8);
	check(count_seen(CRPC_OPEN_WINDOW) == 1, "second Packet53: armed once, no reopen");

	/* Type "Hi" on line 0 and "!" on line 2; column 15 and a stranger
	 * window are refused; control chars refused. */
	spy_reset();
	sign_set(owner, REMOTE_SIGN_EDIT_WINDOW, 0, 'H', 1);
	sign_set(owner, REMOTE_SIGN_EDIT_WINDOW, 1, 'i', 2);
	sign_set(owner, REMOTE_SIGN_EDIT_WINDOW, 32, '!', 3);
	sign_set(owner, REMOTE_SIGN_EDIT_WINDOW, 15, 'X', 4);
	sign_set(owner, 7, 16, 'Y', 5);
	sign_set(owner, REMOTE_SIGN_EDIT_WINDOW, 17, '\n', 6);
	sign_set(owner, REMOTE_SIGN_EDIT_WINDOW, 9999, 'Z', 7);
	bool acks = n_seen == 7;
	for(unsigned i = 0; acks && i < 7; i++)
		acks = seen[i].type == CRPC_WINDOW_TRANSACTION
			&& seen[i].payload.window_transaction.action_id == i + 1
			&& seen[i].payload.window_transaction.accepted == (i < 3);
	check(acks, "sign edits: each answered at once; col 15 / wrong window /"
		" control char / out-of-range refused");

	/* Close: ONE Packet130 at the server-confirmed cell, no Packet101. */
	set_play(impl);
	close_window(owner, REMOTE_SIGN_EDIT_WINDOW);
	struct mp_frame f;
	unsigned n130 = 0, n101 = 0;
	bool got = find_frame(impl, 0x82, &f, &n130);
	(void)find_frame(impl, 0x65, NULL, &n101);
	static const uint8_t expected[] = {0x82, 0,0,0,4, 0,64, 0,0,0,4,
		0,2, 0,'H', 0,'i', 0,0, 0,1, 0,'!', 0,0};
	check(got && n130 == 1 && n101 == 0 && f.length == sizeof expected
		&& memcmp(f.data, expected, sizeof expected) == 0,
		"close: one Packet130 (4,64,4) [\"Hi\",\"\",\"!\",\"\"], no Packet101");
	check(!impl->sign_edit.active, "close: editor state cleared");

	/* A refused placement: the target Packet53 is air -> no editor, and a
	 * later sign there (someone else's) does not open one either. */
	spy_reset();
	set_play(impl);
	place(owner, 10, 63, 10, SIDE_RIGHT, ITEM_SIGN);
	block_change(owner, 11, 63, 10, BLOCK_AIR, 0);
	block_change(owner, 11, 63, 10, BLOCK_SIGN, 5);
	check(count_seen(CRPC_OPEN_WINDOW) == 0, "refused placement: no editor, disarmed");

	/* Bottom face (ItemSign returns false) and non-sign items never arm. */
	place(owner, 20, 63, 20, SIDE_BOTTOM, ITEM_SIGN);
	block_change(owner, 20, 62, 20, BLOCK_SIGN_POST, 0);
	place(owner, 21, 63, 21, SIDE_TOP, 1 /* stone */);
	block_change(owner, 21, 64, 21, BLOCK_SIGN_POST, 0);
	check(count_seen(CRPC_OPEN_WINDOW) == 0, "bottom face / non-sign item: never arms");

	/* No editor: edits are refused, and closing that window is an ordinary
	 * Packet101 (no stray Packet130 at 0,0,0 -- the old behaviour). */
	spy_reset();
	set_play(impl);
	sign_set(owner, 5, 0, 'A', 9);
	close_window(owner, 5);
	unsigned n101b = 0, n130b = 0;
	(void)find_frame(impl, 0x65, NULL, &n101b);
	(void)find_frame(impl, 0x82, NULL, &n130b);
	check(n_seen == 1 && !seen[0].payload.window_transaction.accepted
		&& n130b == 0 && n101b == 1,
		"no editor: edit refused, close sends Packet101 only");

	/* Close with nothing typed still sends Packet130 (Beta's onGuiClosed is
	 * unconditional): four empty lines. */
	set_play(impl);
	place(owner, 30, 63, 30, SIDE_FRONT, ITEM_SIGN);
	block_change(owner, 30, 63, 29, BLOCK_SIGN, 2);
	set_play(impl);
	close_window(owner, REMOTE_SIGN_EDIT_WINDOW);
	static const uint8_t empty[] = {0x82, 0,0,0,30, 0,63, 0,0,0,29,
		0,0, 0,0, 0,0, 0,0};
	got = find_frame(impl, 0x82, &f, NULL);
	check(got && f.length == sizeof empty && memcmp(f.data, empty, sizeof empty) == 0,
		"untouched editor close: Packet130 with four empty lines at (30,63,29)");

	/* Inbound 0x82 (server -> client): text lands in the sign store via
	 * CRPC_SIGN_TEXT; 16 code units on a line is a decode error (Beta's
	 * readString(15) throws -> disconnect), never a partial apply. */
	uint8_t in[] = {0x82, 0,0,0,4, 0,64, 0,0,0,4,
		0,2, 0,'O', 0,'k', 0,0, 0,0, 0,0};
	spy_reset();
	feed(owner, in, sizeof in, MP_EVENT_UPDATE_SIGN);
	check(n_seen == 1 && seen[0].type == CRPC_SIGN_TEXT
		&& seen[0].payload.sign_text.x == 4 && seen[0].payload.sign_text.y == 64
		&& seen[0].payload.sign_text.grid[0] == 'O'
		&& seen[0].payload.sign_text.grid[1] == 'k'
		&& seen[0].payload.sign_text.grid[2] == ' '
		&& seen[0].payload.sign_text.grid[16] == ' ',
		"inbound 0x82: CRPC_SIGN_TEXT with the server's lines");
	uint8_t big[11 + 2 + 32];
	memcpy(big, in, 11);
	put_i16(big + 11, 16);
	for(int i = 0; i < 16; i++) {
		big[13 + 2 * i] = 0;
		big[14 + 2 * i] = 'A';
	}
	struct mp_event evs[2];
	enum mp_decode_result last;
	check(stream(big, sizeof big, evs, 2, &last) == 0 && last == MP_DECODE_ERROR,
		"inbound 0x82 with a 16-unit line: decode error, no event");
	put_i16(big + 11, -1);
	check(stream(big, 13, evs, 2, &last) == 0 && last == MP_DECODE_ERROR,
		"inbound 0x82 with a negative length: decode error");
	check(stream(in, sizeof in - 1, evs, 2, &last) == 0 && last == MP_DECODE_MORE,
		"inbound 0x82 truncated: no event");
}

/* Held slot after respawn (fixed 10-29): verify only -- slot 0 sends no
 * Packet16 (Beta's syncCurrentPlayItem only sends on change from 0). */
static void test_held_slot_zero(struct server_remote* owner) {
	struct server_remote_impl* impl = owner->impl;
	static struct window_container inv_wc;
	if(!gstate.players[0].windows[WINDOWC_INVENTORY]) {
		assert(windowc_create(&inv_wc, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
		gstate.players[0].windows[WINDOWC_INVENTORY] = &inv_wc;
	}
	inventory_set_hotbar(
		windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]), 0);
	set_play(impl);
	uint8_t respawn[] = {0x09, 0};
	feed(owner, respawn, sizeof respawn, MP_EVENT_RESPAWN);
	unsigned n16 = 0;
	(void)find_frame(impl, 0x10, NULL, &n16);
	check(n16 == 0, "respawn with slot 0: no Packet16");
	inventory_set_hotbar(
		windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]), 3);
	set_play(impl);
	feed(owner, respawn, sizeof respawn, MP_EVENT_RESPAWN);
	struct mp_frame f;
	check(find_frame(impl, 0x10, &f, &n16) && n16 == 1 && f.length == 3
		&& f.data[2] == 3, "respawn with slot 3: one Packet16(3)");
}

int main(void) {
	assert(svin_init() && clin_init());
	blocks_init();
	items_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	struct server_remote owner = {0};
	assert(server_remote_init(&owner));
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, LOCAL_ID);
	gstate.remote_session = true;
	struct server_remote_impl* impl = owner.impl;
	impl->entity_id = LOCAL_ID;
	impl->dimension = WORLD_DIM_OVERWORLD;

	test_lightning(&owner);
	test_statistic(&owner);
	test_sign_online(&owner);
	test_held_slot_zero(&owner);

	server_remote_destroy(&owner);
	if(failures) {
		printf("test_mp_misc: %u FAIL\n", failures);
		return 1;
	}
	puts("test_mp_misc: PASS");
	return 0;
}
