/*
	Server-AUTHORITATIVE minecart mounting + throttle.

	THE GAP THIS PINS (milestone brief item 6, twice assigned, still open when
	this was written): mounting and throttle lived only on the CLIENT cart --
	entity_minecart.c's onRightClick read gstate.players[0].local_player and toggled the
	client entity's `occupied`; entity_local_player.c wrote the client cart's
	`speed` directly; and server_local's SRPC_ENTITY_USE handler dropped any
	entity that was not ENTITY_MONSTER. The AUTHORITATIVE cart (the one that is
	ticked for physics, archived, and reloaded) never learned it was ridden and
	never received a single throttle input, so persistence saved a stationary,
	unoccupied cart no matter what the player saw.

	FAIL-BEFORE: the mount case exercises the EXISTING SRPC_ENTITY_USE path, so
	on the pre-authority server it fails at `assert(cart->data.minecart.occupied)`
	-- the monster-only filter silently drops the packet. The throttle cases use
	the new SRPC surface and therefore cannot compile against the old tree; the
	mount failure is the honest fail-before for the milestone.

	Beta framing: EntityMinecart.interact() (740c583) seats the rider by setting
	riddenByEntity server-side; NetServerHandler.func_6006_a validates
	getDistanceSqToEntity < 36.0 before any interact. Direct forward/back
	throttle has NO Beta equivalent (Beta's EntityMinecart deliberately does not
	couple rider motion; EntityBoat does) -- the throttle itself is the
	documented Wii-controller playability deviation; what THIS test pins is that
	the deviation acts on the AUTHORITATIVE cart through a validated server
	path, not on a client-side copy.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "entity/combat.h"
#include "entity/entity_minecart.h"
#include "game/game_state.h"
#include "item/inventory.h"
#include "network/inventory_logic.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/entity_archive.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "particle.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_minecart_authority_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100
#define RY 101

extern struct item item_minecart;

static struct server_local s;
static volatile int g_drain_run = 1;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type, int meta) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
								  (struct block_data) {
									  .type = (uint8_t)type,
									  .metadata = (uint8_t)(meta & 0x0F),
								  }));
}

static bool place_cart_on(int x, int y, int z) {
	struct block_data blk_on, blk_where;
	assert(server_world_get_block(&s.world, x, y, z, &blk_on));
	assert(server_world_get_block(&s.world, x, y + 1, z, &blk_where));
	struct block_info on = {
		.block = &blk_on, .neighbours = NULL, .x = x, .y = y, .z = z};
	struct block_info where = {
		.block = &blk_where, .neighbours = NULL, .x = x, .y = y + 1, .z = z};
	struct item_data held = {.id = ITEM_MINECART, .durability = 0, .count = 1};
	struct item* it = item_get(&held);
	assert(it == &item_minecart && it->onItemPlace);
	return it->onItemPlace(&s, &s.player, &held, &where, &on, SIDE_TOP);
}

static struct entity* only_minecart(void) {
	struct entity* found = NULL;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MINECART) {
			assert(!found);
			found = e;
		}
		dict_entity_next(it);
	}
	assert(found);
	return found;
}

/* Observe the AUTHORITATIVE occupancy publication at emit time (deterministic;
 * the drain thread is not involved). Without this the test could not tell a
 * server that silently changed occupancy from one that told the client. */
static struct {
	unsigned count;
	uint32_t last_id;
	uint32_t last_occupant_id;
	uint32_t last_epoch;
	bool last_occupied;
	uint32_t last_input_epoch;
	uint32_t last_request_seq;
} g_pub;

static struct {
	unsigned spawn_count;
	unsigned snapshot_count;
	struct client_rpc last_spawn;
	struct client_rpc last_snapshot;
} g_cart_rpc;

static unsigned g_sounds;

static void observe_rpc(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_SOUND) {
		g_sounds++;
		return;
	}
	if(call->type == CRPC_SPAWN_MINECART) {
		g_cart_rpc.spawn_count++;
		g_cart_rpc.last_spawn = *call;
		return;
	}
	if(call->type == CRPC_MINECART_SNAPSHOT) {
		g_cart_rpc.snapshot_count++;
		g_cart_rpc.last_snapshot = *call;
		return;
	}
	if(call->type != CRPC_MINECART_OCCUPANCY)
		return;
	g_pub.count++;
	g_pub.last_id = call->payload.minecart_occupancy.entity_id;
	g_pub.last_occupant_id = call->payload.minecart_occupancy.occupant_id;
	g_pub.last_epoch = call->payload.minecart_occupancy.ride_epoch;
	g_pub.last_occupied = call->payload.minecart_occupancy.occupied;
	g_pub.last_input_epoch = call->payload.minecart_occupancy.input_epoch;
	g_pub.last_request_seq = call->payload.minecart_occupancy.request_seq;
}

static void use_entity(uint32_t id) {
	struct server_rpc rpc = {
		.type = SRPC_ENTITY_USE,
		.payload.entity_use.entity_id = id,
	};
	server_local_test_process_rpc(&s, &rpc);
}

static void send_ride_intent(uint32_t id, uint32_t seq, bool desired) {
	struct server_rpc rpc = {
		.type = SRPC_MINECART_RIDE_INTENT,
		.payload.minecart_ride_intent = {
			.entity_id = id,
			.input_epoch = s.input_epoch_counter,
			.request_seq = seq,
			.desired_occupied = desired,
		},
	};
	server_local_test_process_rpc(&s, &rpc);
}

static void send_control_tuple(
	uint32_t id, uint32_t epoch, uint32_t seq, uint32_t positive,
	uint32_t negative) {
	struct server_rpc rpc = {
		.type = SRPC_MINECART_CONTROL,
		.payload.minecart_control = {
			.entity_id = id,
			.ride_epoch = epoch,
			.input_seq = seq,
			.positive_steps = positive,
			.negative_steps = negative,
		},
	};
	server_local_test_process_rpc(&s, &rpc);
}

static void send_throttle(uint32_t id, int8_t dir) {
	struct entity** pp = dict_entity_get(s.entities, id);
	struct entity* cart = pp ? *pp : NULL;
	uint32_t epoch = cart && cart->type == ENTITY_MINECART
		? cart->data.minecart.ride_epoch : 0;
	uint32_t seq = cart && cart->type == ENTITY_MINECART
		? cart->data.minecart.control_seq + 1 : 1;
	uint32_t positive = cart && cart->type == ENTITY_MINECART
		? cart->data.minecart.control_positive_steps : 0;
	uint32_t negative = cart && cart->type == ENTITY_MINECART
		? cart->data.minecart.control_negative_steps : 0;
	if(dir > 0)
		positive++;
	else if(dir < 0)
		negative++;
	send_control_tuple(id, epoch, seq, positive, negative);
}

