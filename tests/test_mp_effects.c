/* mp-effects (2026-10-29): Beta 1.7.3 block events, aux effects and
 * own-player packets on the ONLINE client path. Fixture packet bytes go
 * through the real decoder and process_event (server_remote_protocol.c);
 * clin_process_direct is --wrap'ed as the spy for every sound / particle /
 * state RPC that comes out.
 *   0x36 -> World.playNoteAt dispatch by block id (note block, piston,
 *           other) [World.java:2339-2344, BlockNote.playBlock,
 *           BlockPistonBase.playBlock:82-170]
 *   0x3D -> RenderGlobal.func_28136_a 1000-1005, 2000, 2001
 *           [RenderGlobal.java:1359-1418]
 *   0x26/0x28 for our own id [NetClientHandler.func_9447_a,
 *           func_21148_a; EntityLiving.handleHealthUpdate:850-861]
 *   0x10 re-sent after a respawn [PlayerControllerMP.syncCurrentPlayItem] */
#define main existing_mp_suite_main
#include "test_mp_s0_env.c"
#undef main

#include "block/piston_moving.h"
#include "network/piston_moving_transport.h"

enum { LOCAL_ID = 42 };

/* ---- the spy ----------------------------------------------------------- */
void __real_clin_process_direct(struct client_rpc* call);
static struct client_rpc seen[64];
static unsigned n_seen;
void __wrap_clin_process_direct(struct client_rpc* call) {
	if(call->type == CRPC_SOUND || call->type == CRPC_SMOKE_PUFF
	   || call->type == CRPC_RECORD_EVENT
	   || call->type == CRPC_LOCAL_PLAYER_FIRE) {
		if(n_seen < 64)
			seen[n_seen] = *call;
		n_seen++;
	}
	__real_clin_process_direct(call);
}
static void spy_reset(void) {
	n_seen = 0;
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

static void block_event(struct server_remote* owner, int32_t x, int16_t y,
						int32_t z, uint8_t b1, uint8_t b2) {
	uint8_t b[] = {0x36, 0,0,0,0, 0,0, 0,0,0,0, b1, b2};
	put_i32(b + 1, x);
	b[5] = (uint8_t)((uint16_t)y >> 8);
	b[6] = (uint8_t)y;
	put_i32(b + 7, z);
	feed(owner, b, sizeof b, MP_EVENT_NOTE_BLOCK);
}

static void aux(struct server_remote* owner, int32_t effect, int32_t x,
				int8_t y, int32_t z, int32_t data) {
	uint8_t b[] = {0x3D, 0,0,0,0, 0,0,0,0, 0, 0,0,0,0, 0,0,0,0};
	put_i32(b + 1, effect);
	put_i32(b + 5, x);
	b[9] = (uint8_t)y;
	put_i32(b + 10, z);
	put_i32(b + 14, data);
	feed(owner, b, sizeof b, MP_EVENT_DOOR_CHANGE);
}

static struct block_data at(int x, int y, int z) {
	return world_get_block(&gstate.world, x, y, z);
}
static void put(int x, int y, int z, uint8_t type, uint8_t meta) {
	world_set_block(&gstate.world, x, y, z,
		(struct block_data) {.type = type, .metadata = meta}, false);
}

static void install_air_chunk(void) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	memset(slot, 0, sizeof(*slot));
	struct client_rpc chunk = {.type = CRPC_CHUNK, .payload.chunk = {
		.x = 0, .y = 0, .z = 0, .sx = CHUNK_SIZE, .sy = WORLD_HEIGHT,
		.sz = CHUNK_SIZE, .slot = slot}};
	clin_process(&chunk);
	assert(world_find_chunk(&gstate.world, 4, 64, 4));
}

