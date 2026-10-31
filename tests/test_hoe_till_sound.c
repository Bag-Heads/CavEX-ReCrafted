/* Beta server ItemHoe.onItemUse:38-44: farmland STEP, (v+1)/2,
 * p*0.8 at the clicked center BEFORE setBlockWithNotify and damageItem. */
#include "support/sound_action_env.h"

#define BETA_STEP_VOL ((1.0F + 1.0F) / 2.0F)
#define BETA_STEP_PITCH (1.0F * 0.8F)

static void hoe_capture(const struct client_rpc* call, void* user) {
	sound_capture(call, user);
	if(call->type == CRPC_SOUND) {
		assert(sound_block(8, gy, 8).type == BLOCK_GRASS);
		assert(s.player.inventory.items[INVENTORY_SLOT_HOTBAR].durability == 0);
	}
}

static void hoe_dirt_capture(const struct client_rpc* call, void* user) {
	sound_capture(call, user);
	if(call->type == CRPC_SOUND) { // sound precedes the till and the wear
		assert(sound_block(11, gy, 8).type == BLOCK_DIRT);
		assert(s.player.inventory.items[INVENTORY_SLOT_HOTBAR].durability == 1);
	}
}

int main(void) {
	sound_env_init("test_hoe_till_sound");
	sound_hold(ITEM_IRON_HOE);
	struct random_gen before = s.rand_src;
	clin_test_set_rpc_observer(hoe_capture, NULL);
	sound_place(8, gy, 8, SIDE_TOP);
	// Hard-coded Beta facts, not the production table: tilledField uses
	// soundGravelFootstep = StepSound("gravel", 1.0f, 1.0f) [Block.java:203,
	// 277], so (1+1)/2 = 1.0 volume and 1.0*0.8 = 0.8 pitch [ItemHoe.java:38].
	sound_assert(SOUND_STEP_GRAVEL, BETA_STEP_VOL, BETA_STEP_PITCH, 8, gy, 8);
	assert(sound_block(8, gy, 8).type == BLOCK_FARMLAND);
	assert(s.player.inventory.items[INVENTORY_SLOT_HOTBAR].durability == 1);
	assert(memcmp(&before, &s.rand_src, sizeof before) == 0);
	clin_test_set_rpc_observer(sound_capture, NULL);
	sound_count = 0;
	sound_place(9, gy, 8, SIDE_BOTTOM);
	assert(sound_count == 0 && sound_block(9, gy, 8).type == BLOCK_GRASS);
	sound_set(10, gy + 1, 8, BLOCK_STONE, 0);
	sound_count = 0;
	sound_place(10, gy, 8, SIDE_TOP);
	assert(sound_count == 0 && sound_block(10, gy, 8).type == BLOCK_GRASS);
	assert(s.player.inventory.items[INVENTORY_SLOT_HOTBAR].durability == 1);
	// DIRT tills unconditionally: the refusal test is `(side==0 || above!=0
	// || id!=grass) && id!=dirt` [ItemHoe.java:36], so covered dirt clicked
	// from the BOTTOM face still plays the gravel step and tills.
	sound_set(11, gy, 8, BLOCK_DIRT, 0);
	sound_set(11, gy + 1, 8, BLOCK_STONE, 0);
	clin_test_set_rpc_observer(hoe_dirt_capture, NULL);
	before = s.rand_src;
	sound_count = 0;
	sound_place(11, gy, 8, SIDE_BOTTOM);
	sound_assert(SOUND_STEP_GRAVEL, BETA_STEP_VOL, BETA_STEP_PITCH, 11, gy, 8);
	assert(sound_block(11, gy, 8).type == BLOCK_FARMLAND);
	assert(s.player.inventory.items[INVENTORY_SLOT_HOTBAR].durability == 2);
	assert(memcmp(&before, &s.rand_src, sizeof before) == 0);
	sound_env_finish();
	puts("test_hoe_till_sound: PASS (Beta step.gravel 1.0/0.8 before till/wear; covered dirt tills from bottom face; failed grass till silent; RNG unchanged)");
	return 0;
}