int main(void) {
	test_fixture_dir("test_minecart_authority_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	clin_test_set_rpc_observer(observe_rpc, NULL);

	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	string_clear(level);
	s.world.local = &s;
	s.worlds[0].session = &s; /* A1a (D1): owning session for services/callbacks */

	/* Complete Beta position +/-32 entity-activation neighborhood. */
	struct server_chunk* sc = NULL;
	for(int lcx = -2; lcx <= 2; lcx++)
		for(int lcz = -2; lcz <= 2; lcz++) {
			sc = NULL;
			assert(server_world_load_chunk(&s.world, lcx, lcz, &sc) && sc);
		}

	const int X = 4, Z = 4;
	/* A LINE of rail (z=2..13), not a lone tile: the sound-dispatch section
	 * below runs the cart along it, and a straight line normalises to the same
	 * flat NS shape 0 a lone rail does, so every earlier expectation holds. */
	for(int rz = 2; rz <= 13; rz++) {
		set(X, YF, rz, BLOCK_STONE, 0);
		set(X, RY, rz, BLOCK_RAIL, 0);
	}

	/* Real inventory (capacity INVENTORY_SIZE, all empty -> fist damage): the
	 * ENTITY_ATTACK branch reads the hotbar for per-tool strength, and a
	 * zeroed capacity-0 inventory trips inventory.c's slot assert. */
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
							INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	/* Deviation-#18: zero-init health now reads as DEAD (the dwell gates
	 * discard throttle/actions), so the harness player must be explicitly
	 * alive like every live-game player is. */
	s.player.health = MAX_PLAYER_HEALTH;
	s.input_epoch_counter = 1; /* F3: ride-intent requires nonzero epoch */
	s.player.x = X + 0.5;
	s.player.y = RY + 2.0; /* eye; well inside the 36.0 distSq gate */
	s.player.z = Z + 0.5;

	/* =====================================================================
	 * (0) CONSTRUCTION zeroes the rider-input byte.
	 * Client carts arrive via CRPC_SPAWN_MINECART -> s_clin_alloc, a plain
	 * malloc, so every field the constructor forgets is recycled heap. Poison
	 * the struct first, exactly as test_entity_persistence_env does, so a field
	 * added later without an initialiser cannot pass by landing on a zero byte.
	 * ===================================================================== */
	{
		struct entity poisoned;
		memset(&poisoned, 0xBE, sizeof(poisoned));
		entity_minecart(4242, &poisoned, true, &s.world);
		assert(poisoned.data.minecart.pending_throttle == 0
			   && poisoned.data.minecart.replay_throttle == 0
			   && "rider input must be ZERO at construction, not heap garbage");
		assert(poisoned.data.minecart.roll_dist == 0.0f
			   && "the rolling-sound odometer must be ZERO at construction "
				  "(audit F5) -- client carts are malloc'd");
		assert(poisoned.data.minecart.damage_taken == 0
			   && poisoned.data.minecart.shake_ticks == 0
			   && poisoned.data.minecart.shake_dir == 1
			   && "break-state must be INITIALISED at construction, not heap "
				  "garbage -- it previously lived in minecart_roll_sound's "
				  "parked branch, so a fresh cart could spawn pre-damaged and "
				  "a parked cart had its punch feedback erased every tick");
		assert(poisoned.data.minecart.ride_epoch == 0
			   && poisoned.data.minecart.control_seq == 0
			   && poisoned.data.minecart.control_positive_steps == 0
			   && poisoned.data.minecart.control_negative_steps == 0
			   && poisoned.data.minecart.control_acked_seq == 0
			   && poisoned.data.minecart.control_acked_positive_steps == 0
			   && poisoned.data.minecart.control_acked_negative_steps == 0
			   && !poisoned.data.minecart.control_ready
			   && !poisoned.data.minecart.control_dirty
			   && poisoned.data.minecart.snapshot_last_tick == 0
			   && !poisoned.data.minecart.snapshot_dirty
			   && !poisoned.data.minecart.snapshot_urgent
			   && "all cumulative-control and scheduler state must start clean on "
				  "plain malloc-backed carts");
		assert(!poisoned.data.minecart.occupied);
	}

	assert(place_cart_on(X, RY, Z));
	struct entity* cart = only_minecart();
	uint32_t cart_id = cart->id;
	assert(server_local_test_tick_entities(&s)); /* settle onto the rail */
	cart = only_minecart();

	/* =====================================================================
	 * (0b) A PARKED cart acquires its rail heading on the FIRST tick -- the
	 * full production sequence (constructor -> settle tick AT REST), not the
	 * resolver in isolation (audit F3). Before the fix the at-rest tick
	 * returned before seeding, (hx,hz) stayed (0,0), and the resolver's
	 * facing dot product was 0 whatever way the rider looked -- every
	 * standstill start went +X/+Z. The original one-way-cart report.
	 * ===================================================================== */
	assert(cart->data.minecart.speed == 0.0f && "cart must still be at rest");
	assert(cart->data.minecart.hx == 0 && cart->data.minecart.hz == +1
		   && "a resting cart on an NS rail must have its heading seeded by "
			  "the first tick, not deferred until it moves");
	/* With a real axis, facing decides the standstill start in BOTH ways. */
	assert(entity_minecart_resolve_throttle(+1, 0.0f, cart->data.minecart.hx,
											cart->data.minecart.hz,
											(float)M_PI)
			   == -1
		   && "facing NORTH at rest, forward must set off toward -Z");
	assert(entity_minecart_resolve_throttle(+1, 0.0f, cart->data.minecart.hx,
											cart->data.minecart.hz, 0.0f)
			   == +1
		   && "facing SOUTH at rest, forward must set off toward +Z");

	/* =====================================================================
	 * (1) MOUNT is accepted and recorded by the AUTHORITATIVE cart.
	 * FAIL-BEFORE: the old handler's `type != ENTITY_MONSTER` filter drops
	 * this packet, so `occupied` stays false.
	 * ===================================================================== */
	assert(!cart->data.minecart.occupied);
	unsigned pub_before = g_pub.count;
	send_ride_intent(cart_id, 1, true);
	assert(cart->data.minecart.occupied
		   && "SRPC_MINECART_RIDE_INTENT must mount it SERVER-side (F3)");
	/* ...and it must TELL the client. The client mounts optimistically on
	 * right-click, so without this correction a server-side rejection would
	 * leave the two permanently inverted. */
	assert(g_pub.count == pub_before + 1 && g_pub.last_id == cart_id
		   && g_pub.last_occupied
		   && "the server must PUBLISH authoritative occupancy on mount");
	assert(g_pub.last_epoch == cart->data.minecart.ride_epoch
		   && g_pub.last_epoch != 0
		   && g_pub.last_occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID
		   && "mount publication must establish a nonzero input epoch and carry "
			  "the full-width rider id");
	/* The rider MUST be recorded as the reserved local-player entity id (0).
	 * This is not cosmetic: the client gates BOTH its throttle send and its
	 * death cleanup on `occupant_id == gstate.players[0].local_player->id`, and
	 * local_player is spawned with id 0 at both CRPC_WORLD_RESET sites. An
	 * invented id (say 1) type-checks, passes a naive "is it set" assertion,
	 * and silently disables riding in the real game. */
	assert(cart->data.minecart.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID
		   && "rider must be the RESERVED local-player id the client compares "
			  "against");
	assert(cart->rightClickText && strcmp(cart->rightClickText, "Dismount") == 0
		   && "the HUD prompt must follow authoritative occupancy");

	/* =====================================================================
	 * (1b) THE ROLLING SOUND ACTUALLY DISPATCHES (audit F4+F5). The client
	 * tick used to gate minecart_roll_sound on the tick RETURN VALUE read as
	 * "alive" -- but that value means REMOVE, and normal ticks return false,
	 * so the sound was unreachable and the registration-only audio test never
	 * noticed. Drive the REAL client tick on a moving cart and prove the
	 * odometer both accumulates and WRAPS (the wrap subtract sits immediately
	 * before the audio_play3d call, so a wrap proves the call site ran;
	 * audio_play3d itself no-ops headless).
	 * ===================================================================== */
	{
		bool saved_snd = gstate.config_minecart_sound;
		gstate.config_minecart_sound = true;
		cart->data.minecart.speed = 0.25f;
		assert(cart->data.minecart.roll_dist == 0.0f);
		float moved_total = 0.0f;
		for(int t = 0; t < 12; t++) {
			float pz = cart->pos[2];
			assert(cart->tick_client);
			cart->tick_client(cart); /* prediction path == production path */
			moved_total += fabsf(cart->pos[2] - pz);
		}
		assert(moved_total > 1.5f && "the cart must actually travel");
		assert(cart->data.minecart.roll_dist > 0.0f
			   && "the odometer must ACCUMULATE -- zero here means the sound "
				  "dispatch is dead again (the remove-vs-alive gate)");
		assert(cart->data.minecart.roll_dist < moved_total - 1.0f
			   && "the odometer must WRAP at the clack step -- pure "
				  "accumulation means the play call site was never reached");
		/* Put the cart back the way the later sections expect. */
		cart->data.minecart.speed = 0.0f;
		gstate.config_minecart_sound = saved_snd;
	}

	/* =====================================================================
	 * (1c) MINECART SOUND ADD-ON RE-SCOPE (2026-09-03): the speed->volume
	 * curve and the "inside" key-selection predicate are PURE functions
	 * (same shape as entity_minecart_resolve_throttle above), so they are
	 * tested directly here, plus one real-dispatch drive of the seat loop
	 * through the actual client tick (mirroring (1b)'s odometer proof).
	 * ===================================================================== */
	{
		/* --- volume curve: 0 / half-cap / at-cap / above-cap-clamps --- */
		float vol, pitch;
		entity_minecart_roll_volume(0.0f, &vol, &pitch);
		assert(fabsf(vol - 0.25f) < 1e-6f && fabsf(pitch - 0.85f) < 1e-6f
			   && "stationary speed must floor at the quiet/low-pitch end");
		entity_minecart_roll_volume(0.15f, &vol, &pitch); /* half of the 0.3 cap */
		assert(fabsf(vol - 0.475f) < 1e-6f && fabsf(pitch - 1.025f) < 1e-6f
			   && "half-cap speed must sit exactly halfway up both curves");
		entity_minecart_roll_volume(0.3f, &vol, &pitch); /* the throttle cap */
		assert(fabsf(vol - 0.7f) < 1e-6f && fabsf(pitch - 1.2f) < 1e-6f
			   && "at-cap speed must reach the curve's top");
		float vol_over, pitch_over;
		entity_minecart_roll_volume(0.6f, &vol_over, &pitch_over); /* 2x cap */
		assert(fabsf(vol_over - vol) < 1e-6f && fabsf(pitch_over - pitch) < 1e-6f
			   && "above-cap speed must CLAMP, not keep climbing (and a "
				  "negative -0.6f speed must clamp identically via fabsf)");
		entity_minecart_roll_volume(-0.6f, &vol_over, &pitch_over);
		assert(fabsf(vol_over - vol) < 1e-6f && fabsf(pitch_over - pitch) < 1e-6f);

		/* --- inside-sound key selection: pure predicate --- */
		assert(!entity_minecart_wants_inside_sound(false, 0, 0)
			   && "an unoccupied cart must never claim the seat loop");
		assert(entity_minecart_wants_inside_sound(true, 0, 0)
			   && "the local player's OWN seat (matching ids) must claim it");
		assert(!entity_minecart_wants_inside_sound(true, 1, 0)
			   && "a REMOTE rider's cart must never play in this client's ears");
		assert(!entity_minecart_wants_inside_sound(true, 0, 1)
			   && "symmetric check: comparison is occupant_id vs local id, "
				  "not either id alone");

		/* --- real dispatch: drive the actual client tick with a stand-in
		 * local player, prove the seat-loop countdown both fires immediately
		 * on boarding and then counts down, and prove a non-local occupant
		 * resets it to 0 instead. cart is already occupied by
		 * SERVER_LOCAL_PLAYER_ENTITY_ID from the earlier mount section, so
		 * this only needs a stand-in gstate.players[0].local_player to exercise the
		 * "ours" branch -- state is fully restored before returning. --- */
		struct entity* saved_local_player = gstate.players[0].local_player;
		bool saved_snd_1c = gstate.config_minecart_sound;
		struct entity local_player_stub;
		memset(&local_player_stub, 0, sizeof(local_player_stub));
		local_player_stub.id = SERVER_LOCAL_PLAYER_ENTITY_ID;
		gstate.players[0].local_player = &local_player_stub;
		gstate.config_minecart_sound = true;
		assert(cart->data.minecart.occupied
			   && cart->data.minecart.occupant_id == SERVER_LOCAL_PLAYER_ENTITY_ID
			   && "sanity: still riding our own cart from the earlier mount");
		assert(cart->data.minecart.inside_timer == 0.0f
			   && "must start at zero -- never archived, never simulated");
		cart->tick_client(cart);
		assert(cart->data.minecart.inside_timer > 0.0f
			   && "boarding must fire the seat loop IMMEDIATELY, arming the "
				  "countdown, not wait out a stale timer");
		float armed = cart->data.minecart.inside_timer;
		cart->tick_client(cart);
		assert(cart->data.minecart.inside_timer == armed - 1.0f
			   && "the countdown must tick down by exactly one per tick");

		/* A REMOTE rider must reset the countdown, not merely leave it be. */
		cart->data.minecart.occupant_id = 70000;
		cart->tick_client(cart);
		assert(cart->data.minecart.inside_timer == 0.0f
			   && "a cart no longer ours must reset the seat-loop countdown so "
				  "the NEXT boarding fires immediately");

		/* Restore exactly what the rest of this file expects. */
		cart->data.minecart.occupant_id = SERVER_LOCAL_PLAYER_ENTITY_ID;
		cart->data.minecart.inside_timer = 0.0f;
		gstate.players[0].local_player = saved_local_player;
		gstate.config_minecart_sound = saved_snd_1c;
	}

	/* =====================================================================
	 * (2) BACKWARD throttle drives the authoritative cart NEGATIVE.
	 * ===================================================================== */
	send_throttle(cart_id, -1);
	assert(server_local_test_tick_entities(&s));
	cart = only_minecart();
	assert(cart->data.minecart.speed < 0.0f
		   && "backward throttle must move the AUTHORITATIVE cart negative");
	float after_one = cart->data.minecart.speed;

	/* Throttle is applied at ONE deterministic point and CONSUMED: a tick with
	 * no fresh input must not re-apply the old direction. */
	assert(server_local_test_tick_entities(&s));
	cart = only_minecart();
	assert(cart->data.minecart.speed >= after_one
		   && "a dropped input must not keep accelerating (apply-and-clear)");

	/* =====================================================================
	 * (2b) N input messages between two ticks apply N STEPS, not one.
	 * main.c runs up to 3 client ticks per frame off a single input poll, each
	 * staging a step on the client cart and emitting one message. If the server
	 * merely stored the latest, the client cart would pull steadily ahead --
	 * and since the player's reported position follows the CLIENT cart, the gap
	 * eventually crosses the 6-block reach gate and the server silently refuses
	 * every throttle while the authoritative cart coasts toward zero.
	 * ===================================================================== */
	cart->data.minecart.speed = 0.0f;
	send_throttle(cart_id, +1);
	send_throttle(cart_id, +1);
	send_throttle(cart_id, +1);
	assert(server_local_test_tick_entities(&s));
	cart = only_minecart();
	{
		/* 3 steps of 0.01, then Beta's ridden-cart 0.996999979 friction.
		 * One step would leave ~0.00997, which is what overwrite would produce. */
		float expect = 0.03f * 0.996999979019165f;
		assert(fabsf(cart->data.minecart.speed - expect) < 1e-5f
			   && "three input messages between ticks must apply THREE steps");
	}

	/* =====================================================================
	 * (3) FORWARD throttle symmetric.
	 * ===================================================================== */
	cart->data.minecart.speed = 0.0f;
	send_throttle(cart_id, +1);
	assert(server_local_test_tick_entities(&s));
	cart = only_minecart();
	assert(cart->data.minecart.speed > 0.0f);

	/* =====================================================================
	 * (3b) CUMULATIVE PROTOCOL: recovery, dedupe, wrap and barriers.
	 * ===================================================================== */
	{
		cart->data.minecart.speed = 0.0f;
		cart->data.minecart.pending_throttle = 0;
		uint32_t epoch = cart->data.minecart.ride_epoch;
		uint32_t seq = cart->data.minecart.control_seq;
		uint32_t pos = cart->data.minecart.control_positive_steps;
		uint32_t neg = cart->data.minecart.control_negative_steps;

		/* One coalesced sample recovers three lost per-tick deltas. The split
		 * counters retain a +,+,- reversal as net +1; an exact retry is a no-op. */
		send_control_tuple(cart_id, epoch, seq + 3, pos + 2, neg + 1);
		assert(cart->data.minecart.pending_throttle == 1);
		send_control_tuple(cart_id, epoch, seq + 3, pos + 2, neg + 1);
		assert(cart->data.minecart.pending_throttle == 1
			   && "an exact cumulative retry must never double-apply");
		assert(server_local_test_tick_entities(&s));
		assert(cart->data.minecart.speed > 0.0f);

		/* A modularly older packet cannot rewind counters or stage input. */
		cart->data.minecart.pending_throttle = 0;
		cart->data.minecart.snapshot_urgent = false;
		uint32_t accepted = cart->data.minecart.control_seq;
		send_control_tuple(
			cart_id, epoch, accepted - 1,
			cart->data.minecart.control_positive_steps - 1,
			cart->data.minecart.control_negative_steps);
		assert(cart->data.minecart.control_seq == accepted
			   && cart->data.minecart.pending_throttle == 0
			   && cart->data.minecart.snapshot_urgent
			   && "stale control must be ignored and force an authority echo");

		/* Exactly half the uint32 range is order-ambiguous. It is stale for this
		 * session, never a 2^31-step forward jump and never grounds for rotating
		 * toward client-authored state. */
		cart->data.minecart.snapshot_urgent = false;
		uint32_t ambiguous_epoch = cart->data.minecart.ride_epoch;
		uint32_t ambiguous_seq = cart->data.minecart.control_seq;
		send_control_tuple(
			cart_id, ambiguous_epoch,
			ambiguous_seq + UINT32_C(0x80000000),
			cart->data.minecart.control_positive_steps,
			cart->data.minecart.control_negative_steps);
		assert(cart->data.minecart.ride_epoch == ambiguous_epoch
			   && cart->data.minecart.control_seq == ambiguous_seq
			   && cart->data.minecart.pending_throttle == 0
			   && cart->data.minecart.snapshot_urgent
			   && "half-range control must be ignored without rewinding or rotating");

		/* A forward gap beyond the 64-step validity window rotates ownership;
		 * it never synthesizes the missing impulse. */
		cart->data.minecart.snapshot_urgent = false;
		uint32_t old_epoch = cart->data.minecart.ride_epoch;
		send_control_tuple(
			cart_id, old_epoch, cart->data.minecart.control_seq + 65,
			cart->data.minecart.control_positive_steps + 65,
			cart->data.minecart.control_negative_steps);
		assert(cart->data.minecart.ride_epoch != old_epoch
			   && cart->data.minecart.control_seq == 0
			   && cart->data.minecart.pending_throttle == 0
			   && cart->data.minecart.snapshot_urgent);

		/* A tuple that reuses the accepted sequence with different split counters
		 * is corrupt rather than a retry. Rotate the session and synthesize no
		 * impulse; otherwise one forged counter could move the cart without an
		 * accompanying sequence step. */
		uint32_t malformed_epoch = cart->data.minecart.ride_epoch;
		send_control_tuple(cart_id, malformed_epoch, 0, 1, 0);
		assert(cart->data.minecart.ride_epoch != malformed_epoch
			   && cart->data.minecart.control_seq == 0
			   && cart->data.minecart.control_positive_steps == 0
			   && cart->data.minecart.control_negative_steps == 0
			   && cart->data.minecart.pending_throttle == 0
			   && cart->data.minecart.snapshot_urgent
			   && "same-sequence counter mutation must rotate, not apply");

		/* Structurally valid input crossing a pause barrier is terminally
		 * observed but its impulse is discarded. Retrying after resume stays a
		 * duplicate, so it cannot spring to life later. */
		cart->data.minecart.speed = 0.0f;
		s.paused = true;
		send_control_tuple(
			cart_id, cart->data.minecart.ride_epoch, 1, 1, 0);
		assert(cart->data.minecart.control_seq == 1
			   && cart->data.minecart.pending_throttle == 0);
		s.paused = false;
		send_control_tuple(
			cart_id, cart->data.minecart.ride_epoch, 1, 1, 0);
		assert(server_local_test_tick_entities(&s));
		assert(cart->data.minecart.speed == 0.0f
			   && "barrier-discarded input must not replay after resume");

		/* uint32 wrap is ordinary modular progress, not a forced reset. */
		cart->data.minecart.control_seq = UINT32_MAX;
		cart->data.minecart.control_positive_steps = UINT32_MAX;
		cart->data.minecart.control_negative_steps = 0;
		cart->data.minecart.pending_throttle = 0;
		send_control_tuple(
			cart_id, cart->data.minecart.ride_epoch, 1, 1, 0);
		assert(cart->data.minecart.control_seq == 1
			   && cart->data.minecart.pending_throttle == 2
			   && "two wrapped positive steps must be recovered exactly");
		assert(server_local_test_tick_entities(&s));

		/* A tuple can be structurally accepted before the next FIFO message
		 * suspends simulation. It must not sit in pending_throttle and spring to
		 * life after resume. Drive that ordering through the real inbox/update. */
		cart->data.minecart.speed = 0.0f;
		uint32_t barrier_epoch = cart->data.minecart.ride_epoch;
		struct server_rpc before_pause = {
			.type = SRPC_MINECART_CONTROL,
			.payload.minecart_control = {
				.entity_id = cart_id,
				.ride_epoch = barrier_epoch,
				.input_seq = cart->data.minecart.control_seq + 1,
				.positive_steps
					= cart->data.minecart.control_positive_steps + 1,
				.negative_steps
					= cart->data.minecart.control_negative_steps,
			},
		};
		struct server_rpc toggle_pause = {.type = SRPC_TOGGLE_PAUSE};
		assert(svin_rpc_send(&before_pause));
		assert(svin_rpc_send(&toggle_pause));
		server_local_test_update_once(&s);
		assert(s.paused
			   && cart->data.minecart.control_seq
				  == before_pause.payload.minecart_control.input_seq);
		assert(svin_rpc_send(&toggle_pause));
		server_local_test_update_once(&s);
		assert(!s.paused && cart->data.minecart.speed == 0.0f
			   && cart->data.minecart.pending_throttle == 0
			   && "CONTROL -> PAUSE must be terminally discarded, never replayed "
				  "after resume");
	}

	/* =====================================================================
	 * (4) VALIDATION: stale id, out-of-range, non-rider all rejected.
	 * ===================================================================== */
	cart->data.minecart.speed = 0.0f;
	send_throttle(cart_id + 999, +1); /* stale/unknown id */
	assert(server_local_test_tick_entities(&s));
	cart = only_minecart();
	assert(cart->data.minecart.speed == 0.0f
		   && "an unknown cart id must be rejected");

	s.player.x = X + 40.0; /* > 6 blocks: outside Beta's distSq < 36.0 gate */
	send_throttle(cart_id, +1);
	assert(server_local_test_tick_entities(&s));
	cart = only_minecart();
	assert(cart->data.minecart.speed == 0.0f
		   && "an out-of-range throttle must be rejected");
	pub_before = g_pub.count;
	/* F3: dismount ride-intent does NOT check distance -- the rider can always
	 * dismount even when the server's position lags outside the reach gate.
	 * The out-of-range gate applies only to MOUNT, not dismount. */
	send_ride_intent(cart_id, 99, false);
	assert(!cart->data.minecart.occupied
		   && "a dismount ride-intent always dismounts (F3: no distance gate)");
	assert(g_pub.count == pub_before + 1 && !g_pub.last_occupied
		   && "dismount must be published");
	/* Re-mount for the remaining tests. */
	s.player.x = X + 0.5;
	send_ride_intent(cart_id, 100, true);
	assert(cart->data.minecart.occupied
		   && "re-mount after out-of-range dismount");
	s.player.x = X + 0.5;

	/* =====================================================================
	 * (5) DISMOUNT clears occupancy; throttle from a non-rider is rejected.
	 * ===================================================================== */
	pub_before = g_pub.count;
	uint32_t mounted_epoch = cart->data.minecart.ride_epoch;
	send_ride_intent(cart_id, 2, false);
	cart = only_minecart();
	assert(!cart->data.minecart.occupied && "dismount ride-intent dismounts (F3)");
	assert(g_pub.count == pub_before + 1 && !g_pub.last_occupied
		   && "dismount must be published too -- the correction has to work in "
			  "BOTH directions");
	assert(g_pub.last_epoch == 0
		   && cart->data.minecart.ride_epoch == 0
		   && mounted_epoch != 0
		   && g_pub.last_occupant_id == 0
		   && "dismount must retire the mount-scoped epoch and clear the rider id");
	assert(cart->rightClickText && strcmp(cart->rightClickText, "Ride") == 0
		   && "the HUD prompt must follow authoritative dismount");
	cart->data.minecart.speed = 0.0f;
	send_throttle(cart_id, +1); /* not riding any more */
	assert(server_local_test_tick_entities(&s));
	cart = only_minecart();
	assert(cart->data.minecart.speed == 0.0f
		   && "throttle without occupancy must be rejected");

	/* =====================================================================
	 * (6) PERSISTENCE: mounted + negative motion survive archive round-trip.
	 * The archive snapshots the SERVER cart -- which now carries the truth.
	 * ===================================================================== */
	send_ride_intent(cart_id, 3, true); /* re-mount */
	assert(cart->data.minecart.ride_epoch != 0
		   && cart->data.minecart.ride_epoch != mounted_epoch
		   && "a remount must allocate a fresh epoch, never reuse the retired "
			  "session id");
	send_throttle(cart_id, -1);
	assert(server_local_test_tick_entities(&s));
	cart = only_minecart();
	assert(cart->data.minecart.occupied && cart->data.minecart.speed < 0.0f);
	float speed_saved = cart->data.minecart.speed;
	int8_t hx_saved = cart->data.minecart.hx, hz_saved = cart->data.minecart.hz;

	/* Round-trip through the REAL archive snapshot + restore used at save/load
	 * (server_entity_archive_copy via the test seam, then the production
	 * server_local_restore_chunk_entities). */
	struct archived_entity snap;
	server_local_test_archive_entity(cart, &snap);
	uint32_t old_id = cart->id;
	dict_entity_erase(s.entities, old_id);
	free(cart);
	struct chunk_entities batch = {.entities = &snap, .count = 1};
	assert(server_local_restore_chunk_entities(&s, &batch));
	cart = only_minecart();
	assert(fabsf(cart->data.minecart.speed - speed_saved) < 1e-6f
		   && "NEGATIVE speed must survive save/reload");
	/* The restore path REMAPS occupant_source_id through remap_source_id, which
	 * returns UINT32_MAX for any id not in the remap table. Reserved id 0 is
	 * special-cased as the local player; that relationship is deliberately
	 * dropped below for Beta parity. An invented nonzero id would therefore
	 * evaporate rather than aliasing whatever fresh entity got that number. */
	assert(cart->data.minecart.hx == hx_saved
		   && cart->data.minecart.hz == hz_saved
		   && "heading must survive save/reload");
	/* OCCUPANCY MUST **NOT** SURVIVE -- this assertion used to demand the
	 * opposite, which pinned a bug as if it were the contract.
	 * Beta 1.7.3 persists no rider relationship at all: Entity.writeToNBT
	 * [740c583] writes Pos/Motion/Rotation/FallDistance/Fire/Air/OnGround, and
	 * EntityMinecart.writeEntityToNBT adds only Type/PushX/PushZ/Fuel/Items --
	 * there is no "Riding" tag in the whole source. Reload a Beta world and
	 * every cart is empty.
	 * It is not just unfaithful to keep it: the player respawns where they
	 * saved, NOT in the cart, so a restored "ridden" cart is claimed by a body
	 * that is not there. It would keep accepting throttle, and the next mount
	 * click would hit the toggle's dismount arm -- the cart reads as unmountable
	 * until clicked twice. */
	assert(!cart->data.minecart.occupied
		   && "Beta persists NO rider: a reloaded cart must come back EMPTY");
	assert(cart->data.minecart.occupant_id == 0
		   && "a dropped rider claim must clear the occupant id with it");

	/* The real server-spawn -> clin_process boundary must carry the restored
	 * motion state. The old payload stopped at id+position, so this exact seam
	 * constructed a stationary client cart while the restored server cart moved. */
	assert(g_cart_rpc.spawn_count >= 2
		   && g_cart_rpc.last_spawn.type == CRPC_SPAWN_MINECART);
	assert(fabsf(g_cart_rpc.last_spawn.payload.spawn_minecart.speed
				 - speed_saved) < 1e-6f
		   && g_cart_rpc.last_spawn.payload.spawn_minecart.hx == hx_saved
		   && g_cart_rpc.last_spawn.payload.spawn_minecart.hz == hz_saved);
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	particle_init();
	struct client_rpc client_reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {
			.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = SERVER_LOCAL_PLAYER_ENTITY_ID,
		},
	};
	clin_process(&client_reset);
	assert(gstate.players[0].local_player);
	clin_process(&g_cart_rpc.last_spawn);
	struct entity** client_pp = dict_entity_get(gstate.entities, cart->id);
	assert(client_pp && *client_pp && (*client_pp)->type == ENTITY_MINECART);
	struct entity* client_cart = *client_pp;
	assert(fabsf(client_cart->data.minecart.speed - speed_saved) < 1e-6f
		   && client_cart->data.minecart.hx == hx_saved
		   && client_cart->data.minecart.hz == hz_saved
		   && "clin_process must install restored speed+heading, not constructor "
			  "zeroes");

	/* Epoch zero means no ownership session. Reject a malformed state that tries
	 * to pair it with an occupant even when the client's current epoch is also
	 * zero; accepting it would pin the player to an uncontrollable cart. */
	struct client_rpc invalid_zero_epoch = g_cart_rpc.last_spawn;
	invalid_zero_epoch.type = CRPC_MINECART_SNAPSHOT;
	invalid_zero_epoch.payload.minecart_snapshot.ride_epoch = 0;
	invalid_zero_epoch.payload.minecart_snapshot.occupied = true;
	invalid_zero_epoch.payload.minecart_snapshot.occupant_id = 70000;
	invalid_zero_epoch.payload.minecart_snapshot.pos[0]
		= client_cart->pos[0] + 99.0f;
	float before_invalid_x = client_cart->pos[0];
	clin_process(&invalid_zero_epoch);
	assert(client_cart->pos[0] == before_invalid_x
		   && !client_cart->data.minecart.occupied
		   && client_cart->data.minecart.ride_epoch == 0
		   && "epoch-zero occupied state must fail closed as structurally invalid");
	struct client_rpc invalid_spawn = g_cart_rpc.last_spawn;
	invalid_spawn.payload.spawn_minecart.entity_id = 70001;
	invalid_spawn.payload.spawn_minecart.ride_epoch = 0;
	invalid_spawn.payload.spawn_minecart.occupied = true;
	invalid_spawn.payload.spawn_minecart.occupant_id = 70000;
	clin_process(&invalid_spawn);
	assert(!dict_entity_get(gstate.entities, 70001)
		   && "a malformed occupied+epoch-zero spawn must be dropped without "
			  "leaving a dictionary slot");

	/* Widened occupancy is part of the same wire migration. A future nonzero
	 * player id must not truncate to uint8. */
	struct client_rpc wide_occupancy = {
		.type = CRPC_MINECART_OCCUPANCY,
		.payload.minecart_occupancy = {
			.entity_id = client_cart->id,
			.occupant_id = 70000,
			/* Zero is a sentinel, not a serial predecessor. Prove an empty client
			 * accepts the full nonzero uint32 epoch domain rather than rejecting the
			 * upper half as "older than zero". */
			.ride_epoch = UINT32_MAX,
			.occupied = true,
		},
	};
	clin_process(&wide_occupancy);
	assert(client_cart->data.minecart.occupant_id == 70000
		   && client_cart->data.minecart.occupied
		   && client_cart->data.minecart.ride_epoch == UINT32_MAX);

	/* A full-state snapshot hard-corrects every physical field while preserving
	 * the old interpolation offset when no unacknowledged input needs replay. */
	client_cart->pos[0] = 10.0f;
	client_cart->pos_old[0] = 9.25f;
	struct client_rpc correction = g_cart_rpc.last_spawn;
	correction.type = CRPC_MINECART_SNAPSHOT;
	correction.payload.minecart_snapshot.ride_epoch
		= client_cart->data.minecart.ride_epoch;
	correction.payload.minecart_snapshot.occupant_id = 70000;
	correction.payload.minecart_snapshot.occupied = true;
	correction.payload.minecart_snapshot.pos[0] = 20.0f;
	correction.payload.minecart_snapshot.vel[0] = 0.125f;
	correction.payload.minecart_snapshot.accepted_input_seq
		= client_cart->data.minecart.control_seq;
	correction.payload.minecart_snapshot.accepted_positive_steps
		= client_cart->data.minecart.control_positive_steps;
	correction.payload.minecart_snapshot.accepted_negative_steps
		= client_cart->data.minecart.control_negative_steps;
	clin_process(&correction);
	assert(client_cart->pos[0] == 20.0f
		   && fabsf(client_cart->pos_old[0] - 19.25f) < 1e-6f
		   && client_cart->vel[0] == 0.125f
		   && "snapshot correction must translate interpolation history instead "
			  "of snapping it to zero");

	/* A bounded locally-authored suffix is replayed once from the authority's
	 * baseline and stays dirty until a later snapshot acknowledges that tuple. */
	client_cart->data.minecart.control_seq = 1;
	client_cart->data.minecart.control_positive_steps = 1;
	client_cart->data.minecart.control_negative_steps = 0;
	client_cart->data.minecart.control_dirty = true;
	struct client_rpc replay = g_cart_rpc.last_spawn;
	replay.type = CRPC_MINECART_SNAPSHOT;
	replay.payload.minecart_snapshot.ride_epoch
		= client_cart->data.minecart.ride_epoch;
	replay.payload.minecart_snapshot.occupied = true;
	replay.payload.minecart_snapshot.occupant_id = 70000;
	replay.payload.minecart_snapshot.accepted_input_seq = 0;
	replay.payload.minecart_snapshot.accepted_positive_steps = 0;
	replay.payload.minecart_snapshot.accepted_negative_steps = 0;
	/* Exercise the CLIENT dispatch path against a real client-world rail. Never
	 * point this entity at s.world: struct server_world and struct world are
	 * unrelated layouts, and entity_get_block dispatches solely on on_server. */
	int replay_x = (int)floorf(replay.payload.minecart_snapshot.pos[0]);
	int replay_y = (int)floorf(replay.payload.minecart_snapshot.pos[1] - 0.1f);
	int replay_z = (int)floorf(replay.payload.minecart_snapshot.pos[2]);
	world_set_block(
		&gstate.world, replay_x, replay_y - 1, replay_z,
		(struct block_data) {.type = BLOCK_STONE, .sky_light = 15}, false);
	world_set_block(
		&gstate.world, replay_x, replay_y, replay_z,
		(struct block_data) {
			.type = BLOCK_RAIL,
			.metadata = replay.payload.minecart_snapshot.rail_meta,
			.sky_light = 15,
		},
		false);
	clin_process(&replay);
	assert(client_cart->data.minecart.control_dirty
		   && client_cart->data.minecart.control_acked_seq == 0
		   && client_cart->data.minecart.pending_throttle == 0
		   && client_cart->data.minecart.replay_throttle == 1
		   && client_cart->pos[0] == replay.payload.minecart_snapshot.pos[0]
		   && client_cart->pos[1] == replay.payload.minecart_snapshot.pos[1]
		   && client_cart->pos[2] == replay.payload.minecart_snapshot.pos[2]
		   && "the RPC handler must stage unacknowledged input without running "
			  "an eager extra cart tick");
	/* Simulate the local-player entity being visited first in hash order. The
	 * just-authored current input must remain for the next tick rather than
	 * collapsing into the historical replay. Cart-first order produces the same
	 * post-pass state because the player authors this value afterwards. */
	client_cart->data.minecart.pending_throttle = -1;
	assert(client_cart->tick_client && !client_cart->tick_client(client_cart));
	assert(client_cart->data.minecart.replay_throttle == 0
		   && client_cart->data.minecart.pending_throttle == -1
		   && "the upcoming ordinary client tick must consume the replay suffix "
			  "exactly once without consuming same-pass current input");
	assert(!client_cart->tick_client(client_cart));
	assert(client_cart->data.minecart.pending_throttle == 0);
	struct client_rpc replay_ack = replay;
	replay_ack.payload.minecart_snapshot.accepted_input_seq = 1;
	replay_ack.payload.minecart_snapshot.accepted_positive_steps = 1;
	for(int axis = 0; axis < 3; axis++) {
		replay_ack.payload.minecart_snapshot.pos[axis] = client_cart->pos[axis];
		replay_ack.payload.minecart_snapshot.vel[axis] = client_cart->vel[axis];
	}
	replay_ack.payload.minecart_snapshot.speed = client_cart->data.minecart.speed;
	replay_ack.payload.minecart_snapshot.hx = client_cart->data.minecart.hx;
	replay_ack.payload.minecart_snapshot.hz = client_cart->data.minecart.hz;
	clin_process(&replay_ack);
	assert(!client_cart->data.minecart.control_dirty
		   && client_cart->data.minecart.control_acked_seq == 1
		   && "authoritative acknowledgement must retire the cumulative suffix");

	/* A same-world replacement body leaves the entity dictionary alive. Retire
	 * the old body's authored tuple to its acknowledged baseline so a delayed
	 * same-epoch snapshot cannot reconstruct and replay abandoned input. */
	client_cart->data.minecart.control_seq = 2;
	client_cart->data.minecart.control_positive_steps = 2;
	client_cart->data.minecart.control_negative_steps = 0;
	client_cart->data.minecart.control_dirty = true;
	client_cart->data.minecart.control_ready = true;
	struct client_rpc fresh_body = {
		.type = CRPC_PLAYER_POS,
		.payload.player_pos = {
			.position = {
				replay.payload.minecart_snapshot.pos[0],
				replay.payload.minecart_snapshot.pos[1],
				replay.payload.minecart_snapshot.pos[2],
			},
			.rotation = {0.0f, 0.0f},
			.input_epoch = 77,
			.selected_hotbar = 0,
			.fresh_body = true,
		},
	};
	clin_process(&fresh_body);
	assert(client_cart->data.minecart.control_seq == 1
		   && client_cart->data.minecart.control_positive_steps == 1
		   && client_cart->data.minecart.control_negative_steps == 0
		   && !client_cart->data.minecart.control_ready
		   && !client_cart->data.minecart.control_dirty);
	struct client_rpc after_fresh = replay_ack;
	clin_process(&after_fresh);
	assert(!client_cart->data.minecart.control_dirty
		   && client_cart->data.minecart.pending_throttle == 0
		   && client_cart->data.minecart.replay_throttle == 0
		   && "a delayed snapshot after fresh-body cleanup must not resurrect "
			  "the dead body's abandoned suffix");
	/* And it must be immediately mountable again -- one click, not two. */
	send_ride_intent(cart->id, 4, true);
	assert(cart->data.minecart.occupied
		   && "a reloaded cart must mount on the FIRST ride-intent (F3)");

	/* =====================================================================
	 * (7) MOUNTING A SECOND CART RELEASES THE FIRST.
	 * Beta's Entity.mountEntity releases the previous vehicle before seating
	 * the new one, so a player never claims two. It matters here beyond parity:
	 * the CLIENT finds "the cart I am riding" by scanning dict_entity and taking
	 * the first match, and dict_entity is a hash map whose iteration order is
	 * unspecified -- so two simultaneously-claimed carts make the ride target
	 * genuinely nondeterministic. Releasing on mount removes the ambiguity at
	 * its source.
	 * ===================================================================== */
	{
		/* NB: cart_id is stale here -- section 6 erased the original entity and
		 * the archive restore re-created it under a fresh id. Use the live one. */
		const uint32_t first_id = cart->id;
		const int BX = 7, BZ = 7;
		set(BX, YF, BZ, BLOCK_STONE, 0);
		set(BX, RY, BZ, BLOCK_RAIL, 0);
		assert(place_cart_on(BX, RY, BZ));
		struct entity* second = NULL;
		dict_entity_it_t it2;
		dict_entity_it(it2, s.entities);
		while(!dict_entity_end_p(it2)) {
			struct entity* c = dict_entity_ref(it2)->value;
			if(c && c->type == ENTITY_MINECART && c->id != first_id)
				second = c;
			dict_entity_next(it2);
		}
		assert(second && "fixture must create a SECOND distinct cart");

		struct entity** first_pp = dict_entity_get(s.entities, first_id);
		assert(first_pp && *first_pp);
		/* Ensure the first cart is the one currently claimed. */
		if(!(*first_pp)->data.minecart.occupied)
			send_ride_intent(first_id, 5, true);
		assert((*first_pp)->data.minecart.occupied);

		s.player.x = BX + 0.5;
		s.player.z = BZ + 0.5;
		s.player.y = RY + 2.0;
		send_ride_intent(second->id, 6, true);
		assert(second->data.minecart.occupied
			   && "the second cart must be mounted");
		assert(!(*first_pp)->data.minecart.occupied
			   && "mounting a second cart must RELEASE the first -- two carts "
				  "may never name the same rider");
	}

	/* =====================================================================
	 * (8) A RIDER MAKES NO FOOTSTEPS.
	 * Beta gates the footstep odometer on `this.ridingEntity == null`
	 * [740c583 Entity.moveEntity:478]; that clause was missing, and it only
	 * became audible once the player's position began following the cart --
	 * every position update while riding is a genuine delta, so the odometer
	 * crossed every few blocks of track and the cart clattered along on
	 * footstep sounds, faster the quicker it rolled.
	 * Beta 1.7.3 plays NO minecart sound of its own (no playSound anywhere in
	 * EntityMinecart, no "minecart.*" sound name in the decompile), so SILENCE
	 * is the faithful result -- this asserts silence, not a substitute sound.
	 * ===================================================================== */
	{
		struct entity** rp = dict_entity_get(s.entities, cart->id);
		assert(rp && *rp);

		/* Solid ground to walk on, beside the rail. */
		/* Stay inside the single loaded chunk (0..15). */
		for(int i = 0; i < 12; i++)
			set(X + 1, YF, Z + i, BLOCK_STONE, 0);

		/* Feed a walk as a sequence of PLAYER_POS updates. */
		s.player.on_ground = true;
		s.player.sneaking = false;
		s.player.x = X + 1.5;
		s.player.z = Z + 0.5;
		s.player.y = YF + 1 + PLAYER_EYE_HEIGHT;
#define WALK_ONCE()                                                            	do {                                                                       		for(int i = 1; i <= 14; i++) {                                         			struct server_rpc mv = {                                           				.type = SRPC_PLAYER_POS,                                        				.payload.player_pos.x = (float)(X + 1.5),                       				.payload.player_pos.y = (float)(YF + 1 + PLAYER_EYE_HEIGHT),    				.payload.player_pos.z = (float)(Z + 0.5 + i * 0.5),             				.payload.player_pos.on_ground = true,                           			};                                                                  			server_local_test_process_rpc(&s, &mv);                             		}                                                                       	} while(0)

		/* POSITIVE CONTROL FIRST. Without this the "no footsteps" assertion
		 * below passes vacuously whenever the harness fails to generate any
		 * footsteps at all -- which is exactly what the first version of this
		 * test did, and the fail-before caught it. */
		/* Section 7 left the SECOND cart mounted, so dismount whichever cart
		 * actually holds the rider -- not just this one. */
		for(int guard = 0; guard < 8 && s.player.riding_minecart; guard++) {
			uint32_t occ = 0;
			dict_entity_it_t oit;
			dict_entity_it(oit, s.entities);
			while(!dict_entity_end_p(oit)) {
				struct entity* c = dict_entity_ref(oit)->value;
				if(c && c->type == ENTITY_MINECART
				   && c->data.minecart.occupied)
					occ = c->id;
				dict_entity_next(oit);
			}
			if(!occ)
				break;
			send_ride_intent(occ, 7, false);
		}
		assert(!s.player.riding_minecart);
		g_sounds = 0;
		WALK_ONCE();
		assert(g_sounds > 0
			   && "CONTROL: walking on foot MUST emit footsteps, else the "
				  "silence assertion below proves nothing");

		/* Now the real property: the identical walk while RIDING is silent. */
		s.player.x = X + 1.5;
		s.player.z = Z + 0.5;
		send_ride_intent(cart->id, 8, true);
		assert((*rp)->data.minecart.occupied && s.player.riding_minecart);
		g_sounds = 0;
		WALK_ONCE();
		assert(g_sounds == 0
			   && "a RIDING player must emit no footsteps -- Beta gates the "
				  "odometer on ridingEntity == null, and a Beta minecart makes "
				  "no sound of its own");
	}

	/* =====================================================================
	 * (10) VEHICLE BREAKING [740c583 EntityMinecart.attackEntityFrom:70-85].
	 * Each punch: shake=10, damage += strength*10 (fist=1 -> +10), decay
	 * 1/tick. Five RAPID punches (no ticks between) reach 50 > 40: the cart
	 * dismounts its rider, dies, and drops the minecart item. The player is
	 * still MOUNTED from section 8, so this also pins break-while-ridden.
	 * ===================================================================== */
	{
		struct entity** rp = dict_entity_get(s.entities, cart->id);
		assert(rp && *rp && (*rp)->data.minecart.occupied
			   && s.player.riding_minecart);
		uint32_t victim = cart->id;
		struct server_rpc atk = {
			.type = SRPC_ENTITY_ATTACK,
			.payload.entity_attack.entity_id = victim,
		};
		/* Two hits: shaken and damaged but ALIVE (20 <= 40). */
		server_local_test_process_rpc(&s, &atk);
		server_local_test_process_rpc(&s, &atk);
		rp = dict_entity_get(s.entities, victim);
		assert(rp && *rp && "two punches must NOT break a cart");
		assert((*rp)->data.minecart.shake_ticks == 10
			   && (*rp)->data.minecart.damage_taken == 20
			   && "each punch arms the shake and adds fist*10 damage");
		/* Three more without ticks (no decay): 50 > 40 -> break. */
		server_local_test_process_rpc(&s, &atk);
		server_local_test_process_rpc(&s, &atk);
		server_local_test_process_rpc(&s, &atk);
		assert(!dict_entity_get(s.entities, victim)
			   && "five rapid punches must BREAK the cart");
		assert(!s.player.riding_minecart
			   && "breaking a ridden cart must dismount the rider");
		bool item_dropped = false;
		dict_entity_it_t bit;
		dict_entity_it(bit, s.entities);
		while(!dict_entity_end_p(bit)) {
			struct entity* de = dict_entity_ref(bit)->value;
			if(de && de->type == ENTITY_ITEM
			   && de->data.item.item.id == ITEM_MINECART)
				item_dropped = true;
			dict_entity_next(bit);
		}
		assert(item_dropped && "the broken cart must drop its item");
	}

	/* =====================================================================
	 * (11) CLICK -> RPC: the client cart's onLeftClick must QUEUE
	 * SRPC_ENTITY_ATTACK through the real svin transport. screen_ingame
	 * never sent it (the handler's own comment claimed it did), so the
	 * authoritative break branch above was dead code in-game while this
	 * suite -- which injects the RPC directly in section 10 -- stayed
	 * green. Drive the production click handler on a client-side proxy of
	 * a fresh cart and verify the queued RPC lands on the AUTHORITATIVE
	 * entity through server_local's real drain.
	 * ===================================================================== */
	{
		assert(place_cart_on(X, RY, 10));
		/* Section 7's second cart (at (7,7)) is still live, so only_minecart()
		 * is off-limits here -- find the fresh one by where it sits. */
		struct entity* fresh = NULL;
		dict_entity_it_t fit;
		dict_entity_it(fit, s.entities);
		while(!dict_entity_end_p(fit)) {
			struct entity* c = dict_entity_ref(fit)->value;
			if(c && c->type == ENTITY_MINECART
			   && fabsf(c->pos[0] - (X + 0.5f)) < 1.0f
			   && fabsf(c->pos[2] - 10.5f) < 1.0f)
				fresh = c;
			dict_entity_next(fit);
		}
		assert(fresh && "fixture must place the fresh cart at (X, 10)");
		uint32_t fresh_id = fresh->id;

		/* Inside the 36.0 distSq gate, empty hotbar -> fist strength 1. */
		s.player.x = X + 0.5;
		s.player.y = RY + 2.0;
		s.player.z = 10.5;

		/* The client-side twin: poisoned malloc memory constructed exactly
		 * as s_clin_alloc builds it on CRPC_SPAWN_MINECART. */
		struct entity proxy;
		memset(&proxy, 0xBE, sizeof(proxy));
		entity_minecart(fresh_id, &proxy, false, &s.world);
		assert(proxy.onLeftClick && "a client cart must have a click handler");

		assert(proxy.onLeftClick(&proxy));
		assert(fresh->data.minecart.damage_taken == 0
			   && "click prediction runs on the CLIENT cart; the authoritative "
				  "cart must change only via the queued RPC");
		server_local_test_process_messages(&s);
		struct entity** rp = dict_entity_get(s.entities, fresh_id);
		assert(rp && *rp);
		assert((*rp)->data.minecart.damage_taken == 10
			   && (*rp)->data.minecart.shake_ticks == 10
			   && "a left click must reach the server as SRPC_ENTITY_ATTACK "
				  "(fist = +10 damage, shake armed)");
	}

	/* =====================================================================
	 * (12) SNAPSHOT SCHEDULER: two-attempt ceiling, retry fairness, and exact
	 * ridden/background starting cadences.
	 * ===================================================================== */
	{
		/* Ensure at least three live carts so a failed lowest-id attempt cannot
		 * hide unfair cursor behavior behind a one-cart fixture. */
		set(7, YF, 11, BLOCK_STONE, 0);
		set(7, RY, 11, BLOCK_RAIL, 0);
		assert(place_cart_on(7, RY, 11));

		struct entity* carts[8] = {0};
		size_t cart_count = 0;
		dict_entity_it_t cit;
		dict_entity_it(cit, s.entities);
		while(!dict_entity_end_p(cit)) {
			struct entity* e = dict_entity_ref(cit)->value;
			if(e && e->type == ENTITY_MINECART
			   && cart_count < sizeof(carts) / sizeof(carts[0]))
				carts[cart_count++] = e;
			dict_entity_next(cit);
		}
		assert(cart_count >= 3);

		g_drain_run = 0;
		thread_join(&drain);
		do {
			clin_drain_discard();
		} while(!clin_rpc_flush_pending());
		clin_drain_discard();
		for(size_t i = 0; i < cart_count; i++) {
			carts[i]->data.minecart.occupied = false;
			carts[i]->data.minecart.snapshot_dirty = false;
			carts[i]->data.minecart.snapshot_urgent = false;
			carts[i]->data.minecart.snapshot_last_tick = s.world_time;
		}

		/* Exhaust all client RPC tokens. A failed attempt stays due but advances
		 * fairness, so after capacity returns all three carts eventually publish. */
		unsigned filler_count = 0;
		for(;;) {
			struct client_rpc filler = {
				.type = CRPC_TIME_SET,
				.payload.time_set = (uint64_t)filler_count,
			};
			if(!clin_try_rpc_send(&filler))
				break;
			filler_count++;
			assert(filler_count < 128
				   && "client RPC pool must remain finitely bounded");
		}
		assert(filler_count > 0);
		for(size_t i = 0; i < 3; i++) {
			carts[i]->data.minecart.snapshot_dirty = true;
			carts[i]->data.minecart.snapshot_urgent = true;
		}
		unsigned snapshot_before = g_cart_rpc.snapshot_count;
		server_local_test_publish_minecart_snapshots(&s);
		assert(g_cart_rpc.snapshot_count == snapshot_before
			   && "a full client pool must defer rather than block or pretend to "
				  "deliver a minecart snapshot");
		clin_drain_discard();
		server_local_test_publish_minecart_snapshots(&s);
		assert(g_cart_rpc.snapshot_count == snapshot_before + 2
			   && "one scheduler pass may attempt at most two snapshots");
		server_local_test_publish_minecart_snapshots(&s);
		assert(g_cart_rpc.snapshot_count == snapshot_before + 3
			   && "advancing the cursor on failure must let every due cart retry "
				  "without starvation");
		for(size_t i = 0; i < 3; i++)
			assert(!carts[i]->data.minecart.snapshot_urgent);
		clin_drain_discard();

		for(size_t i = 0; i < cart_count; i++) {
			carts[i]->data.minecart.occupied = false;
			carts[i]->data.minecart.snapshot_dirty = false;
			carts[i]->data.minecart.snapshot_urgent = false;
			carts[i]->data.minecart.snapshot_last_tick = s.world_time;
		}
		uint64_t cadence_base = s.world_time;
		carts[0]->data.minecart.occupied = true;
		s.world_time = cadence_base
			+ MINECART_RIDDEN_SNAPSHOT_INTERVAL - 1;
		snapshot_before = g_cart_rpc.snapshot_count;
		server_local_test_publish_minecart_snapshots(&s);
		assert(g_cart_rpc.snapshot_count == snapshot_before);
		s.world_time++;
		server_local_test_publish_minecart_snapshots(&s);
		assert(g_cart_rpc.snapshot_count == snapshot_before + 1
			   && "a ridden cart must first become due at exactly five ticks");
		clin_drain_discard();

		carts[0]->data.minecart.occupied = false;
		carts[0]->data.minecart.snapshot_dirty = true;
		cadence_base = s.world_time;
		carts[0]->data.minecart.snapshot_last_tick = cadence_base;
		s.world_time = cadence_base
			+ MINECART_BACKGROUND_SNAPSHOT_INTERVAL - 1;
		snapshot_before = g_cart_rpc.snapshot_count;
		server_local_test_publish_minecart_snapshots(&s);
		assert(g_cart_rpc.snapshot_count == snapshot_before);
		s.world_time++;
		server_local_test_publish_minecart_snapshots(&s);
		assert(g_cart_rpc.snapshot_count == snapshot_before + 1
			   && !carts[0]->data.minecart.snapshot_dirty
			   && "an unoccupied active cart must publish its terminal/background "
				  "state at exactly twenty ticks");
		clin_drain_discard();

		entity_minecart_mark_snapshot_dirty(carts[0], true);
		assert(carts[0]->data.minecart.snapshot_dirty
			   && carts[0]->data.minecart.snapshot_urgent
			   && "an external impulse must re-enter a stationary cart into the "
				  "correction rotation");

		/* Sustained priority demand must not consume both attempts forever. */
		for(size_t i = 0; i < cart_count; i++) {
			carts[i]->data.minecart.occupied = false;
			carts[i]->data.minecart.snapshot_dirty = false;
			carts[i]->data.minecart.snapshot_urgent = false;
			carts[i]->data.minecart.snapshot_last_tick = s.world_time;
		}
		carts[0]->data.minecart.occupied = true;
		carts[0]->data.minecart.snapshot_urgent = true;
		carts[1]->data.minecart.occupied = true;
		carts[1]->data.minecart.snapshot_urgent = true;
		carts[2]->data.minecart.snapshot_dirty = true;
		carts[2]->data.minecart.snapshot_last_tick
			= s.world_time - MINECART_BACKGROUND_SNAPSHOT_INTERVAL;
		snapshot_before = g_cart_rpc.snapshot_count;
		server_local_test_publish_minecart_snapshots(&s);
		assert(g_cart_rpc.snapshot_count == snapshot_before + 2
			   && !carts[2]->data.minecart.snapshot_dirty
			   && "one background opportunity must survive concurrent priority "
				  "demand");
		clin_drain_discard();
	}

	/* =====================================================================
	 * (13) LEGACY MOB-RIDER RESTORE: the pre-existing archive format can
	 * preserve a non-player rider relation when both records share a chunk.
	 * The relationship is a known Beta deviation, but it must never cross the
	 * wire as occupied + epoch zero: the client rejects that structurally
	 * impossible state and would leave the restored cart permanently stale.
	 * Exercise archive remap -> fresh epoch -> spawn -> clin_process end to end.
	 * ===================================================================== */
	{
		const uint32_t archived_mob_id = 900001;
		struct archived_entity records[2] = {
			{
				.kind = ARCHIVED_ENTITY_MONSTER,
				.source_id = archived_mob_id,
				.pos = {12.25f, RY + 1.0f, 12.25f},
				.health = 20,
				.data.monster = {
					.monster_id = MONSTER_ZOMBIE,
					.fuse = -1,
					.air = 300,
				},
			},
			{
				.kind = ARCHIVED_ENTITY_MINECART,
				.source_id = 900002,
				.pos = {13.5f, RY + 1.0f, 12.5f},
				.data.minecart = {
					.hz = 1,
					.occupied = true,
					.occupant_source_id = archived_mob_id,
				},
			},
		};
		unsigned spawn_before = g_cart_rpc.spawn_count;
		assert(server_local_restore_chunk_entities(
			&s,
			&(struct chunk_entities) {.entities = records, .count = 2}));

		struct entity* restored_mob = NULL;
		struct entity* restored_cart = NULL;
		dict_entity_it_t rit;
		dict_entity_it(rit, s.entities);
		while(!dict_entity_end_p(rit)) {
			struct entity* e = dict_entity_ref(rit)->value;
			if(e && e->type == ENTITY_MONSTER
			   && fabsf(e->pos[0] - 12.25f) < 1e-6f
			   && fabsf(e->pos[2] - 12.25f) < 1e-6f)
				restored_mob = e;
			else if(e && e->type == ENTITY_MINECART
					&& fabsf(e->pos[0] - 13.5f) < 1e-6f
					&& fabsf(e->pos[2] - 12.5f) < 1e-6f)
				restored_cart = e;
			dict_entity_next(rit);
		}
		assert(restored_mob && restored_cart);
		assert(restored_cart->data.minecart.occupied
			   && restored_cart->data.minecart.occupant_id == restored_mob->id
			   && restored_cart->data.minecart.ride_epoch != 0
			   && "a remapped non-player rider must receive a fresh control epoch");
		assert(g_cart_rpc.spawn_count == spawn_before + 1
			   && g_cart_rpc.last_spawn.payload.spawn_minecart.occupied
			   && g_cart_rpc.last_spawn.payload.spawn_minecart.occupant_id
				  == restored_mob->id
			   && g_cart_rpc.last_spawn.payload.spawn_minecart.ride_epoch
				  == restored_cart->data.minecart.ride_epoch
			   && "the restore spawn must publish an internally valid occupied state");

		clin_process(&g_cart_rpc.last_spawn);
		struct entity** restored_client_pp
			= dict_entity_get(gstate.entities, restored_cart->id);
		assert(restored_client_pp && *restored_client_pp
			   && (*restored_client_pp)->data.minecart.occupied
			   && (*restored_client_pp)->data.minecart.occupant_id
				  == restored_mob->id
			   && (*restored_client_pp)->data.minecart.ride_epoch
				  == restored_cart->data.minecart.ride_epoch
			   && "clin_process must accept the fresh nonzero restore epoch");
		clin_drain_discard();
	}


	/* =====================================================================
	 * (14) F3 ZERO/ZERO FORGE FAIL-BEFORE: SRPC_MINECART_RIDE_INTENT with
	 * input_epoch=0 or request_seq=0 must NOT mutate cart occupancy. The
	 * server must reject the forged request without changing state.
	 *
	 * FAIL-BEFORE: on a server that does not check the zero/zero guard,
	 * the forged request would mount the player (or dismount) without a
	 * valid correlation pair, breaking the authority invariant.
	 * ===================================================================== */
	{
		/* Section 7 added a second cart at (7,7); find the original. */
		struct entity* cart = NULL;
		{
			dict_entity_it_t fit;
			dict_entity_it(fit, s.entities);
			while(!dict_entity_end_p(fit)) {
				struct entity* c = dict_entity_ref(fit)->value;
				if(c && c->type == ENTITY_MINECART
				   && !(fabsf(c->pos[0] - 7.5f) < 1.0f
				        && fabsf(c->pos[2] - 7.5f) < 1.0f))
					cart = c;
				dict_entity_next(fit);
			}
		}
		assert(cart);
		/* Dismount if left occupied by prior tests. */
		if(cart->data.minecart.occupied)
			send_ride_intent(cart->id, 200, false);
		assert(cart && !cart->data.minecart.occupied);

		/* Forge a ride-intent with zero epoch (unsolicited form).
		 * Must mutate nothing AND emit nothing. */
		unsigned pub_before = g_pub.count;
		struct server_rpc forge = {
			.type = SRPC_MINECART_RIDE_INTENT,
			.payload.minecart_ride_intent = {
				.entity_id = cart->id,
				.input_epoch = 0,
				.request_seq = 1,
				.desired_occupied = true,
			},
		};
		server_local_test_process_rpc(&s, &forge);
		assert(!cart->data.minecart.occupied
			   && "zero-epoch ride-intent must not mount");
		assert(g_pub.count == pub_before
			   && "zero-epoch ride-intent must emit nothing");

		/* Forge with zero seq. */
		forge.payload.minecart_ride_intent.input_epoch = s.input_epoch_counter;
		forge.payload.minecart_ride_intent.request_seq = 0;
		server_local_test_process_rpc(&s, &forge);
		assert(!cart->data.minecart.occupied
			   && "zero-seq ride-intent must not mount");
		assert(g_pub.count == pub_before
			   && "zero-seq ride-intent must emit nothing");

		/* Forge with both zero. */
		forge.payload.minecart_ride_intent.input_epoch = 0;
		forge.payload.minecart_ride_intent.request_seq = 0;
		server_local_test_process_rpc(&s, &forge);
		assert(!cart->data.minecart.occupied
			   && "zero/zero ride-intent must not mount");
		assert(g_pub.count == pub_before
			   && "zero/zero ride-intent must emit nothing");

		clin_drain_discard();
	}

	/* =====================================================================
	 * (15) F3 SRPC_ENTITY_USE NO-TOGGLE: the legacy SRPC_ENTITY_USE path
	 * must NOT change cart occupancy. It re-publishes unchanged authority
	 * so any stale client toggle prediction is corrected, but the server
	 * cart state is untouched.
	 *
	 * FAIL-BEFORE: on the pre-F3 server, SRPC_ENTITY_USE would toggle
	 * cart occupancy (the old code path that F3 replaces).
	 * ===================================================================== */
	{
		/* Section 7 added a second cart at (7,7); find the original. */
		struct entity* cart = NULL;
		{
			dict_entity_it_t fit;
			dict_entity_it(fit, s.entities);
			while(!dict_entity_end_p(fit)) {
				struct entity* c = dict_entity_ref(fit)->value;
				if(c && c->type == ENTITY_MINECART
				   && !(fabsf(c->pos[0] - 7.5f) < 1.0f
				        && fabsf(c->pos[2] - 7.5f) < 1.0f))
					cart = c;
				dict_entity_next(fit);
			}
		}
		assert(cart);
		assert(cart && !cart->data.minecart.occupied);

		/* Send SRPC_ENTITY_USE targeting the cart. It must re-publish
		 * unchanged authority in the unsolicited form (epoch=0, seq=0). */
		unsigned use_pub_before = g_pub.count;
		use_entity(cart->id);
		assert(!cart->data.minecart.occupied
			   && "SRPC_ENTITY_USE must not toggle cart occupancy (F3)");
		assert(g_pub.count == use_pub_before + 1
			   && "SRPC_ENTITY_USE must emit exactly one healing publication");
		assert(g_pub.last_input_epoch == 0 && g_pub.last_request_seq == 0
			   && "SRPC_ENTITY_USE publication must be the unsolicited "
				  "epoch=0/seq=0 form");

		/* Manually occupy and verify SRPC_ENTITY_USE does not dismount. */
		cart->data.minecart.occupied = true;
		cart->data.minecart.occupant_id = SERVER_LOCAL_PLAYER_ENTITY_ID;
		cart->data.minecart.ride_epoch = 42;
		use_pub_before = g_pub.count;
		use_entity(cart->id);
		assert(cart->data.minecart.occupied
			   && "SRPC_ENTITY_USE must not dismount (F3)");
		assert(cart->data.minecart.ride_epoch == 42
			   && "SRPC_ENTITY_USE must not change ride epoch (F3)");
		assert(g_pub.count == use_pub_before + 1
			   && "SRPC_ENTITY_USE on occupied cart must still emit one "
				  "healing publication");
		assert(g_pub.last_input_epoch == 0 && g_pub.last_request_seq == 0
			   && "occupied SRPC_ENTITY_USE publication must also be "
				  "unsolicited epoch=0/seq=0");

		/* Clean up. */
		cart->data.minecart.occupied = false;
		cart->data.minecart.occupant_id = 0;
		clin_drain_discard();
	}

	/* =====================================================================
	 * (16) F3 RIDE-INTENT MOUNT/DISMOUNT ROUND-TRIP: a valid ride-intent
	 * with nonzero epoch/seq mounts the cart; a subsequent dismount
	 * intent dismounts it. The correlated CRPC_MINECART_OCCUPANCY reply
	 * is emitted for each.
	 * ===================================================================== */
	{
		/* Section 7 added a second cart at (7,7); find the original. */
		struct entity* cart = NULL;
		{
			dict_entity_it_t fit;
			dict_entity_it(fit, s.entities);
			while(!dict_entity_end_p(fit)) {
				struct entity* c = dict_entity_ref(fit)->value;
				if(c && c->type == ENTITY_MINECART
				   && !(fabsf(c->pos[0] - 7.5f) < 1.0f
				        && fabsf(c->pos[2] - 7.5f) < 1.0f))
					cart = c;
				dict_entity_next(fit);
			}
		}
		assert(cart);
		/* Dismount any cart the player is still riding from prior tests. */
		while(s.player.riding_minecart) {
			uint32_t occ = 0;
			dict_entity_it_t oit;
			dict_entity_it(oit, s.entities);
			while(!dict_entity_end_p(oit)) {
				struct entity* c = dict_entity_ref(oit)->value;
				if(c && c->type == ENTITY_MINECART
				   && c->data.minecart.occupied
				   && c->data.minecart.occupant_id
					  == SERVER_LOCAL_PLAYER_ENTITY_ID)
					occ = c->id;
				dict_entity_next(oit);
			}
			if(!occ)
				break;
			send_ride_intent(occ, 200, false);
		}
		clin_drain_discard();
		/* Also clear any carts manually flagged as player-occupied without
		 * going through the authority path (section 15 does this). */
		{
			dict_entity_it_t oit2;
			dict_entity_it(oit2, s.entities);
			while(!dict_entity_end_p(oit2)) {
				struct entity* c = dict_entity_ref(oit2)->value;
				if(c && c->type == ENTITY_MINECART
				   && c->data.minecart.occupied
				   && c->data.minecart.occupant_id
					  == SERVER_LOCAL_PLAYER_ENTITY_ID
				   && c != cart) {
					c->data.minecart.occupied = false;
					c->data.minecart.occupant_id = 0;
				}
				dict_entity_next(oit2);
			}
			s.player.riding_minecart = false;
		}
		clin_drain_discard();
		assert(cart && !cart->data.minecart.occupied);
		/* Position player within reach of the target cart. */
		s.player.x = cart->pos[0];
		s.player.y = cart->pos[1] + 1.0;
		s.player.z = cart->pos[2];

		/* Mount via ride-intent. The correlated reply must echo the
		 * exact nonzero epoch/seq pair. */
		pub_before = g_pub.count;
		struct server_rpc mount_intent = {
			.type = SRPC_MINECART_RIDE_INTENT,
			.payload.minecart_ride_intent = {
				.entity_id = cart->id,
				.input_epoch = s.input_epoch_counter,
				.request_seq = 1,
				.desired_occupied = true,
			},
		};
		server_local_test_process_rpc(&s, &mount_intent);
		assert(cart->data.minecart.occupied
			   && "valid ride-intent must mount");
		assert(cart->data.minecart.ride_epoch != 0
			   && "mount must assign a nonzero ride epoch");
		assert(g_pub.count == pub_before + 1
			   && "mount must emit exactly one occupancy publication");
		assert(g_pub.last_input_epoch == s.input_epoch_counter
			   && g_pub.last_request_seq == 1
			   && "mount reply must echo the exact nonzero correlation pair");

		/* Dismount via ride-intent. */
		pub_before = g_pub.count;
		struct server_rpc dismount_intent = {
			.type = SRPC_MINECART_RIDE_INTENT,
			.payload.minecart_ride_intent = {
				.entity_id = cart->id,
				.input_epoch = s.input_epoch_counter,
				.request_seq = 2,
				.desired_occupied = false,
			},
		};
		server_local_test_process_rpc(&s, &dismount_intent);
		assert(!cart->data.minecart.occupied
			   && "valid dismount intent must dismount");
		assert(g_pub.count == pub_before + 1
			   && "dismount must emit exactly one occupancy publication");
		assert(g_pub.last_input_epoch == s.input_epoch_counter
			   && g_pub.last_request_seq == 2
			   && "dismount reply must echo the exact nonzero correlation pair");

		clin_drain_discard();
	}


	/* =====================================================================
	 * (17) F3 CRITERION #13: target-destroy reset preserves
	 * next_request_seq within the same input epoch. Exercises the REAL
	 * CRPC_ENTITY_DESTROY -> clin_process -> minecart_ride_client_state_reset
	 * (preserve_seq=true) production path, then drives a real onRightClick
	 * authoring on a surviving cart to prove next_request_seq advances
	 * strictly beyond 5 (not a hand-built seq). A full reset (false) is
	 * checked separately.
	 * ===================================================================== */
	{
		/* Seed ride state: a pending intent targeting a doomed cart. */
		gstate.players[0].ride.next_request_seq = 5;
		gstate.players[0].ride.pending_request_seq = 5;
		gstate.players[0].ride.pending = true;
		gstate.players[0].ride.target_id = 999;
		gstate.players[0].ride.input_epoch = 7;

		/* Pass an actual CRPC_ENTITY_DESTROY { entity_id=999 } through
		 * clin_process. The payload-first path runs the reset helper
		 * even when no dict slot exists for 999. */
		struct client_rpc destroy_msg = {
			.type = CRPC_ENTITY_DESTROY,
			.payload.entity_destroy.entity_id = 999,
		};
		clin_process(&destroy_msg);
		assert(!gstate.players[0].ride.pending
			   && "CRPC_ENTITY_DESTROY must clear pending");
		assert(gstate.players[0].ride.target_id == 0
			   && "CRPC_ENTITY_DESTROY must clear target_id");
		assert(gstate.players[0].ride.next_request_seq == 5
			   && "CRPC_ENTITY_DESTROY must preserve next_request_seq "
				  "(criterion #13)");

		/* Invoke a real surviving cart's onRightClick authoring path
		 * and assert pending_request_seq == 6 (strictly newer). */
		assert(gstate.players[0].local_player);
		struct entity* surviving = NULL;
		{
			dict_entity_it_t sit;
			dict_entity_it(sit, gstate.entities);
			while(!dict_entity_end_p(sit)) {
				struct entity* e = dict_entity_ref(sit)->value;
				if(e && e->type == ENTITY_MINECART
				   && e->onRightClick) {
					surviving = e;
					break;
				}
				dict_entity_next(sit);
			}
		}
		assert(surviving && "need a surviving client cart with onRightClick");
		/* This section pins sequence preservation, not foreign-rider
		 * interaction. Make the chosen cart a valid local mount target so the
		 * production ownership guard cannot make the fixture order-dependent. */
		surviving->data.minecart.occupied = false;
		surviving->data.minecart.occupant_id = 0;
		assert(surviving->onRightClick(surviving));
		assert(gstate.players[0].ride.pending_request_seq == 6
			   && "onRightClick must advance to seq 6 after preserve_seq reset");
		assert(gstate.players[0].ride.next_request_seq == 6
			   && "next_request_seq must advance to 6");
		assert(gstate.players[0].ride.pending
			   && "onRightClick must set pending");

		/* World/body reset: preserve_seq=false must zero everything. */
		gstate.players[0].ride.next_request_seq = 5;
		gstate.players[0].ride.pending = true;
		gstate.players[0].ride.target_id = 999;
		minecart_ride_client_state_reset(&gstate.players[0].ride, &gstate.players[0].camera, false);
		assert(gstate.players[0].ride.next_request_seq == 0
			   && "full reset must zero next_request_seq");
		assert(!gstate.players[0].ride.pending
			   && "full reset must clear pending");

		/* FAIL-BEFORE: if the CRPC_ENTITY_DESTROY caller used
	 * preserve_seq=false, next_request_seq would be 0 and
	 * onRightClick would produce seq 1, not 6. */

		clin_drain_discard();
	}

	/* =====================================================================
	 * (18) F3 OUT-OF-RANGE MOUNT REJECTION (authority half of client
	 * Test 4). Position the player beyond Beta's distSq < 36.0 reach
	 * gate, send a valid current-epoch desired=true ride-intent, and
	 * assert: cart occupancy/epoch UNCHANGED, exactly one correlated
	 * publication echoing the request's epoch/seq with unchanged
	 * authority (occupied=false, epoch=0 for an empty cart).
	 *
	 * The existing section (4) only tests out-of-range THROTTLE
	 * rejection, not mount.
	 * ===================================================================== */
	{
		/* Find the original cart (not the section-7 cart at 7,7). */
		struct entity* cart = NULL;
		{
			dict_entity_it_t fit;
			dict_entity_it(fit, s.entities);
			while(!dict_entity_end_p(fit)) {
				struct entity* c = dict_entity_ref(fit)->value;
				if(c && c->type == ENTITY_MINECART
				   && !(fabsf(c->pos[0] - 7.5f) < 1.0f
				        && fabsf(c->pos[2] - 7.5f) < 1.0f))
					cart = c;
				dict_entity_next(fit);
			}
		}
		assert(cart);
		/* Ensure dismounted. */
		if(cart->data.minecart.occupied)
			send_ride_intent(cart->id, 300, false);
		assert(!cart->data.minecart.occupied
			   && "cart must start empty for out-of-range test");

		/* Position player BEYOND 6-block reach (>sqrt(36)=6 blocks). */
		s.player.x = cart->pos[0] + 40.0;
		s.player.y = cart->pos[1] + 1.0 + PLAYER_EYE_HEIGHT;
		s.player.z = cart->pos[2];

		unsigned pub_before = g_pub.count;
		send_ride_intent(cart->id, 301, true);

		/* Cart must remain unoccupied. */
		assert(!cart->data.minecart.occupied
			   && "out-of-range mount must not occupy cart");
		assert(cart->data.minecart.ride_epoch == 0
			   && "out-of-range mount must not change ride_epoch");

		/* Exactly one correlated publication with unchanged authority. */
		assert(g_pub.count == pub_before + 1
			   && "out-of-range mount must emit exactly one publication");
		assert(!g_pub.last_occupied
			   && "out-of-range publication must report occupied=false");
		assert(g_pub.last_epoch == 0
			   && "out-of-range publication must report epoch=0");
		assert(g_pub.last_input_epoch == s.input_epoch_counter
			   && g_pub.last_request_seq == 301
			   && "out-of-range publication must echo request correlation");

		/* Move back in range and verify mount succeeds (negative control). */
		s.player.x = cart->pos[0];
		s.player.y = cart->pos[1] + 1.0 + PLAYER_EYE_HEIGHT;
		s.player.z = cart->pos[2];
		pub_before = g_pub.count;
		send_ride_intent(cart->id, 302, true);
		assert(cart->data.minecart.occupied
			   && "in-range mount must succeed (negative control)");
		assert(g_pub.count == pub_before + 1
			   && "in-range mount must emit exactly one publication");

		/* Dismount for subsequent tests. */
		send_ride_intent(cart->id, 303, false);
		assert(!cart->data.minecart.occupied);
		clin_drain_discard();
	}

	/* =====================================================================
	 * (19) F3 NONEXISTENT-TARGET RIDE INTENT (authority half of client
	 * Test 5 missing-target). Send a ride-intent for an entity_id that
	 * does not exist in the server entity dict. The server must emit
	 * exactly one correlated empty CRPC_MINECART_OCCUPANCY with the
	 * requested target/epoch/seq, and must not crash or mutate state.
	 *
	 * ===================================================================== */
	{
		unsigned pub_before = g_pub.count;
		uint32_t ghost_id = 99998;
		/* Confirm the ghost id is not in the server entity dict. */
		struct entity** pp = dict_entity_get(s.entities, ghost_id);
		assert(!pp && "ghost id must not exist in server entities");

		struct server_rpc ghost_intent = {
			.type = SRPC_MINECART_RIDE_INTENT,
			.payload.minecart_ride_intent = {
				.entity_id = ghost_id,
				.input_epoch = s.input_epoch_counter,
				.request_seq = 401,
				.desired_occupied = true,
			},
		};
		server_local_test_process_rpc(&s, &ghost_intent);

		/* Exactly one correlated empty publication. */
		assert(g_pub.count == pub_before + 1
			   && "nonexistent-target must emit exactly one publication");
		assert(!g_pub.last_occupied
			   && "nonexistent-target publication must be empty");
		assert(g_pub.last_epoch == 0
			   && "nonexistent-target publication must have epoch=0");
		assert(g_pub.last_id == ghost_id
			   && "nonexistent-target publication must echo target id");
		assert(g_pub.last_input_epoch == s.input_epoch_counter
			   && g_pub.last_request_seq == 401
			   && "nonexistent-target publication must echo correlation");

		clin_drain_discard();
	}

	/* =====================================================================
	 * (20) F3 DUPLICATE RPC IDEMPOTENCE: delivering the SAME desired-state
	 * RPC twice must not invert or rotate. The server emits one correlated
	 * publication per request (even duplicates), but occupancy/epoch do not
	 * change on the duplicate -- the already-satisfied request publishes
	 * unchanged authority.
	 *
	 * Duplicate RPC idempotence belongs here, not in the client test:
	 * protocol idempotence means the same SRPC is delivered twice.
	 * ===================================================================== */
	{
		/* Find the original cart. */
		struct entity* cart = NULL;
		{
			dict_entity_it_t fit;
			dict_entity_it(fit, s.entities);
			while(!dict_entity_end_p(fit)) {
				struct entity* c = dict_entity_ref(fit)->value;
				if(c && c->type == ENTITY_MINECART
				   && !(fabsf(c->pos[0] - 7.5f) < 1.0f
				        && fabsf(c->pos[2] - 7.5f) < 1.0f))
					cart = c;
				dict_entity_next(fit);
			}
		}
		assert(cart);
		/* Ensure dismounted and in range. */
		if(cart->data.minecart.occupied)
			send_ride_intent(cart->id, 500, false);
		assert(!cart->data.minecart.occupied);
		s.player.x = cart->pos[0];
		s.player.y = cart->pos[1] + 1.0 + PLAYER_EYE_HEIGHT;
		s.player.z = cart->pos[2];

		/* Mount via desired=true (epoch=current, seq=501). */
		unsigned pub_before = g_pub.count;
		send_ride_intent(cart->id, 501, true);
		assert(cart->data.minecart.occupied
			   && "first mount must succeed");
		assert(g_pub.count == pub_before + 1
			   && "first mount must emit one publication");
		uint32_t mounted_epoch = cart->data.minecart.ride_epoch;
		assert(mounted_epoch != 0 && "mount must assign nonzero epoch");

		/* Deliver the EXACT SAME desired=true request again. */
		pub_before = g_pub.count;
		send_ride_intent(cart->id, 501, true);
		assert(cart->data.minecart.occupied
			   && "duplicate mount must not dismount");
		assert(cart->data.minecart.ride_epoch == mounted_epoch
			   && "duplicate mount must not change ride_epoch");
		assert(g_pub.count == pub_before + 1
			   && "duplicate mount must still emit one publication");
		assert(g_pub.last_occupied
			   && "duplicate mount publication must report occupied=true");
		assert(g_pub.last_epoch == mounted_epoch
			   && "duplicate mount publication must echo same epoch");

		/* Dismount via desired=false (seq=502). */
		pub_before = g_pub.count;
		send_ride_intent(cart->id, 502, false);
		assert(!cart->data.minecart.occupied
			   && "dismount must succeed");
		assert(g_pub.count == pub_before + 1
			   && "dismount must emit one publication");
		assert(cart->data.minecart.ride_epoch == 0
			   && "dismount must retire epoch");

		/* Deliver the EXACT SAME desired=false request again. */
		pub_before = g_pub.count;
		send_ride_intent(cart->id, 502, false);
		assert(!cart->data.minecart.occupied
			   && "duplicate dismount must not mount");
		assert(cart->data.minecart.ride_epoch == 0
			   && "duplicate dismount must not change epoch");
		assert(g_pub.count == pub_before + 1
			   && "duplicate dismount must still emit one publication");
		assert(!g_pub.last_occupied
			   && "duplicate dismount publication must report occupied=false");

		clin_drain_discard();
	}

	/* =====================================================================
	 * (21) F3 SLEEP-GATE LIVENESS: a structurally valid current-session ride
	 * intent must retire while the player sleeps, but it may not mutate the
	 * cart. Invalid zero correlation remains silent. This pins criterion #14:
	 * the general sleep gate must exempt ride intent so the dedicated handler
	 * can publish unchanged correlated authority.
	 * ===================================================================== */
	{
		struct entity* cart = NULL;
		dict_entity_it_t fit;
		dict_entity_it(fit, s.entities);
		while(!dict_entity_end_p(fit)) {
			struct entity* c = dict_entity_ref(fit)->value;
			if(c && c->type == ENTITY_MINECART) {
				cart = c;
				break;
			}
			dict_entity_next(fit);
		}
		assert(cart);
		if(cart->data.minecart.occupied)
			send_ride_intent(cart->id, 600, false);
		assert(!cart->data.minecart.occupied
			   && cart->data.minecart.ride_epoch == 0);

		s.player.sleeping = true;
		unsigned pub_before = g_pub.count;
		struct server_rpc sleeping_mount = {
			.type = SRPC_MINECART_RIDE_INTENT,
			.payload.minecart_ride_intent = {
				.entity_id = cart->id,
				.input_epoch = s.input_epoch_counter,
				.request_seq = 601,
				.desired_occupied = true,
			},
		};
		server_local_test_process_rpc(&s, &sleeping_mount);
		assert(!cart->data.minecart.occupied
			   && cart->data.minecart.ride_epoch == 0
			   && "sleeping ride intent must not mutate occupancy/session");
		assert(g_pub.count == pub_before + 1
			   && "valid sleeping ride intent must publish once");
		assert(g_pub.last_id == cart->id && !g_pub.last_occupied
			   && g_pub.last_epoch == 0
			   && g_pub.last_input_epoch == s.input_epoch_counter
			   && g_pub.last_request_seq == 601
			   && "sleeping publication must echo exact correlation and authority");

		struct server_rpc zero = sleeping_mount;
		zero.payload.minecart_ride_intent.input_epoch = 0;
		zero.payload.minecart_ride_intent.request_seq = 0;
		pub_before = g_pub.count;
		server_local_test_process_rpc(&s, &zero);
		assert(!cart->data.minecart.occupied
			   && cart->data.minecart.ride_epoch == 0);
		assert(g_pub.count == pub_before
			   && "zero/zero sleeping ride intent must emit nothing");

		s.player.sleeping = false;
		clin_drain_discard();
	}
	clin_test_set_rpc_observer(NULL, NULL);
	inventory_destroy(&s.player.inventory, &s.player);
	server_world_destroy(&s.world);
	dict_entity_it_t server_it;
	dict_entity_it(server_it, s.entities);
	while(!dict_entity_end_p(server_it)) {
		free(dict_entity_ref(server_it)->value);
		dict_entity_next(server_it);
	}
	dict_entity_clear(s.entities);
	for(size_t i = 0; i < 256; i++) {
		if(gstate.players[0].windows[i]) {
			windowc_destroy(gstate.players[0].windows[i]);
			free(gstate.players[0].windows[i]);
			gstate.players[0].windows[i] = NULL;
		}
	}
	dict_entity_it_t client_it;
	dict_entity_it(client_it, gstate.entities);
	while(!dict_entity_end_p(client_it)) {
		free(dict_entity_ref(client_it)->value);
		dict_entity_next(client_it);
	}
	gstate.players[0].local_player = NULL;
	dict_entity_clear(gstate.entities);
	world_destroy(&gstate.world);
	printf("test_minecart_authority: server-authoritative mount/throttle/"
		   "persistence OK\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
