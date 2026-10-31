/*
	Pins the Beta 1.7.3 PLAYER->mob melee damage table (player_melee_damage) on the
	mob's raw 20-pt health scale, plus the mob i-frame / knockback constants that
	gate a hit. Binds the SHARED source/entity/combat.h so the values the
	SRPC_ENTITY_ATTACK handler (server_local.c) uses cannot drift from this test.

	Beta source: EntityPlayer.attackTargetEntityWithCurrentItem -> InventoryPlayer.
	getDamageVsEntity = the held item's weaponDamage, or 1 for an empty hand / a
	non-weapon (Item.getDamageVsEntity's base default). ItemSword.weaponDamage =
	4 + EnumToolMaterial.getDamageVsEntity()*2, with damageVsEntity WOOD=0,
	STONE=1, IRON=2, EMERALD(diamond)=3, GOLD=0 -> sword 4/4/6/8/10 (wood/gold/
	stone/iron/diamond). ItemTool.damageVsEntity = perSubclassBase + material's
	damageVsEntity, where the base is baked into each subclass constructor's
	`super(id, N, ...)` call: ItemSpade(shovel) N=1 -> 1/1/2/3/4, ItemPickaxe N=2
	-> 2/2/3/4/5, ItemAxe N=3 -> 3/3/4/5/6 -- THREE DIFFERENT tables, axe hits
	hardest and shovel softest, never a single shared "tool" value. Hoes + blocks
	+ everything else = 1. The result is mob-scale (NOT *8), because it damages a
	20-pt mob.

	PARITY FIX (audit register 2026-08-25, items-recipes #1): this golden
	previously pinned two independently-wrong tables -- sword `4 + material`
	(dropping ItemSword's `*2`) giving 5/5/6/7/8, and ONE shared pickaxe/axe/
	shovel table `1 + material` giving 2/2/3/4/5 for all three. Both are
	corrected here from the decompile re-derivation above, not by relaxing any
	assertion.
*/

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "entity/mob_motion.h"
#include "entity/entity.h"
#include <string.h>
#include "entity/combat.h"
#include "item/tool.h"

static void test_fist_and_nonweapons(void) {
	// Empty hand / fist: the server passes TOOL_TYPE_ANY + TOOL_TIER_ANY.
	assert(player_melee_damage(TOOL_TYPE_ANY, TOOL_TIER_ANY) == 1);
	// A held block or a generic item also resolves to TOOL_TYPE_ANY -> fist.
	assert(player_melee_damage(TOOL_TYPE_ANY, TOOL_TIER_WOOD) == 1);
	// Hoes are NOT weapons in b1.7.3 -> base 1 at every tier.
	assert(player_melee_damage(TOOL_TYPE_HOE, TOOL_TIER_WOOD) == 1);
	assert(player_melee_damage(TOOL_TYPE_HOE, TOOL_TIER_DIAMOND) == 1);
}

static void test_swords(void) {
	// 4 + damageVsEntity*2: wood/gold 4, stone 6, iron 8, diamond 10.
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_WOOD) == 4);
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_GOLD) == 4);
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_STONE) == 6);
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_IRON) == 8);
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_DIAMOND) == 10);
	// Gold == wood is a real Beta quirk (both raw damageVsEntity 0), pinned
	// explicitly.
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_GOLD)
		   == player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_WOOD));
}

