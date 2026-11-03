/* ci-1101/03: creative flight, ruled 912c987; design section 1.
 * Survival trace captured BEFORE the implementation at 93ec04d618.
 * This is a regression/equivalence tape, not a Beta faithfulness oracle. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The engine pool links objects directly. Give this consumer copy private
 * entry points so it can substitute PC's always-false toggle boundary. */
#define entity_local_player flight_test_local_player
#define entity_local_player_block_collide flight_test_block_collide
#define entity_local_player_flight_input flight_test_flight_input
#define entity_local_player_flying flight_test_flying
#define entity_local_player_ride_action_eligible flight_test_ride_action_eligible
#define entity_local_player_ride_action_frame flight_test_ride_action_frame
#define entity_local_player_ride_action_frame_end flight_test_ride_action_frame_end
#define entity_local_player_walk_owned flight_test_walk_owned
#define local_player_column_present flight_test_column_present
#define local_player_online_dismount flight_test_online_dismount

#include "block/blocks.h"
#include "entity/entity.h"
#include "entity/entity_local_player.h"
#include "game/game_state.h"
#include "item/items.h"
#include "platform/input_owner.h"
#include "platform/time.h"
#include "platform/input_sneak_toggle.h"

/* PC input deliberately has no toggle. Compile the real flight consumer
 * against the pure controller toggle at its query/release boundary. */
static struct input_sneak_toggle sneak_toggle[2];
static bool flight_test_sneak_latched(unsigned slot) {
	return slot < 2 && sneak_toggle[slot].engaged;
}
static void flight_test_sneak_release(unsigned slot) {
	assert(slot < 2);
	sneak_toggle[slot] = (struct input_sneak_toggle){0};
}
#define input_sneak_toggle_latched flight_test_sneak_latched
#define input_sneak_toggle_release flight_test_sneak_release
#include "../source/entity/entity_local_player.c"
#undef input_sneak_toggle_latched
#undef input_sneak_toggle_release

static struct entity bodies[2];

static void reset_player(unsigned slot) {
	struct client_session* s = &gstate.players[slot];
	memset(s, 0, sizeof(*s));
	struct entity* e = &bodies[slot];
	memset(e, 0, sizeof(*e));
	entity_local_player(91 + slot, e, &gstate.world);
	s->local_player = e;
	s->local_bound = true;
	s->local_generation = 1;
	s->local_phase = CLIENT_GUEST_ATTACHED;
	s->local_gameplay_ready = true;
	e->health = 160;
	e->pos[0] = e->pos[2] = 8.5F;
	e->pos[1] = 70.0F;
	e->data.local_player.capture_input = true;
}

static void tick(unsigned slot, bool jump, bool sneak, bool forward) {
	struct input_player_frame frame = {.connected = true};
	frame.down[IB_JUMP] = jump;
	frame.down[IB_SNEAK] = sneak;
	frame.down[IB_FORWARD] = forward;
	assert(!entity_local_player_walk_owned(&bodies[slot],
		&gstate.players[slot], &frame));
}

static uint32_t bits(float f) {
	uint32_t u;
	memcpy(&u, &f, sizeof(u));
	return u;
}

