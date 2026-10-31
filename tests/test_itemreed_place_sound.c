/* Beta server ItemReed.onItemUse:58-63: placed block STEP after setBlock
 * and placement callbacks, (v+1)/2 @ p*0.8. ItemDoor/ItemSign are silent. */
#include "support/sound_action_env.h"

static int placed_x, placed_z, placed_type;
static void placed_capture(const struct client_rpc* call, void* user) {
	sound_capture(call, user);
	if(call->type == CRPC_SOUND)
		assert(sound_block(placed_x, gy + 1, placed_z).type == placed_type);
}

static void check_place(uint16_t item, int type, uint16_t step, int x, int z) {
	placed_x = x;
	placed_z = z;
	placed_type = type;
	sound_hold(item);
	sound_count = 0;
	struct random_gen before = s.rand_src;
	sound_place(x, gy, z, SIDE_TOP);
	assert(sound_block(x, gy + 1, z).type == type);
	// Every Beta step sound here is StepSound(name, 1.0f, 1.0f), so
	// ItemReed's (v+1)/2 and p*0.8 are exactly 1.0 and 0.8 [ItemReed.java:62].
	sound_assert(step, (1.0F + 1.0F) / 2.0F, 1.0F * 0.8F, x, gy + 1, z);
	assert(memcmp(&before, &s.rand_src, sizeof before) == 0);
	assert(s.player.inventory.items[INVENTORY_SLOT_HOTBAR].count == 0);
}

int main(void) {
	sound_env_init("test_itemreed_place_sound");
	sound_set(5, gy, 7, BLOCK_WATER_STILL, 0);
	clin_test_set_rpc_observer(placed_capture, NULL);
	// Hard-coded Beta step sounds, not the production table [Block.java:300
	// reed=soundGrassFootstep, :309 cake=soundClothFootstep, :310 repeater=
	// soundWoodFootstep; :202/204/208 all StepSound(name, 1.0f, 1.0f)].
	check_place(ITEM_REED, BLOCK_REED, SOUND_STEP_GRASS, 5, 8);
	check_place(ITEM_CAKE, BLOCK_CAKE, SOUND_STEP_CLOTH, 6, 10);
	check_place(ITEM_REDSTONE_REPEATER, BLOCK_REDSTONE_REPEATER, SOUND_STEP_WOOD, 10, 10);
	// onBlockPlacedBy has run (yaw 0 -> facing 2).
	assert(sound_block(10, gy + 1, 10).metadata == 2);
	clin_test_set_rpc_observer(sound_capture, NULL);
	// Refused dry reed and covered cake/repeater placements emit nothing.
	const uint16_t refused[] = {ITEM_REED, ITEM_CAKE, ITEM_REDSTONE_REPEATER};
	for(size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
		sound_hold(refused[i]);
		sound_count = 0;
		if(i == 1)
			sound_set(12, gy + 1, 10, BLOCK_STONE, 0);
		sound_place(12, gy, 10, SIDE_TOP);
		assert(sound_count == 0);
		assert(s.player.inventory.items[INVENTORY_SLOT_HOTBAR].count == 1);
	}
	sound_hold(ITEM_DOOR_WOOD);
	sound_count = 0;
	sound_place(11, gy, 6, SIDE_TOP);
	assert(sound_block(11, gy + 1, 6).type == BLOCK_DOOR_WOOD);
	assert(sound_count == 0);
	sound_hold(ITEM_SIGN);
	sound_count = 0;
	sound_place(12, gy, 6, SIDE_TOP);
	assert(sound_block(12, gy + 1, 6).type == BLOCK_SIGN_POST);
	assert(sound_count == 0);
	sound_env_finish();
	puts("test_itemreed_place_sound: PASS (reed/cake/repeater exact STEP after placement; failures/door/sign silent; RNG unchanged)");
	return 0;
}
