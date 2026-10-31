/*
	test_mp_animation_parity_wire -- golden tests for the animation-parity
	pass (2026-09-03)'s shared-formula plumbing, mirroring
	test_mp_visual_parity_pose.c's "bind the shared production composition
	directly, header-only" rig.

	HURT-FLASH TIMING (item 1) and DEATH-ROLL ANGLE (item 2) -- these do NOT
	introduce new formulas: entity_remote_player_render (item 1) and
	entity_remote_player.c's death-roll (item 2) reuse combat.h's EXISTING
	hurt_flash_active/mob_death_roll_deg verbatim, the same functions
	test_mob_polish.c and test_emissive_tint.c already golden-test for mobs.
	Re-asserted here, at the SAME sample points, to document (and pin) that
	the remote-player path gets byte-identical values from the identical
	shared function -- one formula, two callers, one proof.

	FIRE/SNEAK/RIDING BIT DECODE (items 3+4) -- mp_entity_flags.h's
	mp_decode_entity_metadata_flags, the generic Entity DataWatcher flag
	byte0 [740c583 Entity.setEntityFlag/getEntityFlag,
	Entity.isBurning/isSneaking/isRiding]: bit 0 (0x01) onFire, bit 1 (0x02)
	sneaking, bit 2 (0x04) isRiding. This is the SAME decode
	server_remote_protocol.c's MP_EVENT_ENTITY_METADATA player branch calls --
	factored out here so the bit math is golden-tested directly rather than
	only indirectly through a live two-client MP session.

	Packet39AttachEntity has NO decode test here on purpose: per entity.h's
	entity_remote_player.is_riding field comment, the one real-Beta server
	call site for that packet (EntityPlayerMP.onUpdate:
	`this.playerNetServerHandler.sendPacket(new Packet39AttachEntity(this,
	this.ridingEntity))`) sends it ONLY down the mounting player's own
	connection, never to observers -- so a remote OTHER player's client
	genuinely never receives an Attach packet naming that player in faithful
	Beta multiplayer. A decode test for that path would be testing code that
	Beta itself never exercises for an observer; MP_EVENT_ATTACH_ENTITY's
	existing "no client-side consumer" comment (server_remote_protocol.c) is
	the correct, faithful behaviour, not a gap.

	(Item 5's own wire-decode golden -- the Packet5 armor slot mapping --
	extends this file in a later commit on this lane, once that item's
	armor_id plumbing exists.)

	Links only header-only pure-math/decode headers -- no GX/game_state/
	client_interface.h/entity.h dependency, so this target has zero coupling
	to the render/network layers it feeds (test_mp_visual_parity_pose.c's own
	same claim, extended here).
*/

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "network/mp_entity_flags.h"
#include "entity/combat.h"

#define EPS 1e-4f

static void assert_close(float actual, float expect, const char* what) {
	if(fabsf(actual - expect) >= EPS) {
		fprintf(stderr, "%s: expected %.6f, got %.6f\n", what, expect, actual);
		assert(0);
	}
}

static void test_hurt_flash_matches_mob_hurt_time_window(void) {
	// entity_remote_player_render (item 1) guards its gfx_entity_tint call
	// on the SAME e->hurt boolean entity_monster.c already networks from
	// hurt_flash_active(hurt_time) -- reasserted here at
	// MOB_HURT_ANIMATION_TICKS' own boundary so a future change to that
	// shared constant is caught from both call sites' test files.
	assert(hurt_flash_active(MOB_HURT_ANIMATION_TICKS));
	assert(hurt_flash_active(1));
	assert(!hurt_flash_active(0));

	// The tint this pass applies to a hurt remote player is the EXACT SAME
	// combat.h hurt_wash_for_brightness entity_monster.c uses for a hurt mob
	// [740c583 RenderLiving.doRenderLiving: `GL11.glColor4f(var25, 0.0F, 0.0F,
	// 0.4F)`; next-gaps-0924/R1b replaced the old (1.0,0.4,0.4)@0.3 tone].
	struct hurt_wash w = hurt_wash_for_brightness(0.5f);
	assert_close(w.r, 0.5f, "hurt wash R = brightness");
	assert_close(HURT_FLASH_TINT_G, 0.0f, "HURT_FLASH_TINT_G");
	assert_close(HURT_FLASH_TINT_B, 0.0f, "HURT_FLASH_TINT_B");
	assert_close(HURT_FLASH_TINT_AMOUNT, 0.4f, "HURT_FLASH_TINT_AMOUNT");
}