static const uint32_t survival_golden[48][6] = {
	{0x41080000u, 0x428c0000u, 0x41085048u, 0x00000000u, 0xbda0902eu, 0x3c921cceu},
	{0x41080000u, 0x428bd7dcu, 0x4108e99eu, 0x00000000u, 0xbe1ef523u, 0x3d0b8997u},
	{0x41080000u, 0x428b8861u, 0x4109c570u, 0x00000000u, 0xbe6c0f5du, 0x3d48090du},
	{0x41080000u, 0x428b1259u, 0x410addc1u, 0x00000000u, 0xbe9bcf6au, 0x3d7f16a4u},
	{0x41080000u, 0x428a768au, 0x410c2d20u, 0x00000000u, 0xbec0d5b6u, 0x3d9897e8u},
	{0x41080000u, 0x4289b5b4u, 0x410dae98u, 0x00000000u, 0xbee51e71u, 0x3daf635cu},
	{0x41080000u, 0x4288d096u, 0x410f5da7u, 0x00000000u, 0xbf0456b3u, 0x3dc4219fu},
	{0x41080000u, 0x4287c7e9u, 0x41113632u, 0x00000000u, 0xbf15c326u, 0x3dd701f5u},
	{0x41080000u, 0x42869c63u, 0x4113347eu, 0x00000000u, 0xbf26d663u, 0x3de82f62u},
	{0x41080000u, 0x42854eb6u, 0x41155525u, 0x00000000u, 0xbf379234u, 0x3df7d10bu},
	{0x41080000u, 0x4283df92u, 0x4117950fu, 0x00000000u, 0xbf47f857u, 0x3e030546u},
	{0x41080000u, 0x42833d71u, 0x4119f16cu, 0x00000000u, 0xbda0902eu, 0x3e097e29u},
	{0x41080000u, 0x42833d71u, 0x411da8cdu, 0x00000000u, 0xbda0902eu, 0x3e01dd00u},
	{0x41080000u, 0x42833d71u, 0x412141a9u, 0x00000000u, 0xbda0902eu, 0x3dfb6526u},
	{0x41080000u, 0x42833d71u, 0x4124c9dcu, 0x00000000u, 0xbda0902eu, 0x3df6d89cu},
	{0x41080000u, 0x42833d71u, 0x412848f6u, 0x00000000u, 0xbda0902eu, 0x3df45cc5u},
	{0x41080000u, 0x42833d71u, 0x412bc318u, 0x00000000u, 0xbda0902eu, 0x3df3019au},
	{0x41080000u, 0x42833d71u, 0x412f3a84u, 0x00000000u, 0xbda0902eu, 0x3df2440du},
	{0x41080000u, 0x42833d71u, 0x4132b075u, 0x00000000u, 0xbda0902eu, 0x3df1dc8du},
	{0x41080000u, 0x42833d71u, 0x41362597u, 0x00000000u, 0xbda0902eu, 0x3df1a40bu},
	{0x41080000u, 0x42833d71u, 0x41399a48u, 0x00000000u, 0xbda0902eu, 0x3df18531u},
	{0x41080000u, 0x42833d71u, 0x413d0ebbu, 0x00000000u, 0xbda0902eu, 0x3df17458u},
	{0x41080000u, 0x42833d71u, 0x4140830cu, 0x00000000u, 0xbda0902eu, 0x3df16b25u},
	{0x41080000u, 0x42833d71u, 0x4143f74bu, 0x00000000u, 0xbda0902eu, 0x3df1661fu},
	{0x41080000u, 0x4284147bu, 0x41476b80u, 0x00000000u, 0x3eaa9930u, 0x3df16362u},
	{0x41080000u, 0x4284bf14u, 0x41499e8fu, 0x00000000u, 0x3e7e175du, 0x3e001880u},
	{0x41080000u, 0x42853e20u, 0x414bef39u, 0x00000000u, 0x3e28ba54u, 0x3e06d4c7u},
	{0x41080000u, 0x4285927du, 0x414e5ad4u, 0x00000000u, 0x3daa24b5u, 0x3e0cf5dfu},
	{0x41080000u, 0x4285bd06u, 0x4150def4u, 0x00000000u, 0x3b45aca1u, 0x3e1289bfu},
	{0x41080000u, 0x4285be91u, 0x41537963u, 0x00000000u, 0xbd9a826au, 0x3e179d1du},
	{0x41080000u, 0x428597f0u, 0x41562820u, 0x00000000u, 0xbe1bfdc1u, 0x3e1c3b89u},
	{0x41080000u, 0x428549f1u, 0x4158e956u, 0x00000000u, 0xbe69272bu, 0x3e206f8au},
	{0x41080000u, 0x4284d55du, 0x415bbb5cu, 0x00000000u, 0xbe9a62c2u, 0x3e2442b4u},
	{0x41080000u, 0x42843afau, 0x415e9cafu, 0x00000000u, 0xbebf7059u, 0x3e27bdbeu},
	{0x41080000u, 0x42837b8au, 0x41618beeu, 0x00000000u, 0xbee3c03au, 0x3e2ae897u},
	{0x41080000u, 0x42833d71u, 0x41644fa6u, 0x00000000u, 0xbda0902eu, 0x3e21018au},
	{0x41080000u, 0x42833d71u, 0x41674c18u, 0x00000000u, 0xbda0902eu, 0x3dd0b1a3u},
	{0x41080000u, 0x42833d71u, 0x416965e7u, 0x00000000u, 0xbda0902eu, 0x3d92d27bu},
	{0x41080000u, 0x42833d71u, 0x416b03f8u, 0x00000000u, 0xbda0902eu, 0x3d62149bu},
	{0x41080000u, 0x42833d71u, 0x416c5e79u, 0x00000000u, 0xbda0902eu, 0x3d3d30cau},
	{0x41080000u, 0x42833d71u, 0x416d1baau, 0x00000000u, 0xbda0902eu, 0x3cce989du},
	{0x41080000u, 0x42833d71u, 0x416d82f6u, 0x00000000u, 0xbda0902eu, 0x3c619a5fu},
	{0x41080000u, 0x42833d71u, 0x416dbb5du, 0x00000000u, 0xbda0902eu, 0x3bf65bc8u},
	{0x41080000u, 0x42833d71u, 0x416dda28u, 0x00000000u, 0xbda0902eu, 0x3b868303u},
	{0x41080000u, 0x42833d71u, 0x416deaf8u, 0x00000000u, 0xbda0902eu, 0x3b12e30au},
	{0x41080000u, 0x42833d71u, 0x416df426u, 0x00000000u, 0xbda0902eu, 0x3aa06686u},
	{0x41080000u, 0x42833d71u, 0x416df929u, 0x00000000u, 0xbda0902eu, 0x3a2f2848u},
	{0x41080000u, 0x42833d71u, 0x416dfbe6u, 0x00000000u, 0xbda0902eu, 0x39bf4598u},
};