static void test_block_events(struct server_remote* owner) {
	/* Note block: instrument 2 (snare), pitch 12 -> 1.0; volume 3, centre;
	 * plus one note particle. */
	put(8, 64, 8, BLOCK_NOTEBLOCK, 0);
	particle_set_camera((vec3) {8.0f, 65.0f, 8.0f});
	size_t particles = particle_debug_total_count();
	spy_reset();
	block_event(owner, 8, 64, 8, 2, 12);
	check(n_seen == 1 && seen[0].type == CRPC_SOUND
		&& seen[0].payload.sound.sound_id == SOUND_NOTE_SNARE
		&& approx(seen[0].payload.sound.vol, 3.0f)
		&& approx(seen[0].payload.sound.pitch, noteblock_pitch(12))
		&& approx(seen[0].payload.sound.pos[0], 8.5f)
		&& approx(seen[0].payload.sound.pos[1], 64.5f),
		"0x36 on a note block: note.snare 3.0 @ noteblock_pitch(12)");
	check(particle_debug_total_count() == particles + 1,
		"0x36 on a note block: one note particle");

	/* A block with an empty playBlock (stone) and air: silent. */
	put(10, 64, 10, BLOCK_STONE, 0);
	spy_reset();
	block_event(owner, 10, 64, 10, 0, 5);
	block_event(owner, 11, 64, 11, 0, 5);
	check(n_seen == 0, "0x36 on stone / air: no sound (no harp note)");

	/* Piston (33) facing +X (5) with stone in front: extend. */
	put(4, 64, 4, BLOCK_PISTON_BASE, 5);
	put(5, 64, 4, BLOCK_STONE, 0);
	spy_reset();
	block_event(owner, 4, 64, 4, 0, 5);
	check(n_seen == 1 && seen[0].payload.sound.sound_id == SOUND_TILE_PISTON_OUT
		&& approx(seen[0].payload.sound.vol, 0.5f),
		"0x36 piston event 0: tile.piston.out 0.5, no note sound");
	struct piston_moving_state head, pushed;
	check(at(4, 64, 4).type == BLOCK_PISTON_BASE
		&& at(4, 64, 4).metadata == (5 | 8),
		"extend: base metadata facing|8");
	check(at(5, 64, 4).type == BLOCK_PISTON_MOVING
		&& client_piston_moving_query(5, 64, 4, &head)
		&& head.stored_id == BLOCK_PISTON_HEAD && head.stored_meta == 5
		&& head.facing == 5 && piston_moving_is_extending(&head),
		"extend: head cell is a moving tile storing the head (34:5)");
	check(at(6, 64, 4).type == BLOCK_PISTON_MOVING
		&& client_piston_moving_query(6, 64, 4, &pushed)
		&& pushed.stored_id == BLOCK_STONE && piston_moving_is_extending(&pushed),
		"extend: pushed stone animates one cell forward");

	/* Retract: the still-moving head force-settles, the base becomes the
	 * source moving tile, the (non-sticky) head cell is cleared. */
	spy_reset();
	block_event(owner, 4, 64, 4, 1, 5);
	struct piston_moving_state base;
	check(n_seen == 1 && seen[0].payload.sound.sound_id == SOUND_TILE_PISTON_IN
		&& approx(seen[0].payload.sound.vol, 0.5f),
		"0x36 piston event 1: tile.piston.in 0.5");
	check(at(4, 64, 4).type == BLOCK_PISTON_MOVING
		&& client_piston_moving_query(4, 64, 4, &base)
		&& base.stored_id == BLOCK_PISTON_BASE && base.stored_meta == 5
		&& !piston_moving_is_extending(&base),
		"retract: base is a retracting moving tile storing 33:5");
	check(at(5, 64, 4).type == BLOCK_AIR
		&& !client_piston_moving_query(5, 64, 4, &head),
		"retract: head cell cleared");

	/* The server's settle Packet53 replaces the moving cells. Its client
	 * apply is CRPC_SET_BLOCK (remote_apply_block_change's output; this
	 * harness's column is not in server_remote's residency table, so the
	 * apply is driven directly). */
	struct client_rpc settle = {.type = CRPC_SET_BLOCK,
		.payload.set_block = {.x = 4, .y = 64, .z = 4,
			.block = {.type = BLOCK_PISTON_BASE, .metadata = 5}}};
	clin_process(&settle);
	check(at(4, 64, 4).type == BLOCK_PISTON_BASE
		&& !client_piston_moving_query(4, 64, 4, &base),
		"server settle Packet53 retires the client moving tile");

	/* A facing > 5 (Beta would throw) is dropped. */
	spy_reset();
	block_event(owner, 4, 64, 4, 0, 9);
	check(n_seen == 0 && at(4, 64, 4).type == BLOCK_PISTON_BASE,
		"0x36 piston with facing 9: ignored");
}

