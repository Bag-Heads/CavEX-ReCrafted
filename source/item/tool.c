/*
	Copyright (c) 2023 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>

#include "../block/blocks.h"

int tool_tier_divider(enum tool_tier tier) {
	switch(tier) {
		case TOOL_TIER_WOOD: return 2;
		case TOOL_TIER_STONE: return 4;
		case TOOL_TIER_IRON: return 6;
		case TOOL_TIER_DIAMOND: return 8;
		case TOOL_TIER_GOLD: return 12;
		default: return 1;
	}
}

int tool_dig_delay_env_ms(struct block* type, struct item* it, bool in_water,
						  bool on_ground) {
	assert(type);

	if(type->digging.hardness <= 0)
		return -1;

	enum tool_type item_type = it ? it->tool.type : TOOL_TYPE_ANY;
	enum tool_tier item_tier
		= (it && item_type != TOOL_TYPE_ANY) ? it->tool.tier : TOOL_TIER_ANY;

	// [decompile 740c583] EntityPlayer.getCurrentPlayerStrVsBlock
	// (EntityPlayer.java:291-302):
	//     strength = the inventory's getStrVsBlock for the block;
	//     inside water: strength / 5.0F;
	//     not on the ground: strength / 5.0F again.
	// Dividing the STRENGTH by 5 multiplies the DURATION by 5, so this is a
	// numerator here: 1 dry-and-grounded, 5 for either penalty, 25 for both.
	// Beta 1.7.3 has no efficiency enchantment and no haste, so these two are
	// the only environmental terms that exist.
	int env = 1;
	if(in_water)
		env *= 5;
	if(!on_ground)
		env *= 5;

	int delay = type->digging.hardness;

	// `env` multiplies the NUMERATOR before the tier division so the single
	// truncation still lands on floor(hardness*env/divider) -- dividing first
	// would throw away up to (divider-1) ms and then scale the loss by 25,
	// making us finish EARLY, which is precisely what a Beta server refuses.
	if(type->digging.tool == item_type && item_tier >= type->digging.min) {
		delay *= env;
		if(item_type == TOOL_TYPE_SWORD) {
			// [decompile 740c583] ItemSword.getStrVsBlock: `return
			// var2.blockID == Block.web.blockID ? 15.0F : 1.5F;` -- a FLAT
			// 15.0F vs cobweb, independent of the sword's material tier
			// (unlike ItemPickaxe/ItemSpade/ItemAxe, which scale with
			// EnumToolMaterial via tool_tier_divider). Cobweb
			// (block_cobweb.c) is the only block whose digging.tool is
			// SWORD, and ItemSword.canHarvestBlock is true ONLY for
			// Block.web, so this is the only place this branch can fire.
			delay /= 15;
		} else {
			delay /= tool_tier_divider(item_tier > type->digging.best
											? type->digging.best
											: item_tier);
		}
	} else if(item_type == TOOL_TYPE_SWORD && type->digging.min == TOOL_TIER_ANY) {
		// [decompile 740c583] Material.getIsHarvestable() is true for every
		// hand-harvestable block (digging.min==ANY means the block's
		// Material was never .setNoHarvest()'d), so
		// InventoryPlayer.canHarvestBlock short-circuits true for ANY held
		// item. Block.blockStrength then uses
		// getCurrentPlayerStrVsBlock/hardness/30.0F, and a sword's flat
		// 1.5F (the non-web branch above) is a real, if small, speed bonus
		// over the implicit 1.0F hand baseline: delay/1.5 == delay*2/3.
		delay = delay * env * 2 / 3;
	} else if(type->digging.min != TOOL_TIER_ANY) {
		// [decompile 740c583] ItemSword.canHarvestBlock: `return
		// var1.blockID == Block.web.blockID;` -- false for every other
		// tool-gated (digging.min != ANY) block (rock, iron, snow, ore,
		// ...), so a sword here shares this branch with a bare hand and
		// any other mismatched-tool item: Block.blockStrength takes the
		// "cannot harvest" 1.0F/hardness/100.0F fallback either way, with
		// NO strVsBlock bonus for the sword.
		//
		// NOTE the missing `env`, and it is not an oversight: Block.blockStrength
		// reaches getCurrentPlayerStrVsBlock ONLY through the canHarvestBlock
		// arm of its ternary. This arm is a bare 1.0F/hardness/100.0F, so
		// neither /5.0F ever applies -- Beta digs stone bare-handed in the same
		// 150 ticks whether the player is standing, swimming or falling. The
		// mc173 oracle agrees (break.rs:38-53 keeps both `env_modifier /= 5.0`
		// inside `if self.can_break(...)`).
		delay = delay * 10 / 3;
	} else {
		// digging.min == TOOL_TIER_ANY and no tool bonus: Beta's canHarvest arm
		// with strVsBlock == 1.0F (a bare hand, or any item with no bonus vs
		// this block). The environment still divides that 1.0F.
		delay *= env;
	}

	return (delay + 49) / 50 * 50; // round up to next tick
}

int tool_dig_delay_ms(struct block* type, struct item* it) {
	/* Beta's dry, standing-on-ground case. Kept as its own entry point so the
	   per-block speed goldens (test_dig_speed_batch_h, test_ice_dig,
	   test_redstone_ore_dig, test_dig_tier_gates, ...) stay a pure function of
	   (block, held item), which is what they were written to pin. */
	return tool_dig_delay_env_ms(type, it, false, true);
}

int tool_dig_crack_stage(struct block* type, struct item* it, bool in_water,
						 bool on_ground, int elapsed_ms) {
	/* THE SAME CALL THE DIG MAKES (push-0921/B18, review B17 H1). The overlay
	   used to recompute its bucket width from the two-argument
	   tool_dig_delay_ms() -- Beta's dry, on-ground case -- while the dig moved
	   to the environment-aware form at screen_ingame.c:566, so a submerged
	   bare-handed dirt dig showed stage 9 after 675 ms of its real 3750 and
	   then sat frozen for the remaining 3075 (82% of the dig). Beta has ONE
	   accumulator for both (PlayerControllerSP.java:72/78 vs :102 ->
	   RenderGlobal.java:1099 -- see tool.h), so there is one delay here too.
	   The caller reads `in_water` / `on_ground` off the same session the dig
	   does; this function is the only copy of the rule. */
	int delay = tool_dig_delay_env_ms(type, it, in_water, on_ground);
	/* CLAMP: delay/10 is the crack-stage bucket width; any delay < 10ms
	   (fast tools/instant-break tuning) truncated this to 0 -- a divide by
	   zero. Floor the divisor at 1 so a sub-10ms delay just shows every
	   crack stage in the first tick instead of crashing. A never-digs block
	   (hardness <= 0) returns -1 and lands here too.

	   delay is always a whole 50 ms tick, so `delay / 10` is exact and
	   `elapsed / (delay/10)` IS floor(elapsed * 10 / delay) -- Beta's
	   `(int)(damagePartialTime * 10.0F)`, without the intermediate that a
	   literal elapsed*10 would have to overflow-check. */
	int divisor = delay / 10;
	if(divisor < 1)
		divisor = 1;
	int dt = elapsed_ms / divisor;

	if(dt >= 9)
		dt = 9;

	return dt;
}