/* Presses are sampled per rendered frame; no physics tick is necessary. */
static void press(unsigned slot, unsigned ms) {
	struct input_player_frame frame = {.connected = true};
	frame.pressed[IB_JUMP] = true;
	entity_local_player_flight_input(slot, &frame,
		time_add_ms((ptime_t){0}, ms));
}

static void arm(unsigned slot) {
	reset_player(slot);
	press(slot, 1000);
	assert(!entity_local_player_flying(slot));
	press(slot, 1200);
	assert(entity_local_player_flying(slot));
}

static void check_toggle(void) {
	gstate.config_creative = true;
	arm(0);
	/* Catch-up ticks and held jump cannot re-consume the press. */
	for(unsigned t = 0; t < 3; t++) {
		tick(0, true, false, false);
		assert(entity_local_player_flying(0));
		assert(bits(bodies[0].vel[1]) == bits(0.05F));
	}
	press(0, 1400);
	press(0, 1650); /* Inclusive 250 ms boundary. */
	assert(!entity_local_player_flying(0));
	reset_player(0);
	press(0, 1000);
	press(0, 1251);
	assert(!entity_local_player_flying(0));
	reset_player(0);
	bodies[0].on_ground = true;
	press(0, 1000);
	press(0, 1100);
	assert(!entity_local_player_flying(0));
	/* First grounded jump, second airborne edge is the normal entry. */
	reset_player(0);
	bodies[0].on_ground = true;
	press(0, 1000);
	bodies[0].on_ground = false;
	press(0, 1100);
	assert(entity_local_player_flying(0));
	/* Held level without a pressed edge never counts as a tap. */
	reset_player(0);
	struct input_player_frame held = {.connected = true};
	held.down[IB_JUMP] = true;
	for(unsigned ms = 0; ms < 300; ms += 16)
		entity_local_player_flight_input(0, &held,
			time_add_ms((ptime_t){0}, ms));
	assert(!entity_local_player_flying(0));
	/* Pausing or losing capture cancels an unfinished pair. */
	press(0, 1000);
	gstate.paused = true;
	press(0, 1100);
	gstate.paused = false;
	press(0, 1200);
	assert(!entity_local_player_flying(0));
	bodies[0].data.local_player.capture_input = false;
	press(0, 1250);
	bodies[0].data.local_player.capture_input = true;
	press(0, 1300);
	assert(!entity_local_player_flying(0));
	puts("PASS double-tap: frame edges, airborne guard, 250/251 ms, capture");
}