static void test_aux_effects(struct server_remote* owner) {
	struct server_remote_impl* impl = owner->impl;
	spy_reset();
	aux(owner, 1000, 3, 64, -2, 0);
	aux(owner, 1001, 3, 64, -2, 0);
	aux(owner, 1002, 3, 64, -2, 0);
	check(n_seen == 3
		&& seen[0].payload.sound.sound_id == SOUND_RANDOM_CLICK
		&& approx(seen[0].payload.sound.pitch, 1.0f)
		&& seen[1].payload.sound.sound_id == SOUND_RANDOM_CLICK
		&& approx(seen[1].payload.sound.pitch, 1.2f)
		&& seen[2].payload.sound.sound_id == SOUND_RANDOM_BOW
		&& approx(seen[2].payload.sound.pitch, 1.2f)
		&& approx(seen[0].payload.sound.pos[0], 3.0f)
		&& approx(seen[0].payload.sound.pos[2], -2.0f),
		"1000/1001/1002: click 1.0, click 1.2, bow 1.2 at RAW coords");

	bool saw_open = false, saw_close = false, pitch_ok = true;
	for(int i = 0; i < 64; i++) {
		spy_reset();
		aux(owner, 1003, 10, 64, -5, 0);
		if(n_seen != 1) { pitch_ok = false; break; }
		saw_open |= seen[0].payload.sound.sound_id == SOUND_RANDOM_DOOR_OPEN;
		saw_close |= seen[0].payload.sound.sound_id == SOUND_RANDOM_DOOR_CLOSE;
		pitch_ok &= seen[0].payload.sound.pitch >= 0.9f
			&& seen[0].payload.sound.pitch <= 1.0f
			&& approx(seen[0].payload.sound.pos[0], 10.5f);
	}
	check(saw_open && saw_close && pitch_ok,
		"1003: door_open AND door_close both occur (Math.random coin), "
		"pitch 0.9-1.0, centre");

	spy_reset();
	aux(owner, 1004, 1, 70, 1, 0);
	check(n_seen == 1 && seen[0].payload.sound.sound_id == SOUND_RANDOM_FIZZ
		&& approx(seen[0].payload.sound.vol, 0.5f)
		&& seen[0].payload.sound.pitch >= 1.8f
		&& seen[0].payload.sound.pitch <= 3.4f,
		"1004: random.fizz 0.5, pitch 2.6+-0.8");

	spy_reset();
	aux(owner, 1005, 2, 64, 2, ITEM_RECORD_13);
	aux(owner, 1005, 2, 64, 2, 0);
	check(n_seen == 2 && seen[0].type == CRPC_RECORD_EVENT
		&& seen[0].payload.record_event.record_id == ITEM_RECORD_13
		&& seen[1].type == CRPC_RECORD_EVENT
		&& seen[1].payload.record_event.record_id == 0,
		"1005: record 13 plays, data 0 stops");

	spy_reset();
	aux(owner, 2000, 7, 64, 7, 5); /* dx = +1, dz = 0 */
	check(n_seen == 1 && seen[0].type == CRPC_SMOKE_PUFF
		&& approx(seen[0].payload.smoke_puff.intensity, -6.0f)
		&& approx(seen[0].payload.smoke_puff.pos[0], 8.1f)
		&& approx(seen[0].payload.smoke_puff.pos[1], 64.5f)
		&& approx(seen[0].payload.smoke_puff.pos[2], 7.5f),
		"2000: directed dispenser smoke (direction 5 -> +X)");

	spy_reset();
	aux(owner, 2001, 6, 64, 6, BLOCK_STONE);
	aux(owner, 2001, 6, 64, 6, 0);
	check(n_seen == 1
		&& seen[0].payload.sound.sound_id == audio_block_break_sound_id(BLOCK_STONE)
		&& approx(seen[0].payload.sound.vol,
			(audio_block_sound_volume(BLOCK_STONE) + 1.0f) / 2.0f)
		&& approx(seen[0].payload.sound.pitch,
			audio_block_sound_pitch(BLOCK_STONE) * 0.8f)
		&& approx(seen[0].payload.sound.pos[1], 64.5f),
		"2001: another player's break sound (id>0 only)");

	uint32_t unhandled = impl->door_change_unhandled;
	spy_reset();
	aux(owner, 1500, 0, 64, 0, 0);
	check(n_seen == 0 && impl->door_change_unhandled == unhandled + 1,
		"unknown effect id: silent and counted");
}