static void test_death_roll_angle_per_tick_matches_mob_formula(void) {
	// entity_remote_player.c's death-roll (item 2) calls mob_death_roll_deg
	// verbatim -- same sample points test_mob_polish.c/test_emissive_tint.c
	// already pin for a dying MOB, reasserted here for the player path.
	// combat.h's formula is Beta RenderLiving.rotateCorpse's own curve since
	// next-gaps-0924/G8 (the earlier linear stand-in pinned 45/2.25 here):
	// min(1, sqrt((deathTime + partialTick - 1)/20 * 1.6)) * 90.
	assert_close(mob_death_roll_deg(1, 0.0f), 0.0f, "death_time=1,td=0 -> 0deg");
	// death_time=11, td=0 -> sqrt(10/20*1.6) = sqrt(0.8) -> 80.498deg.
	assert_close(mob_death_roll_deg(11, 0.0f), 80.49845f, "death_time=11,td=0 -> 80.498deg");
	// death_time=21, td=0 -> sqrt(1.6) clamped to 1 -> 90.0deg.
	assert_close(mob_death_roll_deg(21, 0.0f), 90.0f, "death_time=21,td=0 -> 90deg");
	// Partial-tick interpolation: death_time=1, td=0.5 -> sqrt(0.5/20*1.6) =
	// sqrt(0.04) = 0.2 -> 18deg -- td blends mid-tick for a remote player
	// exactly like it does for a mob.
	assert_close(mob_death_roll_deg(1, 0.5f), 18.0f, "death_time=1,td=0.5 -> 18deg");
}

// --- fire/sneak/riding bit decode (items 3+4) ---------------------------

static void test_metadata_flags_all_clear(void) {
	struct mp_entity_metadata_flags f = mp_decode_entity_metadata_flags(0x00);
	assert(!f.on_fire);
	assert(!f.sneaking);
	assert(!f.is_riding);
}

static void test_metadata_flags_each_bit_independently(void) {
	// [740c583 Entity.setEntityFlag(0, fire>0)]: bit 0x01 alone.
	struct mp_entity_metadata_flags fire = mp_decode_entity_metadata_flags(0x01);
	assert(fire.on_fire && !fire.sneaking && !fire.is_riding);

	// [740c583 Entity.isSneaking -> getEntityFlag(1)]: bit 0x02 alone.
	struct mp_entity_metadata_flags sneak = mp_decode_entity_metadata_flags(0x02);
	assert(!sneak.on_fire && sneak.sneaking && !sneak.is_riding);

	// [740c583 Entity.setEntityFlag(2, ridingEntity != null)]: bit 0x04 alone.
	struct mp_entity_metadata_flags ride = mp_decode_entity_metadata_flags(0x04);
	assert(!ride.on_fire && !ride.sneaking && ride.is_riding);
}

static void test_metadata_flags_all_set_and_ignores_other_bits(void) {
	// All three set (0x07), PLUS every unrelated bit Beta's DataWatcher byte
	// never assigns for a player (0x08, 0x10, ..., 0x80) -- must not leak
	// into any of the three decoded flags.
	struct mp_entity_metadata_flags all
		= mp_decode_entity_metadata_flags(0x07);
	assert(all.on_fire && all.sneaking && all.is_riding);

	struct mp_entity_metadata_flags noisy
		= mp_decode_entity_metadata_flags((uint8_t) 0xF8); // 0x07's complement
	assert(!noisy.on_fire && !noisy.sneaking && !noisy.is_riding);

	struct mp_entity_metadata_flags mixed
		= mp_decode_entity_metadata_flags((uint8_t) 0xFD); // fire+riding, sneak clear, noise set
	assert(mixed.on_fire && !mixed.sneaking && mixed.is_riding);
}

// --- armor wire-slot mapping (item 5) -------------------------------------

static void test_armor_slot_mapping(void) {
	// [740c583 EntityPlayerMP.getEquipmentInSlot / RenderPlayer.
	// setArmorModel's armorItemInSlot(3-var2) cross-check]: wire slot 1..4
	// -> armor_type ordinal 3,2,1,0 (boots, leggings, chestplate, helmet).
	assert(mp_wire_armor_slot_to_type(1) == 3); // boots
	assert(mp_wire_armor_slot_to_type(2) == 2); // leggings
	assert(mp_wire_armor_slot_to_type(3) == 1); // chestplate
	assert(mp_wire_armor_slot_to_type(4) == 0); // helmet
}

static void test_armor_slot_zero_and_out_of_range_are_not_armor(void) {
	// Slot 0 is the held item (Packet5's OWN slot 0), not armor -- the
	// caller handles it separately before ever calling this function; must
	// return the "not armor" sentinel if it ever is.
	assert(mp_wire_armor_slot_to_type(0) == -1);
	// Any wire value Beta's own protocol never sends for a player
	// (getEquipmentInSlot's range is 0..4) must not alias onto a real slot.
	assert(mp_wire_armor_slot_to_type(5) == -1);
	assert(mp_wire_armor_slot_to_type(255) == -1);
}

int main(void) {
	test_hurt_flash_matches_mob_hurt_time_window();
	test_death_roll_angle_per_tick_matches_mob_formula();
	test_metadata_flags_all_clear();
	test_metadata_flags_each_bit_independently();
	test_metadata_flags_all_set_and_ignores_other_bits();
	test_armor_slot_mapping();
	test_armor_slot_zero_and_out_of_range_are_not_armor();

	printf("test_mp_animation_parity_wire: all assertions passed\n");
	return 0;
}
