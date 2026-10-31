/* Card m2 (docs/intent/m2-mp-remote-bobber.md): another angler's fishing hook
 * is visible on a remote server.
 *
 * Beta 1.7.3 [740c583 minecraft/src_original]:
 *   NetClientHandler.handleVehicleSpawn:80-82 -- Packet23 type 90 builds
 *     new EntityFish(worldClient, x/32, y/32, z/32) (the position-only
 *     constructor, EntityFish.java:42-46: no angler).
 *   NetClientHandler.java:125-133 -- a nonzero thrower id only arms
 *     setVelocity(speed / 8000); the owner lookup is type 60 only.
 *   RenderFish.java:34 -- the line is drawn only `if(angler != null)`: a
 *     remote hook shows the float, never a line.
 *   NetClientHandler.handleEntity / handleDestroyEntity:231 -- moves and
 *     Packet29 apply to it like any tracked entity.
 * Every expected value is computed here from the wire bytes by those rules,
 * never recorded from CaveWii output. The packets go through the REAL decoder
 * (mp_decoder_feed) and the REAL protocol dispatch (process_event) into the
 * REAL client inbox consumer (clin_process_direct -> gstate.entities).
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
#include "item/item_types.h"
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

static void test_hook_with_velocity(void) {
	/* 0x17: eid 700, type 90, x 320 y 2080 z -160, thrower 42,
	 * speed 800 / -1600 / 400. */
	static const uint8_t spawn[] = {0x17, 0, 0, 0x02, 0xBC, 90,
		0, 0, 0x01, 0x40, 0, 0, 0x08, 0x20, 0xFF, 0xFF, 0xFF, 0x60,
		0, 0, 0, 42, 0x03, 0x20, 0xF9, 0xC0, 0x01, 0x90};
	feed(spawn, sizeof spawn);
	struct entity* e = client_entity(700);
	printf("  type-90 spawn -> client entity %s\n", e ? "present" : "ABSENT");
	assert(e && "Packet23 type 90 must create a visible client hook");
	/* The engine's bobber: the ITEM_FISHING_ROD billboard server_local uses
	 * for its own hook (no line). */
	assert(e->type == ENTITY_ARROW && e->data.arrow.item_id == ITEM_FISHING_ROD);
	assert(near(e->pos[0], 320 / 32.0F) && near(e->pos[1], 2080 / 32.0F)
		   && near(e->pos[2], -160 / 32.0F));
	assert(near(e->vel[0], 800 / 8000.0F) && near(e->vel[1], -1600 / 8000.0F)
		   && near(e->vel[2], 400 / 8000.0F));
	struct remote_entity* entry = remote_entity_find(remote.impl, 700);
	assert(entry && entry->kind == REMOTE_ENTITY_PROJECTILE);

	/* Packet31 relative move +1 block x: the tracked hook follows it. */
	static const uint8_t rel[] = {0x1F, 0, 0, 0x02, 0xBC, 32, 0, 0};
	feed(rel, sizeof rel);
	assert(near(e->network_pos[0], (320 + 32) / 32.0F)
		   && near(e->network_pos[1], 2080 / 32.0F));
	/* Packet28 velocity reaches the hook like any projectile. */
	static const uint8_t velo[] = {0x1C, 0, 0, 0x02, 0xBC, 0, 80, 0, 0, 0, 0};
	feed(velo, sizeof velo);
	assert(near(e->vel[0], 80 / 8000.0F));

	/* Packet29 removes it. */
	static const uint8_t destroy[] = {0x1D, 0, 0, 0x02, 0xBC};
	feed(destroy, sizeof destroy);
	assert(!client_entity(700));
	assert(!remote_entity_find(remote.impl, 700));
	puts("PASS type 90 spawns the hook at x/32, speed/8000, moves and is "
		 "removed by Packet29");
}

static void test_hook_without_thrower(void) {
	/* thrower 0: no setVelocity -- the hook starts at rest. */
	static const uint8_t spawn[] = {0x17, 0, 0, 0x02, 0xBD, 90,
		0, 0, 0, 0x10, 0, 0, 0x08, 0x00, 0, 0, 0, 0x30, 0, 0, 0, 0};
	feed(spawn, sizeof spawn);
	struct entity* e = client_entity(701);
	assert(e && e->type == ENTITY_ARROW
		   && e->data.arrow.item_id == ITEM_FISHING_ROD);
	assert(near(e->pos[0], 0.5F) && near(e->pos[1], 64.0F)
		   && near(e->pos[2], 1.5F));
	assert(e->vel[0] == 0.0F && e->vel[1] == 0.0F && e->vel[2] == 0.0F);
	static const uint8_t destroy[] = {0x1D, 0, 0, 0x02, 0xBD};
	feed(destroy, sizeof destroy);
	assert(!client_entity(701));
	puts("PASS type 90 with no thrower: at rest (setVelocity is gated on it)");
}

/* NetClientHandler.java:118-120 seeds serverPosX/Y/Z for every Packet23
 * kind, so a boat's first Packet31 lands at spawn + delta (not at the world
 * origin + delta). */
static void test_vehicle_rel_move_from_spawn(void) {
	/* 0x17: eid 702, type 1 (boat), x 640 y 2048 z 96, thrower 0. */
	static const uint8_t spawn[] = {0x17, 0, 0, 0x02, 0xBE, 1,
		0, 0, 0x02, 0x80, 0, 0, 0x08, 0x00, 0, 0, 0, 0x60, 0, 0, 0, 0};
	feed(spawn, sizeof spawn);
	struct entity* e = client_entity(702);
	assert(e && e->type == ENTITY_BOAT);
	static const uint8_t rel[] = {0x1F, 0, 0, 0x02, 0xBE, 0xF0, 4, 16};
	feed(rel, sizeof rel);
	printf("  boat after Packet31(-16,4,16): network_pos %.4f %.4f %.4f\n",
		   e->network_pos[0], e->network_pos[1], e->network_pos[2]);
	assert(near(e->network_pos[0], (640 - 16) / 32.0F)
		   && near(e->network_pos[1], (2048 + 4) / 32.0F)
		   && near(e->network_pos[2], (96 + 16) / 32.0F));
	static const uint8_t destroy[] = {0x1D, 0, 0, 0x02, 0xBE};
	feed(destroy, sizeof destroy);
	assert(!client_entity(702));
	puts("PASS Packet23 seeds serverPos: a vehicle's relative move starts "
		 "from its spawn");
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
	test_hook_with_velocity();
	test_hook_without_thrower();
	test_vehicle_rel_move_from_spawn();
	puts("PASS test_mp_remote_bobber");
	return 0;
}