static void test_own_entity(struct server_remote* owner) {
	struct server_remote_impl* impl = owner->impl;
	impl->have_last_sent_position = true;
	impl->last_sent_x = 1.0;
	impl->last_sent_y = 70.0;
	impl->last_sent_z = 2.0;
	uint8_t status[] = {0x26, 0,0,0,0, 2};
	put_i32(status + 1, LOCAL_ID);
	spy_reset();
	feed(owner, status, sizeof status, MP_EVENT_ENTITY_STATUS);
	status[5] = 3;
	feed(owner, status, sizeof status, MP_EVENT_ENTITY_STATUS);
	status[5] = 6;
	feed(owner, status, sizeof status, MP_EVENT_ENTITY_STATUS);
	check(n_seen == 2
		&& seen[0].payload.sound.sound_id == SOUND_RANDOM_HURT
		&& seen[1].payload.sound.sound_id == SOUND_RANDOM_HURT
		&& approx(seen[0].payload.sound.vol, 1.0f)
		&& seen[0].payload.sound.pitch >= 0.8f
		&& seen[0].payload.sound.pitch <= 1.2f
		&& approx(seen[0].payload.sound.pos[1], 70.0f),
		"own 0x26 status 2/3: random.hurt at the player; status 6 silent");

	uint8_t meta[] = {0x28, 0,0,0,0, 0x00, 0x01, 0x7f};
	put_i32(meta + 1, LOCAL_ID);
	spy_reset();
	feed(owner, meta, sizeof meta, MP_EVENT_ENTITY_METADATA);
	meta[6] = 0x00;
	feed(owner, meta, sizeof meta, MP_EVENT_ENTITY_METADATA);
	check(n_seen == 2 && seen[0].type == CRPC_LOCAL_PLAYER_FIRE
		&& seen[0].payload.local_player_fire.owner == 0
		&& seen[0].payload.local_player_fire.on_fire
		&& !seen[1].payload.local_player_fire.on_fire,
		"own 0x28 flag 0: first-person fire on, then off");
}

static void test_held_slot_resync(struct server_remote* owner) {
	struct server_remote_impl* impl = owner->impl;
	static struct window_container inv_wc;
	if(!gstate.players[0].windows[WINDOWC_INVENTORY]) {
		assert(windowc_create(&inv_wc, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
		gstate.players[0].windows[WINDOWC_INVENTORY] = &inv_wc;
	}
	inventory_set_hotbar(
		windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]), 5);
	assert(clin_selected_hotbar_slot() == 5);
	thread_mutex_lock(&impl->lock);
	impl->state = REMOTE_PLAY;
	thread_mutex_lock(&impl->frame_lock);
	impl->frame_count = 0;
	thread_mutex_unlock(&impl->frame_lock);
	thread_mutex_unlock(&impl->lock);
	uint8_t respawn[] = {0x09, 0};
	feed(owner, respawn, sizeof respawn, MP_EVENT_RESPAWN);
	bool found = false;
	thread_mutex_lock(&impl->frame_lock);
	for(size_t i = 0; i < impl->frame_count; i++) {
		const struct mp_frame* f
			= &impl->frames[(impl->frame_head + i) % REMOTE_QUEUE_CAPACITY].frame;
		if(f->length == 3 && f->data[0] == 0x10 && f->data[1] == 0
		   && f->data[2] == 5)
			found = true;
	}
	thread_mutex_unlock(&impl->frame_lock);
	check(found, "respawn: Packet16 re-sends the selected slot 5");
}

/* hw1029b/G1: the telemetry-only per-kind wire counters
 * (server_remote_get_kind_counts) move exactly once per packet, and only for
 * the kinds they name. Runs after test_held_slot_resync, which left the impl
 * in REMOTE_PLAY so queue_frame_playing accepts the two outgoing frames. */
