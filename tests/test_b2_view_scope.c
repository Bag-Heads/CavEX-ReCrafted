/* Split-dims B2 (B2-MAINC-PLAN section 0 + Fable R2-2), through the real
 * client consumer:
 *  T0 view scope -- clin_world_view_enter/leave: flag OFF returns the gstate
 *     objects and re-selects nothing (world_view_current() stays
 *     &gstate.world); flag ON maps a view to the payload its context shows,
 *     selects that payload's particle / sign / piston stores and the
 *     world_view_* accessor the renderers and entity code read, snapshots
 *     its clock, nests, and restores every selection on leave.
 *  T5 guest-world sound -- audio_play3d is --wrap'ed: flag OFF plays every
 *     SOUND at its position; flag ON plays a view-0 world's sound as is, a
 *     guest-only world's sound moved by (camera0 - camera1) (heard at the
 *     guest's distance by the view-0 listener), and drops a world no view
 *     shows. Synthetic stamps: mechanism only. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "game/client_world_context.h"
#include "game/game_state.h"
#include "game/sign_text_store.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/client_world_payload.h"
#include "network/piston_moving_transport.h"
#include "network/server_interface.h"
#include "particle.h"
#include "world.h"

#define LOCAL_EID 77

/* ---- audio_play3d wrap: record the last positional play ---------------- */
void __real_audio_play3d(uint16_t sound_id, vec3 pos, float vol, float pitch);
static unsigned s_plays;
static float s_last[3];
void __wrap_audio_play3d(uint16_t sound_id, vec3 pos, float vol, float pitch) {
	s_plays++;
	s_last[0] = pos[0];
	s_last[1] = pos[1];
	s_last[2] = pos[2];
	(void)sound_id; (void)vol; (void)pitch;
}

static struct client_ctx_registry* reg;

static void world_reset(uint8_t index, uint32_t generation) {
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = index ? WORLD_DIM_NETHER
												  : WORLD_DIM_OVERWORLD,
			.local_entity = LOCAL_EID, .world_index = index,
			.world_generation = generation}});
}

static struct clin_world_payload payload(unsigned slot) {
	struct clin_world_payload p;
	assert(clin_world_payload_of_slot(slot, &p));
	return p;
}

static void flag(bool on) {
	client_ctx_registry_retire(reg, 0);
	client_ctx_registry_retire(reg, 1);
	assert(clin_world_payloads_set_enabled(false));
	assert(clin_world_payloads_set_enabled(on));
}

/* Guest view (1) on the overworld (slot 0), player 0 (view 0) in the
 * Nether (slot 1). */
static void two_worlds(void) {
	flag(true);
	world_reset(0, 100);
	world_reset(1, 200);
	assert(clin_world_payload_bind_view(0, 1));
}

static bool defaults_selected(void) {
	return particle_system_selected() == particle_system_primary()
		&& world_view_current() == &gstate.world
		&& world_view_entities() == gstate.entities;
}

