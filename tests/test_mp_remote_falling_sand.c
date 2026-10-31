/* Card m3 (docs/intent/m3-mp-remote-falling-sand.md): falling sand and gravel
 * are visible on a remote server during their entity phase.
 *
 * Beta 1.7.3 [740c583 minecraft/src_original]:
 *   NetClientHandler.handleVehicleSpawn:109-116 -- Packet23 type 70 builds
 *     EntityFallingSand(world, x/32, y/32, z/32, Block.sand.blockID), type 71
 *     the same with Block.gravel.blockID.
 *   NetClientHandler.java:118-120 -- serverPos seeded from the spawn, so
 *     Packet31/33 deltas add to it; handleEntityTeleport adds 1/64 to y.
 *   NetClientHandler.java:125-133 -- velocity only with a thrower id (the
 *     server sends none for falling sand); Packet28 sets it later
 *     (handleEntityVelocity:168); handleDestroyEntity:231 removes it.
 * Expected values are computed from the wire bytes by those rules. Real
 * decoder -> process_event -> clin_process_direct -> gstate.entities.
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "entity/entity.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/mp_codec.h"
#include "network/server_interface.h"
#include "network/server_remote.h"
#include "network/server_remote_internal.h"
#include "world.h"

#define LOCAL_EID 1234

static struct server_remote remote;

static void feed(const uint8_t* bytes, size_t n) {
	struct server_remote_impl* impl = remote.impl;
	struct mp_decoder decoder;
	mp_decoder_init(&decoder);
	struct remote_event event = {.kind = REMOTE_EVENT_PACKET,
		.generation = impl->generation};
	for(size_t i = 0; i < n; i++)
		assert(mp_decoder_feed(&decoder, bytes[i], &event.packet)
			   == (i + 1 == n ? MP_DECODE_EVENT : MP_DECODE_MORE));
	assert(process_event(&remote, &event));
	mp_decoder_destroy(&decoder);
}

static struct entity* client_entity(uint32_t id) {
	struct entity** pp = dict_entity_get(gstate.entities, id);
	return pp ? *pp : NULL;
}

static bool near(float a, float b) {
	return fabsf(a - b) < 1e-6F;
}

static void test_sand(void) {
	/* 0x17: eid 800, type 70, x 528 (16.5) y 2256 (70.5) z -240 (-7.5),
	 * thrower 0 (EntityTrackerEntry sends none for falling sand). */
	static const uint8_t spawn[] = {0x17, 0, 0, 0x03, 0x20, 70,
		0, 0, 0x02, 0x10, 0, 0, 0x08, 0xD0, 0xFF, 0xFF, 0xFF, 0x10,
		0, 0, 0, 0};
	feed(spawn, sizeof spawn);
	struct entity* e = client_entity(800);
	printf("  type-70 spawn -> client entity %s\n", e ? "present" : "ABSENT");
	assert(e && "Packet23 type 70 must create a visible falling block");
	assert(e->type == ENTITY_FALLING_SAND
		   && e->data.falling_sand.block_id == BLOCK_SAND);
	assert(near(e->pos[0], 528 / 32.0F) && near(e->pos[1], 2256 / 32.0F)
		   && near(e->pos[2], -240 / 32.0F));
	assert(e->vel[0] == 0.0F && e->vel[1] == 0.0F && e->vel[2] == 0.0F);
#ifndef M3_BASE_RED /* RED build: the kind is added by the fix */
	struct remote_entity* entry = remote_entity_find(remote.impl, 800);
	assert(entry && entry->kind == REMOTE_ENTITY_FALLING_SAND);
#endif

	/* Packet31 dy = -8: 1/4 block down FROM THE SPAWN (serverPos seed); a
	 * base (non-living) entity snaps to it (Entity.setPositionAndRotation2). */
	static const uint8_t rel[] = {0x1F, 0, 0, 0x03, 0x20, 0, 0xF8, 0};
	feed(rel, sizeof rel);
	assert(near(e->network_pos[1], (2256 - 8) / 32.0F));
	assert(near(e->pos[0], 528 / 32.0F) && near(e->pos[1], (2256 - 8) / 32.0F));
	/* Packet28: motion / 8000. */
	static const uint8_t velo[] = {0x1C, 0, 0, 0x03, 0x20, 0, 0, 0xFE, 0x70,
		0, 0};
	feed(velo, sizeof velo);
	assert(near(e->vel[1], -400 / 8000.0F) && e->vel[0] == 0.0F);
	/* Packet34 teleport: absolute, +1/64 on y. */
	static const uint8_t tp[] = {0x22, 0, 0, 0x03, 0x20, 0, 0, 0x02, 0x10,
		0, 0, 0x08, 0x00, 0xFF, 0xFF, 0xFF, 0x10, 0, 0};
	feed(tp, sizeof tp);
	assert(near(e->network_pos[1], 2048 / 32.0F + 0.015625F));
	/* Packet29 removes it (the landed block comes as its own Packet53). */
	static const uint8_t destroy[] = {0x1D, 0, 0, 0x03, 0x20};
	feed(destroy, sizeof destroy);
	assert(!client_entity(800) && !remote_entity_find(remote.impl, 800));
	puts("PASS type 70: sand at x/32, moves from its spawn, Packet28, "
		 "Packet34 (+1/64), removed by Packet29");
}

static void test_gravel(void) {
	static const uint8_t spawn[] = {0x17, 0, 0, 0x03, 0x21, 71,
		0, 0, 0, 0x30, 0, 0, 0x09, 0x10, 0, 0, 0, 0x50, 0, 0, 0, 0};
	feed(spawn, sizeof spawn);
	struct entity* e = client_entity(801);
	assert(e && e->type == ENTITY_FALLING_SAND
		   && e->data.falling_sand.block_id == BLOCK_GRAVEL);
	assert(near(e->pos[0], 1.5F) && near(e->pos[1], 72.5F)
		   && near(e->pos[2], 2.5F));
	static const uint8_t destroy[] = {0x1D, 0, 0, 0x03, 0x21};
	feed(destroy, sizeof destroy);
	assert(!client_entity(801));
	puts("PASS type 71: gravel");
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
	gstate.remote_session = true;
	clin_process(&(struct client_rpc) {.type = CRPC_WORLD_RESET,
		.payload.world_reset = {.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = LOCAL_EID}});
	assert(server_remote_init(&remote));
	struct server_remote_impl* impl = remote.impl;
	impl->state = REMOTE_PLAY;
	impl->entity_id = LOCAL_EID;
	test_sand();
	test_gravel();
	puts("PASS test_mp_remote_falling_sand");
	return 0;
}