static void test_tools(void) {
	// ItemTool.damageVsEntity = perSubclassBase + material's damageVsEntity --
	// THREE DIFFERENT tables (shovel base 1, pickaxe base 2, axe base 3), never
	// one shared value. Ordering is strict: shovel < pickaxe < axe at every tier.
	assert(player_melee_damage(TOOL_TYPE_SHOVEL, TOOL_TIER_WOOD) == 1);
	assert(player_melee_damage(TOOL_TYPE_SHOVEL, TOOL_TIER_GOLD) == 1);
	assert(player_melee_damage(TOOL_TYPE_SHOVEL, TOOL_TIER_STONE) == 2);
	assert(player_melee_damage(TOOL_TYPE_SHOVEL, TOOL_TIER_IRON) == 3);
	assert(player_melee_damage(TOOL_TYPE_SHOVEL, TOOL_TIER_DIAMOND) == 4);

	assert(player_melee_damage(TOOL_TYPE_PICKAXE, TOOL_TIER_WOOD) == 2);
	assert(player_melee_damage(TOOL_TYPE_PICKAXE, TOOL_TIER_GOLD) == 2);
	assert(player_melee_damage(TOOL_TYPE_PICKAXE, TOOL_TIER_STONE) == 3);
	assert(player_melee_damage(TOOL_TYPE_PICKAXE, TOOL_TIER_IRON) == 4);
	assert(player_melee_damage(TOOL_TYPE_PICKAXE, TOOL_TIER_DIAMOND) == 5);

	assert(player_melee_damage(TOOL_TYPE_AXE, TOOL_TIER_WOOD) == 3);
	assert(player_melee_damage(TOOL_TYPE_AXE, TOOL_TIER_GOLD) == 3);
	assert(player_melee_damage(TOOL_TYPE_AXE, TOOL_TIER_STONE) == 4);
	assert(player_melee_damage(TOOL_TYPE_AXE, TOOL_TIER_IRON) == 5);
	assert(player_melee_damage(TOOL_TYPE_AXE, TOOL_TIER_DIAMOND) == 6);

	enum tool_tier tiers[5] = {TOOL_TIER_WOOD, TOOL_TIER_GOLD, TOOL_TIER_STONE,
							  TOOL_TIER_IRON, TOOL_TIER_DIAMOND};
	for(int i = 0; i < 5; i++) {
		assert(player_melee_damage(TOOL_TYPE_SHOVEL, tiers[i])
			   < player_melee_damage(TOOL_TYPE_PICKAXE, tiers[i]));
		assert(player_melee_damage(TOOL_TYPE_PICKAXE, tiers[i])
			   < player_melee_damage(TOOL_TYPE_AXE, tiers[i]));
	}

	// A tool is always strictly better than a fist.
	assert(player_melee_damage(TOOL_TYPE_PICKAXE, TOOL_TIER_IRON)
		   > player_melee_damage(TOOL_TYPE_ANY, TOOL_TIER_ANY));
	// Sword scales *2 per damageVsEntity step while every ItemTool scales *1, so
	// the sword-vs-tool GAP WIDENS with tier -- only exactly base_gap (axe 1,
	// pickaxe 2, shovel 3) at WOOD/GOLD (damageVsEntity 0); it is NOT a flat
	// "sword = tool + 3" at every tier (the old, wrong assumption). Confirmed at
	// WOOD (the fixed-gap tier) and separately at IRON (the actual value each
	// widened gap must equal): sword 8, axe 5 (gap 3), pickaxe 4 (gap 4),
	// shovel 3 (gap 5).
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_WOOD)
		   == player_melee_damage(TOOL_TYPE_AXE, TOOL_TIER_WOOD) + 1);
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_WOOD)
		   == player_melee_damage(TOOL_TYPE_PICKAXE, TOOL_TIER_WOOD) + 2);
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_WOOD)
		   == player_melee_damage(TOOL_TYPE_SHOVEL, TOOL_TIER_WOOD) + 3);
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_IRON) == 8);
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_IRON)
		   == player_melee_damage(TOOL_TYPE_AXE, TOOL_TIER_IRON) + 3);
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_IRON)
		   == player_melee_damage(TOOL_TYPE_PICKAXE, TOOL_TIER_IRON) + 4);
	assert(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_IRON)
		   == player_melee_damage(TOOL_TYPE_SHOVEL, TOOL_TIER_IRON) + 5);
}

// Hits to drop a 20-pt mob given per-hit damage (the mob health scale).
static int hits_for(int dmg) {
	int hp = 20, n = 0;
	while(hp > 0) { hp -= dmg; n++; }
	return n;
}