static void test_kind_counts(struct server_remote* owner) {
	struct server_remote_impl* impl = owner->impl;
	uint32_t before[SR_KIND_COUNT], after[SR_KIND_COUNT];
	assert(server_remote_get_kind_counts(owner, before));
	check(before[SR_KIND_RX_BLOCK_ACTION] > 0 && before[SR_KIND_RX_AUX_SFX] > 0
		&& before[SR_KIND_RX_SELF_STATUS] == 3
		&& before[SR_KIND_RX_METADATA] == 2,
		"kind counters already saw the earlier 0x36/0x3D/own 0x26 x3/0x28 x2");

	block_event(owner, 12, 64, 12, 0, 0);
	aux(owner, 1500, 0, 64, 0, 0);
	uint8_t sleep[15] = {0x11};
	put_i32(sleep + 1, 999);
	feed(owner, sleep, sizeof sleep, MP_EVENT_SLEEP);
	uint8_t attach[9] = {0x27};
	put_i32(attach + 1, 999);
	put_i32(attach + 5, -1);
	feed(owner, attach, sizeof attach, MP_EVENT_ATTACH_ENTITY);
	uint8_t status[] = {0x26, 0,0,0,0, 2};
	put_i32(status + 1, 999); /* someone else's status: not "self" */
	feed(owner, status, sizeof status, MP_EVENT_ENTITY_STATUS);
	put_i32(status + 1, LOCAL_ID);
	status[5] = 6;
	feed(owner, status, sizeof status, MP_EVENT_ENTITY_STATUS);
	uint8_t meta[] = {0x28, 0,0,0,0, 0x00, 0x00, 0x7f};
	put_i32(meta + 1, 999);
	feed(owner, meta, sizeof meta, MP_EVENT_ENTITY_METADATA);

	impl->local_sleeping = true;
	struct server_rpc leave = {.type = SRPC_PLAYER_STATE,
		.payload.player_state = {.leave_bed_seq = impl->leave_bed_seq_sent + 7}};
	remote_process_srpc(&leave, owner);
	remote_process_srpc(&leave, owner); /* same press again: no resend */
	struct server_rpc use = {.type = SRPC_USE_ITEM};
	remote_process_srpc(&use, owner);

	assert(server_remote_get_kind_counts(owner, after));
	check(after[SR_KIND_RX_BLOCK_ACTION] == before[SR_KIND_RX_BLOCK_ACTION] + 1,
		"mp_rx_blockact +1 per 0x36");
	check(after[SR_KIND_RX_AUX_SFX] == before[SR_KIND_RX_AUX_SFX] + 1,
		"mp_rx_auxsfx +1 per 0x3D (even an unhandled effect id)");
	check(after[SR_KIND_RX_BED] == before[SR_KIND_RX_BED] + 1,
		"mp_rx_bed +1 per 0x11");
	check(after[SR_KIND_RX_ATTACH] == before[SR_KIND_RX_ATTACH] + 1,
		"mp_rx_attach +1 per 0x27");
	check(after[SR_KIND_RX_SELF_STATUS] == before[SR_KIND_RX_SELF_STATUS] + 1,
		"mp_rx_selfstatus +1 for our id only");
	check(after[SR_KIND_RX_METADATA] == before[SR_KIND_RX_METADATA] + 1,
		"mp_rx_meta +1 per 0x28 (any entity)");
	check(after[SR_KIND_TX_LEAVE_BED] == before[SR_KIND_TX_LEAVE_BED] + 1,
		"mp_tx_leavebed +1 per queued 0x13 state 3 (one press, one frame)");
	check(after[SR_KIND_TX_USE_ITEM] == before[SR_KIND_TX_USE_ITEM] + 1,
		"mp_tx_useitem +1 per queued 0x0F air-use");

	static const char* const names[SR_KIND_COUNT] = SERVER_REMOTE_KIND_NAMES;
	check(strcmp(names[SR_KIND_RX_BED], "mp_rx_bed") == 0
		&& strcmp(names[SR_KIND_TX_USE_ITEM], "mp_tx_useitem") == 0,
		"kind names line up with the enum");

	struct server_remote dead = {0};
	uint32_t none[SR_KIND_COUNT];
	check(!server_remote_get_kind_counts(&dead, none) && none[0] == 0,
		"no impl: false and zero-filled");
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
	install_air_chunk();

	test_block_events(&owner);
	test_aux_effects(&owner);
	test_own_entity(&owner);
	test_held_slot_resync(&owner);
	test_kind_counts(&owner);

	server_remote_destroy(&owner);
	if(failures) {
		printf("test_mp_effects: %u FAIL\n", failures);
		return 1;
	}
	puts("test_mp_effects: PASS");
	return 0;
}
