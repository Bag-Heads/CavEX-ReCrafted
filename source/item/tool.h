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

#ifndef TOOL_H
#define TOOL_H

#include <stdbool.h>

enum tool_type {
	TOOL_TYPE_ANY,
	TOOL_TYPE_PICKAXE,
	TOOL_TYPE_SHOVEL,
	TOOL_TYPE_AXE,
	TOOL_TYPE_SWORD,
	TOOL_TYPE_HOE,
};

enum tool_tier {
	TOOL_TIER_ANY,
	TOOL_TIER_WOOD,
	TOOL_TIER_GOLD,
	TOOL_TIER_STONE,
	TOOL_TIER_IRON,
	TOOL_TIER_DIAMOND,
	TOOL_TIER_MAX,
};

struct block;
struct item;

int tool_tier_divider(enum tool_tier tier);

/* Beta's dig time for the OBSERVED player state. `in_water` is
   EntityPlayer.isInsideOfMaterial(Material.water) (the EYE cell), `on_ground`
   is Entity.onGround; each false-to-penalty step divides Beta's strength by
   5.0F, i.e. multiplies this delay by 5. Returns -1 for a never-digs block
   (hardness <= 0), like the two-argument form. */
int tool_dig_delay_env_ms(struct block* type, struct item* it, bool in_water,
						  bool on_ground);

/* Beta's dig time for a player standing on the ground, out of water. */
int tool_dig_delay_ms(struct block* type, struct item* it);

/* THE CRACK OVERLAY'S STAGE, and the ONLY place that rule lives.
   Returns Beta's ten-stage break-tile index (0..9) for a dig that has been held
   `elapsed_ms` against `type` with `it` in the OBSERVED environment.

   [decompile 740c583] PlayerControllerSP.java:72 accumulates
   `this.curBlockDamage += var6.blockStrength(this.mc.thePlayer)` every tick and
   :78 breaks the block the moment that sum reaches `1.0F`; setPartialTime
   (:97-106) publishes the SAME accumulator as `renderGlobal.damagePartialTime`,
   and RenderGlobal.java:1099 indexes the break tile with
   `240 + (int)(this.damagePartialTime * 10.0F)`. So the overlay is
   floor(progress * 10) of the very clock the break runs on -- Beta has one
   strength, never two -- which is why this function takes the identical
   (in_water, on_ground) pair tool_dig_delay_env_ms does and derives the delay
   from it, instead of trusting a caller to pass a number of its own. */
int tool_dig_crack_stage(struct block* type, struct item* it, bool in_water,
						 bool on_ground, int elapsed_ms);

#endif