static void test_hits_to_kill(void) {
	// On the mob's 20-pt health, a diamond sword (10) kills in 2 hits, a wood
	// sword (4) in 5, and a fist (1) in 20 -- the whole point of per-tool damage
	// (the old flat -=5 killed EVERY mob in 4 regardless of weapon).
	assert(hits_for(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_DIAMOND)) == 2);
	assert(hits_for(player_melee_damage(TOOL_TYPE_SWORD, TOOL_TIER_WOOD)) == 5);
	assert(hits_for(player_melee_damage(TOOL_TYPE_ANY, TOOL_TIER_ANY)) == 20);
}

static void test_hit_gates(void) {
	// The i-frame window must be positive (so a click-mash can't multi-hit in one
	// burst) and the knockback-slide window positive but shorter (a brief shove).
	assert(MOB_HURT_RESIST_TICKS > 0);
	assert(MOB_KNOCKBACK_TICKS > 0);
	assert(MOB_KNOCKBACK_TICKS < MOB_HURT_RESIST_TICKS);
	// Knockback reuses the Beta melee horizontal magnitude.
	assert(KNOCKBACK_H > 0.0f);
}

// Beta EntityLiving.knockBack [EntityLiving.java:392-405]: `motion /= 2.0D;
// motionX -= var3/(double)f * (double)0.4F; motionY += (double)0.4F (capped at
// (double)0.4F); motionZ -= var5/(double)f * (double)0.4F` -- HALVES the mob's
// existing motion, THEN adds the impulse. Mob motion units S2: composed AT THE
// HIT on the authoritative doubles by mob_motion_knockback (the float
// halve-then-add helpers and the x25 fall bridge are retired).
static void test_knockback_composition(void) {
	struct entity e;
	memset(&e, 0, sizeof e);
	struct mob_motion* m = &e.data.monster.motion_d;
	/* attacker 1 block -X of the mob: var3 = -1, var5 = 0 -> +X push */
	mob_motion_knockback(&e, m, -1.0, 0.0, NULL);
	assert(m->motion[0] == 0.0 / 2.0 - (-1.0) / (double)1.0F * (double)0.4F);
	assert(m->motion[1] == (double)0.4F);
	assert(m->motion[2] == 0.0);

	/* MULTI-HIT, same direction: the first hit's 0.4 is halved, not dropped */
	double first = m->motion[0];
	mob_motion_knockback(&e, m, -1.0, 0.0, NULL);
	assert(m->motion[0] == first / 2.0 + (double)0.4F);
	assert(m->motion[0] > (double)0.4F);

	/* opposing re-hit: halved and partially cancelled, not replaced */
	double before = m->motion[0];
	mob_motion_knockback(&e, m, 1.0, 0.0, NULL);
	assert(m->motion[0] == before / 2.0 - (double)0.4F);

	/* vertical cap: an already-rising mob is not launched past 0.4F */
	m->motion[1] = 0.9;
	mob_motion_knockback(&e, m, -1.0, 0.0, NULL);
	assert(m->motion[1] == (double)0.4F);
	/* a falling mob keeps half its Beta fall: -1.0/2 + 0.4F */
	m->motion[1] = -1.0;
	mob_motion_knockback(&e, m, -1.0, 0.0, NULL);
	assert(m->motion[1] == -1.0 / 2.0 + (double)0.4F);
	/* a resting Beta mob (motionY -0.0784 after gravity) still pops */
	m->motion[1] = (0.0 - 0.08) * (double)0.98F;
	mob_motion_knockback(&e, m, -1.0, 0.0, NULL);
	assert(m->motion[1] > 0.35);
	/* the float mirror is the single rounding of the double */
	assert(e.vel[1] == (float)m->motion[1]);
}

int main(void) {
	test_fist_and_nonweapons();
	test_swords();
	test_tools();
	test_hits_to_kill();
	test_hit_gates();
	test_knockback_composition();
	printf("test_player_melee: per-tool damage pinned (fist 1, sword 4-10, "
		   "shovel 1-4, pickaxe 2-5, axe 3-6, hoe 1) + %d-tick mob i-frames / "
		   "%d-tick knockback (halve-then-add composition, multi-hit "
		   "verified)\n",
		   MOB_HURT_RESIST_TICKS, MOB_KNOCKBACK_TICKS);
	return 0;
}