static void test_view_scope(void) {
	flag(false);
	struct clin_view_world v;
	assert(clin_world_view_enter(0, &v) && !v.selected);
	assert(v.terrain == &gstate.world && v.entities == gstate.entities
		   && v.dimension == gstate.world.dimension);
	assert(defaults_selected() && "OFF: nothing re-selected");
	clin_world_view_leave(&v);
	assert(defaults_selected());
	assert(clin_world_view_enter(1, &v) && v.terrain == &gstate.world);
	clin_world_view_leave(&v);

	two_worlds();
	gstate.world_time = 1;
	clin_process(&(struct client_rpc) {.type = CRPC_TIME_SET, .world_index = 0,
		.world_generation = 100, .payload.time_set = 6000});
	clin_process(&(struct client_rpc) {.type = CRPC_TIME_SET, .world_index = 1,
		.world_generation = 200, .payload.time_set = 18000});
	struct clin_view_world v0, v1;
	assert(clin_world_view_enter(0, &v0) && v0.selected && v0.slot == 1);
	assert(v0.terrain == payload(1).terrain && v0.dimension == WORLD_DIM_NETHER
		   && v0.clock.world_time == 18000);
	assert(particle_system_selected() == payload(1).particles
		   && world_view_current() == payload(1).terrain
		   && world_view_entities() == payload(1).entities
		   && sign_text_store_select(NULL) == payload(1).signs);
	sign_text_store_select(payload(1).signs); /* undo the probe */
	/* Nested scope: the guest's view. */
	assert(clin_world_view_enter(1, &v1) && v1.slot == 0);
	assert(v1.terrain == &gstate.world && v1.dimension == WORLD_DIM_OVERWORLD
		   && v1.clock.world_time == 6000);
	assert(world_view_current() == &gstate.world
		   && particle_system_selected() == particle_system_primary()
		   && client_piston_moving_store_select(NULL)
			  == client_piston_moving_store_primary());
	client_piston_moving_store_select(payload(0).pistons);
	clin_world_view_leave(&v1);
	assert(world_view_current() == payload(1).terrain
		   && particle_system_selected() == payload(1).particles
		   && "leaving the inner scope restores the outer one");
	clin_world_view_leave(&v0);
	assert(defaults_selected()
		   && sign_text_store_select(NULL) == sign_text_store_primary()
		   && client_piston_moving_store_select(NULL)
			  == client_piston_moving_store_primary()
		   && "leaving the outer scope restores the defaults");
	/* A view no context shows falls back to player 0's slot. */
	assert(client_ctx_unbind_view(client_ctx_registry_current(reg, 0), 1));
	assert(clin_world_view_enter(1, &v1) && v1.slot == 1);
	clin_world_view_leave(&v1);
	puts("PASS T0 view scope: OFF = gstate objects, nothing selected; ON = "
		 "viewed payload's stores + accessor + clock, nested, restored");
}

static void sound(uint8_t index, uint32_t gen, float x, float y, float z) {
	struct client_rpc m = {.type = CRPC_SOUND, .world_index = index,
		.world_generation = gen,
		.payload.sound = {.sound_id = 1, .pos = {x, y, z}, .vol = 1.0F,
			.pitch = 1.0F}};
	clin_process(&m);
}

static bool last_is(float x, float y, float z) {
	return fabsf(s_last[0] - x) < 1e-4F && fabsf(s_last[1] - y) < 1e-4F
		&& fabsf(s_last[2] - z) < 1e-4F;
}

static void test_guest_sound(void) {
	flag(false);
	world_reset(0, 0);
	gstate.players[0].camera.x = 100.0F;
	gstate.players[0].camera.y = 64.0F;
	gstate.players[0].camera.z = 100.0F;
	s_plays = 0;
	sound(0, 0, 2.0F, 64.0F, 0.0F);
	assert(s_plays == 1 && last_is(2.0F, 64.0F, 0.0F) && "OFF: played as is");

	two_worlds();
	gstate.players[0].camera.x = 100.0F;
	gstate.players[0].camera.y = 64.0F;
	gstate.players[0].camera.z = 100.0F;
	gstate.players[1].camera.x = 0.0F;
	gstate.players[1].camera.y = 64.0F;
	gstate.players[1].camera.z = 0.0F;
	s_plays = 0;
	sound(1, 200, 100.0F, 64.0F, 103.0F);
	assert(s_plays == 1 && last_is(100.0F, 64.0F, 103.0F)
		   && "view 0's own world: played as is");
	sound(0, 100, 2.0F, 64.0F, 0.0F);
	assert(s_plays == 2 && last_is(102.0F, 64.0F, 100.0F)
		   && "guest-only world: heard at the guest camera's offset");
	assert(client_ctx_unbind_view(client_ctx_registry_current(reg, 0), 1));
	sound(0, 100, 2.0F, 64.0F, 0.0F);
	assert(s_plays == 2 && "a world no view shows is not heard");
	puts("PASS T5 guest-world sound: OFF unchanged; ON view-0 world as is, "
		 "guest world relative to the guest camera, unviewed world dropped");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	assert(svin_init());
	assert(clin_init());
	memset(&gstate, 0, sizeof gstate);
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	reg = clin_world_contexts();
	test_view_scope();
	test_guest_sound();
	flag(false);
	puts("PASS test_b2_view_scope");
	return 0;
}