static void check_motion(void) {
	arm(0);
	float y = bodies[0].pos[1];
	tick(0, false, false, false);
	assert(bits(bodies[0].pos[1]) == bits(y));
	assert(bits(bodies[0].vel[1]) == bits(0.0F));
	tick(0, true, false, false);
	assert(bits(bodies[0].pos[1]) == bits(y + 0.05F));
	y = bodies[0].pos[1];
	tick(0, false, true, false);
	assert(bits(bodies[0].pos[1]) == bits(y - 0.05F));
	assert(bits(bodies[0].vel[1]) == bits(-0.05F));
	y = bodies[0].pos[1];
	tick(0, true, true, false);
	assert(bits(bodies[0].pos[1]) == bits(y));
	assert(bits(bodies[0].vel[1]) == bits(0.0F));
	/* Fluids must not reintroduce swim impulses, sink drag or climb-out. */
	const unsigned media[] = {BLOCK_WATER_STILL, BLOCK_LAVA_STILL};
	for(unsigned m = 0; m < 2; m++) {
		for(int x = 7; x <= 9; x++)
			for(int z = 7; z <= 9; z++)
				for(int h = 67; h <= 72; h++)
					world_set_block(&gstate.world, x, h, z,
						(struct block_data){.type = media[m]}, false);
		arm(0);
		y = bodies[0].pos[1];
		tick(0, false, false, false);
		assert(bits(bodies[0].pos[1]) == bits(y));
		assert(bits(bodies[0].vel[1]) == bits(0.0F));
		tick(0, true, false, false);
		assert(bits(bodies[0].vel[1]) == bits(0.05F));
		tick(0, false, true, false);
		assert(bits(bodies[0].vel[1]) == bits(-0.05F));
	}
	for(int x = 7; x <= 9; x++)
		for(int z = 7; z <= 9; z++)
			for(int h = 67; h <= 72; h++)
				world_set_block(&gstate.world, x, h, z,
					(struct block_data){.type = BLOCK_AIR}, false);
	puts("PASS flight: hover, +/-0.05, simultaneous controls, water/lava");
}

static void check_lifecycle(void) {
	arm(0);
	bodies[0].pos[1] = 65.63F; /* Feet just above the stone floor. */
	tick(0, false, true, false);
	assert(bodies[0].on_ground);
	assert(!entity_local_player_flying(0));
	bodies[0].on_ground = false;
	press(0, 1250);
	assert(!entity_local_player_flying(0)); /* no stale pair after landing */
	arm(0);
	bodies[0].health = 0;
	assert(!entity_local_player_flying(0));
	arm(0);
	gstate.players[0].death.active = true;
	assert(!entity_local_player_flying(0));
	arm(0);
	gstate.world.dimension = WORLD_DIM_NETHER;
	assert(!entity_local_player_flying(0));
	gstate.world.dimension = WORLD_DIM_OVERWORLD;
	arm(0);
	gstate.world_switch_pending = true;
	assert(!entity_local_player_flying(0));
	gstate.world_switch_pending = false;
	arm(0);
	tick(0, true, false, false);
	gstate.config_creative = false;
	assert(!entity_local_player_flying(0));
	assert(bits(bodies[0].vel[1]) == bits(0.0F));
	gstate.config_creative = true;
	arm(0);
	gstate.players[0].sleeping = true;
	assert(!entity_local_player_flying(0));
	arm(0);
	gstate.players[0].host_respawn_loading = true;
	assert(!entity_local_player_flying(0));
	arm(0);
	reset_player(0); /* allocator reuses the exact same body address */
	assert(!entity_local_player_flying(0));
	arm(0);
	gstate.world_loaded = false;
	assert(!entity_local_player_flying(0));
	gstate.world_loaded = true;
	puts("PASS landing/death/dimension/mode/sleep/respawn/reload reset");
}

static void check_split(void) {
	reset_player(0);
	reset_player(1);
	press(0, 1000);
	press(1, 1100);
	assert(!entity_local_player_flying(0));
	assert(!entity_local_player_flying(1));
	press(1, 1200);
	assert(entity_local_player_flying(1));
	assert(!entity_local_player_flying(0));
	/* Host portal/loading state cannot drop a guest in another live world. */
	gstate.world_switch_pending = true;
	gstate.world_loaded = false;
	assert(entity_local_player_flying(1));
	tick(1, false, true, false);
	assert(bits(bodies[1].vel[1]) == bits(-0.05F));
	gstate.world_switch_pending = false;
	gstate.world_loaded = true;
	gstate.players[1].local_generation++;
	assert(!entity_local_player_flying(1));
	arm(1);
	gstate.players[1].local_gameplay_ready = false;
	assert(!entity_local_player_flying(1));
	puts("PASS independent split-screen gesture, motion and lifetime");
}

static void sneak_tap(unsigned slot) {
	input_sneak_toggle_step(&sneak_toggle[slot], false, true, false, false);
	input_sneak_toggle_step(&sneak_toggle[slot], true, true, false, false);
	input_sneak_toggle_step(&sneak_toggle[slot], false, true, false, false);
}

static void check_flight_sneak_release(void) {
	for(unsigned slot = 0; slot < 2; slot++) {
		arm(slot);
		sneak_tap(slot);
		float y = bodies[slot].pos[1];
		tick(slot, false, flight_test_sneak_latched(slot), false);
		assert(bits(bodies[slot].pos[1]) == bits(y - 0.05F));
		assert(flight_test_sneak_latched(slot)); /* Keep descending. */
		bodies[slot].pos[1] = 65.63F;
		tick(slot, false, flight_test_sneak_latched(slot), false);
		assert(bodies[slot].on_ground);
		assert(!flight_test_sneak_latched(slot));
		assert(!entity_local_player_flying(slot));

		/* A ground latch belongs to the player, not the flight. */
		reset_player(slot);
		sneak_tap(slot);
		press(slot, 1000);
		press(slot, 1200);
		assert(entity_local_player_flying(slot));
		bodies[slot].pos[1] = 65.63F;
		tick(slot, false, flight_test_sneak_latched(slot), false);
		assert(bodies[slot].on_ground);
		assert(flight_test_sneak_latched(slot));
		/* Switching that latch off and back on in flight transfers it to
		 * flight, so the new descent latch must be released. */
		bodies[slot].on_ground = false;
		press(slot, 1400);
		press(slot, 1600);
		assert(entity_local_player_flying(slot));
		sneak_tap(slot);
		assert(!flight_test_sneak_latched(slot));
		assert(entity_local_player_flying(slot));
		sneak_tap(slot);
		tick(slot, false, flight_test_sneak_latched(slot), false);
		assert(bodies[slot].on_ground);
		assert(!flight_test_sneak_latched(slot));
		flight_test_sneak_release(slot);

		/* Representative flight clears must also release their latch. */
		for(unsigned clear = 0; clear < 6; clear++) {
			arm(slot);
			sneak_tap(slot);
			switch(clear) {
			case 0: bodies[slot].on_ground = true; break;
			case 1: bodies[slot].health = 0; break;
			case 2: gstate.players[slot].local_generation++; break;
			case 3:
				press(slot, 1400);
				press(slot, 1600);
				break;
			case 4: gstate.config_creative = false; break;
			case 5: reset_player(slot); break;
			}
			assert(!entity_local_player_flying(slot));
			assert(!flight_test_sneak_latched(slot));
			gstate.config_creative = true;
		}
	}
	puts("PASS flight-only sneak latch released on landing; ground latch preserved");
}

static void survival_trace(bool remote, bool creative) {
	gstate.remote_session = remote;
	gstate.config_creative = creative;
	reset_player(0);
	for(unsigned t = 0; t < 48; t++) {
		press(0, 1000 + t * 50); /* Repeated double taps stay inert. */
		tick(0, t >= 24 && t < 27, t >= 35, t < 40);
		assert(!entity_local_player_flying(0));
		const float* vectors[] = {bodies[0].pos, bodies[0].vel};
		for(unsigned v = 0; v < 2; v++)
			for(unsigned axis = 0; axis < 3; axis++) {
				uint32_t got = bits(vectors[v][axis]);
				uint32_t want = survival_golden[t][v * 3 + axis];
				if(got != want)
					fprintf(stderr, "FAIL remote=%d creative=%d tick=%u component=%u: %08x != %08x\n",
						remote, creative, t, v * 3 + axis, got, want);
				assert(got == want);
			}
	}
	printf("PASS 48-tick pre-edit position/velocity tape remote=%d creative=%d\n",
		remote, creative);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 32; z++)
			for(int y = 63; y < 80; y++)
				world_set_block(&gstate.world, x, y, z,
					(struct block_data){.type = y == 63 ? BLOCK_STONE : BLOCK_AIR}, false);
	gstate.world_loaded = true;
	survival_trace(false, false);
	survival_trace(true, false);
	survival_trace(true, true); /* Stale creative bit must never enable MP. */
	gstate.remote_session = false;
	check_toggle();
	check_motion();
	check_lifecycle();
	check_split();
	check_flight_sneak_release();
	puts("PASS test_creative_flight");
	return 0;
}
